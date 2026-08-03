# Steam "in-game" status (`steam_api64.dll`)

**You usually do not need this folder.** If Steam is installed, GMM finds a
`steam_api64.dll` by itself: it borrows the copy that ships with any 64-bit game
already installed through Steam (libraries are read from
`steamapps/libraryfolders.vdf`, then `steamapps/common/<game>` is walked
depth-limited with a hard entry budget). The DLL that successfully opened a
session is remembered in the instance's `config.json`, so the scan runs at most
once. This folder is the fallback for when that finds nothing — an empty Steam
library, or one where the only copies are emulator shims that refuse the
session.

Anything placed here is embedded directly into `GMM.exe` at build time (Qt
resource `:/runtime-steam/*`) and unpacked next to the program on first launch,
exactly like `runtime/` and `runtime-samp/`, so a single-file build stays a
single file.

## What to put here

    steam_api64.dll

It **must** be the 64-bit build. GMM is a 64-bit program, so the 32-bit
`steam_api.dll` that ships inside a Steam copy of GTA San Andreas (a 32-bit
game) cannot be loaded into it.

## Source

Valve's Steamworks redistributable: `redistributable_bin/win64/steam_api64.dll`
in the Steamworks SDK (<https://partner.steamgames.com/downloads/list>), or a
copy from any 64-bit Steam game you own.

It is **not** checked into this repository — it is Valve's binary, distributed
under the Steamworks SDK Access Agreement, not under GMM's MIT license.

## What it is used for

While a game session runs, GMM's own process opens a Steam API session under
Grand Theft Auto: San Andreas's real AppID (12120) — the same thing Steam
Achievement Manager does — so the client and your friends show "Playing Grand
Theft Auto: San Andreas" and the time counts against the real store entry.
Nothing in Steam's files or in the game folder is modified. Enable it in
Settings → Deployment → Steam.

Requirements at runtime:

- the Steam client is running;
- the signed-in account **owns** Grand Theft Auto: San Andreas — the client
  refuses the session otherwise.

There is deliberately no Steam overlay (Shift+Tab, F12 screenshots): Steam only
injects the overlay into processes it launched itself, and here the game is
launched by GMM.

Leave this folder empty and, if auto-detection also comes up empty, the feature
simply reports the DLL as missing and the game launches without a Steam status.
Everything else works regardless.
