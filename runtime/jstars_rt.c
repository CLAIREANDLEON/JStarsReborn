// Import registry. An import is served by the host only if a real implementation is registered
// in one of the symbol tables below; everything else stays trapped by the loader.
//
// Matching is by NID (the console's hash of the symbol name) and library, computed here from the
// registered names, so the runtime does not depend on any external name table. Names in log lines
// come from the optional import-names.tsv and are diagnostics only.
#include "jstars_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;
typedef struct { const HostSymbol* table; const size_t* count; } SymbolSet;
extern const HostSymbol kernel_symbols[]; extern const size_t kernel_symbol_count;
extern const HostSymbol memory_symbols[]; extern const size_t memory_symbol_count;
extern const HostSymbol system_symbols[]; extern const size_t system_symbol_count;
extern const HostSymbol input_symbols[]; extern const size_t input_symbol_count;
extern const HostSymbol av_symbols[]; extern const size_t av_symbol_count;
extern const HostSymbol fs_symbols[]; extern const size_t fs_symbol_count;
extern const HostSymbol evf_symbols[]; extern const size_t evf_symbol_count;
extern const HostSymbol platform_symbols[]; extern const size_t platform_symbol_count;
extern const HostSymbol net_symbols[]; extern const size_t net_symbol_count;
extern const HostSymbol avplayer_symbols[]; extern const size_t avplayer_symbol_count;
extern const HostSymbol savedata_symbols[]; extern const size_t savedata_symbol_count;

// ---- SHA-1 (RFC 3174) ---------------------------------------------------------------------------
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } Sha1;
static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static void sha1_block(Sha1* s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_update(Sha1* s, const uint8_t* d, size_t n) {
    s->len += n;
    while (n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, d, k); s->n += k; d += k; n -= k;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}
static void sha1_final(Sha1* s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha1_update(s, &z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; ++i) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, l, 8);
    for (int i = 0; i < 5; ++i) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}

// NID: first 8 bytes of SHA-1(name || salt), byte-reversed, base64 without padding, '/' written as '-'.
void rt_nid(const char* name, char out[12]) {
    static const uint8_t salt[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    Sha1 s = {{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {0}, 0};
    sha1_update(&s, (const uint8_t*)name, strlen(name));
    sha1_update(&s, salt, sizeof salt);
    uint8_t d[20];
    sha1_final(&s, d);
    uint8_t r[8];
    for (int i = 0; i < 8; ++i) r[i] = d[7 - i];
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    int o = 0;
    for (int i = 0; i < 8; i += 3) {
        uint32_t v = (uint32_t)r[i] << 16 | (i + 1 < 8 ? (uint32_t)r[i + 1] << 8 : 0) | (i + 2 < 8 ? r[i + 2] : 0);
        out[o++] = a[v >> 18 & 63]; out[o++] = a[v >> 12 & 63];
        if (i + 1 < 8) out[o++] = a[v >> 6 & 63];
        if (i + 2 < 8) out[o++] = a[v & 63];
    }
    out[o] = 0; // 11 characters for 8 bytes
}

typedef struct { char nid[12]; const HostSymbol* sym; } Entry;
static Entry* g_entries;
static size_t g_entry_count;

static void build_index(void) {
    const SymbolSet sets[] = {{kernel_symbols, &kernel_symbol_count}, {memory_symbols, &memory_symbol_count}, {system_symbols, &system_symbol_count},
                              {input_symbols, &input_symbol_count}, {av_symbols, &av_symbol_count},
                              {fs_symbols, &fs_symbol_count}, {evf_symbols, &evf_symbol_count}, {platform_symbols, &platform_symbol_count}, {net_symbols, &net_symbol_count}, {avplayer_symbols, &avplayer_symbol_count},
                              {savedata_symbols, &savedata_symbol_count}};
    size_t total = 0;
    for (size_t k = 0; k < sizeof sets / sizeof *sets; ++k) total += *sets[k].count;
    g_entries = calloc(total ? total : 1, sizeof *g_entries);
    for (size_t k = 0; k < sizeof sets / sizeof *sets; ++k)
        for (size_t i = 0; i < *sets[k].count; ++i) {
            rt_nid(sets[k].table[i].name, g_entries[g_entry_count].nid);
            g_entries[g_entry_count++].sym = &sets[k].table[i];
        }
}

static const char* g_origin = "runtime";
static const char* g_resolved_name = "";
const char* rt_resolve_origin(void) { return g_origin; }
const char* rt_resolve_name(void) { return g_resolved_name; }

void* rt_resolve(const char* nid_lib, const char* name, int is_data) {
    (void)name;
    if (!g_entries) build_index();
    // The GPU library (vendored shadPS4 GnmDriver/VideoOut/event queues) takes precedence over the
    // diagnostic stubs of jstars_av.c when it is active.
    g_origin = "runtime";
    g_resolved_name = "";
    if (!is_data) { void* g = rt_gpu_resolve(nid_lib); if (g) { g_origin = "gpu"; g_resolved_name = rt_gpu_symbol_name(nid_lib); return g; } }
    const char* hash = strchr(nid_lib, '#');
    if (!hash) return NULL;
    size_t nl = (size_t)(hash - nid_lib);
    const char* lib = hash + 1;
    for (size_t i = 0; i < g_entry_count; ++i) {
        const HostSymbol* s = g_entries[i].sym;
        if (strlen(g_entries[i].nid) == nl && !memcmp(g_entries[i].nid, nid_lib, nl) && s->is_data == is_data && (!s->lib || !strcmp(s->lib, lib)))
            { g_resolved_name = s->name; return s->fn; }
    }
    return NULL;
}

// Host implementation by symbol name (self-test only; the guest resolves by NID).
void* rt_find_host(const char* name) {
    if (!g_entries) build_index();
    for (size_t i = 0; i < g_entry_count; ++i)
        if (!strcmp(g_entries[i].sym->name, name)) return g_entries[i].sym->fn;
    return NULL;
}

// Every runtime-implemented import with its audit status, one TSV line each (tools/audit_imports.py reads this).
void rt_audit_dump(void* out_file) {
    FILE* f = (FILE*)out_file;
    if (!g_entries) build_index();
    for (size_t i = 0; i < g_entry_count; ++i) {
        const char* note = "";
        const char* st = rt_audit_status(g_entries[i].nid, g_entries[i].sym->name, "runtime", &note);
        fprintf(f, "%s\t%s\t%s\t%s\t%s\n", g_entries[i].sym->name, g_entries[i].sym->lib ? g_entries[i].sym->lib : "", g_entries[i].nid, st, note);
    }
}
