// POSIX adapter for the complete Steam helper protocol; frames never share stdout.
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#include "steam_net.h"
#include "../host_posix_compat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace steamnet {
namespace {

enum : uint8_t { kSend = 1, kPublish = 2, kRemove = 3, kList = 4, kTicket = 5, kCancelTicket = 6, kFriends = 7, kInvite = 8,
                 kReady = 101, kRecv = 102, kListResult = 103, kJoin = 104, kTicketResult = 105, kFriendsResult = 106,
                 kInviteResult = 107 };
constexpr uint32_t kMaxFrame = 16u << 20;

void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
uint32_t Get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t Get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

bool ReadAll(int h, void* buf, size_t n)
{
    auto* p = static_cast<uint8_t*>(buf);
    while (n)
    {
        ssize_t got = ::read(h, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool ReadFrame(int h, uint8_t& type, std::vector<uint8_t>& payload)
{
    uint8_t head[5];
    if (!ReadAll(h, head, 5)) return false;
    const uint32_t size = Get32(head);
    if (size == 0 || size > kMaxFrame) return false;
    type = head[4];
    payload.resize(size - 1);
    return size == 1 || ReadAll(h, payload.data(), payload.size());
}

// One writer per pipe; frames from several threads never interleave.
struct Writer
{
    int h = -1;
    std::mutex m;

    bool WriteAll(const void* buf, size_t n)
    {
        auto* p = static_cast<const uint8_t*>(buf);
        while (n)
        {
            ssize_t put = ::write(h, p, n);
            if (put < 0 && errno == EINTR) continue;
            if (put <= 0) return false;
            p += put;
            n -= put;
        }
        return true;
    }
    bool Frame(uint8_t type, const void* a, size_t an, const void* b = nullptr, size_t bn = 0)
    {
        std::lock_guard l(m);
        if (h == -1 || 1 + an + bn > kMaxFrame) return false;
        uint8_t head[5];
        Put32(head, uint32_t(1 + an + bn));
        head[4] = type;
        return WriteAll(head, 5) && (!an || WriteAll(a, an)) && (!bn || WriteAll(b, bn));
    }
};

Writer g_out;                                   // game: to the helper; helper: to the game

// ---- game side
bool g_inproc = false;                          // TTT2_STEAM_INPROC=1
std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_local{ 0 };
Receiver g_receiver = nullptr;
int g_in = -1;             // from the helper
pid_t g_helper = -1;
std::atomic<uint32_t> g_generation{ 0 };        // the helper started last; an older one's reader leaves no trace
std::atomic<bool> g_restarting{ false };        // Restart runs
std::mutex g_launchMutex;                       // one launch at a time
std::mutex g_hostedMutex;
std::map<uint64_t, std::vector<uint8_t>> g_hostedRecords;   // what this game hosts: published again to a new helper
std::mutex g_mutex;
std::condition_variable g_cv;
int g_ready = 0;                                // 0 waiting, 1 Steam up, -1 failed
bool g_readyFrame = false;                      // the helper reported (READY) - it did not just end
uint32_t g_nextRequest = 1;
std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_lists;
std::map<uint32_t, std::pair<uint32_t, std::string>> g_tickets;   // request -> handle, ticket hex
std::deque<std::vector<uint8_t>> g_joins;
std::map<uint32_t, std::vector<FriendInfo>> g_friendLists;       // request -> friends
std::map<uint32_t, bool> g_invites;                              // request -> sent
std::chrono::steady_clock::time_point g_launched;

void Restart();
void ReportBadHeader(int, const uint8_t head[5])
{
    std::fprintf(stderr, "[steam] invalid helper frame header: %02X %02X %02X %02X %02X\n", head[0], head[1], head[2], head[3], head[4]);
}

void Reader(int in, uint32_t generation)
{
    uint8_t type = 0;
    std::vector<uint8_t> p;
    for (;;)
    {
        uint8_t head[5];
        if (!ReadAll(in, head, 5)) break;
        const uint32_t size = Get32(head);
        if (size == 0 || size > kMaxFrame) { ReportBadHeader(in, head); break; }
        type = head[4];
        p.resize(size - 1);
        if (size > 1 && !ReadAll(in, p.data(), p.size())) break;
        if (type == kRecv && p.size() >= 9)
        {
            if (g_receiver) g_receiver(Get64(p.data()), p[8] != 0, p.data() + 9, p.size() - 9);
        }
        else if (type == kReady && p.size() >= 9)
        {
            std::lock_guard l(g_mutex);
            g_ready = p[0] ? 1 : -1;
            g_readyFrame = true;
            g_local = Get64(p.data() + 1);
            g_cv.notify_all();
        }
        else if (type == kListResult && p.size() >= 8)
        {
            std::vector<std::vector<uint8_t>> records;
            const uint32_t n = Get32(p.data() + 4);
            size_t at = 8;
            for (uint32_t i = 0; i < n && at + 4 <= p.size(); ++i)
            {
                const uint32_t size = Get32(p.data() + at);
                at += 4;
                if (at + size > p.size()) break;
                records.emplace_back(p.begin() + at, p.begin() + at + size);
                at += size;
            }
            std::lock_guard l(g_mutex);
            g_lists[Get32(p.data())] = std::move(records);
            g_cv.notify_all();
        }
        else if (type == kJoin)
        {
            std::lock_guard l(g_mutex);
            g_joins.push_back(p);
        }
        else if (type == kFriendsResult && p.size() >= 8)
        {
            // u32 request, u32 count, count x (u64 id, u8 in this game, u16 name size, name)
            std::vector<FriendInfo> list;
            const uint32_t n = Get32(p.data() + 4);
            size_t at = 8;
            for (uint32_t i = 0; i < n && i < 200 && at + 11 <= p.size(); ++i)
            {
                FriendInfo f;
                f.id = Get64(p.data() + at);
                f.inGame = p[at + 8] != 0;
                const size_t len = size_t(p[at + 9]) | size_t(p[at + 10]) << 8;
                at += 11;
                if (len > 128 || at + len > p.size()) break;
                f.name.assign(reinterpret_cast<const char*>(p.data() + at), len);
                at += len;
                list.push_back(std::move(f));
            }
            std::lock_guard l(g_mutex);
            g_friendLists[Get32(p.data())] = std::move(list);
            g_cv.notify_all();
        }
        else if (type == kInviteResult && p.size() >= 5)
        {
            std::lock_guard l(g_mutex);
            g_invites[Get32(p.data())] = p[4] != 0;
            g_cv.notify_all();
        }
        else if (type == kTicketResult && p.size() >= 8 && p.size() <= 8 + 2 * 2560)
        {
            std::lock_guard l(g_mutex);
            g_tickets[Get32(p.data())] = { Get32(p.data() + 4), std::string(p.begin() + 8, p.end()) };
            g_cv.notify_all();
        }
    }
    ::close(in);
    if (generation != g_generation.load()) return;      // a helper replaced meanwhile
    const bool wasUp = g_running.exchange(false);
    {
        std::lock_guard l(g_mutex);
        if (!g_ready) g_ready = -1;
        g_cv.notify_all();
    }
    if (!wasUp) return;
    std::fprintf(stderr, "[steam] helper process ended after %.0f s; starting a new helper\n",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - g_launched).count());
    if (!g_restarting.exchange(true)) std::thread(Restart).detach();
}

bool Launch()
{
    int toChild[2], fromChild[2];
    if (host_posix::PipeCloexec(toChild)) return false;
    if (host_posix::PipeCloexec(fromChild)) { ::close(toChild[0]); ::close(toChild[1]); return false; }
    // Frames use private descriptors 3 and 4; Steam's stdout goes to the log.
    // Duplicate above 4 first so spawn actions cannot overwrite a pipe they still need.
    int childIn = ::fcntl(toChild[0], F_DUPFD_CLOEXEC, 5);
    int childOut = ::fcntl(fromChild[1], F_DUPFD_CLOEXEC, 5);
    if (childIn < 0 || childOut < 0)
    {
        for (int fd : {toChild[0], toChild[1], fromChild[0], fromChild[1], childIn, childOut}) if (fd >= 0) ::close(fd);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, childIn, 3);
    posix_spawn_file_actions_adddup2(&actions, childOut, 4);
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
#if defined(__APPLE__)
    // No /proc on macOS: spawn the executable by its resolved path.
    char self[PATH_MAX] = {};
    if (!host_posix::ExecutablePath(self, sizeof(self))) std::strcpy(self, "/nonexistent");
#else
    char self[] = "/proc/self/exe";
#endif
    char flag[] = "--steam-bridge", in[] = "3", out[] = "4";
    char* args[] = {self, flag, in, out, nullptr};
    pid_t child = -1;
    int error = posix_spawn(&child, self, &actions, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(toChild[0]); ::close(fromChild[1]); ::close(childIn); ::close(childOut);
    if (error)
    {
        std::fprintf(stderr, "[steam] could not start helper: %s\n", std::strerror(error));
        ::close(toChild[1]); ::close(fromChild[0]);
        return false;
    }
    g_launched = std::chrono::steady_clock::now();
    g_helper = child;
    { std::lock_guard lock(g_out.m); g_out.h = toChild[1]; }
    g_in = fromChild[0];
    return true;
}

void DropHelper()
{
    { std::lock_guard lock(g_out.m); if (g_out.h >= 0) ::close(g_out.h); g_out.h = -1; }
    if (g_helper > 0)
    {
        if (::waitpid(g_helper, nullptr, WNOHANG) == 0)
        {
            ::kill(g_helper, SIGTERM);
            while (::waitpid(g_helper, nullptr, 0) < 0 && errno == EINTR) {}
        }
        g_helper = -1;
    }
}

// Starts a helper and waits for Steam in it (20 s at most). `why` for the log; false: no Steam now.
bool LaunchHelper(const char* why)
{
    std::lock_guard launch(g_launchMutex);
    const uint32_t generation = ++g_generation;         // first: the old helper's reader is stale from here on
    DropHelper();
    {
        std::lock_guard l(g_mutex);
        g_ready = 0;
        g_readyFrame = false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t before = g_local.load();
    if (!Launch()) return false;
    std::thread(Reader, g_in, generation).detach();
    {
        std::unique_lock w(g_mutex);
        if (!g_cv.wait_for(w, std::chrono::seconds(20), [] { return g_ready != 0; }) || g_ready != 1)
        {
            std::fprintf(stderr, "[steam] Steam not available (helper %s)\n",
                         !g_ready ? "timed out" : g_readyFrame ? "reported no Steam" : "ended before Steam was up");
            w.unlock();
            DropHelper();
            return false;
        }
    }
    if (before && g_local.load() != before)
        std::fprintf(stderr, "[steam] the new helper's Steam id %016llX is not the one before (%016llX)\n",
                     (unsigned long long)g_local.load(), (unsigned long long)before);
    g_running = true;
    std::fprintf(stderr, "[steam] Steam runs in helper process %lu (Steam id %016llX), %s in %.1f s\n", static_cast<unsigned long>(g_helper),
                 (unsigned long long)g_local.load(), why, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    // the lobbies this game hosts: the old helper's Steam lobbies went with it
    std::lock_guard h(g_hostedMutex);
    for (const auto& [session, record] : g_hostedRecords)
    {
        uint8_t head[8];
        Put64(head, session);
        g_out.Frame(kPublish, head, 8, record.data(), record.size());
    }
    if (!g_hostedRecords.empty()) std::fprintf(stderr, "[steam] %zu hosted lobbies published again\n", g_hostedRecords.size());
    return true;
}

// The helper was lost while Steam was up: new ones are started, 2 s, then 10, 30, 60 and every 120 s, until one is up.
void Restart()
{
    static const int kDelays[] = { 2, 10, 30, 60, 120 };
    for (int attempt = 0;; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::seconds(kDelays[(std::min)(attempt, 4)]));
        if (LaunchHelper("started again")) break;
    }
    g_restarting = false;
}

} // namespace

bool Start(Receiver receiver)
{
    static std::mutex m;
    std::lock_guard l(m);
    if (Running()) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    if (const char* e = std::getenv("TTT2_STEAM_INPROC"); e && *e == '1')
    {
        g_inproc = true;
        std::fprintf(stderr, "[steam] TTT2_STEAM_INPROC=1: Steam in the game process (controllers will be hidden)\n");
        return direct::Start(receiver);
    }
    std::signal(SIGPIPE, SIG_IGN);
    g_receiver = receiver;
    return LaunchHelper("started");
}

bool Running() { return g_inproc ? direct::Running() : g_running.load(); }

namespace {
// A call that does nothing because the helper ended (issue #30's "no lobbies after the first match"); a new one is
// on its way (Restart).
void NoteStopped(const char* what, std::atomic<int>& logged)
{
    if (g_launched != std::chrono::steady_clock::time_point{} && logged.fetch_add(1) < 10)
        std::fprintf(stderr, "[steam] %s skipped: no helper process right now (a new one is being started)\n", what);
}
} // namespace
uint64_t LocalId() { return g_inproc ? direct::LocalId() : g_local.load(); }

bool Send(uint64_t peer, bool reliable, const void* data, size_t size)
{
    if (g_inproc) return direct::Send(peer, reliable, data, size);
    if (!g_running) { static std::atomic<int> logged{ 0 }; NoteStopped("a packet send", logged); return false; }
    uint8_t head[9];
    Put64(head, peer);
    head[8] = reliable ? 1 : 0;
    return g_out.Frame(kSend, head, 9, data, size);
}

void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record)
{
    if (g_inproc) return direct::PublishLobby(sessionId, record);
    {
        std::lock_guard l(g_hostedMutex);
        g_hostedRecords[sessionId] = record;            // a new helper publishes it again
    }
    if (!g_running) { static std::atomic<int> logged{ 0 }; NoteStopped("publishing a hosted lobby", logged); return; }
    uint8_t head[8];
    Put64(head, sessionId);
    g_out.Frame(kPublish, head, 8, record.data(), record.size());
}

void RemoveLobby(uint64_t sessionId)
{
    if (g_inproc) return direct::RemoveLobby(sessionId);
    {
        std::lock_guard l(g_hostedMutex);
        g_hostedRecords.erase(sessionId);
    }
    if (!g_running) return;
    uint8_t head[8];
    Put64(head, sessionId);
    g_out.Frame(kRemove, head, 8);
}

std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs)
{
    if (g_inproc) return direct::ListLobbies(max, timeoutMs);
    std::vector<std::vector<uint8_t>> out;
    if (!g_running) { static std::atomic<int> logged{ 0 }; NoteStopped("a lobby search", logged); return out; }
    uint32_t request;
    {
        std::lock_guard l(g_mutex);
        request = g_nextRequest++;
    }
    uint8_t body[12];
    Put32(body, request);
    Put32(body + 4, uint32_t(max));
    Put32(body + 8, uint32_t(timeoutMs));
    if (!g_out.Frame(kList, body, 12)) return out;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs + 3000), [&] { return g_lists.count(request) || !g_running; });
    const auto it = g_lists.find(request);
    if (it != g_lists.end())
    {
        out = std::move(it->second);
        g_lists.erase(it);
    }
    return out;
}

bool WebApiTicket(const std::string& identity, std::string& hex, uint32_t& handle, int timeoutMs)
{
    if (g_inproc) return direct::WebApiTicket(identity, hex, handle, timeoutMs);
    if (!g_running || identity.size() > 256) return false;
    uint32_t request;
    {
        std::lock_guard l(g_mutex);
        request = g_nextRequest++;
    }
    std::vector<uint8_t> body(4 + identity.size());
    Put32(body.data(), request);
    std::memcpy(body.data() + 4, identity.data(), identity.size());
    if (!g_out.Frame(kTicket, body.data(), body.size())) return false;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs + 3000), [&] { return g_tickets.count(request) || !g_running; });
    const auto it = g_tickets.find(request);
    if (it == g_tickets.end()) return false;
    handle = it->second.first;
    hex = std::move(it->second.second);
    g_tickets.erase(it);
    return handle != 0 && !hex.empty();
}

void CancelTicket(uint32_t handle)
{
    if (g_inproc) { direct::CancelTicket(handle); return; }
    uint8_t body[4];
    Put32(body, handle);
    if (g_running && handle) g_out.Frame(kCancelTicket, body, 4);
}

std::vector<FriendInfo> Friends(int timeoutMs)
{
    if (g_inproc) return direct::Friends();
    if (!g_running) return {};
    uint32_t request;
    {
        std::lock_guard l(g_mutex);
        request = g_nextRequest++;
    }
    uint8_t body[4];
    Put32(body, request);
    if (!g_out.Frame(kFriends, body, 4)) return {};
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_friendLists.count(request) || !g_running; });
    std::vector<FriendInfo> out;
    if (const auto it = g_friendLists.find(request); it != g_friendLists.end())
    {
        out = std::move(it->second);
        g_friendLists.erase(it);
    }
    return out;
}

bool InviteToSession(uint64_t sessionId, uint64_t friendId, int timeoutMs)
{
    if (g_inproc) return direct::InviteToSession(sessionId, friendId);
    if (!g_running) return false;
    uint32_t request;
    {
        std::lock_guard l(g_mutex);
        request = g_nextRequest++;
    }
    uint8_t body[20];
    Put32(body, request);
    Put64(body + 4, sessionId);
    Put64(body + 12, friendId);
    if (!g_out.Frame(kInvite, body, 20)) return false;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_invites.count(request) || !g_running; });
    const auto it = g_invites.find(request);
    if (it == g_invites.end()) return false;
    const bool sent = it->second;
    g_invites.erase(it);
    return sent;
}

bool TakeJoinRequest(std::vector<uint8_t>& record)
{
    if (g_inproc) return direct::TakeJoinRequest(record);
    std::lock_guard l(g_mutex);
    if (g_joins.empty()) return false;
    record = std::move(g_joins.front());
    g_joins.pop_front();
    return true;
}

// ---- helper side
int RunBridge(int argc, char** argv)
{
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // EOF on the private pipe ends the helper with its game. PDEATHSIG would
    // also fire when the restart worker thread exits, killing a healthy helper.
    std::signal(SIGPIPE, SIG_IGN);
    // TTT2_STEAM_HELPER_CRASHTEST=1 (diagnostics): the helper crashes before Steam starts - the crash report
    // and the game's exit-code line can be checked without Steam.
    if (const char* e = std::getenv("TTT2_STEAM_HELPER_CRASHTEST"); e && *e == '1')
    {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    // the frame pipes: their handle values from the game (stdout is the log - Steam's libraries print there)
    int in = -1;
    g_out.h = -1;
    if (argc >= 4)
    {
        in = int(std::strtol(argv[2], nullptr, 10));
        g_out.h = int(std::strtol(argv[3], nullptr, 10));
    }
    if (in < 0 || g_out.h < 0) return 2;
    // TTT2_STEAM_HELPER_FAKE=1 (tests of the bridge itself - no Steam is touched): READY with a made-up Steam id,
    // sends dropped, lobbies only logged, searches empty. With it, TTT2_STEAM_HELPER_JUNK_AFTER=<s> prints text to
    // stdout after s seconds (it must reach the log, not the frames) and TTT2_STEAM_HELPER_EXIT_AFTER=<s> ends the
    // helper after s seconds (the game must start a new one).
    const char* fakeEnv = std::getenv("TTT2_STEAM_HELPER_FAKE");
    const bool fake = fakeEnv && *fakeEnv == '1';
    constexpr uint64_t kFakeId = 0x0110000100000F0Full;
    bool ok = false;
    if (fake)
    {
        ok = true;
        std::fprintf(stderr, "[steam-helper] fake Steam (TTT2_STEAM_HELPER_FAKE=1): no Steam in this helper\n");
        if (const char* e = std::getenv("TTT2_STEAM_HELPER_JUNK_AFTER"))
            std::thread([s = std::atoi(e)] {
                std::this_thread::sleep_for(std::chrono::seconds(s));
                std::printf("src\\steamnetworkingsockets\\clientlib\\steamnetworkingsockets_p2p_ice.cpp (628) : false\n");
                std::fflush(stdout);
                std::fprintf(stderr, "[steam-helper] fake: printed a line to stdout\n");
            }).detach();
        if (const char* e = std::getenv("TTT2_STEAM_HELPER_EXIT_AFTER"))
            std::thread([s = std::atoi(e)] {
                std::this_thread::sleep_for(std::chrono::seconds(s));
                std::fprintf(stderr, "[steam-helper] fake: ending the helper\n");
                std::_Exit(9);
            }).detach();
    }
    else
        ok = direct::Start([](uint64_t from, bool reliable, const uint8_t* data, size_t size) {
            uint8_t head[9];
            Put64(head, from);
            head[8] = reliable ? 1 : 0;
            g_out.Frame(kRecv, head, 9, data, size);
        });

    uint8_t ready[9] = { uint8_t(ok ? 1 : 0) };
    Put64(ready + 1, ok ? (fake ? kFakeId : direct::LocalId()) : 0);
    g_out.Frame(kReady, ready, 9);
    if (!ok) return 1;
    std::thread([] {
        for (;;)
        {
            std::vector<uint8_t> record;
            if (direct::TakeJoinRequest(record)) g_out.Frame(kJoin, record.data(), record.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
    uint8_t type = 0;
    std::vector<uint8_t> p;
    while (ReadFrame(in, type, p))
    {
        if (type == kSend && p.size() >= 9)
            direct::Send(Get64(p.data()), p[8] != 0, p.data() + 9, p.size() - 9);
        else if (type == kPublish && p.size() >= 8)
        {
            if (fake) std::fprintf(stderr, "[steam-helper] fake: lobby of session %016llX published\n", (unsigned long long)Get64(p.data()));
            direct::PublishLobby(Get64(p.data()), std::vector<uint8_t>(p.begin() + 8, p.end()));
        }
        else if (type == kRemove && p.size() >= 8)
            direct::RemoveLobby(Get64(p.data()));
        else if (type == kTicket && p.size() >= 4 && p.size() <= 4 + 256)
        {
            const uint32_t request = Get32(p.data());
            std::string identity(p.begin() + 4, p.end());
            std::thread([request, identity] {
                std::string hex;
                uint32_t handle = 0;
                if (!direct::WebApiTicket(identity, hex, handle, 10000)) handle = 0, hex.clear();
                std::vector<uint8_t> body(8 + hex.size());
                Put32(body.data(), request);
                Put32(body.data() + 4, handle);
                if (!hex.empty()) std::memcpy(body.data() + 8, hex.data(), hex.size());
                g_out.Frame(kTicketResult, body.data(), body.size());
            }).detach();
        }
        else if (type == kCancelTicket && p.size() >= 4)
            direct::CancelTicket(Get32(p.data()));
        else if (type == kFriends && p.size() >= 4)
        {
            const uint32_t request = Get32(p.data());
            std::vector<uint8_t> body(8);
            Put32(body.data(), request);
            const std::vector<FriendInfo> list = direct::Friends();
            Put32(body.data() + 4, uint32_t(list.size()));
            for (const FriendInfo& f : list)
            {
                const size_t len = (std::min)(f.name.size(), size_t(128)), at = body.size();
                body.resize(at + 11 + len);
                Put64(body.data() + at, f.id);
                body[at + 8] = f.inGame ? 1 : 0;
                body[at + 9] = uint8_t(len);
                body[at + 10] = uint8_t(len >> 8);
                if (len) std::memcpy(body.data() + at + 11, f.name.data(), len);
            }
            g_out.Frame(kFriendsResult, body.data(), body.size());
        }
        else if (type == kInvite && p.size() >= 20)
        {
            uint8_t body[5];
            Put32(body, Get32(p.data()));
            body[4] = direct::InviteToSession(Get64(p.data() + 4), Get64(p.data() + 12)) ? 1 : 0;
            g_out.Frame(kInviteResult, body, 5);
        }
        else if (type == kList && p.size() >= 12)
        {
            const uint32_t request = Get32(p.data());
            const int max = int(Get32(p.data() + 4)), timeoutMs = int(Get32(p.data() + 8));
            std::thread([request, max, timeoutMs] {
                const auto records = direct::ListLobbies(max, timeoutMs);
                std::vector<uint8_t> body(8);
                Put32(body.data(), request);
                Put32(body.data() + 4, uint32_t(records.size()));
                for (const auto& r : records)
                {
                    const size_t at = body.size();
                    body.resize(at + 4 + r.size());
                    Put32(body.data() + at, uint32_t(r.size()));
                    if (!r.empty()) std::memcpy(body.data() + at + 4, r.data(), r.size());
                }
                g_out.Frame(kListResult, body.data(), body.size());
            }).detach();
        }
    }
    // The game closed its end (it exited or crashed): Steam leaves the lobbies with this process.
    std::fprintf(stderr, "[steam-helper] the game closed its pipe - the helper exits\n");
    return 0;
}

} // namespace steamnet
