#include "SannyBuilder.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace gtamm::sanny {

namespace {

std::string readText(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool writeText(const fs::path& p, const std::string& text)
{
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

// Splits on '\n' but keeps any '\r' at the end of a line, so rejoining with
// '\n' reproduces the file's original CRLF byte for byte.
std::vector<std::string> splitLines(const std::string& text)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char c : text) {
    if (c == '\n') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);  // trailing fragment (empty if the file ended in '\n')
  return out;
}

std::string joinLines(const std::vector<std::string>& lines)
{
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size())
      out += '\n';
  }
  return out;
}

std::string trim(const std::string& s)
{
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Windows-style path, no trailing separator: that is what Sanny Builder writes
// itself, and "@game:\data\gta.dat" is built by plain concatenation.
std::string nativePath(const fs::path& p)
{
  std::string s = p.string();
  for (char& c : s)
    if (c == '/')
      c = '\\';
  while (s.size() > 3 && (s.back() == '\\'))
    s.pop_back();
  return s;
}

std::string xmlEscape(const std::string& s)
{
  std::string out;
  for (const char c : s) {
    if (c == '&')
      out += "&amp;";
    else if (c == '<')
      out += "&lt;";
    else if (c == '>')
      out += "&gt;";
    else
      out += c;
  }
  return out;
}

bool isSectionHeader(const std::string& line)
{
  const std::string t = trim(line);
  return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}

// "GamePath = x" -> true; "GamePathOld=x" -> false.
bool isKey(const std::string& line, const std::string& key)
{
  const std::string t = trim(line);
  if (lower(t).rfind(lower(key), 0) != 0)
    return false;
  const std::string rest = trim(t.substr(key.size()));
  return !rest.empty() && rest.front() == '=';
}

fs::path modeXml(const fs::path& sbDir, const std::string& mode)
{
  return sbDir / "data" / mode / "mode.xml";
}

}  // namespace

fs::path settingsIni(const fs::path& sbDir)
{
  return sbDir / "data" / "settings.ini";
}

std::string currentEditMode(const fs::path& sbDir)
{
  for (const std::string& line : splitLines(readText(settingsIni(sbDir)))) {
    if (!isKey(line, "EditMode"))
      continue;
    const std::string t = trim(line);
    const std::string v = trim(t.substr(t.find('=') + 1));
    if (!v.empty())
      return v;
  }
  return "sa";
}

std::string modeGameId(const fs::path& sbDir, const std::string& mode)
{
  const std::string xml = readText(modeXml(sbDir, mode));
  // The attribute only ever appears on the <mode> tag, so a plain search for
  // game="..." is enough and beats pulling in an XML parser for one value.
  const size_t at = xml.find("game=\"");
  if (at == std::string::npos)
    return mode;
  const size_t start = at + 6;
  const size_t end   = xml.find('"', start);
  if (end == std::string::npos || end == start)
    return mode;
  return xml.substr(start, end - start);
}

bool setGamePath(const fs::path& sbDir, const std::string& gameId,
                 const fs::path& gameDir)
{
  const fs::path ini = settingsIni(sbDir);
  std::error_code ec;
  if (!fs::exists(ini, ec))
    return false;

  std::vector<std::string> lines = splitLines(readText(ini));
  const std::string wanted = "GamePath=" + nativePath(gameDir);
  const std::string header = "[" + lower(gameId) + "]";

  // Find the game's own section; GamePath entries elsewhere belong to other
  // games (SB keeps one path per game, shared by that game's edit modes).
  size_t secStart = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (isSectionHeader(lines[i]) && lower(trim(lines[i])) == header) {
      secStart = i;
      break;
    }
  }

  if (secStart == lines.size()) {
    // No section yet: append one. Keep a blank line out of it if the file
    // already ends with an empty trailing fragment.
    if (!lines.empty() && trim(lines.back()).empty())
      lines.pop_back();
    lines.push_back("[" + gameId + "]");
    lines.push_back(wanted);
    lines.push_back("");
    return writeText(ini, joinLines(lines));
  }

  size_t secEnd = lines.size();
  for (size_t i = secStart + 1; i < lines.size(); ++i) {
    if (isSectionHeader(lines[i])) {
      secEnd = i;
      break;
    }
  }
  for (size_t i = secStart + 1; i < secEnd; ++i) {
    if (isKey(lines[i], "GamePath")) {
      lines[i] = wanted;
      return writeText(ini, joinLines(lines));
    }
  }
  lines.insert(lines.begin() + static_cast<long long>(secStart) + 1, wanted);
  return writeText(ini, joinLines(lines));
}

bool setCleoOutputDir(const fs::path& sbDir, const std::string& mode,
                      const fs::path& cleoDir)
{
  const fs::path xmlPath = modeXml(sbDir, mode);
  std::error_code ec;
  if (!fs::exists(xmlPath, ec))
    return false;

  std::string xml = readText(xmlPath);
  const std::string value =
      cleoDir.empty() ? std::string("@game:\\CLEO") : xmlEscape(nativePath(cleoDir));

  const std::string openTag = "<copy-directory type=\"cleo\">";
  const std::string closeTag = "</copy-directory>";
  const size_t at = xml.find(openTag);
  if (at != std::string::npos) {
    const size_t valStart = at + openTag.size();
    const size_t valEnd   = xml.find(closeTag, valStart);
    if (valEnd == std::string::npos)
      return false;
    if (xml.substr(valStart, valEnd - valStart) == value)
      return true;  // already pointing there -- don't rewrite the file
    xml = xml.substr(0, valStart) + value + xml.substr(valEnd);
    return writeText(xmlPath, xml);
  }

  // Mode without a CLEO output at all (some editions have none): add one just
  // before the closing tag rather than guessing where it "belongs".
  const size_t endMode = xml.rfind("</mode>");
  if (endMode == std::string::npos)
    return false;
  const std::string ins =
      "    " + openTag + value + closeTag + "\n";
  xml = xml.substr(0, endMode) + ins + xml.substr(endMode);
  return writeText(xmlPath, xml);
}

}  // namespace gtamm::sanny
