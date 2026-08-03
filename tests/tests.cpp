#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "App.h"
#include "Baseline.h"
#include "GameRules.h"
#include "Img.h"
#include "SannyBuilder.h"
#include "Sfx.h"
#include "UserFiles.h"

namespace fs = std::filesystem;
using namespace gtamm;

// ---------------------------------------------------------------------------
// GameRules: classification
// ---------------------------------------------------------------------------

TEST_CASE("file type classification", "[rules]")
{
  CHECK(rules::isImgContentFile("infernus.dff"));
  CHECK(rules::isImgContentFile("HUD.TXD"));
  CHECK(rules::isImgContentFile("wheels.col"));
  CHECK(rules::isImgContentFile("ped.ifp"));
  CHECK_FALSE(rules::isImgContentFile("handling.cfg"));
  CHECK_FALSE(rules::isImgContentFile("script.cs"));

  CHECK(rules::isJunkFile("readme.txt"));
  CHECK(rules::isJunkFile("Screenshot.PNG"));
  CHECK_FALSE(rules::isJunkFile("infernus.dff"));

  CHECK(rules::isGameTopDir("data"));
  CHECK(rules::isGameTopDir("Models"));
  CHECK(rules::isGameTopDir("CLEO"));
  CHECK(rules::isGameTopDir("plugins"));  // SA-MP client plugins (<gamedir>/plugins/*.dll)
  CHECK_FALSE(rules::isGameTopDir("MyCoolCar"));

  CHECK(rules::isMergeableData("handling.cfg"));
  CHECK(rules::isMergeableData("WEAPON.DAT"));
  CHECK(rules::isMergeableData("gta.dat"));   // additive load list
  CHECK(rules::isMergeableData("peds.ide"));  // section-aware merge
  CHECK_FALSE(rules::isMergeableData("infernus.dff"));
}

// ---------------------------------------------------------------------------
// GameRules: line keying
// ---------------------------------------------------------------------------

TEST_CASE("data line keying", "[rules]")
{
  CHECK(rules::lineKey("INFERNUS 1400.0 2725.3") == "infernus");
  CHECK(rules::lineKey("   CHEETAH  1.0") == "cheetah");
  // marker lines (boats/bikes/planes) key on marker + name
  CHECK(rules::lineKey("% PREDATOR 1500.0") == "%predator");
  CHECK(rules::lineKey("! BIKE 0") == "!bike");
  CHECK(rules::lineKey("") == "");

  CHECK(rules::isCommentOrBlank("; a comment"));
  CHECK(rules::isCommentOrBlank("   "));
  CHECK(rules::isCommentOrBlank("# hash"));
  CHECK_FALSE(rules::isCommentOrBlank("INFERNUS 1.0"));
}

// ---------------------------------------------------------------------------
// GameRules: keyed merge (the core data-merge behaviour)
// ---------------------------------------------------------------------------

TEST_CASE("keyed line merge composes partial mods", "[rules][merge]")
{
  const std::string base = "; header\r\nINFERNUS van\r\nCHEETAH van\r\nBANSHEE van\r\n";
  const std::vector<std::string> mods = {
      "INFERNUS modA\r\n",                 // replaces INFERNUS
      "CHEETAH modB\r\nZOMG newcar\r\n"};  // replaces CHEETAH, appends ZOMG

  const std::string out = rules::mergeKeyedLines(base, mods);

  CHECK(out.find("INFERNUS modA") != std::string::npos);
  CHECK(out.find("CHEETAH modB") != std::string::npos);
  CHECK(out.find("BANSHEE van") != std::string::npos);     // untouched vanilla kept
  CHECK(out.find("ZOMG newcar") != std::string::npos);     // new key appended
  CHECK(out.find("INFERNUS van") == std::string::npos);    // old line replaced
  CHECK(out.find("; header") != std::string::npos);        // comments preserved
}

TEST_CASE("later provider wins on the same key", "[rules][merge]")
{
  const std::string out =
      rules::mergeKeyedLines("X old\r\n", {"X low\r\n", "X high\r\n"});
  CHECK(out.find("X high") != std::string::npos);
  CHECK(out.find("X low") == std::string::npos);
}

TEST_CASE("additive merge unions load lists and dedups", "[rules][merge]")
{
  const std::string base = "IMG models\\gta3.img\r\nIDE data\\default.ide\r\n";
  const std::vector<std::string> mods = {
      "IMG models\\newcar.img\r\nIMG MODELS\\GTA3.IMG\r\n"};  // dup of gta3 (case)

  const std::string out = rules::mergeData("gta.dat", base, mods);

  CHECK(out.find("models\\newcar.img") != std::string::npos);   // new line added
  CHECK(out.find("data\\default.ide") != std::string::npos);    // base kept
  // gta3.img appears once (the original); the case-different dup is not appended
  std::size_t first = out.find("gta3.img");
  std::size_t last  = out.rfind("gta3.img");
  CHECK(first != std::string::npos);
  CHECK(first == last);
}

TEST_CASE("mergeData dispatches keyed files to keyed merge", "[rules][merge]")
{
  const std::string out =
      rules::mergeData("handling.cfg", "INFERNUS van\r\n", {"INFERNUS mod\r\n"});
  CHECK(out.find("INFERNUS mod") != std::string::npos);
  CHECK(out.find("INFERNUS van") == std::string::npos);  // replaced, not appended
}

TEST_CASE("ide section merge replaces by id and appends within section",
          "[rules][merge][ide]")
{
  const std::string base =
      "objs\r\n"
      "1700, bistro, bistro, 100, 0\r\n"
      "1701, diner, diner, 80, 0\r\n"
      "end\r\n"
      "cars\r\n"
      "400, landstal, landstal, car, LANDSTAL, LANDSTK, null, normal, 10, 0, 0\r\n"
      "end\r\n";
  const std::vector<std::string> mods = {
      // edit obj 1700, add obj 1900, edit car 400
      "objs\r\n1700, bistroMOD, bistro, 300, 0\r\n1900, newobj, newtxd, 50, 0\r\nend\r\n"
      "cars\r\n400, landMOD, landstal, car, LANDSTAL, LANDSTK, null, normal, 10, 0, 0\r\nend\r\n"};

  const std::string out = rules::mergeData("default.ide", base, mods);

  CHECK(out.find("bistroMOD") != std::string::npos);  // 1700 replaced
  CHECK(out.find("1701, diner") != std::string::npos);  // untouched record kept
  CHECK(out.find("1900, newobj") != std::string::npos);  // new record appended
  CHECK(out.find("landMOD") != std::string::npos);       // car 400 replaced
  CHECK(out.find("bistro, bistro, 100") == std::string::npos);  // old 1700 gone
  // both sections still terminated
  CHECK(out.find("objs\r\n") != std::string::npos);
  CHECK(out.find("end\r\n") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Img: VER2 round-trip
// ---------------------------------------------------------------------------

namespace {

void writeBytes(const fs::path& p, const std::vector<unsigned char>& data)
{
  fs::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
}

std::vector<unsigned char> pattern(std::size_t n, unsigned seed)
{
  std::vector<unsigned char> v(n);
  for (std::size_t i = 0; i < n; ++i)
    v[i] = static_cast<unsigned char>((i * 7 + seed) % 251);
  return v;
}

// Build a minimal VER2 IMG from name -> bytes.
void writeImg(const fs::path& path,
              const std::vector<std::pair<std::string, std::vector<unsigned char>>>& entries)
{
  fs::create_directories(path.parent_path());
  constexpr std::uint32_t S = 2048;
  auto sectors = [&](std::size_t bytes) {
    return static_cast<std::uint32_t>((bytes + S - 1) / S);
  };
  const std::uint64_t dirBytes = 8 + entries.size() * 32;
  std::uint32_t cursor         = sectors(dirBytes);

  std::ofstream o(path, std::ios::binary | std::ios::trunc);
  auto u32 = [&](std::uint32_t v) {
    unsigned char b[4] = {(unsigned char)(v), (unsigned char)(v >> 8),
                          (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    o.write(reinterpret_cast<char*>(b), 4);
  };
  auto u16 = [&](std::uint16_t v) {
    unsigned char b[2] = {(unsigned char)(v), (unsigned char)(v >> 8)};
    o.write(reinterpret_cast<char*>(b), 2);
  };

  o.write("VER2", 4);
  u32(static_cast<std::uint32_t>(entries.size()));
  std::vector<std::uint32_t> offs;
  for (const auto& [name, bytes] : entries) {
    const std::uint32_t sz = sectors(bytes.size());
    offs.push_back(cursor);
    u32(cursor);
    u16(static_cast<std::uint16_t>(sz));
    u16(0);
    char nm[24] = {};
    std::snprintf(nm, sizeof(nm), "%s", name.c_str());
    o.write(nm, 24);
    cursor += sz;
  }
  // pad to first data sector
  std::uint64_t pos = dirBytes;
  auto padTo        = [&](std::uint64_t target) {
    static const char z[2048] = {};
    while (pos < target) {
      const std::uint64_t c = std::min<std::uint64_t>(2048, target - pos);
      o.write(z, static_cast<std::streamsize>(c));
      pos += c;
    }
  };
  padTo(static_cast<std::uint64_t>(offs.empty() ? sectors(dirBytes) : offs.front()) * S);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    padTo(static_cast<std::uint64_t>(offs[i]) * S);
    o.write(reinterpret_cast<const char*>(entries[i].second.data()),
            static_cast<std::streamsize>(entries[i].second.size()));
    pos += entries[i].second.size();
    padTo(static_cast<std::uint64_t>(offs[i] + sectors(entries[i].second.size())) * S);
  }
}

}  // namespace

TEST_CASE("IMG read and rebuild preserve untouched entries", "[img]")
{
  const fs::path dir = fs::temp_directory_path() / "gtamm_test_img";
  fs::create_directories(dir);
  const fs::path src = dir / "src.img";
  const fs::path out = dir / "out.img";

  const auto aBytes = pattern(1000, 1);
  const auto bBytes = pattern(3000, 2);
  writeImg(src, {{"a.dff", aBytes}, {"b.txd", bBytes}});

  // Read directory
  auto dirv = imgReadDirectory(src);
  REQUIRE(dirv.size() == 2);
  CHECK(dirv[0].name == "a.dff");
  CHECK(dirv[1].size == 2);  // 3000 bytes -> 2 sectors

  // Inject a larger a.dff; b.txd must be byte-identical afterwards.
  const auto inj = pattern(5000, 9);
  const fs::path injFile = dir / "inj.bin";
  writeBytes(injFile, inj);
  imgRebuild(src, out, {{"a.dff", injFile}});

  auto outv = imgReadDirectory(out);
  REQUIRE(outv.size() == 2);  // replaced, not added

  // b.txd content preserved
  auto findEntry = [&](const std::vector<ImgEntry>& v, const std::string& n) {
    for (const auto& e : v)
      if (e.name == n)
        return e;
    return ImgEntry{};
  };
  const auto bOut    = imgReadEntry(out, findEntry(outv, "b.txd"));
  for (std::size_t i = 0; i < bBytes.size(); ++i)
    REQUIRE(bOut[i] == bBytes[i]);

  // a.dff content is the injected data (first inj.size() bytes)
  const auto aOut = imgReadEntry(out, findEntry(outv, "a.dff"));
  for (std::size_t i = 0; i < inj.size(); ++i)
    REQUIRE(aOut[i] == inj[i]);

  fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// App: full deploy/rollback round-trip on a synthetic game directory
// ---------------------------------------------------------------------------

namespace {

std::vector<unsigned char> readAll(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

void writeText(const fs::path& p, const std::string& s)
{
  fs::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o << s;
}

std::string readText(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("App deploy then rollback restores the game byte-for-byte", "[app]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_app_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  // Synthetic game: a vanilla handling.cfg and a player.img holding bandana.dff.
  writeText(game / "data" / "handling.cfg", "INFERNUS van\r\nBANSHEE van\r\n");
  fs::create_directories(game / "models");
  writeImg(game / "models" / "player.img", {{"bandana.dff", pattern(2000, 5)}});

  // A mod: edits INFERNUS (partial) and replaces the bandana.dff model.
  writeText(mod / "data" / "handling.cfg", "INFERNUS modded\r\n");
  writeBytes(mod / "bandana.dff", pattern(6000, 9));

  const auto vanillaHandling = readAll(game / "data" / "handling.cfg");
  const auto vanillaImg       = readAll(game / "models" / "player.img");

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "Test Mod").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  app.deploy();
  {
    const auto bytes = readAll(game / "data" / "handling.cfg");
    const std::string merged(bytes.begin(), bytes.end());
    CHECK(merged.find("INFERNUS modded") != std::string::npos);  // edit applied
    CHECK(merged.find("BANSHEE van") != std::string::npos);      // vanilla kept
    const auto img = imgReadDirectory(game / "models" / "player.img");
    bool found     = false;
    for (const auto& e : img)
      if (e.name == "bandana.dff" && e.size == 3)  // 6000 bytes -> 3 sectors
        found = true;
    CHECK(found);
  }

  app.rollback();
  CHECK(readAll(game / "data" / "handling.cfg") == vanillaHandling);
  CHECK(readAll(game / "models" / "player.img") == vanillaImg);
  CHECK_FALSE(app.isDeployed());

  fs::remove_all(root);
}

TEST_CASE("map mod: new IDE/IPL register in gta.dat, models go into the IMG",
          "[app][maps]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_maps_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  // Synthetic vanilla game: gta3.img with a world stream IPL + a loose fonts.txd
  // + a data/gta.dat.
  fs::create_directories(game / "models");
  writeImg(game / "models" / "gta3.img",
           {{"countryw_stream4.ipl", pattern(1500, 3)},
            {"vegas_stream0.ipl", pattern(800, 4)}});
  writeText(game / "models" / "fonts.txd", "VANILLA-FONTS");
  writeText(game / "data" / "gta.dat", "IMG models/gta3.img\r\n");

  // A messy map mod: everything dumped at the mod root (as a bad installer leaves
  // it). The deployer must route each file to where the engine actually reads it.
  writeBytes(mod / "countryw_stream4.ipl", pattern(1500, 7));  // replaces IMG entry
  writeBytes(mod / "churchlite.dff", pattern(4096, 8));        // streamed model -> IMG
  writeBytes(mod / "churchlite.txd", pattern(2048, 6));        // streamed txd   -> IMG
  // The IDE references churchlite (model + txd), so those go into the stream...
  writeText(mod / "churchlite.ide", "objs\n4000, churchlite, churchlite, 100\nend\n");
  writeText(mod / "churchlite.ipl", "inst\n4000, churchlite, 0,0,0\nend\n");
  // ...but a plugin's loose texture (no IDE references it) must NOT be swallowed
  // into the IMG -- it has to stay on disk where the plugin reads it.
  writeText(mod / "x360btns.txd", "BUTTONS");
  writeText(mod / "fonts.txd", "MODDED-FONTS");                // -> models/fonts.txd

  App app(data);
  app.init(game.string());
  app.setAutoRouteMaps(true);  // experimental map auto-install is opt-in
  const Mod m = app.importFromFolder(mod, "Church Lite").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  const auto vanillaGtaDat = readAll(game / "data" / "gta.dat");
  const auto vanillaImg    = readAll(game / "models" / "gta3.img");

  app.deploy();

  // New map-definition files staged under data/maps/gmm/ (not dumped at root).
  CHECK(fs::exists(game / "data" / "maps" / "gmm" / "churchlite.ide"));
  CHECK(fs::exists(game / "data" / "maps" / "gmm" / "churchlite.ipl"));
  CHECK_FALSE(fs::exists(game / "churchlite.ide"));
  CHECK_FALSE(fs::exists(game / "churchlite.dff"));  // went into the IMG, not loose

  // gta.dat gained IDE/IPL registrations and kept its base content.
  {
    const auto b = readAll(game / "data" / "gta.dat");
    const std::string dat(b.begin(), b.end());
    CHECK(dat.find("IMG models/gta3.img") != std::string::npos);
    CHECK(dat.find("IDE data/maps/gmm/churchlite.ide") != std::string::npos);
    CHECK(dat.find("IPL data/maps/gmm/churchlite.ipl") != std::string::npos);
  }

  // The IMG now holds the NEW streamed model + txd and the REPLACED stream IPL,
  // but NOT the plugin's loose texture.
  {
    const auto dirv = imgReadDirectory(game / "models" / "gta3.img");
    auto find = [&](const std::string& n) {
      for (const auto& e : dirv)
        if (e.name == n)
          return e;
      return ImgEntry{};
    };
    CHECK(find("churchlite.dff").size > 0);  // streamed model added
    CHECK(find("churchlite.txd").size > 0);  // streamed txd added
    CHECK(find("x360btns.txd").size == 0);   // plugin texture NOT swallowed
    const ImgEntry ce = find("countryw_stream4.ipl");
    REQUIRE(ce.size > 0);
    const auto content = imgReadEntry(game / "models" / "gta3.img", ce);
    const auto modded  = pattern(1500, 7);
    for (std::size_t i = 0; i < modded.size(); ++i)
      REQUIRE(content[i] == modded[i]);  // replaced with the mod's bytes
  }

  // The plugin's loose texture stays on disk (at the game root) so GInput & co.
  // can read it -- this is the regression that closed the game.
  {
    REQUIRE(fs::exists(game / "x360btns.txd"));
    const auto b = readAll(game / "x360btns.txd");
    CHECK(std::string(b.begin(), b.end()) == "BUTTONS");
  }

  // The bare fonts.txd was repathed to its vanilla location.
  {
    const auto f = readAll(game / "models" / "fonts.txd");
    CHECK(std::string(f.begin(), f.end()) == "MODDED-FONTS");
    CHECK_FALSE(fs::exists(game / "fonts.txd"));
  }

  // Rollback restores the game byte-for-byte and removes staged map files.
  app.rollback();
  CHECK_FALSE(fs::exists(game / "data" / "maps" / "gmm"));
  CHECK_FALSE(fs::exists(game / "x360btns.txd"));  // loose deploy removed
  CHECK(readAll(game / "data" / "gta.dat") == vanillaGtaDat);
  CHECK(readAll(game / "models" / "gta3.img") == vanillaImg);
  {
    const auto f = readAll(game / "models" / "fonts.txd");
    CHECK(std::string(f.begin(), f.end()) == "VANILLA-FONTS");
  }

  fs::remove_all(root);
}

TEST_CASE("radio mod: bare station files route to audio/streams", "[app][audio]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_radio_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  // Vanilla: radio station files live at audio/streams/<name> (no extension).
  writeText(game / "audio" / "streams" / "CO", "VANILLA-CO");
  writeText(game / "audio" / "streams" / "RG", "VANILLA-RG");

  // The real "Radio 1C" mod is just a "streams/" folder of station files. On
  // import normalizeModRoot unwraps the single non-game wrapper, so the pool tree
  // holds the bare station files -- which deploy repaths to audio/streams/.
  writeText(mod / "streams" / "CO", "MODDED-CO");
  writeText(mod / "streams" / "RG", "MODDED-RG");

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "Radio 1C").mod;
  // The wrapper was unwrapped: the pool stores bare station files.
  CHECK(fs::exists(app.modsDir() / m.id / "root" / "CO"));

  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  const auto vanillaCO = readAll(game / "audio" / "streams" / "CO");

  app.deploy();
  // Routed to the real radio location, not dumped at the game root.
  CHECK_FALSE(fs::exists(game / "CO"));
  {
    const auto b = readAll(game / "audio" / "streams" / "CO");
    CHECK(std::string(b.begin(), b.end()) == "MODDED-CO");
  }

  app.rollback();
  CHECK(readAll(game / "audio" / "streams" / "CO") == vanillaCO);

  fs::remove_all(root);
}

TEST_CASE("deploy is atomic: a mid-deploy failure rolls back", "[app]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_atomic_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  fs::create_directories(game);
  writeText(game / "blk", "i am a file, not a folder");  // obstacle

  // a.cfg deploys fine; blk/y.cfg can't (blk is a file) -> deploy throws.
  // (.cfg, not .txt, so the import keeps both rather than treating them as junk.)
  writeText(mod / "a.cfg", "good");
  writeText(mod / "blk" / "y.cfg", "cannot place under a file");

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "M").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  CHECK_THROWS(app.deploy());
  CHECK_FALSE(fs::exists(game / "a.cfg"));   // partial change rolled back
  CHECK(fs::is_regular_file(game / "blk"));  // obstacle untouched
  CHECK_FALSE(app.isDeployed());             // no manifest written

  fs::remove_all(root);
}

TEST_CASE("MoonLoader: bare .lua deploys into moonloader/, packaged kept", "[app][lua]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_lua_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";

  fs::create_directories(game);

  App app(data);
  app.init(game.string());

  // Mod A: a bare top-level script -> should be routed into moonloader/.
  const fs::path modA = root / "modA";
  writeText(modA / "trainer.lua", "-- bare moonloader script");
  const Mod a = app.importFromFolder(modA, "Bare").mod;

  // Mod B: already packaged under moonloader/ (with a lib) -> kept as-is, the
  // moonloader/ prefix must NOT be stripped as a wrapper.
  const fs::path modB = root / "modB";
  writeText(modB / "moonloader" / "speedo.lua", "-- packaged script");
  writeText(modB / "moonloader" / "lib" / "helper.lua", "-- lib");
  const Mod b = app.importFromFolder(modB, "Packaged").mod;
  // The stored tree keeps the moonloader/ folder.
  CHECK(fs::exists(app.modsDir() / b.id / "root" / "moonloader" / "speedo.lua"));

  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(a.id, true);
  app.setEnabled(b.id, true);
  app.deploy();

  CHECK(fs::is_regular_file(game / "moonloader" / "trainer.lua"));
  CHECK_FALSE(fs::exists(game / "trainer.lua"));  // not left at the game root
  CHECK(fs::is_regular_file(game / "moonloader" / "speedo.lua"));
  CHECK(fs::is_regular_file(game / "moonloader" / "lib" / "helper.lua"));

  app.rollback();
  CHECK_FALSE(fs::exists(game / "moonloader" / "trainer.lua"));
  CHECK_FALSE(fs::exists(game / "moonloader" / "speedo.lua"));

  fs::remove_all(root);
}

TEST_CASE("profile separators are ignored at deploy", "[app][separator]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_sep_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  writeText(game / "data" / "x.cfg", "vanilla\r\n");
  writeText(mod / "data" / "x.cfg", "modded\r\n");

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "M").mod;
  app.createProfile("p");
  app.useProfile("p");

  // A separator above an enabled mod.
  std::vector<gtamm::ProfileEntry> entries;
  gtamm::ProfileEntry sep;
  sep.separator = true;
  sep.label     = "Section A";
  sep.priority  = 2;
  entries.push_back(sep);
  gtamm::ProfileEntry me;
  me.modId    = m.id;
  me.enabled  = true;
  me.priority = 1;
  entries.push_back(me);
  app.setActiveProfileEntries(entries);

  const auto vanilla = readAll(game / "data" / "x.cfg");
  app.deploy();
  CHECK(readAll(game / "data" / "x.cfg") != vanilla);  // mod applied
  CHECK(app.conflicts().empty());                       // separator is no conflict
  app.rollback();
  CHECK(readAll(game / "data" / "x.cfg") == vanilla);

  fs::remove_all(root);
}

TEST_CASE("import unwraps a wrapper folder and drops junk", "[app][import]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_import_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path src  = root / "src";

  // A mod archive layout: a wrapper folder + a readme alongside it.
  writeText(src / "Cool Car v1.2" / "data" / "handling.cfg", "INFERNUS x\r\n");
  writeBytes(src / "Cool Car v1.2" / "infernus.dff", pattern(100, 1));
  writeText(src / "readme.txt", "install instructions");

  App app(data);
  app.init("");  // no game needed for an import-only test
  const Mod m = app.importFromFolder(src, "Cool Car").mod;

  const fs::path poolRoot = data / "mods" / m.id / "root";
  CHECK(fs::exists(poolRoot / "data" / "handling.cfg"));  // unwrapped
  CHECK(fs::exists(poolRoot / "infernus.dff"));
  CHECK_FALSE(fs::exists(poolRoot / "Cool Car v1.2"));  // wrapper removed
  CHECK_FALSE(fs::exists(poolRoot / "readme.txt"));     // junk dropped

  fs::remove_all(root);
}

TEST_CASE("import keeps a SA-MP client plugin's plugins/ folder intact",
          "[app][import][samp]")
{
  // Regression: a SA-MP client plugin (e.g. a streamer/anti-cheat-bypass DLL)
  // is commonly distributed as a zip whose ONLY content is a "plugins/" folder
  // holding the .dll -- exactly the single-wrapper-directory shape
  // normalizeModRoot() unwraps for things like "Cool Car v1.2/". Before
  // "plugins" was added to isGameTopDir(), it would get unwrapped like any
  // other wrapper, hoisting the .dll to the mod's root -- which then deploys
  // at the GAME ROOT instead of "plugins/", where SA-MP never loads it from.
  const fs::path root = fs::temp_directory_path() / "gtamm_samp_plugin_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path src  = root / "src";

  writeText(src / "plugins" / "streamer.dll", "STREAMER-PLUGIN-BYTES");

  App app(data);
  app.init("");
  const Mod m = app.importFromFolder(src, "Streamer Plugin").mod;

  const fs::path poolRoot = data / "mods" / m.id / "root";
  CHECK(fs::exists(poolRoot / "plugins" / "streamer.dll"));  // NOT unwrapped
  CHECK_FALSE(fs::exists(poolRoot / "streamer.dll"));        // not hoisted to root

  fs::remove_all(root);
}

TEST_CASE("re-importing an identical mod is deduplicated", "[app][import]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_dedup_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path src  = root / "src";
  const fs::path src2 = root / "src2";

  writeText(src / "data" / "handling.cfg", "INFERNUS x\r\n");
  writeBytes(src / "infernus.dff", pattern(500, 3));
  // identical content under a different wrapper / folder name
  writeText(src2 / "wrapper" / "data" / "handling.cfg", "INFERNUS x\r\n");
  writeBytes(src2 / "wrapper" / "infernus.dff", pattern(500, 3));

  App app(data);
  app.init("");
  const auto first = app.importFromFolder(src, "First");
  CHECK_FALSE(first.wasDuplicate);

  const auto second = app.importFromFolder(src2, "Second");
  CHECK(second.wasDuplicate);
  CHECK(second.mod.id == first.mod.id);  // points at the existing mod
  CHECK(app.mods().size() == 1);         // no second copy stored

  // A genuinely different mod is not flagged as duplicate.
  writeText(src / "data" / "handling.cfg", "CHEETAH y\r\n");
  const auto third = app.importFromFolder(src, "Third");
  CHECK_FALSE(third.wasDuplicate);
  CHECK(app.mods().size() == 2);

  fs::remove_all(root);
}

TEST_CASE("importArchiveAsMods/importFromArchive treat a non-archive file as "
          "a single loose mod file instead of failing to open it as an archive",
          "[app][import]")
{
  // Regression: both the GUI drag-and-drop path and the CLI `import` command
  // decide between importFolderAsMods/importArchiveAsMods purely on
  // fs::is_directory(path) -- a bare mod file dropped/passed directly (a
  // standalone .lua script, .asi plugin, ...) took the "archive" branch and
  // extractArchive() failed to open it as one.
  const fs::path root = fs::temp_directory_path() / "gtamm_bare_file_import_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path lua  = root / "src" / "CoolScript.lua";
  writeText(lua, "-- a standalone moonloader script\n");

  App app(data);
  app.init("");

  const auto results = app.importArchiveAsMods(lua, "");
  REQUIRE(results.size() == 1);
  CHECK_FALSE(results.front().wasDuplicate);
  CHECK(results.front().mod.name == "CoolScript");
  CHECK(fs::exists(app.modsDir() / results.front().mod.id / "root" / "CoolScript.lua"));

  // The non-pack-splitting variant gets the same fallback.
  const fs::path lua2 = root / "src2" / "AnotherScript.lua";
  writeText(lua2, "-- another standalone script\n");
  const auto single = app.importFromArchive(lua2, "");
  CHECK_FALSE(single.wasDuplicate);
  CHECK(fs::exists(app.modsDir() / single.mod.id / "root" / "AnotherScript.lua"));

  fs::remove_all(root);
}

TEST_CASE("manageSaves setting persists and follows profile rename/delete",
          "[app][saves]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_saves_cfg_test";
  fs::remove_all(root);
  const fs::path data = root / "data";

  {
    App app(data);
    app.init("");
    CHECK(app.manageSaves());  // on by default
    app.createProfile("Old");
    app.useProfile("Old");
    app.setManageSaves(true);
    // Seed a saves store for the profile.
    writeText(app.profileSavesDir("Old") / "gta_sa.set", "settings");
  }
  {
    App app(data);  // reload from disk
    CHECK(app.manageSaves());  // setting persisted
    app.renameProfile("Old", "New");
    CHECK(fs::exists(app.profileSavesDir("New") / "gta_sa.set"));
    CHECK_FALSE(fs::exists(app.profileSavesDir("Old")));
    app.deleteProfile("New");
    CHECK_FALSE(fs::exists(app.profileSavesDir("New")));
  }

  fs::remove_all(root);
}

TEST_CASE("steamIntegration setting is off by default and persists", "[app][steam]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_steam_cfg_test";
  fs::remove_all(root);
  const fs::path data = root / "data";

  {
    App app(data);
    app.init("");
    CHECK_FALSE(app.steamIntegration());  // off by default
    app.setSteamIntegration(true);
  }
  {
    App app(data);  // reload from disk
    CHECK(app.steamIntegration());  // setting persisted
    app.setSteamIntegration(false);
    CHECK_FALSE(app.steamIntegration());
  }

  fs::remove_all(root);
}

TEST_CASE("exportMod then import re-creates the mod verbatim", "[app][import][zip]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_modzip_test";
  fs::remove_all(root);
  const fs::path zip = root / "out" / "mod.zip";

  std::string srcHash;
  {
    App app(root / "a");
    app.init("");
    writeText(root / "src" / "data" / "handling.cfg", "INFERNUS x\r\n");
    writeText(root / "src" / "models" / "txd" / "radar01.txd", "tex");
    const Mod m = app.importFromFolder(root / "src", "Cool Mod").mod;
    srcHash      = m.contentHash;
    app.exportMod(m.id, zip);
    CHECK(fs::exists(zip));
  }

  // Import the zip into a fresh instance via the normal archive import path.
  {
    App app(root / "b");
    app.init("");
    const auto results = app.importArchiveAsMods(zip, "");
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].wasDuplicate);

    const Mod& m = results[0].mod;
    CHECK(m.name == "Cool Mod");         // metadata preserved
    CHECK(m.contentHash == srcHash);     // identical content
    const fs::path rt = app.modsDir() / m.id / "root";
    // Exact game-relative layout preserved, no manifest leak into the mod tree.
    CHECK(fs::exists(rt / "data" / "handling.cfg"));
    CHECK(fs::exists(rt / "models" / "txd" / "radar01.txd"));
    CHECK_FALSE(fs::exists(rt / "mod.json"));

    // Re-importing the same zip is deduplicated.
    const auto again = app.importArchiveAsMods(zip, "");
    REQUIRE(again.size() == 1);
    CHECK(again[0].wasDuplicate);
    CHECK(app.mods().size() == 1);
  }

  fs::remove_all(root);
}

TEST_CASE("exportBuild then importBuildArchive round-trips a profile + mods",
          "[app][build][zip]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_buildzip_test";
  fs::remove_all(root);
  const fs::path zip = root / "out" / "build.zip";

  // Redirect the live "User Files" folder to a temp dir so export/import never
  // touch the real Documents folder (export falls back to it, import writes it).
  const fs::path userFiles = root / "UserFiles";
  _putenv_s("GMM_USERFILES_DIR", userFiles.string().c_str());

  std::string carId, skinId;
  {
    App app(root / "a");
    app.init("");
    writeText(root / "src" / "car" / "data" / "handling.cfg", "INFERNUS x\r\n");
    writeText(root / "src" / "skin" / "models" / "player.txd", "skin");
    carId  = app.importFromFolder(root / "src" / "car", "Cool Car").mod.id;
    skinId = app.importFromFolder(root / "src" / "skin", "Cool Skin").mod.id;

    app.createProfile("Build1");
    app.useProfile("Build1");
    std::vector<ProfileEntry> entries;
    ProfileEntry sep;
    sep.separator = true;
    sep.label     = "Visuals";
    entries.push_back(sep);
    ProfileEntry e1;
    e1.modId    = carId;
    e1.enabled  = true;
    e1.priority = 2;
    ProfileEntry e2;
    e2.modId    = skinId;
    e2.enabled  = false;
    e2.priority = 1;
    entries.push_back(e1);
    entries.push_back(e2);
    app.setActiveProfileEntries(entries);

    SampConfig s;
    s.enabled = true;
    s.server  = "1.2.3.4";
    s.nick    = "CJ";
    app.setSampConfig("Build1", s);

    // Seed the profile's saves store with a savegame, settings and some media.
    writeText(app.profileSavesDir("Build1") / "GTASAsf1.b", "SAVE");
    writeText(app.profileSavesDir("Build1") / "gta_sa.set", "SETTINGS");
    writeText(app.profileSavesDir("Build1") / "User Tracks" / "song.mp3", "MEDIA");
    // A save that exists ONLY in the live User Files (no per-profile store entry)
    // must still be exported -- this is the reported bug.
    writeText(userFiles / "GTASAsf2.b", "LIVE-SAVE");

    // Export WITH saves + settings (media must be excluded).
    app.exportBuild("Build1", zip, /*withSaves=*/true, /*withSettings=*/true);
    CHECK(fs::exists(zip));
  }

  // Import into a FRESH instance (separate pool + profiles).
  {
    App app(root / "b");
    app.init("");
    const auto res = app.importBuildArchive(zip, "Imported");
    // Only the ENABLED mod (car) is in the archive -- exportBuild() leaves
    // disabled entries out entirely, so the skin never round-trips and its
    // profile entry is dropped (no matching mod folder in the zip).
    CHECK(res.modsAdded == 1);
    CHECK(res.modsReused == 0);
    CHECK(res.entriesDropped == 1);
    CHECK(app.activeProfile() == "Imported");

    const Profile p = app.loadProfile("Imported");
    REQUIRE(p.entries.size() == 2);           // separator + car; skin dropped
    CHECK(p.entries[0].separator);            // separator kept
    CHECK(p.entries[0].label == "Visuals");
    CHECK(p.entries[1].enabled);              // car enabled
    CHECK(p.samp.enabled);                    // SA-MP carried over
    CHECK(p.samp.server == "1.2.3.4");
    CHECK(p.samp.nick == "CJ");

    // The remapped entries point at real pool mods whose files are present.
    bool foundCar = false;
    for (const auto& m : app.mods()) {
      const fs::path rt = app.modsDir() / m.id / "root";
      if (fs::exists(rt / "data" / "handling.cfg"))
        foundCar = true;
    }
    CHECK(foundCar);

    // Bundled saves + settings landed in the target profile's saves store AND in
    // the live User Files folder (usable in-game); media was not exported.
    CHECK(fs::exists(app.profileSavesDir("Imported") / "GTASAsf1.b"));
    CHECK(fs::exists(app.profileSavesDir("Imported") / "gta_sa.set"));
    CHECK(fs::exists(app.profileSavesDir("Imported") / "GTASAsf2.b"));  // from live folder
    CHECK_FALSE(fs::exists(app.profileSavesDir("Imported") / "User Tracks" / "song.mp3"));
    CHECK(fs::exists(userFiles / "GTASAsf1.b"));   // applied to live saves
    CHECK(fs::exists(userFiles / "gta_sa.set"));

    // Re-importing the same zip deduplicates the mods (none added again).
    const auto res2 = app.importBuildArchive(zip, "Imported2");
    CHECK(res2.modsAdded == 0);
    CHECK(res2.modsReused == 1);
  }

  _putenv_s("GMM_USERFILES_DIR", "");
  fs::remove_all(root);
}

TEST_CASE("isBuildArchive tells a build zip apart from a plain mod zip",
          "[app][build][zip]")
{
  // Regression: drag-and-drop needs to route a build zip (exportBuild's output)
  // to importBuildArchive() instead of importing it as an ordinary mod -- this
  // is the detection it relies on, so it must say yes for a real build zip and
  // no for anything else (a mod zip, a non-archive file, a missing path).
  const fs::path root = fs::temp_directory_path() / "gtamm_isbuildarchive_test";
  fs::remove_all(root);

  App app(root / "data");
  app.init("");
  writeText(root / "src" / "data" / "handling.cfg", "INFERNUS x\r\n");
  const Mod m = app.importFromFolder(root / "src", "Cool Mod").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  const fs::path buildZip = root / "out" / "build.zip";
  const fs::path modZip   = root / "out" / "mod.zip";
  app.exportBuild("p", buildZip);
  app.exportMod(m.id, modZip);

  CHECK(app.isBuildArchive(buildZip));
  CHECK_FALSE(app.isBuildArchive(modZip));
  CHECK_FALSE(app.isBuildArchive(root / "src" / "data" / "handling.cfg"));  // not a zip
  CHECK_FALSE(app.isBuildArchive(root / "does-not-exist.zip"));

  fs::remove_all(root);
}

TEST_CASE("exportBuild/importBuildArchive preserve per-mod viaModloader and the "
          "instance's deploy settings",
          "[app][build][zip][modloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_buildzip_modloader_test";
  fs::remove_all(root);
  const fs::path zip = root / "out" / "build.zip";

  std::string nativeId, mlId;
  {
    App app(root / "a");
    app.init("");
    writeText(root / "src" / "native" / "data" / "handling.cfg", "INFERNUS x\r\n");
    writeText(root / "src" / "ml" / "modloader" / "CoolMod" / "cool.asi", "asi");
    nativeId = app.importFromFolder(root / "src" / "native", "Native Mod").mod.id;
    const auto mlResults =
        app.importFolderAsMods(root / "src" / "ml", "", /*viaModloader=*/true);
    REQUIRE(mlResults.size() == 1);
    mlId = mlResults[0].mod.id;
    CHECK(mlResults[0].mod.viaModloader);

    app.createProfile("Build1");
    app.useProfile("Build1");
    ProfileEntry e1;
    e1.modId    = nativeId;
    e1.enabled  = true;
    e1.priority = 2;
    ProfileEntry e2;
    e2.modId    = mlId;
    e2.enabled  = true;
    e2.priority = 1;
    app.setActiveProfileEntries({e1, e2});

    // Flip every instance-wide deploy toggle away from its default so the test
    // can tell a real round-trip from an accidental match with fresh defaults.
    app.setAutoRouteMaps(true);      // default false
    app.setManageSaves(false);       // default true
    app.setManageGenerated(false);   // default true
    app.setEnableModloader(false);   // default true
    app.setReplaceGameExe(true);     // default false

    app.exportBuild("Build1", zip);
    CHECK(fs::exists(zip));
  }

  // Import into a FRESH instance, which starts with the opposite (default) values.
  {
    App app(root / "b");
    app.init("");
    CHECK(app.autoRouteMaps() == false);
    CHECK(app.manageSaves() == true);
    CHECK(app.manageGenerated() == true);
    CHECK(app.enableModloader() == true);
    CHECK(app.replaceGameExe() == false);

    const auto res = app.importBuildArchive(zip, "Imported");
    CHECK(res.modsAdded == 2);

    // The Mod Loader mod kept its deploy target; the native one stayed native.
    bool foundNative = false, foundMl = false;
    for (const auto& m : app.mods()) {
      if (m.name == "Native Mod") {
        foundNative = true;
        CHECK_FALSE(m.viaModloader);
      } else if (m.name.find("CoolMod") != std::string::npos) {
        foundMl = true;
        CHECK(m.viaModloader);
      }
    }
    CHECK(foundNative);
    CHECK(foundMl);

    // The instance's deploy-wide settings now match the exported build.
    CHECK(app.autoRouteMaps() == true);
    CHECK(app.manageSaves() == false);
    CHECK(app.manageGenerated() == false);
    CHECK(app.enableModloader() == false);
    CHECK(app.replaceGameExe() == true);
  }

  fs::remove_all(root);
}

TEST_CASE("filenames with characters outside the system code page do not crash "
          "baseline scanning or import (UTF-8 process code page)",
          "[baseline][import][unicode]")
{
  // "e" with an acute accent (U+00E9) is not representable in Windows-1251
  // (Cyrillic) and several other legacy ANSI code pages -- exactly the class
  // of filename (accented Latin, CJK, etc. from a modder's archive) that used
  // to crash std::filesystem::path::string()/generic_string() with "No mapping
  // for the Unicode character exists in the target multi-byte code page"
  // wherever a game/mod folder was scanned (buildBaseline, importFromFolder,
  // hashing). Fixed process-wide via the <activeCodePage>UTF-8</activeCodePage>
  // manifest wired into every target in CMakeLists.txt (gmm_utf8_manifest) --
  // this test only proves that wiring actually works for gtamm_tests.exe.
  const fs::path root = fs::temp_directory_path() / "gtamm_unicode_path_test";
  fs::remove_all(root);

  writeText(root / "game" / "caf\xc3\xa9.txt", "vanilla");
  writeText(root / "mod" / "caf\xc3\xa9.txt", "modded");

  Baseline b;
  CHECK_NOTHROW(b = buildBaseline(root / "game"));
  CHECK(b.loose.size() == 1);

  App app(root / "data");
  app.init("");
  CHECK_NOTHROW(app.importFromFolder(root / "mod", "Unicode Mod"));

  fs::remove_all(root);
}

TEST_CASE("SA-MP config persists per profile and survives reload", "[app][samp]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_samp_cfg_test";
  fs::remove_all(root);
  const fs::path data = root / "data";

  {
    App app(data);
    app.init("");
    app.createProfile("mp");
    // Default is disabled with a sensible default port/exe.
    const gtamm::SampConfig def = app.sampConfig("mp");
    CHECK_FALSE(def.enabled);
    CHECK(def.port == 7777);
    CHECK(def.exe == "samp.exe");

    gtamm::SampConfig s;
    s.enabled  = true;
    s.server   = "127.0.0.1";
    s.port     = 7778;
    s.nick     = "Carl";
    s.password = "hunter2";
    s.exe      = "samp.exe";
    app.setSampConfig("mp", s);
  }
  {
    App app(data);  // reload from disk
    const gtamm::SampConfig s = app.sampConfig("mp");
    CHECK(s.enabled);
    CHECK(s.server == "127.0.0.1");
    CHECK(s.port == 7778);
    CHECK(s.nick == "Carl");
    CHECK(s.password == "hunter2");
    // Renaming the profile carries the SA-MP settings along.
    app.renameProfile("mp", "mp2");
    CHECK(app.sampConfig("mp2").nick == "Carl");
    CHECK(app.sampConfig("mp2").password == "hunter2");
  }

  fs::remove_all(root);
}

TEST_CASE("relativePathWillExist checks both the disk and the current deploy "
          "plan for a not-yet-deployed mod file",
          "[app][samp]")
{
  // Regression: a portable build can carry samp.exe (or a whole SAMP/ folder)
  // as part of a MOD's own files rather than something pre-installed by hand
  // into the game folder -- before Deploy runs, the file genuinely isn't on
  // disk yet. The SA-MP settings dialog's "file not found" warning must not
  // cry wolf over that legitimate case, so the check also has to ask "would
  // deploying the active profile right now put it there?", not just look at
  // the raw undeployed game folder.
  const fs::path root = fs::temp_directory_path() / "gtamm_samp_pathcheck_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  fs::create_directories(game);
  writeText(mod / "samp.exe", "SAMP-CLIENT-BYTES");

  App app(data);
  app.init(game.string());

  // No profile active yet, nothing deployed, nothing on disk: false.
  CHECK_FALSE(app.relativePathWillExist("samp.exe"));

  const Mod m = app.importFromFolder(mod, "SAMP Client").mod;
  app.createProfile("p");
  app.useProfile("p");

  // Imported but not enabled: still would not end up deployed.
  CHECK_FALSE(app.relativePathWillExist("samp.exe"));

  app.setEnabled(m.id, true);
  // Enabled, but NOT deployed yet -- true because the CURRENT plan would place
  // it there, even though the file isn't physically in the game folder yet.
  CHECK_FALSE(fs::exists(game / "samp.exe"));
  CHECK(app.relativePathWillExist("samp.exe"));
  CHECK_FALSE(app.relativePathWillExist("nonexistent-launcher.exe"));

  app.deploy();
  // Now it genuinely is on disk too.
  CHECK(fs::exists(game / "samp.exe"));
  CHECK(app.relativePathWillExist("samp.exe"));

  // An absolute path is just a plain existence check, deploy plan irrelevant.
  CHECK(app.relativePathWillExist((game / "samp.exe").string()));
  CHECK_FALSE(app.relativePathWillExist((game / "nope.exe").string()));

  app.rollback();
  fs::remove_all(root);
}

TEST_CASE("pendingRootExecutables lists a mod's root-level exe before Deploy "
          "ever runs",
          "[app][samp]")
{
  // Regression: the GUI's run-exe picker only ever scanned the game folder on
  // DISK, so a portable build whose samp.exe comes from a mod (not installed
  // by hand ahead of time) never showed up as a choice until after Deploy --
  // "in the run picker there's only gta_sa.exe, how do I even get SA-MP as an
  // option" was the exact complaint this fixes.
  const fs::path root = fs::temp_directory_path() / "gtamm_pending_exe_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  fs::create_directories(game);
  writeText(mod / "samp.exe", "SAMP-CLIENT-BYTES");
  // A file inside a subfolder is not a "root" executable -- must be excluded.
  writeText(mod / "plugins" / "helper.exe", "NOT-A-ROOT-EXE");

  App app(data);
  app.init(game.string());
  CHECK(app.pendingRootExecutables().empty());  // nothing enabled yet

  const Mod m = app.importFromFolder(mod, "SAMP Client").mod;
  app.createProfile("p");
  app.useProfile("p");
  CHECK(app.pendingRootExecutables().empty());  // imported but not enabled

  app.setEnabled(m.id, true);
  const auto pending = app.pendingRootExecutables();
  CHECK(std::find(pending.begin(), pending.end(), "samp.exe") != pending.end());
  // The subfolder exe must not be reported as a root executable.
  CHECK(std::find(pending.begin(), pending.end(), "helper.exe") == pending.end());

  fs::remove_all(root);
}

TEST_CASE("applyProfileSaves junctions User Files and restore brings it back",
          "[app][saves]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_saves_swap_test";
  fs::remove_all(root);
  const fs::path data      = root / "data";
  const fs::path userFiles = root / "User Files";  // stand-in for Documents\...

  // Pretend the user already has real saves/settings.
  writeText(userFiles / "gta_sa.set", "ORIGINAL");
  _putenv_s("GMM_USERFILES_DIR", userFiles.string().c_str());

  App app(data);
  app.init("");
  app.createProfile("p");
  app.useProfile("p");
  app.setManageSaves(true);

  app.applyProfileSaves();
  CHECK(userfiles::isReparsePoint(userFiles));  // now redirected
  // The original real folder was parked aside, not destroyed.
  CHECK(fs::exists(root / "User Files.gmm-backup" / "gta_sa.set"));

  // The "game" writes a save; it must land in the profile's store via the link.
  writeText(userFiles / "GTASAsf1.b", "SAVEDATA");
  CHECK(fs::exists(app.profileSavesDir("p") / "GTASAsf1.b"));

  app.restoreProfileSaves();
  CHECK_FALSE(userfiles::isReparsePoint(userFiles));
  // Original settings are back in place, byte-for-byte.
  {
    const auto bytes = readAll(userFiles / "gta_sa.set");
    CHECK(std::string(bytes.begin(), bytes.end()) == "ORIGINAL");
  }
  // The profile keeps the save written during the session.
  {
    const auto bytes = readAll(app.profileSavesDir("p") / "GTASAsf1.b");
    CHECK(std::string(bytes.begin(), bytes.end()) == "SAVEDATA");
  }

  _putenv_s("GMM_USERFILES_DIR", "");
  fs::remove_all(root);
}

TEST_CASE("default settings template seeds a fresh profile's saves store, "
          "never clobbers an existing one",
          "[app][settings-template]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_settings_tmpl_test";
  fs::remove_all(root);
  const fs::path data = root / "data";

  App app(data);
  app.init("");
  app.createProfile("A");
  app.useProfile("A");
  app.setManageSaves(true);

  // Nothing captured yet.
  CHECK_FALSE(app.hasDefaultSettingsTemplate());
  CHECK_THROWS(app.saveCurrentSettingsAsTemplate());

  // Simulate the user having played once and configured gta_sa.set (Full HD,
  // invert off) in the active profile's own store.
  writeText(app.profileSavesDir("A") / "gta_sa.set", "FULLHD-NOINVERT");
  app.saveCurrentSettingsAsTemplate();
  CHECK(app.hasDefaultSettingsTemplate());

  // A brand-new profile has no gta_sa.set yet -- seeding copies the template in.
  app.createProfile("B");
  app.useProfile("B");
  CHECK_FALSE(fs::exists(app.profileSavesDir("B") / "gta_sa.set"));
  app.seedDefaultSettingsIfMissing();
  {
    const auto bytes = readAll(app.profileSavesDir("B") / "gta_sa.set");
    CHECK(std::string(bytes.begin(), bytes.end()) == "FULLHD-NOINVERT");
  }

  // It never overwrites settings that already exist (real or previously seeded).
  writeText(app.profileSavesDir("B") / "gta_sa.set", "USER-CHANGED-IT");
  app.seedDefaultSettingsIfMissing();
  {
    const auto bytes = readAll(app.profileSavesDir("B") / "gta_sa.set");
    CHECK(std::string(bytes.begin(), bytes.end()) == "USER-CHANGED-IT");
  }

  // Clearing the template makes seeding a no-op for the next fresh profile.
  app.clearDefaultSettingsTemplate();
  CHECK_FALSE(app.hasDefaultSettingsTemplate());
  app.createProfile("C");
  app.useProfile("C");
  app.seedDefaultSettingsIfMissing();
  CHECK_FALSE(fs::exists(app.profileSavesDir("C") / "gta_sa.set"));

  fs::remove_all(root);
}

TEST_CASE("default settings template seeds the live User Files folder when "
          "manageSaves is off",
          "[app][settings-template]")
{
  const fs::path root      = fs::temp_directory_path() / "gtamm_settings_tmpl_live_test";
  fs::remove_all(root);
  const fs::path data      = root / "data";
  const fs::path userFiles = root / "User Files";
  _putenv_s("GMM_USERFILES_DIR", userFiles.string().c_str());

  App app(data);
  app.init("");
  app.createProfile("p");
  app.useProfile("p");
  app.setManageSaves(false);

  // Capture from the live folder (this is where the game would have written it).
  writeText(userFiles / "gta_sa.set", "TEMPLATE-BYTES");
  app.saveCurrentSettingsAsTemplate();
  CHECK(app.hasDefaultSettingsTemplate());

  // Simulate a fresh install: no live settings file yet.
  fs::remove(userFiles / "gta_sa.set");
  app.seedDefaultSettingsIfMissing();
  {
    const auto bytes = readAll(userFiles / "gta_sa.set");
    CHECK(std::string(bytes.begin(), bytes.end()) == "TEMPLATE-BYTES");
  }

  _putenv_s("GMM_USERFILES_DIR", "");
  fs::remove_all(root);
}

TEST_CASE("renameProfile preserves contents and active selection", "[app][profile]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_rename_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path src  = root / "src";

  writeText(src / "data" / "handling.cfg", "INFERNUS x\r\n");

  App app(data);
  app.init("");
  const Mod m = app.importFromFolder(src, "Mod").mod;
  app.createProfile("Old Name");
  app.useProfile("Old Name");
  app.setEnabled(m.id, true);
  app.setPriority(m.id, 7);

  app.renameProfile("Old Name", "New Name");

  // Old gone, new present, and it is the active profile.
  const auto names = app.profileNames();
  CHECK(std::find(names.begin(), names.end(), "Old Name") == names.end());
  CHECK(std::find(names.begin(), names.end(), "New Name") != names.end());
  CHECK(app.activeProfile() == "New Name");

  // Contents (name + entries) carried over intact.
  const Profile p = app.loadProfile("New Name");
  CHECK(p.name == "New Name");
  REQUIRE(p.entries.size() == 1);
  CHECK(p.entries[0].modId == m.id);
  CHECK(p.entries[0].enabled);
  CHECK(p.entries[0].priority == 7);

  // Renaming onto an existing name is rejected.
  app.createProfile("Other");
  CHECK_THROWS(app.renameProfile("New Name", "Other"));

  fs::remove_all(root);
}

TEST_CASE("copyProfile duplicates mods and selectively copies saves/settings",
          "[app][profile]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_copy_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path src  = root / "src";
  writeText(src / "data" / "handling.cfg", "INFERNUS x\r\n");

  App app(data);
  app.init("");
  const Mod m = app.importFromFolder(src, "Mod").mod;
  app.createProfile("Base");
  app.useProfile("Base");
  app.setEnabled(m.id, true);
  app.setPriority(m.id, 5);
  app.saveProfileInfo("Base", "# Base notes");

  // Seed the source profile's saves store with a save, a settings file and junk.
  const fs::path sv = app.profileSavesDir("Base");
  writeText(sv / "GTASAsf1.b", "save");
  writeText(sv / "gta_sa.set", "settings");
  writeText(sv / "User Tracks" / "track.mp3", "music");

  // Copy with saves only.
  app.copyProfile("Base", "OnlySaves", /*saves=*/true, /*settings=*/false);
  const Profile c1 = app.loadProfile("OnlySaves");
  REQUIRE(c1.entries.size() == 1);
  CHECK(c1.entries[0].modId == m.id);
  CHECK(c1.entries[0].enabled);
  CHECK(c1.entries[0].priority == 5);
  CHECK(app.loadProfileInfo("OnlySaves") == "# Base notes");  // notes travel
  CHECK(fs::exists(app.profileSavesDir("OnlySaves") / "GTASAsf1.b"));
  CHECK(!fs::exists(app.profileSavesDir("OnlySaves") / "gta_sa.set"));
  CHECK(!fs::exists(app.profileSavesDir("OnlySaves") / "User Tracks" / "track.mp3"));

  // Copy with both saves and settings.
  app.copyProfile("Base", "Both", /*saves=*/true, /*settings=*/true);
  CHECK(fs::exists(app.profileSavesDir("Both") / "GTASAsf1.b"));
  CHECK(fs::exists(app.profileSavesDir("Both") / "gta_sa.set"));
  CHECK(!fs::exists(app.profileSavesDir("Both") / "User Tracks" / "track.mp3"));

  // Copy with neither -> just the profile, no saves store.
  app.copyProfile("Base", "Clean", false, false);
  CHECK(fs::exists(data / "profiles" / "Clean.json"));
  CHECK(!fs::exists(app.profileSavesDir("Clean") / "GTASAsf1.b"));

  // Copying onto an existing name is rejected.
  CHECK_THROWS(app.copyProfile("Base", "Both", false, false));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// SFX: sound-bank injection
// ---------------------------------------------------------------------------

namespace {
std::string slurp(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}
// Write a minimal 16-bit-mono PCM WAV with the given sample rate and bytes.
void writeWav(const fs::path& p, std::uint16_t rate, const std::string& pcm)
{
  fs::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::binary);
  auto u32 = [&](std::uint32_t v) { o.put(v & 0xFF).put((v >> 8) & 0xFF).put((v >> 16) & 0xFF).put((v >> 24) & 0xFF); };
  auto u16 = [&](std::uint16_t v) { o.put(v & 0xFF).put((v >> 8) & 0xFF); };
  o.write("RIFF", 4); u32(36 + (std::uint32_t)pcm.size()); o.write("WAVE", 4);
  o.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  o.write("data", 4); u32((std::uint32_t)pcm.size()); o.write(pcm.data(), (std::streamsize)pcm.size());
}
// Write a one-pak SFX set (PakFiles/BankLkup + the pak file) with two banks,
// each holding sounds whose PCM is given. Returns the pak path.
void writeSfxSet(const fs::path& audioDir, const std::vector<std::vector<std::string>>& banksSounds)
{
  fs::create_directories(audioDir / "CONFIG");
  fs::create_directories(audioDir / "SFX");
  { std::ofstream o(audioDir / "CONFIG" / "PakFiles.dat", std::ios::binary);
    std::string nm = "GENRL"; nm.resize(52, '\0'); o.write(nm.data(), 52); }
  std::ofstream pak(audioDir / "SFX" / "GENRL", std::ios::binary);
  std::string lkup;
  std::uint32_t off = 0;
  auto put32 = [](std::string& s, std::uint32_t v) {
    s.push_back(v & 0xFF); s.push_back((v >> 8) & 0xFF);
    s.push_back((v >> 16) & 0xFF); s.push_back((v >> 24) & 0xFF);
  };
  for (const auto& sounds : banksSounds) {
    std::string hdr(sfx::kBankHeader, '\0');
    hdr[0] = (char)(sounds.size() & 0xFF); hdr[1] = (char)((sounds.size() >> 8) & 0xFF);
    std::string audio;
    for (std::size_t k = 0; k < sounds.size(); ++k) {
      std::uint32_t bo = (std::uint32_t)audio.size();
      std::memcpy(&hdr[4 + k * 12], &bo, 4);
      std::uint32_t loop = 0xFFFFFFFF; std::memcpy(&hdr[4 + k * 12 + 4], &loop, 4);
      std::uint16_t rate = 22050; std::memcpy(&hdr[4 + k * 12 + 8], &rate, 2);
      audio += sounds[k];
    }
    pak.write(hdr.data(), (std::streamsize)hdr.size());
    pak.write(audio.data(), (std::streamsize)audio.size());
    lkup.push_back(0); lkup.push_back((char)0xCD); lkup.push_back((char)0xCD); lkup.push_back((char)0xCD);
    put32(lkup, off); put32(lkup, (std::uint32_t)audio.size());
    off += (std::uint32_t)sfx::kBankHeader + (std::uint32_t)audio.size();
  }
  pak.close();
  std::ofstream(audioDir / "CONFIG" / "BankLkup.dat", std::ios::binary).write(lkup.data(), (std::streamsize)lkup.size());
}
}  // namespace

TEST_CASE("SFX rebuildPak: identity with no edits, replaces one sound", "[sfx]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_sfx_test";
  fs::remove_all(root);
  const fs::path audio = root / "audio";
  // bank0: 2 sounds; bank1: 1 sound
  writeSfxSet(audio, {{"AAAA", "BBBBBBBB"}, {"CCCCCC"}});
  const fs::path pak = audio / "SFX" / "GENRL";
  const std::string before = slurp(pak);

  auto banks = sfx::readBankLkup(audio / "CONFIG" / "BankLkup.dat");
  REQUIRE(banks.size() == 2);

  // No edits -> byte-identical rebuild.
  const fs::path out1 = root / "g1";
  sfx::rebuildPak(pak, out1, 0, banks, {});
  CHECK(slurp(out1) == before);

  // Replace bank0 sound1 with a longer wav -> bank1 shifts, lkup updates.
  writeWav(root / "new.wav", 32000, "ZZZZZZZZZZ");  // 10 bytes
  auto banks2 = sfx::readBankLkup(audio / "CONFIG" / "BankLkup.dat");
  const fs::path out2 = root / "g2";
  sfx::rebuildPak(pak, out2, 0, banks2, {{0, {{1, root / "new.wav"}}}});
  // bank0 audioLen: 4 (sound0) + 10 (new sound1) = 14
  CHECK(banks2[0].audioLen == 14);
  CHECK(banks2[0].offset == 0);
  CHECK(banks2[1].offset == sfx::kBankHeader + 14);
  CHECK(banks2[1].audioLen == 6);  // bank1 unchanged

  // Verify the replaced PCM is present in the rebuilt pak.
  const std::string r2 = slurp(out2);
  CHECK(r2.find("ZZZZZZZZZZ") != std::string::npos);
  CHECK(r2.find("AAAA") != std::string::npos);   // sound0 kept
  CHECK(r2.find("CCCCCC") != std::string::npos);  // bank1 kept

  fs::remove_all(root);
}

TEST_CASE("per-profile build info: markdown + image, survives rename", "[app][profile]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_info_test";
  fs::remove_all(root);
  const fs::path data = root / "data";

  App app(data);
  app.init("");
  app.createProfile("Build");
  app.useProfile("Build");

  // Empty by default.
  CHECK(app.loadProfileInfo("Build").empty());

  // Save markdown + add an image, which lands under the info folder.
  writeText(root / "pic.png", "\x89PNG fake");
  const std::string rel = app.addProfileInfoImage("Build", root / "pic.png");
  CHECK(rel == "images/pic.png");
  CHECK(fs::exists(app.profileInfoDir("Build") / "images" / "pic.png"));

  const std::string md = "# Build\n\n![p](" + rel + ")\n";
  app.saveProfileInfo("Build", md);
  CHECK(app.loadProfileInfo("Build") == md);

  // Carried over on rename.
  app.renameProfile("Build", "Build2");
  CHECK(app.loadProfileInfo("Build2") == md);
  CHECK(fs::exists(app.profileInfoDir("Build2") / "images" / "pic.png"));

  // Dropped on delete.
  app.deleteProfile("Build2");
  CHECK(!fs::exists(app.profileInfoDir("Build2")));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Import an existing build: diff vs vanilla, split into mods
// ---------------------------------------------------------------------------

TEST_CASE("classifyBuildFile splits by structure", "[build]")
{
  using gtamm::classifyBuildFile;
  const std::set<std::string> noStems;

  // Mod Loader: strip the wrapper, keep the mod name, content stays game-relative.
  auto g = classifyBuildFile("modloader/CoolCar/models/x.dff", noStems, noStems, noStems, "B");
  CHECK(g.key == "ml:coolcar");
  CHECK(g.name == "CoolCar");
  CHECK(g.innerRel == "models/x.dff");
  CHECK(g.primary);

  // An ASI plugin and its side .ini.
  g = classifyBuildFile("CLEO.asi", noStems, noStems, noStems, "B");
  CHECK(g.key == "asi:cleo");
  CHECK(g.primary);
  g = classifyBuildFile("CLEO.ini", {"cleo"}, noStems, noStems, "B");
  CHECK(g.key == "asi:cleo");
  CHECK_FALSE(g.primary);

  // A CLEO script and its side .ini -- and crucially, an ASI named "CLEO" must
  // NOT swallow the cleo/ scripts folder.
  g = classifyBuildFile("cleo/speedo.cs", noStems, noStems, noStems, "B");
  CHECK(g.key == "cleo:speedo");
  CHECK(g.primary);
  g = classifyBuildFile("cleo/speedo.ini", noStems, {"speedo"}, noStems, "B");
  CHECK(g.key == "cleo:speedo");
  g = classifyBuildFile("cleo/other.cs", {"cleo"}, noStems, noStems, "B");
  CHECK(g.key == "cleo:other");  // not asi:cleo

  // A top-level MoonLoader script and its side .ini get their own mod...
  g = classifyBuildFile("moonloader/speedo.lua", noStems, noStems, noStems, "B");
  CHECK(g.key == "lua:speedo");
  CHECK(g.name == "speedo");
  CHECK(g.primary);
  g = classifyBuildFile("moonloader/speedo.ini", noStems, noStems, {"speedo"}, "B");
  CHECK(g.key == "lua:speedo");
  CHECK_FALSE(g.primary);
  // ...but a script in a SUBFOLDER (a shared library the scripts require(),
  // not a standalone mod) is deliberately left out of the split.
  g = classifyBuildFile("moonloader/lib/samp/events.lua", noStems, noStems, noStems, "B");
  CHECK(g.key == "main");

  // The unstructured remainder is lumped into one mod named after the build.
  g = classifyBuildFile("data/handling.cfg", noStems, noStems, noStems, "MyBuild");
  CHECK(g.key == "main");
  CHECK(g.name == "MyBuild");
}

TEST_CASE("importBuild diffs a modded folder against vanilla", "[app][build]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_build_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";   // clean vanilla (the instance baseline)
  const fs::path data  = root / "data";
  const fs::path build = root / "build";  // a modded copy to import

  // Vanilla: a handling.cfg, an unchanged file, and a gta3.img with infernus.dff.
  writeText(game / "data" / "handling.cfg", "INFERNUS van\r\n");
  writeText(game / "data" / "unchanged.dat", "keep\r\n");
  writeImg(game / "models" / "gta3.img", {{"infernus.dff", pattern(2000, 5)}});

  // The build: same layout but with mods of every recognised shape.
  writeText(build / "data" / "handling.cfg", "INFERNUS modded\r\n");  // changed loose
  writeText(build / "data" / "unchanged.dat", "keep\r\n");            // identical -> skip
  writeImg(build / "models" / "gta3.img", {{"infernus.dff", pattern(2000, 9)}});  // changed entry
  writeText(build / "cleo" / "speedo.cs", "script");                 // CLEO mod
  writeText(build / "cleo" / "speedo.ini", "cfg");                   // CLEO side file
  writeText(build / "CLEO.asi", "plugin");                           // ASI mod
  writeText(build / "modloader" / "MyCar" / "models" / "foo.dff", "car");  // ML mod
  writeText(build / "readme.txt", "docs");                           // junk -> skip

  App app(data);
  app.init(game.string());

  App::ImportBuildOptions opts;
  opts.profileName = "Imported";
  const auto r = app.importBuild(build, opts);

  CHECK(r.profile == "Imported");
  // modloader/MyCar is imported wholesale (not diffed); the rest is diffed:
  // handling.cfg, speedo.cs, speedo.ini, CLEO.asi.
  CHECK(r.looseChanged == 4);
  CHECK(r.imgChanged == 1);  // infernus.dff
  // Four mods: modloader car, CLEO script (+side), ASI plugin, and the remainder.
  CHECK(r.createdModIds.size() == 4);
  CHECK(app.activeProfile() == "Imported");

  // Skipped files are reported individually, with a reason, not just counted.
  CHECK(r.skipped == 2);
  REQUIRE(r.skippedFiles.size() == 2);
  auto findSkip = [&](const std::string& rel) -> const App::SkippedFile* {
    for (const auto& s : r.skippedFiles)
      if (s.rel == rel)
        return &s;
    return nullptr;
  };
  const auto* unchangedSkip = findSkip("data/unchanged.dat");
  REQUIRE(unchangedSkip);
  CHECK(unchangedSkip->reason.find("unchanged") != std::string::npos);
  const auto* junkSkip = findSkip("readme.txt");
  REQUIRE(junkSkip);
  CHECK(junkSkip->reason.find("junk") != std::string::npos);

  // The lumped "main" mod is imported last; it holds the changed loose data file
  // and the extracted IMG entry (which deploy re-injects).
  const std::string mainId = r.createdModIds.back();
  const fs::path mainRoot  = app.modsDir() / mainId / "root";
  CHECK(fs::exists(mainRoot / "data" / "handling.cfg"));
  CHECK(fs::exists(mainRoot / "infernus.dff"));

  // The profile has one entry per mod, all enabled.
  const Profile p = app.loadProfile("Imported");
  int enabled = 0;
  for (const auto& e : p.entries)
    if (e.enabled && !e.separator)
      ++enabled;
  CHECK(enabled == 4);

  // A second import re-uses the cached baseline (no game rescan needed).
  CHECK(app.hasBaseline());

  // Deploying the imported build applies the change and rolls back cleanly.
  const auto vanillaHandling = readAll(game / "data" / "handling.cfg");
  app.deploy();
  {
    const auto bytes = readAll(game / "data" / "handling.cfg");
    const std::string s(bytes.begin(), bytes.end());
    CHECK(s.find("INFERNUS modded") != std::string::npos);
  }
  app.rollback();
  CHECK(readAll(game / "data" / "handling.cfg") == vanillaHandling);

  fs::remove_all(root);
}

TEST_CASE("importBuild skips runtime-written logs and NSIS installer scratch "
          "dirs instead of baking them into a mod forever",
          "[app][build]")
{
  // A folder that's already been played at least once (MoonLoader/CLEO/mod_sa
  // all write their own logs; an NSIS-based modpack installer can leave its
  // "$PLUGINSDIR" extraction scratch dir behind) must not have that debris
  // swept into a mod -- it would then get redeployed on every single Deploy,
  // "keep the game folder clean" notwithstanding (that toggle only manages
  // files a *session* creates after Deploy, not a mod's own baked-in files).
  const fs::path root = fs::temp_directory_path() / "gtamm_build_debris_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  writeText(game / "data" / "handling.cfg", "INFERNUS van\r\n");

  writeText(build / "data" / "handling.cfg", "INFERNUS modded\r\n");  // a real change
  writeText(build / "cleo.log", "cleo ran");                          // CLEO's own log
  writeText(build / "moonloader" / "moonloader.log", "ml ran");       // MoonLoader's own log
  writeText(build / "mod_sa" / "mod_sa.log", "diagnostics");          // mod_sa's own log
  writeText(build / "$PLUGINSDIR" / "installer.dat", "nsis scratch"); // installer temp dir

  App app(data);
  app.init(game.string());

  App::ImportBuildOptions opts;
  opts.profileName = "Imported";
  const auto r = app.importBuild(build, opts);

  auto findSkip = [&](const std::string& rel) -> const App::SkippedFile* {
    for (const auto& s : r.skippedFiles)
      if (s.rel == rel)
        return &s;
    return nullptr;
  };
  for (const std::string& rel :
       {"cleo.log", "moonloader/moonloader.log", "mod_sa/mod_sa.log",
        "$PLUGINSDIR/installer.dat"}) {
    const auto* skip = findSkip(rel);
    REQUIRE(skip);
    CHECK(skip->reason.find("runtime-generated") != std::string::npos);
  }

  // None of the debris made it into any imported mod's tree.
  for (const auto& modId : r.createdModIds) {
    const fs::path modRoot = app.modsDir() / modId / "root";
    CHECK(!fs::exists(modRoot / "cleo.log"));
    CHECK(!fs::exists(modRoot / "moonloader" / "moonloader.log"));
    CHECK(!fs::exists(modRoot / "mod_sa" / "mod_sa.log"));
    CHECK(!fs::exists(modRoot / "$PLUGINSDIR"));
  }

  // The genuine change still went through.
  REQUIRE(!r.createdModIds.empty());
  const fs::path mainRoot = app.modsDir() / r.createdModIds.back() / "root";
  CHECK(fs::exists(mainRoot / "data" / "handling.cfg"));

  fs::remove_all(root);
}

TEST_CASE("rollback's orphan cleanup does not delete vanilla IMG archives",
          "[app][rollback][baseline][img]")
{
  // Regression: cleanOrphansAgainstBaseline() only checked Baseline::loose
  // (whole-file entries) for "is this a known vanilla file" -- but .img
  // archives are tracked per INTERNAL ENTRY in Baseline::img (keys like
  // "gta3.img::infernus.dff"), never as a whole-file path in `loose`. So
  // every .img container in the game folder looked like an unrecognized
  // foreign file and got deleted outright on every rollback with orphan
  // cleanup on (manageGenerated defaults on) and a baseline present -- wiping
  // gta3.img/player.img/gta_int.img/etc. wholesale.
  const fs::path root = fs::temp_directory_path() / "gtamm_orphan_img_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";

  writeText(game / "data" / "handling.cfg", "INFERNUS van\r\n");
  writeImg(game / "models" / "gta3.img", {{"infernus.dff", pattern(2000, 5)}});

  App app(data);
  app.init(game.string());
  app.refreshVanillaBaseline();
  CHECK(app.hasBaseline());

  // A genuinely foreign file that predates GMM tracking entirely -- this one
  // SHOULD be swept up as an orphan.
  writeText(game / "cleo" / "SomeOldScript.cs", "junk");

  app.createProfile("Empty");
  app.useProfile("Empty");
  app.deploy();    // nothing to deploy, but marks the folder as deployed
  app.rollback();  // orphan cleanup runs here

  CHECK(fs::exists(game / "models" / "gta3.img"));              // must survive
  CHECK(fs::exists(game / "data" / "handling.cfg"));            // vanilla loose survives
  CHECK_FALSE(fs::exists(game / "cleo" / "SomeOldScript.cs"));  // true orphan removed

  fs::remove_all(root);
}

TEST_CASE("importBuild preserves a non-vanilla .img whole and does not treat "
          "changed image assets as junk (SA-MP's own SAMP.img/mouse.png)",
          "[app][build][samp]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_build_samp_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  // Vanilla has only the real streaming archive -- no SAMP/ folder at all.
  writeImg(game / "models" / "gta3.img", {{"infernus.dff", pattern(2000, 5)}});

  // The build is the same game PLUS a bundled SA-MP client: its own custom
  // object library (a VER2 archive the baseline has never heard of) and its
  // own cursor/GUI textures at the game root.
  writeImg(build / "models" / "gta3.img", {{"infernus.dff", pattern(2000, 5)}});  // unchanged
  writeBytes(build / "SAMP" / "SAMP.img", pattern(4096, 3));   // opaque, non-vanilla .img
  writeBytes(build / "mouse.png", pattern(64, 7));             // SA-MP cursor texture
  writeBytes(build / "sampgui.png", pattern(64, 11));          // SA-MP GUI texture
  writeText(build / "samp.exe", "samp launcher");

  App app(data);
  app.init(game.string());

  App::ImportBuildOptions opts;
  opts.profileName = "Imported";
  const auto r = app.importBuild(build, opts);

  // None of the SA-MP files should be reported as skipped/junk/unchanged.
  for (const auto& s : r.skippedFiles) {
    CHECK(s.rel != "SAMP/SAMP.img");
    CHECK(s.rel != "mouse.png");
    CHECK(s.rel != "sampgui.png");
  }

  REQUIRE(!r.createdModIds.empty());
  const std::string mainId = r.createdModIds.back();
  const fs::path mainRoot  = app.modsDir() / mainId / "root";

  // SAMP.img must survive as a single container file, not be shattered into
  // its individual entries (it has no vanilla counterpart to diff against).
  CHECK(fs::exists(mainRoot / "SAMP" / "SAMP.img"));
  CHECK(fs::file_size(mainRoot / "SAMP" / "SAMP.img") == fs::file_size(build / "SAMP" / "SAMP.img"));

  // The cursor/GUI textures must be imported as real assets, not dropped.
  CHECK(fs::exists(mainRoot / "mouse.png"));
  CHECK(fs::exists(mainRoot / "sampgui.png"));

  fs::remove_all(root);
}

TEST_CASE("importBuild merges same-stem CLEO script variants into one mod, "
          "and reports the merge instead of silently dropping them",
          "[app][build]")
{
  // Real CLEO packages commonly ship the same script compiled for several
  // CLEO versions (Script.cs + Script.cs3), which classifyBuildFile groups
  // under one mod by design -- but from the outside that can look like "8
  // scripts went in, only 4 mods came out", so it must be visible, not silent.
  const fs::path root = fs::temp_directory_path() / "gtamm_build_merge_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  writeText(game / "data" / "gta.dat", "IMG models\r\n");

  writeText(build / "cleo" / "Speedo.cs", "v3 bytecode");
  writeText(build / "cleo" / "Speedo.cs3", "v3 bytecode, same script");
  writeText(build / "cleo" / "Radar.cs", "another script");

  App app(data);
  app.init(game.string());

  App::ImportBuildOptions opts;
  opts.profileName = "Merged";
  const auto r = app.importBuild(build, opts);

  CHECK(r.looseChanged == 3);      // all three files really did change
  CHECK(r.createdModIds.size() == 2);  // but only 2 mods: Speedo (merged) + Radar

  // Both Speedo variants actually landed on disk, inside the SAME mod
  // (directory iteration order isn't guaranteed, so find it by content
  // rather than assuming a position in createdModIds).
  bool foundSpeedoCs = false, foundSpeedoCs3 = false;
  for (const auto& id : r.createdModIds) {
    const fs::path modRoot = app.modsDir() / id / "root";
    if (fs::exists(modRoot / "cleo" / "Speedo.cs"))
      foundSpeedoCs = true;
    if (fs::exists(modRoot / "cleo" / "Speedo.cs3"))
      foundSpeedoCs3 = true;
    // Both variants must be in the SAME mod, not split across two.
    if (fs::exists(modRoot / "cleo" / "Speedo.cs") !=
        fs::exists(modRoot / "cleo" / "Speedo.cs3"))
      FAIL("Speedo.cs and Speedo.cs3 ended up in different mods: " << id);
  }
  CHECK(foundSpeedoCs);
  CHECK(foundSpeedoCs3);

  // The merge is called out in the notes so it doesn't read as data loss.
  bool mentionsMerge = false;
  for (const auto& n : r.notes)
    if (n.find("Speedo.cs3") != std::string::npos)
      mentionsMerge = true;
  CHECK(mentionsMerge);

  fs::remove_all(root);
}

TEST_CASE("importBuild splits top-level moonloader scripts into their own mods, "
          "but keeps subfolder libraries lumped with the rest",
          "[app][build][moonloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_build_moonloader_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  writeText(game / "data" / "gta.dat", "IMG models\r\n");

  writeText(build / "moonloader" / "cool_script.lua", "print('hi')");
  writeText(build / "moonloader" / "cool_script.ini", "[settings]\r\nfoo=1\r\n");
  writeText(build / "moonloader" / "lib" / "samp" / "events.lua", "-- shared samp lib");

  App app(data);
  app.init(game.string());

  App::ImportBuildOptions opts;
  opts.profileName = "Moon";
  const auto r = app.importBuild(build, opts);

  // cool_script.lua + its .ini side file get their own mod...
  bool foundScript = false;
  std::string scriptModId;
  for (const auto& id : r.createdModIds) {
    const fs::path modRoot = app.modsDir() / id / "root";
    if (fs::exists(modRoot / "moonloader" / "cool_script.lua")) {
      foundScript   = true;
      scriptModId   = id;
      CHECK(fs::exists(modRoot / "moonloader" / "cool_script.ini"));
    }
  }
  CHECK(foundScript);

  // ...but the library under moonloader/lib/ is NOT split out on its own -- it
  // rides along in the unstructured "main" remainder mod instead.
  CHECK_FALSE(fs::exists(app.modsDir() / scriptModId / "root" / "moonloader" / "lib"));
  bool foundLib = false;
  for (const auto& id : r.createdModIds) {
    if (id == scriptModId)
      continue;
    if (fs::exists(app.modsDir() / id / "root" / "moonloader" / "lib" / "samp" / "events.lua"))
      foundLib = true;
  }
  CHECK(foundLib);

  fs::remove_all(root);
}

TEST_CASE("App::splitMoonloaderScripts pulls top-level lua scripts out of an "
          "existing bundled mod, wiring the new mods into every profile that "
          "already had it enabled",
          "[app][moonloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_moonloader_split_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  writeText(mod / "moonloader" / "alpha.lua", "print('alpha')");
  writeText(mod / "moonloader" / "beta.lua", "print('beta')");
  writeText(mod / "moonloader" / "beta.ini", "[cfg]\r\nx=1\r\n");
  writeText(mod / "moonloader" / "lib" / "shared.lua", "-- shared library");
  writeText(mod / "data" / "extra.dat", "unrelated file");
  fs::create_directories(game);

  App app(data);
  app.init(game.string());
  const Mod bundled = app.importFromFolder(mod, "BundledPack").mod;

  app.createProfile("p1");
  app.useProfile("p1");
  app.setEnabled(bundled.id, true);
  app.createProfile("p2");  // never references the bundled mod at all

  CHECK(app.listModLuaScripts(bundled.id).size() == 2);  // alpha, beta (top-level only)

  const auto r = app.splitMoonloaderScripts(bundled.id);
  REQUIRE(r.createdModIds.size() == 2);  // one per script
  CHECK(r.profilesUpdated == 1);         // only p1 actually referenced the mod

  // Each new mod has exactly its own script (+ side file where applicable);
  // the shared library and the unrelated data file stayed with the original.
  bool foundAlpha = false, foundBeta = false;
  for (const auto& id : r.createdModIds) {
    const fs::path modRoot = app.modsDir() / id / "root";
    if (fs::exists(modRoot / "moonloader" / "alpha.lua")) {
      foundAlpha = true;
      CHECK_FALSE(fs::exists(modRoot / "moonloader" / "beta.lua"));
    }
    if (fs::exists(modRoot / "moonloader" / "beta.lua")) {
      foundBeta = true;
      CHECK(fs::exists(modRoot / "moonloader" / "beta.ini"));  // side file rode along
    }
  }
  CHECK(foundAlpha);
  CHECK(foundBeta);

  const fs::path originalRoot = app.modsDir() / bundled.id / "root";
  CHECK_FALSE(fs::exists(originalRoot / "moonloader" / "alpha.lua"));
  CHECK_FALSE(fs::exists(originalRoot / "moonloader" / "beta.lua"));
  CHECK(fs::exists(originalRoot / "moonloader" / "lib" / "shared.lua"));  // untouched
  CHECK(fs::exists(originalRoot / "data" / "extra.dat"));                 // untouched

  // p1 (which had the original mod ENABLED) got both new mods inserted,
  // enabled, right after it -- deploying p1 still gets both scripts.
  const Profile p1 = app.loadProfile("p1");
  int enabledNew    = 0;
  for (const auto& e : p1.entries)
    if ((e.modId == r.createdModIds[0] || e.modId == r.createdModIds[1]) && e.enabled)
      ++enabledNew;
  CHECK(enabledNew == 2);

  // p2 never referenced the bundled mod -- untouched, no new entries.
  const Profile p2 = app.loadProfile("p2");
  for (const auto& e : p2.entries)
    CHECK(e.modId != r.createdModIds[0]);

  fs::remove_all(root);
}

TEST_CASE("App::writeModFile overwrites a mod's file in place and refreshes "
          "its cached content hash",
          "[app][moonloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_writemodfile_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  writeText(mod / "moonloader" / "script.lua", "print('old')");
  fs::create_directories(game);

  App app(data);
  app.init(game.string());
  const Mod m            = app.importFromFolder(mod, "Script").mod;
  const std::string oldHash = m.contentHash;

  app.writeModFile(m.id, "moonloader/script.lua", "print('new content')");

  CHECK(slurp(app.modsDir() / m.id / "root" / "moonloader" / "script.lua") ==
        "print('new content')");

  const auto mods = app.mods();
  const auto it = std::find_if(mods.begin(), mods.end(),
                               [&](const Mod& x) { return x.id == m.id; });
  REQUIRE(it != mods.end());
  CHECK(it->contentHash != oldHash);  // dedup won't compare against stale bytes

  fs::remove_all(root);
}

TEST_CASE("importBuild disables the build's own modloader.asi mod when Mod "
          "Loader auto-install is on, to avoid two runtimes fighting",
          "[app][build][modloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_build_ownmodloader_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  writeText(game / "gta_sa.exe", "vanilla");

  // The build carries its own modloader.asi at the root (a real-world case:
  // exported/copied from an already-working install, imported via diff).
  writeText(build / "modloader.asi", "USERS_OWN_MODLOADER");
  writeText(build / "cleo" / "Radar.cs", "unrelated script");

  App app(data);
  app.init(game.string());  // enableModloader defaults to true

  App::ImportBuildOptions opts;
  opts.profileName = "OwnLoader";
  const auto r = app.importBuild(build, opts);

  // The mod itself is still imported (files present, nothing lost)...
  std::string modloaderId;
  for (const auto& id : r.createdModIds)
    if (fs::exists(app.modsDir() / id / "root" / "modloader.asi"))
      modloaderId = id;
  REQUIRE_FALSE(modloaderId.empty());

  // ...but disabled in the profile, so deploy() doesn't place it and GMM's
  // own auto-installed runtime (enableModloader) is the only one active.
  const Profile p = app.loadProfile("OwnLoader");
  bool foundDisabled = false;
  for (const auto& e : p.entries)
    if (e.modId == modloaderId) {
      CHECK_FALSE(e.enabled);
      foundDisabled = true;
    }
  CHECK(foundDisabled);

  // Explained in the notes, not a silent toggle.
  bool mentionsWhy = false;
  for (const auto& n : r.notes)
    if (n.find("modloader.asi") != std::string::npos)
      mentionsWhy = true;
  CHECK(mentionsWhy);

  fs::remove_all(root);
}

TEST_CASE("importBuild leaves the build's own modloader.asi mod enabled when "
          "Mod Loader auto-install is off",
          "[app][build][modloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_build_ownmodloader_off_test";
  fs::remove_all(root);
  const fs::path game  = root / "game";
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  writeText(game / "gta_sa.exe", "vanilla");
  writeText(build / "modloader.asi", "USERS_OWN_MODLOADER");

  App app(data);
  app.init(game.string());
  app.setEnableModloader(false);  // explicitly off

  App::ImportBuildOptions opts;
  opts.profileName = "OwnLoaderKept";
  const auto r = app.importBuild(build, opts);

  std::string modloaderId;
  for (const auto& id : r.createdModIds)
    if (fs::exists(app.modsDir() / id / "root" / "modloader.asi"))
      modloaderId = id;
  REQUIRE_FALSE(modloaderId.empty());

  const Profile p = app.loadProfile("OwnLoaderKept");
  bool foundEnabled = false;
  for (const auto& e : p.entries)
    if (e.modId == modloaderId) {
      CHECK(e.enabled);
      foundEnabled = true;
    }
  CHECK(foundEnabled);

  fs::remove_all(root);
}

TEST_CASE("importFolderAsMods: one mod per modloader folder, wrappers flattened",
          "[app][import][pack]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_pack_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path pack = root / "pack";

  // Mirrors how Mod Loader sees things: each immediate subfolder of modloader/ is
  // ONE mod. "HD" is a single mod whose internal organization folders (Loadscreens
  // /Weapons) must be flattened to game-relative paths. A stray config and readme
  // must not become mods.
  writeText(pack / "modloader" / "Radar HD" / "models" / "txd" / "radar01.txd", "r");
  writeText(pack / "modloader" / "HD" / "Loadscreens" / "models" / "load.txd", "l");
  writeText(pack / "modloader" / "HD" / "Weapons icons" / "models" / "hud.txd", "w");
  writeText(pack / "modloader" / "modloader.ini", "config");
  writeText(pack / "readme.txt", "junk");

  App app(data);
  app.init("");

  const auto results = app.importFolderAsMods(pack, "MyPack");
  CHECK(results.size() == 2);  // "Radar HD" and "HD" (NOT split further)

  std::set<std::string> names;
  for (const auto& r : results)
    names.insert(r.mod.name);
  CHECK(names.count("Radar HD") == 1);
  CHECK(names.count("HD") == 1);

  for (const auto& r : results) {
    const fs::path rt = app.modsDir() / r.mod.id / "root";
    if (r.mod.name == "Radar HD")
      CHECK(fs::exists(rt / "models" / "txd" / "radar01.txd"));
    if (r.mod.name == "HD") {
      // Both inner wrappers collapse onto the game-relative models/ folder.
      CHECK(fs::exists(rt / "models" / "load.txd"));
      CHECK(fs::exists(rt / "models" / "hud.txd"));
      CHECK_FALSE(fs::exists(rt / "Loadscreens"));
    }
  }

  fs::remove_all(root);
}

TEST_CASE("importFolderAsMods keeps a full build (modloader + root content) whole",
          "[app][import][pack]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_fullbuild_test";
  fs::remove_all(root);
  const fs::path data  = root / "data";
  const fs::path build = root / "build";

  // A complete modded install: a modloader/ folder alongside the game exe, root
  // ASIs, CLEO scripts and DLLs. Splitting it as a Mod Loader pack would silently
  // drop everything outside modloader/ -- here it must import as ONE whole mod.
  writeText(build / "gta_sa.exe", "GAME");
  writeText(build / "CLEO.asi", "cleo");
  writeText(build / "CLEO" / "FileSystemOperations.cleo", "fso");
  writeText(build / "modloader.asi", "ml");
  writeText(build / "vorbisFile.dll", "dll");
  writeText(build / "modloader" / ".data" / "config.ini.0", "cfg");
  writeText(build / "modloader" / "_ESSENTIALS" / "SilentPatch" / "SilentPatchSA.asi", "sp");
  writeText(build / "modloader" / "_ESSENTIALS" / "SilentPatch" / "SilentPatch.url", "url");

  App app(data);
  app.init("");

  const auto results = app.importFolderAsMods(build, "FullBuild");
  CHECK(results.size() == 1);  // one whole mod, not split into modloader mods

  const fs::path rt = app.modsDir() / results[0].mod.id / "root";
  // Root content survives (this is what the old splitting path dropped).
  CHECK(fs::exists(rt / "gta_sa.exe"));
  CHECK(fs::exists(rt / "CLEO.asi"));
  CHECK(fs::exists(rt / "CLEO" / "FileSystemOperations.cleo"));
  CHECK(fs::exists(rt / "modloader.asi"));
  CHECK(fs::exists(rt / "vorbisFile.dll"));
  // The modloader tree is preserved verbatim too (not flattened/re-based).
  CHECK(fs::exists(rt / "modloader" / "_ESSENTIALS" / "SilentPatch" / "SilentPatchSA.asi"));

  fs::remove_all(root);
}

TEST_CASE("importFolderAsMods keeps a SAAT sound mod as one mod", "[app][import][pack]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_sfxmod_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path pack = root / "pack";

  // SGenrl is a single sound mod: its Bank_<n>/ folders are SAAT internals, not
  // separate mods, and must be preserved (so the SFX injector can route them).
  writeText(pack / "modloader" / "SGenrl" / "Bank_046" / "sound_001.wav", "a");
  writeText(pack / "modloader" / "SGenrl" / "Bank_137" / "sound_001.wav", "b");

  App app(data);
  app.init("");

  const auto results = app.importFolderAsMods(pack, "MyPack");
  REQUIRE(results.size() == 1);
  CHECK(results.front().mod.name == "SGenrl");
  const fs::path rt = app.modsDir() / results.front().mod.id / "root";
  CHECK(fs::exists(rt / "Bank_046" / "sound_001.wav"));
  CHECK(fs::exists(rt / "Bank_137" / "sound_001.wav"));

  fs::remove_all(root);
}

TEST_CASE("manageGenerated: captures session files and restores them next launch",
          "[app][generated]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_gen_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";

  writeText(game / "data" / "x.dat", "vanilla");  // a pre-existing game file

  App app(data);
  app.init(game.string());
  app.createProfile("p");
  app.useProfile("p");

  // --- session 1: snapshot the clean folder, the game creates a temp file ---
  std::set<std::string> bf, bd;
  app.snapshotTree(game, bf, bd);
  app.restoreGenerated("p", game);  // stash empty -> nothing
  writeText(game / "cache" / "gen.tmp", "made-by-mod");

  app.captureGenerated("p", game, bf, bd, {});
  // Game folder is clean again; the file (and its new dir) are gone.
  CHECK_FALSE(fs::exists(game / "cache" / "gen.tmp"));
  CHECK_FALSE(fs::exists(game / "cache"));
  CHECK(fs::exists(game / "data" / "x.dat"));  // untouched
  // Captured into the profile store.
  CHECK(fs::exists(app.generatedDir("p") / "cache" / "gen.tmp"));

  // --- session 2: it is restored before launch, then captured again ---
  std::set<std::string> bf2, bd2;
  app.snapshotTree(game, bf2, bd2);
  const auto restored = app.restoreGenerated("p", game);
  CHECK(restored.count("cache/gen.tmp") == 1);
  CHECK(fs::exists(game / "cache" / "gen.tmp"));

  app.captureGenerated("p", game, bf2, bd2, restored);
  CHECK_FALSE(fs::exists(game / "cache" / "gen.tmp"));  // cleaned again
  CHECK_FALSE(fs::exists(game / "cache"));              // restored dir pruned too
  CHECK(fs::exists(app.generatedDir("p") / "cache" / "gen.tmp"));

  // --- restore must never clobber an existing (deployed/vanilla) file ---
  writeText(app.generatedDir("p") / "data" / "x.dat", "stashed-version");
  const auto r3 = app.restoreGenerated("p", game);
  CHECK(r3.count("data/x.dat") == 0);  // skipped: data/x.dat already present
  CHECK(readAll(game / "data" / "x.dat") ==
        std::vector<unsigned char>({'v', 'a', 'n', 'i', 'l', 'l', 'a'}));

  fs::remove_all(root);
}

TEST_CASE("importFolderAsMods keeps an ASI plugin mod whole (no flatten)",
          "[app][import][pack]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_asimod_test";
  fs::remove_all(root);
  const fs::path data = root / "data";
  const fs::path pack = root / "pack";

  // CheatMenuSA: an .asi plus its data subfolder and config -- all game-root-
  // relative and must keep their exact layout (the subfolder must NOT collapse).
  writeText(pack / "modloader" / "CheatMenuSA" / "CheatMenuSA.asi", "asi");
  writeText(pack / "modloader" / "CheatMenuSA" / "CheatMenuSA" / "cheats.json", "d");
  writeText(pack / "modloader" / "CheatMenuSA" / "CheatMenuSA.toml", "cfg");
  writeText(pack / "modloader" / "CheatMenuSA" / "CheatMenuSA.log", "log");

  App app(data);
  app.init("");

  const auto results = app.importFolderAsMods(pack, "x");
  REQUIRE(results.size() == 1);
  CHECK(results.front().mod.name == "CheatMenuSA");
  const fs::path rt = app.modsDir() / results.front().mod.id / "root";
  CHECK(fs::exists(rt / "CheatMenuSA.asi"));
  CHECK(fs::exists(rt / "CheatMenuSA" / "cheats.json"));  // subfolder preserved
  CHECK(fs::exists(rt / "CheatMenuSA.toml"));

  fs::remove_all(root);
}

TEST_CASE("importFolderAsMods repaths a bare loose file to its vanilla location",
          "[app][import][pack]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_repath_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path pack = root / "pack";

  // Vanilla keeps fonts.txd loose under models/. A mod shipping it bare at its
  // root must be re-based to models/fonts.txd (where the game reads it).
  writeText(game / "models" / "fonts.txd", "vanilla");
  writeText(pack / "modloader" / "RusFonts" / "fonts.txd", "modded");

  App app(data);
  app.init(game.string());

  const auto results = app.importFolderAsMods(pack, "x");
  REQUIRE(results.size() == 1);
  const fs::path rt = app.modsDir() / results.front().mod.id / "root";
  CHECK(fs::exists(rt / "models" / "fonts.txd"));
  CHECK_FALSE(fs::exists(rt / "fonts.txd"));

  fs::remove_all(root);
}

TEST_CASE("via Mod Loader: verbatim import, deploy under modloader/, runtime install",
          "[app][modloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_modloader_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path pack = root / "pack";

  // Minimal game folder.
  writeText(game / "data" / "gta.dat", "IMG models\r\n");

  // A Mod Loader mod authored with its own internal wrapper folder -- with Mod
  // Loader support the tree must be kept VERBATIM (no flattening).
  writeText(pack / "modloader" / "CoolMod" / "wrap" / "models" / "cool.txt", "hi");

  App app(data);
  app.init(game.string());

  // Provide a runtime for the manager to install into the game.
  writeText(app.modloaderRuntimeDir() / "modloader.asi", "ASI");
  writeText(app.modloaderRuntimeDir() / "dinput8.dll", "LOADER");
  CHECK(app.hasModloaderRuntime());

  const auto results =
      app.importFolderAsMods(pack, "MyPack", /*viaModloader=*/true);
  REQUIRE(results.size() == 1);
  const Mod m = results.front().mod;
  CHECK(m.name == "CoolMod");
  CHECK(m.viaModloader);
  // Stored verbatim (the wrapper folder is preserved, unlike native flattening).
  CHECK(fs::exists(app.modsDir() / m.id / "root" / "wrap" / "models" / "cool.txt"));

  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  app.deploy();
  // Mod files land under modloader/CoolMod/ exactly as authored.
  CHECK(fs::is_regular_file(game / "modloader" / "CoolMod" / "wrap" / "models" /
                            "cool.txt"));
  // Runtime installed at the game root.
  CHECK(fs::is_regular_file(game / "modloader.asi"));
  CHECK(fs::is_regular_file(game / "dinput8.dll"));

  app.rollback();
  CHECK_FALSE(fs::exists(game / "modloader" / "CoolMod"));
  CHECK_FALSE(fs::exists(game / "modloader.asi"));
  CHECK_FALSE(fs::exists(game / "dinput8.dll"));
  CHECK_FALSE(app.isDeployed());

  // Toggling to native changes routing: files deploy loose, no runtime installed
  // -- with the global "always install" switch off too (it defaults to on, see
  // the [app][modloader] "enableModloader" test below for that behaviour).
  app.setModViaModloader(m.id, false);
  app.setEnableModloader(false);
  app.deploy();
  CHECK_FALSE(fs::exists(game / "modloader" / "CoolMod"));
  CHECK_FALSE(fs::exists(game / "modloader.asi"));
  CHECK(fs::is_regular_file(game / "wrap" / "models" / "cool.txt"));
  app.rollback();

  fs::remove_all(root);
}

TEST_CASE("enableModloader installs the runtime + bundled exe on a bare "
          "profile with no mods, and rollback restores both",
          "[app][modloader]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_modloader_bare_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";

  writeText(game / "gta_sa.exe", "VANILLA_EXE");

  App app(data);
  app.init(game.string());  // also creates+activates a "Default" profile

  // A runtime, including a bundled clean exe, but no mod flagged viaModloader.
  writeText(app.modloaderRuntimeDir() / "modloader.asi", "ASI");
  writeText(app.modloaderRuntimeDir() / "dinput8.dll", "LOADER");
  writeText(app.modloaderRuntimeDir() / "gta_sa.exe", "CLEAN_1_0_EXE");
  CHECK(app.hasModloaderRuntime());
  CHECK(app.hasBundledExe());
  CHECK(app.mods().empty());  // genuinely bare: nothing imported

  app.setEnableModloader(true);
  CHECK_FALSE(app.replaceGameExe());  // the separate toggle stays off...

  app.deploy();
  // ...yet the exe still gets swapped, because Mod Loader implies it.
  CHECK(fs::is_regular_file(game / "modloader.asi"));
  CHECK(fs::is_regular_file(game / "dinput8.dll"));
  CHECK(slurp(game / "gta_sa.exe") == "CLEAN_1_0_EXE");

  app.rollback();
  CHECK_FALSE(fs::exists(game / "modloader.asi"));
  CHECK_FALSE(fs::exists(game / "dinput8.dll"));
  CHECK(slurp(game / "gta_sa.exe") == "VANILLA_EXE");  // original restored
  CHECK_FALSE(app.isDeployed());

  fs::remove_all(root);
}

TEST_CASE("enableModloader does not swap the exe when a mod already brings "
          "its own self-consistent modloader.asi",
          "[app][modloader]")
{
  // Regression: importing an already-working modded build (its own native
  // modloader.asi + loader DLLs, deployed loose like any other mod) must NOT
  // have the bundled exe forced in on top of it just because the global
  // enableModloader switch is on -- that exe may not be what the user's own
  // loader DLLs were calibrated against, breaking an otherwise working setup.
  const fs::path root = fs::temp_directory_path() / "gtamm_modloader_ownexe_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  writeText(game / "gta_sa.exe", "USERS_OWN_EXE");
  // A plain, natively-imported mod that happens to BE the user's own working
  // Mod Loader install (e.g. via "import build"), not flagged viaModloader.
  writeText(mod / "modloader.asi", "USERS_OWN_MODLOADER");

  App app(data);
  app.init(game.string());  // enableModloader defaults to true
  CHECK(app.enableModloader());
  const Mod m = app.importFromFolder(mod, "MyOwnModloader").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  // OUR bundled runtime is also available, with a DIFFERENT exe -- without the
  // fix this would get installed over/alongside the user's own.
  writeText(app.modloaderRuntimeDir() / "modloader.asi", "OUR_MODLOADER");
  writeText(app.modloaderRuntimeDir() / "gta_sa.exe", "OUR_CLEAN_EXE");

  app.deploy();
  // The user's own modloader.asi wins (ours is never even copied over it).
  CHECK(slurp(game / "modloader.asi") == "USERS_OWN_MODLOADER");
  // The exe is left exactly as it was -- not swapped for our bundled one.
  CHECK(slurp(game / "gta_sa.exe") == "USERS_OWN_EXE");

  app.rollback();
  fs::remove_all(root);
}

TEST_CASE("replaceGameExe does not swap (or corrupt rollback of) a gta_sa.exe "
          "a mod already deployed on its own",
          "[app][modloader][exe]")
{
  // Regression: a real import-build of a pre-built compilation (e.g. a "leftover
  // of the build" mod for a pack with its own patched/no-CD exe, which its
  // bundled exe-patching ASI plugins were built against) deploys that exe as
  // ordinary loose content. The exe-replace step used to fire independently
  // afterward (replaceGameExe implies it, or Mod Loader auto-install does), and
  // since it re-checked "does the game already have OUR exe" against whatever
  // is on disk AT THAT POINT (the mod's own exe, already deployed), it would:
  // (a) install ITS bundled exe over the mod's -- silently running the wrong
  // exe against exe-version-sensitive ASI plugins (crash on mission load, a
  // black screen with sound/HUD still present, or screen flicker are the
  // classic symptoms), and worse
  // (b) write a SECOND manifest entry for the same "gta_sa.exe" path. On
  // rollback, the first entry's restore (the mod's) would consume its backup
  // (moveFile moves, not copies) and put the TRUE original back -- but the
  // second entry's restore attempt would then delete that just-restored file
  // and find no backup left to put back, leaving gta_sa.exe MISSING entirely.
  const fs::path root = fs::temp_directory_path() / "gtamm_exe_double_manifest_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  writeText(game / "gta_sa.exe", "TRUE_VANILLA_EXE");
  // A plain native mod (no viaModloader, no modloader.asi of its own) that just
  // happens to include its own patched gta_sa.exe, like import-build's "main"
  // bucket for a compilation with a custom exe.
  writeText(mod / "gta_sa.exe", "PACK_PATCHED_EXE");
  writeText(mod / "somefile.dat", "x");

  App app(data);
  app.init(game.string());
  app.setEnableModloader(false);  // isolate: only replaceGameExe is in play here
  const Mod m = app.importFromFolder(mod, "PackLeftover").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);
  app.setReplaceGameExe(true);

  writeText(app.modloaderRuntimeDir() / "gta_sa.exe", "OUR_BUNDLED_EXE");

  app.deploy();
  // The pack's own exe wins -- ours is never installed over it.
  CHECK(slurp(game / "gta_sa.exe") == "PACK_PATCHED_EXE");

  app.rollback();
  // The TRUE original is intact -- not lost, not left missing.
  REQUIRE(fs::exists(game / "gta_sa.exe"));
  CHECK(slurp(game / "gta_sa.exe") == "TRUE_VANILLA_EXE");
  CHECK_FALSE(app.isDeployed());

  fs::remove_all(root);
}

TEST_CASE("the bundled Mod Loader runtime does not overwrite a runtime file "
          "(CLEO.asi etc.) a native mod already deployed",
          "[app][modloader][runtime]")
{
  // Regression: deployModloaderRuntime()'s "already installed" gate only ever
  // checked for modloader.asi. A profile can have modloader.asi disabled (its
  // own copy deduped/turned off, e.g. because enableModloader will install
  // ours instead) while STILL having other native mods that independently
  // deploy loader-adjacent files under their OWN names -- e.g. import-build's
  // "leftover of the build" mod for a pre-built compilation carries its own
  // CLEO.asi/(_noDEP.asi/bass.dll as plain loose files, not flagged
  // viaModloader. Since the gate only looked at modloader.asi, it used to walk
  // the whole runtime tree and silently overwrite any OTHER same-named file a
  // mod already placed with GMM's own bundled version -- running a different
  // CLEO build than the rest of the pack was calibrated against is a classic
  // cause of the GTA SA "Runtime Error! ... requested the Runtime to terminate
  // it in an unusual way" crash. It would also double the manifest entry for
  // that path, corrupting rollback the same way the gta_sa.exe bug did.
  const fs::path root = fs::temp_directory_path() / "gtamm_runtime_ownfile_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  // A native mod (like import-build's "leftover" bucket) with its own CLEO.asi,
  // but no modloader.asi of its own -- so the top-level gate does not fire.
  writeText(mod / "CLEO.asi", "PACK_OWN_CLEO");
  fs::create_directories(game);  // requireGameDir() needs it to already exist

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "PackLeftover").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);
  app.setEnableModloader(true);  // forces our bundled runtime install

  writeText(app.modloaderRuntimeDir() / "modloader.asi", "OUR_MODLOADER");
  writeText(app.modloaderRuntimeDir() / "CLEO.asi", "OUR_DIFFERENT_CLEO");
  writeText(app.modloaderRuntimeDir() / "dinput8.dll", "OUR_LOADER_DLL");

  app.deploy();
  // The runtime itself still installs (nothing blocks modloader.asi)...
  CHECK(slurp(game / "modloader.asi") == "OUR_MODLOADER");
  CHECK(slurp(game / "dinput8.dll") == "OUR_LOADER_DLL");
  // ...but the pack's own CLEO.asi is left alone, not overwritten by ours.
  CHECK(slurp(game / "CLEO.asi") == "PACK_OWN_CLEO");

  app.rollback();
  CHECK_FALSE(fs::exists(game / "modloader.asi"));
  CHECK_FALSE(fs::exists(game / "dinput8.dll"));
  CHECK_FALSE(fs::exists(game / "CLEO.asi"));  // never existed before deploy
  CHECK_FALSE(app.isDeployed());

  fs::remove_all(root);
}

TEST_CASE("the bundled _ESSENTIALS runtime does not double-install a same-named "
          "ASI plugin a pack's own mods already brought at a different path",
          "[app][modloader][runtime][essentials]")
{
  // Regression: our bundled runtime's _ESSENTIALS folder ships SilentPatchSA.asi,
  // GTASA.WidescreenFix.asi and wshps.asi (Widescreen HOR+ Support) under its OWN
  // "modloader/_ESSENTIALS/<name>/" paths. A pre-built compilation (e.g. Alexander
  // PolyAK's) commonly already carries its OWN copies of these exact plugins --
  // a root-level "wshps.asi" (loaded directly by the ASI loader) and/or its own
  // "modloader/SilentPatch/SilentPatchSA.asi" (a separate viaModloader mod) --
  // at DIFFERENT paths than ours, so the exact-path check alone does not catch
  // the collision. Loading the same exe-patching/D3D9-hooking ASI twice from two
  // locations is a well-known cause of a black 3D world (HUD still visible) and
  // a garbled/torn front end menu -- exactly what was reported after importing
  // such a pack.
  const fs::path root = fs::temp_directory_path() / "gtamm_essentials_dup_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  // A native mod with its own root-level wshps.asi (Widescreen HOR+), exactly
  // like import-build classifies a pack's root-level ASI plugin.
  writeText(mod / "wshps.asi", "PACK_OWN_WSHPS");
  fs::create_directories(game);  // requireGameDir() needs it to already exist

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "wshps").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);
  app.setEnableModloader(true);

  writeText(app.modloaderRuntimeDir() / "modloader.asi", "OUR_MODLOADER");
  writeText(app.modloaderRuntimeDir() / "modloader" / "_ESSENTIALS" /
                "Widescreen HOR+ Support by Wesser" / "wshps.asi",
            "OUR_DIFFERENT_WSHPS");
  writeText(app.modloaderRuntimeDir() / "modloader" / "_ESSENTIALS" /
                "SilentPatch" / "SilentPatchSA.asi",
            "OUR_SILENTPATCH");  // an unrelated essential with no name clash

  app.deploy();
  // The pack's own copy at the game root is untouched...
  CHECK(slurp(game / "wshps.asi") == "PACK_OWN_WSHPS");
  // ...our colliding copy under _ESSENTIALS was never installed at all...
  CHECK_FALSE(fs::exists(
      game / "modloader" / "_ESSENTIALS" / "Widescreen HOR+ Support by Wesser" / "wshps.asi"));
  // ...but an unrelated essential with no name clash still installs normally.
  CHECK(slurp(game / "modloader" / "_ESSENTIALS" / "SilentPatch" / "SilentPatchSA.asi") ==
        "OUR_SILENTPATCH");

  app.rollback();
  CHECK_FALSE(fs::exists(game / "modloader"));
  CHECK_FALSE(fs::exists(game / "wshps.asi"));  // the mod's own file never existed pre-deploy
  CHECK_FALSE(app.isDeployed());

  fs::remove_all(root);
}

TEST_CASE("deploySampRuntime fills in only the SA-MP client files an enabled mod "
          "hasn't already provided, gated on the profile's SampConfig",
          "[app][samp][runtime]")
{
  // Regression: a hand-imported/partial SA-MP mod (exactly the "SAMP_Stable"
  // real-world case that motivated this bundle) can be missing
  // mouse.png/sampgui.png/SAMP.img even after the import-build fixes, simply
  // because the source install it came from wasn't complete, or was imported
  // before those fixes existed. The bundled client backfills whatever is
  // missing without ever overriding a file the mod already got right.
  const fs::path root = fs::temp_directory_path() / "gtamm_samp_runtime_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";

  writeText(mod / "samp.exe", "MOD_SAMP_EXE");
  writeText(mod / "SAMP" / "SAMP.ide", "MOD_SAMP_IDE");
  fs::create_directories(game);

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "SAMP_Stable").mod;
  app.createProfile("p");
  app.useProfile("p");
  app.setEnabled(m.id, true);

  // A full, correct bundled client -- including a DIFFERENT samp.exe than the
  // mod's, to prove the mod's own copy wins where it exists.
  writeText(app.sampRuntimeDir() / "samp.exe", "BUNDLED_SAMP_EXE");
  writeText(app.sampRuntimeDir() / "mouse.png", "BUNDLED_MOUSE");
  writeText(app.sampRuntimeDir() / "sampgui.png", "BUNDLED_SAMPGUI");
  writeText(app.sampRuntimeDir() / "SAMP" / "SAMP.img", "BUNDLED_SAMP_IMG");
  writeText(app.sampRuntimeDir() / "SAMP" / "SAMP.ide", "BUNDLED_SAMP_IDE");

  SECTION("SampConfig disabled by default -- the bundle is not installed at all") {
    app.deploy();
    CHECK_FALSE(fs::exists(game / "mouse.png"));
    CHECK_FALSE(fs::exists(game / "SAMP" / "SAMP.img"));
    app.rollback();
  }

  SECTION("SampConfig enabled -- backfills what's missing, never stomps the mod's own") {
    SampConfig samp;
    samp.enabled = true;
    app.setSampConfig("p", samp);

    app.deploy();
    CHECK(slurp(game / "samp.exe") == "MOD_SAMP_EXE");           // mod wins
    CHECK(slurp(game / "SAMP" / "SAMP.ide") == "MOD_SAMP_IDE");  // mod wins
    CHECK(slurp(game / "mouse.png") == "BUNDLED_MOUSE");         // backfilled
    CHECK(slurp(game / "sampgui.png") == "BUNDLED_SAMPGUI");     // backfilled
    CHECK(slurp(game / "SAMP" / "SAMP.img") == "BUNDLED_SAMP_IMG");  // backfilled

    app.rollback();
    CHECK_FALSE(fs::exists(game / "mouse.png"));
    CHECK_FALSE(fs::exists(game / "SAMP" / "SAMP.img"));
    CHECK_FALSE(fs::exists(game / "samp.exe"));  // never existed pre-deploy either
    CHECK_FALSE(app.isDeployed());
  }

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Mod content types (the badges shown in the mod list)
// ---------------------------------------------------------------------------

TEST_CASE("modTypes classifies what a mod actually contains", "[rules][tags]")
{
  using rules::ModType;
  auto types = [](std::vector<std::string> files, bool viaModloader = false) {
    return rules::modTypes(files, viaModloader);
  };

  // One tag per kind of content.
  CHECK(types({"cleo/Trainer.cs"}) == std::vector<ModType>{ModType::Cleo});
  CHECK(types({"cleo/Something.cs4"}) == std::vector<ModType>{ModType::Cleo});
  CHECK(types({"CLEO/plugin.cleo"}) == std::vector<ModType>{ModType::Cleo});
  CHECK(types({"SilentPatchSA.asi"}) == std::vector<ModType>{ModType::Asi});
  CHECK(types({"moonloader/speedo.lua"}) == std::vector<ModType>{ModType::Lua});
  CHECK(types({"moonloader/compiled.luac"}) == std::vector<ModType>{ModType::Lua});
  CHECK(types({"data/handling.cfg"}) == std::vector<ModType>{ModType::Data});
  CHECK(types({"data/maps/country/countryN.ide"}) == std::vector<ModType>{ModType::Data});

  // A pure texture/model pack has nothing more specific to say about it.
  CHECK(types({"models/gta3/infernus.txd", "models/gta3/infernus.dff"}) ==
        std::vector<ModType>{ModType::Other});
  CHECK(types({"audio/streams/CO"}) == std::vector<ModType>{ModType::Other});
  CHECK(types({}) == std::vector<ModType>{ModType::Other});

  SECTION("several kinds at once produce several tags, in enum order") {
    const auto t = types({"models/skin.txd", "CLEO/menu.cs", "menu.asi",
                          "moonloader/helper.lua", "data/weapon.dat"});
    CHECK(t == std::vector<ModType>{ModType::Cleo, ModType::Asi, ModType::Lua,
                                    ModType::Data});
    // "Other" is NOT added just because the mod also ships a texture: it only
    // stands in when nothing specific matched.
    CHECK(std::find(t.begin(), t.end(), ModType::Other) == t.end());
  }

  SECTION("Mod Loader shows up from the deploy flag or from the path") {
    CHECK(types({"models/car.dff"}, /*viaModloader=*/true) ==
          std::vector<ModType>{ModType::ModLoader});
    CHECK(types({"modloader/CoolMod/models/car.dff"}) ==
          std::vector<ModType>{ModType::ModLoader});
    // ...alongside whatever else the pack carries.
    CHECK(types({"modloader/CoolMod/cleo/x.cs"}) ==
          std::vector<ModType>{ModType::Cleo, ModType::ModLoader});
  }

  SECTION("readmes and screenshots alone don't make a mod 'Other' plus junk") {
    // Junk is ignored for classification, but a mod of nothing but junk still
    // gets exactly one (fallback) tag rather than none.
    CHECK(types({"readme.txt", "Screenshot.png"}) ==
          std::vector<ModType>{ModType::Other});
  }

  SECTION("ids are stable (the GUI keys colours/labels off them)") {
    CHECK(std::string(rules::modTypeId(ModType::Cleo)) == "cleo");
    CHECK(std::string(rules::modTypeId(ModType::Asi)) == "asi");
    CHECK(std::string(rules::modTypeId(ModType::Lua)) == "lua");
    CHECK(std::string(rules::modTypeId(ModType::ModLoader)) == "modloader");
    CHECK(std::string(rules::modTypeId(ModType::Data)) == "data");
    CHECK(std::string(rules::modTypeId(ModType::Other)) == "other");
  }
}

// ---------------------------------------------------------------------------
// Sanny Builder: pushing GMM's settings into a (portable) SB4 install
// ---------------------------------------------------------------------------

TEST_CASE("syncSannyBuilder writes GamePath and redirects the CLEO output",
          "[app][sanny]")
{
  const fs::path root = fs::temp_directory_path() / "gtamm_sanny_test";
  fs::remove_all(root);
  const fs::path game = root / "game";
  const fs::path data = root / "data";
  const fs::path mod  = root / "mod";
  const fs::path sb   = root / "SannyBuilder";

  fs::create_directories(game);
  writeText(game / "gta_sa.exe", "stub");
  writeText(mod / "cleo" / "Existing.cs", "x");

  // A miniature but faithful SB4 install: settings.ini with an unrelated
  // section that must survive, plus the current mode's mode.xml.
  writeText(sb / "sanny.exe", "stub");
  writeText(sb / "data" / "settings.ini",
            "[Main]\r\n"
            "Editor::Theme=Dark Orange\r\n"
            "EditMode=sa_sbl\r\n"
            "[Debugger]\r\n"
            "Debugger::ExePath=\r\n");
  writeText(sb / "data" / "sa_sbl" / "mode.xml",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<mode id=\"sa_sbl\" title=\"GTA SA\" game=\"sa\" type=\"default\">\n"
            "    <ide base=\"@game:\\\">@game:\data\gta.dat</ide>\n"
            "    <copy-directory type=\"scm\">@game:\data\script</copy-directory>\n"
            "    <copy-directory type=\"cleo\">@game:\CLEO</copy-directory>\n"
            "</mode>\n");

  App app(data);
  app.init(game.string());
  const Mod m = app.importFromFolder(mod, "Scripts").mod;
  app.setSannyBuilderPath(sb.string());

  // The exe is found through the folder (SB4 ships it as sanny.exe).
  REQUIRE(app.hasSannyBuilder());
  CHECK(app.sannyBuilderExe() == sb / "sanny.exe");

  SECTION("without a target mod only the game path is pushed")
  {
    const sanny::SyncResult r = app.syncSannyBuilder();
    CHECK(r.mode == "sa_sbl");
    CHECK(r.game == "sa");  // the ini section is the mode's GAME, not the mode
    CHECK(r.gamePathSet);
    CHECK_FALSE(r.cleoDirSet);

    const std::string ini = readText(sb / "data" / "settings.ini");
    CHECK(ini.find("[sa]") != std::string::npos);
    CHECK(ini.find("GamePath=") != std::string::npos);
    // Untouched settings stay exactly as they were.
    CHECK(ini.find("Editor::Theme=Dark Orange") != std::string::npos);
    CHECK(ini.find("EditMode=sa_sbl") != std::string::npos);
    CHECK(ini.find("[Debugger]") != std::string::npos);
    // The CLEO output is left at its stock value.
    CHECK(readText(sb / "data" / "sa_sbl" / "mode.xml")
              .find("<copy-directory type=\"cleo\">@game:\CLEO</copy-directory>") !=
          std::string::npos);
  }

  SECTION("with a target mod the CLEO output points into the pool")
  {
    app.setSannyCleoModId(m.id);
    const sanny::SyncResult r = app.syncSannyBuilder();
    CHECK(r.gamePathSet);
    CHECK(r.cleoDirSet);

    const fs::path want = app.modsDir() / m.id / "root" / "CLEO";
    CHECK(fs::is_directory(want));  // created for SB, which won't make it itself
    const std::string xml = readText(sb / "data" / "sa_sbl" / "mode.xml");
    CHECK(xml.find(want.string()) != std::string::npos);
    CHECK(xml.find("@game:\CLEO") == std::string::npos);
    // Everything else in the mode file is preserved.
    CHECK(xml.find("<copy-directory type=\"scm\">@game:\data\script") !=
          std::string::npos);
    CHECK(xml.find("@game:\data\gta.dat") != std::string::npos);

    // Running it again is a no-op rather than a second rewrite.
    const std::string before = readText(sb / "data" / "settings.ini");
    app.syncSannyBuilder();
    CHECK(readText(sb / "data" / "settings.ini") == before);
    CHECK(readText(sb / "data" / "sa_sbl" / "mode.xml") == xml);
  }

  SECTION("a stale mod id is reported instead of silently doing nothing")
  {
    app.setSannyCleoModId("no-such-mod");
    const sanny::SyncResult r = app.syncSannyBuilder();
    CHECK(r.gamePathSet);
    CHECK_FALSE(r.cleoDirSet);
    CHECK_FALSE(r.notes.empty());
  }
}
