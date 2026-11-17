#include "samples.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include "assets.hpp"
#include "edit.hpp"
#include "sample_analysis.hpp"
#include "sha256.hpp"

namespace core::doc {

namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Words of a name: lower case, split on anything but letters, digits and '#'.
std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  std::string w;
  for (char c : lower(text)) {
    if (std::isalnum((unsigned char)c) || c == '#') w += c;
    else if (!w.empty()) { out.push_back(w); w.clear(); }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

bool has(const std::vector<std::string>& ws, std::initializer_list<const char*> any) {
  for (const auto& w : ws) for (const char* a : any) if (w == a) return true;
  return false;
}

// The role words, in order: the first rule that matches wins ("SNARE BUILD" is a snare, "KICK BASS" a
// kick and bass loop).
std::string roleOf(const std::vector<std::string>& ws) {
  const bool kick = has(ws, {"kick", "kicks", "bd"});
  if (kick && has(ws, {"bass", "k&b", "kb"})) return "kickbass";
  if (kick) return "kick";
  if (has(ws, {"snare", "snares", "sn", "rim"})) return "snare";
  if (has(ws, {"clap", "claps"})) return "clap";
  if (has(ws, {"tom", "toms"})) return "tom";
  if (has(ws, {"oh", "ohh", "openhat", "openhats"}) || (has(ws, {"open"}) && has(ws, {"hat", "hats", "hh", "hihat"}))) return "openhat";
  if (has(ws, {"hat", "hats", "hh", "chh", "ch", "hihat", "hihats", "closed"})) return "hat";
  if (has(ws, {"crash", "crashes", "cymbal", "ride"})) return "crash";
  if (has(ws, {"impact", "impacts", "boom"})) return "impact";
  if (has(ws, {"riser", "risers", "rise", "uplifter", "sweep", "build", "buildup"})) return "riser";
  if (has(ws, {"perc", "percs", "percussion", "shaker", "conga", "bongo"})) return "perc";
  if (has(ws, {"bass", "basses", "bassline", "basslines", "808", "808s", "sub", "contrabass"})) return "bass";
  if (has(ws, {"vox", "vocal", "vocals", "voice", "chop"})) return "vox";
  if (has(ws, {"pad", "pads", "atmos", "atmosphere", "ambience", "drone"})) return "pad";
  if (has(ws, {"horn", "horns", "strings", "tronbone", "trombone", "tuba", "symphony", "brass"})) return "orchestral";
  if (has(ws, {"acid", "synth", "synths", "stab", "stabs", "pluck", "lead", "keys", "arp", "melody", "melodies", "gate"})) return "synth";
  if (has(ws, {"fx", "fxs", "sfx", "zap", "noise", "downlifter"})) return "fx";
  if (has(ws, {"fill", "fills", "drum", "drums", "beat", "groove"})) return "fill";
  return "";
}

bool loopWords(const std::vector<std::string>& ws) {
  return has(ws, {"loop", "loops", "fill", "fills", "stem", "stems", "groove", "beat"});
}

// A tempo in the words: a whole number 70..300 that is not the leading index ("81 SYNTH G"). Faster (slambient goes
// past 500) only when the name says so: "350 BPM", "350bpm" or "bpm 350"; a bare 303 is the TB-303, not a tempo
// (audit 2026-11-05).
double bpmOf(const std::vector<std::string>& ws, bool skipFirst) {
  auto digits = [](const std::string& w) {
    return !w.empty() && w.size() <= 3 && std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit((unsigned char)c); });
  };
  for (size_t i = skipFirst ? 1 : 0; i < ws.size(); ++i) {
    const auto& w = ws[i];
    if (w.size() > 3 && w.size() <= 6 && w.compare(w.size() - 3, 3, "bpm") == 0 && digits(w.substr(0, w.size() - 3))) {
      const int n = std::stoi(w.substr(0, w.size() - 3));
      if (n >= 70 && n <= 999) return n;
      continue;
    }
    if (!digits(w)) continue;
    const int n = std::stoi(w);
    if (n >= 70 && n <= 300) return n;
    const bool said = (i + 1 < ws.size() && ws[i + 1] == "bpm") || (i > 0 && ws[i - 1] == "bpm");
    if (n > 300 && n <= 999 && said) return n;
  }
  return 0;
}

}  // namespace

SampleInfo classifySample(const std::string& relativePath) {
  SampleInfo info;
  std::string path = relativePath;
  std::replace(path.begin(), path.end(), '\\', '/');
  const size_t slash = path.rfind('/');
  std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = file.rfind('.');
  if (dot != std::string::npos) file = file.substr(0, dot);
  const auto fileWords = words(file);
  // Folders from the deepest up: the file's own words decide first.
  std::vector<std::vector<std::string>> folders;
  for (size_t end = slash; end != std::string::npos && end > 0;) {
    const size_t start = path.rfind('/', end - 1);
    folders.push_back(words(path.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1))));
    end = start;
  }
  info.role = roleOf(fileWords);
  if (info.role.empty())
    for (const auto& f : folders) if (!(info.role = roleOf(f)).empty()) break;
  if (info.role.empty()) info.role = "other";
  const bool leadingIndex = !file.empty() && std::isdigit((unsigned char)file[0]);
  info.bpm = bpmOf(fileWords, leadingIndex);
  // A loop by its name, tempo or folder ("Loops", "Synth Loops"); the pack's own name does not count
  // ("Function Loops - Free Psytrance Samples"), and "One Shot" anywhere says it is a hit.
  auto oneShot = [](const std::vector<std::string>& ws) {
    if (has(ws, {"oneshot", "oneshots"})) return true;
    for (size_t i = 0; i + 1 < ws.size(); ++i) if (ws[i] == "one" && (ws[i + 1] == "shot" || ws[i + 1] == "shots")) return true;
    return false;
  };
  bool hit = oneShot(fileWords);
  for (const auto& f : folders) hit = hit || oneShot(f);
  info.loop = !hit && (loopWords(fileWords) || info.bpm > 0 || info.role == "kickbass");
  for (size_t i = 0; i + 1 < folders.size() && !info.loop && !hit; ++i) info.loop = loopWords(folders[i]);  // the last one is the pack
  for (const auto& f : folders) if (info.bpm == 0 && has(f, {"bpm"})) info.bpm = bpmOf(f, false);
  // Key: an upper-case note on its own ("KICK G", "BASS F#", "Dm", "F#m", "C1", "G# 3").
  static const std::regex note(R"((?:^|[\s_\-\(\.])([A-G])(#|b)?(m|min)?(?:\s?(\d))?(?=$|[\s_\-\)\.]))");
  std::smatch m;
  std::string rest = file;
  // The leading index ("07 KICK A") is not part of the name.
  rest = std::regex_replace(rest, std::regex(R"(^\s*\d+\s*[\.\-]*\s*-?\s*)"), " ");
  if (std::regex_search(rest, m, note)) {
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
    int pc = base[m[1].str()[0] - 'A'];
    if (m[2].matched) pc += m[2].str() == "#" ? 1 : -1;
    info.keyClass = (pc + 12) % 12;
    info.minor = m[3].matched;
    if (m[4].matched) {
      // "C1" (no space) is a note; "B 2" only on a bass (elsewhere it is a take number: "HORNS B 2").
      const bool spaced = rest[(size_t)m.position(4) - 1] == ' ';
      // Only a MIDI key (0-127): "A9" is above it, and one such zone would refuse the whole instrument.
      const int key = (std::stoi(m[4].str()) + 1) * 12 + info.keyClass;
      if ((!spaced || info.role == "bass") && key >= 0 && key <= 127) info.rootKey = key;
    }
  }
  return info;
}

const LibrarySample* SampleIndex::findFile(const std::string& file) const {
  for (const auto& s : samples) if (s.file == file) return &s;
  return nullptr;
}
const LibrarySample* SampleIndex::findHash(const std::string& sha256) const {
  if (sha256.empty()) return nullptr;
  for (const auto& s : samples) if (s.sha256 == sha256) return &s;
  return nullptr;
}
LibrarySample* SampleIndex::findHash(const std::string& sha256) {
  return const_cast<LibrarySample*>(static_cast<const SampleIndex&>(*this).findHash(sha256));
}
const LibrarySample* SampleIndex::find(const std::string& key) const {
  if (key.empty()) return nullptr;
  std::string want = key;
  std::replace(want.begin(), want.end(), '\\', '/');
  for (const auto& s : samples) {
    if (s.path == want || s.file == key || s.sha256 == key) return &s;
    for (const auto& o : s.otherPaths) if (o == want) return &s;
  }
  return nullptr;
}

void measureLoudness(const fs::path& file, double& peakDb, double& loudDb) {
  const WavInfo w = readWavInfo(file);
  std::ifstream in(file, std::ios::binary);
  if (!in || !in.seekg((std::streamoff)w.dataOffset)) throw DocumentError("file", "cannot be read");
  const int bytes = w.bits / 8, ch = w.channels;
  const int64_t frames = std::min<int64_t>(w.frames, (int64_t)(w.sampleRate * 60));
  const int64_t block = std::max<int64_t>(1, (int64_t)(w.sampleRate * 0.01));  // 10 ms
  std::vector<double> blocks;  // mean square of each 10 ms
  std::vector<unsigned char> buf((size_t)(block * bytes * ch));
  double peak = 0;
  auto sample = [&](const unsigned char* p) -> double {
    if (w.isFloat) {
      if (bytes == 4) { float f; std::memcpy(&f, p, 4); return f; }
      double d; std::memcpy(&d, p, 8); return d;
    }
    if (bytes == 2) return (int16_t)(p[0] | (p[1] << 8)) / 32768.0;
    if (bytes == 3) return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.0;
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)) / 2147483648.0;
  };
  for (int64_t done = 0; done < frames; done += block) {
    const int64_t n = std::min(block, frames - done);
    if (!in.read((char*)buf.data(), (std::streamsize)(n * bytes * ch))) break;
    double sum = 0;
    for (int64_t i = 0; i < n; ++i)
      for (int c = 0; c < ch; ++c) {
        const double x = sample(&buf[(size_t)((i * ch + c) * bytes)]);
        if (std::isfinite(x)) { peak = std::max(peak, std::fabs(x)); sum += x * x; }
      }
    blocks.push_back(sum / (double)(n * ch));
  }
  double loud = 0, window = 0;
  for (size_t i = 0; i < blocks.size(); ++i) {  // the loudest 100 ms (10 blocks; shorter files: all of it)
    window += blocks[i];
    if (i >= 10) window -= blocks[i - 10];
    loud = std::max(loud, window / (double)std::min<size_t>(i + 1, 10));
  }
  auto db = [](double x) { return x > 1e-10 ? 20 * std::log10(x) : -200.0; };
  peakDb = db(peak);
  loudDb = loud > 1e-20 ? 10 * std::log10(loud) : -200.0;
}

double roleLoudnessTarget(const std::string& role) {
  static const std::map<std::string, double> targets = {
      {"kick", -9}, {"bass", -11}, {"hat", -19}, {"openhat", -19}, {"perc", -17}, {"accent", -15},
      {"impact", -13}, {"riser", -17}, {"atmos", -20}};
  auto t = targets.find(role);
  return t == targets.end() ? -16 : t->second;
}

double levelFor(const LibrarySample& sample, const std::string& role) {
  if (sample.loudDb <= kNotMeasured + 1 || sample.loudDb < -150) return 0;
  double level = roleLoudnessTarget(role) - sample.loudDb;
  if (sample.peakDb > kNotMeasured + 1) level = std::min(level, -0.3 - sample.peakDb + 6);  // never more than 6 dB over full scale before the mixer
  return std::round(std::clamp(level, -24.0, 24.0) * 10) / 10;
}

namespace {
// The hash of the first 64 KiB of a file: one short read that says whether the bytes are still the ones a
// previous scan measured (audit 2026-11-17, #3a: the same length and the same date are not the same file).
// "" when the file cannot be read: the caller then reads it the long way and reports what is wrong with it.
constexpr size_t kSampleHeadBytes = 64 * 1024;
std::string headSha256Of(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return "";
  std::vector<char> buf(kSampleHeadBytes);
  in.read(buf.data(), (std::streamsize)kSampleHeadBytes);
  buf.resize((size_t)std::max<std::streamsize>(0, in.gcount()));
  return sha256Hex(buf.data(), buf.size());
}
}  // namespace

LibrarySample describeSample(const fs::path& file, const fs::path& root) {
  LibrarySample s;
  std::error_code e;
  s.file = utf8(file);
  const fs::path rel = root.empty() ? fs::path() : fs::relative(file, root, e);
  const std::string relText = utf8(rel);
  s.path = !e && !relText.empty() && relText.rfind("..", 0) != 0 ? relText : utf8(file.filename());
  std::replace(s.path.begin(), s.path.end(), '\\', '/');
  s.name = utf8(file.stem());
  s.pack = s.path.find('/') == std::string::npos ? "" : s.path.substr(0, s.path.find('/'));
  s.size = (int64_t)fs::file_size(file, e);
  s.mtime = (int64_t)fs::last_write_time(file, e).time_since_epoch().count();
  s.headSha256 = headSha256Of(file);
  const WavInfo w = readWavInfo(file);
  s.sampleRate = w.sampleRate;
  s.channels = w.channels;
  s.seconds = w.sampleRate > 0 ? (double)w.frames / w.sampleRate : 0;
  try {
    s.sha256 = sha256File(file);
  } catch (const std::exception& x) {
    throw DocumentError("file", x.what());
  }
  measureLoudness(file, s.peakDb, s.loudDb);
  s.embedded = fileMetadata(file, s.sampleRate);
  s.info = s.inferred = classifySample(s.path);
  return s;
}

const LibrarySample& addLibraryFile(SampleIndex& index, const fs::path& file) {
  const fs::path root = fs::u8path(index.root);
  std::error_code e;
  const fs::path rel = fs::relative(file, root, e);
  const std::string relText = utf8(rel);
  if (index.root.empty() || e || relText.empty() || relText.rfind("..", 0) == 0) throw DocumentError("file", "not in the sample library: " + utf8(file));
  LibrarySample s = describeSample(file, root);
  for (auto& x : index.samples) {
    if (x.path == s.path) { x = s; resolveCatalog(index); return *index.findHash(s.sha256); }  // the same file, written again
    if (x.sha256 == s.sha256) {
      if (std::find(x.otherPaths.begin(), x.otherPaths.end(), s.path) == x.otherPaths.end()) {
        x.otherPaths.push_back(s.path);
        std::sort(x.otherPaths.begin(), x.otherPaths.end());
        ++x.duplicates;
      }
      return x;
    }
  }
  const std::string sha = s.sha256;
  auto at = std::lower_bound(index.samples.begin(), index.samples.end(), s.path, [](const LibrarySample& a, const std::string& p) { return a.path < p; });
  index.samples.insert(at, std::move(s));
  resolveCatalog(index);
  return *index.findHash(sha);
}

SampleIndex scanLibrary(const fs::path& root, const SampleIndex* previous, const std::function<bool(int, int)>& progress) {
  std::error_code e;
  if (!fs::is_directory(root, e)) throw DocumentError("library", "folder not found: " + utf8(root));
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, e), end; !e && it != end; it.increment(e)) {
    if (!it->is_regular_file(e)) continue;
    if (lower(utf8(it->path().extension())) == ".wav") files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  std::map<std::string, const LibrarySample*> before, beforeHash;
  if (previous && previous->root == utf8(root))  // same library only
    for (const auto& s : previous->samples) { before[s.file] = &s; beforeHash[s.sha256] = &s; }
  SampleIndex index;
  index.root = utf8(root);
  std::map<std::string, size_t> byHash;
  int done = 0;
  for (const auto& f : files) {
    if (progress && !progress(done, (int)files.size())) throw DocumentError("library", "scan stopped");
    ++done;
    LibrarySample s;
    std::error_code fe;
    const int64_t size = (int64_t)fs::file_size(f, fe);
    const int64_t mtime = (int64_t)fs::last_write_time(f, fe).time_since_epoch().count();
    auto old = before.find(utf8(f));
    bool kept = false;  // the registration of the last scan, still good for these bytes
    if (old != before.end() && old->second->size == size && old->second->mtime == mtime && !old->second->sha256.empty() &&
        old->second->loudDb > kNotMeasured + 1) {
      const std::string head = headSha256Of(f);
      // The size and the date are not enough: a file written again in place has both (audit 2026-11-17, #3a),
      // and only its first bytes say so. An index from an older Core has no head hash yet: take the file as it
      // stands, remember its head now, and do not re-measure the whole library for one scan.
      if (!head.empty() && (old->second->headSha256.empty() || old->second->headSha256 == head)) {
        s = *old->second;  // unchanged: what was read before
        s.headSha256 = head;
        s.duplicates = 0;
        s.otherPaths.clear();
        if (!s.embedded.is_object()) s.embedded = fileMetadata(f, s.sampleRate);  // indexed by an older Core
        kept = true;
      }
    }
    if (!kept) {
      try {
        s = describeSample(f, root);
        // A file that moved (by hand): same content, so its measurements still hold.
        auto moved = beforeHash.find(s.sha256);
        if (moved != beforeHash.end() && moved->second->loudDb > kNotMeasured + 1) {
          s.peakDb = moved->second->peakDb;
          s.loudDb = moved->second->loudDb;
          s.analysis = moved->second->analysis;
        }
      } catch (const std::exception& x) {
        std::string rel = utf8(fs::relative(f, root, e));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        index.unreadable.push_back(rel + ": " + x.what());
        continue;
      }
    }
    s.info = s.inferred = classifySample(s.path);
    s.sidecar = nullptr;
    auto same = byHash.find(s.sha256);
    if (same != byHash.end()) {  // the first path keeps it; the others are its other locations
      ++index.samples[same->second].duplicates;
      index.samples[same->second].otherPaths.push_back(s.path);
      continue;
    }
    byHash[s.sha256] = index.samples.size();
    index.samples.push_back(std::move(s));
  }
  if (progress) progress(done, (int)files.size());
  for (auto& x : index.samples) std::sort(x.otherPaths.begin(), x.otherPaths.end());
  if (previous) {  // the user's catalogue is about contents, wherever the library is
    index.overrides = previous->overrides;
    index.undo = previous->undo;
    index.proposals = previous->proposals;
  }
  // The library's own metadata file (written by hand or by a tool), when there is one.
  const fs::path meta = root / kSampleMetadataFile;
  if (fs::is_regular_file(meta, e)) {
    try {
      std::ifstream in(meta, std::ios::binary);
      const json file = json::parse(in);
      for (auto& [sha, fields] : readSampleMetadataFile(file, index, index.sidecarProblems))
        if (LibrarySample* x = index.findHash(sha)) x->sidecar = fields;
    } catch (const std::exception& x) {
      index.sidecarProblems.push_back(std::string(kSampleMetadataFile) + ": " + x.what());
    }
  }
  resolveCatalog(index);
  return index;
}

json indexToJson(const SampleIndex& index) {
  json samples = json::array();
  for (const auto& s : index.samples)
    samples.push_back({{"file", s.file}, {"path", s.path}, {"sha256", s.sha256}, {"size", s.size}, {"mtime", s.mtime},
                       {"headSha256", s.headSha256},
                       {"seconds", s.seconds}, {"sampleRate", s.sampleRate}, {"channels", s.channels}, {"duplicates", s.duplicates},
                       {"peakDb", s.peakDb}, {"loudDb", s.loudDb}, {"otherPaths", s.otherPaths},
                       {"sidecar", s.sidecar.is_object() ? s.sidecar : json(nullptr)},
                       {"analysis", s.analysis.is_object() ? s.analysis : json(nullptr)},
                       {"embedded", s.embedded.is_object() ? s.embedded : json(nullptr)}});
  json overrides = json::object();
  for (const auto& [sha, fields] : index.overrides) overrides[sha] = fields;
  json out = {{"format", kSampleIndexFormat}, {"version", kSampleIndexVersion}, {"root", index.root}, {"samples", samples},
              {"unreadable", index.unreadable}, {"overrides", overrides}, {"sidecarProblems", index.sidecarProblems}};
  if (!index.proposals.empty()) {  // optional: older Cores ignore it
    json proposals = json::object();
    for (const auto& [sha, p] : index.proposals) proposals[sha] = p;
    out["proposals"] = proposals;
  }
  return out;
}

SampleIndex indexFromJson(const json& v) {
  if (!v.is_object() || v.value("format", "") != kSampleIndexFormat) throw DocumentError("index", "not a Core sample index");
  const int version = v.value("version", 0);
  if (version != 1 && version != kSampleIndexVersion) throw DocumentError("index", "made by another version of Core");
  SampleIndex index;
  try {
    index.root = v.at("root").get<std::string>();
    for (const auto& j : v.at("samples")) {
      LibrarySample s;
      s.file = j.at("file").get<std::string>();
      s.path = j.at("path").get<std::string>();
      s.sha256 = j.at("sha256").get<std::string>();
      s.size = j.value("size", (int64_t)0); s.mtime = j.value("mtime", (int64_t)0);
      s.headSha256 = j.value("headSha256", std::string());  // an index from an older Core: hashed at the next scan
      s.seconds = j.value("seconds", 0.0); s.sampleRate = j.value("sampleRate", 0.0); s.channels = j.value("channels", 0);
      s.duplicates = j.value("duplicates", 0);
      s.peakDb = j.value("peakDb", kNotMeasured); s.loudDb = j.value("loudDb", kNotMeasured);  // older indexes: measured at the next scan
      const fs::path p = fromUtf8(s.path);
      s.name = utf8(p.stem());
      s.pack = s.path.find('/') == std::string::npos ? "" : s.path.substr(0, s.path.find('/'));
      s.info = s.inferred = classifySample(s.path);  // the rules may have improved since the scan
      for (const auto& o : j.value("otherPaths", json::array())) s.otherPaths.push_back(o.get<std::string>());
      if (j.contains("analysis") && j["analysis"].is_object()) s.analysis = j["analysis"];
      if (j.contains("embedded") && j["embedded"].is_object()) s.embedded = j["embedded"];  // older Cores: read at the next scan
      if (j.contains("sidecar") && j["sidecar"].is_object()) {
        try { s.sidecar = normalizeSampleMetadata(j["sidecar"]); } catch (const DocumentError&) {}  // read again at the next scan
      }
      index.samples.push_back(std::move(s));
    }
    for (const auto& u : v.value("unreadable", json::array())) index.unreadable.push_back(u.get<std::string>());
    for (const auto& u : v.value("sidecarProblems", json::array())) index.sidecarProblems.push_back(u.get<std::string>());
    const json overrides = v.value("overrides", json::object());
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
      // A correction Core cannot read any more is dropped, not the whole index.
      try {
        json fields = normalizeSampleMetadata(it.value());
        for (auto f = fields.begin(); f != fields.end();) f = f->is_null() ? fields.erase(f) : std::next(f);
        if (!fields.empty()) index.overrides[it.key()] = fields;
      } catch (const DocumentError&) {}
    }
    const json proposals = v.value("proposals", json::object());
    for (auto it = proposals.begin(); it != proposals.end(); ++it) {
      try {
        const json& p = it.value();
        if (!p.is_object() || !p.contains("fields")) continue;
        index.proposals[it.key()] = {{"fields", normalizeSampleMetadata(p["fields"])}, {"why", p.value("why", std::string())}};
      } catch (const DocumentError&) {}
    }
  } catch (const json::exception& x) {
    throw DocumentError("index", std::string("damaged: ") + x.what());
  }
  resolveCatalog(index);
  return index;
}

// ---------------------------------------------------------------- the catalogue

const std::vector<std::string> kSampleMetadataFields = {"role", "kind", "bpm", "key", "styles", "tags", "name"};

namespace {

const std::vector<std::string> kRoles = {"kick", "bass", "hat", "openhat", "snare", "clap", "perc", "tom", "crash", "impact",
                                         "riser", "fx", "vox", "pad", "synth", "orchestral", "fill", "kickbass", "other"};

const std::map<std::string, std::string> kRoleAliases = {
    {"closedhat", "hat"}, {"closedhats", "hat"}, {"hats", "hat"}, {"hihat", "hat"}, {"hh", "hat"}, {"openhats", "openhat"},
    {"oh", "openhat"}, {"ride", "crash"}, {"cymbal", "crash"}, {"kicks", "kick"}, {"basses", "bass"}, {"rim", "snare"},
    {"percussion", "perc"}, {"atmos", "pad"}, {"atmosphere", "pad"}, {"drone", "pad"}, {"vocal", "vox"}, {"sfx", "fx"},
    {"loop", "fill"}, {"kb", "kickbass"}};

std::string canonicalRole(std::string r) {
  r = lower(r);
  r.erase(std::remove_if(r.begin(), r.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), r.end());
  if (std::find(kRoles.begin(), kRoles.end(), r) != kRoles.end()) return r;
  auto a = kRoleAliases.find(r);
  return a == kRoleAliases.end() ? "" : a->second;
}

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// "F#m", "Bb", "Dmin", "C major", "E1", "G#3": false when it is not a key.
bool parseKey(const std::string& text, int& keyClass, bool& minor, int& rootKey) {
  static const std::regex key(R"(^\s*([A-Ga-g])\s*(#|b|♯|♭)?\s*(m|min|minor|maj|major)?\s*(-?\d)?\s*$)");
  std::smatch m;
  if (!std::regex_match(text, m, key)) return false;
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};
  int pc = base[std::toupper((unsigned char)m[1].str()[0]) - 'A'];
  if (m[2].matched) pc += (m[2].str() == "#" || m[2].str() == "♯") ? 1 : -1;
  keyClass = (pc + 12) % 12;
  minor = m[3].matched && m[3].str().rfind("maj", 0) != 0;
  rootKey = -1;
  if (m[4].matched) {
    const int note = (std::stoi(m[4].str()) + 1) * 12 + pc;  // C1 = 24, as in the names
    if (note < 0 || note > 127) return false;
    rootKey = note;
  }
  return true;
}

std::string keyText(const SampleInfo& i) {
  if (i.keyClass < 0) return "";
  std::string t = kNoteNames[i.keyClass];
  if (i.minor) t += "m";
  if (i.rootKey >= 0) t += std::to_string(i.rootKey / 12 - 1);
  return t;
}

std::vector<std::string> wordList(const json& v, const char* field) {
  if (!v.is_array()) throw DocumentError(field, "a list of words");
  if (v.size() > 32) throw DocumentError(field, "at most 32");
  std::vector<std::string> out;
  for (const auto& x : v) {
    if (!x.is_string()) throw DocumentError(field, "a list of words");
    std::string w = lower(x.get<std::string>());
    while (!w.empty() && std::isspace((unsigned char)w.back())) w.pop_back();
    while (!w.empty() && std::isspace((unsigned char)w.front())) w.erase(w.begin());
    if (w.empty()) continue;
    if (w.size() > 40) throw DocumentError(field, "\"" + w.substr(0, 40) + "...\" is too long (40 characters at most)");
    if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
  }
  return out;
}

// Fields (already normalized) over an info.
void applyFields(SampleInfo& info, const json& fields) {
  if (!fields.is_object()) return;
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    const json& v = it.value();
    if (v.is_null()) continue;
    if (it.key() == "role") info.role = v.get<std::string>();
    else if (it.key() == "kind") info.loop = v.get<std::string>() == "loop";
    else if (it.key() == "bpm") info.bpm = v.get<double>();
    else if (it.key() == "key") {
      info.keyClass = -1; info.minor = false; info.rootKey = -1;
      const std::string k = v.get<std::string>();
      if (!k.empty()) parseKey(k, info.keyClass, info.minor, info.rootKey);
    } else if (it.key() == "styles") info.styles = v.get<std::vector<std::string>>();
    else if (it.key() == "tags") info.tags = v.get<std::vector<std::string>>();
    else if (it.key() == "name") info.name = v.get<std::string>();
  }
}

bool hasField(const json& fields, const std::string& f) { return fields.is_object() && fields.contains(f) && !fields[f].is_null(); }

const json& embeddedFields(const json& embedded) {
  static const json none = json::object();
  return embedded.is_object() && embedded.contains("fields") && embedded["fields"].is_object() ? embedded["fields"] : none;
}

// Whether the file's field counts over what the name says (see fileMetadata).
bool fileGives(const SampleInfo& fromName, const json& embedded, const std::string& f) {
  const json& fields = embeddedFields(embedded);
  if (!hasField(fields, f)) return false;
  if (f == "role") return fromName.role == "other";
  if (f == "key") return fromName.keyClass < 0;
  return true;
}

void applyFileFields(SampleInfo& info, const json& embedded) {
  const json& fields = embeddedFields(embedded);
  json use = json::object();
  for (const auto& f : kSampleMetadataFields) if (fileGives(info, embedded, f)) use[f] = fields[f];
  try { applyFields(info, use); } catch (const std::exception&) {}  // an index edited by hand: the name decides
}

}  // namespace

json normalizeSampleMetadata(const json& fields) {
  if (!fields.is_object()) throw DocumentError("metadata", "an object of fields (role, kind, bpm, key, styles, tags)");
  json out = json::object();
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    const std::string& f = it.key();
    const json& v = it.value();
    if (std::find(kSampleMetadataFields.begin(), kSampleMetadataFields.end(), f) == kSampleMetadataFields.end())
      throw DocumentError(f, "not a field of the catalogue (role, kind, bpm, key, styles, tags)");
    if (v.is_null()) { out[f] = nullptr; continue; }
    if (f == "role") {
      const std::string r = v.is_string() ? canonicalRole(v.get<std::string>()) : "";
      if (r.empty()) throw DocumentError(f, "one of kick, bass, hat, openhat, snare, clap, perc, tom, crash, impact, riser, fx, vox, pad, synth, orchestral, fill, kickbass, other");
      out[f] = r;
    } else if (f == "kind") {
      std::string k = v.is_string() ? lower(v.get<std::string>()) : "";
      k.erase(std::remove_if(k.begin(), k.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }), k.end());
      if (k == "oneshot" || k == "hit" || k == "shot") out[f] = "one-shot";
      else if (k == "loop" || k == "stem" || k == "fill") out[f] = "loop";
      else throw DocumentError(f, "\"one-shot\" or \"loop\"");
    } else if (f == "bpm") {
      if (!v.is_number()) throw DocumentError(f, "a number: a tempo 40..999, or 0 when the sample has none");
      const double b = v.get<double>();
      if (!(b == 0 || (b >= 40 && b <= 999))) throw DocumentError(f, "a tempo 40..999, or 0 when the sample has none");
      out[f] = std::round(b * 100) / 100;
    } else if (f == "name") {
      if (!v.is_string()) throw DocumentError(f, "a name (text), or \"\" to let Core make one");
      std::string t = v.get<std::string>();
      while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
      while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
      if (t.size() > 80) throw DocumentError(f, "80 characters at most");
      if (t.find_first_of("\r\n\t") != std::string::npos) throw DocumentError(f, "one line");
      out[f] = t.empty() ? json(nullptr) : json(t);
    } else if (f == "key") {
      if (!v.is_string()) throw DocumentError(f, "a key like \"F#m\", \"C\" or a note like \"E1\"; \"\" when it has none");
      SampleInfo i;
      if (!v.get<std::string>().empty() && !parseKey(v.get<std::string>(), i.keyClass, i.minor, i.rootKey))
        throw DocumentError(f, "\"" + v.get<std::string>() + "\" is not a key (\"F#m\", \"C\", \"E1\")");
      out[f] = keyText(i);
    } else {
      out[f] = wordList(v, f.c_str());
    }
  }
  return out;
}

json fileMetadata(const fs::path& file, double sampleRate) {
  const WavTags t = readWavTags(file);
  json out = json::object();
  if (t.empty()) return out;
  json info = json::object();
  for (const char* id : {"INAM", "ISBJ", "IKEY", "IGNR", "ICMT", "IART", "ICOP", "IENG", "IPRD", "ICRD", "ISFT"})
    if (auto i = t.info.find(id); i != t.info.end()) info[id] = i->second.substr(0, 200);
  if (!info.empty()) out["info"] = info;
  json fields = json::object();
  std::vector<std::string> tags;
  auto addTag = [&](std::string w) {
    w = lower(w);
    const size_t a = w.find_first_not_of(" \t-_."), b = w.find_last_not_of(" \t-_.");
    if (a == std::string::npos) return;
    w = w.substr(a, b - a + 1);
    if (w.size() <= 40 && tags.size() < 12 && std::find(tags.begin(), tags.end(), w) == tags.end()) tags.push_back(w);
  };
  std::string role;
  for (const char* id : {"IKEY", "ISBJ", "IGNR"}) {
    auto i = t.info.find(id);
    if (i == t.info.end()) continue;
    std::string cur;
    for (char c : i->second + ";") {
      if (c == ';' || c == ',' || c == '/' || c == '|') {
        if (role.empty() && lower(cur).find("loop") == std::string::npos) role = canonicalRole(cur);  // "loop" says a kind, not a role
        addTag(cur);
        cur.clear();
      } else cur += c;
    }
  }
  if (!role.empty()) fields["role"] = role;
  if (!tags.empty()) fields["tags"] = tags;
  if (t.hasAcid) {
    fields["kind"] = t.acidOneShot ? "one-shot" : "loop";
    if (!t.acidOneShot && t.acidBpm > 0) fields["bpm"] = t.acidBpm;
    if (t.acidRootKey >= 0) fields["key"] = std::string(kNoteNames[t.acidRootKey % 12]) + std::to_string(t.acidRootKey / 12 - 1);
  }
  try {
    fields = normalizeSampleMetadata(fields);
  } catch (const DocumentError&) {
    fields.erase("key");  // a root note out of the catalogue's range: the rest still counts
    try { fields = normalizeSampleMetadata(fields); } catch (const DocumentError&) { fields = json::object(); }
  }
  if (!fields.empty()) out["fields"] = fields;
  if (t.loopEnd > t.loopStart && sampleRate > 0)
    out["loop"] = {std::round((double)t.loopStart / sampleRate * 1e6) / 1e6, std::round((double)t.loopEnd / sampleRate * 1e6) / 1e6};
  return out;
}

json sampleInfoJson(const SampleInfo& i) {
  return {{"role", i.role}, {"kind", i.loop ? "loop" : "one-shot"}, {"bpm", i.bpm}, {"key", keyText(i)}, {"styles", i.styles}, {"tags", i.tags},
          {"name", i.name}};
}

namespace {
std::string capitalized(std::string w) {
  if (!w.empty()) w[0] = (char)std::toupper((unsigned char)w[0]);
  return w;
}
std::string styleLabel(const std::string& st) {
  static const std::map<std::string, std::string> known = {
      {"darkpsycore", "Dark psycore"}, {"darkpsy", "Darkpsy"}, {"psycore", "Psycore"}, {"hitech", "Hi-tech"}, {"hi-tech", "Hi-tech"},
      {"slambient", "Slambient"}, {"psytrance", "Psytrance"}, {"fullon", "Full-on"}, {"full-on", "Full-on"}, {"forest", "Forest"}};
  auto k = known.find(st);
  return k != known.end() ? k->second : capitalized(st);
}
}  // namespace

namespace {
const std::map<std::string, std::string>& roleLabels() {
  static const std::map<std::string, std::string> roles = {
      {"kick", "Kick"}, {"bass", "Bass"}, {"hat", "Hat"}, {"openhat", "Open hat"}, {"snare", "Snare"}, {"clap", "Clap"},
      {"perc", "Perc"}, {"tom", "Tom"}, {"crash", "Crash"}, {"impact", "Impact"}, {"riser", "Riser"}, {"fx", "FX"}, {"vox", "Vox"},
      {"pad", "Pad"}, {"synth", "Synth"}, {"orchestral", "Orchestral"}, {"fill", "Fill"}, {"kickbass", "Kick & bass"}};
  return roles;
}
}  // namespace

std::string sampleRoleLabel(const std::string& role) {
  auto r = roleLabels().find(role);
  return r != roleLabels().end() ? r->second : "Other";
}

std::string generatedName(const LibrarySample& s) {
  const auto& roles = roleLabels();
  const SampleInfo& i = s.info;
  auto r = roles.find(i.role);
  if (r == roles.end() && i.styles.empty() && i.tags.empty()) return "";
  std::vector<std::string> parts;
  parts.push_back(r != roles.end() ? r->second : "Sample");
  if (i.loop && i.role != "fill" && i.role != "kickbass") parts.push_back("Loop");
  for (size_t k = 0; k < i.styles.size() && k < 2; ++k) parts.push_back(styleLabel(i.styles[k]));
  for (size_t k = 0; k < i.tags.size() && k < 2; ++k) parts.push_back(capitalized(i.tags[k]));
  const auto measured = soundWords(s);
  for (const char* w : {"short", "long"})
    if (!i.loop && std::find(measured.begin(), measured.end(), w) != measured.end()) parts.push_back(capitalized(w));
  if (const std::string k = keyText(i); !k.empty()) parts.push_back(k);
  if (i.loop && i.bpm > 0) {
    char b[32];
    std::snprintf(b, sizeof b, i.bpm == std::floor(i.bpm) ? "%.0f BPM" : "%.1f BPM", i.bpm);
    parts.push_back(b);
  }
  std::string out;
  for (const auto& p : parts) out += (out.empty() ? "" : " \xc2\xb7 ") + p;
  return out;
}

void resolveCatalog(SampleIndex& index) {
  for (auto& s : index.samples) {
    s.info = s.inferred;
    applyFileFields(s.info, s.embedded);
    applyFields(s.info, s.sidecar);
    auto o = index.overrides.find(s.sha256);
    if (o != index.overrides.end()) applyFields(s.info, o->second);
  }
  // Names: the given one, else the generated one; two that read the same get " 1", " 2"... in content (sha256) order.
  std::map<std::string, int> seen, total;
  std::vector<std::string> base(index.samples.size());
  for (size_t k = 0; k < index.samples.size(); ++k) {
    const auto& s = index.samples[k];
    base[k] = !s.info.name.empty() ? s.info.name : generatedName(s);
    if (base[k].empty()) base[k] = s.name;
    ++total[base[k]];
  }
  // Numbered by content, not by path: moving files never swaps two numbers.
  std::vector<size_t> order(index.samples.size());
  for (size_t k = 0; k < order.size(); ++k) order[k] = k;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return index.samples[a].sha256 < index.samples[b].sha256; });
  for (size_t k : order) {
    const int n = ++seen[base[k]];
    index.samples[k].displayName = total[base[k]] > 1 ? base[k] + " " + std::to_string(n) : base[k];
  }
}

std::string metadataOrigin(const SampleIndex& index, const LibrarySample& s, const std::string& field) {
  auto o = index.overrides.find(s.sha256);
  if (o != index.overrides.end() && hasField(o->second, field)) return "user";
  if (hasField(s.sidecar, field)) return "metadata file";
  const SampleInfo& n = s.inferred;
  if (fileGives(n, s.embedded, field)) return "file";
  if (field == "role") return n.role == "other" ? "" : "name";
  if (field == "kind") return "name";
  if (field == "bpm") return n.bpm > 0 ? "name" : "";
  if (field == "key") return n.keyClass >= 0 ? "name" : "";
  return "";  // styles, tags and a name: only the file, the metadata file and the user say them
}

json sampleMetadata(const SampleIndex& index, const LibrarySample& s) {
  json origin = json::object();
  for (const auto& f : kSampleMetadataFields) origin[f] = metadataOrigin(index, s, f);
  auto o = index.overrides.find(s.sha256);
  json locations = json::array({s.path});
  for (const auto& p : s.otherPaths) locations.push_back(p);
  json name = sampleInfoJson(s.inferred);
  name.erase("styles");
  name.erase("tags");
  name.erase("name");
  return {{"sample", s.path}, {"displayName", s.displayName}, {"sha256", s.sha256}, {"pack", s.pack}, {"seconds", std::round(s.seconds * 1000) / 1000},
          {"fields", sampleInfoJson(s.info)}, {"origin", origin},
          {"layers", {{"name", name}, {"file", s.embedded.is_object() ? s.embedded : json(nullptr)},
                      {"metadataFile", s.sidecar.is_object() ? s.sidecar : json(nullptr)},
                      {"user", o == index.overrides.end() ? json(nullptr) : o->second}}},
          {"locations", locations},
          {"analysis", s.analysis.is_object() ? s.analysis : json(nullptr)}, {"soundWords", soundWords(s)}};
}

bool needsAnalysis(const LibrarySample& s) {
  SampleAnalysis a;
  return !analysisFromJson(s.analysis, a) || a.sha256 != s.sha256;
}

std::vector<std::string> soundWords(const LibrarySample& s) {
  SampleAnalysis a;
  if (!analysisFromJson(s.analysis, a) || a.sha256 != s.sha256) return {};
  return analysisWords(a);
}

const std::vector<std::string>& sampleRoles() { return kRoles; }
const std::map<std::string, std::string>& sampleRoleAliases() { return kRoleAliases; }
std::string canonicalSampleRole(const std::string& role) { return canonicalRole(role); }

int proposeSampleMetadata(SampleIndex& index, const std::vector<std::string>& sha256s, const json& fields, const std::string& why) {
  json set = normalizeSampleMetadata(fields);
  for (auto f = set.begin(); f != set.end();) f = f->is_null() ? set.erase(f) : std::next(f);  // a proposal says what it is
  if (set.empty()) throw DocumentError("metadata", "no field to propose");
  if (sha256s.empty()) throw DocumentError("samples", "no sample");
  for (const auto& sha : sha256s)
    if (!index.findHash(sha)) throw DocumentError("samples", "no sample with content " + sha.substr(0, 12) + "... in the library");
  std::set<std::string> seen;
  for (const auto& sha : sha256s)
    if (seen.insert(sha).second) index.proposals[sha] = {{"fields", set}, {"why", why}};
  return (int)seen.size();
}

int setSampleMetadata(SampleIndex& index, const std::vector<std::string>& sha256s, const json& fields, const std::string& label) {
  const json set = normalizeSampleMetadata(fields);
  if (set.empty()) throw DocumentError("metadata", "no field to change");
  if (sha256s.empty()) throw DocumentError("samples", "no sample");
  for (const auto& sha : sha256s)
    if (!index.findHash(sha)) throw DocumentError("samples", "no sample with content " + sha.substr(0, 12) + "... in the library");
  CatalogEdit step;
  step.label = label;
  std::set<std::string> seen;
  for (const auto& sha : sha256s) {
    if (!seen.insert(sha).second) continue;
    auto o = index.overrides.find(sha);
    const json before = o == index.overrides.end() ? json(nullptr) : o->second;
    json after = before.is_object() ? before : json::object();
    for (auto it = set.begin(); it != set.end(); ++it) {
      if (it.value().is_null()) after.erase(it.key());
      else after[it.key()] = it.value();
    }
    if (after.empty()) after = nullptr;
    if (after == before) continue;
    step.before.emplace_back(sha, before);
    if (after.is_null()) index.overrides.erase(sha);
    else index.overrides[sha] = after;
  }
  if (step.before.empty()) return 0;
  const int changed = (int)step.before.size();
  index.undo.push_back(std::move(step));
  if (index.undo.size() > kCatalogUndoSteps) index.undo.erase(index.undo.begin());
  resolveCatalog(index);
  return changed;
}

std::string undoCatalogEdit(SampleIndex& index) {
  if (index.undo.empty()) return "";
  CatalogEdit step = std::move(index.undo.back());
  index.undo.pop_back();
  for (const auto& [sha, before] : step.before) {
    if (before.is_null()) index.overrides.erase(sha);
    else index.overrides[sha] = before;
  }
  resolveCatalog(index);
  return step.label;
}

std::map<std::string, json> readSampleMetadataFile(const json& file, const SampleIndex& index, std::vector<std::string>& problems) {
  std::map<std::string, json> out;
  if (!file.is_object() || file.value("format", "") != kSampleMetadataFormat) {
    problems.push_back(std::string(kSampleMetadataFile) + ": not a Core sample metadata file (format \"" + kSampleMetadataFormat + "\")");
    return out;
  }
  if (file.value("version", 0) != 1) {
    problems.push_back(std::string(kSampleMetadataFile) + ": version 1 is the one Core reads");
    return out;
  }
  const json items = file.value("items", json::array());
  if (!items.is_array()) { problems.push_back(std::string(kSampleMetadataFile) + ": items must be a list"); return out; }
  for (size_t i = 0; i < items.size(); ++i) {
    const json& it = items[i];
    const std::string where = "items[" + std::to_string(i) + "]";
    if (!it.is_object()) { problems.push_back(where + ": an object"); continue; }
    auto text = [&](std::initializer_list<const char*> keys) {
      for (const char* k : keys) if (it.contains(k) && it[k].is_string()) return it[k].get<std::string>();
      return std::string();
    };
    const std::string sha = lower(text({"sha256", "contentId"})), path = text({"path", "pathHint"});
    const LibrarySample* s = sha.empty() ? nullptr : index.findHash(sha);
    if (!s && !path.empty()) s = index.find(path);
    if (!s) {
      if (sha.empty() && path.empty()) problems.push_back(where + ": needs sha256 or path");
      continue;  // a sample that is not in this library (yet): not a problem
    }
    const json fields = it.contains("set") ? it["set"] : it.value("overrides", json());
    try {
      json f = normalizeSampleMetadata(fields);
      for (auto x = f.begin(); x != f.end();) x = x->is_null() ? f.erase(x) : std::next(x);
      json& dst = out[s->sha256];
      if (!dst.is_object()) dst = json::object();
      dst.update(f);  // a later item adds to an earlier one
    } catch (const DocumentError& x) {
      problems.push_back(where + " (" + s->path + "): " + x.what());
    }
  }
  return out;
}

// ---------------------------------------------------------------- suggestions

std::string trackSampleRole(const Track& t) {
  const auto ws = words(t.id + " " + t.name);
  if (has(ws, {"open"}) && has(ws, {"hat", "hats", "hh"})) return "openhat";
  if (has(ws, {"kick"})) return "kick";
  if (has(ws, {"bass", "bassline"})) return "bass";
  if (has(ws, {"hat", "hats", "hh", "hihat"})) return "hat";
  if (has(ws, {"accent", "accents", "clap", "snare"})) return "accent";
  if (has(ws, {"perc", "percussion", "tom", "toms"})) return "perc";
  if (has(ws, {"impact", "impacts", "crash"})) return "impact";
  if (has(ws, {"riser", "risers", "sweep"})) return "riser";
  return "";  // synths, pads, textures: they keep their instrument
}

namespace {

// How well a sample role fits a track role (0: not at all).
double roleFit(const std::string& want, const std::string& is) {
  static const std::map<std::string, std::map<std::string, double>> fits = {
      {"kick", {{"kick", 100}}},
      {"bass", {{"bass", 100}}},
      {"hat", {{"hat", 100}, {"openhat", 40}}},
      {"openhat", {{"openhat", 100}, {"hat", 30}, {"crash", 30}}},
      {"perc", {{"perc", 100}, {"tom", 80}, {"snare", 70}, {"clap", 70}}},
      {"accent", {{"clap", 100}, {"snare", 95}, {"perc", 60}}},
      {"impact", {{"impact", 100}, {"crash", 85}, {"fx", 40}}},
      {"riser", {{"riser", 100}, {"fx", 55}}},
      // Kinds a plain Sampler track browses (browseSampleRoles); no track name gives them.
      {"fx", {{"fx", 100}, {"riser", 60}, {"impact", 60}}},
      {"vox", {{"vox", 100}}},
      {"pad", {{"pad", 100}}},
      {"synth", {{"synth", 100}}}};
  auto w = fits.find(want);
  if (w == fits.end()) return 0;
  auto i = w->second.find(is);
  return i == w->second.end() ? 0 : i->second;
}

// Siblings of a bass recorded note by note: same folder, same name once the note is taken out.
std::string noteStem(const LibrarySample& s) {
  static const std::regex note(R"(([A-G])(#|b)?\s?(\d)(?=$|[\s_\-\)\.]))");
  std::string stem = std::regex_replace(s.name, note, "#");
  stem = std::regex_replace(stem, std::regex(R"(^\s*\d+\s*[\.\-]*\s*-?\s*)"), "");
  const size_t slash = s.path.rfind('/');
  return (slash == std::string::npos ? "" : s.path.substr(0, slash)) + "|" + stem;
}

std::vector<const LibrarySample*> multiSampleSet(const SampleIndex& index, const LibrarySample& s) {
  std::vector<const LibrarySample*> set;
  if (s.info.rootKey < 0 || s.info.rootKey > 127 || s.info.loop) return set;
  const std::string stem = noteStem(s);
  std::set<int> keys;
  for (const auto& o : index.samples)
    if (o.info.rootKey >= 0 && o.info.rootKey <= 127 && !o.info.loop && noteStem(o) == stem && keys.insert(o.info.rootKey).second) set.push_back(&o);
  std::sort(set.begin(), set.end(), [](const LibrarySample* a, const LibrarySample* b) { return a->info.rootKey < b->info.rootKey; });
  // An instrument recorded note by note: at least 5 notes, close together (a few one-shots in other
  // octaves that happen to share a name are not one).
  bool close = set.size() >= 5;
  for (size_t i = 1; i < set.size() && close; ++i) close = set[i]->info.rootKey - set[i - 1]->info.rootKey <= 3;
  if (!close) set.clear();
  return set;
}

std::string fmtSeconds(double s) {
  char b[32];
  std::snprintf(b, sizeof b, s < 10 ? "%.2f s" : "%.0f s", s);
  return b;
}

}  // namespace

double fastestTempo(const Document& doc) {
  double bpm = doc.bpm;
  for (const auto& t : doc.tempoMap) bpm = std::max(bpm, t.bpm);
  return bpm;
}

SampleFit sampleFit(const LibrarySample& s, const std::string& trackRole, double bpm) {
  SampleFit f;
  if (roleFit(trackRole, s.info.role) < 70) { f.why = "not a " + trackRole + " sound"; return f; }
  if (bpm <= 0) { f.fits = true; f.why = "no tempo to check"; return f; }
  char b[160];
  if (s.info.loop) {
    if (s.info.bpm <= 0) { f.known = false; f.why = "loop without a known BPM"; return f; }
    for (double k : {1.0, 2.0, 0.5})
      if (std::fabs(s.info.bpm * k / bpm - 1) <= kFitTempoTolerance) {
        f.fits = true;
        std::snprintf(b, sizeof b, k == 1 ? "loop at %.0f BPM" : "loop at %.0f BPM (%s time)", s.info.bpm, k == 2 ? "half" : "double");
        f.why = b;
        return f;
      }
    std::snprintf(b, sizeof b, "loop at %.0f BPM, the song goes to %.0f", s.info.bpm, bpm);
    f.why = b;
    return f;
  }
  static const std::map<std::string, double> share = {{"kick", 1.0}, {"hat", 0.5}, {"openhat", 1.0}, {"accent", 1.0}, {"perc", 1.0}};
  auto sh = share.find(trackRole);
  if (sh == share.end()) { f.fits = true; f.why = "a " + s.info.role + " sound"; return f; }
  SampleAnalysis a;
  if (!analysisFromJson(s.analysis, a) || a.sha256 != s.sha256) { f.known = false; f.why = "not measured yet"; return f; }
  if (a.silent) { f.why = "silent"; return f; }
  if (a.tailMs < 0) { std::snprintf(b, sizeof b, "%d hits, not one", a.hits); f.why = b; return f; }
  const double length = a.attackMs + a.tailMs, room = 60000.0 / bpm * sh->second;
  f.fits = length <= room;
  std::snprintf(b, sizeof b, "%s in %.0f ms, %s at %.0f BPM is %.0f ms", f.fits ? "ends" : "too long: ends", length,
                sh->second == 1 ? "a beat" : "half a beat", bpm, room);
  f.why = b;
  return f;
}

std::vector<SampleSuggestion> suggestSamples(const SampleIndex& index, const std::string& role, const std::string& style, int limit,
                                             const std::string& pack) {
  std::vector<SampleSuggestion> out;
  if (role.empty() || limit <= 0) return out;
  const auto styleWords = words(style);
  const bool dark = has(styleWords, {"dark", "psycore", "darkpsycore"});
  const bool hitech = has(styleWords, {"hitech", "hi", "tech"});
  const bool slam = has(styleWords, {"slambient"});
  const auto folders = index.roleFolders.find(role);
  const bool ownFolders = folders != index.roleFolders.end() && !folders->second.empty();
  for (const auto& s : index.samples) {
    // The role's own folders (Settings > Samplers): only their samples, and each of them is of this role.
    const bool inFolders = ownFolders && inRoleFolders(index, s, role);
    if (ownFolders && !inFolders) continue;
    double fit = inFolders ? 100 : roleFit(role, s.info.role);
    if (fit <= 0) continue;
    const auto fileWords = words(s.name);
    // Stated: by the file's own name, the metadata file, the user or a folder the user gave the role.
    const bool byName = inFolders || roleOf(fileWords) == s.info.role || metadataOrigin(index, s, "role") == "user" ||
                        metadataOrigin(index, s, "role") == "metadata file";
    double score = byName ? fit : fit * 0.7;  // only the folder says so
    std::string why = s.info.loop ? "loop" : "one-shot";
    const bool hitRole = role != "impact" && role != "riser" && role != "fx" && role != "vox" && role != "pad" && role != "synth";
    // A pattern plays hits: a loop on every note would smear. On a kick, bass or hat track a loop is never offered
    // (Diogo 2026-11-06, "many don't even sound like kicks": 7 of the 13 kick suggestions were 5..26 s loops).
    if (s.info.loop && (role == "kick" || role == "bass" || role == "hat" || role == "openhat" || role == "accent")) continue;
    if (s.info.loop && hitRole) score -= 70;
    const double t = s.seconds;
    if (role == "kick") score += t >= 0.1 && t <= 1.0 ? 10 : t > 1.5 ? -30 : 0;
    else if (role == "bass") score += t <= 1.0 ? 10 : t > 2.0 ? -30 : 0;
    else if (role == "hat") score += t <= 0.35 ? 10 : t > 1.0 ? -30 : 0;
    else if (role == "openhat") score += t >= 0.15 && t <= 1.5 ? 5 : t > 3 ? -30 : 0;
    else if (role == "perc" || role == "accent") score += t <= 0.8 ? 5 : t > 2.0 ? -30 : 0;
    else if (role == "impact") score += t >= 0.5 ? 5 : 0;
    else if (role == "riser") score += t > 40 ? -30 : t >= 1.5 ? 10 : t < 0.5 ? -30 : 0;
    if (role == "bass") {
      const auto set = multiSampleSet(index, s);
      if (!set.empty()) {
        if (set.front() != &s) continue;  // the set is suggested once, as its lowest note
        score += 25;
        why = "bass played note by note (" + std::to_string(set.size()) + " notes)";
      } else if (s.info.rootKey >= 0 || s.info.keyClass >= 0) {
        score += 5;
      }
    }
    // The style, from the pack and folder names.
    auto pathWords = words(s.path);
    for (const auto& st : s.info.styles) for (const auto& w : words(st)) pathWords.push_back(w);  // the catalogue's styles count too
    if (has(pathWords, {"psy", "psytrance", "psycore", "darkpsy", "psychedelic", "forest", "hitech"})) score += 5;
    if ((dark && has(pathWords, {"dark", "darkpsy", "psycore", "forest"})) || (hitech && has(pathWords, {"hitech"})) ||
        (slam && has(pathWords, {"slambient", "ambient"})))
      score += 10;
    const bool samePack = !pack.empty() && s.pack == pack && role != "bass";
    if (samePack) score += kSamePackBonus;
    if (why == "one-shot" || why == "loop") why += ", " + fmtSeconds(t);
    if (samePack) why += ", same pack as the bass";
    if (!s.pack.empty()) why += ", " + s.pack;
    out.push_back({&s, score, why});
  }
  std::stable_sort(out.begin(), out.end(), [](const SampleSuggestion& a, const SampleSuggestion& b) {
    return a.score != b.score ? a.score > b.score : a.sample->path < b.sample->path;
  });
  if ((int)out.size() > limit) out.resize((size_t)limit);
  return out;
}

const std::vector<std::string>& samplerRoles() {
  static const std::vector<std::string> roles = {"kick", "bass", "hat", "openhat", "accent", "perc", "impact", "riser"};
  return roles;
}

const std::vector<std::string>& browseSampleRoles() {
  static const std::vector<std::string> roles = [] {
    std::vector<std::string> r = samplerRoles();
    for (const char* k : {"fx", "synth", "pad", "vox"}) r.push_back(k);
    return r;
  }();
  return roles;
}

bool inRoleFolders(const SampleIndex& index, const LibrarySample& sample, const std::string& role) {
  const auto it = index.roleFolders.find(role);
  if (it == index.roleFolders.end()) return false;
  auto inside = [](const std::string& path, const std::string& folder) {
    return !folder.empty() && path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
  };
  for (const auto& f : it->second) {
    if (inside(sample.path, f)) return true;
    for (const auto& p : sample.otherPaths) if (inside(p, f)) return true;
  }
  return false;
}

std::string libraryRelativeFolder(const SampleIndex& index, const std::string& folder) {
  std::string root = index.root, f = folder;
  for (auto* x : {&root, &f}) {
    std::replace(x->begin(), x->end(), '\\', '/');
    while (!x->empty() && x->back() == '/') x->pop_back();
  }
  auto lower = [](std::string x) { for (auto& c : x) c = (char)std::tolower((unsigned char)c); return x; };  // Windows paths
  if (root.empty() || f.size() <= root.size() + 1 || lower(f.substr(0, root.size())) != lower(root) || f[root.size()] != '/') return {};
  return f.substr(root.size() + 1);
}

std::string shownName(const LibrarySample& s) { return s.displayName.empty() ? s.name : s.displayName; }

std::string shownName(const SampleIndex& index, const SampleRef& zone) {
  const LibrarySample* s = nullptr;
  if (!zone.sha256.empty())
    for (const auto& x : index.samples)
      if (x.sha256 == zone.sha256) { s = &x; break; }
  if (!s && !zone.file.empty()) s = index.findFile(zone.file);
  return s ? shownName(*s) : zone.name;
}

SampleRef sampleRef(const LibrarySample& s) {
  SampleRef z;
  z.file = s.file;
  z.sha256 = s.sha256;
  z.name = shownName(s);  // the catalogue's name, as the library shows it
  if (s.embedded.is_object() && s.embedded.contains("loop")) {  // the file's own loop (smpl): the sampler loops it while held
    const json& l = s.embedded["loop"];
    if (l.is_array() && l.size() == 2 && l[0].is_number() && l[1].is_number()) {
      const double a = l[0].get<double>(), b = std::min(l[1].get<double>(), s.seconds > 0 ? s.seconds : l[1].get<double>());
      if (a >= 0 && b - a >= kMinSampleLoopSeconds) { z.loopStart = a; z.loopEnd = b; }
    }
  }
  return z;
}

namespace {
// Its measurements, when they are of this content.
bool measuredNow(const LibrarySample& s, SampleAnalysis& a) { return analysisFromJson(s.analysis, a) && a.sha256 == s.sha256 && !a.silent; }
// The sampler's Level that plays the loudest of these samples at kSamplePeakDb (the library's files are left
// as they are: their peaks go from -1 to -13 dBFS). Unmeasured: Level stays at its default.
void levelToPeak(Instrument& in, const std::vector<const LibrarySample*>& samples) {
  double peak = -1000;
  for (const auto* x : samples) {
    SampleAnalysis m;
    if (!measuredNow(*x, m) || m.peakDb <= -120) return;
    peak = std::max(peak, m.peakDb);
  }
  if (peak <= -1000) return;
  in.params["level"] = std::round(std::clamp(kSamplePeakDb - peak, -24.0, kSampleMaxBoostDb) * 10) / 10;
}
// A hit or a bass note that starts after silence plays late on every note (8 bass one-shots in D:\Samples wait
// 31..115 ms). The sampler starts 2 ms before the sound instead (its 0.1 ms fade-in keeps it click-free).
void skipLeadingSilence(Instrument& in, const LibrarySample& s, const std::string& role) {
  static const std::set<std::string> hits = {"kick", "snare", "clap", "hat", "openhat", "perc", "tom", "bass", "kickbass"};
  SampleAnalysis a;
  if (!hits.count(role) || !measuredNow(s, a) || a.leadingSilenceMs < kLateStartMs) return;
  in.params["start"] = a.leadingSilenceMs - 2;
}
}  // namespace

Instrument samplerInstrument(const SampleIndex& index, const LibrarySample& sample, const std::string& role) {
  Instrument in;
  in.kind = "builtin";
  in.name = "sampler";
  in.version = kSamplerVersion;
  if (role == "bass") {
    in.params["pitched"] = 1;
    in.params["release"] = 20;  // KBBB: short notes, short tails
    auto set = multiSampleSet(index, sample);
    // A note of the set whose audio plays another note than its name says is left out (the notes around it
    // cover its keys): Psiger "16 - D#2" plays a B5, twice as long as its neighbours.
    set.erase(std::remove_if(set.begin(), set.end(), [](const LibrarySample* x) {
                SampleAnalysis m;
                const int note = measuredNow(*x, m) ? pitchToMidi(m.pitchHz) : -1;
                return note >= 0 && std::abs(note - x->info.rootKey) > 1;
              }), set.end());
    if (set.size() == 1) set.clear();  // one note left: played as a single sample below
    if (!set.empty()) {
      for (size_t i = 0; i < set.size(); ++i) {
        SampleRef z = sampleRef(*set[i]);
        z.rootKey = set[i]->info.rootKey;
        z.loKey = i == 0 ? 0 : (set[i - 1]->info.rootKey + z.rootKey) / 2 + 1;
        z.hiKey = i + 1 == set.size() ? 127 : (z.rootKey + set[i + 1]->info.rootKey) / 2;
        in.samples.push_back(z);
      }
      levelToPeak(in, set);
      return in;
    }
    SampleRef z = sampleRef(sample);
    // The note its audio plays, when measured: names are often wrong ("BASS Cm" plays an F, "A#1" plays A#2),
    // and a bass a few semitones off the kick and the rest of the song is what makes it sound wrong.
    // Otherwise a bass note in a psy register (octave 1) unless the name says a low octave; E1 when unknown.
    SampleAnalysis a;
    // Only a bass's register (25..300 Hz): higher is a harmonic taken for the note.
    const int measured = analysisFromJson(sample.analysis, a) && a.sha256 == sample.sha256 && a.pitchHz <= 300 ? pitchToMidi(a.pitchHz) : -1;
    z.rootKey = measured >= 0 ? measured
                : sample.info.rootKey >= 0 && sample.info.rootKey <= 47 ? sample.info.rootKey
                : sample.info.keyClass >= 0 ? 24 + sample.info.keyClass : 28;
    in.samples.push_back(z);
    // A long held bass note (most one-shots in the library sound 0.4..2.7 s) cut every 1/16 sounds flat, like
    // an organ: each note gets a psy pluck instead, falling to -60 dB in 400 ms (about -15 dB by the next 1/16).
    if (analysisFromJson(sample.analysis, a) && a.sha256 == sample.sha256 && a.activeMs > kLongBassMs) in.params["decay"] = kBassPluckMs;
    skipLeadingSilence(in, sample, role);
    levelToPeak(in, {&sample});
    return in;
  }
  in.samples.push_back(sampleRef(sample));  // a one-shot: every key plays it as it is
  if (role == "riser") in.params["retrigger"] = 0;  // a roll of notes plays the riser once
  if (!sample.info.loop) skipLeadingSilence(in, sample, role);
  levelToPeak(in, {&sample});
  return in;
}

namespace {
// Where a riser sample starts so it ends on the drop: the sample's length minus its clip's (the longest
// clip, at the song tempo). A riser shorter than the clip starts with it and ends before the drop.
double riserStartMs(const Document& doc, const Track& t, const LibrarySample& s) {
  int64_t ticks = 0;
  for (const auto& c : t.clips) ticks = std::max(ticks, c.lengthTicks);
  if (ticks <= 0 || doc.bpm <= 0 || s.seconds <= 0) return 0;
  const double clipSeconds = (double)ticks / kPpq * 60.0 / doc.bpm;
  return std::round(std::clamp((s.seconds - clipSeconds) * 1000.0, 0.0, 60000.0));
}
}  // namespace

void setTrackSample(Document& doc, const std::string& trackId, const SampleIndex& index, const LibrarySample& sample) {
  Track& t = trackById(doc, trackId);
  if (!playsPatterns(t)) throw DocumentError("instrument", "an audio track or a bus does not play samples");
  std::string role = trackSampleRole(t);
  if (role.empty()) role = sample.info.role == "bass" ? "bass" : sample.info.role;
  // A new sample starts from its own sound: the sampler's defaults plus what suits this sample (how keys play,
  // whether a note restarts it, where it starts, its level). Nothing of the previous sample's knobs or of its
  // automation carries over, so changing samples again and again never stacks anything; Undo brings the old one back.
  Instrument next = samplerInstrument(index, sample, role);
  // The level brings it to its role's loudness, so the template's mix stays in balance.
  next.params["level"] = levelFor(sample, role);
  if (role == "riser") next.params["start"] = riserStartMs(doc, t, sample);
  t.instrument = next;
  t.automation.erase(std::remove_if(t.automation.begin(), t.automation.end(), [](const Automation& a) {
                       return a.param != "gainDb" && a.param != "pan" && a.param.rfind("fx:", 0) != 0;
                     }), t.automation.end());
}

std::string songStyle(const Document& doc) {
  const auto t = doc.extra.find("template");
  if (t == doc.extra.end() || !t->is_object()) return "";
  const auto st = t->find("style");
  return st != t->end() && st->is_string() ? st->get<std::string>() : "";
}

std::string anchorPack(const Document& doc, const SampleIndex& index) {
  for (const auto& t : doc.tracks) {
    if (trackSampleRole(t) != "bass" || t.instrument.samples.empty()) continue;
    const SampleRef& z = t.instrument.samples.front();
    const LibrarySample* s = index.findFile(z.file);
    if (!s) s = index.findHash(z.sha256);
    if (s) return s->pack;
  }
  return "";
}

namespace {
uint64_t mix64(uint64_t x) {  // splitmix64: the same picks for the same seed on every platform
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
uint64_t hashText(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
  return h;
}
}  // namespace

std::vector<std::string> applySuggestedSamples(Document& doc, const SampleIndex& index, const std::string& style, uint64_t seed) {
  std::vector<std::string> changed;
  // The bass first: the other tracks then prefer its pack (kick and bass made to sit together).
  std::vector<Track*> order;
  for (auto& t : doc.tracks) if (trackSampleRole(t) == "bass") order.push_back(&t);
  for (auto& t : doc.tracks) if (trackSampleRole(t) != "bass") order.push_back(&t);
  // A sample plays on one track only: the ones on tracks this does not change are taken already.
  std::set<std::string> used;
  for (const auto& t : doc.tracks)
    if (trackSampleRole(t).empty()) for (const auto& z : t.instrument.samples) used.insert(z.sha256.empty() ? z.file : z.sha256);
  auto key = [](const LibrarySample& s) { return s.sha256.empty() ? s.file : s.sha256; };
  for (Track* t : order) {
    if (!playsPatterns(*t)) continue;
    const std::string role = trackSampleRole(*t);
    if (role.empty()) continue;
    std::vector<SampleSuggestion> pool;
    for (const auto& c : suggestSamples(index, role, style, 40, anchorPack(doc, index)))
      if (c.score >= kGoodSuggestion && !used.count(key(*c.sample))) pool.push_back(c);
    if (pool.empty()) continue;
    const SampleSuggestion* pick = &pool.front();
    if (seed != 0) {
      // Any good sample close to the best can come out, the better ones more often.
      const double floor = pool.front().score - kVarietyRange;
      double total = 0;
      for (const auto& c : pool) if (c.score >= floor) total += c.score - floor + 1;
      double r = (double)(mix64(seed ^ hashText(t->id)) >> 11) / 9007199254740992.0 * total;
      for (const auto& c : pool) {
        if (c.score < floor) break;
        pick = &c;
        r -= c.score - floor + 1;
        if (r < 0) break;
      }
    }
    setTrackSample(doc, t->id, index, *pick->sample);
    for (const LibrarySample* s : multiSampleSet(index, *pick->sample)) used.insert(key(*s));
    used.insert(key(*pick->sample));
    changed.push_back(t->name + ": " + shownName(*pick->sample));
  }
  return changed;
}

int relinkMovedSamples(Document& doc, const SampleIndex& index) {
  int moved = 0;
  for (auto& t : doc.tracks)
    for (auto& z : t.instrument.samples) {
      std::error_code e;
      if (fs::exists(fromUtf8(z.file), e)) continue;
      if (!z.sha256.empty()) {
        if (const LibrarySample* s = index.findHash(z.sha256)) { z.file = s->file; ++moved; }
        continue;  // known content not in the library: stays missing, never another sound
      }
      // No sha256 (an older template or song): the same place inside the library, e.g. D:\Samples\Bass\x.wav after
      // the library moved into Core's own folder is <install>\Samples\Bass\x.wav. The longest tail of the old
      // path that is a path of the library (folder and file at least), so a file of the same name elsewhere is
      // never taken.
      std::vector<std::string> parts;
      std::string part;
      for (char c : z.file) {
        if (c == '/' || c == '\\') { if (!part.empty()) parts.push_back(part); part.clear(); }
        else part += c;
      }
      if (!part.empty()) parts.push_back(part);
      for (size_t from = 1; from + 2 <= parts.size(); ++from) {
        std::string tail;
        for (size_t k = from; k < parts.size(); ++k) tail += (tail.empty() ? "" : "/") + parts[k];
        if (const LibrarySample* s = index.find(tail); s && !s->file.empty()) {
          z.file = s->file;
          z.sha256 = s->sha256;
          ++moved;
          break;
        }
      }
    }
  return moved;
}

// ---- the real notes of a region

namespace {
// The note starts of a track in [from, to), in ticks of the song (patterns repeat inside their clips).
std::vector<int64_t> noteStarts(const Track& t, int64_t from, int64_t to) {
  std::vector<int64_t> out;
  for (const auto& c : t.clips) {
    const Pattern* p = t.findPattern(c.patternId);
    if (!p || p->lengthTicks <= 0 || p->notes.empty()) continue;
    const int64_t clipEnd = c.startTick + c.lengthTicks;
    const int64_t lo = std::max(from, c.startTick), hi = std::min(to, clipEnd);
    if (lo >= hi) continue;
    const int64_t origin = c.startTick - c.offsetTicks;  // where the pattern's tick 0 would be
    for (int64_t k = std::max<int64_t>(0, (lo - origin) / p->lengthTicks - 1); origin + k * p->lengthTicks < hi; ++k)
      for (const auto& n : p->notes) {
        const int64_t at = origin + k * p->lengthTicks + n.tick;
        if (at >= lo && at < hi && n.tick < p->lengthTicks) out.push_back(at);
      }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
}  // namespace

RegionFit sampleFitInRegion(const Document& doc, const std::string& trackId, const LibrarySample& sample, int64_t startTick, int64_t endTick) {
  RegionFit f;
  const Track* track = doc.findTrack(trackId);
  if (!track) { f.why = "no track '" + trackId + "'"; return f; }
  const std::string role = trackSampleRole(*track);
  if (role != "kick") { f.why = "only a kick is checked against the bass notes"; return f; }
  SampleAnalysis a;
  if (!analysisFromJson(sample.analysis, a) || a.sha256 != sample.sha256) { f.why = "the sample is not measured yet"; return f; }
  if (a.silent || a.tailMs < 0) { f.why = a.silent ? "the sample is silent" : "the sample has several hits"; return f; }
  f.soundMs = std::round(a.attackMs + a.tailMs);
  const int64_t from = std::max<int64_t>(0, startTick), to = std::min(endTick, doc.lengthTicks);
  const std::vector<int64_t> kicks = noteStarts(*track, from, to);
  std::vector<int64_t> others;
  std::vector<std::string> names;
  for (const auto& t : doc.tracks) {
    if (t.id == trackId || t.mix.mute || trackSampleRole(t) != "bass") continue;
    const auto n = noteStarts(t, from, doc.lengthTicks);
    others.insert(others.end(), n.begin(), n.end());
    names.push_back(t.name.empty() ? t.id : t.name);
  }
  std::sort(others.begin(), others.end());
  for (const auto& n : names) f.against += (f.against.empty() ? "" : ", ") + n;
  f.hits = (int)kicks.size();
  if (kicks.empty()) { f.why = "the track plays no note in the region"; return f; }
  if (others.empty()) { f.why = "no bass note to leave room for"; return f; }
  for (const int64_t k : kicks) {
    const auto next = std::upper_bound(others.begin(), others.end(), k);
    if (next == others.end()) continue;
    const double gap = (secondsAt(doc, (double)*next) - secondsAt(doc, (double)k)) * 1000.0;
    if (f.worstGapMs < 0 || gap < f.worstGapMs) f.worstGapMs = std::round(gap);
    if (f.soundMs > gap) ++f.overlaps;
  }
  if (f.worstGapMs < 0) { f.why = "no bass note after the kicks of the region"; return f; }
  f.known = true;
  f.fits = f.overlaps == 0;
  char b[200];
  std::snprintf(b, sizeof b, f.fits ? "sounds %.0f ms; the bass comes %.0f ms after a kick at the soonest"
                                    : "sounds %.0f ms, the bass comes %.0f ms after a kick at the soonest: %d of %d kicks run into it",
                f.soundMs, f.worstGapMs, f.overlaps, f.hits);
  f.why = b;
  return f;
}

Document withPreviewTrack(const Document& doc, const SampleIndex& index, const LibrarySample& sample, const std::string& role,
                          std::string& trackId, int& pitch) {
  Document d = doc;
  if (d.tracks.size() >= 128) d.tracks.pop_back();  // the engine's limit: a preview is never refused for it
  const std::string r = role.empty() ? (sample.info.role == "bass" ? "bass" : sample.info.role) : role;
  Track t;
  t.id = "sample-preview";
  for (int n = 2; d.findTrack(t.id); ++n) t.id = "sample-preview-" + std::to_string(n);
  t.name = "Sample preview";
  t.instrument = samplerInstrument(index, sample, r);
  t.instrument.params["level"] = levelFor(sample, r);
  if (r == "riser") t.instrument.params["start"] = 0;
  Pattern p;
  p.id = "p1";
  p.name = "1";
  p.lengthTicks = 4 * kPpq;
  t.patterns.push_back(p);
  pitch = !t.instrument.samples.empty() && t.instrument.samples.front().rootKey >= 0 ? t.instrument.samples.front().rootKey : 60;
  trackId = t.id;
  d.tracks.push_back(std::move(t));
  return d;
}

} // namespace core::doc
