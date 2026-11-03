// JStarsReborn launcher - save management (saves.hpp).
#include "saves.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace saves {

static bool read_all(const fs::path& p, std::vector<std::uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
static bool write_all(const fs::path& p, const std::vector<std::uint8_t>& b) {
    // Written next to the target, then renamed over it: a failure never leaves half a save.
    const fs::path tmp = p.string() + ".jstars-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        if (!f) return false;
    }
    std::error_code ec;
#ifdef _WIN32
    // A read-only save (copied from a USB stick, a ZIP or a CD) cannot be replaced: clear that attribute first.
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p.wstring().c_str(), attrs & ~DWORD(FILE_ATTRIBUTE_READONLY));
    if (!MoveFileExW(tmp.wstring().c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { fs::remove(tmp, ec); return false; }
#else
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
#endif
    return true;
}
static std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return std::uint32_t(b[o]) | std::uint32_t(b[o + 1]) << 8 | std::uint32_t(b[o + 2]) << 16 | std::uint32_t(b[o + 3]) << 24;
}
static void wr32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = std::uint8_t(v >> (8 * i));
}
static void set_bits(std::vector<std::uint8_t>& b, std::size_t o, int n) {
    for (int i = 0; i < n; ++i) b[o + i / 8] |= std::uint8_t(1u << (i % 8));
}
static int count_bits(const std::vector<std::uint8_t>& b, std::size_t o, int n) {
    int c = 0;
    for (int i = 0; i < n; ++i) c += (b[o + i / 8] >> (i % 8)) & 1;
    return c;
}

std::uint64_t checksum_sum(const std::vector<std::uint8_t>& b) {
    std::uint64_t s = 0;
    for (std::size_t i = 8; i + 1 < b.size(); i += 2) s += std::uint64_t(b[i]) | std::uint64_t(b[i + 1]) << 8;
    return s;
}
bool checksum_ok(const std::vector<std::uint8_t>& b) {
    if (b.size() < 16) return false;
    const std::uint64_t s = checksum_sum(b);
    return rd32(b, 0) == std::uint32_t(s >> 8) && rd32(b, 4) == std::uint32_t(s & 0xFF);
}
void fix_checksum(std::vector<std::uint8_t>& b) {
    const std::uint64_t s = checksum_sum(b);
    wr32(b, 0, std::uint32_t(s >> 8));
    wr32(b, 4, std::uint32_t(s & 0xFF));
}
Info inspect(const std::vector<std::uint8_t>& b) {
    Info i;
    if (b.size() != kSaveSize) { i.problem = "this is not a J-Stars Victory VS+ save (wrong size: " + std::to_string(b.size()) + " bytes, expected 200000)"; return i; }
    if (!checksum_ok(b)) { i.problem = "the save's checksum does not match: it is damaged, still encrypted, or edited without fixing the checksum"; return i; }
    i.valid = true;
    i.jpoints = rd32(b, 0x5C5C);
    i.owned = count_bits(b, 0x5C6C, kOwned);
    i.all_unlocked = i.owned == kOwned && count_bits(b, 0x5C40, kCharacters) == kCharacters;
    return i;
}
void unlock_all(std::vector<std::uint8_t>& b) {
    set_bits(b, 0x5C40, kCharacters);
    set_bits(b, 0x5C48, kSupport);
    set_bits(b, 0x269B8, kSupport);
    set_bits(b, 0x5C6C, kOwned);
    wr32(b, 0x5C5C, std::max(rd32(b, 0x5C5C), kJPoints));
    wr32(b, 0x5C58, std::max(rd32(b, 0x5C58), kJPoints));
    fix_checksum(b);
}

fs::path save_dir(const fs::path& user_dir) { return user_dir / "savedata" / "1" / "CUSA01593" / "SYSTEM-DATA"; }
fs::path save_file(const fs::path& user_dir) { return save_dir(user_dir) / "SAVE-DATA.DAT"; }

static std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

fs::path find_importable(const fs::path& chosen, std::string* why) {
    std::error_code ec;
    if (fs::is_regular_file(chosen, ec)) {
        if (fs::file_size(chosen, ec) == kSaveSize) return chosen;
        if (why) *why = "the chosen file is not a J-Stars save (SAVE-DATA.DAT is 200000 bytes)";
        return {};
    }
    if (!fs::is_directory(chosen, ec)) { if (why) *why = "nothing was found at the chosen place"; return {}; }
    std::vector<fs::path> found;
    bool encrypted_hint = false;
    fs::recursive_directory_iterator it(chosen, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string name = lower(it->path().filename().string());
        if (name == "save-data.dat" && fs::file_size(it->path(), ec) == kSaveSize) found.push_back(it->path());
        // PS4 encrypted saves: sdimg_* (internal storage) or SYSTEM-DATA + SYSTEM-DATA.bin (USB export)
        if (name.rfind("sdimg_", 0) == 0 || name == "system-data.bin" || name == "system-data") encrypted_hint = true;
    }
    if (found.empty()) {
        if (why) *why = encrypted_hint ? "this looks like an encrypted PS4 save. Decrypt it first (for example with Apollo Save Tool on a PS4) and import the decrypted folder"
                                       : "no SAVE-DATA.DAT of 200000 bytes was found in that folder";
        return {};
    }
    // Prefer this game's title folder, then the newest file.
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
        const bool ta = a.string().find("CUSA01593") != std::string::npos, tb = b.string().find("CUSA01593") != std::string::npos;
        if (ta != tb) return ta;
        std::error_code e;
        return fs::last_write_time(a, e) > fs::last_write_time(b, e);
    });
    return found.front();
}

static std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 1000;
    char m[8];
    std::snprintf(m, sizeof m, "-%03d", int(ms));
    return std::string(b) + m;  // milliseconds: two backups in one second still sort by time
}

Result backup(const fs::path& user_dir, const fs::path& backups_root, const std::string& label) {
    Result r;
    std::error_code ec;
    const fs::path dir = save_dir(user_dir);
    if (!fs::exists(save_file(user_dir), ec)) { r.ok = true; r.message = "no save to back up"; return r; }
    fs::path dest = backups_root / (stamp() + "-" + label);
    for (int n = 2; fs::exists(dest, ec); ++n) dest = backups_root / (stamp() + "-" + label + "-" + std::to_string(n));
    fs::create_directories(dest, ec);
    if (!ec) fs::copy(dir, dest / "SYSTEM-DATA", fs::copy_options::recursive, ec);
    if (ec) { r.message = "the backup could not be written (" + ec.message() + "); nothing was changed"; return r; }
    // Copies keep a read-only attribute; backups are ordinary files the player can delete or restore.
    for (fs::recursive_directory_iterator it(dest, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    ec.clear();
    r.ok = true;
    r.backup = dest;
    r.message = "backed up to " + dest.filename().string();
    return r;
}

Result import_save(const fs::path& source, const fs::path& user_dir, const fs::path& backups_root) {
    Result r;
    std::error_code ec;
    std::vector<std::uint8_t> b;
    if (!read_all(source, &b)) { r.message = "the chosen save could not be read"; return r; }
    const Info in = inspect(b);
    if (!in.valid) { r.message = "Not imported: " + in.problem + "."; return r; }
    if (!fs::exists(save_dir(user_dir) / "sce_sys" / "param.bin", ec)) {
        r.message = "Start the game once first and let it create its save (choose Yes when it asks), close it, then import.";
        return r;
    }
    if (fs::equivalent(source, save_file(user_dir), ec)) { r.message = "That is already the save in use."; return r; }
    Result bk = backup(user_dir, backups_root, "before-import");
    if (!bk.ok) return bk;
    if (!write_all(save_file(user_dir), b)) { r.message = "the save could not be written; your previous save is unchanged"; return r; }
    r.ok = true;
    r.backup = bk.backup;
    r.message = "Save imported (" + std::to_string(in.jpoints) + " J-Points, " + std::to_string(in.owned) + " of " + std::to_string(kOwned) +
                " characters owned). Your previous save was " + bk.message + ".";
    return r;
}

Result unlock_all_characters(const fs::path& user_dir, const fs::path& backups_root) {
    Result r;
    std::vector<std::uint8_t> b;
    if (!read_all(save_file(user_dir), &b)) { r.message = "No save yet: start the game once and let it create its save, close it, then try again."; return r; }
    const Info in = inspect(b);
    if (!in.valid) { r.message = "Not changed: " + in.problem + "."; return r; }
    if (in.all_unlocked) { r.ok = true; r.message = "Every character is already unlocked."; return r; }
    Result bk = backup(user_dir, backups_root, "before-unlock");
    if (!bk.ok) return bk;
    unlock_all(b);
    if (!write_all(save_file(user_dir), b)) { r.message = "the save could not be written; it is unchanged"; return r; }
    r.ok = true;
    r.backup = bk.backup;
    r.message = "Every character is unlocked and owned, and J-Points are 999,999. To undo, restore the backup " + bk.backup.filename().string() + ".";
    return r;
}

std::vector<fs::path> list_backups(const fs::path& backups_root) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(backups_root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && fs::exists(it->path() / "SYSTEM-DATA" / "SAVE-DATA.DAT", ec)) v.push_back(it->path());
    std::sort(v.begin(), v.end(), [](const fs::path& a, const fs::path& b) { return a.filename().string() > b.filename().string(); });
    return v;
}

Result restore(const fs::path& backup_folder, const fs::path& user_dir, const fs::path& backups_root) {
    Result r;
    std::vector<std::uint8_t> b;
    if (!read_all(backup_folder / "SYSTEM-DATA" / "SAVE-DATA.DAT", &b)) { r.message = "that backup could not be read"; return r; }
    const Info in = inspect(b);
    if (!in.valid) { r.message = "Not restored: " + in.problem + "."; return r; }
    Result bk = backup(user_dir, backups_root, "before-restore");
    if (!bk.ok) return bk;
    std::error_code ec;
    fs::create_directories(save_dir(user_dir), ec);
    if (!write_all(save_file(user_dir), b)) { r.message = "the save could not be written; it is unchanged"; return r; }
    const fs::path params = backup_folder / "SYSTEM-DATA" / "sce_sys" / "param.bin";
    if (fs::exists(params, ec) && !fs::exists(save_dir(user_dir) / "sce_sys" / "param.bin", ec)) {
        fs::create_directories(save_dir(user_dir) / "sce_sys", ec);
        fs::copy_file(params, save_dir(user_dir) / "sce_sys" / "param.bin", ec);
    }
    r.ok = true;
    r.backup = bk.backup;
    r.message = "Restored " + backup_folder.filename().string() + ". The save you had before is " + bk.message + ".";
    return r;
}

}  // namespace saves
