#include "EngineSession.h"
#include <thread>
#include <juce_events/juce_events.h>

namespace core::app {

using client::EngineError;

namespace {
void readJob(ExportJob& job, const json& j) {
  job.jobId = j.value("jobId", job.jobId);
  job.state = j.value("state", job.state);
  job.progress = j.value("progress", job.progress);
  job.path = j.value("path", job.path);
  job.error = j.value("error", std::string());
  const json revision = j.value("revision", json());
  job.revision = revision.is_number_integer() ? revision.get<int64_t>() : -1;
  if (j.contains("files") && j["files"].is_array()) job.files = std::max(1, (int)j["files"].size());
  job.file = j.value("file", job.file);
}
}

const char* statusText(EngineStatus status) {
  switch (status) {
    case EngineStatus::Starting: return "Starting";
    case EngineStatus::Ready: return "Ready";
    case EngineStatus::Syncing: return "Syncing";
    case EngineStatus::NoDevice: return "No device";
    case EngineStatus::Diverged: return "Document refused";
    case EngineStatus::Stopped: return "Engine stopped";
  }
  return "";
}

EngineSession::EngineSession(Options options, doc::Document document)
    : options_(std::move(options)), document_(std::move(document)) {
  wantedProjection_ = doc::toProjection(document_);
  wantedDocument_ = document_;
  loop_ = document_.loop;
  snap_.documentRevision = wantedRevision_;
  worker_ = std::thread([this] { run(); });
}

EngineSession::~EngineSession() {
  { std::lock_guard<std::mutex> g(mutex_); quit_ = true; }
  wake_.notify_all();
  worker_.join();
}

void EngineSession::setDocument(const doc::Document& document) {
  document_ = document;
  json projection = doc::toProjection(document, assetRoot_);
  std::lock_guard<std::mutex> g(mutex_);
  wantedDocument_ = document;
  if (projection == wantedProjection_) return;  // a visual change: nothing to send
  wantedProjection_ = std::move(projection);
  snap_.documentRevision = ++wantedRevision_;   // monotonic, also across engine sessions and undo
  if (snap_.status == EngineStatus::Diverged) snap_.status = EngineStatus::Syncing;  // a full snapshot reconciles
  retryPending_ = false;  // the edit is sent now
  syncRetry_.reset();
  wake_.notify_all();
}

void EngineSession::setAssetRoot(const std::filesystem::path& root, bool send) {
  assetRoot_ = root;
  if (!send) return;
  json projection = doc::toProjection(document_, assetRoot_);
  std::lock_guard<std::mutex> g(mutex_);
  wantedProjection_ = std::move(projection);
  snap_.documentRevision = ++wantedRevision_;
  wantedDocument_ = document_;
  if (snap_.status == EngineStatus::Diverged) snap_.status = EngineStatus::Syncing;
  retryPending_ = false;
  syncRetry_.reset();
  wake_.notify_all();
}

namespace {
// A short-lived waiter thread: the message thread goes on; the answer (or nothing) comes back to it.
void answerLater(std::shared_ptr<std::future<json>> answer, int timeoutMs, std::function<void(json)> done) {
  std::thread([answer, timeoutMs, done = std::move(done)]() mutable {
    json result;  // null: no answer (an engine that answers with nothing gives {})
    if (answer->wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
      try { result = answer->get(); } catch (const std::exception&) {}  // dropped (engine restarted): nothing
    }
    juce::MessageManager::callAsync([result = std::move(result), done = std::move(done)] { done(result); });
  }).detach();
}
}  // namespace

void EngineSession::cpuAsync(int timeoutMs, std::function<void(json)> done) {
  Action a{Action::Cpu};
  a.reply = std::make_shared<std::promise<json>>();
  auto answer = std::make_shared<std::future<json>>(a.reply->get_future());
  queue(a);
  answerLater(answer, timeoutMs, std::move(done));
}

json EngineSession::pluginStates(int timeoutMs) {
  Action a{Action::PluginStates};
  a.reply = std::make_shared<std::promise<json>>();
  std::future<json> answer = a.reply->get_future();
  queue(a);
  // null when the engine gave no answer; {} is an answer (no loaded plugin holds a state), and the two must not be
  // confused: an empty answer counted as "no answer" made Save report a failure that was not one.
  if (answer.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) return json();
  try {
    return answer.get();
  } catch (const std::exception&) {  // dropped (engine restarted): no answer
    return json();
  }
}

void EngineSession::pluginStatesAsync(int timeoutMs, std::function<void(json)> done) {
  Action a{Action::PluginStates};
  a.reply = std::make_shared<std::promise<json>>();
  auto answer = std::make_shared<std::future<json>>(a.reply->get_future());
  queue(a);
  answerLater(answer, timeoutMs, std::move(done));
}

json EngineSession::pluginParams(const std::string& key, int timeoutMs) {
  Action a{Action::PluginParams};
  a.text = key;
  a.reply = std::make_shared<std::promise<json>>();
  std::future<json> answer = a.reply->get_future();
  queue(a);
  if (answer.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) return json::array();
  try {
    const json list = answer.get();
    return list.is_array() ? list : json::array();
  } catch (const std::exception&) {  // dropped (engine restarted)
    return json::array();
  }
}

std::string EngineSession::pluginPreset(const std::string& key, int timeoutMs) {
  Action a{Action::PluginPreset};
  a.text = key;
  a.reply = std::make_shared<std::promise<json>>();
  std::future<json> answer = a.reply->get_future();
  queue(a);
  if (answer.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) return {};
  try {
    const json r = answer.get();
    return r.is_object() ? r.value("preset", std::string()) : std::string();
  } catch (const std::exception&) {  // dropped (engine restarted)
    return {};
  }
}

void EngineSession::releasePlugins() { queue({Action::ReleasePlugins}); }

void EngineSession::openPluginEditor(const std::string& key) {
  Action a{Action::PluginEditor};
  a.text = key;
  queue(a);
}

void EngineSession::newSong() {
  std::lock_guard<std::mutex> g(mutex_);
  ++documentEpoch_;
  acceptedDocument_.reset();
  snap_.hasAcceptedDocument = false;
}

std::optional<doc::Document> EngineSession::acceptedDocument() const {
  std::lock_guard<std::mutex> g(mutex_);
  return acceptedDocument_;
}

void EngineSession::queue(Action action) { std::lock_guard<std::mutex> g(mutex_); actions_.push_back(action); wake_.notify_all(); }
// Play and Stop are shown at once: until the engine has taken the last of them, the snapshot says what
// was asked, so a second press (Space twice, or Stop right after Play) stops instead of playing again.
void EngineSession::queueTransport(Action action, bool playing) {
  std::lock_guard<std::mutex> g(mutex_);
  actions_.push_back(action);
  ++transportPending_;
  transportIntent_ = playing;
  snap_.playing = playing;
  wake_.notify_all();
}
void EngineSession::play() { queueTransport({Action::Play}, true); }
void EngineSession::stop() { queueTransport({Action::Stop}, false); }
void EngineSession::home() { queueTransport({Action::Home}, false); }
void EngineSession::seek(int64_t tick) { queue({Action::Seek, tick}); }
void EngineSession::setLoop(const doc::Loop& loop) {
  std::lock_guard<std::mutex> g(mutex_);
  loop_ = loop;  // also sent again after an engine restart
  actions_.push_back({Action::SetLoop});
  wake_.notify_all();
}
void EngineSession::exportFiles(const std::vector<ExportFile>& files, int64_t startTick, int64_t endTick, int bits) {
  if (files.empty()) return;
  json list = json::array();
  for (const auto& f : files) {
    json one = {{"path", f.pathUtf8}};
    if (!f.trackId.empty()) one["track"] = f.trackId;
    if (!f.trackId.empty() && f.withoutMasterGain) one["withoutMasterGain"] = true;
    if (f.mono) one["mono"] = true;
    list.push_back(std::move(one));
  }
  std::lock_guard<std::mutex> g(mutex_);
  snap_.exportJob = ExportJob{};
  snap_.exportJob.state = "starting";
  snap_.exportJob.path = files.front().pathUtf8;
  snap_.exportJob.files = (int)files.size();
  Action a{Action::Export};
  a.text = files.front().pathUtf8;
  a.bits = bits;
  a.args = {{"files", list}, {"startTick", startTick}, {"endTick", endTick < 0 ? json() : json(endTick)}};
  actions_.push_back(a);
  wake_.notify_all();
}

void EngineSession::exportWav(const std::string& pathUtf8, int bits) {
  std::lock_guard<std::mutex> g(mutex_);
  snap_.exportJob = ExportJob{};
  snap_.exportJob.state = "starting";
  snap_.exportJob.path = pathUtf8;
  Action a{Action::Export};
  a.text = pathUtf8;
  a.bits = bits;
  actions_.push_back(a);
  wake_.notify_all();
}
void EngineSession::cancelExport() { queue({Action::CancelExport}); }
void EngineSession::liveNote(const std::string& trackId, int pitch, double velocity, bool on) {
  Action a{Action::LiveNote};
  a.text = trackId;
  a.tick = pitch;
  a.velocity = velocity;
  a.on = on;
  queue(a);
}
void EngineSession::panic() { queue({Action::Panic}); }
void EngineSession::resetLoudness() { queue({Action::ResetLoudness}); }
void EngineSession::setListenLevel(double db) {
  if (!(db == db)) return;
  {
    std::lock_guard<std::mutex> g(mutex_);
    listenDb_ = std::clamp(db, -60.0, 0.0);
  }
  queue({Action::Listen});
}
void EngineSession::watchEffect(const std::string& owner, const std::string& effectId) {
  {
    std::lock_guard<std::mutex> g(mutex_);
    watchOwner_ = owner;
    watchEffect_ = effectId;
  }
  queue({Action::Watch});
}
void EngineSession::enableAnalysis(bool on) {
  Action a{Action::Analysis};
  a.on = on;
  queue(a);
}
void EngineSession::selectDevice(const std::string& name) {
  std::lock_guard<std::mutex> g(mutex_);
  options_.device = name;
  actions_.push_back({Action::SelectDevice});
  wake_.notify_all();
}
void EngineSession::restart() { std::lock_guard<std::mutex> g(mutex_); restartRequested_ = true; wake_.notify_all(); }

void EngineSession::killEngineForTest() { std::lock_guard<std::mutex> g(mutex_); if (client_) client_->killForTest(); }

SessionSnapshot EngineSession::snapshot() const { std::lock_guard<std::mutex> g(mutex_); return snap_; }

void EngineSession::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  bool first = true;
  while (!quit_) {
    if (first || restartRequested_) {
      first = false;
      restartRequested_ = false;
      for (auto& a : actions_) if (a.reply) a.reply->set_value(json());  // nobody waits for a dropped answer (null: no answer)
      actions_.clear();  // never resume sound automatically after a failure or a restart
      transportPending_ = 0;
      std::unique_ptr<client::EngineClient> old = std::move(client_);
      lock.unlock();
      if (old) old->close();
      old.reset();
      connect();
      lock.lock();
      continue;
    }
    const bool connected = client_ && snap_.status != EngineStatus::Stopped;
    if (!connected)
      for (auto& a : actions_) if (a.reply) { a.reply->set_value(json()); a.reply.reset(); }  // null: no answer
    if (connected && !actions_.empty()) {
      const Action action = actions_.front();
      actions_.pop_front();
      // In ticks: the sample counts depend on the device rate, the song length in ticks does not.
      const int64_t songTicks = wantedProjection_.value("lengthTicks", (int64_t)0);
      const bool fromTheEnd = action.kind == Action::Play && songTicks > 0 && snap_.positionTick >= (double)songTicks;
      // An export takes the document as it is now: send it first if an edit is still pending. So does opening a
      // plugin's window or reading its parameters: a plugin just added is only in the engine once its edit is sent
      // (audit R05: the editor was asked for a plugin the engine did not have yet).
      const bool needsDocument = action.kind == Action::Export || action.kind == Action::PluginEditor || action.kind == Action::PluginParams ||
                                 action.kind == Action::PluginPreset;
      if (needsDocument && wantedRevision_ > sentRevision_ && snap_.status != EngineStatus::Diverged) sync(lock);
      if (action.kind == Action::Export && (snap_.status == EngineStatus::Diverged || snap_.acceptedRevision != wantedRevision_)) {
        snap_.exportJob.state = "failed";
        snap_.exportJob.error = "The engine did not accept the current document; nothing was exported.";
        continue;
      }
      const doc::Loop loop = loop_;
      const int64_t accepted = snap_.acceptedRevision;
      const uint64_t jobId = snap_.exportJob.jobId;
      lock.unlock();
      try {
        switch (action.kind) {
          case Action::Play:
            if (fromTheEnd) client_->command("transport.seek", {{"tick", 0}});  // at the end: play from the start
            client_->command("transport.play");
            break;
          case Action::Stop: case Action::Home:
            client_->command("transport.stop", {{"rewind", action.kind == Action::Home}});
            break;
          case Action::Seek:
            client_->command("transport.seek", {{"tick", action.tick}});
            break;
          case Action::SetLoop:
            client_->command("transport.loop", {{"enabled", loop.enabled}, {"startTick", loop.startTick}, {"endTick", loop.endTick}});
            break;
          case Action::Export: {
            json args = action.args.is_object() ? action.args : json{{"path", action.text}};
            args["bits"] = action.bits;
            args["tailMs"] = 2000;
            args["revision"] = accepted;
            const json job = client_->command("render.start", args);
            std::lock_guard<std::mutex> g(mutex_);
            readJob(snap_.exportJob, job);  // the reply comes before any event of this job
            break;
          }
          case Action::CancelExport:
            if (jobId) client_->command("render.cancel", {{"jobId", jobId}});
            break;
          case Action::LiveNote:
            client_->command(action.on ? "note.on" : "note.off", {{"trackId", action.text}, {"pitch", action.tick}, {"velocity", action.velocity}});
            break;
          case Action::Panic:
            client_->command("transport.panic");
            break;
          case Action::ResetLoudness:
            client_->command("loudness.reset");
            break;
          case Action::Analysis:
            client_->command("analysis.enable", {{"on", action.on}});
            break;
          case Action::Watch: {
            std::string owner, fx;
            { std::lock_guard<std::mutex> g(mutex_); owner = watchOwner_; fx = watchEffect_; }  // the latest
            client_->command("analysis.watch", {{"trackId", owner}, {"effectId", fx}});
            break;
          }
          case Action::Listen: {
            double db;
            { std::lock_guard<std::mutex> g(mutex_); db = listenDb_; }  // the latest, if several were queued
            client_->command("listen.set", {{"db", db}});
            break;
          }
          case Action::SelectDevice:
            openDevice(*client_);
            restoreTransport(*client_);  // a device at another rate means a new audio engine
            break;
          case Action::Cpu:
            action.reply->set_value(client_->command("engine.cpu"));
            break;
          case Action::PluginStates:
            action.reply->set_value(client_->command("plugins.states"));
            break;
          case Action::PluginParams:
            action.reply->set_value(client_->command("plugin.params", {{"key", action.text}}));
            break;
          case Action::PluginPreset:
            action.reply->set_value(client_->command("plugin.preset", {{"key", action.text}}));
            break;
          case Action::PluginEditor:
            client_->command("plugin.editor", {{"key", action.text}, {"open", true}});
            break;
          case Action::ReleasePlugins:
            client_->command("plugins.release");
            break;
        }
      } catch (const EngineError& e) {
        std::lock_guard<std::mutex> g(mutex_);
        if (action.kind == Action::Export) { snap_.exportJob.state = "failed"; snap_.exportJob.error = e.what(); }
        else if (action.reply) action.reply->set_value(json());  // null: no answer
        else snap_.message = e.what();
      }
      lock.lock();
      if ((action.kind == Action::Play || action.kind == Action::Stop || action.kind == Action::Home) && transportPending_ > 0 &&
          --transportPending_ == 0)
        snap_.playing = transportIntent_;  // what the engine does now; its next state events say the same
      continue;
    }
    // Audit 2, bug 2: a project.load that got no answer is sent again by itself, with a new revision (the engine
    // may have taken the old one late and would refuse it again as stale).
    if (connected && retryPending_ && snap_.status == EngineStatus::Diverged && std::chrono::steady_clock::now() >= retryAt_) {
      retryPending_ = false;
      snap_.documentRevision = ++wantedRevision_;
      snap_.status = EngineStatus::Syncing;
      sync(lock);
      continue;
    }
    if (connected && wantedRevision_ > sentRevision_ && snap_.status != EngineStatus::Diverged) {
      sync(lock);
      continue;
    }
    auto ready = [&] {
      const bool canSend = client_ && snap_.status != EngineStatus::Stopped;
      return quit_ || restartRequested_ || (canSend && !actions_.empty()) ||
             (canSend && wantedRevision_ > sentRevision_ && snap_.status != EngineStatus::Diverged) ||
             (canSend && retryPending_ && std::chrono::steady_clock::now() >= retryAt_);
    };
    if (retryPending_) wake_.wait_until(lock, retryAt_, ready);
    else wake_.wait(lock, ready);
  }
  std::unique_ptr<client::EngineClient> last = std::move(client_);
  lock.unlock();
  if (last) last->close();
}

bool EngineSession::connect() {
  client::EngineOptions o;
  o.binary = options_.engineBinary;
  o.stderrLog = options_.engineLog;
  if (!options_.dumpFolder.empty()) {
    const std::u8string folder = options_.dumpFolder.u8string();
    const std::u8string log = (options_.dumpFolder / "plugin-failures.log").u8string();  // one JSON line per plugin failure
    o.arguments = {"serve", "--dumps", std::string(folder.begin(), folder.end()), "--plugin-log", std::string(log.begin(), log.end())};
  }
  auto c = std::make_unique<client::EngineClient>(o);
  c->setEventHandler([this](const json& e) { onEvent(e); });
  {
    std::lock_guard<std::mutex> g(mutex_);
    snap_ = SessionSnapshot{};
    // The count goes on across a restart: one that starts at 0 again looks like a plugin's own change to the
    // window, which compares with the last count it saw, and marks the song modified (audit 2026-11-17, item 8).
    snap_.pluginEdits = pluginEditsTotal_;
    snap_.status = EngineStatus::Starting;
    snap_.message = "Starting the engine...";
    snap_.documentRevision = wantedRevision_;
  }
  try {
    c->start();
  } catch (const EngineError& e) {
    std::lock_guard<std::mutex> g(mutex_);
    snap_.status = EngineStatus::Stopped;
    snap_.message = std::string("The engine did not start: ") + e.what();
    return false;
  }
  client::EngineClient* engine = c.get();
  {
    std::lock_guard<std::mutex> g(mutex_);
    snap_.sessionId = c->sessionId();
    snap_.engineBuild = c->hello().value("build", std::string("unversioned"));
    client_ = std::move(c);
    sentRevision_ = 0;  // a new session knows nothing: send the current projection again
    retryPending_ = false;
    syncRetry_.reset();
  }
  // The document first (it works without a device), then the device (opening it at another sample
  // rate starts a new audio engine), then the loop and the last known position (stopped).
  {
    std::unique_lock<std::mutex> lock(mutex_);
    sync(lock);
  }
  try {
    std::vector<std::string> names;
    json args = json::object();
    if (!options_.backend.empty()) args["backend"] = options_.backend;  // the same backend that engine.start uses
    for (const auto& d : engine->command("devices.list", args).value("devices", json::array())) names.push_back(d.value("name", ""));
    std::lock_guard<std::mutex> g(mutex_);
    snap_.devices = std::move(names);
  } catch (const EngineError&) {}
  openDevice(*engine);
  restoreTransport(*engine);
  return true;
}

void EngineSession::restoreTransport(client::EngineClient& engine) {
  doc::Loop loop;
  double tick;
  {
    std::lock_guard<std::mutex> g(mutex_);
    loop = loop_;
    tick = lastTick_;
  }
  std::string owner, fx;
  { std::lock_guard<std::mutex> g(mutex_); owner = watchOwner_; fx = watchEffect_; }
  try {
    engine.command("transport.loop", {{"enabled", loop.enabled}, {"startTick", loop.startTick}, {"endTick", loop.endTick}});
    engine.command("transport.seek", {{"tick", (int64_t)tick}});
    if (!fx.empty()) engine.command("analysis.watch", {{"trackId", owner}, {"effectId", fx}});
  } catch (const EngineError&) {}  // reported by the next state / sync
}

// Called without the lock.
void EngineSession::openDevice(client::EngineClient& engine) {
  json args = json::object();
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (!options_.backend.empty()) args["backend"] = options_.backend;
    if (!options_.device.empty()) args["device"] = options_.device;
    args["listenDb"] = listenDb_;
  }
  try {
    const json device = engine.command("engine.start", args);
    std::lock_guard<std::mutex> g(mutex_);
    snap_.sampleRate = device.value("sampleRate", 48000.0);
    snap_.device = device.value("device", std::string());
    snap_.outputLatencyMs = std::clamp(device.value("latencyMs", 0.0), 0.0, 500.0);
    if (snap_.status == EngineStatus::Starting || snap_.status == EngineStatus::NoDevice) snap_.status = EngineStatus::Syncing;
    snap_.message = "Output: " + device.value("backend", std::string("?")) + " " + snap_.device +
                    " · " + std::to_string((int)snap_.sampleRate) + " Hz · block " + std::to_string(device.value("blockSize", 0));
  } catch (const EngineError& e) {
    std::lock_guard<std::mutex> g(mutex_);
    snap_.device.clear();
    if (snap_.status != EngineStatus::Stopped && snap_.status != EngineStatus::Diverged) snap_.status = EngineStatus::NoDevice;
    snap_.message = std::string("No audio device: ") + e.what();
  }
}

// Called with the lock held; sends the latest projection and waits for the answer without the lock.
void EngineSession::sync(std::unique_lock<std::mutex>& lock) {
  const json projection = wantedProjection_;
  const doc::Document document = wantedDocument_;
  const auto epoch = documentEpoch_;
  const int64_t revision = wantedRevision_;
  sentRevision_ = revision;
  if (snap_.status == EngineStatus::Ready || snap_.status == EngineStatus::Diverged) snap_.status = EngineStatus::Syncing;
  client::EngineClient* engine = client_.get();
  lock.unlock();
  try {
    const json result = engine->command("project.load", {{"project", projection}, {"revision", revision}});
    lock.lock();
    if (epoch == documentEpoch_) { acceptedDocument_ = document; snap_.hasAcceptedDocument = true; }
    syncRetry_.succeeded();
    snap_.acceptedRevision = std::max(snap_.acceptedRevision, result.value("acceptedRevision", revision));
    snap_.songSamples = result.value("lengthSamples", (int64_t)0);
    // Plugins that could not be loaded: their tracks are silent ("missing").
    snap_.missingPlugins.clear();
    for (const auto& m : result.value("missing", json::array()))
      if (m.is_object() && m.contains("key"))
        snap_.missingPlugins[m.value("key", std::string())] = m.value("plugin", std::string("plugin")) + ": " + m.value("why", std::string());
    // Audio files the engine could not load (too big for its memory, unreadable): say so.
    const json missing = result.value("missingAssets", json::array());
    if (!missing.empty() && missing.is_array())
      snap_.message = "Audio the engine could not load (" + std::to_string(missing.size()) + "): " + client::engineLineText(missing[0]);
  } catch (const EngineError& e) {
    lock.lock();
    if (e.kind == EngineError::Kind::Rejected) {
      retryPending_ = false;
      snap_.status = EngineStatus::Diverged;
      snap_.message = std::string("The engine refused the document (your edits are kept): ") + e.what();
    } else if (e.kind != EngineError::Kind::Disconnected) {  // a disconnect has its own state and restart
      // Not sent or no answer (too large, timeout): the engine plays an older document. Say so. A timeout is
      // tried again by itself (SyncRetry); what cannot get better waits for the next edit or a restart.
      snap_.status = EngineStatus::Diverged;
      snap_.message = std::string("The document did not reach the engine (your edits are kept): ") + e.what();
      int delayMs = 0;
      if (syncRetry_.failed(e.kind == EngineError::Kind::Timeout, delayMs) == client::SyncRetry::After::Retry) {
        retryPending_ = true;
        retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
        snap_.message += " Trying again in " + std::to_string((delayMs + 999) / 1000) + " s.";
      } else {
        retryPending_ = false;
      }
    }
  }
}

void EngineSession::onEvent(const json& event) {
  const std::string kind = event.value("event", "");
  std::lock_guard<std::mutex> g(mutex_);
  if (kind == "disconnected") {
    snap_.status = EngineStatus::Stopped;
    snap_.playing = false;
    snap_.message = "The engine stopped: " + event.value("reason", std::string()) + " The document is intact.";
    wake_.notify_all();
    return;
  }
  if (kind == "ended") { if (transportPending_ == 0) snap_.playing = false; return; }
  if (kind == "pluginsChanged") { snap_.pluginEdits = ++pluginEditsTotal_; return; }
  if (kind == "job") {
    // Only the current job's events: an older job to the same file must not be taken for it.
    if (snap_.exportJob.jobId != 0 && event.value("jobId", (uint64_t)0) == snap_.exportJob.jobId) readJob(snap_.exportJob, event);
    return;
  }
  if (kind != "state") return;
  if (!snap_.sessionId.empty() && event.value("sessionId", "") != snap_.sessionId) return;  // another session
  // A Play or Stop still on its way: the state the engine had before it, so the asked one stays shown.
  snap_.playing = transportPending_ > 0 ? transportIntent_ : event.value("playing", false);
  snap_.positionSamples = event.value("position", (int64_t)0);
  snap_.positionTick = event.value("tick", 0.0);
  snap_.positionAtMs = juce::Time::getMillisecondCounterHiRes();
  lastTick_ = snap_.positionTick;
  const json accepted = event.value("acceptedRevision", json()), applied = event.value("appliedRevision", json());
  if (accepted.is_number_integer()) snap_.acceptedRevision = accepted.get<int64_t>();
  snap_.appliedRevision = applied.is_number_integer() ? applied.get<int64_t>() : -1;
  const json master = event.value("master", json()), tracks = event.value("tracks", json());
  auto stereoPeak = [](const json& v) { return v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number(); };
  if (stereoPeak(master)) { snap_.masterPeak[0] = master[0].get<float>(); snap_.masterPeak[1] = master[1].get<float>(); }
  {
    const json l = event.value("loudness", json());
    auto value = [&](const char* key) { return l.is_object() && l.contains(key) && l[key].is_number() ? l[key].get<double>() : (double)NAN; };
    snap_.loudness = {value("momentary"), value("shortTerm"), value("integrated"), value("truePeak"), l.is_object() ? l.value("seconds", 0.0) : 0.0};
    const json a = event.value("analysis", json());
    snap_.analysis.spectrum.clear();
    if (a.is_object() && a.contains("spectrum") && a["spectrum"].is_array())
      for (const json& v : a["spectrum"]) snap_.analysis.spectrum.push_back(v.is_number() ? v.get<float>() : -120.0f);
    snap_.analysis.correlation = a.is_object() ? a.value("correlation", 0.0) : 0.0;
    snap_.effectSpectrum.clear();
    for (const json& v : event.value("effectSpectrum", json::array())) snap_.effectSpectrum.push_back(v.is_number() ? v.get<float>() : -120.0f);
  }
  snap_.failedUnits.clear();
  for (const json& u : event.value("failedUnits", json::array())) if (u.is_string()) snap_.failedUnits.push_back(u.get<std::string>());
  for (const json& p : event.value("failedPlugins", json::array()))
    if (p.is_object()) snap_.failedUnits.push_back(p.value("plugin", std::string("plugin")) + ": " + p.value("why", std::string("failed")));
  // The engine names each peak with the ids of the graph that produced it (the applied revision).
  snap_.trackPeaks.clear();
  if (tracks.is_object())
    for (auto it = tracks.begin(); it != tracks.end(); ++it)
      if (stereoPeak(it.value())) snap_.trackPeaks[it.key()] = {it.value()[0].get<float>(), it.value()[1].get<float>()};
  if (snap_.status == EngineStatus::Ready || snap_.status == EngineStatus::Syncing)
    snap_.status = snap_.appliedRevision == wantedRevision_ ? EngineStatus::Ready : EngineStatus::Syncing;
}

} // namespace core::app
