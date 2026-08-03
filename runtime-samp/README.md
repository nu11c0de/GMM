# SA-MP client

Everything in this folder (recursively, whole tree preserved) is embedded
directly into `GMM.exe` at build time (Qt resource `:/runtime-samp/*`). On
deploy, GMM installs it into the game if the active profile's "SA-MP settings"
are enabled (`App::deploySampRuntime`, called from `App::deploy()`).

The user doesn't need to manually top up missing SA-MP client files in a mod —
drop the full official client here once.

## Source

Get the SA-MP client (`samp.exe`, `samp.dll`, and everything else below) from
an official SA-MP install — it's installed per server/version; once installed,
the files you need are already in the game folder, just copy them here.

## Why a separate bundle instead of a regular mod import

The regular mod-import pipeline (especially the "import an existing build"
diff against vanilla) is tuned for downloaded mod packs — it has heuristics
like "this image looks like a screenshot, skip it" and "there's a single
subfolder in here, it's a wrapper, unwrap it." Those regularly backfire on
SA-MP's own client files: `mouse.png`/`sampgui.png` were genuinely mistaken for
preview screenshots and lost, and the `SAMP/` folder (with its `SAMP.img` /
`custom.img` / `SAMPCOL.img` archives — SA-MP reuses the IMG format for its own
custom-object library) was mistaken for a wrapper and unwrapped, scattering the
archives to the wrong place. This bundle sidesteps those heuristics entirely:
files are copied as-is, at the exact same relative paths as the original SA-MP
install.

## What to put here

The complete, official SA-MP client install, exactly as it looks in the game
folder after installing SA-MP. **`samp.exe` alone is NOT enough** — it's just
the injector; all the actual client functionality lives in `samp.dll`, and
without it `samp.exe` won't launch anything.

| File/folder | Purpose |
| --- | --- |
| `samp.exe` | the injector — its presence is what GMM checks for, but it's useless on its own without `samp.dll` |
| `samp.dll` | the actual SA-MP client, injected into `gta_sa.exe` — without this file, `samp.exe` doesn't launch the game |
| `samp_debug.exe` | debug build of the injector (optional, for troubleshooting) |
| `rcon.exe` | remote console (RCON) tool for server administration |
| `mouse.png` | SA-MP cursor texture |
| `sampgui.png` | SA-MP GUI textures |
| `sampaux3.ttf` | auxiliary SA-MP UI font |
| `gtaweap3.ttf` | font used for weapon icons in the SA-MP HUD |
| `samp.saa` | client data/cache file |
| `lang/en.json`, `lang/ru.json` | client UI language files |
| `SAMP/SAMP.img` | SA-MP custom-object archive (VER2, same format as the game's own IMG archives) |
| `SAMP/custom.img` | another custom-object archive |
| `SAMP/SAMPCOL.img` | custom-object collision archive |
| `SAMP/SAMP.ide` | object registration |
| `SAMP/SAMP.ipl` | object placement on the map |
| `SAMP/samaps.txd` | SA-MP map textures |
| `SAMP/blanktex.txd` | a placeholder texture |
| `SAMP/CUSTOM.ide` | additional object registration |

The list above is exactly what a real SA-MP installer puts on disk — there is
no single "correct" file list other than whatever the installer itself
produces; if you're installing a different SA-MP version, check against what
its own installer actually wrote, not against this table.

## Rules

- **Everything** here gets embedded into the exe except this `README.md`.
- Empty → nothing gets embedded, the build still succeeds (GMM then looks for
  the bundle on disk instead: `<exe>/runtime-samp` or the instance's store
  `<data>/samp-runtime`).
- Changed the contents → rebuild the project.
- Files are **not committed** to the repository (third-party redistributable /
  size) — only this `README.md`.
- On deploy, the bundle **never** overwrites a file an ordinary mod already
  placed this same deploy (see `App::deploySampRuntime`) — it only **fills in**
  whatever is missing. An already-installed (even partial) SA-MP mod doesn't
  conflict with this bundle, it just gets whatever it's missing topped up.

At startup, `materializeRuntime()` (the same mechanism used for the Mod Loader
runtime) unpacks `:/runtime-samp/*` into `<program folder>/runtime-samp` (or,
if that isn't writable, the instance's store), preserving the whole tree,
including the `SAMP/` folder. Whether the bundle is present is determined
solely by the presence of `samp.exe`.
