#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QString>
#include <filesystem>
#include <string>

#include "App.h"
#include "Dialogs.h"
#include "Instances.h"
#include "Lang.h"
#include "MainWindow.h"
#include "Process.h"
#include "Theme.h"
#include "Welcome.h"

// GUI entry point. Accepts an optional `--data <dir>` (advanced override);
// otherwise the active instance is resolved / chosen. `--play` (only
// meaningful together with `--data`) skips the main window entirely and runs
// the instance's deploy->launch->wait->rollback cycle directly, using
// whatever profile was last active for it -- this is what the desktop
// shortcut created via "Create shortcut" now points at, so double-clicking
// it goes straight into the game instead of opening the manager first.
int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  app.setOrganizationName("GMM");
  app.setApplicationName("GMM");
  app.setWindowIcon(QIcon(":/app.ico"));

  // Portable: keep UI settings (theme/language/window layout) as a plain .ini
  // file inside the program's own "data" folder instead of the registry, so the
  // whole GMM folder can be copied, moved or run from a USB stick as-is. Must
  // happen before the first QSettings object is constructed anywhere.
  // portableRoot() is normally executableDir(), but the single-file launcher
  // redirects it to *its own* folder via GMM_PORTABLE_ROOT (see Process.h) --
  // the real GMM.exe it runs lives in an internal "bin\" subfolder that gets
  // wiped on every version bump, so settings can't live there.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromStdString((gtamm::portableRoot() / "data").string()));

  QSettings settings;
  // Theme: a built-in ("dark"/"light") or a path to an MO2 .qss file. Default
  // from the legacy "dark" boolean for upgrades. Applied before anything else
  // is shown (including the first-run welcome screen below) so it's already
  // themed instead of flashing the raw Qt default style for a frame.
  const QString themeId =
      settings.value("theme", settings.value("dark", true).toBool() ? "dark" : "light")
          .toString();
  gtamm::theme::applyNamed(app, themeId);

  std::filesystem::path dataDir;
  bool haveData = false;
  bool playMode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data" && i + 1 < argc) {
      dataDir  = argv[i + 1];
      haveData = true;
      ++i;
    } else if (arg == "--play") {
      playMode = true;
    }
  }

  // --play (the desktop shortcut's mode) always comes with an explicit
  // --data, so it never needs the welcome screen or the instance picker --
  // skip straight to running the instance.
  if (playMode && haveData) {
    gtamm::App instance(dataDir);
    if (!instance.isInitialized()) {
      gtamm::dialogs::warning(
          nullptr, "GMM",
          "Instance data not found: " + QString::fromStdString(dataDir.string()));
      return 1;
    }
    // Refuse if the manager (or another quick-launch) is already running
    // against this same instance -- two processes deploying/writing the same
    // instance data concurrently (config.json, the deploy manifest, the
    // saves-folder junction swap, ...) is exactly the kind of race a single-
    // instance lock exists to prevent.
    if (!gtamm::acquireSingleInstanceLock(dataDir)) {
      gtamm::dialogs::information(
          nullptr, "GMM",
          "GMM is already running for this instance.");
      return 1;
    }
    gtamm::materializeEmbeddedRuntime(dataDir);
    try {
      instance.play(/*rollbackAfter=*/true, /*exeName=*/"");
    } catch (const std::exception& e) {
      gtamm::dialogs::warning(nullptr, "GMM", QString::fromLocal8Bit(e.what()));
      return 1;
    }
    return 0;
  }

  // First launch ever: show a one-time welcome screen that doubles as the
  // language picker, before anything else in the app is built. Its own
  // choice becomes the saved language immediately, so the translateUi()
  // calls below (and every dialog after) already see it. A dedicated flag
  // (rather than "does a saved language exist yet") avoids re-showing this
  // for an existing user who simply never touched the language setting.
  if (!settings.value("welcomeShown", false).toBool()) {
    gtamm::WelcomeDialog welcome;
    welcome.exec();
    gtamm::lang::save(welcome.selectedLanguage());
    settings.setValue("welcomeShown", true);
  }
  // Language must be chosen before any UI is built (translation happens in place).
  gtamm::lang::setLanguage(gtamm::lang::loadSaved());

  if (!haveData) {
    gtamm::InstanceStore store;
    std::filesystem::path chosen;
    for (const auto& it : store.list())
      if (it.name == store.lastUsed() &&
          std::filesystem::exists(it.dataDir / "config.json")) {
        chosen = it.dataDir;
        break;
      }
    if (chosen.empty()) {
      auto picked = gtamm::InstanceDialog::choose(nullptr);
      if (!picked)
        return 0;
      chosen = *picked;
    }
    dataDir = chosen;
  }

  // Refuse a second copy of the manager on the same instance -- two windows
  // both writing config.json/profiles/the deploy manifest for the same data
  // dir would race and corrupt each other's changes. Different instances
  // (different --data / different picks in the instance dialog) don't
  // contend with each other and can run side by side.
  if (!gtamm::acquireSingleInstanceLock(dataDir)) {
    gtamm::dialogs::warning(
        nullptr, gtamm::lang::T("GMM уже запущен"),
        gtamm::lang::T("GMM уже запущен для этого инстанса. Закройте другое "
                       "окно перед тем как открыть его снова."));
    return 1;
  }

  gtamm::MainWindow window(dataDir);
  window.show();
  return app.exec();
}
