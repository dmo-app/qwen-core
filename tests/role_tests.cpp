// Testes do mapa de papeis (roles.hpp) e das tools que tocam pelos plugins dos papeis (role_tools.hpp):
// SUCK, PUM e os efeitos do build-up. Sem plugins verdadeiros: o dominio so guarda referencias e curvas.
#include <algorithm>
#include <filesystem>
#include <set>
#include "support/check.hpp"
#include "edit.hpp"
#include "follow.hpp"
#include "core_plugins.hpp"
#include "role_tools.hpp"
#include "roles.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
json fakePlugin(const std::string& name) {
  return {{"format", "vst3"}, {"id", "VST3-" + name + "-1234-abcd"}, {"name", name}, {"vendor", "Test"}, {"file", "C:/VST3/" + name + ".vst3"}};
}
// Valhalla-like, filter, a volume shaper, a delay: their parameters as the engine's "params" answer lists them.
json reverbParams() { return json::array({{{"id", "0"}, {"name", "Mix"}}, {{"id", "1"}, {"name", "PreDelay"}}, {{"id", "2"}, {"name", "Decay"}}}); }
json filterParams() { return json::array({{{"id", "10"}, {"name", "Cutoff Frequency"}}, {{"id", "11"}, {"name", "Resonance"}}}); }
json shaperParams() { return json::array({{{"id", "7"}, {"name", "Output Volume"}}, {{"id", "8"}, {"name", "Depth"}}}); }
json delayParams() { return json::array({{{"id", "3"}, {"name", "Dry/Wet"}}, {{"id", "4"}, {"name", "Feedback"}}}); }

RoleMap fullMap() {
  RoleMap m;
  m.roles["reverb"] = guessRoleBinding("reverb", fakePlugin("ValhallaRoom"), reverbParams());
  m.roles["filter"] = guessRoleBinding("filter", fakePlugin("Filter"), filterParams());
  m.roles["sidechain"] = guessRoleBinding("sidechain", fakePlugin("Shaper"), shaperParams());
  m.roles["delay"] = guessRoleBinding("delay", fakePlugin("ValhallaDelay"), delayParams());
  m.roles["bass"] = {fakePlugin("Serum2"), {}, {}};
  return m;
}
std::string trackNamed(const Document& d, const std::string& name) {
  for (const auto& t : d.tracks) if (t.name.find(name) != std::string::npos) return t.id;
  return "";
}
const Automation* curve(const Document& d, const std::string& track, const std::string& key) { return findAutomation(d, track, key); }
}  // namespace

TEST("papeis: o catalogo tem efeitos com macros e instrumentos sem macros") {
  CHECK(findRole("reverb") && !findRole("reverb")->instrument && !findRole("reverb")->macros.empty());
  CHECK(findRole("bass") && findRole("bass")->instrument && findRole("bass")->macros.empty());
  CHECK(findRole("nada") == nullptr);
}

TEST("papeis: as macros adivinham-se pelos nomes dos parametros, palavras inteiras") {
  const RoleBinding r = guessRoleBinding("reverb", fakePlugin("ValhallaRoom"), reverbParams());
  CHECK_EQ(r.params.at("mix"), std::string("0"));
  CHECK_EQ(r.params.at("size"), std::string("2"));
  const RoleBinding f = guessRoleBinding("filter", fakePlugin("Filter"), filterParams());
  CHECK_EQ(f.params.at("cutoff"), std::string("10"));
  CHECK_EQ(f.params.at("resonance"), std::string("11"));
  // "res" nao apanha "Reset", e um parametro so serve uma macro.
  const RoleBinding g = guessRoleBinding("filter", fakePlugin("X"), json::array({{{"id", "1"}, {"name", "Reset"}}, {{"id", "2"}, {"name", "Freq"}}}));
  CHECK(!g.params.count("resonance"));
  CHECK_EQ(g.params.at("cutoff"), std::string("2"));
}

TEST("papeis: o ficheiro guarda e reabre igual; ficheiro estragado da mapa vazio e diz porque") {
  const fs::path dir = fs::temp_directory_path() / "core-role-tests";
  fs::create_directories(dir);
  const fs::path file = dir / "roles.json";
  fs::remove(file);
  CHECK(loadRoleMap(file).roles.empty());  // nao existe: vazio, sem erro
  RoleMap m = fullMap();
  m.roles["reverb"].rest["size"] = 0.3;
  saveRoleMap(m, file);
  const RoleMap back = loadRoleMap(file);
  CHECK_EQ(roleMapToJson(back).dump(), roleMapToJson(m).dump());
  CHECK_NEAR(roleRest(*back.find("reverb"), "reverb", "size"), 0.3, 1e-12);
  CHECK_NEAR(roleRest(*back.find("reverb"), "reverb", "mix"), 0.0, 1e-12);  // o valor do catalogo
  { std::FILE* f = std::fopen(file.string().c_str(), "wb"); std::fputs("{\"roles\": {\"reverb\": {\"plugin\": 3}}}", f); std::fclose(f); }
  std::string problem;
  CHECK(loadRoleMap(file, &problem).roles.empty());
  CHECK(!problem.empty());
}

TEST("papeis: ensureRoleEffect poe o plugin uma vez, marcado com o papel, e toca no motor") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  CHECK(!bass.empty());
  const RoleMap m = fullMap();
  const std::string a = ensureRoleEffect(d, bass, "reverb", m);
  const std::string b = ensureRoleEffect(d, bass, "reverb", m);
  CHECK_EQ(a, b);
  CHECK_EQ(effectChain(d, bass).size(), (size_t)1);
  CHECK_EQ(effectChain(d, bass)[0].value("role", std::string()), std::string("reverb"));
  CHECK_EQ(findRoleEffect(d, bass, "reverb"), 0);
  CHECK_EQ(roleParamKey(m, "reverb", a, "mix"), "fx:" + a + ":vst:0");
  // No master tambem.
  ensureRoleEffect(d, "", "reverb", m);
  CHECK_EQ(findRoleEffect(d, "", "reverb"), 0);
  // Guardado e reaberto, o efeito e o papel ficam; a projeccao leva o plugin ao motor.
  const Document again = parseDocument(toJson(d));
  CHECK_EQ(findRoleEffect(again, bass, "reverb"), 0);
  bool played = false;
  const json projection = toProjection(again);
  for (const auto& t : projection["tracks"])
    if (t.value("id", std::string()) == bass) played = t["effects"].size() == 1;
  CHECK(played);
}

TEST("papeis: um papel sem plugin, ou do tipo errado, e recusado sem mexer na musica") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  const std::string before = toJson(d).dump();
  RoleMap empty;
  CHECK_THROWS(ensureRoleEffect(d, bass, "reverb", empty));
  CHECK_THROWS(ensureRoleEffect(d, bass, "bass", fullMap()));  // instrumento
  CHECK_THROWS(addRoleTrack(d, "reverb", fullMap()));          // efeito
  CHECK_THROWS(toolSuck(d, bass, empty, 8 * 4 * kPpq, 4 * kPpq));
  CHECK_EQ(toJson(d).dump(), before);
}

TEST("papeis: addRoleTrack e setTrackRole poem o plugin do papel como instrumento") {
  Document d = newTemplateDocument();
  const RoleMap m = fullMap();
  const std::string id = addRoleTrack(d, "bass", m);
  CHECK_EQ(trackById(d, id).name, std::string("Bass"));
  CHECK_EQ(trackById(d, id).instrument.kind, std::string("plugin"));
  const std::string bass = trackNamed(d, "ass");
  setTrackRole(d, bass, "bass", m);
  CHECK_EQ(trackById(d, bass).instrument.plugin.value("name", std::string()), std::string("Serum2"));
  CHECK(!trackById(d, bass).patterns[0].notes.empty());  // as notas ficam
}

TEST("SUCK: o reverb abre ate ao drop e volta ao repouso no drop; o filtro fecha") {
  Document d = newTemplateDocument();
  d.lengthTicks = std::max<int64_t>(d.lengthTicks, 16 * 4 * kPpq);
  const std::string bass = trackNamed(d, "ass");
  const RoleMap m = fullMap();
  const int64_t drop = 8 * 4 * kPpq, len = 2 * 4 * kPpq;
  const RoleToolResult r = toolSuck(d, bass, m, drop, len, 1.0);
  CHECK_EQ(r.roles.size(), (size_t)2);
  CHECK(r.skipped.empty());
  const std::string fx = effectChain(d, bass)[(size_t)findRoleEffect(d, bass, "reverb")]["id"].get<std::string>();
  const Automation* mix = curve(d, bass, "fx:" + fx + ":vst:0");
  CHECK(mix != nullptr);
  CHECK_NEAR(automationValueAt(*mix, drop - len - 2 * kPpq), 0.0, 1e-9);  // antes: seco
  CHECK(automationValueAt(*mix, drop - 1) > 0.9);                          // mesmo antes do drop: aberto
  CHECK_NEAR(automationValueAt(*mix, drop), 0.0, 1e-9);                     // no drop: de volta
  CHECK(automationValueAt(*mix, drop - len / 2) < automationValueAt(*mix, drop - len / 4));  // sobe
  const std::string ffx = effectChain(d, bass)[(size_t)findRoleEffect(d, bass, "filter")]["id"].get<std::string>();
  const Automation* cut = curve(d, bass, "fx:" + ffx + ":vst:10");
  CHECK(cut != nullptr && automationValueAt(*cut, drop - 1) < 0.5 && automationValueAt(*cut, drop) > 0.99);
  // Sem filtro mapeado: so o reverb, e diz porque.
  RoleMap only;
  only.roles["reverb"] = m.roles.at("reverb");
  Document e = newTemplateDocument();
  e.lengthTicks = d.lengthTicks;
  const RoleToolResult r2 = toolSuck(e, bass, only, drop, len);
  CHECK_EQ(r2.roles.size(), (size_t)1);
  CHECK_EQ(r2.skipped.size(), (size_t)1);
  // Fazer outra vez nao poe outro reverb.
  toolSuck(d, bass, m, drop, len, 0.5);
  CHECK_EQ(effectChain(d, bass).size(), (size_t)2);
}

TEST("PUM: o volume do plugin de sidechain desce em cada nota do kick e volta antes da seguinte") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass"), kick = trackNamed(d, "ick");
  CHECK(!kick.empty());
  const RoleMap m = fullMap();
  const int64_t end = std::min<int64_t>(d.lengthTicks, 4 * 4 * kPpq);
  const std::vector<int64_t> onsets = songOnsets(d, kick, 0, end);
  CHECK(!onsets.empty());
  const RoleToolResult r = toolPump(d, bass, kick, m, 0, end, 0.75, kPpq / 2);
  CHECK_EQ(r.keys.size(), (size_t)1);
  const Automation* vol = curve(d, bass, r.keys[0]);
  CHECK(vol != nullptr);
  for (int64_t o : onsets) CHECK_NEAR(automationValueAt(*vol, o), 0.25, 1e-9);  // 1.0 * (1 - 0.75)
  if (onsets.size() > 1 && onsets[1] - onsets[0] > kPpq / 2) CHECK_NEAR(automationValueAt(*vol, onsets[0] + kPpq / 2), 1.0, 1e-9);
  CHECK_THROWS(toolPump(d, bass, bass, m, 0, end));  // de si propria nao
  // So "depth" mapeado: profundidade fixa no intervalo.
  RoleMap depthOnly;
  depthOnly.roles["sidechain"] = {fakePlugin("Kickstart"), {{"depth", "8"}}, {}};
  Document e = newTemplateDocument();
  const RoleToolResult r2 = toolPump(e, bass, kick, depthOnly, 0, end, 0.6);
  CHECK_NEAR(automationValueAt(*curve(e, bass, r2.keys[0]), kPpq), 0.6, 1e-9);
}

TEST("build-up: o filtro abre, o reverb e o delay crescem ate ao drop; papeis em falta sao saltados") {
  Document d = newTemplateDocument();
  d.lengthTicks = std::max<int64_t>(d.lengthTicks, 16 * 4 * kPpq);
  const std::string bass = trackNamed(d, "ass");
  RoleMap m = fullMap();
  m.roles.erase("delay");
  BuildupFx fx;
  fx.delay = true;
  const int64_t start = 4 * 4 * kPpq, drop = 8 * 4 * kPpq;
  const RoleToolResult r = toolBuildupFx(d, bass, m, start, drop, fx);
  CHECK_EQ(r.roles.size(), (size_t)2);    // filter, reverb
  CHECK_EQ(r.skipped.size(), (size_t)1);  // delay
  const std::string ffx = effectChain(d, bass)[(size_t)findRoleEffect(d, bass, "filter")]["id"].get<std::string>();
  const Automation* cut = curve(d, bass, "fx:" + ffx + ":vst:10");
  CHECK(automationValueAt(*cut, start) < automationValueAt(*cut, drop - 1));
  CHECK_NEAR(automationValueAt(*cut, drop), 1.0, 1e-9);
  BuildupFx none;
  none.filter = none.reverb = false;
  CHECK_THROWS(toolBuildupFx(d, bass, m, start, drop, none));
  CHECK_THROWS(toolBuildupFx(d, bass, m, drop, start, fx));  // ao contrario
}

CORE_TEST_MAIN

// "Não sai som" (Diogo 2026-11-16): uma musica de antes do 0.7.0 tem os sons antigos do Core (builtin) ou nenhum,
// e o 0.7.0 nao toca nenhum dos dois. giveSilentTracksRolePlugins da a cada pista muda o plugin do seu papel.
TEST("pistas mudas: o papel adivinha-se pelo nome e pelo som antigo") {
  Track t;
  t.name = "Kick"; t.instrument.kind = "builtin"; t.instrument.name = "kick";
  CHECK_EQ(guessInstrumentRole(t), std::string("drums"));
  t.name = "Hats 2"; CHECK_EQ(guessInstrumentRole(t), std::string("drums"));
  t.name = "Sub Bass"; t.instrument.name = "wave"; CHECK_EQ(guessInstrumentRole(t), std::string("bass"));
  t.name = "Big Pad"; CHECK_EQ(guessInstrumentRole(t), std::string("pad"));
  t.name = "Arp"; CHECK_EQ(guessInstrumentRole(t), std::string("pluck"));
  t.name = "Track 7"; CHECK_EQ(guessInstrumentRole(t), std::string("lead"));  // W-Core sem nome: lead
  t.instrument.kind = "none"; t.instrument.name.clear();
  CHECK_EQ(guessInstrumentRole(t), std::string(""));  // sem nome nem som antigo: nao se adivinha
}

TEST("pistas mudas: as builtin e none com papel no mapa passam a tocar o plugin; as outras ficam e sao listadas") {
  Document d = newTemplateDocument();
  // Como uma musica antiga: todas as pistas com o som antigo do Core.
  for (auto& t : d.tracks)
    if (playsPatterns(t)) { t.instrument = Instrument{}; t.instrument.kind = "builtin"; t.instrument.name = "kick"; }
  const std::string bass = trackNamed(d, "ass");
  const size_t notesBefore = trackById(d, bass).patterns.empty() ? 0 : trackById(d, bass).patterns[0].notes.size();
  RoleMap m = fullMap();  // so "bass" tem plugin
  std::vector<std::string> left;
  const int changed = giveSilentTracksRolePlugins(d, m, &left);
  CHECK_EQ(changed, 1);
  CHECK_EQ(trackById(d, bass).instrument.kind, std::string("plugin"));
  CHECK_EQ(trackById(d, bass).instrument.plugin.value("name", std::string()), std::string("Serum2"));
  CHECK_EQ(trackById(d, bass).patterns[0].notes.size(), notesBefore);  // as notas ficam
  CHECK(!left.empty());  // a bateria nao tem plugin no mapa: fica muda e e dita
  // Com bateria no mapa, as restantes pistas mudas ganham-na; uma segunda passagem nao muda nada.
  m.roles["drums"] = {fakePlugin("sfizz"), {}, {}};
  left.clear();
  const int more = giveSilentTracksRolePlugins(d, m, &left);
  int drums = 0;
  for (const auto& t : d.tracks) if (t.instrument.kind == "plugin" && t.instrument.plugin.value("name", std::string()) == "sfizz") ++drums;
  CHECK_EQ(drums, more);
  std::vector<std::string> again;
  const int none = giveSilentTracksRolePlugins(d, m, &again);
  CHECK_EQ(none, 0);
  CHECK_EQ(again.size(), left.size());
}

TEST("pistas mudas: pistas de audio, bus e com plugin nao mudam") {
  Document d = newTemplateDocument();
  const std::string before = toJson(d).dump();
  RoleMap m = fullMap();
  m.roles["drums"] = {fakePlugin("sfizz"), {}, {}};
  bool anySilent = false;
  for (const auto& t : d.tracks) anySilent = anySilent || t.instrument.kind == "builtin" || t.instrument.kind == "none";
  const int changed = giveSilentTracksRolePlugins(d, m);
  if (!anySilent) { CHECK_EQ(changed, 0); CHECK_EQ(toJson(d).dump(), before); }
  for (const auto& t : d.tracks) CHECK(t.instrument.kind != "builtin");
}

// ---- As tools da AI (Diogo 2026-11-16): R-Core / D-Core pelo bus, F-Core por intencao, ROLL, o catalogo.
namespace {
RoleMap aiMap() {
  RoleMap m = fullMap();
  m.roles["drive"] = guessRoleBinding("drive", fakePlugin("Airwindows"), json::array({{{"id", "20"}, {"name", "Drive"}}, {{"id", "21"}, {"name", "Dry/Wet"}}}));
  return m;
}
std::vector<std::string> patternTracks(const Document& d) {
  std::vector<std::string> out;
  for (const auto& t : d.tracks) if (playsPatterns(t)) out.push_back(t.id);
  return out;
}
int busesNamed(const Document& d, const std::string& prefix) {
  int n = 0;
  for (const auto& t : d.tracks) n += t.instrument.kind == "bus" && t.name.rfind(prefix, 0) == 0;
  return n;
}
}  // namespace

TEST("toolSharedBus: um so bus com o plugin do papel, sends sem duplicar, mais baixo nos drops") {
  Document d = newTemplateDocument();
  if (d.sections.empty()) addSection(d, "Drop 1", 0, std::min<int64_t>(d.lengthTicks, 16 * kPpq));
  const auto tracks = patternTracks(d);
  const RoleMap m = aiMap();
  const SharedBusResult a = toolSharedBus(d, tracks, m, "reverb", "hall");
  CHECK(a.newBus);
  CHECK_EQ(a.sends, (int)tracks.size());
  CHECK_EQ(busesNamed(d, "R-Core"), 1);
  CHECK(findRoleEffect(d, a.busId, "reverb") >= 0);  // o Valhalla do papel no bus
  const SharedBusResult b = toolSharedBus(d, tracks, m, "reverb", "hall");  // outra vez: o mesmo bus, os mesmos sends
  CHECK(!b.newBus);
  CHECK_EQ(b.busId, a.busId);
  for (const auto& id : tracks) {
    int toBus = 0;
    for (const auto& s : trackById(d, id).mix.sends) toBus += s.to == a.busId;
    CHECK_EQ(toBus, 1);
  }
  CHECK(findAutomation(d, a.busId, "gainDb") != nullptr);  // segue as seccoes
  // Outro sitio de reverb: os sends mudam para la e o bus antigo, sem ninguem, sai.
  const SharedBusResult c = toolSharedBus(d, tracks, m, "reverb", "plate");
  CHECK(c.busId != a.busId);
  CHECK(d.findTrack(a.busId) == nullptr);
  CHECK_EQ(busesNamed(d, "R-Core"), 1);
}

TEST("toolSharedBus: sem plugin no papel, ou preset errado, recusa sem mudar a musica") {
  Document d = newTemplateDocument();
  const std::string before = toJson(d).dump();
  CHECK_THROWS(toolSharedBus(d, patternTracks(d), RoleMap{}, "reverb", "room"));
  CHECK_THROWS(toolSharedBus(d, patternTracks(d), aiMap(), "reverb", "nada"));
  CHECK_THROWS(toolSharedBus(d, patternTracks(d), aiMap(), "chorus", ""));
  CHECK_THROWS(toolSharedBus(d, {}, aiMap(), "delay", ""));
  CHECK_EQ(toJson(d).dump(), before);
  const SharedBusResult r = toolSharedBus(d, patternTracks(d), aiMap(), "delay", "");  // "": dotted
  CHECK_EQ(busesNamed(d, "D-Core"), 1);
  CHECK(r.sentence.find("dotted") != std::string::npos);
}

TEST("toolIntentFx: cada intencao escreve as macros dos papeis; papel em falta e saltado e dito") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  const int64_t end = std::min<int64_t>(d.lengthTicks, 16 * 4 * kPpq), start = end - 4 * 4 * kPpq;
  for (const auto& intent : fxIntents()) {
    Document x = d;
    const RoleToolResult r = toolIntentFx(x, bass, aiMap(), intent, start, end, 0.7);
    CHECK(!r.keys.empty());
    CHECK(!r.sentence.empty());
  }
  RoleMap noDrive = fullMap();
  const RoleToolResult imp = toolIntentFx(d, bass, noDrive, "impact", start, end, 0.7);
  CHECK_EQ(imp.roles.size(), (size_t)1);  // so o reverb
  CHECK(!imp.skipped.empty());            // e diz que falta o drive
  const std::string before = toJson(d).dump();
  CHECK_THROWS(toolIntentFx(d, bass, aiMap(), "nada", start, end, 0.7));
  CHECK_THROWS(toolIntentFx(d, bass, RoleMap{}, "intro", start, end, 0.7));
  CHECK_EQ(toJson(d).dump(), before);
}

TEST("toolIntentFxTracks: a intencao escreve em TODAS as pistas com notas, nao so na primeira") {
  Document d = newTemplateDocument();
  const auto tracks = patternTracks(d);
  CHECK(tracks.size() >= 2);
  const int64_t end = std::min<int64_t>(d.lengthTicks, 8 * kPpq);
  const RoleToolResult r = toolIntentFxTracks(d, tracks, aiMap(), "intro", 0, end, 0.7);
  CHECK_EQ(r.keys.size(), 2 * tracks.size());  // filter + reverb em cada pista, nao so numa
  for (const auto& id : tracks) {
    CHECK(findRoleEffect(d, id, "filter") >= 0);
    CHECK(findRoleEffect(d, id, "reverb") >= 0);
  }
  CHECK(r.sentence.find("on " + std::to_string(tracks.size()) + " tracks") != std::string::npos);
  // Pistas sem notas (buses, audio) nao recebem a tool; so elas: recusa.
  std::vector<std::string> silent;
  for (const auto& t : d.tracks) if (!playsPatterns(t)) silent.push_back(t.id);
  const std::string before = toJson(d).dump();
  if (!silent.empty()) CHECK_THROWS(toolIntentFxTracks(d, silent, aiMap(), "intro", 0, end, 0.7));
  CHECK_THROWS(toolIntentFxTracks(d, {}, aiMap(), "intro", 0, end, 0.7));
  CHECK_THROWS(toolIntentFxTracks(d, tracks, aiMap(), "nada", 0, end, 0.7));
  CHECK_THROWS(toolIntentFxTracks(d, tracks, aiMap(), "intro", end, end, 0.7));
  CHECK_EQ(toJson(d).dump(), before);  // nenhuma recusa muda a musica
}

TEST("toolRoll: um padrao novo que acelera ate ao drop, os clips do sitio saem; valores errados recusam") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  const int64_t end = std::min<int64_t>(d.lengthTicks, 16 * 4 * kPpq), start = end - 2 * 4 * kPpq;
  const std::string before = toJson(d).dump();
  CHECK_THROWS(toolRoll(d, bass, start, end, "nada"));
  CHECK_THROWS(toolRoll(d, bass, start, end, "steps", -1, 13));
  CHECK_THROWS(toolRoll(d, bass, start, end, "steps", -1, 0, 5));
  CHECK_EQ(toJson(d).dump(), before);
  const RollResult r = toolRoll(d, bass, start, end, "steps", -1, 5, 1);
  CHECK(r.notes > 8);
  const Track& t = trackById(d, bass);
  int rollClips = 0;
  for (const auto& c : t.clips) {
    if (c.patternId == r.patternId) { ++rollClips; CHECK_EQ(c.startTick, start); continue; }
    CHECK(c.startTick + c.lengthTicks <= start || c.startTick >= end);  // nada mais toca ali
  }
  CHECK_EQ(rollClips, 1);
  // Mais rapido no fim: o ultimo intervalo entre notas e mais curto que o primeiro.
  const Pattern* p = t.findPattern(r.patternId);
  CHECK(p != nullptr);
  std::vector<int64_t> ticks;
  for (const auto& n : p->notes) ticks.push_back(n.tick);
  std::sort(ticks.begin(), ticks.end());
  CHECK(ticks[1] - ticks[0] > ticks.back() - ticks[ticks.size() - 2]);
}

TEST("catalogo das tools: ids unicos, cada uma com o que faz e parametros com nome") {
  std::set<std::string> ids;
  for (const auto& t : songToolCatalog()) {
    CHECK(ids.insert(t.id).second);
    CHECK(!t.what.empty());
    for (const auto& p : t.params) CHECK(!p.name.empty() && !p.what.empty());
  }
  CHECK(ids.count("space") && ids.count("fx") && ids.count("roll") && ids.count("suck") && ids.count("pump") && ids.count("buildup"));
  CHECK_EQ(songToolCatalogJson().size(), songToolCatalog().size());
  Document d = newTemplateDocument();
  CHECK_THROWS(sectionSpan(d, "nao existe nenhuma"));
  CHECK_EQ(sectionSpan(d, "").end, d.lengthTicks);
}

TEST("cartoes das tools: a lista esta certa, sem S-Core, e acha-se por id e pelos nomes da AI") {
  const auto problems = corePluginListProblems();
  for (const auto& p : problems) std::printf("  problema: %s\n", p.c_str());
  CHECK(problems.empty());
  CHECK(findCorePlugin("score") == nullptr);  // S-Core nao e um cartao (Diogo 2026-11-16)
  CHECK(findCorePlugin("Pattern Maker") && findCorePlugin("Pattern Maker")->id == "pcore");
  CHECK(findCorePlugin("pump") && findCorePlugin("pump")->id == "pum");
  CHECK(findCorePlugin("R-Core") && findCorePlugin("R-Core")->id == "rcore");
  CHECK(findCorePlugin("") == nullptr);
  // Cada tool do catalogo da AI tem o seu cartao (space = R-Core e D-Core, fx = F-Core).
  for (const auto& t : songToolCatalog()) {
    const std::string card = t.id == "space" ? "rcore" : t.id == "fx" ? "fcore" : t.id == "pump" ? "pum" : t.id;
    CHECK(findCorePlugin(card) != nullptr);
  }
}

// Auditoria 2026-11-17, item 5: com a cadeia dos efeitos cheia, o SUCK escrevia a curva do reverb e so depois
// o filtro era recusado, deixando a musica a meio mudada (o contrato de role_tools.hpp: um passo de undo ou nada).
TEST("SUCK: uma cadeia de efeitos cheia recusa-se antes de escrever seja o que for") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  addPluginEffect(d, bass, fakePlugin("ValhallaRoom"));  // o reverb ja la esta, sem papel marcado
  for (int i = 0; i < 15; ++i) addEffect(d, bass, "eq");
  CHECK_EQ(effectChain(d, bass).size(), (size_t)16);
  const std::string before = toJson(d).dump();
  CHECK_THROWS(toolSuck(d, bass, fullMap(), 8 * 4 * kPpq, 2 * 4 * kPpq));  // o filtro nao cabe
  CHECK_EQ(toJson(d).dump(), before);  // antes: ficava atras a curva "fx:<id>:vst:0" do reverb
  CHECK_EQ(effectChain(d, bass).size(), (size_t)16);
  size_t curves = 0;
  for (const auto& t : d.tracks) curves += t.automation.size();
  CHECK_EQ(curves, (size_t)0);  // nenhuma curva de nenhum papel, em parte nenhuma
  // Os dois papeis ja estao na cadeia: nao e preciso nenhum efeito novo, por isso nao se recusa (nada de recusas falsas).
  Document e = newTemplateDocument();
  const std::string kick = trackNamed(e, "ick");
  addPluginEffect(e, kick, fakePlugin("ValhallaRoom"));
  addPluginEffect(e, kick, fakePlugin("Filter"));
  for (int i = 0; i < 14; ++i) addEffect(e, kick, "eq");
  CHECK_EQ(effectChain(e, kick).size(), (size_t)16);
  const RoleToolResult r = toolSuck(e, kick, fullMap(), 8 * 4 * kPpq, 2 * 4 * kPpq);
  CHECK_EQ(r.roles.size(), (size_t)2);
  CHECK_EQ(effectChain(e, kick).size(), (size_t)16);  // os mesmos 16, agora com o papel marcado
  int tagged = 0;
  for (const auto& fx : effectChain(e, kick))
    if (fx.value("type", std::string()) == "plugin" && fx.contains("role")) ++tagged;
  CHECK_EQ(tagged, 2);
  // Com sitio na cadeia, escreve-se tudo como dantes.
  Document f = newTemplateDocument();
  const std::string hats = trackNamed(f, "ats");
  const RoleToolResult ok = toolSuck(f, hats, fullMap(), 8 * 4 * kPpq, 2 * 4 * kPpq);
  CHECK_EQ(effectChain(f, hats).size(), (size_t)2);
  CHECK_EQ(ok.keys.size(), (size_t)3);  // reverb mix, reverb size, filtro cutoff
}

// O mesmo shape dos outros dois: tambem recusam antes de escrever quando o que ha de escrever nao cabe.
TEST("build-up e fx por intencao: cadeia cheia nao deixa metade escrita") {
  Document d = newTemplateDocument();
  const std::string bass = trackNamed(d, "ass");
  addPluginEffect(d, bass, fakePlugin("ValhallaRoom"));
  for (int i = 0; i < 15; ++i) addEffect(d, bass, "eq");
  const std::string before = toJson(d).dump();
  BuildupFx fx;
  fx.filter = false;
  fx.reverb = true;   // ja esta na cadeia: escreve-se primeiro
  fx.delay = true;    // e este nao cabe, e so se sabe depois (era isso que deixava a curva atras)
  CHECK_THROWS(toolBuildupFx(d, bass, fullMap(), 4 * 4 * kPpq, 6 * 4 * kPpq, fx));
  CHECK_EQ(toJson(d).dump(), before);
  Document e = newTemplateDocument();
  const std::string kick = trackNamed(e, "ick");
  addPluginEffect(e, kick, fakePlugin("Filter"));
  for (int i = 0; i < 15; ++i) addEffect(e, kick, "eq");
  const std::string was = toJson(e).dump();
  CHECK_THROWS(toolIntentFx(e, kick, fullMap(), "intro", 0, 4 * 4 * kPpq, 0.7));  // falta o reverb
  CHECK_EQ(toJson(e).dump(), was);
}
