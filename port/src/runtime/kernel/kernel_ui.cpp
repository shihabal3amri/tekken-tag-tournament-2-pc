// XamShowMessageBoxUI: the guest owns the result and XOVERLAPPED until completion, just as on
// Xbox. The box is shown in the game's own dialog window by the system UI (sys_ui.h, menu_ext.cpp)
// - never a Win32 pop-up; all strings are copied before it is queued, and the answer completes the
// overlapped from the game's main thread without blocking it.
#include "kernel_shared.h"
#include "kernel_content.h"
#include "kernel_trace.h"
#include "sys_ui.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void XamNotifyEnqueueEvent(uint32_t id, uint32_t param);

namespace {
constexpr uint32_t kPending = 997, kInvalidParameter = 87;

bool ReadGuestBytes(uint8_t* base, uint32_t address, void* out, size_t size,
                    uint32_t alignment = 1) {
    if (!XamGuestRange(address, size, alignment)) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), base + address, out, size, &read)
        && read == size;
}
bool ReadGuestWord(uint8_t* base, uint32_t address, uint32_t& word) {
    if (!ReadGuestBytes(base, address, &word, sizeof(word), 4)) return false;
    word = __builtin_bswap32(word);
    return true;
}
bool WritableGuestRange(uint8_t* base, uint32_t address, size_t size) {
    if (!XamGuestRange(address, size, 4)) return false;
    auto* at = base + address;
    const auto* end = at + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(at, &region, sizeof(region)) || region.State != MEM_COMMIT
            || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        switch (region.Protect & 0xFF) {
        case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
        default: return false;
        }
        auto* next = static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    return true;
}
bool CopyGuestText(uint8_t* base, uint32_t address, size_t limit,
                   bool optional, std::wstring& text) {
    text.clear();
    if (!address) return optional;
    for (size_t i = 0; i < limit; ++i) {
        const uint64_t at = uint64_t(address) + i * 2;
        uint16_t unit;
        if (at > UINT32_MAX || !ReadGuestBytes(base, uint32_t(at), &unit, 2, 2)) return false;
        unit = __builtin_bswap16(unit);
        if (!unit) return true;
        text.push_back(static_cast<wchar_t>(unit));
    }
    return false; // Reject unterminated / unbounded guest strings.
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

struct Completion { uint8_t* base; uint32_t result, overlapped; };
void Finish(const Completion& c, int choice) {
    GWrite32(c.base, c.result, uint32_t(choice));
    XamCompleteOverlapped(c.base, c.overlapped, 0, 0, 0);
    // Match XAM's UI-close ordering: polling may observe the completed request before the
    // UI-hidden notification.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    XamNotifyEnqueueEvent(9, 0);
}
} // namespace

// ABI reference: xenia/kernel/xam/xam_ui.cc XamShowMessageBoxUI_entry.
// Argument nine is at SP+0x54, after the eight register arguments.
PPC_FUNC(__imp__XamShowMessageBoxUI) {
    KTrace("XamShowMessageBoxUI");
    const uint32_t count = ctx.r6.u32, buttonArray = ctx.r7.u32;
    const uint32_t active = ctx.r8.u32, result = ctx.r10.u32;
    uint32_t overlapped;
    if (uint64_t(ctx.r1.u32) + 0x54 > UINT32_MAX
        || !ReadGuestWord(base, ctx.r1.u32 + 0x54, overlapped)) {
        ctx.r3.u32 = kInvalidParameter; return;
    }
    // The renderer must keep servicing frames while a player is choosing. The title uses the
    // asynchronous API; synchronous calls are explicitly unsupported.
    if (!overlapped) { ctx.r3.u32 = ERROR_NOT_SUPPORTED; return; }
    if (count < 1 || count > 4 || active >= count
        || !XamGuestRange(buttonArray, uint64_t(count) * 4, 4)
        || !WritableGuestRange(base, result, 4)
        || !WritableGuestRange(base, overlapped, 28)
        || (uint64_t(result) < uint64_t(overlapped) + 28 && uint64_t(overlapped) < uint64_t(result) + 4)) {
        ctx.r3.u32 = kInvalidParameter; return;
    }
    if (const uint32_t error = XamValidateOverlapped(base, overlapped)) {
        ctx.r3.u32 = error; return;
    }
    try {
        std::wstring title, text;
        if (!CopyGuestText(base, ctx.r4.u32, 256, true, title)
            || !CopyGuestText(base, ctx.r5.u32, 8192, true, text)) {
            ctx.r3.u32 = kInvalidParameter; return;
        }
        SysUiMessageBox box;
        box.text = Utf8(text);
        box.active = int(active);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t pointer;
            std::wstring label;
            if (!ReadGuestWord(base, buttonArray + i * 4, pointer)
                || !CopyGuestText(base, pointer, 256, false, label) || label.empty()) {
                ctx.r3.u32 = kInvalidParameter; return;
            }
            if (_wcsicmp(label.c_str(), L"Cancel") == 0) box.cancel = int(i);
            box.buttons.push_back(Utf8(label));
        }
        std::fprintf(stderr, "[xam] XamShowMessageBoxUI '%s' (%u buttons)\n", Utf8(title).c_str(), count);
        const Completion completion{ base, result, overlapped };
        box.done = [completion](int choice) {
            std::thread([completion, choice] { Finish(completion, choice); }).detach();
        };
        // Pending before the box is queued (the answer may complete it from the main thread at
        // once); a refused box restores the caller's words.
        const uint32_t saved[3] = { GRead32(base, overlapped), GRead32(base, overlapped + 4),
                                    GRead32(base, overlapped + 24) };
        GWrite32(base, overlapped + 4, 0);
        GWrite32(base, overlapped + 24, 0);
        std::atomic_thread_fence(std::memory_order_release);
        GWrite32(base, overlapped, kPending);
        if (!SysUiShowMessageBox(std::move(box))) {
            GWrite32(base, overlapped, saved[0]);
            GWrite32(base, overlapped + 4, saved[1]);
            GWrite32(base, overlapped + 24, saved[2]);
            ctx.r3.u32 = ERROR_ACCESS_DENIED; return;
        }
        XamNotifyEnqueueEvent(9, 1);
        ctx.r3.u32 = kPending;
    } catch (...) {
        ctx.r3.u32 = ERROR_NOT_ENOUGH_MEMORY;
    }
}
