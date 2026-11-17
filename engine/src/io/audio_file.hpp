// Audio files for audio clips (M3): WAV reading and sample-rate conversion. Control thread only.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core {

// Planar float audio. A mono source has R == L (same data, so stereo code needs no special case).
struct AudioData {
  double sampleRate = 48000;
  int channels = 2;            // of the source file: 1 or 2
  std::vector<float> L, R;
  int64_t frames() const { return (int64_t)L.size(); }
  size_t bytes() const { return (L.size() + R.size()) * sizeof(float); }
};

// WAV: PCM 16/24/32-bit or float 32/64-bit, mono or stereo (WAVE_FORMAT_EXTENSIBLE too). Path in
// UTF-8. Throws std::runtime_error with a readable message. maxFrames guards memory.
AudioData readWav(const std::string& path, int64_t maxFrames);

// Only what the header says, with no audio read: what a caller needs to know before it decides how
// much memory a file would take (the frames the file holds, its own rate and channel count). False with
// *error set when the file cannot be opened or is not a readable WAV (the same message readWav gives).
struct WavInfo {
  int64_t frames = 0;
  double sampleRate = 0;
  int channels = 0;
};
bool wavInfo(const std::string& path, WavInfo* info, std::string* error);

// Band-limited conversion (windowed sinc, 64 taps, cut-off lowered when converting down).
// Same rate: a copy.
AudioData resample(const AudioData& in, double targetRate);

} // namespace core
