// JStarsReborn GPU library: the few libkernel services the vendored code calls directly. Each one is
// answered by the runtime (the host) so the guest and the vendored code see one set of values.
// GPL-2.0-or-later.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "jgpu.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/kernel/kernel.h"

JgpuHost* jgpu_host_ptr();

namespace Libraries::Kernel {

// ---- errno --------------------------------------------------------------------------------------------
s32* PS4_SYSV_ABI __Error() {
    // No ASSERT here: GCC 13 for mingw-w64 miscompiles SysV-ABI functions that contain a cleanup landing pad.
    auto* host = jgpu_host_ptr();
    if (!host->errno_cell) host->fatal("the runtime did not provide an errno cell");
    return host->errno_cell();
}

// ---- time ---------------------------------------------------------------------------------------------
u64 PS4_SYSV_ABI sceKernelReadTsc() { return jgpu_host_ptr()->read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return jgpu_host_ptr()->tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return jgpu_host_ptr()->process_time_us(); }

// ---- process ------------------------------------------------------------------------------------------
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return jgpu_host_ptr()->is_neo_mode ? jgpu_host_ptr()->is_neo_mode() : 0; }

s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    const int v = jgpu_host_ptr()->compiled_sdk_version ? jgpu_host_ptr()->compiled_sdk_version() : -1;
    if (v < 0) {
        LOG_ERROR(Lib_Kernel, "compiled SDK version is not known; reporting EINVAL instead of inventing one");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *ver = v;
    return ORBIS_OK;
}

// ---- asynchronous requests (event-queue timers) -------------------------------------------------------
boost::asio::io_context io_context;
static std::mutex m_req;
static std::condition_variable cv_req;
static std::atomic<u32> requests{0};
static std::atomic<bool> stop_service{false};
static std::thread service_thread;

void KernelSignalRequest() {
    {
        std::unique_lock lock{m_req};
        ++requests;
    }
    cv_req.notify_one();
}

static void ServiceThread() {
    while (!stop_service) {
        {
            std::unique_lock lock{m_req};
            cv_req.wait(lock, [] { return requests != 0 || stop_service; });
        }
        if (stop_service) break;
        io_context.run();
        io_context.restart();
        requests = 0;
    }
}

void StartKernelService() {
    stop_service = false;
    service_thread = std::thread(ServiceThread);
}

void StopKernelService() {
    stop_service = true;
    cv_req.notify_all();
    io_context.stop();
    if (service_thread.joinable()) service_thread.join();
}

} // namespace Libraries::Kernel
