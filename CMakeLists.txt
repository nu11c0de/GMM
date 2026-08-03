cmake_minimum_required(VERSION 3.21)
project(gtamm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every std::string in this codebase is assumed to be UTF-8 (JSON files, the Qt
# GUI, /utf-8 below). But std::filesystem::path::string()/generic_string() (and
# the fs::path(std::string) constructor) convert through the process's ANSI
# code page, NOT UTF-8, unless told otherwise -- so a mod/game folder
# containing a filename the system code page can't represent (e.g. accented
# Latin on a Russian-locale machine, whose code page is Cyrillic-only) throws
# "No mapping for the Unicode character exists in the target multi-byte code
# page" wherever the tree is scanned (import, baseline diff, hashing, deploy).
# resources/utf8.manifest sets <activeCodePage>UTF-8</activeCodePage> (Windows
# 10 1903+; harmlessly ignored on older Windows), which makes the process's
# ANSI code page ITSELF UTF-8, so those conversions always succeed and
# actually produce UTF-8 instead of legacy-code-page bytes.
#
# Debug-build gotcha: CMake's Ninja+MSVC generator only routes an EXE link
# through its special two-phase "cmake -E vs_link_exe --manifests" wrapper when
# /INCREMENTAL is active (Debug's default) -- that wrapper generates its OWN
# default manifest resource (intermediate.manifest -> manifest.res) as a
# separate embed step, which then collides with our own /MANIFEST:EMBED
# writing a manifest resource directly in the same link: CVTRES1100 "duplicate
# resource. Type: MANIFEST, name: 1". /INCREMENTAL:NO (Release's default,
# where this already worked) skips that wrapper entirely, so forcing it off
# here makes Debug take the same single-pass path Release already proven to
# work on -- a slightly slower Debug link is a fair trade for a build that
# actually links.
function(gmm_utf8_manifest target)
  if (MSVC)
    target_link_options(${target} PRIVATE
      "/INCREMENTAL:NO"
      "/MANIFEST:EMBED"
      "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/utf8.manifest")
  endif()
endfunction()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(LibArchive REQUIRED)

# --- version: semver MAJOR.MINOR.PATCH, PATCH auto-incremented per build ---
# gtamm/BUILD_NUMBER holds the counter; build.bat / release.bat bump it before
# configuring, so the displayed version (<major>.<minor>.<build>) grows on
# every build. MAJOR.MINOR tracks the project version declared in vcpkg.json
# (bump both by hand together when that changes); no artificial zero-padding
# of the patch component -- "0.1.71", not "0.1.0071".
set(GMM_VERSION_MAJOR 0)
set(GMM_VERSION_MINOR 1)
set(GMM_BN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_NUMBER")
if(EXISTS "${GMM_BN_FILE}")
  file(READ "${GMM_BN_FILE}" GMM_BUILD_NUMBER)
  string(STRIP "${GMM_BUILD_NUMBER}" GMM_BUILD_NUMBER)
endif()
if(NOT GMM_BUILD_NUMBER MATCHES "^[0-9]+$")
  set(GMM_BUILD_NUMBER 0)
endif()
set(GMM_VERSION_STRING "${GMM_VERSION_MAJOR}.${GMM_VERSION_MINOR}.${GMM_BUILD_NUMBER}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildVersion.h.in"
               "${CMAKE_BINARY_DIR}/generated/BuildVersion.h" @ONLY)
message(STATUS "GMM version: ${GMM_VERSION_STRING} (build ${GMM_BUILD_NUMBER})")

# --- core: shared by the CLI and the GUI ---
add_library(gtamm_core STATIC
  src/App.cpp
  src/Baseline.cpp
  src/Process.cpp
  src/Steam.cpp
  src/SannyBuilder.cpp
  src/Img.cpp
  src/Sfx.cpp
  src/Unpack.cpp
  src/GameRules.cpp
  src/UserFiles.cpp
  src/Version.cpp
)
target_include_directories(gtamm_core PUBLIC src)
# The generated version header is needed only by Version.cpp.
target_include_directories(gtamm_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(gtamm_core PUBLIC nlohmann_json::nlohmann_json LibArchive::LibArchive)
if (WIN32)
  # SHGetKnownFolderPath (shell32) + CoTaskMemFree (ole32) for User Files paths;
  # advapi32 for the SA-MP nickname registry value (HKCU\Software\SAMP).
  target_link_libraries(gtamm_core PUBLIC shell32 ole32 advapi32)
endif()
if (MSVC)
  target_compile_options(gtamm_core PRIVATE /W4 /permissive- /utf-8)
endif()

# --- CLI ---
add_executable(gtamm src/main.cpp resources/gtamm.rc)
target_link_libraries(gtamm PRIVATE gtamm_core)
if (MSVC)
  target_compile_options(gtamm PRIVATE /W4 /permissive- /utf-8)
endif()
gmm_utf8_manifest(gtamm)

# --- GUI (optional: only if Qt6 is available) ---
find_package(Qt6 QUIET COMPONENTS Widgets Network)
if (Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
  add_executable(gtamm-gui WIN32
    src/gui/main_gui.cpp
    src/gui/MainWindow.cpp
    src/gui/Instances.cpp
    src/gui/Welcome.cpp
    src/gui/Frame.cpp
    src/gui/Dialogs.cpp
    src/gui/ModTags.cpp
    src/gui/ServerBrowser.cpp
    src/gui/Theme.cpp
    src/gui/Lang.cpp
    resources/app.qrc
    resources/gtamm.rc
  )
  set_target_properties(gtamm-gui PROPERTIES OUTPUT_NAME "GMM")
  target_link_libraries(gtamm-gui PRIVATE gtamm_core Qt6::Widgets Qt6::Network)
  target_include_directories(gtamm-gui PRIVATE src/gui)

  # Embed the Mod Loader runtime directly into GMM.exe so it needs no external
  # files or path setup: everything under gtamm/runtime/ (RECURSIVELY, preserving
  # the tree, e.g. modloader/.data/... and gta_sa.exe) is compiled into the exe as
  # ":/runtime/<relpath>". At launch materializeRuntime() unpacks it next to the
  # program. Empty folder -> nothing embedded; the manager then falls back to a
  # runtime folder on disk. README.txt is not embedded.
  set(GMM_RT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
  file(GLOB_RECURSE GMM_RT_FILES "${GMM_RT_SRC}/*")
  list(FILTER GMM_RT_FILES EXCLUDE REGEX "/README\\.txt$")
  if(GMM_RT_FILES)
    qt6_add_resources(gtamm-gui "gmm_runtime"
      PREFIX "/runtime"
      BASE "${GMM_RT_SRC}"
      FILES ${GMM_RT_FILES})
    list(LENGTH GMM_RT_FILES GMM_RT_COUNT)
    message(STATUS "GMM: embedding Mod Loader runtime (${GMM_RT_COUNT} files) from runtime/")
  else()
    message(STATUS "GMM: runtime/ has no binaries -- nothing embedded")
  endif()

  # Same idea, but for a full official SA-MP client install (samp.exe, mouse.png,
  # sampgui.png, the SAMP/ custom-object archives, ...) so App::deploySampRuntime()
  # can install it into the game whenever a profile's SampConfig is enabled --
  # independent of the ordinary mod-import pipeline, which the general junk/
  # wrapper-folder heuristics used for downloaded mod packages are a poor fit
  # for (see the App.h doc comment on sampRuntimeDir()). Empty folder -> nothing
  # embedded; the manager falls back to a per-instance runtime folder on disk.
  set(GMM_SAMP_RT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/runtime-samp")
  file(GLOB_RECURSE GMM_SAMP_RT_FILES "${GMM_SAMP_RT_SRC}/*")
  list(FILTER GMM_SAMP_RT_FILES EXCLUDE REGEX "/README\\.txt$")
  if(GMM_SAMP_RT_FILES)
    qt6_add_resources(gtamm-gui "gmm_runtime_samp"
      PREFIX "/runtime-samp"
      BASE "${GMM_SAMP_RT_SRC}"
      FILES ${GMM_SAMP_RT_FILES})
    list(LENGTH GMM_SAMP_RT_FILES GMM_SAMP_RT_COUNT)
    message(STATUS "GMM: embedding SA-MP client (${GMM_SAMP_RT_COUNT} files) from runtime-samp/")
  else()
    message(STATUS "GMM: runtime-samp/ has no binaries -- nothing embedded")
  endif()
  # And once more for Valve's steam_api64.dll, which App loads at runtime to
  # show a session in Steam as "Playing Grand Theft Auto: San Andreas" (see
  # Steam.h). Not redistributed in this repository -- the user drops it into
  # runtime-steam/ once; empty folder -> the feature just reports the DLL as
  # missing and the game launches without a Steam status.
  set(GMM_STEAM_RT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/runtime-steam")
  file(GLOB_RECURSE GMM_STEAM_RT_FILES "${GMM_STEAM_RT_SRC}/*")
  list(FILTER GMM_STEAM_RT_FILES EXCLUDE REGEX "/README\\.txt$")
  if(GMM_STEAM_RT_FILES)
    qt6_add_resources(gtamm-gui "gmm_runtime_steam"
      PREFIX "/runtime-steam"
      BASE "${GMM_STEAM_RT_SRC}"
      FILES ${GMM_STEAM_RT_FILES})
    message(STATUS "GMM: embedding Steamworks redistributable from runtime-steam/")
  else()
    message(STATUS "GMM: runtime-steam/ is empty -- Steam status unavailable")
  endif()
  if (MSVC)
    target_compile_options(gtamm-gui PRIVATE /permissive- /utf-8)
  endif()
  gmm_utf8_manifest(gtamm-gui)

  # Copy the needed Qt runtime DLLs next to the exe so it runs from the build dir.
  find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")
  if (WINDEPLOYQT_EXE)
    add_custom_command(TARGET gtamm-gui POST_BUILD
      COMMAND "${WINDEPLOYQT_EXE}" --no-translations --no-compiler-runtime
              "$<TARGET_FILE:gtamm-gui>"
      COMMENT "Deploying Qt runtime next to gtamm-gui")
  endif()
  # --- single-file launcher (second build pass) --------------------------
  # gtamm-launcher is the ONE exe end users download: it embeds the whole
  # already-packaged portable folder (GMM.exe + every Qt/libarchive/MSVC DLL
  # it needs) as an UNCOMPRESSED (store-only) zip resource and self-extracts
  # next to itself on first run (see launcher/Launcher.cpp). Store-only is
  # deliberate: the launcher's own extractor is a tiny hand-rolled reader with
  # NO DLL dependencies of its own (not even libarchive) -- it only needs to
  # copy bytes, never decompress -- so the single exe truly runs standalone.
  # Its payload (launcher/payload.zip) is produced by release.bat (7z -mx=0)
  # from THIS pass's own gtamm-gui + windeployqt output -- so the target can
  # only be defined once payload.zip already exists, which is why release.bat
  # reconfigures and rebuilds a second time after packaging. Until then it's
  # simply absent.
  set(GMM_LAUNCHER_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/launcher/payload.zip")
  if (EXISTS "${GMM_LAUNCHER_PAYLOAD}")
    add_executable(gtamm-launcher WIN32
      launcher/Launcher.cpp
      launcher/launcher.rc
    )
    # Deliberately NOT named "GMM" here: the real Qt binary in this same build
    # dir is already build-release\GMM.exe, and CMake/Ninja can't have two
    # targets write the same output path. release.bat copies+renames this to
    # the final release\GMM.exe (the single file users get) once both exist.
    # Ninja only tracks launcher.rc's own mtime, not the payload.zip it embeds
    # via RCDATA -- without this, a changed payload silently keeps the old one
    # baked in on an incremental build (the same gotcha app.ico has).
    set_source_files_properties(launcher/launcher.rc PROPERTIES
      OBJECT_DEPENDS "${GMM_LAUNCHER_PAYLOAD}")
    # gtamm_core only for executableDir()/versionString() (Process.cpp,
    # Version.cpp) -- NOT for its Unpack.cpp/libarchive dependency, which
    # Launcher.cpp never calls, so the linker never pulls that .obj in and the
    # launcher ends up with no archive.dll runtime dependency. Verified via a
    # clean-folder run after adding the custom extractor; if this ever
    # regresses (e.g. someone reaches for gtamm::extractArchive() here again),
    # the launcher will fail to start standalone with STATUS_DLL_NOT_FOUND.
    target_link_libraries(gtamm-launcher PRIVATE gtamm_core)
    if (MSVC)
      target_compile_options(gtamm-launcher PRIVATE /permissive- /utf-8)
    endif()
    gmm_utf8_manifest(gtamm-launcher)
    file(SIZE "${GMM_LAUNCHER_PAYLOAD}" GMM_LAUNCHER_PAYLOAD_SIZE)
    message(STATUS "GMM: launcher target enabled (payload.zip: ${GMM_LAUNCHER_PAYLOAD_SIZE} bytes)")
  else()
    message(STATUS "GMM: launcher/payload.zip not built yet -- release.bat packages "
                    "GMM.exe+deps then reconfigures to build the single-file launcher")
  endif()
else()
  message(STATUS "Qt6 not found - building CLI only (set CMAKE_PREFIX_PATH to a Qt6 kit for the GUI)")
endif()

# --- tests ---
find_package(Catch2 3 CONFIG REQUIRED)
add_executable(gtamm_tests tests/tests.cpp)
target_link_libraries(gtamm_tests PRIVATE gtamm_core Catch2::Catch2WithMain)
if (MSVC)
  target_compile_options(gtamm_tests PRIVATE /permissive- /utf-8)
endif()
gmm_utf8_manifest(gtamm_tests)
enable_testing()
add_test(NAME gtamm_tests COMMAND gtamm_tests)
