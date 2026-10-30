// Audio HLE: each registered render client receives callbacks paced by completed
// 256-sample / 48 kHz buffers. TTT2 creates multiple engines; replacing a callback
// another stops the first engine's mixer and strands its queued Stop/Flush work.
// PCM samples go to a bounded Windows XAudio2 output queue. Each callback receives
// r3 = its own stable guest
// cell holding the registration context, as in Xenia's AudioSystem::RegisterClient.
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "guest_heap.h"
#include "audio_output.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

static constexpr uint32_t AUDIO_DRIVER_KEY = 0x41550000u;
static constexpr size_t AUDIO_MAX_CLIENTS = 8;
static constexpr int64_t FRAME_NS = 1000000000ll * 256 / 48000;
static constexpr uint32_t AUDIO_INVALID_HANDLE = 0xC0000008u;
static constexpr uint32_t AUDIO_INVALID_PARAMETER = 0xC000000Du;
static constexpr uint32_t AUDIO_NO_MEMORY = 0xC000009Au;

struct AudioClient
{
    // The registry mutex is never held while waiting for this mutex or calling
    // the guest. Recursive locking permits unregister from the callback itself.
    std::recursive_mutex execution;
    bool active = true;                       // protected by execution
    uint32_t callback = 0;
    uint32_t paramCell = 0;
    uint64_t pumped = 0;                       // protected by execution
    uint64_t maxCallbackUs = 0;
    std::atomic<uint64_t> submitted{0};
    std::unique_ptr<AudioOutputClient> output;
    ~AudioClient() { if (paramCell) g_virtualHeap.Free(g_base + paramCell); }
};

static std::mutex g_xaClientsMutex;
static std::array<std::shared_ptr<AudioClient>, AUDIO_MAX_CLIENTS> g_xaClients;
static std::atomic<uint64_t> g_xaPumped{0};
static std::atomic<uint64_t> g_xaSubmitted{0};
static std::atomic<bool> g_xaPumpStarted{false};

static std::shared_ptr<AudioClient> FindAudioClient(uint32_t handle)
{
    const uint32_t index = handle & 0xFFFF;
    if ((handle & 0xFFFF0000u) != AUDIO_DRIVER_KEY || index >= AUDIO_MAX_CLIENTS)
        return {};
    std::lock_guard lock(g_xaClientsMutex);
    return g_xaClients[index];
}

static bool PumpAudioClients(uint64_t nowNs = 0)
{
    bool pumped = false;
    std::array<std::shared_ptr<AudioClient>, AUDIO_MAX_CLIENTS> clients;
    {
        std::lock_guard lock(g_xaClientsMutex);
        clients = g_xaClients;
    }
    for (size_t index = 0; index < clients.size(); ++index)
    {
        const auto& client = clients[index];
        if (!client) continue;
        std::lock_guard execution(client->execution);
        if (!client->active) continue;
        if (!client->output->TryAcquireRenderCredit(nowNs)) continue;
        const auto began = std::chrono::steady_clock::now();
        GuestCall(client->callback, client->paramCell);
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - began).count();
        client->maxCallbackUs = std::max(client->maxCallbackUs, uint64_t(duration));
        pumped = true;
        ++g_xaPumped;
        if (++client->pumped % 2048 == 0)
        {
            std::fprintf(stderr, "[snd] driver=%08X pump frames=%llu submits=%llu context=%08X\n",
                         AUDIO_DRIVER_KEY | uint32_t(index),
                         (unsigned long long)client->pumped,
                         (unsigned long long)client->submitted.load(),
                         GRead32(g_base, client->paramCell));
            const auto output = client->output->Stats();
            std::fprintf(stderr, "[snd-output] driver=%08X ready=%u failed=%u received=%llu submitted=%llu completed=%llu dropped=%llu queued=%u nonzero=%llu nonfinite=%llu deviceq=%u credits=%u starved=%llu starved_bytes=%llu max_callback_us=%llu\n",
                         AUDIO_DRIVER_KEY | uint32_t(index), unsigned(output.ready), unsigned(output.failed),
                         (unsigned long long)output.received, (unsigned long long)output.submitted,
                         (unsigned long long)output.completed, (unsigned long long)output.dropped,
                         output.queued, (unsigned long long)output.nonzero,
                         (unsigned long long)output.nonfinite, output.deviceQueued, output.renderCredits,
                         (unsigned long long)output.starvationPasses, (unsigned long long)output.starvationBytes,
                         (unsigned long long)client->maxCallbackUs);
            client->maxCallbackUs = 0;
        }
    }
    return pumped;
}

static void AudioPumpThread()
{
    for (;;)
    {
        const uint64_t version = AudioOutputWakeVersion();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        if (!PumpAudioClients(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count())))
            WaitAudioOutputWake(version, FRAME_NS); // timeout only needed by silent fallback clients
    }
}

// XAudioRegisterRenderDriverClient(r3=&{callback, context}, r4=&driver OUT).
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    KTrace("XAudioRegisterRenderDriverClient");
    if (!ctx.r3.u32 || !ctx.r4.u32 || (ctx.r3.u32 & 3) || (ctx.r4.u32 & 3))
    {
        ctx.r3.u32 = AUDIO_INVALID_PARAMETER;
        return;
    }
    const uint32_t fn = GRead32(base, ctx.r3.u32);
    const uint32_t param = GRead32(base, ctx.r3.u32 + 4);
    if (!fn || (fn & 3)) { ctx.r3.u32 = AUDIO_INVALID_PARAMETER; return; }
    {
        std::lock_guard lock(g_xaClientsMutex);
        size_t index = 0;
        while (index < AUDIO_MAX_CLIENTS && g_xaClients[index]) ++index;
        if (index == AUDIO_MAX_CLIENTS) { ctx.r3.u32 = AUDIO_NO_MEMORY; return; }
        auto client = std::make_shared<AudioClient>();
        void* cell = g_virtualHeap.Alloc(8, 16);
        if (!cell) { ctx.r3.u32 = AUDIO_NO_MEMORY; return; }
        client->callback = fn;
        client->paramCell = GuestAddr(base, cell);
        GWrite32(base, client->paramCell, param);
        const uint32_t handle = AUDIO_DRIVER_KEY | uint32_t(index);
        client->output = std::make_unique<AudioOutputClient>(handle);
        // Publish the caller's handle before its first callback can submit.
        GWrite32(base, ctx.r4.u32, handle);
        g_xaClients[index] = client;
        std::fprintf(stderr, "[snd] render driver=%08X client fn=%08X param=%08X cell=%08X\n",
                     handle, fn, param, client->paramCell);
    }
    if (!g_xaPumpStarted.exchange(true))
        std::thread(AudioPumpThread).detach();
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    KTrace("XAudioUnregisterRenderDriverClient");
    const uint32_t handle = ctx.r3.u32;
    auto client = FindAudioClient(handle);
    if (!client) { ctx.r3.u32 = AUDIO_INVALID_HANDLE; return; }
    // Wait for an existing callback before removing its submission target. A
    // snapshot taken earlier cannot start another callback after active=false.
    std::lock_guard execution(client->execution);
    {
        std::lock_guard lock(g_xaClientsMutex);
        auto& slot = g_xaClients[handle & 0xFFFF];
        if (slot != client) { ctx.r3.u32 = AUDIO_INVALID_HANDLE; return; }
        client->active = false;
        slot.reset();
    }
    client->output->Close();
    ctx.r3.u32 = STATUS_SUCCESS;
}
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    KTrace("XAudioSubmitRenderDriverFrame");
    auto client = FindAudioClient(ctx.r3.u32);
    if (!client) { ctx.r3.u32 = AUDIO_INVALID_HANDLE; return; }
    const uint32_t samples = ctx.r4.u32;
    if (!samples || uint64_t(samples) + AUDIO_PCM_BYTES > 0x100000000ull)
    { ctx.r3.u32 = AUDIO_INVALID_PARAMETER; return; }
    client->output->Submit(base + samples);
    ++client->submitted;
    ++g_xaSubmitted;
    ctx.r3.u32 = STATUS_SUCCESS;
}

// XAudioGetSpeakerConfig(r3=&config OUT) -> stereo.
PPC_FUNC(__imp__XAudioGetSpeakerConfig) { KTrace("XAudioGetSpeakerConfig"); if (ctx.r3.u32) GWrite32(base, ctx.r3.u32, 1); ctx.r3.u32 = STATUS_SUCCESS; }

// Volume/ducker queries -> unity / disabled.
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)        { KTrace("XAudioGetVoiceCategoryVolume"); if (ctx.r4.u32) GWrite32(base, ctx.r4.u32, 0x3F800000 /*1.0f*/); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__XAudioEnableDucker)                  { KTrace("XAudioEnableDucker"); ctx.r3.u32 = STATUS_SUCCESS; }
PPC_FUNC(__imp__XAudioGetDuckerThreshold)            { KTrace("XAudioGetDuckerThreshold"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerAttackTime)           { KTrace("XAudioGetDuckerAttackTime"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerReleaseTime)          { KTrace("XAudioGetDuckerReleaseTime"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerHoldTime)             { KTrace("XAudioGetDuckerHoldTime"); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetDuckerLevel)                { KTrace("XAudioGetDuckerLevel"); ctx.r3.u32 = 0; }

// XMACreateContext / XMAReleaseContext and the decoder itself: xma.cpp.
