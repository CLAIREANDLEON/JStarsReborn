// JStarsReborn - LAN room directory (docs/ADR-007-online.md, addendum "the first room directory").
//
// The console's matching service is replaced by a directory that the running instances keep between themselves on the
// local network. Each room has one owner instance; the owner broadcasts a small announcement of every room it owns once
// a second, and every instance keeps the rooms it has heard about in the last ROOM_STALE_MS. Nothing here invents a room
// or a member: a room exists in a table only because its owner announced it, and it disappears when the owner stops.
//
// Wire format, version 1 (all integers little-endian, every field size-checked on receipt):
//   0  'J','S','R','M'      magic
//   4  u8  version (1)      5  u8 type (1 = room announcement)    6  u16 payload length (bytes after this 16-byte header)
//   8  u64 instance nonce   (lets an instance ignore its own announcements)
//   16 payload: u64 room id, u32 world id, u16 max slots, u16 current members, u32 flags, char owner[16],
//      u16 owner P2P port, u8 int-attribute count (<= ROOM_MAX_INT), u8 bin-attribute count (<= ROOM_MAX_BIN),
//      { u16 id, u32 value } per int attribute, { u16 id, u16 length (<= ROOM_MAX_BIN_LEN), bytes } per bin attribute.
// This format is this project's own. It is not PlayStation Network's and no PS4 understands it.
#include "jstars_rt.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET rsock_t;
#define RSOCK_INVALID INVALID_SOCKET
#define RSOCK_CLOSE(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int rsock_t;
#define RSOCK_INVALID (-1)
#define RSOCK_CLOSE(s) close(s)
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROOM_HEADER 16
#define ROOM_STALE_MS 5000
#define ROOM_ANNOUNCE_MS 1000
#define ROOM_TABLE 64
#define ROOM_LOCAL 4
#define ROOM_MAX_PEERS 8

static pthread_mutex_t g_rooms_lock = PTHREAD_MUTEX_INITIALIZER;
static RoomInfo g_remote[ROOM_TABLE];
static int g_remote_used[ROOM_TABLE];
static RoomInfo g_local[ROOM_LOCAL];
static int g_local_used[ROOM_LOCAL];
static uint64_t g_nonce;
static uint16_t g_port = 47391;
static uint32_t g_peers[ROOM_MAX_PEERS];   // network order
static int g_npeers;
static int g_started;
static rsock_t g_sock = RSOCK_INVALID;
static uint64_t g_sent, g_received, g_rejected;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

void rt_rooms_configure(uint16_t port, const char* peers_csv) {
    if (port) g_port = port;
    g_npeers = 0;
    if (!peers_csv) return;
    char buf[256];
    strncpy(buf, peers_csv, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char* tok = strtok(buf, ","); tok && g_npeers < ROOM_MAX_PEERS; tok = strtok(NULL, ",")) {
        struct in_addr a;
        if (inet_pton(AF_INET, tok, &a) == 1) g_peers[g_npeers++] = a.s_addr;
        else rt_logf("rooms: --lobby-peer '%s' is not an IPv4 address; ignored", tok);
    }
}
uint64_t rt_rooms_nonce(void) {
    if (!g_nonce) {
        uint64_t seed = now_ms() ^ ((uint64_t)(uintptr_t)&g_nonce << 16) ^ (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull;
        seed ^= seed >> 33; seed *= 0xff51afd7ed558ccdull; seed ^= seed >> 33;
        g_nonce = seed ? seed : 1;
    }
    return g_nonce;
}
// A new room id: random-looking, never 0, unique per instance.
uint64_t rt_rooms_new_id(void) {
    static uint64_t counter;
    uint64_t x = rt_rooms_nonce() + (++counter) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 31; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 29;
    return x ? x : 1;
}

int rt_rooms_encode(const RoomInfo* r, uint64_t nonce, uint8_t* buf, int cap) {
    if (r->n_int > ROOM_MAX_INT || r->n_bin > ROOM_MAX_BIN) return -1;
    int n = ROOM_HEADER;
    const int fixed = 8 + 4 + 2 + 2 + 4 + 16 + 2 + 1 + 1;
    int need = n + fixed + r->n_int * 6;
    for (int i = 0; i < r->n_bin; ++i) { if (r->bins[i].len > ROOM_MAX_BIN_LEN) return -1; need += 4 + r->bins[i].len; }
    if (need > cap) return -1;
    memcpy(buf, "JSRM", 4); buf[4] = 1; buf[5] = 1; put16(buf + 6, (uint16_t)(need - ROOM_HEADER)); put64(buf + 8, nonce);
    put64(buf + n, r->room_id); n += 8;
    put32(buf + n, r->world_id); n += 4;
    put16(buf + n, r->max_slot); n += 2;
    put16(buf + n, r->cur_members); n += 2;
    put32(buf + n, r->flags); n += 4;
    memcpy(buf + n, r->owner, 16); n += 16;
    put16(buf + n, r->owner_port); n += 2;
    buf[n++] = r->n_int; buf[n++] = r->n_bin;
    for (int i = 0; i < r->n_int; ++i) { put16(buf + n, r->ints[i].id); put32(buf + n + 2, r->ints[i].value); n += 6; }
    for (int i = 0; i < r->n_bin; ++i) { put16(buf + n, r->bins[i].id); put16(buf + n + 2, r->bins[i].len); memcpy(buf + n + 4, r->bins[i].data, r->bins[i].len); n += 4 + r->bins[i].len; }
    return n;
}

// Parses an announcement. Returns 1 for a room from another instance, 0 for our own, -1 for anything malformed.
int rt_rooms_decode(const uint8_t* buf, int len, RoomInfo* r, uint64_t* nonce_out) {
    if (len < ROOM_HEADER || memcmp(buf, "JSRM", 4) || buf[4] != 1 || buf[5] != 1) return -1;
    const int plen = get16(buf + 6);
    if (plen != len - ROOM_HEADER) return -1;
    const uint64_t nonce = get64(buf + 8);
    if (nonce_out) *nonce_out = nonce;
    const uint8_t* p = buf + ROOM_HEADER;
    const uint8_t* end = buf + len;
    if (end - p < 40) return -1;
    memset(r, 0, sizeof *r);
    r->room_id = get64(p); p += 8;
    r->world_id = get32(p); p += 4;
    r->max_slot = get16(p); p += 2;
    r->cur_members = get16(p); p += 2;
    r->flags = get32(p); p += 4;
    memcpy(r->owner, p, 16); p += 16;
    r->owner_port = get16(p); p += 2;
    r->n_int = *p++; r->n_bin = *p++;
    if (!r->room_id || r->n_int > ROOM_MAX_INT || r->n_bin > ROOM_MAX_BIN || r->max_slot == 0 || r->cur_members > r->max_slot) return -1;
    for (int i = 0; i < r->n_int; ++i) {
        if (end - p < 6) return -1;
        r->ints[i].id = get16(p); r->ints[i].value = get32(p + 2); p += 6;
    }
    for (int i = 0; i < r->n_bin; ++i) {
        if (end - p < 4) return -1;
        r->bins[i].id = get16(p); r->bins[i].len = get16(p + 2); p += 4;
        if (r->bins[i].len > ROOM_MAX_BIN_LEN || end - p < r->bins[i].len) return -1;
        memcpy(r->bins[i].data, p, r->bins[i].len); p += r->bins[i].len;
    }
    if (p != end) return -1;
    return nonce == rt_rooms_nonce() ? 0 : 1;
}

// Takes one received datagram into the table. `ip` is the sender's address in network order.
int rt_rooms_ingest(const uint8_t* buf, int len, uint32_t ip, uint64_t at_ms) {
    RoomInfo r;
    const int k = rt_rooms_decode(buf, len, &r, NULL);
    if (k <= 0) { if (k < 0) ++g_rejected; return k; }
    r.owner_ip = ip;
    r.heard_ms = at_ms;
    pthread_mutex_lock(&g_rooms_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < ROOM_TABLE; ++i) {
        if (g_remote_used[i] && g_remote[i].room_id == r.room_id) { slot = i; break; }
        if (!g_remote_used[i] && slot < 0) slot = i;
        if (g_remote_used[i] && g_remote[i].heard_ms < g_remote[oldest].heard_ms) oldest = i;
    }
    if (slot < 0) slot = oldest;
    const int fresh = !g_remote_used[slot] || g_remote[slot].room_id != r.room_id;
    g_remote[slot] = r;
    g_remote_used[slot] = 1;
    ++g_received;
    pthread_mutex_unlock(&g_rooms_lock);
    if (fresh) {
        char a[INET_ADDRSTRLEN] = "?";
        struct in_addr ia; ia.s_addr = ip;
        inet_ntop(AF_INET, &ia, a, sizeof a);
        rt_logf("rooms: heard room %016llx owned by '%.16s' at %s (%u/%u members)", (unsigned long long)r.room_id, r.owner, a, r.cur_members, r.max_slot);
    }
    return 1;
}

// Copies the rooms of other instances heard in the last ROOM_STALE_MS, oldest-first by room id order of the table.
int rt_rooms_snapshot(RoomInfo* out, int max, uint64_t at_ms) {
    int n = 0;
    pthread_mutex_lock(&g_rooms_lock);
    for (int i = 0; i < ROOM_TABLE; ++i) {
        if (!g_remote_used[i]) continue;
        if (at_ms - g_remote[i].heard_ms > ROOM_STALE_MS) { g_remote_used[i] = 0; continue; }
        if (n < max) out[n++] = g_remote[i];
    }
    pthread_mutex_unlock(&g_rooms_lock);
    return n;
}
uint64_t rt_rooms_now_ms(void) { return now_ms(); }

int rt_rooms_publish(const RoomInfo* r) {
    pthread_mutex_lock(&g_rooms_lock);
    int slot = -1;
    for (int i = 0; i < ROOM_LOCAL; ++i) if (g_local_used[i] && g_local[i].room_id == r->room_id) { slot = i; break; }
    for (int i = 0; i < ROOM_LOCAL && slot < 0; ++i) if (!g_local_used[i]) slot = i;
    if (slot >= 0) { g_local[slot] = *r; g_local_used[slot] = 1; }
    pthread_mutex_unlock(&g_rooms_lock);
    return slot >= 0 ? 0 : -1;
}
void rt_rooms_unpublish(uint64_t room_id) {
    pthread_mutex_lock(&g_rooms_lock);
    for (int i = 0; i < ROOM_LOCAL; ++i) if (g_local_used[i] && g_local[i].room_id == room_id) g_local_used[i] = 0;
    pthread_mutex_unlock(&g_rooms_lock);
}

static void announce_all(void) {
    RoomInfo local[ROOM_LOCAL];
    int n = 0;
    pthread_mutex_lock(&g_rooms_lock);
    for (int i = 0; i < ROOM_LOCAL; ++i) if (g_local_used[i]) local[n++] = g_local[i];
    pthread_mutex_unlock(&g_rooms_lock);
    for (int i = 0; i < n; ++i) {
        uint8_t buf[1024];
        const int len = rt_rooms_encode(&local[i], rt_rooms_nonce(), buf, sizeof buf);
        if (len < 0) continue;
        struct sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_port = htons(g_port);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        if (sendto(g_sock, (const char*)buf, len, 0, (struct sockaddr*)&to, sizeof to) == len) ++g_sent;
        for (int p = 0; p < g_npeers; ++p) {
            to.sin_addr.s_addr = g_peers[p];
            if (sendto(g_sock, (const char*)buf, len, 0, (struct sockaddr*)&to, sizeof to) == len) ++g_sent;
        }
    }
}

static void* rooms_thread(void* arg) {
    (void)arg;
    uint64_t next = 0;
    for (;;) {
        const uint64_t t = now_ms();
        if (t >= next) { announce_all(); next = t + ROOM_ANNOUNCE_MS; }
        uint8_t buf[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = (int)recvfrom(g_sock, (char*)buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
        if (n > 0) rt_rooms_ingest(buf, n, from.sin_addr.s_addr, now_ms());
    }
    return NULL;
}

// Opens the directory socket and starts the announce/listen thread. Only with --online; idempotent.
int rt_rooms_start(void) {
    if (g_started) return 0;
    if (!rt_net_online()) return -1;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
    rsock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == RSOCK_INVALID) { rt_logf("rooms: no socket; the room directory is unavailable"); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
#ifdef _WIN32
    DWORD tmo = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
#else
    struct timeval tv = {0, 200000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(g_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&a, sizeof a) != 0) {
        rt_logf("rooms: UDP port %u could not be bound; the room directory is unavailable", (unsigned)g_port);
        RSOCK_CLOSE(s);
        return -1;
    }
    g_sock = s;
    pthread_t th;
    if (pthread_create(&th, NULL, rooms_thread, NULL) != 0) { RSOCK_CLOSE(s); g_sock = RSOCK_INVALID; return -1; }
    pthread_detach(th);
    g_started = 1;
    rt_logf("rooms: LAN room directory on UDP port %u (broadcast%s%d peer(s)), instance %016llx", (unsigned)g_port,
            g_npeers ? " + " : ", ", g_npeers, (unsigned long long)rt_rooms_nonce());
    return 0;
}
void rt_rooms_report(void) {
    if (g_started) rt_logf("rooms: %llu announcements sent, %llu received, %llu malformed", (unsigned long long)g_sent, (unsigned long long)g_received, (unsigned long long)g_rejected);
}
