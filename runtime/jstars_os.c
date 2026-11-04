#define _GNU_SOURCE
#include "jstars_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <pthread.h>
void rt_logf(const char* fmt, ...);

static DWORD win_prot(int p) {
    switch (p & 7) {
    case 0: return PAGE_NOACCESS;
    case OS_PROT_R: return PAGE_READONLY;
    case OS_PROT_W: case OS_PROT_R | OS_PROT_W: return PAGE_READWRITE;
    case OS_PROT_X: return PAGE_EXECUTE;
    case OS_PROT_R | OS_PROT_X: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}
size_t os_page_size(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwPageSize; }
// ---- guest address window ------------------------------------------------------------------------------------------
// The console's user space is 40 bits wide and the GPU registers/descriptors carry guest addresses in 40 bits (a shader
// address is stored as address>>8 in a LO/HI register pair and the SDK's own code requires HI == 0). Host allocation
// placement is arbitrary (Windows hands out addresses above 1 TiB), so memory the guest can see is placed inside a
// configured window instead. Explicit (FIXED) requests are honoured as given.
static uintptr_t g_win_lo, g_win_hi, g_win_cur;
static SRWLOCK g_win_lock = SRWLOCK_INIT;
void os_va_window(uint64_t lo, uint64_t hi) { g_win_lo = (uintptr_t)lo; g_win_hi = (uintptr_t)hi; g_win_cur = (uintptr_t)lo; }
typedef void* (*PlaceFn)(void* addr, size_t len, void* ctx);
static void* win_place(size_t len, PlaceFn fn, void* ctx) {
    if (!g_win_hi) return NULL;
    const uintptr_t gran = 65536;
    AcquireSRWLockExclusive(&g_win_lock);
    uintptr_t a = (g_win_cur + gran - 1) & ~(gran - 1);
    for (int pass = 0; pass < 2; ++pass) {
        while (a + len <= g_win_hi) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
            uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.State == MEM_FREE && a + len <= end) {
                void* r = fn((void*)a, len, ctx);
                if (r) { g_win_cur = a + ((len + gran - 1) & ~(gran - 1)); ReleaseSRWLockExclusive(&g_win_lock); return r; }
                a += gran;
                continue;
            }
            a = (end + gran - 1) & ~(gran - 1);
        }
        a = (g_win_lo + gran - 1) & ~(gran - 1);   // wrap once: earlier holes may have been released
    }
    ReleaseSRWLockExclusive(&g_win_lock);
    return NULL;
}
typedef struct { int prot; } AnonCtx;
static void* anon_try(void* addr, size_t len, void* c) { return VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, win_prot(((AnonCtx*)c)->prot)); }
void* os_map_anon(void* hint, size_t len, int prot, int mode) {
    if (mode == OS_MAP_FIXED_NOREPLACE || mode == OS_MAP_ANYWHERE || mode == OS_MAP_FIXED) {
        // Windows cannot overwrite an existing reservation in place; FIXED over free space behaves like NOREPLACE.
        void* r = hint ? VirtualAlloc(hint, len, MEM_RESERVE | MEM_COMMIT, win_prot(prot)) : NULL;
        if (!r && mode == OS_MAP_ANYWHERE) {
            AnonCtx c = {prot};
            r = win_place(len, anon_try, &c);
            if (!r) r = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
        }
        return r;
    }
    return NULL;
}
int os_unmap(void* p, size_t len) {
    (void)len;
    if (UnmapViewOfFile(p)) return 0;
    return VirtualFree(p, 0, MEM_RELEASE) ? 0 : -1;
}
int os_protect(void* p, size_t len, int prot) { DWORD old; return VirtualProtect(p, len, win_prot(prot), &old) ? 0 : -1; }

static HANDLE g_dm;
int os_dm_init(uint64_t size) {
    // SEC_RESERVE: pages are committed only where a view is mapped, so a multi-GiB store costs nothing up front.
    g_dm = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, (DWORD)(size >> 32), (DWORD)size, NULL);
    return g_dm ? 0 : -1;
}
size_t os_dm_granularity(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwAllocationGranularity; }
typedef struct { DWORD acc; uint64_t phys; int prot; } DmCtx;
static void* dm_try(void* addr, size_t len, void* c) {
    DmCtx* d = c;
    void* v = MapViewOfFileEx(g_dm, d->acc, (DWORD)(d->phys >> 32), (DWORD)d->phys, len, addr);
    if (!v) return NULL;
    if (!VirtualAlloc(v, len, MEM_COMMIT, win_prot(d->prot))) { UnmapViewOfFile(v); return NULL; }
    return v;
}
void* os_dm_map(void* hint, size_t len, int prot, uint64_t phys, int mode) {
    if (!g_dm && os_dm_init(6ull << 30)) return NULL;
    DmCtx d = {(prot & OS_PROT_W) ? FILE_MAP_WRITE : FILE_MAP_READ, phys, prot};
    if (prot & OS_PROT_X) d.acc |= FILE_MAP_EXECUTE;
    void* v = hint ? dm_try(hint, len, &d) : NULL;
    if (!v && mode == OS_MAP_ANYWHERE) {
        v = win_place(len, dm_try, &d);
        if (!v) v = dm_try(NULL, len, &d);
    }
    return v;
}
void* os_alloc_aligned(size_t a, size_t n) { return _aligned_malloc(n, a); }
void os_free_aligned(void* p) { _aligned_free(p); }

static void (*g_cb)(const OsFault*);
static int (*g_prehook)(const OsFault*);

// "module+0xRVA" for an address inside a loaded module (the host exe or a DLL); plain pointer otherwise.
static const char* where_host(uintptr_t a, char* out, size_t n) {
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, sizeof name);
        const char* b = strrchr(name, '\\');
        snprintf(out, n, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
    } else {
        snprintf(out, n, "%p", (void*)a);
    }
    return out;
}
static void log_exception(const char* tag, EXCEPTION_POINTERS* e) {
    char w[300];
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    rt_logf("%s: exception 0x%08lx flags 0x%lx at %s (%u params)", tag, (unsigned long)r->ExceptionCode, (unsigned long)r->ExceptionFlags,
            where_host((uintptr_t)r->ExceptionAddress, w, sizeof w), (unsigned)r->NumberParameters);
    for (DWORD i = 0; i < r->NumberParameters && i < 4; ++i) rt_logf("  param[%lu] = 0x%llx", (unsigned long)i, (unsigned long long)r->ExceptionInformation[i]);
    CONTEXT* x = e->ContextRecord;
    rt_logf("  rip=%s rsp=0x%llx rbp=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx", where_host((uintptr_t)x->Rip, w, sizeof w), (unsigned long long)x->Rsp,
            (unsigned long long)x->Rbp, (unsigned long long)x->Rax, (unsigned long long)x->Rcx, (unsigned long long)x->Rdx);
    // return addresses found on the stack that point into a loaded module (a frame-pointer-free poor man's backtrace)
    int shown = 0;
    for (int i = 0; i < 256 && shown < 14; ++i) {
        uintptr_t v = ((uintptr_t*)x->Rsp)[i];
        HMODULE m = NULL;
        if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &m) && m) {
            rt_logf("  stack[%3d] -> %s", i, where_host(v, w, sizeof w));
            ++shown;
        }
    }
}
static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* e) {
    log_exception("UNHANDLED", e);
    fflush(NULL);
    _exit(4);
}
static LONG CALLBACK veh(EXCEPTION_POINTERS* e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    // Exceptions that are not guest faults are only reported (first 64), never handled here: the host's own
    // C++ exceptions (code 0x20474343 'GCC'), SEH status codes, and so on. Debugger traffic is skipped.
    static LONG reported;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_PRIV_INSTRUCTION &&
        c != EXCEPTION_INT_DIVIDE_BY_ZERO && c != EXCEPTION_IN_PAGE_ERROR && c != EXCEPTION_STACK_OVERFLOW &&
        c != EXCEPTION_ARRAY_BOUNDS_EXCEEDED && c != EXCEPTION_DATATYPE_MISALIGNMENT) {
        if (c != 0x406D1388 && c != 0x40010006 && c != 0x4001000A && c != EXCEPTION_BREAKPOINT && c != EXCEPTION_SINGLE_STEP &&
            InterlockedIncrement(&reported) <= 64)
            log_exception("first-chance", e);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* x = e->ContextRecord;
    OsFault f = {0};
    f.signal = (int)c;
    if ((c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_IN_PAGE_ERROR) && e->ExceptionRecord->NumberParameters >= 2) {
        f.code = (int)e->ExceptionRecord->ExceptionInformation[0];
        f.address = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    }
    f.rip = (uintptr_t)x->Rip; f.rsp = (uintptr_t)x->Rsp;
    uint64_t g[16] = {x->Rax, x->Rcx, x->Rdx, x->Rbx, x->Rsp, x->Rbp, x->Rsi, x->Rdi, x->R8, x->R9, x->R10, x->R11, x->R12, x->R13, x->R14, x->R15};
    memcpy(f.gpr, g, sizeof g);
    f.native = e;
    if (c == EXCEPTION_ACCESS_VIOLATION && g_prehook && g_prehook(&f)) return EXCEPTION_CONTINUE_EXECUTION;
    g_cb(&f);
    return EXCEPTION_CONTINUE_SEARCH;
}
void os_install_fault_handler(void (*cb)(const OsFault*)) {
    g_cb = cb;
    AddVectoredExceptionHandler(1, veh);
    SetUnhandledExceptionFilter(unhandled_filter);
}
void os_set_fault_prehook(int (*hook)(const OsFault*)) { g_prehook = hook; }
size_t os_safe_read(void* dst, const void* src, size_t len) {
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), src, dst, len, &got); // a partial copy is reported through got
    return (size_t)got;
}

// ---- files (Windows) ------------------------------------------------------------------------------------
static int werr(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: return OS_E_NOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_WRITE_PROTECT: return OS_E_ACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return OS_E_EXIST;
    case ERROR_DIRECTORY: return OS_E_NOTDIR;
    case ERROR_DIR_NOT_EMPTY: return OS_E_NOTEMPTY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return OS_E_NOSPC;
    case ERROR_TOO_MANY_OPEN_FILES: return OS_E_MFILE;
    case ERROR_FILENAME_EXCED_RANGE: return OS_E_NAMETOOLONG;
    case ERROR_INVALID_PARAMETER: case ERROR_NEGATIVE_SEEK: return OS_E_INVAL;
    default: return OS_E_IO;
    }
}
static wchar_t* wpath(const char* p) {
    int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = malloc((size_t)(n + 8) * sizeof *w);
    if (!w) return NULL;
    wchar_t* out = w;
    // long-path prefix for absolute drive paths
    if (p[0] && p[1] == ':') { memcpy(out, L"\\\\?\\", 4 * sizeof(wchar_t)); out += 4; }
    MultiByteToWideChar(CP_UTF8, 0, p, -1, out, n);
    for (wchar_t* c = out; *c; ++c) if (*c == L'/') *c = L'\\';
    return w;
}
static int64_t ft_ns(FILETIME f) { uint64_t t = ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; return (int64_t)((t - 116444736000000000ull) * 100ull); }
void* os_file_open(const char* path, int flags, int* err) {
    wchar_t* w = wpath(path);
    if (!w) { *err = OS_E_INVAL; return NULL; }
    DWORD acc = ((flags & OS_F_READ) ? GENERIC_READ : 0) | ((flags & OS_F_WRITE) ? GENERIC_WRITE : 0);
    DWORD disp = OPEN_EXISTING;
    if ((flags & OS_F_CREATE) && (flags & OS_F_EXCL)) disp = CREATE_NEW;
    else if ((flags & OS_F_CREATE) && (flags & OS_F_TRUNC)) disp = CREATE_ALWAYS;
    else if (flags & OS_F_CREATE) disp = OPEN_ALWAYS;
    else if (flags & OS_F_TRUNC) disp = TRUNCATE_EXISTING;
    HANDLE h = CreateFileW(w, acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) { *err = werr(GetLastError()); return NULL; }
    BY_HANDLE_FILE_INFORMATION bi;
    if (GetFileInformationByHandle(h, &bi) && (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (flags & OS_F_WRITE)) { CloseHandle(h); *err = OS_E_ISDIR; return NULL; }
    return h;
}
int64_t os_file_pread(void* f, void* buf, size_t n, uint64_t off, int* err) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0}; ov.Offset = (DWORD)(off + done); ov.OffsetHigh = (DWORD)((off + done) >> 32);
        DWORD chunk = (DWORD)((n - done) > 0x40000000u ? 0x40000000u : (n - done)), got = 0;
        if (!ReadFile((HANDLE)f, (char*)buf + done, chunk, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            *err = werr(e); return done ? (int64_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (int64_t)done;
}
int64_t os_file_pwrite(void* f, const void* buf, size_t n, uint64_t off, int* err) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0}; ov.Offset = (DWORD)(off + done); ov.OffsetHigh = (DWORD)((off + done) >> 32);
        DWORD chunk = (DWORD)((n - done) > 0x40000000u ? 0x40000000u : (n - done)), put = 0;
        if (!WriteFile((HANDLE)f, (const char*)buf + done, chunk, &put, &ov)) { *err = werr(GetLastError()); return done ? (int64_t)done : -1; }
        done += put;
    }
    return (int64_t)done;
}
int os_file_size(void* f, uint64_t* size) { LARGE_INTEGER s; if (!GetFileSizeEx((HANDLE)f, &s)) return -1; *size = (uint64_t)s.QuadPart; return 0; }
int os_file_truncate(void* f, uint64_t size) {
    LARGE_INTEGER p; p.QuadPart = (LONGLONG)size;
    return SetFilePointerEx((HANDLE)f, p, NULL, FILE_BEGIN) && SetEndOfFile((HANDLE)f) ? 0 : -1;
}
int os_file_sync(void* f) { return FlushFileBuffers((HANDLE)f) ? 0 : -1; }
void os_file_close(void* f) { CloseHandle((HANDLE)f); }
int os_stat(const char* path, OsStat* st) {
    wchar_t* w = wpath(path);
    if (!w) return OS_E_INVAL;
    WIN32_FILE_ATTRIBUTE_DATA d;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    DWORD e = ok ? 0 : GetLastError();
    free(w);
    if (!ok) return werr(e);
    st->size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    st->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->atime_ns = ft_ns(d.ftLastAccessTime); st->mtime_ns = ft_ns(d.ftLastWriteTime); st->ctime_ns = ft_ns(d.ftCreationTime);
    return OS_OK;
}
FILE* os_fopen(const char* path, const char* mode) {
    wchar_t* w = wpath(path);
    if (!w) return NULL;
    wchar_t m[16]; size_t k = 0;
    for (; mode[k] && k < 15; ++k) m[k] = (wchar_t)(unsigned char)mode[k];
    m[k] = 0;
    FILE* f = _wfopen(w, m);
    free(w);
    return f;
}
int os_mkdir(const char* path) { wchar_t* w = wpath(path); if (!w) return OS_E_INVAL; BOOL ok = CreateDirectoryW(w, NULL); DWORD e = GetLastError(); free(w); return ok ? OS_OK : werr(e); }
int os_unlink(const char* path) { wchar_t* w = wpath(path); if (!w) return OS_E_INVAL; BOOL ok = DeleteFileW(w); DWORD e = GetLastError(); free(w); return ok ? OS_OK : werr(e); }
int os_rmdir(const char* path) { wchar_t* w = wpath(path); if (!w) return OS_E_INVAL; BOOL ok = RemoveDirectoryW(w); DWORD e = GetLastError(); free(w); return ok ? OS_OK : werr(e); }
int os_rename(const char* a, const char* b) {
    wchar_t* wa = wpath(a); wchar_t* wb = wpath(b);
    if (!wa || !wb) { free(wa); free(wb); return OS_E_INVAL; }
    BOOL ok = MoveFileExW(wa, wb, MOVEFILE_REPLACE_EXISTING); DWORD e = GetLastError();
    free(wa); free(wb);
    return ok ? OS_OK : werr(e);
}
typedef struct { HANDLE h; WIN32_FIND_DATAW d; int first; } WinDir;
void* os_dir_open(const char* path, int* err) {
    size_t n = strlen(path);
    char* pat = malloc(n + 3);
    memcpy(pat, path, n); if (n && path[n - 1] != '/' && path[n - 1] != '\\') pat[n++] = '/'; pat[n++] = '*'; pat[n] = 0;
    wchar_t* w = wpath(pat); free(pat);
    if (!w) { *err = OS_E_INVAL; return NULL; }
    WinDir* d = calloc(1, sizeof *d);
    d->h = FindFirstFileW(w, &d->d);
    DWORD e = GetLastError(); free(w);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); *err = werr(e); return NULL; }
    d->first = 1;
    return d;
}
int os_dir_next(void* dv, char* name, size_t cap, int* is_dir) {
    WinDir* d = dv;
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->d)) return 0;
        d->first = 0;
        if (!wcscmp(d->d.cFileName, L".") || !wcscmp(d->d.cFileName, L"..")) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, d->d.cFileName, -1, name, (int)cap, NULL, NULL)) continue;
        *is_dir = (d->d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return 1;
    }
}
void os_dir_close(void* dv) { WinDir* d = dv; FindClose(d->h); free(d); }

// ---- controller sources (Windows: XInput, then the keyboard) --------------------------------------------------
#include <xinput.h>
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *XInputSetStateFn)(DWORD, XINPUT_VIBRATION*);
static XInputGetStateFn g_xi_get; static XInputSetStateFn g_xi_set; static int g_xi_tried, g_xi_user = -1;
static void xi_load(void) {
    if (g_xi_tried) return;
    g_xi_tried = 1;
    const char* names[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (size_t i = 0; i < 3 && !g_xi_get; i++) {
        HMODULE m = LoadLibraryA(names[i]);
        if (!m) continue;
        g_xi_get = (XInputGetStateFn)(void*)GetProcAddress(m, "XInputGetState");
        g_xi_set = (XInputSetStateFn)(void*)GetProcAddress(m, "XInputSetState");
    }
}
static uint8_t stick_byte(int v, int dead) {
    if (v > -dead && v < dead) return 128;
    int b = (v + 32768) >> 8;
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}
// Down now, or pressed since the previous query (bit 0): a tap shorter than one game frame is still seen once.
static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8001) != 0; }
static const int k_mapped_keys[] = {VK_RETURN, VK_SPACE, VK_BACK, 'F', 'R', 'Q', 'E', VK_LSHIFT, VK_LCONTROL, 'P', VK_ESCAPE, 'T', 'V', 'B',
                                    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'A', 'D', 'W', 'S', VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD8, VK_NUMPAD5,
                                    'I', 'J', 'K', 'L'};
static int g_kb_focused_before;
int os_pad_poll(OsPadState* out, int allow_keyboard, int window_focused) {
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 128;
    if (os_sonypad_poll(out)) return OS_PAD_SONY; // DualSense / DualShock 4 read directly (jstars_sonypad.c)
    xi_load();
    if (g_xi_get) {
        // The active controller is read on every call. Empty slots are probed at most every 500 ms: XInputGetState on a
        // slot without a controller is slow, and the game reads the pad every frame. A controller that is unplugged is
        // dropped at once; one that is plugged in (again) is picked up by the next probe.
        static uint64_t next_probe_ms;
        const uint64_t now_ms = GetTickCount64();
        const int probe = g_xi_user < 0 && now_ms >= next_probe_ms;
        if (probe) next_probe_ms = now_ms + 500;
        for (DWORD u = 0; u < 4; u++) {
            if (g_xi_user >= 0 && (int)u != g_xi_user) continue;
            if (g_xi_user < 0 && !probe) break;
            XINPUT_STATE st;
            if (g_xi_get(u, &st) != ERROR_SUCCESS) { if ((int)u == g_xi_user) { g_xi_user = -1; next_probe_ms = 0; } continue; }
            g_xi_user = (int)u;
            const XINPUT_GAMEPAD* g = &st.Gamepad; WORD b = g->wButtons;
            if (b & XINPUT_GAMEPAD_A) out->buttons |= PADBTN_CROSS;
            if (b & XINPUT_GAMEPAD_B) out->buttons |= PADBTN_CIRCLE;
            if (b & XINPUT_GAMEPAD_X) out->buttons |= PADBTN_SQUARE;
            if (b & XINPUT_GAMEPAD_Y) out->buttons |= PADBTN_TRIANGLE;
            if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) out->buttons |= PADBTN_L1;
            if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) out->buttons |= PADBTN_R1;
            if (b & XINPUT_GAMEPAD_START) out->buttons |= PADBTN_OPTIONS;
            if (b & XINPUT_GAMEPAD_BACK) out->buttons |= PADBTN_TOUCHPAD;
            if (b & XINPUT_GAMEPAD_LEFT_THUMB) out->buttons |= PADBTN_L3;
            if (b & XINPUT_GAMEPAD_RIGHT_THUMB) out->buttons |= PADBTN_R3;
            if (b & XINPUT_GAMEPAD_DPAD_UP) out->buttons |= PADBTN_UP;
            if (b & XINPUT_GAMEPAD_DPAD_DOWN) out->buttons |= PADBTN_DOWN;
            if (b & XINPUT_GAMEPAD_DPAD_LEFT) out->buttons |= PADBTN_LEFT;
            if (b & XINPUT_GAMEPAD_DPAD_RIGHT) out->buttons |= PADBTN_RIGHT;
            out->l2 = g->bLeftTrigger >= 30 ? g->bLeftTrigger : 0;
            out->r2 = g->bRightTrigger >= 30 ? g->bRightTrigger : 0;
            if (out->l2 > 0) out->buttons |= PADBTN_L2;
            if (out->r2 > 0) out->buttons |= PADBTN_R2;
            out->lx = stick_byte(g->sThumbLX, 7849);  out->ly = stick_byte(-(int)g->sThumbLY - 1, 7849); // console Y grows downwards
            out->rx = stick_byte(g->sThumbRX, 8689);  out->ry = stick_byte(-(int)g->sThumbRY - 1, 8689);
            return OS_PAD_XINPUT;
        }
    }
    if (!allow_keyboard) return OS_PAD_NONE;
    if (window_focused && !g_kb_focused_before) {
        // The game window just got the focus: forget presses made in other programs (bit 0 of GetAsyncKeyState) and report
        // a neutral keyboard for this one read.
        for (size_t i = 0; i < sizeof k_mapped_keys / sizeof *k_mapped_keys; ++i) (void)GetAsyncKeyState(k_mapped_keys[i]);
        g_kb_focused_before = 1;
        return OS_PAD_KEYBOARD;
    }
    g_kb_focused_before = window_focused;
    if (window_focused) { // keyboard mapping, documented in docs/INPUT.md
        if (key(VK_RETURN) || key(VK_SPACE)) out->buttons |= PADBTN_CROSS;
        if (key(VK_BACK)) out->buttons |= PADBTN_CIRCLE;
        if (key('F')) out->buttons |= PADBTN_SQUARE;
        if (key('R')) out->buttons |= PADBTN_TRIANGLE;
        if (key('Q')) out->buttons |= PADBTN_L1;
        if (key('E')) out->buttons |= PADBTN_R1;
        if (key(VK_LSHIFT)) { out->buttons |= PADBTN_L2; out->l2 = 255; }
        if (key(VK_LCONTROL)) { out->buttons |= PADBTN_R2; out->r2 = 255; }
        if (key('P') || key(VK_ESCAPE)) out->buttons |= PADBTN_OPTIONS;
        if (key('T')) out->buttons |= PADBTN_TOUCHPAD;
        if (key('V')) out->buttons |= PADBTN_L3;
        if (key('B')) out->buttons |= PADBTN_R3;
        if (key(VK_UP)) out->buttons |= PADBTN_UP;
        if (key(VK_DOWN)) out->buttons |= PADBTN_DOWN;
        if (key(VK_LEFT)) out->buttons |= PADBTN_LEFT;
        if (key(VK_RIGHT)) out->buttons |= PADBTN_RIGHT;
        if (key('A')) out->lx = 0;   else if (key('D')) out->lx = 255;
        if (key('W')) out->ly = 0;   else if (key('S')) out->ly = 255;
        if (key(VK_NUMPAD4) || key('J')) out->rx = 0; else if (key(VK_NUMPAD6) || key('L')) out->rx = 255;
        if (key(VK_NUMPAD8) || key('I')) out->ry = 0; else if (key(VK_NUMPAD5) || key('K')) out->ry = 255;
    }
    return OS_PAD_KEYBOARD; // the keyboard is always "connected" while enabled; unfocused it reports a neutral state
}
void os_pad_rumble(int large, int small) {
    os_sonypad_rumble(large, small); // ignored unless a Sony controller is connected
    if (!g_xi_set || g_xi_user < 0) return;
    XINPUT_VIBRATION v = { (WORD)(large * 257), (WORD)(small * 257) };
    g_xi_set((DWORD)g_xi_user, &v);
}

void os_setenv(const char* name, const char* value) { _putenv_s(name, value); }
int os_thread_sample(uintptr_t host_thread, OsThreadSample* out) {
    memset(out, 0, sizeof *out);
    HANDLE h = pthread_gethandle((pthread_t)host_thread);
    if (!h || SuspendThread(h) == (DWORD)-1) return -1;
    CONTEXT c; memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int ok = GetThreadContext(h, &c);
    if (ok) {
        out->rip = (uintptr_t)c.Rip; out->rsp = (uintptr_t)c.Rsp; out->rbp = (uintptr_t)c.Rbp;
        out->nwords = (int)(os_safe_read(out->words, (const void*)c.Rsp, sizeof out->words) / sizeof(uintptr_t));
    }
    ResumeThread(h);
    return ok ? 0 : -1;
}
const char* os_where(uintptr_t addr, char* out, size_t n) { return where_host(addr, out, n); }

#else /* Linux */
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

static int lprot(int p) { return (p & OS_PROT_R ? PROT_READ : 0) | (p & OS_PROT_W ? PROT_WRITE : 0) | (p & OS_PROT_X ? PROT_EXEC : 0); }
size_t os_page_size(void) { return (size_t)sysconf(_SC_PAGESIZE); }
static int lflags(int mode) { return mode == OS_MAP_FIXED ? MAP_FIXED : mode == OS_MAP_FIXED_NOREPLACE ? MAP_FIXED_NOREPLACE : 0; }
// Guest address window: see the Windows implementation for the reason. First-fit with a moving cursor, using MAP_FIXED_NOREPLACE.
static uintptr_t g_win_lo, g_win_hi, g_win_cur;
static pthread_mutex_t g_win_lock = PTHREAD_MUTEX_INITIALIZER;
void os_va_window(uint64_t lo, uint64_t hi) { g_win_lo = (uintptr_t)lo; g_win_hi = (uintptr_t)hi; g_win_cur = (uintptr_t)lo; }
static void* win_map(size_t len, int prot, int flags, int fd, off_t off) {
    if (!g_win_hi) return NULL;
    const uintptr_t gran = 16384, span = (len + gran - 1) & ~(gran - 1);
    pthread_mutex_lock(&g_win_lock);
    uintptr_t a = (g_win_cur + gran - 1) & ~(gran - 1);
    for (int pass = 0; pass < 2; ++pass) {
        for (int tries = 0; a + span <= g_win_hi && tries < 4096; ++tries) {
            void* r = mmap((void*)a, len, lprot(prot), flags | MAP_FIXED_NOREPLACE, fd, off);
            if (r != MAP_FAILED && (uintptr_t)r == a) { g_win_cur = a + span; pthread_mutex_unlock(&g_win_lock); return r; }
            if (r != MAP_FAILED) munmap(r, len);   // an old kernel ignored the NOREPLACE hint
            a += span > (1u << 24) ? span : (1u << 24);
        }
        a = (g_win_lo + gran - 1) & ~(gran - 1);
    }
    pthread_mutex_unlock(&g_win_lock);
    return NULL;
}
void* os_map_anon(void* hint, size_t len, int prot, int mode) {
    void* r = mmap(hint, len, lprot(prot), MAP_PRIVATE | MAP_ANONYMOUS | lflags(mode), -1, 0);
    if (r == MAP_FAILED && mode == OS_MAP_ANYWHERE) {
        r = win_map(len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (!r) r = mmap(NULL, len, lprot(prot), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return r == MAP_FAILED ? NULL : r;
    }
    return r == MAP_FAILED ? NULL : r;
}
int os_unmap(void* p, size_t len) { return munmap(p, len); }
int os_protect(void* p, size_t len, int prot) { return mprotect(p, len, lprot(prot)); }
static int g_fd = -1;
int os_dm_init(uint64_t size) {
    g_fd = memfd_create("jstars-direct-memory", MFD_CLOEXEC);
    return g_fd < 0 || ftruncate(g_fd, (off_t)size) ? -1 : 0;
}
size_t os_dm_granularity(void) { return 16384; }
void* os_dm_map(void* hint, size_t len, int prot, uint64_t phys, int mode) {
    if (g_fd < 0 && os_dm_init(6ull << 30)) return NULL;
    void* r = hint ? mmap(hint, len, lprot(prot), MAP_SHARED | lflags(mode), g_fd, (off_t)phys) : MAP_FAILED;
    if (r == MAP_FAILED && mode == OS_MAP_ANYWHERE) {
        r = win_map(len, prot, MAP_SHARED, g_fd, (off_t)phys);
        if (!r) r = mmap(NULL, len, lprot(prot), MAP_SHARED, g_fd, (off_t)phys);
        return r == MAP_FAILED ? NULL : r;
    }
    return r == MAP_FAILED ? NULL : r;
}
void* os_alloc_aligned(size_t a, size_t n) { return aligned_alloc(a, (n + a - 1) & ~(a - 1)); }
void os_free_aligned(void* p) { free(p); }

static void (*g_cb)(const OsFault*);
static int (*g_prehook)(const OsFault*);
static void handler(int sig, siginfo_t* si, void* ctx) {
    ucontext_t* uc = ctx;
    OsFault f = {0};
    f.signal = sig; f.code = si->si_code; f.address = (uintptr_t)si->si_addr;
    f.rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]; f.rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    static const int idx[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (int i = 0; i < 16; ++i) f.gpr[i] = (uint64_t)uc->uc_mcontext.gregs[idx[i]];
    f.native = uc;
    if (sig == SIGSEGV && g_prehook && g_prehook(&f)) return; // handled: the instruction is retried
    g_cb(&f);
    _exit(4);
}
void os_set_fault_prehook(int (*hook)(const OsFault*)) { g_prehook = hook; }
size_t os_safe_read(void* dst, const void* src, size_t len) {
    struct iovec l = {dst, len}, r = {(void*)src, len};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (n >= 0) return (size_t)n;
    // Some unmapped page lies in the range: copy page by page up to the first failure.
    size_t done = 0, pg = 4096;
    while (done < len) {
        size_t chunk = pg - (((uintptr_t)src + done) & (pg - 1));
        if (chunk > len - done) chunk = len - done;
        struct iovec l2 = {(char*)dst + done, chunk}, r2 = {(char*)src + done, chunk};
        if (process_vm_readv(getpid(), &l2, 1, &r2, 1, 0) != (ssize_t)chunk) break;
        done += chunk;
    }
    return done;
}
void os_install_fault_handler(void (*cb)(const OsFault*)) {
    g_cb = cb;
    static uint8_t alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
}

// ---- files (Linux) --------------------------------------------------------------------------------------
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
static int lerr(int e) {
    switch (e) {
    case ENOENT: return OS_E_NOENT;
    case EACCES: case EPERM: case EROFS: return OS_E_ACCES;
    case EEXIST: return OS_E_EXIST;
    case ENOTDIR: return OS_E_NOTDIR;
    case EISDIR: return OS_E_ISDIR;
    case EINVAL: return OS_E_INVAL;
    case ENOSPC: return OS_E_NOSPC;
    case ENOTEMPTY: return OS_E_NOTEMPTY;
    case EMFILE: case ENFILE: return OS_E_MFILE;
    case EBUSY: return OS_E_BUSY;
    case ENAMETOOLONG: return OS_E_NAMETOOLONG;
    default: return OS_E_IO;
    }
}
void* os_file_open(const char* path, int flags, int* err) {
    int o = (flags & OS_F_WRITE) ? ((flags & OS_F_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & OS_F_CREATE) o |= O_CREAT;
    if (flags & OS_F_TRUNC) o |= O_TRUNC;
    if (flags & OS_F_EXCL) o |= O_EXCL;
    int fd = open(path, o | O_CLOEXEC, 0644);
    if (fd < 0) { *err = lerr(errno); return NULL; }
    return (void*)(intptr_t)(fd + 1);
}
#define FDOF(f) ((int)((intptr_t)(f) - 1))
int64_t os_file_pread(void* f, void* buf, size_t n, uint64_t off, int* err) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(FDOF(f), (char*)buf + done, n - done, (off_t)(off + done));
        if (r < 0) { if (errno == EINTR) continue; *err = lerr(errno); return done ? (int64_t)done : -1; }
        if (r == 0) break;
        done += (size_t)r;
    }
    return (int64_t)done;
}
int64_t os_file_pwrite(void* f, const void* buf, size_t n, uint64_t off, int* err) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pwrite(FDOF(f), (const char*)buf + done, n - done, (off_t)(off + done));
        if (r < 0) { if (errno == EINTR) continue; *err = lerr(errno); return done ? (int64_t)done : -1; }
        done += (size_t)r;
    }
    return (int64_t)done;
}
int os_file_size(void* f, uint64_t* size) { struct stat s; if (fstat(FDOF(f), &s)) return -1; *size = (uint64_t)s.st_size; return 0; }
int os_file_truncate(void* f, uint64_t size) { return ftruncate(FDOF(f), (off_t)size); }
int os_file_sync(void* f) { return fsync(FDOF(f)); }
void os_file_close(void* f) { close(FDOF(f)); }
int os_stat(const char* path, OsStat* st) {
    struct stat s;
    if (stat(path, &s)) return lerr(errno);
    st->size = (uint64_t)s.st_size; st->is_dir = S_ISDIR(s.st_mode);
    st->atime_ns = (int64_t)s.st_atim.tv_sec * 1000000000ll + s.st_atim.tv_nsec;
    st->mtime_ns = (int64_t)s.st_mtim.tv_sec * 1000000000ll + s.st_mtim.tv_nsec;
    st->ctime_ns = (int64_t)s.st_ctim.tv_sec * 1000000000ll + s.st_ctim.tv_nsec;
    return OS_OK;
}
FILE* os_fopen(const char* path, const char* mode) { return fopen(path, mode); }
int os_mkdir(const char* path) { return mkdir(path, 0755) ? lerr(errno) : OS_OK; }
int os_unlink(const char* path) { return unlink(path) ? lerr(errno) : OS_OK; }
int os_rmdir(const char* path) { return rmdir(path) ? lerr(errno) : OS_OK; }
int os_rename(const char* a, const char* b) { return rename(a, b) ? lerr(errno) : OS_OK; }
void* os_dir_open(const char* path, int* err) { DIR* d = opendir(path); if (!d) *err = lerr(errno); return d; }
int os_dir_next(void* d, char* name, size_t cap, int* is_dir) {
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, cap, "%s", e->d_name);
        *is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) { /* rare filesystems: caller treats unknown as file */ }
        return 1;
    }
    return 0;
}
void os_dir_close(void* d) { closedir(d); }

// ---- controller sources: none on Linux yet ------------------------------------------------------------------
int os_pad_poll(OsPadState* out, int allow_keyboard, int window_focused) {
    (void)allow_keyboard; (void)window_focused;
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = 128;
    return OS_PAD_NONE;
}
void os_pad_rumble(int large, int small) { (void)large; (void)small; }

void os_setenv(const char* name, const char* value) { setenv(name, value, 1); }
int os_thread_sample(uintptr_t host_thread, OsThreadSample* out) { (void)host_thread; memset(out, 0, sizeof *out); return -1; }
const char* os_where(uintptr_t addr, char* out, size_t n) { snprintf(out, n, "%p", (void*)addr); return out; }
#endif
