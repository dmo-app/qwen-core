// The main window (M1b): transport, BPM, timeline with seek and loop, tracks with volume / mute / solo
// and L/R meters, undo/redo, open/save, engine status.
// Only drawing and input here: the document lives in a History, the engine is reached through EngineSession.
#pragma once
#include <optional>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <set>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "EngineSession.h"
#include "GlobalKeys.h"
#include "SCorePanel.h"
#include "Mixer.h"
#include "Theme.h"
#include "InspectorPanel.h"
#include "LibraryPanel.h"
#include "PatternEditor.h"
#include "PatternsWindow.h"
#include "PluginCards.h"
#include "Plugins.h"
#include "Waveforms.h"
#include "edit.hpp"
#include "history.hpp"
#include "templates.hpp"
#include "template_sheet.hpp"
#include "randomize.hpp"
#include "samples.hpp"
#include "Autosave.h"
#include "autosave.hpp"

#include "bridge.hpp"
#include "calls.hpp"
class ProposalsPanel;

namespace core::app {

class MainComponent final : public juce::Component, public juce::MenuBarModel, private PatternEditor::Host,
                            public juce::DragAndDropContainer, public juce::FileDragAndDropTarget,
                            private juce::MidiInputCallback, private juce::Timer {
public:
  MainComponent(EngineSession::Options options, doc::Document document, juce::String loadProblem);
  ~MainComponent() override;
  // How big the window draws this component: View > UI size (0 = Auto) for a window this wide.
  static float uiScaleFor(int windowWidth, float chosen);
  double chosenUiScale() const;  // the View > UI size setting (0 = Auto)

  void paint(juce::Graphics& g) override;
  void resized() override;
  bool keyPressed(const juce::KeyPress& key) override;
  bool keyStateChanged(bool isKeyDown) override;  // releases the piano keys let go
  void focusLost(FocusChangeType) override;
  void mouseMove(const juce::MouseEvent& e) override;  // the divider between the tracks and the editor
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  // Over the ruler or the clip lanes: Ctrl+wheel zooms around the mouse, the wheel moves along the song.
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

  // Before closing: offers to save unsaved changes, then calls `close` (or not, if cancelled).
  void confirmClose(std::function<void()> close);

  // WAV files from the Explorer: on a sampler or drum track, its sound; elsewhere, imported as audio clips
  // (on an audio track at the drop point, or on a new audio track).
  bool isInterestedInFileDrag(const juce::StringArray& files) override;
  void fileDragMove(const juce::StringArray& files, int x, int y) override;
  void fileDragExit(const juce::StringArray& files) override;
  void filesDropped(const juce::StringArray& files, int x, int y) override;
  juce::String windowTitle() const;
  static juce::String buildText();  // "0.6.0+abc123", with "TEST BUILD (folder)" outside D:\Core versions\Core
  // The window has no title bar of its own (Windows): the top bar is its title bar. What is at a point, in
  // this component's coordinates: a window button, the top bar's empty space (drags the window, a
  // double-click maximises it), or a control.
  juce::Component::WindowControlKind windowControlAt(juce::Point<float> p) const;

  // Menu bar: File (open, save, quit) and Edit (undo, redo), away from the editing controls.
  juce::StringArray getMenuBarNames() override;
  juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
  void menuItemSelected(int id, int index) override;

  // Drives the window by itself (null device), saves a picture of it and reports the result.
  void runSelfTest(juce::File picture, std::function<void(bool ok, juce::String report)> done);

private:
  class TrackRow;
  class AudioEditor;  // the bottom panel for the selected audio clip (in place of the pattern editor)
  void showEditorForSelection();
  void openScore();  // Stage Core: the song's parts
  // The window that shows a part of Core or an "open" call: "score", "mixer", "patterns", "instrument" (the
  // track's plugin window). trackId "" = the master / the song.
  void openCoreWindow(const std::string& what, const std::string& trackId);
  bool scoreOpen_ = false;
  class AutomationEditor;  // one parameter's curve along the song, in the bottom panel, under the lanes
  void openAutomation(const std::string& trackId, const std::string& param);
  void closePanels();  // effects / automation closed: the pattern (or audio) editor again
  // Automation opened from a track's effects stays under their cards (Diogo 2026-11-07: "expandir o automation ao lado
  // dos filters, assim podemos mexer em vários sem ter de voltar para trás"); its close button closes only the curve.
  void closeAutomation();
  void placeBottomPanels();  // the effects and the automation share the bottom panel when both are open
  bool automationOpen_ = false;

  // PatternEditor::Host
  const doc::Document& currentDocument() const override { return history_.current(); }
  void applyEdit(const std::function<void(doc::Document&)>& change, const juce::String& label) override { edit(change, label); }
  void startGesture(const juce::String& label) override { beginGesture(label); }
  void finishGesture() override { endGesture(); }
  double playheadTick() const override { return playTick_; }
  void movePlayhead(int64_t songTick) override { moveShownPosition(songTick); }
  void capturePluginStates() override { fetchPluginStates(); }  // the mixer's Remove asks like the menus do
  void selectTrack(const std::string& trackId);
  // Several tracks at once (a rule for every plugin, Diogo 2026-11-05): a plain click on a track header selects that
  // track alone; Ctrl+click adds or takes away one more; Shift+click adds every track between the shown one and it.
  // selectedTracks() is the shown track first, then the others in song order (tracks that went are left out).
  std::vector<std::string> selectedTracks() const override;
  void selectOnlyTrack(const std::string& trackId);
  void toggleTrackSelected(const std::string& trackId);
  void selectTracksTo(const std::string& trackId);
  bool trackSelected(const std::string& trackId) const;
  // Live notes: the computer keyboard (Z..M row as piano keys, arrows change the octave) and a MIDI
  // keyboard play the track shown in the pattern editor.
  bool pressPianoKey(int keyCode);
  void releasePianoKey(int keyCode);
  void releaseAllKeys();
  void openMidiInput(const juce::String& identifier);
  void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
  // Arrangement (clip lanes and sections), used by the mouse and the self-test.
  void selectClip(const std::string& trackId, const std::string& clipId);
  void addClipAt(const std::string& trackId, int64_t tick);
  void showClipMenu(const std::string& trackId, const std::string& clipId, int64_t atTick = -1);  // atTick: where it was clicked
  void showSectionMenu(int64_t tick);
  // Audio (M3): WAV files copied into the project's Audio folder, placed as clips on audio tracks.
  void importAudio();
  // At the playhead's bar on the selected audio track (or a new one), or on `onTrack` at `at`; `end` gets the clip's end.
  bool importAudioFile(const juce::File& wav, juce::String* problem = nullptr, const std::string& onTrack = {}, int64_t at = -1, int64_t* end = nullptr);
  void importFiles(std::vector<juce::File> wavs, const std::string& onTrack, int64_t at);  // dropped files, one undo step each
  // Drag and drop onto the tracks: a Library sound (by sha256), a clip from another track. The row under the
  // mouse is lit while something can go there.
  static bool takesSamples(const doc::Track& t);
  void dropLibrarySample(const std::string& trackId, const std::string& sha256, int64_t at);
  void useSampleFile(const std::string& trackId, const juce::File& wav);
  TrackRow* rowAtScreen(juce::Point<int> screen) const;
  void setDropRow(const std::string& trackId);
  std::string dropRow_;
  void showAudioClipMenu(const std::string& trackId, const std::string& clipId, int64_t tick);
  void checkMissingAssets();   // after open, import, save as: the list and a notice
  void relocateMissing();      // asks for each missing file in turn
  void projectFolderChanged(); // the engine reads the audio files from the new folder
  std::filesystem::path projectDir() const;
  void askText(const juce::String& title, const juce::String& initial, std::function<void(const std::string&)> done);
  juce::Rectangle<int> laneSpan() const;  // x range of the clip lanes and the ruler
  juce::Rectangle<int> editorArea() const;

  void timerCallback() override;
  void selfTestStep();
  bool savePicture(const juce::File& file);  // the window, as a PNG (self-test)

  // The AI's proposals (2026-11-05): the AI lives in AICORE. Core writes its functions and the song to the bridge
  // folder and shows the proposals of calls AICORE leaves there (app/control: calls.hpp, bridge.hpp); Apply makes
  // them as one "AI: ..." undo step. AI menu: proposals, folder, on/off.
  void setAiControl(bool on);
  juce::File bridgeFolder() const;
  std::vector<control::CoreFunction> callableFunctions() const;
  void pollBridge();          // the timer: the state written when it changed, the inbox read when it changed
  void showProposals();       // the AI button
  void applyProposal(int index, juce::Component* button);
  void finishProposal(int index, const std::string& status, const json& result);
  int indexOf(const std::string& proposalId) const;
  void aiBadgePressed(bool popup);
  void updateAiBadge();
  // AICORE, Core's add-on (2026-11-06): the AICORE button opens its chat; the dot is green while that chat is open
  // and connected to an AI model (aicore-status.json in the bridge folder), red otherwise.
  std::vector<std::filesystem::path> aicoreFolders();  // where AICORE may be: Settings, beside Core, D:
  void openAicore();
  void chooseAicoreFolder();
  void showAicoreMenu(juce::Component* target);

  // Document changes go through the history, then to the engine.
  void edit(const std::function<void(doc::Document&)>& change, const juce::String& label);
  void beginGesture(const juce::String& label);
  void endGesture();
  void documentChanged(bool rebuildRows);  // after undo/redo/open: controls follow the document
  void undo();
  void redo();

  void open();
  void newProject(bool fromTemplate);
  // Song templates (the Library's Templates tab, by style): built-in ones next to Core.exe, the user's own in %APPDATA%\Core\Templates.
  void newFromDocument(doc::Document d, std::function<void()> then = {});  // then: once the song is shown
  void newFromTemplate(const std::string& templateId);
  void saveAsTemplate();
  void deleteTemplate(const std::string& templateId);
  void restoreBuiltinTemplates();
  void showTemplates();  // the Library opens on its Templates tab
  void reloadTemplates();
  juce::File builtinTemplatesFolder() const;
  juce::File userTemplatesFolder() const;
  doc::TemplateLibrary templates_;
  // Sample library (D:\Samples by default): what is in it, scanned on a thread of its own at start and on
  // request; style templates get its best samples per track (one undo step), and each track can swap its
  // sample from the track menu.
  juce::File sampleLibraryFolder() const;
  juce::File sampleIndexFile() const;
  void loadSampleIndex();       // the index saved by the last scan
  void scanSampleLibrary();     // in the background; collectSampleScan() takes the result
  void collectSampleScan();
  void removeUnusedAudio();     // Library > Project > Remove unused
  void saveSampleIndex();       // atomically (a temporary file, then replaced)
  void sampleCatalogChanged();  // a catalogue edit or undo: saved, and the Library shows it
  void chooseSampleLibrary();
  void useSuggestedSamples(bool fromTemplate, bool shuffle);
  void randomizeSong(int strength, const std::string& axis = {});  // axis: "tempo" (J) or "sound" (K) alone
  void putSampleOnTrack(const std::string& trackId, const doc::LibrarySample& sample);
  void chooseSampleFile(const std::string& trackId);
  void auditionTrack(const std::string& trackId);
  doc::SampleIndex sampleIndex_;
  bool hasSampleIndex_ = false;
  struct SampleScan {
    std::atomic<int> state{0};  // 0 running, 1 done, 2 failed
    std::atomic<int> done{0}, total{0};
    std::atomic<bool> stop{false};
    doc::SampleIndex index;
    std::string error;
  };
  std::shared_ptr<SampleScan> sampleScan_;
  // Measuring the sounds of the library (plan A1), one file at a time on a thread of its own, paused while the
  // song plays; results are taken on the window's thread by content (a file changed meanwhile is not touched).
  struct SampleAnalysisJob {
    std::atomic<bool> stop{false}, pause{false}, finished{false};
    std::atomic<int> done{0}, total{0};
    std::mutex lock;
    std::vector<std::pair<std::string, nlohmann::json>> results;  // sha256, analysis
  };
  std::shared_ptr<SampleAnalysisJob> analysisJob_;
  int analysedSinceSave_ = 0;
  void startSampleAnalysis();
  void collectSampleAnalysis();
  juce::String templatesStamp_;
  bool templatesLoaded_ = false;
  void showTrackMenu(const std::string& trackId);  // right click on a track
  void showAddTrackMenu();
  // The Patterns window (P-Core's channel rack), a window of its own beside this one (Diogo, 2026-11-08). While it
  // plays a pattern, the engine gets that pattern alone, looped (doc::rackPlayDocument); stopping gives the song back.
  void giveSilentTracksPlugins();  // Plugins menu: old (builtin) and empty tracks get their role's plugin
  // Core's tools (doc::corePlugins: the Tools panel, the Plugins menu, a track's right-click "Tools"): the same
  // functions the AI calls (role_tools.hpp). coreToolMenu: one tool's menu (where it works, then it writes) for the
  // track (""= the selected one); coreToolsMenu: every tool.
  juce::PopupMenu coreToolMenu(const std::string& toolId, const std::string& trackId);
  juce::PopupMenu coreToolsMenu(const std::string& trackId);
  void openCorePlugin(const doc::CorePluginInfo& info, juce::Component* near);
  // The PLUGINS tab of the Tools panel (Diogo 2026-11-17): a maker's card opens its plugins' options; the F-Core
  // intents can also be applied over chosen bars, on this track, the selected ones or all that play notes.
  void showVendorPlugins(const doc::PluginVendorGroup& group, juce::Component* near);
  void askIntentZone(const std::string& intent, const std::string& trackId);
  void refreshInstalledCards();
  void runTool(const juce::String& label, std::function<std::string(doc::Document&, const doc::RoleMap&)> tool);
  std::string toolTrack() const;  // the track the tools work on: the selected one, else the piano roll's
  void togglePlugins();
  bool pluginsShown() const;
  bool sideShown() const { return libraryShown() || pluginsShown(); }
  PluginCards pluginCards_;
  bool showPlugins_ = false;
  void openPatterns();
  std::unique_ptr<PatternsWindow> patternsWindow_;
  struct PatternPlay { std::string slot; std::optional<doc::Document> preview; int64_t openTick = 0; };
  std::optional<PatternPlay> patternPlay_;
  int64_t patternSongTick_ = 0;  // where the song was when the pattern started playing
  void playPatternSlot(const std::string& slot, std::optional<doc::Document> preview, int64_t openTick);
  void stopPatternSlot();
  doc::Loop engineLoop() const;  // the song's loop, or the pattern's while the Patterns window plays
  void askTrackName(const std::string& trackId);
  void exportWav(int bits, bool mono = false);
  void exportMidi();
  void exportBitwig();  // the .mid and a .vstpreset per plugin, for the Bitwig
  void importMidi();
  // Plugins (VST3): what this PC has (Plugins menu > Scan), their windows and their states in the song.
  std::unique_ptr<PluginCatalog> plugins_;
  uint64_t pluginEditsSeen_ = 0;
  void scanPlugins();
  bool fetchPluginStates();  // the engine's plugin states into the document (false: no answer)
  void selectDevice(const std::string& name);
  // SavedWithoutPlugins: the song is in the file but the plugins' current settings are not (the engine gave no
  // answer): the song stays unsaved and Close does not go on (audit R01).
  enum class SaveResult { Failed, Saved, SavedWithoutPlugins };
  void save(bool chooseName, std::function<void(SaveResult)> then = nullptr);
  SaveResult writeTo(const juce::File& target);  // shows the error itself
  std::string lastSampleTrack_;  // the library's last try on a track (the next try there replaces its undo step)
  juce::String lastSampleLabel_;
  void confirmUnsaved(std::function<void()> next);
  // After Save in "Unsaved changes": goes on only when the file was written.
  void continueAfterSave(SaveResult result, std::function<void()> next);
  void showAbout();
  void showError(const juce::String& title, const juce::String& message);

  void buildRows();
  juce::Rectangle<int> workArea() const;  // between the menu bar and the footer
  juce::Rectangle<int> topBar() const;    // the top row: logo, menus, transport, project, Listen, window buttons
  juce::Rectangle<int> content() const;   // under the top bar, with margins
  juce::Rectangle<int> body() const;      // under the top bar, right of the page rail
  void toggleSidePanel(bool library);     // View > Library / Inspector, and the rail's buttons
  // Under the top bar, between the Library (left) and the Inspector (right): ruler, tracks, editor, master.
  juce::Rectangle<int> centre() const;
  juce::Rectangle<int> libraryArea() const;
  juce::Rectangle<int> inspectorArea() const;
  bool inspectorShown() const;
  int inspectorWidth() const;
  bool libraryShown() const;
  void setupLibrary();
  void showProjectMenu();
  // Plays notes of a track one after the other, spaced as in the pattern (the Inspector's preview).
  void previewNotes(const std::string& trackId, const std::vector<doc::Note>& notes);
  // Plays a library sample once on a hidden track (doc::withPreviewTrack) that only the engine gets: the song and
  // its undo do not change. The AI's preview_sample. False when it cannot (no library, the track was refused).
  bool previewLibrarySample(const doc::LibrarySample& sample, const std::string& role);
  uint64_t samplePreviewGen_ = 0;  // the newest library preview: older ones leave it alone
  std::string samplePreviewTrack_;  // its hidden track while it plays ("" = none)
  int samplePreviewPitch_ = 60;
  // The pattern library (Library > Patterns, piano roll Tools): Core's beside Core.exe, Diogo's in %APPDATA%\Core\Patterns.
  std::vector<doc::PatternPreset> loadPatterns() const;
  // Plays a pattern's notes on a track, as far apart as at the song's tempo (up to 16 bars); the song does not change.
  // A new preview, or stopPatternPreview, ends the one playing.
  void previewPattern(const std::string& trackId, const std::vector<doc::Note>& notes);
  void stopPatternPreview();
  uint64_t patternPreviewGen_ = 0;
  std::string patternPreviewTrack_;
  std::set<int> patternPreviewPitches_;
  void savePatternFromEditor(int64_t start, int64_t end);
  void buildupTrack(int bars, bool roll, bool gap);  // a sampler track with a snare of the library, ending at the next section
  juce::Rectangle<int> timelineArea() const;
  juce::Rectangle<int> tracksArea() const;
  int masterRow() const { return mixerOpen_ ? 30 : 64; }
  void paintCpuFooter(juce::Graphics& g, juce::Rectangle<int> r) const;
  int64_t tickAt(int x, int64_t snap, bool pastEnd = false) const;  // pastEnd: up to the room after the song's end
  double viewSpan() const;
  int editorHeight() const;
  juce::Rectangle<int> dividerArea() const;
  int editorHeight_ = 0, mixerHeight_ = 0, dividerGrab_ = -1;  // 0: the default share
  bool editorCollapsed_ = false;
  int libraryWidth_ = 0, sideGrab_ = 0;  // the Library's width (0: the default); the edge being dragged (1 Library)
  int libraryWidth() const;
  juce::Rectangle<int> libraryEdge() const;
  int splitRows_ = 0;  // the most track rows seen: the default share grows with them and never shrinks when one is removed
  void growSongToFit(const std::string& trackId, const std::string& clipId);
  // The finest of 1/64, 1/32, 1/16, beat, bar whose steps are at least this many pixels apart at the current zoom
  // (a bar when none is).
  int64_t gridStep(int minPixels) const;
  int xOfTick(double tick) const;
  // The part of the song shown in the ruler and the lanes (UI state, never saved).
  double viewStart() const;
  double viewLength() const;
  void setView(double start, double length);
  double viewStart_ = 0, viewLength_ = 0;  // 0: the whole song
  // Grab the song and drag it sideways (the tracks follow up and down): the middle mouse button, or the pan key
  // (Alt) held with the left button.
  bool panKeyHeld(const juce::ModifierKeys& mods) const;
  bool loopKeyHeld() const;
  bool startPan(const juce::MouseEvent& e);  // true: this press is a pan (the caller does nothing else with it)
  void dragPan(const juce::MouseEvent& e);
  void endPan();
  bool panning_ = false;
  juce::Point<int> panFrom_;  // screen
  double panStart_ = 0;
  int panScrollY_ = 0;
  int playheadX_ = -1;  // where the lanes last drew the playhead (they repaint only that strip and the new one)
  double lastPlayTick_ = -1;                // to turn the page once, when the playhead leaves the view

  doc::History history_;
  std::optional<doc::Document> tempoBase_;  // the song when a BPM drag began (doc::setTempo from it)
  std::unique_ptr<EngineSession> session_;
  SessionSnapshot snap_;
  juce::String loadProblem_, notice_;
  juce::File file_;
  bool imported_ = true;  // the demo: "Save" asks for a name
  std::unique_ptr<juce::FileChooser> chooser_;

  // The menu bar, with a short help on hover for each menu (what is in it).
  struct MenuBar final : juce::MenuBarComponent, juce::TooltipClient {
    using juce::MenuBarComponent::MenuBarComponent;
    juce::String getTooltip() override;
  };
  MenuBar menuBar_{this};
  juce::String menuState_;  // what the menus show; refreshed when it changes
  juce::TextButton home_, play_, stop_, loop_, restart_, undo_, redo_, addTrack_;
  theme::BarSlider bpm_;     // drag to change, double-click to type
  theme::BarSlider listen_;  // the listening level (EngineSession::setListenLevel), saved in the user settings
  juce::Label bpmLabel_, position_, status_, message_;
  // Minimise, maximise and close, at the top bar's right end (Windows draws no title bar for Core).
  class WindowButton;
  std::unique_ptr<WindowButton> minimise_, maximise_, close_;
  juce::Label build_;  // in the footer: the build (and "TEST BUILD" outside D:\Core versions\Core)
  juce::TextButton revertAccepted_;
  int topGap_ = 24;                           // the top bar's gaps around the tempo (narrower on a narrow window)
  double exportProgress_ = 0;
  juce::ProgressBar exportBar_{exportProgress_};
  juce::TextButton cancelExport_;
  std::string jobSeen_;  // last export state shown, to announce the end once
  juce::ApplicationProperties settings_;  // user preferences (output device)
  std::vector<std::unique_ptr<TrackRow>> rows_;
  // The tracks scroll inside their panel (the window keeps its size, the transport and ruler stay put).
  class TracksView;
  std::unique_ptr<TracksView> tracksView_;
  class TimeBar;
  std::unique_ptr<TimeBar> timeBar_;  // under the lanes when zoomed in: drag it along the song
  void updateTimeBar();
  juce::Component tracksHolder_;
  PatternEditor editor_{*this};
  std::unique_ptr<AudioEditor> audioEditor_;
  SCorePanel score_{*this};
  std::unique_ptr<MixerPanel> mixer_;  // the Mixer in the bottom panel (top bar: Mixer, Ctrl+M)
  bool mixerOpen_ = false;
  juce::TextButton arrangeButton_;
  std::vector<std::unique_ptr<juce::TextButton>> rail_;  // the page rail on the left: Arrange, Mix, Library, Inspector  // the page tabs on the top bar: Arrange (no page open), Mix
  juce::TextButton mixerButton_;
  void setMixerOpen(bool open);
  std::unique_ptr<AutomationEditor> automation_;
  LibraryPanel library_;
  std::unique_ptr<InspectorPanel> inspector_;
  bool showLibrary_ = true, showInspector_ = true;  // View menu (UI state, kept in the user settings)
  bool sectionShades_ = true;  // View > Section colours behind the tracks (kept in the user settings)
  std::string libraryTrackSeen_ = "-";             // the Library's "use on track" follows the selected track
  juce::TextButton project_;                        // the song's name in the top bar: the File menu
  const void* libraryIndexData_ = nullptr;          // the index the Library last showed (it rebuilds when it changes)
  size_t libraryIndexSize_ = (size_t)-1;
  std::unique_ptr<GlobalKeys> globalKeys_;  // Space plays / stops in every window, whatever has the focus
  void playStop();
  EngineStatus loggedStatus_ = EngineStatus::Stopped;  // the last engine status written to the log
  std::string failedSeen_;  // the failed units last shown (timerCallback)
  bool unsaved() const { return history_.modified() || recovered_ || pluginsDirty_; }
  bool pluginsDirty_ = false;  // a plugin's own window changed it since the last save
  // Another song is on screen (open, new, recovered): nothing of the one before may follow it - not the
  // plugins' unsaved state, not this session's autosave (audit 2026-11-17, items 7 and 8).
  void documentLoaded();
  // Autosave (M6): every minute while there are unsaved changes, a copy goes to the autosave folder (never
  // over the song); saved, or closed cleanly, it is removed. One left by a crash is offered on the next start.
  void autosaveTick();
  void startAutosave(doc::Document document);  // writes this copy on a thread, when it is not on disk yet
  bool autosaveAsking_ = false;  // the plugins' settings were asked for the next copy
  void removeOwnAutosave();
  void offerRecovery();
  void recoverAutosave(const doc::AutosaveInfo& autosave, const doc::Document& recovered);  // opens it, unsaved
  bool recovered_ = false;            // the song came back from an autosave: unsaved until saved
  const std::string autosaveSession_ = juce::Uuid().toString().toStdString();
  double lastAutosaveCheck_ = 0;
  // What is on disk and whether a write is due (a failed write is tried again every minute).
  doc::AutosaveTracker autosave_;
  // The write runs on a thread of its own and leaves its result here; the window collects it.
  struct AutosaveJob { std::atomic<int> state{0}; std::string error; };  // 0 running, 1 written, 2 failed
  std::shared_ptr<AutosaveJob> autosaveJob_;
  void collectAutosave();
  double audioEditorTick_ = -1;  // playhead last drawn there
  std::string selectedTrack_, selectedClip_;  // UI state: never saved (a pattern clip or an audio clip)
  std::optional<doc::PCoreCatalog> pcoreCatalog_;  // Pattern Core (P-Core)' catalogue (pcore/styles.json), read once
  bool pcoreTried_ = false;
  std::set<std::string> alsoSelected_;         // the other selected tracks (Ctrl/Shift+click); UI state, never saved
  // The selection box (Diogo 2026-11-07): a drag on an empty part of the lanes, or under the last track, draws a
  // box; the tracks it crosses are selected and the clips it touches become the box's clips (Delete removes them
  // all, dragging one moves them all, one Ctrl+Z). Screen coordinates; UI state, never saved.
  using ClipKey = std::pair<std::string, std::string>;  // (track, clip)
  std::set<ClipKey> boxClips_;
  std::map<ClipKey, int64_t> boxStarts_;  // where the box's clips were when a group move began
  bool boxing_ = false, boxAdd_ = false;
  juce::Point<int> boxFrom_, boxTo_;
  class BoxOverlay;
  std::unique_ptr<BoxOverlay> boxOverlay_;
  void startBox(const juce::MouseEvent& e);
  void dragBox(const juce::MouseEvent& e);
  void endBox(const juce::MouseEvent& e);
  bool clipBoxed(const std::string& trackId, const std::string& clipId) const { return boxClips_.count({trackId, clipId}) > 0; }
  void beginBoxMove();
  void moveBox(int64_t delta);
  void endBoxMove();
  void deleteBoxClips();
  // Clips as in Ableton (Diogo 2026-11-08): Ctrl+C copies the selected clips (the box's, or the one clicked), Ctrl+X
  // cuts them, Ctrl+V puts them back on their tracks at the playhead, Ctrl+D right after the selection. The copies
  // are kept whole, so a cut can be pasted. UI state, never saved.
  struct CopiedClip { std::string trackId; bool audio = false; doc::Clip clip; doc::AudioClip audioClip; };
  std::vector<CopiedClip> copiedClips_;
  int64_t copiedFrom_ = 0, copiedTo_ = 0;  // where the copied clips started and ended in the song
  std::vector<ClipKey> selectedClipKeys() const;
  bool copyClips();               // false: nothing selected
  void pasteClips(int64_t at);    // the copies' first clip at `at`, the others at the same distances
  void cutClips();
  void duplicateClips();
  std::vector<std::string> missing_;          // asset ids whose file is not in the project folder
  WaveformCache waves_{[this] { repaint(); }};  // the clip lanes and the audio editor
  PatternWaves patternWaves_{[this] { repaint(); }};  // what each pattern sounds like, drawn inside its clips
  std::map<int, std::pair<std::string, int>> heldKeys_;  // key code -> (track, pitch) sounding now
  int keyboardOctave_ = 2;                                // the Z key plays C of this octave (C2 = 36)
  bool realOutput_ = true;
  std::unique_ptr<juce::MidiInput> midiInput_;
  juce::String midiInputId_;
  std::mutex liveMutex_;                                  // the MIDI thread reads these two
  std::string liveTrack_;
  std::vector<std::pair<std::string, int>> midiHeld_;     // notes a MIDI keyboard holds, oldest first
  static constexpr int kTrackHead = 6 + 8 + 130 + 26 + 26 + 6 + 110 + 6 + 56 + 10;  // track header width

  float master_[2] = {0, 0};
  std::map<std::string, std::pair<float, float>> meters_;
  bool cpuAsked_ = false;     // an engine.cpu answer is on its way (the Mixer asks once a second while it is open)
  juce::uint32 cpuAskedAt_ = 0;
  // The engine's CPU for the Mixer's footer (ref-3): each track's share by name, and the whole of it.
  std::vector<std::pair<std::string, float>> cpuTracks_;
  float cpuTotal_ = 0;
  int64_t dragStartTick_ = -1, dragTick_ = -1;  // timeline drag = new loop region
  // The playhead the user just moved (a click, or a drag of its handle) shows at once, before the engine reports it.
  bool scrubbing_ = false;
  bool rulerScrub_ = false;  // a plain drag on the ruler moves the position (its hint not shown yet)
  bool loopEdge_ = false;  // the drag moves one end of the loop region
  int loopEdgeAt(juce::Point<int> p) const;
  int64_t loopSnap() const;
  bool sendPending_ = false;  // a drag changed the song; the timer sends it to the engine
  double shownTick_ = -1;
  juce::uint32 shownUntil_ = 0;
  double shownPosition() const { return shownTick_ >= 0 ? shownTick_ : playTick_; }
  // The engine's position goes on between its reports (about 21 a second): the playhead moves every frame.
  double playTick_ = 0;
  void updatePlayTick();
  void moveShownPosition(int64_t tick);

  struct SelfTest {
    juce::File picture;
    std::function<void(bool, juce::String)> done;
    int step = 0;
    bool keymapChecked = false;
    double started = 0, stepStarted = 0;
    double midiStarted = 0;
    float loudest = 0, limited = 0;
    bool trackMeters = false;
    double maxTickInLoop = 0, tickBeforeCrash = 0;
    std::string audioTrack, partTrack;
    double duration = 0;
    juce::String log;
    nlohmann::json synth;  // the self-test's own test synth (app/selftest-plugins), as the scan found it
  };
  std::unique_ptr<SelfTest> test_;
  void selfTestSynth();  // the self-test's synth on every note track without a plugin
  std::vector<control::InboxItem> inbox_;           // the AI's proposals waiting (bridge inbox)
  std::string samplesAddedSeen_ = "?";               // AICORE's samples-added.json as last seen ("?" = not looked yet)
  std::string inboxSeen_, stateWritten_;            // the inbox as last read, the state as last written
  juce::uint32 bridgePolled_ = 0;
  juce::Component::SafePointer<ProposalsPanel> proposals_;  // shown (in a callout) from the AI button
  class AiBadge;
  std::unique_ptr<AiBadge> aiBadge_;                // the footer's "AICORE" and its dot: green connected, red not
  control::AicoreStatus aicore_;                    // what AICORE said last (read every second)
  std::unique_ptr<juce::ChildProcess> aicoreProcess_;  // the AICORE Core started (left running when Core closes)
  juce::TooltipWindow tooltips_{this, 450};  // a short help on hover for every control
  bool aiAllowed_ = true;                           // AICORE > Allow AICORE proposals: gates the bridge and Apply

  doc::Document engineDocument() const;  // what the engine plays: the song
  // File > Settings (Settings.cpp): the default folders, the samplers' folders, the keyboard shortcuts.
  class SettingsPanel;
  juce::Component::SafePointer<juce::DialogWindow> settingsWindow_;
  juce::File trainingClipsFolder() const;  // Settings > Training clips (kept for AICORE), else Documents\Core training clips
  void openSettings(bool shortcuts = false);  // shortcuts: the Keyboard shortcuts tab
  void applyDrawingHelp();  // Settings > Piano roll (ghost note, alignment guides) into the editor
  // Settings > Samplers: each sampler role's own folders ("samplerFolders": role -> absolute folders) into the
  // sample index (those inside the library), so its suggestions come only from them.
  void applySamplerFolders();
  juce::File folderSetting(const char* key) const;  // a chosen folder ("" -> juce::File())
  juce::File songsFolder() const;          // Save / Open: the chosen one, else Documents
  juce::File exportFolder() const;         // every export: the chosen one, else the song's folder, else Music
  void setSampleLibrary(const juce::File& folder);  // and scans it

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace core::app
