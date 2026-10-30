// Online: opponents' profile names (user report 2026-10-14: "his name shows when he joins the
// lobby, but after we move to selecting characters and start the game his name shows as Player").
//
// MEASURED with two loopback instances: the session room shows both names everywhere, but from
// character select on (and in the fight HUD) the HOST shows the guest as "Player" while the guest
// shows the host's name. Every name label goes through sub_824165C8, which for an online player
// asks sub_82416380(&xuid) for the name and strncpy's it (empty -> the label keeps "Player").
// sub_82416380: the local player -> the profile name; otherwise either
//   (a) a per-XUID object from the manager at [0x82F31DC4] (sub_826CA9B8 / sub_82336E20) whose
//       request type 2 must be in state 5 - a Live-side gamertag request this port never
//       completes - else localized string 1599, "Player"; or
//   (b) NACE's member list (sub_82B34500): member record +8 XUID, +16 name[16].
// The host takes (a) for the guest. NACE's member records do hold every player's name (the host
// writes them from each joiner's message; a guest copies them from the host's roster).
//
// Fix: collect XUID -> name from NACE's member records as NACE writes them (roster serialization
// sub_82B46A68 on the host, member record copies sub_826BA700 on a guest) and make the name
// lookup return it for any remote online XUID whose answer differs.
#include "kernel_shared.h"
#include "guest_heap.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

extern "C" PPC_FUNC(__imp__sub_82B46A68);
extern "C" PPC_FUNC(__imp__sub_826BA700);
extern "C" PPC_FUNC(__imp__sub_82416380);

namespace {

struct Known { std::string name; uint32_t guestText = 0; };   // guestText: the name in guest memory
std::mutex g_mutex;
std::map<uint64_t, Known> g_names;           // online XUID -> profile name

uint64_t Read64(uint8_t* base, uint32_t a) { return (uint64_t(GRead32(base, a)) << 32) | GRead32(base, a + 4); }

bool OnlineXuid(uint64_t xuid) { return (xuid >> 48) == 0x0009; }

std::string Name16(uint8_t* base, uint32_t a)
{
    char text[17] = {};
    std::memcpy(text, base + a, 16);
    return std::string(text, strnlen(text, 16));
}

// A NACE member record {vtable 0x820E7E68, flags, +8 XUID, +16 name[16]}.
void Remember(uint8_t* base, uint32_t record)
{
    if (!record) return;
    const uint64_t xuid = Read64(base, record + 8);
    const std::string name = Name16(base, record + 16);
    if (!OnlineXuid(xuid) || name.empty()) return;
    std::lock_guard l(g_mutex);
    Known& k = g_names[xuid];
    if (k.name == name) return;
    k.name = name;
    k.guestText = 0;                          // re-published on the next lookup
    std::fprintf(stderr, "[names] %016llX = '%s'\n", (unsigned long long)xuid, name.c_str());
}

} // namespace

// NACE (host): serialize a member into a roster entry. r5 = member; member +4 = its record.
PPC_FUNC(sub_82B46A68)
{
    if (ctx.r5.u32) Remember(base, GRead32(base, ctx.r5.u32 + 4));
    __imp__sub_82B46A68(ctx, base);
}

// NACE (guest): member record copy-assign (r3 = destination record, r4 = source record).
PPC_FUNC(sub_826BA700)
{
    const uint32_t dst = ctx.r3.u32;
    __imp__sub_826BA700(ctx, base);
    Remember(base, dst);
}

// Name for a player XUID (r3 = &xuid) -> r3 = name text.
PPC_FUNC(sub_82416380)
{
    const uint32_t xuidPtr = ctx.r3.u32;
    const uint64_t xuid = xuidPtr ? Read64(base, xuidPtr) : 0;
    __imp__sub_82416380(ctx, base);
    if (!OnlineXuid(xuid)) return;
    std::lock_guard l(g_mutex);
    const auto it = g_names.find(xuid);
    if (it == g_names.end()) return;
    Known& k = it->second;
    const uint32_t answer = ctx.r3.u32;
    if (answer && Name16(base, answer) == k.name) return;     // the game already has it
    if (!k.guestText)
    {
        uint8_t* text = static_cast<uint8_t*>(g_virtualHeap.Alloc(32));
        if (!text) return;
        std::memset(text, 0, 32);
        std::memcpy(text, k.name.data(), (std::min)(k.name.size(), size_t(15)));
        k.guestText = uint32_t(text - base);
        std::fprintf(stderr, "[names] lookup %016llX: '%s' -> '%s'\n", (unsigned long long)xuid,
                     answer ? Name16(base, answer).c_str() : "", k.name.c_str());
    }
    ctx.r3.u64 = k.guestText;
}
