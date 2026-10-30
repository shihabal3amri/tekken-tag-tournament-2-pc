"""Check the production guest NTSTATUS conversion against Windows ntdll and known errors.

This covers statuses emitted by kernel_io.cpp, the async file source's EOF=38
contract, unknown statuses, register/memory canaries, and the original low-bits
stub as a failing negative control. No game or game files are used.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
union Register { uint32_t u32; uint64_t u64; };
struct PPCContext { Register r3{},r4{},r5{}; };
#define PPC_FUNC(name) void name(PPCContext& ctx,uint8_t* base)
void KTrace(const char*) {}
@PRODUCTION@
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    using Convert=ULONG (NTAPI*)(LONG);
    auto native=reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError"));
    CHECK(native!=nullptr);
    struct Case { uint32_t status,error; };
    const Case known[]={
        {0,0}, {0xC0000011,38}, // file source explicitly expects ERROR_HANDLE_EOF
        {0xC0000001,31}, {0xC0000008,6}, {0xC000000D,87},
        {0xC0000022,5}, {0xC0000034,2}, {0xC0000054,33},
        {0xC000009C,23}, {0xC0000185,1117}, {0xC0000120,995},
        {0xC000009A,1450}, {0x00000103,997}, {0x80000005,234},
        {0xC0070005,5}, {0xC000FFFF,317}};
    std::array<uint8_t,64> memory; memory.fill(0xA5);auto original=memory;
    for(auto c:known) {
        CHECK(native(static_cast<LONG>(c.status))==c.error);
        PPCContext ctx;ctx.r3.u64=0x1234567800000000ull|c.status;
        ctx.r4.u64=0xFEDCBA9876543210ull;ctx.r5.u64=0xAABBCCDDEEFF0011ull;
        __imp__RtlNtStatusToDosError(ctx,memory.data());
        if(ctx.r3.u32!=c.error) std::fprintf(stderr,"status=%08X got=%u expected=%u\n",c.status,ctx.r3.u32,c.error);
        CHECK(ctx.r3.u32==c.error);
        CHECK(ctx.r4.u64==0xFEDCBA9876543210ull&&ctx.r5.u64==0xAABBCCDDEEFF0011ull);
        CHECK(memory==original);
    }
    // Native behavior also includes customer-defined and Win32-facility statuses;
    // these should not be reduced to arbitrary low bits or a catch-all success.
    for(uint32_t status:{0x00000102u,0x40000000u,0x80000006u,0xC0000010u,
                         0xC000003Au,0xC0000043u,0xC000009Du,0xC00000B5u,
                         0xD0000011u,0xE0001234u,0xC00703E5u,0x80071234u}) {
        PPCContext ctx;ctx.r3.u32=status;
        __imp__RtlNtStatusToDosError(ctx,memory.data());
        CHECK(ctx.r3.u32==native(static_cast<LONG>(status)));
    }
    std::printf("PASS %u status conversion checks: EOF38, all emitted I/O errors, native oracle, unknown status, ABI canaries\n",checks);
}
'''


def main():
    source = (ROOT / "port/src/runtime/kernel/kernel_sys.cpp").read_text(encoding="utf-8-sig")
    begin = source.index("PPC_FUNC(__imp__RtlNtStatusToDosError)")
    end = source.index("\n}", begin) + 2
    production = source[begin:end]
    with tempfile.TemporaryDirectory(prefix="ttt2-status-map-") as temp:
        folder = Path(temp)
        executable = compile_one(folder, "status_mapping", HARNESS.replace("@PRODUCTION@", production), [])
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout, end="")
        old = "PPC_FUNC(__imp__RtlNtStatusToDosError) { ctx.r3.u32 &= 0xFFFF; }"
        executable = compile_one(folder, "status_mapping_old", HARNESS.replace("@PRODUCTION@", old), [])
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
        if result.returncode == 0 or "got=17 expected=38" not in result.stderr:
            raise RuntimeError("Negative control failed to expose incorrect EOF mapping:\n" + result.stdout + result.stderr)
        print("PASS negative control: original low-bits stub misreports EOF as 17 instead of 38")


if __name__ == "__main__":
    main()
