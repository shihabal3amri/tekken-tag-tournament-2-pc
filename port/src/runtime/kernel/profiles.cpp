// Local player profiles - see profiles.h.
#include "profiles.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

namespace profiles {
namespace {

constexpr uint64_t kLegacyXuid = 0xB13EBABEBABEBABEull;   // the single profile before 2026-10-12

std::mutex g_mutex;
bool g_loaded = false;
std::vector<Profile> g_list;
int g_active = -1, g_last = 0;
std::atomic<uint32_t> g_generation{ 0 };

std::filesystem::path File()
{
    const wchar_t* overridePath = _wgetenv(L"TTT2_SAVE_ROOT");
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    std::filesystem::path root;
    if (overridePath && *overridePath) root = overridePath;
    else if (local && *local) root = std::filesystem::path(local) / L"TTT2Recomp" / L"saves";
    else return {};
    return root / L"profiles.ini";
}

void SaveLocked()
{
    const std::filesystem::path path = File();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << "[Profiles]\nLast=" << g_last << "\n";
    char line[64];
    for (size_t i = 0; i < g_list.size(); ++i)
    {
        std::snprintf(line, sizeof(line), "P%zu=%016llX ", i, static_cast<unsigned long long>(g_list[i].xuid));
        out << line << g_list[i].name << "\n";
    }
}

void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    const std::filesystem::path path = File();
    std::ifstream in(path);
    std::string line;
    while (in && std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Last=", 0) == 0) g_last = std::atoi(line.c_str() + 5);
        else if (line.size() > 20 && line[0] == 'P' && line.find('=') != std::string::npos)
        {
            const size_t eq = line.find('=');
            Profile p;
            p.xuid = std::strtoull(line.substr(eq + 1, 16).c_str(), nullptr, 16);
            p.name = line.size() > eq + 18 ? Normalize(line.substr(eq + 18)) : std::string();
            if (p.xuid && !p.name.empty() && g_list.size() < kMaxProfiles) g_list.push_back(p);
        }
    }
    if (g_list.empty()) g_list.push_back({ "TTT2", kLegacyXuid });     // the save made before profiles
    g_last = std::clamp(g_last, 0, int(g_list.size()) - 1);
    SaveLocked();
    // TTT2_AUTO_SIGNIN=1 signs the last used profile in; =<16 hex digits> signs in the profile with
    // that XUID (scripted runs use the original save's, B13EBABEBABEBABE, so they never play on a
    // profile the user made). Neither changes "Last".
    const char* autoSignIn = std::getenv("TTT2_AUTO_SIGNIN");
    if (autoSignIn && std::strlen(autoSignIn) == 16)
    {
        const uint64_t want = std::strtoull(autoSignIn, nullptr, 16);
        for (size_t i = 0; i < g_list.size(); ++i)
            if (g_list[i].xuid == want) g_active = int(i);
    }
    else if (autoSignIn && *autoSignIn == '1') g_active = g_last;
    std::fprintf(stderr, "[profiles] %zu profile(s) in %ls, last '%s'%s%s\n", g_list.size(), path.c_str(),
                 g_list[g_last].name.c_str(), g_active >= 0 ? " - signed in (TTT2_AUTO_SIGNIN): " : " - signed out",
                 g_active >= 0 ? g_list[g_active].name.c_str() : "");
}

} // namespace

std::string Normalize(const std::string& name)
{
    std::string out;
    for (char c : name)
    {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool ValidName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxName || Normalize(name) != name) return false;
    for (char c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')) return false;
    return true;
}

std::vector<Profile> List() { std::lock_guard l(g_mutex); LoadLocked(); return g_list; }
bool SignedIn() { std::lock_guard l(g_mutex); LoadLocked(); return g_active >= 0; }
int ActiveIndex() { std::lock_guard l(g_mutex); LoadLocked(); return g_active; }
Profile Active() { std::lock_guard l(g_mutex); LoadLocked(); return g_active >= 0 ? g_list[g_active] : Profile{}; }
uint64_t ActiveXuid() { std::lock_guard l(g_mutex); LoadLocked(); return g_active >= 0 ? g_list[g_active].xuid : 0; }
int LastIndex() { std::lock_guard l(g_mutex); LoadLocked(); return g_last; }
uint32_t Generation() { return g_generation.load(); }

bool SignIn(int index)
{
    std::lock_guard l(g_mutex);
    LoadLocked();
    if (index < 0 || index >= int(g_list.size())) return false;
    g_active = g_last = index;
    SaveLocked();
    g_generation.fetch_add(1);
    std::fprintf(stderr, "[profiles] signed in '%s' (%016llX)\n", g_list[index].name.c_str(),
                 static_cast<unsigned long long>(g_list[index].xuid));
    return true;
}

void SignOut()
{
    std::lock_guard l(g_mutex);
    LoadLocked();
    if (g_active < 0) return;
    std::fprintf(stderr, "[profiles] signed out '%s'\n", g_list[g_active].name.c_str());
    g_active = -1;
    g_generation.fetch_add(1);
}

int Create(const std::string& rawName)
{
    const std::string name = Normalize(rawName);
    std::lock_guard l(g_mutex);
    LoadLocked();
    if (!ValidName(name) || g_list.size() >= kMaxProfiles) return -1;
    for (const Profile& p : g_list)
    {
        std::string a = p.name, b = name;
        for (char& c : a) c = char(std::toupper(static_cast<unsigned char>(c)));
        for (char& c : b) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (a == b) return -1;
    }
    // An offline XUID (E000 + 48 random bits), unique in the list.
    std::random_device rd;
    uint64_t xuid = 0;
    do xuid = 0xE000000000000000ull | ((uint64_t(rd()) << 32 | rd()) & 0x0000FFFFFFFFFFFFull);
    while (std::any_of(g_list.begin(), g_list.end(), [&](const Profile& p) { return p.xuid == xuid; }));
    g_list.push_back({ name, xuid });
    SaveLocked();
    std::fprintf(stderr, "[profiles] created '%s' (%016llX)\n", name.c_str(),
                 static_cast<unsigned long long>(g_list.back().xuid));
    return int(g_list.size()) - 1;
}

int Rename(int index, const std::string& rawName)
{
    const std::string name = Normalize(rawName);
    std::lock_guard l(g_mutex);
    LoadLocked();
    if (index < 0 || index >= int(g_list.size()) || index == g_active || !ValidName(name)) return 1;
    auto upper = [](std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; };
    for (int i = 0; i < int(g_list.size()); ++i)
        if (i != index && upper(g_list[i].name) == upper(name)) return 2;
    std::fprintf(stderr, "[profiles] renamed '%s' to '%s' (%016llX)\n", g_list[index].name.c_str(), name.c_str(),
                 static_cast<unsigned long long>(g_list[index].xuid));
    g_list[index].name = name;
    SaveLocked();
    return 0;
}

} // namespace profiles
