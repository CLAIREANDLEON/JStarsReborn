// Native Sony controllers on Windows: DualSense, DualSense Edge and DualShock 4, over USB or Bluetooth, read as raw HID
// reports. Without this, a PlayStation controller only worked through a translator (DS4Windows, Steam Input) that presents it
// as an XInput pad. The controller is tried before XInput (jstars_os.c, os_pad_poll); when none is found, or another program
// holds it exclusively, nothing changes.
//
// A reader thread finds the controller (re-checked every second while there is none), reads its input reports as they arrive
// and keeps the latest state; os_sonypad_poll only copies that state, so the game thread never waits on the device. Buttons
// pressed and released between two game reads are still reported once. Rumble is sent from a separate writer thread, only
// when the motor values change. setupapi.dll and hid.dll are loaded at run time, so no link flags change.
//
// Report layouts (byte offsets after the report ID), as documented by the Linux hid-playstation driver:
//   DualShock 4 USB 0x01, Bluetooth 0x01 (basic) and 0x11 (+2 bytes): LX LY RX RY B0 B1 B2 L2 R2
//   DualSense USB 0x01 and Bluetooth 0x31 (+1 byte):                    LX LY RX RY L2 R2 counter B0 B1 B2
//   DualSense Bluetooth 0x01 (basic, until the first output report):    same as DualShock 4
//   B0: d-pad hat in bits 0-3 (8 = released), square, cross, circle, triangle. B1: L1 R1 L2 R2 share/create options L3 R3.
//   B2: PS, touchpad click (DualSense: mute). Sticks are 0..255 with Y growing downwards, as on the console.
#include "jstars_os.h"
#include <stdio.h>
#include <string.h>

// Decodes one input report. Returns 1 when it carried controller state. Shared by every platform so it can be tested.
int os_sonypad_parse(const uint8_t* r, unsigned n, int dualsense, int bluetooth, OsPadState* o) {
    unsigned b; int full;
    if (!n) return 0;
    if (!bluetooth && r[0] == 0x01 && n >= 11) { b = 1; full = dualsense; }
    else if (bluetooth && dualsense && r[0] == 0x31 && n >= 12) { b = 2; full = 1; }
    else if (bluetooth && !dualsense && r[0] == 0x11 && n >= 12) { b = 3; full = 0; }
    else if (bluetooth && r[0] == 0x01 && n >= 10) { b = 1; full = 0; }
    else return 0;
    uint8_t b0, b1, b2;
    o->lx = r[b]; o->ly = r[b + 1]; o->rx = r[b + 2]; o->ry = r[b + 3];
    if (full) { o->l2 = r[b + 4]; o->r2 = r[b + 5]; b0 = r[b + 7]; b1 = r[b + 8]; b2 = r[b + 9]; }
    else { b0 = r[b + 4]; b1 = r[b + 5]; b2 = r[b + 6]; o->l2 = r[b + 7]; o->r2 = r[b + 8]; }
    static const uint32_t hat[8] = {PADBTN_UP, PADBTN_UP | PADBTN_RIGHT, PADBTN_RIGHT, PADBTN_DOWN | PADBTN_RIGHT,
                                    PADBTN_DOWN, PADBTN_DOWN | PADBTN_LEFT, PADBTN_LEFT, PADBTN_UP | PADBTN_LEFT};
    uint32_t x = (b0 & 15) < 8 ? hat[b0 & 15] : 0;
    if (b0 & 0x10) x |= PADBTN_SQUARE;
    if (b0 & 0x20) x |= PADBTN_CROSS;
    if (b0 & 0x40) x |= PADBTN_CIRCLE;
    if (b0 & 0x80) x |= PADBTN_TRIANGLE;
    if (b1 & 0x01) x |= PADBTN_L1;
    if (b1 & 0x02) x |= PADBTN_R1;
    if (b1 & 0x04) x |= PADBTN_L2;
    if (b1 & 0x08) x |= PADBTN_R2;
    if (b1 & 0x20) x |= PADBTN_OPTIONS;   // share/create (0x10) and PS (B2 0x01) are system buttons the game never sees
    if (b1 & 0x40) x |= PADBTN_L3;
    if (b1 & 0x80) x |= PADBTN_R3;
    if (b2 & 0x02) x |= PADBTN_TOUCHPAD;
    o->buttons = x;
    return 1;
}

// Buttons to report at a poll: the current state plus taps that began and ended between two polls (latched), except
// buttons already reported pressed at the previous poll, so a release is never held for an extra frame.
uint32_t os_sonypad_buttons(uint32_t current, uint32_t latched, uint32_t reported_before) {
    return current | (latched & ~reported_before);
}

static uint32_t crc32_sony(const uint8_t* p, size_t n) { // CRC-32 (IEEE) over the 0xA2 output seed byte, then the report
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i <= n; ++i) {
        c ^= i == 0 ? 0xA2u : p[i - 1];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
// Builds the rumble output report for the controller in use (strong = low-frequency left motor, weak = right motor, 0..255).
// Returns its length, or 0 when the device has no usable output report.
unsigned os_sonypad_rumble_report(uint8_t* o, unsigned cap, int dualsense, int bluetooth, unsigned out_len, uint8_t seq, int strong, int weak) {
    // Bluetooth reports are 78 bytes with the CRC at 74; Windows wants the buffer padded to the device's output report length.
    const unsigned len = bluetooth ? (out_len > 78 ? out_len : 78) : out_len;
    if (!len || len > cap || len < 8) return 0;
    memset(o, 0, len);
    if (dualsense && !bluetooth) { o[0] = 0x02; o[1] = 0x03; o[3] = (uint8_t)weak; o[4] = (uint8_t)strong; }      // compatible vibration
    else if (dualsense) { o[0] = 0x31; o[1] = (uint8_t)(seq << 4); o[2] = 0x10; o[3] = 0x03; o[5] = (uint8_t)weak; o[6] = (uint8_t)strong; }
    else if (!bluetooth) { o[0] = 0x05; o[1] = 0x01; o[4] = (uint8_t)weak; o[5] = (uint8_t)strong; }              // motor valid
    else { o[0] = 0x11; o[1] = 0xC0; o[3] = 0x01; o[6] = (uint8_t)weak; o[7] = (uint8_t)strong; }                  // HID + CRC
    if (bluetooth) {
        const uint32_t c = crc32_sony(o, 74);
        o[74] = (uint8_t)c; o[75] = (uint8_t)(c >> 8); o[76] = (uint8_t)(c >> 16); o[77] = (uint8_t)(c >> 24);
    }
    return len;
}

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <wchar.h>

enum { SONY_DS4 = 1, SONY_DUALSENSE = 2 };
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HidAttributes;
typedef BOOLEAN (WINAPI *HidGetAttributesFn)(HANDLE, HidAttributes*);
typedef void (WINAPI *HidGetHidGuidFn)(GUID*);
typedef BOOLEAN (WINAPI *HidGetPreparsedDataFn)(HANDLE, void**);
typedef BOOLEAN (WINAPI *HidFreePreparsedDataFn)(void*);
typedef LONG (WINAPI *HidPGetCapsFn)(void*, USHORT*); // HIDP_CAPS is 32 USHORTs: [2] input, [3] output report length
typedef HDEVINFO (WINAPI *SetupDiGetClassDevsWFn)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI *SetupDiEnumDeviceInterfacesFn)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *SetupDiGetDeviceInterfaceDetailWFn)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *SetupDiDestroyDeviceInfoListFn)(HDEVINFO);
static HidGetAttributesFn p_attr; static HidGetHidGuidFn p_guid; static HidGetPreparsedDataFn p_prep; static HidFreePreparsedDataFn p_free;
static HidPGetCapsFn p_caps; static SetupDiGetClassDevsWFn p_devs; static SetupDiEnumDeviceInterfacesFn p_enum;
static SetupDiGetDeviceInterfaceDetailWFn p_detail; static SetupDiDestroyDeviceInfoListFn p_destroy;

static SRWLOCK g_lock = SRWLOCK_INIT;
static OsPadState g_state;          // latest report
static uint32_t g_latched;          // buttons seen pressed since the last poll
static uint32_t g_reported;         // buttons reported at the last poll
static int g_connected, g_kind, g_bt;
static char g_name[96];
static HANDLE g_out = INVALID_HANDLE_VALUE; static USHORT g_out_len; // write handle and output report length
static unsigned g_out_gen;          // bumped whenever g_out is replaced or closed (the writer keeps its own duplicate)
static HANDLE g_rumble_event; static int g_rumble_strong = -1, g_rumble_weak = -1, g_sent_strong = -1, g_sent_weak = -1;
static volatile LONG g_started;

static int load_apis(void) {
    HMODULE hid = LoadLibraryW(L"hid.dll"), sa = LoadLibraryW(L"setupapi.dll");
    if (!hid || !sa) return 0;
    p_attr = (HidGetAttributesFn)(void*)GetProcAddress(hid, "HidD_GetAttributes");
    p_guid = (HidGetHidGuidFn)(void*)GetProcAddress(hid, "HidD_GetHidGuid");
    p_prep = (HidGetPreparsedDataFn)(void*)GetProcAddress(hid, "HidD_GetPreparsedData");
    p_free = (HidFreePreparsedDataFn)(void*)GetProcAddress(hid, "HidD_FreePreparsedData");
    p_caps = (HidPGetCapsFn)(void*)GetProcAddress(hid, "HidP_GetCaps");
    p_devs = (SetupDiGetClassDevsWFn)(void*)GetProcAddress(sa, "SetupDiGetClassDevsW");
    p_enum = (SetupDiEnumDeviceInterfacesFn)(void*)GetProcAddress(sa, "SetupDiEnumDeviceInterfaces");
    p_detail = (SetupDiGetDeviceInterfaceDetailWFn)(void*)GetProcAddress(sa, "SetupDiGetDeviceInterfaceDetailW");
    p_destroy = (SetupDiDestroyDeviceInfoListFn)(void*)GetProcAddress(sa, "SetupDiDestroyDeviceInfoList");
    return p_attr && p_guid && p_prep && p_free && p_caps && p_devs && p_enum && p_detail && p_destroy;
}

static int sony_kind(USHORT vid, USHORT pid) {
    if (vid != 0x054C) return 0;
    if (pid == 0x0CE6 || pid == 0x0DF2) return SONY_DUALSENSE;                 // DualSense, DualSense Edge
    if (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0) return SONY_DS4;     // DualShock 4 v1, v2, USB wireless adapter
    return 0;
}

// Opens the first supported controller: a read handle (returned) and a write handle (g_out). NULL when there is none.
static HANDLE open_first(int* kind, int* bt, USHORT* in_len, char* name, size_t name_cap) {
    GUID guid; p_guid(&guid);
    HDEVINFO set = p_devs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return NULL;
    HANDLE found = NULL;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; !found && p_enum(set, NULL, &guid, i, &ifd); ++i) {
        DWORD need = 0;
        p_detail(set, &ifd, NULL, 0, &need, NULL);
        if (!need || need > 4096) continue;
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char raw[4096]; } u;
        u.d.cbSize = sizeof u.d;
        if (!p_detail(set, &ifd, &u.d, need, NULL, NULL)) continue;
        const wchar_t* path = u.d.DevicePath;
        HANDLE q = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); // attributes only
        if (q == INVALID_HANDLE_VALUE) continue;
        HidAttributes a; memset(&a, 0, sizeof a); a.Size = sizeof a;
        const int k = p_attr(q, &a) ? sony_kind(a.VendorID, a.ProductID) : 0;
        CloseHandle(q);
        if (!k) continue;
        HANDLE r = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (r == INVALID_HANDLE_VALUE) continue; // held exclusively by another program (for example DS4Windows' exclusive mode)
        USHORT caps[32]; memset(caps, 0, sizeof caps); void* pd = NULL;
        if (p_prep(r, &pd)) { p_caps(pd, caps); p_free(pd); }
        // A second handle for output reports, so rumble never waits behind the blocking read on the first one.
        HANDLE w = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        // USB interface paths contain "vid_054c&pid_..."; Bluetooth paths name the HID service UUID and "vid&0002054c".
        wchar_t low[512]; size_t n = 0;
        for (; path[n] && n < 511; ++n) low[n] = (path[n] >= L'A' && path[n] <= L'Z') ? (wchar_t)(path[n] + 32) : path[n];
        low[n] = 0;
        *kind = k; *bt = wcsstr(low, L"vid_054c") == NULL; *in_len = caps[2];
        AcquireSRWLockExclusive(&g_lock);
        g_out = w; g_out_len = caps[3]; g_sent_strong = g_sent_weak = -1; ++g_out_gen;
        ReleaseSRWLockExclusive(&g_lock);
        snprintf(name, name_cap, "%s (%s, %04x:%04x)", k == SONY_DUALSENSE ? (a.ProductID == 0x0DF2 ? "DualSense Edge" : "DualSense") : "DualShock 4",
                 *bt ? "Bluetooth" : "USB", a.VendorID, a.ProductID);
        found = r;
    }
    p_destroy(set);
    return found;
}

// The writer works on its own duplicate of the write handle: the reader closes g_out when the controller goes away, possibly
// while a WriteFile is in progress here, and a write must never reach a closed (or reused) handle. The duplicate is taken under
// the lock (DuplicateHandle does not block) and closed by this thread only.
static DWORD WINAPI writer_thread(LPVOID unused) {
    (void)unused;
    uint8_t seq = 0;
    static uint8_t buf[1024];
    HANDLE mine = INVALID_HANDLE_VALUE; unsigned mine_gen = 0;
    for (;;) {
        WaitForSingleObject(g_rumble_event, INFINITE);
        HANDLE fresh = INVALID_HANDLE_VALUE; int refresh = 0;
        AcquireSRWLockExclusive(&g_lock);
        const int strong = g_rumble_strong, weak = g_rumble_weak, kind = g_kind, bt = g_bt, connected = g_connected;
        const USHORT out_len = g_out_len;
        if (g_out_gen != mine_gen) {
            refresh = 1; mine_gen = g_out_gen;
            if (g_out != INVALID_HANDLE_VALUE &&
                !DuplicateHandle(GetCurrentProcess(), g_out, GetCurrentProcess(), &fresh, 0, FALSE, DUPLICATE_SAME_ACCESS))
                fresh = INVALID_HANDLE_VALUE;
        }
        const int changed = strong != g_sent_strong || weak != g_sent_weak;
        if (changed) { g_sent_strong = strong; g_sent_weak = weak; }
        ReleaseSRWLockExclusive(&g_lock);
        if (refresh) {
            if (mine != INVALID_HANDLE_VALUE) CloseHandle(mine);
            mine = fresh;
        }
        if (!connected || !changed || mine == INVALID_HANDLE_VALUE || strong < 0) continue;
        const unsigned n = os_sonypad_rumble_report(buf, sizeof buf, kind == SONY_DUALSENSE, bt, out_len, seq, strong, weak);
        seq = (uint8_t)((seq + 1) & 15);
        DWORD wr = 0;
        if (n) WriteFile(mine, buf, n, &wr, NULL); // fails harmlessly if the controller was unplugged meanwhile
    }
}

static DWORD WINAPI reader_thread(LPVOID unused) {
    (void)unused;
    static uint8_t buf[1024];
    for (;;) {
        int kind = 0, bt = 0; USHORT in_len = 0; char name[96];
        HANDLE r = open_first(&kind, &bt, &in_len, name, sizeof name);
        if (!r) { Sleep(1000); continue; }
        AcquireSRWLockExclusive(&g_lock);
        g_kind = kind; g_bt = bt; snprintf(g_name, sizeof g_name, "%s", name);
        memset(&g_state, 0, sizeof g_state); g_state.lx = g_state.ly = g_state.rx = g_state.ry = 128; g_latched = g_reported = 0;
        g_connected = 1;
        ReleaseSRWLockExclusive(&g_lock);
        SetEvent(g_rumble_event); // re-send the current rumble to a controller that just (re)connected
        const DWORD want = in_len && in_len <= sizeof buf ? in_len : (DWORD)sizeof buf;
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(r, buf, want, &n, NULL)) break; // unplugged, switched off, or taken over
            OsPadState s;
            if (!os_sonypad_parse(buf, n, kind == SONY_DUALSENSE, bt, &s)) continue;
            AcquireSRWLockExclusive(&g_lock);
            g_state = s; g_latched |= s.buttons;
            ReleaseSRWLockExclusive(&g_lock);
        }
        AcquireSRWLockExclusive(&g_lock);
        g_connected = 0;
        if (g_out != INVALID_HANDLE_VALUE) { CloseHandle(g_out); g_out = INVALID_HANDLE_VALUE; }
        ++g_out_gen;
        ReleaseSRWLockExclusive(&g_lock);
        SetEvent(g_rumble_event); // the writer drops its duplicate of the old handle
        CloseHandle(r);
        Sleep(250);
    }
}

int os_sonypad_poll(OsPadState* out) {
    if (!InterlockedCompareExchange(&g_started, 1, 0) && load_apis()) {
        g_rumble_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (g_rumble_event) {
            HANDLE t = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            t = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    const int c = g_connected;
    if (c) { *out = g_state; out->buttons = os_sonypad_buttons(g_state.buttons, g_latched, g_reported); g_reported = out->buttons; g_latched = 0; }
    ReleaseSRWLockExclusive(&g_lock);
    return c;
}
const char* os_sonypad_name(void) {
    static char copy[96];
    AcquireSRWLockExclusive(&g_lock); snprintf(copy, sizeof copy, "%s", g_name); ReleaseSRWLockExclusive(&g_lock);
    return copy;
}
void os_sonypad_rumble(int strong, int weak) {
    if (!g_rumble_event) return;
    AcquireSRWLockExclusive(&g_lock);
    const int changed = strong != g_rumble_strong || weak != g_rumble_weak;
    g_rumble_strong = strong; g_rumble_weak = weak;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) SetEvent(g_rumble_event);
}
#else
int os_sonypad_poll(OsPadState* out) { (void)out; return 0; }
const char* os_sonypad_name(void) { return ""; }
void os_sonypad_rumble(int strong, int weak) { (void)strong; (void)weak; }
#endif
