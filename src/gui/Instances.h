#pragma once
#include <QDialog>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class QListWidget;

namespace gtamm {

// One managed instance: a name and the data directory that holds its config,
// mod pool and profiles. The game folder is stored inside that data dir.
struct InstanceInfo
{
  std::string name;
  std::filesystem::path dataDir;
};

// Registry of instances, persisted portably at <exeDir>/data/instances.json
// (next to the program, not in %LOCALAPPDATA%).
class InstanceStore
{
public:
  InstanceStore();

  std::vector<InstanceInfo> list() const { return m_items; }
  std::string lastUsed() const { return m_lastUsed; }
  void setLastUsed(const std::string& name);

  // Create a new instance: makes its data dir under the base folder, writes its
  // config pointing at `gameFolder`. Returns the created instance.
  InstanceInfo create(const std::string& name, const std::string& gameFolder);
  void remove(const std::string& name, bool deleteFiles);

  // Game folder configured for an instance (read from its config.json).
  static std::string gameFolderOf(const std::filesystem::path& dataDir);
  static std::filesystem::path baseDir();

private:
  void load();
  void save();
  std::vector<InstanceInfo> m_items;
  std::string m_lastUsed;
};

// Modal picker to choose / create / remove instances.
class InstanceDialog : public QDialog
{
  Q_OBJECT
public:
  explicit InstanceDialog(QWidget* parent = nullptr);
  std::filesystem::path selected() const { return m_selected; }

  // Show the picker; returns the chosen instance's data dir, or nullopt if the
  // user cancelled.
  static std::optional<std::filesystem::path> choose(QWidget* parent);

private slots:
  void onNew();
  void onRemove();
  void onSelect();

private:
  void reload();
  InstanceStore m_store;
  QListWidget* m_list = nullptr;
  std::filesystem::path m_selected;
};

}  // namespace gtamm
