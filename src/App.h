#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Deploy.h"
#include "Model.h"
#include "SannyBuilder.h"
#include "Steam.h"

namespace gtamm {

// High-level facade tying together the data directory: config, the mod pool and
// the profiles. All on-disk state lives under a single data directory:
//
//   <data>/config.json         { gamePath, activeProfile }
//   <data>/mods/<id>/meta.json  mod metadata
//   <data>/mods/<id>/root/      mod files (relative to the game dir)
//   <data>/profiles/<name>.json profile
//
class App
{
public:
  explicit App(std::filesystem::path dataDir);

  // --- lifecycle ---
  bool isInitialized() const;
  void init(const std::string& gamePath);  // create dirs + config.json

  // --- config ---
  const std::string& gamePath() const { return m_gamePath; }
  const std::string& activeProfile() const { return m_activeProfile; }
  // Point this instance at a different game folder (updates config.json).
  void setGamePath(const std::string& gamePath);

  // --- experimental map auto-install (Mod Loader std.stream/std.data style) ---
  // When ON, deploy injects NEW models/textures referenced by a mod's .ide into
  // gta3.img, routes new .ide/.ipl into data/maps/gmm + registers them in
  // data/gta.dat, and injects .ipl world-stream replacements into the IMG. This
  // can make map mods install standalone, BUT it makes risky static guesses (an
  // ID-conflicting .ide or a bad IPL can crash a new game on load), so it is OFF
  // by default. With it off, such files just deploy loose (harmless; map may not
  // work) and the rest of the build is unaffected.
  bool autoRouteMaps() const { return m_autoRouteMaps; }
  void setAutoRouteMaps(bool on);

  // --- Steam "in-game" status ---
  // When ON, GMM's own process registers with the running Steam client as
  // Grand Theft Auto: San Andreas (AppID 12120) for the duration of a launch,
  // so the client and the player's friends see "Playing Grand Theft Auto: San
  // Andreas" and the time counts against the real store entry. This is what
  // Steam Achievement Manager does, and it requires the signed-in Steam
  // account to actually OWN the game -- the client refuses the session
  // otherwise. Nothing in Steam's files or in the game folder is modified.
  //
  // No overlay: Steam only injects it into processes it launched itself, and
  // the game here is launched by GMM. That trade-off is deliberate (see
  // Steam.h). Replaces an older steam_appid.txt approach that could never
  // have worked at all -- that file is read by steam_api.dll from inside the
  // *game* process, and retail GTA SA 1.0 never calls the Steam API.
  //
  // Off by default. If the toggle is on but the session can't be established
  // (Steam closed, game not owned, steam_api64.dll missing), the game is
  // launched anyway, just without the Steam status.
  bool steamIntegration() const { return m_steamIntegration; }
  void setSteamIntegration(bool on);

  // Valve's redistributable steam_api64.dll, which GMM loads at runtime but
  // does not ship. Looked for, in order:
  //   1. an explicit path the user picked, or one auto-detection already
  //      settled on (cached in config.json);
  //   2. the bundle folders -- <program>/runtime-steam/ (where the embedded
  //      copy is unpacked, like the Mod Loader / SA-MP runtimes), then
  //      <data>/steam-runtime/, then next to the program;
  //   3. failing both, the copies that come with games already installed
  //      through Steam (see steam::findApiDlls) -- so with Steam present this
  //      normally needs no setup at all.
  // Several candidates are returned because a library may also hold a
  // *replacement* steam_api64.dll from a cracked game, which would refuse the
  // session; launchGame() tries them in order and remembers the one that
  // worked. steamApiDll() is just the first of them ("" if there are none).
  std::vector<std::filesystem::path> steamApiDllCandidates();
  std::filesystem::path steamApiDll();
  std::filesystem::path steamRuntimeDir() const;
  // Pin a specific steam_api64.dll (empty clears it and returns to
  // bundle/auto-detection).
  void setSteamApiDll(const std::string& path);

  struct SteamStatus
  {
    bool installed = false;   // Steam found on this machine
    bool running   = false;   // steam.exe up right now
    bool hasApiDll = false;   // the redistributable is available to load
    bool apiDllFromSteam = false;  // borrowed from an installed Steam game
    std::string apiDllPath;   // where it was found (or where to put it)
    // Everything needed is in place, so a launch would show up in Steam
    // (ownership can only be confirmed by actually starting the session).
    bool ready = false;
  };
  SteamStatus steamStatus();

  // --- Sanny Builder 4 ---
  // A separate, third-party script editor/compiler, not shipped with GMM: the
  // user points GMM at their own copy and gets a toolbar button that launches
  // it. Purely a convenience shortcut -- nothing here touches deployment.
  //
  // The stored path may be either the executable or the folder holding it
  // (that's what people have on disk, e.g. "SannyBuilder-v4.2.0"); it is
  // resolved to the actual exe by sannyBuilderExe(), which is empty when the
  // path is unset or holds no Sanny Builder.
  std::string sannyBuilderPath() const { return m_sannyBuilderPath; }
  void setSannyBuilderPath(const std::string& path);
  std::filesystem::path sannyBuilderExe() const;
  bool hasSannyBuilder() const;
  // Whether to show the toolbar button (off by default; a valid path is still
  // required for the button to appear).
  bool showSannyBuilder() const { return m_showSannyBuilder; }
  void setShowSannyBuilder(bool on);

  // Mod that receives scripts compiled in Sanny Builder (empty = don't
  // redirect). Compiling into the *game* folder would be a trap: that folder is
  // generated by GMM, so a rollback removes the script again, and overwriting a
  // deployed file breaks its hardlink and leaves the pool copy stale. Sending
  // the output straight into mods/<id>/root/CLEO keeps the result where GMM can
  // actually manage it.
  std::string sannyCleoModId() const { return m_sannyCleoModId; }
  void setSannyCleoModId(const std::string& modId);

  // Pushes the current instance into the configured Sanny Builder: the game
  // folder it should read (gta.dat/GXT/IDE) and, when a target mod is set, the
  // CLEO output directory. Called right before launching it, so what SB sees
  // always matches the instance/profile in front of the user. Never throws for
  // "not configured" -- it just reports what it did in the result.
  sanny::SyncResult syncSannyBuilder();

  // --- per-profile saves (User Files) ---
  // When enabled, play() redirects "<Documents>\GTA San Andreas User Files" to
  // the active profile's own saves store for the duration of the game session
  // (junction swap, original restored on exit). Off by default.
  bool manageSaves() const { return m_manageSaves; }
  void setManageSaves(bool on);
  // Where a profile's saves/settings are stored on disk.
  std::filesystem::path profileSavesDir(const std::string& name) const;

  // --- per-profile generated/runtime files ---
  // When enabled, play() keeps the game folder pristine: files a mod or the game
  // creates in the game folder during a session are moved into the active
  // profile's own store on exit (and the folder is cleaned), then restored before
  // the next launch. Off by default.
  bool manageGenerated() const { return m_manageGenerated; }
  void setManageGenerated(bool on);
  // Where a profile's captured generated files live on disk.
  std::filesystem::path generatedDir(const std::string& name) const
  {
    return m_dataDir / "generated" / name;
  }
  // Lower-level steps used by play() (exposed for tooling/tests):
  // Copy a profile's stashed generated files into the game dir (without
  // clobbering existing deployed/vanilla files); returns the relpaths placed.
  std::set<std::string> restoreGenerated(const std::string& profile,
                                         const std::filesystem::path& gameDir);
  // Record every file/dir relpath currently under gameDir (the post-deploy state).
  void snapshotTree(const std::filesystem::path& gameDir,
                    std::set<std::string>& files,
                    std::set<std::string>& dirs) const;
  // Move files created/changed during the session (not in `baseFiles`, or among
  // `restored`) into the profile's store, then prune game-created empty dirs, so
  // the game folder returns to its post-deploy state.
  void captureGenerated(const std::string& profile,
                        const std::filesystem::path& gameDir,
                        const std::set<std::string>& baseFiles,
                        const std::set<std::string>& baseDirs,
                        const std::set<std::string>& restored);
  // Redirect the game's User Files to the active profile's store (parking any
  // existing real folder aside), and restore it afterwards. Normally driven by
  // play(); exposed for tooling/tests.
  void applyProfileSaves();
  void restoreProfileSaves();

  // --- default settings template (first-run resolution/controls) ---
  // A user-captured gta_sa.set snapshot that play() seeds into the *currently
  // effective* settings location (the active profile's saves store if
  // manageSaves is on, else the live User Files folder) the first time it has no
  // gta_sa.set of its own -- so a brand-new profile/instance starts with the
  // user's own resolution/control choices instead of the game's first-run
  // setup, with no manual step. Never overwrites an existing gta_sa.set. No-op
  // if no template has been captured yet.
  std::filesystem::path defaultSettingsTemplatePath() const
  {
    return m_dataDir / "default-settings" / "gta_sa.set";
  }
  bool hasDefaultSettingsTemplate() const;
  // Captures the currently effective gta_sa.set as the template (overwrites any
  // previous one). Throws if there isn't one yet -- play the game and set your
  // options once first.
  void saveCurrentSettingsAsTemplate();
  void clearDefaultSettingsTemplate();
  // Copies the template into the currently effective settings location, unless
  // it already has a gta_sa.set (real or previously seeded) or no template has
  // been captured. Normally driven by play(); exposed for tooling/tests.
  void seedDefaultSettingsIfMissing();

  // --- mod pool ---
  // Result of an import: the pool mod, and whether it already existed (an
  // identical mod was found, so nothing new was added).
  struct ImportResult
  {
    Mod mod;
    bool wasDuplicate = false;
  };

  std::vector<Mod> mods() const;
  ImportResult importFromFolder(const std::filesystem::path& src,
                                const std::string& nameOpt);
  // Extract an archive (zip/7z/rar/...) and import its contents into the pool.
  // `password`, if the archive turns out to be encrypted, is tried as its
  // passphrase; throws ArchivePasswordRequired (see Unpack.h) if it's empty or
  // wrong, so a caller can prompt and retry. If `archivePath` doesn't actually
  // look like an archive (by extension), it's treated as a single loose mod
  // file instead (a standalone .lua script, .asi plugin, ...) and imported as
  // the sole file of a new mod, rather than failing to open it as an archive.
  ImportResult importFromArchive(const std::filesystem::path& archivePath,
                                 const std::string& nameOpt,
                                 const std::string& password = std::string());
  // Import a folder/archive that may be a *pack* of several mods: a Mod Loader
  // tree (modloader/<mod>/..., including nested category folders like
  // modloader/HD/<mod>) or a plain folder whose subfolders are each a mod. Every
  // self-contained mod folder becomes its own pool mod; a single mod yields one
  // result. Used by the GUI/CLI import actions.
  //
  // When `viaModloader` is true the imported mod(s) are flagged to deploy under
  // "modloader/<name>/" for the Mod Loader runtime instead of being routed
  // natively into IMG/loose. A Mod Loader pack is then split one-mod-per-subfolder
  // and each subtree is adopted verbatim (no path flattening), so it lands back
  // under modloader/ exactly as authored.
  std::vector<ImportResult> importFolderAsMods(const std::filesystem::path& src,
                                               const std::string& nameOpt,
                                               bool viaModloader = false);
  // `password`: see importFromArchive(). Same non-archive-file fallback too.
  std::vector<ImportResult> importArchiveAsMods(
      const std::filesystem::path& archivePath, const std::string& nameOpt,
      bool viaModloader = false, const std::string& password = std::string());
  void removeMod(const std::string& id);
  // Toggle a pool mod's deploy target between native and Mod Loader (rewrites
  // meta.json). A redeploy is needed for the change to take effect in-game.
  void setModViaModloader(const std::string& id, bool on);
  // Set a mod's freeform description (rewrites meta.json). Purely informational
  // (shown in the GUI's Info tab) -- does not affect deploy or contentHash.
  void setModDescription(const std::string& id, const std::string& text);

  // --- MoonLoader scripts: individually toggleable/editable ---
  // Game-relative paths (e.g. "moonloader/foo.lua") of a mod's scripts that sit
  // directly in moonloader/ (NOT a subfolder -- moonloader/lib/..., .../lang/...
  // etc. are shared libraries the scripts above `require()`, not standalone
  // toggleable mods themselves). Empty if the mod has no moonloader/ folder or
  // no top-level .lua files in it.
  std::vector<std::string> listModLuaScripts(const std::string& modId) const;
  struct SplitScriptsResult
  {
    std::vector<std::string> createdModIds;  // one per script (+ its side files)
    int profilesUpdated = 0;  // profiles that referenced the mod and were rewired
  };
  // Pull every top-level moonloader/*.lua script (and any side file/folder that
  // shares its stem, e.g. moonloader/<script>.ini) out of an existing pool mod
  // into its OWN separate mod -- so each script gets its own enable/disable
  // checkbox and can be edited independently, exactly like the existing CLEO-
  // script/ASI-plugin splitting import-build already does. The original mod
  // keeps whatever is left (possibly nothing). Every profile that already had
  // the original mod enabled/disabled gets the new script-mods inserted right
  // after it with the SAME enabled state, so nothing already deployed is lost
  // on the next Deploy. A no-op (empty result) if the mod has no top-level
  // moonloader scripts to begin with.
  SplitScriptsResult splitMoonloaderScripts(const std::string& modId);
  // Overwrite one file inside a pool mod's tree (used by the script editor's
  // Save) and refresh the mod's cached content hash. Writes IN PLACE (no
  // temp+rename swap): if the mod is currently deployed via hardlink, the edit
  // is visible in the game folder immediately with no redeploy needed; a
  // rename would instead break the hardlink and leave the deployed copy stale.
  // Throws if the mod or the file doesn't exist.
  void writeModFile(const std::string& modId, const std::string& relPath,
                    const std::string& content);

  // --- Mod Loader runtime ---
  // Master switch for the Mod Loader runtime (modloader.asi + ASI loader + CLEO
  // + bundled compatibility fixes/QoL mods under modloader/_ESSENTIALS/, the
  // latter also gated by enableEssentials()): deploy() installs it if and only
  // if this is ON -- deliberately NOT also implied by some active mod being
  // flagged viaModloader. ON by default (a fresh instance should "just work"
  // with a single checkbox, nothing to import first); the user can turn it off
  // in Settings. Turning it OFF while a viaModloader mod is enabled leaves that
  // mod's files staged under modloader/<name>/ as usual, but with no loader to
  // read them the mod simply stops working -- accepted so the switch stays an
  // unambiguous on/off rather than a "usually off but not if some mod needs
  // it" toggle that silently overrides itself.
  bool enableModloader() const { return m_enableModloader; }
  void setEnableModloader(bool on);

  // When ON (default), the modloader/_ESSENTIALS/ subtree of the runtime bundle
  // (SilentPatch, two different widescreen fixes, FramerateVigilante, RunDLL32
  // Fix, Windowed Mode) is installed alongside the Mod Loader core whenever
  // deployModloaderRuntime() runs. Independent of enableModloader so a profile
  // that already curates its own equivalent fixes (its own SilentPatch/
  // widescreen mod, etc.) can keep Mod Loader itself without the bundle's
  // copies fighting the profile's own copies over the same D3D9 hooks/FOV
  // patches -- a real, reproduced cause of a black 3D world + torn menu crash.
  bool enableEssentials() const { return m_enableEssentials; }
  void setEnableEssentials(bool on);

  // Folder holding the Mod Loader runtime (modloader.asi + an ASI loader such as
  // dinput8.dll) that deploy() installs into the game when any active mod deploys
  // via Mod Loader. The user populates it once. Defaults to
  // <data>/modloader-runtime.
  std::filesystem::path modloaderRuntimeDir() const;
  // Override the runtime folder (empty string resets to the default location).
  void setModloaderRuntimeDir(const std::string& dir);
  // Whether a Mod Loader runtime is available: either already present in the game
  // folder (modloader.asi) or stored in modloaderRuntimeDir(). deploy() can only
  // install the runtime itself in the latter case.
  bool hasModloaderRuntime() const;

  // Replace the game's gta_sa.exe with the bundled clean GTA SA 1.0 US (taken from
  // the runtime folder) on deploy. Off by default; fully reversible (the original
  // exe is backed up and restored on rollback). Independent of Mod Loader.
  bool replaceGameExe() const { return m_replaceGameExe; }
  void setReplaceGameExe(bool on);
  // True if a bundled gta_sa.exe is available to deploy (present in the runtime).
  bool hasBundledExe() const;

  // --- SA-MP client runtime ---
  // Folder holding a full, official SA-MP client install (samp.exe, mouse.png,
  // sampgui.png, the SAMP/ custom-object archives, ...) that deploy() installs
  // into the game whenever the active profile's SampConfig is enabled. This
  // sidesteps the general mod-import pipeline entirely -- its junk/wrapper
  // heuristics are tuned for ordinary mods, not SA-MP's own bundled binaries --
  // by copying the whole tree verbatim, filling in only whatever an enabled mod
  // hasn't already provided this deploy. The user populates it once, the same
  // way as modloaderRuntimeDir(). Defaults to <data>/samp-runtime.
  std::filesystem::path sampRuntimeDir() const;
  // Override the runtime folder (empty string resets to the default location).
  void setSampRuntimeDir(const std::string& dir);
  // Whether a bundled SA-MP client is available to install from sampRuntimeDir().
  bool hasSampRuntime() const;

  // --- import an existing modded build ("сборка") ---
  // Diff a complete (already modded) GTA SA folder against the clean vanilla
  // baseline, extract everything that differs from vanilla, split it into named,
  // separately-toggleable mods (Mod Loader folders / CLEO scripts / ASI plugins
  // become their own mods; the unstructured remainder is lumped into one), and
  // wire them all into a brand-new profile.
  struct ImportBuildOptions
  {
    std::string profileName;       // new profile to create (default: folder name)
    bool splitByStructure = true;  // reserved; structural split is always on
    bool refreshBaseline  = false; // re-scan the vanilla baseline before diffing
  };
  // A build-relative file that was NOT imported, and why.
  struct SkippedFile
  {
    std::string rel;     // path relative to the build folder
    std::string reason;  // "junk", "user data", "unchanged (matches vanilla baseline)"
  };
  struct ImportBuildResult
  {
    std::string profile;
    std::vector<std::string> createdModIds;
    int looseChanged = 0;  // loose files that differ from vanilla
    int imgChanged   = 0;  // IMG archive entries that differ from vanilla
    int skipped      = 0;  // junk/unchanged/user-data files ignored == skippedFiles.size()
    std::vector<SkippedFile> skippedFiles;  // every one of them, with a reason
    std::vector<std::string> notes;
  };
  ImportBuildResult importBuild(const std::filesystem::path& buildDir,
                                const ImportBuildOptions& opts);
  // Has a vanilla baseline already been computed for this instance?
  bool hasBaseline() const;
  // Explicitly (re)builds and caches the vanilla baseline from the CURRENT
  // game folder -- used both by importBuild()'s diff and by rollback()'s
  // orphan cleanup (cleanOrphansAgainstBaseline()). Throws if a profile is
  // currently deployed (the folder must genuinely be vanilla right now for
  // the snapshot to mean anything -- run rollback() first). Overwrites any
  // existing cached baseline; importBuild() otherwise only ever builds one
  // lazily the first time it's needed, so an instance that never ran
  // import-build (e.g. mods added by hand into a fresh profile) would
  // otherwise have no baseline at all and rollback()'s orphan cleanup stays a
  // silent no-op -- this gives the user (or the GUI) an explicit way to set
  // one up without going through a build-diff import.
  void refreshVanillaBaseline();

  // --- export / import a build ("сборка") as a portable zip ---
  // Package a profile and every mod it references into a single zip: the profile
  // (order/priority/separators/SA-MP), the currently ENABLED referenced pool
  // mods (metadata + files -- disabled entries are left out entirely, not just
  // their files) and the profile's build-info notes. The game and generated
  // files are never included. Optionally also bundle the profile's savegames (*.b) and/or
  // game settings (gta_sa.set / *.set) from its per-profile saves store, which
  // importBuildArchive restores into the target profile. Shareable; importable
  // into any other instance.
  void exportBuild(const std::string& profileName,
                   const std::filesystem::path& zipPath, bool withSaves = false,
                   bool withSettings = false);

  // Export a single pool mod into a zip our manager re-imports perfectly: a
  // manifest (mod.json: format "gmm-mod" + metadata) plus the mod's exact
  // game-relative file tree under root/. On import it is adopted verbatim (no
  // wrapper-unwrapping / Mod Loader re-routing guesswork), so it deploys exactly
  // as it does now. Importing it is just the normal folder/archive import.
  void exportMod(const std::string& modId, const std::filesystem::path& zipPath);

  struct ImportBuildArchiveResult
  {
    std::string profile;                 // profile created/updated
    int modsAdded   = 0;                 // mods newly added to this pool
    int modsReused  = 0;                 // mods already present (deduped)
    int entriesDropped = 0;              // profile entries whose mod was missing
    std::vector<std::string> notes;
  };
  // Import a build zip produced by exportBuild into this instance under `profileName`
  // (created if new; an existing profile of that name is overwritten). Mods are
  // added to the pool (dedup by content), the profile is rebuilt with the saved
  // order/SA-MP, build-info is restored, and the profile becomes active.
  ImportBuildArchiveResult importBuildArchive(const std::filesystem::path& zipPath,
                                              const std::string& profileName);
  // True if `zipPath` is a build archive produced by exportBuild() (its build.json
  // declares format "gmm-build"), without importing anything -- lets a caller
  // (e.g. drag-and-drop) tell a build zip apart from a plain mod zip before
  // deciding which import path to run. Never throws; returns false for anything
  // that isn't a readable GMM build zip.
  bool isBuildArchive(const std::filesystem::path& zipPath) const;

  // --- profiles ---
  std::vector<std::string> profileNames() const;
  void createProfile(const std::string& name);
  void deleteProfile(const std::string& name);
  void renameProfile(const std::string& oldName, const std::string& newName);
  // Duplicate a profile (mods + order + build-info notes). Optionally also copy
  // the source profile's savegames (*.b) and/or game settings (gta_sa.set) from
  // its per-profile saves store.
  void copyProfile(const std::string& src, const std::string& dst,
                   bool withSaves, bool withSettings);
  void useProfile(const std::string& name);  // set active profile
  Profile loadProfile(const std::string& name) const;

  // --- per-profile SA-MP (multiplayer) settings ---
  // Read/write a profile's SA-MP launch settings (server/port/nick/injector).
  // When enabled, the default Play action launches through samp.exe and waits on
  // the spawned gta_sa.exe so deployed mods survive the whole session.
  SampConfig sampConfig(const std::string& profileName) const;
  void setSampConfig(const std::string& profileName, const SampConfig& cfg);
  // True if a relative game path either already exists on disk in the current
  // (possibly undeployed) game folder, OR would be placed there by deploying
  // the active profile as it stands right now (e.g. samp.exe bundled inside a
  // mod that hasn't been deployed yet). Used by the GUI to validate a
  // configured path (like SA-MP's launcher exe) without crying wolf over
  // something a mod will legitimately provide once you hit Deploy/Play.
  // Absolute paths are checked for existence only (deploy is irrelevant to them).
  bool relativePathWillExist(const std::string& relPath) const;
  // Filenames of .exe files the active profile's CURRENT deploy plan would
  // place directly at the game root (e.g. a portable build's own samp.exe,
  // bundled inside a mod rather than pre-installed by hand) -- even before
  // Deploy has actually run. Lets the GUI's run-exe picker offer such an exe
  // as a choice without requiring a deploy first just to discover it exists.
  std::vector<std::string> pendingRootExecutables() const;

  // --- per-profile build info (Markdown + embedded images) ---
  // Free-form notes about the modpack ("сборка"), stored as Markdown next to the
  // profile. Shown in the GUI's Info panel when no mod is selected. Images are
  // copied into the profile's info folder and referenced by relative path.
  std::filesystem::path profileInfoDir(const std::string& name) const
  {
    return profilesDir() / (name + ".info");
  }
  std::filesystem::path profileInfoFile(const std::string& name) const
  {
    return profileInfoDir(name) / "info.md";
  }
  std::string loadProfileInfo(const std::string& name) const;
  void saveProfileInfo(const std::string& name, const std::string& markdown) const;
  // Copy an image into the profile's info folder; returns the relative path to
  // embed in the Markdown (e.g. "images/screenshot.png").
  std::string addProfileInfoImage(const std::string& name,
                                  const std::filesystem::path& src) const;

  // --- active-profile mod operations ---
  void setEnabled(const std::string& modId, bool enabled);
  void setPriority(const std::string& modId, int priority);
  // Replace the active profile's entries wholesale (used by the GUI when the
  // user reorders / toggles mods).
  void setActiveProfileEntries(const std::vector<ProfileEntry>& entries);

  // --- conflicts & deployment (Stage 2) ---
  // Files provided by more than one enabled mod in the active profile.
  std::vector<Conflict> conflicts() const;
  // Deploy the active profile into the game folder (hardlinks with copy
  // fallback). Re-deploying first rolls back the previous deployment.
  void deploy();
  // Undo the current deployment, restoring the clean game folder.
  void rollback();
  bool isDeployed() const;
  Manifest currentManifest() const;

  // --- launch (Stage 3) ---
  // Launch the game (or `exeName`, relative to the game dir or absolute) and
  // wait for it to exit; returns the process exit code. Does not deploy.
  int launchGame(const std::string& exeName);
  // Full cycle: deploy the active profile, launch and wait, then (optionally)
  // roll back. Returns the game's exit code.
  int play(bool rollbackAfter, const std::string& exeName);

  // --- paths ---
  std::filesystem::path dataDir() const { return m_dataDir; }
  std::filesystem::path modsDir() const { return m_dataDir / "mods"; }
  std::filesystem::path profilesDir() const { return m_dataDir / "profiles"; }
  std::filesystem::path configPath() const { return m_dataDir / "config.json"; }
  std::filesystem::path manifestPath() const { return m_dataDir / "deployed.json"; }
  std::filesystem::path backupDir() const { return m_dataDir / "backup"; }
  std::filesystem::path savesDir() const { return m_dataDir / "saves"; }

private:
  // How the active profile would launch `exeName` right now: which executable
  // to start and with which argument line (under SA-MP that's the injector
  // plus its host:port token, and the process actually waited on is the
  // gta_sa.exe it spawns -- see launchSampAndWait).
  struct LaunchTarget
  {
    std::filesystem::path exe;
    std::wstring args;
    bool viaSamp = false;
    SampConfig samp;
    bool haveSamp = false;
  };
  LaunchTarget resolveLaunchTarget(const std::string& exeName) const;

  void loadConfig();
  void saveConfig() const;
  void requireInit() const;
  std::filesystem::path profilePath(const std::string& name) const;
  void saveProfile(const Profile& p) const;
  std::optional<Mod> findMod(const std::string& id) const;
  std::string makeUniqueId(const std::string& name) const;
  // Import one Mod Loader mod folder, re-basing its files to game-relative paths
  // (Mod Loader routes by type/name, ignoring the mod's internal folder layout).
  // `looseIndex` maps a vanilla loose file's lowercased name to its game-relative
  // path, so a bare loose file (e.g. fonts.txd) lands where the game reads it.
  ImportResult importModloaderMod(
      const std::filesystem::path& modFolder, const std::string& name,
      const std::map<std::string, std::string>& looseIndex);
  // Adopt a Mod Loader mod folder VERBATIM (no path flattening) as a mod flagged
  // to deploy back under modloader/<name>/. Used for "with Mod Loader support"
  // imports and for build imports that keep modloader/ content running under
  // Mod Loader.
  ImportResult importModloaderModVerbatim(const std::filesystem::path& modFolder,
                                          const std::string& name);
  // Install the Mod Loader runtime (from modloaderRuntimeDir()) into the game
  // folder, recording each file in `man`. Each file is deduped individually
  // against whatever an enabled native mod already deployed this run (exact
  // path match, or same .asi filename anywhere) -- so e.g. a mod that already
  // brought its own modloader.asi keeps it, while our CLEO/.data/_ESSENTIALS
  // (the latter only if enableEssentials()) still fill in what that mod did
  // NOT bring. A no-op only if the runtime folder is empty/missing, or every
  // single file it would install already has an equivalent. Returns true only
  // if we actually installed/changed at least one file --
  // deploy() uses this to decide whether swapping in the bundled clean 1.0 exe
  // is appropriate (it is when Mod Loader wouldn't exist without us; it is NOT
  // when an already-self-consistent modloader.asi + its own loader DLLs came
  // from the user's own mods -- forcing a different exe under those is more
  // likely to break a working setup than fix a broken one).
  bool deployModloaderRuntime(const std::filesystem::path& gameDir, Manifest& man);
  // Install the bundled SA-MP client (from sampRuntimeDir()) into the game,
  // filling in only whatever an enabled mod hasn't already deployed this run
  // (so a hand-imported/partial SA-MP mod is topped up, not overwritten).
  // No-op (returns false) if sampRuntimeDir() has nothing to install.
  bool deploySampRuntime(const std::filesystem::path& gameDir, Manifest& man);
  // Index of the game's loose files: lowercased filename -> game-relative path.
  std::map<std::string, std::string> buildLooseIndex() const;
  // Add an already game-relative mod tree to the pool VERBATIM (no wrapper
  // normalization), deduping by content hash. `hashOpt`, if non-empty, is trusted
  // as the tree's content hash (avoids recomputing for a known pool mod). Used
  // when re-importing a previously-exported mod.
  ImportResult adoptMod(const std::filesystem::path& rootDir,
                        const std::string& name, const std::string& sourceLabel,
                        const std::string& hashOpt);
  // Content hash of a pool mod (from meta.json, computed and cached if missing).
  std::string ensureContentHash(const Mod& m);

  // A resolved target in the merged virtual tree: which mod wins, the real-case
  // identifier (loose: game-relative path; IMG: entry name), the path of the
  // winning file inside its mod's root/, and all providers (priority order).
  struct Merged
  {
    std::string winner;
    std::string winnerRel;        // display id (loose path or IMG entry name)
    std::string winnerSourceRel;  // path within the winning mod's root/
    std::vector<std::string> providers;
  };

  // A new map definition file (IDE/IPL) to register in data/gta.dat so the engine
  // loads it (Variant B: emulate Mod Loader's std.data).
  struct MapReg
  {
    std::string kind;     // "IDE" or "IPL"
    std::string relPath;  // game-relative deploy path (e.g. data/maps/gmm/x.ide)
  };

  // The active profile resolved into loose-file targets and per-IMG injections.
  struct ActivePlan
  {
    std::map<std::string, Merged> loose;  // key: lowercased game-relative path
    // key: game-relative IMG path -> (lowercased entry name -> winner)
    std::map<std::string, std::map<std::string, Merged>> img;
    // SFX audio: pak name -> local bank index -> local sound index -> winner.
    // (SAAT "Bank_<n>/sound_<m>.wav" injected into audio/SFX/<pak>.)
    std::map<std::string, std::map<int, std::map<int, Merged>>> sfx;
    // New map IDE/IPL files to register in data/gta.dat (in registration order).
    std::vector<MapReg> mapRegistrations;
    // Mod Loader mods: files staged verbatim under modloader/<name>/. Keyed by
    // lowercased game-relative deploy path; winnerRel is that path.
    std::map<std::string, Merged> modloader;
  };
  ActivePlan computeActive() const;

  // Index of original game IMG archives: lowercased entry name -> IMG path.
  std::map<std::string, std::filesystem::path> buildImgIndex() const;
  // The primary streaming archive (models/gta3.img) for adding NEW map content,
  // or empty if absent / not a VER2 IMG.
  std::filesystem::path primaryStreamImg() const;

  std::filesystem::path requireGameDir() const;
  std::filesystem::path baselinePath() const { return m_dataDir / "vanilla-baseline.json"; }
  std::filesystem::path imgPristineDir() const { return m_dataDir / "img-pristine"; }
  std::filesystem::path sfxPristineDir() const { return m_dataDir / "audio-pristine"; }
  // Restore one SFX pak / BankLkup file (game-relative) from the pristine cache.
  void restoreSfx(const std::string& rel, const std::filesystem::path& gameDir);
  std::filesystem::path savesManifestPath() const { return m_dataDir / "saves-active.json"; }

  // On startup, undo a saves swap left over from a crash mid-session.
  void recoverSaves();

  // Undo the loose-file part of a deployment (files, created dirs, backups);
  // does not touch IMG archives or the manifest.
  void rollbackLoose(const Manifest& man, const std::filesystem::path& gameDir);
  // Restore one IMG archive (game-relative path) from its pristine cache.
  void restoreImg(const std::string& rel, const std::filesystem::path& gameDir);

  // Called at the end of rollback(), only when manageGenerated() is on and a
  // cached vanilla baseline exists (hasBaseline()): removes any loose file
  // still present in gameDir that is not part of the vanilla baseline. Called
  // after rollbackLoose(), which has already stripped everything the deploy
  // being rolled back tracked in its own manifest -- so in practice this only
  // ever finds files that predate GMM's own management of the folder entirely
  // (e.g. a leftover CLEO.asi/cleo/ from before an import-build snapshot,
  // never tracked by any manifest). Content that merely differs from vanilla
  // (rather than being absent from it outright) is left alone -- the baseline
  // only stores a hash, not the original bytes, so there is nothing to
  // restore it to. Prunes directories left empty by the removals.
  void cleanOrphansAgainstBaseline(const std::filesystem::path& gameDir);

  std::filesystem::path m_dataDir;
  std::string m_gamePath;
  std::string m_activeProfile;
  std::string m_modloaderRuntimeDir;  // override; empty => <data>/modloader-runtime
  std::string m_sampRuntimeDir;       // override; empty => <data>/samp-runtime
  // Pinned/auto-detected steam_api64.dll (see steamApiDllCandidates()); empty
  // until one is chosen. Persisted, so the library scan runs at most once per
  // machine rather than once per launch.
  std::string m_steamApiDll;
  // Per-process memo of the Steam-library scan, so merely opening the Settings
  // dialog repeatedly doesn't walk steamapps/common again each time (the
  // persisted path above is only written once a session has actually opened
  // with a given DLL).
  std::vector<std::filesystem::path> m_steamScanHits;
  bool m_steamScanned = false;
  bool m_enableModloader = false;     // always install the Mod Loader runtime on deploy
  bool m_enableEssentials = true;     // also install modloader/_ESSENTIALS/ bundle
  bool m_replaceGameExe = false;      // replace gta_sa.exe with the bundled 1.0
  bool m_manageSaves = false;
  bool m_manageGenerated = false;
  bool m_autoRouteMaps = false;  // experimental map auto-install (OFF by default)
  bool m_steamIntegration = false;  // drop steam_appid.txt during play() (OFF by default)
  std::string m_sannyBuilderPath;   // exe or its folder; empty => not configured
  bool m_showSannyBuilder = false;  // show the Sanny Builder toolbar button
  std::string m_sannyCleoModId;     // mod receiving compiled scripts; empty => none
  bool m_loaded = false;
};

}  // namespace gtamm
