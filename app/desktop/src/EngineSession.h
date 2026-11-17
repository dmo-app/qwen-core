// Keeps the engine in step with the document, away from the UI thread.
// The UI calls the setters (they return at once) and reads snapshot() on a timer; one worker thread
// talks to the engine: at most one document edit in flight, intermediate values coalesced, the last
// one always sent. Plan sections 3.3 (consistency) and 3.4 (IPC).
#pragma once
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "document.hpp"
#include "engine_client.hpp"
#include "engine_protocol.hpp"
#include "sync_retry.hpp"

namespace core::app {

using json = nlohmann::json;

enum class EngineStatus {
  Starting,   // launching the engine process
  Ready,      // the sound matches the document
  Syncing,    // an edit was sent and is not playing yet
  NoDevice,   // the engine runs (accepts edits, could export) but no audio device opened
  Diverged,   // the engine refused the document: edits are kept, sending stops until fixed or undone
  Stopped     // the engine process is not running
};

// The last export (job in the engine).
struct ExportJob {
  uint64_t jobId = 0;
  std::string state;        // "" (none), starting, running, cancelling, succeeded, cancelled, failed
  double progress = 0;
  std::string path, error;  // path UTF-8 (the first file)
  int64_t revision = -1;
  int files = 1, file = 0;  // how many files the job writes, and the one being written
  bool active() const { return state == "starting" || state == "running" || state == "cancelling"; }
};

struct SessionSnapshot {
  EngineStatus status = EngineStatus::Starting;
  std::string message;           // what happened, for the status line
  std::string sessionId;
  std::string engineBuild;       // the engine's version + commit (its hello); Help > About shows it
  int64_t documentRevision = 0;  // the edit the app wants to hear
  int64_t acceptedRevision = -1, appliedRevision = -1;
  bool hasAcceptedDocument = false;
  bool playing = false;
  int64_t positionSamples = 0, songSamples = 0;
  double positionTick = 0;       // in the tempo the engine is playing
  double positionAtMs = 0;       // when that position arrived (juce::Time::getMillisecondCounterHiRes)
  double outputLatencyMs = 0;    // the device's: a sample rendered now is heard this much later
  double sampleRate = 48000;
  float masterPeak[2] = {0, 0};
  // The master's loudness (BS.1770), measured since play started or the last reset; NaN: not measured
  // (silence, or the window not filled yet).
  struct Loudness {
    double momentary = NAN, shortTerm = NAN, integrated = NAN;  // LUFS
    double truePeak = NAN;                                       // dBTP
    double seconds = 0;
  } loudness;
  // The master's spectrum (1/6-octave bands from 20 Hz, dB) and stereo correlation, while enableAnalysis(true).
  struct Analysis {
    std::vector<float> spectrum;  // empty: none received
    double correlation = 0;
  } analysis;
  // The input of the effect a plugin window watches (watchEffect), in the same bands; empty: none.
  std::vector<float> effectSpectrum;
  std::map<std::string, std::pair<float, float>> trackPeaks;  // ids of the applied revision
  ExportJob exportJob;
  std::vector<std::string> devices;  // output devices the engine can open
  std::string device;                // the one in use ("" = none)
  // Sounds and effects that failed while playing and went silent until the next change: "track: unit".
  std::vector<std::string> failedUnits;
  std::map<std::string, std::string> missingPlugins;  // key -> "name: why" (not loaded: silent)
  uint64_t pluginEdits = 0;          // grows when a plugin's own editor changed it
};

class EngineSession {
public:
  struct Options {
    std::filesystem::path engineBinary;
    std::string backend;  // "" = the engine's default device; "null" = no sound (tests)
    std::string device;   // output device name ("" = the system default)
    std::filesystem::path engineLog;  // the engine's own messages ("" = memory only)
    std::filesystem::path dumpFolder;  // where a crash of the engine leaves a dump ("" = none)
  };
  EngineSession(Options options, doc::Document document);
  ~EngineSession();

  // UI thread. Never blocks on the engine.
  void setDocument(const doc::Document& document);  // a new revision only when the sound changes
  // The project folder (audio files are relative to it; empty: the song is not saved yet). Always sends
  // a new revision, so the engine reads the files again (after relocating a missing one, too).
  void setAssetRoot(const std::filesystem::path& root, bool send = true);  // send = false: with the next setDocument
  void play();
  void stop();
  void home();       // stop, clear notes and go to the start
  void seek(int64_t tick);
  void setLoop(const doc::Loop& loop);
  // Export the current document (the engine exports the accepted revision, sent first if needed).
  void exportWav(const std::string& pathUtf8, int bits);
  // Several files in one job (the mix and/or stems by track id, "" = the mix) over a region of the song
  // (endTick -1: the song's end). The files are put in place together when all are written.
  // withoutMasterGain: a track's stem for a bounce, without the master gain (it gets it again when it plays).
  struct ExportFile { std::string pathUtf8, trackId; bool withoutMasterGain = false; bool mono = false; };
  void exportFiles(const std::vector<ExportFile>& files, int64_t startTick, int64_t endTick, int bits);
  void cancelExport();
  void selectDevice(const std::string& name);  // also used by later restarts
  // Live notes (computer keyboard, MIDI): any thread. They reach the engine in order with the other
  // commands and play at the start of its next audio block (latency: one device buffer).
  void liveNote(const std::string& trackId, int pitch, double velocity, bool on);
  void panic();  // every note off now
  void resetLoudness();  // the master's loudness is measured again from now
  // The listening level (dB, -60..0; -10 by default, Diogo 2026-11-07): what the device plays, fixed; exports
  // are not touched. Kept for every later engine start.
  void setListenLevel(double db);
  void enableAnalysis(bool on);  // the state carries the master's spectrum and correlation (unused since References left Core)
  // The effect whose input spectrum the state carries (Core FX's window): owner = track id or "master"; effectId ""
  // stops. One at a time: the window in front. Kept for every later engine start.
  void watchEffect(const std::string& owner, const std::string& effectId);
  // Where the audio time went since the last call (engine.cpu: tracks [{id, cpuPct}], master {cpuPct}), without
  // waiting: `done` runs on the message thread (empty if no answer in timeoutMs). The Mixer asks once a second.
  void cpuAsync(int timeoutMs, std::function<void(json)> done);
  void newSong();  // another song (open, new): the document the engine accepted is not this song's any more
  // Plugins: the state of every loaded plugin ({key: base64}); waits at most timeoutMs (null if no answer; {} is an answer).
  json pluginStates(int timeoutMs);
  // The same without waiting: `done` runs later on the message thread with the states (null if no answer).
  void pluginStatesAsync(int timeoutMs, std::function<void(json)> done);
  // The parameters of a loaded plugin that can be automated ([{id, name, value, default, steps, text}]);
  // empty if it is not loaded or there is no answer in timeoutMs. UI thread.
  json pluginParams(const std::string& key, int timeoutMs);
  // The loaded plugin's sound as a .vstpreset, in base64 ("" if no answer, not loaded or not VST3): Bitwig export.
  std::string pluginPreset(const std::string& key, int timeoutMs);
  void openPluginEditor(const std::string& key);
  void releasePlugins();  // another song (open, new): the engine's plugins start again from its states
  // New engine session: same document, loop and position; stays stopped.
  void restart();
  SessionSnapshot snapshot() const;
  std::optional<doc::Document> acceptedDocument() const;
  void killEngineForTest();  // as if the engine process crashed
  const doc::Document& document() const { return document_; }  // UI thread only

private:
  struct Action {
    enum Kind { Play, Stop, Home, Seek, SetLoop, Export, CancelExport, SelectDevice, LiveNote, Panic, Cpu, ResetLoudness, Analysis, Listen, Watch, PluginStates, PluginParams, PluginPreset, PluginEditor, ReleasePlugins } kind;
    int64_t tick = 0;
    std::string text;
    int bits = 24;
    double velocity = 0;
    bool on = false;
    json args;  // Export: files and region (null: one file, the whole song)
    std::shared_ptr<std::promise<json>> reply;  // Cpu
  };
  void openDevice(client::EngineClient& engine);  // engine.start with the chosen device; updates the status
  void restoreTransport(client::EngineClient& engine);  // loop and last position, after the device
  void queue(Action action);
  void queueTransport(Action action, bool playing);
  void run();
  bool connect();
  void sync(std::unique_lock<std::mutex>& lock);
  void onEvent(const json& event);

  Options options_;
  doc::Document document_;  // UI thread's copy
  std::filesystem::path assetRoot_;  // UI thread

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool quit_ = false, restartRequested_ = false;
  json wantedProjection_;   // latest projection; sent when wantedRevision_ > sentRevision_
  doc::Document wantedDocument_;
  std::optional<doc::Document> acceptedDocument_;
  uint64_t documentEpoch_ = 0;
  int64_t wantedRevision_ = 1, sentRevision_ = 0;
  client::SyncRetry syncRetry_;  // audit 2, bug 2: a project.load with no answer is sent again by itself
  bool retryPending_ = false;
  std::chrono::steady_clock::time_point retryAt_{};
  std::deque<Action> actions_;
  doc::Loop loop_;          // latest loop asked for
  double listenDb_ = -10;   // setListenLevel; sent with every engine.start
  std::string watchOwner_, watchEffect_;  // watchEffect; sent again after a restart
  double lastTick_ = 0;     // last position seen, kept across engine restarts
  uint64_t pluginEditsTotal_ = 0;  // pluginsChanged, kept across restarts too: the count the UI compares with
  SessionSnapshot snap_;
  int transportPending_ = 0;     // Play/Stop/Home queued and not yet taken by the engine
  bool transportIntent_ = false;  // playing, as the last of them asked
  std::unique_ptr<client::EngineClient> client_;
  std::thread worker_;
};

const char* statusText(EngineStatus status);  // Portuguese label for the UI

} // namespace core::app
