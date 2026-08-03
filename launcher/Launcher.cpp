// Single-file entry point for GMM. This tiny exe is the ONE file end users
// download: it embeds the whole packaged program (the real Qt GMM.exe + every
// DLL it needs -- Qt, libarchive, the MSVC runtime, platform plugins) as a
// resource, and on first run extracts everything into one "GMM\" folder next
// to itself ("GMM\bin\" for the payload), then launches "GMM\bin\GMM.exe"
// with GMM_PORTABLE_ROOT set to "GMM\", so the real program's own portability
// logic (InstanceStore::baseDir(), the GUI's QSettings path -- see Process.h's
// portableRoot()) keeps instances/settings at "GMM\data\" -- one tidy folder
// next to the launcher instead of "bin\"/"data\" scattered as loose siblings
// of the exe. The instance-management dialog (create/switch/remove) still
// runs completely normally -- only the base folder it works under moves.
//
// NOTE: an earlier revision of this file also tried to relocate the launcher
// EXE ITSELF into GMM\ on first run (copy self -> GMM\GMM.exe, relaunch,
// delete the original) so nothing but the GMM\ folder is left where the user
// put the exe. That had a real bug (the final bin\GMM.exe never stayed
// running) that wasn't tracked down before being shelved -- reverted back to
// this simpler, verified-working shape. See git history if picking that
// back up.
//
// Deliberately links NOTHING that pulls in a DLL at runtime -- not even
// gtamm_core's libarchive dependency (see App::extractArchive), which would
// defeat the entire point (the launcher must run with zero files next to it).
// So extraction is a small hand-rolled, dependency-free ZIP reader below,
// parsing straight out of the in-memory embedded resource. It only needs to
// understand the STORED (uncompressed) method: release.bat packages
// launcher\payload.zip with 7z's -mx=0 (store, no compression) specifically so
// this reader never has to decompress anything -- it just copies bytes. That
// trades a larger payload for zero runtime dependencies.
//
// The payload (launcher\payload.zip) is produced by release.bat from the
// already-built portable folder in a PRIOR build pass, then this target is
// (re)built in a second pass once payload.zip exists -- see the "launcher
// target" section in CMakeLists.txt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Process.h"
#include "Version.h"

namespace fs = std::filesystem;

namespace {

std::uint16_t le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
std::uint32_t le32(const unsigned char* p)
{
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Load the payload zip embedded as an RCDATA resource named "PAYLOAD". The
// returned pointer/size point directly into the module's mapped image; no
// copy needed (LockResource memory is valid for the process lifetime).
struct Resource
{
  const unsigned char* data;
  std::size_t size;
};

Resource loadPayloadResource()
{
  const HMODULE mod = GetModuleHandleW(nullptr);
  // RT_RCDATA expands through the ANSI/wide-ambiguous MAKEINTRESOURCE macro;
  // the project doesn't define UNICODE globally (every WinAPI call here uses
  // the explicit W-suffixed function instead), so spell out the wide literal.
  const HRSRC res = FindResourceW(mod, L"PAYLOAD", MAKEINTRESOURCEW(10));
  if (!res)
    throw std::runtime_error("embedded payload resource not found (built without "
                             "launcher/payload.zip?)");
  const HGLOBAL handle = LoadResource(mod, res);
  const DWORD size = SizeofResource(mod, res);
  const void* ptr = handle ? LockResource(handle) : nullptr;
  if (!ptr || size == 0)
    throw std::runtime_error("embedded payload resource is empty");
  return {static_cast<const unsigned char*>(ptr), size};
}

// Reject entries that try to escape destDir (absolute path or "..").
bool isSafeZipPath(const std::string& rel)
{
  if (rel.empty() || rel.front() == '/' || rel.front() == '\\')
    return false;
  if (rel.size() >= 2 && rel[1] == ':')
    return false;  // "C:\..."
  std::size_t pos = 0;
  while (pos < rel.size()) {
    std::size_t next = rel.find_first_of("/\\", pos);
    if (next == std::string::npos)
      next = rel.size();
    const std::string part = rel.substr(pos, next - pos);
    if (part == "..")
      return false;
    pos = next + 1;
  }
  return true;
}

// Extract a STORED-only (uncompressed) zip held entirely in memory into
// `destDir`. Understands just enough of the format (End Of Central Directory
// -> Central Directory entries -> Local File Header) to read back exactly
// what release.bat's "7z a -mx=0" packing step produces; throws on anything
// else (a compressed entry, a corrupt/truncated archive) rather than silently
// extracting garbage.
void extractStoredZip(const unsigned char* zip, std::size_t zipSize,
                      const fs::path& destDir)
{
  if (zipSize < 22)
    throw std::runtime_error("payload too small to be a zip");

  // Find the End Of Central Directory record (signature 'PK\x05\x06'),
  // scanning back from the end (it may be followed by a short comment).
  const std::size_t maxBack = std::min<std::size_t>(zipSize, 22 + 65535);
  std::size_t eocd = std::string::npos;
  for (std::size_t back = 22; back <= maxBack; ++back) {
    const std::size_t i = zipSize - back;
    if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 0x05 && zip[i + 3] == 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos)
    throw std::runtime_error("not a zip file (no End Of Central Directory record)");

  const std::uint16_t entryCount = le16(zip + eocd + 10);
  const std::uint32_t cdSize     = le32(zip + eocd + 12);
  const std::uint32_t cdOffset   = le32(zip + eocd + 16);
  if (static_cast<std::uint64_t>(cdOffset) + cdSize > eocd)
    throw std::runtime_error("zip central directory out of range");

  fs::create_directories(destDir);

  std::size_t p = cdOffset;
  for (std::uint16_t n = 0; n < entryCount; ++n) {
    if (p + 46 > zipSize || !(zip[p] == 'P' && zip[p + 1] == 'K' && zip[p + 2] == 0x01 &&
                              zip[p + 3] == 0x02))
      throw std::runtime_error("zip central directory entry #" + std::to_string(n) +
                               " has a bad signature");

    const std::uint16_t method       = le16(zip + p + 10);
    const std::uint32_t compSize     = le32(zip + p + 20);
    const std::uint32_t uncompSize   = le32(zip + p + 24);
    const std::uint16_t nameLen      = le16(zip + p + 28);
    const std::uint16_t extraLen     = le16(zip + p + 30);
    const std::uint16_t commentLen   = le16(zip + p + 32);
    const std::uint32_t localOffset  = le32(zip + p + 42);
    const std::size_t nameOff = p + 46;
    if (nameOff + nameLen > zipSize)
      throw std::runtime_error("zip central directory entry #" + std::to_string(n) +
                               " filename out of range");

    const std::string name(reinterpret_cast<const char*>(zip + nameOff), nameLen);
    const std::size_t next = nameOff + nameLen + extraLen + commentLen;

    const bool isDir = !name.empty() && (name.back() == '/' || name.back() == '\\');
    if (isDir) {
      if (isSafeZipPath(name))
        fs::create_directories(destDir / fs::path(name));
      p = next;
      continue;
    }

    if (!isSafeZipPath(name))
      throw std::runtime_error("zip entry has an unsafe path: " + name);
    if (method != 0)
      throw std::runtime_error("zip entry '" + name + "' is compressed (method " +
                               std::to_string(method) +
                               ") -- the launcher only reads STORED payloads; "
                               "repack launcher/payload.zip with 7z's -mx=0");

    // Local File Header: same shape, but its own name/extra lengths (which
    // can legitimately differ from the central directory's) decide where the
    // file data actually starts.
    if (static_cast<std::uint64_t>(localOffset) + 30 > zipSize ||
        !(zip[localOffset] == 'P' && zip[localOffset + 1] == 'K' &&
          zip[localOffset + 2] == 0x03 && zip[localOffset + 3] == 0x04))
      throw std::runtime_error("zip local header for '" + name + "' has a bad signature");
    const std::uint16_t lNameLen  = le16(zip + localOffset + 26);
    const std::uint16_t lExtraLen = le16(zip + localOffset + 28);
    const std::size_t dataOffset = localOffset + 30 + lNameLen + lExtraLen;
    if (dataOffset + compSize > zipSize)
      throw std::runtime_error("zip entry '" + name + "' data out of range");
    if (compSize != uncompSize)
      throw std::runtime_error("zip entry '" + name +
                               "' claims to be stored but sizes disagree");

    const fs::path outPath = destDir / fs::path(name);
    fs::create_directories(outPath.parent_path());
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot create '" + outPath.string() + "'");
    if (uncompSize > 0)
      out.write(reinterpret_cast<const char*>(zip + dataOffset),
               static_cast<std::streamsize>(uncompSize));

    p = next;
  }
}

// Extract the embedded payload into `binDir` if it isn't already there with a
// matching version marker (so normal launches after the first are instant).
void ensureExtracted(const fs::path& binDir)
{
  const fs::path markerPath = binDir / ".gmm-version";
  const std::string myVersion = gtamm::versionString();

  {
    std::ifstream marker(markerPath);
    std::string v;
    if (marker && std::getline(marker, v) && v == myVersion &&
        fs::exists(binDir / "GMM.exe"))
      return;  // already extracted at this version
  }

  const Resource payload = loadPayloadResource();

  std::error_code ec;
  fs::remove_all(binDir, ec);  // drop any stale/partial previous extraction
  extractStoredZip(payload.data, payload.size, binDir);

  std::ofstream marker(markerPath, std::ios::trunc);
  marker << myVersion << "\n";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  try {
    const fs::path here   = gtamm::executableDir();
    // Everything the launcher creates lives under one "GMM\" folder next to
    // itself -- bin\ (self-extracted payload) and, via GMM_PORTABLE_ROOT
    // below, data\ (instances/settings) -- instead of scattering "bin" and
    // "data" as loose siblings of the exe.
    const fs::path root   = here / "GMM";
    const fs::path binDir = root / "bin";
    ensureExtracted(binDir);

    const fs::path exe = binDir / "GMM.exe";
    if (!fs::exists(exe))
      throw std::runtime_error("extraction succeeded but " + exe.string() +
                               " is missing");

    // Tell the real GMM.exe (running from bin\, which gets wiped on every
    // version bump) to keep its portable state -- instances under data\<name>\
    // and its own UI settings -- under GMM\ instead. Inherited by the child
    // since CreateProcessW below passes lpEnvironment=nullptr. Do NOT pass
    // --data: that picks one specific instance directly and skips the
    // instance-management dialog entirely, which is not what we want here --
    // GMM_PORTABLE_ROOT only relocates the *base* folder instances live
    // under, so create/switch/remove still works normally.
    SetEnvironmentVariableW(L"GMM_PORTABLE_ROOT", root.c_str());

    // CreateProcessW may write into the command-line buffer, so pass a mutable
    // copy (matches the pattern in Process.cpp's launchAndWait).
    const std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr,
                                   FALSE, 0, nullptr, binDir.c_str(), &si, &pi);
    if (!ok)
      throw std::runtime_error("failed to launch " + exe.string());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } catch (const std::exception& e) {
    MessageBoxA(nullptr, e.what(), "GMM", MB_ICONERROR | MB_OK);
    return 1;
  }
  return 0;
}
