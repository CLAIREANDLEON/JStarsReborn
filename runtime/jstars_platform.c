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
static int g_np_cb_registered;
static int GUEST_ABI np_set_title_id(const void* id, const void* secret) { (void)id; (void)secret; return 0; }
static int GUEST_ABI np_set_restriction(const void* r) { (void)r; return 0; }
static int GUEST_ABI np_register_state_cb(void* cb, void* user) { (void)cb; (void)user; g_np_cb_registered = 1; return 0; } // never invoked: the state never changes
static int GUEST_ABI np_check_callback(void) { return 0; }
// OrbisNpState: 0 not signed up, 1 signed out, 2 signed in. The default is signed out; the online-probe experiment
// reports signed in so one run can record what the game tries next. That answer is a probe, not an implementation.
static int GUEST_ABI np_get_state(int user, int* state) {
    if (!state) return NP_INVALID_ARG;
    if (user != 1) return NP_USER_NOT_FOUND;
    *state = rt_experiment("online-probe") ? 2 : 1;
    return 0;
}

// Local identity for the probe: the same name the console user service reports (JSTARS_USER_NAME, default "Player").
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
    if (!rt_experiment("online-probe")) return NP_SIGNED_OUT;
    local_online_id((char*)online_id);
    return 0;
}
static int GUEST_ABI np_get_np_id(int user, void* np_id) {
    if (!np_id) return NP_INVALID_ARG;
    if (user != 1) return NP_USER_NOT_FOUND;
    if (!rt_experiment("online-probe")) return NP_SIGNED_OUT;
    memset(np_id, 0, 36);
    local_online_id((char*)np_id);
    return 0;
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
    {"sceNpCheckCallback", "libSceNpManager", (void*)np_check_callback, 0},
    {"sceNpGetState", "libSceNpManager", (void*)np_get_state, 0},
    {"sceNpGetNpId", "libSceNpManager", (void*)np_get_np_id, 0},
    {"sceNpGetOnlineId", "libSceNpManager", (void*)np_get_online_id, 0},
};
const size_t platform_symbol_count = sizeof platform_symbols / sizeof *platform_symbols;
