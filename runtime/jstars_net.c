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
// Virtual ports (SOCK_DGRAM_P2P) are recorded and logged but not yet multiplexed; see docs/ONLINE.md.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
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
typedef struct {
    int used;
    host_socket_t h;
    int type;             // console socket type as asked for
    int nbio;             // guest asked for non-blocking
    int aborted;
    uint16_t vport;       // P2P virtual port this socket was bound to (0 = none)
    uint32_t rcv_timeo_us; // 0 = wait forever
    char name[32];
    uint64_t sent, received, send_errors, recv_errors;
} NetSocket;

static NetSocket g_sock[NET_MAX_SOCKETS];
static pthread_mutex_t g_sock_lock = PTHREAD_MUTEX_INITIALIZER;
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
    host_socket_t h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h == NET_INVALID) return fail(host_to_sce_errno(NET_LAST_ERROR()));
    set_nonblocking(h);
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
    if (!id) { NET_CLOSE(h); return fail(SCE_NET_EMFILE); }
    rt_logf("sceNetSocket('%s', type %s) -> socket %d", name ? name : "", type == SCE_NET_SOCK_DGRAM_P2P ? "DGRAM_P2P" : "DGRAM", id);
    return id;
}

static int GUEST_ABI net_bind(int s, const void* addr, socklen_t addrlen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    struct sockaddr_in host;
    uint16_t vport = 0;
    if (from_guest_addr(addr, addrlen, &host, &vport) != 0) return fail(SCE_NET_EINVAL);
    if (bind(e->h, (struct sockaddr*)&host, sizeof host) != 0) {
        int err = host_to_sce_errno(NET_LAST_ERROR());
        rt_logf("sceNetBind(socket %d): host bind failed (console errno %d)", s, err);
        return fail(err);
    }
    e->vport = vport;
    struct sockaddr_in bound;
    socklen_t bl = sizeof bound;
    char t[64];
    if (getsockname(e->h, (struct sockaddr*)&bound, &bl) == 0)
        rt_logf("sceNetBind(socket %d '%s') -> %s%s", s, e->name, addr_text(&bound, t, sizeof t),
                e->type == SCE_NET_SOCK_DGRAM_P2P ? " (P2P socket; its virtual port is recorded but not multiplexed yet)" : "");
    if (e->type == SCE_NET_SOCK_DGRAM_P2P && vport)
        rt_logf("sceNetBind(socket %d): P2P virtual port %u requested. Virtual-port multiplexing is NOT implemented; "
                "traffic for this socket uses the real UDP port only.", s, (unsigned)ntohs(vport));
    return 0;
}

static int GUEST_ABI net_sendto(int s, const void* buf, size_t len, int flags, const void* to, socklen_t tolen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    if (!buf && len) return fail(SCE_NET_EFAULT);
    struct sockaddr_in host;
    uint16_t vport = 0;
    if (to && from_guest_addr(to, tolen, &host, &vport) != 0) return fail(SCE_NET_EINVAL);
    int n = (int)sendto(e->h, (const char*)buf, (int)len, 0, to ? (struct sockaddr*)&host : NULL, to ? sizeof host : 0);
    if (n < 0) {
        int err = host_to_sce_errno(NET_LAST_ERROR());
        e->send_errors++;
        net_trace("sendto(socket %d, %zu bytes) failed, console errno %d", s, len, err);
        return fail(err);
    }
    e->sent++;
    if (g_trace) {
        char t[64];
        net_trace("sendto(socket %d '%s', %d bytes, flags 0x%x) -> %s", s, e->name, n, flags, to ? addr_text(&host, t, sizeof t) : "(connected peer)");
    }
    return n;
}

static int GUEST_ABI net_recvfrom(int s, void* buf, size_t len, int flags, void* from, socklen_t* fromlen) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    if (!buf && len) return fail(SCE_NET_EFAULT);
    const int want_block = !e->nbio && !(flags & SCE_NET_MSG_DONTWAIT);
    const int host_flags = (flags & SCE_NET_MSG_PEEK) ? MSG_PEEK : 0;
    uint64_t waited_us = 0;
    for (;;) {
        if (e->aborted) { e->aborted = 0; return fail(SCE_NET_EINTR); }
        struct sockaddr_in host;
        socklen_t hl = sizeof host;
        memset(&host, 0, sizeof host);
        int n = (int)recvfrom(e->h, (char*)buf, (int)len, host_flags, (struct sockaddr*)&host, &hl);
        if (n >= 0) {
            e->received++;
            if (from) to_guest_addr(&host, 0, from, fromlen);
            if (g_trace) {
                char t[64];
                net_trace("recvfrom(socket %d '%s') <- %d bytes from %s", s, e->name, n, addr_text(&host, t, sizeof t));
            }
            return n;
        }
        int herr = NET_LAST_ERROR();
        if (herr != NET_WOULDBLOCK) {
            int err = host_to_sce_errno(herr);
            e->recv_errors++;
            net_trace("recvfrom(socket %d) failed, console errno %d", s, err);
            return fail(err);
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
        setsockopt(e->h, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
        return 0;
    }
    case SCE_NET_SO_BROADCAST: {
        int on = v != 0;
        setsockopt(e->h, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
        return 0;
    }
    case SCE_NET_SO_SNDBUF: setsockopt(e->h, SOL_SOCKET, SO_SNDBUF, (const char*)&v, sizeof v); return 0;
    case SCE_NET_SO_RCVBUF: setsockopt(e->h, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof v); return 0;
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
    shutdown(e->h, how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH);
#else
    shutdown(e->h, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR);
#endif
    net_trace("shutdown(socket %d, how %d)", s, how);
    return 0;
}

static int GUEST_ABI net_socket_close(int s) {
    NetSocket* e = slot_of(s);
    if (!e) return fail(SCE_NET_EBADF);
    pthread_mutex_lock(&g_sock_lock);
    rt_logf("sceNetSocketClose(socket %d '%s'): %" PRIu64 " datagrams sent, %" PRIu64 " received, %" PRIu64 " send errors, %" PRIu64 " receive errors",
            s, e->name, e->sent, e->received, e->send_errors, e->recv_errors);
    NET_CLOSE(e->h);
    e->used = 0;
    pthread_mutex_unlock(&g_sock_lock);
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
            rt_logf("net: socket %d '%s' still open at shutdown: %" PRIu64 " sent, %" PRIu64 " received", i + 1, g_sock[i].name, g_sock[i].sent, g_sock[i].received);
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
