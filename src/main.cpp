#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "App.h"
#include "Img.h"
#include "Unpack.h"
#include "UserFiles.h"
#include "Version.h"

namespace fs = std::filesystem;
using gtamm::App;
using gtamm::ArchivePasswordRequired;
namespace userfiles = gtamm::userfiles;

namespace {

const char* kUsage = R"(gtamm - GTA San Andreas mod manager (Stage 1: core + CLI)

Usage:
  gtamm [--data <dir>] <command> [args]

Global:
  --data <dir>            data directory (default: ./gtamm-data)

Commands:
  version                         print the program version and exit
  init [--game <path>]            create the data directory and config
  status                          show config, mod pool and active profile
  import <folder|archive> [--name <n>] [--modloader] [--password <pw>]
                                  import a folder or archive (zip/7z/rar);
                                  --modloader deploys it under modloader/<name>/;
                                  --password unlocks an encrypted archive
  import-build <gameFolder> [--profile <n>] [--refresh]
                                  diff a modded GTA SA folder against vanilla and
                                  import its changes as toggleable mods + a profile
  baseline-status                 is a vanilla baseline cached for this instance?
  baseline-build                  (re)build it from the CURRENT game folder --
                                  only do this while it's genuinely vanilla
  export-mod <modId> <out.zip>      export one pool mod to a GMM zip (re-imports verbatim)
  export-build <profile> <out.zip> [--saves] [--settings]
                                  export a build (profile + its mods, no game) to zip
  import-build-zip <build.zip> <profile>
                                  import a build zip into a new/existing profile
  mods                            list mods in the pool
  mod-remove <id>                 remove a mod from the pool (and all profiles)
  mod-modloader <id> --on|--off   deploy a mod via Mod Loader (or natively)
  mod-lua-list <id>               list a mod's top-level moonloader/*.lua scripts
  mod-lua-split <id>              split those scripts into their own mods
  modloader-dir [<path>]          show/set the Mod Loader runtime folder
  essentials --on|--off           also install the bundled _ESSENTIALS fixes
  replace-exe --on|--off          replace gta_sa.exe with the bundled clean 1.0
  profile-list                    list profiles
  profile-create <name>           create a new profile
  profile-delete <name>           delete a profile
  profile-rename <old> <new>      rename a profile
  profile-copy <src> <dst> [--saves] [--settings]
                                  duplicate a profile (optionally its saves/settings)
  profile-use <name>              set the active profile
  enable <modId>                  enable a mod in the active profile
  disable <modId>                 disable a mod in the active profile
  order <modId> <priority>        set conflict priority (higher wins)
  conflicts                       show files claimed by multiple enabled mods
  deploy                          install the active profile into the game folder
  rollback                        undo the deployment, restoring the game folder
  deploy-status                   show what is currently deployed
  play [--exe <e>] [--no-rollback]  deploy, launch the game, wait, then roll back
  launch [--exe <e>]              launch the game (no deploy/rollback)
  saves                           show per-profile saves (User Files) status
  saves-on | saves-off            enable/disable per-profile saves redirection
  settings-template [save|clear]  show/capture/clear the default gta_sa.set seeded
                                  into a fresh profile/instance's first run
  samp [<profile>]                show a profile's SA-MP (multiplayer) settings
  samp-set [<profile>] [--on|--off] [--server <ip>] [--port <n>] [--nick <name>]
                                  [--password <pw>] [--exe <path>]   configure
                                  SA-MP launch
  clean                           show keep-game-folder-clean status
  clean-on | clean-off            enable/disable per-profile generated-file capture
  maps                            show experimental map auto-install status
  maps-on | maps-off              enable/disable map auto-install (.ide/.ipl -> gta.dat,
                                  new models -> gta3.img); OFF by default, can crash maps
  steam                           show Steam status (setting, client, steam_api)
  steam-on | steam-off            enable/disable showing the session in Steam as
                                  "Playing Grand Theft Auto: San Andreas"
                                  (requires the game to be OWNED on the signed-in
                                  Steam account); OFF by default

IMG utilities (operate on archive paths directly):
  img-list <img>                  list the entries of a VER2 IMG archive
  img-extract <img> <name> <out>  write one entry's bytes to a file
  img-inject <src> <out> <name> <file>  rebuild an IMG replacing/adding an entry

Notes:
  Priority: HIGHER wins conflicts (deployed on top). Loose files are hardlinked
  (copy fallback) with originals backed up; model/texture files (.dff/.txd/.col/
  .ifp) that belong to an IMG archive are injected into it (rebuilt from a cached
  pristine copy). rollback restores both. CLEO/ASI/Lua mods are loose files and
  work as-is when laid out relative to the game folder.
)";

// Extract `--flag value` from args, removing both tokens. Returns the value if
// present.
std::optional<std::string> takeFlag(std::vector<std::string>& args,
                                    const std::string& flag)
{
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == flag) {
      if (i + 1 >= args.size())
        throw std::runtime_error("missing value for " + flag);
      std::string value = args[i + 1];
      args.erase(args.begin() + i, args.begin() + i + 2);
      return value;
    }
  }
  return std::nullopt;
}

// Remove a boolean flag (e.g. --no-rollback) if present; return whether it was.
bool takeBool(std::vector<std::string>& args, const std::string& flag)
{
  auto it = std::find(args.begin(), args.end(), flag);
  if (it == args.end())
    return false;
  args.erase(it);
  return true;
}

std::string requireArg(const std::vector<std::string>& args, std::size_t index,
                       const std::string& what)
{
  if (index >= args.size())
    throw std::runtime_error("missing argument: " + what);
  return args[index];
}

void printStatus(App& app)
{
  std::cout << "data:    " << app.dataDir().string() << '\n';
  std::cout << "game:    " << (app.gamePath().empty() ? "<not set>" : app.gamePath())
            << '\n';
  const std::string active =
      app.activeProfile().empty() ? "<none>" : app.activeProfile();
  std::cout << "profile: " << active << "\n\n";

  const auto mods = app.mods();
  std::cout << "pool (" << mods.size() << " mods):\n";
  for (const auto& m : mods)
    std::cout << "  " << m.id << "  (" << m.name << ")\n";
  if (mods.empty())
    std::cout << "  <empty>\n";

  if (app.activeProfile().empty())
    return;

  // Show the active profile's load order: enabled mods, highest priority first.
  auto profile = app.loadProfile(app.activeProfile());
  std::sort(profile.entries.begin(), profile.entries.end(),
            [](const gtamm::ProfileEntry& a, const gtamm::ProfileEntry& b) {
              return a.priority > b.priority;
            });
  std::cout << "\nprofile '" << app.activeProfile() << "' load order (top wins):\n";
  bool any = false;
  for (const auto& e : profile.entries) {
    std::cout << "  [" << (e.enabled ? "x" : " ") << "] prio=" << e.priority << "  "
              << e.modId << '\n';
    any = true;
  }
  if (!any)
    std::cout << "  <no mods>\n";
}

int run(std::vector<std::string> args)
{
  // Global --data flag (default ./gtamm-data).
  fs::path dataDir = "gtamm-data";
  if (auto d = takeFlag(args, "--data"))
    dataDir = *d;

  if (args.empty()) {
    std::cout << kUsage;
    return 1;
  }

  const std::string cmd = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());
  App app(dataDir);

  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    std::cout << kUsage;
    return 0;
  }

  // IMG utility commands (operate on archive paths directly, no init needed).
  if (cmd == "img-list") {
    const auto img = requireArg(rest, 0, "<img>");
    const auto dir = gtamm::imgReadDirectory(img);
    std::cout << dir.size() << " entries in " << img << '\n';
    std::size_t shown = 0;
    for (const auto& e : dir) {
      std::cout << "  " << e.name << "  (" << e.size << " sectors)\n";
      if (++shown >= 25) {
        std::cout << "  ... (" << (dir.size() - shown) << " more)\n";
        break;
      }
    }
    return 0;
  }
  if (cmd == "img-extract") {
    const auto img  = requireArg(rest, 0, "<img>");
    const auto name = requireArg(rest, 1, "<entryName>");
    const auto out  = requireArg(rest, 2, "<outFile>");
    const auto dir  = gtamm::imgReadDirectory(img);
    auto it = std::find_if(dir.begin(), dir.end(), [&](const gtamm::ImgEntry& e) {
      return _stricmp(e.name.c_str(), name.c_str()) == 0;
    });
    if (it == dir.end())
      throw std::runtime_error("no such entry: " + name);
    const auto bytes = gtamm::imgReadEntry(img, *it);
    std::ofstream of(out, std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    std::cout << "extracted " << bytes.size() << " bytes to " << out << '\n';
    return 0;
  }
  if (cmd == "img-inject") {
    const auto src  = requireArg(rest, 0, "<srcImg>");
    const auto out  = requireArg(rest, 1, "<outImg>");
    const auto name = requireArg(rest, 2, "<entryName>");
    const auto file = requireArg(rest, 3, "<file>");
    gtamm::imgRebuild(src, out, {{name, file}});
    std::cout << "rebuilt " << out << " injecting '" << name << "'\n";
    return 0;
  }

  if (cmd == "version" || cmd == "--version" || cmd == "-v") {
    std::cout << "GMM " << gtamm::versionString() << "\n";
    return 0;
  }

  if (cmd == "init") {
    auto game = takeFlag(rest, "--game");
    app.init(game.value_or(""));
    std::cout << "initialized data directory at " << app.dataDir().string() << '\n';
    return 0;
  }

  // All remaining commands require an initialized data directory.
  if (!app.isInitialized()) {
    std::cerr << "error: '" << dataDir.string()
              << "' is not initialized; run `gtamm init` first\n";
    return 1;
  }

  if (cmd == "status") {
    printStatus(app);
    return 0;
  }
  if (cmd == "import") {
    auto name            = takeFlag(rest, "--name");
    auto password        = takeFlag(rest, "--password");
    const bool viaModloader = takeBool(rest, "--modloader");
    const auto src       = requireArg(rest, 0, "<folder|archive>");
    std::vector<App::ImportResult> results;
    try {
      results = fs::is_directory(src)
                    ? app.importFolderAsMods(src, name.value_or(""), viaModloader)
                    : app.importArchiveAsMods(src, name.value_or(""), viaModloader,
                                              password.value_or(""));
    } catch (const ArchivePasswordRequired&) {
      std::cerr << "error: archive is password-protected; pass --password <pw>\n";
      return 1;
    }
    if (results.size() > 1)
      std::cout << "pack split into " << results.size() << " mods:\n";
    for (const auto& r : results) {
      if (r.wasDuplicate)
        std::cout << "  already in pool as '" << r.mod.id
                  << "' (identical mod, not re-imported)\n";
      else
        std::cout << "  imported '" << r.mod.name << "' as mod id '" << r.mod.id
                  << "'\n";
    }
    return 0;
  }
  if (cmd == "baseline-status") {
    std::cout << (app.hasBaseline() ? "vanilla baseline: present\n"
                                    : "vanilla baseline: MISSING -- rollback's orphan cleanup "
                                      "(\"keep the game folder clean\") is a silent no-op until "
                                      "one exists; run `baseline-build` on a truly vanilla game "
                                      "folder\n");
    return 0;
  }
  if (cmd == "baseline-build") {
    app.refreshVanillaBaseline();
    std::cout << "vanilla baseline built from the current game folder\n";
    return 0;
  }
  if (cmd == "import-build") {
    App::ImportBuildOptions opts;
    if (auto p = takeFlag(rest, "--profile"))
      opts.profileName = *p;
    opts.refreshBaseline = takeBool(rest, "--refresh");
    const auto src = requireArg(rest, 0, "<gameFolder>");
    const auto r   = app.importBuild(src, opts);
    std::cout << "imported build into profile '" << r.profile << "': "
              << r.createdModIds.size() << " mods ("
              << r.looseChanged << " loose, " << r.imgChanged
              << " IMG entries changed; " << r.skipped << " ignored)\n";
    for (const auto& id : r.createdModIds)
      std::cout << "  + " << id << '\n';
    for (const auto& n : r.notes)
      std::cout << "  note: " << n << '\n';
    for (const auto& s : r.skippedFiles)
      std::cout << "  skipped: " << s.rel << "  (" << s.reason << ")\n";
    return 0;
  }
  if (cmd == "export-build") {
    const bool withSaves    = takeBool(rest, "--saves");
    const bool withSettings = takeBool(rest, "--settings");
    const auto profile = requireArg(rest, 0, "<profile>");
    const auto zip     = requireArg(rest, 1, "<out.zip>");
    app.exportBuild(profile, zip, withSaves, withSettings);
    std::cout << "exported build '" << profile << "' -> " << zip;
    if (withSaves || withSettings) {
      std::cout << " (with";
      if (withSaves)    std::cout << " saves";
      if (withSettings) std::cout << " settings";
      std::cout << ")";
    }
    std::cout << '\n';
    return 0;
  }
  if (cmd == "export-mod") {
    const auto modId = requireArg(rest, 0, "<modId>");
    const auto zip   = requireArg(rest, 1, "<out.zip>");
    app.exportMod(modId, zip);
    std::cout << "exported mod '" << modId << "' -> " << zip << '\n';
    return 0;
  }
  if (cmd == "import-build-zip") {
    const auto zip     = requireArg(rest, 0, "<build.zip>");
    const auto profile = requireArg(rest, 1, "<profile>");
    const auto r       = app.importBuildArchive(zip, profile);
    std::cout << "imported build into profile '" << r.profile << "': "
              << r.modsAdded << " mod(s) added, " << r.modsReused
              << " reused (deduped)\n";
    for (const auto& n : r.notes)
      std::cout << "  note: " << n << '\n';
    return 0;
  }
  if (cmd == "mods") {
    for (const auto& m : app.mods())
      std::cout << m.id << "  (" << m.name << ")"
                << (m.viaModloader ? "  [modloader]" : "") << '\n';
    return 0;
  }
  if (cmd == "mod-remove") {
    const auto id = requireArg(rest, 0, "<id>");
    app.removeMod(id);
    std::cout << "removed mod '" << id << "'\n";
    return 0;
  }
  if (cmd == "mod-modloader") {
    const bool on  = takeBool(rest, "--on");
    const bool off = takeBool(rest, "--off");
    const auto id  = requireArg(rest, 0, "<id>");
    if (on == off) {
      std::cerr << "error: pass exactly one of --on / --off\n";
      return 1;
    }
    app.setModViaModloader(id, on);
    std::cout << "mod '" << id << "' will deploy "
              << (on ? "via Mod Loader (modloader/<name>/)" : "natively") << '\n';
    return 0;
  }
  if (cmd == "mod-lua-list") {
    const auto id = requireArg(rest, 0, "<id>");
    for (const auto& rel : app.listModLuaScripts(id))
      std::cout << rel << '\n';
    return 0;
  }
  if (cmd == "mod-lua-split") {
    const auto id = requireArg(rest, 0, "<id>");
    const auto r  = app.splitMoonloaderScripts(id);
    std::cout << "created " << r.createdModIds.size() << " mod(s):\n";
    for (const auto& newId : r.createdModIds)
      std::cout << "  " << newId << '\n';
    std::cout << "updated " << r.profilesUpdated << " profile(s)\n";
    return 0;
  }
  if (cmd == "modloader-dir") {
    if (!rest.empty())
      app.setModloaderRuntimeDir(rest.front());  // set override (or "" to reset)
    std::cout << "Mod Loader runtime folder: "
              << app.modloaderRuntimeDir().string() << '\n'
              << "runtime available: " << (app.hasModloaderRuntime() ? "yes" : "no")
              << "\n(place modloader.asi + an ASI loader such as dinput8.dll there)\n";
    return 0;
  }
  if (cmd == "essentials") {
    const bool on  = takeBool(rest, "--on");
    const bool off = takeBool(rest, "--off");
    if (on == off) {
      std::cerr << "error: pass exactly one of --on / --off\n";
      return 1;
    }
    app.setEnableEssentials(on);
    std::cout << "install bundled _ESSENTIALS fixes (SilentPatch/widescreen/"
                 "FramerateVigilante/RunDLL32 Fix/Windowed Mode): "
              << (on ? "on" : "off") << '\n';
    return 0;
  }
  if (cmd == "replace-exe") {
    const bool on  = takeBool(rest, "--on");
    const bool off = takeBool(rest, "--off");
    if (on == off) {
      std::cerr << "error: pass exactly one of --on / --off\n";
      return 1;
    }
    app.setReplaceGameExe(on);
    std::cout << "replace game exe with bundled clean 1.0: " << (on ? "on" : "off")
              << (on && !app.hasBundledExe()
                      ? "  (WARNING: no bundled gta_sa.exe found in the runtime)"
                      : "")
              << '\n';
    return 0;
  }
  if (cmd == "profile-list") {
    for (const auto& n : app.profileNames()) {
      const bool active = (n == app.activeProfile());
      std::cout << (active ? "* " : "  ") << n << '\n';
    }
    return 0;
  }
  if (cmd == "profile-create") {
    const auto name = requireArg(rest, 0, "<name>");
    app.createProfile(name);
    std::cout << "created profile '" << name << "'\n";
    return 0;
  }
  if (cmd == "profile-delete") {
    const auto name = requireArg(rest, 0, "<name>");
    app.deleteProfile(name);
    std::cout << "deleted profile '" << name << "'\n";
    return 0;
  }
  if (cmd == "profile-rename") {
    const auto oldName = requireArg(rest, 0, "<old>");
    const auto newName = requireArg(rest, 1, "<new>");
    app.renameProfile(oldName, newName);
    std::cout << "renamed profile '" << oldName << "' to '" << newName << "'\n";
    return 0;
  }
  if (cmd == "profile-copy") {
    const auto src = requireArg(rest, 0, "<src>");
    const auto dst = requireArg(rest, 1, "<dst>");
    bool withSaves = false, withSettings = false;
    for (const auto& a : rest) {
      if (a == "--saves")
        withSaves = true;
      else if (a == "--settings")
        withSettings = true;
    }
    app.copyProfile(src, dst, withSaves, withSettings);
    std::cout << "copied profile '" << src << "' to '" << dst << "'"
              << (withSaves ? " +saves" : "") << (withSettings ? " +settings" : "")
              << "\n";
    return 0;
  }
  if (cmd == "profile-use") {
    const auto name = requireArg(rest, 0, "<name>");
    app.useProfile(name);
    std::cout << "active profile is now '" << name << "'\n";
    return 0;
  }
  if (cmd == "enable" || cmd == "disable") {
    const auto id = requireArg(rest, 0, "<modId>");
    app.setEnabled(id, cmd == "enable");
    std::cout << cmd << "d '" << id << "' in profile '" << app.activeProfile()
              << "'\n";
    return 0;
  }
  if (cmd == "order") {
    const auto id    = requireArg(rest, 0, "<modId>");
    const auto prioS = requireArg(rest, 1, "<priority>");
    const int prio   = std::stoi(prioS);
    app.setPriority(id, prio);
    std::cout << "set priority of '" << id << "' to " << prio << '\n';
    return 0;
  }
  if (cmd == "conflicts") {
    const auto conflicts = app.conflicts();
    if (conflicts.empty()) {
      std::cout << "no conflicts\n";
      return 0;
    }
    std::cout << conflicts.size() << " conflicting file(s):\n";
    for (const auto& c : conflicts) {
      std::cout << "  " << c.path << "\n      winner: " << c.winner << '\n';
      for (const auto& l : c.losers)
        std::cout << "      shadowed: " << l << '\n';
    }
    return 0;
  }
  if (cmd == "deploy") {
    app.deploy();
    const auto man = app.currentManifest();
    std::cout << "deployed profile '" << man.profile << "': " << man.files.size()
              << " file(s) into " << man.gameDir << '\n';
    return 0;
  }
  if (cmd == "rollback") {
    app.rollback();
    std::cout << "rolled back; game folder restored\n";
    return 0;
  }
  if (cmd == "deploy-status") {
    if (!app.isDeployed()) {
      std::cout << "not deployed\n";
      return 0;
    }
    const auto man = app.currentManifest();
    std::cout << "deployed profile: " << man.profile << '\n';
    std::cout << "game dir:         " << man.gameDir << '\n';
    std::cout << "files:            " << man.files.size() << '\n';
    int backups = 0;
    for (const auto& f : man.files)
      if (f.hadBackup)
        ++backups;
    std::cout << "backed-up originals: " << backups << '\n';
    return 0;
  }

  if (cmd == "play") {
    const bool noRollback = takeBool(rest, "--no-rollback");
    const auto exe        = takeFlag(rest, "--exe");
    std::cout << "deploying and launching" << (noRollback ? " (no rollback)" : "")
              << "...\n";
    const int code = app.play(!noRollback, exe.value_or(""));
    std::cout << "game exited with code " << code << '\n';
    if (!noRollback)
      std::cout << "game folder restored\n";
    return 0;
  }
  if (cmd == "launch") {
    const auto exe = takeFlag(rest, "--exe");
    const int code = app.launchGame(exe.value_or(""));
    std::cout << "game exited with code " << code << '\n';
    return 0;
  }
  if (cmd == "saves-on" || cmd == "saves-off") {
    app.setManageSaves(cmd == "saves-on");
    std::cout << "per-profile saves " << (cmd == "saves-on" ? "enabled" : "disabled")
              << '\n';
    return 0;
  }
  if (cmd == "saves") {
    std::cout << "per-profile saves: " << (app.manageSaves() ? "ON" : "OFF") << '\n';
    std::cout << "User Files folder: " << userfiles::gtaUserFilesDir().string() << '\n';
    if (!app.activeProfile().empty())
      std::cout << "active profile store: "
                << app.profileSavesDir(app.activeProfile()).string() << '\n';
    return 0;
  }
  if (cmd == "settings-template") {
    const std::string sub = rest.empty() ? "" : rest[0];
    if (sub == "save") {
      app.saveCurrentSettingsAsTemplate();
      std::cout << "saved current gta_sa.set as the default settings template\n";
      return 0;
    }
    if (sub == "clear") {
      app.clearDefaultSettingsTemplate();
      std::cout << "cleared the default settings template\n";
      return 0;
    }
    std::cout << "default settings template: "
              << (app.hasDefaultSettingsTemplate() ? "captured" : "none") << '\n';
    std::cout << "path: " << app.defaultSettingsTemplatePath().string() << '\n';
    std::cout << "(usage: settings-template [save|clear])\n";
    return 0;
  }
  if (cmd == "samp") {
    const std::string prof = rest.empty() ? app.activeProfile() : rest[0];
    if (prof.empty()) {
      std::cout << "no active profile (pass a profile name)\n";
      return 1;
    }
    const gtamm::SampConfig s = app.sampConfig(prof);
    std::cout << "SA-MP for profile '" << prof << "':\n";
    std::cout << "  enabled: " << (s.enabled ? "ON" : "OFF") << '\n';
    std::cout << "  server:  " << (s.server.empty() ? "(none)" : s.server) << ':'
              << s.port << '\n';
    std::cout << "  nick:    " << (s.nick.empty() ? "(none)" : s.nick) << '\n';
    std::cout << "  password: " << (s.password.empty() ? "(none)" : "(set)") << '\n';
    std::cout << "  exe:     " << s.exe << '\n';
    return 0;
  }
  if (cmd == "samp-set") {
    const bool on  = takeBool(rest, "--on");
    const bool off = takeBool(rest, "--off");
    const auto server   = takeFlag(rest, "--server");
    const auto port     = takeFlag(rest, "--port");
    const auto nick     = takeFlag(rest, "--nick");
    const auto exe      = takeFlag(rest, "--exe");
    const auto password = takeFlag(rest, "--password");
    const std::string prof = rest.empty() ? app.activeProfile() : rest[0];
    if (prof.empty()) {
      std::cout << "no active profile (pass a profile name)\n";
      return 1;
    }
    gtamm::SampConfig s = app.sampConfig(prof);
    if (on)
      s.enabled = true;
    if (off)
      s.enabled = false;
    if (server)
      s.server = *server;
    if (port)
      s.port = std::stoi(*port);
    if (nick)
      s.nick = *nick;
    if (exe)
      s.exe = *exe;
    if (password)
      s.password = *password;
    app.setSampConfig(prof, s);
    std::cout << "SA-MP settings updated for '" << prof << "'\n";
    return 0;
  }
  if (cmd == "maps-on" || cmd == "maps-off") {
    app.setAutoRouteMaps(cmd == "maps-on");
    std::cout << "experimental map auto-install "
              << (cmd == "maps-on" ? "enabled" : "disabled") << '\n';
    return 0;
  }
  if (cmd == "maps") {
    std::cout << "experimental map auto-install: "
              << (app.autoRouteMaps() ? "ON" : "OFF") << '\n';
    return 0;
  }
  if (cmd == "steam-on" || cmd == "steam-off") {
    app.setSteamIntegration(cmd == "steam-on");
    std::cout << "Steam \"in-game\" status "
              << (cmd == "steam-on" ? "enabled" : "disabled") << '\n';
    if (cmd == "steam-on" && !app.steamStatus().hasApiDll)
      std::cout << "note: no " << gtamm::steam::apiDllName()
                << " could be found, so nothing will show up in Steam -- run "
                   "'steam' to see where to put one\n";
    return 0;
  }
  if (cmd == "steam") {
    const gtamm::App::SteamStatus st = app.steamStatus();
    std::cout << "Steam \"in-game\" status: "
              << (app.steamIntegration() ? "ON" : "OFF") << '\n'
              << "Steam client: "
              << (st.installed ? (st.running ? "installed (running)"
                                             : "installed (not running)")
                               : "not found")
              << '\n'
              << gtamm::steam::apiDllName() << ": "
              << (st.hasApiDll ? (st.apiDllFromSteam ? "found (borrowed from an "
                                                       "installed Steam game)"
                                                     : "found")
                               : "MISSING")
              << '\n'
              << "  " << st.apiDllPath << '\n';
    if (!st.hasApiDll)
      std::cout << "no copy could be found among your installed Steam games; "
                   "put Valve's redistributable " << gtamm::steam::apiDllName()
                << " at the path above (GMM loads it, but does not ship it)\n";
    else if (!st.running)
      std::cout << "start Steam before playing, or the status won't show\n";
    return 0;
  }
  if (cmd == "clean-on" || cmd == "clean-off") {
    app.setManageGenerated(cmd == "clean-on");
    std::cout << "keep game folder clean (per-profile generated files) "
              << (cmd == "clean-on" ? "enabled" : "disabled") << '\n';
    return 0;
  }
  if (cmd == "clean") {
    std::cout << "keep game folder clean: " << (app.manageGenerated() ? "ON" : "OFF")
              << '\n';
    if (!app.activeProfile().empty())
      std::cout << "active profile store: "
                << app.generatedDir(app.activeProfile()).string() << '\n';
    return 0;
  }

  std::cerr << "error: unknown command '" << cmd << "'\n\n" << kUsage;
  return 1;
}

}  // namespace

int main(int argc, char** argv)
{
  std::vector<std::string> args(argv + 1, argv + argc);
  try {
    return run(std::move(args));
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
