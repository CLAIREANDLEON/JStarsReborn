// libSceSaveData: save folders on the host. Layout:  <user-dir>/savedata/<userId>/<titleId>/<dirName>/
// with the game's files at the top level and this port's metadata in sce_sys/param.bin (title, sub-title,
// detail, user param, mtime, capacity in blocks) and sce_sys/icon0.png. Mounting binds a /savedataN
// mount point to that folder in the console file system (jstars_fs.c). The folder format is this port's
// own; it is not a console save export.
// Not provided (the run stops with the exact call site if the game uses them): backup, memory-save
// (SaveDataMemory), transferring mount, cloud/upload, events, progress.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

enum {
    SD_OK = 0, SD_USER_SERVICE_NOT_INITIALIZED = (int)0x80960002, SD_PARAMETER = (int)0x809F0000, SD_NOT_INITIALIZED = (int)0x809F0001,
    SD_BUSY = (int)0x809F0003, SD_NOT_MOUNTED = (int)0x809F0004, SD_EXISTS = (int)0x809F0007, SD_NOT_FOUND = (int)0x809F0008,
    SD_NO_SPACE_FS = (int)0x809F000A, SD_INTERNAL = (int)0x809F000B, SD_MOUNT_FULL = (int)0x809F000C, SD_BAD_MOUNTED = (int)0x809F000D,
    SD_INVALID_LOGIN_USER = (int)0x809F0011
};
#define SD_BLOCK 32768ull
#define SD_SLOTS 15
enum { MODE_RDONLY = 1, MODE_RDWR = 2, MODE_CREATE = 4, MODE_DESTRUCT_OFF = 8, MODE_COPY_ICON = 16, MODE_CREATE2 = 32 };

// ---- console structures (sizes and offsets from the SDK headers as used by the reference emulator) ----------------------
typedef struct { char data[32]; } DirName;
typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], sub_title[128], detail[1024];
    uint32_t user_param; int32_t pad0;
    int64_t mtime;
    uint8_t reserved[32];
} SdParam;
_Static_assert(sizeof(SdParam) == 1328, "SaveDataParam layout");
typedef struct { int32_t user; int32_t pad; const TitleId* title_id; const DirName* dir; const void* fingerprint; uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } SdMount;
typedef struct { int32_t user; int32_t pad; const DirName* dir; uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } SdMount2;
typedef struct { MountPoint mount_point; uint64_t required_blocks; uint32_t unused; uint32_t mount_status; uint8_t reserved[28]; int32_t pad; } SdMountResult;
_Static_assert(sizeof(SdMountResult) == 64, "MountResult layout");
typedef struct { int32_t user; int32_t pad; const TitleId* title_id; const DirName* dir; uint32_t unused; uint8_t reserved[32]; int32_t pad2; } SdDelete;
typedef struct { int32_t user; int32_t pad; const TitleId* title_id; const DirName* dir; uint32_t key, order; uint8_t reserved[32]; } SdSearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SdSearchInfo;
typedef struct { uint32_t hit_num; int32_t pad; DirName* dir_names; uint32_t dir_names_num; uint32_t set_num; SdParam* params; SdSearchInfo* infos; uint8_t reserved[12]; int32_t pad2; } SdSearchResult;
typedef struct { void* buf; size_t buf_size; size_t data_size; uint8_t reserved[32]; } SdIcon;

static int g_init;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int used, ro; char dir_name[33]; char host[1536]; int32_t user; uint64_t blocks; } g_slot[SD_SLOTS];
static char g_title_id[11] = "CUSA01593";
void rt_set_title_id(const char* id) { snprintf(g_title_id, sizeof g_title_id, "%s", id); }

// ---- host paths --------------------------------------------------------------------------------------------------------
static int valid_name(const char* n) {
    if (!n || !n[0]) return 0;
    for (const char* c = n; *c; ++c) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.')) return 0;
    return strcmp(n, ".") && strcmp(n, "..");
}
static void title_dir(char* out, size_t n, int32_t user, const char* title) { snprintf(out, n, "%s/savedata/%d/%s", rt_fs_user_root(), user, title ? title : g_title_id); }
static void mkdirs(const char* path) {
    char tmp[1536]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; ++p) if (*p == '/') { *p = 0; os_mkdir(tmp); *p = '/'; }
    os_mkdir(tmp);
}
static int rm_rf(const char* path) {
    int err = 0; void* d = os_dir_open(path, &err);
    if (d) {
        char name[256]; int is_dir;
        while (os_dir_next(d, name, sizeof name, &is_dir)) {
            char child[1792]; snprintf(child, sizeof child, "%s/%s", path, name);
            if (is_dir) rm_rf(child); else os_unlink(child);
        }
        os_dir_close(d);
    }
    return os_rmdir(path);
}
static uint64_t dir_bytes(const char* path) {
    uint64_t total = 0; int err = 0; void* d = os_dir_open(path, &err);
    if (!d) return 0;
    char name[256]; int is_dir;
    while (os_dir_next(d, name, sizeof name, &is_dir)) {
        char child[1792]; snprintf(child, sizeof child, "%s/%s", path, name);
        if (is_dir) total += dir_bytes(child); else { OsStat st; if (!os_stat(child, &st)) total += st.size; }
    }
    os_dir_close(d);
    return total;
}

// ---- metadata -----------------------------------------------------------------------------------------------------------
typedef struct { uint32_t magic; uint32_t version; SdParam param; uint64_t blocks; } ParamFile;
static int read_params(const char* save_dir, ParamFile* pf) {
    char p[1792]; snprintf(p, sizeof p, "%s/sce_sys/param.bin", save_dir);
    int err = 0; void* f = os_file_open(p, OS_F_READ, &err);
    if (!f) return -1;
    int64_t n = os_file_pread(f, pf, sizeof *pf, 0, &err);
    os_file_close(f);
    return n == (int64_t)sizeof *pf && pf->magic == 0x4A534450u ? 0 : -1;
}
static int write_params(const char* save_dir, const ParamFile* pf) {
    char d[1792], p[1792]; snprintf(d, sizeof d, "%s/sce_sys", save_dir); os_mkdir(d);
    snprintf(p, sizeof p, "%s/sce_sys/param.bin", save_dir);
    int err = 0; void* f = os_file_open(p, OS_F_WRITE | OS_F_CREATE | OS_F_TRUNC, &err);
    if (!f) return -1;
    int64_t n = os_file_pwrite(f, pf, sizeof *pf, 0, &err);
    os_file_close(f);
    return n == (int64_t)sizeof *pf ? 0 : -1;
}

// ---- entry points ---------------------------------------------------------------------------------------------------------
static int GUEST_ABI sd_initialize(void* params) { (void)params; g_init = 1; rt_logf("sceSaveDataInitialize: save folders under %s/savedata", rt_fs_user_root()); return SD_OK; }
static int GUEST_ABI sd_terminate(void) { if (!g_init) return SD_NOT_INITIALIZED; g_init = 0; return SD_OK; }

static int do_mount(int32_t user, const char* title, const DirName* dir, uint64_t blocks, uint32_t mode, SdMountResult* res) {
    if (!g_init) return SD_NOT_INITIALIZED;
    if (!res) return SD_PARAMETER;
    if (user < 0) return SD_INVALID_LOGIN_USER;
    if (!dir || !valid_name(dir->data)) return SD_PARAMETER;
    int ro = (mode & MODE_RDONLY) != 0;
    int create = (mode & MODE_CREATE) != 0, create2 = (mode & MODE_CREATE2) != 0;
    if (create && create2) return SD_PARAMETER;
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < SD_SLOTS; ++i) {
        if (g_slot[i].used) { if (!strcmp(g_slot[i].dir_name, dir->data) && g_slot[i].user == user) { pthread_mutex_unlock(&g_lock); return SD_BUSY; } }
        else if (slot < 0) slot = i;
    }
    if (slot < 0) { pthread_mutex_unlock(&g_lock); return SD_MOUNT_FULL; }
    char tdir[1024], sdir[1536];
    title_dir(tdir, sizeof tdir, user, title);
    snprintf(sdir, sizeof sdir, "%s/%s", tdir, dir->data);
    OsStat st; int exists = os_stat(sdir, &st) == OS_OK && st.is_dir;
    if (!create && !create2 && !exists) { pthread_mutex_unlock(&g_lock); return SD_NOT_FOUND; }
    if (create && exists) { pthread_mutex_unlock(&g_lock); return SD_EXISTS; }
    int created = 0;
    if (!exists) {
        if (ro) { pthread_mutex_unlock(&g_lock); return SD_PARAMETER; }
        mkdirs(sdir);
        if (os_stat(sdir, &st) != OS_OK) { pthread_mutex_unlock(&g_lock); return SD_INTERNAL; }
        ParamFile pf; memset(&pf, 0, sizeof pf); pf.magic = 0x4A534450u; pf.version = 1; pf.blocks = blocks; pf.param.mtime = (int64_t)time(NULL);
        if (write_params(sdir, &pf)) { pthread_mutex_unlock(&g_lock); return SD_NO_SPACE_FS; }
        created = 1;
    }
    g_slot[slot].used = 1; g_slot[slot].ro = ro; g_slot[slot].user = user;
    snprintf(g_slot[slot].dir_name, sizeof g_slot[slot].dir_name, "%s", dir->data);
    snprintf(g_slot[slot].host, sizeof g_slot[slot].host, "%s", sdir);
    g_slot[slot].blocks = blocks;
    pthread_mutex_unlock(&g_lock);
    rt_fs_save_mount(slot, sdir, ro);
    memset(res, 0, sizeof *res);
    snprintf(res->mount_point.data, sizeof res->mount_point.data, "/savedata%d", slot & 15);
    res->mount_status = created;   // 1 = CREATED
    rt_logf("sceSaveDataMount: %s -> /savedata%d (%s%s) host %s", dir->data, slot, ro ? "read-only" : "read-write", created ? ", created" : "", sdir);
    return SD_OK;
}
static int GUEST_ABI sd_mount(const SdMount* m, SdMountResult* r) { if (!m) return SD_PARAMETER; return do_mount(m->user, m->title_id ? m->title_id->data : NULL, m->dir, m->blocks, m->mode, r); }
static int GUEST_ABI sd_mount2(const SdMount2* m, SdMountResult* r) { if (!m) return SD_PARAMETER; return do_mount(m->user, NULL, m->dir, m->blocks, m->mode, r); }

static int find_slot(const MountPoint* mp) {
    if (!mp) return -1;
    for (int i = 0; i < SD_SLOTS; ++i) {
        char name[16]; snprintf(name, sizeof name, "/savedata%d", i);
        if (g_slot[i].used && !strncmp(mp->data, name, sizeof mp->data)) return i;
    }
    return -1;
}
static int GUEST_ABI sd_umount(const MountPoint* mp) {
    pthread_mutex_lock(&g_lock);
    int i = find_slot(mp);
    if (i < 0) { pthread_mutex_unlock(&g_lock); return SD_NOT_MOUNTED; }
    if (!g_slot[i].ro) { ParamFile pf; if (!read_params(g_slot[i].host, &pf)) { pf.param.mtime = (int64_t)time(NULL); write_params(g_slot[i].host, &pf); } }
    g_slot[i].used = 0;
    pthread_mutex_unlock(&g_lock);
    rt_fs_save_mount(i, NULL, 0);
    return SD_OK;
}
static int GUEST_ABI sd_set_param(const MountPoint* mp, uint32_t type, const void* buf, size_t size) {
    if (!g_init) return SD_NOT_INITIALIZED;
    if (type > 4 || !mp || !buf) return SD_PARAMETER;
    pthread_mutex_lock(&g_lock);
    int i = find_slot(mp);
    if (i < 0) { pthread_mutex_unlock(&g_lock); return SD_NOT_MOUNTED; }
    if (g_slot[i].ro) { pthread_mutex_unlock(&g_lock); return SD_BAD_MOUNTED; }
    ParamFile pf; int rc = SD_OK;
    if (read_params(g_slot[i].host, &pf)) { memset(&pf, 0, sizeof pf); pf.magic = 0x4A534450u; pf.version = 1; pf.blocks = g_slot[i].blocks; }
    switch (type) {
    case 0: if (size != sizeof(SdParam)) { rc = SD_PARAMETER; break; } memcpy(&pf.param, buf, sizeof(SdParam)); break;
    case 1: snprintf(pf.param.title, sizeof pf.param.title, "%s", (const char*)buf); break;
    case 2: snprintf(pf.param.sub_title, sizeof pf.param.sub_title, "%s", (const char*)buf); break;
    case 3: snprintf(pf.param.detail, sizeof pf.param.detail, "%s", (const char*)buf); break;
    case 4: memcpy(&pf.param.user_param, buf, 4); break;
    }
    if (rc == SD_OK) { pf.param.mtime = (int64_t)time(NULL); if (write_params(g_slot[i].host, &pf)) rc = SD_INTERNAL; }
    pthread_mutex_unlock(&g_lock);
    return rc;
}
static int GUEST_ABI sd_save_icon(const MountPoint* mp, const SdIcon* icon) {
    if (!g_init) return SD_NOT_INITIALIZED;
    if (!mp || !icon || !icon->buf) return SD_PARAMETER;
    pthread_mutex_lock(&g_lock);
    int i = find_slot(mp);
    if (i < 0) { pthread_mutex_unlock(&g_lock); return SD_NOT_MOUNTED; }
    if (g_slot[i].ro) { pthread_mutex_unlock(&g_lock); return SD_BAD_MOUNTED; }
    char d[1792], p[1792]; snprintf(d, sizeof d, "%s/sce_sys", g_slot[i].host); os_mkdir(d);
    snprintf(p, sizeof p, "%s/sce_sys/icon0.png", g_slot[i].host);
    size_t n = icon->buf_size < icon->data_size ? icon->buf_size : icon->data_size;
    int err = 0; void* f = os_file_open(p, OS_F_WRITE | OS_F_CREATE | OS_F_TRUNC, &err);
    int rc = SD_OK;
    if (!f || os_file_pwrite(f, icon->buf, n, 0, &err) != (int64_t)n) rc = SD_INTERNAL;
    if (f) os_file_close(f);
    pthread_mutex_unlock(&g_lock);
    return rc;
}
static int GUEST_ABI sd_delete(const SdDelete* d) {
    if (!g_init) return SD_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data)) return SD_PARAMETER;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SD_SLOTS; ++i) if (g_slot[i].used && !strcmp(g_slot[i].dir_name, d->dir->data) && g_slot[i].user == d->user) { pthread_mutex_unlock(&g_lock); return SD_BUSY; }
    pthread_mutex_unlock(&g_lock);
    char tdir[1024], sdir[1536]; title_dir(tdir, sizeof tdir, d->user, d->title_id ? d->title_id->data : NULL);
    snprintf(sdir, sizeof sdir, "%s/%s", tdir, d->dir->data);
    OsStat st;
    if (os_stat(sdir, &st) == OS_OK && rm_rf(sdir)) return SD_INTERNAL;
    return SD_OK;
}

// SDK pattern: '%' matches any run of characters, '_' any single character (case-insensitive, as in the reference)
static int wild(const char* s, const char* p) {
    while (*p) {
        if (*p == '%') { ++p; for (const char* t = s;; ++t) { if (wild(t, p)) return 1; if (!*t) return 0; } }
        if (!*s) return 0;
        if (*p != '_' && (*p | 32) != (*s | 32)) return 0;
        ++s; ++p;
    }
    return !*s;
}
typedef struct { char name[33]; ParamFile pf; uint64_t used_blocks; } Hit;
static int g_key;
static int cmp_hits(const void* a, const void* b) {
    const Hit* x = a; const Hit* y = b; int64_t d;
    switch (g_key) {
    case 1: d = (int64_t)x->pf.param.user_param - (int64_t)y->pf.param.user_param; break;
    case 2: d = (int64_t)x->pf.blocks - (int64_t)y->pf.blocks; break;
    case 3: d = x->pf.param.mtime - y->pf.param.mtime; break;
    case 5: d = (int64_t)(x->pf.blocks - x->used_blocks) - (int64_t)(y->pf.blocks - y->used_blocks); break;
    default: return strcmp(x->name, y->name);
    }
    return d < 0 ? -1 : d > 0 ? 1 : strcmp(x->name, y->name);
}
static int GUEST_ABI sd_dir_name_search(const SdSearchCond* c, SdSearchResult* r) {
    if (!g_init) return SD_NOT_INITIALIZED;
    if (!c || !r || c->key > 5 || c->key == 4 || c->order > 1) return SD_PARAMETER;
    char tdir[1024]; title_dir(tdir, sizeof tdir, c->user, c->title_id ? c->title_id->data : NULL);
    r->hit_num = 0; r->set_num = 0;
    int err = 0; void* d = os_dir_open(tdir, &err);
    if (!d) return SD_OK;     // no save folder yet: zero hits
    Hit* hits = NULL; size_t n = 0, cap = 0;
    char name[256]; int is_dir;
    while (os_dir_next(d, name, sizeof name, &is_dir)) {
        if (!is_dir || strlen(name) > 31 || !strncmp(name, "sce_", 4)) continue;
        if (c->dir && c->dir->data[0] && !wild(name, c->dir->data)) continue;
        char sdir[1536]; snprintf(sdir, sizeof sdir, "%s/%s", tdir, name);
        ParamFile pf; if (read_params(sdir, &pf)) continue;     // folders without metadata are not save folders
        if (n == cap) { cap = cap ? cap * 2 : 16; hits = realloc(hits, cap * sizeof *hits); }
        snprintf(hits[n].name, sizeof hits[n].name, "%.32s", name); hits[n].pf = pf;
        hits[n].used_blocks = (dir_bytes(sdir) + SD_BLOCK - 1) / SD_BLOCK; ++n;
    }
    os_dir_close(d);
    g_key = (int)c->key;
    if (n) qsort(hits, n, sizeof *hits, cmp_hits);
    if (c->order == 1) for (size_t i = 0; i < n / 2; ++i) { Hit t = hits[i]; hits[i] = hits[n - 1 - i]; hits[n - 1 - i] = t; }
    size_t out = n < r->dir_names_num ? n : r->dir_names_num;
    r->hit_num = (uint32_t)n; r->set_num = (uint32_t)out;
    for (size_t i = 0; i < out; ++i) {
        memset(&r->dir_names[i], 0, sizeof(DirName)); snprintf(r->dir_names[i].data, sizeof r->dir_names[i].data, "%.31s", hits[i].name);
        if (r->params) r->params[i] = hits[i].pf.param;
        if (r->infos) { memset(&r->infos[i], 0, sizeof r->infos[i]); r->infos[i].blocks = hits[i].pf.blocks; r->infos[i].free_blocks = hits[i].pf.blocks > hits[i].used_blocks ? hits[i].pf.blocks - hits[i].used_blocks : 0; }
    }
    free(hits);
    return SD_OK;
}

const HostSymbol savedata_symbols[] = {
    {"sceSaveDataInitialize", "libSceSaveData", (void*)sd_initialize, 0},
    {"sceSaveDataTerminate", "libSceSaveData", (void*)sd_terminate, 0},
    {"sceSaveDataMount", "libSceSaveData", (void*)sd_mount, 0},
    {"sceSaveDataMount2", "libSceSaveData", (void*)sd_mount2, 0},
    {"sceSaveDataUmount", "libSceSaveData", (void*)sd_umount, 0},
    {"sceSaveDataSetParam", "libSceSaveData", (void*)sd_set_param, 0},
    {"sceSaveDataSaveIcon", "libSceSaveData", (void*)sd_save_icon, 0},
    {"sceSaveDataDelete", "libSceSaveData", (void*)sd_delete, 0},
    {"sceSaveDataDirNameSearch", "libSceSaveData", (void*)sd_dir_name_search, 0},
};
const size_t savedata_symbol_count = sizeof savedata_symbols / sizeof *savedata_symbols;

// ---- self-test -------------------------------------------------------------------------------------------------------------
int GUEST_ABI rt_savedata_selftest(int (GUEST_ABI* check)(int ok, const char* what)) {
    int bad = 0;
#define C(ok, what) do { if (!check((ok), (what))) ++bad; } while (0)
    DirName dn = {"SLOT01"}; SdMountResult res; MountPoint mp;
    C(sd_mount2(&(SdMount2){.user = 1, .dir = &dn, .blocks = 96, .mode = MODE_RDWR | MODE_CREATE2}, &res) == (int)0x809F0001, "savedata: mount before initialize is NOT_INITIALIZED");
    sd_initialize(NULL);
    C(sd_mount2(&(SdMount2){.user = 1, .dir = &dn, .blocks = 96, .mode = MODE_RDWR}, &res) == SD_NOT_FOUND, "savedata: mounting a missing save without CREATE is NOT_FOUND");
    C(sd_mount2(&(SdMount2){.user = 1, .dir = &dn, .blocks = 96, .mode = MODE_RDWR | MODE_CREATE2}, &res) == SD_OK && !strcmp(res.mount_point.data, "/savedata0") && res.mount_status == 1, "savedata: CREATE2 creates and reports CREATED");
    mp = res.mount_point;
    C(sd_mount2(&(SdMount2){.user = 1, .dir = &dn, .blocks = 96, .mode = MODE_RDWR}, &res) == SD_BUSY, "savedata: mounting the same save twice is BUSY");
    char p[64]; snprintf(p, sizeof p, "%s/slot.dat", mp.data);
    extern int GUEST_ABI rt_fs_test_write(const char*, const char*);
    C(rt_fs_test_write(p, "PROGRESS") == 0, "savedata: a file written under the mount point lands in the save");
    SdParam prm; memset(&prm, 0, sizeof prm); snprintf(prm.title, sizeof prm.title, "J-Stars"); snprintf(prm.sub_title, sizeof prm.sub_title, "Slot 1"); prm.user_param = 7;
    C(sd_set_param(&mp, 0, &prm, sizeof prm) == SD_OK, "savedata: SetParam(ALL)");
    C(sd_umount(&mp) == SD_OK && sd_umount(&mp) == SD_NOT_MOUNTED, "savedata: umount once, then NOT_MOUNTED");
    DirName pat = {"SLOT%"}; DirName names[4]; SdParam params[4]; SdSearchInfo infos[4];
    SdSearchCond cond = {.user = 1, .dir = &pat, .key = 0, .order = 0}; SdSearchResult sr = {.dir_names = names, .dir_names_num = 4, .params = params, .infos = infos};
    C(sd_dir_name_search(&cond, &sr) == SD_OK && sr.hit_num == 1 && sr.set_num == 1 && !strcmp(names[0].data, "SLOT01") && params[0].user_param == 7 && !strcmp(params[0].title, "J-Stars") && infos[0].blocks == 96, "savedata: DirNameSearch finds the save with its parameters and capacity");
    DirName pat2 = {"NOPE%"}; cond.dir = &pat2;
    C(sd_dir_name_search(&cond, &sr) == SD_OK && sr.hit_num == 0, "savedata: a non-matching pattern finds nothing");
    SdMountResult res2;
    C(sd_mount2(&(SdMount2){.user = 1, .dir = &dn, .blocks = 96, .mode = MODE_RDONLY}, &res2) == SD_OK && !res2.mount_status, "savedata: remount read-only of an existing save");
    mp = res2.mount_point;
    C(sd_set_param(&mp, 1, "x", 2) == SD_BAD_MOUNTED, "savedata: SetParam on a read-only mount is BAD_MOUNTED");
    C(rt_fs_test_write(p, "NOPE") != 0, "savedata: writing under a read-only mount is refused");
    sd_umount(&mp);
    C(sd_delete(&(SdDelete){.user = 1, .dir = &dn}) == SD_OK, "savedata: delete removes the save");
    cond.dir = &pat;
    C(sd_dir_name_search(&cond, &sr) == SD_OK && sr.hit_num == 0, "savedata: deleted save no longer listed");
#undef C
    return bad;
}
