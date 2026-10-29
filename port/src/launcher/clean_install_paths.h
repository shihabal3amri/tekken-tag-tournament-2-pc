#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <cwctype>
#include <stdexcept>
namespace clean_install {
namespace fs=std::filesystem;
inline std::wstring Id(const fs::path& root) {
    unsigned long long hash=14695981039346656037ull;
    for(wchar_t c:fs::absolute(root).lexically_normal().wstring()) {
        c=std::towlower(c);hash^=static_cast<unsigned short>(c);hash*=1099511628211ull;
    }
    wchar_t text[17]{};swprintf(text,17,L"%016llx",hash);return text;
}
inline fs::path UserData() {
    wchar_t local[32768]{};const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if(!n||n>=32768)throw std::runtime_error("Windows user data folder is unavailable.");
    return fs::path(local)/L"TTT2Recomp";
}
inline std::wstring Read(const fs::path& file,const wchar_t* section,const wchar_t* key) {
    wchar_t value[32768]{};GetPrivateProfileStringW(section,key,L"",value,32768,file.c_str());return value;
}
inline fs::path State(const fs::path& root) {
    const auto index=UserData()/L"updates.ini";
    const auto section=L"Install "+Id(root);
    return Read(index,section.c_str(),L"StateDirectory");
}
inline bool Configure(const fs::path& executable) {
    const auto root=fs::absolute(executable).lexically_normal().parent_path();
    const auto state=State(root);if(state.empty())return false;
    const auto info=state/L"install.ini";
    if(Read(info,L"Update",L"Format")!=L"clean-v2" ||
       fs::path(Read(info,L"Update",L"InstalledGame"))!=root ||
       !fs::is_regular_file(root/L"tekken_recomp.exe") ||
       !fs::is_regular_file(root/L"game"/L"tekken.img"))
        throw std::runtime_error("The installed update is incomplete. Run the updater again.");
    const auto data=UserData();
    if(!SetEnvironmentVariableW(L"TTT2_SETTINGS_INI",(data/L"settings.ini").c_str()) ||
       !SetEnvironmentVariableW(L"TTT2_LAUNCHER_INI",info.c_str()) ||
       !SetEnvironmentVariableW(L"TTT2_VFS_ROOT",(root/L"game").c_str()) ||
       !SetEnvironmentVariableW(L"TTT2_NET",L"loopback"))
        throw std::runtime_error("Cannot configure the installed game.");
    for(const auto* key:{L"TTT2_SAVE_ROOT",L"TTT2_INPUT_SCRIPT",L"TTT2_AUTOSTART"})SetEnvironmentVariableW(key,nullptr);
    return true;
}
}
