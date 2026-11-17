#include "assets.hpp"
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace core {

std::shared_ptr<const AudioData> AssetStore::get(const std::string& path, double sampleRate, std::string* error) {
  std::lock_guard<std::mutex> g(mutex_);
  // The file's size and time are part of the key: a file replaced on disk is read again.
  std::error_code e;
  const std::filesystem::path p(std::u8string(path.begin(), path.end()));
  const auto size = std::filesystem::file_size(p, e);
  const auto time = std::filesystem::last_write_time(p, e).time_since_epoch().count();
  const std::string key = path + "|" + std::to_string(sampleRate) + "|" + std::to_string(e ? 0 : size) + "|" + std::to_string(e ? 0 : time);
  if (auto it = files_.find(key); it != files_.end()) {
    // The same size and the same date are not the same audio: a file written again in place has both (audit
    // 2026-11-17, #3b). Only the header is read here, once per graph build and never per audio block; when it
    // says something else than what is cached, the entry goes and the file is read again.
    WavInfo head;
    std::string why;
    bool stale = false;
    if (wavInfo(path, &head, &why) && head.frames > 0 && head.sampleRate > 0) {
      // What the cached buffer would have to be, with the same rounding resample uses.
      const double exact = (double)head.frames * sampleRate / head.sampleRate;
      const int64_t atSongRate = (int64_t)std::ceil(exact - exact * 1e-12);
      stale = head.channels != it->second->channels || atSongRate != it->second->frames();
    }
    if (!stale) return it->second;
    files_.erase(it);
  }
  try {
    size_t used = 0;
    for (const auto& [k, data] : files_) used += data->bytes();
    // Frames that fit in what is left, at the source rate (a rough bound before the conversion).
    const int64_t room = (int64_t)((maxBytes_ > used ? maxBytes_ - used : 0) / (2 * sizeof(float)));
    // The header is read first (audit 2026-11-17 item 10): what the file becomes at the song's rate is
    // known before any audio is allocated, so a file that cannot fit is refused without being read and
    // converted. Loading a long 8 kHz take into a 48 kHz song used to touch six times its own size
    // before the limit was noticed and the clip left silent.
    WavInfo info;
    std::string why;
    if (wavInfo(path, &info, &why) && info.frames > 0 && info.sampleRate > 0) {
      // The same rounding resample uses, so the pre-check and the buffer it produces agree.
      const double exact = (double)info.frames * sampleRate / info.sampleRate;
      const int64_t atSongRate = (int64_t)std::ceil(exact - exact * 1e-12);
      if (info.frames > room || atSongRate > room) throw std::runtime_error(path + ": not enough memory for more audio (limit " + std::to_string(maxBytes_ >> 20) + " MB).");
    }
    AudioData source = readWav(path, room);
    // At the song's own rate there is nothing to convert: the file is used as it came in, instead of
    // being copied once more through the whole of its size (the same audit item).
    AudioData converted = source.sampleRate == sampleRate ? std::move(source) : resample(source, sampleRate);
    auto data = std::make_shared<const AudioData>(std::move(converted));
    if (used + data->bytes() > maxBytes_) throw std::runtime_error(path + ": not enough memory for more audio (limit " + std::to_string(maxBytes_ >> 20) + " MB).");
    files_[key] = data;
    return data;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return nullptr;
  }
}

void AssetStore::prune() {
  std::lock_guard<std::mutex> g(mutex_);
  for (auto it = files_.begin(); it != files_.end();)
    it = it->second.use_count() == 1 ? files_.erase(it) : std::next(it);
}

size_t AssetStore::bytes() const {
  std::lock_guard<std::mutex> g(mutex_);
  size_t used = 0;
  for (const auto& [k, data] : files_) used += data->bytes();
  return used;
}

} // namespace core
