#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>

namespace gtamm {

// Thrown by extractArchive() when the archive is password-protected and no
// password (or an incorrect one) was supplied. Callers can catch this
// specifically to prompt for a password and retry, instead of treating it as
// a generic extraction failure.
struct ArchivePasswordRequired : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

// Extract an archive (zip/7z/rar/tar/...) into `destDir`, which is created if
// needed. `password`, if non-empty, is tried as the archive's passphrase
// (encrypted zip/7z/rar). Throws ArchivePasswordRequired if the archive turns
// out to be encrypted and `password` is empty or wrong; std::runtime_error for
// any other failure. Entry paths are sanitized so they cannot escape `destDir`.
//
// Named Unpack.* (not Archive.*) on purpose: on case-insensitive Windows a file
// called Archive.h would shadow libarchive's own <archive.h>.
void extractArchive(const std::filesystem::path& archivePath,
                    const std::filesystem::path& destDir,
                    const std::string& password = std::string());

// Create a zip archive at `zipPath` containing every regular file under
// `srcDir`, with entry names relative to `srcDir` (forward slashes). Parent
// directories of `zipPath` are created if needed. Throws std::runtime_error on
// failure.
void createZip(const std::filesystem::path& srcDir,
               const std::filesystem::path& zipPath);

// Recursively unpacks any archive found inside `dir` (zip/7z/rar/tar/...) in
// place -- common for mods distributed as "an archive wrapping the real
// archive" (e.g. a password-protected outer zip whose only content is the
// actual mod's own .zip, or a filehost-bypass wrapper), which would otherwise
// leave a useless nested archive sitting in the imported mod's file tree
// instead of the files it actually needs. Each nested archive is extracted
// into its own containing folder and then deleted; repeats until no more
// archives are found (or a safety cap is hit, to bound pathological nesting).
// `fallbackPassword`, if non-empty, is retried for a nested archive that turns
// out to also be encrypted (a common pattern: the same password protects every
// layer) -- one that still can't be opened is left in place rather than
// aborting the whole import. Only ever called on a disposable temp directory
// GMM itself extracted into, never on a path the user still owns.
void flattenNestedArchives(const std::filesystem::path& dir,
                           const std::string& fallbackPassword = std::string());

// True if `p`'s extension looks like a supported archive format (zip/7z/rar/
// tar/gz/tgz/bz2/xz), purely by name -- doesn't open the file. Used to tell a
// real archive apart from a single loose mod file (a standalone .lua script,
// .asi plugin, ...) passed where an archive path is otherwise expected.
bool hasArchiveExtension(const std::filesystem::path& p);

}  // namespace gtamm
