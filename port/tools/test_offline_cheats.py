"""Exercise the real TU3 damage function through the production offline hook.

Requires locally generated PPC; no copyrighted code is embedded in this test.
"""
from pathlib import Path
import argparse, os, re, subprocess, tempfile
from test_vcmpgtuh import ROOT, RECOMP, compile_one

CODE = r'''
#include "kernel_shared.h"
#include <windows.h>
#include <cstdio>
bool online=false;
bool XSessionAnyActive(){return online;}
extern "C" PPC_FUNC(__imp__sub_8265F5D8){ctx.r3.u32=0x110000;}
PPC_FUNC(__savegprlr_25){}
PPC_FUNC(__restgprlr_25){}
PPC_FUNC(sub_824CBF98){ctx.r3.u64=0;}
extern "C" PPC_FUNC(__imp__sub_822ADCB0){ctx.r3.u64=9;}
extern "C" PPC_FUNC(__imp__sub_827163B8){ctx.r3.u64=0;}
extern "C" PPC_FUNC(__imp__sub_8272F190){ctx.r3.u64=0;}
extern "C" PPC_FUNC(__imp__sub_827308C8){ctx.r3.u64=0;}
extern "C" PPC_FUNC(__imp__sub_8272F538){ctx.r3.u64=0;}
@ORIGINAL@
#include "host_settings.cpp"
#include "host_settings_windows.cpp"
#include "offline_cheats.cpp"
#define CHECK(x) do{++checks;if(!(x)){printf("FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(){unsigned checks=0;
auto* base=(uint8_t*)VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE);
if(!base||!VirtualAlloc(base,0x400000,MEM_COMMIT,PAGE_READWRITE)||!VirtualAlloc(base+0x82E00000,0x10000,MEM_COMMIT,PAGE_READWRITE))return 2;
const uint32_t obj=0x100000,max=180u<<16,damage=40u<<16;
for(int mode=0;mode<4;++mode)for(unsigned fighter=0;fighter<4;++fighter)
for(int gate=0;gate<3;++gate){
 offline_cheats::SetHealthSides(mode);offline_cheats::SetOfflineMenu(gate!=0);online=gate==2;
 GWrite32(base,obj,fighter);GWrite32(base,obj+4,max);GWrite32(base,obj+8,max);GWrite32(base,obj+16,max);
 PPCContext ctx{};ctx.r1.u32=0x300000;ctx.r3.u32=obj;ctx.r4.u32=damage;ctx.r8.u32=1;
 sub_824D09B0(ctx,base);
 const bool protect=gate==1 && (mode&(1u<<(fighter&1)));
 CHECK(GRead32(base,obj+4)==(protect?max:max-damage));
 CHECK(GRead32(base,obj+8)==(protect?max:max-damage));
}
// Unknown fighter IDs never receive a cheat. Turning off restores normal damage.
online=false;offline_cheats::SetOfflineMenu(true);offline_cheats::SetHealthSides(3);
GWrite32(base,obj,10);GWrite32(base,obj+4,max);GWrite32(base,obj+8,max);GWrite32(base,obj+16,max);
PPCContext ctx{};ctx.r1.u32=0x300000;ctx.r3.u32=obj;ctx.r4.u32=damage;ctx.r8.u32=1;
sub_824D09B0(ctx,base);CHECK(GRead32(base,obj+4)==max-damage);
offline_cheats::SetHealthSides(0);CHECK(!offline_cheats::HealthSides());
for(int gate=0;gate<3;++gate)for(int enabled=0;enabled<2;++enabled){
 offline_cheats::SetOfflineMenu(gate!=0);online=gate==2;offline_cheats::SetInfiniteMoney(enabled);
 GWrite32(base,0x110004,123);GWrite32(base,0x110008,456);ctx.r3.u32=0;
 sub_8265F5D8(ctx,base);CHECK(ctx.r3.u32==0x110000);
 CHECK(GRead32(base,0x110004)==(gate==1&&enabled?999999999u:123u));
 CHECK(GRead32(base,0x110008)==456);
}
for(int gate=0;gate<3;++gate)for(int enabled=0;enabled<2;++enabled){
 offline_cheats::SetOfflineMenu(gate!=0);online=gate==2;offline_cheats::SetUnlockContent(enabled);
 const bool active=gate==1&&enabled;
 PPCFunc* queries[]={sub_827163B8,sub_8272F190,sub_827308C8,sub_8272F538};
 unsigned limits[]={20000,20000,163,100};
 for(int i=0;i<4;++i)for(unsigned id:{0u,limits[i]-1,limits[i],0xffffffffu}){
   ctx.r4.u32=id;queries[i](ctx,base);CHECK(ctx.r3.u32==(active&&id<limits[i]?1:0));
 }
 for(unsigned id:{0u,772u,773u,0xffffffffu}){ctx.r4.u32=id;sub_822ADCB0(ctx,base);CHECK(ctx.r3.u32==(active&&id<773?10:9));}
}
online=false;offline_cheats::SetOfflineMenu(true);offline_cheats::SetHealthSides(3);
GWrite32(base,0x82E00000,0);GWrite32(base,0x82E00004,max);GWrite32(base,0x82E00008,max);GWrite32(base,0x82E00010,max);
ctx.r1.u32=0x300000;ctx.r3.u32=0x82E00000;ctx.r4.u32=damage;ctx.r8.u32=1;sub_824D09B0(ctx,base);
CHECK(GRead32(base,0x82E00004)==max);CHECK(GRead32(base,0x82E00008)==max);
VirtualFree(base,0,MEM_RELEASE);printf("PASS %u real-damage checks: sides, tag partners, offline/menu/session gates, disabled and invalid fighter\n",checks);
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-control',action='store_true',help='Restore the old heap-only check and require the static-arena regression to fail')
    args=parser.parse_args()
    original = None
    for path in (ROOT/'re/ppc').glob('ppc_recomp.*.cpp'):
        match = re.search(r'PPC_FUNC_IMPL\(__imp__sub_824D09B0\) \{.*?\n\}',path.read_text(),re.S)
        if match:
            original=match.group();break
    if not original:raise SystemExit('Generate the TU3 PPC source first')
    with tempfile.TemporaryDirectory(prefix='ttt2-health-') as temp:
        folder=Path(temp)
        code=CODE.replace('@ORIGINAL@',original)
        if args.negative_control:
            production=(ROOT/'port/src/runtime/kernel/offline_cheats.cpp').read_text()
            old='const bool valid=object>=0x10000u && object<=0xFFFFFFD7u && (object&3u)==0;'
            assert old in production
            production=production.replace(old,'const bool valid=(object>=VHEAP_BEGIN&&object<VHEAP_END-40)||(object>=PHEAP_BEGIN&&object<PHEAP_END-40);')
            (folder/'old_health.cpp').write_text(production)
            code=code.replace('#include "offline_cheats.cpp"','#include "old_health.cpp"')
        exe=compile_one(folder,'health',code,[
            ROOT/'port/src/runtime/kernel',ROOT/'re/ppc',RECOMP/'XenonUtils',RECOMP/'thirdparty/simde'])
        result=subprocess.run([str(exe)],env=dict(os.environ,TTT2_SETTINGS_INI=str(folder/'settings.ini')),capture_output=True,text=True)
        if args.negative_control:
            if result.returncode!=1 or '0x82E00004' not in result.stdout:
                raise AssertionError('Old heap-only health check was not caught: '+result.stdout+result.stderr)
            print('PASS negative control: original heap-only check fails the static-arena health regression')
        else:
            print(result.stdout,end='')
            if result.returncode:raise RuntimeError(result.stderr)

if __name__=='__main__':main()
