// Core Setup.exe (M6): installs Core for the current user only (no administrator rights).
//   Core Setup.exe                                   a window: install (and copy the sample library), update, go back, uninstall
//   Core Setup.exe --install <folder> [--no-shortcuts] [--samples <library> | --no-samples]   silent (tests); exit 0 when installed
//   Core Setup.exe --uninstall <folder> [--no-shortcuts]  silent
// Layout of <folder> (default %LOCALAPPDATA%\Programs\Core):
//   app-<version>\  Core.exe, core-engine.exe, demo, templates, racks (from the package inside this program)
//   current.txt     the version the shortcuts start; previous.txt: the one before (kept for going back)
//   Uninstall.exe   a copy of this program
//   Samples\        the sample library, part of Core (copied, not packed: ~7 GB does not fit in an .exe). It sits
//                   beside the versions so an update copies only the files that changed; Core's Library uses it.
//   samples.txt     the files of Samples\ this installer copied (uninstalling removes only those)
// The library is copied from --samples <folder>, else a Samples folder beside this program, else D:\Samples
// (Diogo's PC). The source is only read; its _core-plan folder (scripts and notes) is not copied.
// Songs, settings, plugins lists, autosaves and logs are never touched: they live in the user's folders.
#include <cstdio>
#include <algorithm>
#include <juce_gui_basics/juce_gui_basics.h>
#include "CoreSetupData.h"

namespace {

const juce::String kVersion = JUCE_APPLICATION_VERSION_STRING;
const juce::String kUninstallKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Core\\";

juce::File defaultFolder() {
  return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Programs").getChildFile("Core");
}
juce::File startMenuLink() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Microsoft\\Windows\\Start Menu\\Programs\\Core.lnk");
}
juce::File desktopLink() { return juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("Core.lnk"); }

juce::String installedVersion(const juce::File& folder) { return folder.getChildFile("current.txt").loadFileAsString().trim(); }
juce::File appFolder(const juce::File& folder, const juce::String& version) { return folder.getChildFile("app-" + version); }

// Points the shortcuts and the Windows "Installed apps" entry at a version that is in the folder.
void integrate(const juce::File& folder, const juce::String& version, bool desktop) {
  const juce::File exe = appFolder(folder, version).getChildFile("Core.exe");
  startMenuLink().getParentDirectory().createDirectory();
  exe.createShortcut("Core", startMenuLink());
  if (desktop || desktopLink().existsAsFile()) exe.createShortcut("Core", desktopLink());
  juce::WindowsRegistry::setValue(kUninstallKey + "DisplayName", "Core");
  juce::WindowsRegistry::setValue(kUninstallKey + "DisplayVersion", version);
  juce::WindowsRegistry::setValue(kUninstallKey + "Publisher", "Core");
  juce::WindowsRegistry::setValue(kUninstallKey + "InstallLocation", folder.getFullPathName());
  juce::WindowsRegistry::setValue(kUninstallKey + "DisplayIcon", exe.getFullPathName());
  juce::WindowsRegistry::setValue(kUninstallKey + "UninstallString", "\"" + folder.getChildFile("Uninstall.exe").getFullPathName() + "\" --uninstall");
  juce::WindowsRegistry::setValue(kUninstallKey + "NoModify", (juce::uint32)1);
  juce::WindowsRegistry::setValue(kUninstallKey + "NoRepair", (juce::uint32)1);
}

// ---------------------------------------------------------------- the sample library

juce::File samplesSource(const juce::String& given) {
  if (given.isNotEmpty()) return juce::File(given);
  const juce::File beside = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("Samples");
  if (beside.isDirectory()) return beside;
  const juce::File usual("D:\\Samples");
  return usual.isDirectory() ? usual : juce::File();
}

bool skipped(const juce::String& relative) { return relative.startsWith("_core-plan"); }

// Copies the library into <folder>\Samples: a file already there with the same size and date is left as it is,
// so an update is quick. Files Core added there (Save as sample) are never touched.
// progress(done, total) is called from this thread.
juce::String copySamples(const juce::File& source, const juce::File& folder, const std::function<void(int, int)>& progress) {
  if (source == juce::File()) return {};
  if (!source.isDirectory()) return "The sample library is not there: " + source.getFullPathName();
  const juce::File target = folder.getChildFile("Samples");
  if (target == source) return {};
  juce::Array<juce::File> files;
  for (const auto& entry : juce::RangedDirectoryIterator(source, true, "*", juce::File::findFiles))
    if (!skipped(entry.getFile().getRelativePathFrom(source).replaceCharacter('\\', '/'))) files.add(entry.getFile());
  // Audit 27: the manifest read BEFORE copying decides ownership. A path becomes ours only when this install
  // really copied it, or an earlier install already listed it. Before, a pre-existing file that merely matched
  // path+size+date (copied back from a backup, robocopy /COPY:DAT) was listed without ever being copied, and
  // uninstall deleted the user's file.
  juce::StringArray listed = juce::StringArray::fromLines(folder.getChildFile("samples.txt").loadFileAsString());
  listed.removeEmptyStrings();
  juce::StringArray ours;
  int failed = 0;
  for (int i = 0; i < files.size(); ++i) {
    const juce::File& from = files.getReference(i);
    const juce::String relative = from.getRelativePathFrom(source);
    const juce::File to = target.getChildFile(relative);
    const juce::String rel = relative.replaceCharacter('\\', '/');
    const bool same = to.existsAsFile() && to.getSize() == from.getSize() && to.getLastModificationTime() == from.getLastModificationTime();
    if (!same) {
      if (to.getParentDirectory().createDirectory() && from.copyFileTo(to)) ours.add(rel);
      else ++failed;
    } else if (listed.contains(rel)) ours.add(rel);  // ours from an earlier install: stays, no copy needed
    if (progress && (i % 25 == 0 || i + 1 == files.size())) progress(i + 1, files.size());
  }
  // Earlier installs' files stay listed too (they are still ours to remove).
  listed.addArray(ours);
  listed.removeDuplicates(true);
  folder.getChildFile("samples.txt").replaceWithText(listed.joinIntoString("\n") + "\n");
  if (failed > 0) return juce::String(failed) + " samples could not be copied to " + target.getFullPathName() + " (is the disk full?). Install again to finish.";
  return {};
}

// Removes the samples this installer copied, then the folders left empty. Anything else in Samples\ stays.
bool removeSamples(const juce::File& folder) {
  const juce::File target = folder.getChildFile("Samples"), list = folder.getChildFile("samples.txt");
  bool ok = true;
  for (const auto& relative : juce::StringArray::fromLines(list.loadFileAsString()))
    if (relative.isNotEmpty() && !relative.contains("..")) {
      const juce::File f = target.getChildFile(relative);
      if (f.existsAsFile()) ok = f.deleteFile() && ok;
    }
  if (target.isDirectory()) {
    juce::Array<juce::File> dirs = target.findChildFiles(juce::File::findDirectories, true);
    for (int i = dirs.size(); --i >= 0;)  // deepest first
      if (dirs[i].getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) dirs[i].deleteFile();
    if (target.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) target.deleteFile();
  }
  if (ok) list.deleteFile();
  return ok;
}

// A version owns only the files listed by its package. Unknown files in an app-* folder
// may be songs/samples saved there by the user, so never recursively delete that folder.
constexpr const char* kAppManifest = ".core-installed-files.txt";
juce::StringArray packageFiles() {
  juce::MemoryInputStream package(CoreSetupData::CorePackage_zip, CoreSetupData::CorePackage_zipSize, false);
  juce::ZipFile zip(package);
  juce::StringArray files;
  for (int i = 0; i < zip.getNumEntries(); ++i) {
    const auto name = zip.getEntry(i)->filename.replaceCharacter('\\', '/');
    if (!name.endsWithChar('/')) files.add(name);
  }
  return files;
}
juce::StringArray installedAppFiles(const juce::File& folder) {
  const auto manifest = folder.getChildFile(kAppManifest);
  // An older installer left no manifest: only its fixed executable names are certain.
  // Do not use this version's ZIP to guess ownership of an older version's resources.
  auto files = manifest.existsAsFile() ? juce::StringArray::fromLines(manifest.loadFileAsString())
                                      : juce::StringArray{"Core.exe", "core-engine.exe", "core-clips.exe"};
  files.removeEmptyStrings();
  files.addIfNotAlreadyThere(kAppManifest);
  return files;
}
bool hasAdditionalAppFiles(const juce::File& folder) {
  const auto known = installedAppFiles(folder);
  for (const auto& f : folder.findChildFiles(juce::File::findFiles, true))
    if (!known.contains(f.getRelativePathFrom(folder).replaceCharacter('\\', '/'), true)) return true;
  return false;
}
bool removeInstalledAppFiles(const juce::File& folder) {
  bool ok = true;
  for (const auto& relative : installedAppFiles(folder)) {
    if (juce::File::isAbsolutePath(relative) || relative.contains("..")) continue;
    const auto file = folder.getChildFile(relative);
    if (file.existsAsFile()) ok = file.deleteFile() && ok;
  }
  auto dirs = folder.findChildFiles(juce::File::findDirectories, true);
  std::sort(dirs.begin(), dirs.end(), [](const juce::File& a, const juce::File& b) {
    return a.getFullPathName().length() > b.getFullPathName().length();
  });
  for (const auto& dir : dirs) if (dir.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) dir.deleteFile();
  if (folder.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0) folder.deleteFile();
  return ok;
}

// Unpacks this program's package into app-<version> (a .part folder first, renamed when complete), makes
// it the current version and keeps the one before it for going back. Older versions are removed.
juce::String install(const juce::File& folder, bool withShortcuts, bool desktop) {
  if (!folder.createDirectory()) return "Cannot create " + folder.getFullPathName();
  juce::MemoryInputStream package(CoreSetupData::CorePackage_zip, CoreSetupData::CorePackage_zipSize, false);
  juce::ZipFile zip(package);
  if (zip.getNumEntries() == 0) return "The package inside the installer is empty.";
  const juce::File target = appFolder(folder, kVersion), part = folder.getChildFile("app-" + kVersion + ".part");
  if (target.isDirectory() && hasAdditionalAppFiles(target))
    return "Additional files are kept in " + target.getFullPathName() + ". Move them out before reinstalling this version.";
  part.deleteRecursively();
  if (auto r = zip.uncompressTo(part); r.failed()) { part.deleteRecursively(); return "Cannot unpack: " + r.getErrorMessage(); }
  if (!part.getChildFile("Core.exe").existsAsFile()) { part.deleteRecursively(); return "The package has no Core.exe."; }
  if (!part.getChildFile(kAppManifest).replaceWithText(packageFiles().joinIntoString("\n") + "\n"))
    return "Cannot write the installed file list.";
  const juce::String before = installedVersion(folder);
  if (target.exists() && !target.deleteRecursively()) {
    part.deleteRecursively();
    return "Core " + kVersion + " is open: close it and try again.";
  }
  if (!part.moveFileTo(target)) return "Cannot move the new version into place.";
  folder.getChildFile("current.txt").replaceWithText(kVersion);
  if (before.isNotEmpty() && before != kVersion) folder.getChildFile("previous.txt").replaceWithText(before);
  // Only the current version and the one before it stay.
  const juce::String keep = folder.getChildFile("previous.txt").loadFileAsString().trim();
  for (const auto& old : folder.findChildFiles(juce::File::findDirectories, false, "app-*"))
    if (old != target && old.getFileName() != "app-" + keep) removeInstalledAppFiles(old);
  const juce::File self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
  if (self != folder.getChildFile("Uninstall.exe")) self.copyFileTo(folder.getChildFile("Uninstall.exe"));
  if (withShortcuts) integrate(folder, kVersion, desktop);
  return {};
}

// Back to the version kept from before (the shortcuts start it again); the newer one stays, unused.
juce::String goBack(const juce::File& folder) {
  const juce::String previous = folder.getChildFile("previous.txt").loadFileAsString().trim();
  const juce::String current = installedVersion(folder);
  if (previous.isEmpty() || !appFolder(folder, previous).getChildFile("Core.exe").existsAsFile()) return "There is no earlier version to go back to.";
  folder.getChildFile("current.txt").replaceWithText(previous);
  folder.getChildFile("previous.txt").replaceWithText(current);
  integrate(folder, previous, false);
  return {};
}

// A folder Core Setup installed: current.txt names a version whose app-<version>\Core.exe is there.
bool isCoreFolder(const juce::File& folder) {
  const juce::String version = installedVersion(folder);
  return version.isNotEmpty() && appFolder(folder, version).getChildFile("Core.exe").existsAsFile();
}

// What this installer puts in a folder; uninstalling removes only these, never anything else.
bool isOurs(const juce::File& f) {
  const juce::String name = f.getFileName();
  if (f.isDirectory()) return name.startsWith("app-");
  return name == "current.txt" || name == "previous.txt" || name == "Uninstall.exe" || name == "samples.txt";
}

// Removes the program, its shortcuts and its entry. The songs and the user's data stay. A folder that is
// not a Core install is refused, and in one that is, only what the installer put there is removed (the
// folder itself goes only when nothing else is left in it).
juce::String uninstall(const juce::File& folder, bool withShortcuts) {
  if (!isCoreFolder(folder)) return "Not a Core install folder (no current.txt with its app folder): " + folder.getFullPathName();
  if (withShortcuts) {
    startMenuLink().deleteFile();
    desktopLink().deleteFile();
    juce::WindowsRegistry::deleteKey(kUninstallKey.dropLastCharacters(1));
  }
  const juce::File self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
  bool ok = removeSamples(folder), othersLeft = false;
  for (const auto& f : folder.findChildFiles(juce::File::findFilesAndDirectories, false)) {
    if (!isOurs(f)) { othersLeft = true; continue; }
    if (f.isDirectory()) {
      ok = removeInstalledAppFiles(f) && ok;
      othersLeft = f.exists() || othersLeft;
      continue;
    }
    if (f != self) ok = f.deleteRecursively() && ok;
  }
  if (!ok) return "Some files are in use (is Core open?): close it and try again.";
  if (self.getParentDirectory() == folder) {
    // This program is running from the folder: it is removed by a separate step once it has closed.
    juce::ChildProcess().start("cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /q \"" + self.getFullPathName() + "\" & rmdir \"" + folder.getFullPathName() + "\"", 0);
  } else if (!othersLeft) {
    folder.deleteFile();  // empty by now: deleteFile removes an empty directory
  }
  return {};
}

// ---------------------------------------------------------------- the window

class SetupComponent final : public juce::Component {
public:
  SetupComponent() {
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &text_, &desktop_, &install_, &back_, &remove_, &open_}) addAndMakeVisible(c);
    title_.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    text_.setJustificationType(juce::Justification::topLeft);
    desktop_.setButtonText("Shortcut on the desktop");
    desktop_.setToggleState(true, juce::dontSendNotification);
    install_.onClick = [this] {
      const juce::String problem = install(folder_, true, desktop_.getToggleState());
      if (problem.isNotEmpty()) return report(problem, {});
      const juce::File source = samplesSource({});
      if (source == juce::File()) return report({}, "Core " + kVersion + " is installed (no sample library found to copy).");
      // The library is large (the first copy takes minutes): it is copied on its own thread, with the count shown.
      for (auto* b : {&install_, &back_, &remove_, &open_}) b->setEnabled(false);
      juce::Component::SafePointer<SetupComponent> self(this);
      const juce::File folder = folder_;
      juce::Thread::launch([self, source, folder] {
        const juce::String copyProblem = copySamples(source, folder, [self](int done, int total) {
          juce::MessageManager::callAsync([self, done, total] {
            if (self != nullptr) self->title_.setText("Copying samples... " + juce::String(done) + " / " + juce::String(total), juce::dontSendNotification);
          });
        });
        juce::MessageManager::callAsync([self, copyProblem] {
          if (self != nullptr) self->report(copyProblem, "Core " + kVersion + " is installed, with its sample library.");
        });
      });
    };
    back_.onClick = [this] { report(goBack(folder_), "Core " + installedVersion(folder_) + " is back."); };
    remove_.onClick = [this] {
      juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon).withTitle("Uninstall Core")
                                       .withMessage("Remove Core from this PC? Your songs, settings and plugins stay where they are.")
                                       .withButton("Uninstall", 1).withButton("Cancel", 0),
                                   [this](int r) {
                                     if (r != 1) return;
                                     const juce::String problem = uninstall(folder_, true);
                                     if (problem.isNotEmpty()) return report(problem, {});
                                     juce::JUCEApplication::quit();
                                   });
    };
    open_.setButtonText("Open Core");
    open_.onClick = [this] {
      appFolder(folder_, installedVersion(folder_)).getChildFile("Core.exe").startAsProcess();
      juce::JUCEApplication::quit();
    };
    refresh();
    setSize(520, 300);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(20);
    title_.setBounds(r.removeFromTop(34));
    auto buttons = r.removeFromBottom(32);
    install_.setBounds(buttons.removeFromLeft(150).reduced(2, 0));
    back_.setBounds(buttons.removeFromLeft(150).reduced(2, 0));
    open_.setBounds(buttons.removeFromRight(100).reduced(2, 0));
    remove_.setBounds(buttons.removeFromRight(100).reduced(2, 0));
    r.removeFromBottom(8);
    desktop_.setBounds(r.removeFromBottom(26));
    text_.setBounds(r);
  }

  void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff15171c)); }

private:
  void refresh() {
    const juce::String installed = installedVersion(folder_);
    const juce::String previous = folder_.getChildFile("previous.txt").loadFileAsString().trim();
    title_.setText(installed.isEmpty() ? "Install Core " + kVersion : "Core " + installed + " is installed", juce::dontSendNotification);
    text_.setText("For this user only, no administrator rights needed.\nFolder: " + folder_.getFullPathName() +
                      "\nA shortcut goes in the Start menu.\n\nYour songs, settings, plugin list, autosaves and logs are never changed by "
                      "installing, updating or uninstalling." + (installed.isNotEmpty() && installed != kVersion ? "\n\nThis installer has Core " + kVersion + "." : juce::String()),
                  juce::dontSendNotification);
    install_.setButtonText(installed.isEmpty() ? "Install" : installed == kVersion ? "Install again" : "Update to " + kVersion);
    back_.setButtonText(previous.isNotEmpty() ? "Go back to " + previous : "Go back");
    back_.setEnabled(installed.isNotEmpty() && previous.isNotEmpty() && appFolder(folder_, previous).exists());
    remove_.setEnabled(installed.isNotEmpty());
    open_.setEnabled(installed.isNotEmpty());
    desktop_.setVisible(installed.isEmpty());
  }

  void report(const juce::String& problem, const juce::String& done) {
    refresh();
    juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(problem.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("Core Setup").withMessage(problem.isEmpty() ? done : problem).withButton("OK", 0), nullptr);
  }

  juce::File folder_ = defaultFolder();
  juce::Label title_, text_;
  juce::ToggleButton desktop_;
  juce::TextButton install_, back_, remove_{"Uninstall"}, open_;
};

class SetupWindow final : public juce::DocumentWindow {
public:
  SetupWindow() : DocumentWindow("Core Setup", juce::Colour(0xff15171c), DocumentWindow::closeButton) {
    setUsingNativeTitleBar(true);
    setContentOwned(new SetupComponent(), true);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
  }
  void closeButtonPressed() override { juce::JUCEApplication::quit(); }
};

juce::String option(const juce::StringArray& args, const char* name) {
  const int i = args.indexOf(name);
  return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String();
}

class SetupApplication final : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return "Core Setup"; }
  const juce::String getApplicationVersion() override { return kVersion; }
  bool moreThanOneInstanceAllowed() override { return false; }
  void initialise(const juce::String& commandLine) override {
    const juce::StringArray args = juce::StringArray::fromTokens(commandLine, true);
    const bool shortcuts = !args.contains("--no-shortcuts");
    if (args.contains("--install") || args.contains("--uninstall")) {
      const juce::String given = option(args, args.contains("--install") ? "--install" : "--uninstall");
      const juce::File self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
      const juce::File folder = given.isNotEmpty() && !given.startsWith("--") ? juce::File(given)
                              : args.contains("--uninstall") && isCoreFolder(self.getParentDirectory()) ? self.getParentDirectory()
                                                                                                         : defaultFolder();
      juce::String problem = args.contains("--install") ? install(folder, shortcuts, false) : uninstall(folder, shortcuts);
      if (problem.isEmpty() && args.contains("--install") && !args.contains("--no-samples"))
        problem = copySamples(samplesSource(option(args, "--samples")), folder, nullptr);
      if (problem.isNotEmpty()) std::fputs((problem + "\n").toRawUTF8(), stderr);
      setApplicationReturnValue(problem.isEmpty() ? 0 : 1);
      quit();
      return;
    }
    window_ = std::make_unique<SetupWindow>();
  }
  void shutdown() override { window_.reset(); }
private:
  std::unique_ptr<SetupWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(SetupApplication)
