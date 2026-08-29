// render.cpp - paints the widget into a 32bpp premultiplied DIB and pushes it
// to a layered window with UpdateLayeredWindow().  This is the technique real
// desktop widgets (Rainmeter, Wallpaper Engine overlays) use: rounded corners,
// soft shadow, smooth crossfades and honest per-pixel transparency - no colour
// key rectangles, no flicker, and the desktop keeps showing through.
#include "dskv_pch.h"
#include "config.h"
#include <algorithm>

using namespace Gdiplus;

namespace dskv {
namespace {

ULONG_PTR g_gdipToken = 0;
HFONT g_font = nullptr;

double EaseInOut(double t)
{
    t = Clampd(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Color Argb(int a, int r, int g, int b)
{
    return Color(BYTE(Clampi(a, 0, 255)), BYTE(r), BYTE(g), BYTE(b));
}

bool ParseHexColor(const std::wstring& s, Color& out)
{
    std::wstring t = s;
    while (!t.empty() && (t.front() == L' ' || t.front() == L'#')) t.erase(t.begin());
    if (t.size() != 6) return false;
    auto dig = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        ch = wchar_t(towlower(ch));
        if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
        return -1;
    };
    int v[3];
    for (int i = 0; i < 3; ++i) {
        int a = dig(t[i * 2]), b = dig(t[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        v[i] = a * 16 + b;
    }
    out = Argb(255, v[0], v[1], v[2]);
    return true;
}

void AddRoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    if (r < 0.5f || w < 2 * r || h < 2 * r) { p.AddRectangle(RectF(x, y, w, h)); return; }
    REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void DrawShadow(Graphics& g, const RECT& clip, int shadow, int corner)
{
    if (shadow <= 0) return;
    for (int i = shadow; i >= 1; --i) {
        REAL grow = REAL(i);
        int a = int(14.0 * (1.0 - (i - 1.0) / shadow));
        if (a <= 0) continue;
        GraphicsPath p;
        AddRoundRect(p, REAL(clip.left) - grow, REAL(clip.top) - grow,
                     REAL(clip.right - clip.left) + 2 * grow,
                     REAL(clip.bottom - clip.top) + 2 * grow,
                     REAL(corner) + grow);
        SolidBrush b(Argb(a, 0, 0, 0));
        g.FillPath(&b, &p);
    }
}

// Draw `img` (pre-scaled, `boxSize` = content size * zoomMax) into `dst`,
// cropping a window of it: zoom = 1 shows the whole bitmap (that is exactly
// the "fit" view), zoom = zoomMax crops to the content box again and the focus
// point slides - which is the Ken Burns move, with zero per-frame resampling.
void DrawScaled(Graphics& g, const ScaledImage& img, const RECT& dst, double zoom, double focusX, double focusY)
{
    if (!img.bmp) return;
    double W = double(dst.right - dst.left), H = double(dst.bottom - dst.top);
    if (W < 1 || H < 1) return;
    zoom = Clampd(zoom, 1.0, 4.0);

    double sw = double(img.boxSize.cx) / zoom;
    double sh = double(img.boxSize.cy) / zoom;
    double ox = (double(img.boxSize.cx) - sw) * Clampd(focusX, 0.0, 1.0);
    double oy = (double(img.boxSize.cy) - sh) * Clampd(focusY, 0.0, 1.0);
    ox = Clampd(ox, 0.0, std::max(0.0, double(img.boxSize.cx) - 1.0));
    oy = Clampd(oy, 0.0, std::max(0.0, double(img.boxSize.cy) - 1.0));
    sw = Clampd(sw, 1.0, double(img.boxSize.cx) - ox);
    sh = Clampd(sh, 1.0, double(img.boxSize.cy) - oy);

    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    RectF d(REAL(dst.left), REAL(dst.top), REAL(W), REAL(H));
    g.DrawImage(img.bmp, d, REAL(ox), REAL(oy), REAL(sw), REAL(sh), UnitPixel);
}

// how far along the Ken-Burns move the current slide is (0..1)
double SlideProgress(Ctx& c, bool incoming)
{
    Config& k = *c.cfg;
    if (k.anim == L"none") return 1.0;
    // the incoming slide animates from the moment its step began; the outgoing
    // slide (still visible during the crossfade) keeps animating from its own
    // step start so its Ken-Burns move finishes on screen instead of snapping.
    unsigned long long start = c.stepStartMs;
    unsigned long long now = GetTickCount64();
    if (now < start) now = start;
    double len = double(c.stepMs ? c.stepMs : unsigned(k.interval_s * 1000));
    if (c.hasFade && !incoming) {
        double tl = double(c.transMs ? c.transMs : 1u);
        double fp = Clampd(double(now - c.transStartMs) / tl, 0.0, 1.0);
        return c.zoomOut ? (1.0 - fp) : 1.0;   // outgoing slide finishes its move
    }
    double p = Clampd(double(now - start) / len, 0.0, 1.0);
    return c.zoomOut ? (1.0 - p) : p;
}

bool AnimOn(Ctx& c) { return c.cfg->anim != L"none"; }

void DrawImageToContent(Graphics& g, Ctx& c, ScaledImage& img, const RECT& content, bool incoming)
{
    Config& k = *c.cfg;
    double p = Clampd(SlideProgress(c, incoming), 0.0, 1.0);
    double zoom = 1.0;
    double fx = 0.5, fy = 0.5;
    if (AnimOn(c)) {
        double zmax = Clampd(k.anim_zoom, 1.0, 2.0);
        // p = 0 -> the whole pre-scaled bitmap is visible (the "fit" view)
        // p = 1 -> cropped down to the exact content box, i.e. zoomed by zmax
        zoom = 1.0 + (zmax - 1.0) * EaseInOut(p);
        if (k.anim == L"pan") {
            fx = Clampd(0.15 + 0.7 * EaseInOut(p), 0.0, 1.0);
            fy = Clampd(0.35 + 0.3 * EaseInOut(p), 0.0, 1.0);
        } else {
            fx = 0.5;
            fy = 0.5;
        }
    }
    DrawScaled(g, img, content, zoom, fx, fy);
}

void DrawCaption(Graphics& g, Ctx& c, const Layout& L)
{
    Config& k = *c.cfg;
    if (!k.caption || k.cap_h <= 0 || L.scrim.bottom <= L.scrim.top) return;
    if (k.cap_hover && !c.edit && !c.hovering) return;      // only on hover
    RECT s = L.scrim;
    float capH = float(s.bottom - s.top);

    RectF r(REAL(s.left), REAL(s.top), REAL(std::max(1, k.cap_h * 8)), capH);
    LinearGradientBrush lb(r, Argb(0, 0, 0, 0), Argb(170, 0, 0, 0), LinearGradientModeVertical);
    RectF real(REAL(s.left), REAL(s.top), REAL(s.right - s.left), capH);
    g.FillRectangle(&lb, real);

    std::wstring text = c.list.empty() ? std::wstring() : BuildCaption(c, c.list[c.index]);
    if (text.empty()) return;

    FontFamily ff(k.cap_font.c_str());
    Font font(&ff, REAL(std::max(9, k.cap_size)), FontStyleRegular, UnitPixel);
    if (font.GetLastStatus() != Ok) {
        FontFamily fb(L"Segoe UI");
        Font f2(&fb, REAL(std::max(9, k.cap_size)), FontStyleRegular, UnitPixel);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        StringFormat sf;
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetFormatFlags(StringFormatFlagsNoWrap);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);
        RectF box(REAL(s.left + 14), REAL(s.top), REAL(s.right - s.left - 28), capH);
        SolidBrush sh(Argb(150, 0, 0, 0)), fg(Argb(k.cap_a, 255, 255, 255));
        RectF box2 = box; box2.X += 1; box2.Y += 1;
        g.DrawString(text.c_str(), -1, &f2, box2, &sf, &sh);
        g.DrawString(text.c_str(), -1, &f2, box, &sf, &fg);
        return;
    }
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF box(REAL(s.left + 14), REAL(s.top), REAL(s.right - s.left - 28), capH);
    SolidBrush sh(Argb(150, 0, 0, 0)), fg(Argb(k.cap_a, 255, 255, 255));
    RectF box2 = box; box2.X += 1; box2.Y += 1;
    g.DrawString(text.c_str(), -1, &font, box2, &sf, &sh);
    g.DrawString(text.c_str(), -1, &font, box, &sf, &fg);
}

void DrawGrip(Graphics& g, Ctx& c, const Layout& L)
{
    Config& k = *c.cfg;
    bool show = k.show_grip || c.edit;
    if (!show || L.grip.right <= L.grip.left) return;
    RECT r = L.grip;
    BYTE a = c.edit ? 230 : 130;
    GraphicsPath p;
    AddRoundRect(p, REAL(r.left), REAL(r.top), REAL(r.right - r.left), REAL(r.bottom - r.top),
                 REAL(ScaleForDpi(7, c.dpi)));
    SolidBrush bg(Argb(c.edit ? 200 : 110, 10, 14, 20));
    g.FillPath(&bg, &p);
    Pen pen(Argb(a, 255, 255, 255), 1.0f);
    g.DrawPath(&pen, &p);
    int step = std::max(3, static_cast<int>(r.right - r.left) / 4);
    for (int i = 1; i <= 3; ++i) {
        int x = r.right - i * step;
        int y = r.bottom - i * step;
        g.DrawLine(&pen, REAL(x), REAL(r.bottom - ScaleForDpi(5, c.dpi)),
                   REAL(r.right - ScaleForDpi(5, c.dpi)), REAL(y));
    }
}

void DrawEditOverlay(Graphics& g, Ctx& c, const Layout& L)
{
    Config& k = *c.cfg;
    RECT content = L.content;
    Pen pen(Argb(k.edit_border_a, 0, 157, 255), 2.0f);
    GraphicsPath p;
    AddRoundRect(p, REAL(content.left) + 1, REAL(content.top) + 1,
                 REAL(content.right - content.left) - 2, REAL(content.bottom - content.top) - 2,
                 REAL(std::max(0, L.radius - 1)));
    g.DrawPath(&pen, &p);

    if (L.bar.bottom <= L.bar.top) return;
    SolidBrush b(Argb(210, 8, 12, 18));
    g.FillRectangle(&b, REAL(L.bar.left), REAL(L.bar.top),
                    REAL(L.bar.right - L.bar.left), REAL(L.bar.bottom - L.bar.top));

    wchar_t buf[200];
    swprintf(buf, 200, L"Edit mode \u2014 drag anywhere to move, the corner grip to resize \u2014 %d \u00d7 %d px",
             int(L.content.right - L.content.left), int(L.content.bottom - L.content.top));
    HDC hdc = g.GetHDC();
    if (hdc) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HFONT old = (HFONT)SelectObject(hdc, UiFont2());
        RECT rc{ L.bar.left + 10, L.bar.top, L.bar.right - 8, L.bar.bottom };
        DrawTextW(hdc, buf, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, old);
        g.ReleaseHDC(hdc);
    }
}

} // namespace

// --------------------------------------------------------------- shared ----
HFONT UiFont2()
{
    if (!g_font) {
        LOGFONTW lf{};
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) lf = ncm.lfMessageFont;
        if (!lf.lfHeight) lf.lfHeight = -12;
        if (!lf.lfFaceName[0]) wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_font = CreateFontIndirectW(&lf);
    }
    return g_font;
}

bool GdiplusInit()
{
    if (g_gdipToken) return true;
    GdiplusStartupInput in;      // version 1, no debug callbacks
    return GdiplusStartup(&g_gdipToken, &in, nullptr) == Ok;
}

void GdiplusShutdown2()
{
    if (g_gdipToken) { GdiplusShutdown(g_gdipToken); g_gdipToken = 0; }
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
}

// ---------------------------------------------------------------- layout ----
Layout LayoutOf(Ctx& c, const RECT& win)
{
    Config& k = *c.cfg;
    Layout L;
    L.win = win;
    L.shadow = k.shadow;
    L.radius = k.corner;
    int pad = k.shadow;
    RECT content{ win.left + pad, win.top + pad, win.right - pad, win.bottom - pad };
    if (content.right - content.left < 24 || content.bottom - content.top < 24) return L;

    L.haveContent = true;
    L.content = content;
    L.clip = content;
    int capH = (k.caption && k.cap_h > 0) ? ScaleForDpi(k.cap_h, c.dpi) : 0;
    L.scrim = { content.left, content.bottom - capH, content.right, content.bottom };

    int gs = ScaleForDpi(k.grip_size, c.dpi);
    int gm = ScaleForDpi(8, c.dpi);
    if (content.right - content.left > gs * 3 && content.bottom - content.top > gs * 3)
        L.grip = { content.right - gs - gm, content.bottom - gs - gm, content.right - gm, content.bottom - gm };

    int bh = ScaleForDpi(26, c.dpi);
    L.bar = { content.left, content.top, content.right, content.top + bh };
    return L;
}

Layout CurrentLayout(Ctx& c)
{
    RECT rc{};
    if (c.widget) GetWindowRect(c.widget, &rc);
    return LayoutOf(c, rc);
}

// LayoutOf works in screen coordinates (that is what the hit test needs); the
// paint surface is window-relative, so shift everything once for drawing.
static Layout ToWindowSpace(Layout L)
{
    LONG dx = -L.win.left, dy = -L.win.top;
    OffsetRect(&L.content, dx, dy);
    OffsetRect(&L.clip, dx, dy);
    OffsetRect(&L.scrim, dx, dy);
    OffsetRect(&L.bar, dx, dy);
    OffsetRect(&L.grip, dx, dy);
    OffsetRect(&L.win, dx, dy);
    return L;
}

// ---------------------------------------------------------------- render ----
void RenderFrame(Ctx& c)
{
    Config& k = *c.cfg;
    if (!c.widget) return;
    RECT win{};
    if (!GetWindowRect(c.widget, &win)) return;
    int W = win.right - win.left, H = win.bottom - win.top;
    if (W < 8 || H < 8) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);

    {
        Bitmap surf(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&surf);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(0, 0, 0, 0));
        g.SetCompositingMode(CompositingModeSourceOver);

        Layout L = ToWindowSpace(LayoutOf(c, win));
        if (!L.haveContent) {
            SolidBrush b(Argb(150, 8, 10, 14));
            g.FillRectangle(&b, 0, 0, W, H);
        } else {
            DrawShadow(g, L.clip, k.shadow, k.corner);

            GraphicsPath clipPath;
            AddRoundRect(clipPath, REAL(L.clip.left), REAL(L.clip.top),
                         REAL(L.clip.right - L.clip.left), REAL(L.clip.bottom - L.clip.top),
                         REAL(L.radius));
            Region region(&clipPath);
            g.SetClip(&region, CombineModeReplace);

            Color bg;
            if (!ParseHexColor(k.bg, bg)) bg = Argb(255, 11, 15, 20);
            SolidBrush bgBrush(bg);
            g.FillRectangle(&bgBrush, REAL(L.content.left), REAL(L.content.top),
                            REAL(L.content.right - L.content.left),
                            REAL(L.content.bottom - L.content.top));

            bool failed = c.list.empty();
            if (!failed && !GetScaled(c, c.index, c.cur)) failed = true;

            if (failed) {
                FontFamily ff(L"Segoe UI");
                Font font(&ff, REAL(ScaleForDpi(14, c.dpi)), FontStyleRegular, UnitPixel);
                SolidBrush tb(Argb(235, 240, 240, 240));
                RectF r(REAL(L.content.left + 18), REAL(L.content.top),
                        REAL(L.content.right - L.content.left - 36),
                        REAL(L.content.bottom - L.content.top));
                StringFormat sf;
                sf.SetAlignment(StringAlignmentCenter);
                sf.SetLineAlignment(StringAlignmentCenter);
                const wchar_t* msg = k.path.empty()
                    ? L"No folder configured.\r\nOpen the tray menu and choose a folder."
                    : L"No pictures found here.\r\nCheck the folder path and the extensions list in slideshow.ini.";
                g.DrawString(msg, -1, &font, r, &sf, &tb);
            } else {
                bool fading = c.hasFade && c.transMs > 0;
                double ft = 1.0;
                if (fading) {
                    ft = 1.0 - EaseInOut(double(GetTickCount64() - c.transStartMs) / c.transMs);
                    if (ft <= 0.0) { fading = false; c.hasFade = false; FreeScaled(c.fadeOld); }
                } else if (c.fadeOld.bmp) {
                    FreeScaled(c.fadeOld);
                }
                DrawImageToContent(g, c, c.cur, L.content, false);
                if (fading && c.fadeOld.bmp) {
                    RECT local{ 0, 0, int(L.content.right - L.content.left),
                               int(L.content.bottom - L.content.top) };
                    Bitmap layer(int(local.right), int(local.bottom), PixelFormat32bppPARGB);
                    if (layer.GetLastStatus() == Ok) {
                        Graphics lg(&layer);
                        lg.SetSmoothingMode(SmoothingModeNone);
                        lg.SetCompositingMode(CompositingModeSourceCopy);
                        lg.Clear(Color(0, 0, 0, 0));
                        lg.SetCompositingMode(CompositingModeSourceOver);
                        DrawImageToContent(lg, c, c.fadeOld, local, true);
                        ImageAttributes at;
                        ColorMatrix cm{};
                        cm.m[0][0] = 1; cm.m[1][1] = 1; cm.m[2][2] = 1;
                        cm.m[3][3] = REAL(ft); cm.m[4][4] = 0;
                        at.SetColorMatrix(&cm, static_cast<ColorMatrixFlags>(0), static_cast<ColorAdjustType>(1));
                        g.DrawImage(&layer, RectF(REAL(L.content.left), REAL(L.content.top),
                                                  REAL(local.right), REAL(local.bottom)),
                                    0, 0, int(local.right), int(local.bottom), UnitPixel, &at);
                    }
                }
                DrawCaption(g, c, L);
            }

            g.ResetClip();
            if (k.border > 0) {
                float t = float(std::max(1, k.border));
                Pen bp(Argb(k.border_a, 255, 255, 255), t);
                GraphicsPath pp;
                AddRoundRect(pp, REAL(L.clip.left) + t / 2, REAL(L.clip.top) + t / 2,
                             REAL(L.clip.right - L.clip.left) - t, REAL(L.clip.bottom - L.clip.top) - t,
                             REAL(std::max(0, L.radius - 1)));
                g.DrawPath(&bp, &pp);
            }
            if (c.edit) DrawEditOverlay(g, c, L);
        }
        DrawGrip(g, c, L);
    }

    POINT pt{ win.left, win.top };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    // psizе = size, pptSrc = origin inside that bitmap, crKey unused with ULW_ALPHA
    UpdateLayeredWindow(c.widget, screen, &pt, &sz, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ------------------------------------------------------------- schedules ----
unsigned FramePeriod(Ctx& c)
{
    Config& k = *c.cfg;
    if (c.hasFade) return 16;                       // ~60 fps while crossfading
    if (k.anim != L"none" && k.anim_fps > 0 && c.playing && !FullscreenRunning())
        return unsigned(std::max(4, int(1000.0 / k.anim_fps)));
    return 0;
}

void Invalidate(Ctx& c)
{
    if (!c.hidden) { RenderFrame(c); return; }
    unsigned fp = FramePeriod(c);
    if (fp) SetTimer(c.hidden, DSKV_TIMER_ANIM, fp, nullptr);
    else    KillTimer(c.hidden, DSKV_TIMER_ANIM);
    RenderFrame(c);
}

} // namespace dskv
