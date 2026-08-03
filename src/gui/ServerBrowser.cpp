#include "ServerBrowser.h"

#include "Dialogs.h"
#include "Frame.h"
#include "Lang.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDialogButtonBox>
#include <QFrame>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace gtamm {

namespace {

const char* kApiUrl              = "https://api.open.mp/servers";
const char* kFavoritesSettingsKey = "sampFavorites";

enum Column { ColFav = 0, ColName, ColPlayers, ColMode, ColLanguage, ColPassword, ColVersion, ColOmp, ColCount };

// Sorts the "Players" column by the actual player count (stored in
// Qt::UserRole) instead of lexicographically comparing "12/100" strings,
// which would otherwise put "9/50" above "100/500".
class NumericItem : public QTableWidgetItem
{
public:
  using QTableWidgetItem::QTableWidgetItem;
  bool operator<(const QTableWidgetItem& other) const override
  {
    return data(Qt::UserRole).toInt() < other.data(Qt::UserRole).toInt();
  }
};

// The list is the one screen in the app that is nothing but rows, so it lives
// or dies on typography and spacing. Colours are fixed rather than palette-
// derived: they are accents on top of the palette (which still supplies the
// row/base background), and they have to stay legible under the built-in
// themes as well as an external MO2 .qss.
const char* kAccent = "#2e9e4f";  // the app's green, used for the selection
const char* kStar   = "#e0a92c";  // favourited star

// A muted-but-readable grey. Not palette(mid): that is the border tone, which
// on the dark theme is so close to the background that text in it disappears
// (the same trap the About box's licence line fell into).
const char* mutedColor(bool dark)
{
  return dark ? "#9aa2ad" : "#5f6670";
}

QString tableStyle(bool dark)
{
  const QString line   = dark ? "#3a3f47" : "#d8dade";
  const QString muted  = mutedColor(dark);
  const QString hover  = dark ? "rgba(255,255,255,0.05)" : "rgba(0,0,0,0.04)";
  // Selection: a translucent wash of the accent instead of the stock hard blue,
  // with the row's own text kept readable on top of it.
  const QString sel    = dark ? "rgba(46,158,79,0.28)" : "rgba(46,158,79,0.20)";
  // Tight paddings and a slightly smaller face: this is a dense list, and the
  // window should stay small enough to sit next to the SA-MP settings dialog
  // rather than take over the screen.
  return QString(
             "QTableWidget{border:1px solid %1;border-radius:5px;"
             "gridline-color:transparent;font-size:11px;}"
             "QTableWidget::item{padding:2px 6px;border:0;}"
             "QTableWidget::item:hover{background:%2;}"
             "QTableWidget::item:selected{background:%3;color:palette(text);}"
             "QHeaderView::section{background:transparent;border:0;"
             "border-bottom:1px solid %1;padding:4px 6px;font-weight:600;"
             "font-size:11px;color:%4;}"
             "QHeaderView::section:hover{color:palette(text);}")
      .arg(line, hover, sel, muted);
}

// "host:port" -> (host, port). Splits on the LAST ':' (safe even for an
// IPv6-literal host, though SA-MP addresses are practically always IPv4).
std::pair<QString, int> splitHostPort(const QString& ip)
{
  const int pos = ip.lastIndexOf(':');
  if (pos < 0)
    return {ip, 7777};
  bool ok       = false;
  const int p   = ip.mid(pos + 1).toInt(&ok);
  return {ip.left(pos), ok ? p : 7777};
}

}  // namespace

ServerBrowserDialog::ServerBrowserDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(lang::T("Обзор серверов SA-MP"));
  resize(900, 520);
  setMinimumSize(620, 340);
  const bool dark = isDarkPalette();
  auto* body = applyFrame(*this, dark);
  body->setContentsMargins(10, 8, 10, 8);
  body->setSpacing(7);

  // Appended, not assigned: applyFrame() already put the window's own border
  // rule (#gmmWindow) in this dialog's stylesheet, and replacing it would drop
  // the frame every custom-framed window in the app shares.
  setStyleSheet(styleSheet() + tableStyle(dark) +
                QString("QLineEdit{padding:3px 7px;border:1px solid %1;"
                        "border-radius:4px;}"
                        "QLineEdit:focus{border-color:%2;}"
                        "QTabWidget::pane{border:0;}"
                        "QTabBar::tab{padding:4px 11px;margin-right:2px;"
                        "font-size:11px;border:0;"
                        "border-bottom:2px solid transparent;}"
                        "QTabBar::tab:selected{border-bottom-color:%2;"
                        "font-weight:600;}"
                        "QPushButton{padding:4px 12px;}")
                    .arg(dark ? "#3a3f47" : "#d8dade", kAccent));

  m_nam = new QNetworkAccessManager(this);
  loadFavorites();

  m_tabs = new QTabWidget(this);
  body->addWidget(m_tabs, 1);

  // --- "All servers" tab: filter box + refresh + the full fetched list ---
  m_allTab        = new QWidget(m_tabs);
  auto* allLayout = new QVBoxLayout(m_allTab);
  allLayout->setContentsMargins(0, 6, 0, 0);
  allLayout->setSpacing(6);

  auto* topRow = new QHBoxLayout;
  m_filter     = new QLineEdit(m_allTab);
  m_filter->setPlaceholderText(lang::T("Фильтр по названию или режиму игры…"));
  m_filter->setClearButtonEnabled(true);  // typing a filter is a throwaway act
  topRow->addWidget(m_filter, 1);
  m_refreshBtn = new QPushButton(lang::T("Обновить"), m_allTab);
  m_refreshBtn->setMinimumHeight(26);
  topRow->addWidget(m_refreshBtn);
  allLayout->addLayout(topRow);

  m_table = new QTableWidget(0, ColCount, m_allTab);
  setupTable(m_table);
  allLayout->addWidget(m_table, 1);

  m_status = new QLabel(lang::T("Загрузка списка серверов…"), m_allTab);
  m_status->setStyleSheet(QString("color:%1;font-size:11px;").arg(mutedColor(dark)));
  allLayout->addWidget(m_status);

  m_tabs->addTab(m_allTab, lang::T("Все серверы"));

  // --- "Favorites" tab: just the starred servers, cached locally ---
  m_favTab        = new QWidget(m_tabs);
  auto* favLayout = new QVBoxLayout(m_favTab);
  favLayout->setContentsMargins(0, 6, 0, 0);
  favLayout->setSpacing(6);

  m_favTable = new QTableWidget(0, ColCount, m_favTab);
  setupTable(m_favTable);
  favLayout->addWidget(m_favTable, 1);

  m_favStatus = new QLabel(m_favTab);
  m_favStatus->setStyleSheet(QString("color:%1;font-size:11px;").arg(mutedColor(dark)));
  favLayout->addWidget(m_favStatus);

  m_tabs->addTab(m_favTab, lang::T("Избранное"));

  auto* box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  m_connectBtn = box->addButton(lang::T("Подключиться"), QDialogButtonBox::AcceptRole);
  m_connectBtn->setEnabled(false);
  // The one action this window exists for, so it reads as the primary button
  // rather than as one more grey rectangle next to Cancel.
  m_connectBtn->setMinimumHeight(28);
  m_connectBtn->setStyleSheet(
      QString("QPushButton{background:%1;color:#ffffff;border:0;"
              "border-radius:4px;padding:5px 16px;font-weight:600;}"
              "QPushButton:hover{background:#37b45b;}"
              "QPushButton:disabled{background:%2;color:%3;}")
          .arg(kAccent, dark ? "#33373e" : "#dfe1e5", mutedColor(dark)));
  body->addWidget(box);

  connect(m_refreshBtn, &QPushButton::clicked, this, &ServerBrowserDialog::refresh);
  connect(m_filter, &QLineEdit::textChanged, this, &ServerBrowserDialog::onFilterChanged);
  connect(m_table, &QTableWidget::cellDoubleClicked, this, &ServerBrowserDialog::onRowActivated);
  connect(m_table, &QTableWidget::cellClicked, this, &ServerBrowserDialog::onTableCellClicked);
  connect(m_table, &QTableWidget::itemSelectionChanged, this, &ServerBrowserDialog::updateConnectEnabled);

  connect(m_favTable, &QTableWidget::cellDoubleClicked, this, &ServerBrowserDialog::onFavRowActivated);
  connect(m_favTable, &QTableWidget::cellClicked, this, &ServerBrowserDialog::onFavTableCellClicked);
  connect(m_favTable, &QTableWidget::itemSelectionChanged, this, &ServerBrowserDialog::updateConnectEnabled);

  connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
    updateConnectEnabled();
    // A table on a hidden tab has no width to divide, so its columns are laid
    // out the moment it becomes visible.
    layoutAllColumns();
  });

  connect(m_connectBtn, &QPushButton::clicked, this, [this] {
    const bool favTab        = (m_tabs->currentWidget() == m_favTab);
    QTableWidget* active = favTab ? m_favTable : m_table;
    const int row             = active->currentRow();
    if (row < 0)
      return;
    if (favTab)
      onFavRowActivated(row, ColName);
    else
      onRowActivated(row, ColName);
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  rebuildFavoritesTable();
  refresh();
}

ServerBrowserDialog::~ServerBrowserDialog() = default;

void ServerBrowserDialog::setupTable(QTableWidget* table)
{
  table->setHorizontalHeaderLabels({
      "★",
      lang::T("Название"),
      lang::T("Игроки"),
      lang::T("Режим"),
      lang::T("Язык"),
      lang::T("Пароль"),
      lang::T("Версия"),
      "open.mp",
  });
  QHeaderView* head = table->horizontalHeader();
  head->setStretchLastSection(false);
  head->setSectionResizeMode(ColFav, QHeaderView::ResizeToContents);
  // Every other column is sized by layoutColumns() from the table's actual
  // width, so none of them may be Stretch/ResizeToContents -- those modes
  // ignore (and fight) an explicit resizeSection().
  for (const int c : {ColName, ColMode, ColPlayers, ColLanguage, ColPassword,
                      ColVersion, ColOmp})
    head->setSectionResizeMode(c, QHeaderView::Interactive);
  head->setHighlightSections(false);  // no bold jump when a column is clicked
  head->setMinimumHeight(24);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setAlternatingRowColors(true);
  table->setSortingEnabled(false);
  // Row numbers down the left tell nobody anything about a server list, and the
  // grid turns a thousand rows into graph paper -- the zebra striping already
  // separates them.
  table->verticalHeader()->setVisible(false);
  table->verticalHeader()->setDefaultSectionSize(21);  // dense, but still clickable
  table->setShowGrid(false);
  table->setFrameShape(QFrame::NoFrame);  // the stylesheet draws the border
}

void ServerBrowserDialog::layoutColumns(QTableWidget* table)
{
  if (!table)
    return;
  int avail = table->viewport()->width();
  if (avail <= 0)
    return;  // not laid out yet; showEvent/resizeEvent will come back to this
  avail -= table->columnWidth(ColFav);  // the star sizes itself to its glyph

  // Two thirds for what you read (server name and game mode), one third for
  // the short facts. Weights inside each group, not equal splits: a mode name
  // is shorter than a server name, and "1/50" needs less room than "0.3.7-R5".
  const int wide = (avail * 2) / 3;
  const int name = (wide * 3) / 5;
  table->setColumnWidth(ColName, std::max(130, name));
  table->setColumnWidth(ColMode, std::max(90, wide - name));

  const int rest = avail - wide;
  struct Share { int col; int weight; int min; };
  const Share shares[] = {{ColPlayers, 22, 52},  {ColLanguage, 24, 56},
                          {ColPassword, 16, 46}, {ColVersion, 22, 58},
                          {ColOmp, 16, 50}};
  int total = 0;
  for (const Share& s : shares)
    total += s.weight;
  for (const Share& s : shares)
    table->setColumnWidth(s.col, std::max(s.min, rest * s.weight / total));
}

void ServerBrowserDialog::layoutAllColumns()
{
  layoutColumns(m_table);
  layoutColumns(m_favTable);
}

void ServerBrowserDialog::resizeEvent(QResizeEvent* e)
{
  QDialog::resizeEvent(e);
  layoutAllColumns();
}

void ServerBrowserDialog::showEvent(QShowEvent* e)
{
  QDialog::showEvent(e);
  // Only now does the viewport have a real width to divide up.
  layoutAllColumns();
}

void ServerBrowserDialog::refresh()
{
  m_status->setText(lang::T("Загрузка списка серверов…"));
  m_refreshBtn->setEnabled(false);

  QNetworkRequest req{QUrl(kApiUrl)};
  req.setTransferTimeout(15000);
  QNetworkReply* reply = m_nam->get(req);
  connect(reply, &QNetworkReply::finished, this, [this, reply] { onReplyFinished(reply); });
}

void ServerBrowserDialog::onReplyFinished(QNetworkReply* reply)
{
  reply->deleteLater();
  m_refreshBtn->setEnabled(true);

  if (reply->error() != QNetworkReply::NoError) {
    m_status->setText(lang::T("Не удалось загрузить список серверов: ") + reply->errorString());
    return;
  }
  populateTable(reply->readAll());
}

SampServerInfo ServerBrowserDialog::parseEntry(const QJsonObject& o)
{
  SampServerInfo e;
  e.ip         = o.value("ip").toString();
  e.name       = o.value("hn").toString();
  e.players    = o.value("pc").toInt();
  e.maxPlayers = o.value("pm").toInt();
  e.mode       = o.value("gm").toString();
  e.lang       = o.value("la").toString();
  e.hasPass    = o.value("pa").toBool();
  e.version    = o.value("vn").toString();
  e.omp        = o.value("omp").toBool();
  return e;
}

QJsonObject ServerBrowserDialog::toJson(const SampServerInfo& e)
{
  QJsonObject o;
  o["ip"]  = e.ip;
  o["hn"]  = e.name;
  o["pc"]  = e.players;
  o["pm"]  = e.maxPlayers;
  o["gm"]  = e.mode;
  o["la"]  = e.lang;
  o["pa"]  = e.hasPass;
  o["vn"]  = e.version;
  o["omp"] = e.omp;
  return o;
}

void ServerBrowserDialog::populateTable(const QByteArray& json)
{
  QJsonParseError perr;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
  if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
    m_status->setText(lang::T("Сервер отдал неожиданный ответ (не список серверов)."));
    return;
  }
  const QJsonArray arr = doc.array();

  m_allServers.clear();
  m_allServers.reserve(arr.size());
  for (const QJsonValue& v : arr)
    m_allServers.push_back(parseEntry(v.toObject()));

  // Keep favorited servers' cached metadata (player count etc.) fresh
  // whenever they show up in a new fetch, instead of freezing it at
  // whatever it looked like at the moment they were starred.
  bool favoritesChanged = false;
  for (const auto& e : m_allServers) {
    auto it = m_favorites.find(e.ip);
    if (it != m_favorites.end()) {
      it.value()       = e;
      favoritesChanged = true;
    }
  }
  if (favoritesChanged)
    saveFavorites();

  m_table->setSortingEnabled(false);
  m_table->setRowCount(0);
  m_table->setRowCount(static_cast<int>(m_allServers.size()));
  for (int row = 0; row < m_allServers.size(); ++row)
    fillRow(m_table, row, m_allServers[row]);
  m_table->setSortingEnabled(true);
  m_table->sortByColumn(ColPlayers, Qt::DescendingOrder);

  m_status->setText(lang::T("Найдено серверов: %1").arg(arr.size()));
  applyFilter();
  rebuildFavoritesTable();
  layoutAllColumns();  // a fresh fill resets nothing else, but be explicit
}

void ServerBrowserDialog::fillRow(QTableWidget* table, int row, const SampServerInfo& info)
{
  const bool dark  = isDarkPalette();
  const QColor dim = QColor(mutedColor(dark));

  const bool fav = isFavorite(info.ip);
  auto* favItem  = new QTableWidgetItem(fav ? QStringLiteral("★") : QStringLiteral("☆"));
  favItem->setTextAlignment(Qt::AlignCenter);
  favItem->setData(Qt::UserRole, info.ip);
  favItem->setToolTip(lang::T("Нажмите, чтобы добавить/убрать из избранного"));
  // A gold star for the starred ones: the glyph alone reads as noise in a
  // thousand-row list, the colour is what makes favourites findable.
  favItem->setForeground(fav ? QColor(kStar) : dim);
  table->setItem(row, ColFav, favItem);

  auto* nameItem = new QTableWidgetItem(info.name);
  nameItem->setData(Qt::UserRole, info.ip);  // "host:port", read back on activation
  nameItem->setToolTip(info.ip);  // the address, without a column of its own
  table->setItem(row, ColName, nameItem);

  auto* playersItem = new NumericItem(QString("%1/%2").arg(info.players).arg(info.maxPlayers));
  playersItem->setData(Qt::UserRole, info.players);
  playersItem->setTextAlignment(Qt::AlignCenter);
  // Empty servers stay in the list but stop competing for attention with the
  // populated ones, which is the whole reason anyone sorts by player count.
  if (info.players == 0)
    playersItem->setForeground(dim);
  table->setItem(row, ColPlayers, playersItem);

  table->setItem(row, ColMode, new QTableWidgetItem(info.mode));

  auto* langItem = new QTableWidgetItem(info.lang);
  langItem->setTextAlignment(Qt::AlignCenter);
  table->setItem(row, ColLanguage, langItem);

  // Yes/no columns as a mark and a dash rather than words: two columns of "Да"/
  // "Нет" repeated a thousand times is most of what made this list a wall of
  // text. The word is still there as a tooltip.
  auto flag = [&](bool on, const QColor& onColor) {
    auto* item = new QTableWidgetItem(on ? QStringLiteral("✓") : QStringLiteral("—"));
    item->setTextAlignment(Qt::AlignCenter);
    item->setForeground(on ? onColor : dim);
    item->setToolTip(on ? lang::T("Да") : lang::T("Нет"));
    return item;
  };
  table->setItem(row, ColPassword, flag(info.hasPass, QColor(kStar)));

  auto* verItem = new QTableWidgetItem(info.version);
  verItem->setForeground(dim);  // version is context, not something you scan for
  table->setItem(row, ColVersion, verItem);

  table->setItem(row, ColOmp, flag(info.omp, QColor(kAccent)));
}

void ServerBrowserDialog::onFilterChanged(const QString&)
{
  applyFilter();
}

void ServerBrowserDialog::applyFilter()
{
  const QString needle = m_filter->text().trimmed();
  for (int r = 0; r < m_table->rowCount(); ++r) {
    bool show = true;
    if (!needle.isEmpty()) {
      const QString name = m_table->item(r, ColName) ? m_table->item(r, ColName)->text() : QString();
      const QString mode = m_table->item(r, ColMode) ? m_table->item(r, ColMode)->text() : QString();
      show = name.contains(needle, Qt::CaseInsensitive) || mode.contains(needle, Qt::CaseInsensitive);
    }
    m_table->setRowHidden(r, !show);
  }
}

void ServerBrowserDialog::onTableCellClicked(int row, int column)
{
  if (column != ColFav)
    return;
  QTableWidgetItem* item = m_table->item(row, ColFav);
  if (!item)
    return;
  toggleFavoriteByIp(item->data(Qt::UserRole).toString());
}

void ServerBrowserDialog::onFavTableCellClicked(int row, int column)
{
  if (column != ColFav)
    return;
  QTableWidgetItem* item = m_favTable->item(row, ColFav);
  if (!item)
    return;
  toggleFavoriteByIp(item->data(Qt::UserRole).toString());
}

void ServerBrowserDialog::toggleFavoriteByIp(const QString& ip)
{
  if (ip.isEmpty())
    return;
  if (m_favorites.contains(ip)) {
    m_favorites.remove(ip);
  } else {
    SampServerInfo info;
    bool found = false;
    for (const auto& e : m_allServers) {
      if (e.ip == ip) {
        info  = e;
        found = true;
        break;
      }
    }
    if (!found)
      return;  // shouldn't happen -- toggled rows always come from a known list
    m_favorites.insert(ip, info);
  }
  saveFavorites();

  const QString glyph = isFavorite(ip) ? QStringLiteral("★") : QStringLiteral("☆");
  for (int r = 0; r < m_table->rowCount(); ++r) {
    QTableWidgetItem* it = m_table->item(r, ColFav);
    if (it && it->data(Qt::UserRole).toString() == ip) {
      it->setText(glyph);
      break;
    }
  }
  rebuildFavoritesTable();
}

void ServerBrowserDialog::loadFavorites()
{
  QSettings settings;
  const QByteArray raw = settings.value(kFavoritesSettingsKey).toByteArray();
  if (raw.isEmpty())
    return;
  const QJsonDocument doc = QJsonDocument::fromJson(raw);
  if (!doc.isArray())
    return;
  for (const QJsonValue& v : doc.array()) {
    SampServerInfo info = parseEntry(v.toObject());
    if (!info.ip.isEmpty())
      m_favorites.insert(info.ip, info);
  }
}

void ServerBrowserDialog::saveFavorites()
{
  QJsonArray arr;
  for (auto it = m_favorites.constBegin(); it != m_favorites.constEnd(); ++it)
    arr.append(toJson(it.value()));
  QSettings settings;
  settings.setValue(kFavoritesSettingsKey, QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void ServerBrowserDialog::rebuildFavoritesTable()
{
  m_favTable->setSortingEnabled(false);
  m_favTable->setRowCount(m_favorites.size());
  int row = 0;
  for (auto it = m_favorites.constBegin(); it != m_favorites.constEnd(); ++it, ++row)
    fillRow(m_favTable, row, it.value());
  m_favTable->setSortingEnabled(true);
  if (m_favTable->rowCount() > 0)
    m_favTable->sortByColumn(ColPlayers, Qt::DescendingOrder);

  m_tabs->setTabText(m_tabs->indexOf(m_favTab),
                      QString("%1 (%2)").arg(lang::T("Избранное")).arg(m_favorites.size()));
  m_favStatus->setText(m_favorites.isEmpty()
                            ? lang::T("Нет избранных серверов — нажмите ★ у сервера в списке.")
                            : lang::T("Избранных серверов: %1").arg(m_favorites.size()));
  updateConnectEnabled();
}

void ServerBrowserDialog::updateConnectEnabled()
{
  const bool favTab        = (m_tabs->currentWidget() == m_favTab);
  QTableWidget* active = favTab ? m_favTable : m_table;
  m_connectBtn->setEnabled(!active->selectedItems().isEmpty());
}

void ServerBrowserDialog::onRowActivated(int row, int column)
{
  if (column == ColFav)
    return;
  QTableWidgetItem* nameItem = m_table->item(row, ColName);
  if (!nameItem)
    return;
  const auto [host, port] = splitHostPort(nameItem->data(Qt::UserRole).toString());
  m_selectedHost           = host;
  m_selectedPort            = port;
  accept();
}

void ServerBrowserDialog::onFavRowActivated(int row, int column)
{
  if (column == ColFav)
    return;
  QTableWidgetItem* nameItem = m_favTable->item(row, ColName);
  if (!nameItem)
    return;
  const auto [host, port] = splitHostPort(nameItem->data(Qt::UserRole).toString());
  m_selectedHost           = host;
  m_selectedPort            = port;
  accept();
}

bool ServerBrowserDialog::pickServer(QWidget* parent, QString& host, int& port)
{
  ServerBrowserDialog dlg(parent);
  centerOverWidget(dlg, parent ? parent->window() : nullptr);
  if (dlg.exec() != QDialog::Accepted || dlg.selectedHost().isEmpty())
    return false;
  host = dlg.selectedHost();
  port = dlg.selectedPort();
  return true;
}

}  // namespace gtamm
