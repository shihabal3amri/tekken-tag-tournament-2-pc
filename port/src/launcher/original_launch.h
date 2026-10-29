#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cwchar>
#include <algorithm>

namespace original_launch {
inline bool Start(const std::filesystem::path& exe, PROCESS_INFORMATION& process,
                  const std::filesystem::path& installation={}) {
    // Build a private child environment: preserve the launcher environment and
    // remove every preview/debug override from the original game's process.
    wchar_t* inherited=GetEnvironmentStringsW();
    if(!inherited)return false;
    std::vector<std::wstring> values;
    for(const wchar_t* item=inherited;*item;item+=std::wcslen(item)+1) {
        if(_wcsnicmp(item,L"TTT2_",5))values.emplace_back(item);
    }
    FreeEnvironmentStringsW(inherited);
    if(!installation.empty()) {
        values.push_back(L"TTT2_VFS_ROOT="+(installation/L"game").wstring());
        values.push_back(L"TTT2_GPU_HW=1");values.push_back(L"TTT2_FBDUMP=0");
    }
    std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    std::vector<wchar_t> environment;
    for(const auto& value:values)environment.insert(environment.end(),value.c_str(),value.c_str()+value.size()+1);
    environment.push_back(0);if(environment.size()==1)environment.push_back(0);
    std::wstring command=L"\""+exe.wstring()+L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    return CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,
        CREATE_UNICODE_ENVIRONMENT,environment.data(),(installation.empty()?exe.parent_path():installation).c_str(),&startup,&process)!=FALSE;
}
}
