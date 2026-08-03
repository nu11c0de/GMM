#include "Welcome.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>

namespace gtamm {

namespace {

const char* kLangBtnStyle =
    "QPushButton#langBtn{padding:9px;border:1px solid palette(mid);"
    "border-radius:6px;background:palette(button);font-size:13px;}"
    "QPushButton#langBtn:checked{background:#2e9e4f;color:#ffffff;"
    "border-color:#2e9e4f;font-weight:bold;}";

}  // namespace

WelcomeDialog::WelcomeDialog(QWidget* parent) : QDialog(parent)
{
  setWindowFlag(Qt::FramelessWindowHint, true);
  setFixedSize(460, 480);

  // Same palette-driven outline as the rest of the app's custom-framed
  // windows (see MainWindow::setupFrame()), reproduced by hand here since
  // MainWindow doesn't exist yet at this point in startup.
  setObjectName("gmmWindow");
  setAttribute(Qt::WA_StyledBackground, true);
  setStyleSheet(
      "#gmmWindow{border:3px solid palette(mid);background:palette(window);}");

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(3, 3, 3, 3);
  root->setSpacing(0);

  // Banner: icon + app name on a fixed dark/green gradient strip, independent
  // of the active app theme -- a small "brand" moment for the very first
  // thing a new user ever sees, like a splash screen baked into the dialog.
  auto* banner = new QFrame(this);
  banner->setObjectName("welcomeBanner");
  banner->setStyleSheet(
      "#welcomeBanner{background:qlineargradient(x1:0,y1:0,x2:1,y2:1,"
      "stop:0 #1b1d21, stop:1 #1e6a37);}"
      "#welcomeBanner QLabel{background:transparent;color:#f2f2f2;}");
  auto* bannerLay = new QVBoxLayout(banner);
  bannerLay->setContentsMargins(24, 30, 24, 26);
  bannerLay->setSpacing(10);
  bannerLay->setAlignment(Qt::AlignCenter);

  auto* icon = new QLabel(banner);
  icon->setPixmap(QApplication::windowIcon().pixmap(72, 72));
  icon->setAlignment(Qt::AlignCenter);
  bannerLay->addWidget(icon, 0, Qt::AlignCenter);

  auto* title = new QLabel("GMM", banner);
  title->setStyleSheet("font-size:26px;font-weight:bold;");
  title->setAlignment(Qt::AlignCenter);
  bannerLay->addWidget(title);

  m_subtitle = new QLabel(banner);
  m_subtitle->setAlignment(Qt::AlignCenter);
  m_subtitle->setStyleSheet("font-size:12px;color:#cfe8d8;");
  bannerLay->addWidget(m_subtitle);

  root->addWidget(banner);

  // Body: language toggle + greeting + start button.
  auto* body    = new QWidget(this);
  auto* bodyLay = new QVBoxLayout(body);
  bodyLay->setContentsMargins(28, 22, 28, 22);
  bodyLay->setSpacing(16);

  auto* langRow = new QHBoxLayout();
  langRow->setSpacing(10);
  m_ruBtn = new QPushButton("Русский", body);
  m_enBtn = new QPushButton("English", body);
  for (QPushButton* b : {m_ruBtn, m_enBtn}) {
    b->setObjectName("langBtn");
    b->setStyleSheet(kLangBtnStyle);
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setMinimumHeight(38);
    langRow->addWidget(b);
  }
  bodyLay->addLayout(langRow);
  connect(m_ruBtn, &QPushButton::clicked, this,
          [this] { applyLanguage(lang::Language::Russian); });
  connect(m_enBtn, &QPushButton::clicked, this,
          [this] { applyLanguage(lang::Language::English); });

  m_greeting = new QLabel(body);
  m_greeting->setWordWrap(true);
  m_greeting->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  bodyLay->addWidget(m_greeting, 1);

  m_start = new QPushButton(body);
  m_start->setCursor(Qt::PointingHandCursor);
  m_start->setMinimumHeight(42);
  m_start->setStyleSheet(
      "QPushButton{background:#2e9e4f;color:#ffffff;border:0;border-radius:7px;"
      "font-size:14px;font-weight:bold;}"
      "QPushButton:hover{background:#37b85d;}");
  connect(m_start, &QPushButton::clicked, this, &QDialog::accept);
  bodyLay->addWidget(m_start);

  root->addWidget(body, 1);

  applyLanguage(lang::Language::English);

  // Center on the primary screen -- there is no parent window yet to center
  // over (this runs before any instance/main window is chosen or built).
  if (auto* screen = QApplication::primaryScreen()) {
    const QRect avail = screen->availableGeometry();
    move(avail.center() - QPoint(width() / 2, height() / 2));
  }
}

void WelcomeDialog::applyLanguage(lang::Language l)
{
  m_lang         = l;
  const bool ru  = (l == lang::Language::Russian);
  m_ruBtn->setChecked(ru);
  m_enBtn->setChecked(!ru);

  setWindowTitle(ru ? "Добро пожаловать" : "Welcome");
  m_subtitle->setText(ru ? "Менеджер модов для GTA San Andreas"
                        : "GTA San Andreas Mod Manager");
  m_greeting->setText(
      ru ? "GMM хранит все ваши моды в одном общем пуле и никогда не трогает "
           "файлы игры напрямую. Создавайте сколько угодно сборок (профилей), "
           "мгновенно переключайтесь между ними и откатывайте любые изменения "
           "одним кликом."
         : "GMM keeps all your mods in one shared pool and never touches your "
           "game files directly. Build as many profiles as you like, switch "
           "between them instantly, and roll back any change with a single "
           "click.");
  m_start->setText(ru ? "Начать работу" : "Get Started");
}

void WelcomeDialog::mousePressEvent(QMouseEvent* event)
{
  // No title bar on this one-off dialog -- let the user drag it from any
  // empty area of the card instead (child widgets consume their own clicks
  // first, so this never interferes with the language/start buttons).
  if (event->button() == Qt::LeftButton) {
    if (QWindow* wh = windowHandle())
      wh->startSystemMove();
  }
  QDialog::mousePressEvent(event);
}

}  // namespace gtamm
