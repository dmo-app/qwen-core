// The user's sample library (D:\Samples by default): an index of its WAV files, what each one is for
// (kick, bass, hats...), and the samples that suit a track of a style template. No GUI, no audio.
// The index is rebuilt by a scan; files whose size and time did not change keep what was read before.
// What the user says about a sample (its role, BPM, key, styles, tags) is kept in the index by content and
// survives every scan; the WAV files are never written.
// Contract: docs/contracts/core-samples.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

constexpr const char* kSampleIndexFormat = "core-sample-index";
constexpr int kSampleIndexVersion = 2;  // 1 is still read (it had no catalogue)
constexpr const char* kSampleMetadataFormat = "core-sample-metadata";
constexpr const char* kSampleMetadataFile = "core-sample-metadata.json";  // in the library folder, optional

constexpr double kNotMeasured = -999;
// A bass one-shot that sounds longer than this (measured) is a held note: the sampler gives it a pluck that
// falls to -60 dB in kBassPluckMs (samplerInstrument), so a 1/16 line does not sound like an organ.
constexpr double kLongBassMs = 300;
constexpr double kBassPluckMs = 400;
// A drum hit or a bass one-shot with this much silence (measured) before its sound starts that late in the sampler.
constexpr double kLateStartMs = 10;
// Every library sample plays at this peak (the sampler's Level is set from the measured peak when the sample is
// picked): the files themselves are not changed. At most +18 dB for a very quiet file.
constexpr double kSamplePeakDb = -3, kSampleMaxBoostDb = 18;

// What a sample is for: read from its name and folders, then corrected by the metadata file and by the user.
//   kick bass hat openhat snare clap perc tom crash impact riser fx vox pad synth orchestral fill kickbass other
struct SampleInfo {
  std::string role = "other";
  bool loop = false;       // a loop or fill (plays a bar or more), not a single hit
  double bpm = 0;          // from the name or the pack ("(Em 175 BPM)"); 0 = not known
  int keyClass = -1;       // 0 = C .. 11 = B; -1 = not known
  bool minor = false;
  int rootKey = -1;        // MIDI note when the name has a note and octave ("C1" = 24, "G# 3" = 56); -1 otherwise
  std::vector<std::string> styles;  // lower case ("darkpsycore", "hitech"...); only from the metadata file or the user
  std::vector<std::string> tags;    // lower case, free
  std::string name;                 // a name the user (or the metadata file) gave it; "" = Core makes one (displayName)
};
// The same rules the scan uses, on a path relative to the library ('/' separated).
SampleInfo classifySample(const std::string& relativePath);

struct LibrarySample {
  std::string file;        // absolute, UTF-8
  std::string path;        // relative to the library, '/' separated
  std::string name;        // the file name without ".wav"
  std::string pack;        // the first folder (the sample pack)
  std::string sha256;
  int64_t size = 0, mtime = 0;
  // The hash of its first 64 KiB (audit 2026-11-17, #3a): size and mtime do not say a file was written again
  // with the same length at the same second, this does, and it costs one short read instead of the whole file.
  std::string headSha256;
  double seconds = 0, sampleRate = 0;
  int channels = 0;
  int duplicates = 0;      // other files with the same content (one entry: the first path)
  std::vector<std::string> otherPaths;  // where those are (relative, sorted)
  // How loud it is (the first minute): its peak, and the loudest 100 ms (RMS of both sides), in dBFS.
  // kNotMeasured until a scan reads the audio. The sampler's level is set from loudDb (levelFor).
  double peakDb = kNotMeasured, loudDb = kNotMeasured;
  SampleInfo inferred;     // what the name and folders say (classifySample)
  SampleInfo info;         // what Core uses: inferred, then the metadata file, then the user (resolveCatalog)
  json sidecar;            // what the library's metadata file says about it (an object of fields; null: nothing)
  // What the file says about itself (its RIFF chunks, read only; fileMetadata): {"fields": catalogue fields,
  // "info": {"ISBJ": ...}, "loop": [start, end] seconds}. null: not read yet (an index from an older Core).
  json embedded;
  json analysis;           // what its audio measures (sample_analysis.hpp, analysisToJson); null until measured
  // The name Core shows (resolveCatalog): info.name, or one made from what is known ("Kick · Hi-tech · Short · F#1"),
  // numbered (by content) when two samples would read the same; the file's own name when nothing is known.
  std::string displayName;
};
// True when the sample has no measurements of this analyzer version for its current content.
bool needsAnalysis(const LibrarySample& sample);
// The words its measurements give ("short", "bright", "sub"...; sample_analysis.hpp), none when not measured.
std::vector<std::string> soundWords(const LibrarySample& sample);

// What a file's own chunks (assets.hpp, readWavTags) give the catalogue: keywords (IKEY) and the subject
// (ISBJ, usually the collection) as tags, a role when a keyword is one, an ACID tempo, kind and root note.
// They fill in what the name does not know (the role only when the name gives none; an ACID tempo wins over a
// number in the name), under the metadata file and the user. The smpl loop becomes the sampler's loop.
json fileMetadata(const std::filesystem::path& file, double sampleRate);

// The fields of the catalogue, as JSON (get/set, the metadata file, the index):
//   role   one of the roles above (aliases: closed-hat, open-hat, ride...)
//   kind   "one-shot" | "loop"
//   bpm    a tempo 40..999; 0 = it has none (a number in the name that is not one)
//   key    "F#m", "C", "E1" (with an octave: the root note of a pitched sample); "" = no key
//   styles, tags  arrays of words
//   name   the name to show instead of the generated one (1..80 characters; "" or null: generated)
// In an edit, null takes the user's value away (the name or the metadata file decide again).
extern const std::vector<std::string> kSampleMetadataFields;
// Checked and written the one way Core stores them; throws DocumentError (field: why).
json normalizeSampleMetadata(const json& fields);

// One catalogue edit, for its Undo: the user's fields of each sample (by content) before it.
struct CatalogEdit {
  std::string label;
  std::vector<std::pair<std::string, json>> before;  // sha256 -> overrides before (null: none)
};
constexpr size_t kCatalogUndoSteps = 100;

struct SampleIndex {
  std::string root;        // the library folder, absolute, UTF-8
  std::vector<LibrarySample> samples;  // one per content, sorted by path
  std::vector<std::string> unreadable; // "path: why" (not WAV, damaged...)
  // The user's corrections, by content (sha256 -> object of fields): kept for files that are gone too, so
  // a sample that comes back (a disk plugged in again) has them.
  std::map<std::string, json> overrides;
  std::vector<CatalogEdit> undo;       // this session's catalogue edits (not saved), the last one last
  // What the AI thinks a sample is, without changing what Core uses (sha256 -> {fields, why}): a proposal for the
  // user to accept or reject, never a correction by itself. Kept by content, like overrides.
  std::map<std::string, json> proposals;
  std::vector<std::string> sidecarProblems;  // what in the metadata file could not be used
  // The user's folders for a track role (File > Settings > Samplers): role ("kick", "bass", "hat"...) -> folders
  // relative to root ('/' separated). A role with folders takes its suggestions only from them, and every sample
  // in them counts as that role. Set by the app from its settings; not part of the saved index.
  std::map<std::string, std::vector<std::string>> roleFolders;
  const LibrarySample* findFile(const std::string& file) const;
  const LibrarySample* findHash(const std::string& sha256) const;
  LibrarySample* findHash(const std::string& sha256);
  // A sample by its path in the library (or one of its other paths), its file, or its sha256.
  const LibrarySample* find(const std::string& pathFileOrHash) const;
};

// The catalogue's roles (the list above) and the other names it takes for them ("hihat" -> "hat").
const std::vector<std::string>& sampleRoles();
const std::map<std::string, std::string>& sampleRoleAliases();
// "Kick", "Open hat", "Kick & bass"... ("Other" for other or an unknown role): folder and menu names.
std::string sampleRoleLabel(const std::string& role);
// A role or an alias as the catalogue's role; "" when it is neither.
std::string canonicalSampleRole(const std::string& role);

// The AI's proposals (SampleIndex::proposals): fields checked like setSampleMetadata's, with why. A new proposal for
// a sample replaces the old one. Returns how many samples got one. Throws DocumentError (nothing changed).
int proposeSampleMetadata(SampleIndex& index, const std::vector<std::string>& sha256s, const json& fields, const std::string& why);

// Every sample's info from its layers (the user's value, else the metadata file's, else the name's), then its
// displayName. Call it again when measurements arrive (the names use them).
void resolveCatalog(SampleIndex& index);
// The name Core makes for a sample, before numbering: role, loop, styles, tags, short/long (measured), key, a loop's
// BPM, joined by " · ". "" when nothing is known (role other, no styles or tags): the file's name is used then.
std::string generatedName(const LibrarySample& sample);
// Where a field's value comes from: "user", "metadata file", "name" or "" (not known).
std::string metadataOrigin(const SampleIndex& index, const LibrarySample& sample, const std::string& field);
// The fields of an info as JSON (role, kind, bpm, key, styles, tags).
json sampleInfoJson(const SampleInfo& info);
// One sample for the window and the AI: its effective fields, where each comes from, the layers, its locations.
json sampleMetadata(const SampleIndex& index, const LibrarySample& sample);

// The user sets fields of these samples (null clears one); one Undo step of the catalogue. Returns how many
// samples changed (0: nothing to undo). Throws DocumentError (nothing changed).
int setSampleMetadata(SampleIndex& index, const std::vector<std::string>& sha256s, const json& fields, const std::string& label);
// Takes the last catalogue edit back; returns its label ("" when there is none).
std::string undoCatalogEdit(SampleIndex& index);
// The library's metadata file (read by the scan): {format, version 1, items: [{sha256 | path, set}]}.
// Items are matched by content first, then by path; returns per sample (sha256) its fields. Problems go to
// `problems` (the rest of the file is used).
std::map<std::string, json> readSampleMetadataFile(const json& file, const SampleIndex& index, std::vector<std::string>& problems);

// One file: its format, content hash and what its name says (relative to root when it is inside it).
// Throws DocumentError when it is not a WAV Core can play.
LibrarySample describeSample(const std::filesystem::path& file, const std::filesystem::path& root);

// Reads every .wav under root. `previous` (may be null) saves re-reading unchanged files, and its catalogue
// (overrides, undo) carries on. The library's metadata file is read too.
// progress(done, total) is called now and then; returning false stops the scan (throws DocumentError).
SampleIndex scanLibrary(const std::filesystem::path& root, const SampleIndex* previous = nullptr,
                        const std::function<bool(int, int)>& progress = {});
// One new file of the library (a sample Core just wrote there): read and put in the index (in path order, as a
// scan would), its catalogue resolved. A file whose content is already indexed becomes one of its other paths.
// Returns the sample (its content's entry). Throws DocumentError (not a WAV, outside the library).
const LibrarySample& addLibraryFile(SampleIndex& index, const std::filesystem::path& file);
json indexToJson(const SampleIndex& index);
SampleIndex indexFromJson(const json& value);  // throws DocumentError

// ---- suggestions for the tracks of a song (a style template)

// The role a track plays, from its id and name ("kick", "open-hats", "Impacts"...): the sample role it
// wants, or "" when samples do not fit it (synths, pads: they keep their instrument).
std::string trackSampleRole(const Track& track);

// ---- fit: does a sample suit a track at the song's tempo (the Library's "fits" filter)

struct SampleFit {
  bool fits = false;   // the role suits the track and nothing measured speaks against it
  bool known = true;   // false: it cannot be said yet (not measured, a loop without a BPM)
  std::string why;     // short, English ("ends in 140 ms, a beat at 225 BPM is 267 ms")
};
// The fastest tempo of a song (its start or any tempo change): the one a sound has to fit.
double fastestTempo(const Document& doc);
// Whether a sample suits a track of this role (trackSampleRole) at this tempo. The sample's role must suit the
// track (kick for kick, clap or snare for accents...). A one-shot of a drum role must fade (to 40 dB under its
// peak, from its start) before the next beat (a kick's tail never runs into the next kick), a hat within half
// a beat; several hits do not fit. A loop must be at the tempo, half or double (2%).
// Bass one-shots (cut at note-off), impacts and risers: the role is enough.
SampleFit sampleFit(const LibrarySample& sample, const std::string& trackRole, double bpm);
constexpr double kFitTempoTolerance = 0.02;

struct SampleSuggestion {
  const LibrarySample* sample = nullptr;
  double score = 0;        // higher is better; under kGoodSuggestion it is not used by itself
  std::string why;         // short, English (shown in the window)
};
constexpr double kGoodSuggestion = 50;
// The best samples of the index for a track role, best first. style: the template's style ("Dark
// psycore", "Hi-tech", "Slambient", "Psytrance"), or "". Deterministic: same index, same answer.
// pack: the pack the song's bass comes from (anchorPack), or "": its samples get a small bonus, so kick
// and bass come from the same pack when both fit (KBBB: they are made to sit together).
std::vector<SampleSuggestion> suggestSamples(const SampleIndex& index, const std::string& role, const std::string& style,
                                             int limit = 5, const std::string& pack = "");
// The track roles a sampler plays (trackSampleRole), in the order the settings show them.
const std::vector<std::string>& samplerRoles();
// The kinds a plain Sampler track (no role in its name) offers, one submenu each: samplerRoles, then fx,
// synth, pad and vox (loops welcome there).
const std::vector<std::string>& browseSampleRoles();
// Whether a sample is in one of the folders the user gave this role (a folder of its own path or of a duplicate's).
// false when the role has none.
bool inRoleFolders(const SampleIndex& index, const LibrarySample& sample, const std::string& role);
// A folder (absolute, UTF-8) as the index's roleFolders keep it: relative to the library, '/' separated; "" when it is
// not inside the library (or is the library itself).
std::string libraryRelativeFolder(const SampleIndex& index, const std::string& folder);
constexpr double kSamePackBonus = 8;
// The pack of the library sample the song's bass track plays ("" when the bass is not a library sample).
std::string anchorPack(const Document& doc, const SampleIndex& index);

// ---- the real notes of a region: does a one-shot of this track end before the next note it must leave room for

struct RegionFit {
  bool known = false;      // false: not measured, no notes, or nothing to check against (why says which)
  bool fits = false;       // no hit of the track runs into the next note of the tracks it leaves room for
  int hits = 0;            // the track's notes in the region
  int overlaps = 0;        // of which run into the next note
  double worstGapMs = -1;  // the shortest room a hit had (ms), -1 when none
  double soundMs = -1;     // how long the sample sounds (attack + tail)
  std::string against;     // the tracks it was checked against ("bass")
  std::string why;
};
// For a kick (or kickbass) track: each kick note in [startTick, endTick) against the next note of the bass tracks
// (trackSampleRole "bass"), at the tempo where it plays (the tempo map). Other roles: known = false.
RegionFit sampleFitInRegion(const Document& doc, const std::string& trackId, const LibrarySample& sample, int64_t startTick, int64_t endTick);

// ---- hearing a sample without putting it on a track

// The document with one more track, "sample-preview", that plays this sample (Core Sampler, its role's level)
// and has no clips: it sounds only from live notes. pitch: the key to play it at (its root for a pitched bass).
// For the engine only; never committed.
Document withPreviewTrack(const Document& doc, const SampleIndex& index, const LibrarySample& sample, const std::string& role,
                          std::string& trackId, int& pitch);

// Peak and loudest 100 ms of a WAV file (dBFS, the first minute). Throws DocumentError.
void measureLoudness(const std::filesystem::path& file, double& peakDb, double& loudDb);
// The loudest 100 ms a track of that role aims at (dBFS), so swapping samples keeps the balance of the
// template: a kick hits harder than the hats, an atmosphere sits under everything.
double roleLoudnessTarget(const std::string& role);
// The sampler level (dB, -24..+24) that brings this sample to its role's target; 0 when not measured.
double levelFor(const LibrarySample& sample, const std::string& role);

// The Core Sampler that plays this sample on a track of that role: a one-shot on every key, or for a
// bass recorded note by note (same folder, names that differ only in the note) every note as a zone.
Instrument samplerInstrument(const SampleIndex& index, const LibrarySample& sample, const std::string& role);
// The name Core shows for a sample of the library (its catalogue name), else its file name.
std::string shownName(const LibrarySample& sample);
// The same for a sound a track plays: the library's name for its content (or its file), else the name kept with it.
std::string shownName(const SampleIndex& index, const SampleRef& zone);
SampleRef sampleRef(const LibrarySample& sample);

// Gives a note track the sampler for this sample (one undo step for the caller). The track's notes,
// mixer, effects and their automation stay; the old instrument's knobs and its own automation go (a new
// sample starts from its own sound, the same however many samples were tried before).
void setTrackSample(Document& doc, const std::string& trackId, const SampleIndex& index, const LibrarySample& sample);

// The style of a song made from a style template ("" otherwise).
std::string songStyle(const Document& doc);

// A sample for every track that wants one (score >= kGoodSuggestion), applied to doc; no sample goes
// on two tracks. seed 0: the best one. Another seed: any good one within kVarietyRange of the best, the
// better ones more often (same seed, same picks), so every new song sounds different. Tracks with no
// good match keep their instrument. Returns "<track name>: <sample name>" per change.
constexpr double kVarietyRange = 25;
std::vector<std::string> applySuggestedSamples(Document& doc, const SampleIndex& index, const std::string& style, uint64_t seed = 0);

// Samples of the document whose file is gone: found again by content in the index when it moved
// (returns how many were repointed); the others stay as they are (reported as missing by the engine).
// One with no sha256 is found by its place inside the library (D:\Samples\Bass\x.wav -> <library>\Bass\x.wav).
int relinkMovedSamples(Document& doc, const SampleIndex& index);

} // namespace core::doc
