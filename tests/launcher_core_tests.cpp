// Host tests of the launcher logic (no UI): settings, game-folder checks, prepared-data cache, exit interpretation,
// redaction and the ZIP writer. Usage: launcher_core_tests <scratch-dir> [<real-game-folder>]
#include "../src/launcher/launcher_core.hpp"
#include <cstdio>
#include <fstream>
using namespace reborn::launcher;
static int failed = 0, passed = 0;
#define CHECK(c, what) do { if (c) { ++passed; } else { ++failed; std::printf("FAIL: %s\n", what); } } while (0)
static void write(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const fs::path scratch = argv[1];
    fs::remove_all(scratch); fs::create_directories(scratch);
    // settings
    Settings s; s.game_folder = "E:\\Games\\J Stars (dump)\\CUSA01593"; s.window_width = 1920; s.window_height = 1080; s.sound = false; s.volume = 35;
    s.keyboard = false; s.confirm_button = "circle"; s.player_name = "Jotaro";
    save_settings(scratch / "launcher.ini", s);
    auto l = load_settings(scratch / "launcher.ini");
    CHECK(l.game_folder == s.game_folder && l.window_width == 1920 && l.window_height == 1080 && !l.sound && l.volume == 35 && !l.keyboard &&
          l.confirm_button == "circle" && l.player_name == "Jotaro", "settings round trip (spaces and parentheses in the path)");
    write(scratch / "bad.ini", "window=99999x1\nvolume=250\nsound=maybe\nconfirm_button=square\nplayer_name=\n");
    std::vector<std::string> notes;
    auto b = load_settings(scratch / "bad.ini", &notes);
    CHECK(b.window_width == 1280 && b.volume == 100 && b.sound && b.confirm_button == "cross" && b.player_name == "Player" && notes.size() == 4,
          "invalid settings fall back to defaults and are reported");
    CHECK(sanitize_player_name("A very long player name indeed") == "A very long play", "player name limited to 16 characters");
    CHECK(sanitize_lobby_peers(" 192.168.1.20, 10.0.0.300;abc,10.0.0.7:5000 ") == "192.168.1.20, 10.0.0.7:5000", "lobby peers keep only valid addresses (and room codes)");
    CHECK(sanitize_lobby_peers("1.2.3") == "" && sanitize_lobby_peers("1.2.3.4.5") == "", "lobby peers reject malformed addresses");
    {
        Settings o; o.online = true; o.upnp = false; o.lobby_peers = "192.168.1.20";
        save_settings(scratch / "online.ini", o);
        auto r = load_settings(scratch / "online.ini", nullptr);
        CHECK(r.online && !r.upnp && r.lobby_peers == "192.168.1.20", "the online settings and peer address round trip");
        CHECK(!load_settings(scratch / "missing.ini", nullptr).online, "online is off by default");
    }
    {
        Settings f; f.frame_rate = 60;
        save_settings(scratch / "fps.ini", f);
        CHECK(load_settings(scratch / "fps.ini", nullptr).frame_rate == 60, "the 60 fps setting round trips");
        CHECK(load_settings(scratch / "missing.ini", nullptr).frame_rate == 30, "the frame rate is the game's 30 by default");
        Settings d = load_settings(scratch / "missing.ini", nullptr);
        CHECK(!d.fullscreen && !d.widescreen, "fullscreen and widescreen are off by default");
        CHECK(d.match_refresh, "refresh matching (fullscreen only) is on by default");
        CHECK(d.fsr, "FSR 1 upscaling is on by default");
        Settings nf; nf.fsr = false; save_settings(scratch / "nofsr.ini", nf);
        CHECK(!load_settings(scratch / "nofsr.ini", nullptr).fsr, "FSR off round trips");
        Settings mr; mr.match_refresh = false; save_settings(scratch / "mr.ini", mr);
        CHECK(!load_settings(scratch / "mr.ini", nullptr).match_refresh, "refresh matching off round trips");
        Settings w; w.fullscreen = true; w.widescreen = true;
        save_settings(scratch / "ws.ini", w);
        Settings wr = load_settings(scratch / "ws.ini", nullptr);
        CHECK(wr.fullscreen && wr.widescreen, "fullscreen and widescreen round trip");
        write(scratch / "fps-bad.ini", "frame_rate=120\n");
        std::vector<std::string> fn;
        CHECK(load_settings(scratch / "fps-bad.ini", &fn).frame_rate == 30 && !fn.empty(), "an unsupported frame rate falls back to 30 and is reported");
    }
    // game folder
    CHECK(check_game_folder("").status == GameStatus::NotChosen, "no folder chosen");
    CHECK(check_game_folder(scratch / "does not exist").status == GameStatus::Missing, "missing folder");
    write(scratch / "notagame" / "readme.txt", "x");
    CHECK(check_game_folder(scratch / "notagame").status == GameStatus::Invalid, "folder without param.sfo is invalid");
    fs::create_directories(scratch / "wrapper" / "My Game" / "sce_sys");
    { std::ofstream(scratch / "wrapper" / "My Game" / "eboot.bin") << "x"; std::ofstream(scratch / "wrapper" / "My Game" / "sce_sys" / "param.sfo") << "x"; }
    {
        const auto w = check_game_folder(scratch / "wrapper");
        bool hint = false;
        for (const auto& d : w.details) hint = hint || (d.rfind("Hint:", 0) == 0 && d.find("My Game") != std::string::npos);
        CHECK(w.status == GameStatus::Invalid && hint, "a folder one level above the dump points at the subfolder");
    }
    // exits
    write(scratch / "s" / "close.log", "[ 1.0] pad: x\n[ 99.1] game window closed by the player\n");
    auto e0 = describe_exit(0, scratch / "s" / "close.log", false);
    CHECK(e0.normal && e0.headline == "Game closed.", "exit 0 after closing the window is normal");
    write(scratch / "s" / "trap.log", "[ 30.88] UNIMPLEMENTED IMPORT #307 0H2vBYbTLHI#libSceGnmDriver [sceGnmDrawInitDefaultHardwareState200] called from eboot.bin+0x45e3a9\n");
    auto e3 = describe_exit(3, scratch / "s" / "trap.log", false);
    CHECK(!e3.normal && e3.detail.find("sceGnmDrawInitDefaultHardwareState200") != std::string::npos, "exit 3 names the missing function");
    auto e7 = describe_exit(7, scratch / "s" / "none.log", false);
    CHECK(!e7.normal && e7.headline.find("time limit") != std::string::npos, "exit 7 is not a normal end");
    auto ef = describe_exit(1, scratch / "s" / "none.log", true);
    CHECK(!ef.normal && ef.headline == "You ended the game session.", "forced end");
    write(scratch / "s" / "fault.log", "[ 6.07] FAULT exception 0xc0000005 (access 0) address ffffffffffffffff at host/other ntdll.dll+0x27381 stage=1\n");
    CHECK(describe_exit(4, scratch / "s" / "fault.log", false).detail.find("ntdll.dll+0x27381") != std::string::npos, "exit 4 quotes the fault line");
    write(scratch / "s" / "boot.log", "[ 0.0] JStarsReborn runtime 0.4.0\n[ 0.1] FATAL: bad boot file signature (run tools/prepare)\n");
    {
        const auto e2 = describe_exit(2, scratch / "s" / "boot.log", false);
        CHECK(!e2.normal && e2.headline == "The game could not start." && e2.detail.find("Delete prepared data") != std::string::npos,
              "exit 2 with a damaged boot file points at Delete prepared data");
    }
    // redaction
    CHECK(redact("C:\\Users\\Alex\\AppData\\Local\\JStarsReborn and alex", {"C:\\Users\\Alex", "Alex"}) ==
              "<redacted>\\AppData\\Local\\JStarsReborn and <redacted>", "redaction is case-insensitive");
    {
        const std::vector<std::string> secrets{"Alex", "D:\\Private Games\\Alex", "DESKTOP-TEST", "10.20.30.40:47391", "10.20.30.40"};
        const auto raw = std::string("D:\\Private Games\\Alex\\eboot.bin desktop-test 10.20.30.40:47391");
        CHECK(redact(raw, secrets) == "<redacted>\\eboot.bin <redacted> <redacted>",
              "longest private root wins over a name inside it, regardless of input order");
        CHECK(redact(json_escape(raw), secrets) == "<redacted>\\\\eboot.bin <redacted> <redacted>",
              "JSON-escaped command lines redact complete private roots and endpoints");
        CHECK(redact("d:/private games/ALEX/eboot.bin", secrets) == "<redacted>/eboot.bin",
              "forward-slash path variants are redacted");
        CHECK(redact("Al on PC: allocate Vulkan", {"", "Al", "PC"}) == "<redacted> on <redacted>: allocate Vulkan",
              "short identities are redacted as words without damaging unrelated diagnostics");
        CHECK(redact("C:\\redacted\\game", {"C:\\redacted", "redacted"}) == "<redacted>\\game",
              "replacement tokens are not reprocessed by later patterns");
        CHECK(redact("frame time ms p50 16.67 p95 17.30 p99 24.10", secrets) ==
              "frame time ms p50 16.67 p95 17.30 p99 24.10", "redaction retains timing measurements");
    }
    {
        Settings diagnostic;
        diagnostic.frame_rate = 60; diagnostic.fullscreen = true; diagnostic.widescreen = true;
        diagnostic.game_folder = "private-directory"; diagnostic.player_name = "private-player";
        diagnostic.lobby_peers = "10.20.30.40";
        auto settings = diagnostic_settings_json(diagnostic);
        CHECK(settings.find("\"frame_rate\": 60") != std::string::npos && settings.find("\"fullscreen\": true") != std::string::npos &&
              settings.find("\"widescreen\": true") != std::string::npos, "reports distinguish 60 fps, fullscreen and widescreen");
        CHECK(settings.find("private-") == std::string::npos && settings.find("10.20.30.40") == std::string::npos,
              "diagnostic settings exclude directories, player identity and peers");
        write(scratch / "diagnostic-settings.json", settings);
        write(scratch / "redacted-session.json", "{\"command_line\":\"" +
              redact(json_escape("D:\\Private Games\\Example\\game.exe --frame-rate 60"), {"D:\\Private Games\\Example"}) + "\"}");
    }
    // zip
    write(scratch / "long.log", "GPU device header\nframe timings\nFAULT at end\n");
    CHECK(read_text(scratch / "long.log", 17, false) == "GPU device header", "bounded startup read preserves GPU metadata");
    CHECK(read_text(scratch / "long.log", 13) == "FAULT at end\n", "bounded tail read preserves the failure");
    CHECK(read_text(scratch / "long.log", 0, false).empty() && read_text(scratch / "long.log", 0).empty(), "zero-size reads stay empty");
    CHECK(read_text(scratch / "missing.log", 17, false).empty(), "missing startup log is allowed");
    {
        ZipWriter z(scratch / "report.zip");
        z.add("a.txt", "hello\n");
        z.add("dir/b.json", "{\"x\": 1}\n");
        z.add("sessions/1/session.json", read_text(scratch / "redacted-session.json"));
        z.add("settings.json", read_text(scratch / "diagnostic-settings.json"));
        z.finish();
    }
    CHECK(fs::file_size(scratch / "report.zip") > 0, "zip written");
    CHECK(crc32("123456789") == 0xCBF43926u, "crc32 check value");
    // sessions
    for (auto n : {"20261101-100000", "20261101-110000", "20261101-120000", "keep-me"}) fs::create_directories(scratch / "sessions" / n);
    prune_sessions(scratch / "sessions", 2);
    CHECK(!fs::exists(scratch / "sessions" / "20261101-100000") && fs::exists(scratch / "sessions" / "20261101-110000") &&
          fs::exists(scratch / "sessions" / "keep-me"), "pruning keeps the newest sessions and foreign folders");
    // real game folder (optional)
    if (argc >= 3) {
        auto g = check_game_folder(argv[2]);
        CHECK(g.ok() && g.title_id == "CUSA01593" && g.app_ver == "01.02", "real dump validates as the supported build");
        const auto dir = prepared_dir(scratch / "data", g);
        std::string why;
        CHECK(!prepared_valid(dir, g.inputs, &why), "not prepared yet");
        prepare_game(g, dir, {}, {});
        CHECK(prepared_valid(dir, g.inputs, &why), "prepared after preparation");
        auto changed = g.inputs; changed[0].sha256[0] = changed[0].sha256[0] == '0' ? '1' : '0';
        CHECK(!prepared_valid(dir, changed, &why) && why.find("changed") != std::string::npos, "changed inputs invalidate the prepared data");
        { std::fstream f(dir / "boot-linked.bin", std::ios::in | std::ios::out | std::ios::binary); f.seekp(100); f.put('\x5a'); }
        CHECK(!prepared_valid(dir, g.inputs, &why) && why.find("damaged") != std::string::npos, "a damaged boot image is detected");
        std::stop_source stop; stop.request_stop();
        bool cancelled = false;
        try { prepare_game(g, scratch / "cancelled", {}, stop.get_token()); } catch (const std::exception& e) { cancelled = std::string(e.what()) == "cancelled"; }
        CHECK(cancelled && !fs::exists(scratch / "cancelled" / "boot-linked.bin") && !prepared_valid(scratch / "cancelled", g.inputs), "cancellation leaves no prepared data");
    }
    std::printf("launcher_core_tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
