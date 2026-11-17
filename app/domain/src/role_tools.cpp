#include "role_tools.hpp"

#include <algorithm>
#include <cmath>
#include "edit.hpp"
#include "follow.hpp"
#include "pattern_tools.hpp"
#include <cctype>
#include <set>

namespace core::doc {

namespace {
void checkSpan(const Document& doc, int64_t start, int64_t end) {
  if (start < 0 || end <= start || end > doc.lengthTicks) throw DocumentError("tool", "the span must be inside the song and end after it starts");
  if (end - start < kPpq / 4) throw DocumentError("tool", "the span is shorter than a 1/16");
}
// The role can be written: mapped, an effect role, and the macro has a parameter. Throws otherwise (before any change).
void checkMacro(const RoleMap& map, const std::string& role, const std::string& macro) {
  roleParamKey(map, role, "check", macro);
}
bool hasMacro(const RoleMap& map, const std::string& role, const std::string& macro) {
  const RoleBinding* b = map.find(role);
  if (!b) return false;
  auto it = b->params.find(macro);
  return it != b->params.end() && !it->second.empty();
}
std::string whyNot(const RoleMap& map, const std::string& role, const std::string& macro) {
  const RoleInfo* info = findRole(role);
  const std::string label = info ? info->label : role;
  if (!map.find(role)) return label + ": no plugin chosen (Settings > Roles)";
  const RoleMacro* m = info ? findRoleMacro(*info, macro) : nullptr;
  return label + ": no parameter for " + (m ? m->label : macro) + " (Settings > Roles)";
}
std::string percent(double v) { return std::to_string((int)std::lround(v * 100)) + "%"; }
// The chain has room for every role about to be written (audit 2026-11-17, #5). Only roles that need a NEW effect
// count: a role already on the chain, tagged with it or played by the same plugin put there by hand, rides on the
// slot it has, which is why this asks roleEffectSlot and not findRoleEffect. Roles that were not asked for are not
// counted, so the caller passes only the ones it will write. Throws the very error addPluginEffect would throw, but
// before the first write, so a full chain leaves the document exactly as it was.
void requireChainRoom(const Document& doc, const std::string& trackId, const RoleMap& map, const std::vector<std::string>& roles) {
  const size_t length = effectChain(doc, trackId).size();
  std::set<std::string> seen;
  std::set<int> taken;  // a slot an earlier role in this same call is about to tag: the next role cannot share it
  int fresh = 0;
  for (const auto& role : roles) {
    if (!seen.insert(role).second) continue;
    const int at = roleEffectSlot(doc, trackId, role, map);
    if (at < 0 || taken.count(at)) ++fresh;
    else taken.insert(at);
  }
  if ((int)length + fresh > kMaxEffectsPerChain) throw DocumentError("effects", "the chain has the maximum of 16 effects");
}
std::string bars(int64_t ticks) {
  const double b = (double)ticks / (4.0 * kPpq);
  if (std::abs(b - std::round(b)) < 1e-9) return std::to_string((int64_t)std::round(b)) + (std::round(b) == 1 ? " bar" : " bars");
  return std::to_string((int64_t)std::round(b * 4)) + " beats";
}
}  // namespace

std::string writeRoleSweep(Document& doc, const std::string& trackId, const RoleMap& map, const std::string& role,
                           const std::string& macro, int64_t start, int64_t end, double from, double to, double curve) {
  checkSpan(doc, start, end);
  checkMacro(map, role, macro);
  if (!trackId.empty()) trackById(doc, trackId);  // throws for a missing track ("" = the master)
  const RoleBinding& b = *map.find(role);
  const double rest = roleRest(b, role, macro);
  const std::string effectId = ensureRoleEffect(doc, trackId, role, map);
  const std::string key = roleParamKey(map, role, effectId, macro);
  from = std::clamp(from, 0.0, 1.0);
  to = std::clamp(to, 0.0, 1.0);
  curve = std::clamp(curve, 0.1, 10.0);
  // A point every 1/16, at most 64 over the span (straight lines between them).
  const int64_t length = end - start;
  const int64_t step = std::max<int64_t>(kPpq / 4, length / 64);
  std::vector<AutomationPoint> points;
  for (int64_t t = start; t < end; t += step) {
    const double x = (double)(t - start) / (double)length;
    points.push_back({t, from + (to - from) * std::pow(x, curve), json::object()});
  }
  if (end - 1 > points.back().tick) points.push_back({end - 1, to, json::object()});  // the top, just before the drop
  setAutomationInRange(doc, trackId, key, start, end, points, rest);
  return key;
}

RoleToolResult toolSuck(Document& doc, const std::string& trackId, const RoleMap& map, int64_t dropTick, int64_t lengthTicks,
                        double amount) {
  amount = std::clamp(amount, 0.0, 1.0);
  const int64_t start = dropTick - lengthTicks;
  checkSpan(doc, start, dropTick);
  checkMacro(map, "reverb", "mix");
  const bool withFilter = hasMacro(map, "filter", "cutoff");
  // All or nothing (audit 2026-11-17, #5): the reverb sweep used to be written before the filter's addPluginEffect
  // found the chain full, so a 16-effect chain ended up half sucked. Nothing is written until the whole tool fits.
  std::vector<std::string> needed{"reverb"};
  if (withFilter) needed.push_back("filter");
  requireChainRoom(doc, trackId, map, needed);
  Document next = doc;
  RoleToolResult r;
  const RoleBinding& reverb = *map.find("reverb");
  r.keys.push_back(writeRoleSweep(next, trackId, map, "reverb", "mix", start, dropTick, roleRest(reverb, "reverb", "mix"), 0.25 + 0.7 * amount, 2.0));
  if (hasMacro(map, "reverb", "size")) {
    const double rest = roleRest(reverb, "reverb", "size");
    r.keys.push_back(writeRoleSweep(next, trackId, map, "reverb", "size", start, dropTick, rest, std::max(rest, 0.5 + 0.5 * amount), 1.5));
  }
  r.roles.push_back("reverb");
  if (withFilter) {
    const double rest = roleRest(*map.find("filter"), "filter", "cutoff");
    r.keys.push_back(writeRoleSweep(next, trackId, map, "filter", "cutoff", start, dropTick, rest, rest * (1.0 - 0.6 * amount), 1.5));
    r.roles.push_back("filter");
  } else {
    r.skipped.push_back(whyNot(map, "filter", "cutoff"));
  }
  r.sentence = "SUCK: the reverb opens over the last " + bars(lengthTicks) + " up to " + percent(0.25 + 0.7 * amount) +
               (r.roles.size() > 1 ? ", the filter closes" : "") + ", and everything is back on the drop";
  doc = std::move(next);
  return r;
}

RoleToolResult toolPump(Document& doc, const std::string& trackId, const std::string& sourceTrackId, const RoleMap& map,
                        int64_t start, int64_t end, double depth, int64_t releaseTicks) {
  checkSpan(doc, start, end);
  if (trackId == sourceTrackId) throw DocumentError("pump", "a track cannot pump from its own notes");
  trackById(doc, trackId);
  const Track& src = trackById(doc, sourceTrackId);
  depth = std::clamp(depth, 0.0, 1.0);
  if (releaseTicks <= 0) releaseTicks = kPpq / 2;
  releaseTicks = std::clamp<int64_t>(releaseTicks, kPpq / 16, 4 * kPpq);
  RoleToolResult r;
  r.roles.push_back("sidechain");
  const RoleBinding* b = map.find("sidechain");
  if (hasMacro(map, "sidechain", "volume")) {
    const std::vector<int64_t> onsets = songOnsets(doc, sourceTrackId, start, end);
    if (onsets.empty()) throw DocumentError("pump", "'" + src.name + "' plays no notes there");
    const double rest = roleRest(*b, "sidechain", "volume"), low = rest * (1.0 - depth);
    std::vector<AutomationPoint> points;
    for (size_t i = 0; i < onsets.size(); ++i) {
      const int64_t o = onsets[i];
      const int64_t next = i + 1 < onsets.size() ? onsets[i + 1] : end;
      points.push_back({o, low, json::object()});
      // Back up over the release, fast at first (a square root), never past the next note.
      for (int k = 1; k <= 4; ++k) {
        const int64_t t = o + releaseTicks * k / 4;
        if (t >= next || t >= end) break;
        points.push_back({t, low + (rest - low) * std::sqrt(k / 4.0), json::object()});
      }
    }
    const std::string effectId = ensureRoleEffect(doc, trackId, "sidechain", map);
    const std::string key = roleParamKey(map, "sidechain", effectId, "volume");
    setAutomationInRange(doc, trackId, key, start, end, points, rest);
    r.keys.push_back(key);
    r.sentence = "PUM: the volume dips " + percent(depth) + " at every note of " + src.name + " (" + std::to_string(onsets.size()) + " notes)";
    return r;
  }
  if (hasMacro(map, "sidechain", "depth")) {
    const std::string effectId = ensureRoleEffect(doc, trackId, "sidechain", map);
    const std::string key = roleParamKey(map, "sidechain", effectId, "depth");
    setAutomationInRange(doc, trackId, key, start, end, {{start, depth, json::object()}, {end - 1, depth, json::object()}},
                         roleRest(*b, "sidechain", "depth"));
    r.keys.push_back(key);
    r.sentence = "PUM: the sidechain plugin pumps at " + percent(depth) + " depth over " + bars(end - start);
    return r;
  }
  throw DocumentError("pump", whyNot(map, "sidechain", "volume"));
}

RoleToolResult toolBuildupFx(Document& doc, const std::string& trackId, const RoleMap& map, int64_t start, int64_t dropTick,
                             const BuildupFx& fx) {
  checkSpan(doc, start, dropTick);
  trackById(doc, trackId);
  if (!fx.any()) throw DocumentError("buildup", "choose at least one effect");
  const double a = std::clamp(fx.amount, 0.0, 1.0);
  struct Move { bool on; const char *role, *macro; double from, to, curve; bool fromRest; };
  // from < 0: the macro's rest value.
  const std::vector<Move> moves = {
      {fx.filter, "filter", "cutoff", 1.0 - 0.75 * a, 1.0, 1.5, false},
      {fx.reverb, "reverb", "mix", 0, 0.5 * a, 2.0, true},
      {fx.delay, "delay", "mix", 0, 0.5 * a, 2.0, true},
      {fx.delay, "delay", "feedback", 0, 0.35 + 0.4 * a, 1.0, true},
      {fx.drive, "drive", "amount", 0, a, 1.5, true},
  };
  // Every role asked for: mapped or skipped, decided before anything is written.
  std::vector<const Move*> todo;
  RoleToolResult r;
  for (const auto& m : moves) {
    if (!m.on) continue;
    const bool main = std::string(m.macro) != "feedback";  // the delay's feedback rides along with its mix
    if (hasMacro(map, m.role, m.macro)) todo.push_back(&m);
    else if (main) r.skipped.push_back(whyNot(map, m.role, m.macro));
  }
  if (todo.empty()) throw DocumentError("buildup", r.skipped.empty() ? "no effect to write" : r.skipped.front());
  // All or nothing (audit 2026-11-17, #5), and the same pre-check as toolSuck: the delay alone takes two roles.
  std::vector<std::string> needed;
  for (const Move* m : todo) needed.push_back(m->role);
  requireChainRoom(doc, trackId, map, needed);
  Document next = doc;
  std::vector<std::string> words;
  for (const Move* m : todo) {
    const double from = m->fromRest ? roleRest(*map.find(m->role), m->role, m->macro) : m->from;
    r.keys.push_back(writeRoleSweep(next, trackId, map, m->role, m->macro, start, dropTick, from, m->to, m->curve));
    if (std::find(r.roles.begin(), r.roles.end(), m->role) == r.roles.end()) {
      r.roles.push_back(m->role);
      const std::string role = m->role;
      words.push_back(role == "filter" ? "the filter opens" : role == "reverb" ? "the reverb grows" : role == "delay" ? "the echoes grow" : "the drive pushes");
    }
  }
  r.sentence = "Build-up effects over " + bars(dropTick - start) + ": ";
  for (size_t i = 0; i < words.size(); ++i) r.sentence += (i ? (i + 1 == words.size() ? " and " : ", ") : "") + words[i];
  r.sentence += ", back on the drop";
  doc = std::move(next);
  return r;
}

// ================================================================================================================
// The song tools for the AI (role_tools.hpp, second part).

namespace {
std::string lowerText(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
// The kind of a section by its name: drop | open (intro, break, build-up, outro) | "" (other).
std::string sectionMood(const std::string& name) {
  const std::string n = lowerText(name);
  if (n.find("drop") != std::string::npos) return "drop";
  for (const char* w : {"break", "intro", "build", "rise", "outro"})
    if (n.find(w) != std::string::npos) return "open";
  return "";
}
constexpr const char* kPlaceKey = "corePlace";  // the bus's tag: {"kind": "reverb" | "delay", "place": "<preset>"}
bool isPlaceBus(const Track& t, const std::string& kind) {
  return t.instrument.kind == "bus" && t.extra.contains(kPlaceKey) && t.extra[kPlaceKey].is_object() &&
         t.extra[kPlaceKey].value("kind", std::string()) == kind;
}
std::string busNameFor(const SpacePreset& p) {
  std::string place = p.id;
  if (!place.empty()) place[0] = (char)std::toupper((unsigned char)place[0]);
  return std::string(p.kind == "reverb" ? "R-Core" : "D-Core") + " \xC2\xB7 " + place;
}
}  // namespace

SongSpan sectionSpan(const Document& doc, const std::string& section) {
  if (section.empty()) return {0, doc.lengthTicks, "the whole song"};
  const std::string want = lowerText(section);
  for (const auto& s : doc.sections)
    if (lowerText(s.name) == want) return {s.startTick, std::min(doc.lengthTicks, s.startTick + s.lengthTicks), s.name};
  for (const auto& s : doc.sections)
    if (lowerText(s.name).find(want) != std::string::npos) return {s.startTick, std::min(doc.lengthTicks, s.startTick + s.lengthTicks), s.name};
  throw DocumentError("section", "no section called '" + section + "'");
}

// Send levels and section moves from the old R-Core / D-Core presets (a reverb at 15-35 % mix on the track is about
// -16 to -9 dB of tail; drops drier, breaks open). Size / feedback are the plugin's macro, 0..1.
const std::vector<SpacePreset>& spacePresets() {
  static const std::vector<SpacePreset> presets = {
      {"reverb", "room", "A short dark room that glues the parts; drier in the drops", -12, -4, 0, 0.3},
      {"reverb", "hall", "A soft hall behind leads and pads; it opens up in the breaks", -12, -5, 0, 0.6},
      {"reverb", "plate", "A bright plate for claps, snares and vocals; drier in the drops", -12, -4, 0, 0.45},
      {"reverb", "huge", "Endless space for pads and slow hits, the same all song long", -9, 0, 0, 0.95},
      {"reverb", "tight", "Hi-tech dry: a tiny room only to glue, almost gone in the drops", -16, -6, 0, 0.15},
      {"delay", "throw", "Quarter-note throws at the ends of phrases, loud in the breaks", -10, -4, 0, 0.3},
      {"delay", "dotted", "Dotted-eighth echoes behind the leads, held down in the drops", -12, -3, 0, 0.4},
      {"delay", "eighths", "Soft eighths behind pads and leads", -14, -3, 0, 0.35},
      {"delay", "sixteenths", "Fast sixteenths for leads and FX, held down in the drops", -14, -6, 0, 0.25},
      {"delay", "slow", "Long far-away echoes, the same all song long", -10, 0, 0, 0.7}};
  return presets;
}

const SpacePreset* findSpacePreset(const std::string& kind, const std::string& id) {
  const std::string want = lowerText(id.empty() ? (kind == "delay" ? "dotted" : "room") : id);
  for (const auto& p : spacePresets())
    if (p.kind == kind && p.id == want) return &p;
  return nullptr;
}

SharedBusResult toolSharedBus(Document& doc, const std::vector<std::string>& trackIds, const RoleMap& map, const std::string& kindIn,
                              const std::string& preset) {
  // Case does not matter, as for preset / intent / section (R3-F).
  const std::string kind = lowerText(kindIn);
  if (kind != "reverb" && kind != "delay") throw DocumentError("bus", "kind is reverb or delay");
  const SpacePreset* p = findSpacePreset(kind, preset);
  if (!p) throw DocumentError("bus", "no " + kind + " preset '" + preset + "'");
  checkMacro(map, kind, "mix");  // the bus plays fully wet: its mix must be reachable
  std::vector<std::string> tracks;
  for (const auto& id : trackIds) {
    const Track& t = trackById(doc, id);  // throws for a missing track, before any change
    if (t.instrument.kind == "bus" || isPlaceBus(t, kind)) continue;
    tracks.push_back(id);
  }
  if (tracks.empty()) throw DocumentError("bus", "choose the tracks that share the " + kind);
  if (doc.lengthTicks < kPpq) throw DocumentError("bus", "the song is empty");
  SharedBusResult r;
  for (const auto& t : doc.tracks)
    if (isPlaceBus(t, kind) && t.extra[kPlaceKey].value("place", std::string()) == p->id) r.busId = t.id;
  if (r.busId.empty()) {
    r.busId = addBusTrack(doc, busNameFor(*p));
    r.newBus = true;
  }
  trackById(doc, r.busId).extra[kPlaceKey] = {{"kind", kind}, {"place", p->id}};
  // The plugin fully wet on the bus (only the tail: the dry sound stays on each track), its size / feedback set.
  writeRoleSweep(doc, r.busId, map, kind, "mix", 0, doc.lengthTicks, 1.0, 1.0);
  const char* sizeMacro = kind == "reverb" ? "size" : "feedback";
  r.sizeSet = hasMacro(map, kind, sizeMacro);
  if (r.sizeSet) writeRoleSweep(doc, r.busId, map, kind, sizeMacro, 0, doc.lengthTicks, p->size, p->size);
  // One place of a kind per track: a send to another R-Core (D-Core) bus moves here; a bus nobody feeds goes.
  // Round 3 (R3-E): each track changes on its own and only if it works. Before, the old send went first and a
  // full track (8 sends) or a routing loop then lost both, in silence. Now the track keeps what it had and is named.
  std::set<std::string> left;
  for (const auto& id : tracks) {
    Track& t = trackById(doc, id);
    const Mix before = t.mix;
    std::vector<std::string> drop, from;
    std::string sendId;
    for (const auto& snd : t.mix.sends) {
      const Track* to = doc.findTrack(snd.to);
      if (snd.to == r.busId) sendId = snd.id;
      else if (to && isPlaceBus(*to, kind)) { drop.push_back(snd.id); from.push_back(snd.to); }
    }
    try {
      for (const auto& sid : drop) removeSend(doc, id, sid);
      if (sendId.empty()) addSend(doc, id, r.busId, p->sendDb, false);
      else setSend(doc, id, sendId, p->sendDb, false);
      ++r.sends;
      left.insert(from.begin(), from.end());
    } catch (const DocumentError& e) {
      trackById(doc, id).mix = before;
      r.skipped.push_back(trackById(doc, id).name + " (" + e.what() + ")");
    }
  }
  for (const auto& bus : left) {
    bool used = false;
    for (const auto& t : doc.tracks) {
      if (t.mix.output == bus) used = true;
      for (const auto& snd : t.mix.sends) used = used || snd.to == bus;
    }
    if (!used && doc.findTrack(bus)) removeTrack(doc, bus);
  }
  // The bus level follows the sections. Every section gets its points (a 0 dB section brings the level back after
  // a drop), but only the ones that move count for the sentence (R3-G: it said "quieter in the drops" with all at 0).
  int drier = 0, opener = 0;
  if (p->dropDb != 0 || p->openDb != 0)
    for (const auto& s : doc.sections) {
      const std::string mood = sectionMood(s.name);
      const double db = mood == "drop" ? p->dropDb : mood == "open" ? p->openDb : 0;
      const int64_t end = std::min(doc.lengthTicks, s.startTick + std::max<int64_t>(1, s.lengthTicks)) - 1;
      if (end <= s.startTick) continue;
      setAutomationPoint(doc, r.busId, "gainDb", s.startTick, db);
      setAutomationPoint(doc, r.busId, "gainDb", end, db);
      if (db < 0) ++drier;
      if (db > 0) ++opener;
    }
  r.sections = drier + opener;
  r.sentence = std::string(kind == "reverb" ? "R-Core" : "D-Core") + " " + p->id + ": " + std::to_string(r.sends) +
               (r.sends == 1 ? " track shares one " : " tracks share one ") + kind + " on " + trackById(doc, r.busId).name;
  if (drier > 0) r.sentence += ", quieter in the drops";
  if (opener > 0) r.sentence += ", louder in the breaks";
  // R3-D: huge / tight are mostly the size. A plugin with no size (feedback) control plays its own, and says so.
  if (!r.sizeSet)
    r.sentence += std::string(". The ") + kind + " has no " + sizeMacro + " control mapped, so its " + sizeMacro +
                  " is the plugin's own, not " + p->id + "'s";
  if (!r.skipped.empty()) {
    r.sentence += ". Left as they were: ";
    for (size_t i = 0; i < r.skipped.size(); ++i) r.sentence += (i ? ", " : "") + r.skipped[i];
  }
  return r;
}

const std::vector<std::string>& fxIntents() {
  static const std::vector<std::string> list = {"buildup", "suck", "intro", "outro", "impact", "break"};
  return list;
}

RoleToolResult toolIntentFx(Document& doc, const std::string& trackId, const RoleMap& map, const std::string& intent,
                            int64_t start, int64_t end, double amount) {
  const std::string what = lowerText(intent);
  const double a = std::clamp(amount, 0.0, 1.0);
  if (what == "buildup") {
    BuildupFx fx;
    fx.amount = a;
    fx.delay = hasMacro(map, "delay", "mix");
    return toolBuildupFx(doc, trackId, map, start, end, fx);
  }
  if (what == "suck") return toolSuck(doc, trackId, map, end, end - start, a);
  checkSpan(doc, start, end);
  trackById(doc, trackId);
  // Each move carries its own words, so the sentence says only what was written (R3-B: with no drive, "impact"
  // still promised grit).
  struct Move { const char *role, *macro; double from, to, curve; bool fromRest, toRest; const char* words; };
  std::vector<Move> moves;
  std::string head;
  if (what == "intro") {  // dark and wet to bright and dry
    moves = {{"filter", "cutoff", 1.0 - 0.8 * a, 0, 1.5, false, true, "the sound comes out of the dark"},
             {"reverb", "mix", 0.45 * a, 0, 1.0, false, true, "from wet to dry"}};
    head = "Intro";
  } else if (what == "outro") {  // bright to dark, the reverb grows
    moves = {{"filter", "cutoff", 0, 1.0 - 0.7 * a, 1.0, true, false, "the sound darkens"},
             {"reverb", "mix", 0, 0.55 * a, 1.5, true, false, "goes far away"}};
    head = "Outro";
  } else if (what == "impact") {  // a burst that dies away fast
    moves = {{"reverb", "mix", 0.3 + 0.6 * a, 0, 0.35, false, true, "a burst of space that dies away"},
             {"drive", "amount", 0.8 * a, 0, 0.35, false, true, "a burst of grit that dies away"}};
    head = "Impact";
  } else if (what == "break") {  // steady: darker and wider
    moves = {{"filter", "cutoff", 1.0 - 0.4 * a, 1.0 - 0.4 * a, 1.0, false, false, "darker"},
             {"reverb", "mix", 0.4 * a, 0.4 * a, 1.0, false, false, "wider"}};
    head = "Break";
  } else {
    throw DocumentError("fx", "no intent '" + intent + "' (buildup, suck, intro, outro, impact, break)");
  }
  RoleToolResult r;
  std::vector<const Move*> todo;
  for (const auto& m : moves) {
    if (hasMacro(map, m.role, m.macro)) todo.push_back(&m);
    else r.skipped.push_back(whyNot(map, m.role, m.macro));
  }
  if (todo.empty()) throw DocumentError("fx", r.skipped.front());
  // All or nothing (audit 2026-11-17, #5): no intent may leave a document with only the first sweep written.
  std::vector<std::string> needed;
  for (const Move* m : todo) needed.push_back(m->role);
  requireChainRoom(doc, trackId, map, needed);
  Document next = doc;
  std::string sentence = head + ": ";
  for (size_t i = 0; i < todo.size(); ++i) {
    const Move* m = todo[i];
    const double rest = roleRest(*map.find(m->role), m->role, m->macro);
    r.keys.push_back(writeRoleSweep(next, trackId, map, m->role, m->macro, start, end, m->fromRest ? rest : m->from,
                                    m->toRest ? rest : m->to, m->curve));
    r.roles.push_back(m->role);
    sentence += std::string(i ? " and " : "") + m->words;
  }
  if (what == "break") sentence += " while it lasts";
  r.sentence = sentence + " (" + bars(end - start) + ")";
  doc = std::move(next);
  return r;
}

RoleToolResult toolIntentFxTracks(Document& doc, const std::vector<std::string>& trackIds, const RoleMap& map,
                                  const std::string& intent, int64_t start, int64_t end, double amount) {
  checkSpan(doc, start, end);
  const std::string what = lowerText(intent);
  if (std::find(fxIntents().begin(), fxIntents().end(), what) == fxIntents().end())
    throw DocumentError("fx", "no intent '" + intent + "' (buildup, suck, intro, outro, impact, break)");
  std::vector<std::string> eligible;
  for (const auto& id : trackIds) {
    const Track* t = doc.findTrack(id);
    if (t && playsPatterns(*t)) eligible.push_back(id);
  }
  if (eligible.empty()) throw DocumentError("fx", "choose at least one track that plays notes");
  RoleToolResult r;
  for (const auto& id : trackIds) {
    const Track* t = doc.findTrack(id);
    if (!t || !playsPatterns(*t)) r.skipped.push_back("track '" + (t ? t->name : id) + "' plays no notes, skipped");
  }
  for (size_t i = 0; i < eligible.size(); ++i) {
    const RoleToolResult one = toolIntentFx(doc, eligible[i], map, what, start, end, amount);
    r.keys.insert(r.keys.end(), one.keys.begin(), one.keys.end());
    for (const auto& role : one.roles)
      if (std::find(r.roles.begin(), r.roles.end(), role) == r.roles.end()) r.roles.push_back(role);
    for (const auto& why : one.skipped)
      if (std::find(r.skipped.begin(), r.skipped.end(), why) == r.skipped.end()) r.skipped.push_back(why);
    if (i == 0) r.sentence = one.sentence;
  }
  r.sentence += " on " + std::to_string(eligible.size()) + (eligible.size() == 1 ? " track" : " tracks");
  return r;
}

RollResult toolRoll(Document& doc, const std::string& trackId, int64_t start, int64_t dropTick, const std::string& styleIn, int pitch,
                    int rise, int gapBeats) {
  checkSpan(doc, start, dropTick);
  if (dropTick - start < kPpq) throw DocumentError("roll", "a roll is at least one beat long");
  const std::string style = lowerText(styleIn);  // R3-F: case does not matter
  if (style != "steps" && style != "gradual") throw DocumentError("roll", "style is steps or gradual");
  if (rise < 0 || rise > 12) throw DocumentError("roll", "rise is 0-12 semitones");
  if (gapBeats < 0 || gapBeats > 4) throw DocumentError("roll", "gap is 0-4 beats");
  Track& target = trackById(doc, trackId);
  if (!playsPatterns(target)) throw DocumentError("roll", "'" + target.name + "' plays no notes (audio or a bus)");
  // All or nothing (round 3, R3-A): the clips were cut before addPattern, which throws on a full track (64
  // patterns), and the song was left with a hole. The work is done on a copy that replaces the track at the end.
  Track t = target;
  if (pitch < 0) {
    const int key = trackRoot(t, t.patterns.empty() ? nullptr : &t.patterns.front());
    pitch = key >= 0 ? key : 38;
  }
  if (pitch > 127) throw DocumentError("roll", "pitch must be 0-127");
  const int64_t length = dropTick - start;
  // The clips over the span step aside: cut at the edges, the inside goes.
  std::vector<std::string> ids;
  for (const auto& c : t.clips) ids.push_back(c.id);
  for (const auto& id : ids) {
    const Clip* c = nullptr;
    for (const auto& x : t.clips) if (x.id == id) c = &x;
    if (!c) continue;
    const int64_t cs = c->startTick, ce = c->startTick + c->lengthTicks;
    if (ce <= start || cs >= dropTick) continue;
    std::string inner = id;
    if (cs < start) inner = splitClip(t, id, start);
    int64_t innerEnd = 0;
    for (const auto& x : t.clips) if (x.id == inner) innerEnd = x.startTick + x.lengthTicks;
    if (innerEnd > dropTick) splitClip(t, inner, dropTick);
    removeClip(t, inner);
  }
  RollResult r;
  r.patternId = addPattern(t, length);
  BuildupOptions o;
  o.start = 0;
  o.length = length;
  o.pitch = pitch;
  o.roll = true;
  o.shape = style;
  o.gapTicks = gapBeats * kPpq;
  o.riseTo = rise;
  Pattern& pat = patternById(t, r.patternId);
  pat.name = "Roll";
  r.notes = makeBuildup(pat, o);
  addClip(t, r.patternId, start, length);
  target = std::move(t);
  const Track& done = target;
  r.sentence = "ROLL on " + done.name + ": " + std::to_string(r.notes) + " notes speeding up over " + bars(length) +
               (rise > 0 ? ", climbing " + std::to_string(rise) + " semitones" : "") + " into the drop";
  return r;
}

const std::vector<SongToolInfo>& songToolCatalog() {
  using P = SongToolParam;
  static const std::vector<SongToolInfo> list = {
      {"space", "Tracks share one reverb or one delay on a bus that follows the sections (R-Core / D-Core)",
       {"reverb", "delay"},
       {P{"tracks", "tracks", "", "the tracks that share it"}, P{"kind", "choice", "reverb|delay", "reverb (R-Core) or delay (D-Core)"},
        P{"preset", "choice", "room|hall|plate|huge|tight|throw|dotted|eighths|sixteenths|slow", "the place; reverb: room..tight, delay: throw..slow"}}},
      {"fx", "Effects by intent over a section, on one track (F-Core)",
       {"filter", "reverb", "delay", "drive"},
       {P{"track", "track", "", "the track"}, P{"intent", "choice", "buildup|suck|intro|outro|impact|break", "what the moment is for"},
        P{"section", "section", "", "the section by name (\"Build-up 1\"); empty: the whole song"},
        P{"amount", "number", "0..1", "how much (0.7 by default)"}}},
      {"roll", "A roll that speeds up into the drop, in a new pattern on the track", {},
       {P{"track", "track", "", "the track (a snare or the kick)"}, P{"section", "section", "", "the build-up it fills; the drop is its end"},
        P{"style", "choice", "steps|gradual", "steps: 1/4, 1/8, 1/16, 1/32; gradual: speeding up all the way"},
        P{"rise", "number", "0..12", "semitones it climbs by the drop"}, P{"gap", "number", "0..4", "beats of silence before the drop"}}},
      {"suck", "The sound is sucked into the drop: the reverb opens, the filter closes, back on the drop", {"reverb", "filter"},
       {P{"track", "track", "", "the track"}, P{"section", "section", "", "the section before the drop"}, P{"amount", "number", "0..1", "how much"}}},
      {"pump", "PUM: the volume dips at every kick (sidechain)", {"sidechain"},
       {P{"track", "track", "", "the track that pumps"}, P{"from", "track", "", "the track whose notes it follows (the kick)"},
        P{"section", "section", "", "where; empty: the whole song"}, P{"depth", "number", "0..1", "how far it dips"}}},
      {"buildup", "Build-up effects up to the drop: filter opens, reverb and delay grow, drive pushes",
       {"filter", "reverb", "delay", "drive"},
       {P{"track", "track", "", "the track"}, P{"section", "section", "", "the build-up"}, P{"amount", "number", "0..1", "how much"}}},
  };
  return list;
}

json songToolCatalogJson() {
  json out = json::array();
  for (const auto& t : songToolCatalog()) {
    json params = json::array();
    for (const auto& p : t.params) params.push_back({{"name", p.name}, {"kind", p.kind}, {"values", p.values}, {"what", p.what}});
    out.push_back({{"id", t.id}, {"what", t.what}, {"roles", t.roles}, {"params", params}});
  }
  return out;
}

}  // namespace core::doc
