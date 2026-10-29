"""Verify both ordinary and external-original launch with a native disposable child."""
import subprocess,tempfile
from pathlib import Path
from test_vcmpgtuh import ROOT,compile_one
CHILD=r'''
#include <windows.h>
#include <filesystem>
int main(int argc,char**){
 wchar_t value[32768]{},expected[32768]{};
 for(auto key:{L"TTT2_SAVE_ROOT",L"TTT2_SETTINGS_INI",L"TTT2_LAUNCHER_INI",L"TTT2_NET",L"TTT2_INPUT_SCRIPT",L"TTT2_NEW_OVERRIDE"})if(GetEnvironmentVariableW(key,value,32768))return 10;
 if(!GetEnvironmentVariableW(L"TTT2TEST_EXPECTED_CWD",expected,32768)||std::filesystem::current_path()!=expected)return 11;
 if(!GetEnvironmentVariableW(L"LOCALAPPDATA",value,32768)||argc!=1)return 12;
 if(GetEnvironmentVariableW(L"TTT2TEST_ASSETS",expected,32768)){
  if(!GetEnvironmentVariableW(L"TTT2_VFS_ROOT",value,32768)||std::filesystem::path(value)!=expected)return 13;
  for(auto pair:{std::pair{L"TTT2_GPU_HW",L"1"},std::pair{L"TTT2_FBDUMP",L"0"}}){GetEnvironmentVariableW(pair.first,value,32768);if(std::wstring(value)!=pair.second)return 14;}
 }else if(GetEnvironmentVariableW(L"TTT2_VFS_ROOT",value,32768))return 15;
 return 0;
}
'''
PARENT=r'''
#include "original_launch.h"
int wmain(int argc,wchar_t** argv){
 if(argc<2||argc>3)return 1;
 for(auto key:{L"TTT2_SAVE_ROOT",L"TTT2_SETTINGS_INI",L"TTT2_LAUNCHER_INI",L"TTT2_VFS_ROOT",L"TTT2_NET",L"TTT2_INPUT_SCRIPT",L"TTT2_NEW_OVERRIDE"})SetEnvironmentVariableW(key,L"offline-sentinel");
 std::filesystem::path exe=argv[1],root=argc==3?std::filesystem::path(argv[2]):std::filesystem::path{};
 SetEnvironmentVariableW(L"TTT2TEST_EXPECTED_CWD",(root.empty()?exe.parent_path():root).c_str());
 SetEnvironmentVariableW(L"TTT2TEST_ASSETS",root.empty()?nullptr:(root/L"game").c_str());
 PROCESS_INFORMATION p{};if(!original_launch::Start(exe,p,root))return 2;
 if(WaitForSingleObject(p.hProcess,15000)!=WAIT_OBJECT_0)return 3;
 DWORD result=99;GetExitCodeProcess(p.hProcess,&result);CloseHandle(p.hThread);CloseHandle(p.hProcess);
 wchar_t value[64]{};if(!GetEnvironmentVariableW(L"TTT2_SAVE_ROOT",value,64)||std::wstring(value)!=L"offline-sentinel")return 4;return int(result);
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-original-launch-') as temp:
    folder=Path(temp);child=compile_one(folder,'original child',CHILD,[]);parent=compile_one(folder,'launch_check',PARENT,[ROOT/'port/src/launcher'])
    subprocess.run([str(parent),str(child)],check=True,timeout=20)
    installation=folder/'main game';(installation/'game').mkdir(parents=True)
    subprocess.run([str(parent),str(child),str(installation)],check=True,timeout=20)
    print('PASS direct and external original: normal user data, offline overrides removed, correct assets/cwd, native startup without image arguments, unchanged parent')
