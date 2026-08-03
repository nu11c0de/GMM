#include "Baseline.h"

#include "Img.h"
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace gtamm {

namespace {

std::string lower(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

std::string fnvHex(std::uint64_t h)
{
  char out[17];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return std::string(out);
}

std::vector<std::string> splitSlash(const std::string& s)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '/') {
      if (!cur.empty())
        out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

std::string extOf(const std::string& filename)
{
  const auto dot = filename.find_last_of('.');
  if (dot == std::string::npos)
    return "";
  return lower(filename.substr(dot));
}

std::string stemOf(const std::string& filename)
{
  const auto dot = filename.find_last_of('.');
  return dot == std::string::npos ? filename : filename.substr(0, dot);
}

bool isCleoScriptExt(const std::string& ext)
{
  return ext == ".cs" || ext == ".cs3" || ext == ".cs4" || ext == ".cs5" ||
         ext == ".cleo";
}

// Standard game folders. An ASI plugin named after one of these (e.g. CLEO.asi)
// must not swallow the whole folder when grouping its side files.
bool isReservedTopDir(const std::string& lc)
{
  static const std::set<std::string> top = {
      "data",     "models",  "text",      "audio",    "anim",
      "movies",   "cleo",    "scripts",   "cutscene", "generic",
      "txd",      "modloader", "moonloader",
      "plugins"};  // SA-MP client plugins (<gamedir>/plugins/*.dll)
  return top.count(lc) > 0;
}

}  // namespace

std::string hashBytesFnv(const unsigned char* data, std::size_t n)
{
  std::uint64_t h = kFnvOffset;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= data[i];
    h *= kFnvPrime;
  }
  return fnvHex(h);
}

std::string hashFileFnv(const fs::path& file)
{
  std::ifstream in(file, std::ios::binary);
  std::uint64_t h = kFnvOffset;
  char buf[65536];
  while (in.read(buf, sizeof(buf)) || in.gcount()) {
    const std::streamsize got = in.gcount();
    for (std::streamsize i = 0; i < got; ++i) {
      h ^= static_cast<unsigned char>(buf[i]);
      h *= kFnvPrime;
    }
  }
  return fnvHex(h);
}

Baseline buildBaseline(const fs::path& gameDir)
{
  Baseline b;
  std::error_code ec;
  if (!fs::is_directory(gameDir, ec))
    throw std::runtime_error("game directory not found: " + gameDir.string());

  for (const auto& de : fs::recursive_directory_iterator(gameDir, ec)) {
    if (!de.is_regular_file(ec))
      continue;
    const fs::path rel = fs::relative(de.path(), gameDir, ec);
    if (ec)
      continue;
    const std::string ext = extOf(de.path().filename().string());

    if (ext == ".img" && imgIsVer2(de.path())) {
      const std::string imgKey = lower(de.path().filename().string());
      for (const auto& entry : imgReadDirectory(de.path())) {
        const std::vector<unsigned char> bytes = imgReadEntry(de.path(), entry);
        BaselineEntry be;
        be.size = entry.size;  // sector count
        be.hash = hashBytesFnv(bytes.data(), bytes.size());
        b.img[imgKey + "::" + lower(entry.name)] = std::move(be);
      }
      continue;
    }

    BaselineEntry be;
    be.size = fs::file_size(de.path(), ec);
    be.hash = hashFileFnv(de.path());
    b.loose[lower(rel.generic_string())] = std::move(be);
  }
  return b;
}

void saveBaseline(const fs::path& path, const Baseline& b)
{
  Json loose = Json::object();
  for (const auto& [k, e] : b.loose)
    loose[k] = Json{{"size", e.size}, {"hash", e.hash}};
  Json img = Json::object();
  for (const auto& [k, e] : b.img)
    img[k] = Json{{"size", e.size}, {"hash", e.hash}};
  const Json j = Json{{"version", 1}, {"loose", loose}, {"img", img}};

  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot write baseline: " + tmp.string());
    out << j.dump() << '\n';
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
    fs::remove(tmp, ec);
  }
}

Baseline loadBaseline(const fs::path& path)
{
  Baseline b;
  std::ifstream in(path);
  if (!in)
    return b;
  Json j;
  in >> j;
  auto read = [](const Json& src, std::map<std::string, BaselineEntry>& dst) {
    if (!src.is_object())
      return;
    for (auto it = src.begin(); it != src.end(); ++it) {
      BaselineEntry e;
      e.size = it.value().value("size", std::uint64_t{0});
      e.hash = it.value().value("hash", std::string{});
      dst[it.key()] = std::move(e);
    }
  };
  read(j.value("loose", Json::object()), b.loose);
  read(j.value("img", Json::object()), b.img);
  return b;
}

BuildGroup classifyBuildFile(const std::string& gameRel,
                             const std::set<std::string>& asiStems,
                             const std::set<std::string>& cleoStems,
                             const std::set<std::string>& moonloaderStems,
                             const std::string& buildName)
{
  const std::vector<std::string> comps = splitSlash(gameRel);
  if (comps.empty())
    return {"main", buildName, gameRel, false};

  const std::string lc0      = lower(comps.front());
  const std::string filename = comps.back();
  const std::string ext      = extOf(filename);
  const std::string stem     = lower(stemOf(filename));

  // 1. Mod Loader layout: modloader/<name>/<game-relative...>. Each top folder is
  // already a named, self-contained mod -- strip the wrapper and keep its name.
  if (lc0 == "modloader" && comps.size() >= 3) {
    std::string inner;
    for (std::size_t i = 2; i < comps.size(); ++i) {
      if (!inner.empty())
        inner += '/';
      inner += comps[i];
    }
    return {"ml:" + lower(comps[1]), comps[1], inner, true};
  }

  // 2. An ASI plugin -- its own mod, deployed where it lay.
  if (ext == ".asi")
    return {"asi:" + stem, stemOf(filename), gameRel, true};

  // 3. A CLEO script -- its own mod. (Checked before ASI side files so a plugin
  // that happens to be named "CLEO" does not claim the whole cleo/ folder.)
  if (lc0 == "cleo" && isCleoScriptExt(ext))
    return {"cleo:" + stem, stemOf(filename), gameRel, true};

  // 4. Side files of a CLEO script (cleo/<script>.ini, cleo/<script>/...).
  if (lc0 == "cleo" && comps.size() >= 2) {
    const std::string cand = lower(stemOf(comps[1]));
    if (cleoStems.count(cand))
      return {"cleo:" + cand, cand, gameRel, false};
  }

  // 5. Side files of an ASI plugin (its <plugin>.ini, or a <plugin>/ data folder).
  // The folder form is ignored for standard game folders to avoid collisions.
  for (const std::string& s : asiStems) {
    const bool topLevelIni = comps.size() == 1 && stem == s;
    const bool inPluginDir = lc0 == s && !isReservedTopDir(lc0);
    if (topLevelIni || inPluginDir)
      return {"asi:" + s, s, gameRel, false};
  }

  // 6. A MoonLoader script sitting directly in moonloader/ -- its own mod, so
  // it gets its own enable/disable checkbox and can be edited independently.
  // A script in a SUBFOLDER (moonloader/lib/..., moonloader/lang/...) is a
  // shared library the scripts above `require()`, not a standalone mod, so it
  // is deliberately excluded here (comps.size() == 2, i.e. moonloader/<file>)
  // and falls through to "main" with the rest of the build.
  if (lc0 == "moonloader" && comps.size() == 2 && ext == ".lua")
    return {"lua:" + stem, stemOf(filename), gameRel, true};

  // 7. Side files of a MoonLoader script (moonloader/<script>.ini, a same-named
  // data subfolder, ...) -- grouped with it the same way as CLEO/ASI above.
  if (lc0 == "moonloader" && comps.size() >= 2) {
    const std::string cand = lower(stemOf(comps[1]));
    if (moonloaderStems.count(cand))
      return {"lua:" + cand, cand, gameRel, false};
  }

  // 8. Everything else: the unstructured remainder of the build, one lumped mod.
  return {"main", buildName, gameRel, false};
}

}  // namespace gtamm
