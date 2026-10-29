"""Exercise native wizard pages on a private Windows desktop; never touch user UI."""
import argparse, os, shutil, subprocess, tempfile
from pathlib import Path
from test_vcmpgtuh import ROOT, compile_one
DRIVER=r'''
#define UNICODE 1
#define _UNICODE 1
#pragma comment(lib,"user32.lib")
#define TTT2_INPLACE_UPDATE 1
#define TTT2_WIZARD_TEST 1
#define wWinMain UnusedUpdaterEntry
#include "preview_main.cpp"
#undef wWinMain
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
int visits[5]{};bool cancelOnly=false;fs::path output;
void Capture(HWND window,int index) {
 RECT rect{};GetWindowRect(window,&rect);const int w=rect.right-rect.left,h=rect.bottom-rect.top;
 HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,w,h);
 auto old=SelectObject(dc,bitmap);PrintWindow(window,dc,2);
 BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=h;
 info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
 std::vector<unsigned char> bytes(w*h*4);GetDIBits(dc,bitmap,0,h,bytes.data(),&info,DIB_RGB_COLORS);
 BITMAPFILEHEADER head{};head.bfType=0x4D42;head.bfOffBits=sizeof(head)+sizeof(BITMAPINFOHEADER);head.bfSize=head.bfOffBits+DWORD(bytes.size());
 std::ofstream f(output/(std::to_wstring(index)+L".bmp"),std::ios::binary);f.write((char*)&head,sizeof(head));f.write((char*)&info.bmiHeader,sizeof(info.bmiHeader));f.write((char*)bytes.data(),bytes.size());
 SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
}
void CALLBACK Tick(HWND page,UINT,UINT_PTR id,DWORD) {
 KillTimer(page,id);auto* current=reinterpret_cast<inplace_update::WizardPage*>(GetWindowLongPtrW(page,GWLP_USERDATA));
 auto& d=*current->data;const auto index=current->index;Capture(d.sheet,index);
 if(index==0 || index==1)PropSheet_PressButton(d.sheet,PSBTN_NEXT);
 if(index==2) {
   if(cancelOnly)PropSheet_PressButton(d.sheet,PSBTN_CANCEL);
   else if(visits[2]==1)PropSheet_PressButton(d.sheet,PSBTN_BACK);
   else PropSheet_PressButton(d.sheet,PSBTN_NEXT);
 }
 if(index==4)PropSheet_PressButton(d.sheet,PSBTN_FINISH);
}
void Observe(HWND page,int index,inplace_update::WizardData*){++visits[index];SetTimer(page,88,250,Tick);}
int wmain(int argc,wchar_t** argv) {
 if(argc!=5)return 10;cancelOnly=std::wstring(argv[1])==L"cancel";output=argv[4];fs::create_directories(output);
 const auto name=L"TTT2WizardTest"+std::to_wstring(GetCurrentProcessId());
 HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
 if(!desktop||!SetThreadDesktop(desktop))return 11;
 const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 inplace_update::Options options;options.install=argv[2];options.recovery=argv[3];options.userData=clean_install::UserData();
 const int result=inplace_update::Wizard(options,Observe);
 if(SUCCEEDED(com))CoUninitialize();
 if(result)return 12;
 if(!visits[0]||!visits[1]||!visits[2])return 13;
 if(!cancelOnly&&(!visits[3]||!visits[4]||visits[1]!=2))return 14;
 std::printf("PASS native wizard: %s\n",cancelOnly?"Next, browse page, review, Cancel without installation":"Next, review, Back, Install, progress, Finish");return 0;
}
'''
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--payload',type=Path,required=True);ap.add_argument('--installed-game-dir',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    payload=a.payload.resolve();a.output.mkdir(parents=True,exist_ok=True);out=a.output.resolve()
    with tempfile.TemporaryDirectory(prefix='ttt2-wizard-') as temp:
        base=Path(temp);local=base/'local';local.mkdir();env={**os.environ,'LOCALAPPDATA':str(local)}
        for item in payload.iterdir():
            if item.is_dir():shutil.copytree(item,base/item.name)
            else:shutil.copy2(item,base/item.name)
        driver=compile_one(base,'wizard_driver',DRIVER,[ROOT/'port/src/launcher'])
        for mode in ('cancel','install'):
            game=base/(mode+' game');(game/'game').mkdir(parents=True)
            (game/'Tekken Tag Tournament 2.exe').write_bytes(b'original sentinel')
            shutil.copy2(a.installed_game_dir/'game/tekken.img',game/'game/tekken.img')
            subprocess.run([str(driver),mode,str(game),str(base/'external recovery'),str(out/mode)],env=env,check=True,timeout=60)
            if mode=='cancel':assert {p.name for p in game.iterdir()}=={'game','Tekken Tag Tournament 2.exe'}
            else:assert (game/'tekken_recomp.exe').is_file()
    try:
        from PIL import Image
        for p in out.rglob('*.bmp'):Image.open(p).save(p.with_suffix('.png'))
    except ImportError:pass
if __name__=='__main__':main()
