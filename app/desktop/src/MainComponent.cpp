#include "MainComponent.h"
#include "ChoiceDialog.h"
#include "core/project.hpp"
#include "follow.hpp"
#include "stutter.hpp"
#include "Language.h"
#include "Keymap.h"
#include "pattern_tools.hpp"
#include "pcore.hpp"
#include "pattern_rack.hpp"
#include "ProposalsPanel.h"
#include "Diagnostics.h"
#include <cctype>
#include <cmath>
#include <thread>
#include "Stretch.h"
#include "Theme.h"
#include "TrackColour.h"
#include "randomize.hpp"
#include "sample_analysis.hpp"
#include "sampling.hpp"
#include <limits>
#include "assets.hpp"
#include "bounce.hpp"
#include "core_version.h"
#include "bridge.hpp"
#include "calls.hpp"
#include "storage.hpp"
#include "midi.hpp"
#include "bitwig.hpp"
#include "core_plugins.hpp"
#include "role_tools.hpp"

namespace core::app {

namespace {

juce::String u8(const char* text) { return juce::String::fromUTF8(text); }
juce::String u8(const std::string& text) { return juce::String::fromUTF8(text.c_str()); }

// The palette lives in Theme.h; these short names are what this file draws with.
using theme::kBackground, theme::kPanel, theme::kCard, theme::kField, theme::kOutline, theme::kText, theme::kMuted, theme::kDim,
    theme::kAccent, theme::kClip, theme::kClipNote;
const juce::Colour kGreen = theme::kOk, kAmber = theme::kWarn, kRed = theme::kError, kBlue = theme::kAccent;
constexpr int64_t kBar = 4 * doc::kPpq, kBeat = doc::kPpq;
constexpr int kWordmarkWidth = 150;
// The top bar is also the window's title bar (no system title bar above it): the logo and the window
// buttons sit against the window's top edge.
constexpr int kTopBar = 40, kWindowButton = 44;
constexpr int kTrackRow = 34;  // a track's row in the arrangement (ref-2: compact rows)  // the logo and "CORE" at the left of the top bar
// The window's layout: the menus sit in the top bar (no strip of their own); the AI footer; the side panels.
constexpr int kRail = 40;  // the page rail's width, left of the panels
constexpr int kTracksScrollBar = 10, kMenuBar = 0, kFooter = 24, kMenusWidth = 430, kLibraryWidth = 260, kInspectorWidth = 250, kEditorHeight = 300,
              kEditorMinHeight = 140, kEditorCollapsed = 40;
constexpr int kMixerHeight = 300, kMixerMinHeight = 210;  // the Mixer's band: by default and at its shortest

// "#rrggbb" (as the document keeps colours) as a JUCE colour.
juce::Colour hexColour(const std::string& hex) {
  return hex.size() == 7 ? juce::Colour::fromString("ff" + juce::String(hex.substr(1))) : juce::Colour(0xff8a8fc0);
}
// A section's colour: its own, or the one of its type (doc::sectionColor).
juce::Colour sectionColour(const doc::Section& s) { return hexColour(doc::sectionColor(s)); }

// A clip's body in the track's colour: the border darker, the inside lighter and slightly see-through in the
// middle (the lane and the section shade show a little), so the notes and the waveform on it stay easy to read.
void fillClip(juce::Graphics& g, juce::Rectangle<int> box, juce::Colour colour) {
  const auto r = box.toFloat();
  const juce::Colour inside = colour.brighter(0.18f);
  juce::ColourGradient fill(inside, 0, r.getY(), inside, 0, r.getBottom(), false);
  fill.addColour(0.5, colour.brighter(0.4f).withMultipliedAlpha(0.72f));  // the middle clearly lighter than the border
  g.setGradientFill(fill);
  g.fillRoundedRectangle(r, theme::kCorner);
  g.setColour(colour.darker(0.6f));
  g.drawRoundedRectangle(r.reduced(0.5f), theme::kCorner, 1.2f);
}

juce::Colour statusColour(EngineStatus s) {
  switch (s) {
    case EngineStatus::Ready: return kGreen;
    case EngineStatus::Syncing: case EngineStatus::Starting: return kAmber;
    case EngineStatus::NoDevice: return kBlue;
    case EngineStatus::Diverged: case EngineStatus::Stopped: return kRed;
  }
  return kMuted;
}

// Peak (linear) to 0..1 on a -60..0 dBFS scale.
float meterFraction(float peak) {
  if (peak <= 0.000001f) return 0;
  return juce::jlimit(0.0f, 1.0f, (20.0f * std::log10(peak) + 60.0f) / 60.0f);
}

void drawMeter(juce::Graphics& g, juce::Rectangle<float> area, float left, float right) {
  // Two rows of small segments (left, right): the accent up to -6 dB, then the warning colour, red at 0 dB.
  const float h = (area.getHeight() - 2) / 2;
  constexpr float seg = 3.0f, gap = 1.0f;
  const int count = std::max(1, (int)((area.getWidth() + gap) / (seg + gap)));
  for (int c = 0; c < 2; ++c) {
    const float y = area.getY() + c * (h + 2);
    const float peak = c == 0 ? left : right;
    const int lit = (int)std::ceil(meterFraction(peak) * count);
    for (int i = 0; i < count; ++i) {
      const float f = (i + 1) / (float)count;
      juce::Colour col = theme::kField.brighter(0.1f);
      if (i < lit) col = peak >= 1.0f && i == count - 1 ? kRed : f > 0.9f ? kAmber : theme::kBlueLight;  // a level: light blue-grey
      g.setColour(col);
      g.fillRect(area.getX() + i * (seg + gap), y, seg, h);
    }
  }
}

doc::Track* findTrack(doc::Document& d, const std::string& id) {
  for (auto& t : d.tracks) if (t.id == id) return &t;
  return nullptr;
}

std::filesystem::path pathOf(const juce::File& f) { return std::filesystem::path(f.getFullPathName().toWideCharPointer()); }
juce::File fileOf(const std::filesystem::path& p) { return juce::File(juce::String(p.wstring().c_str())); }

int64_t audioEndTick(const doc::Document& d, const doc::AudioClip& c) { return doc::audioClipEndTick(d, c); }

// After a clip went past the end: the song grows (whole bars) to hold it. False when it already does. (Only
// that clip: a song shortened on purpose keeps what is past its end.)
bool fitSong(doc::Document& d, const std::string& trackId, const std::string& clipId) {
  const doc::Track* t = d.findTrack(trackId);
  if (!t) return false;
  int64_t end = d.lengthTicks;
  for (const auto& c : t->clips) if (c.id == clipId) end = std::max(end, c.startTick + c.lengthTicks);
  for (const auto& c : t->audioClips) if (c.id == clipId) end = std::max(end, audioEndTick(d, c));
  if (end <= d.lengthTicks) return false;
  try { doc::setSongLength(d, std::min<int64_t>(4096LL * kBar, (end + kBar - 1) / kBar * kBar)); } catch (const std::exception&) { return false; }
  return true;
}

void styleToggle(juce::TextButton& b, juce::Colour on) {
  b.setClickingTogglesState(false);
  b.setColour(juce::TextButton::buttonOnColourId, on);
}

} // namespace

// ---------------------------------------------------------------- one track: header (name, mute, solo, volume,
// meter) and its clip lane, which lines up with the ruler
class MainComponent::TrackRow final : public juce::Component, public juce::DragAndDropTarget {
public:
  TrackRow(MainComponent& owner, const doc::Track& track) : owner_(owner), id_(track.id) {
    fx_.setButtonText("FX");
    fx_.setTooltip(u8("This track's plugins: its instrument and its effects"));
    fx_.setClickingTogglesState(false);
    fx_.setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
    fx_.onClick = [this] { owner_.showTrackMenu(id_); };
    mute_.setButtonText("M");
    solo_.setButtonText("S");
    mute_.setTooltip("Mute");
    solo_.setTooltip("Solo");
    styleToggle(mute_, kAmber);
    styleToggle(solo_, theme::kPrimary);
    mute_.onClick = [this] {
      owner_.edit([this](doc::Document& d) { if (auto* t = findTrack(d, id_)) t->mix.mute = !t->mix.mute; }, "Mute " + name_);
    };
    solo_.onClick = [this] {
      owner_.edit([this](doc::Document& d) { if (auto* t = findTrack(d, id_)) t->mix.solo = !t->mix.solo; }, "Solo " + name_);
    };
    volume_.setSliderStyle(juce::Slider::LinearBar);
    theme::markLevel(volume_);  // a level: light blue-grey
    volume_.setRange(-60.0, 12.0, 0.1);
    volume_.setTextValueSuffix(" dB");
    volume_.setDoubleClickReturnValue(true, 0.0);
    volume_.setTooltip(u8("Track volume (-60 to +12 dB). Drag to change, double-click to type a value, Alt+click: 0 dB."));
    volume_.onDragStart = [this] { owner_.beginGesture("Volume " + name_); };
    volume_.onValueChange = [this] {
      const double v = volume_.getValue();
      owner_.edit([this, v](doc::Document& d) { if (auto* t = findTrack(d, id_)) t->mix.gainDb = v; }, "Volume " + name_);
    };
    volume_.onDragEnd = [this] { owner_.endGesture(); };
    for (auto* c : std::initializer_list<juce::Component*>{&fx_, &mute_, &solo_, &volume_}) {
      c->setWantsKeyboardFocus(false);
      addAndMakeVisible(c);
    }
    update(track);
  }

  const std::string& id() const { return id_; }

  // Where a drop lands on the lane (on the beat), or -1 over the header.
  int64_t dropTick(juce::Point<int> screen) const {
    const int x = getLocalPoint(nullptr, screen).x;
    return inLane(x) ? laneTick(x, kBeat) / kBeat * kBeat : -1;
  }

  // A Library sound: onto a sampler or drum track (its sound), or an audio track (a clip where it is dropped).
  bool isInterestedInDragSource(const SourceDetails& d) override {
    return d.description.toString().startsWith(LibraryPanel::kSampleDrag) && (track_.instrument.kind == "audio" || takesSamples(track_));
  }
  void itemDragEnter(const SourceDetails&) override { owner_.setDropRow(id_); }
  void itemDragExit(const SourceDetails&) override { if (owner_.dropRow_ == id_) owner_.setDropRow({}); }
  void itemDropped(const SourceDetails& d) override {
    owner_.setDropRow({});
    const std::string sha = d.description.toString().fromFirstOccurrenceOf(LibraryPanel::kSampleDrag, false, false).toStdString();
    owner_.dropLibrarySample(id_, sha, inLane(d.localPosition.x) ? laneTick(d.localPosition.x, kBeat) / kBeat * kBeat : -1);
  }

  void paintOverChildren(juce::Graphics& g) override {
    if (owner_.dropRow_ != id_) return;
    g.setColour(kAccent);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), theme::kCorner, 2.0f);
  }

  void update(const doc::Track& t) {
    track_ = t;
    name_ = u8(t.name);
    colour_ = trackColour(t);
    mute_.setToggleState(t.mix.mute, juce::dontSendNotification);
    solo_.setToggleState(t.mix.solo, juce::dontSendNotification);
    int active = 0;
    for (const auto& fx : t.effects) active += !doc::effectBypassed(fx);
    fx_.setToggleState(active > 0, juce::dontSendNotification);  // lit while some effect plays
    fx_.setButtonText(t.effects.empty() ? juce::String("FX") : "FX " + juce::String((int)t.effects.size()));
    if (!volume_.isMouseButtonDown()) volume_.setValue(t.mix.gainDb, juce::dontSendNotification);
    const bool automated = std::any_of(t.automation.begin(), t.automation.end(), [](const doc::Automation& a) { return a.param == "gainDb"; });
    volume_.setEnabled(!automated);
    volume_.setTooltip(automated ? u8("The volume is automated: change it on the curve (track menu > Automation).")
                                 : u8("Track volume (-60 to +12 dB). Drag to change, double-click to type a value, Alt+click: 0 dB."));
    repaint();
  }

  void setLevels(float left, float right) {
    if (std::abs(left - left_) < 1e-4f && std::abs(right - right_) < 1e-4f) return;  // silent tracks: no redraw
    left_ = left;
    right_ = right;
    repaint(meter_);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(0, 5);
    r.removeFromLeft(6 + 8 + 98);
    fx_.setBounds(r.removeFromLeft(32).reduced(1, 0));
    mute_.setBounds(r.removeFromLeft(26).reduced(1, 0));
    solo_.setBounds(r.removeFromLeft(26).reduced(1, 0));
    r.removeFromLeft(6);
    volume_.setBounds(r.removeFromLeft(110));
    r.removeFromLeft(6);
    meter_ = r.removeFromLeft(56).reduced(0, 4);
  }

  // The row sits in the scrolling tracks panel: its x in the main window's frame, where the ruler is.
  int originX() const { return owner_.getLocalPoint(this, juce::Point<int>()).x; }
  int laneX(double tick) const { return owner_.xOfTick(tick) - originX(); }
  int64_t laneTick(int x, int64_t snap) const { return owner_.tickAt(x + originX(), snap, true); }  // clips may go past the end
  bool inLane(int x) const { return x >= kTrackHead; }

  const doc::Clip* clipAt(int x, bool* rightEdge) const {
    for (auto it = track_.clips.rbegin(); it != track_.clips.rend(); ++it) {
      const int x0 = laneX((double)it->startTick), x1 = laneX((double)(it->startTick + it->lengthTicks));
      if (x >= x0 && x < std::max(x1, x0 + 4)) {
        if (rightEdge) *rightEdge = x >= x1 - 6 && x1 - x0 > 14;
        return &*it;
      }
    }
    return nullptr;
  }

  bool audio() const { return track_.instrument.kind == "audio"; }

  // The clips (pattern or audio) that cross the lane between x0 and x1 (row x), for the selection box.
  std::vector<std::string> clipsBetween(int x0, int x1) const {
    std::vector<std::string> out;
    x0 = std::max(x0, kTrackHead);
    if (x1 <= x0) return out;
    const doc::Document& d = owner_.history_.current();
    for (const auto& c : track_.clips) {
      const int a = laneX((double)c.startTick), b = std::max(a + 4, laneX((double)(c.startTick + c.lengthTicks)));
      if (a < x1 && b > x0) out.push_back(c.id);
    }
    for (const auto& c : track_.audioClips) {
      const int a = laneX((double)c.startTick), b = std::max(a + 4, laneX((double)audioEndTick(d, c)));
      if (a < x1 && b > x0) out.push_back(c.id);
    }
    return out;
  }

  // edge: -1 left (trim start), 1 right (trim end), 0 inside (move).
  const doc::AudioClip* audioClipAt(int x, int* edge) const {
    const doc::Document& d = owner_.history_.current();
    for (auto it = track_.audioClips.rbegin(); it != track_.audioClips.rend(); ++it) {
      const int x0 = laneX((double)it->startTick), x1 = std::max(x0 + 4, laneX((double)audioEndTick(d, *it)));
      if (x >= x0 && x < x1) {
        if (edge) *edge = x1 - x0 <= 14 ? 0 : x < x0 + 6 ? -1 : x >= x1 - 6 ? 1 : 0;
        return &*it;
      }
    }
    return nullptr;
  }

  void paintAudioClips(juce::Graphics& g, juce::Rectangle<int> lane) {
    const doc::Document& d = owner_.history_.current();
    for (const auto& c : track_.audioClips) {
      const doc::Asset* asset = d.findAsset(c.assetId);
      const int x0 = laneX((double)c.startTick), x1 = std::max(x0 + 3, laneX((double)audioEndTick(d, c)));
      const auto box = juce::Rectangle<int>::leftTopRightBottom(x0, lane.getY() + 1, std::min(x1, lane.getRight()), lane.getBottom() - 1);
      if (box.getWidth() <= 0) continue;
      const bool missing = !asset || std::count(owner_.missing_.begin(), owner_.missing_.end(), c.assetId) > 0;
      const bool selected = (owner_.selectedTrack_ == id_ && owner_.selectedClip_ == c.id) || owner_.clipBoxed(id_, c.id);
      const juce::Colour base = missing ? kRed : c.mute ? kMuted : kClipNote;
      const juce::Colour fill = kClip.interpolatedWith(colour_, 0.32f);  // tinted with the track's colour
      fillClip(g, box, missing ? kRed.withAlpha(0.3f) : selected ? fill.brighter(0.15f) : fill.withAlpha(c.mute ? 0.5f : 1.0f));
      // Waveform: the loudest sample under each pixel, mirrored around the middle.
      std::shared_ptr<const Waveform> wave;
      if (asset && !missing) wave = owner_.waves_.get(asset->sha256, fileOf(doc::assetFile(*asset, owner_.projectDir())));
      if (wave) {
        const float gain = (float)std::pow(10.0, c.gainDb / 20), mid = (float)box.getCentreY(), half = box.getHeight() / 2.0f - 2;
        const double secondsPerPixel = doc::secondsBetween(d, (double)c.startTick, (double)audioEndTick(d, c)) / std::max(1, x1 - x0);
        const auto visible = box.getIntersection(g.getClipBounds());
        for (int x = visible.getX(); x < visible.getRight(); ++x) {
          const double from = c.offsetSeconds + (x - x0) * secondsPerPixel;
          drawWaveColumn(g, *wave, (float)x, mid, half, from, from + secondsPerPixel, base.withAlpha(c.mute ? 0.4f : 0.95f), gain);
        }
      }
      // Fades: a line from the corner to where the full level starts.
      g.setColour(kText.withAlpha(0.7f));
      const double pixelsPerSecond = (x1 - x0) / std::max(0.001, c.durationSeconds);
      if (c.fadeInSeconds > 0) g.drawLine((float)x0, (float)box.getBottom(), (float)(x0 + c.fadeInSeconds * pixelsPerSecond), (float)box.getY(), 1.5f);
      if (c.fadeOutSeconds > 0) g.drawLine((float)(x1 - c.fadeOutSeconds * pixelsPerSecond), (float)box.getY(), (float)x1, (float)box.getBottom(), 1.5f);
      g.setColour(colour_.withAlpha(c.mute ? 0.35f : 0.85f));  // the track's colour, as a thin top edge
      g.fillRect(box.getX() + 2, box.getY(), box.getWidth() - 4, 2);
      if (selected) {
        g.setColour(kAccent);
        g.drawRoundedRectangle(box.toFloat().reduced(0.5f), theme::kCorner, 1.5f);
      }
      g.setColour(kText);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      const juce::String name = asset ? u8(asset->name) : juce::String(u8("(unknown file)"));
      g.drawText(lang::tr(missing ? u8("Missing: ") + name : name), box.reduced(4, 2), juce::Justification::topLeft, true);
    }
  }

  // A pattern's sound inside its clip, every repeat: the peaks as a light veil and the RMS body over them, in the
  // track's colour and see-through, so the grid shows behind. The fader's level is in the picture (a quieter track
  // draws smaller), the mute dims it.
  void paintClipWave(juce::Graphics& g, const doc::Clip& c, const doc::Pattern& p, const Waveform& w, juce::Rectangle<int> box, int x0, int x1) {
    const auto area = box.reduced(0, 3);
    const auto visible = area.getIntersection(g.getClipBounds());
    if (visible.isEmpty() || x1 <= x0 || p.lengthTicks <= 0 || w.peaks.empty()) return;
    const double patternTicks = (double)p.lengthTicks;
    const double ticksPerPixel = (double)c.lengthTicks / (double)(x1 - x0);
    const double secondsPerTick = (double)w.peaks.size() * w.framesPerPeak / w.sampleRate / patternTicks;
    const bool automated = std::any_of(track_.automation.begin(), track_.automation.end(), [](const doc::Automation& a) { return a.param == "gainDb"; });
    const float gain = automated ? 1.0f : (float)std::pow(10.0, track_.mix.gainDb / 20);
    const juce::Colour colour = colour_.interpolatedWith(kText, 0.3f).withAlpha(track_.mix.mute ? 0.35f : 0.82f);
    const float mid = (float)area.getCentreY(), half = area.getHeight() / 2.0f - 1.0f;
    for (int x = visible.getX(); x < visible.getRight(); ++x) {
      const double at = std::fmod((x - x0) * ticksPerPixel + (double)c.offsetTicks, patternTicks);
      const double to = std::min(patternTicks, at + ticksPerPixel);
      drawWaveColumn(g, w, (float)x, mid, half, at * secondsPerTick, to * secondsPerTick, colour, gain);
    }
  }

  // A pattern's notes, small and light, inside its clip (each repeat), as on the reference. Drum rows (one pitch
  // per sound) and melodies both fit: the pitches in use spread over the clip's height.
  void paintClipNotes(juce::Graphics& g, const doc::Clip& c, const doc::Pattern& p, juce::Rectangle<int> box) {
    if (p.notes.empty() || p.lengthTicks <= 0 || box.getWidth() < 6 || box.getHeight() < 10) return;
    int lo = 127, hi = 0;
    for (const auto& n : p.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
    const auto inner = box.reduced(3, 6).toFloat();
    const float rowH = juce::jlimit(2.0f, 5.0f, inner.getHeight() / (float)(hi - lo + 1));
    const float spread = inner.getHeight() - rowH;
    const auto visible = box.getIntersection(g.getClipBounds());
    if (visible.isEmpty()) return;
    g.setColour(kClipNote);
    for (int64_t rep = c.startTick; rep < c.startTick + c.lengthTicks; rep += p.lengthTicks) {
      if (laneX((double)(rep + p.lengthTicks)) < visible.getX()) continue;
      if (laneX((double)rep) > visible.getRight()) break;
      for (const auto& n : p.notes) {
        const int64_t t0 = rep + n.tick, t1 = std::min(rep + n.tick + n.length, c.startTick + c.lengthTicks);
        if (t0 >= c.startTick + c.lengthTicks) continue;
        const float x0 = (float)laneX((double)t0), x1 = std::max(x0 + 2.0f, (float)laneX((double)t1) - 1.0f);
        const float y = hi == lo ? inner.getCentreY() - rowH / 2 : inner.getBottom() - rowH - spread * (float)(n.pitch - lo) / (float)(hi - lo);
        g.fillRect(juce::Rectangle<float>(x0, y, x1 - x0, rowH).getIntersection(inner));
      }
    }
  }

  void paint(juce::Graphics& g) override {
    // Header: a card with the track's colour as a thin strip; the track in the editor gets an accent edge.
    const bool current = owner_.editor_.track() == id_ || (audio() && owner_.selectedTrack_ == id_);
    const bool also = !current && owner_.alsoSelected_.count(id_) > 0;  // Ctrl/Shift-clicked: plugins act on it too
    const auto card = getLocalBounds().withWidth(kTrackHead - 10).reduced(0, 3).toFloat();
    g.setColour(current || also ? kCard.brighter(0.08f) : kCard);
    g.fillRoundedRectangle(card, theme::kCorner);
    if (current || also) {
      g.setColour(kAccent.withAlpha(current ? 0.7f : 0.4f));
      g.drawRoundedRectangle(card.reduced(0.5f), theme::kCorner, 1.0f);
    }
    auto r = getLocalBounds().reduced(0, 6);
    g.setColour(colour_);
    g.fillRoundedRectangle(r.removeFromLeft(6).toFloat().reduced(2, 4).withX(4), 1.5f);
    r.removeFromLeft(8);
    g.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    g.setColour(track_.mix.mute ? kMuted : kText);
    g.drawFittedText(name_, r.removeFromLeft(90), juce::Justification::centredLeft, 1, 1.0f);  // a gap before FX; a long name ends in "..."
    drawMeter(g, meter_.toFloat(), left_, right_);
    // Lane: a dark strip with the bar lines, then clips (slate, their notes drawn inside, the pattern's name).
    const auto lane = juce::Rectangle<int>::leftTopRightBottom(laneX(0), 3, laneX((double)owner_.history_.current().lengthTicks), getHeight() - 3);
    g.reduceClipRegion(kTrackHead, 0, getWidth() - kTrackHead, getHeight());
    g.setColour(kField);
    g.fillRect(lane);
    // Each section's colour as a light shade behind the clips (View > Section colours behind the tracks).
    if (owner_.sectionShades_)
      for (const auto& s : owner_.history_.current().sections) {
        const auto span = juce::Rectangle<int>::leftTopRightBottom(laneX((double)s.startTick), lane.getY(),
                                                                  laneX((double)(s.startTick + s.lengthTicks)), lane.getBottom())
                              .getIntersection(lane);
        if (span.isEmpty()) continue;
        g.setColour(sectionColour(s).withAlpha(0.08f));
        g.fillRect(span);
      }
    // Zoomed in, the grid shows beats, then sixteenths, fainter than the bars (each only when 12 px apart or more),
    // on the empty lane and, fainter still, through the clips.
    const double barPixels = owner_.laneSpan().getWidth() * kBar / owner_.viewLength();
    const int64_t fineStep = owner_.gridStep(12) < kBar ? owner_.gridStep(12) : 0;  // down to 1/64 when zoomed far in
    const int64_t firstBar = (int64_t)(owner_.viewStart() / kBar), lastBar = (int64_t)((owner_.viewStart() + owner_.viewLength()) / kBar) + 1;
    const auto fineGrid = [&](juce::Rectangle<int> area, juce::Colour beat, juce::Colour sixteenth) {
      if (fineStep <= 0) return;
      const int64_t from = std::max(firstBar * kBar, owner_.tickAt(area.getX() + originX(), fineStep)), to = std::min(lastBar * kBar, owner_.tickAt(area.getRight() + originX(), 1));
      for (int64_t t = from - from % fineStep; t <= to; t += fineStep) {
        if (t % kBar == 0) continue;
        const int x = laneX((double)t);
        if (x <= area.getX() || x >= area.getRight()) continue;
        g.setColour(t % kBeat == 0 ? beat : sixteenth);
        g.drawVerticalLine(x, (float)area.getY(), (float)area.getBottom());
      }
    };
    {
      const int every = barPixels >= 28 ? 1 : barPixels >= 14 ? 2 : barPixels >= 7 ? 4 : barPixels >= 2 ? 16 : 64;
      fineGrid(lane, theme::kGrid.brighter(0.05f), theme::kGrid);
      g.setColour(theme::kGrid.brighter(0.12f));
      for (int64_t b = firstBar; b <= lastBar; ++b)
        if (b % every == 0) g.drawVerticalLine(laneX((double)(b * kBar)), (float)lane.getY(), (float)lane.getBottom());
    }
    if (audio()) paintAudioClips(g, lane);
    for (const auto& c : track_.clips) {
      const doc::Pattern* p = track_.findPattern(c.patternId);
      if (!p) continue;
      const int x0 = laneX((double)c.startTick), x1 = std::max(x0 + 3, laneX((double)(c.startTick + c.lengthTicks)));
      const auto box = juce::Rectangle<int>::leftTopRightBottom(x0, lane.getY() + 1, std::min(x1, lane.getRight()), lane.getBottom() - 1);
      if (box.getWidth() <= 0) continue;
      const bool selected = (owner_.selectedTrack_ == id_ && owner_.selectedClip_ == c.id) || owner_.clipBoxed(id_, c.id);
      const juce::Colour fill = kClip.interpolatedWith(colour_, 0.32f);  // tinted with the track's colour
      fillClip(g, box, selected ? fill.brighter(0.15f) : fill);
      g.setColour(colour_.withAlpha(0.95f));  // and the track's colour as a top edge
      g.fillRect(box.getX() + 2, box.getY(), box.getWidth() - 4, 2);
      fineGrid(box.reduced(0, 3), kField.withAlpha(0.32f), kField.withAlpha(0.16f));
      g.setColour(kField.withAlpha(0.6f));
      for (int64_t t = c.startTick + p->lengthTicks; t < c.startTick + c.lengthTicks; t += p->lengthTicks)
        if (const int x = laneX((double)t); x > box.getX() + 2 && x < box.getRight() - 1) g.drawVerticalLine(x, (float)box.getY() + 2, (float)box.getBottom() - 2);
      // The pattern's real sound as a waveform (rendered by the engine's instruments in the background); its notes
      // until the first render is done, or when there is no sound to show (a plugin, silence).
      if (auto wave = owner_.patternWaves_.get(owner_.history_.current(), owner_.history_.revision(), id_, p->id, owner_.projectDir()))
        paintClipWave(g, c, *p, *wave, box, x0, x1);
      else
        paintClipNotes(g, c, *p, box);
      if (selected) {
        g.setColour(kAccent);
        g.drawRoundedRectangle(box.toFloat().reduced(0.5f), theme::kCorner, 1.5f);
      }
      if (box.getWidth() > 40) {
        g.setColour(kText.withAlpha(0.85f));
        g.setFont(juce::FontOptions(theme::kFontSmall));
        const int shared = track_.clipsUsing(p->id);
        g.drawText(u8(p->name) + (shared > 1 ? juce::String(" *") : juce::String()), box.reduced(5, 3), juce::Justification::topLeft, true);
      }
    }
    // The start line (the playhead) runs down through every lane.
    g.setColour(theme::kStartLine.withAlpha(0.9f));
    g.fillRect(laneX(owner_.shownPosition()), 0, 1, getHeight());
  }

  void mouseDown(const juce::MouseEvent& e) override {
    if (inLane(e.x) && owner_.startPan(e)) return;  // the hand: drag the song sideways
    if (!inLane(e.x)) {
      if (e.mods.isPopupMenu()) { if (!owner_.trackSelected(id_)) owner_.selectOnlyTrack(id_); owner_.showTrackMenu(id_); return; }
      if (e.mods.isCommandDown()) owner_.toggleTrackSelected(id_);
      else if (e.mods.isShiftDown()) owner_.selectTracksTo(id_);
      else {
        owner_.selectOnlyTrack(id_);
        // A double click opens the track's plugin window.
        if (track_.instrument.kind == "plugin" && e.getNumberOfClicks() >= 2) owner_.session_->openPluginEditor(doc::pluginKey(id_));
      }
      return;
    }
    if (audio()) { audioMouseDown(e); return; }
    bool edge = false;
    const doc::Clip* hit = clipAt(e.x, &edge);
    if (!hit) {
      if (e.mods.isPopupMenu()) { owner_.selectTrack(id_); owner_.showTrackMenu(id_); return; }
      if (!e.mods.isCommandDown()) owner_.selectTrack(id_);
      owner_.startBox(e);  // a drag from here draws the selection box
      return;
    }
    if (!edge && !e.mods.isPopupMenu() && owner_.boxClips_.size() > 1 && owner_.clipBoxed(id_, hit->id)) {
      groupDrag_ = hit->id;  // one of the box's clips: dragging it moves them all
      grabOffset_ = laneTick(e.x, 1) - hit->startTick;
      startTick_ = hit->startTick;
      owner_.beginBoxMove();
      return;
    }
    owner_.selectClip(id_, hit->id);
    if (e.mods.isPopupMenu()) { owner_.showClipMenu(id_, hit->id, laneTick(e.x, kBar)); return; }
    drag_ = hit->id;
    resize_ = edge;
    grabOffset_ = laneTick(e.x, 1) - hit->startTick;
    startTick_ = hit->startTick;
    startLength_ = hit->lengthTicks;
    copied_ = copyFailed_ = false;
    owner_.beginGesture(edge ? u8("Clip length") : u8("Move clip"));
  }

  // Drag: move on bars (Shift: beats); Ctrl drag: a copy (Alt alone grabs the view); the right edge: length.
  void audioMouseDown(const juce::MouseEvent& e) {
    int edge = 0;
    const doc::AudioClip* hit = audioClipAt(e.x, &edge);
    if (!hit) {
      if (e.mods.isPopupMenu()) { owner_.selectClip(id_, {}); owner_.showTrackMenu(id_); return; }
      if (!e.mods.isCommandDown()) owner_.selectClip(id_, {});
      owner_.startBox(e);
      return;
    }
    if (edge == 0 && !e.mods.isPopupMenu() && owner_.boxClips_.size() > 1 && owner_.clipBoxed(id_, hit->id)) {
      groupDrag_ = hit->id;
      grabOffset_ = laneTick(e.x, 1) - hit->startTick;
      startTick_ = hit->startTick;
      owner_.beginBoxMove();
      return;
    }
    owner_.selectClip(id_, hit->id);
    if (e.mods.isPopupMenu()) { owner_.showAudioClipMenu(id_, hit->id, laneTick(e.x, 1)); return; }
    drag_ = hit->id;
    audioEdge_ = edge;
    grabOffset_ = laneTick(e.x, 1) - hit->startTick;
    copied_ = copyFailed_ = false;
    owner_.beginGesture(edge < 0 ? u8("Trim start") : edge > 0 ? u8("Trim end") : u8("Move audio clip"));
  }

  // Audio clips: drag moves on beats (Shift: free), Ctrl drag makes a copy; the edges trim (the
  // audio stays in place, only what is heard changes).
  void audioMouseDrag(const juce::MouseEvent& e) {
    const int64_t snap = e.mods.isShiftDown() ? 1 : kBeat;
    const std::string trackId = id_, clipId = drag_;
    const int64_t at = laneTick(e.x, 1);
    if (audioEdge_ < 0) {
      owner_.edit([&](doc::Document& d) { doc::trimAudioClipStart(d, trackId, clipId, (at + snap / 2) / snap * snap); }, u8("Trim start"));
      return;
    }
    if (audioEdge_ > 0) {
      owner_.edit([&](doc::Document& d) {
        const doc::Track& t = doc::trackById(d, trackId);
        for (const auto& c : t.audioClips)
          if (c.id == clipId) doc::trimAudioClipEnd(d, trackId, clipId, doc::secondsBetween(d, (double)c.startTick, (double)((at + snap / 2) / snap * snap)));
      }, u8("Trim end"));
      return;
    }
    if (copyFailed_) return;
    const int64_t start = std::max<int64_t>(0, (at - grabOffset_ + snap / 2) / snap * snap);
    aimAtTrack(e);
    if ((!copied_ && e.mods.isCtrlDown()) || copied_) {
      if (start == startTick_ && owner_.dropRow_.empty()) return; // no hidden copy over its source
    }
    if (!copied_ && e.mods.isCtrlDown()) {
      std::string copy;
      owner_.edit([&](doc::Document& d) {
        for (const auto& c : doc::trackById(d, trackId).audioClips)
          if (c.id == clipId) { copy = doc::duplicateAudioClip(d, trackId, clipId, start); break; }
      }, u8("Copy audio clip"));
      if (copy.empty()) { copyFailed_ = true; owner_.setDropRow({}); return; }
      drag_ = copy; owner_.selectClip(id_, copy);
      copied_ = true;
    }
    const std::string moving = drag_;
    owner_.edit([&](doc::Document& d) { doc::moveAudioClip(d, trackId, moving, start); }, u8("Move audio clip"));
    aimAtTrack(e);
  }

  // Dragged up or down onto another track that can play it (pattern clips on instrument tracks, audio on audio
  // tracks): that row is lit, and the clip goes there on release.
  // A clip changes track only when the pointer is clearly inside the other row: a sideways drag that drifts a few
  // pixels into the next row moved the Kick's clip onto the Bass (2026-11-08, Diogo: the kick "não toca", the bass
  // played on every beat). Half a row of vertical travel and the middle half of the target row.
  void aimAtTrack(const juce::MouseEvent& e) {
    const TrackRow* target = owner_.rowAtScreen(e.getScreenPosition());
    const bool clearly = target && std::abs(e.getDistanceFromDragStartY()) >= getHeight() / 2 &&
                         target->getScreenBounds().reduced(0, target->getHeight() / 4).contains(e.getScreenPosition());
    const bool fits = clearly && target != this && (audio() ? target->track_.instrument.kind == "audio" : doc::playsPatterns(target->track_));
    owner_.setDropRow(fits ? target->id() : std::string());
  }

  void mouseDrag(const juce::MouseEvent& e) override {
    if (owner_.panning_) { owner_.dragPan(e); return; }
    if (owner_.boxing_) { owner_.dragBox(e); return; }
    if (!groupDrag_.empty()) {
      if (e.getDistanceFromDragStart() < 3) return;
      if (e.mods.isCtrlDown()) {
        owner_.notice_ = u8("Ctrl+drag copies one clip at a time. Select one clip to copy it.");
        owner_.repaint();
        return;
      }
      // The box's clips move together: bars (Shift: beats) for pattern clips, beats (Shift: free) for audio.
      const int64_t snap = audio() ? (e.mods.isShiftDown() ? 1 : kBeat) : (e.mods.isShiftDown() ? kBeat : kBar);
      const int64_t start = std::max<int64_t>(0, (laneTick(e.x, 1) - grabOffset_ + snap / 2) / snap * snap);
      groupMoved_ = true;
      owner_.moveBox(start - startTick_);
      return;
    }
    if (drag_.empty() || e.getDistanceFromDragStart() < 3) return;
    if (copyFailed_) return;
    if (audio()) { audioMouseDrag(e); return; }
    const int64_t snap = e.mods.isShiftDown() ? kBeat : kBar;
    const std::string trackId = id_;
    if (resize_) {
      const int64_t end = std::max(startTick_ + snap, laneTick(e.x, snap));
      const std::string clipId = drag_;
      owner_.edit([&](doc::Document& d) { doc::resizeClip(doc::trackById(d, trackId), clipId, end - startTick_); }, u8("Clip length"));
      return;
    }
    const int64_t start = std::max<int64_t>(0, (laneTick(e.x, 1) - grabOffset_ + snap / 2) / snap * snap);
    aimAtTrack(e);
    if (((!copied_ && e.mods.isCtrlDown()) || copied_) && start == startTick_ && owner_.dropRow_.empty()) return;
    if (!copied_ && e.mods.isCtrlDown()) {
      std::string copy;
      const std::string source = drag_;
      owner_.edit([&](doc::Document& d) {
        doc::Track& t = doc::trackById(d, trackId);
        copy = doc::duplicateClip(t, source, start);
      }, u8("Copy clip"));
      if (copy.empty()) { copyFailed_ = true; owner_.setDropRow({}); return; }
      drag_ = copy; owner_.selectClip(id_, copy);
      copied_ = true;
    }
    const std::string clipId = drag_;
    owner_.edit([&](doc::Document& d) { doc::moveClip(doc::trackById(d, trackId), clipId, start); }, u8("Move clip"));
    aimAtTrack(e);
  }

  void mouseUp(const juce::MouseEvent& e) override {
    if (owner_.panning_) { owner_.endPan(); return; }
    if (owner_.boxing_) { owner_.endBox(e); return; }
    if (!groupDrag_.empty()) {
      const std::string clip = groupDrag_, track = id_;
      const bool moved = groupMoved_;
      groupDrag_.clear();
      groupMoved_ = false;
      MainComponent& owner = owner_;  // (the rows may be rebuilt)
      owner.endBoxMove();
      if (!moved) owner.selectClip(track, clip);  // a click on one of the box's clips: that clip alone
      return;
    }
    if (drag_.empty()) return;
    const std::string target = owner_.dropRow_, clipId = drag_, from = id_;
    drag_.clear();
    owner_.setDropRow({});
    if (!target.empty() && target != from) {  // part of the same gesture: the move and the change of track are one undo step
      int64_t start = 0;
      if (const doc::Track* t = owner_.history_.current().findTrack(from)) {
        for (const auto& c : t->clips) if (c.id == clipId) start = c.startTick;
        for (const auto& c : t->audioClips) if (c.id == clipId) start = c.startTick;
      }
      std::string moved;
      MainComponent& owner = owner_;  // (only locals after the edit: the rows may be rebuilt)
      owner.edit([&](doc::Document& d) { moved = doc::moveClipToTrack(d, from, clipId, target, start); fitSong(d, target, moved); }, u8("Move clip to another track"));
      owner.endGesture();
      if (!moved.empty()) {
        owner.selectClip(target, moved);
        if (const doc::Track* t = owner.history_.current().findTrack(target))
          owner.notice_ = u8("Clip moved to ") + u8(t->name) + u8(" (Ctrl+Z puts it back).");
        owner.repaint();
      }
      return;
    }
    owner_.growSongToFit(from, clipId);  // still inside the gesture: one undo step with the move
    owner_.endGesture();
  }

  // Double click on an empty part of the lane: a new clip of the pattern shown in the editor.
  void mouseDoubleClick(const juce::MouseEvent& e) override {
    if (!audio() && inLane(e.x) && !clipAt(e.x, nullptr)) owner_.addClipAt(id_, laneTick(e.x, kBar) / kBar * kBar);
  }

private:
  MainComponent& owner_;
  std::string id_;
  doc::Track track_;
  juce::String name_;
  juce::Colour colour_;
  juce::TextButton fx_, mute_, solo_;
  theme::BarSlider volume_;  // drag to change, double-click to type
  juce::Rectangle<int> meter_;
  float left_ = 0, right_ = 0;
  std::string drag_, groupDrag_;  // groupDrag_: the box's clip under the mouse while the box's clips move
  bool resize_ = false, copied_ = false, copyFailed_ = false, groupMoved_ = false;
  int audioEdge_ = 0;
  int64_t grabOffset_ = 0, startTick_ = 0, startLength_ = 0;
};

// ---------------------------------------------------------------- the selected audio clip, large: the whole
// file with the clip's part lit, a selection made with the mouse, and the edits that use it

class MainComponent::AudioEditor final : public juce::Component {
public:
  explicit AudioEditor(MainComponent& owner) : owner_(owner) {
    title_.setColour(juce::Label::textColourId, kText);
    title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    hint_.setColour(juce::Label::textColourId, kMuted);
    hint_.setFont(juce::FontOptions(theme::kFontBody));
    hint_.setJustificationType(juce::Justification::centredRight);
    hint_.setText(u8("Drag: select · Click: playhead · Wheel: zoom · Shift+wheel: scroll · Del: delete the selection (no selection: the clip)"), juce::dontSendNotification);
    keep_.setButtonText(u8("Keep selection"));
    keep_.setTooltip(u8("The clip keeps only the selected part"));
    remove_.setButtonText(u8("Delete selection"));
    remove_.setTooltip(u8("Removes the selected part; the rest stays in place (Del)"));
    split_.setButtonText("Split");
    split_.setTooltip(u8("Splits at the start of the selection (or at the playhead)"));
    fadeIn_.setButtonText("Fade in");
    fadeIn_.setTooltip(u8("Fade in from the clip's start to the end of the selection"));
    fadeOut_.setButtonText("Fade out");
    fadeOut_.setTooltip(u8("Fade out from the start of the selection to the clip's end"));
    whole_.setButtonText("Show all");
    loopSel_.setButtonText(u8("Loop"));
    loopSel_.setTooltip(u8("Play only the selection, looped"));
    toClip_.setButtonText(u8("To new clip"));
    toClip_.setTooltip(u8("Copies the selection to a clip on a new track, at the same place in the song"));
    for (int i = 0; i < 4; ++i) grid_.addItem(u8(kGridNames[i]), i + 1);
    grid_.setSelectedId(1, juce::dontSendNotification);  // free, until a grid is chosen
    grid_.setTooltip(u8("The selection snaps to this grid (the song's tempo)"));
    loopSel_.onClick = [this] { loopSelection(); };
    more_.setButtonText(u8("More ▾"));
    more_.onClick = [this] { showMoreMenu(); };
    toClip_.onClick = [this] { selectionToNewClip(); };
    keep_.onClick = [this] { keepSelection(); };
    remove_.onClick = [this] { removeSelection(); };
    split_.onClick = [this] { splitHere(); };
    fadeIn_.onClick = [this] { fade(true); };
    fadeOut_.onClick = [this] { fade(false); };
    whole_.onClick = [this] { showWholeClip(); };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &hint_, &keep_, &remove_, &split_, &fadeIn_, &fadeOut_, &whole_, &loopSel_, &toClip_, &grid_, &more_}) {
      c->setWantsKeyboardFocus(false);
      addAndMakeVisible(c);
    }
  }

  const std::string& clipId() const { return clipId_; }
  // A selection is at least one tick (and the shortest clip) long, so every edit can use it.
  bool hasSelection() const { return selTo_ - selFrom_ >= doc::kMinClipSeconds && tickOf(selTo_) > tickOf(selFrom_); }

  void setClip(const std::string& trackId, const std::string& clipId) {
    if (trackId == trackId_ && clipId == clipId_) return;
    trackId_ = trackId;
    clipId_ = clipId;
    selFrom_ = selTo_ = 0;
    showWholeClip();
  }

  // The selection in file seconds (as the mouse makes it); for the self-test too.
  void select(double fromSeconds, double toSeconds) {
    const doc::AudioClip* c = clip();
    if (!c) return;
    selFrom_ = std::clamp(std::min(fromSeconds, toSeconds), c->offsetSeconds, c->offsetSeconds + c->durationSeconds);
    selTo_ = std::clamp(std::max(fromSeconds, toSeconds), c->offsetSeconds, c->offsetSeconds + c->durationSeconds);
    updateButtons();
    repaint();
  }

  void documentChanged() {
    const doc::AudioClip* c = clip();
    if (c) select(selFrom_, selTo_);  // stays inside the clip after a trim
    updateButtons();
    repaint();
  }

  void keepSelection() {
    if (!hasSelection()) return;
    const int64_t a = tickOf(selFrom_), b = tickOf(selTo_);
    const std::string t = trackId_, id = clipId_;
    owner_.edit([&](doc::Document& d) { doc::cropAudioClip(d, t, id, a, b); }, u8("Keep selection"));
    selFrom_ = selTo_ = 0;
    showWholeClip();
  }

  void removeSelection() {
    if (!hasSelection()) return;
    const int64_t a = tickOf(selFrom_), b = tickOf(selTo_);
    const std::string t = trackId_, id = clipId_;
    owner_.edit([&](doc::Document& d) { doc::removeAudioRange(d, t, id, a, b); }, u8("Delete selection"));
    selFrom_ = selTo_ = 0;
    if (!clip()) owner_.selectClip(t, {});
    else documentChanged();
  }

private:
  const doc::Asset* asset() const {
    const doc::AudioClip* c = clip();
    return c ? owner_.history_.current().findAsset(c->assetId) : nullptr;
  }
  const doc::AudioClip* clip() const {
    const doc::Track* t = owner_.history_.current().findTrack(trackId_);
    if (t) for (const auto& c : t->audioClips) if (c.id == clipId_) return &c;
    return nullptr;
  }
  // File seconds <-> song ticks, through the clip's place in the song and the tempo map.
  double exactTickOf(double fileSeconds) const {
    const doc::AudioClip* c = clip();
    if (!c) return 0;
    const doc::Document& d = owner_.history_.current();
    if (d.tempoMap.empty()) return c->startTick + (fileSeconds - c->offsetSeconds) / doc::secondsPerTick(d);
    return doc::tickAtSeconds(d, doc::secondsAt(d, (double)c->startTick) + fileSeconds - c->offsetSeconds);
  }
  int64_t tickOf(double fileSeconds) const { return clip() ? (int64_t)std::llround(exactTickOf(fileSeconds)) : 0; }
  double secondsOfTick(double tick) const {
    const doc::AudioClip* c = clip();
    return c ? c->offsetSeconds + doc::secondsBetween(owner_.history_.current(), (double)c->startTick, tick) : 0;
  }
  juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedTop(40).reduced(10, 6).withTrimmedTop(16); }
  double secondsAt(int x) const {
    const auto a = waveArea();
    return viewFrom_ + (x - a.getX()) * (viewTo_ - viewFrom_) / std::max(1, a.getWidth());
  }
  float xOf(double seconds) const {
    const auto a = waveArea();
    return (float)(a.getX() + (seconds - viewFrom_) / std::max(1e-9, viewTo_ - viewFrom_) * a.getWidth());
  }

  // File seconds on the song's grid (the grid chosen above; Free leaves them).
  double snapped(double seconds) const {
    const int64_t step = kGridSteps[std::clamp(grid_.getSelectedId(), 1, 4) - 1];
    if (step == 0) return seconds;
    const double tick = exactTickOf(seconds);
    return secondsOfTick((double)(std::llround(tick / step) * step));
  }

 public:
  void loopSelection() {
    if (!hasSelection()) return;
    const int64_t a = tickOf(selFrom_), b = tickOf(selTo_);
    owner_.edit([&](doc::Document& d) { d.loop = {true, a, b, d.loop.extra}; }, u8("Loop the selection"));
    owner_.session_->seek(a);
    owner_.session_->play();
  }

  // The selection becomes a clip of its own on a new audio track, at the same place in the song.
  void selectionToNewClip() {
    const doc::AudioClip* c = clip();
    const doc::Asset* a = asset();
    if (!c || !a || !hasSelection()) return;
    const doc::AudioClip source = *c;
    const int64_t from = tickOf(selFrom_), to = tickOf(selTo_);
    const std::string name = juce::File::createLegalFileName(u8(a->name)).upToLastOccurrenceOf(".", false, false).toStdString() + " (part)";
    std::string trackId, clipId;
    owner_.edit([&](doc::Document& d) {
      const std::string t = doc::addAudioTrack(d, name);
      const std::string c = doc::addAudioClip(d, t, source.assetId, source.startTick);
      for (auto& x : doc::trackById(d, t).audioClips)
        if (x.id == c) { const std::string id = x.id; x = source; x.id = id; }  // fades, gain, mute too
      doc::cropAudioClip(d, t, c, from, to);
      trackId = t;  // only once the edit has worked
      clipId = c;
    }, u8("Selection to new clip"));
    if (!clipId.empty()) owner_.selectClip(trackId, clipId);
  }

  // Gain so the loudest sample of the clip reaches -0.3 dBFS (the file is not changed). The peak is
  // read in a stream, so long clips need no big buffer.
  void normalize() {
    const doc::AudioClip* c = clip();
    const doc::Asset* a = asset();
    if (!c || !a) return;
    auto reader = openReader(*a);
    juce::int64 from = 0, frames = 0;
    if (!reader || !clipRange(*c, *reader, from, frames)) return;
    juce::Range<float> levels[2];
    reader->readMaxLevels(from, frames, levels, std::min(2, (int)reader->numChannels));
    float peak = 0;
    for (int ch = 0; ch < std::min(2, (int)reader->numChannels); ++ch) peak = std::max({peak, std::abs(levels[ch].getStart()), std::abs(levels[ch].getEnd())});
    if (peak <= 0.000001f) { owner_.showError(u8("Normalize"), u8("The clip is silent.")); return; }
    const double wanted = -0.3 - 20.0 * std::log10((double)peak), db = std::clamp(wanted, -60.0, 12.0);
    const std::string t = trackId_, id = clipId_;
    owner_.edit([&](doc::Document& d) { for (auto& x : doc::trackById(d, t).audioClips) if (x.id == id) x.gainDb = db; }, u8("Normalize"));
    owner_.notice_ = db < wanted ? u8("Normalized with +12 dB (the maximum); the peak is at ") + juce::String(-0.3 - (wanted - db), 1) + " dB."
                                 : u8("Normalized: gain ") + juce::String(db, 1) + " dB.";
  }

  // The clip starts at its first sound and ends after its last (under -60 dB is silence): the file does not change,
  // the clip's start and length do (one undo step). The audio stays where it is in the song.
  void cutSilence() {
    const doc::AudioClip* c = clip();
    const doc::Asset* a = asset();
    if (!c || !a) return;
    auto reader = openReader(*a);
    juce::int64 from = 0, frames = 0;
    if (!reader || !clipRange(*c, *reader, from, frames)) return;
    if (frames > (juce::int64)(reader->sampleRate * 10 * 60)) { owner_.showError(u8("Cut silence"), u8("The clip is longer than 10 minutes: split it first.")); return; }
    std::vector<std::vector<float>> channels;
    try {
      juce::AudioBuffer<float> buf((int)reader->numChannels, (int)frames);
      if (!reader->read(&buf, 0, (int)frames, from, true, true)) return;
      for (int ch = 0; ch < buf.getNumChannels(); ++ch) channels.emplace_back(buf.getReadPointer(ch), buf.getReadPointer(ch) + frames);
    } catch (const std::bad_alloc&) {
      owner_.showError(u8("Cut silence"), u8("Not enough memory for this clip."));
      return;
    }
    if (core::sampling::hasBrokenSamples(channels)) {  // deep hunt P08: not "silent"
      owner_.showError(u8("Cut silence"), u8("The clip has broken samples (NaN or Inf): the file is damaged."));
      return;
    }
    const auto r = core::sampling::audibleRange(channels, reader->sampleRate, -60, 1, 20);
    if (r.to <= r.from) { owner_.showError(u8("Cut silence"), u8("The clip is silent.")); return; }
    if (r.from == 0 && r.to == (int64_t)frames) { owner_.notice_ = u8("No silence to cut at the start or end."); return; }
    const double newStart = c->offsetSeconds + r.from / reader->sampleRate;
    const double newEnd = c->offsetSeconds + r.to / reader->sampleRate;
    const std::string t = trackId_, id = clipId_;
    const int64_t startTick = (int64_t)std::llround(exactTickOf(newStart));
    owner_.edit([&](doc::Document& d) {
      if (r.from > 0) doc::trimAudioClipStart(d, t, id, startTick);
      for (const auto& x : doc::trackById(d, t).audioClips)
        if (x.id == id) { doc::trimAudioClipEnd(d, t, id, std::max(doc::kMinClipSeconds, newEnd - x.offsetSeconds)); break; }
    }, u8("Cut silence"));
    selFrom_ = selTo_ = 0;
    showWholeClip();
  }

  // The clip's audio backwards: a new 32-bit float file (no clipping, no loss) in the project's Audio
  // folder, imported as its own asset; the original file stays. Written in chunks, from the end.
  // (A file no longer used after an undo stays in the folder.)
  void reverse() {
    const doc::AudioClip* c = clip();
    const doc::Asset* a = asset();
    if (!c || !a || owner_.projectDir().empty() || busy_) return;
    auto reader = openReader(*a);
    juce::int64 from = 0, frames = 0;
    if (!reader || !clipRange(*c, *reader, from, frames)) return;
    const juce::File temp = newAssetFile(*a, u8(" (reversed)"));
    bool written = false;
    {
      auto writer = floatWriter(temp, reader->sampleRate, (int)reader->numChannels);
      if (writer) {
        juce::AudioBuffer<float> chunk((int)reader->numChannels, 65536);
        written = true;
        for (juce::int64 end = from + frames; written && end > from;) {
          const int n = (int)std::min<juce::int64>(65536, end - from);
          written = reader->read(&chunk, 0, n, end - n, true, true);
          chunk.reverse(0, n);
          written = written && writer->writeFromAudioSampleBuffer(chunk, 0, n);
          end -= n;
        }
      }
    }
    if (!written) {
      temp.deleteFile();
      owner_.showError(u8("Reverse"), u8("Could not write the reversed file."));
      return;
    }
    adoptFile(trackId_, clipId_, temp, frames / reader->sampleRate, 1.0, true, u8("Reverse"));
  }

  // Tempo: the clip's audio played at the song's tempo, same pitch. originalBpm is the tempo it was
  // recorded at (a new file; the original stays). Runs in the background.
  void fitToTempo(double originalBpm) {
    const double songBpm = clipBpm();
    const double ratio = originalBpm / songBpm;  // output length / input length
    if (!(originalBpm >= 20 && originalBpm <= 999) || ratio < 0.25 || ratio > 4) {
      owner_.showError(u8("Fit to BPM"), u8("The original BPM must be between ") + juce::String(songBpm / 4, 1) + " and " +
                       juce::String(std::min(999.0, songBpm * 4), 1) + u8(" (up to 4 times slower or faster than the song)."));
      return;
    }
    processClip([ratio](const dsp::Channels& in) { return dsp::stretch(in, ratio); }, ratio,
                u8(" (") + juce::String(songBpm, 1) + " BPM)", u8("Fit to BPM"), ratio);
  }

  // Pitch in semitones, same duration (a new file; the original stays). Runs in the background.
  void shiftPitch(double semitones) {
    if (!(std::abs(semitones) <= 24) || semitones == 0) {
      owner_.showError(u8("Change pitch"), u8("Type a number of semitones between -24 and +24 (not 0)."));
      return;
    }
    processClip([semitones](const dsp::Channels& in) { return dsp::shiftPitch(in, semitones); }, std::pow(2.0, std::abs(semitones) / 12),
                u8(" (") + (semitones > 0 ? "+" : "") + juce::String(semitones, 1) + " st)", u8("Change pitch"), 1.0);
  }

  bool busy() const { return busy_; }

  // The song's tempo where the clip starts (the tempo map's, if the song changes tempo).
  double clipBpm() const {
    const doc::AudioClip* c = clip();
    return doc::bpmAt(owner_.history_.current(), c ? c->startTick : 0);
  }

  // The tempo a clip was probably made at: as if it were a whole number of bars long (within reach).
  double guessBpm() const {
    const doc::AudioClip* c = clip();
    const double bpm = clipBpm();
    if (!c || c->durationSeconds <= 0) return bpm;
    const double barSeconds = 4 * 60.0 / bpm;
    const double bars = std::max(1.0, std::round(c->durationSeconds / barSeconds));
    return std::clamp(bars * 4 * 60.0 / c->durationSeconds, std::max(20.0, bpm / 4), std::min(999.0, bpm * 4));
  }

  // VOICE STUTTER on this clip: fragment clips, then the word (one history step). Ctrl+Z gives the clip back.
  void voiceStutter(const doc::VoiceStutterPreset& preset) {
    const doc::AudioClip* c = clip();
    if (!c) return;
    const double from = hasSelection() ? selFrom_ : c->offsetSeconds, length = hasSelection() ? selTo_ - selFrom_ : std::min(0.12, c->durationSeconds);
    const std::string t = trackId_, id = clipId_;
    owner_.edit([&](doc::Document& d) { doc::voiceStutter(d, t, id, from, length, preset); }, u8("VOICE STUTTER: ") + u8(preset.name));  // a refusal shows its reason
    selFrom_ = selTo_ = 0;
    showWholeClip();
  }

  // One clip per step of the grid (Free: per beat).
  int slice(int64_t step) {
    const std::string t = trackId_, id = clipId_;
    int pieces = 0;
    owner_.edit([&](doc::Document& d) { pieces = doc::sliceAudioClip(d, t, id, step); }, u8("Slice"));
    return pieces;
  }

 private:
  int64_t gridStep() const {  // Free slices per beat
    const int64_t step = kGridSteps[std::clamp(grid_.getSelectedId(), 1, 4) - 1];
    return step > 0 ? step : kBeat;
  }

  void showMoreMenu() {
    juce::PopupMenu menu;
    const bool saved = !owner_.projectDir().empty();
    menu.addItem(1, u8("Normalize (peak at -0.3 dB)"));
    menu.addItem(juce::PopupMenu::Item(u8("Reverse")).setID(2).setEnabled(saved && !busy_));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Fit to the song's BPM…")).setID(4).setEnabled(saved && !busy_));
    menu.addItem(juce::PopupMenu::Item(u8("Change pitch (semitones)…")).setID(5).setEnabled(saved && !busy_));
    menu.addSeparator();
    menu.addItem(3, u8("Slice: one clip per ") + (grid_.getSelectedId() == 1 ? juce::String("beat") : u8(kGridNames[grid_.getSelectedId() - 1]).toLowerCase()));
    menu.addItem(juce::PopupMenu::Item(u8("Cut the silence at the start and end")).setID(7).setEnabled(!busy_));
    {
      // VOICE STUTTER: the selection is the fragment (no selection: the clip's first 120 ms); the result is clips.
      juce::PopupMenu voice;
      voice.addSectionHeader(hasSelection() ? u8("The selection is the fragment") : u8("Select the fragment first (now: the first 120 ms)"));
      std::string lastStyle;
      int n = 0;
      for (const auto& p : doc::voiceStutterPresets()) {
        if (p.style != lastStyle) { if (!lastStyle.empty()) voice.addSeparator(); lastStyle = p.style; }
        voice.addItem(juce::PopupMenu::Item(u8(p.name) + u8("  · ") + u8(p.style)).setID(100 + n++).setEnabled(!busy_));
      }
      menu.addSubMenu(u8("VOICE STUTTER"), voice);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&more_), [this](int chosen) {
      if (chosen == 7) cutSilence();
      if (chosen == 1) normalize();
      if (chosen == 2) reverse();
      if (chosen == 3) slice(gridStep());
      if (chosen >= 100 && chosen - 100 < (int)doc::voiceStutterPresets().size()) voiceStutter(doc::voiceStutterPresets()[(size_t)chosen - 100]);
      if (chosen == 4)
        owner_.askText(u8("The clip's original BPM (the song is at ") + juce::String(clipBpm(), 1) + " BPM there)",
                       juce::String(guessBpm(), 2), [this](const std::string& text) { fitToTempo(juce::String(text).replaceCharacter(',', '.').getDoubleValue()); });
      if (chosen == 5)
        owner_.askText(u8("Change pitch: semitones (-24 to +24; 12 = one octave)"), "0",
                       [this](const std::string& text) { shiftPitch(juce::String(text).replaceCharacter(',', '.').getDoubleValue()); });
    });
  }

  // A new file in the project's Audio folder, named after the asset (never one that exists).
  juce::File newAssetFile(const doc::Asset& a, const juce::String& suffix) const {
    const juce::String stem = juce::File::createLegalFileName(u8(a.name)).upToLastOccurrenceOf(".", false, false);
    const juce::File folder = fileOf(owner_.projectDir() / doc::kAssetFolder);
    folder.createDirectory();
    return folder.getNonexistentChildFile(juce::File::createLegalFileName(stem + suffix), ".wav", false);
  }

  static std::unique_ptr<juce::AudioFormatWriter> floatWriter(const juce::File& file, double sampleRate, int channels) {
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat format;
    return format.createWriterFor(out, juce::AudioFormatWriterOptions().withSampleRate(sampleRate).withNumChannels(channels)
                                           .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
  }

  // The clip plays a new file from its start: the file becomes an asset (importAudio keeps its own copy,
  // named by its content), fades follow the new timing, and the song grows to hold the clip if it can.
  void adoptFile(const std::string& t, const std::string& id, const juce::File& temp, double seconds, double fadeScale, bool swapFades,
                 const juce::String& label) {
    if (seconds < doc::kMinClipSeconds) {
      temp.deleteFile();
      owner_.showError(label, u8("The result would be too short (under 1 ms)."));
      return;
    }
    const std::filesystem::path dir = owner_.projectDir(), file = pathOf(temp);
    bool found = false;
    owner_.edit([&](doc::Document& d) {
      for (auto& x : doc::trackById(d, t).audioClips) found |= x.id == id;
      if (!found) return;  // the clip went away meanwhile
      const std::string made = doc::importAudio(d, file, dir);
      for (auto& x : doc::trackById(d, t).audioClips)
        if (x.id == id) {
          doc::markMadeFrom(d, made, x.assetId, label.toStdString());  // the Library's Project view shows where it came from
          x.assetId = made;
          x.offsetSeconds = 0;
          x.durationSeconds = seconds;
          if (swapFades) std::swap(x.fadeInSeconds, x.fadeOutSeconds);  // reversed: the fade-in becomes the end
          x.fadeInSeconds = std::min(x.fadeInSeconds * fadeScale, seconds);
          x.fadeOutSeconds = std::min(x.fadeOutSeconds * fadeScale, seconds);
          const int64_t end = doc::audioClipEndTick(d, x);
          if (end > d.lengthTicks) {
            try { doc::setSongLength(d, (end + kBar - 1) / kBar * kBar); } catch (const std::exception&) {}  // at the maximum: the end is cut
          }
        }
    }, label);
    temp.deleteFile();
    if (!found) owner_.notice_ = label + u8(": the clip is gone; nothing changed.");
    selFrom_ = selTo_ = 0;
    showWholeClip();
  }

  // The clip's part of the file, processed in the background (the window stays free), then adopted as a
  // new 32-bit float file. growth = the most the audio can grow (memory and time are bounded by it).
  void processClip(std::function<dsp::Channels(const dsp::Channels&)> process, double growth, const juce::String& suffix,
                   const juce::String& label, double fadeScale) {
    const doc::AudioClip* c = clip();
    const doc::Asset* a = asset();
    if (!c || !a || owner_.projectDir().empty() || busy_) return;
    auto reader = openReader(*a);
    juce::int64 from = 0, frames = 0;
    if (!reader || !clipRange(*c, *reader, from, frames)) return;
    const double sampleRate = reader->sampleRate;
    if (frames > (juce::int64)(sampleRate * 10 * 60) || frames * growth > sampleRate * 20 * 60) {
      owner_.showError(label, u8("The clip is too long for this (up to 10 minutes, and 20 for the result): split it first."));
      return;
    }
    dsp::Channels in;
    try {
      juce::AudioBuffer<float> buf((int)reader->numChannels, (int)frames);
      if (!reader->read(&buf, 0, (int)frames, from, true, true)) { owner_.showError(label, u8("Could not read the audio.")); return; }
      in.resize((size_t)buf.getNumChannels());
      for (int ch = 0; ch < buf.getNumChannels(); ++ch) in[(size_t)ch].assign(buf.getReadPointer(ch), buf.getReadPointer(ch) + frames);
    } catch (const std::bad_alloc&) {
      owner_.showError(label, u8("Not enough memory for this clip."));
      return;
    }
    const juce::File temp = newAssetFile(*a, suffix);
    busy_ = true;
    owner_.notice_ = label + u8(": processing...");
    juce::Component::SafePointer<AudioEditor> self(this);
    const std::string t = trackId_, id = clipId_;
    std::thread([self, in = std::move(in), process = std::move(process), temp, sampleRate, label, t, id, fadeScale]() mutable {
      juce::String error;
      double seconds = 0;
      try {
        dsp::Channels result = process(in);
        in.clear();
        in.shrink_to_fit();
        if (result.empty() || result[0].empty()) throw std::runtime_error("empty");
        seconds = result[0].size() / sampleRate;
        auto writer = floatWriter(temp, sampleRate, (int)result.size());
        std::vector<const float*> channels;
        for (const auto& ch : result) channels.push_back(ch.data());
        if (!writer || !writer->writeFromFloatArrays(channels.data(), (int)channels.size(), (int)result[0].size())) error = u8("Could not write the new file.");
      } catch (const std::bad_alloc&) {
        error = u8("Not enough memory for this clip.");
      } catch (const std::exception&) {
        error = u8("Could not process the audio.");
      }
      juce::MessageManager::callAsync([self, error, temp, seconds, label, t, id, fadeScale] {
        if (self == nullptr) { temp.deleteFile(); return; }
        self->busy_ = false;
        self->owner_.notice_.clear();
        if (error.isNotEmpty()) { temp.deleteFile(); self->owner_.showError(label, error); return; }
        self->adoptFile(t, id, temp, seconds, fadeScale, false, label);
      });
    }).detach();
  }

  std::unique_ptr<juce::AudioFormatReader> openReader(const doc::Asset& a) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(fileOf(doc::assetFile(a, owner_.projectDir()))));
    if (!reader) owner_.showError(u8("Audio"), u8("Could not read ") + u8(a.name) + ".");
    return reader;
  }

  // The part of the file the clip plays, in frames at the file's own rate.
  bool clipRange(const doc::AudioClip& c, const juce::AudioFormatReader& reader, juce::int64& from, juce::int64& frames) {
    from = (juce::int64)std::llround(c.offsetSeconds * reader.sampleRate);
    frames = std::min<juce::int64>(reader.lengthInSamples - from, (juce::int64)std::llround(c.durationSeconds * reader.sampleRate));
    if (frames > 0) return true;
    owner_.showError(u8("Audio"), u8("The clip is past the end of its file (did the file change?)."));
    return false;
  }

  void showWholeClip() {
    if (const doc::AudioClip* c = clip()) {
      const double pad = c->durationSeconds * 0.03;
      viewFrom_ = c->offsetSeconds - pad;
      viewTo_ = c->offsetSeconds + c->durationSeconds + pad;
    }
    updateButtons();
    repaint();
  }

  void updateButtons() {
    const doc::Asset* a = asset();
    const doc::AudioClip* c = clip();
    title_.setText(a && c ? u8(a->name) + u8(" · clip of ") + juce::String(c->durationSeconds, 2) + " s" +
                                (hasSelection() ? u8(" · selection ") + juce::String(selTo_ - selFrom_, 3) + " s" : juce::String())
                          : juce::String(), juce::dontSendNotification);
    for (auto* b : {&keep_, &remove_, &fadeIn_, &fadeOut_, &loopSel_, &toClip_}) b->setEnabled(hasSelection());
  }

  void splitHere() {
    const doc::AudioClip* c = clip();
    if (!c) return;
    const int64_t at = hasSelection() ? tickOf(selFrom_) : (int64_t)owner_.snap_.positionTick;
    const std::string t = trackId_, id = clipId_;
    owner_.edit([&](doc::Document& d) { doc::splitAudioClip(d, t, id, at); }, u8("Split audio clip"));
    selFrom_ = selTo_ = 0;
    documentChanged();
  }

  void fade(bool in) {
    const doc::AudioClip* c = clip();
    if (!c || !hasSelection()) return;
    const double seconds = in ? selTo_ - c->offsetSeconds : c->offsetSeconds + c->durationSeconds - selFrom_;
    const std::string t = trackId_, id = clipId_;
    owner_.edit([&](doc::Document& d) {
      for (auto& x : doc::trackById(d, t).audioClips)
        if (x.id == id) (in ? x.fadeInSeconds : x.fadeOutSeconds) = std::min(seconds, x.durationSeconds);
    }, in ? juce::String("Fade in") : juce::String("Fade out"));
  }

  void resized() override {
    auto top = getLocalBounds().reduced(10, 6).removeFromTop(28);
    title_.setBounds(top.removeFromLeft(200));
    grid_.setBounds(top.removeFromRight(110).reduced(2, 0));
    for (auto* b : {&keep_, &remove_, &split_, &fadeIn_, &fadeOut_, &loopSel_, &toClip_, &more_, &whole_})
      b->setBounds(top.removeFromLeft(b == &keep_ || b == &remove_ || b == &toClip_ ? 110 : b == &loopSel_ ? 60 : b == &more_ ? 70 : 76).reduced(2, 0));
    hint_.setBounds(top);
  }

  void paint(juce::Graphics& g) override {
    g.setColour(kPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
    const doc::AudioClip* c = clip();
    const doc::Asset* a = asset();
    const auto area = waveArea();
    g.setColour(kField);
    g.fillRect(area);
    if (!c || !a) return;
    const double clipFrom = c->offsetSeconds, clipTo = c->offsetSeconds + c->durationSeconds;
    // Time ruler (seconds of the file), a step that leaves room for the labels.
    const double span = viewTo_ - viewFrom_;
    double step = 0.001;
    for (double s : {0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0})
      if (area.getWidth() * s / span >= 70) { step = s; break; } else step = s;
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (double s = std::ceil(std::max(0.0, viewFrom_) / step) * step; s <= viewTo_; s += step) {
      const float x = xOf(s);
      g.setColour(kOutline);
      g.drawVerticalLine((int)x, (float)area.getY(), (float)area.getBottom());
      g.setColour(kMuted);
      const int whole = (int)s;
      const juce::String label = step >= 1 ? juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2) : juce::String(s, step >= 0.01 ? 2 : 3) + " s";
      g.drawText(label, (int)x + 3, area.getY() - 16, 70, 14, juce::Justification::centredLeft);
    }
    // The clip's part of the file is lit; the rest of the file is dim (trimmed away, still there).
    const float cx0 = std::max((float)area.getX(), xOf(clipFrom)), cx1 = std::min((float)area.getRight(), xOf(clipTo));
    if (cx1 > cx0) {
      g.setColour(kField.brighter(0.06f));
      g.fillRect(juce::Rectangle<float>(cx0, (float)area.getY(), cx1 - cx0, (float)area.getHeight()));
    }
    const bool missing = std::count(owner_.missing_.begin(), owner_.missing_.end(), a->id) > 0;
    std::shared_ptr<const Waveform> wave;
    if (!missing) wave = owner_.waves_.get(a->sha256, fileOf(doc::assetFile(*a, owner_.projectDir())));
    const float mid = (float)area.getCentreY(), half = area.getHeight() / 2.0f - 4;
    const float gain = (float)std::pow(10.0, c->gainDb / 20);
    if (wave) {
      const double perPixel = span / std::max(1, area.getWidth());
      for (int x = area.getX(); x < area.getRight(); ++x) {
        const double s0 = secondsAt(x);
        if (s0 + perPixel < 0 || s0 > a->seconds()) continue;
        const bool inClip = s0 >= clipFrom - perPixel && s0 <= clipTo;
        drawWaveColumn(g, *wave, (float)x, mid, half, s0, s0 + perPixel, inClip ? (c->mute ? kMuted : kAccent) : kMuted.withAlpha(0.35f),
                       inClip ? gain : 1.0f);
      }
    } else {
      g.setColour(missing ? kRed : kMuted);
      g.drawText(lang::tr(missing ? u8("Missing file: ") + u8(a->name) : u8("Computing the waveform...")), area, juce::Justification::centred);
    }
    g.setColour(kOutline);
    g.drawHorizontalLine((int)mid, (float)area.getX(), (float)area.getRight());
    // Clip edges and fades.
    g.setColour(kText.withAlpha(0.8f));
    for (double s : {clipFrom, clipTo})
      if (s >= viewFrom_ && s <= viewTo_) g.drawVerticalLine((int)xOf(s), (float)area.getY(), (float)area.getBottom());
    g.setColour(kAmber.withAlpha(0.9f));
    if (c->fadeInSeconds > 0) g.drawLine(xOf(clipFrom), (float)area.getBottom(), xOf(clipFrom + c->fadeInSeconds), (float)area.getY(), 1.5f);
    if (c->fadeOutSeconds > 0) g.drawLine(xOf(clipTo - c->fadeOutSeconds), (float)area.getY(), xOf(clipTo), (float)area.getBottom(), 1.5f);
    // Selection, then the playhead.
    const float sx0 = std::max((float)area.getX(), xOf(selFrom_)), sx1 = std::min((float)area.getRight(), xOf(selTo_));
    if (hasSelection() && sx1 >= sx0) {  // drawn only where it is in view
      const float x0 = sx0, x1 = sx1;
      g.setColour(kAccent.withAlpha(0.2f));
      g.fillRect(juce::Rectangle<float>(x0, (float)area.getY(), std::max(1.0f, x1 - x0), (float)area.getHeight()));
      g.setColour(kAccent);
      g.drawRect(juce::Rectangle<float>(x0, (float)area.getY(), std::max(1.0f, x1 - x0), (float)area.getHeight()), 1.0f);
    }
    const double play = secondsOfTick(owner_.playTick_);
    if (play >= viewFrom_ && play <= viewTo_) {
      g.setColour(kText);
      g.fillRect(xOf(play) - 1.0f, (float)area.getY(), 2.0f, (float)area.getHeight());
    }
  }

  void mouseDown(const juce::MouseEvent& e) override {
    dragging_ = waveArea().contains(e.getPosition());
    anchor_ = secondsAt(e.x);
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (dragging_ && e.getDistanceFromDragStart() >= 3) select(snapped(anchor_), snapped(secondsAt(e.x)));
  }
  // A click (no drag): no selection, and the playhead moves there.
  void mouseUp(const juce::MouseEvent& e) override {
    if (!dragging_) return;
    dragging_ = false;
    if (e.getDistanceFromDragStart() < 3) {
      selFrom_ = selTo_ = 0;
      updateButtons();
      if (const doc::AudioClip* c = clip())
        owner_.session_->seek(std::max<int64_t>(0, tickOf(std::clamp(anchor_, c->offsetSeconds, c->offsetSeconds + c->durationSeconds))));
      repaint();
    }
  }
  // Wheel: zoom around the mouse; Shift (or a sideways wheel): move along the file.
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
    const doc::Asset* a = asset();
    if (!a) return;
    const double span = viewTo_ - viewFrom_;
    if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY)) {
      const double move = -(std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY) * span * 0.5;
      viewFrom_ += move;
      viewTo_ += move;
    } else {
      const double at = secondsAt(e.x), factor = w.deltaY > 0 ? 0.8 : 1.25;
      const double next = std::clamp(span * factor, 0.01, std::max(1.0, a->seconds() * 1.2));
      viewFrom_ = at - (at - viewFrom_) * next / span;
      viewTo_ = viewFrom_ + next;
    }
    repaint();
  }

  MainComponent& owner_;
  std::string trackId_, clipId_;
  double viewFrom_ = 0, viewTo_ = 1;   // file seconds shown
  double selFrom_ = 0, selTo_ = 0;     // file seconds selected (empty when equal)
  double anchor_ = 0;
  bool dragging_ = false;
  juce::Label title_, hint_;
  juce::TextButton keep_, remove_, split_, fadeIn_, fadeOut_, whole_, loopSel_, toClip_, more_;
  bool busy_ = false;  // a tempo or pitch change is being computed
  juce::ComboBox grid_;
  static constexpr const char* kGridNames[] = {"Free", "1/16", "Beat", "Bar"};
  static constexpr int64_t kGridSteps[] = {0, kBeat / 4, kBeat, kBar};  // Free: 0
};

// ---------------------------------------------------------------- automation: one parameter along the song.
// The curve lines up with the clip lanes above (same zoom); points snap to 1/16 (Shift: free).

class MainComponent::AutomationEditor final : public juce::Component {
public:
  explicit AutomationEditor(MainComponent& owner) : owner_(owner) {
    title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    hint_.setFont(juce::FontOptions(theme::kFontBody));
    hint_.setColour(juce::Label::textColourId, kMuted);
    hint_.setJustificationType(juce::Justification::centredRight);
    hint_.setText(u8("Click: point · Drag: move (Shift: off the grid) · Right-click or double-click: delete"), juce::dontSendNotification);
    params_.setTooltip(u8("What this lane moves: volume and pan here, each effect and the instrument in its own submenu (● = already automated)"));
    params_.onClick = [this] { showParamMenu(); };
    clear_.setButtonText("Clear");
    clear_.setTooltip(u8("Remove this value's automation (back to the fixed value)"));
    clear_.onClick = [this] {
      const std::string t = trackId_, p = param_;
      owner_.edit([&](doc::Document& d) { doc::clearAutomation(d, t, p); }, u8("Clear automation"));
    };
    close_.setButtonText(u8("✕"));
    close_.setTooltip(u8("Close (Esc): back to the pattern editor"));
    close_.onClick = [this] { owner_.closeAutomation(); };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &hint_, &params_, &clear_, &close_}) {
      c->setWantsKeyboardFocus(false);
      addAndMakeVisible(c);
    }
  }

  const std::string& target() const { return trackId_; }
  const std::string& param() const { return param_; }

  void setTarget(const std::string& trackId, const std::string& param) {
    trackId_ = trackId;
    param_ = param;
    documentChanged();
  }

  void documentChanged() {
    const doc::Document& d = owner_.history_.current();
    const doc::Track* t = trackId_.empty() ? nullptr : d.findTrack(trackId_);
    list_ = doc::automatableParams(d, trackId_);
    // The menu is rebuilt only when what it lists changes (not on every drag of a point).
    juce::String key = trackId_ + "|" + param_;
    for (const auto& p : list_) key << p.param << (doc::findAutomation(d, trackId_, p.param) ? "*" : "") << p.label << "|";
    if (key != menuKey_) {
      menuKey_ = key;
      title_.setText(u8("Automation · ") + (t ? u8(t->name) : juce::String("Master")), juce::dontSendNotification);
      const bool found = std::any_of(list_.begin(), list_.end(), [&](const doc::AutomatableParam& p) { return p.param == param_; });
      if (!found && !list_.empty()) param_ = list_[0].param;
    }
    const doc::AutomatableParam* shown = paramInfo();
    params_.setButtonText((shown ? u8(shown->label) : juce::String()) + u8("  ▾"));
    clear_.setEnabled(doc::findAutomation(d, trackId_, param_) != nullptr);
    repaint();
  }

  // Diogo 2026-11-07: one long list was hard to read. Volume and pan come first; then each effect (and the
  // instrument, and each plugin) is one entry whose submenu lists only its own settings. "METAL · Ring" is
  // grouped under METAL; two effects of the same kind are told apart by their place in the chain (METAL 2).
  struct ParamGroup { std::string key, name; std::vector<size_t> items; };
  static std::vector<ParamGroup> groupParams(const std::vector<doc::AutomatableParam>& list, std::vector<size_t>& loose) {
    std::vector<ParamGroup> groups;
    for (size_t i = 0; i < list.size(); ++i) {
      const std::string& label = list[i].label;
      const size_t dot = label.find(" \xc2\xb7 ");  // " · "
      if (dot == std::string::npos) { loose.push_back(i); continue; }
      std::string key = label.substr(0, dot);
      const std::string& param = list[i].param;
      if (param.rfind("fx:", 0) == 0) key = param.substr(0, param.find(':', 3));  // fx:<effect id>
      auto g = std::find_if(groups.begin(), groups.end(), [&](const ParamGroup& x) { return x.key == key; });
      if (g == groups.end()) { groups.push_back({key, label.substr(0, dot), {}}); g = groups.end() - 1; }
      g->items.push_back(i);
    }
    std::map<std::string, int> seen;  // the same name twice: METAL, METAL 2
    for (auto& g : groups) if (const int n = ++seen[g.name]; n > 1) g.name += " " + std::to_string(n);
    return groups;
  }

  void showParamMenu() {
    const doc::Document& d = owner_.history_.current();
    auto automated = [&](size_t i) { return doc::findAutomation(d, trackId_, list_[i].param) != nullptr; };
    const juce::String mark = u8("  ●");
    juce::PopupMenu menu;
    std::vector<size_t> loose;
    const auto groups = groupParams(list_, loose);
    for (size_t i : loose) menu.addItem((int)i + 1, u8(list_[i].label) + (automated(i) ? mark : juce::String()), true, list_[i].param == param_);
    if (!loose.empty() && !groups.empty()) menu.addSeparator();
    for (const auto& g : groups) {
      juce::PopupMenu sub;
      bool any = false, current = false;
      for (size_t i : g.items) {
        const std::string& label = list_[i].label;
        const size_t dot = label.find(" \xc2\xb7 ");
        sub.addItem((int)i + 1, u8(label.substr(dot + 4)) + (automated(i) ? mark : juce::String()), true, list_[i].param == param_);
        any = any || automated(i);
        current = current || list_[i].param == param_;
      }
      menu.addSubMenu(u8(g.name) + (any ? mark : juce::String()), sub, true, nullptr, current);
    }
    juce::Component::SafePointer<AutomationEditor> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&params_), [self](int id) {
      if (!self || id <= 0 || id > (int)self->list_.size()) return;
      self->param_ = self->list_[(size_t)id - 1].param;
      self->documentChanged();
    });
  }

  // The same as a click there (self-test).
  void addPoint(int64_t tick, double value) {
    const std::string t = trackId_, p = param_;
    owner_.edit([&](doc::Document& d) { doc::setAutomationPoint(d, t, p, tick, value); }, u8("Automation point"));
  }

  void resized() override {
    auto top = getLocalBounds().reduced(10, 6).removeFromTop(28);
    close_.setBounds(top.removeFromRight(32).reduced(2, 0));
    title_.setBounds(top.removeFromLeft(200));
    params_.setBounds(top.removeFromLeft(240).reduced(2, 0));
    clear_.setBounds(top.removeFromLeft(80).reduced(2, 0));
    hint_.setBounds(top);
  }

  void paint(juce::Graphics& g) override {
    g.setColour(kPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
    const doc::AutomatableParam* info = paramInfo();
    if (!info) return;
    const auto area = plot();
    g.setColour(kField);
    g.fillRect(area);
    // Bars, as on the ruler.
    const doc::Document& d = owner_.history_.current();
    const double barPixels = area.getWidth() * kBar / owner_.viewLength();
    const int every = barPixels >= 28 ? 1 : barPixels >= 14 ? 2 : barPixels >= 7 ? 4 : barPixels >= 2 ? 16 : 64;
    g.setColour(kOutline.withAlpha(0.6f));
    for (int64_t b = (int64_t)(owner_.viewStart() / kBar); b * kBar <= std::min<double>((double)d.lengthTicks, owner_.viewStart() + owner_.viewLength()); ++b)
      if (b % every == 0) g.drawVerticalLine(xOf((double)(b * kBar)), (float)area.getY(), (float)area.getBottom());
    // The range, on the left.
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.setColour(kMuted);
    const auto scale = juce::Rectangle<int>(10, area.getY(), area.getX() - 16, area.getHeight());
    g.drawText(format(*info, info->max), scale.withHeight(14), juce::Justification::centredRight);
    g.drawText(format(*info, info->min), scale.withTrimmedTop(scale.getHeight() - 14), juce::Justification::centredRight);
    g.drawText(u8(info->label), scale.withSizeKeepingCentre(scale.getWidth(), 16), juce::Justification::centredRight);
    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(area);
    const doc::Automation* a = doc::findAutomation(d, trackId_, param_);
    if (!a) {  // the fixed value, dashed
      g.setColour(kMuted);
      const float y = (float)yOf(info->current);
      const float dashes[] = {6.0f, 4.0f};
      g.drawDashedLine(juce::Line<float>((float)area.getX(), y, (float)area.getRight(), y), dashes, 2, 1.5f);
      g.drawText(lang::tr(u8("No automation: the fixed value is ") + format(*info, info->current) + u8(". Click to add the first point.")),
                 area.withTrimmedTop(area.getHeight() / 2 + 8).withHeight(18), juce::Justification::centred);
    } else {
      juce::Path curve;
      curve.startNewSubPath((float)area.getX(), (float)yOf(a->points.front().value));
      for (const auto& p : a->points) curve.lineTo((float)xOf((double)p.tick), (float)yOf(p.value));
      curve.lineTo((float)area.getRight(), (float)yOf(a->points.back().value));
      g.setColour(kAccent);
      g.strokePath(curve, juce::PathStrokeType(2.0f));
      for (size_t i = 0; i < a->points.size(); ++i) {
        const auto& p = a->points[i];
        const juce::Point<float> at((float)xOf((double)p.tick), (float)yOf(p.value));
        g.setColour((int)i == drag_ ? kAccent : kText);
        g.fillEllipse(juce::Rectangle<float>(9, 9).withCentre(at));
      }
      if (drag_ >= 0 && drag_ < (int)a->points.size()) {  // the value being set
        const auto& p = a->points[(size_t)drag_];
        g.setColour(kText);
        g.drawText(format(*info, p.value), xOf((double)p.tick) + 8, yOf(p.value) - 18, 90, 16, juce::Justification::centredLeft);
      }
    }
    g.setColour(kText.withAlpha(0.8f));
    g.fillRect(xOf(owner_.playTick_) - 1, area.getY(), 2, area.getHeight());
  }

  void mouseDown(const juce::MouseEvent& e) override {
    if (!plot().contains(e.getPosition()) || !paramInfo()) return;
    const int hit = pointAt(e.getPosition());
    const std::string t = trackId_, p = param_;
    if (hit >= 0 && e.mods.isPopupMenu()) {
      owner_.edit([&](doc::Document& d) { doc::removeAutomationPoint(d, t, p, hit); }, u8("Delete point"));
      return;
    }
    if (e.mods.isPopupMenu()) return;
    owner_.beginGesture(u8("Automation"));
    gesture_ = true;
    if (hit >= 0) { drag_ = hit; repaint(); return; }
    int index = -1;  // a new point where the click is, then dragged
    const int64_t tick = tickAt(e.x, e.mods.isShiftDown());
    const double value = valueAt(e.y);
    owner_.edit([&](doc::Document& d) { index = doc::setAutomationPoint(d, t, p, tick, value); }, u8("Automation"));
    drag_ = index;
  }

  void mouseDrag(const juce::MouseEvent& e) override {
    if (drag_ < 0) return;
    const std::string t = trackId_, p = param_;
    const int index = drag_;
    const int64_t tick = tickAt(e.x, e.mods.isShiftDown());
    const double value = valueAt(e.y);
    owner_.edit([&](doc::Document& d) { doc::moveAutomationPoint(d, t, p, index, tick, value); }, u8("Automation"));
  }

  void mouseUp(const juce::MouseEvent&) override {
    drag_ = -1;
    if (gesture_) owner_.endGesture();
    gesture_ = false;
    repaint();
  }

  void mouseDoubleClick(const juce::MouseEvent& e) override {
    const int hit = pointAt(e.getPosition());
    if (hit < 0) return;
    const std::string t = trackId_, p = param_;
    owner_.edit([&](doc::Document& d) { doc::removeAutomationPoint(d, t, p, hit); }, u8("Delete point"));
  }

  // The wheel zooms and scrolls the song, as over the lanes.
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { owner_.mouseWheelMove(e.getEventRelativeTo(&owner_), w); }

private:
  const doc::AutomatableParam* paramInfo() const {
    for (const auto& p : list_) if (p.param == param_) return &p;
    return nullptr;
  }
  // Under the clip lanes: the same x for the same tick.
  juce::Rectangle<int> plot() const {
    const auto lanes = owner_.laneSpan();
    return juce::Rectangle<int>::leftTopRightBottom(lanes.getX() - getX(), 44, lanes.getRight() - getX(), getHeight() - 10);
  }
  int xOf(double tick) const { return owner_.xOfTick(tick) - getX(); }
  int64_t tickAt(int x, bool free) const { return owner_.tickAt(x + getX(), free ? 1 : std::min<int64_t>(kBeat / 4, owner_.gridStep(8))); }  // 1/16, finer when zoomed in
  double norm(double v) const {  // 0..1 up the plot (logarithmic for frequencies and times)
    const doc::AutomatableParam* i = paramInfo();
    if (!i || i->max <= i->min) return 0;
    if (i->logScale && i->min > 0) return std::log(v / i->min) / std::log(i->max / i->min);
    return (v - i->min) / (i->max - i->min);
  }
  int yOf(double v) const {
    const auto a = plot();
    return a.getBottom() - 4 - (int)std::round(juce::jlimit(0.0, 1.0, norm(v)) * (a.getHeight() - 8));
  }
  double valueAt(int y) const {
    const doc::AutomatableParam* i = paramInfo();
    const auto a = plot();
    const double f = juce::jlimit(0.0, 1.0, (a.getBottom() - 4 - y) / (double)std::max(1, a.getHeight() - 8));
    if (!i) return 0;
    if (i->logScale && i->min > 0) return i->min * std::pow(i->max / i->min, f);
    return i->min + f * (i->max - i->min);
  }
  int pointAt(juce::Point<int> at) const {
    const doc::Automation* a = doc::findAutomation(owner_.history_.current(), trackId_, param_);
    if (!a) return -1;
    for (size_t i = 0; i < a->points.size(); ++i)
      if (std::abs(xOf((double)a->points[i].tick) - at.x) <= 6 && std::abs(yOf(a->points[i].value) - at.y) <= 6) return (int)i;
    return -1;
  }
  static juce::String format(const doc::AutomatableParam& p, double v) {
    if (p.unit == "dB") return juce::String(v, 1) + " dB";
    if (p.unit == "Hz") return v >= 1000 ? juce::String(v / 1000, 2) + " kHz" : juce::String((int)std::lround(v)) + " Hz";
    if (p.unit == "ms") return juce::String(v, 0) + " ms";
    if (p.param == "pan") return std::abs(v) < 0.005 ? u8("center") : (v < 0 ? "L " : "R ") + juce::String((int)std::lround(std::abs(v) * 100));
    return juce::String(v, 2);
  }

  MainComponent& owner_;
  std::string trackId_, param_;
  std::vector<doc::AutomatableParam> list_;
  juce::Label title_, hint_;
  juce::TextButton params_;  // opens the grouped menu (showParamMenu)
  juce::TextButton clear_, close_;
  int drag_ = -1;
  bool gesture_ = false;
  juce::String menuKey_;
};

// ---------------------------------------------------------------- main component

// ---------------------------------------------------------------- the footer's AI button

// "AICORE" and a dot, at the bottom right (Diogo, 2026-11-06): green when AICORE's chat is open and connected to an
// AI model, red when there is no connection; the number of proposals waiting is in the dot. A click opens AICORE's
// chat (and the proposals, when some wait); a right-click shows the AICORE menu.
class MainComponent::AiBadge final : public juce::Button {
public:
  AiBadge() : juce::Button("AICORE") {}
  std::function<void(bool popup)> onPress;
  void setDot(juce::Colour c) { dot_ = c; repaint(); }
  void setCount(int n) { if (n != count_) { count_ = n; repaint(); } }

  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const auto r = getLocalBounds().toFloat().reduced(2.0f, 2.0f);
    if (over || down || getToggleState()) {
      g.setColour(over || down ? theme::kControlHover : theme::kControl);
      g.fillRoundedRectangle(r, theme::kCorner);
    }
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
    auto text = r.withTrimmedRight(22.0f);
    g.drawText("AICORE", text, juce::Justification::centredRight);
    g.setColour(dot_);
    const auto dot = juce::Rectangle<float>(count_ > 0 ? 15.0f : 9.0f, count_ > 0 ? 15.0f : 9.0f).withCentre({r.getRight() - 12.0f, r.getCentreY()});
    g.fillEllipse(dot);
    if (count_ > 0) {
      g.setColour(theme::kBackground);
      g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
      g.drawText(count_ > 9 ? juce::String("9+") : juce::String(count_), dot, juce::Justification::centred);
    }
  }

private:
  void clicked(const juce::ModifierKeys& mods) override { if (onPress) onPress(mods.isPopupMenu()); }
  juce::Colour dot_ = kRed;
  int count_ = 0;
};

// The tracks panel's scrolling: the wheel over the track names (or with Shift) scrolls the tracks; over the
// lanes it keeps moving and zooming the song, as before.
// The selection box, drawn over the tracks while it is dragged (it takes no clicks).
class MainComponent::BoxOverlay final : public juce::Component {
public:
  explicit BoxOverlay(MainComponent& owner) : owner_(owner) { setInterceptsMouseClicks(false, false); }
  void paint(juce::Graphics& g) override {
    if (!owner_.boxing_) return;
    const auto r = juce::Rectangle<int>(getLocalPoint(nullptr, owner_.boxFrom_), getLocalPoint(nullptr, owner_.boxTo_)).toFloat();
    g.setColour(kAccent.withAlpha(0.12f));
    g.fillRect(r);
    g.setColour(kAccent.withAlpha(0.8f));
    g.drawRect(r, 1.0f);
  }

private:
  MainComponent& owner_;
};

class MainComponent::TracksView final : public juce::Viewport {
public:
  explicit TracksView(MainComponent& owner) : owner_(owner) {
    setScrollBarsShown(true, false);
    setScrollBarThickness(kTracksScrollBar - 2);
    setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
  }
  // Over the clips the wheel moves along the song (Ctrl+wheel zooms); over the track names it scrolls the
  // tracks up and down (when they do not all fit). Shift+wheel or a sideways swipe always moves along the song.
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
    const auto at = e.getEventRelativeTo(&owner_);
    const bool sideways = std::abs(w.deltaX) > std::abs(w.deltaY);
    const bool overflow = getViewedComponent() != nullptr && getViewedComponent()->getHeight() > getViewHeight();
    const bool overClips = at.x >= owner_.laneSpan().getX();
    if (e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown() || sideways || !overflow || overClips) {
      if (at.x >= owner_.laneSpan().getX() || e.mods.isShiftDown() || sideways) {
        juce::MouseWheelDetails along = w;
        if (e.mods.isShiftDown() && !sideways) { along.deltaX = -w.deltaY; along.deltaY = 0; }
        owner_.mouseWheelMove(at.withNewPosition(juce::Point<int>(std::max(at.x, owner_.laneSpan().getX()), at.y)), along);
      }
      return;
    }
    juce::MouseWheelDetails vertical = w;
    vertical.deltaX = 0;
    juce::Viewport::mouseWheelMove(e, vertical);
  }

private:
  MainComponent& owner_;
};

// The song's overview under the lanes while zoomed in: the thumb is what is shown; drag it to move along.
class MainComponent::TimeBar final : public juce::ScrollBar, public juce::SettableTooltipClient, private juce::ScrollBar::Listener {
public:
  explicit TimeBar(MainComponent& owner) : juce::ScrollBar(false), owner_(owner) {
    setAutoHide(false);
    addListener(this);
    setTooltip(u8("Drag to move along the song. Ctrl+wheel zooms; Alt (or the middle button) + drag on the tracks grabs the song"));
  }

private:
  void scrollBarMoved(juce::ScrollBar*, double start) override {
    if (!updating) owner_.setView(start, owner_.viewLength());
  }
  MainComponent& owner_;

public:
  bool updating = false;
};

void MainComponent::updateTimeBar() {
  if (!timeBar_) return;
  // Always shown (the whole song: the thumb fills it), so it is easy to find; Ctrl+wheel zooms in.
  timeBar_->setVisible(true);
  timeBar_->updating = true;
  timeBar_->setRangeLimits(0.0, viewSpan());
  timeBar_->setCurrentRange(viewStart(), viewLength());
  timeBar_->updating = false;
}

// Minimise, maximise (restore when maximised) and close, drawn by Core at the top bar's right end, as on
// Windows 11: thin icons, a light hover, red under the close button. On Windows the system handles their
// clicks (MainWindow::findControlAtPoint: snap layouts on the maximise button); onClick is for a plain click
// that reaches the button (keyboard, accessibility).
class MainComponent::WindowButton final : public juce::Button {
public:
  enum class Kind { minimise, maximise, close };
  explicit WindowButton(Kind kind) : juce::Button(kind == Kind::close ? "Close" : kind == Kind::minimise ? "Minimise" : "Maximise"), kind_(kind) {
    setWantsKeyboardFocus(false);
    onClick = [this] {
      auto* window = findParentComponentOfClass<juce::DocumentWindow>();
      if (window == nullptr) return;
      if (kind_ == Kind::close) window->closeButtonPressed();
      else if (kind_ == Kind::minimise) window->minimiseButtonPressed();
      else window->maximiseButtonPressed();
    };
  }
  Kind kind() const { return kind_; }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    const auto* window = findParentComponentOfClass<juce::ResizableWindow>();
    const bool maximised = window != nullptr && window->isFullScreen();
    if (over || down) {
      g.setColour(kind_ == Kind::close ? juce::Colour(0xffc42b1c).withAlpha(down ? 0.8f : 1.0f) : kText.withAlpha(down ? 0.14f : 0.09f));
      g.fillRect(getLocalBounds());
    }
    g.setColour(kind_ == Kind::close && (over || down) ? juce::Colours::white : kText.withAlpha(0.85f));
    const auto c = getLocalBounds().toFloat().getCentre();
    constexpr float h = 5.0f;  // half the icon's size
    if (kind_ == Kind::minimise) g.fillRect(c.x - h, c.y, 2 * h, 1.0f);
    else if (kind_ == Kind::close) {
      g.drawLine(c.x - h, c.y - h, c.x + h, c.y + h, 1.1f);
      g.drawLine(c.x - h, c.y + h, c.x + h, c.y - h, 1.1f);
    } else if (!maximised) g.drawRoundedRectangle(c.x - h, c.y - h, 2 * h, 2 * h, 1.5f, 1.0f);
    else {  // restore: two overlapping squares
      g.drawRoundedRectangle(c.x - h, c.y - h + 2, 2 * h - 2, 2 * h - 2, 1.5f, 1.0f);
      g.drawLine(c.x - h + 2, c.y - h, c.x + h, c.y - h, 1.0f);
      g.drawLine(c.x + h, c.y - h, c.x + h, c.y + h - 2, 1.0f);
    }
  }

private:
  Kind kind_;
};

MainComponent::MainComponent(EngineSession::Options options, doc::Document document, juce::String loadProblem)
    : history_(std::move(document)), loadProblem_(std::move(loadProblem)) {
  juce::PropertiesFile::Options storage;
  storage.applicationName = "Core";
  storage.folderName = "Core";
  storage.filenameSuffix = ".settings";
  settings_.setStorageParameters(storage);
  realOutput_ = options.backend.empty();  // false for tests (null device): user preferences stay untouched
  if (realOutput_) Keymap::get().load(*settings_.getUserSettings());  // the tests keep the default keys
  if (realOutput_) lang::use(settings_.getUserSettings()->getValue("language", "en"));  // Settings > General > Language
  if (realOutput_)  // the chosen output, unless a test asked for a backend
    options.device = settings_.getUserSettings()->getValue("audioDevice").toStdString();
  plugins_ = std::make_unique<PluginCatalog>(juce::File(juce::String(options.engineBinary.wstring().c_str())));
  session_ = std::make_unique<EngineSession>(std::move(options), history_.current());
  globalKeys_ = std::make_unique<GlobalKeys>([this] { playStop(); });  // also in the plugin windows and docked panels
  cancelExport_.setButtonText(u8("Cancel export"));
  cancelExport_.setTooltip(u8("Stop the export now (the file is not written)"));
  cancelExport_.onClick = [this] { session_->cancelExport(); };
  cancelExport_.setWantsKeyboardFocus(false);
  addChildComponent(cancelExport_);
  addChildComponent(exportBar_);

  home_.setButtonText(u8("Home"));
  play_.setButtonText(u8("Play"));
  stop_.setButtonText(u8("Stop"));
  loop_.setButtonText("Loop");
  using Icon = std::pair<juce::TextButton*, const char*>;
  for (const Icon& i : std::initializer_list<Icon>{{&home_, "home"}, {&play_, "play"}, {&stop_, "stop"}, {&loop_, "loop"}})
    i.first->getProperties().set(theme::kIconProperty, juce::String(i.second));  // drawn as an icon; the text stays for screen readers
  styleToggle(play_, theme::kPrimary);  // lit while playing (light, as every chosen button)
  restart_.setButtonText(u8("Restart engine"));
  restart_.setTooltip(u8("Start the audio engine again (the song is sent to it again; nothing is lost)"));
  undo_.setButtonText("Undo");
  redo_.setButtonText("Redo");
  undo_.getProperties().set(theme::kIconProperty, "undo");  // small icons in the top bar, next to the menus
  redo_.getProperties().set(theme::kIconProperty, "redo");
  home_.setTooltip(u8("Stop and go back to the start (Home)"));
  play_.setTooltip(u8("Play from the current position (Space)"));
  stop_.setTooltip(u8("Stop and keep the position (Space)"));
  loop_.setTooltip(u8("Repeat the region marked on the ruler (L). To mark it: hold Ctrl and drag on the ruler. A plain click or drag on the ruler, or a click on an empty part of a track, moves the start line; drag an end of the region to change it. With the loop on, Play starts at the loop's start when the start line is outside it."));
  styleToggle(loop_, theme::kPrimary);
  loop_.setToggleState(history_.current().loop.enabled, juce::dontSendNotification);
  home_.onClick = [this] { session_->home(); };
  play_.onClick = [this] {
    if (patternPlay_) { stopPatternSlot(); return; }  // the Patterns window was playing: the song again
    snap_.playing ? session_->stop() : session_->play();
  };  // Play while stopped, Pause while playing
  stop_.onClick = [this] { if (patternPlay_) stopPatternSlot(); else session_->stop(); };
  loop_.onClick = [this] {
    const int64_t at = (int64_t)shownPosition() / kBar * kBar;
    edit([at](doc::Document& d) {
      if (d.loop.endTick <= d.loop.startTick) d.loop = {true, at, at + 4 * kBar, d.loop.extra};  // none yet: 4 bars from here
      else d.loop.enabled = !d.loop.enabled;
    }, "Loop");
  };
  restart_.onClick = [this] { session_->restart(); };
  undo_.onClick = [this] { undo(); };
  addTrack_.setButtonText("+ Track");
  addTrack_.setTooltip(u8("Add a track (Ctrl+T). Right-click a track: rename, duplicate, remove."));
  addTrack_.onClick = [this] { showAddTrackMenu(); };
  redo_.onClick = [this] { redo(); };
  addAndMakeVisible(menuBar_);
  for (auto* b : {&home_, &play_, &stop_, &loop_, &restart_, &undo_, &redo_, &addTrack_}) {
    b->setWantsKeyboardFocus(false);
    addAndMakeVisible(b);
  }
  tracksView_ = std::make_unique<TracksView>(*this);
  timeBar_ = std::make_unique<TimeBar>(*this);
  addChildComponent(*timeBar_);
  tracksView_->setViewedComponent(&tracksHolder_, false);
  addAndMakeVisible(*tracksView_);
  tracksHolder_.addAndMakeVisible(addTrack_);  // under the last track, scrolling with them
  tracksHolder_.addMouseListener(this, false);  // the empty space under the last track: the selection box
  boxOverlay_ = std::make_unique<BoxOverlay>(*this);
  addAndMakeVisible(*boxOverlay_);  // over the tracks (kept in front in resized)

  bpm_.setSliderStyle(juce::Slider::LinearBar);
  theme::markPlain(bpm_);  // the number big and centred, no fill behind it
  bpm_.setRange(40.0, 600.0, 0.1);
  bpm_.setValue(history_.current().bpm, juce::dontSendNotification);
  bpm_.setTextValueSuffix(" BPM");
  bpm_.setTooltip(u8("Tempo (40–600 BPM). Drag to change, double-click to type a value."));
  bpm_.onDragStart = [this] { tempoBase_ = history_.current(); beginGesture("Tempo"); };
  bpm_.onValueChange = [this] {
    const double bpm = std::round(bpm_.getValue() * 10.0) / 10.0;
    // The bass keeps its gates in ms (doc::setTempo). During a drag every step starts from the song as it
    // was when the drag began, so the lengths do not drift and the copies of the patterns do not pile up.
    const doc::Document* base = history_.inGesture() && tempoBase_ ? &*tempoBase_ : nullptr;
    edit([bpm, base](doc::Document& d) {
      if (base) d = *base;
      doc::setTempo(d, bpm, d.tempoMap);
    }, "Tempo");
  };
  bpm_.onDragEnd = [this] { endGesture(); tempoBase_.reset(); };
  addAndMakeVisible(bpm_);

  // Diogo 2026-11-07 ("Volume de escuta fixo -10 dB"): Core plays 10 dB under the song by default, fixed.
  listen_.setSliderStyle(juce::Slider::LinearBar);
  listen_.setRange(-60.0, 0.0, 1.0);
  listen_.setDoubleClickReturnValue(true, -10.0);
  listen_.textFromValueFunction = [](double v) { return lang::tr("Listen") + " " + juce::String((int)std::round(v)) + " dB"; };
  listen_.valueFromTextFunction = [](const juce::String& t) {
    return (double)t.retainCharacters("-0123456789.").getFloatValue();
  };
  listen_.setTooltip(u8("Listening level: how loud Core plays in your headphones or speakers, from -60 to 0 dB (-10 by default). It stays where you put it and never changes by itself. Exports are not changed. Drag to change, double-click to type, Alt+click: back to -10 dB."));
  listen_.setValue(juce::jlimit(-60.0, 0.0, (double)settings_.getUserSettings()->getDoubleValue("listenDb", -10.0)), juce::dontSendNotification);
  session_->setListenLevel(listen_.getValue());
  listen_.onValueChange = [this] {
    session_->setListenLevel(listen_.getValue());
    settings_.getUserSettings()->setValue("listenDb", listen_.getValue());
  };
  listen_.onDragEnd = [this] { settings_.saveIfNeeded(); };
  addAndMakeVisible(listen_);
  using Kind = WindowButton::Kind;
  minimise_ = std::make_unique<WindowButton>(Kind::minimise);
  maximise_ = std::make_unique<WindowButton>(Kind::maximise);
  close_ = std::make_unique<WindowButton>(Kind::close);
  for (auto* b : {minimise_.get(), maximise_.get(), close_.get()}) addAndMakeVisible(b);
  build_.setColour(juce::Label::textColourId, kMuted);
  build_.setFont(juce::FontOptions(theme::kFontSmall));
  build_.setJustificationType(juce::Justification::centredRight);
  addAndMakeVisible(build_);

  for (auto* l : {&bpmLabel_, &position_, &status_, &message_}) {
    l->setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(l);
  }
  bpmLabel_.setText("Tempo", juce::dontSendNotification);
  bpmLabel_.setFont(juce::FontOptions(theme::kFontBody));
  bpmLabel_.setBorderSize({0, 0, 0, 6});  // the word right next to its field, nothing cut
  position_.setFont(juce::FontOptions(theme::kFontHeading));  // the same font as the rest (Diogo 2026-11-09: no other font in the time)
  bpmLabel_.setColour(juce::Label::textColourId, kMuted);
  bpmLabel_.setJustificationType(juce::Justification::centredRight);
  position_.setJustificationType(juce::Justification::centred);
  status_.setJustificationType(juce::Justification::centred);
  status_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  message_.setColour(juce::Label::textColourId, kMuted);
  message_.setFont(juce::FontOptions(theme::kFontBody));
  revertAccepted_.setButtonText(u8("Revert to accepted"));
  revertAccepted_.setTooltip(u8("Restore the last document accepted by the engine. Undo keeps your current edits."));
  revertAccepted_.setWantsKeyboardFocus(false);
  revertAccepted_.onClick = [this] {
    if (const auto accepted = session_->acceptedDocument())
      edit([&](doc::Document& d) { d = *accepted; }, u8("Revert to accepted document"));
  };
  addChildComponent(revertAccepted_);

  addAndMakeVisible(editor_);
  audioEditor_ = std::make_unique<AudioEditor>(*this);
  addChildComponent(*audioEditor_);
  addChildComponent(score_);
  score_.onClose = [this] { closePanels(); };
  score_.onSay = [this](const juce::String& text) { notice_ = text; logLine(text); repaint(); };
  score_.onOpenTrack = [this](const std::string& id) { selectTrack(id); };
  automation_ = std::make_unique<AutomationEditor>(*this);
  addChildComponent(*automation_);
  mixer_ = std::make_unique<MixerPanel>(static_cast<PatternEditor::Host&>(*this));
  mixer_->onSelect = [this](const std::string& trackId) {
    if (!trackId.empty()) selectTrack(trackId);
    mixer_->select(trackId);
  };
  mixer_->onClose = [this] { setMixerOpen(false); };
  addChildComponent(*mixer_);
  arrangeButton_.setButtonText("ARRANGE");
  arrangeButton_.setTooltip(u8("The arrangement and the piano roll (every page closed)"));
  arrangeButton_.setWantsKeyboardFocus(false);
  arrangeButton_.onClick = [this] { if (mixerOpen_) setMixerOpen(false); };
  addAndMakeVisible(arrangeButton_);
  {
    struct Item { const char* icon; const char* tip; std::function<void()> click; };
    const std::vector<Item> items = {
        {"arrange", "Arrangement", [this] { arrangeButton_.onClick(); }},
        {"mixer", "Mixer (Ctrl+M)", [this] { setMixerOpen(!mixerOpen_); }},
        // The Patterns window (channel rack, templates, swing, Send to Arrange): Diogo 2026-11-16, "facilidade em criar".
        {"patterns", "Patterns: the channel rack and the templates (Ctrl+P)", [this] { openPatterns(); }},
        {"library", "Library", [this] { toggleSidePanel(true); }},
        {"plugins", "Tools and plugins: Core's tools as cards, and the installed plugins by maker", [this] { togglePlugins(); }},
        {"inspector", "Inspector", [this] { toggleSidePanel(false); }}};
    for (const auto& item : items) {
      auto b = std::make_unique<juce::TextButton>(item.tip);
      b->getProperties().set(theme::kIconProperty, juce::String(item.icon));
      b->setTooltip(u8(item.tip));
      b->setWantsKeyboardFocus(false);
      b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
      b->setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
      b->setColour(juce::TextButton::textColourOffId, theme::kMuted);
      b->onClick = item.click;
      addAndMakeVisible(*b);
      rail_.push_back(std::move(b));
    }
  }
  addChildComponent(pluginCards_);
  pluginCards_.onOpen = [this](const doc::CorePluginInfo& p, juce::Component& card) { openCorePlugin(p, &card); };
  pluginCards_.onGrey = [this](const juce::String& missing) { notice_ = u8("Settings > Roles: choose the plugin of ") + missing; repaint(); };
  pluginCards_.onVendor = [this](const doc::PluginVendorGroup& g, juce::Component& card) { showVendorPlugins(g, &card); };
  mixerButton_.setButtonText("MIX");
  mixerButton_.setTooltip(u8("The mixer in the bottom panel (Ctrl+M)"));
  mixerButton_.setClickingTogglesState(false);
  mixerButton_.setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
  mixerButton_.setWantsKeyboardFocus(false);
  mixerButton_.onClick = [this] { setMixerOpen(!mixerOpen_); };
  addAndMakeVisible(mixerButton_);
  for (auto* tab : {&arrangeButton_, &mixerButton_}) {  // tabs: text only, the chosen one on a grey-blue pill
    tab->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    tab->setColour(juce::TextButton::buttonOnColourId, theme::kPrimary);
    tab->setColour(juce::TextButton::textColourOffId, theme::kMuted);
  }
  // The side panels: the Library on the left, then the Inspector beside the tracks (Diogo 2026-11-07).
  inspector_ = std::make_unique<InspectorPanel>(static_cast<PatternEditor::Host&>(*this), editor_);
  editor_.onPreviewNotes = [this](const std::string& trackId, const std::vector<doc::Note>& notes) { previewNotes(trackId, notes); };
  editor_.onMessage = [this](const juce::String& text) { notice_ = text; logLine(text); repaint(); };
  // The Sound card's «Open», or a right-click on one of its bars: the sound and its modulation on one page.
  editor_.onSavePattern = [this](int64_t start, int64_t end) { savePatternFromEditor(start, end); };
  editor_.onBuildupTrack = [this](int bars, bool roll, bool gap) { buildupTrack(bars, roll, gap); };
  editor_.bpm = [this] { return doc::bpmAt(history_.current(), (int64_t)shownPosition()); };
  editor_.patternLibrary = [this] { return loadPatterns(); };
  editor_.pcoreCatalog = [this]() -> const doc::PCoreCatalog* {
    // pcore/styles.json beside Core.exe, read once (a broken file is logged and P-Core says it is missing).
    if (!pcoreTried_) {
      pcoreTried_ = true;
      const juce::File f = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("pcore").getChildFile("styles.json");
      try { pcoreCatalog_ = doc::loadPCoreCatalog(pathOf(f)); } catch (const std::exception& e) { logLine(u8("P-Core catalogue not read: ") + u8(e.what())); }
    }
    return pcoreCatalog_ ? &*pcoreCatalog_ : nullptr;
  };
  editor_.songStyle = [this] { return doc::songStyle(history_.current()); };
  editor_.onOpenPatterns = [this] { openPatterns(); };
  addAndMakeVisible(*inspector_);
  setupLibrary();
  library_.onMinimise = [this] {
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)] { if (safe && safe->showLibrary_) safe->toggleSidePanel(true); });
  };
  addAndMakeVisible(library_);
  libraryWidth_ = settings_.getUserSettings()->getIntValue("libraryWidth", 0);
  showLibrary_ = settings_.getUserSettings()->getBoolValue("showLibrary", true);
  showInspector_ = settings_.getUserSettings()->getBoolValue("showInspector", true);
  inspector_->setFolded(settings_.getUserSettings()->getBoolValue("inspectorFolded", false));
  inspector_->onFold = [this](bool folded) {
    settings_.getUserSettings()->setValue("inspectorFolded", folded);
    resized();
    repaint();
  };
  applyDrawingHelp();
  sectionShades_ = settings_.getUserSettings()->getBoolValue("sectionShades", true);
  project_.setTooltip(u8("This song: open, save, export (the File menu)"));
  project_.setWantsKeyboardFocus(false);
  project_.onClick = [this] { showProjectMenu(); };
  addAndMakeVisible(project_);
  aiBadge_ = std::make_unique<AiBadge>();
  aiBadge_->onPress = [this](bool popup) { aiBadgePressed(popup); };
  addAndMakeVisible(*aiBadge_);
  setWantsKeyboardFocus(true);
  if (realOutput_) {  // the user's MIDI keyboard, if it is plugged in
    editorHeight_ = settings_.getUserSettings()->getIntValue("editorHeight", 0);
    mixerHeight_ = settings_.getUserSettings()->getIntValue("mixerHeight", 0);
    const juce::String saved = settings_.getUserSettings()->getValue("midiInput");
    for (const auto& input : juce::MidiInput::getAvailableDevices())
      if (input.identifier == saved) openMidiInput(saved);
  }
  buildRows();
  if (realOutput_) setAiControl(settings_.getUserSettings()->getBoolValue("aiControl", true));  // not in the tests (null device)
  else aiAllowed_ = false;
  updateAiBadge();
  if (realOutput_) {  // the sample library: what the last scan found now, new or changed files in a moment
    loadSampleIndex();
    if (sampleLibraryFolder().isDirectory()) scanSampleLibrary();
  }
  startTimerHz(30);
  // Once the window is up: work an earlier session left unsaved (a crash) is offered back.
  juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)] { if (safe) safe->offerRecovery(); });
}

MainComponent::~MainComponent() {
  globalKeys_.reset();  // no more keys into a component being destroyed
  patternsWindow_.reset();  // its buttons call this window
  if (settingsWindow_ != nullptr) delete settingsWindow_.getComponent();  // it shows this window's settings and jobs
  if (sampleScan_) sampleScan_->stop = true;  // its thread owns what it uses and ends at the next file
  if (analysisJob_) analysisJob_->stop = true;  // the same
  for (int i = 0; i < 300 && autosaveJob_ && autosaveJob_->state.load() == 0; ++i) juce::Thread::sleep(10);  // a copy being written finishes first
  doc::removeAutosave(doc::autosavePath(pathOf(autosaveFolder()), autosaveSession_));  // a clean end: nothing to recover
  stopTimer();
  if (midiInput_) midiInput_->stop();  // no more callbacks into a component being destroyed
  midiInput_.reset();
  session_.reset();  // closes the engine
}

// ---------------------------------------------------------------- editing

void MainComponent::edit(const std::function<void(doc::Document&)>& change, const juce::String& label) {
  doc::Document next = history_.current();
  try {
    change(next);
    doc::refreshFollows(next);   // FOLLOW on: the kick plays the STUTTER, the bass follows the kick (same undo step)
    // Refuse an invalid edit before putting it into History or sending it: otherwise every
    // later edit is built on a document the engine cannot play (report parts 12–14).
    (void)core::fromJson(doc::toProjection(next));
  } catch (const std::exception& e) {  // e.g. the track limit: nothing changes
    showError(u8("Could not do that"), u8(e.what()));
    return;
  }
  const doc::Loop before = history_.current().loop;
  if (history_.inGesture()) history_.updateGesture(next);
  else history_.commit(next, label.toStdString());
  // During a drag the engine gets the song at most once per frame (the timer), not on every mouse move:
  // turning the whole song into the engine's form is the slow part of an edit on a long song.
  if (patternPlay_) patternPlay_->preview.reset();  // an edit ends a template's preview: the song plays its own notes
  if (history_.inGesture()) sendPending_ = true;
  else { sendPending_ = false; session_->setDocument(engineDocument()); }  // intermediate values coalesce; the last one is always sent
  const doc::Loop& after = history_.current().loop;
  if (!patternPlay_ && (after.enabled != before.enabled || after.startTick != before.startTick || after.endTick != before.endTick))
    session_->setLoop(after);
  bool sameTracks = rows_.size() == history_.current().tracks.size();
  for (size_t i = 0; sameTracks && i < rows_.size(); ++i) sameTracks = rows_[i]->id() == history_.current().tracks[i].id;
  if (!sameTracks) buildRows();
  else for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->update(history_.current().tracks[i]);
  loop_.setToggleState(after.enabled, juce::dontSendNotification);
  editor_.documentChanged();
  showEditorForSelection();
  repaint(timelineArea());
  if (patternsWindow_) patternsWindow_->panel().documentChanged();
}

void MainComponent::beginGesture(const juce::String& label) { history_.beginGesture(label.toStdString()); }
void MainComponent::endGesture() {
  history_.endGesture();
  if (sendPending_) { sendPending_ = false; session_->setDocument(engineDocument()); }
}

void MainComponent::documentChanged(bool rebuildRows) {
  const doc::Document& d = history_.current();
  // Another document (open, new) or another state (undo, redo): keep the clip selection only if that
  // clip still exists.
  bool clipExists = false;
  if (const doc::Track* t = d.findTrack(selectedTrack_))
  {
    for (const auto& c : t->clips) clipExists |= c.id == selectedClip_;
    for (const auto& c : t->audioClips) clipExists |= c.id == selectedClip_;
  }
  if (!clipExists) selectedClip_.clear();
  if (patternPlay_) patternPlay_->preview.reset();
  session_->setDocument(engineDocument());
  session_->setLoop(engineLoop());
  if (patternsWindow_) patternsWindow_->panel().documentChanged();
  bpm_.setValue(d.bpm, juce::dontSendNotification);
  // A song that changes tempo: the field is its tempo at the start; the changes are on the ruler.
  bpmLabel_.setText(d.tempoMap.empty() ? "Tempo" : "Start", juce::dontSendNotification);
  {
    juce::String tip = u8("Tempo (40–600 BPM). Drag to change, double-click to type a value.");
    if (!d.tempoMap.empty()) {
      tip = u8("Tempo at the start (40–600 BPM). The song changes tempo at:");
      for (const auto& t : d.tempoMap)
        tip += "\n" + u8("bar ") + juce::String(t.tick / kBar + 1) + (t.tick % kBar ? u8(" (mid-bar)") : juce::String()) + ": " + juce::String(t.bpm, 1) + " BPM";
    }
    bpm_.setTooltip(tip);
  }
  loop_.setToggleState(d.loop.enabled, juce::dontSendNotification);
  bool sameTracks = rows_.size() == d.tracks.size();
  for (size_t i = 0; sameTracks && i < rows_.size(); ++i) sameTracks = rows_[i]->id() == d.tracks[i].id;
  if (rebuildRows || !sameTracks) buildRows();
  else for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->update(d.tracks[i]);
  splitRows_ = std::max(splitRows_, (int)rows_.size());
  editor_.documentChanged();
  if (editor_.track().empty() && !d.tracks.empty() && !d.findTrack(selectedTrack_)) selectTrack(d.tracks.front().id);
  showEditorForSelection();
  repaint();
}

void MainComponent::openScore() {
  automationOpen_ = false;
  scoreOpen_ = true;
  showEditorForSelection();
}

void MainComponent::openCoreWindow(const std::string& what, const std::string& track) {
  const doc::Track* t = track.empty() ? nullptr : history_.current().findTrack(track);
  if (!track.empty() && !t) return;
  if (what == "score") { openScore(); return; }
  if (what == "mixer") {
    if (!track.empty()) selectTrack(track);
    setMixerOpen(true);
    return;
  }
  if (what == "patterns" && !t) { openPatterns(); return; }  // a window of its own: no track needed
  if (what == "instrument" && !t) {  // a track's window, and no track chosen
    notice_ = u8("Choose a track first (click its name), then open it again.");
    repaint();
    return;
  }
  if (what == "patterns") {  // P-Core: the Patterns window, its own window, that track chosen there
    selectTrack(track);
    openPatterns();
    if (patternsWindow_ && t && doc::playsPatterns(*t)) patternsWindow_->panel().select(track);
    return;
  }
  selectTrack(track);
  if (what == "instrument" && t->instrument.kind == "plugin") session_->openPluginEditor(doc::pluginKey(track));
}

void MainComponent::setMixerOpen(bool open) {
  mixerOpen_ = open;
  if (open) {
    automationOpen_ = scoreOpen_ = false;
    mixer_->select(selectedTrack_.empty() ? editor_.track() : selectedTrack_);
  }
  mixerButton_.setToggleState(open, juce::dontSendNotification);
  showEditorForSelection();
  resized();  // the Mixer takes the whole width (ref-3): the Library and the Inspector step aside
}

doc::Document MainComponent::engineDocument() const {
  if (patternPlay_) return doc::rackPlayDocument(patternPlay_->preview ? *patternPlay_->preview : history_.current(), patternPlay_->slot, patternPlay_->openTick);
  return history_.current();
}

doc::Loop MainComponent::engineLoop() const { return patternPlay_ ? engineDocument().loop : history_.current().loop; }

void MainComponent::playPatternSlot(const std::string& slot, std::optional<doc::Document> preview, int64_t openTick) {
  const bool already = patternPlay_.has_value();
  if (!already) patternSongTick_ = (int64_t)shownPosition();
  patternPlay_ = PatternPlay{slot, std::move(preview), openTick};
  session_->setDocument(engineDocument());
  session_->setLoop(engineLoop());
  if (!already || !snap_.playing) {
    session_->seek(0);
    session_->play();
  }
}

void MainComponent::stopPatternSlot() {
  if (!patternPlay_) return;
  patternPlay_.reset();
  session_->stop();
  session_->setDocument(engineDocument());
  session_->setLoop(history_.current().loop);
  session_->seek(patternSongTick_);
}

// ---------------------------------------------------------------- Core's tools (cards, menus; the AI calls the same)

std::string MainComponent::toolTrack() const { return selectedTrack_.empty() ? editor_.track() : selectedTrack_; }

void MainComponent::runTool(const juce::String& label, std::function<std::string(doc::Document&, const doc::RoleMap&)> tool) {
  std::string problem;
  const doc::RoleMap roles = PluginCatalog::roles(&problem);
  if (!problem.empty()) { showError(label, u8("The role map could not be read (") + u8(problem) + u8("): Settings > Roles.")); return; }
  std::string sentence;
  bool done = false;
  edit([&](doc::Document& d) { sentence = tool(d, roles); done = true; }, label);  // a refusal shows its reason; nothing changes
  if (!done) return;
  notice_ = u8(sentence) + u8(" (Ctrl+Z puts it back)");
  repaint();
}

namespace {
std::string withSkipped(const doc::RoleToolResult& r) {
  std::string s = r.sentence;
  for (const auto& k : r.skipped) s += ". Skipped: " + k;
  return s;
}
std::string capital(std::string s) {
  if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
  return s;
}
}  // namespace

juce::PopupMenu MainComponent::coreToolMenu(const std::string& toolId, const std::string& trackIdIn) {
  juce::PopupMenu m;
  juce::Component::SafePointer<MainComponent> safe(this);
  const doc::Document& song = history_.current();
  const std::string trackId = trackIdIn.empty() ? toolTrack() : trackIdIn;
  const doc::Track* t = song.findTrack(trackId);
  const bool notes = t && doc::playsPatterns(*t);
  using Act = std::function<void(const doc::SongSpan&)>;
  // Where it works: each section by name, and the whole song when it fits.
  auto where = [&song](const Act& act, bool whole) {
    juce::PopupMenu s;
    for (const auto& sec : song.sections) {
      const doc::SongSpan span{sec.startTick, std::min(song.lengthTicks, sec.startTick + sec.lengthTicks), sec.name};
      if (span.end > span.start) s.addItem(juce::PopupMenu::Item(u8(sec.name)).setAction([act, span] { act(span); }));
    }
    if (whole) s.addItem(juce::PopupMenu::Item(u8("The whole song")).setAction([act, len = song.lengthTicks] { act({0, len, "the whole song"}); }));
    if (s.getNumItems() == 0) s.addItem(juce::PopupMenu::Item(u8("No sections yet: right-click the timeline to add one")).setEnabled(false));
    return s;
  };
  auto needTrack = [&m, notes] {
    if (!notes) m.addItem(juce::PopupMenu::Item(u8("Choose a track with notes first (click its name)")).setEnabled(false));
    return notes;
  };
  const juce::String on = t ? u8(" on ") + u8(t->name) : juce::String();
  std::string rolesProblem;
  const doc::RoleMap roleMap = PluginCatalog::roles(&rolesProblem);
  if (toolId == "pcore") {
    m.addItem(juce::PopupMenu::Item(u8("Open the Patterns window…")).setAction([safe] { if (safe) safe->openPatterns(); }));
  } else if (toolId == "wcore") {
    for (const char* role : {"bass", "lead", "pad", "pluck", "drums"}) {
      const juce::String label = u8(doc::findRole(role) ? doc::findRole(role)->label : role);
      if (notes)
        m.addItem(juce::PopupMenu::Item(label + on).setEnabled(roleMap.find(role) != nullptr).setAction([safe, trackId, role = std::string(role), label] {
          if (!safe) return;
          safe->runTool(u8("Instrument: ") + label, [&](doc::Document& d, const doc::RoleMap& r) {
            doc::setTrackRole(d, trackId, role, r);
            return doc::trackById(d, trackId).name + " plays the " + role + " plugin";
          });
          safe->session_->openPluginEditor(doc::pluginKey(trackId));
        }));
    }
    juce::PopupMenu fresh;
    for (const char* role : {"bass", "lead", "pad", "pluck", "drums"}) {
      const juce::String label = u8(doc::findRole(role) ? doc::findRole(role)->label : role);
      // A role with no plugin is a grey item, not an error dialog after the click (round 3, R3-B / C3).
      fresh.addItem(juce::PopupMenu::Item(label).setEnabled(roleMap.find(role) != nullptr).setAction([safe, role = std::string(role), label] {
        if (!safe) return;
        safe->runTool(u8("New track: ") + label, [&](doc::Document& d, const doc::RoleMap& r) {
          doc::addRoleTrack(d, role, r);
          return "A new " + role + " track, played by the role's plugin";
        });
      }));
    }
    m.addSubMenu(u8("A new track"), fresh);
  } else if (toolId == "fcore") {
    if (needTrack())
      for (const auto& intent : doc::fxIntents()) {
        juce::PopupMenu sub = where([safe, trackId, intent](const doc::SongSpan& span) {
          if (!safe) return;
          safe->runTool(u8("F-Core: ") + u8(intent), [&](doc::Document& d, const doc::RoleMap& r) {
            return withSkipped(doc::toolIntentFx(d, trackId, r, intent, span.start, span.end, 0.7));
          });
        }, true);
        // Or a zone of their own: first and last bar, this track / the selected ones / all (Diogo 2026-11-17).
        sub.addSeparator();
        sub.addItem(juce::PopupMenu::Item(u8("Choose bars and tracks…")).setAction([safe, trackId, intent] {
          if (safe) safe->askIntentZone(intent, trackId);
        }));
        m.addSubMenu(u8(capital(intent)) + on, sub);
      }
  } else if (toolId == "rcore" || toolId == "dcore") {
    const std::string kind = toolId == "rcore" ? "reverb" : "delay";
    std::vector<std::string> tracks = selectedTracks();
    if (tracks.empty() && notes) tracks.push_back(trackId);
    if (tracks.empty()) m.addItem(juce::PopupMenu::Item(u8("Select the tracks that share it first")).setEnabled(false));
    else
      for (const auto& p : doc::spacePresets()) {
        if (p.kind != kind) continue;
        m.addItem(juce::PopupMenu::Item(u8(capital(p.id)) + u8(": ") + u8(p.what) + u8(" (") + juce::String((int)tracks.size()) + u8(" tracks)"))
                      .setAction([safe, tracks, kind, id = p.id] {
                        if (!safe) return;
                        safe->runTool(u8(kind == "reverb" ? "R-Core " : "D-Core ") + u8(id), [&](doc::Document& d, const doc::RoleMap& r) {
                          return doc::toolSharedBus(d, tracks, r, kind, id).sentence;
                        });
                      }));
      }
  } else if (toolId == "suck" || toolId == "buildup") {
    if (needTrack())
      m.addSubMenu(u8(toolId == "suck" ? "Into the drop after" : "Over the build-up") + on, where([safe, trackId, toolId](const doc::SongSpan& span) {
        if (!safe) return;
        safe->runTool(u8(toolId == "suck" ? "SUCK" : "Build-up"), [&](doc::Document& d, const doc::RoleMap& r) {
          return withSkipped(doc::toolIntentFx(d, trackId, r, toolId, span.start, span.end, 0.8));
        });
      }, false));
  } else if (toolId == "roll") {
    if (needTrack())
      for (const char* style : {"steps", "gradual"})
        m.addSubMenu(u8(style == std::string("steps") ? "Steps (1/4, 1/8, 1/16, 1/32)" : "Gradual (speeding up all the way)") + on,
                     where([safe, trackId, style = std::string(style)](const doc::SongSpan& span) {
                       if (!safe) return;
                       safe->runTool(u8("ROLL"), [&](doc::Document& d, const doc::RoleMap&) {
                         return doc::toolRoll(d, trackId, span.start, span.end, style).sentence;
                       });
                     }, false));
  } else if (toolId == "pum") {
    if (needTrack())
      for (const auto& src : song.tracks) {
        if (src.id == trackId || !doc::playsPatterns(src)) continue;
        m.addSubMenu(u8("From ") + u8(src.name) + on, where([safe, trackId, from = src.id](const doc::SongSpan& span) {
          if (!safe) return;
          safe->runTool(u8("PUM"), [&](doc::Document& d, const doc::RoleMap& r) {
            return withSkipped(doc::toolPump(d, trackId, from, r, span.start, span.end));
          });
        }, true));
      }
  }
  return m;
}

juce::PopupMenu MainComponent::coreToolsMenu(const std::string& trackId) {
  juce::PopupMenu m;
  for (const auto& p : doc::corePlugins()) {
    juce::PopupMenu one = coreToolMenu(p.id, trackId);
    if (p.id == "pcore") { m.addItem(juce::PopupMenu::Item(u8("P-Core: the Patterns window…")).setAction([this] { openPatterns(); })); continue; }
    m.addSubMenu(u8(p.shortName) + u8(" · ") + u8(p.word), one);
  }
  return m;
}

void MainComponent::openCorePlugin(const doc::CorePluginInfo& info, juce::Component* near) {
  if (info.opens == "window") { openPatterns(); return; }
  juce::PopupMenu m = coreToolMenu(info.id, "");
  auto options = juce::PopupMenu::Options();
  if (near) options = options.withTargetComponent(near);
  m.showMenuAsync(options);
}

// A maker's card in the Tools panel's PLUGINS tab: its plugins, each with what it can do here (Diogo 2026-11-17:
// "ao carregar num deles assim aparecer as opçoes que cada tem"). Instruments: a new track, or replace the selected
// track's; effects: add to the selected track's chain. The plugin's window opens right after, as everywhere else.
void MainComponent::showVendorPlugins(const doc::PluginVendorGroup& group, juce::Component* near) {
  juce::PopupMenu m;
  juce::Component::SafePointer<MainComponent> safe(this);
  const std::string trackId = toolTrack();
  const doc::Track* t = history_.current().findTrack(trackId);
  const juce::String on = t ? u8(" on ") + u8(t->name) : juce::String();
  for (const auto& p : group.plugins) {
    const juce::String name = u8(p.value("name", std::string("Plugin")));
    const bool instrument = p.value("instrument", false);
    juce::PopupMenu one;
    if (instrument) {
      one.addItem(juce::PopupMenu::Item(u8("New track with it")).setAction([safe, p] {
        if (!safe) return;
        std::string id;
        safe->edit([&](doc::Document& d) { id = doc::addPluginTrack(d, p); }, u8("Add ") + u8(p.value("name", std::string("plugin"))));
        if (!id.empty()) { safe->selectTrack(id); safe->session_->openPluginEditor(doc::pluginKey(id)); }
      }));
      if (t && doc::playsPatterns(*t))
        one.addItem(juce::PopupMenu::Item(u8("Replace the instrument") + on).setAction([safe, p, trackId] {
          if (!safe) return;
          safe->fetchPluginStates();
          safe->edit([&](doc::Document& d) { doc::replaceInstrumentWithPlugin(d, trackId, p); },
                     u8("Instrument: ") + u8(p.value("name", std::string("plugin"))));
          safe->session_->openPluginEditor(doc::pluginKey(trackId));
        }));
    } else if (t) {
      one.addItem(juce::PopupMenu::Item(u8("Add as effect") + on).setAction([safe, p, trackId] {
        if (!safe) return;
        int at = -1;
        safe->edit([&](doc::Document& d) { at = doc::addPluginEffect(d, trackId, p); }, u8("Add ") + u8(p.value("name", std::string("plugin"))));
        if (const doc::Track* now = safe->history_.current().findTrack(trackId); now && at >= 0 && at < (int)now->effects.size())
          safe->session_->openPluginEditor(doc::pluginKey(trackId, now->effects[(size_t)at].value("id", std::string())));
      }));
    }
    if (one.getNumItems() == 0)
      one.addItem(juce::PopupMenu::Item(u8(instrument ? "Click a track with notes first" : "Click a track first")).setEnabled(false));
    m.addSubMenu(name, one);
  }
  if (m.getNumItems() == 0) m.addItem(juce::PopupMenu::Item(u8("No plugins of this maker")).setEnabled(false));
  auto options = juce::PopupMenu::Options();
  if (near) options = options.withTargetComponent(near);
  m.showMenuAsync(options);
}

// F-Core over a chosen zone (Diogo 2026-11-17: "ao selecionar a zona aplicava nas pistas que queria ou em todas e
// ainda tinha opçao de escolher os numero do bar inicial e final"): first and last bar, the tracks, and the amount.
// The write itself is toolIntentFxTracks, through runTool: one undo step, a refusal changes nothing.
void MainComponent::askIntentZone(const std::string& intent, const std::string& trackId) {
  const doc::Document& song = history_.current();
  const int64_t kBar = 4 * doc::kPpq;
  const int totalBars = (int)std::max<int64_t>(1, song.lengthTicks / kBar);
  const doc::Track* t = song.findTrack(trackId);
  const std::vector<std::string> selected = selectedTracks();
  auto* w = new juce::AlertWindow(u8("F-Core: ") + u8(intent), juce::String(), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("from", "1", lang::tr("First bar:"));
  w->addTextEditor("to", juce::String(totalBars), lang::tr("Last bar:"));
  juce::StringArray choices;
  choices.add(t ? u8("This track (") + u8(t->name) + u8(")") : u8("This track"));
  choices.add(u8("Selected tracks (") + juce::String((int)selected.size()) + u8(")"));
  choices.add(u8("All tracks with notes"));
  w->addComboBox("tracks", choices, lang::tr("Apply to:"));
  w->addTextBlock(lang::tr("Amount (0 to 1):"));
  auto* amount = new juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft);
  amount->setRange(0.0, 1.0, 0.05);
  amount->setValue(0.7, juce::dontSendNotification);
  w->addCustomComponent(amount);  // the window owns it; it lives as long as the safe pointer below
  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> safe(w);
  w->enterModalState(true, juce::ModalCallbackFunction::create([this, safe, intent, trackId, selected, kBar, totalBars, amount](int result) {
    if (result != 1 || safe == nullptr) return;
    const int from = safe->getTextEditorContents("from").getIntValue();
    const int to = safe->getTextEditorContents("to").getIntValue();
    if (from < 1 || to < from || to > totalBars) {
      showError(u8("F-Core: ") + u8(intent),
                u8("Choose bars between 1 and ") + juce::String(totalBars) + u8(", the first no later than the last."));
      return;
    }
    const int64_t start = (int64_t)(from - 1) * kBar, end = std::min<int64_t>((int64_t)to * kBar, history_.current().lengthTicks);
    std::vector<std::string> tracks;
    const int choice = safe->getComboBoxComponent("tracks")->getSelectedItemIndex();
    if (choice == 1) tracks = selected;
    else if (choice == 2) { for (const auto& tr : history_.current().tracks) if (doc::playsPatterns(tr)) tracks.push_back(tr.id); }
    else tracks.push_back(trackId);
    const double strength = amount->getValue();
    runTool(u8("F-Core: ") + u8(intent), [&](doc::Document& d, const doc::RoleMap& r) {
      return withSkipped(doc::toolIntentFxTracks(d, tracks, r, intent, start, end, strength));
    });
  }), true);
}

void MainComponent::giveSilentTracksPlugins() {
  std::string problem;
  const doc::RoleMap roles = PluginCatalog::roles(&problem);
  if (roles.roles.empty()) {
    showError(lang::tr("No plugins for the roles yet"),
              lang::tr("Settings > Roles says which plugin plays each part (Drums: sfizz, Bass / Lead / Pad: Vital...). Choose them there, then try again.") +
                  (problem.empty() ? juce::String() : u8("\n\n") + u8(problem)));
    return;
  }
  int changed = 0;
  std::vector<std::string> left;
  try {
    edit([&](doc::Document& d) { changed = doc::giveSilentTracksRolePlugins(d, roles, &left); }, u8("Plugins for the silent tracks"));
  } catch (const std::exception& e) {
    showError(u8("Plugins for the silent tracks"), u8(e.what()));
    return;
  }
  // Each piece goes through tr on its own: the whole text is two sentences and no single entry matches it (R3-C).
  juce::String text = lang::tr(juce::String(changed) + u8(" tracks play their role's plugin now (Ctrl+Z puts them back)."));
  if (!left.empty()) {
    juce::String names;
    for (const auto& n : left) names += (names.isEmpty() ? juce::String() : juce::String(", ")) + u8(n);
    text += " " + lang::tr(u8("Still silent (no role guessed, or no plugin for it in Settings > Roles): ") + names + ".");
  }
  notice_ = text;
  repaint();
}

void MainComponent::openPatterns() {
  if (!patternsWindow_) {
    PatternsPanel::Host h;
    h.document = [this]() -> const doc::Document& { return history_.current(); };
    h.edit = [this](const std::function<void(doc::Document&)>& f, const juce::String& label) { edit(f, label); };
    h.startGesture = [this](const juce::String& label) { beginGesture(label); };
    h.finishGesture = [this] { endGesture(); };
    h.undo = [this] { undo(); };
    h.redo = [this] { redo(); };
    h.songTick = [this] { return patternPlay_ ? (double)patternSongTick_ : shownPosition(); };
    h.playSlot = [this](const std::string& slot, std::optional<doc::Document> preview, int64_t openTick) { playPatternSlot(slot, std::move(preview), openTick); };
    h.stopSlot = [this] { stopPatternSlot(); };
    h.patternTick = [this] { return patternPlay_ && snap_.playing ? playTick_ : -1.0; };
    h.beginTempo = [this] { tempoBase_ = history_.current(); beginGesture("Tempo"); };
    h.endTempo = [this] { endGesture(); tempoBase_.reset(); };
    h.setTempo = [this](double bpm) {
      const doc::Document* base = history_.inGesture() && tempoBase_ ? &*tempoBase_ : nullptr;
      edit([bpm, base](doc::Document& d) {
        if (base) d = *base;
        doc::setTempo(d, bpm, d.tempoMap);
      }, "Tempo");
    };
    h.openPianoRoll = [this](const std::string& trackId, const std::string& patternId) {
      selectTrack(trackId);
      editor_.setTrack(trackId, patternId);
      if (auto* top = getTopLevelComponent()) top->toFront(true);
    };
    h.openEffects = [this](const std::string& trackId) {
      if (auto* top = getTopLevelComponent()) top->toFront(true);
      showTrackMenu(trackId);
    };
    h.addTrack = [this] {
      if (auto* top = getTopLevelComponent()) top->toFront(true);
      showAddTrackMenu();
    };
    h.level = [this](const std::string& trackId) {
      const auto it = meters_.find(trackId);
      return it == meters_.end() ? 0.0f : std::max(it->second.first, it->second.second);
    };
    h.colourOf = [](const doc::Track& t) { return trackColour(t); };
    h.catalog = [this] { return editor_.pcoreCatalog ? editor_.pcoreCatalog() : nullptr; };
    h.songStyle = [this] { return doc::songStyle(history_.current()); };
    h.templatesFolder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("PatternTemplates");
    patternsWindow_ = std::make_unique<PatternsWindow>(std::move(h));
  }
  patternsWindow_->show();
}

void MainComponent::closePanels() {
  scoreOpen_ = false;
  automationOpen_ = false;
  showEditorForSelection();
}

void MainComponent::openAutomation(const std::string& trackId, const std::string& param) {
  if (!trackId.empty()) selectTrack(trackId);
  scoreOpen_ = false;
  automationOpen_ = true;
  automation_->setTarget(trackId, param);
  showEditorForSelection();
  placeBottomPanels();
}

void MainComponent::closeAutomation() { closePanels(); }

void MainComponent::placeBottomPanels() {
  const auto area = editorArea();
  score_.setBounds(area);
  automation_->setBounds(area);
  editor_.setBounds(area);
  audioEditor_->setBounds(area);
}

void MainComponent::showEditorForSelection() {
  // The bottom panels' places follow what is open, on every way out.
  struct Place { MainComponent* m; ~Place() { m->placeBottomPanels(); } } place{this};
  // The Mixer, until it is closed (or a curve or Stage Core is opened).
  if (automationOpen_ || scoreOpen_ || !mixer_) mixerOpen_ = false;
  mixerButton_.setToggleState(mixerOpen_, juce::dontSendNotification);
  if (mixer_) mixer_->setVisible(mixerOpen_);
  if (library_.isVisible() != libraryShown()) resized();  // the Mixer closed by another view: the side panels come back
  if (mixerOpen_) {
    mixer_->documentChanged();
    for (juce::Component* c : std::initializer_list<juce::Component*>{automation_.get(), &score_, audioEditor_.get(), &editor_}) c->setVisible(false);
    return;
  }
  // Automation of a track or the master, until something else is chosen.
  if (automationOpen_ && !automation_->target().empty() && !history_.current().findTrack(automation_->target())) automationOpen_ = false;
  automation_->setVisible(automationOpen_);
  if (automationOpen_) {
    automation_->documentChanged();
    score_.setVisible(false);
    audioEditor_->setVisible(false);
    editor_.setVisible(false);
    return;
  }
  // Stage Core (the song's parts), until it is closed or something else is chosen.
  score_.setVisible(scoreOpen_);
  if (scoreOpen_) {
    score_.documentChanged();
    audioEditor_->setVisible(false);
    editor_.setVisible(false);
    return;
  }
  const doc::Track* t = history_.current().findTrack(selectedTrack_);
  bool audioClip = false;
  if (t && t->instrument.kind == "audio") for (const auto& c : t->audioClips) audioClip |= c.id == selectedClip_;
  if (audioClip) audioEditor_->setClip(selectedTrack_, selectedClip_);
  else audioEditor_->setClip({}, {});
  audioEditor_->documentChanged();
  audioEditor_->setVisible(audioClip);
  editor_.setVisible(!audioClip);
}

void MainComponent::selectTrack(const std::string& trackId) {
  releaseAllKeys();  // a held key ends on the track that started it
  scoreOpen_ = false;
  automationOpen_ = false;
  boxClips_.clear();
  if (trackId != selectedTrack_) { selectedTrack_ = trackId; selectedClip_.clear(); }
  editor_.setTrack(trackId);
  if (mixer_ && mixerOpen_ && !trackId.empty()) mixer_->select(trackId);  // a click on a track row chooses its strip
  showEditorForSelection();
  { std::lock_guard<std::mutex> g(liveMutex_); liveTrack_ = editor_.track(); }
  for (auto& row : rows_) row->repaint();
}
std::vector<std::string> MainComponent::selectedTracks() const {
  const doc::Document& d = history_.current();
  const std::string shown = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
  std::vector<std::string> out;
  if (d.findTrack(shown)) out.push_back(shown);
  for (const auto& t : d.tracks)
    if (t.id != shown && alsoSelected_.count(t.id)) out.push_back(t.id);
  return out;
}

bool MainComponent::trackSelected(const std::string& trackId) const {
  return trackId == (selectedTrack_.empty() ? editor_.track() : selectedTrack_) || alsoSelected_.count(trackId) > 0;
}

void MainComponent::selectOnlyTrack(const std::string& trackId) {
  alsoSelected_.clear();
  selectTrack(trackId);
}

void MainComponent::toggleTrackSelected(const std::string& trackId) {
  const std::string shown = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
  if (shown.empty()) { selectTrack(trackId); return; }
  if (trackId == shown) {
    // Ctrl+click on the shown track: the next selected one becomes the shown one (the last one stays).
    if (alsoSelected_.empty()) return;
    const auto all = selectedTracks();
    const std::string next = all.size() > 1 ? all[1] : std::string();
    alsoSelected_.erase(next);
    selectTrack(next);
    return;
  }
  if (!alsoSelected_.erase(trackId)) alsoSelected_.insert(trackId);
  for (auto& row : rows_) row->repaint();
}

void MainComponent::selectTracksTo(const std::string& trackId) {
  const std::string shown = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
  const auto& tracks = history_.current().tracks;
  int a = -1, b = -1;
  for (int i = 0; i < (int)tracks.size(); ++i) {
    if (tracks[(size_t)i].id == shown) a = i;
    if (tracks[(size_t)i].id == trackId) b = i;
  }
  if (a < 0 || b < 0) { selectOnlyTrack(trackId); return; }
  for (int i = std::min(a, b); i <= std::max(a, b); ++i)
    if (tracks[(size_t)i].id != shown) alsoSelected_.insert(tracks[(size_t)i].id);
  for (auto& row : rows_) row->repaint();
}

void MainComponent::selectClip(const std::string& trackId, const std::string& clipId) {
  scoreOpen_ = false;
  automationOpen_ = false;
  boxClips_.clear();
  if (trackId != editor_.track()) releaseAllKeys();
  selectedTrack_ = trackId;
  selectedClip_ = clipId;
  const doc::Track* t = history_.current().findTrack(trackId);
  std::string patternId;
  if (t) for (const auto& c : t->clips) if (c.id == clipId) patternId = c.patternId;
  editor_.setTrack(trackId, patternId);
  { std::lock_guard<std::mutex> g(liveMutex_); liveTrack_ = editor_.track(); }
  showEditorForSelection();
  for (auto& row : rows_) row->repaint();
}

// ---------------------------------------------------------------- the selection box

void MainComponent::startBox(const juce::MouseEvent& e) {
  boxing_ = true;
  boxAdd_ = e.mods.isCommandDown();  // Ctrl: the box adds to what is selected
  boxFrom_ = boxTo_ = e.getScreenPosition();
}

void MainComponent::dragBox(const juce::MouseEvent& e) {
  boxTo_ = e.getScreenPosition();
  if (boxOverlay_) boxOverlay_->repaint();
}

void MainComponent::endBox(const juce::MouseEvent& e) {
  boxTo_ = e.getScreenPosition();
  boxing_ = false;
  if (boxOverlay_) boxOverlay_->repaint();
  const juce::Rectangle<int> box(boxFrom_, boxTo_);
  if (box.getWidth() < 4 && box.getHeight() < 4) {  // a click: what it did on the press stays
    // A click on an empty part of a lane also puts the start line there (to the beat), as in Ableton. Ctrl adds
    // to the selection and leaves the line where it is; while playing, only the ruler moves it (no jump by accident).
    if (!boxAdd_ && !snap_.playing) {
      const int x = getLocalPoint(nullptr, boxFrom_).x;
      if (x >= laneSpan().getX() && x < laneSpan().getRight()) moveShownPosition(tickAt(x, kBeat));
    }
    return;
  }
  // Only what can be seen counts (not the tracks scrolled away).
  const juce::Rectangle<int> seen = tracksView_ ? tracksView_->getScreenBounds() : box;
  std::vector<std::string> tracks;
  std::set<ClipKey> clips;
  for (const auto& row : rows_) {
    const auto rb = row->getScreenBounds().getIntersection(seen);
    if (rb.isEmpty() || box.getY() >= rb.getBottom() || box.getBottom() <= rb.getY()) continue;
    tracks.push_back(row->id());
    const int origin = row->getScreenX();
    for (const auto& c : row->clipsBetween(box.getX() - origin, box.getRight() - origin)) clips.insert({row->id(), c});
  }
  if (tracks.empty()) return;
  std::set<ClipKey> before = boxAdd_ ? boxClips_ : std::set<ClipKey>{};
  if (boxAdd_ && !selectedClip_.empty()) before.insert({selectedTrack_, selectedClip_});
  const auto selectedBefore = selectedTracks();
  std::set<std::string> others = boxAdd_ ? alsoSelected_ : std::set<std::string>{};
  const std::string shown = boxAdd_ && !selectedBefore.empty() ? selectedBefore.front() : tracks.front();
  clips.insert(before.begin(), before.end());
  if (clips.size() == 1) selectClip(clips.begin()->first, clips.begin()->second);  // one clip: as a click on it
  else if (shown != selectedTrack_ || !selectedClip_.empty()) {
    selectedClip_.clear();
    selectTrack(shown);
  }
  for (const auto& t : tracks) if (t != shown) others.insert(t);
  alsoSelected_ = others;
  if (clips.size() > 1) boxClips_ = clips;
  const size_t nTracks = selectedTracks().size();
  const juce::String n((int)nTracks);
  if (clips.size() > 1)
    notice_ = n + (nTracks == 1 ? u8(" track and ") : u8(" tracks and ")) + juce::String((int)clips.size()) +
              u8(" clips selected. Delete removes the clips; drag one to move them all.");
  else
    notice_ = nTracks == 1 ? juce::String(u8("1 track selected.")) : n + u8(" tracks selected.");
  for (auto& row : rows_) row->repaint();
}

void MainComponent::beginBoxMove() {
  boxStarts_.clear();
  const doc::Document& d = history_.current();
  for (const auto& key : boxClips_) {
    const doc::Track* t = d.findTrack(key.first);
    if (!t) continue;
    for (const auto& c : t->clips) if (c.id == key.second) boxStarts_[key] = c.startTick;
    for (const auto& c : t->audioClips) if (c.id == key.second) boxStarts_[key] = c.startTick;
  }
  beginGesture(u8("Move clips"));
}

// Every clip of the box by the same amount (none before the song's start), as one step of Undo.
void MainComponent::moveBox(int64_t delta) {
  if (boxStarts_.empty()) return;
  int64_t first = std::numeric_limits<int64_t>::max();
  for (const auto& [key, start] : boxStarts_) first = std::min(first, start);
  delta = std::max(delta, -first);
  const auto starts = boxStarts_;
  edit([&](doc::Document& d) {
    for (const auto& [key, start] : starts) {
      doc::Track* t = nullptr;
      for (auto& x : d.tracks) if (x.id == key.first) t = &x;
      if (!t) continue;
      bool pattern = false;
      for (const auto& c : t->clips) if (c.id == key.second) pattern = true;
      if (pattern) doc::moveClip(*t, key.second, start + delta);
      else doc::moveAudioClip(d, key.first, key.second, start + delta);
    }
  }, u8("Move clips"));
}

void MainComponent::endBoxMove() {
  const auto keys = boxClips_;
  for (const auto& key : keys) growSongToFit(key.first, key.second);  // inside the gesture: one undo step
  endGesture();
  boxStarts_.clear();
  for (auto& row : rows_) row->repaint();
}

void MainComponent::deleteBoxClips() {
  const auto keys = boxClips_;
  edit([&](doc::Document& d) {
    for (const auto& key : keys) {
      doc::Track* t = nullptr;
      for (auto& x : d.tracks) if (x.id == key.first) t = &x;
      if (!t) continue;
      bool pattern = false;
      for (const auto& c : t->clips) if (c.id == key.second) pattern = true;
      bool audio = false;
      for (const auto& c : t->audioClips) if (c.id == key.second) audio = true;
      if (pattern) doc::removeClip(*t, key.second);
      else if (audio) doc::removeAudioClip(d, key.first, key.second);
    }
  }, u8("Delete clips"));
  boxClips_.clear();
  notice_ = juce::String((int)keys.size()) + u8(" clips deleted (Ctrl+Z brings them back).");
  for (auto& row : rows_) row->repaint();
}

// ---------------------------------------------------------------- copy, cut, paste and duplicate clips

std::vector<MainComponent::ClipKey> MainComponent::selectedClipKeys() const {
  if (!boxClips_.empty()) return {boxClips_.begin(), boxClips_.end()};
  if (!selectedTrack_.empty() && !selectedClip_.empty()) return {{selectedTrack_, selectedClip_}};
  return {};
}

bool MainComponent::copyClips() {
  const doc::Document& d = history_.current();
  std::vector<CopiedClip> copied;
  int64_t from = std::numeric_limits<int64_t>::max(), to = 0;
  for (const auto& [trackId, clipId] : selectedClipKeys()) {
    const doc::Track* t = d.findTrack(trackId);
    if (!t) continue;
    for (const auto& c : t->clips)
      if (c.id == clipId) {
        CopiedClip x; x.trackId = trackId; x.clip = c;
        copied.push_back(x);
        from = std::min(from, c.startTick);
        to = std::max(to, c.startTick + c.lengthTicks);
      }
    for (const auto& c : t->audioClips)
      if (c.id == clipId) {
        CopiedClip x; x.trackId = trackId; x.audio = true; x.audioClip = c;
        copied.push_back(x);
        from = std::min(from, c.startTick);
        to = std::max(to, audioEndTick(d, c));  // through the tempo map (sweep: the first BPM only)
      }
  }
  if (copied.empty()) {
    notice_ = u8("Select a clip (or draw a box around several) to copy it.");
    repaint();
    return false;
  }
  copiedClips_ = std::move(copied);
  copiedFrom_ = from;
  copiedTo_ = std::max(to, from + 1);
  notice_ = juce::String((int)copiedClips_.size()) + (copiedClips_.size() == 1 ? u8(" clip copied") : u8(" clips copied")) +
            u8(" (Ctrl+V pastes at the playhead).");
  repaint();
  return true;
}

void MainComponent::pasteClips(int64_t at) {
  if (copiedClips_.empty()) {
    notice_ = u8("Nothing copied yet: select a clip and press Ctrl+C.");
    repaint();
    return;
  }
  at = std::max<int64_t>(0, at);
  // Never on top of the clips it was copied from: at the same place it goes right after them, as Ctrl+D.
  if (at == copiedFrom_) at = copiedTo_;
  std::set<ClipKey> pasted;
  int skipped = 0;
  const auto copies = copiedClips_;
  const int64_t from = copiedFrom_;
  edit([&](doc::Document& d) {
    for (const auto& c : copies) {
      if (!d.findTrack(c.trackId)) { ++skipped; continue; }
      try {
        const std::string id = c.audio ? doc::pasteAudioClip(d, c.trackId, c.audioClip, at + (c.audioClip.startTick - from))
                                       : doc::pasteClip(doc::trackById(d, c.trackId), c.clip, at + (c.clip.startTick - from));
        pasted.insert({c.trackId, id});
      } catch (const doc::DocumentError&) {
        ++skipped;  // its track or pattern is gone, or the track is full
      }
    }
    // The song grows to hold what was pasted, audio clips too (sweep: only note clips counted).
    for (const auto& [trackId, clipId] : pasted) fitSong(d, trackId, clipId);
  }, pasted.size() == 1 ? u8("Paste clip") : u8("Paste clips"));
  boxClips_ = pasted;
  if (pasted.size() == 1) selectClip(pasted.begin()->first, pasted.begin()->second);
  notice_ = juce::String((int)pasted.size()) + (pasted.size() == 1 ? u8(" clip pasted") : u8(" clips pasted")) +
            (skipped ? u8(", ") + juce::String(skipped) + u8(" could not be (their track or pattern is gone)") : juce::String()) + ".";
  for (auto& row : rows_) row->repaint();
  repaint();
}

void MainComponent::cutClips() {
  if (!copyClips()) return;
  const auto keys = selectedClipKeys();
  edit([&](doc::Document& d) {
    for (const auto& [trackId, clipId] : keys) {
      doc::Track* t = nullptr;
      for (auto& x : d.tracks) if (x.id == trackId) t = &x;
      if (!t) continue;
      if (std::any_of(t->clips.begin(), t->clips.end(), [&](const doc::Clip& c) { return c.id == clipId; })) doc::removeClip(*t, clipId);
      else if (std::any_of(t->audioClips.begin(), t->audioClips.end(), [&](const doc::AudioClip& c) { return c.id == clipId; }))
        doc::removeAudioClip(d, trackId, clipId);
    }
  }, keys.size() == 1 ? u8("Cut clip") : u8("Cut clips"));
  boxClips_.clear();
  if (!selectedTrack_.empty()) selectClip(selectedTrack_, {});
  notice_ = juce::String((int)keys.size()) + (keys.size() == 1 ? u8(" clip cut") : u8(" clips cut")) + u8(" (Ctrl+V pastes it at the playhead).");
  for (auto& row : rows_) row->repaint();
}

void MainComponent::duplicateClips() {
  const auto before = copiedClips_;
  const int64_t beforeFrom = copiedFrom_, beforeTo = copiedTo_;
  if (!copyClips()) return;
  pasteClips(copiedTo_);
  copiedClips_ = before;  // Ctrl+D does not change what Ctrl+C copied
  copiedFrom_ = beforeFrom;
  copiedTo_ = beforeTo;
}

// ---------------------------------------------------------------- live notes

namespace {
// Lower letter row as a piano: Z = C, S = C#, X = D ... M = B, comma = the next C.
int pianoOffset(int keyCode) {
  static const std::pair<int, int> keys[] = {{'Z', 0}, {'S', 1}, {'X', 2}, {'D', 3}, {'C', 4}, {'V', 5}, {'G', 6},
                                             {'B', 7}, {'H', 8}, {'N', 9}, {'J', 10}, {'M', 11}, {',', 12}};
  for (auto [k, o] : keys) if (keyCode == k || keyCode == juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k)) return o;
  return -1;
}
}  // namespace

bool MainComponent::pressPianoKey(int keyCode) {
  const int offset = pianoOffset(keyCode);
  if (offset < 0 || editor_.track().empty()) return false;
  if (heldKeys_.count(keyCode)) return true;  // key repeat
  const int pitch = juce::jlimit(0, 127, 12 * (keyboardOctave_ + 1) + offset);
  heldKeys_[keyCode] = {editor_.track(), pitch};
  session_->liveNote(editor_.track(), pitch, 0.8, true);
  return true;
}

void MainComponent::releasePianoKey(int keyCode) {
  auto it = heldKeys_.find(keyCode);
  if (it == heldKeys_.end()) return;
  session_->liveNote(it->second.first, it->second.second, 0, false);
  heldKeys_.erase(it);
}

void MainComponent::releaseAllKeys() {
  while (!heldKeys_.empty()) releasePianoKey(heldKeys_.begin()->first);
}

bool MainComponent::keyStateChanged(bool) {
  std::vector<int> released;
  for (const auto& [code, note] : heldKeys_)
    if (!juce::KeyPress::isKeyCurrentlyDown(code)) released.push_back(code);
  for (int code : released) releasePianoKey(code);
  return false;
}

void MainComponent::focusLost(FocusChangeType) { releaseAllKeys(); }

void MainComponent::openMidiInput(const juce::String& identifier) {
  if (midiInput_) { midiInput_->stop(); midiInput_.reset(); }
  {
    // Notes still held on the old keyboard end now.
    std::lock_guard<std::mutex> g(liveMutex_);
    for (const auto& [track, pitch] : midiHeld_) session_->liveNote(track, pitch, 0, false);
    midiHeld_.clear();
  }
  midiInputId_ = identifier;
  if (identifier.isNotEmpty()) {
    midiInput_ = juce::MidiInput::openDevice(identifier, this);
    if (midiInput_) midiInput_->start();
    else notice_ = u8("Could not open the MIDI input.");
  }
  if (!test_) {
    settings_.getUserSettings()->setValue("midiInput", identifier);
    settings_.saveIfNeeded();
  }
}

// MIDI thread. Notes go straight to the engine session (never through the window's timer), and play
// at the start of the engine's next audio block: the event's own timestamp is not kept (MVP).
void MainComponent::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) {
  std::lock_guard<std::mutex> g(liveMutex_);
  if (m.isAllNotesOff() || m.isAllSoundOff()) {
    midiHeld_.clear();
    session_->panic();
    return;
  }
  if (m.isNoteOn()) {
    if (liveTrack_.empty()) return;
    midiHeld_.push_back({liveTrack_, m.getNoteNumber()});
    session_->liveNote(liveTrack_, m.getNoteNumber(), m.getFloatVelocity(), true);
  } else if (m.isNoteOff()) {
    // The note ends on the track that started it, even if another track was selected meanwhile; with the
    // same pitch held twice, the oldest one ends first.
    for (auto it = midiHeld_.begin(); it != midiHeld_.end(); ++it)
      if (it->second == m.getNoteNumber()) {
        session_->liveNote(it->first, it->second, 0, false);
        midiHeld_.erase(it);
        break;
      }
  }
}

void MainComponent::growSongToFit(const std::string& trackId, const std::string& clipId) {
  doc::Document probe = history_.current();
  if (fitSong(probe, trackId, clipId)) edit([&](doc::Document& d) { fitSong(d, trackId, clipId); }, u8("Song length"));
}

void MainComponent::addClipAt(const std::string& trackId, int64_t tick) {
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!t || t->patterns.empty()) return;  // an audio track: clips come from Import audio
  const std::string patternId = editor_.track() == trackId ? editor_.patternId() : t->patterns.front().id;
  std::string clipId;
  edit([&](doc::Document& d) { clipId = doc::addClip(doc::trackById(d, trackId), patternId, tick, 4 * kBar); fitSong(d, trackId, clipId); }, "New clip");
  if (!clipId.empty()) selectClip(trackId, clipId);
}

void MainComponent::showClipMenu(const std::string& trackId, const std::string& clipId, int64_t atTick) {
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!t) return;
  const doc::Clip* clip = nullptr;
  for (const auto& c : t->clips) if (c.id == clipId) clip = &c;
  if (!clip) return;
  juce::PopupMenu menu, patterns;
  menu.addItem(juce::PopupMenu::Item(u8("Make independent copy")).setID(1).setEnabled(t->clipsUsing(clip->patternId) > 1));
  // One section with its own notes: the clip cut at the section's edges, that part on a copy of the pattern
  // (the other sections keep the original), opened in the editor.
  const doc::Section* section = nullptr;
  const int64_t clipEnd = clip->startTick + clip->lengthTicks;
  if (atTick >= 0)
    for (const auto& s : history_.current().sections)
      if (atTick >= s.startTick && atTick < s.startTick + s.lengthTicks) section = &s;
  const int64_t secFrom = section ? std::max(section->startTick, clip->startTick) : -1, secTo = section ? std::min(section->startTick + section->lengthTicks, clipEnd) : -1;
  const bool splitBar = atTick > clip->startTick && atTick < clipEnd;
  if (section && secTo > secFrom && (secFrom > clip->startTick || secTo < clipEnd || t->clipsUsing(clip->patternId) > 1))
    menu.addItem(juce::PopupMenu::Item(u8("Own notes in section ") + u8(section->name)).setID(5));
  menu.addItem(juce::PopupMenu::Item(u8("Split here (bar ") + juce::String(atTick / kBar + 1) + ")").setID(4).setEnabled(splitBar));
  for (size_t i = 0; i < t->patterns.size(); ++i)
    patterns.addItem(juce::PopupMenu::Item(u8("Pattern ") + u8(t->patterns[i].name)).setID(100 + (int)i).setTicked(t->patterns[i].id == clip->patternId));
  patterns.addSeparator();
  patterns.addItem(juce::PopupMenu::Item(u8("New empty pattern")).setID(2));
  menu.addSubMenu(u8("Use pattern"), patterns);
  menu.addSeparator();
  menu.addItem(juce::PopupMenu::Item("Delete clip").setID(3));
  menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackId, clipId, atTick, secFrom, secTo, clipEnd](int chosen) {
    if (chosen == 4) {
      edit([&](doc::Document& d) { doc::splitClip(doc::trackById(d, trackId), clipId, atTick); }, u8("Split clip"));
      return;
    }
    if (chosen == 5) {
      std::string middle = clipId, pattern;
      edit([&](doc::Document& d) {
        doc::Track& tr = doc::trackById(d, trackId);
        int64_t start = 0;
        for (const auto& c : tr.clips) if (c.id == clipId) start = c.startTick;
        if (secFrom > start) middle = doc::splitClip(tr, clipId, secFrom);
        if (secTo < clipEnd) doc::splitClip(tr, middle, secTo);
        pattern = doc::makeClipUnique(tr, middle);
      }, u8("Own notes in a section"));
      selectClip(trackId, middle);
      return;
    }
    if (chosen == 1) {
      std::string own;
      edit([&](doc::Document& d) { own = doc::makeClipUnique(doc::trackById(d, trackId), clipId); }, u8("Independent copy"));
      selectClip(trackId, clipId);
    } else if (chosen == 2) {
      edit([&](doc::Document& d) {
        doc::Track& t = doc::trackById(d, trackId);
        const std::string p = doc::addPattern(t, doc::kPpq * 4);
        for (auto& c : t.clips) if (c.id == clipId) c.patternId = p;
      }, u8("New pattern"));
      selectClip(trackId, clipId);
    } else if (chosen == 3) {
      edit([&](doc::Document& d) { doc::removeClip(doc::trackById(d, trackId), clipId); }, "Delete clip");
      selectedClip_.clear();
    } else if (chosen >= 100) {
      edit([&](doc::Document& d) {
        doc::Track& t = doc::trackById(d, trackId);
        if ((size_t)(chosen - 100) < t.patterns.size())
          for (auto& c : t.clips) if (c.id == clipId) c.patternId = t.patterns[(size_t)(chosen - 100)].id;
      }, u8("Change pattern"));
      selectClip(trackId, clipId);
    }
  });
}

// ---------------------------------------------------------------- audio

std::filesystem::path MainComponent::projectDir() const {
  return !imported_ && file_ != juce::File() ? pathOf(file_.getParentDirectory()) : std::filesystem::path();
}

void MainComponent::projectFolderChanged() {
  session_->setAssetRoot(projectDir());
  checkMissingAssets();
}

void MainComponent::checkMissingAssets() {
  missing_ = projectDir().empty() ? std::vector<std::string>() : doc::missingAssets(history_.current(), projectDir());
  if (!missing_.empty())
    notice_ = juce::String((int)missing_.size()) + u8(" missing audio file(s) (in red). File > Locate missing files…");
  for (auto& row : rows_) row->repaint();
}

void MainComponent::importAudio() {
  if (projectDir().empty()) {
    // The files are copied next to the song: it needs a folder first.
    notice_ = u8("Save the song first: the audio is copied into its folder.");
    save(true, [this](SaveResult r) { if (r != SaveResult::Failed) importAudio(); });  // the song has its folder now
    return;
  }
  chooser_ = std::make_unique<juce::FileChooser>(u8("Import audio (WAV)"), juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
    const juce::File f = fc.getResult();
    juce::String problem;
    if (f != juce::File() && !importAudioFile(f, &problem)) showError(u8("Could not import"), f.getFileName() + ": " + problem);
  });
}

// One undo step: the asset, a new audio track (unless an audio track is selected) and the clip.
bool MainComponent::importAudioFile(const juce::File& wav, juce::String* problem, const std::string& onTrack, int64_t where, int64_t* end) {
  const std::filesystem::path dir = projectDir();
  if (dir.empty()) { if (problem) *problem = u8("the song has not been saved yet"); return false; }
  const doc::Track* selected = history_.current().findTrack(onTrack.empty() ? selectedTrack_ : onTrack);
  std::string trackId = selected && selected->instrument.kind == "audio" ? selected->id : std::string();
  const int64_t at = where >= 0 ? where : (int64_t)snap_.positionTick / kBar * kBar;
  std::string clipId, error;
  doc::Document next = history_.current();
  try {
    const std::string assetId = doc::importAudio(next, pathOf(wav), dir);
    if (trackId.empty()) trackId = doc::addAudioTrack(next, wav.getFileNameWithoutExtension().toStdString());
    clipId = doc::addAudioClip(next, trackId, assetId, at);
    for (const auto& c : doc::trackById(next, trackId).audioClips)  // the song grows to hold the whole clip
      if (c.id == clipId && audioEndTick(next, c) > next.lengthTicks) doc::setSongLength(next, (audioEndTick(next, c) + kBar - 1) / kBar * kBar);
  } catch (const std::exception& e) {
    if (problem) *problem = u8(e.what());
    return false;
  }
  edit([&](doc::Document& d) { d = next; }, u8("Import ") + wav.getFileName());
  if (end)
    for (const auto& c : doc::trackById(next, trackId).audioClips) if (c.id == clipId) *end = audioEndTick(next, c);
  selectClip(trackId, clipId);
  checkMissingAssets();
  notice_ = u8("Imported: ") + wav.getFileName() + u8(" (copied into the song's Audio folder)");
  return true;
}

void MainComponent::importFiles(std::vector<juce::File> wavs, const std::string& onTrack, int64_t at) {
  if (wavs.empty()) return;
  if (projectDir().empty()) {  // the files are copied next to the song: it needs a folder first
    notice_ = u8("Save the song first: the audio is copied into its folder.");
    save(true, [this, wavs, onTrack, at](SaveResult r) { if (r != SaveResult::Failed) importFiles(wavs, onTrack, at); });
    return;
  }
  std::string track = onTrack;
  int64_t next = at;
  for (const auto& wav : wavs) {
    juce::String problem;
    int64_t end = -1;
    if (!importAudioFile(wav, &problem, track, next, &end)) { showError(u8("Could not import"), wav.getFileName() + ": " + problem); return; }
    // The next file goes right after it, on the same track (the one just made, when there was none).
    track = selectedTrack_;
    if (end >= 0) next = end;
  }
}

bool MainComponent::takesSamples(const doc::Track& t) {
  return t.instrument.kind != "audio" && (t.instrument.name == "sampler" || !doc::trackSampleRole(t).empty());
}

void MainComponent::dropLibrarySample(const std::string& trackId, const std::string& sha256, int64_t at) {
  const doc::LibrarySample* s = hasSampleIndex_ ? sampleIndex_.findHash(sha256) : nullptr;
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!s || !t) return;
  if (t->instrument.kind == "audio") { importFiles({juce::File(u8(s->file))}, trackId, at); return; }
  selectTrack(trackId);
  putSampleOnTrack(trackId, *s);
}

void MainComponent::useSampleFile(const std::string& trackId, const juce::File& f) {
  const std::filesystem::path p = pathOf(f);
  const auto u = p.u8string();
  if (const doc::LibrarySample* s = hasSampleIndex_ ? sampleIndex_.findFile(std::string(u.begin(), u.end())) : nullptr)
    return putSampleOnTrack(trackId, *s);
  try {
    const doc::LibrarySample s = doc::describeSample(p, pathOf(sampleLibraryFolder()));
    putSampleOnTrack(trackId, s);
  } catch (const std::exception& e) {
    showError(u8("Sample not used"), f.getFileName() + ": " + u8(e.what()));
  }
}

MainComponent::TrackRow* MainComponent::rowAtScreen(juce::Point<int> screen) const {
  if (!tracksView_ || !tracksView_->getScreenBounds().contains(screen)) return nullptr;  // not the part scrolled away
  for (const auto& row : rows_)
    if (row->isShowing() && row->getScreenBounds().contains(screen)) return row.get();
  return nullptr;
}

void MainComponent::setDropRow(const std::string& trackId) {
  if (dropRow_ == trackId) return;
  dropRow_ = trackId;
  for (const auto& row : rows_) row->repaint();
}

static std::vector<juce::File> wavFiles(const juce::StringArray& files) {
  std::vector<juce::File> out;
  for (const auto& f : files) if (juce::File(f).hasFileExtension("wav")) out.push_back(juce::File(f));
  return out;
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files) { return !wavFiles(files).empty(); }

void MainComponent::fileDragMove(const juce::StringArray&, int x, int y) {
  const TrackRow* row = rowAtScreen(localPointToGlobal(juce::Point<int>(x, y)));
  setDropRow(row ? row->id() : std::string());
}

void MainComponent::fileDragExit(const juce::StringArray&) { setDropRow({}); }

void MainComponent::filesDropped(const juce::StringArray& files, int x, int y) {
  setDropRow({});
  const auto wavs = wavFiles(files);
  const juce::Point<int> screen = localPointToGlobal(juce::Point<int>(x, y));
  TrackRow* row = rowAtScreen(screen);
  const doc::Track* t = row ? history_.current().findTrack(row->id()) : nullptr;
  if (t && takesSamples(*t)) {  // a sound for the track
    if (wavs.size() != 1) { notice_ = u8("One sound per track: drop a single WAV on a sampler or drum track."); return; }
    selectTrack(t->id);
    useSampleFile(t->id, wavs.front());
    return;
  }
  // On an audio track: there; on an instrument track: a new audio track (importAudioFile makes one).
  importFiles(wavs, t ? t->id : std::string(), row ? row->dropTick(screen) : -1);
}

void MainComponent::showAudioClipMenu(const std::string& trackId, const std::string& clipId, int64_t tick) {
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!t) return;
  const doc::AudioClip* clip = nullptr;
  for (const auto& c : t->audioClips) if (c.id == clipId) clip = &c;
  if (!clip) return;
  juce::PopupMenu menu, fadeIn, fadeOut;
  const int64_t splitAt = tick / kBeat * kBeat > clip->startTick ? tick / kBeat * kBeat : tick;
  menu.addItem(1, u8("Split here"), splitAt > clip->startTick && splitAt < audioEndTick(history_.current(), *clip));
  static const double fades[] = {0, 0.01, 0.1, 0.5, 2.0};
  static const char* fadeNames[] = {"No fade", "10 ms", "100 ms", "0.5 s", "2 s"};
  for (int i = 0; i < 5; ++i) {
    fadeIn.addItem(juce::PopupMenu::Item(u8(fadeNames[i])).setID(10 + i).setTicked(std::abs(clip->fadeInSeconds - fades[i]) < 1e-6));
    fadeOut.addItem(juce::PopupMenu::Item(u8(fadeNames[i])).setID(20 + i).setTicked(std::abs(clip->fadeOutSeconds - fades[i]) < 1e-6));
  }
  menu.addSubMenu("Fade in", fadeIn);
  menu.addSubMenu("Fade out", fadeOut);
  menu.addItem(2, u8("Gain…"));
  menu.addItem(juce::PopupMenu::Item("Mute").setID(3).setTicked(clip->mute));
  menu.addSeparator();
  menu.addItem(4, "Delete clip");
  const juce::String gain = juce::String(clip->gainDb, 1);
  menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackId, clipId, splitAt, gain](int chosen) {
    auto change = [&](const std::function<void(doc::AudioClip&)>& f, const juce::String& label) {
      edit([&](doc::Document& d) { for (auto& c : doc::trackById(d, trackId).audioClips) if (c.id == clipId) f(c); }, label);
    };
    if (chosen == 1) {
      std::string second;
      edit([&](doc::Document& d) { second = doc::splitAudioClip(d, trackId, clipId, splitAt); }, u8("Split audio clip"));
      if (!second.empty()) selectClip(trackId, second);
    } else if (chosen >= 10 && chosen < 15) {
      const double s = fades[chosen - 10];
      change([s](doc::AudioClip& c) { c.fadeInSeconds = std::min(s, c.durationSeconds); }, "Fade in");
    } else if (chosen >= 20 && chosen < 25) {
      const double s = fades[chosen - 20];
      change([s](doc::AudioClip& c) { c.fadeOutSeconds = std::min(s, c.durationSeconds); }, "Fade out");
    } else if (chosen == 2) {
      askText(u8("Clip gain (dB, -60 to +12)"), gain, [this, trackId, clipId](const std::string& text) {
        const double db = juce::jlimit(-60.0, 12.0, juce::String(text).replaceCharacter(',', '.').getDoubleValue());
        edit([&](doc::Document& d) { for (auto& c : doc::trackById(d, trackId).audioClips) if (c.id == clipId) c.gainDb = db; }, u8("Clip gain"));
      });
    } else if (chosen == 3) {
      change([](doc::AudioClip& c) { c.mute = !c.mute; }, "Clip mute");
    } else if (chosen == 4) {
      edit([&](doc::Document& d) { doc::removeAudioClip(d, trackId, clipId); }, "Delete clip");
      selectedClip_.clear();
    }
  });
}

void MainComponent::relocateMissing() {
  if (missing_.empty() || projectDir().empty()) return;
  const doc::Asset* asset = history_.current().findAsset(missing_.front());
  if (!asset) return;
  const std::string assetId = asset->id;
  chooser_ = std::make_unique<juce::FileChooser>(u8("Where is \"") + u8(asset->name) + "\"?", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, assetId](const juce::FileChooser& fc) {
    const juce::File f = fc.getResult();
    if (f == juce::File()) return;
    try {
      doc::Document copy = history_.current();  // the document does not change: the file goes back in place
      doc::relocateAsset(copy, assetId, pathOf(f), projectDir());
    } catch (const std::exception& e) {
      showError(u8("Could not locate the file"), f.getFileName() + ": " + u8(e.what()));
      return;
    }
    projectFolderChanged();
    if (missing_.empty()) notice_ = u8("All audio files are in place.");
    else relocateMissing();
  });
}

void MainComponent::askText(const juce::String& title, const juce::String& initial, std::function<void(const std::string&)> done) {
  auto* w = new juce::AlertWindow(lang::tr(title), juce::String(), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("text", initial);
  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> safe(w);
  w->enterModalState(true, juce::ModalCallbackFunction::create([safe, done](int result) {
    if (result == 1 && safe != nullptr) done(safe->getTextEditorContents("text").toStdString());
  }), true);
}

// Right click on the ruler: new section here, or the section under the mouse.
void MainComponent::showSectionMenu(int64_t tick) {
  const doc::Document& d = history_.current();
  const doc::Section* at = nullptr;
  for (const auto& s : d.sections) if (tick >= s.startTick && tick < s.startTick + s.lengthTicks) at = &s;
  juce::PopupMenu menu;
  const int64_t bar = tick / kBar * kBar;
  if (!at) menu.addItem(1, u8("New section here (8 bars)"));
  else {
    menu.addSectionHeader(u8(at->name));
    menu.addItem(2, u8("Rename…"));
    menu.addItem(3, u8("Swap with the previous one"));
    menu.addItem(4, u8("Swap with the next one"));
    menu.addItem(5, u8("Duplicate (inserts the copy after it)"));
    // Colour: the same hues as the tracks; "By type" follows the name (intro green, drop red…).
    juce::PopupMenu colour;
    const std::string typeColor = doc::sectionTypeColor(at->name);
    colour.addItem(juce::PopupMenu::Item(u8("By type (from the name)")).setID(20).setTicked(at->color.empty())
                       .setColour(hexColour(typeColor)));
    colour.addSeparator();
    for (int i = 0; i < (int)std::size(doc::kSectionSwatches); ++i) {
      const auto& sw = doc::kSectionSwatches[i];
      colour.addItem(juce::PopupMenu::Item(u8(sw.name)).setID(21 + i).setTicked(at->color == sw.color).setColour(hexColour(sw.color)));
    }
    menu.addSubMenu(u8("Colour"), colour);
    menu.addSeparator();
    menu.addItem(6, u8("Remove section (the music stays)"));
  }
  menu.addSeparator();
  // Tempo changes (the tempo map): one at the beat under the mouse, or the one near it removed.
  const int64_t beat = (tick + kBeat / 2) / kBeat * kBeat;
  int64_t near = -1;
  for (const auto& t : d.tempoMap) if (std::abs(t.tick - tick) <= kBeat && (near < 0 || std::abs(t.tick - tick) < std::abs(near - tick))) near = t.tick;
  menu.addItem(8, near >= 0 ? u8("Change the tempo here…") : u8("Tempo change here…"), beat > 0 && beat < d.lengthTicks);
  if (near >= 0) menu.addItem(9, u8("Remove this tempo change"));
  if (!d.tempoMap.empty()) menu.addItem(10, u8("Remove all tempo changes (") + juce::String((int)d.tempoMap.size()) + ")");
  menu.addSeparator();
  menu.addItem(12, u8("Insert empty bars here…"));
  menu.addItem(13, u8("Delete bars from here…"), bar < d.lengthTicks && d.lengthTicks > kBar);
  menu.addSeparator();
  if (d.loop.endTick > d.loop.startTick) menu.addItem(11, u8("Remove loop (or double-click it)"));
  menu.addItem(7, u8("Song length…"));
  const std::string sectionId = at ? at->id : std::string();
  const juce::String sectionName = at ? u8(at->name) : juce::String();
  menu.showMenuAsync(juce::PopupMenu::Options(), [this, bar, beat, near, sectionId, sectionName](int chosen) {
    if (chosen == 12 || chosen == 13) {
      const bool insert = chosen == 12;
      askText(insert ? u8("Insert empty bars at bar ") + juce::String(bar / kBar + 1) + u8(" (everything after moves on)")
                     : u8("Delete bars from bar ") + juce::String(bar / kBar + 1) + u8(" (the music there goes, the rest moves back)"),
              "1", [this, bar, insert](const std::string& text) {
        const int bars = juce::String(text).getIntValue();
        if (bars <= 0) return;
        edit([&](doc::Document& d) {
          if (insert) doc::insertTime(d, bar, (int64_t)bars * kBar);
          else doc::deleteTime(d, bar, std::min<int64_t>((int64_t)bars * kBar, d.lengthTicks - bar));
        }, insert ? u8("Insert bars") : u8("Delete bars"));
      });
      return;
    }
    if (chosen == 11) {
      edit([](doc::Document& d) { d.loop = {false, 0, 0, d.loop.extra}; }, u8("Remove loop"));
      return;
    }
    if (chosen == 8) {
      const int64_t where = near >= 0 ? near : beat;
      askText(u8("Tempo from here (BPM, 40-600)"), juce::String(doc::bpmAt(history_.current(), where), 1).trimCharactersAtEnd("0").trimCharactersAtEnd("."),
              [this, where](const std::string& text) {
                const double bpm = juce::String(text).getDoubleValue();
                edit([&](doc::Document& d) {
                  std::vector<doc::TempoPoint> map = d.tempoMap;
                  map.erase(std::remove_if(map.begin(), map.end(), [&](const doc::TempoPoint& t) { return t.tick == where; }), map.end());
                  doc::TempoPoint point;
                  point.tick = where;
                  point.bpm = bpm;
                  map.push_back(point);
                  doc::setTempoMap(d, std::move(map));  // throws (shown) outside 40-600
                }, u8("Tempo change"));
              });
      return;
    }
    if (chosen == 9 || chosen == 10) {
      edit([&](doc::Document& d) {
        std::vector<doc::TempoPoint> map = d.tempoMap;
        if (chosen == 10) map.clear();
        else map.erase(std::remove_if(map.begin(), map.end(), [&](const doc::TempoPoint& t) { return t.tick == near; }), map.end());
        doc::setTempoMap(d, std::move(map));
      }, chosen == 10 ? u8("Remove tempo changes") : u8("Remove tempo change"));
      return;
    }
    if (chosen == 1) {
      // Up to the next section (or 8 bars), so new sections never overlap.
      int64_t length = 8 * kBar;
      for (const auto& s : history_.current().sections) if (s.startTick > bar) length = std::min(length, s.startTick - bar);
      const juce::String name = u8("Section ") + juce::String(history_.current().sections.size() + 1);
      edit([&](doc::Document& d) { doc::addSection(d, name.toStdString(), bar, length); }, u8("New section"));
    } else if (chosen == 2) {
      askText(u8("Rename section"), sectionName, [this, sectionId](const std::string& name) {
        edit([&](doc::Document& d) { doc::renameSection(d, sectionId, name); }, u8("Rename section"));
      });
    } else if (chosen >= 20 && chosen < 21 + (int)std::size(doc::kSectionSwatches)) {
      const std::string color = chosen == 20 ? std::string() : std::string(doc::kSectionSwatches[chosen - 21].color);
      edit([&](doc::Document& d) { doc::setSectionColor(d, sectionId, color); }, u8("Section colour"));
    } else if (chosen == 3 || chosen == 4) {
      edit([&](doc::Document& d) { doc::swapSection(d, sectionId, chosen == 4); }, u8("Swap sections"));
    } else if (chosen == 5) {
      edit([&](doc::Document& d) { doc::duplicateSection(d, sectionId); }, u8("Duplicate section"));
    } else if (chosen == 6) {
      edit([&](doc::Document& d) { doc::removeSection(d, sectionId); }, u8("Remove section"));
    } else if (chosen == 7) {
      askText(u8("Song length (bars)"), juce::String(history_.current().lengthTicks / kBar), [this](const std::string& text) {
        const int bars = juce::String(text).getIntValue();
        const size_t changesBefore = history_.current().tempoMap.size();
        edit([&](doc::Document& d) { doc::setSongLength(d, (int64_t)bars * kBar); }, u8("Song length"));
        if (const size_t gone = changesBefore - history_.current().tempoMap.size(); gone > 0)  // past the new end (Undo brings them back)
          notice_ = juce::String((int)gone) + u8(gone == 1 ? " tempo change was past the new end and went (Undo brings it back)."
                                                           : " tempo changes were past the new end and went (Undo brings them back).");
      });
    }
  });
}

void MainComponent::undo() { if (history_.undo()) documentChanged(false); }
void MainComponent::redo() { if (history_.redo()) documentChanged(false); }

void MainComponent::buildRows() {
  if (!selectedTrack_.empty() && !history_.current().findTrack(selectedTrack_)) {
    // Deep hunt P06: not tracks.front(), which Stage Core then acted on with the rest of the selection.
    std::vector<std::string> before;
    for (const auto& row : rows_) before.push_back(row->id());
    selectedTrack_ = doc::shownAfterRemoval(history_.current(), before, selectedTrack_, alsoSelected_);
    selectedClip_.clear();
    editor_.setTrack(selectedTrack_);
    std::lock_guard<std::mutex> g(liveMutex_);
    liveTrack_ = selectedTrack_;
  }
  rows_.clear();
  meters_.clear();
  for (const auto& t : history_.current().tracks) {
    rows_.push_back(std::make_unique<TrackRow>(*this, t));
    tracksHolder_.addAndMakeVisible(*rows_.back());
  }
  editor_.documentChanged();
  if (editor_.track().empty() && !history_.current().tracks.empty()) editor_.setTrack(history_.current().tracks.front().id);
  // The first time only: room for the song's tracks (up to eight), never taller than the screen. Later the window
  // keeps its size and the tracks scroll.
  if (getHeight() == 0) {
    int screen = 1000;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) screen = (int)display->userBounds.getHeight() - 40;
    const int wanted = kFooter + kTopBar + 4 + 36 + 8 + 52 + 8 + 24 + std::min<int>((int)rows_.size(), 8) * kTrackRow + 40 + 8 + kEditorHeight + 8 + 64 + 16;
    setSize(std::max(getWidth(), 1560), std::max(720, std::min(wanted, screen)));
  }
  resized();
}

// ---------------------------------------------------------------- files

void MainComponent::showError(const juce::String& title, const juce::String& message) {
  logLine("Error shown: " + title + ": " + message.replaceCharacters("\n", " "));
  juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
                                   .withTitle(title).withMessage(message).withButton("OK", 0), nullptr);
}

// ---------------------------------------------------------------- the AI's proposals (AICORE)

// The AI lives in AICORE (Diogo, 2026-11-05): Core only writes what it can do and the song (core-state.json), and
// shows the proposals AICORE drops in the bridge folder. Nothing runs without Apply. AI > Allow AI proposals off:
// Core writes nothing and shows nothing (the files wait).
juce::File MainComponent::bridgeFolder() const {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("AI");
}

void MainComponent::setAiControl(bool on) {
  if (realOutput_) settings_.getUserSettings()->setValue("aiControl", on);
  if (aiAllowed_ != on) logLine(on ? "AI proposals on" : "AI proposals off");
  aiAllowed_ = on;
  inboxSeen_.clear();  // read again (or forgotten) at the next tick
  stateWritten_.clear();
  if (!on) inbox_.clear();
  menuItemsChanged();
  updateAiBadge();
}

std::vector<control::CoreFunction> MainComponent::callableFunctions() const {
  return control::buildFunctions();
}

// Every second: the state AICORE reads (written when it changed), and the proposals waiting.
void MainComponent::pollBridge() {
  if (!realOutput_) return;
  const juce::uint32 now = juce::Time::getMillisecondCounter();
  if (now - bridgePolled_ < 1000) return;
  bridgePolled_ = now;
  const std::filesystem::path folder = pathOf(bridgeFolder());
  // AICORE saved samples into the library (Studio > Edit): read the new files now, no Rescan by hand.
  const std::string added = control::samplesAddedSignature(folder);
  if (samplesAddedSeen_ == "?") samplesAddedSeen_ = added;  // at start the library is scanned anyway
  else if (added != samplesAddedSeen_ && !(sampleScan_ && sampleScan_->state.load() == 0)) {  // a scan running: next second
    samplesAddedSeen_ = added;
    if (!added.empty() && sampleLibraryFolder().isDirectory()) {
      logLine("AICORE saved new samples: reading the library again");
      scanSampleLibrary();
    }
  }
  const control::AicoreStatus was = aicore_;
  aicore_ = control::readAicoreStatus(folder);
  if (was.ready() != aicore_.ready() || was.running != aicore_.running) {
    logLine(aicore_.ready() ? "AICORE: chat open and connected" + (aicore_.model.empty() ? juce::String() : " (" + u8(aicore_.model) + ")")
            : aicore_.running ? juce::String("AICORE: running, no AI connection") : juce::String("AICORE: not running"));
    updateAiBadge();
  }
  if (!aiAllowed_) return;
  if (!history_.inGesture()) {
    json st = control::coreState(callableFunctions(), history_.current(), hasSampleIndex_ ? &sampleIndex_ : nullptr);
    // The whole library is never in the state: AICORE reads Core's own index file to search it.
    if (st["samples"].is_object()) st["samples"]["indexFile"] = sampleIndexFile().getFullPathName().toStdString();
    const std::string state = st.dump();
    if (state != stateWritten_) {
      std::string why;
      if (control::writeState(folder, json::parse(state), &why)) stateWritten_ = state;
      else if (stateWritten_ != "!") { logLine("AI: " + u8(why)); stateWritten_ = "!"; }
    }
  }
  const std::string sig = control::inboxSignature(folder);
  if (sig == inboxSeen_) return;
  inboxSeen_ = sig;
  inbox_ = control::readInbox(folder);
  logLine("AI: " + juce::String((int)inbox_.size()) + " proposal(s) waiting");
  updateAiBadge();
  if (proposals_ != nullptr) showProposals();  // open: shows the new list
}

void MainComponent::showProposals() {
  const auto fns = callableFunctions();
  std::vector<ProposalsPanel::Item> items;
  for (const auto& in : inbox_) {
    ProposalsPanel::Item item;
    item.title = u8(in.id);
    if (!in.error.empty()) {
      item.problems.push_back(u8(in.error));
    } else {
      const control::Proposal p = control::validateProposal(in.proposal, fns, history_.current(), hasSampleIndex_ ? &sampleIndex_ : nullptr);
      if (!p.summary.empty()) item.title = u8(p.summary);
      for (const auto& c : p.calls) item.calls.push_back(u8(control::describeCall(c)));
      for (const auto& w : p.warnings()) item.warnings.push_back(u8(w));
      for (const auto& x : p.problems) item.problems.push_back(u8(x));
      item.canApply = !p.calls.empty();
    }
    items.push_back(std::move(item));
  }
  const juce::String empty = u8("No proposals. The AI (AICORE) reads what Core can do from ") + bridgeFolder().getChildFile("core-state.json").getFullPathName() +
                             u8(" and leaves its proposals in the inbox there.");
  if (proposals_ != nullptr) {
    proposals_->setItems(std::move(items), empty);
    if (auto* box = proposals_->findParentComponentOfClass<juce::CallOutBox>()) box->updatePosition(aiBadge_->getScreenBounds(), getScreenBounds());
    return;
  }
  auto panel = std::make_unique<ProposalsPanel>();
  panel->setItems(std::move(items), empty);
  juce::Component::SafePointer<MainComponent> self(this);
  panel->onApply = [self](int i, juce::Component* button) { if (self) self->applyProposal(i, button); };
  panel->onDismiss = [self](int i) { if (self) self->finishProposal(i, "dismissed", json::object()); };
  proposals_ = panel.get();
  juce::CallOutBox::launchAsynchronously(std::move(panel), aiBadge_->getScreenBounds(), nullptr);
}

// Apply: checked again against the song as it is now; calls without a track go to the one Diogo picks (the
// selected track first). One "AI: ..." step of Undo. A refusal changes nothing and is told to AICORE.
void MainComponent::applyProposal(int index, juce::Component* button) {
  if (index < 0 || index >= (int)inbox_.size() || !aiAllowed_) return;
  const control::InboxItem item = inbox_[(size_t)index];
  const auto fns = callableFunctions();
  const doc::SampleIndex* samples = hasSampleIndex_ ? &sampleIndex_ : nullptr;
  const control::Proposal p = control::validateProposal(item.proposal, fns, history_.current(), samples);
  if (p.calls.empty()) return;
  auto run = [this, item, fns, samples](const std::string& target) {
    const control::Proposal again = control::validateProposal(item.proposal, fns, history_.current(), samples);
    control::ApplyEnv env{samples};
    std::vector<std::string> lines;
    try {
      doc::Document probe = history_.current();  // refused here, before any undo step
      lines = control::applyProposal(probe, again, target, env);
    } catch (const std::exception& e) {
      showError(u8("AI proposal"), u8(e.what()));
      json problems = again.problems;
      problems.push_back(e.what());
      finishProposal(indexOf(item.id), "refused", {{"problems", problems}});
      return;
    }
    std::string label = control::kAiLabelPrefix + (again.summary.empty() ? lines.front() : again.summary);
    if (label.size() > 70) {
      size_t cut = 70;
      while (cut > 0 && ((unsigned char)label[cut] & 0xC0) == 0x80) --cut;
      label = label.substr(0, cut) + "\xE2\x80\xA6";
    }
    edit([&](doc::Document& d) { control::applyProposal(d, again, target, env); }, u8(label));
    notice_ = u8(label) + u8(" (Edit > Undo takes it back)");
    // "open" calls: the windows AICORE asked for, once the song has the changes (opening changes nothing).
    for (const auto& o : control::opensOf(again, target)) {
      const std::string track = o.trackId == "master" ? std::string() : o.trackId;
      if (!track.empty() && !history_.current().findTrack(track)) continue;
      openCoreWindow(o.what, track);
    }
    finishProposal(indexOf(item.id), "applied", {{"applied", lines}, {"warnings", again.warnings()}, {"problems", again.problems}});
  };
  if (!p.needsTrack()) { run(""); return; }
  juce::PopupMenu menu;
  menu.addSectionHeader(u8("Where to?"));
  std::vector<std::string> targets;
  const std::string first = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
  if (const doc::Track* t = history_.current().findTrack(first)) { targets.push_back(t->id); menu.addItem(1, u8(t->name) + u8("  (selected)")); menu.addSeparator(); }
  for (const auto& t : history_.current().tracks)
    if (t.id != first) { targets.push_back(t.id); menu.addItem((int)targets.size(), u8(t.name)); }
  const bool effectsOnly = std::all_of(p.calls.begin(), p.calls.end(), [](const control::Call& c) {
    return c.fn->family == "effect" || c.fn->family == "mix" || (c.fn->family == "open" && (c.open == "effects" || c.open == "mixer")) || !c.trackId.empty();
  });
  if (effectsOnly) { targets.push_back("master"); menu.addItem((int)targets.size(), u8("Master")); }
  juce::Component::SafePointer<MainComponent> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(button), [self, targets, run](int r) {
    if (self && r >= 1 && r <= (int)targets.size()) run(targets[(size_t)r - 1]);
  });
}

int MainComponent::indexOf(const std::string& proposalId) const {
  for (size_t i = 0; i < inbox_.size(); ++i) if (inbox_[i].id == proposalId) return (int)i;
  return -1;
}

// The end of a proposal: its result for AICORE, and it leaves the list.
void MainComponent::finishProposal(int index, const std::string& status, const json& result) {
  if (index < 0 || index >= (int)inbox_.size()) return;
  std::string why;
  if (!control::finishProposal(pathOf(bridgeFolder()), inbox_[(size_t)index], status, result, &why)) logLine("AI: " + u8(why));
  logLine("AI proposal " + u8(inbox_[(size_t)index].id) + ": " + u8(status));
  inbox_.erase(inbox_.begin() + index);
  inboxSeen_ = control::inboxSignature(pathOf(bridgeFolder()));
  updateAiBadge();
  if (proposals_ != nullptr) showProposals();
}

void MainComponent::aiBadgePressed(bool popup) {
  if (popup) { showAicoreMenu(aiBadge_.get()); return; }
  openAicore();
  if (aiAllowed_ && !inbox_.empty()) showProposals();
}

namespace { enum AicoreMenuId { kAicoreOpen = 2000, kAicoreProposals, kAicoreBridge, kAicoreChoose, kAicoreAllow }; }

// The same items in the AICORE menu (menu bar) and on the button's right-click.
static void addAicoreItems(juce::PopupMenu& menu, bool allowed, bool canSwitch, int waiting, const juce::String& state) {
  menu.addSectionHeader(state);
  menu.addItem(juce::PopupMenu::Item(u8("Open AICORE (chat)")).setID(kAicoreOpen));
  menu.addItem(juce::PopupMenu::Item(u8("Show AICORE's proposals") + (waiting > 0 ? " (" + juce::String(waiting) + ")" : juce::String()))
                   .setID(kAicoreProposals).setEnabled(allowed));
  menu.addSeparator();
  menu.addItem(juce::PopupMenu::Item(u8("Allow AICORE proposals")).setID(kAicoreAllow).setTicked(allowed).setEnabled(canSwitch));
  menu.addItem(juce::PopupMenu::Item(u8("Open the bridge folder (state and proposals)")).setID(kAicoreBridge));
  menu.addItem(juce::PopupMenu::Item(u8("Choose AICORE's folder…")).setID(kAicoreChoose));
}

static juce::String aicoreState(const control::AicoreStatus& st, bool allowed) {
  juce::String s = st.ready() ? u8("AICORE: chat open, connected") + (st.model.empty() ? juce::String() : " (" + u8(st.model) + ")")
                   : st.running ? u8("AICORE: open, not connected to an AI")
                                : u8("AICORE: not open");
  if (!allowed) s += u8(" · proposals off");
  return s;
}

void MainComponent::showAicoreMenu(juce::Component* target) {
  juce::PopupMenu menu;
  addAicoreItems(menu, aiAllowed_, realOutput_, aiAllowed_ ? (int)inbox_.size() : 0, aicoreState(aicore_, aiAllowed_));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),
                     [safe = juce::Component::SafePointer<MainComponent>(this)](int r) { if (safe && r != 0) safe->menuItemSelected(r, 0); });
}

// Where AICORE may be, in order: the folder chosen in the AICORE menu; installed with Core (<install>\addons\AICORE,
// beside the app-<version> folders, or beside Core.exe in a build); the runnable copy on Diogo's PC (D:\AICORE-data\app).
std::vector<std::filesystem::path> MainComponent::aicoreFolders() {
  std::vector<std::filesystem::path> out;
  const juce::String chosen = settings_.getUserSettings()->getValue("aicoreFolder");
  if (chosen.isNotEmpty()) out.push_back(pathOf(juce::File(chosen)));
  const juce::File exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
  out.push_back(pathOf(exeDir.getParentDirectory().getChildFile("addons").getChildFile("AICORE")));
  out.push_back(pathOf(exeDir.getChildFile("addons").getChildFile("AICORE")));
#if JUCE_WINDOWS
  out.push_back(pathOf(juce::File("D:\\AICORE-data\\app")));  // not D:\AI Dev: what runs from there cannot write AppData
#endif
  return out;
}

// Opens AICORE's chat: a running AICORE is asked to show it (aicore-command.json); otherwise Core starts AICORE as
// its addon.json says, on the chat page.
void MainComponent::openAicore() {
  if (aicore_.running || (aicoreProcess_ && aicoreProcess_->isRunning())) {  // running, or still starting: one AICORE
    std::string why;
    if (control::writeAicoreCommand(pathOf(bridgeFolder()), "assistant", &why)) logLine("AICORE: asked to show the chat");
    else showError(u8("AICORE"), u8(why));
    return;
  }
  const auto addon = control::findAddon(aicoreFolders());
  if (!addon) {
    showError(u8("AICORE"), u8("AICORE was not found. Choose its folder (the one with addon.json) in AICORE > Choose AICORE's folder…"));
    return;
  }
  if (!control::addonFits(*addon, CORE_VERSION)) {
    showError(u8("AICORE"), u8("This AICORE (") + u8(addon->version) + u8(") needs Core ") + u8(addon->minCore) + u8(" or newer; this Core is ") + CORE_VERSION + ".");
    return;
  }
  juce::StringArray cmd;
  cmd.add(u8(addon->program));
  for (const auto& a : control::addonArgs(*addon, "assistant")) cmd.add(u8(a));
  auto process = std::make_unique<juce::ChildProcess>();
  if (!process->start(cmd, 0)) {
    showError(u8("AICORE"), u8("AICORE could not be started: ") + cmd.joinIntoString(" "));
    return;
  }
  logLine("AICORE: started from " + juce::String(addon->folder.wstring().c_str()));
  aicoreProcess_ = std::move(process);
  notice_ = u8("Opening AICORE… (the first time takes a while)");
}

void MainComponent::chooseAicoreFolder() {
  const auto found = control::findAddon(aicoreFolders());
  const juce::File start = found ? juce::File(juce::String(found->folder.wstring().c_str())) : juce::File::getSpecialLocation(juce::File::userHomeDirectory);
  chooser_ = std::make_unique<juce::FileChooser>(u8("AICORE's folder (the one with addon.json)"), start);
  juce::Component::SafePointer<MainComponent> self(this);
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [self](const juce::FileChooser& fc) {
    if (!self) return;
    const juce::File dir = fc.getResult();
    if (dir == juce::File()) return;
    std::string why;
    if (!control::readAddon(pathOf(dir), &why)) { self->showError(u8("AICORE"), u8(why)); return; }
    self->settings_.getUserSettings()->setValue("aicoreFolder", dir.getFullPathName());
    logLine("AICORE folder: " + dir.getFullPathName());
  });
}

void MainComponent::updateAiBadge() {
  if (!aiBadge_) return;
  const int waiting = aiAllowed_ ? (int)inbox_.size() : 0;
  aiBadge_->setDot(aicore_.ready() ? kGreen : kRed);
  aiBadge_->setCount(waiting);
  juce::String tip = aicoreState(aicore_, aiAllowed_) + ". ";
  if (waiting > 0) tip += juce::String(waiting) + u8(" proposal(s) waiting. ");
  tip += u8("Click: open AICORE's chat") + (waiting > 0 ? u8(" and the proposals") : juce::String()) + u8(". Right-click: the AICORE menu.");
  aiBadge_->setTooltip(tip);
}

juce::File MainComponent::trainingClipsFolder() const {
  const juce::File chosen = folderSetting("folderTrainingClips");
  return chosen != juce::File() ? chosen
                                : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Core training clips");
}

void MainComponent::scanPlugins() {
  notice_ = u8("Scanning the plugins (each one is checked on its own)...");
  juce::Component::SafePointer<MainComponent> self(this);
  plugins_->scan([this, self](int found, int failed) {
    if (self == nullptr) return;
    notice_ = u8("Plugins: ") + juce::String(found) + u8(" found") +
              (failed ? u8(", ") + juce::String(failed) + u8(" with problems (set aside)") : juce::String()) + ".";
    if (showPlugins_) refreshInstalledCards();
    menuItemsChanged();
    repaint();
  });
}

// What the plugins hold now goes into the document (no new undo step): saving, exporting, before a change that
// replaces a plugin (so Undo brings it back as it sounded). False when the engine gave no answer.
bool MainComponent::fetchPluginStates() {
  if (!doc::hasPlugins(history_.current())) return true;
  const json states = session_->pluginStates(5000);
  if (!states.is_object()) return false;  // no answer from the engine ({} is an answer: nothing to take)
  doc::Document withStates = history_.current();
  if (doc::applyPluginStates(withStates, states) > 0) {
    history_.amend(withStates);
    session_->setDocument(engineDocument());  // the same plugins: the engine keeps them as they are
  }
  return true;
}

MainComponent::SaveResult MainComponent::writeTo(const juce::File& target) {
  // The plugins' sound goes into the file first (the engine's copy is never the only one).
  const bool pluginsOk = fetchPluginStates();
  const std::filesystem::path before = projectDir();
  try {
    if (!before.empty() && !history_.current().assets.empty())
      doc::copyAssets(history_.current(), before, pathOf(target.getParentDirectory()));
    doc::saveFile(history_.current(), pathOf(target));
  } catch (const std::exception& e) {
    showError(u8("Could not save"), u8(e.what()) + u8("\n\nThe previous file was not changed."));
    return SaveResult::Failed;
  }
  file_ = target;
  imported_ = false;
  history_.markSaved();
  // Without the plugins' answer the file has their settings from the last save: what they hold now is still
  // unsaved, so the song stays modified and closing asks again (audit R01: it was marked saved and closed).
  pluginsDirty_ = !pluginsOk;
  logLine("Saved " + target.getFullPathName() + (pluginsOk ? "" : " (without the plugins' current settings)"));
  recovered_ = false;
  if (!unsaved()) removeOwnAutosave();  // saved: the autosave is not needed any more
  if (projectDir() != before) projectFolderChanged();
  if (!pluginsOk) {
    showError(u8("Plugins"), u8("The song was saved, but the engine did not give the plugins' current settings in time: "
                                "the file keeps the ones from the last save. Save again before closing."));
    return SaveResult::SavedWithoutPlugins;
  }
  return SaveResult::Saved;
}

void MainComponent::save(bool chooseName, std::function<void(SaveResult)> then) {
  if (!chooseName && !imported_ && file_ != juce::File()) {
    const SaveResult r = writeTo(file_);
    if (then) then(r);
    return;
  }
  const juce::String name = history_.current().name.empty() ? juce::String("Song") : u8(history_.current().name);
  const juce::File start = file_ != juce::File() && !imported_ ? file_
      : songsFolder().getChildFile(juce::File::createLegalFileName(name) + doc::kFileExtension);
  chooser_ = std::make_unique<juce::FileChooser>(u8("Save song"), start, "*.core");
  chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [this, then](const juce::FileChooser& fc) {
                          const juce::File f = fc.getResult();
                          if (f == juce::File()) { if (then) then(SaveResult::Failed); return; }
                          const SaveResult r = writeTo(f.withFileExtension(doc::kFileExtension));
                          if (then) then(r);
                        });
}

void MainComponent::open() {
  confirmUnsaved([this] {
    chooser_ = std::make_unique<juce::FileChooser>(u8("Open song"), file_ != juce::File() ? file_.getParentDirectory() : songsFolder(), "*.core;*.json");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
      const juce::File f = fc.getResult();
      if (f == juce::File()) return;
      try {
        doc::OpenResult r = doc::openFile(pathOf(f));
        logLine((r.imported ? "Imported " : "Opened ") + f.getFullPathName());
        // Samples whose library file moved are found again by content: the song then has unsaved changes.
        doc::Document moved = r.document;
        const int relinked = hasSampleIndex_ ? doc::relinkMovedSamples(moved, sampleIndex_) : 0;
        history_.reset(std::move(r.document));
        if (relinked) history_.amend(moved);
        selectedTrack_.clear();
        selectedClip_.clear();
        file_ = r.imported ? juce::File() : f;
        imported_ = r.imported;
        loadProblem_.clear();
        viewStart_ = viewLength_ = 0;
        recovered_ = false;
        session_->newSong(); session_->releasePlugins();
        missing_.clear();
        session_->setAssetRoot(projectDir(), false);
        documentLoaded();
        documentChanged(true);
        notice_ = r.imported ? u8("Imported from ") + f.getFileName() + u8(": save it under a new name (the original is not changed).")
                             : juce::String();
        if (relinked) notice_ = juce::String(relinked) + u8(" sample(s) found in their new place in the library: save to keep it.");
        checkMissingAssets();
      } catch (const doc::FutureVersionError&) {
        showError(u8("Newer version"), f.getFileName() + u8(" was saved by a newer version of Core. It was not opened or changed."));
      } catch (const std::exception& e) {
        showError(u8("Could not open"), f.getFileName() + ": " + u8(e.what()));
      }
    });
  });
}

void MainComponent::newProject(bool fromTemplate) {
  newFromDocument(fromTemplate ? doc::newTemplateDocument() : doc::newEmptyDocument());
}

void MainComponent::newFromDocument(doc::Document fresh, std::function<void()> then) {
  confirmUnsaved([this, fresh = std::move(fresh), then] {
    history_.reset(fresh);
    selectedTrack_.clear();
    selectedClip_.clear();
    file_ = juce::File();
    imported_ = true;  // never saved: Save asks for a name
    loadProblem_.clear();
    viewStart_ = viewLength_ = 0;
    recovered_ = false;
    session_->newSong(); session_->releasePlugins();
    notice_.clear();
    missing_.clear();
    session_->home();
    session_->setAssetRoot({}, false);
    documentLoaded();
    documentChanged(true);
    if (then) then();
  });
}

juce::File MainComponent::builtinTemplatesFolder() const {
  return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("templates");
}

juce::File MainComponent::userTemplatesFolder() const {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("Templates");
}

// Reads the template files again only when one of them (or hidden.json) changed: the File menu asks
// for the list each time it opens.
void MainComponent::reloadTemplates() {
  juce::String stamp;
  for (const auto& folder : {builtinTemplatesFolder(), userTemplatesFolder()})
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, juce::String("*") + doc::kTemplateExtension))
      stamp << f.getFullPathName() << ':' << f.getSize() << ':' << f.getLastModificationTime().toMilliseconds() << ';';
  const juce::File hidden = userTemplatesFolder().getChildFile("hidden.json");  // built-in templates the user deleted
  stamp << hidden.getSize() << ':' << hidden.getLastModificationTime().toMilliseconds();
  if (stamp == templatesStamp_ && templatesLoaded_) return;
  templates_ = doc::loadTemplates(builtinTemplatesFolder().getFullPathName().toWideCharPointer(),
                                  userTemplatesFolder().getFullPathName().toWideCharPointer());
  templatesStamp_ = stamp;
  templatesLoaded_ = true;
}

void MainComponent::newFromTemplate(const std::string& templateId) {
  reloadTemplates();  // the file may have changed since the menu was shown
  const doc::TemplateEntry* entry = templates_.find(templateId);
  if (!entry) return showError("Template not found", u8(templateId) + " is no longer in the templates folders.");
  // The template's own sounds first (the song as made), then the library's samples as one undo step:
  // Undo gives the template's sounds back.
  // Its samples found in the library where it is now (a template written when the library was D:\Samples).
  doc::Document song = doc::documentFromTemplate(entry->tmpl);
  if (hasSampleIndex_) doc::relinkMovedSamples(song, sampleIndex_);
  newFromDocument(std::move(song), [this] {
    if (hasSampleIndex_ && settings_.getUserSettings()->getBoolValue("suggestSamples", true)) useSuggestedSamples(true, true);
  });
}

// ---------------------------------------------------------------- sample library

juce::File MainComponent::sampleLibraryFolder() const {
  const juce::String chosen = const_cast<juce::ApplicationProperties&>(settings_).getUserSettings()->getValue("sampleLibrary");
  if (chosen.isNotEmpty()) return juce::File(chosen);
  // The library Core Setup installed beside the versions (<install>\\app-<version>\\Core.exe -> <install>\\Samples).
  const juce::File installed = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getSiblingFile("Samples");
  if (installed.isDirectory()) return installed;
  const juce::File usual("D:\\Samples");  // where Diogo keeps them
  return usual.isDirectory() ? usual : juce::File();
}

juce::File MainComponent::sampleIndexFile() const {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("samples-index.json");
}

void MainComponent::loadSampleIndex() {
  const juce::File f = sampleIndexFile();
  if (!f.existsAsFile()) return;
  try {
    doc::SampleIndex index = doc::indexFromJson(nlohmann::json::parse(f.loadFileAsString().toStdString()));
    if (juce::File(u8(index.root)) != sampleLibraryFolder()) {  // another folder was chosen since: its catalogue stays (by content)
      sampleIndex_.overrides = std::move(index.overrides);
      return;
    }
    sampleIndex_ = std::move(index);
    hasSampleIndex_ = true;
    applySamplerFolders();
  } catch (const std::exception& e) {
    logLine(u8("Sample index not read (it is made again): ") + u8(e.what()));
  }
}

// Reads the library on a thread of its own (files that did not change keep what the last scan read);
// the window goes on working and takes the result in collectSampleScan().
void MainComponent::scanSampleLibrary() {
  const juce::File folder = sampleLibraryFolder();
  if (!folder.isDirectory() || (sampleScan_ && sampleScan_->state.load() == 0)) return;
  auto scan = std::make_shared<SampleScan>();
  sampleScan_ = scan;
  std::shared_ptr<const doc::SampleIndex> previous = hasSampleIndex_ ? std::make_shared<const doc::SampleIndex>(sampleIndex_) : nullptr;
  const std::filesystem::path root = pathOf(folder);
  std::thread([scan, previous, root] {
    try {
      scan->index = doc::scanLibrary(root, previous.get(), [&](int done, int total) {
        scan->done = done;
        scan->total = total;
        return !scan->stop.load();
      });
      scan->state = 1;
    } catch (const std::exception& e) {
      scan->error = e.what();
      scan->state = 2;
    }
  }).detach();
}

void MainComponent::collectSampleScan() {
  if (!sampleScan_ || sampleScan_->state.load() == 0) return;
  const auto scan = std::move(sampleScan_);
  if (scan->state.load() == 2) {
    logLine(u8("Sample library not scanned: ") + u8(scan->error));
    notice_ = u8("Sample library not scanned: ") + u8(scan->error);
    return;
  }
  const size_t before = hasSampleIndex_ ? sampleIndex_.samples.size() : 0;
  // The catalogue as it is now: edits made while the scan ran are kept.
  doc::SampleIndex next = std::move(scan->index);
  next.overrides = std::move(sampleIndex_.overrides);
  next.undo = std::move(sampleIndex_.undo);
  doc::resolveCatalog(next);
  sampleIndex_ = std::move(next);
  hasSampleIndex_ = true;
  applySamplerFolders();
  saveSampleIndex();
  for (const auto& p : sampleIndex_.sidecarProblems) logLine(u8("Sample metadata file: ") + u8(p));
  startSampleAnalysis();
  logLine(u8("Sample library: ") + juce::String((int)sampleIndex_.samples.size()) + u8(" samples in ") + u8(sampleIndex_.root));
  if (sampleIndex_.samples.size() != before)
    notice_ = u8("Sample library: ") + juce::String((int)sampleIndex_.samples.size()) + u8(" samples (Samples menu).");
}

// Library > Project > Remove unused: the song's audio files no clip plays leave the song (one undo step), and
// their files and the stray ones in the Audio folder go to the Recycle Bin (from where they can be brought back).
void MainComponent::removeUnusedAudio() {
  const std::filesystem::path dir = projectDir();
  const auto unused = doc::unusedAssets(history_.current());
  const auto stray = doc::strayAudioFiles(history_.current(), dir);
  if (unused.empty() && stray.empty()) return;
  juce::String text = juce::String((int)(unused.size() + stray.size())) + u8(" audio file(s) of this song are not played by any clip.\n\n") +
                      u8("They leave the song (Edit > Undo puts them back in the song) and their files go to the Recycle Bin, ") +
                      u8("from where they can be restored. Files Undo can still bring back stay until the song is closed. Save the song afterwards.");
  juce::Component::SafePointer<MainComponent> safe(this);
  juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
                                   .withTitle(u8("Remove unused audio files")).withMessage(text)
                                   .withButton("Remove", 1).withButton("Cancel", 0),
                               [safe, dir](int result) {
    if (result != 1 || safe == nullptr) return;
    MainComponent& o = *safe;
    // Again now: the song may have changed while the question was open.
    const auto strayNow = doc::strayAudioFiles(o.history_.current(), dir);
    std::vector<doc::Asset> removed;
    if (!doc::unusedAssets(o.history_.current()).empty())
      o.edit([&](doc::Document& d) { removed = doc::removeUnusedAssets(d); }, u8("Remove unused audio files"));
    int trashed = 0, failed = 0;
    auto toTrash = [&](const std::filesystem::path& p) {
      const juce::File f = fileOf(p);
      if (!f.existsAsFile()) return;
      if (f.moveToTrash()) ++trashed; else ++failed;
    };
    // Only the files no Undo or Redo step names (Codex review 2026-11-09 C4): Undo must find its audio. The others
    // stay until the song is closed; Remove unused finds them as stray files the next time.
    std::vector<std::filesystem::path> candidates;
    for (const auto& a : removed) candidates.push_back(doc::assetFile(a, dir));
    for (const auto& p : strayNow) candidates.push_back(p);
    const auto trashable = doc::filesNoStateNeeds(o.history_, candidates, dir);
    for (const auto& p : trashable) toTrash(p);
    const int kept = (int)(candidates.size() - trashable.size());
    o.notice_ = juce::String(trashed) + u8(" unused audio file(s) sent to the Recycle Bin") +
                (failed > 0 ? u8(" (") + juce::String(failed) + u8(" could not be moved: in use?)") : juce::String()) +
                (kept > 0 ? u8("; ") + juce::String(kept) + u8(" kept while Undo can bring them back") : juce::String()) + u8(".");
    logLine(o.notice_);
    o.repaint();
  });
}

void MainComponent::startSampleAnalysis() {
  if (analysisJob_) analysisJob_->stop = true;  // an older list: replaced
  analysisJob_.reset();
  std::vector<std::pair<std::filesystem::path, std::string>> todo;
  for (const auto& x : sampleIndex_.samples) if (doc::needsAnalysis(x)) todo.emplace_back(pathOf(juce::File(u8(x.file))), x.sha256);
  if (todo.empty()) return;
  auto job = std::make_shared<SampleAnalysisJob>();
  job->total = (int)todo.size();
  analysisJob_ = job;
  std::thread([job, todo = std::move(todo)] {
    for (const auto& [file, sha] : todo) {
      while (job->pause.load() && !job->stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (job->stop.load()) return;
      try {
        nlohmann::json result = doc::analysisToJson(doc::analyzeSample(file, sha, &job->stop));
        std::lock_guard<std::mutex> hold(job->lock);
        job->results.emplace_back(sha, std::move(result));
      } catch (const std::exception&) {
        // Stopped, or the file changed or went away since the scan: the next scan sees it again.
      }
      ++job->done;
    }
    job->finished = true;
  }).detach();
}

void MainComponent::collectSampleAnalysis() {
  if (!analysisJob_) return;
  analysisJob_->pause = snap_.playing;  // the song comes first
  std::vector<std::pair<std::string, nlohmann::json>> results;
  {
    std::lock_guard<std::mutex> hold(analysisJob_->lock);
    results.swap(analysisJob_->results);
  }
  for (auto& [sha, result] : results)
    if (doc::LibrarySample* x = sampleIndex_.findHash(sha); x && doc::needsAnalysis(*x)) {
      x->analysis = std::move(result);
      ++analysedSinceSave_;
    }
  const bool finished = analysisJob_->finished.load();
  if (analysedSinceSave_ > 0 && (finished || analysedSinceSave_ >= 50)) {
    analysedSinceSave_ = 0;
    doc::resolveCatalog(sampleIndex_);  // the names use the measurements (Short, Long)
    saveSampleIndex();
    library_.catalogChanged();
  }
  if (finished) {
    logLine(u8("Sample library: ") + juce::String(analysisJob_->done.load()) + u8(" sounds measured"));
    analysisJob_.reset();
  }
}

void MainComponent::saveSampleIndex() {
  try {
    sampleIndexFile().getParentDirectory().createDirectory();
    // replaceWithText writes a temporary file and then swaps it in: a failure leaves the old index readable.
    if (!sampleIndexFile().replaceWithText(u8(doc::indexToJson(sampleIndex_).dump(1))))
      logLine(u8("Sample index not saved: could not write ") + sampleIndexFile().getFullPathName());
  } catch (const std::exception& e) {
    logLine(u8("Sample index not saved: ") + u8(e.what()));
  }
}

void MainComponent::sampleCatalogChanged() {
  saveSampleIndex();
  library_.catalogChanged();
  repaint();
}

void MainComponent::chooseSampleLibrary() {
  chooser_ = std::make_unique<juce::FileChooser>(u8("Sample library folder"), sampleLibraryFolder());
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this](const juce::FileChooser& fc) {
    setSampleLibrary(fc.getResult());
  });
}

void MainComponent::setSampleLibrary(const juce::File& folder) {
    if (folder == juce::File() || !folder.isDirectory()) return;
    settings_.getUserSettings()->setValue("sampleLibrary", folder.getFullPathName());
    settings_.saveIfNeeded();
    if (sampleScan_) sampleScan_->stop = true;  // a scan of the old folder: its result is dropped
    if (analysisJob_) analysisJob_->stop = true;
    analysisJob_.reset();
    sampleScan_.reset();
    hasSampleIndex_ = false;
    doc::SampleIndex empty;  // the user's catalogue is about contents: it goes on to the new folder
    empty.overrides = std::move(sampleIndex_.overrides);
    empty.undo = std::move(sampleIndex_.undo);
    sampleIndex_ = std::move(empty);
    applySamplerFolders();
    scanSampleLibrary();
    notice_ = u8("Scanning the sample library: ") + folder.getFullPathName();
    repaint();
}

// ---------------------------------------------------------------- File > Settings: default folders

juce::File MainComponent::folderSetting(const char* key) const {
  const juce::String chosen = const_cast<juce::ApplicationProperties&>(settings_).getUserSettings()->getValue(key).trim();
  if (chosen.isEmpty() || !juce::File::isAbsolutePath(chosen)) return {};
  return juce::File(chosen);
}

juce::File MainComponent::songsFolder() const {
  const juce::File chosen = folderSetting("folderSongs");
  if (chosen.isDirectory()) return chosen;
  return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

juce::File MainComponent::exportFolder() const {
  const juce::File chosen = folderSetting("folderExports");
  if (chosen.isDirectory() || (chosen != juce::File() && chosen.createDirectory())) return chosen;
  if (file_ != juce::File()) return file_.getParentDirectory();
  return juce::File::getSpecialLocation(juce::File::userMusicDirectory);
}

// Randomize the open song inside its style (domain randomizeStyle), as ONE undo step with a new seed; the
// notice lists what changed per section and the seed (the AI's randomize_style gives the same result).
void MainComponent::randomizeSong(int strength, const std::string& axis) {
  doc::Document next = history_.current();
  doc::RandomizeOptions o;
  o.seed = (uint64_t)(juce::Random::getSystemRandom().nextInt64() & 0x1FFFFFFFFFFFFFLL) | 1;  // fits a JSON number exactly
  o.strength = strength;
  if (!axis.empty()) o.axes = {axis};  // tempo (J) or sound (K) alone: the groove stays
  std::vector<std::string> changes;
  try {
    changes = doc::randomizeStyle(next, o);
  } catch (const std::exception& e) {
    return showError(u8("Randomize"), u8(e.what()));
  }
  if (changes.empty()) {
    notice_ = u8("Randomize changed nothing this time: try again or a stronger one.");
    repaint();
    return;
  }
  edit([&](doc::Document& d) { d = next; }, u8(axis.empty() ? std::string("Randomize (seed ") : "Randomize " + axis + " (seed ") + juce::String((juce::int64)o.seed) + ")");
  juce::StringArray list;
  for (const auto& c : changes) list.add(u8(c));
  notice_ = u8("Randomized (Undo to go back): ") + list.joinIntoString("; ");
  logLine(notice_);
  repaint();
}

// A sample of the library for every track that suits one, as ONE undo step: the best ones, or (shuffle,
// and every new song from a template) a varied good pick, so two songs from one template differ. Tracks
// with no good match keep their sound; nothing is replaced in silence (the notice lists every change).
void MainComponent::useSuggestedSamples(bool fromTemplate, bool shuffle) {
  if (!hasSampleIndex_) {
    if (!fromTemplate) showError(u8("Sample library"), u8("There is no sample library yet: Samples > Choose sample library folder…"));
    return;
  }
  auto apply = [this, fromTemplate, shuffle] {
    doc::Document next = history_.current();
    std::vector<std::string> changed;
    try {
      const uint64_t seed = shuffle ? ((uint64_t)juce::Random::getSystemRandom().nextInt64() | 1) : 0;
      changed = doc::applySuggestedSamples(next, sampleIndex_, doc::songStyle(next), seed);
    } catch (const std::exception& e) {
      return showError(u8("Samples"), u8(e.what()));
    }
    if (changed.empty()) {
      notice_ = u8("No sample in the library suits these tracks: they keep their sounds.");
      repaint();
      return;
    }
    edit([&](doc::Document& d) { d = next; }, shuffle && !fromTemplate ? u8("Shuffle samples") : u8("Suggested samples"));
    juce::StringArray list;
    for (const auto& c : changed) list.add(u8(c));
    notice_ = u8("Samples from your library (Undo for the template's sounds): ") + list.joinIntoString(", ");
    logLine(notice_);
    repaint();
  };
  apply();
}

void MainComponent::putSampleOnTrack(const std::string& trackId, const doc::LibrarySample& sample) {
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!t) return;
  // Trying sounds one after another on the same track is one undo step, not one per sound: the last step,
  // when it was the previous try on this track, is replaced.
  const juce::String label = u8("Sample: ") + u8(doc::shownName(sample)) + u8(" on ") + u8(t->name);
  if (trackId == lastSampleTrack_ && history_.canUndo() && !history_.canRedo() && u8(history_.undoLabel()) == lastSampleLabel_) {
    bool fits = true;
    try { doc::Document trial = history_.current(); doc::setTrackSample(trial, trackId, sampleIndex_, sample); } catch (const std::exception&) { fits = false; }
    if (fits) history_.undo();  // (when the new sound cannot go there, the error shows and the last try stays)
  }
  edit([&](doc::Document& d) { doc::setTrackSample(d, trackId, sampleIndex_, sample); }, label);
  lastSampleTrack_ = trackId;
  lastSampleLabel_ = label;
  auditionTrack(trackId);
}

// Any WAV: one of the library is taken from the index (its siblings make a bass note by note); another
// one is read now.
void MainComponent::chooseSampleFile(const std::string& trackId) {
  const juce::File start = sampleLibraryFolder();
  chooser_ = std::make_unique<juce::FileChooser>(u8("Sample (WAV)"), start, "*.wav");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, trackId](const juce::FileChooser& fc) {
    const juce::File f = fc.getResult();
    if (f != juce::File()) useSampleFile(trackId, f);
  });
}

// Plays the track's first note once (after the engine has the new sound), so a swap is heard at once.
void MainComponent::auditionTrack(const std::string& trackId) {
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!t) return;
  int pitch = 36;
  for (const auto& p : t->patterns) if (!p.notes.empty()) { pitch = p.notes.front().pitch; break; }
  juce::Component::SafePointer<MainComponent> safe(this);
  juce::Timer::callAfterDelay(200, [safe, trackId, pitch] {
    if (safe == nullptr) return;
    safe->session_->liveNote(trackId, pitch, 0.9, true);
    juce::Timer::callAfterDelay(400, [safe, trackId, pitch] { if (safe != nullptr) safe->session_->liveNote(trackId, pitch, 0, false); });
  });
}

// The song as it is now, or a passage of it (the loop or one section), becomes a template in the user's folder.
// Saving under the id of a built-in template makes the user's edited copy, which then takes its place in the
// Library's Templates tab. The template carries its sheet ("ficha": every sample and parameter by name, value and
// unit, written by saveTemplate); when asked, the same passage goes next to it as a mono WAV (<id>.wav): examples
// for the AI (Diogo 2026-11-09).
void MainComponent::saveAsTemplate() {
  reloadTemplates();
  const doc::Document& d = history_.current();
  const doc::TemplateEntry* origin = nullptr;
  if (d.extra.contains("template") && d.extra["template"].is_object())
    origin = templates_.find(d.extra["template"].value("id", std::string()));
  auto* w = new juce::AlertWindow(lang::tr("Save as template"), lang::tr("Saved in your templates folder. Use the name of an existing template to replace it."),
                                  juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", origin ? u8(origin->tmpl.info.name) : u8(d.name), "Name:");
  w->addTextEditor("style", origin ? u8(origin->tmpl.info.style) : juce::String(), "Style:");
  w->addTextEditor("description", origin ? u8(origin->tmpl.info.description) : juce::String(), "Description:");
  // What goes in: the whole song, the loop, or one section ([start, end) in ticks; end -1 = the song's end).
  struct Part { juce::String label; int64_t start, end; };
  std::vector<Part> parts{{u8("Whole song"), 0, -1}};
  auto barsOf = [](int64_t a, int64_t b) {
    return " (bars " + juce::String(a / kBar + 1) + "-" + juce::String((b + kBar - 1) / kBar) + ")";
  };
  if (d.loop.endTick - d.loop.startTick >= doc::kPpq && d.loop.endTick <= d.lengthTicks)
    parts.push_back({u8("Loop") + barsOf(d.loop.startTick, d.loop.endTick), d.loop.startTick, d.loop.endTick});
  for (const auto& sec : d.sections) {
    const int64_t end = std::min(d.lengthTicks, sec.startTick + sec.lengthTicks);
    if (end - sec.startTick >= doc::kPpq) parts.push_back({u8("Section: ") + u8(sec.name) + barsOf(sec.startTick, end), sec.startTick, end});
  }
  juce::StringArray partNames;
  for (const auto& p : parts) partNames.add(p.label);
  w->addComboBox("part", partNames, "Part:");
  w->getComboBoxComponent("part")->setSelectedItemIndex(0);
  w->addComboBox("audio", {u8("Template"), u8("Template and WAV (mono)")}, "Files:");
  w->getComboBoxComponent("audio")->setSelectedItemIndex(0);
  w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> safe(w);
  w->enterModalState(true, juce::ModalCallbackFunction::create([this, safe, parts](int result) {
    if (result != 1 || safe == nullptr) return;
    const Part part = parts[(size_t)std::clamp(safe->getComboBoxComponent("part")->getSelectedItemIndex(), 0, (int)parts.size() - 1)];
    const bool wav = safe->getComboBoxComponent("audio")->getSelectedItemIndex() == 1;
    doc::TemplateInfo info;
    info.name = safe->getTextEditorContents("name").trim().toStdString();
    info.style = safe->getTextEditorContents("style").trim().toStdString();
    info.description = safe->getTextEditorContents("description").trim().toStdString();
    if (info.name.empty()) return showError("Template name", "The template needs a name.");
    info.id = doc::templateIdFromName(info.name);
    info.source = {{"kind", "user"}};
    for (const auto& e : templates_.templates)  // same name as an existing one: replace that one (keeps its id)
      if (juce::String(u8(e.tmpl.info.name)).equalsIgnoreCase(u8(info.name))) info.id = e.tmpl.info.id;
    auto write = [this, info, part, wav] {
      // The plugins as they sound now, not as last saved (audit R03); no answer: nothing is written.
      if (!fetchPluginStates())
        return showError("Template not saved", "The engine did not give the plugins' current settings in time. Try again.");
      try {
        const doc::Document& song = history_.current();
        const doc::SongTemplate made = part.end < 0 ? doc::templateFromDocument(song, info)
                                                    : doc::templateFromRange(song, info, part.start, part.end);
        const auto file = doc::saveTemplate(made, templates_, userTemplatesFolder().getFullPathName().toWideCharPointer());
        // The WAV is the same passage of the song, mono, next to the template.
        const juce::File savedFile(juce::String(file.wstring().c_str()));
        const std::string base = savedFile.getFullPathName().toStdString();
        if (wav) {
          if (snap_.exportJob.active()) showError("WAV not exported", "Another export is running; the template is saved.");
          else {
            stopPatternSlot();  // the engine plays the song again, not the Patterns preview (audit R04)
            session_->exportFiles({{doc::templateCompanionPath(base, ".wav"), "", false, true}}, part.end < 0 ? 0 : part.start, part.end, 24);
          }
        }
        reloadTemplates();
        library_.templatesChanged();
        const doc::TemplateEntry* saved = templates_.find(info.id);
        notice_ = "Template saved: " + u8(info.name) + (saved ? " (version " + juce::String(saved->tmpl.info.version) + ")" : juce::String()) +
                  " in " + juce::String(file.wstring().c_str());
        repaint();
      } catch (const std::exception& e) {
        showError("Template not saved", u8(e.what()));
      }
    
    };
    const doc::TemplateEntry* existing = templates_.find(info.id);
    if (!existing) return write();
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Replace template")
            .withMessage("\"" + u8(existing->tmpl.info.name) + "\" (version " + juce::String(existing->tmpl.info.version) + ") will be replaced by this song" +
                         (existing->builtin ? juce::String(". The built-in file stays as it is: your copy is used instead.") : juce::String(". The previous file is kept as .bak.")))
            .withButton("Replace", 1).withButton("Cancel", 0),
        [write](int r) { if (r == 1) write(); });
  }), true);
}

// Takes a template out of the Library's Templates tab after asking: the user's file is kept as .bak, a built-in
// one is only hidden (right click > Bring back the built-in templates shows it again).
void MainComponent::restoreBuiltinTemplates() {
  try {
    const int n = doc::restoreBuiltinTemplates(userTemplatesFolder().getFullPathName().toWideCharPointer());
    notice_ = n ? juce::String(n) + " built-in template(s) are back in Templates." : juce::String("No built-in template was hidden.");
    reloadTemplates();
    library_.templatesChanged();
  } catch (const std::exception& e) {
    showError("Templates", u8(e.what()));
  }
  repaint();
}

// Ctrl+Shift+N: the Library opens on its Templates tab.
void MainComponent::showTemplates() {
  if (mixerOpen_) setMixerOpen(false);
  if (!showLibrary_) toggleSidePanel(true);
  library_.showTemplates();
}

void MainComponent::deleteTemplate(const std::string& templateId) {
  reloadTemplates();
  const doc::TemplateEntry* entry = templates_.find(templateId);
  if (!entry) return;
  const juce::String name = u8(entry->tmpl.info.name);
  const juce::String what = entry->builtin ? juce::String("It is hidden; right click in Templates > Bring back the built-in templates shows it again.")
                                           : juce::String("Your file is kept as .bak in the templates folder.");
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon)
          .withTitle("Delete template")
          .withMessage("Delete the template \"" + name + "\"? " + what)
          .withButton("Delete", 1).withButton("Cancel", 0),
      [this, templateId, name](int r) {
        if (r != 1) return;
        try {
          reloadTemplates();
          doc::deleteTemplate(templates_, templateId, userTemplatesFolder().getFullPathName().toWideCharPointer());
          notice_ = "Template deleted: " + name;
          library_.templatesChanged();
        } catch (const std::exception& e) {
          showError("Template not deleted", u8(e.what()));
        }
        repaint();
      });
}

// New tracks are played by Core's own instruments (CORE-ONLY, Diogo 2026-11-05): the kick, the bass (W-Core with its
// start bass), W-Core, the hats, the snare and the sampler.
void MainComponent::showAddTrackMenu() {
  juce::PopupMenu menu;
  // The plugins of this PC first (Diogo 2026-11-16: the sound comes from plugins), by maker.
  const std::vector<nlohmann::json> instruments = plugins_->instruments();
  menu.addSectionHeader(u8("Plugin instruments"));
  {
    std::map<std::string, juce::PopupMenu> byVendor;
    for (size_t i = 0; i < instruments.size(); ++i) {
      std::string vendor = instruments[i].value("vendor", std::string());
      if (vendor.empty()) vendor = "Other";
      byVendor[vendor].addItem(2000 + (int)i, u8(instruments[i].value("name", std::string("Plugin"))));
    }
    for (auto& [vendor, sub] : byVendor) menu.addSubMenu(u8(vendor), sub);
    if (instruments.empty()) menu.addItem(juce::PopupMenu::Item(u8("None found yet: Plugins menu > Scan plugins")).setID(99999).setEnabled(false));
  }
  menu.addSeparator();
  const std::vector<doc::InstrumentChoice>& native = doc::builtinInstruments();
  menu.addSectionHeader(u8("Core's own instruments"));
  for (size_t i = 0; i < native.size(); ++i)
    if (native[i].name != std::string("sampler")) menu.addItem(3000 + (int)i, u8(native[i].label));
  menu.addItem(1002, u8("Sampler track (empty)"));
  menu.addSeparator();
  menu.addItem(1000, u8("Audio track (empty)"));
  menu.addItem(1001, u8("Import audio (WAV)…"));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addTrack_), [this, instruments](int chosen) {
    if (chosen >= 2000 && chosen - 2000 < (int)instruments.size()) {
      const nlohmann::json plugin = instruments[(size_t)chosen - 2000];
      std::string id;
      edit([&](doc::Document& d) { id = doc::addPluginTrack(d, plugin); }, u8("Add ") + u8(plugin.value("name", std::string("plugin"))));
      if (!id.empty()) { selectTrack(id); session_->openPluginEditor(doc::pluginKey(id)); }
      return;
    }
    const std::vector<doc::InstrumentChoice>& list = doc::builtinInstruments();
    if (chosen >= 3000 && chosen - 3000 < (int)list.size()) {
      const doc::InstrumentChoice choice = list[(size_t)chosen - 3000];
      std::string id;
      edit([&](doc::Document& d) { id = doc::addTrack(d, choice.name); }, u8("Add ") + u8(choice.label));
      if (!id.empty()) selectTrack(id);
      return;
    }
    if (chosen == 1002) {
      std::string id;
      edit([&](doc::Document& d) { id = doc::addTrack(d, "sampler"); }, u8("Sampler track"));
      if (!id.empty()) selectTrack(id);
      return;
    }
    if (chosen == 1000) { edit([](doc::Document& d) { doc::addAudioTrack(d, "Audio"); }, u8("Audio track")); return; }
    if (chosen == 1001) { importAudio(); return; }
  });
}

void MainComponent::showTrackMenu(const std::string& trackId) {
  // Library samples offered in the Sample submenu: kOfferedSampleId + their place (a plain Sampler track offers
  // up to 12 kinds x 12, past the menu's other ids).
  static constexpr int kOfferedSampleId = 5000;
  juce::PopupMenu menu;
  menu.addItem(1, u8("Rename…"));
  menu.addItem(2, "Duplicate");
  menu.addItem(4, u8("Automation (volume, pan, effects)…"));
  // Another instrument for this track (its notes, effects, volume and automation stay).
  const doc::Track* current = history_.current().findTrack(trackId);
  // Core's own instruments (ids 800 + place in builtinInstruments()).
  juce::PopupMenu replace;
  const std::vector<nlohmann::json> instruments = plugins_->instruments(), pluginFx = plugins_->effects();
  auto byVendor = [](const std::vector<nlohmann::json>& list, int firstId, const std::function<bool(const nlohmann::json&)>& ticked) {
    std::map<std::string, juce::PopupMenu> groups;
    for (size_t i = 0; i < list.size(); ++i) {
      std::string vendor = list[i].value("vendor", std::string());
      if (vendor.empty()) vendor = "Other";
      groups[vendor].addItem(juce::PopupMenu::Item(u8(list[i].value("name", std::string("Plugin")))).setID(firstId + (int)i).setTicked(ticked(list[i])));
    }
    juce::PopupMenu out;
    for (auto& [vendor, sub] : groups) out.addSubMenu(u8(vendor), sub);
    if (list.empty()) out.addItem(juce::PopupMenu::Item(u8("None found yet: Plugins menu > Scan plugins")).setID(99999).setEnabled(false));
    return out;
  };
  // The track's plugins: its instrument's window, and each plugin effect's window (ids 900 + its place in the chain).
  if (current && current->instrument.kind == "plugin")
    menu.addItem(juce::PopupMenu::Item(u8("Open ") + u8(current->instrument.plugin.value("name", std::string("the plugin"))) + u8("…"))
                     .setID(6).setEnabled(!snap_.missingPlugins.count(doc::pluginKey(trackId))));
  if (current) {
    juce::PopupMenu fxMenu;
    // Every effect of the chain is listed, Core's own ones included (an old song or a template carries
    // them): each gets On and Remove, so nothing in a chain is stuck (bug 2026-11-17). A window opens
    // for a plugin only.
    for (const auto& entry : doc::effectMenuEntries(current->effects)) {
      const int i = entry.index;
      juce::PopupMenu one;
      if (entry.window) {
        const std::string key = doc::pluginKey(trackId, current->effects[(size_t)i].value("id", std::string()));
        one.addItem(juce::PopupMenu::Item(u8("Open…")).setID(900 + i).setEnabled(!snap_.missingPlugins.count(key)));
      }
      one.addItem(juce::PopupMenu::Item(u8("On")).setID(940 + i).setTicked(!doc::effectBypassed(current->effects[(size_t)i])));
      one.addItem(juce::PopupMenu::Item(u8("Remove")).setID(920 + i));
      fxMenu.addSubMenu(u8(entry.label), one);
    }
    fxMenu.addSeparator();
    fxMenu.addSubMenu(u8("Add a plugin effect"), byVendor(pluginFx, 4000, [](const nlohmann::json&) { return false; }));
    menu.addSubMenu(u8("Plugin effects"), fxMenu);
  }
  if (current && doc::playsPatterns(*current)) {
    replace.addSubMenu(u8("Plugin instruments"), byVendor(instruments, 300, [&](const nlohmann::json& p) {
      return current->instrument.kind == "plugin" && current->instrument.plugin.value("id", std::string()) == p.value("id", std::string()) &&
             current->instrument.plugin.value("file", std::string()) == p.value("file", std::string());
    }));
    replace.addSeparator();
    const std::vector<doc::InstrumentChoice>& native = doc::builtinInstruments();
    for (size_t i = 0; i < native.size(); ++i)
      replace.addItem(juce::PopupMenu::Item(u8(native[i].label)).setID(800 + (int)i)
                          .setTicked(current->instrument.kind == "builtin" && current->instrument.name == native[i].name));
    menu.addSubMenu(u8("Replace instrument"), replace);
  }
  std::vector<std::string> offered;
  if (current && doc::playsPatterns(*current)) menu.addSubMenu(u8("Tools"), coreToolsMenu(trackId));
  menu.addSeparator();
  menu.addItem(3, "Remove");
  menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackId, offered, instruments, pluginFx](int chosen) {
    const doc::Track* t = history_.current().findTrack(trackId);
    if (!t) return;
    const juce::String name = u8(t->name);
    if (chosen >= 300 && chosen - 300 < (int)instruments.size()) {
      fetchPluginStates();  // Undo brings a replaced plugin back as it sounded, not as last saved
      const nlohmann::json plugin = instruments[(size_t)chosen - 300];
      edit([&](doc::Document& d) { doc::replaceInstrumentWithPlugin(d, trackId, plugin); },
           u8("Instrument: ") + u8(plugin.value("name", std::string("plugin"))));
      session_->openPluginEditor(doc::pluginKey(trackId));
      return;
    }
    if (chosen >= 4000 && chosen - 4000 < (int)pluginFx.size()) {
      const nlohmann::json plugin = pluginFx[(size_t)chosen - 4000];
      int at = -1;
      edit([&](doc::Document& d) { at = doc::addPluginEffect(d, trackId, plugin); }, u8("Add ") + u8(plugin.value("name", std::string("plugin"))));
      if (const doc::Track* now = history_.current().findTrack(trackId); now && at >= 0 && at < (int)now->effects.size())
        session_->openPluginEditor(doc::pluginKey(trackId, now->effects[(size_t)at].value("id", std::string())));
      return;
    }
    if (chosen >= 900 && chosen < 960) {
      const int i = (chosen - 900) % 20;
      if (i >= (int)t->effects.size()) return;
      const std::string fxId = t->effects[(size_t)i].value("id", std::string());
      if (chosen < 920) session_->openPluginEditor(doc::pluginKey(trackId, fxId));
      else if (chosen < 940) { fetchPluginStates(); edit([&](doc::Document& d) { doc::removeEffect(d, trackId, i); }, u8("Remove effect")); }
      else edit([&](doc::Document& d) { doc::setEffectBypass(d, trackId, i, !doc::effectBypassed(d.findTrack(trackId)->effects[(size_t)i])); }, u8("Effect on/off"));
      return;
    }
    if (chosen == 6) { session_->openPluginEditor(doc::pluginKey(trackId)); return; }
    if (chosen >= 800 && chosen - 800 < (int)doc::builtinInstruments().size()) {
      const doc::InstrumentChoice choice = doc::builtinInstruments()[(size_t)chosen - 800];
      edit([&](doc::Document& d) { doc::replaceInstrument(d, trackId, choice.name); }, u8("Replace instrument: ") + u8(choice.label));
      return;
    }
    if (chosen == 1) askTrackName(trackId);
    if (chosen == 2) edit([&](doc::Document& d) { doc::duplicateTrack(d, trackId); }, "Duplicate " + name);
    if (chosen == 3) edit([&](doc::Document& d) { doc::removeTrack(d, trackId); }, "Remove " + name);  // undo brings it back
    if (chosen == 4) openAutomation(trackId, "gainDb");
  });
}

void MainComponent::askTrackName(const std::string& trackId) {
  const doc::Track* t = history_.current().findTrack(trackId);
  if (!t) return;
  auto* w = new juce::AlertWindow(lang::tr(u8("Rename track")), juce::String(), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", u8(t->name), "Name:");
  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> safe(w);
  w->enterModalState(true, juce::ModalCallbackFunction::create([this, safe, trackId](int result) {
    if (result != 1 || safe == nullptr) return;
    const std::string name = safe->getTextEditorContents("name").toStdString();
    try {
      edit([&](doc::Document& d) { doc::renameTrack(d, trackId, name); }, "Rename");
    } catch (const std::exception& e) {
      showError(u8("Invalid name"), u8(e.what()));
    }
  }), true);
}

// mono: one channel, (L + R) / 2 (Diogo 2026-11-09: songs and passages as examples for the AI).
void MainComponent::exportWav(int bits, bool mono) {
  fetchPluginStates();  // the export makes its own plugins: from what they hold now, not the last save
  const juce::String name = history_.current().name.empty() ? juce::String("Song") : u8(history_.current().name);
  const juce::File folder = exportFolder();
  chooser_ = std::make_unique<juce::FileChooser>(u8("Export WAV"), folder.getChildFile(juce::File::createLegalFileName(name) + ".wav"), "*.wav");
  chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [this, bits, mono](const juce::FileChooser& fc) {
                          const juce::File f = fc.getResult();
                          if (f == juce::File()) return;
                          const std::string path = f.withFileExtension(".wav").getFullPathName().toStdString();
                          // The export takes the engine's document: while Patterns plays a slot that is the slot's
                          // preview, not the song (audit R04). Stopping it gives the engine the song back first.
                          stopPatternSlot();
                          if (mono) session_->exportFiles({{path, "", false, true}}, 0, -1, bits);
                          else session_->exportWav(path, bits);
                        });
}

// MIDI out (midi.hpp): the song as it plays, rolls expanded, one MIDI track per note track, the tempo with its glides.
// For the Bitwig or any other DAW (Diogo 2026-11-16: the Bitwig stays an option, by .mid).
void MainComponent::exportMidi() {
  const juce::String name = history_.current().name.empty() ? juce::String("Song") : u8(history_.current().name);
  chooser_ = std::make_unique<juce::FileChooser>(u8("Export MIDI"), exportFolder().getChildFile(juce::File::createLegalFileName(name) + ".mid"), "*.mid");
  chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [this](const juce::FileChooser& fc) {
                          const juce::File f = fc.getResult();
                          if (f == juce::File()) return;
                          try {
                            const doc::MidiFile midi = doc::songToMidi(history_.current());
                            size_t notes = 0;
                            for (const auto& t : midi.tracks) notes += t.notes.size();
                            doc::saveMidiFile(doc::writeMidi(midi), pathOf(f.withFileExtension(".mid")));
                            notice_ = u8("Exported MIDI: ") + juce::String((int)midi.tracks.size()) + u8(" tracks, ") + juce::String((int)notes) +
                                      u8(" notes (rolls expanded) to ") + f.withFileExtension(".mid").getFullPathName();
                            repaint();
                          } catch (const std::exception& e) {
                            showError(u8("Could not export MIDI"), u8(e.what()));
                          }
                        });
}

// For the Bitwig (bitwig.hpp): a folder "<song> (Bitwig)" with the .mid, a .vstpreset per plugin with its sound as
// it is now (the engine writes it from the live plugin), and a text file saying which preset goes on which track.
void MainComponent::exportBitwig() {
  const juce::String name = history_.current().name.empty() ? juce::String("Song") : u8(history_.current().name);
  chooser_ = std::make_unique<juce::FileChooser>(u8("Export for Bitwig: choose a folder"), exportFolder());
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                        [this, name](const juce::FileChooser& fc) {
    const juce::File parent = fc.getResult();
    if (parent == juce::File()) return;
    stopPatternSlot();  // the engine plays the song's plugins, not a Patterns preview
    // The plugins as they sound now go into the document first (the presets come from the same live plugins).
    fetchPluginStates();
    const juce::File folder = parent.getNonexistentChildFile(juce::File::createLegalFileName(name) + " (Bitwig)", "", true);
    try {
      if (!folder.createDirectory()) throw doc::DocumentError("folder", "cannot create " + folder.getFullPathName().toStdString());
      const doc::Document& song = history_.current();
      const doc::BitwigPlan plan = doc::bitwigPlan(song);
      doc::saveMidiFile(doc::exportMidi(song), pathOf(folder.getChildFile(u8(plan.midiFile))));
      std::vector<std::string> missing;
      int written = 0;
      auto writePreset = [&](const doc::BitwigPreset& p) {
        const std::string preset = session_->pluginPreset(p.key, 10000);
        juce::MemoryOutputStream bytes;
        if (preset.empty() || !juce::Base64::convertFromBase64(bytes, juce::String(preset)) ||
            !folder.getChildFile(u8(p.file)).replaceWithData(bytes.getData(), bytes.getDataSize())) {
          missing.push_back(p.key);
          return;
        }
        ++written;
      };
      for (const auto& t : plan.tracks) for (const auto& p : t.presets) writePreset(p);
      for (const auto& p : plan.master) writePreset(p);
      folder.getChildFile("Bitwig - read me.txt").replaceWithText(u8(doc::bitwigReadme(song, plan, missing)));
      notice_ = u8("Exported for Bitwig: ") + u8(plan.midiFile) + u8(" and ") + juce::String(written) + u8(" plugin presets") +
                (missing.empty() ? juce::String() : u8(" (") + juce::String((int)missing.size()) + u8(" without a preset: see the read me)")) +
                u8(" in ") + folder.getFullPathName();
      logLine("Bitwig export: " + folder.getFullPathName() + ", " + juce::String(written) + " presets, " + juce::String((int)missing.size()) + " missing");
      folder.revealToUser();
      repaint();
    } catch (const std::exception& e) {
      showError(u8("Could not export for Bitwig"), u8(e.what()));
    }
  });
}

// MIDI in: each track of the file becomes a track of the song (no instrument yet: choose a plugin), its notes in
// 4-bar patterns from the bar where the song is; the same bars twice are one pattern. One undo step.
void MainComponent::importMidi() {
  chooser_ = std::make_unique<juce::FileChooser>(u8("Import MIDI"), juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.mid;*.midi");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
    const juce::File f = fc.getResult();
    if (f == juce::File()) return;
    doc::MidiFile midi;
    try {
      midi = doc::parseMidi(doc::readMidiFile(pathOf(f)));
    } catch (const std::exception& e) {
      showError(u8("Could not import"), f.getFileName() + ": " + u8(e.what()));
      return;
    }
    doc::MidiImportOptions o;
    o.atTick = (int64_t)snap_.positionTick / kBar * kBar;
    o.useTempo = history_.current().tracks.empty();  // an empty song takes the file's tempo
    doc::MidiImportResult result;
    edit([&](doc::Document& d) { result = doc::importMidi(d, midi, o); }, u8("Import MIDI"));
    if (result.trackIds.empty()) return;  // the edit said why
    notice_ = u8("Imported ") + f.getFileName() + ": " + juce::String((int)result.trackIds.size()) + u8(" tracks, ") + juce::String(result.notes) +
              u8(" notes in ") + juce::String(result.patterns) + u8(" patterns. Choose a plugin for each track.");
    repaint();
  });
}

void MainComponent::selectDevice(const std::string& name) {
  if (!test_) {  // a self-test never changes the user's preferences
    settings_.getUserSettings()->setValue("audioDevice", u8(name));
    settings_.saveIfNeeded();
  }
  session_->selectDevice(name);
}

void MainComponent::confirmUnsaved(std::function<void()> next) {
  if (!unsaved()) return next();
  juce::String name = file_ != juce::File() ? file_.getFileName() : u8(history_.current().name);
  if (name.trim().isEmpty()) name = u8("Untitled");
  ChoiceDialog::show(*this, u8("Unsaved changes"), lang::tr("Save the changes to \"{}\"?").replace("{}", name),
      {{u8("Don't save"), 2}, {u8("Cancel"), 0}, {u8("Save"), 1, true}}, 0,
      [this, next](int result) {
        if (result == 1) save(false, [this, next](SaveResult r) { continueAfterSave(r, next); });
        else if (result == 2) next();
      });
}

void MainComponent::continueAfterSave(SaveResult result, std::function<void()> next) {
  if (result == SaveResult::Saved) next();  // Failed: the error was shown; the song stays open
}

void MainComponent::confirmClose(std::function<void()> close) { confirmUnsaved(std::move(close)); }

// The build, shown in the footer and the taskbar: a screenshot or a report says which code it came from.
// Only the build in D:\Core versions\Core is the one to use: any other folder is a test build, said up front
// so it is never used by mistake.
juce::String MainComponent::buildText() {
  static const juce::String folder = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getFileName();
  return (folder == "Core" ? juce::String() : "TEST BUILD (" + folder + ")  ") + CORE_BUILD_ID;
}

juce::String MainComponent::windowTitle() const {
  const juce::String name = file_ != juce::File() ? file_.getFileNameWithoutExtension() : u8(history_.current().name);
  return "Core - " + (name.isEmpty() ? juce::String("Untitled") : name) + (unsaved() ? " *" : "") + "  [" + buildText() + "]";
}

juce::Component::WindowControlKind MainComponent::windowControlAt(juce::Point<float> p) const {
  using Kind = juce::Component::WindowControlKind;
  for (const auto* b : {minimise_.get(), maximise_.get(), close_.get()})
    if (b->getBounds().toFloat().contains(p))
      return b->kind() == WindowButton::Kind::close ? Kind::close : b->kind() == WindowButton::Kind::minimise ? Kind::minimise : Kind::maximise;
  if (!topBar().toFloat().contains(p)) return Kind::client;
  // Bounds only (Component::contains would ask the system again, which asks this function).
  for (const auto* c : getChildren())
    if (c->isVisible() && c->getBounds().toFloat().contains(p)) return Kind::client;
  return Kind::caption;  // the logo and the gaps: drag the window, double-click to maximise
}

// Help > About: the version and commit of the window and of the engine, with a button to copy them.
void MainComponent::showAbout() {
  const juce::String engine = snap_.engineBuild.empty() ? juce::String("not running") : u8(snap_.engineBuild);
  juce::String text;
  text << "Core " << CORE_BUILD_ID << "\n"
       << "Version: " << CORE_VERSION << "\n"
       << "Commit: " << CORE_GIT_HASH << (CORE_GIT_MODIFIED ? " (with uncommitted changes)" : "") << "\n"
       << "Commit date: " << CORE_GIT_DATE << "\n"
       << "Built: " << __DATE__ << " " << __TIME__ << "\n"
       << "Engine: " << engine << "\n"
       << juce::SystemStats::getOperatingSystemName();
  if (!snap_.engineBuild.empty() && snap_.engineBuild != CORE_BUILD_ID)
    text << "\n\nThe engine is from another build than the window: reinstall or rebuild both.";
  juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::InfoIcon)
                                   .withTitle("About Core").withMessage(text).withButton("Copy", 1).withButton("OK", 0),
                               [text](int result) { if (result == 1) juce::SystemClipboard::copyTextToClipboard(text); });
}

// ---------------------------------------------------------------- the Library and the Inspector

void MainComponent::setupLibrary() {
  library_.index = [this]() -> const doc::SampleIndex* { return hasSampleIndex_ ? &sampleIndex_ : nullptr; };
  library_.trackName = [this] {
    const doc::Track* t = history_.current().findTrack(selectedTrack_.empty() ? editor_.track() : selectedTrack_);
    return t ? u8(t->name) : juce::String();
  };
  library_.trackTakesSamples = [this] {
    const doc::Track* t = history_.current().findTrack(selectedTrack_.empty() ? editor_.track() : selectedTrack_);
    return t && takesSamples(*t);
  };
  library_.fitContext = [this]() -> std::pair<std::string, double> {
    const doc::Track* t = history_.current().findTrack(selectedTrack_.empty() ? editor_.track() : selectedTrack_);
    if (!t || !takesSamples(*t)) return {};
    return {doc::trackSampleRole(*t), doc::fastestTempo(history_.current())};
  };
  library_.presets = [this] {
    reloadTemplates();
    std::vector<LibraryPanel::Preset> out;
    out.push_back({LibraryPanel::kBasicTemplate, "Kick, bass and hats", "Basic", doc::newTemplateDocument().bpm, true});
    for (const auto& e : templates_.templates) out.push_back({e.tmpl.info.id, e.tmpl.info.name, e.tmpl.info.style, e.tmpl.document.bpm, e.builtin,
                     e.tmpl.info.source.is_object() && e.tmpl.info.source.value("kind", std::string()) == "analysis"});
    return out;
  };
  library_.waveform = [this](const doc::LibrarySample& s) { return waves_.get(s.sha256, juce::File(u8(s.file))); };
  library_.loadFavourites = [this] {
    std::set<std::string> out;
    for (const auto& sha : juce::StringArray::fromTokens(settings_.getUserSettings()->getValue("libraryFavourites"), ";", ""))
      if (sha.isNotEmpty()) out.insert(sha.toStdString());
    return out;
  };
  library_.saveFavourites = [this](const std::set<std::string>& favourites) {
    juce::StringArray list;
    for (const auto& sha : favourites) list.add(u8(sha));
    settings_.getUserSettings()->setValue("libraryFavourites", list.joinIntoString(";"));
  };
  library_.onUseSample = [this](const doc::LibrarySample& s) {
    const std::string track = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
    if (!track.empty()) putSampleOnTrack(track, s);
  };
  library_.onPreviewSample = [this](const doc::LibrarySample& s, const std::string& role) { return previewLibrarySample(s, role); };
  // Templates live in the Library (Diogo, 2026-11-05: out of File > New, browsed like the samples, by style).
  library_.onOpenPreset = [this](const std::string& id) {
    if (id == LibraryPanel::kBasicTemplate) newProject(true);
    else newFromTemplate(id);
  };
  library_.onSaveTemplate = [this] { saveAsTemplate(); };
  library_.onDeleteTemplate = [this](const std::string& id) { deleteTemplate(id); };
  library_.onRestoreTemplates = [this] { restoreBuiltinTemplates(); };
  library_.onOpenTemplatesFolder = [this] { userTemplatesFolder().createDirectory(); userTemplatesFolder().startAsProcess(); };
  library_.patterns = [this] { return loadPatterns(); };
  library_.onStopPreview = [this] { stopPatternPreview(); };
  library_.onPreviewPattern = [this](const doc::PatternPreset& preset, int bars) {
    const std::string trackId = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
    const doc::Track* t = history_.current().findTrack(trackId);
    if (!t || !doc::playsPatterns(*t)) return;
    const doc::Pattern* p = trackId == editor_.track() ? t->findPattern(editor_.patternId()) : nullptr;
    if (!p && !t->patterns.empty()) p = &t->patterns.front();
    previewPattern(trackId, doc::fitPreset(preset, (int64_t)std::clamp(bars, 1, 16) * 4 * doc::kPpq, doc::trackRoot(*t, p)));
  };
  library_.onApplyPattern = [this](const doc::PatternPreset& preset, int bars, bool add) {
    stopPatternPreview();
    const std::string trackId = selectedTrack_.empty() ? editor_.track() : selectedTrack_;
    const doc::Track* t = history_.current().findTrack(trackId);
    if (!t || !doc::playsPatterns(*t)) { notice_ = u8("Select a note track first: the pattern goes on it."); repaint(); return; }
    if (editor_.track() != trackId) selectTrack(trackId);
    editor_.applyPreset(preset, bars, add);
  };
  library_.onChooseFolder = [this] { chooseSampleLibrary(); };
  library_.document = [this]() -> const doc::Document* { return &history_.current(); };
  library_.projectFolder = [this] { return projectDir(); };
  library_.onRemoveUnused = [this] { removeUnusedAudio(); };
  library_.onEditMetadata = [this](const std::string& sha, const nlohmann::json& fields) -> std::string {
    if (!hasSampleIndex_) return "There is no sample library.";
    const doc::LibrarySample* s = sampleIndex_.findHash(sha);
    try {
      if (doc::setSampleMetadata(sampleIndex_, {sha}, fields, "sound " + (s ? s->name : std::string("sample"))) > 0) {
        sampleCatalogChanged();
        notice_ = u8("Saved in Core's sample catalogue (Samples > Undo puts it back).");
      }
    } catch (const std::exception& e) {
      return e.what();
    }
    return {};
  };
}

void MainComponent::showProjectMenu() {
  getMenuForIndex(0, "File").showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&project_),
                                           [safe = juce::Component::SafePointer<MainComponent>(this)](int id) {
                                             if (safe && id != 0) safe->menuItemSelected(id, 0);
                                           });
}

// The notes sound on their track one after the other, as far apart as in the pattern (at most 64, at most 8 s).
void MainComponent::previewNotes(const std::string& trackId, const std::vector<doc::Note>& notes) {
  if (notes.empty() || trackId.empty()) return;
  // The tempo playing where the song is now (the tempo map's, not the starting BPM); the engine's range is 40-600.
  const double msPerTick = 60000.0 / (std::clamp(doc::bpmAt(history_.current(), (int64_t)shownPosition()), 40.0, 600.0) * doc::kPpq);
  const int64_t first = notes.front().tick;
  juce::Component::SafePointer<MainComponent> safe(this);
  for (size_t i = 0; i < notes.size() && i < 64; ++i) {
    const auto& n = notes[i];
    const int on = (int)std::round((double)(n.tick - first) * msPerTick);
    if (on > 8000) break;
    const int off = on + std::max(60, (int)std::round((double)n.length * msPerTick));
    const int pitch = n.pitch;
    const double velocity = n.velocity;
    juce::Timer::callAfterDelay(on + 1, [safe, trackId, pitch, velocity] { if (safe) safe->session_->liveNote(trackId, pitch, velocity, true); });
    juce::Timer::callAfterDelay(off, [safe, trackId, pitch] { if (safe) safe->session_->liveNote(trackId, pitch, 0, false); });
  }
}

// ---------------------------------------------------------------- the pattern library

std::vector<doc::PatternPreset> MainComponent::loadPatterns() const {
  const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
  const juce::File mine = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("Patterns");
  std::vector<std::string> problems;
  auto out = doc::loadPatternLibrary({{"builtin", pathOf(exe.getChildFile("patterns"))}, {"user", pathOf(mine)}}, &problems);
  for (const auto& p : problems) logLine(u8("Pattern skipped: ") + u8(p));
  return out;
}

void MainComponent::stopPatternPreview() {
  ++patternPreviewGen_;
  if (session_ != nullptr && !patternPreviewTrack_.empty())
    for (int pitch : patternPreviewPitches_) session_->liveNote(patternPreviewTrack_, pitch, 0, false);
  patternPreviewPitches_.clear();
}

// Like previewNotes, but long (up to 16 bars, 2000 notes, 60 s) and stoppable: every timer checks that its preview
// is still the one playing.
void MainComponent::previewPattern(const std::string& trackId, const std::vector<doc::Note>& notes) {
  stopPatternPreview();
  if (notes.empty() || trackId.empty() || session_ == nullptr) return;
  const uint64_t gen = patternPreviewGen_;
  patternPreviewTrack_ = trackId;
  const double msPerTick = 60000.0 / (std::clamp(doc::bpmAt(history_.current(), (int64_t)shownPosition()), 40.0, 600.0) * doc::kPpq);
  juce::Component::SafePointer<MainComponent> safe(this);
  for (size_t i = 0; i < notes.size() && i < 2000; ++i) {
    const auto& n = notes[i];
    const int on = (int)std::round((double)n.tick * msPerTick);
    if (on > 60000) break;
    const int off = on + std::max(30, (int)std::round((double)n.length * msPerTick));
    const int pitch = n.pitch;
    const double velocity = n.velocity;
    patternPreviewPitches_.insert(pitch);
    juce::Timer::callAfterDelay(on + 1, [safe, gen, trackId, pitch, velocity] {
      if (safe && safe->patternPreviewGen_ == gen) safe->session_->liveNote(trackId, pitch, velocity, true);
    });
    juce::Timer::callAfterDelay(off, [safe, gen, trackId, pitch] {
      if (safe && safe->patternPreviewGen_ == gen) safe->session_->liveNote(trackId, pitch, 0, false);
    });
  }
}

// "Save the bars shown as a pattern": a name and a role, then a new file in %APPDATA%\Core\Patterns (never over one).
void MainComponent::savePatternFromEditor(int64_t start, int64_t end) {
  const doc::Track* t = history_.current().findTrack(editor_.track());
  const doc::Pattern* p = t ? t->findPattern(editor_.patternId()) : nullptr;
  if (!p) return;
  auto* w = new juce::AlertWindow(lang::tr(u8("Save as a pattern")), lang::tr(u8("The notes of the bars shown go into your pattern library (Library > Patterns). "
                                                              "Any track can use it later.")), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", u8(t->name) + " " + juce::String(start / (4 * doc::kPpq) + 1), u8("Name:"));
  static const juce::StringArray roles{"bass", "kick", "hats", "cymbals", "perc", "build", "lead", "other"};
  w->addComboBox("role", roles, u8("Role (only to sort the list):"));
  const std::string guess = doc::trackSampleRole(*t);
  const int r = roles.indexOf(guess == "hat" || guess == "openhat" ? juce::String("hats") : guess == "accent" ? juce::String("perc") : u8(guess));
  w->getComboBoxComponent("role")->setSelectedItemIndex(r >= 0 ? r : roles.size() - 1);
  w->addTextEditor("styles", u8(doc::songStyle(history_.current())), u8("Styles (comma separated, may be empty):"));
  w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<juce::AlertWindow> win(w);
  juce::Component::SafePointer<MainComponent> self(this);
  const doc::Pattern copy = *p;
  w->enterModalState(true, juce::ModalCallbackFunction::create([win, self, copy, start, end](int ok) {
    if (ok != 1 || !win || !self) return;
    const juce::String name = win->getTextEditorContents("name").trim().substring(0, 80);
    if (name.isEmpty()) return;
    try {
      doc::PatternPreset preset = doc::presetFromPattern(copy, start, end, name.toStdString());
      preset.role = win->getComboBoxComponent("role")->getText().toStdString();
      if (preset.role == "other") preset.role.clear();
      for (const auto& st : juce::StringArray::fromTokens(win->getTextEditorContents("styles"), ",", ""))
        if (st.trim().isNotEmpty()) preset.styles.push_back(st.trim().toLowerCase().toStdString());
      std::string slug;
      for (const char c : name.toLowerCase().toStdString()) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug += c;
        else if (!slug.empty() && slug.back() != '-') slug += '-';
      }
      while (!slug.empty() && slug.back() == '-') slug.pop_back();
      preset.id = "mine/" + (slug.empty() ? std::string("pattern") : slug.substr(0, 60));
      preset.source = "Saved from the piano roll";
      const juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Core").getChildFile("Patterns");
      const auto saved = doc::savePatternPreset(pathOf(dir), preset, self->loadPatterns());
      self->library_.patternsChanged();
      self->notice_ = u8("Saved as the pattern \"") + u8(saved.name) + u8("\" (Library > Patterns).");
    } catch (const std::exception& e) {
      self->notice_ = u8("Pattern not saved: ") + u8(e.what());
    }
    self->repaint();
  }), true);
}

// "Build-up on a new track": a sampler track ("Build-up") with the library's best snare or clap for the song's
// style, its notes rising into the next section after the playhead (the drop); one undo step.
void MainComponent::buildupTrack(int bars, bool roll, bool gap) {
  const doc::Document& d0 = history_.current();
  const int64_t bar = 4 * doc::kPpq, length = (int64_t)std::clamp(bars, 1, 16) * bar;
  const int64_t now = (int64_t)shownPosition();
  int64_t drop = -1;
  for (const auto& sec : d0.sections) if (sec.startTick > now && sec.startTick >= length) { drop = sec.startTick; break; }
  if (drop < 0) drop = std::max<int64_t>(length, ((now + length) / (8 * bar) + 1) * 8 * bar);
  drop = std::min(drop, d0.lengthTicks);
  const int64_t start = std::max<int64_t>(0, drop - length);
  const doc::LibrarySample* snare = nullptr;
  if (hasSampleIndex_) {
    const auto found = doc::suggestSamples(sampleIndex_, "accent", doc::songStyle(d0), 8);
    for (const auto& f : found) if (f.sample && f.sample->info.role == "snare") { snare = f.sample; break; }
    if (!snare && !found.empty()) snare = found.front().sample;
  }
  std::string id;
  edit([&](doc::Document& d) {
    id = doc::addTrack(d, "sampler", "Build-up");
    if (snare) doc::setTrackSample(d, id, sampleIndex_, *snare);
    doc::Track& t = doc::trackById(d, id);
    doc::Pattern& p = t.patterns.front();
    p.notes.clear();
    p.lengthTicks = drop - start;
    doc::BuildupOptions o;
    o.start = 0;
    o.length = drop - start;
    o.roll = roll;
    o.gapTicks = gap ? doc::kPpq : 0;
    o.pitch = t.instrument.samples.empty() ? 60 : t.instrument.samples.front().rootKey;
    doc::makeBuildup(p, o);
    t.clips.clear();
    t.clips.push_back({"c1", p.id, start, drop - start, 0, doc::json::object()});
  }, u8("Build-up track"));
  if (id.empty() || !history_.current().findTrack(id)) return;  // refused: edit() said why
  selectTrack(id);
  notice_ = u8("Build-up: ") + juce::String(bars) + u8(" bars into bar ") + juce::String(drop / bar + 1) +
            (snare ? u8(" with ") + u8(doc::shownName(*snare)) : u8(" (no snare in the library: choose a sound for the track)")) + u8(". Ctrl+Z undoes.");
  repaint();
}

bool MainComponent::previewLibrarySample(const doc::LibrarySample& sample, const std::string& role) {
  if (!hasSampleIndex_ || session_ == nullptr) return false;
  std::string trackId;
  int pitch = 60;
  if (!samplePreviewTrack_.empty()) session_->liveNote(samplePreviewTrack_, samplePreviewPitch_, 0, false);  // the older one stops
  try {
    session_->setDocument(doc::withPreviewTrack(history_.current(), sampleIndex_, sample, role, trackId, pitch));
  } catch (const std::exception&) {
    return false;
  }
  // The note starts once the engine has the track (commands reach it in order), lasts the sample's length (at most
  // 6 s), then the engine gets the song back without the hidden track. A newer preview takes over: the older one's
  // note-off and restore are skipped so they never cut it.
  const int ms = std::clamp((int)std::round(sample.seconds * 1000.0), 150, 6000);
  samplePreviewTrack_ = trackId;
  samplePreviewPitch_ = pitch;
  const uint64_t gen = ++samplePreviewGen_;
  juce::Component::SafePointer<MainComponent> safe(this);
  juce::Timer::callAfterDelay(120, [safe, trackId, pitch, gen] { if (safe != nullptr && safe->samplePreviewGen_ == gen) safe->session_->liveNote(trackId, pitch, 0.9, true); });
  juce::Timer::callAfterDelay(120 + ms, [safe, trackId, pitch, gen] { if (safe != nullptr && safe->samplePreviewGen_ == gen) safe->session_->liveNote(trackId, pitch, 0, false); });
  juce::Timer::callAfterDelay(120 + ms + 400, [safe, gen] { if (safe != nullptr && safe->samplePreviewGen_ == gen) { safe->samplePreviewTrack_.clear(); safe->session_->setDocument(safe->engineDocument()); } });
  return true;
}

// ---------------------------------------------------------------- menus

namespace { enum MenuId { kMidiNone = 200, kMidiDevice = 201, kNewEmpty = 50, kOpen = 1, kSave, kSaveAs, kExport24, kExport32, kQuit, kUndo, kRedo, kImportAudio, kRelocate, kDevice = 100,
                       kMidiDevicesEnd = 400, kPluginCoreFirst = 420 /* + place in doc::corePlugins(), up to 20 */,
                       kPluginPatterns = 440, kPluginSilent = 442,
                       kPluginScan = 460, kPluginFolder, kPluginRetry, kMasterFx = 2400 /* + 60 */, kMasterFxAdd = 2500 /* + 400 */,
                       kOpenLogs = 580, kAbout = 581, kExportMono = 590, kImportMidi = 595, kExportMidi = 596, kExportBitwig = 597,
                       kSampleFolder = 1600, kSampleRescan, kSampleApply, kSampleAuto, kSampleShuffle, kSampleCatalogUndo,
                                              kRandomize = 1700 /* + strength 1..3; + 4: tempo per section; + 5: sound */, kViewLibrary = 1800, kViewInspector, kViewMixer, kViewScale = 1810 /* + 0 auto, + 1..5: 100..200 % */,
                       kViewSectionShades = 1820,
                       kRenderMixToSample = 1900, kRenderTrackToSample, kSettings, kShortcuts, kResampleLoop, kBounceTrack,
                       kTrainingClips = 1950,
                       kPluginHelpFirst = 3000 /* + the plugin's place in pluginHelpIds(), up to 100 */ }; }


juce::String MainComponent::MenuBar::getTooltip() {
  static const char* const help[] = {
      "Songs: new, open, save, import audio, export WAV (templates are in the Library)",
      "Undo and redo, and Randomize a song made from a style template (pattern, tempo, sound)",
      "Show or hide the Library and Inspector; open the Mixer; the UI size",
      "Cut or render samples, the audio output and the MIDI input",
      "Patterns (the channel rack) and Core's tools; the VST3 plugins: scan, folders, the master's effects",
      "Your sample library: its folder, rescan, the best samples for this song",
      "AICORE, Core's add-on: open its chat, its proposals, its folder",
      "Logs (problems, crash dumps) and the version",
  };

  auto* model = getModel();
  if (model == nullptr) return {};
  const auto names = model->getMenuBarNames();
  int x = getMouseXYRelative().x;
  for (int i = 0; i < names.size() && i < (int)std::size(help); ++i) {
    x -= getLookAndFeel().getMenuBarItemWidth(*this, i, names[i]);
    if (x < 0) return juce::String::fromUTF8(help[i]);
  }
  return {};
}

juce::StringArray MainComponent::getMenuBarNames() { return {"File", "Edit", "View", "Audio", "Plugins", "Samples", "AICORE", "Help"}; }

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&) {
  juce::PopupMenu menu;
  if (index == 2) {  // View: the side panels
    menu.addItem(juce::PopupMenu::Item(u8("Library")).setID(kViewLibrary).setTicked(showLibrary_));
    menu.addItem(juce::PopupMenu::Item(u8("Inspector")).setID(kViewInspector).setTicked(showInspector_));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Mixer")).setID(kViewMixer).setTicked(mixerOpen_));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Section colours behind the tracks")).setID(kViewSectionShades).setTicked(sectionShades_));
    menu.addSeparator();
    juce::PopupMenu size;  // how big the whole window is drawn (Auto: 150 % on a 2560 px wide window)
    const float chosen = (float)settings_.getUserSettings()->getDoubleValue("uiScale", 0.0);
    size.addItem(juce::PopupMenu::Item(u8("Auto (") + juce::String(juce::roundToInt(uiScaleFor(getParentWidth(), 0.0f) * 100)) + " %)")
                     .setID(kViewScale).setTicked(chosen <= 0));
    for (int i = 1; i <= 5; ++i) {
      const float scale = 0.75f + 0.25f * (float)i;
      size.addItem(juce::PopupMenu::Item(juce::String(juce::roundToInt(scale * 100)) + " %").setID(kViewScale + i).setTicked(std::abs(chosen - scale) < 0.01f));
    }
    menu.addSubMenu(u8("UI size"), size);
    return menu;
  }
  if (index > 2) --index;  // the menus after View keep their numbers below
  if (index == 0) {
    // A new empty song; the templates are in the Library (Templates tab), browsed by style like the samples.
    menu.addItem(juce::PopupMenu::Item(u8("New")).setID(kNewEmpty));
    menu.addItem(juce::PopupMenu::Item(u8("Open…")).setID(kOpen).setTicked(false));
    menu.addItem(juce::PopupMenu::Item("Save").setID(kSave));
    menu.addItem(juce::PopupMenu::Item(u8("Save as…")).setID(kSaveAs));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Import audio (WAV)…")).setID(kImportAudio));
    menu.addItem(juce::PopupMenu::Item(u8("Import MIDI (.mid)…")).setID(kImportMidi));
    menu.addItem(juce::PopupMenu::Item(u8("Locate missing files…")).setID(kRelocate).setEnabled(!missing_.empty()));
    menu.addSeparator();
    const bool exporting = snap_.exportJob.active();
    menu.addItem(juce::PopupMenu::Item(u8("Export WAV 24-bit…")).setID(kExport24).setEnabled(!exporting));
    menu.addItem(juce::PopupMenu::Item(u8("Export WAV 32-bit float…")).setID(kExport32).setEnabled(!exporting));
    menu.addItem(juce::PopupMenu::Item(u8("Export WAV mono…")).setID(kExportMono).setEnabled(!exporting));
    menu.addItem(juce::PopupMenu::Item(u8("Export MIDI (.mid)…")).setID(kExportMidi));
    menu.addItem(juce::PopupMenu::Item(u8("Export for Bitwig (MIDI + plugin presets)…")).setID(kExportBitwig));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Settings…")).setID(kSettings));
    menu.addItem(juce::PopupMenu::Item(u8("Keyboard shortcuts…")).setID(kShortcuts));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item("Quit").setID(kQuit));
  } else if (index == 3) {
    // The VST plugins of this PC (Diogo 2026-11-16: Core's sound comes from plugins). Each file is scanned and played
    // on its own first, in its own process: one that hangs or crashes is set aside, never taking Core down.
    // Core's own windows first: the Patterns window is where songs start (Diogo 2026-11-16), and it plays the tracks' plugins.
    menu.addSectionHeader(u8("Core"));
    menu.addItem(juce::PopupMenu::Item(u8("Patterns (channel rack, templates)…")).setID(kPluginPatterns));
    for (const auto& p : doc::corePlugins())
      if (p.id != "pcore") menu.addSubMenu(u8(p.shortName) + u8(": ") + u8(p.about), coreToolMenu(p.id, ""));
    {  // A song from before 0.7.0: its old Core sounds are kept but silent (Diogo 2026-11-16, "não sai som").
      int silent = 0;
      for (const auto& t : history_.current().tracks) silent += t.instrument.kind == "builtin" || t.instrument.kind == "none";
      menu.addItem(juce::PopupMenu::Item(u8("Give the silent tracks their role's plugin (") + juce::String(silent) + u8(" silent)"))
                       .setID(kPluginSilent).setEnabled(silent > 0));
    }
    menu.addSectionHeader(u8("VST3 plugins"));
    menu.addItem(juce::PopupMenu::Item(plugins_->scanning() ? u8("Scanning: ") + plugins_->progress() : u8("Scan plugins (VST3)"))
                     .setID(kPluginScan).setEnabled(!plugins_->scanning()));
    menu.addItem(juce::PopupMenu::Item(u8("Add a plugin folder…")).setID(kPluginFolder));
    const auto quarantine = plugins_->quarantine();
    menu.addItem(juce::PopupMenu::Item(u8("Try the set-aside plugins again (") + juce::String((int)quarantine.size()) + ")")
                     .setID(kPluginRetry).setEnabled(!quarantine.empty() && !plugins_->scanning()));
    menu.addSectionHeader(u8("Found: ") + juce::String((int)plugins_->instruments().size()) + u8(" instruments, ") +
                          juce::String((int)plugins_->effects().size()) + u8(" effects"));
    for (const auto& f : plugins_->folders()) menu.addItem(juce::PopupMenu::Item(f).setEnabled(false));
    menu.addSeparator();
    // The master's effects (after the master volume): open, on/off, remove, add. Every effect of the chain
    // is listed, Core's own ones included (an old song or a template carries them), each with Remove, so
    // nothing on the master is stuck (bug 2026-11-17). A window opens for a plugin only.
    {
      const auto& chain = history_.current().masterEffects;
      juce::PopupMenu master;
      for (const auto& entry : doc::effectMenuEntries(chain)) {
        const int i = entry.index;
        juce::PopupMenu one;
        if (entry.window) {
          const std::string key = doc::pluginKey("", chain[(size_t)i].value("id", std::string()));
          one.addItem(juce::PopupMenu::Item(u8("Open…")).setID(kMasterFx + i).setEnabled(!snap_.missingPlugins.count(key)));
        }
        one.addItem(juce::PopupMenu::Item(u8("On")).setID(kMasterFx + 40 + i).setTicked(!doc::effectBypassed(chain[(size_t)i])));
        one.addItem(juce::PopupMenu::Item(u8("Remove")).setID(kMasterFx + 20 + i));
        master.addSubMenu(u8(entry.label), one);
      }
      const auto effects = plugins_->effects();
      juce::PopupMenu add;
      for (size_t i = 0; i < effects.size() && i < 400; ++i)
        add.addItem(kMasterFxAdd + (int)i, u8(effects[i].value("name", std::string("Plugin"))) + "  (" + u8(effects[i].value("vendor", std::string())) + ")");
      if (effects.empty()) add.addItem(juce::PopupMenu::Item(u8("None found yet: Scan plugins")).setEnabled(false));
      master.addSeparator();
      master.addSubMenu(u8("Add a plugin effect"), add);
      menu.addSubMenu(u8("Master effects"), master);
    }
  } else if (index == 4) {
    const juce::File folder = sampleLibraryFolder();
    menu.addSectionHeader(folder == juce::File() ? u8("No sample library folder yet") : u8("Library: ") + folder.getFullPathName());
    menu.addItem(juce::PopupMenu::Item(u8("Choose sample library folder…")).setID(kSampleFolder));
    const bool scanning = sampleScan_ && sampleScan_->state.load() == 0;
    menu.addItem(juce::PopupMenu::Item(scanning ? u8("Scanning: ") + juce::String(sampleScan_->done.load()) + " / " + juce::String(sampleScan_->total.load())
                                                : u8("Rescan the library"))
                     .setID(kSampleRescan).setEnabled(!scanning && folder.isDirectory()));
    if (analysisJob_)
      menu.addItem(juce::PopupMenu::Item(u8("Measuring sounds: ") + juce::String(analysisJob_->done.load()) + " / " + juce::String(analysisJob_->total.load()) +
                                         (analysisJob_->pause.load() ? u8(" (paused while playing)") : juce::String()))
                       .setEnabled(false));
    if (hasSampleIndex_) {
      int duplicates = 0;
      for (const auto& x : sampleIndex_.samples) duplicates += x.duplicates;
      menu.addItem(juce::PopupMenu::Item(juce::String((int)sampleIndex_.samples.size()) + u8(" samples (") + juce::String(duplicates) +
                                         u8(" duplicates left out, ") + juce::String((int)sampleIndex_.unreadable.size()) + u8(" unreadable)"))
                       .setEnabled(false));
    }
    const juce::String lastEdit = sampleIndex_.undo.empty() ? juce::String() : u8(sampleIndex_.undo.back().label);
    menu.addItem(juce::PopupMenu::Item(lastEdit.isEmpty() ? u8("Undo sample catalogue edit") : u8("Undo sample catalogue edit: ") + lastEdit)
                     .setID(kSampleCatalogUndo).setEnabled(hasSampleIndex_ && lastEdit.isNotEmpty()));
    if (!sampleIndex_.sidecarProblems.empty())
      menu.addItem(juce::PopupMenu::Item(juce::String((int)sampleIndex_.sidecarProblems.size()) + u8(" problem(s) in ") + doc::kSampleMetadataFile + u8(" (see the log)"))
                       .setEnabled(false));
    menu.addSeparator();
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Use the best samples in this song")).setID(kSampleApply).setEnabled(hasSampleIndex_));
    menu.addItem(juce::PopupMenu::Item(u8("Shuffle samples (another good pick)")).setID(kSampleShuffle).setEnabled(hasSampleIndex_));
    menu.addItem(juce::PopupMenu::Item(u8("Suggest samples for new songs from style templates")).setID(kSampleAuto)
                     .setTicked(settings_.getUserSettings()->getBoolValue("suggestSamples", true)));
  } else if (index == 5) {  // AICORE: the same items as the AICORE button's right-click
    addAicoreItems(menu, aiAllowed_, realOutput_, aiAllowed_ ? (int)inbox_.size() : 0, aicoreState(aicore_, aiAllowed_));
  } else if (index == 6) {
    menu.addItem(juce::PopupMenu::Item(u8("Open the logs folder (problems, crash dumps)")).setID(kOpenLogs));
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("About Core (version ") + CORE_VERSION + ")").setID(kAbout));
  } else if (index == 2) {
    // Cutting a song into a sample: import it (a new audio clip), click the clip, select, "Save the selection as a sample".
    if (snap_.devices.empty()) menu.addItem(juce::PopupMenu::Item(u8("(no audio outputs)")).setEnabled(false));
    for (size_t i = 0; i < snap_.devices.size(); ++i)
      menu.addItem(juce::PopupMenu::Item(u8(snap_.devices[i])).setID(kDevice + (int)i).setTicked(snap_.devices[i] == snap_.device));
    menu.addSeparator();
    menu.addSectionHeader(u8("MIDI input (plays the editor's track)"));
    menu.addItem(juce::PopupMenu::Item(u8("None")).setID(kMidiNone).setTicked(midiInputId_.isEmpty()));
    const auto inputs = juce::MidiInput::getAvailableDevices();
    for (int i = 0; i < inputs.size(); ++i)
      menu.addItem(juce::PopupMenu::Item(inputs[i].name).setID(kMidiDevice + i).setTicked(inputs[i].identifier == midiInputId_));
  } else {
    menu.addItem(juce::PopupMenu::Item(history_.canUndo() ? "Undo " + u8(history_.undoLabel()) : juce::String("Undo"))
                     .setID(kUndo).setEnabled(history_.canUndo()));
    menu.addItem(juce::PopupMenu::Item(history_.canRedo() ? "Redo " + u8(history_.redoLabel()) : juce::String("Redo"))
                     .setID(kRedo).setEnabled(history_.canRedo()));
    // Randomize: songs made from a style template (a new seed every time; Undo goes back).
    const bool styled = !doc::songStyle(history_.current()).empty();
    menu.addSeparator();
    menu.addItem(juce::PopupMenu::Item(u8("Randomize (subtle)")).setID(kRandomize + 1).setEnabled(styled));
    menu.addItem(juce::PopupMenu::Item(u8("Randomize (medium)")).setID(kRandomize + 2).setEnabled(styled));
    menu.addItem(juce::PopupMenu::Item(u8("Randomize (bold)")).setID(kRandomize + 3).setEnabled(styled));
    menu.addItem(juce::PopupMenu::Item(u8("Randomize tempo per section")).setID(kRandomize + 4).setEnabled(styled));
    menu.addItem(juce::PopupMenu::Item(u8("Randomize sound (built-in instruments)")).setID(kRandomize + 5).setEnabled(styled));
  }
  // Shortcut hints on the right, as in other Windows programs.
  juce::PopupMenu::MenuItemIterator it(menu);
  while (it.next()) {
    auto& item = it.getItem();
    const char* id = item.itemID == kOpen ? "file.open" : item.itemID == kSave ? "file.save" : item.itemID == kSaveAs ? "file.saveAs"
                   : item.itemID == kUndo ? "edit.undo" : item.itemID == kRedo ? "edit.redo"
                   : item.itemID == kNewEmpty ? "file.new"
                   : item.itemID == kExport24 ? "file.export" : item.itemID == kSettings ? "file.settings" : item.itemID == kImportAudio ? "file.importAudio"
                   : item.itemID == kViewMixer ? "view.mixer" : item.itemID == kPluginPatterns ? "view.patterns" : nullptr;
    item.shortcutKeyDescription = id != nullptr ? Keymap::get().text(id) : juce::String();
  }
  return menu;
}

void MainComponent::menuItemSelected(int id, int) {
  switch (id) {
    case kAicoreOpen: openAicore(); break;
    case kAicoreProposals: showProposals(); break;
    case kAicoreAllow: setAiControl(!aiAllowed_); break;
    case kAicoreBridge: bridgeFolder().createDirectory(); bridgeFolder().startAsProcess(); break;
    case kAicoreChoose: chooseAicoreFolder(); break;
    case kOpen: open(); break;
    case kSettings: openSettings(false); break;
    case kShortcuts: openSettings(true); break;
    case kSave: save(false); break;
    case kSaveAs: save(true); break;
    case kQuit: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
    case kUndo: undo(); break;
    case kRedo: redo(); break;
    case kNewEmpty: newProject(false); break;
    case kOpenLogs: logsFolder().createDirectory(); logsFolder().startAsProcess(); break;
    case kAbout: showAbout(); break;
    case kSampleFolder: chooseSampleLibrary(); break;
    case kSampleRescan: scanSampleLibrary(); break;
    case kSampleCatalogUndo:
      if (const std::string label = doc::undoCatalogEdit(sampleIndex_); !label.empty()) {
        sampleCatalogChanged();
        notice_ = u8("Undone in the sample catalogue: ") + u8(label);
      }
      break;
    case kSampleApply: useSuggestedSamples(false, false); break;
    case kSampleShuffle: useSuggestedSamples(false, true); break;
    case kRandomize + 1: case kRandomize + 2: case kRandomize + 3: randomizeSong(id - kRandomize); break;
    case kRandomize + 4: randomizeSong(2, "tempo"); break;
    case kRandomize + 5: randomizeSong(2, "sound"); break;
    case kViewMixer: setMixerOpen(!mixerOpen_); break;
    case kViewSectionShades:
      sectionShades_ = !sectionShades_;
      settings_.getUserSettings()->setValue("sectionShades", sectionShades_);
      settings_.saveIfNeeded();
      repaint();  // the ruler and the track rows inside it
      break;
    case kViewScale: case kViewScale + 1: case kViewScale + 2: case kViewScale + 3: case kViewScale + 4: case kViewScale + 5:
      settings_.getUserSettings()->setValue("uiScale", id == kViewScale ? 0.0 : 0.75 + 0.25 * (id - kViewScale));
      if (auto* parent = getParentComponent()) parent->resized();  // the window's holder draws this one at the new size
      break;
    case kViewLibrary:
    case kViewInspector:
      toggleSidePanel(id == kViewLibrary);
      break;
    case kSampleAuto:
      settings_.getUserSettings()->setValue("suggestSamples", !settings_.getUserSettings()->getBoolValue("suggestSamples", true));
      settings_.saveIfNeeded();
      break;
    case kExport24: exportWav(24); break;
    case kExport32: exportWav(32); break;
    case kExportMono: exportWav(24, true); break;
    case kImportMidi: importMidi(); break;
    case kExportMidi: exportMidi(); break;
    case kExportBitwig: exportBitwig(); break;
    case kImportAudio: importAudio(); break;
    case kRelocate: relocateMissing(); break;
    case kMidiNone: openMidiInput({}); break;
    case kPluginPatterns: openPatterns(); break;
    case kPluginSilent: giveSilentTracksPlugins(); break;
    case kPluginScan: scanPlugins(); break;
    case kPluginRetry: plugins_->clearQuarantine(); scanPlugins(); break;
    case kPluginFolder: {
      chooser_ = std::make_unique<juce::FileChooser>(u8("A folder with VST3 plugins"), juce::File("C:\\Program Files\\Common Files\\VST3"));
      juce::Component::SafePointer<MainComponent> safe(this);
      chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe](const juce::FileChooser& fc) {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        safe->plugins_->addFolder(fc.getResult());
        safe->scanPlugins();
      });
      break;
    }
    default:
      if (id >= kMasterFxAdd && id < kMasterFxAdd + 400) {
        const auto effects = plugins_->effects();
        if (id - kMasterFxAdd >= (int)effects.size()) break;
        const nlohmann::json plugin = effects[(size_t)(id - kMasterFxAdd)];
        int at = -1;
        edit([&](doc::Document& d) { at = doc::addPluginEffect(d, "", plugin); }, u8("Master: add ") + u8(plugin.value("name", std::string("plugin"))));
        const auto& chain = history_.current().masterEffects;
        if (at >= 0 && at < (int)chain.size()) session_->openPluginEditor(doc::pluginKey("", chain[(size_t)at].value("id", std::string())));
      } else if (id >= kMasterFx && id < kMasterFx + 60) {
        const int i = (id - kMasterFx) % 20;
        const auto& chain = history_.current().masterEffects;
        if (i >= (int)chain.size()) break;
        const std::string fxId = chain[(size_t)i].value("id", std::string());
        if (id < kMasterFx + 20) session_->openPluginEditor(doc::pluginKey("", fxId));
        else if (id < kMasterFx + 40) { fetchPluginStates(); edit([&](doc::Document& d) { doc::removeEffect(d, "", i); }, u8("Master: remove effect")); }
        else edit([&](doc::Document& d) { doc::setEffectBypass(d, "", i, !doc::effectBypassed(d.masterEffects[(size_t)i])); }, u8("Master: effect on/off"));
      } else if (id >= kMidiDevice && id < kMidiDevicesEnd) {
        const auto inputs = juce::MidiInput::getAvailableDevices();
        if (id - kMidiDevice < inputs.size()) openMidiInput(inputs[id - kMidiDevice].identifier);
      } else if (id >= kDevice && id - kDevice < (int)snap_.devices.size()) {
        selectDevice(snap_.devices[(size_t)(id - kDevice)]);
      }
      break;
  }
}

// ---------------------------------------------------------------- layout and drawing


juce::Rectangle<int> MainComponent::workArea() const {
  return getLocalBounds().withTrimmedTop(kMenuBar).withTrimmedBottom(kFooter);
}

void MainComponent::toggleSidePanel(bool library) {
  bool& shown = library ? showLibrary_ : showInspector_;
  shown = library && showPlugins_ ? true : !shown;  // from the Tools: back to the Library
  if (library) showPlugins_ = false;
  settings_.getUserSettings()->setValue(library ? "showLibrary" : "showInspector", shown);
  resized();
  repaint();
  for (auto& row : rows_) row->repaint();
}


juce::Rectangle<int> MainComponent::topBar() const { return workArea().removeFromTop(kTopBar); }
juce::Rectangle<int> MainComponent::content() const { return workArea().withTrimmedTop(kTopBar + 4).reduced(16, 0).withTrimmedBottom(16); }
// Under the top bar, right of the page rail: where the panels and pages go.
juce::Rectangle<int> MainComponent::body() const { return content().withTrimmedLeft(kRail + 8); }
juce::Rectangle<int> MainComponent::centre() const {
  auto r = body();
  if (sideShown()) r.removeFromLeft(libraryWidth() + 8);
  if (inspectorShown()) r.removeFromLeft(inspectorWidth() + 8);
  return r;
}
// The Inspector on the left, beside the tracks (Diogo 2026-11-07); folded, a thin tab.
int MainComponent::inspectorWidth() const { return inspector_ && inspector_->folded() ? InspectorPanel::kFoldedWidth : kInspectorWidth; }

bool MainComponent::inspectorShown() const { return showInspector_ && !mixerOpen_; }
// With the Mixer open the strips get the whole width, as on ref-3 (the Library comes back when it closes).
bool MainComponent::libraryShown() const { return showLibrary_ && !mixerOpen_ && !showPlugins_; }
// The Tools panel takes the Library's place (one of the two at a time).
bool MainComponent::pluginsShown() const { return showPlugins_ && !mixerOpen_; }
void MainComponent::togglePlugins() {
  showPlugins_ = !showPlugins_;
  if (showPlugins_) {
    std::string problem;
    pluginCards_.setRoles(PluginCatalog::roles(&problem));
    refreshInstalledCards();
  }
  resized();
  repaint();
  for (auto& row : rows_) row->repaint();
}

void MainComponent::refreshInstalledCards() {
  std::vector<doc::json> all = plugins_->instruments();
  for (const auto& p : plugins_->effects()) all.push_back(p);
  pluginCards_.setInstalled(all);
}
juce::Rectangle<int> MainComponent::libraryArea() const { return body().removeFromLeft(libraryWidth()); }
// Every panel's edge is dragged (Diogo 2026-11-09: "não me deixa expandir ou encurtar nenhuma janela"): the Library's
// right edge and the instrument dock's left edge, kept in the settings like the editor's height. Layout only.
int MainComponent::libraryWidth() const { return juce::jlimit(200, std::max(200, body().getWidth() / 3), libraryWidth_ > 0 ? libraryWidth_ : kLibraryWidth); }
juce::Rectangle<int> MainComponent::libraryEdge() const {
  if (!sideShown()) return {};
  const auto l = libraryArea();
  return {l.getRight(), l.getY(), 8, l.getHeight()};
}
juce::Rectangle<int> MainComponent::inspectorArea() const {
  auto r = body();
  if (sideShown()) r.removeFromLeft(libraryWidth() + 8);
  return r.removeFromLeft(inspectorWidth());
}
juce::Rectangle<int> MainComponent::timelineArea() const { return centre().withHeight(52); }
juce::Rectangle<int> MainComponent::tracksArea() const { return centre().withTrimmedTop(52 + 8).withTrimmedBottom(masterRow() + 8 + editorHeight() + 8); }
juce::Rectangle<int> MainComponent::editorArea() const { return centre().withTrimmedBottom(masterRow() + 8).removeFromBottom(editorHeight()); }

// The tracks and the editor share the height: the divider between them is dragged (kept in the settings); by
// default the editor takes a little under half.
int MainComponent::editorHeight() const {
  const int room = centre().getHeight() - (52 + 8) - (masterRow() + 8) - 8;  // the ruler and the master row stay
  // By default the tracks take what their rows need (between 30% and 55% of the room); the editor the rest.
  const int rowsNeeded = std::max(splitRows_, (int)rows_.size()) * kTrackRow + 36 + 24;  // removing a track leaves room
  // The Mixer: a compact band (kMixerHeight) so the tracks stay in view; the divider makes it taller or shorter
  // (its own height, kept in the settings apart from the editor's).
  if (mixerOpen_)
    return juce::jlimit(std::min(kMixerMinHeight, std::max(120, room - 110)), std::max(kMixerMinHeight, room - 110), mixerHeight_ > 0 ? mixerHeight_ : kMixerHeight);
  const int wanted = editorHeight_ > 0 ? editorHeight_ : room - juce::jlimit((int)(room * 0.3), (int)(room * 0.55), rowsNeeded);
  // Down to a short band (Diogo 2026-11-09: "não me deixa ir mais abaixo").
  return juce::jlimit(std::min(kEditorMinHeight, std::max(120, room - 140)), std::max(kEditorMinHeight, room - 140), wanted);
}

juce::Rectangle<int> MainComponent::dividerArea() const {
  const auto t = tracksArea();
  return {t.getX(), t.getBottom(), t.getWidth(), 8};
}

void MainComponent::resized() {
  auto footer = getLocalBounds().removeFromBottom(kFooter);
  if (aiBadge_) aiBadge_->setBounds(footer.removeFromRight(96).reduced(4, 0));
  // Top bar: the logo, the menus, the song's name; the transport (icons), tempo and position in the middle;
  // the engine's state on the right.
  auto top = topBar().withTrimmedLeft(16);
  for (auto* b : {close_.get(), maximise_.get(), minimise_.get()}) b->setBounds(top.removeFromRight(kWindowButton));
  top.removeFromRight(8);
  top.removeFromLeft(kWordmarkWidth);
  // The menus take the width their names need (the song's name starts after the last one, never over it).
  menuBar_.setSize(menuBar_.getWidth(), 28);
  int menusWidth = 8;
  const auto names = getMenuBarNames();
  for (int i = 0; i < names.size(); ++i) menusWidth += menuBar_.getLookAndFeel().getMenuBarItemWidth(menuBar_, i, names[i]);
  menusWidth = std::min(std::max(menusWidth, kMenusWidth), top.getWidth());
  menuBar_.setBounds(top.removeFromLeft(menusWidth).withSizeKeepingCentre(menusWidth, 28));
  for (auto* b : {&undo_, &redo_}) b->setBounds(top.removeFromLeft(36).reduced(2, 6));
  top.removeFromLeft(8);
  constexpr int transport = 4 * 48 + 24 + 64 + 150 + 24 + 132;
  // The page tabs come first: full names whenever the transport still fits without its gaps and the
  // "Tempo" label (the field says BPM). A narrow window gives them short names.
  constexpr int tabsFull = 96 + 80 + 64, transportMin = transport - 2 * 24 - 56;
  const bool roomy = top.getWidth() >= tabsFull + transportMin;
  arrangeButton_.setBounds(top.removeFromLeft(roomy ? 96 : 84).reduced(3, 7));
  mixerButton_.setBounds(top.removeFromLeft(roomy ? 64 : 52).reduced(3, 7));
  // The song's name on the right (as on the reference images), with what is left beyond the transport.
  // The listening level on the far right, while the transport still fits beside it.
  listen_.setVisible(top.getWidth() - 116 >= transportMin);
  if (listen_.isVisible()) listen_.setBounds(top.removeFromRight(116).reduced(4, 6));
  const int projectRoom = std::min(220, top.getWidth() - transport);
  project_.setVisible(projectRoom >= 80);
  if (project_.isVisible()) project_.setBounds(top.removeFromRight(projectRoom).reduced(4, 6));
  // The two gaps (24 px each) shrink first, then the "Tempo" label is left out (the field says what it
  // is), then the position's field shrinks (132 px down to 100: "002.01.01" still fits); the position
  // is left out only below that.
  constexpr int controls = transport - 2 * 24, positionMin = 100, labelWidth = 64;
  const int avail = top.getWidth() - (controls - 132 - labelWidth);
  const int label = avail - labelWidth >= positionMin ? labelWidth : 0;
  topGap_ = juce::jlimit(0, 24, (avail - label - 132) / 2);
  const int positionWidth = std::min(132, avail - label - 2 * topGap_);
  position_.setVisible(positionWidth >= positionMin);
  bpmLabel_.setVisible(label > 0);
  auto mid = top.withSizeKeepingCentre(std::min(controls - 132 - labelWidth + label + std::max(positionWidth, 0) + 2 * topGap_, top.getWidth()), top.getHeight());
  for (auto* b : {&home_, &play_, &stop_, &loop_}) b->setBounds(mid.removeFromLeft(48).reduced(3, 5));
  mid.removeFromLeft(topGap_);
  bpmLabel_.setBounds(mid.removeFromLeft(label));
  bpm_.setBounds(mid.removeFromLeft(150).reduced(4, 6));
  mid.removeFromLeft(topGap_);
  position_.setBounds(mid.removeFromLeft(std::max(positionWidth, 0)).reduced(2, 5));
  library_.setVisible(libraryShown());
  pluginCards_.setVisible(pluginsShown());
  pluginCards_.setBounds(libraryArea());
  library_.setBounds(libraryArea());
  if (inspector_) {
    inspector_->setVisible(inspectorShown());
    inspector_->setBounds(inspectorArea());
  }
  {
    // The footer: the engine's output and notices on the left, an export's progress, the engine's restart.
    auto bar = footer.withTrimmedTop(1);
    bar.removeFromLeft(16);
    restart_.setBounds(bar.removeFromRight(130).reduced(2, 2));
    status_.setBounds(bar.removeFromRight(110).reduced(2, 3));  // the engine's state (the top bar keeps its room for the transport)
    build_.setBounds(bar.removeFromRight(std::min(bar.getWidth() / 3, juce::GlyphArrangement::getStringWidthInt(build_.getFont(), build_.getText()) + 16)));
    if (revertAccepted_.isVisible()) revertAccepted_.setBounds(bar.removeFromRight(165).reduced(2, 2));
    if (exportBar_.isVisible()) {
      cancelExport_.setBounds(bar.removeFromRight(120).reduced(2, 2));
      exportBar_.setBounds(bar.removeFromRight(240).reduced(4, 5));
    }
    message_.setBounds(bar);
  }
  {
    // The rows keep the ruler's width; the scroll bar (when the tracks do not fit) sits in the panel's margin.
    const auto tracks = tracksArea().reduced(12);
    tracksView_->setBounds(tracks.withWidth(tracks.getWidth() + kTracksScrollBar));
    const auto lanes = laneSpan();
    timeBar_->setBounds(lanes.getX(), tracksArea().getBottom() - 11, lanes.getWidth(), 9);
    updateTimeBar();
    // At least as tall as the panel: the empty space under the last track takes the selection box.
    tracksHolder_.setSize(tracks.getWidth(), std::max((int)rows_.size() * kTrackRow + 36, tracks.getHeight()));
    boxOverlay_->setBounds(tracksView_->getBounds());
    boxOverlay_->toFront(false);
    auto inside = tracksHolder_.getLocalBounds();
    for (auto& row : rows_) row->setBounds(inside.removeFromTop(kTrackRow));
    addTrack_.setBounds(inside.removeFromTop(36).removeFromLeft(110).reduced(0, 4));
  }
  if (mixer_) mixer_->setBounds(editorArea());
  placeBottomPanels();
  {  // the page rail: the pages, then the side panels (as on the reference's left edge)
    auto rail = content().withWidth(kRail);
    for (size_t i = 0; i < rail_.size(); ++i) {
      if (i == 3) rail.removeFromTop(14);  // the side panels: Library, Tools, Inspector
      rail_[i]->setBounds(rail.removeFromTop(kRail).reduced(4));
    }
  }
}

// The Mixer's footer (ref-3): each track's CPU share, the engine's total as a bar, and the master's headroom.
void MainComponent::paintCpuFooter(juce::Graphics& g, juce::Rectangle<int> r) const {
  g.setColour(kPanel);
  g.fillRoundedRectangle(r.toFloat(), theme::kCorner);
  r = r.reduced(12, 0);
  const float peak = std::max(master_[0], master_[1]);
  const juce::String headroom = peak > 1e-5f ? juce::String(-juce::Decibels::gainToDecibels(peak), 1) + " dB" : juce::String(u8("–"));
  g.setFont(juce::FontOptions(theme::kFontBody));
  // Right side first: headroom, then the total with its bar.
  g.setColour(kMuted);
  auto right = r.removeFromRight(150);
  g.drawText(lang::tr(u8("Headroom: ") + (peak > 1e-5f && peak < 1.0f ? "+" : "") + headroom), right, juce::Justification::centredRight, false);
  auto total = r.removeFromRight(220).reduced(8, 0);
  g.drawText(lang::tr(u8("Total CPU: ") + juce::String(cpuTotal_, 1) + "%"), total.removeFromLeft(110), juce::Justification::centredLeft, false);
  const auto bar = total.withSizeKeepingCentre(total.getWidth(), 5).toFloat();
  g.setColour(kField);
  g.fillRoundedRectangle(bar, theme::kCorner);
  g.setColour(cpuTotal_ > 70 ? kRed : kAccent);
  g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, cpuTotal_ / 100.0f)), theme::kCorner);
  // Left: "CPU (Tracks)" and as many tracks as fit, divided by thin lines.
  g.setColour(theme::kTitle);
  g.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold));
  g.drawText(lang::tr(u8("CPU (Tracks)")), r.removeFromLeft(96), juce::Justification::centredLeft, false);
  const juce::Font name{juce::FontOptions(theme::kFontSmall)};
  for (const auto& [n, pct] : cpuTracks_) {
    const juce::String label = juce::String(u8(n)).toUpperCase(), value = juce::String(pct, 1) + "%";
    const int w = juce::GlyphArrangement::getStringWidthInt(name, label) + juce::GlyphArrangement::getStringWidthInt(name, value) + 30;
    if (w > r.getWidth()) break;
    auto cell = r.removeFromLeft(w);
    g.setColour(theme::kGrid);
    g.fillRect(cell.getX(), cell.getCentreY() - 6, 1, 12);
    cell.removeFromLeft(10);
    g.setFont(name);
    g.setColour(kMuted);
    g.drawText(label, cell, juce::Justification::centredLeft, false);
    g.setColour(kText);
    g.drawText(value, cell.withTrimmedRight(10), juce::Justification::centredRight, false);
  }
}

juce::Rectangle<int> MainComponent::laneSpan() const {
  const auto rows = tracksArea().reduced(12);
  return rows.withTrimmedLeft(kTrackHead).withY(timelineArea().getY()).withHeight(timelineArea().getHeight());
}

// The song plus some room after its end (a clip dragged there, or a double click, makes the song longer).
double MainComponent::viewSpan() const {
  const int64_t song = std::max<int64_t>(1, history_.current().lengthTicks);
  return (double)(song + std::max<int64_t>(2 * kBar, (song / 8 + kBar - 1) / kBar * kBar));
}

double MainComponent::viewLength() const {
  const double song = viewSpan();
  return viewLength_ > 0 ? std::min(viewLength_, song) : song;
}

double MainComponent::viewStart() const {
  const double song = viewSpan();
  return juce::jlimit(0.0, std::max(0.0, song - viewLength()), viewStart_);
}

void MainComponent::setView(double start, double length) {
  const double song = viewSpan();
  viewLength_ = juce::jlimit((double)kBeat, song, length);
  viewStart_ = juce::jlimit(0.0, std::max(0.0, song - viewLength_), start);
  if (viewLength_ >= song) viewLength_ = 0;  // the whole song: follows its length
  updateTimeBar();
  repaint();
  for (auto& row : rows_) row->repaint();
}

int MainComponent::xOfTick(double tick) const {
  const auto r = laneSpan();
  return r.getX() + (int)std::round(r.getWidth() * (tick - viewStart()) / viewLength());
}

int64_t MainComponent::gridStep(int minPixels) const {
  const double pixelsPerTick = laneSpan().getWidth() / std::max(1.0, viewLength());
  for (const int64_t step : {kBeat / 16, kBeat / 8, kBeat / 4, kBeat})
    if (step * pixelsPerTick >= minPixels) return step;
  return kBar;
}

int64_t MainComponent::tickAt(int x, int64_t snap, bool pastEnd) const {
  const auto r = laneSpan();
  const double tick = viewStart() + (x - r.getX()) / (double)std::max(1, r.getWidth()) * viewLength();
  const int64_t length = pastEnd ? (int64_t)viewSpan() : history_.current().lengthTicks;
  return juce::jlimit<int64_t>(0, length, (int64_t)std::round(tick / snap) * snap);
}

void MainComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& w) {
  if (event.eventComponent == &tracksHolder_) return;  // (the box's listener: the tracks' panel already has it)
  const auto e = event.getEventRelativeTo(this);
  const auto lanes = laneSpan().withBottom(tracksArea().getBottom());
  if (!lanes.contains(e.getPosition())) return;
  const double delta = std::abs(w.deltaX) > std::abs(w.deltaY) ? -w.deltaX : w.deltaY;
  if (e.mods.isCtrlDown() || e.mods.isCommandDown()) {
    const double at = viewStart() + (e.x - laneSpan().getX()) / (double)std::max(1, laneSpan().getWidth()) * viewLength();
    if (delta == 0) return;
    const double length = juce::jlimit((double)kBeat, viewSpan(), viewLength() * std::pow(2.0, -delta * 2.0));  // a notch: about 15%
    setView(at - (at - viewStart()) * length / viewLength(), length);
  } else {
    setView(viewStart() - delta * viewLength() * 0.5, viewLength());
  }
}

bool MainComponent::panKeyHeld(const juce::ModifierKeys&) const {
  return core::app::Keymap::get().held("view.panHold");  // Settings > Keyboard shortcuts (Alt by default)
}

// A new loop region is marked only on purpose: this key held (Ctrl by default) while dragging on the ruler.
bool MainComponent::loopKeyHeld() const {
  return core::app::Keymap::get().held("transport.loopHold");  // Settings > Keyboard shortcuts
}

bool MainComponent::startPan(const juce::MouseEvent& e) {
  if (!(e.mods.isMiddleButtonDown() || (e.mods.isLeftButtonDown() && panKeyHeld(e.mods)))) return false;
  panning_ = true;
  panFrom_ = e.getScreenPosition();
  panStart_ = viewStart();
  panScrollY_ = tracksView_ ? tracksView_->getViewPositionY() : 0;
  if (viewLength() >= viewSpan() - 1) notice_ = u8("The whole song is in view: zoom in first (Ctrl+wheel), then drag to move along it");
  e.eventComponent->setMouseCursor(juce::MouseCursor::DraggingHandCursor);
  return true;
}

void MainComponent::dragPan(const juce::MouseEvent& e) {
  const auto at = e.getScreenPosition();
  const double ticksPerPixel = viewLength() / std::max(1, laneSpan().getWidth());
  setView(panStart_ - (at.x - panFrom_.x) * ticksPerPixel, viewLength());
  if (tracksView_) tracksView_->setViewPosition(tracksView_->getViewPositionX(), std::max(0, panScrollY_ - (at.y - panFrom_.y)));
}

void MainComponent::endPan() {
  panning_ = false;
  for (auto& row : rows_) row->setMouseCursor(juce::MouseCursor::NormalCursor);
  setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MainComponent::paint(juce::Graphics& g) {
  g.fillAll(kBackground);
  auto footer = getLocalBounds().removeFromBottom(kFooter);
  g.setColour(kPanel);
  g.fillRect(footer);
  g.setColour(kOutline);
  g.fillRect(footer.removeFromTop(1));
  {
    // Top bar: the logo, separators between the transport, tempo and position groups, the position's field.
    const auto top = topBar().withTrimmedLeft(16);
    theme::drawWordmark(g, top.withWidth(kWordmarkWidth - 20).withSizeKeepingCentre(kWordmarkWidth - 20, 28).toFloat());  // a 26 px C: big enough for its gaps and the diamond to show
    g.setColour(kOutline);
    if (topGap_ >= 8)
      for (int x : {loop_.getRight() + topGap_ / 2, bpm_.getRight() + topGap_ / 2}) g.fillRect(x, top.getY() + 10, 1, top.getHeight() - 20);
    if (position_.isVisible()) {
      const auto field = position_.getBounds().toFloat();
      g.setColour(kField);
      g.fillRoundedRectangle(field, theme::kCorner);
      g.setColour(kOutline);
      g.drawRoundedRectangle(field.reduced(0.5f), theme::kCorner, 1);
    }
  }
  const doc::Document& d = history_.current();

  // Timeline: bars, loop region, playhead.
  const auto tl = timelineArea();
  g.setColour(kPanel);
  g.fillRoundedRectangle(tl.toFloat(), theme::kCorner);
  const auto ruler = laneSpan();
  g.saveState();  // the ruler draws only in its strip
  g.reduceClipRegion(ruler.withLeft(tl.getX() + 10));
  // Sections: named bands on the top half of the ruler.
  g.setFont(juce::FontOptions(theme::kFontBody));
  g.setColour(kMuted);
  g.drawText(lang::tr(u8("Sections")), tl.getX() + 10, tl.getY() + 4, kTrackHead - 20, 16, juce::Justification::centredLeft);
  g.drawText(lang::tr(u8("Bars")), tl.getX() + 10, tl.getY() + 26, kTrackHead - 20, 16, juce::Justification::centredLeft);
  g.setFont(juce::FontOptions(theme::kFontSmall));
  g.drawText(lang::tr(u8("Ctrl+wheel: zoom")), tl.getX() + 10, tl.getBottom() - 14, kTrackHead - 20, 12, juce::Justification::centredLeft);
  g.reduceClipRegion(ruler);
  // Sections: slate bands with their names in capitals, as on the reference.
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold).withKerningFactor(0.08f));
  for (size_t i = 0; i < d.sections.size(); ++i) {
    const auto& s = d.sections[i];
    const auto band = juce::Rectangle<int>::leftTopRightBottom(xOfTick((double)s.startTick), ruler.getY() + 3, xOfTick((double)(s.startTick + s.lengthTicks)), ruler.getY() + 20);
    const juce::Colour colour = sectionColour(s);  // its own, or the one of its type (intro, build-up, drop…)
    g.setColour(kClip.interpolatedWith(colour, 0.38f));
    g.fillRoundedRectangle(band.toFloat().reduced(1, 0), theme::kCorner);
    g.setColour(colour.withAlpha(0.95f));
    g.fillRect(band.getX() + 3, band.getY(), std::max(0, band.getWidth() - 6), 2);
    g.setColour(kText.withAlpha(0.9f));
    g.drawText(u8(s.name).toUpperCase(), band.reduced(4, 0), juce::Justification::centred, true);
  }
  const int64_t bars = std::max<int64_t>(1, d.lengthTicks / kBar);
  const double barPixels = ruler.getWidth() * kBar / viewLength();
  const int every = barPixels >= 28 ? 1 : barPixels >= 14 ? 2 : barPixels >= 7 ? 4 : barPixels >= 2 ? 16 : 64;
  g.setFont(juce::FontOptions(theme::kFontBody));
  const int64_t firstBar = (int64_t)(viewStart() / kBar), lastBar = std::min<int64_t>(bars, (int64_t)((viewStart() + viewLength()) / kBar) + 1);
  if (const int64_t tickStep = gridStep(12); tickStep < kBar) {  // zoomed in: a short tick per beat, a shorter one per grid step
    for (int64_t t = firstBar * kBar; t <= lastBar * kBar; t += tickStep) {
      if (t % kBar == 0) continue;
      const bool beat = t % kBeat == 0;
      g.setColour(beat ? kOutline : kOutline.withAlpha(0.5f));
      g.drawVerticalLine(xOfTick((double)t), (float)ruler.getBottom() - (beat ? 9.0f : 5.0f), (float)ruler.getBottom() - 2);
    }
  }
  for (int64_t b = firstBar; b <= lastBar; ++b) {
    if (every > 1 && b % every) continue;
    const int x = xOfTick((double)(b * kBar));
    g.setColour(kOutline);
    g.drawVerticalLine(x, (float)ruler.getY() + 38, (float)ruler.getBottom() - 2);
    // A tempo change at (or just after) this bar number takes its place on the ruler.
    const bool tempoHere = std::any_of(d.tempoMap.begin(), d.tempoMap.end(), [&](const doc::TempoPoint& t) {
      const int tx = xOfTick((double)t.tick);
      return tx >= x - 40 && tx < x + 68;
    });
    if (b % every == 0 && b < bars && !tempoHere) {
      g.setColour(kMuted);
      g.drawText(juce::String(b + 1), x + 3, ruler.getY() + 24, 40, 14, juce::Justification::centredLeft);
    }
  }
  // The end of the song: after it, room to drag clips into (the song grows to hold them).
  if (const int endX = xOfTick((double)d.lengthTicks); endX < ruler.getRight()) {
    g.setColour(kBackground.withAlpha(0.55f));
    g.fillRect(juce::Rectangle<int>::leftTopRightBottom(endX, ruler.getY() + 22, ruler.getRight(), ruler.getBottom()));
    g.setColour(kMuted);
    g.fillRect(endX, ruler.getY() + 22, 1, ruler.getHeight() - 22);
    g.setFont(juce::FontOptions(theme::kFontSmall));
    g.drawText(lang::tr(u8("End")), endX + 4, ruler.getY() + 24, 40, 14, juce::Justification::centredLeft);
  }
  // Tempo changes (the song's tempo map): a small accent tick with the new BPM, on the bar numbers.
  g.setFont(juce::FontOptions(theme::kFontSmall, juce::Font::bold));
  for (const auto& t : d.tempoMap) {
    const int x = xOfTick((double)t.tick);
    g.setColour(kAccent);
    g.fillRect((float)x - 1, (float)ruler.getY() + 22, 2.0f, 16.0f);
    g.drawText(lang::tr(juce::String(t.bpm, t.bpm == std::round(t.bpm) ? 0 : 1) + " BPM"), x + 4, ruler.getY() + 24, 64, 14, juce::Justification::centredLeft);
  }
  const bool dragging = dragStartTick_ >= 0 && dragTick_ != dragStartTick_;
  const int64_t loopStart = dragging ? std::min(dragStartTick_, dragTick_) : d.loop.startTick;
  const int64_t loopEnd = dragging ? std::max(dragStartTick_, dragTick_) : d.loop.endTick;
  const auto loopArea = juce::Rectangle<int>::leftTopRightBottom(xOfTick((double)loopStart), ruler.getY() + 38, xOfTick((double)loopEnd), ruler.getBottom() - 3);
  const juce::Colour loopColour = d.loop.enabled || dragging ? kAccent : kMuted;
  g.setColour(loopColour.withAlpha(0.22f));
  g.fillRect(loopArea);
  g.setColour(loopColour.withAlpha(0.8f));
  g.drawRect(loopArea, 1);
  // The start line (the playhead): its own warm line with a small handle on the bar numbers.
  const float px = (float)xOfTick(shownPosition());
  g.setColour(theme::kStartLine);
  g.fillRect(px - 0.5f, (float)ruler.getY() + 22, 1.0f, (float)ruler.getHeight() - 22);
  juce::Path handle;
  handle.addRoundedRectangle(px - 5, (float)ruler.getY() + 22, 10, 9, 2);
  handle.addTriangle(px - 5, (float)ruler.getY() + 30, px + 5, (float)ruler.getY() + 30, px, (float)ruler.getY() + 35);
  g.fillPath(handle);
  g.restoreState();

  // Tracks panel and master.
  auto area = tracksArea();
  g.setColour(kPanel);
  g.fillRoundedRectangle(area.toFloat(), theme::kCorner);
  if (!loadProblem_.isEmpty()) {
    g.setColour(kRed);
    g.setFont(juce::FontOptions(theme::kFontTitle));
    g.drawFittedText(loadProblem_, area.reduced(12).removeFromTop(40), juce::Justification::centredLeft, 2);
  }
  if (mixerOpen_) { paintCpuFooter(g, centre().removeFromBottom(masterRow())); return; }
  auto master = centre().removeFromBottom(64).reduced(12, 10);
  g.setFont(juce::FontOptions(theme::kFontTitle));
  g.setColour(kText);
  g.drawText(lang::tr("Master"), master.removeFromLeft(186), juce::Justification::centredLeft);
  g.setColour(kMuted);
  g.drawText("L / R", master.removeFromLeft(80 + 60 + 8 + 140 + 12), juce::Justification::centredLeft);
  drawMeter(g, master.toFloat(), master_[0], master_[1]);
}

// ---------------------------------------------------------------- input

void MainComponent::playStop() {
  if (patternPlay_) { stopPatternSlot(); snap_.playing = false; return; }  // a pattern playing alone stops first
  if (!snap_.playing) {
    // The loop on and the start line outside it: Play starts at the loop's start (the line goes there too).
    const int64_t at = (int64_t)shownPosition();
    if (const int64_t from = doc::playStartTick(history_.current().loop, at); from != at) moveShownPosition(from);
  }
  snap_.playing ? session_->stop() : session_->play();
  snap_.playing = !snap_.playing;  // a second press before the next refresh toggles again
}

bool MainComponent::keyPressed(const juce::KeyPress& key) {
  // The shortcuts are the keymap's (File > Settings > Keyboard shortcuts); Esc and the typing keyboard's notes are fixed.
  const Keymap& keys = Keymap::get();
  auto is = [&](const char* id) { return keys.matches(id, key); };
  const int code = key.getKeyCode();
  if (is("transport.playStop")) { playStop(); return true; }  // GlobalKeys takes it first, in every window
  if (is("transport.home")) { session_->home(); return true; }
  if (is("transport.end")) { moveShownPosition(history_.current().lengthTicks); return true; }  // the song's end
  // The position a bar, a beat or 4 bars back or on; the view follows it.
  struct Move { const char* id; int64_t step; bool back; };
  static constexpr Move kMoves[] = {{"transport.barBack", kBar, true},   {"transport.barForward", kBar, false},
                                    {"transport.beatBack", kBeat, true}, {"transport.beatForward", kBeat, false},
                                    {"transport.bar4Back", 4 * kBar, true}, {"transport.bar4Forward", 4 * kBar, false}};
  for (const Move& m : kMoves) {
    const int64_t step = m.step;
    const bool back = m.back;
    if (is(m.id)) {
      const int64_t now = (int64_t)shownPosition();
      const int64_t to = back ? std::max<int64_t>(0, (now - 1) / step * step) : std::min<int64_t>(history_.current().lengthTicks, now / step * step + step);
      moveShownPosition(to);
      if ((double)to < viewStart() || (double)to > viewStart() + viewLength()) setView((double)to - viewLength() * 0.25, viewLength());
      return true;
    }
  }
  if (is("view.pageBack") || is("view.pageForward")) {  // the view a screen back or on
    setView(viewStart() + (is("view.pageBack") ? -0.9 : 0.9) * viewLength(), viewLength());
    return true;
  }
  if (is("view.wholeSong")) { setView(0, viewSpan()); return true; }
  if (key == juce::KeyPress::escapeKey && (automationOpen_ || scoreOpen_)) { closePanels(); return true; }
  if (is("edit.redo")) { redo(); return true; }
  if (is("edit.undo")) { undo(); return true; }
  if (is("file.saveAs")) { save(true); return true; }
  if (is("file.save")) { save(false); return true; }
  if (is("file.open")) { open(); return true; }
  if (is("file.newTemplate")) { showTemplates(); return true; }  // the Library's Templates tab
  if (is("file.new")) { newProject(false); return true; }
  if (is("file.export") && !snap_.exportJob.active()) { exportWav(24); return true; }
  if (is("file.importAudio")) { importAudio(); return true; }
  if (is("file.settings")) { openSettings(false); return true; }
  if (is("edit.addTrack")) { showAddTrackMenu(); return true; }
  if (is("view.mixer")) { setMixerOpen(!mixerOpen_); return true; }
  if (is("view.patterns")) { openPatterns(); return true; }
  if (is("view.effects")) {  // the selected track's plugins
    if (const std::string id = selectedTrack_.empty() ? editor_.track() : selectedTrack_; !id.empty()) showTrackMenu(id);
    return true;
  }
  if (is("transport.loop")) { loop_.triggerClick(); return true; }
  if (is("keys.octaveUp") || is("keys.octaveDown")) {
    keyboardOctave_ = juce::jlimit(0, 8, keyboardOctave_ + (is("keys.octaveUp") ? 1 : -1));
    notice_ = u8("Keyboard: Z = C") + juce::String(keyboardOctave_) + u8(" (arrows change the octave)");
    return true;
  }
  if (!key.getModifiers().isAnyModifierKeyDown() && pressPianoKey(code)) return true;
  const bool del = is("edit.delete");
  if (del && audioEditor_->isVisible() && audioEditor_->hasSelection()) {
    audioEditor_->removeSelection();
    return true;
  }
  // Clips: copy, cut, paste at the playhead (on a beat), duplicate after the selection, as in Ableton.
  if (is("transport.loopSelection")) {  // Ableton's Ctrl+L: the loop around the selected clips, on
    std::vector<CopiedClip> keep = copiedClips_;
    const int64_t f = copiedFrom_, t = copiedTo_;
    if (copyClips()) {
      const int64_t a = copiedFrom_, b = copiedTo_;
      edit([&](doc::Document& d) { d.loop = {true, a, b, d.loop.extra}; }, u8("Loop the selection"));
      notice_ = u8("Loop around the selected clips.");
    }
    copiedClips_ = std::move(keep); copiedFrom_ = f; copiedTo_ = t;  // Ctrl+L does not change what Ctrl+C copied
    return true;
  }
  if (is("edit.copy")) { copyClips(); return true; }
  if (is("edit.cut")) { cutClips(); return true; }
  if (is("edit.paste")) { pasteClips((int64_t)shownPosition() / kBeat * kBeat); return true; }
  if (is("edit.duplicate") && !selectedClipKeys().empty()) { duplicateClips(); return true; }
  if (del && boxClips_.size() > 1) { deleteBoxClips(); return true; }
  if (del && !selectedClip_.empty()) {
    const std::string trackId = selectedTrack_, clipId = selectedClip_;
    edit([&](doc::Document& d) {
      doc::Track& t = doc::trackById(d, trackId);
      if (t.instrument.kind == "audio") doc::removeAudioClip(d, trackId, clipId);
      else doc::removeClip(t, clipId);
    }, "Delete clip");
    selectClip(trackId, {});
    return true;
  }
  return false;
}

// Which end of the loop region is under the mouse (on its strip under the bar numbers): 1 start, 2 end, 0 none.
int MainComponent::loopEdgeAt(juce::Point<int> p) const {
  const doc::Loop& loop = history_.current().loop;
  const auto ruler = laneSpan();
  if (!timelineArea().contains(p) || p.y < ruler.getY() + 34 || loop.endTick <= loop.startTick) return 0;
  const int ds = std::abs(p.x - xOfTick((double)loop.startTick)), de = std::abs(p.x - xOfTick((double)loop.endTick));
  if (std::min(ds, de) > 8) return 0;
  return de <= ds ? 2 : 1;  // the end first: a one-bar loop zoomed out still shrinks from its end
}

void MainComponent::mouseMove(const juce::MouseEvent& e) {
  if (e.eventComponent == &tracksHolder_) return;
  setMouseCursor(dividerArea().contains(e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor
                 : libraryEdge().contains(e.getPosition()) || loopEdgeAt(e.getPosition())
                     ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseDown(const juce::MouseEvent& e) {
  if (e.eventComponent == &tracksHolder_) {  // under the last track
    if (e.mods.isPopupMenu() || startPan(e)) return;
    startBox(e);
    return;
  }
  if (libraryEdge().contains(e.getPosition())) { sideGrab_ = 1; return; }
  if (dividerArea().contains(e.getPosition())) {
    // A double-click: the editor as tall as it gets, or back to the default share.
    if (e.getNumberOfClicks() >= 2 && mixerOpen_) {  // the Mixer: its shortest, or back to the default
      mixerHeight_ = editorHeight() > kMixerMinHeight ? kMixerMinHeight : 0;
      settings_.getUserSettings()->setValue("mixerHeight", mixerHeight_);
      resized();
      repaint();
      return;
    }
    if (e.getNumberOfClicks() >= 2) {
      editorHeight_ = editorHeight_ > 0 && editorHeight_ >= centre().getHeight() / 2 ? 0 : 10000;
      editorHeight_ = editorHeight_ > 0 ? editorHeight() : 0;  // within the limits
      settings_.getUserSettings()->setValue("editorHeight", editorHeight_);
      resized();
      repaint();
      return;
    }
    dividerGrab_ = e.y - dividerArea().getY();
    return;
  }
  if (!timelineArea().contains(e.getPosition()) || e.x < laneSpan().getX()) return;
  if (startPan(e)) return;
  if (e.mods.isPopupMenu()) { showSectionMenu(tickAt(e.x, 1)); return; }
  // The playhead's handle (on the bar numbers): drag it, and the song follows (to the beat).
  const auto ruler = laneSpan();
  if (std::abs(e.x - xOfTick(shownPosition())) <= 8 && e.y >= ruler.getY() + 18 && e.y < ruler.getY() + 38) {
    scrubbing_ = true;
    moveShownPosition(tickAt(e.x, kBeat));
    return;
  }
  // An end of the loop region (on its strip, under the bar numbers): drag it alone; the loop stays on or off.
  const doc::Loop& loop = history_.current().loop;
  loopEdge_ = false;
  if (const int end = loopEdgeAt(e.getPosition())) {
    dragStartTick_ = end == 1 ? loop.endTick : loop.startTick;
    loopEdge_ = true;
    dragTick_ = tickAt(e.x, loopSnap());
    return;
  }
  // A double-click on the loop region removes it.
  if (e.getNumberOfClicks() >= 2 && e.y >= ruler.getY() + 38 && loop.endTick > loop.startTick && e.x > xOfTick((double)loop.startTick) &&
      e.x < xOfTick((double)loop.endTick)) {
    edit([](doc::Document& d) { d.loop = {false, 0, 0, d.loop.extra}; }, u8("Remove loop"));
    return;
  }
  // A drag on the loop strip (under the bar numbers, as Ableton's loop bar), or Ctrl (held) + drag anywhere on the
  // ruler: a new loop region, and the position stays where it was. A click there without dragging only moves the
  // position (mouseUp). On the bar numbers a click or a drag moves the position (to the beat).
  // Diogo 2026-11-08: "é difícil escolher bars para loop: ao carregar no sítio dele vai para lá o start line".
  if (loopKeyHeld() || e.y >= ruler.getY() + 38) {
    dragStartTick_ = dragTick_ = tickAt(e.x, loopSnap());
    return;
  }
  scrubbing_ = true;
  rulerScrub_ = true;
  moveShownPosition(tickAt(e.x, kBeat));
}

// A loop region snaps to the bar, or finer (down to 1/16) when zoomed in far enough to see it.
int64_t MainComponent::loopSnap() const { return std::max<int64_t>(kBeat / 4, gridStep(16)); }

void MainComponent::mouseDrag(const juce::MouseEvent& e) {
  if (e.eventComponent == &tracksHolder_) {
    if (panning_) dragPan(e);
    else if (boxing_) dragBox(e);
    return;
  }
  if (sideGrab_ > 0) {  // a side panel's edge follows the mouse
    libraryWidth_ = e.x - libraryArea().getX() - 4;
    libraryWidth_ = libraryWidth();
    resized();
    repaint();
    for (auto& row : rows_) row->repaint();
    return;
  }
  if (dividerGrab_ >= 0) {
    // The editor's top follows the mouse; its bottom stays (over the master row).
    const int h = editorArea().getBottom() - (e.y - dividerGrab_ + 8);
    if (mixerOpen_) {
      mixerHeight_ = h;
      mixerHeight_ = editorHeight();  // within the limits
    } else {
      editorCollapsed_ = false;
      editorHeight_ = h;
      editorHeight_ = editorHeight();
    }
    resized();
    repaint();
    return;
  }
  if (panning_) { dragPan(e); return; }
  if (scrubbing_) {
    moveShownPosition(tickAt(e.x, kBeat));
    if (rulerScrub_ && e.getDistanceFromDragStart() > 12) {  // the hint, once per drag
      notice_ = u8("Moving the position. To mark a loop: drag on the strip under the bar numbers (or hold ") +
                core::app::Keymap::get().text("transport.loopHold") + u8(" and drag on the ruler), or select clips and press ") +
                core::app::Keymap::get().text("transport.loopSelection") + ".";
      rulerScrub_ = false;
    }
    return;
  }
  if (dragStartTick_ < 0) return;
  dragTick_ = tickAt(e.x, loopSnap());
  repaint(timelineArea());
}

// Click or drag: move the position (to the beat). Ctrl + drag: mark a new loop region (to the grid) and turn
// the loop on; dragging an end of the region moves only that end.
void MainComponent::mouseUp(const juce::MouseEvent& e) {
  if (e.eventComponent == &tracksHolder_) {
    if (panning_) endPan();
    else if (boxing_) endBox(e);
    return;
  }
  if (sideGrab_ > 0) {
    settings_.getUserSettings()->setValue("libraryWidth", libraryWidth_);
    sideGrab_ = 0;
    return;
  }
  if (dividerGrab_ >= 0) {
    dividerGrab_ = -1;
    settings_.getUserSettings()->setValue(mixerOpen_ ? "mixerHeight" : "editorHeight", mixerOpen_ ? mixerHeight_ : editorHeight_);
    return;
  }
  if (panning_) { endPan(); return; }
  if (scrubbing_) { scrubbing_ = rulerScrub_ = false; moveShownPosition(tickAt(e.x, kBeat)); return; }
  if (dragStartTick_ < 0) return;
  const int64_t a = dragStartTick_, b = tickAt(e.x, loopSnap());
  const bool edge = loopEdge_;
  dragStartTick_ = dragTick_ = -1;
  loopEdge_ = false;
  if (!edge && (e.getDistanceFromDragStart() < 4 || a == b)) {
    // Ctrl + click on the bar numbers: still just the position. A click on the loop strip changes nothing: the
    // start line stays where it was (Diogo 2026-11-08).
    if (e.getMouseDownY() < laneSpan().getY() + 38) moveShownPosition(tickAt(e.x, kBeat));
  } else if (a != b) {
    edit([&](doc::Document& d) { d.loop = {edge ? d.loop.enabled : true, std::min(a, b), std::max(a, b), d.loop.extra}; }, "Loop");
  }
  repaint(timelineArea());
}

// The engine is told at once; the ruler and the lanes draw the new place now, not when the engine's report comes back.
void MainComponent::moveShownPosition(int64_t tick) {
  tick = std::max<int64_t>(0, tick);
  if (tick != (int64_t)shownTick_) session_->seek(tick);
  shownTick_ = (double)tick;
  shownUntil_ = juce::Time::getMillisecondCounter() + 300;
  const int x = xOfTick(shownTick_);
  if (x != playheadX_) {
    for (auto& row : rows_) {
      const int rx = getLocalPoint(row.get(), juce::Point<int>()).x;
      if (playheadX_ >= 0) row->repaint(playheadX_ - rx - 1, 0, 3, row->getHeight());
      row->repaint(x - rx - 1, 0, 3, row->getHeight());
    }
    playheadX_ = x;
  }
  repaint(timelineArea());
}

// ---------------------------------------------------------------- updates (30 per second)

// The engine reports where it plays about 21 times a second, on its own clock: drawn as it comes, the playhead
// moves in uneven steps against the 30 frames. Between reports the position goes on at the song's tempo, at
// most 120 ms past the last one (an engine that stalls stops the line) and never past the loop's end. The
// engine's position is what it renders: the line shows what is heard, the device's latency later. A report
// a little behind the guess does not pull the line back (a jump of more than a 16th, a loop or a seek, does).
void MainComponent::updatePlayTick() {
  double tick = snap_.positionTick;
  if (snap_.playing && snap_.positionAtMs > 0) {
    const auto& d = history_.current();
    const double ms = std::clamp(juce::Time::getMillisecondCounterHiRes() - snap_.positionAtMs, 0.0, 120.0) - snap_.outputLatencyMs;
    tick += ms * std::clamp(doc::bpmAt(d, (int64_t)tick), 40.0, 600.0) * doc::kPpq / 60000.0;
    if (d.loop.enabled && snap_.positionTick < (double)d.loop.endTick) tick = std::min(tick, (double)d.loop.endTick);
    if (d.loop.enabled && snap_.positionTick >= (double)d.loop.startTick) tick = std::max(tick, (double)d.loop.startTick);
    tick = std::max(tick, 0.0);
    if (tick < playTick_ && playTick_ - tick < doc::kPpq / 4.0) tick = playTick_;
  }
  playTick_ = tick;
}

void MainComponent::timerCallback() {
  // Space and the other shortcuts work on every page: a button, slider or list that took the keyboard
  // focus with a click gives it back (it would eat Space as its own click). Text fields and the editors
  // with their own keys (piano roll: Delete, arrows) keep it.
  if (auto* focused = juce::Component::getCurrentlyFocusedComponent();
      focused != nullptr && focused != this && isParentOf(focused) && !juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown() &&
      (dynamic_cast<juce::Button*>(focused) != nullptr || dynamic_cast<juce::Slider*>(focused) != nullptr ||
       dynamic_cast<juce::ComboBox*>(focused) != nullptr) && isShowing())
    grabKeyboardFocus();
  if (sendPending_) { sendPending_ = false; session_->setDocument(engineDocument()); }  // a drag's latest state
  snap_ = session_->snapshot();
  // A plugin's own window changed its sound: the song has unsaved changes (its state is taken when it is saved).
  if (snap_.pluginEdits != pluginEditsSeen_) { pluginEditsSeen_ = snap_.pluginEdits; pluginsDirty_ = true; }
  updatePlayTick();
  // The place just clicked stays shown until the engine reports it (or 2 s pass): never back to the old place
  // for a moment when the engine's report is late (paused, or the PC busy).
  if (!scrubbing_ && shownTick_ >= 0 && juce::Time::getMillisecondCounter() > shownUntil_ &&
      (std::abs(snap_.positionTick - shownTick_) <= (double)kBar || juce::Time::getMillisecondCounter() > shownUntil_ + 1700))
    shownTick_ = -1;
  collectSampleScan();
  collectSampleAnalysis();
  pollBridge();
  // The side panels follow the selection and the sample index.
  if (const void* data = hasSampleIndex_ ? (const void*)sampleIndex_.samples.data() : nullptr;
      data != libraryIndexData_ || (hasSampleIndex_ ? sampleIndex_.samples.size() : 0) != libraryIndexSize_) {
    libraryIndexData_ = data;
    libraryIndexSize_ = hasSampleIndex_ ? sampleIndex_.samples.size() : 0;
    library_.indexChanged();
  }
  if (const std::string t = selectedTrack_ + "|" + editor_.track(); t != libraryTrackSeen_) { libraryTrackSeen_ = t; library_.refresh(); }
  library_.poll();
  if (inspector_) inspector_->update(selectedTrack_.empty() ? editor_.track() : selectedTrack_, selectedClip_);
  {
    juce::String name = u8(history_.current().name).toUpperCase();
    if (name.isEmpty()) name = "UNTITLED";
    if (project_.getButtonText() != name) project_.setButtonText(name);
  }
  constexpr float fall = 0.88f;  // per frame at 30 Hz: about -1.1 dB
  for (int c = 0; c < 2; ++c) master_[c] = std::max(snap_.masterPeak[c], master_[c] * fall);
  for (auto& [id, m] : meters_) { m.first *= fall; m.second *= fall; }
  for (const auto& [id, peak] : snap_.trackPeaks) {
    auto& m = meters_[id];
    m.first = std::max(m.first, peak.first);
    m.second = std::max(m.second, peak.second);
  }
  if (mixer_) mixer_->setLevels(meters_, master_[0], master_[1]);
  if (mixer_ && mixerOpen_ && !cpuAsked_ && juce::Time::getMillisecondCounter() - cpuAskedAt_ >= 1000) {
    cpuAsked_ = true;
    cpuAskedAt_ = juce::Time::getMillisecondCounter();
    session_->cpuAsync(2000, [safe = juce::Component::SafePointer<MainComponent>(this)](json cpu) {
      if (!safe) return;
      safe->cpuAsked_ = false;
      if (!safe->mixer_ || !cpu.contains("tracks")) return;  // no answer (engine restarting): keep the last values
      std::map<std::string, float> tracks;
      for (const auto& t : cpu["tracks"]) tracks[t.value("id", std::string())] = (float)t.value("cpuPct", 0.0);
      safe->mixer_->setCpu(tracks, (float)cpu["master"].value("cpuPct", 0.0));
      safe->cpuTracks_.clear();
      for (const auto& t : safe->history_.current().tracks) {
        const auto it = tracks.find(t.id);
        safe->cpuTracks_.emplace_back(t.name, it == tracks.end() ? 0.0f : it->second);
      }
      safe->cpuTotal_ = (float)cpu.value("totalPct", 0.0);
      safe->repaint(safe->centre().removeFromBottom(safe->masterRow()));
    });
  }
  for (auto& row : rows_) {
    auto it = meters_.find(row->id());
    row->setLevels(it == meters_.end() ? 0 : it->second.first, it == meters_.end() ? 0 : it->second.second);
  }
  const int64_t tick = (int64_t)shownPosition();
  // Bar, beat and sixteenth, as a counter: 009.01.01.
  position_.setText(juce::String(tick / kBar + 1).paddedLeft('0', 3) + "." + juce::String((tick / kBeat) % 4 + 1).paddedLeft('0', 2) + "." +
                        juce::String((tick % kBeat) / (kBeat / 4) + 1).paddedLeft('0', 2),
                    juce::dontSendNotification);
  if (const int x = xOfTick(shownPosition()); x != playheadX_) {
    for (auto& row : rows_) {
      const int rx = getLocalPoint(row.get(), juce::Point<int>()).x;
      if (playheadX_ >= 0) row->repaint(playheadX_ - rx - 1, 0, 3, row->getHeight());
      row->repaint(x - rx - 1, 0, 3, row->getHeight());
    }
    playheadX_ = x;
  }

  juce::String status = statusText(snap_.status);
  // The engine's life in the log: start, ready, crash, restart (not each edit: Syncing counts as Ready).
  const EngineStatus logged = snap_.status == EngineStatus::Syncing ? EngineStatus::Ready : snap_.status;
  if (logged != loggedStatus_) {
    loggedStatus_ = logged;
    logLine(juce::String("Engine: ") + statusText(logged) + (snap_.message.empty() ? juce::String() : " - " + u8(snap_.message)));
  }
  if (snap_.status == EngineStatus::Syncing && snap_.acceptedRevision < snap_.documentRevision) status = u8("Sending...");
  status_.setText(status, juce::dontSendNotification);
  status_.setColour(juce::Label::backgroundColourId, statusColour(snap_.status).withAlpha(0.25f));
  status_.setColour(juce::Label::textColourId, statusColour(snap_.status));
  const bool refused = snap_.status == EngineStatus::Diverged;
  if (revertAccepted_.isVisible() != refused) { revertAccepted_.setVisible(refused); resized(); }
  revertAccepted_.setEnabled(snap_.hasAcceptedDocument);
  message_.setText(refused ? u8(snap_.message) : (notice_.isNotEmpty() ? notice_ : u8(snap_.message)), juce::dontSendNotification);
  const bool alive = snap_.status == EngineStatus::Ready || snap_.status == EngineStatus::Syncing;
  play_.setEnabled(alive);
  play_.setToggleState(snap_.playing, juce::dontSendNotification);
  // One button: it shows Play while stopped and Pause while playing (Diogo, 2026-11-07).
  if (const juce::String icon = snap_.playing ? "pause" : "play"; play_.getProperties()[theme::kIconProperty].toString() != icon) {
    play_.getProperties().set(theme::kIconProperty, icon);
    play_.setButtonText(snap_.playing ? u8("Pause") : u8("Play"));
    play_.setTooltip(snap_.playing ? u8("Pause: stop here and keep the position (Space)") : u8("Play from the current position (Space)"));
    play_.repaint();
  }
  stop_.setEnabled(alive && snap_.playing);
  home_.setEnabled(alive);
  restart_.setVisible(snap_.status == EngineStatus::Stopped || snap_.status == EngineStatus::NoDevice);
  undo_.setEnabled(history_.canUndo());
  redo_.setEnabled(history_.canRedo());
  undo_.setTooltip(history_.canUndo() ? "Undo " + u8(history_.undoLabel()) + " (Ctrl+Z)" : juce::String());
  redo_.setTooltip(history_.canRedo() ? "Redo " + u8(history_.redoLabel()) + " (Ctrl+Y)" : juce::String());
  // The title goes to the taskbar; the window shows no title bar, so the build is in the footer.
  if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    if (window->getName() != windowTitle()) window->setName(windowTitle());
  if (const juce::String build = buildText(); build_.getText() != build) {
    build_.setText(build, juce::dontSendNotification);
    build_.setColour(juce::Label::textColourId, build.startsWith("TEST") ? kAmber : kMuted);
    resized();
  }
  // Export: progress while it runs, one notice at the end.
  const ExportJob& job = snap_.exportJob;
  const bool exporting = job.active();
  exportProgress_ = job.state == "starting" ? -1.0 : job.progress;  // -1: busy animation
  if (exportBar_.isVisible() != exporting) { exportBar_.setVisible(exporting); cancelExport_.setVisible(exporting); resized(); }
  cancelExport_.setEnabled(job.state == "running");
  const std::string jobKey = std::to_string(job.jobId) + job.state;
  if (jobKey != jobSeen_) {
    jobSeen_ = jobKey;
    const juce::String path = u8(job.path);
    if (job.state == "succeeded") notice_ = job.files > 1 ? u8("Exported ") + juce::String(job.files) + u8(" files to ") + juce::File(path).getParentDirectory().getFullPathName() : u8("Exported: ") + path;
    else if (job.state == "cancelled") notice_ = u8("Export cancelled; no file was changed.");
    else if (job.state == "failed") {
      notice_ = u8("The export failed.");
      if (!test_) showError(u8("Could not export"), u8(job.error) + u8("\n\nNo existing file was changed."));
    } else if (exporting) notice_ = job.files > 1 ? u8("Exporting ") + juce::String(job.files) + u8(" files...") : u8("Exporting ") + juce::File(path).getFileName() + "...";
  }
  if (test_) selfTestStep();
  arrangeButton_.setToggleState(!mixerOpen_, juce::dontSendNotification);
  if (rail_.size() == 6) {
    const bool states[] = {arrangeButton_.getToggleState(), mixerOpen_, patternsWindow_ != nullptr && patternsWindow_->isVisible(), libraryShown(),
                           pluginsShown(), inspectorShown()};
    for (size_t i = 0; i < 6; ++i) rail_[i]->setToggleState(states[i], juce::dontSendNotification);
  }
  repaint(timelineArea());
  editor_.playheadMoved();  // only the old and the new line, not the whole piano roll
  // A sound or effect that failed while playing is silent until the next change: say which, once.
  {
    std::string failedNow;
    for (const auto& u : snap_.failedUnits) failedNow += u + ", ";
    if (failedNow != failedSeen_) {
      failedSeen_ = failedNow;
      if (!failedNow.empty()) {
        failedNow.resize(failedNow.size() - 2);
        notice_ = u8("Stopped after a fault, silent until your next change: ") + u8(failedNow);
        logLine("Engine: failed units - " + u8(failedNow));
      }
    }
  }
  if (automationOpen_ && snap_.playing) automation_->repaint();
  const double viewEnd = viewStart() + viewLength();
  if (snap_.playing && viewLength_ > 0 && lastPlayTick_ >= viewStart() && lastPlayTick_ <= viewEnd && snap_.positionTick > viewEnd)
    setView(snap_.positionTick - viewLength() * 0.05, viewLength());  // the next page
  else if (snap_.playing && viewLength_ > 0 && lastPlayTick_ >= viewStart() && lastPlayTick_ <= viewEnd && snap_.positionTick < viewStart())
    setView(snap_.positionTick - viewLength() * 0.05, viewLength());  // round to the start (song end or loop)
  lastPlayTick_ = snap_.playing ? snap_.positionTick : -1;
  if (audioEditor_->isVisible() && playTick_ != audioEditorTick_) { audioEditorTick_ = playTick_; audioEditor_->repaint(); }
  repaint(centre().removeFromBottom(masterRow()));
  // Menus show the undo labels and the save state: rebuild them only when those change.
  const juce::String menuState = u8(history_.undoLabel()) + "|" + u8(history_.redoLabel()) + "|" + juce::String((int)history_.canUndo()) +
                                 juce::String((int)history_.canRedo()) + juce::String((int)unsaved()) +
                                 juce::String((int)snap_.exportJob.active()) + u8(snap_.device) + juce::String((int)snap_.devices.size()) + juce::String((int)missing_.size());
  if (menuState != menuState_) { menuState_ = menuState; menuItemsChanged(); }
  autosaveTick();
}

// ---------------------------------------------------------------- autosave (M6)

void MainComponent::autosaveTick() {
  collectAutosave();
  const double now = juce::Time::getMillisecondCounterHiRes();
  if (test_ || now - lastAutosaveCheck_ < 60000 || autosave_.writing()) return;  // (the self-test leaves none)
  lastAutosaveCheck_ = now;
  if (!unsaved()) { removeOwnAutosave(); return; }
  // Changes made in a plugin's own window are only in the engine: they are asked for first, so the copy has them
  // (audit R02: it held the plugins as last saved). Asked without waiting; the copy is written when they come.
  if (pluginsDirty_ && doc::hasPlugins(history_.current())) {
    if (autosaveAsking_) return;
    autosaveAsking_ = true;
    juce::Component::SafePointer<MainComponent> self(this);
    session_->pluginStatesAsync(5000, [self](json states) {
      if (!self) return;
      self->autosaveAsking_ = false;
      doc::Document document = self->history_.current();
      if (!states.is_object()) logLine("Autosave: the engine did not give the plugins' settings; the copy has the last ones");
      else doc::applyPluginStates(document, states);
      self->startAutosave(std::move(document));
    });
    return;
  }
  startAutosave(history_.current());
}

void MainComponent::startAutosave(doc::Document document) {
  if (autosave_.writing()) return;
  std::string text = doc::toJson(document).dump();
  if (!autosave_.due(text)) return;  // this song is already on disk (a failed write stays due)
  autosave_.started(std::move(text));
  // Written on a thread of its own (a song with plugin states can be a few MB); the copy is taken here.
  autosaveJob_ = std::make_shared<AutosaveJob>();
  juce::Thread::launch([job = autosaveJob_, folder = autosaveFolder(), session = autosaveSession_, document = std::move(document),
                        song = file_ != juce::File() ? file_.getFullPathName().toStdString() : std::string()] {
    try {
      doc::writeAutosave(pathOf(folder), session, currentProcessId(), document, song, juce::Time::currentTimeMillis());
      job->state.store(1);
    } catch (const std::exception& e) {  // a full disk or a locked folder: tried again in a minute
      job->error = e.what();
      job->state.store(2);  // (after the error: the window reads it once it sees 2)
    }
  });
}

// The result of the last autosave write, once it is there.
void MainComponent::collectAutosave() {
  if (!autosaveJob_) return;
  const int state = autosaveJob_->state.load();
  if (state == 0) return;
  const bool ok = state == 1;
  const std::string error = ok ? std::string() : autosaveJob_->error;
  autosaveJob_.reset();
  const int failuresBefore = autosave_.failures();
  if (autosave_.finished(ok, error)) {
    if (ok) {
      logLine("Autosave works again (after " + juce::String(failuresBefore) + " failed attempt(s))");
      notice_ = u8("Autosave works again.");
    } else {
      logLine("Autosave failed: " + u8(error));
      notice_ = u8("Autosave failed (") + u8(error) + u8("). It is tried again every minute; save the song to be safe.");
    }
  } else if (!ok) {
    logLine("Autosave failed again: " + u8(error));
  }
}

void MainComponent::removeOwnAutosave() {
  collectAutosave();
  if (!autosave_.mayHaveFile() || autosave_.writing()) return;  // nothing written yet, or still being written
  doc::removeAutosave(doc::autosavePath(pathOf(autosaveFolder()), autosaveSession_));
  autosave_.removed();
}

// A new song is on screen: the two things that belonged to the one before go with it.
// pluginsDirty_ said "a plugin's own window changed the sound since the last save" - of the OLD song: opened or
// made new, the copy on disk cannot be behind, so the song would stay shown modified and be autosaved without
// an edit (audit 2026-11-17, item 8). And the autosave of this session is a copy of the old song: "Don't save"
// used to leave it there until the next check, up to a minute, and a crash in between offers the discarded
// song back (item 7). lastAutosaveCheck_ = 0 looks again on the next tick, which also catches a copy that was
// still being written when the song changed.
void MainComponent::documentLoaded() {
  pluginsDirty_ = false;
  pluginEditsSeen_ = session_->snapshot().pluginEdits;
  removeOwnAutosave();
  lastAutosaveCheck_ = 0;
}

// At start: a song autosaved by a session that is not running any more (Core or the PC stopped without
// closing) is offered back, the newest first. Recover opens it with unsaved changes; Discard deletes it;
// Later keeps it for the next start.
void MainComponent::offerRecovery() {
  if (test_) return;
  std::vector<doc::AutosaveInfo> left;
  for (auto& a : doc::findAutosaves(pathOf(autosaveFolder()), autosaveSession_))
    if (!processRunning(a.process)) left.push_back(std::move(a));
  if (left.empty()) return;
  const doc::AutosaveInfo a = left.front();
  const juce::String name = a.name.empty() ? (a.songPath.empty() ? juce::String("Untitled") : juce::File(u8(a.songPath)).getFileNameWithoutExtension())
                                           : u8(a.name);
  const juce::String when = juce::Time(a.savedAtMs).formatted("%d/%m %H:%M");
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
          .withTitle("Recover unsaved work")
          .withMessage("Core closed without saving \"" + name + "\". An autosave from " + when + " is there.\n\n"
                       "Recover opens it (then save it to keep it). Discard deletes the autosave.")
          .withButton("Recover", 1).withButton("Discard", 2).withButton("Later", 0),
      [this, a](int result) {
        if (result == 2) { doc::removeAutosave(a.file); offerRecovery(); return; }
        if (result != 1) return;
        try {
          doc::Document recovered = doc::readAutosave(a.file);
          confirmUnsaved([this, a, recovered = std::move(recovered)] { recoverAutosave(a, recovered); });
        } catch (const std::exception& e) {
          showError("Could not recover", u8(e.what()) + "\n\nThe autosave was left where it is: " + juce::String(a.file.wstring().c_str()));
        }
      });
}

void MainComponent::recoverAutosave(const doc::AutosaveInfo& a, const doc::Document& recoveredDoc) {
  doc::Document recovered = recoveredDoc;
  // Like File > Open: samples whose library file moved are found again by content.
  doc::Document moved = recovered;
  const int relinked = hasSampleIndex_ ? doc::relinkMovedSamples(moved, sampleIndex_) : 0;
  history_.reset(recovered);
  if (relinked) history_.amend(moved);
  selectedTrack_.clear();
  selectedClip_.clear();
  const juce::File song = a.songPath.empty() ? juce::File() : juce::File(u8(a.songPath));
  file_ = song;
  imported_ = song == juce::File();  // never saved: Save asks for a name
  loadProblem_.clear();
  viewStart_ = viewLength_ = 0;
  recovered_ = true;
  session_->newSong(); session_->releasePlugins();
  missing_.clear();
  session_->setAssetRoot(projectDir(), false);
  documentLoaded();
  documentChanged(true);
  checkMissingAssets();
  doc::removeAutosave(a.file);  // this session autosaves it again from now on
  logLine("Recovered an autosave of " + juce::Time(a.savedAtMs).toString(true, true) + (song == juce::File() ? juce::String() : " for " + song.getFullPathName()));
  const juce::String name = a.name.empty() ? (song == juce::File() ? juce::String("Untitled") : song.getFileNameWithoutExtension()) : u8(a.name);
  notice_ = "Recovered \"" + name + "\" from the autosave of " + juce::Time(a.savedAtMs).formatted("%d/%m %H:%M") + ": save it to keep it.";
}

// ---------------------------------------------------------------- self-test (null device)

void MainComponent::runSelfTest(juce::File picture, std::function<void(bool, juce::String)> done) {
  test_ = std::make_unique<SelfTest>();
  test_->picture = std::move(picture);
  test_->done = std::move(done);
  // Core's sound comes from plugins (2026-11-16): the self-test plays its own test synth (app/selftest-plugins).
#ifdef CORE_TEST_SYNTH_VST3
  juce::String problem;
  const nlohmann::json found = PluginCatalog::scanFile(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("core-engine.exe"),
                                                       juce::File(CORE_TEST_SYNTH_VST3), &problem);
  if (found.is_array() && !found.empty()) test_->synth = found[0];
  else logLine("Self-test: the test synth was not read: " + problem);
#endif
  selfTestSynth();
  test_->started = test_->stepStarted = juce::Time::getMillisecondCounterHiRes();
}

// Every track that plays notes with no plugin yet gets the self-test's synth (no undo step: the song as it was opened).
void MainComponent::selfTestSynth() {
  if (!test_ || test_->synth.is_null()) return;
  doc::Document d = history_.current();
  bool changed = false;
  for (const auto& t : history_.current().tracks)
    if (doc::playsPatterns(t) && t.instrument.kind != "plugin") { doc::replaceInstrumentWithPlugin(d, t.id, test_->synth); changed = true; }
  if (!changed) return;
  const bool wasModified = history_.modified();
  history_.amend(d);
  if (!wasModified) history_.markSaved();  // the song as it was opened, only played by the test synth
  session_->setDocument(engineDocument());
}

// The path a user takes in M1b: play, drag the BPM, undo, seek, loop, mute and a volume drag, save,
// reopen. Each step waits until the engine plays the result. Fails after 30 s.
void MainComponent::selfTestStep() {
  SelfTest& t = *test_;
  const double now = juce::Time::getMillisecondCounterHiRes(), inStep = now - t.stepStarted;
  t.loudest = std::max({t.loudest, snap_.masterPeak[0], snap_.masterPeak[1]});
  if (!snap_.trackPeaks.empty()) t.trackMeters = true;
  const bool applied = snap_.appliedRevision == snap_.documentRevision;
  auto next = [&](const juce::String& done) { t.log << done << "; "; ++t.step; t.stepStarted = now; };
  auto finish = [&](bool ok, const juce::String& why) {
    auto done = std::move(t.done);
    const juce::String report = juce::String(ok ? "ok" : "failed") + ": " + why + " | build=" + CORE_BUILD_ID + " engine=" + u8(snap_.engineBuild) + " | steps: " + t.log + "| status=" + statusText(snap_.status) +
        " session=" + u8(snap_.sessionId) + " document=" + juce::String(snap_.documentRevision) +
        " accepted=" + juce::String(snap_.acceptedRevision) + " applied=" + juce::String(snap_.appliedRevision) +
        " tick=" + juce::String(snap_.positionTick, 1) + " loudest=" + juce::String(t.loudest, 3) + " message=" + u8(snap_.message);
    test_.reset();
    done(ok, report);
  };
  if (now - t.started > 45000) return finish(false, "timeout at step " + juce::String(t.step));
  if (!t.keymapChecked) {  // once: the keymap: text form, matching, a key moved to another action, kept in the settings
    Keymap& km = Keymap::get();
    const Key ctrlShiftS = Key::parse("Ctrl+Shift+S");
    bool ok = ctrlShiftS.text() == "Ctrl+Shift+S" && Key::parse("ctrl + ,").text() == "Ctrl+," && Key::parse("Alt").valid() && !Key::parse("Ctrl+A+B").valid() &&
              km.matches("file.save", juce::KeyPress('S', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier), 0)) &&
              !km.matches("file.save", juce::KeyPress('S', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier), 0)) &&
              km.matches("edit.redo", juce::KeyPress('Z', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier), 0)) &&
              km.actionFor(ctrlShiftS) == "file.saveAs";
    km.set("transport.loop", {Key::parse("Ctrl+K")});
    juce::PropertySet props;
    km.save(props);
    km.resetAll();
    ok = ok && km.matches("transport.loop", juce::KeyPress('L', juce::ModifierKeys(), 0));
    km.load(props);
    ok = ok && km.matches("transport.loop", juce::KeyPress('K', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier), 0)) && !km.matches("transport.loop", juce::KeyPress('L', juce::ModifierKeys(), 0)) &&
         !km.isDefault("transport.loop") && props.getValue("keymap").contains("transport.loop") && !props.getValue("keymap").contains("file.save");
    km.resetAll();
    t.keymapChecked = true;
    if (!ok) return finish(false, "keymap");
  }
  const bool crashSteps = t.step == 10 || t.step == 11;
  if ((snap_.status == EngineStatus::Stopped && !crashSteps) || snap_.status == EngineStatus::Diverged) return finish(false, "engine problem");
  switch (t.step) {
    case 0:
      if (snap_.status == EngineStatus::Ready) { session_->play(); next("play"); }
      break;
    case 1:  // sound in the meters, then a BPM drag: many values, one undo step
      if (snap_.playing && t.loudest > 0.05f && t.trackMeters && inStep > 800) {
        beginGesture("Tempo");
        for (double bpm = 191; bpm <= 200; bpm += 1) edit([bpm](doc::Document& d) { d.bpm = bpm; }, "Tempo");
        endGesture();
        if (history_.undoLabel() != "Tempo" || history_.current().bpm != 200) return finish(false, "BPM gesture");
        next("bpm 200");
      }
      break;
    case 2:  // the drag is heard, then undo: back to the demo tempo, and not modified
      if (applied && inStep > 300) {
        undo();
        if (history_.current().bpm != 190 || history_.modified() || history_.canUndo()) return finish(false, "undo");
        next("undo");
      }
      break;
    case 3:  // seek to bar 2
      if (applied && history_.current().bpm == 190) { session_->seek(kBar); next("applied 190"); }
      break;
    case 4:
      if (inStep > 250 && snap_.positionTick >= kBar && snap_.positionTick < kBar + 2 * kBeat) {
        edit([](doc::Document& d) { d.loop = {true, 0, kBeat, d.loop.extra}; }, "Loop");
        next("seek bar 2");
      } else if (inStep > 2000) {
        return finish(false, "seek");
      }
      break;
    case 5:  // the loop keeps the position in the first beat
      if (inStep > 400) t.maxTickInLoop = std::max(t.maxTickInLoop, snap_.positionTick);
      if (inStep > 1600) {
        if (t.maxTickInLoop >= kBeat || t.maxTickInLoop <= 0) return finish(false, "loop, max tick " + juce::String(t.maxTickInLoop));
        edit([](doc::Document& d) { d.tracks[1].mix.mute = true; }, "Mute");
        beginGesture("Volume");
        for (double v = -1; v >= -12; v -= 1) edit([v](doc::Document& d) { d.tracks[0].mix.gainDb = v; }, "Volume");
        endGesture();
        next("loop");
      }
      break;
    case 6:  // mute and volume heard: save, reopen, same content, not modified
      if (applied && inStep > 300) {
        const juce::File song = t.picture.withFileExtension(doc::kFileExtension);
        song.deleteFile();
        if (writeTo(song) != SaveResult::Saved) return finish(false, "save");
        const doc::OpenResult reopened = doc::openFile(pathOf(song));
        if (!doc::sameContent(reopened.document, history_.current()) || history_.modified() || reopened.imported ||
            !reopened.document.tracks[1].mix.mute || reopened.document.tracks[0].mix.gainDb != -12 || !reopened.document.loop.enabled)
          return finish(false, "reopen");
        next("saved and reopened");
      }
      break;
    case 7:  // export the saved song (24-bit), as File > Export does after the file dialog
      session_->exportWav(t.picture.withFileExtension(".wav").getFullPathName().toStdString(), 24);
      next("export started");
      break;
    case 8:
      if (snap_.exportJob.state == "succeeded") {
        const juce::File wav = t.picture.withFileExtension(".wav");
        if (!wav.existsAsFile() || wav.getSize() < 44 + 48000 * 6 || snap_.exportJob.revision != snap_.acceptedRevision)
          return finish(false, "export file");
        if (snap_.devices.empty()) return finish(false, "no devices listed");
        selectDevice(snap_.devices.front());  // the Audio menu: reopen the output by name
        next("exported " + juce::String(wav.getSize() / 1024) + " KB");
      } else if (snap_.exportJob.state == "failed" || inStep > 10000) {
        return finish(false, "export: " + u8(snap_.exportJob.error));
      }
      break;
    case 9:
      if (!snap_.device.empty() && snap_.status == EngineStatus::Ready && inStep > 500) next("device " + u8(snap_.device));
      else if (inStep > 5000) return finish(false, "device");
      break;
    case 10:  // the engine crashes: the document stays, the window says so
      if (inStep > 300) { t.tickBeforeCrash = snap_.positionTick; session_->killEngineForTest(); next("engine killed"); }
      break;
    case 11:
      if (snap_.status == EngineStatus::Stopped) {
        if (history_.modified() || !history_.current().tracks[1].mix.mute) return finish(false, "document lost after crash");
        session_->restart();
        next("stopped seen");
      } else if (inStep > 3000) {
        return finish(false, "crash not reported");
      }
      break;
    case 12:  // new session: same document and loop, stopped, at the last position
      if (snap_.status == EngineStatus::Ready && applied && inStep > 1200) {
        if (snap_.playing || std::abs(snap_.positionTick - t.tickBeforeCrash) > kBeat) return finish(false, "restart state, tick " + juce::String(snap_.positionTick));
        next("restarted");
      }
      break;
    case 13:  // M2: a new project from the template, then track management
      newProject(true);
      next("new from template");
      break;
    case 14:
      if (snap_.status == EngineStatus::Ready && applied && history_.current().tracks.size() == 3 && !history_.modified()) {
        selfTestSynth();  // the template's tracks have no plugin yet: the test synth plays them
        edit([](doc::Document& d) { doc::addTrack(d, "bass"); }, "Add");
        edit([](doc::Document& d) { doc::duplicateTrack(d, "kick"); }, "Duplicate");
        edit([](doc::Document& d) { doc::renameTrack(d, "kick-2", "Kick layer"); }, "Rename");
        edit([](doc::Document& d) { doc::removeTrack(d, "hats"); }, "Remove");
        const auto& tracks = history_.current().tracks;
        if (tracks.size() != 4 || rows_.size() != 4 || tracks[1].id != "kick-2" || tracks[1].name != "Kick layer" || tracks[3].id != "bass-2")
          return finish(false, "track edits");
        session_->play();
        next("tracks edited");
      } else if (inStep > 5000) {
        return finish(false, "new project");
      }
      break;
    case 15:  // the duplicated kick sounds under its own id; undo brings the removed track back
      if (applied && snap_.playing && snap_.trackPeaks.count("kick-2") && snap_.trackPeaks.at("kick-2").first > 0.01f && !snap_.trackPeaks.count("hats")) {
        undo();
        if (!history_.current().findTrack("hats") || rows_.size() != 5) return finish(false, "undo remove");
        session_->stop();
        next("kick-2 heard, undo remove");
      } else if (inStep > 5000) {
        return finish(false, "duplicated track not heard");
      }
      break;
    case 16:  // the pattern editors: a bass note, a longer pattern, duplicate; a kick step
      if (applied && inStep > 300) {
        selectTrack("bass");
        const size_t before = history_.current().findTrack("bass")->patterns[0].notes.size();
        editor_.clickCell(0, 40);
        if (history_.current().findTrack("bass")->patterns[0].notes.size() != before + 1 || editor_.selection().size() != 1) return finish(false, "bass note");
        edit([](doc::Document& d) { doc::setPatternLength(doc::trackById(d, "bass").patterns[0], 2 * 4 * doc::kPpq); }, "Length");
        editor_.selectAll();
        editor_.duplicateSelection();
        if (history_.current().findTrack("bass")->patterns[0].notes.size() != 2 * (before + 1)) return finish(false, "duplicate notes");
        selectTrack("kick");
        editor_.clickCell(2 * 240, 36);
        if (history_.current().findTrack("kick")->patterns[0].notes.size() != 5) return finish(false, "kick step");
        {  // the mouse: drag that step one 1/16 later, then a click in place turns it off
          auto mouse = [&](juce::Point<int> at, juce::Point<int> down, juce::ModifierKeys mods) {
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods, 0, 0, 0, 0, 0, &editor_, &editor_,
                                    juce::Time::getCurrentTime(), down.toFloat(), juce::Time::getCurrentTime(), 1, at != down);
          };
          const auto from = editor_.cellGrabPoint(2 * 240, 36), to = editor_.cellGrabPoint(3 * 240, 36);
          const auto left = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
          editor_.mouseDown(mouse(from, from, left));
          editor_.mouseDrag(mouse(to, from, left));
          editor_.mouseUp(mouse(to, from, {}));
          const auto& notes = history_.current().findTrack("kick")->patterns[0].notes;
          const bool moved = std::any_of(notes.begin(), notes.end(), [](const doc::Note& n) { return n.tick == 3 * 240; }) &&
                             std::none_of(notes.begin(), notes.end(), [](const doc::Note& n) { return n.tick == 2 * 240; });
          if (!moved || notes.size() != 5 || history_.undoLabel() != "Move notes") return finish(false, "drag step");
          editor_.mouseDown(mouse(to, to, left));
          editor_.mouseUp(mouse(to, to, {}));
          if (history_.current().findTrack("kick")->patterns[0].notes.size() != 4) return finish(false, "click step off");
          undo();
          undo();  // back to the step at 2 * 240, as before
        }
        session_->play();
        next("notes edited");
      }
      break;
    case 17:
      if (applied && snap_.playing && inStep > 1500) { session_->stop(); next("heard"); }
      break;
    case 18: {  // M2 arrangement: a second bass pattern from bar 9, a shared clip made independent, sections swapped
      selectClip("bass", "c1");
      edit([](doc::Document& d) { doc::resizeClip(doc::trackById(d, "bass"), "c1", 8 * kBar); }, "Clip length");
      std::string p2;
      edit([&](doc::Document& d) { p2 = doc::addPattern(doc::trackById(d, "bass"), kBar); }, u8("New pattern"));
      editor_.setTrack("bass", p2);
      addClipAt("bass", 8 * kBar);  // a clip of the pattern shown in the editor
      editor_.clickCell(0, 45);
      editor_.clickCell(kBeat, 45);
      const doc::Track& bass = *history_.current().findTrack("bass");
      if (bass.clips.size() != 2 || bass.clips[1].patternId != p2 || bass.findPattern(p2)->notes.size() != 2) return finish(false, "clips");
      addClipAt("bass", 12 * kBar);  // shares p2
      const std::string c3 = selectedClip_;
      if (history_.current().findTrack("bass")->clipsUsing(p2) != 2) return finish(false, "shared clip");
      edit([&](doc::Document& d) { doc::makeClipUnique(doc::trackById(d, "bass"), c3); }, u8("Independent copy"));
      if (history_.current().findTrack("bass")->clipsUsing(p2) != 1) return finish(false, "independent clip");
      edit([](doc::Document& d) { doc::addSection(d, "Intro", 0, 8 * kBar); }, u8("New section"));
      edit([](doc::Document& d) { doc::addSection(d, "Drop", 8 * kBar, 8 * kBar); }, u8("New section"));
      edit([](doc::Document& d) { doc::swapSection(d, d.sections[0].id, true); }, u8("Swap sections"));
      const doc::Document& d = history_.current();
      if (d.sections[0].name != "Drop" || d.findTrack("bass")->clips.front().patternId != p2) return finish(false, "sections swap");
      session_->home();
      session_->play();
      next("arranged");
      break;
    }
    case 19:
      if (applied && snap_.playing && inStep > 1500) { session_->stop(); next("arrangement heard"); }
      break;
    case 20:  // live notes, stopped: the Z key on the bass, then a MIDI note-on / note-off
      if (!snap_.playing && inStep > 800) {
        selectTrack("bass");
        if (!pressPianoKey('Z')) return finish(false, "piano key");
        next("key down");
      }
      break;
    case 21:
      if (snap_.trackPeaks.count("bass") && snap_.trackPeaks.at("bass").first > 0.01f) {
        releasePianoKey('Z');
        if (!heldKeys_.empty()) return finish(false, "key release");
        next("key heard");
      } else if (inStep > 3000) {
        return finish(false, "piano key not heard");
      }
      break;
    case 22:
      // First let the piano tail finish so it cannot pass the MIDI check. Then inspect
      // fresh states immediately: waiting 300 ms missed this short envelope (audit part 7).
      if (t.midiStarted == 0 && snap_.positionAtMs > t.stepStarted && snap_.trackPeaks.count("bass") &&
          snap_.trackPeaks.at("bass").first <= 0.001f) {
        handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOn(1, 45, (juce::uint8)100));
        t.midiStarted = now;
      } else if (t.midiStarted > 0 && snap_.positionAtMs > t.midiStarted && snap_.trackPeaks.count("bass") &&
                 snap_.trackPeaks.at("bass").first > 0.01f) {
        handleIncomingMidiMessage(nullptr, juce::MidiMessage::noteOff(1, 45));
        next("midi heard");
      } else if (inStep > 3000) {
        return finish(false, "midi note not heard");
      }
      break;
    case 23:  // released: the bass falls silent (release 25 ms); wait for the tail to finish.
      if (inStep > 2300) {
        const float level = snap_.trackPeaks.count("bass") ? snap_.trackPeaks.at("bass").first : 0.0f;
        if (level > 0.001f) return finish(false, "note still sounding after release: " + juce::String(level));
        next("released");
      }
      break;
    case 24: {  // M3: save the song, import a WAV (2 s of a 220 Hz tone) at bar 1, hear it on its own track
      const juce::File song = t.picture.getSiblingFile(t.picture.getFileNameWithoutExtension() + "-audio").getChildFile("song.core");
      song.getParentDirectory().deleteRecursively();
      song.getParentDirectory().createDirectory();
      if (writeTo(song) != SaveResult::Saved) return finish(false, "save before import");
      const juce::File wav = song.getSiblingFile("tone.wav");
      {
        juce::AudioBuffer<float> tone(2, 96000);
        for (int i = 0; i < tone.getNumSamples(); ++i)
          for (int c = 0; c < 2; ++c) tone.setSample(c, i, 0.5f * (float)std::sin(2 * juce::MathConstants<double>::pi * 220.0 * i / 48000.0));
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(wav);
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(out, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24)));
        if (!writer || !writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples())) return finish(false, "write test wav");
      }
      session_->home();
      juce::String problem;
      if (!importAudioFile(wav, &problem)) return finish(false, "import: " + problem);
      const doc::Document& d = history_.current();
      const doc::Track& track = d.tracks.back();
      if (track.instrument.kind != "audio" || track.audioClips.size() != 1 || d.assets.size() != 1 ||
          !song.getSiblingFile(u8(d.assets[0].path)).existsAsFile() || !missing_.empty())
        return finish(false, "imported document");
      t.audioTrack = track.id;
      const std::string clipId = track.audioClips[0].id;
      std::vector<std::string> others;
      for (const auto& tr : d.tracks) if (tr.id != t.audioTrack) others.push_back(tr.id);
      // Placed at the playhead's bar (home may not have reached the engine yet): bring it to the start.
      edit([&](doc::Document& x) {
        doc::moveAudioClip(x, t.audioTrack, clipId, 0);
        for (const auto& id : others) doc::trackById(x, id).mix.mute = true;
      }, "Mute");
      session_->play();
      next("imported");
      break;
    }
    case 25:
      if (applied && snap_.playing && snap_.trackPeaks.count(t.audioTrack) && snap_.trackPeaks.at(t.audioTrack).first > 0.05f) {
        session_->stop();
        // Split at beat 4, trim the second part's end, undo twice: one clip again.
        const std::string trackId = t.audioTrack, first = history_.current().findTrack(trackId)->audioClips[0].id;
        std::string second;
        edit([&](doc::Document& x) { second = doc::splitAudioClip(x, trackId, first, 3 * kBeat); }, "Split");
        edit([&](doc::Document& x) { doc::trimAudioClipEnd(x, trackId, second, 0.25); }, "Trim end");
        const auto& clips = history_.current().findTrack(trackId)->audioClips;
        if (clips.size() != 2 || std::abs(clips[1].durationSeconds - 0.25) > 1e-9) return finish(false, "split and trim");
        undo();
        undo();
        if (history_.current().findTrack(trackId)->audioClips.size() != 1) return finish(false, "undo split");
        next("audio heard, split, trimmed, undone");
      } else if (inStep > 5000) {
        return finish(false, "audio clip not heard: " + u8(doc::toProjection(history_.current(), projectDir())["tracks"].back().dump()));
      }
      break;
    case 26: {  // the audio editor: select one second in the middle and remove it (one undo step), then keep a part
      const std::string trackId = t.audioTrack, clipId = history_.current().findTrack(trackId)->audioClips[0].id;
      selectClip(trackId, clipId);
      if (!audioEditor_->isVisible() || editor_.isVisible() || audioEditor_->clipId() != clipId) return finish(false, "audio editor not shown");
      audioEditor_->select(0.5, 1.0);
      audioEditor_->removeSelection();
      if (history_.current().findTrack(trackId)->audioClips.size() != 2) return finish(false, "remove selection");
      undo();
      audioEditor_->select(0.25, 1.75);
      audioEditor_->keepSelection();
      const auto& clips = history_.current().findTrack(trackId)->audioClips;
      if (clips.size() != 1 || std::abs(clips[0].durationSeconds - 1.5) > 0.002 || std::abs(clips[0].offsetSeconds - 0.25) > 0.002)
        return finish(false, "keep selection");
      // A part of it to a new clip (a new track, same place in the song), then loop that part.
      audioEditor_->select(0.5, 1.0);
      const size_t tracksBefore = history_.current().tracks.size();
      audioEditor_->selectionToNewClip();
      const doc::Track& part = history_.current().tracks.back();
      if (history_.current().tracks.size() != tracksBefore + 1 || part.audioClips.size() != 1 || std::abs(part.audioClips[0].durationSeconds - 0.5) > 0.01 ||
          selectedTrack_ != part.id || !audioEditor_->isVisible())
        return finish(false, "selection to new clip");
      audioEditor_->select(part.audioClips[0].offsetSeconds, part.audioClips[0].offsetSeconds + 0.25);
      audioEditor_->loopSelection();
      if (!history_.current().loop.enabled || history_.current().loop.endTick <= history_.current().loop.startTick) return finish(false, "loop selection");
      // The timeline zoom: four bars from bar 2 fill the lanes.
      setView(kBar, 4 * kBar);
      if (xOfTick(kBar) != laneSpan().getX() || std::abs(xOfTick(5 * kBar) - laneSpan().getRight()) > 1) return finish(false, "timeline zoom");
      // Normalize (a 0.5 tone: +5.7 dB), slice on beats, reverse (a new file; the clip uses it).
      const std::string partId = history_.current().tracks.back().id;  // (part: a reference to an older state)
      audioEditor_->normalize();
      const auto partClip = [&] { return history_.current().findTrack(partId)->audioClips; };
      if (std::abs(partClip()[0].gainDb - (-0.3 - 20 * std::log10(0.5))) > 0.05) return finish(false, "normalize " + juce::String(partClip()[0].gainDb));
      if (audioEditor_->slice(kBeat) != 2 || partClip().size() != 2) return finish(false, "slice");
      const std::string before = partClip()[0].assetId;
      audioEditor_->reverse();
      if (partClip()[0].assetId == before || history_.current().assets.size() != 2) return finish(false, "reverse");
      {  // Tempo and pitch on a 220 Hz tone: the frequency (zero crossings) and the length
        dsp::Channels tone(2, std::vector<float>(48000));
        for (int i = 0; i < 48000; ++i) tone[0][(size_t)i] = tone[1][(size_t)i] = 0.5f * (float)std::sin(2 * juce::MathConstants<double>::pi * 220.0 * i / 48000.0);
        auto hz = [](const std::vector<float>& v) {
          int crossings = 0;
          for (size_t i = 4800; i + 4800 < v.size(); ++i) crossings += (v[i - 1] < 0) != (v[i] < 0);
          return crossings / 2.0 / ((v.size() - 9600) / 48000.0);
        };
        const dsp::Channels slow = dsp::stretch(tone, 2.0), up = dsp::shiftPitch(tone, 12);
        if (slow[0].size() != 96000 || std::abs(hz(slow[0]) - 220) > 220 * 0.03) return finish(false, "stretch: " + juce::String(hz(slow[0])) + " Hz");
        if (up[0].size() != 48000 || std::abs(hz(up[0]) - 440) > 440 * 0.03) return finish(false, "pitch: " + juce::String(hz(up[0])) + " Hz");
        // No holes or bumps: the level of every 20 ms stays near the tone's 0.5.
        auto steady = [](const std::vector<float>& v) {
          for (size_t s = 0; s + 960 <= v.size(); s += 960) {
            float peak = 0;
            for (size_t i = s; i < s + 960; ++i) peak = std::max(peak, std::abs(v[i]));
            if (peak < 0.4f || peak > 0.6f) return false;
          }
          return true;
        };
        if (!steady(slow[0]) || !steady(up[0])) return finish(false, "stretch level");
      }
      t.partTrack = partId;
      t.duration = partClip()[0].durationSeconds;
      audioEditor_->fitToTempo(history_.current().bpm / 2);  // recorded at half the tempo: half as long (in the background)
      if (!audioEditor_->busy()) return finish(false, "fit to tempo did not start");
      next("audio editor");
      break;
    }
    case 27:
      if (!audioEditor_->busy()) {
        const auto& clips = history_.current().findTrack(t.partTrack)->audioClips;
        if (std::abs(clips[0].durationSeconds - t.duration / 2) > 0.002) return finish(false, "fit to tempo");
        audioEditor_->shiftPitch(7);
        next("tempo");
      } else if (inStep > 10000) {
        return finish(false, "fit to tempo: too slow");
      }
      break;
    case 28:
      if (!audioEditor_->busy()) {
        const auto& clips = history_.current().findTrack(t.partTrack)->audioClips;
        if (std::abs(clips[0].durationSeconds - t.duration / 2) > 0.002 || history_.current().assets.size() != 4) return finish(false, "shift pitch");
        next("pitch");
      } else if (inStep > 10000) {
        return finish(false, "shift pitch: too slow");
      }
      break;
    case 29:  // Core's own effects are gone (2026-11-16: plugins only); the plugin chain is checked by the render test
      next("effects: plugins only");
      break;
    case 30:
      next("no built-in limiter");
      break;
    case 31: {  // automation: the bass volume falls over 4 bars
      openAutomation("bass", "gainDb");
      if (!automation_->isVisible() || editor_.isVisible()) return finish(false, "automation editor not shown");
      automation_->addPoint(0, 0);
      automation_->addPoint(4 * kBar, -40);
      const doc::Track& bass = *history_.current().findTrack("bass");
      if (bass.automation.empty() || bass.automation[0].points.size() != 2)
        return finish(false, "automation in the document");
      session_->home();
      session_->play();
      next("automation");
      break;
    }
    case 32:
      if (applied && snap_.playing && inStep > 1200) {
        keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));  // Esc closes the panel: the pattern editor again
        if (automation_->isVisible() || !editor_.isVisible()) return finish(false, "Esc did not close the automation panel");
        next("automation plays");
      }
      else if (inStep > 5000) return finish(false, "automation not applied");
      break;
    case 33:  // (33-34 were the VST3 test plugins; Core has no VST since 2026-11-08)
      t.step = 35;
      t.stepStarted = now;
      break;
    case 35:  // the picture shows the waveforms once they are computed
      if (inStep > 800) {
        session_->stop();
        // M6: an autosave left by a session that is not running (a crash) comes back as the song, unsaved.
        const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                      .getChildFile("core-selftest-autosave-" + juce::String(currentProcessId()));  // audit 28: two concurrent self-tests no longer wipe each other
        folder.deleteRecursively();
        doc::Document copy = history_.current();
        copy.name = "Recovered by the self-test";
        // Recorded for the song as saved (the M3 audio stays in its folder, and the save below brings it along).
        doc::writeAutosave(pathOf(folder), "selftest", 0, copy, file_ != juce::File() ? file_.getFullPathName().toStdString() : std::string(),
                           juce::Time::currentTimeMillis());
        const auto found = doc::findAutosaves(pathOf(folder), autosaveSession_);
        if (found.size() != 1 || processRunning(found[0].process) || !processRunning(currentProcessId()))
          return finish(false, "autosave: not found as left by a crash");
        recoverAutosave(found[0], doc::readAutosave(found[0].file));
        const bool gone = !folder.getChildFile("selftest.core-autosave").exists();
        folder.deleteRecursively();
        if (!unsaved() || history_.current().name != copy.name || !gone) return finish(false, "autosave: not recovered");
        next("waveform, autosave recovered");
      }
      break;
    case 36: {  // the picture of the window; then the Mixer, with a track that has effects chosen
      if (!savePicture(t.picture)) return finish(false, "cannot write " + t.picture.getFullPathName());
      std::string chosen;
      for (const auto& track : history_.current().tracks) {
        try {
          if (!doc::effectChain(history_.current(), track.id).empty()) { chosen = track.id; break; }
        } catch (const std::exception&) {}
      }
      if (!chosen.empty()) selectTrack(chosen);
      setMixerOpen(true);
      next("picture");
      break;
    }
    case 37:  // the Mixer's strips are built after a message loop pass; the LFO is played
      if (inStep > 500 && applied) {
        const juce::File mixer = t.picture.getSiblingFile(t.picture.getFileNameWithoutExtension() + "-mixer.png");
        const bool shown = mixer_->isVisible() && mixerButton_.getToggleState();
        if (!savePicture(mixer)) return finish(false, "cannot write " + mixer.getFullPathName());
        setMixerOpen(false);
        if (!shown) return finish(false, "mixer: not shown");
        // The song saved into a new folder takes its audio with it.
        const juce::File song = t.picture.getSiblingFile(t.picture.getFileNameWithoutExtension() + "-saved").getChildFile("song.core");
        song.getParentDirectory().deleteRecursively();
        song.getParentDirectory().createDirectory();
        if (writeTo(song) != SaveResult::Saved) return finish(false, "save to a new folder");
        if (!missing_.empty()) return finish(false, "the song's audio did not come with it to the new folder");
        session_->play();
        next("mixer");
      }
      break;
    case 38:
      if (inStep > 300 && applied) {
        session_->stop();
        // M6: this session's logs are there, with the engine's life in them.
        if (!engineLogFile().existsAsFile() || !appLogFile().loadFileAsString().contains("Engine: Ready"))
          return finish(false, "diagnostics: no log in " + logsFolder().getFullPathName());
        return finish(true, "M1c + M2 path, live notes, audio clips, mixer, save with audio");
      }
      break;
  }
}

float MainComponent::uiScaleFor(int windowWidth, float chosen) {
  if (chosen > 0) return std::min(chosen, std::max(1.0f, (float)windowWidth / 1200.0f));
  // Auto: as large as the layout allows (it is made for at least 1560 px), in steps of 25 %, at most 200 %.
  const float fits = (float)windowWidth / 1560.0f;
  return juce::jlimit(1.0f, 2.0f, std::floor(fits * 4.0f) / 4.0f);
}

double MainComponent::chosenUiScale() const {
  return const_cast<juce::ApplicationProperties&>(settings_).getUserSettings()->getDoubleValue("uiScale", 0.0);
}

bool MainComponent::savePicture(const juce::File& file) {
  // At the size it is on the screen (the window draws it scaled), not at its own logical size.
  const juce::Image image = createComponentSnapshot(getLocalBounds(), true, std::sqrt(std::abs(getTransform().getDeterminant())));
  file.deleteFile();
  juce::FileOutputStream out(file);
  return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out);
}

} // namespace core::app
