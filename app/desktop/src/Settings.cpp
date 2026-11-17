// File > Settings (Ctrl+,): the program's own settings, kept in Core.settings (juce::ApplicationProperties).
//
// Folders: where songs are saved and opened, where exports go, the sample library. Samplers: each sampler's
// folders of the library. Keyboard shortcuts (the second tab): every action of the keymap (Keymap.h) with its keys; click a key to press a new
// one, + adds another, x clears, Default puts it back. Nothing here changes the song: every value applies from the
// next save, export or key press.
#include "MainComponent.h"
#include "Keymap.h"
#include "Language.h"
#include "roles.hpp"
#include "samples.hpp"
#include "Theme.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <tuple>

namespace core::app {
using namespace theme;
namespace {

juce::String ellipsisPath(const juce::String& path, const juce::Font& font, int width) {
  juce::String p = path;
  while (p.length() > 4 && juce::GlyphArrangement::getStringWidthInt(font, p) > width) p = juce::String::fromUTF8("\xe2\x80\xa6") + p.substring(2);
  return p;
}

// One folder: its name, where it is now (or what the default does), Choose... and Default.
class FolderRow final : public juce::Component {
 public:
  FolderRow(juce::String title, juce::String help) : help_(std::move(help)) {
    title_.setText(title, juce::dontSendNotification);
    title_.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kText);
    choose_.setButtonText(juce::String::fromUTF8("Choose\xe2\x80\xa6"));
    reset_.setButtonText("Default");
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &choose_, &reset_}) addAndMakeVisible(c);
  }
  void show(const juce::String& path, bool isDefault) {
    path_ = path;
    isDefault_ = isDefault;
    reset_.setEnabled(!isDefault);
    repaint();
  }
  juce::TextButton choose_, reset_;

  void paint(juce::Graphics& g) override {
    const auto box = pathBox();
    g.setColour(kField);
    g.fillRoundedRectangle(box.toFloat(), theme::kCorner);
    g.setColour(kOutline);
    g.drawRoundedRectangle(box.toFloat().reduced(0.5f), theme::kCorner, 1.0f);
    const juce::Font font{juce::FontOptions(theme::kFontBody)};
    g.setFont(font);
    g.setColour(isDefault_ ? kMuted : kText);
    g.drawText(ellipsisPath(path_, font, box.getWidth() - 16), box.reduced(8, 0), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(theme::kFontBody));
    g.drawText(lang::tr(help_), getLocalBounds().removeFromBottom(18).withTrimmedLeft(150), juce::Justification::centredLeft);
  }
  void resized() override {
    auto r = getLocalBounds().removeFromTop(28);
    title_.setBounds(r.removeFromLeft(150));
    reset_.setBounds(r.removeFromRight(80));
    r.removeFromRight(6);
    choose_.setBounds(r.removeFromRight(90));
  }

 private:
  juce::Rectangle<int> pathBox() const { return getLocalBounds().removeFromTop(28).withTrimmedLeft(150).withTrimmedRight(80 + 6 + 90 + 8); }
  juce::Label title_;
  juce::String path_, help_;
  bool isDefault_ = true;
};

// A key to press: a click records the next key (Esc cancels); a "hold" action also takes a modifier alone.
class KeyButton final : public juce::TextButton {
 public:
  std::function<void(Key)> onKey;
  bool hold = false;
  void start() {
    recording_ = true;
    modifier_ = {};
    setButtonText(juce::String::fromUTF8(hold ? "Press a key or a modifier\xe2\x80\xa6 (Esc cancels)" : "Press the new key\xe2\x80\xa6 (Esc cancels)"));
    setToggleState(true, juce::dontSendNotification);
    setWantsKeyboardFocus(true);
    getProperties().set(GlobalKeys::kTakesAllKeys, true);  // Space is recorded, not played
    grabKeyboardFocus();
  }
  void stop() {
    recording_ = false;
    setToggleState(false, juce::dontSendNotification);
    setWantsKeyboardFocus(false);
    getProperties().remove(GlobalKeys::kTakesAllKeys);
  }
  bool recording() const { return recording_; }
  bool keyPressed(const juce::KeyPress& press) override {
    if (!recording_) return false;
    modifier_ = {};
    if (press == juce::KeyPress::escapeKey) { stop(); if (onKey) onKey({}); return true; }
    stop();
    if (onKey) onKey(Key::of(press));
    return true;
  }
  void modifierKeysChanged(const juce::ModifierKeys& m) override {
    if (!recording_ || !hold) return;
    if (m.isCtrlDown() || m.isShiftDown() || m.isAltDown()) {  // the modifiers held so far
      modifier_.ctrl = modifier_.ctrl || m.isCtrlDown();
      modifier_.shift = modifier_.shift || m.isShiftDown();
      modifier_.alt = modifier_.alt || m.isAltDown();
    } else if (modifier_.valid()) {  // all released with no key between: the modifier alone
      const Key key = modifier_;
      stop();
      if (onKey) onKey(key);
    }
  }
  void focusLost(FocusChangeType) override {
    if (recording_) { stop(); if (onKey) onKey({}); }
  }

 private:
  bool recording_ = false;
  Key modifier_;
};

class ShortcutRow final : public juce::Component {
 public:
  explicit ShortcutRow(const KeyAction& a) : action(a) {
    name_.setText(a.name, juce::dontSendNotification);
    name_.setFont(juce::FontOptions(theme::kFontBody));
    name_.setColour(juce::Label::textColourId, kText);
    keys.hold = a.hold;
    keys.setColour(juce::TextButton::buttonOnColourId, kPrimary);
    keys.setTooltip("Click, then press the new key");
    add.setButtonText("+");
    add.setTooltip("Add another key for this");
    clear.setButtonText(juce::String::fromUTF8("\xe2\x9c\x95"));
    clear.setTooltip("No key");
    reset.setButtonText("Default");
    for (juce::Component* c : std::initializer_list<juce::Component*>{&name_, &keys, &add, &clear, &reset}) {
      c->setWantsKeyboardFocus(false);
      addAndMakeVisible(c);
    }
  }
  void show() {
    if (keys.recording()) return;
    juce::StringArray texts;
    for (const Key& k : Keymap::get().keys(action.id)) texts.add(k.text());
    keys.setButtonText(texts.isEmpty() ? juce::String("None") : texts.joinIntoString(",  "));
    clear.setEnabled(!texts.isEmpty());
    reset.setEnabled(!Keymap::get().isDefault(action.id));
  }
  void resized() override {
    auto r = getLocalBounds().reduced(0, 3);
    name_.setBounds(r.removeFromLeft(r.getWidth() - 400));
    reset.setBounds(r.removeFromRight(70));
    r.removeFromRight(4);
    clear.setBounds(r.removeFromRight(28));
    r.removeFromRight(4);
    add.setBounds(r.removeFromRight(28));
    r.removeFromRight(6);
    keys.setBounds(r);
  }
  const KeyAction& action;
  KeyButton keys;
  juce::TextButton add, clear, reset;

 private:
  juce::Label name_;
};

class ShortcutsPage final : public juce::Component {
 public:
  explicit ShortcutsPage(std::function<void()> saved) : saved_(std::move(saved)) {
    juce::String group;
    int y = 0;
    for (const KeyAction& a : Keymap::actions()) {
      if (a.group != group) {
        group = a.group;
        auto& l = *headers_.emplace_back(std::make_unique<juce::Label>());
        l.setText(group, juce::dontSendNotification);
        l.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
        l.setColour(juce::Label::textColourId, kTitle);  // recoloured below in the neutral scale (rule 11)
        l.setBounds(0, y + (y > 0 ? 10 : 0), 300, 24);
        content_.addAndMakeVisible(l);
        y += y > 0 ? 36 : 26;
      }
      auto& row = *rows_.emplace_back(std::make_unique<ShortcutRow>(a));
      row.setTopLeftPosition(0, y);
      y += 34;
      ShortcutRow* r = &row;
      row.keys.onClick = [this, r] { adding_ = false; r->keys.start(); };
      row.add.onClick = [this, r] { adding_ = true; r->keys.start(); };
      row.keys.onKey = [this, r](Key key) { assign(*r, key); };
      row.clear.onClick = [this, r] { Keymap::get().set(r->action.id, {}); changed(juce::String()); };
      row.reset.onClick = [this, r] { Keymap::get().reset(r->action.id); changed(juce::String()); };
      content_.addAndMakeVisible(row);
    }
    for (size_t i = 0; i < headers_.size(); ++i)  // each group's title a tone of the neutral scale, light first
      headers_[i]->setColour(juce::Label::textColourId, theme::neutralScale((int)headers_.size(), (int)i).interpolatedWith(kText, 0.45f));
    contentHeight_ = y + 8;
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    resetAll_.setButtonText("Reset all to defaults");
    resetAll_.onClick = [this] { Keymap::get().resetAll(); changed("Every shortcut is back to its default."); };
    addAndMakeVisible(resetAll_);
    notice_.setFont(juce::FontOptions(theme::kFontBody));
    notice_.setColour(juce::Label::textColourId, kAccentBright);
    addAndMakeVisible(notice_);
    fixed_.setFont(juce::FontOptions(theme::kFontBody));
    fixed_.setColour(juce::Label::textColourId, kDim);
    fixed_.setJustificationType(juce::Justification::topLeft);
    fixed_.setText("Fixed: Esc closes a panel; the letter keys without Ctrl play notes on the typing keyboard (a letter given to a shortcut "
                   "here stops playing its note). Ctrl+drag copies a clip; the middle mouse button always drags the view.",
                   juce::dontSendNotification);
    addAndMakeVisible(fixed_);
    for (auto& r : rows_) r->show();
  }
  void resized() override {
    auto r = getLocalBounds().reduced(16, 12);
    auto bottom = r.removeFromBottom(40);
    resetAll_.setBounds(bottom.removeFromRight(170).withSizeKeepingCentre(170, 30));
    fixed_.setBounds(bottom);
    notice_.setBounds(r.removeFromBottom(24));
    r.removeFromBottom(4);
    viewport_.setBounds(r);
    const int w = r.getWidth() - viewport_.getScrollBarThickness() - 4;
    content_.setSize(w, contentHeight_);
    for (auto& row : rows_) row->setSize(w, 34);
  }

 private:
  void assign(ShortcutRow& row, Key key) {
    if (!key.valid()) { row.show(); return; }  // cancelled
    if (key.code == 0 && !row.action.hold) { changed("A modifier alone only works for the hold actions."); return; }
    Keymap& km = Keymap::get();
    juce::String note;
    const std::string other = km.actionFor(key, row.action.id);
    if (!other.empty()) {  // a key does one thing: it moves here
      std::vector<Key> keys = km.keys(other);
      keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
      km.set(other, keys);
      note = key.text() + " was on \"" + Keymap::action(other)->name + "\": it is now here.";
    }
    std::vector<Key> keys = adding_ ? km.keys(row.action.id) : std::vector<Key>{};
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    km.set(row.action.id, keys);
    changed(note);
  }
  void changed(const juce::String& note) {
    notice_.setText(note, juce::dontSendNotification);
    for (auto& r : rows_) r->show();
    if (saved_) saved_();
  }

  std::function<void()> saved_;
  juce::Viewport viewport_;
  juce::Component content_;
  std::vector<std::unique_ptr<ShortcutRow>> rows_;
  std::vector<std::unique_ptr<juce::Label>> headers_;
  juce::TextButton resetAll_;
  juce::Label notice_, fixed_;
  int contentHeight_ = 0;
  bool adding_ = false;
};

// Settings > Samplers: the folders of each sampler role (kick, bass, hats...). A role with folders takes its sample
// suggestions (the track's sample menu, a template's samples, the AI) only from them, and every sample in them counts
// as that role. Kept as "samplerFolders" (role -> absolute folders); only folders inside the sample library work.
class SamplersPage final : public juce::Component {
 public:
  struct Host {
    std::function<juce::File()> library;
    std::function<juce::var()> read;              // the setting, parsed (an object)
    std::function<void(const juce::var&)> write;  // stores it and puts it into the index
    std::function<int(const std::string& role)> count;  // samples in that role's folders now (-1: no index yet)
  };
  explicit SamplersPage(Host host) : host_(std::move(host)) {
    title_.setText("Sampler folders", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kTitle);
    help_.setFont(juce::FontOptions(theme::kFontBody));
    help_.setColour(juce::Label::textColourId, kMuted);
    help_.setJustificationType(juce::Justification::topLeft);
    help_.setText("Link each sampler to its folders of the sample library. A sampler with folders takes its samples only from them "
                  "(the track's sample menu, the templates' samples and the AI), and every sample in them counts as its kind. "
                  "Without folders it uses the whole library, by what Core knows of each sample.",
                  juce::dontSendNotification);
    notice_.setFont(juce::FontOptions(theme::kFontBody));
    notice_.setColour(juce::Label::textColourId, kAccentBright);
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &help_, &notice_}) addAndMakeVisible(c);
    static const char* labels[] = {"Kick", "Bass", "Closed hats", "Open hats", "Claps & snares", "Percussion", "Impacts", "Risers"};
    const auto& roles = core::doc::samplerRoles();
    for (size_t i = 0; i < roles.size(); ++i) {
      auto& row = *rows_.emplace_back(std::make_unique<Row>());
      row.role = roles[i];
      row.name.setText(i < std::size(labels) ? labels[i] : roles[i].c_str(), juce::dontSendNotification);
      row.name.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
      row.name.setColour(juce::Label::textColourId, kText);
      row.add.setButtonText(juce::String::fromUTF8("Add folder\xe2\x80\xa6"));
      row.clear.setButtonText("Whole library");
      row.clear.setTooltip("No folders: the sampler uses the whole library");
      Row* r = &row;
      row.add.onClick = [this, r] { choose(*r); };
      row.clear.onClick = [this, r] { setFolders(r->role, {}); notice_.setText({}, juce::dontSendNotification); };
      for (auto* c : std::initializer_list<juce::Component*>{&row.name, &row.add, &row.clear}) row.addAndMakeVisible(c);
      addAndMakeVisible(row);
    }
    refresh();
  }

  void refresh() {
    const juce::var all = host_.read();
    for (auto& r : rows_) {
      juce::StringArray folders;
      if (const auto* list = all[juce::Identifier(r->role.c_str())].getArray())
        for (const auto& f : *list) folders.add(f.toString());
      r->folders = folders;
      r->count = host_.count(r->role);
      r->clear.setEnabled(!folders.isEmpty());
      r->repaint();
    }
  }

  void resized() override {
    auto r = getLocalBounds().reduced(16, 12);
    title_.setBounds(r.removeFromTop(26));
    help_.setBounds(r.removeFromTop(52));
    r.removeFromTop(6);
    for (auto& row : rows_) {
      auto line = r.removeFromTop(64);
      row->setBounds(line);
      auto head = row->getLocalBounds().removeFromTop(28);
      row->name.setBounds(head.removeFromLeft(150));
      row->clear.setBounds(head.removeFromRight(110));
      head.removeFromRight(6);
      row->add.setBounds(head.removeFromRight(110));
      r.removeFromTop(4);
    }
    notice_.setBounds(r.removeFromTop(24));
  }

 private:
  struct Row final : public juce::Component {
    std::string role;
    juce::Label name;
    juce::TextButton add, clear;
    juce::StringArray folders;
    int count = -1;
    void paint(juce::Graphics& g) override {
      auto box = getLocalBounds().withTrimmedTop(32).withTrimmedLeft(150).withHeight(26);
      g.setColour(kField);
      g.fillRoundedRectangle(box.toFloat(), theme::kCorner);
      const juce::Font font{juce::FontOptions(theme::kFontBody)};
      g.setFont(font);
      g.setColour(folders.isEmpty() ? kMuted : kText);
      juce::String text = folders.isEmpty() ? juce::String("Whole library") : folders.joinIntoString(";  ");
      if (!folders.isEmpty() && count >= 0) text = juce::String(count) + (count == 1 ? " sample   " : " samples   ") + text;
      g.drawText(ellipsisPath(text, font, box.getWidth() - 16), box.reduced(8, 0), juce::Justification::centredLeft);
    }
  };

  void choose(Row& row) {
    const juce::File library = host_.library();
    if (!library.isDirectory()) { notice_.setText("Choose the sample library first (General > Sample library).", juce::dontSendNotification); return; }
    chooser_ = std::make_unique<juce::FileChooser>("A folder of the sample library for " + row.name.getText(), library);
    juce::Component::SafePointer<SamplersPage> safe(this);
    const std::string role = row.role;
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe, role, library](const juce::FileChooser& fc) {
      if (safe == nullptr || fc.getResult() == juce::File()) return;
      const juce::File f = fc.getResult();
      if (!f.isAChildOf(library)) {
        safe->notice_.setText("That folder is not inside the sample library (" + library.getFullPathName() + ").", juce::dontSendNotification);
        return;
      }
      juce::StringArray folders;
      for (auto& r : safe->rows_) if (r->role == role) folders = r->folders;
      folders.addIfNotAlreadyThere(f.getFullPathName());
      safe->notice_.setText({}, juce::dontSendNotification);
      safe->setFolders(role, folders);
    });
  }

  void setFolders(const std::string& role, const juce::StringArray& folders) {
    juce::var all = host_.read();
    auto* obj = all.getDynamicObject();
    if (obj == nullptr) { all = juce::var(new juce::DynamicObject()); obj = all.getDynamicObject(); }
    if (folders.isEmpty()) {
      obj->removeProperty(juce::Identifier(role.c_str()));
    } else {
      juce::Array<juce::var> list;
      for (const auto& f : folders) list.add(f);
      obj->setProperty(juce::Identifier(role.c_str()), list);
    }
    host_.write(all);
    refresh();
  }

  Host host_;
  juce::Label title_, help_, notice_;
  std::vector<std::unique_ptr<Row>> rows_;
  std::unique_ptr<juce::FileChooser> chooser_;
};

// Settings > Roles (Diogo, 2026-11-16): once, the plugin that plays each role on this computer (reverb -> Valhalla,
// bass -> Serum 2...), and for an effect role the plugin's parameters its macros move (the reverb's mix and size...).
// SUCK, PUM, the build-ups and the AI ask for a role and never name a plugin. Kept in %APPDATA%/Core/roles.json
// (roles.hpp); choosing a plugin reads its parameters (core-engine params, in its own process) and guesses the macros
// by their names, and "Parameters..." changes any of them.
std::filesystem::path rolesPath() { return std::filesystem::path(PluginCatalog::rolesFile().getFullPathName().toWideCharPointer()); }

class RolesPage final : public juce::Component {
 public:
  using json = nlohmann::json;
  struct Host {
    std::function<std::vector<json>(bool instrument)> plugins;  // what the scan found
    std::function<juce::File()> engine;
  };
  explicit RolesPage(Host host) : host_(std::move(host)) {
    title_.setText("Roles", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kTitle);
    help_.setFont(juce::FontOptions(theme::kFontBody));
    help_.setColour(juce::Label::textColourId, kMuted);
    help_.setJustificationType(juce::Justification::topLeft);
    help_.setText("Choose once the plugin for each role. SUCK, PUM, the build-ups and the AI put the role's plugin on a track and "
                  "move its parameters; they never name a plugin. No plugin in the list: Plugins > Scan plugins first.",
                  juce::dontSendNotification);
    notice_.setFont(juce::FontOptions(theme::kFontBody));
    notice_.setColour(juce::Label::textColourId, kAccentBright);
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &help_, &notice_}) addAndMakeVisible(c);
    for (const auto& role : core::doc::roleCatalog()) {
      auto& row = *rows_.emplace_back(std::make_unique<Row>());
      row.role = role.id;
      row.instrument = role.instrument;
      row.name.setText(role.label, juce::dontSendNotification);
      row.name.setFont(juce::FontOptions(theme::kFontStrong, juce::Font::bold));
      row.name.setColour(juce::Label::textColourId, kText);
      row.name.setTooltip(role.what);
      row.params.setButtonText(juce::String::fromUTF8("Parameters\xe2\x80\xa6"));
      row.params.setTooltip("Which of the plugin's parameters each macro moves");
      row.params.setVisible(!role.instrument);
      Row* r = &row;
      row.plugin.onChange = [this, r] { choosePlugin(*r); };
      row.params.onClick = [this, r] { chooseParams(*r); };
      for (auto* c : std::initializer_list<juce::Component*>{&row.name, &row.plugin, &row.params}) row.addChildComponent(c);
      row.name.setVisible(true);
      row.plugin.setVisible(true);
      addAndMakeVisible(row);
    }
    refresh();
  }

  void refresh() {
    std::string problem;
    map_ = core::doc::loadRoleMap(rolesPath(), &problem);
    if (!problem.empty()) notice_.setText("roles.json could not be read (" + juce::String(problem) + "): choose the roles again.", juce::dontSendNotification);
    for (auto& r : rows_) {
      r->choices = host_.plugins(r->instrument);
      r->plugin.clear(juce::dontSendNotification);
      r->plugin.addItem("(none)", 1);
      const core::doc::RoleBinding* b = map_.find(r->role);
      int selected = 1;
      for (size_t i = 0; i < r->choices.size(); ++i) {
        const json& p = r->choices[i];
        r->plugin.addItem(juce::String::fromUTF8(p.value("name", std::string("?")).c_str()) + "  (" +
                              juce::String::fromUTF8(p.value("vendor", std::string()).c_str()) + ")", (int)i + 2);
        if (b && p.value("id", std::string()) == b->plugin.value("id", std::string())) selected = (int)i + 2;
      }
      if (b && selected == 1) {  // chosen on another scan (the plugin moved or was removed): still shown
        r->plugin.addItem(juce::String::fromUTF8(b->plugin.value("name", std::string("?")).c_str()) + " (not found)", 999);
        selected = 999;
      }
      r->plugin.setSelectedId(selected, juce::dontSendNotification);
      r->params.setEnabled(b != nullptr && !r->busy);
      // While its parameters are being read the row cannot be given another plugin: that is how one plugin's
      // list came to be saved for another (audit 2026-11-17, item 18). The read always ends with a refresh().
      r->plugin.setEnabled(!r->busy);
      r->summary = summary(r->role);
      r->repaint();
    }
  }

  void resized() override {
    auto r = getLocalBounds().reduced(16, 12);
    title_.setBounds(r.removeFromTop(26));
    help_.setBounds(r.removeFromTop(40));
    notice_.setBounds(r.removeFromBottom(24));
    for (auto& row : rows_) {
      row->setBounds(r.removeFromTop(row->instrument ? 32 : 52));
      auto head = row->getLocalBounds().removeFromTop(28);
      row->name.setBounds(head.removeFromLeft(150));
      if (!row->instrument) { row->params.setBounds(head.removeFromRight(110)); head.removeFromRight(6); }
      row->plugin.setBounds(head);
      r.removeFromTop(4);
    }
  }

 private:
  struct Row final : public juce::Component {
    std::string role;
    bool instrument = false, busy = false;
    // Which read of the parameters this row is waiting for (audit 2026-11-17, item 18): a reply is only taken
    // when it is the one still asked for - the plugin may have been changed while it was being read.
    uint64_t generation = 0;
    juce::Label name;
    juce::ComboBox plugin;
    juce::TextButton params;
    std::vector<json> choices;
    json paramList;  // the plugin's parameters, once read
    juce::String summary;
    void paint(juce::Graphics& g) override {
      if (instrument) return;
      g.setFont(juce::FontOptions(theme::kFontBody));
      g.setColour(summary.startsWith("!") ? kAccentBright : kDim);
      g.drawText(summary.trimCharactersAtStart("!"), getLocalBounds().withTrimmedTop(30).withTrimmedLeft(150), juce::Justification::centredLeft);
    }
  };

  juce::String summary(const std::string& role) const {
    const core::doc::RoleInfo* info = core::doc::findRole(role);
    const core::doc::RoleBinding* b = map_.find(role);
    if (!info || info->instrument || !b) return {};
    juce::StringArray parts;
    bool missing = false;
    for (const auto& m : info->macros) {
      auto it = b->params.find(m.id);
      if (it == b->params.end()) { missing = true; parts.add(juce::String(m.label) + ": -"); continue; }
      juce::String name = "#" + juce::String(it->second);
      for (const auto& rr : rows_)
        if (rr->role == role && rr->paramList.is_array())
          for (const auto& p : rr->paramList) if (p.value("id", std::string()) == it->second) name = juce::String::fromUTF8(p.value("name", std::string()).c_str());
      parts.add(juce::String(m.label) + ": " + name);
    }
    return (missing ? "!" : "") + parts.joinIntoString("   ") + (missing ? juce::String::fromUTF8("   (Parameters\xe2\x80\xa6 to choose)") : juce::String());
  }

  void store() {
    try {
      core::doc::saveRoleMap(map_, rolesPath());
    } catch (const std::exception& e) {
      notice_.setText("Not saved: " + juce::String(e.what()), juce::dontSendNotification);
    }
  }

  // Reads the plugin's parameters in the background, then `then` on the message thread (the list, or null and why).
  void readParams(Row& row, std::function<void(RolesPage&, Row&, const json&, const juce::String&)> then) {
    const core::doc::RoleBinding* b = map_.find(row.role);
    if (!b) return;
    row.busy = true;
    const uint64_t gen = ++row.generation;  // this read, and no other, may answer for the row now
    row.params.setEnabled(false);
    notice_.setText(juce::String::fromUTF8("Reading the parameters of ") + juce::String::fromUTF8(b->plugin.value("name", std::string()).c_str()) +
                        juce::String::fromUTF8("\xe2\x80\xa6"), juce::dontSendNotification);
    juce::Component::SafePointer<RolesPage> safe(this);
    const json plugin = b->plugin;
    const std::string role = row.role;
    const juce::File engine = host_.engine();
    juce::Thread::launch([safe, plugin, role, gen, engine, then] {
      juce::String problem;
      const json list = PluginCatalog::paramsOf(engine, plugin, &problem);
      juce::MessageManager::callAsync([safe, role, gen, list, problem, then] {
        if (safe == nullptr) return;
        for (auto& r : safe->rows_)
          if (r->role == role) {
            // A reply of an older read (the row was given another plugin, or its role cleared, while this one
            // was on its way): it belongs to a plugin that is not there any more, so it is dropped whole.
            if (r->generation != gen) continue;
            r->busy = false;
            r->paramList = list;
            safe->notice_.setText(list.is_array() ? juce::String() : "Could not read its parameters: " + problem, juce::dontSendNotification);
            then(*safe, *r, list, problem);
            safe->refresh();
          }
      });
    });
  }

  void choosePlugin(Row& row) {
    const int id = row.plugin.getSelectedId();
    if (id == 999) return;
    if (id <= 1 || id - 2 >= (int)row.choices.size()) {
      map_.roles.erase(row.role);
      ++row.generation;  // a read on its way belongs to the plugin that is gone: it may not answer for this row
      row.busy = false;
      row.paramList = json();
      store();
      refresh();
      return;
    }
    map_.roles[row.role] = core::doc::RoleBinding{row.choices[(size_t)id - 2], {}, {}};
    ++row.generation;  // same: the read asked for below is the only one this row answers to now
    row.paramList = json();
    store();
    refresh();
    if (row.instrument) return;
    readParams(row, [](RolesPage& page, Row& r, const json& list, const juce::String&) {
      if (!list.is_array()) return;
      auto it = page.map_.roles.find(r.role);
      if (it == page.map_.roles.end()) return;
      it->second = core::doc::guessRoleBinding(r.role, it->second.plugin, list);
      page.store();
    });
  }

  void chooseParams(Row& row) {
    if (!row.paramList.is_array()) {
      readParams(row, [](RolesPage& page, Row& r, const json& list, const juce::String&) { if (list.is_array()) page.showParamsMenu(r); });
      return;
    }
    showParamsMenu(row);
  }

  void showParamsMenu(Row& row) {
    const core::doc::RoleInfo* info = core::doc::findRole(row.role);
    auto it = map_.roles.find(row.role);
    if (!info || it == map_.roles.end() || !row.paramList.is_array()) return;
    juce::PopupMenu menu;
    int id = 1;
    std::vector<std::pair<std::string, std::string>> picks;  // menu id - 1 -> (macro, param id; "" = none)
    for (const auto& m : info->macros) {
      juce::PopupMenu sub;
      const auto current = it->second.params.find(m.id);
      sub.addItem(id++, "(none)", true, current == it->second.params.end());
      picks.push_back({m.id, ""});
      for (const auto& p : row.paramList) {
        const std::string pid = p.value("id", std::string());
        sub.addItem(id++, juce::String::fromUTF8(p.value("name", std::string()).c_str()), true, current != it->second.params.end() && current->second == pid);
        picks.push_back({m.id, pid});
      }
      menu.addSubMenu(m.label, sub);
    }
    juce::Component::SafePointer<RolesPage> safe(this);
    const std::string role = row.role;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&row.params), [safe, role, picks](int r) {
      if (safe == nullptr || r <= 0 || r > (int)picks.size()) return;
      auto found = safe->map_.roles.find(role);
      if (found == safe->map_.roles.end()) return;
      const auto& [macro, pid] = picks[(size_t)r - 1];
      if (pid.empty()) found->second.params.erase(macro);
      else found->second.params[macro] = pid;
      safe->store();
      safe->refresh();
    });
  }

  Host host_;
  core::doc::RoleMap map_;
  juce::Label title_, help_, notice_;
  std::vector<std::unique_ptr<Row>> rows_;
};
}  // namespace

class MainComponent::SettingsPanel final : public juce::Component {
 public:
  SettingsPanel(MainComponent& owner, bool shortcuts = false) : o_(owner) {
    for (auto* l : {&foldersTitle_, &pianoTitle_}) {
      l->setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
      l->setColour(juce::Label::textColourId, kTitle);
      addAndMakeVisible(l);
    }
    // Language: the words of the whole program (English, or a file of lang/: Portuguese). Changes at once.
    languageTitle_.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
    languageTitle_.setColour(juce::Label::textColourId, kTitle);
    languageTitle_.setText("Language", juce::dontSendNotification);
    addAndMakeVisible(languageTitle_);
    {
      const auto choices = lang::choices();
      for (int i = 0; i < (int)choices.size(); ++i) {
        language_.addItem(choices[(size_t)i].name, i + 1);
        if (choices[(size_t)i].code == lang::current()) language_.setSelectedId(i + 1, juce::dontSendNotification);
      }
      if (language_.getSelectedId() == 0) language_.setSelectedId(1, juce::dontSendNotification);
      language_.setTooltip("The language of Core's menus, buttons, tips and plugin help. Names (styles, presets, plugins) stay as they are.");
      language_.onChange = [this, choices] {
        const int i = language_.getSelectedId() - 1;
        if (i < 0 || i >= (int)choices.size()) return;
        o_.settings_.getUserSettings()->setValue("language", choices[(size_t)i].code);
        o_.settings_.saveIfNeeded();
        lang::use(choices[(size_t)i].code);
        o_.menuItemsChanged();
        o_.resized();
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->setName(lang::tr("Settings"));
        repaint();
      };
      addAndMakeVisible(language_);
      languageNote_.setText(choices.size() > 1 ? "Words you typed (song, track and sample names) are never changed."
                                               : "Only English: the lang folder next to Core.exe is missing.",
                            juce::dontSendNotification);
      languageNote_.setColour(juce::Label::textColourId, kDim);
      languageNote_.setFont(juce::FontOptions(theme::kFontBody));
      addAndMakeVisible(languageNote_);
    }
    foldersTitle_.setText("Folders", juce::dontSendNotification);
    pianoTitle_.setText("Piano roll", juce::dontSendNotification);
    // Drawing help in the piano roll (both on by default; MainComponent::applyDrawingHelp reads them).
    for (auto [b, key, text, tip] : {std::tuple<juce::ToggleButton*, const char*, const char*, const char*>{
             &ghost_, "pianoGhost", "Show where the note will go", "A faint note under the mouse (draw tool): where a click puts it, on the grid."},
         {&guides_, "pianoGuides", "Show alignment guides", "A thin line when the note under the mouse, or one you drag, lines up with another note: same key (across) or same time (down)."}}) {
      b->setButtonText(text);
      b->setTooltip(tip);
      b->setColour(juce::ToggleButton::textColourId, kTitle);
      b->setColour(juce::ToggleButton::tickColourId, kAccent);
      b->setToggleState(o_.settings_.getUserSettings()->getBoolValue(key, true), juce::dontSendNotification);
      b->setWantsKeyboardFocus(false);
      b->onClick = [this, b, key] {
        o_.settings_.getUserSettings()->setValue(key, b->getToggleState());
        o_.settings_.saveIfNeeded();
        o_.applyDrawingHelp();
      };
      addAndMakeVisible(b);
    }

    for (auto* r : {&songs_, &exports_, &samples_, &trainingClips_}) addAndMakeVisible(r);
    folderButtons(songs_, "folderSongs", "Songs folder");
    folderButtons(exports_, "folderExports", "Exports folder");
    folderButtons(trainingClips_, "folderTrainingClips", "Training clips folder");
    samples_.choose_.onClick = [this] {
      chooseFolder("Sample library folder", o_.sampleLibraryFolder(), [this](const juce::File& f) { o_.setSampleLibrary(f); });
    };
    samples_.reset_.onClick = [this] {
      const juce::File before = o_.sampleLibraryFolder();
      o_.settings_.getUserSettings()->removeValue("sampleLibrary");
      const juce::File after = o_.sampleLibraryFolder();
      if (after != before && after.isDirectory()) {
        o_.setSampleLibrary(after);  // scans it (and stores it: removed again, so it stays "the default")
        o_.settings_.getUserSettings()->removeValue("sampleLibrary");
      }
      o_.settings_.saveIfNeeded();
      refresh();
    };
    // The two tabs: General (the above) and Keyboard shortcuts.
    keysPage_ = std::make_unique<ShortcutsPage>([this] {
      Keymap::get().save(*o_.settings_.getUserSettings());
      o_.settings_.saveIfNeeded();
    });
    addChildComponent(*keysPage_);
    {
      SamplersPage::Host host;
      host.library = [this] { return o_.sampleLibraryFolder(); };
      host.read = [this] {
        juce::var v = juce::JSON::parse(o_.settings_.getUserSettings()->getValue("samplerFolders", "{}"));
        return v.isObject() ? v : juce::var(new juce::DynamicObject());
      };
      host.write = [this](const juce::var& v) {
        o_.settings_.getUserSettings()->setValue("samplerFolders", juce::JSON::toString(v, true));
        o_.settings_.saveIfNeeded();
        o_.applySamplerFolders();
      };
      host.count = [this](const std::string& role) {
        if (!o_.hasSampleIndex_) return -1;
        int n = 0;
        for (const auto& smp : o_.sampleIndex_.samples) n += core::doc::inRoleFolders(o_.sampleIndex_, smp, role) ? 1 : 0;
        return n;
      };
      samplersPage_ = std::make_unique<SamplersPage>(std::move(host));
      addChildComponent(*samplersPage_);
    }
    {
      RolesPage::Host host;
      host.plugins = [this](bool instrument) {
        if (!o_.plugins_) return std::vector<nlohmann::json>{};
        return instrument ? o_.plugins_->instruments() : o_.plugins_->effects();
      };
      host.engine = [this] { return o_.plugins_ ? o_.plugins_->engine() : juce::File(); };
      rolesPage_ = std::make_unique<RolesPage>(std::move(host));
      addChildComponent(*rolesPage_);
    }
    for (auto* b : {&tabGeneral_, &tabSamplers_, &tabRoles_, &tabKeys_}) {
      b->setClickingTogglesState(true);
      b->setRadioGroupId(4801);
      b->setColour(juce::TextButton::buttonOnColourId, kPrimary);
      b->setWantsKeyboardFocus(false);
      addAndMakeVisible(b);
    }
    tabGeneral_.setButtonText("General");
    tabSamplers_.setButtonText("Samplers");
    tabKeys_.setButtonText("Keyboard shortcuts");
    tabRoles_.setButtonText("Roles");
    tabRoles_.onClick = [this] { showTab(3); };
    tabGeneral_.onClick = [this] { showTab(0); };
    tabSamplers_.onClick = [this] { showTab(1); };
    tabKeys_.onClick = [this] { showTab(2); };

    setSize(820, 800);
    showTab(shortcuts ? 2 : 0);
    refresh();
  }

  void paint(juce::Graphics& g) override {
    g.fillAll(kPanel);
    g.setColour(kOutline);
    g.fillRect(juce::Rectangle<int>(0, kTabs, getWidth(), 1));
  }

  void showTab(int tab) {  // 0 General, 1 Samplers, 2 Keyboard shortcuts, 3 Roles
    tab_ = juce::jlimit(0, 3, tab);
    (tab_ == 3 ? tabRoles_ : tab_ == 2 ? tabKeys_ : tab_ == 1 ? tabSamplers_ : tabGeneral_).setToggleState(true, juce::dontSendNotification);
    for (auto* c : getChildren())
      if (c != &tabGeneral_ && c != &tabSamplers_ && c != &tabKeys_ && c != &tabRoles_ && c != keysPage_.get() && c != samplersPage_.get() &&
          c != rolesPage_.get())
        c->setVisible(tab_ == 0);
    keysPage_->setVisible(tab_ == 2);
    samplersPage_->setVisible(tab_ == 1);
    rolesPage_->setVisible(tab_ == 3);
    if (tab_ == 1) samplersPage_->refresh();
    if (tab_ == 3) rolesPage_->refresh();
    repaint();
  }

  void resized() override {
    {
      auto tabs = getLocalBounds().removeFromTop(kTabs).reduced(16, 6);
      tabGeneral_.setBounds(tabs.removeFromLeft(110));
      tabs.removeFromLeft(6);
      tabSamplers_.setBounds(tabs.removeFromLeft(110));
      tabs.removeFromLeft(6);
      tabRoles_.setBounds(tabs.removeFromLeft(110));
      tabs.removeFromLeft(6);
      tabKeys_.setBounds(tabs.removeFromLeft(170));
    }
    if (rolesPage_ != nullptr) rolesPage_->setBounds(getLocalBounds().withTrimmedTop(kTabs + 1));
    if (keysPage_ != nullptr) keysPage_->setBounds(getLocalBounds().withTrimmedTop(kTabs + 1));
    if (samplersPage_ != nullptr) samplersPage_->setBounds(getLocalBounds().withTrimmedTop(kTabs + 1));
    auto r = getLocalBounds().withTrimmedTop(kTabs + 1).reduced(16, 12);
    languageTitle_.setBounds(r.removeFromTop(26));
    r.removeFromTop(4);
    {
      auto row = r.removeFromTop(28);
      language_.setBounds(row.removeFromLeft(220));
      row.removeFromLeft(12);
      languageNote_.setBounds(row);
    }
    r.removeFromTop(18);
    foldersTitle_.setBounds(r.removeFromTop(26));
    r.removeFromTop(4);
    for (auto* row : {&songs_, &exports_, &samples_, &trainingClips_}) {
      row->setBounds(r.removeFromTop(48));
      r.removeFromTop(6);
    }
    r.removeFromTop(12);
    pianoTitle_.setBounds(r.removeFromTop(26));
    r.removeFromTop(4);
    for (auto* b : {&ghost_, &guides_}) b->setBounds(r.removeFromTop(26).withWidth(420));
  }

 private:
  void folderButtons(FolderRow& row, const char* key, const juce::String& title) {
    row.choose_.onClick = [this, key, title] {
      const juce::File now = o_.folderSetting(key);
      chooseFolder(title, now.isDirectory() ? now : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), [this, key](const juce::File& f) {
        o_.settings_.getUserSettings()->setValue(key, f.getFullPathName());
        o_.settings_.saveIfNeeded();
      });
    };
    row.reset_.onClick = [this, key] {
      o_.settings_.getUserSettings()->removeValue(key);
      o_.settings_.saveIfNeeded();
      refresh();
    };
  }

  void chooseFolder(const juce::String& title, const juce::File& start, std::function<void(const juce::File&)> then) {
    chooser_ = std::make_unique<juce::FileChooser>(title, start);
    juce::Component::SafePointer<SettingsPanel> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe, then = std::move(then)](const juce::FileChooser& fc) {
                            if (safe == nullptr || fc.getResult() == juce::File()) return;
                            then(fc.getResult());
                            safe->refresh();
                          });
  }

  void refresh() {
    const juce::File songs = o_.folderSetting("folderSongs"), exports = o_.folderSetting("folderExports");
    songs_.show(songs != juce::File() ? songs.getFullPathName() : "Documents: " + o_.songsFolder().getFullPathName(), songs == juce::File());
    exports_.show(exports != juce::File() ? exports.getFullPathName() : juce::String("The song's own folder (Music before the first save)"), exports == juce::File());
    const juce::String chosenLibrary = o_.settings_.getUserSettings()->getValue("sampleLibrary");
    const juce::File library = o_.sampleLibraryFolder();
    samples_.show(library == juce::File() ? juce::String("None yet (D:\\Samples when it exists)") : library.getFullPathName(), chosenLibrary.isEmpty());
    trainingClips_.show(o_.trainingClipsFolder().getFullPathName(), o_.folderSetting("folderTrainingClips") == juce::File());
  }

  static constexpr int kTabs = 40;
  juce::TextButton tabGeneral_, tabSamplers_, tabRoles_, tabKeys_;
  std::unique_ptr<ShortcutsPage> keysPage_;
  std::unique_ptr<SamplersPage> samplersPage_;
  std::unique_ptr<RolesPage> rolesPage_;
  int tab_ = 0;
  MainComponent& o_;
  juce::Label languageTitle_, languageNote_, foldersTitle_, pianoTitle_;
  juce::ComboBox language_;
  juce::ToggleButton ghost_, guides_;
  FolderRow songs_{"Songs", "Where Save and Open start."},
      exports_{"Exports", "Where Export WAV starts."},
      samples_{"Sample library", "Scanned for the Library; Save as sample puts new samples in Core\\<role> inside it."},
      trainingClips_{"Training clips", "File > Make training clips writes here: one folder per batch, a WAV and its facts per clip."};
  std::unique_ptr<juce::FileChooser> chooser_;
};

void MainComponent::openSettings(bool shortcuts) {
  if (settingsWindow_ != nullptr) {
    if (auto* panel = dynamic_cast<SettingsPanel*>(settingsWindow_->getContentComponent())) panel->showTab(shortcuts ? 2 : 0);
    settingsWindow_->toFront(true);
    return;
  }
  juce::DialogWindow::LaunchOptions options;
  options.content.setOwned(new SettingsPanel(*this, shortcuts));
  options.dialogTitle = lang::tr("Settings");
  options.dialogBackgroundColour = theme::kPanel;
  options.escapeKeyTriggersCloseButton = true;
  options.useNativeTitleBar = true;
  options.resizable = false;
  options.componentToCentreAround = this;
  settingsWindow_ = options.launchAsync();
}

// Settings > Piano roll into the piano roll: the ghost note and the alignment guides (both on unless turned off).
void MainComponent::applyDrawingHelp() {
  editor_.setDrawingHelp(settings_.getUserSettings()->getBoolValue("pianoGhost", true), settings_.getUserSettings()->getBoolValue("pianoGuides", true));
}

// Settings > Samplers into the sample index: each role's folders, as folders of the library ('/' separated).
void MainComponent::applySamplerFolders() {
  sampleIndex_.roleFolders.clear();
  const juce::var all = juce::JSON::parse(settings_.getUserSettings()->getValue("samplerFolders", "{}"));
  if (!all.isObject() || sampleIndex_.root.empty()) return;
  for (const auto& role : doc::samplerRoles())
    if (const auto* list = all[juce::Identifier(role.c_str())].getArray())
      for (const auto& f : *list)
        if (const std::string rel = doc::libraryRelativeFolder(sampleIndex_, f.toString().toStdString()); !rel.empty())
          sampleIndex_.roleFolders[role].push_back(rel);
}

}  // namespace core::app
