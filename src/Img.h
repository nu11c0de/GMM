#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gtamm {

// One entry in a VER2 IMG archive directory.
struct ImgEntry
{
  std::string name;      // original case, up to 23 chars
  std::uint32_t offset;  // start, in 2048-byte sectors, from the file start
  std::uint32_t size;    // size, in sectors
};

// A file to inject when rebuilding an IMG. If `name` matches an existing entry
// (case-insensitive) it replaces it, otherwise it is appended as a new entry.
struct ImgInjection
{
  std::string name;            // entry name to write (e.g. "infernus.dff")
  std::filesystem::path file;  // source file on disk
};

inline constexpr std::uint32_t kImgSectorSize = 2048;

// True if the file looks like a VER2 IMG archive.
bool imgIsVer2(const std::filesystem::path& imgPath);

// Read the directory of a VER2 IMG archive.
std::vector<ImgEntry> imgReadDirectory(const std::filesystem::path& imgPath);

// Read the raw (sector-padded) bytes of one entry.
std::vector<unsigned char> imgReadEntry(const std::filesystem::path& imgPath,
                                        const ImgEntry& entry);

// Rebuild `srcImg` into `outImg`, applying `injections` (replace or append).
// The result is a valid VER2 archive with a freshly laid-out, sector-aligned
// data section. `srcImg` and `outImg` may not be the same path.
void imgRebuild(const std::filesystem::path& srcImg,
                const std::filesystem::path& outImg,
                const std::vector<ImgInjection>& injections);

}  // namespace gtamm
