// Testes do MIDI (midi.hpp): exportar a musica como toca (rolls expandidos, loops, fim da musica, tempo) e importar
// .mid para pistas e padroes. Os ficheiros sao gerados nos testes.
#include <filesystem>
#include "support/check.hpp"
#include "edit.hpp"
#include "midi.hpp"

using namespace core::doc;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kBar = 4 * kPpq;
// A song with one track: a 1-bar pattern looping over 2 bars, one plain note and one note with a 4-hit roll.
Document rollSong() {
  Document d = newEmptyDocument();
  d.bpm = 150;
  d.lengthTicks = 2 * kBar;
  const std::string id = addTrack(d, "", "Snare");
  Track& t = trackById(d, id);
  Pattern& p = t.patterns[0];
  p.lengthTicks = kBar;
  Note a; a.id = "n1"; a.tick = 0; a.length = 480; a.pitch = 38; a.velocity = 1.0;
  Note r; r.id = "n2"; r.tick = 2 * kPpq; r.length = kPpq; r.pitch = 40; r.velocity = 0.5;
  r.hasRoll = true; r.roll.count = 4; r.roll.velocityFrom = 0.25; r.roll.velocityTo = 1.0;
  p.notes = {a, r};
  t.clips = {{"c1", p.id, 0, 2 * kBar, 0, json::object()}};
  return d;
}
}  // namespace

TEST("MIDI exportar: os rolls saem expandidos como o motor os toca, em cada repeticao do loop") {
  const MidiFile f = songToMidi(rollSong());
  CHECK_EQ(f.ppq, 960);
  CHECK_EQ(f.tracks.size(), (size_t)1);
  CHECK_EQ(f.tracks[0].name, std::string("Snare"));
  const auto& n = f.tracks[0].notes;
  CHECK_EQ(n.size(), (size_t)10);  // (1 + 4) por compasso, 2 compassos
  // Compasso 1: a nota simples, depois 4 batidas de 1/16 a 85%.
  CHECK_EQ(n[0].tick, (int64_t)0);
  CHECK_EQ(n[0].length, (int64_t)480);
  CHECK_EQ(n[0].velocity, 127);
  for (int i = 0; i < 4; ++i) {
    CHECK_EQ(n[(size_t)1 + i].tick, (int64_t)(2 * kPpq + i * 240));
    CHECK_EQ(n[(size_t)1 + i].length, (int64_t)204);  // 240 * 0.85
    CHECK_EQ(n[(size_t)1 + i].pitch, 40);
  }
  CHECK_EQ(n[1].velocity, 32);    // 0.25 * 127
  CHECK_EQ(n[4].velocity, 127);   // 1.0
  CHECK_EQ(n[5].tick, kBar);      // a repeticao
  CHECK_EQ(n[9].tick, kBar + 2 * kPpq + 3 * 240);
}

TEST("MIDI exportar: o fim da musica corta, pistas mudas ficam de fora, o tempo e as rampas vao no ficheiro") {
  Document d = rollSong();
  d.lengthTicks = kBar + kPpq;  // corta a meio do 2o compasso
  const MidiFile f = songToMidi(d);
  CHECK_EQ(f.tracks[0].notes.size(), (size_t)6);  // compasso 1 inteiro + a nota simples do 2o
  CHECK_EQ(f.tracks[0].notes.back().length, (int64_t)480);
  d.lengthTicks = kBar + 240;
  CHECK_EQ(songToMidi(d).tracks[0].notes.back().length, (int64_t)240);  // cortada pelo fim
  d.tracks[0].mix.mute = true;
  CHECK(songToMidi(d).tracks.empty());
  MidiExportOptions all;
  all.mutedTracks = true;
  CHECK_EQ(songToMidi(d, all).tracks.size(), (size_t)1);
  Document t = rollSong();
  t.tempoMap = {{kBar, 180, json::object(), true}};
  const MidiFile g = songToMidi(t);
  CHECK(g.tempos.size() > 4);
  CHECK_NEAR(g.tempos.back().bpm, 180, 1e-9);
  CHECK(g.tempos[1].bpm > 150 && g.tempos[1].bpm < 180);
}

TEST("MIDI: escrever e ler da as mesmas notas e o mesmo tempo") {
  const MidiFile f = songToMidi(rollSong());
  const MidiFile back = parseMidi(writeMidi(f));
  CHECK_EQ(back.ppq, 960);
  CHECK_EQ(back.tracks.size(), (size_t)1);
  CHECK_EQ(back.tracks[0].name, std::string("Snare"));
  CHECK_EQ(back.tracks[0].notes.size(), f.tracks[0].notes.size());
  for (size_t i = 0; i < f.tracks[0].notes.size(); ++i) {
    CHECK_EQ(back.tracks[0].notes[i].tick, f.tracks[0].notes[i].tick);
    CHECK_EQ(back.tracks[0].notes[i].length, f.tracks[0].notes[i].length);
    CHECK_EQ(back.tracks[0].notes[i].pitch, f.tracks[0].notes[i].pitch);
    CHECK_EQ(back.tracks[0].notes[i].velocity, f.tracks[0].notes[i].velocity);
  }
  CHECK(!back.tempos.empty());
  CHECK_NEAR(back.tempos[0].bpm, 150, 0.01);
  // Ficheiro em disco.
  const fs::path file = fs::temp_directory_path() / "core-midi-test.mid";
  saveMidiFile(exportMidi(rollSong()), file);
  CHECK_EQ(parseMidi(readMidiFile(file)).tracks[0].notes.size(), (size_t)10);
}

TEST("MIDI ler: running status, note-on com velocidade 0, formato 0 com dois canais, 96 ticks por seminima") {
  // Formato 0, 96 ppq: canal 1 (36) e canal 10 (42), running status e note-on 0 como note-off.
  const std::vector<uint8_t> bytes = {
      'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
      'M', 'T', 'r', 'k', 0, 0, 0, 30,
      0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,  // 120 BPM
      0x00, 0x90, 36, 100,
      0x00, 42, 0,                                // running status: nota 42 com vel 0 (nada a fechar)
      0x00, 0x99, 42, 90,
      0x30, 0x90, 36, 0,                          // 48 ticks depois: fecha o 36
      0x00, 0x89, 42, 0,
      0x00, 0xFF, 0x2F, 0x00};
  const MidiFile f = parseMidi(bytes);
  CHECK_EQ(f.ppq, 96);
  CHECK_EQ(f.tracks.size(), (size_t)2);  // separado por canal
  CHECK_NEAR(f.tempos.at(0).bpm, 120, 1e-6);
  CHECK_EQ(f.tracks[0].notes.at(0).length, (int64_t)48);
  CHECK_EQ(f.tracks[1].notes.at(0).pitch, 42);
  CHECK_THROWS(parseMidi({'R', 'I', 'F', 'F'}));
  CHECK_THROWS(parseMidi(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 30)));  // cortado
}

TEST("MIDI importar: cada pista vira uma pista do Core com padroes; compassos iguais sao o mesmo padrao") {
  MidiFile f;
  f.ppq = 480;
  MidiTrack bass;
  bass.name = "Bass";
  for (int bar = 0; bar < 8; ++bar)  // 8 compassos, os 4 primeiros iguais aos 4 seguintes
    for (int s = 0; s < 4; ++s) bass.notes.push_back({(int64_t)(bar * 4 + s) * 480, 240, (bar % 4) == 3 ? 38 : 36, 100, 0});
  f.tracks = {bass, MidiTrack{"vazia", {}}};
  f.tempos = {{0, 145}};
  Document d = newEmptyDocument();
  d.lengthTicks = 4 * kBar;
  MidiImportOptions o;
  o.useTempo = true;
  const MidiImportResult r = importMidi(d, f, o);
  CHECK_EQ(r.trackIds.size(), (size_t)1);
  CHECK_EQ(r.patterns, 1);
  CHECK_EQ(r.clips, 2);
  CHECK_EQ(r.notes, 32);
  CHECK(r.songLengthened);
  CHECK_EQ(d.lengthTicks, 8 * kBar);
  CHECK_NEAR(d.bpm, 145, 1e-9);
  const Track& t = trackById(d, r.trackIds[0]);
  CHECK_EQ(t.name, std::string("Bass"));
  CHECK_EQ(t.instrument.kind, std::string("none"));
  CHECK_EQ(t.patterns[0].notes[0].length, (int64_t)480);  // 240 a 480 ppq -> 480 a 960
  // E toca o mesmo: exportar devolve as mesmas notas, nos mesmos tempos.
  const MidiFile back = songToMidi(d);
  CHECK_EQ(back.tracks[0].notes.size(), (size_t)32);
  CHECK_EQ(back.tracks[0].notes[13].tick, (int64_t)13 * 960);
  // A musica continua valida (guarda e reabre).
  CHECK_EQ(toJson(parseDocument(toJson(d))).dump(), toJson(d).dump());
}

TEST("MIDI importar: nomes estragados ficam validos; sem notas e recusado sem mexer em nada") {
  MidiFile f;
  MidiTrack t;
  t.name = std::string("Ba\xE7o \x01", 6);  // Latin-1 c cedilhado e um caracter de controlo
  t.notes = {{0, 960, 60, 90, 0}};
  f.tracks = {t};
  Document d = newEmptyDocument();
  const MidiImportResult r = importMidi(d, f);
  const std::string name = trackById(d, r.trackIds[0]).name;
  CHECK_EQ(name, std::string("Ba\xC3\xA7o"));
  CHECK_EQ(toJson(parseDocument(toJson(d))).dump(), toJson(d).dump());
  Document e = newEmptyDocument();
  const std::string before = toJson(e).dump();
  CHECK_THROWS(importMidi(e, MidiFile{}));
  MidiImportOptions bad;
  bad.barsPerPattern = 0;
  CHECK_THROWS(importMidi(e, f, bad));
  CHECK_EQ(toJson(e).dump(), before);
}

TEST("MIDI importar para um padrao: notas no padrao a partir de um ponto, as de fora ficam de fora") {
  MidiFile f;
  f.ppq = 960;
  f.tracks = {MidiTrack{"x", {{0, 240, 60, 100, 0}, {960, 240, 62, 100, 0}, {8 * 960, 240, 64, 100, 0}}}};
  Pattern p;
  p.id = "p1";
  p.lengthTicks = kBar;
  CHECK_EQ(importMidiIntoPattern(p, f, 960), 2);  // a terceira passa o fim
  CHECK_EQ(p.notes.size(), (size_t)2);
  CHECK_EQ(p.notes[0].tick, (int64_t)960);
  CHECK(p.notes[0].id != p.notes[1].id);
  CHECK_THROWS(importMidiIntoPattern(p, f, 0, 3));
}

// Auditoria 2026-11-17, item 2: com o cabecalho a prometer mais pistas do que as que ha nos bytes, o leitor
// parava em silencio e a app dizia "Importadas N pistas" com uma lista mais curta. midi.hpp promete DocumentError.
TEST("MIDI ler: o cabecalho a prometer mais pistas do que as que o ficheiro tem da erro") {
  // Uma MTrk completa (19 bytes): tempo, uma nota, a sua pausa e o fim da pista.
  const std::vector<uint8_t> chunk = {'M', 'T', 'r', 'k', 0, 0, 0, 19,
                                      0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,   // 120 BPM
                                      0x00, 0x90, 60, 100,
                                      0x60, 0x80, 60, 0,
                                      0x00, 0xFF, 0x2F, 0x00};
  // Formato 1, 960 por seminima; a cabecalho di 2 pistas, so uma esta aqui.
  std::vector<uint8_t> cut = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x03, 0xC0};
  cut.insert(cut.end(), chunk.begin(), chunk.end());
  CHECK_THROWS(parseMidi(cut));
  std::string message;
  try {
    (void)parseMidi(cut);
  } catch (const DocumentError& e) {
    message = e.what();
  }
  CHECK(message.find("1 of 2") != std::string::npos);  // o que se leu contra o que se prometeu
  // O mesmo ficheiro com o numero de pistas certo le-se.
  std::vector<uint8_t> whole = cut;
  whole[11] = 1;
  const MidiFile f = parseMidi(whole);
  CHECK_EQ(f.ppq, 960);
  CHECK_EQ(f.tracks.size(), (size_t)1);
  CHECK_EQ(f.tracks[0].notes.size(), (size_t)1);
  // Pistas a mais do que as prometidas, e bytes depois delas, continua a ler-se.
  std::vector<uint8_t> more = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0x03, 0xC0};
  more.insert(more.end(), chunk.begin(), chunk.end());
  more.insert(more.end(), chunk.begin(), chunk.end());
  CHECK_EQ(parseMidi(more).tracks.size(), (size_t)1);
  // Um chunk que nao e MTrk no meio nao conta como pista: as duas leem-se.
  std::vector<uint8_t> listed = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x03, 0xC0};
  listed.insert(listed.end(), chunk.begin(), chunk.end());
  const std::vector<uint8_t> listChunk = {'L', 'I', 'S', 'T', 0, 0, 0, 4, 'i', 'n', 'f', 'o'};
  listed.insert(listed.end(), listChunk.begin(), listChunk.end());
  listed.insert(listed.end(), chunk.begin(), chunk.end());
  const MidiFile both = parseMidi(listed);
  CHECK_EQ(both.tracks.size(), (size_t)2);
}

CORE_TEST_MAIN
