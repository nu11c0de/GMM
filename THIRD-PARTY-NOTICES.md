# Third-party notices

GMM itself is released under the [MIT License](LICENSE). It is built on, and
distributed alongside, third-party components that keep their own licenses.

## Libraries linked into GMM

| Component | Used for | License |
|---|---|---|
| [Qt 6](https://www.qt.io/) (`Qt6Widgets`, `Qt6Network`, `Qt6Gui`, `Qt6Core` + platform/style plugins) | the GUI | LGPL-3.0 — **dynamically linked**, DLLs shipped unmodified next to the exe |
| [libarchive](https://www.libarchive.org/) | importing zip/7z/rar/tar mod archives | BSD-2-Clause |
| [nlohmann/json](https://github.com/nlohmann/json) | all on-disk JSON (config, profiles, manifests) | MIT |
| [Catch2](https://github.com/catchorg/Catch2) | test suite only (not shipped) | BSL-1.0 |
| zlib, bzip2, liblzma, zstd | compression backends pulled in by libarchive | zlib / BSD-style / public domain / BSD |
| Microsoft Visual C++ runtime | required by MSVC-built binaries | Microsoft redistributable terms |

Qt is used under the LGPL: it is linked dynamically and its DLLs are shipped
unmodified, so a user may replace them with their own build of the same Qt
version. No Qt source is modified in this repository.

## Knowledge and reference material (no code reused)

- **[Mod Loader](https://github.com/thelink2012/modloader)** by LINK/2012 (MIT) —
  GMM's knowledge of GTA SA file types (what lives inside IMG archives, which
  data files merge and how) is informed by Mod Loader's behaviour. No code is
  copied.
- **[Mod Organizer 2](https://github.com/ModOrganizer2/modorganizer)** (GPL-3.0) —
  studied clean-room for architecture ideas only (mod pool, profiles, load
  order, conflict resolution). **No MO2 code is used**, which is why GMM can be
  MIT-licensed. The MO2 sources kept in `modorganizer-2.5.2/` are reference
  material and are not part of GMM's build.

## Binaries that are NOT covered by GMM's license

These are third-party programs that a user may place into the runtime bundle
folders (`gtamm/runtime/`, `gtamm/runtime-samp/`) so GMM can install them into
the game. They are **not** part of this repository (they are git-ignored) and
each remains under its own author's terms:

- Mod Loader (`modloader.asi` + its `.data` plugins), CLEO, and the
  compatibility/QoL plugins under `modloader/_ESSENTIALS/` (SilentPatch,
  widescreen fixes, FramerateVigilante, RunDLL32 Fix, Windowed Mode).
- The SA-MP client (`samp.exe`, `samp.dll`, the `SAMP/` archives, ...).
- Valve's Steamworks redistributable `steam_api64.dll` (`gtamm/runtime-steam/`),
  loaded at runtime — never linked — to show a session in Steam. It is
  distributed under the Steamworks SDK Access Agreement, not under GMM's
  license.
- `gta_sa.exe` and any other Grand Theft Auto: San Andreas game file.
  Grand Theft Auto: San Andreas is © Rockstar Games. GMM is an unofficial,
  unaffiliated tool; no game content is distributed with it.
