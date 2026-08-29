// config.cpp - reads / writes slideshow.ini next to the .exe (portable by design)
//
// The .ini is written as UTF-16LE + BOM so that non-ASCII folder names
// (e.g. "C:\Users\مجید\Pictures\Vacation") survive round-trips.
#include "dskv_pch.h"
#include "config.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;

namespace dskv {
namespace {

HFONT g_uiFont = nullptr;

std::wstring Sq(const std::wstring& s) { return L"'" + s + L"'"; }

std::wstring TrimWs(std::wstring s)
{
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

std::wstring Unquote(std::wstring s)
{
    s = TrimWs(std::move(s));
    if (s.size() >= 2) {
        wchar_t a = s.front(), b = s.back();
        if ((a == L'"' && b == L'"') || (a == L'\'' && b == L'\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
    for (auto& ch : s)
        if (ch == L'/') ch = L'\\';
    return s;
}

// ---------- raw ini access (UTF-16 file) ----------
struct IniFile {
    std::wstring text;                       // full file contents (may be empty)
    std::vector<std::wstring> lines;
    std::wstring path;
    bool loaded = false;

    void Load(const std::wstring& p)
    {
        path = p;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > 4 * 1024 * 1024) { CloseHandle(h); return; }   // sanity limit
        std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
        DWORD got = 0;
        if (sz.QuadPart > 0)
            ReadFile(h, raw.data(), DWORD(raw.size()), &got, nullptr);
        CloseHandle(h);
        raw.resize(got);
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
            text.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        } else {
            // legacy / hand written ANSI (or UTF-8): convert once
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
            if (n > 0) {
                text.resize(n);
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), text.data(), n);
            } else {
                n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
                text.resize(n > 0 ? n : 0);
                if (n > 0) MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), text.data(), n);
            }
        }
        loaded = true;
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t e = text.find(L'\n', pos);
            if (e == std::wstring::npos) { lines.push_back(text.substr(pos)); break; }
            lines.push_back(text.substr(pos, e - pos));
            pos = e + 1;
        }
    }

    bool HasKey(const std::wstring& key) const
    {
        for (auto& l : lines) {
            std::wstring t = TrimWs(l);
            if (t.empty() || t[0] == L';' || t[0] == L'[') continue;
            size_t eq = t.find(L'=');
            if (eq == std::wstring::npos) continue;
            if (_wcsicmp(TrimWs(t.substr(0, eq)).c_str(), key.c_str()) == 0) return true;
        }
        return false;
    }

    std::wstring Get(const std::wstring& key, const std::wstring& def = L"") const
    {
        for (auto& l : lines) {
            std::wstring t = TrimWs(l);
            if (t.empty() || t[0] == L';' || t[0] == L'[') continue;
            size_t eq = t.find(L'=');
            if (eq == std::wstring::npos) continue;
            if (_wcsicmp(TrimWs(t.substr(0, eq)).c_str(), key.c_str()) == 0) {
                std::wstring v = TrimWs(t.substr(eq + 1));
                // strip an inline comment, but never a ';' that is part of the
                // value (the extensions list is ';' separated)
                size_t c = std::wstring::npos;
                for (size_t i = 1; i < v.size(); ++i) {
                    if (v[i] == L';' && (v[i - 1] == L' ' || v[i - 1] == L'\t')) { c = i; break; }
                }
                if (c != std::wstring::npos) v = v.substr(0, c);
                return Unquote(TrimWs(v));
            }
        }
        return def;
    }

    bool GetB(const std::wstring& k, bool def) const
    {
        std::wstring v = Get(k);
        if (v.empty()) return def;
        if (v == L"1" || _wcsicmp(v.c_str(), L"true") == 0 || _wcsicmp(v.c_str(), L"yes") == 0 ||
            _wcsicmp(v.c_str(), L"on") == 0) return true;
        if (v == L"0" || _wcsicmp(v.c_str(), L"false") == 0 || _wcsicmp(v.c_str(), L"no") == 0 ||
            _wcsicmp(v.c_str(), L"off") == 0) return false;
        return def;
    }
    long GetL(const std::wstring& k, long def) const
    {
        std::wstring v = Get(k);
        if (v.empty()) return def;
        wchar_t* endp = nullptr;
        long r = wcstol(v.c_str(), &endp, 10);
        return (endp && endp != v.c_str()) ? r : def;
    }
    double GetD(const std::wstring& k, double def) const
    {
        std::wstring v = Get(k);
        if (v.empty()) return def;
        wchar_t* endp = nullptr;
        double r = wcstod(v.c_str(), &endp);
        return (endp && endp != v.c_str()) ? r : def;
    }

    bool Write(Config& k, const std::wstring& header);

  private:
    static std::wstring Q(const std::wstring& key, const std::wstring& v)
    { return key + L" = " + Sq(v); }
};

std::wstring B(bool v) { return v ? L"1" : L"0"; }

bool IniFile::Write(Config& k, const std::wstring& header)
{
    std::wstring o;
    o += L"; ---------------------------------------------------------------\r\n";
    if (!header.empty()) {
        size_t p = 0;
        while ((p = header.find(L'\n', p)) != std::wstring::npos) { o += L"; "; o += header.substr(0, p + 1); p += 1; }
        if (!o.empty() && o.back() != L'\n') o += L"\r\n";
    }
    o += L"; ---------------------------------------------------------------\r\n";
    o += L"[\r\n";
    o += Q(L"path", k.path) + L"\r\n";
    o += L"; ---- images ----\r\n";
    o += Q(L"recursive", B(k.recursive)) + L"\r\n";
    o += Q(L"extensions", k.exts) + L"\r\n";
    o += Q(L"sort_desc", B(k.sort_desc)) + L"\r\n";
    o += Q(L"repeat", B(k.repeat)) + L"\r\n";
    o += Q(L"shuffle", B(k.shuffle)) + L"\r\n";
    o += Q(L"exif_auto_rotate", B(k.exif_auto_rotate)) + L"\r\n";
    o += Q(L"min_side", std::to_wstring(k.min_side)) + L"\r\n";
    o += L"; ---- timing ----\r\n";
    o += Q(L"interval_s", std::to_wstring(k.interval_s)) + L"\r\n";
    o += Q(L"random_interval", B(k.random_interval)) + L"\r\n";
    o += Q(L"random_jitter", std::to_wstring(k.random_jitter)) + L"\r\n";
    o += Q(L"transition_ms", std::to_wstring(k.transition_ms)) + L"\r\n";
    o += L"; ---- motion (Ken Burns) ----\r\n";
    o += Q(L"anim", k.anim) + L"\r\n";
    o += Q(L"anim_zoom", std::to_wstring(k.anim_zoom)) + L"\r\n";
    o += Q(L"anim_alt", B(k.anim_alt)) + L"\r\n";
    o += Q(L"anim_fps", std::to_wstring(k.anim_fps)) + L"\r\n";
    o += L"; ---- geometry & look ----\r\n";
    o += Q(L"half_screen", B(k.half_screen)) + L"\r\n";
    o += Q(L"left", std::to_wstring(k.left)) + L"\r\n";
    o += Q(L"top", std::to_wstring(k.top)) + L"\r\n";
    o += Q(L"width", std::to_wstring(k.width)) + L"\r\n";
    o += Q(L"height", std::to_wstring(k.height)) + L"\r\n";
    o += Q(L"corner", std::to_wstring(k.corner)) + L"\r\n";
    o += Q(L"shadow", std::to_wstring(k.shadow)) + L"\r\n";
    o += Q(L"border", std::to_wstring(k.border)) + L"\r\n";
    o += Q(L"border_alpha", std::to_wstring(k.border_a)) + L"\r\n";
    o += Q(L"bg", k.bg) + L"\r\n";
    {
        const wchar_t* fn = k.fit == Fit::Contain ? L"contain" : (k.fit == Fit::Stretch ? L"stretch" : L"cover");
        o += Q(L"fit", fn) + L"\r\n";
    }
    o += L"; ---- caption ----\r\n";
    o += Q(L"caption", B(k.caption)) + L"\r\n";
    o += Q(L"caption_fmt", k.cap_fmt) + L"\r\n";
    o += Q(L"caption_size", std::to_wstring(k.cap_size)) + L"\r\n";
    o += Q(L"caption_font", k.cap_font) + L"\r\n";
    o += Q(L"caption_h", std::to_wstring(k.cap_h)) + L"\r\n";
    o += Q(L"caption_alpha", std::to_wstring(k.cap_a)) + L"\r\n";
    o += Q(L"caption_hover_only", B(k.cap_hover)) + L"\r\n";
    o += L"; ---- behaviour ----\r\n";
    o += Q(L"click_through", B(k.click_through)) + L"\r\n";
    o += Q(L"auto_bottom", B(k.auto_bottom)) + L"\r\n";
    o += Q(L"topmost", B(k.topmost)) + L"\r\n";
    o += Q(L"bottom_check_ms", std::to_wstring(k.bottom_check_ms)) + L"\r\n";
    o += Q(L"pause_when_fullscreen", B(k.pause_fs)) + L"\r\n";
    o += Q(L"pause_on_lock", B(k.pause_on_lock)) + L"\r\n";
    o += Q(L"resume_after_pause", B(k.resume_after_pause)) + L"\r\n";
    o += Q(L"auto_start", B(k.auto_start)) + L"\r\n";
    o += Q(L"show_in_taskbar", B(k.show_in_taskbar)) + L"\r\n";
    o += Q(L"hotkeys", k.hotkeys) + L"\r\n";
    o += Q(L"log", B(k.log)) + L"\r\n";
    o += L"; ---- edit mode ----\r\n";
    o += Q(L"edit", B(k.edit)) + L"\r\n";
    o += Q(L"edit_border_alpha", std::to_wstring(k.edit_border_a)) + L"\r\n";
    o += Q(L"show_grip", B(k.show_grip)) + L"\r\n";
    o += Q(L"grip_size", std::to_wstring(k.grip_size)) + L"\r\n";

    std::string out;
    out.push_back((char)0xFF); out.push_back((char)0xFE);
    out.append(reinterpret_cast<const char*>(o.data()), o.size() * 2);

    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            BOOL ok = WriteFile(h, out.data(), DWORD(out.size()), &written, nullptr);
            CloseHandle(h);
            if (ok) return true;
        }
        Sleep(60);
    }
    return false;
}

// Known-folder lookup that does not depend on any single SDK's macro set.
std::wstring KnownFolder(const GUID* id, const wchar_t* envVar)
{
    wchar_t buf[MAX_PATH * 4] = {};
    if (id) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &p)) && p) {
            std::wstring r = p;
            CoTaskMemFree(p);
            if (!r.empty()) return r;
        }
    }
    if (envVar) {
        DWORD n = GetEnvironmentVariableW(envVar, buf, MAX_PATH * 4);
        if (n > 0 && n < MAX_PATH * 4) return buf;
    }
    return L"";
}

std::wstring GetKnownPictures()
{
    std::wstring r = KnownFolder(&FOLDERID_Pictures, L"USERPROFILE");
    if (r.empty()) return L"";
    std::wstring cand = r + L"\\Pictures";
    std::error_code ec;
    if (!r.empty() && r.find_last_of(L"\\/") + 1 == r.size()) cand = r + L"Pictures";
    if (std::filesystem::exists(cand, ec)) return cand;
    return r;
}

} // namespace

HFONT UiFont()
{
    if (g_uiFont) return g_uiFont;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        ncm.lfMessageFont.lfHeight = -12;
    g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    return g_uiFont;
}

void ConfigDefaults(Config& k)
{
    k = Config{};
    k.path = GetKnownPictures();
}

bool LoadConfig(Ctx& c)
{
    Config& k = *c.cfg;
    ConfigDefaults(k);

    std::wstring primary = c.cfg->ini_path;
    if (primary.empty()) primary = c.exeDir + L"\\slideshow.ini";
    else if (!std::filesystem::path(primary).is_absolute())
        primary = c.exeDir + L"\\" + primary;
    IniFile f;
    f.Load(primary);
    if (!f.loaded) {
        std::wstring appd = KnownFolder(&FOLDERID_RoamingAppData, nullptr);
        if (!appd.empty()) {
            std::wstring alt = appd + L"\\DesktopSlideshow\\slideshow.ini";
            std::error_code ec;
            if (std::filesystem::exists(alt, ec)) { f.Load(alt); if (f.loaded) primary = alt; }
        }
    }
    c.cfgPath = primary;
    c.cfgWritable = true;
    if (f.loaded) {
        DWORD attr = GetFileAttributesW(primary.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_READONLY))
            c.cfgWritable = false;
    }

    auto has = [&](const wchar_t* key) { return f.HasKey(key); };

    std::wstring p = f.Get(L"path");
    if (!p.empty()) k.path = p;
    if (k.path.empty()) k.path = GetKnownPictures();
    k.recursive        = f.GetB(L"recursive", k.recursive);
    {
        std::wstring e = f.Get(L"extensions");
        if (!e.empty()) k.exts = e;
    }
    k.sort_desc        = f.GetB(L"sort_desc", k.sort_desc);
    k.repeat           = f.GetB(L"repeat", k.repeat);
    k.shuffle          = f.GetB(L"shuffle", k.shuffle);
    k.exif_auto_rotate = f.GetB(L"exif_auto_rotate", k.exif_auto_rotate);
    k.min_side         = Clampi((int)f.GetL(L"min_side", k.min_side), 0, 4096);

    k.interval_s       = Clampi((int)f.GetL(L"interval_s", k.interval_s), 1, 86400);
    k.random_interval  = f.GetB(L"random_interval", k.random_interval);
    k.random_jitter    = Clampi((int)f.GetL(L"random_jitter", k.random_jitter), 0, 90);
    k.transition_ms    = Clampi((int)f.GetL(L"transition_ms", k.transition_ms), 0, 8000);

    {
        std::wstring a = f.Get(L"anim", k.anim);
        if (_wcsicmp(a.c_str(), L"none") == 0)        k.anim = L"none";
        else if (_wcsicmp(a.c_str(), L"pan") == 0)    k.anim = L"pan";
        else                                          k.anim = L"zoom";
    }
    k.anim_zoom        = Clampd(f.GetD(L"anim_zoom", k.anim_zoom), 1.0, 2.0);
    k.anim_alt         = f.GetB(L"anim_alt", k.anim_alt);
    k.anim_fps         = (int)Clampi((int)f.GetD(L"anim_fps", k.anim_fps), 0, 60);

    k.half_screen      = f.GetB(L"half_screen", k.half_screen);
    k.left             = (int)f.GetL(L"left", k.left);
    k.top              = (int)f.GetL(L"top", k.top);
    k.width            = (int)f.GetL(L"width", k.width);
    k.height           = (int)f.GetL(L"height", k.height);
    k.have_rect        = has(L"left") && has(L"width") && k.width > 40 && k.height > 40;
    k.corner           = Clampi((int)f.GetL(L"corner", k.corner), 0, 64);
    k.shadow           = Clampi((int)f.GetL(L"shadow", k.shadow), 0, 64);
    k.border           = Clampi((int)f.GetL(L"border", k.border), 0, 20);
    k.border_a         = Clampi((int)f.GetL(L"border_alpha", k.border_a), 0, 255);
    {
        std::wstring b = f.Get(L"bg", k.bg);
        if (!b.empty()) k.bg = b;
    }
    {
        std::wstring ft = f.Get(L"fit", L"cover");
        if (_wcsicmp(ft.c_str(), L"contain") == 0)      k.fit = Fit::Contain;
        else if (_wcsicmp(ft.c_str(), L"stretch") == 0) k.fit = Fit::Stretch;
        else                                            k.fit = Fit::Cover;
    }

    k.caption          = f.GetB(L"caption", k.caption);
    {
        std::wstring s = f.Get(L"caption_fmt", k.cap_fmt);
        if (!s.empty()) k.cap_fmt = s;
    }
    k.cap_size         = Clampi((int)f.GetL(L"caption_size", k.cap_size), 8, 72);
    {
        std::wstring s = f.Get(L"caption_font", k.cap_font);
        if (!s.empty()) k.cap_font = s;
    }
    k.cap_h            = Clampi((int)f.GetL(L"caption_h", k.cap_h), 0, 300);
    k.cap_a            = Clampi((int)f.GetL(L"caption_alpha", k.cap_a), 0, 255);
    k.cap_hover        = f.GetB(L"caption_hover_only", k.cap_hover);

    k.click_through    = f.GetB(L"click_through", k.click_through);
    k.auto_bottom      = f.GetB(L"auto_bottom", k.auto_bottom);
    k.topmost          = f.GetB(L"topmost", k.topmost);
    k.bottom_check_ms  = Clampi((int)f.GetL(L"bottom_check_ms", k.bottom_check_ms), 250, 60000);
    k.pause_fs         = f.GetB(L"pause_when_fullscreen", k.pause_fs);
    k.pause_on_lock    = f.GetB(L"pause_on_lock", k.pause_on_lock);
    k.resume_after_pause = f.GetB(L"resume_after_pause", k.resume_after_pause);
    k.auto_start       = f.GetB(L"auto_start", k.auto_start);
    k.show_in_taskbar  = f.GetB(L"show_in_taskbar", k.show_in_taskbar);
    {
        std::wstring h = f.Get(L"hotkeys", k.hotkeys);
        if (_wcsicmp(h.c_str(), L"none") == 0) k.hotkeys = L"none";
        else                                   k.hotkeys = L"auto";
    }
    k.log              = f.GetB(L"log", k.log);
    k.edit             = f.GetB(L"edit", k.edit);
    k.edit_border_a    = Clampi((int)f.GetL(L"edit_border_alpha", k.edit_border_a), 0, 255);
    k.show_grip        = f.GetB(L"show_grip", k.show_grip);
    k.grip_size        = Clampi((int)f.GetL(L"grip_size", k.grip_size), 14, 80);

    return f.loaded;
}

bool SaveConfig(Ctx& c)
{
    Config& k = *c.cfg;
    RECT rc{};
    if (c.widget && GetWindowRect(c.widget, &rc)) {
        k.left = rc.left; k.top = rc.top;
        k.width = rc.right - rc.left; k.height = rc.bottom - rc.top;
        k.have_rect = true;
    }
    IniFile f;
    f.path = c.cfgPath;
    static const std::wstring header =
        L" Desktop Photo Slideshow - settings for a Windows 11 desktop widget.\r\n"
        L" Save the file and the widget picks the changes up on the next tick\r\n"
        L" of its menu / tray actions (or restart it).\r\n";
    if (!f.Write(k, header)) { c.cfgWritable = false; return false; }
    return true;
}

} // namespace dskv
