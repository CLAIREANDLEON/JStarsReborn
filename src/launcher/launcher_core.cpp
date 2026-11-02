#include "launcher_core.hpp"
#include "online.hpp"

#include "reborn/game_validate.hpp"
#include "reborn/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace reborn::launcher {
std::string u8(const fs::path& p);
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
bool parse_int(const std::string& v, int lo, int hi, int& out) {
    if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return false;
    const int n = std::stoi(v);
    if (n < lo || n > hi) return false;
    out = n;
    return true;
}
bool parse_bool(const std::string& v, bool& out) {
    if (v == "1" || v == "true") { out = true; return true; }
    if (v == "0" || v == "false") { out = false; return true; }
    return false;
}
void write_file_atomic(const fs::path& file, const std::string& data) {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + u8(tmp));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) throw std::runtime_error("write failed: " + u8(tmp));
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("cannot replace " + u8(file));
    }
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string first_line_with(const std::string& log, const std::string& needle) {
    const auto p = log.find(needle);
    if (p == std::string::npos) return {};
    auto b = log.rfind('\n', p);
    b = b == std::string::npos ? 0 : b + 1;
    auto e = log.find('\n', p);
    std::string line = log.substr(b, (e == std::string::npos ? log.size() : e) - b);
    if (!line.empty() && line[0] == '[') {
        const auto close = line.find("] ");
        if (close != std::string::npos) line = line.substr(close + 2);
    }
    return trim(line);
}

} // namespace

// ---- settings ------------------------------------------------------------------------------------------------------
const std::vector<std::pair<int, int>>& window_sizes() {
    static const std::vector<std::pair<int, int>> s = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    return s;
}

std::string sanitize_player_name(const std::string& in) {
    std::string out;
    for (unsigned char c : in)
        if (c >= 0x20 && c != 0x7f && out.size() < 16) out += static_cast<char>(c);
    out = trim(out);
    // keep only complete UTF-8 sequences (a cut at 16 bytes may split one)
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
        std::size_t k = out.size();
        while (k > 0 && (static_cast<unsigned char>(out[k - 1]) & 0xC0) == 0x80) --k;
        if (k == 0) { out.clear(); break; }
        const unsigned char lead = static_cast<unsigned char>(out[k - 1]);
        const std::size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (out.size() - (k - 1) >= need) break;
        out.erase(k - 1);
    }
    if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0xC0) out.pop_back();
    return out.empty() ? "Player" : out;
}

// Keeps what the player typed (room codes or IPv4 addresses with an optional port), drops entries that are neither.
std::string sanitize_lobby_peers(const std::string& in) {
    std::string out, cur;
    int kept = 0;
    auto flush = [&] {
        const auto a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
        const std::string t = a == std::string::npos ? std::string() : cur.substr(a, b - a + 1);
        cur.clear();
        uint32_t ip = 0;
        uint16_t port = 0;
        if (t.empty() || kept >= 8 || !online::decode_peer(t, &ip, &port)) return;
        if (!out.empty()) out += ", ";
        out += t;
        ++kept;
    };
    for (char c : in) {
        if (c == ',' || c == ';') flush();
        else cur += c;
    }
    flush();
    return out;
}

Settings load_settings(const fs::path& file, std::vector<std::string>* notes) {
    Settings s;
    std::ifstream f(file, std::ios::binary);
    if (!f) return s;
    std::string line;
    auto note = [&](const std::string& n) { if (notes) notes->push_back(n); };
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        bool ok = true;
        if (k == "game_folder") s.game_folder = v;
        else if (k == "window") {
            const auto x = v.find('x');
            int w = 0, h = 0;
            ok = x != std::string::npos && parse_int(v.substr(0, x), 640, 7680, w) && parse_int(v.substr(x + 1), 360, 4320, h);
            if (ok) { s.window_width = w; s.window_height = h; }
        } else if (k == "sound") ok = parse_bool(v, s.sound);
        else if (k == "volume") ok = parse_int(v, 0, 100, s.volume);
        else if (k == "keyboard") ok = parse_bool(v, s.keyboard);
        else if (k == "confirm_button") { ok = v == "cross" || v == "circle"; if (ok) s.confirm_button = v; }
        else if (k == "player_name") s.player_name = sanitize_player_name(v);
        else if (k == "online") ok = parse_bool(v, s.online);
        else if (k == "upnp") ok = parse_bool(v, s.upnp);
        else if (k == "lobby_peers") s.lobby_peers = sanitize_lobby_peers(v);
        if (!ok) note("settings: ignored invalid value for " + k);
    }
    return s;
}

void save_settings(const fs::path& file, const Settings& s) {
    std::ostringstream o;
    o << "# JStarsReborn launcher settings (written by the launcher; UTF-8)\n"
      << "game_folder=" << s.game_folder << "\n"
      << "window=" << s.window_width << "x" << s.window_height << "\n"
      << "sound=" << (s.sound ? 1 : 0) << "\n"
      << "volume=" << s.volume << "\n"
      << "keyboard=" << (s.keyboard ? 1 : 0) << "\n"
      << "confirm_button=" << s.confirm_button << "\n"
      << "player_name=" << sanitize_player_name(s.player_name) << "\n"
      << "online=" << (s.online ? 1 : 0) << "\n"
      << "upnp=" << (s.upnp ? 1 : 0) << "\n"
      << "lobby_peers=" << sanitize_lobby_peers(s.lobby_peers) << "\n";
    write_file_atomic(file, o.str());
}

// ---- supported builds ------------------------------------------------------------------------------------------------
const std::vector<SupportedBuild>& supported_builds() {
    static const std::vector<SupportedBuild> b = {
        {"CUSA01593", "01.02", "J-Stars Victory VS+ (US, version 01.02)",
         "d43b7b5db10b5808c06691b27ef0ce1b5d27da6b1e07d811db5bea125ceaed54",
         "4378b47f46f1d856824a6f971db1a0c3f41833b77f49d2fba9538f667a139166",
         "3b8eb4d5a5c5a7b381d8d0610077e6cd404c29695a686274802f6b2737f6bf22"},
    };
    return b;
}

// ---- game folder -------------------------------------------------------------------------------------------------------
GameCheck check_game_folder(const fs::path& folder, std::stop_token stop) {
    GameCheck r;
    r.folder = folder;
    if (folder.empty()) {
        r.status = GameStatus::NotChosen;
        r.summary = "Choose your game folder to get started.";
        return r;
    }
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        r.status = GameStatus::Missing;
        r.summary = "The game folder was not found. Choose it again on the Game folder page.";
        r.details.push_back("Problem: the folder does not exist or cannot be opened: " + u8(folder));
        return r;
    }
    const auto v = setup::validate_game_folder(folder, {}, stop);
    r.title = v.title;
    r.title_id = v.title_id;
    r.app_ver = v.app_ver;
    r.content_id = v.content_id;
    for (const auto& p : v.problems) r.details.push_back("Problem: " + p);
    if (!v.ok()) {
        r.status = GameStatus::Invalid;
        r.summary = "This folder is not a usable game dump. See the Game folder page for details.";
        if (!v.param_sfo_present) r.details.push_back("Problem: sce_sys/param.sfo is missing - choose the folder that contains eboot.bin and sce_sys.");
        // A common mistake: choosing the folder one level above the dump. Point at the subfolder that looks like the game.
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2) && fs::is_regular_file(it->path() / "eboot.bin", e2) &&
                fs::is_regular_file(it->path() / "sce_sys" / "param.sfo", e2)) {
                r.details.push_back("Hint: the game files seem to be in the subfolder \"" + u8(it->path().filename()) +
                                    "\". Choose that folder instead: " + u8(it->path()));
                break;
            }
        }
        return r;
    }
    if (!r.title.empty()) r.details.push_back("OK: game title: " + r.title);
    r.details.push_back("OK: title ID " + r.title_id + ", version " + r.app_ver);
    if (v.executable == setup::ExecutableKind::Ps4Self || v.executable == setup::ExecutableKind::Elf)
        r.details.push_back(std::string("OK: eboot.bin is a readable ") + (v.executable == setup::ExecutableKind::Elf ? "ELF" : "SELF") + " executable");
    for (const auto& m : prep::linked_modules()) {
        if (!fs::is_regular_file(folder / "sce_module" / m, ec)) {
            r.status = GameStatus::Invalid;
            r.details.push_back("Problem: sce_module/" + m + " is missing (it is part of the game dump and is required).");
        }
    }
    if (r.status == GameStatus::Invalid) {
        r.summary = "Required game files are missing. See the Game folder page.";
        return r;
    }
    try {
        r.inputs = prep::hash_inputs(folder, stop);
    } catch (const std::exception& e) {
        r.status = GameStatus::Invalid;
        r.summary = "The game files could not be read. See the Game folder page.";
        r.details.push_back(std::string("Problem: ") + e.what());
        return r;
    }
    auto hash_of = [&](const std::string& rel) {
        for (const auto& i : r.inputs)
            if (i.relative == rel) return i.sha256;
        return std::string();
    };
    for (const auto& b : supported_builds()) {
        if (b.title_id == r.title_id && b.app_ver == r.app_ver && b.eboot_sha256 == hash_of("eboot.bin") &&
            b.libc_sha256 == hash_of("sce_module/libc.prx") && b.fios2_sha256 == hash_of("sce_module/libSceFios2.prx")) {
            r.build = &b;
        }
    }
    if (fs::is_regular_file(folder / "sce_sys" / "icon0.png", ec)) r.icon = folder / "sce_sys" / "icon0.png";
    if (!r.build) {
        r.status = GameStatus::Unsupported;
        r.summary = "This game version is not supported yet (only J-Stars Victory VS+ CUSA01593 version 01.02 is).";
        r.details.push_back("Problem: this exact build has not been tested with JStarsReborn. Supported: CUSA01593 version 01.02 "
                            "(the game with its 01.02 update applied). Your folder: " + r.title_id + " version " + r.app_ver + ".");
        r.details.push_back("Note: eboot.bin SHA-256 " + hash_of("eboot.bin"));
        return r;
    }
    r.status = GameStatus::Ok;
    r.details.push_back("OK: matches the supported build: " + r.build->label);
    r.details.push_back("OK: bundled modules libc.prx and libSceFios2.prx match that build");
    r.details.push_back("Note: the game folder is only read; nothing is written to it.");
    r.summary = "Ready.";
    return r;
}

// ---- prepared data ---------------------------------------------------------------------------------------------------
namespace {
const char* prepare_format = "prepare=native-1 format=JSTBOOT5";
std::string inputs_text(const std::vector<prep::InputFile>& inputs, const std::string& boot_sha) {
    std::ostringstream o;
    o << prepare_format << "\n";
    for (const auto& i : inputs) o << i.relative << " " << i.bytes << " " << i.sha256 << "\n";
    o << "boot-linked.bin " << boot_sha << "\n";
    return o.str();
}
std::string file_sha256(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    PortableSha256 h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (f.gcount() > 0) h.update(buf.data(), static_cast<std::size_t>(f.gcount()));
    }
    return PortableSha256::to_hex(h.finish());
}
} // namespace

fs::path prepared_dir(const fs::path& data_root, const GameCheck& game) {
    std::string eboot;
    for (const auto& i : game.inputs)
        if (i.relative == "eboot.bin") eboot = i.sha256.substr(0, 12);
    return data_root / "prepared" / (game.title_id + "_" + game.app_ver + "_" + eboot);
}

bool prepared_valid(const fs::path& dir, const std::vector<prep::InputFile>& inputs, std::string* why) {
    const std::string recorded = read_text(dir / "inputs.txt", 1 << 16);
    if (recorded.empty()) { if (why) *why = "not prepared yet"; return false; }
    const auto last = recorded.rfind("boot-linked.bin ");
    if (last == std::string::npos) { if (why) *why = "incomplete preparation record"; return false; }
    const std::string boot_sha = trim(recorded.substr(last + 16));
    if (recorded != inputs_text(inputs, boot_sha)) { if (why) *why = "the game files changed since the last preparation"; return false; }
    if (file_sha256(dir / "boot-linked.bin") != boot_sha) { if (why) *why = "the prepared boot image is damaged"; return false; }
    if (why) *why = "prepared";
    return true;
}

void prepare_game(const GameCheck& game, const fs::path& dir, const prep::Progress& progress, std::stop_token stop) {
    std::error_code ec;
    fs::remove(dir / "inputs.txt", ec); // the record goes last: an interrupted run is never taken as complete
    const auto r = prep::prepare(game.folder, dir, progress, stop);
    write_file_atomic(dir / "inputs.txt", inputs_text(r.inputs, r.boot_sha256));
}

// ---- sessions --------------------------------------------------------------------------------------------------------
std::string utc_stamp(bool for_folder) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, for_folder ? "%Y%m%d-%H%M%S" : "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

ExitInfo describe_exit(std::uint32_t code, const fs::path& game_log, bool forced) {
    ExitInfo e;
    const std::string log = read_text(game_log);
    if (forced) {
        e.headline = "You ended the game session.";
        e.detail = "The game was not responding to a normal close, so it was ended at your request. Unsaved progress is lost.";
        return e;
    }
    const std::string guest_exit = first_line_with(log, "guest called _exit(");
    if (code == 0 || !guest_exit.empty()) {
        e.normal = true;
        if (!first_line_with(log, "game window closed by the player").empty()) e.headline = "Game closed.";
        else if (!guest_exit.empty()) e.headline = "The game ended by itself.";
        else e.headline = "Game closed.";
        return e;
    }
    switch (code) {
    case 3: {
        e.headline = "The game stopped: it asked for a console feature this version does not support yet.";
        std::string line = first_line_with(log, "UNIMPLEMENTED IMPORT");
        std::string name;
        const auto b = line.find('['), c = line.find(']', b == std::string::npos ? 0 : b);
        if (b != std::string::npos && c != std::string::npos) name = line.substr(b + 1, c - b - 1);
        e.detail = name.empty() ? "The session log names the missing function." : "Missing function: " + name + ". This was stopped safely; nothing was damaged.";
        break;
    }
    case 4:
        e.headline = "The game crashed.";
        e.detail = first_line_with(log, "FAULT");
        if (e.detail.empty()) e.detail = "An unexpected error stopped the game. The session log has the details.";
        break;
    case 5:
        e.headline = "The game stopped because of a graphics error.";
        e.detail = first_line_with(log, "GPU FATAL");
        break;
    case 6:
        e.headline = "Graphics could not start.";
        e.detail = "JStarsReborn needs a GPU with a Vulkan 1.3 driver. Update your graphics driver and try again.";
        break;
    case 7:
        e.headline = "The session ended at a test time limit.";
        e.detail = "This only happens in diagnostic runs; normal play has no time limit. Please report it with a diagnostic report.";
        break;
    case 8:
        e.headline = "The game stopped after detecting memory corruption (stack check).";
        e.detail = first_line_with(log, "__stack_chk_fail");
        break;
    case 2:
        e.headline = "The game could not start.";
        e.detail = first_line_with(log, "FATAL:");
        if (e.detail.find("boot file") != std::string::npos || e.detail.find("import-names") != std::string::npos)
            e.detail += " The prepared data seems damaged: use Troubleshooting > Delete prepared data, then press Play again.";
        else if (e.detail.empty()) e.detail = "The session log has the details.";
        break;
    case 1:
        if (first_line_with(log, "JStarsReborn runtime").empty()) {
            e.headline = "The game runtime rejected its start options.";
            e.detail = "This is a launcher problem; please create a diagnostic report.";
        } else {
            e.headline = "The game process was ended unexpectedly (exit code 1).";
            e.detail = "This happens when it is ended from outside, for example with Task Manager. If you did not do that, please create a diagnostic report.";
        }
        break;
    default: {
        char b[96];
        std::snprintf(b, sizeof b, "The game ended unexpectedly (exit code 0x%08X).", code);
        e.headline = b;
        e.detail = "The session log may have more details.";
    }
    }
    if (e.detail.empty()) e.detail = "The session log has the details.";
    return e;
}

std::optional<fs::path> latest_session(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) dirs.push_back(it->path());
    if (dirs.empty()) return std::nullopt;
    std::sort(dirs.begin(), dirs.end());
    return dirs.back();
}

void prune_sessions(const fs::path& root, std::size_t keep) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = u8(it->path().filename());
        // only folders the launcher created: 15 characters "YYYYMMDD-HHMMSS" (+ optional suffix)
        if (it->is_directory(ec) && name.size() >= 15 && name[8] == '-' && std::isdigit(static_cast<unsigned char>(name[0])))
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    while (dirs.size() > keep) {
        fs::remove_all(dirs.front(), ec);
        dirs.erase(dirs.begin());
    }
}

// ---- diagnostics -----------------------------------------------------------------------------------------------------
std::string redact(std::string text, const std::vector<std::string>& secrets) {
    for (const auto& s0 : secrets) {
        if (s0.size() < 3) continue;
        const std::string s = lower(s0);
        std::string low = lower(text);
        std::string out;
        std::size_t pos = 0;
        while (true) {
            const auto f = low.find(s, pos);
            if (f == std::string::npos) { out += text.substr(pos); break; }
            out += text.substr(pos, f - pos);
            out += "<redacted>";
            pos = f + s.size();
        }
        text = out;
    }
    return text;
}

std::uint32_t crc32(const std::string& data) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

namespace {
void put16(std::string& o, std::uint16_t v) { o += static_cast<char>(v & 0xFF); o += static_cast<char>(v >> 8); }
void put32(std::string& o, std::uint32_t v) { for (int i = 0; i < 4; ++i) o += static_cast<char>((v >> (i * 8)) & 0xFF); }
}

ZipWriter::ZipWriter(const fs::path& file) : file_(file) {}
ZipWriter::~ZipWriter() = default;
void ZipWriter::add(const std::string& name, const std::string& data) {
    if (data.size() > 0x7fffffff || out_.size() > 0x7fffffff) throw std::runtime_error("report too large");
    Entry e{name, crc32(data), static_cast<std::uint32_t>(data.size()), static_cast<std::uint32_t>(out_.size())};
    put32(out_, 0x04034b50); put16(out_, 20); put16(out_, 0x0800 /* UTF-8 names */); put16(out_, 0 /* stored */);
    put16(out_, 0); put16(out_, 0x21); // DOS time/date (1980-01-01)
    put32(out_, e.crc); put32(out_, e.size); put32(out_, e.size);
    put16(out_, static_cast<std::uint16_t>(name.size())); put16(out_, 0);
    out_ += name;
    out_ += data;
    entries_.push_back(e);
}
void ZipWriter::finish() {
    const auto cd = static_cast<std::uint32_t>(out_.size());
    for (const auto& e : entries_) {
        put32(out_, 0x02014b50); put16(out_, 20); put16(out_, 20); put16(out_, 0x0800); put16(out_, 0);
        put16(out_, 0); put16(out_, 0x21); put32(out_, e.crc); put32(out_, e.size); put32(out_, e.size);
        put16(out_, static_cast<std::uint16_t>(e.name.size())); put16(out_, 0); put16(out_, 0); put16(out_, 0); put16(out_, 0);
        put32(out_, 0); put32(out_, e.offset);
        out_ += e.name;
    }
    const auto cd_size = static_cast<std::uint32_t>(out_.size()) - cd;
    put32(out_, 0x06054b50); put16(out_, 0); put16(out_, 0);
    put16(out_, static_cast<std::uint16_t>(entries_.size())); put16(out_, static_cast<std::uint16_t>(entries_.size()));
    put32(out_, cd_size); put32(out_, cd); put16(out_, 0);
    write_file_atomic(file_, out_);
    done_ = true;
}

std::string json_escape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
    return out;
}

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string read_text(const fs::path& p, std::size_t max_bytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(f.tellg());
    const std::uint64_t start = size > max_bytes ? size - max_bytes : 0;
    f.seekg(static_cast<std::streamoff>(start));
    std::string s(static_cast<std::size_t>(size - start), '\0');
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    s.resize(static_cast<std::size_t>(f.gcount()));
    return s;
}

} // namespace reborn::launcher
