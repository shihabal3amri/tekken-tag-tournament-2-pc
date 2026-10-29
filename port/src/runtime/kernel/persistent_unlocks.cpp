#ifdef _WIN32
// One-shot grants through TU3's original bulk-acquisition and profile-save
// routines. No raw save-file or ownership-bit writes. Original online installs
// are rejected: this action requires the updater's copied, marked preview root.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "persistent_unlocks.h"
#include "kernel_shared.h"
#include "offline_cheats.h"
#include "sys_ui.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <initializer_list>

extern "C" PPC_FUNC(__imp__sub_822547D0);
extern "C" PPC_FUNC(__imp__sub_82588238);
extern "C" PPC_FUNC(__imp__sub_82718818);
extern "C" PPC_FUNC(__imp__sub_82718A40);
extern "C" PPC_FUNC(__imp__sub_82276D38);
extern "C" PPC_FUNC(__imp__sub_8272EEB0);
extern "C" PPC_FUNC(__imp__sub_8272F260);
extern "C" PPC_FUNC(__imp__sub_8272F738);
extern "C" PPC_FUNC(__imp__sub_82411F08);

namespace persistent_unlocks {
namespace {
namespace fs=std::filesystem;
std::atomic<bool> pending{false};
constexpr uint32_t kManagerPointer=0x82E6B6FC;
void Require(bool condition,const char* text) { if(!condition)throw std::runtime_error(text); }
bool Span(uint32_t address,uint32_t size) {
    return address>=0x10000u && size<=0xFFFFFFFFu-address && (address&3u)==0;
}
std::wstring Environment(const wchar_t* name) {
    const DWORD size=GetEnvironmentVariableW(name,nullptr,0);
    if(!size)return {};
    std::wstring value(size,L'\0');
    const DWORD copied=GetEnvironmentVariableW(name,value.data(),size);
    Require(copied && copied<size,"Preview environment changed. Restart the offline updater.");
    value.resize(copied);return value;
}
void OrdinaryPath(const fs::path& path,bool directory) {
    const auto full=fs::absolute(path).lexically_normal();
    auto current=full.root_path();
    for(const auto& component:full.relative_path()) {
        current/=component;
        const DWORD attr=GetFileAttributesW(current.c_str());
        Require(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_REPARSE_POINT),
                "Unlock backup requires ordinary existing preview paths, without links.");
        if(current!=full || directory)Require((attr&FILE_ATTRIBUTE_DIRECTORY)!=0,"Invalid preview directory.");
        else Require(!(attr&FILE_ATTRIBUTE_DIRECTORY),"Invalid preview marker or configuration.");
    }
}
fs::path PreviewSaveRoot() {
    const auto value=Environment(L"TTT2_SAVE_ROOT");
    Require(!value.empty(),"Use the offline updater first. Permanent grants require copied preview saves.");
    const auto saves=fs::absolute(value).lexically_normal();
    Require(saves.filename()==L"saves","Permanent grants require the updater's copied preview saves.");
    OrdinaryPath(saves,true);
    const auto state=saves.parent_path();
    const auto marker=state/L".ttt2-community-preview-v1";
    OrdinaryPath(marker,false);
    std::ifstream stream(marker);
    std::string text;std::getline(stream,text);
    Require(text=="TTT2 community offline preview v1","Unrecognized preview marker.");
    const auto settings=Environment(L"TTT2_SETTINGS_INI");
    Require(!settings.empty(),"Preview settings are not isolated. Reopen the offline updater.");
    const auto ini=fs::absolute(settings).lexically_normal();
    OrdinaryPath(ini,false);
    Require(ini==state/L"settings.ini","Preview settings and saves must share the updater's state folder.");
    // Even a mistakenly copied marker must never authorize the ordinary save root.
    const auto local=Environment(L"LOCALAPPDATA");
    if(!local.empty())Require(fs::weakly_canonical(saves)!=fs::weakly_canonical(fs::path(local)/L"TTT2Recomp"/L"saves"),
                             "The original save folder cannot receive permanent preview grants.");
    return saves;
}
fs::path Backup(const fs::path& saves) {
    const auto backups=saves.parent_path()/L"backups";
    if(!fs::exists(backups))fs::create_directory(backups);
    OrdinaryPath(backups,true);
    FILETIME now{};GetSystemTimeAsFileTime(&now);
    const auto stamp=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
    const auto folder=backups/(L"before-permanent-unlocks-"+std::to_wstring(stamp));
    Require(fs::create_directory(folder),"Cannot create a unique unlock backup.");
    const auto copy=folder/L"saves";fs::create_directory(copy);
    for(const auto& entry:fs::recursive_directory_iterator(saves)) {
        const auto target=copy/entry.path().lexically_relative(saves);
        const DWORD attr=GetFileAttributesW(entry.path().c_str());
        Require(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_REPARSE_POINT),
                "A linked preview save cannot be backed up safely. No grants were made.");
        if(entry.is_directory())fs::create_directory(target);
        else {
            Require(entry.is_regular_file(),"Unsupported preview save entry. No grants were made.");
            fs::copy_file(entry.path(),target);
        }
    }
    std::ofstream complete(folder/L"BACKUP-COMPLETE.txt");
    complete<<"Copied preview saves before the one-shot mapped customization grant.\n";
    complete.close();Require(bool(complete),"Cannot complete the preview backup. No grants were made.");
    return folder;
}
struct GuestFrame {
    PPCContext& ctx;PPCContext original;
    explicit GuestFrame(PPCContext& context,uint8_t* base):ctx(context),original(context) {
        Require(Span(ctx.r1.u32,0x100) && ctx.r1.u32>=0x10400u,"Invalid guest stack; no grants were made.");
        ctx.r1.u32-=0x400;
        GWrite32(base,ctx.r1.u32,original.r1.u32);
    }
    ~GuestFrame(){ctx=original;}
};
void ValidateCustomization(uint8_t* base,uint32_t object) {
    Require(Span(object,6844),"No loaded customization record; no grants were made.");
    Require(GRead32(base,object+6680)==6 && GRead32(base,object+6696)==6,
            "Unsupported customization schema; no grants were made.");
    // These are the TU3 constructor's header sizes, not save-file offsets.
    const struct {uint32_t offset,count,lastBits,bytes;} headers[]={{8,625,32,2500},{2524,625,32,2500},
        {5040,4,4,16},{5072,4,4,16},{5104,32,16,128},{5248,32,16,128}};
    for(const auto& h:headers)
        Require(GRead32(base,object+h.offset)==h.count && GRead32(base,object+h.offset+4)==h.lastBits &&
                GRead32(base,object+h.offset+12)==h.bytes,
                "Unexpected customization bitset bounds; no grants were made.");
}
std::string Apply(PPCContext& ctx,uint8_t* base) {
    Require(offline_cheats::Allowed(),"Permanent grants are available only in the offline preview.");
    const auto saves=PreviewSaveRoot();
    GuestFrame frame(ctx,base);
    const uint32_t manager=GRead32(base,kManagerPointer);
    Require(Span(manager,0x2900),"The preview profile has not loaded; no grants were made.");
    ctx.r3.u64=manager;__imp__sub_82718818(ctx,base);
    Require((ctx.r3.u32&0xFF)!=0,"The preview profile has not loaded; no grants were made.");
    Require(GRead32(base,manager+20)==0 && GRead32(base,manager+28)==0,
            "The profile save is busy. Wait for saving to finish and try again.");
    ctx.r3.u64=manager;__imp__sub_82718A40(ctx,base);
    const uint32_t object=ctx.r3.u32;ValidateCustomization(base,object);
    __imp__sub_822547D0(ctx,base);__imp__sub_82588238(ctx,base);
    Require((ctx.r3.u32&0xFF)==0,"The game is saving. Wait for saving to finish and try again.");
    __imp__sub_82276D38(ctx,base);const uint32_t profile=ctx.r3.u32;
    Require(Span(profile,407),"No loaded release profile; no grants were made.");
    const auto backup=Backup(saves);
    // Recheck the live session gate immediately before the owning routines run.
    Require(offline_cheats::Allowed(),"The session changed. No grants were made.");
    for(auto grant:{__imp__sub_8272EEB0,__imp__sub_8272F260,__imp__sub_8272F738}) {
        ctx.r3.u64=object;ctx.r4.u64=0;grant(ctx,base);
    }
    // Save the normal full progression/customization record AND acquisition
    // deltas through the guest scheduler and existing host container store.
    ctx.r3.u64=profile;__imp__sub_82411F08(ctx,base);
    std::fprintf(stderr,"[unlocks] mapped item/panel/customization grants submitted through original profile save; backup %ls\n",backup.c_str());
    return "Mapped item, panel and customization groups granted.\nSave requested through the normal game routine.\nRestart with temporary unlocks OFF to check persistence.\nBackup: "+backup.filename().string();
}
}
bool QueuePermanentUnlocks() {bool idle=false;return pending.compare_exchange_strong(idle,true);}
void ProcessPending(PPCContext& ctx,uint8_t* base,bool allowMenu) {
    if(!pending.load() || SysUiBoxBusy())return;
    if(!pending.exchange(false))return;
    std::string result;
    try {
        Require(allowMenu,"Open the main/options menu before requesting permanent grants.");
        result=Apply(ctx,base);
    } catch(const std::exception& error) {
        result=std::string("Permanent grants stopped.\n")+error.what();
        std::fprintf(stderr,"[unlocks] stopped: %s\n",error.what());
    }
    SysUiMessageBox box;box.text=std::move(result);box.buttons={"OK"};box.cancel=0;
    SysUiShowMessageBox(std::move(box));
}
}

#else
#include "persistent_unlocks.h"
namespace persistent_unlocks {
bool QueuePermanentUnlocks(){return false;}
void ProcessPending(PPCContext&,uint8_t*,bool){}
}
#endif
