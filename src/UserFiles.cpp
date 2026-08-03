#include "UserFiles.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shlobj.h>    // SHGetKnownFolderPath, FOLDERID_Documents
#include <winioctl.h>  // FSCTL_SET_REPARSE_POINT, IO_REPARSE_TAG_MOUNT_POINT

namespace fs = std::filesystem;

namespace gtamm::userfiles {

namespace {

// Flat layout of a mount-point REPARSE_DATA_BUFFER (ntifs.h isn't in the user
// SDK headers, so we declare the bytes we need ourselves).
struct MountPointReparse
{
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  USHORT SubstituteNameOffset;
  USHORT SubstituteNameLength;
  USHORT PrintNameOffset;
  USHORT PrintNameLength;
  WCHAR PathBuffer[1];
};

struct Handle
{
  HANDLE h = INVALID_HANDLE_VALUE;
  ~Handle()
  {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
};

}  // namespace

fs::path gtaUserFilesDir()
{
  if (const char* envDir = std::getenv("GMM_USERFILES_DIR"); envDir && *envDir)
    return fs::path(envDir);

  PWSTR docs = nullptr;
  const HRESULT hr =
      SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs);
  if (FAILED(hr) || !docs) {
    if (docs)
      CoTaskMemFree(docs);
    throw std::runtime_error("cannot resolve the Documents folder");
  }
  fs::path result(docs);
  CoTaskMemFree(docs);
  result /= L"GTA San Andreas User Files";
  return result;
}

bool isReparsePoint(const fs::path& p)
{
  const DWORD attr = GetFileAttributesW(p.wstring().c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void createJunction(const fs::path& link, const fs::path& target)
{
  const fs::path abst = fs::absolute(target);
  const std::wstring sub   = L"\\??\\" + abst.wstring();  // substitute (NT path)
  const std::wstring print = abst.wstring();              // print (display) name

  if (!CreateDirectoryW(link.wstring().c_str(), nullptr)) {
    if (GetLastError() != ERROR_ALREADY_EXISTS)
      throw std::runtime_error("cannot create junction dir: " + link.string());
  }

  Handle dir;
  dir.h = CreateFileW(link.wstring().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (dir.h == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(link.wstring().c_str());
    throw std::runtime_error("cannot open junction dir: " + link.string());
  }

  const USHORT subBytes   = static_cast<USHORT>(sub.size() * sizeof(wchar_t));
  const USHORT printBytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
  // PathBuffer = substitute + NUL + print + NUL
  const size_t pathBytes = subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
  const size_t headerToPath = offsetof(MountPointReparse, PathBuffer);
  std::vector<char> buf(headerToPath + pathBytes, 0);

  auto* rb            = reinterpret_cast<MountPointReparse*>(buf.data());
  rb->ReparseTag      = IO_REPARSE_TAG_MOUNT_POINT;
  // ReparseDataLength counts everything after the 8-byte common header.
  rb->ReparseDataLength = static_cast<USHORT>((headerToPath - 8) + pathBytes);
  rb->Reserved          = 0;
  rb->SubstituteNameOffset = 0;
  rb->SubstituteNameLength = subBytes;
  rb->PrintNameOffset      = static_cast<USHORT>(subBytes + sizeof(wchar_t));
  rb->PrintNameLength      = printBytes;
  std::memcpy(rb->PathBuffer, sub.c_str(), subBytes + sizeof(wchar_t));
  std::memcpy(reinterpret_cast<char*>(rb->PathBuffer) + rb->PrintNameOffset,
              print.c_str(), printBytes + sizeof(wchar_t));

  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(dir.h, FSCTL_SET_REPARSE_POINT, rb,
                                  static_cast<DWORD>(buf.size()), nullptr, 0,
                                  &returned, nullptr);
  if (!ok) {
    const DWORD err = GetLastError();
    CloseHandle(dir.h);
    dir.h = INVALID_HANDLE_VALUE;
    RemoveDirectoryW(link.wstring().c_str());
    throw std::runtime_error("FSCTL_SET_REPARSE_POINT failed (error " +
                             std::to_string(err) + ") for " + link.string());
  }
}

void removeJunction(const fs::path& link)
{
  // RemoveDirectory on a reparse point deletes the link, not the target.
  if (!RemoveDirectoryW(link.wstring().c_str())) {
    const DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
      throw std::runtime_error("cannot remove junction: " + link.string() +
                               " (error " + std::to_string(err) + ")");
  }
}

}  // namespace gtamm::userfiles
