#include "Sfx.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace gtamm::sfx {

namespace {

std::vector<unsigned char> readAll(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read: " + p.string());
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

std::uint16_t rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
std::uint32_t rd32(const unsigned char* p)
{
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
void wr16(std::vector<unsigned char>& b, std::size_t o, std::uint16_t v)
{
  b[o] = v & 0xFF;
  b[o + 1] = (v >> 8) & 0xFF;
}
void wr32(std::vector<unsigned char>& b, std::size_t o, std::uint32_t v)
{
  b[o] = v & 0xFF;
  b[o + 1] = (v >> 8) & 0xFF;
  b[o + 2] = (v >> 16) & 0xFF;
  b[o + 3] = (v >> 24) & 0xFF;
}

// A 16-bit mono PCM stream extracted from a WAV file.
struct Wav
{
  std::vector<unsigned char> pcm;
  std::uint16_t sampleRate = 0;
};

Wav readWavMono16(const fs::path& path)
{
  const auto d = readAll(path);
  if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 ||
      std::memcmp(d.data() + 8, "WAVE", 4) != 0)
    throw std::runtime_error("not a WAV file: " + path.string());

  Wav w;
  std::uint16_t channels = 0, bits = 0, fmt = 0;
  bool haveFmt = false, haveData = false;
  std::size_t pos = 12;
  while (pos + 8 <= d.size()) {
    const char* id = reinterpret_cast<const char*>(d.data() + pos);
    const std::uint32_t sz = rd32(d.data() + pos + 4);
    const std::size_t body = pos + 8;
    if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= d.size()) {
      fmt      = rd16(d.data() + body);
      channels = rd16(d.data() + body + 2);
      w.sampleRate = static_cast<std::uint16_t>(rd32(d.data() + body + 4));
      bits     = rd16(d.data() + body + 14);
      haveFmt  = true;
    } else if (std::memcmp(id, "data", 4) == 0) {
      const std::size_t n = std::min<std::size_t>(sz, d.size() - body);
      w.pcm.assign(d.begin() + body, d.begin() + body + n);
      haveData = true;
    }
    pos = body + sz + (sz & 1);  // chunks are word-aligned
  }
  if (!haveFmt || !haveData)
    throw std::runtime_error("malformed WAV (missing fmt/data): " + path.string());
  if (fmt != 1 || channels != 1 || bits != 16)
    throw std::runtime_error("WAV must be PCM 16-bit mono: " + path.string());
  return w;
}

}  // namespace

std::vector<std::string> readPakNames(const fs::path& audioDir)
{
  const auto d = readAll(audioDir / "CONFIG" / "PakFiles.dat");
  std::vector<std::string> names;
  for (std::size_t i = 0; i + 52 <= d.size(); i += 52) {
    std::string nm;
    for (std::size_t k = 0; k < 52 && d[i + k] != 0; ++k)
      nm += static_cast<char>(d[i + k]);
    if (!nm.empty())
      names.push_back(nm);
  }
  return names;
}

std::vector<BankRef> readBankLkup(const fs::path& bankLkupPath)
{
  const auto d = readAll(bankLkupPath);
  std::vector<BankRef> banks;
  for (std::size_t i = 0; i + 12 <= d.size(); i += 12) {
    BankRef b;
    b.pak      = d[i];
    b.offset   = rd32(d.data() + i + 4);
    b.audioLen = rd32(d.data() + i + 8);
    banks.push_back(b);
  }
  return banks;
}

void patchBankLkup(const fs::path& srcBankLkup, const fs::path& outBankLkup,
                   const std::vector<BankRef>& banks)
{
  auto d = readAll(srcBankLkup);  // preserve pak/pad bytes, patch offset+audioLen
  for (std::size_t k = 0; k < banks.size() && (k * 12 + 12) <= d.size(); ++k) {
    wr32(d, k * 12 + 4, banks[k].offset);
    wr32(d, k * 12 + 8, banks[k].audioLen);
  }
  std::ofstream out(outBankLkup, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot write: " + outBankLkup.string());
  out.write(reinterpret_cast<const char*>(d.data()),
            static_cast<std::streamsize>(d.size()));
}

namespace {
// Parse "<prefix>_<number>" (case-insensitive prefix); returns number or -1.
int parseNumbered(const std::string& name, const char* prefix)
{
  std::string lower;
  for (char c : name)
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const std::string pre = prefix;  // already lowercase
  if (lower.rfind(pre, 0) != 0)
    return -1;
  std::size_t i = pre.size();
  if (i >= lower.size() || !std::isdigit(static_cast<unsigned char>(lower[i])))
    return -1;
  int n = 0;
  for (; i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i])); ++i)
    n = n * 10 + (lower[i] - '0');
  return n;
}
}  // namespace

int parseBankDir(const std::string& name) { return parseNumbered(name, "bank_"); }

int parseSoundFile(const std::string& name)
{
  // strip extension first
  const auto dot = name.find_last_of('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  return parseNumbered(stem, "sound_");
}

void rebuildPak(const fs::path& pristinePak, const fs::path& outPak, int pakIndex,
                std::vector<BankRef>& banks,
                const std::map<int, std::map<int, fs::path>>& edits)
{
  // Global indices of this pak's banks, in order = local bank order.
  std::vector<std::size_t> idx;
  for (std::size_t i = 0; i < banks.size(); ++i)
    if (banks[i].pak == pakIndex)
      idx.push_back(i);

  std::ifstream in(pristinePak, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read pak: " + pristinePak.string());
  std::ofstream out(outPak, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot write pak: " + outPak.string());

  std::uint32_t running = 0;  // running offset within the rebuilt pak
  std::vector<char> copyBuf(1 << 20);

  for (std::size_t local = 0; local < idx.size(); ++local) {
    const std::size_t gi = idx[local];
    const std::uint32_t origOff = banks[gi].offset;
    const std::uint32_t origLen = kBankHeader + banks[gi].audioLen;

    const auto editIt = edits.find(static_cast<int>(local));
    if (editIt == edits.end()) {
      // Unmodified: stream the original bank bytes straight through.
      in.seekg(origOff);
      std::uint32_t left = origLen;
      while (left > 0) {
        const std::streamsize n =
            std::min<std::uint32_t>(left, static_cast<std::uint32_t>(copyBuf.size()));
        in.read(copyBuf.data(), n);
        if (in.gcount() != n)
          throw std::runtime_error("short read in pak: " + pristinePak.string());
        out.write(copyBuf.data(), n);
        left -= static_cast<std::uint32_t>(n);
      }
      banks[gi].offset = running;
      running += origLen;
      continue;
    }

    // Modified bank: read header + audio, replace the listed sounds, repack.
    std::vector<unsigned char> bank(origLen);
    in.seekg(origOff);
    in.read(reinterpret_cast<char*>(bank.data()), origLen);
    if (static_cast<std::uint32_t>(in.gcount()) != origLen)
      throw std::runtime_error("short read in pak: " + pristinePak.string());

    const int numSounds = rd16(bank.data());
    const unsigned char* audio = bank.data() + kBankHeader;
    const std::uint32_t audioLen = banks[gi].audioLen;

    // Split the audio region into per-sound PCM blobs.
    struct Snd { std::vector<unsigned char> pcm; std::uint32_t loop; std::uint16_t rate, extra; };
    std::vector<Snd> sounds(numSounds);
    for (int k = 0; k < numSounds; ++k) {
      const std::uint32_t off  = rd32(bank.data() + 4 + k * 12);
      const std::uint32_t next =
          (k + 1 < numSounds) ? rd32(bank.data() + 4 + (k + 1) * 12) : audioLen;
      const std::uint32_t len = next >= off ? next - off : 0;
      sounds[k].pcm.assign(audio + off, audio + off + std::min(len, audioLen - off));
      sounds[k].loop  = rd32(bank.data() + 4 + k * 12 + 4);
      sounds[k].rate  = rd16(bank.data() + 4 + k * 12 + 8);
      sounds[k].extra = rd16(bank.data() + 4 + k * 12 + 10);
    }

    // Apply replacements (in-range only).
    for (const auto& [soundIdx, wavPath] : editIt->second) {
      if (soundIdx < 0 || soundIdx >= numSounds)
        continue;
      const Wav w = readWavMono16(wavPath);
      sounds[soundIdx].pcm  = w.pcm;
      sounds[soundIdx].rate = w.sampleRate;
      // Match Mod Loader's CAEBankInfo::ProcessVirtualBank: when a sound is
      // replaced only the sample rate is updated; the loop offset and headroom
      // are kept from the original slot.
    }

    // Repack: fresh 4804 header + concatenated PCM.
    std::vector<unsigned char> outBank(kBankHeader, 0);
    wr16(outBank, 0, static_cast<std::uint16_t>(numSounds));
    std::vector<unsigned char> newAudio;
    for (int k = 0; k < numSounds; ++k) {
      const std::uint32_t off = static_cast<std::uint32_t>(newAudio.size());
      wr32(outBank, 4 + k * 12, off);
      wr32(outBank, 4 + k * 12 + 4, sounds[k].loop);
      wr16(outBank, 4 + k * 12 + 8, sounds[k].rate);
      wr16(outBank, 4 + k * 12 + 10, sounds[k].extra);
      newAudio.insert(newAudio.end(), sounds[k].pcm.begin(), sounds[k].pcm.end());
    }
    out.write(reinterpret_cast<const char*>(outBank.data()), kBankHeader);
    out.write(reinterpret_cast<const char*>(newAudio.data()),
              static_cast<std::streamsize>(newAudio.size()));

    banks[gi].offset   = running;
    banks[gi].audioLen = static_cast<std::uint32_t>(newAudio.size());
    running += kBankHeader + static_cast<std::uint32_t>(newAudio.size());
  }
}

}  // namespace gtamm::sfx
