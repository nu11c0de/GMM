#include "App.h"

#include "Baseline.h"
#include "GameRules.h"
#include "Unpack.h"
#include "Img.h"
#include "Sfx.h"
#include "Process.h"
#include "UserFiles.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace gtamm {

namespace {

Json readJson(const fs::path& p)
{
  std::ifstream in(p);
  if (!in)
    throw std::runtime_error("cannot open file for reading: " + p.string());
  Json j;
  in >> j;
  return j;
}

void writeJson(const fs::path& p, const Json& j)
{
  // Write atomically: serialize to a temp file, then replace, so a crash mid-
  // write can never leave a truncated/corrupt config or profile behind.
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot open file for writing: " + tmp.string());
    out << j.dump(2) << '\n';
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp, ec);
    if (ec)
      throw std::runtime_error("cannot write file: " + p.string());
  }
}

// Sanitize a display name into a filesystem-safe pool id.
std::string sanitizeId(const std::string& name)
{
  std::string s;
  for (char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '.' || c == '-' || c == '_')
      s += c;
    else if (c == ' ')
      s += '_';
    // everything else is dropped
  }
  if (s.empty())
    s = "mod";
  return s;
}

bool isValidProfileName(const std::string& name)
{
  if (name.empty())
    return false;
  for (char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (!(std::isalnum(uc) || c == '.' || c == '-' || c == '_' || c == ' '))
      return false;
  }
  return true;
}

// Mod archives often wrap content in one or more folders (e.g.
// "CoolCar v1.2/models/..."). Descend through single wrapper directories until
// we reach the real mod root, i.e. content laid out relative to the game dir.
// Stops as soon as a recognized game folder (data/models/cleo/...) or real files
// appear at the current level.
fs::path normalizeModRoot(fs::path root)
{
  for (int guard = 0; guard < 16; ++guard) {
    std::vector<fs::path> nonDoc;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (e.is_regular_file(ec) && rules::isJunkFile(e.path().filename().string()))
        continue;  // ignore readme/screenshots when deciding
      nonDoc.push_back(e.path());
    }
    if (nonDoc.size() != 1 || !fs::is_directory(nonDoc[0], ec))
      break;
    if (rules::isGameTopDir(nonDoc[0].filename().string()))
      break;  // the single dir IS game content (e.g. a lone "models" folder)
    root = nonDoc[0];
  }
  return root;
}

std::string lowerAscii(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool iequals(const std::string& a, const std::string& b)
{
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

// List the mods inside a "modloader" directory. Mirroring Mod Loader itself
// (FolderInformation::Scan walks modloader/ non-recursively and treats each
// immediate subfolder as one mod), every direct subfolder is a single mod -- we
// do NOT descend into "category" folders or split a mod by its inner layout.
// Dotfolders (Mod Loader's own .data/.profiles/...) and loose files are skipped.
void listModloaderMods(const fs::path& mlDir,
                       std::vector<std::pair<std::string, fs::path>>& out)
{
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(mlDir, ec)) {
    if (!e.is_directory(ec))
      continue;
    const std::string name = e.path().filename().string();
    if (name.empty() || name[0] == '.')
      continue;
    out.emplace_back(name, e.path());
  }
}

// Re-base a file's path inside a Mod Loader mod to where the game actually wants
// it. Mod Loader routes files by type/name and ignores a mod's internal folder
// layout (e.g. "MSKL/data/x.gxt", "Radar HD/radar00.txd"); our deployer is
// path-based, so we strip organizational wrappers: keep the path from the first
// recognized game folder (data/models/text/...), or from a SAAT "Bank_<n>"
// folder (so sound mods still route through the SFX injector), otherwise fall
// back to the bare filename (IMG content is then injected into its archive by
// name, the rest lands at the game root).
std::string modloaderGameRel(const fs::path& rel)
{
  std::vector<std::string> comps;
  for (const auto& p : rel)
    comps.push_back(p.string());
  if (comps.empty())
    return rel.generic_string();

  auto joinFrom = [&](std::size_t i) {
    std::string s;
    for (; i < comps.size(); ++i) {
      if (!s.empty())
        s += '/';
      s += comps[i];
    }
    return s;
  };
  for (std::size_t i = 0; i < comps.size(); ++i)
    if (rules::isGameTopDir(comps[i]))
      return joinFrom(i);
  for (std::size_t i = 0; i < comps.size(); ++i)
    if (sfx::parseBankDir(comps[i]) >= 1)
      // Keep the pak-name parent folder if present ("GENRL/Bank_046/..."), so the
      // deployer resolves the SFX pak explicitly instead of guessing from the
      // mod's name.
      return joinFrom(i >= 1 ? i - 1 : i);
  return comps.back();
}

// Turn a mod's display name into a single safe path component for use as its
// "modloader/<folder>/" deploy folder (Mod Loader identifies mods by folder name).
std::string modloaderFolderName(const std::string& name)
{
  std::string s;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' ||
        c == '|' || c == '?' || c == '*')
      s += '_';
    else
      s += c;
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
    s.pop_back();
  return s.empty() ? std::string("mod") : s;
}

// Whether `root` holds meaningful content OUTSIDE the modloader folder `ml`.
// True for a full game build / mixed folder that merely *contains* a modloader
// folder (e.g. gta_sa.exe + CLEO.asi + DLLs + modloader/); false for a clean Mod
// Loader *pack* whose entire payload is the modloader folder. We must not split
// such a mixed folder into Mod Loader mods: that silently drops everything that
// is not under modloader/. Junk files (readme/screenshots) don't count.
bool hasContentOutsideModloader(const fs::path& root, const fs::path& ml)
{
  std::error_code ec;
  for (const auto& de : fs::recursive_directory_iterator(root, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    // A file is "inside modloader/" when its path relative to ml does not climb
    // out with "..". (When ml == root every file is inside, so there is no
    // outside content -- a clean Mod Loader pack.)
    const std::string rel = fs::relative(de.path(), ml, ec).generic_string();
    const bool insideMl = !ec && !rel.empty() && rel.rfind("..", 0) != 0;
    if (insideMl)
      continue;
    if (rules::isJunkFile(de.path().filename().string()))
      continue;  // readme/screenshot next to modloader/
    return true;  // a real sibling: game exe, root ASI, CLEO script, DLL, ...
  }
  return false;
}

// Find a directory named `target` (case-insensitive) at `root` or within `depth`
// levels below it. Used to locate a "modloader" folder in an imported tree.
fs::path findDirNamed(const fs::path& root, const std::string& target, int depth)
{
  if (iequals(root.filename().string(), target))
    return root;
  if (depth <= 0)
    return {};
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (e.is_directory(ec)) {
      const fs::path f = findDirNamed(e.path(), target, depth - 1);
      if (!f.empty())
        return f;
    }
  }
  return {};
}

// Stable content digest of a mod tree: FNV-1a 64 over each file's (lowercased
// relative path + contents), files visited in sorted order. Used to detect an
// identical mod already in the pool.
std::string hashModTree(const fs::path& root)
{
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& de : fs::recursive_directory_iterator(root, ec))
    if (de.is_regular_file(ec))
      files.push_back(fs::relative(de.path(), root, ec));
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    return a.generic_string() < b.generic_string();
  });

  std::uint64_t h = 1469598103934665603ULL;
  auto feed       = [&](const char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      h ^= static_cast<unsigned char>(p[i]);
      h *= 1099511628211ULL;
    }
  };
  for (const fs::path& rel : files) {
    std::string key = rel.generic_string();
    for (char& c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    feed(key.data(), key.size());
    feed("\0", 1);
    std::ifstream in(root / rel, std::ios::binary);
    char buf[65536];
    while (in.read(buf, sizeof(buf)) || in.gcount())
      feed(buf, static_cast<std::size_t>(in.gcount()));
    feed("\0", 1);
  }
  char out[17];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return std::string(out);
}

}  // namespace

App::App(fs::path dataDir) : m_dataDir(std::move(dataDir))
{
  if (isInitialized())
    loadConfig();
}

bool App::isInitialized() const
{
  std::error_code ec;
  return fs::exists(configPath(), ec);
}

void App::requireInit() const
{
  if (!m_loaded)
    throw std::runtime_error(
        "data directory is not initialized; run `gtamm init` first");
}

void App::init(const std::string& gamePath)
{
  fs::create_directories(modsDir());
  fs::create_directories(profilesDir());
  m_gamePath      = gamePath;
  m_activeProfile = "";
  // Keep the game folder pristine and give each profile its own saves by default
  // (the user can turn these off in the Profile menu).
  m_manageSaves     = true;
  m_manageGenerated = true;
  // Mod Loader (+ CLEO + bundled compatibility fixes + the matching clean 1.0
  // exe) on by default too, so a bare/vanilla build works out of the box with
  // zero setup -- the user can turn it off in Settings -> Mod Loader.
  m_enableModloader = true;
  m_loaded          = true;
  saveConfig();
  // Give a fresh instance a ready-to-use "Default" profile so the user can
  // import mods and play right away without creating one by hand.
  createProfile("Default");
}

void App::setGamePath(const std::string& gamePath)
{
  requireInit();
  m_gamePath = gamePath;
  saveConfig();
}

void App::loadConfig()
{
  const Json j   = readJson(configPath());
  m_gamePath     = j.value("gamePath", std::string{});
  m_activeProfile = j.value("activeProfile", std::string{});
  m_manageSaves  = j.value("manageSaves", true);
  m_manageGenerated = j.value("manageGenerated", true);
  m_autoRouteMaps = j.value("autoRouteMaps", false);
  m_modloaderRuntimeDir = j.value("modloaderRuntimeDir", std::string{});
  m_sampRuntimeDir = j.value("sampRuntimeDir", std::string{});
  m_enableModloader = j.value("enableModloader", true);
  m_enableEssentials = j.value("enableEssentials", true);
  m_replaceGameExe = j.value("replaceGameExe", false);
  m_steamIntegration = j.value("steamIntegration", false);
  m_steamApiDll  = j.value("steamApiDll", std::string{});
  m_sannyBuilderPath = j.value("sannyBuilderPath", std::string{});
  m_showSannyBuilder = j.value("showSannyBuilder", false);
  m_sannyCleoModId = j.value("sannyCleoModId", std::string{});
  m_loaded       = true;
  // If a previous session crashed mid-game, undo the leftover saves swap so the
  // user's real User Files folder is back in place.
  recoverSaves();
}

void App::saveConfig() const
{
  const Json j = Json{{"gamePath", m_gamePath},
                      {"activeProfile", m_activeProfile},
                      {"manageSaves", m_manageSaves},
                      {"manageGenerated", m_manageGenerated},
                      {"autoRouteMaps", m_autoRouteMaps},
                      {"modloaderRuntimeDir", m_modloaderRuntimeDir},
                      {"sampRuntimeDir", m_sampRuntimeDir},
                      {"enableModloader", m_enableModloader},
                      {"enableEssentials", m_enableEssentials},
                      {"replaceGameExe", m_replaceGameExe},
                      {"steamIntegration", m_steamIntegration},
                      {"steamApiDll", m_steamApiDll},
                      {"sannyBuilderPath", m_sannyBuilderPath},
                      {"showSannyBuilder", m_showSannyBuilder},
                      {"sannyCleoModId", m_sannyCleoModId}};
  writeJson(configPath(), j);
}

void App::setManageSaves(bool on)
{
  requireInit();
  m_manageSaves = on;
  saveConfig();
}

void App::setManageGenerated(bool on)
{
  requireInit();
  m_manageGenerated = on;
  saveConfig();
}

void App::setAutoRouteMaps(bool on)
{
  requireInit();
  m_autoRouteMaps = on;
  saveConfig();
}

void App::setSteamIntegration(bool on)
{
  requireInit();
  m_steamIntegration = on;
  saveConfig();
}

void App::setSannyBuilderPath(const std::string& path)
{
  requireInit();
  m_sannyBuilderPath = path;
  saveConfig();
}

void App::setShowSannyBuilder(bool on)
{
  requireInit();
  m_showSannyBuilder = on;
  saveConfig();
}

fs::path App::sannyBuilderExe() const
{
  if (m_sannyBuilderPath.empty())
    return {};
  const fs::path p(m_sannyBuilderPath);
  std::error_code ec;
  // Either the executable itself or the folder it lives in -- pointing a file
  // picker at "SannyBuilder-v4.2.0" is the natural thing to do, and Sanny
  // Builder 4 ships its launcher as sanny.exe in that folder's root.
  if (fs::is_regular_file(p, ec))
    return p;
  if (fs::is_directory(p, ec)) {
    for (const char* name : {"sanny.exe", "sannybuilder.exe"}) {
      const fs::path candidate = p / name;
      if (fs::is_regular_file(candidate, ec))
        return candidate;
    }
  }
  return {};
}

bool App::hasSannyBuilder() const
{
  return !sannyBuilderExe().empty();
}

void App::setSannyCleoModId(const std::string& modId)
{
  requireInit();
  m_sannyCleoModId = modId;
  saveConfig();
}

sanny::SyncResult App::syncSannyBuilder()
{
  requireInit();
  sanny::SyncResult r;
  const fs::path exe = sannyBuilderExe();
  if (exe.empty()) {
    r.notes.push_back("Sanny Builder is not configured");
    return r;
  }
  const fs::path sbDir = exe.parent_path();
  r.mode = sanny::currentEditMode(sbDir);
  r.game = sanny::modeGameId(sbDir, r.mode);

  // Non-ASCII is worth calling out: Sanny Builder is a Delphi program writing a
  // plain INI, and its idea of the code page is not necessarily ours, so a path
  // with Cyrillic in it may come back mangled on its side.
  auto nonAscii = [](const std::string& s) {
    for (const char c : s)
      if (static_cast<unsigned char>(c) > 0x7F)
        return true;
    return false;
  };

  std::error_code ec;
  if (m_gamePath.empty() || !fs::exists(fs::path(m_gamePath), ec)) {
    r.notes.push_back("game folder is not set or missing; GamePath left alone");
  } else {
    r.gamePathSet = sanny::setGamePath(sbDir, r.game, fs::path(m_gamePath));
    if (!r.gamePathSet)
      r.notes.push_back("could not write " + sanny::settingsIni(sbDir).string());
    else if (nonAscii(m_gamePath))
      r.notes.push_back("the game path contains non-Latin characters; check it "
                        "in Sanny Builder's options");
  }

  if (!m_sannyCleoModId.empty()) {
    const auto mod = findMod(m_sannyCleoModId);
    if (!mod) {
      r.notes.push_back("the mod chosen for compiled scripts no longer exists");
    } else {
      // Created up front: Sanny Builder copies into this directory and won't
      // make it for us.
      const fs::path cleoDir = modsDir() / mod->id / "root" / "CLEO";
      fs::create_directories(cleoDir, ec);
      r.cleoDirSet = sanny::setCleoOutputDir(sbDir, r.mode, cleoDir);
      if (!r.cleoDirSet)
        r.notes.push_back("could not update the edit mode's mode.xml");
      else if (nonAscii(cleoDir.string()))
        r.notes.push_back("the mod path contains non-Latin characters; check "
                          "the output directory in Sanny Builder");
    }
  }
  return r;
}

fs::path App::modloaderRuntimeDir() const
{
  // 1. An explicit per-instance override always wins.
  if (!m_modloaderRuntimeDir.empty())
    return fs::path(m_modloaderRuntimeDir);
  // 2. The runtime bundled next to the program ("runtime/" beside GMM.exe) — this
  // is the default so it "just works" for every instance without any setup.
  std::error_code ec;
  const fs::path bundled = executableDir() / "runtime";
  if (fs::exists(bundled / "modloader.asi", ec))
    return bundled;
  // 3. Fall back to a per-instance store the user can populate manually.
  return m_dataDir / "modloader-runtime";
}

void App::setModloaderRuntimeDir(const std::string& dir)
{
  requireInit();
  m_modloaderRuntimeDir = dir;
  saveConfig();
}

void App::setReplaceGameExe(bool on)
{
  requireInit();
  m_replaceGameExe = on;
  saveConfig();
}

void App::setEnableModloader(bool on)
{
  requireInit();
  m_enableModloader = on;
  saveConfig();
}

void App::setEnableEssentials(bool on)
{
  requireInit();
  m_enableEssentials = on;
  saveConfig();
}

bool App::hasBundledExe() const
{
  std::error_code ec;
  return fs::exists(modloaderRuntimeDir() / "gta_sa.exe", ec);
}

bool App::hasModloaderRuntime() const
{
  std::error_code ec;
  // Already installed in the game folder?
  if (!m_gamePath.empty() &&
      fs::exists(fs::path(m_gamePath) / "modloader.asi", ec))
    return true;
  // Or available for us to install from the runtime store?
  const fs::path rt = modloaderRuntimeDir();
  return fs::exists(rt / "modloader.asi", ec);
}

fs::path App::sampRuntimeDir() const
{
  // 1. An explicit per-instance override always wins.
  if (!m_sampRuntimeDir.empty())
    return fs::path(m_sampRuntimeDir);
  // 2. The client bundled next to the program ("runtime-samp/" beside GMM.exe) --
  // the default so a fresh instance needs no manual setup.
  std::error_code ec;
  const fs::path bundled = executableDir() / "runtime-samp";
  if (fs::exists(bundled / "samp.exe", ec))
    return bundled;
  // 3. Fall back to a per-instance store the user can populate manually.
  return m_dataDir / "samp-runtime";
}

void App::setSampRuntimeDir(const std::string& dir)
{
  requireInit();
  m_sampRuntimeDir = dir;
  saveConfig();
}

bool App::hasSampRuntime() const
{
  std::error_code ec;
  return fs::exists(sampRuntimeDir() / "samp.exe", ec);
}

fs::path App::profileSavesDir(const std::string& name) const
{
  return savesDir() / name;
}

// --- mod pool ---------------------------------------------------------------

std::vector<Mod> App::mods() const
{
  requireInit();
  std::vector<Mod> result;
  std::error_code ec;
  if (!fs::exists(modsDir(), ec))
    return result;

  for (const auto& entry : fs::directory_iterator(modsDir())) {
    if (!entry.is_directory())
      continue;
    const fs::path meta = entry.path() / "meta.json";
    if (!fs::exists(meta))
      continue;
    Mod m = readJson(meta).get<Mod>();
    result.push_back(std::move(m));
  }
  std::sort(result.begin(), result.end(),
            [](const Mod& a, const Mod& b) { return a.id < b.id; });
  return result;
}

std::optional<Mod> App::findMod(const std::string& id) const
{
  const fs::path meta = modsDir() / id / "meta.json";
  if (!fs::exists(meta))
    return std::nullopt;
  return readJson(meta).get<Mod>();
}

std::string App::makeUniqueId(const std::string& name) const
{
  const std::string base = sanitizeId(name);
  std::string id         = base;
  int n                  = 2;
  std::error_code ec;
  while (fs::exists(modsDir() / id, ec))
    id = base + "-" + std::to_string(n++);
  return id;
}

std::string App::ensureContentHash(const Mod& m)
{
  if (!m.contentHash.empty())
    return m.contentHash;
  const std::string h = hashModTree(modsDir() / m.id / "root");
  // Backfill meta.json so older mods don't get rehashed every time.
  std::error_code ec;
  if (fs::exists(modsDir() / m.id / "meta.json", ec)) {
    Mod updated        = m;
    updated.contentHash = h;
    writeJson(modsDir() / m.id / "meta.json", Json(updated));
  }
  return h;
}

App::ImportResult App::importFromFolder(const fs::path& src, const std::string& nameOpt)
{
  requireInit();
  std::error_code ec;
  if (!fs::exists(src, ec) || !fs::is_directory(src, ec))
    throw std::runtime_error("source is not an existing folder: " + src.string());

  std::string name = nameOpt;
  if (name.empty())
    name = src.filename().string();
  if (name.empty())
    name = "mod";

  // Unwrap any wrapper folders so the stored tree is game-dir-relative.
  const fs::path effectiveSrc = normalizeModRoot(src);
  const std::string hash      = hashModTree(effectiveSrc);

  // Dedup: if an identical mod is already in the pool, don't import a copy.
  for (const Mod& existing : mods()) {
    if (ensureContentHash(existing) == hash)
      return {existing, true};
  }

  const std::string id   = makeUniqueId(name);
  const fs::path modDir  = modsDir() / id;
  const fs::path rootDir = modDir / "root";
  fs::create_directories(rootDir);

  fs::copy(effectiveSrc, rootDir,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing);

  Mod m;
  m.id          = id;
  m.name        = name;
  m.source      = src.string();
  m.importedAt  = static_cast<std::int64_t>(std::time(nullptr));
  m.contentHash = hash;
  writeJson(modDir / "meta.json", Json(m));
  return {m, false};
}

App::ImportResult App::adoptMod(const fs::path& rootDir, const std::string& name,
                                const std::string& sourceLabel,
                                const std::string& hashOpt)
{
  requireInit();
  std::error_code ec;
  if (!fs::exists(rootDir, ec) || !fs::is_directory(rootDir, ec))
    throw std::runtime_error("mod root not found: " + rootDir.string());

  const std::string hash = hashOpt.empty() ? hashModTree(rootDir) : hashOpt;

  // Dedup: an identical mod already in the pool is reused as-is.
  for (const Mod& existing : mods()) {
    if (ensureContentHash(existing) == hash)
      return {existing, true};
  }

  std::string nm = name.empty() ? "mod" : name;
  const std::string id  = makeUniqueId(nm);
  const fs::path modDir = modsDir() / id;
  const fs::path poolRt = modDir / "root";
  fs::create_directories(poolRt);
  fs::copy(rootDir, poolRt,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing);

  Mod m;
  m.id          = id;
  m.name        = nm;
  m.source      = sourceLabel;
  m.importedAt  = static_cast<std::int64_t>(std::time(nullptr));
  m.contentHash = hash;
  writeJson(modDir / "meta.json", Json(m));
  return {m, false};
}

App::ImportResult App::importFromArchive(const fs::path& archivePath,
                                         const std::string& nameOpt,
                                         const std::string& password)
{
  requireInit();
  std::error_code ec;
  if (!fs::exists(archivePath, ec) || !fs::is_regular_file(archivePath, ec))
    throw std::runtime_error("archive not found: " + archivePath.string());

  const fs::path tmp =
      dataDir() / "_import_tmp" / std::to_string(std::time(nullptr));
  fs::create_directories(tmp);
  try {
    if (hasArchiveExtension(archivePath)) {
      extractArchive(archivePath, tmp, password);
      // Some mods are distributed as a wrapper archive whose only content is
      // the real mod's own archive (a common pattern for password-protected
      // zips used to bypass filehost restrictions) -- unpack any of those in
      // place so the mod ends up as actual files, not a useless nested zip.
      flattenNestedArchives(tmp, password);
    } else {
      // Not actually an archive -- a single loose mod file (a standalone
      // .lua script, .asi plugin, .cs/.cleo CLEO script, ...) passed
      // directly instead of wrapped in a folder or zip. Stage it alone so it
      // goes through the exact same import pipeline as if it had been the
      // sole file inside a dropped folder.
      fs::copy_file(archivePath, tmp / archivePath.filename());
    }
    std::string name = nameOpt;
    if (name.empty())
      name = archivePath.stem().string();
    ImportResult r = importFromFolder(tmp, name);  // normalizes + dedups
    fs::remove_all(tmp, ec);
    return r;
  } catch (...) {
    fs::remove_all(tmp, ec);
    throw;
  }
}

std::vector<App::ImportResult> App::importFolderAsMods(const fs::path& src,
                                                       const std::string& nameOpt,
                                                       bool viaModloader)
{
  requireInit();
  std::error_code ec;
  if (!fs::exists(src, ec) || !fs::is_directory(src, ec))
    throw std::runtime_error("source is not an existing folder: " + src.string());

  const fs::path root = normalizeModRoot(src);

  // "With Mod Loader support": keep the mod's tree exactly as authored and flag
  // it to deploy back under modloader/<name>/. A Mod Loader *pack* is still split
  // one-mod-per-subfolder, but each subtree is adopted verbatim (no flattening).
  if (viaModloader) {
    const fs::path ml = findDirNamed(root, "modloader", 2);
    if (!ml.empty() && !hasContentOutsideModloader(root, ml)) {
      std::vector<std::pair<std::string, fs::path>> mlMods;
      listModloaderMods(ml, mlMods);
      if (!mlMods.empty()) {
        std::sort(mlMods.begin(), mlMods.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<ImportResult> results;
        for (const auto& [name, path] : mlMods)
          results.push_back(importModloaderModVerbatim(path, name));
        return results;
      }
    }
    std::string name = nameOpt.empty() ? root.filename().string() : nameOpt;
    if (name.empty())
      name = src.filename().string();
    return {importModloaderModVerbatim(root, name)};
  }

  // A GMM single-mod archive (mod.json + root/) produced by exportMod(): adopt
  // its exact game-relative tree verbatim, so it deploys identically to the
  // source pool -- no wrapper-unwrapping or Mod Loader re-routing guesswork.
  {
    const fs::path manifest = root / "mod.json";
    const fs::path modRoot   = root / "root";
    if (fs::exists(manifest, ec) && fs::is_directory(modRoot, ec)) {
      const Json j = readJson(manifest);
      if (j.value("format", std::string{}) == "gmm-mod") {
        std::string name, hash;
        bool wantModloader = false;
        if (j.contains("mod")) {
          const Mod mm  = j.at("mod").get<Mod>();
          name          = mm.name;
          hash          = mm.contentHash;
          wantModloader = mm.viaModloader;
        }
        if (name.empty())
          name = nameOpt.empty() ? src.filename().string() : nameOpt;
        ImportResult r = adoptMod(modRoot, name, "modzip:" + src.filename().string(), hash);
        // Only a freshly-added mod's deploy target is set here -- a dedup hit
        // returns an existing pool mod whose flag may already differ deliberately
        // (e.g. the user re-flagged it natively), so it must not be overwritten.
        if (!r.wasDuplicate && r.mod.viaModloader != wantModloader) {
          setModViaModloader(r.mod.id, wantModloader);
          r.mod.viaModloader = wantModloader;
        }
        return {r};
      }
    }
  }

  // If this is a Mod Loader *pack* (a "modloader" folder that is the whole
  // payload), import each of its mods separately -- exactly as Mod Loader sees
  // them: one mod per immediate subfolder, re-based to game-relative paths.
  // BUT if the folder is a full game build / mixed tree that merely contains a
  // modloader folder alongside other content (the game exe, root ASIs, CLEO,
  // DLLs), splitting would silently drop everything outside modloader/ -- so we
  // fall through and import the whole tree as one ordinary mod instead.
  const fs::path ml = findDirNamed(root, "modloader", 2);
  if (!ml.empty() && !hasContentOutsideModloader(root, ml)) {
    std::vector<std::pair<std::string, fs::path>> mlMods;
    listModloaderMods(ml, mlMods);
    if (!mlMods.empty()) {
      std::sort(mlMods.begin(), mlMods.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      const auto looseIndex = buildLooseIndex();
      std::vector<ImportResult> results;
      for (const auto& [name, path] : mlMods)
        results.push_back(importModloaderMod(path, name, looseIndex));
      return results;
    }
  }

  return {importFromFolder(src, nameOpt)};
}

App::ImportResult App::importModloaderMod(
    const fs::path& modFolder, const std::string& name,
    const std::map<std::string, std::string>& looseIndex)
{
  requireInit();
  std::error_code ec;

  // An ASI-plugin mod (it contains a *.asi) is game-root-relative as-is: the
  // plugin and its data folder/config (e.g. CheatMenuSA.asi + CheatMenuSA/ +
  // CheatMenuSA.toml) must keep their exact layout, so we copy it whole without
  // flattening or re-routing.
  bool asiMod = false;
  for (const auto& de : fs::recursive_directory_iterator(modFolder, ec)) {
    if (de.is_regular_file(ec) &&
        lowerAscii(de.path().extension().string()) == ".asi") {
      asiMod = true;
      break;
    }
  }

  const fs::path staging =
      dataDir() / "_ml_tmp" / std::to_string(std::time(nullptr));
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  try {
    for (const auto& de : fs::recursive_directory_iterator(modFolder, ec)) {
      if (!de.is_regular_file(ec))
        continue;
      const fs::path rel = fs::relative(de.path(), modFolder, ec);
      if (ec || rel.empty())
        continue;
      if (rules::isJunkFile(rel.filename().string()))
        continue;
      std::string gameRel = rel.generic_string();
      if (!asiMod) {
        gameRel = modloaderGameRel(rel);
        // A bare loose file (no game folder in its path) that exists loose in the
        // vanilla game is placed where the game actually reads it (e.g.
        // "fonts.txd" -> "models/fonts.txd"). IMG-content names are left bare and
        // get injected into their archive by name at deploy time.
        if (gameRel.find('/') == std::string::npos) {
          const auto it = looseIndex.find(lowerAscii(gameRel));
          if (it != looseIndex.end())
            gameRel = it->second;
        }
      }
      const fs::path dest = staging / fs::path(gameRel);
      fs::create_directories(dest.parent_path(), ec);
      fs::copy_file(de.path(), dest, fs::copy_options::overwrite_existing, ec);
    }
    ImportResult r = importFromFolder(staging, name);
    fs::remove_all(staging, ec);
    return r;
  } catch (...) {
    fs::remove_all(staging, ec);
    throw;
  }
}

App::ImportResult App::importModloaderModVerbatim(const fs::path& modFolder,
                                                  const std::string& name)
{
  requireInit();
  // Adopt the mod's tree exactly as authored, then flag it for Mod Loader deploy.
  // Only touch a freshly-added mod: a dedup hit returns an existing pool mod
  // (possibly used natively elsewhere), whose deploy target we must not flip.
  ImportResult r = adoptMod(modFolder, name, modFolder.string(), "");
  if (!r.wasDuplicate && !r.mod.viaModloader) {
    r.mod.viaModloader = true;
    std::error_code ec;
    const fs::path meta = modsDir() / r.mod.id / "meta.json";
    if (fs::exists(meta, ec))
      writeJson(meta, Json(r.mod));
  }
  return r;
}

void App::setModViaModloader(const std::string& id, bool on)
{
  requireInit();
  const auto m = findMod(id);
  if (!m)
    throw std::runtime_error("no such mod in pool: " + id);
  if (m->viaModloader == on)
    return;
  Mod updated         = *m;
  updated.viaModloader = on;
  writeJson(modsDir() / m->id / "meta.json", Json(updated));
}

void App::setModDescription(const std::string& id, const std::string& text)
{
  requireInit();
  const auto m = findMod(id);
  if (!m)
    throw std::runtime_error("no such mod in pool: " + id);
  if (m->description == text)
    return;
  Mod updated        = *m;
  updated.description = text;
  writeJson(modsDir() / m->id / "meta.json", Json(updated));
}

std::vector<std::string> App::listModLuaScripts(const std::string& modId) const
{
  std::vector<std::string> out;
  const fs::path mlDir = modsDir() / modId / "root" / "moonloader";
  std::error_code ec;
  if (!fs::exists(mlDir, ec) || !fs::is_directory(mlDir, ec))
    return out;
  for (const auto& e : fs::directory_iterator(mlDir, ec)) {
    if (e.is_regular_file(ec) && lowerAscii(e.path().extension().string()) == ".lua")
      out.push_back("moonloader/" + e.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

App::SplitScriptsResult App::splitMoonloaderScripts(const std::string& modId)
{
  requireInit();
  SplitScriptsResult result;
  const auto modOpt = findMod(modId);
  if (!modOpt)
    throw std::runtime_error("no such mod in pool: " + modId);

  const fs::path root  = modsDir() / modOpt->id / "root";
  const fs::path mlDir = root / "moonloader";
  std::error_code ec;
  if (!fs::exists(mlDir, ec) || !fs::is_directory(mlDir, ec))
    return result;  // nothing to split

  // Every top-level .lua names a group; any other top-level entry that shares
  // its stem (a .ini/.cfg side file, or a same-named data subfolder) rides
  // along with it -- the same convention classifyBuildFile() uses for CLEO.
  std::set<std::string> luaStems;
  for (const auto& e : fs::directory_iterator(mlDir, ec))
    if (e.is_regular_file(ec) && lowerAscii(e.path().extension().string()) == ".lua")
      luaStems.insert(lowerAscii(e.path().stem().string()));
  if (luaStems.empty())
    return result;

  std::map<std::string, std::vector<fs::path>> groups;  // lowercased stem -> paths
  for (const auto& e : fs::directory_iterator(mlDir, ec)) {
    const std::string stem = lowerAscii(e.path().stem().string());
    if (luaStems.count(stem))
      groups[stem].push_back(e.path());
  }

  const fs::path staging =
      dataDir() / "_split_tmp" / std::to_string(std::time(nullptr));
  fs::remove_all(staging, ec);

  for (const auto& [stem, paths] : groups) {
    const fs::path grpDir = staging / stem / "moonloader";
    fs::create_directories(grpDir, ec);
    std::string displayName;
    for (const fs::path& p : paths) {
      const fs::path dest = grpDir / p.filename();
      if (fs::is_directory(p, ec))
        fs::copy(p, dest, fs::copy_options::recursive, ec);
      else
        fs::copy_file(p, dest, ec);
      if (lowerAscii(p.extension().string()) == ".lua")
        displayName = p.stem().string();
    }
    const ImportResult r =
        importFromFolder(staging / stem, displayName.empty() ? stem : displayName);
    result.createdModIds.push_back(r.mod.id);
    for (const fs::path& p : paths)
      fs::remove_all(p, ec);
  }
  fs::remove_all(staging, ec);

  // The original mod's content changed (its scripts moved out) -- refresh its
  // cached hash so future dedup checks against it stay correct.
  {
    Mod updated         = *modOpt;
    updated.contentHash = hashModTree(root);
    writeJson(modsDir() / modOpt->id / "meta.json", Json(updated));
  }

  // Wire the new script-mods into every profile that already had the original
  // mod: same enabled state, inserted right after it, so a profile that relied
  // on those scripts doesn't silently lose them on the next deploy.
  for (const std::string& pname : profileNames()) {
    Profile p = loadProfile(pname);
    auto it   = std::find_if(p.entries.begin(), p.entries.end(), [&](const ProfileEntry& e) {
      return e.modId == modOpt->id;
    });
    if (it == p.entries.end())
      continue;
    const bool enabled = it->enabled;
    std::vector<ProfileEntry> fresh;
    for (const std::string& newId : result.createdModIds) {
      ProfileEntry ne;
      ne.modId   = newId;
      ne.enabled = enabled;
      fresh.push_back(ne);
    }
    p.entries.insert(it + 1, fresh.begin(), fresh.end());
    for (std::size_t i = 0; i < p.entries.size(); ++i)
      p.entries[i].priority = static_cast<int>(p.entries.size() - 1 - i);
    saveProfile(p);
    ++result.profilesUpdated;
  }

  return result;
}

void App::writeModFile(const std::string& modId, const std::string& relPath,
                       const std::string& content)
{
  requireInit();
  const auto modOpt      = findMod(modId);
  if (!modOpt)
    throw std::runtime_error("no such mod in pool: " + modId);
  const fs::path target = modsDir() / modOpt->id / "root" / fs::path(relPath);
  std::error_code ec;
  if (!fs::exists(target, ec) || !fs::is_regular_file(target, ec))
    throw std::runtime_error("no such file in mod: " + relPath);

  // Truncate-in-place, not a temp+rename swap: if this mod is currently
  // deployed via hardlink, the edit is visible in the game folder right away
  // with no redeploy needed -- a rename would instead break the hardlink and
  // leave the deployed copy stale until the next Deploy.
  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot write: " + target.string());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();

  Mod updated         = *modOpt;
  updated.contentHash = hashModTree(modsDir() / modOpt->id / "root");
  writeJson(modsDir() / modOpt->id / "meta.json", Json(updated));
}

std::vector<App::ImportResult> App::importArchiveAsMods(const fs::path& archivePath,
                                                        const std::string& nameOpt,
                                                        bool viaModloader,
                                                        const std::string& password)
{
  requireInit();
  std::error_code ec;
  if (!fs::exists(archivePath, ec) || !fs::is_regular_file(archivePath, ec))
    throw std::runtime_error("archive not found: " + archivePath.string());

  const fs::path tmp =
      dataDir() / "_import_tmp" / std::to_string(std::time(nullptr));
  fs::create_directories(tmp);
  try {
    if (hasArchiveExtension(archivePath)) {
      extractArchive(archivePath, tmp, password);
      // See importFromArchive(): unwrap a wrapper-archive-around-the-real-archive
      // in place so the mod ends up as actual files.
      flattenNestedArchives(tmp, password);
    } else {
      // Not actually an archive -- see importFromArchive() for why a bare
      // loose file (dropped/passed directly, not wrapped in a folder or zip)
      // is staged alone instead of run through extractArchive().
      fs::copy_file(archivePath, tmp / archivePath.filename());
    }
    std::string name = nameOpt;
    if (name.empty())
      name = archivePath.stem().string();
    std::vector<ImportResult> r = importFolderAsMods(tmp, name, viaModloader);
    fs::remove_all(tmp, ec);
    return r;
  } catch (...) {
    fs::remove_all(tmp, ec);
    throw;
  }
}

void App::removeMod(const std::string& id)
{
  requireInit();
  // Resolve to the canonical pool id (the filesystem is case-insensitive on
  // Windows, but profile entries store the exact id).
  const auto mod = findMod(id);
  if (!mod)
    throw std::runtime_error("no such mod in pool: " + id);
  const std::string cid = mod->id;
  fs::remove_all(modsDir() / cid);

  // Drop the mod from every profile that references it.
  for (const std::string& pname : profileNames()) {
    Profile p     = loadProfile(pname);
    const auto it = std::remove_if(p.entries.begin(), p.entries.end(),
                                   [&](const ProfileEntry& e) { return e.modId == cid; });
    if (it != p.entries.end()) {
      p.entries.erase(it, p.entries.end());
      saveProfile(p);
    }
  }
}

// --- import an existing modded build ----------------------------------------

namespace {

std::string lowerStr(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string stemLower(const std::string& filename)
{
  const auto dot = filename.find_last_of('.');
  return lowerStr(dot == std::string::npos ? filename : filename.substr(0, dot));
}

// Reject paths that escape the build dir (absolute or containing "..").
bool isSafeBuildRel(const fs::path& rel)
{
  if (rel.is_absolute())
    return false;
  for (const auto& part : rel) {
    const std::string s = part.string();
    if (s == ".." || s.empty())
      return false;
  }
  return true;
}

}  // namespace

bool App::hasBaseline() const
{
  std::error_code ec;
  return fs::exists(baselinePath(), ec);
}

void App::refreshVanillaBaseline()
{
  requireInit();
  if (m_gamePath.empty())
    throw std::runtime_error("instance has no game folder set");
  if (isDeployed())
    throw std::runtime_error(
        "cannot build the vanilla baseline while a profile is deployed; "
        "run rollback first, then retry");
  const Baseline base = buildBaseline(requireGameDir());
  saveBaseline(baselinePath(), base);
}

App::ImportBuildResult App::importBuild(const fs::path& buildDir,
                                        const ImportBuildOptions& opts)
{
  requireInit();
  std::error_code ec;
  if (m_gamePath.empty())
    throw std::runtime_error("instance has no game folder set");
  if (!fs::is_directory(buildDir, ec))
    throw std::runtime_error("build folder not found: " + buildDir.string());

  std::string profName = opts.profileName;
  if (profName.empty())
    profName = buildDir.filename().string();
  if (profName.empty())
    profName = "Imported build";
  if (!isValidProfileName(profName))
    throw std::runtime_error("invalid profile name: '" + profName + "'");
  if (fs::exists(profilePath(profName)))
    throw std::runtime_error("profile already exists: " + profName);

  ImportBuildResult result;
  result.profile = profName;

  // --- vanilla baseline (cached per instance) ---
  if (opts.refreshBaseline)
    fs::remove(baselinePath(), ec);
  Baseline base;
  if (hasBaseline()) {
    base = loadBaseline(baselinePath());
  } else {
    if (isDeployed())
      throw std::runtime_error(
          "cannot build the vanilla baseline while a profile is deployed; "
          "run `rollback` first, then retry");
    result.notes.push_back("built vanilla baseline from the game folder");
    base = buildBaseline(requireGameDir());
    saveBaseline(baselinePath(), base);
  }

  const std::string buildName = profName;

  // IMG filenames the baseline actually knows about (i.e. real vanilla
  // streaming archives: gta3.img, gta_int.img, player.img, cutscene.img).
  // Anything else with a ".img" extension -- e.g. SA-MP's own SAMP/SAMP.img,
  // SAMP/custom.img, SAMP/SAMPCOL.img, which happen to ALSO be valid VER2 IMG
  // archives since SA-MP repurposes the GTA IMG format for its shared
  // custom-object library -- has no vanilla counterpart to diff against or
  // rebuild from a pristine cache, so it must be treated as an opaque loose
  // file (copied whole), not diffed entry-by-entry like a real game archive.
  std::set<std::string> vanillaImgNames;
  for (const auto& [key, entry] : base.img) {
    const auto sep = key.find("::");
    if (sep != std::string::npos)
      vanillaImgNames.insert(key.substr(0, sep));
  }

  // Mod Loader mods are self-contained, named overlays -- import each wholesale
  // (including nested category folders like modloader/HD/<mod>) and exclude their
  // files from the diff below so they don't also land in the lumped "main" mod.
  std::vector<std::pair<std::string, fs::path>> mlMods;
  const fs::path mlDir = findDirNamed(buildDir, "modloader", 2);
  if (!mlDir.empty())
    listModloaderMods(mlDir, mlMods);

  // --- 1. diff the build against the baseline ---
  struct LooseChange
  {
    fs::path abs;
    std::string gameRel;
  };
  struct ImgChange
  {
    std::string entryName;
    std::vector<unsigned char> bytes;
  };
  std::vector<LooseChange> looseChanges;
  std::vector<ImgChange> imgChanges;
  bool sawNewImgEntry = false;

  for (const auto& de : fs::recursive_directory_iterator(buildDir, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), buildDir, ec);
    if (ec || !isSafeBuildRel(rel))
      continue;
    // Mod Loader content is imported wholesale above, not diffed.
    if (rel.begin() != rel.end() && iequals(rel.begin()->string(), "modloader"))
      continue;
    const std::string filename = rel.filename().string();
    const std::string ext      = lowerStr(de.path().extension().string());

    // Documentation and user data are never toggleable mod content. Unlike a
    // downloaded mod package (where rules::isJunkFile()'s readme/screenshot
    // heuristic is right -- e.g. a "Screenshots/" folder bundled just for
    // browsing), this is a diff of a live, already-INSTALLED game folder: there
    // is no "preview image for humans" scenario here, so an image file that
    // differs from vanilla is virtually always a real asset, not decoration --
    // e.g. SA-MP's own mouse.png/sampgui.png cursor/GUI textures used to get
    // silently dropped here as "junk", breaking SA-MP's own first-run check.
    // Keep only the genuinely non-functional document extensions.
    const bool isDoc = ext == ".txt" || ext == ".nfo" || ext == ".url" || ext == ".pdf" ||
                      ext == ".doc" || ext == ".docx" || ext == ".rtf" ||
                      ext == ".html" || ext == ".htm" || ext == ".md";
    if (isDoc || ext == ".set" || ext == ".b") {
      ++result.skipped;
      result.skippedFiles.push_back({rel.generic_string(), "junk/readme/user-data file"});
      continue;
    }
    // Runtime-written debris from loaders/scripts (MoonLoader/CLEO/mod_sa logs,
    // NSIS installer scratch dir left behind by a modpack installer) is never
    // something a mod ships on purpose -- if it's baked into a diffed group it
    // gets redeployed on every Deploy forever after, since "keep the game folder
    // clean" only manages files a session creates *after* Deploy, not files
    // already part of a mod's own tree. Unlike the doc/screenshot heuristic
    // above (which only applies to build diffs, not regular mod imports), a log
    // file is unambiguous in both contexts, so this check is safe everywhere.
    const bool isRuntimeDebris =
        ext == ".log" ||
        (rel.begin() != rel.end() && iequals(rel.begin()->string(), "$PLUGINSDIR"));
    if (isRuntimeDebris) {
      ++result.skipped;
      result.skippedFiles.push_back({rel.generic_string(), "runtime-generated log/installer-temp file"});
      continue;
    }
    bool userData = false;
    for (const auto& part : rel)
      if (lowerStr(part.string()).find("user files") != std::string::npos)
        userData = true;
    if (userData) {
      ++result.skipped;
      result.skippedFiles.push_back({rel.generic_string(), "under a \"User Files\" folder"});
      continue;
    }

    // A known VANILLA archive diffs at the entry level (never copied whole).
    // A .img the baseline has never heard of (SA-MP's own SAMP.img/custom.img/
    // SAMPCOL.img, etc.) has nothing to diff against or rebuild from a
    // pristine cache -- it falls through to the plain-loose-file path below,
    // which copies it whole and preserves it as an actual container file.
    const std::string imgKeyMaybe = lowerStr(filename);
    if (ext == ".img" && vanillaImgNames.count(imgKeyMaybe) > 0 && imgIsVer2(de.path())) {
      const std::string imgKey = imgKeyMaybe;
      for (const auto& entry : imgReadDirectory(de.path())) {
        const std::string ek = imgKey + "::" + lowerStr(entry.name);
        auto it              = base.img.find(ek);
        const bool isNew     = it == base.img.end();
        bool changed         = isNew || it->second.size != entry.size;
        std::vector<unsigned char> bytes;
        if (!changed) {
          bytes   = imgReadEntry(de.path(), entry);
          changed = hashBytesFnv(bytes.data(), bytes.size()) != it->second.hash;
        }
        if (!changed)
          continue;
        if (bytes.empty())
          bytes = imgReadEntry(de.path(), entry);
        if (isNew)
          sawNewImgEntry = true;
        imgChanges.push_back({entry.name, std::move(bytes)});
        ++result.imgChanged;
      }
      continue;
    }

    // Loose file: changed if absent, or size/hash differs from vanilla.
    const std::string key = lowerStr(rel.generic_string());
    auto it               = base.loose.find(key);
    bool changed          = it == base.loose.end();
    if (!changed) {
      const std::uint64_t sz = fs::file_size(de.path(), ec);
      changed = ec || sz != it->second.size ||
                hashFileFnv(de.path()) != it->second.hash;
    }
    if (!changed) {
      ++result.skipped;
      result.skippedFiles.push_back(
          {rel.generic_string(), "unchanged (matches the vanilla baseline)"});
      continue;
    }
    looseChanges.push_back({de.path(), rel.generic_string()});
    ++result.looseChanged;
  }

  if (looseChanges.empty() && imgChanges.empty() && mlMods.empty())
    throw std::runtime_error(
        "nothing to import: the build matches vanilla (or is not GTA SA 1.0)");

  // --- 2. learn the ASI/CLEO/MoonLoader stems so side files group with their
  // plugin/script ---
  std::set<std::string> asiStems, cleoStems, moonloaderStems;
  for (const LooseChange& lc : looseChanges) {
    const fs::path p          = lc.gameRel;
    const std::string fname   = p.filename().string();
    const std::string ext     = lowerStr(p.extension().string());
    const std::string firstLc = lowerStr(p.begin()->string());
    if (ext == ".asi")
      asiStems.insert(stemLower(fname));
    else if (firstLc == "cleo" &&
             (ext == ".cs" || ext == ".cs3" || ext == ".cs4" ||
              ext == ".cs5" || ext == ".cleo"))
      cleoStems.insert(stemLower(fname));
    else if (firstLc == "moonloader" && ext == ".lua" &&
             std::distance(p.begin(), p.end()) == 2)
      moonloaderStems.insert(stemLower(fname));
  }

  // --- 3. group changed files and stage each group as a game-relative tree ---
  struct Group
  {
    std::string name;
    fs::path dir;
    bool nameLocked = false;
  };
  std::map<std::string, Group> groups;  // key -> staged group
  std::vector<std::string> order;       // group keys, first-seen order

  const fs::path staging =
      dataDir() / "_build_tmp" / std::to_string(std::time(nullptr));
  fs::remove_all(staging, ec);
  fs::create_directories(staging);

  auto groupDirFor = [&](const std::string& key, const std::string& defaultName,
                         bool primary, const std::string& nameHint) -> Group& {
    auto it = groups.find(key);
    if (it == groups.end()) {
      Group g;
      g.name = defaultName;
      g.dir  = staging / sanitizeId(key);
      fs::create_directories(g.dir, ec);
      it = groups.emplace(key, std::move(g)).first;
      order.push_back(key);
    }
    if (primary && !it->second.nameLocked) {
      it->second.name       = nameHint;
      it->second.nameLocked = true;
    }
    return it->second;
  };

  // Files that classify to a group key ALREADY claimed by an earlier file
  // (e.g. "MyScript.cs" and "MyScript.cs3" both stem to "cleo:myscript") land
  // in the SAME mod rather than getting their own -- correct (they're the
  // same script in different formats), but easy to mistake for a dropped
  // file, so it's called out by name below.
  std::vector<std::string> mergedFiles;

  try {
    for (const LooseChange& lc : looseChanges) {
      const BuildGroup g =
          classifyBuildFile(lc.gameRel, asiStems, cleoStems, moonloaderStems, buildName);
      const bool groupExisted = groups.count(g.key) != 0;
      Group& grp = groupDirFor(g.key, g.name, g.primary, g.name);
      if (g.primary && groupExisted)
        mergedFiles.push_back(lc.gameRel + " -> \"" + grp.name + "\"");
      const fs::path dest = grp.dir / fs::path(g.innerRel);
      fs::create_directories(dest.parent_path(), ec);
      fs::copy_file(lc.abs, dest, fs::copy_options::overwrite_existing, ec);
      if (ec)
        throw std::runtime_error("cannot stage '" + g.innerRel +
                                 "': " + ec.message());
    }
    // Changed IMG entries go into the lumped build mod as loose model files;
    // deploy() re-injects them into the matching archive by name.
    for (const ImgChange& ic : imgChanges) {
      Group& grp = groupDirFor("main", buildName, false, buildName);
      const fs::path dest = grp.dir / ic.entryName;
      std::ofstream out(dest, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(ic.bytes.data()),
                static_cast<std::streamsize>(ic.bytes.size()));
    }

    // --- 4. import each group into the pool and wire up a new profile ---
    createProfile(profName);
    Profile profile = loadProfile(profName);

    // Structural mods first (higher priority), the lumped remainder last.
    std::vector<std::string> ordered;
    for (const std::string& k : order)
      if (k != "main")
        ordered.push_back(k);
    for (const std::string& k : order)
      if (k == "main")
        ordered.push_back(k);

    std::set<std::string> placed;
    std::map<std::string, std::string> keyToModId;  // group key -> imported mod id
    int prio = static_cast<int>(mlMods.size() + ordered.size());
    auto addEntry = [&](const ImportResult& r, const std::string& groupKey) {
      if (!groupKey.empty())
        keyToModId[groupKey] = r.mod.id;
      if (!placed.insert(r.mod.id).second)
        return;  // identical content already added under another group
      result.createdModIds.push_back(r.mod.id);
      ProfileEntry e;
      e.modId    = r.mod.id;
      e.enabled  = true;
      e.priority = prio--;
      profile.entries.push_back(e);
    };

    // Mod Loader mods (each imported as its own mod) sit on top. They keep
    // running under Mod Loader: adopted verbatim and flagged so deploy() stages
    // them back under modloader/<name>/ exactly as authored.
    std::sort(mlMods.begin(), mlMods.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [name, path] : mlMods)
      addEntry(importModloaderModVerbatim(path, name), "");

    for (const std::string& key : ordered)
      addEntry(importFromFolder(groups[key].dir, groups[key].name), key);

    // The build may have brought its own modloader.asi along as a plain ASI
    // mod (classifyBuildFile groups any *.asi by stem: "asi:modloader"). If
    // Mod Loader auto-install is on, GMM installs its own vetted runtime
    // (matched modloader.asi + std.*.dll + CLEO/essentials, see
    // deployModloaderRuntime()) on every deploy anyway -- and deploying BOTH
    // risks two different Mod Loader builds' files landing in the same game
    // folder. Disable the build's own copy in the profile so only one is
    // ever active; the user can still flip it back on in the mod list.
    if (m_enableModloader) {
      auto it = keyToModId.find("asi:modloader");
      if (it != keyToModId.end()) {
        for (auto& e : profile.entries)
          if (e.modId == it->second) {
            e.enabled = false;
            result.notes.push_back(
                "disabled the build's own \"" + groups["asi:modloader"].name +
                "\" mod (its modloader.asi) because Mod Loader auto-install is "
                "on in Settings -- GMM's own runtime will be used instead; "
                "re-enable it in the mod list to use the build's copy");
            break;
          }
      }
    }

    saveProfile(profile);
    m_activeProfile = profName;
    saveConfig();
  } catch (...) {
    fs::remove_all(staging, ec);
    throw;
  }
  fs::remove_all(staging, ec);

  if (!mlMods.empty())
    result.notes.push_back("split " + std::to_string(mlMods.size()) +
                           " Mod Loader mod(s) into separate mods (deploying via "
                           "Mod Loader)");
  if (sawNewImgEntry)
    result.notes.push_back(
        "some IMG entries are new (not in vanilla); they will deploy as loose "
        "files unless an archive already holds that name");
  if (!mergedFiles.empty()) {
    result.notes.push_back(
        std::to_string(mergedFiles.size()) +
        " file(s) share a name with another changed file (e.g. a script's "
        ".cs + .cs3 variants) and were merged into that one mod instead of "
        "getting their own:");
    for (const auto& m : mergedFiles)
      result.notes.push_back("  " + m);
  }
  return result;
}

// --- export / import a build as a portable zip ------------------------------

void App::exportBuild(const std::string& profileName, const fs::path& zipPath,
                      bool withSaves, bool withSettings)
{
  requireInit();
  const Profile prof = loadProfile(profileName);  // throws if missing

  const fs::path staging =
      dataDir() / "_export_tmp" / std::to_string(std::time(nullptr));
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  try {
    // 1. The profile (order/priority/separators/SA-MP).
    writeJson(staging / "profile.json", Json(prof));

    // 2. Each ENABLED referenced mod, once, with metadata + files. Disabled
    // entries are left out of the archive entirely (not just their files --
    // their mod folder never gets staged), so a build exported from a
    // heavily-experimented-on profile only carries what's actually active,
    // not every mod ever toggled on at some point. importBuildArchive()
    // already drops profile entries whose modId has no matching "mods/<id>"
    // folder in the zip, so a disabled entry here simply doesn't round-trip
    // on import -- no separate filtering of profile.json is needed.
    std::set<std::string> seen;
    Json modList = Json::array();
    for (const ProfileEntry& e : prof.entries) {
      if (e.separator || e.modId.empty() || !e.enabled)
        continue;
      if (!seen.insert(e.modId).second)
        continue;
      const auto m = findMod(e.modId);
      if (!m)
        continue;  // referenced mod no longer in the pool; skip silently
      const fs::path srcRoot = modsDir() / m->id / "root";
      if (!fs::exists(srcRoot, ec))
        continue;
      const fs::path dstMod = staging / "mods" / m->id;
      fs::create_directories(dstMod, ec);
      writeJson(dstMod / "meta.json", Json(*m));
      fs::copy(srcRoot, dstMod / "root",
               fs::copy_options::recursive | fs::copy_options::overwrite_existing,
               ec);
      modList.push_back(Json{{"id", m->id}, {"name", m->name}});
    }

    // 3. Build-info notes (Markdown + images), if any.
    bool hasInfo = false;
    if (fs::exists(profileInfoDir(profileName), ec)) {
      fs::copy(profileInfoDir(profileName), staging / "info",
               fs::copy_options::recursive | fs::copy_options::overwrite_existing,
               ec);
      hasInfo = true;
    }

    // 4. Optionally bundle savegames (*.b) and/or settings (gta_sa.set / *.set).
    // Source: the profile's per-profile saves store first (if it has any), then
    // the live "GTA San Andreas User Files" folder -- so saves export even when
    // per-profile saves is off and the store is empty (the common case). Media
    // (User Tracks/Gallery) is never exported.
    int savesCount = 0;
    if (withSaves || withSettings) {
      const auto lc = [](std::string v) {
        for (char& c : v)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
      };
      std::set<std::string> taken;  // relative paths already staged (store wins)
      auto gather = [&](const fs::path& s) {
        std::error_code e2;
        if (s.empty() || !fs::exists(s, e2))
          return;
        for (const auto& de : fs::recursive_directory_iterator(s, e2)) {
          if (!de.is_regular_file(e2))
            continue;
          const std::string nm  = lc(de.path().filename().string());
          const std::string ext = lc(de.path().extension().string());
          const bool isSave     = ext == ".b";
          const bool isSettings = ext == ".set" || nm == "gta_sa.set";
          if (!((withSaves && isSave) || (withSettings && isSettings)))
            continue;
          const fs::path rel    = fs::relative(de.path(), s, e2);
          const std::string key = lc(rel.generic_string());
          if (!taken.insert(key).second)
            continue;  // already taken from the profile store
          const fs::path out = staging / "saves" / rel;
          fs::create_directories(out.parent_path(), e2);
          fs::copy_file(de.path(), out, fs::copy_options::overwrite_existing, e2);
          ++savesCount;
        }
      };
      gather(profileSavesDir(profileName));    // the profile's own store first
      gather(userfiles::gtaUserFilesDir());    // then the live User Files folder
    }

    // 5. Manifest. "settings" carries the INSTANCE-wide deploy toggles (Mod
    // Loader on/off, bundled exe swap, per-profile saves/clean-folder, map
    // auto-install) alongside the mods/profile -- without them a build that
    // relies on Mod Loader (viaModloader mods) could silently fail to work on a
    // fresh instance where the global Mod Loader switch defaults off.
    const Json settings = Json{{"autoRouteMaps", m_autoRouteMaps},
                               {"manageSaves", m_manageSaves},
                               {"manageGenerated", m_manageGenerated},
                               {"enableModloader", m_enableModloader},
                               {"enableEssentials", m_enableEssentials},
                               {"replaceGameExe", m_replaceGameExe}};
    const Json manifest = Json{{"format", "gmm-build"},
                               {"version", 1},
                               {"profile", profileName},
                               {"exportedAt",
                                static_cast<std::int64_t>(std::time(nullptr))},
                               {"mods", modList},
                               {"hasInfo", hasInfo},
                               {"hasSaves", savesCount > 0},
                               {"settings", settings}};
    writeJson(staging / "build.json", manifest);

    createZip(staging, zipPath);
    fs::remove_all(staging, ec);
  } catch (...) {
    fs::remove_all(staging, ec);
    throw;
  }
}

void App::exportMod(const std::string& modId, const fs::path& zipPath)
{
  requireInit();
  const auto m = findMod(modId);
  if (!m)
    throw std::runtime_error("no such mod: " + modId);
  const fs::path srcRoot = modsDir() / m->id / "root";
  std::error_code ec;
  if (!fs::exists(srcRoot, ec))
    throw std::runtime_error("mod has no files: " + modId);

  const fs::path staging =
      dataDir() / "_export_tmp" / std::to_string(std::time(nullptr));
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  try {
    const Json manifest =
        Json{{"format", "gmm-mod"}, {"version", 1}, {"mod", Json(*m)}};
    writeJson(staging / "mod.json", manifest);
    fs::copy(srcRoot, staging / "root",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    createZip(staging, zipPath);
    fs::remove_all(staging, ec);
  } catch (...) {
    fs::remove_all(staging, ec);
    throw;
  }
}

App::ImportBuildArchiveResult App::importBuildArchive(const fs::path& zipPath,
                                                      const std::string& profileName)
{
  requireInit();
  if (!isValidProfileName(profileName))
    throw std::runtime_error("invalid profile name: '" + profileName + "'");
  std::error_code ec;
  if (!fs::exists(zipPath, ec) || !fs::is_regular_file(zipPath, ec))
    throw std::runtime_error("archive not found: " + zipPath.string());

  const fs::path tmp =
      dataDir() / "_import_tmp" / std::to_string(std::time(nullptr));
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  ImportBuildArchiveResult result;
  result.profile = profileName;
  try {
    extractArchive(zipPath, tmp);

    if (!fs::exists(tmp / "build.json", ec) || !fs::exists(tmp / "profile.json", ec))
      throw std::runtime_error(
          "not a GMM build archive (missing build.json/profile.json)");
    const Json manifest = readJson(tmp / "build.json");
    if (manifest.value("format", std::string{}) != "gmm-build")
      throw std::runtime_error("unrecognized build archive format");

    const Profile exported = readJson(tmp / "profile.json").get<Profile>();

    // Import each mod folder (dedup by content), mapping old id -> new pool id.
    std::map<std::string, std::string> idMap;
    const fs::path modsRoot = tmp / "mods";
    if (fs::exists(modsRoot, ec)) {
      for (const auto& de : fs::directory_iterator(modsRoot, ec)) {
        if (!de.is_directory(ec))
          continue;
        const std::string oldId = de.path().filename().string();
        const fs::path root     = de.path() / "root";
        if (!fs::exists(root, ec))
          continue;
        std::string name = oldId, hash;
        bool wantModloader = false;
        const fs::path meta = de.path() / "meta.json";
        if (fs::exists(meta, ec)) {
          const Mod mm  = readJson(meta).get<Mod>();
          name          = mm.name.empty() ? oldId : mm.name;
          hash          = mm.contentHash;
          wantModloader = mm.viaModloader;
        }
        ImportResult ir =
            adoptMod(root, name, "build:" + zipPath.filename().string(), hash);
        // Preserve the exported deploy target (native vs. Mod Loader) for a
        // freshly-added mod. A dedup hit reuses an existing pool mod whose flag
        // may already differ deliberately, so it is left untouched.
        if (!ir.wasDuplicate && ir.mod.viaModloader != wantModloader) {
          setModViaModloader(ir.mod.id, wantModloader);
          ir.mod.viaModloader = wantModloader;
        }
        idMap[oldId] = ir.mod.id;
        if (ir.wasDuplicate)
          ++result.modsReused;
        else
          ++result.modsAdded;
      }
    }

    // Rebuild the profile with remapped ids; keep separators and SA-MP settings.
    Profile p;
    p.name = profileName;
    p.samp = exported.samp;
    for (const ProfileEntry& e : exported.entries) {
      if (e.separator) {
        p.entries.push_back(e);
        continue;
      }
      const auto it = idMap.find(e.modId);
      if (it == idMap.end()) {
        ++result.entriesDropped;
        continue;
      }
      ProfileEntry ne = e;
      ne.modId        = it->second;
      p.entries.push_back(ne);
    }
    saveProfile(p);  // creates or overwrites

    // Restore build-info notes.
    if (fs::exists(tmp / "info", ec)) {
      fs::remove_all(profileInfoDir(profileName), ec);
      fs::copy(tmp / "info", profileInfoDir(profileName),
               fs::copy_options::recursive | fs::copy_options::overwrite_existing,
               ec);
    }

    // Restore bundled savegames/settings. They go BOTH into the target profile's
    // per-profile saves store (so they travel with the profile / work with
    // per-profile saves) AND into the live "GTA San Andreas User Files" folder
    // (so they apply in-game immediately, even when per-profile saves is off).
    // Merge only: same-named files are overwritten, everything else is kept.
    if (fs::exists(tmp / "saves", ec)) {
      int restored = 0;
      auto restoreInto = [&](const fs::path& dst) {
        std::error_code e2;
        if (dst.empty())
          return;
        for (const auto& de : fs::recursive_directory_iterator(tmp / "saves", e2)) {
          if (!de.is_regular_file(e2))
            continue;
          const fs::path rel = fs::relative(de.path(), tmp / "saves", e2);
          const fs::path out = dst / rel;
          fs::create_directories(out.parent_path(), e2);
          fs::copy_file(de.path(), out, fs::copy_options::overwrite_existing, e2);
        }
      };
      for (const auto& de : fs::recursive_directory_iterator(tmp / "saves", ec))
        if (de.is_regular_file(ec))
          ++restored;
      restoreInto(profileSavesDir(profileName));   // travels with the profile
      restoreInto(userfiles::gtaUserFilesDir());   // usable in-game right away
      if (restored > 0)
        result.notes.push_back("restored " + std::to_string(restored) +
                               " save/settings file(s)");
    }

    // Apply the build's instance-wide deploy settings (Mod Loader on/off,
    // bundled exe swap, per-profile saves/clean-folder, map auto-install), so a
    // build that relies on Mod Loader mods actually works out of the box on a
    // fresh instance. Older build archives (exported before this existed) have
    // no "settings" key, so this leaves the current instance untouched.
    if (manifest.contains("settings")) {
      const Json& s = manifest.at("settings");
      setAutoRouteMaps(s.value("autoRouteMaps", m_autoRouteMaps));
      setManageSaves(s.value("manageSaves", m_manageSaves));
      setManageGenerated(s.value("manageGenerated", m_manageGenerated));
      setEnableModloader(s.value("enableModloader", m_enableModloader));
      setEnableEssentials(s.value("enableEssentials", m_enableEssentials));
      setReplaceGameExe(s.value("replaceGameExe", m_replaceGameExe));
      result.notes.push_back(
          "applied the build's deploy settings (Mod Loader/clean folder/saves/"
          "map auto-install) to this instance");
    }

    if (result.entriesDropped > 0)
      result.notes.push_back(std::to_string(result.entriesDropped) +
                             " entr(y/ies) had no matching mod and were skipped");

    // Make the imported build the active profile.
    m_activeProfile = profileName;
    saveConfig();

    fs::remove_all(tmp, ec);
  } catch (...) {
    fs::remove_all(tmp, ec);
    throw;
  }
  return result;
}

bool App::isBuildArchive(const fs::path& zipPath) const
{
  std::error_code ec;
  if (!fs::exists(zipPath, ec) || !fs::is_regular_file(zipPath, ec))
    return false;

  const fs::path tmp =
      dataDir() / "_import_tmp" / (std::to_string(std::time(nullptr)) + "_peek");
  fs::remove_all(tmp, ec);
  bool result = false;
  try {
    extractArchive(zipPath, tmp);
    if (fs::exists(tmp / "build.json", ec))
      result = readJson(tmp / "build.json").value("format", std::string{}) == "gmm-build";
  } catch (...) {
    result = false;  // not a zip, corrupt, or some other archive format entirely
  }
  fs::remove_all(tmp, ec);
  return result;
}

// --- profiles ---------------------------------------------------------------

fs::path App::profilePath(const std::string& name) const
{
  return profilesDir() / (name + ".json");
}

std::vector<std::string> App::profileNames() const
{
  requireInit();
  std::vector<std::string> names;
  std::error_code ec;
  if (!fs::exists(profilesDir(), ec))
    return names;
  for (const auto& entry : fs::directory_iterator(profilesDir())) {
    if (entry.is_regular_file() && entry.path().extension() == ".json")
      names.push_back(entry.path().stem().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

void App::createProfile(const std::string& name)
{
  requireInit();
  if (!isValidProfileName(name))
    throw std::runtime_error("invalid profile name: '" + name + "'");
  if (fs::exists(profilePath(name)))
    throw std::runtime_error("profile already exists: " + name);
  Profile p;
  p.name = name;
  saveProfile(p);
  // First profile becomes the active one for convenience.
  if (m_activeProfile.empty()) {
    m_activeProfile = name;
    saveConfig();
  }
}

void App::deleteProfile(const std::string& name)
{
  requireInit();
  if (!fs::exists(profilePath(name)))
    throw std::runtime_error("no such profile: " + name);
  fs::remove(profilePath(name));
  // Drop the profile's saves store, generated store and build-info folder too.
  std::error_code ec;
  fs::remove_all(profileSavesDir(name), ec);
  fs::remove_all(generatedDir(name), ec);
  fs::remove_all(profileInfoDir(name), ec);
  if (m_activeProfile == name) {
    m_activeProfile = "";
    saveConfig();
  }
}

void App::renameProfile(const std::string& oldName, const std::string& newName)
{
  requireInit();
  if (!isValidProfileName(newName))
    throw std::runtime_error("invalid profile name: '" + newName + "'");
  if (!fs::exists(profilePath(oldName)))
    throw std::runtime_error("no such profile: " + oldName);
  if (oldName == newName)
    return;

  // Lowercased compare: Windows' filesystem is case-insensitive, so a case-only
  // rename ("Build" -> "build") maps to the same file and must be allowed.
  auto lower = [](std::string s) {
    for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const bool caseOnly = lower(oldName) == lower(newName);
  if (!caseOnly && fs::exists(profilePath(newName)))
    throw std::runtime_error("profile already exists: " + newName);

  Profile p = loadProfile(oldName);
  p.name    = newName;
  saveProfile(p);  // writes the new file (same file on a case-only rename)

  std::error_code ec;
  if (!caseOnly) {
    fs::remove(profilePath(oldName), ec);
    // Carry the profile's saves store, generated store and build-info folder.
    if (fs::exists(profileSavesDir(oldName), ec))
      fs::rename(profileSavesDir(oldName), profileSavesDir(newName), ec);
    if (fs::exists(generatedDir(oldName), ec))
      fs::rename(generatedDir(oldName), generatedDir(newName), ec);
    if (fs::exists(profileInfoDir(oldName), ec))
      fs::rename(profileInfoDir(oldName), profileInfoDir(newName), ec);
  }

  if (m_activeProfile == oldName) {
    m_activeProfile = newName;
    saveConfig();
  }

  // Keep the deployed-state label in sync if the renamed profile is the one
  // currently deployed into the game folder.
  if (fs::exists(manifestPath(), ec)) {
    Manifest man = readJson(manifestPath()).get<Manifest>();
    if (man.profile == oldName) {
      man.profile = newName;
      writeJson(manifestPath(), Json(man));
    }
  }
}

void App::useProfile(const std::string& name)
{
  requireInit();
  if (!fs::exists(profilePath(name)))
    throw std::runtime_error("no such profile: " + name);
  m_activeProfile = name;
  saveConfig();
}

Profile App::loadProfile(const std::string& name) const
{
  requireInit();
  if (!fs::exists(profilePath(name)))
    throw std::runtime_error("no such profile: " + name);
  return readJson(profilePath(name)).get<Profile>();
}

SampConfig App::sampConfig(const std::string& profileName) const
{
  return loadProfile(profileName).samp;
}

void App::setSampConfig(const std::string& profileName, const SampConfig& cfg)
{
  Profile p = loadProfile(profileName);
  p.samp    = cfg;
  saveProfile(p);
}

bool App::relativePathWillExist(const std::string& relPath) const
{
  if (relPath.empty())
    return false;
  const fs::path rel = fs::path(relPath);
  std::error_code ec;
  if (rel.is_absolute())
    return fs::exists(rel, ec);

  if (!m_gamePath.empty() && fs::exists(fs::path(m_gamePath) / rel, ec))
    return true;  // already deployed / a vanilla file

  // Not there yet -- but would deploying the active profile right now put it
  // there? (e.g. samp.exe bundled inside a mod that hasn't been deployed yet.)
  const std::string key = lowerAscii(rel.generic_string());
  const ActivePlan plan = computeActive();
  return plan.loose.count(key) > 0 || plan.modloader.count(key) > 0;
}

std::vector<std::string> App::pendingRootExecutables() const
{
  std::vector<std::string> names;
  const ActivePlan plan = computeActive();
  for (const auto& [key, merged] : plan.loose) {
    const fs::path rel(merged.winnerRel);
    if (!rel.parent_path().empty())
      continue;  // only files landing directly at the game root
    if (lowerAscii(rel.extension().string()) == ".exe")
      names.push_back(rel.filename().string());
  }
  return names;
}

void App::copyProfile(const std::string& src, const std::string& dst,
                      bool withSaves, bool withSettings)
{
  requireInit();
  if (!isValidProfileName(dst))
    throw std::runtime_error("invalid profile name: '" + dst + "'");
  if (!fs::exists(profilePath(src)))
    throw std::runtime_error("no such profile: " + src);
  if (fs::exists(profilePath(dst)))
    throw std::runtime_error("profile already exists: " + dst);

  // 1. The profile itself (mods + order + separators).
  Profile p = loadProfile(src);
  p.name    = dst;
  saveProfile(p);

  std::error_code ec;
  // 2. Build-info notes travel with the copy.
  if (fs::exists(profileInfoDir(src), ec))
    fs::copy(profileInfoDir(src), profileInfoDir(dst),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);

  // 3. Optionally copy savegames (*.b) and/or settings (gta_sa.set / *.set) from
  // the source profile's saves store.
  if ((withSaves || withSettings) && fs::exists(profileSavesDir(src), ec)) {
    const fs::path s = profileSavesDir(src);
    const fs::path d = profileSavesDir(dst);
    const auto lc = [](std::string v) {
      for (char& c : v)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return v;
    };
    for (const auto& de : fs::recursive_directory_iterator(s, ec)) {
      if (!de.is_regular_file(ec))
        continue;
      const std::string name = lc(de.path().filename().string());
      const std::string ext  = lc(de.path().extension().string());
      const bool isSave     = ext == ".b";
      const bool isSettings = ext == ".set" || name == "gta_sa.set";
      if (!((withSaves && isSave) || (withSettings && isSettings)))
        continue;
      const fs::path rel = fs::relative(de.path(), s, ec);
      const fs::path out = d / rel;
      fs::create_directories(out.parent_path(), ec);
      fs::copy_file(de.path(), out, fs::copy_options::overwrite_existing, ec);
    }
  }
}

std::string App::loadProfileInfo(const std::string& name) const
{
  requireInit();
  std::error_code ec;
  const fs::path f = profileInfoFile(name);
  if (!fs::exists(f, ec))
    return "";
  std::ifstream in(f, std::ios::binary);
  if (!in)
    return "";
  std::string out((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  return out;
}

void App::saveProfileInfo(const std::string& name, const std::string& markdown) const
{
  requireInit();
  std::error_code ec;
  fs::create_directories(profileInfoDir(name), ec);
  const fs::path f   = profileInfoFile(name);
  const fs::path tmp = fs::path(f).concat(".tmp");
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o)
      throw std::runtime_error("cannot write profile info: " + f.string());
    o.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
  }
  fs::rename(tmp, f, ec);
  if (ec) {  // cross-something fallback
    fs::copy_file(tmp, f, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp, ec);
  }
}

std::string App::addProfileInfoImage(const std::string& name,
                                     const fs::path& src) const
{
  requireInit();
  std::error_code ec;
  const fs::path imagesDir = profileInfoDir(name) / "images";
  fs::create_directories(imagesDir, ec);

  // Keep the original name; disambiguate with a numeric suffix on collision.
  const std::string stem = src.stem().string();
  const std::string ext  = src.extension().string();
  fs::path dst = imagesDir / (stem + ext);
  for (int i = 1; fs::exists(dst, ec); ++i)
    dst = imagesDir / (stem + "_" + std::to_string(i) + ext);

  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec)
    throw std::runtime_error("cannot copy image: " + src.string());
  return "images/" + dst.filename().string();
}

void App::saveProfile(const Profile& p) const
{
  writeJson(profilePath(p.name), Json(p));
}

// --- active-profile mod operations ------------------------------------------

void App::setEnabled(const std::string& modId, bool enabled)
{
  requireInit();
  if (m_activeProfile.empty())
    throw std::runtime_error("no active profile; run `gtamm profile-use <name>`");
  const auto mod = findMod(modId);
  if (!mod)
    throw std::runtime_error("no such mod in pool: " + modId);
  const std::string id = mod->id;  // canonical

  Profile p   = loadProfile(m_activeProfile);
  auto it     = std::find_if(p.entries.begin(), p.entries.end(),
                             [&](const ProfileEntry& e) { return e.modId == id; });
  if (it == p.entries.end()) {
    ProfileEntry e;
    e.modId    = id;
    e.enabled  = enabled;
    // Place new mods on top of the conflict order by default.
    int maxPrio = -1;
    for (const auto& existing : p.entries)
      maxPrio = std::max(maxPrio, existing.priority);
    e.priority = maxPrio + 1;
    p.entries.push_back(e);
  } else {
    it->enabled = enabled;
  }
  saveProfile(p);
}

namespace {

std::string toLower(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Known SFX pak names (audio/SFX/<pak>). Used to recognise a pak-named folder in
// a mod and to infer the target pak from the mod's name.
const char* const kSfxPaks[] = {"GENRL",  "FEET",   "PAIN_A", "SCRIPT", "SPC_EA",
                                "SPC_FA", "SPC_GA", "SPC_NA", "SPC_PA"};

// Which SFX pak a sound bank targets: a pak-named parent folder wins; otherwise
// infer from the mod id (e.g. "sgenrl" -> GENRL); default GENRL (general sounds).
std::string sfxResolvePak(const std::string& pakFromPath, const std::string& modId)
{
  const std::string pf = toLower(pakFromPath);
  for (const char* k : kSfxPaks)
    if (pf == toLower(k))
      return k;
  const std::string m = toLower(modId);
  for (const char* k : kSfxPaks)
    if (m.find(toLower(k)) != std::string::npos)
      return k;
  return "GENRL";
}

// Reject paths that try to escape the game dir (absolute or containing "..").
bool isSafeRel(const fs::path& rel)
{
  if (rel.is_absolute())
    return false;
  for (const auto& part : rel) {
    const std::string s = part.string();
    if (s == ".." || s == "")
      return false;
  }
  return true;
}

// Replace `target` with `tmp` (both on the same volume). Retries to ride out
// transient locks (antivirus/indexer scanning a freshly written archive), then
// falls back to overwriting in place.
void replaceFile(const fs::path& tmp, const fs::path& target)
{
  std::error_code ec;
  for (int attempt = 0; attempt < 20; ++attempt) {
    ec.clear();
    fs::rename(tmp, target, ec);  // MoveFileEx replaces an existing target
    if (!ec)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // Fallback: truncate-overwrite the target in place (no delete needed).
  ec.clear();
  fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
  if (!ec) {
    fs::remove(tmp, ec);
    return;
  }
  throw std::runtime_error("failed to replace '" + target.string() +
                           "': " + ec.message());
}

// Move a file, retrying briefly to ride out transient locks (antivirus/indexer
// scanning a freshly written file -- the same class of failure replaceFile()
// guards against) before falling back to copy+remove across volumes. Used for
// every backup/restore in deploy()/rollback(), including the game exe and Mod
// Loader runtime DLLs: without the retry, a momentary lock right after replace
// would throw here, aborting rollbackLoose() mid-loop and leaving the just-
// removed target (e.g. gta_sa.exe) deleted instead of restored from backup.
void moveFile(const fs::path& from, const fs::path& to)
{
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec)
    return;
  // A genuine cross-volume rename fails immediately every time -- don't waste
  // time retrying that case, go straight to the copy fallback below.
  if (ec != std::errc::cross_device_link) {
    for (int attempt = 0; attempt < 20 && ec; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      ec.clear();
      fs::rename(from, to, ec);
    }
    if (!ec)
      return;
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ec)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (ec)
    throw std::runtime_error("failed to move '" + from.string() + "' to '" +
                             to.string() + "': " + ec.message());
  fs::remove(from, ec);  // best-effort; the copy already succeeded
}

// Create parent directories for `rel` under `gameDir`, recording the ones we
// actually create so rollback can remove them again (never touching pre-existing
// game folders).
void ensureParentDirs(const fs::path& gameDir, const fs::path& rel,
                      std::vector<std::string>& createdDirs)
{
  const fs::path relDir = rel.parent_path();
  fs::path cur;
  fs::path abs = gameDir;
  for (const auto& comp : relDir) {
    cur /= comp;
    abs /= comp;
    std::error_code ec;
    if (!fs::exists(abs, ec)) {
      fs::create_directory(abs);
      createdDirs.push_back(cur.generic_string());
    }
  }
}

std::string readFileText(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// True if the file's first bytes equal `magic` (e.g. "bnry" for a binary IPL).
bool fileHasMagic(const fs::path& p, const std::string& magic)
{
  std::ifstream in(p, std::ios::binary);
  std::string buf(magic.size(), '\0');
  in.read(buf.data(), static_cast<std::streamsize>(magic.size()));
  return in && buf == magic;
}

// Collect the model/texture names referenced by an .ide so a genuinely STREAMED
// model/texture (church.dff named by an objs line) can be told apart from a
// plugin's loose texture (GInput's x360btns.txd, which no IDE references and
// which the engine never streams). Object sections look like
// "<id>, <model>, <txd>, ...". Names are stored lowercased, without extension.
void collectIdeNames(const std::string& text, std::set<std::string>& out)
{
  static const std::set<std::string> objectSections = {
      "objs", "tobj", "anim", "cars", "peds", "weap", "hier"};
  std::string section;
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t nl = text.find('\n', i);
    if (nl == std::string::npos)
      nl = text.size();
    std::string t = text.substr(i, nl - i);
    i = nl + 1;
    // Trim surrounding whitespace/CR.
    std::size_t a = 0, b = t.size();
    while (a < b && std::isspace(static_cast<unsigned char>(t[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(t[b - 1]))) --b;
    t = t.substr(a, b - a);
    if (t.empty() || t[0] == ';' || t[0] == '#')
      continue;
    const std::string low = toLower(t);
    if (section.empty()) {
      if (objectSections.count(low))
        section = low;
      continue;
    }
    if (low == "end") {
      section.clear();
      continue;
    }
    // Data line: "<id>, <model>, <txd>, ...".
    std::vector<std::string> f;
    std::string cur;
    for (char c : t) {
      if (c == ',') {
        f.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    f.push_back(cur);
    auto trim = [](std::string v) {
      std::size_t x = 0, y = v.size();
      while (x < y && std::isspace(static_cast<unsigned char>(v[x]))) ++x;
      while (y > x && std::isspace(static_cast<unsigned char>(v[y - 1]))) --y;
      return v.substr(x, y - x);
    };
    if (f.size() >= 3) {
      out.insert(toLower(trim(f[1])));  // model name (-> <model>.dff)
      out.insert(toLower(trim(f[2])));  // txd name   (-> <txd>.txd)
    }
  }
}

}  // namespace

std::map<std::string, fs::path> App::buildImgIndex() const
{
  std::map<std::string, fs::path> index;
  if (m_gamePath.empty())
    return index;
  const fs::path gameDir = m_gamePath;
  // SA keeps its IMG archives under models/ (and anim/).
  for (const char* sub : {"models", "anim"}) {
    const fs::path dir = gameDir / sub;
    std::error_code ec;
    if (!fs::exists(dir, ec))
      continue;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
      if (!de.is_regular_file() || toLower(de.path().extension().string()) != ".img")
        continue;
      if (!imgIsVer2(de.path()))
        continue;
      for (const auto& entry : imgReadDirectory(de.path()))
        index.emplace(toLower(entry.name), de.path());  // first archive wins
    }
  }
  return index;
}

fs::path App::primaryStreamImg() const
{
  if (m_gamePath.empty())
    return {};
  const fs::path gta3 = fs::path(m_gamePath) / "models" / "gta3.img";
  std::error_code ec;
  if (fs::exists(gta3, ec) && imgIsVer2(gta3))
    return gta3;
  return {};
}

std::map<std::string, std::string> App::buildLooseIndex() const
{
  std::map<std::string, std::string> index;
  if (m_gamePath.empty())
    return index;
  const fs::path gameDir = m_gamePath;
  std::error_code ec;
  for (const auto& de : fs::recursive_directory_iterator(gameDir, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    if (toLower(de.path().extension().string()) == ".img")
      continue;  // IMG archives are handled by name via buildImgIndex
    const fs::path rel = fs::relative(de.path(), gameDir, ec);
    if (ec)
      continue;
    // First occurrence wins (shallowest paths come first in iteration).
    index.emplace(toLower(de.path().filename().string()), rel.generic_string());
  }
  return index;
}

App::ActivePlan App::computeActive() const
{
  ActivePlan plan;
  if (m_activeProfile.empty())
    return plan;

  const auto imgIndex   = buildImgIndex();
  const auto looseIndex = buildLooseIndex();    // vanilla loose: name -> game path
  const fs::path primaryImg = primaryStreamImg();  // gta3.img, for NEW map content

  Profile profile = loadProfile(m_activeProfile);
  // Highest priority first: the first mod to claim a target wins it.
  std::sort(profile.entries.begin(), profile.entries.end(),
            [](const ProfileEntry& a, const ProfileEntry& b) {
              return a.priority > b.priority;
            });

  // Pre-pass (only when experimental map auto-install is on): the set of
  // model/texture names referenced by any enabled mod's .ide files. A NEW
  // .dff/.txd is injected into gta3.img only if it is actually streamed (named
  // here) -- this keeps a plugin's loose textures (e.g. GInput's x360btns.txd,
  // which no IDE references) out of the archive so the plugin can still read them.
  std::set<std::string> streamedNames;
  if (m_autoRouteMaps) {
    for (const auto& e : profile.entries) {
      if (e.separator || !e.enabled)
        continue;
      const fs::path root = modsDir() / e.modId / "root";
      std::error_code ec;
      if (!fs::exists(root, ec))
        continue;
      for (const auto& de : fs::recursive_directory_iterator(root, ec))
        if (de.is_regular_file(ec) &&
            toLower(de.path().extension().string()) == ".ide")
          collectIdeNames(readFileText(de.path()), streamedNames);
    }
  }

  for (const auto& e : profile.entries) {
    if (e.separator || !e.enabled)
      continue;
    const fs::path root = modsDir() / e.modId / "root";
    std::error_code ec;
    if (!fs::exists(root, ec))
      continue;

    // Mods flagged "via Mod Loader" bypass native routing entirely: their whole
    // tree is staged verbatim under modloader/<name>/ and the Mod Loader runtime
    // loads it at game time. (Same-target files across mods resolve by priority.)
    const auto modInfo = findMod(e.modId);
    if (modInfo && modInfo->viaModloader) {
      const std::string folder = modloaderFolderName(modInfo->name);
      for (const auto& de : fs::recursive_directory_iterator(root, ec)) {
        if (!de.is_regular_file())
          continue;
        const fs::path rel = fs::relative(de.path(), root, ec);
        if (ec || !isSafeRel(rel))
          continue;
        const std::string target = "modloader/" + folder + "/" + rel.generic_string();
        const std::string key    = toLower(target);
        auto it                  = plan.modloader.find(key);
        if (it == plan.modloader.end()) {
          Merged m;
          m.winner          = e.modId;
          m.winnerRel       = target;
          m.winnerSourceRel = rel.generic_string();
          m.providers       = {e.modId};
          plan.modloader.emplace(key, std::move(m));
        } else {
          it->second.providers.push_back(e.modId);
        }
      }
      continue;
    }

    for (const auto& de : fs::recursive_directory_iterator(root, ec)) {
      if (!de.is_regular_file())
        continue;
      const fs::path rel = fs::relative(de.path(), root, ec);
      if (ec || !isSafeRel(rel))
        continue;
      const std::string realRel = rel.generic_string();

      // Route SAAT sound-bank files ("[<pak>/]Bank_<n>/sound_<m>.wav") into the
      // SFX archive. These are injected at deploy; deploying them loose does
      // nothing (the engine only reads sounds from audio/SFX/<pak>).
      if (toLower(rel.extension().string()) == ".wav" &&
          sfx::parseSoundFile(rel.filename().string()) >= 1) {
        std::vector<std::string> comps;
        for (const auto& part : rel)
          comps.push_back(part.string());
        int bankNo = -1;
        std::string pakFromPath;
        for (std::size_t ci = 0; ci + 1 < comps.size(); ++ci) {
          const int b = sfx::parseBankDir(comps[ci]);
          if (b >= 1) {
            bankNo = b;
            if (ci >= 1)
              pakFromPath = comps[ci - 1];
            break;
          }
        }
        if (bankNo >= 1) {
          const int soundNo = sfx::parseSoundFile(rel.filename().string());
          const std::string pak = sfxResolvePak(pakFromPath, e.modId);
          auto& bankMap = plan.sfx[pak][bankNo - 1];  // 1-based -> 0-based local
          auto sit      = bankMap.find(soundNo - 1);
          if (sit == bankMap.end()) {
            Merged m;
            m.winner          = e.modId;
            m.winnerRel       = realRel;
            m.winnerSourceRel = realRel;
            m.providers       = {e.modId};
            bankMap.emplace(soundNo - 1, std::move(m));
          } else {
            sit->second.providers.push_back(e.modId);
          }
          continue;
        }
      }

      // Route model/texture/stream files into the IMG archive that holds them.
      // .ipl is treated as IMG-routable too: vanilla world stream IPLs live inside
      // gta3.img, so a same-named replacement is injected there.
      const std::string base = toLower(rel.filename().string());
      const std::string ext  = toLower(rel.extension().string());
      // .dff/.txd/.col/.ifp replacing a vanilla IMG entry is always safe. Routing
      // .ipl world-streams into the IMG is part of the experimental map path.
      const bool imgExt = rules::isImgContentFile(rel.filename().string()) ||
                          (m_autoRouteMaps && ext == ".ipl");
      auto imgHit = imgExt ? imgIndex.find(base) : imgIndex.end();

      // Helper: add a winning entry to a given IMG archive (by lowercased name).
      auto addToImg = [&](const std::string& imgPathKey) {
        auto& archive = plan.img[imgPathKey];
        auto it       = archive.find(base);
        if (it == archive.end()) {
          Merged m;
          m.winner          = e.modId;
          m.winnerRel       = rel.filename().string();  // entry name
          m.winnerSourceRel = realRel;
          m.providers       = {e.modId};
          archive.emplace(base, std::move(m));
        } else {
          it->second.providers.push_back(e.modId);
        }
      };

      if (imgHit != imgIndex.end()) {
        addToImg(imgHit->second.generic_string());
        continue;
      }

      // --- Experimental map auto-install (OFF by default): route streaming/map
      // content not present in any vanilla IMG, Mod Loader std.stream/std.data
      // style. Disabled by default because injecting a mod's .ide/.ipl/models can
      // crash a new game when the mod's object IDs clash with vanilla. ---
      const bool inLoose = looseIndex.count(base) > 0;  // a vanilla loose file?
      if (m_autoRouteMaps && !inLoose) {
        // A NEW model/texture/collision/animation belongs in the stream ONLY if an
        // installed .ide references it by name; otherwise it is a loose asset a
        // plugin reads directly (GInput's button textures, custom HUD/menu txds)
        // and must stay on disk. A NEW binary stream IPL is always map content.
        const std::string stem = toLower(rel.stem().string());
        const bool streamExt =
            ext == ".dff" || ext == ".txd" || ext == ".col" || ext == ".ifp";
        const bool binaryIpl = ext == ".ipl" && fileHasMagic(de.path(), "bnry");
        if (((streamExt && streamedNames.count(stem)) || binaryIpl) &&
            !primaryImg.empty()) {
          addToImg(primaryImg.generic_string());
          continue;
        }
        // A NEW item-definition (.ide) or text placement (.ipl) dumped at the
        // game root is staged under data/maps/gmm/ and registered in data/gta.dat
        // so the engine loads it. A file the mod already placed under a folder
        // (e.g. data/maps/...) is left as-is -- it is assumed intentional / part
        // of a self-contained map that ships its own gta.dat.
        if ((ext == ".ide" || ext == ".ipl") && rel.parent_path().empty()) {
          const std::string target = "data/maps/gmm/" + rel.filename().string();
          const std::string mkey    = toLower(target);
          auto it                   = plan.loose.find(mkey);
          if (it == plan.loose.end()) {
            Merged m;
            m.winner          = e.modId;
            m.winnerRel       = target;
            m.winnerSourceRel = realRel;
            m.providers       = {e.modId};
            plan.loose.emplace(mkey, std::move(m));
            plan.mapRegistrations.push_back(
                {ext == ".ide" ? "IDE" : "IPL", target});
          } else {
            it->second.providers.push_back(e.modId);
          }
          continue;
        }
      }

      // Otherwise it is a plain loose file. A BARE top-level file is repathed to
      // where the game actually reads it: a ".lua" is a MoonLoader script, and any
      // other bare file whose name matches a vanilla loose file lands at that
      // location (fonts.txd -> models/fonts.txd, a bare radio "CO" ->
      // audio/streams/CO) instead of cluttering the game root. Files the mod
      // already placed under a folder are left exactly where the mod put them
      // (conservative: we don't second-guess a mod's own layout).
      std::string targetRel = realRel;
      if (rel.parent_path().empty()) {
        if (ext == ".lua") {
          targetRel = "moonloader/" + realRel;
        } else {
          auto li = looseIndex.find(base);
          if (li != looseIndex.end())
            targetRel = li->second;
        }
      }

      const std::string key = toLower(targetRel);
      auto it               = plan.loose.find(key);
      if (it == plan.loose.end()) {
        Merged m;
        m.winner          = e.modId;
        m.winnerRel       = targetRel;  // where it deploys (game-relative)
        m.winnerSourceRel = realRel;    // where it lives in the mod root
        m.providers       = {e.modId};
        plan.loose.emplace(key, std::move(m));
      } else {
        it->second.providers.push_back(e.modId);
      }
    }
  }
  return plan;
}

std::vector<Conflict> App::conflicts() const
{
  requireInit();
  std::vector<Conflict> result;
  const auto plan = computeActive();
  for (const auto& [key, m] : plan.loose) {
    if (m.providers.size() < 2)
      continue;
    // Mergeable data files are line-merged, not won/shadowed.
    if (rules::isMergeableData(fs::path(m.winnerRel).filename().string()))
      continue;
    Conflict c;
    c.path   = m.winnerRel;
    c.winner = m.winner;
    c.losers.assign(m.providers.begin() + 1, m.providers.end());
    result.push_back(std::move(c));
  }
  for (const auto& [imgPath, archive] : plan.img) {
    const std::string imgName = fs::path(imgPath).filename().string();
    for (const auto& [base, m] : archive) {
      if (m.providers.size() < 2)
        continue;
      Conflict c;
      c.path   = imgName + " :: " + m.winnerRel;  // e.g. "gta3.img :: infernus.dff"
      c.winner = m.winner;
      c.losers.assign(m.providers.begin() + 1, m.providers.end());
      result.push_back(std::move(c));
    }
  }
  // Same-target files across Mod Loader mods (unlikely, but possible when two
  // enabled mods share a folder name).
  for (const auto& [key, m] : plan.modloader) {
    if (m.providers.size() < 2)
      continue;
    Conflict c;
    c.path   = m.winnerRel;
    c.winner = m.winner;
    c.losers.assign(m.providers.begin() + 1, m.providers.end());
    result.push_back(std::move(c));
  }
  return result;
}

fs::path App::requireGameDir() const
{
  if (m_gamePath.empty())
    throw std::runtime_error("game path is not set; run `gtamm init --game <path>`");
  const fs::path g = m_gamePath;
  std::error_code ec;
  if (!fs::exists(g, ec) || !fs::is_directory(g, ec))
    throw std::runtime_error("game path does not exist: " + m_gamePath);
  return g;
}

bool App::isDeployed() const
{
  std::error_code ec;
  return fs::exists(manifestPath(), ec);
}

Manifest App::currentManifest() const
{
  requireInit();
  if (!isDeployed())
    throw std::runtime_error("nothing is deployed");
  return readJson(manifestPath()).get<Manifest>();
}

void App::deploy()
{
  requireInit();
  if (m_activeProfile.empty())
    throw std::runtime_error("no active profile; run `gtamm profile-use <name>`");
  const fs::path gameDir = requireGameDir();

  ActivePlan plan = computeActive();

  // IMG archives + SFX paks this deploy will rebuild (game-relative paths).
  auto computeTargets = [&](const ActivePlan& p, std::set<std::string>& imgs,
                            std::set<std::string>& sfxp) {
    imgs.clear();
    sfxp.clear();
    for (const auto& [imgPathStr, archive] : p.img)
      if (!archive.empty())
        imgs.insert(fs::relative(fs::path(imgPathStr), gameDir).generic_string());
    for (const auto& [pakName, bankEdits] : p.sfx)
      if (!bankEdits.empty())
        sfxp.insert("audio/SFX/" + pakName);
  };

  std::set<std::string> targetImgs, targetSfx;
  computeTargets(plan, targetImgs, targetSfx);
  bool willSfx = !targetSfx.empty();

  // Redeploy: undo the previous deployment, but skip restoring archives we are
  // about to rebuild from pristine anyway (rebuild reads pristine, so the current
  // game archive is irrelevant -- restoring it first would be wasted ~GB of IO).
  if (isDeployed()) {
    const Manifest prev = readJson(manifestPath()).get<Manifest>();
    rollbackLoose(prev, gameDir);
    for (const auto& rel : prev.imgFiles)
      if (targetImgs.count(rel) == 0)
        restoreImg(rel, gameDir);
    for (const auto& rel : prev.sfxPaks)
      if (targetSfx.count(rel) == 0)
        restoreSfx(rel, gameDir);
    // BankLkup is rewritten from pristine whenever we have SFX edits; only restore
    // it here if this deploy won't touch SFX at all.
    if (prev.sfxConfig && !willSfx)
      restoreSfx("audio/CONFIG/BankLkup.dat", gameDir);
    std::error_code ec;
    fs::remove(manifestPath(), ec);

    // Re-resolve against the now-clean (vanilla) game folder: the loose/IMG
    // indices that drive map routing must not see the just-removed prior deploy
    // (otherwise a staged data/maps/gmm/*.ide would look "vanilla" and skip its
    // gta.dat registration).
    plan = computeActive();
    computeTargets(plan, targetImgs, targetSfx);
    willSfx = !targetSfx.empty();
  }

  Manifest man;
  man.gameDir    = gameDir.generic_string();
  man.profile    = m_activeProfile;
  man.deployedAt = static_cast<std::int64_t>(std::time(nullptr));

  try {
  // --- loose files: hardlink into the game folder ---
  for (const auto& [key, m] : plan.loose) {
    const fs::path rel    = fs::path(m.winnerRel);
    const fs::path source = modsDir() / m.winner / "root" / fs::path(m.winnerSourceRel);
    const fs::path target = gameDir / rel;

    ensureParentDirs(gameDir, rel, man.createdDirs);

    ManifestFile mf;
    mf.rel = m.winnerRel;
    mf.mod = m.winner;

    // Mergeable data files (handling.cfg, ...) are line-merged over the vanilla
    // base so a partial mod does not wipe the rest of the file. But actually
    // merging only makes sense when there's something to merge: multiple mods
    // providing this same path, or an existing (vanilla) file to preserve the
    // untouched parts of. A single provider with no pre-existing file at this
    // path has nothing to merge against -- treat it as a plain file instead of
    // round-tripping it through the merge grammar, which can silently drop
    // content it doesn't recognize as a section/keyed line. This matters for
    // filenames that share an extension with a mergeable vanilla file but
    // aren't actually one -- e.g. SA-MP's own SAMP/SAMP.ide, which has no
    // vanilla ".ide" counterpart at all: mergeIde() would parse it as a bare
    // preamble block (no recognized section keyword) and drop it entirely,
    // silently installing an empty file.
    std::error_code ec0;
    const std::string base =
        fs::exists(target, ec0) ? readFileText(target) : std::string{};
    const bool mergeable = rules::isMergeableData(rel.filename().string()) &&
                           (m.providers.size() > 1 || !base.empty());
    std::string mergedContent;
    if (mergeable) {
      std::vector<std::string> texts;  // low -> high priority
      for (auto it = m.providers.rbegin(); it != m.providers.rend(); ++it)
        texts.push_back(
            readFileText(modsDir() / *it / "root" / fs::path(m.winnerSourceRel)));
      mergedContent = rules::mergeData(rel.filename().string(), base, texts);
    }

    // Preserve any existing (original) game file before overwriting it.
    std::error_code ec;
    if (fs::exists(target, ec)) {
      const fs::path backup = backupDir() / rel;
      fs::create_directories(backup.parent_path());
      moveFile(target, backup);
      mf.hadBackup = true;
    }

    if (mergeable) {
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      out.write(mergedContent.data(),
                static_cast<std::streamsize>(mergedContent.size()));
      mf.method = "merge";
    } else {
      // Hardlink if possible (no admin needed, zero extra disk), else copy.
      try {
        fs::create_hard_link(source, target);
        mf.method = "hardlink";
      } catch (const fs::filesystem_error&) {
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        mf.method = "copy";
      }
    }
    man.files.push_back(std::move(mf));
  }

  // --- Mod Loader mods: stage each mod's tree verbatim under modloader/<name>/
  // and let the Mod Loader runtime load it at game time. These are plain new
  // files (they live under modloader/, so nothing vanilla is overwritten); the
  // normal manifest handles their removal + dir pruning on rollback. ---
  for (const auto& [key, m] : plan.modloader) {
    const fs::path rel    = fs::path(m.winnerRel);
    const fs::path source = modsDir() / m.winner / "root" / fs::path(m.winnerSourceRel);
    const fs::path target = gameDir / rel;

    ensureParentDirs(gameDir, rel, man.createdDirs);

    ManifestFile mf;
    mf.rel = m.winnerRel;
    mf.mod = m.winner;

    std::error_code ec;
    if (fs::exists(target, ec)) {
      const fs::path backup = backupDir() / rel;
      fs::create_directories(backup.parent_path());
      moveFile(target, backup);
      mf.hadBackup = true;
    }
    try {
      fs::create_hard_link(source, target);
      mf.method = "hardlink";
    } catch (const fs::filesystem_error&) {
      fs::copy_file(source, target, fs::copy_options::overwrite_existing);
      mf.method = "copy";
    }
    man.files.push_back(std::move(mf));
  }

  // Install the Mod Loader runtime (modloader.asi + ASI loader + CLEO + bundled
  // compatibility fixes, the latter gated separately by enableEssentials()).
  // enableModloader() is a true master switch: it is the ONLY thing that gates
  // this, deliberately NOT implied by plan.modloader being non-empty. Earlier
  // this also auto-turned on whenever any mod was flagged viaModloader, on the
  // reasoning that such a mod needs modloader.asi to load at all -- but that
  // meant the checkbox could not actually turn Mod Loader off: a single
  // enabled viaModloader mod would silently keep it running regardless of the
  // setting, which is exactly the confusing "I disabled it and it's still
  // there" behavior reported. Turning this OFF now means a viaModloader mod's
  // files still get staged under modloader/<name>/ (the loop above is
  // unconditional) but sit inert with no loader to read them -- the mod
  // silently stops working, which is the accepted tradeoff for the switch
  // being unambiguous. Deduped file-by-file against whatever a native mod (or
  // an imported build) already deployed loose this run -- a mod's own
  // modloader.asi/CLEO/etc. wins for that exact file, but anything it does NOT
  // bring (e.g. modloader/.data/, or CLEO/essentials alongside a bare
  // modloader.asi-only mod) still gets filled in. installedOurRuntime reflects
  // whether we actually installed at least one file, not merely whether
  // enableModloader() was true.
  const bool wantModloader = m_enableModloader;
  const bool installedOurRuntime = wantModloader && deployModloaderRuntime(gameDir, man);

  // If an enabled mod already deployed its OWN gta_sa.exe above as ordinary
  // loose content (e.g. the leftover-of-the-build mod import-build produces
  // for a pre-built compilation that ships a patched/no-CD exe its bundled ASI
  // plugins were built against), leave it alone -- exactly the same reasoning
  // as installedOurRuntime's modloader.asi check below. Forcing a *different*
  // exe under exe-patching ASI plugins (SkyGFX, limit adjusters, etc.) is a
  // common cause of crashes on mission load, a black screen with sound/HUD
  // still present, or a flickering screen: those plugins hook the exe binary
  // they were matched against, not a generic vanilla one. This also prevents a
  // second manifest entry for the same "gta_sa.exe" path below, which used to
  // corrupt rollback: the first restore's backup gets consumed (moveFile moves,
  // not copies), so the second restore attempt finds no backup and the exe
  // ends up deleted rather than restored.
  const bool modBroughtOwnExe =
      std::any_of(man.files.begin(), man.files.end(), [](const ManifestFile& f) {
        return toLower(f.rel) == "gta_sa.exe";
      });

  // Replace the game exe with the bundled clean GTA SA 1.0 (from the runtime):
  // explicitly via replaceGameExe(), or implicitly when WE just installed the
  // Mod Loader runtime above (it's built against 1.0, and mixing it with a
  // different exe build is a common reason it silently fails to load
  // anything). Deliberately NOT implied merely by wantModloader/plan.modloader:
  // if every runtime file was deduped away (a mod's own modloader.asi + loader
  // DLLs + .data already cover everything our bundle would provide --
  // installedOurRuntime false), that setup was presumably already working
  // together with THEIR exe -- forcing ours in under them is more likely to
  // break a working setup than fix a broken one. Reversible either way (backed
  // up + restored on rollback).
  if ((m_replaceGameExe || installedOurRuntime) && !modBroughtOwnExe) {
    const fs::path srcExe = modloaderRuntimeDir() / "gta_sa.exe";
    std::error_code ec;
    if (fs::exists(srcExe, ec)) {
      const std::string rel = "gta_sa.exe";
      const fs::path target = gameDir / fs::path(rel);
      // Skip if the game already has this exact exe.
      bool identical = false;
      if (fs::exists(target, ec) &&
          fs::file_size(srcExe, ec) == fs::file_size(target, ec)) {
        std::ifstream fa(srcExe, std::ios::binary), fb(target, std::ios::binary);
        identical = fa && fb &&
                    std::equal(std::istreambuf_iterator<char>(fa),
                               std::istreambuf_iterator<char>(),
                               std::istreambuf_iterator<char>(fb));
      }
      if (!identical) {
        ManifestFile mf;
        mf.rel = rel;
        mf.mod = "(gmm-exe)";
        if (fs::exists(target, ec)) {
          const fs::path backup = backupDir() / fs::path(rel);
          fs::create_directories(backup.parent_path());
          moveFile(target, backup);
          mf.hadBackup = true;
        }
        try {
          fs::create_hard_link(srcExe, target);
          mf.method = "hardlink";
        } catch (const fs::filesystem_error&) {
          fs::copy_file(srcExe, target, fs::copy_options::overwrite_existing);
          mf.method = "copy";
        }
        man.files.push_back(std::move(mf));
      }
    }
  }

  // Install the bundled SA-MP client (samp.exe, mouse.png, sampgui.png, the
  // SAMP/ custom-object archives, ...) whenever this profile is set up to
  // launch through SA-MP. This is independent of any mod: it exists precisely
  // because a hand-imported SA-MP mod (via the general import pipeline, or via
  // import-build's vanilla-diff) can end up missing pieces the ordinary mod
  // machinery isn't a good fit for (see deploySampRuntime()'s doc comment) --
  // it only fills in what an enabled mod hasn't already placed this deploy.
  if (!m_activeProfile.empty() && loadProfile(m_activeProfile).samp.enabled)
    deploySampRuntime(gameDir, man);

  // --- gta.dat: register new map IDE/IPL files staged under data/maps/gmm/ so
  // the engine actually loads them (Variant B: emulate Mod Loader's std.data) ---
  if (!plan.mapRegistrations.empty()) {
    const std::string datRel = "data/gta.dat";
    const fs::path datAbs    = gameDir / fs::path(datRel);
    std::error_code ec;

    // If a mod already deployed gta.dat this run (its backup is recorded), append
    // to it; otherwise back up the vanilla file before editing.
    bool alreadyDeployed = false;
    for (const auto& mf : man.files)
      if (toLower(mf.rel) == datRel) {
        alreadyDeployed = true;
        break;
      }

    const std::string before =
        fs::exists(datAbs, ec) ? readFileText(datAbs) : std::string{};

    if (!alreadyDeployed) {
      ManifestFile mf;
      mf.rel    = "data/gta.dat";
      mf.mod    = "(gmm-maps)";
      mf.method = "merge";
      ensureParentDirs(gameDir, fs::path(datRel), man.createdDirs);
      if (fs::exists(datAbs, ec)) {
        const fs::path backup = backupDir() / fs::path(datRel);
        fs::create_directories(backup.parent_path());
        moveFile(datAbs, backup);
        mf.hadBackup = true;
      }
      man.files.push_back(std::move(mf));
    }

    // Case-insensitive whole-line membership test.
    auto hasLine = [](const std::string& hay, const std::string& line) {
      auto lc = [](std::string s) {
        for (char& c : s)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
      };
      const std::string h = lc(hay), l = lc(line);
      std::size_t pos = 0;
      while ((pos = h.find(l, pos)) != std::string::npos) {
        const bool startOk = pos == 0 || h[pos - 1] == '\n' || h[pos - 1] == '\r';
        const std::size_t end = pos + l.size();
        const bool endOk = end == h.size() || h[end] == '\n' || h[end] == '\r';
        if (startOk && endOk)
          return true;
        pos = end;
      }
      return false;
    };

    std::string out = before;
    if (!out.empty() && out.back() != '\n')
      out += "\n";
    // IDE lines first, then IPL (item definitions must be parsed before placements).
    for (const char* kind : {"IDE", "IPL"})
      for (const auto& reg : plan.mapRegistrations)
        if (reg.kind == kind) {
          const std::string line = reg.kind + " " + reg.relPath;
          if (!hasLine(before, line))
            out += line + "\n";
        }

    // If a mod already deployed gta.dat as a HARDLINK, an in-place write would
    // edit the shared pool file too -- remove it first to break the link, then
    // write a fresh standalone file (the vanilla original is safe in backup/).
    fs::remove(datAbs, ec);
    std::ofstream o(datAbs, std::ios::binary | std::ios::trunc);
    o.write(out.data(), static_cast<std::streamsize>(out.size()));
  }

  // --- IMG archives: rebuild from a pristine copy with the winning injections ---
  for (const auto& [imgPathStr, archive] : plan.img) {
    if (archive.empty())
      continue;
    const fs::path imgAbs  = imgPathStr;
    const std::string rel  = fs::relative(imgAbs, gameDir).generic_string();
    const fs::path pristine = imgPristineDir() / rel;

    // Cache the untouched original once, then always rebuild from it so repeated
    // deploys never compound changes. (Copy, not move: the game archive must stay
    // intact if the rebuild below fails.)
    if (!fs::exists(pristine)) {
      fs::create_directories(pristine.parent_path());
      fs::copy_file(imgAbs, pristine, fs::copy_options::overwrite_existing);
    }

    std::vector<ImgInjection> injections;
    injections.reserve(archive.size());
    for (const auto& [base, m] : archive)
      injections.push_back(
          {m.winnerRel, modsDir() / m.winner / "root" / fs::path(m.winnerSourceRel)});

    const fs::path tmp = imgAbs.string() + ".gtamm_tmp";
    imgRebuild(pristine, tmp, injections);
    replaceFile(tmp, imgAbs);

    man.imgFiles.push_back(rel);
  }

  // --- SFX audio: inject SAAT sound banks into audio/SFX/<pak>, updating the
  // shared audio/CONFIG/BankLkup.dat. Rebuilt from a pristine cache so repeated
  // deploys never compound. ---
  if (!plan.sfx.empty()) {
    const fs::path audioDir = gameDir / "audio";
    const fs::path lkupAbs  = audioDir / "CONFIG" / "BankLkup.dat";
    const std::string lkupRel = fs::relative(lkupAbs, gameDir).generic_string();

    // Cache pristine BankLkup once, then always start from it.
    const fs::path lkupPristine = sfxPristineDir() / lkupRel;
    if (!fs::exists(lkupPristine)) {
      fs::create_directories(lkupPristine.parent_path());
      fs::copy_file(lkupAbs, lkupPristine, fs::copy_options::overwrite_existing);
    }

    auto banks = sfx::readBankLkup(lkupPristine);
    const auto pakNames = sfx::readPakNames(audioDir);

    for (const auto& [pakName, bankEdits] : plan.sfx) {
      int pakIndex = -1;
      for (std::size_t i = 0; i < pakNames.size(); ++i)
        if (pakNames[i] == pakName) {
          pakIndex = static_cast<int>(i);
          break;
        }
      if (pakIndex < 0)
        continue;  // unknown pak name; skip rather than corrupt

      const std::string pakRel = ("audio/SFX/" + pakName);
      const fs::path pakAbs = gameDir / fs::path(pakRel);
      const fs::path pakPristine = sfxPristineDir() / pakRel;
      if (!fs::exists(pakPristine)) {
        fs::create_directories(pakPristine.parent_path());
        fs::copy_file(pakAbs, pakPristine, fs::copy_options::overwrite_existing);
      }

      // local bank -> local sound -> winning wav file
      std::map<int, std::map<int, fs::path>> edits;
      for (const auto& [localBank, soundMap] : bankEdits)
        for (const auto& [soundIdx, m] : soundMap)
          edits[localBank][soundIdx] =
              modsDir() / m.winner / "root" / fs::path(m.winnerSourceRel);

      const fs::path tmp = pakAbs.string() + ".gtamm_tmp";
      sfx::rebuildPak(pakPristine, tmp, pakIndex, banks, edits);
      replaceFile(tmp, pakAbs);
      man.sfxPaks.push_back(pakRel);
    }

    // Rewrite BankLkup.dat (offsets/audioLen) from the pristine base.
    const fs::path lkupTmp = lkupAbs.string() + ".gtamm_tmp";
    sfx::patchBankLkup(lkupPristine, lkupTmp, banks);
    replaceFile(lkupTmp, lkupAbs);
    man.sfxConfig = true;
  }
  } catch (...) {
    // Atomic deploy: if anything fails partway, undo every change made so far
    // and re-throw, so the game folder returns to its pre-deploy state even
    // though no manifest was written.
    rollbackLoose(man, gameDir);
    for (const std::string& rel : targetImgs)
      restoreImg(rel, gameDir);
    for (const std::string& rel : targetSfx)
      restoreSfx(rel, gameDir);
    if (willSfx)
      restoreSfx("audio/CONFIG/BankLkup.dat", gameDir);
    throw;
  }

  writeJson(manifestPath(), Json(man));
}

bool App::deployModloaderRuntime(const fs::path& gameDir, Manifest& man)
{
  std::error_code ec;
  // NOTE: there used to be a blanket "if gameDir already has a modloader.asi,
  // skip this whole function" gate here. That was too coarse: a profile's own
  // NATIVE mod can be a bare modloader.asi with no companion modloader/.data/
  // (config.ini/plugins.ini/language files) -- e.g. import-build's classifier
  // recognizes "some *.asi named modloader" and adopts just that one file, it
  // doesn't fabricate the .data folder Mod Loader itself needs to run. With the
  // old gate, enabling that mod made deployModloaderRuntime() no-op ENTIRELY --
  // not just skip the duplicate modloader.asi, but also skip CLEO/_ESSENTIALS/
  // the ASI loader DLL/.data -- silently leaving an incomplete, config-less
  // modloader.asi installed alone, which Mod Loader does not tolerate (crash on
  // start). The fix: let modloader.asi flow through the SAME per-file dedup
  // logic below as every other runtime file (exact-path match against
  // man.files) -- a mod's own copy still wins and ours is skipped for that one
  // file, but everything else our bundle provides still fills in the gaps.
  const fs::path rt = modloaderRuntimeDir();
  if (!fs::exists(rt / "modloader.asi", ec))
    return false;  // nothing to install; the UI warns via hasModloaderRuntime()

  // Cheap identity check so a large bundled file (e.g. gta_sa.exe) that is already
  // installed isn't re-copied every deploy.
  auto sameFile = [](const fs::path& a, const fs::path& b) {
    std::error_code e;
    if (fs::file_size(a, e) != fs::file_size(b, e) || e)
      return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb)
      return false;
    return std::equal(std::istreambuf_iterator<char>(fa),
                      std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb));
  };

  // Whether we actually installed/changed at least one file this call -- as
  // opposed to every file being deduped away (already present, or a mod's own
  // copy winning). deploy() ties the automatic exe swap to this: swapping the
  // exe in only makes sense when WE are actually the one providing (part of)
  // the Mod Loader runtime.
  bool installedAnything = false;

  for (const auto& de : fs::recursive_directory_iterator(rt, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), rt, ec);
    if (ec || rel.empty())
      continue;
    // The game exe is NOT part of the blanket runtime install; it is deployed
    // only when the user enables "replace game exe" (handled separately in deploy).
    if (toLower(rel.filename().string()) == "gta_sa.exe")
      continue;

    // The _ESSENTIALS bundle (SilentPatch, two widescreen fixes, FramerateVigilante,
    // RunDLL32 Fix, Windowed Mode) is a separate, independently-toggleable layer on
    // top of the Mod Loader core: a profile that already curates its own equivalent
    // fixes doesn't want our copies installed alongside them fighting over the same
    // D3D9 hooks/FOV patches (reproduced cause of a black-world+torn-menu crash).
    if (!m_enableEssentials) {
      const std::string relLower = toLower(rel.generic_string());
      if (relLower.rfind("modloader/_essentials/", 0) == 0)
        continue;
    }

    // The top-level gate above only checks modloader.asi -- a profile can still
    // have an enabled NATIVE mod that already placed some OTHER runtime file
    // this deploy (its own CLEO.asi, _noDEP.asi, bass.dll, ...; e.g. import-build's
    // "leftover of the build" mod for a pre-built compilation with its own
    // calibrated loader set). Respect it file-by-file, same reasoning as the
    // gta_sa.exe check in deploy(): forcing OUR version of a loader/CLEO/DEP-
    // bypass file in over one a mod already brought risks running two
    // mismatched builds against each other, and is a well-known cause of the
    // "Runtime Error! ... requested the Runtime to terminate it in an unusual
    // way" crash. Also avoids a second manifest entry for the same path, which
    // would otherwise corrupt rollback the same way the gta_sa.exe case did.
    const std::string relKey = toLower(rel.generic_string());
    const bool modAlreadyDeployedThis =
        std::any_of(man.files.begin(), man.files.end(), [&](const ManifestFile& f) {
          return toLower(f.rel) == relKey;
        });
    if (modAlreadyDeployedThis)
      continue;

    // A same-named .asi plugin already deployed ANYWHERE this run (the game
    // root, or inside a mod's own modloader/<name>/ folder at a DIFFERENT path
    // than ours) is almost certainly the exact same plugin the profile's own
    // mods already brought -- our bundled _ESSENTIALS ships SilentPatchSA.asi,
    // GTASA.WidescreenFix.asi and wshps.asi (Widescreen HOR+) under its OWN
    // "modloader/_ESSENTIALS/<name>/" paths, which do NOT path-match a pack's
    // own copies living at e.g. "modloader/SilentPatch/SilentPatchSA.asi" or a
    // root "wshps.asi" -- so the exact-path check above misses this case. Two
    // copies of the same exe-patching/D3D9-hooking ASI both loading is a
    // well-known cause of exactly the reported corruption: a black 3D world
    // (the world renderer never draws) with the 2D HUD still showing, and a
    // garbled/torn-looking front end menu.
    if (toLower(rel.extension().string()) == ".asi") {
      const std::string base = toLower(rel.filename().string());
      const bool sameAsiElsewhere =
          std::any_of(man.files.begin(), man.files.end(), [&](const ManifestFile& f) {
            return toLower(fs::path(f.rel).filename().string()) == base;
          });
      if (sameAsiElsewhere)
        continue;
    }

    const fs::path target = gameDir / rel;

    // Already present and identical -> nothing to do (and nothing to undo).
    if (fs::exists(target, ec) && sameFile(de.path(), target))
      continue;

    ensureParentDirs(gameDir, rel, man.createdDirs);

    ManifestFile mf;
    mf.rel = rel.generic_string();
    mf.mod = "(modloader-runtime)";

    if (fs::exists(target, ec)) {
      const fs::path backup = backupDir() / rel;
      fs::create_directories(backup.parent_path());
      moveFile(target, backup);
      mf.hadBackup = true;
    }
    try {
      fs::create_hard_link(de.path(), target);
      mf.method = "hardlink";
    } catch (const fs::filesystem_error&) {
      fs::copy_file(de.path(), target, fs::copy_options::overwrite_existing);
      mf.method = "copy";
    }
    man.files.push_back(std::move(mf));
    installedAnything = true;
  }
  return installedAnything;
}

bool App::deploySampRuntime(const fs::path& gameDir, Manifest& man)
{
  std::error_code ec;
  const fs::path rt = sampRuntimeDir();
  if (!fs::exists(rt / "samp.exe", ec))
    return false;  // nothing to install; the UI warns via hasSampRuntime()

  // Cheap identity check so large files aren't re-copied every deploy.
  auto sameFile = [](const fs::path& a, const fs::path& b) {
    std::error_code e;
    if (fs::file_size(a, e) != fs::file_size(b, e) || e)
      return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb)
      return false;
    return std::equal(std::istreambuf_iterator<char>(fa),
                      std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb));
  };

  bool installedAny = false;
  for (const auto& de : fs::recursive_directory_iterator(rt, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), rt, ec);
    if (ec || rel.empty())
      continue;

    // An enabled mod already deployed a file at this exact path this run --
    // e.g. a hand-imported SA-MP mod that got some but not all of its files
    // right (the very reason this bundled runtime exists), or a mod
    // deliberately overriding a piece of the client. Fill in only what is
    // actually missing; never fight a mod for a path it already won.
    const std::string relKey = toLower(rel.generic_string());
    const bool modAlreadyDeployedThis =
        std::any_of(man.files.begin(), man.files.end(), [&](const ManifestFile& f) {
          return toLower(f.rel) == relKey;
        });
    if (modAlreadyDeployedThis)
      continue;

    const fs::path target = gameDir / rel;

    // Already present and identical -> nothing to do (and nothing to undo).
    if (fs::exists(target, ec) && sameFile(de.path(), target)) {
      installedAny = true;
      continue;
    }

    ensureParentDirs(gameDir, rel, man.createdDirs);

    ManifestFile mf;
    mf.rel = rel.generic_string();
    mf.mod = "(samp-runtime)";

    if (fs::exists(target, ec)) {
      const fs::path backup = backupDir() / rel;
      fs::create_directories(backup.parent_path());
      moveFile(target, backup);
      mf.hadBackup = true;
    }
    try {
      fs::create_hard_link(de.path(), target);
      mf.method = "hardlink";
    } catch (const fs::filesystem_error&) {
      fs::copy_file(de.path(), target, fs::copy_options::overwrite_existing);
      mf.method = "copy";
    }
    man.files.push_back(std::move(mf));
    installedAny = true;
  }
  return installedAny;
}

void App::rollbackLoose(const Manifest& man, const fs::path& gameDir)
{
  // Remove our files; restore originals where we backed them up. Each file is
  // independent, so a failure restoring one (e.g. a persistent lock moveFile()'s
  // own retries couldn't ride out) must not abort the rest of the loop -- every
  // other file, including anything that comes after it (like a replaced
  // gta_sa.exe or Mod Loader DLL), still needs its restore attempt. Failures are
  // collected and reported together once every file has been processed.
  std::vector<std::string> failed;
  for (const auto& f : man.files) {
    const fs::path rel    = fs::path(f.rel);
    const fs::path target = gameDir / rel;
    std::error_code ec;
    fs::remove(target, ec);
    if (f.hadBackup) {
      const fs::path backup = backupDir() / rel;
      if (fs::exists(backup, ec)) {
        try {
          moveFile(backup, target);
        } catch (const std::exception& e) {
          failed.push_back(f.rel + ": " + e.what());
        }
      }
    }
  }

  // Remove directories we created, deepest first, only if empty.
  std::vector<std::string> dirs = man.createdDirs;
  std::sort(dirs.begin(), dirs.end(),
            [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  for (const auto& d : dirs) {
    const fs::path abs = gameDir / fs::path(d);
    std::error_code ec;
    if (fs::exists(abs, ec) && fs::is_empty(abs, ec))
      fs::remove(abs, ec);
  }

  // Prune the (now empty) backup tree -- but only once every file has actually
  // been restored. If something is still stuck (persistent lock), leave its
  // backup on disk so a retried rollback can still recover it, and report the
  // failure instead of silently declaring the rollback complete.
  if (failed.empty()) {
    std::error_code ec;
    if (fs::exists(backupDir(), ec))
      fs::remove_all(backupDir(), ec);
  } else {
    std::string msg = "failed to restore " + std::to_string(failed.size()) +
                      " file(s) from backup:";
    for (const auto& f : failed)
      msg += "\n  " + f;
    throw std::runtime_error(msg);
  }
}

void App::restoreImg(const std::string& rel, const fs::path& gameDir)
{
  const fs::path pristine = imgPristineDir() / fs::path(rel);
  const fs::path imgAbs   = gameDir / fs::path(rel);
  std::error_code ec;
  if (fs::exists(pristine, ec))
    fs::copy_file(pristine, imgAbs, fs::copy_options::overwrite_existing, ec);
}

void App::restoreSfx(const std::string& rel, const fs::path& gameDir)
{
  const fs::path pristine = sfxPristineDir() / fs::path(rel);
  const fs::path abs      = gameDir / fs::path(rel);
  std::error_code ec;
  if (fs::exists(pristine, ec))
    fs::copy_file(pristine, abs, fs::copy_options::overwrite_existing, ec);
}

void App::cleanOrphansAgainstBaseline(const fs::path& gameDir)
{
  if (!m_manageGenerated || !hasBaseline())
    return;
  const Baseline base = loadBaseline(baselinePath());
  if (base.empty())
    return;

  // Names of IMG archives the baseline knows about. IMG containers (gta3.img,
  // player.img, ...) are tracked per INTERNAL ENTRY in base.img (keys like
  // "gta3.img::churchlite.dff"), never as a whole-file path in base.loose --
  // so without this, every .img container in the game folder would look like
  // an unrecognized foreign file below and get deleted outright, wiping the
  // game's models/animations/scripts wholesale on every rollback.
  std::set<std::string> knownImgNames;
  for (const auto& kv : base.img) {
    const auto pos = kv.first.find("::");
    if (pos != std::string::npos)
      knownImgNames.insert(kv.first.substr(0, pos));
  }

  std::vector<fs::path> orphans;
  std::error_code ec;
  for (const auto& de : fs::recursive_directory_iterator(gameDir, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), gameDir, ec);
    if (ec || rel.empty())
      continue;
    if (base.loose.count(lowerAscii(rel.generic_string())))
      continue;  // known vanilla file -- nothing to restore differing content to, leave as-is
    if (knownImgNames.count(lowerAscii(de.path().filename().string())))
      continue;  // known vanilla IMG archive -- tracked per-entry, not whole-file
    orphans.push_back(de.path());
  }

  std::set<fs::path> dirsToCheck;
  for (const auto& p : orphans) {
    fs::remove(p, ec);
    dirsToCheck.insert(p.parent_path());
  }

  // Prune directories left empty by the removals, walking up toward gameDir.
  std::vector<fs::path> ordered(dirsToCheck.begin(), dirsToCheck.end());
  std::sort(ordered.begin(), ordered.end(), [](const fs::path& a, const fs::path& b) {
    return a.generic_string().size() > b.generic_string().size();
  });
  for (fs::path dir : ordered) {
    while (dir != gameDir && dir.generic_string().size() > gameDir.generic_string().size()) {
      std::error_code ec2;
      if (fs::exists(dir, ec2) && fs::is_empty(dir, ec2)) {
        fs::remove(dir, ec2);
        dir = dir.parent_path();
      } else {
        break;
      }
    }
  }
}

void App::rollback()
{
  requireInit();
  if (!isDeployed())
    throw std::runtime_error("nothing is deployed");
  const Manifest man     = readJson(manifestPath()).get<Manifest>();
  const fs::path gameDir = man.gameDir;

  rollbackLoose(man, gameDir);
  for (const auto& rel : man.imgFiles)
    restoreImg(rel, gameDir);
  for (const auto& rel : man.sfxPaks)
    restoreSfx(rel, gameDir);
  if (man.sfxConfig)
    restoreSfx("audio/CONFIG/BankLkup.dat", gameDir);

  // With "keep the game folder clean" on, also sweep up anything left in the
  // folder that predates GMM's own deploy/rollback tracking entirely (see
  // cleanOrphansAgainstBaseline()) -- files rollbackLoose() above just handled
  // are already gone from disk by this point, so this only ever catches
  // genuine pre-existing leftovers.
  cleanOrphansAgainstBaseline(gameDir);

  std::error_code ec;
  fs::remove(manifestPath(), ec);
}

void App::setPriority(const std::string& modId, int priority)
{
  requireInit();
  if (m_activeProfile.empty())
    throw std::runtime_error("no active profile; run `gtamm profile-use <name>`");
  const auto mod = findMod(modId);
  if (!mod)
    throw std::runtime_error("no such mod in pool: " + modId);
  const std::string id = mod->id;  // canonical

  Profile p = loadProfile(m_activeProfile);
  auto it   = std::find_if(p.entries.begin(), p.entries.end(),
                           [&](const ProfileEntry& e) { return e.modId == id; });
  if (it == p.entries.end()) {
    ProfileEntry e;
    e.modId    = id;
    e.enabled  = true;
    e.priority = priority;
    p.entries.push_back(e);
  } else {
    it->priority = priority;
  }
  saveProfile(p);
}

void App::setActiveProfileEntries(const std::vector<ProfileEntry>& entries)
{
  requireInit();
  if (m_activeProfile.empty())
    throw std::runtime_error("no active profile");
  Profile p = loadProfile(m_activeProfile);
  p.entries = entries;
  saveProfile(p);
}

namespace {

// Where the real User Files folder is parked while a profile's store is swapped
// in (a sibling of the original, so the rename stays on the same volume).
fs::path savesStashPath(const fs::path& userFiles)
{
  return userFiles.parent_path() / (userFiles.filename().wstring() + L".gmm-backup");
}

// Move a directory tree: fast rename, falling back to recursive copy + delete
// when rename can't work (e.g. crossing volumes).
void moveTree(const fs::path& from, const fs::path& to)
{
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec)
    return;
  ec.clear();
  fs::copy(from, to, fs::copy_options::recursive, ec);
  if (ec)
    throw std::runtime_error("cannot move '" + from.string() + "' to '" + to.string() +
                             "'");
  fs::remove_all(from, ec);
}

}  // namespace

void App::applyProfileSaves()
{
  const fs::path link  = userfiles::gtaUserFilesDir();
  const fs::path stash = savesStashPath(link);
  const fs::path target = profileSavesDir(m_activeProfile);
  fs::create_directories(target);

  std::error_code ec;
  bool stashed = false;
  if (userfiles::isReparsePoint(link)) {
    // A leftover junction (recoverSaves should have cleared it, but be safe).
    userfiles::removeJunction(link);
  } else if (fs::exists(link, ec)) {
    // A real User Files folder: park it aside so the original is preserved.
    if (fs::exists(stash, ec))
      throw std::runtime_error("saves backup already exists: " + stash.string());
    moveTree(link, stash);
    stashed = true;
  }

  try {
    userfiles::createJunction(link, target);
  } catch (...) {
    if (stashed) {
      std::error_code e2;
      fs::rename(stash, link, e2);  // best-effort restore of the original
    }
    throw;
  }

  // Record the swap so a crash mid-session can be undone on next startup.
  writeJson(savesManifestPath(), Json{{"profile", m_activeProfile}, {"stashed", stashed}});
}

void App::restoreProfileSaves()
{
  std::error_code ec;
  if (!fs::exists(savesManifestPath(), ec))
    return;

  bool stashed = true;
  try {
    stashed = readJson(savesManifestPath()).value("stashed", true);
  } catch (...) {
    stashed = true;
  }

  // Recompute the paths rather than trusting stored strings (avoids any
  // encoding round-trip on the Documents path).
  const fs::path link  = userfiles::gtaUserFilesDir();
  const fs::path stash = savesStashPath(link);

  if (userfiles::isReparsePoint(link))
    userfiles::removeJunction(link);

  if (stashed && fs::exists(stash, ec))
    moveTree(stash, link);

  fs::remove(savesManifestPath(), ec);
}

void App::recoverSaves()
{
  std::error_code ec;
  if (!fs::exists(savesManifestPath(), ec))
    return;
  try {
    restoreProfileSaves();
  } catch (...) {
    // Best effort: never let a stale saves manifest block startup.
  }
}

bool App::hasDefaultSettingsTemplate() const
{
  std::error_code ec;
  return fs::exists(defaultSettingsTemplatePath(), ec);
}

void App::saveCurrentSettingsAsTemplate()
{
  requireInit();
  const fs::path src = (m_manageSaves && !m_activeProfile.empty())
                          ? profileSavesDir(m_activeProfile) / "gta_sa.set"
                          : userfiles::gtaUserFilesDir() / "gta_sa.set";
  std::error_code ec;
  if (!fs::exists(src, ec))
    throw std::runtime_error(
        "no gta_sa.set found yet -- launch the game, set your resolution/controls "
        "once, exit, then try again");

  const fs::path dst = defaultSettingsTemplatePath();
  fs::create_directories(dst.parent_path(), ec);
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec)
    throw std::runtime_error("cannot save settings template: " + ec.message());
}

void App::clearDefaultSettingsTemplate()
{
  requireInit();
  std::error_code ec;
  fs::remove(defaultSettingsTemplatePath(), ec);
}

void App::seedDefaultSettingsIfMissing()
{
  std::error_code ec;
  const fs::path tmpl = defaultSettingsTemplatePath();
  if (!fs::exists(tmpl, ec))
    return;  // nothing captured yet

  const fs::path dir = (m_manageSaves && !m_activeProfile.empty())
                          ? profileSavesDir(m_activeProfile)
                          : userfiles::gtaUserFilesDir();
  fs::create_directories(dir, ec);
  const fs::path dst = dir / "gta_sa.set";
  if (fs::exists(dst, ec))
    return;  // already has settings (real or previously seeded) -- never overwrite
  fs::copy_file(tmpl, dst, ec);
  if (ec)
    throw std::runtime_error("cannot seed default settings template: " + ec.message());
}

std::set<std::string> App::restoreGenerated(const std::string& profile,
                                            const fs::path& gameDir)
{
  std::set<std::string> placed;
  const fs::path stash = generatedDir(profile);
  std::error_code ec;
  if (!fs::exists(stash, ec))
    return placed;
  for (const auto& de : fs::recursive_directory_iterator(stash, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), stash, ec);
    if (ec)
      continue;
    const fs::path target = gameDir / rel;
    if (fs::exists(target, ec))
      continue;  // never clobber a deployed/vanilla file
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(de.path(), target, fs::copy_options::overwrite_existing, ec);
    if (!ec)
      placed.insert(rel.generic_string());
  }
  return placed;
}

void App::snapshotTree(const fs::path& gameDir, std::set<std::string>& files,
                       std::set<std::string>& dirs) const
{
  std::error_code ec;
  for (const auto& de : fs::recursive_directory_iterator(gameDir, ec)) {
    const fs::path rel = fs::relative(de.path(), gameDir, ec);
    if (ec)
      continue;
    if (de.is_directory(ec))
      dirs.insert(rel.generic_string());
    else if (de.is_regular_file(ec))
      files.insert(rel.generic_string());
  }
}

void App::captureGenerated(const std::string& profile, const fs::path& gameDir,
                           const std::set<std::string>& baseFiles,
                           const std::set<std::string>& baseDirs,
                           const std::set<std::string>& restored)
{
  const fs::path stash = generatedDir(profile);
  std::error_code ec;

  // 1. Collect files that appeared/changed this session (new, or ones we
  // restored) -- never touch deployed/vanilla files. Collect first, move after
  // (mutating the tree mid-iteration is unsafe).
  std::vector<std::pair<fs::path, std::string>> capture;
  for (const auto& de : fs::recursive_directory_iterator(gameDir, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), gameDir, ec);
    if (ec)
      continue;
    const std::string r = rel.generic_string();
    if (baseFiles.count(r) && !restored.count(r))
      continue;  // part of the deployed/vanilla state
    capture.emplace_back(de.path(), r);
  }
  for (const auto& [src, r] : capture) {
    const fs::path dst = stash / fs::path(r);
    fs::create_directories(dst.parent_path(), ec);
    fs::remove(dst, ec);  // overwrite a previous capture
    fs::rename(src, dst, ec);
    if (ec) {
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
      fs::remove(src, ec);
    }
  }

  // 2. Remove now-empty directories the session created (not in the snapshot),
  // deepest first, so the game folder matches its post-deploy layout again.
  std::vector<fs::path> dirs;
  for (const auto& de : fs::recursive_directory_iterator(gameDir, ec))
    if (de.is_directory(ec))
      dirs.push_back(de.path());
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return a.generic_string().size() > b.generic_string().size();
  });
  for (const fs::path& d : dirs) {
    const fs::path rel = fs::relative(d, gameDir, ec);
    if (ec || baseDirs.count(rel.generic_string()))
      continue;
    if (fs::is_empty(d, ec))
      fs::remove(d, ec);
  }
}

App::LaunchTarget App::resolveLaunchTarget(const std::string& exeName) const
{
  const fs::path gameDir = requireGameDir();

  auto resolve = [&](const std::string& name) -> fs::path {
    if (name.empty())
      return gameDir / "gta_sa.exe";
    if (fs::path(name).is_absolute())
      return fs::path(name);
    return gameDir / name;
  };

  LaunchTarget t;
  // SA-MP settings of the active profile (if any).
  if (!m_activeProfile.empty() && fs::exists(profilePath(m_activeProfile))) {
    t.samp     = loadProfile(m_activeProfile).samp;
    t.haveSamp = true;
  }

  // Launch through SA-MP when the profile enables it and the caller asked for the
  // default game exe (empty or gta_sa.exe) — picking a *different* exe in the run
  // selector is treated as an explicit override and launched as-is. Choosing
  // samp.exe directly also routes through SA-MP. Either way the injector exits
  // within a second, so we must wait on the gta_sa.exe it spawns instead —
  // otherwise we'd roll the deployed mods back out from under the live game.
  const std::string exeBase = toLower(fs::path(exeName).filename().string());
  const bool defaultExe = exeName.empty() || exeBase == "gta_sa.exe";

  if (t.haveSamp && t.samp.enabled && defaultExe) {
    t.viaSamp = true;
    t.exe     = resolve(t.samp.exe.empty() ? "samp.exe" : t.samp.exe);
  } else {
    t.exe = resolve(exeName);
    if (toLower(t.exe.filename().string()) == "samp.exe")
      t.viaSamp = true;
  }

  if (t.viaSamp) {
    if (t.haveSamp && !t.samp.server.empty()) {
      // samp.exe's own command-line convention is a single "host:port" token
      // (colon-joined, not space-separated -- e.g. the shortcuts SA-MP's own
      // docs tell you to make are "samp.exe 127.0.0.1:7777"), optionally
      // followed by the server password as a second token. Server/host is
      // ASCII (IP/hostname), so a plain widening is enough.
      const std::string hostPort =
          t.samp.server + ":" + std::to_string(t.samp.port);
      t.args.assign(hostPort.begin(), hostPort.end());
      if (!t.samp.password.empty()) {
        const std::wstring pass(t.samp.password.begin(), t.samp.password.end());
        t.args += L" " + pass;
      }
    }
  }
  return t;
}

int App::launchGame(const std::string& exeName)
{
  requireInit();
  const fs::path gameDir = requireGameDir();
  const LaunchTarget t = resolveLaunchTarget(exeName);
  const SampConfig& samp = t.samp;
  const bool haveSamp    = t.haveSamp;

  if (t.viaSamp) {
    // The nickname is read by samp.exe from the registry, not the command line.
    if (haveSamp)
      setSampNick(samp.nick);
    // samp.exe locates gta_sa.exe via its OWN remembered registry path (set the
    // first time you point it at a game through its "locate gta_sa.exe" file
    // picker), not by looking next to itself or in the working directory. A
    // portable/mod-managed install can legitimately live somewhere different
    // each time, so keep that registry value pointed at THIS session's actual
    // exe -- otherwise samp.exe shows "GTA: San Andreas executable not found.
    // Please locate it now." instead of launching.
    setSampGtaExePath(gameDir / "gta_sa.exe");
  }

  // Tell the running Steam client we ARE Grand Theft Auto: San Andreas for as
  // long as the game is up, so it shows "in-game" and counts the playtime
  // against the real store entry (see Steam.h). Held by an RAII object, so the
  // session is released on every path out of here, exception included -- GMM
  // must never stay stuck showing as in-game after the game has closed. A
  // session that can't be established (Steam closed, game not owned, no
  // steam_api64.dll) is not a reason to refuse to launch: the game starts
  // anyway, just without the status.
  steam::Presence presence;
  if (m_steamIntegration) {
    // Try each candidate DLL until one opens a session: a copy borrowed from
    // a Steam library can turn out to be an emulator shim that refuses. The
    // one that worked is remembered, so later launches skip straight to it
    // (and never re-run the library scan).
    for (const fs::path& dll : steamApiDllCandidates()) {
      if (!presence.begin(steam::kGtaSanAndreasAppId, dll))
        continue;
      if (m_steamApiDll != dll.string()) {
        m_steamApiDll = dll.string();
        saveConfig();
      }
      break;
    }
  }

  if (t.viaSamp)
    return launchSampAndWait(t.exe, t.args, gameDir);

  return launchAndWait(t.exe, gameDir);
}

fs::path App::steamRuntimeDir() const
{
  // Same layout as the Mod Loader / SA-MP runtimes: a bundle folder next to
  // the program (where the embedded copy is unpacked), with the per-instance
  // data dir as the writable fallback.
  std::error_code ec;
  const fs::path bundled = executableDir() / "runtime-steam";
  if (fs::exists(bundled / steam::apiDllName(), ec))
    return bundled;
  const fs::path local = m_dataDir / "steam-runtime";
  if (fs::exists(local / steam::apiDllName(), ec))
    return local;
  return bundled;
}

std::vector<fs::path> App::steamApiDllCandidates()
{
  std::error_code ec;
  std::vector<fs::path> out;
  auto add = [&](const fs::path& p) {
    if (p.empty() || !fs::exists(p, ec))
      return;
    if (std::find(out.begin(), out.end(), p) == out.end())
      out.push_back(p);
  };

  // 1. Whatever was pinned by hand or settled on by a previous detection.
  if (!m_steamApiDll.empty())
    add(fs::path(m_steamApiDll));

  // 2. The bundle folders (a copy the user deliberately put with the program).
  const std::string name = steam::apiDllName();
  for (const fs::path& dir :
       {steamRuntimeDir(), m_dataDir / "steam-runtime", executableDir()})
    add(dir / name);

  // 3. Borrow one from a game already installed through Steam. Only when the
  // first two came up empty, memoized for this process, and persisted by
  // launchGame() once a session actually opens with one of them -- so the walk
  // happens at most once per machine in practice.
  if (out.empty()) {
    if (!m_steamScanned) {
      m_steamScanHits = steam::findApiDlls();
      m_steamScanned  = true;
    }
    for (const fs::path& p : m_steamScanHits)
      add(p);
  }

  return out;
}

fs::path App::steamApiDll()
{
  const std::vector<fs::path> all = steamApiDllCandidates();
  return all.empty() ? fs::path() : all.front();
}

void App::setSteamApiDll(const std::string& path)
{
  requireInit();
  m_steamApiDll = path;
  saveConfig();
}

App::SteamStatus App::steamStatus()
{
  SteamStatus st;
  st.installed = steam::isInstalled();
  st.running   = steam::isRunning();
  const fs::path dll = steamApiDll();
  st.hasApiDll = !dll.empty();
  if (st.hasApiDll) {
    st.apiDllPath = dll.string();
    // Distinguish "we borrowed this from one of your Steam games" from "you
    // put it there yourself" -- the GUI says so, because a borrowed copy can
    // disappear when that game is uninstalled.
    const fs::path steamRoot = steam::installPath();
    if (!steamRoot.empty()) {
      for (const fs::path& lib : steam::libraryFolders()) {
        const std::string prefix = (lib / "steamapps" / "common").string();
        if (st.apiDllPath.rfind(prefix, 0) == 0) {
          st.apiDllFromSteam = true;
          break;
        }
      }
    }
  } else {
    // Report where it should go rather than an empty string -- that path is
    // exactly what the GUI needs to tell the user.
    st.apiDllPath = (steamRuntimeDir() / steam::apiDllName()).string();
  }
  st.ready = st.installed && st.hasApiDll;
  return st;
}

int App::play(bool rollbackAfter, const std::string& exeName)
{
  requireInit();
  seedDefaultSettingsIfMissing();
  deploy();

  const fs::path gameDir = requireGameDir();
  const bool manageGen   = m_manageGenerated && !m_activeProfile.empty();

  // Snapshot the clean post-deploy folder FIRST, then restore the profile's
  // previously-captured generated files on top. Anything not in the snapshot at
  // exit (restored files included) is moved back out, and dirs created for them
  // are pruned, so the folder returns to exactly its post-deploy state.
  std::set<std::string> baseFiles, baseDirs, restored;
  if (manageGen) {
    snapshotTree(gameDir, baseFiles, baseDirs);
    restored = restoreGenerated(m_activeProfile, gameDir);
  }
  auto captureGen = [&] {
    if (manageGen)
      captureGenerated(m_activeProfile, gameDir, baseFiles, baseDirs, restored);
  };

  // Steam integration is handled entirely inside launchGame() now (it starts
  // the game through steam://rungameid so Steam injects its overlay). Earlier
  // versions instead wrote a steam_appid.txt here for the session; that file
  // only ever meant anything to a game that calls SteamAPI_Init() itself, so
  // it did nothing at all for retail GTA SA -- it is not written any more.
  // Sweep away a leftover from one of those versions so manageGenerated
  // doesn't archive it as if a mod had created it.
  {
    std::error_code ec2;
    fs::remove(gameDir / "steam_appid.txt", ec2);
  }

  // Whether this session launches through SA-MP -- same check launchGame()
  // makes internally, duplicated here so play() knows whether to sweep up a
  // leftover samp.exe once the session ends (see killSampLeftovers below).
  const bool sampEnabled = !m_activeProfile.empty() &&
                           fs::exists(profilePath(m_activeProfile)) &&
                           loadProfile(m_activeProfile).samp.enabled;
  // samp.exe is supposed to inject and exit within a second of launching the
  // game (see launchSampAndWait); if it got stuck instead, it lingers holding
  // SA-MP's shared registry/memory state, and the NEXT samp.exe launched by a
  // later Play collides with it and can crash (EAccessViolation) instead of
  // connecting. Force-killing any leftover copy here guarantees the next
  // launch starts clean. No-op if SA-MP wasn't used, or if samp.exe already
  // exited normally (nothing left to find).
  auto killSampLeftovers = [&] {
    if (sampEnabled)
      killProcessesByName(L"samp.exe");
  };

  int code          = 0;
  bool savesApplied = false;
  try {
    // Swap in the active profile's saves/settings for the session (if enabled).
    if (m_manageSaves && !m_activeProfile.empty()) {
      applyProfileSaves();
      savesApplied = true;
    }
    code = launchGame(exeName);
  } catch (...) {
    // Never leave the game folder deployed or the User Files redirected if the
    // launch failed.
    killSampLeftovers();
    if (savesApplied) {
      try {
        restoreProfileSaves();
      } catch (...) {
      }
    }
    try {
      captureGen();
    } catch (...) {
    }
    if (rollbackAfter)
      rollback();
    throw;
  }

  killSampLeftovers();
  if (savesApplied)
    restoreProfileSaves();
  // Stash generated files and clean the game folder BEFORE rollback strips the
  // deployment, so only genuinely new files are captured.
  captureGen();
  if (rollbackAfter)
    rollback();
  return code;
}

}  // namespace gtamm
