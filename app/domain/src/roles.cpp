#include "roles.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include "edit.hpp"

namespace core::doc {

const std::vector<RoleInfo>& roleCatalog() {
  // Words: what the parameter is usually called (Valhalla, FabFilter, Serum's FX, TAL, Airwindows, Dragonfly...).
  static const std::vector<RoleInfo> roles = {
      {"reverb", "Reverb", false, "Space: the sound in a room. SUCK and the build-ups open it towards the drop",
       {{"mix", "Mix", 0.0, {"mix", "dry/wet", "wet", "blend"}},
        {"size", "Size / decay", 0.5, {"decay", "size", "room size", "time", "length"}}}},
      {"delay", "Delay", false, "Echoes. The build-ups let them grow before the drop",
       {{"mix", "Mix", 0.0, {"mix", "dry/wet", "wet", "blend"}},
        {"feedback", "Feedback", 0.35, {"feedback", "fdbk", "regen", "repeats"}}}},
      {"filter", "Filter", false, "Darker or brighter. The build-ups open it, SUCK closes it",
       {{"cutoff", "Cutoff", 1.0, {"cutoff", "freq", "frequency", "cut"}},
        {"resonance", "Resonance", 0.1, {"resonance", "reso", "res", "q"}}}},
      {"sidechain", "Sidechain / volume", false, "The pump: PUM dips its volume at every kick",
       {{"volume", "Volume", 1.0, {"volume", "gain", "level", "output", "vol"}},
        {"depth", "Depth", 0.0, {"depth", "amount", "mix", "intensity"}}}},
      {"drive", "Drive", false, "Grit and weight. The build-ups can push it",
       {{"amount", "Amount", 0.0, {"drive", "amount", "saturation", "gain", "dist"}},
        {"mix", "Mix", 1.0, {"mix", "dry/wet", "wet", "blend"}}}},
      {"bass", "Bass", true, "The bass line", {}},
      {"lead", "Lead", true, "Leads and melodies", {}},
      {"pad", "Pad", true, "Pads and atmospheres", {}},
      {"pluck", "Pluck", true, "Short plucked notes, arps", {}},
      {"drums", "Drums", true, "Kick, snare, hats: a drum plugin played by notes", {}},
  };
  return roles;
}

const RoleInfo* findRole(const std::string& id) {
  for (const auto& r : roleCatalog()) if (r.id == id) return &r;
  return nullptr;
}

const RoleMacro* findRoleMacro(const RoleInfo& role, const std::string& macro) {
  for (const auto& m : role.macros) if (m.id == macro) return &m;
  return nullptr;
}

const RoleBinding* RoleMap::find(const std::string& role) const {
  auto it = roles.find(role);
  if (it == roles.end() || !it->second.plugin.is_object() || !it->second.plugin.contains("id")) return nullptr;
  return &it->second;
}

json roleMapToJson(const RoleMap& map) {
  json roles = json::object();
  for (const auto& [id, b] : map.roles) {
    json r = {{"plugin", b.plugin}, {"params", json::object()}};
    for (const auto& [m, p] : b.params) r["params"][m] = p;
    if (!b.rest.empty()) {
      r["rest"] = json::object();
      for (const auto& [m, v] : b.rest) r["rest"][m] = v;
    }
    roles[id] = std::move(r);
  }
  return {{"version", 1}, {"roles", roles}};
}

RoleMap roleMapFromJson(const json& j) {
  if (!j.is_object()) throw DocumentError("roles", "must be an object");
  RoleMap map;
  if (!j.contains("roles")) return map;
  if (!j["roles"].is_object()) throw DocumentError("roles.roles", "must be an object");
  for (const auto& [id, r] : j["roles"].items()) {
    const std::string where = "roles.roles." + id;
    if (!r.is_object()) throw DocumentError(where, "must be an object");
    RoleBinding b;
    if (r.contains("plugin") && !r["plugin"].is_null()) {
      const json& p = r["plugin"];
      if (!p.is_object() || !p.contains("id") || !p["id"].is_string() || !p.contains("file") || !p["file"].is_string())
        throw DocumentError(where + ".plugin", "needs its id and its file");
      b.plugin = p;
    }
    if (r.contains("params")) {
      if (!r["params"].is_object()) throw DocumentError(where + ".params", "must be an object");
      for (const auto& [m, v] : r["params"].items()) {
        if (!v.is_string()) throw DocumentError(where + ".params." + m, "must be a parameter id");
        b.params[m] = v.get<std::string>();
      }
    }
    if (r.contains("rest")) {
      if (!r["rest"].is_object()) throw DocumentError(where + ".rest", "must be an object");
      for (const auto& [m, v] : r["rest"].items()) {
        if (!v.is_number()) throw DocumentError(where + ".rest." + m, "must be a number");
        b.rest[m] = std::clamp(v.get<double>(), 0.0, 1.0);
      }
    }
    map.roles[id] = std::move(b);
  }
  return map;
}

RoleMap loadRoleMap(const std::filesystem::path& file, std::string* problem) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return {};
  try {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot be read");
    std::stringstream ss;
    ss << in.rdbuf();
    return roleMapFromJson(json::parse(ss.str()));
  } catch (const std::exception& e) {
    if (problem) *problem = file.filename().string() + ": " + e.what();
    return {};
  }
}

void saveRoleMap(const RoleMap& map, const std::filesystem::path& file) {
  std::filesystem::create_directories(file.parent_path());
  std::filesystem::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw DocumentError("roles", "cannot write " + tmp.string());
    out << roleMapToJson(map).dump(2) << '\n';
    if (!out) throw DocumentError("roles", "cannot write " + tmp.string());
  }
  std::filesystem::rename(tmp, file);
}

namespace {
std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
// A whole word (or words) inside the name: "mix" is in "Dry/Wet Mix" but "res" is not in "Pre-Delay Reset".
bool hasWord(const std::string& name, const std::string& word) {
  for (size_t at = name.find(word); at != std::string::npos; at = name.find(word, at + 1)) {
    const bool startOk = at == 0 || !std::isalnum((unsigned char)name[at - 1]);
    const size_t end = at + word.size();
    const bool endOk = end == name.size() || !std::isalnum((unsigned char)name[end]);
    if (startOk && endOk) return true;
  }
  return false;
}
}  // namespace

RoleBinding guessRoleBinding(const std::string& role, const json& plugin, const json& params) {
  RoleBinding b;
  b.plugin = plugin;
  const RoleInfo* info = findRole(role);
  if (!info || !params.is_array()) return b;
  std::vector<std::string> taken;
  for (const auto& macro : info->macros)
    for (const auto& word : macro.words) {
      const json* best = nullptr;
      for (const auto& p : params) {
        if (!p.is_object() || !p.contains("id") || !p.contains("name")) continue;
        const std::string id = p["id"].is_string() ? p["id"].get<std::string>() : p["id"].dump();
        if (std::find(taken.begin(), taken.end(), id) != taken.end()) continue;
        if (hasWord(lower(p.value("name", std::string())), word)) { best = &p; break; }
      }
      if (best) {
        const std::string id = (*best)["id"].is_string() ? (*best)["id"].get<std::string>() : (*best)["id"].dump();
        b.params[macro.id] = id;
        taken.push_back(id);
        break;
      }
    }
  return b;
}

double roleRest(const RoleBinding& b, const std::string& role, const std::string& macro) {
  auto it = b.rest.find(macro);
  if (it != b.rest.end()) return it->second;
  if (const RoleInfo* info = findRole(role))
    if (const RoleMacro* m = findRoleMacro(*info, macro)) return m->rest;
  return 0;
}

namespace {
const RoleBinding& bindingFor(const RoleMap& map, const std::string& role, bool instrument) {
  const RoleInfo* info = findRole(role);
  if (!info) throw DocumentError("role", "no role '" + role + "'");
  if (info->instrument != instrument)
    throw DocumentError("role", "'" + info->label + "' is " + (info->instrument ? "an instrument" : "an effect") + " role");
  const RoleBinding* b = map.find(role);
  if (!b) throw DocumentError("role", "no plugin is chosen for '" + info->label + "' (Settings > Roles)");
  return *b;
}
bool samePlugin(const json& a, const json& b) {
  return a.is_object() && b.is_object() && a.value("id", std::string()) == b.value("id", std::string()) &&
         a.value("file", std::string()) == b.value("file", std::string());
}
}  // namespace

int findRoleEffect(const Document& doc, const std::string& trackId, const std::string& role) {
  const json& chain = effectChain(doc, trackId);
  for (size_t i = 0; i < chain.size(); ++i)
    if (chain[i].is_object() && chain[i].value("type", std::string()) == "plugin" && chain[i].value("role", std::string()) == role) return (int)i;
  return -1;
}

int roleEffectSlot(const Document& doc, const std::string& trackId, const std::string& role, const RoleMap& map) {
  const RoleBinding& b = bindingFor(map, role, false);
  const json& chain = effectChain(doc, trackId);
  int at = findRoleEffect(doc, trackId, role);
  if (at < 0)  // the same plugin put there by hand plays the role too
    for (size_t i = 0; i < chain.size(); ++i)
      if (chain[i].is_object() && chain[i].value("type", std::string()) == "plugin" && !chain[i].contains("role") &&
          samePlugin(chain[i].value("plugin", json::object()), b.plugin)) { at = (int)i; break; }
  return at;  // -1: playing this role takes a new slot on the chain
}

std::string ensureRoleEffect(Document& doc, const std::string& trackId, const std::string& role, const RoleMap& map) {
  const RoleBinding& b = bindingFor(map, role, false);
  int at = roleEffectSlot(doc, trackId, role, map);
  if (at < 0) at = addPluginEffect(doc, trackId, b.plugin);
  json& fx = effectChain(doc, trackId)[(size_t)at];
  fx["role"] = role;
  return fx["id"].get<std::string>();
}

std::string roleParamKey(const RoleMap& map, const std::string& role, const std::string& effectId, const std::string& macro) {
  const RoleBinding& b = bindingFor(map, role, false);
  auto it = b.params.find(macro);
  if (it == b.params.end() || it->second.empty()) {
    const RoleInfo* info = findRole(role);
    const RoleMacro* m = info ? findRoleMacro(*info, macro) : nullptr;
    throw DocumentError("role", "'" + (info ? info->label : role) + "' has no parameter for " + (m ? m->label : macro) + " (Settings > Roles)");
  }
  return "fx:" + effectId + ":vst:" + it->second;
}

std::string addRoleTrack(Document& doc, const std::string& role, const RoleMap& map) {
  const RoleBinding& b = bindingFor(map, role, true);
  const std::string id = addPluginTrack(doc, b.plugin);
  trackById(doc, id).name = findRole(role)->label;
  return id;
}

void setTrackRole(Document& doc, const std::string& trackId, const std::string& role, const RoleMap& map) {
  replaceInstrumentWithPlugin(doc, trackId, bindingFor(map, role, true).plugin);
}

std::string guessInstrumentRole(const Track& t) {
  std::string name;
  for (char c : t.name) name += (char)std::tolower((unsigned char)c);
  const auto has = [&name](std::initializer_list<const char*> words) {
    for (const char* w : words) if (name.find(w) != std::string::npos) return true;
    return false;
  };
  // The name says the part first (a W-Core track called "Bass" is the bass); then the old sound.
  if (has({"kick", "snare", "hat", "clap", "drum", "perc", "rim", "tom", "cymbal", "ride", "crash", "shaker", "beat"})) return "drums";
  if (has({"bass", "sub", "808"})) return "bass";
  if (has({"pad", "atmo", "drone", "string", "chord"})) return "pad";
  if (has({"pluck", "arp"})) return "pluck";
  if (has({"lead", "melod", "synth", "keys", "piano", "hook"})) return "lead";
  if (t.instrument.kind == "builtin") {
    const std::string& n = t.instrument.name;
    if (n == "kick" || n == "hats" || n == "snare" || n == "drums" || n == "sampler") return "drums";
    if (n == "wave") return "lead";
  }
  return "";
}

int giveSilentTracksRolePlugins(Document& doc, const RoleMap& map, std::vector<std::string>* left) {
  int changed = 0;
  std::vector<std::pair<std::string, std::string>> todo;  // track id, role: decided first, then changed
  for (const auto& t : doc.tracks) {
    if (t.instrument.kind != "builtin" && t.instrument.kind != "none") continue;
    const std::string role = guessInstrumentRole(t);
    if (!role.empty() && map.find(role)) todo.emplace_back(t.id, role);
    else if (left) left->push_back(t.name);
  }
  for (const auto& [id, role] : todo) { setTrackRole(doc, id, role, map); ++changed; }
  return changed;
}

}  // namespace core::doc
