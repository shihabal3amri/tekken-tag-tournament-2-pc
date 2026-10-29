// Visual layer only: native controls retain their keyboard and accessibility behavior.
HBRUSH panelBrush = CreateSolidBrush(RGB(19,22,29));
HBRUSH fieldBrush = CreateSolidBrush(RGB(31,35,44));
std::unique_ptr<Gdiplus::Bitmap> hero;
IStream* heroStream = nullptr;
ULONG_PTR graphicsToken = 0;
int activePage = 0;
void LoadArtwork() {
    HRSRC resource = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource) return;
    const DWORD size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* bytes = LockResource(LoadResource(GetModuleHandleW(nullptr), resource));
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory || !bytes) { if(memory) GlobalFree(memory); return; }
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return; }
    std::memcpy(target, bytes, size); GlobalUnlock(memory);
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &heroStream))) { GlobalFree(memory); return; }
    hero.reset(Gdiplus::Bitmap::FromStream(heroStream));
}
void Label(Gdiplus::Graphics& g, const wchar_t* text, float x, float y, float w, float h,
           float size, Gdiplus::Color color, int style = Gdiplus::FontStyleRegular) {
    Gdiplus::Font family(L"Segoe UI", float(Px(int(size))), style, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(color);
    g.DrawString(text, -1, &family, Gdiplus::RectF(float(Px(int(x))),float(Px(int(y))),float(Px(int(w))),float(Px(int(h)))), nullptr, &brush);
}
void PaintLauncher(HDC dc) {
    using namespace Gdiplus;
    Graphics g(dc); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(Color(255,13,16,22));
    if (hero && hero->GetLastStatus()==Ok) {
        const float crop = hero->GetHeight()*400.0f/790.0f;
        g.DrawImage(hero.get(),Rect(0,0,Px(400),Px(790)),float(hero->GetWidth())-crop-30,0,crop,float(hero->GetHeight()),UnitPixel);
    }
    SolidBrush shade(Color(110,0,0,0)); g.FillRectangle(&shade,0,0,Px(400),Px(790));
    LinearGradientBrush fade(Point(0,Px(310)),Point(0,Px(790)),Color(0,0,0,0),Color(250,8,10,15));
    g.FillRectangle(&fade,0,Px(310),Px(400),Px(480));
    SolidBrush accent(Color(255,235,48,66)); g.FillRectangle(&accent,Px(40),Px(44),Px(42),Px(4));
    Label(g,L"TEKKEN",36,72,350,73,55,Color(255,255,255,255),FontStyleBold);
    Label(g,L"TAG TOURNAMENT 2",40,143,340,42,25,Color(255,244,244,247),FontStyleBold);
    Label(g,L"NATIVE PC PORT",42,198,320,28,12,Color(255,208,210,218),FontStyleBold);
    Label(g,L"SET UP.\nSTEP INTO THE ARENA.",38,590,330,90,26,Color(255,255,255,255),FontStyleBold);
    Label(g,L"Your settings. Your next round.",40,698,330,28,14,Color(255,166,173,187));
    Label(g,L"Community PC port  /  v0.1.7",40,748,330,25,12,Color(255,125,135,153));
    SolidBrush panel(Color(255,19,22,29)); g.FillRectangle(&panel,Px(416),Px(24),Px(640),Px(742));
    Label(g,L"GAME CONFIGURATION",446,44,560,34,23,Color(255,244,246,250),FontStyleBold);
    Pen line(Color(255,44,49,61),1); g.DrawLine(&line,Px(448),Px(136),Px(1024),Px(136));
    g.DrawLine(&line,Px(448),Px(574),Px(1024),Px(574));
}
void DrawButton(const DRAWITEMSTRUCT& item) {
    using namespace Gdiplus;
    Graphics g(item.hDC); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    RectF bounds(float(item.rcItem.left),float(item.rcItem.top),float(item.rcItem.right-item.rcItem.left),float(item.rcItem.bottom-item.rcItem.top));
    const bool play = item.CtlID==Play;
    const bool selected = (item.CtlID==GraphicsTab && activePage==0)||(item.CtlID==InputTab && activePage==1)||(item.CtlID==CheatsTab && activePage==2)||(item.CtlID==AdvancedTab && activePage==3)||(item.CtlID==QualityTab && activePage==4);
    const bool pressed=(item.itemState&ODS_SELECTED)!=0;
    Color fill = play ? Color(255,pressed?183:227,36,57) : selected ? Color(255,57,31,41) : Color(255,31,35,44);
    SolidBrush brush(fill); g.FillRectangle(&brush,bounds);
    Pen border(selected||play?Color(255,235,60,78):Color(255,62,69,84)); g.DrawRectangle(&border,bounds.X,bounds.Y,bounds.Width-1,bounds.Height-1);
    wchar_t text[128]{}; GetWindowTextW(item.hwndItem,text,128);
    Font family(L"Segoe UI",float(Px(play?17:14)),play?FontStyleBold:FontStyleRegular,UnitPixel);
    SolidBrush ink(Color(255,245,247,252)); StringFormat format;format.SetAlignment(StringAlignmentCenter);format.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text,-1,&family,bounds,&format,&ink);
    if(item.itemState&ODS_FOCUS){RECT focus=item.rcItem;InflateRect(&focus,-4,-4);DrawFocusRect(item.hDC,&focus);}
}
void DrawCombo(const DRAWITEMSTRUCT& item) {
    if (item.itemID==UINT(-1)) return;
    const bool selected=(item.itemState&ODS_SELECTED)!=0;
    HBRUSH brush=CreateSolidBrush(selected?RGB(70,39,52):RGB(31,35,44));FillRect(item.hDC,&item.rcItem,brush);DeleteObject(brush);
    SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,RGB(239,242,248));
    SelectObject(item.hDC,font);
    wchar_t text[256]{};SendMessageW(item.hwndItem,CB_GETLBTEXT,item.itemID,reinterpret_cast<LPARAM>(text));
    RECT rect=item.rcItem;rect.left+=Px(12);DrawTextW(item.hDC,text,-1,&rect,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    if(item.itemState&ODS_FOCUS){InflateRect(&rect,-2,-2);DrawFocusRect(item.hDC,&rect);}
}

LRESULT CALLBACK ComboTheme(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    const LRESULT result=DefSubclassProc(hwnd,msg,wp,lp);
    if(msg==CB_SETCURSEL && !internalSelection && !buildingUI && result!=CB_ERR)
        SendMessageW(GetParent(hwnd),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(hwnd),CBN_SELCHANGE),reinterpret_cast<LPARAM>(hwnd));
    if(msg==WM_PAINT) {
        RECT rect{};GetClientRect(hwnd,&rect);HDC dc=GetDC(hwnd);
        RECT arrow{rect.right-Px(26),1,rect.right-1,rect.bottom-1};FillRect(dc,&arrow,fieldBrush);
        HPEN pen=CreatePen(PS_SOLID,1,RGB(120,132,153));HGDIOBJ old=SelectObject(dc,pen);
        const int x=rect.right-Px(14),y=rect.bottom/2;
        MoveToEx(dc,x-Px(4),y-Px(2),nullptr);LineTo(dc,x,y+Px(2));LineTo(dc,x+Px(4),y-Px(2));
        SelectObject(dc,old);DeleteObject(pen);
        HBRUSH border=CreateSolidBrush(RGB(62,69,84));FrameRect(dc,&rect,border);DeleteObject(border);ReleaseDC(hwnd,dc);
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,ComboTheme,id);
    return result;
}
