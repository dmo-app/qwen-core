// Command layer of the C++ engine; see docs/contracts/engine-protocol-v1.md.
// Undo/redo and the complete document belong to the application domain.
// Pure: no stdio, no audio device (those are injected), so tests drive it directly.
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "engine.hpp"
#include "project.hpp"
#include "render_job.hpp"

namespace core {

class AudioOutput {
public:
  virtual ~AudioOutput() = default;
  // Starts the device and returns {sampleRate, blockSize, latencyMs, backend, device, ...}.
  virtual json start(Engine& engine, const json& args) = 0;
  virtual void stop() = 0;
  virtual json devices(const json& args) = 0;  // {backend?}
};

struct RenderResult {
  std::vector<float> L, R;
  std::vector<std::vector<float>> stemL, stemR;  // one per track when stems are asked
  double renderMs = 0;
  float peak = 0;
  uint64_t denseEventBlocks = 0;  // extra event passes (dense blocks: slower, nothing dropped)
};
// Offline render of the whole song plus a tail. Block size does not change the result.
RenderResult renderProject(const Project& project, double sampleRate, int blockSize = 512, bool stems = false,
                           double tailSeconds = 1.5, EventLog* log = nullptr, AssetStore* assets = nullptr,
                           PluginProvider* plugins = nullptr, int threads = -1,  // threads: export workers (-1 = WorkerPool::forExport(), 0 = serial)
                           bool stemsWithoutMasterGain = false);  // stems for a bounce (Graph::setSolo)

// What the server needs from a plugin host beyond loading plugins (the JUCE host gives all of it).
class PluginHost : public PluginProvider {
public:
  virtual json captureStates() = 0;                                              // {key: base64}
  virtual bool showEditor(const std::string& key, bool open, std::string* why) = 0;
  virtual bool takeChanged() = 0;                                                // a plugin's own editor changed it
  virtual void releaseAll() = 0;                                                 // another song: no instance carries over
  virtual json parameters(const std::string& key) { (void)key; return json::array(); }  // [{id, name, value, ...}]
  virtual void resetAll() {}                                                     // Stop: every plugin drops its tails
  // The plugin's sound as a .vstpreset file, in base64 (VST3 only; empty if not loaded or not VST3): Bitwig export.
  virtual std::string vstPreset(const std::string& key) { (void)key; return {}; }
};

class Server {
public:
  static constexpr int kProtocolVersion = 1;
  static constexpr size_t kMaxMessageBytes = 8u << 20;  // one JSON line, either direction
  using OutputFactory = std::function<std::unique_ptr<AudioOutput>()>;
  explicit Server(OutputFactory makeOutput, double sampleRate = 48000, PluginHost* plugins = nullptr);
  ~Server();
  // {id?, cmd, args} -> {id?, ok, result | error}
  json handle(const json& message);
  json execute(const std::string& cmd, const json& args);
  // First message of a session: protocol and format versions, session id, limits, commands.
  json hello() const;
  // {event:'state', sessionId, acceptedRevision, appliedRevision, position, playing, master, tracks}.
  // acceptedRevision: last project revision validated here; appliedRevision: the one the audio is
  // really playing (stays null until the device has taken it, and may skip revisions).
  json stateEvent();
  bool takeEnded();
  // Export jobs whose state or progress changed (sent as {event: "job", ...}).
  std::vector<json> takeJobUpdates() { return jobs_.takeUpdates(); }
  static json commandList();
  const std::string& sessionId() const { return sessionId_; }
  int64_t acceptedRevision() const { return revision_; }
  const Project& project() const { return project_; }
  Engine& engine() { return *engine_; }
  // A plugin reported another latency since the graph was built: a new graph lines the tracks up again.
  // Call it now and then (the serve loop does, with the state ticks). True if it rebuilt.
  bool checkPluginLatency();
  // A file the plugin failures are appended to (one JSON line each: time, key, plugin, vendor, version,
  // why), so a plugin that misbehaves leaves a trace across sessions. "" = none.
  void setFailureLog(const std::string& pathUtf8) { failureLog_ = pathUtf8; }

private:
  struct Source { bool v1 = false; json data; };
  void rebuild(bool keepTails = true);
  void stopTransport(bool rewind);
  int64_t requestedRevision(const json& args) const;  // validates only: a failed command changes nothing
  // Transactional on the revision: rebuild() is the only step that can throw, and it does so before publishing
  // (the engine keeps playing the old graph), so a failed edit spends no revision (engine-protocol-v1
  // §Revisões). The document is validated before it is mutated (fromJson/trackFromJson/buildTrack throw first)
  // and rebuild() does not throw for a validated project, so the mutated document is never left half-applied.
  // Cheap on purpose: no per-edit copy of the project (that would cost the fluidity of rapid edits).
  void commit(int64_t revision) {
    const int64_t prevRevision = revision_, prevLast = lastRevision_;
    revision_ = revision;  // the graph rebuilt below carries it (Graph::revision)
    try {
      rebuild();
    } catch (...) {
      revision_ = prevRevision; lastRevision_ = prevLast;  // a failed edit spends nothing
      throw;
    }
    if (revision >= 0) lastRevision_ = revision;
  }
  Track buildTrack(const Source& source);
  void updateSongLength();
  json render(const json& args);
  json missingPlugins() const;
  // The accepted project's tempo map, rebuilt only when its BPM, changes or the sample rate change (a seek
  // or loop per UI frame reuses it: audit 2026-11-04 E5).
  const TempoMap& tempo();
  json latencyJson() const;
  json failedPlugins() const;  // M3b: [{key, plugin, why}] of the plugins that failed (silent from then on)
  json pluginVersions() const;  // [{key, plugin, saved, installed}] where the installed version is another one
  void logFailures();           // appends each new failure to the failure log (once per instance)
  json cpu();                  // M0b: engine.cpu

  OutputFactory makeOutput_;
  double sampleRate_;
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<AudioOutput> output_;
  json outputInfo_;
  json outputArgs_ = json::object();  // what the running device was started with: an engine.start that fails puts it back
  Project project_;
  TempoMap tempo_;
  int trackLatency_ = 0, masterLatency_ = 0;  // of the last graph built (samples)
  double tempoBpm_ = -1, tempoRate_ = -1;
  std::vector<TempoChange> tempoChanges_;
  bool projectFromV1_ = true;
  double listenDb_ = -10;  // listen.set or engine.start {listenDb}: kept across engine.start (Diogo 2026-11-07)
  bool analysis_ = false;  // analysis.enable: the state event carries the spectrum and correlation
  std::map<std::string, Source> sources_;
  uint64_t lastRebuildNs_ = 0;
  std::string sessionId_;
  std::deque<std::pair<uint64_t, std::vector<std::string>>> published_;  // graph serial -> its track ids
  std::shared_ptr<AssetStore> assets_ = std::make_shared<AssetStore>();  // audio clip files, shared with export jobs
  std::vector<std::string> missingAssets_;                                 // of the last graph built
  PluginHost* plugins_ = nullptr;                                          // null: plugin tracks are silent
  std::vector<json> missingPlugins_;                                       // of the last graph built
  std::vector<std::pair<std::shared_ptr<PluginProcessor>, int>> latencies_;  // its plugins and the latency it used
  std::vector<Graph::LoadedPlugin> loadedPlugins_;                           // of the last graph built
  std::string failureLog_;                                                   // where failures are written ("" = nowhere)
  std::vector<const PluginProcessor*> logged_;                               // instances already written there
  std::shared_ptr<CpuMeter> cpu_;                                            // of the last graph built
  uint64_t cpuBuiltNs_ = 0;                                                  // when it was built
  // Which build engine.cpu last read: a count, not the meter's address (a new meter can land on a freed one's
  // address and the old counters were then read against it, past the end; sweep 2026-11-14 E1).
  uint64_t cpuBuilt_ = 0, cpuRead_ = 0;
  std::vector<uint64_t> cpuPrev_;                                            //   what it read
  uint64_t cpuReadNs_ = 0, cpuRenderNs_ = 0, cpuBudgetNs_ = 0;               //   and when, with the callback totals
  int64_t revision_ = -1;      // revision of the current project (-1 = edited without one)
  int64_t lastRevision_ = -1;  // highest revision accepted: revisions only move forward
  RenderJobs jobs_;            // last member: its destructor stops a running export first
};

} // namespace core
