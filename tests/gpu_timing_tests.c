// Tests the actual GPU-enabled wrappers without a Vulkan device/game.
// Unused host/renderer dependencies abort if accidentally reached by this test.
#define JSTARS_GPU 1
#include "../runtime/jstars_gpu_host.c"
#include <assert.h>
#include <stdarg.h>
static int calls, submissions, fail_rate;
static uint64_t last_handle, last_rate;
static pthread_t caller;
void rt_logf(const char* fmt, ...) { (void)fmt; }
uint64_t rt_read_tsc(void) { static uint64_t t; return ++t; }
uint64_t rt_tsc_frequency(void) { return 1000; }
void* jgpu_resolve(const char* x) { (void)x;abort(); }
const char* jgpu_symbol_name(const char* x) { (void)x;abort(); }
void jgpu_mem_map(uintptr_t a,uint64_t b) { (void)a;(void)b;abort(); }
void jgpu_mem_unmap(uintptr_t a,uint64_t b) { (void)a;(void)b;abort(); }
void jgpu_set_host(const JgpuHost* x) { (void)x;abort(); }
int jgpu_init(const JgpuConfig* x) { (void)x;abort(); }
int jgpu_window_focused(void) { abort(); }
unsigned jgpu_symbol_count(void) { abort(); }
void jgpu_request_capture(int x) { (void)x;abort(); }
int jgpu_fsr_probe(void) { abort(); }
const char* jgpu_fsr_report(void) { abort(); }
void jgpu_get_stats(JgpuStats* x) { (void)x;abort(); }
int jgpu_handle_fault(void* a,uintptr_t b) { (void)a;(void)b;abort(); }
int os_protect(void* a,size_t b,int c) { (void)a;(void)b;(void)c;abort(); }
size_t os_safe_read(void* a,const void* b,size_t c) { (void)a;(void)b;(void)c;abort(); }
void os_set_fault_prehook(int (*x)(const OsFault*)) { (void)x;abort(); }
int rt_fd_alloc(void) { abort(); }
void rt_fd_free(int x) { (void)x;abort(); }
int* rt_errno_cell(void) { abort(); }
uint64_t rt_process_time_us(void) { abort(); }
int rt_compiled_sdk_version(void) { abort(); }
void rt_input_set_focus_hook(int (*x)(void)) { (void)x;abort(); }
static int64_t GUEST_ABI mock_rate(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    (void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;
    ++calls;last_handle=a;last_rate=b;caller=pthread_self();return fail_rate ? -1 : 0;
}
static int64_t GUEST_ABI mock_submit(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;++submissions;return 123;
}
static void submit(void) { assert(submit_flip_wrapper(0,0,0,0,0,0,0,0,0,0)==123); }
static int64_t rate(uint64_t h,uint64_t r) { return set_flip_rate_wrapper(h,r,0,0,0,0,0,0,0,0); }
static void reset(int value,int after) { calls=submissions=fail_rate=0;rt_gpu_timing_configure(0,value,after);g_real_set_flip_rate=mock_rate;g_real_submit_flip=mock_submit; }
static void* monitor(void* unused) { (void)unused;rt_gpu_timing_tick(270);return NULL; }
int main(void) {
    reset(0,270);assert(rate(7,1)==0);assert(last_rate==1);
    rt_gpu_timing_tick(269);submit();assert(calls==1);
    pthread_t t;assert(pthread_create(&t,NULL,monitor,NULL)==0);assert(pthread_join(t,NULL)==0);
    assert(calls==1);submit();assert(calls==2 && last_handle==7 && last_rate==0);
    assert(pthread_equal(caller,pthread_self()));assert(g_flip_override_active);
    submit();rt_gpu_timing_tick(280);submit();assert(calls==2);
    rate(7,2);assert(last_rate==0);
    reset(0,270);rt_gpu_timing_tick(270);submit();assert(calls==0);
    fail_rate=1;assert(rate(99,1)==-1);submit();assert(calls==1 && !g_flip_handle_known);
    fail_rate=0;rate(8,1);submit();assert(calls==3 && last_handle==8 && g_flip_override_active);
    reset(0,270);rate(7,1);fail_rate=1;rt_gpu_timing_tick(270);submit();
    assert(!g_flip_override_active && calls==2);submit();assert(calls==2);
    fail_rate=0;rate(7,1);assert(last_rate==1);
    reset(-1,270);rate(7,1);rt_gpu_timing_tick(270);submit();assert(calls==1 && last_rate==1);
    reset(0,0);rate(7,1);assert(last_rate==0);submit();assert(calls==1);
    puts("GPU timing wrapper scenarios passed: threshold, thread, handle, one-shot, failure, disabled, immediate");
    return 0;
}
