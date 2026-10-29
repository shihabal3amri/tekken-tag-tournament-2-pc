// Keyboard key settings (see keyboard_keys.h). The input poll thread reads the keys, the game's
// main thread changes them: one atomic per control.
#include "keyboard_keys.h"
#include "host_keycodes.h"
#include "host_settings.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace keyboard_keys {
namespace {

struct Info { const char* ini; const char* name; uint16_t mask; int defaultKey; };
// Defaults: the keys the port always had (arrows, A S Z X, Q W, ENTER, BACKSPACE) plus E R D F for
// the controls that had none.
const Info kInfo[Count] = {
    { "Up", "UP", 0x0001, VK_UP },          { "Down", "DOWN", 0x0002, VK_DOWN },
    { "Left", "LEFT", 0x0004, VK_LEFT },    { "Right", "RIGHT", 0x0008, VK_RIGHT },
    { "A", "A", 0x1000, 'A' },              { "B", "B", 0x2000, 'S' },
    { "X", "X", 0x4000, 'Z' },              { "Y", "Y", 0x8000, 'X' },
    { "LB", "LB", 0x0100, 'Q' },            { "RB", "RB", 0x0200, 'W' },
    { "LT", "LT", 0, 'E' },                 { "RT", "RT", 0, 'R' },
    { "LS", "LS", 0x0040, 'D' },            { "RS", "RS", 0x0080, 'F' },
    { "Start", "START", 0x0010, VK_RETURN }, { "Back", "BACK", 0x0020, VK_BACK },
};

std::atomic<int> g_keys[Count];
std::once_flag g_loaded;
std::mutex g_writeMutex;

void Load()
{
    std::call_once(g_loaded, [] {
        for (int c = 0; c < Count; ++c)
        {
            const std::string value = host_settings::Read("Keyboard", kInfo[c].ini, "");
            int vk = kInfo[c].defaultKey;
            if (!value.empty()) vk = int(std::strtol(value.c_str(), nullptr, 0));
            g_keys[c] = (vk >= 0 && vk < 256) ? vk : kInfo[c].defaultKey;
        }
        // Two controls on one key (an edited file): the later one loses the key.
        for (int c = 0; c < Count; ++c)
            for (int d = 0; d < c; ++d)
                if (g_keys[c] && g_keys[c] == g_keys[d]) g_keys[c] = 0;
        std::string line = "[keyboard] keys:";
        for (int c = 0; c < Count; ++c) line += std::string(" ") + kInfo[c].name + "=" + KeyName(g_keys[c]);
        std::fprintf(stderr, "%s\n", line.c_str());
    });
}

void Save()
{
    for (int c = 0; c < Count; ++c)
    {
        char value[16];
        std::snprintf(value, sizeof(value), "0x%02X", unsigned(g_keys[c].load()));
        host_settings::Write("Keyboard", kInfo[c].ini, value);
    }
}

} // namespace

bool SeparatePlayer()
{
    static const bool separate = host_settings::ReadInt("Keyboard", "SeparatePlayer", 0) == 1;
    return separate;
}

int PlayerOnePreference()
{
    static const int preference = [] {
        const unsigned value = host_settings::ReadInt("Input", "PlayerOnePreference", 0);
        return value <= 2 ? int(value) : 0;
    }();
    return preference;
}

int SOCD()
{
    static const int mode = [] {
        const unsigned value = host_settings::ReadInt("Keyboard", "SOCD", 0);
        return value <= 3 ? int(value) : 0;
    }();
    return mode;
}

bool DPadOnly()
{
    static const bool on = host_settings::ReadInt("Input", "DPadOnly", 0) == 1;
    return on;
}
int StickDeadzone()
{
    static const int percent = [] {
        const unsigned value = host_settings::ReadInt("Input", "StickDeadzone", 0);
        return value <= 95 ? int(value) : 0;
    }();
    return percent;
}

int StickDiagonal()
{
    static const int percent = [] {
        const unsigned value = host_settings::ReadInt("Input", "StickDiagonal", 0);
        return value <= 100 ? int(value) : 0;
    }();
    return percent;
}

int Key(int control)
{
    Load();
    return control >= 0 && control < Count ? g_keys[control].load() : 0;
}

bool Bound(int vk)
{
    Load();
    for (int c = 0; c < Count; ++c)
        if (vk && g_keys[c].load() == vk) return true;
    return false;
}

void Assign(int control, int vk)
{
    Load();
    if (control < 0 || control >= Count || !Assignable(vk)) return;
    std::lock_guard<std::mutex> l(g_writeMutex);
    const int old = g_keys[control].load();
    if (old == vk) return;
    for (int c = 0; c < Count; ++c)
        if (c != control && g_keys[c].load() == vk) g_keys[c] = old;      // swap
    g_keys[control] = vk;
    Save();
    std::fprintf(stderr, "[keyboard] %s = %s\n", kInfo[control].name, KeyName(vk).c_str());
}

void RestoreDefaults()
{
    Load();
    std::lock_guard<std::mutex> l(g_writeMutex);
    for (int c = 0; c < Count; ++c) g_keys[c] = kInfo[c].defaultKey;
    Save();
    std::fprintf(stderr, "[keyboard] default keys restored\n");
}

uint16_t Mask(int control) { return control >= 0 && control < Count ? kInfo[control].mask : 0; }
const char* ControlName(int control) { return control >= 0 && control < Count ? kInfo[control].name : "?"; }

bool Assignable(int vk)
{
    // ESC keeps a key in the guided setup; the Windows keys and the lock / system keys do not
    // reach a game reliably.
    switch (vk)
    {
    case 0: case VK_ESCAPE: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT: case VK_SCROLL:
    case VK_PAUSE: case VK_NUMLOCK: case VK_CAPITAL: case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
        return false;
    default:
        return vk > 0 && vk < 256;
    }
}

std::string KeyName(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk)
    {
    case 0: return "-";
    case VK_UP: return "UP";
    case VK_DOWN: return "DOWN";
    case VK_LEFT: return "LEFT";
    case VK_RIGHT: return "RIGHT";
    case VK_RETURN: return "ENTER";
    case VK_SPACE: return "SPACE";
    case VK_BACK: return "BACKSPACE";
    case VK_TAB: return "TAB";
    case VK_LSHIFT: return "L SHIFT";
    case VK_RSHIFT: return "R SHIFT";
    case VK_LCONTROL: return "L CTRL";
    case VK_RCONTROL: return "R CTRL";
    case VK_LMENU: return "L ALT";
    case VK_RMENU: return "R ALT";
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PAGE UP";
    case VK_NEXT: return "PAGE DOWN";
    case VK_MULTIPLY: return "NUM *";
    case VK_ADD: return "NUM +";
    case VK_SUBTRACT: return "NUM -";
    case VK_DECIMAL: return "NUM .";
    case VK_DIVIDE: return "NUM /";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "MINUS";                 // "-" means no key
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    default:
    {
        char name[32];
        std::snprintf(name, sizeof(name), "KEY %02X", unsigned(vk));
        return name;
    }
    }
}

} // namespace keyboard_keys
