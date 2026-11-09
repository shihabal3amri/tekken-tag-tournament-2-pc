// Phase 0 of port/ONLINE_2V2_PLAN.md - diagnostics only, off unless TTT2_2V2_DIAG=1: what four-player online
// would build on.
//  - The sync engine's node table, every half second (on the clock's half seconds) while it has nodes: every room member is a node (S+4828, the
//    count at S+4856; per node +720 newest frame stored, +724 current, +736 published RNG pair, +748 gone).
//  - The record this instance consumes: nace +5976 {frame, frame, in0, in1}, ready +6020, prediction +5932, and
//    the battle state (0x82EAC03C, 4 = fighting).
//  - The word this machine sends for its side (sub_82664CE0) with the pad slots sub_8266D398 read for it. Pair
//    play: the active character's controller (slot 9 / 10, or the partner's 11 / 12 when fighter +7792 says so)
//    | the other controller's TAG bit (0x200), unless fighter +7800 and its state +48 > 38.
#include "kernel_shared.h"
#include "online_2v2.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>

namespace {
constexpr uint32_t kNace = 0x82E6C5C8u, kSync = kNace + 64;
constexpr uint32_t kFighters = 0x82E88878u;              // per side: the fighter on point (sub_824CB300)
bool On()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_2V2_DIAG"); return e && *e == '1'; }();
    return on;
}
thread_local int t_reads = 0;
thread_local uint32_t t_slot[4], t_value[4];
std::atomic<int> g_lines{ 0 };
int32_t S32(uint32_t a) { return int32_t(GRead32(g_base, a)); }
} // namespace
std::string XSocketTrafficDiag();                         // xsocket.cpp
namespace {

void NodeTable()
{
    int32_t lastCount = -1;
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500 - online2v2::DiagMs() % 500));   // every instance samples on the same half seconds
        if (!g_base) continue;
        const int32_t count = S32(kSync + 4856);
        if (count <= 0 && lastCount <= 0) { lastCount = count; continue; }
        lastCount = count;
        // Once per node table (count and battle state): every word of every node, to tell their fields apart.
        static int32_t dumpedCount = -1;
        static uint32_t dumpedState = ~0u;
        const uint32_t state = GRead32(g_base, 0x82EAC03Cu);
        if (count != dumpedCount || (state == 4 && dumpedState != 4))
        {
            dumpedCount = count;
            dumpedState = state;
            for (int i = 0; i < 7 && i < count; ++i)
            {
                const uint32_t node = GRead32(g_base, kSync + 4828 + 4 * i);
                if (!node) continue;
                std::string dump = "[2v2] node " + std::to_string(i) + " words (battle state " + std::to_string(state) + "):";
                for (uint32_t w = 0; w < 804; w += 4)
                    if (const uint32_t v = GRead32(g_base, node + w)) { char b[24]; std::snprintf(b, sizeof(b), " +%u=%08X", w, v); dump += b; }
                dump += "\n";
                std::fputs(dump.c_str(), stderr);
            }
        }
        char line[1024];
        // DiagMs: one clock for every instance on this PC, so their lines line up.
        int n = std::snprintf(line, sizeof(line), "[2v2] t=%llu nodes %d:", online2v2::DiagMs(), count);
        for (int i = 0; i < 7 && i < count && n < 900; ++i)
        {
            const uint32_t node = GRead32(g_base, kSync + 4828 + 4 * i);
            if (!node) { n += std::snprintf(line + n, sizeof(line) - n, " [%d: none]", i); continue; }
            n += std::snprintf(line + n, sizeof(line) - n, " [%d: %08X newest %d current %d gone %u w0 %08X w4 %08X w8 %08X]", i, node,
                               S32(node + 720), S32(node + 724), GRead8(g_base, node + 748), GRead32(g_base, node),
                               GRead32(g_base, node + 4), GRead32(g_base, node + 8));
        }
        n += std::snprintf(line + n, sizeof(line) - n, "; stream newest %d / %d", online2v2::StreamNewest(0), online2v2::StreamNewest(1));
        n += std::snprintf(line + n, sizeof(line) - n, "; record %d/%d in %04X %04X ready %u predict %u; battle state %u\n",
                           S32(kNace + 5976), S32(kNace + 5980), GRead16(g_base, kNace + 5984), GRead16(g_base, kNace + 5986),
                           GRead8(g_base, kNace + 6020), GRead8(g_base, kNace + 5932), GRead32(g_base, 0x82EAC03Cu));
        std::fputs(line, stderr);
        static int every = 0;
        if (++every % 4 == 0)
            std::fprintf(stderr, "[2v2] t=%llu traffic (2 s):%s\n", online2v2::DiagMs(), XSocketTrafficDiag().c_str());
    }
}
const bool g_started = [] {
    if (On()) std::thread(NodeTable).detach();
    return true;
}();
} // namespace

// Pad reads made for the side's word (called from sub_8266D398): slot and value, for the next send.
extern "C" PPC_FUNC(__imp__sub_8228DC40);
PPC_FUNC(sub_8228DC40)
{
    const uint32_t slot = ctx.r4.u32, lr = uint32_t(ctx.lr);
    __imp__sub_8228DC40(ctx, base);
    if (On() && lr >= 0x8266D398u && lr < 0x8266D548u && t_reads < 4)
    {
        t_slot[t_reads] = slot;
        t_value[t_reads] = ctx.r3.u32 & 0xFFFFu;
        ++t_reads;
    }
}

// The side's word for NACE (called from rollback_session.cpp's sub_82664CE0 hook): printed when it or the pads
// behind it change (the first 4000 such lines).
void Diag2v2Send(PPCContext& ctx, uint8_t* base)
{
    if (On())
    {
        const uint32_t word = ctx.r4.u32 & 0xFFFFu;
        uint64_t sig = word;
        for (int i = 0; i < t_reads; ++i) sig = sig * 1000003u + (uint64_t(t_slot[i]) << 16 | t_value[i]);
        static thread_local uint64_t last = ~0ull;
        if (sig != last && g_lines.fetch_add(1) < 4000)
        {
            char line[512];
            int n = std::snprintf(line, sizeof(line), "[2v2] send %04X (record %d) pads:", word, S32(kNace + 5976));
            for (int i = 0; i < t_reads; ++i) n += std::snprintf(line + n, sizeof(line) - n, " slot %u=%04X", t_slot[i], t_value[i]);
            // The pair test of sub_8266D398 (obj = r3, P = obj+7488): P+0 != -1, P+12 (pair mode), and per side the
            // 64-byte entry at [P+124] + 64 * side, +8 (that side is a pair; the count at P+120).
            const uint32_t obj = ctx.r3.u32, P = obj + 7488;
            const uint32_t count = GRead32(base, P + 120), table = GRead32(base, P + 124);
            n += std::snprintf(line + n, sizeof(line) - n, "; obj %08X P0 %08X pairmode %u sides %u", obj, GRead32(base, P), GRead8(base, P + 12), count);
            for (uint32_t s = 0; s < count && s < 2 && table; ++s)
                n += std::snprintf(line + n, sizeof(line) - n, " [%u: +8=%u]", s, GRead8(base, table + 64 * s + 8));
            for (int s = 0; s < 2; ++s)
            {
                const uint32_t f = GRead32(base, kFighters + 4 * s);
                if (f) n += std::snprintf(line + n, sizeof(line) - n, "; side %d fighter %08X +7792 %u +7800 %08X", s, f, GRead32(base, f + 7792),
                                          GRead32(base, f + 7800));
            }
            std::snprintf(line + n, sizeof(line) - n, "\n");
            std::fputs(line, stderr);
        }
        last = sig;
    }
    t_reads = 0;
}


// Research (TTT2_2V2_DIAG=1): the Lumen interface calls taking a clip name (std::string r4) and a label or value (r5:
// std::string or not) - those naming a side marker or a player plate, to learn which call colours the room's fighter
// arrows.
namespace {
std::string GuestText(uint8_t* base, uint32_t a)
{
    std::string s;
    if (a < 0x00100000u || a >= 0x90000000u) return s;
    for (uint32_t i = 0; i < 40; ++i)
    {
        const char c = char(GRead8(base, a + i));
        if (!c) break;
        if (c < 32 || c > 126) return std::string();
        s += c;
    }
    return s;
}
std::string GuestStdString(uint8_t* base, uint32_t s)
{
    if (s < 0x00100000u || s >= 0x90000000u) return {};
    const uint32_t cap = GRead32(base, s + 20), size = GRead32(base, s + 16);
    if (size > 64 || cap < size) return {};
    return GuestText(base, cap >= 16 ? GRead32(base, s) : s);
}
void ClipCall(uint8_t* base, const char* what, PPCContext& ctx)
{
    if (!On()) return;
    const std::string clip = GuestStdString(base, ctx.r4.u32), label = GuestStdString(base, ctx.r5.u32);
    auto wanted = [](const std::string& t) { return t.find("side") != std::string::npos || t.find("plate") != std::string::npos; };
    if (!wanted(clip) && !wanted(label)) return;
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    const std::string key = std::string(what) + clip + "|" + label + "|" + std::to_string(uint32_t(ctx.lr));
    std::lock_guard l(m);
    if (seen[key]++ < 2)
        std::fprintf(stderr, "[clip] t=%llu %s(%08X, '%s', '%s' / %08X) lr %08X\n", online2v2::DiagMs(), what, ctx.r3.u32,
                     clip.c_str(), label.c_str(), ctx.r5.u32, uint32_t(ctx.lr));
}
} // namespace
#define CLIP_HOOK(addr)                                \
    extern "C" PPC_FUNC(__imp__sub_##addr);           \
    PPC_FUNC(sub_##addr)                               \
    {                                                  \
        ClipCall(base, #addr, ctx);                    \
        __imp__sub_##addr(ctx, base);                  \
    }
CLIP_HOOK(826277C8)
CLIP_HOOK(82627860)
CLIP_HOOK(826278E0)
CLIP_HOOK(82627A28)
CLIP_HOOK(82627AE0)
CLIP_HOOK(82627BA0)
CLIP_HOOK(82627C68)
CLIP_HOOK(82627CF8)
CLIP_HOOK(82627D88)
CLIP_HOOK(82627E28)
CLIP_HOOK(82627EE0)
#undef CLIP_HOOK
