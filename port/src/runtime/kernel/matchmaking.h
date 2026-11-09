// Session discovery behind XSessionSearch / SearchByID (port/ONLINE_PLAN.md). A host publishes
// its matchmaking sessions; searchers list them. The backend follows the transport:
//  - steam: one Steam lobby per session with this record as lobby data (steam_net.cpp);
//  - loopback (two instances on one PC): %LOCALAPPDATA%\TTT2Recomp\net\sessions\<id>.ses,
//    rewritten every 2 s while the host lives (older than 10 s = gone).
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace mm {

struct Property { uint32_t id = 0; std::vector<uint8_t> value; };   // raw value bytes (big-endian)
struct Context { uint32_t id = 0, value = 0; };

struct SessionRecord
{
    uint64_t id = 0;                    // XNKID
    uint64_t hostPeer = 0;              // transport peer id of the host
    uint64_t hostXuid = 0;              // the host's online XUID (invites name the inviter)
    uint32_t flags = 0;
    uint32_t maxPublic = 0, maxPrivate = 0, filledPublic = 0, filledPrivate = 0;
    uint8_t info[0x3C] = {};            // XSESSION_INFO as the host wrote it
    std::vector<Context> contexts;
    std::vector<Property> properties;
    // Rollback builds: the host's ONLINE NETCODE (display_settings.h) - kNetcodeKnown | mode |
    // custom delay << 8 | custom rollback << 16; 0 = none. The guest picks the battle's input delay
    // with it (NACE lets only the guest request one), so the host's setting decides.
    uint32_t netcode = 0;
};
constexpr uint32_t kNetcodeKnown = 0x80000000u;
// A PAIR PLAY session (online_2v2_pair.cpp): only PAIR PLAY's searches list it, Player Match's never do.
constexpr uint32_t kPairRoom = 0x40000000u;

void Publish(const SessionRecord& record);     // add or update (host)
void Remove(uint64_t id);
// Sessions of other hosts, newest first, at most `max`.
// `contexts` / `openOnly`: what a search wants (a community server filters by them; elsewhere the caller does).
std::vector<SessionRecord> List(uint32_t max, const std::vector<std::pair<uint32_t, uint32_t>>& contexts = {}, bool openOnly = false);
bool Find(uint64_t id, SessionRecord& out);       // also sessions seen in a list or an invite
// A friend's "Join Game" through Steam, once: the session to join (then findable by id).
bool TakeInvite(SessionRecord& out);

} // namespace mm
