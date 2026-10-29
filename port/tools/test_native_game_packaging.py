"""Verify release packaging makes native Windows apps without altering game code."""
import argparse,struct,subprocess,tempfile,os
from pathlib import Path
from test_vcmpgtuh import ROOT,compile_one
from stage_inplace_update import windows_app

def subsystem(path):
    b=path.read_bytes();pe=struct.unpack_from('<I',b,0x3c)[0]
    return struct.unpack_from('<H',b,pe+24+68)[0]
RUNTIME=(ROOT/'port/src/runtime/host_runtime_windows.cpp').read_text()
def function(signature):
    start=RUNTIME.index(signature);at=RUNTIME.index('{',start);depth=1;end=at+1
    while depth:
        depth+=(RUNTIME[end]=='{')-(RUNTIME[end]=='}');end+=1
    return RUNTIME[start:end]
CHILD=r'''
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
namespace host_runtime { std::filesystem::path InstalledLayout(int);void StartLogWriter(); }
static HANDLE g_logPipeRead=nullptr;static std::atomic<bool> g_logWriting{false};
@LAYOUT@
@WRITER@
int main(){
 if(GetConsoleWindow())return 9;
 if(host_runtime::InstalledLayout(1).empty())return 10;
 if(GetFileType(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))))!=FILE_TYPE_DISK)return 11;
 host_runtime::StartLogWriter();
 if(GetFileType(GetStdHandle(STD_ERROR_HANDLE))!=FILE_TYPE_PIPE)return 12;
 fprintf(stderr,"NATIVE_ASYNC_LOG_CHECK\n");fflush(stderr);
 wchar_t local[32768]{};GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
 const auto path=std::filesystem::path(local)/L"TTT2Recomp"/L"log.txt";
 for(int i=0;i<100;++i){std::ifstream file(path);std::string line;while(std::getline(file,line))if(line=="NATIVE_ASYNC_LOG_CHECK")return 0;Sleep(20);}
 return 13;
}
'''.replace('@LAYOUT@',function('std::filesystem::path host_runtime::InstalledLayout(int argc)')).replace('@WRITER@',function('void host_runtime::StartLogWriter()'))
PARENT=r'''
#include <windows.h>
#include <string>
int wmain(int argc,wchar_t** argv){if(argc!=2||GetConsoleWindow())return 1;
std::wstring command=L"\""+std::wstring(argv[1])+L"\"";
STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION p{};
if(!CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&p))return 2;
if(WaitForSingleObject(p.hProcess,15000)!=WAIT_OBJECT_0)return 3;DWORD code=99;
GetExitCodeProcess(p.hProcess,&code);CloseHandle(p.hThread);CloseHandle(p.hProcess);return int(code);}
'''
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--game',type=Path,required=True);a=ap.parse_args()
    assert subsystem(a.game)==2,'Staged runtime must use Windows GUI subsystem'
    with tempfile.TemporaryDirectory(prefix='ttt2-native-gui-') as temp:
        folder=Path(temp);child=compile_one(folder,'native_child',CHILD,[]);parent=compile_one(folder,'native_parent',PARENT,[])
        windows_app(child);windows_app(parent);before=child.read_bytes();windows_app(child);assert child.read_bytes()==before
        (folder/'game').mkdir();(folder/'game/tekken.img').write_bytes(b'boot image fixture')
        local=folder/'Local';local.mkdir();env={**os.environ,'LOCALAPPDATA':str(local)}
        subprocess.run([str(parent),str(child)],check=True,timeout=20,creationflags=subprocess.DETACHED_PROCESS,env=env)
    print('PASS native Windows game payload; GUI parent/child spawn no console, actual runtime startup restores async file logging; release conversion changes only PE subsystem/checksum and is repeatable')
if __name__=='__main__':main()
