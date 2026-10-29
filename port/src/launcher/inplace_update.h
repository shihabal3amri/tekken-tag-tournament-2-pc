#pragma once
#include <shobjidl.h>
#include <commctrl.h>
#include <prsht.h>
#include <dlgs.h>
#include <thread>
#include "clean_install_paths.h"
#include "update_version.h"

namespace inplace_update {
constexpr wchar_t kEntry[]=L"Tekken Tag Tournament 2.exe";
struct Options {fs::path install,recovery,userData;bool quiet=false;};
inline void Ini(const fs::path& path,const wchar_t* section,const wchar_t* key,const std::wstring& value) {
    Require(WritePrivateProfileStringW(section,key,value.c_str(),path.c_str())!=0,"Cannot write update configuration.");
}
inline bool Within(const fs::path& child,const fs::path& parent) {
    const auto a=fs::absolute(child).lexically_normal(),b=fs::absolute(parent).lexically_normal();
    auto i=a.begin();for(auto j=b.begin();j!=b.end();++j,++i)if(i==a.end()||_wcsicmp(i->c_str(),j->c_str()))return false;return true;
}
inline fs::path DefaultRecovery() {
    // A local developer delivery keeps recovery files with its project.
    // A downloaded release uses Windows' normal per-user application data.
    const auto beside=Self().parent_path();
    if(fs::is_directory(beside/L".git") || fs::is_directory(beside/L".development"))
        return beside/L".development"/L"game-update-recovery";
    return clean_install::UserData()/L"updates";
}
inline fs::path ChooseFolder(HWND owner) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    Require(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))),"Cannot open the folder browser.");
    DWORD options{};dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select the existing Tekken Tag Tournament 2 folder");
    const HRESULT result=dialog->Show(owner);
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED))return {};
    Require(SUCCEEDED(result),"Cannot choose the game folder.");
    Microsoft::WRL::ComPtr<IShellItem> item;PWSTR text=nullptr;
    Require(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&text)),"Cannot read the selected folder.");
    fs::path folder=text;CoTaskMemFree(text);return folder;
}
inline void Validate(const Options& options) {
    Require(fs::is_directory(options.install),"Select your existing game folder.");
    const auto root=fs::canonical(options.install);PlainDirectory(root);
    Require(fs::is_regular_file(root/kEntry),"This folder does not contain Tekken Tag Tournament 2.exe.");
    Require(fs::is_regular_file(root/L"game"/L"tekken.img") && Hash(root/L"game"/L"tekken.img",BCRYPT_SHA1_ALGORITHM)==kImageSha1,
            "This update requires the USA/Europe TU3 game installation.");
    Require(!fs::exists(root/L".ttt2-installed-update-v1")&&!fs::exists(root/L".ttt2-community"),
            "Select the clean original game installation, not an older experimental copy.");
    Require(!Within(options.recovery,root),"Recovery files must be stored outside the game folder.");
    PlainDirectory(options.recovery);PlainDirectory(options.userData);
}
struct Transaction {
    struct File {fs::path target,backup;bool existed;};
    fs::path backupRoot;std::vector<File> files;bool committed=false;
    explicit Transaction(const fs::path& state) {
        FILETIME time{};GetSystemTimeAsFileTime(&time);
        const auto stamp=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
        backupRoot=state/L"history"/std::to_wstring(stamp);fs::create_directories(backupRoot);
    }
    void Save(const fs::path& target) {
        for(const auto& f:files)if(f.target==target)return;
        PlainDirectory(target.parent_path());
        const DWORD attributes=GetFileAttributesW(target.c_str());
        Require(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"A target file is a link or directory. No replacement was made.");
        const bool existed=attributes!=INVALID_FILE_ATTRIBUTES;
        const auto backup=backupRoot/(std::to_wstring(files.size())+L".bin");
        if(existed)fs::copy_file(target,backup);
        const auto section=L"File "+std::to_wstring(files.size());
        Ini(backupRoot/L"rollback.ini",section.c_str(),L"Target",target.wstring());
        Ini(backupRoot/L"rollback.ini",section.c_str(),L"Backup",existed?backup.wstring():L"");
        files.push_back({target,backup,existed});
    }
    void Put(const fs::path& source,const fs::path& target) {Save(target);fs::create_directories(target.parent_path());ReplaceCopy(source,target);}
    ~Transaction(){if(committed)return;for(auto i=files.rbegin();i!=files.rend();++i)try {
        if(i->existed)ReplaceCopy(i->backup,i->target);else {std::error_code ec;fs::remove(i->target,ec);}
    }catch(...){std::fprintf(stderr,"Recovery incomplete; rollback.ini identifies the preserved originals.\n");}}
};
inline void Install(const fs::path& package,const Options& options) {
    Require(!GameRunning(),"Close the game and launcher before updating.");
    const auto expected=Payload(package);Validate(options);
    const auto root=fs::canonical(options.install);
    const auto id=clean_install::Id(root),section=L"Install "+id;
    const auto index=options.userData/L"updates.ini";
    const auto registered=clean_install::Read(index,section.c_str(),L"StateDirectory");
    const auto state=registered.empty()?fs::absolute(options.recovery)/id:fs::path(registered);
    Require(!Within(state,root),"Recovery files cannot be inside the game folder.");PlainDirectory(state);fs::create_directories(state);
    const auto info=state/L"install.ini",original=state/L"original"/kEntry;
    if(!registered.empty()) {
        Require(clean_install::Read(info,L"Update",L"Format")==L"clean-v2" &&
                fs::path(clean_install::Read(info,L"Update",L"InstalledGame"))==root && fs::is_regular_file(original),
                "Previous update metadata is incomplete. Keep its recovery files before retrying.");
    }else {
        Require(Hash(root/kEntry,BCRYPT_SHA256_ALGORITHM)!=expected.at(L"launcher.exe"),"The selected entry is already a launcher without its update record. Select the original installation.");
        fs::create_directories(original.parent_path());
        if(fs::exists(original))Require(Hash(original,BCRYPT_SHA256_ALGORITHM)==Hash(root/kEntry,BCRYPT_SHA256_ALGORITHM),"Existing original backup differs. Keep the recovery folder and resolve this conflict.");
        else fs::copy_file(root/kEntry,original);
        for(const auto* dll:{L"steam_api64.dll",L"msvcp140.dll",L"msvcp140_atomic_wait.dll",L"vcruntime140.dll",L"vcruntime140_1.dll",L"SDL3.dll"})
            if(fs::is_regular_file(root/dll))ReplaceCopy(root/dll,original.parent_path()/dll);
    }
    // Retain the small boot image beside the external original runtime so its
    // normal installed startup/logging still applies. Large assets stay in place.
    const auto originalImage=original.parent_path()/L"game"/L"tekken.img";
    if(!fs::is_regular_file(originalImage)) {
        fs::create_directories(originalImage.parent_path());fs::copy_file(root/L"game"/L"tekken.img",originalImage);
    }
    Require(Hash(originalImage,BCRYPT_SHA1_ALGORITHM)==kImageSha1,"Original runtime boot-image backup is invalid.");
    Transaction transaction(state);
    if(fs::is_directory(options.userData/L"saves"))CopySaveTree(options.userData/L"saves",transaction.backupRoot/L"saves-before");
    transaction.Put(package/L"bin"/L"game.exe",root/L"tekken_recomp.exe");
    transaction.Put(package/L"bin"/L"SDL3.dll",root/L"SDL3.dll");
    for(const auto& [name,target]:std::map<std::wstring,std::wstring>{{L"game.exe",L"tekken_recomp.exe"},{L"SDL3.dll",L"SDL3.dll"}})
        Require(Hash(root/target,BCRYPT_SHA256_ALGORITHM)==expected.at(name),"Installed payload checksum mismatch.");
    for(const auto* name:{L"README.txt",L"build-info.json",L"LICENSES/SMAA-LICENSE.txt",L"LICENSES/SMAA-README.md",L"LICENSES/SDL-LICENSE.txt"})
        transaction.Put(package/name,state/name);
    fs::create_directories(options.userData);transaction.Save(info);transaction.Save(index);
    Ini(info,L"Update",L"Format",L"clean-v2");Ini(info,L"Update",L"InstalledGame",root.wstring());
    Ini(info,L"Launcher",L"GameExecutable",(root/L"tekken_recomp.exe").wstring());
    Ini(info,L"Launcher",L"OriginalExecutable",original.wstring());
    Ini(index,section.c_str(),L"StateDirectory",state.wstring());
    // Keep the existing shortcut target and activate the native launcher last.
    transaction.Put(package/L"bin"/L"launcher.exe",root/kEntry);
    Require(Hash(root/kEntry,BCRYPT_SHA256_ALGORITHM)==expected.at(L"launcher.exe"),"Launcher checksum mismatch.");
    transaction.committed=true;
    std::ofstream(transaction.backupRoot/L"UPDATE-COMPLETE.txt")<<"Update complete. Original saves/settings retained. Rollback files and original online runtime are outside the installation.\n";
}
inline std::wstring Quote(const fs::path& path){return L"\""+path.wstring()+L"\"";}
inline void RunInstall(const Options& options) {
    UpdateLock lock;
    auto package=Self().parent_path();
#ifdef TTT2_EMBEDDED_PREVIEW
    EmbeddedPackage embedded;embedded.Prepare();package=embedded.dir;
#endif
    Install(package,options);
}
inline bool CanWrite(const fs::path& root) {
    const auto probe=root/(L".ttt2-access-"+std::to_wstring(GetCurrentProcessId()));
    HANDLE file=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);return true;}
    const auto error=GetLastError();Require(error==ERROR_ACCESS_DENIED,"The game folder is locked or unavailable.");return false;
}
inline void InstallWithElevation(const Options& options) {
    Validate(options);
    if(CanWrite(options.install)){RunInstall(options);return;}
    const auto args=L"--install-only --install-dir "+Quote(options.install)+L" --recovery-dir "+Quote(options.recovery)+L" --user-data-dir "+Quote(options.userData);
    const auto self=Self();SHELLEXECUTEINFOW shell{sizeof(shell)};shell.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    shell.lpVerb=L"runas";shell.lpFile=self.c_str();shell.lpParameters=args.c_str();shell.nShow=SW_HIDE;
    Require(ShellExecuteExW(&shell)!=FALSE,"Administrator permission was cancelled or Windows could not start the update.");
    Require(WaitForSingleObject(shell.hProcess,INFINITE)==WAIT_OBJECT_0,"Cannot wait for the update.");
    DWORD code=1;GetExitCodeProcess(shell.hProcess,&code);CloseHandle(shell.hProcess);
    Require(code==0,"The administrator update failed. Your originals are preserved in the recovery folder.");
}
#include "update_wizard.h"
inline int Main() {
    Options options;
    try {
        const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        Require(SUCCEEDED(com)||com==RPC_E_CHANGED_MODE,"Windows setup dialogs are unavailable.");
        struct Cleanup{bool active;~Cleanup(){if(active)CoUninitialize();}}cleanup{SUCCEEDED(com)};
        options.recovery=DefaultRecovery();options.userData=clean_install::UserData();
        int count{};auto argv=CommandLineToArgvW(GetCommandLineW(),&count);Require(argv!=nullptr,"Cannot read updater arguments.");
        struct Args{LPWSTR* p;~Args(){LocalFree(p);}}args{argv};
        for(int i=1;i<count;++i){const std::wstring arg=argv[i];
            if(arg==L"--install-only")options.quiet=true;
            else {Require(i+1<count,"Missing updater argument.");const fs::path value=argv[++i];
                if(arg==L"--install-dir")options.install=value;
                else if(arg==L"--recovery-dir")options.recovery=value;
                else if(arg==L"--user-data-dir")options.userData=value;
                else throw std::runtime_error("Unknown updater argument.");}}
        if(options.quiet){RunInstall(options);return 0;}
        return Wizard(options);
    }catch(const std::exception& error){
        if(!options.quiet)MessageBoxA(nullptr,error.what(),"Tekken Tag Tournament 2 Update",MB_OK|MB_ICONERROR);
        std::fprintf(stderr,"%s\n",error.what());return 1;
    }
}
}
