// The role map (Diogo, 2026-11-16): Core's sound comes from plugins, and the tools and the AI never name a plugin.
// They ask for a role ("reverb", "filter", "bass"...) and Settings > Roles says, once, which plugin plays it on this
// computer (reverb -> ValhallaRoom, bass -> Serum 2...). An effect role also says which of the plugin's parameters
// are its macros (the reverb's mix and size, the filter's cutoff...): those are what the tools automate.
//
// Kept in %APPDATA%/Core/roles.json, a file of the program (not of a song): a song only holds the plugins the tools
// put in it, as ordinary plugin effects tagged with their role ({"role": "reverb"}), so it opens on any computer.
// No JUCE here: the tools (role_tools.hpp), the AI and the tests all use these functions.
#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// ---- the roles Core knows
struct RoleMacro {
  std::string id, label;
  double rest = 0;                  // the value (0..1) it sits at where no tool moves it (a reverb's mix: dry)
  std::vector<std::string> words;   // parameter names it is guessed from, in order of preference (lower case)
};
struct RoleInfo {
  std::string id, label;
  bool instrument = false;          // an instrument role is played by a track; an effect role goes on a chain
  std::string what;                 // what it is for, in plain words
  std::vector<RoleMacro> macros;    // effect roles only
};
const std::vector<RoleInfo>& roleCatalog();  // effects first, in menu order
const RoleInfo* findRole(const std::string& id);
const RoleMacro* findRoleMacro(const RoleInfo& role, const std::string& macro);

// ---- what this computer plays each role with
struct RoleBinding {
  json plugin;                                  // what the scan found {format, id, name, vendor, version, file, ...}
  std::map<std::string, std::string> params;    // macro -> the plugin's parameter id (VST3: its ParamID as text)
  std::map<std::string, double> rest;           // macro -> its value where no tool moves it (absent: the role's default)
};
struct RoleMap {
  std::map<std::string, RoleBinding> roles;     // role id -> binding (an unknown role is kept, unused)
  const RoleBinding* find(const std::string& role) const;
};
json roleMapToJson(const RoleMap& map);
RoleMap roleMapFromJson(const json& j);         // throws DocumentError on a malformed file
// The file: missing = an empty map; unreadable or malformed: an empty map and `problem` set (never throws).
RoleMap loadRoleMap(const std::filesystem::path& file, std::string* problem = nullptr);
void saveRoleMap(const RoleMap& map, const std::filesystem::path& file);  // atomically (temp file + rename)

// A binding for `plugin` on `role`, its macros guessed from the plugin's parameters [{id, name}] (what the engine's
// "params" answer gives): each macro takes the first parameter whose name holds one of its words. A macro with no
// match is left out (chosen by hand in Settings).
RoleBinding guessRoleBinding(const std::string& role, const json& plugin, const json& params);
double roleRest(const RoleBinding& b, const std::string& role, const std::string& macro);

// ---- roles in a song
// The plugin effect of `role` on the track's chain (trackId "" = the master): the one tagged with the role, else
// an effect of the same plugin. -1 when there is none.
int findRoleEffect(const Document& doc, const std::string& trackId, const std::string& role);
// The chain slot that plays `role` the same way ensureRoleEffect would find it: the effect tagged with the role, or
// the role's plugin already on the chain put there by hand. -1 when the role would need a NEW effect, so a caller
// that is about to write can check the chain has room before it writes anything (audit 2026-11-17, #5). Throws
// DocumentError for a role that is not mapped or is an instrument role, exactly as ensureRoleEffect does.
int roleEffectSlot(const Document& doc, const std::string& trackId, const std::string& role, const RoleMap& map);
// The role's effect on the track: the one there, or the role's plugin added at the end of the chain (tagged with the
// role). Returns the effect's id. Throws DocumentError when the role is not mapped, is an instrument role, or the
// chain is full.
std::string ensureRoleEffect(Document& doc, const std::string& trackId, const std::string& role, const RoleMap& map);
// The automation key of a macro of the role's effect: "fx:<effect id>:vst:<param id>". Throws DocumentError when
// the macro has no parameter in the map.
std::string roleParamKey(const RoleMap& map, const std::string& role, const std::string& effectId, const std::string& macro);
// A new track played by the role's plugin, named after the role ("Bass"). Returns its id. Throws DocumentError when
// the role is not mapped or is not an instrument role.
std::string addRoleTrack(Document& doc, const std::string& role, const RoleMap& map);
// The track plays the role's plugin (its notes stay). Throws as addRoleTrack.
void setTrackRole(Document& doc, const std::string& trackId, const std::string& role, const RoleMap& map);

// ---- silent tracks (Diogo 2026-11-16, "não sai som"): a song or template from before 0.7.0 keeps its old Core
// sounds ("builtin") or has none, and 0.7.0 plays neither. The instrument role a track plays, guessed from its old
// sound and its name (kick, hats, snare, drums -> drums; bass; pad; pluck/arp; lead): "" when nothing fits.
std::string guessInstrumentRole(const Track& t);
// Each silent track (kind "builtin" or "none", not audio or a bus) whose guessed role has a plugin in the map plays
// that plugin; its notes stay. Returns how many changed; `left` gets the names of the silent tracks that stayed so.
int giveSilentTracksRolePlugins(Document& doc, const RoleMap& map, std::vector<std::string>* left = nullptr);

}  // namespace core::doc
