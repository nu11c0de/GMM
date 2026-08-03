#include "Img.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace gtamm {

namespace {

std::uint32_t sectorsFor(std::uint64_t bytes)
{
  return static_cast<std::uint32_t>((bytes + kImgSectorSize - 1) / kImgSectorSize);
}

std::uint16_t readU16(std::istream& s)
{
  unsigned char b[2];
  s.read(reinterpret_cast<char*>(b), 2);
  return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

std::uint32_t readU32(std::istream& s)
{
  unsigned char b[4];
  s.read(reinterpret_cast<char*>(b), 4);
  return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
         (static_cast<std::uint32_t>(b[2]) << 16) |
         (static_cast<std::uint32_t>(b[3]) << 24);
}

void writeU16(std::ostream& s, std::uint16_t v)
{
  unsigned char b[2] = {static_cast<unsigned char>(v & 0xFF),
                        static_cast<unsigned char>((v >> 8) & 0xFF)};
  s.write(reinterpret_cast<char*>(b), 2);
}

void writeU32(std::ostream& s, std::uint32_t v)
{
  unsigned char b[4] = {static_cast<unsigned char>(v & 0xFF),
                        static_cast<unsigned char>((v >> 8) & 0xFF),
                        static_cast<unsigned char>((v >> 16) & 0xFF),
                        static_cast<unsigned char>((v >> 24) & 0xFF)};
  s.write(reinterpret_cast<char*>(b), 4);
}

std::ifstream openIn(const fs::path& p)
{
  std::ifstream f(p, std::ios::binary);
  if (!f)
    throw std::runtime_error("cannot open IMG: " + p.string());
  return f;
}

std::string toLower(std::string s)
{
  for (char& c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}

void padTo(std::ostream& out, std::uint64_t targetBytes, std::uint64_t& written)
{
  static const std::array<char, kImgSectorSize> zeros{};
  while (written < targetBytes) {
    const std::uint64_t chunk =
        std::min<std::uint64_t>(zeros.size(), targetBytes - written);
    out.write(zeros.data(), static_cast<std::streamsize>(chunk));
    written += chunk;
  }
}

}  // namespace

bool imgIsVer2(const fs::path& imgPath)
{
  std::ifstream f(imgPath, std::ios::binary);
  if (!f)
    return false;
  char magic[4] = {};
  f.read(magic, 4);
  return f.gcount() == 4 && std::memcmp(magic, "VER2", 4) == 0;
}

std::vector<ImgEntry> imgReadDirectory(const fs::path& imgPath)
{
  std::ifstream f = openIn(imgPath);
  char magic[4];
  f.read(magic, 4);
  if (std::memcmp(magic, "VER2", 4) != 0)
    throw std::runtime_error("not a VER2 IMG archive: " + imgPath.string());

  const std::uint32_t count = readU32(f);
  std::vector<ImgEntry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t offset    = readU32(f);
    const std::uint16_t streaming = readU16(f);
    const std::uint16_t inArchive = readU16(f);
    char name[24];
    f.read(name, 24);
    if (!f)
      throw std::runtime_error("truncated IMG directory: " + imgPath.string());
    // Vanilla SA uses the streaming size; a non-zero "size in archive" overrides.
    const std::uint32_t size = inArchive != 0 ? inArchive : streaming;
    std::string n(name, strnlen(name, 24));
    entries.push_back({std::move(n), offset, size});
  }
  return entries;
}

std::vector<unsigned char> imgReadEntry(const fs::path& imgPath, const ImgEntry& entry)
{
  std::ifstream f = openIn(imgPath);
  f.seekg(static_cast<std::streamoff>(entry.offset) * kImgSectorSize);
  std::vector<unsigned char> data(static_cast<std::size_t>(entry.size) * kImgSectorSize);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f)
    throw std::runtime_error("failed reading IMG entry: " + entry.name);
  return data;
}

void imgRebuild(const fs::path& srcImg, const fs::path& outImg,
                const std::vector<ImgInjection>& injections)
{
  if (fs::weakly_canonical(srcImg) == fs::weakly_canonical(outImg))
    throw std::runtime_error("imgRebuild: source and output must differ");

  const std::vector<ImgEntry> srcDir = imgReadDirectory(srcImg);

  // One output item per archive entry: either copied from the source or taken
  // from an injected file.
  struct Item
  {
    std::string name;
    bool fromFile = false;
    fs::path file;             // when fromFile
    std::uint32_t srcOffset;   // when copied (sectors)
    std::uint32_t sizeSectors; // final size (sectors)
    std::uint32_t newOffset;   // assigned below (sectors)
  };

  std::vector<Item> items;
  items.reserve(srcDir.size() + injections.size());

  // Map lowercased name -> index of the injection that targets it.
  std::vector<bool> injUsed(injections.size(), false);
  auto findInjection = [&](const std::string& name) -> int {
    const std::string key = toLower(name);
    for (std::size_t i = 0; i < injections.size(); ++i)
      if (!injUsed[i] && toLower(injections[i].name) == key)
        return static_cast<int>(i);
    return -1;
  };

  for (const ImgEntry& e : srcDir) {
    Item it;
    it.name        = e.name;
    const int inj  = findInjection(e.name);
    if (inj >= 0) {
      injUsed[inj]   = true;
      it.fromFile    = true;
      it.file        = injections[inj].file;
      it.sizeSectors = sectorsFor(fs::file_size(it.file));
    } else {
      it.fromFile    = false;
      it.srcOffset   = e.offset;
      it.sizeSectors = e.size;
    }
    items.push_back(std::move(it));
  }
  // Injections that did not match an existing entry are appended.
  for (std::size_t i = 0; i < injections.size(); ++i) {
    if (injUsed[i])
      continue;
    Item it;
    it.name        = injections[i].name;
    it.fromFile    = true;
    it.file        = injections[i].file;
    it.sizeSectors = sectorsFor(fs::file_size(it.file));
    items.push_back(std::move(it));
  }

  // Lay out: header (8) + 32 bytes per entry, padded to a sector boundary, then
  // each entry's data on sector boundaries.
  const std::uint64_t dirBytes = 8ull + static_cast<std::uint64_t>(items.size()) * 32ull;
  std::uint32_t cursor         = sectorsFor(dirBytes);
  for (Item& it : items) {
    it.newOffset = cursor;
    cursor += it.sizeSectors;
  }

  std::ofstream out(outImg, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot create IMG: " + outImg.string());

  // Directory.
  out.write("VER2", 4);
  writeU32(out, static_cast<std::uint32_t>(items.size()));
  for (const Item& it : items) {
    writeU32(out, it.newOffset);
    writeU16(out, static_cast<std::uint16_t>(it.sizeSectors));
    writeU16(out, 0);
    char name[24] = {};
    std::strncpy(name, it.name.c_str(), 23);
    out.write(name, 24);
  }

  std::uint64_t written = dirBytes;
  // Pad the header/directory up to the first data sector.
  const std::uint32_t firstDataSector =
      items.empty() ? sectorsFor(dirBytes) : items.front().newOffset;
  padTo(out, static_cast<std::uint64_t>(firstDataSector) * kImgSectorSize, written);

  std::ifstream src = openIn(srcImg);
  std::vector<char> buf;
  for (const Item& it : items) {
    const std::uint64_t target =
        static_cast<std::uint64_t>(it.newOffset) * kImgSectorSize;
    padTo(out, target, written);  // align (already aligned, defensive)

    if (it.fromFile) {
      std::ifstream in(it.file, std::ios::binary);
      if (!in)
        throw std::runtime_error("cannot read injection file: " + it.file.string());
      buf.assign(std::istreambuf_iterator<char>(in), {});
      out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
      written += buf.size();
    } else {
      const std::uint64_t bytes =
          static_cast<std::uint64_t>(it.sizeSectors) * kImgSectorSize;
      src.seekg(static_cast<std::streamoff>(it.srcOffset) * kImgSectorSize);
      buf.resize(static_cast<std::size_t>(bytes));
      src.read(buf.data(), static_cast<std::streamsize>(bytes));
      if (!src)
        throw std::runtime_error("failed copying entry from source: " + it.name);
      out.write(buf.data(), static_cast<std::streamsize>(bytes));
      written += bytes;
    }
    // Pad this entry up to its sector boundary.
    padTo(out, static_cast<std::uint64_t>(it.newOffset + it.sizeSectors) * kImgSectorSize,
          written);
  }
}

}  // namespace gtamm
