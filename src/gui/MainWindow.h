#pragma once
#include <QMainWindow>
#include <filesystem>
#include <functional>

#include "App.h"

class QComboBox;
class QDialog;
class QVBoxLayout;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QPlainTextEdit;
class QTextEdit;
class QCheckBox;
class QTimer;
class QMenuBar;
class QEvent;
class QTextBrowser;
class QAction;
class QActionGroup;
class QDockWidget;
class QMenu;
class QSplitter;
class QTabWidget;
class QToolBar;
class QDockWidget;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QResizeEvent;

namespace gtamm {

// Unpacks the embedded Mod Loader / SA-MP runtime bundles (Qt resources under
// ":/runtime", ":/runtime-samp") to disk next to the program, so deploy() can
// find modloaderRuntimeDir()/sampRuntimeDir() content. MainWindow's own
// materializeRuntime() just forwards to this; it's a free function so the
// "--play" shortcut path in main_gui.cpp (which never constructs a
// MainWindow) can call it too.
void materializeEmbeddedRuntime(const std::filesystem::path& dataDir);

class TitleBar;  // custom, theme-aware window caption (defined in Frame.h/.cpp)

// Full Qt Widgets front-end over the App core: a multi-column mod table with
// drag-reorder + checkboxes, a details panel (info/files + conflicts), a menu,
// a toolbar, a filter box and a status bar.
class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(std::filesystem::path dataDir, QWidget* parent = nullptr);

protected:
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  bool event(QEvent* event) override;                  // sync title/maximize state
  bool eventFilter(QObject* obj, QEvent* event) override;  // edge-resize handling

private slots:
  void onRefresh();
  void onProfileChanged(int index);
  void onSwitchInstance();
  void onChangeGameFolder();
  void onOpenInstanceFolder();
  void onOpenGameFolder();
  void onNewProfile();
  void onRenameProfile();
  void onCopyProfile();
  void onDeleteProfile();
  void onManageProfiles();  // list all profiles: rename/delete/export/import in one window
  void onToggleManageSaves(bool on);
  void onOpenSavesFolder();
  void onToggleManageGenerated(bool on);
  void onOpenGeneratedFolder();
  void onToggleAutoMaps(bool on);  // experimental map auto-install
  void onToggleSteamIntegration(bool on);  // drop steam_appid.txt during play()
  void onSampSettings();  // edit the active profile's SA-MP (multiplayer) launch settings
  void onLaunchSannyBuilder();  // start the user's own Sanny Builder 4 copy
  void onImportFolder();
  void onImportArchive();
  void onImportBuild();
  void onExportBuild();    // export active profile + its mods to a zip
  void onImportBuildZip(); // import a build zip into a new/existing profile
  void onAddSeparator();
  void onDeploy();
  void onRollback();
  void onPlay();
  void onCreateShortcut();  // create a desktop .lnk that opens this instance
  void onUnlockPlay();  // dismiss the play lock and browse while the game runs
  void onKillGame();    // forcibly terminate the running game process
  void onConflicts();
  void onSettings();  // open the consolidated program-settings dialog
  void onAbout();
  void onEditBuildInfo();    // enter rich-text edit mode for the active profile
  void onEditModDescription();  // small dialog to edit the selected mod's description
  void onInsertBuildImage(); // copy an image into the profile and embed it
  void onSaveBuildInfo();    // persist the HTML and return to the rendered view
  void onCancelBuildInfo();  // discard edits and return to the rendered view
  void onModContextMenu(const QPoint& pos);
  // Open a simple text editor for one moonloader/*.lua file inside a pool mod
  // (relPath is game-relative, e.g. "moonloader/foo.lua"). Saving writes
  // straight back into the pool via App::writeModFile().
  void onEditLuaScript(const QString& modId, const QString& relPath);
  // The pencil next to a mod's blue "Lua" badge was clicked: open that mod's
  // script (asking which one if it has several).
  void onEditLuaFromTag(const QString& modId);
  void onListEdited();            // checkbox toggled -> persist enabled state
  void onRowsMoved();             // drag-reorder -> renumber priority + persist
  void onHeaderClicked(int column);  // column sort (browse) / return to manual
  void onSortComboChanged(int index);  // pick the sort field from the toolbar combo
  void onSortDirToggled();             // flip ascending/descending
  void onSelectionChanged();      // update the details panel
  void onFilterChanged(const QString& text);
  void onRevealPanel();           // restore the collapsed run/info side panel
  void onToggleLogPanel();        // show/hide the bottom log viewer
  void onRefreshLogs();           // rescan the build for *.log files
  void onLogSelected(int index);  // load the chosen log file

private:
  void setupFrame();           // frameless window + custom title bar + menu area
  void applyFrameTheme();      // (re)style the title bar from the active palette
  // Gives an arbitrary QDialog the same frameless custom title bar as MainWindow
  // (icon + title + minimize/maximize/close, themed, draggable, edge-resizable
  // via the app-wide event filter below). `showMinMax` false drops the
  // minimize/maximize buttons for a shorter caption, for windows never meant
  // to be resized that way (e.g. "About"). Returns the layout the caller
  // should add the dialog's own content to -- the dialog must not create its
  // own top-level layout.
  QVBoxLayout* frameless(QDialog& dlg, bool showMinMax = true);
  // Centers `dlg` over this window, both horizontally and vertically. Must
  // be called by the caller, AFTER all of the dialog's own content has been
  // added, and immediately before its own exec()/show().
  void centerDialog(QDialog& dlg);
  // Which edges of `w` the cursor at `globalPos` is on (grab band for
  // edge-resize). `w` is whatever frameless top-level window is under the
  // cursor -- MainWindow itself, or a dialog framed via frameless() above.
  Qt::Edges edgesAt(const QPoint& globalPos, const QWidget* w) const;
  void updateResizeCursor(Qt::Edges edges);
  void clearResizeCursor();
  void buildMenu();
  void buildToolBar();
  void buildCentral();
  void buildLogPanel();              // bottom dock listing/showing *.log files
  void loadLog(const QString& path, bool keepPosition = false);  // read a log into the viewer
  void pollLog();                    // timer tick: live-tail the current log
  void enforceToolbarWidth();  // widen window so toolbar rows never overflow to ">>"
  void ensureInitialized();
  void refreshProfiles();
  void refreshMods();
  void persistOrder();
  void updateStatusBar();
  void updateDerivedColumns();  // recompute priority + conflict marks in place
  void renumberPriorities();    // priority := top-down row position
  void autoSizeTagsColumn();    // "Type" width := widest row's badges
  void moveModPriority(int delta);  // move the selected row up(-1)/down(+1)
  void applySort(int column, Qt::SortOrder order);  // sort the view (or manual order)
  void syncSortControls();           // reflect the active sort in the combo/dir button
  void updateRevealButton();         // show/place the reveal button when the panel is hidden
  void updateDetails(const QString& modId);
  void showBuildInfo();              // render the active profile's build notes (HTML)
  void setBuildInfoEditing(bool on); // toggle rendered view <-> rich-text editor
  void applyFilter();
  void saveUiState();
  void restoreUiState();
  void applyTheme(const QString& id);  // "dark"/"light" or a path to an MO2 .qss
  void rebuildThemeMenu();             // (re)scan the themes folder into the menu
  std::filesystem::path themesDir() const;
  QString detailsCss() const;  // HTML colors for the details panel per theme
  void populateExecutables();
  // Show/hide the SA-MP and Sanny Builder toolbar buttons: SA-MP only while the
  // active profile actually launches through it, Sanny Builder only once it's
  // been enabled and pointed at a real copy.
  void updateExternalToolActions();
  void updateRunIcon();  // show the selected exe's own icon next to the run combo
  // `nameOverride`, if non-empty, replaces the mod's default derived name
  // (folder/archive basename) -- only applies when the source imports as a
  // single mod; a Mod Loader pack that splits into several mods still names
  // each one after its own subfolder regardless.
  void importPath(const QString& path, bool viaModloader, const QString& nameOverride = QString());
  // Ask whether to import a mod natively or with Mod Loader support, and let
  // the user rename it away from `defaultName` (pre-filled, editable). Returns
  // false on cancel; on success sets `viaModloader` and `name` (empty if the
  // user left the default name untouched -- importPath() then falls back to
  // the source's own name).
  bool askImportMode(bool& viaModloader, QString& name, const QString& defaultName);
  // Prompts for a target profile name and imports a GMM build zip into it
  // (shared by the "Импорт сборки из zip…" menu action, the profile manager's
  // own import button, and drag-and-drop). Returns the profile name on success,
  // or an empty string if cancelled/failed (caller-side state is left as-is).
  QString importBuildZipInteractive(const QString& zipPath);
  void onModloaderRuntimeFolder();  // choose/open the Mod Loader runtime folder
  // Unpack the Mod Loader runtime embedded in the executable (Qt resource
  // ":/runtime/*") into a folder the core deploys from, so it "just works" with
  // no path setup. No-op if nothing is embedded.
  void materializeRuntime();
  void addModRow(const std::string& id, const std::string& name, bool enabled,
                 bool viaModloader);
  void styleSeparatorItem(QTreeWidgetItem* item, const QString& label);
  void guarded(const QString& context, const std::function<void()>& fn);
  void setPlayLock(bool locked);  // disable + dim the window while the game runs

  App m_app;

  TitleBar* m_titleBar        = nullptr;  // custom caption (icon/title/min/max/close)
  QMenuBar* m_menuBar         = nullptr;  // our own bar (lives under the title bar)
  QSplitter* m_splitter       = nullptr;
  QToolBar* m_toolBar         = nullptr;
  QComboBox* m_profile        = nullptr;
  QComboBox* m_runExe         = nullptr;
  QLabel* m_runIcon           = nullptr;
  QPushButton* m_runButton    = nullptr;
  QPushButton* m_shortcutButton = nullptr;
  QTreeWidget* m_mods         = nullptr;
  QLineEdit* m_filter         = nullptr;
  QComboBox* m_sortCombo      = nullptr;  // sort field selector (load order/name/…)
  QToolButton* m_sortDirButton = nullptr; // ascending/descending toggle
  QToolButton* m_revealPanelButton = nullptr;  // edge button to restore a hidden panel

  // Bottom log viewer: a dock with a file selector + read-only text view that
  // live-tails the selected *.log file.
  QDockWidget* m_logDock      = nullptr;
  QComboBox* m_logCombo       = nullptr;
  QPlainTextEdit* m_logView   = nullptr;
  QCheckBox* m_logAutoCheck   = nullptr;
  QTimer* m_logTimer          = nullptr;
  QToolButton* m_logToggleButton = nullptr;  // in the status bar
  QString m_logCurrentPath;                  // file currently shown
  qint64 m_logSize  = -1;                     // last seen size (change detection)
  qint64 m_logMtime = 0;                      // last seen mtime (ms)
  QTextBrowser* m_info        = nullptr;
  QTextBrowser* m_conflicts   = nullptr;

  // Shown in the Info tab only when a mod (not the build) is selected: opens a
  // small dialog to edit that mod's plain-text description (Mod::description).
  QPushButton* m_modDescButton = nullptr;
  QString m_detailModId;  // which mod updateDetails() last rendered, for the edit button

  // Build-info (per-profile rich-text notes, stored as HTML) shown in the Info
  // tab when no mod is selected, with a WYSIWYG editor + image insertion.
  QTextEdit* m_infoEditor           = nullptr;
  QPushButton* m_infoEditButton     = nullptr;
  QPushButton* m_infoInsertImgButton = nullptr;
  QPushButton* m_infoSaveButton     = nullptr;
  QPushButton* m_infoCancelButton   = nullptr;
  // Formatting toolbar for m_infoEditor: bold/italic/underline, alignment,
  // font size. Only visible while editing.
  QToolBar* m_infoFormatBar         = nullptr;
  QAction* m_infoBoldAction         = nullptr;
  QAction* m_infoItalicAction       = nullptr;
  QAction* m_infoUnderlineAction    = nullptr;
  QAction* m_infoAlignLeftAction    = nullptr;
  QAction* m_infoAlignCenterAction  = nullptr;
  QAction* m_infoAlignRightAction   = nullptr;
  QAction* m_infoAlignJustifyAction = nullptr;
  QComboBox* m_infoFontSizeCombo    = nullptr;
  // Resizes the image the cursor is currently on/next to, as a percentage of
  // its natural (on-disk) size. Disabled when the cursor isn't near an image.
  QSpinBox* m_infoImageSizeSpin     = nullptr;
  bool m_infoEditing  = false;
  bool m_showingBuild = false;  // the Info tab currently shows build notes
  QLabel* m_gameLabel         = nullptr;
  QLabel* m_deployLabel       = nullptr;
  QLabel* m_countLabel        = nullptr;
  QAction* m_deployAction     = nullptr;
  QAction* m_rollbackAction   = nullptr;
  QAction* m_sampAction       = nullptr;  // shown only when SA-MP is on
  QAction* m_sannyAction      = nullptr;  // shown only when Sanny Builder is set up
  QMenu* m_themeMenu          = nullptr;  // legacy (theme now lives in Settings dialog)
  QActionGroup* m_themeGroup  = nullptr;
  QString m_themeId           = "dark";  // active theme id (built-in or .qss path)

  QWidget* m_playOverlay = nullptr;  // dim "game running" cover, child of window

  bool m_populating  = false;
  bool m_dark        = true;
  bool m_playRunning = false;  // a play cycle (deploy→run→rollback) is in flight

  Qt::Edges m_hoverEdges;            // window edges under the cursor (for resize)
  bool m_cursorOverridden = false;   // a resize cursor is currently pushed

  // Active browse-sort column (-1 = manual priority order); used to toggle dir.
  int m_sortColumn = -1;
  Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};

}  // namespace gtamm
