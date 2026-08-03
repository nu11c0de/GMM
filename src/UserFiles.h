#pragma once
#include <filesystem>

// Helpers for managing GTA SA's "User Files" folder (saves + settings), which
// the game always reads/writes under the user's Documents folder, independent
// of the install dir. Per-profile saves are implemented by redirecting this
// folder to a profile-owned store via a directory junction (no admin needed).
namespace gtamm::userfiles {

// Absolute path to "<Documents>\GTA San Andreas User Files". The Documents path
// is resolved at runtime (handles OneDrive-redirected Documents). For tests, the
// environment variable GMM_USERFILES_DIR overrides this entirely.
std::filesystem::path gtaUserFilesDir();

// True if the path exists and is a reparse point (junction/symlink).
bool isReparsePoint(const std::filesystem::path& p);

// Create a directory junction at `link` pointing to `target` (a mount-point
// reparse point). `link` must not already exist; `target` should exist. Throws
// std::runtime_error on failure.
void createJunction(const std::filesystem::path& link,
                    const std::filesystem::path& target);

// Remove a directory junction (deletes the link only, never the target).
void removeJunction(const std::filesystem::path& link);

}  // namespace gtamm::userfiles
