// TU3: sub_8231CFF0 initializes fighter+92 as the health object. Its +4/+8
// are current/recoverable HP, +16 max HP (16.16 fixed point). sub_824D09B0
// subtracts r4 damage and updates recoverable health; +0 is fighter index,
// whose low bit is the side (also used by the original regeneration routine).
// Intercept on the game thread, never scan or modify save files.
#include "host_settings.h"
#include "kernel_shared.h"
#include "offline_cheats.h"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <cstdio>
bool XSessionAnyActive();
extern "C" PPC_FUNC(__imp__sub_824D09B0);
extern "C" PPC_FUNC(__imp__sub_8265F5D8);
namespace offline_cheats {
namespace {
std::atomic<bool> offlineMenu{false};
std::atomic<int>& Health() {
    static std::atomic<int> value{std::clamp<int>(host_settings::ReadInt("Mods","InfiniteHealth",0),0,3)};
    return value;
}
std::atomic<bool>& Money() {
    static std::atomic<bool> value{host_settings::ReadInt("Mods","InfiniteMoney",0)==1};
    return value;
}
std::atomic<bool>& Unlock() {
    static std::atomic<bool> value{host_settings::ReadInt("Mods","UnlockContent",0)==1};
    return value;
}
}
int HealthSides() { return Health().load(); }
bool InfiniteMoney() { return Money().load(); }
bool UnlockContent() { return Unlock().load(); }
void SetUnlockContent(bool enabled) {
    Unlock().store(enabled);
    host_settings::Write("Mods","UnlockContent",enabled?"1":"0");
}
void SetInfiniteMoney(bool enabled) {
    Money().store(enabled);
    host_settings::Write("Mods","InfiniteMoney",enabled?"1":"0");
}
void SetHealthSides(int sides) {
    sides=std::clamp(sides,0,3); Health().store(sides);
    const std::string value=std::to_string(sides);
    host_settings::Write("Mods","InfiniteHealth",value.c_str());
}
bool Allowed() { return offlineMenu.load() && !XSessionAnyActive(); }
void SetOfflineMenu(bool offline) { offlineMenu.store(offline); }
}
PPC_FUNC(sub_824D09B0) {
    const uint32_t object=ctx.r3.u32;
    // This game-owned routine also receives objects in its image/static arena,
    // not just the host virtual/physical heaps. The entire guest space is committed.
    const bool valid=object>=0x10000u && object<=0xFFFFFFD7u && (object&3u)==0;
    static const bool trace=[] {const char* v=std::getenv("TTT2_CHEAT_TRACE");return v&&*v=='1';}();
    if(trace){static unsigned count=0;if(count++<32)std::fprintf(stderr,"[cheats-trace] damage object=%08X fighter=%u max=%u damage=%d mask=%d allowed=%d\n",object,GRead32(base,object),GRead32(base,object+16),ctx.r4.s32,offline_cheats::HealthSides(),int(offline_cheats::Allowed()));}
    if (valid && offline_cheats::HealthSides() && offline_cheats::Allowed()) {
        const uint32_t fighter=GRead32(base,object), maximum=GRead32(base,object+16);
        // Four fighter indices, sane positive 16.16 maximum. Reject other objects.
        if (fighter<4 && maximum>=65536 && maximum<=2000u*65536 &&
            (offline_cheats::HealthSides() & (1u << (fighter&1))) && ctx.r4.s32>0) {
            GWrite32(base,object+4,maximum);
            GWrite32(base,object+8,maximum);
            ctx.r4.u64=0;
            static std::atomic<unsigned> loggedSides{0};const unsigned bit=1u<<(fighter&1);
            if(!(loggedSides.fetch_or(bit)&bit))std::fprintf(stderr,"[cheats] infinite health active: side %u, damage prevented\n",(fighter&1)+1);
        }
    }
    __imp__sub_824D09B0(ctx,base);
}

// Customize getMoney (82709920), addMoney (82709D60) and subMoney
// (82709E08) all resolve this profile record. +4 is the spendable wallet;
// +8 is lifetime earnings and is deliberately left to the game's accounting.
// The cap is the original addMoney clamp, 0x3B9AC9FF (999,999,999).
PPC_FUNC(sub_8265F5D8) {
    __imp__sub_8265F5D8(ctx,base);
    if (!offline_cheats::InfiniteMoney() || !offline_cheats::Allowed()) return;
    const uint32_t record=ctx.r3.u32;
    if ((record>=VHEAP_BEGIN && record<VHEAP_END-12) ||
        (record>=PHEAP_BEGIN && record<PHEAP_END-12))
        if(GRead32(base,record+4)!=999999999u){
            GWrite32(base,record+4,999999999u);
            static std::atomic<bool> logged{false};if(!logged.exchange(true))std::fprintf(stderr,"[cheats] infinite money active: wallet refilled to 999999999\n");
        }
}

// Temporary availability overrides. Original queries still execute, preserving their
// side effects, argument validation and normal behavior when disabled/online.
// No unlock/save bitfields are rewritten. Asset existence and DLC licence checks
// remain separate, so this cannot supply content absent from the installed game.
extern "C" PPC_FUNC(__imp__sub_822ADCB0);
PPC_FUNC(sub_822ADCB0) {
    const uint32_t id=ctx.r4.u32;
    __imp__sub_822ADCB0(ctx,base);
    if (id<773 && offline_cheats::UnlockContent() && offline_cheats::Allowed())
    {
        ctx.r3.u64=(ctx.r3.u32&~7u)|2u; // original progression state: 2 = unlocked
        static std::atomic<bool> logged{false};if(!logged.exchange(true))std::fprintf(stderr,"[cheats] progression content override active\n");
    }
}
#define OFFLINE_OWNERSHIP_QUERY(address, limit) \
extern "C" PPC_FUNC(__imp__sub_##address); \
PPC_FUNC(sub_##address) { \
    const uint32_t id=ctx.r4.u32; \
    __imp__sub_##address(ctx,base); \
    if(id<limit && offline_cheats::UnlockContent() && offline_cheats::Allowed()) ctx.r3.u64=1; \
}
// Limits are the original functions' range checks / 625 32-bit ownership words.
OFFLINE_OWNERSHIP_QUERY(827163B8,20000u) // wardrobe ownership
OFFLINE_OWNERSHIP_QUERY(8272F190,20000u) // customization unlock inventory
OFFLINE_OWNERSHIP_QUERY(827308C8,163u)   // LED categories
OFFLINE_OWNERSHIP_QUERY(8272F538,100u)   // character panels
#undef OFFLINE_OWNERSHIP_QUERY
