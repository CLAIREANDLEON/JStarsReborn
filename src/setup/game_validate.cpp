#include "reborn/game_validate.hpp"
#include "reborn/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace reborn::setup {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t max_psf_entries = 1024;
constexpr std::size_t max_key_length = 64;
constexpr std::uint64_t max_eboot_bytes = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t max_directory_entries = 10000;
constexpr std::size_t max_manifest_bytes = 1024 * 1024;
constexpr std::size_t max_manifest_entries = 4096;

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Strict UTF-8 (no overlongs, surrogates, or code points above U+10FFFF).
bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t extra;
        unsigned char low = 0x80, high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c == 0xE0) { extra = 2; low = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) extra = 2;
        else if (c == 0xED) { extra = 2; high = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) extra = 2;
        else if (c == 0xF0) { extra = 3; low = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) extra = 3;
        else if (c == 0xF4) { extra = 3; high = 0x8F; }
        else return false;
        if (i + extra >= s.size()) return false; // sequence would run past the end
        const auto second = static_cast<unsigned char>(s[i + 1]);
        if (second < low || second > high) return false;
        for (std::size_t k = 2; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(s[i + k]);
            if (cont < 0x80 || cont > 0xBF) return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string json_escape(std::string_view in) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7F) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
    }
    return out;
}
std::string json_string(std::string_view in) { return "\"" + json_escape(in) + "\""; }

std::string lower_ascii(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::string path_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

bool is_title_id(std::string_view v) {
    if (v.size() != 9) return false;
    for (std::size_t i = 0; i < 4; ++i) if (v[i] < 'A' || v[i] > 'Z') return false;
    for (std::size_t i = 4; i < 9; ++i) if (v[i] < '0' || v[i] > '9') return false;
    return true;
}
bool is_app_ver(std::string_view v) {
    return v.size() == 5 && std::isdigit(static_cast<unsigned char>(v[0])) && std::isdigit(static_cast<unsigned char>(v[1])) &&
           v[2] == '.' && std::isdigit(static_cast<unsigned char>(v[3])) && std::isdigit(static_cast<unsigned char>(v[4]));
}
bool is_sha256(std::string_view v) {
    return v.size() == 64 && std::all_of(v.begin(), v.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Case-insensitive child lookup. Returns {} if absent. Throws nothing; sets 'problem' when
// the match is a symbolic link (refused) so callers can report it.
fs::path find_child(const fs::path& directory, std::string_view lowercase_name, std::string& problem) {
    std::error_code ec;
    fs::directory_iterator it(directory, fs::directory_options::none, ec), end;
    if (ec) return {};
    std::size_t seen = 0;
    for (; it != end; it.increment(ec)) {
        if (ec) return {};
        if (++seen > max_directory_entries) { problem = "directory has more than 10000 entries; lookup abandoned"; return {}; }
        if (lower_ascii(path_utf8(it->path().filename())) == lowercase_name) {
            std::error_code status_ec;
            const auto status = fs::symlink_status(it->path(), status_ec);
            if (status_ec) { problem = "cannot inspect " + std::string(lowercase_name); return {}; }
            if (fs::is_symlink(status)) { problem = std::string(lowercase_name) + " is a symbolic link; links are refused"; return {}; }
            return it->path();
        }
    }
    return {};
}

bool read_all(const fs::path& file, std::uint64_t limit, std::vector<std::uint8_t>& out, std::string& problem) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) { problem = "cannot determine size of " + path_utf8(file.filename()); return false; }
    if (size > limit) { problem = path_utf8(file.filename()) + " exceeds the " + std::to_string(limit) + "-byte limit"; return false; }
    std::ifstream in(file, std::ios::binary);
    if (!in) { problem = "cannot open " + path_utf8(file.filename()); return false; }
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (static_cast<std::uint64_t>(in.gcount()) != size) { problem = "short read of " + path_utf8(file.filename()); return false; }
    return true;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    for (;;) {
        const auto tab = line.find('\t', start);
        if (tab == std::string_view::npos) { fields.push_back(line.substr(start)); break; }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}
}

const PsfEntry* PsfFile::find(std::string_view key) const noexcept {
    for (const auto& e : entries) if (e.key == key) return &e;
    return nullptr;
}

PsfFile parse_psf(std::span<const std::uint8_t> b) {
    if (b.size() > max_psf_bytes) throw std::runtime_error("PARAM.SFO exceeds the 1 MiB limit");
    if (b.size() < 20) throw std::runtime_error("PARAM.SFO is truncated (header needs 20 bytes)");
    if (le32(b.data()) != 0x46535000u) throw std::runtime_error("PARAM.SFO magic is not \\0PSF");
    if (le32(b.data() + 4) != 0x00000101u) throw std::runtime_error("PARAM.SFO version is not 1.1");
    const std::uint64_t key_start = le32(b.data() + 8);
    const std::uint64_t data_start = le32(b.data() + 12);
    const std::uint64_t count = le32(b.data() + 16);
    if (count > max_psf_entries) throw std::runtime_error("PARAM.SFO declares more than 1024 entries");
    const std::uint64_t index_end = 20 + count * 16;
    if (index_end > b.size()) throw std::runtime_error("PARAM.SFO index table exceeds the file");
    if (key_start < index_end || key_start > data_start || data_start > b.size())
        throw std::runtime_error("PARAM.SFO table offsets are inconsistent");
    const std::uint64_t key_table_size = data_start - key_start;
    const std::uint64_t data_table_size = b.size() - data_start;

    PsfFile file;
    std::set<std::string> seen;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* e = b.data() + 20 + i * 16;
        const std::uint16_t key_offset = le16(e);
        const std::uint16_t format = le16(e + 2);
        const std::uint32_t length = le32(e + 4);
        const std::uint32_t max_length = le32(e + 8);
        const std::uint32_t data_offset = le32(e + 12);

        if (key_offset >= key_table_size) throw std::runtime_error("PARAM.SFO key offset lies outside the key table");
        const std::uint8_t* key_begin = b.data() + key_start + key_offset;
        const std::uint64_t room = key_table_size - key_offset;
        std::size_t key_length = 0;
        while (key_length < room && key_length <= max_key_length && key_begin[key_length] != 0) ++key_length;
        if (key_length == 0) throw std::runtime_error("PARAM.SFO contains an empty key");
        if (key_length > max_key_length) throw std::runtime_error("PARAM.SFO key exceeds 64 characters");
        if (key_length == room) throw std::runtime_error("PARAM.SFO key is not NUL-terminated inside the key table");
        PsfEntry entry;
        entry.key.assign(reinterpret_cast<const char*>(key_begin), key_length);
        for (unsigned char c : entry.key)
            if (c < 0x21 || c > 0x7E) throw std::runtime_error("PARAM.SFO key contains a non-printable character");
        if (!seen.insert(entry.key).second) throw std::runtime_error("PARAM.SFO contains duplicate key " + entry.key);

        if (length > max_length) throw std::runtime_error("PARAM.SFO entry length exceeds its reserved length");
        if (static_cast<std::uint64_t>(data_offset) + max_length > data_table_size)
            throw std::runtime_error("PARAM.SFO entry data lies outside the data table");
        const std::uint8_t* data = b.data() + data_start + data_offset;
        if (format == 0x0404) {
            if (length != 4 || max_length != 4) throw std::runtime_error("PARAM.SFO integer entry must be exactly 4 bytes");
            entry.kind = PsfKind::Integer;
            entry.integer = le32(data);
        } else if (format == 0x0204 || format == 0x0004) {
            entry.kind = format == 0x0204 ? PsfKind::Utf8 : PsfKind::Utf8Special;
            std::size_t used = 0;
            while (used < length && data[used] != 0) ++used;
            entry.text.assign(reinterpret_cast<const char*>(data), used);
            if (!valid_utf8(entry.text)) throw std::runtime_error("PARAM.SFO text for " + entry.key + " is not valid UTF-8");
        } else {
            throw std::runtime_error("PARAM.SFO entry " + entry.key + " has an unknown data format");
        }
        file.entries.push_back(std::move(entry));
    }
    return file;
}

std::vector<SupportedBuild> parse_supported_versions(std::string_view text) {
    if (text.size() > max_manifest_bytes) throw std::runtime_error("Supported-version manifest exceeds 1 MiB");
    if (!valid_utf8(text)) throw std::runtime_error("Supported-version manifest is not valid UTF-8");
    std::vector<SupportedBuild> builds;
    std::set<std::string> keys;
    std::size_t line_number = 0, position = 0;
    while (position <= text.size()) {
        const auto newline = text.find('\n', position);
        auto line = text.substr(position, newline == std::string_view::npos ? std::string_view::npos : newline - position);
        position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const auto where = " (manifest line " + std::to_string(line_number) + ")";
        const auto fields = split_tabs(line);
        if (fields.size() < 4 || fields.size() > 5) throw std::runtime_error("Expected 4 or 5 tab-separated fields" + where);
        SupportedBuild build;
        build.title_id = std::string(fields[0]);
        build.app_ver = std::string(fields[1]);
        build.eboot_sha256 = fields[2] == "-" ? std::string() : std::string(fields[2]);
        build.status = std::string(fields[3]);
        if (fields.size() == 5) build.notes = std::string(fields[4]);
        if (!is_title_id(build.title_id)) throw std::runtime_error("TITLE_ID must be four capitals and five digits" + where);
        if (!is_app_ver(build.app_ver)) throw std::runtime_error("APP_VER must look like 01.00" + where);
        if (!build.eboot_sha256.empty() && !is_sha256(build.eboot_sha256))
            throw std::runtime_error("EBOOT_SHA256 must be 64 lowercase hex digits or '-'" + where);
        if (build.status != "verified" && build.status != "untested")
            throw std::runtime_error("Status must be 'verified' or 'untested'" + where);
        if (!keys.insert(build.title_id + "|" + build.app_ver + "|" + build.eboot_sha256).second)
            throw std::runtime_error("Duplicate build entry" + where);
        if (builds.size() >= max_manifest_entries) throw std::runtime_error("Manifest exceeds 4096 entries");
        builds.push_back(std::move(build));
    }
    return builds;
}

namespace {
ElfFacts elf_facts(const std::uint8_t* elf) {
    ElfFacts facts;
    facts.is_64bit = elf[4] == 2;
    facts.little_endian = elf[5] == 1;
    facts.type = le16(elf + 16);
    facts.machine = le16(elf + 18);
    return facts;
}
bool is_ps4_like(const ElfFacts& f) { return f.is_64bit && f.little_endian && f.machine == 0x3E; }
}

// SELF layout (public format facts, matching the shadPS4 loader's reading of it):
// 32-byte header {u32 magic 0x1D3D154F; u8 version, mode, endian, attributes, category,
// program_type; u16 pad; u16 header_size; u16 meta_size; u32 file_size; u32 pad;
// u16 segment_count @24; u16; u32 pad}, then segment_count 32-byte entries whose first u64
// holds flags (bit 1 = encrypted), then the embedded ELF header. Nothing here decrypts.
ExecutableHeader inspect_executable_header(std::span<const std::uint8_t> head) {
    ExecutableHeader out;
    if (head.size() > executable_head_bytes) head = head.first(executable_head_bytes);
    const auto starts = [&](std::initializer_list<std::uint8_t> sig) {
        return head.size() >= sig.size() && std::equal(sig.begin(), sig.end(), head.begin());
    };
    if (starts({0x7F, 'E', 'L', 'F'})) {
        out.kind = ExecutableKind::Elf;
        if (head.size() < 20) { out.warnings.push_back("eboot.bin has an ELF signature but a truncated header"); return out; }
        out.elf = elf_facts(head.data());
        if (!is_ps4_like(*out.elf))
            out.warnings.push_back("eboot.bin is an ELF but not a 64-bit little-endian x86-64 image; it is not a PS4 executable");
        return out;
    }
    if (starts({0x4F, 0x15, 0x3D, 0x1D})) {
        out.kind = ExecutableKind::Ps4Self;
        SelfFacts self;
        if (head.size() < 32) {
            out.self = self;
            out.problems.push_back("eboot.bin has a SELF signature but a truncated SELF header");
            return out;
        }
        self.segment_count = le16(head.data() + 24);
        const std::uint64_t elf_offset = 32 + 32ull * self.segment_count;
        const std::uint64_t listed = std::min<std::uint64_t>(self.segment_count, (head.size() - 32) / 32);
        for (std::uint64_t i = 0; i < listed; ++i) {
            const std::uint32_t flags_low = le32(head.data() + 32 + i * 32);
            if (flags_low & 2u) ++self.segments_flagged_encrypted;
        }
        if (elf_offset + 20 <= head.size() && head[elf_offset] == 0x7F && head[elf_offset + 1] == 'E' &&
            head[elf_offset + 2] == 'L' && head[elf_offset + 3] == 'F') {
            self.embedded_elf_readable = true;
            out.elf = elf_facts(head.data() + elf_offset);
            if (!is_ps4_like(*out.elf))
                out.warnings.push_back("The ELF embedded in eboot.bin is not a 64-bit little-endian x86-64 image");
            out.warnings.push_back("eboot.bin is a SELF container with a readable embedded ELF header (" +
                                   std::to_string(self.segment_count) + " segments, " +
                                   std::to_string(self.segments_flagged_encrypted) +
                                   " carrying the 'encrypted' flag). Segment payloads were not decrypted or examined; "
                                   "the flag alone does not establish whether they are plaintext.");
        } else if (elf_offset + 20 > executable_head_bytes) {
            out.problems.push_back("eboot.bin declares " + std::to_string(self.segment_count) +
                                   " SELF segments, placing its ELF header beyond the inspected 16 KiB; not inspected");
        } else {
            out.problems.push_back("eboot.bin is a SELF container whose embedded ELF header is not readable (encrypted or "
                                   "truncated). This project does not decrypt; a runtime needs a dump decrypted on the owner's console.");
        }
        out.self = self;
        return out;
    }
    out.kind = ExecutableKind::Unknown;
    out.warnings.push_back("eboot.bin has an unrecognized signature");
    return out;
}

GameValidation validate_game_folder(const fs::path& folder, const std::vector<SupportedBuild>& supported, std::stop_token stop) {
    GameValidation r;
    r.folder = folder;
    const auto cancelled = [&] { if (stop.stop_requested()) throw std::runtime_error("Validation cancelled"); };

    std::error_code ec;
    const auto folder_status = fs::symlink_status(folder, ec);
    if (ec || !fs::is_directory(folder_status)) { r.problems.push_back("The selected path is not a folder"); return r; }
    if (fs::is_symlink(folder_status)) { r.problems.push_back("The selected folder is a symbolic link; links are refused"); return r; }

    // ---- sce_sys/param.sfo
    std::string problem;
    const auto sce_sys = find_child(folder, "sce_sys", problem);
    if (!problem.empty()) r.problems.push_back(problem);
    problem.clear();
    fs::path param;
    if (!sce_sys.empty()) {
        param = find_child(sce_sys, "param.sfo", problem);
        if (!problem.empty()) r.problems.push_back(problem);
        problem.clear();
    } else if (r.problems.empty()) {
        r.problems.push_back("sce_sys folder not found");
    }
    r.param_sfo_present = !param.empty();
    if (r.param_sfo_present) {
        std::vector<std::uint8_t> bytes;
        if (!read_all(param, max_psf_bytes, bytes, problem)) {
            r.problems.push_back(problem);
        } else {
            try {
                const auto psf = parse_psf(bytes);
                r.param_sfo_valid = true;
                const auto text = [&](const char* key) -> std::string {
                    const auto* e = psf.find(key);
                    return e && e->kind != PsfKind::Integer ? e->text : std::string();
                };
                r.title_id = text("TITLE_ID");
                r.content_id = text("CONTENT_ID");
                r.app_ver = text("APP_VER");
                r.title = text("TITLE");
                r.category = text("CATEGORY");
                if (const auto* sv = psf.find("SYSTEM_VER"); sv && sv->kind == PsfKind::Integer) r.system_ver = sv->integer;
                if (r.title_id.empty()) r.warnings.push_back("PARAM.SFO has no TITLE_ID");
                else if (!is_title_id(r.title_id)) r.warnings.push_back("TITLE_ID does not look like CUSA00000");
                if (r.app_ver.empty()) r.warnings.push_back("PARAM.SFO has no APP_VER");
                else if (!is_app_ver(r.app_ver)) r.warnings.push_back("APP_VER does not look like 01.00");
                if (r.category == "gp") {
                    std::string ignored;
                    if (!find_child(folder, "sce_discmap_patch.plt", ignored).empty())
                        r.warnings.push_back("CATEGORY is 'gp' and sce_discmap_patch.plt is present: this looks like a base game merged "
                                             "with its patch, so APP_VER is the patch version");
                    else
                        r.warnings.push_back("CATEGORY is 'gp' (patch) without sce_discmap_patch.plt: this may be a patch alone; "
                                             "it needs its base game merged in");
                } else if (!r.category.empty() && r.category != "gd") {
                    r.warnings.push_back("CATEGORY is '" + r.category + "', not 'gd' (base game) or 'gp' (patch)");
                }
            } catch (const std::runtime_error& error) {
                r.problems.push_back(std::string("param.sfo rejected: ") + error.what());
            }
        }
    } else if (!sce_sys.empty()) {
        r.problems.push_back("sce_sys/param.sfo not found");
    }
    cancelled();

    // ---- eboot.bin
    problem.clear();
    const auto eboot = find_child(folder, "eboot.bin", problem);
    if (!problem.empty()) r.problems.push_back(problem);
    r.eboot_present = !eboot.empty();
    if (!r.eboot_present) {
        if (problem.empty()) r.problems.push_back("eboot.bin not found");
    } else {
        std::error_code size_ec;
        r.eboot_bytes = fs::file_size(eboot, size_ec);
        std::ifstream in(eboot, std::ios::binary);
        if (size_ec || !in) {
            r.problems.push_back("eboot.bin cannot be read");
        } else if (r.eboot_bytes > max_eboot_bytes) {
            r.problems.push_back("eboot.bin exceeds the 2 GiB sanity limit; it was not hashed");
        } else {
            PortableSha256 hash;
            std::vector<std::uint8_t> block(1024 * 1024);
            std::uint64_t remaining = r.eboot_bytes;
            std::vector<std::uint8_t> head(executable_head_bytes);
            std::size_t head_size = 0;
            bool failed = false;
            while (remaining > 0) {
                cancelled();
                const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), remaining));
                in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(want));
                if (static_cast<std::size_t>(in.gcount()) != want) { failed = true; break; }
                if (head_size < head.size()) {
                    const auto copy = std::min(head.size() - head_size, want);
                    std::copy_n(block.begin(), copy, head.begin() + static_cast<std::ptrdiff_t>(head_size));
                    head_size += copy;
                }
                hash.update(block.data(), want);
                remaining -= want;
            }
            if (failed) {
                r.problems.push_back("eboot.bin ended before its reported size");
            } else {
                r.eboot_sha256 = PortableSha256::to_hex(hash.finish());
                auto header = inspect_executable_header(std::span<const std::uint8_t>(head.data(), head_size));
                r.executable = header.kind;
                r.elf = header.elf;
                r.self = header.self;
                for (auto& w : header.warnings) r.warnings.push_back(std::move(w));
                for (auto& p : header.problems) r.problems.push_back(std::move(p));
            }
        }
    }

    // ---- modules shipped with the game (names and sizes only; never loaded or parsed)
    {
        std::string ignored;
        const auto modules = find_child(folder, "sce_module", ignored);
        std::error_code module_ec;
        if (!modules.empty() && fs::is_directory(fs::symlink_status(modules, module_ec))) {
            std::size_t seen = 0;
            for (fs::directory_iterator it(modules, module_ec), end; !module_ec && it != end; it.increment(module_ec)) {
                if (++seen > max_directory_entries) break;
                std::error_code entry_ec;
                const auto status = fs::symlink_status(it->path(), entry_ec);
                if (entry_ec || !fs::is_regular_file(status)) continue;
                const auto size = fs::file_size(it->path(), entry_ec);
                r.bundled_modules.push_back({path_utf8(it->path().filename()), entry_ec ? 0 : size});
            }
            std::sort(r.bundled_modules.begin(), r.bundled_modules.end(),
                      [](const BundledModule& a, const BundledModule& b) { return a.name < b.name; });
        }
    }

    // ---- compare with the supported-build manifest
    bool listed_title = false;
    for (const auto& build : supported) {
        if (build.title_id != r.title_id || r.title_id.empty()) continue;
        listed_title = true;
        if (build.app_ver != r.app_ver) continue;
        if (!build.eboot_sha256.empty() && build.eboot_sha256 != r.eboot_sha256) continue;
        if (build.status == "verified") { r.match = SupportMatch::Verified; break; }
        r.match = SupportMatch::Untested;
    }
    if (r.match == SupportMatch::Unknown && !r.title_id.empty()) {
        r.warnings.push_back(listed_title ? "This title is listed, but not this APP_VER or executable hash"
                                          : "This TITLE_ID is not in the supported-build manifest");
    }
    return r;
}

std::string validation_json(const GameValidation& r) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    const char* kind = "missing";
    switch (r.executable) {
    case ExecutableKind::Elf: kind = "elf"; break;
    case ExecutableKind::Ps4Self: kind = "self"; break;
    case ExecutableKind::Unknown: kind = "unknown"; break;
    case ExecutableKind::Missing: break;
    }
    const char* match = r.match == SupportMatch::Verified ? "verified" : r.match == SupportMatch::Untested ? "untested" : "unknown";
    std::string j = "{\n  \"schema\":\"reborn.game-validation.v1\",\n";
    j += "  \"folder\":" + json_string(path_utf8(r.folder)) + ",\n";
    j += std::string("  \"param_sfo\":{\"present\":") + b(r.param_sfo_present) + ",\"valid\":" + b(r.param_sfo_valid) +
         ",\"title_id\":" + json_string(r.title_id) + ",\"content_id\":" + json_string(r.content_id) + ",\"app_ver\":" + json_string(r.app_ver) +
         ",\"title\":" + json_string(r.title) + ",\"category\":" + json_string(r.category) + ",\"system_ver\":" +
         (r.system_ver ? std::to_string(*r.system_ver) : std::string("null")) + "},\n";
    j += std::string("  \"executable\":{\"present\":") + b(r.eboot_present) + ",\"kind\":\"" + kind + "\",\"bytes\":" +
         std::to_string(r.eboot_bytes) + ",\"sha256\":" + json_string(r.eboot_sha256) + ",\"elf\":";
    if (r.elf) {
        j += std::string("{\"is_64bit\":") + b(r.elf->is_64bit) + ",\"little_endian\":" + b(r.elf->little_endian) +
             ",\"type\":" + std::to_string(r.elf->type) + ",\"machine\":" + std::to_string(r.elf->machine) + "}";
    } else {
        j += "null";
    }
    j += ",\"self\":";
    if (r.self) {
        j += std::string("{\"segment_count\":") + std::to_string(r.self->segment_count) + ",\"segments_flagged_encrypted\":" +
             std::to_string(r.self->segments_flagged_encrypted) + ",\"embedded_elf_readable\":" + b(r.self->embedded_elf_readable) +
             ",\"payloads_examined\":false}";
    } else {
        j += "null";
    }
    j += "},\n";
    j += "  \"bundled_modules\":[";
    for (std::size_t i = 0; i < r.bundled_modules.size(); ++i)
        j += std::string(i ? "," : "") + "{\"name\":" + json_string(r.bundled_modules[i].name) + ",\"bytes\":" +
             std::to_string(r.bundled_modules[i].bytes) + "}";
    j += "],\n";
    j += std::string("  \"supported_build\":\"") + match + "\",\n";
    j += std::string("  \"game_import_supported\":") + b(r.game_import_supported) + ",\n";
    j += std::string("  \"structurally_valid\":") + b(r.ok()) + ",\n";
    for (const auto& [name, list] : {std::pair<const char*, const std::vector<std::string>*>{"problems", &r.problems},
                                     std::pair<const char*, const std::vector<std::string>*>{"warnings", &r.warnings}}) {
        j += std::string("  \"") + name + "\":[";
        for (std::size_t i = 0; i < list->size(); ++i) j += (i ? "," : "") + json_string((*list)[i]);
        j += std::string("]") + (std::string_view(name) == "problems" ? ",\n" : "\n");
    }
    j += "}\n";
    return j;
}
}
