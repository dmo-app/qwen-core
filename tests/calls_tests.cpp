// Testes do control (app/control/calls.cpp): a proposta de chamadas do AICORE vista pelo Core — como um "on" e uma
// "section" são escolhidos na canção e o que é recusado antes de chegar ao Apply. O relatorio profundo de 2026-11-17
// (itens 22 e 26) abriu estas duas portas: o resolvedor não parava na primeira faixa igual (a última ganhava) e um
// Mix sem botões era guardado como chamada, aplicado sem mudar nada e anunciado como feito.
#include <string>
#include <utility>
#include <vector>
#include "support/check.hpp"
#include "calls.hpp"
#include "edit.hpp"

using namespace core::doc;
using core::control::CoreFunction;
using core::control::Proposal;

namespace {

const std::vector<CoreFunction>& functions() {
  static const std::vector<CoreFunction> f = core::control::buildFunctions();
  return f;
}

// Uma canção só com as faixas pedidas, pela ordem dada (a ordem em que a app as mostra).
Document songWith(const std::vector<std::pair<std::string, std::string>>& tracks) {
  Document d = newEmptyDocument();
  d.name = "Teste calls";
  for (const auto& [id, name] : tracks) {
    Track t;
    t.id = id;
    t.name = name;
    d.tracks.push_back(std::move(t));
  }
  return d;
}

Document songWithSections(const std::vector<Section>& sections) {
  Document d = songWith({{"kick", "Kick"}});
  d.sections = sections;
  return d;
}

// {"calls":[{"fn":..., "on":..., "args":..., "section":...}]} — só o que o teste nomeia.
json proposalOf(const std::string& fn, const json& fields) {
  json c = {{"fn", fn}};
  for (auto kv = fields.begin(); kv != fields.end(); ++kv) c[kv.key()] = kv.value();
  return {{"id", "p1"}, {"summary", "teste"}, {"calls", json::array({c})}};
}

Proposal check(json raw, const Document& song) {
  return core::control::validateProposal(raw, functions(), song, nullptr);
}

// O texto do que ficou de fora: um teste que falha tem de se poder ler.
bool hasProblem(const Proposal& p, const std::string& needle) {
  for (const auto& s : p.problems) if (s.find(needle) != std::string::npos) return true;
  return false;
}
}  // namespace

CORE_TEST_MAIN

// ---- audit 22: a quem pertence um nome

TEST("audit 22: um on que e o id de uma faixa ganha a outra que se chama assim") {
  // "lead" e o id da primeira; a segunda chama-se "Lead". O Core tem uma regra para isto (a primeira igual, com
  // break: as effects logo abaixo em calls.cpp, o sectionSpan dos papeis) — o resolvedor de faixas nao a seguia.
  Document d = songWith({{"lead", "Bass"}, {"t2", "Lead"}});
  Proposal p = check(proposalOf("mix", {{"on", "lead"}, {"args", {{"gainDb", -3.0}}}}), d);
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  CHECK_EQ(p.calls[0].trackId, std::string("lead"));
  CHECK_EQ(p.calls[0].trackName, std::string("Bass"));
  // E o Apply mexe nessa faixa, nao na outra.
  const auto lines = core::control::applyProposal(d, p, "", {});
  CHECK_EQ(lines.size(), (size_t)1);
  CHECK_EQ(d.tracks[0].mix.gainDb, -3.0);
  CHECK_EQ(d.tracks[1].mix.gainDb, 0.0);
}

TEST("audit 22: duas faixas com o mesmo nome fica a primeira") {
  Document d = songWith({{"a", "Lead"}, {"b", "Lead"}});
  Proposal p = check(proposalOf("mix", {{"on", "Lead"}, {"args", {{"gainDb", -6.0}}}}), d);
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  CHECK_EQ(p.calls[0].trackId, std::string("a"));
  core::control::applyProposal(d, p, "", {});
  CHECK_EQ(d.tracks[0].mix.gainDb, -6.0);
  CHECK_EQ(d.tracks[1].mix.gainDb, 0.0);
}

TEST("audit 22: o nome continua a servir quando so ha uma faixa com ele") {
  // O contrario tambem tem de continuar a funcionar: "on" que nao e id de ninguem e nome de uma so faixa.
  Document d = songWith({{"t1", "Acid lead"}, {"t2", "Kick"}});
  Proposal p = check(proposalOf("mix", {{"on", "acid LEAD"}, {"args", {{"pan", 0.5}}}}), d);
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  CHECK_EQ(p.calls[0].trackId, std::string("t1"));
  // Uma faixa que nao existe: aviso, e a chamada fica para Diogo escolher a faixa no Apply (como sempre).
  Proposal q = check(proposalOf("mix", {{"on", "Ninguem"}, {"args", {{"pan", 0.5}}}}), d);
  CHECK_EQ(q.calls.size(), (size_t)1);
  CHECK(q.calls.empty() || !q.calls[0].warnings.empty());
  CHECK(q.calls.empty() || q.calls[0].trackId.empty());
}

TEST("audit 22: uma section que e o id de uma parte ganha a outra que se chama assim") {
  // As partes pela ordem em que comecam (o que o Core guarda). "intro" e o id da primeira; a segunda chama-se "Intro".
  std::vector<Section> secs;
  secs.push_back(Section{"intro", "Break", 0, 4 * kPpq, json::object(), ""});
  secs.push_back(Section{"s2", "Intro", 4 * kPpq, 4 * kPpq, json::object(), ""});
  const auto* f = core::control::findFunction(functions(), "voicestutter");
  CHECK(f != nullptr);
  if (!f || f->presets.empty()) return;
  Proposal p = check(proposalOf("voicestutter", {{"preset", f->presets[0]}, {"section", "intro"}}), songWithSections(secs));
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  CHECK_EQ(p.calls[0].sectionName, std::string("Break"));
  CHECK_EQ(p.calls[0].startTick, (int64_t)0);
  CHECK_EQ(p.calls[0].endTick, (int64_t)4 * kPpq);
}

// ---- audit 26: uma chamada que nao muda nada nao pode ser aplicada nem anunciada como aplicada

TEST("audit 26: um Mix sem volume nem pan e deixado de fora, nao aplicado a vazio") {
  Document d = songWith({{"kick", "Kick"}});
  const double antes = d.tracks[0].mix.gainDb;
  Proposal p = check(proposalOf("mix", {{"on", "Kick"}}), d);
  CHECK(p.calls.empty());
  CHECK(!p.problems.empty());
  std::vector<std::string> lines = core::control::applyProposal(d, p, "", {});
  CHECK(lines.empty());
  CHECK_EQ(d.tracks[0].mix.gainDb, antes);
}

TEST("audit 26: um Mix do master so com pan e deixado de fora (o master nao tem pan)") {
  Document d = songWith({{"kick", "Kick"}});
  const double antes = d.masterGainDb;
  Proposal p = check(proposalOf("mix", {{"on", "master"}, {"args", {{"pan", 0.7}}}}), d);
  CHECK(p.calls.empty());
  CHECK(hasProblem(p, "the master has no pan"));
  core::control::applyProposal(d, p, "", {});
  CHECK_EQ(d.masterGainDb, antes);
}

TEST("audit 26: um Mix so com um botao que nao existe e deixado de fora") {
  Document d = songWith({{"kick", "Kick"}});
  Proposal p = check(proposalOf("mix", {{"on", "Kick"}, {"args", {{"loudness", -3.0}}}}), d);
  CHECK(p.calls.empty());
  CHECK(hasProblem(p, "no knob"));
  core::control::applyProposal(d, p, "", {});
  CHECK_EQ(d.tracks[0].mix.gainDb, 0.0);
}

TEST("audit 26: um Mix com o que o master tem fica, e so isso") {
  Document d = songWith({{"kick", "Kick"}});
  Proposal p = check(proposalOf("mix", {{"on", "master"}, {"args", {{"gainDb", -1.5}, {"pan", 0.7}}}}), d);
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  CHECK_EQ(p.calls[0].args.size(), (size_t)1);
  CHECK_EQ(p.calls[0].args.count("gainDb"), (size_t)1);
  const auto lines = core::control::applyProposal(d, p, "", {});
  CHECK_EQ(lines.size(), (size_t)1);
  CHECK_EQ(d.masterGainDb, -1.5);
}

TEST("audit 26: um Mix de faixa com volume e pan fica e aplica os dois") {
  Document d = songWith({{"kick", "Kick"}});
  Proposal p = check(proposalOf("mix", {{"on", "Kick"}, {"args", {{"gainDb", -2.5}, {"pan", -0.4}}}}), d);
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  const auto lines = core::control::applyProposal(d, p, "", {});
  CHECK_EQ(lines.size(), (size_t)1);
  CHECK_EQ(d.tracks[0].mix.gainDb, -2.5);
  CHECK_EQ(d.tracks[0].mix.pan, -0.4);
}

TEST("audit 26: um Open sem botoes continua a ser uma chamada que nao muda nada (de proposito)") {
  // A recusa do Mix nao pode generalizar-se: o Open e a unica familia que nao muda nada e diz uma frase mesmo assim.
  Document d = songWith({{"kick", "Kick"}});
  Proposal p = check(proposalOf("open", {{"on", "Kick"}, {"what", "mixer"}}), d);
  CHECK_EQ(p.calls.size(), (size_t)1);
  if (p.calls.size() != 1) return;
  const auto lines = core::control::applyProposal(d, p, "", {});
  CHECK_EQ(lines.size(), (size_t)1);
  CHECK(!lines[0].empty());
  const auto opens = core::control::opensOf(p, "");
  CHECK_EQ(opens.size(), (size_t)1);
  CHECK_EQ(opens.empty() ? std::string() : opens[0].what, std::string("mixer"));
}
