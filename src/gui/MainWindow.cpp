#include "MainWindow.h"

#include "Dialogs.h"
#include "Frame.h"
#include "ServerBrowser.h"
#include "Instances.h"
#include "Lang.h"
#include "ModTags.h"
#include "Process.h"
#include "SannyBuilder.h"
#include "Theme.h"
#include "Unpack.h"
#include "Version.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#include <objbase.h>
#endif

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QMimeData>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QIcon>
#include <QImage>
#include <QPlainTextEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygon>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSize>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringConverter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

namespace fs = std::filesystem;

namespace gtamm {

namespace {

enum Column { ColCheck = 0, ColPrio, ColName, ColTags, ColConflict, ColFiles,
              ColId, ColCount };
constexpr int kModIdRole = Qt::UserRole + 1;
constexpr int kIsSepRole = Qt::UserRole + 2;
// Authoritative load-order priority (higher wins). Kept independent of the row's
// visual position so column sorting can be a non-destructive view: only an
// explicit drag-reorder renumbers it. Stored on the ColCheck column like the
// other custom roles.
constexpr int kPrioRole = Qt::UserRole + 3;
// Content-type tag ids (rules::modTypeId) painted as badges in ColTags by
// ModTagDelegate. Stored on that column, since that's where it draws them.
constexpr int kTagsRole = Qt::UserRole + 4;
// True when the mod carries editable .lua source (as opposed to compiled
// .luac bytecode only) -- that's what the badge's pencil opens.
constexpr int kLuaEditRole = Qt::UserRole + 5;

// Tree item that sorts the numeric columns (priority, file count) numerically
// instead of lexicographically when the user clicks those headers.
class ModItem : public QTreeWidgetItem
{
public:
  using QTreeWidgetItem::QTreeWidgetItem;
  bool operator<(const QTreeWidgetItem& other) const override
  {
    const QTreeWidget* tw = treeWidget();
    const int col = tw ? tw->sortColumn() : ColName;
    if (col == ColPrio)
      return data(ColCheck, kPrioRole).toInt() < other.data(ColCheck, kPrioRole).toInt();
    if (col == ColFiles)
      return text(ColFiles).toInt() < other.text(ColFiles).toInt();
    return QTreeWidgetItem::operator<(other);
  }
};

// QTreeWidget whose internal drag-and-drop reorder reliably reports back. Qt's
// QTreeWidget does NOT emit rowsMoved for an internal move, so a pure drag
// (without any other edit) would never get persisted — fixed here by hooking
// dropEvent and notifying once the move has fully settled.
class ModTree : public QTreeWidget
{
public:
  using QTreeWidget::QTreeWidget;
  std::function<void()> afterDrop;

protected:
  void dropEvent(QDropEvent* event) override
  {
    QTreeWidget::dropEvent(event);
    // Defer: an internal move may finish removing the source row right after
    // dropEvent returns, so read the final order on the next event-loop turn.
    if (afterDrop)
      QTimer::singleShot(0, this, [this] {
        if (afterDrop)
          afterDrop();
      });
  }
};

QString humanTime(std::int64_t secs)
{
  if (secs <= 0)
    return "—";
  return QDateTime::fromSecsSinceEpoch(secs).toString("yyyy-MM-dd hh:mm");
}

// A green "play" triangle icon for the launch button.
QIcon greenPlayIcon()
{
  QPixmap pm(22, 22);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QPolygon tri;
  tri << QPoint(5, 4) << QPoint(5, 18) << QPoint(18, 11);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x2e, 0xcc, 0x40));
  p.drawPolygon(tri);
  return QIcon(pm);
}

std::vector<std::string> listModFiles(const fs::path& root)
{
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(root, ec))
    return out;
  for (const auto& de : fs::recursive_directory_iterator(root, ec))
    if (de.is_regular_file(ec))
      out.push_back(fs::relative(de.path(), root, ec).generic_string());
  std::sort(out.begin(), out.end());
  return out;
}

// Editable MoonLoader scripts inside a mod: .lua sources anywhere in its tree
// (compiled .luac bytecode is excluded -- opening that in a text editor would
// only show binary garbage). Broader on purpose than App::listModLuaScripts,
// which is limited to top-level moonloader/*.lua because it feeds the
// split-into-separate-mods action; a script dropped straight into a mod's root
// is still perfectly editable.
QStringList luaScriptsOf(const std::vector<std::string>& files)
{
  QStringList out;
  for (const std::string& f : files)
    if (QString::fromStdString(fs::path(f).extension().string()).toLower() == ".lua")
      out << QString::fromStdString(f);
  return out;
}

// An inline image is a single "object replacement" character; clicking one (or
// arrowing next to it) leaves the caret sitting at either its start or its end
// boundary, so both neighboring characters need checking.
struct CursorImage
{
  QTextImageFormat format;
  int start = 0;  // document position of the image's one character
};

std::optional<CursorImage> imageFormatAtCursor(const QTextEdit* editor)
{
  if (!editor)
    return std::nullopt;
  QTextDocument* doc = editor->document();
  const int pos      = editor->textCursor().position();
  auto charFormatAt   = [&](int p) {
    QTextCursor probe(doc);
    probe.setPosition(p);
    probe.setPosition(p + 1, QTextCursor::KeepAnchor);
    return probe.charFormat();
  };
  if (pos > 0) {
    const QTextCharFormat fmt = charFormatAt(pos - 1);
    if (fmt.isImageFormat())
      return CursorImage{fmt.toImageFormat(), pos - 1};
  }
  if (pos < doc->characterCount() - 1) {
    const QTextCharFormat fmt = charFormatAt(pos);
    if (fmt.isImageFormat())
      return CursorImage{fmt.toImageFormat(), pos};
  }
  return std::nullopt;
}

// The image format's own width()/height() reflect whatever size is currently
// DISPLAYED (0 if never explicitly resized), not the file's real dimensions --
// resizing must always scale from the latter, read straight from disk, or
// repeated resizes would compound against an already-scaled size instead of
// the original.
QSize imageNaturalSize(const App& app, const std::string& profile, const QString& name)
{
  if (profile.empty() || name.isEmpty())
    return {};
  const QString path =
      QString::fromStdString(app.profileInfoDir(profile).string()) + "/" + name;
  const QImage img(path);
  return img.isNull() ? QSize() : img.size();
}

}  // namespace

MainWindow::MainWindow(std::filesystem::path dataDir, QWidget* parent)
    : QMainWindow(parent), m_app(std::move(dataDir))
{
  setWindowTitle("GMM — GTA San Andreas Mod Manager");
  resize(1040, 660);
  setAcceptDrops(true);  // drag folders/archives in to import
  {
    QSettings s;
    m_themeId = s.value("theme", s.value("dark", true).toBool() ? "dark" : "light")
                    .toString();
    m_dark = !theme::isLightId(m_themeId);
  }

  ensureInitialized();
  materializeRuntime();  // unpack the embedded Mod Loader runtime (if any)
  setupFrame();   // frameless window + custom title bar; creates m_menuBar
  buildMenu();    // populates m_menuBar
  buildToolBar();
  buildCentral();
  buildLogPanel();
  lang::translateUi(this);  // flip the static chrome to English if selected
  if (m_titleBar)
    m_titleBar->setTitleText(windowTitle());
  qApp->installEventFilter(this);  // window-edge resize for the frameless window

  // Status bar: a "Logs" toggle on the left, then the game path; counters on the
  // right.
  m_logToggleButton = new QToolButton(this);
  m_logToggleButton->setText(lang::T("Логи ▴"));
  m_logToggleButton->setAutoRaise(true);
  m_logToggleButton->setToolTip(lang::T("Показать/скрыть панель логов"));
  connect(m_logToggleButton, &QToolButton::clicked, this,
          &MainWindow::onToggleLogPanel);
  m_gameLabel   = new QLabel(this);
  m_countLabel  = new QLabel(this);
  m_deployLabel = new QLabel(this);
  statusBar()->addWidget(m_logToggleButton);
  statusBar()->addWidget(m_gameLabel, 1);
  statusBar()->addPermanentWidget(m_countLabel);
  statusBar()->addPermanentWidget(m_deployLabel);

  refreshProfiles();
  populateExecutables();
  updateExternalToolActions();  // SA-MP/Sanny Builder buttons start out hidden
  refreshMods();
  updateStatusBar();

  // Keep the window wide enough that no toolbar row collapses its buttons into
  // the overflow ">>" popup — the window minimum must cover the widest of the
  // two rows. English labels are wider, so compute after translation.
  enforceToolbarWidth();
  setMinimumHeight(440);

  restoreUiState();
  // A previously-saved (narrower) geometry can come back below the new minimum
  // and slip past restoreGeometry's clamp, re-introducing the overflow; re-apply
  // it here and once more on the next event-loop turn when the toolbars are fully
  // laid out and their size hints are final.
  enforceToolbarWidth();
  QTimer::singleShot(0, this, [this] {
    enforceToolbarWidth();
    updateRevealButton();  // show the reveal button if a saved state left it hidden
    if (m_logDock && m_logDock->isVisible())
      onRefreshLogs();     // a saved state may have left the log dock open
  });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  if (m_playRunning) {
    // A play cycle owns a worker thread that still touches this window; closing
    // now would crash on the detached thread. Block until the game exits.
    dialogs::information(this, lang::T("Игра запущена"),
                             lang::T("Дождитесь выхода из игры — лаунчер заблокирован."));
    event->ignore();
    return;
  }
  saveUiState();
  QMainWindow::closeEvent(event);
}

void MainWindow::saveUiState()
{
  QSettings s;
  s.setValue("geometry", saveGeometry());
  s.setValue("windowState", saveState());
  if (m_splitter)
    s.setValue("splitter", m_splitter->saveState());
  if (m_mods)
    s.setValue("headerV2", m_mods->header()->saveState());
}

void MainWindow::restoreUiState()
{
  QSettings s;
  if (s.contains("geometry"))
    restoreGeometry(s.value("geometry").toByteArray());
  if (s.contains("windowState"))
    restoreState(s.value("windowState").toByteArray());
  if (m_splitter && s.contains("splitter"))
    m_splitter->restoreState(s.value("splitter").toByteArray());
  // "headerV2": the column set changed (Type was added), and restoring a
  // layout saved for the old one would put the widths back where they
  // were. A new key retires the stale state instead of migrating it.
  if (m_mods && s.contains("headerV2")) {
    m_mods->header()->restoreState(s.value("headerV2").toByteArray());
    // A saved header state carries the resize MODES too, and older states have
    // Type as ResizeToContents -- which would silently ignore the width
    // autoSizeTagsColumn() computes. Re-assert the modes we own after restore.
    m_mods->header()->setStretchLastSection(false);
    m_mods->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_mods->header()->setSectionResizeMode(ColTags, QHeaderView::Interactive);
  }
}

// --- custom (frameless) window frame ----------------------------------------

void MainWindow::setupFrame()
{
  // Drop the native chrome and supply our own caption so it can follow the theme.
  setWindowFlag(Qt::FramelessWindowHint, true);

  // A frameless window has no OS-drawn edge at all, so it can be hard to tell
  // apart from whatever is behind/around it (especially over a similarly dark
  // desktop). Paint a 3px outline around the whole window, palette-driven so
  // it adapts to the active theme automatically without needing to be
  // re-applied on theme switches. WA_StyledBackground is required for a
  // top-level widget to actually honor a stylesheet border at all (otherwise
  // it just paints the plain palette background, ignoring `border`). The
  // contents margin matches the border width so it actually shows instead of
  // being covered by the menu widget/toolbars/central widget sitting flush
  // against the edge. Every one of the app's own dialogs gets the identical
  // treatment in frameless() below, so all custom-framed windows look the same.
  setObjectName("gmmWindow");
  setAttribute(Qt::WA_StyledBackground, true);
  setStyleSheet("#gmmWindow{border:3px solid palette(mid);background:palette(window);}");
  setContentsMargins(3, 3, 3, 3);

  // Stack the custom title bar above our own menu bar and install the pair as the
  // QMainWindow "menu widget" (the top strip above the toolbars).
  auto* top = new QWidget(this);
  top->setObjectName("frameTop");
  auto* v = new QVBoxLayout(top);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  m_titleBar = new TitleBar(this);
  v->addWidget(m_titleBar);

  m_menuBar = new QMenuBar(top);
  v->addWidget(m_menuBar);

  setMenuWidget(top);
  applyFrameTheme();
}

void MainWindow::applyFrameTheme()
{
  if (!m_titleBar)
    return;
  m_titleBar->setTheme(m_dark);  // high-contrast glyphs for the active theme
  m_titleBar->setStyleSheet(titleBarStyleSheet(m_dark));
}

QVBoxLayout* MainWindow::frameless(QDialog& dlg, bool showMinMax)
{
  // Thin wrapper: the actual chrome-setup logic lives in Frame.h/.cpp
  // (gtamm::applyFrame), shared with the standalone dialogs in gui/Dialogs.*
  // that don't have a MainWindow instance to call back into.
  return applyFrame(dlg, m_dark, showMinMax);
}

void MainWindow::centerDialog(QDialog& dlg)
{
  // Thin wrapper over the shared gtamm::centerOverWidget().
  centerOverWidget(dlg, this);
}

Qt::Edges MainWindow::edgesAt(const QPoint& gp, const QWidget* w) const
{
  const int b = 6;  // grab band in logical px
  const QRect r = w->frameGeometry();  // == geometry() while frameless
  if (!r.contains(gp))
    return {};
  Qt::Edges e;
  if (gp.x() <= r.left() + b)   e |= Qt::LeftEdge;
  if (gp.x() >= r.right() - b)  e |= Qt::RightEdge;
  if (gp.y() <= r.top() + b)    e |= Qt::TopEdge;
  if (gp.y() >= r.bottom() - b) e |= Qt::BottomEdge;
  return e;
}

void MainWindow::updateResizeCursor(Qt::Edges e)
{
  Qt::CursorShape shape;
  const bool l = e & Qt::LeftEdge, rr = e & Qt::RightEdge;
  const bool t = e & Qt::TopEdge, bo = e & Qt::BottomEdge;
  if ((t && l) || (bo && rr))
    shape = Qt::SizeFDiagCursor;
  else if ((t && rr) || (bo && l))
    shape = Qt::SizeBDiagCursor;
  else if (l || rr)
    shape = Qt::SizeHorCursor;
  else if (t || bo)
    shape = Qt::SizeVerCursor;
  else {
    clearResizeCursor();
    return;
  }
  if (!m_cursorOverridden) {
    QApplication::setOverrideCursor(shape);
    m_cursorOverridden = true;
  } else {
    QApplication::changeOverrideCursor(shape);
  }
}

void MainWindow::clearResizeCursor()
{
  if (m_cursorOverridden) {
    QApplication::restoreOverrideCursor();
    m_cursorOverridden = false;
  }
}

bool MainWindow::event(QEvent* e)
{
  // Keep the custom caption in sync with programmatic title / maximize changes.
  if (e->type() == QEvent::WindowTitleChange && m_titleBar)
    m_titleBar->setTitleText(windowTitle());
  else if (e->type() == QEvent::WindowStateChange && m_titleBar)
    m_titleBar->updateMaximizeIcon();
  return QMainWindow::event(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev)
{
  const QEvent::Type t = ev->type();
  if (t == QEvent::MouseMove || t == QEvent::MouseButtonPress) {
    QWidget* w = qobject_cast<QWidget*>(obj);
    // Installed on qApp (see the constructor), so this sees mouse events from
    // every widget in the app -- generalized to any frameless top-level window,
    // not just MainWindow itself, so the app's own QDialogs (framed via
    // frameless()) get the same edge-resize for free just by carrying the
    // Qt::FramelessWindowHint flag.
    QWidget* topWin = w ? w->window() : nullptr;
    const bool resizable =
        topWin && topWin->windowFlags().testFlag(Qt::FramelessWindowHint);
    // Edge-resize works in both normal and maximized state: grabbing an edge of a
    // maximized window restores it and begins the resize (native, with Aero snap).
    if (w && resizable && !topWin->isFullScreen()) {
      auto* me = static_cast<QMouseEvent*>(ev);
      const QPoint gp = me->globalPosition().toPoint();
      if (t == QEvent::MouseMove && me->buttons() == Qt::NoButton) {
        m_hoverEdges = edgesAt(gp, topWin);
        updateResizeCursor(m_hoverEdges);
      } else if (t == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
        const Qt::Edges e = edgesAt(gp, topWin);
        if (e) {
          if (QWindow* h = topWin->windowHandle()) {
            h->startSystemResize(e);  // native resize (supports Aero snap)
            return true;
          }
        }
      }
    } else if (m_cursorOverridden) {
      clearResizeCursor();
    }
  }
  return QMainWindow::eventFilter(obj, ev);
}

// --- construction helpers ---------------------------------------------------

void MainWindow::buildMenu()
{
  QMenu* inst = m_menuBar->addMenu("&Инстанс");
  inst->addAction("&Сменить / управление инстансами…", this,
                  &MainWindow::onSwitchInstance);
  inst->addAction("Сменить папку &игры…", this, &MainWindow::onChangeGameFolder);
  inst->addAction("Открыть папку &игры", this, &MainWindow::onOpenGameFolder);
  inst->addAction("&Открыть папку инстанса", this, &MainWindow::onOpenInstanceFolder);

  QMenu* file = m_menuBar->addMenu("&Файл");
  file->addAction("Импорт &папки…", this, &MainWindow::onImportFolder);
  file->addAction("Импорт &архива…", this, &MainWindow::onImportArchive);
  file->addAction("Импорт &сборки (из папки игры)…", this,
                  &MainWindow::onImportBuild);
  file->addSeparator();
  file->addAction("&Экспорт сборки в zip…", this, &MainWindow::onExportBuild);
  file->addAction("Импорт сборки из &zip…", this, &MainWindow::onImportBuildZip);
  file->addSeparator();
  file->addAction("&Выход", this, &QWidget::close);

  QMenu* profile = m_menuBar->addMenu("&Профиль");
  profile->addAction("&Новый профиль…", this, &MainWindow::onNewProfile);
  profile->addAction("&Переименовать профиль…", this, &MainWindow::onRenameProfile);
  profile->addAction("&Копировать профиль…", this, &MainWindow::onCopyProfile);
  profile->addAction("&Удалить профиль", this, &MainWindow::onDeleteProfile);
  profile->addSeparator();
  profile->addAction("&Менеджер профилей…", this, &MainWindow::onManageProfiles);
  profile->addSeparator();
  profile->addAction("Добавить &разделитель…", this, &MainWindow::onAddSeparator);
  profile->addSeparator();
  profile->addAction("Настройки &SA-MP (мультиплеер)…", this,
                     &MainWindow::onSampSettings);
  profile->addAction("Открыть папку &сохранений профиля", this,
                     &MainWindow::onOpenSavesFolder);
  profile->addAction("Открыть папку созданных &файлов профиля", this,
                     &MainWindow::onOpenGeneratedFolder);

  QMenu* deploy = m_menuBar->addMenu("&Сборка");
  deploy->addAction("&Развернуть", this, &MainWindow::onDeploy);
  deploy->addAction("&Откатить", this, &MainWindow::onRollback);
  deploy->addAction("&Запустить игру", this, &MainWindow::onPlay);

  // All program options now live in one consolidated Settings dialog. A proper
  // dropdown menu here (not a bare QAction directly on the menu bar) so it
  // looks and behaves the same as its neighbors instead of standing out.
  QMenu* settings = m_menuBar->addMenu("⚙ &Настройки");
  settings->addAction("&Открыть настройки…", this, &MainWindow::onSettings);

  QMenu* help = m_menuBar->addMenu("&Справка");
  help->addAction("&О программе", this, &MainWindow::onAbout);
}

std::filesystem::path MainWindow::themesDir() const
{
  // MO2-style: drop .qss theme files next to the exe under "stylesheets/".
  return fs::path(QCoreApplication::applicationDirPath().toStdString()) / "stylesheets";
}

void MainWindow::rebuildThemeMenu()
{
  if (!m_themeMenu)
    return;
  m_themeMenu->clear();
  const auto add = [this](const QString& label, const QString& id) {
    QAction* a = m_themeMenu->addAction(label);
    a->setCheckable(true);
    a->setChecked(m_themeId == id);
    m_themeGroup->addAction(a);
    connect(a, &QAction::triggered, this, [this, id] { applyTheme(id); });
  };
  add(lang::T("Тёмная"), "dark");
  add(lang::T("Светлая"), "light");

  // External MO2 .qss themes from the stylesheets folder.
  std::error_code ec;
  fs::create_directories(themesDir(), ec);
  QDir dir(QString::fromStdString(themesDir().string()));
  const auto files = dir.entryList({"*.qss"}, QDir::Files, QDir::Name);
  if (!files.isEmpty())
    m_themeMenu->addSeparator();
  for (const QString& f : files) {
    const QString path = dir.absoluteFilePath(f);
    add(QFileInfo(f).completeBaseName(), path);
  }

  m_themeMenu->addSeparator();
  m_themeMenu->addAction(lang::T("Открыть папку тем"), this, [this] {
    std::error_code e;
    fs::create_directories(themesDir(), e);
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(themesDir().string())));
  });
  m_themeMenu->addAction(lang::T("Обновить список тем"), this,
                         &MainWindow::rebuildThemeMenu);
}

void MainWindow::applyTheme(const QString& id)
{
  m_themeId = id;
  m_dark    = !theme::isLightId(id);
  theme::applyNamed(*qApp, id);
  QSettings().setValue("theme", id);
  applyFrameTheme();   // restyle the custom caption for the new palette
  rebuildThemeMenu();  // refresh the checkmarks
  // Re-apply theme-dependent colors in the details panel (mod details or build
  // notes), unless the user is mid-edit (don't clobber unsaved text).
  if (m_mods && !m_infoEditing) {
    QTreeWidgetItem* cur = m_mods->currentItem();
    updateDetails(cur ? cur->data(ColCheck, kModIdRole).toString() : QString());
  }
}

QString MainWindow::detailsCss() const
{
  return m_dark
             ? "h2,h3{color:#ffffff;} body,p,td,th,li,b{color:#e6e6e6;} i{color:#9fb6d6;}"
             : "h2,h3{color:#111111;} body,p,td,th,li,b{color:#1e1e1e;} i{color:#3a5f8a;}";
}

void MainWindow::buildToolBar()
{
  const QStyle* s = style();

  // --- row 1: import + deploy (run controls moved to the run panel) ---
  QToolBar* tb2 = addToolBar("Сборка");
  tb2->setMovable(false);
  tb2->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tb2->addAction(s->standardIcon(QStyle::SP_DirOpenIcon), "Импорт папки",
                 this, &MainWindow::onImportFolder);
  tb2->addAction(s->standardIcon(QStyle::SP_DialogOpenButton), "Импорт архива",
                 this, &MainWindow::onImportArchive);
  tb2->addSeparator();
  m_deployAction = tb2->addAction(s->standardIcon(QStyle::SP_DialogApplyButton),
                                  "Развернуть", this, &MainWindow::onDeploy);
  m_deployAction->setShortcut(QKeySequence("Ctrl+D"));
  m_rollbackAction = tb2->addAction(s->standardIcon(QStyle::SP_DialogResetButton),
                                    "Откатить", this, &MainWindow::onRollback);
  tb2->addSeparator();
  // SA-MP and Sanny Builder are shown only when they're actually in use for
  // this profile/instance -- see updateExternalToolActions(), which is what
  // decides their visibility (both start hidden).
  m_sampAction =
      tb2->addAction(QIcon(":/samp.jpg"), "SAMP", this, &MainWindow::onSampSettings);
  m_sannyAction = tb2->addAction(QIcon(":/sanny4.png"), "Sanny Builder", this,
                                 &MainWindow::onLaunchSannyBuilder);
  m_sannyAction->setToolTip("Запустить Sanny Builder");
  // "Add separator" and program settings moved off the toolbar (Profile menu and
  // the ⚙ Settings dialog respectively) to keep only the daily actions here.

  // --- row 2: profile + load-order ---
  addToolBarBreak();
  QToolBar* tb1 = addToolBar("Профиль");
  tb1->setMovable(false);
  tb1->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tb1->addWidget(new QLabel(" Профиль: ", this));
  m_profile = new QComboBox(this);
  m_profile->setMinimumWidth(200);
  tb1->addWidget(m_profile);
  // Profile create/rename/copy/delete live in the Profile menu — the toolbar keeps
  // only the daily actions (switch profile via the combo, reorder load order).
  tb1->addSeparator();
  QAction* up = tb1->addAction(s->standardIcon(QStyle::SP_ArrowUp), "Выше", this,
                               [this] { moveModPriority(-1); });
  up->setToolTip("Повысить приоритет (выше в списке)");
  up->setShortcut(QKeySequence("Ctrl+Shift+Up"));
  QAction* down = tb1->addAction(s->standardIcon(QStyle::SP_ArrowDown), "Ниже", this,
                                 [this] { moveModPriority(1); });
  down->setToolTip("Понизить приоритет");
  down->setShortcut(QKeySequence("Ctrl+Shift+Down"));

  // The executable selector + Run + Shortcut buttons now live in a dedicated
  // panel above the details (see buildCentral / the run panel). A Ctrl+R shortcut
  // for Play is registered window-wide so it works without the toolbar button.
  QAction* play = new QAction("Играть", this);
  play->setShortcut(QKeySequence("Ctrl+R"));
  connect(play, &QAction::triggered, this, &MainWindow::onPlay);
  addAction(play);

  QAction* refresh = new QAction("Обновить", this);
  refresh->setShortcut(QKeySequence("F5"));
  connect(refresh, &QAction::triggered, this, &MainWindow::onRefresh);
  addAction(refresh);

  m_toolBar = tb2;
}

void MainWindow::enforceToolbarWidth()
{
  // The window must be at least as wide as the widest toolbar row, otherwise
  // QToolBar hides the trailing buttons behind the ">>" extension popup. Take
  // the max over both rows and clamp the current width up if it fell below it.
  int tbW = 0;
  for (QToolBar* tb : findChildren<QToolBar*>())
    tbW = std::max(tbW, tb->sizeHint().width());
  const int minW = std::max(520, tbW + 16);
  if (minimumWidth() != minW)
    setMinimumWidth(minW);
  if (width() < minW)
    resize(minW, height());
}

void MainWindow::updateRunIcon()
{
  if (!m_runIcon)
    return;
  const QString exeName  = m_runExe ? m_runExe->currentText() : QString();
  const fs::path gameDir = m_app.gamePath();
  fs::path exe;
  if (exeName.isEmpty())
    exe = gameDir / "gta_sa.exe";
  else if (fs::path(exeName.toStdString()).is_absolute())
    exe = exeName.toStdString();
  else
    exe = gameDir / exeName.toStdString();

  std::error_code ec;
  if (gameDir.empty() || !fs::exists(exe, ec)) {
    m_runIcon->clear();
    m_runIcon->setToolTip(QString());
    return;
  }
  // The system icon for an .exe is its own embedded application icon. Render it
  // at the label's full square size (DPI-aware) so it stays crisp and uncropped.
  const QIcon icon =
      QFileIconProvider().icon(QFileInfo(QString::fromStdString(exe.string())));
  const qreal dpr  = devicePixelRatioF();
  const int side   = 28;  // a little inset inside the 32px square label
  QPixmap pm = icon.pixmap(QSize(side, side), dpr);
  pm.setDevicePixelRatio(dpr);
  m_runIcon->setPixmap(pm);
  m_runIcon->setToolTip(QString::fromStdString(exe.filename().string()));
}

void MainWindow::updateExternalToolActions()
{
  if (m_sampAction) {
    // SA-MP is a per-profile setting, so this follows the active profile: with
    // multiplayer off there is nothing to configure from here and the button is
    // just noise. It stays reachable in the Profile menu, which is how you turn
    // SA-MP back on once the button is gone.
    bool samp = false;
    if (!m_app.activeProfile().empty()) {
      try {
        samp = m_app.sampConfig(m_app.activeProfile()).enabled;
      } catch (const std::exception&) {
        samp = false;  // profile unreadable: treat as off rather than break the toolbar
      }
    }
    m_sampAction->setVisible(samp);
  }
  if (m_sannyAction) {
    // Both halves must hold: the toggle is on AND the configured path really
    // holds a Sanny Builder executable -- a button that only ever reports "not
    // found" would be worse than no button.
    m_sannyAction->setVisible(m_app.showSannyBuilder() && m_app.hasSannyBuilder());
  }
  // Showing a button again makes its row wider, so re-assert the window minimum
  // that keeps toolbar rows out of the ">>" overflow popup.
  enforceToolbarWidth();
}

void MainWindow::onLaunchSannyBuilder()
{
  const fs::path exe = m_app.sannyBuilderExe();
  if (exe.empty()) {
    dialogs::warning(this, lang::T("Sanny Builder"),
                     lang::T("Путь к Sanny Builder не указан или в нём нет "
                             "исполняемого файла. Укажите его в Настройках."));
    return;
  }
  // Push the current instance into Sanny Builder first, so what it sees (game
  // folder, and where compiled scripts go) always matches the instance in front
  // of the user rather than whatever was configured last time.
  sanny::SyncResult sync;
  guarded(lang::T("Sanny Builder"), [&] { sync = m_app.syncSannyBuilder(); });
  if (!sync.notes.empty()) {
    // Not fatal -- SB still starts, just possibly with stale settings.
    QStringList notes;
    for (const std::string& n : sync.notes)
      notes << QString::fromStdString(n);
    statusBar()->showMessage(lang::T("Sanny Builder: ") + notes.join("; "), 8000);
  }

  // Detached and with its own folder as the working directory: Sanny Builder
  // loads data/, lang/ and lib/ relative to itself, and GMM has no reason to
  // wait around for an editor the user may keep open all day.
  const QString program = QString::fromStdString(exe.string());
  const QString workDir = QString::fromStdString(exe.parent_path().string());
  if (!QProcess::startDetached(program, {}, workDir)) {
    dialogs::warning(this, lang::T("Sanny Builder"),
                     lang::T("Не удалось запустить Sanny Builder."));
    return;
  }
  statusBar()->showMessage(lang::T("Sanny Builder запущен."), 4000);
}

void MainWindow::populateExecutables()
{
  if (!m_runExe)
    return;
  // A checkbox toggle or reorder can call this repeatedly (pendingRootExecutables()
  // depends on the current deploy plan, not just disk) -- preserve whatever the
  // user had picked instead of silently snapping back to gta_sa.exe each time.
  const QString previous = m_runExe->currentText();
  m_runExe->clear();
  std::error_code ec;
  const fs::path game = m_app.gamePath();
  if (m_app.gamePath().empty() || !fs::exists(game, ec)) {
    updateRunIcon();
    return;
  }
  std::set<std::string> seen;  // lowercased, so an already-deployed exe isn't listed twice
  auto addExe = [&](const std::string& name) {
    std::string lc = name;
    for (char& c : lc)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (seen.insert(lc).second)
      m_runExe->addItem(QString::fromStdString(name));
  };
  for (const auto& de : fs::directory_iterator(game, ec))
    if (de.is_regular_file(ec) &&
        QString::fromStdString(de.path().extension().string()).toLower() == ".exe")
      addExe(de.path().filename().string());
  // A portable build can carry its own exe (e.g. samp.exe) as a mod's files
  // rather than something pre-installed by hand -- offer it as a run choice
  // even before Deploy has physically put it in the game folder.
  for (const auto& name : m_app.pendingRootExecutables())
    addExe(name);
  int idx = previous.isEmpty() ? -1 : m_runExe->findText(previous, Qt::MatchFixedString);
  if (idx < 0)
    idx = m_runExe->findText("gta_sa.exe", Qt::MatchFixedString);
  if (idx >= 0)
    m_runExe->setCurrentIndex(idx);
  updateRunIcon();
}

void MainWindow::buildCentral()
{
  m_splitter   = new QSplitter(Qt::Horizontal, this);
  QSplitter* splitter = m_splitter;

  // --- left: filter + sort controls + mod table ---
  auto* left   = new QWidget(splitter);
  auto* lv     = new QVBoxLayout(left);
  lv->setContentsMargins(0, 0, 0, 0);

  auto* topRow = new QHBoxLayout();
  topRow->setContentsMargins(0, 0, 0, 0);
  m_filter = new QLineEdit(left);
  m_filter->setPlaceholderText("Фильтр модов…");
  m_filter->setClearButtonEnabled(true);
  topRow->addWidget(m_filter, 1);

  topRow->addWidget(new QLabel(lang::T("Сортировка:"), left));
  m_sortCombo = new QComboBox(left);
  // userData = the column the field maps to; ColPrio means the manual load order.
  m_sortCombo->addItem(lang::T("Порядок загрузки"), int(ColPrio));
  m_sortCombo->addItem(lang::T("Название"), int(ColName));
  m_sortCombo->addItem(lang::T("Тип"), int(ColTags));
  m_sortCombo->addItem(lang::T("Кол-во файлов"), int(ColFiles));
  m_sortCombo->addItem(lang::T("Конфликты"), int(ColConflict));
  m_sortCombo->addItem("Id", int(ColId));
  m_sortCombo->setToolTip(lang::T("Поле сортировки списка модов"));
  topRow->addWidget(m_sortCombo);

  m_sortDirButton = new QToolButton(left);
  m_sortDirButton->setText("▲");
  m_sortDirButton->setToolTip(lang::T("Направление сортировки (по возр./убыв.)"));
  m_sortDirButton->setAutoRaise(true);
  topRow->addWidget(m_sortDirButton);
  lv->addLayout(topRow);

  connect(m_sortCombo, &QComboBox::currentIndexChanged, this,
          &MainWindow::onSortComboChanged);
  connect(m_sortDirButton, &QToolButton::clicked, this,
          &MainWindow::onSortDirToggled);

  auto* tree = new ModTree(left);
  tree->afterDrop = [this] { onRowsMoved(); };  // persist a drag-reorder
  m_mods = tree;
  m_mods->setColumnCount(ColCount);
  m_mods->setHeaderLabels(
      {"", "#", lang::T("Мод"), lang::T("Тип"), "!", lang::T("Файлы"), "Id"});
  m_mods->setRootIsDecorated(false);
  m_mods->setUniformRowHeights(true);
  m_mods->setAlternatingRowColors(true);
  m_mods->setSelectionMode(QAbstractItemView::SingleSelection);
  m_mods->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_mods->setDragEnabled(true);
  m_mods->setAcceptDrops(true);
  m_mods->setDragDropMode(QAbstractItemView::InternalMove);
  m_mods->setContextMenuPolicy(Qt::CustomContextMenu);
  m_mods->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
  // The mod name is the only column that should soak up spare width. Qt
  // stretches the LAST section as well by default, which made Id -- a short
  // technical string -- the widest column in the table.
  m_mods->header()->setStretchLastSection(false);
  m_mods->header()->resizeSection(ColCheck, 28);
  m_mods->header()->resizeSection(ColPrio, 40);
  // Type: width is computed by autoSizeTagsColumn() from the widest row in the
  // whole list. ResizeToContents was not enough -- QTreeView measures only the
  // rows currently scrolled into view, so a mod carrying several badges further
  // down the list still came out clipped. The room comes out of the mod name,
  // the only stretching column.
  m_mods->header()->setSectionResizeMode(ColTags, QHeaderView::Interactive);
  // Content-type badges (+ the pencil that opens a Lua mod's script editor).
  auto* tagDelegate =
      new ModTagDelegate(kTagsRole, kModIdRole, kLuaEditRole, this);
  connect(tagDelegate, &ModTagDelegate::editLuaRequested, this,
          &MainWindow::onEditLuaFromTag);
  m_mods->setItemDelegateForColumn(ColTags, tagDelegate);
  m_mods->header()->resizeSection(ColConflict, 28);
  m_mods->header()->resizeSection(ColFiles, 56);
  m_mods->header()->resizeSection(ColId, 56);  // as narrow as the file count
  lv->addWidget(m_mods, 1);

  // --- right: a run panel on top + a (smaller) details panel below ---
  auto* right = new QWidget(splitter);
  auto* rv    = new QVBoxLayout(right);
  rv->setContentsMargins(0, 0, 0, 0);
  rv->setSpacing(6);

  // Run panel (MO2-style): the executable selector on the left, a big "Run"
  // button on the right, with a "Shortcut" button beneath it.
  auto* runPanel = new QWidget(right);
  runPanel->setObjectName("runPanel");
  runPanel->setStyleSheet(
      "#runPanel{border:1px solid palette(mid);border-radius:5px;}");
  auto* runGrid = new QGridLayout(runPanel);
  runGrid->setContentsMargins(8, 8, 8, 8);
  runGrid->setHorizontalSpacing(8);
  runGrid->setVerticalSpacing(6);

  auto* exeBox = new QWidget(runPanel);
  auto* exeLay = new QHBoxLayout(exeBox);
  exeLay->setContentsMargins(0, 0, 0, 0);
  exeLay->setSpacing(6);
  m_runIcon = new QLabel(exeBox);  // icon of the exe that will be launched
  m_runIcon->setFixedSize(32, 32);  // square box so the exe icon isn't cropped
  m_runIcon->setAlignment(Qt::AlignCenter);
  exeLay->addWidget(m_runIcon);
  m_runExe = new QComboBox(exeBox);
  m_runExe->setMinimumWidth(150);
  m_runExe->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  exeLay->addWidget(m_runExe, 1);
  connect(m_runExe, &QComboBox::currentTextChanged, this,
          [this](const QString&) { updateRunIcon(); });
  runGrid->addWidget(exeBox, 0, 0, 2, 1);

  m_runButton = new QPushButton(greenPlayIcon(), "Запуск", runPanel);
  m_runButton->setIconSize(QSize(22, 22));
  m_runButton->setMinimumHeight(40);
  m_runButton->setMinimumWidth(120);
  m_runButton->setToolTip("Развернуть → запустить игру → откат после выхода (Ctrl+R)");
  connect(m_runButton, &QPushButton::clicked, this, &MainWindow::onPlay);
  runGrid->addWidget(m_runButton, 0, 1);

  m_shortcutButton = new QPushButton(
      style()->standardIcon(QStyle::SP_DesktopIcon), "Ярлык", runPanel);
  m_shortcutButton->setToolTip(
      "Создать ярлык на рабочем столе, который сразу запускает игру этого "
      "инстанса (текущий активный профиль), без открытия менеджера");
  connect(m_shortcutButton, &QPushButton::clicked, this,
          &MainWindow::onCreateShortcut);
  runGrid->addWidget(m_shortcutButton, 1, 1);

  runGrid->setColumnStretch(0, 1);
  rv->addWidget(runPanel);

  // --- details tabs (kept compact: the run panel sits above) ---
  auto* tabs = new QTabWidget(right);

  // Info tab: a button row (edit build notes / insert image / save / cancel),
  // a rendered view (mod details OR the profile's build notes) and, while
  // editing, a WYSIWYG rich-text editor + its formatting toolbar. Notes are
  // stored as HTML (QTextEdit's native format) rather than Markdown, since
  // Qt's Markdown renderer chokes on raw HTML blocks (e.g. centered images)
  // and inline code spans next to punctuation.
  auto* infoTab = new QWidget(tabs);
  auto* infoLay = new QVBoxLayout(infoTab);
  infoLay->setContentsMargins(0, 0, 0, 0);
  infoLay->setSpacing(4);

  auto* infoBtns = new QHBoxLayout();
  infoBtns->setContentsMargins(4, 4, 4, 0);
  m_infoEditButton      = new QPushButton("Редактировать сборку", infoTab);
  m_infoInsertImgButton = new QPushButton("Вставить изображение…", infoTab);
  m_infoSaveButton      = new QPushButton("Сохранить", infoTab);
  m_infoCancelButton    = new QPushButton("Отмена", infoTab);
  // Shown instead of m_infoEditButton when a mod (not the build) is selected.
  m_modDescButton = new QPushButton(lang::T("Редактировать описание…"), infoTab);
  m_modDescButton->setVisible(false);
  connect(m_modDescButton, &QPushButton::clicked, this,
          &MainWindow::onEditModDescription);
  infoBtns->addWidget(m_infoEditButton);
  infoBtns->addWidget(m_modDescButton);
  infoBtns->addWidget(m_infoInsertImgButton);
  infoBtns->addStretch(1);
  infoBtns->addWidget(m_infoSaveButton);
  infoBtns->addWidget(m_infoCancelButton);
  infoLay->addLayout(infoBtns);

  // Formatting toolbar for the editor below: bold/italic/underline, paragraph
  // alignment (also centers images, since they're inline objects within a
  // paragraph), font size. Mirrors Qt's own Rich Text Editor example pattern
  // (QTextEdit::mergeCurrentCharFormat applies to the selection, or to text
  // typed next if there's no selection).
  m_infoFormatBar = new QToolBar(infoTab);
  m_infoFormatBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  m_infoFormatBar->setContentsMargins(4, 0, 4, 0);

  m_infoBoldAction = m_infoFormatBar->addAction("Ж");
  m_infoBoldAction->setCheckable(true);
  m_infoBoldAction->setToolTip("Жирный (Ctrl+B)");
  m_infoBoldAction->setShortcut(QKeySequence::Bold);
  {
    QFont f = m_infoFormatBar->widgetForAction(m_infoBoldAction)->font();
    f.setBold(true);
    m_infoFormatBar->widgetForAction(m_infoBoldAction)->setFont(f);
  }

  m_infoItalicAction = m_infoFormatBar->addAction("К");
  m_infoItalicAction->setCheckable(true);
  m_infoItalicAction->setToolTip("Курсив (Ctrl+I)");
  m_infoItalicAction->setShortcut(QKeySequence::Italic);
  {
    QFont f = m_infoFormatBar->widgetForAction(m_infoItalicAction)->font();
    f.setItalic(true);
    m_infoFormatBar->widgetForAction(m_infoItalicAction)->setFont(f);
  }

  m_infoUnderlineAction = m_infoFormatBar->addAction("Ч");
  m_infoUnderlineAction->setCheckable(true);
  m_infoUnderlineAction->setToolTip("Подчёркнутый (Ctrl+U)");
  m_infoUnderlineAction->setShortcut(QKeySequence::Underline);
  {
    QFont f = m_infoFormatBar->widgetForAction(m_infoUnderlineAction)->font();
    f.setUnderline(true);
    m_infoFormatBar->widgetForAction(m_infoUnderlineAction)->setFont(f);
  }

  m_infoFormatBar->addSeparator();

  auto* alignGroup = new QActionGroup(this);
  alignGroup->setExclusive(true);
  m_infoAlignLeftAction = m_infoFormatBar->addAction("Слева");
  m_infoAlignLeftAction->setCheckable(true);
  m_infoAlignLeftAction->setChecked(true);
  m_infoAlignLeftAction->setToolTip("По левому краю");
  alignGroup->addAction(m_infoAlignLeftAction);

  m_infoAlignCenterAction = m_infoFormatBar->addAction("Центр");
  m_infoAlignCenterAction->setCheckable(true);
  m_infoAlignCenterAction->setToolTip("По центру");
  alignGroup->addAction(m_infoAlignCenterAction);

  m_infoAlignRightAction = m_infoFormatBar->addAction("Справа");
  m_infoAlignRightAction->setCheckable(true);
  m_infoAlignRightAction->setToolTip("По правому краю");
  alignGroup->addAction(m_infoAlignRightAction);

  m_infoAlignJustifyAction = m_infoFormatBar->addAction("Ширина");
  m_infoAlignJustifyAction->setCheckable(true);
  m_infoAlignJustifyAction->setToolTip("По ширине");
  alignGroup->addAction(m_infoAlignJustifyAction);

  m_infoFormatBar->addSeparator();

  m_infoFontSizeCombo = new QComboBox(m_infoFormatBar);
  m_infoFontSizeCombo->setEditable(true);
  m_infoFontSizeCombo->setInsertPolicy(QComboBox::NoInsert);
  m_infoFontSizeCombo->setMaximumWidth(64);
  m_infoFontSizeCombo->setToolTip("Размер текста");
  for (int sz : {8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 64})
    m_infoFontSizeCombo->addItem(QString::number(sz));
  m_infoFontSizeCombo->setCurrentText("12");
  m_infoFormatBar->addWidget(m_infoFontSizeCombo);

  m_infoFormatBar->addSeparator();

  m_infoImageSizeSpin = new QSpinBox(m_infoFormatBar);
  m_infoImageSizeSpin->setRange(5, 400);
  m_infoImageSizeSpin->setSingleStep(10);
  m_infoImageSizeSpin->setSuffix("%");
  m_infoImageSizeSpin->setMaximumWidth(72);
  m_infoImageSizeSpin->setToolTip(
      lang::T("Размер изображения под курсором (% от исходного файла)"));
  m_infoImageSizeSpin->setEnabled(false);  // enabled only while on an image
  m_infoFormatBar->addWidget(m_infoImageSizeSpin);

  infoLay->addWidget(m_infoFormatBar);

  m_info = new QTextBrowser(infoTab);
  m_info->setOpenExternalLinks(true);
  m_info->document()->setDefaultStyleSheet(detailsCss());
  infoLay->addWidget(m_info, 1);

  m_infoEditor = new QTextEdit(infoTab);
  m_infoEditor->setAcceptRichText(true);
  m_infoEditor->setPlaceholderText(
      "Описание сборки. Наберите текст и оформите его тулбаром выше "
      "(жирный/курсив/подчёркнутый, выравнивание, размер), либо вставьте "
      "изображение кнопкой «Вставить изображение…».");
  infoLay->addWidget(m_infoEditor, 1);

  connect(m_infoEditButton, &QPushButton::clicked, this,
          &MainWindow::onEditBuildInfo);
  connect(m_infoInsertImgButton, &QPushButton::clicked, this,
          &MainWindow::onInsertBuildImage);
  connect(m_infoSaveButton, &QPushButton::clicked, this,
          &MainWindow::onSaveBuildInfo);
  connect(m_infoCancelButton, &QPushButton::clicked, this,
          &MainWindow::onCancelBuildInfo);

  connect(m_infoBoldAction, &QAction::toggled, this, [this](bool checked) {
    QTextCharFormat fmt;
    fmt.setFontWeight(checked ? QFont::Bold : QFont::Normal);
    m_infoEditor->mergeCurrentCharFormat(fmt);
    m_infoEditor->setFocus();
  });
  connect(m_infoItalicAction, &QAction::toggled, this, [this](bool checked) {
    QTextCharFormat fmt;
    fmt.setFontItalic(checked);
    m_infoEditor->mergeCurrentCharFormat(fmt);
    m_infoEditor->setFocus();
  });
  connect(m_infoUnderlineAction, &QAction::toggled, this, [this](bool checked) {
    QTextCharFormat fmt;
    fmt.setFontUnderline(checked);
    m_infoEditor->mergeCurrentCharFormat(fmt);
    m_infoEditor->setFocus();
  });
  connect(m_infoAlignLeftAction, &QAction::triggered, this, [this] {
    m_infoEditor->setAlignment(Qt::AlignLeft);
    m_infoEditor->setFocus();
  });
  connect(m_infoAlignCenterAction, &QAction::triggered, this, [this] {
    m_infoEditor->setAlignment(Qt::AlignHCenter);
    m_infoEditor->setFocus();
  });
  connect(m_infoAlignRightAction, &QAction::triggered, this, [this] {
    m_infoEditor->setAlignment(Qt::AlignRight);
    m_infoEditor->setFocus();
  });
  connect(m_infoAlignJustifyAction, &QAction::triggered, this, [this] {
    m_infoEditor->setAlignment(Qt::AlignJustify);
    m_infoEditor->setFocus();
  });
  connect(m_infoFontSizeCombo, &QComboBox::currentTextChanged, this,
          [this](const QString& text) {
            bool ok = false;
            const double pt = text.toDouble(&ok);
            if (!ok || pt <= 0 || !m_infoEditor)
              return;
            QTextCharFormat fmt;
            fmt.setFontPointSize(pt);
            m_infoEditor->mergeCurrentCharFormat(fmt);
          });
  // Reflect the cursor's current formatting back onto the toolbar (bold
  // button pressed when the caret is inside bold text, etc.) without
  // re-triggering the actions above.
  connect(m_infoEditor, &QTextEdit::currentCharFormatChanged, this,
          [this](const QTextCharFormat& fmt) {
            const QSignalBlocker b1(m_infoBoldAction);
            const QSignalBlocker b2(m_infoItalicAction);
            const QSignalBlocker b3(m_infoUnderlineAction);
            m_infoBoldAction->setChecked(fmt.fontWeight() > QFont::Normal);
            m_infoItalicAction->setChecked(fmt.fontItalic());
            m_infoUnderlineAction->setChecked(fmt.fontUnderline());
            if (fmt.fontPointSize() > 0) {
              const QSignalBlocker b4(m_infoFontSizeCombo);
              m_infoFontSizeCombo->setCurrentText(
                  QString::number(int(fmt.fontPointSize())));
            }
          });
  connect(m_infoEditor, &QTextEdit::cursorPositionChanged, this, [this] {
    QAction* a = m_infoAlignLeftAction;
    switch (m_infoEditor->alignment()) {
      case Qt::AlignHCenter: a = m_infoAlignCenterAction; break;
      case Qt::AlignRight:   a = m_infoAlignRightAction; break;
      case Qt::AlignJustify: a = m_infoAlignJustifyAction; break;
      default:               a = m_infoAlignLeftAction; break;
    }
    const QSignalBlocker b(a);
    a->setChecked(true);

    // Reflect the image the caret is on/next to (if any) in the size spinner,
    // as a percentage of its real on-disk size.
    const QSignalBlocker bs(m_infoImageSizeSpin);
    const auto img = imageFormatAtCursor(m_infoEditor);
    m_infoImageSizeSpin->setEnabled(img.has_value());
    int pct = 100;
    if (img && img->format.width() > 0) {
      const QSize natural = imageNaturalSize(m_app, m_app.activeProfile(), img->format.name());
      if (natural.width() > 0)
        pct = qRound(100.0 * img->format.width() / natural.width());
    }
    m_infoImageSizeSpin->setValue(pct);
  });
  connect(m_infoImageSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int percent) {
            const auto img = imageFormatAtCursor(m_infoEditor);
            if (!img)
              return;
            const QSize natural =
                imageNaturalSize(m_app, m_app.activeProfile(), img->format.name());
            if (natural.width() <= 0 || natural.height() <= 0)
              return;
            QTextImageFormat fmt = img->format;
            fmt.setWidth(natural.width() * percent / 100.0);
            fmt.setHeight(natural.height() * percent / 100.0);
            QTextCursor cur(m_infoEditor->document());
            cur.setPosition(img->start);
            cur.setPosition(img->start + 1, QTextCursor::KeepAnchor);
            cur.setCharFormat(fmt);
            m_infoEditor->setFocus();
          });

  m_conflicts = new QTextBrowser(tabs);
  m_conflicts->document()->setDefaultStyleSheet(detailsCss());
  tabs->addTab(infoTab, "Инфо");
  tabs->addTab(m_conflicts, "Конфликты");
  rv->addWidget(tabs, 1);

  splitter->addWidget(left);
  splitter->addWidget(right);
  splitter->setStretchFactor(0, 4);  // give the mod list the room; info is smaller
  splitter->setStretchFactor(1, 2);
  setCentralWidget(splitter);

  // If the user collapses the run/info side panel to zero width (it stays
  // collapsible so it can be hidden on purpose), a floating edge button appears
  // to bring it back — otherwise it can't be grabbed again.
  m_revealPanelButton = new QToolButton(this);
  m_revealPanelButton->setText("◀");
  m_revealPanelButton->setToolTip(lang::T("Показать панель запуска"));
  m_revealPanelButton->setCursor(Qt::PointingHandCursor);
  m_revealPanelButton->hide();
  connect(m_revealPanelButton, &QToolButton::clicked, this,
          &MainWindow::onRevealPanel);
  connect(m_splitter, &QSplitter::splitterMoved, this,
          [this](int, int) { updateRevealButton(); });

  connect(m_profile, &QComboBox::currentIndexChanged, this,
          &MainWindow::onProfileChanged);
  connect(m_mods, &QTreeWidget::itemChanged, this, &MainWindow::onListEdited);
  // A drag-reorder renumbers priority and persists — handled in ModTree::dropEvent
  // (QTreeWidget does not emit rowsMoved for internal moves) via afterDrop above,
  // kept apart from checkbox edits so a sorted view never corrupts the load order.
  // Clicking a header sorts the view (by name / files / id / conflicts); clicking
  // the "#" header returns to the manual priority order with drag-reorder.
  m_mods->header()->setSectionsClickable(true);
  m_mods->header()->setSortIndicatorShown(true);
  connect(m_mods->header(), &QHeaderView::sectionClicked, this,
          &MainWindow::onHeaderClicked);
  connect(m_mods, &QTreeWidget::currentItemChanged, this,
          &MainWindow::onSelectionChanged);
  connect(m_mods, &QTreeWidget::customContextMenuRequested, this,
          &MainWindow::onModContextMenu);
  connect(m_mods, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* it, int) {
            if (!it || it->data(ColCheck, kIsSepRole).toBool())
              return;
            const fs::path folder =
                m_app.modsDir() /
                it->data(ColCheck, kModIdRole).toString().toStdString() / "root";
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QString::fromStdString(folder.string())));
          });
  connect(m_filter, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);
}

// --- helpers ----------------------------------------------------------------

void MainWindow::guarded(const QString& context, const std::function<void()>& fn)
{
  if (m_playRunning) {
    // Launcher is busy while the game runs (it still owns the deploy/saves swap).
    statusBar()->showMessage(
        lang::T("Дождитесь выхода из игры — действие недоступно"), 3000);
    return;
  }
  try {
    fn();
  } catch (const std::exception& e) {
    dialogs::warning(this, context, QString::fromLocal8Bit(e.what()));
  }
}

void MainWindow::setPlayLock(bool locked)
{
  if (locked) {
    if (!m_playOverlay) {
      m_playOverlay = new QWidget(this);
      m_playOverlay->setObjectName("playOverlay");
      m_playOverlay->setStyleSheet(
          "#playOverlay{background:rgba(8,9,11,170);}"
          "#playCard{background:rgba(26,28,33,238);"
          "border:1px solid rgba(255,255,255,0.10);border-radius:12px;}"
          "#playCard QLabel{background:transparent;color:#f2f2f2;}"
          "#playTitle{font-size:18px;font-weight:bold;}"
          "#playHint{color:#aab4c2;font-size:12px;}"
          "#playCard QPushButton{background:#2e9e4f;color:#ffffff;border:0;"
          "border-radius:7px;padding:9px 28px;font-size:14px;}"
          "#playCard QPushButton:hover{background:#37b85d;}"
          "#playCard QPushButton#killGameButton{background:#a8332c;}"
          "#playCard QPushButton#killGameButton:hover{background:#c53a32;}");
      auto* outer = new QVBoxLayout(m_playOverlay);
      outer->setAlignment(Qt::AlignCenter);

      auto* card = new QFrame(m_playOverlay);
      card->setObjectName("playCard");
      card->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
      auto* lay = new QVBoxLayout(card);
      lay->setContentsMargins(34, 28, 34, 28);
      lay->setSpacing(14);
      lay->setAlignment(Qt::AlignCenter);

      auto* title = new QLabel(lang::T("▶  Игра запущена"), card);
      title->setObjectName("playTitle");
      title->setAlignment(Qt::AlignCenter);
      lay->addWidget(title, 0, Qt::AlignCenter);

      auto* hint = new QLabel(
          lang::T("Лаунчер заблокирован до выхода из игры.\nОкно можно перемещать, "
                  "а логи — смотреть внизу."),
          card);
      hint->setObjectName("playHint");
      hint->setAlignment(Qt::AlignCenter);
      lay->addWidget(hint, 0, Qt::AlignCenter);

      auto* unlock = new QPushButton(lang::T("Разблокировать"), card);
      unlock->setCursor(Qt::PointingHandCursor);
      unlock->setToolTip(
          lang::T("Пользоваться менеджером, пока игра запущена (изменения станут "
                  "доступны после выхода из игры)"));
      connect(unlock, &QPushButton::clicked, this, &MainWindow::onUnlockPlay);
      lay->addWidget(unlock, 0, Qt::AlignCenter);

      auto* kill = new QPushButton(lang::T("Убить игру"), card);
      kill->setObjectName("killGameButton");
      kill->setCursor(Qt::PointingHandCursor);
      kill->setToolTip(
          lang::T("Принудительно завершить процесс игры (несохранённый прогресс "
                  "будет потерян)"));
      connect(kill, &QPushButton::clicked, this, &MainWindow::onKillGame);
      lay->addWidget(kill, 0, Qt::AlignCenter);

      outer->addWidget(card, 0, Qt::AlignCenter);
    }
    // Cover only the central (mod/run) area — the mutation surface — so the title
    // bar stays draggable (move the window) and the log dock stays usable.
    if (auto* cw = centralWidget())
      m_playOverlay->setGeometry(cw->geometry());
    m_playOverlay->show();
    m_playOverlay->raise();
  } else if (m_playOverlay) {
    m_playOverlay->hide();
  }

  // Disable the mutation surfaces (their shortcuts could race the worker thread's
  // access to m_app), but leave the title bar, the status bar and the log dock
  // alive so the window can be moved and logs watched while the game runs.
  if (auto* mb = m_menuBar)
    mb->setEnabled(!locked);
  if (auto* cw = centralWidget())
    cw->setEnabled(!locked);
  for (QToolBar* tb : findChildren<QToolBar*>())
    tb->setEnabled(!locked);
  updateRevealButton();  // hide while locked / restore afterwards
}

void MainWindow::onUnlockPlay()
{
  if (!m_playRunning)
    return;
  // MO2-style unlock: dismiss the cover and let the user browse the manager while
  // the game keeps running. The worker thread still owns the deploy/saves state
  // and will roll back on exit, so state-changing actions stay gated (guarded()
  // refuses them with a hint); any stray visual edits heal on the exit refresh.
  if (m_playOverlay)
    m_playOverlay->hide();
  if (auto* mb = m_menuBar)
    mb->setEnabled(true);
  if (auto* cw = centralWidget())
    cw->setEnabled(true);
  for (QToolBar* tb : findChildren<QToolBar*>())
    tb->setEnabled(true);
  if (m_mods)
    m_mods->setDragEnabled(false);  // reordering can't persist until the game exits
  statusBar()->showMessage(
      lang::T("Игра запущена в фоне — изменения станут доступны после выхода из игры"),
      0);
}

void MainWindow::onKillGame()
{
  if (!m_playRunning)
    return;
  if (dialogs::question(
          this, lang::T("Убить игру"),
          lang::T("Принудительно завершить процесс игры?") + "\n\n" +
              lang::T("Несохранённый прогресс будет потерян. Лаунчер сам развернёт "
                      "откат после завершения процесса, как при обычном выходе из игры."),
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
    return;
  // The actual process is only known to the worker thread's call stack
  // (Process.cpp tracks it in a global atomic set right before it blocks in
  // WaitForSingleObject); this just signals it to die early and unblock that
  // wait, the same as if the player had closed the game window themselves --
  // the existing deploy->launch->wait->rollback flow on the worker thread
  // picks up from there unchanged.
  if (!killRunningGame())
    statusBar()->showMessage(
        lang::T("Не удалось завершить процесс игры (возможно, он уже закрылся)"), 6000);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
  QMainWindow::resizeEvent(event);
  if (m_playOverlay && m_playOverlay->isVisible() && centralWidget())
    m_playOverlay->setGeometry(centralWidget()->geometry());
  updateRevealButton();
}

void MainWindow::ensureInitialized()
{
  if (!m_app.isInitialized()) {
    const QString dir =
        QFileDialog::getExistingDirectory(this, lang::T("Выберите папку GTA San Andreas"));
    guarded(lang::T("Инициализация"), [&] { m_app.init(dir.toStdString()); });
  }
  // Make sure the instance always has at least one profile to work with. New
  // instances get "Default" from init(); this also covers older instances that
  // were created before that behaviour existed.
  if (m_app.isInitialized() && m_app.profileNames().empty()) {
    guarded(lang::T("Инициализация"), [&] {
      m_app.createProfile("Default");
      m_app.useProfile("Default");
    });
  }
}

void MainWindow::refreshProfiles()
{
  m_populating = true;
  m_profile->clear();
  for (const auto& n : m_app.profileNames())
    m_profile->addItem(QString::fromStdString(n));
  const int idx = m_profile->findText(QString::fromStdString(m_app.activeProfile()));
  if (idx >= 0)
    m_profile->setCurrentIndex(idx);
  m_populating = false;
}

void MainWindow::addModRow(const std::string& id, const std::string& name,
                           bool enabled, bool viaModloader)
{
  auto* item = new ModItem(m_mods);
  item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                 Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
  item->setCheckState(ColCheck, enabled ? Qt::Checked : Qt::Unchecked);
  item->setText(ColId, QString::fromStdString(id));
  item->setToolTip(ColId, QString::fromStdString(id));  // column is narrow; ids elide
  item->setData(ColCheck, kModIdRole, QString::fromStdString(id));
  item->setData(ColCheck, kIsSepRole, false);
  const auto files = listModFiles(m_app.modsDir() / id / "root");
  item->setText(ColFiles, QString::number(files.size()));

  // What the mod actually contains (CLEO / ASI / Lua / Mod Loader / data /
  // other), painted as coloured badges by ModTagDelegate. This replaces the
  // old "(LUA)" suffix glued onto the mod's name: the name stays the name,
  // and every other content type is now visible at a glance too.
  QStringList tags;
  for (rules::ModType t : rules::modTypes(files, viaModloader))
    tags << QString::fromLatin1(rules::modTypeId(t));
  item->setData(ColTags, kTagsRole, tags);
  item->setData(ColTags, kModIdRole, QString::fromStdString(id));
  item->setData(ColTags, kLuaEditRole, !luaScriptsOf(files).isEmpty());
  // Not drawn (the delegate paints badges instead) -- it exists so that
  // sorting by this column groups mods of the same type together.
  item->setText(ColTags, tags.join(','));

  item->setText(ColName, QString::fromStdString(name));
  item->setTextAlignment(ColFiles, Qt::AlignCenter);
  item->setTextAlignment(ColPrio, Qt::AlignCenter);
  item->setTextAlignment(ColConflict, Qt::AlignCenter);
}

void MainWindow::styleSeparatorItem(QTreeWidgetItem* item, const QString& label)
{
  item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);
  item->setData(ColCheck, kIsSepRole, true);
  item->setData(ColCheck, kModIdRole, QString());
  item->setText(ColName, label);
  for (int c = 0; c < ColCount; ++c) {
    item->setBackground(c, QBrush(QColor(0x2f, 0x3a, 0x52)));
    item->setForeground(c, QBrush(QColor(0xcf, 0xdc, 0xf2)));
  }
  QFont f = item->font(ColName);
  f.setBold(true);
  item->setFont(ColName, f);
}

void MainWindow::refreshMods()
{
  m_populating = true;
  // A full refresh returns to the manual priority order with drag-reorder; any
  // column sort the user had applied is a transient browse aid.
  m_sortColumn = -1;
  m_mods->header()->setSortIndicator(-1, Qt::AscendingOrder);
  m_mods->setDragEnabled(true);
  m_mods->clear();

  // Render the profile's explicit order (top = highest priority), interleaving
  // separators, then append pool mods not yet in the profile.
  std::set<std::string> seen;
  if (!m_app.activeProfile().empty()) {
    auto entries = m_app.loadProfile(m_app.activeProfile()).entries;
    std::sort(entries.begin(), entries.end(),
              [](const ProfileEntry& a, const ProfileEntry& b) {
                return a.priority > b.priority;
              });
    std::map<std::string, Mod> pool;
    for (const auto& m : m_app.mods())
      pool[m.id] = m;
    for (const auto& e : entries) {
      if (e.separator) {
        styleSeparatorItem(new ModItem(m_mods), QString::fromStdString(e.label));
      } else {
        auto it = pool.find(e.modId);
        if (it == pool.end())
          continue;  // entry for a removed mod
        addModRow(e.modId, it->second.name, e.enabled, it->second.viaModloader);
        seen.insert(e.modId);
      }
    }
  }
  // New pool mods not referenced by the profile yet (disabled, at the bottom).
  std::vector<Mod> extra;
  for (const auto& m : m_app.mods())
    if (seen.count(m.id) == 0)
      extra.push_back(m);
  std::sort(extra.begin(), extra.end(),
            [](const Mod& a, const Mod& b) { return a.name < b.name; });
  for (const auto& m : extra)
    addModRow(m.id, m.name, false, m.viaModloader);

  renumberPriorities();
  m_populating = false;

  updateDerivedColumns();
  autoSizeTagsColumn();
  applyFilter();
  updateStatusBar();
  syncSortControls();  // a refresh returns to the manual load-order view
  // Start on the build overview (no mod selected) so the Info tab shows the
  // profile's notes; selecting a mod switches it to that mod's details.
  m_mods->setCurrentItem(nullptr);
  updateDetails(QString());
}

void MainWindow::autoSizeTagsColumn()
{
  if (!m_mods)
    return;
  // Widest row wins, scanning every row (not just the visible ones), so a mod
  // with four badges never gets its last badge clipped. Capped so that borrowing
  // this width from the stretching mod-name column stays a nudge, not a squeeze.
  constexpr int kMinTagsWidth = 72;
  constexpr int kMaxTagsWidth = 260;
  int w = 0;
  const int n = m_mods->topLevelItemCount();
  for (int i = 0; i < n; ++i) {
    QTreeWidgetItem* it = m_mods->topLevelItem(i);
    if (it->data(ColCheck, kIsSepRole).toBool())
      continue;
    w = std::max(w, ModTagDelegate::widthForTags(
                        it->data(ColTags, kTagsRole).toStringList(),
                        it->data(ColTags, kLuaEditRole).toBool()));
  }
  // Never narrower than the header caption itself (plus room for the sort
  // indicator), or the column title would elide on a list with no tags at all.
  w = std::max(w, m_mods->fontMetrics().horizontalAdvance(lang::T("Тип")) + 28);
  m_mods->header()->resizeSection(ColTags,
                                  std::clamp(w, kMinTagsWidth, kMaxTagsWidth));
}

void MainWindow::renumberPriorities()
{
  // Stored priority stays "top = highest" (kPrioRole = n-i) for persistence and
  // conflict resolution, but the displayed "#" is the human-friendly 1-based
  // top-down position (1 at the top, counting down), numbering mods only.
  // Called after a refresh or a drag-reorder — never on a column sort, so the
  // stored order is stable.
  const int n = m_mods->topLevelItemCount();
  int pos = 0;
  for (int i = 0; i < n; ++i) {
    QTreeWidgetItem* it = m_mods->topLevelItem(i);
    it->setData(ColCheck, kPrioRole, n - i);
    if (!it->data(ColCheck, kIsSepRole).toBool())
      it->setText(ColPrio, QString::number(++pos));
  }
}

void MainWindow::moveModPriority(int delta)
{
  if (m_app.activeProfile().empty())
    return;
  QTreeWidgetItem* cur = m_mods->currentItem();
  if (!cur)
    return;

  // Priority order is meaningful only in the manual view. If the list is showing
  // a browse sort, return to manual order and re-find the same row first.
  if (m_sortColumn != -1) {
    const bool isSep = cur->data(ColCheck, kIsSepRole).toBool();
    const QString id = cur->data(ColCheck, kModIdRole).toString();
    refreshMods();
    cur = nullptr;
    if (!isSep) {
      for (int i = 0; i < m_mods->topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = m_mods->topLevelItem(i);
        if (!it->data(ColCheck, kIsSepRole).toBool() &&
            it->data(ColCheck, kModIdRole).toString() == id) {
          cur = it;
          break;
        }
      }
    }
    if (!cur)
      return;
  }

  const int idx    = m_mods->indexOfTopLevelItem(cur);
  const int target = idx + delta;  // delta -1 = up (higher priority), +1 = down
  if (target < 0 || target >= m_mods->topLevelItemCount())
    return;  // already at the edge

  m_populating = true;
  QTreeWidgetItem* taken = m_mods->takeTopLevelItem(idx);
  m_mods->insertTopLevelItem(target, taken);
  m_mods->setCurrentItem(taken);
  renumberPriorities();
  m_populating = false;

  persistOrder();
  updateDerivedColumns();
  updateStatusBar();
}

void MainWindow::updateDerivedColumns()
{
  // Conflict participation for the active set.
  std::set<std::string> overrides, overridden;
  guarded("Conflicts", [&] {
    for (const auto& c : m_app.conflicts()) {
      overrides.insert(c.winner);
      for (const auto& l : c.losers)
        overridden.insert(l);
    }
  });

  m_populating = true;
  const int n = m_mods->topLevelItemCount();
  int pos = 0;
  for (int i = 0; i < n; ++i) {
    QTreeWidgetItem* it = m_mods->topLevelItem(i);
    if (it->data(ColCheck, kIsSepRole).toBool())
      continue;  // separators carry no priority/conflict marks
    // 1-based top-down position (mods only), matching renumberPriorities().
    it->setText(ColPrio, QString::number(++pos));
    const std::string id = it->data(ColCheck, kModIdRole).toString().toStdString();
    const bool over  = overrides.count(id) > 0;
    const bool under = overridden.count(id) > 0;
    QString mark;
    if (over)
      mark += "▲";
    if (under)
      mark += "▼";
    it->setText(ColConflict, mark);
    it->setToolTip(ColConflict,
                   over && under ? lang::T("Перекрывает и перекрыт")
                                 : over ? lang::T("Перекрывает другие моды")
                                        : under ? lang::T("Перекрыт модом выше") : "");
    it->setForeground(ColConflict,
                      QBrush(under ? QColor(0xE0, 0x8A, 0x3C) : QColor(0x5C, 0xB8, 0x5C)));
  }
  m_populating = false;
}

void MainWindow::persistOrder()
{
  if (m_app.activeProfile().empty())
    return;
  const int n = m_mods->topLevelItemCount();
  std::vector<ProfileEntry> entries;
  entries.reserve(n);
  for (int i = 0; i < n; ++i) {
    QTreeWidgetItem* it = m_mods->topLevelItem(i);
    ProfileEntry e;
    // Read the authoritative priority from the item (NOT the row position) so a
    // column-sorted view persists the real load order unchanged.
    e.priority = it->data(ColCheck, kPrioRole).toInt();
    if (it->data(ColCheck, kIsSepRole).toBool()) {
      e.separator = true;
      e.label     = it->text(ColName).toStdString();
    } else {
      e.modId   = it->data(ColCheck, kModIdRole).toString().toStdString();
      e.enabled = it->checkState(ColCheck) == Qt::Checked;
    }
    entries.push_back(std::move(e));
  }
  guarded("Save order", [&] { m_app.setActiveProfileEntries(entries); });
}

void MainWindow::applyFilter()
{
  const QString f = m_filter ? m_filter->text().trimmed() : QString();
  const int n     = m_mods->topLevelItemCount();
  for (int i = 0; i < n; ++i) {
    QTreeWidgetItem* it = m_mods->topLevelItem(i);
    if (it->data(ColCheck, kIsSepRole).toBool()) {
      it->setHidden(false);  // separators are structure, always shown
      continue;
    }
    // The type column's hidden text is the tag ids ("cleo,asi"), so typing
    // "cleo" or "lua" in the filter narrows the list to that kind of mod.
    const bool show =
        f.isEmpty() || it->text(ColName).contains(f, Qt::CaseInsensitive) ||
        it->text(ColId).contains(f, Qt::CaseInsensitive) ||
        it->text(ColTags).contains(f, Qt::CaseInsensitive);
    it->setHidden(!show);
  }
}

void MainWindow::updateStatusBar()
{
  m_gameLabel->setText(
      lang::T("  Игра: ") + (m_app.gamePath().empty()
                                ? lang::T("<не задана>")
                                : QString::fromStdString(m_app.gamePath())));
  m_countLabel->setText(lang::T("Модов: ") + QString::number(m_app.mods().size()));

  const bool dep = m_app.isDeployed();
  QString deployedProfile;
  if (dep) {
    try {
      deployedProfile = QString::fromStdString(m_app.currentManifest().profile);
    } catch (...) {
    }
  }
  const QString active = QString::fromStdString(m_app.activeProfile());
  if (!dep) {
    m_deployLabel->setText(lang::T("  ○ не развёрнуто  "));
    m_deployLabel->setStyleSheet("color:#888;");
  } else if (deployedProfile == active) {
    m_deployLabel->setText(lang::T("  ● РАЗВЁРНУТО: ") + deployedProfile + "  ");
    m_deployLabel->setStyleSheet("color:#5CB85C;font-weight:bold;");
  } else {
    // The selected profile differs from what is actually in the game folder.
    m_deployLabel->setText(lang::T("  ● развёрнуто: ") + deployedProfile +
                           lang::T(" (выбрано: ") + active +
                           lang::T(") — нажмите «Развернуть»  "));
    m_deployLabel->setStyleSheet("color:#E0A93C;font-weight:bold;");
  }

  // Window title shows the game and the instance (data dir) in use.
  const QString gameName =
      QString::fromStdString(fs::path(m_app.gamePath()).filename().string());
  const QString instance =
      QString::fromStdString(m_app.dataDir().filename().string());
  setWindowTitle("GMM — " + (gameName.isEmpty() ? lang::T("нет игры") : gameName) + "  [" +
                 instance + "]");

  if (m_deployAction)
    m_deployAction->setEnabled(!m_app.activeProfile().empty());
  if (m_rollbackAction)
    m_rollbackAction->setEnabled(dep);
}

void MainWindow::updateDetails(const QString& modId)
{
  if (modId.isEmpty()) {
    // No mod selected → show the active profile's build notes (Markdown).
    m_detailModId.clear();
    if (m_modDescButton)
      m_modDescButton->setVisible(false);
    showBuildInfo();
    m_conflicts->clear();
    return;
  }
  // A mod is selected: leave build-edit mode and hide its controls.
  setBuildInfoEditing(false);
  m_showingBuild = false;
  if (m_infoEditButton)
    m_infoEditButton->setVisible(false);

  const std::string id = modId.toStdString();
  Mod mod;
  bool found = false;
  for (const auto& m : m_app.mods())
    if (m.id == id) {
      mod   = m;
      found = true;
      break;
    }
  if (!found) {
    m_detailModId.clear();
    if (m_modDescButton)
      m_modDescButton->setVisible(false);
    showBuildInfo();
    m_conflicts->clear();
    return;
  }
  m_detailModId = modId;
  if (m_modDescButton)
    m_modDescButton->setVisible(true);

  m_info->setSearchPaths({});  // mod details are self-contained HTML
  m_info->document()->setDefaultStyleSheet(detailsCss());
  m_conflicts->document()->setDefaultStyleSheet(detailsCss());

  const auto files = listModFiles(m_app.modsDir() / id / "root");
  QString html = "<h2>" + QString::fromStdString(mod.name).toHtmlEscaped() + "</h2>";
  if (!mod.description.empty()) {
    // Plain text with preserved newlines/paragraphs -- escape then convert.
    html += "<p style='color:palette(text)'>" +
            QString::fromStdString(mod.description).toHtmlEscaped().replace("\n", "<br>") +
            "</p><hr>";
  }
  html += "<table cellspacing=6>";
  auto row = [&](const QString& k, const QString& v) {
    html += "<tr><td><b>" + k + "</b></td><td>" + v.toHtmlEscaped() + "</td></tr>";
  };
  row("Id", QString::fromStdString(mod.id));
  row(lang::T("Источник"), QString::fromStdString(mod.source));
  row(lang::T("Импортирован"), humanTime(mod.importedAt));
  row(lang::T("Хеш"), QString::fromStdString(mod.contentHash));
  row(lang::T("Файлов"), QString::number(files.size()));
  html += lang::T("</table><hr><b>Файлы</b><ul>");
  int shown = 0;
  for (const auto& f : files) {
    html += "<li>" + QString::fromStdString(f).toHtmlEscaped() + "</li>";
    if (++shown >= 1000) {
      html += "<li>…</li>";
      break;
    }
  }
  html += "</ul>";
  m_info->setHtml(html);

  // Conflicts involving this mod.
  QString over, under;
  guarded("Conflicts", [&] {
    for (const auto& c : m_app.conflicts()) {
      if (c.winner == id) {
        QString losers;
        for (const auto& l : c.losers)
          losers += " " + QString::fromStdString(l).toHtmlEscaped();
        over += "<li>" + QString::fromStdString(c.path).toHtmlEscaped() +
                lang::T(" <i>(перекрывает:") + losers + ")</i></li>";
      } else if (std::find(c.losers.begin(), c.losers.end(), id) != c.losers.end()) {
        under += "<li>" + QString::fromStdString(c.path).toHtmlEscaped() +
                 lang::T(" <i>(модом ") + QString::fromStdString(c.winner).toHtmlEscaped() +
                 ")</i></li>";
      }
    }
  });
  QString ch = lang::T("<h3>Перекрывает</h3>");
  ch += over.isEmpty() ? lang::T("<p>Нет.</p>") : "<ul>" + over + "</ul>";
  ch += lang::T("<h3>Перекрыт</h3>");
  ch += under.isEmpty() ? lang::T("<p>Нет.</p>") : "<ul>" + under + "</ul>";
  m_conflicts->setHtml(ch);
}

// --- build info (per-profile rich-text notes, stored as HTML) ---------------
//
// Notes used to be stored as Markdown, but Qt's setMarkdown() has real bugs
// with raw HTML blocks (e.g. a centered <img>) and with inline code spans
// next to punctuation — both silently break rendering (empty panel / mangled
// text). Content is now edited with a WYSIWYG QTextEdit and persisted as
// HTML. looksLikeHtml() lets us still render any pre-existing Markdown notes
// (best-effort) until they're re-saved in the new format.

namespace {
bool looksLikeHtml(const std::string& s)
{
  const auto pos = s.find_first_not_of(" \t\r\n");
  return pos != std::string::npos && s[pos] == '<';
}
}  // namespace

void MainWindow::showBuildInfo()
{
  m_showingBuild = true;
  setBuildInfoEditing(false);

  const std::string profile = m_app.activeProfile();
  if (m_infoEditButton)
    m_infoEditButton->setVisible(!profile.empty());

  if (profile.empty()) {
    m_info->setSearchPaths({});
    m_info->setHtml("<p><i>" + lang::T("Профиль не выбран.") + "</i></p>");
    return;
  }

  // Resolve relative image paths (images/foo.png) against the profile's info dir.
  const QString infoDir =
      QString::fromStdString(m_app.profileInfoDir(profile).string());
  m_info->setSearchPaths({infoDir});
  m_info->document()->setDefaultStyleSheet(detailsCss());

  std::string stored;
  guarded(lang::T("Инфо о сборке"),
          [&] { stored = m_app.loadProfileInfo(profile); });
  if (stored.empty()) {
    m_info->setHtml("<h1>" + QString::fromStdString(profile) + "</h1><p><i>" +
                     lang::T("Нет описания сборки. Нажмите «Редактировать "
                             "сборку», чтобы добавить текст и изображения.") +
                     "</i></p>");
    return;
  }
  if (looksLikeHtml(stored))
    m_info->setHtml(QString::fromStdString(stored));
  else
    m_info->setMarkdown(QString::fromStdString(stored));  // legacy notes
}

void MainWindow::setBuildInfoEditing(bool on)
{
  m_infoEditing = on;
  if (m_infoEditor)
    m_infoEditor->setVisible(on);
  if (m_info)
    m_info->setVisible(!on);
  // The edit-toggle button only makes sense in the rendered build view.
  if (m_infoEditButton)
    m_infoEditButton->setVisible(!on && m_showingBuild &&
                                 !m_app.activeProfile().empty());
  if (m_infoInsertImgButton)
    m_infoInsertImgButton->setVisible(on);
  if (m_infoSaveButton)
    m_infoSaveButton->setVisible(on);
  if (m_infoCancelButton)
    m_infoCancelButton->setVisible(on);
  if (m_infoFormatBar)
    m_infoFormatBar->setVisible(on);
}

void MainWindow::onEditBuildInfo()
{
  if (m_app.activeProfile().empty()) {
    dialogs::information(this, lang::T("Инфо о сборке"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }
  std::string stored;
  guarded(lang::T("Инфо о сборке"),
          [&] { stored = m_app.loadProfileInfo(m_app.activeProfile()); });
  if (stored.empty()) {
    m_infoEditor->clear();
  } else if (looksLikeHtml(stored)) {
    m_infoEditor->setHtml(QString::fromStdString(stored));
  } else {
    // Legacy Markdown notes, saved before the rich-text editor existed:
    // convert once so they land in the editor as real formatting.
    QTextDocument tmp;
    tmp.setMarkdown(QString::fromStdString(stored));
    m_infoEditor->setHtml(tmp.toHtml());
  }
  setBuildInfoEditing(true);
  m_infoEditor->setFocus();
}

void MainWindow::onEditModDescription()
{
  if (m_detailModId.isEmpty())
    return;
  const std::string id = m_detailModId.toStdString();
  QString current;
  for (const auto& m : m_app.mods())
    if (m.id == id) {
      current = QString::fromStdString(m.description);
      break;
    }

  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Описание мода"));
  dlg.resize(480, 320);
  auto* body = frameless(dlg);

  auto* editor = new QPlainTextEdit(current, &dlg);
  editor->setPlaceholderText(
      lang::T("Что делает этот мод, зачем он тут — просто заметка для себя."));
  body->addWidget(editor, 1);

  auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  body->addWidget(box);

  editor->setFocus();
  centerDialog(dlg);
  if (dlg.exec() != QDialog::Accepted)
    return;

  guarded(lang::T("Описание мода"), [&] {
    m_app.setModDescription(id, editor->toPlainText().toStdString());
  });
  updateDetails(m_detailModId);
}

void MainWindow::onInsertBuildImage()
{
  const std::string profile = m_app.activeProfile();
  if (profile.empty())
    return;
  const QString file = QFileDialog::getOpenFileName(
      this, lang::T("Выберите изображение"), QString(),
      lang::T("Изображения (*.png *.jpg *.jpeg *.gif *.bmp *.webp);;Все файлы (*)"));
  if (file.isEmpty())
    return;
  std::string rel;
  guarded(lang::T("Вставка изображения"), [&] {
    rel = m_app.addProfileInfoImage(profile, file.toStdString());
  });
  if (rel.empty())
    return;
  const QString relQ = QString::fromStdString(rel);
  const QString infoDir =
      QString::fromStdString(m_app.profileInfoDir(profile).string());
  const QImage image(infoDir + "/" + relQ);
  if (image.isNull()) {
    dialogs::warning(this, lang::T("Вставка изображения"),
                         lang::T("Не удалось загрузить изображение."));
    return;
  }
  // Register it under its profile-relative path so the saved HTML's <img
  // src="images/…"> stays portable (same resolution scheme the rendered
  // view uses via QTextBrowser::setSearchPaths).
  QTextCursor cur = m_infoEditor->textCursor();
  cur.insertImage(image, relQ);
  m_infoEditor->setTextCursor(cur);
  m_infoEditor->setFocus();
}

void MainWindow::onSaveBuildInfo()
{
  const std::string profile = m_app.activeProfile();
  if (profile.empty())
    return;
  const QString html = m_infoEditor->toHtml();
  guarded(lang::T("Инфо о сборке"),
          [&] { m_app.saveProfileInfo(profile, html.toStdString()); });
  setBuildInfoEditing(false);
  showBuildInfo();
  statusBar()->showMessage(lang::T("Описание сборки сохранено"), 4000);
}

void MainWindow::onCancelBuildInfo()
{
  setBuildInfoEditing(false);
  showBuildInfo();
}

// --- slots ------------------------------------------------------------------

void MainWindow::onProfileChanged(int index)
{
  if (m_populating || index < 0)
    return;
  guarded("Switch profile",
          [&] { m_app.useProfile(m_profile->itemText(index).toStdString()); });
  refreshMods();
  populateExecutables();  // a different profile can deploy a different root exe
  updateExternalToolActions();  // SA-MP is per-profile, so its button follows it
  updateStatusBar();
}

void MainWindow::onSwitchInstance()
{
  if (m_playRunning) {
    statusBar()->showMessage("Дождитесь выхода из игры — действие недоступно", 3000);
    return;  // reassigning m_app under the running worker would crash
  }
  auto chosen = InstanceDialog::choose(this);
  if (!chosen)
    return;
  if (fs::weakly_canonical(*chosen) == fs::weakly_canonical(m_app.dataDir()))
    return;  // already on this instance
  m_app = App(*chosen);
  refreshProfiles();
  populateExecutables();
  updateExternalToolActions();  // another instance has its own tools/profile
  refreshMods();
  updateStatusBar();
}

void MainWindow::onChangeGameFolder()
{
  const QString dir = QFileDialog::getExistingDirectory(
      this, lang::T("Выберите папку GTA San Andreas (с gta_sa.exe)"));
  if (dir.isEmpty())
    return;
  std::error_code ec;
  if (!fs::exists(fs::path(dir.toStdString()) / "gta_sa.exe", ec)) {
    if (dialogs::question(
            this, lang::T("gta_sa.exe не найден"),
            lang::T("В этой папке нет gta_sa.exe. Всё равно использовать?")) !=
        QMessageBox::Yes)
      return;
  }
  guarded(lang::T("Смена папки игры"), [&] { m_app.setGamePath(dir.toStdString()); });
  populateExecutables();
  updateStatusBar();
}

void MainWindow::onOpenInstanceFolder()
{
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(m_app.dataDir().string())));
}

void MainWindow::onOpenGameFolder()
{
  const QString path = QString::fromStdString(m_app.gamePath());
  if (path.isEmpty() || !QDir(path).exists())
  {
    dialogs::warning(this, lang::T("Открыть папку игры"),
                         lang::T("Папка игры не найдена."));
    return;
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::onNewProfile()
{
  bool ok = false;
  const QString name = dialogs::getText(this, lang::T("Новый профиль"),
                                             lang::T("Название:"), QLineEdit::Normal, "", &ok);
  if (!ok || name.isEmpty())
    return;
  guarded(lang::T("Новый профиль"), [&] {
    m_app.createProfile(name.toStdString());
    m_app.useProfile(name.toStdString());
  });
  refreshProfiles();
  refreshMods();
  updateStatusBar();
}

void MainWindow::onRenameProfile()
{
  const std::string oldName = m_app.activeProfile();
  if (oldName.empty())
    return;
  bool ok = false;
  const QString name =
      dialogs::getText(this, lang::T("Переименовать профиль"), lang::T("Новое название:"),
                            QLineEdit::Normal, QString::fromStdString(oldName), &ok);
  if (!ok)
    return;
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty() || trimmed.toStdString() == oldName)
    return;
  guarded(lang::T("Переименование профиля"),
          [&] { m_app.renameProfile(oldName, trimmed.toStdString()); });
  refreshProfiles();
  refreshMods();
  updateStatusBar();
}

void MainWindow::onCopyProfile()
{
  const std::string src = m_app.activeProfile();
  if (src.empty()) {
    dialogs::information(this, lang::T("Копировать профиль"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }

  // A small dialog: new name + "copy saves" / "copy settings" toggles.
  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Копировать профиль"));
  auto* body = frameless(dlg);
  auto* form = new QFormLayout;
  body->addLayout(form);
  // Profile names are ASCII-only (isValidProfileName); use a plain suffix.
  auto* nameEdit = new QLineEdit(QString::fromStdString(src) + " copy", &dlg);
  form->addRow(new QLabel(lang::T("Название копии:"), &dlg), nameEdit);
  auto* cbSaves = new QCheckBox(lang::T("Скопировать сохранения (*.b)"), &dlg);
  auto* cbSettings = new QCheckBox(lang::T("Скопировать настройки (gta_sa.set)"), &dlg);
  form->addRow(cbSaves);
  form->addRow(cbSettings);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                   &dlg);
  form->addRow(box);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  centerDialog(dlg);
  if (dlg.exec() != QDialog::Accepted)
    return;

  const QString dst = nameEdit->text().trimmed();
  if (dst.isEmpty() || dst.toStdString() == src)
    return;
  const bool withSaves    = cbSaves->isChecked();
  const bool withSettings = cbSettings->isChecked();

  guarded(lang::T("Копирование профиля"), [&] {
    m_app.copyProfile(src, dst.toStdString(), withSaves, withSettings);
    m_app.useProfile(dst.toStdString());
  });
  refreshProfiles();
  refreshMods();
  updateStatusBar();
  statusBar()->showMessage(lang::T("Профиль скопирован: ") + dst, 4000);
}

void MainWindow::onSampSettings()
{
  const std::string prof = m_app.activeProfile();
  if (prof.empty()) {
    dialogs::information(this, lang::T("Настройки SA-MP"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }

  gtamm::SampConfig s = m_app.sampConfig(prof);

  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Настройки SA-MP (мультиплеер)"));
  auto* body = frameless(dlg);
  auto* form = new QFormLayout;
  body->addLayout(form);

  auto* cbEnabled = new QCheckBox(
      lang::T("Запускать игру через SA-MP (samp.exe)"), &dlg);
  cbEnabled->setChecked(s.enabled);
  cbEnabled->setToolTip(lang::T(
      "samp.exe внедряет samp.dll в gta_sa.exe и завершается; менеджер ждёт "
      "именно игру, поэтому моды не выдёргиваются до выхода из игры."));
  form->addRow(cbEnabled);

  auto* serverRow  = new QHBoxLayout;
  auto* serverEdit = new QLineEdit(QString::fromStdString(s.server), &dlg);
  serverEdit->setPlaceholderText(lang::T("IP или хост (пусто — главное меню)"));
  auto* portSpin = new QSpinBox(&dlg);
  portSpin->setRange(1, 65535);
  portSpin->setValue(s.port > 0 ? s.port : 7777);
  auto* browseServersBtn = new QPushButton(lang::T("Обзор серверов…"), &dlg);
  serverRow->addWidget(serverEdit, 1);
  serverRow->addWidget(browseServersBtn);
  form->addRow(new QLabel(lang::T("Сервер:"), &dlg), serverRow);
  form->addRow(new QLabel(lang::T("Порт:"), &dlg), portSpin);
  connect(browseServersBtn, &QPushButton::clicked, &dlg, [&, this] {
    QString host;
    int port = 0;
    if (ServerBrowserDialog::pickServer(&dlg, host, port)) {
      serverEdit->setText(host);
      portSpin->setValue(port);
    }
  });

  auto* passwordEdit = new QLineEdit(QString::fromStdString(s.password), &dlg);
  passwordEdit->setEchoMode(QLineEdit::Password);
  passwordEdit->setPlaceholderText(lang::T("Пароль сервера (если нужен)"));
  form->addRow(new QLabel(lang::T("Пароль:"), &dlg), passwordEdit);

  auto* nickEdit = new QLineEdit(QString::fromStdString(s.nick), &dlg);
  nickEdit->setPlaceholderText(lang::T("Имя игрока (никнейм)"));
  form->addRow(new QLabel(lang::T("Ник:"), &dlg), nickEdit);

  auto* exeRow  = new QHBoxLayout;
  auto* exeEdit = new QLineEdit(
      QString::fromStdString(s.exe.empty() ? "samp.exe" : s.exe), &dlg);
  exeEdit->setToolTip(lang::T("Путь к samp.exe (относительно папки игры или абсолютный)"));
  auto* exeBrowseBtn = new QPushButton(lang::T("Обзор…"), &dlg);
  exeRow->addWidget(exeEdit, 1);
  exeRow->addWidget(exeBrowseBtn);
  form->addRow(new QLabel(lang::T("Launcher SA-MP:"), &dlg), exeRow);
  connect(exeBrowseBtn, &QPushButton::clicked, &dlg, [&, this] {
    const QString start = QString::fromStdString(m_app.gamePath());
    const QString file  = QFileDialog::getOpenFileName(
        &dlg, lang::T("Выберите samp.exe"), start, lang::T("samp.exe;;Все файлы (*.exe)"));
    if (!file.isEmpty())
      exeEdit->setText(file);
  });

  // Warn (don't block) if the configured exe isn't actually there yet -- SA-MP
  // isn't bundled with GMM by default (usually installed by hand into the game
  // folder), but it's also entirely normal for it to come from a MOD that
  // simply hasn't been deployed yet (a portable build where samp.exe/SAMP/
  // ships as part of a mod's own files) -- relativePathWillExist() checks the
  // CURRENT deploy plan too, so that legitimate case doesn't get flagged as an
  // error. It's ALSO normal for it to come from the bundled SA-MP client
  // (App::deploySampRuntime(), see the status label below): that bundle isn't
  // part of the mod plan relativePathWillExist() checks, so it's checked here
  // separately -- otherwise this warning and the bundle's own "✓ found" status
  // would contradict each other for the exact same file.
  auto* warnLabel = new QLabel(&dlg);
  warnLabel->setStyleSheet("color: #d08a3c;");
  warnLabel->setWordWrap(true);
  auto refreshWarn = [&] {
    const std::string exeText = exeEdit->text().trimmed().toStdString();
    const std::string exeName = exeText.empty() ? "samp.exe" : exeText;
    const fs::path exeRel = fs::path(exeName);
    std::error_code ec;
    const bool bundleProvides =
        !exeRel.is_absolute() && fs::exists(m_app.sampRuntimeDir() / exeRel, ec);
    warnLabel->setVisible(!m_app.relativePathWillExist(exeName) && !bundleProvides);
    warnLabel->setText(lang::T(
        "⚠ Файл не найден, ни один включённый мод его не разворачивает, и "
        "встроенного клиента с таким файлом тоже нет. Если samp.exe входит в "
        "сборку — проверь, что нужный мод включён; если нет — установи клиент "
        "SA-MP в папку игры вручную, либо положи полный клиент в "
        "gtamm/runtime-samp/ (см. Mod Loader → статус ниже)."));
  };
  connect(exeEdit, &QLineEdit::textChanged, &dlg, refreshWarn);
  refreshWarn();
  form->addRow(warnLabel);

  // Status of the bundled SA-MP client (App::deploySampRuntime()): if present,
  // deploy() tops up whatever a mod's own SA-MP files are missing (mouse.png,
  // sampgui.png, the SAMP/ archives, ...) automatically -- no need to hunt down
  // and re-import those files by hand.
  auto* bundleLabel = new QLabel(&dlg);
  bundleLabel->setWordWrap(true);
  if (m_app.hasSampRuntime()) {
    bundleLabel->setStyleSheet("color: #3c9d5c;");
    bundleLabel->setText(lang::T(
        "✓ Найден встроенный клиент SA-MP — при развёртывании менеджер сам "
        "докладывает недостающие файлы (mouse.png, sampgui.png, архивы SAMP/), "
        "даже если мод с SA-MP импортирован не полностью."));
  } else {
    bundleLabel->setStyleSheet("color: #808080;");
    bundleLabel->setText(lang::T(
        "Встроенный клиент SA-MP не найден — недостающие файлы придётся "
        "переносить вручную (см. runtime-samp/README.txt)."));
  }
  form->addRow(bundleLabel);

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                   &dlg);
  form->addRow(box);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  centerDialog(dlg);
  if (dlg.exec() != QDialog::Accepted)
    return;

  s.enabled  = cbEnabled->isChecked();
  s.server   = serverEdit->text().trimmed().toStdString();
  s.port     = portSpin->value();
  s.password = passwordEdit->text().toStdString();
  s.nick     = nickEdit->text().trimmed().toStdString();
  s.exe      = exeEdit->text().trimmed().toStdString();
  if (s.exe.empty())
    s.exe = "samp.exe";

  guarded(lang::T("Сохранение настроек SA-MP"),
          [&] { m_app.setSampConfig(prof, s); });
  updateExternalToolActions();  // toggling SA-MP shows/hides its toolbar button
  statusBar()->showMessage(
      s.enabled ? lang::T("SA-MP включён для профиля") : lang::T("SA-MP выключен"),
      4000);
}

void MainWindow::onDeleteProfile()
{
  const std::string name = m_app.activeProfile();
  if (name.empty())
    return;
  if (dialogs::question(
          this, lang::T("Удаление профиля"),
          lang::T("Удалить профиль «%1»?").arg(QString::fromStdString(name))) !=
      QMessageBox::Yes)
    return;
  guarded(lang::T("Удаление профиля"), [&] { m_app.deleteProfile(name); });
  refreshProfiles();
  refreshMods();
  updateStatusBar();
}

void MainWindow::onManageProfiles()
{
  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Менеджер профилей"));
  dlg.resize(520, 380);
  auto* root = frameless(dlg);

  auto* row = new QHBoxLayout;
  auto* list = new QListWidget(&dlg);
  row->addWidget(list, 1);

  auto* btns          = new QVBoxLayout;
  auto* renameBtn     = new QPushButton(lang::T("Переименовать…"), &dlg);
  auto* deleteBtn     = new QPushButton(lang::T("Удалить"), &dlg);
  auto* exportBtn     = new QPushButton(lang::T("Экспорт в zip…"), &dlg);
  auto* importBtn     = new QPushButton(lang::T("Импорт из zip…"), &dlg);
  btns->addWidget(renameBtn);
  btns->addWidget(deleteBtn);
  btns->addWidget(exportBtn);
  btns->addSpacing(12);
  btns->addWidget(importBtn);
  btns->addStretch(1);
  row->addLayout(btns);
  root->addLayout(row, 1);

  auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(closeBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  connect(closeBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  root->addWidget(closeBox);

  // The active profile is shown in bold; everything else acts on whichever
  // row is selected, independent of what's active in the main window.
  auto reloadList = [&](const std::string& selectName = {}) {
    list->clear();
    const std::string active = m_app.activeProfile();
    QListWidgetItem* toSelect = nullptr;
    for (const auto& n : m_app.profileNames()) {
      auto* item = new QListWidgetItem(QString::fromStdString(n), list);
      if (n == active) {
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
      }
      if (n == selectName)
        toSelect = item;
    }
    if (toSelect)
      toSelect->setSelected(true);
  };
  reloadList();

  auto selectedName = [&]() -> std::string {
    const auto sel = list->selectedItems();
    return sel.isEmpty() ? std::string{} : sel.first()->text().toStdString();
  };
  auto syncMainWindow = [&] {
    refreshProfiles();
    refreshMods();
    updateStatusBar();
  };
  auto updateButtons = [&] {
    const bool has = !selectedName().empty();
    renameBtn->setEnabled(has);
    deleteBtn->setEnabled(has);
    exportBtn->setEnabled(has);
  };
  connect(list, &QListWidget::itemSelectionChanged, &dlg, updateButtons);
  updateButtons();

  connect(renameBtn, &QPushButton::clicked, &dlg, [&] {
    const std::string oldName = selectedName();
    if (oldName.empty())
      return;
    bool ok = false;
    const QString name = dialogs::getText(
        &dlg, lang::T("Переименовать профиль"), lang::T("Новое название:"),
        QLineEdit::Normal, QString::fromStdString(oldName), &ok);
    if (!ok)
      return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.toStdString() == oldName)
      return;
    bool done = false;
    guarded(lang::T("Переименование профиля"), [&] {
      m_app.renameProfile(oldName, trimmed.toStdString());
      done = true;
    });
    if (done) {
      reloadList(trimmed.toStdString());
      updateButtons();
      syncMainWindow();
    }
  });

  connect(deleteBtn, &QPushButton::clicked, &dlg, [&] {
    const std::string name = selectedName();
    if (name.empty())
      return;
    if (dialogs::question(&dlg, lang::T("Удаление профиля"),
                              lang::T("Удалить профиль «%1»?")
                                  .arg(QString::fromStdString(name))) != QMessageBox::Yes)
      return;
    bool done = false;
    guarded(lang::T("Удаление профиля"), [&] {
      m_app.deleteProfile(name);
      done = true;
    });
    if (done) {
      reloadList();
      updateButtons();
      syncMainWindow();
    }
  });

  connect(exportBtn, &QPushButton::clicked, &dlg, [&] {
    const std::string profileName = selectedName();
    if (profileName.empty())
      return;

    QDialog opts(&dlg);
    opts.setWindowTitle(lang::T("Экспорт сборки"));
    auto* optsBody = frameless(opts);
    auto* form = new QFormLayout;
    optsBody->addLayout(form);
    form->addRow(new QLabel(
        lang::T("Что включить в архив сборки (профиль и моды включены всегда):"),
        &opts));
    auto* cbSaves    = new QCheckBox(lang::T("Сохранения профиля (*.b)"), &opts);
    auto* cbSettings = new QCheckBox(lang::T("Настройки игры (gta_sa.set)"), &opts);
    form->addRow(cbSaves);
    form->addRow(cbSettings);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &opts);
    form->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &opts, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &opts, &QDialog::reject);
    centerDialog(opts);
    if (opts.exec() != QDialog::Accepted)
      return;
    const bool withSaves    = cbSaves->isChecked();
    const bool withSettings = cbSettings->isChecked();

    const QString suggested = QString::fromStdString(profileName) + ".gmmbuild.zip";
    const QString zip = QFileDialog::getSaveFileName(
        &dlg, lang::T("Экспорт сборки в zip"), suggested, lang::T("Сборки GMM (*.zip)"));
    if (zip.isEmpty())
      return;

    bool done = false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    guarded(lang::T("Экспорт сборки"), [&] {
      m_app.exportBuild(profileName, zip.toStdString(), withSaves, withSettings);
      done = true;
    });
    QApplication::restoreOverrideCursor();
    if (done)
      dialogs::information(&dlg, lang::T("Экспорт сборки"),
                               lang::T("Сборка экспортирована:") + "\n" + zip);
  });

  connect(importBtn, &QPushButton::clicked, &dlg, [&] {
    const QString zip = QFileDialog::getOpenFileName(
        &dlg, lang::T("Импорт сборки из zip"), QString(), lang::T("Сборки GMM (*.zip)"));
    if (zip.isEmpty())
      return;
    const QString newProfile = importBuildZipInteractive(zip);
    if (!newProfile.isEmpty()) {
      reloadList(newProfile.toStdString());
      updateButtons();
      syncMainWindow();
    }
  });

  centerDialog(dlg);
  dlg.exec();
  syncMainWindow();
}

void MainWindow::onToggleManageSaves(bool on)
{
  guarded(lang::T("Сохранения профилей"), [&] { m_app.setManageSaves(on); });
  if (on)
    statusBar()->showMessage(
        lang::T("Свои сохранения для профилей включены — подменяются на время игры"), 5000);
  else
    statusBar()->showMessage(lang::T("Свои сохранения для профилей выключены"), 4000);
}

void MainWindow::onOpenSavesFolder()
{
  const std::string name = m_app.activeProfile();
  if (name.empty()) {
    dialogs::information(this, lang::T("Сохранения профиля"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }
  const auto dir = m_app.profileSavesDir(name);
  std::error_code ec;
  fs::create_directories(dir, ec);
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(dir.string())));
}

void MainWindow::onToggleManageGenerated(bool on)
{
  guarded(lang::T("Чистая папка игры"), [&] { m_app.setManageGenerated(on); });
  if (on)
    statusBar()->showMessage(
        lang::T("Папка игры будет очищаться после игры; созданные файлы — в стор "
                "профиля"),
        5000);
  else
    statusBar()->showMessage(lang::T("Очистка папки игры выключена"), 4000);
}

void MainWindow::onToggleAutoMaps(bool on)
{
  guarded(lang::T("Авто-установка карт"), [&] { m_app.setAutoRouteMaps(on); });
  if (on)
    statusBar()->showMessage(
        lang::T("Авто-установка карт включена (экспериментально). Переразверните "
                "профиль. Если новая игра не запускается — выключите обратно."),
        7000);
  else
    statusBar()->showMessage(
        lang::T("Авто-установка карт выключена. Переразверните профиль."), 5000);
}

void MainWindow::onToggleSteamIntegration(bool on)
{
  guarded(lang::T("Интеграция со Steam"), [&] { m_app.setSteamIntegration(on); });
  if (on) {
    // The toggle alone achieves nothing without the redistributable -- say so
    // instead of leaving the user waiting for a status that never appears.
    if (m_app.steamStatus().hasApiDll)
      statusBar()->showMessage(
          lang::T("Статус в Steam включён: пока идёт игра, в Steam будет "
                  "«играет в Grand Theft Auto: San Andreas»."),
          6000);
    else
      statusBar()->showMessage(
          lang::T("Статус в Steam включён, но нет steam_api64.dll — путь "
                  "показан в настройках, без неё статус не появится."),
          8000);
  } else
    statusBar()->showMessage(lang::T("Статус в Steam выключен."), 4000);
}

void MainWindow::onOpenGeneratedFolder()
{
  const std::string name = m_app.activeProfile();
  if (name.empty()) {
    dialogs::information(this, lang::T("Созданные файлы профиля"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }
  const auto dir = m_app.generatedDir(name);
  std::error_code ec;
  fs::create_directories(dir, ec);
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QString::fromStdString(dir.string())));
}

void MainWindow::importPath(const QString& path, bool viaModloader, const QString& nameOverride)
{
  if (m_playRunning) {
    // Same gate guarded() applies -- the launcher still owns the deploy/saves
    // swap while the game runs.
    statusBar()->showMessage(lang::T("Дождитесь выхода из игры — действие недоступно"), 3000);
    return;
  }
  std::error_code ec;
  const bool isDir = fs::is_directory(path.toStdString(), ec);
  const std::string nameOpt = nameOverride.toStdString();
  std::string password;  // filled in if the archive turns out to be encrypted
  auto showResults = [this](const std::vector<App::ImportResult>& results) {
    if (results.size() > 1) {
      statusBar()->showMessage(
          lang::T("Импортировано модов: %1").arg(static_cast<int>(results.size())),
          5000);
    } else if (!results.empty()) {
      const auto& r = results.front();
      if (r.wasDuplicate)
        statusBar()->showMessage(
            lang::T("Уже в пуле как «%1»").arg(QString::fromStdString(r.mod.id)),
            4000);
      else
        statusBar()->showMessage(
            lang::T("Импортирован «%1»").arg(QString::fromStdString(r.mod.name)),
            4000);
    }
  };
  // Archives can turn out to be password-protected -- caught specifically (not
  // via guarded(), which would just show it as a generic error) so we can
  // prompt and retry with the typed password instead of failing outright.
  for (;;) {
    try {
      showResults(isDir ? m_app.importFolderAsMods(path.toStdString(), nameOpt, viaModloader)
                        : m_app.importArchiveAsMods(path.toStdString(), nameOpt, viaModloader,
                                                    password));
      break;
    } catch (const ArchivePasswordRequired&) {
      bool ok            = false;
      const QString hint = password.empty()
                               ? lang::T("Архив защищён паролем. Введите пароль:")
                               : lang::T("Неверный пароль. Попробуйте ещё раз:");
      const QString typed = dialogs::getText(this, lang::T("Пароль архива"), hint,
                                                   QLineEdit::Password, QString(), &ok);
      if (!ok) {
        statusBar()->showMessage(lang::T("Импорт отменён — нужен пароль архива"), 4000);
        break;
      }
      password = typed.toStdString();
      // loop and retry with the new password
    } catch (const std::exception& e) {
      dialogs::warning(this, lang::T("Импорт"), QString::fromLocal8Bit(e.what()));
      break;
    }
  }
  refreshMods();
}

bool MainWindow::askImportMode(bool& viaModloader, QString& name, const QString& defaultName)
{
  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Импорт мода"));
  auto* v = frameless(dlg);

  auto hint = [](const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color: gray; font-size: 11px;");
    return l;
  };

  v->addWidget(new QLabel(lang::T("Как интегрировать этот мод?")));
  auto* native = new QRadioButton(lang::T("Напрямую"));
  auto* ml     = new QRadioButton(lang::T("Через Modloader"));
  ml->setChecked(true);
  v->addWidget(native);
  v->addWidget(hint(lang::T(
      "Менеджер сам инжектит файлы в IMG/loose (как обычно).")));
  v->addWidget(ml);
  v->addWidget(hint(lang::T(
      "Мод кладётся в modloader/<имя>/ и грузится рантаймом Modloader "
      "(нужен modloader.asi).")));

  auto* form     = new QFormLayout;
  auto* nameEdit = new QLineEdit(defaultName);
  form->addRow(lang::T("Имя мода:"), nameEdit);
  v->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  v->addWidget(buttons);

  centerDialog(dlg);
  if (dlg.exec() != QDialog::Accepted)
    return false;  // cancelled

  viaModloader = ml->isChecked();
  const QString typed = nameEdit->text().trimmed();
  name = (typed.isEmpty() || typed == defaultName) ? QString() : typed;
  return true;
}

void MainWindow::onImportFolder()
{
  const QString dir =
      QFileDialog::getExistingDirectory(this, lang::T("Выберите папку мода"));
  if (dir.isEmpty())
    return;
  bool viaMl = false;
  QString name;
  const QString defaultName = QFileInfo(dir).fileName();
  if (askImportMode(viaMl, name, defaultName))
    importPath(dir, viaMl, name);
}

void MainWindow::onImportArchive()
{
  const QString file = QFileDialog::getOpenFileName(
      this, lang::T("Выберите архив мода"), QString(),
      lang::T("Архивы модов (*.zip *.7z *.rar *.tar *.gz *.bz2 *.xz);;Все файлы (*)"));
  if (file.isEmpty())
    return;
  bool viaMl = false;
  QString name;
  const QString defaultName = QFileInfo(file).completeBaseName();
  if (askImportMode(viaMl, name, defaultName))
    importPath(file, viaMl, name);
}

void MainWindow::onModloaderRuntimeFolder()
{
  const fs::path cur = m_app.modloaderRuntimeDir();
  std::error_code ec;
  fs::create_directories(cur, ec);

  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Рантайм Modloader"));
  auto* body = frameless(dlg);
  body->setContentsMargins(20, 16, 20, 16);
  body->setSpacing(10);

  auto* textLabel = new QLabel(
      lang::T("Папка рантайма Modloader:") + "\n" + QString::fromStdString(cur.string()) +
          "\n\n" +
          (m_app.hasModloaderRuntime()
               ? lang::T("Рантайм найден (modloader.asi).")
               : lang::T("Рантайм не найден. Положите сюда modloader.asi и ASI-loader "
                        "(напр. dinput8.dll) — менеджер установит их в игру при "
                        "развёртывании мода через Modloader.")),
      &dlg);
  textLabel->setWordWrap(true);
  body->addWidget(textLabel);

  auto* box = new QDialogButtonBox(&dlg);
  QPushButton* open = box->addButton(lang::T("Открыть папку"), QDialogButtonBox::ActionRole);
  QPushButton* choose =
      box->addButton(lang::T("Выбрать другую…"), QDialogButtonBox::ActionRole);
  box->addButton(QDialogButtonBox::Close);
  body->addWidget(box);
  QAbstractButton* clicked = nullptr;
  connect(box, &QDialogButtonBox::clicked, &dlg, [&dlg, &clicked](QAbstractButton* b) {
    clicked = b;
    dlg.accept();
  });

  centerDialog(dlg);
  dlg.exec();
  if (clicked == open) {
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(cur.string())));
  } else if (clicked == choose) {
    const QString dir = QFileDialog::getExistingDirectory(
        this, lang::T("Папка рантайма Modloader"),
        QString::fromStdString(cur.string()));
    if (!dir.isEmpty())
      guarded(lang::T("Рантайм Modloader"),
              [&] { m_app.setModloaderRuntimeDir(dir.toStdString()); });
  }
}

void MainWindow::onImportBuild()
{
  const QString dir = QFileDialog::getExistingDirectory(
      this, lang::T("Выберите папку игры со сборкой (модами)"));
  if (dir.isEmpty())
    return;

  bool ok = false;
  const QString suggested =
      QString::fromStdString(fs::path(dir.toStdString()).filename().string());
  const QString prof = dialogs::getText(
      this, lang::T("Импорт сборки"), lang::T("Имя нового профиля для сборки:"),
      QLineEdit::Normal, suggested, &ok);
  if (!ok || prof.trimmed().isEmpty())
    return;

  App::ImportBuildResult res;
  bool done = false;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  statusBar()->showMessage(
      lang::T("Сравнение сборки с ванилью… (первый раз может занять минуту)"));
  guarded(lang::T("Импорт сборки"), [&] {
    App::ImportBuildOptions opts;
    opts.profileName = prof.trimmed().toStdString();
    res  = m_app.importBuild(dir.toStdString(), opts);
    done = true;
  });
  QApplication::restoreOverrideCursor();

  if (done) {
    QString msg = lang::T("Импортирована сборка в профиль «%1»: %2 модов "
                          "(%3 loose, %4 в IMG; %5 пропущено).")
                      .arg(QString::fromStdString(res.profile))
                      .arg(static_cast<int>(res.createdModIds.size()))
                      .arg(res.looseChanged)
                      .arg(res.imgChanged)
                      .arg(res.skipped);
    for (const auto& n : res.notes)
      msg += "\n" + QString::fromStdString(n);

    QString details;
    if (!res.skippedFiles.empty()) {
      details = lang::T("Пропущенные файлы (%1):")
                    .arg(static_cast<int>(res.skippedFiles.size())) +
               "\n\n";
      for (const auto& f : res.skippedFiles)
        details += QString::fromStdString(f.rel) + "  —  " +
                   QString::fromStdString(f.reason) + "\n";
    }
    dialogs::information(this, lang::T("Импорт сборки"), msg, details);
  }
  refreshProfiles();
  refreshMods();
  updateStatusBar();
}

void MainWindow::onExportBuild()
{
  const std::string profile = m_app.activeProfile();
  if (profile.empty()) {
    dialogs::information(this, lang::T("Экспорт сборки"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }

  // Ask what to bundle: savegames and/or game settings (off by default).
  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Экспорт сборки"));
  auto* body = frameless(dlg);
  auto* form = new QFormLayout;
  body->addLayout(form);
  form->addRow(new QLabel(
      lang::T("Что включить в архив сборки (профиль и моды включены всегда):"),
      &dlg));
  auto* cbSaves = new QCheckBox(lang::T("Сохранения профиля (*.b)"), &dlg);
  auto* cbSettings = new QCheckBox(lang::T("Настройки игры (gta_sa.set)"), &dlg);
  form->addRow(cbSaves);
  form->addRow(cbSettings);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                   &dlg);
  form->addRow(box);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  centerDialog(dlg);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const bool withSaves    = cbSaves->isChecked();
  const bool withSettings = cbSettings->isChecked();

  const QString suggested = QString::fromStdString(profile) + ".gmmbuild.zip";
  const QString zip = QFileDialog::getSaveFileName(
      this, lang::T("Экспорт сборки в zip"), suggested,
      lang::T("Сборки GMM (*.zip)"));
  if (zip.isEmpty())
    return;

  bool done = false;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  statusBar()->showMessage(lang::T("Упаковка сборки…"));
  guarded(lang::T("Экспорт сборки"), [&] {
    m_app.exportBuild(profile, zip.toStdString(), withSaves, withSettings);
    done = true;
  });
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();

  if (done)
    dialogs::information(
        this, lang::T("Экспорт сборки"),
        lang::T("Сборка экспортирована:") + "\n" + zip);
}

void MainWindow::onImportBuildZip()
{
  const QString zip = QFileDialog::getOpenFileName(
      this, lang::T("Импорт сборки из zip"), QString(),
      lang::T("Сборки GMM (*.zip)"));
  if (zip.isEmpty())
    return;
  if (!importBuildZipInteractive(zip).isEmpty()) {
    refreshProfiles();
    refreshMods();
    updateStatusBar();
  }
}

QString MainWindow::importBuildZipInteractive(const QString& zipPath)
{
  // Pick a target profile name (new or existing — existing is overwritten).
  bool ok = false;
  const QString suggested =
      QString::fromStdString(fs::path(zipPath.toStdString()).stem().stem().string());
  const QString prof = dialogs::getText(
      this, lang::T("Импорт сборки"),
      lang::T("Профиль для импорта (новый или существующий — будет перезаписан):"),
      QLineEdit::Normal, suggested, &ok);
  if (!ok || prof.trimmed().isEmpty())
    return {};

  const std::string profName = prof.trimmed().toStdString();
  const auto existing = m_app.profileNames();
  if (std::find(existing.begin(), existing.end(), profName) != existing.end()) {
    if (dialogs::question(
            this, lang::T("Импорт сборки"),
            lang::T("Профиль «%1» уже существует. Перезаписать его моды и порядок?")
                .arg(prof.trimmed())) != QMessageBox::Yes)
      return {};
  }

  App::ImportBuildArchiveResult res;
  bool done = false;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  statusBar()->showMessage(lang::T("Распаковка сборки…"));
  guarded(lang::T("Импорт сборки"), [&] {
    res  = m_app.importBuildArchive(zipPath.toStdString(), profName);
    done = true;
  });
  QApplication::restoreOverrideCursor();
  statusBar()->clearMessage();

  if (!done)
    return {};

  QString msg = lang::T("Импортирована сборка в профиль «%1»: %2 модов добавлено, "
                        "%3 переиспользовано.")
                    .arg(QString::fromStdString(res.profile))
                    .arg(res.modsAdded)
                    .arg(res.modsReused);
  for (const auto& n : res.notes)
    msg += "\n" + QString::fromStdString(n);
  dialogs::information(this, lang::T("Импорт сборки"), msg);
  return QString::fromStdString(res.profile);
}

void MainWindow::onAddSeparator()
{
  if (m_app.activeProfile().empty()) {
    dialogs::information(this, lang::T("Разделитель"),
                             lang::T("Сначала создайте или выберите профиль."));
    return;
  }
  bool ok = false;
  const QString name =
      dialogs::getText(this, lang::T("Добавить разделитель"), lang::T("Название:"),
                            QLineEdit::Normal, lang::T("Новый раздел"), &ok);
  if (!ok || name.isEmpty())
    return;
  if (m_sortColumn != -1)
    refreshMods();  // adding to the order needs the manual (priority) view
  m_populating = true;
  auto* it = new ModItem;
  styleSeparatorItem(it, name);
  m_mods->insertTopLevelItem(0, it);
  renumberPriorities();
  m_populating = false;
  persistOrder();
  updateDerivedColumns();
}

void MainWindow::onDeploy()
{
  // Re-check the embedded Mod Loader runtime is actually unpacked on disk. It
  // normally happens once at startup, but re-running here (cheap -- files that
  // already match are skipped) rides out a one-off failure on a fresh install
  // (e.g. Defender/SmartScreen blocking the very first write from a freshly
  // downloaded, unrecognized exe) instead of leaving Mod Loader silently unable
  // to install on the first attempt.
  materializeRuntime();
  guarded(lang::T("Развёртывание"), [&] {
    m_app.deploy();
    QString msg = lang::T("Развёрнуто файлов: %1 (+ IMG) в игру")
                      .arg(m_app.currentManifest().files.size());
    // Mod Loader was requested but nothing actually ended up in the game
    // folder -- most likely the bundled runtime failed to unpack next to the
    // program (see materializeRuntime()). Say so instead of a silent success.
    std::error_code ec;
    if (m_app.enableModloader() &&
        !fs::exists(fs::path(m_app.gamePath()) / "modloader.asi", ec)) {
      msg += "  " + lang::T("⚠ Mod Loader не установился — рантайм не найден "
                            "(см. Настройки → Mod Loader).");
    }
    statusBar()->showMessage(msg, 8000);
  });
  populateExecutables();  // an exe that just landed on disk should show up now
  updateStatusBar();
}

void MainWindow::onRollback()
{
  // Checked before rollback() (its own state doesn't change either way) so the
  // status message below can say whether the orphan-cleanup part of "keep the
  // game folder clean" actually ran, instead of leaving that silently unclear.
  const bool orphanCleanupSkipped = m_app.manageGenerated() && !m_app.hasBaseline();
  guarded(lang::T("Откат"), [&] {
    m_app.rollback();
    statusBar()->showMessage(
        orphanCleanupSkipped
            ? lang::T("Откат выполнен, но посторонние файлы НЕ убраны — нет "
                      "эталона ванили (Настройки → Развёртывание)")
            : lang::T("Откат выполнен — папка игры восстановлена"),
        6000);
  });
  populateExecutables();
  updateStatusBar();
}

void MainWindow::onPlay()
{
  if (m_playRunning)
    return;
  // See onDeploy(): re-check the embedded runtime is unpacked before every
  // deploy, not just once at startup.
  materializeRuntime();
  const std::string exe = m_runExe ? m_runExe->currentText().toStdString() : "";

  // Run the whole deploy → launch → wait → rollback cycle on a worker thread so
  // the window stays responsive (and visibly locked) while the game is running,
  // instead of freezing as "Not Responding". The launcher is locked meanwhile;
  // guarded() drops any stray action triggers until the game exits.
  m_playRunning = true;
  setPlayLock(true);
  // Surface logs automatically so the user can watch the game live while locked.
  if (m_logDock && !m_logDock->isVisible())
    m_logDock->show();
  onRefreshLogs();
  updateStatusBar();
  statusBar()->showMessage(lang::T("Игра запущена — лаунчер заблокирован…"));

  QThread* worker = QThread::create([this, exe] {
    std::string err;
    try {
      m_app.play(/*rollbackAfter=*/true, exe);
    } catch (const std::exception& e) {
      err = e.what();
    }
    QMetaObject::invokeMethod(
        this,
        [this, err] {
          m_playRunning = false;
          setPlayLock(false);
          if (!err.empty())
            dialogs::warning(this, lang::T("Запуск"),
                                 QString::fromLocal8Bit(err.c_str()));
          // play() rolls back internally on the way out -- same orphan-cleanup
          // caveat as the explicit Rollback button (see onRollback()).
          if (err.empty() && m_app.manageGenerated() && !m_app.hasBaseline())
            statusBar()->showMessage(
                lang::T("Откат выполнен, но посторонние файлы НЕ убраны — нет "
                        "эталона ванили (Настройки → Развёртывание)"),
                6000);
          else
            statusBar()->clearMessage();
          // Rebuild the list: rolls back any visual-only edits made while unlocked
          // and re-enables drag-reorder.
          refreshMods();
          updateStatusBar();
        },
        Qt::QueuedConnection);
  });
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
}

void MainWindow::onCreateShortcut()
{
#ifdef _WIN32
  // Create a desktop .lnk that launches straight into the game for this
  // instance -- deploy -> launch -> wait -> rollback using whatever profile
  // is currently active, with no manager window at all (MO2-style quick
  // launcher). `--play` is handled in main_gui.cpp; it never touches the
  // instance picker or the welcome screen, only requires --data. Built via
  // the Shell's IShellLink COM object.
  const QString desktop =
      QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  if (desktop.isEmpty()) {
    dialogs::warning(this, lang::T("Ярлык"),
                         lang::T("Не удалось определить папку рабочего стола."));
    return;
  }
  const QString exePath = QCoreApplication::applicationFilePath();
  const QString workDir = QCoreApplication::applicationDirPath();
  const QString args = "--data \"" + QString::fromStdString(m_app.dataDir().string()) +
                       "\" --play";
  const QString instance =
      QString::fromStdString(m_app.dataDir().filename().string());
  const QString linkName = "GMM — " + instance + ".lnk";
  const QString linkPath = QDir(desktop).absoluteFilePath(linkName);

  bool ok = false;
  const bool needUninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  IShellLinkW* link = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IShellLinkW, reinterpret_cast<void**>(&link)))) {
    link->SetPath(reinterpret_cast<const wchar_t*>(exePath.utf16()));
    link->SetArguments(reinterpret_cast<const wchar_t*>(args.utf16()));
    link->SetWorkingDirectory(reinterpret_cast<const wchar_t*>(workDir.utf16()));
    link->SetIconLocation(reinterpret_cast<const wchar_t*>(exePath.utf16()), 0);
    link->SetDescription(reinterpret_cast<const wchar_t*>(
        QString(lang::T("GMM — быстрый запуск игры (") + instance + ")").utf16()));
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                       reinterpret_cast<void**>(&file)))) {
      ok = SUCCEEDED(file->Save(reinterpret_cast<const wchar_t*>(linkPath.utf16()), TRUE));
      file->Release();
    }
    link->Release();
  }
  if (needUninit)
    CoUninitialize();

  if (ok)
    statusBar()->showMessage(
        lang::T("Ярлык создан на рабочем столе: ") + linkName, 5000);
  else
    dialogs::warning(this, lang::T("Ярлык"),
                         lang::T("Не удалось создать ярлык."));
#else
  dialogs::information(this, lang::T("Ярлык"),
                           lang::T("Создание ярлыков доступно только в Windows."));
#endif
}

void MainWindow::onRefresh()
{
  refreshProfiles();
  populateExecutables();
  refreshMods();
  updateStatusBar();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls())
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const auto urls = event->mimeData()->urls();
  bool importedBuild = false;
  for (const QUrl& u : urls) {
    if (!u.isLocalFile())
      continue;
    const QString path = u.toLocalFile();
    // A zip exported by exportBuild() (profile + mods + notes) is a different
    // beast from a plain mod archive -- it needs a target profile name and
    // creates/overwrites a whole profile, so route it to the same interactive
    // flow as "Импорт сборки из zip…" instead of importing it as one mod.
    if (m_app.isBuildArchive(path.toStdString())) {
      if (!importBuildZipInteractive(path).isEmpty())
        importedBuild = true;
    } else {
      std::error_code ec;
      const bool isDir = fs::is_directory(path.toStdString(), ec);
      const QString defaultName =
          isDir ? QFileInfo(path).fileName() : QFileInfo(path).completeBaseName();
      bool viaMl = false;
      QString name;
      if (askImportMode(viaMl, name, defaultName))
        importPath(path, viaMl, name);
    }
  }
  if (importedBuild) {
    refreshProfiles();
    refreshMods();
    updateStatusBar();
  }
}

void MainWindow::onConflicts()
{
  // Kept for menu compatibility: jump to the Conflicts tab of the selection.
}

namespace {

// Recreate the tree embedded under `qrcPrefix` (e.g. ":/runtime") under `dir` on
// disk, skipping files already present with the same size (so a big gta_sa.exe
// isn't rewritten every launch). Returns true if `markerFile` (e.g.
// "modloader.asi") ended up in place, i.e. the bundle is actually usable there.
bool unpackEmbeddedRuntime(const QString& qrcPrefix, const std::string& markerFile,
                          const fs::path& dir)
{
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec)
    return false;
  const QDir base(qrcPrefix);
  // AllEntries + Hidden so hidden dirs like ".data" are recursed into.
  QDirIterator it(qrcPrefix, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    const QFileInfo fi = it.fileInfo();
    if (!fi.isFile())
      continue;
    const QString rel     = base.relativeFilePath(fi.filePath());
    const fs::path target = dir / rel.toStdString();
    QFile rf(fi.filePath());
    if (!rf.open(QIODevice::ReadOnly))
      continue;
    std::error_code e2;
    if (fs::exists(target, e2) &&
        static_cast<qint64>(fs::file_size(target, e2)) == rf.size())
      continue;  // already up to date
    fs::create_directories(target.parent_path(), e2);
    QFile out(QString::fromStdString(target.string()));
    if (out.open(QIODevice::WriteOnly)) {
      out.write(rf.readAll());
      out.close();
    }
  }
  return fs::exists(dir / markerFile, ec);
}

}  // namespace

void materializeEmbeddedRuntime(const fs::path& dataDir)
{
  // Everything embedded via the generated runtime.qrc lives under ":/runtime/",
  // preserving the original tree (including modloader/.data/... and gta_sa.exe).
  // Free function (not a MainWindow member) so it's also reachable from
  // main_gui.cpp's "--play" shortcut path, which never constructs a
  // MainWindow at all -- both need the runtime unpacked before deploy() can
  // find modloaderRuntimeDir()/sampRuntimeDir() on a truly fresh install.
  if (QDir(":/runtime").exists()) {
    // Prefer a folder next to the program (portable, and the core's default
    // runtime location); fall back to the writable instance data dir.
    if (!unpackEmbeddedRuntime(":/runtime", "modloader.asi", gtamm::executableDir() / "runtime"))
      unpackEmbeddedRuntime(":/runtime", "modloader.asi", dataDir / "modloader-runtime");
  }

  // Same idea for the bundled SA-MP client (see App::deploySampRuntime()).
  if (QDir(":/runtime-samp").exists()) {
    if (!unpackEmbeddedRuntime(":/runtime-samp", "samp.exe",
                              gtamm::executableDir() / "runtime-samp"))
      unpackEmbeddedRuntime(":/runtime-samp", "samp.exe", dataDir / "samp-runtime");
  }

  // And for Valve's steam_api64.dll, which App::launchGame() loads to show the
  // session in Steam (see App::steamApiDll(), which looks in exactly these two
  // places).
  if (QDir(":/runtime-steam").exists()) {
    const std::string dll = gtamm::steam::apiDllName();
    if (!unpackEmbeddedRuntime(":/runtime-steam", dll,
                               gtamm::executableDir() / "runtime-steam"))
      unpackEmbeddedRuntime(":/runtime-steam", dll, dataDir / "steam-runtime");
  }
}

void MainWindow::materializeRuntime()
{
  materializeEmbeddedRuntime(m_app.dataDir());
}

void MainWindow::onSettings()
{
  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Настройки"));
  dlg.resize(600, 470);  // a bit taller: less scrolling on the fuller tabs
  dlg.setMinimumSize(520, 320);

  auto* tabs = new QTabWidget(&dlg);

  // Each tab's own content no longer has to fit inside the (now shorter)
  // dialog -- it's wrapped in its own QScrollArea below, so only that tab's
  // page scrolls while the tab bar itself stays put.
  auto wrapScrollable = [](QWidget* content) {
    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
  };

  // Small gray wrapped hint under a control.
  auto hint = [](const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color: gray; font-size: 11px;");
    return l;
  };

  // ---- Tab 1: General (appearance + language) -----------------------------
  {
    auto* tab = new QWidget;
    auto* v   = new QVBoxLayout(tab);

    // Header: icon + name + version, same look as the About dialog — gives the
    // tab some visual weight instead of just two combo boxes floating in a
    // mostly-empty page.
    auto* header = new QHBoxLayout;
    auto* headerIcon = new QLabel(tab);
    headerIcon->setPixmap(QApplication::windowIcon().pixmap(48, 48));
    headerIcon->setFixedSize(48, 48);
    header->addWidget(headerIcon);
    auto* headerText = new QLabel(
        QString("<b style='font-size:15pt'>GMM</b><br><span style='color:gray'>%1 %2</span>")
            .arg(lang::T("Версия"))
            .arg(gtamm::versionString()),
        tab);
    header->addWidget(headerText, 1);
    v->addLayout(header);

    auto* headerLine = new QFrame;
    headerLine->setFrameShape(QFrame::HLine);
    headerLine->setFrameShadow(QFrame::Sunken);
    v->addWidget(headerLine);

    auto* look = new QGroupBox(lang::T("Внешний вид"));
    auto* form = new QFormLayout(look);

    // Theme.
    auto* themeCombo = new QComboBox;
    std::vector<std::pair<QString, QString>> themes = {
        {lang::T("Тёмная"), "dark"}, {lang::T("Светлая"), "light"}};
    {
      std::error_code ec;
      fs::create_directories(themesDir(), ec);
      QDir tdir(QString::fromStdString(themesDir().string()));
      for (const QString& f : tdir.entryList({"*.qss"}, QDir::Files, QDir::Name))
        themes.push_back({QFileInfo(f).completeBaseName(), tdir.absoluteFilePath(f)});
    }
    for (const auto& [label, id] : themes)
      themeCombo->addItem(label, id);
    for (int i = 0; i < themeCombo->count(); ++i)
      if (themeCombo->itemData(i).toString() == m_themeId)
        themeCombo->setCurrentIndex(i);
    connect(themeCombo, &QComboBox::currentIndexChanged, this, [this, themeCombo] {
      applyTheme(themeCombo->currentData().toString());
    });
    form->addRow(lang::T("Тема:"), themeCombo);

    auto* themesFolderBtn = new QPushButton(lang::T("Открыть папку тем"));
    connect(themesFolderBtn, &QPushButton::clicked, this, [this] {
      std::error_code e;
      fs::create_directories(themesDir(), e);
      QDesktopServices::openUrl(
          QUrl::fromLocalFile(QString::fromStdString(themesDir().string())));
    });
    form->addRow(QString(), themesFolderBtn);

    // Language.
    auto* langCombo = new QComboBox;
    langCombo->addItem("Русский", static_cast<int>(lang::Language::Russian));
    langCombo->addItem("English", static_cast<int>(lang::Language::English));
    langCombo->setCurrentIndex(lang::language() == lang::Language::English ? 1 : 0);
    connect(langCombo, &QComboBox::currentIndexChanged, this, [this, langCombo] {
      const auto chosen = static_cast<lang::Language>(langCombo->currentData().toInt());
      if (lang::language() == chosen)
        return;
      lang::save(chosen);
      dialogs::information(this, lang::T("Сменить язык"),
                              lang::T("Язык изменится после перезапуска программы."));
    });
    form->addRow(lang::T("Язык:"), langCombo);

    v->addWidget(look);
    v->addWidget(hint(lang::T("Тема применяется сразу. Язык — после перезапуска.")));

    // Quick-glance instance info (otherwise only visible in the status bar).
    auto* instGroup = new QGroupBox(lang::T("Текущий инстанс"));
    auto* instForm  = new QFormLayout(instGroup);
    auto* gamePathLbl = new QLabel(
        m_app.gamePath().empty() ? lang::T("(не задана)")
                                 : QString::fromStdString(m_app.gamePath()),
        instGroup);
    gamePathLbl->setWordWrap(true);
    gamePathLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    instForm->addRow(lang::T("Папка игры:"), gamePathLbl);
    auto* dataDirLbl = new QLabel(
        QString::fromStdString(m_app.dataDir().string()), instGroup);
    dataDirLbl->setWordWrap(true);
    dataDirLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    instForm->addRow(lang::T("Папка данных:"), dataDirLbl);
    instForm->addRow(lang::T("Активный профиль:"),
                     new QLabel(m_app.activeProfile().empty()
                                    ? lang::T("(не выбран)")
                                    : QString::fromStdString(m_app.activeProfile()),
                                instGroup));
    v->addWidget(instGroup);

    v->addStretch(1);
    tabs->addTab(wrapScrollable(tab), lang::T("Общие"));
  }

  // ---- Tab 2: Deployment (saves / clean folder / maps / SA-MP) ------------
  {
    auto* tab = new QWidget;
    auto* v   = new QVBoxLayout(tab);

    auto* store = new QGroupBox(lang::T("Сохранения и чистота"));
    auto* sv    = new QVBoxLayout(store);

    auto* saves = new QCheckBox(lang::T("Свои сохранения для профилей"));
    saves->setChecked(m_app.manageSaves());
    connect(saves, &QCheckBox::toggled, this, &MainWindow::onToggleManageSaves);
    sv->addWidget(saves);
    sv->addWidget(hint(lang::T(
        "Каждый профиль получает свою папку GTA San Andreas User Files (сейвы + "
        "настройки), подменяемую на время игры.")));
    auto* savesBtn = new QPushButton(lang::T("Открыть папку сохранений профиля"));
    connect(savesBtn, &QPushButton::clicked, this, &MainWindow::onOpenSavesFolder);
    sv->addWidget(savesBtn);

    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    sv->addWidget(line);

    auto* gen = new QCheckBox(lang::T("Держать папку игры чистой"));
    gen->setChecked(m_app.manageGenerated());
    connect(gen, &QCheckBox::toggled, this, &MainWindow::onToggleManageGenerated);
    sv->addWidget(gen);
    sv->addWidget(hint(lang::T(
        "Файлы, создаваемые модами/игрой в папке игры, после выхода убираются в "
        "стор профиля и возвращаются перед следующим запуском — папка игры "
        "остаётся в исходном состоянии.")));
    auto* genBtn = new QPushButton(lang::T("Открыть папку созданных файлов профиля"));
    connect(genBtn, &QPushButton::clicked, this, &MainWindow::onOpenGeneratedFolder);
    sv->addWidget(genBtn);

    // "Держать папку игры чистой" above only manages files created DURING a
    // play session; on rollback it ALSO sweeps up anything left in the folder
    // that predates GMM's own tracking entirely (leftover CLEO.asi/cleo/ etc.
    // never deployed by any manifest) -- but only if a vanilla baseline is
    // cached (used by import-build's diff too). Without one, that sweep is a
    // silent no-op, which is exactly what caused stray files to keep coming
    // back with no indication why -- so surface the status directly here.
    auto* baselineStatus = new QLabel;
    baselineStatus->setWordWrap(true);
    auto refreshBaselineStatus = [this, baselineStatus] {
      if (m_app.hasBaseline())
        baselineStatus->setText(
            "<span style='color:#2e9e46'>● " +
            lang::T("Эталон ванили есть — откат убирает и посторонние файлы, "
                    "не относящиеся ни к одному деплою.") +
            "</span>");
      else
        baselineStatus->setText(
            "<span style='color:#c0392b'>● " +
            lang::T("Эталон ванили не построен — откат НЕ убирает файлы, "
                    "оставшиеся вне манифеста деплоя (см. кнопку ниже).") +
            "</span>");
    };
    refreshBaselineStatus();
    sv->addWidget(baselineStatus);
    auto* baselineBtn = new QPushButton(lang::T("Построить эталон ванили сейчас"));
    connect(baselineBtn, &QPushButton::clicked, this, [this, refreshBaselineStatus] {
      if (dialogs::question(
              this, lang::T("Эталон ванили"),
              lang::T("Стройте эталон, ТОЛЬКО когда папка игры сейчас действительно "
                      "чистая: ничего не задеплоено и нет посторонних файлов. Он "
                      "запоминает текущее состояние папки как «ваниль» — если в ней "
                      "уже что-то лишнее, откат будет считать это нормой и не "
                      "уберёт. Продолжить?"),
              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
      guarded(lang::T("Эталон ванили"), [&] { m_app.refreshVanillaBaseline(); });
      refreshBaselineStatus();
    });
    sv->addWidget(baselineBtn);

    v->addWidget(store);

    // Default settings template: a captured gta_sa.set (resolution/controls) that
    // gets seeded into any profile/instance that doesn't have one yet, so a
    // brand-new profile starts already configured instead of showing the game's
    // own first-run setup.
    auto* tmplBox    = new QGroupBox(lang::T("Настройки при первом запуске"));
    auto* tv         = new QVBoxLayout(tmplBox);
    auto* tmplStatus = new QLabel;
    auto refreshTmplStatus = [this, tmplStatus] {
      tmplStatus->setText(
          m_app.hasDefaultSettingsTemplate()
              ? lang::T("Шаблон сохранён — новые профили получат его "
                        "автоматически на первом запуске.")
              : lang::T("Шаблон не задан — новые профили получат настройки "
                        "игры по умолчанию."));
    };
    refreshTmplStatus();
    tv->addWidget(tmplStatus);
    tv->addWidget(hint(lang::T(
        "Один раз настрой разрешение экрана и управление в самой игре, затем "
        "нажми «Сохранить». Дальше при первом запуске КАЖДОГО нового профиля "
        "или инстанса эти настройки (gta_sa.set) подставятся сами до старта "
        "игры — мастер настройки экрана не появится.")));
    auto* tmplRow     = new QHBoxLayout;
    auto* saveTmplBtn = new QPushButton(lang::T("Сохранить текущие настройки как шаблон"));
    connect(saveTmplBtn, &QPushButton::clicked, this, [this, refreshTmplStatus] {
      guarded(lang::T("Шаблон настроек"), [&] { m_app.saveCurrentSettingsAsTemplate(); });
      refreshTmplStatus();
    });
    auto* clearTmplBtn = new QPushButton(lang::T("Очистить шаблон"));
    connect(clearTmplBtn, &QPushButton::clicked, this, [this, refreshTmplStatus] {
      guarded(lang::T("Шаблон настроек"), [&] { m_app.clearDefaultSettingsTemplate(); });
      refreshTmplStatus();
    });
    tmplRow->addWidget(saveTmplBtn);
    tmplRow->addWidget(clearTmplBtn);
    tmplRow->addStretch(1);
    tv->addLayout(tmplRow);
    v->addWidget(tmplBox);

    auto* maps = new QGroupBox(lang::T("Карты (экспериментально)"));
    auto* mv   = new QVBoxLayout(maps);
    auto* auto_ = new QCheckBox(lang::T("Авто-установка карт"));
    auto_->setChecked(m_app.autoRouteMaps());
    connect(auto_, &QCheckBox::toggled, this, &MainWindow::onToggleAutoMaps);
    mv->addWidget(auto_);
    mv->addWidget(hint(lang::T(
        "Вставлять новые модели/текстуры карт в gta3.img и регистрировать .ide/.ipl "
        "в gta.dat. По умолчанию ВЫКЛ: может ронять новую игру при конфликте ID "
        "объектов мода. Без неё файлы карт просто кладутся как есть.")));
    v->addWidget(maps);

    auto* steam   = new QGroupBox(lang::T("Steam"));
    auto* steamLay = new QVBoxLayout(steam);
    auto* steamCb = new QCheckBox(
        lang::T("Показывать в Steam, что я играю в GTA San Andreas"));
    steamCb->setChecked(m_app.steamIntegration());
    connect(steamCb, &QCheckBox::toggled, this, &MainWindow::onToggleSteamIntegration);
    steamLay->addWidget(steamCb);
    steamLay->addWidget(hint(lang::T(
        "Пока идёт игра, GMM подключается к запущенному Steam под настоящим "
        "AppID Grand Theft Auto: San Andreas — как это делает Steam Achievement "
        "Manager. У друзей и в клиенте видно «играет в Grand Theft Auto: San "
        "Andreas», время идёт в счёт настоящей страницы игры. Библиотеку Steam "
        "(steam_api64.dll) GMM подхватывает сам — из установленных у вас игр "
        "Steam, так что настраивать обычно нечего. Ничего в файлах "
        "Steam и в папке игры не меняется. Работает ТОЛЬКО если GTA San "
        "Andreas куплена на этом аккаунте Steam — иначе клиент не примет "
        "сессию, и игра просто запустится без статуса. Оверлея (Shift+Tab, "
        "скриншоты F12) не будет: его Steam добавляет только тем процессам, "
        "которые запустил сам. По умолчанию выключено.")));

    auto* steamStatusLabel = new QLabel;
    steamStatusLabel->setWordWrap(true);
    steamStatusLabel->setTextFormat(Qt::RichText);
    steamStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    steamLay->addWidget(steamStatusLabel);

    auto* steamRow = new QHBoxLayout;
    auto* steamPick = new QPushButton(lang::T("Указать steam_api64.dll…"));
    steamRow->addWidget(steamPick);
    steamRow->addStretch(1);
    steamLay->addLayout(steamRow);
    v->addWidget(steam);

    auto refreshSteam = [this, steamStatusLabel] {
      // Note: this resolves the DLL, which on a machine with Steam may scan
      // the Steam libraries once (result cached in config.json afterwards).
      const App::SteamStatus st = m_app.steamStatus();
      QStringList lines;
      lines << (st.installed
                    ? QString("<span style='color:#2e9e46'>%1</span>")
                          .arg(st.running ? lang::T("Steam найден и запущен.")
                                          : lang::T("Steam найден (сейчас не запущен — "
                                                    "запустите его до начала игры)."))
                    : QString("<span style='color:#c0653a'>%1</span>")
                          .arg(lang::T("Steam на этом компьютере не найден.")));
      // The Steamworks redistributable is loaded at runtime but deliberately
      // not shipped (it's Valve's, not ours): normally it is borrowed from a
      // game already installed through Steam, and only if that fails does the
      // user have to supply one -- so say which of the two happened.
      if (st.hasApiDll && st.apiDllFromSteam)
        lines << QString("<span style='color:#2e9e46'>%1</span>")
                     .arg(lang::T("Библиотека Steam взята автоматически из "
                                  "установленной игры: ") +
                          QString::fromStdString(st.apiDllPath));
      else if (st.hasApiDll)
        lines << QString("<span style='color:#2e9e46'>%1</span>")
                     .arg(lang::T("Библиотека Steam найдена: ") +
                          QString::fromStdString(st.apiDllPath));
      else
        lines << QString("<span style='color:#c0653a'>%1</span>")
                     .arg(lang::T("Нет steam_api64.dll — положите её сюда: ") +
                          QString::fromStdString(st.apiDllPath));
      steamStatusLabel->setText(lines.join("<br>"));
    };
    refreshSteam();

    connect(steamPick, &QPushButton::clicked, this, [this, refreshSteam] {
      const QString path = QFileDialog::getOpenFileName(
          this, lang::T("Выберите steam_api64.dll"), QString(),
          lang::T("Библиотека Steam (steam_api64.dll)"));
      if (path.isEmpty())
        return;
      guarded(lang::T("Steam"),
              [&] { m_app.setSteamApiDll(path.toStdString()); });
      refreshSteam();
    });

    auto* mp     = new QGroupBox(lang::T("Мультиплеер"));
    auto* mpv    = new QHBoxLayout(mp);
    auto* sampBtn = new QPushButton(lang::T("Настройки SA-MP (мультиплеер)…"));
    connect(sampBtn, &QPushButton::clicked, this, &MainWindow::onSampSettings);
    mpv->addWidget(sampBtn);
    mpv->addStretch(1);
    v->addWidget(mp);

    // --- external tools: Sanny Builder 4 -----------------------------------
    // Not bundled and not touched by deployment: GMM only remembers where the
    // user's own copy lives and offers a toolbar button that starts it.
    auto* toolsBox = new QGroupBox(lang::T("Внешние программы"));
    auto* toolsV   = new QVBoxLayout(toolsBox);

    auto* sbRow  = new QHBoxLayout;
    auto* sbEdit = new QLineEdit(QString::fromStdString(m_app.sannyBuilderPath()));
    sbEdit->setPlaceholderText(
        lang::T("Папка Sanny Builder 4 (например C:\\...\\SannyBuilder-v4.2.0)"));
    auto* sbBrowse = new QPushButton(lang::T("Обзор…"));
    sbRow->addWidget(new QLabel(lang::T("Sanny Builder 4:")));
    sbRow->addWidget(sbEdit, 1);
    sbRow->addWidget(sbBrowse);
    toolsV->addLayout(sbRow);

    auto* sbCb = new QCheckBox(lang::T("Показывать Sanny Builder на панели"));
    sbCb->setChecked(m_app.showSannyBuilder());
    toolsV->addWidget(sbCb);

    // Where compiled scripts should land. Deliberately a mod in the pool and
    // not the game folder -- see App::sannyCleoModId().
    auto* modRow  = new QHBoxLayout;
    auto* sbModCb = new QComboBox;
    sbModCb->addItem(lang::T("— не менять (папка игры) —"), QString());
    for (const auto& m : m_app.mods())
      sbModCb->addItem(QString::fromStdString(m.name),
                       QString::fromStdString(m.id));
    {
      const int at = sbModCb->findData(QString::fromStdString(m_app.sannyCleoModId()));
      sbModCb->setCurrentIndex(at >= 0 ? at : 0);
    }
    modRow->addWidget(new QLabel(lang::T("Компилировать CLEO в мод:")));
    modRow->addWidget(sbModCb, 1);
    toolsV->addLayout(modRow);

    auto* sbStatus = new QLabel;
    sbStatus->setWordWrap(true);
    sbStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolsV->addWidget(sbStatus);

    // One place that writes the path + repaints the status, so typing a path,
    // browsing for one and toggling the checkbox can't disagree about what is
    // currently configured.
    auto applySanny = [this, sbEdit, sbCb, sbModCb, sbStatus] {
      guarded(lang::T("Sanny Builder"), [&] {
        m_app.setSannyBuilderPath(sbEdit->text().trimmed().toStdString());
        m_app.setShowSannyBuilder(sbCb->isChecked());
        m_app.setSannyCleoModId(sbModCb->currentData().toString().toStdString());
      });
      const fs::path exe = m_app.sannyBuilderExe();
      if (m_app.sannyBuilderPath().empty())
        sbStatus->setText(QString("<span style='color:gray'>%1</span>")
                              .arg(lang::T("Путь не указан.")));
      else if (exe.empty())
        sbStatus->setText(
            QString("<span style='color:#c0653a'>%1</span>")
                .arg(lang::T("Sanny Builder не найден по этому пути (ожидается "
                             "sanny.exe).")));
      else
        sbStatus->setText(QString("<span style='color:#2e9e46'>%1</span>")
                              .arg(lang::T("Найден: ") +
                                   QString::fromStdString(exe.string())));
      updateExternalToolActions();
    };
    applySanny();

    connect(sbEdit, &QLineEdit::editingFinished, this, applySanny);
    connect(sbCb, &QCheckBox::toggled, this, [applySanny](bool) { applySanny(); });
    connect(sbModCb, &QComboBox::currentIndexChanged, this,
            [applySanny](int) { applySanny(); });
    connect(sbBrowse, &QPushButton::clicked, this, [this, sbEdit, applySanny] {
      // A folder, because that's the shape Sanny Builder ships in and what
      // people have on disk; a path typed straight to the exe works too, which
      // is why sannyBuilderExe() accepts either.
      const QString dir = QFileDialog::getExistingDirectory(
          this, lang::T("Папка Sanny Builder 4"), sbEdit->text());
      if (dir.isEmpty())
        return;
      sbEdit->setText(QDir::toNativeSeparators(dir));
      applySanny();
    });

    toolsV->addWidget(hint(lang::T(
        "Sanny Builder не входит в GMM и не участвует в развёртывании — "
        "программа только запоминает, где лежит ваша копия, и добавляет кнопку "
        "запуска на панель (рядом с SAMP). Кнопка появляется, только если "
        "галочка включена и по указанному пути действительно есть "
        "sanny.exe.")));
    toolsV->addWidget(hint(lang::T(
        "При запуске по кнопке GMM передаёт в Sanny Builder папку игры этого "
        "инстанса (settings.ini → GamePath — оттуда он берёт gta.dat, "
        "american.gxt и подсказки) и, если выбран мод, перенаправляет вывод "
        "компиляции CLEO в mods/<мод>/root/CLEO (mode.xml текущего режима). "
        "Компилировать прямо в папку игры не стоит: она создаётся "
        "развёртыванием, и откат такой скрипт удалит, а перезапись "
        "задеплоенного файла рвёт жёсткую ссылку — мод в пуле останется со "
        "старой версией.")));
    v->addWidget(toolsBox);

    v->addStretch(1);
    tabs->addTab(wrapScrollable(tab), lang::T("Развёртывание"));
  }

  // ---- Tab 3: Mod Loader --------------------------------------------------
  {
    auto* tab = new QWidget;
    auto* v   = new QVBoxLayout(tab);

    // The single checkbox: turns Mod Loader on for the instance regardless of
    // whether any mod is individually flagged "via Modloader" -- so a bare
    // build with no mods imported yet still gets a working Modloader + CLEO +
    // bundled compatibility fixes on the next deploy.
    auto* mlCb = new QCheckBox(lang::T("Включить Mod Loader"));
    mlCb->setChecked(m_app.enableModloader());
    connect(mlCb, &QCheckBox::toggled, this, [this](bool on) {
      guarded(lang::T("Mod Loader"), [&] { m_app.setEnableModloader(on); });
    });
    v->addWidget(mlCb);
    v->addWidget(hint(lang::T(
        "Ставит в игру рантайм Modloader (сам Modloader, CLEO и его "
        "расширения, снятие DEP) при каждом развёртывании — даже если ни один "
        "мод не помечен «через Modloader». Откат убирает всё обратно. Встроено "
        "в программу, ничего скачивать не нужно.")));

    auto* esCb = new QCheckBox(lang::T("Устанавливать встроенные фиксы (_ESSENTIALS)"));
    esCb->setChecked(m_app.enableEssentials());
    connect(esCb, &QCheckBox::toggled, this, [this](bool on) {
      guarded(lang::T("Mod Loader"), [&] { m_app.setEnableEssentials(on); });
    });
    v->addWidget(esCb);
    v->addWidget(hint(lang::T(
        "SilentPatch, ДВА широкоформатных фикса, Framerate Vigilante, RunDLL32 "
        "Fix и Windowed Mode — идут вместе с Modloader отдельным слоем. "
        "Выключите, если сборка/профиль уже содержит СВОИ копии этих же "
        "фиксов: две копии одного и того же (напр. widescreen) плагина, "
        "хукающие один и тот же D3D9/FOV, — частая причина чёрного мира и "
        "рваного меню во время игры.")));

    v->addWidget(hint(lang::T(
        "Мод можно ТАКЖЕ деплоить «через Modloader» отдельно (правый клик по "
        "моду в списке → «Деплоить через Modloader»): тогда он кладётся в "
        "modloader/<имя>/, а не инжектится нативно в IMG/loose. Но грузит его "
        "именно рантайм Modloader выше — если этот чекбокс выключен, файлы "
        "мода всё равно лягут в modloader/<имя>/, но подхватывать их будет "
        "нечему, и мод молча не заработает.")));

    auto* rt   = new QGroupBox(lang::T("Папка рантайма Modloader"));
    auto* rtv  = new QVBoxLayout(rt);

    auto* path   = new QLineEdit;
    path->setReadOnly(true);
    auto* status = new QLabel;
    status->setWordWrap(true);
    auto refresh = [this, path, status] {
      path->setText(QString::fromStdString(m_app.modloaderRuntimeDir().string()));
      if (m_app.hasModloaderRuntime())
        status->setText("<span style='color:#2e9e46'>● " +
                        lang::T("Рантайм найден (modloader.asi).") + "</span>");
      else
        status->setText("<span style='color:#c0392b'>● " +
                        lang::T("Рантайм не найден.") + "</span>");
    };
    refresh();
    rtv->addWidget(path);
    rtv->addWidget(status);

    auto* row = new QHBoxLayout;
    auto* browse = new QPushButton(lang::T("Обзор…"));
    connect(browse, &QPushButton::clicked, this, [this, refresh] {
      const QString dir = QFileDialog::getExistingDirectory(
          this, lang::T("Папка рантайма Modloader"),
          QString::fromStdString(m_app.modloaderRuntimeDir().string()));
      if (!dir.isEmpty()) {
        guarded(lang::T("Рантайм Modloader"),
                [&] { m_app.setModloaderRuntimeDir(dir.toStdString()); });
        refresh();
      }
    });
    auto* open = new QPushButton(lang::T("Открыть"));
    connect(open, &QPushButton::clicked, this, [this] {
      const auto d = m_app.modloaderRuntimeDir();
      std::error_code e;
      fs::create_directories(d, e);
      QDesktopServices::openUrl(
          QUrl::fromLocalFile(QString::fromStdString(d.string())));
    });
    auto* reset = new QPushButton(lang::T("Сбросить (по умолчанию)"));
    connect(reset, &QPushButton::clicked, this, [this, refresh] {
      guarded(lang::T("Рантайм Modloader"),
              [&] { m_app.setModloaderRuntimeDir(""); });
      refresh();
    });
    row->addWidget(browse);
    row->addWidget(open);
    row->addWidget(reset);
    row->addStretch(1);
    rtv->addLayout(row);
    v->addWidget(rt);
    v->addWidget(hint(lang::T(
        "Рантайм Mod Loader встроен в программу и разворачивается автоматически — "
        "указывать пути обычно НЕ нужно. Поля выше нужны, только если хотите взять "
        "рантайм из своей папки. Менеджер сам ставит его в игру при развёртывании "
        "мода через Modloader и убирает при откате.")));

    // Replace game exe with the bundled clean 1.0.
    auto* exeGrp = new QGroupBox(lang::T("Исполняемый файл игры"));
    auto* ev     = new QVBoxLayout(exeGrp);
    auto* exeCb  = new QCheckBox(lang::T("Заменять gta_sa.exe встроенным (чистый 1.0)"));
    exeCb->setChecked(m_app.replaceGameExe());
    connect(exeCb, &QCheckBox::toggled, this, [this](bool on) {
      guarded(lang::T("Замена exe"), [&] { m_app.setReplaceGameExe(on); });
    });
    ev->addWidget(exeCb);
    if (!m_app.hasBundledExe())
      ev->addWidget(hint(lang::T(
          "Встроенный gta_sa.exe не найден — положите его в рантайм "
          "(gtamm/runtime/) и пересоберите, либо в папку рантайма.")));
    ev->addWidget(hint(lang::T(
        "При развёртывании подменяет gta_sa.exe игры на заведомо чистый GTA SA "
        "1.0 US. Оригинал сохраняется и восстанавливается при откате. Включается "
        "И АВТОМАТИЧЕСКИ, когда включён Mod Loader (выше) — он рассчитан именно "
        "на 1.0, и другой билд exe (Steam/пиратка/другой регион) — частая причина "
        "«Mod Loader не активируется». Этот чекбокс — для случая, когда нужна "
        "только замена exe без Mod Loader.")));
    v->addWidget(exeGrp);

    v->addStretch(1);
    tabs->addTab(wrapScrollable(tab), lang::T("Mod Loader"));
  }

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

  auto* lay = frameless(dlg);
  lay->addWidget(tabs);
  lay->addWidget(buttons);
  centerDialog(dlg);
  dlg.exec();
}

void MainWindow::onAbout()
{
  const bool en = lang::isEnglish();
  const QString ver =
      QString("<p style='color:#2e9e46'><b>%1 %2</b></p>")
          .arg(en ? "Version" : "Версия")
          .arg(gtamm::versionString());
  // The licence line used to be palette(mid) -- the tone used for borders and
  // separators, which on the dark theme sits so close to the background that
  // the text was barely there. Subdued is fine, invisible is not: use a real
  // grey with enough contrast in either theme.
  const QString muted = m_dark ? "#a9b1bc" : "#5c636d";
  const QString text =
      ver +
      (en ? "<h3>GMM — GTA San Andreas Mod Manager</h3>"
           "<p>A standalone mod manager for GTA San Andreas: a shared mod "
           "storage, per-build profiles, conflict resolution, IMG archive "
           "injection and data-file merging.</p>"
           "<p>Every deployment is fully reversible — the game folder is "
           "restored exactly as it was.</p>"
           "<p style='color:" + muted + "'>Free and open-source software, "
           "MIT license. Not affiliated with Rockstar Games.</p>"
         : "<h3>GMM — менеджер модов для GTA San Andreas</h3>"
           "<p>Менеджер модов для GTA San Andreas: общее хранилище модов, "
           "профили сборок, разрешение конфликтов, инъекция в IMG-архивы и "
           "мердж data-файлов.</p>"
           "<p>Любое развёртывание полностью обратимо — папка игры "
           "восстанавливается в точности как была.</p>"
           "<p style='color:" + muted + "'>Свободное ПО с открытым исходным "
           "кодом, лицензия MIT. Не связано с Rockstar Games.</p>");

  // Its own framed dialog (not QMessageBox::about()) so it matches the rest of
  // the app's custom window chrome -- but short (icon + title + close only,
  // no minimize/maximize): an about box is never meant to be resized that way.
  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("О программе GMM"));
  // Fixed width (taller, not wider): the text wraps to a narrower column and
  // the extra vertical spacing below gives the dialog more height instead.
  dlg.setFixedWidth(440);
  auto* body = frameless(dlg, /*showMinMax=*/false);
  body->setContentsMargins(20, 20, 20, 20);
  body->setSpacing(20);

  auto* row = new QHBoxLayout;
  row->setSpacing(16);
  auto* icon = new QLabel(&dlg);
  icon->setPixmap(QApplication::windowIcon().pixmap(48, 48));
  icon->setFixedSize(48, 48);
  row->addWidget(icon, 0, Qt::AlignTop);
  auto* label = new QLabel(text, &dlg);
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  row->addWidget(label, 1);
  body->addLayout(row);
  body->addStretch(1);

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
  connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  body->addWidget(box);

  centerDialog(dlg);
  dlg.exec();
}

void MainWindow::onModContextMenu(const QPoint& pos)
{
  QTreeWidgetItem* item = m_mods->itemAt(pos);
  if (!item)
    return;
  const QString id = item->data(ColCheck, kModIdRole).toString();

  if (item->data(ColCheck, kIsSepRole).toBool()) {
    QMenu menu(this);
    QAction* renameAct = menu.addAction(lang::T("Переименовать разделитель…"));
    QAction* removeAct = menu.addAction(lang::T("Удалить разделитель"));
    QAction* chosen    = menu.exec(m_mods->viewport()->mapToGlobal(pos));
    if (chosen == renameAct) {
      bool ok = false;
      const QString name =
          dialogs::getText(this, lang::T("Переименовать"), lang::T("Название:"),
                                QLineEdit::Normal, item->text(ColName), &ok);
      if (ok && !name.isEmpty()) {
        m_populating = true;
        item->setText(ColName, name);
        m_populating = false;
        persistOrder();
      }
    } else if (chosen == removeAct) {
      delete item;
      persistOrder();
      updateDerivedColumns();
    }
    return;
  }

  QMenu menu(this);
  QAction* enableAct  = menu.addAction(lang::T("Включить"));
  QAction* disableAct = menu.addAction(lang::T("Выключить"));
  menu.addSeparator();
  QAction* upAct     = menu.addAction(lang::T("Повысить приоритет"));
  QAction* downAct   = menu.addAction(lang::T("Понизить приоритет"));
  menu.addSeparator();
  bool viaMl = false;
  for (const auto& m : m_app.mods())
    if (m.id == id.toStdString()) {
      viaMl = m.viaModloader;
      break;
    }
  QAction* mlAct = menu.addAction(lang::T("Деплоить через Modloader"));
  mlAct->setCheckable(true);
  mlAct->setChecked(viaMl);

  // Only offer moonloader script actions when the mod actually has any --
  // most mods don't, and an always-visible-but-disabled item just clutters
  // the menu.
  // Editing works for any .lua in the mod (same set the badge's pencil opens);
  // splitting is limited to top-level moonloader/*.lua, which is what
  // App::splitMoonloaderScripts operates on.
  const std::vector<std::string> luaScripts = m_app.listModLuaScripts(id.toStdString());
  const QStringList editableLua =
      luaScriptsOf(listModFiles(m_app.modsDir() / id.toStdString() / "root"));
  QAction* editLuaAct  = nullptr;
  QAction* splitLuaAct = nullptr;
  if (!editableLua.isEmpty() || !luaScripts.empty()) {
    menu.addSeparator();
    if (!editableLua.isEmpty())
      editLuaAct = menu.addAction(lang::T("Редактировать Lua-скрипт…"));
    if (luaScripts.size() > 1) {
      splitLuaAct = menu.addAction(lang::T("Разбить на отдельные моды по скрипту…"));
      splitLuaAct->setToolTip(lang::T(
          "Каждый moonloader/*.lua станет отдельным модом со своим "
          "вкл/выкл — как уже сделано для CLEO-скриптов и ASI-плагинов."));
    }
  }

  menu.addSeparator();
  QAction* openAct   = menu.addAction(lang::T("Открыть папку мода"));
  QAction* exportAct = menu.addAction(lang::T("Экспорт мода в zip…"));
  QAction* removeAct = menu.addAction(lang::T("Удалить из пула…"));
  QAction* chosen    = menu.exec(m_mods->viewport()->mapToGlobal(pos));
  if (!chosen)
    return;

  if (chosen == editLuaAct) {
    onEditLuaFromTag(id);  // picks the script if the mod has more than one
    return;
  }
  if (chosen == splitLuaAct) {
    App::SplitScriptsResult r;
    bool ok = false;
    guarded(lang::T("Разбивка на отдельные моды"), [&] {
      r  = m_app.splitMoonloaderScripts(id.toStdString());
      ok = true;
    });
    if (ok) {
      refreshMods();
      refreshProfiles();
      statusBar()->showMessage(
          lang::T("Создано модов: %1. Обновлено профилей: %2.")
              .arg(static_cast<int>(r.createdModIds.size()))
              .arg(r.profilesUpdated),
          6000);
    }
    return;
  }

  if (chosen == mlAct) {
    guarded(lang::T("Modloader"),
            [&] { m_app.setModViaModloader(id.toStdString(), mlAct->isChecked()); });
    refreshMods();
    return;
  }

  if (chosen == upAct) {
    m_mods->setCurrentItem(item);
    moveModPriority(-1);
    return;
  }
  if (chosen == downAct) {
    m_mods->setCurrentItem(item);
    moveModPriority(1);
    return;
  }

  if (chosen == enableAct || chosen == disableAct) {
    item->setCheckState(ColCheck, chosen == enableAct ? Qt::Checked : Qt::Unchecked);
  } else if (chosen == openAct) {
    const fs::path folder = m_app.modsDir() / id.toStdString() / "root";
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(folder.string())));
  } else if (chosen == exportAct) {
    QString base = item->text(ColName).trimmed();
    if (base.isEmpty())
      base = id;
    const QString zip = QFileDialog::getSaveFileName(
        this, lang::T("Экспорт мода в zip"), base + ".gmmmod.zip",
        lang::T("Моды GMM (*.zip)"));
    if (zip.isEmpty())
      return;
    bool done = false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    guarded(lang::T("Экспорт мода"), [&] {
      m_app.exportMod(id.toStdString(), zip.toStdString());
      done = true;
    });
    QApplication::restoreOverrideCursor();
    if (done)
      dialogs::information(this, lang::T("Экспорт мода"),
                              lang::T("Мод экспортирован:") + "\n" + zip);
  } else if (chosen == removeAct) {
    if (dialogs::question(
            this, lang::T("Удаление мода"),
            lang::T("Удалить мод «%1» из пула? Действие необратимо.").arg(id)) !=
        QMessageBox::Yes)
      return;
    guarded(lang::T("Удаление мода"), [&] { m_app.removeMod(id.toStdString()); });
    refreshMods();
  }
}

void MainWindow::onEditLuaFromTag(const QString& modId)
{
  const QStringList scripts =
      luaScriptsOf(listModFiles(m_app.modsDir() / modId.toStdString() / "root"));
  if (scripts.isEmpty()) {
    statusBar()->showMessage(lang::T("В этом моде нет Lua-скрипта для правки."), 4000);
    return;
  }
  QString rel = scripts.front();
  if (scripts.size() > 1) {
    bool ok = false;
    const QString picked =
        dialogs::getItem(this, lang::T("Редактировать Lua-скрипт"),
                         lang::T("Скрипт:"), scripts, 0, false, &ok);
    if (!ok)
      return;
    rel = picked;
  }
  onEditLuaScript(modId, rel);
}

namespace {

// --- text encoding of a mod's script file -------------------------------------
//
// MoonLoader/CLEO scripts from the Russian-speaking community are as a rule
// saved in Windows-1251 (the Cyrillic ANSI code page), not UTF-8: the loaders
// are byte-oriented, so whatever bytes the author saved are what the game
// draws. Reading such a file as UTF-8 (which is what this editor used to do)
// turns every Cyrillic string into replacement characters, and saving it back
// then baked that damage into the mod for good.
//
// So: sniff the encoding on load, show it, and write the file back in the SAME
// encoding. Silently converting someone's script to UTF-8 would corrupt its
// Russian text just as thoroughly as reading it wrong did.
enum class ScriptCodec { Utf8, Cp1251 };

// The irregular half of Windows-1251 plus its contiguous Cyrillic tail, spelled
// out rather than computed so the mapping can be checked by eye against the
// code page chart. Index = byte - 0x80.
const char16_t kCp1251High[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,  // 80
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,  // 88
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 90
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,  // 98
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,  // A0
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,  // A8
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,  // B0
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,  // B8
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,  // C0
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,  // C8
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,  // D0
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,  // D8
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,  // E0
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,  // E8
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,  // F0
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,  // F8
};
// 0x98 is undefined in Windows-1251; it is mapped to U+0098 above purely so a
// file that happens to contain that byte still round-trips through the editor.

QString decodeCp1251(const QByteArray& bytes)
{
  QString out;
  out.reserve(bytes.size());
  for (char raw : bytes) {
    const unsigned char b = static_cast<unsigned char>(raw);
    out.append(b < 0x80 ? QChar(b) : QChar(kCp1251High[b - 0x80]));
  }
  return out;
}

// Encodes to Windows-1251, substituting '?' for anything the code page cannot
// represent. Returns false when that happened, so the caller can offer UTF-8
// instead of quietly mangling the text.
bool encodeCp1251(const QString& text, QByteArray& out)
{
  bool lossless = true;
  out.clear();
  out.reserve(text.size());
  for (const QChar qc : text) {
    const char16_t c = qc.unicode();
    if (c < 0x80) {
      out.append(static_cast<char>(c));
      continue;
    }
    int found = -1;
    for (int i = 0; i < 128 && found < 0; ++i)
      if (kCp1251High[i] == c)
        found = i;
    if (found < 0) {
      out.append('?');
      lossless = false;
    } else {
      out.append(static_cast<char>(0x80 + found));
    }
  }
  return lossless;
}

// UTF-8 unless the bytes aren't valid UTF-8 -- in which case, for a GTA SA
// script, Windows-1251 is what they are in practice. Pure ASCII decodes
// identically either way, so it is reported as UTF-8 and stays byte-identical
// on save.
ScriptCodec sniffScriptCodec(const QByteArray& bytes)
{
  if (bytes.startsWith("\xEF\xBB\xBF"))
    return ScriptCodec::Utf8;
  QStringDecoder dec(QStringConverter::Utf8);
  const QString probe = dec.decode(bytes);
  Q_UNUSED(probe);
  return dec.hasError() ? ScriptCodec::Cp1251 : ScriptCodec::Utf8;
}

}  // namespace

void MainWindow::onEditLuaScript(const QString& modId, const QString& relPath)
{
  const fs::path abs =
      m_app.modsDir() / modId.toStdString() / "root" / fs::path(relPath.toStdString());
  std::error_code ec;
  if (!fs::exists(abs, ec)) {
    dialogs::warning(this, lang::T("Редактировать Lua-скрипт"),
                        lang::T("Файл не найден в моде."));
    return;
  }

  // Read the raw bytes (no QIODevice::Text): the encoding is decided below, and
  // the file's own line endings are preserved rather than silently rewritten.
  QFile f(QString::fromStdString(abs.string()));
  if (!f.open(QIODevice::ReadOnly)) {
    dialogs::warning(this, lang::T("Редактировать Lua-скрипт"),
                        lang::T("Не удалось открыть файл."));
    return;
  }
  const QByteArray raw = f.readAll();
  f.close();

  ScriptCodec codec  = sniffScriptCodec(raw);
  const bool hadBom  = raw.startsWith("\xEF\xBB\xBF");
  const bool crlf    = raw.contains("\r\n");

  // Editing always happens on '\n' text; the file's original CRLF (if any) is
  // put back on save, so a Windows-authored script doesn't change line endings
  // wholesale just because it was opened here.
  auto decodeWith = [&raw](ScriptCodec c) {
    QString s = c == ScriptCodec::Cp1251 ? decodeCp1251(raw)
                                         : QString::fromUtf8(raw);
    if (s.startsWith(QChar(char16_t(0xFEFF))))
      s.remove(0, 1);  // BOM belongs to the file, not to the text
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return s;
  };
  const QString content = decodeWith(codec);

  QDialog dlg(this);
  dlg.setWindowTitle(lang::T("Редактировать Lua-скрипт") + " \xe2\x80\x94 " + relPath);
  dlg.resize(760, 560);
  auto* layout = frameless(dlg);

  // Was styled in palette(mid) -- the same subdued tone used for borders/
  // separators -- which made the file name almost impossible to read. This is
  // the one piece of chrome that tells you which mod/file you're actually
  // editing, so give it real contrast and weight instead.
  auto* label = new QLabel(relPath, &dlg);
  label->setStyleSheet("font-weight:bold;font-size:13px;");

  // The sniffed encoding, shown rather than hidden -- and switchable, because
  // detection can only guess for a script whose Russian text happens to be
  // valid UTF-8 by accident (or for one with no Cyrillic at all yet).
  auto* encCombo = new QComboBox(&dlg);
  encCombo->addItem("UTF-8");
  encCombo->addItem(lang::T("Windows-1251 (кириллица)"));
  encCombo->setCurrentIndex(codec == ScriptCodec::Cp1251 ? 1 : 0);
  encCombo->setToolTip(
      lang::T("Кодировка файла. Скрипт сохраняется в той же кодировке, в "
              "которой был открыт."));
  auto* head = new QHBoxLayout();
  head->addWidget(label, 1);
  head->addWidget(new QLabel(lang::T("Кодировка:"), &dlg));
  head->addWidget(encCombo);
  layout->addLayout(head);

  // Handles zoom itself (Ctrl+Plus/Minus/Equal and Ctrl+wheel) directly off
  // the raw key/wheel event instead of going through QShortcut+QKeySequence
  // -- Ctrl+Minus turned out not to fire reliably as a parsed key sequence,
  // so this sidesteps that parsing entirely and just checks modifiers()/key()/
  // angleDelta() by hand. Also persists the resulting font size to QSettings
  // (one shared size across every script, not per-file) so it survives
  // reopening the editor, including across app restarts.
  class ZoomableEditor : public QPlainTextEdit
  {
  public:
    using QPlainTextEdit::QPlainTextEdit;

  protected:
    void wheelEvent(QWheelEvent* e) override
    {
      if (e->modifiers() & Qt::ControlModifier) {
        if (e->angleDelta().y() > 0)
          zoomIn(1);
        else if (e->angleDelta().y() < 0)
          zoomOut(1);
        persistZoom();
        e->accept();
        return;
      }
      QPlainTextEdit::wheelEvent(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
      if (e->modifiers() & Qt::ControlModifier) {
        if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) {
          zoomIn(1);
          persistZoom();
          e->accept();
          return;
        }
        if (e->key() == Qt::Key_Minus) {
          zoomOut(1);
          persistZoom();
          e->accept();
          return;
        }
      }
      QPlainTextEdit::keyPressEvent(e);
    }

  private:
    void persistZoom() { QSettings().setValue("luaEditorFontPointSize", font().pointSizeF()); }
  };

  auto* editor = new ZoomableEditor(&dlg);
  editor->setPlainText(content);
  QFont mono("Consolas");
  mono.setStyleHint(QFont::Monospace);
  const double savedPt = QSettings().value("luaEditorFontPointSize", 10.0).toDouble();
  mono.setPointSizeF(savedPt > 0 ? savedPt : 10.0);
  editor->setFont(mono);
  editor->setLineWrapMode(QPlainTextEdit::NoWrap);
  layout->addWidget(editor, 1);
  editor->document()->setModified(false);  // loaded content isn't a "change"

  auto* box =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
  layout->addWidget(box);

  // Re-decodes the file when the encoding is switched by hand. Unsaved edits
  // would not survive that (the text is rebuilt from the bytes on disk), so
  // they're confirmed away first.
  connect(encCombo, &QComboBox::currentIndexChanged, &dlg, [&](int idx) {
    const ScriptCodec chosen =
        idx == 1 ? ScriptCodec::Cp1251 : ScriptCodec::Utf8;
    if (chosen == codec)
      return;
    if (editor->document()->isModified() &&
        dialogs::question(
            &dlg, lang::T("Кодировка"),
            lang::T("Файл будет перечитан в выбранной кодировке. "
                    "Несохранённые изменения будут потеряны. Продолжить?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
      const QSignalBlocker block(encCombo);
      encCombo->setCurrentIndex(codec == ScriptCodec::Cp1251 ? 1 : 0);
      return;
    }
    codec = chosen;
    editor->setPlainText(decodeWith(codec));
    editor->document()->setModified(false);
  });

  // Text -> the exact bytes to put on disk: the file's own encoding, its BOM if
  // it had one, and its own line endings.
  auto encodeForDisk = [&](QString text, bool* lossless) {
    if (crlf)
      text.replace(QChar(u'\n'), QStringLiteral("\r\n"));
    QByteArray bytes;
    if (codec == ScriptCodec::Cp1251) {
      *lossless = encodeCp1251(text, bytes);
    } else {
      bytes     = text.toUtf8();
      *lossless = true;
      if (hadBom)
        bytes.prepend("\xEF\xBB\xBF", 3);
    }
    return bytes;
  };

  // Writes the current text to the mod's file on disk right now (does not
  // close the dialog) -- shared by the Save button, Ctrl+S, and the "Save"
  // choice in the unsaved-changes prompt below.
  auto doSave = [&]() -> bool {
    bool lossless    = true;
    QByteArray bytes = encodeForDisk(editor->toPlainText(), &lossless);
    if (!lossless) {
      // Typed something Windows-1251 has no room for (an emoji, say). Saying so
      // beats writing '?' over it silently -- but converting the whole script
      // to UTF-8 is the user's call, since the loader may not expect it.
      const auto ans = dialogs::question(
          &dlg, lang::T("Кодировка"),
          lang::T("В тексте есть символы, которых нет в Windows-1251 — при "
                  "сохранении они станут «?». Сохранить файл в UTF-8?"),
          QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
          QMessageBox::Yes);
      if (ans == QMessageBox::Cancel)
        return false;
      if (ans == QMessageBox::Yes) {
        codec = ScriptCodec::Utf8;
        encCombo->setCurrentIndex(0);  // no-op handler: codec already matches
        bytes = encodeForDisk(editor->toPlainText(), &lossless);
      }
    }
    bool ok = false;
    guarded(lang::T("Сохранение скрипта"), [&] {
      m_app.writeModFile(modId.toStdString(), relPath.toStdString(),
                         std::string(bytes.constData(),
                                     static_cast<size_t>(bytes.size())));
      ok = true;
    });
    if (ok) {
      editor->document()->setModified(false);
      statusBar()->showMessage(
          lang::T("Скрипт сохранён. Если мод уже развёрнут через жёсткую ссылку — "
                  "изменение уже видно в игре; иначе потребуется Развернуть заново."),
          8000);
    }
    return ok;
  };

  // Asks to save/discard/cancel if there are unsaved changes; returns true if
  // it's now fine to actually close (nothing to lose, or the user resolved
  // it). Shared by the Cancel button and the close/Escape guard below, so
  // there's exactly one way out of the dialog that can silently drop edits:
  // an explicit "Discard".
  auto confirmClose = [&dlg, editor, doSave]() -> bool {
    if (!editor->document()->isModified())
      return true;
    const auto ans = dialogs::question(
        &dlg, lang::T("Несохранённые изменения"),
        lang::T("Скрипт изменён, но не сохранён. Сохранить перед выходом?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (ans == QMessageBox::Cancel)
      return false;
    if (ans == QMessageBox::Save)
      return doSave();
    return true;  // Discard
  };

  connect(box, &QDialogButtonBox::accepted, &dlg, [&dlg, doSave] {
    if (doSave())
      dlg.accept();
  });
  connect(box, &QDialogButtonBox::rejected, &dlg, [&dlg, confirmClose] {
    if (confirmClose())
      dlg.reject();
  });

  // The custom title bar's own close button just calls QWidget::close(), and
  // QDialog's Escape handling calls reject() directly -- neither goes through
  // the button box above, so both need their own guard against silently
  // discarding unsaved edits. See UnsavedCloseGuard::eventFilter() for why
  // this can't just be done by connecting to QDialog::rejected() instead.
  class UnsavedCloseGuard : public QObject
  {
  public:
    UnsavedCloseGuard(QObject* parent, std::function<bool()> okToClose)
        : QObject(parent), m_okToClose(std::move(okToClose))
    {
    }

  protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
      if (ev->type() == QEvent::Close) {
        if (!m_okToClose()) {
          ev->ignore();
          // Swallow entirely: QDialog::closeEvent() would call reject()
          // unconditionally (it doesn't check whether the event was
          // ignored), so letting it through would close the dialog anyway.
          return true;
        }
        return false;  // fine to close -- let QDialog::closeEvent() run normally
      }
      if (ev->type() == QEvent::KeyPress &&
          static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape)
        return !m_okToClose();  // swallow Escape only if the user cancelled
      return QObject::eventFilter(obj, ev);
    }

  private:
    std::function<bool()> m_okToClose;
  };
  dlg.installEventFilter(new UnsavedCloseGuard(&dlg, confirmClose));

  // Ctrl+S saves without closing -- a real editor doesn't force you to leave
  // just to save your work.
  auto* saveShortcut = new QShortcut(QKeySequence::Save, &dlg);
  connect(saveShortcut, &QShortcut::activated, &dlg, [doSave] { doSave(); });

  // Zoom (Ctrl+Plus/Minus/Equal, Ctrl+wheel) is handled directly by
  // ZoomableEditor above.

  centerDialog(dlg);
  dlg.exec();
}

void MainWindow::onListEdited()
{
  if (m_populating)
    return;
  // A checkbox toggle (or label edit): persist without touching the order — the
  // priority is read from each item's stored role, so a sorted view is safe.
  persistOrder();
  updateDerivedColumns();
  updateStatusBar();
  // Enabling/disabling a mod can change what the run-exe picker should offer
  // (e.g. a mod that carries its own samp.exe) even before Deploy runs.
  populateExecutables();
  if (m_mods->currentItem())
    updateDetails(m_mods->currentItem()->data(ColCheck, kModIdRole).toString());
}

void MainWindow::onRowsMoved()
{
  if (m_populating)
    return;
  // A manual drag-reorder is the one action that redefines load order.
  m_populating = true;
  renumberPriorities();
  m_populating = false;
  persistOrder();
  updateDerivedColumns();
  updateStatusBar();
}

void MainWindow::onHeaderClicked(int column)
{
  // The "#"/checkbox headers mean "back to manual priority order" (with drag).
  if (column == ColPrio || column == ColCheck) {
    applySort(ColPrio, Qt::AscendingOrder);
    return;
  }
  // Otherwise: a non-destructive browse sort by that column. Toggle direction on
  // repeat clicks of the same column.
  const Qt::SortOrder order =
      (m_sortColumn == column && m_sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder
                                                                    : Qt::AscendingOrder;
  applySort(column, order);
}

void MainWindow::applySort(int column, Qt::SortOrder order)
{
  // ColPrio (= the "#"/load-order field) is the manual priority view: rebuild the
  // list in stored order and re-enable drag-reorder.
  if (column == ColPrio || column == ColCheck || column < 0) {
    if (m_sortColumn != -1)
      refreshMods();  // refreshMods resets m_sortColumn to -1 and restores order
    else
      m_sortColumn = -1;
    syncSortControls();
    return;
  }
  // A non-destructive browse sort by the column. Drag-reorder is disabled while
  // sorted (ambiguous); stored priorities are untouched, so returning to the
  // load-order view restores the manual order.
  m_sortColumn = column;
  m_sortOrder  = order;
  m_mods->setDragEnabled(false);
  m_populating = true;
  m_mods->sortItems(column, order);
  m_populating = false;
  m_mods->header()->setSortIndicator(column, order);
  applyFilter();
  syncSortControls();
}

void MainWindow::syncSortControls()
{
  if (!m_sortCombo || !m_sortDirButton)
    return;
  const int col = (m_sortColumn < 0) ? ColPrio : m_sortColumn;
  const int idx = m_sortCombo->findData(col);
  m_populating = true;  // suppress the combo's change signal
  if (idx >= 0)
    m_sortCombo->setCurrentIndex(idx);
  m_populating = false;
  const bool manual = (m_sortColumn < 0);
  m_sortDirButton->setEnabled(!manual);
  m_sortDirButton->setText(m_sortOrder == Qt::DescendingOrder ? "▼" : "▲");
}

void MainWindow::onSortComboChanged(int index)
{
  if (m_populating || index < 0)
    return;
  const int col = m_sortCombo->itemData(index).toInt();
  if (col == ColPrio)
    applySort(ColPrio, Qt::AscendingOrder);
  else
    applySort(col, m_sortOrder);  // keep the current direction
}

void MainWindow::onSortDirToggled()
{
  if (m_sortColumn < 0)
    return;  // manual order has no direction
  applySort(m_sortColumn,
            m_sortOrder == Qt::AscendingOrder ? Qt::DescendingOrder
                                              : Qt::AscendingOrder);
}

void MainWindow::onSelectionChanged()
{
  QTreeWidgetItem* it = m_mods->currentItem();
  updateDetails(it ? it->data(ColCheck, kModIdRole).toString() : QString());
}

void MainWindow::onFilterChanged(const QString&)
{
  applyFilter();
}

void MainWindow::updateRevealButton()
{
  if (!m_splitter || !m_revealPanelButton)
    return;
  const QList<int> sizes = m_splitter->sizes();
  // The run/info panel is the second pane; treat <= a few px as "collapsed".
  const bool collapsed = sizes.size() >= 2 && sizes[1] <= 6;
  if (!collapsed || m_playRunning) {  // never float it over the play-lock overlay
    m_revealPanelButton->hide();
    return;
  }
  const QRect cg = centralWidget() ? centralWidget()->geometry() : rect();
  const int w = 22, h = 64;
  m_revealPanelButton->setGeometry(cg.right() - w + 1, cg.center().y() - h / 2, w, h);
  m_revealPanelButton->show();
  m_revealPanelButton->raise();
}

void MainWindow::onRevealPanel()
{
  if (!m_splitter)
    return;
  const int total = m_splitter->width();
  // Give the panel back a sensible width (about a third, capped), keep the rest
  // for the mod list.
  const int panel = std::min(380, std::max(260, total / 3));
  m_splitter->setSizes({std::max(120, total - panel), panel});
  updateRevealButton();
}

// --- log viewer -------------------------------------------------------------

void MainWindow::buildLogPanel()
{
  m_logDock = new QDockWidget(lang::T("Логи"), this);
  m_logDock->setObjectName("logDock");
  m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  m_logDock->setFeatures(QDockWidget::DockWidgetMovable |
                         QDockWidget::DockWidgetFloatable |
                         QDockWidget::DockWidgetClosable);

  auto* body = new QWidget(m_logDock);
  auto* v    = new QVBoxLayout(body);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);

  auto* row = new QHBoxLayout();
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(6);
  row->addWidget(new QLabel(lang::T("Файл лога:"), body));
  m_logCombo = new QComboBox(body);
  m_logCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  row->addWidget(m_logCombo, 1);
  m_logAutoCheck = new QCheckBox(lang::T("Авто"), body);
  m_logAutoCheck->setChecked(true);
  m_logAutoCheck->setToolTip(lang::T("Автоматически обновлять лог в реальном времени"));
  row->addWidget(m_logAutoCheck);
  auto* refresh = new QPushButton(lang::T("Обновить"), body);
  row->addWidget(refresh);
  auto* openExt = new QPushButton(lang::T("Открыть в редакторе"), body);
  row->addWidget(openExt);
  v->addLayout(row);

  m_logView = new QPlainTextEdit(body);
  m_logView->setReadOnly(true);
  m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_logView->setMaximumBlockCount(50000);  // keep memory bounded for huge logs
  QFont mono("Consolas");
  mono.setStyleHint(QFont::Monospace);
  m_logView->setFont(mono);
  v->addWidget(m_logView, 1);

  m_logDock->setWidget(body);
  addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
  m_logDock->hide();

  // Polls the current log file ~once a second and live-tails it when "Авто" is on
  // and the dock is visible (works while the game runs / launcher is locked).
  m_logTimer = new QTimer(this);
  m_logTimer->setInterval(1000);
  connect(m_logTimer, &QTimer::timeout, this, &MainWindow::pollLog);

  connect(refresh, &QPushButton::clicked, this, &MainWindow::onRefreshLogs);
  connect(m_logCombo, &QComboBox::currentIndexChanged, this,
          &MainWindow::onLogSelected);
  connect(openExt, &QPushButton::clicked, this, [this] {
    if (!m_logCombo || m_logCombo->currentIndex() < 0)
      return;
    const QString p = m_logCombo->currentData().toString();
    if (!p.isEmpty())
      QDesktopServices::openUrl(QUrl::fromLocalFile(p));
  });
  // Run the live-tail timer only while the dock is shown; keep the status-bar
  // toggle button's arrow in sync if the dock is closed via its own ✕.
  connect(m_logDock, &QDockWidget::visibilityChanged, this, [this](bool vis) {
    if (m_logToggleButton)
      m_logToggleButton->setText(vis ? lang::T("Логи ▾") : lang::T("Логи ▴"));
    if (m_logTimer) {
      if (vis)
        m_logTimer->start();
      else
        m_logTimer->stop();
    }
    // Showing/hiding the dock resizes the central area; keep the play overlay
    // (if locked) aligned to it after the layout settles.
    if (m_playOverlay && m_playOverlay->isVisible())
      QTimer::singleShot(0, this, [this] {
        if (m_playOverlay && m_playOverlay->isVisible() && centralWidget())
          m_playOverlay->setGeometry(centralWidget()->geometry());
      });
  });
}

void MainWindow::onToggleLogPanel()
{
  if (!m_logDock)
    return;
  const bool show = !m_logDock->isVisible();
  m_logDock->setVisible(show);
  if (show)
    onRefreshLogs();
}

void MainWindow::onRefreshLogs()
{
  if (!m_logCombo)
    return;
  const QString prev = m_logCombo->currentData().toString();  // keep selection

  // Scan the deployed build (the game folder) recursively for *.log files.
  std::vector<fs::path> logs;
  const fs::path game = m_app.gamePath();
  std::error_code ec;
  if (!game.empty() && fs::exists(game, ec)) {
    for (auto it = fs::recursive_directory_iterator(
             game, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        const std::string ext = it->path().extension().string();
        if (QString::fromStdString(ext).compare(".log", Qt::CaseInsensitive) == 0)
          logs.push_back(it->path());
      }
      if (logs.size() >= 500)  // sanity cap
        break;
    }
  }
  std::sort(logs.begin(), logs.end());

  m_populating = true;
  m_logCombo->clear();
  for (const fs::path& p : logs) {
    const QString rel =
        QString::fromStdString(fs::relative(p, game, ec).generic_string());
    m_logCombo->addItem(rel.isEmpty() ? QString::fromStdString(p.filename().string())
                                      : rel,
                        QString::fromStdString(p.string()));
  }
  m_populating = false;

  if (m_logCombo->count() == 0) {
    m_logView->setPlainText(
        lang::T("В сборке не найдено файлов .log.\nЗапустите игру — моды (CLEO/ASI/"
                "modloader) создадут логи в папке игры."));
    return;
  }
  int idx = m_logCombo->findData(prev);
  if (idx < 0)
    idx = 0;
  m_logCombo->setCurrentIndex(idx);
  onLogSelected(idx);  // ensure content loads even if the index didn't change
}

void MainWindow::onLogSelected(int index)
{
  if (m_populating || !m_logCombo || index < 0)
    return;
  // A manual file switch resets change-tracking and jumps to the end.
  m_logSize  = -1;
  m_logMtime = 0;
  loadLog(m_logCombo->itemData(index).toString(), /*keepPosition=*/false);
}

void MainWindow::pollLog()
{
  if (!m_logDock || !m_logDock->isVisible())
    return;
  if (!m_logAutoCheck || !m_logAutoCheck->isChecked())
    return;
  // If no logs are listed yet, keep rescanning so a freshly created one appears.
  if (!m_logCombo || m_logCombo->count() == 0) {
    onRefreshLogs();
    return;
  }
  if (m_logCurrentPath.isEmpty())
    return;
  QFileInfo fi(m_logCurrentPath);
  if (!fi.exists())
    return;
  const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
  if (fi.size() == m_logSize && mtime == m_logMtime)
    return;  // unchanged
  loadLog(m_logCurrentPath, /*keepPosition=*/true);
}

void MainWindow::loadLog(const QString& path, bool keepPosition)
{
  if (!m_logView || path.isEmpty())
    return;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    m_logView->setPlainText(lang::T("Не удалось открыть файл лога: ") + path);
    return;
  }
  // Remember where the view was: if the user was at the bottom we keep following
  // the tail; otherwise we restore their scroll position after the reload.
  QScrollBar* sb       = m_logView->verticalScrollBar();
  const bool wasAtEnd  = !keepPosition || sb->value() >= sb->maximum() - 4;
  const int savedValue = sb->value();

  // Cap very large logs: show the tail (most recent) up to ~2 MB.
  constexpr qint64 kCap = 2 * 1024 * 1024;
  const qint64 size = f.size();
  QString note;
  if (size > kCap) {
    f.seek(size - kCap);
    note = lang::T("… (показаны последние 2 МБ из ") +
           QString::number(size / 1024) + lang::T(" КБ) …\n\n");
  }
  const QByteArray data = f.readAll();
  m_logView->setPlainText(note + QString::fromUtf8(data));

  if (wasAtEnd)
    m_logView->moveCursor(QTextCursor::End);  // follow the newest lines
  else
    sb->setValue(qMin(savedValue, sb->maximum()));

  m_logCurrentPath = path;
  m_logSize        = size;
  m_logMtime       = QFileInfo(path).lastModified().toMSecsSinceEpoch();
}

}  // namespace gtamm
