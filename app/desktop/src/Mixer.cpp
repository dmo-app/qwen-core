#include "Mixer.h"
#include "Language.h"
#include <algorithm>
#include <cmath>
#include "Theme.h"
#include "TrackColour.h"
#include "edit.hpp"

namespace core::app {
using namespace theme;

namespace {
juce::String u8(const std::string& t) { return juce::String::fromUTF8(t.c_str()); }
juce::String u8(const char* t) { return juce::String::fromUTF8(t); }
// Compact strips (Diogo, 2026-11-07: the mixer was so tall the tracks had no room): no captions (the lists say
// what they are in their tooltips), the output and pan on one row, mute/solo beside the fader's value. A short
// strip shows fewer insert rows; the fader takes whatever height is left.
constexpr int kStripWidth = 112, kHeader = 20, kInsertRow = 17, kInsertRows = 4;
constexpr int kCompactStripHeight = 170;  // the shortest strip (below it the strips scroll)
constexpr float kMinDb = -60.0f;

// A level (linear peak) as a share of the meter's height: -60 dB at the bottom, +6 dB at the top.
float meterShare(float level) {
  if (level <= 0.0f) return 0.0f;
  const float db = 20.0f * std::log10(level);
  return juce::jlimit(0.0f, 1.0f, (db - kMinDb) / (6.0f - kMinDb));
}

doc::Track* trackIn(doc::Document& d, const std::string& id) {
  for (auto& t : d.tracks) if (t.id == id) return &t;
  return nullptr;
}
}  // namespace

// ---------------------------------------------------------------- one strip (a track, a bus or the master)
class MixerPanel::Strip final : public juce::Component {
public:
  Strip(MixerPanel& owner, std::string trackId) : owner_(owner), id_(std::move(trackId)) {
    name_.setJustificationType(juce::Justification::centredLeft);
    name_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.06f));
    name_.setColour(juce::Label::textColourId, kText);
    name_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(name_);

    send_.setTooltip(u8("A copy of this track into a bus (post-fader)"));
    send_.onChange = [this] { sendChosen(); };
    sendLevel_.setSliderStyle(juce::Slider::LinearBar);
    sendLevel_.setRange(-60.0, 6.0, 0.1);
    sendLevel_.setTextValueSuffix(" dB");
    sendLevel_.setDoubleClickReturnValue(true, 0.0);
    sendLevel_.setTooltip(u8("Send level (-60 to +6 dB). Drag to change, double-click to type a value, Alt+click: 0 dB."));
    sendLevel_.onDragStart = [this] { owner_.host_.startGesture(u8("Send level")); };
    sendLevel_.onValueChange = [this] {
      const double v = sendLevel_.getValue();
      edit([&](doc::Document& d) {
        if (const doc::Track* t = d.findTrack(id_); t && !t->mix.sends.empty()) doc::setSend(d, id_, t->mix.sends.front().id, v, t->mix.sends.front().pre);
      }, u8("Send level"));
    };
    sendLevel_.onDragEnd = [this] { owner_.host_.finishGesture(); };
    output_.setTooltip(u8("Where this track goes: the master or a bus"));
    output_.onChange = [this] { outputChosen(); };

    pan_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pan_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pan_.setRange(-1.0, 1.0, 0.01);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTooltip(u8("Pan. Double-click: centre."));
    pan_.onDragStart = [this] { owner_.host_.startGesture(u8("Pan")); };
    pan_.onValueChange = [this] {
      const double v = pan_.getValue();
      edit([&](doc::Document& d) { if (doc::Track* t = trackIn(d, id_)) t->mix.pan = v; }, u8("Pan"));
    };
    pan_.onDragEnd = [this] { owner_.host_.finishGesture(); };

    fader_.setSliderStyle(juce::Slider::LinearVertical);
    theme::markLevel(fader_);  // a level: light blue-grey
    fader_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    fader_.setRange(-60.0, 12.0, 0.1);
    fader_.setSkewFactorFromMidPoint(-12.0);
    fader_.setDoubleClickReturnValue(true, 0.0);
    fader_.onDragStart = [this] { owner_.host_.startGesture(master() ? u8("Master volume") : u8("Volume")); };
    fader_.onValueChange = [this] {
      const double v = fader_.getValue();
      edit([&](doc::Document& d) {
        if (master()) d.masterGainDb = v;
        else if (doc::Track* t = trackIn(d, id_)) t->mix.gainDb = v;
      }, master() ? u8("Master volume") : u8("Volume"));
      repaint();
    };
    fader_.onDragEnd = [this] { owner_.host_.finishGesture(); };

    mute_.setButtonText("M");
    solo_.setButtonText("S");
    mute_.setTooltip("Mute");
    solo_.setTooltip("Solo");
    mute_.setColour(juce::TextButton::buttonOnColourId, theme::kBlueLight);  // a normal button: light when on (amber is a state)
    solo_.setColour(juce::TextButton::buttonOnColourId, kPrimary);
    mute_.onClick = [this] { edit([&](doc::Document& d) { if (doc::Track* t = trackIn(d, id_)) t->mix.mute = !t->mix.mute; }, u8("Mute")); };
    solo_.onClick = [this] { edit([&](doc::Document& d) { if (doc::Track* t = trackIn(d, id_)) t->mix.solo = !t->mix.solo; }, u8("Solo")); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&send_, &sendLevel_, &output_, &pan_, &fader_, &mute_, &solo_}) {
      c->setWantsKeyboardFocus(false);
      addAndMakeVisible(c);
    }
    const bool tracks = !master();
    for (juce::Component* c : std::initializer_list<juce::Component*>{&send_, &sendLevel_, &output_, &pan_, &mute_, &solo_})
      c->setVisible(tracks);
    update();
  }

  const std::string& id() const { return id_; }
  bool master() const { return id_.empty(); }

  void update() {
    const doc::Document& d = owner_.host_.currentDocument();
    const doc::Track* t = master() ? nullptr : d.findTrack(id_);
    if (!master() && !t) return;
    name_.setText(master() ? juce::String("MASTER") : u8(t->name).toUpperCase(), juce::dontSendNotification);
    bus_ = t && t->instrument.kind == "bus";
    colour_ = t ? trackColour(*t) : kMasterColour;  // the same colour as the track's lane and clips
    theme::setIdentity(*this, colour_);  // the pan knob and the fader's fill in the same colour
    effects_.clear();
    for (const auto& fx : master() ? d.masterEffects : t->effects) {
      const doc::EffectInfo* info = doc::findEffect(doc::effectName(fx));
      const std::string label = info ? std::string(info->label) : doc::effectName(fx);
      effects_.push_back({u8(label), !doc::effectBypassed(fx)});
    }
    const double gain = master() ? d.masterGainDb : t->mix.gainDb;
    if (!fader_.isMouseButtonDown()) fader_.setValue(gain, juce::dontSendNotification);
    const auto& automation = master() ? d.masterAutomation : t->automation;
    const bool automated = std::any_of(automation.begin(), automation.end(), [](const doc::Automation& a) { return a.param == "gainDb"; });
    fader_.setEnabled(!automated);
    fader_.setTooltip(automated ? u8("The volume is automated: change it on the curve.") : u8("Volume (-60 to +12 dB). Double-click: 0 dB."));
    if (master()) { repaint(); return; }
    if (!pan_.isMouseButtonDown()) pan_.setValue(t->mix.pan, juce::dontSendNotification);
    mute_.setToggleState(t->mix.mute, juce::dontSendNotification);
    solo_.setToggleState(t->mix.solo, juce::dontSendNotification);
    // The buses this strip may send or go to (not itself).
    buses_.clear();
    for (const auto& other : d.tracks)
      if (other.instrument.kind == "bus" && other.id != id_) buses_.push_back({other.id, u8(other.name)});
    send_.clear(juce::dontSendNotification);
    output_.clear(juce::dontSendNotification);
    send_.addItem(u8("—"), 1);
    output_.addItem(u8("Master"), 1);
    for (size_t i = 0; i < buses_.size(); ++i) {
      send_.addItem(buses_[i].second, (int)i + 2);
      output_.addItem(buses_[i].second, (int)i + 2);
    }
    const std::string sendTo = t->mix.sends.empty() ? std::string() : t->mix.sends.front().to;
    send_.setSelectedId(idOfBus(sendTo), juce::dontSendNotification);
    output_.setSelectedId(idOfBus(t->mix.output), juce::dontSendNotification);
    send_.setEnabled(!buses_.empty());
    send_.setTooltip(buses_.empty() ? u8("No bus yet: + Bus adds one") : u8("A copy of this track into a bus (post-fader)"));
    if (sendLevel_.isVisible() != !t->mix.sends.empty()) {
      sendLevel_.setVisible(!t->mix.sends.empty());
      resized();  // the level's row comes and goes
    }
    if (!t->mix.sends.empty() && !sendLevel_.isMouseButtonDown()) sendLevel_.setValue(t->mix.sends.front().gainDb, juce::dontSendNotification);
    repaint();
  }

  void setLevels(float left, float right) {
    if (std::abs(left - left_) < 1e-4f && std::abs(right - right_) < 1e-4f) return;
    left_ = left;
    right_ = right;
    repaint(meterArea());
  }

  void setCpu(float pct) {
    if (std::abs(pct - cpu_) < 0.05f) return;
    cpu_ = pct;
    repaint(cpuArea_);
  }

  void paint(juce::Graphics& g) override {
    const bool chosen = owner_.selected_ == id_;
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(kCard);
    g.fillRoundedRectangle(r, theme::kCorner);
    // The header carries the track's colour (its entity): a solid edge on top, the name's band tinted; the body
    // stays charcoal. A bus also gets a thin line under its name, so it reads as a group.
    {
      juce::Path head;
      head.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), (float)kHeader - 8, theme::kCorner, theme::kCorner, true, true, false, false);
      g.setColour(colour_.withAlpha(0.22f));
      g.fillPath(head);
      g.setColour(colour_);
      juce::Path edge;
      edge.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), 3.0f, theme::kCorner, theme::kCorner, true, true, false, false);
      g.fillPath(edge);
      if (bus_) {
        g.setColour(colour_.withAlpha(0.7f));
        g.fillRect(8.0f, (float)kHeader - 6, (float)getWidth() - 16, 2.0f);
      }
    }
    // The chosen strip: the 2 px line in its own colour (Diogo 2026-11-09: "o que está selecionado ganha a cor dele").
    g.setColour(chosen ? colour_ : kOutline);
    g.drawRoundedRectangle(chosen ? r.reduced(0.5f) : r, theme::kCorner, chosen ? 2.0f : 1.0f);
    // Inserts: one row per effect (lit when on), empty rows dotted.
    auto rows = insertsArea();
    g.setFont(juce::FontOptions(theme::kFontSmall));
    for (int i = 0; i < insertRows(); ++i) {
      auto row = rows.removeFromTop(kInsertRow).reduced(0, 1).toFloat();
      g.setColour(kField);
      g.fillRoundedRectangle(row, theme::kCorner);
      if (i < (int)effects_.size()) {
        const bool on = effects_[(size_t)i].second;
        g.setColour(on ? colour_ : kDim);  // an effect that is on lights in the track's colour
        g.fillEllipse(row.getRight() - 12, row.getCentreY() - 3, 6, 6);
        g.setColour(on ? kText : kMuted);
        // Squeezed a little before it ends in "...": the full name is on the effect's card.
        g.drawFittedText(effects_[(size_t)i].first, row.withTrimmedLeft(6).withTrimmedRight(16).toNearestInt(),
                         juce::Justification::centredLeft, 1, 0.85f);
      } else if (i == (int)effects_.size()) {
        g.setColour(kDim);
        g.drawText(lang::tr(u8("+ insert")), row.withTrimmedLeft(6), juce::Justification::centredLeft, true);
      }
    }
    if (effects_.size() > (size_t)insertRows()) {
      g.setColour(kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(lang::tr("+" + juce::String((int)effects_.size() - insertRows()) + u8(" more")), insertsArea().translated(0, kInsertRow * insertRows()).withHeight(11),
                 juce::Justification::centredRight, false);
    }
    // The meter: L and R bars, a dB scale on the left of the fader.
    const auto meter = meterArea();
    g.setFont(juce::FontOptions(theme::kFontSmall));
    float lastLabel = -1e9f;
    for (const float db : {-60.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f, 6.0f}) {  // from the bottom: a short meter drops labels that would touch
      const float y = (float)meter.getBottom() - meterShare(std::pow(10.0f, db / 20.0f)) * (float)meter.getHeight();
      if (std::abs(y - lastLabel) < 11.0f) continue;
      lastLabel = y;
      g.setColour(kDim);
      g.drawText(db > 0 ? "+" + juce::String((int)db) : juce::String((int)db), meter.getX() - 30, (int)y - 6, 24, 12, juce::Justification::centredRight);
      g.setColour(kOutline.withAlpha(0.6f));
      g.fillRect((float)meter.getX() - 4, y, 3.0f, 1.0f);
    }
    for (int ch = 0; ch < 2; ++ch) {
      const auto bar = juce::Rectangle<float>((float)meter.getX() + ch * 8.0f, (float)meter.getY(), 6.0f, (float)meter.getHeight());
      g.setColour(kField);
      g.fillRect(bar);
      const float level = ch == 0 ? left_ : right_;
      const float h = meterShare(level) * bar.getHeight();
      g.setColour(level >= 1.0f ? kError : level >= 0.5f ? kWarn : theme::kBlueLight);  // a level: light blue-grey
      g.fillRect(bar.withTop(bar.getBottom() - h));
    }
    // Where the audio time goes: this strip's share of one core (from engine.cpu, once a second).
    if (!cpuArea_.isEmpty()) {
      auto c = cpuArea_;
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.setColour(kDim);
      g.drawText(lang::tr("CPU"), c.removeFromLeft(26), juce::Justification::centredLeft, false);
      g.setColour(kMuted);
      g.drawText(cpu_ < 0 ? juce::String(u8("–")) : juce::String(cpu_, 1) + "%", c.removeFromRight(36), juce::Justification::centredRight, false);
      const auto bar = c.reduced(2, 0).withSizeKeepingCentre(c.getWidth() - 4, 4).toFloat();
      g.setColour(kField);
      g.fillRoundedRectangle(bar, 2);
      if (cpu_ > 0) {
        g.setColour(cpu_ >= 25 ? kError : cpu_ >= 10 ? kWarn : kAccent);
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0.03f, 1.0f, cpu_ / 10.0f)), 2);  // a full bar: 10 % of a core
      }
    }
    // The fader's value, beside mute and solo.
    g.setColour(kText);
    g.setFont(juce::FontOptions(theme::kFontBody));
    const double v = fader_.getValue();
    g.drawText(lang::tr((v <= -59.95 ? juce::String(u8("-∞")) : juce::String(v, 1)) + " dB"), valueArea_, juce::Justification::centredRight, false);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(6, 5);
    name_.setBounds(r.removeFromTop(kHeader - 2));
    r.removeFromTop(2);
    r.removeFromTop(kInsertRow * insertRows() + (insertRows() < kInsertRows ? 12 : 4));  // the inserts (painted; "+N more" under them)
    const int listRow = 20, outRow = 22;
    if (!master()) {
      send_.setBounds(r.removeFromTop(listRow));
      if (sendLevel_.isVisible()) sendLevel_.setBounds(r.removeFromTop(16).reduced(0, 2));
      r.removeFromTop(3);
      auto row = r.removeFromTop(outRow);
      pan_.setBounds(row.removeFromRight(outRow));  // the pan knob beside the output list
      row.removeFromRight(4);
      output_.setBounds(row);
    } else {
      r.removeFromTop(listRow + 3 + outRow);  // the master's fader lines up with the others
    }
    r.removeFromTop(4);
    cpuArea_ = r.removeFromBottom(12);
    r.removeFromBottom(3);
    auto buttons = r.removeFromBottom(20);  // M, S and the fader's value on one row
    mute_.setBounds(buttons.removeFromLeft(24));
    buttons.removeFromLeft(3);
    solo_.setBounds(buttons.removeFromLeft(24));
    valueArea_ = buttons;
    r.removeFromBottom(3);
    faderArea_ = r;
    fader_.setBounds(r.withX(r.getRight() - 30).withWidth(26));
  }

  void mouseUp(const juce::MouseEvent& e) override {
    const auto inserts = insertsArea();
    if (e.y >= inserts.getBottom() + 4) return;
    // The name or the inserts choose the strip: its chain shows at the right.
    if (owner_.onSelect) owner_.onSelect(id_);
    // A right-click on an insert: the On and the Remove the track's and the master's menus already give for
    // every chain entry, so an effect is not stuck when the mixer is the page you are on (audit 2026-11-17, item 15).
    if (e.mods.isRightButtonDown() && e.y >= inserts.getY()) showInsertMenu();
  }

private:
  void showInsertMenu() {
    const auto& chain = doc::effectChain(owner_.host_.currentDocument(), id_);  // "" = the master's chain
    const int rows = std::min(insertRows(), (int)effects_.size());  // only the inserts this strip shows
    std::vector<std::string> ids;  // what each row of the menu stands for: found again by id when it is answered
    juce::PopupMenu menu;
    for (int i = 0; i < (int)chain.size() && i < rows; ++i) {
      ids.push_back(chain[(size_t)i].value("id", std::string()));
      juce::PopupMenu one;
      one.addItem(juce::PopupMenu::Item(u8("On")).setID(100 + i).setTicked(!doc::effectBypassed(chain[(size_t)i])));
      one.addItem(juce::PopupMenu::Item(u8("Remove")).setID(200 + i));
      menu.addSubMenu(effects_[(size_t)i].first, one);
    }
    if (ids.empty()) return;
    juce::Component::SafePointer<Strip> self(this);
    const juce::String what = master() ? juce::String("Master: ") : juce::String();
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [self, what, ids](int chosen) {
      if (!self || chosen == 0) return;
      const int row = chosen % 100;
      if (row < 0 || row >= (int)ids.size()) return;
      // The chain may have changed while the menu was up: the effect is looked for by its id, and a chain that
      // no longer has it is left alone.
      const auto& list = doc::effectChain(self->owner_.host_.currentDocument(), self->id_);
      int at = -1;
      for (int k = 0; k < (int)list.size(); ++k)
        if (list[(size_t)k].value("id", std::string()) == ids[(size_t)row]) { at = k; break; }
      if (at < 0) return;
      if (chosen < 200) {
        self->edit([self, at](doc::Document& d) {
          doc::setEffectBypass(d, self->id_, at, !doc::effectBypassed(doc::effectChain(d, self->id_)[(size_t)at]));
        }, what + u8("Effect on/off"));
      } else {
        self->owner_.host_.capturePluginStates();  // Undo brings the plugin back as it sounded, not as last saved
        self->edit([self, at](doc::Document& d) { doc::removeEffect(d, self->id_, at); }, what + u8("Remove effect"));
      }
    });
  }

  juce::Rectangle<int> insertsArea() const { return getLocalBounds().reduced(6, 5).withTrimmedTop(kHeader).withHeight(kInsertRow * insertRows()); }
  // A tall strip shows four inserts, shorter ones three or two, the shortest the first and "+N more".
  int insertRows() const { return getHeight() >= 380 ? kInsertRows : getHeight() >= 310 ? 3 : getHeight() >= 240 ? 2 : 1; }
  juce::Rectangle<int> meterArea() const {
    const auto r = faderArea_;
    return {r.getX() + 32, r.getY() + 4, 14, std::max(10, r.getHeight() - 8)};
  }
  int idOfBus(const std::string& bus) const {
    for (size_t i = 0; i < buses_.size(); ++i) if (buses_[i].first == bus) return (int)i + 2;
    return 1;
  }
  void edit(const std::function<void(doc::Document&)>& change, const juce::String& label) { owner_.host_.applyEdit(change, label); }

  void sendChosen() {
    const int chosen = send_.getSelectedId();
    const std::string to = chosen >= 2 && chosen - 2 < (int)buses_.size() ? buses_[(size_t)chosen - 2].first : std::string();
    edit([&](doc::Document& d) {
      const doc::Track* t = d.findTrack(id_);
      if (!t) return;
      double gain = 0;
      bool pre = false;
      if (!t->mix.sends.empty()) {  // the first send changes bus: the same level, to the new one
        gain = t->mix.sends.front().gainDb;
        pre = t->mix.sends.front().pre;
        doc::removeSend(d, id_, t->mix.sends.front().id);
      }
      if (!to.empty()) doc::addSend(d, id_, to, gain, pre);
    }, to.empty() ? u8("Remove send") : u8("Send to ") + send_.getText());
  }

  void outputChosen() {
    const int chosen = output_.getSelectedId();
    const std::string to = chosen >= 2 && chosen - 2 < (int)buses_.size() ? buses_[(size_t)chosen - 2].first : std::string();
    edit([&](doc::Document& d) { doc::setTrackOutput(d, id_, to); }, u8("Output: ") + output_.getText());
  }

  MixerPanel& owner_;
  std::string id_;
  bool bus_ = false;
  juce::Colour colour_ = kMasterColour;
  std::vector<std::pair<juce::String, bool>> effects_;
  std::vector<std::pair<std::string, juce::String>> buses_;
  juce::Label name_;
  juce::ComboBox send_, output_;
  theme::BarSlider sendLevel_;  // drag to change, double-click to type
  juce::Slider pan_, fader_;
  juce::TextButton mute_, solo_;
  juce::Rectangle<int> faderArea_, cpuArea_, valueArea_;
  float left_ = 0, right_ = 0;
  float cpu_ = -1;  // % of one core; -1 = not measured yet
};

// ---------------------------------------------------------------- the panel

MixerPanel::MixerPanel(PatternEditor::Host& host) : host_(host) {
  title_.setText("MIXER", juce::dontSendNotification);
  title_.setFont(juce::FontOptions(theme::kFontBody, juce::Font::bold).withKerningFactor(0.08f));
  title_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(title_);
  addBus_.setButtonText(u8("+ Bus"));
  addBus_.setTooltip(u8("A bus: tracks can go to it (Output) or send a copy to it (Send), e.g. a shared reverb or a drum group"));
  addBus_.onClick = [this] {
    int n = 1;
    for (const auto& t : host_.currentDocument().tracks) n += t.instrument.kind == "bus";
    std::string id;
    host_.applyEdit([&](doc::Document& d) { id = doc::addBusTrack(d, "Bus " + std::to_string(n)); }, u8("Add bus"));
    if (!id.empty() && onSelect) onSelect(id);
  };
  close_.setButtonText(u8("✕"));
  close_.setTooltip(u8("Close the mixer (back to the piano roll)"));
  close_.onClick = [this] { if (onClose) onClose(); };
  for (auto* b : {&addBus_, &close_}) {
    b->setWantsKeyboardFocus(false);
    addAndMakeVisible(b);
  }
  view_.setViewedComponent(&strip_, false);
  view_.setScrollBarsShown(true, true);
  addAndMakeVisible(view_);
  documentChanged();
}

MixerPanel::~MixerPanel() = default;

std::string MixerPanel::shapeOf() const {
  std::string s;
  for (const auto& t : host_.currentDocument().tracks) s += t.id + ":" + t.instrument.kind + "|";
  return s;
}

void MixerPanel::documentChanged() {
  const doc::Document& d = host_.currentDocument();
  if (!selected_.empty() && !d.findTrack(selected_)) select({});
  if (shapeOf() != shape_) { triggerAsyncUpdate(); return; }
  for (auto& s : strips_) s->update();
}

void MixerPanel::handleAsyncUpdate() {
  shape_ = shapeOf();
  strips_.clear();
  for (const auto& t : host_.currentDocument().tracks) {
    strips_.push_back(std::make_unique<Strip>(*this, t.id));
    strip_.addAndMakeVisible(*strips_.back());
  }
  strips_.push_back(std::make_unique<Strip>(*this, std::string()));  // the master, last: beside the strips, never scrolled away
  addAndMakeVisible(*strips_.back());
  resized();
}

void MixerPanel::select(const std::string& trackId) {
  selected_ = trackId;
  for (auto& s : strips_) s->repaint();
  resized();
}

void MixerPanel::setLevels(const std::map<std::string, std::pair<float, float>>& tracks, float masterLeft, float masterRight) {
  if (!isShowing()) return;
  for (auto& s : strips_) {
    if (s->master()) { s->setLevels(masterLeft, masterRight); continue; }
    const auto it = tracks.find(s->id());
    s->setLevels(it == tracks.end() ? 0.0f : it->second.first, it == tracks.end() ? 0.0f : it->second.second);
  }
}

void MixerPanel::setCpu(const std::map<std::string, float>& tracks, float master) {
  for (auto& s : strips_) {
    if (s->master()) { s->setCpu(master); continue; }
    const auto it = tracks.find(s->id());
    s->setCpu(it == tracks.end() ? 0.0f : it->second);
  }
}

void MixerPanel::paint(juce::Graphics& g) {
  g.setColour(kPanel);
  g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
}

void MixerPanel::resized() {
  auto r = getLocalBounds().reduced(8, 6);
  auto head = r.removeFromTop(24);
  close_.setBounds(head.removeFromRight(28).reduced(2));
  title_.setBounds(head.removeFromLeft(90));
  addBus_.setBounds(head.removeFromLeft(70).reduced(2));
  r.removeFromTop(4);
  auto masterColumn = r.removeFromRight(kStripWidth);  // the master strip stays in view while the others scroll
  r.removeFromRight(10);
  // When the strips scroll, the view shows only whole strips (the spare pixels go to the gap).
  if ((int)(strips_.size() - 1) * (kStripWidth + 6) > r.getWidth())
    r.setWidth(std::max(kStripWidth, (r.getWidth() + 6) / (kStripWidth + 6) * (kStripWidth + 6) - 6));
  // Fewer strips than the room: the view ends at the last one, so MASTER follows it without a gap.
  r.setWidth(std::min(r.getWidth(), std::max(kStripWidth, (int)(strips_.size() - 1) * (kStripWidth + 6) - 6)));
  view_.setBounds(r);
  masterColumn.setX(r.getRight() + 10);  // right after the last whole strip
  masterColumn.setWidth(kStripWidth);
  const int bar = view_.getScrollBarThickness();
  const int width = (int)(strips_.size() - 1) * (kStripWidth + 6);
  const bool tall = r.getHeight() - (width > r.getWidth() ? bar : 0) >= kCompactStripHeight;
  const int height = std::max(kCompactStripHeight, r.getHeight() - (width > r.getWidth() - (tall ? 0 : bar) ? bar : 0));
  int x = 0;
  for (auto& s : strips_) {
    if (s->master()) { s->setBounds(masterColumn.withHeight(height)); continue; }
    s->setBounds(x, 0, kStripWidth, height);
    x += kStripWidth + 6;
  }
  strip_.setSize(x, height);
}

} // namespace core::app
