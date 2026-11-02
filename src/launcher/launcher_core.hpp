#pragma once
// Launcher logic that does not depend on the Windows UI: settings, game-folder checks, the prepared-data cache,
// exit-code interpretation, session records, diagnostic-report packaging and redaction. Portable C++20 so it can be
// tested on any host (tests/launcher_core_tests.cpp).
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "reborn/prepare.hpp"

namespace reborn::launcher {
namespace fs = std::filesystem;

inline constexpr const char* launcher_version = "0.5.1";

// ---- settings (launcher.ini, UTF-8 key=value) ---------------------------------------------------------------------
struct Settings {
    std::string game_folder;            // UTF-8; empty = not chosen yet
    int window_width = 1280, window_height = 720;
    bool sound = true;
    int volume = 100;                   // 0..100
    bool keyboard = true;               // keyboard acts as a controller while the game window has focus
    std::string confirm_button = "cross"; // "cross" (default of this build) or "circle"
    std::string player_name = "Player";
    bool online = false;                // experimental: --online, LAN room directory (Friendly Match), see docs/ADR-007-online.md
    bool upnp = true;                   // ask the router to open the two UDP ports (online.hpp)
    std::string lobby_peers;            // friends' room codes or addresses, as typed (comma-separated)
};
// Reads settings; unknown keys are ignored, invalid values fall back to defaults (reported in `notes`).
Settings load_settings(const fs::path& file, std::vector<std::string>* notes = nullptr);
// Writes atomically (temporary file, then rename). Throws std::runtime_error on failure.
void save_settings(const fs::path& file, const Settings& s);
// Supported window sizes offered in the UI (the runtime's --window option).
const std::vector<std::pair<int, int>>& window_sizes();
std::string sanitize_player_name(const std::string& in); // 1..16 printable characters, default "Player"
std::string sanitize_lobby_peers(const std::string& in);  // keeps up to 8 valid room codes / IPv4[:port] entries, as typed

// ---- supported builds --------------------------------------------------------------------------------------------
struct SupportedBuild {
    std::string title_id, app_ver, label;
    std::string eboot_sha256, libc_sha256, fios2_sha256;
};
const std::vector<SupportedBuild>& supported_builds();

// ---- game folder check -------------------------------------------------------------------------------------------
enum class GameStatus { NotChosen, Missing, Invalid, Unsupported, Ok };
struct GameCheck {
    GameStatus status = GameStatus::NotChosen;
    fs::path folder;
    std::string title, title_id, app_ver, content_id;
    std::string summary;                  // one line for the Play page
    std::vector<std::string> details;     // lines for the Game folder page ("OK ..." / "Problem ..." / "Note ...")
    std::vector<prep::InputFile> inputs;  // hashed inputs (when readable)
    const SupportedBuild* build = nullptr;
    fs::path icon;                        // sce_sys/icon0.png in the user's folder, if present (read locally, never copied)
    [[nodiscard]] bool ok() const noexcept { return status == GameStatus::Ok; }
};
GameCheck check_game_folder(const fs::path& folder, std::stop_token stop = {});

// ---- prepared data -------------------------------------------------------------------------------------------------
// <data>/prepared/<TITLE>_<VER>_<first 12 of eboot sha256>/ : boot-linked.bin, import-names.tsv, inputs.txt
fs::path prepared_dir(const fs::path& data_root, const GameCheck& game);
// True when the folder holds a complete boot image made from exactly these inputs by this preparation version.
bool prepared_valid(const fs::path& dir, const std::vector<prep::InputFile>& inputs, std::string* why = nullptr);
// Runs the native preparation into dir and records the inputs (atomic: inputs.txt is written last).
void prepare_game(const GameCheck& game, const fs::path& dir, const prep::Progress& progress, std::stop_token stop);

// ---- sessions ------------------------------------------------------------------------------------------------------
struct ExitInfo {
    bool normal = false;   // the player ended the session (closed the window / the game exited by itself)
    std::string headline;  // short sentence for the Play page
    std::string detail;    // one or two sentences with the reason, from the session log when available
};
// Interprets the runtime exit code together with the session log (game.log). forced = the launcher ended the process
// after the player confirmed.
ExitInfo describe_exit(std::uint32_t code, const fs::path& game_log, bool forced);
std::string utc_stamp(bool for_folder);  // "20261101-142233" or "2026-11-01T14:22:33Z"
// Session folder names sorted oldest first; removes all but the newest `keep` session folders (never other files).
void prune_sessions(const fs::path& sessions_root, std::size_t keep);
std::optional<fs::path> latest_session(const fs::path& sessions_root);

// ---- diagnostics ----------------------------------------------------------------------------------------------------
// Replaces the user's profile path and user name with placeholders (case-insensitive).
std::string redact(std::string text, const std::vector<std::string>& secrets);
// Minimal ZIP writer (stored, no compression) with CRC-32. Throws std::runtime_error on failure.
class ZipWriter {
public:
    explicit ZipWriter(const fs::path& file);
    ~ZipWriter();
    void add(const std::string& name, const std::string& data);
    void finish();
private:
    struct Entry { std::string name; std::uint32_t crc, size, offset; };
    std::vector<Entry> entries_;
    std::string out_;
    fs::path file_;
    bool done_ = false;
};
std::uint32_t crc32(const std::string& data);
std::string json_escape(const std::string& s);
std::string read_text(const fs::path& p, std::size_t max_bytes = 4u << 20); // whole file (or its last max_bytes)
// UTF-8 <-> path without the Windows ANSI code page (paths may contain any character).
std::string u8(const fs::path& p);
fs::path from_u8(const std::string& s);

} // namespace reborn::launcher
