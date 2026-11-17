// Core desktop app (M1a).
//   Core.exe                          opens the demo next to the executable (Ctrl+O opens a song)
//   Core.exe --backend null           no audio device (the engine still runs)
//   Core.exe --self-test picture.png  plays, changes the BPM, saves a picture of the window, exits 0/1
#include <fstream>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Autosave.h"  // currentProcessId(): the self-test folder is named by it (audit 28)
#include "Diagnostics.h"
#include "MainComponent.h"
#include "Theme.h"

namespace {

using core::app::MainComponent;

juce::String option(const juce::StringArray& args, const char* name) {
  const int i = args.indexOf(name);
  return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
}

// Holds the main component and draws it scaled (View > UI size): the layout works in logical pixels, so on a
// 2560 px window it is laid out for about 1700 px and drawn 1.5 times as large, as on the reference images.
class ScaledHolder final : public juce::Component {
public:
  explicit ScaledHolder(MainComponent* content) : content_(content) {
    addAndMakeVisible(*content_);
    setSize(content_->getWidth(), content_->getHeight());
  }
  void resized() override {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    const float scale = content_->uiScaleFor(getWidth(), (float)content_->chosenUiScale());
    content_->setTransform(juce::AffineTransform::scale(scale));
    content_->setBounds(0, 0, (int)std::ceil(getWidth() / scale), (int)std::ceil(getHeight() / scale));
  }

  MainComponent& content() const { return *content_; }

private:
  std::unique_ptr<MainComponent> content_;
};

// On Windows the window has no title bar: Core's top bar (logo, menus, transport, window buttons) is at the
// window's top edge and acts as the title bar (drag, double-click, snap layouts), so no height is lost to a
// strip that only says the name. The taskbar takes the exe's icons (core-icon-small.png for 16/32 px,
// core-icon.png above): one 256 px picture scaled to 16 px lost the C's gaps and the diamond.
class MainWindow final : public juce::DocumentWindow {
public:
  MainWindow(const juce::String& name, ScaledHolder* content)
      : DocumentWindow(name, core::app::theme::kBackground, DocumentWindow::allButtons), holder_(content) {
    setUsingNativeTitleBar(false);
    setTitleBarHeight(0);
    setContentOwned(content, true);
    setResizable(true, false);  // Windows resizes from the edges (the system's own border, no corner grip)
    // Never narrower than the menus and the transport: a 130 px window hid the whole menu bar (Diogo 2026-11-16).
    setResizeLimits(960, 560, 16384, 16384);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
  }
  void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
  // No border of Core's own: the content reaches the window's edges (Windows draws the outline and shadow).
  juce::BorderSize<int> getBorderThickness() const override { return {}; }
  juce::BorderSize<int> getContentComponentBorder() const override { return {}; }
  // The top bar's empty space moves the window and its buttons are the window's buttons. The edges stay
  // for resizing (JUCE answers the edges when this says "client").
  WindowControlKind findControlAtPoint(juce::Point<float> p) const override {
    if (!isFullScreen() && (p.x < kEdge || p.y < kEdge || p.x > getWidth() - kEdge)) return WindowControlKind::client;
    auto& content = holder_->content();
    return content.windowControlAt(content.getLocalPoint(this, p));
  }

private:
  static constexpr float kEdge = 5.0f;
  ScaledHolder* holder_;
};

class CoreApplication final : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
  const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String& commandLine) override {
    const juce::StringArray args = juce::StringArray::fromTokens(commandLine, true);
    core::app::startDiagnostics(commandLine);
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel_);  // every window and dialog in Core's colours
    const juce::File here = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    core::app::EngineSession::Options options;
    options.engineBinary = here.getChildFile("core-engine.exe").getFullPathName().toStdString();
    options.backend = option(args, "--backend").toStdString();
    options.engineLog = core::app::engineLogFile().getFullPathName().toWideCharPointer();
    options.dumpFolder = core::app::logsFolder().getFullPathName().toWideCharPointer();
    const juce::String selfTest = option(args, "--self-test").unquoted();
    if (selfTest.isNotEmpty()) options.backend = "null";  // repeatable on any machine, no sound

    core::doc::Document document;
    juce::String problem;
    const juce::File demo = here.getChildFile("demo.core-document.json");
    try {
      std::ifstream in(demo.getFullPathName().toStdString());
      if (!in) throw std::runtime_error("not found");
      document = core::doc::parseDocument(nlohmann::json::parse(in));
    } catch (const std::exception& e) {
      problem = juce::String::fromUTF8("Could not open the demo (") + demo.getFullPathName() + "): " + e.what();
    }
    auto* content = new MainComponent(options, document, problem);
    content_ = content;
    window_ = std::make_unique<MainWindow>(getApplicationName() + " - " + juce::String::fromUTF8(document.name.c_str()), new ScaledHolder(content));
    if (selfTest.isNotEmpty()) {
      const juce::File requested = juce::File::getCurrentWorkingDirectory().getChildFile(selfTest);
      // Audit 28: the self-test deletes and overwrites things NEXT TO its output path (<name>.core, <name>.wav,
      // <name>-audio/, <name>-saved/) — aimed at a real path, a typo (Core.exe --self-test MySong.core) could
      // destroy a user's song. Everything now lives in its own fresh temp folder; only the two asked-for
      // outputs, the picture and its .txt report, are copied out at the end.
      juce::File work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("core-selftest-" + juce::String(core::app::currentProcessId()));
      for (int n = 1; work.exists(); ++n) work = work.getSiblingFile(work.getFileNameWithoutExtension() + "-" + juce::String(n));
      work.createDirectory();
      const juce::File picture = work.getChildFile(requested.getFileName().isEmpty() ? juce::String("self-test.png") : requested.getFileName());
      content->runSelfTest(picture, [this, picture, requested, work](bool ok, juce::String report) {
        picture.copyFileTo(requested);
        requested.withFileExtension("txt").replaceWithText(report + "\n");
        work.deleteRecursively();
        setApplicationReturnValue(ok ? 0 : 1);
        quit();
      });
    }
  }

  void shutdown() override {
    content_ = nullptr;
    window_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);  // before lookAndFeel_ goes
    core::app::stopDiagnostics();
  }
  // Unsaved changes: the window asks first (save, don't save, cancel).
  void systemRequestedQuit() override {
    if (content_ == nullptr) return quit();
    content_->confirmClose([] { juce::JUCEApplication::quit(); });
  }

private:
  core::app::theme::LookAndFeel lookAndFeel_;  // first: it outlives every window
  std::unique_ptr<MainWindow> window_;
  MainComponent* content_ = nullptr;  // owned by the window
};

} // namespace

START_JUCE_APPLICATION(CoreApplication)
