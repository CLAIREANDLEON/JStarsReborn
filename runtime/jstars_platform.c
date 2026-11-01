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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

// ---- common dialogs: initialization and status only --------------------------------------------------------
enum { DLG_NONE = 0, DLG_INITIALIZED = 1, DLG_RUNNING = 2, DLG_FINISHED = 3 };
static int g_dlg[8];
enum { D_MSG, D_ERROR, D_SAVE, D_IME, D_INVITE, D_COMMERCE, D_PROFILE, D_COUNT };
#define DLG_FN(tag, idx) \
    static int GUEST_ABI tag##_init(void) { g_dlg[idx] = DLG_INITIALIZED; return 0; } \
    static int GUEST_ABI tag##_term(void) { g_dlg[idx] = DLG_NONE; return 0; } \
    static int GUEST_ABI tag##_status(void) { return g_dlg[idx]; }
DLG_FN(msg, D_MSG) DLG_FN(err, D_ERROR) DLG_FN(save, D_SAVE) DLG_FN(invite, D_INVITE) DLG_FN(commerce, D_COMMERCE) DLG_FN(profile, D_PROFILE)
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
    if (np_current_state() != NP_STATE_SIGNED_IN) return NP_SIGNED_OUT;
    local_online_id((char*)online_id);
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
static int GUEST_ABI np_notify_plus_feature(const void* param) { (void)param; return 0; }
static int GUEST_ABI np_get_account_country(int user_id, void* country) {
    if (!country) return NP_INVALID_ARG;
    if (user_id != 1) return NP_USER_NOT_FOUND;
    const char* c = getenv("JSTARS_COUNTRY");
    memset(country, 0, 4);
    memcpy(country, (c && strlen(c) == 2) ? c : "US", 2);
    return 0;
}
static int GUEST_ABI np_get_account_language(int user_id, int* language) {
    if (!language) return NP_INVALID_ARG;
    if (user_id != 1) return NP_USER_NOT_FOUND;
    const char* l = getenv("JSTARS_LANG");
    *language = l && *l ? atoi(l) : 1;   // 1 = English (US), the same default the system service reports
    return 0;
}

// ---- libSceNpCommon ------------------------------------------------------------------------------------------
// SceNpId is the 20-byte online id followed by opt[8] and reserved[8]; only the online id identifies a player.
static int GUEST_ABI np_cmp_np_id(const void* a, const void* b) {
    if (!a || !b) return NP_INVALID_ARG;
    return memcmp(a, b, 16) == 0 ? 0 : 1;
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

// ---- libSceNpMatching2 / libSceNpSignaling: library lifetime only (no network, no sign-in => no contexts can be used) -----
static int g_np_m2_init, g_np_sig_init;
static int GUEST_ABI m2_initialize(const void* param) { (void)param; g_np_m2_init = 1; return 0; }
static int GUEST_ABI m2_terminate(void) { g_np_m2_init = 0; return 0; }
static int GUEST_ABI sig_initialize(const void* param) { (void)param; g_np_sig_init = 1; return 0; }
static int GUEST_ABI sig_terminate(void) { g_np_sig_init = 0; return 0; }

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
static int webapi_local_answer(const char* path, char* out, uint32_t cap, uint32_t* len) {
    if (!path) return 0;
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
            const int local = webapi_local_answer(g_webapi_req[i].path, g_webapi_req[i].body, WEBAPI_BODY_MAX, &g_webapi_req[i].body_len);
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
    if (g_webapi_req[i].body_len) {
        g_webapi_req[i].http_status = 200;
        g_webapi_req[i].body_read = 0;
        t_webapi_error = 0;
        rt_logf("sceNpWebApiSendRequest(request %lld '%s') -> answered on this machine: %s",
                (long long)request_id, g_webapi_req[i].path, g_webapi_req[i].body);
        return 0;
    }
    t_webapi_error = WEBAPI_ERR_NOT_SIGNED_IN;
    return WEBAPI_ERR_NOT_SIGNED_IN;
}
// Reads the response body in pieces, exactly as the console does: the number of bytes copied, then 0 at the end.
static int GUEST_ABI webapi_read_data(int64_t request_id, void* data, uint64_t size) {
    const int i = webapi_req_index(request_id);
    if (i < 0) return WEBAPI_ERR_REQUEST_NOT_FOUND;
    if (!data || !size) return WEBAPI_ERR_INVALID_ARGUMENT;
    const uint32_t left = g_webapi_req[i].body_len - g_webapi_req[i].body_read;
    if (!left) return 0;
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
    {"sceNpSignalingInitialize", "libSceNpSignaling", (void*)sig_initialize, 0},
    {"sceNpSignalingTerminate", "libSceNpSignaling", (void*)sig_terminate, 0},
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
    {"sceNpUnregisterStateCallback", "libSceNpManager", (void*)np_unregister_state_cb, 0},
    {"sceNpCheckCallback", "libSceNpManager", (void*)np_check_callback, 0},
    {"sceNpGetState", "libSceNpManager", (void*)np_get_state, 0},
    {"sceNpGetNpId", "libSceNpManager", (void*)np_get_np_id, 0},
    {"sceNpGetOnlineId", "libSceNpManager", (void*)np_get_online_id, 0},
};
const size_t platform_symbol_count = sizeof platform_symbols / sizeof *platform_symbols;
