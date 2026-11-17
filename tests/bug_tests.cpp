// Testes dos bugs medios e pequenos corrigidos a 2026-11-16 (lista do sweep antigo revista contra o main 06e0882).
// Cada teste falhava antes da correcao.
#include <algorithm>
#include <filesystem>
#include "support/check.hpp"
#include "edit.hpp"
#include "follow.hpp"
#include "pattern_tools.hpp"
#include "pcore.hpp"
#include "stutter.hpp"
#include "templates.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kBar = 4 * kPpq;
}

TEST("sweep: addPattern recusa um padrao de comprimento 0 (dividia por zero na projeccao)") {
  Document d = newEmptyDocument();
  const std::string id = addTrack(d, "", "X");
  CHECK_THROWS(addPattern(trackById(d, id), 0));
  CHECK_THROWS(addPattern(trackById(d, id), 65 * kBar));
  CHECK(!addPattern(trackById(d, id), kBar).empty());
}

TEST("sweep: ids enormes num ficheiro nao dao a volta ao gerar o seguinte") {
  CHECK_EQ(idNumber("99999999999999999999999"), 1000000000L);
  CHECK_EQ(idNumber("-5"), 0L);
  CHECK_EQ(idNumber("abc"), 0L);
  Pattern p;
  p.id = "p1";
  p.lengthTicks = kBar;
  Note n;
  n.id = "n99999999999999999999";
  p.notes.push_back(n);
  CHECK_EQ(newNoteId(p), std::string("n1000000001"));
}

TEST("sweep: um padrao aprovado guarda o 'approval' e volta a ler; um id longo com sufixo cabe nos 80") {
  PatternPreset p;
  p.id = std::string(79, 'a');
  p.name = "Teste";
  p.status = "approved";
  p.approval = "Diogo: gosto";
  p.lengthTicks = kBar;
  Note n;
  n.id = "n1";
  n.tick = 0;
  n.pitch = 36;
  p.notes.push_back(n);
  const PatternPreset back = presetFromJson(presetToJson(p));
  CHECK_EQ(back.approval, p.approval);
  const fs::path dir = fs::temp_directory_path() / "core-bug-tests-presets";
  fs::remove_all(dir);
  const PatternPreset first = savePatternPreset(dir, p, {});
  const PatternPreset second = savePatternPreset(dir, p, {first});
  CHECK(second.id.size() <= 80);
  CHECK(second.id != first.id);
  std::vector<std::string> problems;
  CHECK_EQ(loadPatternLibrary({{"user", dir}}, &problems).size(), (size_t)2);
  CHECK(problems.empty());
}

TEST("sweep: schemaVersion 2^32 + 1 de um template nao e lido como 1") {
  json t = {{"format", "core-template"}, {"schemaVersion", 4294967297LL}, {"id", "x"}};
  CHECK_THROWS(parseTemplate(t));
}

TEST("sweep: P-Core com numeros enormes nao tem cast indefinido (recusado ou limitado)") {
  json j = {{"mode", "create"}, {"variationEvery", 1e300}, {"rollBeats", -1e300}, {"breakSpacing", 1e300}};
  bool threw = false;
  try { (void)pcoreLayersFromJson(j); } catch (const DocumentError&) { threw = true; }
  CHECK(threw);  // fora do intervalo: recusado com mensagem, nao um int qualquer
}

TEST("sweep: isolateRegion guarda os campos extra do clip cortado e o nome da copia nao parte um caracter") {
  Document d = newEmptyDocument();
  d.lengthTicks = 8 * kBar;
  const std::string id = addTrack(d, "", "X");
  Track& t = trackById(d, id);
  t.patterns[0].name = std::string(36, 'a') + "\xC3\xA7\xC3\xA7";  // 40 bytes, acaba em 'cc' cedilhado
  t.clips = {{"c1", "p1", 0, 8 * kBar, 0, json{{"meu", 1}}}};
  isolateRegion(d, id, 2 * kBar, 4 * kBar, {});
  for (const auto& c : trackById(d, id).clips) CHECK_EQ(c.extra.value("meu", 0), 1);
  CHECK_EQ(toJson(parseDocument(toJson(d))).dump(), toJson(d).dump());  // nomes validos: guarda e reabre
}

TEST("sweep: STUTTER numa zona apanha as notas das repeticoes de um clip em loop") {
  Document d = newEmptyDocument();
  d.lengthTicks = 8 * kBar;
  const std::string id = addTrack(d, "", "Lead");
  Track& t = trackById(d, id);
  t.patterns[0].lengthTicks = kBar;
  Note n;
  n.id = "n1";
  n.tick = 0;
  n.length = kPpq;
  n.pitch = 60;
  t.patterns[0].notes = {n};
  t.clips = {{"c1", "p1", 0, 4 * kBar, 0, json::object()}};  // o compasso repete 4 vezes
  // Zona no 3o compasso: so a repeticao conta (antes: nada, porque a 1a passagem esta fora da zona).
  const int written = stutterTrack(d, id, 2 * kBar, 3 * kBar, stutterPresets().front());
  CHECK(written > 0);
}

TEST("sweep: FOLLOW grava o corte da nota que atravessa o inicio da zona") {
  Document d = newEmptyDocument();
  d.lengthTicks = 4 * kBar;
  const std::string kick = addTrack(d, "", "Kick");
  Track& t = trackById(d, kick);
  t.patterns[0].lengthTicks = 4 * kBar;
  Note longNote;
  longNote.id = "n1";
  longNote.tick = 0;
  longNote.length = 2 * kBar;  // toca ate meio do 3o compasso
  longNote.pitch = 36;
  t.patterns[0].notes = {longNote};
  t.clips = {{"c1", "p1", 0, 4 * kBar, 0, json::object()}};
  FollowZone z;
  z.start = kBar;
  z.end = kBar + kPpq;
  z.onsets = {kBar, kBar + kPpq / 4, kBar + kPpq / 2};
  kickFollow(d, kick, {z}, FollowSettings{});
  // A nota longa acaba onde a zona comeca, na musica (nao so na partitura de trabalho).
  bool cut = false, found = false;
  for (const auto& c : trackById(d, kick).clips) {
    const Pattern* p = trackById(d, kick).findPattern(c.patternId);
    for (const auto& note : p->notes)
      if (note.tick >= c.offsetTicks && c.startTick + note.tick - c.offsetTicks == 0 && note.pitch == 36) {
        found = true;
        cut = note.length <= kBar;
      }
  }
  CHECK(found);
  CHECK(cut);
}

TEST("reset: o menu da chain oferece TODOS os efeitos (os proprios incluidos) e o Remove leva o FX proprio e as curvas dele") {
  Document d = newEmptyDocument();
  const std::string id = addTrack(d, "", "X");
  const int atUtility = addEffect(d, id, "utility");  // Core's own, as a template or an old song carries them
  json verb = {{"id", "fx9"}, {"type", "plugin"},
               {"plugin", json{{"id", "v1"}, {"name", "FakeVerb"}, {"format", "vst3"}, {"file", "C:/plugins/FakeVerb.vst3"}}},
               {"bypass", false}};
  trackById(d, id).effects.push_back(verb);
  addEffect(d, id, "eq");  // another own one, after the plugin
  const std::vector<EffectMenuEntry> entries = effectMenuEntries(trackById(d, id).effects);
  CHECK_EQ((int)entries.size(), 3);  // the old menus offered only the plugin: the own ones had no Remove anywhere
  if (entries.size() == 3) {  // the detail checks only read what the size check above guarantees
    CHECK_EQ(entries[0].index, atUtility);
    CHECK_EQ(entries[0].label, std::string("Utility"));
    CHECK(!entries[0].plugin);
    CHECK(!entries[0].window);
    CHECK_EQ(entries[1].index, 1);
    CHECK_EQ(entries[1].label, std::string("FakeVerb"));
    CHECK(entries[1].plugin && entries[1].window);
    CHECK_EQ(entries[2].index, 2);
    CHECK_EQ(entries[2].label, std::string("Core EQ"));
  }
  // And the Remove the menu offers works on an own effect: the chain and its curves go together.
  setAutomationPoint(d, id, effectParamKey("fx1", "gain"), 0, -3);
  CHECK(!trackById(d, id).automation.empty());
  removeEffect(d, id, entries[0].index);
  CHECK_EQ((int)trackById(d, id).effects.size(), 2);
  CHECK_EQ(trackById(d, id).effects[0].value("id", std::string()), std::string("fx9"));
  const bool curvesGone = std::none_of(trackById(d, id).automation.begin(), trackById(d, id).automation.end(),
                                       [](const Automation& a) { return a.param == "fx:fx1:gain"; });
  CHECK(curvesGone);
}

// Auditoria 2026-11-17, item 4: duplicateSection empurrava as mudancas de tempo de depois da seccao mas nao
// copiava as de dentro, por isso a seccao duplicada perdia o tempo (e o ramp) que o randomizador EIXO J escreve.
TEST("duplicar uma seccao copia tambem as mudancas de tempo que ha dentro dela") {
  Document d = newEmptyDocument();
  const std::string a = addSection(d, "Build", 0, 4 * kBar);
  addSection(d, "Drop", 4 * kBar, 4 * kBar);
  setTempoMap(d, {{1 * kBar, 200, json::object(), false}, {3 * kBar, 160, json::object(), true}, {6 * kBar, 120, json::object(), false}});
  const size_t before = d.tempoMap.size();
  const std::string dup = duplicateSection(d, a);
  CHECK_EQ(d.tempoMap.size(), before + 2);  // os dois pontos de dentro tem copia na seccao nova
  const auto at = [&](int64_t tick) -> const TempoPoint* {
    for (const auto& p : d.tempoMap) if (p.tick == tick) return &p;
    return nullptr;
  };
  const TempoPoint* copy = at(5 * kBar);
  CHECK(copy != nullptr);
  if (copy) {
    CHECK_NEAR(copy->bpm, 200, 1e-9);
    CHECK(!copy->ramp);
  }
  const TempoPoint* glide = at(7 * kBar);
  CHECK(glide != nullptr);
  if (glide) {
    CHECK_NEAR(glide->bpm, 160, 1e-9);
    CHECK(glide->ramp);  // o ramp tambem
  }
  const TempoPoint* later = at(10 * kBar);  // o que ja ia para depois move-se uma seccao
  CHECK(later != nullptr);
  if (later) CHECK_NEAR(later->bpm, 120, 1e-9);
  // O tempo que toca na seccao duplicada e o da seccao original, nao o ultimo de antes.
  CHECK_NEAR(bpmAt(d, 5 * kBar), 200, 1e-9);   // antes: 160, o ponto de 3 compassos, que ficava a valer
  CHECK_NEAR(bpmAt(d, 7 * kBar), 160, 1e-9);
  CHECK(!dup.empty());
  const Section* moved = nullptr;
  for (const auto& s : d.sections) if (s.id == dup) moved = &s;
  CHECK(moved != nullptr);
  if (moved) CHECK_EQ(moved->startTick, 4 * kBar);
}

CORE_TEST_MAIN
