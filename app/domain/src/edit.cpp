#include "edit.hpp"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <deque>
#include <cstdlib>
#include <regex>
#include <set>
#include <sstream>

namespace core::doc {

namespace {
// W-Core knobs every layer takes from Wave 1 (engine WaveStack::sync).
constexpr const char* kWaveSharedKnobs[] = {"mode", "glide", "driftHz", "bend", "modWheel", "polyVoices", "legato", "glideAlways"};

Track& find(Document& doc, const std::string& id) {
  for (auto& t : doc.tracks) if (t.id == id) return t;
  throw DocumentError("tracks", "no track '" + id + "'");
}

Note note(int index, int64_t tick, int64_t length, int pitch, double velocity) {
  Note n;
  n.id = "n" + std::to_string(index);
  n.tick = tick; n.length = length; n.pitch = pitch; n.velocity = velocity;
  return n;
}

// Lower-case letters, digits and '-' from a name or an instrument, for a readable id.
std::string slug(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c)) out += (char)std::tolower(c);
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) out = "track";
  return out.substr(0, 32);
}

} // namespace

const std::vector<InstrumentChoice>& builtinInstruments() {
  static const std::vector<InstrumentChoice> list = {{"kick", "Kick"}, {"bass", "Bass"}, {"wave", "W-Core"}, {"hats", "Hats"}, {"snare", "Snare + clap"}, {"drums", "Drums (kick + hats)"},
                                                              {"sampler", "Sampler"}, {"samplepath", "Core Glitch"}};
  return list;
}

Document newEmptyDocument() {
  Document d;
  d.name = "";
  d.bpm = 145;
  d.lengthTicks = 16 * 4 * kPpq;
  d.loop = {false, 0, 4 * 4 * kPpq, json::object()};
  // Headroom: an empty song has no limiter on the master, and kick + bass + hats at their default levels (P-Core
  // writes them so) peaked at 0.95..1.44 with the master at 0 dB (audit 2026-11-08, part 17). At -6 dB they stay
  // under 0 dBFS, as in the basic template.
  d.masterGainDb = -6;
  return d;
}

Document newTemplateDocument() {
  Document d = newEmptyDocument();
  const std::string kick = addTrack(d, "kick", "Kick"), bass = addTrack(d, "bass", "Bass"), hats = addTrack(d, "hats", "Hats");
  Track& k = find(d, kick);
  k.color.clear();  // the role colour (TrackColour), the same as in every song
  for (int beat = 0; beat < 4; ++beat) k.patterns[0].notes.push_back(note(beat + 1, beat * kPpq, kPpq / 4, 36, 0.95));
  Track& b = find(d, bass);
  b.color.clear();
  b.mix.gainDb = -3;
  int i = 0;
  for (int beat = 0; beat < 4; ++beat)
    for (int sixteenth = 1; sixteenth < 4; ++sixteenth)
      b.patterns[0].notes.push_back(note(++i, beat * kPpq + sixteenth * kPpq / 4, kPpq / 4 - 40, 33, 0.8));
  Track& h = find(d, hats);
  h.color.clear();
  h.mix.gainDb = -6;
  for (int beat = 0; beat < 4; ++beat) h.patterns[0].notes.push_back(note(beat + 1, beat * kPpq + kPpq / 2, 120, 46, 0.7));
  return d;
}

// ---- built-in instruments' parameters

const std::vector<InstrumentInfo>& instrumentCatalogue() {
  static const std::vector<InstrumentInfo> list = [] {
    // Core's one kick (2026-11-07): its defaults are C2's sound, fitted to Psiger's KICK (2026-11-08, engine instruments.cpp).
    const std::vector<InstrumentParamInfo> kick = {
        {"pitchStart", "Start pitch", 1938, 60, 4000, "Hz", 0, true, true}, {"pitchEnd", "End pitch", 42.13, 25, 120, "Hz", 0, true, true},
        {"pitchDecay", "Pitch decay", 3.409, 0.2, 200, "ms", 0, true, true}, {"ampDecay", "Decay", 251.9, 5, 1500, "ms", 0, true, true},
        {"click", "Click", 0.1615, 0, 1, "", 0, false, true},                  {"drive", "Drive", 0.09822, 0, 1, "", 0, false, true},
        {"level", "Level", 0.85, 0, 1.5, "", 0, false, true},               {"hold", "Hold", 68.98, 0, 500, "ms", 0, false, true},
        {"knee", "Knee", 144.7, 40, 500, "Hz", 0, true, true},                 {"kneeDecay", "Knee decay", 56.01, 5, 400, "ms", 0, true, true},
        {"length", "Length", 206.6, 60, 1500, "ms", 0, true, true},            {"clickTone", "Click tone", 6364, 300, 12000, "Hz", 0, true, true}};
    const std::vector<InstrumentParamInfo> hats = {
        {"tone", "Tone", 8000, 2000, 16000, "Hz", 0, true, true},          {"closedDecay", "Closed decay", 35, 5, 400, "ms", 0, true, true},
        {"openDecay", "Open decay", 220, 20, 2000, "ms", 0, true, true},   {"level", "Level", 0.45, 0, 1.5, "", 0, false, true}};
    // "drums" plays kick (36) and hats (42/46) with the same parameters, prefixed. The names must outlive the list.
    static std::vector<std::string> names, labels;
    names.reserve(kick.size() + hats.size());
    labels.reserve(kick.size() + hats.size());
    std::vector<InstrumentParamInfo> drums;
    const std::vector<InstrumentParamInfo> drumKick(kick.begin(), kick.begin() + 7);  // as the engine: no punch/hold knobs
    for (const auto* part : {&drumKick, &hats})
      for (const auto& p : *part) {
        const bool isKick = part == &drumKick;
        names.push_back(std::string(isKick ? "kick." : "hats.") + p.name);
        labels.push_back(std::string(isKick ? "Kick " : "Hats ") + p.label);
        InstrumentParamInfo x = p;
        x.name = names.back().c_str();
        x.label = labels.back().c_str();
        drums.push_back(x);
      }
    std::vector<InstrumentInfo> out{
        {"kick", "Kick", kick},
        // W-Core version 2 (2026-11-08): the engine's Wave::specList, same order. The movements (ENV 2/3, LFOs,
        // Velocity, Note, Macros) reach the knobs through the track's links (Instrument::synth).
        {"wave", "Synth",
         {{"wave", "Shape", 3, 0, 4, "", 0, false, true},              {"uniDetune", "Detune", 0.3, 0, 1, "", 0, false, true},
          {"subOct", "Sub octave", -1, -3, 1, "oct", 1, false, false},         {"octave", "Octave", 0, -2, 2, "oct", 1, false, false},
          {"tone", "Tone", 1800, 40, 12000, "Hz", 0, true, true},       {"bite", "Bite", 0.3, 0, 0.95, "", 0, false, true},
          {"glide", "Glide", 0, 0, 1000, "ms", 0, false, true},         {"mode", "Pad", 0, 0, 1, "", 1, false, false},
          {"level", "Volume", 0.7, 0, 1.5, "", 0, false, true},
          {"noise", "Noise", 0, 0, 1, "", 0, false, true},              {"noiseColor", "Noise colour", 0.6, 0, 1, "", 0, false, true},
          {"fm", "FM", 0, 0, 10, "", 0, false, true},                   {"fmRatio", "FM ratio", 2, 0.25, 16, "", 0, true, true},
          {"tune", "Fine tune", 0, -100, 100, "ct", 0, false, true},
          {"grain", "Grain", 0, 0, 1, "", 0, false, true},              {"grainSize", "Grain size", 80, 5, 1000, "ms", 0, true, true},
          {"grainDensity", "Density", 20, 1, 200, "/s", 0, true, true}, {"grainPos", "Position", 0, 0, 1, "", 0, false, true},
          {"grainScan", "Scan", 0.2, 0, 2, "", 0, false, true},         {"grainScatter", "Scatter", 0.3, 0, 1, "", 0, false, true},
          {"osc2", "Osc 2", 0, 0, 1, "", 0, false, true},                {"osc2Wave", "Osc 2 shape", 3, 0, 4, "", 0, false, true},
          {"osc2Semi", "Osc 2 interval", 0, -24, 24, "st", 1, false, false}, {"osc2Fine", "Osc 2 detune", 7, -100, 100, "ct", 0, false, true},
          {"filterMode", "Filter type", 0, 0, 19, "", 1, false, false},   {"drive", "Drive", 0, 0, 1, "", 0, false, true},
          {"osc1Table", "Osc 1 table", 0, 0, 8, "", 1, false, false},   {"osc1Pos", "Osc 1 position", 0, 0, 1, "", 0, false, true},
          {"osc2Table", "Osc 2 table", 0, 0, 8, "", 1, false, false},   {"osc2Pos", "Osc 2 position", 0, 0, 1, "", 0, false, true},
          {"osc1Semi", "Osc 1 interval", 0, -24, 24, "st", 1, false, false},
          {"crush", "Crush", 0, 0, 1, "", 0, false, true},
          {"filterDrive", "Filter drive", 0, 0, 1, "", 0, false, true},  {"filterShape", "Filter shape", 0.5, 0, 1, "", 0, false, true},
          {"env1Attack", "Attack", 5, 0, 8000, "ms", 0, true, true},     {"env1Hold", "Hold", 0, 0, 8000, "ms", 0, true, true},
          {"env1Decay", "Decay", 1000, 1, 20000, "ms", 0, true, true},   {"env1Sustain", "Sustain", 1, 0, 1, "", 0, false, true},
          {"env1Release", "Release", 300, 1, 30000, "ms", 0, true, true},
          {"env2Attack", "ENV 2 attack", 0, 0, 8000, "ms", 0, true, true}, {"env2Hold", "ENV 2 hold", 0, 0, 8000, "ms", 0, true, true},
          {"env2Decay", "ENV 2 decay", 1840, 1, 20000, "ms", 0, true, true}, {"env2Sustain", "ENV 2 sustain", 0, 0, 1, "", 0, false, true},
          {"env2Release", "ENV 2 release", 2763.102111592855, 1, 30000, "ms", 0, true, true},
          {"env3Attack", "ENV 3 attack", 0, 0, 8000, "ms", 0, true, true}, {"env3Hold", "ENV 3 hold", 0, 0, 8000, "ms", 0, true, true},
          {"env3Decay", "ENV 3 decay", 368, 1, 20000, "ms", 0, true, true}, {"env3Sustain", "ENV 3 sustain", 0, 0, 1, "", 0, false, true},
          {"env3Release", "ENV 3 release", 552.620422318571, 1, 30000, "ms", 0, true, true},
          {"lfo1Rate", "LFO 1 rate", 1, 0.03125, 64, "beats", 0, true, false}, {"lfo1Hz", "LFO 1 Hz", 1, 0.005, 40, "Hz", 0, true, true},
          {"lfo1Phase", "LFO 1 phase", 0, 0, 1, "", 0, false, true},    {"lfo1Smooth", "LFO 1 smooth", 1.5, 0, 200, "ms", 0, false, true},
          {"lfo2Rate", "LFO 2 rate", 4, 0.03125, 64, "beats", 0, true, false}, {"lfo2Hz", "LFO 2 Hz", 1, 0.005, 40, "Hz", 0, true, true},
          {"lfo2Phase", "LFO 2 phase", 0, 0, 1, "", 0, false, true},    {"lfo2Smooth", "LFO 2 smooth", 1.5, 0, 200, "ms", 0, false, true},
          {"lfo3Rate", "LFO 3 rate", 16, 0.03125, 64, "beats", 0, true, false}, {"lfo3Hz", "LFO 3 Hz", 0.25, 0.005, 40, "Hz", 0, true, true},
          {"lfo3Phase", "LFO 3 phase", 0, 0, 1, "", 0, false, true},    {"lfo3Smooth", "LFO 3 smooth", 0, 0, 200, "ms", 0, false, true},
          {"lfo4Rate", "LFO 4 rate", 2, 0.03125, 64, "beats", 0, true, false}, {"lfo4Hz", "LFO 4 Hz", 1, 0.005, 40, "Hz", 0, true, true},
          {"lfo4Phase", "LFO 4 phase", 0, 0, 1, "", 0, false, true},    {"lfo4Smooth", "LFO 4 smooth", 1.5, 0, 200, "ms", 0, false, true},
          {"macro1", "Macro 1", 0, 0, 1, "", 0, false, true},           {"macro2", "Macro 2", 0, 0, 1, "", 0, false, true},
          {"macro3", "Macro 3", 0, 0, 1, "", 0, false, true},           {"macro4", "Macro 4", 0, 0, 1, "", 0, false, true},
          {"unison", "Unison", 7, 1, 16, "", 1, false, false},          {"uniBlend", "Blend", 0.3, 0, 1, "", 0, false, true},
          {"uniWidth", "Width", 1, 0, 1, "", 0, false, true},           {"subLevel", "Sub", 0, 0, 1, "", 0, false, true},
          {"subShape", "Sub shape", 0, 0, 5, "", 1, false, false},
          {"osc1Warp", "Osc 1 warp", 0, 0, 11, "", 1, false, false},    {"osc1WarpAmt", "Osc 1 warp amount", 0, 0, 1, "", 0, false, true},
          {"osc2Warp", "Osc 2 warp", 0, 0, 11, "", 1, false, false},    {"osc2WarpAmt", "Osc 2 warp amount", 0, 0, 1, "", 0, false, true},
          {"bend", "Pitch bend", 0, -1, 1, "", 0, false, true},         {"bendRange", "Bend range", 2, 0, 48, "st", 1, false, false},
          {"modWheel", "Mod wheel", 0, 0, 1, "", 0, false, true},       {"polyVoices", "Voices", 8, 1, 8, "", 1, false, false},
          {"legato", "Legato", 0, 0, 1, "", 1, false, false},           {"glideAlways", "Glide always", 0, 0, 1, "", 1, false, false},
          {"filter2Mode", "Filter 2 type", 0, 0, 19, "", 1, false, false}, {"tone2", "Filter 2 tone", 3000, 40, 12000, "Hz", 0, true, true},
          {"bite2", "Filter 2 bite", 0.3, 0, 0.95, "", 0, false, true}, {"filter2Drive", "Filter 2 drive", 0, 0, 1, "", 0, false, true},
          {"filter2Shape", "Filter 2 shape", 0.5, 0, 1, "", 0, false, true}, {"filterSerial", "Serial", 0, 0, 1, "", 1, false, false},
          {"osc1Route", "Osc 1 to", 0, 0, 2, "", 1, false, false},       {"osc2Route", "Osc 2 to", 0, 0, 2, "", 1, false, false},
          {"subRoute", "Sub to", 0, 0, 2, "", 1, false, false},
          {"noiseRoute", "Noise to", 0, 0, 2, "", 1, false, false},
          {"osc1Level", "Osc 1 level", 1, 0, 1, "", 0, false, true},    {"subPan", "Sub pan", 0, -1, 1, "", 0, false, true},
          {"filterMix", "Filter mix", 1, 0, 1, "", 0, false, true},     {"filter2Mix", "Filter 2 mix", 1, 0, 1, "", 0, false, true},
          {"driveColour", "Drive colour", 0, 0, 1, "", 0, false, true}, {"driveMix", "Drive mix", 1, 0, 1, "", 0, false, true},
          {"lfo1Depth", "LFO 1 amount", 1, 0, 1, "", 0, false, true},  {"lfo2Depth", "LFO 2 amount", 1, 0, 1, "", 0, false, true},
          {"lfo3Depth", "LFO 3 amount", 1, 0, 1, "", 0, false, true},  {"lfo4Depth", "LFO 4 amount", 1, 0, 1, "", 0, false, true},
          {"outPan", "Output pan", 0, -1, 1, "", 0, false, true},              {"grainPitch", "Grain pitch", 0, -24, 24, "st", 0, false, true}}},
        {"hats", "Hats", hats},
        {"snare", "Snare + clap",
         {{"tone", "Tone", 190, 120, 400, "Hz", 0, true, true},         {"snap", "Snap", 0.6, 0, 1, "", 0, false, true},
          {"decay", "Decay", 220, 40, 2000, "ms", 0, true, true},       {"clapTone", "Clap tone", 1100, 500, 4000, "Hz", 0, true, true},
          {"level", "Level", 0.6, 0, 1.5, "", 0, false, true}}},
        {"drums", "Drums", drums},
        {"sampler", "Sampler",
         {{"tune", "Tune", 0, -24, 24, "st", 0, false, true},           {"start", "Start", 0, 0, 60000, "ms", 0, false, true},
          {"attack", "Attack", 0, 0, 200, "ms", 0, false, true},        {"decay", "Decay", 10000, 10, 10000, "ms", 0, true, true},
          {"release", "Release", 30, 5, 2000, "ms", 0, true, true},     {"pitched", "Pitched", 0, 0, 1, "", 1, false, false},
          {"level", "Level", 0, -24, 24, "dB", 0, false, true},         {"retrigger", "Retrigger", 1, 0, 1, "", 1, false, false},
          {"sustain", "Sustain", 0, 0, 1, "", 0, false, true},           {"reverse", "Reverse", 0, 0, 1, "", 1, false, false},
          {"filterType", "Filter", 0, 0, 3, "", 1, false, false},        {"cutoff", "Cutoff", 20000, 20, 20000, "Hz", 0, true, true},
          {"resonance", "Resonance", 0.1, 0, 0.95, "", 0, false, true},  {"filterEnv", "Filter env", 0, -5, 5, "oct", 0, false, true},
          {"filterDecay", "Filter decay", 200, 5, 4000, "ms", 0, true, true}, {"velocity", "Velocity", 1, 0, 1, "", 0, false, true}}},
        // Core Glitch · Sample Path: its Global row (the map is the track's "synth", glitch.hpp).
        {"samplepath", "Core Glitch",
         {{"amp", "Amp", -8, -40, 6, "dB", 0, false, true},             {"pitch", "Pitch", 0, -12, 12, "st", 0, false, true},
          {"size", "Size", 0.45, 0, 1, "", 0, false, true},             {"damp", "Damp", 0.58, 0, 1, "", 0, false, true},
          {"mix", "Mix", 0.12, 0, 1, "", 0, false, true},               {"limiter", "Limiter", 1, 0, 1, "", 1, false, false}}}};
    // W-Core layers 2..4 (Diogo 2026-11-07): an on switch, then layer 1's parameters again under "lN_" (as the engine).
    static std::vector<std::string> layerNames, layerLabels;
    for (auto& info : out) {
      if (std::string(info.name) != "wave") continue;
      const std::vector<InstrumentParamInfo> one = info.params;
      layerNames.reserve(3 * (one.size() + 1) + 40);
      layerLabels.reserve(3 * (one.size() + 1) + 40);
      for (int n = 2; n <= 4; ++n) {
        const std::string prefix = "l" + std::to_string(n) + "_", label = "Wave " + std::to_string(n) + " ";
        layerNames.push_back(prefix + "on");
        layerLabels.push_back(label + "on");
        info.params.push_back({layerNames.back().c_str(), layerLabels.back().c_str(), 0, 0, 1, "", 0, false, false});  // 0 / 0.25 off / 1 on
        for (const auto& p : one) {
          layerNames.push_back(prefix + p.name);
          layerLabels.push_back(label + p.label);
          InstrumentParamInfo x = p;
          x.name = layerNames.back().c_str();
          x.label = layerLabels.back().c_str();
          // One keyboard, one tempo: Lead/Pad, Glide, the drift's rate and how notes are played (bend, mod wheel,
          // voices, legato, glide always) are Wave 1's for every layer (the engine's WaveStack::sync), so a layer's
          // own copy does nothing. It stays in the song, but is not offered for automation, where it was a knob that
          // moved nothing (audits 2026-11-08 part 17 and 57763c1).
          for (const char* shared : kWaveSharedKnobs)
            if (std::string(p.name) == shared) x.automatable = false;
          info.params.push_back(x);
        }
      }
      // The section lights, 9 per layer (engine WaveStack::kSectionIds): 1 = on, 0 = left out.
      static const char* sections[] = {"colour", "grain", "filter", "motion", "osc2", "tables", "shape", "lfo1", "lfo2"};
      static const char* names[] = {"Colour", "Grain", "Filter", "Motion", "Osc 2", "Tables", "Filter+", "LFO 1", "LFO 2"};
      for (int n = 1; n <= 4; ++n)
        for (int k = 0; k < 9; ++k) {
          layerNames.push_back((n == 1 ? std::string() : "l" + std::to_string(n) + "_") + "sec_" + sections[k]);
          layerLabels.push_back((n == 1 ? std::string() : "Wave " + std::to_string(n) + " ") + names[k] + " on");
          info.params.push_back({layerNames.back().c_str(), layerLabels.back().c_str(), 1, 0, 1, "", 1, false, false});
        }
    }
    return out;
  }();
  return list;
}

const InstrumentInfo* findInstrumentInfo(const std::string& name) {
  for (const auto& i : instrumentCatalogue()) if (name == i.name) return &i;
  return nullptr;
}

const InstrumentParamInfo* findInstrumentParam(const Track& track, const std::string& param) {
  if (track.instrument.kind != "builtin") return nullptr;
  const InstrumentInfo* info = findInstrumentInfo(track.instrument.name);
  if (!info) return nullptr;
  for (const auto& p : info->params) if (param == p.name) return &p;
  return nullptr;
}

double instrumentParamValue(const Track& track, const InstrumentParamInfo& param) {
  auto it = track.instrument.params.find(param.name);
  return it == track.instrument.params.end() ? param.def : std::clamp(it->second, param.min, param.max);
}

void setInstrumentParam(Document& doc, const std::string& trackId, const std::string& param, double value) {
  Track& t = find(doc, trackId);
  if (t.instrument.kind != "builtin") throw DocumentError("instrument", "only a built-in instrument has these parameters");
  const InstrumentInfo* info = findInstrumentInfo(t.instrument.name);
  if (!info) throw DocumentError("instrument", "unknown instrument '" + t.instrument.name + "'");
  const InstrumentParamInfo* p = findInstrumentParam(t, param);
  if (!p) {
    std::string list;
    for (const auto& x : info->params) list += (list.empty() ? "" : ", ") + std::string(x.name);
    throw DocumentError("instrument", std::string(info->label) + " has no parameter '" + param + "' (it has " + list + ")");
  }
  if (!std::isfinite(value) || value < p->min - 1e-9 || value > p->max + 1e-9) {
    std::ostringstream m;
    m << param << ": must be between " << p->min << " and " << p->max << (*p->unit ? std::string(" ") + p->unit : std::string()) << " (got " << value << ")";
    throw DocumentError("instrument", m.str());
  }
  if (p->step >= 1 && std::abs(value - std::round(value)) > 1e-9) throw DocumentError("instrument", param + ": must be a whole number");
  t.instrument.params[param] = p->step >= 1 ? std::round(value) : std::clamp(value, p->min, p->max);
}

int resetInstrumentParams(Document& doc, const std::string& trackId) {
  Track& t = find(doc, trackId);
  if (t.instrument.kind != "builtin") throw DocumentError("instrument", "only a built-in instrument has these parameters");
  const InstrumentInfo* info = findInstrumentInfo(t.instrument.name);
  if (!info) throw DocumentError("instrument", "unknown instrument '" + t.instrument.name + "'");
  int changed = 0;
  for (const auto& p : info->params) {
    auto it = t.instrument.params.find(p.name);
    if (it == t.instrument.params.end()) continue;
    if (std::abs(it->second - p.def) > 1e-12) ++changed;
    t.instrument.params.erase(it);
  }
  return changed;
}

bool instrumentAtDefaults(const Track& track) {
  if (track.instrument.kind != "builtin") return true;
  const InstrumentInfo* info = findInstrumentInfo(track.instrument.name);
  if (!info) return true;
  for (const auto& p : info->params)
    if (std::abs(instrumentParamValue(track, p) - p.def) > 1e-12) return false;
  return true;
}

std::string uniqueTrackId(const Document& doc, const std::string& base) {
  const std::string root = slug(base);
  std::string id = root;
  // "master" is the master's name in plugin keys and routing (audit B02): a track never takes it.
  for (int n = 2; doc.findTrack(id) || id == "master"; ++n) id = root + "-" + std::to_string(n);
  return id;
}

// A track for notes with no instrument yet (Diogo 2026-11-16: the sound comes from plugins): its notes play once a
// plugin is chosen for it. The role names it ("kick", "bass"...) when no name is given.
std::string addTrack(Document& doc, const std::string& role, const std::string& name) {
  if (doc.tracks.size() >= 128) throw DocumentError("tracks", "has the maximum of 128 tracks");
  Track t;
  t.id = uniqueTrackId(doc, role.empty() ? std::string("track") : role);
  std::string label = name;
  if (label.empty())
    for (const auto& c : builtinInstruments()) if (role == c.name) label = c.label;
  t.name = cutUtf8(label.empty() ? (role.empty() ? std::string("Track") : role) : label, 80);  // never half a character
  t.instrument.kind = "none";
  // One 1-bar pattern played over the whole song: edit it and hear it at once.
  Pattern p;
  p.id = "p1";
  p.name = "1";
  p.lengthTicks = 4 * kPpq;
  t.patterns.push_back(p);
  t.clips.push_back({"c1", "p1", 0, doc.lengthTicks, 0, json::object()});
  doc.tracks.push_back(std::move(t));
  return doc.tracks.back().id;
}

std::string duplicateTrack(Document& doc, const std::string& trackId) {
  if (doc.tracks.size() >= 128) throw DocumentError("tracks", "has the maximum of 128 tracks");
  const auto it = std::find_if(doc.tracks.begin(), doc.tracks.end(), [&](const Track& t) { return t.id == trackId; });
  if (it == doc.tracks.end()) throw DocumentError("tracks", "no track '" + trackId + "'");
  Track copy = *it;  // notes keep their ids: they are unique within their own pattern
  copy.id = uniqueTrackId(doc, trackId);
  copy.name = nameWithSuffix(it->name, " 2", kMaxTrackNameBytes);
  copy.mix.solo = false;
  const auto at = it - doc.tracks.begin() + 1;  // right after the original
  doc.tracks.insert(doc.tracks.begin() + at, std::move(copy));
  return doc.tracks[(size_t)at].id;
}

std::string shownAfterRemoval(const Document& now, const std::vector<std::string>& orderBefore, const std::string& removed,
                              std::set<std::string>& alsoSelected) {
  for (auto it = alsoSelected.begin(); it != alsoSelected.end();) it = now.findTrack(*it) ? std::next(it) : alsoSelected.erase(it);
  for (const auto& t : now.tracks)
    if (alsoSelected.count(t.id)) { alsoSelected.erase(t.id); return t.id; }
  const auto at = std::find(orderBefore.begin(), orderBefore.end(), removed);
  if (at != orderBefore.end()) {
    for (auto it = at + 1; it != orderBefore.end(); ++it) if (now.findTrack(*it)) return *it;
    for (auto it = at; it != orderBefore.begin();) if (now.findTrack(*--it)) return *it;
  }
  return now.tracks.empty() ? std::string() : now.tracks.front().id;
}

void renameTrack(Document& doc, const std::string& trackId, const std::string& name) {
  std::string clean = name;
  clean.erase(0, clean.find_first_not_of(" \t"));
  clean.erase(clean.find_last_not_of(" \t") + 1);
  if (clean.empty()) throw DocumentError("name", "cannot be empty");
  if (clean.size() > 80) throw DocumentError("name", "is longer than 80 bytes (an accented letter counts as 2)");
  find(doc, trackId).name = clean;  // the id stays: it is what links meters, history and the engine
}

Track& trackById(Document& doc, const std::string& trackId) { return find(doc, trackId); }

std::string newNoteId(const Pattern& pattern) {
  long highest = 0;
  for (const auto& n : pattern.notes)
    if (n.id.size() > 1 && n.id[0] == 'n') highest = std::max(highest, idNumber(n.id.c_str() + 1));
  return "n" + std::to_string(highest + 1);
}

bool toggleStep(Pattern& pattern, int64_t tick, int pitch, int64_t length, double velocity) {
  const auto before = pattern.notes.size();
  pattern.notes.erase(std::remove_if(pattern.notes.begin(), pattern.notes.end(),
                                     [&](const Note& n) { return n.tick == tick && n.pitch == pitch; }),
                      pattern.notes.end());
  if (pattern.notes.size() != before) return false;
  if (tick < 0 || tick >= pattern.lengthTicks) throw DocumentError("tick", "outside the pattern");
  if (pattern.notes.size() >= 4096) throw DocumentError("notes", "the pattern has the maximum of 4096 notes");
  Note n;
  n.id = newNoteId(pattern);
  n.tick = tick;
  n.length = std::max<int64_t>(1, std::min(length, pattern.lengthTicks - tick));
  n.pitch = pitch;
  n.velocity = velocity;
  pattern.notes.push_back(n);
  return true;
}

std::vector<std::string> duplicateNotes(Pattern& pattern, const std::vector<std::string>& noteIds, int64_t snapTicks) {
  int64_t first = INT64_MAX, last = 0;
  std::vector<Note> chosen;
  for (const auto& n : pattern.notes)
    if (std::find(noteIds.begin(), noteIds.end(), n.id) != noteIds.end()) {
      chosen.push_back(n);
      first = std::min(first, n.tick);
      last = std::max(last, n.tick + n.length);
    }
  std::vector<std::string> copies;
  if (chosen.empty()) return copies;
  const int64_t snap = std::max<int64_t>(1, snapTicks);
  const int64_t span = (last - first + snap - 1) / snap * snap;
  for (Note n : chosen) {
    n.tick += span;
    if (n.tick >= pattern.lengthTicks) continue;  // past the end of the pattern: not copied
    if (pattern.notes.size() >= 4096) break;
    n.length = std::min(n.length, pattern.lengthTicks - n.tick);
    n.id = newNoteId(pattern);
    pattern.notes.push_back(n);
    copies.push_back(n.id);
  }
  return copies;
}

void setPatternLength(Pattern& pattern, int64_t lengthTicks) {
  if (lengthTicks < kPpq / 8 || lengthTicks > 64LL * 4 * kPpq) throw DocumentError("lengthTicks", "invalid pattern length");
  pattern.lengthTicks = lengthTicks;
  pattern.notes.erase(std::remove_if(pattern.notes.begin(), pattern.notes.end(), [&](const Note& n) { return n.tick >= lengthTicks; }),
                      pattern.notes.end());
  for (auto& n : pattern.notes) n.length = std::min(n.length, lengthTicks - n.tick);
}

void extendPatternRepeating(Pattern& pattern, int64_t lengthTicks) {
  const int64_t old = pattern.lengthTicks;
  const std::vector<Note> source = pattern.notes;
  setPatternLength(pattern, lengthTicks);
  if (lengthTicks <= old || old <= 0 || source.empty()) return;
  for (int64_t offset = old; offset < lengthTicks; offset += old)
    for (Note n : source) {
      n.tick += offset;
      if (n.tick >= lengthTicks) continue;
      if (pattern.notes.size() >= 4096) return;
      n.length = std::min(n.length, lengthTicks - n.tick);
      n.id = newNoteId(pattern);
      pattern.notes.push_back(n);
    }
}

Pattern& patternById(Track& track, const std::string& patternId) {
  if (Pattern* p = track.findPattern(patternId)) return *p;
  throw DocumentError("patterns", "no pattern '" + patternId + "'");
}

namespace {
// Next free "<prefix><n>" among existing ids.
template <typename List>
std::string nextId(const List& items, const std::string& prefix) {
  long highest = 0;
  for (const auto& x : items)
    if (x.id.rfind(prefix, 0) == 0) highest = std::max(highest, idNumber(x.id.c_str() + prefix.size()));
  return prefix + std::to_string(highest + 1);
}
Clip& clipById(Track& t, const std::string& clipId) {
  for (auto& c : t.clips) if (c.id == clipId) return c;
  throw DocumentError("clips", "no clip '" + clipId + "'");
}
void sortClips(Track& t) {
  std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.startTick < b.startTick; });
}
}  // namespace

std::string addPattern(Track& track, int64_t lengthTicks) {
  if (track.patterns.size() >= 64) throw DocumentError("patterns", "the track has the maximum of 64 patterns");
  // The reader's range (1/32 .. 64 bars): a 0 length divided by zero in the projection (sweep).
  if (lengthTicks < kPpq / 8 || lengthTicks > 64LL * 4 * kPpq) throw DocumentError("patterns", "a pattern is 1/32 to 64 bars long");
  Pattern p;
  p.id = nextId(track.patterns, "p");
  p.name = std::to_string(track.patterns.size() + 1);
  p.lengthTicks = lengthTicks;
  track.patterns.push_back(p);
  return p.id;
}

void clearPattern(Track& track, const std::string& patternId) { patternById(track, patternId).notes.clear(); }

void removePattern(Track& track, const std::string& patternId) {
  patternById(track, patternId);
  if (track.patterns.size() <= 1) throw DocumentError("patterns", "a track keeps at least one pattern (clear it instead)");
  track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(), [&](const Clip& c) { return c.patternId == patternId; }), track.clips.end());
  track.patterns.erase(std::remove_if(track.patterns.begin(), track.patterns.end(), [&](const Pattern& p) { return p.id == patternId; }), track.patterns.end());
}

std::string addClip(Track& track, const std::string& patternId, int64_t startTick, int64_t lengthTicks) {
  if (!track.findPattern(patternId)) throw DocumentError("patternId", "no pattern '" + patternId + "'");
  if (track.clips.size() >= 256) throw DocumentError("clips", "the track has the maximum of 256 clips");
  if (startTick < 0 || lengthTicks < 1) throw DocumentError("clips", "invalid position");
  Clip c{nextId(track.clips, "c"), patternId, startTick, lengthTicks, 0, json::object()};
  track.clips.push_back(c);
  sortClips(track);
  return c.id;
}

std::string duplicateClip(Track& track, const std::string& clipId, int64_t startTick) {
  Clip copy = clipById(track, clipId);
  if (track.clips.size() >= 256) throw DocumentError("clips", "the track has the maximum of 256 clips");
  if (startTick < 0) throw DocumentError("clips", "invalid position");
  copy.id = nextId(track.clips, "c");
  copy.startTick = startTick;
  const std::string id = copy.id;
  track.clips.push_back(std::move(copy));
  sortClips(track);
  return id;
}

// A copied clip put back on its track (Ctrl+V, also after a cut: the source may be gone). Its pattern must still be on
// the track; the copy gets a new id.
std::string pasteClip(Track& track, Clip copy, int64_t startTick) {
  patternById(track, copy.patternId);  // throws when the pattern is gone
  if (track.clips.size() >= 256) throw DocumentError("clips", "the track has the maximum of 256 clips");
  copy.id = nextId(track.clips, "c");
  copy.startTick = std::max<int64_t>(0, startTick);
  const std::string id = copy.id;
  track.clips.push_back(std::move(copy));
  sortClips(track);
  return id;
}

void moveClip(Track& track, const std::string& clipId, int64_t startTick) {
  clipById(track, clipId).startTick = std::max<int64_t>(0, startTick);
  sortClips(track);
}

void resizeClip(Track& track, const std::string& clipId, int64_t lengthTicks) {
  clipById(track, clipId).lengthTicks = std::max<int64_t>(1, lengthTicks);
}

void removeClip(Track& track, const std::string& clipId) {
  clipById(track, clipId);
  track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(), [&](const Clip& c) { return c.id == clipId; }), track.clips.end());
}

std::string makeClipUnique(Track& track, const std::string& clipId) {
  Clip& clip = clipById(track, clipId);
  const Pattern& shared = patternById(track, clip.patternId);
  if (track.clipsUsing(shared.id) <= 1) return shared.id;  // already its own
  if (track.patterns.size() >= 64) throw DocumentError("patterns", "the track has the maximum of 64 patterns");
  Pattern copy = shared;  // note ids stay: they are unique within each pattern
  copy.id = nextId(track.patterns, "p");
  copy.name = nameWithSuffix(shared.name, "b", kMaxPatternNameBytes);
  track.patterns.push_back(copy);
  clipById(track, clipId).patternId = copy.id;  // the reference above may have moved
  return copy.id;
}

std::string splitClip(Track& track, const std::string& clipId, int64_t atTick) {
  const Clip c = clipById(track, clipId);
  if (atTick <= c.startTick || atTick >= c.startTick + c.lengthTicks) throw DocumentError("clips", "the cut is not inside the clip");
  const std::string second = duplicateClip(track, clipId, atTick);
  clipById(track, second).lengthTicks = c.startTick + c.lengthTicks - atTick;
  const int64_t len = std::max<int64_t>(1, patternById(track, c.patternId).lengthTicks);
  clipById(track, second).offsetTicks = (c.offsetTicks + atTick - c.startTick) % len;
  clipById(track, clipId).lengthTicks = atTick - c.startTick;
  return second;
}

void removeUnusedPatterns(Track& track) {
  if (track.patterns.size() <= 1) return;
  std::vector<Pattern> kept;
  for (auto& p : track.patterns) if (track.clipsUsing(p.id) > 0) kept.push_back(std::move(p));
  if (kept.empty()) kept.push_back(track.patterns.front());
  track.patterns = std::move(kept);
}

// ---- regions

RegionIsolation isolateRegion(Document& doc, const std::string& trackId, int64_t startTick, int64_t endTick,
                              const std::map<std::string, std::string>& ids) {
  Track& t = find(doc, trackId);
  if (!playsPatterns(t)) throw DocumentError("region", "track '" + trackId + "' has no patterns (audio track or bus)");
  if (startTick < 0 || endTick <= startTick) throw DocumentError("region", "the region must end after it starts");
  RegionIsolation out;
  // 1. Cut the clips that cross an edge: the second part continues the pattern where the first stops.
  for (int64_t at : {startTick, endTick}) {
    std::vector<Clip> added;
    for (auto& c : t.clips)
      if (c.startTick < at && c.startTick + c.lengthTicks > at) {
        Clip second = c;
        second.startTick = at;
        second.lengthTicks = c.startTick + c.lengthTicks - at;
        const Pattern* p = t.findPattern(c.patternId);
        const int64_t len = p ? std::max<int64_t>(1, p->lengthTicks) : 1;
        second.offsetTicks = (c.offsetTicks + (at - c.startTick)) % len;  // its other fields stay (sweep: they were wiped)
        c.lengthTicks = at - c.startTick;
        added.push_back(second);
      }
    if (t.clips.size() + added.size() > 256) throw DocumentError("clips", "the track would have more than 256 clips");
    for (auto& a : added) { a.id = nextId(t.clips, "c"); t.clips.push_back(a); ++out.splitClips; }
  }
  sortClips(t);
  // 2. Which patterns play inside, and which of them also play outside.
  auto inside = [&](const Clip& c) { return c.startTick >= startTick && c.startTick + c.lengthTicks <= endTick; };
  std::vector<std::string> shared;
  for (const auto& c : t.clips) {
    if (!inside(c)) continue;
    out.insideClips.push_back(c.id);
    if (std::find(shared.begin(), shared.end(), c.patternId) != shared.end()) continue;
    if (std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& o) { return !inside(o) && o.patternId == c.patternId; }))
      shared.push_back(c.patternId);
  }
  for (const auto& [from, to] : ids)
    if (std::find(shared.begin(), shared.end(), from) == shared.end())
      throw DocumentError("region", "pattern '" + from + "' does not play both inside and outside the region, so it gets no copy");
  // 3. One copy per shared pattern, used by every clip inside.
  if (t.patterns.size() + shared.size() > 64) throw DocumentError("patterns", "the track would have more than 64 patterns");
  static const std::regex idRule("^[A-Za-z0-9_-]{1,40}$");
  for (const auto& pid : shared) {
    Pattern copy = patternById(t, pid);  // note ids stay: they are unique within each pattern
    auto wanted = ids.find(pid);
    if (wanted != ids.end()) {
      if (!std::regex_match(wanted->second, idRule)) throw DocumentError("region", "'" + wanted->second + "' is not a valid pattern id");
      if (t.findPattern(wanted->second)) throw DocumentError("region", "track '" + trackId + "' already has a pattern '" + wanted->second + "'");
      copy.id = wanted->second;
    } else {
      copy.id = nextId(t.patterns, "p");
    }
    copy.name = copy.name + " region";
    if (copy.name.size() > 40) {  // never cut inside a character (a broken name made the song unsavable)
      size_t cut = 40;
      while (cut > 0 && ((unsigned char)copy.name[cut] & 0xC0) == 0x80) --cut;
      copy.name.resize(cut);
    }
    t.patterns.push_back(copy);
    for (auto& c : t.clips) if (inside(c) && c.patternId == pid) c.patternId = copy.id;
    out.patterns.emplace_back(pid, copy.id);
  }
  return out;
}

// ---- sections

namespace {
Section& sectionById(Document& doc, const std::string& id) {
  for (auto& s : doc.sections) if (s.id == id) return s;
  throw DocumentError("sections", "no section '" + id + "'");
}
void sortSections(Document& doc) {
  std::stable_sort(doc.sections.begin(), doc.sections.end(), [](const Section& a, const Section& b) { return a.startTick < b.startTick; });
}
// Splits every clip that crosses tick into two clips (same pattern, the second restarts the pattern).
void splitClipsAt(Document& doc, int64_t tick) {
  for (auto& t : doc.tracks)
    for (size_t i = 0; i < t.audioClips.size(); ++i) {
      const AudioClip& a = t.audioClips[i];
      const double cut = secondsBetween(doc, (double)a.startTick, (double)tick);
      if (cut >= kMinClipSeconds && a.durationSeconds - cut >= kMinClipSeconds) splitAudioClip(doc, t.id, a.id, tick);
    }
  for (auto& t : doc.tracks) {
    std::vector<Clip> added;
    for (auto& c : t.clips)
      if (c.startTick < tick && c.startTick + c.lengthTicks > tick) {
        Clip second = c;
        second.startTick = tick;
        second.lengthTicks = c.startTick + c.lengthTicks - tick;
        // The second part continues the pattern where the first one stops, so nothing sounds different.
        const Pattern* p = t.findPattern(c.patternId);
        const int64_t len = p ? std::max<int64_t>(1, p->lengthTicks) : 1;
        second.offsetTicks = (c.offsetTicks + (tick - c.startTick)) % len;
        c.lengthTicks = tick - c.startTick;
        added.push_back(second);
      }
    for (auto& a : added) { a.id = nextId(t.clips, "c"); t.clips.push_back(a); }
    sortClips(t);
  }
}
// Every automation list of the song: each track's and the master's.
std::vector<std::vector<Automation>*> automationLists(Document& doc) {
  std::vector<std::vector<Automation>*> lists;
  for (auto& t : doc.tracks) lists.push_back(&t.automation);
  lists.push_back(&doc.masterAutomation);
  return lists;
}

// Automation points are in song ticks: a point is added at tick (the value there, interpolated) so
// the curve keeps its shape when the music on either side moves.
void splitAutomationAt(Document& doc, int64_t tick) {
  for (auto* list : automationLists(doc))
    for (auto& a : *list) {
      // Two points: just before the boundary (the left side keeps its own end value) and on it.
      for (int64_t at : {tick - 1, tick}) {
        if (a.points.empty() || at <= a.points.front().tick || at >= a.points.back().tick) continue;
        auto it = std::lower_bound(a.points.begin(), a.points.end(), at, [](const AutomationPoint& p, int64_t v) { return p.tick < v; });
        if (it->tick == at) continue;
        const AutomationPoint& b = *it;
        const AutomationPoint& prev = *(it - 1);
        const double v = prev.value + (b.value - prev.value) * double(at - prev.tick) / double(b.tick - prev.tick);
        a.points.insert(it, AutomationPoint{at, v, json::object()});
      }
    }
}
void sortAutomation(Document& doc) {
  for (auto* list : automationLists(doc))
    for (auto& a : *list)
      std::stable_sort(a.points.begin(), a.points.end(), [](const AutomationPoint& x, const AutomationPoint& y) { return x.tick < y.tick; });
}
}  // namespace

std::string addSection(Document& doc, const std::string& name, int64_t startTick, int64_t lengthTicks) {
  if (doc.sections.size() >= 64) throw DocumentError("sections", "the song has the maximum of 64 sections");
  if (startTick < 0 || lengthTicks < 1) throw DocumentError("sections", "invalid position");
  for (const auto& s : doc.sections)
    if (startTick < s.startTick + s.lengthTicks && s.startTick < startTick + lengthTicks)
      throw DocumentError("sections", "overlaps the section '" + s.name + "'");
  Section s{nextId(doc.sections, "s"), cutUtf8(name, kMaxSectionNameBytes), startTick, lengthTicks, json::object()};  // never half a character (sweep D6)
  doc.sections.push_back(s);
  sortSections(doc);
  return s.id;
}

void renameSection(Document& doc, const std::string& sectionId, const std::string& name) {
  if (name.empty() || name.size() > 40) throw DocumentError("name", "must have 1-40 characters");
  sectionById(doc, sectionId).name = name;
}

void setSectionColor(Document& doc, const std::string& sectionId, const std::string& color) {
  static const std::regex pattern("^#[0-9A-Fa-f]{6}$");
  if (!color.empty() && !std::regex_match(color, pattern)) throw DocumentError("sections", "a colour must look like #rrggbb");
  sectionById(doc, sectionId).color = color;
}

void removeSection(Document& doc, const std::string& sectionId) {
  sectionById(doc, sectionId);
  doc.sections.erase(std::remove_if(doc.sections.begin(), doc.sections.end(), [&](const Section& s) { return s.id == sectionId; }),
                     doc.sections.end());
}

// Exchanges the music of a section with the next (or previous) one: both must touch. Clips that cross
// the boundaries are split there first; the song length does not change.
void swapSection(Document& doc, const std::string& sectionId, bool withNext) {
  sortSections(doc);
  auto it = std::find_if(doc.sections.begin(), doc.sections.end(), [&](const Section& s) { return s.id == sectionId; });
  if (it == doc.sections.end()) throw DocumentError("sections", "no section '" + sectionId + "'");
  if (withNext ? it + 1 == doc.sections.end() : it == doc.sections.begin()) throw DocumentError("sections", "no section on that side");
  Section& a = withNext ? *it : *(it - 1);
  Section& b = withNext ? *(it + 1) : *it;
  if (a.startTick + a.lengthTicks != b.startTick) throw DocumentError("sections", "only sections that touch can swap");
  const int64_t aStart = a.startTick, bStart = b.startTick, end = b.startTick + b.lengthTicks;
  for (int64_t at : {aStart, bStart, end}) { splitClipsAt(doc, at); splitAutomationAt(doc, at); }
  const int64_t aLen = bStart - aStart, bLen = end - bStart;
  for (auto* list : automationLists(doc))
    for (auto& a : *list)
      for (auto& p : a.points) {
        // Each section's curve, from its start point to the point just before its end, moves with it.
        if (p.tick >= aStart && p.tick < bStart) p.tick += bLen;
        else if (p.tick >= bStart && p.tick < end) p.tick -= aLen;
      }
  sortAutomation(doc);
  for (auto& t : doc.tracks) {
    for (auto& c : t.clips) {
      if (c.startTick >= aStart && c.startTick < bStart) c.startTick += bLen;       // A moves after B
      else if (c.startTick >= bStart && c.startTick < end) c.startTick -= aLen;     // B moves to A's place
    }
    for (auto& c : t.audioClips) {
      if (c.startTick >= aStart && c.startTick < bStart) c.startTick += bLen;
      else if (c.startTick >= bStart && c.startTick < end) c.startTick -= aLen;
    }
    sortClips(t);
  }
  std::swap(a.name, b.name);
  std::swap(a.extra, b.extra);
  std::swap(a.color, b.color);  // the colour goes with the name
  std::swap(a.id, b.id);
  a.lengthTicks = bLen;
  b.startTick = aStart + bLen;
  b.lengthTicks = aLen;
  sortSections(doc);
}

// Inserts a copy of the section right after it: everything later moves on, the song gets longer.
std::string duplicateSection(Document& doc, const std::string& sectionId) {
  const Section original = sectionById(doc, sectionId);
  const int64_t start = original.startTick, len = original.lengthTicks, end = start + len;
  if (doc.lengthTicks + len > 4096LL * 4 * kPpq) throw DocumentError("lengthTicks", "the song would be too long");
  if (doc.sections.size() >= 64) throw DocumentError("sections", "the song has the maximum of 64 sections");
  // The section's tempo changes go with its music (audit 2026-11-17, #4: they did not, and a duplicated section
  // lost the tempo Randomize axis J wrote for it). Decided here, before anything moves.
  std::vector<TempoPoint> tempoCopies;
  for (const auto& p : doc.tempoMap)
    if (p.tick >= start && p.tick < end) tempoCopies.push_back({p.tick + len, p.bpm, p.extra, p.ramp});
  if (doc.tempoMap.size() + tempoCopies.size() > (size_t)kMaxTempoPoints)
    throw DocumentError("tempoMap", "at most " + std::to_string(kMaxTempoPoints) + " tempo changes");
  // All or nothing (the same discipline as importMidi): a track over its 256 clips used to leave the automation,
  // the tempo map and the earlier tracks already moved.
  Document next = doc;
  splitClipsAt(next, start);
  splitClipsAt(next, end);
  splitAutomationAt(next, start);
  splitAutomationAt(next, end);
  for (auto* list : automationLists(next))
    for (auto& a : *list) {
      std::vector<AutomationPoint> copies;
      for (auto& p : a.points) {
        if (p.tick >= end) p.tick += len;                                        // later music moves on
        else if (p.tick >= start) { AutomationPoint c = p; c.tick += len; copies.push_back(c); }  // the section's curve is copied
      }
      // The original keeps its value just before the copy (the point at end - 1); the copy starts
      // with the section's start value.
      a.points.insert(a.points.end(), copies.begin(), copies.end());
    }
  sortAutomation(next);
  for (auto& t : next.tracks) {
    std::vector<Clip> copies;
    for (auto& c : t.clips) {
      if (c.startTick >= end) c.startTick += len;
      else if (c.startTick >= start) { Clip copy = c; copy.startTick += len; copies.push_back(copy); }
    }
    if (t.clips.size() + copies.size() > 256) throw DocumentError("clips", "a track would pass 256 clips");
    for (auto& c : copies) { c.id = nextId(t.clips, "c"); t.clips.push_back(c); }
    sortClips(t);
    std::vector<AudioClip> audioCopies;
    for (auto& c : t.audioClips) {
      if (c.startTick >= end) c.startTick += len;
      else if (c.startTick >= start) { AudioClip copy = c; copy.startTick += len; audioCopies.push_back(copy); }
    }
    if (t.audioClips.size() + audioCopies.size() > 1024) throw DocumentError("audioClips", "a track would pass 1024 audio clips");
    for (auto& c : audioCopies) { c.id = nextId(t.audioClips, "x"); t.audioClips.push_back(c); }
  }
  for (auto& s : next.sections) if (s.startTick >= end) s.startTick += len;
  // Match the inserted time: later tempo changes follow the later music, and the section's own ones are copied.
  for (auto& p : next.tempoMap) if (p.tick >= end) p.tick += len;
  next.tempoMap.insert(next.tempoMap.end(), tempoCopies.begin(), tempoCopies.end());
  std::sort(next.tempoMap.begin(), next.tempoMap.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });
  if (next.loop.startTick >= end) { next.loop.startTick += len; next.loop.endTick += len; }
  next.lengthTicks += len;
  Section copy{nextId(next.sections, "s"), nameWithSuffix(original.name, " 2", kMaxSectionNameBytes), end, len, json::object(), original.color};
  next.sections.push_back(copy);
  sortSections(next);
  doc = std::move(next);
  return copy.id;
}

void insertTime(Document& doc, int64_t at, int64_t ticks) {
  if (at < 0 || at > doc.lengthTicks || ticks <= 0) throw DocumentError("ticks", "invalid place or length");
  if (doc.lengthTicks + ticks > 4096LL * 4 * kPpq) throw DocumentError("lengthTicks", "the song would be too long");
  splitClipsAt(doc, at);
  splitAutomationAt(doc, at);
  for (auto* list : automationLists(doc))
    for (auto& a : *list)
      for (auto& p : a.points) if (p.tick >= at) p.tick += ticks;
  for (auto& t : doc.tracks) {
    for (auto& c : t.clips) if (c.startTick >= at) c.startTick += ticks;
    for (auto& c : t.audioClips) if (c.startTick >= at) c.startTick += ticks;
    sortClips(t);
  }
  for (auto& sec : doc.sections) {
    if (sec.startTick >= at) sec.startTick += ticks;
    else if (sec.startTick + sec.lengthTicks > at) sec.lengthTicks += ticks;
  }
  for (auto& p : doc.tempoMap) if (p.tick >= at) p.tick += ticks;
  if (doc.loop.startTick >= at) doc.loop.startTick += ticks;
  if (doc.loop.endTick > at) doc.loop.endTick += ticks;
  doc.lengthTicks += ticks;
}

void deleteTime(Document& doc, int64_t from, int64_t ticks) {
  const int64_t to = from + ticks;
  if (from < 0 || ticks <= 0 || to > doc.lengthTicks) throw DocumentError("ticks", "invalid place or length");
  if (doc.lengthTicks - ticks < kPpq) throw DocumentError("lengthTicks", "the song would be empty");
  splitClipsAt(doc, from);
  splitClipsAt(doc, to);
  splitAutomationAt(doc, from);
  splitAutomationAt(doc, to);
  auto map = [&](int64_t t) { return t < from ? t : t < to ? from : t - ticks; };
  for (auto* list : automationLists(doc))
    for (auto& a : *list) {
      a.points.erase(std::remove_if(a.points.begin(), a.points.end(), [&](const AutomationPoint& p) { return p.tick > from && p.tick < to; }),
                     a.points.end());
      for (auto& p : a.points) p.tick = map(p.tick);
      // Two points may now share `from` (the one before the cut and the one after): keep the later value.
      for (size_t i = 1; i < a.points.size();)
        if (a.points[i].tick == a.points[i - 1].tick && a.points[i].tick == from && i >= 1) a.points.erase(a.points.begin() + (long)(i - 1));
        else ++i;
    }
  sortAutomation(doc);
  for (auto& t : doc.tracks) {
    t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.startTick >= from && c.startTick < to; }), t.clips.end());
    for (auto& c : t.clips) if (c.startTick >= to) c.startTick -= ticks;
    t.audioClips.erase(std::remove_if(t.audioClips.begin(), t.audioClips.end(), [&](const AudioClip& c) { return c.startTick >= from && c.startTick < to; }),
                       t.audioClips.end());
    for (auto& c : t.audioClips) if (c.startTick >= to) c.startTick -= ticks;
    sortClips(t);
  }
  std::vector<Section> kept;
  for (auto sec : doc.sections) {
    const int64_t a = map(sec.startTick), b = map(sec.startTick + sec.lengthTicks);
    if (b <= a) continue;
    sec.startTick = a;
    sec.lengthTicks = b - a;
    kept.push_back(sec);
  }
  doc.sections = kept;
  // Tempo: the song plays as before up to `from`, and from there on as it played from `to` (audit B03: a glide
  // that crosses either edge keeps its shape, rather than the cut dropping its end point).
  if (!doc.tempoMap.empty()) {
    const int64_t oldLength = doc.lengthTicks;
    const double atTo = bpmAt(doc, to);
    const TempoPoint* firstAfter = nullptr;
    for (const auto& p : doc.tempoMap) if (p.tick > to) { firstAfter = &p; break; }
    const TempoPoint* crossing = nullptr;  // the point the song is heading for at `from`
    for (const auto& p : doc.tempoMap) if (p.tick > from) { crossing = &p; break; }
    std::vector<TempoPoint> tempo;
    for (const auto& p : doc.tempoMap) if (p.tick <= from) tempo.push_back(p);
    if (from == 0) {
      doc.bpm = atTo;
    } else {
      const bool step = to < oldLength && (std::abs(bpmAt(doc, from) - atTo) > 1e-9 || (firstAfter && firstAfter->ramp));
      if (crossing && crossing->ramp) {  // still gliding at `from`: arrive where the glide was
        const int64_t at = step || to >= oldLength ? from - 1 : from;
        if (at >= 1 && (tempo.empty() || at > tempo.back().tick)) {
          TempoPoint p = *crossing;
          p.tick = at;
          p.bpm = bpmAt(doc, at);
          tempo.push_back(p);
        }
      }
      // A point already at `from` arriving by a glide moves one tick back, so the step can take its place (or, at
      // the new end of the song, so it stays inside it).
      if ((step || to >= oldLength) && !tempo.empty() && tempo.back().tick == from) {
        TempoPoint arrive = tempo.back();
        tempo.pop_back();
        if (arrive.ramp && from - 1 >= 1 && (tempo.empty() || from - 1 > tempo.back().tick)) {
          arrive.tick = from - 1;
          arrive.bpm = bpmAt(doc, from - 1);
          tempo.push_back(arrive);
        }
      }
      if (step) {
        TempoPoint p;
        p.tick = from;
        p.bpm = atTo;
        tempo.push_back(p);
      }
    }
    for (auto p : doc.tempoMap)
      if (p.tick > to) { p.tick -= ticks; tempo.push_back(p); }
    doc.lengthTicks -= ticks;
    tempo.erase(std::remove_if(tempo.begin(), tempo.end(), [&](const TempoPoint& p) { return p.tick < 1 || p.tick >= doc.lengthTicks; }),
                tempo.end());
    doc.tempoMap = tempo;
  } else {
    doc.lengthTicks -= ticks;
  }
  const int64_t ls = map(doc.loop.startTick), le = map(doc.loop.endTick);
  doc.loop.startTick = ls;
  doc.loop.endTick = le;
  if (le <= ls) doc.loop.enabled = false;
}

// ---- effects

// CORE EQ in the catalogue: the same 82 parameters as the engine's (effects.cpp coreEqParams), in its order.
static EffectInfo coreEqInfo() {
  static std::deque<std::string> text;
  const auto keep = [](std::string s) { text.push_back(std::move(s)); return text.back().c_str(); };
  struct Def { double on, type, freq, q; };
  static const Def defs[8] = {{1, 3, 20, 0.707}, {1, 1, 100, 0.707}, {1, 0, 1000, 1}, {1, 2, 8000, 0.707},
                              {1, 4, 20000, 0.707}, {0, 0, 250, 1}, {0, 0, 2500, 1}, {0, 0, 5000, 1}};
  EffectInfo info{"eq", "Core EQ", {}, 2};
  auto& ps = info.params;
  for (int b = 0; b < 8; ++b) {
    const std::string id = "b" + std::to_string(b + 1), label = "Band " + std::to_string(b + 1);
    ps.push_back({keep(id + "On"), keep(label), defs[b].on, 0, 1, "", 1, false, {"Off", "On"}, false, "This band on or off"});
    ps.push_back({keep(id + "Type"), keep(label + " shape"), defs[b].type, 0, 5, "", 1, false,
                  {"Bell", "Low shelf", "High shelf", "Low cut", "High cut", "Notch"}, false, "What this band does to the sound"});
    ps.push_back({keep(id + "Freq"), keep(label + " frequency"), defs[b].freq, 20, 20000, "Hz", 0, true, {}, true, "Where the band sits"});
    ps.push_back({keep(id + "Gain"), keep(label + " gain"), 0, -24, 24, "dB", 0, false, {}, true, "Up or down (bells and shelves)"});
    ps.push_back({keep(id + "Q"), keep(label + " Q"), defs[b].q, 0.1, 18, "", 0, true, {}, true, "Narrow (high) or wide (low)"});
    ps.push_back({keep(id + "Slope"), keep(label + " slope"), 0, 0, 2, "", 1, false, {"12 dB/oct", "24 dB/oct", "48 dB/oct"}, false, "How steep a cut is"});
    ps.push_back({keep(id + "Chan"), keep(label + " channel"), 0, 0, 4, "", 1, false, {"Stereo", "Left", "Right", "Mid", "Side"}, false,
                  "Which part of the stereo sound the band works on"});
    ps.push_back({keep(id + "Dyn"), keep(label + " dynamic"), 0, 0, 1, "", 1, false, {"Off", "On"}, false,
                  "The band moves its gain only when that part of the sound gets loud"});
    ps.push_back({keep(id + "Thr"), keep(label + " threshold"), -24, -60, 0, "dB", 0, false, {}, true, "How loud the band's part must be before it moves"});
    ps.push_back({keep(id + "Range"), keep(label + " range"), -6, -24, 24, "dB", 0, false, {}, true, "How far it moves when loud (negative: it ducks)"});
  }
  ps.push_back({"output", "Output", 0, -24, 24, "dB", 0, false, {}, true, "The volume after the EQ"});
  ps.push_back({"autoGain", "Auto gain", 0, 0, 1, "", 1, false, {"Off", "On"}, false, "Keeps the overall loudness about the same while you EQ"});
  return info;
}

// CORE FX in the catalogue: the same 95 parameters as the engine's (effects.cpp coreFxParams), in its order.
static EffectInfo coreFxInfo() {
  static std::deque<std::string> text;  // names and labels kept for the pointers below (a deque never moves them)
  struct Mod { const char* id; const char* label; std::vector<const char*> types; EffectParamInfo a, b; bool on; double freq, amount, width; };
  const std::vector<const char*> times = {"1/16", "1/8", "1/8 D", "1/4", "1/4 D", "1/2", "1/2 D", "1 bar"};
  const std::vector<Mod> mods = {
      {"sat", "Saturate", {"Soft Clip", "Tube", "Tape", "Fold", "Hard Clip", "Rectify"},
       {"Drive", "Drive", 6, 0, 36, "dB", 0, false, {}, true, "How hard the region is pushed: more harmonics"},
       {"Tone", "Tone", 0.5, 0, 1, "", 0, false, {}, true, "Dark (left) or bright (right) harmonics"}, true, 150, 0.4, 2},
      {"rev", "Reverb", {"Hall", "Cathedral", "Shimmer", "Plate", "Spring", "Reverse", "Stone church"},
       {"PreDelay", "Pre-delay", 20, 0, 250, "ms", 0, false, {}, false, "A gap before the tail starts"},
       {"Decay", "Decay", 2, 0.2, 20, "s", 0, true, {}, true, "How long the tail rings"}, true, 6000, 0.3, 2.5},
      {"del", "Delay", {"Ping Pong", "Stereo", "Freq Shift"},
       {"Time", "Time", 2, 0, 7, "", 1, false, times, false, "How far apart the echoes are, in notes"},
       {"Feedback", "Feedback", 0.35, 0, 0.95, "", 0, false, {}, true, "How many echoes"}, true, 2500, 0.3, 2},
      {"mod", "Modulate", {"Phaser", "Flanger", "Chorus"},
       {"Depth", "Depth", 0.5, 0, 1, "", 0, false, {}, true, "How far it moves"},
       {"Rate", "Rate", 2, 0, 6, "", 1, false, {"1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"}, true, "How fast it moves, on the song's tempo"},
       false, 900, 0.3, 2},
      {"lofi", "Lo-fi", {"Digital", "Radio", "Vinyl"},
       {"Bits", "Bits", 12, 2, 16, "", 0, false, {}, true, "Fewer bits: grainier, broken"},
       {"Rate", "Rate", 22000, 1000, 48000, "Hz", 0, true, {}, true, "A lower sample rate: metallic aliasing"}, false, 9000, 0.25, 1.5}};
  const auto keep = [](std::string s) { text.push_back(std::move(s)); return text.back().c_str(); };
  EffectInfo info{"corefx", "Core FX", {}, 1};
  auto& ps = info.params;
  ps.push_back({"amount", "Amount", 0.5, 0, 1, "", 0, false, {}, true, "How much of all the effects: 0 is the dry sound only"});
  ps.push_back({"fxOnly", "FX Only", 0, 0, 1, "", 1, false, {"Off", "On"}, false, "Only the effects, without the dry sound (an FX bus)"});
  ps.push_back({"limiter", "Limiter", 1, 0, 1, "", 1, false, {"Off", "On"}, false, "Keeps the output under -0.3 dB"});
  ps.push_back({"output", "Output", 0, -24, 6, "dB", 0, false, {}, true, "The volume after everything"});
  ps.push_back({"solo", "Solo", 0, 0, 5, "", 1, false, {"Off", "Saturate", "Reverb", "Delay", "Modulate", "Lo-fi"}, false, "Hear one module alone"});
  static const double spread[4] = {1, 0.5, 2, 4};
  for (const auto& m : mods) {
    const std::string id = m.id, label = m.label;
    // On can be automated (attachments switch it): the module's send fades in and out, nothing clicks.
    ps.push_back({keep(id + "On"), keep(label), m.on ? 1.0 : 0.0, 0, 1, "", 1, false, {"Off", "On"}, true, nullptr});
    ps.push_back({keep(id + "Type"), keep(label + " type"), 0, 0, (double)m.types.size() - 1, "", 1, false, m.types, false, nullptr});
    EffectParamInfo a = m.a, b = m.b;
    a.name = keep(id + m.a.name); b.name = keep(id + m.b.name);
    ps.push_back(a);
    ps.push_back(b);
    ps.push_back({keep(id + "Return"), keep(label + " return"), 0, -24, 24, "dB", 0, false, {}, true, "How loud what comes back from this module is"});
    ps.push_back({keep(id + "Fit"), keep(label + " fit to regions"), 1, 0, 1, "", 1, false, {"Off", "On"}, false,
                  "Filters what comes back with the same regions, so it stays where it was sent from"});
    for (int r = 0; r < 4; ++r) {
      const std::string rn = id + "R" + std::to_string(r + 1), rl = label + " region " + std::to_string(r + 1);
      ps.push_back({keep(rn + "Freq"), keep(rl + " frequency"), std::clamp(m.freq * spread[r], 20.0, 20000.0), 20, 20000, "Hz", 0, true, {}, true, nullptr});
      ps.push_back({keep(rn + "Amount"), keep(rl + " amount"), r == 0 ? m.amount : 0.0, 0, 1, "", 0, false, {}, true, nullptr});
      ps.push_back({keep(rn + "Width"), keep(rl + " width"), m.width, 0.3, 8, "oct", 0, false, {}, true, nullptr});
    }
  }
  // The psytrance wash (Diogo 2026-11-08: "nao é wet é wash"): the delay's echoes go on into the reverb.
  ps.push_back({"delToRev", "Delay into reverb", 0, 0, 1, "", 0, false, {}, true, "How much of the delay's echoes goes on into the reverb: the echoes smear into the tail"});
  ps.push_back({"dry", "Dry", 1, 0, 1, "", 0, false, {}, true, "The dry sound's level under the effects"});
  return info;
}

const std::vector<EffectInfo>& builtinEffects() {
  static const std::vector<EffectInfo> effects = {
      {"filter", "Filter", {{"mode", "Type", 0, 0, 19, "", 1, false, {"Low-pass", "High-pass", "Band-pass", "Low 6", "High 6", "Band 24",
                                                                 "Notch", "Ladder", "Comb", "Formant", "Low 24", "High 24",
                                                                 "Diode", "MS-20 Low", "MS-20 High", "Comb -", "Phaser", "Morph",
                                                                 "Dual Notch", "Dual Band"}},
                            {"cutoff", "Cutoff", 2000, 20, 20000, "Hz", 0, true, {}}, {"resonance", "Resonance", 0.707, 0.3, 10, "", 0, true, {}},
                            {"mix", "Mix", 1, 0, 1, "", 0, false, {}}, {"drive", "Drive", 0, 0, 1, "", 0, false, {}},
                            {"shape", "Shape", 0.5, 0, 1, "", 0, false, {}}}},
      {"compressor", "Compressor", {{"threshold", "Threshold", -18, -60, 0, "dB", 0, false, {}}, {"ratio", "Ratio", 4, 1, 20, ":1", 0, true, {}},
                                    {"attack", "Attack", 10, 0.1, 100, "ms", 0, true, {}}, {"release", "Release", 100, 10, 1000, "ms", 0, true, {}},
                                    {"makeup", "Gain", 0, 0, 24, "dB", 0, false, {}}, {"knee", "Knee", 6, 0, 12, "dB", 0, false, {}}}},
      {"limiter", "Limiter", {{"ceiling", "Ceiling", -0.3, -12, 0, "dB", 0, false, {}}, {"release", "Release", 80, 10, 500, "ms", 0, true, {}}}, 2},
      {"utility", "Utility", {{"gain", "Gain", 0, -24, 24, "dB", 0, false, {}}, {"width", "Width", 1, 0, 2, "", 0, false, {}},
                                 {"monoBelow", "Mono below", 20, 20, 300, "Hz", 0, true, {}},
                                 {"polarity", "Polarity", 0, 0, 1, "", 1, false, {"Normal", "Flipped (Ø)"}, false}}},
      // Core Devices: one musical result each (engine devices.cpp)
      {"growl", "GROWL", {{"vowel", "Vowel", 0.3, 0, 1, "", 0, false, {}, true, "Which vowel the sound says: a, e, i, o, u from left to right"},
                          {"talk", "Talk", 0.5, 0, 1, "", 0, false, {}, true, "How much the vowel moves on the beat: the bass talks"},
                          {"grit", "Grit", 12, 0, 36, "dB", 0, false, {}, true, "How dirty the talking part gets"},
                          {"body", "Body (clean below)", 150, 60, 400, "Hz", 0, true, {}, true, "Below this the bass stays clean and untouched"},
                          {"mix", "Mix", 1, 0, 1, "", 0, false, {}, true, "How much growl against the original sound"}}},
      {"metal", "METAL", {{"note", "Note", 30, 24, 84, "note", 1, false, {}, true, "The note the metal rings at. A preset sets it to the key of your bass"},
                          {"ring", "Ring", 0.2, 0, 1, "", 0, false, {}, true, "A harsher, bell-like grain on the ring"},
                          {"decay", "Decay", 120, 10, 2000, "ms", 0, true, {}, true, "How long the metal rings after each hit"},
                          {"crush", "Crush", 0, 0, 1, "", 0, false, {}, true, "Takes detail away: digital, broken, lo-fi"},
                          {"mix", "Mix", 0.35, 0, 1, "", 0, false, {}, true, "How much metal against the original sound"}}},
      {"punch", "PUNCH", {{"click", "Click", 6, -12, 18, "dB", 0, false, {}, true, "Louder or softer start of each hit: the click of a kick"},
                          {"tail", "Tail", 0, -24, 12, "dB", 0, false, {}, true, "Louder or softer what follows the hit: shorter or fuller"},
                          {"clip", "Clip", 0.3, 0, 1, "", 0, false, {}, true, "Rounds off the loudest peaks: louder and harder"},
                          {"output", "Output", 0, -24, 6, "dB", 0, false, {}, true, "The volume after everything"}}},
      {"zap", "ZAP", {{"pitch", "Pitch", 1500, 100, 8000, "Hz", 0, true, {}, true, "Where the laser sits: low is a bloop, high a pew"},
                      {"amount", "Amount", 0.6, 0, 1, "", 0, false, {}, true, "How much each hit is smeared into a laser"},
                      {"sweep", "Sweep", 2, -4, 4, "oct", 0, false, {}, true, "How far the laser falls (or rises, below 0) on each hit"},
                      {"length", "Length", 80, 10, 500, "ms", 0, true, {}, true, "How long each laser sweep takes"},
                      {"mix", "Mix", 1, 0, 1, "", 0, false, {}, true, "How much laser against the original sound"},
                      {"sync", "Sync", 0, 0, 5, "", 1, false, {"Off", "1/16", "1/8", "1/4", "1/2", "1 bar"}, false,
                       "Off: a laser on every hit. Otherwise a laser on the grid, in time"}}},
      {"glitch", "GLITCH", {{"rate", "Rate", 2, 0, 3, "", 1, false, {"1/8", "1/16", "1/32", "1/64"}, true,
                             "How short the repeated piece is: 1/8 is a stammer, 1/64 a buzz"},
                            {"repeats", "Repeats", 4, 2, 16, "", 1, false, {}, true, "How many times the piece sounds before the music goes on"},
                            {"chance", "Chance", 0.3, 0, 1, "", 0, false, {}, true, "How often a beat stutters: 0 never, 1 on every beat"},
                            {"mix", "Mix", 1, 0, 1, "", 0, false, {}, true, "How loud the repeats are against the music: low = far away"},
                            {"reverse", "Reverse", 0, 0, 1, "", 0, false, {}, true, "How often the repeats play backwards: a reverse swoosh"},
                            {"pitch", "Pitch", 0, -24, 24, "st", 0, false, {}, true, "The repeats slide down (a tape stop) or up (a rising pew) by the last one; 0 = as played"},
                            {"tone", "Tone", 0, -1, 1, "", 0, false, {}, true, "Each repeat gets darker (left) or thinner (right), so they sit behind the music; 0 = as played"},
                            {"vary", "Vary", 0, 0, 1, "", 0, false, {}, true, "How often a stutter takes a longer or shorter piece than Rate: less like a machine"},
                            {"length", "Length", 1, 0.05, 1, "", 0, false, {}, true, "How much of each repeat sounds: short = a choppy gate, full = smooth"},
                            {"echo", "Echo", 0, 0, 1, "", 0, false, {}, true, "The repeats ring on in a short dark echo after the stutter, instead of stopping dry"},
                            {"spread", "Spread", 0, 0, 1, "", 0, false, {}, true, "The repeats jump left and right"}}},
      {"pulse", "PULSE", {{"pattern", "Pattern", 1, 0, 7, "", 1, false,
                           {"Eighths", "KBBB", "Off-beat", "Gallop", "Trance gate", "Broken", "Quarters", "Half bar"}, true,
                           "Which sixteenths of the bar are open: the rhythm the volume is cut to"},
                          {"depth", "Depth", 0.8, 0, 1, "", 0, false, {}, true, "How much the closed steps go down: 1 is silence"},
                          {"smooth", "Smooth", 0.2, 0, 1, "", 0, false, {}, true, "Soft edges: low cuts hard and clicky, high breathes"},
                          {"swing", "Swing", 0, 0, 1, "", 0, false, {}, true, "Pushes every second sixteenth late: a rolling, human feel"}}},
      {"body", "BODY", {{"drive", "Drive", 6, 0, 24, "dB", 0, false, {}, true, "How much warmth and thickness above the sub"},
                        {"color", "Color", 900, 100, 8000, "Hz", 0, true, {}, true, "Where the warmth sits: low is fat, high is edgy"},
                        {"width", "Width", 0.4, 0, 1, "", 0, false, {}, true, "Wider on the sides, the middle and the sub stay in place"},
                        {"mix", "Mix", 1, 0, 1, "", 0, false, {}, true, "How much body against the original sound"}}},
      coreEqInfo(), coreFxInfo()};
  return effects;
}

// Starting points per style (sources and why: AICORE docs/knowledge, GLITCH and PULSE).
static std::vector<EffectPreset> presetRecipes() {
  return {
      // GROWL, METAL, PUNCH, ZAP: values of the style racks they replace (darkpsycore/growl-talk: Formant
      // 300 Hz-2 kHz + Faturator drive 65 %, mix 80 %; darkpsycore/metal-hits: Resonator F#1, 60 ms, mix 40 %,
      // ring mod 20 %; hitech/kick-click: transient attack 50 %, clipper -3 dB; hitech/zap-laser: Disperser 2 kHz,
      // comb 40 %), and the AI guide (the sub stays clean; at 225 BPM everything is shorter).
      {"growl", "darkpsycore", "KBBB talk", "The KBBB bass's middle speaks, the sub stays clean (put it on the bass)",
       {{"vowel", 0.2}, {"talk", 0.6}, {"grit", 15}, {"body", 150}, {"mix", 0.7}}},
      {"growl", "darkpsycore", "Night talk", "Darker o-u vowels, slower talk: a night bass that mutters",
       {{"vowel", 0.75}, {"talk", 0.4}, {"grit", 10}, {"body", 180}, {"mix", 0.6}}},
      {"growl", "psytrance", "Twilight vowel", "A light vowel on the rolling bass, more colour than growl",
       {{"vowel", 0.4}, {"talk", 0.3}, {"grit", 6}, {"body", 140}, {"mix", 0.5}}},
      {"growl", "hitech", "Snarl", "Fast, dirty talk for hi-tech basses and leads",
       {{"vowel", 0.5}, {"talk", 0.85}, {"grit", 18}, {"body", 200}, {"mix", 0.6}}},
      {"growl", "slambient", "Low moan", "A slow dark vowel for drones and long notes",
       {{"vowel", 0.85}, {"talk", 0.15}, {"grit", 4}, {"body", 120}, {"mix", 0.5}}},
      {"metal", "darkpsycore", "Key hits", "Percussion rings a short low metal note in the key of your bass (F#1 without a bass)",
       {{"note", 30}, {"ring", 0.2}, {"decay", 60}, {"crush", 0}, {"mix", 0.4}}},
      {"metal", "darkpsycore", "Rust", "Dirtier and longer: rusty, broken metal on accents",
       {{"note", 30}, {"ring", 0.4}, {"decay", 120}, {"crush", 0.5}, {"mix", 0.45}}},
      {"metal", "psytrance", "Tuned perc", "Percussion gets a short tuned ring, an octave up",
       {{"note", 42}, {"ring", 0.1}, {"decay", 90}, {"crush", 0}, {"mix", 0.3}}},
      {"metal", "hitech", "Glass bells", "High, clean bells on short hits",
       {{"note", 66}, {"ring", 0.1}, {"decay", 300}, {"crush", 0}, {"mix", 0.35}}},
      {"metal", "slambient", "Iron drone", "Hits ring long and low, like a struck iron beam",
       {{"note", 30}, {"ring", 0.3}, {"decay", 1500}, {"crush", 0.2}, {"mix", 0.3}}},
      {"punch", "hitech", "Kick click", "More attack on the kick, a clipper catches the peak",
       {{"click", 6}, {"tail", 0}, {"clip", 0.25}, {"output", 0}}},
      {"punch", "hitech", "Bass snap", "A sharp start and a short tail for fast bass notes",
       {{"click", 8}, {"tail", -6}, {"clip", 0.4}, {"output", 0}}},
      {"punch", "darkpsycore", "Tight kick", "A harder kick with a shorter tail, room for the KBBB bass",
       {{"click", 4}, {"tail", -4}, {"clip", 0.3}, {"output", 0}}},
      {"punch", "psytrance", "Rolling bass", "Each bass note starts clearer, the roll stays even",
       {{"click", 3}, {"tail", -2}, {"clip", 0.2}, {"output", 0}}},
      {"punch", "slambient", "Soft thump", "Softer start, fuller body: a slow, round hit",
       {{"click", -3}, {"tail", 2}, {"clip", 0}, {"output", 0}}},
      {"zap", "hitech", "Laser", "Accents turn into lasers that fall two octaves",
       {{"pitch", 2000}, {"amount", 0.7}, {"sweep", 2}, {"length", 60}, {"mix", 1}}},
      {"zap", "hitech", "Click zap", "A tiny zap on each click, for kicks and hats",
       {{"pitch", 3000}, {"amount", 0.4}, {"sweep", 1}, {"length", 20}, {"mix", 0.6}}},
      {"zap", "darkpsycore", "Down laser", "Longer, deeper lasers for dark accents",
       {{"pitch", 800}, {"amount", 0.8}, {"sweep", 3}, {"length", 120}, {"mix", 0.8}}},
      {"zap", "psytrance", "Psy zap", "The classic psy zap on short synth stabs",
       {{"pitch", 1500}, {"amount", 0.6}, {"sweep", 2}, {"length", 80}, {"mix", 0.7}}},
      {"zap", "slambient", "Slow chirp", "A slow chirp that rises out of each hit",
       {{"pitch", 600}, {"amount", 0.5}, {"sweep", -2}, {"length", 400}, {"mix", 0.5}}},
      {"glitch", "hitech", "Stutter 1/32", "Fast stutters on some beats, the hi-tech signature",
       {{"rate", 2}, {"repeats", 4}, {"chance", 0.35}, {"mix", 1}}},
      {"glitch", "hitech", "Machine gun 1/64", "Rare bursts that buzz like a machine gun",
       {{"rate", 3}, {"repeats", 8}, {"chance", 0.2}, {"mix", 1}}},
      {"glitch", "darkpsycore", "Fill 1/16", "A broken fill now and then, for the end of a phrase",
       {{"rate", 1}, {"repeats", 4}, {"chance", 0.25}, {"mix", 1}}},
      {"glitch", "darkpsycore", "Broken 1/32", "Restless chops all along, a little under the music",
       {{"rate", 2}, {"repeats", 6}, {"chance", 0.3}, {"mix", 0.8}}},
      {"glitch", "slambient", "Distant echo 1/8", "Slow stammers far in the back (Mix 20 %)",
       {{"rate", 0}, {"repeats", 3}, {"chance", 0.25}, {"mix", 0.2}}},
      {"glitch", "psytrance", "Light 1/16", "A rare, short stutter; leave it off in progressive",
       {{"rate", 1}, {"repeats", 2}, {"chance", 0.12}, {"mix", 0.6}}},
      {"pulse", "psytrance", "KBBB pump", "The bass opens between the kicks: the rolling psy groove",
       {{"pattern", 1}, {"depth", 0.8}, {"smooth", 0.25}, {"swing", 0}}},
      {"pulse", "psytrance", "Off-beat", "Open only between the kicks, like an off-beat bass",
       {{"pattern", 2}, {"depth", 0.7}, {"smooth", 0.3}, {"swing", 0}}},
      {"pulse", "hitech", "Pre-drop gate", "A hard trance gate for the bars before the drop",
       {{"pattern", 4}, {"depth", 1}, {"smooth", 0.1}, {"swing", 0}}},
      {"pulse", "hitech", "Gallop", "A galloping cut, fast and tight",
       {{"pattern", 3}, {"depth", 0.9}, {"smooth", 0.1}, {"swing", 0}}},
      {"pulse", "darkpsycore", "Chop 1/8", "Every eighth cut short, a little late, for pads and noise",
       {{"pattern", 0}, {"depth", 0.85}, {"smooth", 0.15}, {"swing", 0.2}}},
      {"pulse", "slambient", "Slow breath", "Half a bar open, half held down, with soft edges",
       {{"pattern", 7}, {"depth", 0.6}, {"smooth", 1}, {"swing", 0}}},
      // SPACE: values of the style racks' ValhallaVintageVerb/Supermassive settings it replaces, and the AI guide
      // (AICORE docs/knowledge/sound-techniques: space-slambient-1/2, no reverb below ~150 Hz).
      {"space", "darkpsycore", "Dark room", "A short dark room: the dark psy rack's reverb, low end kept out",
       {{"size", 0.4}, {"decay", 1.2}, {"predelay", 20}, {"tone", 6000}, {"lowcut", 380}, {"motion", 0.2}, {"mix", 0.25}}},
      {"space", "darkpsycore", "Night hall", "A longer, darker hall for pads and FX, behind the music",
       {{"size", 0.75}, {"decay", 4}, {"predelay", 30}, {"tone", 3500}, {"lowcut", 300}, {"motion", 0.4}, {"mix", 0.3}}},
      {"space", "psytrance", "Progressive space", "A soft 2 s hall for leads and pads, bright but not harsh",
       {{"size", 0.6}, {"decay", 2}, {"predelay", 10}, {"tone", 8000}, {"lowcut", 380}, {"motion", 0.3}, {"mix", 0.25}}},
      {"space", "psytrance", "Bass air", "Just a touch of room on the bass's top, the sub stays dry",
       {{"size", 0.4}, {"decay", 1.2}, {"predelay", 20}, {"tone", 6000}, {"lowcut", 380}, {"motion", 0.1}, {"mix", 0.15}}},
      {"space", "hitech", "Tight room", "Hi-tech stays dry: a very short room only to glue the hits",
       {{"size", 0.25}, {"decay", 0.6}, {"predelay", 0}, {"tone", 9000}, {"lowcut", 500}, {"motion", 0}, {"mix", 0.12}}},
      {"space", "slambient", "Abyss", "A huge 7 s room for slow hits, its top kept dark",
       {{"size", 1}, {"decay", 7}, {"predelay", 40}, {"tone", 6000}, {"lowcut", 200}, {"motion", 0.3}, {"mix", 0.35}}},
      {"space", "slambient", "Huge space", "Endless space, 15 s: the slambient background",
       {{"size", 1}, {"decay", 15}, {"predelay", 60}, {"tone", 7000}, {"lowcut", 200}, {"motion", 0.3}, {"mix", 0.5}}},
      {"space", "slambient", "Haunted", "A long tail that slowly wobbles: uneasy, ghostly",
       {{"size", 0.9}, {"decay", 15}, {"predelay", 30}, {"tone", 8000}, {"lowcut", 150}, {"motion", 0.65}, {"mix", 0.5}}},
      // ECHO: values of the style racks' kHs Dual Delay / ValhallaFreqEcho settings it replaces (mix, feedback,
      // duck 40 %, low cut 300 Hz); 3/16 and 1/8 dotted are the psytrance lead delays of the AI guide.
      {"echo", "psytrance", "Acid 3/16", "The classic psy lead delay: dotted eighths that duck under the notes",
       {{"time", 2}, {"feedback", 0.4}, {"tone", 6000}, {"lowcut", 300}, {"pingpong", 0.6}, {"duck", 0.4}, {"mix", 0.25}}},
      {"echo", "psytrance", "Progressive 1/8", "Soft, darker eighths for progressive pads and leads",
       {{"time", 1}, {"feedback", 0.35}, {"tone", 3500}, {"lowcut", 300}, {"pingpong", 0.5}, {"duck", 0.2}, {"mix", 0.2}}},
      {"echo", "hitech", "Motion 1/16", "Fast sixteenths that make a lead run, ducked so it stays clean",
       {{"time", 0}, {"feedback", 0.35}, {"tone", 8000}, {"lowcut", 400}, {"pingpong", 0.8}, {"duck", 0.4}, {"mix", 0.2}}},
      {"echo", "darkpsycore", "Dark trail", "A long dark trail that spirals around the lead",
       {{"time", 2}, {"feedback", 0.6}, {"tone", 3000}, {"lowcut", 300}, {"pingpong", 1}, {"duck", 0.2}, {"mix", 0.25}}},
      {"echo", "darkpsycore", "Throw 1/4", "One or two quarter-note echoes, for a vocal or an FX at the end of a phrase",
       {{"time", 3}, {"feedback", 0.3}, {"tone", 5000}, {"lowcut", 350}, {"pingpong", 0.5}, {"duck", 0}, {"mix", 0.3}}},
      // SPIRAL: the ValhallaFreqEcho settings of darkpsycore/dark-spiral-lead and psytrance/twilight-swirl
      // (shift 12 / 8 Hz, ~100 ms, feedback 60 %, low cut 300 Hz, mix 25 / 20 %) and slambient/slow-unease (4 Hz).
      {"spiral", "darkpsycore", "Dark spiral", "Each echo climbs 12 Hz: the lead spirals out of tune",
       {{"time", 1}, {"shift", 12}, {"feedback", 0.6}, {"tone", 4000}, {"lowcut", 300}, {"mix", 0.25}}},
      {"spiral", "psytrance", "Twilight swirl", "A slow 8 Hz climb on the echoes, dreamy twilight leads",
       {{"time", 1}, {"shift", 8}, {"feedback", 0.6}, {"tone", 6000}, {"lowcut", 300}, {"mix", 0.2}}},
      {"spiral", "hitech", "Laser fall", "Fast 1/16 echoes that fall away, for zaps and stabs",
       {{"time", 0}, {"shift", -25}, {"feedback", 0.55}, {"tone", 8000}, {"lowcut", 400}, {"mix", 0.25}}},
      {"spiral", "slambient", "Slow unease", "Long echoes drifting 4 Hz each turn: something is wrong",
       {{"time", 5}, {"shift", 4}, {"feedback", 0.7}, {"tone", 3000}, {"lowcut", 200}, {"mix", 0.3}}},
      // slambient/haunted: kHs Frequency Shifter (Eerie macro 0-15 Hz, 30 % = 4.5 Hz, no feedback) next to the dry sound
      {"spiral", "slambient", "Haunted beats", "A copy a few Hz off the sound: slow beats that never settle",
       {{"time", 0}, {"shift", 4.5}, {"feedback", 0}, {"tone", 8000}, {"lowcut", 200}, {"mix", 0.35}}},
      // BODY: Faturator (drive 65 %, colour 900 Hz, mix 80 %) of darkpsycore/growl-talk, kHs Ensemble (spread
      // 70-75 %, mix 40-60 %) of psytrance/progressive-space and slambient/haunted; the sub stays clean (AI guide).
      {"body", "darkpsycore", "Fat mids", "Thick, warm mids on the bass or lead; the sub stays clean",
       {{"drive", 12}, {"color", 900}, {"width", 0.2}, {"mix", 0.8}}},
      {"body", "psytrance", "Wide pad", "A wider, warmer pad or lead, the middle stays put",
       {{"drive", 4}, {"color", 1500}, {"width", 0.7}, {"mix", 1}}},
      {"body", "hitech", "Edge", "A hard edge up high so short synths cut through",
       {{"drive", 15}, {"color", 3000}, {"width", 0.1}, {"mix", 0.7}}},
      {"body", "slambient", "Haunted width", "Very wide and soft, for drones and ghostly pads",
       {{"drive", 3}, {"color", 600}, {"width", 0.9}, {"mix", 1}}},
      // FLUX: kHs Flanger of darkpsycore/hats-flange (0.2 Hz, feedback 50 %, mix 50 %), kHs Phaser of
      // slambient/slow-unease (0.05 Hz, depth 60 %, mix 50 %) and buildup/tension (1.81 Hz), kHs Ensemble of
      // psytrance/progressive-space (detune 25 %, mix 40 %), ValhallaSpaceModulator of hitech/lead-motion (30 %) and
      // psytrance/twilight-swirl (35 %, feedback 50 %).
      {"flux", "darkpsycore", "Hats flange", "A slow metal flange on bright hats, no louder",
       {{"mode", 1}, {"rate", 0.2}, {"depth", 0.6}, {"feedback", 0.5}, {"mix", 0.5}}},
      {"flux", "darkpsycore", "Tension phaser", "A fast phaser that makes the build-up nervous",
       {{"mode", 0}, {"rate", 1.81}, {"depth", 0.7}, {"feedback", 0.6}, {"mix", 0.5}}},
      {"flux", "psytrance", "Wide ensemble", "A wider, thicker pad or lead",
       {{"mode", 2}, {"rate", 0.8}, {"depth", 0.25}, {"feedback", 0}, {"mix", 0.4}}},
      {"flux", "psytrance", "Twilight motion", "Slow flanged motion on twilight leads",
       {{"mode", 1}, {"rate", 0.3}, {"depth", 0.5}, {"feedback", 0.5}, {"mix", 0.35}}},
      {"flux", "hitech", "Space motion", "A light moving shine on fast leads",
       {{"mode", 2}, {"rate", 0.5}, {"depth", 0.4}, {"feedback", 0}, {"mix", 0.3}}},
      {"flux", "slambient", "Slow unease phase", "A very slow phaser sweep, like the room is breathing",
       {{"mode", 0}, {"rate", 0.05}, {"depth", 0.6}, {"feedback", 0.5}, {"mix", 0.5}}},
      // slambient/haunted: kHs Pitch Shifter (jitter 45 %, grains 36 ms, mix 40 %) and kHs Ensemble (detune 25 %,
      // spread 75 %, mix 60 %): a slow, deep chorus gives the same wandering pitch
      {"flux", "slambient", "Haunted ensemble", "Voices that drift apart and wander out of tune: ghostly pads",
       {{"mode", 2}, {"rate", 0.18}, {"depth", 0.75}, {"feedback", 0}, {"mix", 0.5}}},
      // GLITCH Reverse (kHs Reverser of slambient/ghost-voice, mix 35 %), SPACE Shimmer (ValhallaShimmer of
      // slambient/ghost-voice), ZAP on the grid (hi-tech lasers in time).
      {"glitch", "slambient", "Reverse fill", "Now and then a slow piece plays backwards, far away",
       {{"rate", 0}, {"repeats", 2}, {"chance", 0.2}, {"mix", 0.35}, {"reverse", 1}}},
      {"glitch", "darkpsycore", "Backspin 1/16", "Stutters that often spin backwards",
       {{"rate", 1}, {"repeats", 4}, {"chance", 0.2}, {"mix", 1}, {"reverse", 0.7}}},
      {"space", "slambient", "Ghost shimmer", "A long tail that rises an octave: ghostly choir",
       {{"size", 0.9}, {"decay", 10}, {"predelay", 40}, {"tone", 9000}, {"lowcut", 300}, {"motion", 0.4}, {"mix", 0.4}, {"shimmer", 0.5}}},
      // SPACE types (2026-11-07, the psychedelic reverb list). Measured against Diogo's samples: the psy "CLAP REVERB"
      // tail (2.8 s, centre 1.7 kHz, below 500 Hz -22 dB, almost mono), the spring drums (centre 1.6-2.2 kHz, sparse first).
      {"space", "psytrance", "Psy clap plate", "A bright, dense plate behind claps and snares (it rings like the psy CLAP REVERB sample, 2.6 s), the low end kept out",
       {{"kind", 1}, {"size", 0.5}, {"decay", 4.5}, {"predelay", 10}, {"tone", 4000}, {"lowcut", 500}, {"motion", 0.1}, {"mix", 0.3}}},
      {"space", "psytrance", "Glass plate", "A glassy plate for leads and vocals: the top lifted, a touch of shimmer",
       {{"kind", 1}, {"size", 0.6}, {"decay", 3}, {"predelay", 20}, {"tone", 12000}, {"lowcut", 400}, {"motion", 0.3}, {"mix", 0.25}, {"shimmer", 0.15}, {"air", 0.6}}},
      {"space", "darkpsycore", "Rusty spring", "A twangy metal spring: dub hits, weird FX and acid stabs bounce in it",
       {{"kind", 2}, {"size", 0.6}, {"decay", 2.5}, {"predelay", 0}, {"tone", 4500}, {"lowcut", 200}, {"motion", 0}, {"mix", 0.3}}},
      {"space", "slambient", "Cathedral", "A huge stone church: the tail builds up slowly and rings 12 s",
       {{"kind", 3}, {"size", 1}, {"decay", 12}, {"predelay", 40}, {"tone", 5000}, {"lowcut", 200}, {"motion", 0.4}, {"mix", 0.45}}},
      {"space", "slambient", "Reverse swell", "A ghostly rush that swells up after each hit for 1 s and stops",
       {{"kind", 4}, {"decay", 1}, {"predelay", 0}, {"tone", 7000}, {"lowcut", 300}, {"mix", 0.4}}},
      {"space", "psytrance", "Reverse 1/2 bar", "A swell of about half a bar at 145 BPM behind leads and vocals in the breaks",
       {{"kind", 4}, {"decay", 0.83}, {"predelay", 0}, {"tone", 8000}, {"lowcut", 400}, {"mix", 0.35}}},
      {"space", "slambient", "Total wash", "Almost only the tail: the sound dissolves into a wall of space",
       {{"kind", 3}, {"size", 0.9}, {"decay", 20}, {"predelay", 30}, {"tone", 6000}, {"lowcut", 150}, {"motion", 0.6}, {"mix", 0.85}, {"shimmer", 0.2}}},
      {"space", "psytrance", "Sunrise shimmer", "A bright shimmer under a lead in the breakdown",
       {{"size", 0.7}, {"decay", 4}, {"predelay", 20}, {"tone", 10000}, {"lowcut", 400}, {"motion", 0.3}, {"mix", 0.25}, {"shimmer", 0.35}}},
      {"zap", "hitech", "Laser 1/16", "A laser on every sixteenth, locked to the grid",
       {{"pitch", 2000}, {"amount", 0.7}, {"sweep", 2}, {"length", 40}, {"mix", 0.8}, {"sync", 1}}},
      {"echo", "slambient", "Slow canyon 1/2", "Half-note echoes that fade far away, for slow hits",
       {{"time", 5}, {"feedback", 0.55}, {"tone", 2500}, {"lowcut", 200}, {"pingpong", 0.7}, {"duck", 0}, {"mix", 0.3}}}};
}

const std::vector<EffectPreset>& effectPresets() {
  static const std::vector<EffectPreset> presets = presetRecipes();
  return presets;
}

const EffectPreset* findEffectPreset(const std::string& effect, const std::string& name) {
  for (const auto& p : effectPresets())
    if (effect == p.effect && name == p.name) return &p;
  return nullptr;
}

const EffectInfo* findEffect(const std::string& name) {
  for (const auto& e : builtinEffects()) if (name == e.name) return &e;
  return nullptr;
}

void upgradeOldVersions(Document& d) {
  auto chain = [](json& effects) {
    for (auto& fx : effects) {
      if (!fx.is_object() || fx.value("type", std::string("builtin")) != "builtin") continue;
      const EffectInfo* info = findEffect(effectName(fx));
      if (!info) continue;
      const int version = fx.contains("version") && fx["version"].is_number_integer() ? fx["version"].get<int>() : 1;
      if (version >= 1 && version < info->version) fx["version"] = info->version;
    }
  };
  for (auto& t : d.tracks) {
    chain(t.effects);
    if (t.instrument.kind == "builtin" && t.instrument.name == "sampler" && t.instrument.version < kSamplerVersion) t.instrument.version = kSamplerVersion;
  }
  chain(d.masterEffects);
}

std::string effectName(const json& fx) { return fx.is_object() && fx.contains("name") && fx["name"].is_string() ? fx["name"].get<std::string>() : std::string(); }
bool effectBypassed(const json& fx) { return fx.is_object() && fx.contains("bypass") && fx["bypass"].is_boolean() && fx["bypass"].get<bool>(); }
double effectParam(const json& fx, const std::string& param, double fallback) {
  if (!fx.is_object() || !fx.contains("params") || !fx["params"].is_object()) return fallback;
  const json& p = fx["params"];
  return p.contains(param) && p[param].is_number() ? p[param].get<double>() : fallback;
}

json& effectChain(Document& doc, const std::string& trackId) { return trackId.empty() ? doc.masterEffects : trackById(doc, trackId).effects; }

const json& effectChain(const Document& doc, const std::string& trackId) {
  if (trackId.empty()) return doc.masterEffects;
  const Track* t = doc.findTrack(trackId);
  if (!t) throw DocumentError("tracks", "no track '" + trackId + "'");
  return t->effects;
}

std::vector<EffectMenuEntry> effectMenuEntries(const json& chain) {
  std::vector<EffectMenuEntry> out;
  for (int i = 0; i < (int)chain.size() && i < kMaxEffectsPerChain; ++i) {
    const json& fx = chain[(size_t)i];
    if (!fx.is_object()) continue;
    const bool plugin = fx.value("type", std::string()) == "plugin" && fx.contains("plugin");
    std::string label;
    if (plugin) label = fx["plugin"].value("name", std::string("Plugin"));
    else {
      const EffectInfo* info = findEffect(effectName(fx));
      label = info ? std::string(info->label) : effectName(fx);
    }
    if (label.empty()) label = "Effect";
    out.push_back({i, plugin, plugin, std::move(label)});
  }
  return out;
}

namespace {
json& effectAt(Document& doc, const std::string& trackId, int index) {
  json& chain = effectChain(doc, trackId);
  if (index < 0 || index >= (int)chain.size()) throw DocumentError("effects", "no effect at " + std::to_string(index));
  return chain[(size_t)index];
}
}  // namespace

namespace {
// A new effect id in a chain: fx1, fx2... (never reused while the others are there).
std::string nextEffectId(const json& chain) {
  long highest = 0;
  for (const auto& fx : chain)
    if (fx.is_object() && fx.contains("id") && fx["id"].is_string() && fx["id"].get<std::string>().rfind("fx", 0) == 0)
      highest = std::max(highest, idNumber(fx["id"].get<std::string>().c_str() + 2));
  return "fx" + std::to_string(highest + 1);
}

// What a document keeps of a scanned plugin (the state is added when the engine gives it).
json pluginRef(const json& plugin) {
  if (!plugin.is_object() || !plugin.contains("id") || !plugin["id"].is_string() || !plugin.contains("file"))
    throw DocumentError("plugin", "a plugin needs its id and its file");
  std::string format = plugin.value("format", std::string("vst3"));
  for (auto& c : format) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  json ref = {{"format", format}, {"id", plugin["id"]}, {"name", plugin.value("name", "Plugin")},
              {"vendor", plugin.value("vendor", "")}, {"version", plugin.value("version", "")}, {"file", plugin["file"]}};
  if (plugin.contains("state") && plugin["state"].is_string()) ref["state"] = plugin["state"];
  return ref;
}

}  // namespace

std::string addPluginTrack(Document& doc, const json& plugin) {
  const json ref = pluginRef(plugin);
  if (doc.tracks.size() >= 128) throw DocumentError("tracks", "has the maximum of 128 tracks");
  Track t;
  t.id = uniqueTrackId(doc, "vst");
  t.name = ref["name"].get<std::string>().substr(0, 80);
  t.instrument.kind = "plugin";
  t.instrument.name.clear();
  t.instrument.plugin = ref;
  Pattern p;  // one 1-bar pattern over the whole song, as for the other instruments
  p.id = "p1";
  p.name = "1";
  p.lengthTicks = 4 * kPpq;
  t.patterns.push_back(p);
  t.clips.push_back({"c1", "p1", 0, doc.lengthTicks, 0, json::object()});
  doc.tracks.push_back(std::move(t));
  return doc.tracks.back().id;
}

int addPluginEffect(Document& doc, const std::string& trackId, const json& plugin) {
  const json ref = pluginRef(plugin);
  json& chain = effectChain(doc, trackId);
  if ((int)chain.size() >= kMaxEffectsPerChain) throw DocumentError("effects", "the chain has the maximum of 16 effects");
  chain.push_back({{"id", nextEffectId(chain)}, {"type", "plugin"}, {"plugin", ref}, {"bypass", false}});
  return (int)chain.size() - 1;
}

void replaceInstrumentWithPlugin(Document& doc, const std::string& trackId, const json& plugin) {
  Track& t = trackById(doc, trackId);
  if (!playsPatterns(t)) throw DocumentError("instrument", "an audio track or a bus has no instrument");
  Instrument next;
  next.kind = "plugin";
  next.plugin = pluginRef(plugin);
  if (t.instrument.kind == "plugin" && t.instrument.plugin.value("id", std::string()) == next.plugin.value("id", std::string()) &&
      t.instrument.plugin.value("file", std::string()) == next.plugin.value("file", std::string()))
    return;  // the same plugin: its state and automation stay as they are
  dropInstrumentMoods(doc, trackId);
  trackById(doc, trackId).instrument = next;
  Track& t2 = trackById(doc, trackId);
  // The old instrument's own curves and movements go; the mixer's and the effects' stay.
  auto own = [](const std::string& p) { return p != "gainDb" && p != "pan" && p.rfind("fx:", 0) != 0; };
  t2.automation.erase(std::remove_if(t2.automation.begin(), t2.automation.end(), [&](const Automation& a) { return own(a.param); }), t2.automation.end());
  t2.modulators.erase(std::remove_if(t2.modulators.begin(), t2.modulators.end(), [&](const json& m) { return own(m.value("param", "")); }), t2.modulators.end());
}

std::string pluginKey(const std::string& trackId, const std::string& effectId) {
  if (effectId.empty()) return "track:" + trackId;
  return "fx:" + (trackId.empty() ? std::string("master") : trackId) + ":" + effectId;
}

bool hasPlugins(const Document& doc) {
  auto chainHas = [](const json& chain) {
    for (const auto& fx : chain) if (fx.is_object() && fx.value("type", "") == "plugin") return true;
    return false;
  };
  if (chainHas(doc.masterEffects)) return true;
  for (const auto& t : doc.tracks) if (t.instrument.kind == "plugin" || chainHas(t.effects)) return true;
  return false;
}

int applyPluginStates(Document& doc, const json& states) {
  if (!states.is_object()) return 0;
  int changed = 0;
  auto set = [&](json& plugin, const std::string& key) {
    if (!states.contains(key) || !states[key].is_string() || !plugin.is_object()) return;
    if (plugin.value("state", "") == states[key].get<std::string>()) return;
    plugin["state"] = states[key];
    ++changed;
  };
  auto chain = [&](json& list, const std::string& trackId) {
    for (auto& fx : list)
      if (fx.is_object() && fx.value("type", "") == "plugin" && fx.contains("id") && fx["id"].is_string() && fx.contains("plugin"))
        set(fx["plugin"], pluginKey(trackId, fx["id"].get<std::string>()));
  };
  chain(doc.masterEffects, "");
  for (auto& t : doc.tracks) {
    if (t.instrument.kind == "plugin") set(t.instrument.plugin, pluginKey(t.id));
    chain(t.effects, t.id);
  }
  return changed;
}

int addEffect(Document& doc, const std::string& trackId, const std::string& name) {
  const EffectInfo* info = findEffect(name);
  if (!info) throw DocumentError("effects", "unknown effect '" + name + "'");
  json& chain = effectChain(doc, trackId);
  if ((int)chain.size() >= kMaxEffectsPerChain) throw DocumentError("effects", "the chain has the maximum of 16 effects");
  json params = json::object();
  for (const auto& p : info->params) params[p.name] = p.def;
  chain.push_back({{"id", nextEffectId(chain)}, {"type", "builtin"}, {"name", name}, {"version", info->version}, {"params", params}, {"bypass", false}});
  return (int)chain.size() - 1;
}

int addEffectWith(Document& doc, const std::string& trackId, const std::string& name, const json& params) {
  const int at = addEffect(doc, trackId, name);
  if (params.is_object())
    for (const auto& [k, v] : params.items()) if (v.is_number()) setEffectParam(doc, trackId, at, k, v.get<double>());
  return at;
}

int songRoot(const Document& doc) {
  // The bass tracks say the key; without one, every melodic track (drums never do).
  auto lower = [](std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  auto isDrums = [](const Track& t) {
    const auto& n = t.instrument.name;
    return t.instrument.kind == "builtin" && (n == "kick" || n == "hats" || n == "snare" || n == "drums");
  };
  auto isBass = [&](const Track& t) {
    return lower(t.name).find("bass") != std::string::npos;
  };
  for (int pass = 0; pass < 2; ++pass) {
    double weight[12] = {};
    double total = 0;
    for (const auto& t : doc.tracks) {
      if (isDrums(t) || t.instrument.kind == "audio" || t.instrument.kind == "bus") continue;
      if (pass == 0 && !isBass(t)) continue;
      for (const auto& p : t.patterns) {
        const int uses = std::max(1, t.clipsUsing(p.id));
        for (const auto& n : p.notes) {
          const double w = (double)std::max<int64_t>(1, n.length) * uses;
          weight[((n.pitch % 12) + 12) % 12] += w;
          total += w;
        }
      }
    }
    if (total > 0) return (int)(std::max_element(weight, weight + 12) - weight);
  }
  return -1;
}

int followSongKey(const Document& doc, int note) {
  const int root = songRoot(doc);
  if (root < 0) return note;
  int d = (root - note % 12 + 12) % 12;  // up to the root...
  if (d > 6) d -= 12;                      // ...or down, whichever is nearer
  return note + d;
}

namespace {
void presetValues(const Document& doc, const EffectPreset& preset, json& params) {
  const EffectInfo* info = findEffect(preset.effect);
  if (!info) return;
  for (const auto& [name, value] : preset.values)
    for (const auto& p : info->params)
      if (std::string(p.name) == name) {
        double v = value;
        // METAL rings at the song's key: the preset's octave, the note of the bass.
        if (std::string(preset.effect) == "metal" && name == std::string("note")) v = followSongKey(doc, (int)std::lround(v));
        params[p.name] = std::clamp(v, p.min, p.max);
      }
}
}  // namespace

int addEffect(Document& doc, const std::string& trackId, const EffectPreset& preset) {
  const int at = addEffect(doc, trackId, preset.effect);
  presetValues(doc, preset, effectChain(doc, trackId)[(size_t)at]["params"]);
  return at;
}

int addPhraseEndComb(Document& doc, const std::string& trackId, int phraseBars) {
  if (phraseBars < 1 || phraseBars > 64) throw DocumentError("comb", "a phrase is 1-64 bars");
  const int at = addEffect(doc, trackId, "filter");
  setEffectParam(doc, trackId, at, "mode", 8);         // Comb
  setEffectParam(doc, trackId, at, "resonance", 8.5);  // a long, ringing comb
  setEffectParam(doc, trackId, at, "cutoff", 1200);
  setEffectParam(doc, trackId, at, "mix", 0);
  const std::string id = effectChain(doc, trackId)[(size_t)at].value("id", std::string());
  const std::string mixKey = effectParamKey(id, "mix"), cutKey = effectParamKey(id, "cutoff");
  const int64_t bar = 4 * kPpq, phrase = (int64_t)phraseBars * bar;
  setAutomationPoint(doc, trackId, mixKey, 0, 0);
  setAutomationPoint(doc, trackId, cutKey, 0, 1200);
  for (int64_t end = phrase; end <= doc.lengthTicks; end += phrase) {
    const int64_t from = end - bar;
    setAutomationPoint(doc, trackId, mixKey, from, 0);
    setAutomationPoint(doc, trackId, mixKey, from + bar / 4, 0.7);
    setAutomationPoint(doc, trackId, mixKey, end - 1, 0.7);
    setAutomationPoint(doc, trackId, mixKey, end, 0);
    setAutomationPoint(doc, trackId, cutKey, from, 1200);
    setAutomationPoint(doc, trackId, cutKey, end - 1, 180);
    setAutomationPoint(doc, trackId, cutKey, end, 1200);
  }
  return at;
}

void applyEffectPreset(Document& doc, const std::string& trackId, int index, const EffectPreset& preset) {
  json& chain = effectChain(doc, trackId);
  if (index < 0 || index >= (int)chain.size()) throw DocumentError("effects", "has no effect at " + std::to_string(index));
  json& fx = chain[(size_t)index];
  if (effectName(fx) != preset.effect) throw DocumentError("effects", "the effect there is not " + std::string(preset.effect));
  // A preset is a whole sound: the knobs it does not list go back to their default, so choosing another preset never
  // keeps part of the previous one (Diogo, 2026-11-06: "everything I change stacks on what was already changed").
  fx["params"] = json::object();
  presetValues(doc, preset, fx["params"]);
}


namespace {
std::vector<Automation>& automationOf(Document& doc, const std::string& trackId) {
  return trackId.empty() ? doc.masterAutomation : trackById(doc, trackId).automation;
}
}  // namespace

void dropInstrumentMoods(Document& doc, const std::string& trackId) {
  // Audit 2026-11-07 (8): a mood's effects go with the instrument that put them there.
  static const char* keys[] = {"wcoreMood"};
  Track& t = trackById(doc, trackId);
  for (int i = (int)t.effects.size() - 1; i >= 0; --i) {
    const json& fx = trackById(doc, trackId).effects[(size_t)i];
    if (fx.is_object() && std::any_of(std::begin(keys), std::end(keys), [&](const char* k) { return fx.contains(k); }))
      removeEffect(doc, trackId, i);
  }
  Track& tt = trackById(doc, trackId);
  if (tt.extra.is_object())
    for (const char* k : keys) tt.extra.erase(std::string(k) + "Knobs");
}

// Core's own instruments left Core (2026-11-16): the track keeps its notes and waits for a plugin. uiltin is only a
// name kept for older callers; the old instrument's own settings, curves and movements go.
void replaceInstrument(Document& doc, const std::string& trackId, const std::string&) {
  Track& t = trackById(doc, trackId);
  if (!playsPatterns(t)) throw DocumentError("instrument", "an audio track or a bus has no instrument");
  if (t.instrument.kind == "none") return;
  dropInstrumentMoods(doc, trackId);
  Track& t2 = trackById(doc, trackId);
  t2.instrument = Instrument{};
  t2.instrument.kind = "none";
  auto own = [](const std::string& p) { return p != "gainDb" && p != "pan" && p.rfind("fx:", 0) != 0; };
  t2.automation.erase(std::remove_if(t2.automation.begin(), t2.automation.end(), [&](const Automation& a) { return own(a.param); }), t2.automation.end());
  t2.modulators.erase(std::remove_if(t2.modulators.begin(), t2.modulators.end(), [&](const json& m) { return own(m.value("param", "")); }), t2.modulators.end());
}

void removeEffect(Document& doc, const std::string& trackId, int index) {
  const json fx = effectAt(doc, trackId, index);
  json& chain = effectChain(doc, trackId);
  chain.erase(chain.begin() + index);
  // Its automation goes with it.
  if (fx.contains("id") && fx["id"].is_string()) {
    const std::string prefix = "fx:" + fx["id"].get<std::string>() + ":";
    auto& list = automationOf(doc, trackId);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Automation& a) { return a.param.rfind(prefix, 0) == 0; }), list.end());
    if (!trackId.empty()) {
      auto& mods = trackById(doc, trackId).modulators;
      mods.erase(std::remove_if(mods.begin(), mods.end(), [&](const json& m) {
        return m.value("param", "").rfind(prefix, 0) == 0;
      }), mods.end());
    }
  }
}

void moveEffect(Document& doc, const std::string& trackId, int from, int to) {
  json moved = effectAt(doc, trackId, from);
  json& chain = effectChain(doc, trackId);
  to = std::clamp(to, 0, (int)chain.size() - 1);
  chain.erase(chain.begin() + from);
  chain.insert(chain.begin() + to, moved);
}

void setEffectParam(Document& doc, const std::string& trackId, int index, const std::string& param, double value) {
  json& fx = effectAt(doc, trackId, index);
  const EffectInfo* info = findEffect(effectName(fx));
  if (!info) throw DocumentError("effects", "not a built-in effect");
  for (const auto& p : info->params)
    if (param == p.name) {
      double v = std::clamp(value, p.min, p.max);
      if (p.step > 0) v = std::round(v / p.step) * p.step;
      if (!fx.contains("params") || !fx["params"].is_object()) fx["params"] = json::object();
      fx["params"][param] = v;
      return;
    }
  throw DocumentError("effects", "unknown parameter '" + param + "'");
}

void setEffectBypass(Document& doc, const std::string& trackId, int index, bool bypass) { effectAt(doc, trackId, index)["bypass"] = bypass; }

void setEffectSidechain(Document& doc, const std::string& trackId, int index, const std::string& sourceTrackId) {
  json& fx = effectAt(doc, trackId, index);
  const json before = fx;
  if (sourceTrackId.empty()) fx.erase("sidechain");
  else fx["sidechain"] = sourceTrackId;
  try { checkRouting(doc); } catch (...) { fx = before; throw; }
}

// ---- automation

namespace {
const std::vector<Automation>* automationOf(const Document& doc, const std::string& trackId) {
  if (trackId.empty()) return &doc.masterAutomation;
  const Track* t = doc.findTrack(trackId);
  return t ? &t->automation : nullptr;
}
// The plugin a 'vst:' parameter belongs to (the track's instrument, or a plugin effect of the chain),
// or null: its name for the window, and whether the parameter can be automated there at all.
const json* pluginOfParam(const Document& doc, const std::string& trackId, const std::string& param) {
  const Track* t = trackId.empty() ? nullptr : doc.findTrack(trackId);
  if (!trackId.empty() && !t) return nullptr;
  if (t && param.rfind("vst:", 0) == 0 && param.size() > 4 && t->instrument.kind == "plugin") return &t->instrument.plugin;
  if (param.rfind("fx:", 0) != 0) return nullptr;
  const size_t colon = param.find(':', 3);
  if (colon == std::string::npos || param.compare(colon + 1, 4, "vst:") != 0 || param.size() <= colon + 5) return nullptr;
  const std::string id = param.substr(3, colon - 3);
  for (const auto& fx : t ? t->effects : doc.masterEffects)
    if (fx.value("type", std::string()) == "plugin" && fx.value("id", std::string()) == id && fx.contains("plugin")) return &fx["plugin"];
  return nullptr;
}
AutomatableParam paramInfo(const Document& doc, const std::string& trackId, const std::string& param) {
  for (const auto& p : automatableParams(doc, trackId)) if (p.param == param) return p;
  if (pluginOfParam(doc, trackId, param)) return {param, param, 0, 1, 0.5, 0.5, "", false};  // a plugin parameter, 0..1
  throw DocumentError("automation", "'" + param + "' cannot be automated here");
}
Automation& automationAt(Document& doc, const std::string& trackId, const std::string& param) {
  for (auto& a : automationOf(doc, trackId)) if (a.param == param) return a;
  throw DocumentError("automation", "no automation of '" + param + "'");
}
}  // namespace

std::vector<AutomatableParam> automatableParams(const Document& doc, const std::string& trackId) {
  std::vector<AutomatableParam> out;
  const Track* t = trackId.empty() ? nullptr : doc.findTrack(trackId);
  if (!trackId.empty() && !t) return out;
  out.push_back({"gainDb", t ? "Volume" : "Master volume", -60, 12, 0, t ? t->mix.gainDb : doc.masterGainDb, "dB", false});
  if (t) out.push_back({"pan", "Pan", -1, 1, 0, t->mix.pan, "", false});
  // The built-in instrument's own parameters, with the engine's ranges (a new curve can start from nothing).
  if (t && t->instrument.kind == "builtin")
    if (const InstrumentInfo* info = findInstrumentInfo(t->instrument.name))
      for (const auto& p : info->params)
        if (p.automatable)
          out.push_back({p.name, std::string(info->label) + " · " + p.label, p.min, p.max, p.def, instrumentParamValue(*t, p), p.unit, p.logScale});
  const json& chain = t ? t->effects : doc.masterEffects;
  for (const auto& fx : chain) {
    const EffectInfo* info = findEffect(effectName(fx));
    if (!info || !fx.contains("id") || !fx["id"].is_string()) continue;
    const std::string id = fx["id"].get<std::string>();
    for (const auto& p : info->params) {
      // Choices and times that would jump are left out; a Core FX module's On is a switch that fades.
      const std::string n = p.name;
      const bool fadingSwitch = info->name == std::string("corefx") && n.size() > 2 && n.compare(n.size() - 2, 2, "On") == 0;
      if ((p.step > 0 && !fadingSwitch) || !p.automatable) continue;
      out.push_back({effectParamKey(id, p.name), std::string(info->label) + " · " + p.label, p.min, p.max, p.def,
                     effectParam(fx, p.name, p.def), p.unit, p.logScale});
    }
  }
  // Automation already there that the list above does not name (an instrument's parameter from an
  // imported song): shown with the range of its own points, so it can be seen, edited and cleared.
  for (const auto& a : t ? t->automation : doc.masterAutomation) {
    if (std::any_of(out.begin(), out.end(), [&](const AutomatableParam& p) { return p.param == a.param; }) || a.points.empty()) continue;
    if (const json* plugin = pluginOfParam(doc, trackId, a.param)) {  // its name comes from the engine (the window asks)
      out.push_back({a.param, plugin->value("name", std::string("Plugin")) + " · " + a.param, 0, 1, 0.5, a.points[0].value, "", false});
      continue;
    }
    double lo = a.points[0].value, hi = lo;
    for (const auto& p : a.points) { lo = std::min(lo, p.value); hi = std::max(hi, p.value); }
    if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
    out.push_back({a.param, a.param + " (instrument)", lo, hi, lo, a.points[0].value, "", false});
  }
  return out;
}

std::string effectParamKey(const std::string& effectId, const std::string& param) { return "fx:" + effectId + ":" + param; }

const Automation* findAutomation(const Document& doc, const std::string& trackId, const std::string& param) {
  if (const auto* list = automationOf(doc, trackId))
    for (const auto& a : *list) if (a.param == param) return &a;
  return nullptr;
}

double automationValueAt(const Automation& a, int64_t tick) {
  if (a.points.empty()) return 0;
  if (tick <= a.points.front().tick) return a.points.front().value;
  if (tick >= a.points.back().tick) return a.points.back().value;
  auto it = std::upper_bound(a.points.begin(), a.points.end(), tick, [](int64_t v, const AutomationPoint& p) { return v < p.tick; });
  const AutomationPoint& b = *it;
  const AutomationPoint& prev = *(it - 1);
  return prev.value + (b.value - prev.value) * double(tick - prev.tick) / double(std::max<int64_t>(1, b.tick - prev.tick));
}

int setAutomationPoint(Document& doc, const std::string& trackId, const std::string& param, int64_t tick, double value) {
  const AutomatableParam info = paramInfo(doc, trackId, param);
  auto& list = automationOf(doc, trackId);
  Automation* a = nullptr;
  for (auto& x : list) if (x.param == param) { a = &x; break; }  // the first, as findAutomation
  if (!a) {
    if (list.size() >= 256) throw DocumentError("automation", "the maximum of 256 automations is reached");
    list.push_back(Automation{param, {}, json::object()});
    a = &list.back();
  }
  if (a->points.size() >= 100000) throw DocumentError("automation", "too many points");
  tick = std::clamp<int64_t>(tick, 0, doc.lengthTicks);
  const AutomationPoint p{tick, std::clamp(value, info.min, info.max), json::object()};
  auto it = std::lower_bound(a->points.begin(), a->points.end(), tick, [](const AutomationPoint& x, int64_t v) { return x.tick < v; });
  if (it != a->points.end() && it->tick == tick) { it->value = p.value; return (int)(it - a->points.begin()); }
  // Two statements: insert may reallocate, so begin() must be read after it.
  const auto inserted = a->points.insert(it, p);
  return (int)(inserted - a->points.begin());
}

int moveAutomationPoint(Document& doc, const std::string& trackId, const std::string& param, int index, int64_t tick, double value) {
  const AutomatableParam info = paramInfo(doc, trackId, param);
  Automation& a = automationAt(doc, trackId, param);
  if (index < 0 || index >= (int)a.points.size()) throw DocumentError("automation", "no point " + std::to_string(index));
  const int64_t lo = index > 0 ? a.points[(size_t)index - 1].tick + 1 : 0;
  const int64_t hi = index + 1 < (int)a.points.size() ? a.points[(size_t)index + 1].tick - 1 : doc.lengthTicks;
  if (lo <= hi) a.points[(size_t)index].tick = std::clamp(tick, lo, hi);  // between its neighbours (no room: it keeps its tick)
  a.points[(size_t)index].value = std::clamp(value, info.min, info.max);
  return index;
}

void removeAutomationPoint(Document& doc, const std::string& trackId, const std::string& param, int index) {
  Automation& a = automationAt(doc, trackId, param);
  if (index < 0 || index >= (int)a.points.size()) throw DocumentError("automation", "no point " + std::to_string(index));
  a.points.erase(a.points.begin() + index);
  if (a.points.empty()) clearAutomation(doc, trackId, param);
}

void clearAutomation(Document& doc, const std::string& trackId, const std::string& param) {
  auto& list = automationOf(doc, trackId);
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Automation& a) { return a.param == param; }), list.end());
}

void setAutomationInRange(Document& doc, const std::string& trackId, const std::string& param, int64_t startTick, int64_t endTick,
                          const std::vector<AutomationPoint>& points, double heldValue) {
  const AutomatableParam info = paramInfo(doc, trackId, param);
  if (startTick < 0 || endTick <= startTick || endTick > doc.lengthTicks) throw DocumentError("automation", "the range must be inside the song and end after it starts");
  for (const auto& p : points)
    if (p.tick < startTick || p.tick >= endTick) throw DocumentError("automation", "a point at tick " + std::to_string(p.tick) + " is outside the range");
  const Automation* old = findAutomation(doc, trackId, param);
  auto valueAt = [&](int64_t tick) { return old && !old->points.empty() ? automationValueAt(*old, tick) : std::clamp(heldValue, info.min, info.max); };
  std::vector<AutomationPoint> kept;
  if (old) for (const auto& p : old->points) if (p.tick < startTick || p.tick > endTick) kept.push_back(p);
  std::vector<AutomationPoint> result;
  for (const auto& p : kept) if (p.tick < startTick) result.push_back(p);
  // Left edge: the old value right where the region begins (one tick earlier when a new point sits on the start).
  const bool pointOnStart = std::any_of(points.begin(), points.end(), [&](const AutomationPoint& p) { return p.tick == startTick; });
  const int64_t leftEdge = pointOnStart ? startTick - 1 : startTick;
  if (leftEdge >= 0 && (result.empty() || result.back().tick < leftEdge)) result.push_back({leftEdge, valueAt(leftEdge), json::object()});
  for (auto p : points) { p.value = std::clamp(p.value, info.min, info.max); p.extra = json::object(); result.push_back(p); }
  std::stable_sort(result.begin() + (long)(result.size() - points.size()), result.end(),
                   [](const AutomationPoint& x, const AutomationPoint& y) { return x.tick < y.tick; });
  // Right edge: the old value at the end of the region, from where the curve goes on as before.
  result.push_back({endTick, valueAt(endTick), json::object()});
  for (const auto& p : kept) if (p.tick > endTick) result.push_back(p);
  for (size_t i = 1; i < result.size(); ++i)
    if (result[i].tick == result[i - 1].tick) throw DocumentError("automation", "two points at tick " + std::to_string(result[i].tick));
  if (result.size() > 100000) throw DocumentError("automation", "too many points");
  auto& list = automationOf(doc, trackId);
  for (auto& a : list)
    if (a.param == param) { a.points = std::move(result); return; }
  if (list.size() >= 256) throw DocumentError("automation", "the maximum of 256 automations is reached");
  list.push_back(Automation{param, std::move(result), json::object()});
}

// ---- audio

namespace {

AudioClip& audioClipById(Track& t, const std::string& id) {
  for (auto& c : t.audioClips) if (c.id == id) return c;
  throw DocumentError("audioClips", "no audio clip '" + id + "'");
}
}  // namespace

double secondsPerTick(const Document& doc) { return 60.0 / (doc.bpm * kPpq); }

// The same sums as the engine's tempo map (core/tempo_map.cpp): steps are tick * seconds-per-tick, a ramp
// (the BPM in a straight line from the previous point to a point with ramp) is integrated in closed form.
namespace {
constexpr double kSecondsAtOneBpm = 60.0 / kPpq;  // seconds per tick at 1 BPM
// Seconds over d ticks of a segment that starts at bpm and changes by slope BPM per tick.
double segmentSeconds(double bpm, double slope, double d) {
  if (slope == 0 || d <= 0) return d * kSecondsAtOneBpm / bpm;
  return (kSecondsAtOneBpm / slope) * std::log1p(slope * d / bpm);
}
double segmentTicks(double bpm, double slope, double seconds) {  // the inverse
  if (slope == 0 || seconds <= 0) return seconds * bpm / kSecondsAtOneBpm;
  return (bpm / slope) * std::expm1(slope * seconds / kSecondsAtOneBpm);
}
// The slope (BPM per tick) of the segment from (fromTick, fromBpm) to point `next`.
double slopeTo(double fromTick, double fromBpm, const TempoPoint* next) {
  if (!next || !next->ramp || next->bpm == fromBpm) return 0;
  return (next->bpm - fromBpm) / ((double)next->tick - fromTick);
}
}  // namespace

double secondsAt(const Document& doc, double tick) {
  double from = 0, at = 0, bpm = doc.bpm;
  for (size_t i = 0;; ++i) {
    const TempoPoint* next = i < doc.tempoMap.size() ? &doc.tempoMap[i] : nullptr;
    const double slope = slopeTo(from, bpm, next);
    if (!next || next->tick > tick) return at + segmentSeconds(bpm, slope, tick - from);
    at += segmentSeconds(bpm, slope, (double)next->tick - from);
    from = (double)next->tick;
    bpm = next->bpm;
  }
}

double tickAtSeconds(const Document& doc, double seconds) {
  double from = 0, at = 0, bpm = doc.bpm;
  for (size_t i = 0;; ++i) {
    const TempoPoint* next = i < doc.tempoMap.size() ? &doc.tempoMap[i] : nullptr;
    const double slope = slopeTo(from, bpm, next);
    if (next) {
      const double end = at + segmentSeconds(bpm, slope, (double)next->tick - from);
      if (end <= seconds) { at = end; from = (double)next->tick; bpm = next->bpm; continue; }
    }
    return from + segmentTicks(bpm, slope, seconds - at);
  }
}

double secondsBetween(const Document& doc, double fromTick, double toTick) {
  if (doc.tempoMap.empty()) return (toTick - fromTick) * secondsPerTick(doc);
  return secondsAt(doc, toTick) - secondsAt(doc, fromTick);
}

std::string addAudioTrack(Document& doc, const std::string& name) {
  if (doc.tracks.size() >= 128) throw DocumentError("tracks", "has the maximum of 128 tracks");
  Track t;
  t.id = uniqueTrackId(doc, name.empty() ? "audio" : name);
  t.name = name.empty() ? "Audio" : cutUtf8(name, 80);
  t.instrument.kind = "audio";
  t.instrument.name.clear();
  doc.tracks.push_back(std::move(t));
  return doc.tracks.back().id;
}

std::string addAudioClip(Document& doc, const std::string& trackId, const std::string& assetId, int64_t startTick) {
  const Asset* asset = doc.findAsset(assetId);
  if (!asset) throw DocumentError("assetId", "no asset '" + assetId + "'");
  Track& t = trackById(doc, trackId);
  if (t.instrument.kind != "audio") throw DocumentError("tracks", "audio clips go on an audio track");
  if (t.audioClips.size() >= 1024) throw DocumentError("audioClips", "the track has the maximum of 1024 audio clips");
  AudioClip c;
  c.id = nextId(t.audioClips, "x");
  c.assetId = assetId;
  c.startTick = std::max<int64_t>(0, startTick);
  c.durationSeconds = std::max(kMinClipSeconds, asset->seconds());
  t.audioClips.push_back(c);
  return c.id;
}

void moveAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t startTick) {
  audioClipById(trackById(doc, trackId), clipId).startTick = std::max<int64_t>(0, startTick);
}

void setSampleLoop(Document& doc, const std::string& trackId, size_t zone, double loopStart, double loopEnd, double fileSeconds) {
  Track& t = trackById(doc, trackId);
  if (t.instrument.kind != "builtin" || t.instrument.name != "sampler") throw DocumentError("track", "is not a sampler");
  if (zone >= t.instrument.samples.size()) throw DocumentError("zone", "no such zone");
  if (!std::isfinite(loopStart) || !std::isfinite(loopEnd) || loopStart < 0 || loopEnd < 0) throw DocumentError("loop", "must be seconds, 0 or more");
  SampleRef& z = t.instrument.samples[zone];
  if (loopEnd <= 0) { z.loopStart = z.loopEnd = 0; return; }
  if (fileSeconds > 0 && loopEnd > fileSeconds + 1e-6) throw DocumentError("loopEnd", "is past the end of the sample");
  if (loopEnd < loopStart + kMinSampleLoopSeconds) throw DocumentError("loopEnd", "must be at least 2 ms after loopStart");
  z.loopStart = loopStart;
  z.loopEnd = loopEnd;
}

void trimAudioClipStart(Document& doc, const std::string& trackId, const std::string& clipId, int64_t startTick) {
  AudioClip& c = audioClipById(trackById(doc, trackId), clipId);
  // Song seconds of a tick: tick * spt with one tempo (as before the map), the map otherwise.
  const double spt = secondsPerTick(doc);
  auto sec = [&](double tick) { return doc.tempoMap.empty() ? tick * spt : secondsAt(doc, tick); };
  const double end = sec((double)c.startTick) + c.durationSeconds;   // where it ends, in song seconds
  const double fileStart = sec((double)c.startTick) - c.offsetSeconds;  // where the file's first sample sits
  const double firstTick = doc.tempoMap.empty() ? fileStart / spt : tickAtSeconds(doc, fileStart);
  int64_t s = std::max<int64_t>({0, startTick, (int64_t)std::ceil(firstTick - 1e-9)});  // not before the file
  while (s > 0 && end - sec((double)s) < kMinClipSeconds) --s;       // not past its own end (whole ticks)
  if (end - sec((double)s) < kMinClipSeconds) return;
  c.offsetSeconds = std::max(0.0, sec((double)s) - fileStart);
  c.startTick = s;
  c.durationSeconds = end - sec((double)s);
}

void trimAudioClipEnd(Document& doc, const std::string& trackId, const std::string& clipId, double durationSeconds) {
  AudioClip& c = audioClipById(trackById(doc, trackId), clipId);
  const Asset* asset = doc.findAsset(c.assetId);
  const double available = asset ? asset->seconds() - c.offsetSeconds : durationSeconds;
  c.durationSeconds = std::clamp(durationSeconds, kMinClipSeconds, std::max(kMinClipSeconds, available));
}

std::string splitAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t atTick) {
  Track& t = trackById(doc, trackId);
  AudioClip& c = audioClipById(t, clipId);
  const double cut = secondsBetween(doc, (double)c.startTick, (double)atTick);
  if (cut < kMinClipSeconds || c.durationSeconds - cut < kMinClipSeconds) throw DocumentError("audioClips", "the split point must be inside the clip");
  if (t.audioClips.size() >= 1024) throw DocumentError("audioClips", "the track has the maximum of 1024 audio clips");
  AudioClip second = c;
  second.id = nextId(t.audioClips, "x");
  second.startTick = atTick;
  second.offsetSeconds = c.offsetSeconds + cut;
  second.durationSeconds = c.durationSeconds - cut;
  second.fadeInSeconds = 0;
  second.fadeOutSeconds = std::min(second.fadeOutSeconds, second.durationSeconds);
  c.durationSeconds = cut;
  c.fadeOutSeconds = 0;
  c.fadeInSeconds = std::min(c.fadeInSeconds, c.durationSeconds);  // a fade longer than the piece ends with it
  t.audioClips.push_back(second);  // c may move: take the id from the copy
  return second.id;
}

std::string duplicateAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t startTick) {
  Track& t = trackById(doc, trackId);
  if (t.audioClips.size() >= 1024) throw DocumentError("audioClips", "the track has the maximum of 1024 audio clips");
  AudioClip copy = audioClipById(t, clipId);
  copy.id = nextId(t.audioClips, "x");
  copy.startTick = std::max<int64_t>(0, startTick);
  t.audioClips.push_back(copy);
  return copy.id;
}

std::string pasteAudioClip(Document& doc, const std::string& trackId, AudioClip copy, int64_t startTick) {
  Track& t = trackById(doc, trackId);
  if (t.instrument.kind != "audio") throw DocumentError("tracks", "audio clips go on an audio track");
  if (t.audioClips.size() >= 1024) throw DocumentError("audioClips", "the track has the maximum of 1024 audio clips");
  copy.id = nextId(t.audioClips, "x");
  copy.startTick = std::max<int64_t>(0, startTick);
  t.audioClips.push_back(copy);
  return copy.id;
}

std::string moveClipToTrack(Document& doc, const std::string& fromTrack, const std::string& clipId, const std::string& toTrack, int64_t startTick) {
  startTick = std::max<int64_t>(0, startTick);
  Track& from = trackById(doc, fromTrack);
  trackById(doc, toTrack);  // throws when it is not there
  for (const auto& c : from.audioClips) {
    if (c.id != clipId) continue;
    if (fromTrack == toTrack) { moveAudioClip(doc, fromTrack, clipId, startTick); return clipId; }
    Track& to = trackById(doc, toTrack);
    if (to.instrument.kind != "audio") throw DocumentError("tracks", "audio clips go on an audio track");
    if (to.audioClips.size() >= 1024) throw DocumentError("audioClips", "the track has the maximum of 1024 audio clips");
    AudioClip moved = c;
    moved.id = nextId(to.audioClips, "x");
    moved.startTick = startTick;
    to.audioClips.push_back(moved);
    removeAudioClip(doc, fromTrack, clipId);
    return moved.id;
  }
  const Clip clip = clipById(from, clipId);  // a copy: the tracks' vectors change below
  if (fromTrack == toTrack) { moveClip(from, clipId, startTick); return clipId; }
  Track& to = trackById(doc, toTrack);
  if (!playsPatterns(to)) throw DocumentError("tracks", "pattern clips go on an instrument track");
  if (to.clips.size() >= 256) throw DocumentError("clips", "the track has the maximum of 256 clips");
  if (to.patterns.size() >= 64) throw DocumentError("patterns", "the track has the maximum of 64 patterns");
  Pattern pattern = patternById(from, clip.patternId);
  pattern.id = nextId(to.patterns, "p");
  pattern.name = std::to_string(to.patterns.size() + 1);
  to.patterns.push_back(pattern);
  Clip moved = clip;
  moved.id = nextId(to.clips, "c");
  moved.patternId = pattern.id;
  moved.startTick = startTick;
  to.clips.push_back(moved);
  sortClips(to);
  removeClip(trackById(doc, fromTrack), clipId);
  return moved.id;
}

void removeAudioClip(Document& doc, const std::string& trackId, const std::string& clipId) {
  Track& t = trackById(doc, trackId);
  audioClipById(t, clipId);
  t.audioClips.erase(std::remove_if(t.audioClips.begin(), t.audioClips.end(), [&](const AudioClip& c) { return c.id == clipId; }), t.audioClips.end());
}

int64_t audioClipEndTick(const Document& doc, const AudioClip& c) {
  if (doc.tempoMap.empty()) return c.startTick + (int64_t)std::llround(c.durationSeconds / secondsPerTick(doc));
  return (int64_t)std::llround(tickAtSeconds(doc, secondsAt(doc, (double)c.startTick) + c.durationSeconds));
}

void cropAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t fromTick, int64_t toTick) {
  {
    const AudioClip& c = audioClipById(trackById(doc, trackId), clipId);
    fromTick = std::max(fromTick, c.startTick);  // only ever shorter: trimmed-away audio stays away
    toTick = std::min(toTick, audioClipEndTick(doc, c));
  }
  if (toTick <= fromTick) throw DocumentError("audioClips", "the selection is empty");
  const int64_t oldStart = audioClipById(trackById(doc, trackId), clipId).startTick;
  const int64_t oldEnd = audioClipEndTick(doc, audioClipById(trackById(doc, trackId), clipId));
  trimAudioClipStart(doc, trackId, clipId, fromTick);
  AudioClip& c = audioClipById(trackById(doc, trackId), clipId);
  trimAudioClipEnd(doc, trackId, clipId, secondsBetween(doc, (double)c.startTick, (double)toTick));
  if (c.startTick != oldStart) c.fadeInSeconds = 0;  // a new cut: its old fade belonged to the old edge
  if (audioClipEndTick(doc, c) != oldEnd) c.fadeOutSeconds = 0;
  c.fadeInSeconds = std::min(c.fadeInSeconds, c.durationSeconds);
  c.fadeOutSeconds = std::min(c.fadeOutSeconds, c.durationSeconds);
}

int sliceAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t stepTicks) {
  if (stepTicks <= 0) throw DocumentError("audioClips", "the slice step must be positive");
  const AudioClip& c = audioClipById(trackById(doc, trackId), clipId);
  const int64_t start = c.startTick, end = audioClipEndTick(doc, c);
  std::vector<int64_t> cuts;
  for (int64_t t = (start / stepTicks + 1) * stepTicks; t < end; t += stepTicks) {
    const double cut = secondsBetween(doc, (double)start, (double)t);
    if (cut >= kMinClipSeconds && c.durationSeconds - cut >= kMinClipSeconds) cuts.push_back(t);
  }
  if (trackById(doc, trackId).audioClips.size() + cuts.size() > 1024) throw DocumentError("audioClips", "the track would pass 1024 audio clips");
  std::string piece = clipId;
  for (int64_t t : cuts) piece = splitAudioClip(doc, trackId, piece, t);  // each cut splits the last piece
  return (int)cuts.size() + 1;
}

void removeAudioRange(Document& doc, const std::string& trackId, const std::string& clipId, int64_t fromTick, int64_t toTick) {
  AudioClip& c = audioClipById(trackById(doc, trackId), clipId);
  const int64_t start = c.startTick, end = audioClipEndTick(doc, c);
  fromTick = std::max(fromTick, start);
  toTick = std::min(toTick, end);
  if (toTick <= fromTick) throw DocumentError("audioClips", "the selection is empty");
  const bool keepsStart = secondsBetween(doc, (double)start, (double)fromTick) >= kMinClipSeconds,
             keepsEnd = secondsBetween(doc, (double)toTick, (double)end) >= kMinClipSeconds;
  if (!keepsStart && !keepsEnd) { removeAudioClip(doc, trackId, clipId); return; }
  if (!keepsStart) {
    trimAudioClipStart(doc, trackId, clipId, toTick);
    AudioClip& k = audioClipById(trackById(doc, trackId), clipId);
    k.fadeInSeconds = 0;  // the start is a new cut
    k.fadeOutSeconds = std::min(k.fadeOutSeconds, k.durationSeconds);
    return;
  }
  if (!keepsEnd) {
    trimAudioClipEnd(doc, trackId, clipId, secondsBetween(doc, (double)start, (double)fromTick));
    AudioClip& k = audioClipById(trackById(doc, trackId), clipId);
    k.fadeOutSeconds = 0;  // the end is a new cut
    k.fadeInSeconds = std::min(k.fadeInSeconds, k.durationSeconds);
    return;
  }
  const std::string second = splitAudioClip(doc, trackId, clipId, fromTick);  // c may move: no use after this
  trimAudioClipStart(doc, trackId, second, toTick);
}

void setSongLength(Document& doc, int64_t lengthTicks) {
  if (lengthTicks < kPpq || lengthTicks > 4096LL * 4 * kPpq) throw DocumentError("lengthTicks", "invalid song length");
  doc.lengthTicks = lengthTicks;
  // Tempo changes after the new end go with it (they could never play).
  doc.tempoMap.erase(std::remove_if(doc.tempoMap.begin(), doc.tempoMap.end(), [&](const TempoPoint& t) { return t.tick >= lengthTicks; }), doc.tempoMap.end());
}

void setTempoMap(Document& doc, std::vector<TempoPoint> changes) {
  if (changes.size() > (size_t)kMaxTempoPoints) throw DocumentError("tempoMap", "at most " + std::to_string(kMaxTempoPoints) + " tempo changes");
  std::sort(changes.begin(), changes.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });
  for (size_t i = 0; i < changes.size(); ++i) {
    const std::string where = "tempoMap[" + std::to_string(i) + "]";
    if (changes[i].tick <= 0 || changes[i].tick >= doc.lengthTicks) throw DocumentError(where + ".tick", "must be after the start and inside the song");
    if (i && changes[i].tick == changes[i - 1].tick) throw DocumentError(where + ".tick", "two tempo changes on the same tick");
    if (!(changes[i].bpm >= 40 && changes[i].bpm <= 600)) throw DocumentError(where + ".bpm", "must be 40-600");
  }
  doc.tempoMap = std::move(changes);
}

double bpmAt(const Document& doc, int64_t tick) {
  double from = 0, bpm = doc.bpm;
  for (const auto& t : doc.tempoMap) {
    if (t.tick <= tick) { from = (double)t.tick; bpm = t.bpm; continue; }
    if (t.ramp) return bpm + (t.bpm - bpm) * ((double)tick - from) / ((double)t.tick - from);  // gliding to t
    break;
  }
  return bpm;
}

void removeTrack(Document& doc, const std::string& trackId) {
  const auto it = std::find_if(doc.tracks.begin(), doc.tracks.end(), [&](const Track& t) { return t.id == trackId; });
  if (it == doc.tracks.end()) throw DocumentError("tracks", "no track '" + trackId + "'");
  const std::string goesTo = it->mix.output;
  doc.tracks.erase(it);
  for (auto& t : doc.tracks) {  // what fed it goes where it went; sends to it end
    if (t.mix.output == trackId) t.mix.output = goesTo;
    t.mix.sends.erase(std::remove_if(t.mix.sends.begin(), t.mix.sends.end(), [&](const Send& s) { return s.to == trackId; }), t.mix.sends.end());
    // Removing a trigger removes its mapping, rather than silently triggering it from the
    // destination's own notes. The document snapshot used by Undo restores both together.
    t.modulators.erase(std::remove_if(t.modulators.begin(), t.modulators.end(), [&](const json& m) {
      return m.value("source", "") == trackId;
    }), t.modulators.end());
    for (auto& fx : t.effects)  // a compressor it keyed goes back to its own input
      if (fx.is_object() && fx.value("sidechain", "") == trackId) fx.erase("sidechain");
  }
}

std::string addBusTrack(Document& doc, const std::string& name) {
  if (doc.tracks.size() >= 128) throw DocumentError("tracks", "has the maximum of 128 tracks");
  Track t;
  t.id = uniqueTrackId(doc, name.empty() ? "bus" : name);
  t.name = name.empty() ? "Bus" : cutUtf8(name, 80);
  t.instrument.kind = "bus";
  t.instrument.name.clear();
  for (const char* c : kGroupColors) {  // a group colour no other bus of the song has (the first one if all are used)
    bool used = false;
    for (const Track& o : doc.tracks) used = used || (o.instrument.kind == "bus" && o.color == c);
    if (!used) { t.color = c; break; }
  }
  if (t.color.empty()) t.color = kGroupColors[0];
  doc.tracks.push_back(std::move(t));
  return doc.tracks.back().id;
}

namespace {
// Applies a routing change to a copy and keeps it only if the routing stays valid.
template <typename Change>
void reroute(Document& doc, const std::string& trackId, Change&& change) {
  Track& t = trackById(doc, trackId);
  const Mix before = t.mix;
  change(t.mix);
  try { checkRouting(doc); } catch (...) { t.mix = before; throw; }
}
Send& sendById(Track& t, const std::string& sendId) {
  for (auto& s : t.mix.sends) if (s.id == sendId) return s;
  throw DocumentError("sends", "no send '" + sendId + "' on track '" + t.id + "'");
}
}  // namespace

void setTrackOutput(Document& doc, const std::string& trackId, const std::string& busId) {
  reroute(doc, trackId, [&](Mix& m) { m.output = busId == "master" ? "" : busId; });
}

std::string addSend(Document& doc, const std::string& trackId, const std::string& busId, double gainDb, bool pre) {
  std::string id;
  reroute(doc, trackId, [&](Mix& m) {
    if (m.sends.size() >= (size_t)kMaxSends) throw DocumentError("sends", "the track has the maximum of " + std::to_string(kMaxSends) + " sends");
    for (int n = 1; id.empty(); ++n) {
      const std::string candidate = "s" + std::to_string(n);
      if (std::none_of(m.sends.begin(), m.sends.end(), [&](const Send& s) { return s.id == candidate; })) id = candidate;
    }
    m.sends.push_back(Send{id, busId, std::clamp(gainDb, -60.0, 12.0), pre, json::object()});
  });
  return id;
}

void setSend(Document& doc, const std::string& trackId, const std::string& sendId, double gainDb, bool pre) {
  Send& s = sendById(trackById(doc, trackId), sendId);
  s.gainDb = std::clamp(gainDb, -60.0, 12.0);
  s.pre = pre;
}

std::string setModulator(Document& doc, const std::string& trackId, const std::string& id, const std::string& param,
                         const std::string& shape, double rateBeats, double depth, double phase) {
  static const std::set<std::string> shapes = {"sine", "triangle", "square", "saw", "ramp", "random"};
  if (!shapes.count(shape)) throw DocumentError("shape", "must be sine, triangle, square, saw, ramp or random");
  if (param.empty()) throw DocumentError("param", "is required");
  Track& t = trackById(doc, trackId);
  json* found = nullptr;
  for (auto& m : t.modulators) if (m.value("id", "") == id) found = &m;
  std::string useId = id;
  if (!found) {
    if (t.modulators.size() >= (size_t)kMaxModulators) throw DocumentError("modulators", "the track has the maximum of " + std::to_string(kMaxModulators) + " modulators");
    for (int n = 1; useId.empty(); ++n) {
      const std::string candidate = "m" + std::to_string(n);
      if (std::none_of(t.modulators.begin(), t.modulators.end(), [&](const json& m) { return m.value("id", "") == candidate; })) useId = candidate;
    }
    t.modulators.push_back(json::object());
    found = &t.modulators.back();
  }
  const bool off = found->value("on", true) == false;  // a modulator turned off stays off
  *found = {{"id", useId}, {"param", param}, {"shape", shape}, {"rateBeats", std::clamp(rateBeats, 1.0 / 64, 64.0)},
            {"depth", depth}, {"phase", std::clamp(phase, 0.0, 1.0)}};
  if (off) (*found)["on"] = false;
  return useId;
}

void setModulatorOn(Document& doc, const std::string& trackId, const std::string& id, bool on) {
  for (auto& m : trackById(doc, trackId).modulators)
    if (m.value("id", "") == id) {
      if (on) m.erase("on"); else m["on"] = false;
      return;
    }
  throw DocumentError("modulator", "not found");
}

std::string setEnvelopeModulator(Document& doc, const std::string& trackId, const std::string& id, const std::string& param,
                                 double attackMs, double decayMs, double depth) {
  const std::string useId = setModulator(doc, trackId, id, param, "sine", 1, depth, 0);
  for (auto& m : trackById(doc, trackId).modulators)
    if (m.value("id", "") == useId) {
      m["shape"] = "envelope";
      m["attackMs"] = std::clamp(attackMs, 0.0, 2000.0);
      m["decayMs"] = std::clamp(decayMs, 1.0, 10000.0);
    }
  return useId;
}

std::string setStepModulator(Document& doc, const std::string& trackId, const std::string& id, const std::string& param,
                             double stepBeats, const std::vector<double>& steps, double depth) {
  if (steps.empty() || steps.size() > (size_t)kMaxModSteps) throw DocumentError("steps", "must be 1 to " + std::to_string(kMaxModSteps) + " values");
  double phase = 0;
  for (const auto& m : trackById(doc, trackId).modulators) if (m.value("id", "") == id) phase = m.value("phase", 0.0);
  const std::string useId = setModulator(doc, trackId, id, param, "sine", stepBeats, depth, phase);
  json values = json::array();
  for (double v : steps) values.push_back(std::clamp(v, -1.0, 1.0));
  for (auto& m : trackById(doc, trackId).modulators)
    if (m.value("id", "") == useId) {
      m["shape"] = "steps";
      m["steps"] = values;
    }
  return useId;
}

void setModulatorSource(Document& doc, const std::string& trackId, const std::string& id, const std::string& sourceTrackId) {
  if (!sourceTrackId.empty()) {
    if (sourceTrackId == trackId) throw DocumentError("source", "leave it empty for the track's own notes");
    if (!playsPatterns(trackById(doc, sourceTrackId))) throw DocumentError("source", "that track plays no notes");
  }
  for (auto& m : trackById(doc, trackId).modulators)
    if (m.value("id", "") == id) {
      if (m.value("shape", "") != "envelope" && m.value("shape", "") != "rise") throw DocumentError("source", "only an envelope starts from another track's notes");
      if (sourceTrackId.empty()) m.erase("source"); else m["source"] = sourceTrackId;
      return;
    }
  throw DocumentError("modulator", "not found");
}

const std::vector<DuckPreset>& duckPresets() {
  static const std::vector<DuckPreset> presets = {
      {"tight", "Tight", "Dips 4 dB at each hit and is back by the next sixteenth: room for the kick, no pumping", 4, 2, 0.25},
      {"pump", "Pump", "Dips 8 dB and comes back over an eighth: you hear it breathe with the kick", 8, 2, 0.5},
      {"soft", "Soft", "A gentle 3 dB dip over a quarter: only makes space", 3, 10, 1}};
  return presets;
}

void setDucking(Document& doc, const std::string& trackId, const std::string& sourceTrackId, double depthDb, double attackMs, double releaseMs) {
  Track& t = trackById(doc, trackId);
  const Track& src = trackById(doc, sourceTrackId);
  if (sourceTrackId == trackId) throw DocumentError("duck", "a track cannot dip from its own notes");
  if (!playsPatterns(src)) throw DocumentError("duck", "'" + src.name + "' plays no notes");
  bool has = false;
  for (const auto& m : t.modulators) has = has || m.value("id", "") == "duck";
  const std::string id = has ? "duck" : setEnvelopeModulator(doc, trackId, "duck", "gainDb", attackMs, releaseMs, -std::clamp(depthDb, 0.0, 48.0));
  for (auto& m : trackById(doc, trackId).modulators)
    if (m.value("id", "") == id) {
      m["param"] = "gainDb";
      m["shape"] = "envelope";
      m["depth"] = -std::clamp(depthDb, 0.0, 48.0);
      m["attackMs"] = std::clamp(attackMs, 0.0, 2000.0);
      m["decayMs"] = std::clamp(releaseMs, 1.0, 10000.0);
      m["source"] = sourceTrackId;
    }
}

void setDuckingPreset(Document& doc, const std::string& trackId, const std::string& sourceTrackId, const std::string& preset) {
  for (const auto& p : duckPresets())
    if (preset == p.id) {
      const double beatMs = 60000.0 / std::max(1.0, doc.bpm);
      setDucking(doc, trackId, sourceTrackId, p.depthDb, p.attackMs, p.releaseBeats * beatMs);
      return;
    }
  throw DocumentError("duck", "no preset '" + preset + "'");
}

void removeDucking(Document& doc, const std::string& trackId) { removeModulator(doc, trackId, "duck"); }

std::optional<std::string> duckingSource(const Track& track) {
  for (const auto& m : track.modulators)
    if (m.value("id", "") == "duck" && m.contains("source") && m["source"].is_string()) return m["source"].get<std::string>();
  return std::nullopt;
}

void removeModulator(Document& doc, const std::string& trackId, const std::string& id) {
  Track& t = trackById(doc, trackId);
  const auto before = t.modulators.size();
  t.modulators.erase(std::remove_if(t.modulators.begin(), t.modulators.end(), [&](const json& m) { return m.value("id", "") == id; }), t.modulators.end());
  if (t.modulators.size() == before) throw DocumentError("modulators", "no modulator '" + id + "' on track '" + trackId + "'");
}

void removeSend(Document& doc, const std::string& trackId, const std::string& sendId) {
  Track& t = trackById(doc, trackId);
  sendById(t, sendId);
  t.mix.sends.erase(std::remove_if(t.mix.sends.begin(), t.mix.sends.end(), [&](const Send& s) { return s.id == sendId; }), t.mix.sends.end());
}

namespace {
// The records in a track's extra that are part of its sound.
bool soundKey(const std::string& k) {
  return k == "racks" || k == "fcore" || (k.size() > 9 && k.compare(k.size() - 9, 9, "MoodKnobs") == 0);
}
json soundExtra(const json& extra) {
  json out = json::object();
  if (extra.is_object())
    for (auto it = extra.begin(); it != extra.end(); ++it) if (soundKey(it.key())) out[it.key()] = it.value();
  return out;
}
json instrumentJson(const Instrument& i) {
  json zones = json::array();
  for (const auto& z : i.samples)
    zones.push_back({{"file", z.file}, {"sha256", z.sha256}, {"name", z.name}, {"root", z.rootKey}, {"lo", z.loKey},
                     {"hi", z.hiKey}, {"loopStart", z.loopStart}, {"loopEnd", z.loopEnd}, {"extra", z.extra}});
  return {{"kind", i.kind}, {"name", i.name}, {"version", i.version}, {"params", i.params},
          {"samples", zones}, {"extra", i.extra}};
}
}  // namespace

TrackSound trackSoundOf(const Document& doc, const std::string& trackId) {
  TrackSound s;
  if (trackId.empty()) { s.effects = doc.masterEffects; return s; }
  const Track* t = doc.findTrack(trackId);
  if (!t) throw DocumentError("tracks", "no track '" + trackId + "'");
  s.instrument = t->instrument;
  s.effects = t->effects;
  s.modulators = t->modulators;
  s.extra = soundExtra(t->extra);
  return s;
}

std::string trackSoundKind(const TrackSound& sound, const std::string& trackId) {
  if (trackId.empty()) return "master";
  const Instrument& i = sound.instrument;
  return i.kind + ":" + i.name;
}

void setTrackSound(Document& doc, const std::string& trackId, const TrackSound& sound) {
  if (!sound.effects.is_array() || (int)sound.effects.size() > kMaxEffectsPerChain) throw DocumentError("effects", "not a chain");
  if (!trackId.empty()) {
    Track& t = trackById(doc, trackId);
    if (trackSoundKind(trackSoundOf(doc, trackId), trackId) != trackSoundKind(sound, trackId))
      throw DocumentError("instrument", "the track has another instrument now");
    if (!sound.modulators.is_array() || (int)sound.modulators.size() > kMaxModulators) throw DocumentError("modulators", "too many");
    t.instrument = sound.instrument;
    t.effects = sound.effects;
    t.modulators = sound.modulators;
    if (!t.extra.is_object()) t.extra = json::object();
    for (auto it = t.extra.begin(); it != t.extra.end();) it = soundKey(it.key()) ? t.extra.erase(it) : std::next(it);
    for (auto it = sound.extra.begin(); it != sound.extra.end(); ++it) t.extra[it.key()] = it.value();
  } else {
    doc.masterEffects = sound.effects;
  }
  // Curves of effects that are not in this chain go (they would point at nothing).
  std::set<std::string> ids;
  for (const auto& fx : sound.effects)
    if (fx.is_object() && fx.contains("id") && fx["id"].is_string()) ids.insert(fx["id"].get<std::string>());
  auto& list = automationOf(doc, trackId);
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Automation& a) {
    if (a.param.rfind("fx:", 0) != 0) return false;
    const auto end = a.param.find(':', 3);
    return end == std::string::npos || ids.count(a.param.substr(3, end - 3)) == 0;
  }), list.end());
}

bool sameTrackSound(const TrackSound& a, const TrackSound& b) {
  return instrumentJson(a.instrument) == instrumentJson(b.instrument) && a.effects == b.effects &&
         a.modulators == b.modulators && soundExtra(a.extra) == soundExtra(b.extra);
}

} // namespace core::doc
