"""Directional truth tables and transitions; no game input or saves required."""
from pathlib import Path
import tempfile,subprocess
from test_vcmpgtuh import compile_one,ROOT
CODE=r'''
#include "direction_filter.h"
#include <cstdio>
#include <initializer_list>
using namespace input_direction;
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d\n",__LINE__);return 1;}++checks;}while(0)
int main(){unsigned checks=0;
for(int mode=0;mode<4;++mode)for(unsigned mask=0;mask<16;++mask){
 Filter f;unsigned got=f.Apply(mask|0x1000,mode);CHECK((got&0x1000)!=0);
 unsigned expected=mask;
 if(mode){if((mask&12)==12)expected&=~12;if((mask&3)==3){expected&=~3;if(mode==UpPriority)expected|=1;}}
 CHECK((got&15)==expected);
}
for(int value=-32768;value<=32767;++value) {
 CHECK(Stick(int16_t(value),true,0)==0);
 CHECK(Stick(int16_t(value),false,0)==value);
 const int magnitude=value<0?-value:value;
 CHECK(Stick(int16_t(value),false,20)==(magnitude<=6553?0:value));
}
Filter f;CHECK(f.Apply(4,LastInput)==4);CHECK(f.Apply(12,LastInput)==8);
CHECK(f.Apply(12,LastInput)==8);CHECK(f.Apply(4,LastInput)==4);
CHECK(f.Apply(0,LastInput)==0);CHECK(f.Apply(12,LastInput)==0);
CHECK(f.Apply(8,LastInput)==8);CHECK(f.Apply(12,LastInput)==4);
Filter y;CHECK(y.Apply(2,LastInput)==2);CHECK(y.Apply(3,LastInput)==1);
CHECK(y.Apply(2,LastInput)==2);CHECK(y.Apply(0,LastInput)==0);
Filter independent;CHECK(independent.Apply(12,LastInput)==0);
// Asymmetric cardinal drift, both signs and the -32768 extreme. The
// boundary stays diagonal; zero leaves even a weak secondary axis intact.
for(int sx:{-1,1})for(int sy:{-1,1}) {
 int16_t x=int16_t(sx*20000),y=int16_t(sy*9999);
 TuneDiagonal(x,y,50);CHECK(x==sx*20000 && y==0);
 x=int16_t(sx*20000);y=int16_t(sy*10000);
 TuneDiagonal(x,y,50);CHECK(x==sx*20000 && y==sy*10000);
 x=int16_t(sx*9999);y=int16_t(sy*20000);
 TuneDiagonal(x,y,50);CHECK(x==0 && y==sy*20000);
 x=int16_t(sx*1);y=int16_t(sy*32767);
 TuneDiagonal(x,y,0);CHECK(x==sx && y==sy*32767);
}
int16_t dx=-32768,dy=32767;TuneDiagonal(dx,dy,50);CHECK(dx==-32768 && dy==32767);
dx=-32768;dy=100;TuneDiagonal(dx,dy,100);CHECK(dx==-32768 && dy==0);
dx=0;dy=0;TuneDiagonal(dx,dy,100);CHECK(dx==0 && dy==0);
std::printf("PASS %u SOCD truth-table and transition checks\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-socd-') as temp:
 exe=compile_one(Path(temp),'socd',CODE,[ROOT/'port/src/runtime/kernel'])
 subprocess.run([str(exe)],check=True)
