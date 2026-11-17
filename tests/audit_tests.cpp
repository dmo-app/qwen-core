// Testes dos bugs da auditoria externa de 2026-11-16 (RELATORIO-BUGS-MELHORIAS.md, B01-B06). Cada teste repete o
// caso que a auditoria reproduziu e falhava antes da correcao. B05 (caminhos Unicode na linha de comandos) so se
// ve no Windows: esta no cli_unicode.cmake.
#include <filesystem>
#include <fstream>
#include "support/check.hpp"
#include "support/signals.hpp"
#include "assets.hpp"
#include "autosave.hpp"
#include "edit.hpp"
#include "samples.hpp"
#include "storage.hpp"
#include "templates.hpp"
#include "core/project.hpp"
#include "core/assets.hpp"
#include "core/render_job.hpp"
#include "core/server.hpp"
#include "io/audio_file.hpp"
#include "io/wav.hpp"

namespace fs = std::filesystem;
using core::doc::Document;
using core::json;

namespace {
fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / "core-audit-tests" / name;
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// 0,5 s de um seno de 440 Hz a 0,8, estereo; com `opposite` o canal direito e o esquerdo invertido.
fs::path sineFile(const fs::path& file, bool opposite) {
  const auto L = coretest::sine(440, 48000, 24000, 0.8f);
  std::vector<float> R = L;
  if (opposite) for (auto& x : R) x = -x;
  core::writeWav(file.string(), L, R, 48000, 24);
  return file;
}

json audioProject(const fs::path& wav, double gainDb) {
  json clip = {{"path", wav.string()}, {"startTick", 0}, {"durationSeconds", 0.5}, {"gainDb", gainDb}};
  json track = {{"id", "audio1"}, {"instrument", "audio"}, {"gainDb", 0}, {"audioClips", json::array({clip})}};
  return {{"format", core::kProjectFormat}, {"version", 2}, {"bpm", 120}, {"lengthTicks", 960}, {"masterGainDb", 0},
          {"tracks", json::array({track})}};
}
}  // namespace

TEST("auditoria B01: importar take..1.wav deixa guardar e fazer autosave; um '..' a sair da pasta continua recusado") {
  for (const std::string name : {"take1.wav", "take..1.wav"}) {
    const fs::path dir = freshDir(name == "take1.wav" ? "b01-control" : "b01-doubledot");
    const fs::path song = dir / "song.core";
    Document d = core::doc::newEmptyDocument();
    core::doc::saveFile(d, song);
    sineFile(dir / name, false);
    const std::string asset = core::doc::importAudio(d, dir / name, dir);
    const std::string track = core::doc::addAudioTrack(d, "take");
    core::doc::addAudioClip(d, track, asset, 0);
    core::doc::saveFile(d, song);  // antes: "assets[0].path: must be a path inside the project folder"
    CHECK_EQ(core::doc::openFile(song).document.assets.size(), (size_t)1);
    core::doc::writeAutosave(dir / "autosaves", "audit", 0, d, song.string(), 1);
    CHECK(fs::exists(core::doc::assetFile(d.assets[0], dir)));
  }
  // O nome gerado ja nao leva dois pontos seguidos.
  Document d = core::doc::newEmptyDocument();
  const fs::path dir = freshDir("b01-name");
  sineFile(dir / "a..b...c.wav", false);
  const std::string id = core::doc::importAudio(d, dir / "a..b...c.wav", dir);
  CHECK(d.findAsset(id)->path.find("..") == std::string::npos);
  // Um componente ".." verdadeiro continua recusado.
  json j = core::doc::toJson(d);
  for (const std::string bad : {"../x.wav", "Audio/../../x.wav", "Audio\\..\\x.wav", ".."}) {
    j["assets"][0]["path"] = bad;
    CHECK_THROWS(core::doc::parseDocument(j));
  }
  j["assets"][0]["path"] = "Audio/take..1.wav";
  CHECK_EQ(core::doc::parseDocument(j).assets[0].path, std::string("Audio/take..1.wav"));
}

TEST("auditoria B02: uma pista chamada master nao partilha os estados de plugin do master") {
  Document d = core::doc::newEmptyDocument();
  const std::string track = core::doc::addAudioTrack(d, "master");  // importar master.wav sem pista escolhida
  CHECK(track != "master");
  json plugin = {{"format", "vst3"}, {"id", "audit-effect"}, {"file", "audit-effect.vst3"}, {"state", "QQ=="}};
  core::doc::addPluginEffect(d, track, plugin);
  plugin["state"] = "Qg==";
  core::doc::addPluginEffect(d, "", plugin);
  const std::string masterKey = core::doc::pluginKey("", d.masterEffects[0]["id"]);
  CHECK(core::doc::pluginKey(track, d.tracks[0].effects[0]["id"]) != masterKey);
  CHECK_EQ(core::doc::applyPluginStates(d, {{masterKey, "Qw=="}}), 1);
  CHECK_EQ(d.tracks[0].effects[0]["plugin"]["state"].get<std::string>(), std::string("QQ=="));
  CHECK_EQ(d.masterEffects[0]["plugin"]["state"].get<std::string>(), std::string("Qw=="));

  // Um ficheiro que ja tem a pista "master" (feito antes da correcao) abre com ela renomeada, e o que lhe chegava
  // (saida de outra pista) segue-a.
  Document old = core::doc::newEmptyDocument();
  const std::string bus = core::doc::addAudioTrack(old, "bus");
  const std::string feeder = core::doc::addAudioTrack(old, "feeder");
  json j = core::doc::toJson(old);
  for (auto& t : j["tracks"]) {
    if (t["id"] == bus) { t["id"] = "master"; t["instrument"] = {{"kind", "bus"}}; }
    if (t["id"] == feeder) t["mix"]["output"] = "master";
  }
  const Document opened = core::doc::parseDocument(j);
  for (const auto& t : opened.tracks) CHECK(t.id != "master");
  const auto* f = opened.findTrack(feeder);
  CHECK(f != nullptr);
  CHECK(opened.findTrack(f->mix.output) != nullptr);
  CHECK_EQ(opened.findTrack(f->mix.output)->instrument.kind, std::string("bus"));
}

TEST("auditoria B03: recortar um template a meio de uma rampa de tempo guarda a rampa") {
  Document d = core::doc::newEmptyDocument();
  core::doc::addTrack(d, "none", "Notes");
  d.bpm = 120;
  d.lengthTicks = 16 * core::doc::kPpq;
  d.loop.endTick = d.lengthTicks;
  core::doc::TempoPoint end;
  end.tick = 8 * core::doc::kPpq;
  end.bpm = 240;
  end.ramp = true;
  core::doc::setTempoMap(d, {end});
  core::doc::TemplateInfo info;
  info.id = "audit-tempo";
  info.name = "Audit tempo";
  const int64_t a = 2 * core::doc::kPpq, b = 6 * core::doc::kPpq;
  const Document cut = core::doc::templateFromRange(d, info, a, b).document;
  CHECK_EQ(cut.lengthTicks, b - a);
  CHECK_NEAR(core::doc::bpmAt(cut, 0), 150, 1e-9);                                     // antes: 120
  CHECK_NEAR(core::doc::bpmAt(cut, cut.lengthTicks - 1), core::doc::bpmAt(d, b - 1), 1e-6);  // antes: 120
  CHECK_NEAR(core::doc::bpmAt(cut, 2 * core::doc::kPpq), core::doc::bpmAt(d, a + 2 * core::doc::kPpq), 1e-6);
  CHECK_EQ(core::doc::toJson(core::doc::parseDocument(core::doc::toJson(cut))).dump(), core::doc::toJson(cut).dump());

  // Apagar compassos no meio de uma rampa: antes do corte tudo igual, depois toca o que tocava do fim do corte.
  Document m = d;
  core::doc::deleteTime(m, 2 * core::doc::kPpq, 2 * core::doc::kPpq);
  for (int64_t t : {(int64_t)0, (int64_t)960, (int64_t)1900})
    CHECK_NEAR(core::doc::bpmAt(m, t), core::doc::bpmAt(d, t), 0.05);
  for (int64_t t : {(int64_t)1920, (int64_t)3000, (int64_t)5000})
    CHECK_NEAR(core::doc::bpmAt(m, t), core::doc::bpmAt(d, t + 2 * core::doc::kPpq), 0.05);
  // Sem mapa de tempo, nada muda (o documento continua no esquema 2).
  Document plain = core::doc::newEmptyDocument();
  core::doc::deleteTime(plain, 0, core::doc::kPpq);
  CHECK(plain.tempoMap.empty());
}

TEST("auditoria B04: exportar em mono mede o que o ficheiro mono leva, nao o pico estereo") {
  const fs::path dir = freshDir("b04");
  const fs::path anti = sineFile(dir / "antiphase.wav", true);
  core::Server server(nullptr, 48000);
  json r = server.handle({{"cmd", "project.load"}, {"args", {{"project", audioProject(anti, 6)}}}});
  CHECK(r["ok"].get<bool>());
  for (int bits : {24, 32}) {
    const fs::path out = dir / ("mono-" + std::to_string(bits) + ".wav");
    r = server.handle({{"cmd", "render"}, {"args", {{"path", out.string()}, {"bits", bits}, {"mono", true}, {"tailMs", 0}}}});
    CHECK(r["ok"].get<bool>());  // antes: recusado por "clipping" de +4,1 dBFS que o mono nao tem
    CHECK(fs::exists(out));
    if (r["ok"].get<bool>()) CHECK(r["result"]["peak"].get<double>() < 1e-6);
  }
  // Controlo: em estereo o mesmo sinal passa de 0 dBFS e continua recusado.
  r = server.handle({{"cmd", "render"}, {"args", {{"path", (dir / "stereo.wav").string()}, {"bits", 32}, {"tailMs", 0}}}});
  CHECK(!r["ok"].get<bool>());
  // ExportLevel: um NaN num lado conta como estragado tambem em mono.
  core::ExportLevel level;
  const float L[2] = {0.5f, std::nanf("")}, R[2] = {-0.5f, 0.0f};
  level.addMono(L, R, 2);
  CHECK(level.broken);
  CHECK_NEAR(level.peak, 0, 0);
}

TEST("auditoria B06: render com endTick 0 e uma regiao vazia e e recusado, como no render.start") {
  const fs::path dir = freshDir("b06");
  const fs::path wav = sineFile(dir / "sine.wav", false);
  core::Server server(nullptr, 48000);
  CHECK(server.handle({{"cmd", "project.load"}, {"args", {{"project", audioProject(wav, -6)}}}})["ok"].get<bool>());
  const fs::path out = dir / "end-zero.wav";
  json r = server.handle({{"cmd", "render"}, {"args", {{"path", out.string()}, {"bits", 32}, {"endTick", 0}, {"tailMs", 0}}}});
  CHECK(!r["ok"].get<bool>());  // antes: ok e a musica inteira
  CHECK(!fs::exists(out));
  r = server.handle({{"cmd", "render"}, {"args", {{"path", out.string()}, {"bits", 32}, {"endTick", 480}, {"tailMs", 0}}}});
  CHECK(r["ok"].get<bool>());
  r = server.handle({{"cmd", "render"}, {"args", {{"path", out.string()}, {"bits", 32}, {"startTick", 480}, {"endTick", 480}, {"tailMs", 0}}}});
  CHECK(!r["ok"].get<bool>());
  r = server.handle({{"cmd", "render"}, {"args", {{"path", out.string()}, {"bits", 32}, {"tailMs", 0}}}});
  CHECK(r["ok"].get<bool>());  // sem fim: a musica toda
}

// Auditoria 2026-11-17, item 3a: a varredura guardava o registo anterior pelo caminho, o tamanho e a data; um
// ficheiro trocado por dentro, com os mesmos bytes e a mesma data, ficava com o sha256 e o nivel de antes.
TEST("amostras: um wav trocado por dentro, com os mesmos bytes e a mesma data, le de novo") {
  const fs::path dir = freshDir("samples-in-place");
  const fs::path wav = dir / "kick.wav";
  // 0,5 s de um seno a 48 kHz / 24 bits / estereo; `amp` muda o nivel, nunca o numero de bytes.
  const auto write = [&](float amp) {
    const auto L = coretest::sine(440, 48000, 24000, amp);
    core::writeWav(wav.string(), L, L, 48000, 24);
  };
  write(0.8f);
  const core::doc::SampleIndex first = core::doc::scanLibrary(dir);
  CHECK_EQ(first.samples.size(), (size_t)1);
  const std::string sha = first.samples.at(0).sha256;
  const double loud = first.samples.at(0).loudDb;
  const int64_t size = first.samples.at(0).size;
  const auto stamp = fs::last_write_time(wav);
  write(0.2f);                        // o mesmo sitio, os mesmos bytes, outro som
  fs::last_write_time(wav, stamp);    // e a mesma data
  CHECK_EQ((int64_t)fs::file_size(wav), size);
  CHECK(fs::last_write_time(wav) == stamp);  // a data voltou mesmo
  const core::doc::SampleIndex again = core::doc::scanLibrary(dir, &first);
  const core::doc::SampleIndex fresh = core::doc::scanLibrary(dir);  // a expectativa medida, nao escrita a mao
  CHECK_EQ(again.samples.size(), (size_t)1);
  CHECK_EQ(again.samples.at(0).sha256, fresh.samples.at(0).sha256);
  CHECK_EQ(again.samples.at(0).loudDb, fresh.samples.at(0).loudDb);
  CHECK(again.samples.at(0).sha256 != sha);   // o conteudo e outro
  CHECK(again.samples.at(0).loudDb != loud);  // e o nivel foi atras
}

// Auditoria 2026-11-17, item 3b: a cache do motor era guardada por caminho, taxa, tamanho e data; o mesmo
// ficheiro trocado por dentro servia para sempre do cache.
TEST("motor: um wav trocado por dentro, com os mesmos bytes e a mesma data, sai do cache") {
  const fs::path dir = freshDir("assets-in-place");
  const fs::path wav = dir / "take.wav";
  const auto write = [&](int rate, int64_t frames) {  // os mesmos bytes sempre: frames e canais constantes
    const auto L = coretest::sine(440, (double)rate, frames, 0.5f);
    core::writeWav(wav.string(), L, L, rate, 24);
  };
  write(48000, 2000);
  const int64_t size = (int64_t)fs::file_size(wav);
  const auto stamp = fs::last_write_time(wav);
  core::AssetStore store;
  std::string err;
  const auto before = store.get(wav.string(), 48000, &err);
  CHECK(before != nullptr);
  CHECK_EQ(before->frames(), (int64_t)2000);
  write(24000, 2000);                     // as mesmas 2000 amostras a 24 kHz: os mesmos bytes, outro audio
  fs::last_write_time(wav, stamp);
  CHECK_EQ((int64_t)fs::file_size(wav), size);
  CHECK(fs::last_write_time(wav) == stamp);
  const auto after = store.get(wav.string(), 48000, &err);
  core::AssetStore fresh;                 // o que uma carga nova da: a expectativa medida, nao escrita a mao
  std::string err2;
  const auto expected = fresh.get(wav.string(), 48000, &err2);
  CHECK(expected != nullptr);
  CHECK_EQ(expected->frames(), (int64_t)4000);  // 2000 amostras a 24 kHz sao 4000 a 48 kHz
  CHECK_EQ(after->frames(), expected->frames());
  CHECK_EQ((int)after->channels, (int)expected->channels);
  CHECK_EQ(after->sampleRate, expected->sampleRate);
}

CORE_TEST_MAIN
