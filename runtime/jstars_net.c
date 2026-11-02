// Sockets for the console network library (libSceNet) and the network-status library (libSceNetCtl).
//
// Default: OFF. Without --online the runtime keeps the offline answers it has always given (socket creation
// reports ENETDOWN, NetCtl reports DISCONNECTED). With --online the game gets real UDP sockets on this host.
// Nothing here contacts PlayStation Network: these are ordinary host sockets used by the game's own traffic.
//
// Console details that matter and are implemented here:
//   * SceNetSockaddrIn is 16 bytes: {u8 len, u8 family, u16 port(be), u32 addr(be), u16 vport(be), char zero[6]}.
//     It is NOT the host sockaddr_in, so every address crossing the boundary is translated.
//   * errno values are FreeBSD's (EAGAIN is 35, not Linux's 11), reported through the per-thread cell that
//     sceNetErrnoLoc returns.
//   * Socket ids are the console's small descriptors, not host handles.
//   * sceNetSocketAbort must unblock a thread that is waiting in recvfrom, so every host socket is non-blocking
//     and blocking semantics are emulated in a bounded wait loop that also watches the abort flag.
// This game uses P2P datagram sockets (sceNetSocket(..., family 2, type 6, protocol 0) at eboot.bin+0x2cc760),
// so several logical channels share one real UDP port and are told apart by a 16-bit virtual port. That
// multiplexing is implemented here with a framing of this project's own (see P2P_MAGIC below): both ends of a
// session run JStarsReborn, and the console's own P2P framing is not reproduced. Traffic is therefore NOT
// interoperable with a PlayStation 4.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define NET_LAST_ERROR() WSAGetLastError()
#define NET_WOULDBLOCK WSAEWOULDBLOCK
#define NET_CLOSE(s) closesocket(s)
typedef SOCKET host_socket_t;
#define NET_INVALID INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#define NET_LAST_ERROR() errno
#define NET_WOULDBLOCK EWOULDBLOCK
#define NET_CLOSE(s) close(s)
typedef int host_socket_t;
#define NET_INVALID (-1)
#endif
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

// ---- console constants (values from the public SDK headers; same as upstream shadPS4 net_types.h) ------------
enum { SCE_NET_AF_INET = 2 };
enum { SCE_NET_SOCK_STREAM = 1, SCE_NET_SOCK_DGRAM = 2, SCE_NET_SOCK_RAW = 3, SCE_NET_SOCK_DGRAM_P2P = 6, SCE_NET_SOCK_STREAM_P2P = 10 };
enum { SCE_NET_IPPROTO_UDP = 17 };
enum { SCE_NET_SOL_SOCKET = 0xFFFF };
enum { SCE_NET_SO_REUSEADDR = 0x0004, SCE_NET_SO_BROADCAST = 0x0020, SCE_NET_SO_REUSEPORT = 0x0200,
       SCE_NET_SO_SNDBUF = 0x1001, SCE_NET_SO_RCVBUF = 0x1002, SCE_NET_SO_ERROR = 0x1007, SCE_NET_SO_TYPE = 0x1008,
       SCE_NET_SO_SNDTIMEO = 0x1105, SCE_NET_SO_RCVTIMEO = 0x1106, SCE_NET_SO_NBIO = 0x1200, SCE_NET_SO_PRIORITY = 0x1203 };
enum { SCE_NET_MSG_PEEK = 0x0002, SCE_NET_MSG_WAITALL = 0x0040, SCE_NET_MSG_DONTWAIT = 0x0080 };
// FreeBSD errno numbering, which is what the console reports.
enum { SCE_NET_EINTR = 4, SCE_NET_EBADF = 9, SCE_NET_EACCES = 13, SCE_NET_EFAULT = 14, SCE_NET_EINVAL = 22,
       SCE_NET_EMFILE = 24, SCE_NET_EAGAIN = 35, SCE_NET_EMSGSIZE = 40, SCE_NET_EPROTONOSUPPORT = 43,
       SCE_NET_EOPNOTSUPP = 45, SCE_NET_EAFNOSUPPORT = 47, SCE_NET_EADDRINUSE = 48, SCE_NET_EADDRNOTAVAIL = 49,
       SCE_NET_ENETDOWN = 50, SCE_NET_ECONNRESET = 54, SCE_NET_ETIMEDOUT = 60 };
enum { SCE_NET_ERROR_EBADF = (int)0x80410109 };

typedef struct {
    uint8_t sin_len, sin_family;
    uint16_t sin_port;   // network order
    uint32_t sin_addr;   // network order
    uint16_t sin_vport;  // network order (P2P virtual port)
    char sin_zero[6];
} SceNetSockaddrIn;

// ---- state ---------------------------------------------------------------------------------------------------
#define NET_MAX_SOCKETS 64
#define NET_MAX_GROUPS 16
#define NET_QUEUE_MAX 128          // datagrams held for a virtual port before the oldest is dropped
#define NET_DATAGRAM_MAX 2048

// Framing for P2P (virtual port) sockets. The console multiplexes virtual ports over one UDP port with its own
// header; that header is not reproduced here. This is JStarsReborn's own 8-byte framing, used between two
// JStarsReborn instances only.
static const uint8_t P2P_MAGIC[4] = {'J', 'P', '2', 'P'};
#define P2P_HEADER_SIZE 8

typedef struct QueuedDatagram {
    struct QueuedDatagram* next;
    struct sockaddr_in from;
    uint16_t src_vport;
    uint32_t len;
    uint8_t data[1];
} QueuedDatagram;

// One real UDP port shared by every P2P socket bound to it.
typedef struct {
    int used;
    host_socket_t h;
    uint16_t port;          // real port, host order (0 while unbound)
    int refs;
    pthread_mutex_t mu;
    uint64_t foreign;       // datagrams dropped because they did not carry this framing
    uint64_t undeliverable; // datagrams for a virtual port nothing is bound to
} PortGroup;

typedef struct {
    int used;
    host_socket_t h;      // non-P2P sockets own their host socket; P2P sockets use their group's
    PortGroup* group;     // P2P only
    int type;             // console socket type as asked for
    int nbio;             // guest asked for non-blocking
    int aborted;
    uint16_t vport;       // P2P virtual port, network order (0 = none)
    uint32_t rcv_timeo_us; // 0 = wait forever
    char name[32];
    QueuedDatagram *qhead, *qtail;
    int qcount;
    uint64_t sent, received, send_errors, recv_errors, dropped;
} NetSocket;

static NetSocket g_sock[NET_MAX_SOCKETS];
static PortGroup g_group[NET_MAX_GROUPS];
static pthread_mutex_t g_sock_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_p2p(const NetSocket* e) { return e->type == SCE_NET_SOCK_DGRAM_P2P; }
static host_socket_t sock_handle(const NetSocket* e) { return is_p2p(e) && e->group ? e->group->h : e->h; }
static int g_online;          // --online: the network layer is active
static int g_trace;           // --trace-net: per-call tracing (bounded)
static int g_started;
static __thread int t_neterrno;
static uint64_t g_trace_lines;
#define NET_TRACE_MAX 4000

static void net_trace(const char* fmt, ...) {
    if (!g_trace || g_trace_lines >= NET_TRACE_MAX) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (++g_trace_lines == NET_TRACE_MAX) rt_logf("net: %s", line), rt_logf("net: trace limit of %d lines reached; further calls are counted but not logged", NET_TRACE_MAX);
    else rt_logf("net: %s", line);
}

void rt_net_configure(int online, int trace) { g_online = online; g_trace = trace; }
int rt_net_online(void) { return g_online; }

static int host_to_sce_errno(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEWOULDBLOCK: return SCE_NET_EAGAIN;
    case WSAEINTR: return SCE_NET_EINTR;
    case WSAEBADF: case WSAENOTSOCK: return SCE_NET_EBADF;
    case WSAEACCES: return SCE_NET_EACCES;
    case WSAEFAULT: return SCE_NET_EFAULT;
    case WSAEINVAL: return SCE_NET_EINVAL;
    case WSAEMFILE: return SCE_NET_EMFILE;
    case WSAEMSGSIZE: return SCE_NET_EMSGSIZE;
    case WSAEPROTONOSUPPORT: return SCE_NET_EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return SCE_NET_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return SCE_NET_EAFNOSUPPORT;
    case WSAEADDRINUSE: return SCE_NET_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return SCE_NET_EADDRNOTAVAIL;
    case WSAENETDOWN: return SCE_NET_ENETDOWN;
    case WSAECONNRESET: return SCE_NET_ECONNRESET;
    case WSAETIMEDOUT: return SCE_NET_ETIMEDOUT;
    default: return SCE_NET_EINVAL;
    }
#else
    switch (e) {
    case EWOULDBLOCK: return SCE_NET_EAGAIN;
    case EINTR: return SCE_NET_EINTR;
    case EBADF: case ENOTSOCK: return SCE_NET_EBADF;
    case EACCES: return SCE_NET_EACCES;
    case EFAULT: return SCE_NET_EFAULT;
    case EINVAL: return SCE_NET_EINVAL;
    case EMFILE: return SCE_NET_EMFILE;
    case EMSGSIZE: return SCE_NET_EMSGSIZE;
    case EPROTONOSUPPORT: return SCE_NET_EPROTONOSUPPORT;
    case EOPNOTSUPP: return SCE_NET_EOPNOTSUPP;
    case EAFNOSUPPORT: return SCE_NET_EAFNOSUPPORT;
    case EADDRINUSE: return SCE_NET_EADDRINUSE;
    case EADDRNOTAVAIL: return SCE_NET_EADDRNOTAVAIL;
    case ENETDOWN: return SCE_NET_ENETDOWN;
    case ECONNRESET: return SCE_NET_ECONNRESET;
    case ETIMEDOUT: return SCE_NET_ETIMEDOUT;
    default: return SCE_NET_EINVAL;
    }
#endif
}

static void net_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static int fail(int sce_errno) { t_neterrno = sce_errno; return -1; }
static NetSocket* slot_of(int s) {
    if (s < 1 || s > NET_MAX_SOCKETS) return NULL;
    NetSocket* e = &g_sock[s - 1];
    return e->used ? e : NULL;
}
static void set_nonblocking(host_socket_t h) {
#ifdef _WIN32
    u_long on = 1; ioctlsocket(h, FIONBIO, &on);
#else
    int f = fcntl(h, F_GETFL, 0); fcntl(h, F_SETFL, f | O_NONBLOCK);
#endif
}

// ---- address translation -------------------------------------------------------------------------------------
static int from_guest_addr(const void* guest, socklen_t guest_len, struct sockaddr_in* out, uint16_t* vport) {
    if (!guest || guest_len < 8) return -1;
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof a);
    memcpy(&a, guest, guest_len < (socklen_t)sizeof a ? (size_t)guest_len : sizeof a);
    if (a.sin_family != SCE_NET_AF_INET) return -1;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = a.sin_port;              // already network order on both sides
    memcpy(&out->sin_addr, &a.sin_addr, 4);  // already network order
    if (vport) *vport = a.sin_vport;
    return 0;
}
static void to_guest_addr(const struct sockaddr_in* in, uint16_t vport, void* guest, socklen_t* guest_len) {
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof a);
    a.sin_len = (uint8_t)sizeof a;
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = in->sin_port;
    memcpy(&a.sin_addr, &in->sin_addr, 4);
    a.sin_vport = vport;
    socklen_t want = guest_len && *guest_len > 0 && *guest_len < (socklen_t)sizeof a ? *guest_len : (socklen_t)sizeof a;
    if (guest) memcpy(guest, &a, (size_t)want);
    if (guest_len) *guest_len = (socklen_t)sizeof a;
}
static const char* addr_text(const struct sockaddr_in* a, char* buf, size_t n) {
    unsigned char* b = (unsigned char*)&a->sin_addr;
    snprintf(buf, n, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)ntohs(a->sin_port));
    return buf;
}

// ---- library lifetime ----------------------------------------------------------------------------------------
static int GUEST_ABI net_init(void) {
    if (!g_started) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { rt_logf("net: WSAStartup failed; the network library stays offline"); return 0; }
#endif
        g_started = 1;
        rt_logf("sceNetInit: network layer %s", g_online ? "ENABLED (real host UDP sockets; no PlayStation Network service is contacted)" : "offline (no --online: socket creation reports ENETDOWN)");
    }
    return 0;
}
static int GUEST_ABI net_term(void) { return 0; }
static int g_net_pool;
static int GUEST_ABI net_pool_create(const char* name, int size, int flags) {
    (void)size; (void)flags;
    rt_logf("sceNetPoolCreate('%s') -> pool %d (bookkeeping only)", name ? name : "", g_net_pool + 1);
    return ++g_net_pool;
}
static int GUEST_ABI net_pool_destroy(int id) { (void)id; return 0; }
static int* GUEST_ABI net_errno_loc(void) { return &t_neterrno; }
static uint16_t GUEST_ABI net_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t GUEST_ABI net_htonl(uint32_t v) { return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24); }
static int GUEST_ABI net_inet_pton(int af, const char* src, void* dst) { return af == SCE_NET_AF_INET ? inet_pton(AF_INET, src, dst) : 0; }

// ---- sockets -------------------------------------------------------------------------------------------------
static int GUEST_ABI net_socket(const char* name, int family, int type, int protocol) {
    if (!g_online) {
        rt_logf("sceNetSocket('%s', family %d, type %d, protocol %d): the runtime is offline (start with --online) -> ENETDOWN",
                name ? name : "", family, type, protocol);
        return fail(SCE_NET_ENETDOWN);
    }
    if (family != SCE_NET_AF_INET) return fail(SCE_NET_EAFNOSUPPORT);
    if (type != SCE_NET_SOCK_DGRAM && type != SCE_NET_SOCK_DGRAM_P2P) {
        rt_logf("sceNetSocket('%s'): socket type %d is not implemented (only datagram sockets are) -> EPROTONOSUPPORT", name ? name : "", type);
        return fail(SCE_NET_EPROTONOSUPPORT);
    }
    // A P2P socket gets no host socket of its own: it joins the group of its real UDP port when it is bound.
    host_socket_t h = NET_INVALID;
    if (type != SCE_NET_SOCK_DGRAM_P2P) {
        h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (h == NET_INVALID) return fail(host_to_sce_errno(NET_LAST_ERROR()));
        set_nonblocking(h);
    }
    pthread_mutex_lock(&g_sock_lock);
    int id = 0;
    for (int i = 0; i < NET_MAX_SOCKETS; ++i)
        if (!g_sock[i].used) {
            memset(&g_sock[i], 0, sizeof g_sock[i]);
            g_sock[i].used = 1;
            g_sock[i].h = h;
            g_sock[i].type = type;
            snprintf(g_sock[i].name, sizeof g_sock[i].name, "%s", name ? name : "");
            id = i + 1;
            break;
        }
    pthread_mutex_unlock(&g_sock_lock);
    if (!id) { if (h != NET_INVALID) NET_CLOSE(h); return fail(SCE_NET_EMFILE); }
    rt_logf("sceNetSocket('%s', type %s) -> socket %d", name ? name : "", type == SCE_NET_SOCK_DGRAM_P2P ? "DGRAM_P2P" : "DGRAM", id);
    return id;
}

// A P2P socket does not own a host socket: it joins the group of every socket on the same real UDP port, and is
// told apart inside that group by its virtual port.
static PortGroup* group_for(uint16_t port_host, const struct sockaddr_in* want, int* err) {
    for (int i = 0; i < NET_MAX_GROUPS; ++i)
        if (g_group[i].used && g_group[i].port == port_host) { g_group[i].refs++; return &g_group[i]; }
    for (int i = 0; i < NET_MAX_GROUPS; ++i) {
        if (g_group[i].used) continue;
        host_socket_t h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (h == NET_INVALID) { *err = host_to_sce_errno(NET_LAST_ERROR()); return NULL; }
        int on = 1;
        setsockopt(h, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
        if (bind(h, (const struct sockaddr*)want, sizeof *want) != 0) {
            *err = host_to_sce_errno(NET_LAST_ERROR());
            NET_CLOSE(h);
            return NULL;
        }
        set_nonblocking(h);
        struct sockaddr_in bound;
        socklen_t bl = sizeof bound;
        uint16_t real = port_host;
        if (getsockname(h, (struct sockaddr*)&bound, &bl) == 0) real = ntohs(bound.sin_port);
        memset(&g_group[i], 0, sizeof g_group[i]);
        g_group[i].used = 1;
        g_group[i].h = h;
        g_group[i].port = real;
        g_group[i].refs = 1;
        pthread_mutex_init(&g_group[i].mu, NULL);
        return &g_group[i];
    }
    *err = SCE_NET_EMFILE;
    return NULL;
}

static void group_release(PortGroup* g) {
    if (!g) return;
    if (--g->refs > 0) return;
    rt_logf("net: UDP port %u released (%" PRIu64 " datagrams without this framing, %" PRIu64 " for an unknown virtual port)",
            (unsigned)g->port, g->foreign, g->undeliverable);
    NET_CLOSE(g->h);
    pthread_mutex_destroy(&g->mu);
    g->used = 0;
}

static int GUEST_ABI net_bind(int s, const void* addr, socklen_t addrlen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    struct sockaddr_in host;
    uint16_t vport = 0;
    if (from_guest_addr(addr, addrlen, &host, &vport) != 0) return fail(SCE_NET_EINVAL);
    char t[64];
    if (is_p2p(e)) {
        pthread_mutex_lock(&g_sock_lock);
        for (int i = 0; i < NET_MAX_SOCKETS; ++i)
            if (g_sock[i].used && &g_sock[i] != e && is_p2p(&g_sock[i]) && g_sock[i].group &&
                g_sock[i].group->port == ntohs(host.sin_port) && g_sock[i].vport == vport && vport) {
                pthread_mutex_unlock(&g_sock_lock);
                rt_logf("sceNetBind(socket %d): virtual port %u on UDP port %u is already in use", s, (unsigned)ntohs(vport), (unsigned)ntohs(host.sin_port));
                return fail(SCE_NET_EADDRINUSE);
            }
        int err = 0;
        PortGroup* g = group_for(ntohs(host.sin_port), &host, &err);
        pthread_mutex_unlock(&g_sock_lock);
        if (!g) return fail(err);
        e->group = g;
        e->vport = vport;
        struct sockaddr_in shown = host;
        shown.sin_port = htons(g->port);
        rt_logf("sceNetBind(socket %d '%s', P2P) -> %s virtual port %u (%d socket(s) share this UDP port)",
                s, e->name, addr_text(&shown, t, sizeof t), (unsigned)ntohs(vport), g->refs);
        return 0;
    }
    if (bind(e->h, (struct sockaddr*)&host, sizeof host) != 0) {
        int err = host_to_sce_errno(NET_LAST_ERROR());
        rt_logf("sceNetBind(socket %d): host bind failed (console errno %d)", s, err);
        return fail(err);
    }
    e->vport = vport;
    struct sockaddr_in bound;
    socklen_t bl = sizeof bound;
    if (getsockname(e->h, (struct sockaddr*)&bound, &bl) == 0)
        rt_logf("sceNetBind(socket %d '%s') -> %s", s, e->name, addr_text(&bound, t, sizeof t));
    return 0;
}

static int GUEST_ABI net_sendto(int s, const void* buf, size_t len, int flags, const void* to, socklen_t tolen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    if (!buf && len) return fail(SCE_NET_EFAULT);
    struct sockaddr_in host;
    uint16_t dst_vport = 0;
    if (to && from_guest_addr(to, tolen, &host, &dst_vport) != 0) return fail(SCE_NET_EINVAL);
    if (!to) return fail(SCE_NET_EINVAL);   // these sockets are never connected, so a destination is required
    int n;
    if (is_p2p(e)) {
        if (!e->group) {
            // Sending before bind: join an ephemeral group so the datagram has a source port to come back to.
            struct sockaddr_in any;
            memset(&any, 0, sizeof any);
            any.sin_family = AF_INET;
            int err = 0;
            pthread_mutex_lock(&g_sock_lock);
            e->group = group_for(0, &any, &err);
            pthread_mutex_unlock(&g_sock_lock);
            if (!e->group) return fail(err);
        }
        if (len > NET_DATAGRAM_MAX - P2P_HEADER_SIZE) return fail(SCE_NET_EMSGSIZE);
        uint8_t frame[NET_DATAGRAM_MAX];
        memcpy(frame, P2P_MAGIC, 4);
        memcpy(frame + 4, &e->vport, 2);
        memcpy(frame + 6, &dst_vport, 2);
        memcpy(frame + P2P_HEADER_SIZE, buf, len);
        n = (int)sendto(e->group->h, (const char*)frame, (int)(len + P2P_HEADER_SIZE), 0, (struct sockaddr*)&host, sizeof host);
        if (n >= P2P_HEADER_SIZE) n -= P2P_HEADER_SIZE;
    } else {
        n = (int)sendto(e->h, (const char*)buf, (int)len, 0, (struct sockaddr*)&host, sizeof host);
    }
    if (n < 0) {
        int err = host_to_sce_errno(NET_LAST_ERROR());
        e->send_errors++;
        net_trace("sendto(socket %d, %zu bytes) failed, console errno %d", s, len, err);
        return fail(err);
    }
    e->sent++;
    if (g_trace) {
        char t[64];
        net_trace("sendto(socket %d '%s', %d bytes, flags 0x%x) -> %s virtual port %u", s, e->name, n, flags,
                  addr_text(&host, t, sizeof t), (unsigned)ntohs(dst_vport));
    }
    return n;
}

// Puts a datagram on the queue of the P2P socket that owns `vport` in this group. The group lock is held.
static void queue_for_vport(PortGroup* g, uint16_t vport, const struct sockaddr_in* from, uint16_t src_vport,
                            const uint8_t* payload, uint32_t len) {
    NetSocket* target = NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; ++i)
        if (g_sock[i].used && g_sock[i].group == g && g_sock[i].vport == vport) { target = &g_sock[i]; break; }
    if (!target) { g->undeliverable++; return; }
    QueuedDatagram* d = malloc(sizeof(QueuedDatagram) + len);
    if (!d) { target->dropped++; return; }
    d->next = NULL;
    d->from = *from;
    d->src_vport = src_vport;
    d->len = len;
    memcpy(d->data, payload, len);
    if (target->qcount >= NET_QUEUE_MAX) {
        QueuedDatagram* old = target->qhead;
        target->qhead = old->next;
        if (!target->qhead) target->qtail = NULL;
        target->qcount--;
        target->dropped++;
        free(old);
    }
    if (target->qtail) target->qtail->next = d; else target->qhead = d;
    target->qtail = d;
    target->qcount++;
}

// One non-blocking read from the group's shared socket, demultiplexed by virtual port. Returns 1 when something
// was read (for this socket or another one), 0 when the socket had nothing, -1 on a host error (in *err).
static int group_pump(PortGroup* g, int* err) {
    uint8_t frame[NET_DATAGRAM_MAX];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    memset(&from, 0, sizeof from);
    int n = (int)recvfrom(g->h, (char*)frame, (int)sizeof frame, 0, (struct sockaddr*)&from, &fl);
    if (n < 0) {
        int herr = NET_LAST_ERROR();
        if (herr == NET_WOULDBLOCK) return 0;
        *err = host_to_sce_errno(herr);
        return -1;
    }
    if (n < P2P_HEADER_SIZE || memcmp(frame, P2P_MAGIC, 4) != 0) {
        if (g->foreign++ == 0)
            rt_logf("net: a datagram on UDP port %u does not carry this build's virtual-port framing and was dropped "
                    "(both machines must run JStarsReborn; a PlayStation 4 is not compatible)", (unsigned)g->port);
        return 1;
    }
    uint16_t src_vport, dst_vport;
    memcpy(&src_vport, frame + 4, 2);
    memcpy(&dst_vport, frame + 6, 2);
    queue_for_vport(g, dst_vport, &from, src_vport, frame + P2P_HEADER_SIZE, (uint32_t)(n - P2P_HEADER_SIZE));
    return 1;
}

// Takes the oldest queued datagram for this socket, if there is one. The group lock is held.
static int take_queued(NetSocket* e, void* buf, size_t len, int peek, struct sockaddr_in* from, uint16_t* src_vport) {
    QueuedDatagram* d = e->qhead;
    if (!d) return -1;
    uint32_t copy = d->len < len ? d->len : (uint32_t)len;
    memcpy(buf, d->data, copy);
    if (from) *from = d->from;
    if (src_vport) *src_vport = d->src_vport;
    if (!peek) {
        e->qhead = d->next;
        if (!e->qhead) e->qtail = NULL;
        e->qcount--;
        free(d);
    }
    return (int)copy;
}

static int GUEST_ABI net_recvfrom(int s, void* buf, size_t len, int flags, void* from, socklen_t* fromlen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    if (!buf && len) return fail(SCE_NET_EFAULT);
    const int want_block = !e->nbio && !(flags & SCE_NET_MSG_DONTWAIT);
    const int peek = (flags & SCE_NET_MSG_PEEK) != 0;
    uint64_t waited_us = 0;
    for (;;) {
        if (e->aborted) { e->aborted = 0; return fail(SCE_NET_EINTR); }
        struct sockaddr_in host;
        memset(&host, 0, sizeof host);
        uint16_t src_vport = 0;
        int n = -1;
        if (is_p2p(e)) {
            if (!e->group) return fail(SCE_NET_EAGAIN);   // nothing can arrive before this socket has a port
            PortGroup* g = e->group;
            pthread_mutex_lock(&g->mu);
            n = take_queued(e, buf, len, peek, &host, &src_vport);
            while (n < 0) {
                int err = 0;
                int got = group_pump(g, &err);
                if (got < 0) { pthread_mutex_unlock(&g->mu); e->recv_errors++; return fail(err); }
                if (got == 0) break;
                n = take_queued(e, buf, len, peek, &host, &src_vport);
            }
            pthread_mutex_unlock(&g->mu);
        } else {
            socklen_t hl = sizeof host;
            n = (int)recvfrom(e->h, (char*)buf, (int)len, peek ? MSG_PEEK : 0, (struct sockaddr*)&host, &hl);
            if (n < 0) {
                int herr = NET_LAST_ERROR();
                if (herr != NET_WOULDBLOCK) {
                    int err = host_to_sce_errno(herr);
                    e->recv_errors++;
                    net_trace("recvfrom(socket %d) failed, console errno %d", s, err);
                    return fail(err);
                }
            }
        }
        if (n >= 0) {
            e->received++;
            if (from) to_guest_addr(&host, src_vport, from, fromlen);
            if (g_trace) {
                char t[64];
                net_trace("recvfrom(socket %d '%s') <- %d bytes from %s virtual port %u", s, e->name, n,
                          addr_text(&host, t, sizeof t), (unsigned)ntohs(src_vport));
            }
            return n;
        }
        if (!want_block) return fail(SCE_NET_EAGAIN);
        if (e->rcv_timeo_us && waited_us >= e->rcv_timeo_us) return fail(SCE_NET_EAGAIN);
        net_sleep_ms(1);
        waited_us += 1000;
    }
}

static int GUEST_ABI net_setsockopt(int s, int level, int optname, const void* optval, socklen_t optlen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    if (!optval || optlen < 4) return fail(SCE_NET_EINVAL);
    const int v = *(const int*)optval;
    if (sock_handle(e) == NET_INVALID && optname != SCE_NET_SO_NBIO && optname != SCE_NET_SO_RCVTIMEO) {
        // A P2P socket has no host socket before it is bound; buffer sizes and flags set this early are accepted
        // and recorded in the trace, which is what the console does for options it applies at bind time.
        net_trace("setsockopt(socket %d, option 0x%x) before bind: recorded only", s, optname);
        return 0;
    }
    if (level != SCE_NET_SOL_SOCKET) {
        net_trace("setsockopt(socket %d, level 0x%x, option 0x%x) accepted without effect", s, level, optname);
        return 0;
    }
    switch (optname) {
    case SCE_NET_SO_NBIO:
        e->nbio = v != 0;
        net_trace("setsockopt(socket %d, SO_NBIO %d)", s, v);
        return 0;
    case SCE_NET_SO_REUSEADDR: {
        int on = v != 0;
        setsockopt(sock_handle(e), SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
        return 0;
    }
    case SCE_NET_SO_BROADCAST: {
        int on = v != 0;
        setsockopt(sock_handle(e), SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
        return 0;
    }
    case SCE_NET_SO_SNDBUF: setsockopt(sock_handle(e), SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof v); return 0;
    case SCE_NET_SO_RCVBUF: setsockopt(sock_handle(e), SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof v); return 0;
    case SCE_NET_SO_RCVTIMEO: e->rcv_timeo_us = (uint32_t)v; return 0;   // the console takes microseconds
    case SCE_NET_SO_SNDTIMEO: return 0;
    case SCE_NET_SO_REUSEPORT:
    case SCE_NET_SO_PRIORITY:
        net_trace("setsockopt(socket %d, option 0x%x) accepted without effect", s, optname);
        return 0;
    default:
        net_trace("setsockopt(socket %d, SOL_SOCKET option 0x%x) accepted without effect", s, optname);
        return 0;
    }
}

static int GUEST_ABI net_shutdown(int s, int how) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
#ifdef _WIN32
    shutdown(sock_handle(e), how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH);
#else
    shutdown(sock_handle(e), how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR);
#endif
    net_trace("shutdown(socket %d, how %d)", s, how);
    return 0;
}

static int GUEST_ABI net_socket_close(int s) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    // A P2P socket gives back its share of the UDP port and drops whatever is still queued for it. The group lock is
    // taken first, as the receive path does, so no datagram is queued for this socket while it is being closed.
    PortGroup* g = is_p2p(e) ? e->group : NULL;
    if (g) pthread_mutex_lock(&g->mu);
    pthread_mutex_lock(&g_sock_lock);
    rt_logf("sceNetSocketClose(socket %d '%s'): %" PRIu64 " datagrams sent, %" PRIu64 " received, %" PRIu64 " send errors, %" PRIu64 " receive errors, %d still queued",
            s, e->name, e->sent, e->received, e->send_errors, e->recv_errors, e->qcount);
    if (e->h != NET_INVALID) NET_CLOSE(e->h);
    QueuedDatagram* q = e->qhead;
    e->qhead = e->qtail = NULL;
    e->qcount = 0;
    e->group = NULL;
    e->used = 0;
    pthread_mutex_unlock(&g_sock_lock);
    if (g) pthread_mutex_unlock(&g->mu);
    while (q) { QueuedDatagram* n = q->next; free(q); q = n; }
    if (g) {
        pthread_mutex_lock(&g_sock_lock);
        group_release(g);
        pthread_mutex_unlock(&g_sock_lock);
    }
    return 0;
}

// Unblocks a thread waiting in recvfrom on this socket (the console's way of cancelling a blocking call).
static int GUEST_ABI net_socket_abort(int s, int flags) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    e->aborted = 1;
    net_trace("socketAbort(socket %d, flags 0x%x)", s, flags);
    return 0;
}

// ---- libSceNetCtl: the status of this host's network ----------------------------------------------------------
enum { NETCTL_ERR_NOT_INITIALIZED = (int)0x80412107, NETCTL_ERR_NOT_CONNECTED = (int)0x80412108, NETCTL_ERR_INVALID_ADDR = (int)0x80412105 };
enum { NETCTL_STATE_DISCONNECTED = 0, NETCTL_STATE_IPOBTAINED = 3 };

// The address of the interface this host would use to reach other machines, in network order. Found by asking the
// routing table through a connected UDP socket (no packet is sent).
static uint32_t local_ipv4(void) {
    static uint32_t cached;
    static int tried;
    if (tried) return cached;
    tried = 1;
    host_socket_t h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h == NET_INVALID) return 0;
    struct sockaddr_in probe;
    memset(&probe, 0, sizeof probe);
    probe.sin_family = AF_INET;
    probe.sin_port = htons(9);               // discard port; a connect on UDP sends nothing
    probe.sin_addr.s_addr = inet_addr("192.168.1.1");
    if (connect(h, (struct sockaddr*)&probe, sizeof probe) == 0) {
        struct sockaddr_in me;
        socklen_t ml = sizeof me;
        if (getsockname(h, (struct sockaddr*)&me, &ml) == 0) memcpy(&cached, &me.sin_addr, 4);
    }
    NET_CLOSE(h);
    return cached;
}

static int GUEST_ABI netctl_init(void) { net_init(); return 0; }
static void GUEST_ABI netctl_term(void) {}
static int GUEST_ABI netctl_get_state(int* state) {
    if (!state) return NETCTL_ERR_NOT_INITIALIZED;
    *state = (g_online && local_ipv4()) ? NETCTL_STATE_IPOBTAINED : NETCTL_STATE_DISCONNECTED;
    return 0;
}
// SceNetCtlInfo is a union of every field; the code below fills only the fields this game asks for.
enum { NETCTL_INFO_IP_ADDRESS = 14, NETCTL_INFO_DEVICE = 1, NETCTL_INFO_LINK = 6, NETCTL_INFO_DEFAULT_ROUTE = 17, NETCTL_INFO_NETMASK = 16 };
static int GUEST_ABI netctl_get_info(int code, void* info) {
    if (!info) return NETCTL_ERR_INVALID_ADDR;
    if (!g_online) return NETCTL_ERR_NOT_CONNECTED;
    uint32_t ip = local_ipv4();
    if (!ip) return NETCTL_ERR_NOT_CONNECTED;
    switch (code) {
    case NETCTL_INFO_IP_ADDRESS: {
        unsigned char* b = (unsigned char*)&ip;
        char text[16];
        snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        memset(info, 0, 16);
        memcpy(info, text, strlen(text));
        rt_logf("sceNetCtlGetInfo(IP address) -> %s", text);
        return 0;
    }
    case NETCTL_INFO_DEVICE: *(int*)info = 0; return 0;   // wired
    case NETCTL_INFO_LINK: *(int*)info = 1; return 0;     // up
    default:
        rt_logf("sceNetCtlGetInfo: information code %d is not implemented -> NOT_CONNECTED", code);
        return NETCTL_ERR_NOT_CONNECTED;
    }
}
// NAT type 1..3. Reported as 2 (moderate): this host's real NAT behavior is not probed, and claiming type 1 would be a guess.
static int GUEST_ABI netctl_get_nat_info(void* nat_info) {
    if (!nat_info) return NETCTL_ERR_INVALID_ADDR;
    if (!g_online) return NETCTL_ERR_NOT_CONNECTED;
    uint32_t* w = (uint32_t*)nat_info;   // { size, stunStatus, natType, mappedAddr }
    w[0] = 16;
    w[1] = 1;
    w[2] = 2;
    w[3] = local_ipv4();
    rt_logf("sceNetCtlGetNatInfo -> NAT type 2 (not probed; reported so the game does not treat the connection as unusable)");
    return 0;
}

void rt_sleep_ms_for_selftest(int ms) { net_sleep_ms(ms); }

void rt_net_report(void) {
    if (!g_started) return;
    for (int i = 0; i < NET_MAX_SOCKETS; ++i)
        if (g_sock[i].used)
            rt_logf("net: socket %d '%s' still open at shutdown: %" PRIu64 " sent, %" PRIu64 " received, %" PRIu64 " dropped",
                    i + 1, g_sock[i].name, g_sock[i].sent, g_sock[i].received, g_sock[i].dropped);
}

const HostSymbol net_symbols[] = {
    {"sceNetInit", "libSceNet", (void*)net_init, 0},
    {"sceNetTerm", "libSceNet", (void*)net_term, 0},
    {"sceNetPoolCreate", "libSceNet", (void*)net_pool_create, 0},
    {"sceNetPoolDestroy", "libSceNet", (void*)net_pool_destroy, 0},
    {"sceNetErrnoLoc", "libSceNet", (void*)net_errno_loc, 0},
    {"sceNetHtons", "libSceNet", (void*)net_htons, 0},
    {"sceNetHtonl", "libSceNet", (void*)net_htonl, 0},
    {"sceNetInetPton", "libSceNet", (void*)net_inet_pton, 0},
    {"sceNetSocket", "libSceNet", (void*)net_socket, 0},
    {"sceNetBind", "libSceNet", (void*)net_bind, 0},
    {"sceNetSendto", "libSceNet", (void*)net_sendto, 0},
    {"sceNetRecvfrom", "libSceNet", (void*)net_recvfrom, 0},
    {"sceNetSetsockopt", "libSceNet", (void*)net_setsockopt, 0},
    {"sceNetShutdown", "libSceNet", (void*)net_shutdown, 0},
    {"sceNetSocketClose", "libSceNet", (void*)net_socket_close, 0},
    {"sceNetSocketAbort", "libSceNet", (void*)net_socket_abort, 0},
    {"sceNetCtlInit", "libSceNetCtl", (void*)netctl_init, 0},
    {"sceNetCtlTerm", "libSceNetCtl", (void*)netctl_term, 0},
    {"sceNetCtlGetState", "libSceNetCtl", (void*)netctl_get_state, 0},
    {"sceNetCtlGetInfo", "libSceNetCtl", (void*)netctl_get_info, 0},
    {"sceNetCtlGetNatInfo", "libSceNetCtl", (void*)netctl_get_nat_info, 0},
};
const size_t net_symbol_count = sizeof net_symbols / sizeof *net_symbols;
