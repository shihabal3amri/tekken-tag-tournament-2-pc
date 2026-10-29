"""Exercise extended settings persistence, bounds and restart behavior."""
import os, subprocess, tempfile
from pathlib import Path
from test_vcmpgtuh import ROOT,compile_one
CODE=r'''
#include "host_settings.cpp"
#include "host_settings_windows.cpp"
#include "display_settings.cpp"
#include <cstdio>
using namespace display_settings;
#define CHECK(x) do{++checks;if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}}while(0)
int main(int argc,char**){unsigned checks=0;
CHECK(!GetCalibrationPattern()); // transient reference must start disabled, including restart
SetCalibrationPattern(true);CHECK(GetCalibrationPattern());
SetCalibrationPattern(false);CHECK(!GetCalibrationPattern());
if(argc>1){CHECK(GetBrightness()==150);CHECK(GetGamma()==50);CHECK(GetContrast()==150);CHECK(GetGpuAdapter()==31);
 CHECK(GetTextureMipBias()==3);CHECK(GetRenderScale()==50);CHECK(GetPerformanceStats()==2);CHECK(GetMonitorDevice()==L"\\\\.\\DISPLAY1");}
else {
 CHECK(GetBrightness()==100&&GetGamma()==100&&GetContrast()==100);
 CHECK(GetGpuAdapter()==-1&&GetRenderScale()==0&&GetTextureMipBias()==0&&GetPerformanceStats()==0);
 const auto generation=Generation();SetBrightness(999);SetGamma(-1);SetContrast(999);SetGpuAdapter(999);
 SetTextureMipBias(9);SetRenderScale(1);SetPerformanceStats(9);SetMonitorDevice(L"\\\\.\\DISPLAY1");
 CHECK(Generation()>generation);
 CHECK(GetBrightness()==150&&GetGamma()==50&&GetContrast()==150&&GetGpuAdapter()==31);
 CHECK(GetRenderScale()==50&&GetTextureMipBias()==3&&GetPerformanceStats()==2);
}
const char* path=std::getenv("TTT2_SETTINGS_INI");CHECK(GetPrivateProfileIntA("Online","CustomDelay",0,path)==4);
CHECK(GetPrivateProfileIntA("Keyboard","Start",0,path)==78);
puts("PASS graphics settings: default/clamp/persist/restart, display generation, unrelated online/bindings preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-graphics-settings-') as temp:
    folder=Path(temp);exe=compile_one(folder,'settings',CODE,[ROOT/'port/src/runtime/kernel'])
    ini=folder/'settings.ini';ini.write_text('[Online]\nCustomDelay=4\n[Keyboard]\nStart=78\n')
    env=dict(os.environ,TTT2_SETTINGS_INI=str(ini))
    subprocess.run([str(exe)],env=env,check=True)
    subprocess.run([str(exe),'restart'],env=env,check=True)
