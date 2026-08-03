#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QString>
#include <QStyleFactory>

namespace gtamm::theme {

namespace {

const char* kDarkQss =
    "QWidget{color:#e6e6e6;}"
    "QToolBar{border:0;padding:4px;spacing:4px;background:#1e2024;}"
    "QMenuBar{background:#1e2024;color:#e6e6e6;}"
    "QMenuBar::item:selected{background:#2e9e4f;color:#ffffff;}"
    "QMenu{background:#23252b;color:#e6e6e6;border:1px solid #3a3d42;}"
    "QMenu::item:selected{background:#2e9e4f;color:#ffffff;}"
    "QHeaderView::section{background:#2b2e33;color:#e6e6e6;padding:4px;border:0;"
    "border-right:1px solid #1e2024;}"
    "QTreeView{background:#23252b;alternate-background-color:#282b30;color:#e6e6e6;"
    "border:1px solid #14151a;outline:0;}"
    "QTreeView::item{height:22px;}"
    "QTreeView::item:selected{background:#2e9e4f;color:#ffffff;}"
    "QTextBrowser{background:#202226;color:#e6e6e6;border:1px solid #14151a;}"
    "QComboBox{background:#2b2e33;color:#e6e6e6;padding:3px;border:1px solid #3a3d42;"
    "border-radius:4px;}"
    "QComboBox QAbstractItemView{background:#23252b;color:#e6e6e6;"
    "selection-background-color:#2e9e4f;}"
    "QLineEdit{background:#2b2e33;color:#e6e6e6;padding:4px;border:1px solid #3a3d42;"
    "border-radius:4px;}"
    "QStatusBar{background:#1a1c1f;color:#e6e6e6;}"
    "QStatusBar::item{border:0;}"
    "QToolTip{background:#23252b;color:#e6e6e6;border:1px solid #3a3d42;}"
    // Kill the bright Fusion bevels on separators/tab frame, but keep the
    // splitter handle (the divider between the mod list and the info/run
    // panel) and the dock/toolbar-row separator clearly visible -- they're
    // functional grab handles, not just decoration, and used to be nearly
    // invisible against the surrounding panels.
    "QToolBar::separator{background:#33363c;width:1px;height:1px;margin:5px 6px;}"
    "QMainWindow::separator{background:#4a4f58;width:4px;height:4px;}"
    "QMainWindow::separator:hover{background:#2e9e4f;}"
    "QSplitter::handle{background:#4a4f58;}"
    "QSplitter::handle:hover{background:#2e9e4f;}"
    "QSplitter::handle:horizontal{width:5px;}"
    "QSplitter::handle:vertical{height:5px;}"
    "QTabWidget::pane{border:1px solid #2b2e33;top:-1px;}"
    "QTabBar::tab{padding:6px 12px;background:#23252b;color:#bfbfbf;"
    "border:1px solid #2b2e33;border-bottom:0;}"
    "QTabBar::tab:selected{background:#2b2e33;color:#ffffff;}";

const char* kLightQss =
    "QWidget{color:#1e1e1e;}"
    "QToolBar{border:0;padding:4px;spacing:4px;background:#e8e8e8;}"
    "QMenuBar{background:#e8e8e8;color:#1e1e1e;}"
    "QMenuBar::item:selected{background:#2e9e4f;color:#ffffff;}"
    "QMenu{background:#ffffff;color:#1e1e1e;border:1px solid #c0c0c0;}"
    "QMenu::item:selected{background:#2e9e4f;color:#ffffff;}"
    "QHeaderView::section{background:#e8e8e8;color:#1e1e1e;padding:4px;border:0;"
    "border-right:1px solid #d0d0d0;}"
    "QTreeView{background:#ffffff;alternate-background-color:#f4f5f7;color:#1e1e1e;"
    "border:1px solid #c8c8c8;outline:0;}"
    "QTreeView::item{height:22px;}"
    "QTreeView::item:selected{background:#2e9e4f;color:#ffffff;}"
    "QTextBrowser{background:#ffffff;color:#1e1e1e;border:1px solid #c8c8c8;}"
    "QComboBox{background:#ffffff;color:#1e1e1e;padding:3px;border:1px solid #b8b8b8;"
    "border-radius:4px;}"
    "QComboBox QAbstractItemView{background:#ffffff;color:#1e1e1e;"
    "selection-background-color:#2e9e4f;}"
    "QLineEdit{background:#ffffff;color:#1e1e1e;padding:4px;border:1px solid #b8b8b8;"
    "border-radius:4px;}"
    "QStatusBar{background:#e0e0e0;color:#1e1e1e;}"
    "QStatusBar::item{border:0;}"
    "QToolTip{background:#ffffff;color:#1e1e1e;border:1px solid #b8b8b8;}"
    "QToolBar::separator{background:#c4c4c4;width:1px;height:1px;margin:5px 6px;}"
    "QMainWindow::separator{background:#a0a0a0;width:4px;height:4px;}"
    "QMainWindow::separator:hover{background:#2e9e4f;}"
    "QSplitter::handle{background:#a0a0a0;}"
    "QSplitter::handle:hover{background:#2e9e4f;}"
    "QSplitter::handle:horizontal{width:5px;}"
    "QSplitter::handle:vertical{height:5px;}"
    "QTabWidget::pane{border:1px solid #c8c8c8;top:-1px;}"
    "QTabBar::tab{padding:6px 12px;background:#e4e4e4;color:#505050;"
    "border:1px solid #c8c8c8;border-bottom:0;}"
    "QTabBar::tab:selected{background:#ffffff;color:#1e1e1e;}";

QPalette darkPalette()
{
  QPalette p;
  const QColor base(0x23, 0x25, 0x29), alt(0x2b, 0x2e, 0x33), window(0x1e, 0x20, 0x24);
  const QColor text(0xe6, 0xe6, 0xe6), accent(0x2e, 0x9e, 0x4f);
  p.setColor(QPalette::Window, window);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, base);
  p.setColor(QPalette::AlternateBase, alt);
  p.setColor(QPalette::ToolTipBase, base);
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, alt);
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::Highlight, accent);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, accent);
  // Mid-tones: Fusion draws frame/bevel lines from these. Keep them dark so
  // borders and separators don't render as bright white bars.
  p.setColor(QPalette::Light, QColor(0x35, 0x38, 0x3d));
  p.setColor(QPalette::Midlight, QColor(0x2c, 0x2f, 0x34));
  p.setColor(QPalette::Mid, QColor(0x24, 0x26, 0x2b));
  p.setColor(QPalette::Dark, QColor(0x16, 0x18, 0x1b));
  p.setColor(QPalette::Shadow, QColor(0x0d, 0x0e, 0x10));
  p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x80, 0x80, 0x80));
  p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x80, 0x80, 0x80));
  return p;
}

QPalette lightPalette()
{
  QPalette p;
  const QColor base(0xff, 0xff, 0xff), alt(0xf0, 0xf0, 0xf0), window(0xf3, 0xf3, 0xf3);
  const QColor text(0x1e, 0x1e, 0x1e), accent(0x2e, 0x9e, 0x4f);
  p.setColor(QPalette::Window, window);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, base);
  p.setColor(QPalette::AlternateBase, alt);
  p.setColor(QPalette::ToolTipBase, base);
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, alt);
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::Highlight, accent);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, accent);
  p.setColor(QPalette::Disabled, QPalette::Text, QColor(0xa0, 0xa0, 0xa0));
  p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xa0, 0xa0, 0xa0));
  return p;
}

}  // namespace

void apply(QApplication& app, bool dark)
{
  app.setStyle(QStyleFactory::create("Fusion"));
  app.setPalette(dark ? darkPalette() : lightPalette());
  app.setStyleSheet(dark ? kDarkQss : kLightQss);
}

void applyExternal(QApplication& app, const QString& qss, bool darkBase)
{
  // MO2 themes assume a Fusion base; the .qss provides the colors. We still set a
  // palette so any widget the sheet doesn't cover stays legible.
  app.setStyle(QStyleFactory::create("Fusion"));
  app.setPalette(darkBase ? darkPalette() : lightPalette());
  app.setStyleSheet(qss);
}

bool isLightId(const QString& id)
{
  return id == "light";
}

void applyNamed(QApplication& app, const QString& id)
{
  if (id == "light") {
    apply(app, false);
    return;
  }
  if (id == "dark" || id.isEmpty()) {
    apply(app, true);
    return;
  }
  QFile f(id);
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    applyExternal(app, QString::fromUtf8(f.readAll()), /*darkBase=*/true);
    return;
  }
  apply(app, true);  // missing file -> dark built-in
}

}  // namespace gtamm::theme
