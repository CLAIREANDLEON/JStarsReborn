#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace reborn {

// Native rendering foundation only. No proprietary geometry, shaders, or game
// behavior is included. All methods must be called on the window's owner thread.
class Renderer final {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void initialize(HWND hwnd, std::uint32_t width, std::uint32_t height,
                    bool warp, bool validation);
    void resize(std::uint32_t width, std::uint32_t height);
    void render(bool vsync);
    // Explicit normal shutdown checkpoint: propagates fence/device errors so
    // the application can persist them before the noexcept destructor runs.
    void wait_idle();
    void shutdown() noexcept;
    [[nodiscard]] std::string adapter_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace reborn
