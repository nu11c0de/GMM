#include "Process.h"

#include <atomic>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace gtamm {

namespace {

// RAII wrapper so handles are always closed, even on early return/throw.
struct Handle
{
  HANDLE h = nullptr;
  ~Handle()
  {
    if (h)
      CloseHandle(h);
  }
};

// Case-insensitive ASCII compare of two wide strings (process image names).
bool iEqualsW(const std::wstring& a, const std::wstring& b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (towlower(a[i]) != towlower(b[i]))
      return false;
  return true;
}

// PIDs of all currently-running processes whose image name equals `exeName`.
std::set<DWORD> pidsByName(const std::wstring& exeName)
{
  std::set<DWORD> pids;
  Handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
  if (!snap.h || snap.h == INVALID_HANDLE_VALUE)
    return pids;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap.h, &pe)) {
    do {
      if (iEqualsW(pe.szExeFile, exeName))
        pids.insert(pe.th32ProcessID);
    } while (Process32NextW(snap.h, &pe));
  }
  return pids;
}

// HANDLE of the game process currently blocked on in launchAndWait()/
// launchSampAndWait() (not the samp.exe injector -- see launchSampAndWait),
// or nullptr if nothing is running. Not owning: the Handle RAII wrapper on
// each function's stack still closes it; this is just a non-owning reference
// killRunningGame() (called from the GUI thread) can hand to TerminateProcess
// while the worker thread sits in WaitForSingleObject on the same handle.
std::atomic<HANDLE> g_runningGame{nullptr};

}  // namespace

int launchAndWait(const fs::path& exe, const fs::path& workingDir)
{
  std::error_code ec;
  if (!fs::exists(exe, ec))
    throw std::runtime_error("executable not found: " + exe.string());

  // CreateProcessW may write into the command-line buffer, so pass a mutable
  // copy. The command line is just the quoted executable path (no args yet).
  const std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  const std::wstring cwd = workingDir.wstring();

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  const BOOL ok = CreateProcessW(exe.wstring().c_str(),  // explicit application
                                 cmdBuf.data(),          // command line
                                 nullptr, nullptr, FALSE, 0, nullptr,
                                 cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  if (!ok)
    throw std::runtime_error("failed to launch '" + exe.string() +
                             "' (CreateProcess error " +
                             std::to_string(GetLastError()) + ")");

  Handle process{pi.hProcess};
  Handle thread{pi.hThread};

  g_runningGame.store(process.h);
  WaitForSingleObject(process.h, INFINITE);
  g_runningGame.store(nullptr);

  DWORD code = 0;
  GetExitCodeProcess(process.h, &code);
  return static_cast<int>(code);
}

int launchSampAndWait(const fs::path& sampExe, const std::wstring& argsLine,
                      const fs::path& workingDir, const std::wstring& gameExeName)
{
  std::error_code ec;
  if (!fs::exists(sampExe, ec))
    throw std::runtime_error("SA-MP launcher not found: " + sampExe.string());

  // Remember gta_sa.exe instances already running so we can tell which one
  // samp.exe spawns (a copy may already be open unrelated to us).
  const std::set<DWORD> before = pidsByName(gameExeName);

  std::wstring cmd = L"\"" + sampExe.wstring() + L"\"";
  if (!argsLine.empty())
    cmd += L" " + argsLine;
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  const std::wstring cwd = workingDir.wstring();

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  const BOOL ok = CreateProcessW(sampExe.wstring().c_str(), cmdBuf.data(), nullptr,
                                 nullptr, FALSE, 0, nullptr,
                                 cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  if (!ok)
    throw std::runtime_error("failed to launch '" + sampExe.string() +
                             "' (CreateProcess error " +
                             std::to_string(GetLastError()) + ")");
  Handle sampProcess{pi.hProcess};
  Handle sampThread{pi.hThread};

  // Poll for the gta_sa.exe that samp.exe injects into. It usually appears within
  // a couple of seconds; give it a generous window (the SA-MP loader can be slow).
  Handle game;
  const int kMaxPolls = 240;  // 240 * 250ms = 60s
  for (int i = 0; i < kMaxPolls; ++i) {
    for (DWORD pid : pidsByName(gameExeName)) {
      if (before.count(pid))
        continue;  // was already running before we started
      HANDLE h = OpenProcess(
          SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
          FALSE, pid);
      if (h) {
        game.h = h;
        break;
      }
    }
    if (game.h)
      break;
    // If samp.exe exited without ever spawning the game, stop waiting.
    if (WaitForSingleObject(sampProcess.h, 0) == WAIT_OBJECT_0 && i > 4)
      break;
    Sleep(250);
  }

  if (!game.h) {
    // Couldn't observe the game process (samp may have failed to start it, or it
    // exited before we caught it). Fall back to samp.exe's own exit code.
    g_runningGame.store(sampProcess.h);
    WaitForSingleObject(sampProcess.h, INFINITE);
    g_runningGame.store(nullptr);
    DWORD code = 0;
    GetExitCodeProcess(sampProcess.h, &code);
    return static_cast<int>(code);
  }

  g_runningGame.store(game.h);
  WaitForSingleObject(game.h, INFINITE);
  g_runningGame.store(nullptr);
  DWORD code = 0;
  GetExitCodeProcess(game.h, &code);
  return static_cast<int>(code);
}

bool isProcessRunning(const std::wstring& exeName)
{
  return !pidsByName(exeName).empty();
}

fs::path executableDir()
{
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n =
        GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0)
      return fs::current_path();
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);  // path was truncated; grow and retry
  }
  return fs::path(buf).parent_path();
}

fs::path portableRoot()
{
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"GMM_PORTABLE_ROOT", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    return fs::path(std::wstring(buf, n));
  return executableDir();
}

void setSampNick(const std::string& nick)
{
  if (nick.empty())
    return;
  // UTF-8 -> UTF-16 for the registry value.
  const int len =
      MultiByteToWideChar(CP_UTF8, 0, nick.c_str(), -1, nullptr, 0);
  if (len <= 0)
    return;
  std::wstring wnick(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, nick.c_str(), -1, wnick.data(), len);
  wnick.resize(static_cast<size_t>(len) - 1);  // drop terminating NUL

  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SAMP", 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  const DWORD bytes =
      static_cast<DWORD>((wnick.size() + 1) * sizeof(wchar_t));
  RegSetValueExW(key, L"PlayerName", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(wnick.c_str()), bytes);
  RegCloseKey(key);
}

bool killRunningGame()
{
  const HANDLE h = g_runningGame.load();
  if (!h)
    return false;
  return TerminateProcess(h, 1) != 0;
}

int killProcessesByName(const std::wstring& exeName)
{
  int killed = 0;
  for (DWORD pid : pidsByName(exeName)) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h)
      continue;
    if (TerminateProcess(h, 0))
      ++killed;
    CloseHandle(h);
  }
  return killed;
}

void setSampGtaExePath(const fs::path& exePath)
{
  if (exePath.empty())
    return;
  const std::wstring wpath = exePath.wstring();

  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SAMP", 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  const DWORD bytes = static_cast<DWORD>((wpath.size() + 1) * sizeof(wchar_t));
  RegSetValueExW(key, L"gta_sa_exe", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(wpath.c_str()), bytes);
  RegCloseKey(key);
}

bool acquireSingleInstanceLock(const fs::path& dataDir)
{
  std::error_code ec;
  const fs::path abs = fs::absolute(dataDir, ec);
  const std::string key = (ec ? dataDir : abs).generic_string();

  // FNV-1a hash of the absolute path -> a fixed-length hex string. A raw path
  // isn't usable as-is: kernel-object names can't contain backslashes (they're
  // namespace separators) and have a length limit, and case/formatting
  // differences (e.g. from a relative --data) would otherwise defeat matching.
  std::uint64_t h = 1469598103934665603ull;  // FNV offset basis
  for (unsigned char c : key) {
    h ^= c;
    h *= 1099511628211ull;  // FNV prime
  }
  wchar_t hex[17];
  for (int i = 15; i >= 0; --i) {
    hex[i] = L"0123456789abcdef"[h & 0xF];
    h >>= 4;
  }
  hex[16] = L'\0';
  const std::wstring name = L"Local\\GMM-Instance-" + std::wstring(hex);

  // Deliberately never closed: an OS-owned handle held for the entire process
  // lifetime is exactly the point (the mutex -- and with it, the lock -- goes
  // away automatically when this process exits for any reason, including a
  // crash, without needing an explicit release call anywhere).
  const HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
  if (!mutex)
    return true;  // couldn't even try to lock -- fail open, don't block a legit launch
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return false;
  }
  return true;
}

}  // namespace gtamm
