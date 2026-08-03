#pragma once
#include <filesystem>
#include <string>

namespace gtamm {

// Launch an executable with the given working directory, block until it exits,
// and return its exit code. Throws std::runtime_error if the process cannot be
// started. The working directory matters for GTA SA: the game resolves its data
// relative to the current directory, so it must be the game folder.
int launchAndWait(const std::filesystem::path& exe,
                  const std::filesystem::path& workingDir);

// Launch the SA-MP injector (samp.exe) with `argsLine` (a ready command-line
// argument string, e.g. L"127.0.0.1 7777"), then wait for the gta_sa.exe process
// it spawns — NOT samp.exe, which injects samp.dll and exits within a second. If
// we waited on samp.exe the caller would roll back the deployed mods out from
// under the running game. Returns the GTA process's exit code (0 if the game
// process could not be observed, e.g. samp failed to start it). Throws if
// samp.exe itself cannot be launched.
int launchSampAndWait(const std::filesystem::path& sampExe,
                      const std::wstring& argsLine,
                      const std::filesystem::path& workingDir,
                      const std::wstring& gameExeName = L"gta_sa.exe");

// True if any currently-running process has this image name (e.g. L"steam.exe").
bool isProcessRunning(const std::wstring& exeName);

// Absolute directory containing the running executable (GMM.exe / gtamm.exe).
// Used to locate resources bundled next to the program (e.g. the Mod Loader
// runtime). Falls back to the current directory if it cannot be determined.
std::filesystem::path executableDir();

// Where this program's portable state should live: instances (data/<name>/)
// and the GUI's own UI settings (data/GMM/GMM.ini). Normally executableDir(),
// so a copied/moved program folder keeps its data with it -- but the
// single-file launcher (gtamm/launcher/) runs the real GMM.exe from an
// internal "bin\" subfolder it re-extracts on every version bump, so data
// living there would be wiped along with it. The launcher instead points
// this at *its own* folder via the GMM_PORTABLE_ROOT environment variable
// (inherited by the child process), which this checks first.
std::filesystem::path portableRoot();

// Write the SA-MP player name to HKCU\Software\SAMP\PlayerName (REG_SZ). This is
// where samp.exe reads the nickname from — it is not a command-line argument.
// `nick` is UTF-8. No-op on empty. Best-effort: failures are silently ignored.
void setSampNick(const std::string& nick);

// Write the absolute path samp.exe should use to find/launch gta_sa.exe, to
// HKCU\Software\SAMP\gta_sa_exe (REG_SZ) -- the same key samp.exe itself
// writes there the first time you point it at your game (its own "locate
// gta_sa.exe" file-picker dialog). Without this, samp.exe falls back to
// whatever path it remembers from a PREVIOUS install (or none at all on a
// fresh Windows account), and if that path doesn't exist anymore -- e.g. a
// portable/mod-managed install living somewhere new each time -- it prompts
// "GTA: San Andreas executable not found. Please locate it now." instead of
// launching. `exePath` should be the absolute path to the actual gta_sa.exe
// about to be used (already resolved against the game's working directory).
// Best-effort: failures are silently ignored, matching setSampNick().
void setSampGtaExePath(const std::filesystem::path& exePath);

// Forcibly terminates whichever game process launchAndWait()/
// launchSampAndWait() is currently blocked waiting on (if any), letting that
// wait return early instead of for the process's natural exit. Safe to call
// from a different thread than the one blocked in the wait -- that's the
// whole point: play() runs on a worker thread, this is meant to be called
// from the GUI thread in response to a "kill the game" button. Returns false
// if no game process is currently tracked (nothing running, or it already
// exited).
bool killRunningGame();

// Forcibly terminates every currently-running process whose image name
// matches `exeName` (case-insensitive), e.g. L"samp.exe". SA-MP's injector is
// supposed to exit within a second of launching the game (see
// launchSampAndWait); if it hangs instead of exiting cleanly, a NEW samp.exe
// started on the next Play collides with the old one's still-held state
// (registry/shared memory) and the client can crash instead of connecting
// (EAccessViolation in samp.exe) -- so this is used to sweep away leftovers
// after a SA-MP session ends, before the next launch. Returns the number of
// processes actually terminated.
int killProcessesByName(const std::wstring& exeName);

// Attempts to become the sole GMM process operating on `dataDir` for as long
// as the current process runs. Backed by a named Windows mutex keyed off
// dataDir's absolute path (hashed -- a raw path isn't a valid kernel-object
// name), held for the whole process lifetime and released automatically by
// the OS on exit (even a crash), so there's nothing to explicitly release.
// Returns false if another GMM process already holds it for this exact
// instance -- the caller should refuse to start rather than risk two
// processes deploying/writing the same instance concurrently. Different
// instances (different data dirs) never contend with each other. Fails open
// (returns true) if the mutex itself can't be created, so a lock-acquisition
// problem never blocks a legitimate launch.
bool acquireSingleInstanceLock(const std::filesystem::path& dataDir);

}  // namespace gtamm
