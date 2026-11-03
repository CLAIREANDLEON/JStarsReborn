// Platform services for an offline console: common dialogs, network, NP (PlayStation Network).
// Model, stated once: one local user, never signed in to PSN, no network interface connected, no system
// dialog can be displayed. Initialization calls succeed (the library is "loaded"); everything that would
// need a network or a signed-in account reports the console's own offline error. Nothing here pretends a
// connection, a sign-in or a button press happened. Dialog Open/GetResult and the NP request functions are
// intentionally NOT provided until the game reaches them, so the run stops there with the exact call site.
#define _GNU_SOURCE
#include "jstars_rt.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

// ---- opt-in tracing of the console's network identity libraries (--trace-np) ---------------------------------
// Off by default. When on, every call into libSceNpManager, libSceNpWebApi, libSceNpScore, libSceNpMatching2 and
// libSceNpSignaling is recorded with its arguments and its answer, bounded so a long run cannot fill the disk.
// First-call tracing already records that an import was reached once; this records what was asked and answered.
static int g_np_trace;
static uint64_t g_np_trace_lines;
#define NP_TRACE_MAX 20000
void rt_np_configure(int trace) { g_np_trace = trace; }
static void np_trace(const char* fmt, ...) {
    if (!g_np_trace || g_np_trace_lines >= NP_TRACE_MAX) return;
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_logf("np: %s", line);
    if (++g_np_trace_lines == NP_TRACE_MAX) rt_logf("np: trace limit of %d lines reached; further calls are not logged", NP_TRACE_MAX);
}

// ---- common dialogs: initialization and status only --------------------------------------------------------
enum { DLG_NONE = 0, DLG_INITIALIZED = 1, DLG_RUNNING = 2, DLG_FINISHED = 3 };
static int g_dlg[8];
enum { D_MSG, D_ERROR, D_SAVE, D_IME, D_INVITE, D_COMMERCE, D_PROFILE, D_COUNT };
#define DLG_FN(tag, idx) \
    static int GUEST_ABI tag##_init(void) { g_dlg[idx] = DLG_INITIALIZED; return 0; } \
    static int GUEST_ABI tag##_term(void) { g_dlg[idx] = DLG_NONE; return 0; } \
    static int GUEST_ABI tag##_status(void) { return g_dlg[idx]; }
DLG_FN(msg, D_MSG) DLG_FN(save, D_SAVE) DLG_FN(invite, D_INVITE) DLG_FN(commerce, D_COMMERCE) DLG_FN(profile, D_PROFILE)
// libSceErrorDialog (contract from upstream shadPS4 error_dialog.cpp). The console shows "An error has occurred" with the code and
// waits for OK. There is no system dialog on this host: Open logs the code and the dialog reports FINISHED, as if the player
// had pressed OK at once, so the game continues on its own error path (e.g. a sign-in error returns to the menu).
enum { CD_NOT_INITIALIZED = (int)0x80B80003, CD_ALREADY_INITIALIZED = (int)0x80B80004, CD_INVALID_STATE = (int)0x80B80006,
       CD_NOT_RUNNING = (int)0x80B8000B, CD_ARG_NULL = (int)0x80B8000D };
typedef struct { int32_t size; int32_t error_code; int32_t user_id; int32_t reserved; } ErrorDialogParam;
static int GUEST_ABI err_init(void) {
    if (g_dlg[D_ERROR] != DLG_NONE) return CD_ALREADY_INITIALIZED;
    g_dlg[D_ERROR] = DLG_INITIALIZED;
    return 0;
}
static int GUEST_ABI err_open(const ErrorDialogParam* p) {
    if (g_dlg[D_ERROR] != DLG_INITIALIZED && g_dlg[D_ERROR] != DLG_FINISHED) return CD_INVALID_STATE;
    if (!p) return CD_ARG_NULL;
    rt_logf("sceErrorDialogOpen: the game reports system error 0x%08X (user %d); no system dialog on this host, treated as "
            "acknowledged", (unsigned)p->error_code, p->user_id);
    g_dlg[D_ERROR] = DLG_FINISHED;
    return 0;
}
static int GUEST_ABI err_close(void) { return g_dlg[D_ERROR] == DLG_RUNNING ? (g_dlg[D_ERROR] = DLG_FINISHED, 0) : CD_NOT_RUNNING; }
static int GUEST_ABI err_term(void) {
    if (g_dlg[D_ERROR] == DLG_NONE) return CD_NOT_INITIALIZED;
    g_dlg[D_ERROR] = DLG_NONE;
    return 0;
}
static int GUEST_ABI err_status(void) { return g_dlg[D_ERROR]; }
static int GUEST_ABI cd_init(void) { return 0; }
// The game updates a save-data progress bar while it writes the save file, without the system dialog being open. No system
// dialog exists on this host, so the call reports the console's NOT_RUNNING error (value from upstream shadPS4 commondialog.h);
// the game ignores the result (checked in the disassembly at eboot.bin+0x27e0bf).
static int g_progress_calls;
static int GUEST_ABI save_progress_set(int target, uint32_t rate) {
    if (g_progress_calls++ < 3) rt_logf("sceSaveDataDialogProgressBarSetValue(target %d, rate %u): no system dialog is running on this host -> NOT_RUNNING (further calls are not logged)", target, rate);
    return (int)0x80B8000B;
}
static int GUEST_ABI save_ready(void) { return 1; }
static int GUEST_ABI ime_init(void* param, void* ext) { (void)param; (void)ext; g_dlg[D_IME] = DLG_INITIALIZED; return 0; }
static int GUEST_ABI ime_status(void) { return g_dlg[D_IME]; }
static int GUEST_ABI ime_term(void) { g_dlg[D_IME] = DLG_NONE; return 0; }

// ---- network ---------------------------------------------------------------------------------------------------
// Sockets and the network status library moved to jstars_net.c (real host UDP sockets behind --online; the default
// is still the offline answer this runtime has always given).

// ---- libSceSsl / libSceHttp: library handles only. Nothing here opens a TLS or HTTP connection; the game's online
// path does not need them for a match, and anything that would contact a service stays unimplemented.
static int g_ssl, g_http;
static int GUEST_ABI ssl_init(size_t pool) { (void)pool; return ++g_ssl; }
static int GUEST_ABI ssl_term(int id) { (void)id; return 0; }
static int GUEST_ABI http_init(int net, int ssl, size_t pool) { (void)net; (void)ssl; (void)pool; return ++g_http; }
static int GUEST_ABI http_term(int id) { (void)id; return 0; }

// ---- NP manager: the local user is signed out ------------------------------------------------------------------
enum { NP_SIGNED_OUT = 0x80550006, NP_USER_NOT_FOUND = 0x80550007, NP_INVALID_ARG = 0x80550003 };
enum { NP_STATE_NOT_SIGNED_UP = 0, NP_STATE_SIGNED_OUT = 1, NP_STATE_SIGNED_IN = 2 };
static int np_current_state(void);

// Local identity: the same name the console user service reports (JSTARS_USER_NAME, default "Player").
// SceNpOnlineId is 16 characters + terminator + 3 unused bytes; SceNpId adds opt[8] and reserved[8] (36 bytes).
static void local_online_id(char out[20]) {
    const char* n = getenv("JSTARS_USER_NAME");
    if (!n || !*n) n = "Player";
    memset(out, 0, 20);
    for (int i = 0; i < 16 && n[i]; ++i) out[i] = n[i];
}
static int GUEST_ABI np_get_online_id(int user, void* online_id) {
    if (!online_id) return NP_INVALID_ARG;
    if (user != 1) return NP_USER_NOT_FOUND;
    if (np_current_state() != NP_STATE_SIGNED_IN) { np_trace("sceNpGetOnlineId(user %d) -> SIGNED_OUT", user); return NP_SIGNED_OUT; }
    local_online_id((char*)online_id);
    np_trace("sceNpGetOnlineId(user %d) -> '%.16s'", user, (const char*)online_id);
    return 0;
}
static int GUEST_ABI np_get_np_id(int user, void* np_id) {
    if (!np_id) return NP_INVALID_ARG;
    if (user != 1) return NP_USER_NOT_FOUND;
    if (np_current_state() != NP_STATE_SIGNED_IN) return NP_SIGNED_OUT;
    memset(np_id, 0, 36);
    local_online_id((char*)np_id);
    return 0;
}

// The console delivers NP state changes from inside sceNpCheckCallback, on whichever game thread calls it, so the
// callback runs on a guest thread with the guest ABI and the runtime never invents a thread for it. The game
// registers its callback at eboot.bin+0x2bde4f, inside the routine that starts the online services.
typedef void (GUEST_ABI* NpStateCb)(int user_id, uint32_t state, void* np_id, void* userdata);
enum { NP_ERR_CALLBACK_ALREADY_REGISTERED = (int)0x80550008, NP_ERR_CALLBACK_NOT_REGISTERED = (int)0x80550009 };
static NpStateCb g_np_state_cb;
static void* g_np_state_userdata;
static int g_np_state_pending = -1;   // state queued for the next sceNpCheckCallback, -1 = nothing queued
static int g_np_state_delivered = -1;
static int g_np_cb_registered;

static int GUEST_ABI np_set_title_id(const void* id, const void* secret) {
    (void)secret;   // the title secret is never logged
    if (id) rt_logf("sceNpSetNpTitleId('%.12s')", (const char*)id);
    return 0;
}
static int GUEST_ABI np_set_restriction(const void* r) { (void)r; return 0; }

static int np_current_state(void) { return (rt_experiment("online-probe") || rt_net_online()) ? NP_STATE_SIGNED_IN : NP_STATE_SIGNED_OUT; }

static int GUEST_ABI np_register_state_cb(void* cb, void* user) {
    if (!cb) return NP_INVALID_ARG;
    if (g_np_state_cb) return NP_ERR_CALLBACK_ALREADY_REGISTERED;
    g_np_state_cb = (NpStateCb)cb;
    g_np_state_userdata = user;
    g_np_cb_registered = 1;
    g_np_state_pending = np_current_state();   // the console reports the current state shortly after registration
    rt_logf("sceNpRegisterStateCallback(callback %p, userdata %p): the state %s will be delivered from the game's next sceNpCheckCallback",
            cb, user, g_np_state_pending == NP_STATE_SIGNED_IN ? "SIGNED_IN" : "SIGNED_OUT");
    return 0;
}
static int GUEST_ABI np_unregister_state_cb(void) {
    if (!g_np_state_cb) return NP_ERR_CALLBACK_NOT_REGISTERED;
    g_np_state_cb = NULL;
    g_np_state_userdata = NULL;
    return 0;
}
// Called by the game on its own thread to let the console deliver queued notifications.
static int GUEST_ABI np_check_callback(void) {
    const int want = g_np_state_pending;
    if (want < 0 || !g_np_state_cb) return 0;
    np_trace("sceNpCheckCallback: a state change is pending");
    g_np_state_pending = -1;
    if (want == g_np_state_delivered) return 0;
    g_np_state_delivered = want;
    uint8_t np_id[36];
    memset(np_id, 0, sizeof np_id);
    local_online_id((char*)np_id);
    rt_logf("sceNpCheckCallback: delivering NP state %s to the game's callback %p", want == NP_STATE_SIGNED_IN ? "SIGNED_IN" : "SIGNED_OUT", (void*)g_np_state_cb);
    g_np_state_cb(1, (uint32_t)want, np_id, g_np_state_userdata);
    rt_logf("sceNpCheckCallback: the game's NP state callback returned");
    return 0;
}
// OrbisNpState: 0 not signed up, 1 signed out, 2 signed in. The default is signed out; the online-probe experiment
// reports signed in so one run can record what the game tries next. That answer is a probe, not an implementation.
static int GUEST_ABI np_get_state(int user, int* state) {
    if (!state) return NP_INVALID_ARG;
    if (user != 1) return NP_USER_NOT_FOUND;
    *state = np_current_state();
    np_trace("sceNpGetState(user %d) -> state %d", user, *state);
    return 0;
}



// ---- libSceNpManager requests ------------------------------------------------------------------------------
// The console hands out a request id, the game starts an operation on it and then polls. Evidence that this is
// the gate for online play: in the Windows probe run 20261101-200324 the game entered Online Battle, called
// sceNpGetOnlineId (eboot.bin+0x2beb7b) and then sceNpCreateAsyncRequest (eboot.bin+0x2c6bb8) three times, eight
// seconds apart, retrying after each failure. The helper that owns the request is eboot.bin+0x2c6b70, which calls
// CreateAsyncRequest, AbortRequest and PollAsync.
//
// There is no PlayStation Network here, so no request ever contacts a service: each one completes locally and
// reports what is true of this machine. Anything that would need an account or a store stays unimplemented.
#define NP_REQUEST_ID_OFFSET 0x20000000
#define NP_MAX_REQUESTS 32
enum { NP_ERR_INVALID_SIZE = (int)0x80550004, NP_ERR_REQUEST_MAX = (int)0x8055000A,
       NP_ERR_REQUEST_NOT_FOUND = (int)0x8055000B, NP_ERR_INVALID_ID = (int)0x80550005,
       NP_ERR_ABORTED = (int)0x8055000C };
static struct { int used, async, aborted, result; } g_np_req[NP_MAX_REQUESTS];
static pthread_mutex_t g_np_req_lock = PTHREAD_MUTEX_INITIALIZER;

static int np_request_new(int async) {
    pthread_mutex_lock(&g_np_req_lock);
    for (int i = 0; i < NP_MAX_REQUESTS; ++i)
        if (!g_np_req[i].used) {
            g_np_req[i].used = 1;
            g_np_req[i].async = async;
            g_np_req[i].aborted = 0;
            g_np_req[i].result = 0;
            pthread_mutex_unlock(&g_np_req_lock);
            return NP_REQUEST_ID_OFFSET + i + 1;
        }
    pthread_mutex_unlock(&g_np_req_lock);
    return NP_ERR_REQUEST_MAX;
}
static int np_request_index(int id) {
    const int i = id - NP_REQUEST_ID_OFFSET - 1;
    return (i >= 0 && i < NP_MAX_REQUESTS && g_np_req[i].used) ? i : -1;
}

static int GUEST_ABI np_create_request(void) { return np_request_new(0); }
static int GUEST_ABI np_create_async_request(const void* param) {
    if (!param) return NP_INVALID_ARG;
    const uint64_t size = *(const uint64_t*)param;   // { u64 size; u64 cpu_affinity; s32 priority; u8 pad[4]; }
    if (size != 24) return NP_ERR_INVALID_SIZE;
    const int id = np_request_new(1);
    if (id > 0) rt_logf("sceNpCreateAsyncRequest -> request %#x (completed locally; no network service is contacted)", id);
    return id;
}
static int GUEST_ABI np_delete_request(int id) {
    const int i = np_request_index(id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    pthread_mutex_lock(&g_np_req_lock);
    g_np_req[i].used = 0;
    pthread_mutex_unlock(&g_np_req_lock);
    return 0;
}
static int GUEST_ABI np_abort_request(int id) {
    const int i = np_request_index(id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    g_np_req[i].aborted = 1;
    g_np_req[i].result = NP_ERR_ABORTED;
    return 0;
}
// Every request here finishes before it is polled, so this reports the stored result rather than "in progress".
static int GUEST_ABI np_poll_async(int id, int* result) {
    if (!result) return NP_INVALID_ARG;
    const int i = np_request_index(id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    if (!g_np_req[i].async) return NP_ERR_INVALID_ID;
    *result = g_np_req[i].result;
    np_trace("sceNpPollAsync(request %#x) -> finished, result %#x", id, (unsigned)*result);
    return 0;
}
// "Can this user use the network?" The answer here is about this machine, not about any account.
static int GUEST_ABI np_check_availability(int id, const void* online_id) {
    if (!online_id) return NP_INVALID_ARG;
    const int i = np_request_index(id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    const int ok = np_current_state() == NP_STATE_SIGNED_IN;
    g_np_req[i].result = ok ? 0 : NP_SIGNED_OUT;
    rt_logf("sceNpCheckNpAvailability(request %#x) -> %s", id, ok ? "available" : "signed out");
    return 0;
}
static int GUEST_ABI np_check_availability_a(int id, int user_id) {
    const int i = np_request_index(id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    if (user_id != 1) return NP_USER_NOT_FOUND;
    g_np_req[i].result = np_current_state() == NP_STATE_SIGNED_IN ? 0 : NP_SIGNED_OUT;
    return 0;
}
// PlayStation Plus gates online play on a console. There is no subscription service here and nothing to check,
// so the local user is reported as authorized for the feature that was asked about, and the answer is logged.
static int GUEST_ABI np_check_plus(int id, const void* param, void* result) {
    if (!id || !param || !result) return NP_INVALID_ARG;
    const uint64_t size = *(const uint64_t*)param;   // { u64 size; s32 user_id; u8 pad[4]; u64 features; u8 reserved[32]; }
    if (size != 56) return NP_ERR_INVALID_SIZE;
    const int user_id = *(const int*)((const uint8_t*)param + 8);
    const uint64_t features = *(const uint64_t*)((const uint8_t*)param + 16);
    if (user_id != 1) return NP_INVALID_ARG;
    if (features < 1 || features > 3) return NP_INVALID_ARG;
    const int i = np_request_index(id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    memset(result, 0, 33);
    *(uint8_t*)result = 1;   // authorized
    g_np_req[i].result = 0;
    rt_logf("sceNpCheckPlus(request %#x, features %llu) -> authorized (there is no subscription service here; "
            "this is a local answer, not a PlayStation Plus entitlement)", id, (unsigned long long)features);
    return 0;
}
static int GUEST_ABI np_notify_plus_feature(const void* param) { np_trace("sceNpNotifyPlusFeature(param %p) -> 0", param); return 0; }
// These two take a REQUEST id and an online id, not a user id. Getting that wrong is what kept the game out of
// Online Battle until now: it passed request 0x20000001 where a user id was expected, the runtime answered
// USER_NOT_FOUND, and the game returned to the main menu (probe runs up to 20261101-203850).
//   sceNpGetAccountCountry(const SceNpOnlineId*, SceNpCountryCode*)      country code is 2 characters + terminator
//   sceNpGetAccountLanguage(s32 reqId, const SceNpOnlineId*, SceNpLanguageCode*)   language code is a string, e.g. "en"
static int GUEST_ABI np_get_account_country(const void* online_id, void* country) {
    if (!online_id || !country) return NP_INVALID_ARG;
    if (np_current_state() != NP_STATE_SIGNED_IN) return NP_SIGNED_OUT;
    const char* c = getenv("JSTARS_COUNTRY");
    memset(country, 0, 4);
    memcpy(country, (c && strlen(c) == 2) ? c : "US", 2);
    np_trace("sceNpGetAccountCountry('%.16s') -> '%s'", (const char*)online_id, (const char*)country);
    return 0;
}
// SceNpLanguageCode is a 6-byte string plus padding, not an integer.
static int GUEST_ABI np_get_account_language(int req_id, const void* online_id, void* language) {
    if (!online_id || !language) return NP_INVALID_ARG;
    const int i = np_request_index(req_id);
    if (i < 0) return NP_ERR_REQUEST_NOT_FOUND;
    if (np_current_state() != NP_STATE_SIGNED_IN) { g_np_req[i].result = NP_SIGNED_OUT; return 0; }
    const char* l = getenv("JSTARS_LANG_CODE");
    memset(language, 0, 16);
    const char* code = (l && *l) ? l : "en";
    for (int k = 0; k < 5 && code[k]; ++k) ((char*)language)[k] = code[k];
    g_np_req[i].result = 0;
    np_trace("sceNpGetAccountLanguage(request %#x, '%.16s') -> '%s'", req_id, (const char*)online_id, (const char*)language);
    return 0;
}

// ---- libSceNpCommon ------------------------------------------------------------------------------------------
// SceNpId is the 20-byte online id followed by opt[8] and reserved[8]; only the online id identifies a player.
static int GUEST_ABI np_cmp_np_id(const void* a, const void* b) {
    if (!a || !b) return NP_INVALID_ARG;
    const int r = memcmp(a, b, 16) == 0 ? 0 : 1;
    np_trace("sceNpCmpNpId('%.16s', '%.16s') -> %d", (const char*)a, (const char*)b, r);
    return r;
}

// ---- libSceSharePlay: this machine never has a Share Play session, so the connection is permanently DORMANT -------------
// OrbisSharePlayConnectionInfo: status@0 (0 dormant / 1 ready / 2 connected), mode@4, host/visitor online ids (20 bytes each), user ids.
static int g_shareplay_init, g_shareplay_prohibition = -1;
static int GUEST_ABI sp_initialize(void) { g_shareplay_init = 1; return 0; }
static int GUEST_ABI sp_terminate(void) { g_shareplay_init = 0; return 0; }
static int GUEST_ABI sp_set_prohibition(int mode) { g_shareplay_prohibition = mode; rt_logf("sceSharePlaySetProhibition(%d): recorded; no Share Play session can exist here", mode); return 0; }
static int GUEST_ABI sp_connection_info(void* info) { if (!info) return (int)0x80B80001; memset(info, 0, 56); return 0; }  // status 0 = DORMANT

// ---- libSceScreenShot: only the enable flag exists; this host has no console screenshot service ------------------------
static int g_screenshot_enabled = 1;
static int GUEST_ABI ss_enable(void) { g_screenshot_enabled = 1; return 0; }
static int GUEST_ABI ss_disable(void) { g_screenshot_enabled = 0; return 0; }
static int GUEST_ABI ss_overlay(const char* path, int32_t x, int32_t y) { rt_logf("sceScreenShotSetOverlayImage('%s', %d, %d): remembered, not drawn (no screenshot service)", path ? path : "(null)", x, y); return 0; }

// ---- libSceNpMatching2: contexts ------------------------------------------------------------------------------
// Matching2 is the console's room system: a context is a session with a matching service, and every room
// operation is asynchronous, answered through callbacks the game registers.
//
// Evidence for this step (probe run 20261101-204539, with the sign-in path complete): the game reaches
// sceNpMatching2CreateContext (return address eboot.bin+0x2bfce7, inside function 0x2bfc40) and, when it fails, shows "A communication error has
// occurred." over the main menu. That capture is in the run's own captures folder.
//
// What exists here is the context lifetime and the callback machinery. There is no room directory yet, so the
// context starts and reports itself started, and nothing can be searched or joined; those entries are still
// unimplemented and stop the run (or, with --experiment online-probe, report an error).
enum {
    M2_EVENT_CONTEXT_STARTED = 0x6F02,
    M2_EVENT_CONTEXT_STOPPED = 0x6F03,
    M2_CAUSE_CONTEXT_ACTION = 11,
};
// Error values: shadPS4 src/core/libraries/np/np_error.h (a reference implementation, not the console itself).
enum { M2_ERR_OUT_OF_MEMORY = (int)0x80550C01, M2_ERR_NOT_INITIALIZED = (int)0x80550C03,
       M2_ERR_CONTEXT_MAX = (int)0x80550C04, M2_ERR_CONTEXT_ALREADY_STARTED = (int)0x80550C07,
       M2_ERR_CONTEXT_NOT_STARTED = (int)0x80550C08, M2_ERR_INVALID_ARGUMENT = (int)0x80550C0A,
       M2_ERR_INVALID_CONTEXT_ID = (int)0x80550C0B, M2_ERR_INVALID_SERVER_ID = (int)0x80550C0C,
       M2_ERR_ABORTED = (int)0x80550C2A };

typedef void (GUEST_ABI* M2ContextCb)(uint16_t ctx_id, uint16_t event, uint8_t cause, int error_code, void* userdata);
typedef void (GUEST_ABI* M2RoomEventCb)(uint16_t ctx_id, uint64_t room_id, uint16_t event, const void* data, void* userdata);

#define M2_MAX_CTX 4
#define M2_SERVER_ID 1
#define M2_START_DELAY_MS 30
static int g_np_m2_init, g_np_sig_init;
// One lock for the whole table. The game's callbacks are always called with it released, so a callback may call
// back into this library (destroy the context, start it again) without deadlocking.
static pthread_mutex_t g_m2_lock = PTHREAD_MUTEX_INITIALIZER;
static M2ContextCb g_m2_context_cb;
static void* g_m2_context_userdata;
static struct {
    int used, started, starting, aborted;
    uint32_t generation;       // bumped whenever the slot is created or destroyed, so a late start thread can tell
    char online_id[20];
    uint32_t service_label;
    M2RoomEventCb room_cb;
    void* room_userdata;
    void* request_cb;          // M2RequestCb from the last request that carried an option parameter with a callback
    void* request_arg;
} g_m2_ctx[M2_MAX_CTX];
static uint32_t g_m2_generation;
// The room this instance is in (one at a time, as the game uses it). Guarded by g_m2_lock.
static struct { int in_room; uint16_t ctx_id; RoomInfo info; uint16_t team_id; uint64_t lobby_id; } g_m2_room;

// Callers hold g_m2_lock.
static int m2_ctx_index(uint16_t id) { const int i = (int)id - 1; return (i >= 0 && i < M2_MAX_CTX && g_m2_ctx[i].used) ? i : -1; }

static int GUEST_ABI m2_initialize(const void* param) { (void)param; g_np_m2_init = 1; np_trace("sceNpMatching2Initialize"); return 0; }
static int GUEST_ABI m2_terminate(void) {
    pthread_mutex_lock(&g_m2_lock);
    g_np_m2_init = 0;
    for (int i = 0; i < M2_MAX_CTX; ++i) { const uint32_t gen = ++g_m2_generation; memset(&g_m2_ctx[i], 0, sizeof g_m2_ctx[i]); g_m2_ctx[i].generation = gen; }
    g_m2_context_cb = NULL;
    g_m2_context_userdata = NULL;
    const uint64_t left = g_m2_room.in_room ? g_m2_room.info.room_id : 0;
    g_m2_room.in_room = 0;
    pthread_mutex_unlock(&g_m2_lock);
    if (left) rt_rooms_unpublish(left);
    np_trace("sceNpMatching2Terminate");
    return 0;
}

// SceNpMatching2CreateContextParameter is 0x28 bytes: { SceNpId* npId; void* communicationId; void* passphrase;
// SceNpServiceLabel serviceLabel; (4 bytes of padding) size_t size; } (layout as in shadPS4's np_types2.h, which
// asserts the size). The passphrase is a title secret and is never read or logged.
static int GUEST_ABI m2_create_context(const void* param, uint16_t* ctx_id) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    if (!param || !ctx_id) return M2_ERR_INVALID_ARGUMENT;
    const uint8_t* p = (const uint8_t*)param;
    const void* np_id = *(void* const*)(p + 0);
    uint32_t service_label;
    memcpy(&service_label, p + 24, 4);
    pthread_mutex_lock(&g_m2_lock);
    for (int i = 0; i < M2_MAX_CTX; ++i) {
        if (g_m2_ctx[i].used) continue;
        memset(&g_m2_ctx[i], 0, sizeof g_m2_ctx[i]);
        g_m2_ctx[i].used = 1;
        g_m2_ctx[i].generation = ++g_m2_generation;
        g_m2_ctx[i].service_label = service_label;
        if (np_id) memcpy(g_m2_ctx[i].online_id, np_id, 16);
        else local_online_id(g_m2_ctx[i].online_id);
        *ctx_id = (uint16_t)(i + 1);
        char who[20]; memcpy(who, g_m2_ctx[i].online_id, sizeof who);
        pthread_mutex_unlock(&g_m2_lock);
        rt_logf("sceNpMatching2CreateContext(player '%.16s', service label %u) -> context %u", who, service_label, *ctx_id);
        return 0;
    }
    pthread_mutex_unlock(&g_m2_lock);
    rt_logf("sceNpMatching2CreateContext: all %d contexts in use -> CONTEXT_MAX", M2_MAX_CTX);
    return M2_ERR_CONTEXT_MAX;
}
static int GUEST_ABI m2_destroy_context(uint16_t ctx_id) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i < 0) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_CONTEXT_ID; }
    const uint32_t gen = ++g_m2_generation;     // a start still pending for this slot finds a new generation and gives up
    memset(&g_m2_ctx[i], 0, sizeof g_m2_ctx[i]);
    g_m2_ctx[i].generation = gen;
    const uint64_t left = g_m2_room.in_room && g_m2_room.ctx_id == ctx_id ? g_m2_room.info.room_id : 0;
    if (left) g_m2_room.in_room = 0;
    pthread_mutex_unlock(&g_m2_lock);
    if (left) rt_rooms_unpublish(left);
    np_trace("sceNpMatching2DestroyContext(context %u)", ctx_id);
    return 0;
}
static int GUEST_ABI m2_register_context_callback(void* cb, void* userdata) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_m2_lock);
    g_m2_context_cb = (M2ContextCb)cb;
    g_m2_context_userdata = userdata;
    pthread_mutex_unlock(&g_m2_lock);
    rt_logf("sceNpMatching2RegisterContextCallback(callback %p, userdata %p)", cb, userdata);
    return 0;
}
static int GUEST_ABI m2_register_room_event_callback(uint16_t ctx_id, void* cb, void* userdata) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i >= 0) { g_m2_ctx[i].room_cb = (M2RoomEventCb)cb; g_m2_ctx[i].room_userdata = userdata; }
    pthread_mutex_unlock(&g_m2_lock);
    if (i < 0) return M2_ERR_INVALID_CONTEXT_ID;
    np_trace("sceNpMatching2RegisterRoomEventCallback(context %u, callback %p)", ctx_id, cb);
    return 0;
}

// The console starts a context asynchronously and reports the result through the context callback, so the
// callback runs on a thread of this library's own rather than inside the call the game made. The delay is ours
// (nothing is contacted); it only keeps the report from arriving before ContextStart has returned.
static void* GUEST_ABI m2_start_thread(void* arg) {
    const uintptr_t a = (uintptr_t)arg;
    const uint16_t ctx_id = (uint16_t)(a & 0xFFFF);
    const uint32_t gen = (uint32_t)(a >> 16);
    struct timespec ts = {0, M2_START_DELAY_MS * 1000 * 1000};
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i < 0 || g_m2_ctx[i].generation != gen || !g_m2_ctx[i].starting) {
        pthread_mutex_unlock(&g_m2_lock);
        rt_logf("sceNpMatching2ContextStart(context %u): the context was destroyed before the start completed; nothing reported", ctx_id);
        return NULL;
    }
    g_m2_ctx[i].starting = 0;
    const int aborted = g_m2_ctx[i].aborted;
    g_m2_ctx[i].aborted = 0;
    if (!aborted) g_m2_ctx[i].started = 1;
    const M2ContextCb cb = g_m2_context_cb;
    void* const ud = g_m2_context_userdata;
    pthread_mutex_unlock(&g_m2_lock);
    if (aborted) {
        rt_logf("sceNpMatching2ContextStart(context %u): aborted by the game; reporting STOPPED/ABORTED to callback %p", ctx_id, (void*)cb);
        if (cb) cb(ctx_id, M2_EVENT_CONTEXT_STOPPED, M2_CAUSE_CONTEXT_ACTION, M2_ERR_ABORTED, ud);
        return NULL;
    }
    rt_logf("sceNpMatching2ContextStart(context %u): reporting the context as started to the game's callback %p", ctx_id, (void*)cb);
    if (cb) cb(ctx_id, M2_EVENT_CONTEXT_STARTED, M2_CAUSE_CONTEXT_ACTION, 0, ud);
    rt_logf("sceNpMatching2ContextStart(context %u): the game's context callback returned", ctx_id);
    return NULL;
}
static int GUEST_ABI m2_context_start(uint16_t ctx_id, uint64_t timeout) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i < 0) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_CONTEXT_ID; }
    if (g_m2_ctx[i].started || g_m2_ctx[i].starting) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_CONTEXT_ALREADY_STARTED; }
    g_m2_ctx[i].starting = 1;
    g_m2_ctx[i].aborted = 0;
    const uint32_t gen = g_m2_ctx[i].generation;
    pthread_mutex_unlock(&g_m2_lock);
    np_trace("sceNpMatching2ContextStart(context %u, timeout %llu us)", ctx_id, (unsigned long long)timeout);
    rt_rooms_start();
    if (rt_spawn_guest_thread(m2_start_thread, (void*)(((uintptr_t)gen << 16) | ctx_id), "NpMatching2")) {
        pthread_mutex_lock(&g_m2_lock);
        if (g_m2_ctx[i].generation == gen) g_m2_ctx[i].starting = 0;
        pthread_mutex_unlock(&g_m2_lock);
        return M2_ERR_OUT_OF_MEMORY;
    }
    return 0;
}
// The game's import takes the context id; anything after it is ignored.
static int GUEST_ABI m2_abort_context_start(uint16_t ctx_id) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    int rc = 0;
    if (i < 0) rc = M2_ERR_INVALID_CONTEXT_ID;
    else if (!g_m2_ctx[i].starting) rc = M2_ERR_CONTEXT_NOT_STARTED;
    else g_m2_ctx[i].aborted = 1;
    pthread_mutex_unlock(&g_m2_lock);
    np_trace("sceNpMatching2AbortContextStart(context %u) -> 0x%08x", ctx_id, (unsigned)rc);
    return rc;
}
static int GUEST_ABI m2_context_stop(uint16_t ctx_id) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i < 0) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_CONTEXT_ID; }
    if (!g_m2_ctx[i].started) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_CONTEXT_NOT_STARTED; }
    g_m2_ctx[i].started = 0;
    const M2ContextCb cb = g_m2_context_cb;
    void* const ud = g_m2_context_userdata;
    pthread_mutex_unlock(&g_m2_lock);
    np_trace("sceNpMatching2ContextStop(context %u)", ctx_id);
    if (cb) cb(ctx_id, M2_EVENT_CONTEXT_STOPPED, M2_CAUSE_CONTEXT_ACTION, 0, ud);
    return 0;
}
// ---- Matching2 requests ----------------------------------------------------------------------------------------
// Every room operation is a request: the call returns a request id at once and the answer arrives later through a
// request callback, which the game passes in SceNpMatching2RequestOptParam { callback; arg; uint32 timeout; uint16 appId }
// (24 bytes). Verified on the game's own call to GetWorldInfoList (function 0x2c03e0): the option parameter is built on
// the stack with the callback at +0 and a zero arg, the request id is written to a uint32. The game does not import
// SetDefaultRequestOptParam, so, as in shadPS4, the last callback given is kept on the context for requests without one.
typedef void (GUEST_ABI* M2RequestCb)(uint16_t ctx_id, uint32_t req_id, uint16_t event, int error_code, const void* data, void* arg);
enum { M2_REQUEST_EVENT_GET_WORLD_INFO_LIST = 0x0002 };
#define M2_LOCAL_WORLD_ID 1
static uint32_t g_m2_next_request = 1;

typedef struct {
    uint16_t ctx_id;
    uint32_t generation;
    uint32_t req_id;
    uint16_t event;
    int error;
    void* data;                // malloc'd answer, owned by the job, freed after the callback returns
} M2Job;

// Called with g_m2_lock held.
static void m2_store_request_cb(int i, const void* opt) {
    if (!opt) return;
    void* cb;
    void* arg;
    memcpy(&cb, (const uint8_t*)opt + 0, sizeof cb);
    memcpy(&arg, (const uint8_t*)opt + 8, sizeof arg);
    if (!cb) return;
    g_m2_ctx[i].request_cb = cb;
    g_m2_ctx[i].request_arg = arg;
}

static void* GUEST_ABI m2_request_thread(void* p) {
    M2Job* job = (M2Job*)p;
    struct timespec ts = {0, M2_START_DELAY_MS * 1000 * 1000};
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(job->ctx_id);
    M2RequestCb cb = NULL;
    void* arg = NULL;
    if (i >= 0 && g_m2_ctx[i].generation == job->generation) { cb = (M2RequestCb)g_m2_ctx[i].request_cb; arg = g_m2_ctx[i].request_arg; }
    pthread_mutex_unlock(&g_m2_lock);
    if (i < 0 || !cb) {
        rt_logf("Matching2 request %u (event 0x%04x): %s; nothing reported", job->req_id, job->event,
                i < 0 ? "the context is gone" : "no request callback was ever given");
    } else {
        np_trace("Matching2 request %u: reporting event 0x%04x, error 0x%08x to callback %p", job->req_id, job->event, (unsigned)job->error, (void*)cb);
        cb(job->ctx_id, job->req_id, job->event, job->error, job->data, arg);
    }
    free(job->data);
    free(job);
    return NULL;
}

// Hands the answer to a library thread; the caller has already validated the context (index i, lock held) and
// filled `data`. Returns the request id, or 0 if no thread could be started (the data is freed).
static uint32_t m2_post_request_locked(int i, uint16_t ctx_id, uint16_t event, int error, void* data) {
    M2Job* job = (M2Job*)calloc(1, sizeof *job);
    if (!job) { free(data); return 0; }
    job->ctx_id = ctx_id;
    job->generation = g_m2_ctx[i].generation;
    job->req_id = g_m2_next_request++;
    if (!g_m2_next_request) g_m2_next_request = 1;
    job->event = event;
    job->error = error;
    job->data = data;
    if (rt_spawn_guest_thread(m2_request_thread, job, "NpMatching2Req")) { free(job->data); free(job); return 0; }
    return job->req_id;
}

// SceNpMatching2World as the game reads it: a contiguous array of 0x28-byte entries (the game's callback at 0x2c04c0
// copies worldNum * 0x28 bytes). Layout as in shadPS4's np_types2.h: { World* next; uint32 worldId; uint32 lobbiesNum;
// uint32 maxLobbyMembersNum; uint32 lobbyMembersNum; uint32 roomsNum; uint32 roomMembersNum; pad }.
typedef struct {
    void* next;
    uint32_t world_id, lobbies, max_lobby_members, lobby_members, rooms, room_members;
    uint8_t pad[8];
} M2World;
_Static_assert(sizeof(M2World) == 0x28, "SceNpMatching2World is 0x28 bytes");
typedef struct { M2World* world; uint64_t world_num; M2World one; } M2WorldList;

// The local matching service has one world. Its counts are the real counts of the local room directory, which does not
// exist yet, so they are all zero: nobody is reported who is not there.
static int GUEST_ABI m2_get_world_info_list(uint16_t ctx_id, const void* request, const void* opt, uint32_t* req_id) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    if (!request || !req_id) return M2_ERR_INVALID_ARGUMENT;
    uint16_t server_id;
    memcpy(&server_id, request, 2);
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i < 0) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_CONTEXT_ID; }
    if (server_id != M2_SERVER_ID) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_SERVER_ID; }
    m2_store_request_cb(i, opt);
    M2WorldList* list = (M2WorldList*)calloc(1, sizeof *list);
    if (!list) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_OUT_OF_MEMORY; }
    list->world = &list->one;
    list->world_num = 1;
    list->one.world_id = M2_LOCAL_WORLD_ID;
    { RoomInfo seen[64]; list->one.rooms = (uint32_t)rt_rooms_snapshot(seen, 64, rt_rooms_now_ms()); }
    const uint32_t list_rooms = list->one.rooms;
    const uint32_t id = m2_post_request_locked(i, ctx_id, M2_REQUEST_EVENT_GET_WORLD_INFO_LIST, 0, list);
    pthread_mutex_unlock(&g_m2_lock);
    if (!id) return M2_ERR_OUT_OF_MEMORY;
    *req_id = id;
    rt_logf("sceNpMatching2GetWorldInfoList(context %u, server %u) -> request %u: one local world (id %u), %u room(s) heard on the network", ctx_id, server_id, id, M2_LOCAL_WORLD_ID, list_rooms);
    return 0;
}

// ---- Matching2 rooms over the LAN directory (jstars_rooms.c) --------------------------------------------------------
// Layouts below are shadPS4's np_types2.h ones, confirmed against what this game reads: its CreateJoinRoom callback
// (0x2c1240) copies a 0x58-byte room and 0x58-byte members from { room; members; membersNum; me; owner }, and its
// SearchRoom callback (0x2c22c0) walks the `next` chain of 0x88-byte external room entries, copying the owner NpId
// (0x24 bytes) and the int (8-byte) and bin (0x18-byte) search attributes. The request offsets are those of the same
// header; the game's CreateJoinRoom call (0x2c0fd9) puts its blocked-user list at +0x50/+0x58 as that layout says.
enum { M2_REQUEST_EVENT_SET_ROOM_DATA_EXTERNAL = 0x0004, M2_REQUEST_EVENT_CREATE_JOIN_ROOM = 0x0101,
       M2_REQUEST_EVENT_LEAVE_ROOM = 0x0103, M2_REQUEST_EVENT_SEARCH_ROOM = 0x0106 };
enum { M2_ERR_ALREADY_JOINED = (int)0x80550C24, M2_ERR_INVALID_ROOM_ID = (int)0x80550C0F, M2_ERR_ROOM_NOT_FOUND = (int)0x80550C30 };
#define M2_MEMBER_FLAG_OWNER 0x80000000u
#define M2_ROOM_FLAG_FULL 0x20000000u
#define M2_ROOM_FLAG_HIDDEN 0x10000000u

typedef struct {
    void* next; uint64_t join_date; uint8_t np_id[36]; uint8_t pad[4];
    uint16_t member_id; uint8_t team_id; uint8_t nat_type; uint32_t flag_attr;
    void* room_group; void* bin_attr; uint64_t bin_attr_num;
} M2Member;
_Static_assert(sizeof(M2Member) == 0x58, "SceNpMatching2RoomMemberDataInternal is 0x58 bytes");
typedef struct {
    uint16_t public_slots, private_slots, open_public_slots, open_private_slots, max_slot, server_id;
    uint32_t world_id; uint64_t lobby_id, room_id, passwd_slot_mask, joined_slot_mask;
    void* room_group; uint64_t room_groups; uint32_t flags; uint8_t pad[4]; void* internal_bin_attr; uint64_t internal_bin_attrs;
} M2RoomInternal;
_Static_assert(sizeof(M2RoomInternal) == 0x58, "SceNpMatching2RoomDataInternal is 0x58 bytes");
typedef struct {
    void* next; uint16_t max_slot, cur_members; uint32_t flags; uint16_t server_id; uint8_t pad[2]; uint32_t world_id;
    uint64_t lobby_id, room_id, passwd_slot_mask, joined_slot_mask;
    uint16_t public_slots, private_slots, open_public_slots, open_private_slots;
    void* owner_np_id; void* room_group; uint64_t room_groups;
    void* int_attr; uint64_t int_attrs; void* search_bin_attr; uint64_t search_bin_attrs; void* bin_attr; uint64_t bin_attrs;
} M2RoomExternal;
_Static_assert(sizeof(M2RoomExternal) == 0x88, "SceNpMatching2RoomDataExternal is 0x88 bytes");
typedef struct { uint16_t id; uint8_t pad[2]; uint32_t num; } M2IntAttr;
typedef struct { uint16_t id; uint8_t pad[6]; const uint8_t* data; uint64_t size; } M2BinAttr;
_Static_assert(sizeof(M2BinAttr) == 0x18, "SceNpMatching2BinAttr is 0x18 bytes");


static uint64_t m2_rtc_now(void) {   // SceRtcTick: microseconds since 0001-01-01
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 62135596800ull) * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
}
static void m2_np_id(uint8_t out[36], const char* online_id) {
    memset(out, 0, 36);
    size_t n = strnlen(online_id, 16);
    memcpy(out, online_id, n);
}
static void m2_copy_attrs(RoomInfo* r, const M2IntAttr* ints, uint64_t n_int, const M2BinAttr* bins, uint64_t n_bin) {
    if (ints) {
        r->n_int = 0;
        for (uint64_t i = 0; i < n_int && r->n_int < ROOM_MAX_INT; ++i) { r->ints[r->n_int].id = ints[i].id; r->ints[r->n_int].value = ints[i].num; r->n_int++; }
        if (n_int > ROOM_MAX_INT) rt_logf("Matching2: %llu int search attributes given, %d kept", (unsigned long long)n_int, ROOM_MAX_INT);
    }
    if (bins) {
        r->n_bin = 0;
        for (uint64_t i = 0; i < n_bin && r->n_bin < ROOM_MAX_BIN; ++i) {
            const uint64_t len = bins[i].size > ROOM_MAX_BIN_LEN ? ROOM_MAX_BIN_LEN : bins[i].size;
            if (bins[i].size > ROOM_MAX_BIN_LEN) rt_logf("Matching2: bin search attribute %u is %llu bytes; %d kept", bins[i].id, (unsigned long long)bins[i].size, ROOM_MAX_BIN_LEN);
            r->bins[r->n_bin].id = bins[i].id; r->bins[r->n_bin].len = (uint16_t)len;
            if (len && bins[i].data) memcpy(r->bins[r->n_bin].data, bins[i].data, len);
            r->n_bin++;
        }
    }
}

// Common checks for a room request; on success the lock is HELD and *idx is the context's index.
static int m2_begin_request(uint16_t ctx_id, const void* request, const void* opt, const uint32_t* req_id, int* idx) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    if (!request || !req_id) return M2_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    if (i < 0) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_CONTEXT_ID; }
    if (!g_m2_ctx[i].started) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_CONTEXT_NOT_STARTED; }
    m2_store_request_cb(i, opt);
    *idx = i;
    return 0;
}

// CreateJoinRoom: this instance becomes the owner and only member of a new room, which the directory announces on the
// local network. Nobody else is reported in it until somebody really joins.
static int GUEST_ABI m2_create_join_room(uint16_t ctx_id, const void* request, const void* opt, uint32_t* req_id) {
    int i, rc = m2_begin_request(ctx_id, request, opt, req_id, &i);
    if (rc) return rc;
    const uint8_t* q = (const uint8_t*)request;
    uint16_t max_slot; uint32_t flags, world_id; uint64_t lobby_id;
    memcpy(&max_slot, q + 0x00, 2); memcpy(&flags, q + 0x08, 4); memcpy(&world_id, q + 0x0C, 4); memcpy(&lobby_id, q + 0x10, 8);
    const uint8_t team_id = q[2];
    if (g_m2_room.in_room) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_ALREADY_JOINED; }
    if (max_slot == 0 || max_slot > 64) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_INVALID_ARGUMENT; }
    RoomInfo r; memset(&r, 0, sizeof r);
    r.room_id = rt_rooms_new_id(); r.world_id = world_id; r.max_slot = max_slot; r.cur_members = 1; r.flags = flags;
    memcpy(r.owner, g_m2_ctx[i].online_id, 16);
    const M2IntAttr* ints; uint64_t n_int; const M2BinAttr* bins; uint64_t n_bin;
    memcpy(&ints, q + 0x70, 8); memcpy(&n_int, q + 0x78, 8); memcpy(&bins, q + 0x80, 8); memcpy(&n_bin, q + 0x88, 8);
    m2_copy_attrs(&r, ints, n_int, bins, n_bin);

    struct Answer { M2RoomInternal* room; M2Member* members; uint64_t members_num; M2Member* me; M2Member* owner; M2RoomInternal room_data; M2Member member; };
    struct Answer* a = (struct Answer*)calloc(1, sizeof *a);
    if (!a) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_OUT_OF_MEMORY; }
    a->room = &a->room_data; a->members = a->me = a->owner = &a->member; a->members_num = 1;
    a->room_data.public_slots = max_slot; a->room_data.open_public_slots = (uint16_t)(max_slot - 1); a->room_data.max_slot = max_slot;
    a->room_data.server_id = M2_SERVER_ID; a->room_data.world_id = world_id; a->room_data.lobby_id = lobby_id; a->room_data.room_id = r.room_id;
    a->room_data.joined_slot_mask = 1; a->room_data.flags = flags;
    a->member.join_date = m2_rtc_now(); m2_np_id(a->member.np_id, g_m2_ctx[i].online_id);
    a->member.member_id = 1; a->member.team_id = team_id; a->member.nat_type = 2; a->member.flag_attr = M2_MEMBER_FLAG_OWNER;

    g_m2_room.in_room = 1; g_m2_room.ctx_id = ctx_id; g_m2_room.info = r; g_m2_room.team_id = team_id; g_m2_room.lobby_id = lobby_id;
    const uint32_t id = m2_post_request_locked(i, ctx_id, M2_REQUEST_EVENT_CREATE_JOIN_ROOM, 0, a);
    if (!id) { g_m2_room.in_room = 0; pthread_mutex_unlock(&g_m2_lock); return M2_ERR_OUT_OF_MEMORY; }
    pthread_mutex_unlock(&g_m2_lock);
    *req_id = id;
    rt_rooms_start();
    rt_rooms_publish(&r);
    rt_logf("sceNpMatching2CreateJoinRoom(context %u, world %u, %u slots, flags 0x%08x, %u int / %u bin search attributes) -> request %u: "
            "room %016llx owned by this instance, announced on the local network",
            ctx_id, world_id, max_slot, flags, r.n_int, r.n_bin, id, (unsigned long long)r.room_id);
    return 0;
}

static int GUEST_ABI m2_leave_room(uint16_t ctx_id, const void* request, const void* opt, uint32_t* req_id) {
    int i, rc = m2_begin_request(ctx_id, request, opt, req_id, &i);
    if (rc) return rc;
    uint64_t room_id; memcpy(&room_id, request, 8);
    if (!g_m2_room.in_room || g_m2_room.info.room_id != room_id) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_ROOM_NOT_FOUND; }
    g_m2_room.in_room = 0;
    uint64_t* answer = (uint64_t*)malloc(sizeof *answer);
    if (answer) *answer = room_id;
    const uint32_t id = answer ? m2_post_request_locked(i, ctx_id, M2_REQUEST_EVENT_LEAVE_ROOM, 0, answer) : 0;
    pthread_mutex_unlock(&g_m2_lock);
    rt_rooms_unpublish(room_id);
    if (!id) return M2_ERR_OUT_OF_MEMORY;
    *req_id = id;
    rt_logf("sceNpMatching2LeaveRoom(context %u, room %016llx) -> request %u: the room is no longer announced", ctx_id, (unsigned long long)room_id, id);
    return 0;
}

static int GUEST_ABI m2_set_room_data_external(uint16_t ctx_id, const void* request, const void* opt, uint32_t* req_id) {
    int i, rc = m2_begin_request(ctx_id, request, opt, req_id, &i);
    if (rc) return rc;
    const uint8_t* q = (const uint8_t*)request;
    uint64_t room_id; const M2IntAttr* ints; uint64_t n_int; const M2BinAttr* bins; uint64_t n_bin;
    memcpy(&room_id, q, 8); memcpy(&ints, q + 8, 8); memcpy(&n_int, q + 16, 8); memcpy(&bins, q + 24, 8); memcpy(&n_bin, q + 32, 8);
    if (!g_m2_room.in_room || g_m2_room.info.room_id != room_id) { pthread_mutex_unlock(&g_m2_lock); return M2_ERR_ROOM_NOT_FOUND; }
    m2_copy_attrs(&g_m2_room.info, ints, n_int, bins, n_bin);
    const RoomInfo r = g_m2_room.info;
    const uint32_t id = m2_post_request_locked(i, ctx_id, M2_REQUEST_EVENT_SET_ROOM_DATA_EXTERNAL, 0, NULL);
    pthread_mutex_unlock(&g_m2_lock);
    if (!id) return M2_ERR_OUT_OF_MEMORY;
    rt_rooms_publish(&r);
    *req_id = id;
    rt_logf("sceNpMatching2SetRoomDataExternal(context %u, room %016llx, %u int / %u bin search attributes) -> request %u",
            ctx_id, (unsigned long long)room_id, r.n_int, r.n_bin, id);
    return 0;
}

// Search operators: SCE_NP_MATCHING2_OPERATOR_EQ..GE = 1..6 (the PS3 values, as in RPCS3's sceNp2.h; shadPS4 does not
// name them). An unknown operator does not exclude a room, and is logged.
static int m2_int_match(uint8_t op, uint32_t have, uint32_t want) {
    switch (op) {
    case 1: return have == want; case 2: return have != want; case 3: return have < want;
    case 4: return have <= want; case 5: return have > want; case 6: return have >= want;
    default: rt_logf("Matching2 SearchRoom: unknown operator %u; not used to exclude rooms", op); return 1;
    }
}
static int m2_room_matches(const RoomInfo* r, uint32_t world_id, uint32_t flag_filter, uint32_t flag_attr, const uint8_t* filters, uint64_t n_filters) {
    if (world_id && r->world_id != world_id) return 0;
    if ((r->flags & flag_filter) != (flag_attr & flag_filter)) return 0;
    for (uint64_t f = 0; f < n_filters; ++f) {
        const uint8_t* e = filters + f * 16;   // { u8 operator; pad[7]; IntAttr { u16 id; pad[2]; u32 num } }
        uint16_t id; uint32_t want; memcpy(&id, e + 8, 2); memcpy(&want, e + 12, 4);
        int found = 0, ok = 0;
        for (int k = 0; k < r->n_int; ++k) if (r->ints[k].id == id) { found = 1; ok = m2_int_match(e[0], r->ints[k].value, want); }
        if (!found || !ok) return 0;
    }
    return 1;
}

// SearchRoom: the rooms other instances on this network announced in the last few seconds, filtered with the game's own
// conditions. If none are there, the answer is an empty list.
static int GUEST_ABI m2_search_room(uint16_t ctx_id, const void* request, const void* opt, uint32_t* req_id) {
    int i, rc = m2_begin_request(ctx_id, request, opt, req_id, &i);
    if (rc) return rc;
    pthread_mutex_unlock(&g_m2_lock);
    const uint8_t* q = (const uint8_t*)request;
    uint32_t world_id, start, max, flag_filter, flag_attr; const uint8_t* filters; uint64_t n_filters;
    memcpy(&world_id, q + 0x04, 4); memcpy(&start, q + 0x10, 4); memcpy(&max, q + 0x14, 4);
    memcpy(&flag_filter, q + 0x18, 4); memcpy(&flag_attr, q + 0x1C, 4); memcpy(&filters, q + 0x20, 8); memcpy(&n_filters, q + 0x28, 8);
    if (!filters) n_filters = 0;
    if (start == 0) start = 1;
    if (max == 0 || max > 20) max = 20;
    rt_rooms_start();
    RoomInfo seen[64];
    const int n_seen = rt_rooms_snapshot(seen, 64, rt_rooms_now_ms());
    RoomInfo match[64]; int n_match = 0;
    for (int k = 0; k < n_seen; ++k) {
        if (seen[k].flags & M2_ROOM_FLAG_HIDDEN) continue;
        if (m2_room_matches(&seen[k], world_id, flag_filter, flag_attr, filters, n_filters)) match[n_match++] = seen[k];
    }
    const int first = (int)start - 1 < n_match ? (int)start - 1 : n_match;
    const int count = n_match - first < (int)max ? n_match - first : (int)max;
    struct Item { M2RoomExternal ext; uint8_t owner[36]; uint8_t pad[4]; M2IntAttr ints[ROOM_MAX_INT]; M2BinAttr bins[ROOM_MAX_BIN]; uint8_t bin_data[ROOM_MAX_BIN][ROOM_MAX_BIN_LEN]; };
    struct Answer { uint32_t start, total, results; uint8_t pad[4]; M2RoomExternal* first; struct Item items[]; };
    struct Answer* a = (struct Answer*)calloc(1, sizeof *a + (size_t)(count > 0 ? count : 0) * sizeof(struct Item));
    if (!a) return M2_ERR_OUT_OF_MEMORY;
    a->start = start; a->total = (uint32_t)n_match; a->results = (uint32_t)count;
    for (int k = 0; k < count; ++k) {
        const RoomInfo* r = &match[first + k];
        struct Item* it = &a->items[k];
        it->ext.next = k + 1 < count ? &a->items[k + 1].ext : NULL;
        it->ext.max_slot = r->max_slot; it->ext.cur_members = r->cur_members; it->ext.flags = r->flags; it->ext.server_id = M2_SERVER_ID;
        it->ext.world_id = r->world_id; it->ext.room_id = r->room_id;
        it->ext.public_slots = r->max_slot; it->ext.open_public_slots = (uint16_t)(r->max_slot - r->cur_members);
        it->ext.joined_slot_mask = r->cur_members >= 64 ? ~0ull : ((1ull << r->cur_members) - 1);
        char who[17]; memcpy(who, r->owner, 16); who[16] = 0;
        m2_np_id(it->owner, who); it->ext.owner_np_id = it->owner;
        for (int x = 0; x < r->n_int; ++x) { it->ints[x].id = r->ints[x].id; it->ints[x].num = r->ints[x].value; }
        it->ext.int_attr = r->n_int ? it->ints : NULL; it->ext.int_attrs = r->n_int;
        for (int x = 0; x < r->n_bin; ++x) { memcpy(it->bin_data[x], r->bins[x].data, r->bins[x].len); it->bins[x].id = r->bins[x].id; it->bins[x].data = it->bin_data[x]; it->bins[x].size = r->bins[x].len; }
        it->ext.search_bin_attr = r->n_bin ? it->bins : NULL; it->ext.search_bin_attrs = r->n_bin;
    }
    a->first = count > 0 ? &a->items[0].ext : NULL;
    pthread_mutex_lock(&g_m2_lock);
    const int still = m2_ctx_index(ctx_id) == i;
    const uint32_t id = still ? m2_post_request_locked(i, ctx_id, M2_REQUEST_EVENT_SEARCH_ROOM, 0, a) : 0;
    pthread_mutex_unlock(&g_m2_lock);
    if (!still) { free(a); return M2_ERR_INVALID_CONTEXT_ID; }
    if (!id) return M2_ERR_OUT_OF_MEMORY;
    *req_id = id;
    rt_logf("sceNpMatching2SearchRoom(context %u, world %u, from %u max %u, %llu int filters, flags 0x%x/0x%x) -> request %u: "
            "%d room(s) heard on the network, %d match, %d returned", ctx_id, world_id, start, max, (unsigned long long)n_filters,
            flag_filter, flag_attr, id, n_seen, n_match, count);
    return 0;
}

// There is one matching service here, this machine, so it has one server id.
static int GUEST_ABI m2_get_server_id(uint16_t ctx_id, uint16_t* server_id) {
    if (!g_np_m2_init) return M2_ERR_NOT_INITIALIZED;
    if (!server_id) return M2_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_m2_lock);
    const int i = m2_ctx_index(ctx_id);
    pthread_mutex_unlock(&g_m2_lock);
    if (i < 0) return M2_ERR_INVALID_CONTEXT_ID;
    *server_id = M2_SERVER_ID;
    np_trace("sceNpMatching2GetServerId(context %u) -> server %u", ctx_id, *server_id);
    return 0;
}

// ---- libSceNpSignaling: contexts --------------------------------------------------------------------------------
// Signaling is how the console finds a peer's address and keeps a connection to it alive. The game creates a
// signaling context right after its Matching2 context (eboot.bin+0x2c90e8, function 0x2c9090, probe run
// 20261102-005629) and destroys the Matching2 context again if this fails. Only the context lifetime exists here:
// no connection can be activated yet, so the game's handler is stored and never called.
// Error values and the id scheme (1..8, handed out round-robin) follow shadPS4's np_error.h and np_signaling_state.cpp.
enum { SIG_ERR_NOT_INITIALIZED = (int)0x80552701, SIG_ERR_CTXID_NOT_AVAILABLE = (int)0x80552704,
       SIG_ERR_INVALID_ARGUMENT = (int)0x80552715 };
#define SIG_MAX_CTX 8
static pthread_mutex_t g_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; char online_id[20]; void* handler; void* arg; } g_sig_ctx[SIG_MAX_CTX + 1];   // index = id
static int g_sig_last_id;

static int GUEST_ABI sig_initialize(const void* param) { (void)param; g_np_sig_init = 1; np_trace("sceNpSignalingInitialize"); return 0; }
static int GUEST_ABI sig_terminate(void) {
    pthread_mutex_lock(&g_sig_lock);
    g_np_sig_init = 0;
    memset(g_sig_ctx, 0, sizeof g_sig_ctx);
    g_sig_last_id = 0;
    pthread_mutex_unlock(&g_sig_lock);
    np_trace("sceNpSignalingTerminate");
    return 0;
}
static int GUEST_ABI sig_create_context(const void* np_id, void* handler, void* arg, int32_t* ctx_id) {
    if (!g_np_sig_init) return SIG_ERR_NOT_INITIALIZED;
    if (!np_id || !ctx_id) return SIG_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_sig_lock);
    int id = g_sig_last_id >= SIG_MAX_CTX ? 1 : g_sig_last_id + 1;
    for (int tried = 0; tried < SIG_MAX_CTX; ++tried, id = id >= SIG_MAX_CTX ? 1 : id + 1) {
        if (g_sig_ctx[id].used) continue;
        memset(&g_sig_ctx[id], 0, sizeof g_sig_ctx[id]);
        g_sig_ctx[id].used = 1;
        memcpy(g_sig_ctx[id].online_id, np_id, 16);
        g_sig_ctx[id].handler = handler;
        g_sig_ctx[id].arg = arg;
        g_sig_last_id = id;
        pthread_mutex_unlock(&g_sig_lock);
        *ctx_id = id;
        rt_logf("sceNpSignalingCreateContext(player '%.16s', handler %p) -> context %d (no connection can be made yet)", (const char*)np_id, handler, id);
        return 0;
    }
    pthread_mutex_unlock(&g_sig_lock);
    rt_logf("sceNpSignalingCreateContext: all %d contexts in use", SIG_MAX_CTX);
    return SIG_ERR_CTXID_NOT_AVAILABLE;
}
// Like the reference, deleting an id that is not in use is not an error.
static int GUEST_ABI sig_delete_context(int32_t ctx_id) {
    if (!g_np_sig_init) return SIG_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_sig_lock);
    const int existed = ctx_id >= 1 && ctx_id <= SIG_MAX_CTX && g_sig_ctx[ctx_id].used;
    if (existed) memset(&g_sig_ctx[ctx_id], 0, sizeof g_sig_ctx[ctx_id]);
    pthread_mutex_unlock(&g_sig_lock);
    np_trace("sceNpSignalingDeleteContext(context %d)%s", ctx_id, existed ? "" : ": no such context");
    return 0;
}

// ---- libSceNpWebApi: library contexts only. No user context can be created (the local user is never signed in to the
// network), so everything that would talk to a PlayStation Network service stays unimplemented and stops the run loudly.
enum { WEBAPI_ERR_INVALID_ARGUMENT = (int)0x80552902, WEBAPI_ERR_INVALID_LIB_CONTEXT_ID = (int)0x80552903, WEBAPI_ERR_LIB_CONTEXT_NOT_FOUND = (int)0x80552904,
       WEBAPI_ERR_LIB_CONTEXT_MAX = (int)0x8055291A };
#define WEBAPI_MAX_LIB 8
static int g_webapi_lib[WEBAPI_MAX_LIB]; static int g_webapi_next = 1;
static int GUEST_ABI webapi_initialize(int http_ctx, uint64_t pool_size) {
    if (pool_size == 0) return WEBAPI_ERR_INVALID_ARGUMENT;
    for (int i = 0; i < WEBAPI_MAX_LIB; ++i) if (!g_webapi_lib[i]) { g_webapi_lib[i] = g_webapi_next++; rt_logf("sceNpWebApiInitialize(http ctx %d, pool %llu) -> library context %d (bookkeeping only)", http_ctx, (unsigned long long)pool_size, g_webapi_lib[i]); return g_webapi_lib[i]; }
    return WEBAPI_ERR_LIB_CONTEXT_MAX;
}
static int GUEST_ABI webapi_terminate(int lib_ctx) {
    if (lib_ctx <= 0 || lib_ctx >= 0x8000) return WEBAPI_ERR_INVALID_LIB_CONTEXT_ID;
    for (int i = 0; i < WEBAPI_MAX_LIB; ++i) if (g_webapi_lib[i] == lib_ctx) { g_webapi_lib[i] = 0; return 0; }
    return WEBAPI_ERR_LIB_CONTEXT_NOT_FOUND;
}

// ---- libSceNpWebApi user contexts and requests ----------------------------------------------------------------
// The console's WebApi is the HTTP interface to PlayStation Network services. Nothing here contacts any service
// and nothing ever will: there is no account and no endpoint to talk to. What is implemented is the local part,
// because the game needs it before it opens its online menus (probe run 20261101-201127: it reached
// sceNpWebApiCreateContext at eboot.bin+0x2cd320 and retried every eight seconds until it succeeded).
// A request can be created and then fails to send, with the console's own "not signed in" error, which is the
// truthful answer: this machine has no PlayStation Network session.
enum { WEBAPI_ERR_OUT_OF_MEMORY = (int)0x80552901, WEBAPI_ERR_USER_CONTEXT_NOT_FOUND = (int)0x80552905, WEBAPI_ERR_REQUEST_NOT_FOUND = (int)0x80552906,
       WEBAPI_ERR_NOT_SIGNED_IN = (int)0x80552907, WEBAPI_ERR_ABORTED = (int)0x80552909,
       WEBAPI_ERR_USER_CONTEXT_ALREADY_EXIST = (int)0x8055290A };
#define WEBAPI_MAX_USER_CTX 8
#define WEBAPI_MAX_REQUEST 16
static struct { int used, lib_ctx; char online_id[20]; } g_webapi_user[WEBAPI_MAX_USER_CTX];
#define WEBAPI_BODY_MAX 512
static struct {
    int used, user_ctx, aborted, http_status;
    int64_t id;
    char path[160];
    char body[WEBAPI_BODY_MAX];
    uint32_t body_len, body_read;
    int method, local, local_status;
} g_webapi_req[WEBAPI_MAX_REQUEST];
static int64_t g_webapi_next_req = 1;
static __thread int t_webapi_error;

// Requests this machine can answer by itself.
//
// The console's WebApi is the HTTP interface to PlayStation Network. There is no account here and no service to
// ask, so almost every request has no truthful answer and reports NOT_SIGNED_IN. One request does have a
// truthful local answer: the player's own block list. Nobody can be blocked on a machine with no accounts and
// no friends service, so the answer is an empty list - that is a fact about this installation, not a reply
// invented on behalf of a service.
//
// Why it matters (probe run 20261101-201845): on entering Online Battle the game asks for
// 'userProfile' '/v1/users/<online id>/blockList' and, when the request fails, shows an error and starts the
// whole sign-in sequence again. The empty list lets it carry on to what this project actually needs to
// implement, which is matching.
// Game sessions (probe run 20261102-013701): right after CreateJoinRoom the game POSTs 'sessionInvitation' '/v1/sessions'
// to create the session that invitations point at, and leaves its room when that fails. A session created here is a
// record on this machine, tied to the room this instance announces; its id is this machine's, not PlayStation Network's.
#define WEBAPI_MAX_SESSIONS 4
static char g_webapi_sessions[WEBAPI_MAX_SESSIONS][40];
static int webapi_session_answer(const char* path, int method, char* out, uint32_t cap, uint32_t* len, int* status) {
    if (strncmp(path, "/v1/sessions", 12) != 0) return 0;
    const char* rest = path + 12;
    if (method == 1 && *rest == 0) {                                   // POST /v1/sessions: create
        for (int s = 0; s < WEBAPI_MAX_SESSIONS; ++s) {
            if (g_webapi_sessions[s][0]) continue;
            const uint64_t a = rt_rooms_new_id(), b = rt_rooms_new_id();
            snprintf(g_webapi_sessions[s], sizeof g_webapi_sessions[s], "%08x-%04x-%04x-%04x-%012llx", (unsigned)(a >> 32), (unsigned)(a >> 16) & 0xffff,
                     (unsigned)a & 0xffff, (unsigned)(b >> 48), (unsigned long long)(b & 0xffffffffffffull));
            const int n = snprintf(out, cap, "{\"sessionId\":\"%s\"}", g_webapi_sessions[s]);
            if (n < 0 || (uint32_t)n >= cap) return 0;
            *len = (uint32_t)n; *status = 200;
            return 1;
        }
        return 0;
    }
    if (*rest != '/') return 0;
    for (int s = 0; s < WEBAPI_MAX_SESSIONS; ++s) {                   // requests on a session created here
        const size_t k = strlen(g_webapi_sessions[s]);
        if (!k || strncmp(rest + 1, g_webapi_sessions[s], k) != 0 || (rest[1 + k] != 0 && rest[1 + k] != '/' && rest[1 + k] != '?')) continue;
        if (method == 3 && rest[1 + k] == 0) { g_webapi_sessions[s][0] = 0; *len = 0; *status = 204; return 1; }   // DELETE: gone
        return 0;
    }
    return 0;
}
static int webapi_local_answer(const char* path, int method, char* out, uint32_t cap, uint32_t* len, int* status) {
    if (!path) return 0;
    *status = 200;
    if (webapi_session_answer(path, method, out, cap, len, status)) return 1;
    const char* tail = strrchr(path, '/');
    if (!tail || strcmp(tail, "/blockList") != 0) return 0;
    const char* body = "{\"start\":0,\"size\":0,\"totalResults\":0,\"blockList\":[]}";
    const uint32_t n = (uint32_t)strlen(body);
    if (n + 1 > cap) return 0;
    memcpy(out, body, n + 1);
    *len = n;
    return 1;
}

static int GUEST_ABI webapi_create_context(int lib_ctx, const void* online_id) {
    if (lib_ctx <= 0 || lib_ctx >= 0x8000) return WEBAPI_ERR_INVALID_LIB_CONTEXT_ID;
    if (!online_id) return WEBAPI_ERR_INVALID_ARGUMENT;
    for (int i = 0; i < WEBAPI_MAX_USER_CTX; ++i)
        if (g_webapi_user[i].used && g_webapi_user[i].lib_ctx == lib_ctx && !memcmp(g_webapi_user[i].online_id, online_id, 16))
            return WEBAPI_ERR_USER_CONTEXT_ALREADY_EXIST;
    for (int i = 0; i < WEBAPI_MAX_USER_CTX; ++i)
        if (!g_webapi_user[i].used) {
            g_webapi_user[i].used = 1;
            g_webapi_user[i].lib_ctx = lib_ctx;
            memcpy(g_webapi_user[i].online_id, online_id, 16);
            g_webapi_user[i].online_id[16] = 0;
            rt_logf("sceNpWebApiCreateContext(library context %d, online id '%.16s') -> user context %d "
                    "(local bookkeeping; no PlayStation Network service is contacted)", lib_ctx, g_webapi_user[i].online_id, i + 1);
            return i + 1;
        }
    return WEBAPI_ERR_OUT_OF_MEMORY;
}
static int webapi_user_index(int ctx) { const int i = ctx - 1; return (i >= 0 && i < WEBAPI_MAX_USER_CTX && g_webapi_user[i].used) ? i : -1; }
static int GUEST_ABI webapi_delete_context(int user_ctx) {
    np_trace("sceNpWebApiDeleteContext(user context %d)", user_ctx);
    const int i = webapi_user_index(user_ctx);
    if (i < 0) return WEBAPI_ERR_USER_CONTEXT_NOT_FOUND;
    g_webapi_user[i].used = 0;
    return 0;
}
static int GUEST_ABI webapi_create_request(int user_ctx, const char* api_group, const char* path, int method,
                                           const void* content, int64_t* request_id) {
    if (!request_id) return WEBAPI_ERR_INVALID_ARGUMENT;
    (void)content;
    if (webapi_user_index(user_ctx) < 0) return WEBAPI_ERR_USER_CONTEXT_NOT_FOUND;
    for (int i = 0; i < WEBAPI_MAX_REQUEST; ++i)
        if (!g_webapi_req[i].used) {
            g_webapi_req[i].used = 1;
            g_webapi_req[i].user_ctx = user_ctx;
            g_webapi_req[i].aborted = 0;
            g_webapi_req[i].id = g_webapi_next_req++;
            g_webapi_req[i].http_status = 0;
            g_webapi_req[i].body_len = g_webapi_req[i].body_read = 0;
            snprintf(g_webapi_req[i].path, sizeof g_webapi_req[i].path, "%s", path ? path : "");
            *request_id = g_webapi_req[i].id;
            g_webapi_req[i].method = method;
            const int local = webapi_local_answer(g_webapi_req[i].path, method, g_webapi_req[i].body, WEBAPI_BODY_MAX, &g_webapi_req[i].body_len, &g_webapi_req[i].local_status);
            g_webapi_req[i].local = local;
            rt_logf("sceNpWebApiCreateRequest(user context %d, '%s' '%s', method %d) -> request %lld (%s)", user_ctx,
                    api_group ? api_group : "", g_webapi_req[i].path, method, (long long)*request_id,
                    local ? "answered locally; no service is contacted" : "it cannot be sent: there is no PlayStation Network session on this machine");
            return 0;
        }
    return WEBAPI_ERR_OUT_OF_MEMORY;
}
static int webapi_req_index(int64_t id) {
    for (int i = 0; i < WEBAPI_MAX_REQUEST; ++i)
        if (g_webapi_req[i].used && g_webapi_req[i].id == id) return i;
    return -1;
}
// Sending is where the console would talk to a PlayStation Network service. There is none, so this reports the
// console's own "not signed in" error rather than inventing a response.
static int GUEST_ABI webapi_send_request(int64_t request_id, const void* data, uint64_t data_size, void* resp_info) {
    (void)data; (void)data_size; (void)resp_info;
    const int i = webapi_req_index(request_id);
    if (i < 0) return WEBAPI_ERR_REQUEST_NOT_FOUND;
    if (g_webapi_req[i].aborted) { t_webapi_error = WEBAPI_ERR_ABORTED; return WEBAPI_ERR_ABORTED; }
    if (g_webapi_req[i].local) {
        g_webapi_req[i].http_status = g_webapi_req[i].local_status;
        g_webapi_req[i].body_read = 0;
        t_webapi_error = 0;
        rt_logf("sceNpWebApiSendRequest(request %lld '%s') -> answered on this machine: %s",
                (long long)request_id, g_webapi_req[i].path, g_webapi_req[i].body);
        return 0;
    }
    t_webapi_error = WEBAPI_ERR_NOT_SIGNED_IN;
    np_trace("sceNpWebApiSendRequest(request %lld '%s', method %d) -> NOT_SIGNED_IN (no local answer; no PlayStation Network is contacted)",
             (long long)request_id, g_webapi_req[i].path, g_webapi_req[i].method);
    return WEBAPI_ERR_NOT_SIGNED_IN;
}
// Reads the response body in pieces, exactly as the console does: the number of bytes copied, then 0 at the end.
static int GUEST_ABI webapi_read_data(int64_t request_id, void* data, uint64_t size) {
    const int i = webapi_req_index(request_id);
    if (i < 0) return WEBAPI_ERR_REQUEST_NOT_FOUND;
    if (!data || !size) return WEBAPI_ERR_INVALID_ARGUMENT;
    const uint32_t left = g_webapi_req[i].body_len - g_webapi_req[i].body_read;
    if (!left) { np_trace("sceNpWebApiReadData(request %lld) -> 0 (end of the body)", (long long)request_id); return 0; }
    const uint32_t n = size < left ? (uint32_t)size : left;
    memcpy(data, g_webapi_req[i].body + g_webapi_req[i].body_read, n);
    g_webapi_req[i].body_read += n;
    return (int)n;
}
static int GUEST_ABI webapi_abort_request(int64_t request_id) {
    const int i = webapi_req_index(request_id);
    if (i < 0) return WEBAPI_ERR_REQUEST_NOT_FOUND;
    g_webapi_req[i].aborted = 1;
    t_webapi_error = WEBAPI_ERR_ABORTED;
    return 0;
}
static int GUEST_ABI webapi_delete_request(int64_t request_id) {
    const int i = webapi_req_index(request_id);
    if (i < 0) return WEBAPI_ERR_REQUEST_NOT_FOUND;
    g_webapi_req[i].used = 0;
    return 0;
}
static int GUEST_ABI webapi_get_error_code(void) { return t_webapi_error; }
static int GUEST_ABI webapi_get_http_status(int64_t request_id, int* status) {
    if (!status) return WEBAPI_ERR_INVALID_ARGUMENT;
    const int i = webapi_req_index(request_id);
    if (i < 0) return WEBAPI_ERR_REQUEST_NOT_FOUND;
    *status = g_webapi_req[i].http_status;   // 0 when no response arrived
    np_trace("sceNpWebApiGetHttpStatusCode(request %lld) -> %d", (long long)request_id, *status);
    return 0;
}
// Pure text handling, so this one is exact: it reads {"onlineId":"name"} out of a JSON object into an SceNpId.
static int GUEST_ABI webapi_parse_np_id(const char* json, void* np_id) {
    if (!json || !np_id) return WEBAPI_ERR_INVALID_ARGUMENT;
    const char* key = strstr(json, "\"onlineId\"");
    if (!key) return WEBAPI_ERR_INVALID_ARGUMENT;
    const char* colon = strchr(key + 10, ':');
    if (!colon) return WEBAPI_ERR_INVALID_ARGUMENT;
    const char* open = strchr(colon, '"');
    if (!open) return WEBAPI_ERR_INVALID_ARGUMENT;
    const char* close = strchr(open + 1, '"');
    if (!close || close - open - 1 > 16) return WEBAPI_ERR_INVALID_ARGUMENT;
    memset(np_id, 0, 36);
    memcpy(np_id, open + 1, (size_t)(close - open - 1));
    return 0;
}

// ---- libSceNpScore: a local leaderboard ------------------------------------------------------------------------
// The console's Score library keeps leaderboards on a PlayStation Network service. There is none here, so this is
// a local board: contexts and requests exist, a recorded score is kept in memory for this session, and a ranking
// query reports an empty board, which is what an installation with no service and no other players has.
//
// Why it is implemented at all (probe run 20261101-202509): sceNpScoreCreateNpTitleCtx is called once at start-up
// from eboot.bin+0x2c8936 and was the only console function still failing while the game refused to open Online
// Battle, which it does from the main menu entry shown in that run's captures.
enum { SCORE_ERR_INVALID_ARGUMENT = (int)0x80550704, SCORE_ERR_TOO_MANY_OBJECTS = (int)0x80550706,
       SCORE_ERR_INSUFFICIENT_ARGUMENT = (int)0x8055070C, SCORE_ERR_INVALID_ID = (int)0x8055070E,
       SCORE_ERR_ABORTED = (int)0x80550705 };
#define SCORE_MAX_CTX 4
#define SCORE_MAX_REQ 8
#define SCORE_INVALID_SERVICE_LABEL 0xFFFFFFFFu
static struct { int used; uint32_t service_label; } g_score_ctx[SCORE_MAX_CTX];
static struct { int used, ctx, result; } g_score_req[SCORE_MAX_REQ];
static struct { int used; uint32_t board; int64_t value; } g_score_board[8];

static int GUEST_ABI score_create_title_ctx(uint32_t service_label, const void* self_np_id) {
    if (service_label == SCORE_INVALID_SERVICE_LABEL) return SCORE_ERR_INVALID_ARGUMENT;
    if (!self_np_id) return SCORE_ERR_INSUFFICIENT_ARGUMENT;
    for (int i = 0; i < SCORE_MAX_CTX; ++i)
        if (!g_score_ctx[i].used) {
            g_score_ctx[i].used = 1;
            g_score_ctx[i].service_label = service_label;
            rt_logf("sceNpScoreCreateNpTitleCtx(service label %u, player '%.16s') -> title context %d "
                    "(a local board: there is no ranking service and no other players)", service_label, (const char*)self_np_id, i + 1);
            return i + 1;
        }
    return SCORE_ERR_TOO_MANY_OBJECTS;
}
static int score_ctx_index(int id) { const int i = id - 1; return (i >= 0 && i < SCORE_MAX_CTX && g_score_ctx[i].used) ? i : -1; }
static int GUEST_ABI score_delete_title_ctx(int ctx) {
    const int i = score_ctx_index(ctx);
    if (i < 0) return SCORE_ERR_INVALID_ID;
    for (int r = 0; r < SCORE_MAX_REQ; ++r)
        if (g_score_req[r].used && g_score_req[r].ctx == ctx) { g_score_req[r].result = SCORE_ERR_ABORTED; g_score_req[r].used = 0; }
    g_score_ctx[i].used = 0;
    return 0;
}
static int GUEST_ABI score_create_request(int ctx) {
    if (score_ctx_index(ctx) < 0) return SCORE_ERR_INVALID_ID;
    for (int i = 0; i < SCORE_MAX_REQ; ++i)
        if (!g_score_req[i].used) { g_score_req[i].used = 1; g_score_req[i].ctx = ctx; g_score_req[i].result = 0; return i + 1; }
    return SCORE_ERR_TOO_MANY_OBJECTS;
}
static int score_req_index(int id) { const int i = id - 1; return (i >= 0 && i < SCORE_MAX_REQ && g_score_req[i].used) ? i : -1; }
static int GUEST_ABI score_delete_request(int req) {
    const int i = score_req_index(req);
    if (i < 0) return SCORE_ERR_INVALID_ID;
    g_score_req[i].used = 0;
    return 0;
}
static int GUEST_ABI score_abort_request(int req) {
    const int i = score_req_index(req);
    if (i < 0) return SCORE_ERR_INVALID_ID;
    g_score_req[i].result = SCORE_ERR_ABORTED;
    return 0;
}
static int GUEST_ABI score_poll_async(int req, int* result) {
    if (!result) return SCORE_ERR_INVALID_ARGUMENT;
    const int i = score_req_index(req);
    if (i < 0) return SCORE_ERR_INVALID_ID;
    *result = g_score_req[i].result;
    np_trace("sceNpScorePollAsync(request %d) -> result %#x", req, (unsigned)*result);
    return 0;
}
// A score is kept for this session only; nothing is uploaded and nothing is read back from any service.
static int GUEST_ABI score_record_score_async(int req, uint32_t board, int64_t value, const void* comment,
                                              const void* game_info, uint32_t* tmp_rank, const void* compare_date, void* option) {
    (void)comment; (void)game_info; (void)compare_date; (void)option;
    const int i = score_req_index(req);
    if (i < 0) return SCORE_ERR_INVALID_ID;
    int slot = -1;
    for (int b = 0; b < 8; ++b) if (g_score_board[b].used && g_score_board[b].board == board) { slot = b; break; }
    if (slot < 0) for (int b = 0; b < 8; ++b) if (!g_score_board[b].used) { g_score_board[b].used = 1; g_score_board[b].board = board; g_score_board[b].value = 0; slot = b; break; }
    if (slot >= 0 && value > g_score_board[slot].value) g_score_board[slot].value = value;
    if (tmp_rank) *tmp_rank = 1;   // the only player on a local board
    g_score_req[i].result = 0;
    rt_logf("sceNpScoreRecordScoreAsync(board %u, score %lld): kept on this machine for this session only", board, (long long)value);
    return 0;
}
// An empty board: no service, no other players, so there is no ranking to report.
static int GUEST_ABI score_get_ranking_by_range_async(int req, uint32_t board, uint32_t start_rank, void* rank_array,
                                                      uint64_t rank_array_size, void* comment_array, uint64_t comment_array_size,
                                                      void* info_array, uint64_t info_array_size, uint64_t array_num,
                                                      void* last_sort_date, uint32_t* total_record, void* option) {
    (void)board; (void)start_rank; (void)comment_array_size; (void)info_array_size; (void)array_num; (void)option;
    const int i = score_req_index(req);
    if (i < 0) return SCORE_ERR_INVALID_ID;
    if (rank_array && rank_array_size) memset(rank_array, 0, (size_t)rank_array_size);
    if (comment_array && comment_array_size) memset(comment_array, 0, (size_t)comment_array_size);
    if (info_array && info_array_size) memset(info_array, 0, (size_t)info_array_size);
    if (last_sort_date) memset(last_sort_date, 0, 8);
    if (total_record) *total_record = 0;
    g_score_req[i].result = 0;
    return 0;
}
static int GUEST_ABI score_get_ranking_by_np_id_async(int req, uint32_t board, const void* np_id_array, uint64_t np_id_array_size,
                                                      void* rank_array, uint64_t rank_array_size, void* comment_array, uint64_t comment_array_size,
                                                      void* info_array, uint64_t info_array_size, uint64_t array_num,
                                                      void* last_sort_date, uint32_t* total_record, void* option) {
    (void)np_id_array; (void)np_id_array_size;
    return score_get_ranking_by_range_async(req, board, 0, rank_array, rank_array_size, comment_array, comment_array_size,
                                            info_array, info_array_size, array_num, last_sort_date, total_record, option);
}

// ---- libSceNpTrophy: contexts and handles are tracked; the registered trophy set is EMPTY ---------------------------------
// sce_sys/trophy/trophy00.trp keeps its trophy configuration encrypted, and this port does not decrypt game content, so there is
// no trophy data to read. RegisterContext succeeds with an EMPTY set (loud log, audit status "diagnostic-stub"): the game needs a
// successful registration to leave its startup screen (Windows runs 20261101-044207 vs 044604), and "title configuration not
// installed" would be the console's answer only for a title that has no trophies, which this one has. The unlock-state query
// reports zero trophies; no unlock is ever recorded. sceNpTrophyUnlockTrophy (first reached in the first battle, Windows run
// 20261101-061005 trapped on it) answers INVALID_TROPHY_ID for every id: the registered set is empty, so no id can exist.
// --experiment trophy-strict restores the old error answer for comparison.
enum { TROPHY_ERR_INVALID_ARGUMENT = (int)0x80551604, TROPHY_ERR_INVALID_HANDLE = (int)0x80551608, TROPHY_ERR_INVALID_CONTEXT = (int)0x80551609,
       TROPHY_ERR_NOT_REGISTERED = (int)0x8055160F, TROPHY_ERR_CONTEXT_ALREADY_EXISTS = (int)0x80551613, TROPHY_ERR_CONTEXT_EXCEEDS_MAX = (int)0x80551623,
       TROPHY_ERR_HANDLE_EXCEEDS_MAX = (int)0x80551624, TROPHY_ERR_TITLE_CONF_NOT_INSTALLED = (int)0x80551626,
       TROPHY_ERR_INVALID_TROPHY_ID = (int)0x8055160A };
#define TROPHY_MAX_CTX 4
#define TROPHY_MAX_HANDLE 4
static struct { int used; int user; uint32_t label; int registered; } g_trophy_ctx[TROPHY_MAX_CTX];
static int g_trophy_handle[TROPHY_MAX_HANDLE];
static int GUEST_ABI trophy_create_context(int* ctx, int user, uint32_t label, uint64_t options) {
    if (!ctx || options) return TROPHY_ERR_INVALID_ARGUMENT;
    int free_slot = -1;
    for (int i = 0; i < TROPHY_MAX_CTX; ++i) {
        if (g_trophy_ctx[i].used && g_trophy_ctx[i].user == user && g_trophy_ctx[i].label == label) return TROPHY_ERR_CONTEXT_ALREADY_EXISTS;
        if (!g_trophy_ctx[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return TROPHY_ERR_CONTEXT_EXCEEDS_MAX;
    g_trophy_ctx[free_slot].used = 1; g_trophy_ctx[free_slot].user = user; g_trophy_ctx[free_slot].label = label;
    *ctx = free_slot + 1;
    rt_logf("sceNpTrophyCreateContext(user %d, label %u) -> context %d (trophy pack is encrypted: it will be registered with an EMPTY trophy set)", user, label, *ctx);
    return 0;
}
static int GUEST_ABI trophy_create_handle(int* h) {
    if (!h) return TROPHY_ERR_INVALID_ARGUMENT;
    for (int i = 0; i < TROPHY_MAX_HANDLE; ++i) if (!g_trophy_handle[i]) { g_trophy_handle[i] = 1; *h = i + 1; return 0; }
    return TROPHY_ERR_HANDLE_EXCEEDS_MAX;
}
static int GUEST_ABI trophy_destroy_context(int ctx) {
    if (ctx < 1 || ctx > TROPHY_MAX_CTX || !g_trophy_ctx[ctx - 1].used) return TROPHY_ERR_INVALID_CONTEXT;
    g_trophy_ctx[ctx - 1].used = 0; g_trophy_ctx[ctx - 1].registered = 0; return 0;
}
static int GUEST_ABI trophy_destroy_handle(int h) {
    if (h < 1 || h > TROPHY_MAX_HANDLE || !g_trophy_handle[h - 1]) return TROPHY_ERR_INVALID_HANDLE;
    g_trophy_handle[h - 1] = 0; return 0;
}
// Diagnostic experiments (--experiment name[,name]): each changes one platform answer to find out whether the game depends
// on it. They are never on by default and are logged loudly; an answer is only adopted after the evidence is understood.
static char g_experiments[256];
void rt_set_experiments(const char* list) {
    snprintf(g_experiments, sizeof g_experiments, "%s", list ? list : "");
    if (g_experiments[0]) rt_logf("EXPERIMENTS ENABLED: %s (diagnosis only: platform answers differ from the default behavior)", g_experiments);
}
int rt_experiment(const char* name) {
    size_t n = strlen(name);
    for (const char* p = g_experiments; *p;) {
        const char* e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return 1;
        p += l + (e ? 1 : 0);
    }
    return 0;
}
static int GUEST_ABI trophy_register_context(int ctx, int h, uint64_t options) {
    if (ctx < 1 || ctx > TROPHY_MAX_CTX || !g_trophy_ctx[ctx - 1].used) return TROPHY_ERR_INVALID_CONTEXT;
    if (h < 1 || h > TROPHY_MAX_HANDLE || !g_trophy_handle[h - 1]) return TROPHY_ERR_INVALID_HANDLE;
    if (options) return TROPHY_ERR_INVALID_ARGUMENT;
    if (rt_experiment("trophy-strict")) { // restores the previous behavior (error); the game then stalls on a black screen
        rt_logf("sceNpTrophyRegisterContext(context %d): experiment trophy-strict: reporting TITLE_CONF_NOT_INSTALLED", ctx);
        return TROPHY_ERR_TITLE_CONF_NOT_INSTALLED;
    }
    // Evidence (Windows run 20261101-044207 vs 044604): when this call fails the game never leaves a black screen with a controller
    // connected; when it succeeds the game goes on to its copyright screen. The trophy pack is encrypted and this port does not
    // decrypt it (no keys are added), so there is no trophy data to register: the context is registered with an EMPTY trophy set.
    if (!g_trophy_ctx[ctx - 1].registered)
        rt_logf("sceNpTrophyRegisterContext(context %d): LIMITED SUPPORT - sce_sys/trophy/trophy00.trp is encrypted and is not decrypted by this port; "
                "registering an EMPTY trophy set so the game can continue. No trophy can unlock and nothing is recorded", ctx);
    g_trophy_ctx[ctx - 1].registered = 1;
    return 0;
}
static int GUEST_ABI trophy_unlock_state(int ctx, int h, void* flags, uint32_t* count) {
    if (ctx < 1 || ctx > TROPHY_MAX_CTX || !g_trophy_ctx[ctx - 1].used) return TROPHY_ERR_INVALID_CONTEXT;
    if (h < 1 || h > TROPHY_MAX_HANDLE || !g_trophy_handle[h - 1]) return TROPHY_ERR_INVALID_HANDLE;
    if (!flags || !count) return TROPHY_ERR_INVALID_ARGUMENT;
    if (!g_trophy_ctx[ctx - 1].registered) return TROPHY_ERR_NOT_REGISTERED;
    memset(flags, 0, 16); *count = 0; // the registered set is empty: no trophies, so no unlock flags
    return 0;
}

// int sceNpTrophyUnlockTrophy(context, handle, trophyId, int* platinumId). The registered trophy set is EMPTY (the trophy pack is
// encrypted and not decrypted here), so no trophy id exists and nothing can be unlocked: the call reports INVALID_TROPHY_ID, which is
// what the console library answers for an id that is not in the registered configuration. The result is logged every time so a
// game that depends on it is visible; nothing is recorded, platinumId is left untouched.
static int GUEST_ABI trophy_unlock(int ctx, int h, int trophy_id, int* platinum_id) {
    if (ctx < 1 || ctx > TROPHY_MAX_CTX || !g_trophy_ctx[ctx - 1].used) return TROPHY_ERR_INVALID_CONTEXT;
    if (h < 1 || h > TROPHY_MAX_HANDLE || !g_trophy_handle[h - 1]) return TROPHY_ERR_INVALID_HANDLE;
    if (trophy_id < 0 || trophy_id >= 128) return TROPHY_ERR_INVALID_TROPHY_ID;
    if (!platinum_id) return TROPHY_ERR_INVALID_ARGUMENT;
    if (!g_trophy_ctx[ctx - 1].registered) return TROPHY_ERR_NOT_REGISTERED;
    rt_logf("sceNpTrophyUnlockTrophy(context %d, trophy id %d): LIMITED SUPPORT - the registered trophy set is EMPTY (encrypted trophy pack not "
            "decrypted by this port), so the trophy cannot be unlocked and nothing is recorded; answering INVALID_TROPHY_ID", ctx, trophy_id);
    return TROPHY_ERR_INVALID_TROPHY_ID;
}

const HostSymbol platform_symbols[] = {
    {"sceNpTrophyCreateContext", "libSceNpTrophy", (void*)trophy_create_context, 0},
    {"sceNpTrophyCreateHandle", "libSceNpTrophy", (void*)trophy_create_handle, 0},
    {"sceNpTrophyDestroyContext", "libSceNpTrophy", (void*)trophy_destroy_context, 0},
    {"sceNpTrophyDestroyHandle", "libSceNpTrophy", (void*)trophy_destroy_handle, 0},
    {"sceNpTrophyRegisterContext", "libSceNpTrophy", (void*)trophy_register_context, 0},
    {"sceNpTrophyGetTrophyUnlockState", "libSceNpTrophy", (void*)trophy_unlock_state, 0},
    {"sceNpTrophyUnlockTrophy", "libSceNpTrophy", (void*)trophy_unlock, 0},
    {"sceNpWebApiInitialize", "libSceNpWebApi", (void*)webapi_initialize, 0},
    {"sceNpWebApiTerminate", "libSceNpWebApi", (void*)webapi_terminate, 0},
    {"sceNpWebApiCreateContext", "libSceNpWebApi", (void*)webapi_create_context, 0},
    {"sceNpWebApiDeleteContext", "libSceNpWebApi", (void*)webapi_delete_context, 0},
    {"sceNpWebApiCreateRequest", "libSceNpWebApi", (void*)webapi_create_request, 0},
    {"sceNpWebApiSendRequest", "libSceNpWebApi", (void*)webapi_send_request, 0},
    {"sceNpWebApiReadData", "libSceNpWebApi", (void*)webapi_read_data, 0},
    {"sceNpWebApiAbortRequest", "libSceNpWebApi", (void*)webapi_abort_request, 0},
    {"sceNpWebApiDeleteRequest", "libSceNpWebApi", (void*)webapi_delete_request, 0},
    {"sceNpWebApiGetErrorCode", "libSceNpWebApi", (void*)webapi_get_error_code, 0},
    {"sceNpWebApiGetHttpStatusCode", "libSceNpWebApi", (void*)webapi_get_http_status, 0},
    {"sceNpWebApiUtilityParseNpId", "libSceNpWebApi", (void*)webapi_parse_np_id, 0},
    {"sceScreenShotEnable", "libSceScreenShot", (void*)ss_enable, 0},
    {"sceScreenShotDisable", "libSceScreenShot", (void*)ss_disable, 0},
    {"sceScreenShotSetOverlayImage", "libSceScreenShot", (void*)ss_overlay, 0},
    {"sceNpMatching2Initialize", "libSceNpMatching2", (void*)m2_initialize, 0},
    {"sceNpMatching2Terminate", "libSceNpMatching2", (void*)m2_terminate, 0},
    {"sceNpMatching2CreateContext", "libSceNpMatching2", (void*)m2_create_context, 0},
    {"sceNpMatching2DestroyContext", "libSceNpMatching2", (void*)m2_destroy_context, 0},
    {"sceNpMatching2RegisterContextCallback", "libSceNpMatching2", (void*)m2_register_context_callback, 0},
    {"sceNpMatching2RegisterRoomEventCallback", "libSceNpMatching2", (void*)m2_register_room_event_callback, 0},
    {"sceNpMatching2ContextStart", "libSceNpMatching2", (void*)m2_context_start, 0},
    {"sceNpMatching2ContextStop", "libSceNpMatching2", (void*)m2_context_stop, 0},
    {"sceNpMatching2AbortContextStart", "libSceNpMatching2", (void*)m2_abort_context_start, 0},
    {"sceNpMatching2GetServerId", "libSceNpMatching2", (void*)m2_get_server_id, 0},
    {"sceNpMatching2GetWorldInfoList", "libSceNpMatching2", (void*)m2_get_world_info_list, 0},
    {"sceNpMatching2CreateJoinRoom", "libSceNpMatching2", (void*)m2_create_join_room, 0},
    {"sceNpMatching2LeaveRoom", "libSceNpMatching2", (void*)m2_leave_room, 0},
    {"sceNpMatching2SetRoomDataExternal", "libSceNpMatching2", (void*)m2_set_room_data_external, 0},
    {"sceNpMatching2SearchRoom", "libSceNpMatching2", (void*)m2_search_room, 0},
    {"sceNpSignalingInitialize", "libSceNpSignaling", (void*)sig_initialize, 0},
    {"sceNpSignalingTerminate", "libSceNpSignaling", (void*)sig_terminate, 0},
    {"sceNpSignalingCreateContext", "libSceNpSignaling", (void*)sig_create_context, 0},
    {"sceNpSignalingDeleteContext", "libSceNpSignaling", (void*)sig_delete_context, 0},
    {"sceSharePlayInitialize", "libSceSharePlay", (void*)sp_initialize, 0},
    {"sceSharePlayTerminate", "libSceSharePlay", (void*)sp_terminate, 0},
    {"sceSharePlaySetProhibition", "libSceSharePlay", (void*)sp_set_prohibition, 0},
    {"sceSharePlayGetCurrentConnectionInfo", "libSceSharePlay", (void*)sp_connection_info, 0},
    {"sceCommonDialogInitialize", "libSceCommonDialog", (void*)cd_init, 0},
    {"sceSaveDataDialogProgressBarSetValue", "libSceSaveDataDialog", (void*)save_progress_set, 0},
    {"sceMsgDialogInitialize", "libSceMsgDialog", (void*)msg_init, 0},
    {"sceMsgDialogTerminate", "libSceMsgDialog", (void*)msg_term, 0},
    {"sceMsgDialogUpdateStatus", "libSceMsgDialog", (void*)msg_status, 0},
    {"sceErrorDialogInitialize", "libSceErrorDialog", (void*)err_init, 0},
    {"sceErrorDialogTerminate", "libSceErrorDialog", (void*)err_term, 0},
    {"sceErrorDialogUpdateStatus", "libSceErrorDialog", (void*)err_status, 0},
    {"sceErrorDialogGetStatus", "libSceErrorDialog", (void*)err_status, 0},
    {"sceErrorDialogOpen", "libSceErrorDialog", (void*)err_open, 0},
    {"sceErrorDialogClose", "libSceErrorDialog", (void*)err_close, 0},
    {"sceSaveDataDialogInitialize", "libSceSaveDataDialog", (void*)save_init, 0},
    {"sceSaveDataDialogTerminate", "libSceSaveDataDialog", (void*)save_term, 0},
    {"sceSaveDataDialogUpdateStatus", "libSceSaveDataDialog", (void*)save_status, 0},
    {"sceSaveDataDialogGetStatus", "libSceSaveDataDialog", (void*)save_status, 0},
    {"sceSaveDataDialogIsReadyToDisplay", "libSceSaveDataDialog", (void*)save_ready, 0},
    {"sceImeDialogInit", "libSceImeDialog", (void*)ime_init, 0},
    {"sceImeDialogGetStatus", "libSceImeDialog", (void*)ime_status, 0},
    {"sceImeDialogTerm", "libSceImeDialog", (void*)ime_term, 0},
    {"sceInvitationDialogInitialize", "libSceInvitationDialog", (void*)invite_init, 0},
    {"sceInvitationDialogTerminate", "libSceInvitationDialog", (void*)invite_term, 0},
    {"sceInvitationDialogUpdateStatus", "libSceInvitationDialog", (void*)invite_status, 0},
    {"sceNpCommerceDialogInitialize", "libSceNpCommerce", (void*)commerce_init, 0},
    {"sceNpCommerceDialogTerminate", "libSceNpCommerce", (void*)commerce_term, 0},
    {"sceNpCommerceDialogUpdateStatus", "libSceNpCommerce", (void*)commerce_status, 0},
    {"sceNpCommerceDialogGetStatus", "libSceNpCommerce", (void*)commerce_status, 0},
    {"sceNpProfileDialogInitialize", "libSceNpProfileDialog", (void*)profile_init, 0},
    {"sceNpProfileDialogTerminate", "libSceNpProfileDialog", (void*)profile_term, 0},
    {"sceNpProfileDialogUpdateStatus", "libSceNpProfileDialog", (void*)profile_status, 0},
    {"sceSslInit", "libSceSsl", (void*)ssl_init, 0},
    {"sceSslTerm", "libSceSsl", (void*)ssl_term, 0},
    {"sceHttpInit", "libSceHttp", (void*)http_init, 0},
    {"sceHttpTerm", "libSceHttp", (void*)http_term, 0},
    {"sceNpSetNpTitleId", "libSceNpManager", (void*)np_set_title_id, 0},
    {"sceNpSetContentRestriction", "libSceNpManager", (void*)np_set_restriction, 0},
    {"sceNpRegisterStateCallback", "libSceNpManager", (void*)np_register_state_cb, 0},
    {"sceNpCreateRequest", "libSceNpManager", (void*)np_create_request, 0},
    {"sceNpCreateAsyncRequest", "libSceNpManager", (void*)np_create_async_request, 0},
    {"sceNpDeleteRequest", "libSceNpManager", (void*)np_delete_request, 0},
    {"sceNpAbortRequest", "libSceNpManager", (void*)np_abort_request, 0},
    {"sceNpPollAsync", "libSceNpManager", (void*)np_poll_async, 0},
    {"sceNpCheckNpAvailability", "libSceNpManager", (void*)np_check_availability, 0},
    {"sceNpCheckNpAvailabilityA", "libSceNpManager", (void*)np_check_availability_a, 0},
    {"sceNpCheckPlus", "libSceNpManager", (void*)np_check_plus, 0},
    {"sceNpNotifyPlusFeature", "libSceNpManager", (void*)np_notify_plus_feature, 0},
    {"sceNpGetAccountCountry", "libSceNpManager", (void*)np_get_account_country, 0},
    {"sceNpGetAccountLanguage", "libSceNpManager", (void*)np_get_account_language, 0},
    {"sceNpCmpNpId", "libSceNpCommon", (void*)np_cmp_np_id, 0},
    {"sceNpScoreCreateNpTitleCtx", "libSceNpScore", (void*)score_create_title_ctx, 0},
    {"sceNpScoreDeleteNpTitleCtx", "libSceNpScore", (void*)score_delete_title_ctx, 0},
    {"sceNpScoreCreateRequest", "libSceNpScore", (void*)score_create_request, 0},
    {"sceNpScoreDeleteRequest", "libSceNpScore", (void*)score_delete_request, 0},
    {"sceNpScoreAbortRequest", "libSceNpScore", (void*)score_abort_request, 0},
    {"sceNpScorePollAsync", "libSceNpScore", (void*)score_poll_async, 0},
    {"sceNpScoreRecordScoreAsync", "libSceNpScore", (void*)score_record_score_async, 0},
    {"sceNpScoreGetRankingByRangeAsync", "libSceNpScore", (void*)score_get_ranking_by_range_async, 0},
    {"sceNpScoreGetRankingByNpIdAsync", "libSceNpScore", (void*)score_get_ranking_by_np_id_async, 0},
    {"sceNpUnregisterStateCallback", "libSceNpManager", (void*)np_unregister_state_cb, 0},
    {"sceNpCheckCallback", "libSceNpManager", (void*)np_check_callback, 0},
    {"sceNpGetState", "libSceNpManager", (void*)np_get_state, 0},
    {"sceNpGetNpId", "libSceNpManager", (void*)np_get_np_id, 0},
    {"sceNpGetOnlineId", "libSceNpManager", (void*)np_get_online_id, 0},
};
const size_t platform_symbol_count = sizeof platform_symbols / sizeof *platform_symbols;
