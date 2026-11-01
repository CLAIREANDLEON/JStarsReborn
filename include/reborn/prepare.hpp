#pragma once
// Native port of tools/prepare (prepare.py + link_modules.py + nid_names.py): turns the user's own decrypted
// dump (eboot.bin and the game's bundled sce_module/libc.prx and libSceFios2.prx, all plaintext SELF or ELF)
// into the runtime's boot image (boot-linked.bin, format JSTBOOT5) and the diagnostic name table
// (import-names.tsv). The output is byte-identical to the Python tools for the same inputs (checked by
// tools/prepare_native_check.sh). Nothing is decrypted; the game folder is only read.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace reborn::prep {

// One input file of the preparation, with its content hash (lower-case hex SHA-256).
struct InputFile {
    std::string relative; // e.g. "eboot.bin", "sce_module/libc.prx"
    std::uint64_t bytes = 0;
    std::string sha256;
};

struct PrepareResult {
    std::vector<InputFile> inputs;
    std::uint64_t image_bytes = 0;
    std::size_t imports = 0, bindings = 0, relocations = 0, segments = 0;
    std::string boot_sha256; // of the written boot-linked.bin
};

// Progress: fraction 0..1 and a short human-readable step name.
using Progress = std::function<void(double fraction, const std::string& step)>;

// The bundled modules that are linked into the boot image, in link order.
const std::vector<std::string>& linked_modules();

// SHA-256 of the files the preparation reads (eboot.bin, sce_module/<linked modules>, sce_sys/param.sfo).
// Throws std::runtime_error naming the missing/unreadable file.
std::vector<InputFile> hash_inputs(const std::filesystem::path& game, std::stop_token stop = {});

// Runs the whole preparation into out_dir (created if needed). Files are written as *.tmp and renamed only
// when complete, so a cancelled or failed run never leaves a half-written boot image. Throws
// std::runtime_error with a readable reason (or "cancelled") on failure.
PrepareResult prepare(const std::filesystem::path& game, const std::filesystem::path& out_dir, const Progress& progress = {},
                      std::stop_token stop = {});

// The PS4 NID of a symbol name (SHA-1 with the fixed suffix, first 8 bytes reversed, base64 without padding,
// '/' replaced by '-'), as used by the runtime and the import tables.
std::string nid(const std::string& name);

} // namespace reborn::prep
