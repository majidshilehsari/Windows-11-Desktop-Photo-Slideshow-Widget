// images.cpp - playlist scanning + image decoding / pre-scaling (GDI+)
#include "dskv_pch.h"
#include "config.h"
#include <random>

using namespace Gdiplus;

namespace dskv {
namespace {

std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep)
{
    std::vector<std::wstring> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(sep, p);
        std::wstring t = (e == std::wstring::npos) ? s.substr(p) : s.substr(p, e - p);
        while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
        if (!t.empty()) out.push_back(t);
        if (e == std::wstring::npos) break;
        p = e + 1;
    }
    return out;
}

bool ExtAllowed(const Config& k, const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || path.find_last_of(L"\\/") > dot) return false;
    std::wstring e = path.substr(dot);
    for (auto& ch : e) ch = wchar_t(tolower(ch));
    static std::vector<std::wstring> cache;
    static std::wstring cacheSrc;
    if (cacheSrc != k.exts) { cache.clear(); cacheSrc = k.exts; }
    if (cache.empty()) {
        for (auto t : SplitList(k.exts, L';')) {
            if (t.empty()) continue;
            if (t.front() != L'.') t.insert(t.begin(), L'.');
            for (auto& ch : t) ch = wchar_t(tolower(ch));
            cache.push_back(t);
        }
    }
    for (auto& t : cache) if (t == e) return true;
    return false;
}

bool DecodeFirst(const std::wstring& path, bool exifFix, Bitmap*& outBmp, SIZE& outSize)
{
    outBmp = nullptr;
    outSize = {0, 0};
    Bitmap* bmp = new Bitmap(path.c_str(), FALSE);      // decodes, then releases the file handle
    if (!bmp || bmp->GetLastStatus() != Ok) { delete bmp; return false; }

    // GDI+ normally honours the EXIF orientation itself.  `exif_auto_rotate = 1` is
    // the escape hatch for pictures that still show up sideways: we rotate the
    // pixels and then neutralise the stored tag so nothing is applied twice.
    if (exifFix) {
        UINT sz = bmp->GetPropertyItemSize(PropertyTagOrientation);
        if (sz >= sizeof(PropertyItem) + 2) {
            std::vector<BYTE> buf(sz);
            PropertyItem* pi = reinterpret_cast<PropertyItem*>(buf.data());
            if (bmp->GetPropertyItem(PropertyTagOrientation, sz, pi) == Ok &&
                pi->type == 3 && pi->length >= 2) {
                UINT16 o = *reinterpret_cast<UINT16*>(pi->value);
                RotateFlipType rf = RotateNoneFlipNone;
                bool flip = false;
                switch (o) {
                    case 2: rf = RotateNoneFlipNone;  flip = true; break;
                    case 3: rf = Rotate180FlipNone;   break;
                    case 4: rf = Rotate180FlipNone;   flip = true; break;
                    case 5: rf = Rotate90FlipNone;    flip = true; break;
                    case 6: rf = Rotate90FlipNone;    break;
                    case 7: rf = Rotate270FlipNone;   flip = true; break;
                    case 8: rf = Rotate270FlipNone;   break;
                    default: break;
                }
                if (o >= 2 && o <= 8 && bmp->RotateFlip(rf) == Ok) {
                    if (flip) { bmp->RotateFlip(RotateNoneFlipX); }
                    *reinterpret_cast<UINT16*>(pi->value) = 1;   // tag -> "normal"
                    bmp->SetPropertyItem(pi);
                }
            }
        }
    }
    outSize.cx = static_cast<LONG>(bmp->GetWidth());
    outSize.cy = static_cast<LONG>(bmp->GetHeight());
    if (outSize.cx < 1 || outSize.cy < 1) { delete bmp; return false; }
    outBmp = bmp;
    return true;
}

} // namespace

void FreeScaled(ScaledImage& s)
{
    if (s.bmp) { delete s.bmp; s.bmp = nullptr; }
    s.path.clear();
    s.srcSize = {0, 0};
    s.boxSize = {0, 0};
    s.baseScale = 1.0;
    s.sig.clear();
}

void RebuildPlaylist(Ctx& c)
{
    Config& k = *c.cfg;
    c.list.clear();
    std::error_code ec;
    std::wstring root = k.path;
    if (root.empty()) return;
    if (!std::filesystem::exists(root, ec)) return;

    if (k.recursive) {
        std::filesystem::recursive_directory_iterator it(root,
            std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator end;
        size_t guard = 0;
        for (; !ec && it != end && guard < 200000; ++it, ++guard) {
            bool isReg = false;
            it->is_regular_file(ec);
            isReg = !ec && it->is_regular_file(ec);
            if (ec) { ec.clear(); continue; }
            std::wstring p = it->path().wstring();
            if (isReg && ExtAllowed(k, p)) c.list.push_back(p);
        }
    } else {
        std::filesystem::directory_iterator it(root, ec), end;
        for (; !ec && it != end; ++it) {
            std::wstring p = it->path().wstring();
            if (ExtAllowed(k, p)) c.list.push_back(p);
        }
    }

    std::sort(c.list.begin(), c.list.end(), [&](const std::wstring& a, const std::wstring& b) {
        std::filesystem::path pa(a), pb(b);
        int r = _wcsicmp(pa.filename().c_str(), pb.filename().c_str());
        if (r == 0) r = _wcsicmp(a.c_str(), b.c_str());
        return k.sort_desc ? (r > 0) : (r < 0);
    });

    if (k.shuffle && c.list.size() > 1) {
        unsigned seed = (unsigned)(GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u));
        std::mt19937_64 rng(seed);
        std::shuffle(c.list.begin(), c.list.end(), rng);
    }
    if (c.index >= c.list.size()) c.index = 0;
}

// Pre-scale an image one step beyond the size it will be drawn at, so that a
// zoom / pan animation only has to move a crop rectangle - no resampling per
// animation frame.  At zoom 1 the visible crop is the whole bitmap.
Layout CurrentLayout(Ctx& c);

bool GetScaled(Ctx& c, size_t idx, ScaledImage& out)
{
    Config& k = *c.cfg;
    if (idx >= c.list.size()) return false;
    Layout L = CurrentLayout(c);
    LONG W = L.content.right - L.content.left;
    LONG H = L.content.bottom - L.content.top;
    if (W < 16) W = 16;
    if (H < 16) H = 16;

    double zmax = (k.anim == L"none") ? 1.0 : Clampd(k.anim_zoom, 1.0, 2.0);
    std::wstring sig = std::to_wstring(W) + L"x" + std::to_wstring(H) + L"z" + std::to_wstring(int(zmax * 100)) +
                       (k.fit == Fit::Cover ? L"c" : (k.fit == Fit::Contain ? L"f" : L"s"));
    (void)L;

    if (out.bmp && out.sig == sig && out.path == c.list[idx]) return true;

    FreeScaled(out);

    Bitmap* full = nullptr;
    SIZE src{};
    if (!DecodeFirst(c.list[idx], k.exif_auto_rotate, full, src) || !full) return false;
    if (k.min_side > 0 && (src.cx < k.min_side || src.cy < k.min_side)) {
        delete full;
        return false;
    }

    double sx = double(W) / src.cx, sy = double(H) / src.cy;
    (void)sx;
    double exact = (k.fit == Fit::Cover) ? std::max(sx, sy)
                 : (k.fit == Fit::Contain) ? std::min(sx, sy) : 1.0;
    if (k.fit == Fit::Stretch) { exact = 1.0; sx = 1.0; sy = 1.0; }
    (void)sy;

    double over = (k.fit == Fit::Stretch || k.anim == L"none") ? 1.0 : zmax;
    LONG bw = LONG(std::llround(src.cx * exact * over));
    LONG bh = LONG(std::llround(src.cy * exact * over));
    if (k.fit == Fit::Stretch) { bw = W; bh = H; }
    bw = Clampi(bw, 1, 16384);
    bh = Clampi(bh, 1, 16384);

    Bitmap* box = new Bitmap(bw, bh, PixelFormat32bppPARGB);
    if (box->GetLastStatus() != Ok) { delete box; delete full; return false; }
    {
        Graphics g(box);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetCompositingMode(CompositingModeSourceCopy);
        RectF dst(0, 0, REAL(bw), REAL(bh));
        g.DrawImage(full, dst, 0, 0, REAL(src.cx), REAL(src.cy), UnitPixel);
    }
    delete full;

    out.path = c.list[idx];
    out.srcSize = src;
    out.boxSize = { bw, bh };
    out.bmp = box;
    out.coverFill = (k.fit == Fit::Cover);
    out.baseScale = exact;
    out.sig = sig;
    return true;
}

std::wstring BuildCaption(Ctx& c, const std::wstring& file)
{
    Config& k = *c.cfg;
    std::filesystem::path p(file);
    std::wstring name = p.filename().wstring();
    std::wstring folder = p.parent_path().filename().wstring();
    std::wstring f = k.cap_fmt;
    if (f.empty()) f = L"{name}";

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t num[64];

    std::wstring out;
    for (size_t i = 0; i < f.size();) {
        if (f[i] == L'{' ) {
            size_t e = f.find(L'}', i);
            if (e == std::wstring::npos) { out += f[i++]; continue; }
            std::wstring tok = f.substr(i + 1, e - i - 1);
            i = e + 1;
            if (tok == L"name") out += name;
            else if (tok == L"folder") out += folder;
            else if (tok == L"path") out += file;
            else if (tok == L"i" || tok == L"index") {
                swprintf(num, 64, L"%zu", c.list.empty() ? 0 : c.index + 1);
                out += num;
            } else if (tok == L"n" || tok == L"count") {
                swprintf(num, 64, L"%zu", c.list.size());
                out += num;
            } else if (tok == L"time") {
                swprintf(num, 64, L"%02d:%02d", st.wHour, st.wMinute);
                out += num;
            } else if (tok == L"date") {
                swprintf(num, 64, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
                out += num;
            } else {
                out += L"{" + tok + L"}";
            }
        } else out += f[i++];
    }
    return out;
}

} // namespace dskv
