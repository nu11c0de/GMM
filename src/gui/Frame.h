#pragma once
#include <QDialog>
#include <QWidget>

// Real, global-scope forward declarations -- NOT elaborated-type-specifiers
// (`class Foo*`) inline inside `namespace gtamm` below: doing that where the
// real ::QLabel etc. isn't already visible creates a brand new, unrelated
// gtamm::QLabel type instead of referring to the actual Qt class, which is
// exactly the bug this fixes (Frame.cpp's `new QLabel(...)` then silently
// built a `gtamm::QLabel`, incompatible with the real `QWidget*` it needs to
// be treated as).
class QLabel;
class QToolButton;
class QVBoxLayout;

// Shared "custom-framed window" building blocks: everything MainWindow uses
// for its own frameless title bar, factored out so any QDialog anywhere in
// the app -- not just ones MainWindow builds -- can get the identical look,
// including standalone helper dialogs that don't have a MainWindow instance
// to call back into (see Dialogs.h, Instances.cpp).
namespace gtamm {

// Custom, theme-aware window caption: app icon + title + minimize/maximize/
// close. The window is frameless, so this bar also drives moving
// (startSystemMove) and double-click maximize; edge-resize is handled by
// MainWindow's app-wide event filter (checks windowFlags() for
// Qt::FramelessWindowHint, not any specific window, so it applies here too).
class TitleBar : public QWidget
{
public:
  // `showMinMax`: some windows (e.g. a short "About" dialog) are never meant to
  // be minimized/maximized -- pass false to get just an icon, title, and close
  // button, matching how a plain system dialog's caption is usually shorter
  // than a resizable window's.
  explicit TitleBar(QWidget* win, bool showMinMax = true);

  void setTitleText(const QString& t);

  // Recolor the glyphs for the active theme: bright on dark, dark on light.
  void setTheme(bool dark);
  void updateMaximizeIcon();

protected:
  void mousePressEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
  QToolButton* makeButton(const QString& objName);
  // A real OS-level showMaximized()/showNormal() toggle turned out to be a
  // silent no-op for a QDialog (Qt::Dialog window type) even with
  // Qt::WindowMaximizeButtonHint set -- QMainWindow (Qt::Window type) is
  // fine, but a frameless QDialog just never actually maximized. Rather than
  // keep chasing the exact Win32 flag combination that would make native
  // maximize work for a Dialog, fake it: remember the pre-maximize geometry
  // and set/restore it directly. Works identically for every window type,
  // since it's just setGeometry() -- no dependency on WS_MAXIMIZEBOX/native
  // maximize state at all.
  void toggleMaximize();

  QWidget* m_win         = nullptr;
  QLabel* m_icon         = nullptr;
  QLabel* m_title        = nullptr;
  QToolButton* m_min     = nullptr;
  QToolButton* m_max     = nullptr;
  QToolButton* m_close   = nullptr;
  bool m_dark = true;
  bool m_pseudoMaximized = false;
  QRect m_restoreGeometry;
};

// Shared stylesheet for any TitleBar instance -- palette-driven so it tracks
// the built-in dark/light themes and any external MO2 .qss (which ships its
// own palette).
QString titleBarStyleSheet(bool dark);

// Best-effort guess at whether the currently active application palette is a
// dark one, for standalone dialogs (not built by MainWindow, which already
// tracks this itself) that need to pick a TitleBar theme. Works for the
// built-in dark/light themes and for an external MO2 .qss (applyExternal()
// always sets a real palette alongside the stylesheet).
bool isDarkPalette();

// Strips a QDialog's native chrome, gives it the same 3px palette-driven
// outline and custom TitleBar as every other custom-framed window in the
// app, and returns the QVBoxLayout the caller should add the dialog's own
// content to (the dialog must not create its own top-level layout). Also
// grants Qt::WindowMinimizeButtonHint/WindowMaximizeButtonHint when
// `showMinMax` is true -- without them a frameless QDialog can't actually be
// maximized (see TitleBar::toggleMaximize()).
QVBoxLayout* applyFrame(QDialog& dlg, bool dark, bool showMinMax = true);

// Centers `dlg` over `anchor` (or the primary screen if `anchor` is null),
// both horizontally and vertically. Must be called by the caller, after all
// of the dialog's own content has been added, immediately before its own
// exec()/show(): it calls dlg.show() itself to read the dialog's real
// post-show size (a pre-show sizeHint can undercount wrapped labels).
void centerOverWidget(QDialog& dlg, QWidget* anchor);

}  // namespace gtamm
