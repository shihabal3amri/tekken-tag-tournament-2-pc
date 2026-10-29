"""Production save mount/content/file roundtrips using temporary Windows folders.

Runs in two processes to prove discovery and reopening do not depend on a host
registry surviving. Includes real directory records, event completion, permission
and path rejection, and a native NtWriteFile oracle. No game files are used.
"""
from pathlib import Path
import os
import subprocess
import tempfile

from test_kernel_waits import ROOT, RECOMP, SHARED
from test_vcmpgtuh import compile_one

CHECKER = r'''
#define TTT2_LOCAL_SAVES 1
#include "@OBJECTS@"
#include "@CONTENT@"
#include "@STORE@"
#include "@IO@"
#include "@HOST_FILE_COMMON@"
#include "@HOST_FILE_WINDOWS@"
#include "@HOST_STORAGE_WINDOWS@"
#include "@HOST_THREAD@"
#pragma comment(lib,"@SDL_LIB@")
#include <fstream>
#include <future>
#undef OUT
bool WTrace(){return false;}
namespace host_vm { bool PrepareHostWrite(void*,size_t){return true;} }
void RollbackNoteIo(bool,const char*,const PPCContext*){}
void RollbackBeforeHostWrite(uint32_t,uint32_t){}
void RollbackSaveOpen(bool){}
void RollbackImportNote(const char*){}
bool XLiveCloseHandle(uint32_t,uint32_t&){return false;}
bool XLiveEnumerate(uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t&,uint32_t&){return false;}
int32_t RollbackSaveJournalFrame(){return -1;}
namespace profiles { uint64_t ActiveXuid(){return 0xB13EBABEBABEBABEull;} }

void KernelQueueGuestCall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){std::abort();}
static void Reporter(PPCContext&,uint8_t*){std::abort();}
PPCFunc* const g_unmappedReporter=Reporter;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);std::abort();}}while(0)
alignas(16) std::array<uint8_t,0x12000> memory;
uint8_t* base=memory.data();
constexpr uint32_t ROOT_NAME=0x10100,RECORD=0x10200,OUT=0x10400,IOS=0x10420,OV=0x10440;
constexpr uint32_t OBJECT=0x10500,ANSI=0x10520,PATH=0x10600,DATA=0x10900,OFFSET=0x10B00,STACK=0x11800;
void Put64(uint32_t p,uint64_t v){GWrite32(base,p,uint32_t(v>>32));GWrite32(base,p+4,uint32_t(v));}
uint64_t Get64(uint32_t p){return uint64_t(GRead32(base,p))<<32|GRead32(base,p+4);}
void Reset(){memory.fill(0xA5);std::memcpy(base+ROOT_NAME,"tag25save",10);
 std::memset(base+RECORD,0,308);GWrite32(base,RECORD,1);GWrite32(base,RECORD+4,1);
 std::memcpy(base+RECORD+264,"Tag25SaveData",14);GWrite16(base,RECORD+8,'T');
 GWrite32(base,STACK+84,0);}
void Overlap(uint32_t event=0){std::memset(base+OV,0,28);GWrite32(base,OV,997);GWrite32(base,OV+8,0x12345678);GWrite32(base,OV+12,event);}
uint32_t Content(uint32_t mode,uint32_t ov=0){PPCContext c{};c.r1.u32=STACK;c.r4.u32=ROOT_NAME;c.r5.u32=RECORD;
 c.r6.u32=mode|16;c.r7.u32=OUT;c.r8.u32=OUT+8;GWrite32(base,STACK+84,ov);__imp__XamContentCreateEx(c,base);return c.r3.u32;}
uint32_t ContentClose(bool flush=false,uint32_t ov=0){PPCContext c{};c.r3.u32=ROOT_NAME;c.r4.u32=ov;
 if(flush)__imp__XamContentFlush(c,base);else __imp__XamContentClose(c,base);return c.r3.u32;}
uint32_t Delete(){PPCContext c{};c.r4.u32=RECORD;__imp__XamContentDelete(c,base);return c.r3.u32;}
void Path(const std::string& s){GWrite32(base,OBJECT,0xFFFFFFFD);GWrite32(base,OBJECT+4,ANSI);GWrite32(base,OBJECT+8,64);
 GWrite16(base,ANSI,uint16_t(s.size()));GWrite16(base,ANSI+2,uint16_t(s.size()+1));GWrite32(base,ANSI+4,PATH);std::memcpy(base+PATH,s.c_str(),s.size()+1);}
uint32_t Open(const std::string& name,uint32_t access=GENERIC_READ|GENERIC_WRITE,uint32_t disp=1,uint32_t options=0x60,bool create=true){
 Path(name);GWrite32(base,OUT,0xA5A5A5A5);GWrite32(base,STACK+84,options);PPCContext c{};
 c.r1.u32=STACK;c.r3.u32=OUT;c.r4.u32=access;c.r5.u32=OBJECT;c.r6.u32=IOS;
 if(create){c.r8.u32=128;c.r9.u32=3;c.r10.u32=disp;__imp__NtCreateFile(c,base);}
 else{c.r7.u32=3;c.r8.u32=options;__imp__NtOpenFile(c,base);}
 return c.r3.u32;}
uint32_t Write(uint32_t handle,uint32_t len,int64_t offset=-2,bool pointer=false,uint32_t event=0){
 Put64(OFFSET,uint64_t(offset));PPCContext c{};c.r3.u32=handle;c.r4.u32=event;c.r7.u32=IOS;
 c.r8.u32=DATA;c.r9.u32=len;c.r10.u32=pointer?OFFSET:0;__imp__NtWriteFile(c,base);return c.r3.u32;}
uint32_t Read(uint32_t handle,uint32_t len){PPCContext c{};c.r3.u32=handle;c.r7.u32=IOS;c.r8.u32=DATA;c.r9.u32=len;
 __imp__NtReadFile(c,base);return c.r3.u32;}
void Close(uint32_t handle){PPCContext c{};c.r3.u32=handle;__imp__NtClose(c,base);CHECK(c.r3.u32==0);}
uint32_t Enumerate(){PPCContext c{};c.r4.u32=1;c.r5.u32=1;c.r7.u32=1;c.r8.u32=OUT;c.r9.u32=OUT+8;
 __imp__XamContentCreateEnumerator(c,base);CHECK(c.r3.u32==0&&GRead32(base,OUT)==308);return GRead32(base,OUT+8);}
uint32_t Next(uint32_t handle,uint32_t ov=0){PPCContext c{};c.r3.u32=handle;c.r5.u32=DATA;c.r6.u32=308;
 c.r7.u32=ov?0:OUT;c.r8.u32=ov;__imp__XamEnumerate(c,base);return c.r3.u32;}
void Creator(){PPCContext c{};c.r4.u32=RECORD;c.r5.u32=OUT;c.r6.u32=OUT+8;__imp__XamContentGetCreator(c,base);
 CHECK(c.r3.u32==0&&GRead32(base,OUT)==1&&Get64(OUT+8)==0xB13EBABEBABEBABEull);}
uint32_t Directory(uint32_t handle,uint32_t length=328,bool restart=false){PPCContext c{};
 c.r1.u32=STACK;GWrite32(base,STACK+84,restart);c.r3.u32=handle;c.r7.u32=IOS;c.r8.u32=DATA;c.r9.u32=length;
 __imp__NtQueryDirectoryFile(c,base);return c.r3.u32;}
void QueryDirectoryInfo(uint32_t handle){PPCContext c{};c.r3.u32=handle;c.r4.u32=IOS;c.r5.u32=DATA;c.r6.u32=24;c.r7.u32=5;
 __imp__NtQueryInformationFile(c,base);CHECK(c.r3.u32==0&&Get64(DATA)==0&&Get64(DATA+8)==0&&base[DATA+21]==1);
 Path("tag25save:\\");c={};c.r3.u32=OBJECT;c.r4.u32=DATA;__imp__NtQueryFullAttributesFile(c,base);
 CHECK(c.r3.u32==0&&Get64(DATA+40)==0&&GRead32(base,DATA+48)==FILE_ATTRIBUTE_DIRECTORY);}
struct NativeIosb {union {LONG status;void* pointer;};ULONG_PTR information;};
using NativeWrite=LONG(NTAPI*)(HANDLE,HANDLE,void*,void*,NativeIosb*,void*,ULONG,LARGE_INTEGER*,ULONG*);
int64_t Position(HANDLE h){LARGE_INTEGER z{},p{};CHECK(SetFilePointerEx(h,z,&p,FILE_CURRENT));return p.QuadPart;}
void SetPosition(HANDLE h,int64_t v){LARGE_INTEGER p{};p.QuadPart=v;CHECK(SetFilePointerEx(h,p,nullptr,FILE_BEGIN));}
void WriteOracle(const std::filesystem::path& folder){
 CHECK(Open("tag25save:\\oracle.bin",GENERIC_READ|GENERIC_WRITE,5)==0);uint32_t handle=GRead32(base,OUT);auto file=GetFile(handle);
 HANDLE native=CreateFileW((folder/L"native-write.bin").c_str(),GENERIC_READ|GENERIC_WRITE,3,nullptr,CREATE_ALWAYS,128,nullptr);
 CHECK(native!=INVALID_HANDLE_VALUE);auto fn=reinterpret_cast<NativeWrite>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtWriteFile"));CHECK(fn);
 struct C{int64_t off;uint32_t len;bool ptr;};const C cases[]={{0,3,true},{7,3,true},{-2,2,true},{0,2,false},{-1,3,true},{0,0,true},{7,0,true},{-2,0,true},{-1,0,true},{-3,3,true},{-3,0,true}};
 for(const auto& v:cases){
  SetPosition(native,0);SetPosition(file->handle->handle,0);CHECK(SetEndOfFile(native)&&SetEndOfFile(file->handle->handle));
  DWORD n=0;CHECK(WriteFile(native,"0123456789",10,&n,nullptr)&&n==10);CHECK(WriteFile(file->handle->handle,"0123456789",10,&n,nullptr)&&n==10);
  SetPosition(native,2);SetPosition(file->handle->handle,2);std::memcpy(base+DATA,"XYZ",3);GWrite32(base,IOS,0xA5A5A5A5);GWrite32(base,IOS+4,0xA5A5A5A5);
  NativeIosb ios;std::memset(&ios,0xA5,sizeof(ios));LARGE_INTEGER off{};off.QuadPart=v.off;
  uint32_t wanted=uint32_t(fn(native,nullptr,nullptr,nullptr,&ios,base+DATA,v.len,v.ptr?&off:nullptr,nullptr));
  auto got=Write(handle,v.len,v.off,v.ptr);if(got!=wanted)std::fprintf(stderr,"write off%lld length%u got%08X want%08X\n",v.off,v.len,got,wanted);
  CHECK(got==wanted);CHECK(GRead32(base,IOS)==uint32_t(ios.status)&&GRead32(base,IOS+4)==uint32_t(ios.information));
  CHECK(Position(native)==Position(file->handle->handle));
  SetPosition(native,0);SetPosition(file->handle->handle,0);std::array<char,32>a{},b{};DWORD na=0,nb=0;
  CHECK(ReadFile(native,a.data(),32,&na,nullptr)&&ReadFile(file->handle->handle,b.data(),32,&nb,nullptr));CHECK(na==nb&&a==b);
 }
 CloseHandle(native);Close(handle);file.reset();
 CHECK(DeleteFileW((folder/L"B13EBABEBABEBABE"/L"tag25savedata"/L"oracle.bin").c_str()));
}
uint32_t SetInfo(uint32_t handle,uint32_t cls,uint32_t length){PPCContext c{};c.r3.u32=handle;c.r4.u32=IOS;c.r5.u32=DATA;
 c.r6.u32=length;c.r7.u32=cls;__imp__NtSetInformationFile(c,base);return c.r3.u32;}
uint32_t RenameTo(uint32_t handle,const std::string& target,bool replace){GWrite32(base,DATA,replace);GWrite32(base,DATA+4,0);
 GWrite16(base,DATA+8,uint16_t(target.size()));GWrite16(base,DATA+10,uint16_t(target.size()+1));GWrite32(base,DATA+12,DATA+16);
 std::memcpy(base+DATA+16,target.c_str(),target.size()+1);return SetInfo(handle,10,16);}
// TTT2's safe save: write NN_000_0.BIN, delete the old NN_000_1.BIN through a DELETE-only
// handle, then rename _0 over _1. Rename stays inside the mounted container.
void SafeSave(const std::filesystem::path& folder){
 const auto dir=folder/L"B13EBABEBABEBABE"/L"tag25savedata";
 CHECK(Open("tag25save:\\77_000_1.BIN",0x00010000,1,0x4040)==0xC0000034u);         // probe: absent
 for(const char* name:{"tag25save:\\77_000_1.BIN","tag25save:\\77_000_0.BIN"}){
  CHECK(Open(name,GENERIC_READ|GENERIC_WRITE,5,0x48)==0);uint32_t h=GRead32(base,OUT);
  std::memcpy(base+DATA,name+11,4);CHECK(Write(h,4)==0);Close(h);}
 CHECK(Open("tag25save:\\77_000_1.BIN",0x00010000,1,0x4040)==0);uint32_t old=GRead32(base,OUT);  // DELETE only
 base[DATA]=1;CHECK(SetInfo(old,13,1)==0);Close(old);CHECK(!std::filesystem::exists(dir/L"77_000_1.BIN"));
 CHECK(Open("tag25save:\\77_000_0.BIN",GENERIC_READ)==0);uint32_t ro=GRead32(base,OUT);
 CHECK(RenameTo(ro,"tag25save:\\77_000_1.BIN",true)==0xC0000022u);base[DATA]=1;CHECK(SetInfo(ro,13,1)==0xC0000022u);Close(ro);
 CHECK(Open("tag25save:\\77_000_0.BIN",0x00010000|GENERIC_READ,1,0x4040)==0);uint32_t h=GRead32(base,OUT);
 CHECK(RenameTo(h,"tag25save:\\..\\escape.bin",true)==0xC0000033u);CHECK(RenameTo(h,"game:\\escape.bin",true)==0xC0000033u);
 CHECK(RenameTo(h,"tag25save:\\77_000_1.BIN",true)==0);Close(h);
 CHECK(!std::filesystem::exists(dir/L"77_000_0.BIN")&&std::filesystem::file_size(dir/L"77_000_1.BIN")==4);
 CHECK(Open("tag25save:\\77_000_1.BIN",0x00010000,1,0x4040)==0);h=GRead32(base,OUT);base[DATA]=1;CHECK(SetInfo(h,13,1)==0);Close(h);
 CHECK(!std::filesystem::exists(dir/L"77_000_1.BIN"));
}
void Lifecycle(const std::filesystem::path& folder){
 Reset();std::memset(base+RECORD+264,0,42);std::memcpy(base+RECORD+264,"ModeCases",10);
 CHECK(Content(5)==3);CHECK(Content(0)==87);CHECK(Content(6)==87);
 CHECK(Content(1)==0&&GRead32(base,OUT)==1);CHECK(ContentClose()==0);
 CHECK(Content(1)==183);CHECK(Content(4)==0&&GRead32(base,OUT)==2);
 CHECK(Open("tag25save:\\erase.bin",GENERIC_READ|GENERIC_WRITE,2)==0);uint32_t h=GRead32(base,OUT);
 std::memcpy(base+DATA,"x",1);CHECK(Write(h,1)==0);CHECK(ContentClose()==0);CHECK(Delete()==ERROR_BUSY);Close(h);
 CHECK(Content(2)==0&&GRead32(base,OUT)==1);CHECK(!std::filesystem::exists(folder/L"B13EBABEBABEBABE"/L"modecases"/L"erase.bin"));
 std::memcpy(base+ROOT_NAME,"dummysave",10);CHECK(Content(3)==0&&GRead32(base,OUT)==2);
 std::memcpy(base+ROOT_NAME,"tag25save",10);CHECK(ContentClose()==0);CHECK(Content(2)==ERROR_BUSY);
 std::memcpy(base+ROOT_NAME,"dummysave",10);CHECK(ContentClose()==0);
 std::memcpy(base+ROOT_NAME,"tag25save",10);CHECK(Content(5)==0&&GRead32(base,OUT)==1);CHECK(ContentClose()==0);CHECK(Delete()==0);
 // An unrelated folder must not be adopted by CreateAlways or OpenAlways.
 auto orphan=folder/L"B13EBABEBABEBABE"/L"modecases";CHECK(CreateDirectoryW(orphan.c_str(),nullptr));
 CHECK(Content(2)==ERROR_FILE_CORRUPT);CHECK(Content(4)==ERROR_FILE_CORRUPT);CHECK(RemoveDirectoryW(orphan.c_str()));
 // A directory junction is a real reparse point and requires no symlink privilege.
 auto outside=folder/L"outside";CHECK(CreateDirectoryW(outside.c_str(),nullptr));CHECK(CreateDirectoryW(orphan.c_str(),nullptr));
 HANDLE junction=CreateFileW(orphan.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);CHECK(junction!=INVALID_HANDLE_VALUE);
 std::wstring target=L"\\??\\"+outside.wstring();std::vector<uint8_t> data(16+(target.size()+1)*2+2,0);
 *reinterpret_cast<DWORD*>(data.data())=0xA0000003;*reinterpret_cast<WORD*>(data.data()+4)=WORD(data.size()-8);
 *reinterpret_cast<WORD*>(data.data()+10)=WORD(target.size()*2);*reinterpret_cast<WORD*>(data.data()+12)=WORD((target.size()+1)*2);
 std::memcpy(data.data()+16,target.data(),target.size()*2);DWORD n=0;
 CHECK(DeviceIoControl(junction,0x000900A4,data.data(),DWORD(data.size()),nullptr,0,&n,nullptr));CloseHandle(junction);
 CHECK(Content(1)==ERROR_ACCESS_DENIED);CHECK(Content(2)==ERROR_ACCESS_DENIED);CHECK(RemoveDirectoryW(orphan.c_str()));CHECK(RemoveDirectoryW(outside.c_str()));
 Reset();
}
int main(int argc,char**argv){CHECK(argc==3);_putenv_s("TTT2_SAVE_ROOT",argv[1]);_putenv_s("TTT2_VFS_ROOT",argv[1]);
 const std::filesystem::path folder=argv[1];const auto container=folder/L"B13EBABEBABEBABE"/L"tag25savedata";
 Reset();const bool reopen=std::string(argv[2])=="reopen";
 auto* event=new KEvent(false,false);uint32_t eh=KernelObjectHandle(event);
 if(!reopen){
  Lifecycle(folder);
  uint32_t en=Enumerate();CHECK(Next(en)==18&&GRead32(base,OUT)==0);Close(en);
  CHECK(Content(3)==3&&GRead32(base,OUT)==0);CHECK(!std::filesystem::exists(container));
  Overlap(eh);CHECK(Content(1,OV)==997);CHECK(GRead32(base,OV)==0&&GRead32(base,OV+4)==1&&GRead32(base,OV+24)==0&&event->Wait(0)==0);
  CHECK(GRead32(base,OUT)==1&&GRead32(base,OUT+8)==0);CHECK(base[OV-1]==0xA5&&base[OV+28]==0xA5&&GRead32(base,OV+8)==0x12345678);
  Creator();CHECK(Delete()==ERROR_BUSY);CHECK(Content(4)==ERROR_ALREADY_EXISTS);
  WriteOracle(folder);
  SafeSave(folder);
  CHECK(Open("tag25save:\\00_001_0.BIN",GENERIC_READ|GENERIC_WRITE,2)==0);uint32_t h=GRead32(base,OUT);
  CHECK(GRead32(base,IOS+4)==2);std::memcpy(base+DATA,"persistent-save",15);CHECK(Write(h,15)==0&&GRead32(base,IOS+4)==15);
  CHECK(ContentClose(true)==0);PPCContext c{};c.r3.u32=h;c.r4.u32=IOS;__imp__NtFlushBuffersFile(c,base);CHECK(c.r3.u32==0&&GRead32(base,IOS+4)==0);
  Close(h);CHECK(ContentClose()==0);CHECK(Open("tag25save:\\00_001_0.BIN",GENERIC_READ)==0xC000003Au);
  CHECK(Content(1)==183&&GRead32(base,OUT)==0);CHECK(Content(3)==0&&GRead32(base,OUT)==2);
 }else{CHECK(Content(3)==0&&GRead32(base,OUT)==2);Creator();}
 CHECK(Open("tag25save:\\00_001_0.BIN",GENERIC_READ)==0);uint32_t h=GRead32(base,OUT);std::memset(base+DATA,0xA5,32);
 CHECK(Read(h,32)==0&&GRead32(base,IOS+4)==15&&!std::memcmp(base+DATA,"persistent-save",15));CHECK(base[DATA+15]==0xA5);
 CHECK(Write(h,1)==0xC0000022u);Close(h);
 CHECK(Open("tag25save:\\",GENERIC_READ,1,0x4021,false)==0);h=GRead32(base,OUT);QueryDirectoryInfo(h);
 std::memset(base+DATA,0xA5,400);CHECK(Directory(h,64)==0xC0000023u);CHECK(base[DATA]==0xA5);
 CHECK(Directory(h)==0&&GRead32(base,DATA+60)==12&&Get64(DATA+40)==15);
 CHECK(std::string(reinterpret_cast<char*>(base+DATA+64),12)=="00_001_0.BIN");CHECK(GRead32(base,IOS+4)==76&&base[DATA+76]==0xA5);
 CHECK(Directory(h)==0x80000006u&&GRead32(base,IOS+4)==0);CHECK(Directory(h,328,true)==0);
 // TTT2's load: a name relative to the container directory handle (ObjectAttributes.RootDirectory).
 Path("00_001_0.BIN");GWrite32(base,OBJECT,h);{PPCContext c{};c.r3.u32=OUT;c.r4.u32=GENERIC_READ;c.r5.u32=OBJECT;c.r6.u32=IOS;
  c.r7.u32=1;c.r8.u32=0x60;__imp__NtOpenFile(c,base);CHECK(c.r3.u32==0);}
 {uint32_t rel=GRead32(base,OUT);std::memset(base+DATA,0xA5,32);CHECK(Read(rel,32)==0&&GRead32(base,IOS+4)==15&&!std::memcmp(base+DATA,"persistent-save",15));Close(rel);}
 Path("missing.bin");GWrite32(base,OBJECT,h);{PPCContext c{};c.r3.u32=OUT;c.r4.u32=GENERIC_READ;c.r5.u32=OBJECT;c.r6.u32=IOS;
  c.r7.u32=1;c.r8.u32=0x60;__imp__NtOpenFile(c,base);CHECK(c.r3.u32==0xC0000034u);}
 Close(h);
 uint32_t en=Enumerate();Overlap(eh);CHECK(Next(en,OV)==997&&GRead32(base,OV)==0&&GRead32(base,OV+4)==1&&event->Wait(0)==0);
 CHECK(!std::memcmp(base+DATA,base+RECORD,308));CHECK(Next(en)==18&&GRead32(base,OUT)==0);Close(en);
 for(const char* bad:{"tag25save:\\..\\sentinel.bin","tag25save:\\x/y","tag25save:\\x:ads","tag25save:\\CON","tag25save:\\.ttt2-content-v1","tag25save:\\\\sentinel.bin"}){
  CHECK(Open(bad,GENERIC_READ|GENERIC_WRITE,5)==0xC0000033u);CHECK(GRead32(base,OUT)==0xA5A5A5A5);
 }
 CHECK(Open("game:\\sentinel.bin",GENERIC_WRITE,5)==0xC0000022u);
 CHECK(ContentClose()==0);CHECK(ContentClose()==3);
 if(reopen){CHECK(Delete()==0&&!std::filesystem::exists(container));CHECK(Delete()==3);uint32_t e=Enumerate();CHECK(Next(e)==18);Close(e);}
 std::printf("PASS %u save checks (%s): persistent roundtrip, native write oracle, enumeration, directories, ownership, paths and readonly disc\n",checks,argv[2]);
}
'''

def main():
    kernel = ROOT / "port/src/runtime/kernel"
    source = CHECKER
    for key, name in [("OBJECTS", "kernel_objects.cpp"), ("CONTENT", "kernel_content.cpp"),
                      ("STORE", "save_store.cpp"), ("IO", "kernel_io.cpp")]:
        source = source.replace(f"@{key}@", (kernel / name).as_posix())
    runtime=ROOT / "port/src/runtime"
    for key,name in (("HOST_FILE_COMMON","host_file_common.cpp"),("HOST_FILE_WINDOWS","host_file_windows.cpp"),("HOST_STORAGE_WINDOWS","host_storage_windows.cpp"),("HOST_THREAD","host_thread_sdl.cpp")):
        source=source.replace(f"@{key}@",(runtime/name).as_posix())
    sdl=Path(os.environ['SDL3_DIR']).parent
    source=source.replace('@SDL_LIB@',(sdl/'lib/x64/SDL3.lib').as_posix())
    with tempfile.TemporaryDirectory(prefix="ttt2-save-") as temp:
        folder = Path(temp)
        (folder / "ppc_recomp_shared.h").write_text(SHARED)
        includes = [folder, RECOMP / "XenonUtils", RECOMP / "thirdparty/simde",
                    ROOT / "port/src/runtime", kernel, sdl/"include"]
        # This harness unity-includes production modules to inspect file handles.
        # Remove only the duplicate default-argument declaration in its IO copy.
        io_path = folder / "kernel_io_test.cpp"
        io_text = (kernel / "kernel_io.cpp").read_text(encoding="utf-8-sig")
        declaration = 'void RollbackNoteIo(bool frameOnly, const char* op = "", const PPCContext* ctx = nullptr);'
        assert declaration in io_text
        io_text = io_text.replace(declaration, "", 1)
        io_path.write_text(io_text, encoding="utf-8")
        source = source.replace((kernel / "kernel_io.cpp").as_posix(), io_path.as_posix())
        exe = compile_one(folder, "save_persistence", source, includes)
        sentinel = folder / "sentinel.bin"
        sentinel.write_bytes(b"disc-must-stay-readonly")
        for phase in ("create", "reopen"):
            result = subprocess.run([str(exe), str(folder), phase], capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout, end="")
            assert sentinel.read_bytes() == b"disc-must-stay-readonly"
        print("PASS new-process reopen and disc sentinel unchanged")
        # Removing either a real write or component validation must fail before
        # the harness can claim a persistent or confined save.
        io = io_text
        start = io.index("PPC_FUNC(__imp__NtWriteFile)\n{")
        end = io.index("\nPPC_FUNC(__imp__NtWriteFileGather)", start)
        no_write = io[:start] + "PPC_FUNC(__imp__NtWriteFile) {ctx.r3.u32=0;}\n" + io[end:]
        store = (kernel / "save_store.cpp").read_text(encoding="utf-8-sig")
        unchecked = store.replace("if(!tail.empty()&&!Component(tail))", "if(false)")
        assert unchecked != store
        for name, changed in [
            ("false_write", source.replace(f'#include "{io_path.as_posix()}"', no_write)),
            ("unchecked_path", source.replace(f'#include "{(kernel / "save_store.cpp").as_posix()}"', unchecked)),
        ]:
            run_folder = folder / name
            run_folder.mkdir()
            (run_folder / "sentinel.bin").write_bytes(b"disc-must-stay-readonly")
            exe = compile_one(folder, name, changed, includes)
            result = subprocess.run([str(exe), str(run_folder), "create"], capture_output=True, text=True, timeout=30)
            if result.returncode == 0 or "FAIL line" not in result.stderr:
                raise RuntimeError(name + " negative control failed:\n" + result.stdout + result.stderr)
            assert (run_folder / "sentinel.bin").read_bytes() == b"disc-must-stay-readonly"
            print("PASS " + name + " negative control rejected: " + result.stderr.strip().splitlines()[-1])

if __name__ == "__main__":
    main()
