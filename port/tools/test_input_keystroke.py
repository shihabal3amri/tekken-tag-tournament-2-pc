"""Verify the production XAM no-event ABI without a live game or input device."""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
union Register { uint32_t u32; uint64_t u64; };
struct Context { Register r3{},r4{},r5{}; };
#define PPC_FUNC(name) void name(Context& ctx,uint8_t* base)
void KTrace(const char*) {}
unsigned started=0;
void EnsurePadThread() { ++started; }
struct Pad { std::atomic<bool> connected{false}; } g_pads[4];
uint32_t GRead32(uint8_t* base,uint32_t p) {
    return (uint32_t(base[p])<<24)|(uint32_t(base[p+1])<<16)|
           (uint32_t(base[p+2])<<8)|base[p+3];
}
@PRODUCTION@
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); return 1; } }while(0)
int main() {
    std::array<uint8_t,128> memory;
    auto call=[&](uint32_t user,uint32_t flags,uint32_t userPtr=16,uint32_t out=64) {
        memory.fill(0xA5);
        for(unsigned i=0;i<4;++i) memory[16+i]=uint8_t(user>>(24-i*8));
        auto original=memory;
        Context ctx;ctx.r3.u32=userPtr;ctx.r4.u32=flags;ctx.r5.u32=out;
        __imp__XamInputGetKeystrokeEx(ctx,memory.data());
        if(memory!=original) return uint32_t(0xDEADBEEF);
        return ctx.r3.u32;
    };
    CHECK(call(0,1)==1167); // disconnected pad cannot provide an event
    g_pads[0].connected=true;
    for(uint32_t user=0;user<4;++user) CHECK(call(user,1)==(user==0?4306:1167));
    CHECK(call(0,0)==4306);
    CHECK(call(0,2)==1167); // the game's raw-keyboard query: no such device
    CHECK(call(0,3)==4306); // flags can include the supported gamepad bit
    CHECK(call(0xFF,1)==4306);
    CHECK(call(3,0x40000001)==4306);
    CHECK(call(4,1)==1167);
    CHECK(call(0,1,0)==160);
    CHECK(call(0,1,16,0)==160);
    g_pads[0].connected=false;g_pads[2].connected=true;
    CHECK(call(0xFF,1)==4306);
    CHECK(call(0,0x40000001)==4306);
    g_pads[2].connected=false;
    CHECK(call(0xFF,1)==1167);
    std::printf("PASS %u XAM keystroke checks: registers, device filter, empty/disconnected, any-user, unchanged output canaries\n",checks);
}
'''


def main():
    source = (ROOT / "port/src/runtime/kernel/kernel_xam.cpp").read_text(encoding="utf-8-sig")
    begin = source.index("PPC_FUNC(__imp__XamInputGetKeystrokeEx)")
    end = source.index("\n}", begin) + 2
    # Upstream generates stubs into each build directory. Verify their source
    # generator excludes the explicitly implemented import.
    stubs_path=ROOT / "port/src/runtime/kernel_stubs.gen.cpp"
    if stubs_path.exists():
        assert "PPC_FUNC(__imp__XamInputGetKeystrokeEx)" not in stubs_path.read_text()
    with tempfile.TemporaryDirectory(prefix="ttt2-keystroke-") as temp:
        executable = compile_one(Path(temp), "keystroke", HARNESS.replace("@PRODUCTION@", source[begin:end]), [])
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
