// Editor for the selected track's pattern (M2): a step grid for drums, a note editor for the bass.
// Every click is one history step; a drag (move, resize, velocity) is one gesture.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "document.hpp"
#include "Theme.h"
#include "pattern_tools.hpp"
#include "pcore.hpp"
#include "ToolDock.h"

namespace core::app {


class PatternEditor final : public juce::Component, public juce::SettableTooltipClient {
public:
  struct Host {
    virtual ~Host() = default;
    virtual const doc::Document& currentDocument() const = 0;
    virtual void applyEdit(const std::function<void(doc::Document&)>& change, const juce::String& label) = 0;
    virtual void startGesture(const juce::String& label) = 0;
    virtual void finishGesture() = 0;
    virtual double playheadTick() const = 0;
    // Moves the song's position (the start line): the piano roll's ruler, clicked or dragged.
    virtual void movePlayhead(int64_t /*songTick*/) {}
    // What a plugin holds in the engine right now goes back into the document, before a change that would lose
    // it (the same call the track's and the master's menus make before Remove). Panels without plugins: nothing.
    virtual void capturePluginStates() {}
    // The tracks a plugin acts on (Diogo, 2026-11-05: "every plugin applies to the selected tracks at once, then
    // each track is touched alone"): the track shown first, then the others Ctrl/Shift-clicked, in song order.
    virtual std::vector<std::string> selectedTracks() const = 0;
  };

  explicit PatternEditor(Host& host);
  ~PatternEditor() override;
  // The Sound card's «Open» (param empty), or a right-click on one of its bars (that control, kind 0 LFO, 1 envelope,
  // 2 steps): the instrument's page, with its sound and its modulation together.
  // "" = nothing selected. Without a pattern id, the track's first pattern.
  void setTrack(const std::string& trackId, const std::string& patternId = "");
  void setPattern(const std::string& patternId);
  const std::string& track() const { return trackId_; }
  const std::string& patternId() const { return patternId_; }
  void documentChanged();                     // after any edit, undo or open
  void playheadMoved();                       // every frame: redraws only where the playhead line was and is
  // The mouse's tool: draw (click adds a note), select (drag a box), erase (click or drag over notes).
  enum class Tool { Draw, Select, Erase };
  void setTool(Tool tool);
  Tool tool() const { return tool_; }
  // Drawing help (File > Settings > Piano roll, both on by default): a faint note where a click would put one,
  // and thin guides when that note (or a dragged one) lines up with another: same key across, same time down.
  void setDrawingHelp(bool ghost, bool guides);
  // Groove tools (Edit > Timing, Variation) on the notes of the bars shown, the selected ones among them; one undo step each.
  // Strength and swing (0..1) are set in the Inspector.
  double quantizeStrength = 1.0, swing = 0.0;
  // Edit > Play the selected notes: the notes on their track (they were in the Inspector's PREVIEW, Diogo 2026-11-09).
  std::function<void(const std::string& trackId, const std::vector<doc::Note>& notes)> onPreviewNotes;
  void quantize();
  void humanize();
  void vary(const std::string& kind);
  void stutter(const std::string& presetId);  // STUTTER note slice: the notes become repeats
  // FOLLOW (Tools > FOLLOW): 0 the kick and the bass follow the selected notes, 1 the kick follows the STUTTER of the
  // bars shown, 2 the bass follows the kick there, 3 this track's FOLLOW on/off (it then follows at every edit).
  void follow(int what);
  // SUCK / PUM / build-up effects through the role map (role_tools.hpp) on the current track. tool: "suck" (amount =
  // bars before the drop), "pump" / "pumpSong" (sourceIndex = the track the pump follows), "buildupFx" (amount 1..3:
  // filter+reverb, +delay, +drive). Pattern ticks [from, to) of the bars shown.
  void roleTool(const std::string& tool, int amount, int sourceIndex, int64_t from, int64_t to);
  // Edit > Notes and Edit > Randomize (see the .cpp): transpose, add notes above, length, velocity, shuffle, random pitches.
  void noteTool(const std::string& kind, int value);
  // The Tools menu (2026-11-05: create, not only trim). Each acts on the bars shown (the selected notes among them,
  // when some are selected) and is one undo step; random choices come from a seed that moves on at every click.
  // The bars shown: [start, end) in pattern ticks, whole bars, inside the pattern.
  std::pair<int64_t, int64_t> visibleBars() const;
  void normalize(const doc::NormalizeOptions& options, const juce::String& label);
  void density(const std::string& mode, double amount);  // percent / count / perBeat; how: densityHow
  std::string densityHow = "fill";                        // fill | ghost | ratchet | octave
  void buildup(int bars, bool roll, bool gap);            // ends where the shown bars end, on this track
  // A pattern of the library on this track: from the first bar shown, over `bars` bars (the pattern grows when it
  // is shorter, its notes repeating), moved to the track's key. add: keep the notes there. One undo step.
  void applyPreset(const doc::PatternPreset& preset, int bars, bool add);
  std::function<void(const juce::String&)> onMessage;          // what a tool did ("+10 notes: 38 -> 48")
  std::function<void(int64_t start, int64_t end)> onSavePattern;  // "Save shown bars as a pattern..."
  // "Build-up on a new track": the window makes a sampler track with a snare/clap of the library and places it.
  std::function<void(int bars, bool roll, bool gap)> onBuildupTrack;
  std::function<double()> bpm;                                 // the tempo where the song is (seconds -> bars)
  std::function<std::vector<doc::PatternPreset>()> patternLibrary;  // the library (PCore's templates)
  // Build-up → roll → drop (one-time use): the window over the bars shown. A take is one undo step; "again" puts back what the
  // last take changed (when nothing was edited since) and writes another. tempoTo > 0: the song's tempo glides from
  // the build-up (or roll) to the drop. Returns what happened.
  void showBuildup();
  juce::String buildupTake(const doc::PCoreSettings& settings, int64_t from, int64_t to, double tempoTo, bool again, int effects = 0);
  // The Patterns window (P-Core's channel rack, a window of its own): the menus' "Patterns…" open it.
  std::function<void()> onOpenPatterns;
  std::function<const doc::PCoreCatalog*()> pcoreCatalog;  // pcore/styles.json beside Core.exe (nullptr: missing)
  std::function<std::string()> songStyle;                  // the song's style name ("Hi-tech"), "" when none

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseExit(const juce::MouseEvent& e) override;
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
  bool keyPressed(const juce::KeyPress& key) override;

  // For tests and the self-test: the same operations the mouse does.
  void clickCell(int64_t tick, int pitch);
  const std::set<std::string>& selection() const { return selected_; }
  void selectAll();
  void revealSelection();  // scrolls the keys so the first selected note shows
  void duplicateSelection();
  void deleteSelection();
  // A point inside the note drawn at this tick and pitch, 2 px from its left edge (self-test mouse events).
  juce::Point<int> cellGrabPoint(int64_t tick, int pitch) const;

private:
  void stepMouseDown(const juce::MouseEvent& e, const doc::Note* hit);  // drums
  const doc::Track* currentTrack() const;
  const doc::Pattern* pattern() const;  // the pattern being edited (non-null when a track is)
  bool drums() const;
  std::vector<int> rows() const;  // pitches, top to bottom
  float rowHeight() const;
  // Drum tracks: the hat sounds (closed, open, pedal, ride, shaker). Closed and open always show; the others
  // when they have notes or were added with "+ Sound" (UI only, per track; ✕ on an empty one hides it).
  bool hatSounds() const;  // a drum track with several sounds as rows (hats, drums, kick) and "+ Sound"
  const std::vector<int>& soundOrder() const;
  juce::String soundName(int pitch) const;
  std::vector<int> hiddenSounds() const;
  void placeAddSound();
  void removeAddedRow(int pitch);
  bool removableRow(int pitch) const;
  juce::Rectangle<int> removeMark(int pitch) const;
  juce::Rectangle<int> rowLabel(int pitch) const;  // the row's name, left of the grid
  std::map<std::string, std::set<int>> addedRows_;
  juce::TextButton addSound_;
  // No knobs card under the notes: every built-in instrument has its editor on the right (the Kick Designer, the
  // W-Core Synth), so a second copy here was redundant (Diogo 2026-11-09).
  // Build-up and Attachments, docked at the right (ToolDock.h): open while the song plays, Space plays.
  // `reopen` builds it again for another track (P-Core, Build-up); without it another track closes it (Attachments:
  // the notes it was opened on).
  ToolDock dock_;
  std::function<void()> reopenDock_;
  void dockTool(std::unique_ptr<juce::Component> tool, const juce::String& title, std::function<void()> reopen);
  int dockWidth() const;                 // with its gap; 0 when nothing is docked
  juce::Rectangle<int> area() const;     // the editor without the dock
  juce::String editorName() const;  // "STEPS" for drums, "PIANO ROLL" for the rest
  // Drums open on the bars that fit as pads; ‹ › page through a longer pattern.
  double drumFitLength() const;
  juce::TextButton pagePrev_, pageNext_;
  void placePaging();
  int64_t snapTicks() const;
  juce::Rectangle<int> grid() const;
  juce::Rectangle<int> velocityLane() const;  // under the grid: one bar per note, its height the velocity
  void setVelocityAt(int x, int y);           // the notes drawn at x get the velocity of height y
  void eraseAt(juce::Point<int> p);
  int64_t tickAtX(int x, bool snap) const;
  void movePlayheadTo(int x);  // the ruler: the start line to the tick under x
  int pitchAtY(int y) const;
  int xOfTick(int64_t tick) const;
  int playheadX() const;
  int playheadX_ = -1;  // where the line was last drawn
  double shownLength() const, shownStart() const;
  double viewStart_ = 0, viewLength_ = 0;  // zoom (UI only): 0 = the whole pattern
  juce::Rectangle<int> noteBounds(const doc::Note& n) const;
  const doc::Note* noteAt(juce::Point<int> p, bool* onRightEdge) const;
  juce::String trackLabel() const;

  Host& host_;
  std::string trackId_, patternId_;
  std::set<std::string> selected_;
  int hoverPitch_ = -1;  // the row under the mouse: lit, its note named on the keyboard
  int lowPitch_ = 24;  // bass view: the lowest key shown
  int rowsWanted_ = 0;  // keys shown (Alt+wheel); 0: as many as fit at a comfortable height
  int keysShown() const;
  juce::ComboBox patternBox_, snap_, length_;
  juce::TextButton newPattern_, clearPattern_, removePattern_;
  juce::TextButton octaveUp_, octaveDown_, zoomIn_, zoomOut_;
  void zoom(double factor, double at);
  juce::TextButton drawTool_, selectTool_, eraseTool_;
  juce::TextButton edit_;        // Edit ▾: every note tool (showTools)
  theme::InfoDot editInfo_;
  std::set<std::string> editIds() const;  // the notes an Edit tool works on (bars shown; the selected ones among them)
  void editNotes(const juce::String& label, const std::function<int(doc::Pattern&, const std::set<std::string>&)>& op);
  // A Randomize tool: tries the next seeds until a draw changes something (a single unlucky draw is not "nothing").
  void editNotesRandom(const juce::String& label,
                       const std::function<int(doc::Pattern&, const std::set<std::string>&, uint64_t seed)>& op);
  // What an Edit tool did, or why it could not: in the status line (onMessage) and in a note under the Edit button
  // for a few seconds, so a choice never looks like it did nothing (Diogo, 2026-11-05: "NAO FUNCIONA").
  void say(const juce::String& text);
  juce::Label toast_;
  int toastStamp_ = 0;
  void showTools();
  std::set<std::string> toolIds() const;  // the selected notes among the bars shown ({} = all of them)
  bool buildRoll_ = true, buildGap_ = true;
  // FOLLOW: late (+) or early (-) and how loose, in ticks, for the next follow (Tools > FOLLOW); a new seed each time.
  int64_t followOffset_ = 0, followLoose_ = 0;
  uint64_t followSeed_ = 0;
  std::string followKickChoice_, followBassChoice_;  // the tracks chosen in FOLLOW ("": automatic, by name)
  std::string followKickTrack() const;
  std::string followBassTrack() const;
  struct PCoreTake {  // what the last PCore take changed, for "Again"
    std::string trackId, patternId;
    doc::Pattern before, after;
    std::vector<doc::TempoPoint> tempoBefore, tempoAfter;
  };
  std::optional<PCoreTake> lastTake_;
  // Never the same twice (Diogo, 2026-11-07): per pattern and Edit > Randomize action the states it has made; the
  // next draw skips them.
  std::map<std::string, std::set<std::string>> randomSeen_;
  uint64_t grooveSeed_ = 1;  // the next humanize / variation draws from here (then moves on)
  Tool tool_ = Tool::Draw;
  juce::Rectangle<int> marquee_;               // the select tool's box while it is dragged
  juce::Label title_, hint_;

  enum class Drag { None, Move, Resize, Velocity, Marquee, Erase, Playhead } drag_ = Drag::None;
  juce::Point<int> marqueeStart_;
  std::set<std::string> marqueeBase_;          // selected before the box (Ctrl adds the box to them)
  std::string dragNote_;
  int64_t dragStartTick_ = 0, dragOffsetTicks_ = 0;
  int dragStartPitch_ = 0;
  std::vector<doc::Note> dragOriginal_;  // the selected notes when the drag started
  bool gestureOpen_ = false;             // drums: the move gesture starts with the first real drag

  // The ghost note and the alignment guides (see setDrawingHelp). Lines are 1 px rectangles; empty = none.
  struct Overlay {
    juce::Rectangle<int> ghost, across, down;
    juce::Rectangle<int> area() const { return ghost.getUnion(across).getUnion(down); }
    bool operator==(const Overlay& o) const { return ghost == o.ghost && across == o.across && down == o.down; }
  };
  Overlay overlay_;  // under the mouse while nothing is dragged
  bool showGhost_ = true, showGuides_ = true;
  Overlay guidesFor(juce::Rectangle<int> note, int64_t tick, int64_t end, int pitch, const std::set<std::string>& skip) const;
  Overlay hoverOverlay(juce::Point<int> p) const;
  Overlay dragOverlay() const;     // the grabbed note while it is moved
  void updateHover(juce::Point<int> p);  // repaints only the rows and the overlay that changed
  juce::Rectangle<int> rowStrip(int pitch) const;  // a row with its name and key, for small repaints
  void paintOverlay(juce::Graphics& g, const Overlay& o) const;
};

} // namespace core::app
