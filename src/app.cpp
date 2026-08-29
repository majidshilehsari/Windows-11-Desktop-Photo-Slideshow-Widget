// app.cpp - widget window, hidden message window, tray icon, menu, settings
// dialog, "stay on the desktop layer" logic, autostart and hotkeys.
#include "dskv_pch.h"
#include "config.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wtsapi32.lib")

using namespace Gdiplus;

namespace dskv {

// =========================================================== small utils ====
void Log(const Ctx& c, const std::wstring& msg)
{
    if (!c.cfg || !c.cfg->log) return;
    std::wstring p = c.exeDir + L"\\slideshow.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t head[64];
    swprintf(head, 64, L"[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(head) + msg + L"\r\n";
    std::string out;
    out.push_back(char(0xFF)); out.push_back(char(0xFE));
    out.append(reinterpret_cast<const char*>(line.data()), line.size() * 2);
    DWORD w = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    WriteFile(h, out.data(), DWORD(out.size()), &w, nullptr);
    CloseHandle(h);
}

namespace {

HINSTANCE g_inst = nullptr;
NOTIFYICONDATAW g_nid{};
bool g_nidOk = false;
constexpr UINT TRAY_UID = 0x5D43;

// everything in this file is in namespace dskv; only the pieces listed in
// dskv_pch.h are public, the rest stays in this translation unit.
// forward declarations (these are the entry points listed in dskv_pch.h)
void UpdateDpi(Ctx& c);
RECT DefaultRect(Ctx& c);
void EnforceBottom(Ctx& c);
void ApplyWindowStyles(Ctx& c);

bool SlideAdvanceAllowed(Ctx& c);
void StartTimerSchedule(Ctx& c);
void AdvanceStepTiming(Ctx& c, bool randomized);
void CommitPending(Ctx& c);
void TogglePause(Ctx& c);
void Next(Ctx& c, bool autoStep);
// the incoming slide keeps its own timing until the crossfade is over
void CommitPending(Ctx& c)
{
    if (!c.havePend) return;
    c.havePend = false;
    c.stepMs = c.pStepMs;
    c.stepStartMs = c.pStepStart;
    c.zoomOut = c.pZoomOut;
}

void GoTo(Ctx& c, long long delta, bool userAction);
void SetEditImpl(Ctx& c, bool on);
void UpdateHover(Ctx& c);
bool CreateTrayIcon(Ctx& c);
void DestroyTrayIcon(Ctx& c);
void UpdateTrayTip(Ctx& c);
bool AutostartIsOnRaw();
void RegisterHotkeys(Ctx& c, bool on);
void PullFromDialog(Ctx& c, HWND dlg);
void LoadIntoDialog(Ctx& c, HWND dlg);
void ApplyAll(Ctx& c, bool rebuildPlaylist);
void ShowSettingsDialogImpl(Ctx& c);
void OnTimerTickImpl(Ctx& c, unsigned id);
LRESULT CALLBACK WidgetProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK HiddenProc(HWND, UINT, WPARAM, LPARAM);
BOOL CALLBACK HiddenEnumQuitProc(HWND, LPARAM);
constexpr int HOTK_EDIT = 0xB001;
constexpr int HOTK_PLAY = 0xB002;
constexpr int HOTK_NEXT = 0xB003;
constexpr int HOTK_PREV = 0xB004;

enum Cmd {
    CM_PLAY = 1, CM_NEXT, CM_PREV, CM_SHUFFLE, CM_REPEAT, CM_EDIT, CM_CLICK, CM_BOTTOM,
    CM_TOPMOST, CM_CAPTION, CM_TASKBAR, CM_AUTOSTART, CM_GRIPOPT, CM_FOLDER, CM_RELOAD,
    CM_SETTINGS, CM_OPENCFG, CM_ABOUT, CM_QUIT, CM_HALF, CM_POSFolder
};
constexpr UINT CM_INTERVAL_BASE = 0x3000;
constexpr int kIntervals[] = { 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600 };

std::wstring Num(size_t v) { return std::to_wstring(v); }

// ------------------------------------------------------------- geometry ----
RECT WorkOf(HWND h)
{
    HMONITOR m = MonitorFromWindow(h ? h : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (m && GetMonitorInfoW(m, &mi)) return mi.rcWork;
    RECT r{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0)) r = { 0, 0, 1280, 720 };
    return r;
}

RECT DefaultRect(Ctx& c)
{
    Config& k = *c.cfg;
    RECT rc{};
    if (k.have_rect && k.width > 60 && k.height > 60)
        rc = { k.left, k.top, k.left + k.width, k.top + k.height };
    else {
        RECT wa = WorkOf(c.widget);
        int w = std::max(320, int((wa.right - wa.left) * 0.42));
        int h = std::max(220, int((wa.bottom - wa.top) * 0.62));
        int x = wa.right - w - int((wa.right - wa.left) * 0.03);
        int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
        rc = { x, y, x + w, y + h };
    }
    HMONITOR m = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, &mi)) return rc;
    RECT wa = mi.rcWork;
    int w = Clampi(rc.right - rc.left, 160, wa.right - wa.left);
    int h = Clampi(rc.bottom - rc.top, 120, wa.bottom - wa.top);
    const int vis = 80;
    int x = Clampi(rc.left, wa.left - (w - vis), wa.right - vis);
    int y = Clampi(rc.top, wa.top - (h - vis), wa.bottom - vis);
    if (k.half_screen) {
        int cw = (wa.right - wa.left) / 2;
        return { wa.right - cw, wa.top, wa.right, wa.bottom };
    }
    return { x, y, x + w, y + h };
}

void UpdateDpi(Ctx& c)
{
    UINT dpiX = 96, dpiY = 96;
    HMONITOR m = MonitorFromWindow(c.widget ? c.widget : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    if (m) GetDpiForMonitor(m, static_cast<MONITOR_DPI_TYPE>(MDT_EFFECTIVE_DPI), &dpiX, &dpiY);
    if (!dpiX) {                                  // very old SDKs / RDP sessions
        HDC dc = GetDC(nullptr);
        dpiX = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(nullptr, dc);
    }
    c.dpi = static_cast<int>(dpiX ? dpiX : 96);
}

// --------------------------------------------------------- window styles ----
void ApplyWindowStyles(Ctx& c)
{
    Config& k = *c.cfg;
    if (!c.widget) return;
    LONG_PTR ex = GetWindowLongPtrW(c.widget, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED;
    ex &= ~LONG_PTR(WS_EX_TOPMOST);
    if (k.show_in_taskbar) ex &= ~LONG_PTR(WS_EX_TOOLWINDOW); else ex |= WS_EX_TOOLWINDOW;
    bool ct = k.click_through && !c.edit;
    if (ct) ex |= WS_EX_TRANSPARENT; else ex &= ~LONG_PTR(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(c.widget, GWL_EXSTYLE, ex);
    SetWindowPos(c.widget, k.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Log(c, L"styles: click_through=" + std::wstring(ct ? L"1" : L"0") +
                   L" taskbar=" + std::wstring(k.show_in_taskbar ? L"1" : L"0") +
                   L" topmost=" + std::wstring(k.topmost ? L"1" : L"0"));
}

void EnforceBottom(Ctx& c)
{
    Config& k = *c.cfg;
    if (!c.widget || k.topmost || !k.auto_bottom || c.edit) return;
    if (!IsWindowVisible(c.widget)) return;
    if (GetWindow(c.widget, GW_HWNDPREV) == nullptr) return;      // already the bottom-most
    SetWindowPos(c.widget, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void StartTimerSchedule(Ctx& c)
{
    Config& k = *c.cfg;
    if (!c.hidden) return;
    if (k.auto_bottom && !k.topmost)
        SetTimer(c.hidden, DSKV_TIMER_BOTTOM, static_cast<UINT>(Clampi(k.bottom_check_ms, 250, 60000)), nullptr);
    else
        KillTimer(c.hidden, DSKV_TIMER_BOTTOM);
    if (SlideAdvanceAllowed(c))
        SetTimer(c.hidden, DSKV_TIMER_STEP, c.stepMs ? c.stepMs : static_cast<UINT>(k.interval_s * 1000), nullptr);
    else
        KillTimer(c.hidden, DSKV_TIMER_STEP);
    Invalidate(c);
}


// may the slideshow move on to the next picture right now?
bool SlideAdvanceAllowed(Ctx& c)
{
    Config& k = *c.cfg;
    if (!c.playing || c.list.size() < 2) return false;
    if (c.locked) return false;                       // workstation locked
    if (k.pause_fs && FullscreenRunning()) return false;
    return true;
}

// ------------------------------------------------------------- slides ------
void StartFade(Ctx& c)
{
    Config& k = *c.cfg;
    if (!c.cur.bmp || k.transition_ms <= 0 || c.list.size() < 2) return;
    FreeScaled(c.fadeOld);
    c.fadeOld.path = c.cur.path;
    c.fadeOld.sig = c.cur.sig;
    c.fadeOld.srcSize = c.cur.srcSize;
    c.fadeOld.boxSize = c.cur.boxSize;
    c.fadeOld.coverFill = c.cur.coverFill;
    c.fadeOld.baseScale = c.cur.baseScale;
    c.fadeOld.bmp = new Bitmap(c.cur.boxSize.cx, c.cur.boxSize.cy, c.cur.bmp->GetPixelFormat());
    if (c.fadeOld.bmp && c.fadeOld.bmp->GetLastStatus() == Ok) {
        Graphics gg(c.fadeOld.bmp);
        gg.SetCompositingMode(CompositingModeSourceCopy);
        
    }
    if (c.fadeOld.bmp && c.fadeOld.bmp->GetLastStatus() == Ok) {
        c.hasFade = true;
        c.transStartMs = GetTickCount64();
        c.transMs = static_cast<unsigned>(k.transition_ms);
    } else {
        FreeScaled(c.fadeOld);
        c.hasFade = false;
    }
}

void AdvanceStepTiming(Ctx& c, bool randomized)
{
    Config& k = *c.cfg;
    c.zoomOut = k.anim_alt && (c.index % 2 == 1);
    c.stepStartMs = GetTickCount64();
    int secs = std::max(1, k.interval_s);
    if (randomized && k.random_interval && k.random_jitter > 0) {
        int lo = std::max(1, secs * (100 - k.random_jitter) / 100);
        int hi = std::max(lo, secs * (100 + k.random_jitter) / 100);
        secs = lo + (hi > lo ? rand() % (hi - lo) : 0);
    }
    c.stepMs = static_cast<unsigned>(secs * 1000);
}

void GoTo(Ctx& c, long long delta, bool userAction)
{
    if (c.list.empty()) { Invalidate(c); return; }
    long long n = static_cast<long long>(c.list.size());
    long long ni = (static_cast<long long>(c.index) + delta) % n;
    if (ni < 0) ni += n;
    c.index = size_t(ni);
    FreeScaled(c.cur);
    FreeScaled(c.fadeOld);
    c.hasFade = false;
    AdvanceStepTiming(c, false);
    StartTimerSchedule(c);
    RenderFrame(c);
    UpdateTrayTip(c);
    (void)userAction;
}

void Next(Ctx& c, bool autoStep)
{
    Config& k = *c.cfg;
    if (c.list.empty()) { Invalidate(c); return; }
    if (autoStep && !k.shuffle && !k.repeat && c.index + 1 >= c.list.size()) {
        // one pass through the folder, then rest on the last picture
        c.playing = false;
        c.userPlaying = false;
        if (c.hasFade) CommitPending(c);
        StartTimerSchedule(c);
        RenderFrame(c);
        UpdateTrayTip(c);
        return;
    }
    StartFade(c);
    c.index = (c.index + 1) % c.list.size();
    if (c.hasFade) {
        // remember the new timing, but let the old picture keep its motion
        Ctx tmp = c;
        tmp.cfg = c.cfg;
        AdvanceStepTiming(tmp, autoStep);
        c.pStepMs = tmp.stepMs;
        c.pStepStart = tmp.stepStartMs;
        c.pZoomOut = tmp.zoomOut;
        c.havePend = true;
    } else {
        AdvanceStepTiming(c, autoStep);
    }
    FreeScaled(c.cur);
    StartTimerSchedule(c);
    RenderFrame(c);
    UpdateTrayTip(c);
}

void TogglePause(Ctx& c)
{
    c.userPlaying = !c.playing;
    c.playing = c.userPlaying;
    StartTimerSchedule(c);
    RenderFrame(c);
    UpdateTrayTip(c);
}

void SetEditImpl(Ctx& c, bool on)
{
    c.edit = on;
    c.cfg->edit = on;
    ApplyWindowStyles(c);
    if (on) {
        KillTimer(c.hidden, DSKV_TIMER_STEP);
        SetForegroundWindow(c.widget);
    } else {
        SaveConfig(c);
    }
    StartTimerSchedule(c);
    RenderFrame(c);
}

void UpdateHover(Ctx& c)
{
    if (!c.widget || (!c.cfg->cap_hover && !c.cfg->show_grip)) return;
    POINT p{};
    bool hov = false;
    RECT wr{};
    if (GetWindowRect(c.widget, &wr) && GetCursorPos(&p))
        hov = PtInRect(&wr, p) != FALSE;
    if (hov != c.hovering) { c.hovering = hov; Invalidate(c); }
}

// ---------------------------------------------------------------- tray -----
void DestroyTrayIcon(Ctx&)
{
    if (g_nidOk) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_nidOk = false; }
}

void UpdateTrayTip(Ctx& c)
{
    if (!g_nidOk) return;
    Config& k = *c.cfg;
    std::wstring tip = DSKV_APP_NAME;
    if (!c.list.empty()) tip += L" \u2014 " + Num(c.index + 1) + L"/" + Num(c.list.size());
    if (!c.playing) tip += L" \u00b7 paused";
    if (!k.path.empty()) tip += L"\r\n" + std::filesystem::path(k.path).filename().wstring();
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

bool CreateTrayIcon(Ctx& c)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = c.hidden;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = c.icon ? c.icon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, DSKV_APP_NAME);
    g_nidOk = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    c.trayReady = g_nidOk;
    if (!g_nidOk) Log(c, L"tray icon not registered (hidden by shell settings?)");
    UpdateTrayTip(c);
    return g_nidOk;
}


// ------------------------------------------------------------- autostart ---
std::wstring ExePath()
{
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n ? n : 0);
    return buf;
}

bool AutostartIsOnRaw()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    wchar_t val[1024] = {};
    DWORD cb = sizeof(val), type = 0;
    LONG r = RegQueryValueExW(hk, DSKV_APP_NAME, nullptr, &type, (BYTE*)val, &cb);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS && val[0] != 0;
}


// --------------------------------------------------------------- hotkeys ---
void RegisterHotkeys(Ctx& c, bool on)
{
    if (!c.hidden) return;
    UnregisterHotKey(c.hidden, HOTK_EDIT);
    UnregisterHotKey(c.hidden, HOTK_PLAY);
    UnregisterHotKey(c.hidden, HOTK_NEXT);
    UnregisterHotKey(c.hidden, HOTK_PREV);
    if (!on || c.cfg->hotkeys == L"none") { Log(c, L"hotkeys: off"); return; }
    int ok = 0;
    ok += RegisterHotKey(c.hidden, HOTK_EDIT, MOD_CONTROL | MOD_ALT, 'W') != 0;
    ok += RegisterHotKey(c.hidden, HOTK_PLAY, MOD_CONTROL | MOD_ALT, 'P') != 0;
    ok += RegisterHotKey(c.hidden, HOTK_NEXT, MOD_CONTROL | MOD_ALT, VK_RIGHT) != 0;
    ok += RegisterHotKey(c.hidden, HOTK_PREV, MOD_CONTROL | MOD_ALT, VK_LEFT) != 0;
    Log(c, L"hotkeys registered " + Num(unsigned(ok)) + L"/4");
}

// --------------------------------------------------------- folder picker ---
bool PickFolder(Ctx& c, HWND owner, std::wstring& out)
{
    bool ok = false;
    IFileOpenDialog* fd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&fd)))) {
        DWORD opts = 0;
        fd->GetOptions(&opts);
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        std::error_code ec;
        if (!c.cfg->path.empty() && std::filesystem::exists(c.cfg->path, ec)) {
            IShellItem* si = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(c.cfg->path.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
                fd->SetFolder(si);
                si->Release();
            }
        }
        if (SUCCEEDED(fd->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(fd->GetResult(&item))) {
                PWSTR raw = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) {
                    out = raw;
                    CoTaskMemFree(raw);
                    ok = !out.empty();
                }
                item->Release();
            }
        }
        fd->Release();
    }
    if (!ok) {
        BROWSEINFOW bi{};
        bi.hwndOwner = owner;
        bi.lpszTitle = L"Pick the folder that holds your photos";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t buf[4096] = {};
            if (SHGetPathFromIDListW(pidl, buf) && buf[0]) { out = buf; ok = true; }
            CoTaskMemFree(pidl);
        }
    }
    return ok;
}

// ============================================================ the dialog ====
struct DlgState {
    Ctx*  ctx = nullptr;
    HWND  h = nullptr;
    int   dpi = 96;
    std::vector<HWND> order;
};
DlgState* g_dlg = nullptr;

int S(Ctx& c, int v) { return MulDiv(v, c.dpi ? c.dpi : 96, 96); }

HWND Mk(Ctx& c, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
         int x, int y, int w, int h, int id, HFONT font)
{
    HWND hw = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                              S(c, x), S(c, y), S(c, w), S(c, h), parent,
                              HMENU(INT_PTR(id)), c.inst, nullptr);
    if (hw) {
        SendMessageW(hw, WM_SETFONT, WPARAM(font), TRUE);
        if (g_dlg) g_dlg->order.push_back(hw);
    }
    return hw;
}

std::wstring GetTxt(HWND dlg, int id)
{
    HWND hw = GetDlgItem(dlg, id);
    if (!hw) return L"";
    int n = GetWindowTextLengthW(hw);
    std::wstring s(size_t(n) + 1, L'\0');
    GetWindowTextW(hw, s.data(), n + 1);
    s.resize(wcslen(s.c_str()));
    return s;
}

double GetNum(HWND dlg, int id, double def)
{
    std::wstring s = GetTxt(dlg, id);
    if (s.empty()) return def;
    wchar_t* e = nullptr;
    double v = wcstod(s.c_str(), &e);
    return (e && e != s.c_str()) ? v : def;
}

bool Ck(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }

enum {
    ID_PATH = 1002, ID_BROWSE, ID_OPENFOLDER, ID_RECURSIVE, ID_SORTDESC, ID_SHUFFLE, ID_REPEAT,
    ID_EXIF, ID_INTERVAL, ID_TRANS, ID_RANDI, ID_JITTER, ID_ANIM, ID_ZOOM, ID_FPS, ID_FIT,
    ID_CAPTION, ID_CAPFMT, ID_CORNER, ID_SHADOW, ID_BORDER, ID_GRIP, ID_CLICK, ID_BOTTOM,
    ID_TOPMOST, ID_PAUSEFS, ID_AUTOSTART, ID_TASKBAR, ID_HALF, ID_EDITMODE, ID_LOG, ID_RESETPOS
};

void PullFromDialog(Ctx& c, HWND dlg)
{
    Config& k = *c.cfg;
    k.recursive = Ck(dlg, ID_RECURSIVE);
    {
        std::wstring p = GetTxt(dlg, ID_PATH);
        for (auto& ch : p) if (ch == L'/') ch = L'\\';
        while (!p.empty() && (p.back() == L'\\' || p.back() == L' ')) p.pop_back();
        if (!p.empty()) k.path = p;
    }
    k.shuffle = Ck(dlg, ID_SHUFFLE);
    k.sort_desc = Ck(dlg, ID_SORTDESC);
    k.repeat = Ck(dlg, ID_REPEAT);
    k.exif_auto_rotate = Ck(dlg, ID_EXIF);
    k.interval_s = Clampi(int(GetNum(dlg, ID_INTERVAL, k.interval_s)), 1, 86400);
    k.transition_ms = Clampi(int(GetNum(dlg, ID_TRANS, k.transition_ms)), 0, 8000);
    k.random_interval = Ck(dlg, ID_RANDI);
    k.random_jitter = Clampi(int(GetNum(dlg, ID_JITTER, k.random_jitter)), 0, 90);
    {
        int sel = int(SendMessageW(GetDlgItem(dlg, ID_ANIM), CB_GETCURSEL, 0, 0));
        k.anim = sel == 0 ? L"none" : (sel == 2 ? L"pan" : L"zoom");
    }
    k.anim_zoom = Clampd(GetNum(dlg, ID_ZOOM, 1.10), 1.0, 2.0);
    k.anim_fps = Clampi(int(GetNum(dlg, ID_FPS, k.anim_fps)), 0, 60);
    {
        int sel = int(SendMessageW(GetDlgItem(dlg, ID_FIT), CB_GETCURSEL, 0, 0));
        k.fit = sel == 0 ? Fit::Cover : (sel == 1 ? Fit::Contain : Fit::Stretch);
    }
    k.caption = Ck(dlg, ID_CAPTION);
    {
        std::wstring s = GetTxt(dlg, ID_CAPFMT);
        if (!s.empty()) k.cap_fmt = s;
    }
    k.corner = Clampi(int(GetNum(dlg, ID_CORNER, k.corner)), 0, 64);
    k.shadow = Clampi(int(GetNum(dlg, ID_SHADOW, k.shadow)), 0, 64);
    k.border = Clampi(int(GetNum(dlg, ID_BORDER, k.border)), 0, 20);
    k.show_grip = Ck(dlg, ID_GRIP);
    k.click_through = Ck(dlg, ID_CLICK);
    k.auto_bottom = Ck(dlg, ID_BOTTOM);
    k.topmost = Ck(dlg, ID_TOPMOST);
    k.pause_fs = Ck(dlg, ID_PAUSEFS);
    bool wantAuto = Ck(dlg, ID_AUTOSTART);
    if (wantAuto != AutostartIsOnRaw()) SetAutostart(c, wantAuto, true);
    k.show_in_taskbar = Ck(dlg, ID_TASKBAR);
    k.half_screen = Ck(dlg, ID_HALF);
    k.edit = Ck(dlg, ID_EDITMODE);
    k.log = Ck(dlg, ID_LOG);
}

void LoadIntoDialog(Ctx& c, HWND dlg)
{
    Config& k = *c.cfg;
    auto chk = [&](int id, bool v) { CheckDlgButton(dlg, id, v ? BST_CHECKED : BST_UNCHECKED); };
    auto set = [&](int id, const std::wstring& v) { SetWindowTextW(GetDlgItem(dlg, id), v.c_str()); };
    set(ID_PATH, k.path);
    chk(ID_RECURSIVE, k.recursive);
    chk(ID_SORTDESC, k.sort_desc);
    chk(ID_SHUFFLE, k.shuffle);
    chk(ID_REPEAT, k.repeat);
    chk(ID_EXIF, k.exif_auto_rotate);
    set(ID_INTERVAL, Num(k.interval_s));
    set(ID_TRANS, Num(k.transition_ms));
    chk(ID_RANDI, k.random_interval);
    set(ID_JITTER, Num(k.random_jitter));
    SendMessageW(GetDlgItem(dlg, ID_ANIM), CB_SETCURSEL, k.anim == L"none" ? 0 : (k.anim == L"pan" ? 2 : 1), 0);
    wchar_t zb[32]; swprintf(zb, 32, L"%.2f", k.anim_zoom);
    set(ID_ZOOM, zb);
    set(ID_FPS, Num(unsigned(k.anim_fps)));
    SendMessageW(GetDlgItem(dlg, ID_FIT), CB_SETCURSEL, k.fit == Fit::Cover ? 0 : (k.fit == Fit::Contain ? 1 : 2), 0);
    chk(ID_CAPTION, k.caption);
    set(ID_CAPFMT, k.cap_fmt);
    set(ID_CORNER, Num(k.corner));
    set(ID_SHADOW, Num(k.shadow));
    set(ID_BORDER, Num(k.border));
    chk(ID_GRIP, k.show_grip);
    chk(ID_CLICK, k.click_through);
    chk(ID_BOTTOM, k.auto_bottom);
    chk(ID_TOPMOST, k.topmost);
    chk(ID_PAUSEFS, k.pause_fs);
    chk(ID_AUTOSTART, AutostartIsOnRaw());
    chk(ID_TASKBAR, k.show_in_taskbar);
    chk(ID_HALF, k.half_screen);
    chk(ID_EDITMODE, k.edit);
    chk(ID_LOG, k.log);
}

void ApplyAll(Ctx& c, bool rebuildPlaylist)
{
    Config& k = *c.cfg;
    if (rebuildPlaylist) {
        FreeScaled(c.cur);
        FreeScaled(c.fadeOld);
        c.hasFade = false;
        RebuildPlaylist(c);
        if (c.index >= c.list.size()) c.index = 0;
        RECT rc = DefaultRect(c);
        if (!k.have_rect || k.half_screen)
            SetWindowPos(c.widget, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }
    c.edit = k.edit;
    ApplyWindowStyles(c);
    AdvanceStepTiming(c, false);
    RegisterHotkeys(c, k.hotkeys != L"none");
    StartTimerSchedule(c);
    RenderFrame(c);
    UpdateTrayTip(c);
}

LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Ctx* cp = g_dlg ? g_dlg->ctx : nullptr;
    switch (m) {
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_dlg) { if (g_dlg->ctx) g_dlg->ctx->settings = nullptr; delete g_dlg; g_dlg = nullptr; }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_GETDLGCODE:
        return DLGC_WANTTAB | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (w == VK_TAB) {
            // simple tab order through the focusables we created
            if (g_dlg && g_dlg->order.size() > 1) {
                auto& v = g_dlg->order;
                size_t idx = 0;
                for (size_t i = 0; i < v.size(); ++i) if (GetFocus() == v[i]) { idx = i; break; }
                idx = (idx + 1) % v.size();
                SetFocus(v[idx]);
            }
            return 0;
        }
        break;
    case WM_COMMAND: {
        if (!cp) return 0;
        int id = LOWORD(w);
        if (id == IDOK || id == 3) {                    // OK / Apply
            PullFromDialog(*cp, h);
            SaveConfig(*cp);
            ApplyAll(*cp, true);
            if (id == IDOK) DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(h); return 0; }
        if (id == ID_BROWSE) {
            std::wstring p;
            if (PickFolder(*cp, h, p)) SetWindowTextW(GetDlgItem(h, ID_PATH), p.c_str());
            return 0;
        }
        if (id == ID_OPENFOLDER) {
            std::wstring folder = GetTxt(h, ID_PATH);
            if (!folder.empty())
                ShellExecuteW(h, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == ID_RESETPOS) {
            cp->cfg->have_rect = false;
            cp->cfg->half_screen = false;
            CheckDlgButton(h, ID_HALF, BST_UNCHECKED);
            RECT rc = DefaultRect(*cp);
            SetWindowPos(cp->widget, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate(*cp);
            return 0;
        }
        break;
    }
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void ShowSettingsDialogImpl(Ctx& c)
{
    if (c.settings && IsWindow(c.settings)) { SetForegroundWindow(c.settings); return; }
    g_dlg = new DlgState();
    g_dlg->ctx = &c;
    g_dlg->dpi = c.dpi ? c.dpi : 96;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = c.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DskvSettingsWnd";
    static bool registered = false;
    if (!registered) { RegisterClassW(&wc); registered = true; }

    HFONT font = UiFont2();
    const int CW = 470, CH = 520;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int wpx = S(c, CW), hpx = S(c, CH);
    int nc = S(c, 32);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, L"DskvSettingsWnd",
                             L"Desktop Photo Slideshow \u2014 settings",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             std::max(0, (sw - wpx) / 2), std::max(0, (sh - hpx) / 2),
                             wpx, hpx + nc, c.hidden, nullptr, c.inst, nullptr);
    if (!h) { delete g_dlg; g_dlg = nullptr; return; }
    g_dlg->h = h;
    c.settings = h;

    int y = 12;
    auto label = [&](const wchar_t* t, int x, int yy, int ww) {
        HWND hw = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                                  S(c, x), S(c, yy), S(c, ww), S(c, 18), h, nullptr, c.inst, nullptr);
        SendMessageW(hw, WM_SETFONT, WPARAM(font), TRUE);
    };
    auto sep = [&](const wchar_t* t) {
        label(t, 14, y, CW - 28);
        y += 22;
        HWND hw = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                                  S(c, 14), S(c, y), S(c, CW - 28), 2, h, nullptr, c.inst, nullptr);
        SendMessageW(hw, WM_SETFONT, WPARAM(font), TRUE);
        y += 10;
    };
    auto check = [&](const wchar_t* t, int x, int id) {
        Mk(c, h, L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, 170, 20, id, font);
    };
    sep(L"Folder");
    label(L"Pictures folder", 14, y + 2, 110);
    Mk(c, h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 130, y, 244, 22, ID_PATH, font);
    Mk(c, h, L"BUTTON", L"Browse\u2026", BS_PUSHBUTTON | WS_TABSTOP, 380, y, 76, 22, ID_BROWSE, font);
    y += 28;
    check(L"Include subfolders", 130, ID_RECURSIVE);
    check(L"Newest file first", 316, ID_SORTDESC);
    y += 24;
    check(L"Shuffle", 130, ID_SHUFFLE);
    check(L"Loop forever", 316, ID_REPEAT);
    y += 24;
    check(L"Fix EXIF rotation (photos sideways?)", 130, ID_EXIF);
    y += 26;

    sep(L"Timing");
    label(L"Seconds per photo", 14, y + 2, 110);
    Mk(c, h, L"EDIT", L"30", WS_TABSTOP | ES_NUMBER, 130, y, 56, 22, ID_INTERVAL, font);
    label(L"Crossfade ms", 208, y + 2, 70);
    Mk(c, h, L"EDIT", L"900", WS_TABSTOP | ES_NUMBER, 284, y, 56, 22, ID_TRANS, font);
    y += 28;
    check(L"Random interval", 130, ID_RANDI);
    label(L"jitter %", 316, y + 2, 50);
    Mk(c, h, L"EDIT", L"30", WS_TABSTOP | ES_NUMBER, 380, y, 56, 22, ID_JITTER, font);
    y += 26;

    sep(L"Motion and fit");
    label(L"Ken Burns motion", 14, y + 2, 110);
    HWND cbAnim = Mk(c, h, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 130, y, 140, 220, ID_ANIM, font);
    SendMessageW(cbAnim, CB_ADDSTRING, 0, LPARAM(L"none"));
    SendMessageW(cbAnim, CB_ADDSTRING, 0, LPARAM(L"zoom"));
    SendMessageW(cbAnim, CB_ADDSTRING, 0, LPARAM(L"pan"));
    label(L"amount 1.", 288, y + 2, 56);
    Mk(c, h, L"EDIT", L"1.10", WS_TABSTOP, 348, y, 40, 22, ID_ZOOM, font);
    label(L"fps", 398, y + 2, 22);
    Mk(c, h, L"EDIT", L"15", WS_TABSTOP | ES_NUMBER, 424, y, 34, 22, ID_FPS, font);
    y += 28;
    label(L"Image fit", 14, y + 2, 110);
    HWND cbFit = Mk(c, h, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 130, y, 140, 220, ID_FIT, font);
    SendMessageW(cbFit, CB_ADDSTRING, 0, LPARAM(L"cover (crop to fill)"));
    SendMessageW(cbFit, CB_ADDSTRING, 0, LPARAM(L"contain (show all)"));
    SendMessageW(cbFit, CB_ADDSTRING, 0, LPARAM(L"stretch"));
    check(L"Show file name", 316, ID_CAPTION);
    y += 24;
    label(L"Caption text", 14, y + 2, 110);
    Mk(c, h, L"EDIT", L"{name}  \u00b7  {i}/{n}", WS_TABSTOP | ES_AUTOHSCROLL, 130, y, 326, 22, ID_CAPFMT, font);
    y += 28;

    sep(L"Look");
    label(L"Corner radius", 14, y + 2, 110);
    Mk(c, h, L"EDIT", L"12", WS_TABSTOP | ES_NUMBER, 130, y, 44, 22, ID_CORNER, font);
    label(L"Shadow", 196, y + 2, 44);
    Mk(c, h, L"EDIT", L"18", WS_TABSTOP | ES_NUMBER, 244, y, 44, 22, ID_SHADOW, font);
    label(L"Border", 310, y + 2, 40);
    Mk(c, h, L"EDIT", L"1", WS_TABSTOP | ES_NUMBER, 354, y, 40, 22, ID_BORDER, font);
    check(L"Show grip", 404, ID_GRIP);
    y += 28;

    sep(L"Behaviour on the desktop");
    check(L"Click-through (mouse ignores it)", 130, ID_CLICK);
    check(L"Always on the desktop", 330, ID_BOTTOM);
    y += 24;
    check(L"Float above windows (topmost)", 130, ID_TOPMOST);
    check(L"Pause for fullscreen apps", 330, ID_PAUSEFS);
    y += 24;
    check(L"Show in taskbar", 130, ID_TASKBAR);
    check(L"Snap to half the screen", 330, ID_HALF);
    y += 24;
    check(L"Start with Windows", 130, ID_AUTOSTART);
    check(L"Edit mode (drag / resize)", 330, ID_EDITMODE);
    y += 24;
    check(L"Write slideshow.log", 130, ID_LOG);
    y += 26;

    int by = CH - 44;
    Mk(c, h, L"BUTTON", L"Open pictures folder", BS_PUSHBUTTON, 14, by, 150, 24, ID_OPENFOLDER, font);
    Mk(c, h, L"BUTTON", L"Reset position", BS_PUSHBUTTON, 172, by, 110, 24, ID_RESETPOS, font);
    Mk(c, h, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, CW - 246, by, 74, 24, IDOK, font);
    Mk(c, h, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, CW - 166, by, 74, 24, IDCANCEL, font);
    Mk(c, h, L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, CW - 86, by, 74, 24, 3, font);

    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(h, SW_SHOW);
    LoadIntoDialog(c, h);
    UpdateWindow(h);
    SetForegroundWindow(h);
    if (!g_dlg->order.empty()) SetFocus(g_dlg->order[0]);
}

// ======================================================== widget window =====
LRESULT CALLBACK WidgetProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Ctx* cp = (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!cp) {
        if (m == WM_NCCREATE) {
            cp = (Ctx*)((CREATESTRUCTW*)l)->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(cp));
        } else {
            return DefWindowProcW(h, m, w, l);
        }
    }
    if (!cp) return DefWindowProcW(h, m, w, l);
    Config& k = *cp->cfg;

    switch (m) {
    case WM_DPICHANGED: {
        cp->dpi = HIWORD(w);
        RECT* prc = (RECT*)l;
        if (prc)
            SetWindowPos(h, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        Invalidate(*cp);
        return 0;
    }
    case WM_WINDOWPOSCHANGED:
        UpdateDpi(*cp);
        Invalidate(*cp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        RenderFrame(*cp);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        POINT cl = pt;
        ScreenToClient(h, &cl);
        Layout L = CurrentLayout(*cp);
        RECT grip = L.grip, content = L.content;
        OffsetRect(&grip, -L.win.left, -L.win.top);
        OffsetRect(&content, -L.win.left, -L.win.top);
        bool inGrip = grip.right > grip.left && PtInRect(&grip, cl) != FALSE;
        bool inContent = L.haveContent && PtInRect(&content, cl) != FALSE;
        bool gripCorner = inGrip && cl.x >= L.grip.right - S(*cp, 12) && cl.y >= L.grip.bottom - S(*cp, 12);
        if (k.click_through && !cp->edit) {
            if (k.show_grip && inGrip) return gripCorner ? HTBOTTOMRIGHT : HTCAPTION;
            return HTTRANSPARENT;
        }
        if (gripCorner) return HTBOTTOMRIGHT;
        if (inGrip || inContent) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTBOTTOMRIGHT) { SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE)); return TRUE; }
        if (LOWORD(l) == HTCAPTION) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
        break;
    case WM_NCLBUTTONDOWN: {
        if (w == HTCAPTION || w == HTBOTTOMRIGHT) {
            cp->dragging = (w == HTCAPTION);
            cp->resizing = (w == HTBOTTOMRIGHT);
            GetCursorPos(&cp->dragStart);
            GetWindowRect(h, &cp->rectStart);
            SetCapture(h);
            return 0;
        }
        break;
    }
    case WM_NCLBUTTONDBLCLK:
        if (w == HTCAPTION) { SetEditImpl(*cp, !cp->edit); return 0; }
        break;
    case WM_NCRBUTTONUP: {
        if (k.click_through && !cp->edit) break;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ShowMenuAt(*cp, pt);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE: {
        if (cp->dragging || cp->resizing) {
            POINT cur{};
            if (m == WM_MOUSEMOVE) { cur.x = GET_X_LPARAM(l); cur.y = GET_Y_LPARAM(l); ClientToScreen(h, &cur); }
            else GetCursorPos(&cur);            // non-client drags only deliver WM_NCMOUSEMOVE
            int dx = cur.x - cp->dragStart.x, dy = cur.y - cp->dragStart.y;
            if (cp->dragging) {
                SetWindowPos(h, nullptr, cp->rectStart.left + dx, cp->rectStart.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                int ww = std::max(140, int(cp->rectStart.right - cp->rectStart.left) + dx);
                int hh = std::max(110, int(cp->rectStart.bottom - cp->rectStart.top) + dy);
                SetWindowPos(h, nullptr, cp->rectStart.left, cp->rectStart.top, ww, hh,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RenderFrame(*cp);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP:
        if (cp->dragging || cp->resizing) {
            cp->dragging = cp->resizing = false;
            ReleaseCapture();
            SaveConfig(*cp);
            Invalidate(*cp);
            return 0;
        }
        break;
    case WM_RBUTTONUP: {
        if (k.click_through && !cp->edit) break;
        POINT pt{};
        GetCursorPos(&pt);
        ShowMenuAt(*cp, pt);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        SetEditImpl(*cp, !cp->edit);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ====================================================== hidden window =======
void OnTimerTickImpl(Ctx& c, unsigned id)
{
    switch (id) {
    case DSKV_TIMER_STEP:
        if (!SlideAdvanceAllowed(c)) { StartTimerSchedule(c); return; }
        Next(c, true);
        break;
    case DSKV_TIMER_ANIM:
        if (c.hasFade) {
            unsigned long long el = GetTickCount64() - c.transStartMs;
            if (c.transMs == 0 || el >= c.transMs) {
                c.hasFade = false;
                FreeScaled(c.fadeOld);
                CommitPending(c);
                StartTimerSchedule(c);
            }
        }
        RenderFrame(c);
        if (c.cfg->cap_hover) UpdateHover(c);
        break;
    case DSKV_TIMER_BOTTOM:
        UpdateHover(c);
        EnforceBottom(c);
        if (c.list.empty()) {
            std::error_code ec;
            if (!c.cfg->path.empty() && std::filesystem::exists(c.cfg->path, ec)) {
                RebuildPlaylist(c);
                StartTimerSchedule(c);
                RenderFrame(c);
            }
        }
        break;
    case DSKV_TIMER_RETRY:
        KillTimer(c.hidden, DSKV_TIMER_RETRY);
        if (!c.trayReady) CreateTrayIcon(c);
        break;
    default: break;
    }
}


BOOL CALLBACK HiddenEnumQuitProc(HWND h, LPARAM lParam)
{
    wchar_t cls[64] = {};
    if (GetClassNameW(h, cls, 64) && _wcsicmp(cls, DSKV_HIDDEN_CLASS) == 0) {
        PostMessageW(h, static_cast<UINT>(lParam), 0, 0);
    }
    return TRUE;
}

LRESULT CALLBACK HiddenProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Ctx* cp = (Ctx*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_CREATE) {
        cp = (Ctx*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(cp));
    }
    if (!cp) return DefWindowProcW(h, m, w, l);
    Config& k = *cp->cfg;

    switch (m) {
    case WM_TIMER: OnTimerTickImpl(*cp, unsigned(w)); return 0;
    case WM_APP_SAVE: SaveConfig(*cp); return 0;
    case WM_APP_REMOTEQUIT: cp->quit = true; return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l)) {   // the shell sends the event id in LOWORD on modern Windows
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_CONTEXTMENU: {
            POINT pt{};
            GetCursorPos(&pt);
            ShowMenuAt(*cp, pt);
            break;
        }
        case WM_LBUTTONDBLCLK: SetEditImpl(*cp, !cp->edit); break;
        default: break;
        }
        return 0;
    case WM_APP_MENU: {
        POINT pt{};
        GetCursorPos(&pt);
        ShowMenuAt(*cp, pt);
        return 0;
    }
    case WM_APP_RELOAD: {
        bool e = cp->edit;
        LoadConfig(*cp);
        cp->edit = e;
        ApplyAll(*cp, true);
        return 0;
    }
    case WM_HOTKEY:
        switch (w) {
        case HOTK_EDIT: SetEditImpl(*cp, !cp->edit); break;
        case HOTK_PLAY: TogglePause(*cp); break;
        case HOTK_NEXT: GoTo(*cp, +1, true); break;
        case HOTK_PREV: GoTo(*cp, -1, true); break;
        default: break;
        }
        return 0;
    case WM_SETTINGCHANGE: UpdateDpi(*cp); Invalidate(*cp); return 0;
    case WM_DISPLAYCHANGE:
    case WM_DWMCOMPOSITIONCHANGED: {
        if (k.half_screen) {
            RECT rc = DefaultRect(*cp);
            SetWindowPos(cp->widget, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UpdateDpi(*cp);
        Invalidate(*cp);
        EnforceBottom(*cp);
        return 0;
    }
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK) { cp->locked = true; StartTimerSchedule(*cp); }
        else if (w == WTS_SESSION_UNLOCK) { cp->locked = false; StartTimerSchedule(*cp); }
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE || w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            cp->stepStartMs = GetTickCount64();
            StartTimerSchedule(*cp);
            RenderFrame(*cp);
        }
        return 0;
    case WM_DEVICECHANGE:
        if (cp->list.empty() || w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE) {
            RebuildPlaylist(*cp);
            StartTimerSchedule(*cp);
            RenderFrame(*cp);
        }
        return 0;
    case WM_CLOSE: cp->quit = true; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace
// ====================================================== public entry points ==
void ShowTrayBalloon(Ctx& c, const wchar_t* title, const wchar_t* text)
{
    (void)c;
    if (!g_nidOk) return;
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text, _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = 0;
}

bool SetAutostart(Ctx& c, bool on, bool silent)
{
    HKEY hk = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                             0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr);
    if (r != ERROR_SUCCESS) {
        if (!silent) MessageBoxW(c.hidden, L"Could not open the registry Run key.", DSKV_APP_NAME, MB_ICONWARNING);
        return false;
    }
    bool ok = false;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --startup";
        ok = RegSetValueExW(hk, DSKV_APP_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            DWORD((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = (RegDeleteValueW(hk, DSKV_APP_NAME) == ERROR_SUCCESS) ||
             (RegQueryValueExW(hk, DSKV_APP_NAME, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS);
    }
    RegCloseKey(hk);
    if (ok) c.cfg->auto_start = on;
    if (!ok && !silent)
        MessageBoxW(c.hidden, on ? L"Could not add the startup entry." : L"Could not remove the startup entry.",
                    DSKV_APP_NAME, MB_ICONWARNING);
    Log(c, L"autostart=" + std::wstring(on ? L"1" : L"0") + (ok ? L" ok" : L" FAILED"));
    return ok;
}


void ShowMenuAt(Ctx& c, POINT pt)
{
    Config& k = *c.cfg;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CM_PLAY, c.playing ? L"Pause" : L"Play");
    AppendMenuW(menu, MF_STRING, CM_PREV, L"Previous photo");
    AppendMenuW(menu, MF_STRING, CM_NEXT, L"Next photo");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU sub = CreatePopupMenu();
    for (int v : kIntervals) {
        wchar_t label[40];
        if (v < 60) swprintf(label, 40, L"%d seconds", v);
        else if (v < 3600) swprintf(label, 40, L"%d minutes", v / 60);
        else swprintf(label, 40, L"%d hours", v / 3600);
        AppendMenuW(sub, MF_STRING, UINT_PTR(CM_INTERVAL_BASE + v), label);
    }
    AppendMenuW(menu, MF_POPUP, UINT_PTR(sub), L"Time per photo");
    {
        int v = k.interval_s;
        UINT_PTR checkId = 0;
        for (int t : kIntervals) if (t == v) checkId = UINT_PTR(CM_INTERVAL_BASE + t);
        if (checkId) CheckMenuRadioItem(sub, CM_INTERVAL_BASE, CM_INTERVAL_BASE + 3600, checkId, MF_BYCOMMAND);
    }
    AppendMenuW(menu, MF_STRING, CM_FOLDER, L"Choose folder\u2026");
    AppendMenuW(menu, MF_STRING, CM_SETTINGS, L"Settings\u2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    auto ck = [&](const wchar_t* t, int id, bool on) {
        AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), UINT_PTR(id), t);
    };
    ck(L"Shuffle", CM_SHUFFLE, k.shuffle);
    ck(L"Loop forever", CM_REPEAT, k.repeat);
    ck(L"Edit mode (drag / resize)", CM_EDIT, c.edit);
    ck(L"Click-through (never steal clicks)", CM_CLICK, k.click_through);
    ck(L"Always on the desktop", CM_BOTTOM, k.auto_bottom);
    ck(L"Float above windows (topmost)", CM_TOPMOST, k.topmost);
    ck(L"Show file name", CM_CAPTION, k.caption);
    ck(L"Show grip handle", CM_GRIPOPT, k.show_grip);
    ck(L"Snap to half the screen", CM_HALF, k.half_screen);
    ck(L"Show in taskbar", CM_TASKBAR, k.show_in_taskbar);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ck(L"Start with Windows", CM_AUTOSTART, AutostartIsOnRaw());
    AppendMenuW(menu, MF_STRING, CM_OPENCFG, L"Edit slideshow.ini");
    AppendMenuW(menu, MF_STRING, CM_RELOAD, L"Reload slideshow.ini");
    AppendMenuW(menu, MF_STRING, CM_ABOUT, L"About\u2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CM_QUIT, L"Quit");

    SetForegroundWindow(c.hidden);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, c.hidden, nullptr);
    DestroyMenu(menu);
    if (!cmd) return;

    switch (cmd) {
    case CM_PLAY: TogglePause(c); break;
    case CM_NEXT: GoTo(c, +1, true); break;
    case CM_PREV: GoTo(c, -1, true); break;
    case CM_SHUFFLE: k.shuffle = !k.shuffle; SaveConfig(c); ApplyAll(c, true); break;
    case CM_REPEAT: k.repeat = !k.repeat; SaveConfig(c); StartTimerSchedule(c); break;
    case CM_EDIT: SetEditImpl(c, !c.edit); break;
    case CM_CLICK: k.click_through = !k.click_through; SaveConfig(c); ApplyWindowStyles(c); RenderFrame(c); break;
    case CM_BOTTOM: k.auto_bottom = !k.auto_bottom; SaveConfig(c); ApplyWindowStyles(c); StartTimerSchedule(c); break;
    case CM_TOPMOST: k.topmost = !k.topmost; SaveConfig(c); ApplyWindowStyles(c); StartTimerSchedule(c); break;
    case CM_CAPTION: k.caption = !k.caption; SaveConfig(c); RenderFrame(c); break;
    case CM_GRIPOPT: k.show_grip = !k.show_grip; SaveConfig(c); RenderFrame(c); break;
    case CM_TASKBAR: k.show_in_taskbar = !k.show_in_taskbar; SaveConfig(c); ApplyWindowStyles(c); break;
    case CM_HALF: {
        k.half_screen = !k.half_screen;
        RECT rc = DefaultRect(c);
        SetWindowPos(c.widget, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SaveConfig(c);
        Invalidate(c);
        break;
    }
    case CM_AUTOSTART: SetAutostart(c, !AutostartIsOnRaw(), false); break;
    case CM_FOLDER: {
        std::wstring p;
        if (PickFolder(c, c.hidden, p)) { k.path = p; SaveConfig(c); ApplyAll(c, true); }
        break;
    }
    case CM_SETTINGS: ShowSettingsDialogImpl(c); break;
    case CM_OPENCFG:
        ShellExecuteW(c.hidden, L"open", c.cfgPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case CM_RELOAD: {
        bool e = c.edit;
        LoadConfig(c);
        c.edit = e;
        ApplyAll(c, true);
        break;
    }
    case CM_ABOUT: {
        std::wstring txt = std::wstring(DSKV_APP_NAME) + L" " DSKV_VERSION_STR L"\r\n"
            L"Portable desktop widget for Windows 11 \u2014 no installer, no runtime.\r\n\r\n"
            L"Settings: " + c.cfgPath + L"\r\n"
            L"Folder: " + (k.path.empty() ? std::wstring(L"(none)") : k.path) + L"\r\n"
            L"Pictures found: " + Num(c.list.size()) + L"\r\n\r\n"
            L"Ctrl+Alt+W  edit mode \u00b7 Ctrl+Alt+P  play / pause \u00b7 Ctrl+Alt+\u2190 / \u2192  step\r\n"
            L"Right-click the widget (when click-through is off) or its grip handle for the menu.";
        MessageBoxW(c.hidden, txt.c_str(), L"About", MB_OK | MB_ICONINFORMATION);
        break;
    }
    case CM_QUIT: c.quit = true; break;
    default:
        if (cmd >= CM_INTERVAL_BASE) {
            int v = int(cmd - CM_INTERVAL_BASE);
            if (v >= 1) { k.interval_s = v; SaveConfig(c); ApplyAll(c, false); }
        }
        break;
    }
}


// "is a fullscreen game / presentation running?" - shared with render.cpp
bool FullscreenRunning()
{
    QUERY_USER_NOTIFICATION_STATE q = QUNS_NOT_PRESENT;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)))
        return q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE;
    return false;
}


void ShowSettingsDialog(Ctx& c) { ShowSettingsDialogImpl(c); }
void SetEdit(Ctx& c, bool on) { SetEditImpl(c, on); }
bool RegisterClasses(Ctx& c)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WidgetProc;
    wc.hInstance = c.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = DSKV_CLASS_NAME;
    if (!RegisterClassW(&wc)) return false;

    WNDCLASSW hc{};
    hc.lpfnWndProc = HiddenProc;
    hc.hInstance = c.inst;
    hc.lpszClassName = DSKV_HIDDEN_CLASS;
    if (!RegisterClassW(&hc)) return false;
    return true;
}

bool CreateWidget(Ctx& c)
{
    RECT rc = DefaultRect(c);
    c.widget = CreateWindowExW(WS_EX_LAYERED | (c.cfg->show_in_taskbar ? 0 : WS_EX_TOOLWINDOW),
                               DSKV_CLASS_NAME, L"Desktop Photo Slideshow", WS_POPUP,
                               rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                               nullptr, nullptr, c.inst, &c);
    if (!c.widget) return false;
    SetLayeredWindowAttributes(c.widget, 0, 255, LWA_ALPHA);
    UpdateDpi(c);
    ApplyWindowStyles(c);
    RenderFrame(c);
    ShowWindow(c.widget, SW_SHOWNOACTIVATE);
    EnforceBottom(c);
    return true;
}

bool CreateHiddenWindow(Ctx& c)
{
    c.hidden = CreateWindowExW(0, DSKV_HIDDEN_CLASS, L"Desktop Photo Slideshow", 0,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, c.inst, &c);
    return c.hidden != nullptr;
}

void FinishStartup(Ctx& c)
{
    g_inst = c.inst;
    if (c.cfg->auto_start != AutostartIsOnRaw()) { /* keep the file in sync, do not write */ }
    RebuildPlaylist(c);
    AdvanceStepTiming(c, false);
    CreateTrayIcon(c);
    if (!c.trayReady) SetTimer(c.hidden, DSKV_TIMER_RETRY, 1500, nullptr);
    RegisterHotkeys(c, c.cfg->hotkeys != L"none");
    c.userPlaying = c.playing;
    if (c.cfg->pause_on_lock)
        c.sessionNotify = WTSRegisterSessionNotification(c.hidden, NOTIFY_FOR_THIS_SESSION) != FALSE;
    StartTimerSchedule(c);
    RenderFrame(c);
    Log(c, L"started: " + Num(c.list.size()) + L" pictures, folder '" + c.cfg->path + L"'");
}

void Cleanup(Ctx& c)
{
    Log(c, L"stopping");
    if (c.sessionNotify) WTSUnRegisterSessionNotification(c.hidden);
    if (c.hidden) {
        KillTimer(c.hidden, DSKV_TIMER_STEP);
        KillTimer(c.hidden, DSKV_TIMER_ANIM);
        KillTimer(c.hidden, DSKV_TIMER_BOTTOM);
        KillTimer(c.hidden, DSKV_TIMER_RETRY);
    }
    if (c.cfg->edit) { c.cfg->edit = false; }
    SaveConfig(c);
    DestroyTrayIcon(c);
    UnregisterHotKey(c.hidden, HOTK_EDIT);
    UnregisterHotKey(c.hidden, HOTK_PLAY);
    UnregisterHotKey(c.hidden, HOTK_NEXT);
    UnregisterHotKey(c.hidden, HOTK_PREV);
    if (c.settings) DestroyWindow(c.settings);
    if (c.widget) DestroyWindow(c.widget);
    if (c.hidden) DestroyWindow(c.hidden);
    FreeScaled(c.cur);
    FreeScaled(c.fadeOld);
}

void BroadcastToInstances(UINT msg)
{
    EnumWindows(HiddenEnumQuitProc, LPARAM(msg));
}

} // namespace dskv