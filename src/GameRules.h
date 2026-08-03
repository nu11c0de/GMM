#pragma once
#include <string>
#include <vector>

// GTA San Andreas file-type knowledge: which files live inside IMG archives,
// which are junk, which game folders are roots, and how line-based data files
// are keyed/merged.
//
// The classification tables are informed by Mod Organizer 2's mod-data model
// (architecture only) and by Mod Loader by LINK/2012 (MIT licensed), whose
// std.data/std.stream handlers enumerate these file types. No code is copied
// from either; this is an independent, offline-deploy reimplementation.
namespace gtamm::rules {

// Model/texture/collision/animation files that belong inside an IMG archive.
bool isImgContentFile(const std::string& filename);

// Documentation/screenshot files to ignore when detecting a mod's root.
bool isJunkFile(const std::string& filename);

// Top-level folders that mirror the game directory (data, models, cleo, ...).
bool isGameTopDir(const std::string& name);

// What kind of content a mod carries, for display in the mod list. A mod can
// legitimately be several of these at once (a CLEO script that ships its own
// ASI plugin, a Mod Loader pack containing scripts, ...), so classification
// returns a set rather than one answer.
enum class ModType
{
  Cleo,       // .cs / .cs3 / .cs4 / .cleo
  Asi,        // .asi plugin
  Lua,        // MoonLoader .lua / .luac script
  ModLoader,  // deployed verbatim under modloader/
  Data,       // line-based data files (handling.cfg, .ide, gta.dat, ...)
  Other       // everything else: models, textures, audio, fonts, ...
};

// Distinct types present in `relPaths` (game-relative paths inside the mod),
// in the fixed order of the enum above. `viaModloader` is the mod's own deploy
// flag (see Mod::viaModloader). Never empty: a mod whose files match none of
// the specific types -- a pure texture/model pack, say -- classifies as Other.
std::vector<ModType> modTypes(const std::vector<std::string>& relPaths,
                              bool viaModloader);

// Stable ASCII identifier for a type ("cleo", "asi", "lua", "modloader",
// "data", "other"). The user-facing label and colour live in the GUI.
const char* modTypeId(ModType type);

// Line-based data files we line-merge by key instead of overwriting wholesale.
// Restricted to flat, name/symbol-keyed files where a single key identifies a
// record (handling.cfg, weapon.dat, ...). Section-structured files (.ide,
// gta.dat, carcols) are intentionally excluded -- they need a section-aware
// merge and are still handled whole-file.
bool isMergeableData(const std::string& filename);

// True for comment (';' or '#') or all-whitespace lines.
bool isCommentOrBlank(const std::string& line);

// Key a data line by its first token; lines led by a marker (% ! $ ^ @) are
// keyed by marker + the following token (boats/bikes/planes in handling.cfg).
// Returns "" for comment/blank lines.
std::string lineKey(const std::string& line);

// Merge keyed lines: start from `base` (vanilla), then apply each provider in
// order; a line replaces the line with the same key, or is appended. Comments
// and blank lines from providers are ignored; the base's layout is preserved.
std::string mergeKeyedLines(const std::string& base,
                            const std::vector<std::string>& providerTexts);

// Additive union merge for load-list files (gta.dat / default.dat): keep the
// base, then append each provider line not already present (case/whitespace
// insensitive). Used so mods that register new IMG/IDE/IPL files compose.
std::string mergeAdditive(const std::string& base,
                          const std::vector<std::string>& providerTexts);

// Section-aware merge for .ide item-definition files. Records are merged within
// their section (objs/tobj/cars/peds/weap/...) by their first field (model id /
// name): a provider record replaces the matching one or is appended; provider
// sections absent from the base are added. The base layout is preserved.
std::string mergeIde(const std::string& base,
                     const std::vector<std::string>& providerTexts);

// Pick and apply the right merge strategy for `filename` (keyed vs additive).
std::string mergeData(const std::string& filename, const std::string& base,
                      const std::vector<std::string>& providerTexts);

}  // namespace gtamm::rules
