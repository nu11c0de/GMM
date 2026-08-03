#include "GameRules.h"

#include <cctype>
#include <map>
#include <set>

namespace gtamm::rules {

namespace {

std::string lower(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string extOf(const std::string& filename)
{
  const auto dot = filename.find_last_of('.');
  if (dot == std::string::npos)
    return "";
  return lower(filename.substr(dot));
}

std::vector<std::string> splitLines(const std::string& s)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r')
        cur.pop_back();
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    if (cur.back() == '\r')
      cur.pop_back();
    out.push_back(cur);
  }
  return out;
}

}  // namespace

bool isImgContentFile(const std::string& filename)
{
  const std::string e = extOf(filename);
  return e == ".dff" || e == ".txd" || e == ".col" || e == ".ifp";
}

bool isJunkFile(const std::string& filename)
{
  static const std::set<std::string> junk = {
      ".txt", ".nfo", ".url", ".jpg", ".jpeg", ".png", ".gif",
      ".bmp", ".pdf", ".doc", ".docx", ".rtf", ".html", ".htm", ".md"};
  return junk.count(extOf(filename)) > 0;
}

bool isGameTopDir(const std::string& name)
{
  static const std::set<std::string> top = {
      "data",    "models",   "text",     "audio",  "anim",
      "movies",  "cleo",     "scripts",  "cutscene",   "generic",
      "txd",     "modloader", "moonloader",
      "plugins"};  // SA-MP client plugins (<gamedir>/plugins/*.dll)
  return top.count(lower(name)) > 0;
}

const char* modTypeId(ModType type)
{
  switch (type) {
    case ModType::Cleo:      return "cleo";
    case ModType::Asi:       return "asi";
    case ModType::Lua:       return "lua";
    case ModType::ModLoader: return "modloader";
    case ModType::Data:      return "data";
    case ModType::Other:     return "other";
  }
  return "other";
}

std::vector<ModType> modTypes(const std::vector<std::string>& relPaths,
                              bool viaModloader)
{
  bool cleo = false, asi = false, lua = false, data = false;
  bool modloader = viaModloader;

  for (const std::string& rel : relPaths) {
    // Path components: the first one tells us about modloader/ staging, the
    // last is the file name we classify by extension.
    const size_t slash = rel.find_last_of("/\\");
    const std::string file = slash == std::string::npos ? rel : rel.substr(slash + 1);
    const std::string ext  = extOf(file);

    const size_t firstSlash = rel.find_first_of("/\\");
    if (firstSlash != std::string::npos &&
        lower(rel.substr(0, firstSlash)) == "modloader")
      modloader = true;

    if (ext == ".cs" || ext == ".cs3" || ext == ".cs4" || ext == ".cleo")
      cleo = true;
    else if (ext == ".asi")
      asi = true;
    else if (ext == ".lua" || ext == ".luac")
      lua = true;
    else if (isMergeableData(file) || ext == ".ide" || ext == ".ipl" ||
             ext == ".dat" || ext == ".cfg")
      data = true;
    // Anything else (models, textures, audio, fonts, ...) needs no flag: it
    // only shows up as "Other", and only when nothing more specific applies.
  }

  std::vector<ModType> out;
  if (cleo)
    out.push_back(ModType::Cleo);
  if (asi)
    out.push_back(ModType::Asi);
  if (lua)
    out.push_back(ModType::Lua);
  if (modloader)
    out.push_back(ModType::ModLoader);
  if (data)
    out.push_back(ModType::Data);
  // "Other" only when nothing more specific applies: most mods carry plain
  // files (models, textures, readmes) alongside their scripts, and tagging
  // every one of them "Other" as well would turn the column into noise.
  if (out.empty())
    out.push_back(ModType::Other);
  return out;
}

namespace {

// Flat, name/symbol-keyed data files (one record per key).
const std::set<std::string>& keyedDataFiles()
{
  static const std::set<std::string> set = {
      "handling.cfg", "weapon.dat",  "melee.dat",
      "ar_stats.dat", "pedstats.dat", "carmods.dat"};
  return set;
}

// Additive load-list files (lines are appended, never keyed).
const std::set<std::string>& additiveDataFiles()
{
  static const std::set<std::string> set = {"gta.dat", "default.dat"};
  return set;
}

// Collapse runs of whitespace and lowercase, for case/space-insensitive compare.
std::string normalizeLine(const std::string& line)
{
  std::string s;
  bool pendingSpace = false;
  for (char c : line) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      pendingSpace = !s.empty();
    } else {
      if (pendingSpace)
        s += ' ';
      pendingSpace = false;
      s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return s;
}

}  // namespace

bool isMergeableData(const std::string& filename)
{
  const std::string f = lower(filename);
  return extOf(f) == ".ide" || keyedDataFiles().count(f) > 0 ||
         additiveDataFiles().count(f) > 0;
}

bool isCommentOrBlank(const std::string& line)
{
  for (char c : line) {
    if (std::isspace(static_cast<unsigned char>(c)))
      continue;
    return c == ';' || c == '#';
  }
  return true;
}

std::string lineKey(const std::string& line)
{
  const std::size_t n = line.size();
  auto skipWs = [&](std::size_t p) {
    while (p < n && std::isspace(static_cast<unsigned char>(line[p])))
      ++p;
    return p;
  };
  auto tokEnd = [&](std::size_t p) {
    while (p < n && !std::isspace(static_cast<unsigned char>(line[p])))
      ++p;
    return p;
  };
  const std::size_t a = skipWs(0);
  if (a >= n)
    return "";
  const std::size_t b = tokEnd(a);
  std::string key     = line.substr(a, b - a);
  static const std::string markers = "%!$^@";
  if (key.size() == 1 && markers.find(key[0]) != std::string::npos) {
    const std::size_t c = skipWs(b);
    const std::size_t d = tokEnd(c);
    if (d > c)
      key += line.substr(c, d - c);
  }
  return lower(key);
}

std::string mergeKeyedLines(const std::string& base,
                            const std::vector<std::string>& providerTexts)
{
  std::vector<std::string> result = splitLines(base);
  std::map<std::string, std::size_t> keyIdx;
  for (std::size_t i = 0; i < result.size(); ++i) {
    if (isCommentOrBlank(result[i]))
      continue;
    const std::string k = lineKey(result[i]);
    if (!k.empty())
      keyIdx.emplace(k, i);
  }
  for (const std::string& text : providerTexts) {
    for (const std::string& line : splitLines(text)) {
      if (isCommentOrBlank(line))
        continue;
      const std::string k = lineKey(line);
      if (k.empty())
        continue;
      auto it = keyIdx.find(k);
      if (it != keyIdx.end())
        result[it->second] = line;
      else {
        keyIdx.emplace(k, result.size());
        result.push_back(line);
      }
    }
  }
  std::string out;
  for (const std::string& l : result) {
    out += l;
    out += "\r\n";
  }
  return out;
}

std::string mergeAdditive(const std::string& base,
                          const std::vector<std::string>& providerTexts)
{
  std::vector<std::string> result = splitLines(base);
  std::set<std::string> seen;
  for (const std::string& l : result)
    if (!isCommentOrBlank(l))
      seen.insert(normalizeLine(l));

  for (const std::string& text : providerTexts) {
    for (const std::string& line : splitLines(text)) {
      if (isCommentOrBlank(line))
        continue;
      const std::string k = normalizeLine(line);
      if (seen.insert(k).second)  // newly inserted -> not present before
        result.push_back(line);
    }
  }

  std::string out;
  for (const std::string& l : result) {
    out += l;
    out += "\r\n";
  }
  return out;
}

namespace {

std::string trim(const std::string& s)
{
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return s.substr(a, b - a);
}

bool isIdeSectionKeyword(const std::string& word)
{
  static const std::set<std::string> kw = {"objs", "tobj", "anim", "cars",
                                            "peds", "weap", "hier", "txdp",
                                            "2dfx", "path"};
  return kw.count(word) > 0;
}

// First comma-separated field of a record, trimmed and lowercased; "" for
// comment/blank lines (which carry no merge key).
std::string ideKey(const std::string& line)
{
  if (isCommentOrBlank(line))
    return "";
  const std::size_t comma = line.find(',');
  return lower(trim(comma == std::string::npos ? line : line.substr(0, comma)));
}

struct IdeBlock
{
  bool isSection = false;
  std::string raw;                   // when !isSection: comment/blank/preamble
  std::string name;                  // when isSection: section keyword
  std::vector<std::string> records;  // when isSection: record lines
};

std::vector<IdeBlock> parseIde(const std::string& text)
{
  std::vector<IdeBlock> blocks;
  bool inSection = false;
  for (const std::string& line : splitLines(text)) {
    const std::string t = lower(trim(line));
    if (!inSection) {
      if (isIdeSectionKeyword(t)) {
        IdeBlock b;
        b.isSection = true;
        b.name      = t;
        blocks.push_back(std::move(b));
        inSection = true;
      } else {
        IdeBlock b;
        b.raw = line;
        blocks.push_back(std::move(b));
      }
    } else {
      if (t == "end") {
        inSection = false;
      } else {
        blocks.back().records.push_back(line);
      }
    }
  }
  return blocks;
}

}  // namespace

std::string mergeIde(const std::string& base,
                     const std::vector<std::string>& providerTexts)
{
  std::vector<IdeBlock> result = parseIde(base);
  std::map<std::string, std::size_t> sectionIdx;
  for (std::size_t i = 0; i < result.size(); ++i)
    if (result[i].isSection)
      sectionIdx.emplace(result[i].name, i);

  for (const std::string& text : providerTexts) {
    for (IdeBlock& pb : parseIde(text)) {
      if (!pb.isSection)
        continue;  // drop provider comments/preamble
      auto found = sectionIdx.find(pb.name);
      if (found == sectionIdx.end()) {
        result.push_back(pb);
        sectionIdx.emplace(pb.name, result.size() - 1);
        continue;
      }
      IdeBlock& bb = result[found->second];
      std::map<std::string, std::size_t> km;
      for (std::size_t j = 0; j < bb.records.size(); ++j) {
        const std::string k = ideKey(bb.records[j]);
        if (!k.empty())
          km.emplace(k, j);
      }
      for (const std::string& rec : pb.records) {
        const std::string k = ideKey(rec);
        if (k.empty())
          continue;
        auto it = km.find(k);
        if (it != km.end())
          bb.records[it->second] = rec;  // replace
        else {
          km.emplace(k, bb.records.size());
          bb.records.push_back(rec);  // append within section
        }
      }
    }
  }

  std::string out;
  for (const IdeBlock& b : result) {
    if (!b.isSection) {
      out += b.raw;
      out += "\r\n";
    } else {
      out += b.name;
      out += "\r\n";
      for (const std::string& r : b.records) {
        out += r;
        out += "\r\n";
      }
      out += "end\r\n";
    }
  }
  return out;
}

std::string mergeData(const std::string& filename, const std::string& base,
                      const std::vector<std::string>& providerTexts)
{
  const std::string f = lower(filename);
  if (extOf(f) == ".ide")
    return mergeIde(base, providerTexts);
  if (additiveDataFiles().count(f) > 0)
    return mergeAdditive(base, providerTexts);
  if (keyedDataFiles().count(f) > 0)
    return mergeKeyedLines(base, providerTexts);
  // Not mergeable: last provider wins wholesale.
  if (!providerTexts.empty())
    return providerTexts.back();
  return base;
}

}  // namespace gtamm::rules
