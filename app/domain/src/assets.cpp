#include "assets.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include "history.hpp"
#include "sha256.hpp"

namespace core::doc {

namespace fs = std::filesystem;

namespace {
uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
std::string utf8(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// A safe file name: letters, digits, '-', '_' and '.' from the original name.
std::string safeName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    const bool ok = std::isalnum(c) || c == '-' || c == '_' || c == '.';
    // Two dots in a row become "._" so the name is never read as a step out of the folder.
    out += !ok ? '_' : (c == '.' && !out.empty() && out.back() == '.') ? '_' : (char)c;
  }
  return out.substr(0, 60);
}
}  // namespace

fs::path assetFile(const Asset& asset, const fs::path& projectDir) { return projectDir / fromUtf8(asset.path); }

WavInfo readWavInfo(const fs::path& file) {
  std::error_code e;
  const uintmax_t fileSize = fs::file_size(file, e);
  std::ifstream in(file, std::ios::binary);
  if (e || !in) throw DocumentError("file", "cannot be read");
  unsigned char riff[12];
  if (!in.read((char*)riff, 12) || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4))
    throw DocumentError("file", "is not a WAV file (only WAV can be imported for now)");
  WavInfo info;
  int format = 0;
  bool haveFormat = false;
  uint64_t pos = 12;  // where the next chunk starts; every chunk must fit inside the file
  for (;;) {
    unsigned char head[8];
    if (pos + 8 > fileSize || !in.read((char*)head, 8)) throw DocumentError("file", "has no audio data");
    pos += 8;
    const uint32_t size = u32(head + 4);
    const uint64_t left = fileSize - pos;
    if (!std::memcmp(head, "fmt ", 4)) {
      if (size < 16 || size > left) throw DocumentError("file", "has a broken format header");
      std::vector<unsigned char> f(size);
      if (!in.read((char*)f.data(), size)) throw DocumentError("file", "has a broken format header");
      format = u16(&f[0]);
      info.channels = u16(&f[2]);
      info.sampleRate = u32(&f[4]);
      info.bits = u16(&f[14]);
      if (format == 0xFFFE && size >= 26) format = u16(&f[24]);
      haveFormat = true;
      pos += size;
      if ((size & 1) && pos < fileSize) { in.ignore(1); ++pos; }
      continue;
    }
    if (!std::memcmp(head, "data", 4)) {
      if (!haveFormat) throw DocumentError("file", "has its audio before its format header");
      info.isFloat = format == 3;
      info.dataOffset = (int64_t)pos;
      const bool ok = (format == 1 && (info.bits == 16 || info.bits == 24 || info.bits == 32)) || (format == 3 && (info.bits == 32 || info.bits == 64));
      if (!ok) throw DocumentError("file", "uses a sample format that is not supported (PCM 16/24/32-bit or float)");
      if (info.channels != 1 && info.channels != 2) throw DocumentError("file", "must be mono or stereo");
      if (!(info.sampleRate >= 8000 && info.sampleRate <= 384000)) throw DocumentError("file", "has an unsupported sample rate");
      const int64_t frameBytes = (int64_t)info.bits / 8 * info.channels;
      int64_t bytes = size;
      // Size 0 with another chunk right after it (LIST...) is an empty data chunk, not a stream (part 21, B2).
      bool chunkAfter = false;
      if (size == 0 && left >= 8) {
        unsigned char h[8];
        const auto here = in.tellg();
        if (in.read((char*)h, 8)) {
          chunkAfter = true;
          for (int i = 0; i < 4; ++i) if (!std::isalnum(h[i]) && h[i] != ' ') chunkAfter = false;
          if (u32(h + 4) > left - 8) chunkAfter = false;
        }
        in.clear();
        in.seekg(here);
      }
      if (size == 0xFFFFFFFFu || (size == 0 && !chunkAfter)) {
        // Streaming writers (size not known when the header was written): the audio goes to the end of
        // the file, whole frames only.
        bytes = (int64_t)left / frameBytes * frameBytes;
      } else {
        // The header promises `size` bytes: they must all be there, in whole frames (a cut copy or a
        // broken download would otherwise be taken for a shorter take).
        if (size > left) throw DocumentError("file", "is cut short: its header promises more audio than the file holds");
        if (bytes % frameBytes) throw DocumentError("file", "has a broken audio chunk (not a whole number of frames)");
      }
      info.frames = bytes / frameBytes;
      if (info.frames <= 0) throw DocumentError("file", "has no audio");
      return info;
    }
    // Any other chunk (LIST, bext, cue, ...) is skipped; its pad byte may be missing at the very end.
    if (size > left) throw DocumentError("file", "is cut short or has a broken chunk");
    pos += size + (size & 1);
    if (pos > fileSize) pos = fileSize;
    in.seekg((std::streamoff)pos);
  }
}

namespace {
// INFO texts are 8-bit with no declared encoding: valid UTF-8 stays, anything else is read as Latin-1.
std::string infoText(const unsigned char* p, size_t n) {
  std::string raw((const char*)p, n);
  if (const size_t z = raw.find('\0'); z != std::string::npos) raw.resize(z);
  bool utf = true;
  for (size_t i = 0; i < raw.size() && utf;) {
    const unsigned char c = (unsigned char)raw[i];
    const size_t more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
    if (more == 9 || i + more >= raw.size() + (more ? 0 : 1)) { utf = false; break; }
    for (size_t k = 1; k <= more; ++k) utf = utf && ((unsigned char)raw[i + k] >> 6) == 2;
    i += 1 + more;
  }
  std::string out;
  if (utf) out = raw;
  else
    for (unsigned char c : raw) {
      if (c < 0x80) out += (char)c;
      else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    }
  for (char& c : out) if ((unsigned char)c < 0x20) c = ' ';
  const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
  return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}
}  // namespace

WavTags readWavTags(const fs::path& file) {
  WavTags tags;
  std::error_code e;
  const uintmax_t fileSize = fs::file_size(file, e);
  std::ifstream in(file, std::ios::binary);
  unsigned char riff[12];
  if (e || !in || !in.read((char*)riff, 12) || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4)) return tags;
  uint64_t pos = 12;
  for (int guard = 0; guard < 4096 && pos + 8 <= fileSize; ++guard) {
    in.seekg((std::streamoff)pos);
    unsigned char head[8];
    if (!in.read((char*)head, 8)) break;
    const uint64_t size = u32(head + 4), body = pos + 8;
    if (size > fileSize - body) break;  // broken: stop, keep what was read
    const bool list = !std::memcmp(head, "LIST", 4), smpl = !std::memcmp(head, "smpl", 4), acid = !std::memcmp(head, "acid", 4);
    if ((list && size <= (1u << 20)) || (smpl && size >= 36 && size <= (1u << 16)) || (acid && size >= 24 && size <= 4096)) {
      std::vector<unsigned char> b((size_t)size);
      if (!in.read((char*)b.data(), (std::streamsize)size)) break;
      if (list && size >= 4 && !std::memcmp(b.data(), "INFO", 4)) {
        for (size_t q = 4; q + 8 <= b.size();) {
          const size_t n = u32(&b[q + 4]);
          if (n > b.size() - q - 8) break;
          bool id = true;
          for (int k = 0; k < 4; ++k) id = id && std::isalnum(b[q + k]);
          if (id) {
            const std::string text = infoText(&b[q + 8], std::min<size_t>(n, 4096));
            if (!text.empty()) tags.info.emplace(std::string((const char*)&b[q], 4), text);
          }
          q += 8 + n + (n & 1);
        }
      } else if (smpl) {
        const uint32_t loops = u32(&b[28]);
        for (uint32_t k = 0; k < loops && 36 + 24 * (size_t)(k + 1) <= b.size(); ++k) {
          const unsigned char* l = &b[36 + 24 * (size_t)k];
          const uint32_t type = u32(l + 4), start = u32(l + 8), end = u32(l + 12);
          if (type == 0 && end > start) { tags.loopStart = start; tags.loopEnd = (int64_t)end + 1; break; }  // smpl's end is inclusive
        }
      } else if (acid) {
        const uint32_t flags = u32(&b[0]);
        tags.hasAcid = true;
        tags.acidOneShot = flags & 1;
        if (flags & 2) {
          const int note = u16(&b[4]);
          if (note >= 0 && note <= 127) tags.acidRootKey = note;
        }
        float bpm;
        std::memcpy(&bpm, &b[20], 4);
        if (std::isfinite(bpm) && bpm >= 40 && bpm <= 999) tags.acidBpm = std::round((double)bpm * 100) / 100;
      }
    }
    pos = body + size + (size & 1);
  }
  return tags;
}

namespace {
// Puts the file at target unless target already has this content. Copies to a temporary name, checks
// it, then renames: a half-copied file is never taken for the asset.
void place(const fs::path& file, const fs::path& target, const std::string& hash) {
  std::error_code e;
  if (fs::exists(target, e) && sha256File(target) == hash) return;
  fs::create_directories(target.parent_path(), e);
  const fs::path part = fs::path(target.native() + fs::path(".part").native());
  fs::copy_file(file, part, fs::copy_options::overwrite_existing, e);
  if (e || sha256File(part) != hash) { fs::remove(part, e); throw DocumentError("file", "could not be copied into the project folder"); }
  fs::rename(part, target, e);  // replaces a file with other content
  if (e) throw DocumentError("file", "could not be copied into the project folder: " + e.message());
}

}  // namespace

std::string importAudio(Document& doc, const fs::path& file, const fs::path& projectDir) {
  const WavInfo info = readWavInfo(file);
  const std::string hash = sha256File(file);
  for (const auto& a : doc.assets)
    if (a.sha256 == hash) { place(file, assetFile(a, projectDir), hash); return a.id; }  // already in the project (repaired if needed)
  const std::string name = utf8(file.filename());
  const std::string rel = std::string(kAssetFolder) + "/" + hash.substr(0, 12) + "-" + safeName(utf8(file.stem())) + ".wav";
  place(file, projectDir / fromUtf8(rel), hash);
  Asset a;
  long highest = 0;
  for (const auto& x : doc.assets) if (x.id.rfind("a", 0) == 0) highest = std::max(highest, idNumber(x.id.c_str() + 1));
  a.id = "a" + std::to_string(highest + 1);
  a.name = cutUtf8(name, 200);
  a.path = rel;
  a.sha256 = hash;
  a.sampleRate = info.sampleRate;
  a.channels = info.channels;
  a.frames = info.frames;
  doc.assets.push_back(a);
  return a.id;
}

std::vector<std::string> missingAssets(const Document& doc, const fs::path& projectDir) {
  std::vector<std::string> out;
  for (const auto& a : doc.assets) {
    std::error_code e;
    const fs::path f = assetFile(a, projectDir);
    // Size is checked on open (cheap); the hash when relocating or importing.
    if (!fs::exists(f, e)) { out.push_back(a.id); continue; }
    try {
      const WavInfo w = readWavInfo(f);  // a cut file fails here (the header promises more than is there)
      if (w.frames != a.frames || w.channels != a.channels || w.sampleRate != a.sampleRate) out.push_back(a.id);
    } catch (const std::exception&) {
      out.push_back(a.id);
    }
  }
  return out;
}

void relocateAsset(Document& doc, const std::string& assetId, const fs::path& file, const fs::path& projectDir) {
  Asset* asset = nullptr;
  for (auto& a : doc.assets) if (a.id == assetId) asset = &a;
  if (!asset) throw DocumentError("assets", "no asset '" + assetId + "'");
  if (sha256File(file) != asset->sha256) throw DocumentError("file", "is not the same audio as the missing file (different content)");
  place(file, assetFile(*asset, projectDir), asset->sha256);  // copied, checked, then put in place
}

int copyAssets(const Document& doc, const fs::path& fromDir, const fs::path& toDir) {
  std::error_code same;
  if (fs::equivalent(fromDir, toDir, same)) return 0;
  // First every check, then the copies: a conflict leaves the other folder as it was.
  std::vector<const Asset*> toCopy;
  std::string conflicts;
  for (const auto& a : doc.assets) {
    const fs::path from = assetFile(a, fromDir), to = assetFile(a, toDir);
    std::error_code e;
    if (fs::exists(to, e)) {
      // Already there only if it is the same audio. A file with this name and other content (changed,
      // cut short, another take) is never taken for the asset, and never overwritten without asking.
      if (fs::is_regular_file(to, e) && sha256File(to) == a.sha256) continue;
      conflicts += "\n" + utf8(to);
      continue;
    }
    if (fs::is_regular_file(from, e)) toCopy.push_back(&a);  // (missing here: it stays reported as missing)
  }
  if (!conflicts.empty())
    throw DocumentError("assets", "the other folder already has files with these names but different audio (move or delete them, then save again):" + conflicts);
  // Each file is read once: copied to a temporary name, hashed there, then put in place. A source that
  // changed since it was imported is not copied (it stays reported as missing, never as other audio).
  int copied = 0;
  for (const Asset* a : toCopy) {
    const fs::path to = assetFile(*a, toDir), part = fs::path(to.native() + fs::path(".part").native());
    std::error_code e;
    fs::create_directories(to.parent_path(), e);
    fs::copy_file(assetFile(*a, fromDir), part, fs::copy_options::overwrite_existing, e);
    if (e) { fs::remove(part, e); throw DocumentError("assets", "could not copy " + a->name + " into the other folder"); }
    if (sha256File(part) != a->sha256) { fs::remove(part, e); continue; }
    fs::rename(part, to, e);
    if (e) { fs::remove(part, e); throw DocumentError("assets", "could not copy " + a->name + " into the other folder: " + e.message()); }
    ++copied;
  }
  return copied;
}

void markMadeFrom(Document& doc, const std::string& made, const std::string& from, const std::string& what) {
  if (made == from) return;
  const Asset* source = doc.findAsset(from);
  for (auto& a : doc.assets)
    if (a.id == made && a.madeFrom.empty() && source) {
      a.madeFrom = source->sha256;
      a.made = what.substr(0, 80);
    }
}

int clipsUsingAsset(const Document& doc, const std::string& assetId) {
  int n = 0;
  for (const auto& t : doc.tracks) for (const auto& c : t.audioClips) n += c.assetId == assetId;
  return n;
}

std::vector<std::string> unusedAssets(const Document& doc) {
  std::vector<std::string> out;
  for (const auto& a : doc.assets) if (clipsUsingAsset(doc, a.id) == 0 && !isProjectSample(a)) out.push_back(a.id);
  return out;
}

bool isProjectSample(const Asset& asset) {
  return asset.extra.is_object() && asset.extra.value("sample", false);
}

std::string saveProjectSample(Document& doc, const fs::path& file, const fs::path& projectDir, const std::string& name,
                              const std::string& fromAsset, const std::string& what) {
  const std::string id = importAudio(doc, file, projectDir);
  for (auto& a : doc.assets)
    if (a.id == id) {
      if (!name.empty()) a.name = name.size() > 4 && name.substr(name.size() - 4) == ".wav" ? name : name + ".wav";
      if (!a.extra.is_object()) a.extra = json::object();
      a.extra["sample"] = true;
    }
  if (!fromAsset.empty()) markMadeFrom(doc, id, fromAsset, what);
  return id;
}

std::vector<Asset> removeUnusedAssets(Document& doc) {
  std::vector<Asset> removed;
  const auto unused = unusedAssets(doc);
  std::vector<Asset> kept;
  for (auto& a : doc.assets) {
    if (std::find(unused.begin(), unused.end(), a.id) != unused.end()) removed.push_back(std::move(a));
    else kept.push_back(std::move(a));
  }
  doc.assets = std::move(kept);
  // A file is only given back when no asset that stays points at it.
  removed.erase(std::remove_if(removed.begin(), removed.end(), [&](const Asset& r) {
                  return std::any_of(doc.assets.begin(), doc.assets.end(), [&](const Asset& a) { return a.path == r.path; });
                }), removed.end());
  return removed;
}

std::vector<fs::path> strayAudioFiles(const Document& doc, const fs::path& projectDir) {
  std::vector<fs::path> out;
  std::error_code e;
  const fs::path folder = projectDir / kAssetFolder;
  if (projectDir.empty() || !fs::is_directory(folder, e)) return out;
  std::set<fs::path> known;
  for (const auto& a : doc.assets) known.insert(fs::weakly_canonical(assetFile(a, projectDir), e));
  for (fs::directory_iterator it(folder, e), end; !e && it != end; it.increment(e)) {
    if (!it->is_regular_file(e)) continue;
    std::string ext = utf8(it->path().extension());
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext != ".wav") continue;  // ".part" files of a copy in progress and anything else stay
    if (!known.count(fs::weakly_canonical(it->path(), e))) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<fs::path> filesNoStateNeeds(const History& history, const std::vector<fs::path>& files, const fs::path& projectDir) {
  std::error_code e;
  std::set<fs::path> needed;
  history.forEachState([&](const Document& d) {
    for (const auto& a : d.assets) needed.insert(fs::weakly_canonical(assetFile(a, projectDir), e));
  });
  std::vector<fs::path> out;
  for (const auto& f : files)
    if (!needed.count(fs::weakly_canonical(f, e))) out.push_back(f);
  return out;
}

} // namespace core::doc
