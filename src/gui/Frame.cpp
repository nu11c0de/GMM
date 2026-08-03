#include "Frame.h"

#include <QApplication>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QScreen>
#include <QToolButton>
#include <QWindow>

namespace gtamm {

namespace {

// Crisp, high-contrast window-control glyphs drawn by hand (the QStyle standard
// title-bar icons are low-contrast greys that wash out, especially on dark themes).
enum class WinGlyph { Min, Max, Restore, Close };

QIcon winGlyphIcon(WinGlyph g, const QColor& c)
{
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QPen pen(c);
  pen.setWidthF(2.0);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::MiterJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  switch (g) {
    case WinGlyph::Min:
      p.drawLine(5, 14, 15, 14);
      break;
    case WinGlyph::Max:
      p.drawRect(5, 5, 10, 10);
      break;
    case WinGlyph::Restore:
      p.drawRect(4, 7, 9, 9);  // front window
      p.drawPolyline(QPolygon() << QPoint(7, 7) << QPoint(7, 4) << QPoint(16, 4)
                                << QPoint(16, 13) << QPoint(13, 13));  // back window
      break;
    case WinGlyph::Close:
      p.drawLine(5, 5, 15, 15);
      p.drawLine(15, 5, 5, 15);
      break;
  }
  return QIcon(pm);
}

}  // namespace

QString titleBarStyleSheet(bool dark)
{
  const QString hover = dark ? "rgba(255,255,255,0.18)" : "rgba(0,0,0,0.14)";
  const QString press = dark ? "rgba(255,255,255,0.28)" : "rgba(0,0,0,0.22)";
  return "#titleBar{background:palette(window);}"
        "#titleBar QLabel{background:transparent;color:palette(window-text);}"
        "#titleBar QLabel#winTitle{font-weight:bold;}"
        "QToolButton#winBtn,QToolButton#winCloseBtn{"
        "border:none;border-radius:0;background:transparent;}"
        "QToolButton#winBtn:hover{background:" + hover + ";}"
        "QToolButton#winBtn:pressed{background:" + press + ";}"
        "QToolButton#winCloseBtn:hover{background:#e81123;}"
        "QToolButton#winCloseBtn:pressed{background:#c50f1f;}";
}

bool isDarkPalette()
{
  return qApp->palette().color(QPalette::Window).lightness() < 128;
}

TitleBar::TitleBar(QWidget* win, bool showMinMax) : QWidget(win), m_win(win)
{
  setObjectName("titleBar");
  setFixedHeight(34);
  auto* lay = new QHBoxLayout(this);
  lay->setContentsMargins(8, 0, 0, 0);
  lay->setSpacing(8);

  m_icon = new QLabel(this);
  m_icon->setObjectName("winIcon");
  m_icon->setFixedSize(18, 18);
  m_icon->setScaledContents(true);
  m_icon->setPixmap(QApplication::windowIcon().pixmap(18, 18));
  lay->addWidget(m_icon);

  m_title = new QLabel(this);
  m_title->setObjectName("winTitle");
  lay->addWidget(m_title);
  lay->addStretch(1);

  if (showMinMax) {
    m_min = makeButton("winBtn");
    m_max = makeButton("winBtn");
    lay->addWidget(m_min);
    lay->addWidget(m_max);
  }
  m_close = makeButton("winCloseBtn");
  lay->addWidget(m_close);
  setTheme(true);  // initial high-contrast glyphs (restyled by the caller)

  if (m_min)
    connect(m_min, &QToolButton::clicked, win, &QWidget::showMinimized);
  if (m_max)
    connect(m_max, &QToolButton::clicked, this, [this] { toggleMaximize(); });
  connect(m_close, &QToolButton::clicked, win, &QWidget::close);
}

void TitleBar::setTitleText(const QString& t)
{
  m_title->setText(t);
}

void TitleBar::setTheme(bool dark)
{
  m_dark = dark;
  const QColor c = dark ? QColor(0xF2, 0xF2, 0xF2) : QColor(0x20, 0x20, 0x20);
  if (m_min)
    m_min->setIcon(winGlyphIcon(WinGlyph::Min, c));
  m_close->setIcon(winGlyphIcon(WinGlyph::Close, c));
  updateMaximizeIcon();
}

void TitleBar::updateMaximizeIcon()
{
  if (!m_max)
    return;
  const QColor c = m_dark ? QColor(0xF2, 0xF2, 0xF2) : QColor(0x20, 0x20, 0x20);
  m_max->setIcon(winGlyphIcon(m_pseudoMaximized ? WinGlyph::Restore : WinGlyph::Max, c));
}

void TitleBar::mousePressEvent(QMouseEvent* e)
{
  if (e->button() == Qt::LeftButton)
    if (QWindow* h = m_win->windowHandle())
      h->startSystemMove();  // native move (supports Aero snap)
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* e)
{
  if (e->button() == Qt::LeftButton && m_max)
    toggleMaximize();
}

QToolButton* TitleBar::makeButton(const QString& objName)
{
  auto* b = new QToolButton(this);
  b->setObjectName(objName);
  b->setIconSize(QSize(16, 16));
  b->setFixedSize(44, 34);
  b->setAutoRaise(true);
  b->setFocusPolicy(Qt::NoFocus);
  return b;
}

void TitleBar::toggleMaximize()
{
  if (m_pseudoMaximized) {
    m_win->setGeometry(m_restoreGeometry);
    m_pseudoMaximized = false;
  } else {
    m_restoreGeometry = m_win->geometry();
    QScreen* screen = m_win->screen();
    if (!screen)
      screen = QGuiApplication::primaryScreen();
    if (screen)
      m_win->setGeometry(screen->availableGeometry());
    m_pseudoMaximized = true;
  }
  updateMaximizeIcon();
}

QVBoxLayout* applyFrame(QDialog& dlg, bool dark, bool showMinMax)
{
  dlg.setWindowFlag(Qt::FramelessWindowHint, true);
  if (showMinMax) {
    dlg.setWindowFlag(Qt::WindowMinimizeButtonHint, true);
    dlg.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
  }

  dlg.setObjectName("gmmWindow");
  dlg.setAttribute(Qt::WA_StyledBackground, true);
  dlg.setStyleSheet("#gmmWindow{border:3px solid palette(mid);background:palette(window);}");

  auto* outer = new QVBoxLayout(&dlg);
  outer->setContentsMargins(3, 3, 3, 3);
  outer->setSpacing(0);

  auto* bar = new TitleBar(&dlg, showMinMax);
  bar->setTitleText(dlg.windowTitle());
  bar->setTheme(dark);
  bar->setStyleSheet(titleBarStyleSheet(dark));
  outer->addWidget(bar);

  auto* body = new QWidget(&dlg);
  auto* bodyLayout = new QVBoxLayout(body);
  outer->addWidget(body, 1);

  return bodyLayout;
}

namespace {
QRect anchorRect(QWidget* anchor)
{
  if (anchor)
    return anchor->frameGeometry();
  if (QScreen* screen = QApplication::primaryScreen())
    return screen->availableGeometry();
  return QRect();
}
}  // namespace

void centerOverWidget(QDialog& dlg, QWidget* anchor)
{
  dlg.show();
  const QSize sz = dlg.size();
  const QRect pg = anchorRect(anchor);
  if (pg.isNull())
    return;
  dlg.move(pg.left() + (pg.width() - sz.width()) / 2,
           pg.top() + (pg.height() - sz.height()) / 2);
}

}  // namespace gtamm
