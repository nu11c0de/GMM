#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

// Vanilla GTA SA 1.0 "fingerprint" used to import an existing modded build by
// diffing it against the clean game. A baseline records, for every original
// file, its size and a content hash -- both for loose files and for the entries
// inside IMG archives. A build's file that is missing from, or differs from, the
// baseline is a mod file; everything that matches the baseline is untouched
// vanilla and is ignored.
namespace gtamm {

// One baseline entry: a cheap size guard plus a content hash. Size lets us skip
// hashing a build file outright when its size already differs from vanilla.
struct BaselineEntry
{
  std::uint64_t size = 0;  // bytes for loose files; sector count for IMG entries
  std::string hash;        // FNV-1a 64 hex digest of the contents
};

struct Baseline
{
  // lowercased game-relative path -> entry (loose vanilla files)
  std::map<std::string, BaselineEntry> loose;
  // "<imgfilename-lc>::<entryname-lc>" -> entry (contents of IMG archive entries)
  std::map<std::string, BaselineEntry> img;

  bool empty() const { return loose.empty() && img.empty(); }
};

// FNV-1a 64 hex digest helpers.
std::string hashBytesFnv(const unsigned char* data, std::size_t n);
std::string hashFileFnv(const std::filesystem::path& file);

// Build a baseline by scanning a clean game directory (hashes every loose file
// and every IMG archive entry). One-off and somewhat slow on a full install.
Baseline buildBaseline(const std::filesystem::path& gameDir);

void saveBaseline(const std::filesystem::path& path, const Baseline& b);
Baseline loadBaseline(const std::filesystem::path& path);

// Which mod a changed build file is attributed to. Builds laid out on Mod Loader
// (modloader/<name>/...), CLEO scripts and ASI plugins carry enough structure to
// split into named, separately toggleable mods; everything else is lumped into a
// single "rest of the build" mod (key "main").
struct BuildGroup
{
  std::string key;       // stable grouping id ("ml:<name>", "cleo:<stem>", ...)
  std::string name;      // best display name for the resulting mod
  std::string innerRel;  // path of the file within the mod (game-dir relative)
  bool primary = false;  // true for the file that names the group (.asi/.cs/...)
};

// Classify one changed build file (game-relative, '/'-separated, original case).
// `asiStems` / `cleoStems` / `moonloaderStems` are the lowercased stems of
// every .asi plugin, CLEO script and top-level MoonLoader script (moonloader/
// <name>.lua -- NOT a subfolder, those are shared libraries the scripts
// `require()`, not standalone mods) seen in the build, so each one's side
// files (its .ini, its data folder) are grouped with it instead of scattered
// into "main".
BuildGroup classifyBuildFile(const std::string& gameRel,
                             const std::set<std::string>& asiStems,
                             const std::set<std::string>& cleoStems,
                             const std::set<std::string>& moonloaderStems,
                             const std::string& buildName);

}  // namespace gtamm
