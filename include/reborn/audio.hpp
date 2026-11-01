#pragma once
#include <memory>
namespace reborn {
// XAudio2 device ownership and an original procedural diagnostic tone only.
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    void initialize(float volume);
    void play_test_tone();
    bool available() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
