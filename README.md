<p align="center">
  <img src="resources/app.png" width="128" height="128" alt="GMM icon">
</p>

<h1 align="center">GMM — GTA San Andreas Mod Manager</h1>

A standalone, Mod Organizer–style mod manager for **GTA San Andreas (classic, 1.0)**
on Windows. Mods live in one central pool and are never modified in place; profiles
compose a build from that pool; deployment into the game folder is transactional and
fully reversible.

Unlike a runtime ASI loader, GMM deploys **offline**, before the game starts: the
running game sees a plain install with ordinary loose files and standard
`gta3.img` / `player.img` archives. Nothing about the deployment mechanism depends
on hooking the running process.

## Contents

- [Highlights](#highlights)
- [Requirements](#requirements)
- [Building from source](#building-from-source)
- [Optional runtime bundles](#optional-runtime-bundles)
- [Getting started (CLI)](#getting-started-cli)
- [Command reference](#command-reference)
- [Project layout](#project-layout)
- [Data directory layout](#data-directory-layout)
- [Testing](#testing)
- [Authors](#authors)
- [License](#license)
- [Acknowledgments](#acknowledgments)

## Highlights

**Core**
- **Shared mod pool** — every mod is stored exactly once, identified by a content
  hash; importing an identical mod again (even repackaged under a different name)
  is detected and skipped.
- **Profiles** — any number of independent builds, each an ordered list of enabled
  mods. Reordering, enabling/disabling, and separators (visual grouping) are all
  per-profile.
- **Conflict resolution** — higher priority wins, whole file; a dedicated view
  lists every overlap before you deploy.
- **Deploy / rollback** — loose files are hard-linked into the game folder (falling
  back to a copy across volumes); every original file the deploy overwrites is
  backed up. Rollback restores the game folder byte-for-byte, and a failed deploy
  is unwound automatically (atomic — no partial state left behind).
- **Import from a folder or an archive** — `zip` / `7z` / `rar` / `tar` (+ `gz` /
  `bz2` / `xz`), including password-protected archives (prompts for the passphrase)
  and archives that wrap another archive as their only real payload (unwrapped
  automatically, recursively).
- **Import an existing build** — point it at an already-modded game install and it
  diffs the folder against a cached vanilla baseline, splitting every change into
  separate toggleable mods (Mod Loader packs, CLEO scripts, ASI plugins, MoonLoader
  scripts, and a catch-all for everything else) plus a ready-to-use profile.

**Game-specific handling**
- **IMG archive injection** — new/replaced `.dff` / `.txd` / `.col` / `.ifp` content
  is injected directly into the matching game archive (`gta3.img`, `player.img`,
  `gta_int.img`, `cutscene.img`), rebuilt from a cached pristine copy so the
  original is never touched on disk.
- **SFX bank injection** — replacement audio for SAAT sound banks
  (`audio/SFX/<PAK>`) is injected the same way, following the same pak/bank/offset
  layout the game's sound engine expects.
- **Data-file merging** instead of whole-file overwrite:
  - keyed replace — `handling.cfg`, `weapon.dat`, `melee.dat`, `ar_stats.dat`,
    `pedstats.dat`, `carmods.dat` (line merged by key, so a partial mod doesn't
    wipe the rest of the file)
  - additive union — `gta.dat`, `default.dat` (new `IMG`/`IDE`/`IPL` registrations)
  - section-aware — `.ide` (records merged per section, keyed by model id/name)
- **Mod Loader interop** — any mod can be flagged to deploy natively (default) or
  "via Mod Loader" (staged under `modloader/<name>/`, loaded by the Mod Loader
  runtime at game start instead of being injected). An optional bundled Mod Loader
  runtime (core loader + CLEO + a curated set of compatibility fixes, each
  independently toggleable) can be installed automatically so a bare profile works
  out of the box.
- **SA-MP integration** — launch through `samp.exe` instead of `gta_sa.exe` with
  per-profile server/port/password/nickname, a built-in server browser
  (api.open.mp) with favourites, and an optional bundled client install.
- **Steam "in-game" status** — optional (off by default): while the game runs,
  GMM holds a Steam API session under GTA San Andreas's real AppID, so the
  client and your friends see "Playing Grand Theft Auto: San Andreas" and the
  time counts against the real store entry — the same mechanism Steam
  Achievement Manager uses. Requires the game to be owned on the signed-in
  Steam account. Nothing in Steam's files or the game folder is modified, and
  there is no overlay (Steam only injects that into processes it launched).
- **Sanny Builder 4 integration** — optional: GMM points Sanny Builder at the
  current instance's game folder and redirects its compiled CLEO output into a mod
  of your choosing, so what you compile lands where GMM manages it and survives a
  rollback (compiling straight into the game folder does not — the rollback removes
  it, and overwriting an already-deployed file breaks the hard link). Sanny
  Builder's own `settings.ini` / `mode.xml` are edited in place, one value at a
  time, so themes, hotkeys and history are left byte-for-byte intact.

**Quality of life**
- **Per-profile saves** — each profile can get its own `GTA San Andreas User
  Files` folder, swapped in only for the duration of a play session.
- **Keep the game folder clean** — files created by mods/the game during a play
  session are captured back into the profile's store and restored before the next
  launch; on rollback, files that predate GMM's own tracking entirely (once a
  vanilla baseline has been captured) are swept up too.
- **Named, portable instances** — MO2-style: each instance is a self-contained
  folder (its own pool, profiles, and config) pointing at one game install; a
  single-file launcher unpacks the whole portable install next to itself on first
  run.
- **Mod type badges** — every mod in the list is tagged with what it actually
  contains (CLEO / ASI / Lua / Mod Loader / Data / Other), derived from its file
  tree rather than its name; a mod that is several things at once gets several
  tags. The column sorts and the filter box searches on them, so typing `lua`
  narrows the list to Lua mods.
- **Built-in Lua editor** — edit a MoonLoader script in place from the mod list.
  Encoding is detected per file and preserved on save (UTF-8 or Windows-1251, the
  code page most Russian-language scripts are written in), as are the original
  line endings — so editing a script never silently mangles its text.
- **Rich build notes** — a WYSIWYG per-profile "About this build" page (images,
  formatting) shown when no mod is selected.
- **Qt 6 GUI** with light/dark themes (including support for Mod Organizer 2–style
  `.qss` stylesheets) and a full **CLI** covering every operation the GUI exposes.
- Runs in **English or Russian**.

## Requirements

- Windows 10/11.
- A clean, unmodified **GTA San Andreas 1.0** (US release) install.

Building from source additionally requires:

- Visual Studio 2022 or newer with the C++ desktop workload (MSVC, C++20). CMake,
  Ninja, and vcpkg are bundled with a standard Visual Studio install and need no
  separate setup.
- [Qt 6](https://www.qt.io/) (MSVC kit) — only needed to build the GUI target; the
  CLI and test suite build without it.

## Building from source

```bat
scripts\build.bat
```

This configures the project with vcpkg (dependencies are fetched and built
automatically on first run), builds `gtamm.exe` (CLI), `GMM.exe` (GUI, if a Qt 6
kit is found), and runs the test suite.

Set `CMAKE_PREFIX_PATH` to your Qt 6 kit if it isn't at the default location the
script checks (see the top of `scripts\build.bat`). The GUI target is skipped
automatically if no Qt 6 kit is found — the CLI and tests still build and run.

`scripts\release.bat` produces a Release build plus an assembled, portable
`release/gtamm/` folder (the GUI executable next to every DLL it needs) and, if a
prior packaged folder is available to bundle, a single-file launcher `GMM.exe`
that self-extracts next to wherever it's run from. `scripts\buildall.bat` runs
both the Debug and Release builds in one go, sharing a single build-number bump.

## Optional runtime bundles

GMM's core functionality needs nothing beyond a vanilla game install. A few
optional bundles extend it further, and are **not included in this repository** —
each is a folder you populate yourself with files you already have a license to
use; GMM embeds whatever it finds there into the built executable at compile time
(and unpacks it back out next to the program at runtime).

| Folder | Purpose | Marker file GMM looks for |
| --- | --- | --- |
| `runtime/` | Mod Loader runtime: the loader itself, CLEO + its extensions, and an optional curated set of compatibility fixes (SilentPatch, widescreen fixes, a framerate fix, etc.) | `modloader.asi` |
| `runtime-samp/` | A full, official SA-MP client install | `samp.exe` |
| `runtime-steam/` | Valve's `steam_api64.dll`, for the Steam "in-game" status. Rarely needed: GMM normally borrows a copy from a game already installed through Steam. | `steam_api64.dll` |
| `resources/runtime/` | Deprecated; superseded by `runtime/` above. Kept only so old checkouts don't silently lose the folder. | — |

Suggested upstream sources for the files `runtime/` expects:

- **Mod Loader** (the loader itself) — <https://github.com/thelink2012/modloader>
- **Essentials Pack** (for `modloader/_ESSENTIALS/` — SilentPatch, widescreen
  fixes, etc.) —
  <https://libertycity.net/files/gta-san-andreas/154094-essentials-essential-mods-pack.html>

`runtime-samp/` is sourced from an official SA-MP client install instead, and
`runtime-steam/` from the Steamworks SDK or any 64-bit Steam game you own (see
their own `README.md` files).

Each folder's own `README.md` (checked into the repo) documents its exact expected
contents in detail. In short:

- **`runtime/`** — a self-consistent Mod Loader install: `modloader.asi`, its ASI
  loader DLL, the whole `modloader/.data/` folder (config, plugins, languages),
  optionally `modloader/_ESSENTIALS/` (the compatibility-fix mods, each toggleable
  independently at runtime), `CLEO.asi` + `CLEO/*.cleo`, `_noDEP.asi`, `bass.dll`,
  and optionally a clean `gta_sa.exe` (1.0 US) if you want GMM able to swap it in.
  All of it must come from one consistent Mod Loader release — don't mix
  `modloader.asi` from one build with `std.*.dll` plugins from another.
- **`runtime-samp/`** — a complete SA-MP client as it looks after installing SA-MP
  into a game folder: `samp.exe` **and `samp.dll`** (the exe alone does nothing —
  it's just the injector, `samp.dll` is the actual client), `samp_debug.exe`,
  `rcon.exe`, `mouse.png`, `sampgui.png`, `sampaux3.ttf`, `gtaweap3.ttf`,
  `samp.saa`, `lang/*.json`, and the `SAMP/` subfolder with its
  `.img`/`.ide`/`.ipl`/`.txd` files.

Leave a folder empty (just its `README.md`) and the corresponding feature is
simply unavailable — everything else still builds and runs normally.

## Getting started (CLI)

```bat
gtamm --data <dir> init --game "C:\Path\To\Grand Theft Auto San Andreas"
gtamm --data <dir> import <folder|archive> [--name <n>] [--modloader] [--password <pw>]
gtamm --data <dir> profile-create main
gtamm --data <dir> enable <modId>
gtamm --data <dir> order <modId> <priority>
gtamm --data <dir> conflicts
gtamm --data <dir> deploy        # install the active profile into the game
gtamm --data <dir> play          # deploy, launch, wait for exit, then roll back
gtamm --data <dir> rollback      # restore the game folder
```

`<dir>` is the instance's data folder (mods, profiles, config) — pick anything;
the GUI manages one automatically per named instance next to the executable.

## Command reference

Run `gtamm help` for the full, always-up-to-date list. Categories:

- **Pool & profiles** — `import`, `import-build`, `mods`, `mod-remove`,
  `mod-modloader`, `mod-lua-list`, `mod-lua-split`, `profile-create/list/delete/
  rename/copy/use`, `enable`, `disable`, `order`.
- **Deployment** — `conflicts`, `deploy`, `rollback`, `deploy-status`,
  `img-list/extract/inject`.
- **Build export/import** — `export-mod`, `export-build`, `import-build-zip`.
- **Launch** — `play`, `launch`, `samp`, `samp-set`.
- **Settings** — `saves`, `clean`, `maps`, `essentials`, `replace-exe`,
  `modloader-dir`, `steam`, `settings-template`, `baseline-status`,
  `baseline-build`.
- **Info** — `status`, `version`.

## Project layout

```
src/
  App.{h,cpp}          domain logic: pool, profiles, conflicts, deploy/rollback
  Baseline.{h,cpp}      vanilla fingerprint + build-diff classification
  Img.{h,cpp}           IMG archive (VER2) reader/rebuilder
  Sfx.{h,cpp}           SAAT audio bank reader/rebuilder
  GameRules.{h,cpp}     GTA SA file-type knowledge (routing, data-file merge rules)
  Unpack.{h,cpp}        archive extraction/creation (libarchive), password support
  Process.{h,cpp}       Win32 process launch, registry helpers (SA-MP)
  UserFiles.{h,cpp}     "GTA San Andreas User Files" folder resolution + junctions
  Steam.{h,cpp}         Steam presence session (holds the game's AppID while playing)
  SannyBuilder.{h,cpp}  Sanny Builder 4 settings.ini / mode.xml patching
  Model.h                data model (Mod, Profile, ProfileEntry, ...)
  Deploy.h               deployment plan / manifest types
  Version.{h,cpp}        generated build version (kept out of the main sources so
                          bumping the build number doesn't force a full rebuild)
  main.cpp                CLI entry point
  gui/                    Qt 6 GUI (MainWindow, Instances, ServerBrowser, ModTags,
                          Lang, Theme, ...)
launcher/                 single-file launcher (self-extracting, no library deps)
cmake/                    CMake templates (generated build-version header)
resources/                icon, Windows resource script, UTF-8 manifest
scripts/                  build.bat / release.bat / buildall.bat / run-gui.bat
tests/                    Catch2 test suite
```

## Data directory layout

```
<data>/config.json              { gamePath, activeProfile, feature toggles... }
<data>/mods/<id>/meta.json       mod metadata (name, source, content hash, ...)
<data>/mods/<id>/root/           the mod's files, relative to the game directory
<data>/profiles/<name>.json      { name, entries: [{modId, enabled, priority, ...}] }
<data>/profiles/<name>.info/      the profile's rich-text "about this build" page
<data>/deployed.json             deployment manifest (drives rollback)
<data>/backup/                   originals replaced by the current deployment
<data>/img-pristine/              untouched copies of rebuilt IMG archives
<data>/audio-pristine/            untouched copies of rebuilt SFX paks
<data>/vanilla-baseline.json       cached vanilla fingerprint (import-build / cleanup)
<data>/saves/<profile>/           per-profile "User Files" (saves/settings) store
<data>/generated/<profile>/       files captured back from the game folder
```

## Testing

```bat
build\gtamm_tests.exe --reporter compact
```

`build.bat` runs the full suite automatically after every build.

## Authors

- [**nu11c0de**](https://github.com/nu11c0de) — author and maintainer.

Contributions are welcome — open an issue or a pull request.

## License

**MIT** — see [LICENSE](LICENSE).

Third-party components keep their own licenses: Qt 6 is used under the LGPL
(linked dynamically, its DLLs shipped unmodified), libarchive under BSD-2,
nlohmann/json under MIT, Catch2 under BSL-1.0. The optional runtime bundles
(Mod Loader, the SA-MP client, Valve's `steam_api64.dll`) are third-party
binaries that are neither included here nor covered by this license. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full list.

## Acknowledgments

GMM is an independent implementation. Its knowledge of GTA SA's file layout
(which files live inside IMG archives, which data files merge and how) was
informed by **Mod Loader** by LINK/2012 (MIT-licensed) — no source code is
shared. Some architectural ideas were studied from **Mod Organizer 2**
(clean-room; its GPL-licensed source is not used).
