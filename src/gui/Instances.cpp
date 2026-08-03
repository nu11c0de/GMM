#include "Instances.h"

#include <fstream>

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "App.h"
#include "Dialogs.h"
#include "Frame.h"
#include "Json.h"
#include "Lang.h"
#include "Process.h"

namespace fs = std::filesystem;

namespace gtamm {

namespace {

std::string sanitize(const std::string& name)
{
  std::string s;
  for (char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '-' || c == '_')
      s += c;
    else if (c == ' ')
      s += '_';
  }
  if (s.empty())
    s = "instance";
  return s;
}

}  // namespace

fs::path InstanceStore::baseDir()
{
  // Portable: instances (each with its own config/mod pool/profiles) live in a
  // "data" folder next to the program, not in %LOCALAPPDATA% -- the whole
  // program folder can be copied, moved or run from a USB stick as-is.
  // portableRoot() is normally executableDir(), but the single-file launcher
  // redirects it to *its own* folder (see Process.h) since the real GMM.exe it
  // runs lives in a "bin\" subfolder that gets wiped on every version bump.
  return portableRoot() / "data";
}

std::string InstanceStore::gameFolderOf(const fs::path& dataDir)
{
  std::ifstream in(dataDir / "config.json");
  if (!in)
    return "";
  try {
    Json j;
    in >> j;
    return j.value("gamePath", std::string{});
  } catch (...) {
    return "";
  }
}

InstanceStore::InstanceStore()
{
  load();
}

void InstanceStore::load()
{
  m_items.clear();
  m_lastUsed.clear();
  const fs::path file = baseDir() / "instances.json";
  std::ifstream in(file);
  if (!in)
    return;
  try {
    Json j;
    in >> j;
    m_lastUsed = j.value("lastUsed", std::string{});
    for (const auto& e : j.value("instances", Json::array())) {
      InstanceInfo info;
      info.name    = e.value("name", std::string{});
      info.dataDir = fs::path(e.value("path", std::string{}));
      if (!info.name.empty())
        m_items.push_back(std::move(info));
    }
  } catch (...) {
  }
}

void InstanceStore::save()
{
  fs::create_directories(baseDir());
  Json j;
  j["lastUsed"]  = m_lastUsed;
  j["instances"] = Json::array();
  for (const auto& it : m_items)
    j["instances"].push_back(
        {{"name", it.name}, {"path", it.dataDir.generic_string()}});
  std::ofstream out(baseDir() / "instances.json", std::ios::trunc);
  out << j.dump(2) << '\n';
}

void InstanceStore::setLastUsed(const std::string& name)
{
  m_lastUsed = name;
  save();
}

InstanceInfo InstanceStore::create(const std::string& name,
                                   const std::string& gameFolder)
{
  std::string base = sanitize(name);
  fs::path dir     = baseDir() / base;
  int n            = 2;
  while (fs::exists(dir))
    dir = baseDir() / (base + "-" + std::to_string(n++));

  App app(dir);
  app.init(gameFolder);  // creates dirs + config.json

  InstanceInfo info{name, dir};
  m_items.push_back(info);
  m_lastUsed = name;
  save();
  return info;
}

void InstanceStore::remove(const std::string& name, bool deleteFiles)
{
  for (auto it = m_items.begin(); it != m_items.end(); ++it) {
    if (it->name == name) {
      if (deleteFiles) {
        std::error_code ec;
        fs::remove_all(it->dataDir, ec);
      }
      m_items.erase(it);
      break;
    }
  }
  if (m_lastUsed == name)
    m_lastUsed.clear();
  save();
}

// ---------------------------------------------------------------------------

InstanceDialog::InstanceDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle("Инстансы");
  // Standalone applyFrame()/isDarkPalette() (not MainWindow::frameless()):
  // this dialog can run before any MainWindow exists (the very first thing
  // main_gui.cpp shows when there's no instance to open yet), so it can't
  // call back into a MainWindow instance for its theme/chrome.
  auto* root = applyFrame(*this, isDarkPalette());
  resize(560, 360);

  root->addWidget(new QLabel(
      "Выберите инстанс или создайте новый, указав папку GTA San Andreas:", this));

  m_list = new QListWidget(this);
  root->addWidget(m_list, 1);

  auto* row    = new QHBoxLayout();
  auto* newBtn = new QPushButton("Создать…", this);
  auto* rmBtn  = new QPushButton("Удалить…", this);
  row->addWidget(newBtn);
  row->addWidget(rmBtn);
  row->addStretch();
  root->addLayout(row);

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                   this);
  box->button(QDialogButtonBox::Ok)->setText("Выбрать");
  box->button(QDialogButtonBox::Cancel)->setText("Отмена");
  root->addWidget(box);

  connect(newBtn, &QPushButton::clicked, this, &InstanceDialog::onNew);
  connect(rmBtn, &QPushButton::clicked, this, &InstanceDialog::onRemove);
  connect(box, &QDialogButtonBox::accepted, this, &InstanceDialog::onSelect);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_list, &QListWidget::itemDoubleClicked, this, &InstanceDialog::onSelect);

  reload();
  lang::translateUi(this);  // flip the static chrome to English if selected
}

void InstanceDialog::reload()
{
  m_list->clear();
  for (const auto& it : m_store.list()) {
    const std::string game = InstanceStore::gameFolderOf(it.dataDir);
    const QString tail =
        game.empty() ? lang::T("<папка игры не задана>") : QString::fromStdString(game);
    auto* item = new QListWidgetItem(
        QString::fromStdString(it.name) + "    —    " + tail, m_list);
    item->setData(Qt::UserRole, QString::fromStdString(it.name));
    item->setData(Qt::UserRole + 1,
                  QString::fromStdString(it.dataDir.generic_string()));
  }
  if (m_list->count() > 0)
    m_list->setCurrentRow(0);
}

void InstanceDialog::onNew()
{
  bool ok = false;
  const QString name = dialogs::getText(this, lang::T("Новый инстанс"),
                                             lang::T("Название инстанса:"), QLineEdit::Normal,
                                             "GTA San Andreas", &ok);
  if (!ok || name.trimmed().isEmpty())
    return;

  const QString folder = QFileDialog::getExistingDirectory(
      this, lang::T("Выберите папку GTA San Andreas (с gta_sa.exe)"));
  if (folder.isEmpty())
    return;

  std::error_code ec;
  if (!fs::exists(fs::path(folder.toStdString()) / "gta_sa.exe", ec)) {
    if (dialogs::question(
            this, lang::T("gta_sa.exe не найден"),
            lang::T("В этой папке нет gta_sa.exe. Всё равно использовать?")) !=
        QMessageBox::Yes)
      return;
  }

  InstanceInfo created;
  try {
    created = m_store.create(name.trimmed().toStdString(), folder.toStdString());
  } catch (const std::exception& e) {
    dialogs::warning(this, lang::T("Новый инстанс"), QString::fromLocal8Bit(e.what()));
    return;
  }
  reload();
  if (m_list->count() > 0)
    m_list->setCurrentRow(m_list->count() - 1);

  // Offer to capture the vanilla baseline right now, while the game folder is
  // presumably still untouched (nothing deployed into it yet by this brand-new
  // instance) -- it's what rollback()'s orphan cleanup (App::
  // cleanOrphansAgainstBaseline) needs to actually remove stray files later,
  // and without it the cleanup is a silent no-op until the user discovers the
  // "Build the vanilla baseline now" button buried in Settings themselves.
  if (dialogs::question(
          this, lang::T("Эталон ванили"),
          lang::T("Построить эталон ванили для этого инстанса сейчас? Он нужен, "
                  "чтобы откат мог убирать посторонние файлы из папки игры. "
                  "Стройте, только если папка игры сейчас ДЕЙСТВИТЕЛЬНО чистая — "
                  "в неё ещё ничего не разворачивали."),
          QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
    try {
      App app(created.dataDir);
      app.refreshVanillaBaseline();
      dialogs::information(this, lang::T("Эталон ванили"),
                               lang::T("Эталон ванили построен."));
    } catch (const std::exception& e) {
      dialogs::warning(this, lang::T("Эталон ванили"),
                           QString::fromLocal8Bit(e.what()));
    }
  }
}

void InstanceDialog::onRemove()
{
  QListWidgetItem* item = m_list->currentItem();
  if (!item)
    return;
  const QString name = item->data(Qt::UserRole).toString();
  const QMessageBox::StandardButton ans = dialogs::question(
      this, lang::T("Удаление инстанса"),
      lang::T("Убрать инстанс «%1» из списка?\n\nДа = также удалить его данные (пул, "
              "профили).\nНет = оставить файлы на диске, только убрать из списка.")
          .arg(name),
      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (ans == QMessageBox::Cancel)
    return;
  m_store.remove(name.toStdString(), ans == QMessageBox::Yes);
  reload();
}

void InstanceDialog::onSelect()
{
  QListWidgetItem* item = m_list->currentItem();
  if (!item) {
    dialogs::information(this, lang::T("Инстансы"),
                             lang::T("Сначала создайте инстанс (кнопка «Создать…»)."));
    return;
  }
  m_selected = fs::path(item->data(Qt::UserRole + 1).toString().toStdString());
  m_store.setLastUsed(item->data(Qt::UserRole).toString().toStdString());
  accept();
}

std::optional<fs::path> InstanceDialog::choose(QWidget* parent)
{
  InstanceDialog dlg(parent);
  centerOverWidget(dlg, parent ? parent->window() : nullptr);
  if (dlg.exec() == QDialog::Accepted && !dlg.selected().empty())
    return dlg.selected();
  return std::nullopt;
}

}  // namespace gtamm
