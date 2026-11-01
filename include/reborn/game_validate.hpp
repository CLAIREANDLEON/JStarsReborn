#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Read-only validation of a user-supplied, already-decrypted PlayStation 4 dump folder.
// Nothing here decrypts, unpacks or executes game content, and no function reports a
// game as playable: that claim needs a runtime that does not exist yet.
namespace reborn::setup {

// ---- PARAM.SFO ("PSF") -------------------------------------------------------------
// Layout follows the public PSF description (little-endian header, index table, key
// table, data table). The parser is strict: any inconsistency throws std::runtime_error
// naming the cause. It has been exercised with synthetic files and mutation fuzzing
// only; it has not been run against a real J-Stars PARAM.SFO.
enum class PsfKind { Utf8Special, Utf8, Integer };
struct PsfEntry {
    std::string key;
    PsfKind kind = PsfKind::Utf8;
    std::string text;         // Utf8 / Utf8Special (cut at the first NUL)
    std::uint32_t integer = 0; // Integer
};
struct PsfFile {
    std::vector<PsfEntry> entries;
    [[nodiscard]] const PsfEntry* find(std::string_view key) const noexcept;
};
inline constexpr std::size_t max_psf_bytes = 1024 * 1024;
[[nodiscard]] PsfFile parse_psf(std::span<const std::uint8_t> bytes);

// ---- Supported-build manifest --------------------------------------------------------
// Tab-separated, one build per line, '#' comments:
//   TITLE_ID <TAB> APP_VER <TAB> EBOOT_SHA256-or-'-' <TAB> verified|untested [<TAB> notes]
// "verified" means a person played that exact build through the runtime. Nothing is
// verified yet, so the shipped manifest contains no entries.
struct SupportedBuild {
    std::string title_id;
    std::string app_ver;
    std::string eboot_sha256; // empty = any executable hash
    std::string status;       // "verified" | "untested"
    std::string notes;
};
[[nodiscard]] std::vector<SupportedBuild> parse_supported_versions(std::string_view text);

// ---- Folder validation ---------------------------------------------------------------
enum class ExecutableKind { Missing, Elf, Ps4Self, Unknown };
struct ElfFacts {
    bool is_64bit = false;
    bool little_endian = false;
    std::uint16_t type = 0;
    std::uint16_t machine = 0;
};
struct SelfFacts {
    std::uint16_t segment_count = 0;
    std::uint16_t segments_flagged_encrypted = 0; // header flag only; payloads are never examined
    bool embedded_elf_readable = false;
};
enum class SupportMatch { Unknown, Untested, Verified };
struct BundledModule {
    std::string name;    // file in sce_module/ (e.g. libc.prx); recorded, never loaded
    std::uint64_t bytes = 0;
};

// Classifies the first bytes of eboot.bin (at most executable_head_bytes are used):
// plain ELF, SELF container (with its embedded ELF header if readable), or unknown.
// Pure and bounds-checked; never decrypts.
inline constexpr std::size_t executable_head_bytes = 16384;
struct ExecutableHeader {
    ExecutableKind kind = ExecutableKind::Unknown;
    std::optional<ElfFacts> elf;
    std::optional<SelfFacts> self;
    std::vector<std::string> warnings;
    std::vector<std::string> problems;
};
[[nodiscard]] ExecutableHeader inspect_executable_header(std::span<const std::uint8_t> head);

struct GameValidation {
    std::filesystem::path folder;
    bool param_sfo_present = false;
    bool param_sfo_valid = false;
    std::string title_id, content_id, app_ver, title, category;
    std::optional<std::uint32_t> system_ver;
    bool eboot_present = false;
    ExecutableKind executable = ExecutableKind::Missing;
    std::optional<ElfFacts> elf;
    std::optional<SelfFacts> self;
    std::uint64_t eboot_bytes = 0;
    std::string eboot_sha256;
    std::vector<BundledModule> bundled_modules;
    SupportMatch match = SupportMatch::Unknown;
    // Always false: this build contains no code path that runs game content.
    bool game_import_supported = false;
    std::vector<std::string> problems; // structural failures
    std::vector<std::string> warnings; // observations that do not block validation
    [[nodiscard]] bool ok() const noexcept { return problems.empty(); }
};

// Looks for sce_sys/param.sfo and eboot.bin (names matched case-insensitively), refuses
// symbolic links, bounds every read, and never writes. Cancellation throws
// std::runtime_error. Problems and warnings are returned, not thrown.
[[nodiscard]] GameValidation validate_game_folder(const std::filesystem::path& folder,
                                                  const std::vector<SupportedBuild>& supported,
                                                  std::stop_token stop = {});
[[nodiscard]] std::string validation_json(const GameValidation& result);
}
