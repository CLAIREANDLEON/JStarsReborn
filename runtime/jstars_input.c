// libScePad / libSceMouse. The pad contract follows the SDK layout (ScePadData); the data comes from one of
// these sources, chosen at start-up and recorded in the log:
//   scripted   --pad-script <file>: timed test input for automated runs. It is NOT a player controller; every
//              transition it injects is logged ("SCRIPTED TEST INPUT") so a run's evidence shows what drove the game.
//   xinput     the first connected XInput controller (Windows).
//   keyboard   the keyboard while the game window has focus (Windows; mapping in docs/INPUT.md). The keyboard is
//              reported as a connected controller so the game does not stop at its "connect a controller" screen.
//   none       no source: connected=0 and neutral sticks (Linux builds, --no-keyboard without a controller).
// Only the five libScePad functions and sceMouseInit that the game imports are provided; motion sensors, touch
// and the light bar are not (the game never imports them), and orientation/touch data are reported neutral.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

typedef struct { uint8_t touchNum, reserve[3]; uint32_t held; struct { uint16_t x, y; uint8_t id, reserve[3]; } touch[2]; } PadTouchData;
typedef struct {
    uint32_t buttons;
    uint8_t left[2], right[2];
    uint8_t l2, r2, pad2[2];
    float orientation[4], accel[3], gyro[3];
    PadTouchData touch;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t connectedCount, reserve[2], uniqueLen, unique[12];
} PadData;
_Static_assert(offsetof(PadData, left) == 4 && offsetof(PadData, orientation) == 12 && offsetof(PadData, connected) == 76 &&
               offsetof(PadData, timestamp) == 80, "pad layout must match the SDK ScePadData");

// ---- scripted test input ---------------------------------------------------------------------------------------
// Lines (seconds are counted from the first scePadOpen; '#' starts a comment):
//   at <s> press <BUTTON>[+<BUTTON>...] [for <ms>]            default hold 150 ms
//   at <s> stick left|right <x> <y> [for <ms>]                x,y 0..255 (128 = centre), default hold 500 ms
//   every <ms> from <s> to <s> press <BUTTON>[+...] [for <ms>]  repeating press
// BUTTON: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 OPTIONS TOUCHPAD UP DOWN LEFT RIGHT
typedef struct { double t0, t1; uint32_t buttons; int stick; uint8_t x, y; double period, end; } ScriptEv; // period>0: repeating
static ScriptEv* g_ev; static size_t g_nev;
static char g_script_path[512]; static int g_script_on, g_keyboard_ok = 1, g_focus_hooked;
static int (*g_focus)(void);
static uint64_t g_open_us; // time of the first scePadOpen
static uint32_t g_last_logged = 0xffffffffu; static int g_last_source = -1;

static const struct { const char* n; uint32_t b; } k_names[] = {
    {"CROSS", PADBTN_CROSS}, {"CIRCLE", PADBTN_CIRCLE}, {"SQUARE", PADBTN_SQUARE}, {"TRIANGLE", PADBTN_TRIANGLE},
    {"L1", PADBTN_L1}, {"R1", PADBTN_R1}, {"L2", PADBTN_L2}, {"R2", PADBTN_R2}, {"L3", PADBTN_L3}, {"R3", PADBTN_R3},
    {"OPTIONS", PADBTN_OPTIONS}, {"TOUCHPAD", PADBTN_TOUCHPAD}, {"UP", PADBTN_UP}, {"DOWN", PADBTN_DOWN},
    {"LEFT", PADBTN_LEFT}, {"RIGHT", PADBTN_RIGHT}};
static uint32_t parse_buttons(const char* s, int* ok) {
    uint32_t m = 0; *ok = 1;
    char buf[128]; snprintf(buf, sizeof buf, "%s", s);
    for (char* tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        for (char* c = tok; *c; c++) *c = (char)toupper((unsigned char)*c);
        int found = 0;
        for (size_t i = 0; i < sizeof k_names / sizeof *k_names; i++) if (!strcmp(tok, k_names[i].n)) { m |= k_names[i].b; found = 1; }
        if (!found) *ok = 0;
    }
    return m;
}
static void names_of(uint32_t m, char* out, size_t cap) {
    out[0] = 0;
    for (size_t i = 0; i < sizeof k_names / sizeof *k_names; i++)
        if (m & k_names[i].b) { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%s", l ? "+" : "", k_names[i].n); }
    if (!out[0]) snprintf(out, cap, "(none)");
}
static void add_ev(const ScriptEv* e) { g_ev = realloc(g_ev, (g_nev + 1) * sizeof *g_ev); g_ev[g_nev++] = *e; }

// One script line -> one event. Returns 1 = event parsed, 0 = blank/comment line, -1 = error (message in err).
// (Kept separate from the file loop: mingw-w64 GCC 13 hits an internal compiler error, choose_baseaddr in i386.cc,
// when the double locals and the stdio loop share one frame at -O2.)
static __attribute__((noinline)) int parse_line(char* line, ScriptEv* ev, char* err, size_t errcap) {
    char* h = strchr(line, '#'); if (h) *h = 0;
    char btn[64] = "", which[16] = "";
    double s0 = 0, s1 = 0, period = 0, hold = 0; int x = 0, y = 0, ok = 1;
    const int has_for = strstr(line, " for ") != NULL;
    memset(ev, 0, sizeof *ev);
    if (sscanf(line, " every %lf from %lf to %lf press %63s for %lf", &period, &s0, &s1, btn, &hold) >= 4) {
        if (!has_for) hold = 150;
        ev->buttons = parse_buttons(btn, &ok);
        if (!ok || period <= 0) { snprintf(err, errcap, "bad 'every' line"); return -1; }
        ev->t0 = s0; ev->t1 = s0 + hold / 1000.0; ev->period = period / 1000.0; ev->end = s1;
        return 1;
    }
    if (sscanf(line, " at %lf press %63s for %lf", &s0, btn, &hold) >= 2) {
        if (!has_for) hold = 150;
        ev->buttons = parse_buttons(btn, &ok);
        if (!ok) { snprintf(err, errcap, "unknown button in '%s'", btn); return -1; }
        ev->t0 = s0; ev->t1 = s0 + hold / 1000.0;
        return 1;
    }
    if (sscanf(line, " at %lf stick %15s %d %d for %lf", &s0, which, &x, &y, &hold) >= 4) {
        if (!has_for) hold = 500;
        ev->stick = !strcmp(which, "right") ? 2 : 1; ev->x = (uint8_t)x; ev->y = (uint8_t)y; ev->t0 = s0; ev->t1 = s0 + hold / 1000.0;
        return 1;
    }
    char* q = line; while (isspace((unsigned char)*q)) q++;
    if (!*q) return 0;
    snprintf(err, errcap, "unrecognised line: %s", q);
    return -1;
}

int rt_input_load_script(const char* path) {
    FILE* f = os_fopen(path, "r");
    if (!f) { rt_logf("pad script %s: cannot open", path); return -1; }
    char line[256], err[128]; int ln = 0, bad = 0;
    ScriptEv ev;
    while (fgets(line, sizeof line, f)) {
        ln++;
        int r = parse_line(line, &ev, err, sizeof err);
        if (r > 0) add_ev(&ev);
        else if (r < 0) { bad++; rt_logf("pad script %s:%d: %s", path, ln, err); }
    }
    fclose(f);
    snprintf(g_script_path, sizeof g_script_path, "%s", path);
    g_script_on = 1;
    rt_logf("pad: SCRIPTED TEST INPUT loaded from %s (%zu events, %d bad lines). This is automated test input, not a controller", path, g_nev, bad);
    return bad ? -1 : 0;
}
static int g_none;
void rt_input_set_keyboard(int enabled) { g_keyboard_ok = enabled; }
void rt_input_set_none(int on) { g_none = on; } // --pad-none: report no controller at all (even if an XInput pad is attached)
void rt_input_set_focus_hook(int (*fn)(void)) { g_focus = fn; g_focus_hooked = fn != NULL; }

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000; }

static void script_state(double t, OsPadState* st) {
    for (size_t i = 0; i < g_nev; i++) {
        const ScriptEv* e = &g_ev[i];
        int active = 0;
        if (e->period > 0) {
            if (t >= e->t0 && t <= e->end) { double x = t - e->t0, ph = x - (double)(long long)(x / e->period) * e->period; active = ph < (e->t1 - e->t0); }
        } else active = t >= e->t0 && t < e->t1;
        if (!active) continue;
        if (e->stick == 1) { st->lx = e->x; st->ly = e->y; }
        else if (e->stick == 2) { st->rx = e->x; st->ry = e->y; }
        else {
            st->buttons |= e->buttons;
            if (e->buttons & PADBTN_L2) st->l2 = 255;
            if (e->buttons & PADBTN_R2) st->r2 = 255;
        }
    }
}

static int GUEST_ABI p_init(void) { return 0; }
static int GUEST_ABI p_open(int user, int type, int index, const void* param) {
    (void)user; (void)type; (void)index; (void)param;
    if (!g_open_us) {
        g_open_us = now_us();
        rt_logf("pad: scePadOpen; source order: %s%s%s", g_script_on ? "scripted test input, " : "", "XInput controller, ",
                g_keyboard_ok && g_focus_hooked ? "keyboard (game window focused)" : "no keyboard");
    }
    return 1;
}
static int GUEST_ABI p_close(int handle) { return handle == 1 ? 0 : (int)0x80920003; }
static int GUEST_ABI p_read(int handle, PadData* d) {
    if (handle != 1) return (int)0x80920003; // SCE_PAD_ERROR_INVALID_HANDLE
    if (!d) return (int)0x80920001;
    OsPadState st; int source = OS_PAD_NONE; int scripted = 0;
    if (g_none) {
        memset(&st, 0, sizeof st); st.lx = st.ly = st.rx = st.ry = 128; // source stays OS_PAD_NONE: connected=0
    } else if (g_script_on) {
        memset(&st, 0, sizeof st); st.lx = st.ly = st.rx = st.ry = 128;
        script_state((double)(now_us() - g_open_us) / 1e6, &st);
        source = OS_PAD_KEYBOARD + 1; scripted = 1; // reported as source 3 in the log only
    } else {
        const int focused = g_focus ? g_focus() : 1;
        source = os_pad_poll(&st, g_keyboard_ok && g_focus_hooked, focused);
        // Live input reaches the game only while its window is in the foreground: a controller (or keyboard) used in
        // another program is not forwarded. The controller stays "connected" so the game does not show its reconnect screen.
        static int last_focus = -1;
        if (focused != last_focus) { rt_logf("pad: game window %s", focused ? "has focus: live input enabled" : "lost focus: input held neutral"); last_focus = focused; }
        if (!focused) { st.buttons = 0; st.l2 = st.r2 = 0; st.lx = st.ly = st.rx = st.ry = 128; }
    }
    memset(d, 0, sizeof *d);
    d->buttons = st.buttons; d->left[0] = st.lx; d->left[1] = st.ly; d->right[0] = st.rx; d->right[1] = st.ry; d->l2 = st.l2; d->r2 = st.r2;
    d->orientation[3] = 1.0f;
    d->timestamp = now_us();
    d->connected = source != OS_PAD_NONE; d->connectedCount = d->connected;
    // Log source changes and every change of the button set (bounded: only transitions).
    if (source != g_last_source) {
        rt_logf("pad: input source is now %s", scripted ? "SCRIPTED TEST INPUT" : source == OS_PAD_XINPUT ? "XInput controller" : source == OS_PAD_KEYBOARD ? "keyboard" : "none (connected=0)");
        g_last_source = source;
    }
    if (st.buttons != g_last_logged) {
        char nm[128]; names_of(st.buttons, nm, sizeof nm);
        rt_logf("pad: %s buttons -> %s (t=%.2fs)", scripted ? "SCRIPTED TEST INPUT" : "live input", nm, (double)(now_us() - g_open_us) / 1e6);
        g_last_logged = st.buttons;
    }
    return 0;
}
static int GUEST_ABI p_vibration(int handle, const void* param) {
    if (handle != 1) return (int)0x80920003;
    if (param) os_pad_rumble(((const uint8_t*)param)[0], ((const uint8_t*)param)[1]); // { largeMotor, smallMotor }
    return 0;
}
static int GUEST_ABI m_init(void) { return 0; }

const HostSymbol input_symbols[] = {
    {"scePadInit", "libScePad", (void*)p_init, 0},
    {"scePadOpen", "libScePad", (void*)p_open, 0},
    {"scePadClose", "libScePad", (void*)p_close, 0},
    {"scePadReadState", "libScePad", (void*)p_read, 0},
    {"scePadSetVibration", "libScePad", (void*)p_vibration, 0},
    {"sceMouseInit", "libSceMouse", (void*)m_init, 0},
};
const size_t input_symbol_count = sizeof input_symbols / sizeof *input_symbols;
