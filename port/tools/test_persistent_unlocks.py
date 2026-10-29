"""Mapped preview grants: production guards/backup plus real TU3 bulk/serialization.

Guest functions are extracted locally, never embedded in the repository. The I/O
scheduler is mocked: this does not prove live-game save/restart acceptance.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
from test_vcmpgtuh import ROOT, RECOMP, compile_one

CODE = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "kernel_shared.h"
#include "sys_ui.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdio>
namespace fs=std::filesystem;
bool online=false,uiBusy=false,loaded=true,saveBusy=false,badGuest=false;
unsigned grants=0,savesSubmitted=0;SysUiMessageBox resultBox;fs::path root;
namespace offline_cheats {bool Allowed(){return !online;}}
bool SysUiBoxBusy(){return uiBusy;}
bool SysUiShowMessageBox(SysUiMessageBox box){resultBox=std::move(box);return true;}
PPC_FUNC(__savegprlr_24){} PPC_FUNC(__restgprlr_24){}
PPC_FUNC(__savegprlr_25){} PPC_FUNC(__restgprlr_25){}
PPC_FUNC(sub_82BD3DF0){std::memcpy(base+ctx.r3.u32,base+ctx.r4.u32,ctx.r5.u32);}
PPC_FUNC(sub_82BD4320){std::memset(base+ctx.r3.u32,ctx.r4.u8,ctx.r5.u32);}
PPC_FUNC(sub_8225D460){badGuest=true;}
#define sub_82732088 OriginalFill
@ORIGINAL@
#undef sub_82732088
bool CompleteBackup(){
 if(!fs::exists(root/L"backups"))return false;
 for(const auto& entry:fs::directory_iterator(root/L"backups"))
  if(fs::is_regular_file(entry.path()/L"BACKUP-COMPLETE.txt") &&
     fs::is_regular_file(entry.path()/L"saves/profile/opaque.bin"))return true;
 return false;
}
extern "C" PPC_FUNC(__imp__sub_822547D0){ctx.r3.u64=0x100000;}
extern "C" PPC_FUNC(__imp__sub_82588238){ctx.r3.u64=saveBusy;}
extern "C" PPC_FUNC(__imp__sub_82718818){ctx.r3.u64=loaded;}
extern "C" PPC_FUNC(__imp__sub_82718A40){ctx.r3.u64=0x110000;}
extern "C" PPC_FUNC(__imp__sub_82276D38){ctx.r3.u64=0x120000;}
extern "C" PPC_FUNC(__imp__sub_8272EEB0){if(!CompleteBackup())badGuest=true;++grants;OriginalItems(ctx,base);}
extern "C" PPC_FUNC(__imp__sub_8272F260){if(!CompleteBackup())badGuest=true;++grants;OriginalPanels(ctx,base);}
extern "C" PPC_FUNC(__imp__sub_8272F738){if(!CompleteBackup())badGuest=true;++grants;OriginalGroup(ctx,base);}
extern "C" PPC_FUNC(__imp__sub_82411F08){
 ++savesSubmitted;if(ctx.r3.u32!=0x120000)badGuest=true;
 const auto original=ctx;
 ctx.r3.u64=0x110000;ctx.r4.u64=0x130000;OriginalSerialize(ctx,base);
 ctx.r3.u64=0x110000;ctx.r4.u64=0x140000;OriginalDelta(ctx,base);
 ctx=original;
}
#include "persistent_unlocks.cpp"
#define CHECK(x) do{++checks;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
void Init(uint8_t* base,uint32_t object){
 std::memset(base+object,0,6844);
 const unsigned headers[][4]={{8,625,32,2500},{2524,625,32,2500},{5040,4,4,16},
  {5072,4,4,16},{5104,32,16,128},{5248,32,16,128}};
 for(const auto& h:headers){GWrite32(base,object+h[0],h[1]);GWrite32(base,object+h[0]+4,h[2]);GWrite32(base,object+h[0]+12,h[3]);}
 GWrite32(base,object+6680,6);GWrite32(base,object+6696,6);
}
int main(int argc,char** argv){unsigned checks=0;
 if(argc!=2)return 2;root=fs::path(argv[1]);
 auto* base=(uint8_t*)VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE);
 if(!base||!VirtualAlloc(base,0x400000,MEM_COMMIT,PAGE_READWRITE)||
    !VirtualAlloc(base+0x82E60000,0x10000,MEM_COMMIT,PAGE_READWRITE))return 2;
 GWrite32(base,0x82E6B6FC,0x100000);Init(base,0x110000);
 PPCContext ctx{};ctx.r1.u64=0x300000;ctx.r3.u64=0xDEADBEEF;ctx.r31.u64=0xAABBCCDD;ctx.lr=0x12345678;
 auto unchanged=[&]{return ctx.r1.u32==0x300000&&ctx.r3.u32==0xDEADBEEF&&ctx.r31.u32==0xAABBCCDD&&ctx.lr==0x12345678;};
 CHECK(persistent_unlocks::QueuePermanentUnlocks());CHECK(!persistent_unlocks::QueuePermanentUnlocks());
 persistent_unlocks::ProcessPending(ctx,base,false);CHECK(grants==0 && unchanged());
 online=true;persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);
 CHECK(grants==0 && unchanged());online=false;
 // The action fails closed without updater isolation/marker and before backup.
 SetEnvironmentVariableW(L"TTT2_SAVE_ROOT",nullptr);
 persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);CHECK(grants==0);
 const auto saveRoot=root/L"saves";fs::create_directories(saveRoot/L"profile");
 std::ofstream(saveRoot/L"profile/opaque.bin",std::ios::binary)<<"original opaque preview bytes";
 std::ofstream(root/L"settings.ini")<<"[Keyboard]\nStart=78\n";
 SetEnvironmentVariableW(L"TTT2_SAVE_ROOT",saveRoot.c_str());
 SetEnvironmentVariableW(L"TTT2_SETTINGS_INI",(root/L"settings.ini").c_str());
 persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);CHECK(grants==0);
 std::ofstream(root/L".ttt2-community-preview-v1")<<"TTT2 community offline preview v1\n";
 loaded=false;persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);
 CHECK(grants==0 && !fs::exists(root/L"backups") && unchanged());loaded=true;
 GWrite32(base,0x110000+6680,99);persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);
 CHECK(grants==0 && unchanged());GWrite32(base,0x110000+6680,6);
 GWrite32(base,0x110000+8,999);persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);
 CHECK(grants==0 && unchanged());GWrite32(base,0x110000+8,625);
 saveBusy=true;persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);
 CHECK(grants==0 && !fs::exists(root/L"backups") && unchanged());saveBusy=false;
 GWrite32(base,0x110000-4,0xDEADC0DE);GWrite32(base,0x110000+6844,0xCAFEBABE);
 uiBusy=true;persistent_unlocks::QueuePermanentUnlocks();persistent_unlocks::ProcessPending(ctx,base,true);CHECK(grants==0);
 uiBusy=false;persistent_unlocks::ProcessPending(ctx,base,true);
 CHECK(grants==3 && savesSubmitted==1 && CompleteBackup() && unchanged() && !badGuest);
 CHECK(resultBox.text.find("Save requested")!=std::string::npos);
 CHECK(GRead32(base,0x110000-4)==0xDEADC0DE && GRead32(base,0x110000+6844)==0xCAFEBABE);
 CHECK(GRead32(base,0x110000+8)==625 && GRead32(base,0x110000+2524)==625);
 // Real TU3 query functions, with no temporary override, see grants across
 // boundaries and retain their out-of-range rejection.
 for(unsigned id:{0u,31u,32u,19999u,20000u}) {
  ctx.r3.u64=0x110000;ctx.r4.u64=id;OriginalItemQuery(ctx,base);CHECK(ctx.r3.u32==(id<20000));
 }
 for(unsigned id:{0u,31u,32u,99u,100u}) {
  ctx.r3.u64=0x110000;ctx.r4.u64=id;OriginalPanelQuery(ctx,base);CHECK(ctx.r3.u32==(id<100));
 }
 // Original primary serializer/loader round-trip, independent of the mocked
 // save scheduler. Deltas are separately checked against its real output.
 Init(base,0x150000);ctx.r3.u64=0x150000;ctx.r4.u64=0x130000;OriginalLoad(ctx,base);
 for(unsigned id:{0u,31u,32u,19999u}) {
  ctx.r3.u64=0x150000;ctx.r4.u64=id;OriginalItemQuery(ctx,base);CHECK(ctx.r3.u32==1);
 }
 CHECK(GRead32(base,0x140000)==6);
 for(unsigned word=0;word<625;++word)CHECK(GRead32(base,0x140000+24+4*word)==0xFFFFFFFF);
 CHECK(GRead32(base,0x140000+2536)==15); // 24-byte header + 2500 item bytes + three panel words
 CHECK(!badGuest);
 VirtualFree(base,0,MEM_RELEASE);
 printf("PASS %u permanent-grant checks: preview/menu/session/schema/busy guards, pre-write backup, guest context, real bulk/query/serialization; live scheduler mocked\n",checks);
}
'''

FUNCTIONS = {
    "82732088": "OriginalFill", "8272EEB0": "OriginalItems",
    "8272F260": "OriginalPanels", "8272F738": "OriginalGroup",
    "82730EF8": "OriginalSerialize", "82731330": "OriginalDelta",
    "82731090": "OriginalLoad", "8272F190": "OriginalItemQuery",
    "8272F538": "OriginalPanelQuery",
}


def main():
    source = (ROOT / "re/ppc/ppc_recomp.153.cpp").read_text()
    extracted = []
    for address, name in FUNCTIONS.items():
        match = re.search(r"PPC_FUNC_IMPL\(__imp__sub_" + address + r"\) \{.*?\n\}", source, re.S)
        if not match:
            raise RuntimeError("Missing locally generated guest routine " + address)
        extracted.append(match.group().replace("__imp__sub_" + address, name, 1))
    with tempfile.TemporaryDirectory(prefix="ttt2-persistent-grants-") as temp:
        folder = Path(temp)
        exe = compile_one(folder, "permanent_grants", CODE.replace("@ORIGINAL@", "\n".join(extracted)), [
            ROOT / "port/src/runtime/kernel", ROOT / "re/ppc", RECOMP / "XenonUtils", RECOMP / "thirdparty/simde"])
        subprocess.run([str(exe), str(folder / "copied-preview")], check=True, timeout=60,
                       env=dict(os.environ))


if __name__ == "__main__":
    main()
