// Testes da caça profunda de 2026-11-16 (RELATORIO-CAÇA-PROFUNDA.md, P01-P06). Cada teste repete o caso do
// relatorio.
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>
#include "support/check.hpp"
#include "support/signals.hpp"
#include <limits>
#include <tuple>
#include "assets.hpp"
#include "autosave.hpp"
#include "bounce.hpp"
#include "edit.hpp"
#include "pattern_rack.hpp"
#include "randomize.hpp"
#include "storage.hpp"
#include "templates.hpp"
#include "core/project.hpp"
#include "core/render_job.hpp"
#include "core/server.hpp"
#include "io/audio_file.hpp"
#include "engine_protocol.hpp"
#include "pattern_sound.hpp"
#include "sampling.hpp"
#include <fstream>
#include "io/wav.hpp"

namespace fs = std::filesystem;
using core::doc::Document;
using core::json;

namespace {
fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / "core-hunt-tests" / name;
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

fs::path sineFile(const fs::path& file) {
  const auto L = coretest::sine(440, 48000, 24000, 0.8f);
  core::writeWav(file.string(), L, L, 48000, 24);
  return file;
}

bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

json pluginEffect(const std::string& id, const std::string& state) {
  return {{"id", id}, {"type", "plugin"}, {"bypass", false},
          {"plugin", {{"format", "vst3"}, {"id", "hunt-effect"}, {"file", "hunt-effect.vst3"}, {"state", state}}}};
}
}  // namespace

TEST("caca P01: o bounce de uma pista com nome comprido (72+ bytes, com acentos) guarda e faz autosave") {
  const fs::path dir = freshDir("p01");
  sineFile(dir / "bounce.wav");
  // 71 'a' + "ç" = 73 bytes; 79 bytes so de "ç" e 'a' (corte a meio de um caracter se fosse por bytes).
  for (const std::string name : {std::string(72, 'a'), std::string(71, 'a') + "\xc3\xa7", std::string(78, 'a') + "\xc3\xa7",
                                 std::string(2, 'a') + [] { std::string s; for (int i = 0; i < 39; ++i) s += "\xc3\xa7"; return s; }()}) {
    Document d = core::doc::newEmptyDocument();
    const std::string track = core::doc::addAudioTrack(d, "x");
    core::doc::renameTrack(d, track, name);
    const auto out = core::doc::placeBounce(d, track, dir / "bounce.wav", dir, 0, 960);
    const std::string printed = core::doc::trackById(d, out.trackId).name;
    CHECK(printed.size() <= 80);
    CHECK(validUtf8(printed));
    CHECK(printed.size() >= 9 && printed.compare(printed.size() - 9, 9, " (bounce)") == 0);  // o sufixo fica
    core::doc::saveFile(d, dir / "song.core");  // antes (relatorio): "tracks[..].name: is longer than 80 characters"
    CHECK_EQ(core::doc::openFile(dir / "song.core").document.tracks.size(), d.tracks.size());
  }
  // O limite conta bytes, e a mensagem diz bytes: 41 "ç" sao 41 letras e 82 bytes.
  std::string cedillas;
  for (int i = 0; i < 41; ++i) cedillas += "\xc3\xa7";
  Document d = core::doc::newEmptyDocument();
  const std::string track = core::doc::addAudioTrack(d, "x");
  std::string message;
  try { core::doc::renameTrack(d, track, cedillas); } catch (const core::doc::DocumentError& e) { message = e.what(); }
  CHECK(message.find("bytes") != std::string::npos);
  json j = core::doc::toJson(d);
  j["tracks"][0]["name"] = cedillas;
  message.clear();
  try { (void)core::doc::parseDocument(j); } catch (const core::doc::DocumentError& e) { message = e.what(); }
  CHECK(message.find("bytes") != std::string::npos);
  // addTrack corta o nome sem partir um caracter.
  const std::string id = core::doc::addTrack(d, "bass", std::string(79, 'a') + "\xc3\xa7");
  CHECK(validUtf8(core::doc::trackById(d, id).name));
  CHECK_EQ(core::doc::trackById(d, id).name.size(), (size_t)79);
}

TEST("caca P02: render e render.start tem a mesma cauda por omissao") {
  CHECK_EQ(core::kDefaultTailMs, 2000.0);
  const fs::path dir = freshDir("p02");
  const fs::path wav = sineFile(dir / "a.wav");
  json clip = {{"path", wav.string()}, {"startTick", 0}, {"durationSeconds", 0.5}, {"gainDb", 0}};
  json track = {{"id", "audio1"}, {"instrument", "audio"}, {"gainDb", 0}, {"audioClips", json::array({clip})}};
  json project = {{"format", core::kProjectFormat}, {"version", 2}, {"bpm", 120}, {"lengthTicks", 960}, {"masterGainDb", 0},
                  {"tracks", json::array({track})}};
  core::Server server(nullptr, 48000);
  CHECK(server.handle({{"cmd", "project.load"}, {"args", {{"project", project}}}})["ok"].get<bool>());
  // A CLI (render, sem tailMs): 0,5 s de musica + a cauda por omissao. Antes: 1,5 s em vez de 2.
  json r = server.handle({{"cmd", "render"}, {"args", {{"path", (dir / "cli.wav").string()}, {"bits", 32}}}});
  CHECK(r["ok"].get<bool>());
  CHECK(std::abs(r["result"]["seconds"].get<double>() - (0.5 + core::kDefaultTailMs / 1000)) < 0.01);
  // A app (render.start, sem tailMs): o mesmo valor.
  r = server.handle({{"cmd", "render.start"}, {"args", {{"path", (dir / "app.wav").string()}, {"bits", 32}}}});
  CHECK(r["ok"].get<bool>());
  CHECK_EQ(r["result"]["tailMs"].get<double>(), core::kDefaultTailMs);
  const auto job = r["result"]["jobId"];
  for (int i = 0; i < 500; ++i) {  // o job acaba antes de o servidor sair
    const std::string state = server.handle({{"cmd", "render.status"}, {"args", {{"jobId", job}}}})["result"].value("state", "");
    if (state == "succeeded" || state == "failed" || state == "cancelled") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK_EQ(core::readWav((dir / "app.wav").string(), 1 << 24).frames(), core::readWav((dir / "cli.wav").string(), 1 << 24).frames());
}

TEST("caca P03: ids de efeito repetidos ou com ':' sao limpos ao abrir, e o motor ja nao renomeia nada") {
  Document d = core::doc::newEmptyDocument();
  const std::string track = core::doc::addAudioTrack(d, "t");
  json j = core::doc::toJson(d);
  j["tracks"][0]["effects"] = json::array({pluginEffect("dup", "QQ=="), pluginEffect("dup", "Qg=="), pluginEffect("we:ird", "Qw==")});
  j["tracks"][0]["automation"] = json::array({{{"param", "fx:dup:vst:1"}, {"points", json::array({{{"tick", 0}, {"value", 0.5}}})}},
                                              {{"param", "fx:we:ird:vst:2"}, {"points", json::array({{{"tick", 0}, {"value", 0.25}}})}}});
  j["master"]["effects"] = json::array({pluginEffect("m:1", "RA==")});
  j["master"]["automation"] = json::array({{{"param", "fx:m:1:vst:3"}, {"points", json::array({{{"tick", 0}, {"value", 1}}})}}});
  const Document opened = core::doc::parseDocument(j);
  const json& fx = opened.tracks[0].effects;
  CHECK_EQ(fx[0]["id"].get<std::string>(), std::string("dup"));      // o primeiro fica com o id e a curva
  CHECK_EQ(fx[1]["id"].get<std::string>(), std::string("dup_2"));
  CHECK_EQ(fx[2]["id"].get<std::string>(), std::string("we_ird"));
  CHECK_EQ(opened.masterEffects[0]["id"].get<std::string>(), std::string("m_1"));
  CHECK_EQ(opened.tracks[0].automation[0].param, std::string("fx:dup:vst:1"));
  CHECK_EQ(opened.tracks[0].automation[1].param, std::string("fx:we_ird:vst:2"));
  CHECK_EQ(opened.masterAutomation[0].param, std::string("fx:m_1:vst:3"));

  // O motor recebe ids que guarda tal como estao: nada em renamedEffects, e as curvas tocam.
  const json projection = core::doc::toProjection(opened);
  const core::Project p = core::fromJson(projection);
  CHECK(p.renamedEffects.empty());
  CHECK_EQ(projection["tracks"][0]["envelopes"].size(), (size_t)2);
  CHECK_EQ(projection["masterEnvelopes"].size(), (size_t)1);

  // O estado que o motor devolve (chave fx:<pista>:<id>) chega a cada plugin: antes o 2.o e o 3.o perdiam-no.
  Document live = opened;
  const json states = {{core::doc::pluginKey(track, "dup_2"), "AA=="}, {core::doc::pluginKey(track, "we_ird"), "AQ=="},
                       {core::doc::pluginKey("", "m_1"), "Ag=="}};
  CHECK_EQ(core::doc::applyPluginStates(live, states), 3);
  CHECK_EQ(live.tracks[0].effects[0]["plugin"]["state"].get<std::string>(), std::string("QQ=="));
  CHECK_EQ(live.tracks[0].effects[1]["plugin"]["state"].get<std::string>(), std::string("AA=="));
  CHECK_EQ(live.tracks[0].effects[2]["plugin"]["state"].get<std::string>(), std::string("AQ=="));
  CHECK_EQ(live.masterEffects[0]["plugin"]["state"].get<std::string>(), std::string("Ag=="));

  // Guardar e reabrir da o mesmo (ja limpo).
  CHECK_EQ(core::doc::toJson(core::doc::parseDocument(core::doc::toJson(opened))).dump(), core::doc::toJson(opened).dump());
  // Um documento com ids limpos fica byte a byte igual.
  Document clean = core::doc::newEmptyDocument();
  const std::string t2 = core::doc::addAudioTrack(clean, "t");
  core::doc::addPluginEffect(clean, t2, pluginEffect("x", "QQ==")["plugin"]);
  core::doc::addPluginEffect(clean, t2, pluginEffect("x", "Qg==")["plugin"]);
  CHECK_EQ(core::doc::toJson(core::doc::parseDocument(core::doc::toJson(clean))).dump(), core::doc::toJson(clean).dump());
}

// P04: o codigo esta certo (Diogo 2026-11-16, PR #2: o som vem so de plugins). Os efeitos proprios e os moduladores
// dos templates ficam guardados na musica e calados; o plano dizia-o de forma ambigua e foi corrigido.
TEST("caca P04: efeitos proprios e moduladores ficam no .core e fora do motor; os plugins e as suas curvas tocam") {
  core::doc::TemplateLibrary lib = core::doc::loadTemplates(CORE_TEMPLATES_DIR, freshDir("p04-user"), true);
  size_t builtin = 0, modulators = 0;
  for (const auto& e : lib.templates) {
    const Document d = core::doc::documentFromTemplate(e.tmpl);
    const json saved = core::doc::toJson(d), projection = core::doc::toProjection(d);
    for (size_t i = 0; i < d.tracks.size(); ++i) {
      for (const auto& fx : d.tracks[i].effects) builtin += fx.value("type", std::string()) == "builtin";
      modulators += d.tracks[i].modulators.size();
      CHECK_EQ(saved["tracks"][i].value("effects", json::array()).size(), d.tracks[i].effects.size());  // guardados
      for (const auto& fx : projection["tracks"][i].value("effects", json::array()))
        CHECK_EQ(fx.value("type", std::string()), std::string("plugin"));                              // calados
      CHECK(!projection["tracks"][i].contains("modulators"));
    }
  }
  CHECK(builtin > 0);  // os templates tem-nos (1108 no relatorio): o teste ve-os mesmo
  CHECK(modulators > 0);

  Document d = core::doc::newEmptyDocument();
  const std::string track = core::doc::addAudioTrack(d, "t");
  core::doc::addPluginEffect(d, track, pluginEffect("p", "QQ==")["plugin"]);
  const std::string pid = d.tracks[0].effects[0]["id"].get<std::string>();
  d.tracks[0].automation.push_back({"fx:" + pid + ":vst:7", {{0, 0.5, json::object()}}, json::object()});
  const json projection = core::doc::toProjection(d);
  CHECK_EQ(projection["tracks"][0]["effects"].size(), (size_t)1);
  CHECK_EQ(projection["tracks"][0]["envelopes"].size(), (size_t)1);
}

TEST("caca P06: apagar a pista mostrada nao mete na selecao uma pista que ninguem escolheu") {
  Document d = core::doc::newEmptyDocument();
  const std::string a = core::doc::addAudioTrack(d, "a"), b = core::doc::addAudioTrack(d, "b"),
                    c = core::doc::addAudioTrack(d, "c"), e = core::doc::addAudioTrack(d, "e");
  const std::vector<std::string> before = {a, b, c, e};

  // b mostrada, e tambem selecionada; apaga-se b: fica so e (antes: a, a primeira, mais e).
  Document now = d;
  core::doc::removeTrack(now, b);
  std::set<std::string> also = {e};
  CHECK_EQ(core::doc::shownAfterRemoval(now, before, b, also), e);
  CHECK(also.empty());

  // So b selecionada: mostra a que ficou no lugar dela (c), nao a primeira.
  also.clear();
  CHECK_EQ(core::doc::shownAfterRemoval(now, before, b, also), c);

  // A ultima apagada: a de antes.
  now = d;
  core::doc::removeTrack(now, e);
  CHECK_EQ(core::doc::shownAfterRemoval(now, before, e, also), c);

  // Varias apagadas de uma vez (b e c, com c selecionada): as que ja nao existem saem da selecao.
  now = d;
  core::doc::removeTrack(now, b);
  core::doc::removeTrack(now, c);
  also = {c};
  CHECK_EQ(core::doc::shownAfterRemoval(now, before, b, also), e);
  CHECK(also.empty());

  // Nenhuma pista: nada mostrado.
  now = core::doc::newEmptyDocument();
  CHECK_EQ(core::doc::shownAfterRemoval(now, before, a, also), std::string());
}

TEST("caca 2, P05: converter 44,1 kHz para 48 kHz da o numero certo de frames") {
  for (const auto& [from, to, frames, want] : std::vector<std::tuple<double, double, int64_t, int64_t>>{
           {44100, 48000, 44100, 48000}, {48000, 44100, 48000, 44100}, {44100, 96000, 44100, 96000}, {22050, 48000, 22050, 48000},
           {44100, 48000, 44101, 48002}}) {  // o ultimo nao e exacto: arredonda para cima
    core::AudioData in;
    in.sampleRate = from;
    in.L = coretest::sine(440, from, (int)frames, 0.5f);
    in.R = in.L;
    CHECK_EQ(core::resample(in, to).frames(), want);
  }
}

TEST("caca 2, IPC: uma resposta com um campo event continua a ser a resposta") {
  using core::client::EngineLine;
  using core::client::readEngineLine;
  EngineLine r = readEngineLine(R"({"id":5,"ok":true,"result":{"a":1},"event":{"x":1}})");
  CHECK(r.kind == EngineLine::Kind::Reply);
  CHECK_EQ(r.id, (int64_t)5);
  CHECK(r.ok);
  r = readEngineLine(R"({"id":6,"ok":false,"error":"no","event":"job"})");
  CHECK(r.kind == EngineLine::Kind::Reply);
  CHECK_EQ(r.error, std::string("no"));
  // Os eventos de verdade nao mudam.
  CHECK(readEngineLine(R"({"event":"ready","protocol":1})").kind == EngineLine::Kind::Ready);
  CHECK(readEngineLine(R"({"event":"job","jobId":3})").kind == EngineLine::Kind::Event);
  CHECK(readEngineLine(R"({"event":{"a":1}})").kind == EngineLine::Kind::Event);  // sem id: um evento, como antes
}

TEST("caca 2, U-5: a duracao de um preview com sampleRate 0 e 0, nao infinito") {
  core::preview::Sound s;
  s.peaks.assign(10, 0.5f);
  s.sampleRate = 0;
  CHECK_EQ(s.seconds(), 0.0);
  s.sampleRate = 48000;
  CHECK(std::abs(s.seconds() - 10.0 * 128 / 48000) < 1e-12);
}

TEST("caca 2, U-8: os tres textos que faltavam tem traducao") {
  std::ifstream f(CORE_LANG_DIR "/pt-PT.json", std::ios::binary);
  const json lang = json::parse(f);
  for (const char* key : {"Rename pattern", "Save as template", "This pattern, all its rows, goes into Templates > Mine."})
    CHECK(lang["strings"].contains(key));
}

TEST("caca 2, P08: um clip so de NaN e um ficheiro estragado, nao silencio") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  core::sampling::Channels broken = {std::vector<float>(4800, nan), std::vector<float>(4800, nan)};
  CHECK(core::sampling::hasBrokenSamples(broken));
  CHECK_EQ(core::sampling::normalizePeak(broken), 0.0);  // nada muda
  core::sampling::Channels one = {coretest::sine(440, 48000, 4800, 0.5f)};
  CHECK(!core::sampling::hasBrokenSamples(one));
  one[0][100] = std::numeric_limits<float>::infinity();
  CHECK(core::sampling::hasBrokenSamples(one));
  core::sampling::Channels silent = {std::vector<float>(4800, 0.0f)};
  CHECK(!core::sampling::hasBrokenSamples(silent));  // o silencio continua silencio
}

TEST("caca 2, seccao 4: um ppq que nao e 960 tem uma mensagem que se entende") {
  json j = core::doc::toJson(core::doc::newEmptyDocument());
  j["transport"]["ppq"] = 480;
  std::string message;
  try { (void)core::doc::parseDocument(j); } catch (const core::doc::DocumentError& e) { message = e.what(); }
  CHECK(message.find("must be 960") != std::string::npos);
  CHECK(message.find("between") == std::string::npos);
}

// Auditoria 2026-11-17, item 1: o randomizador dava nomes de ate 80 bytes e o leitor (document.cpp) recusa
// padroes com nome acima de 40 bytes, por isso guardar e autosave falhavam numa musica que a app aceitou.
TEST("caca audit 2026-11-17 #1: os nomes que o randomizador da aos padroes entram no leitor e guardam-se") {
  const fs::path dir = freshDir("audit1");
  Document d = core::doc::newTemplateDocument();
  d.extra["template"] = {{"style", "Dark psycore"}};
  d.lengthTicks = 32 * 4 * core::doc::kPpq;  // duas seccoes de 16 compassos: e assim que o "second roll" toca
  for (auto& t : d.tracks)
    for (auto& c : t.clips) c.lengthTicks = d.lengthTicks;
  // A primeira chamase 20 "ç" (40 bytes): um corte feito por bytes a meio deles parte um caracter ao meio.
  std::string cedillas;
  for (int i = 0; i < 20; ++i) cedillas += "\xc3\xa7";
  core::doc::addSection(d, cedillas, 0, 16 * 4 * core::doc::kPpq);
  core::doc::addSection(d, std::string(40, 'x'), 16 * 4 * core::doc::kPpq, 16 * 4 * core::doc::kPpq);
  for (auto& s : d.sections) s.extra["energy"] = 4;  // com energia 2 ou mais o baixo reescreve-se
  std::set<std::string> beforeIds;
  for (const auto& p : core::doc::trackById(d, "bass").patterns) beforeIds.insert(p.id);
  const std::vector<std::string> did = core::doc::randomizeStyle(d, {3, 2, {"bass"}});
  size_t created = 0;
  std::set<std::string> bassNames;
  for (const auto& p : core::doc::trackById(d, "bass").patterns)
    if (!beforeIds.count(p.id)) { ++created; bassNames.insert(p.name); }
  CHECK(!did.empty());
  CHECK_EQ(created, (size_t)6);                  // roll, turn e second roll em cada uma das duas seccoes
  CHECK_EQ(bassNames.size(), created);           // e todos com nome diferente uns dos outros
  size_t over = 0, split = 0;
  for (const auto& t : d.tracks)
    for (const auto& p : t.patterns) {
      if (p.name.size() > 40) ++over;            // o limite do leitor sao 40 BYTES
      if (!validUtf8(p.name)) ++split;           // e um corte a meio de um caracter nao e UTF-8
    }
  CHECK_EQ(over, (size_t)0);
  CHECK_EQ(split, (size_t)0);
  core::doc::saveFile(d, dir / "song.core");     // antes (relatorio): SaveError na leitura de confirmacao
  CHECK_EQ(core::doc::openFile(dir / "song.core").document.tracks.size(), d.tracks.size());
  core::doc::writeAutosave(dir, "core-audit-1", 4242, d, "", 1);  // antes: a mesma recusa, mais alto
  CHECK(fs::exists(core::doc::autosavePath(dir, "core-audit-1")));
}

// O mesmo buraco do item 1, pelo rack. A janela "Rename pattern" aceita 60 caracteres (PatternsWindow.cpp:795,
// substring(0, 60)) e o nome do slot vai direitinho para o nome de um padrao (pattern_rack.cpp:184 e :294) — que
// o leitor recusa acima de 40 BYTES. 60 "ç" sao 120 bytes: a musica guarda-se e volta a abrir-se, e o autosave
// passa a falhar calado na cancao que a app acabou de aceitar.
TEST("caca audit #1 (irmao): os nomes que o rack da aos padroes entram no leitor") {
  const fs::path dir = freshDir("audit1-rack");
  std::string name;
  for (int i = 0; i < 60; ++i) name += "\xc3\xa7";  // 120 bytes, 60 letras: o maximo do dialog
  Document d = core::doc::newTemplateDocument();
  const auto shown = core::doc::rackSlots(d);
  CHECK(!shown.empty());
  core::doc::renameRackSlot(d, shown.front().id, name);   // o que o dialog faz
  const std::string duplicated = core::doc::duplicateRackSlot(d, shown.front().id);  // escreve o nome em cada copia
  const std::string added = core::doc::addRackSlot(d, name, 4);                       // slot novo, sem padroes
  core::doc::rackPatternFor(d, added, "kick");            // ai o padrao nasce com o nome do slot
  size_t over = 0, split = 0;
  for (const auto& t : d.tracks)
    for (const auto& p : t.patterns) {
      if (p.name.size() > 40) ++over;
      if (!validUtf8(p.name)) ++split;
    }
  CHECK_EQ(over, (size_t)0);
  CHECK_EQ(split, (size_t)0);
  for (const auto& s : core::doc::rackSlots(d)) {
    CHECK(s.name.size() <= 40);
    CHECK(validUtf8(s.name));
  }
  CHECK(!duplicated.empty());
  core::doc::saveFile(d, dir / "song.core");
  CHECK_EQ(core::doc::openFile(dir / "song.core").document.tracks.size(), d.tracks.size());
  core::doc::writeAutosave(dir, "core-audit-1", 4242, d, "", 1);
  CHECK(fs::exists(core::doc::autosavePath(dir, "core-audit-1")));

  // O controlo: um nome curto passa pelo rack exactamente como foi escrito, com " copy" atraz.
  Document e = core::doc::newTemplateDocument();
  const std::string brk = core::doc::addRackSlot(e, "Break", 4);
  CHECK_EQ(core::doc::rackPatternFor(e, brk, "kick").name, std::string("Break"));
  const std::string cp = core::doc::duplicateRackSlot(e, brk);
  const auto after = core::doc::rackSlots(e);
  const core::doc::PatternSlot* found = core::doc::findRackSlot(after, cp);
  CHECK(found != nullptr);
  if (found) CHECK_EQ(found->name, std::string("Break copy"));
}

// Os outros tres sitios que colavam um sufixo a um nome e cortavam por bytes: a copia ficava com o MESMO nome
// da origem, e nao se distinguia dela na janela nem na cancao guardada.
TEST("caca audit #1 (irmao): duplicar com o nome todo cheio da um nome diferente") {
  std::string forty;
  for (int i = 0; i < 20; ++i) forty += "\xc3\xa7";              // 40 bytes: o limite de uma seccao e de um padrao
  CHECK_EQ(forty.size(), (size_t)40);

  Document d = core::doc::newEmptyDocument();
  const std::string a = core::doc::addSection(d, forty, 0, 4 * core::doc::kPpq);
  const std::string b = core::doc::duplicateSection(d, a);
  std::string copied;
  for (const auto& s : d.sections) if (s.id == b) copied = s.name;
  CHECK(!copied.empty());
  CHECK(copied != forty);                                        // o "2" tem de se ver
  CHECK(copied.size() <= 40);
  CHECK(validUtf8(copied));
  CHECK(copied.find("2") != std::string::npos);

  Document e = core::doc::newTemplateDocument();
  core::doc::Track& kick = core::doc::trackById(e, "kick");
  const std::string pid = kick.patterns.front().id;
  kick.patterns.front().name = forty;
  const std::string second = core::doc::addClip(kick, pid, 4 * core::doc::kPpq, kick.findPattern(pid)->lengthTicks);
  const std::string own = core::doc::makeClipUnique(kick, second);   // "Independent copy" do clique direito
  std::string unique;
  for (const auto& p : kick.patterns) if (p.id == own) unique = p.name;
  CHECK(!unique.empty());
  CHECK(unique != forty);
  CHECK(unique.size() <= 40);
  CHECK(validUtf8(unique));

  Document f = core::doc::newEmptyDocument();
  std::string eighty;
  for (int i = 0; i < 40; ++i) eighty += "\xc3\xa7";                // 80 bytes: o limite de uma faixa
  const std::string track = core::doc::addAudioTrack(f, eighty);
  const std::string twin = core::doc::duplicateTrack(f, track);
  std::string twinName;
  for (const auto& t : f.tracks) if (t.id == twin) twinName = t.name;
  CHECK(!twinName.empty());
  CHECK(twinName != eighty);
  CHECK(twinName.size() <= 80);
  CHECK(validUtf8(twinName));

  // O controlo: com nome curto o sufixo e exactamente o que sempre foi.
  Document g = core::doc::newEmptyDocument();
  const std::string shortA = core::doc::addSection(g, "Intro", 0, 4 * core::doc::kPpq);
  const std::string shortB = core::doc::duplicateSection(g, shortA);
  std::string shortCopy;
  for (const auto& s : g.sections) if (s.id == shortB) shortCopy = s.name;
  CHECK_EQ(shortCopy, std::string("Intro 2"));
}

CORE_TEST_MAIN
