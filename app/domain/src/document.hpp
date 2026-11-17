// The app document ("core-document", schema 1): everything the app opens, edits and saves.
// The engine never sees it; it receives a projection (core-project v2) made by toProjection().
// No GUI, no audio device, no engine code here: the domain is plain data, validation and conversion.
// Contract: docs/contracts/core-document.md.
#pragma once
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "json.hpp"

namespace core::doc {

using json = nlohmann::json;

constexpr const char* kDocumentFormat = "core-document";
// 2: tracks hold named patterns placed as clips, and the song has sections. Schema 1 files are
// migrated when opened (to a copy: the app saves them under a new name).
constexpr int kSchemaVersion = 3;  // 3: transport.tempoMap. A song without tempo changes is still written as 2.
constexpr int kPpq = 960;
// The number after an id's prefix ("n12" -> 12) for the next free id: at most a billion, so "+ 1" never overflows
// with a huge or odd id from a file (sweep). Not a number: 0.
inline long idNumber(const char* digits) {
  const long v = std::strtol(digits, nullptr, 10);
  return v < 0 ? 0 : v > 1000000000L ? 1000000000L : v;
}

// A document that cannot be read. `path` names the entity and field, e.g. "tracks[1].patterns[0].notes[3].pitch".
struct DocumentError : std::runtime_error {
  std::string path;
  DocumentError(std::string where, const std::string& message)
      : std::runtime_error(where.empty() ? message : where + ": " + message), path(std::move(where)) {}
};
// Saved by a newer app: never converted blindly. The UI may show it read-only; the file stays intact.
struct FutureVersionError : DocumentError {
  int schemaVersion;
  explicit FutureVersionError(int version)
      : DocumentError("schemaVersion", "made by a newer version of Core (schema " + std::to_string(version) + ")"), schemaVersion(version) {}
};

// Every object keeps the keys this version does not know in `extra`, and writes them back unchanged,
// so opening and saving never loses content written by a newer minor version or by hand.
struct Roll { int count = 0; double velocityFrom = 1, velocityTo = 1; json extra = json::object(); };

struct Note {
  std::string id;          // stable, unique in its pattern
  int64_t tick = 0;        // from the start of the pattern
  int64_t length = 240;
  int pitch = 36;
  double velocity = 0.8;
  bool hasRoll = false;
  Roll roll;
  json extra = json::object();
};

struct AutomationPoint { int64_t tick = 0; double value = 0; json extra = json::object(); };
struct Automation {
  std::string param;       // instrument parameter, or "gainDb" / "pan"
  std::vector<AutomationPoint> points;  // absolute song ticks, sorted
  json extra = json::object();
};

// A file of the user's sample library played by the Core Sampler on loKey..hiKey (rootKey: the note it
// was recorded at). The file stays in the library: `file` is its absolute
// path and `sha256` identifies it, so a moved library is found again by content, never replaced by
// another sound. Contract: docs/contracts/core-document.md ("Sampler").
struct SampleRef {
  std::string file;        // absolute, UTF-8
  std::string sha256;      // of the file's bytes (64 hex), "" if not known
  std::string name;        // shown to the user (the file name)
  int rootKey = 60, loKey = 0, hiKey = 127;
  // A sustain loop in the file's seconds (the engine plays it while the note is held); loopEnd 0 = none.
  double loopStart = 0, loopEnd = 0;
  bool loops() const { return loopEnd > 0; }
  json extra = json::object();
};
constexpr double kMinSampleLoopSeconds = 0.002;

// The built-in sampler's sound version (engine sampler.hpp): Hermite reading, tune followed while a note plays,
// band-limited when played above its rate (version 4).
constexpr int kSamplerVersion = 4;
struct Instrument {
  std::string kind = "none";           // "plugin" | "none" (notes, no instrument yet) | "audio" | "bus" | "builtin" (an old Core instrument, kept, not played)
  std::string name;                    // builtin: kick | wave (W-Core) | hats | drums | sampler (validated by the engine)
  int version = 1;                     // builtin sound version
  std::map<std::string, double> params;  // only the values set; the engine fills the defaults
  std::vector<SampleRef> samples;      // builtin "sampler": its zones (the first one holding the key plays)
  // builtin "wave" (W-Core, version 2): its modulation, {"layers": [{"lfos": [...], "links": [...]}]}
  // (engine/src/dsp/wave_mod.hpp). null = none.
  json synth;
  // kind "plugin": a VST3 played by the engine, {format: "vst3", id, name, vendor, version, file, state}.
  // Only a reference to A/B Core's own sounds against (Serum 2); Core's templates never use one.
  json plugin;
  json extra = json::object();
};

// M7a: a copy of the track's signal into a bus (post-fader unless pre).
struct Send { std::string id; std::string to; double gainDb = 0; bool pre = false; json extra = json::object(); };
constexpr int kMaxSends = 8;
constexpr int kMaxModulators = 16;  // the engine's limit (FCore intents use several)
constexpr int kMaxModSteps = 16;     // the engine's limit of a "steps" modulator
// output: "" = the master, or a bus track's id (instrument kind "bus"). Saved only when set.
struct Mix {
  double gainDb = 0, pan = 0; bool mute = false, solo = false;
  std::string output;
  std::vector<Send> sends;
  json extra = json::object();
};
// A named pattern of one track. Clips that use the same pattern share its notes.
struct Pattern {
  std::string id;          // stable, unique in its track
  std::string name;
  int64_t lengthTicks = 4 * kPpq;
  std::vector<Note> notes;
  json extra = json::object();
};

// A pattern placed on the song: it repeats from startTick for lengthTicks.
struct Clip {
  std::string id;          // stable, unique in its track
  std::string patternId;
  int64_t startTick = 0;
  int64_t lengthTicks = 4 * 4 * kPpq;
  int64_t offsetTicks = 0;  // where in the pattern the clip starts (a clip split mid-pattern keeps the phase)
  json extra = json::object();
};

// An audio file of the project, copied into its folder and never changed (an edit makes a new asset).
struct Asset {
  std::string id;
  std::string name;            // shown to the user (the original file name)
  std::string path;            // relative to the project folder, '/' separated, e.g. "Audio/1a2b3c4d-vox.wav"
  std::string sha256;          // of the file's bytes: identifies it, and checks a relocated file
  double sampleRate = 48000;
  int channels = 2;
  int64_t frames = 0;
  // Made by Core from another asset (reversed, stretched...): that one's sha256 and what was done ("" = imported).
  std::string madeFrom, made;
  json extra = json::object();
  double seconds() const { return sampleRate > 0 ? (double)frames / sampleRate : 0; }
};

// A piece of an asset on an audio track. The start follows the musical grid (ticks); the offset into
// the file and the duration are in seconds, so tempo changes move it but never stretch it.
struct AudioClip {
  std::string id;
  std::string assetId;
  int64_t startTick = 0;
  double offsetSeconds = 0, durationSeconds = 1;
  double gainDb = 0, fadeInSeconds = 0, fadeOutSeconds = 0;
  bool mute = false;
  // VOICE STUTTER's processors, written only when on: the pitch moved (length kept), or a piece of the file held
  // for the clip's whole length (a vocal freeze, from offsetSeconds).
  double pitchSemitones = 0, freezeSeconds = 0;
  json extra = json::object();
};

// A named part of the song (intro, drop...), on the ruler. Sections do not overlap.
struct Section {
  std::string id;
  std::string name;
  int64_t startTick = 0;
  int64_t lengthTicks = 8 * 4 * kPpq;
  json extra = json::object();
  std::string color;  // "#rrggbb" chosen by the user, or empty: the colour of its type (sectionColor); visual only
};

// The section colours (CORE-MANUAL app/ui-colours.md, Diogo 2026-11-09): deep, dark tones that read as a background
// behind the tracks and never repeat a track colour. A section without its own colour takes the one of its type,
// read from the name: intro green, build-up amber, drop/main red, break blue, outro teal, groove purple.
struct SectionSwatch { const char* name; const char* color; };
inline constexpr SectionSwatch kSectionSwatches[] = {
    {"Green", "#2f6f57"}, {"Amber", "#8a6d1e"}, {"Red", "#8e3341"},    {"Blue", "#24508f"},
    {"Teal", "#2f6470"},  {"Purple", "#5b3f8c"}, {"Slate", "#4a4f6e"}};
// Groups (buses): vivid, all at the same tone, never a track's role colour (Diogo 2026-11-09 08:04); a new bus takes
// the first one no other bus of the song has.
inline constexpr const char* kGroupColors[] = {"#257bf4", "#f4d125", "#25f459", "#f48525",
                                               "#f42525", "#25d8f4", "#9325f4", "#af6a98"};
std::string sectionTypeColor(const std::string& name);  // "#rrggbb" for the type in the name (Slate when unknown)
// A colour saved with one of the old section swatches (the track hues, before 2026-11-09) shows as its new swatch.
std::string currentSectionSwatch(const std::string& color);
inline std::string sectionColor(const Section& s) { return s.color.empty() ? sectionTypeColor(s.name) : currentSectionSwatch(s.color); }

struct Track {
  std::string id;          // stable, [A-Za-z0-9_-]{1,40}
  std::string name;
  std::string color;       // "#rrggbb" or empty; visual only
  Instrument instrument;
  Mix mix;
  std::vector<Pattern> patterns;  // at least one (none on an audio track)
  std::vector<Clip> clips;
  std::vector<AudioClip> audioClips;  // instrument.kind == "audio"
  std::vector<Automation> automation;
  json effects = json::array();
  // M8a: LFOs on the track's parameters [{id, param, shape, rateBeats, depth, phase}] (the engine checks
  // the parameter and the ranges). Saved only when there is one.
  json modulators = json::array();
  json extra = json::object();

  const Pattern* findPattern(const std::string& id) const;
  Pattern* findPattern(const std::string& id);
  int clipsUsing(const std::string& patternId) const;
};

// Loop region in ticks, [startTick, endTick). Transport state saved with the song; not part of the
// projection (the app sends it with transport.loop).
// A tempo change: from `tick` on the song plays at `bpm`: a step, or with `ramp` a glide (the BPM goes in a
// straight line from the previous point, or the start, and arrives here at `tick`).
struct TempoPoint { int64_t tick = 0; double bpm = 145; json extra = json::object(); bool ramp = false; };
inline json tempoPointJson(const TempoPoint& t) {  // {tick, bpm} and ramp only when it glides
  json out = {{"tick", t.tick}, {"bpm", t.bpm}};
  if (t.ramp) out["ramp"] = true;
  return out;
}
constexpr int kMaxTempoPoints = 64;
constexpr size_t kMaxPatternNotes = 4096;  // the reader refuses a pattern with more notes

struct Loop { bool enabled = false; int64_t startTick = 0, endTick = 4 * 4 * kPpq; json extra = json::object(); };
// Where Play starts. The loop on and the start line outside it: the loop's start (Diogo 2026-11-09: "tive problema a
// meter loop devido ao iniciar ir sempre para o cursor"). Inside the loop, or the loop off: the start line, as it is.
// The piano roll shows the song's position inside the pattern (the position modulo the pattern's length). A click
// on its ruler at patternTick moves the song's position to that tick in the same pass of the pattern.
inline int64_t songTickInPattern(int64_t songNow, int64_t patternLength, int64_t patternTick) {
  if (patternLength <= 0) return std::max<int64_t>(0, patternTick);
  const int64_t pass = std::max<int64_t>(0, songNow) / patternLength;
  return pass * patternLength + std::clamp<int64_t>(patternTick, 0, patternLength - 1);
}
inline int64_t playStartTick(const Loop& loop, int64_t position) {
  if (!loop.enabled || loop.endTick <= loop.startTick) return position;
  return position >= loop.startTick && position < loop.endTick ? position : loop.startTick;
}

struct Document {
  std::string name;
  double bpm = 145;                    // at the song's start
  std::vector<TempoPoint> tempoMap;    // later changes (transport.tempoMap): ticks > 0, increasing, inside the song
  Loop loop;
  int64_t lengthTicks = 16 * 4 * kPpq;
  double masterGainDb = 0;
  json masterEffects = json::array();  // the master chain (same shape as Track::effects), after the master gain
  std::vector<Automation> masterAutomation;  // "gainDb" and "fx:<id>:<param>" of the master chain
  std::vector<Track> tracks;
  std::vector<Section> sections;  // sorted by start
  std::vector<Asset> assets;      // audio files used by audio clips
  json ui = json::object();       // layout, zoom...: saved, never sent to the engine
  json transportExtra = json::object(), masterExtra = json::object(), extra = json::object();

  const Track* findTrack(const std::string& id) const;
  const Asset* findAsset(const std::string& id) const;
};

// Validates; throws DocumentError / FutureVersionError. Older schemas are migrated; 
// (optional) receives the schema the file had, or 0 if it was current.
Document parseDocument(const json& value, int* migratedFrom = nullptr);
// A track of B-Core or of the older "bass" synth (both removed) becomes W-Core: the closest B-Core preset's W-Core
// copy, Octave and Volume kept (parseDocument does it).
void migrateLegacyBCore(Track& t);
// A W-Core of version 1 becomes version 2: its fixed movements are links of its modulation (the engine's
// wave_legacy.hpp, the same conversion as for project files), so it plays the same. parseDocument does it.
void migrateLegacyWave(Track& t);
// One version of each built-in effect and of the sampler (2026-11-09, Diogo: "não existem versões agora, só 1"): an
// older one, or none written, becomes the current one, so the song plays and shows the newest sound. parseDocument does it.
void upgradeOldVersions(Document& d);
json toJson(const Document& doc);            // the saved form (sorted keys, so dumps are canonical)
// What the engine plays (core-project v2). Names, colors and ui are left out: changing them
// never needs a new projection.
// assetRoot: the project folder, where the assets' relative paths start (audio clips are sent with
// absolute paths). Empty: audio clips are left out.
json toProjection(const Document& doc, const std::filesystem::path& assetRoot = {});

// Same saved content (dirty flag: compare with the last saved state, not a counter).
// Instrument tracks play patterns; audio tracks play audio clips; a bus plays what is routed to it.
inline bool playsPatterns(const Track& t) { return t.instrument.kind != "audio" && t.instrument.kind != "bus"; }
// M7a: outputs and sends go to buses of this document, never to their own track, and never loop.
void checkRouting(const Document& doc);  // throws DocumentError
bool sameContent(const Document& a, const Document& b);
// Same sound: no new projection needed.
bool sameSound(const Document& a, const Document& b);

// What an import kept, changed and could not carry over.
struct ImportReport {
  std::vector<std::string> converted;  // e.g. note ids created
  std::vector<std::string> kept;       // stored in the document but not played
};
// A core-project v2 file (engine format) as a new document. The source file is never written.
// v1 (JS editor) files are not opened here yet: their audio tracks need the asset support of M3.
Document importProjectV2(const json& project, ImportReport* report = nullptr);

// ---- names, the way the reader takes them
// parseDocument refuses a pattern or a section name over 40 bytes and a track name over 80 (an accented letter
// counts as 2), so a name Core writes for itself has to fit before the song is saved - and a cut that lands
// inside a character is not UTF-8 at all, which fails the save as well (audit 2026-11-17, #1).
inline constexpr size_t kMaxPatternNameBytes = 40;
inline constexpr size_t kMaxSectionNameBytes = 40;
inline constexpr size_t kMaxTrackNameBytes = 80;

// `s` cut to at most max bytes, never inside a UTF-8 character.
std::string cutUtf8(const std::string& s, size_t max);
// head + suffix inside max bytes. The suffix is what tells the two apart (" 2", " copy", "b"), so when there is
// no room the head loses bytes, never the suffix. A suffix that is the name on its own keeps its own tail.
std::string nameWithSuffix(const std::string& head, const std::string& suffix, size_t max);

} // namespace core::doc
