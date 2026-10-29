"""Test the production shared-menu merge and exact frame allowlist."""
from pathlib import Path
import tempfile,subprocess
from test_vcmpgtuh import ROOT,compile_one
kernel=ROOT/'port/src/runtime/kernel'
source=(kernel/'kernel_xam.cpp').read_text(encoding='utf-8-sig')
a=source.index('    if (user == 0 && keyboard_keys::SeparatePlayer() && g_sharedMenuInput.load())')
b=source.index('    if (SysUiOwnsInput())',a)
merge=source[a:b]
menu=(kernel/'menu_ext.cpp').read_text(encoding='utf-8-sig')
a=menu.index('HostSetSharedMenuInput(inputFrame');b=menu.index(';',a)
expression=menu[a:b].removeprefix('HostSetSharedMenuInput(')[:-1]
code=r'''
#include <windows.h>
#include <algorithm>
#include <Xinput.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdlib>
#include <cstdio>
namespace cheat_overlay{bool ConsumeChord(){return false;}}
namespace keyboard_keys{bool SeparatePlayer(){return true;}}
struct Pad{std::mutex m;std::atomic<bool> connected{false};XINPUT_STATE state{};}g_pads[4];
std::atomic<bool> g_sharedMenuInput{false};
XINPUT_STATE Read(unsigned user){XINPUT_STATE st=g_pads[user].state;
@MERGE@
return st;}
bool Shared(const std::string& inputFrame){return @EXPR@;}
#define CHECK(x) do{if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}++checks;}while(0)
int main(){unsigned checks=0;
g_pads[0].connected=true;g_pads[1].connected=true;
g_pads[0].state.Gamepad.wButtons=0x1000;g_pads[0].state.dwPacketNumber=7;
g_pads[1].state.Gamepad.bLeftTrigger=99;g_pads[1].state.Gamepad.wButtons=2;g_pads[1].state.Gamepad.sThumbLX=-32768;g_pads[1].state.dwPacketNumber=9;
CHECK(Read(0).Gamepad.wButtons==0x1000);
g_sharedMenuInput=true;CHECK(Read(0).Gamepad.wButtons==0x1002);
CHECK(Read(0).Gamepad.sThumbLX==-32768);CHECK(Read(0).Gamepad.bLeftTrigger==99);CHECK(Read(0).dwPacketNumber==16);
CHECK(Read(1).Gamepad.wButtons==2);CHECK(g_pads[0].state.Gamepad.wButtons==0x1000);
g_pads[1].connected=false;CHECK(Read(0).Gamepad.wButtons==0x1000);
for(const char* f:{"FrameMainMenu","FrameMode007-01","FrameMode007-04-01","FrameOnlineMenu","FrameOnlineServerList"})CHECK(Shared(f));
for(const char* f:{"","FrameControlerSetup","FrameCharacterSelect","FrameBattle","Unknown"})CHECK(!Shared(f));
printf("PASS %u shared-menu checks: P2 navigation, isolation, packet updates, safe frame scope\n",checks);
}
'''.replace('@MERGE@',merge).replace('@EXPR@',expression)
with tempfile.TemporaryDirectory(prefix='ttt2-shared-menu-') as temp:
 exe=compile_one(Path(temp),'menu',code,[]);subprocess.run([str(exe)],check=True)
