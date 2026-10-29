"""Round-trip the extended AA enum through production settings, preserving unrelated keys."""
from pathlib import Path
import tempfile,subprocess,os
from test_vcmpgtuh import ROOT,compile_one
CODE=r'''
#include "host_settings.cpp"
#include "host_settings_windows.cpp"
#include "display_settings.cpp"
int main(int argc,char** argv){
 if(argc!=2||display_settings::GetAntiAliasing()!=std::atoi(argv[1]))return 1;
 display_settings::SetAntiAliasing(0);display_settings::SetAntiAliasing(2);
 const char* ini=std::getenv("TTT2_SETTINGS_INI");
 if(GetPrivateProfileIntA("Display","AntiAliasing",99,ini)!=2)return 2;
 if(GetPrivateProfileIntA("Online","CustomDelay",0,ini)!=4)return 3;
 char key[32]{};GetPrivateProfileStringA("Keyboard","Start","",key,32,ini);
 if(std::string(key)!="0x4E")return 4;
 puts("PASS AA settings: SMAA round-trip; online and keyboard settings preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-aa-settings-') as temp:
 folder=Path(temp);exe=compile_one(folder,'aa',CODE,[ROOT/'port/src/runtime/kernel'])
 for mode,want in ((0,0),(1,1),(2,2),(99,2)):
  ini=folder/'settings.ini';ini.write_text(f'[Display]\nAntiAliasing={mode}\n[Online]\nCustomDelay=4\n[Keyboard]\nStart=0x4E\n')
  subprocess.run([str(exe),str(want)],env=dict(os.environ,TTT2_SETTINGS_INI=str(ini)),check=True,timeout=10)
