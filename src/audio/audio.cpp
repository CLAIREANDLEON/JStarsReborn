#include "reborn/audio.hpp"
#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
namespace reborn {
namespace {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) { std::ostringstream out; out << operation << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(hr); throw std::runtime_error(out.str()); }
}
}
struct AudioSystem::Impl {
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    std::array<int16_t, 4800> samples{};
    ~Impl() { if (source) source->DestroyVoice(); if (master) master->DestroyVoice(); }
};
AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() = default;
void AudioSystem::initialize(float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1) throw std::invalid_argument("Audio volume must be in 0..1");
    if (impl_) throw std::logic_error("Audio already initialized");
    auto state = std::make_unique<Impl>();
    check(XAudio2Create(&state->engine, 0, XAUDIO2_DEFAULT_PROCESSOR), "XAudio2Create");
    check(state->engine->CreateMasteringVoice(&state->master), "Create mastering voice");
    check(state->master->SetVolume(volume), "Set audio volume");
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1; format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16; format.nBlockAlign = 2; format.nAvgBytesPerSec = 96000;
    check(state->engine->CreateSourceVoice(&state->source, &format), "Create test voice");
    for (size_t i=0; i<state->samples.size(); ++i) {
        const double envelope = std::min(1.0, static_cast<double>(i)/240.0) * std::min(1.0, static_cast<double>(state->samples.size()-i)/480.0);
        state->samples[i] = static_cast<int16_t>(4000 * envelope * std::sin(2*3.141592653589793*440*static_cast<double>(i)/48000));
    }
    impl_ = std::move(state);
}
void AudioSystem::play_test_tone() {
    if (!impl_) return;
    check(impl_->source->Stop(), "Stop test voice");
    check(impl_->source->FlushSourceBuffers(), "Flush test voice");
    XAUDIO2_BUFFER buffer{}; buffer.AudioBytes = static_cast<UINT32>(impl_->samples.size()*sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(impl_->samples.data()); buffer.Flags=XAUDIO2_END_OF_STREAM;
    check(impl_->source->SubmitSourceBuffer(&buffer), "Submit test tone");
    check(impl_->source->Start(), "Start test tone");
}
bool AudioSystem::available() const noexcept { return impl_ != nullptr; }
}
