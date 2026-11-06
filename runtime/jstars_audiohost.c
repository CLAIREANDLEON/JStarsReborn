// Host audio device. Windows: WASAPI shared mode, event driven, default render endpoint, 48 kHz stereo float with the system's
// sample-rate converter (AUTOCONVERTPCM). Every console audio port owns a ring of stereo frames; the render thread sums the
// rings into the device buffer. Nothing here fabricates sound: it only forwards samples the game submitted, and the counters in
// OsAudioStats say how much was submitted, how much reached the device, and how often the device ran dry.
#include "jstars_os.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <pthread.h>

#define PORTS 8
#define RING 4096                      // stereo frames of storage per port (about 85 ms at 48 kHz)
// Output delay. A port keeps at most `limit` frames queued: os_audio_port_write blocks beyond that, the way the console's
// AudioOut blocks until the previous buffer has been taken. Whatever is queued here is heard that much later, so the limit
// starts at QUEUE_MS instead of the whole ring (which, with a 30 ms device buffer on top, put every sound about 110 ms behind
// the picture). It adapts: a gap that was heard (the port ran dry in non-silent audio while the game kept feeding it) raises
// the limit by the length of the gap plus two steps, so a pause of that length is covered from then on; every QUEUE_CALM_MS
// without such a gap lowers it by a step again, down to the starting value. --audio-queue-ms sets the starting value (85 =
// as much as the ring holds, as before). The device buffer is the smallest the audio engine offers for a shared stream
// unless --audio-device-ms asks for more.
// Pacing. The device takes a whole period at a time (10 ms), but the queue is counted as if it drained steadily: a buffer
// is accepted as soon as it would fit had the device taken its share of the current period already. The game's audio thread
// then gets a buffer accepted every 5.3 ms, as on the console, instead of two at once every device period; with those
// bursts a game thread that waits for the audio thread was held up for 11 or 22 ms every few seconds.
#define QUEUE_MS 20
#define QUEUE_STEP 240                 // 5 ms at 48 kHz
#define QUEUE_CALM_MS 10000
// After a port runs dry while the game is still feeding it, it waits until PREROLL frames are queued before playing again
// (one short gap instead of a run of tiny ones, which sounds scratchy). A port whose feed stopped plays out what it has.
#define PREROLL 960                    // 20 ms at 48 kHz (or the port's limit, if that is lower)
// Output delay measurement: every write leaves a mark (its first frame's number and the time of the call). When the render
// thread hands that frame to the device it knows how many frames are still ahead of it there, hence when it will be played.
#define MARKS 64
typedef struct {
    int used; float buf[RING * 2]; unsigned rd, wr; unsigned count; uint64_t last_data_ms; int refilling;
    unsigned limit;                    // frames this port may keep queued (see above)
    uint64_t last_sound_ms;            // last write that carried non-silent audio
    int64_t dry_qpc;                   // when the port ran dry while it was being fed (0: not since the last write)
    uint64_t calm_since_ms;            // start of the current stretch without a gap that was heard
    uint64_t written, taken;           // frames ever queued / ever handed to the device
    struct { uint64_t frame; int64_t qpc; } mark[MARKS]; unsigned mark_rd, mark_wr;
} Port;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out here to avoid depending on ksmedia.h's conditional GUID definitions
static const GUID k_subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static Port g_port[PORTS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_state;                     // 0 not tried, 1 running, -1 failed
static OsAudioStats g_stats;
static HANDLE g_event;
static IAudioClient* g_client; static IAudioRenderClient* g_render; static UINT32 g_bufframes;
static unsigned g_queue_frames = QUEUE_MS * 48; // a port's starting limit (os_audio_configure; raised to period + 256 at start)
static int g_device_ms;                         // device buffer to ask for; 0 = the smallest the audio engine offers
static unsigned g_period_frames = 480;          // how much the audio engine takes from the device buffer at a time
static unsigned g_queue_max = RING - 480;       // highest limit: the ring also holds the period the device has not taken yet
static int64_t g_pull_qpc;                      // when the render thread last took frames from the ports
static int64_t g_qpc_hz = 10000000;
static uint64_t g_delay_sum_us, g_delay_n, g_delay_max_us, g_delay_window_ms; // output delay of the current 5 s window

// Sleeps on a high-resolution timer (Windows 10 1803 and later). Unlike Sleep() or a timed wait it does not depend on the
// timer resolution Windows grants the process, which is coarse while the game window is covered. Falls back to Sleep().
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static void sleep_precise(uint64_t ns) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(ns >= 100 ? ns / 100 : 1);
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)(ns >= 1000000 ? ns / 1000000 : 1));
    if (timer) CloseHandle(timer);
}

static void* render_thread(void* unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // Real-time scheduling for the device feed: MMCSS "Pro Audio" (avrt.dll), else a time-critical thread priority. Without it
    // a busy CPU (game threads, shader compilation) can delay this thread past the device's period: an audible click.
    {
        HMODULE avrt = LoadLibraryW(L"avrt.dll");
        typedef HANDLE(WINAPI * SetMm)(LPCWSTR, LPDWORD);
        SetMm set = avrt ? (SetMm)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
        DWORD task = 0;
        g_stats.realtime = set && set(L"Pro Audio", &task) ? 2 : SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) ? 1 : 0;
    }
    for (;;) {
        if (WaitForSingleObject(g_event, 200) == WAIT_FAILED) break;
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(g_client, &pad))) continue;
        UINT32 n = g_bufframes - pad;
        if (!n) continue;
        BYTE* data = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(g_render, n, &data))) continue;
        float* out = (float*)data;
        memset(out, 0, (size_t)n * 2 * sizeof(float));
        uint64_t now = GetTickCount64();
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        pthread_mutex_lock(&g_mu);
        for (int p = 0; p < PORTS; ++p) {
            Port* pt = &g_port[p];
            if (!pt->used) continue;
            const int feeding = now - pt->last_data_ms < 50;
            const unsigned preroll = pt->limit < PREROLL ? pt->limit : PREROLL;
            if (pt->refilling && (pt->count >= preroll || !feeding)) pt->refilling = 0;
            if (pt->refilling) continue;
            unsigned take = pt->count < n ? pt->count : n;
            for (unsigned i = 0; i < take; ++i) {
                unsigned r = (pt->rd + i) % RING;
                out[i * 2] += pt->buf[r * 2]; out[i * 2 + 1] += pt->buf[r * 2 + 1];
            }
            // Output delay of the marked frames in this batch: `pad` frames are still ahead of the batch in the device buffer.
            while (pt->mark_rd != pt->mark_wr && pt->mark[pt->mark_rd % MARKS].frame < pt->taken + take) {
                const uint64_t frame = pt->mark[pt->mark_rd % MARKS].frame;
                const int64_t called = pt->mark[pt->mark_rd % MARKS].qpc;
                pt->mark_rd++;
                if (frame < pt->taken) continue;
                const uint64_t us = (uint64_t)((qpc.QuadPart - called) * 1000000 / g_qpc_hz) + ((uint64_t)pad + (frame - pt->taken)) * 1000000 / 48000;
                g_delay_sum_us += us; g_delay_n++;
                if (us > g_delay_max_us) g_delay_max_us = us;
            }
            pt->rd = (pt->rd + take) % RING; pt->count -= take; pt->taken += take;
            if (take < n && feeding) { // an active port had too little queued
                g_stats.underruns++; pt->refilling = 1;
                if (!pt->dry_qpc) pt->dry_qpc = qpc.QuadPart;
            } else if (pt->limit > g_queue_frames && now - pt->calm_since_ms >= QUEUE_CALM_MS) { // no gap for a while: try a shorter queue again
                pt->limit = pt->limit - g_queue_frames > QUEUE_STEP ? pt->limit - QUEUE_STEP : g_queue_frames;
                pt->calm_since_ms = now;
            }
        }
        g_stats.frames_rendered += n;
        if (now - g_delay_window_ms >= 5000) { // publish the output delay of the last five seconds
            g_stats.delay_avg_us = g_delay_n ? (unsigned)(g_delay_sum_us / g_delay_n) : 0;
            g_stats.delay_max_us = (unsigned)g_delay_max_us;
            g_delay_sum_us = g_delay_n = g_delay_max_us = 0; g_delay_window_ms = now;
        }
        g_pull_qpc = qpc.QuadPart;
        pthread_mutex_unlock(&g_mu);
        for (UINT32 i = 0; i < n * 2; ++i) { float v = out[i]; out[i] = v > 1.f ? 1.f : v < -1.f ? -1.f : v; }
        IAudioRenderClient_ReleaseBuffer(g_render, n, 0);
    }
    return NULL;
}

int os_audio_start(void) {
    pthread_mutex_lock(&g_mu);
    if (g_state) { int r = g_state > 0 ? 0 : -1; pthread_mutex_unlock(&g_mu); return r; }
    g_state = -1;
    IMMDeviceEnumerator* en = NULL; IMMDevice* dev = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) goto done;
    IPropertyStore* ps = NULL;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, g_stats.device, (int)sizeof g_stats.device, NULL, NULL);
        PropVariantClear(&v); IPropertyStore_Release(ps);
    }
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&g_client))) goto done;
    WAVEFORMATEXTENSIBLE wf; memset(&wf, 0, sizeof wf);
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; wf.Format.nChannels = 2; wf.Format.nSamplesPerSec = 48000;
    wf.Format.wBitsPerSample = 32; wf.Format.nBlockAlign = 8; wf.Format.nAvgBytesPerSec = 48000 * 8; wf.Format.cbSize = 22;
    wf.Samples.wValidBitsPerSample = 32; wf.dwChannelMask = 0x3; /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
    wf.SubFormat = k_subtype_float;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    // A buffer duration of 0 asks for the smallest buffer the audio engine offers for a shared, event-driven stream (about two
    // device periods, 22 ms, on current Windows). The render thread keeps the buffer full, so its size is output delay.
    if (FAILED(IAudioClient_Initialize(g_client, AUDCLNT_SHAREMODE_SHARED, flags, (REFERENCE_TIME)g_device_ms * 10000, 0, &wf.Format, NULL))) goto done;
    g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_event || FAILED(IAudioClient_SetEventHandle(g_client, g_event))) goto done;
    if (FAILED(IAudioClient_GetBufferSize(g_client, &g_bufframes))) goto done;
    { // The engine takes one device period at a time: a port's queue has to cover that plus a console buffer still being written.
        REFERENCE_TIME period = 0; LARGE_INTEGER hz;
        if (SUCCEEDED(IAudioClient_GetDevicePeriod(g_client, &period, NULL)) && period > 0) g_period_frames = (unsigned)(period * 48000 / 10000000);
        g_queue_max = g_period_frames + 512 < RING ? RING - g_period_frames : RING / 2;
        if (g_queue_frames < g_period_frames + 256) g_queue_frames = g_period_frames + 256;
        if (g_queue_frames > g_queue_max) g_queue_frames = g_queue_max;
        if (QueryPerformanceFrequency(&hz) && hz.QuadPart > 0) g_qpc_hz = hz.QuadPart;
        g_stats.device_frames = g_bufframes;
    }
    if (FAILED(IAudioClient_GetService(g_client, &IID_IAudioRenderClient, (void**)&g_render))) goto done;
    { // pre-fill with silence so the first period does not underrun
        BYTE* d = NULL;
        if (SUCCEEDED(IAudioRenderClient_GetBuffer(g_render, g_bufframes, &d))) IAudioRenderClient_ReleaseBuffer(g_render, g_bufframes, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    pthread_t th;
    if (pthread_create(&th, NULL, render_thread, NULL)) goto done;
    pthread_detach(th);
    if (FAILED(IAudioClient_Start(g_client))) goto done;
    g_state = 1;
done:
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    int ok = g_state > 0;
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}
int os_audio_port_new(void) {
    pthread_mutex_lock(&g_mu);
    int id = -1;
    for (int i = 0; i < PORTS; ++i) if (!g_port[i].used) {
        memset(&g_port[i], 0, sizeof g_port[i]);
        g_port[i].used = 1; g_port[i].limit = g_queue_frames; g_port[i].calm_since_ms = GetTickCount64();
        id = i; break;
    }
    pthread_mutex_unlock(&g_mu);
    return id;
}
void os_audio_port_free(int port) {
    if (port < 0 || port >= PORTS) return;
    pthread_mutex_lock(&g_mu); g_port[port].used = 0; pthread_mutex_unlock(&g_mu);
}
int os_audio_port_write(int port, const float* stereo, unsigned frames) {
    if (port < 0 || port >= PORTS || g_state <= 0 || !frames || frames > RING / 2) return -1;
    LARGE_INTEGER called; QueryPerformanceCounter(&called); // the output delay counts from here: waiting for room is part of it
    int sound = 0; // anything above -80 dBFS: a gap in it would be heard
    for (unsigned i = 0; i < frames * 2 && !sound; ++i) sound = stereo[i] > 1e-4f || stereo[i] < -1e-4f;
    pthread_mutex_lock(&g_mu);
    Port* pt = &g_port[port];
    for (;;) { // wait until the buffer fits (see "Pacing" above)
        if (!pt->used) { pthread_mutex_unlock(&g_mu); return -1; }
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        uint64_t drained = g_pull_qpc ? (uint64_t)((t.QuadPart - g_pull_qpc) * 48000 / g_qpc_hz) : g_period_frames; // the device's share of the current period
        if (drained > g_period_frames) drained = g_period_frames;
        const unsigned queued = pt->count > drained ? pt->count - (unsigned)drained : 0;
        const unsigned limit = pt->limit > frames + g_period_frames ? pt->limit : frames + g_period_frames; // a large console buffer needs more
        if (!pt->count || queued + frames <= limit) break;
        uint64_t wait_ns = (uint64_t)(queued + frames - limit) * 1000000000ull / 48000;
        if (wait_ns > 5000000) wait_ns = 5000000; // then look again: the limit may have changed or the port been closed
        if (wait_ns < 500000) wait_ns = 500000;
        pthread_mutex_unlock(&g_mu);
        sleep_precise(wait_ns);
        pthread_mutex_lock(&g_mu);
    }
    const uint64_t now = GetTickCount64();
    // The port ran dry a moment ago although the game kept feeding it, and what it was playing was not silence: that gap was
    // heard. The queue was too short for a pause of that length, so keep that much more queued from now on, and two steps.
    if (pt->dry_qpc) {
        const int64_t dry_ms = (called.QuadPart - pt->dry_qpc) * 1000 / g_qpc_hz;
        if (dry_ms < 200 && pt->last_sound_ms && now - pt->last_sound_ms < 700) {
            const unsigned raise = ((unsigned)(dry_ms > 0 ? dry_ms : 0) / 5 + 2) * QUEUE_STEP;
            pt->limit = pt->limit + raise > g_queue_max ? g_queue_max : pt->limit + raise;
            g_stats.queue_raised++;
            pt->calm_since_ms = now;
        }
        pt->dry_qpc = 0;
    }
    if (pt->mark_wr - pt->mark_rd < MARKS) {
        pt->mark[pt->mark_wr % MARKS].frame = pt->written; pt->mark[pt->mark_wr % MARKS].qpc = called.QuadPart;
        pt->mark_wr++;
    }
    if (frames > RING - pt->count) frames = RING - pt->count; // cannot happen: the limit leaves room for a device period
    for (unsigned i = 0; i < frames; ++i) {
        unsigned w = (pt->wr + i) % RING;
        pt->buf[w * 2] = stereo[i * 2]; pt->buf[w * 2 + 1] = stereo[i * 2 + 1];
    }
    pt->wr = (pt->wr + frames) % RING; pt->count += frames; pt->written += frames;
    pt->last_data_ms = now;
    if (sound) pt->last_sound_ms = now;
    g_stats.frames_submitted += frames;
    pthread_mutex_unlock(&g_mu);
    return (int)frames;
}
void os_audio_stats(OsAudioStats* out) {
    pthread_mutex_lock(&g_mu);
    *out = g_stats;
    out->ports_open = 0; out->queue_frames = 0;
    for (int i = 0; i < PORTS; ++i) {
        out->ports_open += g_port[i].used;
        if (g_port[i].used && g_port[i].limit > out->queue_frames) out->queue_frames = g_port[i].limit;
    }
    if (!out->queue_frames) out->queue_frames = g_queue_frames;
    pthread_mutex_unlock(&g_mu);
}
void os_audio_configure(int queue_ms, int device_ms) {
    if (queue_ms > 0) g_queue_frames = (unsigned)queue_ms * 48 > RING ? RING : (unsigned)queue_ms * 48;
    if (device_ms > 0) g_device_ms = device_ms > 200 ? 200 : device_ms;
}
#else
void os_audio_configure(int queue_ms, int device_ms) { (void)queue_ms; (void)device_ms; }
int os_audio_start(void) { return -1; }
int os_audio_port_new(void) { return -1; }
void os_audio_port_free(int port) { (void)port; }
int os_audio_port_write(int port, const float* stereo, unsigned frames) { (void)port; (void)stereo; (void)frames; return -1; }
void os_audio_stats(OsAudioStats* out) { memset(out, 0, sizeof *out); }
#endif
