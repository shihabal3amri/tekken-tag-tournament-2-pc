// Offline preview bootstrap: reuse the user's installed assets, isolate settings/saves,
// and retain the original executable. The public archive contains no game assets.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <map>
#include <string>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
namespace fs=std::filesystem;
namespace {
constexpr wchar_t kMarker[]=L".ttt2-community-preview-v1";
constexpr char kImageSha1[]="66adbc1a7744ec840a3ae8e51f8aeb677140d789";
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct UpdateLock {
    HANDLE handle{};
    UpdateLock(){
        handle=CreateMutexW(nullptr,FALSE,L"Local\\TTT2CommunityPreviewUpdate");
        Require(handle!=nullptr,"Cannot lock the preview updater.");
        const DWORD wait=WaitForSingleObject(handle,0);
        if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED){CloseHandle(handle);handle=nullptr;throw std::runtime_error("Another preview update is running. Wait for it to finish.");}
    }
    ~UpdateLock(){if(handle){ReleaseMutex(handle);CloseHandle(handle);}}
};
#ifdef TTT2_EMBEDDED_PREVIEW
// Extract the fixed embedded payloads into a newly created temporary
// directory. The existing updater performs its normal checksum validation.
struct EmbeddedPackage {
    fs::path dir;
    ~EmbeddedPackage() {
        if(dir.empty()) return;
        std::error_code ignored;
        fs::remove(dir/L"bin"/L"game.exe",ignored);
        fs::remove(dir/L"bin"/L"launcher.exe",ignored);
        fs::remove(dir/L"bin"/L"SDL3.dll",ignored);
        fs::remove(dir/L"LICENSES"/L"SDL-LICENSE.txt",ignored);
        fs::remove(dir/L"payload.sha256",ignored);
        fs::remove(dir/L"README.txt",ignored);
        fs::remove(dir/L"build-info.json",ignored);
        fs::remove(dir/L"LICENSES"/L"SMAA-LICENSE.txt",ignored);
        fs::remove(dir/L"LICENSES"/L"SMAA-README.md",ignored);
        fs::remove(dir/L"LICENSES",ignored);
        fs::remove(dir/L"bin",ignored);
        fs::remove(dir,ignored);
    }
    void Extract(int id,const fs::path& path) {
        HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
        Require(resource!=nullptr,"An embedded update file is missing.");
        HGLOBAL data=LoadResource(nullptr,resource);
        DWORD size=SizeofResource(nullptr,resource);
        const void* bytes=data?LockResource(data):nullptr;
        Require(bytes&&size,"An embedded update file is empty.");
        std::ofstream file(path,std::ios::binary|std::ios::trunc);
        file.write(static_cast<const char*>(bytes),size);
        Require(bool(file),"Cannot extract the update files.");
    }
    void Prepare() {
        wchar_t folder[MAX_PATH]{},name[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH,folder)>0,"Cannot locate the temporary folder.");
        Require(GetTempFileNameW(folder,L"tt2",0,name)!=0,"Cannot create an update staging folder.");
        Require(DeleteFileW(name)!=0,"Cannot create an update staging folder.");
        Require(CreateDirectoryW(name,nullptr)!=0,"Cannot create an update staging folder.");
        dir=name;
        fs::create_directory(dir/L"bin");
        Extract(101,dir/L"bin"/L"game.exe");
        Extract(102,dir/L"bin"/L"launcher.exe");
        Extract(103,dir/L"payload.sha256");
        fs::create_directory(dir/L"LICENSES");
        Extract(104,dir/L"README.txt");
        Extract(105,dir/L"build-info.json");
        Extract(106,dir/L"LICENSES"/L"SMAA-LICENSE.txt");
        Extract(107,dir/L"LICENSES"/L"SMAA-README.md");
        Extract(108,dir/L"bin"/L"SDL3.dll");
        Extract(109,dir/L"LICENSES"/L"SDL-LICENSE.txt");
    }
};
#endif
std::wstring Environment(const wchar_t* name){
    DWORD size=GetEnvironmentVariableW(name,nullptr,0);if(!size)return {};
    std::wstring value(size,L'\0');GetEnvironmentVariableW(name,value.data(),size);value.resize(size-1);return value;
}
fs::path Self(){std::wstring path(32768,L'\0');DWORD size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));Require(size&&size<path.size(),"Cannot find this update package.");path.resize(size);return path;}
std::string Hash(const fs::path& path,LPCWSTR algorithm){
    BCRYPT_ALG_HANDLE provider{};BCRYPT_HASH_HANDLE hash{};DWORD objectSize{},hashSize{},used{};
    Require(BCryptOpenAlgorithmProvider(&provider,algorithm,nullptr,0)>=0,"Hash provider unavailable.");
    struct CloseProvider{BCRYPT_ALG_HANDLE handle;~CloseProvider(){BCryptCloseAlgorithmProvider(handle,0);}}close{provider};
    Require(BCryptGetProperty(provider,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&used,0)>=0&&
            BCryptGetProperty(provider,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&hashSize),sizeof(hashSize),&used,0)>=0,"Cannot initialize hashing.");
    std::vector<UCHAR> object(objectSize),digest(hashSize);
    Require(BCryptCreateHash(provider,&hash,object.data(),objectSize,nullptr,0,0)>=0,"Cannot create hash.");
    struct CloseHash{BCRYPT_HASH_HANDLE handle;~CloseHash(){BCryptDestroyHash(handle);}}closeHash{hash};
    std::ifstream input(path,std::ios::binary);Require(bool(input),"Cannot read a required package or game file.");
    std::array<char,65536> buffer{};
    while(input){input.read(buffer.data(),buffer.size());if(input.gcount())Require(BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),ULONG(input.gcount()),0)>=0,"Cannot hash file.");}
    Require(input.eof(),"File read failed while checking the update.");
    Require(BCryptFinishHash(hash,digest.data(),hashSize,0)>=0,"Cannot complete hash.");
    std::string hex;for(UCHAR byte:digest){hex+="0123456789abcdef"[byte>>4];hex+="0123456789abcdef"[byte&15];}return hex;
}
bool GameRunning(){
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);Require(snapshot!=INVALID_HANDLE_VALUE,"Cannot check running games.");
    PROCESSENTRY32W process{sizeof(process)};bool running=false;
    if(Process32FirstW(snapshot,&process))do{
        if(!_wcsicmp(process.szExeFile,L"tekken_recomp.exe")||!_wcsicmp(process.szExeFile,L"Tekken Tag Tournament 2.exe")||!_wcsicmp(process.szExeFile,L"TTT2 Launcher.exe"))running=true;
    }while(Process32NextW(snapshot,&process));
    CloseHandle(snapshot);return running;
}
void PlainDirectory(const fs::path& path){
    const fs::path full=fs::absolute(path).lexically_normal();fs::path at=full.root_path();
    for(const auto& part:full.relative_path()){
        at/=part;DWORD attributes=GetFileAttributesW(at.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES)Require((attributes&FILE_ATTRIBUTE_REPARSE_POINT)==0&&(attributes&FILE_ATTRIBUTE_DIRECTORY),"Preview folders must be ordinary directories, not links or files.");
    }
}
void CopySaveTree(const fs::path& from,const fs::path& to){
    PlainDirectory(from);fs::create_directories(to);
    for(const auto& entry:fs::recursive_directory_iterator(from)){
        DWORD attributes=GetFileAttributesW(entry.path().c_str());Require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"A linked save entry cannot be imported safely.");
        const auto destination=to/entry.path().lexically_relative(from);
        if(entry.is_directory())fs::create_directories(destination);
        else if(entry.is_regular_file()){fs::create_directories(destination.parent_path());fs::copy_file(entry.path(),destination);}
        else throw std::runtime_error("Unsupported save entry.");
    }
}
std::map<std::wstring,std::string> Payload(const fs::path& package){
    std::ifstream input(package/L"payload.sha256");Require(bool(input),"Extract the entire update ZIP before opening it.");
    std::map<std::wstring,std::string> expected;std::string digest,name;
    while(input>>digest>>name){
        Require(digest.size()==64&&digest.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid package checksum file.");
        // Spaces are not used in payload names; the launcher is renamed on install.
        Require(name=="game.exe"||name=="launcher.exe"||name=="SDL3.dll","Unexpected payload filename.");
        Require(expected.emplace(std::wstring(name.begin(),name.end()),digest).second,"Duplicate payload entry.");
    }
    Require(input.eof()&&expected.size()==3,"Incomplete package checksum file.");
    for(const auto& [name,sum]:expected)Require(Hash(package/L"bin"/name,BCRYPT_SHA256_ALGORITHM)==sum,"Package checksum mismatch. Download and extract it again.");
    return expected;
}
fs::path ChooseInstalledGame(){
    wchar_t path[32768]{};OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.lpstrTitle=L"Choose your EXISTING Tekken Tag Tournament 2 game executable";
    dialog.lpstrFilter=L"Installed game executable (*.exe)\0*.exe\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog))return {};return fs::path(path);
}
void DefaultIni(const fs::path& path,const wchar_t* section,const wchar_t* key,const wchar_t* value){
    wchar_t current[128]{};GetPrivateProfileStringW(section,key,L"",current,128,path.c_str());
    if(!*current)Require(WritePrivateProfileStringW(section,key,value,path.c_str()),"Cannot write preview settings.");
}
void ReplaceCopy(const fs::path& source,const fs::path& target){
    // Replace a directory entry rather than truncating an existing file. A
    // leftover hard link must never turn a preview update into an original edit.
    fs::path temporary=target.wstring()+L".ttt2-incoming-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    Require(CopyFileW(source.c_str(),temporary.c_str(),TRUE)!=0,"Cannot stage a preview update file.");
    struct RemoveTemporary{fs::path path;~RemoveTemporary(){std::error_code ignored;fs::remove(path,ignored);}}cleanup{temporary};
    Require(MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot replace a preview file. Close the game and launcher, then run the updater again.");
}
void DesktopShortcut(const fs::path& state,fs::path desktop) {
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Require(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE,"Update installed, but Windows shortcut setup is unavailable.");
    struct Uninitialize{bool owns;~Uninitialize(){if(owns)CoUninitialize();}}com{SUCCEEDED(initialized)};
    if(desktop.empty()) {
        PWSTR known=nullptr;
        Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,KF_FLAG_DEFAULT,nullptr,&known)),"Update installed, but the desktop folder was not found.");
        desktop=known;CoTaskMemFree(known);
    }
    desktop=fs::absolute(desktop).lexically_normal();
    // The Windows known folder can be redirected to OneDrive or another
    // user-selected location. Only our named shortcut is written there.
    fs::create_directories(desktop);
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link))),"Update installed, but Windows could not create its desktop shortcut.");
    const auto app=state/L"app";
    Require(SUCCEEDED(link->SetPath((app/L"TTT2 Launcher.exe").c_str())) &&
            SUCCEEDED(link->SetWorkingDirectory(app.c_str())) &&
            SUCCEEDED(link->SetDescription(L"Open the installed TTT2 offline launcher")) &&
            SUCCEEDED(link->SetIconLocation((app/L"tekken_recomp.exe").c_str(),0)),"Cannot configure the offline launcher shortcut.");
    Microsoft::WRL::ComPtr<IPersistFile> file;
    Require(SUCCEEDED(link.As(&file)),"Cannot save the offline launcher shortcut.");
    const auto destination=desktop/L"TTT2 Offline Launcher.lnk";
    const auto temporary=desktop/(L"TTT2 Offline Launcher.incoming-"+std::to_wstring(GetCurrentProcessId())+L".lnk");
    Require(!fs::exists(temporary),"An interrupted shortcut update exists. Keep it and try again after closing other updaters.");
    Require(SUCCEEDED(file->Save(temporary.c_str(),TRUE)),"Update installed, but its desktop shortcut could not be saved.");
    struct Cleanup{fs::path path;~Cleanup(){std::error_code ignored;fs::remove(path,ignored);}}cleanup{temporary};
    Require(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Update installed, but its desktop shortcut could not be replaced.");
}
void Prepare(const fs::path& package,const fs::path& install,const fs::path& state,const fs::path& sourceSettings,const fs::path& sourceSaves){
    Require(!GameRunning(),"Close all running copies of the game and launcher before preparing the preview.");
    const auto payload=Payload(package);Require(fs::is_regular_file(install/L"game"/L"tekken.img"),"Choose the original installation containing game/tekken.img.");
    Require(Hash(install/L"game"/L"tekken.img",BCRYPT_SHA1_ALGORITHM)==kImageSha1,"This preview requires the USA/Europe Title Update 3 installation. The game image does not match.");
    PlainDirectory(state);
    if(fs::exists(state)&&!fs::is_empty(state))Require(fs::is_regular_file(state/kMarker),"The preview destination contains unrelated files. Choose an empty folder.");
    fs::create_directories(state);std::ofstream(state/kMarker)<<"TTT2 community offline preview v1\n";
    const fs::path app=state/L"app",saves=state/L"saves",settings=state/L"settings.ini";
    PlainDirectory(app/L"game");PlainDirectory(saves);
    for(const auto& path:{settings,state/L"preview.ini",state/L"launcher.ini"}){
        DWORD attributes=GetFileAttributesW(path.c_str());
        Require(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Preview configuration must not contain linked files.");
    }
    fs::create_directories(app/L"game");
    if(!fs::exists(saves)){
        const fs::path staging=state/L"save-import.incomplete";
        Require(!fs::exists(staging),"A previous save import was interrupted. Keep that folder and use a new preview destination.");
        if(fs::is_directory(sourceSaves))CopySaveTree(sourceSaves,staging);else fs::create_directory(staging);
        fs::rename(staging,saves);
    }
    if(!fs::exists(settings)){
        if(fs::is_regular_file(sourceSettings))fs::copy_file(sourceSettings,settings);
        else std::ofstream(settings)<<"[Display]\n";
    }
    DefaultIni(settings,L"Keyboard",L"SeparatePlayer",L"1");DefaultIni(settings,L"Mods",L"Overlay",L"1");
    DefaultIni(settings,L"Mods",L"InfiniteHealth",L"0");DefaultIni(settings,L"Mods",L"InfiniteMoney",L"0");DefaultIni(settings,L"Mods",L"UnlockContent",L"0");
    ReplaceCopy(package/L"bin"/L"game.exe",app/L"tekken_recomp.exe");
    ReplaceCopy(package/L"bin"/L"launcher.exe",app/L"TTT2 Launcher.exe");
    Require(Hash(app/L"tekken_recomp.exe",BCRYPT_SHA256_ALGORITHM)==payload.at(L"game.exe") &&
            Hash(app/L"TTT2 Launcher.exe",BCRYPT_SHA256_ALGORITHM)==payload.at(L"launcher.exe"),"Installed preview checksum mismatch. Close the game and run the updater again.");
    ReplaceCopy(package/L"bin"/L"SDL3.dll",app/L"SDL3.dll");
    Require(Hash(app/L"SDL3.dll",BCRYPT_SHA256_ALGORITHM)==payload.at(L"SDL3.dll"),"Installed SDL runtime checksum mismatch.");
    // Standalone and ZIP updates retain the same provenance and notices locally.
    PlainDirectory(app/L"LICENSES");fs::create_directories(app/L"LICENSES");
    for(const wchar_t* name:{L"README.txt",L"build-info.json",L"LICENSES/SMAA-LICENSE.txt",L"LICENSES/SMAA-README.md",L"LICENSES/SDL-LICENSE.txt"}) {
        const auto target=app/name;
        const DWORD attrs=GetFileAttributesW(target.c_str());
        Require(attrs==INVALID_FILE_ATTRIBUTES||!(attrs&FILE_ATTRIBUTE_REPARSE_POINT),"Preview documentation must not contain linked files.");
        ReplaceCopy(package/name,target);
    }
    // Only the small local boot image is copied. All assets remain in the user's
    // installation and are read through TTT2_VFS_ROOT. No link/admin rights needed.
    ReplaceCopy(install/L"game"/L"tekken.img",app/L"game"/L"tekken.img");
    for(const wchar_t* name:{L"msvcp140.dll",L"msvcp140_atomic_wait.dll",L"vcruntime140.dll",L"vcruntime140_1.dll"}){
        if(fs::is_regular_file(install/name))ReplaceCopy(install/name,app/name);
    }
    Require(WritePrivateProfileStringW(L"Launcher",L"GameExecutable",(app/L"tekken_recomp.exe").c_str(),(state/L"launcher.ini").c_str()),"Cannot save the preview game location.");
    Require(WritePrivateProfileStringW(L"Preview",L"InstalledGame",install.c_str(),(state/L"preview.ini").c_str()),"Cannot save the installed game location.");
}
}
#ifdef TTT2_INPLACE_UPDATE
#include "inplace_update.h"
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){return inplace_update::Main();}
#else
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    bool prepareOnly=false,launchOriginal=false;
    try{
        fs::path package=Self().parent_path();
#ifdef TTT2_EMBEDDED_PREVIEW
        EmbeddedPackage embedded;
#endif
        const auto local=Environment(L"LOCALAPPDATA");Require(!local.empty(),"LOCALAPPDATA is unavailable.");
        fs::path install,originalExe,shortcutDir,state=fs::path(local)/L"TTT2RecompCommunityPreview"/L"v0.1.7-community.1";
        fs::path sourceSettings=fs::path(local)/L"TTT2Recomp"/L"settings.ini",sourceSaves=fs::path(local)/L"TTT2Recomp"/L"saves";
        int count{};LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&count);Require(argv!=nullptr,"Cannot read command line.");
        for(int i=1;i<count;++i){std::wstring arg=argv[i];
            if(arg==L"--prepare-only")prepareOnly=true;
            else if(arg==L"--launch-original")launchOriginal=true;
            else {Require(i+1<count,"Missing command-line value.");const fs::path value=argv[++i];
                if(arg==L"--install-dir")install=value;else if(arg==L"--state-dir")state=value;
                else if(arg==L"--original-exe")originalExe=value;
                else if(arg==L"--shortcut-dir")shortcutDir=value;
                else if(arg==L"--source-settings")sourceSettings=value;else if(arg==L"--source-saves")sourceSaves=value;
                else throw std::runtime_error("Unknown command-line option.");}
        }LocalFree(argv);
        state=fs::absolute(state).lexically_normal();
        if(install.empty()){
            wchar_t saved[32768]{};GetPrivateProfileStringW(L"Preview",L"InstalledGame",L"",saved,32768,(state/L"preview.ini").c_str());
            if(*saved)install=saved;else { originalExe=ChooseInstalledGame();if(!originalExe.empty())install=originalExe.parent_path(); }
            if(install.empty())return 0;
        }
        install=fs::canonical(install);
        if(originalExe.empty()) {
            wchar_t saved[32768]{};GetPrivateProfileStringW(L"Preview",L"OriginalExecutable",L"",saved,32768,(state/L"preview.ini").c_str());
            if(*saved)originalExe=saved;
            else if(fs::is_regular_file(install/L"Tekken Tag Tournament 2.exe"))originalExe=install/L"Tekken Tag Tournament 2.exe";
            else if(fs::is_regular_file(install/L"tekken_recomp.exe"))originalExe=install/L"tekken_recomp.exe";
        }
        Require(!(prepareOnly&&launchOriginal),"Choose either preparation or original game launch.");
        if(launchOriginal) {
                if((originalExe.empty()||!fs::is_regular_file(originalExe))&&!launchOriginal)originalExe=ChooseInstalledGame();
                Require(!originalExe.empty()&&fs::is_regular_file(originalExe),"The original game executable was not found. Select it again from your game installation.");
                originalExe=fs::canonical(originalExe);
                Require(originalExe.parent_path()==install,"The original game executable must be in the selected installation folder.");
                for(const wchar_t* name:{L"TTT2_SETTINGS_INI",L"TTT2_SAVE_ROOT",L"TTT2_VFS_ROOT",L"TTT2_NET",L"TTT2_INPUT_SCRIPT",L"TTT2_AUTOSTART"}) SetEnvironmentVariableW(name,nullptr);
                std::wstring command=L"\""+originalExe.wstring()+L"\"";
                STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
                Require(CreateProcessW(originalExe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,install.c_str(),&startup,&process),"Cannot start the original game.");
                CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
        }
        UpdateLock updateLock;
#ifdef TTT2_EMBEDDED_PREVIEW
        embedded.Prepare();package=embedded.dir;
#endif
        Prepare(package,install,state,sourceSettings,sourceSaves);
        if(!originalExe.empty() && fs::is_regular_file(originalExe)) {
            originalExe=fs::canonical(originalExe);
            Require(originalExe.parent_path()==install,"The original game executable must be in the selected installation folder.");
            Require(WritePrivateProfileStringW(L"Preview",L"OriginalExecutable",originalExe.c_str(),(state/L"preview.ini").c_str()),"Cannot save the original game location.");
        }
        if(!prepareOnly || !shortcutDir.empty())DesktopShortcut(state,shortcutDir);
        if(prepareOnly)return 0;
        Require(SetEnvironmentVariableW(L"TTT2_SETTINGS_INI",(state/L"settings.ini").c_str())&&
                SetEnvironmentVariableW(L"TTT2_SAVE_ROOT",(state/L"saves").c_str())&&
                SetEnvironmentVariableW(L"TTT2_VFS_ROOT",(install/L"game").c_str()),"Cannot configure the preview environment.");
        SetEnvironmentVariableW(L"TTT2_NET",L"loopback");SetEnvironmentVariableW(L"TTT2_INPUT_SCRIPT",nullptr);SetEnvironmentVariableW(L"TTT2_AUTOSTART",nullptr);
        const fs::path launcher=state/L"app"/L"TTT2 Launcher.exe";std::wstring command=L"\""+launcher.wstring()+L"\"";
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        Require(CreateProcessW(launcher.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,launcher.parent_path().c_str(),&startup,&process),"Cannot start the preview launcher.");
        CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
    }catch(const std::exception& error){
        if(!prepareOnly)MessageBoxA(nullptr,error.what(),"TTT2 Offline Preview Update",MB_OK|MB_ICONERROR);
        std::fprintf(stderr,"%s\n",error.what());
        OutputDebugStringA(error.what());return 1;
    }
}
#endif
