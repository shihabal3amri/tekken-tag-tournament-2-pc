"""Production SDL snapshot mock used only by device ownership fixtures."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'port/src/runtime/kernel/kernel_xam.cpp').read_text()
wire=source[source.index('struct XboxPadData'):source.index('// --- ExGetXConfigSetting')]
snapshot=(ROOT/'port/src/runtime/host_input_sdl.h').read_text()
snapshot=snapshot[snapshot.index('struct Pad {'):snapshot.index('// Call from the SDL event thread')]
CHECKER=r'''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
'''+wire+r'''
namespace host_input {'''+snapshot+r'''}
std::array<bool,256> keys{};
bool physical[4]{};
XboxPadState host[4]{};
bool focused=true,windowExists=true,textEntry=false,captureActive=false;
bool SysUiTextEntryActive(){return textEntry;}
bool SysUiKeyCaptureActive(){return captureActive;}
namespace host_input {
Snapshot Read(){Snapshot result;result.focused=focused&&windowExists;result.keys=keys;
 for(unsigned i=0;i<4;++i){auto& p=result.pads[i];const auto& g=host[i].Gamepad;
 p.connected=physical[i];p.buttons=g.wButtons;p.leftTrigger=g.bLeftTrigger;p.rightTrigger=g.bRightTrigger;
 p.leftX=g.sThumbLX;p.leftY=g.sThumbLY;p.rightX=g.sThumbRX;p.rightY=g.sThumbRY;}return result;}
}
@KEYS@
@PRODUCTION@
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
void Reset(){keys.fill(false);focused=windowExists=true;
 for(unsigned i=0;i<4;++i){physical[i]=false;host[i]={};g_pads[i].connected=false;g_pads[i].physical=false;g_pads[i].state={};g_pads[i].caps={};}}
int main()
'''
