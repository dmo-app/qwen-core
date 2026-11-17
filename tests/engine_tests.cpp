// Testes do motor (core_engine_lib): sinais gerados, sem fixtures nem goldens presos a um compilador.
// Correr: engine_tests [parte-do-nome]
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "support/check.hpp"
#include "support/signals.hpp"
#include "app/measure.hpp"
#include "core/assets.hpp"
#include "core/engine.hpp"
#include "core/graph.hpp"
#include "core/project.hpp"
#include "core/server.hpp"
#include "io/audio_file.hpp"
#include "io/wav.hpp"

using namespace core;
using namespace coretest;
namespace fs = std::filesystem;

namespace {

const double kSr = 48000;

fs::path tempDir() {
  fs::path d = fs::temp_directory_path() / "core-engine-tests";
  fs::create_directories(d);
  return d;
}

// A song with one audio track playing one file from startTick.
json audioSong(const std::string& path, double durationSeconds, int64_t startTick = 0, double gainDb = 0) {
  json clip = {{"startTick", startTick}, {"path", path}, {"offsetSeconds", 0}, {"durationSeconds", durationSeconds}, {"gainDb", gainDb}};
  json track = {{"id", "audio"}, {"name", "Audio"}, {"instrument", "audio"}, {"audioClips", json::array({clip})}};
  return {{"format", kProjectFormat}, {"version", 2}, {"name", "teste"}, {"bpm", 150.0},
          {"lengthTicks", 4 * 4 * kPpq}, {"masterGainDb", 0}, {"tracks", json::array({track})}};
}

std::string writeSine(const std::string& name, double hz, double seconds, float amp) {
  const auto s = sine(hz, kSr, (int64_t)(seconds * kSr), amp);
  const std::string path = (tempDir() / name).string();
  writeWav(path, s, s, (int)kSr, 32);
  return path;
}

// Uma música com uma pista de plugin: o som vem do plugin falso abaixo.
json pluginSong(double bpm, const json& notes, const json& mods, int64_t patternTicks = 4 * kPpq) {
  json track = {{"id", "lead"}, {"name", "Lead"}, {"instrument", "plugin"},
                {"plugin", {{"format", "vst3"}, {"id", "fake"}}}, {"patternTicks", patternTicks}, {"notes", notes}};
  if (mods.is_array() && !mods.empty()) track["modulators"] = mods;
  return {{"format", kProjectFormat}, {"version", 2}, {"name", "teste"}, {"bpm", bpm},
          {"lengthTicks", 16LL * 4 * kPpq}, {"masterGainDb", 0.0}, {"tracks", json::array({track})}};
}

// Um plugin falso: soma uma constante ao que recebe e fica com todos os eventos MIDI que lhe chegaram.
struct FakeProc final : PluginProcessor {
  std::vector<MidiNote> seen;
  float level = 0.5f;
  void process(float* left, float* right, int frames, const MidiNote* notes, int count) override {
    for (int i = 0; i < count; ++i) seen.push_back(notes[i]);
    for (int i = 0; i < frames; ++i) { left[i] += level; right[i] += level; }
  }
  bool isInstrument() const override { return true; }
  int latencySamples() const override { return 0; }
};

// O mesmo plugin atravessa os grafos, como no tempo real: o que muda é o grafo, não a instância.
struct FakeProvider final : PluginProvider {
  std::shared_ptr<FakeProc> proc = std::make_shared<FakeProc>();
  std::shared_ptr<PluginProcessor> get(const std::string&, const json&, double, bool, std::string* why) override {
    if (why) why->clear();
    return proc;
  }
  void keepOnly(const std::vector<std::string>&) override {}
};

// O que uma saída de áudio fez (audit 14): o Server não devolve nada disto, por isso fica aqui fora.
struct DeviceLog {
  int starts = 0, stops = 0;
  bool running = false, failNext = false;
};

struct FakeOutput final : AudioOutput {
  DeviceLog* log = nullptr;
  json start(Engine&, const json&) override {
    if (log->failNext) throw std::runtime_error("fake: the device is busy");
    ++log->starts;
    log->running = true;
    return {{"sampleRate", 48000.0}, {"blockSize", 512}, {"backend", "fake"}, {"device", "falso"}};
  }
  void stop() override { ++log->stops; log->running = false; }
  json devices(const json&) override { return json::array(); }
};

struct FakeHost {
  std::vector<std::unique_ptr<DeviceLog>> logs;  // vivem tanto quanto o teste
  bool failNext = false, nullNext = false;
  std::unique_ptr<AudioOutput> make() {
    if (nullNext) { nullNext = false; return nullptr; }
    auto l = std::make_unique<DeviceLog>();
    l->failNext = failNext;
    failNext = false;
    DeviceLog* raw = l.get();
    logs.push_back(std::move(l));
    auto out = std::make_unique<FakeOutput>();
    out->log = raw;
    return out;
  }
};

}  // namespace

// --- Medidor de memória (audit 10) -----------------------------------------------------------------------------
// O new global deste executável soma o que lhe é pedido: assim «quanta memória esta chamada tocou» é uma
// medição e não uma estimativa. Só se conta entre liga/desliga, e o libertado volta para o mesmo malloc.
namespace {
std::atomic<size_t> g_allocBytes{0};
bool g_allocOn = false;
void gaugeAdd(size_t n) { if (g_allocOn) g_allocBytes.fetch_add(n, std::memory_order_relaxed); }
size_t gaugeStop() { const size_t bytes = g_allocBytes.load(); g_allocOn = false; return bytes; }
}  // namespace

void* operator new(size_t n) {
  gaugeAdd(n);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) {
  gaugeAdd(n);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  gaugeAdd(n);
  return std::malloc(n ? n : 1);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  gaugeAdd(n);
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// --- Ficheiros WAV ---------------------------------------------------------------------------------------------

TEST("wav: escrever e ler um seno de 32 bits devolve as mesmas amostras") {
  const auto L = sine(440, kSr, 4800, 0.5f);
  const auto R = noise(4800, 7, 0.25f);
  const std::string path = (tempDir() / "roundtrip.wav").string();
  writeWav(path, L, R, (int)kSr, 32);
  AudioData d = readWav(path, 1 << 20);
  CHECK_EQ(d.frames(), (int64_t)4800);
  CHECK_EQ(d.channels, 2);
  CHECK_NEAR(d.sampleRate, kSr, 0);
  double worst = 0;
  for (size_t i = 0; i < L.size() && i < d.L.size(); ++i)
    worst = std::max({worst, (double)std::fabs(d.L[i] - L[i]), (double)std::fabs(d.R[i] - R[i])});
  CHECK_NEAR(worst, 0, 1e-6);
}

TEST("wav: 24 bits fica a menos de 2 passos de quantizacao (-132 dBFS)") {
  const auto L = sine(1000, kSr, 4800, 0.9f);
  const std::string path = (tempDir() / "q24.wav").string();
  writeWav(path, L, L, (int)kSr, 24);
  AudioData d = readWav(path, 1 << 20);
  double worst = 0;
  for (size_t i = 0; i < L.size() && i < d.L.size(); ++i) worst = std::max(worst, (double)std::fabs(d.L[i] - L[i]));
  CHECK_NEAR(worst, 0, 2.0 / 8388608.0);  // medido 2026-11-16: 1,5 passos (escala 2^23-1 na escrita)
}

TEST("wav: ficheiro inexistente da erro, nao audio vazio") {
  CHECK_THROWS(readWav((tempDir() / "nao-existe.wav").string(), 1 << 20));
}

// --- Tempo -----------------------------------------------------------------------------------------------------

TEST("tempo: samplesPerTick a 150 BPM / 48 kHz") {
  // 150 BPM = 0,4 s por batida = 19200 amostras por 960 ticks = 20 amostras por tick.
  CHECK_NEAR(samplesPerTick(150, 48000), 20.0, 1e-12);
  CHECK_NEAR(samplesPerTick(500, 48000) * kPpq, 48000 * 60.0 / 500, 1e-9);
}

// --- Projecto (JSON) -------------------------------------------------------------------------------------------

TEST("projecto: toJson(fromJson(x)) e estavel") {
  Project p = fromJson(audioSong("a.wav", 1.0));
  const std::string once = toJson(p).dump();
  const std::string twice = toJson(fromJson(json::parse(once))).dump();
  CHECK_EQ(once, twice);
}

TEST("projecto: entrada invalida e recusada") {
  json bad = audioSong("a.wav", 1.0);
  bad["tracks"][0]["id"] = "id com espacos";
  CHECK_THROWS(fromJson(bad));
  json noInstrument = audioSong("a.wav", 1.0);
  noInstrument["tracks"][0].erase("instrument");
  CHECK_THROWS(fromJson(noInstrument));
}

// Regressao (2026-11-16): com a saida dos instrumentos proprios, uma musica antiga com automacao num parametro do
// W-Core ("tone") deixava de abrir ("envelope on unknown parameter 'tone'"). Abre, a automacao sai com uma nota.
TEST("projecto: musica antiga com automacao num instrumento proprio abre") {
  Project p;
  CHECK_EQ((p = fromJson(demoProject(145)), (int)p.tracks.size()), 3);
  bool noted = false;
  for (const auto& t : p.tracks) {
    CHECK_EQ(t.instrument, std::string("none"));
    for (const auto& n : t.loadNotes) noted = noted || n.find("tone") != std::string::npos;
  }
  CHECK(noted);
  // A automacao de mistura (gainDb) de uma pista antiga continua.
  json song = demoProject(145);
  song["tracks"][1]["envelopes"].push_back({{"param", "gainDb"}, {"points", {{{"tick", 0}, {"value", -6}}}}});
  Project q = fromJson(song);
  CHECK_EQ((int)q.tracks[1].envelopes.size(), 1);
  CHECK_EQ(q.tracks[1].envelopes[0].param, std::string("gainDb"));
}

// --- Render ----------------------------------------------------------------------------------------------------

TEST("render: musica sem som da silencio finito com a duracao certa") {
  Project p = fromJson(demoProject(145));
  RenderResult r = renderProject(p, kSr, 512, false, 1.0, nullptr, nullptr, nullptr, 0);
  CHECK(allFinite(r.L) && allFinite(r.R));
  CHECK_NEAR(r.peak, 0, 0);
  const double expected = 8 * 4 * kPpq * samplesPerTick(145, kSr) + kSr;
  CHECK_NEAR((double)r.L.size(), expected, 1);
}

TEST("render: clip de audio toca no sitio, ao nivel certo e sem NaN") {
  const std::string path = writeSine("sine440.wav", 440, 2.0, 0.5f);
  const int64_t startTick = 2 * kPpq;  // 2 batidas a 150 BPM = 0,8 s
  const int64_t start = (int64_t)(startTick * samplesPerTick(150, kSr));  // 38400
  Project p = fromJson(audioSong(path, 1.0, startTick));
  AssetStore assets;
  RenderResult r = renderProject(p, kSr, 512, false, 0.5, nullptr, &assets, nullptr, 0);
  CHECK(allFinite(r.L) && allFinite(r.R));
  CHECK_EQ(start, (int64_t)38400);
  // O seno comeca em 0: a primeira amostra acima do limiar fica a menos de 1 ms do inicio do clip.
  const int64_t onset = firstAbove(r.L, 1e-4f);
  CHECK(onset >= start && onset < start + 48);
  if (onset < start || onset >= start + 48) coretest::fail(__FILE__, __LINE__, coretest::pair(onset, start));
  CHECK_NEAR(r.peak, 0.5, 0.02);  // ganho 0 dB, mistura 0 dB: o nivel do ficheiro
  // Antes do clip, silencio absoluto; depois de 1 s de clip, silencio outra vez.
  CHECK_NEAR(peakOf(std::vector<float>(r.L.begin(), r.L.begin() + start)), 0, 0);
  CHECK_NEAR(peakOf(std::vector<float>(r.L.begin() + start + 48000 + 64, r.L.end())), 0, 1e-6);
}

TEST("render: ganho do clip de -6 dB baixa o pico para metade") {
  const std::string path = writeSine("sine1k.wav", 1000, 1.0, 0.5f);
  AssetStore assets;
  RenderResult a = renderProject(fromJson(audioSong(path, 0.5, 0, 0)), kSr, 512, false, 0.1, nullptr, &assets, nullptr, 0);
  RenderResult b = renderProject(fromJson(audioSong(path, 0.5, 0, -6.0206)), kSr, 512, false, 0.1, nullptr, &assets, nullptr, 0);
  CHECK(a.peak > 0.1f);
  CHECK_NEAR(b.peak / a.peak, 0.5, 0.01);
}

TEST("render: o tamanho do bloco nao muda o resultado") {
  const std::string path = writeSine("sine-block.wav", 220, 1.5, 0.4f);
  Project p = fromJson(audioSong(path, 1.2, kPpq / 3));
  AssetStore assets;
  RenderResult ref = renderProject(p, kSr, 512, false, 0.2, nullptr, &assets, nullptr, 0);
  for (int block : {1, 64, 333, 1024}) {
    RenderResult r = renderProject(p, kSr, block, false, 0.2, nullptr, &assets, nullptr, 0);
    CHECK_EQ(r.L.size(), ref.L.size());
    double worst = 0;
    for (size_t i = 0; i < r.L.size() && i < ref.L.size(); ++i) worst = std::max(worst, (double)std::fabs(r.L[i] - ref.L[i]));
    CHECK_NEAR(worst, 0, 0);
  }
}

TEST("render: deterministico na mesma build (duas vezes igual)") {
  const std::string path = (tempDir() / "noise.wav").string();
  const auto n = noise(48000, 99, 0.3f);
  writeWav(path, n, n, (int)kSr, 32);
  Project p = fromJson(audioSong(path, 1.0, kPpq));
  AssetStore assets;
  RenderResult a = renderProject(p, kSr, 512, false, 0.3, nullptr, &assets, nullptr, -1);
  RenderResult b = renderProject(p, kSr, 512, false, 0.3, nullptr, &assets, nullptr, 0);
  CHECK(a.L == b.L);
  CHECK(a.R == b.R);
}

TEST("render: ficheiro em falta nao parte o render") {
  Project p = fromJson(audioSong((tempDir() / "falta.wav").string(), 1.0));
  AssetStore assets;
  RenderResult r = renderProject(p, kSr, 512, false, 0.1, nullptr, &assets, nullptr, 0);
  CHECK(allFinite(r.L));
  CHECK_NEAR(r.peak, 0, 0);
}

// --- Loop e mudanca de tempo (sweep: o loop era convertido duas vezes) ------------------------------------------

TEST("loop: posto no tempo novo antes de o audio aplicar o grafo novo nao e convertido outra vez") {
  Engine e(kSr);
  std::vector<float> L(256), R(256);
  e.publish(std::make_unique<Graph>(fromJson(demoProject(120)), kSr));
  e.process(L.data(), R.data(), 256);  // o audio tem o grafo a 120
  e.setLoop(true, 48000, 96000);
  e.publish(std::make_unique<Graph>(fromJson(demoProject(240)), kSr));
  e.setLoop(true, 24000, 48000);  // a app manda o loop ja no tempo novo (metade das amostras)
  e.process(L.data(), R.data(), 256);
  CHECK_EQ(e.loopStart(), (int64_t)24000);
  CHECK_EQ(e.loopEnd(), (int64_t)48000);
  // Sem loop novo, a mudanca de tempo continua a levar o loop consigo.
  e.publish(std::make_unique<Graph>(fromJson(demoProject(120)), kSr));
  e.process(L.data(), R.data(), 256);
  CHECK_EQ(e.loopStart(), (int64_t)48000);
  CHECK_EQ(e.loopEnd(), (int64_t)96000);
  // O fim conta a partir do inicio ja limitado a 0.
  e.setLoop(true, -500, -100);
  CHECK_EQ(e.loopStart(), (int64_t)0);
  CHECK(e.loopEnd() >= 1);
}

// --- Arredura profunda do core (audit 2026-11-17), itens 10 a 14 ------------------------------------------------

TEST("audio: o limite de memoria recusa antes de ler o ficheiro que nao cabe (audit 10)") {
  // 100 000 frames a 8 kHz = 763 KiB em float estéreo; a 48 kHz ficam 600 000 = 4,6 MiB. Com o limite em
  // 1 MiB o motor lê o ficheiro todo, converte, e só depois recusa: são ~6 MiB tocados para dizer que não cabia.
  const std::string path = (tempDir() / "subida-8k.wav").string();
  const auto s = sine(200, 8000, 100000, 0.4f);
  writeWav(path, s, s, 8000, 32);
  AssetStore assets(1u << 20);
  std::string why;
  g_allocBytes = 0;
  g_allocOn = true;
  const auto data = assets.get(path, kSr, &why);
  const size_t bytes = gaugeStop();
  CHECK(data == nullptr);
  CHECK(why.find("not enough memory") != std::string::npos);
  CHECK(bytes < (1u << 18));  // 256 KiB: uma recusa não lê nem converte nada
  if (bytes >= (1u << 18)) coretest::fail(__FILE__, __LINE__, coretest::pair(bytes, (size_t)262144));
}

TEST("audio: um ficheiro ao ritmo do projecto nao e copiado outra vez (audit 10)") {
  // 500 000 frames a 48 kHz estéreo = 3,8 MiB. Ao mesmo ritmo não há conversão a fazer: o que se toca é
  // o ficheiro uma vez, mais o buffer de leitura, não duas vezes o áudio.
  const int64_t frames = 500000;
  const std::string path = (tempDir() / "quatro-mi.wav").string();
  const auto s = sine(300, kSr, frames, 0.4f);
  writeWav(path, s, s, (int)kSr, 32);
  AssetStore assets;
  std::string why;
  g_allocBytes = 0;
  g_allocOn = true;
  const auto data = assets.get(path, kSr, &why);
  const size_t bytes = gaugeStop();
  CHECK(data != nullptr);
  if (data) {
    CHECK_EQ(data->frames(), frames);
    CHECK_NEAR(data->sampleRate, kSr, 0);
  }
  const size_t audio = (size_t)frames * 2 * sizeof(float);
  CHECK(bytes <= audio + audio / 4);  // hoje ~2x o áudio: a conversão ao "mesmo ritmo" era uma cópia
  if (bytes > audio + audio / 4) coretest::fail(__FILE__, __LINE__, coretest::pair(bytes, audio + audio / 4));
}

TEST("freeze: o congelamento fica dentro do pedaco pedido (audit 11)") {
  // Ficheiro de corrente contínua: -1 em todo o lado, +1 nos primeiros 480 frames (0,01 s a 48 kHz).
  // Um freeze de 0,01 s só pode soar a +1; o grão de 40 ms era maior do que a região e saía dela.
  const auto frozen = [](int64_t region) {
    std::vector<float> v((size_t)96000, -1.0f);
    for (int64_t i = 0; i < region; ++i) v[(size_t)i] = 1.0f;
    return v;
  };
  auto renderFreeze = [&](double freezeSeconds, int64_t region) {
    const std::string path = (tempDir() / "dc-regiao.wav").string();
    writeWav(path, frozen(region), frozen(region), (int)kSr, 32);
    json clip = {{"startTick", 0}, {"path", path}, {"offsetSeconds", 0.0}, {"durationSeconds", 0.2},
                 {"gainDb", 0.0}, {"freezeSeconds", freezeSeconds}};
    json track = {{"id", "audio"}, {"name", "Audio"}, {"instrument", "audio"}, {"audioClips", json::array({clip})}};
    json p = {{"format", kProjectFormat}, {"version", 2}, {"name", "teste"}, {"bpm", 150.0},
              {"lengthTicks", 4 * 4 * kPpq}, {"masterGainDb", 0.0}, {"tracks", json::array({track})}};
    AssetStore assets;
    return renderProject(fromJson(p), kSr, 512, false, 0.05, nullptr, &assets, nullptr, 0);
  };
  for (const auto& attempt : std::vector<std::pair<double, int64_t>>{{0.01, 480}, {0.5, 24000}}) {
    const RenderResult r = renderFreeze(attempt.first, attempt.second);
    CHECK(allFinite(r.L));
    int64_t fora = 0, primeira = -1;
    for (int64_t i = 0; i < 9600; ++i)  // o clip todo (0,2 s) tem de vir da região congelada
      if (!(r.L[(size_t)i] > 0.5f)) { ++fora; if (primeira < 0) primeira = i; }
    CHECK_EQ(fora, (int64_t)0);
    if (fora) coretest::fail(__FILE__, __LINE__, "freeze " + std::to_string(attempt.second) + " frames: " +
                    std::to_string(fora) + " amostras fora da região, a primeira em " + std::to_string(primeira));
  }
}

TEST("adopt: a mudanca de tempo nao desloca o arranque do envelope (audit 12)") {
  // Uma nota no tick 1920 (2 batidas) e um rise de 4 batidas no volume. O grafo toca a 120 até 2 batidas e
  // meia e dá lugar a um grafo a 60, onde o motor põe o cursor na mesma posição musical. O rise tem de
  // continuar de onde estava; a versão do mesmo projecto a 60, tocada desde o princípio, é a referência.
  const json notes = json::array({{{"tick", 2 * kPpq}, {"length", kPpq / 4}, {"pitch", 60}, {"velocity", 0.8}}});
  const json mods = json::array({{{"id", "m1"}, {"param", "gainDb"}, {"shape", "rise"}, {"rateBeats", 4.0}, {"depth", -32.0}}});
  FakeProvider provider;
  Graph slow(fromJson(pluginSong(120, notes, mods)), kSr, nullptr, &provider);
  Graph fast(fromJson(pluginSong(60, notes, mods)), kSr, nullptr, &provider);
  std::vector<float> L(32), R(32);
  auto block = [&](Graph& g, int64_t pos) {         // process() soma no buffer: cada bloco comeca a zero
    std::fill(L.begin(), L.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    g.process(L.data(), R.data(), 32, pos, false);
    return peakOf(L);
  };
  float before = 0;
  for (int64_t pos = 0; pos < 60000; pos += 32) before = block(slow, pos);
  fast.adopt(slow);
  float depois = 0;
  for (int64_t pos = 120000; pos < 124128; pos += 32) depois = block(fast, pos);
  Graph ref(fromJson(pluginSong(60, notes, mods)), kSr, nullptr, &provider);
  float esperado = 0;
  for (int64_t pos = 0; pos < 124128; pos += 32) esperado = block(ref, pos);
  CHECK(before > 0.2f);
  CHECK_NEAR(depois, esperado, 0.02);  // hoje lê-se uma batida e meia de rise, não meia
  if (std::fabs(depois - esperado) > 0.02)
    coretest::fail(__FILE__, __LINE__, "nivel depois da mudanca " + std::to_string(depois) +
                    ", nivel do mesmo ponto musical " + std::to_string(esperado) + ", antes " + std::to_string(before));
}

TEST("adopt: os note-offs que ficaram na reserva passam para o grafo novo (audit 13)") {
  // 600 notas no mesmo tick: o bloco leva mais eventos do que a fila de um plugin cabe, e os note-offs que
  // sobram para a reserva têm de sair no grafo que adopta. Sem eles a nota fica pendurada.
  json notes = json::array();
  for (int i = 0; i < 600; ++i) notes.push_back({{"tick", 0}, {"length", 1}, {"pitch", 60}, {"velocity", 0.8}});
  FakeProvider provider;
  Graph a(fromJson(pluginSong(120, notes, json::array())), kSr, nullptr, &provider);
  std::vector<float> L(32), R(32);
  a.process(L.data(), R.data(), 32, 0, false);  // um sub-bloco: a fila enche e fica reserva atrás
  Graph b(fromJson(pluginSong(120, notes, json::array())), kSr, nullptr, &provider);
  b.adopt(a);
  for (int n = 1; n <= 8; ++n) b.process(L.data(), R.data(), 32, 32 * n, false);
  int ons = 0, offs = 0;
  for (const auto& e : provider.proc->seen)
    if (e.pitch == 60) { if (e.on) ++ons; else ++offs; }
  CHECK_EQ(ons, 600);                    // os 600 note-ons chegaram todos
  CHECK_EQ(offs, 600);                   // e os 600 note-offs também
  CHECK_EQ(pluginNotesDropped(), (uint64_t)0);
  if (offs != ons) coretest::fail(__FILE__, __LINE__, "note-offs perdidos na troca: " + std::to_string(ons - offs));
}

TEST("engine.start: uma saída que falha não deixa a app sem som (audit 14)") {
  FakeHost host;
  Server server([&host] { return host.make(); }, kSr);
  const auto first = server.execute("engine.start", json::object());
  CHECK_EQ(host.logs.size(), (size_t)1);
  CHECK(first.is_object());
  CHECK(host.logs[0]->running);
  CHECK_EQ(host.logs[0]->stops, 0);
  host.failNext = true;  // a próxima saída recusa-se a abrir
  CHECK_THROWS(server.execute("engine.start", json::object()));
  CHECK_EQ(host.logs.size(), (size_t)2);
  CHECK(host.logs[0]->running);  // hoje: parado e destruído antes da tentativa → silêncio
  CHECK_EQ(host.logs[0]->starts, 2);  // a anterior volta a abrir-se
  CHECK_EQ(host.logs[0]->stops, 1);
  host.nullNext = true;          // e quando não há saída nenhuma
  CHECK_THROWS(server.execute("engine.start", json::object()));
  CHECK(host.logs[0]->running);
  CHECK_EQ(host.logs.size(), (size_t)2);   // sem saída não há um segundo objecto
  // Uma tentativa que resulta volta a ser aceite, com a saída nova em curso.
  const auto ok = server.execute("engine.start", json::object());
  CHECK(ok.is_object());
  CHECK_EQ(host.logs.size(), (size_t)3);   // cada tentativa parada abre um dispositivo novo
  CHECK(host.logs[2]->running);
  CHECK(!host.logs[0]->running);           // esta largada quando a nova responde
  CHECK_EQ(host.logs[0]->starts, 3);       // reaberta em cada uma das duas recusas
  CHECK_EQ(host.logs[0]->stops, 3);
}

CORE_TEST_MAIN
