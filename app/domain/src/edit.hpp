// Document operations used by the editors (M2): new projects and track management.
// Pure functions on the document; the caller commits the result to the History (one step each).
#pragma once
#include <map>
#include <set>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "document.hpp"

namespace core::doc {

Document newEmptyDocument();
// Kick on every beat, rolling bass on the off-sixteenths, hats on the off-beats: 145 BPM, 16 bars.
Document newTemplateDocument();

// Built-in instruments the editors can add, with their display names.
struct InstrumentChoice { const char* name; const char* label; };
const std::vector<InstrumentChoice>& builtinInstruments();

// The built-in instruments' own parameters: the engine's catalogue (core::instrumentParams), same names,
// order, defaults and ranges (a test checks it), with what the window and the AI tools need on top.
struct InstrumentParamInfo {
  const char* name;
  const char* label;
  double def, min, max;
  const char* unit;
  double step;        // 0 = continuous; 1 = whole steps (octave, a switch)
  bool logScale;      // frequencies and times: the slider is logarithmic
  bool automatable;   // false: a jump would click or mean nothing mid-note (octave, a switch)
};
struct InstrumentInfo {
  const char* name;   // kick | bass | hats | drums | sampler
  const char* label;
  std::vector<InstrumentParamInfo> params;  // in the engine's order
};
const std::vector<InstrumentInfo>& instrumentCatalogue();
const InstrumentInfo* findInstrumentInfo(const std::string& name);
// The parameter of a built-in instrument, or null (audio, an unknown name).
const InstrumentParamInfo* findInstrumentParam(const Track& track, const std::string& param);
// What the instrument plays with now: the value set on the track, or the default.
double instrumentParamValue(const Track& track, const InstrumentParamInfo& param);
// Sets one parameter of a built-in instrument (the fixed value; automation, when there is some, plays over it).
// Throws DocumentError for an audio track, an unknown name, a value out of range, a choice not whole.
void setInstrumentParam(Document& doc, const std::string& trackId, const std::string& param, double value);
// Puts every parameter of a built-in instrument back to its default (the sound it starts with). Automation curves stay.
// Returns how many values changed (0: already at the defaults). Throws DocumentError for an audio track.
int resetInstrumentParams(Document& doc, const std::string& trackId);
// True when every parameter of the track's built-in instrument is at its default (or it has none).
bool instrumentAtDefaults(const Track& track);

// A free id from a base ("bass" -> "bass", "bass-2", ...), valid for the id rules.
std::string uniqueTrackId(const Document& doc, const std::string& base);

// Each returns the id of the track it created (or changed), or throws DocumentError.
std::string addTrack(Document& doc, const std::string& instrument, const std::string& name = "");
std::string duplicateTrack(Document& doc, const std::string& trackId);
void renameTrack(Document& doc, const std::string& trackId, const std::string& name);
// Removing a bus sends what fed it to where the bus went, and drops the sends to it.
void removeTrack(Document& doc, const std::string& trackId);
// The shown track is gone (deleted, or undone away): which track is shown now (deep hunt P06). One of the other
// selected tracks that are still there (the first in song order, taken out of alsoSelected), else the track that took
// its place in orderBefore (the one after it, or before it at the end), else "". Never a track nobody picked added to
// a multi-selection: Stage Core's Arrange acts on the selection. alsoSelected loses the tracks that are gone.
std::string shownAfterRemoval(const Document& now, const std::vector<std::string>& orderBefore, const std::string& removed,
                              std::set<std::string>& alsoSelected);

// ---- routing (M7a): buses, outputs and sends. Every change is checked (only to a bus, no loops).
std::string addBusTrack(Document& doc, const std::string& name = "");  // e.g. "Reverb" or "Drums"
void setTrackOutput(Document& doc, const std::string& trackId, const std::string& busId);  // "" = the master
std::string addSend(Document& doc, const std::string& trackId, const std::string& busId, double gainDb = 0, bool pre = false);
void setSend(Document& doc, const std::string& trackId, const std::string& sendId, double gainDb, bool pre);
void removeSend(Document& doc, const std::string& trackId, const std::string& sendId);

// ---- modulation (M8a): a tempo-synced LFO on a parameter ("cutoff", "gainDb", "pan"...).
// shape: sine | triangle | square | saw | ramp | random; rateBeats: beats per cycle (0.25 = a sixteenth);
// depth in the parameter's units (octaves for a parameter in Hz); phase 0..1. Returns its id ("m1"...).
std::string setModulator(Document& doc, const std::string& trackId, const std::string& id, const std::string& param,
                         const std::string& shape, double rateBeats, double depth, double phase = 0);
// Turns a modulator on or off; off, it moves nothing and keeps its depth ("on": false in the song).
void setModulatorOn(Document& doc, const std::string& trackId, const std::string& id, bool on);
// M8b: an envelope from each note of the track on param (0 -> 1 over attackMs, then down to -60 dB over
// decayMs), times depth in the parameter's units (octaves for a parameter in Hz). Not glided.
std::string setEnvelopeModulator(Document& doc, const std::string& trackId, const std::string& id, const std::string& param,
                                 double attackMs, double decayMs, double depth);
// A step sequence on param: one value per step (1..16, each -1..1, times depth), each held for stepBeats.
// It loops on the song's grid like an LFO. Returns its id.
std::string setStepModulator(Document& doc, const std::string& trackId, const std::string& id, const std::string& param,
                             double stepBeats, const std::vector<double>& steps, double depth);
// The trigger of an envelope: "" = the track's own notes, else another track that plays notes.
void setModulatorSource(Document& doc, const std::string& trackId, const std::string& id, const std::string& sourceTrackId);
void removeModulator(Document& doc, const std::string& trackId, const std::string& id);
// Designed ducking: the track's volume dips at every note of another track (the bass at every kick), with an
// envelope's shape: down by depthDb over attackMs, back over releaseMs. One per track (modulator "duck"); the
// source plays even when muted (a ghost kick). Refused: the track itself, a bus or an audio track as source.
struct DuckPreset {
  const char* id;
  const char* label;
  const char* what;     // plain words (the hover dot)
  double depthDb, attackMs, releaseBeats;
};
const std::vector<DuckPreset>& duckPresets();  // Tight, Pump, Soft (sources: cinetrance 3-6 dB; kick tail 1/16)
void setDucking(Document& doc, const std::string& trackId, const std::string& sourceTrackId, double depthDb, double attackMs, double releaseMs);
void setDuckingPreset(Document& doc, const std::string& trackId, const std::string& sourceTrackId, const std::string& preset);  // at the song's BPM
void removeDucking(Document& doc, const std::string& trackId);
std::optional<std::string> duckingSource(const Track& track);

// ---- patterns and clips (one track)
Track& trackById(Document& doc, const std::string& trackId);  // throws DocumentError
Pattern& patternById(Track& track, const std::string& patternId);
std::string addPattern(Track& track, int64_t lengthTicks);  // a new empty pattern; returns its id
void clearPattern(Track& track, const std::string& patternId);  // every note goes; the pattern and its clips stay
// The pattern and every clip that plays it. A track keeps at least one pattern (clear that one instead).
void removePattern(Track& track, const std::string& patternId);
std::string addClip(Track& track, const std::string& patternId, int64_t startTick, int64_t lengthTicks);
// A whole-clip copy: phase, attachments and unknown fields stay with it; only id/start change.
std::string duplicateClip(Track& track, const std::string& clipId, int64_t startTick);
std::string pasteClip(Track& track, Clip copy, int64_t startTick);  // a copied clip (Ctrl+V), new id; its pattern must be there
void moveClip(Track& track, const std::string& clipId, int64_t startTick);
void resizeClip(Track& track, const std::string& clipId, int64_t lengthTicks);  // the pattern repeats inside
void removeClip(Track& track, const std::string& clipId);
// Gives the clip its own copy of a shared pattern (editing it no longer changes the other clips).
// Returns the clip's pattern id.
std::string makeClipUnique(Track& track, const std::string& clipId);
// Cuts a clip at atTick (strictly inside it): the second part keeps the pattern's phase. Returns its id.
std::string splitClip(Track& track, const std::string& clipId, int64_t atTick);
void removeUnusedPatterns(Track& track);  // keeps at least one

// ---- effects (M4): built-in inserts on a track or on the master (trackId "")
struct EffectParamInfo {
  const char* name;
  const char* label;  // Portuguese, for the window
  double def, min, max;
  const char* unit;
  double step;        // 0 = continuous; 1 = whole steps (a choice)
  bool logScale;      // frequencies and times: the slider is logarithmic
  std::vector<const char*> choices;  // labels of the whole steps, when step is 1
  bool automatable = true;           // false: a jump would click (delay time, pre-delay)
  const char* hint = nullptr;        // in plain words, what it changes in the sound (the knob's tip)
};
struct EffectInfo {
  const char* name;
  const char* label;
  std::vector<EffectParamInfo> params;  // in the engine's order (core::effectParams)
  int version = 1;                      // written into new effects (core::currentEffectVersion)
};
constexpr int kMaxEffectsPerChain = 16;
const std::vector<EffectInfo>& builtinEffects();
// Reading an effect of a document that may have come from elsewhere: never throws on odd values.
std::string effectName(const json& fx);
bool effectBypassed(const json& fx);
double effectParam(const json& fx, const std::string& param, double fallback);
const EffectInfo* findEffect(const std::string& name);
// One entry per chain effect for the menus that list a chain (the track's right-click, Plugins > Master
// effects). Every effect in the chain is offered, Core's own ones included: an old song or a template
// carries them, and the menus listed the plugins only, so an own effect could never be removed from the
// app (bug 2026-11-17). index: its place in the chain (removeEffect and setEffectBypass take the same);
// window: a plugin effect with a window to open; label: the name shown.
struct EffectMenuEntry { int index = 0; bool plugin = false; bool window = false; std::string label; };
std::vector<EffectMenuEntry> effectMenuEntries(const json& chain);
// Starting points of a Core Device for one style (GLITCH, PULSE): a name, a line on what it does, its values.
// Every preset can be chosen in any style; the style only orders the menu. Unlisted knobs keep their default.
struct EffectPreset {
  const char* effect;
  const char* style;  // the Style FX ids: darkpsycore, hitech, slambient, psytrance
  const char* name;
  const char* tip;
  std::vector<std::pair<const char*, double>> values;
  const char* from = nullptr;  // a Core FX preset: the old effect it was made for ("space", "echo"...)
};
const std::vector<EffectPreset>& effectPresets();
// A built-in effect with these values. A recipe written for an old effect (saturation, delay, reverb, space,
// echo, spiral, flux) becomes the Core FX that sounds like it (corefx_legacy.hpp).
int addEffectWith(Document& doc, const std::string& trackId, const std::string& name, const json& params);
json& effectChain(Document& doc, const std::string& trackId);
const json& effectChain(const Document& doc, const std::string& trackId);
// At the end, with an id of its own (the engine keeps an effect's tail across edits by that id); returns its place.
int addEffect(Document& doc, const std::string& trackId, const std::string& name);
// The key of the song as a note name number 0..11 (0 = C), from the bass tracks (the notes they hold longest),
// or from every melodic track when there is no bass; -1 when the song has no notes.
int songRoot(const Document& doc);
// The note nearest to `note` (within 6 semitones) that is the song's root; `note` when there is no root.
int followSongKey(const Document& doc, int note);
// The same, with a preset's values (clamped to each knob's range). A METAL preset's note follows songRoot.
int addEffect(Document& doc, const std::string& trackId, const EffectPreset& preset);
// The psytrance phrase-end comb (2026-11-07, the FL tutorial Diogo sent): a Comb filter, silent (Mix 0) except in
// the last bar of every `phraseBars` bars, where it fades in while its pitch falls (a decaying, comb-like sweep that
// marks the end of the phrase). Two automation lanes, ordinary ones afterwards. Returns the effect's index.
int addPhraseEndComb(Document& doc, const std::string& trackId, int phraseBars = 8);
// A preset's values on the effect already at `index` (the help window's "Try it"): knobs it does not list go back
// to their default (a preset never stacks on the previous one); the same clamping and METAL key rule as addEffect. Refused (DocumentError) when the effect there is not
// the preset's.
void applyEffectPreset(Document& doc, const std::string& trackId, int index, const EffectPreset& preset);
// The preset of that effect with that name, or null.
const EffectPreset* findEffectPreset(const std::string& effect, const std::string& name);
void removeEffect(Document& doc, const std::string& trackId, int index);
void moveEffect(Document& doc, const std::string& trackId, int from, int to);
void setEffectParam(Document& doc, const std::string& trackId, int index, const std::string& param, double value);
void setEffectBypass(Document& doc, const std::string& trackId, int index, bool bypass);
// M7c: the compressor at index follows another track's signal (after its inserts, before its fader: a
// muted "ghost" kick still ducks). "" = its own input again. Refused: not a compressor, a bus, itself,
// a loop, the master.
void setEffectSidechain(Document& doc, const std::string& trackId, int index, const std::string& sourceTrackId);

// ---- instruments
// Another built-in instrument for a track that plays notes. The notes, patterns, clips, effects, mixer and
// their automation stay; the old instrument's own settings and their automation go (they would not mean
// anything to the new one).
void replaceInstrument(Document& doc, const std::string& trackId, const std::string& builtin);

// ---- plugins (VST3, VST2): `plugin` is what the scan found {format, id, name, vendor, version, file, ...}
std::string addPluginTrack(Document& doc, const json& plugin);  // a track played by the plugin; returns its id
int addPluginEffect(Document& doc, const std::string& trackId, const json& plugin);  // at the end of the chain
// The track plays this plugin (the same one: nothing changes). Notes, clips, effects and the mixer stay.
void replaceInstrumentWithPlugin(Document& doc, const std::string& trackId, const json& plugin);
// Where the engine keeps a plugin: "track:<id>", or "fx:<track id or master>:<effect id>".
std::string pluginKey(const std::string& trackId, const std::string& effectId = "");
bool hasPlugins(const Document& doc);
// The states the engine gave ({key: base64}) written into the document; returns how many changed.
int applyPluginStates(Document& doc, const json& states);
// Takes away what an instrument's mood put on the track (the W-Core mood effects, their curves, and the
// record of the knobs a mood moved), for when the track changes instrument. The user's own effects stay.
void dropInstrumentMoods(Document& doc, const std::string& trackId);

// ---- automation (M4): a parameter that changes along the song, on a track or on the master (trackId "")
struct AutomatableParam {
  std::string param;   // "gainDb", "pan", "fx:<effect id>:<parameter>"
  std::string label;   // for the window
  double min, max, def;
  double current;      // the value set without automation (the fader, the effect's knob)
  std::string unit;
  bool logScale;
};
// What can be automated there: volume, pan (tracks), the built-in instrument's parameters and every parameter
// of the built-in effects.
std::vector<AutomatableParam> automatableParams(const Document& doc, const std::string& trackId);
std::string effectParamKey(const std::string& effectId, const std::string& param);  // "fx:<id>:<param>"
const Automation* findAutomation(const Document& doc, const std::string& trackId, const std::string& param);
double automationValueAt(const Automation& a, int64_t tick);  // linear between points, held outside them
// Adds a point (or replaces the one at that tick); the value is kept in the parameter's range. Returns its index.
int setAutomationPoint(Document& doc, const std::string& trackId, const std::string& param, int64_t tick, double value);
// Moves a point; it stays between its neighbours (time order never changes). Returns its index.
int moveAutomationPoint(Document& doc, const std::string& trackId, const std::string& param, int index, int64_t tick, double value);
void removeAutomationPoint(Document& doc, const std::string& trackId, const std::string& param, int index);  // the last one removes the automation
void clearAutomation(Document& doc, const std::string& trackId, const std::string& param);

// Replaces the points of a curve in [startTick, endTick) and leaves the curve as it was outside: points are added
// at the edges with the values it had there (heldValue when there was no curve yet: the knob's value), so only
// the region sounds different. A point at startTick is kept by moving the left edge one tick earlier. Empty
// points: the curve goes straight from its old value at the start to its old value at the end.
void setAutomationInRange(Document& doc, const std::string& trackId, const std::string& param, int64_t startTick, int64_t endTick,
                          const std::vector<AutomationPoint>& points, double heldValue);

// ---- regions: a span of the song [startTick, endTick) on one track
struct RegionIsolation {
  std::vector<std::string> insideClips;               // the clips that now lie inside the region, in time order
  std::vector<std::pair<std::string, std::string>> patterns;  // pattern -> the region's own copy (only those that also play outside)
  int splitClips = 0;                                 // clips cut at an edge
};
// Makes the region editable on its own: the track's clips are cut at both edges (the second part keeps the
// pattern's phase, so nothing sounds different), then every pattern that plays both inside and outside gets a
// copy that only the clips inside use. Editing those copies (or the patterns used only inside) changes the
// region and nothing else. ids: wanted ids for the copies (pattern -> id), so a batch can name them; the rest get
// the next free id. Throws DocumentError (audio track, empty region, too many clips or patterns, an id taken).
RegionIsolation isolateRegion(Document& doc, const std::string& trackId, int64_t startTick, int64_t endTick,
                              const std::map<std::string, std::string>& ids = {});

// ---- audio tracks and clips (M3)
constexpr double kMinClipSeconds = 0.001;  // the shortest audio clip a document accepts
std::string addAudioTrack(Document& doc, const std::string& name);
// The whole asset from startTick; returns the clip id.
std::string addAudioClip(Document& doc, const std::string& trackId, const std::string& assetId, int64_t startTick);
void moveAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t startTick);
// A sampler zone's sustain loop (file seconds; loopEnd 0 takes it away). Throws DocumentError (not a sampler,
// no such zone, loopEnd < loopStart + 2 ms, past the zone's file when its length is known: fileSeconds > 0).
void setSampleLoop(Document& doc, const std::string& trackId, size_t zone, double loopStart, double loopEnd, double fileSeconds = 0);

// Trim: the start moves with the music and the audio stays where it was (the offset follows); the end
// changes the duration. Both are limited to the file.
void trimAudioClipStart(Document& doc, const std::string& trackId, const std::string& clipId, int64_t startTick);
void trimAudioClipEnd(Document& doc, const std::string& trackId, const std::string& clipId, double durationSeconds);
// Two clips that play exactly what the one did. Returns the id of the second.
std::string splitAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t atTick);
void removeAudioClip(Document& doc, const std::string& trackId, const std::string& clipId);
// The audio editor's selection (song ticks): keep only that part of the clip, or remove it (the clip
// becomes two, with a gap; at an edge, the clip just gets shorter).
void cropAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t fromTick, int64_t toTick);
// Cuts the clip at every multiple of stepTicks (the song's grid) inside it; returns how many clips it became.
int sliceAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t stepTicks);
void removeAudioRange(Document& doc, const std::string& trackId, const std::string& clipId, int64_t fromTick, int64_t toTick);
std::string duplicateAudioClip(Document& doc, const std::string& trackId, const std::string& clipId, int64_t startTick);
std::string pasteAudioClip(Document& doc, const std::string& trackId, AudioClip copy, int64_t startTick);  // Ctrl+V, new id
// A clip dragged to another track (or the same one), at startTick; returns its id there. A pattern clip takes a
// copy of its pattern (the source keeps its own); an audio clip goes only to an audio track, a pattern clip only
// to an instrument track.
std::string moveClipToTrack(Document& doc, const std::string& fromTrack, const std::string& clipId, const std::string& toTrack, int64_t startTick);
double secondsPerTick(const Document& doc);  // at the song's start tempo
// Song time through the tempo map (the same conversion as the engine). With no tempo change these are
// tick * secondsPerTick and its inverse, exactly as before the map.
double secondsAt(const Document& doc, double tick);
double tickAtSeconds(const Document& doc, double seconds);
double secondsBetween(const Document& doc, double fromTick, double toTick);  // (to - from) * spt with one tempo
int64_t audioClipEndTick(const Document& doc, const AudioClip& clip);  // where it stops, in song ticks

// ---- sections (the whole song)
std::string addSection(Document& doc, const std::string& name, int64_t startTick, int64_t lengthTicks);
void renameSection(Document& doc, const std::string& sectionId, const std::string& name);
// "#rrggbb", or empty: back to the colour of its type (from the name).
void setSectionColor(Document& doc, const std::string& sectionId, const std::string& color);
void removeSection(Document& doc, const std::string& sectionId);  // the music stays
void swapSection(Document& doc, const std::string& sectionId, bool withNext);
std::string duplicateSection(Document& doc, const std::string& sectionId);
// Empty time at atTick: everything from there (clips, automation, sections, tempo changes, the loop) moves
// on by `ticks` and the song gets longer. Clips that cross atTick are split there; a section that
// crosses it grows.
void insertTime(Document& doc, int64_t atTick, int64_t ticks);
// Removes [fromTick, fromTick + ticks): the music there goes, everything after moves back, the song gets
// shorter. Clips and sections that cross an edge are cut there.
void deleteTime(Document& doc, int64_t fromTick, int64_t ticks);
void setSongLength(Document& doc, int64_t lengthTicks);  // tempo changes past the new end go
// The song's tempo changes (sorted here; ticks after the start and inside the song, BPM 40-600). Throws.
void setTempoMap(Document& doc, std::vector<TempoPoint> changes);
double bpmAt(const Document& doc, int64_t tick);  // the tempo playing at a tick

// ---- notes (one pattern)
std::string newNoteId(const Pattern& pattern);                 // n<highest + 1>
// Step grid: removes the notes of this pitch starting at this tick, or adds one. Returns true if added.
bool toggleStep(Pattern& pattern, int64_t tick, int pitch, int64_t length, double velocity);
// Copies the notes right after themselves (by the span they cover, snapped up to snapTicks), inside
// the pattern. Returns the ids of the copies.
std::vector<std::string> duplicateNotes(Pattern& pattern, const std::vector<std::string>& noteIds, int64_t snapTicks);
// Pattern length change: notes that no longer fit are removed, lengths are cut to the end.
void setPatternLength(Pattern& pattern, int64_t lengthTicks);
// Longer pattern with its notes repeated to fill the new bars (as a loop would play them); shorter or
// equal: the same as setPatternLength.
void extendPatternRepeating(Pattern& pattern, int64_t lengthTicks);

// ---- A/B (Diogo 2026-11-07: "eu nao tenho A B no core?"): two versions of a track's sound, swapped to compare.
// The sound is the instrument (its knobs, samples, plugin state), the effect chain, the Movement (LFOs) and the
// records that belong to them (a mood's knobs, racks, FCore). Notes, clips, the mixer, sends and automation stay.
// trackId "" = the master: its effect chain only.
struct TrackSound {
  Instrument instrument;
  json effects = json::array(), modulators = json::array(), extra = json::object();
};
TrackSound trackSoundOf(const Document& doc, const std::string& trackId);  // throws DocumentError (no track)
// Puts a sound back. Refused (DocumentError) when the track now has another instrument (a W-Core sound never
// lands on a W-Core track). Automation of an effect the chain no longer has goes with it, as removeEffect does.
void setTrackSound(Document& doc, const std::string& trackId, const TrackSound& sound);
bool sameTrackSound(const TrackSound& a, const TrackSound& b);
// What must match for a stored sound to be put back on the track: "builtin:wave", "plugin:<id>", "master".
std::string trackSoundKind(const TrackSound& sound, const std::string& trackId);

} // namespace core::doc
