#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Json.h"

namespace gtamm {

// A mod stored exactly once in the central pool (storage/mods/<id>/).
// The mod's actual files live under <id>/root/ (paths relative to the game dir);
// metadata lives in <id>/meta.json.
struct Mod
{
  std::string id;              // unique pool id (= folder name)
  std::string name;            // human-readable display name
  std::string source;          // original import path (for reference)
  std::int64_t importedAt = 0; // unix time (seconds)
  std::string contentHash;     // digest of the mod's files (for dedup)
  // Deploy target. false (default): native routing -- our deployer injects the
  // mod's files into IMG/loose/data itself. true: the mod's tree is staged
  // verbatim under "modloader/<name>/" and left for the Mod Loader runtime
  // (modloader.asi) to load at game time. Set at import; togglable per mod.
  bool viaModloader = false;
  // Freeform user note about the mod (what it does, why it's here, ...),
  // shown in the GUI's Info tab. Plain text (newlines preserved); empty by
  // default. Not used for anything but display -- purely informational.
  std::string description;
};

// A mod's state within a profile. An entry may also be a visual separator
// (group header), which carries no mod and is ignored at deploy time.
struct ProfileEntry
{
  std::string modId;
  bool enabled = false;
  // Conflict priority: HIGHER wins (its file is deployed on top). Mods not
  // listed here are treated as disabled with priority 0.
  int priority = 0;
  bool separator = false;     // true => a group header, not a real mod
  std::string label;          // separator display text
};

// SA-MP (multiplayer) launch settings, stored per profile. When enabled, the
// default Play action launches samp.exe (the injector) instead of gta_sa.exe;
// the manager then waits on the spawned gta_sa.exe so the deployed mods stay in
// place for the whole session. samp.exe injects samp.dll into gta_sa.exe and
// exits almost immediately, so waiting on it directly would roll back too early.
struct SampConfig
{
  bool enabled = false;          // route Play through SA-MP
  std::string exe = "samp.exe";  // injector path (relative to game dir or absolute)
  std::string server;            // host/IP; empty => just inject (main menu)
  int port = 7777;               // server port
  std::string nick;              // player name (written to HKCU\Software\SAMP\PlayerName)
  // Server RCON/join password, passed as samp.exe's second command-line
  // argument (after "host:port"). Empty => omitted (server must be public or
  // the player enters it from SA-MP's own connect dialog).
  std::string password;
};

// A build ("сборка"): an ordered set of mods referencing the shared pool.
struct Profile
{
  std::string name;
  std::vector<ProfileEntry> entries;
  SampConfig samp;
};

// ---- JSON (de)serialization -------------------------------------------------

inline void to_json(Json& j, const Mod& m)
{
  j = Json{{"id", m.id},
           {"name", m.name},
           {"source", m.source},
           {"importedAt", m.importedAt},
           {"contentHash", m.contentHash},
           {"viaModloader", m.viaModloader},
           {"description", m.description}};
}

inline void from_json(const Json& j, Mod& m)
{
  j.at("id").get_to(m.id);
  m.name        = j.value("name", m.id);
  m.source      = j.value("source", std::string{});
  m.importedAt  = j.value("importedAt", static_cast<std::int64_t>(0));
  m.contentHash = j.value("contentHash", std::string{});
  m.viaModloader = j.value("viaModloader", false);
  m.description = j.value("description", std::string{});
}

inline void to_json(Json& j, const ProfileEntry& e)
{
  j = Json{{"modId", e.modId},
           {"enabled", e.enabled},
           {"priority", e.priority},
           {"separator", e.separator},
           {"label", e.label}};
}

inline void from_json(const Json& j, ProfileEntry& e)
{
  e.modId     = j.value("modId", std::string{});
  e.enabled   = j.value("enabled", false);
  e.priority  = j.value("priority", 0);
  e.separator = j.value("separator", false);
  e.label     = j.value("label", std::string{});
}

inline void to_json(Json& j, const SampConfig& s)
{
  j = Json{{"enabled", s.enabled},
           {"exe", s.exe},
           {"server", s.server},
           {"port", s.port},
           {"nick", s.nick},
           {"password", s.password}};
}

inline void from_json(const Json& j, SampConfig& s)
{
  s.enabled = j.value("enabled", false);
  s.exe     = j.value("exe", std::string{"samp.exe"});
  s.server  = j.value("server", std::string{});
  s.port    = j.value("port", 7777);
  s.nick    = j.value("nick", std::string{});
  s.password = j.value("password", std::string{});
}

inline void to_json(Json& j, const Profile& p)
{
  j = Json{{"name", p.name}, {"entries", p.entries}, {"samp", p.samp}};
}

inline void from_json(const Json& j, Profile& p)
{
  j.at("name").get_to(p.name);
  p.entries = j.value("entries", std::vector<ProfileEntry>{});
  p.samp    = j.value("samp", SampConfig{});
}

}  // namespace gtamm
