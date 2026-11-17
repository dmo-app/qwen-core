// VOICE STUTTER's clip processors (an audio clip's pitchSemitones and freezeSeconds), as one stateless function of the
// position in the clip: the graph plays clips with it, and "Save as sample" writes the same samples (audit 2026-11-07:
// the sample was the unprocessed file). No allocation, no state: a seek or an export reads the same samples.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core {

// A vocal freeze drifts through its held piece at a quarter of the speed it was recorded (the plan's "slow source scan").
constexpr double kFreezeScan = 0.25;

struct ClipGrains {
  double ratio = 1;        // 2^(semitones/12)
  int64_t grain = 0;       // grain length in samples (0 = plain playback)
  int64_t freezeSpan = 0;  // > 0: grains start inside [offset, offset + freezeSpan), drifting at kFreezeScan
};

// The grains for a clip that starts `offset` frames into a file of `fileFrames` frames.
inline ClipGrains clipGrains(double pitchSemitones, double freezeSeconds, double sampleRate, int64_t offset, int64_t fileFrames) {
  ClipGrains g;
  if (freezeSeconds > 0) {
    g.freezeSpan = std::min<int64_t>((int64_t)std::llround(freezeSeconds * sampleRate), fileFrames - offset);
    g.grain = (int64_t)std::llround(0.040 * sampleRate);
  }
  if (pitchSemitones != 0) {
    g.ratio = std::pow(2.0, std::clamp(pitchSemitones, -24.0, 24.0) / 12.0);
    if (!g.grain) g.grain = (int64_t)std::llround(0.030 * sampleRate);
  }
  // A frozen piece shorter than one grain's read cannot be held by a grain that long, so the grain
  // shrinks to fit it: freezeSeconds says how much of the file is held, and that is what is played
  // (audit 2026-11-17 item 11: with a 10 ms freeze and 40 ms grains the scan left the piece).
  if (g.freezeSpan > 0 && g.grain > 0 && g.ratio > 0) {
    const int64_t longest = (int64_t)((double)(g.freezeSpan - 1) / g.ratio);
    if (longest < g.grain) g.grain = std::max<int64_t>(1, longest);
  }
  return g;
}

// Frame inClip of the clip (two Hann grains half a grain apart: they sum to 1), from the file's channels.
inline void clipGrainFrame(const ClipGrains& c, int64_t offset, const float* srcL, const float* srcR, int64_t frames, int64_t inClip,
                           double& l, double& r) {
  const double G = (double)c.grain;
  // How far into the file this clip may be read: to the end of the file, or, with a freeze, to the end
  // of the held piece (including the sample the interpolation reaches for).
  const int64_t limit = c.freezeSpan > 0 ? std::min<int64_t>(frames - 1, offset + c.freezeSpan - 1) : frames - 1;
  l = 0; r = 0;
  for (int j = 0; j < 2; ++j) {
    const double shifted = (double)inClip + j * G * 0.5;
    const double phase = std::fmod(shifted, G);
    const double grainStart = (double)inClip - phase;  // where this grain began, in clip samples
    double at;
    if (c.freezeSpan > 0) {
      const double span = (double)c.freezeSpan;
      at = std::fmod(std::max(0.0, grainStart) * kFreezeScan, std::max(1.0, span - G * c.ratio));
    } else {
      at = std::max(0.0, grainStart);
    }
    double src = (double)offset + at + phase * c.ratio;
    src = std::clamp(src, 0.0, (double)limit);
    const int64_t i0 = (int64_t)src;
    const int64_t i1 = std::min<int64_t>(i0 + 1, limit);
    const double f = src - (double)i0;
    const double w = std::sin(3.14159265358979323846 * phase / G);
    const double win = w * w;
    l += win * (srcL[i0] + (srcL[i1] - srcL[i0]) * f);
    r += win * (srcR[i0] + (srcR[i1] - srcR[i0]) * f);
  }
}

}  // namespace core
