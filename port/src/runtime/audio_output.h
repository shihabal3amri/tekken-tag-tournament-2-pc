#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

inline constexpr size_t AUDIO_PCM_CHANNELS = 6;
inline constexpr size_t AUDIO_PCM_SAMPLES = 256;
inline constexpr size_t AUDIO_PCM_VALUES = AUDIO_PCM_CHANNELS * AUDIO_PCM_SAMPLES;
inline constexpr size_t AUDIO_PCM_BYTES = AUDIO_PCM_VALUES * sizeof(float);
inline constexpr size_t AUDIO_OUTPUT_QUEUE_FRAMES = 16;
inline constexpr size_t AUDIO_OUTPUT_PRIME_FRAMES = 8;
inline constexpr uint64_t AUDIO_PCM_FRAME_NS = 1000000000ull * AUDIO_PCM_SAMPLES / 48000;

struct AudioPcmConversion
{
    bool nonzero = false;
    uint32_t nonfinite = 0;
};

// Render-driver frames are BE float32 planes: FL, FR, FC, LFE, BL, BR.
// Preserve finite samples; replace NaN/Inf with silence before device playback.
AudioPcmConversion ConvertAudioPcm(const uint8_t* guest,
                                  std::array<float, AUDIO_PCM_VALUES>& interleaved);

struct AudioOutputStats
{
    uint64_t received = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t dropped = 0;
    uint64_t nonzero = 0;
    uint64_t nonfinite = 0;
    uint64_t starvationPasses = 0;
    uint64_t starvationBytes = 0;
    uint32_t queued = 0;
    uint32_t deviceQueued = 0;
    uint32_t renderCredits = 0;
    bool ready = false;
    bool started = false;
    bool failed = false;
};

struct AudioOutputState;

// The output worker owns all device calls. Submit copies synchronously; Close
// waits for DestroyVoice to finish before releasing storage used by callbacks.
class AudioOutputClient
{
public:
    explicit AudioOutputClient(uint32_t driver);
    ~AudioOutputClient();
    AudioOutputClient(const AudioOutputClient&) = delete;
    AudioOutputClient& operator=(const AudioOutputClient&) = delete;
    void Submit(const uint8_t* guest);
    // One initial prefill credit per frame, then one credit per device buffer
    // completion. Failed devices fall back to a monotonic 48kHz silent clock.
    bool TryAcquireRenderCredit(uint64_t nowNs);
    void Close();
    AudioOutputStats Stats() const;

private:
    std::shared_ptr<AudioOutputState> state_;
};

uint64_t AudioOutputWakeVersion();
void WaitAudioOutputWake(uint64_t version, uint64_t timeoutNs);
