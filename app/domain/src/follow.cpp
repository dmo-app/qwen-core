#include "follow.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <random>
#include "edit.hpp"

namespace core::doc {

namespace {

constexpr int64_t kBar = 4 * kPpq;
constexpr int64_t kMaxPatternTicks = 64 * kBar;

// A note where it sounds in the song.
struct SongNote {
  int64_t at = 0, length = 0;
  int pitch = 36;
  double velocity = 0.8;
  Note note;  // the note it comes from (its roll and extras stay when it is written back); a new note: none
  bool operator<(const SongNote& o) const { return at < o.at || (at == o.at && pitch < o.pitch); }
  bool operator==(const SongNote& o) const {
    return at == o.at && length == o.length && pitch == o.pitch && std::fabs(velocity - o.velocity) < 1e-9;
  }
};

// Draws from the generator's raw output, never from std:: distributions (the same seed gives the same notes on every
// build, as in pattern_tools).
struct Draw {
  std::mt19937_64 rng;
  explicit Draw(uint64_t seed) : rng(seed) {}
  double unit() { return (double)(rng() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
  int64_t within(int64_t r) { return r <= 0 ? 0 : (int64_t)std::llround((unit() * 2.0 - 1.0) * (double)r); }
};

double vel(double v) { return std::clamp(std::round(v * 100.0) / 100.0, 0.05, 1.0); }

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

const Clip* findClip(const Track& t, const std::string& id) {
  for (const auto& c : t.clips) if (c.id == id) return &c;
  return nullptr;
}
Clip* findClip(Track& t, const std::string& id) {
  for (auto& c : t.clips) if (c.id == id) return &c;
  return nullptr;
}

// Calls f(note, song tick, length cut at the clip's end) for every time a note of the clip sounds.
template <typename F>
void eachSounding(const Track& t, const Clip& c, F&& f) {
  const Pattern* p = t.findPattern(c.patternId);
  if (!p || p->lengthTicks <= 0) return;
  const int64_t end = c.startTick + c.lengthTicks;
  for (int64_t base = c.startTick - c.offsetTicks; base < end; base += p->lengthTicks)
    for (const auto& n : p->notes) {
      const int64_t at = base + n.tick;
      if (at < c.startTick || at >= end) continue;
      f(n, at, std::max<int64_t>(1, std::min(n.length, end - at)));
    }
}

int mostUsedPitch(const Track& t, const std::vector<SongNote>& near, int fallback) {
  std::map<int, int> uses;
  for (const auto& n : near) ++uses[n.pitch];
  if (uses.empty())
    for (const auto& p : t.patterns) for (const auto& n : p.notes) ++uses[n.pitch];
  int best = fallback, most = 0;
  for (const auto& [pitch, k] : uses) if (k > most) { best = pitch; most = k; }
  return best;
}

// The loudest note's velocity: what a follow writes stays the base of the next one (the same notes again).
double topVelocity(const std::vector<SongNote>& v, double fallback) {
  if (v.empty()) return fallback;
  double top = 0;
  for (const auto& n : v) top = std::max(top, n.velocity);
  return top;
}

// A track's notes as they sound in the song (lengths cut at their clip's end), edited zone by zone and written back to
// the clips once (commit). Writing every zone straight into the clips made a pattern and two clips per zone: a long
// clip with a stutter in every bar ran into the 64 patterns of a track (audit of main a3d9f14, F1).
struct Score {
  const Track& track;
  std::vector<SongNote> notes;                         // sorted
  std::vector<std::pair<int64_t, int64_t>> changed;   // the ranges written

  explicit Score(const Track& t) : track(t) {
    for (const auto& c : t.clips)
      eachSounding(t, c, [&](const Note& n, int64_t at, int64_t len) {
        SongNote s{at, len, n.pitch, n.velocity, n};
        notes.push_back(std::move(s));
      });
    std::stable_sort(notes.begin(), notes.end());
  }

  // What sounding() gives on the clips: the notes starting in [from, to), cut at `to`.
  std::vector<SongNote> in(int64_t from, int64_t to) const {
    std::vector<SongNote> out;
    for (const auto& n : notes)
      if (n.at >= from && n.at < to) {
        out.push_back(n);
        out.back().length = std::min(n.length, to - n.at);
      }
    return out;
  }

  // The track plays exactly `v` in [from, to). Returns the notes written plus the notes taken out (0: they were
  // already there).
  int write(int64_t from, int64_t to, std::vector<SongNote> v) {
    if (to <= from) return 0;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const SongNote& n) { return n.at < from || n.at >= to; }), v.end());
    for (auto& n : v) n.length = std::max<int64_t>(1, std::min(n.length, to - n.at));
    std::stable_sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(), [](const SongNote& a, const SongNote& b) { return a.at == b.at && a.pitch == b.pitch; }), v.end());
    const auto now = in(from, to);
    if (now == v) return 0;
    const int written = (int)(now.size() + v.size());
    // The clip that crosses `from` is cut there: its notes stop at the cut. The range written back starts at the
    // earliest of them, so the cut reaches the song too (sweep: it stayed in this score only).
    int64_t first = from;
    for (auto& n : notes)
      if (n.at < from && n.at + n.length > from) { n.length = from - n.at; first = std::min(first, n.at); }
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const SongNote& n) { return n.at >= from && n.at < to; }), notes.end());
    notes.insert(notes.end(), v.begin(), v.end());
    std::stable_sort(notes.begin(), notes.end());
    changed.push_back({first, to});
    return written;
  }

  // The ranges written into the track's clips. Few zones: each its own clip, as before. Many zones (a stutter in
  // every bar of a 160-bar clip): zones near each other share their clips, and a range longer than 64 bars is cut
  // into clips of at most 64 bars. The first way the track's limits (64 patterns, 256 clips) allow wins.
  void commit(Track& t) const {
    if (changed.empty()) return;
    auto spans = changed;
    std::sort(spans.begin(), spans.end());
    static const int64_t gaps[] = {0, 4 * kBar, 16 * kBar, 64 * kBar, INT64_MAX / 4};
    for (size_t g = 0; g < std::size(gaps); ++g) {
      std::vector<std::pair<int64_t, int64_t>> regions;
      for (const auto& s : spans) {
        if (!regions.empty() && s.first - regions.back().second <= gaps[g]) regions.back().second = std::max(regions.back().second, s.second);
        else regions.push_back(s);
      }
      const bool last = g + 1 == std::size(gaps);
      if (!last && regions.size() > 16) continue;  // a clip per zone is for a few zones only
      Track trial = t;
      try {
        for (const auto& [a, b] : regions) writeBack(trial, a, b);
        t = std::move(trial);
        return;
      } catch (const DocumentError&) {
        if (last) throw;
      }
    }
  }

 private:
  // The pattern a clip plays when it must play exactly `v` (ticks from its start, no loop): its own pattern changed
  // when no other clip plays it, else the same notes another pattern already has, else a new pattern.
  static std::string patternFor(Track& t, const std::string& current, int64_t length, std::vector<Note> v) {
    std::sort(v.begin(), v.end(), [](const Note& a, const Note& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
    long k = 1;
    for (auto& n : v) n.id = "n" + std::to_string(k++);
    if (v.size() > kMaxPatternNotes) throw DocumentError("follow", "a pattern of " + t.name + " would have more than 4096 notes");
    if (!current.empty() && t.clipsUsing(current) == 1) {
      Pattern& p = patternById(t, current);
      p.lengthTicks = length;
      p.notes = std::move(v);
      return current;
    }
    auto same = [&](const Pattern& p) {
      if (p.lengthTicks != length || p.notes.size() != v.size()) return false;
      for (size_t i = 0; i < v.size(); ++i) {
        const Note &a = p.notes[i], &b = v[i];
        if (a.tick != b.tick || a.length != b.length || a.pitch != b.pitch || std::fabs(a.velocity - b.velocity) > 1e-9 ||
            a.hasRoll != b.hasRoll || a.extra != b.extra)
          return false;
      }
      return true;
    };
    for (const auto& p : t.patterns) if (same(p) && t.clipsUsing(p.id) > 0) return p.id;
    const Pattern* old = current.empty() ? nullptr : t.findPattern(current);
    const std::string name = old ? old->name : std::string();
    const std::string pid = addPattern(t, std::max<int64_t>(length, kPpq / 8));
    Pattern& np = patternById(t, pid);
    if (!name.empty()) np.name = nameWithSuffix(name, "f", kMaxPatternNameBytes);
    np.notes = std::move(v);
    return pid;
  }

  // The clips play the score in [a, b): cut at the edges (and every 64 bars, between notes), each clip inside then
  // plays its part flat; the notes no clip covers go into new clips.
  void writeBack(Track& t, int64_t a, int64_t b) const {
    const auto part = in(a, b);
    std::vector<int64_t> edges = {a};
    for (int64_t s = a; b - s > kMaxPatternTicks;) {
      int64_t c = s + kMaxPatternTicks;
      for (bool moved = true; moved;) {
        moved = false;
        for (const auto& n : part)
          if (n.at > s && n.at < c && n.at + n.length > c) { c = n.at; moved = true; }
      }
      edges.push_back(c);
      s = c;
    }
    edges.push_back(b);
    for (int64_t edge : edges) {
      std::vector<std::string> crossing;
      for (const auto& c : t.clips) if (c.startTick < edge && c.startTick + c.lengthTicks > edge) crossing.push_back(c.id);
      for (const auto& id : crossing) splitClip(t, id, edge);
    }
    std::vector<std::string> inside;
    for (const auto& c : t.clips) if (c.startTick >= a && c.startTick + c.lengthTicks <= b) inside.push_back(c.id);
    std::map<std::string, std::vector<Note>> home;
    std::vector<SongNote> homeless;
    for (const auto& s : part) {
      const Clip* to = nullptr;
      for (const auto& id : inside)
        if (const Clip* c = findClip(t, id); c && s.at >= c->startTick && s.at < c->startTick + c->lengthTicks) { to = c; break; }
      if (!to) { homeless.push_back(s); continue; }
      Note n = s.note;
      n.tick = s.at - to->startTick;
      n.length = std::max<int64_t>(1, std::min(s.length, to->lengthTicks - n.tick));
      n.pitch = s.pitch;
      n.velocity = s.velocity;
      home[to->id].push_back(std::move(n));
    }
    for (const auto& id : inside) {
      const Clip c = *findClip(t, id);
      const std::string pid = patternFor(t, c.patternId, c.lengthTicks, home[id]);
      Clip* cc = findClip(t, id);
      cc->patternId = pid;
      cc->offsetTicks = 0;
    }
    if (homeless.empty()) return;
    // The gaps of [a, b) that no clip covers, each piece between two edges its own new clip.
    std::vector<std::pair<int64_t, int64_t>> covered;
    for (const auto& c : t.clips)
      if (c.startTick < b && c.startTick + c.lengthTicks > a) covered.push_back({std::max(a, c.startTick), std::min(b, c.startTick + c.lengthTicks)});
    std::sort(covered.begin(), covered.end());
    std::vector<std::pair<int64_t, int64_t>> holes;
    int64_t at = a;
    for (const auto& [x, y] : covered) { if (x > at) holes.push_back({at, x}); at = std::max(at, y); }
    if (at < b) holes.push_back({at, b});
    for (const auto& [x, y] : holes)
      for (size_t e = 0; e + 1 < edges.size(); ++e) {
        const int64_t s0 = std::max(x, edges[e]), s1 = std::min(y, edges[e + 1]);
        if (s0 >= s1) continue;
        std::vector<Note> v;
        for (const auto& s : homeless)
          if (s.at >= s0 && s.at < s1) {
            Note n = s.note;
            n.tick = s.at - s0;
            n.length = std::max<int64_t>(1, std::min(s.length, s1 - s.at));
            n.pitch = s.pitch;
            n.velocity = s.velocity;
            v.push_back(std::move(n));
          }
        if (v.empty()) continue;
        const std::string pid = patternFor(t, std::string(), s1 - s0, std::move(v));
        addClip(t, pid, s0, s1 - s0);
      }
  }
};


Track& notesTrack(Document& doc, const std::string& id) {
  Track& t = trackById(doc, id);
  if (!playsPatterns(t)) throw DocumentError("follow", t.name + " plays no notes");
  return t;
}

// Where a zone is written: wide enough for the offset and the looseness on both sides.
std::pair<int64_t, int64_t> writeSpan(const FollowZone& z, const FollowSettings& s) {
  return {std::max<int64_t>(0, z.start + std::min<int64_t>(0, s.offsetTicks) - s.looseTicks),
          z.end + std::max<int64_t>(0, s.offsetTicks) + s.looseTicks};
}

std::vector<SongNote> bassNotes(const Score& bass, const std::vector<int64_t>& kicks, int64_t from, int64_t to, const FollowSettings& s) {
  const auto before = bass.in(std::max<int64_t>(0, from - 4 * kBar), to);
  const int root = mostUsedPitch(bass.track, before, 36);
  double base = 0.8;
  {
    std::vector<SongNote> in;
    for (const auto& n : before) if (n.at >= from) in.push_back(n);
    base = topVelocity(in.empty() ? before : in, 0.8);
  }
  // KBBB accents after the kick (1 · .72 · .52 · .64 with the kick's 1 taken out): the first bass step leads.
  static const double accent[] = {1.0, 0.72 / 0.72, 0.52 / 0.72, 0.64 / 0.72};
  Draw draw(s.seed * 2654435761ULL + 17);
  std::vector<SongNote> out;
  for (size_t i = 0; i < kicks.size(); ++i) {
    const int64_t next = i + 1 < kicks.size() ? kicks[i + 1] : INT64_MAX;
    // A fast kick (a stutter, a roll: no room for a step before the next one): the bass plays with it, on the same
    // notes, so everything breaks and rolls together (Diogo's build-up rules, 2026-11-09).
    if (next != INT64_MAX && next - kicks[i] < s.grid + s.grid / 2) {
      const int64_t at = kicks[i] + s.offsetTicks + draw.within(s.looseTicks);
      if (at < from || at >= to) continue;
      int pitch = root;
      for (const auto& n : before) { if (n.at > kicks[i]) break; pitch = n.pitch; }
      const int64_t gap = std::max<int64_t>(1, next - at);
      out.push_back({at, std::max<int64_t>(1, (int64_t)std::llround(s.gate * (double)gap)), pitch, vel(base)});
      continue;
    }
    for (int k = 1; k <= s.afterKick; ++k) {
      const int64_t step = kicks[i] + k * s.grid;
      if (step > next - s.grid / 2) break;  // never on (or just before) the next kick
      int64_t at = step + s.offsetTicks + draw.within(s.looseTicks);
      if (at < from || at >= to) continue;
      int pitch = root;
      for (const auto& n : before) { if (n.at > step) break; pitch = n.pitch; }
      const int64_t room = std::min<int64_t>(s.grid, (next == INT64_MAX ? INT64_MAX / 2 : next) - at);
      out.push_back({at, std::max<int64_t>(1, (int64_t)std::llround(s.gate * (double)std::max<int64_t>(1, room))), pitch,
                     vel(base * accent[std::min(k, 3)])});
    }
  }
  return out;
}

}  // namespace

void checkFollow(const FollowSettings& s) {
  if (s.offsetTicks < -kPpq / 4 || s.offsetTicks > kPpq / 4) throw DocumentError("follow", "the offset must be within a 1/16 (-240..240 ticks)");
  if (s.looseTicks < 0 || s.looseTicks > kPpq / 8) throw DocumentError("follow", "the looseness must be 0..120 ticks (a 1/32)");
  if (s.grid < kPpq / 16 || s.grid > kPpq) throw DocumentError("follow", "the bass step must be 1/64 to 1/4");
  if (s.afterKick < 1 || s.afterKick > 15) throw DocumentError("follow", "notes after each kick: 1 to 15");
  if (!(s.gate >= 0.05 && s.gate <= 1.0)) throw DocumentError("follow", "the gate must be 0.05..1");
}

std::vector<int64_t> songOnsets(const Document& doc, const std::string& trackId, int64_t from, int64_t to, bool stutterOnly) {
  std::vector<int64_t> out;
  const Track* t = doc.findTrack(trackId);
  if (!t || !playsPatterns(*t)) return out;
  const bool whole = to <= from;
  for (const auto& c : t->clips)
    eachSounding(*t, c, [&](const Note& n, int64_t at, int64_t) {
      if (stutterOnly && !n.extra.contains("stutter")) return;
      if (whole || (at >= from && at < to)) out.push_back(at);
    });
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<int64_t> songOnsetsOfNotes(const Document& doc, const std::string& trackId, const std::string& patternId,
                                       const std::set<std::string>& noteIds) {
  std::vector<int64_t> out;
  const Track* t = doc.findTrack(trackId);
  if (!t) return out;
  for (const auto& c : t->clips)
    if (c.patternId == patternId)
      eachSounding(*t, c, [&](const Note& n, int64_t at, int64_t) { if (noteIds.count(n.id)) out.push_back(at); });
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<std::pair<int64_t, int64_t>> songRangesOfPattern(const Document& doc, const std::string& trackId,
                                                              const std::string& patternId, int64_t from, int64_t to) {
  std::vector<std::pair<int64_t, int64_t>> out;
  const Track* t = doc.findTrack(trackId);
  const Pattern* p = t ? t->findPattern(patternId) : nullptr;
  if (!p || p->lengthTicks <= 0 || to <= from) return out;
  for (const auto& c : t->clips) {
    if (c.patternId != patternId) continue;
    const int64_t end = c.startTick + c.lengthTicks;
    for (int64_t base = c.startTick - c.offsetTicks; base < end; base += p->lengthTicks) {
      const int64_t a = std::max(c.startTick, base + from), b = std::min(end, base + to);
      if (a < b) out.push_back({a, b});
    }
  }
  std::sort(out.begin(), out.end());
  std::vector<std::pair<int64_t, int64_t>> merged;
  for (const auto& r : out) {
    if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
    else merged.push_back(r);
  }
  return merged;
}

std::vector<int64_t> stutterOnsets(const Document& doc, const std::string& except, int64_t from, int64_t to) {
  std::vector<int64_t> out;
  for (const auto& t : doc.tracks) {
    if (t.id == except) continue;
    const auto v = songOnsets(doc, t.id, from, to, true);
    out.insert(out.end(), v.begin(), v.end());
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<FollowZone> followZones(std::vector<int64_t> onsets) {
  std::sort(onsets.begin(), onsets.end());
  onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());
  std::vector<FollowZone> zones;
  for (size_t i = 0; i < onsets.size(); ++i) {
    if (zones.empty() || onsets[i] - zones.back().onsets.back() > kPpq) zones.push_back({onsets[i], onsets[i], {}});
    zones.back().onsets.push_back(onsets[i]);
  }
  for (auto& z : zones) {
    const auto& o = z.onsets;
    const int64_t step = o.size() > 1 ? std::min<int64_t>(o.back() - o[o.size() - 2], kPpq / 4) : kPpq / 4;
    z.end = o.back() + std::max<int64_t>(1, step);
  }
  return zones;
}

namespace {

int kickInto(Score& kick, const std::vector<FollowZone>& zones, const FollowSettings& s) {
  const auto all = kick.in(0, INT64_MAX / 4);
  const int pitch = mostUsedPitch(kick.track, all, 36);
  const double base = vel(topVelocity(all, 0.9));
  Draw draw(s.seed);
  int written = 0;
  for (const auto& z : zones) {
    const auto [from, to] = writeSpan(z, s);
    std::vector<SongNote> notes;
    for (size_t i = 0; i < z.onsets.size(); ++i) {
      const int64_t at = std::clamp<int64_t>(z.onsets[i] + s.offsetTicks + draw.within(s.looseTicks), from, to - 1);
      const int64_t gap = i + 1 < z.onsets.size() ? z.onsets[i + 1] - z.onsets[i] : z.end - z.onsets[i];
      notes.push_back({at, std::max<int64_t>(1, (int64_t)std::llround(s.gate * (double)std::min<int64_t>(gap, kPpq / 2))), pitch, base});
    }
    // Only the zone itself is cleared: the kick's notes in the margins the offset needs (the drop's first kick) stay.
    for (const auto& n : kick.in(from, z.start)) notes.push_back(n);
    for (const auto& n : kick.in(z.end, to)) notes.push_back(n);
    written += kick.write(from, to, std::move(notes));
  }
  return written;
}

int bassInto(Score& bass, const Document& doc, const std::string& kickTrackId, int64_t from, int64_t to, const FollowSettings& s) {
  if (to <= from) return 0;
  const auto kicks = songOnsets(doc, kickTrackId, std::max<int64_t>(0, from - kPpq), to);
  return bass.write(from, to, bassNotes(bass, kicks, from, to, s));
}

}  // namespace

int kickFollow(Document& doc, const std::string& kickTrackId, const std::vector<FollowZone>& zones, const FollowSettings& s) {
  checkFollow(s);
  Track& kick = notesTrack(doc, kickTrackId);
  Score score(kick);
  const int written = kickInto(score, zones, s);
  score.commit(kick);
  return written;
}

int bassFollow(Document& doc, const std::string& bassTrackId, const std::string& kickTrackId, int64_t from, int64_t to,
               const FollowSettings& s) {
  checkFollow(s);
  if (bassTrackId == kickTrackId) throw DocumentError("follow", "the bass cannot follow itself");
  notesTrack(doc, kickTrackId);
  Track& bass = notesTrack(doc, bassTrackId);
  Score score(bass);
  const int written = bassInto(score, doc, kickTrackId, from, to, s);
  score.commit(bass);
  return written;
}

int bassFollowZones(Document& doc, const std::string& bassTrackId, const std::string& kickTrackId,
                    const std::vector<FollowZone>& zones, const FollowSettings& s) {
  checkFollow(s);
  if (bassTrackId == kickTrackId) throw DocumentError("follow", "the bass cannot follow itself");
  notesTrack(doc, kickTrackId);
  Track& bass = notesTrack(doc, bassTrackId);
  Score score(bass);
  int written = 0;
  for (const auto& z : zones) {
    const auto [from, to] = writeSpan(z, s);
    written += bassInto(score, doc, kickTrackId, from, to, s);
  }
  score.commit(bass);
  return written;
}

std::string findKickTrack(const Document& doc) {
  for (const auto& t : doc.tracks)
    if (t.instrument.kind == "builtin" && t.instrument.name == "kick") return t.id;
  for (const auto& t : doc.tracks)
    if (playsPatterns(t) && lower(t.name).find("kick") != std::string::npos) return t.id;
  return {};
}

std::string findBassTrack(const Document& doc) {
  for (const char* word : {"bass", "kbbb", "baixo"})
    for (const auto& t : doc.tracks)
      if (playsPatterns(t) && lower(t.name).find(word) != std::string::npos) return t.id;
  return {};
}

// ------------------------------------------------------------------------------------------------- on ("ligado")
void setFollowLink(Document& doc, const std::string& trackId, const FollowLink& link) {
  checkFollow(link.settings);
  Track& t = notesTrack(doc, trackId);
  if (link.mode != "stutter" && link.mode != "kick") throw DocumentError("follow", "follow mode must be stutter or kick");
  if (!link.source.empty()) {
    if (link.source == trackId) throw DocumentError("follow", "a track cannot follow itself");
    notesTrack(doc, link.source);
  }
  json j = {{"mode", link.mode}, {"offset", link.settings.offsetTicks}, {"loose", link.settings.looseTicks},
            {"seed", link.settings.seed}};
  if (!link.source.empty()) j["source"] = link.source;
  trackById(doc, trackId).extra["follow"] = j;
  (void)t;
}

void removeFollowLink(Document& doc, const std::string& trackId) { trackById(doc, trackId).extra.erase("follow"); }

bool followLink(const Track& track, FollowLink* out) {
  if (!track.extra.is_object() || !track.extra.contains("follow")) return false;
  const json& j = track.extra["follow"];
  if (!j.is_object()) return false;
  FollowLink l;
  l.mode = j.value("mode", std::string());
  if (l.mode != "stutter" && l.mode != "kick") return false;
  l.source = j.value("source", std::string());
  if (j.contains("offset") && j["offset"].is_number()) l.settings.offsetTicks = std::clamp<int64_t>(j["offset"].get<int64_t>(), -kPpq / 4, kPpq / 4);
  if (j.contains("loose") && j["loose"].is_number()) l.settings.looseTicks = std::clamp<int64_t>(j["loose"].get<int64_t>(), 0, kPpq / 8);
  if (j.contains("seed") && j["seed"].is_number_unsigned()) l.settings.seed = j["seed"].get<uint64_t>();
  if (out) *out = l;
  return true;
}

int refreshFollows(Document& doc) {
  std::vector<std::pair<std::string, FollowLink>> kicks, basses;
  for (const auto& t : doc.tracks) {
    FollowLink l;
    if (!playsPatterns(t) || !followLink(t, &l)) continue;
    (l.mode == "stutter" ? kicks : basses).push_back({t.id, l});
  }
  if (kicks.empty() && basses.empty()) return 0;
  int changed = 0;
  for (const auto& [id, l] : kicks) {
    const auto zones = followZones(l.source.empty() ? stutterOnsets(doc, id, 0, 0) : songOnsets(doc, l.source, 0, 0, true));
    Track& kt = trackById(doc, id);
    Score score(kt);
    if (kickInto(score, zones, l.settings) > 0) ++changed;
    score.commit(kt);
  }
  for (const auto& [id, l] : basses) {
    const std::string kick = l.source.empty() ? findKickTrack(doc) : l.source;
    if (kick.empty() || kick == id || !doc.findTrack(kick)) continue;
    // The bass follows its kick inside the stutter zones (of every track but the bass and the kick), as wide as the
    // kick's own follow writes them.
    std::vector<int64_t> st;
    for (const auto& t : doc.tracks) {
      if (t.id == id || t.id == kick) continue;
      const auto v = songOnsets(doc, t.id, 0, 0, true);
      st.insert(st.end(), v.begin(), v.end());
    }
    FollowSettings wide = l.settings;
    FollowLink kl;
    if (const Track* kt = doc.findTrack(kick); kt && followLink(*kt, &kl)) {
      wide.offsetTicks = kl.settings.offsetTicks;
      wide.looseTicks = kl.settings.looseTicks;
    }
    Track& bt = trackById(doc, id);
    Score score(bt);
    int n = 0;
    for (const auto& z : followZones(st)) {
      const auto [from, to] = writeSpan(z, wide);
      n += bassInto(score, doc, kick, from, to, l.settings);
    }
    if (n > 0) ++changed;
    score.commit(bt);
  }
  return changed;
}

}  // namespace core::doc
