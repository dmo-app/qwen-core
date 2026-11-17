// The Patterns window's rules (pattern_rack.hpp).
#include "pattern_rack.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include "edit.hpp"
#include "pattern_tools.hpp"

namespace core::doc {
namespace {

constexpr int64_t kBar = 4 * kPpq;
constexpr int64_t kSixteenth = kPpq / 4;
constexpr const char* kSlotsKey = "patternSlots";

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

int validBars(int bars) {
  if (bars <= 1) return 1;
  if (bars <= 2) return 2;
  if (bars <= 4) return 4;
  return kRackMaxBars;
}

Track* trackPtr(Document& doc, const std::string& id) {
  for (auto& t : doc.tracks) if (t.id == id) return &t;
  return nullptr;
}

PatternSlot slotFromJson(const json& j) {
  PatternSlot s;
  s.id = j.value("id", std::string());
  s.name = j.value("name", std::string());
  s.bars = validBars(j.value("bars", 4));
  s.swing = std::clamp(j.value("swing", 0.0), 0.0, kRackMaxSwing);
  s.root = j.value("root", std::string());
  s.scale = j.value("scale", std::string());
  if (j.contains("tracks") && j["tracks"].is_object())
    for (const auto& [k, v] : j["tracks"].items()) if (v.is_string()) s.tracks[k] = v.get<std::string>();
  return s;
}

json slotToJson(const PatternSlot& s) {
  json tracks = json::object();
  for (const auto& [k, v] : s.tracks) tracks[k] = v;
  json out = {{"id", s.id}, {"name", s.name}, {"bars", s.bars}, {"tracks", tracks}};
  if (s.swing > 0) out["swing"] = s.swing;
  if (!s.root.empty()) out["root"] = s.root;
  if (!s.scale.empty()) out["scale"] = s.scale;
  return out;
}

PatternSlot& slotById(std::vector<PatternSlot>& slots, const std::string& id) {
  for (auto& s : slots) if (s.id == id) return s;
  throw DocumentError("patternSlots", "no pattern '" + id + "' in the Patterns window");
}

std::string nextSlotId(const std::vector<PatternSlot>& slots) {
  int high = 0;
  for (const auto& s : slots)
    if (s.id.size() > 1 && s.id[0] == 's' && std::all_of(s.id.begin() + 1, s.id.end(), [](char c) { return std::isdigit((unsigned char)c); }))
      if (s.id.size() <= 9) high = std::max(high, std::stoi(s.id.substr(1)));  // longer: no stoi out_of_range (sweep)
  return "s" + std::to_string(high + 1);
}

// The swing's delay of an odd 1/16, in ticks.
int64_t swingTicks(double swing) { return std::llround(std::clamp(swing, 0.0, kRackMaxSwing) * kSixteenth / 2.0); }

constexpr const char* kSwingBase = "rackSwingBase";
void swingNote(Note& n, int64_t patternLength, int64_t from, int64_t to, int64_t jitter = 0) {
  int64_t base = n.tick - from, length = n.length;
  bool saved = false;
  if (n.extra.contains(kSwingBase) && n.extra[kSwingBase].is_object()) {
    const auto& record = n.extra[kSwingBase];
    if (record.value("renderedTick", n.tick) == n.tick && record.value("renderedLength", n.length) == n.length) {
      base = record.value("tick", base);
      length = record.value("length", length);
      saved = true;
    } else { // A note edited while swung becomes a fresh base; retain an untouched original length.
      base = n.tick - (record.value("renderedTick", n.tick) - record.value("tick", n.tick));
      if (record.value("renderedLength", n.length) == n.length) length = record.value("length", n.length);
      saved = true;
    }
  }
  const int64_t grid = (base + kSixteenth / 2) / kSixteenth * kSixteenth;
  if (!saved && (base < 0 || std::abs(base - grid) > jitter || (grid / kSixteenth) % 2 != 1)) return;
  base = std::clamp<int64_t>(base, 0, patternLength - 1);
  length = std::clamp<int64_t>(length, 1, patternLength - base);
  n.tick = std::min(base + to, patternLength - 1);
  n.length = std::clamp<int64_t>(length, 1, patternLength - n.tick);
  if (to == 0) n.extra.erase(kSwingBase);
  else n.extra[kSwingBase] = {{"tick", base}, {"length", length}, {"renderedTick", n.tick}, {"renderedLength", n.length}};
}

std::string songRootName(const Document& doc) {
  static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int key = songRoot(doc); // shared domain rule: the bass notes' durations, then melodic tracks
  return key >= 0 ? roots[key] : std::string();
}

// A copy of the notes with fresh ids for this pattern.
void putNotes(Pattern& p, const std::vector<Note>& notes) {
  p.notes.clear();
  for (const auto& n : notes) {
    if (n.tick < 0 || n.tick >= p.lengthTicks) continue;
    Note x = n;
    x.id = newNoteId(p);
    x.length = std::clamp<int64_t>(x.length, 1, p.lengthTicks - x.tick);
    p.notes.push_back(x);
  }
}

}  // namespace

std::vector<PatternSlot> rackSlots(const Document& doc, int64_t songTick) {
  std::vector<PatternSlot> out;
  if (doc.extra.contains(kSlotsKey) && doc.extra[kSlotsKey].is_array())
    for (const auto& j : doc.extra[kSlotsKey]) {
      if (!j.is_object()) continue;
      PatternSlot s = slotFromJson(j);
      if (s.id.empty() || findRackSlot(out, s.id)) continue;
      if (s.name.empty()) s.name = "Pattern " + std::to_string(out.size() + 1);
      for (auto it = s.tracks.begin(); it != s.tracks.end();) {  // tracks or patterns that went
        const Track* t = doc.findTrack(it->first);
        if (!t || !playsPatterns(*t) || !t->findPattern(it->second)) it = s.tracks.erase(it);
        else ++it;
      }
      out.push_back(s);
    }
  if (!out.empty()) return out;
  PatternSlot first;
  first.id = "s1";
  first.name = "Main Groove";
  int64_t longest = 0;
  for (const auto& t : doc.tracks) {
    if (!playsPatterns(t)) continue;
    const std::string pid = patternAt(t, songTick);
    const Pattern* p = t.findPattern(pid);
    if (!p) continue;
    first.tracks[t.id] = pid;
    longest = std::max(longest, p->lengthTicks);
  }
  first.bars = validBars((int)((longest + kBar - 1) / kBar));
  out.push_back(first);
  return out;
}

void setRackSlots(Document& doc, const std::vector<PatternSlot>& slots) {
  json list = json::array();
  for (const auto& s : slots) list.push_back(slotToJson(s));
  doc.extra[kSlotsKey] = list;
}

void ensureRackSlots(Document& doc, int64_t songTick) {
  if (doc.extra.contains(kSlotsKey) && doc.extra[kSlotsKey].is_array() && !doc.extra[kSlotsKey].empty()) return;
  setRackSlots(doc, rackSlots(doc, songTick));
}

const PatternSlot* findRackSlot(const std::vector<PatternSlot>& slots, const std::string& id) {
  for (const auto& s : slots) if (s.id == id) return &s;
  return nullptr;
}

const Pattern* rackPattern(const Document& doc, const PatternSlot& slot, const std::string& trackId) {
  const auto it = slot.tracks.find(trackId);
  if (it == slot.tracks.end()) return nullptr;
  const Track* t = doc.findTrack(trackId);
  return t ? t->findPattern(it->second) : nullptr;
}

Pattern& rackPatternFor(Document& doc, const std::string& slotId, const std::string& trackId) {
  auto slots = rackSlots(doc);
  PatternSlot& slot = slotById(slots, slotId);
  Track* t = trackPtr(doc, trackId);
  if (!t || !playsPatterns(*t)) throw DocumentError("patternSlots", "no note track '" + trackId + "'");
  const auto it = slot.tracks.find(trackId);
  if (it == slot.tracks.end() || !t->findPattern(it->second)) {
    const std::string pid = addPattern(*t, slot.bars * kBar);
    patternById(*t, pid).name = cutUtf8(slot.name, kMaxPatternNameBytes);  // a song from before 2026-11-17 can hold a longer one
    slot.tracks[trackId] = pid;
  }
  setRackSlots(doc, slots);
  return patternById(*t, slot.tracks[trackId]);
}

bool rackStepRow(const Track& t) {
  const std::string& n = t.instrument.name;
  if (t.instrument.kind == "builtin" && (n == "kick" || n == "hats" || n == "snare" || n == "drums")) return true;
  const std::string part = pcoreGuessPart(t);
  if (part == "bass" || part == "lead" || part == "arp" || part == "pad") return false;
  if (part == "kick" || part == "hats" || part == "cymbals" || part == "perc" || part == "build") return true;
  return t.instrument.kind == "builtin" && n == "sampler";  // a one-shot sound (fx, stabs...)
}

int rackStepPitch(const Track& t, const Pattern* p) {
  if (p && !p->notes.empty()) {
    std::map<int, int> count;
    for (const auto& n : p->notes) ++count[n.pitch];
    return std::max_element(count.begin(), count.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
  }
  const std::string& n = t.instrument.name;
  if (n == "kick" || n == "drums") return 36;
  if (n == "snare") return 38;
  if (n == "hats") return lower(t.name).find("open") != std::string::npos ? 46 : 42;
  if (n == "sampler" && !t.instrument.samples.empty()) return t.instrument.samples.front().rootKey;
  const int root = trackRoot(t, p);
  return root >= 0 ? root : 60;
}

bool rackStepOn(const Pattern& p, int64_t start, int64_t length) {
  return std::any_of(p.notes.begin(), p.notes.end(), [&](const Note& n) { return n.tick >= start && n.tick < start + length; });
}

bool toggleRackStep(Pattern& p, int64_t start, int64_t length, int pitch, double velocity, double swing) {
  if (start < 0 || start >= p.lengthTicks || length < 1) throw DocumentError("step", "outside the pattern");
  if (rackStepOn(p, start, length)) {
    p.notes.erase(std::remove_if(p.notes.begin(), p.notes.end(), [&](const Note& n) { return n.tick >= start && n.tick < start + length; }),
                  p.notes.end());
    return false;
  }
  int64_t tick = start;
  if (length == kSixteenth && (start / kSixteenth) % 2 == 1 && start % kSixteenth == 0) tick += swingTicks(swing);
  Note n;
  n.id = newNoteId(p);
  n.tick = std::min(tick, p.lengthTicks - 1);
  n.length = std::clamp<int64_t>(length / 2, 1, p.lengthTicks - n.tick);
  n.pitch = std::clamp(pitch, 0, 127);
  n.velocity = std::clamp(velocity, 0.0, 1.0);
  if (tick != start) { n.tick = start; n.length = std::clamp<int64_t>(length / 2, 1, p.lengthTicks - start); swingNote(n, p.lengthTicks, 0, swingTicks(swing)); }
  p.notes.push_back(n);
  std::sort(p.notes.begin(), p.notes.end(), [](const Note& a, const Note& b) { return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch; });
  return true;
}

void setRackSwing(Document& doc, const std::string& slotId, double swing) {
  if (!std::isfinite(swing)) throw DocumentError("swing", "a number");
  auto slots = rackSlots(doc);
  PatternSlot& slot = slotById(slots, slotId);
  swing = std::clamp(swing, 0.0, kRackMaxSwing);
  const int64_t from = swingTicks(slot.swing), to = swingTicks(swing);
  if (from != to)
    for (const auto& [trackId, pid] : slot.tracks) {
      Track* t = trackPtr(doc, trackId);
      Pattern* p = t ? t->findPattern(pid) : nullptr;
      if (!p) continue;
      for (auto& n : p->notes) swingNote(n, p->lengthTicks, from, to);
    }
  slot.swing = swing;
  setRackSlots(doc, slots);
}

void setRackBars(Document& doc, const std::string& slotId, int bars) {
  if (bars != 1 && bars != 2 && bars != 4 && bars != 8) throw DocumentError("bars", "1, 2, 4 or 8");
  auto slots = rackSlots(doc);
  PatternSlot& slot = slotById(slots, slotId);
  for (const auto& [trackId, pid] : slot.tracks) {
    Track* t = trackPtr(doc, trackId);
    Pattern* p = t ? t->findPattern(pid) : nullptr;
    if (p) extendPatternRepeating(*p, bars * kBar);
  }
  slot.bars = bars;
  setRackSlots(doc, slots);
}

std::string addRackSlot(Document& doc, const std::string& name, int bars) {
  auto slots = rackSlots(doc);
  PatternSlot s;
  s.id = nextSlotId(slots);
  s.name = cutUtf8(name.empty() ? "Pattern " + std::to_string(slots.size() + 1) : name, kMaxPatternNameBytes);
  s.bars = validBars(bars);
  slots.push_back(s);
  setRackSlots(doc, slots);
  return s.id;
}

std::string duplicateRackSlot(Document& doc, const std::string& slotId) {
  auto slots = rackSlots(doc);
  const PatternSlot from = slotById(slots, slotId);
  PatternSlot copy = from;
  copy.id = nextSlotId(slots);
  copy.name = nameWithSuffix(from.name, " copy", kMaxPatternNameBytes);
  copy.tracks.clear();
  for (const auto& [trackId, pid] : from.tracks) {
    Track* t = trackPtr(doc, trackId);
    if (!t || !t->findPattern(pid)) continue;
    Pattern p = patternById(*t, pid);
    const std::string newId = addPattern(*t, p.lengthTicks);
    Pattern& dst = patternById(*t, newId);
    dst.name = copy.name;
    dst.notes = p.notes;
    dst.extra = p.extra;
    copy.tracks[trackId] = newId;
  }
  slots.push_back(copy);
  setRackSlots(doc, slots);
  return copy.id;
}

void renameRackSlot(Document& doc, const std::string& slotId, const std::string& name) {
  if (name.empty()) throw DocumentError("name", "a pattern needs a name");
  auto slots = rackSlots(doc);
  slotById(slots, slotId).name = cutUtf8(name, kMaxPatternNameBytes);
  setRackSlots(doc, slots);
}

void removeRackSlot(Document& doc, const std::string& slotId) {
  auto slots = rackSlots(doc);
  slotById(slots, slotId);
  if (slots.size() <= 1) throw DocumentError("patternSlots", "the window keeps at least one pattern");
  slots.erase(std::remove_if(slots.begin(), slots.end(), [&](const PatternSlot& s) { return s.id == slotId; }), slots.end());
  setRackSlots(doc, slots);
}

int clearRackSlot(Document& doc, const std::string& slotId, const std::vector<std::string>& trackIds) {
  auto slots = rackSlots(doc);
  const PatternSlot& slot = slotById(slots, slotId);
  int removed = 0;
  for (const auto& [trackId, pid] : slot.tracks) {
    if (!trackIds.empty() && std::find(trackIds.begin(), trackIds.end(), trackId) == trackIds.end()) continue;
    Track* t = trackPtr(doc, trackId);
    Pattern* p = t ? t->findPattern(pid) : nullptr;
    if (!p) continue;
    removed += (int)p->notes.size();
    p->notes.clear();
  }
  setRackSlots(doc, slots);
  return removed;
}

int sendRackSlotToArrange(Document& document, const std::string& slotId, int64_t atTick) {
  Document next = document;
  Document& doc = next;
  auto slots = rackSlots(doc);
  const PatternSlot& slot = slotById(slots, slotId);
  const int64_t start = std::max<int64_t>(0, atTick) / kBar * kBar;
  if (start > 4096LL * kBar - slot.bars * kBar) throw DocumentError("lengthTicks", "the song would be too long; nothing changed");
  const int64_t end = start + slot.bars * kBar;
  int placed = 0;
  for (const auto& [trackId, pid] : slot.tracks) {
    Track* t = trackPtr(doc, trackId);
    if (!t || !t->findPattern(pid)) continue;
    // Cut what was there: clips across the edges are split, the pieces inside go.
    for (const int64_t cut : {start, end}) {
      std::vector<std::string> crossing;
      for (const auto& c : t->clips) if (c.startTick < cut && c.startTick + c.lengthTicks > cut) crossing.push_back(c.id);
      for (const auto& id : crossing) splitClip(*t, id, cut);
    }
    std::vector<std::string> inside;
    for (const auto& c : t->clips) if (c.startTick >= start && c.startTick + c.lengthTicks <= end) inside.push_back(c.id);
    for (const auto& id : inside) removeClip(*t, id);
    addClip(*t, pid, start, end - start);
    ++placed;
  }
  if (placed && end > doc.lengthTicks) setSongLength(doc, end);
  setRackSlots(doc, slots);
  document = std::move(next);
  return placed;
}

Document rackPlayDocument(const Document& doc, const std::string& slotId, int64_t songTick) {
  const auto slots = rackSlots(doc, songTick);
  const PatternSlot* slot = findRackSlot(slots, slotId);
  if (!slot) throw DocumentError("patternSlots", "no pattern '" + slotId + "' in the Patterns window");
  Document out = doc;
  const int64_t length = slot->bars * kBar;
  for (auto& t : out.tracks) {
    t.clips.clear();
    t.audioClips.clear();
    const auto it = slot->tracks.find(t.id);
    if (it != slot->tracks.end() && t.findPattern(it->second)) addClip(t, it->second, 0, length);
  }
  out.tempoMap.clear();
  out.sections.clear();
  out.lengthTicks = length;
  out.loop = Loop{true, 0, length, json::object()};
  return out;
}

// ---- templates

std::vector<PatternTemplate> rackStyleTemplates(const PCoreCatalog& c) {
  std::vector<PatternTemplate> out;
  for (const auto& s : c.styles) {
    PatternTemplate t;
    t.id = "style-" + s.id;
    t.name = s.name;
    t.style = s.id;
    t.about = s.about;
    t.bpm = s.bpm;
    t.bars = 4;
    t.builtin = true;
    out.push_back(t);
  }
  return out;
}

PatternTemplate rackTemplateFromJson(const json& j) {
  if (!j.is_object() || j.value("format", std::string()) != "core-patterns") throw DocumentError("format", "not a Core patterns template");
  PatternTemplate t;
  t.id = j.value("id", std::string());
  t.name = j.value("name", t.id);
  t.style = j.value("style", std::string());
  t.about = j.value("about", std::string());
  t.bpm = j.value("bpm", 0.0);
  t.bars = validBars(j.value("bars", 4));
  if (t.id.empty() || t.name.empty()) throw DocumentError("id", "a template has an id and a name");
  if (!j.contains("rows") || !j["rows"].is_array()) throw DocumentError("rows", "a list of rows");
  for (const auto& r : j["rows"]) {
    PatternTemplateRow row;
    row.part = r.value("part", std::string());
    row.name = r.value("name", std::string());
    row.lengthTicks = std::clamp<int64_t>(r.value("lengthTicks", (int64_t)t.bars * kBar), kSixteenth, kRackMaxBars * kBar);
    if (r.contains("notes") && r["notes"].is_array())
      for (const auto& n : r["notes"]) {
        Note x;
        x.tick = n.value("tick", (int64_t)0);
        x.length = std::max<int64_t>(1, n.value("length", (int64_t)kSixteenth));
        x.pitch = std::clamp(n.value("pitch", 60), 0, 127);
        x.velocity = std::clamp(n.value("velocity", 0.8), 0.0, 1.0);
        if (x.tick >= 0 && x.tick < row.lengthTicks) row.notes.push_back(x);
      }
    t.rows.push_back(row);
  }
  return t;
}

json rackTemplateToJson(const PatternTemplate& t) {
  json rows = json::array();
  for (const auto& r : t.rows) {
    json notes = json::array();
    for (const auto& n : r.notes) notes.push_back({{"tick", n.tick}, {"length", n.length}, {"pitch", n.pitch}, {"velocity", n.velocity}});
    rows.push_back({{"part", r.part}, {"name", r.name}, {"lengthTicks", r.lengthTicks}, {"notes", notes}});
  }
  json out = {{"format", "core-patterns"}, {"version", 1}, {"id", t.id}, {"name", t.name}, {"bars", t.bars}, {"rows", rows}};
  if (!t.style.empty()) out["style"] = t.style;
  if (!t.about.empty()) out["about"] = t.about;
  if (t.bpm > 0) out["bpm"] = t.bpm;
  return out;
}

std::vector<PatternTemplate> loadRackTemplates(const std::filesystem::path& folder, std::vector<std::string>* problems) {
  std::vector<PatternTemplate> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return out;
  const std::string ext = kRackTemplateExtension;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    const std::string name = e.path().filename().string();
    if (!e.is_regular_file() || name.size() <= ext.size() || name.compare(name.size() - ext.size(), ext.size(), ext) != 0) continue;
    try {
      std::ifstream in(e.path(), std::ios::binary);
      PatternTemplate t = rackTemplateFromJson(json::parse(in));
      t.file = e.path().string();
      if (std::none_of(out.begin(), out.end(), [&](const PatternTemplate& x) { return x.id == t.id; })) out.push_back(t);
    } catch (const std::exception& x) {
      if (problems) problems->push_back(name + ": " + x.what());
    }
  }
  std::sort(out.begin(), out.end(), [](const PatternTemplate& a, const PatternTemplate& b) { return lower(a.name) < lower(b.name); });
  return out;
}

PatternTemplate rackTemplateFromSlot(const Document& doc, const std::string& slotId, const std::string& name) {
  const auto slots = rackSlots(doc);
  const PatternSlot* slot = findRackSlot(slots, slotId);
  if (!slot) throw DocumentError("patternSlots", "no pattern '" + slotId + "'");
  if (name.empty()) throw DocumentError("name", "a template needs a name");
  PatternTemplate t;
  t.name = name;
  for (char ch : lower(name)) t.id += std::isalnum((unsigned char)ch) ? ch : '-';
  t.bpm = doc.bpm;
  t.bars = slot->bars;
  for (const auto& tr : doc.tracks) {
    const Pattern* p = rackPattern(doc, *slot, tr.id);
    if (!p || !playsPatterns(tr)) continue;
    PatternTemplateRow row{pcoreGuessPart(tr), tr.name, p->lengthTicks, p->notes};
    for (auto& n : row.notes) { n.id.clear(); n.extra = json::object(); n.hasRoll = false; }
    t.rows.push_back(row);
  }
  if (t.rows.empty()) throw DocumentError("rows", "the pattern has no tracks");
  return t;
}

std::filesystem::path saveRackTemplate(const std::filesystem::path& folder, PatternTemplate t) {
  std::filesystem::create_directories(folder);
  const std::string base = t.id.empty() ? std::string("patterns") : t.id;
  std::string id = base;
  for (int i = 2; std::filesystem::exists(folder / (id + kRackTemplateExtension)); ++i) id = base + "-" + std::to_string(i);
  t.id = id;
  const auto file = folder / (id + kRackTemplateExtension);
  std::ofstream out(file, std::ios::binary);
  out << rackTemplateToJson(t).dump(2);
  if (!out) throw DocumentError("file", "could not write " + file.string());
  return file;
}

namespace {
// The template row for each track: the same part first (each row once), else the same name.
std::map<std::string, const PatternTemplateRow*> matchRows(const Document& doc, const PatternTemplate& t) {
  std::map<std::string, const PatternTemplateRow*> out;
  std::set<const PatternTemplateRow*> used;
  for (const auto& tr : doc.tracks) {
    if (!playsPatterns(tr)) continue;
    const std::string part = pcoreGuessPart(tr);
    const PatternTemplateRow* pick = nullptr;
    for (const auto& r : t.rows)
      if (!used.count(&r) && lower(r.name) == lower(tr.name)) { pick = &r; break; }
    if (!pick && !part.empty())
      for (const auto& r : t.rows)
        if (!used.count(&r) && r.part == part) { pick = &r; break; }
    if (pick) { out[tr.id] = pick; used.insert(pick); }
  }
  return out;
}
}  // namespace

int rackTemplateTracks(const Document& doc, const PatternTemplate& t) {
  if (!t.builtin) return (int)matchRows(doc, t).size();
  int n = 0;
  for (const auto& tr : doc.tracks) if (playsPatterns(tr) && !pcoreGuessPart(tr).empty()) ++n;
  return n;
}

int applyRackTemplate(Document& doc, const PCoreCatalog* c, const PatternTemplate& t, const std::string& slotId, uint64_t seed) {
  {
    auto slots = rackSlots(doc);
    slotById(slots, slotId);
    setRackSlots(doc, slots);
  }
  setRackBars(doc, slotId, t.bars);
  if (!t.builtin) {
    const auto rows = matchRows(doc, t);
    if (rows.empty()) throw DocumentError("template", "no track of the song plays a part of this template");
    for (const auto& [trackId, row] : rows) {
      Pattern& p = rackPatternFor(doc, slotId, trackId);
      std::vector<Note> notes;
      for (int64_t at = 0; at < p.lengthTicks; at += row->lengthTicks)  // a shorter row repeats
        for (Note n : row->notes) { n.tick += at; notes.push_back(n); }
      const Track* tr = doc.findTrack(trackId);
      // A drum row on a sampler plays the sampler's own key (another key may be silent there).
      if (tr && rackStepRow(*tr) && tr->instrument.name == "sampler") {
        const int key = rackStepPitch(*tr, nullptr);
        for (auto& n : notes) n.pitch = key;
      }
      putNotes(p, notes);
    }
    return (int)rows.size();
  }
  if (!c) throw DocumentError("template", "the P-Core catalogue is missing");
  std::vector<PCoreTrackChoice> choices;
  std::map<std::string, std::string> patterns;
  for (const auto& tr : std::vector<Track>(doc.tracks)) {
    if (!playsPatterns(tr)) continue;
    const std::string part = pcoreGuessPart(tr);
    if (part.empty() || !c->part(part)) continue;
    const Pattern& p = rackPatternFor(doc, slotId, tr.id);
    patterns[tr.id] = p.id;
    choices.push_back({tr.id, part, ""});
  }
  if (choices.empty()) throw DocumentError("template", "no track of the song has a part P-Core knows (kick, bass, hats...)");
  PCoreLayers l;
  l.style = t.style;
  l.align = "style";
  l.seed = seed;
  const auto slots = rackSlots(doc);
  const PatternSlot* slot = findRackSlot(slots, slotId);
  l.root = slot && !slot->root.empty() ? slot->root : songRootName(doc);
  if (slot) l.scale = slot->scale;
  pcoreApplyToSong(doc, *c, l, choices, 0, 0, slot ? slot->bars : t.bars, patterns);
  return (int)choices.size();
}

PCoreResult rackWriteShape(Document& doc, const PCoreCatalog& c, const std::string& slotId, const std::string& trackId,
                           const std::string& part, const std::string& shape, const std::string& style, uint64_t seed, bool vary) {
  const Pattern& p = rackPatternFor(doc, slotId, trackId);
  const std::string pid = p.id;
  const auto slots = rackSlots(doc);
  const PatternSlot* slot = findRackSlot(slots, slotId);
  PCoreLayers l;
  l.style = style;
  l.seed = seed;
  l.vary = vary;
  l.root = slot && !slot->root.empty() ? slot->root : songRootName(doc);
  if (slot) l.scale = slot->scale;
  if (style.empty()) {  // no style: the first scale, the catalogue's first root
    if (l.scale.empty()) l.scale = c.scales.empty() ? "" : c.scales.front().id;
    if (l.root.empty()) l.root = "A";
  }
  const auto r = pcoreApplyToSong(doc, c, l, {{trackId, part, shape}}, 0, 0, slot ? slot->bars : 4, {{trackId, p.id}});
  if (r.empty()) throw DocumentError("pcore", "nothing written");
  if (slot && slot->swing > 0) {
    Pattern& written = patternById(trackById(doc, trackId), pid);
    const auto* selectedStyle = c.style(style);
    const int64_t jitter = selectedStyle ? (int64_t)std::lround(selectedStyle->humanize * 24) : 0;
    for (auto& note : written.notes) swingNote(note, written.lengthTicks, 0, swingTicks(slot->swing), jitter);
  }
  return r.front();
}

} // namespace core::doc
