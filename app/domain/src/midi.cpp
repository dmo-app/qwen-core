#include "midi.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include "edit.hpp"

namespace core::doc {

namespace {
constexpr int64_t kBarTicks = 4 * kPpq;
constexpr int64_t kSongTicksMax = 4096LL * kBarTicks;

// ---- reading
struct Reader {
  const std::vector<uint8_t>& b;
  size_t at = 0, end = 0;
  uint8_t byte() {
    if (at >= end) throw DocumentError("midi", "the file is cut short");
    return b[at++];
  }
  uint32_t be(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | byte();
    return v;
  }
  uint32_t varlen() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t c = byte();
      v = (v << 7) | (c & 0x7F);
      if (!(c & 0x80)) return v;
    }
    throw DocumentError("midi", "a length is too long");
  }
  void skip(size_t n) {
    if (n > end - at) throw DocumentError("midi", "the file is cut short");
    at += n;
  }
};

// One MTrk: its notes (on/off paired per channel and pitch, first in first out), its name and its tempos.
void readTrack(Reader& r, std::vector<MidiTrack>& out, std::vector<MidiTempo>& tempos, bool splitChannels) {
  int64_t tick = 0;
  uint8_t status = 0;
  std::string name;
  std::vector<MidiNote> notes;
  std::map<int, std::vector<size_t>> open;  // channel * 128 + pitch -> notes still held
  auto close = [&](int channel, int pitch, int64_t at) {
    auto it = open.find(channel * 128 + pitch);
    if (it == open.end() || it->second.empty()) return;
    MidiNote& n = notes[it->second.front()];
    n.length = std::max<int64_t>(1, at - n.tick);
    it->second.erase(it->second.begin());
  };
  while (r.at < r.end) {
    tick += r.varlen();
    uint8_t first = r.byte();
    if (first == 0xFF) {
      const uint8_t type = r.byte();
      const uint32_t len = r.varlen();
      if (type == 0x2F) break;
      if (type == 0x03 && name.empty()) {
        if (len > r.end - r.at) throw DocumentError("midi", "the file is cut short");
        name.assign(r.b.begin() + (long)r.at, r.b.begin() + (long)(r.at + len));
        r.skip(len);
      } else if (type == 0x51 && len == 3) {
        const uint32_t us = r.be(3);
        if (us > 0) tempos.push_back({tick, 60000000.0 / us});
      } else {
        r.skip(len);
      }
      continue;
    }
    if (first == 0xF0 || first == 0xF7) { r.skip(r.varlen()); status = 0; continue; }
    uint8_t data1;
    if (first & 0x80) { status = first; data1 = r.byte(); }
    else {
      if (!status) throw DocumentError("midi", "data before any status byte");
      data1 = first;
    }
    const int kind = status & 0xF0, channel = status & 0x0F;
    const bool two = kind != 0xC0 && kind != 0xD0;
    const uint8_t data2 = two ? r.byte() : 0;
    if (kind == 0x90 && data2 > 0) {
      notes.push_back({tick, 0, data1 & 0x7F, data2 & 0x7F, channel});
      open[channel * 128 + (data1 & 0x7F)].push_back(notes.size() - 1);
    } else if (kind == 0x80 || (kind == 0x90 && data2 == 0)) {
      close(channel, data1 & 0x7F, tick);
    }
  }
  for (auto& [key, list] : open)  // never released: they last to the end of the track
    for (size_t i : list) notes[i].length = std::max<int64_t>(1, tick - notes[i].tick);
  std::stable_sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
  if (!splitChannels) { out.push_back({name, std::move(notes)}); return; }
  std::map<int, std::vector<MidiNote>> byChannel;
  for (const auto& n : notes) byChannel[n.channel].push_back(n);
  if (byChannel.size() <= 1) { out.push_back({name, std::move(notes)}); return; }
  for (auto& [ch, list] : byChannel)
    out.push_back({(name.empty() ? std::string("MIDI") : name) + " ch " + std::to_string(ch + 1), std::move(list)});
}

// ---- writing
void put32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s)); }
void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
void putVar(std::vector<uint8_t>& o, uint64_t v) {
  v = std::min<uint64_t>(v, 0x0FFFFFFF);
  uint8_t buf[4];
  int n = 0;
  buf[n++] = (uint8_t)(v & 0x7F);
  while (v >>= 7) buf[n++] = (uint8_t)((v & 0x7F) | 0x80);
  while (n) o.push_back(buf[--n]);
}
struct Event { int64_t tick; int order; std::vector<uint8_t> bytes; };  // order: meta 0, off 1, on 2 (offs first)
void chunk(std::vector<uint8_t>& out, std::vector<Event> events) {
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick || (a.tick == b.tick && a.order < b.order); });
  std::vector<uint8_t> body;
  int64_t last = 0;
  for (const auto& e : events) {
    putVar(body, (uint64_t)std::max<int64_t>(0, e.tick - last));
    last = std::max(last, e.tick);
    body.insert(body.end(), e.bytes.begin(), e.bytes.end());
  }
  body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
  out.insert(out.end(), {'M', 'T', 'r', 'k'});
  put32(out, (uint32_t)body.size());
  out.insert(out.end(), body.begin(), body.end());
}
Event meta(int64_t tick, uint8_t type, const std::vector<uint8_t>& data) {
  Event e{tick, 0, {0xFF, type}};
  putVar(e.bytes, data.size());
  e.bytes.insert(e.bytes.end(), data.begin(), data.end());
  return e;
}
Event tempoEvent(int64_t tick, double bpm) {
  const uint32_t us = (uint32_t)std::clamp(std::llround(60000000.0 / std::max(1.0, bpm)), 1LL, 0xFFFFFFLL);
  return meta(tick, 0x51, {(uint8_t)(us >> 16), (uint8_t)(us >> 8), (uint8_t)us});
}
int velocity127(double v) { return std::clamp((int)std::lround(v * 127.0), 1, 127); }
}  // namespace

MidiFile parseMidi(const std::vector<uint8_t>& bytes) {
  Reader r{bytes, 0, bytes.size()};
  if (bytes.size() < 14 || bytes[0] != 'M' || bytes[1] != 'T' || bytes[2] != 'h' || bytes[3] != 'd') throw DocumentError("midi", "not a MIDI file");
  r.at = 4;
  const uint32_t headerLength = r.be(4);
  if (headerLength < 6) throw DocumentError("midi", "the header is too short");
  const size_t headerEnd = r.at + headerLength;
  const int format = (int)r.be(2), count = (int)r.be(2), division = (int)r.be(2);
  if (division & 0x8000) throw DocumentError("midi", "SMPTE time is not supported (only ticks per quarter)");
  if (division <= 0) throw DocumentError("midi", "no ticks per quarter");
  if (format > 2) throw DocumentError("midi", "unknown format " + std::to_string(format));
  r.at = headerEnd;
  MidiFile f;
  f.ppq = division;
  int read = 0;  // the MTrk chunks really read (an unknown chunk is not one, and the file can run short)
  for (int i = 0; i < count && r.at + 8 <= bytes.size(); ++i) {
    const bool isTrack = bytes[r.at] == 'M' && bytes[r.at + 1] == 'T' && bytes[r.at + 2] == 'r' && bytes[r.at + 3] == 'k';
    r.at += 4;
    const uint32_t length = r.be(4);
    if (length > bytes.size() - r.at) throw DocumentError("midi", "the file is cut short");
    const size_t next = r.at + length;
    if (isTrack) {
      Reader tr{bytes, r.at, next};
      readTrack(tr, f.tracks, f.tempos, format == 0);
      ++read;
    } else {
      --i;  // an unknown chunk does not count as a track
    }
    r.at = next;
  }
  // A file whose header promises more tracks than its bytes hold is cut short: say so, do not hand back a
  // shorter list as if it were the whole song (audit 2026-11-17, #2; midi.hpp promises DocumentError).
  // The tracks that play no notes are dropped below, so it is the chunks that are counted, not f.tracks.
  if (read < count)
    throw DocumentError("midi", "the file is cut short: " + std::to_string(read) + " of " + std::to_string(count) + " tracks");
  std::stable_sort(f.tempos.begin(), f.tempos.end(), [](const MidiTempo& a, const MidiTempo& b) { return a.tick < b.tick; });
  // Only the tracks that play notes (the tempo track, lyrics, empty ones go).
  f.tracks.erase(std::remove_if(f.tracks.begin(), f.tracks.end(), [](const MidiTrack& t) { return t.notes.empty(); }), f.tracks.end());
  return f;
}

std::vector<uint8_t> writeMidi(const MidiFile& file) {
  std::vector<uint8_t> out = {'M', 'T', 'h', 'd'};
  put32(out, 6);
  put16(out, 1);
  put16(out, (uint32_t)(file.tracks.size() + 1));
  put16(out, (uint32_t)std::clamp(file.ppq, 1, 0x7FFF));
  std::vector<Event> conductor = {meta(0, 0x58, {4, 2, 24, 8})};
  if (file.tempos.empty() || file.tempos.front().tick > 0) conductor.push_back(tempoEvent(0, 120));
  for (const auto& t : file.tempos) conductor.push_back(tempoEvent(t.tick, t.bpm));
  chunk(out, std::move(conductor));
  for (const auto& t : file.tracks) {
    std::vector<Event> events;
    if (!t.name.empty()) events.push_back(meta(0, 0x03, std::vector<uint8_t>(t.name.begin(), t.name.end())));
    for (const auto& n : t.notes) {
      const uint8_t ch = (uint8_t)(n.channel & 0x0F);
      events.push_back({n.tick, 2, {(uint8_t)(0x90 | ch), (uint8_t)(n.pitch & 0x7F), (uint8_t)std::clamp(n.velocity, 1, 127)}});
      events.push_back({n.tick + std::max<int64_t>(1, n.length), 1, {(uint8_t)(0x80 | ch), (uint8_t)(n.pitch & 0x7F), 0}});
    }
    chunk(out, std::move(events));
  }
  return out;
}

MidiFile songToMidi(const Document& doc, const MidiExportOptions& options) {
  MidiFile f;
  f.ppq = kPpq;
  // The tempo: the song's, then each change; a glide goes in 1/16 steps (each step at the tempo of its middle).
  f.tempos.push_back({0, doc.bpm});
  int64_t prevTick = 0;
  double prevBpm = doc.bpm;
  for (const auto& t : doc.tempoMap) {
    if (t.ramp && t.tick > prevTick) {
      const int64_t step = kPpq / 4;
      for (int64_t at = prevTick; at < t.tick; at += step) {
        const double mid = (double)(std::min(at + step, t.tick) + at) / 2.0;
        const double bpm = prevBpm + (t.bpm - prevBpm) * (mid - (double)prevTick) / (double)(t.tick - prevTick);
        if (at == 0) f.tempos.front().bpm = bpm;
        else f.tempos.push_back({at, bpm});
      }
    }
    f.tempos.push_back({t.tick, t.bpm});
    prevTick = t.tick;
    prevBpm = t.bpm;
  }
  const int64_t songEnd = doc.lengthTicks;
  for (const auto& t : doc.tracks) {
    if (t.instrument.kind == "audio" || t.instrument.kind == "bus") continue;
    if (t.mix.mute && !options.mutedTracks) continue;
    if (!options.onlyTracks.empty() && std::find(options.onlyTracks.begin(), options.onlyTracks.end(), t.id) == options.onlyTracks.end()) continue;
    MidiTrack mt;
    mt.name = t.name;
    for (const auto& c : t.clips) {
      const Pattern* p = t.findPattern(c.patternId);
      if (!p || p->lengthTicks <= 0 || c.startTick >= songEnd) continue;
      const int64_t clipEnd = std::min(c.startTick + c.lengthTicks, songEnd);
      const int64_t origin = c.startTick - c.offsetTicks % p->lengthTicks;
      for (int64_t k = 0; origin + k * p->lengthTicks < clipEnd; ++k) {
        const int64_t base = origin + k * p->lengthTicks;
        for (const auto& n : p->notes) {
          // As the engine plays it (graph.cpp): a roll's hits spread evenly, each 85% of its step.
          const int count = std::max(1, n.roll.count);
          for (int i = 0; i < count; ++i) {
            const int64_t on = n.tick + (int64_t)std::llround((double)n.length * i / count);
            const int64_t next = n.tick + (int64_t)std::llround((double)n.length * (i + 1) / count);
            const int64_t off = count > 1 ? on + std::max<int64_t>(1, (int64_t)std::llround((double)(next - on) * 0.85)) : n.tick + n.length;
            const double vel = count > 1 ? n.roll.velocityFrom + (n.roll.velocityTo - n.roll.velocityFrom) * (double)i / (count - 1) : n.velocity;
            const int64_t songOn = base + on, songOff = std::min(base + off, songEnd);
            if (songOn < c.startTick || songOn >= clipEnd || songOn >= songEnd || songOff <= songOn) continue;
            mt.notes.push_back({songOn, songOff - songOn, n.pitch, velocity127(std::min(1.0, vel)), 0});
          }
        }
      }
    }
    std::stable_sort(mt.notes.begin(), mt.notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
    f.tracks.push_back(std::move(mt));
  }
  return f;
}

std::vector<uint8_t> exportMidi(const Document& doc, const MidiExportOptions& options) { return writeMidi(songToMidi(doc, options)); }

void saveMidiFile(const std::vector<uint8_t>& bytes, const std::filesystem::path& file) {
  std::filesystem::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    if (!out) throw DocumentError("midi", "cannot write " + file.filename().string());
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (ec) { std::filesystem::remove(tmp, ec); throw DocumentError("midi", "cannot write " + file.filename().string()); }
}

std::vector<uint8_t> readMidiFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw DocumentError("midi", "cannot read " + file.filename().string());
  in.seekg(0, std::ios::end);
  const auto size = (size_t)in.tellg();
  if (size > 64u * 1024 * 1024) throw DocumentError("midi", "the file is too big");
  in.seekg(0);
  std::vector<uint8_t> bytes(size);
  in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)size);
  return bytes;
}

namespace {
// A track name from a file: valid UTF-8 (a byte that is not is read as Latin-1, as old MIDI files write names), no
// control characters, at most 80 bytes and never cut inside a character (a bad name made the song unsavable, D4/D6).
std::string cleanName(const std::string& raw) {
  std::string out;
  for (size_t i = 0; i < raw.size();) {
    const unsigned char c = (unsigned char)raw[i];
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    bool valid = len > 0 && i + len <= raw.size() && !(len == 2 && c < 0xC2);
    for (size_t k = 1; valid && k < len; ++k) valid = ((unsigned char)raw[i + k] & 0xC0) == 0x80;
    std::string piece;
    if (!valid) { piece = {(char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F))}; len = 1; }  // Latin-1
    else if (len == 1 && (c < 0x20 || c == 0x7F)) piece = " ";
    else piece = raw.substr(i, len);
    if (out.size() + piece.size() > 80) break;
    out += piece;
    i += len;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out.empty() ? std::string("MIDI") : out;
}
int64_t scaled(int64_t tick, int ppq) { return (int64_t)std::llround((double)tick * kPpq / std::max(1, ppq)); }
}  // namespace

MidiImportResult importMidi(Document& doc, const MidiFile& file, const MidiImportOptions& options) {
  if (options.barsPerPattern < 1 || options.barsPerPattern > 64) throw DocumentError("midi", "a pattern is 1 to 64 bars");
  if (options.atTick < 0 || options.atTick >= kSongTicksMax) throw DocumentError("midi", "the start must be inside the song");
  std::vector<const MidiTrack*> sources;
  for (const auto& t : file.tracks) if (!t.notes.empty()) sources.push_back(&t);
  if (sources.empty()) throw DocumentError("midi", "the file has no notes");
  if (doc.tracks.size() + sources.size() > 128) throw DocumentError("tracks", "the file's " + std::to_string(sources.size()) + " tracks do not fit (128 at most)");
  const int64_t chunkTicks = (int64_t)options.barsPerPattern * kBarTicks;
  Document next = doc;  // everything is checked on a copy: an error changes nothing
  MidiImportResult result;
  int64_t songEnd = next.lengthTicks;
  int number = 0;
  for (const MidiTrack* src : sources) {
    ++number;
    // Each chunk of bars: its notes, from the chunk's start.
    std::map<int64_t, std::vector<Note>> chunks;
    for (const auto& n : src->notes) {
      const int64_t tick = scaled(n.tick, file.ppq);
      const int64_t index = tick / chunkTicks;
      Note note;
      note.tick = tick - index * chunkTicks;
      note.length = std::clamp<int64_t>(scaled(n.length, file.ppq), 1, chunkTicks);
      note.pitch = std::clamp(n.pitch, 0, 127);
      note.velocity = std::clamp(n.velocity, 1, 127) / 127.0;
      chunks[index].push_back(note);
    }
    const std::string name = cleanName(options.namePrefix + (src->name.empty() ? "MIDI " + std::to_string(number) : src->name));
    const std::string id = addTrack(next, "", name);
    Track& t = trackById(next, id);
    t.patterns.clear();
    t.clips.clear();
    std::vector<std::pair<std::string, std::string>> seen;  // a chunk's notes (as text) -> its pattern
    for (auto& [index, notes] : chunks) {
      if (notes.size() > kMaxPatternNotes) throw DocumentError("midi", "'" + name + "' has more than " + std::to_string(kMaxPatternNotes) + " notes in " + std::to_string(options.barsPerPattern) + " bars");
      std::string key;
      for (const auto& n : notes) key += std::to_string(n.tick) + "," + std::to_string(n.length) + "," + std::to_string(n.pitch) + "," + std::to_string(n.velocity) + ";";
      std::string patternId;
      for (const auto& [k, pid] : seen) if (k == key) patternId = pid;
      if (patternId.empty()) {
        Pattern p;
        p.id = "p" + std::to_string(t.patterns.size() + 1);
        p.name = std::to_string(t.patterns.size() + 1);
        p.lengthTicks = chunkTicks;
        for (size_t i = 0; i < notes.size(); ++i) { notes[i].id = "n" + std::to_string(i + 1); p.notes.push_back(notes[i]); }
        t.patterns.push_back(std::move(p));
        patternId = t.patterns.back().id;
        seen.push_back({key, patternId});
        ++result.patterns;
      }
      const int64_t start = options.atTick + index * chunkTicks;
      if (start + chunkTicks > kSongTicksMax) throw DocumentError("midi", "the file goes past the longest song (4096 bars)");
      t.clips.push_back({"c" + std::to_string(t.clips.size() + 1), patternId, start, chunkTicks, 0, json::object()});
      songEnd = std::max(songEnd, start + chunkTicks);
      ++result.clips;
      result.notes += (int)notes.size();
    }
    result.trackIds.push_back(id);
  }
  if (songEnd > next.lengthTicks) {
    // The song grows to the end of the last bar holding a clip.
    next.lengthTicks = (songEnd + kBarTicks - 1) / kBarTicks * kBarTicks;
    result.songLengthened = true;
  }
  if (options.useTempo && !file.tempos.empty()) next.bpm = std::clamp(file.tempos.front().bpm, 40.0, 600.0);  // the song's range (document.cpp)
  (void)parseDocument(toJson(next));  // the song must still be valid
  doc = std::move(next);
  return result;
}

int importMidiIntoPattern(Pattern& pattern, const MidiFile& file, int64_t patternTick, int trackIndex) {
  if (trackIndex >= (int)file.tracks.size()) throw DocumentError("midi", "the file has no track " + std::to_string(trackIndex + 1));
  std::vector<Note> add;
  for (size_t ti = 0; ti < file.tracks.size(); ++ti) {
    if (trackIndex >= 0 && (int)ti != trackIndex) continue;
    for (const auto& n : file.tracks[ti].notes) {
      Note note;
      note.tick = patternTick + scaled(n.tick, file.ppq);
      if (note.tick < 0 || note.tick >= pattern.lengthTicks) continue;
      note.length = std::clamp<int64_t>(scaled(n.length, file.ppq), 1, pattern.lengthTicks);
      note.pitch = std::clamp(n.pitch, 0, 127);
      note.velocity = std::clamp(n.velocity, 1, 127) / 127.0;
      add.push_back(note);
    }
  }
  if (pattern.notes.size() + add.size() > kMaxPatternNotes) throw DocumentError("midi", "the pattern would pass " + std::to_string(kMaxPatternNotes) + " notes");
  for (auto& n : add) {
    n.id = newNoteId(pattern);
    pattern.notes.push_back(n);
  }
  std::stable_sort(pattern.notes.begin(), pattern.notes.end(), [](const Note& a, const Note& b) { return a.tick < b.tick; });
  return (int)add.size();
}

}  // namespace core::doc
