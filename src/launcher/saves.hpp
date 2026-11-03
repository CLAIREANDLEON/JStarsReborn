// JStarsReborn launcher - save management: import a save, unlock every character, back up and restore.
//
// The game's save is <user dir>/savedata/1/CUSA01593/SYSTEM-DATA/SAVE-DATA.DAT (200000 bytes, unencrypted on this port).
// Checksum and character offsets: tools/save_editor (CLAIREANDLEON, PR #3), confirmed on saves the game wrote in our test runs.
// Every change is preceded by a backup of the whole SYSTEM-DATA folder; nothing here runs while the game is running (the
// launcher checks that before calling).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace saves {
namespace fs = std::filesystem;

constexpr std::size_t kSaveSize = 200000;
constexpr int kCharacters = 52;        // 0x5C40: 39 playable + 13 support, as the save editor sets them
constexpr int kOwned = 46;             // 0x5C6C: every battle character (confirmed in game by the save editor's author)
constexpr int kSupport = 13;           // 0x5C48 and 0x269B8
constexpr std::uint32_t kJPoints = 999999;

struct Info {
    bool valid = false;                // right size and checksum
    std::string problem;               // why not, in words for the player
    std::uint32_t jpoints = 0;         // current J-Points
    int owned = 0;                     // owned-character bits set (of kOwned)
    bool all_unlocked = false;
};
std::uint64_t checksum_sum(const std::vector<std::uint8_t>& b);   // sum of LE u16 words from 0x8
bool checksum_ok(const std::vector<std::uint8_t>& b);
void fix_checksum(std::vector<std::uint8_t>& b);
Info inspect(const std::vector<std::uint8_t>& b);
// Sets every character unlocked and owned and J-Points to kJPoints (keeps a higher total), then fixes the checksum.
void unlock_all(std::vector<std::uint8_t>& b);

fs::path save_dir(const fs::path& user_dir);   // .../SYSTEM-DATA
fs::path save_file(const fs::path& user_dir);  // .../SYSTEM-DATA/SAVE-DATA.DAT

// Finds a save to import: the file itself, or SAVE-DATA.DAT inside a folder (searched a few levels deep, e.g. a shadPS4
// user folder or a decrypted PS4 export). Empty path and *why set when there is none (e.g. an encrypted PS4 save).
fs::path find_importable(const fs::path& chosen, std::string* why);

struct Result { bool ok = false; std::string message; fs::path backup; };
Result backup(const fs::path& user_dir, const fs::path& backups_root, const std::string& label);
Result import_save(const fs::path& source, const fs::path& user_dir, const fs::path& backups_root);
Result unlock_all_characters(const fs::path& user_dir, const fs::path& backups_root);
std::vector<fs::path> list_backups(const fs::path& backups_root);   // newest first
Result restore(const fs::path& backup_folder, const fs::path& user_dir, const fs::path& backups_root);

}  // namespace saves
