// JStarsReborn: the file-handle table surface the vendored event queue code needs. Event queues are
// kernel objects that share the descriptor number space with files, so ids come from the runtime's
// descriptor allocator (JgpuHost.fd_alloc/fd_free).
#pragma once
#include <mutex>
#include <unordered_map>
#include "common/singleton.h"
#include "common/types.h"

extern "C" int jgpu_fd_alloc(void);
extern "C" void jgpu_fd_free(int fd);

namespace Core::FileSys {
enum class FileType : u32 { Regular, Directory, Device, Socket, Equeue };
struct File { FileType type{FileType::Regular}; };

class HandleTable {
public:
    s32 CreateHandle() {
        const s32 fd = jgpu_fd_alloc();
        std::scoped_lock lk{m};
        files[fd] = File{};
        return fd;
    }
    void DeleteHandle(s32 fd) {
        { std::scoped_lock lk{m}; files.erase(fd); }
        jgpu_fd_free(fd);
    }
    File* GetFile(s32 fd) {
        std::scoped_lock lk{m};
        auto it = files.find(fd);
        return it == files.end() ? nullptr : &it->second;
    }

private:
    std::mutex m;
    std::unordered_map<s32, File> files; // node-based: pointers stay valid
};
} // namespace Core::FileSys
