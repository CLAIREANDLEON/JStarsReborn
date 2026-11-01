// Console file system as the game sees it, backed by the host. Mounts:
//   /app0       the user's own game folder (--game-root), read-only: writes are refused
//   /savedata0, /savedata1, /data, /temp0, /temp, /download0, /download1
//               writable folders under --user-dir (never inside the game folder)
//   /dev/null   discards writes, reads end immediately
// Everything else does not exist (ENOENT). Descriptors share the number space of the event queues.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

// FreeBSD errno values used by the console libc
enum { K_EPERM = 1, K_ENOENT = 2, K_EIO = 5, K_EBADF = 9, K_EACCES = 13, K_EBUSY = 16, K_EEXIST = 17, K_ENOTDIR = 20, K_EISDIR = 21,
       K_EINVAL = 22, K_EMFILE = 24, K_ENOSPC = 28, K_EROFS = 30, K_ENAMETOOLONG = 63, K_ENOTEMPTY = 66 };
#define SCE_ERR(e) ((int)(0x80020000u | (unsigned)(e)))
#define SCE_ERR64(e) ((int64_t)(int32_t)SCE_ERR(e))

static int kerrno(int os) {
    switch (os) {
    case OS_E_NOENT: return K_ENOENT;
    case OS_E_ACCES: return K_EACCES;
    case OS_E_EXIST: return K_EEXIST;
    case OS_E_NOTDIR: return K_ENOTDIR;
    case OS_E_ISDIR: return K_EISDIR;
    case OS_E_INVAL: return K_EINVAL;
    case OS_E_NOSPC: return K_ENOSPC;
    case OS_E_NOTEMPTY: return K_ENOTEMPTY;
    case OS_E_MFILE: return K_EMFILE;
    case OS_E_BUSY: return K_EBUSY;
    case OS_E_NAMETOOLONG: return K_ENAMETOOLONG;
    default: return K_EIO;
    }
}

// ---- mounts -----------------------------------------------------------------------------------------------
#define RT_SAVE_SLOTS 15
static char g_game_root[1024], g_user_root[1024];
static char g_save_mount[RT_SAVE_SLOTS][1536];
static int g_save_ro[RT_SAVE_SLOTS];
void rt_fs_save_mount(int slot, const char* host_dir, int read_only) {
    if (slot < 0 || slot >= RT_SAVE_SLOTS) return;
    snprintf(g_save_mount[slot], sizeof g_save_mount[slot], "%s", host_dir ? host_dir : "");
    g_save_ro[slot] = read_only;
}
const char* rt_fs_user_root(void) { return g_user_root; }
static uint64_t g_opens, g_open_fail, g_bytes_read;

static void reset_mounts(void);
void rt_fs_configure(const char* game_root, const char* user_root) {
    reset_mounts();
    snprintf(g_game_root, sizeof g_game_root, "%s", game_root ? game_root : "");
    snprintf(g_user_root, sizeof g_user_root, "%s", user_root ? user_root : "");
    // trailing separators are not wanted
    char* roots[2] = {g_game_root, g_user_root};
    for (int i = 0; i < 2; ++i) {
        size_t n = strlen(roots[i]);
        while (n > 1 && (roots[i][n - 1] == '/' || roots[i][n - 1] == '\\')) roots[i][--n] = 0;
    }
    rt_logf("fs: /app0 -> %s (read-only), writable mounts under %s", g_game_root[0] ? g_game_root : "(not configured)", g_user_root[0] ? g_user_root : "(not configured)");
}

typedef enum { M_NONE, M_APP0, M_USER, M_NULL } MountKind;
// Writable mounts are plain folders under the user directory, created the first time they are used.
static char made[16][32]; static int nmade; static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static void reset_mounts(void) { pthread_mutex_lock(&lk); nmade = 0; pthread_mutex_unlock(&lk); }
static void ensure_user_mount(const char* mount) {
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nmade; ++i) if (!strcmp(made[i], mount) && made[i][31] == 1) { pthread_mutex_unlock(&lk); return; }
    char dir[1536];
    os_mkdir(g_user_root);
    snprintf(dir, sizeof dir, "%s/%s", g_user_root, mount);
    os_mkdir(dir);
    if (nmade < 16) { snprintf(made[nmade], 31, "%s", mount); made[nmade][31] = 1; ++nmade; }
    pthread_mutex_unlock(&lk);
}
// Resolves a console path to a host path. Returns the mount kind; *err gets a console errno for M_NONE.
static MountKind resolve(const char* path, char* host, size_t cap, int* err) {
    *err = K_ENOENT;
    if (!path || path[0] != '/') return M_NONE;
    // normalise: collapse '//', '.', handle '..' without leaving the mount
    char comp[64][256]; int nc = 0;
    const char* p = path;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        size_t n = 0; char c[256];
        while (p[n] && p[n] != '/') { if (n < 255) c[n] = p[n]; ++n; }
        if (n > 255) { *err = K_ENAMETOOLONG; return M_NONE; }
        memcpy(c, p, n); c[n] = 0; p += n;
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (nc > 1) --nc; else { *err = K_EACCES; return M_NONE; } continue; }
        if (nc >= 64) { *err = K_ENAMETOOLONG; return M_NONE; }
        memcpy(comp[nc++], c, n + 1);
    }
    if (nc == 0) return M_NONE;
    if (!strcmp(comp[0], "dev")) return (nc == 2 && !strcmp(comp[1], "null")) ? M_NULL : M_NONE;
    const char* root = NULL; MountKind kind = M_NONE;
    if (!strcmp(comp[0], "app0")) { root = g_game_root; kind = M_APP0; }
    else if (!strncmp(comp[0], "savedata", 8) && comp[0][8] >= '0' && comp[0][8] <= '9') {
        // save-data mount points exist only while sceSaveDataMount has them mounted
        int slot = atoi(comp[0] + 8);
        if (slot < 0 || slot >= RT_SAVE_SLOTS || !g_save_mount[slot][0]) return M_NONE;
        root = g_save_mount[slot]; kind = g_save_ro[slot] ? M_APP0 : M_USER;
        size_t len = (size_t)snprintf(host, cap, "%s", root);
        for (int i = 1; i < nc; ++i) len += (size_t)snprintf(host + len, cap - len < cap ? cap - len : 0, "/%s", comp[i]);
        if (len >= cap) { *err = K_ENAMETOOLONG; return M_NONE; }
        return kind;
    }
    else if (!strcmp(comp[0], "data") || !strcmp(comp[0], "temp0") || !strcmp(comp[0], "temp") || !strcmp(comp[0], "download0") || !strcmp(comp[0], "download1")) { root = g_user_root; kind = M_USER; }
    if (!root || !root[0]) return M_NONE;
    size_t len = (size_t)snprintf(host, cap, "%s", root);
    for (int i = (kind == M_APP0 ? 1 : 0); i < nc; ++i) len += (size_t)snprintf(host + len, cap - len < cap ? cap - len : 0, "/%s", comp[i]);
    if (len >= cap) { *err = K_ENAMETOOLONG; return M_NONE; }
    if (kind == M_USER) ensure_user_mount(comp[0]);
    return kind;
}

// ---- descriptor table -----------------------------------------------------------------------------------
typedef struct KFile {
    int kind;                // 0 file, 1 directory, 2 /dev/null
    void* h;
    char path[1536];
    uint64_t pos;
    int readable, writable, append;
    int refs;
    pthread_mutex_t lock;
    int dir_done;
    uint64_t reads, read_bytes, rep_reads, rep_bytes; // diagnostics: read counters (total and since the last report)
} KFile;
static KFile* g_files[4096];
static pthread_mutex_t g_tab = PTHREAD_MUTEX_INITIALIZER;

static KFile* get(int fd) {
    if (fd < 3 || fd >= 4096) return NULL;
    pthread_mutex_lock(&g_tab);
    KFile* f = g_files[fd];
    if (f) ++f->refs;
    pthread_mutex_unlock(&g_tab);
    return f;
}
static void release(KFile* f) {
    pthread_mutex_lock(&g_tab);
    int last = --f->refs == 0;
    pthread_mutex_unlock(&g_tab);
    if (last) {
        if (f->kind == 0 && f->h) os_file_close(f->h);
        if (f->kind == 1 && f->h) os_dir_close(f->h);
        pthread_mutex_destroy(&f->lock);
        free(f);
    }
}

// ---- open / close -----------------------------------------------------------------------------------------
enum { K_O_WRONLY = 1, K_O_RDWR = 2, K_O_APPEND = 8, K_O_CREAT = 0x200, K_O_TRUNC = 0x400, K_O_EXCL = 0x800, K_O_DIRECTORY = 0x20000 };

static int open_impl(const char* path, int flags) {
    char host[1536]; int err;
    host[0] = 0; // /dev/null has no host path: never format an uninitialised buffer
    MountKind m = resolve(path, host, sizeof host, &err);
    int want_write = (flags & (K_O_WRONLY | K_O_RDWR)) != 0 || (flags & (K_O_CREAT | K_O_TRUNC));
    ++g_opens;
    if (m == M_NONE) { ++g_open_fail; rt_logf("fs: open %s -> errno %d (no such mount or path)", path ? path : "(null)", err); return -err; }
    if (m == M_APP0 && want_write) { ++g_open_fail; rt_logf("fs: open %s for writing refused: /app0 is read-only", path); return -K_EACCES; }
    int fd = rt_fd_alloc();
    if (fd < 0) return -K_EMFILE;
    KFile* f = calloc(1, sizeof *f);
    f->refs = 1;
    pthread_mutex_init(&f->lock, NULL);
    snprintf(f->path, sizeof f->path, "%s", host);
    f->readable = !(flags & K_O_WRONLY);
    f->writable = (flags & (K_O_WRONLY | K_O_RDWR)) != 0;
    f->append = (flags & K_O_APPEND) != 0;
    if (m == M_NULL) { f->kind = 2; }
    else {
        int oserr = 0;
        OsStat st;
        int sr = os_stat(host, &st);
        if (sr == OS_OK && st.is_dir) {
            if (want_write) { oserr = OS_E_ISDIR; }
            else { f->kind = 1; f->h = os_dir_open(host, &oserr); if (!f->h && !oserr) oserr = OS_E_IO; }
        } else {
            if (flags & K_O_DIRECTORY) oserr = sr == OS_OK ? OS_E_NOTDIR : sr;
            else {
                int of = (f->readable ? OS_F_READ : 0) | (f->writable ? OS_F_WRITE : 0) | ((flags & K_O_CREAT) ? OS_F_CREATE : 0) |
                         ((flags & K_O_TRUNC) ? OS_F_TRUNC : 0) | ((flags & K_O_EXCL) ? OS_F_EXCL : 0);
                f->kind = 0; f->h = os_file_open(host, of, &oserr);
            }
        }
        if (oserr || !f->h) {
            ++g_open_fail;
            int e = kerrno(oserr ? oserr : OS_E_IO);
            rt_logf("fs: open %s (%s) -> errno %d", path, host, e);
            rt_fd_free(fd); pthread_mutex_destroy(&f->lock); free(f);
            return -e;
        }
    }
    if (g_opens <= 200 || g_open_fail <= 50) rt_logf("fs: open %s -> fd %d%s", path, fd, f->kind == 1 ? " (directory)" : "");
    pthread_mutex_lock(&g_tab); g_files[fd] = f; pthread_mutex_unlock(&g_tab);
    return fd;
}

static int close_impl(int fd) {
    pthread_mutex_lock(&g_tab);
    KFile* f = (fd >= 3 && fd < 4096) ? g_files[fd] : NULL;
    if (f) g_files[fd] = NULL;
    pthread_mutex_unlock(&g_tab);
    if (!f) return -K_EBADF;
    rt_fd_free(fd);
    release(f);
    return 0;
}

// Diagnostics (--sample-threads): which open files were read since the previous report.
void rt_fs_report(void) {
    char line[1024]; size_t l = 0; int shown = 0;
    pthread_mutex_lock(&g_tab);
    for (int fd = 3; fd < 4096; ++fd) {
        KFile* f = g_files[fd];
        if (!f || f->kind != 0) continue;
        uint64_t dr = f->reads - f->rep_reads, db = f->read_bytes - f->rep_bytes;
        f->rep_reads = f->reads; f->rep_bytes = f->read_bytes;
        if (!dr) continue;
        const char* base = strrchr(f->path, '/'); base = base ? base + 1 : f->path;
        if (l < sizeof line - 160) l += (size_t)snprintf(line + l, sizeof line - l, "%s%s fd %d: %llu reads, %llu KiB", shown ? "; " : "", base, fd, (unsigned long long)dr, (unsigned long long)(db >> 10));
        ++shown;
    }
    pthread_mutex_unlock(&g_tab);
    rt_logf("fs: reads since the last report: %s", shown ? line : "(none)");
}

// ---- I/O --------------------------------------------------------------------------------------------------
static int64_t read_at(KFile* f, void* buf, size_t n, uint64_t off, int* e) {
    if (f->kind == 2) return 0;
    if (f->kind == 1) { *e = K_EISDIR; return -1; }
    if (!f->readable) { *e = K_EBADF; return -1; }
    int oe = 0;
    int64_t r = os_file_pread(f->h, buf, n, off, &oe);
    if (r < 0) { *e = kerrno(oe); return -1; }
    g_bytes_read += (uint64_t)r;
    f->reads++; f->read_bytes += (uint64_t)r;
    return r;
}
static int64_t write_at(KFile* f, const void* buf, size_t n, uint64_t off, int* e) {
    if (f->kind == 2) return (int64_t)n;
    if (f->kind == 1) { *e = K_EISDIR; return -1; }
    if (!f->writable) { *e = K_EBADF; return -1; }
    int oe = 0;
    int64_t r = os_file_pwrite(f->h, buf, n, off, &oe);
    if (r < 0) { *e = kerrno(oe); return -1; }
    return r;
}
static int64_t do_read(int fd, void* buf, size_t n, int* e) {
    KFile* f = get(fd);
    if (!f) { *e = K_EBADF; return -1; }
    pthread_mutex_lock(&f->lock);
    int64_t r = read_at(f, buf, n, f->pos, e);
    if (r > 0) f->pos += (uint64_t)r;
    pthread_mutex_unlock(&f->lock);
    release(f);
    return r;
}
static int64_t do_write(int fd, const void* buf, size_t n, int* e) {
    KFile* f = get(fd);
    if (!f) { *e = K_EBADF; return -1; }
    pthread_mutex_lock(&f->lock);
    uint64_t off = f->pos;
    if (f->append && f->kind == 0) { uint64_t sz = 0; if (!os_file_size(f->h, &sz)) off = sz; }
    int64_t r = write_at(f, buf, n, off, e);
    if (r > 0) f->pos = off + (uint64_t)r;
    pthread_mutex_unlock(&f->lock);
    release(f);
    return r;
}

// guest stdout/stderr go to the log, line by line
static void guest_stream(int fd, const char* buf, size_t n) {
    static __thread char line[2][512]; static __thread size_t len[2];
    int s = fd == 2;
    for (size_t i = 0; i < n; ++i) {
        char c = buf[i];
        if (c == '\n' || len[s] == sizeof line[s] - 1) { line[s][len[s]] = 0; rt_logf("guest %s: %s", s ? "stderr" : "stdout", line[s]); len[s] = 0; }
        if (c != '\n') line[s][len[s]++] = c;
    }
}

// ---- stat / dirent layouts (FreeBSD 9 as used by the console) -----------------------------------------------
typedef struct { int64_t sec, nsec; } KTimespec;
typedef struct {
    uint32_t st_dev, st_ino; uint16_t st_mode, st_nlink; uint32_t st_uid, st_gid, st_rdev;
    KTimespec st_atim, st_mtim, st_ctim;
    int64_t st_size, st_blocks; uint32_t st_blksize, st_flags, st_gen; int32_t st_lspare;
    KTimespec st_birthtim;
} KStat;
_Static_assert(sizeof(KStat) == 0x78, "stat layout"); // the console's struct stat is 120 bytes
typedef struct { uint32_t d_fileno; uint16_t d_reclen; uint8_t d_type; uint8_t d_namlen; char d_name[256]; } KDirent;
_Static_assert(sizeof(KDirent) == 264, "dirent layout");

static uint32_t fnv(const char* s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void fill_stat(KStat* k, const OsStat* st, const char* host, int writable) {
    memset(k, 0, sizeof *k);
    k->st_dev = 1; k->st_ino = fnv(host); k->st_nlink = 1;
    k->st_mode = (uint16_t)((st->is_dir ? 040000 : 0100000) | (writable ? 0777 : 0555));
    k->st_atim = (KTimespec){st->atime_ns / 1000000000ll, st->atime_ns % 1000000000ll};
    k->st_mtim = (KTimespec){st->mtime_ns / 1000000000ll, st->mtime_ns % 1000000000ll};
    k->st_ctim = (KTimespec){st->ctime_ns / 1000000000ll, st->ctime_ns % 1000000000ll};
    k->st_birthtim = k->st_ctim;
    k->st_size = (int64_t)st->size; k->st_blksize = 0x10000; k->st_blocks = (int64_t)((st->size + 511) / 512);
}
static int stat_path(const char* path, KStat* out) {
    char host[1536]; int err;
    MountKind m = resolve(path, host, sizeof host, &err);
    if (m == M_NONE) return -err;
    if (m == M_NULL) { OsStat z = {0}; fill_stat(out, &z, host, 1); out->st_mode = 020666; return 0; }
    OsStat st; int r = os_stat(host, &st);
    if (r) return -kerrno(r);
    fill_stat(out, &st, host, m == M_USER);
    return 0;
}
static int stat_fd(int fd, KStat* out) {
    KFile* f = get(fd);
    if (!f) return -K_EBADF;
    int r = 0;
    if (f->kind == 2) { OsStat z = {0}; fill_stat(out, &z, "/dev/null", 1); out->st_mode = 020666; }
    else {
        OsStat st; int e = os_stat(f->path, &st);
        if (e) r = -kerrno(e); else fill_stat(out, &st, f->path, 0);
        if (!e) { MountKind mk = (strncmp(f->path, g_game_root, strlen(g_game_root)) == 0 && g_game_root[0]) ? M_APP0 : M_USER; out->st_mode = (uint16_t)((st.is_dir ? 040000 : 0100000) | (mk == M_USER ? 0777 : 0555)); }
    }
    release(f);
    return r;
}

// ---- SCE-style entry points (return a negative SCE error code on failure) -------------------------------------
extern int* rt_errno_cell(void);
static int GUEST_ABI k_open(const char* path, int flags, int mode) { (void)mode; int r = open_impl(path, flags); return r < 0 ? SCE_ERR(-r) : r; }
static int GUEST_ABI k_close(int fd) { int r = close_impl(fd); return r < 0 ? SCE_ERR(-r) : 0; }
static int64_t GUEST_ABI k_read(int fd, void* b, size_t n) { int e = 0; int64_t r = do_read(fd, b, n, &e); return r < 0 ? SCE_ERR64(e) : r; }
static int64_t GUEST_ABI k_write(int fd, const void* b, size_t n) {
    if (fd == 1 || fd == 2) { guest_stream(fd, b, n); return (int64_t)n; }
    int e = 0; int64_t r = do_write(fd, b, n, &e); return r < 0 ? SCE_ERR64(e) : r;
}
static int64_t GUEST_ABI k_pread(int fd, void* b, size_t n, int64_t off) {
    KFile* f = get(fd); int e = K_EBADF; int64_t r = -1;
    if (f && off >= 0) { r = read_at(f, b, n, (uint64_t)off, &e); release(f); } else if (f) { release(f); e = K_EINVAL; }
    return r < 0 ? SCE_ERR64(e) : r;
}
static int64_t GUEST_ABI k_pwrite(int fd, const void* b, size_t n, int64_t off) {
    KFile* f = get(fd); int e = K_EBADF; int64_t r = -1;
    if (f && off >= 0) { r = write_at(f, b, n, (uint64_t)off, &e); release(f); } else if (f) { release(f); e = K_EINVAL; }
    return r < 0 ? SCE_ERR64(e) : r;
}
static int64_t GUEST_ABI k_lseek(int fd, int64_t off, int whence) {
    KFile* f = get(fd);
    if (!f) return SCE_ERR64(K_EBADF);
    int64_t r;
    pthread_mutex_lock(&f->lock);
    if (f->kind == 1) { r = 0; f->pos = 0; f->dir_done = 0; }
    else {
        int64_t base = 0;
        if (whence == 1) base = (int64_t)f->pos;
        else if (whence == 2) { uint64_t sz = 0; if (f->kind == 0 && os_file_size(f->h, &sz) == 0) base = (int64_t)sz; }
        else if (whence != 0) base = INT64_MIN;
        r = base == INT64_MIN || base + off < 0 ? SCE_ERR64(K_EINVAL) : (f->pos = (uint64_t)(base + off), (int64_t)f->pos);
    }
    pthread_mutex_unlock(&f->lock);
    release(f);
    return r;
}
static int GUEST_ABI k_fsync(int fd) { KFile* f = get(fd); if (!f) return SCE_ERR(K_EBADF); if (f->kind == 0 && f->writable) os_file_sync(f->h); release(f); return 0; }
static int GUEST_ABI k_ftruncate(int fd, int64_t len) {
    KFile* f = get(fd);
    if (!f) return SCE_ERR(K_EBADF);
    int r = (f->kind == 0 && f->writable && len >= 0 && os_file_truncate(f->h, (uint64_t)len) == 0) ? 0 : SCE_ERR(f->writable ? K_EINVAL : K_EBADF);
    release(f);
    return r;
}
static int GUEST_ABI k_stat(const char* path, KStat* st) { int r = stat_path(path, st); if (r && path) rt_logf("fs: stat %s -> errno %d", path, -r); return r ? SCE_ERR(-r) : 0; }
static int GUEST_ABI k_fstat(int fd, KStat* st) { int r = stat_fd(fd, st); return r ? SCE_ERR(-r) : 0; }
static int GUEST_ABI k_mkdir(const char* path, int mode) {
    (void)mode;
    char host[1536]; int err; MountKind m = resolve(path, host, sizeof host, &err);
    if (m == M_NONE) return SCE_ERR(err);
    if (m != M_USER) return SCE_ERR(K_EACCES);
    int r = os_mkdir(host);
    return r ? SCE_ERR(kerrno(r)) : 0;
}
static int GUEST_ABI k_rmdir(const char* path) {
    char host[1536]; int err; MountKind m = resolve(path, host, sizeof host, &err);
    if (m == M_NONE) return SCE_ERR(err);
    if (m != M_USER) return SCE_ERR(K_EACCES);
    int r = os_rmdir(host);
    return r ? SCE_ERR(kerrno(r)) : 0;
}
static int GUEST_ABI k_unlink(const char* path) {
    char host[1536]; int err; MountKind m = resolve(path, host, sizeof host, &err);
    if (m == M_NONE) return SCE_ERR(err);
    if (m != M_USER) return SCE_ERR(K_EACCES);
    int r = os_unlink(host);
    return r ? SCE_ERR(kerrno(r)) : 0;
}
static int GUEST_ABI k_rename(const char* a, const char* b) {
    char ha[1536], hb[1536]; int e1, e2;
    MountKind ma = resolve(a, ha, sizeof ha, &e1), mb = resolve(b, hb, sizeof hb, &e2);
    if (ma == M_NONE) return SCE_ERR(e1);
    if (mb == M_NONE) return SCE_ERR(e2);
    if (ma != M_USER || mb != M_USER) return SCE_ERR(K_EACCES);
    int r = os_rename(ha, hb);
    return r ? SCE_ERR(kerrno(r)) : 0;
}
static int GUEST_ABI k_getdirentries(int fd, char* buf, uint32_t nbytes, int64_t* basep) {
    KFile* f = get(fd);
    if (!f) return SCE_ERR(K_EBADF);
    if (f->kind != 1) { release(f); return SCE_ERR(K_ENOTDIR); }
    pthread_mutex_lock(&f->lock);
    uint32_t used = 0;
    while (!f->dir_done && used + sizeof(KDirent) <= nbytes) {
        char name[256]; int is_dir = 0;
        if (!os_dir_next(f->h, name, sizeof name, &is_dir)) { f->dir_done = 1; break; }
        KDirent* d = (KDirent*)(buf + used);
        memset(d, 0, sizeof *d);
        d->d_fileno = fnv(name); d->d_reclen = sizeof *d; d->d_type = is_dir ? 4 : 8; d->d_namlen = (uint8_t)strlen(name);
        memcpy(d->d_name, name, d->d_namlen + 1u);
        used += sizeof *d;
    }
    if (basep) *basep = (int64_t)f->pos;
    f->pos += used;
    pthread_mutex_unlock(&f->lock);
    release(f);
    return (int)used;
}

// ---- POSIX-style entry points used by the console libc (return -1 and set errno) -------------------------------
static int64_t GUEST_ABI p_read(int fd, void* b, size_t n) {
    if (fd == 0) return 0; // no standard input
    int e = 0; int64_t r = do_read(fd, b, n, &e);
    if (r < 0) { *rt_errno_cell() = e; return -1; }
    return r;
}
static int64_t GUEST_ABI p_write(int fd, const void* b, size_t n) {
    if (fd == 1 || fd == 2) { guest_stream(fd, b, n); return (int64_t)n; }
    int e = 0; int64_t r = do_write(fd, b, n, &e);
    if (r < 0) { *rt_errno_cell() = e; return -1; }
    return r;
}
static int GUEST_ABI p_fstat(int fd, KStat* st) {
    if (fd >= 0 && fd <= 2) { OsStat z = {0}; fill_stat(st, &z, "/dev/tty", 1); st->st_mode = 020620; return 0; }
    int r = stat_fd(fd, st);
    if (r) { *rt_errno_cell() = -r; return -1; }
    return 0;
}

const HostSymbol fs_symbols[] = {
    {"sceKernelOpen", "libkernel", (void*)k_open, 0},
    {"sceKernelClose", "libkernel", (void*)k_close, 0},
    {"sceKernelRead", "libkernel", (void*)k_read, 0},
    {"sceKernelWrite", "libkernel", (void*)k_write, 0},
    {"sceKernelPread", "libkernel", (void*)k_pread, 0},
    {"sceKernelPwrite", "libkernel", (void*)k_pwrite, 0},
    {"sceKernelLseek", "libkernel", (void*)k_lseek, 0},
    {"sceKernelFsync", "libkernel", (void*)k_fsync, 0},
    {"sceKernelFtruncate", "libkernel", (void*)k_ftruncate, 0},
    {"sceKernelStat", "libkernel", (void*)k_stat, 0},
    {"sceKernelFstat", "libkernel", (void*)k_fstat, 0},
    {"sceKernelMkdir", "libkernel", (void*)k_mkdir, 0},
    {"sceKernelRmdir", "libkernel", (void*)k_rmdir, 0},
    {"sceKernelUnlink", "libkernel", (void*)k_unlink, 0},
    {"sceKernelRename", "libkernel", (void*)k_rename, 0},
    {"sceKernelGetdirentries", "libkernel", (void*)k_getdirentries, 0},
    {"_read", "libkernel", (void*)p_read, 0},
    {"_write", "libkernel", (void*)p_write, 0},
    {"fstat", "libkernel", (void*)p_fstat, 0},
};
const size_t fs_symbol_count = sizeof fs_symbols / sizeof *fs_symbols;

// ---- self-test (jstars_selftest.c) ---------------------------------------------------------------------------
int GUEST_ABI rt_fs_selftest(const char* scratch_dir, int (GUEST_ABI *check)(int ok, const char* what)) {
    char root[1100]; snprintf(root, sizeof root, "%s/fs-selftest", scratch_dir);
    os_mkdir(scratch_dir); os_mkdir(root);
    char game[1200], user[1200];
    snprintf(game, sizeof game, "%s/game", root); snprintf(user, sizeof user, "%s/user", root);
    os_mkdir(game); os_mkdir(user);
    char p[1300]; int e = 0;
    snprintf(p, sizeof p, "%s/a.bin", game);
    void* f = os_file_open(p, OS_F_WRITE | OS_F_CREATE | OS_F_TRUNC, &e);
    static uint8_t pat[100000]; for (size_t i = 0; i < sizeof pat; ++i) pat[i] = (uint8_t)(i * 7 + 3);
    if (f) { os_file_pwrite(f, pat, sizeof pat, 0, &e); os_file_close(f); }
    snprintf(p, sizeof p, "%s/sub", game); os_mkdir(p);
    snprintf(p, sizeof p, "%s/sub/b.txt", game);
    f = os_file_open(p, OS_F_WRITE | OS_F_CREATE | OS_F_TRUNC, &e);
    if (f) { os_file_pwrite(f, "hello", 5, 0, &e); os_file_close(f); }
    rt_fs_configure(game, user);
    char sd[1300]; snprintf(sd, sizeof sd, "%s/slot0", user); os_mkdir(user); os_mkdir(sd);
    rt_fs_save_mount(0, sd, 0);
    int bad = 0;
#define C(ok, what) do { if (!check((ok), (what))) ++bad; } while (0)
    int fd = k_open("/app0/a.bin", 0, 0);
    C(fd >= 3, "fs: open an existing /app0 file");
    static uint8_t buf[100000];
    C(k_read(fd, buf, 4096) == 4096 && !memcmp(buf, pat, 4096), "fs: read returns the file's bytes");
    C(k_lseek(fd, 0, 1) == 4096, "fs: lseek(cur) reports the position after a read");
    C(k_pread(fd, buf, 100, 50000) == 100 && !memcmp(buf, pat + 50000, 100), "fs: pread at an offset leaves the position alone and returns the bytes");
    C(k_lseek(fd, 0, 1) == 4096, "fs: pread did not move the position");
    C(k_lseek(fd, -10, 2) == 99990 && k_read(fd, buf, 100) == 10, "fs: lseek(end) then a short read at EOF");
    C(k_read(fd, buf, 100) == 0, "fs: read at EOF returns 0");
    KStat st; C(k_fstat(fd, &st) == 0 && st.st_size == 100000 && (st.st_mode & 0170000) == 0100000 && (st.st_mode & 0222) == 0, "fs: fstat reports size, regular file and read-only mode");
    C(k_close(fd) == 0 && k_close(fd) == SCE_ERR(K_EBADF), "fs: close succeeds once, then EBADF");
    C(k_open("/app0/missing", 0, 0) == SCE_ERR(K_ENOENT), "fs: open of a missing file is ENOENT");
    C(k_open("/app0/a.bin", 1, 0) == SCE_ERR(K_EACCES), "fs: opening /app0 for writing is refused");
    C(k_open("/app0/../etc/passwd", 0, 0) == SCE_ERR(K_EACCES), "fs: .. cannot leave the mount");
    C(k_open("/nowhere/x", 0, 0) == SCE_ERR(K_ENOENT), "fs: unknown mount is ENOENT");
    C(k_stat("/app0/sub", &st) == 0 && (st.st_mode & 0170000) == 040000, "fs: stat of a directory");
    fd = k_open("/app0/sub", 0, 0x20000);
    char dbuf[1024];
    int n = fd >= 3 ? k_getdirentries(fd, dbuf, sizeof dbuf, NULL) : -1;
    C(n == (int)sizeof(KDirent) && !strcmp(((KDirent*)dbuf)->d_name, "b.txt") && ((KDirent*)dbuf)->d_type == 8, "fs: getdirentries lists b.txt as a regular file");
    C(k_getdirentries(fd, dbuf, sizeof dbuf, NULL) == 0, "fs: getdirentries returns 0 at the end");
    k_close(fd);
    int w = k_open("/savedata0/slot.dat", K_O_RDWR | K_O_CREAT | K_O_TRUNC, 0);
    C(w >= 3 && k_write(w, "SAVE", 4) == 4 && k_lseek(w, 0, 0) == 0 && k_read(w, buf, 4) == 4 && !memcmp(buf, "SAVE", 4), "fs: write then read back in /savedata0");
    C(k_ftruncate(w, 2) == 0 && k_fstat(w, &st) == 0 && st.st_size == 2, "fs: ftruncate shrinks the file");
    k_close(w);
    C(k_mkdir("/data/dir", 0777) == 0 && k_mkdir("/data/dir", 0777) == SCE_ERR(K_EEXIST), "fs: mkdir then EEXIST");
    C(k_rename("/savedata0/slot.dat", "/data/dir/slot2.dat") == 0 && k_stat("/data/dir/slot2.dat", &st) == 0, "fs: rename between writable mounts");
    C(k_unlink("/data/dir/slot2.dat") == 0 && k_rmdir("/data/dir") == 0, "fs: unlink and rmdir");
    C(k_unlink("/app0/a.bin") == SCE_ERR(K_EACCES), "fs: unlink under /app0 is refused");
    int nul = k_open("/dev/null", K_O_WRONLY, 0);
    C(nul >= 3 && k_write(nul, "xx", 2) == 2, "fs: /dev/null accepts writes");
    k_close(nul);
#undef C
    return bad;
}

// Resolves a console path to the host path of an existing /app0 file for diagnostics and media decoding (read-only use).
// Returns 0 and fills `host` on success, or a console errno.
int rt_fs_host_path(const char* path, char* host, size_t cap) {
    int err = 0;
    MountKind m = resolve(path, host, cap, &err);
    return m == M_NONE ? (err ? err : K_ENOENT) : 0;
}

// helper for the save data self-test: writes `text` through the console file layer; 0 on success
int GUEST_ABI rt_fs_test_write(const char* path, const char* text) {
    int fd = open_impl(path, K_O_WRONLY | K_O_CREAT | K_O_TRUNC);
    if (fd < 0) return -fd;
    int e = 0; int64_t n = do_write(fd, text, strlen(text), &e);
    close_impl(fd);
    return n == (int64_t)strlen(text) ? 0 : e ? e : 5;
}
