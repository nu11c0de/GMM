# Mod Loader runtime

Everything in this folder (recursively, whole tree preserved) is embedded
directly into `GMM.exe` at build time (Qt resource `:/runtime/*`). At startup,
the program unpacks it back out next to itself and installs it into the game
whenever a mod is deployed "via Mod Loader" (see `App::deployModloaderRuntime`).

The user doesn't need to configure anything manually: on a bare vanilla 1.0
install, checking "via Mod Loader" on a mod is enough — on deploy, GMM unpacks
the whole working set from this folder into the game itself.

## Source

- **Mod Loader** (`modloader.asi` itself, the ASI loader, `modloader/.data/...`) —
  <https://github.com/thelink2012/modloader>
- **Essentials Pack** (for `modloader/_ESSENTIALS/` — SilentPatch, Widescreen
  Fix, etc.) — <https://libertycity.net/files/gta-san-andreas/154094-essentials-essential-mods-pack.html>

## What to put here

A complete, self-consistent set (`modloader.asi` and its `std.*.dll` plugins —
from ONE build, don't mix different releases):

| File/folder | Purpose |
| --- | --- |
| `modloader.asi` | Mod Loader itself (its presence is what GMM checks for) |
| `vorbisFile.dll` | ASI loader (loads `.asi` files from the game root) |
| `vorbisHooked.dll` | CLEO/ASI hook |
| `modloader/.data/...` | config, plugins (`std.asi`/`std.data`/`std.stream`/...), languages (1033 EN + 1046 RU) |
| `modloader/_ESSENTIALS/` | QoL mods that Mod Loader itself (`std.asi`) loads automatically: SilentPatch, Widescreen Fix (ThirteenAG + HOR+/Wesser), FramerateVigilante, RunDLL32 Fix, Windowed Mode |
| `CLEO.asi` + `CLEO/*.cleo` | the CLEO loader and its extensions (CLEO+, FileSystemOperations, IniFiles, IntOperations) — without these, a mod's CLEO scripts either fail to load or only partially load |
| `_noDEP.asi` | disables Data Execution Prevention for the old 32-bit exe (a common cause of crashes on newer Windows) |
| `bass.dll` | the audio library CLEO/ASI plugins depend on |
| `gta_sa.exe` | a clean GTA SA 1.0 US executable (~14 MB; optional — bloats the exe; must be byte-verified against the original 2005-06-07 release) |

## Rules

- **Everything** here gets embedded into the exe except this `README.md`.
- Empty → nothing gets embedded, the build still succeeds (GMM then looks for
  the runtime on disk instead: `<exe>/runtime` or the instance's store).
- Changed the contents → rebuild the project (the build scripts reconfigure
  CMake, which regenerates the Qt resource).
- Files are **not committed** to the repository (copyright/size) — only this
  `README.md`.
- Changing the Mod Loader version entirely → take `modloader.asi` together
  with its `modloader/.data/plugins/gta3/std.*.dll` (one release = one
  consistent set; `std.*.dll` from a different `modloader.asi` build either
  won't load or will crash the game).

At startup, `materializeRuntime()` unpacks `:/runtime/*` into
`<program folder>/runtime` (or, if that isn't writable, the instance's store),
preserving the whole tree. Whether the runtime is present is determined solely
by the presence of `modloader.asi`.
