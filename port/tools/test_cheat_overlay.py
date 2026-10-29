"""Production overlay state transitions with mocked UI/session, isolated INI."""
from pathlib import Path
import tempfile,os,subprocess
from test_vcmpgtuh import ROOT,compile_one
CODE=r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
ULONGLONG testClock=0;
ULONGLONG TestClock(){return testClock;}
#define GetTickCount64 TestClock
#include "host_settings.cpp"
#include "host_settings_windows.cpp"
@PRODUCTION@
#include <cstdio>
namespace offline_cheats { int health=0;bool money=false,gate=false,unlock=false; int HealthSides(){return health;}void SetHealthSides(int v){health=v;}bool InfiniteMoney(){return money;}void SetInfiniteMoney(bool v){money=v;}void SetOfflineMenu(bool v){gate=v;}bool UnlockContent(){return unlock;}void SetUnlockContent(bool v){unlock=v;} }
namespace persistent_unlocks{unsigned queued=0;bool QueuePermanentUnlocks(){++queued;return true;}}
bool online=false,down=false,busy=false,delay=false,shadows=true;
unsigned opened=0,dismissed=0;SysUiMessageBox current;
bool XSessionAnyActive(){return online;}
bool HostCheatOverlayChord(){return down;}
bool SysUiBoxBusy(){return busy;}
bool SysUiShowMessageBox(SysUiMessageBox box){if(busy)return false;current=std::move(box);busy=true;++opened;return true;}
void SysUiDismissBox(){busy=false;++dismissed;}
namespace display_settings {
int brightness=100,gamma=100,contrast=100;bool pattern=false;
int GetBrightness(){return brightness;}void SetBrightness(int v){brightness=v;}
int GetGamma(){return gamma;}void SetGamma(int v){gamma=v;}
int GetContrast(){return contrast;}void SetContrast(int v){contrast=v;}
bool GetCalibrationPattern(){return pattern;}void SetCalibrationPattern(bool v){pattern=v;}
bool GetInputDelayOn(){return delay;}void SetInputDelayOn(bool v){delay=v;}
bool GetShadows(){return shadows;}void SetShadows(bool v){shadows=v;}
}
#define CHECK(x) do{++checks;if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}}while(0)
int main(){unsigned checks=0;
CheatOverlayChord c;CHECK(!c.Update(false));CHECK(c.Update(true));CHECK(!c.Update(true));CHECK(!c.Update(false));CHECK(c.Update(true));
CHECK(CheatOverlayAllowed(false,false));CHECK(!CheatOverlayAllowed(true,false));CHECK(!CheatOverlayAllowed(false,true));
cheat_overlay::Frame("FrameMainMenu");CHECK(opened==0);
down=true;cheat_overlay::Frame("FrameMainMenu");CHECK(opened==1&&busy);CHECK(cheat_overlay::ConsumeChord());
cheat_overlay::Frame("FrameMainMenu");CHECK(opened==1);
online=true;cheat_overlay::Frame("FrameMainMenu");CHECK(!busy&&dismissed==1);CHECK(!cheat_overlay::ConsumeChord());
current.done(0);CHECK(offline_cheats::health==0);
online=false;down=false;cheat_overlay::Frame("FrameMainMenu");down=true;cheat_overlay::Frame("FrameOnlineMenu");CHECK(opened==1);
down=false;cheat_overlay::Frame("FrameMainMenu");down=true;cheat_overlay::Frame("FrameMainMenu");CHECK(opened==2);
busy=false;current.done(0);CHECK(offline_cheats::health==1);down=false;cheat_overlay::Frame("FrameMainMenu");CHECK(opened==3);
busy=false;current.done(5);cheat_overlay::Frame("FrameMainMenu");CHECK(!busy);
// A calibration callback opens a real owned dialog and activates the reference
// only in front-end menus. Adjustments reopen the same selection.
down=true;cheat_overlay::Frame("FrameMainMenu");CHECK(busy);
busy=false;current.done(3);down=false;cheat_overlay::Frame("FrameMainMenu");
CHECK(busy && display_settings::pattern && current.buttons.size()==8);
auto staleCalibration=current.done;
busy=false;current.done(1);cheat_overlay::Frame("FrameMainMenu");
CHECK(display_settings::brightness==105 && current.active==1 && display_settings::pattern);
staleCalibration(1);CHECK(display_settings::brightness==105 && busy);
cheat_overlay::Frame("FrameMainMenu");CHECK(current.active==1 && display_settings::pattern);
busy=false;current.done(7);cheat_overlay::Frame("FrameMainMenu");
CHECK(!display_settings::pattern && current.buttons.size()==6);
busy=false;current.done(3);cheat_overlay::Frame("FrameMainMenu");CHECK(display_settings::pattern);
testClock=60000;cheat_overlay::Frame("FrameMainMenu");CHECK(!display_settings::pattern && !busy);
current.done(1);CHECK(display_settings::brightness==105); // expired callback cannot write
down=false;cheat_overlay::Frame("FrameMainMenu");down=true;cheat_overlay::Frame("FrameMainMenu");
busy=false;current.done(3);down=false;cheat_overlay::Frame("FrameMainMenu");CHECK(display_settings::pattern);
cheat_overlay::Frame("FrameOnlineMenu");CHECK(!display_settings::pattern && !busy);
current.done(1);CHECK(display_settings::brightness==105);
cheat_overlay::Frame("FrameMainMenu");down=true;cheat_overlay::Frame("FrameMainMenu");
busy=false;current.done(3);down=false;cheat_overlay::Frame("FrameMainMenu");CHECK(display_settings::pattern);
cheat_overlay::Frame("FrameCharacterSelect");CHECK(!display_settings::pattern && !busy);
// Another system component can dismiss an owned dialog without answering it.
// Once the busy/release phase ends, reference panels must disappear too.
cheat_overlay::Frame("FrameMainMenu");down=true;cheat_overlay::Frame("FrameMainMenu");
busy=false;current.done(3);down=false;cheat_overlay::Frame("FrameMainMenu");CHECK(display_settings::pattern);
busy=false;cheat_overlay::Frame("FrameMainMenu");CHECK(!display_settings::pattern);
// Permanent grants require their own confirmation, not the temporary toggle.
down=false;cheat_overlay::Frame("FrameMainMenu");down=true;cheat_overlay::Frame("FrameMainMenu");
busy=false;current.done(4);down=false;cheat_overlay::Frame("FrameMainMenu");
CHECK(current.buttons.size()==2 && persistent_unlocks::queued==0);
busy=false;current.done(0);CHECK(persistent_unlocks::queued==1);
current.done(0);CHECK(persistent_unlocks::queued==1); // no duplicate request
down=true;cheat_overlay::Frame("FrameMainMenu");busy=false;current.done(4);down=false;cheat_overlay::Frame("FrameMainMenu");
cheat_overlay::Frame("FrameOnlineMenu");current.done(0);CHECK(persistent_unlocks::queued==1);
printf("PASS %u overlay checks: chord edges, busy UI, online closure, stale callback guard, offline action\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-overlay-') as temp:
 folder=Path(temp)
 source=(ROOT/'port/src/runtime/kernel/cheat_overlay.cpp').read_text()
 source=source.replace('uint64_t OverlayNowMs(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}','uint64_t OverlayNowMs(){return testClock;}')
 exe=compile_one(folder,'overlay',CODE.replace('@PRODUCTION@',source),[ROOT/'port/src/runtime/kernel'])
 ini=folder/'settings.ini';ini.write_text('[Mods]\nOverlay=1\n')
 subprocess.run([str(exe)],env=dict(os.environ,TTT2_SETTINGS_INI=str(ini)),check=True)
