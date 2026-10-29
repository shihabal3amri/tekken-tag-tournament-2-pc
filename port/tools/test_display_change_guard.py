"""Test display transaction timing without changing Windows display modes."""
from pathlib import Path
import tempfile,subprocess
from test_vcmpgtuh import ROOT,compile_one
CODE=r'''
#include "display_change_guard.h"
#include <cstdio>
#define CHECK(x) do{if(!(x)){printf("FAIL %d\n",__LINE__);return 1;}++checks;}while(0)
int main(){unsigned checks=0;DisplayChangeGuard g;
CHECK(!g.Observe({0,2},0));CHECK(!g.pending);
CHECK(g.Observe({2,4},100));CHECK(g.pending);CHECK(!g.Expired(15099));CHECK(g.Expired(15100));
auto old=g.Revert();CHECK(old.mode==0&&old.resolution==2);CHECK(!g.pending);
CHECK(!g.Observe({0,2},16000));CHECK(g.Observe({1,5},17000));g.Confirm();CHECK(!g.pending);
CHECK(g.Observe({2,6},18000));old=g.Revert();CHECK(old.mode==1&&old.resolution==5);
CHECK(g.Observe({0,1},19000));CHECK(!g.Observe({0,1},20000));CHECK(g.deadline==34000);
CHECK(g.Observe({2,4},21000));CHECK(g.deadline==36000);
CHECK(!g.Observe({1,5},22000));CHECK(!g.pending);
printf("PASS %u display-change checks: first load, confirm, cancel, timeout, repeated and returned selections\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='ttt2-display-guard-') as temp:
 exe=compile_one(Path(temp),'guard',CODE,[ROOT/'port/src/runtime/kernel']);subprocess.run([str(exe)],check=True)
