// Research (TTT2_2V2_DIAG=1): the room's member messages (2026-11-07, READY lost after a member left). A member's change
// (READY, back from a match) goes to the room host as message 513 - its member entry, 56 bytes (sub_826B8A70; READY:
// sub_826B90B8); the host takes them in its network update (CRoomControlClient, sub_826C3A30 -> sub_826C21B8, after
// sub_826C1B50's check of this PC's session member) and copies them into the room data. Logged: each entry sent, each
// 513 decoded on arrival (sub_826BA688), and the host update's runs / refusals every 5 s.
#include "kernel_shared.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {
bool Diag()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    return on;
}
int64_t Ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<uint32_t> g_updates{ 0 }, g_refused{ 0 };
} // namespace

// a member entry to the room host (r3 the room manager, r4 the 56-byte entry: +0 the member, +8 in a match, +20 READY)
extern "C" PPC_FUNC(__imp__sub_826B8A70);
PPC_FUNC(sub_826B8A70)
{
    if (Diag() && ctx.r4.u32)
        std::fprintf(stderr, "[room-msg] send entry: member %d, in a match %u, streak %u, READY %u (lr %08X)\n",
                     int32_t(GRead32(base, ctx.r4.u32)), GRead32(base, ctx.r4.u32 + 8), GRead32(base, ctx.r4.u32 + 12),
                     GRead8(base, ctx.r4.u32 + 20), uint32_t(ctx.lr));
    __imp__sub_826B8A70(ctx, base);
}

// message 513 decoded from the network (r3 the message: +4 the entry; r4 the bytes, r5 their size)
extern "C" PPC_FUNC(__imp__sub_826BA688);
PPC_FUNC(sub_826BA688)
{
    const uint32_t msg = ctx.r3.u32;
    __imp__sub_826BA688(ctx, base);
    if (Diag() && msg && ctx.r3.u32)
        std::fprintf(stderr, "[room-msg] got entry: member %d, in a match %u, streak %u, READY %u\n", int32_t(GRead32(base, msg + 4)),
                     GRead32(base, msg + 12), GRead32(base, msg + 16), GRead8(base, msg + 24));
}

// the room host's network update (r3 the client: +12 the room data); -1: refused (sub_826C1B50)
extern "C" PPC_FUNC(__imp__sub_826C3A30);
PPC_FUNC(sub_826C3A30)
{
    __imp__sub_826C3A30(ctx, base);
    if (!Diag()) return;
    ++g_updates;
    if (int32_t(ctx.r3.u32) < 0) ++g_refused;
    static std::atomic<int64_t> at{ 0 };
    const int64_t now = Ms();
    if (now - at.load() >= 5000)
    {
        at = now;
        std::fprintf(stderr, "[room-msg] host update: %u runs, %u refused (5 s)\n", g_updates.exchange(0), g_refused.exchange(0));
    }
}
