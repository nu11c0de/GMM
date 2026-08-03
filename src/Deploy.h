#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Json.h"

namespace gtamm {

// A file path provided by more than one enabled mod. `winner` is deployed; the
// `losers` are shadowed (lower priority).
struct Conflict
{
  std::string path;                 // game-relative path (generic, lowercased key)
  std::string winner;               // mod id that wins
  std::vector<std::string> losers;  // other providers, highest priority first
};

// One deployed file, recorded so it can be undone exactly.
struct ManifestFile
{
  std::string rel;        // game-relative path (as deployed, original case)
  std::string mod;        // winning mod id
  std::string method;     // "hardlink" or "copy"
  bool hadBackup = false; // an original game file was moved aside
};

// The record of a deployment, used to roll back to the clean game folder.
struct Manifest
{
  std::string gameDir;
  std::string profile;
  std::int64_t deployedAt = 0;
  std::vector<ManifestFile> files;
  std::vector<std::string> createdDirs;  // dirs we created (game-relative, generic)
  std::vector<std::string> imgFiles;     // IMG archives we rebuilt (game-relative)
  std::vector<std::string> sfxPaks;      // SFX paks we rebuilt (game-relative)
  bool sfxConfig = false;                // audio/CONFIG/BankLkup.dat was rewritten
};

inline void to_json(Json& j, const ManifestFile& f)
{
  j = Json{{"rel", f.rel}, {"mod", f.mod}, {"method", f.method}, {"hadBackup", f.hadBackup}};
}

inline void from_json(const Json& j, ManifestFile& f)
{
  j.at("rel").get_to(f.rel);
  f.mod       = j.value("mod", std::string{});
  f.method    = j.value("method", std::string{"hardlink"});
  f.hadBackup = j.value("hadBackup", false);
}

inline void to_json(Json& j, const Manifest& m)
{
  j = Json{{"gameDir", m.gameDir},
           {"profile", m.profile},
           {"deployedAt", m.deployedAt},
           {"files", m.files},
           {"createdDirs", m.createdDirs},
           {"imgFiles", m.imgFiles},
           {"sfxPaks", m.sfxPaks},
           {"sfxConfig", m.sfxConfig}};
}

inline void from_json(const Json& j, Manifest& m)
{
  m.gameDir     = j.value("gameDir", std::string{});
  m.profile     = j.value("profile", std::string{});
  m.deployedAt  = j.value("deployedAt", static_cast<std::int64_t>(0));
  m.files       = j.value("files", std::vector<ManifestFile>{});
  m.createdDirs = j.value("createdDirs", std::vector<std::string>{});
  m.imgFiles    = j.value("imgFiles", std::vector<std::string>{});
  m.sfxPaks     = j.value("sfxPaks", std::vector<std::string>{});
  m.sfxConfig   = j.value("sfxConfig", false);
}

}  // namespace gtamm
