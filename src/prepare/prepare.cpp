// Native port of tools/prepare/{prepare.py, link_modules.py, nid_names.py} (adapted for JStarsReborn from
// HayatoG/iss_pc port/scripts, GPL-2.0-or-later). Same checks, same output bytes. The game folder is only read.
#include "reborn/prepare.hpp"
#include "reborn/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace reborn::prep {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

void check_stop(const std::stop_token& stop) {
    if (stop.stop_requested()) fail("cancelled");
}

// ---- bounds-checked little-endian reads (prepare.py: span / unpack) -------------------------------------------
void need(const Bytes& d, std::uint64_t off, std::uint64_t size) {
    if (off > d.size() || size > d.size() - off) {
        char b[160];
        std::snprintf(b, sizeof b, "out of bounds: %#llx+%#llx / %#llx", (unsigned long long)off, (unsigned long long)size,
                      (unsigned long long)d.size());
        fail(b);
    }
}
template <class T> T rd(const Bytes& d, std::uint64_t off) {
    need(d, off, sizeof(T));
    T v;
    std::memcpy(&v, d.data() + off, sizeof v);
    return v;
}
template <class T> void wr(Bytes& d, std::uint64_t off, T v) {
    need(d, off, sizeof(T));
    std::memcpy(d.data() + off, &v, sizeof v);
}
Bytes sub(const Bytes& d, std::uint64_t off, std::uint64_t size) {
    need(d, off, size);
    return Bytes(d.begin() + static_cast<std::ptrdiff_t>(off), d.begin() + static_cast<std::ptrdiff_t>(off + size));
}

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) fail("cannot read " + u8(p.filename()));
    Bytes d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.empty()) fail(u8(p.filename()) + " is empty (0 bytes): the game files are incomplete");
    return d;
}

// ---- SHA-1 (only for NIDs) ----------------------------------------------------------------------------------
std::array<std::uint8_t, 20> sha1(const std::string& msg) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<std::uint8_t> m(msg.begin(), msg.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(m.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    auto rol = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (std::size_t c = 0; c < m.size(); c += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t)m[c + i * 4] << 24 | (std::uint32_t)m[c + i * 4 + 1] << 16 | (std::uint32_t)m[c + i * 4 + 2] << 8 | m[c + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
            else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = cc; cc = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - j * 8));
    return out;
}

// ---- ELF / SELF (prepare.py: parse_elf, parse_self) ---------------------------------------------------------
struct Ph { std::uint32_t type, flags; std::uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Parsed {
    Bytes elf;
    std::uint64_t entry = 0;
    std::vector<Ph> ph;
};
constexpr std::uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_RELRO = 0x61000010,
                        PT_SCE_PROCPARAM = 0x61000001;
bool required_type(std::uint32_t t) { return t == PT_LOAD || t == PT_DYNAMIC || t == PT_SCE_DYNLIBDATA || t == PT_SCE_RELRO; }

std::vector<Ph> read_ph(const Bytes& d, std::uint64_t base) {
    // header '<16sHHIQQQIHHHHHH': ident, type, machine, version, entry, phoff, shoff, flags, ehsize, phentsize, phnum, ...
    need(d, base, 64);
    static const std::uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    if (std::memcmp(d.data() + base, ident, 7) != 0 || rd<std::uint16_t>(d, base + 18) != 62)
        fail("expected little-endian x86-64 ELF");
    const std::uint16_t phentsize = rd<std::uint16_t>(d, base + 54), phnum = rd<std::uint16_t>(d, base + 56);
    if (phentsize != 56 || !(phnum > 0 && phnum < 256)) fail("unsupported program headers");
    const std::uint64_t phoff = rd<std::uint64_t>(d, base + 32);
    std::vector<Ph> ph(phnum);
    for (unsigned i = 0; i < phnum; ++i) {
        const std::uint64_t o = base + phoff + i * 56ull;
        need(d, o, 56);
        ph[i] = {rd<std::uint32_t>(d, o), rd<std::uint32_t>(d, o + 4), rd<std::uint64_t>(d, o + 8), rd<std::uint64_t>(d, o + 16),
                 rd<std::uint64_t>(d, o + 24), rd<std::uint64_t>(d, o + 32), rd<std::uint64_t>(d, o + 40), rd<std::uint64_t>(d, o + 48)};
    }
    return ph;
}
std::uint64_t image_end(const std::vector<Ph>& ph) {
    std::uint64_t end = 0;
    for (const auto& p : ph) end = std::max(end, p.offset + p.filesz);
    if (end > 512ull * 1024 * 1024) fail("probe image exceeds 512 MiB limit");
    return end;
}

Parsed parse_self(const Bytes& data) {
    Parsed r;
    if (data.size() >= 4 && std::memcmp(data.data(), "\x7f" "ELF", 4) == 0) {
        const std::uint16_t type = rd<std::uint16_t>(data, 16);
        r.ph = read_ph(data, 0);
        if (type != 0xfe00 && type != 0xfe10 && type != 0xfe18) fail("unexpected ELF type (want ET_SCE_EXEC/DYNEXEC/DYNAMIC)");
        const std::uint64_t end = image_end(r.ph);
        r.elf = sub(data, 0, end);
        for (const auto& p : r.ph)
            if ((required_type(p.type) || p.type == PT_DYNAMIC) && p.filesz) need(data, p.offset, p.filesz);
        r.entry = rd<std::uint64_t>(data, 24);
        return r;
    }
    need(data, 0, 4);
    if (std::memcmp(data.data(), "O\x15=\x1d", 4) != 0) fail("expected PS4 SELF or decrypted ELF");
    const std::uint16_t count = rd<std::uint16_t>(data, 24);
    const std::uint64_t base = 32 + count * 32ull;
    r.ph = read_ph(data, base);
    r.entry = rd<std::uint64_t>(data, base + 24);
    const std::uint64_t end = image_end(r.ph);
    r.elf.assign(end, 0);
    const std::uint64_t phoff = rd<std::uint64_t>(data, base + 32);
    const std::uint64_t header_end = phoff + r.ph.size() * 56;
    {
        Bytes head = sub(data, base, header_end);
        if (head.size() > r.elf.size()) r.elf.resize(head.size()); // Python slice assignment grows the buffer
        std::copy(head.begin(), head.end(), r.elf.begin());
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> covered{{0, header_end}};
    for (unsigned i = 0; i < count; ++i) {
        const std::uint64_t o = 32 + i * 32ull;
        const std::uint64_t flags = rd<std::uint64_t>(data, o), off = rd<std::uint64_t>(data, o + 8), size = rd<std::uint64_t>(data, o + 16),
                            mem = rd<std::uint64_t>(data, o + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) fail("encrypted/compressed SELF segment is unsupported (this file is not a decrypted dump)");
        const std::uint64_t index = (flags >> 20) & 4095;
        if (index >= r.ph.size()) fail("invalid segment index");
        const Ph& p = r.ph[index];
        if (size != p.filesz || size != mem) fail("unsupported blocked segment layout");
        Bytes seg = sub(data, off, size);
        if (p.offset + size > r.elf.size()) r.elf.resize(p.offset + size);
        std::copy(seg.begin(), seg.end(), r.elf.begin() + static_cast<std::ptrdiff_t>(p.offset));
        covered.emplace_back(p.offset, p.offset + size);
    }
    for (std::size_t i = 0; i < r.ph.size(); ++i) {
        const Ph& p = r.ph[i];
        if (!p.filesz) continue;
        bool ok = false;
        for (auto [a, z] : covered) ok |= a <= p.offset && p.offset + p.filesz <= z;
        if (!ok && required_type(p.type)) fail("required segment " + std::to_string(i) + " is unavailable");
    }
    return r;
}

const Ph* find_ph(const std::vector<Ph>& ph, std::uint32_t type) {
    for (const auto& p : ph)
        if (p.type == type) return &p;
    return nullptr;
}
const Ph& need_ph(const std::vector<Ph>& ph, std::uint32_t type, const char* what) {
    const Ph* p = find_ph(ph, type);
    if (!p) fail(std::string("missing ") + what + " program header");
    return *p;
}

std::string encode_id(std::uint64_t value) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string r(1, alphabet[value & 63]);
    value >>= 6;
    while (value) {
        r.insert(r.begin(), alphabet[value & 63]);
        value >>= 6;
    }
    return r;
}

// ---- module (link_modules.py: module) ------------------------------------------------------------------------
// identity: (nid, (library, version), (module, version)); "none" for names that are not NID#lib#mod.
struct Identity {
    bool none = true;
    std::string nid, lib, mod;
    std::uint16_t libver = 0, modver = 0;
    auto key() const { return std::tie(none, nid, lib, libver, mod, modver); }
    bool operator<(const Identity& o) const { return key() < o.key(); }
    bool operator==(const Identity& o) const { return key() == o.key(); }
};
struct Symbol { std::string name; unsigned type, binding; std::uint16_t section; std::uint64_t value, size; Identity identity; };
struct Rela { std::uint64_t target; std::uint32_t kind; std::uint32_t sym; std::int64_t addend; };
struct Module {
    Parsed p;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dynamic; // until DT_NULL
    std::map<std::uint64_t, std::uint64_t> tags;                  // dict(dynamic): last value wins
    std::vector<Symbol> symbols;
    std::vector<Rela> relocs;
    std::string sha256;
};

std::string cstr(const Bytes& strings, std::uint64_t off) {
    need(strings, off, 1);
    const auto* b = strings.data() + off;
    const auto* e = static_cast<const std::uint8_t*>(std::memchr(b, 0, strings.size() - off));
    if (!e) fail("unterminated string table entry");
    for (const auto* q = b; q < e; ++q)
        if (*q >= 0x80) fail("non-ASCII symbol name");
    return std::string(reinterpret_cast<const char*>(b), static_cast<std::size_t>(e - b));
}

std::uint64_t tag(const Module& m, std::uint64_t t) {
    auto it = m.tags.find(t);
    if (it == m.tags.end()) {
        char b[64];
        std::snprintf(b, sizeof b, "missing dynamic tag %#llx", (unsigned long long)t);
        fail(b);
    }
    return it->second;
}

Module load_module(const fs::path& path, bool identities_required) {
    Module m;
    const Bytes source = read_file(path);
    m.sha256 = sha256_hex(source.data(), source.size());
    m.p = parse_self(source);
    const Ph& dp = need_ph(m.p.ph, PT_DYNAMIC, "dynamic");
    for (std::uint64_t pos = dp.offset; pos < dp.offset + dp.filesz; pos += 16) {
        const auto t = rd<std::uint64_t>(m.p.elf, pos), v = rd<std::uint64_t>(m.p.elf, pos + 8);
        if (!t) break;
        m.dynamic.emplace_back(t, v);
    }
    for (auto [t, v] : m.dynamic) m.tags[t] = v;
    const Ph& lp = need_ph(m.p.ph, PT_SCE_DYNLIBDATA, "SCE dynlib data");
    const Bytes blob = sub(m.p.elf, lp.offset, lp.filesz);
    const Bytes strings = sub(blob, tag(m, 0x61000035), tag(m, 0x61000037));
    std::map<std::string, std::pair<std::string, std::uint16_t>> libraries, modules;
    for (auto [t, v] : m.dynamic) {
        if (t == 0x61000013 || t == 0x61000015 || t == 0x6100000d || t == 0x6100000f) {
            auto& table = (t == 0x61000013 || t == 0x61000015) ? libraries : modules;
            const std::string key = encode_id(v >> 48);
            std::pair<std::string, std::uint16_t> id{cstr(strings, v & 0xffffffff), static_cast<std::uint16_t>((v >> 32) & 65535)};
            auto it = table.find(key);
            if (it != table.end() && it->second != id) fail("conflicting module/library IDs");
            table[key] = id;
        }
    }
    const Bytes syms = sub(blob, tag(m, 0x61000039), tag(m, 0x6100003f));
    if (syms.size() % 24) fail("bad symbol table size");
    for (std::size_t pos = 0; pos < syms.size(); pos += 24) {
        Symbol s;
        const auto n = rd<std::uint32_t>(syms, pos);
        const auto info = rd<std::uint8_t>(syms, pos + 4);
        s.section = rd<std::uint16_t>(syms, pos + 6);
        s.value = rd<std::uint64_t>(syms, pos + 8);
        s.size = rd<std::uint64_t>(syms, pos + 16);
        s.type = info & 15;
        s.binding = info >> 4;
        s.name = cstr(strings, n);
        if (!s.name.empty()) {
            const auto a = s.name.find('#');
            const auto b = a == std::string::npos ? a : s.name.find('#', a + 1);
            const bool three = a != std::string::npos && b != std::string::npos && s.name.find('#', b + 1) == std::string::npos;
            if (three) {
                const std::string lib = s.name.substr(a + 1, b - a - 1), mod = s.name.substr(b + 1);
                auto li = libraries.find(lib);
                auto mi = modules.find(mod);
                if (li == libraries.end() || mi == modules.end()) fail("symbol refers to an unknown library/module id: " + s.name);
                s.identity = {false, s.name.substr(0, a), li->second.first, mi->second.first, li->second.second, mi->second.second};
            } else if (identities_required) {
                fail("unsupported symbol encoding " + s.name);
            }
        }
        m.symbols.push_back(std::move(s));
    }
    for (auto [ot, st] : {std::pair<std::uint64_t, std::uint64_t>{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}}) {
        const Bytes table = sub(blob, tag(m, ot), tag(m, st));
        if (table.size() % 24) fail("bad relocation table size");
        for (std::size_t pos = 0; pos < table.size(); pos += 24) {
            const auto info = rd<std::uint64_t>(table, pos + 8);
            m.relocs.push_back({rd<std::uint64_t>(table, pos), static_cast<std::uint32_t>(info & 0xffffffff),
                                static_cast<std::uint32_t>(info >> 32), rd<std::int64_t>(table, pos + 16)});
        }
    }
    return m;
}

bool mapped_in(const std::vector<Ph>& loads, std::uint64_t addr, std::uint64_t size) {
    for (const auto& p : loads)
        if (p.vaddr <= addr && addr + size <= p.vaddr + p.memsz) return true;
    return false;
}
std::vector<Ph> load_segments(const std::vector<Ph>& ph) {
    std::vector<Ph> l;
    for (const auto& p : ph)
        if (p.type == PT_LOAD || p.type == PT_SCE_RELRO) l.push_back(p);
    return l;
}

// `mov r64, fs:[0]` -> gs (link_modules.py: patch_fs_loads)
std::size_t patch_fs_loads(Bytes& image, const std::vector<Ph>& ph, std::uint64_t base) {
    static const std::uint8_t modrm[] = {0x04, 0x0c, 0x14, 0x1c, 0x24, 0x2c, 0x34, 0x3c};
    std::size_t patched = 0;
    for (const auto& p : ph) {
        if (p.type != PT_LOAD || !(p.flags & 1)) continue;
        std::uint64_t i = base + p.vaddr;
        const std::uint64_t end = std::min<std::uint64_t>(base + p.vaddr + p.filesz, image.size());
        while (i + 9 <= end) {
            const std::uint8_t* q = image.data() + i;
            if (q[0] == 0x64 && (q[1] == 0x48 || q[1] == 0x4c) && q[2] == 0x8b &&
                std::find(std::begin(modrm), std::end(modrm), q[3]) != std::end(modrm) && q[4] == 0x25 && !q[5] && !q[6] && !q[7] && !q[8]) {
                image[i] = 0x65;
                ++patched;
                i += 9;
            } else {
                ++i;
            }
        }
    }
    return patched;
}

std::string canonical(const Identity& id, const std::string& fallback) { return id.none ? fallback : id.nid + "#" + id.lib; }

struct Reloc { std::uint64_t target, kind; std::int64_t value, addend; };

// prepare.py: inspect_libc -> does this dump's _init_env consist of a single RET?
bool libc_init_env_is_ret(const fs::path& path) {
    const Bytes source = read_file(path);
    const Parsed p = parse_self(source);
    const Ph& dp = need_ph(p.ph, PT_DYNAMIC, "dynamic");
    std::map<std::uint64_t, std::uint64_t> tags; // every entry of the segment (the Python reads past DT_NULL too)
    for (std::uint64_t pos = dp.offset; pos < dp.offset + dp.filesz; pos += 16) tags[rd<std::uint64_t>(p.elf, pos)] = rd<std::uint64_t>(p.elf, pos + 8);
    auto t = [&](std::uint64_t k) {
        auto it = tags.find(k);
        if (it == tags.end()) fail("libc.prx: missing dynamic tag");
        return it->second;
    };
    const Ph& lib = need_ph(p.ph, PT_SCE_DYNLIBDATA, "SCE dynlib data");
    const Bytes blob = sub(p.elf, lib.offset, lib.filesz);
    const Bytes strings = sub(blob, t(0x61000035), t(0x61000037));
    const Bytes syms = sub(blob, t(0x61000039), t(0x6100003f));
    const std::string want = nid("_init_env"), ios = nid("_ZNSt8ios_base4InitC1Ev");
    bool is_ret = false;
    for (std::size_t pos = 0; pos + 24 <= syms.size(); pos += 24) {
        const auto name = rd<std::uint32_t>(syms, pos);
        const auto info = rd<std::uint8_t>(syms, pos + 4);
        const auto section = rd<std::uint16_t>(syms, pos + 6);
        const auto value = rd<std::uint64_t>(syms, pos + 8), size = rd<std::uint64_t>(syms, pos + 16);
        const std::string symbol = cstr(strings, name);
        const std::string key = symbol.substr(0, symbol.find('#'));
        if ((key != want && key != ios) || !section || (info & 15) != 2) continue;
        const Ph* code_seg = nullptr;
        for (const auto& q : p.ph)
            if (q.type == PT_LOAD && (q.flags & 1) && q.vaddr <= value && value < q.vaddr + q.filesz) { code_seg = &q; break; }
        if (!code_seg) fail("libc.prx: function outside its code segment");
        const Bytes code = sub(p.elf, code_seg->offset + value - code_seg->vaddr, size);
        if (key == want) is_ret = code.size() == 1 && code[0] == 0xc3;
    }
    return is_ret;
}

const std::unordered_map<std::string, std::string>& nid_table() {
    static const std::unordered_map<std::string, std::string> t = {
#include "nid_names.inc"
    };
    return t;
}

void write_atomic(const fs::path& path, const Bytes& data) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) fail("cannot write " + u8(tmp));
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) fail("write failed: " + u8(tmp) + " (disk full?)");
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        fail("cannot replace " + u8(path));
    }
}

template <class T> void put(Bytes& out, T v) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof v);
}

} // namespace

std::string nid(const std::string& name) {
    static const std::uint8_t salt[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    const auto d = sha1(name + std::string(reinterpret_cast<const char*>(salt), sizeof salt));
    std::uint8_t v[8];
    for (int i = 0; i < 8; ++i) v[i] = d[7 - i];
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (int i = 0; i < 8; i += 3) {
        const std::uint32_t n = (std::uint32_t)v[i] << 16 | (i + 1 < 8 ? (std::uint32_t)v[i + 1] << 8 : 0) | (i + 2 < 8 ? v[i + 2] : 0);
        s += b64[(n >> 18) & 63];
        s += b64[(n >> 12) & 63];
        if (i + 1 < 8) s += b64[(n >> 6) & 63];
        if (i + 2 < 8) s += b64[n & 63];
    }
    for (auto& c : s)
        if (c == '/') c = '-';
    return s;
}

const std::vector<std::string>& linked_modules() {
    static const std::vector<std::string> m = {"libc.prx", "libSceFios2.prx"};
    return m;
}

std::vector<InputFile> hash_inputs(const fs::path& game, std::stop_token stop) {
    std::vector<std::string> rel = {"eboot.bin"};
    for (const auto& m : linked_modules()) rel.push_back("sce_module/" + m);
    rel.push_back("sce_sys/param.sfo");
    std::vector<InputFile> out;
    for (const auto& r : rel) {
        check_stop(stop);
        const fs::path p = game / fs::path(r);
        std::ifstream f(p, std::ios::binary);
        if (!f) fail("missing or unreadable: " + r);
        PortableSha256 h;
        std::vector<char> buf(1 << 20);
        std::uint64_t total = 0;
        while (f) {
            check_stop(stop);
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = f.gcount();
            if (n > 0) { h.update(buf.data(), static_cast<std::size_t>(n)); total += static_cast<std::uint64_t>(n); }
        }
        if (!total) fail(r + " is empty (0 bytes): the game files are incomplete");
        out.push_back({r, total, PortableSha256::to_hex(h.finish())});
    }
    return out;
}

PrepareResult prepare(const fs::path& game, const fs::path& out_dir, const Progress& progress, std::stop_token stop) {
    auto step = [&](double f, const char* what) {
        check_stop(stop);
        if (progress) progress(f, what);
    };
    PrepareResult result;
    step(0.02, "Checking game files");
    result.inputs = hash_inputs(game, stop);

    // ---- prepare.py: the eboot's memory image, relocations and imports ----
    step(0.25, "Reading the game executable");
    Module main = load_module(game / "eboot.bin", false);
    const auto loads = load_segments(main.p.ph);
    std::uint64_t size = 0;
    for (const auto& p : loads) size = std::max(size, p.vaddr + p.memsz);
    if (size > 512ull * 1024 * 1024 || !mapped_in(loads, main.p.entry, 1)) fail("invalid memory image");
    Bytes image(size, 0);
    for (const auto& p : loads) {
        if (p.filesz > p.memsz) fail("segment file size exceeds memory size");
        Bytes seg = sub(main.p.elf, p.offset, p.filesz);
        std::copy(seg.begin(), seg.end(), image.begin() + static_cast<std::ptrdiff_t>(p.vaddr));
    }
    std::vector<Reloc> relocs;
    std::vector<std::uint32_t> import_syms;                 // symbol index per import, in first-use order
    std::unordered_map<std::uint32_t, std::uint32_t> import_index;
    for (const auto& r : main.relocs) {
        if (!mapped_in(loads, r.target, 8)) {
            char b[96];
            std::snprintf(b, sizeof b, "relocation outside mapped segments: %#llx", (unsigned long long)r.target);
            fail(b);
        }
        if (r.kind == 8) {
            relocs.push_back({r.target, 0, r.addend, 0});
        } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
            if (r.sym >= main.symbols.size()) fail("relocation symbol out of range");
            const Symbol& s = main.symbols[r.sym];
            if (s.section) {
                if (s.section == 0xfff1) wr<std::uint64_t>(image, r.target, s.value + static_cast<std::uint64_t>(r.addend));
                else relocs.push_back({r.target, 0, static_cast<std::int64_t>(s.value) + r.addend, 0});
            } else {
                auto it = import_index.find(r.sym);
                if (it == import_index.end()) {
                    it = import_index.emplace(r.sym, static_cast<std::uint32_t>(import_syms.size())).first;
                    import_syms.push_back(r.sym);
                }
                const std::uint64_t kind = s.type == 1 ? 2 : 1;
                if ((kind == 1 && r.addend) || !(r.addend >= 0 && r.addend < 4096)) fail("unsupported import addend");
                relocs.push_back({r.target, kind, static_cast<std::int64_t>(it->second), r.addend});
            }
        } else {
            fail("unsupported relocation " + std::to_string(r.kind));
        }
    }
    std::vector<std::string> raw_names;
    for (auto i : import_syms) raw_names.push_back(main.symbols[i].name);
    step(0.4, "Checking the game's C library");
    const std::uint64_t flags = libc_init_env_is_ret(game / "sce_module" / "libc.prx") ? 1 : 0;
    std::vector<std::array<std::uint64_t, 3>> segments;
    for (const auto& p : loads) segments.push_back({p.vaddr, p.memsz, p.flags});

    // ---- link_modules.py ----
    step(0.5, "Linking the game's bundled modules");
    auto identity_of = [&](const std::string& name) -> Identity {
        for (const auto& s : main.symbols)
            if (s.name == name) return s.identity;
        return {};
    };
    std::vector<std::string> names;
    std::vector<std::pair<Identity, std::uint64_t>> identities; // insertion-ordered dict
    std::map<Identity, std::size_t> identity_pos;
    auto set_identity = [&](const Identity& id, std::uint64_t index) {
        auto it = identity_pos.find(id);
        if (it == identity_pos.end()) { identity_pos[id] = identities.size(); identities.emplace_back(id, index); }
        else identities[it->second].second = index;
    };
    for (std::size_t i = 0; i < raw_names.size(); ++i) {
        const Identity id = identity_of(raw_names[i]);
        set_identity(id, i);
        names.push_back(canonical(id, raw_names[i]));
    }
    auto imported = [&](const Symbol& s) -> std::uint64_t {
        auto it = identity_pos.find(s.identity);
        if (it == identity_pos.end()) {
            if (s.identity.none) fail("import without a library identity");
            set_identity(s.identity, names.size());
            names.push_back(canonical(s.identity, ""));
            return names.size() - 1;
        }
        return identities[it->second].second;
    };
    struct Export { std::uint64_t address, kind; std::string file; };
    std::map<Identity, Export> exports;
    std::map<std::pair<std::string, std::string>, std::vector<Export>> by_nid;
    struct TableRow { std::uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; };
    std::vector<TableRow> table;
    std::uint64_t base = (size + 65535) & ~65535ull;
    patch_fs_loads(image, main.p.ph, 0);
    std::uint64_t tls_module = 2;
    double frac = 0.55;
    for (const auto& filename : linked_modules()) {
        step(frac, ("Linking " + filename).c_str());
        frac += 0.15;
        Module m = load_module(game / "sce_module" / filename, false);
        const auto mloads = load_segments(m.p.ph);
        std::uint64_t modsize = 0;
        for (const auto& p : mloads) modsize = std::max(modsize, p.vaddr + p.memsz);
        if (base + modsize > 512ull * 1024 * 1024) fail("linked image exceeds probe limit");
        if (base + modsize < image.size()) fail(filename + ": module overlaps the image");
        image.resize(base + modsize, 0);
        for (const auto& p : mloads) {
            if (p.filesz > p.memsz) fail(filename + ": invalid segment size");
            Bytes seg = sub(m.p.elf, p.offset, p.filesz);
            std::copy(seg.begin(), seg.end(), image.begin() + static_cast<std::ptrdiff_t>(base + p.vaddr));
            segments.push_back({base + p.vaddr, p.memsz, p.flags});
        }
        const Ph* tls = find_ph(m.p.ph, PT_TLS);
        std::uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped_in(mloads, tls->vaddr, tls->memsz)) fail(filename + ": unsupported TLS layout");
            module_id = tls_module++;
        }
        for (const auto& r : m.relocs) {
            if (!mapped_in(mloads, r.target, 8)) fail(filename + ": relocation outside image");
            const std::uint64_t target = r.target + base;
            if (r.kind == 8) {
                relocs.push_back({target, 0, static_cast<std::int64_t>(base) + r.addend, 0});
            } else if (r.kind == 16) {
                if (r.sym >= m.symbols.size()) fail(filename + ": relocation symbol out of range");
                const Symbol& s = m.symbols[r.sym];
                const bool local = s.name.empty() && s.type == 3 && !r.addend && module_id;
                if (!local) fail(filename + ": unsupported external TLS relocation (module from an older SDK: use the one from the game update)");
                wr<std::uint64_t>(image, target, module_id);
            } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                if (r.sym >= m.symbols.size()) fail(filename + ": relocation symbol out of range");
                const Symbol& s = m.symbols[r.sym];
                if (s.section) {
                    if (s.section == 0xfff1) wr<std::uint64_t>(image, target, s.value + static_cast<std::uint64_t>(r.addend));
                    else relocs.push_back({target, 0, static_cast<std::int64_t>(base + s.value) + r.addend, 0});
                } else if (s.identity.none && s.binding == 2) {
                    wr<std::uint64_t>(image, target, 0); // weak undefined (module_start/stop)
                } else {
                    const std::uint64_t rk = s.type == 1 ? 2 : 1;
                    if ((rk == 1 && r.addend) || !(r.addend >= 0 && r.addend < 4096)) fail(filename + ": unsupported import addend");
                    relocs.push_back({target, rk, static_cast<std::int64_t>(imported(s)), r.addend});
                }
            } else {
                fail(filename + ": unsupported relocation type " + std::to_string(r.kind));
            }
        }
        for (const auto& s : m.symbols) {
            if (s.section && (s.binding == 1 || s.binding == 2) && !s.identity.none && (s.type == 1 || s.type == 2)) {
                if (!mapped_in(mloads, s.value, std::max<std::uint64_t>(1, s.size))) fail(filename + ": export outside load segments");
                if (exports.count(s.identity)) fail(filename + ": ambiguous export");
                const Export e{base + s.value, s.type == 1 ? 2u : 1u, filename};
                exports[s.identity] = e;
                by_nid[{s.identity.nid, s.identity.lib}].push_back(e);
            }
        }
        patch_fs_loads(image, m.p.ph, base);
        table.push_back({base, modsize, base + (m.tags.count(12) ? m.tags[12] : 0), tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0,
                         tls ? tls->filesz : 0, module_id});
        base = (base + modsize + 65535) & ~65535ull;
    }

    step(0.85, "Binding imports");
    std::vector<std::array<std::uint64_t, 3>> bindings;
    std::size_t unresolved = 0;
    for (const auto& [id, index] : identities) {
        const Export* found = nullptr;
        if (auto it = exports.find(id); it != exports.end()) found = &it->second;
        if (!found && !id.none && id.lib == "libSceLibcInternal") {
            auto it = by_nid.find({id.nid, "libc"});
            if (it != by_nid.end() && it->second.size() == 1) found = &it->second[0];
        }
        if (!found && !id.none) {
            auto it = by_nid.find({id.nid, id.lib});
            if (it != by_nid.end() && it->second.size() == 1) found = &it->second[0];
        }
        if (found) bindings.push_back({index, found->address, found->kind});
        else ++unresolved;
    }
    const Ph* main_tls = find_ph(main.p.ph, PT_TLS);
    std::array<std::uint64_t, 4> main_tls_values{};
    if (main_tls) {
        main_tls_values = {main_tls->vaddr, main_tls->filesz, main_tls->memsz, main_tls->align};
        if (main_tls->filesz > main_tls->memsz || main_tls->memsz > 1024 * 1024 || main_tls->vaddr + main_tls->filesz > size)
            fail("unsupported eboot TLS layout");
    }
    const Ph& procparam = need_ph(main.p.ph, PT_SCE_PROCPARAM, "process parameter");

    step(0.92, "Writing the prepared boot image");
    Bytes out;
    out.reserve(image.size() + relocs.size() * 32 + names.size() * 128 + 4096);
    out.insert(out.end(), {'J', 'S', 'T', 'B', 'O', 'O', 'T', '5'});
    put<std::uint64_t>(out, image.size());
    put<std::uint64_t>(out, main.p.entry);
    put<std::uint64_t>(out, segments.size());
    put<std::uint64_t>(out, relocs.size());
    put<std::uint64_t>(out, names.size());
    put<std::uint64_t>(out, flags);
    put<std::uint64_t>(out, procparam.vaddr);
    for (auto v : main_tls_values) put<std::uint64_t>(out, v);
    put<std::uint64_t>(out, table.size());
    for (const auto& t : table)
        for (auto v : {t.base, t.size, t.init, t.tls_address, t.tls_memsz, t.tls_filesz, t.tls_module}) put<std::uint64_t>(out, v);
    put<std::uint64_t>(out, bindings.size());
    for (const auto& b : bindings)
        for (auto v : b) put<std::uint64_t>(out, v);
    for (const auto& s : segments)
        for (auto v : s) put<std::uint64_t>(out, v);
    for (const auto& n : names) {
        if (n.size() >= 128) fail("name too long");
        std::uint8_t buf[128] = {};
        std::memcpy(buf, n.data(), n.size());
        out.insert(out.end(), buf, buf + 128);
    }
    for (const auto& r : relocs) {
        put<std::uint64_t>(out, r.target);
        put<std::uint64_t>(out, r.kind);
        put<std::int64_t>(out, r.value);
        put<std::int64_t>(out, r.addend);
    }
    out.insert(out.end(), image.begin(), image.end());
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) fail("cannot create " + u8(out_dir));
    check_stop(stop);
    write_atomic(out_dir / "boot-linked.bin", out);
    result.boot_sha256 = sha256_hex(out.data(), out.size());

    // nid_names.py: diagnostic names
    std::string tsv;
    for (const auto& n : names) {
        const std::string key = n.substr(0, n.find('#'));
        auto it = nid_table().find(key);
        tsv += n + "\t" + (it == nid_table().end() ? std::string() : it->second) + "\n";
    }
    write_atomic(out_dir / "import-names.tsv", Bytes(tsv.begin(), tsv.end()));
    result.image_bytes = image.size();
    result.imports = names.size();
    result.bindings = bindings.size();
    result.relocations = relocs.size();
    result.segments = segments.size();
    (void)unresolved;
    step(1.0, "Done");
    return result;
}

} // namespace reborn::prep
