// System-level services: SystemService, UserService, Sysmodule, Rtc.
// Defaults (language, date format, enter button, user name) are configuration, not detection:
// override with JSTARS_LANG (SCE language id), JSTARS_ENTER_BUTTON (circle|cross), JSTARS_USER_NAME.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

enum { SS_ERR_PARAMETER = (int)0x80A10003, SS_ERR_NO_EVENT = (int)0x80A10004,
       US_ERR_INVALID = (int)0x80960005, US_ERR_NO_EVENT = (int)0x80960007, US_ERR_NOT_INIT = (int)0x80960002 };

#ifdef _WIN32
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#define gmtime_r(t, tm) (gmtime_s((tm), (t)) ? NULL : (tm))
static int std_offset_minutes(void) { _tzset(); return (int)(-(long)_timezone / 60); }
#else
static int std_offset_minutes(void) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); return (int)((tm.tm_gmtoff - (tm.tm_isdst > 0 ? 3600 : 0)) / 60); }
#endif
static int cfg_int(const char* env, int def) { const char* v = getenv(env); return v && *v ? atoi(v) : def; }

static int GUEST_ABI s_paramgetint(int id, int* value) {
    if (!value) return SS_ERR_PARAMETER;
    switch (id) {
    case 1: *value = cfg_int("JSTARS_LANG", 1); return 0;      // 1 = English (US)
    case 2: *value = 1; return 0;                              // DDMMYYYY
    case 3: *value = 1; return 0;                              // 24-hour clock
    case 4: *value = std_offset_minutes(); return 0;
    case 5: { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); *value = tm.tm_isdst > 0; return 0; }
    case 7: *value = 0; return 0;                              // parental level off
    case 1000: { const char* e = getenv("JSTARS_ENTER_BUTTON"); *value = e && !strcmp(e, "circle") ? 0 : 1; return 0; }
    default: rt_logf("sceSystemServiceParamGetInt: unsupported parameter id %d", id); return SS_ERR_PARAMETER;
    }
}
static int GUEST_ABI s_hide_splash(void) { return 0; }
static int GUEST_ABI s_getstatus(void* status) {
    if (!status) return SS_ERR_PARAMETER;
    memset(status, 0, 4 + 5); // event_num + five bool flags (shadPS4 / SDK layout)
    return 0;
}
static int GUEST_ABI s_receiveevent(void* ev) { return ev ? SS_ERR_NO_EVENT : SS_ERR_PARAMETER; } // no system events are produced by this host yet
static int GUEST_ABI s_music(void) { return 0; }

static int g_user_init;
static int GUEST_ABI u_initialize(void* params) { (void)params; g_user_init = 1; return 0; }
static int GUEST_ABI u_terminate(void) { g_user_init = 0; return 0; }
static int GUEST_ABI u_initial_user(int* id) { if (!id) return US_ERR_INVALID; if (!g_user_init) return US_ERR_NOT_INIT; *id = 1; return 0; }
static int GUEST_ABI u_username(int id, char* out, size_t size) {
    if (id != 1 || !out || !size) return US_ERR_INVALID;
    const char* n = getenv("JSTARS_USER_NAME");
    snprintf(out, size, "%s", n && *n ? n : "Player");
    return 0;
}
static int GUEST_ABI u_getevent(void* ev) { (void)ev; return US_ERR_NO_EVENT; }

static int GUEST_ABI sm_load(unsigned id) { rt_logf("sceSysmoduleLoadModule(0x%x): API availability is decided per import by this runtime", id); return 0; }
static int GUEST_ABI sm_unload(unsigned id) { (void)id; return 0; }

typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } RtcDateTime;
#define RTC_EPOCH_OFFSET_US 62135596800000000ull // 0001-01-01 -> 1970-01-01
static int GUEST_ABI r_local_time(RtcDateTime* out) {
    struct timeval tv; gettimeofday(&tv, NULL);
    time_t ts = (time_t)tv.tv_sec; struct tm tm; localtime_r(&ts, &tm);
    *out = (RtcDateTime){(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, (uint32_t)tv.tv_usec};
    return 0;
}
static int GUEST_ABI r_current_tick(uint64_t* out) { struct timeval tv; gettimeofday(&tv, NULL); *out = (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec + RTC_EPOCH_OFFSET_US; return 0; }
static int GUEST_ABI r_set_tick(RtcDateTime* out, const uint64_t* tick) {
    uint64_t us = *tick - RTC_EPOCH_OFFSET_US; time_t s = (time_t)(us / 1000000ull);
    struct tm tm; gmtime_r(&s, &tm);
    *out = (RtcDateTime){(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, (uint32_t)(us % 1000000ull)};
    return 0;
}
static int GUEST_ABI r_tick_add_minutes(uint64_t* out, const uint64_t* in, int64_t m) { *out = *in + (uint64_t)(m * 60 * 1000000ll); return 0; }
#ifdef _WIN32
#define TIMEGM _mkgmtime
#else
#define TIMEGM timegm
#endif
static int GUEST_ABI r_day_of_week(int y, int m, int d) { struct tm tm = {0}; tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 12; TIMEGM(&tm); return tm.tm_wday; }

const HostSymbol system_symbols[] = {
    {"sceSystemServiceParamGetInt", "libSceSystemService", (void*)s_paramgetint, 0},
    {"sceSystemServiceHideSplashScreen", "libSceSystemService", (void*)s_hide_splash, 0},
    {"sceSystemServiceGetStatus", "libSceSystemService", (void*)s_getstatus, 0},
    {"sceSystemServiceReceiveEvent", "libSceSystemService", (void*)s_receiveevent, 0},
    {"sceSystemServiceReenableMusicPlayer", "libSceSystemService", (void*)s_music, 0},
    {"sceSystemServiceDisableMusicPlayer", "libSceSystemService", (void*)s_music, 0},
    {"sceUserServiceInitialize", "libSceUserService", (void*)u_initialize, 0},
    {"sceUserServiceTerminate", "libSceUserService", (void*)u_terminate, 0},
    {"sceUserServiceGetInitialUser", "libSceUserService", (void*)u_initial_user, 0},
    {"sceUserServiceGetUserName", "libSceUserService", (void*)u_username, 0},
    {"sceUserServiceGetEvent", "libSceUserService", (void*)u_getevent, 0},
    {"sceSysmoduleLoadModule", "libSceSysmodule", (void*)sm_load, 0},
    {"sceSysmoduleUnloadModule", "libSceSysmodule", (void*)sm_unload, 0},
    {"sceRtcGetCurrentClockLocalTime", "libSceRtc", (void*)r_local_time, 0},
    {"sceRtcGetCurrentTick", "libSceRtc", (void*)r_current_tick, 0},
    {"sceRtcGetCurrentNetworkTick", "libSceRtc", (void*)r_current_tick, 0}, // no network time source: local clock
    {"sceRtcSetTick", "libSceRtc", (void*)r_set_tick, 0},
    {"sceRtcTickAddMinutes", "libSceRtc", (void*)r_tick_add_minutes, 0},
    {"sceRtcGetDayOfWeek", "libSceRtc", (void*)r_day_of_week, 0},
};
const size_t system_symbol_count = sizeof system_symbols / sizeof *system_symbols;
