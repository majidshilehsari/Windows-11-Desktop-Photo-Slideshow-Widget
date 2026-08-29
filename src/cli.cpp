// cli.cpp - command line handling: -quit / -save / -reload / -menu let a
// running (portable, single instance) widget be controlled from a script, and
// -edit / -startup / -path are used for first run and by the Run key.
#include "dskv_pch.h"
#include "config.h"

namespace dskv {
namespace {

bool Same(const std::wstring& a, const wchar_t* b)
{
    std::wstring t = b;
    for (auto& ch : t) ch = wchar_t(towlower(ch));
    std::wstring s = a;
    for (auto& ch : s) ch = wchar_t(towlower(ch));
    // tolerate both -flag and --flag
    while (!s.empty() && s.front() == L'-') s.erase(s.begin());
    while (!t.empty() && t.front() == L'-') t.erase(t.begin());
    return s == t;
}

} // namespace

bool HandleCommandLine(Ctx& c, int argc, wchar_t** argv, int& exitCode)
{
    // argv[0] is the exe
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (Same(a, L"-quit") || Same(a, L"--stop")) { BroadcastToInstances(WM_APP_REMOTEQUIT); exitCode = 0; return false; }
        if (Same(a, L"-save")) { BroadcastToInstances(WM_APP_SAVE); exitCode = 0; return false; }
        if (Same(a, L"-reload")) { BroadcastToInstances(WM_APP_RELOAD); exitCode = 0; return false; }
        if (Same(a, L"-menu")) { BroadcastToInstances(WM_APP_MENU); exitCode = 0; return false; }
        if (Same(a, L"-help") || Same(a, L"-h") || Same(a, L"--help")) {
            const wchar_t* txt =
                L"Desktop Photo Slideshow - a tiny portable photo widget for the Windows 11 desktop.\r\n\r\n"
                L"  (no arguments)        start / show the widget\r\n"
                L"  -edit                 start with the widget selected so you can move / resize it\r\n"
                L"  -startup              used by the \"Start with Windows\" entry (honours edit= in the .ini)\r\n"
                L"  -path \"C:\\My Photos\"  set the picture folder and save it\r\n"
                L"  -seconds 30           set the interval (seconds per photo) and save it\r\n"
                L"  -autostart 0|1        remove / add the startup entry\r\n"
                L"  -reload               tell a running instance to re-read slideshow.ini\r\n"
                L"  -save                 tell a running instance to store its current position\r\n"
                L"  -menu                 pop up the widget menu (useful from a shortcut / PowerToys)\r\n"
                L"  -quit                 ask the running instance to exit\r\n\r\n"
                L"Settings file: <folder of the .exe>\\slideshow.ini";
            MessageBoxW(nullptr, txt, DSKV_APP_NAME, MB_OK | MB_ICONINFORMATION);
            exitCode = 0;
            return false;
        }
        if (Same(a, L"-config") && i + 1 < argc) { c.cfg->ini_path = argv[++i]; SaveConfig(c); continue; }
        if (Same(a, L"-edit")) { c.cfg->edit = true; c.edit = true; continue; }
        if (Same(a, L"-startup") || Same(a, L"--runatstartup")) { c.cfg->edit = false; continue; }
        if (Same(a, L"-path") && i + 1 < argc) {
            std::wstring p = argv[++i];
            for (auto& ch : p) if (ch == L'/') ch = L'\\';
            c.cfg->path = p;
            c.cfg->have_rect = c.cfg->have_rect;   // keep position
            SaveConfig(c);
            continue;
        }
        if (Same(a, L"-seconds") && i + 1 < argc) {
            c.cfg->interval_s = Clampi(_wtoi(argv[++i]), 1, 86400);
            SaveConfig(c);
            continue;
        }
        if (Same(a, L"-autostart") && i + 1 < argc) {
            std::wstring v = argv[++i];
            bool on = (v == L"1" || _wcsicmp(v.c_str(), L"true") == 0 || _wcsicmp(v.c_str(), L"on") == 0);
            SetAutostart(c, on, false);
            SaveConfig(c);
            exitCode = 0;
            return false;
        }
        // unknown switch: ignore, never fail the start of a desktop widget
    }
    exitCode = 0;
    return true;    // keep going and show the widget
}

} // namespace dskv
