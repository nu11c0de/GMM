#include "Steam.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

#include "Process.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace gtamm::steam {

namespace {

std::wstring regString(HKEY root, const wchar_t* sub, const wchar_t* value)
{
  wchar_t buf[MAX_PATH]{};
  DWORD bytes = sizeof(buf);
  if (RegGetValueW(root, sub, value, RRF_RT_REG_SZ, nullptr, buf, &bytes) !=
      ERROR_SUCCESS)
    return std::wstring();
  return std::wstring(buf);
}

// steam_api's entry points. Only these three are needed for a presence
// session, and they are resolved by name at runtime -- GMM neither links nor
// ships the Steamworks SDK.
using SteamAPI_Init_t     = bool(__cdecl*)();
using SteamAPI_Shutdown_t = void(__cdecl*)();
// SDK 1.59+ renamed the real entry point; SteamAPI_Init became a header-side
// wrapper around it. Both spellings are exported by their respective
// versions, so try the classic one first and fall back.
using SteamAPI_InitFlat_t = int(__cdecl*)(char (*errMsg)[1024]);

// The AppID is announced through the environment, so it must be cleared again
// on every exit path: the game is launched as a child process and would
// otherwise inherit it.
void clearAppIdEnv()
{
  SetEnvironmentVariableW(L"SteamAppId", nullptr);
  SetEnvironmentVariableW(L"SteamGameId", nullptr);
}

}  // namespace

fs::path installPath()
{
  std::wstring p = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (p.empty())
    p = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                  L"InstallPath");
  if (p.empty())
    p = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
  if (p.empty())
    return {};
  std::error_code ec;
  fs::path path(p);
  if (!fs::exists(path, ec))
    return {};
  return path;
}

bool isInstalled()
{
  return !installPath().empty();
}

bool isRunning()
{
  return isProcessRunning(L"steam.exe");
}

const char* apiDllName()
{
  return sizeof(void*) == 8 ? "steam_api64.dll" : "steam_api.dll";
}

std::vector<fs::path> libraryFolders()
{
  std::vector<fs::path> out;
  const fs::path steam = installPath();
  if (steam.empty())
    return out;
  out.push_back(steam);

  // steamapps/libraryfolders.vdf is TEXT VDF; each library block carries a
  // "path" key, e.g.:   "path"    "D:\\SteamLibrary"
  // Only that one key is needed, so a line scan beats a full VDF parser here.
  std::ifstream in(steam / "steamapps" / "libraryfolders.vdf");
  std::string line;
  while (std::getline(in, line)) {
    const size_t key = line.find("\"path\"");
    if (key == std::string::npos)
      continue;
    const size_t open = line.find('"', key + 6);
    if (open == std::string::npos)
      continue;
    const size_t close = line.find('"', open + 1);
    if (close == std::string::npos)
      continue;
    std::string p = line.substr(open + 1, close - open - 1);
    // VDF escapes the separators; turn "D:\\Games" back into "D:\Games".
    for (size_t i = p.find("\\\\"); i != std::string::npos; i = p.find("\\\\", i + 1))
      p.erase(i, 1);
    if (p.empty())
      continue;
    std::error_code ec;
    const fs::path lib(p);
    if (!fs::is_directory(lib, ec))
      continue;
    if (std::find(out.begin(), out.end(), lib) == out.end())
      out.push_back(lib);
  }
  return out;
}

std::vector<fs::path> findApiDlls(int maxHits)
{
  std::vector<fs::path> hits;
  const std::string name = apiDllName();
  std::error_code ec;

  // Hard budget on filesystem entries visited: a Steam library can hold
  // hundreds of thousands of files, and this runs on the launch path.
  int budget = 60000;

  for (const fs::path& lib : libraryFolders()) {
    const fs::path common = lib / "steamapps" / "common";
    if (!fs::is_directory(common, ec))
      continue;
    for (const auto& game : fs::directory_iterator(common, ec)) {
      if (!game.is_directory(ec))
        continue;
      auto it  = fs::recursive_directory_iterator(
          game.path(), fs::directory_options::skip_permission_denied, ec);
      const auto end = fs::recursive_directory_iterator();
      for (; !ec && it != end; it.increment(ec)) {
        if (--budget <= 0)
          return hits;
        // The DLL sits at the game root or one or two folders down (Binaries/
        // Win64/, bin/x64/, ...); deeper than that is asset territory.
        if (it.depth() >= 3)
          it.disable_recursion_pending();
        if (!it->is_regular_file(ec))
          continue;
        const std::string fn = it->path().filename().string();
        if (fn.size() == name.size() &&
            std::equal(fn.begin(), fn.end(), name.begin(), [](char a, char b) {
              return std::tolower(static_cast<unsigned char>(a)) ==
                     std::tolower(static_cast<unsigned char>(b));
            })) {
          hits.push_back(it->path());
          if (static_cast<int>(hits.size()) >= maxHits)
            return hits;
          break;  // one per game is plenty
        }
      }
      ec.clear();
    }
  }
  return hits;
}

Presence::~Presence()
{
  end();
}

bool Presence::begin(std::uint32_t appId, const fs::path& apiDll, std::string* error)
{
  auto fail = [&](const std::string& msg) {
    if (error)
      *error = msg;
    return false;
  };

  if (m_inited)
    return true;

  std::error_code ec;
  if (apiDll.empty() || !fs::exists(apiDll, ec))
    return fail(std::string(apiDllName()) + " not found");
  if (!isRunning())
    return fail("the Steam client is not running");

  // How steam_api learns which app it is speaking for when the process was NOT
  // launched by Steam: the SteamAppId environment variable (the same job
  // steam_appid.txt does for a game, without writing a file into the game
  // folder). Must be set before SteamAPI_Init().
  const std::wstring id = std::to_wstring(appId);
  SetEnvironmentVariableW(L"SteamAppId", id.c_str());
  SetEnvironmentVariableW(L"SteamGameId", id.c_str());

  HMODULE dll = LoadLibraryW(apiDll.wstring().c_str());
  if (!dll) {
    const DWORD err = GetLastError();
    clearAppIdEnv();
    return fail("cannot load " + apiDll.string() + " (error " +
                std::to_string(err) + ") -- is it the 64-bit " +
                apiDllName() + "?");
  }

  bool ok = false;
  if (auto init = reinterpret_cast<SteamAPI_Init_t>(
          reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_Init")))) {
    ok = init();
  } else if (auto initFlat = reinterpret_cast<SteamAPI_InitFlat_t>(
                 reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_InitFlat")))) {
    char msg[1024]{};
    ok = initFlat(&msg) == 0;  // k_ESteamAPIInitResult_OK
  } else {
    FreeLibrary(dll);
    clearAppIdEnv();
    return fail("no SteamAPI_Init entry point in " + apiDll.string());
  }

  if (!ok) {
    FreeLibrary(dll);
    clearAppIdEnv();
    // Init's failure modes all come down to the client refusing the session:
    // no client, this account doesn't own the app, or the DLL turned out to be
    // an emulator shim rather than Valve's.
    return fail("Steam refused the session -- the signed-in account probably "
                "does not own Grand Theft Auto: San Andreas");
  }

  m_dll    = dll;
  m_inited = true;
  return true;
}

void Presence::end()
{
  if (m_inited) {
    if (auto shutdown = reinterpret_cast<SteamAPI_Shutdown_t>(reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(m_dll), "SteamAPI_Shutdown"))))
      shutdown();
    m_inited = false;
  }
  if (m_dll) {
    FreeLibrary(static_cast<HMODULE>(m_dll));
    m_dll = nullptr;
  }
  clearAppIdEnv();
}

}  // namespace gtamm::steam
