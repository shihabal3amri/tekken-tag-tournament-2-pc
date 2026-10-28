#include "audio_output.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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
    AudioOutputState* owner = nullptr;
    std::array<float, AUDIO_PCM_VALUES> samples{};
    FramePhase phase = FramePhase::Free;
};
} // namespace

struct AudioOutputState
{
    explicit AudioOutputState(uint32_t id) : driver(id)
    {
        for (auto& frame : frames) frame.owner = this;
    }
    mutable std::mutex mutex;
    std::condition_variable closedEvent;
    const uint32_t driver;
    bool closing = false;
    bool closed = false;
    bool initialized = false;                // worker only
    std::atomic<bool> deviceError{false};
    uint32_t renderCredits = AUDIO_OUTPUT_PRIME_FRAMES;
    uint64_t fallbackNextNs = 0;
    SDL_AudioStream* stream = nullptr;       // worker owns creation and destruction
    std::array<OutputFrame, AUDIO_OUTPUT_QUEUE_FRAMES> frames;
    std::deque<size_t> pending;
    AudioOutputStats stats;
    uint64_t dumpFrames = 0, dumpLimit = 0;
    std::chrono::steady_clock::time_point nextCapturePoll{};
    std::FILE* dump = nullptr;               // TTT2_AUDIO_DUMP, protected by mutex
    ~AudioOutputState() { if (dump) std::fclose(dump); }
};

// TTT2_AUDIO_DUMP=<path prefix> writes the submitted 48 kHz, six-channel float32
// PCM for each guest render client before SDL resamples or mixes it.
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
void SDLCALL OnStreamDataComplete(void* userdata, const void*, int)
{
    // SDL will not read the frame after this callback, including during stream
    // destruction. Do not call the guest or a device API on the audio thread.
    auto* frame = static_cast<OutputFrame*>(userdata);
    auto& state = *frame->owner;
    {
        std::lock_guard lock(state.mutex);
        if (frame->phase == FramePhase::InFlight)
        {
            frame->phase = FramePhase::Free;
            --state.stats.queued;
            --state.stats.deviceQueued;
            if (state.closing || state.stats.failed)
                ++state.stats.dropped;
            else
            {
                ++state.stats.completed;
                state.renderCredits = std::min<uint32_t>(state.renderCredits + 1,
                                                         AUDIO_OUTPUT_PRIME_FRAMES);
            }
        }
    }
    NotifyRender();
}

void SDLCALL OnStreamGet(void* userdata, SDL_AudioStream* stream,
                         int additionalAmount, int)
{
    auto& state = *static_cast<AudioOutputState*>(userdata);
    int remaining = std::max(0, additionalAmount);
    while (remaining > 0)
    {
        OutputFrame* frame;
        {
            std::lock_guard lock(state.mutex);
            if (state.closing || state.stats.failed || state.pending.empty()) break;
            frame = &state.frames[state.pending.front()];
            state.pending.pop_front();
            frame->phase = FramePhase::InFlight;
            ++state.stats.deviceQueued;
        }
        // This get callback holds SDL's stream lock. SDL takes ownership of
        // the pointer until OnStreamDataComplete releases this frame.
        if (!SDL_PutAudioStreamDataNoCopy(stream, frame->samples.data(),
                                          int(AUDIO_PCM_BYTES),
                                          OnStreamDataComplete, frame))
        {
            {
                std::lock_guard lock(state.mutex);
                if (frame->phase == FramePhase::InFlight)
                {
                    frame->phase = FramePhase::Free;
                    --state.stats.queued;
                    --state.stats.deviceQueued;
                    ++state.stats.dropped;
                }
            }
            state.deviceError.store(true);
            WakeOutputWorker();
            break;
        }
        { std::lock_guard lock(state.mutex); ++state.stats.submitted; }
        remaining -= int(AUDIO_PCM_BYTES);
    }
    if (remaining > 0)
    {
        std::lock_guard lock(state.mutex);
        if (state.stats.started && !state.closing && !state.stats.failed)
        {
            ++state.stats.starvationPasses;
            state.stats.starvationBytes += uint32_t(remaining);
        }
    }
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
            std::fprintf(stderr, "[snd-output] TTT2_AUDIO_SILENT=1: no SDL device, silent callback pacing\n");
            return;
        }
        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        {
            std::fprintf(stderr, "[snd-output] SDL audio was not initialized on the host main thread; silent callback pacing\n");
            return;
        }
        // The guest submits six channels, but the default desktop sink is
        // commonly stereo. Ask SDL for stereo output and let its stream
        // convert the six-channel guest mix, as on the Windows build.
        const SDL_AudioSpec requested{SDL_AUDIO_F32, 2, 48000};
        device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &requested);
        if (!device_)
        {
            std::fprintf(stderr, "[snd-output] SDL_OpenAudioDevice: %s; silent callback pacing\n",
                         SDL_GetError());
            return;
        }
        // TTT2_AUDIO_MUTE=1 (tests on shared machines): the real device and its clock, at zero gain.
        // Unlike TTT2_AUDIO_SILENT the output still paces the mixer exactly as an audible run does.
        if (const char* e = std::getenv("TTT2_AUDIO_MUTE"); e && *e == '1')
        {
            if (SDL_SetAudioDeviceGain(device_, 0.0f))
                std::fprintf(stderr, "[snd-output] TTT2_AUDIO_MUTE=1: device opened at zero gain\n");
            else
                std::fprintf(stderr, "[snd-output] SDL_SetAudioDeviceGain: %s\n", SDL_GetError());
        }
        SDL_AudioSpec obtained{};
        int deviceFrames = 0;
        SDL_GetAudioDeviceFormat(device_, &obtained, &deviceFrames);
        std::fprintf(stderr, "[snd-output] device callback frames=%d\n", deviceFrames);
        std::fprintf(stderr, "[snd-output] SDL audio ready: guest 48000Hz float32 6ch, device %dHz %dch format=%u, queue=%zu prime=%zu frames/client\n",
                     obtained.freq, obtained.channels, unsigned(obtained.format),
                     AUDIO_OUTPUT_QUEUE_FRAMES, AUDIO_OUTPUT_PRIME_FRAMES);
    }

    void DestroyStream(AudioOutputState& state)
    {
        // SDL may call OnStreamDataComplete for pending no-copy buffers here.
        // Never hold state.mutex while destroying a stream.
        if (state.stream) { SDL_DestroyAudioStream(state.stream); state.stream = nullptr; }
    }

    void Fail(AudioOutputState& state, const char* operation)
    {
        std::fprintf(stderr, "[snd-output] driver=%08X %s failed: %s; silent callback pacing\n",
                     state.driver, operation, SDL_GetError());
        {
            std::lock_guard lock(state.mutex);
            state.stats.ready = false;
            state.stats.started = false;
            state.stats.failed = true;
        }
        DestroyStream(state);
        {
            std::lock_guard lock(state.mutex);
            DiscardFrames(state);
        }
        NotifyRender();
    }

    void Process(AudioOutputState& state)
    {
        bool closing;
        { std::lock_guard lock(state.mutex); closing = state.closing; }
        if (closing)
        {
            DestroyStream(state);
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
            if (!device_)
            {
                std::lock_guard lock(state.mutex);
                state.stats.failed = true;
                DiscardFrames(state);
                NotifyRender();
                return;
            }
            const SDL_AudioSpec source{SDL_AUDIO_F32, int(AUDIO_PCM_CHANNELS), 48000};
            state.stream = SDL_CreateAudioStream(&source, nullptr);
            if (!state.stream) { Fail(state, "SDL_CreateAudioStream"); return; }
            if (!SDL_SetAudioStreamGetCallback(state.stream, OnStreamGet, &state))
            { Fail(state, "SDL_SetAudioStreamGetCallback"); return; }
            { std::lock_guard lock(state.mutex); state.stats.ready = true; }
        }
        if (state.deviceError.exchange(false)) { Fail(state, "SDL_PutAudioStreamDataNoCopy"); return; }
        bool start;
        {
            std::lock_guard lock(state.mutex);
            start = !state.stats.failed && !state.stats.started &&
                    state.pending.size() >= AUDIO_OUTPUT_PRIME_FRAMES;
            if (start) state.stats.started = true;
        }
        if (start && !SDL_BindAudioStream(device_, state.stream))
            Fail(state, "SDL_BindAudioStream");
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
            workVersion_.wait(version);
        }
    }

    SDL_AudioDeviceID device_ = 0;
    std::mutex mutex_;
    std::atomic<uint64_t> workVersion_{0};
    std::vector<std::shared_ptr<AudioOutputState>> clients_;
};

AudioOutputSystem& OutputSystem()
{
    // The guest audio pump and device worker are detached for process lifetime.
    static auto* system = new AudioOutputSystem;
    return *system;
}
void WakeOutputWorker() { OutputSystem().Wake(); }
} // namespace

bool InitializeHostAudio()
{
    if (const char* e = std::getenv("TTT2_AUDIO_SILENT"); e && *e == '1') return true;
#if defined(__linux__) && !defined(__ANDROID__)
    // Some PipeWire setups pace this game's SDL stream incorrectly. Prefer
    // the PulseAudio compatibility server when it is actually reachable;
    // leave SDL's normal driver choice in place on other Linux systems.
    if (!std::getenv("SDL_AUDIODRIVER") &&
        !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
    {
        bool hasPulseDriver = false;
        for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i)
            if (const char* name = SDL_GetAudioDriver(i);
                name && std::strcmp(name, "pulseaudio") == 0)
                hasPulseDriver = true;
        bool pulseReachable = false;
        if (const char* server = std::getenv("PULSE_SERVER"); server && *server)
            pulseReachable = true;
        else if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        {
            const std::string path = std::string(runtime) + "/pulse/native";
            if (path.size() < sizeof(sockaddr_un::sun_path))
            {
                const int socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (socketFd >= 0)
                {
                    sockaddr_un address{};
                    address.sun_family = AF_UNIX;
                    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
                    pulseReachable = connect(socketFd, reinterpret_cast<sockaddr*>(&address),
                                             sizeof(address)) == 0;
                    close(socketFd);
                }
            }
        }
        if (hasPulseDriver && pulseReachable)
            setenv("SDL_AUDIODRIVER", "pulseaudio", 0);
    }
#endif
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        std::fprintf(stderr, "[snd-output] SDL audio driver: %s\n", SDL_GetCurrentAudioDriver());
        return true;
    }
    std::fprintf(stderr, "[snd-output] SDL_InitSubSystem(audio): %s\n", SDL_GetError());
    return false;
}

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
    if (const char* seconds = std::getenv("TTT2_AUDIO_DUMP_SECONDS"))
        state_->dumpLimit = uint64_t(std::clamp(std::atoi(seconds), 1, 300)) * 48000;
    OutputSystem().Add(state_);
}

AudioOutputClient::~AudioOutputClient() { Close(); }

void AudioOutputClient::Close()
{
    std::unique_lock lock(state_->mutex);
    if (state_->closed) return;
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
        // Opt-in, bounded capture of the guest mixer output, before SDL conversion.
        static const char* captureDir = std::getenv("TTT2_AUDIO_CAPTURE_DIR");
        if (captureDir) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= state_->nextCapturePoll) {
                state_->nextCapturePoll = now + std::chrono::seconds(1);
                char request[1024], output[1024];
                std::snprintf(request, sizeof(request), "%s/capture_%08X.request", captureDir, state_->driver);
                if (auto* input = std::fopen(request, "r")) {
                    int seconds = 10;
                    const bool valid = std::fscanf(input, "%d", &seconds) == 1 && seconds >= 1 && seconds <= 60;
                    std::fclose(input);
                    std::remove(request);
                    if (valid) {
                        if (state_->dump) std::fclose(state_->dump);
                        std::snprintf(output, sizeof(output), "%s/capture_%08X.f32", captureDir, state_->driver);
                        state_->dump = std::fopen(output, "wb");
                        state_->dumpFrames = 0;
                        state_->dumpLimit = uint64_t(seconds) * 48000;
                    }
                }
            }
        }
        const auto conversion = ConvertAudioPcm(guest, free->samples);
        if (state_->dump) {
            std::fwrite(free->samples.data(), sizeof(float), free->samples.size(), state_->dump);
            state_->dumpFrames += AUDIO_PCM_SAMPLES;
            if (state_->dumpLimit && state_->dumpFrames >= state_->dumpLimit) {
                std::fclose(state_->dump);
                state_->dump = nullptr;
            }
        }
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
