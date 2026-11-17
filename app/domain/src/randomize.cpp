#include "randomize.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "edit.hpp"
#include "samples.hpp"

namespace core::doc {

namespace {

constexpr int64_t S16 = kPpq / 4, TRIPLET = kPpq / 3, BAR = 4 * kPpq;

// ---------------------------------------------------------------- the style cards (menus, from the baseline)

struct Weighted { std::string name; int weight; };
struct StyleCard {
  std::string style;
  std::vector<Weighted> grammars;                 // bass rhythm families
  std::vector<std::vector<int>> turns;            // the last 4 beats of "Roll with turn" (semitones from the root)
  std::vector<std::vector<int>> alts;             // 16 beats of "Second roll"
  std::vector<double> gateFractions;              // of the shortest gap to the next bass note
  double gateMsLo, gateMsHi;                      // the "dry" KBBB signature, in real time
  std::vector<Weighted> hatGrids;                 // straight | holes | offbeat
  std::vector<std::vector<int>> hatRolls;         // 16th steps that roll
  std::vector<int> rollCounts;                    // notes per roll
  // J: the style's tempo range and how a section moves away from the song's tempo (ratios of it).
  double bpmLo, bpmHi;
  std::vector<double> breakRatios;                // a drop into a break or reduction
  std::vector<double> liftRatios;                 // a lift into a drop: faster
};

const StyleCard* cardFor(const std::string& style) {
  static const std::vector<StyleCard> cards = {
      {"Dark psycore",
       {{"kbbb", 4}, {"kbb", 2}, {"kb-b", 2}},
       {{0, 1, 0, -2}, {0, 0, 3, 1}, {0, -2, 0, 1}, {0, 1, 3, 1}},
       {{0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 0, 6, 3, 1}, {0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 6, 1, 0},
        {0, 0, 0, -2, 0, 0, 0, 1, 0, 0, 0, -2, 0, 3, 1, 0}},
       {0.5, 0.55, 0.6}, 32, 45,
       {{"straight", 4}, {"holes", 2}},
       {{}, {15}, {7, 15}, {3, 7, 11, 15}},
       {2, 3, 4},
       200, 260, {0.75, 0.8, 0.85}, {1.05, 1.1}},
      {"Hi-tech",
       {{"kbbb", 4}, {"kbb", 1}, {"triplet", 2}},
       {{0, 12, 0, -2}, {0, 0, 12, 5}, {0, 3, 0, 12}, {0, 12, 3, 12}},
       {{0, 12, 0, 3, 0, 12, 0, 5, 0, 12, 0, 3, 0, 12, 5, 3}, {0, 0, 12, 0, 0, 3, 12, 0, 0, 0, 12, 0, 5, 3, 12, 0},
        {0, 12, 0, 0, 3, 12, 0, 0, 0, 12, 0, 0, 5, 12, 3, 0}},
       {0.55, 0.6}, 40, 50,
       {{"straight", 5}, {"holes", 2}},
       {{15}, {7, 15}, {3, 7, 11, 15}, {11, 15}},
       {2, 3, 4},
       170, 220, {0.9, 0.95}, {1.1, 1.15}},
      {"Slambient",
       {{"kbb", 2}, {"kbbb", 2}, {"offbeat", 2}},
       {{0, 1, 0, -2}, {0, -2, -2, 1}, {0, 0, -2, 0}},
       {{0, 0, 0, 0, -2, -2, -2, -2, 1, 1, 1, 1, 0, 0, 3, 1}, {0, 0, -2, 0, 0, 0, 1, 0, 0, 0, -2, 0, 3, 0, 1, 0}},
       {0.55, 0.6, 0.7}, 25, 45,
       {{"offbeat", 4}, {"holes", 2}},
       {{}, {}, {15}},
       {2, 3},
       180, 500, {0.67, 0.75}, {1.25, 1.5}},
      // The psytrance family (2026-11-04): the genre's ranges until Diogo sets his own; tempo moves are small here.
      {"Darkpsy",
       {{"kbbb", 5}, {"kbb", 1}, {"kb-b", 2}},
       {{0, 1, 0, -2}, {0, 0, 3, 1}, {0, -2, 0, 1}, {0, 1, 3, 1}},
       {{0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 0, 6, 3, 1}, {0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 6, 1, 0}},
       {0.5, 0.55, 0.6}, 35, 50,
       {{"straight", 4}, {"holes", 2}},
       {{15}, {7, 15}, {3, 7, 11, 15}},
       {2, 3, 4},
       148, 160, {0.95}, {1.03, 1.05}},
      {"Twilight",
       {{"kbbb", 5}, {"kbb", 2}, {"kb-b", 1}},
       {{0, 0, 3, 1}, {0, 1, 0, -2}, {0, 3, 0, 1}},
       {{0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 3, 0, 5, 3, 1}, {0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 1, 0, 6, 3, 1}},
       {0.55, 0.6}, 45, 60,
       {{"holes", 4}, {"straight", 2}},
       {{}, {11}, {11, 15}},
       {2, 3},
       145, 150, {0.97}, {1.02, 1.03}},
      {"Full-on",
       {{"kbbb", 6}, {"kbb", 1}},
       {{0, 0, 7, 5}, {0, 12, 0, 7}, {0, 0, 3, 5}, {0, 7, 12, 7}},
       {{0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3, 3, 5, 5, 7, 5}, {0, 0, 12, 0, 0, 0, 7, 0, 0, 0, 12, 0, 5, 5, 7, 5}},
       {0.55, 0.6, 0.65}, 50, 65,
       {{"straight", 5}, {"holes", 1}},
       {{15}, {7, 15}},
       {2, 3},
       140, 148, {0.97}, {1.02, 1.03}},
      {"Progressive psy",
       {{"kbb", 4}, {"kbbb", 3}, {"offbeat", 1}},
       {{0, 0, -2, -4}, {0, 0, 0, -2}, {0, -2, 0, 3}},
       {{0, 0, 0, 0, -2, -2, -2, -2, -4, -4, -4, -4, -2, -2, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, -2, -2, -2, -2, 3, 3, 0, 0}},
       {0.6, 0.7}, 55, 75,
       {{"holes", 4}, {"offbeat", 2}, {"straight", 1}},
       {{}, {}, {15}},
       {2, 3},
       130, 140, {0.95, 0.97}, {1.02, 1.04}}};
  for (const auto& c : cards)
    if (c.style == style) return &c;
  return nullptr;
}

std::vector<int64_t> grammarOffsets(const std::string& g) {
  if (g == "kbb") return {S16, 2 * S16};
  if (g == "kb-b") return {S16, 3 * S16};
  if (g == "triplet") return {TRIPLET, 2 * TRIPLET};
  if (g == "offbeat") return {2 * S16};
  return {S16, 2 * S16, 3 * S16};  // kbbb
}

// The shortest gap from a bass note to the next one (the next beat's first note included).
int64_t shortestGap(const std::vector<int64_t>& offs) {
  int64_t gap = kPpq + offs.front() - offs.back();
  for (size_t i = 1; i < offs.size(); ++i) gap = std::min(gap, offs[i] - offs[i - 1]);
  return gap;
}

// ---------------------------------------------------------------- a seeded random source (same on every platform)

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t x = (s += 0x9e3779b97f4a7c15ULL);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }
  double unit() { return (double)(next() >> 11) / 9007199254740992.0; }
  bool chance(double p) { return unit() < p; }
  int below(int n) { return n <= 1 ? 0 : (int)(next() % (uint64_t)n); }
  const std::string& pick(const std::vector<Weighted>& w) {
    int total = 0;
    for (const auto& x : w) total += x.weight;
    int r = below(total);
    for (const auto& x : w) { if (r < x.weight) return x.name; r -= x.weight; }
    return w.back().name;
  }
  template <class T> const T& pick(const std::vector<T>& v) { return v[(size_t)below((int)v.size())]; }
};
uint64_t hashText(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
  return h;
}
// The song's style and BPM, so one seed sounds different on two templates.
std::string& styleSalt() { static thread_local std::string salt; return salt; }
// One stream per (section, axis): turning an axis off never changes what the others draw.
Rng streamFor(uint64_t seed, const std::string& section, const std::string& axis) {
  Rng r(seed ^ hashText(section + "|" + axis) ^ (hashText(styleSalt()) << 1));
  r.next();
  return r;
}

// ---------------------------------------------------------------- clips and patterns

Track* findTrack(Document& doc, const std::string& id) {
  for (auto& t : doc.tracks) if (t.id == id) return &t;
  return nullptr;
}

// Cuts every clip that crosses `tick` into two, the pattern keeping its phase.
void splitAt(Track& t, int64_t tick) {
  std::vector<Clip> add;
  for (auto& c : t.clips) {
    if (c.startTick < tick && c.startTick + c.lengthTicks > tick) {
      const Pattern* p = t.findPattern(c.patternId);
      const int64_t plen = p ? std::max<int64_t>(1, p->lengthTicks) : 1;
      Clip second = c;
      second.lengthTicks = c.startTick + c.lengthTicks - tick;
      second.offsetTicks = (c.offsetTicks + (tick - c.startTick)) % plen;
      second.startTick = tick;
      c.lengthTicks = tick - c.startTick;
      add.push_back(second);
    }
  }
  for (auto& c : add) {
    const std::string id = addClip(t, c.patternId, c.startTick, c.lengthTicks);
    for (auto& x : t.clips) if (x.id == id) { x.offsetTicks = c.offsetTicks; x.extra = c.extra; }
  }
}

struct Span { int64_t from = 0, to = 0; bool any() const { return to > from; } };
Span coverage(const Track& t, int64_t a, int64_t b) {
  Span s{b, a};
  for (const auto& c : t.clips)
    if (c.startTick >= a && c.startTick < b) { s.from = std::min(s.from, c.startTick); s.to = std::max(s.to, c.startTick + c.lengthTicks); }
  return s;
}
bool usesPattern(const Track& t, int64_t a, int64_t b, const std::string& pid) {
  for (const auto& c : t.clips) if (c.startTick >= a && c.startTick < b && c.patternId == pid) return true;
  return false;
}
void clearSection(Track& t, int64_t a, int64_t b) {
  t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.startTick >= a && c.startTick < b; }), t.clips.end());
}
bool isFill(const Pattern& p) {
  std::string n = p.id + " " + p.name;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return n.find("fill") != std::string::npos;
}
// The pattern that plays longest in the song (fills aside): the track's main groove.
std::string mainPattern(const Track& t, bool fills) {
  std::map<std::string, int64_t> length;
  for (const auto& c : t.clips) {
    const Pattern* p = t.findPattern(c.patternId);
    if (p && isFill(*p) == fills) length[c.patternId] += c.lengthTicks;
  }
  std::string best;
  int64_t most = -1;
  for (const auto& [id, len] : length) if (len > most) { most = len; best = id; }
  return best;
}
// The pattern that plays longest in [a, b) (fills aside).
std::string mainPatternIn(const Track& t, int64_t a, int64_t b) {
  std::map<std::string, int64_t> length;
  for (const auto& c : t.clips) {
    const Pattern* p = t.findPattern(c.patternId);
    if (c.startTick >= a && c.startTick < b && p && !isFill(*p)) length[c.patternId] += c.lengthTicks;
  }
  std::string best;
  int64_t most = -1;
  for (const auto& [id, len] : length) if (len > most) { most = len; best = id; }
  return best;
}

// A pattern name the document reader takes back: it refuses a pattern whose "name" is over 40 bytes
// (document.cpp: p.name = pr.string("name", p.id, 40)), so Save and the autosave failed on a song the app
// had just accepted (audit 2026-11-17, #1: "Groove second roll (triplet)" is 59 bytes). The cut never splits
// a UTF-8 character (the same rule as cutUtf8 in edit.cpp), and the suffix stays whole: it is what tells the
// patterns of one section apart, and isFill() reads the word "fill" out of it.
std::string newPattern(Track& t, const std::string& head, const std::string& suffix, int64_t length, std::vector<Note> notes) {
  const std::string id = addPattern(t, length);
  Pattern& p = patternById(t, id);
  p.name = nameWithSuffix(head, suffix, kMaxPatternNameBytes);
  std::sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.tick != y.tick ? x.tick < y.tick : x.pitch < y.pitch; });
  for (size_t i = 0; i < notes.size(); ++i) notes[i].id = "n" + std::to_string(i + 1);
  p.notes = std::move(notes);
  return id;
}
Note note(int64_t tick, int64_t length, int pitch, double velocity) {
  Note n;
  n.tick = tick;
  n.length = std::max<int64_t>(1, length);
  n.pitch = std::clamp(pitch, 0, 127);
  n.velocity = std::clamp(velocity, 0.0, 1.0);
  return n;
}

// Lays a groove over [from, to) of a section that starts at `a`: phrases of `every` bars from the section's
// start, the last bar of each phrase the fill (when there is one and the phrase has 2 bars or more).
void lay(Track& t, int64_t a, int64_t from, int64_t to, const std::string& main, const std::string& fill, int every) {
  const int64_t phrase = (int64_t)every * BAR;
  int64_t tick = from;
  while (tick < to) {
    const int64_t phraseEnd = std::min(to, a + ((tick - a) / phrase + 1) * phrase);
    if (!fill.empty() && phraseEnd - tick >= 2 * BAR) {
      addClip(t, main, tick, phraseEnd - BAR - tick);
      addClip(t, fill, phraseEnd - BAR, BAR);
    } else {
      addClip(t, main, tick, phraseEnd - tick);
    }
    tick = phraseEnd;
  }
}

// The energy of each section (0 break .. 4 peak): the template's, or read from what plays.
std::vector<int> energies(const Document& doc) {
  std::vector<int> e;
  for (const auto& s : doc.sections) {
    const auto it = s.extra.find("energy");
    if (it != s.extra.end() && it->is_number()) { e.push_back(std::clamp(it->get<int>(), 0, 4)); continue; }
    int playing = 0;
    bool kick = false;
    for (const auto& t : doc.tracks)
      for (const auto& c : t.clips)
        if (c.startTick < s.startTick + s.lengthTicks && c.startTick + c.lengthTicks > s.startTick) {
          ++playing;
          kick |= t.id == "kick";
          break;
        }
    e.push_back(!kick ? 0 : playing >= 8 ? 4 : playing >= 6 ? 3 : playing >= 4 ? 2 : 1);
  }
  return e;
}

std::string num(double v) { return std::to_string((long long)std::llround(v)); }
std::string fmt(double v, const char* unit) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f %s", v, unit);
  return b;
}

}  // namespace

// The bass notes of [a, b) at a new tempo: the same length in ms (ratio = new BPM / old BPM), never reaching
// the next note. Shared patterns get a copy for the section.
// K: the sound of the built-in instruments, per style (the study's §8.5: the generator's values per style and
// variant, with room around them). One value per song: the instrument's fixed parameter.
struct SoundRange { const char* track; const char* instrument; const char* param; double lo, hi; };
static const std::vector<SoundRange>& soundRanges(const std::string& style) {
  static const std::vector<SoundRange> dark = {
      {"kick", "kick", "pitchStart", 250, 300}, {"kick", "kick", "pitchEnd", 44, 50}, {"kick", "kick", "pitchDecay", 16, 24},
      {"kick", "kick", "ampDecay", 110, 150}, {"kick", "kick", "click", 0.45, 0.65}, {"kick", "kick", "drive", 0.3, 0.45},
      {"hats", "hats", "tone", 9500, 11500}, {"hats", "hats", "closedDecay", 12, 20},
      {"open-hats", "hats", "tone", 9500, 11500}, {"open-hats", "hats", "openDecay", 150, 220}};
  static const std::vector<SoundRange> hitech = {
      {"kick", "kick", "pitchStart", 320, 370}, {"kick", "kick", "pitchEnd", 52, 58}, {"kick", "kick", "pitchDecay", 11, 16},
      {"kick", "kick", "ampDecay", 80, 110}, {"kick", "kick", "click", 0.65, 0.8}, {"kick", "kick", "drive", 0.28, 0.38},
      {"hats", "hats", "tone", 12000, 14000}, {"hats", "hats", "closedDecay", 10, 14},
      {"open-hats", "hats", "tone", 12000, 14000}, {"open-hats", "hats", "openDecay", 80, 120}};
  static const std::vector<SoundRange> slam = {
      {"kick", "kick", "pitchStart", 110, 150}, {"kick", "kick", "pitchEnd", 34, 40}, {"kick", "kick", "pitchDecay", 18, 32},
      {"kick", "kick", "ampDecay", 65, 115}, {"kick", "kick", "click", 0.02, 0.08}, {"kick", "kick", "drive", 0.05, 0.12},
      {"hats", "hats", "tone", 5500, 7000}, {"hats", "hats", "closedDecay", 16, 24},
      {"open-hats", "hats", "tone", 5500, 7000}, {"open-hats", "hats", "openDecay", 250, 350}};
  static const std::vector<SoundRange> none;
  return style == "Dark psycore" ? dark : style == "Hi-tech" ? hitech : style == "Slambient" ? slam : none;
}

static void retimeBass(Track& t, int64_t a, int64_t b, double ratio, std::vector<std::string>& did, double oldMs, double newMs) {
  std::map<std::string, std::string> copies;
  int64_t shown = -1;
  for (auto& c : t.clips) {
    if (c.startTick < a || c.startTick >= b) continue;
    auto it = copies.find(c.patternId);
    if (it == copies.end()) {
      Pattern src = patternById(t, c.patternId);
      std::vector<Note> notes = src.notes;
      std::vector<int64_t> starts;
      for (const auto& n : notes) starts.push_back(n.tick);
      std::sort(starts.begin(), starts.end());
      for (auto& n : notes) {
        // The gap to the next note (the pattern repeats: the first one comes back after its length).
        auto next = std::upper_bound(starts.begin(), starts.end(), n.tick);
        const int64_t gap = next != starts.end() ? *next - n.tick : src.lengthTicks - n.tick + starts.front();
        const int64_t want = std::max<int64_t>(20, (int64_t)std::llround(n.length * ratio));
        n.length = ratio > 1 ? std::min(want, std::max(n.length, gap * 8 / 10)) : want;
        if (shown < 0) shown = n.length;
      }
      // "Groove @ 240 BPM" retimed again is "Groove @ 120 BPM", not "Groove @ 240 BPM @ 120 BPM".
      std::string name = src.name;
      const size_t at = name.rfind(" @ ");
      if (at != std::string::npos && name.size() > 4 && name.compare(name.size() - 4, 4, " BPM") == 0) name.erase(at);
      const std::string id = newPattern(t, name, " @ " + fmt(60000.0 / (newMs * kPpq), "BPM"), src.lengthTicks, notes);
      it = copies.emplace(c.patternId, id).first;
    }
    c.patternId = it->second;
  }
  (void)oldMs;
  if (shown >= 0) did.push_back("bass gate kept at " + fmt(shown * newMs, "ms"));
}

const std::vector<std::string>& randomizeAxes() {
  static const std::vector<std::string> axes = {"bass", "gate", "hats", "kick", "roles", "fills", "tempo", "sound"};
  return axes;
}

std::vector<std::string> randomizeStyle(Document& doc, const RandomizeOptions& o) {
  const std::string style = songStyle(doc);
  const StyleCard* card = cardFor(style);
  if (!card) throw DocumentError("style", "Randomize works on songs made from a style template (dark psycore, hi-tech, slambient, darkpsy, twilight, full-on, progressive psy)");
  if (o.strength < 1 || o.strength > 3) throw DocumentError("strength", "must be 1 (subtle), 2 (medium) or 3 (bold)");
  for (const auto& a : o.axes)
    if (std::find(randomizeAxes().begin(), randomizeAxes().end(), a) == randomizeAxes().end()) throw DocumentError("axes", "no axis '" + a + "'");
  auto named = [&](const char* axis) { return std::find(o.axes.begin(), o.axes.end(), axis) != o.axes.end(); };
  auto on = [&](const char* axis) { return o.axes.empty() || named(axis); };
  const bool tempoOn = named("tempo");  // J only when asked for: it changes the song's length in time
  const bool soundOn = named("sound");  // K only when asked for: it changes the instruments, not the music
  const double p = o.strength == 1 ? 0.35 : o.strength == 2 ? 0.65 : 0.9;  // a section leaves the template's choice
  if (doc.sections.empty() || doc.bpm <= 0) return {};
  const std::vector<int> energy = energies(doc);
  styleSalt() = style + "|" + std::to_string((int)doc.bpm);
  auto msPerTickAt = [](double bpm) { return 60000.0 / (bpm * kPpq); };

  // J: the tempo per section. The song's tempo (transport bpm) is the style's; a section moves from it at an
  // energy change (slower into a break, back or faster into a drop), sometimes with a ramp over the last bars
  // before it; the last section comes back to the song's tempo. A section outside the style's range is allowed only if it is short (under a quarter of the
  // song) and within 40% of the song's tempo, with a warning; otherwise the tempo stays in the style's range.
  const Document before = doc;  // the tempo each section had: what its gates were written at
  std::vector<std::string> tempoLines;
  if (tempoOn) {
    std::vector<TempoPoint> map;
    double cur = doc.bpm;
    for (size_t i = 1; i < doc.sections.size(); ++i) {
      const Section& sec = doc.sections[i];
      Rng r = streamFor(o.seed, sec.id, "tempo");
      const bool intoBreak = energy[i] <= energy[i - 1] - 2;
      const bool intoLift = energy[i] >= 3 && energy[i] > energy[i - 1];
      double want = cur;
      if (intoBreak && r.chance(p)) want = doc.bpm * r.pick(card->breakRatios);
      else if (intoLift && cur != doc.bpm && r.chance(0.5 + p / 2)) want = doc.bpm;
      else if (intoLift && r.chance(p * 0.5)) want = doc.bpm * r.pick(card->liftRatios);
      else if (cur != doc.bpm && energy[i] >= 3 && r.chance(p)) want = doc.bpm;
      if (i + 1 == doc.sections.size()) want = doc.bpm;  // the outro is at the song's tempo (a DJ mixes out of it)
      want = std::clamp(std::round(want), 40.0, 600.0);
      if (want < card->bpmLo || want > card->bpmHi) {
        const bool shortSection = sec.lengthTicks * 4 < doc.lengthTicks;
        if (shortSection && std::abs(want / doc.bpm - 1) <= 0.4 + 1e-9) {
          tempoLines.push_back("Warning: " + sec.name + " at " + fmt(want, "BPM") + " is outside " + style + " (" + num(card->bpmLo) + "-" +
                               fmt(card->bpmHi, "BPM") + ")");
        } else {
          want = std::clamp(want, card->bpmLo, card->bpmHi);
        }
      }
      if (want == cur) continue;
      // A ramp of one step per bar over the last 4 bars before a faster section, sometimes.
      const Section& prev = doc.sections[i - 1];
      int ramp = 0;
      if (want > cur && prev.lengthTicks >= 16 * BAR && r.chance(p * 0.5)) {
        ramp = 4;
        for (int k = ramp; k >= 1; --k)
          map.push_back({sec.startTick - k * BAR, std::round(cur + (want - cur) * (ramp - k + 1) / (ramp + 1)), json::object()});
      }
      map.push_back({sec.startTick, want, json::object()});
      tempoLines.push_back(sec.name + ": " + fmt(want, "BPM") + (ramp ? ", ramp over the last " + std::to_string(ramp) + " bars before it" : ""));
      cur = want;
    }
    setTempoMap(doc, map);
    if (map.empty()) tempoLines.push_back("Tempo: " + fmt(doc.bpm, "BPM") + " all through");
  }

  // Clips end at section boundaries, so each section can be laid on its own.
  static const char* parts[] = {"kick", "bass", "hats", "open-hats", "perc", "accents", "lead", "riser"};
  const bool grooveOn = o.axes.empty() || std::any_of(o.axes.begin(), o.axes.end(), [](const std::string& x) { return x != "tempo" && x != "sound"; });
  for (const char* id : parts)
    if (Track* t = (grooveOn || tempoOn) ? findTrack(doc, id) : nullptr)
      for (const auto& s : doc.sections) splitAt(*t, s.startTick), splitAt(*t, s.startTick + s.lengthTicks);

  Track* kick = findTrack(doc, "kick");
  Track* bass = findTrack(doc, "bass");
  Track* hats = findTrack(doc, "hats");
  const std::string kickMain = kick ? mainPattern(*kick, false) : "";
  const std::string kickFill = kick ? mainPattern(*kick, true) : "";
  const std::string hatMain = hats ? mainPattern(*hats, false) : "";
  const std::string hatFill = hats ? mainPattern(*hats, true) : "";
  // The bass root: its most common note.
  int root = 36;
  if (bass) {
    std::map<int, int> count;
    for (const auto& c : bass->clips)
      if (const Pattern* pp = bass->findPattern(c.patternId)) for (const auto& n : pp->notes) ++count[n.pitch];
    int most = 0;
    for (const auto& [pitch, n] : count) if (n > most) { most = n; root = pitch; }
  }

  std::vector<std::string> changes;
  for (size_t i = 0; i < doc.sections.size(); ++i) {
    const Section& sec = doc.sections[i];
    const int64_t a = sec.startTick, b = sec.startTick + sec.lengthTicks;
    const bool lift = i + 1 < doc.sections.size() && energy[i + 1] >= 3 && energy[i + 1] > energy[i];
    // Ticks and milliseconds: a gate is a time in ms (the dry bass), so it is written at the section's tempo.
    const double oldBpm = bpmAt(before, a), newBpm = bpmAt(doc, a);
    const double msPerTick = msPerTickAt(newBpm), oldMsPerTick = msPerTickAt(oldBpm);
    const bool drop = i + 1 < doc.sections.size() && energy[i + 1] <= energy[i] - 2;
    std::vector<std::string> did;

    // F: phrase length, fill style, bars without kick before a lift or a drop.
    int every = 16;
    int cutBars = -1;  // -1: as the template
    std::string fillStyle;
    bool fillsEvery8 = false;  // said once, when a part really got them
    if (on("fills")) {
      Rng r = streamFor(o.seed, sec.id, "fills");
      if (r.chance(p * 0.6) && sec.lengthTicks >= 16 * BAR) every = 8;
      if (r.chance(p)) fillStyle = r.pick(std::vector<std::string>{"rolls", "ratchet", "build"});
      if ((lift || drop) && r.chance(p)) cutBars = 1 + r.below(2);
    }

    // D + F on the kick: a ghost on the last 16th; phrases and the pause before the next section.
    if (kick && !kickMain.empty() && energy[i] > 0) {
      const Span cov = coverage(*kick, a, b);
      Rng r = streamFor(o.seed, sec.id, "kick");
      const bool ghost = on("kick") && energy[i] >= 3 && r.chance(p * 0.6);
      if (cov.any() && (ghost || every != 16 || cutBars >= 0)) {
        const bool hadFill = !kickFill.empty() && usesPattern(*kick, a, b, kickFill);
        std::string groove = mainPatternIn(*kick, a, b);
        if (groove.empty()) groove = kickMain;
        if (ghost) {
          Pattern src = patternById(*kick, groove);
          std::vector<Note> notes = src.notes;
          const int64_t len = notes.empty() ? S16 : notes.front().length;
          const int pitch = notes.empty() ? 36 : notes.front().pitch;
          bool has15 = false;
          for (const auto& n : notes) has15 |= n.tick % BAR == 15 * S16;
          // Only ghosts inside the pattern: a 1.5-bar kick put one past its end and the song never opened again (sweep P1).
          if (!has15) for (int64_t bar = 0; bar + 15 * S16 < src.lengthTicks; bar += BAR) notes.push_back(note(bar + 15 * S16, std::min(len, S16), pitch, 0.45));
          groove = newPattern(*kick, src.name, " + ghost", src.lengthTicks, notes);
          did.push_back("ghost kick");
        }
        int64_t to = cov.to;
        if (cutBars >= 0 && (lift || drop)) {
          to = std::max(cov.from + BAR, b - cutBars * BAR);
          if (to != cov.to) did.push_back(std::to_string((b - to) / BAR) + ((b - to) == BAR ? " bar" : " bars") + " without kick before the next section");
        }
        clearSection(*kick, a, b);
        lay(*kick, a, cov.from, to, groove, hadFill ? kickFill : "", every);
        fillsEvery8 |= every == 8 && hadFill;
      }
    }

    // A + B: the bass grammar and its gate.
    bool bassRewritten = false;
    if (bass && (on("bass") || on("gate")) && energy[i] >= 2) {
      const Span cov = coverage(*bass, a, b);
      Rng ra = streamFor(o.seed, sec.id, "bass");
      Rng rb = streamFor(o.seed, sec.id, "gate");
      const bool newGrammar = on("bass") && ra.chance(p);
      const bool newGate = on("gate") && rb.chance(p);
      if (cov.any() && (newGrammar || newGate)) {
        bassRewritten = true;
        // What the section plays now: its main pattern's offsets in the beat and its note length.
        std::string g = "kbbb";
        int64_t gate = 144;
        if (const Pattern* cur = bass->findPattern(mainPatternIn(*bass, a, b))) {
          std::set<int64_t> offs;
          for (const auto& n : cur->notes) offs.insert(n.tick % kPpq);
          for (const char* name : {"kbbb", "kbb", "kb-b", "triplet", "offbeat"}) {
            const auto want = grammarOffsets(name);
            if (std::set<int64_t>(want.begin(), want.end()) == offs) g = name;
          }
          if (!cur->notes.empty()) gate = cur->notes.front().length;
        }
        if (newGrammar) g = ra.pick(card->grammars);
        const auto offs = grammarOffsets(g);
        if (newGate || newGrammar) {
          const double f = newGate ? rb.pick(card->gateFractions) : 0.6;
          const double ms = newGate ? card->gateMsLo + rb.unit() * (card->gateMsHi - card->gateMsLo) : gate * oldMsPerTick;
          gate = std::max<int64_t>(20, (int64_t)std::llround(std::min(f * (double)shortestGap(offs), ms / msPerTick)));
        }
        const std::vector<int> turn = newGrammar ? ra.pick(card->turns) : card->turns.front();
        const std::vector<int> alt = newGrammar ? ra.pick(card->alts) : card->alts.front();
        const std::vector<double> vel = {0.9, 0.72, 0.8};
        auto build = [&](const std::string& name, const std::vector<int>& pitches) {
          std::vector<Note> notes;
          for (size_t beat = 0; beat < pitches.size(); ++beat)
            for (size_t k = 0; k < offs.size(); ++k)
              notes.push_back(note((int64_t)beat * kPpq + offs[k], gate, root + pitches[beat], vel[k % vel.size()]));
          return newPattern(*bass, sec.name, " " + name + " (" + g + ")", 16 * kPpq, notes);
        };
        std::vector<int> plain(16, 0), withTurn(12, 0);
        withTurn.insert(withTurn.end(), turn.begin(), turn.end());
        const std::string b1 = build("roll", plain), b2 = build("turn", withTurn), b3 = build("second roll", alt);
        int64_t to = cov.to;
        if (cutBars >= 0 && (lift || drop)) to = std::max(cov.from + BAR, b - cutBars * BAR);
        clearSection(*bass, a, b);
        int k = 0;
        for (int64_t t = cov.from; t < to; t += 4 * BAR, ++k)
          addClip(*bass, k % 4 == 0 || k % 4 == 2 ? b1 : (k % 4 == 1 ? b2 : (energy[i] >= 4 ? b3 : b2)), t, std::min(4 * BAR, to - t));
        did.push_back("bass " + g + ", gate " + fmt(gate * msPerTick, "ms"));
      } else if (cov.any() && cutBars >= 0 && (lift || drop)) {
        // The bass keeps its notes but pauses with the kick before the next section.
        const int64_t cut = std::max(cov.from + BAR, b - cutBars * BAR);
        splitAt(*bass, cut);
        clearSection(*bass, cut, b);
      }
    }
    // J: a section at another tempo whose bass was not rewritten keeps its gate in ms.
    if (bass && newBpm != oldBpm && !bassRewritten) retimeBass(*bass, a, b, newBpm / oldBpm, did, oldMsPerTick, msPerTick);

    // C + F on the closed hats: grid, rolls, the fill.
    if (hats && !hatMain.empty() && energy[i] >= 2) {
      const Span cov = coverage(*hats, a, b);
      Rng r = streamFor(o.seed, sec.id, "hats");
      const bool newGrid = on("hats") && r.chance(p);
      if (cov.any() && (newGrid || every != 16 || !fillStyle.empty() || cutBars >= 0)) {
        const bool hadFill = !hatFill.empty() && usesPattern(*hats, a, b, hatFill);
        std::string groove = mainPatternIn(*hats, a, b);
        if (groove.empty()) groove = hatMain;
        const Pattern src = patternById(*hats, groove);
        int pitch = 42;
        int64_t len = 45;
        for (const auto& n : src.notes) if (!n.hasRoll) { pitch = n.pitch; len = n.length; break; }
        if (newGrid) {
          const std::string grid = r.pick(card->hatGrids);
          std::vector<int> steps;
          if (grid == "offbeat") steps = {2, 6, 10, 14};
          else for (int s = 0; s < 16; ++s) steps.push_back(s);
          if (grid == "holes") {
            const std::vector<std::vector<int>> holes = {{5, 13}, {3, 11}, {1, 9}, {5, 9, 13}};
            for (int h : r.pick(holes)) steps.erase(std::remove(steps.begin(), steps.end(), h), steps.end());
          }
          const std::vector<int> rolls = r.pick(card->hatRolls);
          const int count = r.pick(card->rollCounts);
          std::vector<Note> notes;
          for (int s : steps) {
            const bool accent = s % 4 == 2;
            const bool roll = std::find(rolls.begin(), rolls.end(), s) != rolls.end();
            Note n = note(s * S16, roll ? S16 : len, pitch, accent ? 0.55 : 0.33);
            if (roll) { n.hasRoll = true; n.roll.count = count; n.roll.velocityFrom = 0.35; n.roll.velocityTo = 0.6; }
            notes.push_back(n);
          }
          std::string rollText;
          for (int s : rolls) rollText += (rollText.empty() ? "" : "/") + std::to_string(s + 1);
          groove = newPattern(*hats, sec.name, " " + grid + (rolls.empty() ? "" : " + rolls"), BAR, notes);
          did.push_back("hats " + grid + (rolls.empty() ? "" : ", rolls on 16th " + rollText + " x" + std::to_string(count)));
        }
        std::string fill = hadFill ? hatFill : "";
        if (hadFill && !fillStyle.empty()) {
          std::vector<Note> notes;
          for (int s = 0; s < 16; ++s) {
            Note n = note(s * S16, 40, pitch, fillStyle == "build" ? 0.25 + 0.03 * s : (s % 4 == 0 ? 0.5 : 0.3));
            const bool roll = fillStyle == "rolls" ? s >= 14 : fillStyle == "ratchet" ? s >= 12 : s >= 15;
            if (roll) { n.length = S16; n.hasRoll = true; n.roll.count = fillStyle == "ratchet" ? 3 : 4; n.roll.velocityFrom = 0.3; n.roll.velocityTo = 0.75; }
            notes.push_back(n);
          }
          fill = newPattern(*hats, sec.name, " fill (" + fillStyle + ")", BAR, notes);
          did.push_back("hat fills " + fillStyle);
        }
        int64_t to = cov.to;
        if (cutBars >= 0 && (lift || drop)) to = std::max(cov.from + BAR, b - cutBars * BAR);
        clearSection(*hats, a, b);
        lay(*hats, a, cov.from, to, groove, fill, every);
        fillsEvery8 |= every == 8 && !fill.empty();
      }
    }

    // E: percussion, accents and open hats in or out (only in sections of energy 3 or more: the template's
    // breaks and intros keep their shape); the lead's entry.
    if (on("roles") && energy[i] >= 3) {
      Rng r = streamFor(o.seed, sec.id, "roles");
      for (const char* id : {"perc", "accents", "open-hats"}) {
        Track* t = findTrack(doc, id);
        if (!t || t->clips.empty() || !r.chance(p * 0.5)) continue;
        const Span cov = coverage(*t, a, b);
        if (cov.any() && energy[i] < 4) {
          clearSection(*t, a, b);
          did.push_back(t->name + " out");
        } else if (!cov.any()) {
          const std::string groove = mainPattern(*t, false);
          if (groove.empty()) continue;
          const int64_t to = lift || drop ? b - BAR : b;
          addClip(*t, groove, a, to - a);
          did.push_back(t->name + " in");
        }
      }
      if (Track* lead = findTrack(doc, "lead")) {
        const Span cov = coverage(*lead, a, b);
        if (cov.any() && cov.from == a && r.chance(p * 0.6)) {
          const int quarters = 1 + r.below(2);  // from a quarter or half of the section
          const int64_t entry = a + (sec.lengthTicks / 4 * quarters) / BAR * BAR;
          splitAt(*lead, entry);
          clearSection(*lead, a, entry);
          did.push_back("lead from bar " + std::to_string((entry - a) / BAR + 1));
        }
      }
    }

    // F: the riser into the next section: 2, 4 or 8 bars (never longer than its pattern), ending on the drop.
    if (on("fills") && (lift || drop)) {
      if (Track* riser = findTrack(doc, "riser")) {
        Rng r = streamFor(o.seed, sec.id, "riser");
        for (auto& c : riser->clips) {
          if (c.startTick + c.lengthTicks != b || !r.chance(p)) continue;
          const Pattern* pp = riser->findPattern(c.patternId);
          if (!pp) continue;
          std::vector<int64_t> lengths;
          for (int64_t bars : {2, 4, 8}) if (bars * BAR <= pp->lengthTicks && bars * BAR <= sec.lengthTicks) lengths.push_back(bars * BAR);
          if (lengths.empty()) continue;
          const int64_t len = r.pick(lengths);
          if (len == c.lengthTicks) continue;
          c.startTick = b - len;
          c.lengthTicks = len;
          c.offsetTicks = pp->lengthTicks - len;  // the end of the roll: the fast part stays on the drop
          did.push_back("riser " + std::to_string(len / BAR) + " bars");
        }
        std::sort(riser->clips.begin(), riser->clips.end(), [](const Clip& x, const Clip& y) { return x.startTick < y.startTick; });
      }
    }

    if (fillsEvery8) did.insert(did.begin(), "fills every 8 bars");
    if (!did.empty()) {
      std::string line = sec.name + ": ";
      for (size_t k = 0; k < did.size(); ++k) line += (k ? ", " : "") + did[k];
      changes.push_back(line);
    }
  }
  for (const char* id : parts)
    if (Track* t = (grooveOn || tempoOn) ? findTrack(doc, id) : nullptr) removeUnusedPatterns(*t);
  // K: the built-in kick, bass and hats get new values inside the style. Plugin tracks keep their patch.
  json sound = json::object();
  if (soundOn) {
    std::map<std::string, std::string> lines;
    for (const SoundRange& r : soundRanges(style)) {
      Track* t = findTrack(doc, r.track);
      if (!t || t->instrument.kind != "builtin" || t->instrument.name != r.instrument) continue;
      const InstrumentParamInfo* info = findInstrumentParam(*t, r.param);
      if (!info) continue;
      Rng rng = streamFor(o.seed, "song", std::string("sound:") + r.track + ":" + r.param);
      if (!rng.chance(p)) continue;
      double v = r.lo + rng.unit() * (r.hi - r.lo);
      const bool whole = *info->unit != 0 && std::string(info->unit) != "oct";  // Hz and ms in whole numbers
      v = whole ? std::round(v) : std::round(v * 100) / 100;
      v = std::clamp(v, info->min, info->max);
      setInstrumentParam(doc, r.track, r.param, v);
      sound[r.track][r.param] = v;
      std::string& line = lines[t->name];
      char text[64];
      std::snprintf(text, sizeof text, whole ? "%s %.0f%s%s" : "%s %.2f%s%s", info->label, v, *info->unit ? " " : "", info->unit);
      line += (line.empty() ? "" : ", ") + std::string(text);
    }
    if (lines.empty() && o.axes.size() == 1)
      throw DocumentError("sound", "this song's kick, bass and hats have no built-in sound to change (samples or audio): Randomize sound "
                                   "works on Core's own instruments");
    for (const auto& [name, line] : lines) changes.insert(changes.begin(), "Sound, " + name + ": " + line);
  }
  json allAxes = json::array();
  for (const auto& x : randomizeAxes()) if (x != "tempo" && x != "sound") allAxes.push_back(x);
  json recipe = {{"seed", o.seed}, {"strength", o.strength}, {"style", style}, {"axes", o.axes.empty() ? allAxes : json(o.axes)}};
  if (soundOn) recipe["sound"] = sound;
  if (tempoOn) {
    json map = json::array();
    for (const auto& t : doc.tempoMap) map.push_back(tempoPointJson(t));
    recipe["tempoMap"] = map;
    changes.insert(changes.begin(), tempoLines.begin(), tempoLines.end());
  }
  doc.extra["randomizer"] = recipe;
  return changes;
}

std::vector<std::string> setTempo(Document& doc, double bpm, std::vector<TempoPoint> changes) {
  if (!(bpm >= 40 && bpm <= 600)) throw DocumentError("bpm", "must be 40-600");
  Document next = doc;
  setTempoMap(next, std::move(changes));  // validates
  next.bpm = bpm;
  // The stretches between every old and new change; consecutive ones with the same new/old ratio join.
  std::set<int64_t> cuts = {0, next.lengthTicks};
  for (const auto& t : doc.tempoMap) cuts.insert(t.tick);
  for (const auto& t : next.tempoMap) cuts.insert(t.tick);
  struct Stretch { int64_t a, b; double from, to; };
  std::vector<Stretch> stretches;
  for (auto it = cuts.begin(); std::next(it) != cuts.end(); ++it) {
    const int64_t a = *it, b = *std::next(it);
    const double from = bpmAt(doc, a), to = bpmAt(next, a);
    if (!stretches.empty() && stretches.back().b == a && stretches.back().to / stretches.back().from == to / from) stretches.back().b = b;
    else stretches.push_back({a, b, from, to});
  }
  std::vector<std::string> did;
  const auto msPerTickAt = [](double b) { return 60000.0 / (b * kPpq); };
  for (auto& t : next.tracks) {
    const bool bass = t.id == "bass" || t.name.find("Bass") != std::string::npos || t.name.find("bass") != std::string::npos;
    if (!bass) continue;
    for (const auto& s : stretches)
      if (s.to != s.from) retimeBass(t, s.a, s.b, s.to / s.from, did, msPerTickAt(s.from), msPerTickAt(s.to));
  }
  doc = std::move(next);
  return did;
}

} // namespace core::doc
