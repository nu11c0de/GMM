#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// GTA SA audio SFX banks. Sounds live inside the archives audio/SFX/<PAK>
// (GENRL, FEET, PAIN_A, SCRIPT, SPC_*), described by audio/CONFIG/BankLkup.dat.
// SAAT-style sound mods ship folders "Bank_<n>/sound_<m>.wav"; to take effect a
// sound must be injected back into its pak (the game ignores loose folders).
//
// Layout (validated against vanilla 1.0):
//   PakFiles.dat : 52-byte ASCII pak names, in pak-index order.
//   BankLkup.dat : 12-byte entries {u8 pak; u8[3] pad; u32 offset; u32 audioLen}
//                  one per bank, grouped by pak in order; audioLen = bankLen-4804.
//   A pak file   : contiguous concatenation of its banks from offset 0.
//   A bank       : 4804-byte header + audioLen bytes of 16-bit-mono PCM.
//   Bank header  : u16 numSounds; u16 pad; then 400 × SoundEntry.
//   SoundEntry   : {u32 bufOffset (rel. to audio start); u32 loopOffset (-1 = none,
//                  sound-relative); u16 sampleRate; u16 extra}.
namespace gtamm::sfx {

inline constexpr std::uint32_t kBankHeader = 4804;  // 4 + 400*12
inline constexpr int kMaxSounds = 400;

// One BankLkup.dat entry (pak index + location within that pak file).
struct BankRef
{
  int pak = 0;
  std::uint32_t offset   = 0;  // byte offset of the bank within its pak file
  std::uint32_t audioLen = 0;  // PCM bytes after the 4804-byte header
};

// Pak names from audio/CONFIG/PakFiles.dat (index = pak number).
std::vector<std::string> readPakNames(const std::filesystem::path& audioDir);

// Read/patch the global BankLkup table. `patchBankLkup` rewrites only the
// offset+audioLen of each entry from `banks`, preserving the pak/pad bytes.
std::vector<BankRef> readBankLkup(const std::filesystem::path& bankLkupPath);
void patchBankLkup(const std::filesystem::path& srcBankLkup,
                   const std::filesystem::path& outBankLkup,
                   const std::vector<BankRef>& banks);

// Parse a SAAT bank-dir / sound-file name. Returns the embedded number (e.g.
// "Bank_046" -> 46, "sound_001.wav" -> 1) or -1 if it does not match.
int parseBankDir(const std::string& name);
int parseSoundFile(const std::string& name);

// Rebuild one pak from `pristinePak` into `outPak`, replacing sounds per `edits`
// (local bank index -> local sound index -> wav file). Updates the offset/audioLen
// of this pak's entries in `banks` (the full global table) in place. Indices are
// 0-based local indices within the pak. Sounds/banks out of range are skipped.
// A replaced sound updates only its PCM and sample rate; its loop offset and
// headroom are kept from the original (matching Mod Loader's bank loader).
void rebuildPak(const std::filesystem::path& pristinePak,
                const std::filesystem::path& outPak,
                int pakIndex,
                std::vector<BankRef>& banks,
                const std::map<int, std::map<int, std::filesystem::path>>& edits);

}  // namespace gtamm::sfx
