#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Pushing GMM's view of the world into a Sanny Builder 4 installation.
//
// SB4 is portable: everything lives next to sanny.exe. Two files matter here,
// and both are edited IN PLACE, line by line, rather than rewritten from a
// parsed model -- they are the user's own files (hand-tuned themes, hotkeys,
// per-mode data) and must come back unchanged apart from the one value we own.
//
//   data/settings.ini        -- "EditMode=<mode>" plus, per *game*, "GamePath".
//   data/<mode>/mode.xml     -- where "@game:" expands to, and the compile
//                               output directories (<copy-directory>).
//
// The game directory is per edit mode in the UI, but stored under the mode's
// *game* id (mode.xml's game="..." attribute), so several SA modes share one
// path -- which is why setGamePath() takes a game id and not a mode id.
namespace gtamm::sanny {

struct SyncResult
{
  bool gamePathSet = false;  // settings.ini updated
  bool cleoDirSet  = false;  // mode.xml's CLEO output redirected
  std::string mode;          // edit mode in use, e.g. "sa_sbl"
  std::string game;          // its game id / ini section, e.g. "sa"
  std::vector<std::string> notes;  // anything the user should know about
};

std::filesystem::path settingsIni(const std::filesystem::path& sbDir);

// The edit mode currently selected in Sanny Builder ("sa" when unset -- the
// stock GTA SA mode, which is the only thing GMM targets anyway).
std::string currentEditMode(const std::filesystem::path& sbDir);

// The game id a mode belongs to, read from data/<mode>/mode.xml. Falls back to
// the mode id itself when the file or the attribute is missing.
std::string modeGameId(const std::filesystem::path& sbDir, const std::string& mode);

// Writes GamePath into the [<gameId>] section of settings.ini, creating the
// section if needed. GamePath keys in *other* sections belong to other games
// and are left alone. Returns false if the file could not be written.
bool setGamePath(const std::filesystem::path& sbDir, const std::string& gameId,
                 const std::filesystem::path& gameDir);

// Points <copy-directory type="cleo"> of data/<mode>/mode.xml at `cleoDir`, so
// compiled scripts land there instead of in the game folder. An empty path
// restores the stock "@game:\CLEO".
bool setCleoOutputDir(const std::filesystem::path& sbDir, const std::string& mode,
                      const std::filesystem::path& cleoDir);

}  // namespace gtamm::sanny
