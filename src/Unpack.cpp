#include "Unpack.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

namespace fs = std::filesystem;

namespace gtamm {

namespace {

std::string errOf(archive* a)
{
  const char* e = archive_error_string(a);
  return e ? e : "unknown libarchive error";
}

std::string toLowerAscii(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool looksLikePasswordError(const std::string& msg)
{
  const std::string lower = toLowerAscii(msg);
  return lower.find("passphrase") != std::string::npos ||
         lower.find("password") != std::string::npos;
}

void copyData(archive* in, archive* out)
{
  const void* buff;
  std::size_t size;
  la_int64_t offset;
  for (;;) {
    const int r = archive_read_data_block(in, &buff, &size, &offset);
    if (r == ARCHIVE_EOF)
      return;
    if (r < ARCHIVE_OK)
      throw std::runtime_error("archive read: " + errOf(in));
    if (archive_write_data_block(out, buff, size, offset) < ARCHIVE_OK)
      throw std::runtime_error("archive write: " + errOf(out));
  }
}

}  // namespace

void extractArchive(const fs::path& archivePath, const fs::path& destDir,
                    const std::string& password)
{
  archive* in = archive_read_new();
  archive_read_support_format_all(in);
  archive_read_support_filter_all(in);
  if (!password.empty())
    archive_read_add_passphrase(in, password.c_str());

  archive* out = archive_write_disk_new();
  archive_write_disk_set_options(out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                          ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                          ARCHIVE_EXTRACT_SECURE_SYMLINKS);

  if (archive_read_open_filename(in, archivePath.string().c_str(), 10240) != ARCHIVE_OK) {
    const std::string e = errOf(in);
    archive_read_free(in);
    archive_write_free(out);
    if (looksLikePasswordError(e))
      throw ArchivePasswordRequired(password.empty() ? "archive is password-protected"
                                                       : "incorrect archive password");
    throw std::runtime_error("cannot open archive '" + archivePath.string() + "': " + e);
  }

  fs::create_directories(destDir);

  // Some formats (RAR, encrypted zip with a full central directory) already
  // know at this point that entries are encrypted, even before reading a
  // header -- ask upfront so an empty/wrong password fails fast with a clear
  // signal instead of surfacing as a generic mid-extraction I/O error.
  if (archive_read_has_encrypted_entries(in) == 1) {
    archive_read_free(in);
    archive_write_free(out);
    throw ArchivePasswordRequired(password.empty() ? "archive is password-protected"
                                                     : "incorrect archive password");
  }

  try {
    archive_entry* entry;
    for (;;) {
      const int r = archive_read_next_header(in, &entry);
      if (r == ARCHIVE_EOF)
        break;
      if (r < ARCHIVE_OK) {
        const std::string e = errOf(in);
        if (looksLikePasswordError(e) || archive_read_has_encrypted_entries(in) == 1)
          throw ArchivePasswordRequired(password.empty() ? "archive is password-protected"
                                                           : "incorrect archive password");
        throw std::runtime_error("archive header: " + e);
      }

      // Redirect each entry under destDir (strip any absolute root; ".." is
      // rejected by SECURE_NODOTDOT).
      fs::path rel = fs::path(archive_entry_pathname(entry));
      if (rel.is_absolute())
        rel = rel.relative_path();
      const std::string outPath = (destDir / rel).generic_string();
      archive_entry_set_pathname(entry, outPath.c_str());

      if (archive_write_header(out, entry) < ARCHIVE_OK)
        throw std::runtime_error("archive extract: " + errOf(out));
      if (archive_entry_size(entry) > 0) {
        try {
          copyData(in, out);
        } catch (const std::runtime_error& e) {
          if (looksLikePasswordError(e.what()))
            throw ArchivePasswordRequired(password.empty() ? "archive is password-protected"
                                                             : "incorrect archive password");
          throw;
        }
      }
      archive_write_finish_entry(out);
    }
  } catch (...) {
    archive_read_free(in);
    archive_write_free(out);
    throw;
  }

  archive_read_free(in);
  archive_write_free(out);
}

void createZip(const fs::path& srcDir, const fs::path& zipPath)
{
  std::error_code ec;
  if (!fs::is_directory(srcDir, ec))
    throw std::runtime_error("not a directory: " + srcDir.string());
  if (zipPath.has_parent_path())
    fs::create_directories(zipPath.parent_path(), ec);

  archive* a = archive_write_new();
  archive_write_set_format_zip(a);
  archive_write_set_options(a, "zip:compression=deflate");
  if (archive_write_open_filename(a, zipPath.string().c_str()) != ARCHIVE_OK) {
    const std::string e = errOf(a);
    archive_write_free(a);
    throw std::runtime_error("cannot create zip '" + zipPath.string() + "': " + e);
  }

  try {
    std::vector<char> buf(65536);
    for (const auto& de : fs::recursive_directory_iterator(srcDir, ec)) {
      if (!de.is_regular_file(ec))
        continue;
      const fs::path rel = fs::relative(de.path(), srcDir, ec);
      if (ec || rel.empty())
        continue;
      const std::uintmax_t size = fs::file_size(de.path(), ec);
      if (ec)
        throw std::runtime_error("cannot stat: " + de.path().string());

      archive_entry* entry = archive_entry_new();
      archive_entry_set_pathname(entry, rel.generic_string().c_str());
      archive_entry_set_size(entry, static_cast<la_int64_t>(size));
      archive_entry_set_filetype(entry, AE_IFREG);
      archive_entry_set_perm(entry, 0644);
      const int hr = archive_write_header(a, entry);
      if (hr < ARCHIVE_OK) {
        const std::string e = errOf(a);
        archive_entry_free(entry);
        throw std::runtime_error("zip header: " + e);
      }

      std::ifstream in(de.path(), std::ios::binary);
      if (!in) {
        archive_entry_free(entry);
        throw std::runtime_error("cannot read: " + de.path().string());
      }
      while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) ||
             in.gcount()) {
        const std::streamsize n = in.gcount();
        if (n > 0 &&
            archive_write_data(a, buf.data(), static_cast<std::size_t>(n)) < 0)
          throw std::runtime_error("zip write: " + errOf(a));
      }
      archive_entry_free(entry);
    }
  } catch (...) {
    archive_write_close(a);
    archive_write_free(a);
    throw;
  }

  archive_write_close(a);
  archive_write_free(a);
}

bool hasArchiveExtension(const fs::path& p)
{
  static const std::vector<std::string> kExts = {
      ".zip", ".7z", ".rar", ".tar", ".gz", ".tgz", ".bz2", ".xz"};
  const std::string ext = toLowerAscii(p.extension().string());
  for (const auto& e : kExts)
    if (ext == e)
      return true;
  return false;
}

void flattenNestedArchives(const fs::path& dir, const std::string& fallbackPassword)
{
  // Bounds pathological/self-referential nesting; real mod archives are never
  // wrapped more than a couple of levels deep.
  for (int pass = 0; pass < 6; ++pass) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& de : fs::recursive_directory_iterator(dir, ec)) {
      if (ec)
        break;
      if (de.is_regular_file(ec) && hasArchiveExtension(de.path()))
        found.push_back(de.path());
    }
    if (found.empty())
      return;

    bool extractedAny = false;
    for (const fs::path& archivePath : found) {
      const fs::path destDir = archivePath.parent_path();
      bool ok = false;
      try {
        extractArchive(archivePath, destDir);
        ok = true;
      } catch (const ArchivePasswordRequired&) {
        if (fallbackPassword.empty())
          continue;  // no password to retry with -- leave it, not an error
        try {
          extractArchive(archivePath, destDir, fallbackPassword);
          ok = true;
        } catch (const ArchivePasswordRequired&) {
          continue;  // this nested archive needs a DIFFERENT password -- leave it
        }
        // Any other exception from the retry (corrupt data, unsupported
        // format/filter, ...) is a real problem, not "wrong password" -- let
        // it propagate out below, same as the first attempt.
      }
      // Deliberately not catching std::exception here: a nested archive
      // libarchive genuinely can't read (missing filter/format support,
      // corrupt data, an unrecognized RAR variant, ...) used to fail
      // completely silently -- the mod would "import successfully" with a
      // dead .7z/.rar left in its file tree and zero indication why. Let it
      // propagate instead of swallowing it -- extractArchive()'s own message
      // already names the offending path.
      if (ok) {
        fs::remove(archivePath, ec);
        extractedAny = true;
      }
    }
    if (!extractedAny)
      return;  // nothing more we can do with what's left
  }
}

}  // namespace gtamm
