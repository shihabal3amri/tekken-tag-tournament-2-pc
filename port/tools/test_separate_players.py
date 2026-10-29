"""Exercise separate keyboard/controller slots using the production poller and INI setting."""
import os
import subprocess
import tempfile
from pathlib import Path
from test_polling_harness import CHECKER, ROOT
from test_vcmpgtuh import compile_one

CASES = r'''
int main(int argc, char** argv) {
    const bool dynamic=argc>1;
    CHECK(keyboard_keys::SeparatePlayer()==dynamic);
    if(argc>2) {
        const int preference=std::atoi(argv[2]);CHECK(keyboard_keys::PlayerOnePreference()==preference);
        Reset();g_usedUsers=0;PadPollState p;physical[0]=true;
        if(preference==1) host[0].Gamepad.wButtons=0x10; else keys[VK_RETURN]=true;
        PollPadsOnce(p,false,0);CHECK(!g_pads[0].connected);
        keys.fill(false);host[0].Gamepad.wButtons=0;PollPadsOnce(p,false,1);
        if(preference==1) keys[VK_RETURN]=true; else host[0].Gamepad.wButtons=0x10;
        PollPadsOnce(p,false,2);CHECK(g_pads[0].connected);
        CHECK(g_pads[0].physical==(preference==2));CHECK((g_usedUsers.load()&2)==0);
        keys.fill(false);host[0].Gamepad.wButtons=0;PollPadsOnce(p,false,3);
        if(preference==1) host[0].Gamepad.wButtons=0x10; else keys[VK_RETURN]=true;
        PollPadsOnce(p,false,4);CHECK((g_usedUsers.load()&2)!=0);
        CHECK(g_pads[1].physical==(preference==1));
        std::printf("PASS %u preference %d ownership checks\n",checks,preference);return 0;
    }
    if(!dynamic) {
        Reset();PadPollState p;physical[0]=true;host[0].Gamepad.wButtons=0x1000;keys['Z']=true;
        PollPadsOnce(p,false,0);CHECK(g_pads[0].state.Gamepad.wButtons==0x5000);
        std::puts("PASS legacy shared input");return 0;
    }
    for(int slot=0;slot<4;++slot) for(int keyboardFirst=0;keyboardFirst<2;++keyboardFirst) {
        Reset();g_usedUsers=0;PadPollState p;
        physical[slot]=true;PollPadsOnce(p,false,0);
        CHECK(!g_pads[0].connected && !g_pads[1].connected);
        keys['A']=true;host[slot].Gamepad.wButtons=0x1000;
        PollPadsOnce(p,false,1);CHECK(!g_pads[0].connected);
        keys.fill(false);host[slot].Gamepad.wButtons=0;PollPadsOnce(p,false,2);
        if(keyboardFirst) keys[VK_RETURN]=true;else host[slot].Gamepad.wButtons=0x10;
        PollPadsOnce(p,false,1);
        CHECK(g_pads[0].connected && g_pads[1].connected);
        CHECK((g_usedUsers.load()&2)==0);CHECK(g_pads[1].state.Gamepad.wButtons==0);
        CHECK(g_pads[0].physical==!keyboardFirst);
        keys.fill(false);host[slot].Gamepad.wButtons=0;PollPadsOnce(p,false,2);
        // A combat press by the other device must not join or affect P1.
        if(keyboardFirst)host[slot].Gamepad.wButtons=0x4000;else keys['Z']=true;
        PollPadsOnce(p,false,3);CHECK((g_usedUsers.load()&2)==0);
        CHECK(g_pads[1].state.Gamepad.wButtons==0);
        CHECK(g_pads[0].state.Gamepad.wButtons==0);
        keys.fill(false);host[slot].Gamepad.wButtons=0;PollPadsOnce(p,false,4);
        if(keyboardFirst)host[slot].Gamepad.wButtons=0x10;else keys[VK_RETURN]=true;
        PollPadsOnce(p,false,5);CHECK(g_pads[1].connected);
        CHECK(g_pads[1].physical==bool(keyboardFirst));CHECK((g_usedUsers.load()&2)!=0);
        keys.fill(false);keys['Z']=true;host[slot].Gamepad.wButtons=0x2000;
        PollPadsOnce(p,false,6);
        CHECK(g_pads[keyboardFirst?0:1].state.Gamepad.wButtons==0x4000);
        CHECK(g_pads[keyboardFirst?1:0].state.Gamepad.wButtons==0x2000);
        physical[slot]=false;PollPadsOnce(p,false,7);
        physical[slot]=true;PollPadsOnce(p,false,2007);
        CHECK(g_pads[keyboardFirst?1:0].physical);
        focused=false;PollPadsOnce(p,false,2008);
        CHECK(g_pads[keyboardFirst?0:1].state.Gamepad.wButtons==0);
    }
    Reset();g_usedUsers=0;PadPollState stick;
    physical[0]=true;host[0].Gamepad.wButtons=0x10;
    host[0].Gamepad.sThumbLX=-32768;host[0].Gamepad.sThumbLY=1000;
    PollPadsOnce(stick,false,0);
    CHECK(g_pads[0].state.Gamepad.sThumbLX==(keyboard_keys::DPadOnly()?0:-32768));
    CHECK(g_pads[0].state.Gamepad.sThumbLY==((keyboard_keys::DPadOnly()||keyboard_keys::StickDeadzone()>=4||keyboard_keys::StickDiagonal()>=4)?0:1000));
    CHECK(g_pads[0].state.Gamepad.wButtons==0x10);
    Reset();g_usedUsers=0;PadPollState p;physical[0]=physical[1]=true;
    host[1].Gamepad.wButtons=0x10;PollPadsOnce(p,false,0);
    CHECK(p.guestOf[1]==0 && p.guestOf[0]==-1);
    host[0].Gamepad.wButtons=0x10;PollPadsOnce(p,false,1);
    CHECK(p.guestOf[0]==1);CHECK(g_pads[0].physical && g_pads[1].physical);
    std::printf("PASS %u dynamic ownership checks: both start orders, solo, explicit join, isolation, reconnect, focus, two pads\n",checks);
}
'''

def main():
    kernel=ROOT/'port/src/runtime/kernel'
    production=(kernel/'kernel_xam.cpp').read_text(encoding='utf-8-sig')
    production=production[production.index('struct PadSnapshot'):production.index('void EnsurePadThread')]
    prefix='#include "direction_filter.h"\n'+CHECKER[:CHECKER.index('int main()')]
    prefix=prefix.replace('@KEYS@', 'std::atomic<uint32_t> g_usedUsers{0};\nbool User0SignedIn(){return true;}\nstd::string UserName(unsigned){return "test";}\nvoid XamNotifyEnqueueEvent(unsigned,unsigned){}\n'+'#include "host_settings.cpp"\n#include "host_settings_windows.cpp"\n'+(kernel/'keyboard_keys.cpp').read_text(encoding='utf-8-sig'))
    source=prefix.replace('@PRODUCTION@',production)+CASES
    with tempfile.TemporaryDirectory(prefix='ttt2-separate-input-') as temp:
        folder=Path(temp);exe=compile_one(folder,'separate_input',source,[kernel])
        for value,dpad,deadzone,diagonal in ((0,0,0,0),(1,0,0,0),(1,1,0,0),(1,0,20,0),(1,0,0,50),(1,0,20,50),(2,0,0,0)):
            ini=folder/'settings.ini';ini.write_text(f'[Keyboard]\nSeparatePlayer={value}\n[Input]\nDPadOnly={dpad}\nStickDeadzone={deadzone}\nStickDiagonal={diagonal}\n')
            env=dict(os.environ,TTT2_SETTINGS_INI=str(ini))
            subprocess.run([str(exe)]+(['separate'] if value==1 else []),check=True,env=env,timeout=15)
        for preference in (1,2):
            ini.write_text(f'[Keyboard]\nSeparatePlayer=1\n[Input]\nPlayerOnePreference={preference}\n')
            subprocess.run([str(exe),'separate',str(preference)],check=True,env=env,timeout=15)
if __name__=='__main__':main()
