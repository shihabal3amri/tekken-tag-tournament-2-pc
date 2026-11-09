#include "audio_output.h"
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
void DiagNameThread(const wchar_t* name);   // kernel_thread.cpp (TEST ONLY)
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
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
    // XAudio2's pass starts on this voice, written in OnVoiceProcessingPassStart only; Stats() takes and clears
    // the last two.
    std::atomic<int64_t> lastPassNs{0};
    std::atomic<uint64_t> maxPassGapUs{0};
    std::atomic<uint32_t> latePasses{0};
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) override;
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override;
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { error.store(hr); WakeOutputWorker(); }
};

// 0.1.8 TEST 17: only OnCriticalError is used; the output worker logs it (no logging on XAudio2's thread).
struct EngineEvents final : IXAudio2EngineCallback
{
    std::atomic<HRESULT> error{S_OK};
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) override { error.store(hr); WakeOutputWorker(); }
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

// XAudio2's performance data, read by the output worker after each pass (AudioOutputStats).
static std::atomic<uint32_t> g_engineGlitches{0}, g_engineLatency{0};

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

// 0.1.8 TEST 17: what the sound plays into, once when XAudio2 has opened the device. The tester hears stutter that
// no counter here shows (TEST 16 logs 48-51: starved=0, engine_glitches=0, NUSound underruns only at loads), so:
// the default output's kind (speakers, headphones, HDMI/DisplayPort, SPDIF, headset...), its adapter (the driver's
// name, e.g. "NVIDIA High Definition Audio", or just "Bluetooth" - never the endpoint's own name, which the user can
// edit, nor a Bluetooth device's, often its owner's), the format
// Windows mixes for it (Sound control panel: Default Format) and what XAudio2's mastering voice mixes. MEASURED
// (scratch harness, dev PC, 2.2-2.7 ms once): a virtual headphones device, format 8ch 96000Hz 24bit, XAudio2 mixes
// 8 channels at 96000Hz - so engine_latency's ~3800 samples there are ~39 ms; the tester's ~1800 are ~38 ms only if
// their device runs at 48 kHz (inferred; this line will say).
// The keys as mmdeviceapi.h / functiondiscoverykeys_devpkey.h define them: no SDK library defines the first and the
// last (uuid.lib, in CMake's default libraries, has only the adapter name's), so like the float subtype below all
// three are spelled out.
static const PROPERTYKEY ENDPOINT_FORM_FACTOR_KEY = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
static const PROPERTYKEY ADAPTER_NAME_KEY = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
static const PROPERTYKEY DEVICE_FORMAT_KEY = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

// Whether the endpoint's adapter is a Bluetooth device: its connector leads to a device interface the Bluetooth stack
// enumerated (BTHENUM: A2DP, BTHHFENUM: hands-free, BTHLE...: LE Audio). Such a name is the one the device was given
// ("AirPods de Juan", "Max's AirPods"), so it is not logged.
static bool BluetoothEndpoint(IMMDevice* device)
{
    bool bluetooth = false;
    IDeviceTopology* topology = nullptr;
    IConnector* connector = nullptr;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&topology))) &&
        SUCCEEDED(topology->GetConnector(0, &connector)) && SUCCEEDED(connector->GetDeviceIdConnectedTo(&id)) && id)
    {
        for (wchar_t* c = id; *c; ++c) *c = wchar_t(std::towupper(*c));
        bluetooth = std::wcsstr(id, L"BTH") != nullptr;
    }
    if (id) CoTaskMemFree(id);
    if (connector) connector->Release();
    if (topology) topology->Release();
    return bluetooth;
}

// On the output worker, which CoInitializeEx'd itself (MTA) in Initialize. What cannot be read prints "unknown".
static void LogOutputDevice(IXAudio2MasteringVoice* master)
{
    static const char* const FORMS[] = {"remote network device", "speakers", "line level", "headphones",
        "microphone", "headset", "handset", "digital passthrough", "SPDIF", "HDMI/DisplayPort", "unknown"};
    const char* form = "unknown";
    char adapter[128] = "unknown";
    char format[48] = "unknown";
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)))
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(ENDPOINT_FORM_FACTOR_KEY, &value)) && value.vt == VT_UI4 && value.ulVal < std::size(FORMS))
            form = FORMS[value.ulVal];
        PropVariantClear(&value);
        if (BluetoothEndpoint(device))
            std::snprintf(adapter, sizeof(adapter), "Bluetooth (its name is not logged)");
        else if (SUCCEEDED(store->GetValue(ADAPTER_NAME_KEY, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        {
            // In case a Bluetooth device got past the check above, whose name is often its owner's ("Name's AirPods"):
            // from the last apostrophe (0x27 or the typographic 0x2019) on, after a '*'.
            const wchar_t* name = value.pwszVal;
            for (const wchar_t* c = value.pwszVal; *c; ++c)
                if (*c == 0x27 || *c == 0x2019) name = c;
            const int cut = name != value.pwszVal;
            adapter[0] = '*';
            if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, adapter + cut, int(sizeof(adapter)) - 1 - cut, nullptr, nullptr))
                std::snprintf(adapter, sizeof(adapter), "unknown");
        }
        PropVariantClear(&value);
        if (SUCCEEDED(store->GetValue(DEVICE_FORMAT_KEY, &value)) && value.vt == VT_BLOB && value.blob.pBlobData &&
            value.blob.cbSize >= sizeof(WAVEFORMATEX))
        {
            const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData);
            std::snprintf(format, sizeof(format), "%uch %luHz %ubit", unsigned(wave->nChannels),
                          (unsigned long)wave->nSamplesPerSec, unsigned(wave->wBitsPerSample));
        }
        PropVariantClear(&value);
    }
    if (store) store->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    DWORD mask = 0;
    master->GetChannelMask(&mask);
    std::fprintf(stderr, "[snd-output] output device: %s, adapter \"%s\", format %s; XAudio2 mixes %u channels (mask %08lX) at %uHz\n",
                 form, adapter, format, details.InputChannels, (unsigned long)mask, details.InputSampleRate);
}

namespace {
// 0.1.8 TEST 17: when XAudio2's passes come. It mixes 10 ms a pass (XAUDIO2_QUANTUM_DENOMINATOR 100 in xaudio2.h,
// SDK 10.0.26100). MEASURED (scratch probe, dev PC, a voice like ours, 30 s): 3001 pass starts, mean gap 10.0 ms,
// longest 11.9 ms, none in pairs, none over 20 ms. A gap over LATE_PASS_US = that pass came at least a pass late.
// MEASURED (scratch harness on this file, dev PC): every other thread, XAudio2's included, stalled for 25 / 60 /
// 150 ms gave max_pass_gap_us 45913 / 69271 / 159471, pass_gaps_over_20ms 1 each - and engine_glitches stayed 0,
// so XAudio2's own count does not show a late pass. Each [snd-output] line prints the longest gap and the late
// count since the line before (per voice; every voice sees the same passes).
constexpr uint64_t LATE_PASS_US = 20000;

void VoiceEvents::OnVoiceProcessingPassStart(UINT32 bytesRequired)
{
    // Every pass, first: one clock read and relaxed atomics, no lock (MEASURED ~19 ns a call on the dev PC).
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t last = lastPassNs.load(std::memory_order_relaxed);
    lastPassNs.store(now, std::memory_order_relaxed);
    if (last)
    {
        const uint64_t gapUs = uint64_t(now - last) / 1000;
        uint64_t longest = maxPassGapUs.load(std::memory_order_relaxed);
        while (gapUs > longest && !maxPassGapUs.compare_exchange_weak(longest, gapUs, std::memory_order_relaxed)) {}
        if (gapUs > LATE_PASS_US) latePasses.fetch_add(1, std::memory_order_relaxed);
    }
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
    AudioOutputSystem()
    {
        std::thread([this] {
            SetThreadDescription(GetCurrentThread(), L"audio-output");     // DIAGNOSTIC BUILD (p72): per-thread report
            Run();
        }).detach();
    }

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
            audio_->RegisterForCallbacks(&engineEvents_);   // 0.1.8 TEST 17: OnCriticalError, logged by Run
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
        LogOutputDevice(master_);
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
        DiagNameThread(L"audio-output");
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
            // 0.1.8 TEST 14: a tester hears the sound stutter while our queue never ran short (log 44:
            // starved=0, queued 7-8) - a mixing pass that misses its deadline is counted only here.
            if (audio_)
            {
                XAUDIO2_PERFORMANCE_DATA perf{};
                audio_->GetPerformanceData(&perf);
                g_engineGlitches.store(perf.GlitchesSinceEngineStarted, std::memory_order_relaxed);
                g_engineLatency.store(perf.CurrentLatencyInSamples, std::memory_order_relaxed);
                // XAudio2's documentation: after a critical error it must be closed down and restarted (not done here).
                if (const HRESULT error = engineEvents_.error.exchange(S_OK); FAILED(error))
                    std::fprintf(stderr, "[snd-output] XAudio2 critical error hr=%08X (OnCriticalError)\n", unsigned(error));
            }
            // Notifications received during processing remain visible, so a
            // submit cannot lose its wake and wait another whole audio quantum.
            workVersion_.wait(version);
        }
    }

    IXAudio2* audio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    EngineEvents engineEvents_;
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

uint32_t AudioOutputClient::Queued() const
{
    std::lock_guard lock(state_->mutex);
    return state_->stats.queued;
}

AudioOutputStats AudioOutputClient::Stats() const
{
    std::lock_guard lock(state_->mutex);
    auto result = state_->stats;
    result.renderCredits = state_->renderCredits;
    result.engineGlitches = g_engineGlitches.load(std::memory_order_relaxed);
    result.engineLatency = g_engineLatency.load(std::memory_order_relaxed);
    result.maxPassGapUs = state_->events.maxPassGapUs.exchange(0, std::memory_order_relaxed);
    result.latePasses = state_->events.latePasses.exchange(0, std::memory_order_relaxed);
    return result;
}
