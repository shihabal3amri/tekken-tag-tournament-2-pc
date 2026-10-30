#include "audio_output.h"
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

AudioPcmConversion ConvertAudioPcm(const uint8_t* guest,
                                  std::array<float, AUDIO_PCM_VALUES>& interleaved)
{
    AudioPcmConversion result;
    for (size_t sample = 0; sample < AUDIO_PCM_SAMPLES; ++sample)
        for (size_t channel = 0; channel < AUDIO_PCM_CHANNELS; ++channel)
        {
            const uint8_t* p = guest + (channel * AUDIO_PCM_SAMPLES + sample) * 4;
            const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                  (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            float value = std::bit_cast<float>(bits);
            if (!std::isfinite(value)) { value = 0; ++result.nonfinite; }
            result.nonzero |= value != 0;
            interleaved[sample * AUDIO_PCM_CHANNELS + channel] = value;
        }
    return result;
}

namespace {
struct RenderWake
{
    std::mutex mutex;
    std::condition_variable event;
    uint64_t version = 0;
};
RenderWake& RenderSignal() { static auto* signal = new RenderWake; return *signal; }
void NotifyRender()
{
    auto& signal = RenderSignal();
    { std::lock_guard lock(signal.mutex); ++signal.version; }
    signal.event.notify_one();
}
void WakeOutputWorker();

enum class FramePhase { Free, Pending, InFlight };
struct OutputFrame
{
    std::array<float, AUDIO_PCM_VALUES> samples{};
    FramePhase phase = FramePhase::Free;
};

struct VoiceEvents final : IXAudio2VoiceCallback
{
    explicit VoiceEvents(AudioOutputState* owner) : owner(owner) {}
    AudioOutputState* owner;
    std::atomic<HRESULT> error{S_OK};
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) override;
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { error.store(hr); WakeOutputWorker(); }
};
} // namespace

struct AudioOutputState
{
    explicit AudioOutputState(uint32_t id) : driver(id), events(this) {}
    mutable std::mutex mutex;
    std::condition_variable closedEvent;
    const uint32_t driver;
    bool closing = false;
    bool closed = false;
    bool initialized = false;                 // worker only
    uint32_t renderCredits = AUDIO_OUTPUT_PRIME_FRAMES;
    uint64_t fallbackNextNs = 0;
    IXAudio2SourceVoice* voice = nullptr;       // worker only
    VoiceEvents events;
    std::array<OutputFrame, AUDIO_OUTPUT_QUEUE_FRAMES> frames;
    std::deque<size_t> pending;
    AudioOutputStats stats;
    std::FILE* dump = nullptr;                 // TTT2_AUDIO_DUMP capture (protected by mutex)
    ~AudioOutputState() { if (dump) std::fclose(dump); }
};

// Diagnostic capture of what each render client submits: TTT2_AUDIO_DUMP=<path prefix> writes
// <prefix>_<driver>.f32, raw little-endian float32, 6 interleaved channels, 48 kHz.
static std::FILE* OpenAudioDump(uint32_t driver)
{
    const char* prefix = std::getenv("TTT2_AUDIO_DUMP");
    if (!prefix || !*prefix) return nullptr;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s_%08X.f32", prefix, driver);
    std::FILE* f = std::fopen(path, "wb");
    if (f) std::fprintf(stderr, "[snd-output] capturing driver %08X to %s\n", driver, path);
    return f;
}

namespace {
void VoiceEvents::OnVoiceProcessingPassStart(UINT32 bytesRequired)
{
    if (!bytesRequired) return;
    std::lock_guard lock(owner->mutex);
    if (owner->stats.started && !owner->closing && !owner->stats.failed)
    {
        ++owner->stats.starvationPasses;
        owner->stats.starvationBytes += bytesRequired;
    }
}

void VoiceEvents::OnBufferEnd(void* context)
{
    // This is a device thread: no guest calls, logging, allocation or device API.
    auto* frame = static_cast<OutputFrame*>(context);
    {
        std::lock_guard lock(owner->mutex);
        if (frame->phase == FramePhase::InFlight)
        {
            frame->phase = FramePhase::Free;
            --owner->stats.queued;
            --owner->stats.deviceQueued;
            ++owner->stats.completed;
            owner->renderCredits = std::min<uint32_t>(owner->renderCredits + 1, AUDIO_OUTPUT_PRIME_FRAMES);
        }
    }
    NotifyRender();
}

void DiscardFrames(AudioOutputState& state)
{
    for (auto& frame : state.frames)
        if (frame.phase != FramePhase::Free)
        {
            frame.phase = FramePhase::Free;
            ++state.stats.dropped;
        }
    state.pending.clear();
    state.stats.queued = 0;
    state.stats.deviceQueued = 0;
}

class AudioOutputSystem
{
public:
    AudioOutputSystem() { std::thread([this] { Run(); }).detach(); }

    void Add(const std::shared_ptr<AudioOutputState>& state)
    {
        { std::lock_guard lock(mutex_); clients_.push_back(state); }
        Wake();
        NotifyRender();
    }
    void Wake() { workVersion_.fetch_add(1); workVersion_.notify_one(); }

private:
    void Initialize()
    {
        if (const char* e = std::getenv("TTT2_AUDIO_SILENT"); e && *e == '1')
        {
            std::fprintf(stderr, "[snd-output] TTT2_AUDIO_SILENT=1: no XAudio2, silent callback pacing\n");
            return;                                     // diagnostics: no audio device at all
        }
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const char* operation = "CoInitializeEx";
        if (SUCCEEDED(hr)) { operation = "XAudio2Create"; hr = XAudio2Create(&audio_); }
        if (SUCCEEDED(hr))
        {
            operation = "CreateMasteringVoice";
            hr = audio_->CreateMasteringVoice(&master_);
        }
        if (FAILED(hr))
        {
            std::fprintf(stderr, "[snd-output] %s failed hr=%08X; continuing silent callback pacing\n",
                         operation, unsigned(hr));
            if (audio_) { audio_->Release(); audio_ = nullptr; }
            return;
        }
        std::fprintf(stderr, "[snd-output] XAudio2 ready: 48000Hz float32, 6 channels, queue=%zu prime=%zu frames/client, completion pacing\n",
                     AUDIO_OUTPUT_QUEUE_FRAMES, AUDIO_OUTPUT_PRIME_FRAMES);
    }

    void DestroySource(AudioOutputState& state)
    {
        // DestroyVoice waits for outstanding device callbacks. Never hold the
        // state mutex here: OnBufferEnd may be running or waiting to take it.
        if (state.voice) { state.voice->DestroyVoice(); state.voice = nullptr; }
    }

    void Fail(AudioOutputState& state, const char* operation, HRESULT hr)
    {
        std::fprintf(stderr, "[snd-output] driver=%08X %s failed hr=%08X; continuing silent callback pacing\n",
                     state.driver, operation, unsigned(hr));
        DestroySource(state);
        std::lock_guard lock(state.mutex);
        state.stats.ready = false;
        state.stats.started = false;
        state.stats.failed = true;
        DiscardFrames(state);
        NotifyRender();
    }

    void Process(AudioOutputState& state)
    {
        bool closing;
        { std::lock_guard lock(state.mutex); closing = state.closing; }
        if (closing)
        {
            DestroySource(state);
            std::lock_guard lock(state.mutex);
            DiscardFrames(state);
            state.stats.ready = false;
            state.stats.started = false;
            state.closed = true;
            state.closedEvent.notify_all();
            return;
        }
        if (!state.initialized)
        {
            state.initialized = true;
            if (!audio_)
            {
                std::lock_guard lock(state.mutex);
                state.stats.failed = true;
                DiscardFrames(state);
                NotifyRender();
                return;
            }
            WAVEFORMATEXTENSIBLE format{};
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.nChannels = AUDIO_PCM_CHANNELS;
            format.Format.nSamplesPerSec = 48000;
            format.Format.wBitsPerSample = 32;
            format.Format.nBlockAlign = AUDIO_PCM_CHANNELS * sizeof(float);
            format.Format.nAvgBytesPerSec = 48000 * format.Format.nBlockAlign;
            format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
            format.Samples.wValidBitsPerSample = 32;
            format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, without an additional GUID library.
            format.SubFormat = {WAVE_FORMAT_IEEE_FLOAT, 0, 0x0010,
                                {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
            HRESULT hr = audio_->CreateSourceVoice(&state.voice, &format.Format, 0,
                                                   XAUDIO2_DEFAULT_FREQ_RATIO, &state.events);
            if (FAILED(hr)) { Fail(state, "CreateSourceVoice", hr); return; }
            { std::lock_guard lock(state.mutex); state.stats.ready = true; }
        }
        const HRESULT error = state.events.error.exchange(S_OK);
        if (FAILED(error)) { Fail(state, "OnVoiceError", error); return; }
        for (;;)
        {
            OutputFrame* frame;
            {
                std::lock_guard lock(state.mutex);
                if (state.closing || state.stats.failed) return;
                if (state.pending.empty()) break;
                frame = &state.frames[state.pending.front()];
                state.pending.pop_front();
                frame->phase = FramePhase::InFlight;
                ++state.stats.deviceQueued;
            }
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = AUDIO_PCM_BYTES;
            buffer.pAudioData = reinterpret_cast<const BYTE*>(frame->samples.data());
            buffer.PlayLength = AUDIO_PCM_SAMPLES;
            buffer.pContext = frame;
            // No mutex across an API call that may trigger OnBufferEnd.
            const HRESULT hr = state.voice->SubmitSourceBuffer(&buffer);
            if (FAILED(hr)) { Fail(state, "SubmitSourceBuffer", hr); return; }
            { std::lock_guard lock(state.mutex); ++state.stats.submitted; }
        }
        bool start;
        {
            std::lock_guard lock(state.mutex);
            start = !state.closing && !state.stats.started &&
                    state.stats.deviceQueued >= AUDIO_OUTPUT_PRIME_FRAMES;
        }
        if (start)
        {
            const HRESULT hr = state.voice->Start();
            if (FAILED(hr)) { Fail(state, "Start", hr); return; }
            std::lock_guard lock(state.mutex);
            state.stats.started = true;
        }
    }

    void Run()
    {
        Initialize();
        for (;;)
        {
            const uint64_t version = workVersion_.load();
            std::vector<std::shared_ptr<AudioOutputState>> clients;
            { std::lock_guard lock(mutex_); clients = clients_; }
            for (const auto& client : clients) Process(*client);
            {
                std::lock_guard lock(mutex_);
                std::erase_if(clients_, [](const auto& state) {
                    std::lock_guard stateLock(state->mutex); return state->closed;
                });
            }
            // Notifications received during processing remain visible, so a
            // submit cannot lose its wake and wait another whole audio quantum.
            workVersion_.wait(version);
        }
    }

    IXAudio2* audio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    std::mutex mutex_;
    std::atomic<uint64_t> workVersion_{0};
    std::vector<std::shared_ptr<AudioOutputState>> clients_;
};

AudioOutputSystem& OutputSystem()
{
    // Like the existing detached guest callback pump, the device worker lives
    // until process exit. Avoid static destruction racing that live thread.
    static auto* system = new AudioOutputSystem;
    return *system;
}
void WakeOutputWorker() { OutputSystem().Wake(); }
} // namespace

uint64_t AudioOutputWakeVersion()
{
    auto& signal = RenderSignal();
    std::lock_guard lock(signal.mutex);
    return signal.version;
}

void WaitAudioOutputWake(uint64_t version, uint64_t timeoutNs)
{
    auto& signal = RenderSignal();
    std::unique_lock lock(signal.mutex);
    signal.event.wait_for(lock, std::chrono::nanoseconds(timeoutNs),
                         [&] { return signal.version != version; });
}

AudioOutputClient::AudioOutputClient(uint32_t driver)
    : state_(std::make_shared<AudioOutputState>(driver))
{
    state_->dump = OpenAudioDump(driver);
    OutputSystem().Add(state_);
}

AudioOutputClient::~AudioOutputClient()
{
    Close();
}

void AudioOutputClient::Close()
{
    std::unique_lock lock(state_->mutex);
    state_->closing = true;
    OutputSystem().Wake();
    state_->closedEvent.wait(lock, [this] { return state_->closed; });
}

void AudioOutputClient::Submit(const uint8_t* guest)
{
    {
        std::lock_guard lock(state_->mutex);
        ++state_->stats.received;
        if (state_->closing || state_->stats.failed) { ++state_->stats.dropped; return; }
        auto free = std::find_if(state_->frames.begin(), state_->frames.end(),
                                [](const auto& frame) { return frame.phase == FramePhase::Free; });
        if (free == state_->frames.end()) { ++state_->stats.dropped; return; }
        const auto conversion = ConvertAudioPcm(guest, free->samples);
        if (state_->dump) std::fwrite(free->samples.data(), sizeof(float), free->samples.size(), state_->dump);
        state_->stats.nonzero += conversion.nonzero;
        state_->stats.nonfinite += conversion.nonfinite;
        free->phase = FramePhase::Pending;
        state_->pending.push_back(size_t(free - state_->frames.begin()));
        ++state_->stats.queued;
    }
    OutputSystem().Wake();
}

bool AudioOutputClient::TryAcquireRenderCredit(uint64_t nowNs)
{
    std::lock_guard lock(state_->mutex);
    if (state_->closing) return false;
    if (state_->stats.failed)
    {
        if (nowNs < state_->fallbackNextNs) return false;
        state_->fallbackNextNs = nowNs + AUDIO_PCM_FRAME_NS;
        return true;
    }
    if (!state_->renderCredits) return false;
    --state_->renderCredits;
    return true;
}

AudioOutputStats AudioOutputClient::Stats() const
{
    std::lock_guard lock(state_->mutex);
    auto result = state_->stats;
    result.renderCredits = state_->renderCredits;
    return result;
}
