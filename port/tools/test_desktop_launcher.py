"""Install into temporary state/desktop and inspect the real .lnk and startup helper.

No game or graphical launcher is opened; the actual desktop is untouched.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_vcmpgtuh import ROOT, compile_one

CODE=r'''
#define WIN32_LEAN_AND_MEAN
#include "preview_environment.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <cstdio>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"uuid.lib")
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
std::wstring Env(const wchar_t* key){wchar_t b[32768]{};GetEnvironmentVariableW(key,b,32768);return b;}
int wmain(int argc,wchar_t** argv){
 CHECK(argc==4);const fs::path state=argv[1],desktop=argv[2];
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 ComPtr<IShellLinkW> link;ComPtr<IPersistFile> file;
 CHECK(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))));
 CHECK(SUCCEEDED(link.As(&file)));
 CHECK(SUCCEEDED(file->Load((desktop/L"TTT2 Offline Launcher.lnk").c_str(),STGM_READ)));
 wchar_t target[32768]{},cwd[32768]{},args[32768]{};
 CHECK(SUCCEEDED(link->GetPath(target,32768,nullptr,SLGP_RAWPATH)));
 CHECK(SUCCEEDED(link->GetWorkingDirectory(cwd,32768)));
 CHECK(SUCCEEDED(link->GetArguments(args,32768)) && !*args);
 CHECK(fs::path(target)==state/L"app"/L"TTT2 Launcher.exe");
 CHECK(fs::path(cwd)==state/L"app");
 for(const auto* name:{L"TTT2_SETTINGS_INI",L"TTT2_SAVE_ROOT",L"TTT2_VFS_ROOT",L"TTT2_NET"})SetEnvironmentVariableW(name,L"inherited-wrong-value");
 SetEnvironmentVariableW(L"TTT2_INPUT_SCRIPT",L"1:START:10");SetEnvironmentVariableW(L"TTT2_AUTOSTART",L"1");
 const bool reject=std::wstring(argv[3])==L"reject";
 try {
  CHECK(preview_environment::Configure(target));CHECK(!reject);
 }catch(const std::exception&){CHECK(reject);puts("PASS missing preview/assets rejected without fallback to original settings");return 0;}
 CHECK(fs::path(Env(L"TTT2_SETTINGS_INI"))==state/L"settings.ini");
 CHECK(fs::path(Env(L"TTT2_SAVE_ROOT"))==state/L"saves");
 wchar_t original[32768]{};GetPrivateProfileStringW(L"Preview",L"InstalledGame",L"",original,32768,(state/L"preview.ini").c_str());
 CHECK(fs::path(Env(L"TTT2_VFS_ROOT"))==fs::path(original)/L"game");
 CHECK(Env(L"TTT2_NET")==L"loopback");CHECK(Env(L"TTT2_INPUT_SCRIPT").empty() && Env(L"TTT2_AUTOSTART").empty());
 CHECK(!preview_environment::Configure(state.parent_path()/L"ordinary"/L"launcher.exe"));
 CHECK(fs::path(Env(L"TTT2_SAVE_ROOT"))==state/L"saves");
 CoUninitialize();puts("PASS installed desktop shortcut targets launcher directly; fresh startup restores isolated preview environment");
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--updater',type=Path,required=True)
    parser.add_argument('--installed-game-dir',type=Path,required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ttt2-desktop-') as tmp:
        root=Path(tmp);state=root/'preview with spaces';desktop=root/'test desktop'
        desktop.mkdir();original_shortcut=desktop/'Tekken Tag Tournament 2.lnk'
        original_shortcut.write_bytes(b'preserve original online shortcut')
        original_settings=root/'original.ini';original_settings.write_text('[Keyboard]\nStart=78\n')
        original_saves=root/'original saves';original_saves.mkdir();(original_saves/'save.bin').write_bytes(b'original progress')
        command=[str(args.updater.resolve()),'--prepare-only','--install-dir',str(args.installed_game_dir.resolve()),
                 '--state-dir',str(state),'--source-settings',str(original_settings),'--source-saves',str(original_saves),
                 '--shortcut-dir',str(desktop)]
        subprocess.run(command,check=True,timeout=120,capture_output=True)
        checker=compile_one(root,'shortcut_check',CODE,[ROOT/'port/src/launcher'])
        subprocess.run([str(checker),str(state),str(desktop),'valid'],check=True,timeout=15)
        (state/'saves/save.bin').write_bytes(b'new preview progress')
        subprocess.run(command,check=True,timeout=120,capture_output=True)
        subprocess.run([str(checker),str(state),str(desktop),'valid'],check=True,timeout=15)
        assert (state/'saves/save.bin').read_bytes()==b'new preview progress'
        assert len(list(desktop.iterdir()))==2,'Duplicate or temporary shortcuts left behind'
        assert original_shortcut.read_bytes()==b'preserve original online shortcut'
        assert original_settings.read_text()=='[Keyboard]\nStart=78\n'
        assert (original_saves/'save.bin').read_bytes()==b'original progress'
        (state/'preview.ini').write_text('[Preview]\nInstalledGame='+str(root/'missing-install')+'\n')
        subprocess.run([str(checker),str(state),str(desktop),'reject'],check=True,timeout=15)
    print('PASS repeat install retains one offline shortcut, preview progress and original online shortcut/settings/saves')

if __name__=='__main__':main()
