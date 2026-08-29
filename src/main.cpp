// main.cpp - entry point.  The whole app is one GUI thread: a message-only
// window holds the timers / tray icon / dialogs, a layered popup window is the
// widget itself.
#include "dskv_pch.h"
#include "config.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace dskv;

namespace {
HANDLE g_mutex = nullptr;
bool   g_amFirst = false;
}

// GDI+ is initialised in render.cpp, the icon lives in the exe resources.
namespace dskv {
bool LoadWidgetIcon(Ctx& c)
{
    // icon id 1 == IDI_ICON1 in app.rc
    c.icon = (HICON)LoadImageW(c.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!c.icon) c.icon = LoadIconW(nullptr, IDI_APPLICATION);
    return c.icon != nullptr;
}
} // namespace dskv

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = GetCommandLineW() ? CommandLineToArgvW(GetCommandLineW(), &argc) : nullptr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- single instance (a desktop widget must not double up) ----
    g_mutex = CreateMutexW(nullptr, TRUE, DSKV_MUTEX_NAME);
    g_amFirst = (GetLastError() != ERROR_ALREADY_EXISTS);
    if (!g_amFirst) {
        // another instance owns it: forward control verbs to it, then exit
        int dummy = 0;
        Ctx probe{};
        probe.inst = hInst;
        wchar_t tmp[MAX_PATH];
        GetModuleFileNameW(nullptr, tmp, MAX_PATH);
        probe.exeDir = std::filesystem::path(tmp).parent_path().wstring();
        static Config cfg;
        cfg.log = false;
        probe.cfg = &cfg;
        HandleCommandLine(probe, argc, argv, dummy);
        if (argc <= 1) {                       // plain double-click: show its menu
            BroadcastToInstances(WM_APP_MENU);
        }
        if (argv) LocalFree(argv);
        return 0;
    }

    Ctx c{};
    static Config cfg;
    c.cfg = &cfg;
    c.inst = hInst;
    wchar_t tmp[MAX_PATH * 8] = {};
    GetModuleFileNameW(nullptr, tmp, MAX_PATH * 8);
    c.exeDir = std::filesystem::path(tmp).parent_path().wstring();

    // -config must be known before the settings are read
    for (int i = 1; i + 1 < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-config" || a == L"--config") { cfg.ini_path = argv[i + 1]; break; }
    }
    LoadConfig(c);
    if (argc > 1) {
        int exitCode = 0;
        if (!HandleCommandLine(c, argc, argv, exitCode)) {
            if (argv) LocalFree(argv);
            return exitCode;
        }
    }
    if (argv) LocalFree(argv);

    if (!GdiplusInit()) {
        MessageBoxW(nullptr, L"GDI+ could not be started - the widget cannot draw anything.",
                    DSKV_APP_NAME, MB_ICONERROR);
        return 1;
    }
    LoadWidgetIcon(c);

    if (!RegisterClasses(c) || !CreateHiddenWindow(c)) {
        MessageBoxW(nullptr, L"Could not create the widget window (is another copy running?).",
                    DSKV_APP_NAME, MB_ICONERROR);
        return 1;
    }
    if (!CreateWidget(c)) {
        MessageBoxW(nullptr, L"Could not create the widget window.", DSKV_APP_NAME, MB_ICONERROR);
        return 1;
    }
    if (c.cfg->edit) SetEdit(c, true);
    FinishStartup(c);
    if (c.cfg->edit) ShowTrayBalloon(c, DSKV_APP_NAME,
        L"Edit mode: drag the picture to place it, use the corner grip to size it, then press Ctrl+Alt+W.");

    MSG msg{};
    while (!c.quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) { c.quit = true; break; }
        if (c.settings && IsWindow(c.settings)) {
            if (IsDialogMessageW(c.settings, &msg)) continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Cleanup(c);
    GdiplusShutdown2();
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
    CoUninitialize();
    return 0;
}
