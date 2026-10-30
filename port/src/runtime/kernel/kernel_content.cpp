// Persistent local save containers and snapshot enumeration. Disc archives remain
// read-only; save_store owns their separate host directory and guest mounts.
// ABI references:
// https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_content.cc
// https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_enum.cc
// https://github.com/xenia-project/xenia/blob/master/src/xenia/xbox.h (XAM_OVERLAPPED)
#include "kernel_shared.h"
#include "kernel_trace.h"
#include "kernel_objects.h"
#include "kernel_content.h"
#include "save_store.h"
#include "profiles.h"
#include "xlive.h"
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {
constexpr uint32_t CONTENT_HANDLE_BEGIN = 0xD1000000u;
constexpr uint32_t CONTENT_HANDLE_END = 0xD2000000u;
constexpr uint32_t CONTENT_RECORD_BYTES = 308;
constexpr uint32_t XAM_SUCCESS = 0, XAM_ACCESS_DENIED = 5, XAM_INVALID_HANDLE = 6;
constexpr uint32_t XAM_NO_MORE_FILES = 18, XAM_NOT_SUPPORTED = 50;
constexpr uint32_t XAM_INVALID_PARAMETER = 87, XAM_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t XAM_IO_PENDING = 997, XAM_NO_SUCH_USER = 1317;
struct ContentEnumerator { uint32_t bufferBytes; std::vector<SaveContentRecord> records; size_t cursor=0; };
std::mutex g_contentMutex;
std::unordered_map<uint32_t, ContentEnumerator> g_contentEnumerators;
uint32_t g_nextContentHandle = CONTENT_HANDLE_BEGIN;

bool OptionalWord(uint32_t address) { return !address || XamGuestRange(address, 4); }
uint32_t HResult(uint32_t result) { return result ? 0x80070000u | result : 0; }
bool ContentRoot(uint8_t* base,uint32_t address,std::string& root)
{
    root.clear();if(!XamGuestRange(address,33,1))return false;
    for(unsigned i=0;i<33;++i){char c=char(GRead8(base,address+i));if(!c)return !root.empty();root+=c;}
    return false;
}
}

bool XamGuestRange(uint32_t address, uint64_t size, uint32_t alignment)
{
    return address && !(address & (alignment - 1)) &&
           uint64_t(address) + size <= 0x100000000ull;
}

uint32_t XamValidateOverlapped(uint8_t* base, uint32_t overlapped)
{
    if (!overlapped) return XAM_SUCCESS;
    if (!XamGuestRange(overlapped, 28)) return XAM_INVALID_PARAMETER;
    // A global callback worker is not the initiating thread's APC queue.
    if (GRead32(base, overlapped + 16)) return XAM_NOT_SUPPORTED;
    const uint32_t event = GRead32(base, overlapped + 12);
    if (event && !KernelIsEventHandle(event)) return XAM_INVALID_HANDLE;
    return XAM_SUCCESS;
}

uint32_t XamCompleteOverlapped(uint8_t* base, uint32_t overlapped,
                             uint32_t result, uint32_t length, uint32_t extendedError)
{
    if (!overlapped) return result;
    // Validation precedes all operations using this helper. Preserve the caller's
    // event, completion routine/context, and unused APC owner-context fields.
    GWrite32(base, overlapped + 4, length);
    GWrite32(base, overlapped + 24, extendedError);
    std::atomic_thread_fence(std::memory_order_release);
    GWrite32(base, overlapped, result);
    const uint32_t event = GRead32(base, overlapped + 12);
    if (event) KernelSignalEventHandle(event);
    // Completion may occur inline, but an overlapped request still returns pending.
    return XAM_IO_PENDING;
}

bool TryCloseContentEnumerator(uint32_t handle, uint32_t& status)
{
    if (handle < CONTENT_HANDLE_BEGIN || handle >= CONTENT_HANDLE_END) return false;
    std::lock_guard lock(g_contentMutex);
    status = g_contentEnumerators.erase(handle) ? STATUS_SUCCESS : 0xC0000008u;
    return true;
}

// r3=user, r4=device, r5=type, r6=flags, r7=count, r8=&bytes, r9=&handle.
void RollbackNoteIo(bool frameOnly, const char* op = "", const PPCContext* ctx = nullptr);   // rollback.cpp: no rewind across a save's I/O
void RollbackSaveOpen(bool open);   // rollback.cpp: a save container opened / closed (rewinds wait for the save)
int32_t RollbackSaveJournalFrame(); // rollback.cpp: the frame of a save a rewind may still re-simulate, else -1

PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    KTrace("XamContentCreateEnumerator");
    const uint32_t bytesOut = ctx.r8.u32, handleOut = ctx.r9.u32;
    if (!OptionalWord(bytesOut) || !XamGuestRange(handleOut, 4))
    { ctx.r3.u32 = XAM_INVALID_PARAMETER; return; }
    GWrite32(base, bytesOut, 0);
    GWrite32(base, handleOut, 0);
    if (ctx.r3.u32 != 0 && ctx.r3.u32 != 0xFE && ctx.r3.u32 != 0xFF)
    { ctx.r3.u32 = XAM_NO_SUCH_USER; return; }
    const uint64_t bytes = uint64_t(ctx.r7.u32) * CONTENT_RECORD_BYTES;
    if (ctx.r4.u32 > 1 || !ctx.r5.u32 || !ctx.r7.u32 || bytes > 0xFFFFFFFFu)
    { ctx.r3.u32 = XAM_INVALID_PARAMETER; return; }
    std::vector<SaveContentRecord> records;
    if(ctx.r5.u32==1)if(uint32_t e=SaveListContent(records)){
        std::fprintf(stderr,"[content] enumerate type=%u -> error %u\n",ctx.r5.u32,e);ctx.r3.u32=e;return;}
    std::fprintf(stderr,"[content] enumerate user=%u device=%u type=%u count=%u -> %zu records\n",
                 ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r7.u32,records.size());
    std::lock_guard lock(g_contentMutex);
    if (g_nextContentHandle == CONTENT_HANDLE_END)
    { ctx.r3.u32 = 8; return; } // ERROR_NOT_ENOUGH_MEMORY; never recycle stale handles.
    const uint32_t handle = g_nextContentHandle++;
    g_contentEnumerators.emplace(handle, ContentEnumerator{uint32_t(bytes),std::move(records)});
    GWrite32(base, bytesOut, uint32_t(bytes));
    GWrite32(base, handleOut, handle);
    ctx.r3.u32 = XAM_SUCCESS;
}

// r3=handle, r4=flags, r5=buffer, r6=bytes, r7=&count, r8=overlapped.
PPC_FUNC(__imp__XamEnumerate)
{
    KTrace("XamEnumerate");
    const uint32_t countOut = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (!OptionalWord(countOut) || ctx.r4.u32 || (countOut && overlapped))
    { ctx.r3.u32 = XAM_INVALID_PARAMETER; return; }
    if (const uint32_t error = XamValidateOverlapped(base, overlapped))
    { ctx.r3.u32 = error; return; }
    uint32_t result = XAM_NO_MORE_FILES;
    uint32_t count=0;
    if (XLiveEnumerate(base, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, count, result))
    {
        // A Live enumerator (xlive.cpp: friends, title servers, marketplace).
        GWrite32(base, countOut, count);
        ctx.r3.u32 = XamCompleteOverlapped(base, overlapped, result, count, HResult(result));
        return;
    }
    {
        std::lock_guard lock(g_contentMutex);
        const auto it = g_contentEnumerators.find(ctx.r3.u32);
        if (it == g_contentEnumerators.end()) result = XAM_INVALID_HANDLE;
        else if (!XamGuestRange(ctx.r5.u32, ctx.r6.u32, 1)) result = XAM_INVALID_PARAMETER;
        else if (ctx.r6.u32 < it->second.bufferBytes) result = XAM_INSUFFICIENT_BUFFER;
        else {
            auto& e=it->second;
            while(e.cursor<e.records.size() && count<e.bufferBytes/CONTENT_RECORD_BYTES) {
                std::memcpy(base+ctx.r5.u32+count*CONTENT_RECORD_BYTES,e.records[e.cursor++].data(),CONTENT_RECORD_BYTES);++count;
            }
            if(count)result=XAM_SUCCESS;
        }
    }
    // Never touch the caller's record buffer on exhaustion/error.
    GWrite32(base, countOut, count);
    ctx.r3.u32 = XamCompleteOverlapped(base, overlapped, result, count, HResult(result));
}

// The ninth argument (overlapped) follows the qword in r10 at caller SP+0x54.
PPC_FUNC(__imp__XamContentCreateEx)
{ RollbackSaveOpen(true); RollbackNoteIo(false, "XamContentCreateEx");
    KTrace("XamContentCreateEx");
    if (!XamGuestRange(ctx.r1.u32, 0x58) || !OptionalWord(ctx.r7.u32) ||
        !OptionalWord(ctx.r8.u32) || !XamGuestRange(ctx.r4.u32, 1, 1) ||
        !XamGuestRange(ctx.r5.u32, CONTENT_RECORD_BYTES))
    { ctx.r3.u32 = XAM_INVALID_PARAMETER; return; }
    const uint32_t overlapped = GRead32(base, ctx.r1.u32 + 0x54);
    if (const uint32_t error = XamValidateOverlapped(base, overlapped))
    { ctx.r3.u32 = error; return; }
    GWrite32(base, ctx.r7.u32, 0);
    GWrite32(base, ctx.r8.u32, 0);
    uint32_t result=0, disposition=0;std::string root;SaveContentRecord record;
    std::memcpy(record.data(),base+ctx.r5.u32,record.size());
    if(ctx.r3.u32!=0)result=XAM_NO_SUCH_USER;
    else if(!ContentRoot(base,ctx.r4.u32,root))result=XAM_INVALID_PARAMETER;
    else {
        // Online: a save in a frame a rewind may still re-simulate is copied first (save_store.cpp).
        if(const int32_t frame=RollbackSaveJournalFrame(); frame>=0) SaveJournalCapture(record,frame);
        result=SaveCreateContent(root,record,ctx.r6.u32&15,disposition);
    }
    {
        // Record: device id @4, content type @8, display name (UTF-16BE) @12, file name @264.
        char file[43]={};std::memcpy(file,record.data()+264,42);
        std::fprintf(stderr,"[content] create root=%s file=%s mode=%u flags=%08X -> result=%u disposition=%u\n",
                     root.c_str(),file,ctx.r6.u32&15,ctx.r6.u32,result,disposition);
    }
    GWrite32(base,ctx.r7.u32,disposition);
    if (result != 0) RollbackSaveOpen(false);                  // nothing opened
    ctx.r3.u32 = XamCompleteOverlapped(base, overlapped, result, disposition, HResult(result));
}

static void ContentCloseOrFlush(PPCContext& ctx,uint8_t* base,bool close)
{
    const uint32_t ov=ctx.r4.u32;std::string root;
    if(uint32_t e=XamValidateOverlapped(base,ov)){ctx.r3.u32=e;return;}
    uint32_t result=ContentRoot(base,ctx.r3.u32,root)?SaveCloseContent(root,close):XAM_INVALID_PARAMETER;
    std::fprintf(stderr,"[content] %s root=%s -> %u\n",close?"close":"flush",root.c_str(),result);
    ctx.r3.u32=XamCompleteOverlapped(base,ov,result,0,HResult(result));
}
PPC_FUNC(__imp__XamContentClose) { KTrace("XamContentClose");RollbackNoteIo(false, "XamContentClose");ContentCloseOrFlush(ctx,base,true);RollbackSaveOpen(false); }
PPC_FUNC(__imp__XamContentFlush) { KTrace("XamContentFlush");RollbackNoteIo(false, "XamContentFlush");ContentCloseOrFlush(ctx,base,false); }
PPC_FUNC(__imp__XamContentDelete)
{ RollbackNoteIo(false, "XamContentDelete");
    KTrace("XamContentDelete");const auto ov=ctx.r5.u32;
    if(uint32_t e=XamValidateOverlapped(base,ov)){ctx.r3.u32=e;return;}
    uint32_t result=XAM_INVALID_PARAMETER;
    if(ctx.r3.u32!=0)result=XAM_NO_SUCH_USER;
    else if(XamGuestRange(ctx.r4.u32,CONTENT_RECORD_BYTES)) {
        SaveContentRecord record;std::memcpy(record.data(),base+ctx.r4.u32,record.size());result=SaveDeleteContent(record);
    }
    std::fprintf(stderr,"[content] delete -> %u\n",result);
    ctx.r3.u32=XamCompleteOverlapped(base,ov,result,0,HResult(result));
}
PPC_FUNC(__imp__XamContentGetCreator)
{
    KTrace("XamContentGetCreator");const auto ov=ctx.r7.u32;
    if(!XamGuestRange(ctx.r5.u32,4)|| (ctx.r6.u32&&!XamGuestRange(ctx.r6.u32,8))) {ctx.r3.u32=XAM_INVALID_PARAMETER;return;}
    if(uint32_t e=XamValidateOverlapped(base,ov)){ctx.r3.u32=e;return;}
    GWrite32(base,ctx.r5.u32,0);if(ctx.r6.u32){GWrite32(base,ctx.r6.u32,0);GWrite32(base,ctx.r6.u32+4,0);}
    uint32_t result=XAM_INVALID_PARAMETER;uint64_t owner=0;
    if(ctx.r3.u32!=0)result=XAM_NO_SUCH_USER;
    else if(XamGuestRange(ctx.r4.u32,CONTENT_RECORD_BYTES)) {
        SaveContentRecord record;std::memcpy(record.data(),base+ctx.r4.u32,record.size());result=SaveContentCreator(record,owner);
    }
    if(!result){GWrite32(base,ctx.r5.u32,owner==profiles::ActiveXuid());if(ctx.r6.u32){GWrite32(base,ctx.r6.u32,uint32_t(owner>>32));GWrite32(base,ctx.r6.u32+4,uint32_t(owner));}}
    ctx.r3.u32=XamCompleteOverlapped(base,ov,result,0,HResult(result));
}
