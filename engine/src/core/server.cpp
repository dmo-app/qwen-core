#include "server.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <random>
#include <set>
#include <stdexcept>
#include <chrono>
#include <thread>
#include "../io/wav.hpp"
#include "tempo_map.hpp"

namespace core {

namespace {
const char* kCommands[][2] = {
  {"engine.start", "Start the audio device {sampleRate?, blockSize?, backend?: 'wasapi'|'null'|..., exclusive?, device?, liveThreads?: workers that play tracks with the audio thread (default 4, 0 = one core)} -> {sampleRate, blockSize, latencyMs, latency: {tracksSamples, masterSamples, graphMs, deviceMs, totalMs}}"},
  {"engine.stop", "Stop the audio device"},
  {"engine.stats", "Timing stats: callbacks, overruns, late callbacks, render time, edit->audio delay"},
  {"engine.cpu", "Where the audio time went since the last call (M0b), in % of one core = % of real time -> {intervalMs, load: {avgPct, maxPct, marginPct}, totalPct, tracks: [{id, cpuPct, instrument: {name, cpuPct}, effects: [{id, name, cpuPct}]}], master: {cpuPct, effects}}"},
  {"engine.resetStats", "Zero the timing stats (e.g. after the device has warmed up)"},
  {"devices.list", "{backend?} -> {backend, devices: [{name, default}]}"},
  {"project.load", "Replace the whole project {project} (core-project v1 from the JS editor, or v2)"},
  {"project.get", "The project as core-project v2 (ticks, PPQ 960)"},
  {"tempo.set", "{bpm} 40-600"},
  {"track.add", "{track} in the v1 shape (kind synth|drums, pattern in steps) or v2 (instrument, notes in ticks, or clips: [{startTick, lengthTicks, patternTicks, notes}])"},
  {"track.remove", "{trackId}"},
  {"track.set", "{trackId, patch}: volumeDb, pan, mute, solo, name, params, pattern, bars (v1) or notes, envelopes, patternTicks (v2)"},
  {"master.set", "{volumeDb, chain?}"},
  {"transport.play", "Start playback"},
  {"transport.stop", "{rewind?}"},
  {"transport.seek", "{tick} or {position} in samples"},
  {"listen.set", "{db}: the listening level, -60..0 dB (default -10): what the device plays is that much quieter, fixed; exports are not touched. engine.start takes {listenDb} too"},
  {"earGuard.set", "{on: true}: the ear guard on the output, always on (off is refused): no peak over -1 dBFS, NaN is silence; the state event's \"earGuard\" says how far it turned the output down (dB)"},
  {"loudness.reset", "starts a new loudness measurement of the master (also done by transport.play from stopped)"},
  {"analysis.enable", "{on}: the state event carries \"analysis\" (the master's spectrum in 1/6-octave bands from 20 Hz, dB; stereo correlation)"},
  {"analysis.watch", "{trackId, effectId}: the state event carries \"effectSpectrum\", that effect's input in the same bands (trackId \"master\" for the master chain; no effectId stops)"},
  {"transport.loop", "{enabled, startTick, endTick} or {enabled, start, end} in samples"},
  {"transport.panic", "Release every note (sequenced, live, auditions) and drop queued live notes"},
  {"note.on", "{trackId, pitch, velocity} live note"},
  {"note.off", "{trackId, pitch}"},
  {"note.audition", "{trackId, pitch, velocity, durationMs?}"},
  {"render", "{path, bits?: 24|32, stems?, tailMs?, startTick?, endTick?} -> {path, stems, seconds, peak, renderMs, realtimeFactor, denseEventBlocks} (blocks until done)"},
  {"render.start", "{path | files: [{path, track?: id (none = the mix), withoutMasterGain?: bool (a stem for a bounce)}], startTick?, endTick?, bits?: 24|32, tailMs?, sampleRate?, revision?} -> job status; runs in the background, sends job events. Stems are taken before the master chain; a region plays the song before it (not written) and cuts it at endTick, tails ring out"},
  {"render.status", "{jobId} -> {jobId, state: queued|running|cancelling|succeeded|cancelled|failed, progress, ...}"},
  {"render.cancel", "{jobId}: stops the job and leaves the target file as it was (cancelRefused if the file is already being put in place)"},
  {"plugins.missing", "Plugin tracks/effects that are not loaded -> [{key, plugin, why}] (UI: 'Missing instrument')"},
  {"plugins.failed", "Plugins that failed while playing (M3b: exception, crash, NaN/Inf or runaway level) -> [{key, plugin, why}]: each is silent (its track mutes; on the master the mix goes through) until plugin.retry, the song is loaded again or the plugin replaced"},
  {"plugin.retry", "{key} a failed plugin gets a new instance from the song's state -> {retried, loaded}"},
  {"plugins.versions", "Plugins whose installed version is not the one the song was saved with -> [{key, plugin, saved, installed}]"},
    {"plugins.states", "The state of every loaded plugin -> {key: base64} (saved in the document)"},
    {"plugin.editor", "{key, open} opens or closes a plugin's own editor window"},
    {"plugins.release", "Another song: every loaded plugin is let go (the next load starts from that song's states)"},
    {"plugin.preset", "{key} the loaded plugin's sound as a .vstpreset file -> {preset: base64} (VST3 only)"},
    {"plugin.params", "{key} the parameters of a loaded plugin that can be automated -> [{id, name, value, default, steps, text}]"},
  {"commands.list", "This list"},
};

bool isEdit(const std::string& cmd) {
  static const std::set<std::string> edits = {"project.load", "tempo.set", "track.add", "track.remove", "track.set", "master.set"};
  return edits.count(cmd) > 0;
}

double numArg(const json& args, const char* key, double fallback) {
  if (!args.contains(key) || args[key].is_null()) return fallback;
  if (!args[key].is_number()) throw std::runtime_error(std::string("'") + key + "' must be a number.");
  const double value = args[key].get<double>();
  // Check the bits: /fp:fast may fold std::isfinite to true on the Windows build.
  if ((std::bit_cast<uint64_t>(value) & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000))
    throw std::runtime_error(std::string("'") + key + "' must be finite.");
  return value;
}
std::string strArg(const json& args, const char* key) {
  if (!args.contains(key) || !args[key].is_string()) throw std::runtime_error(std::string("'") + key + "' is required.");
  return args[key].get<std::string>();
}
int64_t integerArg(const json& args, const char* key, int64_t fallback, int64_t lo, int64_t hi) {
  const double value = numArg(args, key, (double)fallback);
  if (value < (double)lo || value > (double)hi)
    throw std::runtime_error(std::string("'") + key + "' is outside the allowed range.");
  return (int64_t)value;
}
// M5: CORE_LIVE_THREADS=<n> workers for real time when engine.start does not say (0 = serial); otherwise
// 4, or one less than the cores (PC 2026-11-05, 22 Surge XT tracks: p99 72 % -> 23 %, 6 threads barely better).
int liveThreadsFromEnv() {
  std::string v;
#ifdef _MSC_VER
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, "CORE_LIVE_THREADS") == 0 && buf) { v = buf; free(buf); }
#else
  if (const char* e = std::getenv("CORE_LIVE_THREADS")) v = e;
#endif
  if (v.empty()) return WorkerPool::forThisMachine(4);
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  return end != v.c_str() && n > 0 ? (int)std::min<long>(n, 32) : 0;
}
} // namespace

RenderResult renderProject(const Project& project, double sampleRate, int blockSize, bool stems, double tailSeconds, EventLog* log, AssetStore* assets,
                           PluginProvider* plugins, int threads, bool stemsWithoutMasterGain) {
  RenderResult out;
  const uint64_t t0 = nowNs();
  WorkerPool workers(threads < 0 ? WorkerPool::forExport() : threads);
  auto renderOnce = [&](int solo, std::vector<float>& L, std::vector<float>& R, EventLog* lg) {
    Graph g(project, sampleRate, assets, plugins, true);  // an export: its own plugin instances
    g.setSolo(solo, stemsWithoutMasterGain);
    g.useWorkers(&workers);
    g.preroll();  // plugin latency: the song starts on sample 0 of the file
    const int64_t total = g.songSamples() + (int64_t)(tailSeconds * sampleRate);
    L.assign((size_t)total, 0.0f); R.assign((size_t)total, 0.0f);
    for (int64_t pos = 0; pos < total; pos += blockSize) {
      int n = (int)std::min<int64_t>(blockSize, total - pos);
      g.process(L.data() + pos, R.data() + pos, n, pos, false, lg);
    }
    out.denseEventBlocks += g.takeExtraEventPasses();
  };
  renderOnce(-1, out.L, out.R, log);
  if (stems) {
    out.stemL.resize(project.tracks.size()); out.stemR.resize(project.tracks.size());
    for (size_t i = 0; i < project.tracks.size(); ++i) renderOnce((int)i, out.stemL[i], out.stemR[i], nullptr);
  }
  out.renderMs = (nowNs() - t0) / 1e6;
  for (size_t i = 0; i < out.L.size(); ++i) out.peak = std::max({out.peak, std::fabs(out.L[i]), std::fabs(out.R[i])});
  return out;
}

Server::Server(OutputFactory makeOutput, double sampleRate, PluginHost* plugins)
    : makeOutput_(std::move(makeOutput)), sampleRate_(sampleRate), engine_(std::make_unique<Engine>(sampleRate)), plugins_(plugins) {
  engine_->setListenDb(listenDb_);
  // A new id for every engine process: replies and state from an old session can never be mistaken
  // for this one after a restart.
  std::random_device random;
  char id[17];
  std::snprintf(id, sizeof id, "%08x%08x", random(), (unsigned)(nowNs() & 0xffffffffu));
  sessionId_ = id;
  project_.lengthTicks = 16 * 4 * kPpq;
  rebuild();
}

// What a note played now waits before it is heard (audit 48c9c38 I2): the tracks lined up with the
// slowest one (plugin blocks of 128 + what plugins declare, limiter 2: 39, saturation 2: 16), the master
// chain, and the device's buffers.
json Server::latencyJson() const {
  const double deviceMs = outputInfo_.is_object() ? outputInfo_.value("latencyMs", 0.0) : 0.0;
  const double graphMs = (trackLatency_ + masterLatency_) * 1000.0 / sampleRate_;
  return {{"tracksSamples", trackLatency_}, {"masterSamples", masterLatency_}, {"graphMs", graphMs}, {"deviceMs", deviceMs}, {"totalMs", graphMs + deviceMs}};
}

const TempoMap& Server::tempo() {
  const auto same = [](const std::vector<TempoChange>& a, const std::vector<TempoChange>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].tick != b[i].tick || a[i].bpm != b[i].bpm || a[i].ramp != b[i].ramp) return false;  // ramp too (audit 2026-11-07)
    return true;
  };
  if (project_.bpm != tempoBpm_ || sampleRate_ != tempoRate_ || !same(project_.tempoMap, tempoChanges_)) {
    tempo_ = TempoMap(project_.bpm, project_.tempoMap, sampleRate_);
    tempoBpm_ = project_.bpm;
    tempoRate_ = sampleRate_;
    tempoChanges_ = project_.tempoMap;
  }
  return tempo_;
}

json Server::hello() const {
  return {{"event", "ready"}, {"protocol", kProtocolVersion}, {"sessionId", sessionId_}, {"version", "0.2.0"},
          {"format", kProjectFormat}, {"projectVersion", kProjectVersion},
          {"limits", {{"maxMessageBytes", kMaxMessageBytes}, {"maxTracks", Engine::kMaxTracks}, {"maxNotesPerTrack", 4096},
                      {"liveNoteQueue", 255}, {"timedNoteOffs", 32}}},
          {"capabilities", {"revisions", "renderJobs", "clips", "tempoMap"}},  // what this engine understands
          {"dsp", "plugins"},  // Core's own DSP left (2026-11-16): the sound comes from plugins
          {"commands", commandList()}};
}

int64_t Server::requestedRevision(const json& args) const {
  if (!args.is_object() || !args.contains("revision") || args["revision"].is_null()) return -1;
  const json& r = args["revision"];
  if (!r.is_number_integer() || r.get<int64_t>() < 0) throw std::runtime_error("'revision' must be a whole number >= 0.");
  const int64_t revision = r.get<int64_t>();
  if (revision <= lastRevision_)
    throw std::runtime_error("Stale revision " + std::to_string(revision) + " (last accepted " + std::to_string(lastRevision_) + ").");
  return revision;
}

Server::~Server() { if (output_) output_->stop(); }

json Server::commandList() {
  json list = json::array();
  for (auto& c : kCommands) list.push_back({{"cmd", c[0]}, {"description", c[1]}});
  return list;
}

void Server::rebuild(bool keepTails) {
  const uint64_t t0 = nowNs();
  auto graph = std::make_unique<Graph>(project_, sampleRate_, assets_.get(), plugins_, false);
  missingAssets_ = graph->missingAssets();
  missingPlugins_ = graph->missingPlugins();
  latencies_ = graph->pluginLatencies();
  loadedPlugins_ = graph->plugins();
  cpu_ = graph->cpu();
  cpuBuiltNs_ = nowNs();
  ++cpuBuilt_;
  trackLatency_ = graph->latency();
  masterLatency_ = graph->masterLatency();
  if (plugins_) plugins_->keepOnly(graph->pluginKeys());  // plugins no longer in the song are released
  graph->revision = revision_;
  graph->keepTails = keepTails;
  std::vector<std::string> ids;
  for (int i = 0; i < graph->trackCount(); ++i) ids.push_back(graph->trackId(i));
  published_.emplace_back(engine_->publish(std::move(graph)), std::move(ids));
  while (published_.size() > 64) published_.pop_front();  // far more than can be pending at once
  engine_->collectGarbage();
  assets_->prune();  // files no graph uses any more (the one playing still holds its own)
  lastRebuildNs_ = nowNs() - t0;
}

// Stop silences everything, delay and reverb tails too: an echo ringing on after Stop sounds like a fault
// (2026-11-05). The output fades out first, so cutting the tails is never heard; then the plugins drop
// their tails, and a graph that takes none from the old one plays from silence.
void Server::stopTransport(bool rewind) {
  engine_->stop(rewind, true);
  if (output_) {
    const uint64_t until = nowNs() + 200'000'000;  // a device that stopped calling back: go on without it
    while (!engine_->stopFaded() && nowNs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  uint64_t serial = 0;  // should the rebuild fail: sound again at once
  try {
    if (plugins_) plugins_->resetAll();
    rebuild(false);
    serial = published_.back().first;
  } catch (...) {
    engine_->unmuteAt(serial);
    throw;
  }
  engine_->unmuteAt(serial);
}

bool Server::checkPluginLatency() {
  for (const auto& [processor, used] : latencies_)
    if (processor->latencySamples() != used) { rebuild(); return true; }
  return false;
}

Track Server::buildTrack(const Source& s) {
  return trackFromJson(s.v1 ? trackFromV1(s.data) : s.data);
}

void Server::updateSongLength() {
  if (!projectFromV1_) return;
  int64_t longest = 0;
  for (const auto& t : project_.tracks) longest = std::max(longest, t.lengthTicks);
  project_.lengthTicks = longest > 0 ? longest : 16 * 4 * kPpq;
}

// M7a: the routing as it would be after an edit (only ids, instruments, outputs and sends are copied).
static void checkRouting(const Project& project, const Track* changed, const std::string& removed = "") {
  Project skeleton;
  bool replaced = false;
  for (const auto& t : project.tracks) {
    if (t.id == removed) continue;
    const Track& use = changed && changed->id == t.id ? *changed : t;
    replaced = replaced || &use == changed;
    Track k; k.id = use.id; k.instrument = use.instrument; k.output = use.output; k.sends = use.sends; k.effects = use.effects;
    skeleton.tracks.push_back(std::move(k));
  }
  if (changed && !replaced) { Track k; k.id = changed->id; k.instrument = changed->instrument; k.output = changed->output; k.sends = changed->sends; k.effects = changed->effects; skeleton.tracks.push_back(std::move(k)); }
  validateRouting(skeleton);
}

json Server::handle(const json& message) {
  json response = json::object();
  if (message.contains("id")) response["id"] = message["id"];
  try {
    if (!message.is_object() || !message.contains("cmd") || !message["cmd"].is_string()) throw std::runtime_error("Missing 'cmd'.");
    const std::string cmd = message["cmd"].get<std::string>();
    json result;
    if (isEdit(cmd)) {
      // An edit that fails changes nothing (README, protocol): if building the new graph throws after the
      // document was already changed, the document, its sources and the revisions go back as they were.
      Project project = project_;
      std::map<std::string, Source> sources = sources_;
      const bool fromV1 = projectFromV1_;
      const int64_t revision = revision_, lastRevision = lastRevision_;
      try {
        result = execute(cmd, message.value("args", json::object()));
      } catch (...) {
        project_ = std::move(project);
        sources_ = std::move(sources);
        projectFromV1_ = fromV1;
        revision_ = revision;
        lastRevision_ = lastRevision;
        throw;
      }
    } else {
      result = execute(cmd, message.value("args", json::object()));
    }
    if (isEdit(cmd) && result.is_object()) result["acceptedRevision"] = revision_ >= 0 ? json(revision_) : json();
    response["result"] = std::move(result);
    response["ok"] = true;
  } catch (const std::exception& e) {
    response["ok"] = false;
    response["error"] = e.what();
  }
  return response;
}

json Server::execute(const std::string& cmd, const json& args) {
  // Project edits may carry {revision}: checked before anything changes, recorded only on success.
  const int64_t revision = isEdit(cmd) ? requestedRevision(args) : -1;
  if (cmd == "commands.list") return commandList();
  if (cmd == "devices.list") { auto out = makeOutput_(); return out ? out->devices(args) : json::array(); }
  if (cmd == "engine.start") {
    // The arguments are checked before the sound stops: a refused start changes nothing (sweep: it used to leave
    // the device stopped).
    double sr = numArg(args, "sampleRate", sampleRate_);
    if (!(sr >= 8000 && sr <= 384000)) throw std::runtime_error("sampleRate must be 8000-384000.");
    // M5: tracks on several cores while playing. {liveThreads: n} or CORE_LIVE_THREADS; default 4 (0 = serial).
    const int live = (int)std::clamp(numArg(args, "liveThreads", (double)liveThreadsFromEnv()), 0.0, 32.0);
    if (args.contains("listenDb") && !args["listenDb"].is_number()) throw std::runtime_error("listenDb must be a number (dB).");
    // The device that is playing is put aside, not thrown away: if the new one does not come up the old
    // one goes back on the engine it was playing, and the song keeps sounding (audit 2026-11-17 item 14:
    // it was stopped and destroyed before the try, so a refused start — a device busy, no device at all,
    // the second start of a rate change — left the app with no output until the user asked again).
    std::unique_ptr<AudioOutput> kept = std::move(output_);
    json keptInfo = std::move(outputInfo_);
    const json keptArgs = outputArgs_;
    const double keptRate = sampleRate_;
    const bool otherEngine = sr != sampleRate_ || !engine_ || live != engine_->liveThreads();
    std::unique_ptr<Engine> keptEngine;  // set only when this start replaces it
    if (kept) kept->stop();
    if (otherEngine) keptEngine = std::move(engine_);
    try {
      if (args.contains("listenDb"))
        listenDb_ = std::clamp(args["listenDb"].get<double>(), OutputGuard::kListenMinDb, OutputGuard::kListenMaxDb);
      if (otherEngine) {
        sampleRate_ = sr;
        engine_ = std::make_unique<Engine>(sampleRate_, live);
        engine_->setEarGuard(true);
        engine_->setListenDb(listenDb_);
        rebuild();
      }
      output_ = makeOutput_();
      if (!output_) throw std::runtime_error("No audio output available.");
      engine_->setListenDb(listenDb_);
      engine_->setEarGuard(true);  // a device: the guard is on, whatever an embedding code did to this engine
      outputArgs_ = args;
      outputInfo_ = output_->start(*engine_, args);
      const double actual = outputInfo_.value("sampleRate", sampleRate_);
      if (actual != sampleRate_) {  // the device chose another rate: rebuild for it
        output_->stop();
        sampleRate_ = actual;
        engine_ = std::make_unique<Engine>(sampleRate_, live);
        engine_->setEarGuard(true);
        engine_->setListenDb(listenDb_);
        rebuild();
        json again = args; again["sampleRate"] = actual;
        outputArgs_ = again;
        outputInfo_ = output_->start(*engine_, again);
      }
      keptEngine.reset();  // the new device answered: the engine it replaced no longer plays
      json started = outputInfo_;
      started["latency"] = latencyJson();  // the device's latencyMs, plus what the song's graph adds
      return started;
    } catch (...) {
      output_.reset();  // the one that did not come up
      sampleRate_ = keptRate;
      if (keptEngine) engine_ = std::move(keptEngine);  // its own engine, with the transport where it was
      if (kept) {
        output_ = std::move(kept);
        try {
          outputInfo_ = output_->start(*engine_, keptArgs);  // as it was started before this command
          outputArgs_ = keptArgs;                            // and the next failed try puts it back the same way
        } catch (...) {  // the device is gone as well: nothing to put back, but the command still says why
          output_->stop();
          output_.reset();
          outputInfo_ = json();
        }
      } else {
        outputInfo_ = std::move(keptInfo);
      }
      throw;
    }
  }
  if (cmd == "listen.set") {
    if (!args.contains("db") || !args["db"].is_number()) throw std::runtime_error("listen.set needs {db: -60..0}.");
    listenDb_ = std::clamp(args["db"].get<double>(), OutputGuard::kListenMinDb, OutputGuard::kListenMaxDb);
    if (engine_) engine_->setListenDb(listenDb_);
    return {{"db", listenDb_}};
  }
  if (cmd == "earGuard.set") {
    // Always on at the speakers (Codex review 2026-11-09 C1: turned off, a +6 dBFS signal and NaN reached them). The
    // song before the guard is in the capture and the offline render.
    if (!args.contains("on") || !args["on"].is_boolean()) throw std::runtime_error("earGuard.set needs {on: true}.");
    if (!args["on"].get<bool>())
      throw std::runtime_error("The ear guard cannot be turned off: it protects the speakers and your ears. Measure the song before it with the capture or an offline render.");
    engine_->setEarGuard(true);
    return {{"on", true}};
  }
  if (cmd == "engine.stop") { if (output_) { output_->stop(); output_.reset(); } return json::object(); }
  if (cmd == "engine.resetStats") { engine_->stats().reset(); return json::object(); }
  if (cmd == "engine.stats") {
    auto& s = engine_->stats();
    const double calls = (double)std::max<uint64_t>(1, s.callbacks.load());
    return {{"callbacks", s.callbacks.load()}, {"overruns", s.overruns.load()}, {"starvations", s.lateCallbacks.load()},
            {"maxRenderMs", s.maxRenderNs.load() / 1e6}, {"avgRenderMs", s.totalRenderNs.load() / calls / 1e6},
            {"graphSwaps", s.graphSwaps.load()}, {"minCallbackFrames", s.minFrames.load() == UINT64_MAX ? 0 : s.minFrames.load()}, {"maxCallbackFrames", s.maxFrames.load()}, {"maxEditToAudioMs", s.maxSwapDelayNs.load() / 1e6},
            {"lastRebuildMs", lastRebuildNs_ / 1e6}, {"leakedGraphs", s.leakedGraphs.load()}, {"pluginNotesDropped", pluginNotesDropped()},
            {"liveNotesRejected", s.liveNotesRejected.load()}, {"liveOffOverflows", s.liveOffOverflows.load()}, {"liveOffAllTracks", s.liveOffAllTracks.load()},
            {"auditionSteals", s.auditionSteals.load()}, {"denseEventBlocks", s.denseEventBlocks.load()}, {"droppedNotes", s.droppedNotes.load()}, {"callbackErrors", s.callbackErrors.load()},
            {"avgLoadPct", s.totalBudgetNs.load() ? 100.0 * s.totalRenderNs.load() / s.totalBudgetNs.load() : 0.0}, {"maxLoadPct", s.maxLoadPpm.load() / 1e4},
            {"loadP50Pct", s.loadPercentile(50)}, {"loadP95Pct", s.loadPercentile(95)}, {"loadP99Pct", s.loadPercentile(99)},
            {"nonFiniteBlocks", s.nonFiniteBlocks.load()}, {"deviceDriftPpm", s.deviceDriftPpm.load()},
            {"pluginFailures", pluginFailures()}, {"nativeFailures", nativeFailures()}, {"failedUnits", failedUnits()}, {"failedPlugins", failedPlugins()},
            {"liveThreads", engine_->liveThreads()}, {"liveFallback", engine_->liveFallback()},
            {"liveStalls", s.liveStalls.load()}, {"liveStallBlocks", s.liveStallBlocks.load()},
            {"device", outputInfo_}, {"latency", latencyJson()}};
  }
  if (cmd == "project.get") return toJson(project_);
  if (cmd == "plugins.missing") return missingPlugins();
  if (cmd == "plugins.failed") { logFailures(); return failedPlugins(); }
  if (cmd == "plugins.versions") return pluginVersions();
  if (cmd == "plugin.retry") {
    // A new instance of a failed plugin, from the song's state: the host lets the old one go first.
    const std::string key = strArg(args, "key");
    auto it = std::find_if(loadedPlugins_.begin(), loadedPlugins_.end(), [&](const Graph::LoadedPlugin& p) { return p.key == key; });
    if (it == loadedPlugins_.end()) throw std::runtime_error("No plugin '" + key + "' in this song.");
    if (!it->processor->failed()) return {{"retried", false}};
    logFailures();
    if (plugins_) {
      std::vector<std::string> keep;
      for (const auto& p : loadedPlugins_) if (p.key != key) keep.push_back(p.key);
      plugins_->keepOnly(keep);
    }
    rebuild();
    auto again = std::find_if(loadedPlugins_.begin(), loadedPlugins_.end(), [&](const Graph::LoadedPlugin& p) { return p.key == key; });
    return {{"retried", true}, {"loaded", again != loadedPlugins_.end() && !again->processor->failed()}};
  }
  if (cmd == "engine.cpu") return cpu();
  if (cmd == "project.load") {
    if (!args.contains("project")) throw std::runtime_error("'project' is required.");
    const json& p = args["project"];
    Project next = fromJson(p);  // validates (and migrates v1)
    clearFailedUnits();  // the new graph's units are fresh: they play again
    std::map<std::string, Source> sources;
    if (p.value("version", 0) == 1) {
      for (const auto& t : p["tracks"]) if (t.value("kind", "") != "audio") sources[t.value("id", "")] = {true, t};
    } else {
      const json normalized = toJson(next);
      for (const auto& t : normalized["tracks"]) sources[t["id"].get<std::string>()] = {false, t};
    }
    project_ = std::move(next);
    sources_ = std::move(sources);
    projectFromV1_ = p.value("version", 0) == 1;
    commit(revision);
    json loaded = {{"tracks", project_.tracks.size()}, {"missing", missingPlugins()}, {"missingAssets", missingAssets_}, {"lengthSamples", tempo().sampleOfTick(project_.lengthTicks)}};
    if (json versions = pluginVersions(); !versions.empty()) loaded["versionChanged"] = std::move(versions);  // never silent
    if (!project_.renamedEffects.empty()) loaded["renamedEffects"] = project_.renamedEffects;  // ids with ':' (E6)
    if (!project_.conversionNotes.empty()) loaded["converted"] = project_.conversionNotes;  // what an old version lost
    return loaded;
  }
  if (cmd == "tempo.set") {
    double bpm = numArg(args, "bpm", project_.bpm);
    if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) throw std::runtime_error("BPM must be 40-600.");
    // The audio thread keeps the musical position (same tick) and the sounding notes on the swap.
    project_.bpm = bpm;
    commit(revision);
    return {{"bpm", bpm}};
  }
  if (cmd == "track.add") {
    if (!args.contains("track") || !args["track"].is_object()) throw std::runtime_error("'track' is required.");
    const json& t = args["track"];
    Source s{t.contains("kind"), t};
    Track track = buildTrack(s);
    if (project_.findTrack(track.id)) throw std::runtime_error("A track with id '" + track.id + "' already exists.");
    if (project_.tracks.size() >= (size_t)Engine::kMaxTracks) throw std::runtime_error("Too many tracks.");
    checkRouting(project_, &track);
    sources_[track.id] = s;
    project_.tracks.push_back(std::move(track));
    updateSongLength();
    commit(revision);
    return {{"trackId", project_.tracks.back().id}};
  }
  if (cmd == "track.remove") {
    std::string id = strArg(args, "trackId");
    auto it = std::find_if(project_.tracks.begin(), project_.tracks.end(), [&](const Track& t) { return t.id == id; });
    if (it == project_.tracks.end()) throw std::runtime_error("No track '" + id + "'.");
    checkRouting(project_, nullptr, id);  // a bus still in use cannot go: route its inputs elsewhere first
    project_.tracks.erase(it);
    sources_.erase(id);
    updateSongLength();
    commit(revision);
    return json::object();
  }
  if (cmd == "track.set") {
    std::string id = strArg(args, "trackId");
    Track* track = project_.findTrack(id);
    if (!track) throw std::runtime_error("No track '" + id + "'.");
    const json patch = args.value("patch", json::object());
    if (!patch.is_object()) throw std::runtime_error("'patch' must be an object.");
    Source next = sources_[id];
    json ignored = json::array();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
      const std::string& key = it.key();
      if (key == "color" || key == "presetName" || (key == "effects" && next.v1)) { ignored.push_back(key); continue; }
      if (key == "id" || key == "kind" || key == "instrument") throw std::runtime_error("'" + key + "' cannot change; remove and add the track.");
      if (next.v1) next.data[key] = it.value();
      else next.data[key == "volumeDb" ? "gainDb" : key] = it.value();
    }
    Track updated = buildTrack(next);
    checkRouting(project_, &updated);
    *track = std::move(updated);
    sources_[id] = next;
    updateSongLength();
    commit(revision);
    return {{"ignored", ignored}};
  }
  if (cmd == "master.set") {
    project_.masterGainDb = std::clamp(numArg(args, "volumeDb", project_.masterGainDb), -60.0, 12.0);
    commit(revision);
    return {{"ignored", args.contains("chain") ? json::array({"chain"}) : json::array()}};
  }
  if (cmd == "transport.play") { engine_->play(); return json::object(); }
  if (cmd == "transport.stop") { stopTransport(args.value("rewind", false)); return json::object(); }
  // Positions in ticks are converted with the accepted project's tempo (the engine keeps the musical
  // position when that tempo reaches the audio thread); "position"/"start"/"end" in samples still work.
  if (cmd == "transport.seek") {
    const TempoMap& tempo = this->tempo();
    const int64_t end = tempo.sampleOfTick(project_.lengthTicks);
    const int64_t sample = args.contains("tick")
        ? (int64_t)std::llround(tempo.sampleAt(std::clamp(numArg(args, "tick", 0), 0.0, (double)project_.lengthTicks)))
        : (int64_t)std::clamp(numArg(args, "position", 0), 0.0, (double)end);
    engine_->seek(sample);
    return {{"position", sample}};
  }
  if (cmd == "transport.panic") { engine_->panic(); return json::object(); }
  if (cmd == "loudness.reset") { engine_->resetLoudness(); return json::object(); }
  if (cmd == "analysis.enable") { analysis_ = args.value("on", true); return {{"on", analysis_}}; }
  if (cmd == "analysis.watch") {
    const std::string track = args.value("trackId", std::string("master")), fx = args.value("effectId", std::string());
    engine_->watchEffect(track.empty() ? "master" : track, fx);
    return {{"watching", !fx.empty()}};
  }
  if (cmd == "transport.loop") {
    const TempoMap& tempo = this->tempo();
    const bool ticks = args.contains("startTick") || args.contains("endTick");
    const int64_t songEnd = tempo.sampleOfTick(project_.lengthTicks);
    const int64_t start = ticks ? (int64_t)std::llround(tempo.sampleAt(std::clamp(numArg(args, "startTick", 0), 0.0, (double)project_.lengthTicks)))
        : (int64_t)std::clamp(numArg(args, "start", 0), 0.0, (double)songEnd);
    const int64_t end = ticks ? (int64_t)std::llround(tempo.sampleAt(std::clamp(numArg(args, "endTick", 0), 0.0, (double)project_.lengthTicks)))
        : (int64_t)std::clamp(numArg(args, "end", 0), 0.0, (double)songEnd);
    if (args.value("enabled", false) && end <= start) throw std::runtime_error("The loop must end after it starts.");
    engine_->setLoop(args.value("enabled", false), start, end);
    return {{"start", start}, {"end", end}};
  }
  if (cmd == "note.on" || cmd == "note.off" || cmd == "note.audition") {
    std::string id = strArg(args, "trackId");
    if (!project_.findTrack(id)) throw std::runtime_error("No track '" + id + "'.");
    const double pitchArg = numArg(args, "pitch", 60);
    if (!(pitchArg >= 0 && pitchArg <= 127)) throw std::runtime_error("pitch must be 0-127.");
    int pitch = (int)pitchArg;
    double velocity = std::clamp(numArg(args, "velocity", 0.8), 0.0, 1.0);
    int64_t off = cmd == "note.audition"
        ? std::max<int64_t>(1, (int64_t)(std::clamp(numArg(args, "durationMs", 250), 1.0, 60000.0) * 0.001 * sampleRate_)) : 0;
    if (!engine_->liveNote(id, pitch, velocity, cmd != "note.off", off))
      throw std::runtime_error("Live note queue is full: note not played.");
    return json::object();
  }
  if (cmd == "render") return render(args);
  if (cmd == "plugins.states") {
    if (!plugins_) return json::object();
    return plugins_->captureStates();
  }
  if (cmd == "plugins.release") {
    if (plugins_) plugins_->releaseAll();
    return json::object();
  }
  if (cmd == "plugin.params") {
    if (!plugins_) return json::array();
    return plugins_->parameters(strArg(args, "key"));
  }
  if (cmd == "plugin.preset") {
    // {key} -> {preset: base64 of a .vstpreset} (the Bitwig export: the same plugin with the same sound).
    if (!plugins_) throw std::runtime_error("Plugins are not available in this engine.");
    const std::string key = strArg(args, "key");
    const std::string preset = plugins_->vstPreset(key);
    if (preset.empty()) throw std::runtime_error("No VST3 preset for " + key + " (not loaded, or not a VST3).");
    return json{{"preset", preset}};
  }
  if (cmd == "plugin.editor") {
    if (!plugins_) throw std::runtime_error("Plugins are not available in this engine.");
    std::string why;
    if (!plugins_->showEditor(strArg(args, "key"), args.value("open", true), &why)) throw std::runtime_error(why);
    return json::object();
  }
  if (cmd == "render.start") {
    // Exports a validated revision: the one asked for must be the accepted one (or none given).
    if (args.contains("revision") && !args["revision"].is_null() && args["revision"] != json(revision_))
      throw std::runtime_error("Revision " + args["revision"].dump() + " is not the accepted one (" + std::to_string(revision_) + ").");
    RenderJobRequest request;
    request.project = project_;
    request.revision = revision_;
    request.sampleRate = numArg(args, "sampleRate", sampleRate_);
    if (!(request.sampleRate >= 8000 && request.sampleRate <= 384000)) throw std::runtime_error("sampleRate must be 8000-384000.");
    request.bits = (int)integerArg(args, "bits", 24, 24, 32);
    request.tailSeconds = numArg(args, "tailMs", kDefaultTailMs) / 1000.0;
    if (args.contains("files")) {
      if (!args["files"].is_array()) throw std::runtime_error("'files' must be a list.");
      for (const auto& f : args["files"]) {
        RenderOutput out;
        out.path = f.value("path", "");
        if (f.contains("track") && !f["track"].is_null()) {
          const std::string id = f["track"].is_string() ? f["track"].get<std::string>() : "";
          for (size_t i = 0; i < project_.tracks.size(); ++i) if (project_.tracks[i].id == id) out.track = (int)i;
          if (out.track < 0) throw std::runtime_error("No track '" + id + "' to export.");
          out.withoutMasterGain = f.value("withoutMasterGain", false);
        }
        out.mono = f.value("mono", false);
        request.outputs.push_back(std::move(out));
      }
    } else {
      request.path = strArg(args, "path");
    }
    request.startTick = integerArg(args, "startTick", 0, 0, project_.lengthTicks);
    request.endTick = integerArg(args, "endTick", -1, -1, project_.lengthTicks);
    request.assets = assets_;
    request.plugins = plugins_;
    return jobs_.start(std::move(request));
  }
  if (cmd == "render.status") return jobs_.status((uint64_t)integerArg(args, "jobId", 0, 0, INT64_C(9007199254740991)));
  if (cmd == "render.cancel") return jobs_.cancel((uint64_t)integerArg(args, "jobId", 0, 0, INT64_C(9007199254740991)));
  throw std::runtime_error("Unknown command '" + cmd + "'.");
}

json Server::render(const json& args) {
  if (jobs_.busy()) throw std::runtime_error("Another export is running; wait for it before rendering synchronously.");
  std::string path = strArg(args, "path");
  const double bitsArg = numArg(args, "bits", 24);
  if (bitsArg != 24 && bitsArg != 32) throw std::runtime_error("bits must be 24 or 32.");
  int bits = (int)bitsArg;
  bool stems = args.value("stems", false);
  const int channels = args.value("mono", false) ? 1 : 2;  // mono: (L + R) / 2 in every file
  double sr = numArg(args, "sampleRate", sampleRate_);
  if (!(sr >= 8000 && sr <= 384000)) throw std::runtime_error("sampleRate must be 8000-384000.");
  const double tail = numArg(args, "tailMs", kDefaultTailMs);
  if (!(tail >= 0 && tail <= 60000)) throw std::runtime_error("tailMs must be 0-60000.");
  // A region (startTick/endTick), as render.start does it but on this thread (the CLI): the song is played from its start up to endTick (held notes, tails and LFOs are where they would
  // be), then cut there; the file starts at startTick.
  const bool region = args.contains("startTick") || (args.contains("endTick") && !args["endTick"].is_null());
  Project project = project_;
  int64_t skip = 0;
  if (region) {
    const int64_t endTick = integerArg(args, "endTick", project.lengthTicks, -1, project.lengthTicks);
    const int64_t startTick = integerArg(args, "startTick", 0, 0, project.lengthTicks);
    // As render.start: an explicit end of 0 is an empty region and is refused (audit B06: it exported the whole song).
    if (endTick >= 0) project.lengthTicks = std::min(project.lengthTicks, endTick);
    if (!(startTick >= 0 && startTick < project.lengthTicks)) throw std::runtime_error("The region must start at 0 or later and before its end.");
    skip = TempoMap(project.bpm, project.tempoMap, sr).sampleOfTick(startTick);
  }
  std::vector<RenderOutput> outputs{{path, -1}};
  json stemPaths = json::array();
  if (stems) {
    const std::string base = path.size() > 4 && path.substr(path.size() - 4) == ".wav" ? path.substr(0, path.size() - 4) : path;
    for (size_t i = 0; i < project_.tracks.size(); ++i) {
      const std::string stem = base + "-" + project_.tracks[i].id + ".wav";
      outputs.push_back({stem, (int)i});
      stemPaths.push_back(stem);
    }
  }
  validateRenderOutputs(outputs);
  RenderResult r = renderProject(project, sr, 512, stems, tail / 1000.0, nullptr, assets_.get(), plugins_);
  if (skip > 0) {
    auto cut = [skip](std::vector<float>& v) { v.erase(v.begin(), v.begin() + std::min<int64_t>(skip, (int64_t)v.size())); };
    cut(r.L);
    cut(r.R);
    for (auto& s : r.stemL) cut(s);
    for (auto& s : r.stemR) cut(s);
  }
  {  // C2: checked before anything is written
    ExportLevel mix;
    mix.addFrames(r.L.data(), r.R.data(), (int)std::min(r.L.size(), r.R.size()), channels == 1);  // what the file holds
    checkExportLevel(mix, outputs[0]);
    r.peak = mix.peak;
    for (size_t i = 0; i < r.stemL.size(); ++i) {
      ExportLevel stem;
      stem.addFrames(r.stemL[i].data(), r.stemR[i].data(), (int)std::min(r.stemL[i].size(), r.stemR[i].size()), channels == 1);
      checkExportLevel(stem, outputs[i + 1]);
    }
  }
  try {
    writeWav(path + ".part", r.L, r.R, (int)sr, bits, channels);
    for (size_t i = 0; i < r.stemL.size(); ++i)
      writeWav(outputs[i + 1].path + ".part", r.stemL[i], r.stemR[i], (int)sr, bits, channels);
    int published = 0;
    publishRenderOutputs(outputs, published);
  } catch (...) {
    for (const auto& output : outputs) {
      std::error_code ignored;
      const auto part = std::filesystem::u8path(output.path + ".part");
      if (std::filesystem::is_regular_file(part, ignored)) std::filesystem::remove(part, ignored);
    }
    throw;
  }
  const double seconds = r.L.size() / sr;
  return {{"path", path}, {"stems", stemPaths}, {"seconds", seconds}, {"channels", channels}, {"sampleRate", sr}, {"peak", r.peak},
          {"renderMs", r.renderMs}, {"realtimeFactor", seconds * 1000 / std::max(0.001, r.renderMs)},
          {"denseEventBlocks", r.denseEventBlocks}};
}

// Plugins (tracks and effects) that could not be loaded: {key, plugin, why}. Their tracks keep their
// notes and the effects their state; they are silent ("Missing instrument").
json Server::missingPlugins() const {
  json out = json::array();
  for (const auto& m : missingPlugins_) out.push_back(m);
  return out;
}

json Server::stateEvent() {
  engine_->collectGarbage();
  // Meters, position and revision all describe the graph the audio thread is playing, which can be
  // older than the accepted project: its track ids come from the list kept at publish time.
  float master[2];
  const std::vector<std::string>* ids = nullptr;
  const uint64_t serial = engine_->appliedSerial();
  for (const auto& [s, list] : published_) if (s == serial) ids = &list;
  const size_t count = ids ? std::min(ids->size(), (size_t)Engine::kMaxTracks) : 0;
  std::vector<float> tracks(count * 2);
  engine_->takePeaks(master, tracks.data(), (int)count);
  json t = json::object();
  if (engine_->appliedSerial() == serial)  // a swap right now: these peaks may be the new graph's, skip one tick
    for (size_t i = 0; i < count; ++i) t[(*ids)[i]] = {tracks[2 * i], tracks[2 * i + 1]};
  const int64_t applied = engine_->appliedRevision();
  json state = {{"event", "state"}, {"sessionId", sessionId_},
          {"acceptedRevision", revision_ >= 0 ? json(revision_) : json()}, {"appliedRevision", applied >= 0 ? json(applied) : json()},
          {"position", engine_->position()}, {"tick", engine_->positionTicks()}, {"playing", engine_->playing()},
          {"master", {master[0], master[1]}}, {"gainReduction", std::round(engine_->takeMasterReduction() * 10) / 10}, {"tracks", t},
          {"earGuard", std::round(engine_->takeEarGuardReduction() * 10) / 10}};
  // The master's loudness (null: silence, or not long enough yet for that window).
  const LoudnessReading lr = engine_->loudness();
  auto rounded = [](double v, const char* format) {  // printed as such (not -15.450000000000001)
    char text[32];
    std::snprintf(text, sizeof text, format, v);
    return std::strtod(text, nullptr);
  };
  auto value = [&](double v) { return LoudnessReading::measured(v) ? json(rounded(v, "%.2f")) : json(); };
  state["loudness"] = {{"momentary", value(lr.momentary)}, {"shortTerm", value(lr.shortTerm)}, {"integrated", value(lr.integrated)},
                       {"truePeak", value(lr.truePeak)}, {"seconds", rounded(lr.seconds, "%.1f")}};
  if (analysis_) {  // only while a page shows it (60 numbers, 25 times a second)
    json bands = json::array();
    for (const float db : engine_->analyzer().spectrum()) bands.push_back(rounded(db, "%.1f"));
    state["analysis"] = {{"spectrum", bands}, {"bandsFromHz", 20}, {"bandsPerOctave", 6},
                         {"correlation", rounded(engine_->analyzer().correlation(), "%.2f")}};
  }
  if (engine_->watchingEffect()) {  // a Core plugin window shows one effect's input
    json bands = json::array();
    for (const float db : engine_->effectAnalyzer().spectrum()) bands.push_back(rounded(db, "%.1f"));
    state["effectSpectrum"] = std::move(bands);
  }
  json failed = failedPlugins();
  if (!failed.empty()) { state["failedPlugins"] = std::move(failed); logFailures(); }  // only when a plugin failed (M3b)
  // Built-in instruments and effects that failed (non-finite or runaway audio) and went silent: the app says
  // which, instead of a track that just goes quiet (Diogo's report 2026-11-06, "the engine never says so").
  if (const auto units = failedUnits(); !units.empty()) state["failedUnits"] = units;
  return state;
}

json Server::failedPlugins() const {
  json list = json::array();
  for (const auto& p : loadedPlugins_)
    if (p.processor && p.processor->failed())
      list.push_back({{"key", p.key}, {"plugin", p.name}, {"why", PluginProcessor::failureName(p.processor->failure())}});
  return list;
}

json Server::pluginVersions() const {
  json list = json::array();
  for (const auto& p : loadedPlugins_) {
    if (!p.processor) continue;
    const std::string installed = p.processor->installedVersion(), saved = p.ref.value("version", std::string());
    if (!installed.empty() && !saved.empty() && installed != saved)
      list.push_back({{"key", p.key}, {"plugin", p.name}, {"saved", saved}, {"installed", installed}});
  }
  return list;
}

void Server::logFailures() {
  for (const auto& p : loadedPlugins_) {
    if (!p.processor || !p.processor->failed()) continue;
    if (std::find(logged_.begin(), logged_.end(), p.processor.get()) != logged_.end()) continue;
    logged_.push_back(p.processor.get());
    if (failureLog_.empty()) continue;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char when[32] = {};
    std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &local);
    const json line = {{"time", when}, {"key", p.key}, {"plugin", p.name}, {"vendor", p.ref.value("vendor", "")},
                       {"version", p.ref.value("version", "")}, {"installed", p.processor->installedVersion()},
                       {"why", PluginProcessor::failureName(p.processor->failure())}};
    const std::filesystem::path file(std::u8string(reinterpret_cast<const char8_t*>(failureLog_.data()), failureLog_.size()));
    std::ofstream out(file, std::ios::app);
    if (out) out << line.dump() << '\n';
  }
}

json Server::cpu() {
  const uint64_t now = nowNs();
  auto& s = engine_->stats();
  const uint64_t render = s.totalRenderNs.load(), budget = s.totalBudgetNs.load();
  const double maxPct = s.windowMaxLoadPpm.exchange(0) / 1e4;
  const double avgPct = budget > cpuBudgetNs_ ? 100.0 * (double)(render - cpuRenderNs_) / (double)(budget - cpuBudgetNs_) : 0.0;
  cpuRenderNs_ = render; cpuBudgetNs_ = budget;
  json out = {{"load", {{"avgPct", avgPct}, {"maxPct", maxPct}, {"marginPct", std::max(0.0, 100.0 - maxPct)}}}};
  if (!cpu_) { out["intervalMs"] = 0; out["totalPct"] = 0; out["tracks"] = json::array(); out["master"] = {{"cpuPct", 0}, {"effects", json::array()}}; return out; }
  const auto& entries = cpu_->entries;
  if (cpuRead_ != cpuBuilt_ || cpuPrev_.size() != entries.size()) {  // another graph since the last call: from when it was built
    cpuRead_ = cpuBuilt_;
    cpuPrev_.assign(entries.size(), 0);
    cpuReadNs_ = cpuBuiltNs_;
  }
  const double wall = (double)std::max<uint64_t>(1, now - cpuReadNs_);
  cpuReadNs_ = now;
  json tracks = json::array(), masterFx = json::array();
  double total = 0, masterPct = 0;
  size_t track = SIZE_MAX;  // index of the track the next effects belong to
  for (size_t i = 0; i < entries.size(); ++i) {
    const uint64_t ns = cpu_->read(i);
    const double pct = 100.0 * (double)(ns - cpuPrev_[i]) / wall;
    cpuPrev_[i] = ns;
    total += pct;
    const auto& e = entries[i];
    if (e.track.empty()) { masterFx.push_back({{"id", e.effect}, {"name", e.name}, {"cpuPct", pct}}); masterPct += pct; continue; }
    if (e.effect.empty()) {
      tracks.push_back({{"id", e.track}, {"cpuPct", pct}, {"instrument", {{"name", e.name}, {"cpuPct", pct}}}, {"effects", json::array()}});
      track = tracks.size() - 1;
      continue;
    }
    if (track == SIZE_MAX) continue;
    tracks[track]["effects"].push_back({{"id", e.effect}, {"name", e.name}, {"cpuPct", pct}});
    tracks[track]["cpuPct"] = tracks[track]["cpuPct"].get<double>() + pct;
  }
  out["intervalMs"] = wall / 1e6;
  out["totalPct"] = total;
  out["tracks"] = std::move(tracks);
  out["master"] = {{"cpuPct", masterPct}, {"effects", std::move(masterFx)}};
  return out;
}

bool Server::takeEnded() { return engine_->takeEnded(); }

} // namespace core
