#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QMap>
#include <QList>
#include <QString>

class QLineEdit;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace gtamm {

// One row of the open.mp server list -- also the shape cached to disk for
// favorited servers (via QSettings), so the Favorites tab has something to
// show immediately on open and stays populated even for a server that isn't
// present in the latest fetch (e.g. temporarily delisted/offline).
struct SampServerInfo
{
  QString ip;  // "host:port", the unique key
  QString name;
  int players    = 0;
  int maxPlayers = 0;
  QString mode;
  QString lang;
  bool hasPass = false;
  QString version;
  bool omp = false;
};

// SA-MP/open.mp server browser: fetches the public server list from
// api.open.mp (an open, no-auth-required directory of SA-MP/open.mp
// servers), lets the user filter/sort/star it, and returns the picked
// server's host+port -- meant to fill in the "Сервер:"/"Порт:" fields in the
// SA-MP settings dialog instead of typing an address by hand, the same way
// the real SA-MP client's own in-game server browser works.
class ServerBrowserDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ServerBrowserDialog(QWidget* parent = nullptr);
  ~ServerBrowserDialog() override;

  QString selectedHost() const { return m_selectedHost; }
  int selectedPort() const { return m_selectedPort; }

  // Shows the browser; on a picked+accepted server, `host`/`port` are set
  // and true is returned. Returns false if the user cancelled.
  static bool pickServer(QWidget* parent, QString& host, int& port);

protected:
  void resizeEvent(QResizeEvent* e) override;
  void showEvent(QShowEvent* e) override;

private slots:
  void refresh();
  void onReplyFinished(QNetworkReply* reply);
  void onRowActivated(int row, int column);
  void onFavRowActivated(int row, int column);
  void onTableCellClicked(int row, int column);
  void onFavTableCellClicked(int row, int column);
  void onFilterChanged(const QString& text);

private:
  void setupTable(QTableWidget* table);
  // Splits the width by hand: the two columns you actually read (name + game
  // mode) get two thirds of it, everything else shares the rest. QHeaderView
  // has no per-section stretch weights, so proportions have to be assigned.
  void layoutColumns(QTableWidget* table);
  void layoutAllColumns();
  void populateTable(const QByteArray& json);
  void applyFilter();
  void fillRow(QTableWidget* table, int row, const SampServerInfo& info);
  void updateConnectEnabled();

  void loadFavorites();
  void saveFavorites();
  void rebuildFavoritesTable();
  void toggleFavoriteByIp(const QString& ip);
  bool isFavorite(const QString& ip) const { return m_favorites.contains(ip); }

  static SampServerInfo parseEntry(const QJsonObject& o);
  static QJsonObject toJson(const SampServerInfo& e);

  QNetworkAccessManager* m_nam = nullptr;
  QTabWidget* m_tabs           = nullptr;
  QWidget* m_allTab            = nullptr;
  QWidget* m_favTab            = nullptr;
  QTableWidget* m_table        = nullptr;  // "All servers" tab
  QTableWidget* m_favTable     = nullptr;  // "Favorites" tab
  QLineEdit* m_filter          = nullptr;
  QLabel* m_status             = nullptr;
  QLabel* m_favStatus          = nullptr;
  QPushButton* m_refreshBtn    = nullptr;
  QPushButton* m_connectBtn    = nullptr;

  QList<SampServerInfo> m_allServers;       // last fetched full list
  QMap<QString, SampServerInfo> m_favorites;  // keyed by "host:port"

  QString m_selectedHost;
  int m_selectedPort = 0;
};

}  // namespace gtamm
