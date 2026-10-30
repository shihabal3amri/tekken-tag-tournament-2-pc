"""Exercise production input polling with synthetic keyboard and XInput devices.

No window, hardware pad, real keystroke or game data is needed. Host sampling is
mocked; source selection, backoff, publication and packet numbers are production, and
so are the keyboard key settings (keyboard_keys.cpp, kept in a temporary settings.ini).
"""

import os
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]

CHECKER = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
std::array<bool,256> keys{};
bool physical[4]{};
XINPUT_STATE host[4]{};
XINPUT_CAPABILITIES capabilities[4]{};
unsigned samples[4]{},capsSamples[4]{};
bool focused=true,windowExists=true,textEntry=false;
bool SysUiTextEntryActive() { return textEntry; }
bool captureActive=false;
bool SysUiKeyCaptureActive() { return captureActive; }
HWND TestForegroundWindow() { return windowExists ? reinterpret_cast<HWND>(1) : nullptr; }
DWORD TestWindowProcess(HWND,LPDWORD pid) { *pid=GetCurrentProcessId()+(focused?0:1);return 1; }
SHORT ReadTestKey(int key) { return keys.at(key) ? SHORT(0x8000) : 0; }
DWORD SampleTestPad(DWORD index,XINPUT_STATE* out) {
    ++samples[index];
    if(!physical[index]) return ERROR_DEVICE_NOT_CONNECTED;
    *out=host[index];return ERROR_SUCCESS;
}
DWORD SampleTestCaps(DWORD index,DWORD,XINPUT_CAPABILITIES* out) {
    ++capsSamples[index];*out=capabilities[index];return ERROR_SUCCESS;
}
@KEYS@
#define GetAsyncKeyState ReadTestKey
#define GetForegroundWindow TestForegroundWindow
#define GetWindowThreadProcessId TestWindowProcess
#define XInputGetState SampleTestPad
#define XInputGetCapabilities SampleTestCaps
@PRODUCTION@
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
void Reset() {
    keys.fill(false);
    focused=windowExists=true;
    for(unsigned i=0;i<4;++i) {
        physical[i]=false;host[i]={};capabilities[i]={};samples[i]=capsSamples[i]=0;
        g_pads[i].connected=false;g_pads[i].physical=false;g_pads[i].state={};g_pads[i].caps={};
    }
}
int main() {
    CHECK(!AutoStartEnabled(nullptr));
    for(const char* value : {"", "0", "false", "off", "01", " 1", "1 "}) {
        CHECK(!AutoStartEnabled(value));
        Reset();PadPollState disabled;
        PollPadsOnce(disabled,AutoStartEnabled(value),54001);
        CHECK(!g_pads[0].connected && g_pads[0].state.Gamepad.wButtons==0);
    }
    CHECK(AutoStartEnabled("1"));
    Reset();PadPollState poll;
    PollPadsOnce(poll,false,0);
    for(unsigned i=0;i<4;++i) CHECK(samples[i]==1 && !g_pads[i].connected);
    for(unsigned tick=1;tick<=100;++tick) PollPadsOnce(poll,false,tick*8);
    CHECK(samples[0]==1 && !g_pads[0].connected); // Pad 0 was probed before any key.

    keys[VK_RETURN]=true;PollPadsOnce(poll,false,1000);
    CHECK(g_pads[0].connected && !g_pads[0].physical && g_pads[0].state.Gamepad.wButtons==0x10);
    CHECK(g_pads[0].caps.Gamepad.wButtons==0xF3FF);
    DWORD packet=g_pads[0].state.dwPacketNumber;
    for(unsigned tick=0;tick<400;++tick) {
        PollPadsOnce(poll,false,1010+tick*8);
        CHECK(g_pads[0].connected && g_pads[0].state.dwPacketNumber==packet);
    }
    CHECK(samples[0]>=2 && samples[0]<=3); // Physical retry continues without flapping keyboard.
    keys[VK_RETURN]=false;PollPadsOnce(poll,false,5000);
    CHECK(g_pads[0].connected && g_pads[0].state.Gamepad.wButtons==0);
    CHECK(g_pads[0].caps.Gamepad.wButtons==0xF3FF && g_pads[0].caps.Gamepad.bLeftTrigger==255);
    CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    packet=g_pads[0].state.dwPacketNumber;
    PollPadsOnce(poll,false,5008);CHECK(g_pads[0].state.dwPacketNumber==packet);

    // Hot-plug discovery does not depend on the keyboard's connected flag.
    physical[0]=true;host[0].dwPacketNumber=packet;host[0].Gamepad.wButtons=0x1000;
    host[0].Gamepad.bLeftTrigger=90;host[0].Gamepad.sThumbLX=-1234;
    capabilities[0].Flags=0x55;keys[VK_RETURN]=true;
    unsigned attempts=0;
    while(!poll.physicalConnected[0] && attempts++<252) PollPadsOnce(poll,false,6000+attempts*8);
    CHECK(poll.physicalConnected[0] && g_pads[0].connected && g_pads[0].physical);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x1010 && g_pads[0].caps.Flags==0x55);
    CHECK(g_pads[0].state.Gamepad.bLeftTrigger==90 && g_pads[0].state.Gamepad.sThumbLX==-1234);
    CHECK(g_pads[0].state.dwPacketNumber!=packet); // Host counter collision cannot hide the handoff.
    packet=g_pads[0].state.dwPacketNumber;
    PollPadsOnce(poll,false,54000); // Keyboard and physical buttons share player 0.
    CHECK(g_pads[0].state.Gamepad.wButtons==0x1010 && g_pads[0].state.dwPacketNumber==packet);
    keys[VK_RETURN]=false;PollPadsOnce(poll,false,54000);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x1000 && g_pads[0].state.dwPacketNumber==packet+1);
    packet=g_pads[0].state.dwPacketNumber;
    PollPadsOnce(poll,true,54000); // Explicit unattended mode also exercises physical pad 0.
    CHECK(g_pads[0].physical && g_pads[0].state.Gamepad.wButtons==0x1010);
    CHECK(g_pads[0].state.Gamepad.bLeftTrigger==90 && g_pads[0].state.Gamepad.sThumbLX==-1234);
    CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    PollPadsOnce(poll,true,54299);CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    PollPadsOnce(poll,true,54300);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x1000 && g_pads[0].state.dwPacketNumber==packet+2);
    CHECK(host[0].Gamepad.wButtons==0x1000); // Do not modify the sampled device state.
    packet=g_pads[0].state.dwPacketNumber;
    host[0].Gamepad.sThumbLX=-1235;PollPadsOnce(poll,false,7000);
    CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    packet=g_pads[0].state.dwPacketNumber;
    physical[0]=false;keys[VK_RETURN]=false;PollPadsOnce(poll,false,7008);
    CHECK(g_pads[0].connected && !g_pads[0].physical && g_pads[0].state.Gamepad.wButtons==0);
    CHECK(g_pads[0].state.Gamepad.bLeftTrigger==0 && g_pads[0].state.Gamepad.sThumbLX==0);
    CHECK(g_pads[0].state.dwPacketNumber==packet+1);

    // A real pad already present at startup must be found without a keypress.
    Reset();poll={};physical[0]=true;physical[1]=true;
    host[0].Gamepad.wButtons=0x2000;host[1].Gamepad.wButtons=0x4000;
    PollPadsOnce(poll,false,0);
    CHECK(g_pads[0].physical && g_pads[0].connected && samples[0]==1);
    CHECK(g_pads[1].physical && g_pads[1].connected);
    PollPadsOnce(poll,true,54000);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x2010);
    CHECK(g_pads[1].state.Gamepad.wButtons==0x4000); // Pulse only controller 0.
    physical[0]=physical[1]=false;PollPadsOnce(poll,false,8);
    CHECK(!g_pads[0].connected && !g_pads[1].connected); // Keyboard never activated in this scenario.
    keys['A']=true;PollPadsOnce(poll,false,16);
    CHECK(g_pads[0].connected && !g_pads[1].connected && g_pads[0].state.Gamepad.wButtons==0x1000);
    // Even identical held buttons need a new packet at a source handoff.
    packet=g_pads[0].state.dwPacketNumber;physical[0]=true;host[0].Gamepad.wButtons=0x1000;
    poll.disconnectedBackoff[0]=0;PollPadsOnce(poll,false,24);
    CHECK(g_pads[0].physical && g_pads[0].state.dwPacketNumber==packet+1);
    physical[0]=false;PollPadsOnce(poll,false,32);
    CHECK(!g_pads[0].physical && g_pads[0].state.dwPacketNumber==packet+2);

    // Focus loss releases only keyboard buttons, with correct packet edges.
    Reset();poll={};physical[0]=physical[1]=true;
    host[0].Gamepad.wButtons=0x2000;host[0].Gamepad.sThumbLX=1234;
    host[1].Gamepad.wButtons=0x4000;keys['A']=true;
    PollPadsOnce(poll,false,0);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x3000);
    CHECK(g_pads[1].state.Gamepad.wButtons==0x4000);
    packet=g_pads[0].state.dwPacketNumber;focused=false;
    PollPadsOnce(poll,false,8);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x2000 && g_pads[0].state.Gamepad.sThumbLX==1234);
    CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    PollPadsOnce(poll,false,16);CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    focused=true;PollPadsOnce(poll,false,24);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x3000 && g_pads[0].state.dwPacketNumber==packet+2);
    windowExists=false;PollPadsOnce(poll,false,32);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x2000 && g_pads[0].state.dwPacketNumber==packet+3);
    Reset();poll={};focused=false;keys['A']=true;PollPadsOnce(poll,false,0);
    CHECK(!g_pads[0].connected); // Background typing cannot create a keyboard pad.
    focused=true;PollPadsOnce(poll,false,8);CHECK(g_pads[0].connected);
    packet=g_pads[0].state.dwPacketNumber;focused=false;PollPadsOnce(poll,false,16);
    CHECK(g_pads[0].connected && g_pads[0].state.Gamepad.wButtons==0 && g_pads[0].state.dwPacketNumber==packet+1);

    // The controller actually used is player 1 whatever its XInput slot: with an idle device on
    // slot 0 the real pad on slot 1 used to be user 1, which TTT2 runs signed out (no save, no
    // title-storage unlock).
    Reset();poll={};physical[0]=physical[1]=true;
    PollPadsOnce(poll,false,0);
    CHECK(g_pads[0].physical && g_pads[1].physical); // Unused pads fill users in slot order.
    host[1].Gamepad.wButtons=0x0010;PollPadsOnce(poll,false,8);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x0010 && g_pads[1].state.Gamepad.wButtons==0);
    CHECK(poll.guestOf[1]==0 && poll.guestOf[0]==-1);
    host[1].Gamepad.wButtons=0;host[1].Gamepad.sThumbLX=500;PollPadsOnce(poll,false,16);
    CHECK(g_pads[0].state.Gamepad.sThumbLX==500 && g_pads[1].state.Gamepad.sThumbLX==0);
    keys['A']=true;PollPadsOnce(poll,false,24);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x1000); // The keyboard joins player 1.
    keys['A']=false;host[0].Gamepad.wButtons=0x2000;PollPadsOnce(poll,false,32);
    CHECK(g_pads[1].state.Gamepad.wButtons==0x2000 && g_pads[0].state.Gamepad.wButtons==0);
    CHECK(poll.guestOf[0]==1); // The second pad used is player 2.
    // A claimed pad keeps its user across a disconnect; nobody else takes it meanwhile.
    physical[1]=false;PollPadsOnce(poll,false,40);
    CHECK(g_pads[0].connected && !g_pads[0].physical && g_pads[1].physical); // keyboard keeps player 1
    physical[1]=true;poll.disconnectedBackoff[1]=0;host[1].Gamepad.wButtons=0x4000;
    PollPadsOnce(poll,false,48);
    CHECK(g_pads[0].physical && g_pads[0].state.Gamepad.wButtons==0x4000);
    // Stick drift does not claim a user; a trigger press does.
    Reset();poll={};physical[0]=physical[1]=true;host[0].Gamepad.sThumbLX=9000;
    PollPadsOnce(poll,false,0);
    CHECK(poll.guestOf[0]==-1 && g_pads[0].state.Gamepad.sThumbLX==9000);
    host[1].Gamepad.bRightTrigger=200;PollPadsOnce(poll,false,8);
    CHECK(g_pads[0].state.Gamepad.bRightTrigger==200 && g_pads[1].state.Gamepad.sThumbLX==9000);
    // Scripted and unattended presses still go to player 1 (here the claimed slot-1 pad).
    PollPadsOnce(poll,true,45001);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x0010 && g_pads[1].state.Gamepad.wButtons==0);

    // Scripted player-2 presses (Pair Play tests) drive user 1 as a virtual pad.
    Reset();poll={};
    PadScript script;LoadScript(script,"100:P2+A:50,100:B:50,300:P2+DOWN+X:50");
    CHECK(script.player2 && script.presses.size()==3);
    PollPadsOnce(poll,false,120,&script);
    CHECK(g_pads[1].connected && !g_pads[1].physical && g_pads[1].state.Gamepad.wButtons==0x1000);
    CHECK(g_pads[0].connected && g_pads[0].state.Gamepad.wButtons==0x2000);
    PollPadsOnce(poll,false,200,&script);
    CHECK(g_pads[1].connected && g_pads[1].state.Gamepad.wButtons==0 && g_pads[0].state.Gamepad.wButtons==0);
    PollPadsOnce(poll,false,310,&script);
    CHECK(g_pads[1].state.Gamepad.wButtons==0x4002 && g_pads[0].state.Gamepad.wButtons==0);
    physical[1]=true;poll.disconnectedBackoff[1]=0;host[1].Gamepad.wButtons=0x0100;PollPadsOnce(poll,false,320,&script);
    CHECK(g_pads[0].physical && g_pads[0].state.Gamepad.wButtons==0x0100); // a used real pad is still player 1
    CHECK(g_pads[1].connected && g_pads[1].state.Gamepad.wButtons==0x4002);

    // Existing unattended timing: >45 s then 300-ms pulses at nine-second intervals.
    Reset();poll={};PollPadsOnce(poll,true,0);
    CHECK(g_pads[0].connected && g_pads[0].state.Gamepad.wButtons==0);
    packet=g_pads[0].state.dwPacketNumber;
    PollPadsOnce(poll,true,45000);CHECK(g_pads[0].state.dwPacketNumber==packet);
    PollPadsOnce(poll,true,45001);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x10 && g_pads[0].state.dwPacketNumber==packet+1);
    PollPadsOnce(poll,true,45299);CHECK(g_pads[0].state.dwPacketNumber==packet+1);
    PollPadsOnce(poll,true,45300);
    CHECK(g_pads[0].state.Gamepad.wButtons==0 && g_pads[0].state.dwPacketNumber==packet+2);
    PollPadsOnce(poll,true,54000);CHECK(g_pads[0].state.Gamepad.wButtons==0x10);
    PollPadsOnce(poll,true,54300);CHECK(g_pads[0].state.Gamepad.wButtons==0);

    // While the name dialog takes typed text, letter / ENTER keys are not buttons; arrows are.
    Reset();poll={};textEntry=true;keys['A']=keys['S']=keys[VK_RETURN]=keys[VK_UP]=true;
    PollPadsOnce(poll,false,0);
    CHECK(g_pads[0].connected && g_pads[0].state.Gamepad.wButtons==0x0001);
    textEntry=false;PollPadsOnce(poll,false,8);
    CHECK(g_pads[0].state.Gamepad.wButtons==(0x0001|0x0010|0x1000|0x2000));
    // Key settings: the default keys reach every control (E / R triggers, D / F stick clicks), a
    // new key replaces the old one, a taken key swaps, Space is START only while unbound, and
    // nothing is mapped while a key is being chosen.
    textEntry=false;keyboard_keys::RestoreDefaults();
    Reset();poll={};keys['E']=keys['D']=true;PollPadsOnce(poll,false,0);
    CHECK(g_pads[0].state.Gamepad.bLeftTrigger==255 && g_pads[0].state.Gamepad.bRightTrigger==0);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x0040);
    keys['E']=keys['D']=false;keys['R']=keys['F']=true;PollPadsOnce(poll,false,8);
    CHECK(g_pads[0].state.Gamepad.bRightTrigger==255 && g_pads[0].state.Gamepad.bLeftTrigger==0);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x0080);
    keys['R']=keys['F']=false;keys[VK_SPACE]=true;PollPadsOnce(poll,false,16);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x0010);
    keyboard_keys::Assign(keyboard_keys::X,'J');
    keys[VK_SPACE]=false;keys['J']=true;PollPadsOnce(poll,false,24);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x4000);
    keys['J']=false;keys['Z']=true;PollPadsOnce(poll,false,32);
    CHECK(g_pads[0].state.Gamepad.wButtons==0 && !keyboard_keys::Bound('Z'));
    keyboard_keys::Assign(keyboard_keys::X,'S');   // B's key: B takes J
    CHECK(keyboard_keys::Key(keyboard_keys::X)=='S' && keyboard_keys::Key(keyboard_keys::B)=='J');
    keys['Z']=false;keys['S']=true;PollPadsOnce(poll,false,40);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x4000);
    keyboard_keys::Assign(keyboard_keys::LB,VK_SPACE);
    keys['S']=false;keys[VK_SPACE]=true;PollPadsOnce(poll,false,48);
    CHECK(g_pads[0].state.Gamepad.wButtons==0x0100); // Space is LB now, not START
    captureActive=true;PollPadsOnce(poll,false,56);
    CHECK(g_pads[0].state.Gamepad.wButtons==0);
    captureActive=false;CHECK(!keyboard_keys::Assignable(VK_ESCAPE));
    keyboard_keys::RestoreDefaults();
    CHECK(keyboard_keys::Key(keyboard_keys::X)=='Z' && keyboard_keys::Key(keyboard_keys::LB)=='Q');
    std::printf("PASS %u input checks: focused keyboard/controller combination, hotplug, packet edges, START timing, player 1 = first pad used, text entry, key settings\n",checks);
}
'''


def main():
    kernel = ROOT / "port/src/runtime/kernel"
    text = (kernel / "kernel_xam.cpp").read_text(encoding="utf-8-sig")
    begin, end = text.index("struct PadSnapshot"), text.index("void EnsurePadThread")
    keys = (kernel / "keyboard_keys.cpp").read_text(encoding="utf-8-sig")
    with tempfile.TemporaryDirectory(prefix="ttt2-input-") as temp:
        folder = Path(temp)
        source = CHECKER.replace("@KEYS@", keys).replace("@PRODUCTION@", text[begin:end])
        exe = compile_one(folder, "input", source, [kernel])
        env = dict(os.environ, TTT2_SETTINGS_INI=str(folder / "settings.ini"))
        subprocess.run([str(exe)], check=True, timeout=10, env=env)


if __name__ == "__main__":
    main()
