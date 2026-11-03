// Tests for src/launcher/saves.cpp with synthetic saves (no game data). Optional argument: a real SAVE-DATA.DAT written by
// the game, to check that it is recognised as valid (not committed; the file stays on the tester's PC).
//   g++ -std=c++20 -Isrc/launcher tests/saves_tests.cpp src/launcher/saves.cpp -o saves_tests && ./saves_tests [save]
#include "saves.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>

static int g_pass, g_fail;
#define CHECK(c, what) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)
namespace fs = std::filesystem;

static std::vector<std::uint8_t> synthetic(std::uint8_t seed) {
    std::vector<std::uint8_t> b(saves::kSaveSize);
    for (std::size_t i = 8; i < b.size(); ++i) b[i] = std::uint8_t((i * 31 + seed) & 0x3F);
    for (std::size_t i = 0x5C40; i < 0x5C80; ++i) b[i] = 0;
    b[0x5C5C] = 0x10; b[0x5C5D] = 0x27;  // 10000 J-Points
    saves::fix_checksum(b);
    return b;
}
static void put(const fs::path& p, const std::vector<std::uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
}
static std::vector<std::uint8_t> get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    using namespace saves;
    const fs::path root = fs::temp_directory_path() / "jstars-saves-tests";
    fs::remove_all(root);
    const fs::path user = root / "user", backups = root / "save-backups";

    auto b = synthetic(1);
    CHECK(checksum_ok(b) && inspect(b).valid && inspect(b).jpoints == 10000 && inspect(b).owned == 0, "a synthetic save with a fixed checksum is valid");
    auto broken = b; broken[100] ^= 1;
    CHECK(!checksum_ok(broken) && !inspect(broken).valid, "one changed byte breaks the checksum");
    CHECK(!inspect(std::vector<std::uint8_t>(1000)).valid, "a file of the wrong size is refused");
    auto u = b; unlock_all(u);
    const Info ui = inspect(u);
    CHECK(ui.valid && ui.all_unlocked && ui.owned == kOwned && ui.jpoints == kJPoints, "unlock_all sets every character, owned flags and J-Points, checksum stays valid");
    CHECK(u[0x5C40 + 6] == 0x0F && u[0x5C40 + 7] == 0x00 && u[0x5C6C + 5] == 0x3F && u[0x5C6C + 6] == 0x00, "unlock_all touches exactly 52 character and 46 owned bits");

    // unlock on disk: needs a save; backs up first; second call is a no-op
    CHECK(!unlock_all_characters(user, backups).ok, "unlock without a save asks to start the game once");
    put(save_file(user), b); put(save_dir(user) / "sce_sys" / "param.bin", std::vector<std::uint8_t>(64, 1));
    Result r = unlock_all_characters(user, backups);
    CHECK(r.ok && inspect(get(save_file(user))).all_unlocked && fs::exists(r.backup / "SYSTEM-DATA" / "SAVE-DATA.DAT"), "unlock writes the save after a backup");
    CHECK(get(r.backup / "SYSTEM-DATA" / "SAVE-DATA.DAT") == b, "the backup holds the save as it was before");
    {   // a read-only save (copied from a USB stick or a ZIP) can still be replaced
        const fs::path u2 = root / "ro-user", b2 = root / "ro-backups";
        put(save_file(u2), b); put(save_dir(u2) / "sce_sys" / "param.bin", std::vector<std::uint8_t>(64, 1));
        fs::permissions(save_file(u2), fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
        Result ro = unlock_all_characters(u2, b2);
        CHECK(ro.ok && inspect(get(save_file(u2))).all_unlocked, "a read-only save is unlocked too");
    }
    CHECK(unlock_all_characters(user, backups).ok && list_backups(backups).size() == 1, "unlocking again changes nothing and makes no backup");

    // import: file or folder (shadPS4-style layout), refusals
    auto other = synthetic(7);
    put(root / "friend" / "shadPS4" / "user" / "savedata" / "1" / "CUSA01593" / "SYSTEM-DATA" / "SAVE-DATA.DAT", other);
    std::string why;
    const fs::path found = find_importable(root / "friend", &why);
    CHECK(!found.empty() && get(found) == other, "a save is found inside a shadPS4-style folder");
    r = import_save(found, user, backups);
    CHECK(r.ok && get(save_file(user)) == other && list_backups(backups).size() == 2, "import replaces the save after backing up the old one");
    put(root / "bad.dat", broken);
    CHECK(!import_save(root / "bad.dat", user, backups).ok && get(save_file(user)) == other, "a damaged save is refused and the current one kept");
    put(root / "enc" / "SYSTEM-DATA", std::vector<std::uint8_t>(4096)); put(root / "enc" / "SYSTEM-DATA.bin", std::vector<std::uint8_t>(96));
    CHECK(find_importable(root / "enc", &why).empty() && why.find("encrypted") != std::string::npos, "an encrypted PS4 export is recognised and explained");
    const fs::path fresh = root / "fresh-user";
    CHECK(!import_save(found, fresh, backups).ok && !fs::exists(save_file(fresh)), "import into an install whose game never made a save asks to start it once");

    // restore: newest first, restores bytes, backs up the current one
    const auto list = list_backups(backups);
    CHECK(list.size() == 2 && list[0].filename().string() > list[1].filename().string(), "backups are listed newest first");
    const fs::path oldest = list.back();
    r = restore(oldest, user, backups);
    CHECK(r.ok && get(save_file(user)) == b && list_backups(backups).size() == 3, "restore brings back the backed-up save and keeps the replaced one");

    if (argc > 1) {
        const auto real = get(argv[1]);
        const Info ri = inspect(real);
        std::printf("real save %s: %s, %u J-Points, %d/%d owned\n", argv[1], ri.valid ? "valid" : ri.problem.c_str(), ri.jpoints, ri.owned, kOwned);
        CHECK(ri.valid, "a save written by the game is valid");
        auto ru = real; unlock_all(ru);
        CHECK(inspect(ru).valid && inspect(ru).all_unlocked, "a real save unlocks and stays valid");
    }
    fs::remove_all(root);
    std::printf("saves_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
