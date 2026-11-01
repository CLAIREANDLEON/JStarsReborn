// Portable tests for SHA-256, PARAM.SFO parsing, the supported-build manifest and
// read-only game-folder validation. No Windows headers: this file is built by CTest on
// Windows and by tools/portable_tests.sh under ASan/UBSan on any C++20 host.
// All fixtures are synthetic. None of them is, or is derived from, game data.
#include "reborn/game_validate.hpp"
#include "reborn/sha256.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace reborn::setup;

namespace {
int checks = 0, skips = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) throw std::runtime_error(what);
}
template <class F> void rejects(F fn, const std::string& what) {
    bool thrown = false;
    try { fn(); } catch (const std::runtime_error&) { thrown = true; }
    check(thrown, what);
}

// ---------------------------------------------------------------- SHA-256
void sha_tests(const char* self_check_file, const char* self_check_hash) {
    check(reborn::sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty");
    check(reborn::sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
    const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check(reborn::sha256_hex(two_block.data(), two_block.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 448-bit vector");
    const std::string million(1000000, 'a');
    check(reborn::sha256_hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA-256 million-a vector");
    // Chunked updates across every block-boundary case must equal one-shot hashing.
    std::string data;
    for (int i = 0; i < 300; ++i) data += static_cast<char>((i * 37 + 11) & 0xFF);
    for (std::size_t length : {0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u, 300u}) {
        const auto expected = reborn::sha256_hex(data.data(), length);
        for (std::size_t chunk : {1u, 3u, 7u, 63u, 64u, 65u}) {
            reborn::PortableSha256 h;
            for (std::size_t at = 0; at < length; at += chunk) h.update(data.data() + at, std::min(chunk, length - at));
            check(reborn::PortableSha256::to_hex(h.finish()) == expected, "Chunked SHA-256 length " + std::to_string(length));
        }
    }
    // Optional real-file cross-check against an independently produced digest.
    if (self_check_file && self_check_hash) {
        std::ifstream in(self_check_file, std::ios::binary);
        check(static_cast<bool>(in), "Cross-check file readable");
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(reborn::sha256_hex(bytes.data(), bytes.size()) == self_check_hash, "SHA-256 matches independently recorded digest");
    }
}

// ---------------------------------------------------------------- PSF builder
struct Field {
    std::string key;
    std::uint16_t format;
    std::string value; // raw bytes for text, 4 LE bytes for integer
    std::uint32_t max_length;
};
void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) { v[at] = x & 0xFF; v[at + 1] = x >> 8; }
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
Field text(const std::string& key, const std::string& value, std::uint32_t reserve = 0) {
    const auto used = static_cast<std::uint32_t>(value.size() + 1);
    return {key, 0x0204, value + std::string(1, '\0'), std::max(used, reserve)};
}
Field integer(const std::string& key, std::uint32_t x) {
    std::string bytes(4, '\0');
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>(x >> (8 * i));
    return {key, 0x0404, bytes, 4};
}
std::vector<std::uint8_t> build_psf(const std::vector<Field>& fields) {
    std::string keys;
    std::vector<std::uint16_t> key_offsets;
    for (const auto& f : fields) { key_offsets.push_back(static_cast<std::uint16_t>(keys.size())); keys += f.key + std::string(1, '\0'); }
    while (keys.size() % 4) keys += '\0';
    const std::size_t key_start = 20 + fields.size() * 16;
    const std::size_t data_start = key_start + keys.size();
    std::size_t data_size = 0;
    for (const auto& f : fields) data_size += f.max_length;
    std::vector<std::uint8_t> out(data_start + data_size, 0);
    put32(out, 0, 0x46535000); put32(out, 4, 0x101);
    put32(out, 8, static_cast<std::uint32_t>(key_start)); put32(out, 12, static_cast<std::uint32_t>(data_start));
    put32(out, 16, static_cast<std::uint32_t>(fields.size()));
    std::memcpy(out.data() + key_start, keys.data(), keys.size());
    std::size_t data_at = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        const std::size_t e = 20 + i * 16;
        put16(out, e, key_offsets[i]); put16(out, e + 2, f.format);
        put32(out, e + 4, static_cast<std::uint32_t>(f.value.size())); put32(out, e + 8, f.max_length);
        put32(out, e + 12, static_cast<std::uint32_t>(data_at));
        std::memcpy(out.data() + data_start + data_at, f.value.data(), f.value.size());
        data_at += f.max_length;
    }
    return out;
}
// Synthetic metadata. The title ID is an obviously fictitious placeholder.
std::vector<Field> sample_fields() {
    return {text("APP_VER", "01.00", 8), text("CATEGORY", "gd", 4),
            text("CONTENT_ID", "XX0000-ZZZZ99999_00-SYNTHETICFIXTURE", 48), integer("SYSTEM_VER", 0x01700000),
            text("TITLE", "Synthetic \xe6\x97\xa5\xe6\x9c\xac Fixture", 128), text("TITLE_ID", "ZZZZ99999", 12)};
}

void psf_tests() {
    const auto good = build_psf(sample_fields());
    const auto psf = parse_psf(good);
    check(psf.entries.size() == 6, "PSF entry count");
    check(psf.find("TITLE_ID") && psf.find("TITLE_ID")->text == "ZZZZ99999", "PSF TITLE_ID");
    check(psf.find("APP_VER")->text == "01.00", "PSF APP_VER");
    check(psf.find("SYSTEM_VER")->kind == PsfKind::Integer && psf.find("SYSTEM_VER")->integer == 0x01700000, "PSF integer");
    check(psf.find("TITLE")->text == "Synthetic \xe6\x97\xa5\xe6\x9c\xac Fixture", "PSF UTF-8 title preserved");
    check(psf.find("MISSING") == nullptr, "PSF absent key");

    // Every truncation must be rejected with a diagnostic, never read out of bounds.
    for (std::size_t n = 0; n < good.size(); ++n) {
        const std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        rejects([&] { (void)parse_psf(cut); }, "Truncated PSF accepted at " + std::to_string(n));
    }
    auto mutate = [&](auto change, const std::string& what) {
        auto bad = good;
        change(bad);
        rejects([&] { (void)parse_psf(bad); }, what);
    };
    mutate([](auto& v) { v[1] = 'Q'; }, "Bad magic");
    mutate([](auto& v) { put32(v, 4, 0x102); }, "Bad version");
    mutate([](auto& v) { put32(v, 16, 0xFFFFFFFF); }, "Huge entry count");
    mutate([](auto& v) { put32(v, 16, 1025); }, "Entry count above cap");
    mutate([](auto& v) { put32(v, 8, 0xFFFFFFF0); }, "Key table offset overflow");
    mutate([](auto& v) { put32(v, 12, 0xFFFFFFF0); }, "Data table offset overflow");
    mutate([](auto& v) { put32(v, 8, 4); }, "Key table overlapping index");
    mutate([](auto& v) { put16(v, 20, 0xFFFF); }, "Key offset outside table");
    mutate([](auto& v) { put32(v, 20 + 4, 1000); }, "Length above reserved");
    mutate([](auto& v) { put32(v, 20 + 8, 0xFFFFFFFF); put32(v, 20 + 4, 5); }, "Reserved length overflow");
    mutate([](auto& v) { put32(v, 20 + 12, 0xFFFFFFFF); }, "Data offset overflow");
    mutate([](auto& v) { put16(v, 20 + 2, 0x0999); }, "Unknown data format");
    {
        auto fields = sample_fields();
        fields.push_back(text("TITLE_ID", "ZZZZ00000", 12));
        rejects([&] { (void)parse_psf(build_psf(fields)); }, "Duplicate key");
    }
    {
        std::vector<Field> fields{{"BAD", 0x0404, std::string("\x01\x02", 2), 4}};
        rejects([&] { (void)parse_psf(build_psf(fields)); }, "Short integer");
    }
    for (const char* invalid : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe6\x97", "\xff", "\xe0\x80\x80"}) {
        std::vector<Field> fields{text("TITLE", std::string("ok") + invalid, 16)};
        rejects([&] { (void)parse_psf(build_psf(fields)); }, "Invalid UTF-8 accepted");
    }
    {
        std::vector<Field> fields{text("K EY", "x")};
        rejects([&] { (void)parse_psf(build_psf(fields)); }, "Key with space accepted");
        std::vector<Field> longkey{text(std::string(65, 'K'), "x")};
        rejects([&] { (void)parse_psf(build_psf(longkey)); }, "65-character key accepted");
        std::vector<Field> okkey{text(std::string(64, 'K'), "x")};
        check(parse_psf(build_psf(okkey)).entries.size() == 1, "64-character key accepted");
    }
    {
        // Key table with no NUL terminator before the data table.
        auto bad = good;
        const std::uint32_t key_start = 20 + 6 * 16;
        const std::uint32_t data_start = bad[12] | (bad[13] << 8);
        for (std::uint32_t i = key_start; i < data_start; ++i) bad[i] = 'A';
        rejects([&] { (void)parse_psf(bad); }, "Unterminated key");
    }
    rejects([&] { (void)parse_psf(std::vector<std::uint8_t>(max_psf_bytes + 1, 0)); }, "Oversize PSF");

    // Deterministic mutation fuzzing. Accepted results must stay internally consistent;
    // rejection must be a std::runtime_error. Run under ASan/UBSan to catch memory errors.
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    std::size_t accepted = 0;
    for (int round = 0; round < 50000; ++round) {
        auto bytes = good;
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int k = 0; k < edits; ++k) {
            const auto at = static_cast<std::size_t>(next() % bytes.size());
            switch (next() % 3) {
            case 0: bytes[at] = static_cast<std::uint8_t>(next()); break;
            case 1: bytes[at] ^= static_cast<std::uint8_t>(1u << (next() % 8)); break;
            default: bytes.resize(at); break;
            }
            if (bytes.empty()) break;
        }
        try {
            const auto parsed = parse_psf(bytes);
            ++accepted;
            for (const auto& e : parsed.entries) {
                if (e.key.empty() || e.key.size() > 64) throw std::logic_error("fuzz: inconsistent accepted key");
            }
        } catch (const std::runtime_error&) {
        }
    }
    ++checks;
    std::cout << "fuzz: 50000 mutated PARAM.SFO inputs, " << accepted << " accepted, remainder rejected cleanly\n";
}

void manifest_tests() {
    const std::string sha(64, 'a');
    const auto builds = parse_supported_versions("# comment\n\nZZZZ99999\t01.00\t-\tuntested\tsynthetic\r\nZZZZ99999\t01.01\t" + sha + "\tverified\n");
    check(builds.size() == 2, "Manifest count");
    check(builds[0].eboot_sha256.empty() && builds[0].notes == "synthetic", "Manifest wildcard hash and notes");
    check(builds[1].status == "verified" && builds[1].eboot_sha256 == sha, "Manifest verified row");
    check(parse_supported_versions("").empty() && parse_supported_versions("# none\n").empty(), "Empty manifest");
    for (const std::string bad : {"ZZZZ99999\t01.00\t-", "zzzz99999\t01.00\t-\tuntested", "ZZZZ9999\t01.00\t-\tuntested",
                                  "ZZZZ99999\t1.0\t-\tuntested", "ZZZZ99999\t01.00\tABC\tuntested",
                                  "ZZZZ99999\t01.00\t-\tplayable", "ZZZZ99999\t01.00\t-\tuntested\tn\textra",
                                  "ZZZZ99999\t01.00\t-\tuntested\nZZZZ99999\t01.00\t-\tverified", "\xff"}) {
        rejects([&] { (void)parse_supported_versions(bad); }, "Bad manifest accepted: " + bad);
    }
}

// ---------------------------------------------------------------- folder validation
struct TempDir {
    fs::path root;
    TempDir() {
        auto base = fs::temp_directory_path();
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = base / ("reborn-gamecheck-" + std::to_string(tick));
        if (!fs::create_directory(root)) throw std::runtime_error("Fresh fixture directory required");
    }
    ~TempDir() { std::error_code ec; fs::remove_all(root, ec); }
};
void write_file(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Fixture write failed");
}
std::vector<std::uint8_t> fake_elf(std::uint16_t type, std::uint16_t machine) {
    std::vector<std::uint8_t> v(256, 0);
    v[0] = 0x7F; v[1] = 'E'; v[2] = 'L'; v[3] = 'F'; v[4] = 2; v[5] = 1; v[6] = 1;
    put16(v, 16, type); put16(v, 18, machine);
    return v;
}
// Synthetic SELF: 32-byte header, segment entries (first u64 = flags), then an ELF header
// (or garbage when elf_readable is false). Layout facts only; no real content.
std::vector<std::uint8_t> fake_self(std::uint16_t segments, std::vector<std::uint64_t> flags, bool elf_readable) {
    const std::size_t elf_at = 32 + 32u * segments;
    std::vector<std::uint8_t> v(elf_at + 256, 0);
    v[0] = 0x4F; v[1] = 0x15; v[2] = 0x3D; v[3] = 0x1D; v[4] = 0; v[6] = 1;
    put16(v, 24, segments);
    for (std::size_t i = 0; i < flags.size() && i < segments; ++i) {
        put32(v, 32 + i * 32, static_cast<std::uint32_t>(flags[i]));
        put32(v, 32 + i * 32 + 4, static_cast<std::uint32_t>(flags[i] >> 32));
    }
    if (elf_readable) {
        const auto elf = fake_elf(0xFE10, 0x3E);
        std::copy(elf.begin(), elf.begin() + 64, v.begin() + static_cast<std::ptrdiff_t>(elf_at));
    } else {
        for (std::size_t i = elf_at; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>(i * 131 + 7);
    }
    return v;
}
void executable_header_tests() {
    const auto readable = fake_self(3, {2, 2, 0}, true);
    const auto ok = inspect_executable_header(readable);
    check(ok.kind == ExecutableKind::Ps4Self && ok.self && ok.self->embedded_elf_readable && ok.problems.empty(), "Readable SELF");
    check(ok.self->segments_flagged_encrypted == 2, "Encrypted flags counted");

    const auto opaque = inspect_executable_header(fake_self(1, {2}, false));
    check(opaque.self && !opaque.self->embedded_elf_readable && !opaque.problems.empty(), "Unreadable SELF is a problem");
    check(opaque.problems[0].find("does not decrypt") != std::string::npos, "Unreadable SELF says no decryption");

    auto huge = fake_self(1, {0}, true);
    put16(huge, 24, 0xFFFF);
    const auto big = inspect_executable_header(huge);
    check(!big.problems.empty() && big.problems[0].find("beyond the inspected") != std::string::npos, "Huge segment count bounded");

    const std::vector<std::uint8_t> truncated(readable.begin(), readable.begin() + 20);
    check(!inspect_executable_header(truncated).problems.empty(), "Truncated SELF header");
    const std::vector<std::uint8_t> elf_short{0x7F, 'E', 'L', 'F', 2, 1};
    check(inspect_executable_header(elf_short).kind == ExecutableKind::Elf && !inspect_executable_header(elf_short).elf, "Truncated ELF");
    check(inspect_executable_header({}).kind == ExecutableKind::Unknown, "Empty head");
    std::vector<std::uint8_t> oversized(executable_head_bytes + 4096, 0);
    std::copy(readable.begin(), readable.end(), oversized.begin());
    check(inspect_executable_header(oversized).self->embedded_elf_readable, "Oversized head clipped safely");

    // Mutation fuzzing; under ASan/UBSan any out-of-bounds read aborts the run.
    std::uint64_t state = 0xD1B54A32D192ED03ull;
    auto next = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    for (int round = 0; round < 50000; ++round) {
        auto bytes = readable;
        for (int k = 0, n = 1 + static_cast<int>(next() % 4); k < n && !bytes.empty(); ++k) {
            const auto at = static_cast<std::size_t>(next() % bytes.size());
            switch (next() % 3) {
            case 0: bytes[at] = static_cast<std::uint8_t>(next()); break;
            case 1: bytes[at] ^= static_cast<std::uint8_t>(1u << (next() % 8)); break;
            default: bytes.resize(at); break;
            }
        }
        const auto h = inspect_executable_header(bytes);
        if (h.self && h.self->embedded_elf_readable && !h.elf) throw std::logic_error("fuzz: readable SELF without ELF facts");
    }
    ++checks;
    std::cout << "fuzz: 50000 mutated executable headers inspected without fault\n";
}
bool has(const std::vector<std::string>& list, const std::string& needle) {
    for (const auto& s : list) if (s.find(needle) != std::string::npos) return true;
    return false;
}

void validation_tests() {
    TempDir temp;
    const auto psf = build_psf(sample_fields());
    const auto elf = fake_elf(0xFE10, 0x3E);
    const auto elf_hash = reborn::sha256_hex(elf.data(), elf.size());

    {   // Well-formed synthetic dump, unknown to the manifest.
        const auto g = temp.root / "dump-elf";
        write_file(g / "sce_sys" / "param.sfo", psf);
        write_file(g / "eboot.bin", elf);
        const auto r = validate_game_folder(g, {});
        check(r.ok(), "Synthetic ELF dump structurally valid");
        check(r.param_sfo_valid && r.title_id == "ZZZZ99999" && r.app_ver == "01.00" && r.category == "gd", "Metadata extracted");
        check(r.system_ver && *r.system_ver == 0x01700000, "SYSTEM_VER extracted");
        check(r.executable == ExecutableKind::Elf && r.elf && r.elf->is_64bit && r.elf->little_endian &&
                  r.elf->machine == 0x3E && r.elf->type == 0xFE10, "ELF facts");
        check(r.eboot_bytes == elf.size() && r.eboot_sha256 == elf_hash, "Executable hash");
        check(r.match == SupportMatch::Unknown && has(r.warnings, "not in the supported-build manifest"), "Unknown build warned");
        check(!r.game_import_supported, "Validation never implies import support");

        check(validate_game_folder(g, {{"ZZZZ99999", "01.00", "", "untested", ""}}).match == SupportMatch::Untested, "Untested match");
        check(validate_game_folder(g, {{"ZZZZ99999", "01.00", elf_hash, "verified", ""}}).match == SupportMatch::Verified, "Verified hash match");
        const auto wrong = validate_game_folder(g, {{"ZZZZ99999", "01.00", std::string(64, '0'), "verified", ""}});
        check(wrong.match == SupportMatch::Unknown && has(wrong.warnings, "not this APP_VER or executable hash"), "Hash mismatch not matched");
        check(validate_game_folder(g, {{"ZZZZ99999", "01.01", "", "verified", ""}}).match == SupportMatch::Unknown, "Version mismatch not matched");

        const auto json = validation_json(r);
        for (const char* key : {"\"schema\":\"reborn.game-validation.v1\"", "\"title_id\":\"ZZZZ99999\"", "\"kind\":\"elf\"",
                                "\"game_import_supported\":false", "\"supported_build\":\"unknown\"", "\"structurally_valid\":true"})
            check(json.find(key) != std::string::npos, std::string("JSON contains ") + key);
        // tools/portable_tests.sh re-reads this with a real JSON parser.
        if (const char* out = std::getenv("REBORN_JSON_OUT")) std::ofstream(out, std::ios::binary) << json;
    }
    {   // Case-insensitive names and a SELF container whose embedded ELF header is readable.
        const auto g = temp.root / "dump-self";
        write_file(g / "SCE_SYS" / "PARAM.SFO", psf);
        write_file(g / "EBOOT.BIN", fake_self(2, {0x802, 0x800}, true));
        const auto r = validate_game_folder(g, {});
        check(r.ok() && r.param_sfo_valid, "Uppercase names found; readable SELF accepted");
        check(r.executable == ExecutableKind::Ps4Self && r.self && r.self->embedded_elf_readable, "Embedded ELF found");
        check(r.self->segment_count == 2 && r.self->segments_flagged_encrypted == 1, "SELF segment flags counted");
        check(r.elf && r.elf->type == 0xFE10 && r.elf->machine == 0x3E, "Embedded ELF facts");
        check(has(r.warnings, "not decrypted or examined"), "SELF payload state not overclaimed");
        const auto json = validation_json(r);
        check(json.find("\"embedded_elf_readable\":true") != std::string::npos && json.find("\"payloads_examined\":false") != std::string::npos,
              "SELF facts in JSON");
    }
    {   // Missing pieces, wrong architecture, invalid metadata, garbage executable.
        const auto empty = temp.root / "empty";
        fs::create_directory(empty);
        const auto r = validate_game_folder(empty, {});
        check(!r.ok() && has(r.problems, "sce_sys folder not found") && has(r.problems, "eboot.bin not found"), "Empty folder problems");

        const auto arm = temp.root / "arm";
        write_file(arm / "sce_sys" / "param.sfo", psf);
        write_file(arm / "eboot.bin", fake_elf(2, 0xB7));
        check(has(validate_game_folder(arm, {}).warnings, "not a PS4 executable"), "Non-x86-64 ELF warned");

        const auto broken = temp.root / "broken";
        auto bad_psf = psf; bad_psf[0] = 1;
        write_file(broken / "sce_sys" / "param.sfo", bad_psf);
        write_file(broken / "eboot.bin", {1, 2, 3, 4, 5});
        const auto b = validate_game_folder(broken, {});
        check(!b.ok() && has(b.problems, "param.sfo rejected") && b.executable == ExecutableKind::Unknown, "Broken dump problems");

        const auto patch = temp.root / "patch";
        auto fields = sample_fields(); fields[1] = text("CATEGORY", "gp", 4);
        write_file(patch / "sce_sys" / "param.sfo", build_psf(fields));
        write_file(patch / "eboot.bin", elf);
        check(has(validate_game_folder(patch, {}).warnings, "may be a patch alone"), "Lone patch warned");
        // Regression (observed on a real merged dump): 'gp' plus sce_discmap_patch.plt is a merged base+patch.
        write_file(patch / "sce_discmap_patch.plt", {0});
        const auto merged = validate_game_folder(patch, {});
        check(merged.ok() && has(merged.warnings, "merged") && !has(merged.warnings, "patch alone"), "Merged base+patch recognised");
        write_file(patch / "sce_module" / "libc.prx", {1, 2, 3});
        write_file(patch / "sce_module" / "libSceFios2.prx", {1});
        const auto with_modules = validate_game_folder(patch, {});
        check(with_modules.bundled_modules.size() == 2 && with_modules.bundled_modules[0].name == "libSceFios2.prx" &&
              with_modules.bundled_modules[1].bytes == 3, "Bundled modules listed by name and size");
        check(validation_json(with_modules).find("\"bundled_modules\":[{\"name\":\"libSceFios2.prx\"") != std::string::npos, "Modules in JSON");

        check(!validate_game_folder(temp.root / "does-not-exist", {}).ok(), "Missing folder rejected");
        check(!validate_game_folder(temp.root / "dump-elf" / "eboot.bin", {}).ok(), "File instead of folder rejected");
    }
    {   // Symbolic links are refused, when the host can actually create one.
        const auto g = temp.root / "linked";
        write_file(g / "sce_sys" / "param.sfo", psf);
        std::error_code ec;
        fs::create_symlink(temp.root / "dump-elf" / "eboot.bin", g / "eboot.bin", ec);
        if (ec || !fs::is_symlink(fs::symlink_status(g / "eboot.bin", ec))) {
            ++skips;
            std::cout << "SKIP symbolic-link fixture unavailable on this host\n";
        } else {
            const auto r = validate_game_folder(g, {});
            check(!r.ok() && has(r.problems, "symbolic link") && r.eboot_sha256.empty(), "Linked eboot refused and unread");
        }
    }
    {   // Cancellation is reported by exception, not as a silent partial result.
        std::stop_source stop;
        stop.request_stop();
        rejects([&] { (void)validate_game_folder(temp.root / "dump-elf", {}, stop.get_token()); }, "Cancellation honoured");
    }
}
}

int main(int argc, char** argv) {
    try {
        sha_tests(argc >= 3 ? argv[1] : nullptr, argc >= 3 ? argv[2] : nullptr);
        psf_tests();
        manifest_tests();
        executable_header_tests();
        validation_tests();
        std::cout << "PASS: " << checks << " game-validation checks, " << skips << " environment skips\n";
        return 0;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
