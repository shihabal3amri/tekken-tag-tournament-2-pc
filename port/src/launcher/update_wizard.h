#pragma once
// Included inside inplace_update: standard Windows property-sheet wizard.
constexpr int kHeading=1500,kBody=1501,kPath=1502,kBrowse=1503,kProgress=1504;
constexpr UINT kInstallDone=WM_APP+91;
struct WizardData;
struct WizardPage {WizardData* data;int index;};
struct WizardData {
    Options options;WizardPage pages[5]{};HWND sheet{};bool working=false,failed=false,finished=false;
    std::thread worker;std::string error;
#ifdef TTT2_WIZARD_TEST
    void (*observe)(HWND,int,WizardData*){};
#endif
};
inline HWND Widget(HWND page,const wchar_t* type,const wchar_t* text,DWORD style,int id,int x,int y,int w,int h) {
    RECT r{x,y,x+w,y+h};MapDialogRect(page,&r);
    auto control=CreateWindowExW(type==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,type,text,
        WS_CHILD|WS_VISIBLE|style,r.left,r.top,r.right-r.left,r.bottom-r.top,page,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(control,WM_SETFONT,SendMessageW(page,WM_GETFONT,0,0),TRUE);return control;
}
inline void WizardError(HWND page,const char* error) {MessageBoxA(page,error,"Tekken Tag Tournament 2 Update",MB_OK|MB_ICONERROR);}
inline INT_PTR CALLBACK WizardPageProc(HWND page,UINT message,WPARAM wp,LPARAM lp) {
    auto* current=reinterpret_cast<WizardPage*>(GetWindowLongPtrW(page,GWLP_USERDATA));
    if(message==WM_INITDIALOG) {
        current=reinterpret_cast<WizardPage*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
        SetWindowLongPtrW(page,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(current));
        const wchar_t* titles[]={L"Welcome",L"Choose game folder",L"Ready to update",L"Updating your game",L"Update complete"};
        Widget(page,L"STATIC",titles[current->index],0,kHeading,12,12,316,20);
        const wchar_t* bodies[]={
            L"Version " TTT2_UPDATE_VERSION_W L" (2026-10-29)\r\n\r\nWhat is new:\r\n- Standard Windows update wizard.\r\n- Native game startup without a CMD window.\r\n- Existing game folder, shortcut and normal saves retained.\r\n- Offline update and original online launch options.\r\n\r\nClick Next to choose your installed game folder.",
            L"Select the folder containing Tekken Tag Tournament 2.exe.\r\nThe existing desktop shortcut will keep working.",
            L"",L"Please wait while the required program files are replaced.\r\n\r\nDo not close this wizard during the update.",
            L"Your game has been updated.\r\n\r\nOpen your existing desktop shortcut to use the launcher.\r\nChoose Play Offline for the updated game or Play Online for the original game.\r\n\r\nYour saves stay in their normal location."
        };
        Widget(page,L"STATIC",bodies[current->index],0,kBody,12,40,316,130);
        if(current->index==1) {
            Widget(page,L"EDIT",current->data->options.install.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,kPath,12,89,251,16);
            Widget(page,L"BUTTON",L"Browse...",WS_TABSTOP|BS_PUSHBUTTON,kBrowse,269,88,59,18);
            Widget(page,L"STATIC",L"No second game installation will be created.",0,0,12,121,316,25);
        }
        if(current->index==3)Widget(page,PROGRESS_CLASSW,L"",PBS_MARQUEE,kProgress,12,132,316,14);
        return TRUE;
    }
    if(!current)return FALSE;
    auto& data=*current->data;
    if(message==WM_APP+92) {
        SetWindowTextW(data.sheet,L"Tekken Tag Tournament 2 Update - v" TTT2_UPDATE_VERSION_W);
        // The basic property-sheet wizard resets Next after PSN_SETACTIVE.
        // Apply its label once activation has completed (standard control ID).
        SetWindowTextW(GetDlgItem(data.sheet,0x3024),current->index==2?L"Install":L"Next >");
        return TRUE;
    }
    if(message==WM_COMMAND && LOWORD(wp)==kBrowse) {
        try{const auto folder=ChooseFolder(page);if(!folder.empty())SetDlgItemTextW(page,kPath,folder.c_str());}catch(const std::exception& e){WizardError(page,e.what());}return TRUE;
    }
    if(message==kInstallDone) {
        data.worker.join();data.working=false;SendDlgItemMessageW(page,kProgress,PBM_SETMARQUEE,FALSE,0);
        if(data.error.empty()){data.finished=true;PropSheet_SetCurSel(data.sheet,nullptr,4);}
        else {data.failed=true;SetDlgItemTextA(page,kBody,("Update could not finish.\r\n\r\n"+data.error+"\r\n\r\nReplaced files were rolled back. Click Back to review the folder or Cancel to exit.").c_str());PropSheet_SetWizButtons(data.sheet,PSWIZB_BACK);EnableWindow(GetDlgItem(data.sheet,IDCANCEL),TRUE);}
        return TRUE;
    }
    if(message==WM_NOTIFY) {
        const auto* notice=reinterpret_cast<NMHDR*>(lp);
        if(notice->code==PSN_SETACTIVE) {
            data.sheet=GetParent(page);
            PostMessageW(page,WM_APP+92,0,0);
            if(current->index==0)PropSheet_SetWizButtons(data.sheet,PSWIZB_NEXT);
            if(current->index==1)PropSheet_SetWizButtons(data.sheet,PSWIZB_BACK|PSWIZB_NEXT);
            if(current->index==2) {
                const auto text=L"Game folder:\r\n"+data.options.install.wstring()+L"\r\n\r\nRecovery files (outside the game folder):\r\n"+data.options.recovery.wstring()+L"\r\n\r\nClick Install to update. Existing game assets and saves are kept.";
                SetDlgItemTextW(page,kBody,text.c_str());PropSheet_SetWizButtons(data.sheet,PSWIZB_BACK|PSWIZB_NEXT);
                PropSheet_SetNextText(data.sheet,L"Install");
            }else PropSheet_SetNextText(data.sheet,L"Next >");
            if(current->index==3 && !data.working) {
                data.error.clear();data.working=true;PropSheet_SetWizButtons(data.sheet,0);EnableWindow(GetDlgItem(data.sheet,IDCANCEL),FALSE);
                SendDlgItemMessageW(page,kProgress,PBM_SETMARQUEE,TRUE,40);
                data.worker=std::thread([&data,page]{
                    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
                    try{InstallWithElevation(data.options);}catch(const std::exception& e){data.error=e.what();}
                    if(SUCCEEDED(com))CoUninitialize();PostMessageW(page,kInstallDone,0,0);
                });
            }
            if(current->index==4){PropSheet_SetWizButtons(data.sheet,PSWIZB_FINISH);ShowWindow(GetDlgItem(data.sheet,IDCANCEL),SW_HIDE);}
#ifdef TTT2_WIZARD_TEST
            if(data.observe)data.observe(page,current->index,&data);
#endif
            return TRUE;
        }
        if(notice->code==PSN_WIZNEXT && current->index==1) {
            wchar_t path[32768]{};GetDlgItemTextW(page,kPath,path,32768);data.options.install=path;
            try{Validate(data.options);}catch(const std::exception& e){WizardError(page,e.what());SetWindowLongPtrW(page,DWLP_MSGRESULT,-1);return TRUE;}
        }
        if((notice->code==PSN_QUERYCANCEL || notice->code==PSN_KILLACTIVE)&&data.working) {
            SetWindowLongPtrW(page,DWLP_MSGRESULT,TRUE);return TRUE;
        }
    }
    return FALSE;
}
struct alignas(4) WizardTemplate {DLGTEMPLATE dialog;WORD menu=0,windowClass=0;wchar_t title=0;WORD fontSize=9;wchar_t font[9]=L"Segoe UI";};
inline int Wizard(Options options
#ifdef TTT2_WIZARD_TEST
    ,void (*observer)(HWND,int,WizardData*)=nullptr
#endif
) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES|ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    WizardData data;data.options=options;
#ifdef TTT2_WIZARD_TEST
    data.observe=observer;
#endif
    if(data.options.install.empty()) {
        const auto candidate=fs::path(Environment(L"ProgramFiles"))/L"Tekken Tag Tournament 2";
        if(fs::is_regular_file(candidate/kEntry))data.options.install=candidate;
    }
    WizardTemplate resource{};resource.dialog.style=WS_CHILD|WS_DISABLED|WS_CAPTION|DS_CONTROL|DS_SETFONT;
    resource.dialog.cx=340;resource.dialog.cy=190;
    PROPSHEETPAGEW pages[5]{};
    for(int i=0;i<5;++i){data.pages[i]={&data,i};pages[i].dwSize=sizeof(PROPSHEETPAGEW);pages[i].dwFlags=PSP_DLGINDIRECT;
        pages[i].hInstance=GetModuleHandleW(nullptr);pages[i].pResource=&resource.dialog;pages[i].pfnDlgProc=WizardPageProc;pages[i].lParam=reinterpret_cast<LPARAM>(&data.pages[i]);}
    PROPSHEETHEADERW sheet{sizeof(sheet)};sheet.dwFlags=PSH_WIZARD|PSH_PROPSHEETPAGE|PSH_NOAPPLYNOW|PSH_NOCONTEXTHELP;
    sheet.hInstance=GetModuleHandleW(nullptr);sheet.pszCaption=L"Tekken Tag Tournament 2 Update - v" TTT2_UPDATE_VERSION_W;sheet.nPages=5;sheet.ppsp=pages;
    const auto result=PropertySheetW(&sheet);if(data.worker.joinable())data.worker.join();
    return result==-1||(!data.finished&&data.failed)?1:0;
}
