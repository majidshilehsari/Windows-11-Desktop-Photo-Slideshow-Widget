// dskv_pch.h - shared includes / global declarations for
// "Desktop Photo Slideshow" - a tiny, portable Windows 11 desktop widget.
#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <shcore.h>
#include <shellscalingapi.h>   // GetDpiForMonitor (mingw-w64 declares it here)
#include <dbt.h>

// a couple of SDK revisions ship the header without the enum
#ifndef MDT_EFFECTIVE_DPI
#define MDT_EFFECTIVE_DPI 0
#endif

#include <gdiplus.h>

#include <string>
#include <cwchar>
#include <vector>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "gdiplus.lib")

// ---------- custom window messages ----------
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_REMOTEQUIT  (WM_APP + 2)
#define WM_APP_MENU        (WM_APP + 3)
#define WM_APP_RELOAD      (WM_APP + 4)
#define WM_APP_SAVE        (WM_APP + 5)

#define DSKV_CLASS_NAME    L"DskvSlideshowWidgetWnd"
#define DSKV_HIDDEN_CLASS  L"DskvSlideshowHiddenWnd"
#define DSKV_MUTEX_NAME    L"Local\\DskvDesktopSlideshow.SingleInstance"
#define DSKV_APP_NAME      L"Desktop Photo Slideshow"
#define DSKV_INI_CANDIDATES 2

// ---------- tunables ----------
#ifndef DSKV_VERSION_STR
#define DSKV_VERSION_STR L"0.1.0"
#endif
#define DSKV_TIMER_STEP     1001
#define DSKV_TIMER_ANIM     1002
#define DSKV_TIMER_BOTTOM   1003
#define DSKV_TIMER_RETRY    1004

#define DSKV_IDC_BASE       1000
#define DSKV_ID_OK          1
#define DSKV_ID_CANCEL      2
#define DSKV_ID_APPLY       3

namespace dskv {

struct Config;

struct ScaledImage {
    std::wstring        path;
    std::wstring        sig;               // layout signature the cached bitmap was built for
    SIZE                srcSize{0, 0};     // decoded (already EXIF-rotated) size
    SIZE                boxSize{0, 0};     // size of `bmp` in pixels
    Gdiplus::Bitmap*    bmp = nullptr;     // pre-scaled, deleted by owner
    bool                coverFill = true;  // true: crop-to-fill, false: fit-inside
    double              baseScale = 1.0;   // scale that exactly covers `boxSize`
};

struct Ctx {
    HWND      hidden  = nullptr;   // message-only window: timers, tray, dialogs
    HWND      widget  = nullptr;   // the layered widget window itself
    HICON     icon    = nullptr;
    HINSTANCE inst    = nullptr;
    Config*   cfg     = nullptr;

    std::vector<std::wstring> list;      // playlist of image files
    size_t      index     = 0;           // index inside `list`
    bool        playing   = true;

    // transition / animation state
    unsigned long long stepStartMs = 0;  // GetTickCount64() of the current slide
    unsigned long long transStartMs = 0; // GetTickCount64() of the crossfade
    unsigned    transMs   = 0;           // remaining crossfade length (0 = none)
    unsigned    stepMs    = 0;           // resolved length of the current slide
    bool        zoomOut   = false;       // direction of the current zoom step
    // pending values for the *incoming* slide (applied when the crossfade ends,
    // so the outgoing picture can finish its Ken-Burns move on screen)
    bool        havePend  = false;
    unsigned    pStepMs   = 0;
    unsigned long long pStepStart = 0;
    bool        pZoomOut  = false;
    bool        pendingStopHere = false;   // stop advancing after the fade
    bool        hasFade   = false;
    ScaledImage cur;                     // pre-scaled current picture
    ScaledImage fadeOld;                 // pre-scaled outgoing picture (crossfade)
    HWND        settings = nullptr;      // settings dialog, if open

    // edit (drag / resize) state
    bool        dragging   = false;
    bool        resizing   = false;
    POINT       dragStart{};
    RECT        rectStart{};

    bool        quit       = false;
    bool        edit       = false;      // edit mode active (interactive, visible border)
    bool        trayReady  = false;
    bool        sessionNotify = false;
    bool        hovering  = false;
    bool        locked    = false;   // session is locked
    bool        userPlaying = true;  // what the user last asked for
    std::wstring cfgPath;                // where the .ini lives
    std::wstring exeDir;
    bool        cfgWritable = true;
    int         dpi         = 96;
};

// --- config (config.cpp) ---
bool LoadConfig(Ctx& c);
bool SaveConfig(Ctx& c);

// --- render helpers (render.cpp) ---
bool GdiplusInit();
void GdiplusShutdown2();
HFONT UiFont2();      // the shell UI font, shared by the dialog + the edit bar

// --- images (images.cpp) ---
void RebuildPlaylist(Ctx& c);
std::wstring BuildCaption(Ctx& c, const std::wstring& file);
bool GetScaled(Ctx& c, size_t listIndex, ScaledImage& out);
void FreeScaled(ScaledImage& s);

// --- layout (render.cpp) ---
struct Layout {
    RECT win{};        // whole window rect (screen px, includes the shadow band)
    RECT content{};    // picture area
    RECT clip{};       // rounded picture area (clipping)
    RECT scrim{};      // caption strip
    RECT grip{};       // interactive handle
    RECT bar{};        // edit-mode info bar
    int  radius = 0;
    int  shadow = 0;
    bool haveContent = false;
};
Layout LayoutOf(Ctx& c, const RECT& win);
Layout CurrentLayout(Ctx& c);

inline double Clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int  Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int  ScaleForDpi(int px, int dpi) { return MulDiv(px, dpi ? dpi : 96, 96); }

// --- render (render.cpp) ---
void RenderFrame(Ctx& c);          // draws the current state and pushes it on screen
void Invalidate(Ctx& c);           // re-evaluate the timers + force one render

// --- app / window (app.cpp) ---
bool RegisterClasses(Ctx& c);
bool CreateWidget(Ctx& c);
bool CreateHiddenWindow(Ctx& c);
void FinishStartup(Ctx& c);
void Cleanup(Ctx& c);
void ShowTrayBalloon(Ctx& c, const wchar_t* title, const wchar_t* text);
void ShowMenuAt(Ctx& c, POINT pt);
void ShowSettingsDialog(Ctx& c);
void BroadcastToInstances(unsigned msg);
bool FullscreenRunning();
bool SetAutostart(Ctx& c, bool on, bool silent);   // HKCU Run key
void SetEdit(Ctx& c, bool on);

// --- cli.cpp ---
bool HandleCommandLine(Ctx& c, int argc, wchar_t** argv, int& exitCode);

} // namespace dskv
