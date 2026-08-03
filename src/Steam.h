#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Steam "in-game" presence for a copy of the game Steam did not install.
//
// The point of reference is Steam Achievement Manager: SAM makes Steam show
// "Playing <game>" without Steam launching that game at all. It does so by
// being a process that loads steam_api and calls SteamAPI_Init() with the
// game's AppID -- the Steam client then treats that process as the game for as
// long as the API session lives. Ownership is enforced by the client: Init
// fails for an AppID the signed-in account does not own.
//
// GMM does exactly that around a play session: while the game runs, GMM's own
// process holds an API session for GTA San Andreas (AppID 12120), so friends
// and the client see "Playing Grand Theft Auto: San Andreas", and playtime is
// counted against the real store entry. Nothing in Steam's own files or in the
// game folder is touched, the managed game folder can live anywhere, and
// Steam's "verify integrity of game files" can't undo any of it.
//
// What this deliberately does NOT provide is the overlay (Shift+Tab, F12
// screenshots): Steam only injects GameOverlayRenderer into processes it
// launched itself, and the game here is launched by GMM.
//
// Why this needs a DLL: steam_api is Valve's redistributable, not something
// this MIT-licensed repository ships. It is loaded at runtime from a bundle
// folder the user populates once (see App::steamApiDll()), exactly like the
// Mod Loader and SA-MP runtimes. GMM is 64-bit, so it needs steam_api64.dll --
// the 32-bit steam_api.dll shipped inside a Steam copy of GTA SA (a 32-bit
// game) cannot be loaded into this process.
namespace gtamm::steam {

// Grand Theft Auto: San Andreas on Steam.
inline constexpr std::uint32_t kGtaSanAndreasAppId = 12120;

// Steam's install folder, from HKCU\Software\Valve\Steam\SteamPath (falling
// back to the machine-wide InstallPath). Empty if Steam isn't installed.
std::filesystem::path installPath();
bool isInstalled();
// Whether the Steam client is up right now -- an API session can only be
// established while it is.
bool isRunning();

// The file name this process needs (steam_api64.dll for a 64-bit build).
const char* apiDllName();

// Every Steam library on this machine: the Steam install itself plus each
// path listed in steamapps/libraryfolders.vdf. Empty if Steam isn't installed.
std::vector<std::filesystem::path> libraryFolders();

// Look for a usable steam_api64.dll among the games already installed through
// Steam, so the user doesn't have to supply one by hand: virtually every
// 64-bit Steam game ships it, and it is version-neutral for our purposes (a
// thin shim that forwards to the client's own steamclient64.dll -- any
// reasonably recent copy can open a session for any AppID).
//
// Returns up to `maxHits` candidates rather than one, because a library can
// also contain a *replacement* steam_api64.dll (an emulator shipped with a
// cracked game) that would refuse the session -- the caller tries them in
// order until one works. Bounded: it walks only steamapps/common/<game> trees,
// a few levels deep, with a hard cap on entries visited, so it stays quick
// even on a large library. Empty if Steam isn't installed or nothing was found.
std::vector<std::filesystem::path> findApiDlls(int maxHits = 8);

// Holds an "in-game" registration with the running Steam client for as long as
// it is alive. Non-copyable; end() is idempotent and also runs from the
// destructor, so an exception on the launch path can never leave GMM stuck
// showing as in-game.
class Presence
{
public:
  Presence() = default;
  ~Presence();
  Presence(const Presence&)            = delete;
  Presence& operator=(const Presence&) = delete;

  // Loads `apiDll`, declares this process to be `appId` and starts the API
  // session. Returns false (with a human-readable reason in `error`, if given)
  // when the DLL is missing/unloadable, Steam isn't running, or the account
  // doesn't own the AppID -- all of which are reasons to launch the game
  // anyway, just without the Steam status.
  bool begin(std::uint32_t appId, const std::filesystem::path& apiDll,
             std::string* error = nullptr);
  void end();
  bool active() const { return m_inited; }

private:
  void* m_dll   = nullptr;  // HMODULE
  bool m_inited = false;
};

}  // namespace gtamm::steam
