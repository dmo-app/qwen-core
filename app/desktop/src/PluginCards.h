// The Tools panel (Diogo 2026-11-08, back 2026-11-16 with plugins only): a button on the page rail, beside the
// Library's, shows Core's tools as cards in the Library's place (doc::corePlugins()). A click opens the tool: the
// Patterns window, or the tool's menu (where it works, then it writes) on the selected tracks. The AI calls the
// same tools by the same ids. A card whose roles have no plugin yet is grey and says which to choose.
// The PLUGINS tab (Diogo 2026-11-17): the plugins installed on this PC as one card per maker (KiloHearts,
// GlitchMachines...); a click on a maker shows its plugins and what each can do (new track, replace, add as effect).
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Language.h"
#include "Theme.h"
#include "core_plugins.hpp"
#include "plugin_refs.hpp"
#include "roles.hpp"

namespace core::app {

class PluginCards final : public juce::Component {
public:
  std::function<void(const doc::CorePluginInfo&, juce::Component&)> onOpen;
  // A click on a grey card: no tool runs (round 3, R3-B: it used to open the tool anyway); the text says what is missing.
  std::function<void(const juce::String& missingRoles)> onGrey;
  // A click on a maker's card in the PLUGINS tab: its plugins, with their options.
  std::function<void(const doc::PluginVendorGroup&, juce::Component&)> onVendor;

  PluginCards() {
    for (const auto& p : doc::corePlugins()) {
      auto c = std::make_unique<Card>(*this, p);
      list_.addAndMakeVisible(*c);
      cards_.push_back(std::move(c));
    }
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    addAndMakeVisible(view_);
  }

  // Which roles have a plugin on this computer (Settings > Roles): a card none of whose roles has one is grey.
  void setRoles(const doc::RoleMap& map) {
    for (auto& c : cards_) c->setMissing(map);
    repaint();
  }

  // The installed plugins (instruments and effects together), grouped by maker for the PLUGINS tab.
  void setInstalled(const std::vector<doc::json>& plugins) {
    doc::json all = doc::json::array();
    for (const auto& p : plugins) all.push_back(p);
    vendorCards_.clear();  // before groups_ moves: the old cards point into it
    groups_ = doc::pluginsByVendor(all);
    for (const auto& g : groups_) {
      auto c = std::make_unique<VendorCard>(*this, g);
      list_.addChildComponent(*c);
      vendorCards_.push_back(std::move(c));
    }
    resized();
    repaint();
  }

  void paint(juce::Graphics& g) override {
    g.setColour(theme::kPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), theme::kCorner);
    auto head = getLocalBounds().removeFromTop(kHead);
    const int half = head.getWidth() / 2;
    for (int i = 0; i < 2; ++i) {
      auto tab = head.removeFromLeft(half);
      const bool active = (i == 0) == (mode_ == Mode::tools);
      g.setColour(active ? theme::kTitle : theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontBody, active ? juce::Font::bold : juce::Font::plain));
      g.drawText(lang::tr(i == 0 ? "TOOLS" : "PLUGINS"), tab.reduced(14, 0), juce::Justification::centredLeft, false);
      if (active) {
        g.setColour(theme::kAccent);
        g.fillRect(tab.removeFromBottom(2).reduced(14, 0));
      }
    }
  }
  void resized() override {
    view_.setBounds(getLocalBounds().withTrimmedTop(kHead).reduced(8, 0).withTrimmedBottom(8));
    const int w = view_.getWidth() - view_.getScrollBarThickness();
    int y = 0;
    if (mode_ == Mode::tools) {
      for (auto& c : cards_) { c->setVisible(true); c->setBounds(0, y, w, kCardH); y += kCardH + 6; }
      for (auto& c : vendorCards_) c->setVisible(false);
    } else {
      for (auto& c : cards_) c->setVisible(false);
      for (auto& c : vendorCards_) { c->setVisible(true); c->setBounds(0, y, w, kCardH); y += kCardH + 6; }
    }
    list_.setSize(w, y);
  }
  void mouseUp(const juce::MouseEvent& e) override {
    if (!e.mouseWasClicked() || e.position.y >= (float)kHead) return;
    const Mode next = e.position.x < (float)getWidth() / 2.0f ? Mode::tools : Mode::installed;
    if (next == mode_) return;
    mode_ = next;
    resized();
    repaint();
  }

private:
  static constexpr int kHead = 40, kCardH = 74;
  enum class Mode { tools, installed };
  class Card final : public juce::Component, public juce::SettableTooltipClient {
  public:
    Card(PluginCards& o, const doc::CorePluginInfo& p) : owner_(o), info_(p) {
      setMouseCursor(juce::MouseCursor::PointingHandCursor);
      setTooltip(juce::String::fromUTF8(p.about.c_str()));
    }
    void setMissing(const doc::RoleMap& map) {
      missing_.clear();
      bool any = info_.roles.empty();
      for (const auto& r : info_.roles) {
        if (map.find(r)) any = true;
        else missing_ += (missing_.isEmpty() ? juce::String() : juce::String(", ")) + juce::String::fromUTF8(r.c_str());
      }
      grey_ = !any;
      setTooltip(juce::String::fromUTF8(info_.about.c_str()) +
                 (missing_.isEmpty() ? juce::String() : juce::String("\n") + lang::tr("Settings > Roles: choose the plugin of " + missing_)));
    }
    void paint(juce::Graphics& g) override {
      const auto r = getLocalBounds().toFloat();
      g.setColour(isMouseOver() ? theme::kControlHover : theme::kCard);
      g.fillRoundedRectangle(r, theme::kCorner);
      g.setColour(grey_ ? theme::kDim : theme::kAccent);
      g.fillRoundedRectangle(r.withWidth(3.0f), 1.5f);
      auto b = getLocalBounds().reduced(12, 8);
      g.setColour(grey_ ? theme::kMuted : theme::kText);
      g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
      auto title = b.removeFromTop(20);
      g.drawText(juce::String::fromUTF8(info_.shortName.c_str()), title, juce::Justification::centredLeft, false);
      g.setColour(theme::kDim);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawText(juce::String::fromUTF8(info_.word.c_str()), title, juce::Justification::centredRight, false);
      g.setColour(theme::kMuted);
      const juce::String text = grey_ ? lang::tr("Settings > Roles: choose the plugin of " + missing_)
                                      : lang::tr(juce::String::fromUTF8(info_.about.c_str()));
      g.drawFittedText(text, b, juce::Justification::topLeft, 3, 0.9f);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override {
      if (!e.mouseWasClicked()) return;
      if (grey_) { if (owner_.onGrey) owner_.onGrey(missing_); return; }
      if (owner_.onOpen) owner_.onOpen(info_, *this);
    }

  private:
    PluginCards& owner_;
    const doc::CorePluginInfo& info_;
    juce::String missing_;
    bool grey_ = false;
  };
  // One maker's card in the PLUGINS tab: its name and how many instruments/effects it has; a click shows them.
  class VendorCard final : public juce::Component {
  public:
    VendorCard(PluginCards& o, const doc::PluginVendorGroup& g) : owner_(o), group_(g) {
      setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void paint(juce::Graphics& g) override {
      const auto r = getLocalBounds().toFloat();
      g.setColour(isMouseOver() ? theme::kControlHover : theme::kCard);
      g.fillRoundedRectangle(r, theme::kCorner);
      g.setColour(theme::kAccent);
      g.fillRoundedRectangle(r.withWidth(3.0f), 1.5f);
      auto b = getLocalBounds().reduced(12, 8);
      g.setColour(theme::kText);
      g.setFont(juce::FontOptions(theme::kFontTitle, juce::Font::bold));
      g.drawText(juce::String::fromUTF8(group_.vendor.c_str()), b.removeFromTop(20), juce::Justification::centredLeft, false);
      int instruments = 0;
      for (const auto& p : group_.plugins) instruments += p.value("instrument", false);
      const int effects = (int)group_.plugins.size() - instruments;
      g.setColour(theme::kMuted);
      g.setFont(juce::FontOptions(theme::kFontSmall));
      g.drawFittedText(juce::String(instruments) + " " + lang::tr("instruments") + ", " + juce::String(effects) + " " + lang::tr("effects"),
                       b, juce::Justification::topLeft, 2, 0.9f);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override {
      if (e.mouseWasClicked() && owner_.onVendor) owner_.onVendor(group_, *this);
    }

  private:
    PluginCards& owner_;
    const doc::PluginVendorGroup& group_;
  };
  juce::Viewport view_;
  juce::Component list_;
  std::vector<std::unique_ptr<Card>> cards_;
  std::vector<std::unique_ptr<VendorCard>> vendorCards_;
  std::vector<doc::PluginVendorGroup> groups_;
  Mode mode_ = Mode::tools;
};

}  // namespace core::app
