#include "audio_file.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace core {

namespace {

uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::filesystem::path utf8Path(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// Whether the next 8 bytes are a chunk header (four letters, digits or spaces, and a size that fits in the file).
// The stream is left where it was.
bool chunkFollows(std::istream& in) {
  const auto here = in.tellg();
  in.seekg(0, std::ios::end);
  const auto end = in.tellg();
  in.seekg(here);
  unsigned char h[8];
  const bool read = (bool)in.read((char*)h, 8);
  in.clear();
  in.seekg(here);
  if (!read) return false;
  for (int i = 0; i < 4; ++i)
    if (!std::isalnum(h[i]) && h[i] != ' ') return false;
  return (int64_t)u32(h + 4) <= (int64_t)(end - here) - 8;
}


// The chunks up to the audio data: the format and how many frames the file really holds. Nothing past
// the headers is read and no audio-sized buffer is allocated, so a caller can decide before loading
// (audit 2026-11-17 item 10: the memory limit used to be found out only after the file had been read
// and converted). The stream is left at the first audio frame. Throws std::runtime_error with the same
// messages readWav has always used.
struct WavHeader {
  int format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  int64_t frames = 0;
  std::streamoff dataAt = 0;
};

WavHeader parseWavHeader(std::istream& in, const std::string& path) {
  unsigned char riff[12];
  if (!in.read((char*)riff, 12) || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4))
    throw std::runtime_error(path + " is not a WAV file.");
  WavHeader h;
  bool haveFormat = false;
  for (;;) {
    unsigned char head[8];
    if (!in.read((char*)head, 8)) throw std::runtime_error(path + ": no audio data found.");
    const uint32_t size = u32(head + 4);
    if (!std::memcmp(head, "fmt ", 4)) {
      // Only the fields used are read (at most 40 bytes); the rest of the chunk is skipped. Its declared size never
      // decides an allocation (audit of main fa3b428, F08: a 20-byte file declaring 512 MB threw bad_alloc).
      unsigned char fmt[40] = {};
      const auto here = in.tellg();
      in.seekg(0, std::ios::end);
      const auto left = in.tellg() - here;
      in.seekg(here);
      if (size < 16 || (int64_t)size > (int64_t)left) throw std::runtime_error(path + ": broken format header.");
      const uint32_t used = std::min<uint32_t>(size, sizeof fmt);
      if (!in.read((char*)fmt, used)) throw std::runtime_error(path + ": broken format header.");
      in.ignore(size - used);
      h.format = u16(&fmt[0]);
      h.channels = u16(&fmt[2]);
      h.rate = u32(&fmt[4]);
      h.bits = u16(&fmt[14]);
      if (h.format == 0xFFFE && size >= 26) h.format = u16(&fmt[24]);  // WAVE_FORMAT_EXTENSIBLE: the sub-format
      haveFormat = true;
      if (size & 1) in.ignore(1);
      continue;
    }
    if (!std::memcmp(head, "data", 4)) {
      if (!haveFormat) throw std::runtime_error(path + ": audio data before its format.");
      if (h.format != 1 && h.format != 3) throw std::runtime_error(path + ": only PCM or float WAV files are supported.");
      if (h.channels != 1 && h.channels != 2) throw std::runtime_error(path + ": only mono or stereo files are supported (" + std::to_string(h.channels) + " channels).");
      if (!(h.rate >= 8000 && h.rate <= 384000)) throw std::runtime_error(path + ": unsupported sample rate " + std::to_string(h.rate) + ".");
      const bool ok = (h.format == 1 && (h.bits == 16 || h.bits == 24 || h.bits == 32)) || (h.format == 3 && (h.bits == 32 || h.bits == 64));
      if (!ok) throw std::runtime_error(path + ": unsupported sample format (" + std::to_string(h.bits) + "-bit).");
      const int frameBytes = (h.bits / 8) * h.channels;
      h.frames = size / frameBytes;
      // Streaming writers: the data runs to the end of the file. Only when its length is really unknown: 0xFFFFFFFF,
      // or 0 with no chunk after it. A legal empty "data" followed by another chunk (LIST...) is 0 frames, not the
      // metadata read as audio (audit 2026-11-08 part 21, B2: 5 frames of noise, peak 0.79).
      if (size == 0xFFFFFFFFu || (size == 0 && !chunkFollows(in))) {
        const auto here = in.tellg();
        in.seekg(0, std::ios::end);
        h.frames = (int64_t)(in.tellg() - here) / frameBytes;
        in.seekg(here);
      }
      {  // never more frames than the file holds: a size that says more does not decide the allocation
        const auto here = in.tellg();
        in.seekg(0, std::ios::end);
        h.frames = std::min<int64_t>(h.frames, (int64_t)(in.tellg() - here) / frameBytes);
        in.seekg(here);
      }
      h.dataAt = in.tellg();
      return h;
    }
    in.ignore((std::streamsize)size + (std::streamsize)(size & 1));  // another chunk (LIST, bext...); 64-bit: 0xFFFFFFFF + 1 is not 0 (sweep)
  }
}

}  // namespace

AudioData readWav(const std::string& path, int64_t maxFrames) {
  std::ifstream in(utf8Path(path), std::ios::binary);
  if (!in) throw std::runtime_error("Cannot open " + path + ".");
  const WavHeader h = parseWavHeader(in, path);
  if (h.frames > maxFrames) throw std::runtime_error(path + " is too long to load (" + std::to_string(h.frames) + " frames).");
  in.seekg(h.dataAt);
  const int bytes = h.bits / 8, frameBytes = bytes * h.channels;
  int64_t frames = h.frames;
  AudioData out;
  out.sampleRate = h.rate;
  out.channels = h.channels;
  out.L.resize((size_t)frames);
  if (h.channels == 2) out.R.resize((size_t)frames);
  std::vector<unsigned char> buf((size_t)frameBytes * 65536);
  for (int64_t at = 0; at < frames;) {
    const int64_t n = std::min<int64_t>(65536, frames - at);
    // A file cut short: the whole frames that were read are kept, then it stops (audit of main fa3b428, F07:
    // they were left as silence).
    const bool cut = !in.read((char*)buf.data(), n * frameBytes);
    const int64_t got = cut ? (int64_t)in.gcount() / frameBytes : n;
    for (int64_t i = 0; i < got; ++i)
      for (int c = 0; c < h.channels; ++c) {
        const unsigned char* p = &buf[(size_t)(i * frameBytes + c * bytes)];
        float v;
        if (h.format == 3 && h.bits == 32) std::memcpy(&v, p, 4);
        else if (h.format == 3) { double d; std::memcpy(&d, p, 8); v = (float)d; }
        else if (h.bits == 16) v = (int16_t)u16(p) / 32768.0f;
        else if (h.bits == 24) v = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.0f;
        else v = (int32_t)u32(p) / 2147483648.0f;
        (c == 0 ? out.L : out.R)[(size_t)(at + i)] = v;
      }
    at += got;
    if (cut) {
      frames = at;
      out.L.resize((size_t)frames);
      if (h.channels == 2) out.R.resize((size_t)frames);
      break;
    }
  }
  if (h.channels == 1) out.R = out.L;
  return out;
}

bool wavInfo(const std::string& path, WavInfo* info, std::string* error) {
  try {
    std::ifstream in(utf8Path(path), std::ios::binary);
    if (!in) {
      if (error) *error = "Cannot open " + path + ".";
      return false;
    }
    const WavHeader h = parseWavHeader(in, path);
    if (info) { info->frames = h.frames; info->sampleRate = h.rate; info->channels = h.channels; }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

AudioData resample(const AudioData& in, double targetRate) {
  if (in.sampleRate == targetRate) return in;
  const double ratio = targetRate / in.sampleRate;
  const double cutoff = std::min(1.0, ratio);  // below the lower Nyquist
  constexpr int kHalf = 32;                    // 64 taps around each output sample
  const double half = kHalf / cutoff;
  AudioData out;
  out.sampleRate = targetRate;
  out.channels = in.channels;
  // frames * target / source, rounded up, without the rounding error of the ratio: 44100 frames at 44.1 kHz are
  // 48000 at 48 kHz, not 48001 (deep hunt P05).
  const double exact = (double)in.frames() * targetRate / in.sampleRate;
  const int64_t frames = (int64_t)std::ceil(exact - exact * 1e-12);
  const int chans = in.channels;
  std::vector<float>* dst[2] = {&out.L, &out.R};
  const std::vector<float>* src[2] = {&in.L, &in.R};
  for (int c = 0; c < chans; ++c) dst[c]->resize((size_t)frames);
  constexpr double pi = 3.141592653589793;
  // The kernel (sinc, Blackman window) sampled kRes times per unit and read with linear interpolation:
  // no trig per tap, so a long file converts in about a second instead of a minute.
  constexpr int kRes = 1024;
  std::vector<float> kernel((size_t)(2 * kHalf * kRes + 2));
  for (size_t i = 0; i < kernel.size(); ++i) {
    const double x = (double)i / kRes - kHalf;
    const double sinc = x == 0 ? 1.0 : std::sin(pi * x) / (pi * x);
    const double w = std::abs(x) >= kHalf ? 0.0 : 0.42 + 0.5 * std::cos(pi * x / kHalf) + 0.08 * std::cos(2 * pi * x / kHalf);  // Blackman
    kernel[i] = (float)(sinc * w * cutoff);
  }
  for (int64_t o = 0; o < frames; ++o) {
    const double center = o / ratio;
    const int64_t first = (int64_t)std::ceil(center - half), last = (int64_t)std::floor(center + half);
    double acc[2] = {0, 0};
    for (int64_t k = std::max<int64_t>(0, first); k <= std::min<int64_t>(in.frames() - 1, last); ++k) {
      const double pos = ((k - center) * cutoff + kHalf) * kRes;
      const size_t i = (size_t)pos;
      if (i + 1 >= kernel.size()) continue;
      const double f = pos - (double)i;
      const double h = kernel[i] + (kernel[i + 1] - kernel[i]) * f;
      for (int c = 0; c < chans; ++c) acc[c] += (*src[c])[(size_t)k] * h;
    }
    for (int c = 0; c < chans; ++c) (*dst[c])[(size_t)o] = (float)acc[c];
  }
  if (chans == 1) out.R = out.L;
  return out;
}

} // namespace core
