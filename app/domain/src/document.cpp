#include "document.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <regex>
#include <set>
#include <map>
#include <functional>

namespace core::doc {

namespace {

constexpr double kMinBpm = 40, kMaxBpm = 600;
constexpr int64_t kMaxSongTicks = 4096LL * 4 * kPpq, kMaxPatternTicks = 64LL * 4 * kPpq;
constexpr size_t kMaxTracks = 128, kMaxNotes = kMaxPatternNotes, kMaxEffects = 16, kMaxPatterns = 64, kMaxClips = 256, kMaxSections = 64;

std::string join(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }
std::string item(const std::string& path, size_t i) { return path + "[" + std::to_string(i) + "]"; }

bool validId(const std::string& id) {
  static const std::regex pattern("^[A-Za-z0-9_-]{1,40}$");
  return std::regex_match(id, pattern);
}

// True when a part of the path (between '/' or '\\') is "..": that would leave the project folder.
bool hasDotDotPart(const std::string& path) {
  size_t start = 0;
  while (true) {
    const size_t end = path.find_first_of("/\\", start);
    if (path.compare(start, (end == std::string::npos ? path.size() : end) - start, "..") == 0 &&
        (end == std::string::npos ? path.size() : end) - start == 2)
      return true;
    if (end == std::string::npos) return false;
    start = end + 1;
  }
}

// Reads the known keys of one object and keeps the others for writing back.
class Reader {
  json rest_;
  std::string path_;
public:
  Reader(const json& value, std::string path) : path_(std::move(path)) {
    if (!value.is_object()) throw DocumentError(path_, "must be an object");
    rest_ = value;
  }
  const std::string& path() const { return path_; }
  std::string at(const char* key) const { return join(path_, key); }
  json take(const char* key) {
    auto it = rest_.find(key);
    if (it == rest_.end()) return json();
    json v = std::move(*it);
    rest_.erase(it);
    return v;
  }
  double number(const char* key, double fallback, double lo, double hi) {
    json v = take(key);
    if (v.is_null()) return fallback;
    if (!v.is_number()) throw DocumentError(at(key), "must be a number");
    const double x = v.get<double>();
    if (!std::isfinite(x) || x < lo || x > hi)
      throw DocumentError(at(key), "must be between " + json(lo).dump() + " and " + json(hi).dump());
    return x;
  }
  int64_t integer(const char* key, int64_t fallback, int64_t lo, int64_t hi) {
    json v = take(key);
    if (v.is_null()) return fallback;
    if (!v.is_number()) throw DocumentError(at(key), "must be a whole number");
    const double x = v.get<double>();
    if (!std::isfinite(x) || x != std::floor(x)) throw DocumentError(at(key), "must be a whole number");
    if (x < (double)lo || x > (double)hi) throw DocumentError(at(key), "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    return (int64_t)x;
  }
  bool boolean(const char* key, bool fallback) {
    json v = take(key);
    if (v.is_null()) return fallback;
    if (!v.is_boolean()) throw DocumentError(at(key), "must be true or false");
    return v.get<bool>();
  }
  std::string string(const char* key, const std::string& fallback, size_t maxLength) {
    json v = take(key);
    if (v.is_null()) return fallback;
    if (!v.is_string()) throw DocumentError(at(key), "must be text");
    std::string s = v.get<std::string>();
    // Bytes of UTF-8, as the limit counts them: an accented letter takes 2 (deep hunt P01: it said "characters").
    if (s.size() > maxLength) throw DocumentError(at(key), "is longer than " + std::to_string(maxLength) + " bytes (an accented letter counts as 2)");
    return s;
  }
  json object(const char* key) {
    json v = take(key);
    if (v.is_null()) return json::object();
    if (!v.is_object()) throw DocumentError(at(key), "must be an object");
    return v;
  }
  json array(const char* key, size_t maxItems) {
    json v = take(key);
    if (v.is_null()) return json::array();
    if (!v.is_array()) throw DocumentError(at(key), "must be a list");
    if (v.size() > maxItems) throw DocumentError(at(key), "has more than " + std::to_string(maxItems) + " items");
    return v;
  }
  json rest() { return std::move(rest_); }
};

json withExtra(json known, const json& extra) {
  for (auto it = extra.begin(); it != extra.end(); ++it) known[it.key()] = it.value();
  return known;
}

Note parseNote(const json& v, const std::string& path, int64_t patternTicks) {
  Reader r(v, path);
  Note n;
  n.id = r.string("id", "", 40);
  if (!validId(n.id)) throw DocumentError(r.at("id"), "must be 1-40 letters, digits, '_' or '-'");
  n.tick = r.integer("tick", 0, 0, patternTicks - 1);
  n.length = r.integer("length", 240, 1, patternTicks);
  n.pitch = (int)r.integer("pitch", 36, 0, 127);
  n.velocity = r.number("velocity", 0.8, 0, 1);
  json roll = r.take("roll");
  if (!roll.is_null()) {
    Reader rr(roll, r.at("roll"));
    n.hasRoll = true;
    n.roll.count = (int)rr.integer("count", 0, 0, 64);
    n.roll.velocityFrom = rr.number("velocityFrom", n.velocity, 0, 1);
    n.roll.velocityTo = rr.number("velocityTo", n.velocity, 0, 1);
    n.roll.extra = rr.rest();
  }
  n.extra = r.rest();
  return n;
}

// One automation per parameter; on the master only the master volume and the master effects.
std::vector<Automation> readAutomation(Reader& r, bool master = false) {
  std::vector<Automation> out;
  const json automation = r.array("automation", 256);
  for (size_t i = 0; i < automation.size(); ++i) {
    Reader ar(automation[i], item(r.at("automation"), i));
    Automation a;
    a.param = ar.string("param", "", 64);
    if (a.param.empty()) throw DocumentError(ar.at("param"), "is required");
    if (master && a.param != "gainDb" && a.param.rfind("fx:", 0) != 0)
      throw DocumentError(ar.at("param"), "on the master only 'gainDb' and effect parameters can be automated");
    for (const auto& other : out)
      if (other.param == a.param) throw DocumentError(ar.at("param"), "is automated twice");
    const json points = ar.array("points", 100000);
    for (size_t j = 0; j < points.size(); ++j) {
      Reader ptr(points[j], item(ar.at("points"), j));
      AutomationPoint p;
      p.tick = ptr.integer("tick", 0, 0, kMaxSongTicks);
      p.value = ptr.number("value", 0, -1e9, 1e9);
      p.extra = ptr.rest();
      if (!a.points.empty() && p.tick < a.points.back().tick) throw DocumentError(ptr.at("tick"), "points must be in time order");
      a.points.push_back(std::move(p));
    }
    a.extra = ar.rest();
    out.push_back(std::move(a));
  }
  return out;
}

json automationJson(const std::vector<Automation>& list) {
  json automation = json::array();
  for (const auto& a : list) {
    json points = json::array();
    for (const auto& p : a.points) points.push_back(withExtra({{"tick", p.tick}, {"value", p.value}}, p.extra));
    automation.push_back(withExtra({{"param", a.param}, {"points", points}}, a.extra));
  }
  return automation;
}

json envelopesJson(const std::vector<Automation>& list) {
  json envelopes = json::array();
  for (const auto& a : list) {
    json points = json::array();
    for (const auto& p : a.points) points.push_back({{"tick", p.tick}, {"value", p.value}});
    envelopes.push_back({{"param", a.param}, {"points", points}});
  }
  return envelopes;
}

// Effect ids the engine would rename (deep hunt P03): ':' (it splits "fx:<track>:<id>"), longer than 40 bytes, empty or
// used twice in a chain. The engine renamed them on its side only, so the app's plugin keys and curves pointed at
// nothing and the plugin's state was lost. They are renamed here as the engine does (the first of a pair keeps the id
// and the curves; ':' becomes '_'; a number is added), with the curves and modulators that name them, so the engine
// gets ids it keeps as they are.
void cleanEffectIds(json& chain, std::vector<Automation>& curves, json* modulators) {
  auto clean = [](const std::string& id) { return !id.empty() && id.size() <= 40 && id.find(':') == std::string::npos; };
  std::vector<std::string> ids;
  std::vector<bool> keep;
  for (const auto& fx : chain) {
    const std::string id = fx.contains("id") && fx["id"].is_string() ? fx["id"].get<std::string>() : std::string();
    const bool k = clean(id) && std::find(ids.begin(), ids.end(), id) == ids.end();
    if (k) ids.push_back(id);
    keep.push_back(k);
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    if (keep[i]) continue;
    json& fx = chain[i];
    const std::string old = fx.contains("id") && fx["id"].is_string() ? fx["id"].get<std::string>() : std::string();
    std::string base = old.empty() ? std::string("fx") : old.substr(0, 40);
    while (!base.empty() && ((unsigned char)base.back() & 0xC0) == 0x80) base.pop_back();  // never half a character
    std::replace(base.begin(), base.end(), ':', '_');
    if (base.empty()) base = "fx";
    std::string id = base;
    for (int n = 2; std::find(ids.begin(), ids.end(), id) != ids.end(); ++n) {
      std::string root = base.substr(0, 36);
      while (!root.empty() && ((unsigned char)root.back() & 0xC0) == 0x80) root.pop_back();
      id = root + "_" + std::to_string(n);
    }
    const bool owned = !old.empty() && std::find(ids.begin(), ids.end(), old) != ids.end();  // a twin before keeps them
    ids.push_back(id);
    fx["id"] = id;
    if (owned || old.empty()) continue;
    const std::string from = "fx:" + old + ":", to = "fx:" + id + ":";
    for (auto& a : curves)
      if (a.param.rfind(from, 0) == 0) {
        const std::string renamed = to + a.param.substr(from.size());
        if (std::none_of(curves.begin(), curves.end(), [&](const Automation& o) { return o.param == renamed; }) && renamed.size() <= 64)
          a.param = renamed;
      }
    if (modulators)
      for (auto& m : *modulators)
        if (m.is_object() && m.contains("param") && m["param"].is_string() && m["param"].get<std::string>().rfind(from, 0) == 0)
          m["param"] = to + m["param"].get<std::string>().substr(from.size());
  }
}

Track parseTrack(const json& v, const std::string& path, const std::set<std::string>& assetIds) {
  Reader r(v, path);
  Track t;
  t.id = r.string("id", "", 40);
  if (!validId(t.id)) throw DocumentError(r.at("id"), "must be 1-40 letters, digits, '_' or '-'");
  t.name = r.string("name", t.id, kMaxTrackNameBytes);
  t.color = r.string("color", "", 7);
  static const std::regex color("^#[0-9A-Fa-f]{6}$");
  if (!t.color.empty() && !std::regex_match(t.color, color)) throw DocumentError(r.at("color"), "must look like #rrggbb");

  Reader ir(r.object("instrument"), r.at("instrument"));
  t.instrument.kind = ir.string("kind", "builtin", 16);
  if (t.instrument.kind == "builtin") {
    t.instrument.name = ir.string("name", "", 40);
    if (t.instrument.name.empty()) throw DocumentError(ir.at("name"), "is required");
    t.instrument.version = (int)ir.integer("version", 1, 1, 1000000);
    json params = ir.object("params");
    for (auto it = params.begin(); it != params.end(); ++it) {
      const std::string where = join(ir.at("params"), it.key());
      if (!it.value().is_number() || !std::isfinite(it.value().get<double>())) throw DocumentError(where, "must be a number");
      t.instrument.params[it.key()] = it.value().get<double>();
    }
    // Core's own instruments left Core (2026-11-16: the sound comes from plugins). A song made with them still opens:
    // the instrument is kept as it was saved (name, knobs, samples, synth), not played, until a plugin replaces it.
    const json samples = ir.array("samples", 128);
    for (size_t si = 0; si < samples.size(); ++si) {
      Reader sr(samples[si], item(ir.at("samples"), si));
      SampleRef z;
      z.file = sr.string("file", "", 1000);
      if (z.file.empty()) throw DocumentError(sr.at("file"), "is required");
      // A full path: a relative one would be read from the engine's working folder, not the song's (audit D8).
      static const std::regex full(R"(^([A-Za-z]:[\\/]|[\\/]))");
      if (!std::regex_search(z.file, full)) throw DocumentError(sr.at("file"), "must be a full path (e.g. D:\\Samples\\kick.wav)");
      z.sha256 = sr.string("sha256", "", 64);
      static const std::regex hex("^([0-9a-f]{64})?$");
      if (!std::regex_match(z.sha256, hex)) throw DocumentError(sr.at("sha256"), "must be 64 lowercase hex digits");
      z.name = sr.string("name", "", 200);
      z.rootKey = (int)sr.integer("rootKey", 60, 0, 127);
      z.loKey = (int)sr.integer("loKey", 0, 0, 127);
      z.hiKey = (int)sr.integer("hiKey", 127, 0, 127);
      if (z.loKey > z.hiKey) throw DocumentError(sr.at("hiKey"), "must not be below loKey");
      z.loopStart = sr.number("loopStart", 0, 0, 3600);
      z.loopEnd = sr.number("loopEnd", 0, 0, 3600);
      if (z.loopEnd > 0 && z.loopEnd < z.loopStart + kMinSampleLoopSeconds) throw DocumentError(sr.at("loopEnd"), "must be at least 2 ms after loopStart");
      if (z.loopEnd == 0 && z.loopStart > 0) throw DocumentError(sr.at("loopEnd"), "is needed with loopStart (0: no loop)");
      z.extra = sr.rest();
      t.instrument.samples.push_back(std::move(z));
    }
    t.instrument.synth = ir.take("synth");
  } else if (t.instrument.kind == "plugin") {
    // A plugin plays the track (Diogo 2026-11-16: Core's sound comes from plugins): {format, id, name, vendor, file, state}.
    t.instrument.plugin = ir.object("plugin");
    const json& p = t.instrument.plugin;
    const std::string format = p.value("format", std::string("vst3"));
    if (format != "vst3" && format != "vst") throw DocumentError(join(ir.at("plugin"), "format"), "must be vst3 or vst");
    if (!p.contains("id") || !p["id"].is_string() || p["id"].get<std::string>().empty()) throw DocumentError(join(ir.at("plugin"), "id"), "is required");
    if (!p.contains("file") || !p["file"].is_string() || p["file"].get<std::string>().empty()) throw DocumentError(join(ir.at("plugin"), "file"), "is required");
    t.instrument.plugin.erase("missing");
  } else if (t.instrument.kind != "audio" && t.instrument.kind != "bus" && t.instrument.kind != "none") {
    throw DocumentError(ir.at("kind"), "must be plugin, none, audio, bus or builtin");
  }
  {
    const bool fromVst = t.instrument.extra.is_object() && t.instrument.extra.contains("fromVst");
    t.instrument.extra = ir.rest();
    if (fromVst) t.instrument.extra["fromVst"] = true;
  }

  Reader mr(r.object("mix"), r.at("mix"));
  t.mix.gainDb = mr.number("gainDb", 0, -60, 12);
  t.mix.pan = mr.number("pan", 0, -1, 1);
  t.mix.mute = mr.boolean("mute", false);
  t.mix.solo = mr.boolean("solo", false);
  t.mix.output = mr.string("output", "", 40);
  const json sends = mr.array("sends", kMaxSends);
  std::set<std::string> sendIds;
  for (size_t i = 0; i < sends.size(); ++i) {
    Reader sr(sends[i], item(mr.at("sends"), i));
    Send sd;
    sd.id = sr.string("id", "", 40);
    if (!validId(sd.id)) throw DocumentError(sr.at("id"), "must be 1-40 letters, digits, '_' or '-'");
    if (!sendIds.insert(sd.id).second) throw DocumentError(sr.at("id"), "is used twice in this track");
    sd.to = sr.string("to", "", 40);
    sd.gainDb = sr.number("gainDb", 0, -60, 12);
    sd.pre = sr.boolean("pre", false);
    sd.extra = sr.rest();
    t.mix.sends.push_back(std::move(sd));
  }
  t.mix.extra = mr.rest();

  const json patterns = r.array("patterns", kMaxPatterns);
  const bool audio = t.instrument.kind == "audio", bus = t.instrument.kind == "bus";
  if (patterns.empty() && !audio && !bus) throw DocumentError(r.at("patterns"), "needs at least one pattern");
  if (!patterns.empty() && audio) throw DocumentError(r.at("patterns"), "an audio track has audio clips, not patterns");
  if (!patterns.empty() && bus) throw DocumentError(r.at("patterns"), "a bus plays what is routed to it, not patterns");
  for (size_t pi = 0; pi < patterns.size(); ++pi) {
    Reader pr(patterns[pi], item(r.at("patterns"), pi));
    Pattern p;
    p.id = pr.string("id", "", 40);
    if (!validId(p.id)) throw DocumentError(pr.at("id"), "must be 1-40 letters, digits, '_' or '-'");
    if (t.findPattern(p.id)) throw DocumentError(pr.at("id"), "is used twice in this track");
    p.name = pr.string("name", p.id, kMaxPatternNameBytes);
    p.lengthTicks = pr.integer("lengthTicks", 4 * kPpq, kPpq / 8, kMaxPatternTicks);
    const json notes = pr.array("notes", kMaxNotes);
    std::set<std::string> noteIds;
    for (size_t i = 0; i < notes.size(); ++i) {
      Note n = parseNote(notes[i], item(pr.at("notes"), i), p.lengthTicks);
      if (!noteIds.insert(n.id).second) throw DocumentError(join(item(pr.at("notes"), i), "id"), "is used twice in this pattern");
      p.notes.push_back(std::move(n));
    }
    p.extra = pr.rest();
    t.patterns.push_back(std::move(p));
  }
  const json clips = r.array("clips", kMaxClips);
  std::set<std::string> clipIds;
  for (size_t ci = 0; ci < clips.size(); ++ci) {
    Reader cr(clips[ci], item(r.at("clips"), ci));
    Clip c;
    c.id = cr.string("id", "", 40);
    if (!validId(c.id)) throw DocumentError(cr.at("id"), "must be 1-40 letters, digits, '_' or '-'");
    if (!clipIds.insert(c.id).second) throw DocumentError(cr.at("id"), "is used twice in this track");
    c.patternId = cr.string("patternId", "", 40);
    if (!t.findPattern(c.patternId)) throw DocumentError(cr.at("patternId"), "no pattern '" + c.patternId + "' in this track");
    c.startTick = cr.integer("startTick", 0, 0, kMaxSongTicks - 1);
    c.lengthTicks = cr.integer("lengthTicks", 4 * 4 * kPpq, 1, kMaxSongTicks);
    c.offsetTicks = cr.integer("offsetTicks", 0, 0, kMaxPatternTicks - 1);
    c.extra = cr.rest();
    t.clips.push_back(std::move(c));
  }
  const json audioClips = r.array("audioClips", 1024);
  if (!audioClips.empty() && !audio) throw DocumentError(r.at("audioClips"), "only an audio track has audio clips");
  std::set<std::string> audioIds;
  for (size_t i = 0; i < audioClips.size(); ++i) {
    Reader ar(audioClips[i], item(r.at("audioClips"), i));
    AudioClip a;
    a.id = ar.string("id", "", 40);
    if (!validId(a.id)) throw DocumentError(ar.at("id"), "must be 1-40 letters, digits, '_' or '-'");
    if (!audioIds.insert(a.id).second) throw DocumentError(ar.at("id"), "is used twice in this track");
    a.assetId = ar.string("assetId", "", 40);
    if (!assetIds.count(a.assetId)) throw DocumentError(ar.at("assetId"), "no asset '" + a.assetId + "'");
    a.startTick = ar.integer("startTick", 0, 0, kMaxSongTicks - 1);
    a.offsetSeconds = ar.number("offsetSeconds", 0, 0, 24 * 3600);
    a.durationSeconds = ar.number("durationSeconds", 1, 0.001, 24 * 3600);
    a.gainDb = ar.number("gainDb", 0, -60, 24);
    a.fadeInSeconds = ar.number("fadeInSeconds", 0, 0, 24 * 3600);
    a.fadeOutSeconds = ar.number("fadeOutSeconds", 0, 0, 24 * 3600);
    a.mute = ar.boolean("mute", false);
    a.pitchSemitones = ar.number("pitchSemitones", 0, -24, 24);
    a.freezeSeconds = ar.number("freezeSeconds", 0, 0, 10);
    a.extra = ar.rest();
    t.audioClips.push_back(std::move(a));
  }
  t.automation = readAutomation(r);
  t.effects = r.array("effects", kMaxEffects);
  for (size_t i = 0; i < t.effects.size(); ++i)
    if (!t.effects[i].is_object()) throw DocumentError(item(r.at("effects"), i), "must be an object");
  t.modulators = r.array("modulators", kMaxModulators);
  std::set<std::string> modIds;
  for (size_t i = 0; i < t.modulators.size(); ++i) {
    const json& m = t.modulators[i];
    const std::string where = item(r.at("modulators"), i);
    if (!m.is_object()) throw DocumentError(where, "must be an object");
    if (!m.contains("id") || !m["id"].is_string() || !validId(m["id"].get<std::string>())) throw DocumentError(where + ".id", "must be 1-40 letters, digits, '_' or '-'");
    if (!modIds.insert(m["id"].get<std::string>()).second) throw DocumentError(where + ".id", "is used twice in this track");
    if (!m.contains("param") || !m["param"].is_string()) throw DocumentError(where + ".param", "is required");
  }
  cleanEffectIds(t.effects, t.automation, &t.modulators);
  t.extra = r.rest();
  return t;
}

json noteJson(const Note& n) {
  json out = {{"id", n.id}, {"tick", n.tick}, {"length", n.length}, {"pitch", n.pitch}, {"velocity", n.velocity}};
  if (n.hasRoll) out["roll"] = withExtra({{"count", n.roll.count}, {"velocityFrom", n.roll.velocityFrom}, {"velocityTo", n.roll.velocityTo}}, n.roll.extra);
  return withExtra(out, n.extra);
}

json mixJson(const Mix& m) {
  json out = {{"gainDb", m.gainDb}, {"pan", m.pan}, {"mute", m.mute}, {"solo", m.solo}};
  if (!m.output.empty()) out["output"] = m.output;  // older documents stay byte-identical
  if (!m.sends.empty()) {
    json sends = json::array();
    for (const auto& sd : m.sends) sends.push_back(withExtra({{"id", sd.id}, {"to", sd.to}, {"gainDb", sd.gainDb}, {"pre", sd.pre}}, sd.extra));
    out["sends"] = sends;
  }
  return out;
}

json trackJson(const Track& t) {
  json instrument = {{"kind", t.instrument.kind}};
  if (t.instrument.kind == "plugin") instrument["plugin"] = t.instrument.plugin;
  else if (t.instrument.kind == "builtin") { instrument["name"] = t.instrument.name; instrument["version"] = t.instrument.version; instrument["params"] = t.instrument.params; }
  if (!t.instrument.synth.is_null()) instrument["synth"] = t.instrument.synth;
  if (!t.instrument.samples.empty()) {
    json samples = json::array();
    for (const auto& z : t.instrument.samples)
    {
      json zone = {{"file", z.file}, {"sha256", z.sha256}, {"name", z.name}, {"rootKey", z.rootKey}, {"loKey", z.loKey}, {"hiKey", z.hiKey}};
      if (z.loops()) { zone["loopStart"] = z.loopStart; zone["loopEnd"] = z.loopEnd; }  // saved only when set
      samples.push_back(withExtra(zone, z.extra));
    }
    instrument["samples"] = samples;
  }
  json patterns = json::array();
  for (const auto& p : t.patterns) {
    json notes = json::array();
    for (const auto& n : p.notes) notes.push_back(noteJson(n));
    patterns.push_back(withExtra({{"id", p.id}, {"name", p.name}, {"lengthTicks", p.lengthTicks}, {"notes", notes}}, p.extra));
  }
  json clips = json::array();
  for (const auto& c : t.clips) {
    json clip = {{"id", c.id}, {"patternId", c.patternId}, {"startTick", c.startTick}, {"lengthTicks", c.lengthTicks}};
    if (c.offsetTicks) clip["offsetTicks"] = c.offsetTicks;
    clips.push_back(withExtra(clip, c.extra));
  }
  const json automation = automationJson(t.automation);
  json audioClips = json::array();
  for (const auto& a : t.audioClips) {
    json clip = {{"id", a.id}, {"assetId", a.assetId}, {"startTick", a.startTick}, {"offsetSeconds", a.offsetSeconds},
                 {"durationSeconds", a.durationSeconds}, {"gainDb", a.gainDb}, {"fadeInSeconds", a.fadeInSeconds},
                 {"fadeOutSeconds", a.fadeOutSeconds}, {"mute", a.mute}};
    if (a.pitchSemitones != 0) clip["pitchSemitones"] = a.pitchSemitones;  // only when on: old files stay the same
    if (a.freezeSeconds > 0) clip["freezeSeconds"] = a.freezeSeconds;
    audioClips.push_back(withExtra(clip, a.extra));
  }
  json out = {{"id", t.id}, {"name", t.name},
              {"instrument", withExtra(instrument, t.instrument.extra)},
              {"mix", withExtra(mixJson(t.mix), t.mix.extra)},
              {"patterns", patterns}, {"clips", clips}, {"automation", automation}, {"effects", t.effects}};
  if (!t.color.empty()) out["color"] = t.color;
  if (t.instrument.kind == "audio") out["audioClips"] = audioClips;
  if (!t.modulators.empty()) out["modulators"] = t.modulators;  // older documents stay byte-identical
  return withExtra(out, t.extra);
}

} // namespace

const Pattern* Track::findPattern(const std::string& id) const {
  for (const auto& p : patterns) if (p.id == id) return &p;
  return nullptr;
}
Pattern* Track::findPattern(const std::string& id) {
  for (auto& p : patterns) if (p.id == id) return &p;
  return nullptr;
}
int Track::clipsUsing(const std::string& patternId) const {
  int n = 0;
  for (const auto& c : clips) n += c.patternId == patternId;
  return n;
}

// Schema 1 -> 2: each track's single pattern becomes pattern "p1", placed by one clip from the song
// start for the track's length (or the whole song), which is how schema 1 played. No sections yet.
static json migrateSchema1(json d) {
  const int64_t song = d.value("lengthTicks", (int64_t)16 * 4 * kPpq);
  if (d.contains("tracks") && d["tracks"].is_array())
    for (auto& t : d["tracks"]) {
      if (!t.is_object()) continue;
      if (t.contains("instrument") && t["instrument"].is_object() && t["instrument"].value("kind", "") == "bus") {
        t.erase("pattern"); t.erase("lengthTicks");  // a bus (imported from the engine's format) plays no pattern
        t["patterns"] = json::array(); t["clips"] = json::array();
        continue;
      }
      json p = t.contains("pattern") && t["pattern"].is_object() ? t["pattern"] : json::object();
      p["id"] = "p1";
      p["name"] = "1";
      const json len = t.value("lengthTicks", json(0));
      const int64_t trackLength = len.is_number() ? len.get<int64_t>() : 0;
      t["patterns"] = json::array({p});
      t["clips"] = json::array({json{{"id", "c1"}, {"patternId", "p1"}, {"startTick", 0},
                                     {"lengthTicks", trackLength > 0 ? std::min(trackLength, song) : song}}});
      t.erase("pattern");
      t.erase("lengthTicks");
    }
  d["schemaVersion"] = 2;
  if (!d.contains("sections")) d["sections"] = json::array();
  return d;
}

const Asset* Document::findAsset(const std::string& id) const {
  for (const auto& a : assets) if (a.id == id) return &a;
  return nullptr;
}

const Track* Document::findTrack(const std::string& id) const {
  for (const auto& t : tracks) if (t.id == id) return &t;
  return nullptr;
}

// A song saved between 2026-11-08 and 2026-11-16 turned its VST tracks into W-Core and marked them: the mark goes
// (the plugin is not known any more; the track keeps its notes). Plugin effects and their curves stay: Core plays
// plugins again (Diogo 2026-11-16).
// "master" names the master in plugin keys ("fx:master:<fx>") and routing: a track with that id (an import of
// master.wav made one, audit B02) would share the master's plugin states. It is renamed, with what points at it.
static void renameMasterTrack(Document& d) {
  Track* bad = nullptr;
  std::set<std::string> ids;
  for (auto& t : d.tracks) { ids.insert(t.id); if (t.id == "master") bad = &t; }
  if (!bad) return;
  std::string id = "master-track";
  for (int n = 2; ids.count(id); ++n) id = "master-track-" + std::to_string(n);
  bad->id = id;
  for (auto& t : d.tracks) {
    if (t.mix.output == "master") t.mix.output = id;
    for (auto& sd : t.mix.sends) if (sd.to == "master") sd.to = id;
    for (auto& fx : t.effects)
      if (fx.is_object() && fx.contains("sidechain") && fx["sidechain"] == "master") fx["sidechain"] = id;
  }
}

static void dropVst(Document& d) {
  for (auto& t : d.tracks)
    if (t.instrument.extra.is_object()) t.instrument.extra.erase("fromVst");
}

Document parseDocument(const json& input, int* migratedFrom) {
  if (migratedFrom) *migratedFrom = 0;
  json value = input;
  if (value.is_object() && value.value("format", "") == kDocumentFormat && value.value("schemaVersion", json()) == 1) {
    value = migrateSchema1(value);
    if (migratedFrom) *migratedFrom = 1;
  }
  Reader r(value, "");
  if (r.string("format", "", 64) != kDocumentFormat) throw DocumentError("format", std::string("is not ") + kDocumentFormat);
  json version = r.take("schemaVersion");
  if (!version.is_number_integer()) throw DocumentError("schemaVersion", "must be a whole number");
  // The range is checked on the number as written: a value past int (4294967298) never wraps into a valid one.
  const double written = version.get<double>();
  if (written > kSchemaVersion) throw FutureVersionError(written > (double)INT_MAX ? INT_MAX : (int)written);
  if (written < 2) throw DocumentError("schemaVersion", "must be 1, 2 or 3");
  const int schema = (int)written;

  Document d;
  d.name = r.string("name", "", 200);
  Reader tr(r.object("transport"), "transport");
  d.bpm = tr.number("bpm", 145, kMinBpm, kMaxBpm);
  if (tr.integer("ppq", kPpq, 1, 1000000) != kPpq)  // one message, not "between 960 and 960" (deep hunt §4)
    throw DocumentError("transport.ppq", "must be " + std::to_string(kPpq) + ": Core songs use " + std::to_string(kPpq) + " ticks per beat");
  json meter = tr.take("timeSignature");
  if (!meter.is_null() && meter != json::array({4, 4})) throw DocumentError("transport.timeSignature", "only 4/4 is supported");
  json loop = tr.take("loop");
  if (!loop.is_null()) {
    Reader lr(loop, "transport.loop");
    d.loop.enabled = lr.boolean("enabled", false);
    d.loop.startTick = lr.integer("startTick", 0, 0, kMaxSongTicks - 1);
    // "Remove loop" writes {enabled false, 0, 0} and deleting bars can fold a loop to nothing: an empty loop that is off
    // is a song without a loop and must save and open (sweep 2026-11-14 D1: Save and autosave refused it).
    d.loop.endTick = lr.integer("endTick", 4 * 4 * kPpq, 0, kMaxSongTicks);
    if (d.loop.enabled && d.loop.endTick <= d.loop.startTick) throw DocumentError("transport.loop.endTick", "must be after startTick");
    d.loop.extra = lr.rest();
  }
  const json tempo = tr.array("tempoMap", kMaxTempoPoints);
  d.transportExtra = tr.rest();
  d.lengthTicks = r.integer("lengthTicks", 16 * 4 * kPpq, kPpq, kMaxSongTicks);
  for (size_t i = 0; i < tempo.size(); ++i) {
    const std::string where = "transport.tempoMap[" + std::to_string(i) + "]";
    Reader pr(tempo[i], where);
    TempoPoint t;
    t.tick = pr.integer("tick", 0, 1, d.lengthTicks - 1);
    t.bpm = pr.number("bpm", 145, kMinBpm, kMaxBpm);
    t.ramp = pr.boolean("ramp", false);
    if (!d.tempoMap.empty() && t.tick <= d.tempoMap.back().tick) throw DocumentError(where + ".tick", "must come after the previous tempo change");
    t.extra = pr.rest();
    d.tempoMap.push_back(t);
  }
  Reader mr(r.object("master"), "master");
  d.masterGainDb = mr.number("gainDb", 0, -60, 12);
  d.masterEffects = mr.array("effects", kMaxEffects);
  d.masterAutomation = readAutomation(mr, true);
  for (size_t i = 0; i < d.masterEffects.size(); ++i)
    if (!d.masterEffects[i].is_object()) throw DocumentError(item(mr.at("effects"), i), "must be an object");
  cleanEffectIds(d.masterEffects, d.masterAutomation, nullptr);
  d.masterExtra = mr.rest();
  // Assets first: audio clips refer to them.
  const json assets = r.array("assets", 4096);
  std::set<std::string> assetIds;
  for (size_t i = 0; i < assets.size(); ++i) {
    Reader ar(assets[i], item("assets", i));
    Asset a;
    a.id = ar.string("id", "", 40);
    if (!validId(a.id)) throw DocumentError(ar.at("id"), "must be 1-40 letters, digits, '_' or '-'");
    if (!assetIds.insert(a.id).second) throw DocumentError(ar.at("id"), "is used twice");
    a.name = ar.string("name", a.id, 200);
    a.path = ar.string("path", "", 400);
    // Inside the project folder: relative, no ".." part, no drive or root ("take..1.wav" is a valid name).
    if (a.path.empty() || a.path[0] == '/' || a.path[0] == '\\' || a.path.find(':') != std::string::npos ||
        hasDotDotPart(a.path))
      throw DocumentError(ar.at("path"), "must be a path inside the project folder");
    a.sha256 = ar.string("sha256", "", 64);
    a.sampleRate = ar.number("sampleRate", 48000, 8000, 384000);
    a.channels = (int)ar.integer("channels", 2, 1, 2);
    a.frames = ar.integer("frames", 0, 0, (int64_t)384000 * 24 * 3600);
    a.madeFrom = ar.string("madeFrom", "", 64);
    a.made = ar.string("made", "", 80);
    a.extra = ar.rest();
    d.assets.push_back(std::move(a));
  }
  const json tracks = r.array("tracks", kMaxTracks);
  std::set<std::string> ids;
  for (size_t i = 0; i < tracks.size(); ++i) {
    Track t = parseTrack(tracks[i], item("tracks", i), assetIds);
    if (!ids.insert(t.id).second) throw DocumentError(item("tracks", i) + ".id", "is used by another track");
    d.tracks.push_back(std::move(t));
  }
  renameMasterTrack(d);
  checkRouting(d);
  const json sections = r.array("sections", kMaxSections);
  std::set<std::string> sectionIds;
  for (size_t i = 0; i < sections.size(); ++i) {
    Reader sr(sections[i], item("sections", i));
    Section s;
    s.id = sr.string("id", "", 40);
    if (!validId(s.id)) throw DocumentError(sr.at("id"), "must be 1-40 letters, digits, '_' or '-'");
    if (!sectionIds.insert(s.id).second) throw DocumentError(sr.at("id"), "is used twice");
    s.name = sr.string("name", s.id, kMaxSectionNameBytes);
    s.startTick = sr.integer("startTick", 0, 0, kMaxSongTicks - 1);
    s.lengthTicks = sr.integer("lengthTicks", 8 * 4 * kPpq, 1, kMaxSongTicks);
    s.color = sr.string("color", "", 7);
    static const std::regex sectionColorPattern("^#[0-9A-Fa-f]{6}$");
    if (!s.color.empty() && !std::regex_match(s.color, sectionColorPattern)) throw DocumentError(sr.at("color"), "must look like #rrggbb");
    if (!d.sections.empty() && s.startTick < d.sections.back().startTick + d.sections.back().lengthTicks)
      throw DocumentError(sr.at("startTick"), "sections must be in order and must not overlap");
    s.extra = sr.rest();
    d.sections.push_back(std::move(s));
  }
  d.ui = r.object("ui");
  d.extra = r.rest();
  dropVst(d);
  return d;
}

json toJson(const Document& d) {
  json tracks = json::array();
  for (const auto& t : d.tracks) tracks.push_back(trackJson(t));
  // Schema 3 only when the song changes tempo: everything else stays readable by the apps before the map.
  json out = {{"format", kDocumentFormat}, {"schemaVersion", d.tempoMap.empty() ? 2 : kSchemaVersion}, {"name", d.name},
              {"transport", withExtra({{"bpm", d.bpm}, {"ppq", kPpq}, {"timeSignature", {4, 4}},
                                       {"loop", withExtra({{"enabled", d.loop.enabled}, {"startTick", d.loop.startTick}, {"endTick", d.loop.endTick}}, d.loop.extra)}},
                                      d.transportExtra)},
              {"lengthTicks", d.lengthTicks},
              {"master", withExtra({{"gainDb", d.masterGainDb}}, d.masterExtra)},
              {"tracks", tracks}, {"ui", d.ui}};
  if (!d.masterEffects.empty()) out["master"]["effects"] = d.masterEffects;  // older documents stay byte-identical
  if (!d.masterAutomation.empty()) out["master"]["automation"] = automationJson(d.masterAutomation);
  if (!d.tempoMap.empty()) {
    json map = json::array();
    for (const auto& t : d.tempoMap) map.push_back(withExtra(tempoPointJson(t), t.extra));
    out["transport"]["tempoMap"] = map;
  }
  json sections = json::array();
  for (const auto& s : d.sections) {
    json x = {{"id", s.id}, {"name", s.name}, {"startTick", s.startTick}, {"lengthTicks", s.lengthTicks}};
    if (!s.color.empty()) x["color"] = s.color;  // only when chosen: older documents stay byte-identical
    sections.push_back(withExtra(x, s.extra));
  }
  out["sections"] = sections;
  json assets = json::array();
  for (const auto& a : d.assets) {
    json x = {{"id", a.id}, {"name", a.name}, {"path", a.path}, {"sha256", a.sha256}, {"sampleRate", a.sampleRate},
              {"channels", a.channels}, {"frames", a.frames}};
    if (!a.madeFrom.empty()) x["madeFrom"] = a.madeFrom;  // only when set: older documents stay byte-identical
    if (!a.made.empty()) x["made"] = a.made;
    assets.push_back(withExtra(x, a.extra));
  }
  if (!assets.empty()) out["assets"] = assets;  // older documents stay byte-identical
  return withExtra(out, d.extra);
}

// What the engine plays (Diogo 2026-11-16: the sound comes from plugins): the plugin effects of a chain, and the curves of
// the mixer and of plugin parameters. Core's own effects, instruments and their curves stay in the song, unplayed.
namespace {
json pluginChain(const json& chain) {
  json out = json::array();
  for (const auto& fx : chain) if (fx.is_object() && fx.value("type", std::string()) == "plugin") out.push_back(fx);
  return out;
}
std::vector<Automation> playedCurves(const std::vector<Automation>& list, const json& chain) {
  std::set<std::string> plugins;
  for (const auto& fx : chain)
    if (fx.is_object() && fx.value("type", std::string()) == "plugin" && fx.contains("id") && fx["id"].is_string()) plugins.insert(fx["id"].get<std::string>());
  std::vector<Automation> out;
  for (const auto& a : list) {
    bool played = a.param == "gainDb" || a.param == "pan" || a.param.rfind("vst:", 0) == 0;
    if (!played && a.param.rfind("fx:", 0) == 0) {
      const size_t colon = a.param.find(':', 3);
      played = colon != std::string::npos && plugins.count(a.param.substr(3, colon - 3)) && a.param.compare(colon + 1, 4, "vst:") == 0;
    }
    if (played) out.push_back(a);
  }
  return out;
}
}  // namespace

json toProjection(const Document& d, const std::filesystem::path& assetRoot) {
  json tracks = json::array();
  for (const auto& t : d.tracks) {
    json clips = json::array();
    for (const auto& c : t.clips) {
      const Pattern* p = t.findPattern(c.patternId);
      if (!p || c.startTick >= d.lengthTicks) continue;  // after the end of the song: nothing to play
      json notes = json::array();
      for (const auto& n : p->notes) {
        json note = {{"tick", n.tick}, {"length", n.length}, {"pitch", n.pitch}, {"velocity", n.velocity}};
        if (n.hasRoll) note["roll"] = {{"count", n.roll.count}, {"velocityFrom", n.roll.velocityFrom}, {"velocityTo", n.roll.velocityTo}};
        notes.push_back(note);
      }
      json clip = {{"startTick", c.startTick}, {"lengthTicks", c.lengthTicks}, {"patternTicks", p->lengthTicks}, {"notes", notes}};
      if (c.offsetTicks % p->lengthTicks) clip["offsetTicks"] = c.offsetTicks % p->lengthTicks;  // the pattern may have been shortened
      clips.push_back(clip);
    }
    const json envelopes = envelopesJson(playedCurves(t.automation, t.effects));
    json track = {{"id", t.id}, {"gainDb", t.mix.gainDb}, {"pan", t.mix.pan}, {"mute", t.mix.mute}, {"solo", t.mix.solo},
                  {"clips", clips}, {"envelopes", envelopes}, {"effects", pluginChain(t.effects)}};
    if (t.instrument.kind == "audio") {
      track["instrument"] = "audio";
      track.erase("clips");
      json audioClips = json::array();
      for (const auto& a : t.audioClips) {
        const Asset* asset = d.findAsset(a.assetId);
        if (!asset || assetRoot.empty() || a.mute) continue;
        const std::u8string full = (assetRoot / std::filesystem::path(std::u8string(asset->path.begin(), asset->path.end()))).u8string();
        audioClips.push_back({{"startTick", a.startTick}, {"path", std::string(full.begin(), full.end())}, {"offsetSeconds", a.offsetSeconds},
                              {"durationSeconds", a.durationSeconds}, {"gainDb", a.gainDb}, {"fadeInSeconds", a.fadeInSeconds},
                              {"fadeOutSeconds", a.fadeOutSeconds}});
        if (a.pitchSemitones != 0) audioClips.back()["pitchSemitones"] = a.pitchSemitones;
        if (a.freezeSeconds > 0) audioClips.back()["freezeSeconds"] = a.freezeSeconds;
      }
      track["audioClips"] = audioClips;
    } else if (t.instrument.kind == "bus") { track["instrument"] = "bus"; track.erase("clips"); }
    else if (t.instrument.kind == "plugin") { track["instrument"] = "plugin"; track["plugin"] = t.instrument.plugin; }
    else track["instrument"] = "none";  // an old Core instrument, or no instrument yet: its notes wait for a plugin
    if (!t.mix.output.empty()) track["output"] = t.mix.output;
    if (!t.mix.sends.empty()) {
      json sends = json::array();
      for (const auto& sd : t.mix.sends) sends.push_back({{"id", sd.id}, {"to", sd.to}, {"gainDb", sd.gainDb}, {"pre", sd.pre}});
      track["sends"] = sends;
    }
    tracks.push_back(track);
  }
  json project = {{"format", "core-project"}, {"version", 2}, {"bpm", d.bpm}, {"lengthTicks", d.lengthTicks},
                  {"masterGainDb", d.masterGainDb}, {"tracks", tracks}};
  if (const json chain = pluginChain(d.masterEffects); !chain.empty()) project["masterEffects"] = chain;
  if (const auto curves = playedCurves(d.masterAutomation, d.masterEffects); !curves.empty()) project["masterEnvelopes"] = envelopesJson(curves);
  if (!d.tempoMap.empty()) {
    json map = json::array();
    for (const auto& t : d.tempoMap) map.push_back(tempoPointJson(t));
    project["tempoMap"] = map;
  }
  return project;
}

void checkRouting(const Document& d) {
  std::map<std::string, const Track*> byId;
  for (const auto& t : d.tracks) byId[t.id] = &t;
  auto target = [&](const Track& t, const std::string& to, const std::string& where) {
    auto it = byId.find(to);
    if (it == byId.end()) throw DocumentError(where, "goes to '" + to + "', which is not a track");
    if (it->second->instrument.kind != "bus") throw DocumentError(where, "goes to '" + to + "', which is not a bus");
    if (to == t.id) throw DocumentError(where, "goes to its own track");
  };
  for (size_t i = 0; i < d.tracks.size(); ++i) {
    const Track& t = d.tracks[i];
    if (!t.mix.output.empty()) target(t, t.mix.output, item("tracks", i) + ".mix.output");
    for (size_t k = 0; k < t.mix.sends.size(); ++k) target(t, t.mix.sends[k].to, item(item("tracks", i) + ".mix.sends", k) + ".to");
  }
  // M7c sidechains: a compressor keyed by another track that is not a bus and not its own.
  std::map<std::string, std::vector<const Track*>> keyed;  // source -> the tracks it keys
  for (size_t i = 0; i < d.tracks.size(); ++i) {
    const Track& t = d.tracks[i];
    for (size_t k = 0; k < t.effects.size(); ++k) {
      const json& fx = t.effects[k];
      if (!fx.is_object() || !fx.contains("sidechain")) continue;
      const std::string where = item(item("tracks", i) + ".effects", k) + ".sidechain";
      if (!fx["sidechain"].is_string()) throw DocumentError(where, "must be a track id");
      if (fx.value("name", "") != "compressor") throw DocumentError(where, "only a compressor takes a sidechain");
      const std::string from = fx["sidechain"].get<std::string>();
      auto it = byId.find(from);
      if (it == byId.end()) throw DocumentError(where, "comes from '" + from + "', which is not a track");
      if (from == t.id) throw DocumentError(where, "comes from its own track");
      if (it->second->instrument.kind == "bus") throw DocumentError(where, "comes from '" + from + "', a bus (use a track)");
      keyed[from].push_back(&t);
    }
  }
  for (size_t k = 0; k < d.masterEffects.size(); ++k)
    if (d.masterEffects[k].is_object() && d.masterEffects[k].contains("sidechain")) throw DocumentError(item("master.effects", k) + ".sidechain", "the master takes no sidechain");
  std::map<std::string, int> mark;  // 1 = on the path, 2 = done
  std::function<void(const Track&)> visit = [&](const Track& t) {
    int& m = mark[t.id];
    if (m == 2) return;
    if (m == 1) throw DocumentError("tracks", "the routing loops back into '" + t.id + "'");
    m = 1;
    if (!t.mix.output.empty()) visit(*byId[t.mix.output]);
    for (const auto& sd : t.mix.sends) visit(*byId[sd.to]);
    if (auto k = keyed.find(t.id); k != keyed.end()) for (const Track* x : k->second) visit(*x);
    mark[t.id] = 2;
  };
  for (const auto& t : d.tracks) visit(t);
}

bool sameContent(const Document& a, const Document& b) { return toJson(a) == toJson(b); }
bool sameSound(const Document& a, const Document& b) { return toProjection(a) == toProjection(b); }

namespace {
// v2 envelopes as document automation: in time order, one per parameter (a repeated one is dropped).
json importEnvelopes(const json& envelopes, const std::string& where, ImportReport& rep) {
  json automation = json::array();
  std::set<std::string> seen;
  for (const auto& e : envelopes) {
    json points = e.value("points", json::array());
    std::stable_sort(points.begin(), points.end(), [](const json& a, const json& b) { return a.value("tick", 0) < b.value("tick", 0); });
    const std::string param = e.value("param", "");
    if (points.empty()) continue;
    if (!seen.insert(param).second) { rep.converted.push_back(where + ": a second envelope for '" + param + "' dropped"); continue; }
    automation.push_back({{"param", param}, {"points", points}});
  }
  return automation;
}
}  // namespace

Document importProjectV2(const json& project, ImportReport* report) {
  ImportReport local;
  ImportReport& rep = report ? *report : local;
  if (!project.is_object() || project.value("format", "") != "core-project") throw DocumentError("format", "is not core-project");
  const json version = project.value("version", json());
  if (version == 1) throw DocumentError("version", "core-project v1 (JS editor) files open in a later version");
  if (version != 2) throw DocumentError("version", "must be 2");
  // Same defaults and clamping as the engine (engine/src/core/project.cpp), so the import sounds the same.
  auto clamp = [&rep](double x, double lo, double hi, const std::string& where) {
    const double c = std::min(hi, std::max(lo, x));
    if (c != x) rep.converted.push_back(where + ": " + json(x).dump() + " limited to " + json(c).dump());
    return c;
  };
  auto num = [](const json& v, const char* key, double fallback) {
    return v.contains(key) && v[key].is_number() ? v[key].get<double>() : fallback;
  };
  // Built as schema 1 (one pattern per track), then migrated like any schema 1 file.
  json doc = {{"format", kDocumentFormat}, {"schemaVersion", 1}};
  Reader r(project, "");
  r.take("format"); r.take("version"); r.take("ppq");
  doc["name"] = r.string("name", "", 200);
  doc["transport"] = {{"bpm", project.value("bpm", 145.0)}};
  r.take("bpm");
  doc["lengthTicks"] = project.value("lengthTicks", (int64_t)64 * kPpq);
  r.take("lengthTicks");
  doc["master"] = {{"gainDb", clamp(num(project, "masterGainDb", 0), -60, 12, "masterGainDb")}};
  r.take("masterGainDb");
  const json masterEffects = r.array("masterEffects", kMaxEffects);
  if (!masterEffects.empty()) doc["master"]["effects"] = masterEffects;
  const json masterAutomation = importEnvelopes(r.array("masterEnvelopes", 256), "masterEnvelopes", rep);
  if (!masterAutomation.empty()) doc["master"]["automation"] = masterAutomation;
  json tracks = json::array();
  const json sourceTracks = r.array("tracks", kMaxTracks);
  const json tempo = r.array("tempoMap", kMaxTempoPoints);
  size_t created = 0;
  // Notes as the engine reads them (tick, length, pitch, velocity, roll), with ids n1, n2...
  auto importNotes = [&](const json& sourceNotes, const std::string& where) {
    json notes = json::array();
    for (size_t j = 0; j < sourceNotes.size(); ++j) {
      const std::string nwhere = item(where + ".notes", j);
      Reader nr(sourceNotes[j], nwhere);
      const json& sn = sourceNotes[j];
      // Whole numbers go on as written (no cast here): the document's reader checks their range.
      auto as = [&](const json& o, const char* k, json fallback) { return o.contains(k) && !o[k].is_null() ? o[k] : fallback; };
      json n = {{"id", "n" + std::to_string(j + 1)}, {"tick", as(sn, "tick", (int64_t)0)}, {"length", as(sn, "length", (int64_t)kPpq / 4)},
                {"pitch", as(sn, "pitch", 36)}, {"velocity", clamp(num(sn, "velocity", 0.8), 0, 1, nwhere + ".velocity")}};
      for (const char* k : {"tick", "length", "pitch", "velocity"}) nr.take(k);
      json roll = nr.take("roll");
      if (roll.is_object()) {
        const double v = n["velocity"].get<double>();
        n["roll"] = {{"count", as(roll, "count", 0)},
                     {"velocityFrom", clamp(num(roll, "velocityFrom", v), 0, 1, nwhere + ".roll.velocityFrom")},
                     {"velocityTo", clamp(num(roll, "velocityTo", v), 0, 1, nwhere + ".roll.velocityTo")}};
      }
      json rest = nr.rest();
      if (!rest.empty()) rep.kept.push_back(nwhere + ": unknown fields kept, not played");
      notes.push_back(withExtra(n, rest));
      ++created;
    }
    return notes;
  };
  // Tracks that play clips (the shape the app itself sends the engine): their patterns and clips, made
  // after the schema 1 migration below. One pattern per distinct (length, notes); clips keep their place.
  std::vector<std::pair<json, json>> clipTracks(sourceTracks.size());  // patterns, clips
  std::vector<bool> hasClips(sourceTracks.size(), false);
  for (size_t i = 0; i < sourceTracks.size(); ++i) {
    const std::string where = item("tracks", i);
    Reader tr(sourceTracks[i], where);
    const json& src = sourceTracks[i];
    json t;
    t["id"] = tr.string("id", "", 40);
    t["name"] = tr.string("name", t["id"].get<std::string>(), 80);
    const std::string instrument = tr.string("instrument", "", 40);
    if (instrument == "plugin") {
      json plugin = tr.object("plugin");
      if (plugin.contains("missing")) { plugin.erase("missing"); rep.converted.push_back(where + ".plugin.missing: dropped (decided when the plugin loads)"); }
      t["instrument"] = {{"kind", "plugin"}, {"plugin", plugin}};
      if (tr.take("params").is_object()) rep.kept.push_back(where + ".params: ignored for a plugin track");
      tr.take("instrumentVersion");
    } else if (instrument == "bus") {
      t["instrument"] = {{"kind", "bus"}};
    } else {
      t["instrument"] = {{"kind", "builtin"}, {"name", instrument}, {"version", (int64_t)num(src, "instrumentVersion", 1)}, {"params", tr.object("params")}};
      tr.take("instrumentVersion");
      if (const json synth = tr.take("synth"); !synth.is_null()) t["instrument"]["synth"] = synth;  // W-Core's links
      // The projection puts a sampler's zones on the track, with "path"; the document keeps them on the
      // instrument, with "file". Other fields of a zone come along as they are.
      if (instrument == "sampler" || instrument == "wave" || instrument == "samplepath") {
        json zones = json::array();
        for (json z : tr.array("samples", 128)) {
          if (z.is_object() && z.contains("path") && !z.contains("file")) { z["file"] = z["path"]; z.erase("path"); }
          zones.push_back(std::move(z));
        }
        if (!zones.empty()) t["instrument"]["samples"] = zones;
      }
    }
    t["mix"] = {{"gainDb", clamp(num(src, "gainDb", 0), -60, 12, where + ".gainDb")},
                {"pan", clamp(num(src, "pan", 0), -1, 1, where + ".pan")},
                {"mute", src.value("mute", false)}, {"solo", src.value("solo", false)}};
    for (const char* k : {"gainDb", "pan", "mute", "solo"}) tr.take(k);
    if (const json output = tr.take("output"); output.is_string() && !output.get<std::string>().empty()) t["mix"]["output"] = output;
    if (const json sends = tr.take("sends"); sends.is_array() && !sends.empty()) {
      json list = json::array();
      for (size_t k = 0; k < sends.size(); ++k) {
        json sd = sends[k];
        if (sd.is_object() && !sd.contains("id")) sd["id"] = "s" + std::to_string(k + 1);
        list.push_back(sd);
      }
      t["mix"]["sends"] = list;
    }
    const json notes = importNotes(tr.array("notes", kMaxNotes), where);
    const json sourceClips = tr.array("clips", kMaxClips);
    if (!sourceClips.empty()) {
      if (!notes.empty()) throw DocumentError(where + ".clips", "a track has notes or clips, not both");
      hasClips[i] = true;
      json patterns = json::array(), clips = json::array();
      std::vector<std::pair<int64_t, json>> seen;  // (length, notes without ids) -> pattern index
      for (size_t c = 0; c < sourceClips.size(); ++c) {
        const std::string cwhere = item(where + ".clips", c);
        Reader cr(sourceClips[c], cwhere);
        const int64_t length = cr.integer("patternTicks", 4 * kPpq, kPpq / 8, 64LL * 4 * kPpq);
        const json cn = importNotes(cr.array("notes", kMaxNotes), cwhere);
        json key = cn;
        for (auto& n : key) n.erase("id");
        size_t index = 0;
        while (index < seen.size() && !(seen[index].first == length && seen[index].second == key)) ++index;
        if (index == seen.size()) {
          seen.push_back({length, key});
          patterns.push_back({{"id", "p" + std::to_string(index + 1)}, {"name", "Pattern " + std::to_string(index + 1)}, {"lengthTicks", length}, {"notes", cn}});
        }
        json clip = {{"id", "c" + std::to_string(c + 1)}, {"patternId", "p" + std::to_string(index + 1)},
                     {"startTick", cr.integer("startTick", 0, 0, 4096LL * 4 * kPpq - 1)},
                     {"lengthTicks", cr.integer("lengthTicks", 4 * 4 * kPpq, 1, 4096LL * 4 * kPpq)}};
        const int64_t offset = cr.integer("offsetTicks", 0, 0, length - 1);
        if (offset) clip["offsetTicks"] = offset;
        json rest = cr.rest();
        if (!rest.empty()) rep.kept.push_back(cwhere + ": unknown fields kept, not played");
        clips.push_back(withExtra(clip, rest));
      }
      clipTracks[i] = {patterns, clips};
    }
    t["pattern"] = {{"lengthTicks", src.value("patternTicks", (int64_t)4 * kPpq)}, {"notes", notes}};
    tr.take("patternTicks");
    t["lengthTicks"] = std::max<int64_t>(0, src.value("lengthTicks", (int64_t)0));
    tr.take("lengthTicks");
    t["automation"] = importEnvelopes(tr.array("envelopes", 256), where, rep);
    t["effects"] = tr.array("effects", kMaxEffects);
    if (const json mods = tr.take("modulators"); mods.is_array() && !mods.empty()) t["modulators"] = mods;
    json rest = tr.rest();
    if (!rest.empty()) rep.kept.push_back(where + ": unknown fields kept, not played");
    tracks.push_back(withExtra(t, rest));
  }
  if (created) rep.converted.push_back("note ids created for " + std::to_string(created) + " notes");
  doc["tracks"] = tracks;
  json rest = r.rest();
  if (!rest.empty()) rep.kept.push_back("unknown top-level fields kept, not played");
  doc = withExtra(doc, rest);
  Document out = parseDocument(doc);  // the same checks as any saved document
  if (std::find(hasClips.begin(), hasClips.end(), true) == hasClips.end() && tempo.empty()) return out;
  // Clips and the tempo map: written into the migrated (schema 2) document and read again, so they get the
  // same checks too.
  json d2 = toJson(out);
  for (size_t i = 0; i < hasClips.size(); ++i) {
    if (!hasClips[i]) continue;
    d2["tracks"][i]["patterns"] = clipTracks[i].first;
    d2["tracks"][i]["clips"] = clipTracks[i].second;
  }
  if (!tempo.empty()) {
    json map = json::array();
    for (const auto& p : tempo) {
      json point = {{"tick", p.value("tick", json())}, {"bpm", p.value("bpm", json())}};
      if (p.contains("ramp")) point["ramp"] = p["ramp"];  // checked by parseDocument
      map.push_back(point);
    }
    d2["transport"]["tempoMap"] = map;
    d2["schemaVersion"] = kSchemaVersion;
  }
  return parseDocument(d2);
}

// The colour of a section's type, from words in its name (in any case). Checked in this order, so a
// "Pre-drop" is a build-up and "Break hits" a break.
std::string sectionTypeColor(const std::string& name) {
  std::string key = name;
  for (auto& c : key) c = (char)std::tolower((unsigned char)c);
  struct Type { std::vector<const char*> words; const char* color; };
  static const Type types[] = {
      {{"intro", "start", "opening"}, "#2f6f57"},                                                     // green
      {{"outro", "ending", "end"}, "#2f6470"},                                                        // teal
      {{"break", "reduction", "deep", "bridge", "ambient", "calm", "interlude", "down"}, "#24508f"},  // blue
      {{"build", "rise", "riser", "tension", "pre-drop", "predrop", "pre drop", "development", "layers", "transform", "climb"},
       "#8a6d1e"},                                                                                    // amber
      {{"drop", "main", "peak", "climax", "return", "chorus", "hook"}, "#8e3341"},                    // red
      {{"groove", "verse", "body", "flow"}, "#5b3f8c"},                                               // purple
  };
  for (const auto& t : types)
    for (const char* w : t.words)
      if (key.find(w) != std::string::npos) return t.color;
  return "#4a4f6e";  // slate
}

std::string currentSectionSwatch(const std::string& color) {
  std::string c = color;
  for (auto& ch : c) ch = (char)std::tolower((unsigned char)ch);
  static const std::pair<const char*, const char*> old[] = {
      {"#e39d96", "#8e3341"}, {"#d9845a", "#8e3341"}, {"#e0c070", "#8a6d1e"}, {"#6fb08f", "#2f6f57"}, {"#7fa9c6", "#2f6470"},
      {"#6a9cf0", "#24508f"}, {"#a98bd0", "#5b3f8c"}, {"#c47fa8", "#5b3f8c"}, {"#8a8fc0", "#4a4f6e"}};
  for (const auto& [from, to] : old)
    if (c == from) return to;
  return color;
}

std::string cutUtf8(const std::string& s, size_t max) {
  if (s.size() <= max) return s;
  while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) --max;
  return s.substr(0, max);
}

std::string nameWithSuffix(const std::string& head, const std::string& suffix, size_t max) {
  if (head.size() + suffix.size() <= max) return head + suffix;
  if (suffix.size() >= max) {  // the suffix is the name on its own: keep its tail, whole
    std::string tail = suffix.substr(suffix.size() - max);
    while (!tail.empty() && ((unsigned char)tail[0] & 0xC0) == 0x80) tail.erase(0, 1);
    return tail;
  }
  return cutUtf8(head, max - suffix.size()) + suffix;
}

} // namespace core::doc
