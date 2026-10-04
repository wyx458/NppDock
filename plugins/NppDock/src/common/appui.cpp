// ===========================================================================
// appui.cpp —— 见 appui.h 的说明
// ===========================================================================
#include "appui.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace appui {

static int g_dpi = 96;

void SetDpi(int dpi) { if (dpi > 0) g_dpi = dpi; }
int  Dp(int px)      { return ::MulDiv(px, g_dpi, 96); }
int  Dpi()           { return g_dpi; }

int UiFontPt()
{
    HFONT f = MakeUiFont();
    if (!f) return 9;
    LOGFONTW lf{};
    const bool ok = ((int)::GetObjectW(f, sizeof(lf), &lf) == sizeof(lf));
    ::DeleteObject(f);
    if (!ok || lf.lfHeight == 0) return 9;
    int pt = ::MulDiv(-lf.lfHeight, 72, g_dpi);
    if (pt < 6) pt = 6;
    return pt;
}

int FontCellHeight(HFONT f)
{
    if (!f) return 0;
    int h = 0;
    HDC dc = ::GetDC(nullptr);
    if (!dc) return 0;
    HGDIOBJ old = ::SelectObject(dc, f);
    TEXTMETRICW tm{};
    if (::GetTextMetricsW(dc, &tm)) h = (int)tm.tmHeight;
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(nullptr, dc);
    return h;
}

void AdoptHostDpiAwareness(HWND host)
{
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) return;

    auto pGetCtx = (HANDLE (WINAPI*)(HWND))::GetProcAddress(u, "GetWindowDpiAwarenessContext");
    auto pSetCtx = (BOOL (WINAPI*)(HANDLE))::GetProcAddress(u, "SetProcessDpiAwarenessContext");

    // ① 嵌入时：直接用宿主那一份上下文，最准
    if (pGetCtx && pSetCtx && host) {
        HANDLE ctx = pGetCtx(host);
        if (ctx && pSetCtx(ctx)) return;
    }
    // ② 独立运行：per-monitor-v2（== -4）
    if (pSetCtx && pSetCtx((HANDLE)(INT_PTR)-4)) return;

    // ③ 老系统：SetProcessDpiAwareness 在 **shcore.dll** 里（不是在 user32！）
    if (HMODULE sh = ::LoadLibraryW(L"shcore.dll")) {
        auto p2 = (HRESULT (WINAPI*)(int))::GetProcAddress(sh, "SetProcessDpiAwareness");
        if (p2 && p2(2) == 0 /*S_OK*/) return;   // 2 = PROCESS_PER_MONITOR_DPI_AWARE
    }
    // ④ 最老的兜底
    if (auto p3 = (BOOL (WINAPI*)())::GetProcAddress(u, "SetProcessDPIAware")) p3();
}

HFONT MakeUiFont()
{
    typedef BOOL (WINAPI* PFN_SpiForDpi)(UINT, UINT, PVOID, UINT, UINT);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    bool got = false;
    if (HMODULE u = ::GetModuleHandleW(L"user32.dll")) {
        auto forDpi = (PFN_SpiForDpi)::GetProcAddress(u, "SystemParametersInfoForDpi");
        if (forDpi)
            got = forDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi) != FALSE;
    }
    if (!got) {
        if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return nullptr;
        // 老系统没有 *ForDpi：拿到的是**系统 DPI** 的一份，按比例补正**一次**
        HDC dc = ::GetDC(nullptr);
        UINT sysDpi = dc ? (UINT)::GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ::ReleaseDC(nullptr, dc);
        if (sysDpi && sysDpi != (UINT)g_dpi && ncm.lfMessageFont.lfHeight) {
            ncm.lfMessageFont.lfHeight =
                ::MulDiv(ncm.lfMessageFont.lfHeight, g_dpi, (int)sysDpi);
        }
    }
    return ::CreateFontIndirectW(&ncm.lfMessageFont);
}

HFONT MakeMonoFont(HFONT uiFont)
{
    if (!uiFont) return nullptr;
    LOGFONTW uilf{};
    if (::GetObjectW(uiFont, sizeof(uilf), &uilf) != sizeof(uilf)) return nullptr;

    int em = uilf.lfHeight;                       // 负值 = 按字符高度算
    if (em == 0) em = -FontCellHeight(uiFont);
    if (em == 0) return nullptr;

    static const wchar_t* kMono[] = { L"Consolas", L"Lucida Console", L"Courier New" };
    for (size_t i = 0; i < sizeof(kMono) / sizeof(kMono[0]); ++i) {
        LOGFONTW lf{};
        lf.lfHeight         = em;
        lf.lfWeight         = FW_NORMAL;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfOutPrecision   = OUT_TT_PRECIS;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcsncpy_s(lf.lfFaceName, kMono[i], _TRUNCATE);

        HFONT f = ::CreateFontIndirectW(&lf);
        if (!f) continue;

        // ⚠️ 必须回读核对：CreateFontIndirect 在字型不存在时**不报错**，
        //    会静默换成别的（通常还是比例字体）。
        bool ok = false;
        if (HDC dc = ::GetDC(nullptr)) {
            HGDIOBJ old = ::SelectObject(dc, f);
            wchar_t face[LF_FACESIZE]{};
            ::GetTextFaceW(dc, LF_FACESIZE, face);
            if (old) ::SelectObject(dc, old);
            ::ReleaseDC(nullptr, dc);
            ok = (_wcsicmp(face, kMono[i]) == 0);
        }
        if (ok) return f;
        ::DeleteObject(f);
    }
    return nullptr;
}

// 回读核对：CreateFontIndirect 找不到字型时**不报错**，会静默换一个。
static bool FaceMatches(HFONT f, const wchar_t* want)
{
    if (!f || !want || !*want) return true;
    bool ok = false;
    if (HDC dc = ::GetDC(nullptr)) {
        HGDIOBJ old = ::SelectObject(dc, f);
        wchar_t face[LF_FACESIZE]{};
        ::GetTextFaceW(dc, LF_FACESIZE, face);
        if (old) ::SelectObject(dc, old);
        ::ReleaseDC(nullptr, dc);
        ok = (_wcsicmp(face, want) == 0);
    }
    return ok;
}

HFONT MakeFont(const wchar_t* face, int pt, bool mono, bool bold)
{
    // 基准字号取自"当前 DPI 下的系统消息字体"（MakeUiFont 已经处理好 DPI 那套坑）
    HFONT base = MakeUiFont();
    if (!base) return nullptr;
    LOGFONTW lf{};
    const int got = (int)::GetObjectW(base, sizeof(lf), &lf);
    ::DeleteObject(base);
    if (got != sizeof(lf)) return nullptr;

    int em = lf.lfHeight;                       // 负值 = 按字符高度算
    if (em == 0) em = -Dp(12);
    if (pt > 0) {
        em = -::MulDiv(pt, g_dpi, 72);
    } else if (mono) {
        em = em * 8 / 9;                        // 等宽默认小一号（等宽字看起来更大）
        if (em > -6) em = -6;
    }

    lf.lfHeight         = em;
    lf.lfWeight         = bold ? FW_BOLD : FW_NORMAL;   // 默认不加粗
    lf.lfItalic         = FALSE;
    lf.lfQuality        = CLEARTYPE_QUALITY;
    if (mono) lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;

    const bool wantFace = (face && *face);
    if (wantFace) wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);

    HFONT f = ::CreateFontIndirectW(&lf);
    if (!f) return nullptr;
    if (wantFace && !FaceMatches(f, face)) {
        // 字型不存在：退回系统字体的脸，但**保留**字号/等宽属性
        ::DeleteObject(f);
        wcsncpy_s(lf.lfFaceName, L"", _TRUNCATE);
        // 重新拿一次系统脸名
        HFONT b2 = MakeUiFont();
        if (b2) {
            LOGFONTW l2{};
            if ((int)::GetObjectW(b2, sizeof(l2), &l2) == sizeof(l2))
                wcsncpy_s(lf.lfFaceName, l2.lfFaceName, _TRUNCATE);
            ::DeleteObject(b2);
        }
        f = ::CreateFontIndirectW(&lf);
    }
    return f;
}

HPEN MakeIconPen(int width, COLORREF col)
{
    LOGBRUSH lb{};
    lb.lbStyle = BS_SOLID;
    lb.lbColor = col;
    HPEN p = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND,
                            (DWORD)(width > 1 ? width : 1), &lb, 0, nullptr);
    return p ? p : ::CreatePen(PS_SOLID, width, col);
}

static COLORREF IconColor(bool disabled)
{
    return disabled ? ::GetSysColor(COLOR_GRAYTEXT) : RGB(0x3A, 0x3A, 0x3A);
}

void DrawGearGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int cx = rc.left + w / 2, cy = rc.top + h / 2;
    const int R = (std::min)(w, h) / 2;
    const int r = (std::max)(3, R * 6 / 10);

    HPEN pen = MakeIconPen((std::max)(2, R / 4), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    static const int kDir[8][2] = {
        { 1000, 0 }, { 707, 707 }, { 0, 1000 }, { -707, 707 },
        { -1000, 0 }, { -707, -707 }, { 0, -1000 }, { 707, -707 },
    };
    for (int k = 0; k < 8; ++k) {
        ::MoveToEx(dc, cx + r * kDir[k][0] / 1000, cy + r * kDir[k][1] / 1000, nullptr);
        ::LineTo  (dc, cx + R * kDir[k][0] / 1000, cy + R * kDir[k][1] / 1000);
    }
    ::Ellipse(dc, cx - r, cy - r, cx + r, cy + r);

    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// ↻ 刷新：整圈留一个缺口 + **实心三角**箭头（v1.5.1 重画）
//
// 为什么不沿用上一版：上一版是"8 段折线逼近的圆"+"两笔画出来的箭头"，
// 在实际那点尺寸下圆看着发方、箭头糊成一团 —— 王原话是"图标很不好"。
// 现在：折线密到 48 段（看不出棱角），箭头改成 `Polygon` 填充的实心三角。
void DrawRefreshGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 6 || h <= 6) return;
    const int cx = rc.left + w / 2, cy = rc.top + h / 2;
    const int side = (std::min)(w, h);
    const int R = side / 2 - (std::max)(2, side / 6);   // 给箭头留出余量
    if (R < 3) return;
    const int penW = (std::max)(2, R / 3);

    // 圆弧：从 -15° 顺时针扫到 290°，缺口留在**右上**（↻ 最容易被认出的形态）。
    const double kPi   = 3.14159265358979;
    const int    kStep = 48;
    const double a0 = -15.0 * kPi / 180.0;
    const double a1 = 290.0 * kPi / 180.0;

    HPEN pen = MakeIconPen(penW, col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    for (int i = 0; i <= kStep; ++i) {
        const double a = a0 + (a1 - a0) * i / kStep;
        const int x = cx + (int)std::lround(R * std::cos(a));
        const int y = cy + (int)std::lround(R * std::sin(a));
        if (i == 0) ::MoveToEx(dc, x, y, nullptr);
        else        ::LineTo(dc, x, y);
    }
    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    // 箭头：实心三角，摆在圆弧靠上那一端、沿切线指向**顺时针**（也就是向右）。
    const double pxc = cx + R * std::cos(a1);
    const double pyc = cy + R * std::sin(a1);
    const double dx  = -std::sin(a1);          // 参数增大方向 = 顺时针前进方向
    const double dy  =  std::cos(a1);
    const double nx  = -dy, ny = dx;           // 法向（用来撑开箭头两翼）
    const double L   = (std::max)(4.0, R * 0.72);
    const double Wd  = (std::max)(2.5, R * 0.40);

    POINT tri[3];
    tri[0].x = (LONG)std::lround(pxc + dx * L * 0.62);
    tri[0].y = (LONG)std::lround(pyc + dy * L * 0.62);
    tri[1].x = (LONG)std::lround(pxc - dx * L * 0.38 + nx * Wd);
    tri[1].y = (LONG)std::lround(pyc - dy * L * 0.38 + ny * Wd);
    tri[2].x = (LONG)std::lround(pxc - dx * L * 0.38 - nx * Wd);
    tri[2].y = (LONG)std::lround(pyc - dy * L * 0.38 - ny * Wd);

    HBRUSH  br = ::CreateSolidBrush(col);
    HGDIOBJ ob = ::SelectObject(dc, br);
    HGDIOBJ op = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, tri, 3);
    ::SelectObject(dc, op);
    ::SelectObject(dc, ob);
    ::DeleteObject(br);
}

void DrawTrashGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int l = rc.left, t = rc.top;
    auto X = [&](int per) { return l + w * per / 100; };
    auto Y = [&](int per) { return t + h * per / 100; };

    HPEN pen = MakeIconPen((std::max)(2, (std::min)(w, h) / 9), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::MoveToEx(dc, X(40), Y(12), nullptr);
    ::LineTo  (dc, X(60), Y(12));
    ::MoveToEx(dc, X(8),  Y(24), nullptr);
    ::LineTo  (dc, X(92), Y(24));
    ::MoveToEx(dc, X(22), Y(24), nullptr);
    ::LineTo  (dc, X(28), Y(90));
    ::LineTo  (dc, X(72), Y(90));
    ::LineTo  (dc, X(78), Y(24));
    ::MoveToEx(dc, X(41), Y(38), nullptr);
    ::LineTo  (dc, X(44), Y(78));
    ::MoveToEx(dc, X(59), Y(38), nullptr);
    ::LineTo  (dc, X(56), Y(78));

    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// 🔧：开口扳手。王说齿轮画出来"像个灯泡"，所以换它 ——
// 结构就两笔：右上角一个**带缺口的环**（钳口）+ 一根斜手柄（左下）。
void DrawWrenchGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 6 || h <= 6) return;
    const int m  = (std::min)(w, h);
    const int cx = rc.left + w / 2, cy = rc.top + h / 2;
    const int penW = (std::max)(2, m / 9);

    HPEN pen = MakeIconPen(penW, col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    const double kPi = 3.14159265358979;
    // 比例：环小、手柄长（同 nettest 里那份 —— 两处必须长得一样）
    const int r  = (int)(m * 0.17);
    const int jx = cx + m * 24 / 100, jy = cy - m * 24 / 100;

    // ① 钳口：开口的环（缺口 70°，朝右上）
    {
        const int steps = 26;
        for (int i = 0; i <= steps; ++i) {
            const double a = (-110.0 + 290.0 * i / steps) * kPi / 180.0;
            const int px = jx + (int)(r * std::cos(a));
            const int py = jy + (int)(r * std::sin(a));
            if (i == 0) ::MoveToEx(dc, px, py, nullptr);
            else        ::LineTo(dc, px, py);
        }
    }
    // ② 手柄：从钳口左下方（135°）斜向左下，比钳口线粗一点
    {
        HPEN hpen = MakeIconPen((std::max)(2, penW * 3 / 2), col);
        HGDIOBJ oldHP = ::SelectObject(dc, hpen);
        ::MoveToEx(dc, jx + (int)(r * -0.7071), jy + (int)(r * 0.7071), nullptr);
        ::LineTo  (dc, cx - m * 32 / 100,       cy + m * 32 / 100);
        ::SelectObject(dc, oldHP);
        ::DeleteObject(hpen);
    }

    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// ❗：圆里的感叹号（查看属性）
void DrawInfoGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int m = (std::min)(w, h);
    const int cx = rc.left + w / 2, cy = rc.top + h / 2;
    const int R  = m / 2 - (std::max)(1, m / 12);
    const int penW = (std::max)(2, m / 9);

    HPEN pen = MakeIconPen(penW, col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Ellipse(dc, cx - R, cy - R, cx + R + 1, cy + R + 1);
    ::SelectObject(dc, oldBr);       // 还原画刷：DC 是调用方的，别把 NULL_BRUSH 留给它
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    // 感叹号：上半竖线 + 下半一个点（用短横线代替圆点，省一个画刷）
    HBRUSH br = ::CreateSolidBrush(col);
    HGDIOBJ oldBr2 = ::SelectObject(dc, br);
    HGDIOBJ nullPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    const int bw = (std::max)(2, m / 9);
    const int x0 = cx - bw / 2, x1 = x0 + bw;
    RECT bar{ x0, cy - R / 2, x1, cy + R / 8 };
    ::FillRect(dc, &bar, br);
    RECT dot{ x0, cy + R / 3, x1, cy + R / 3 + bw };
    ::FillRect(dc, &dot, br);
    ::SelectObject(dc, nullPen);
    ::SelectObject(dc, oldBr2);
    ::DeleteObject(br);
}

static void GlyphRectPath(HDC dc, int l, int t, int r, int b,
                          const int (*pts)[2], int n)
{
    const int w = r - l, h = b - t;
    for (int i = 0; i < n; ++i) {
        const int x = l + w * pts[i][0] / 100;
        const int y = t + h * pts[i][1] / 100;
        if (i == 0) ::MoveToEx(dc, x, y, nullptr);
        else        ::LineTo(dc, x, y);
    }
}

void DrawFolderGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    HPEN pen = MakeIconPen((std::max)(2, (std::min)(w, h) / 8), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    // 左边高一点的"夹子" + 右边矮一点的顶边 —— 一眼就是文件夹
    static const int kFolder[][2] = {
        { 8, 26 }, { 40, 26 }, { 50, 38 }, { 92, 38 },
        { 92, 86 }, { 8, 86 }, { 8, 26 },
    };
    GlyphRectPath(dc, rc.left, rc.top, rc.right, rc.bottom, kFolder, 7);
    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawFileGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    HPEN pen = MakeIconPen((std::max)(1, (std::min)(w, h) / 10), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    static const int kFile[][2] = {
        { 22, 8 }, { 62, 8 }, { 80, 28 }, { 80, 92 }, { 22, 92 }, { 22, 8 },
    };
    GlyphRectPath(dc, rc.left, rc.top, rc.right, rc.bottom, kFile, 6);
    static const int kFold[][2]  = {
        { 62, 8 }, { 62, 28 }, { 80, 28 },
    };
    GlyphRectPath(dc, rc.left, rc.top, rc.right, rc.bottom, kFold, 3);
    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawUpGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    HPEN pen = MakeIconPen((std::max)(2, (std::min)(w, h) / 7), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    static const int kArrow[][2] = {
        { 50, 12 }, { 14, 52 }, { 50, 52 }, { 50, 90 },
    };
    // 箭杆单独一笔（竖直），箭头两笔
    const int l = rc.left, t = rc.top, r = rc.right, b = rc.bottom;
    const int cxm = l + (r - l) / 2;
    ::MoveToEx(dc, l + (r - l) * 12 / 100, t + (b - t) * 48 / 100, nullptr);
    ::LineTo  (dc, cxm, t + (b - t) * 10 / 100);
    ::LineTo  (dc, l + (r - l) * 88 / 100, t + (b - t) * 48 / 100);
    ::MoveToEx(dc, cxm, t + (b - t) * 12 / 100, nullptr);
    ::LineTo  (dc, cxm, t + (b - t) * 92 / 100);
    (void)kArrow;
    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawIconButton(const DRAWITEMSTRUCT& dis, GlyphFn icon)
{
    HDC dc = dis.hDC;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis.itemState & ODS_SELECTED) != 0;

    RECT rc = dis.rcItem;
    ::FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
    UINT st = DFCS_BUTTONPUSH;
    if (pressed)  st |= DFCS_PUSHED;
    if (disabled) st |= DFCS_INACTIVE;
    ::DrawFrameControl(dc, &rc, DFC_BUTTON, st);

    if (icon) {
        RECT gi = rc;
        if (pressed) ::OffsetRect(&gi, Dp(1), Dp(1));
        const int inset = (std::max)(Dp(5), (int)(rc.bottom - rc.top) / 5);
        ::InflateRect(&gi, -inset, -inset);
        icon(dc, gi, IconColor(disabled));
    }
    if (dis.itemState & ODS_FOCUS) {
        RECT f = rc;
        ::InflateRect(&f, -Dp(2), -Dp(2));
        ::DrawFocusRect(dc, &f);
    }
}

void DrawStatusDot(HDC dc, const RECT& rc, COLORREF dotColor, const wchar_t* text)
{
    const int d = (std::max)(Dp(8), (int)(rc.bottom - rc.top) / 3);   // 圆点直径
    const int cy = rc.top + (rc.bottom - rc.top) / 2;
    const int cx = rc.left + d / 2 + Dp(2);

    HBRUSH br = ::CreateSolidBrush(dotColor);
    HGDIOBJ oldBr = ::SelectObject(dc, br);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Ellipse(dc, cx - d / 2, cy - d / 2, cx + d / 2 + 1, cy + d / 2 + 1);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBr);
    ::DeleteObject(br);

    if (text && *text) {
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, ::GetSysColor(COLOR_BTNTEXT));
        RECT tr = rc;
        tr.left = cx + d / 2 + Dp(6);
        ::DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// v1.6：状态行 = 圆点 + 提示词 +（可选）进度条
void DrawStatusLine(HDC dc, const RECT& rc, COLORREF dotColor,
                    const wchar_t* text, int pct)
{
    RECT area = rc;
    if (pct >= 0) {
        const int barH = (std::max)(Dp(6), (int)(rc.bottom - rc.top) / 5);
        const int gap  = Dp(4);
        area.bottom = rc.bottom - barH - gap;

        RECT bar{ rc.left + Dp(2), rc.bottom - barH,
                  rc.left + (std::min)((int)(rc.right - rc.left) - Dp(2), Dp(260)), rc.bottom };
        if (bar.right > bar.left) {
            HBRUSH slot = ::CreateSolidBrush(RGB(0xDC, 0xDC, 0xDC));
            ::FillRect(dc, &bar, slot);
            ::DeleteObject(slot);
            int p = pct; if (p > 100) p = 100;
            RECT fill = bar;
            fill.right = bar.left + (bar.right - bar.left) * p / 100;
            if (fill.right > fill.left) {
                HBRUSH fb = ::CreateSolidBrush(p >= 100 ? RGB(0x2E, 0xA0, 0x43)
                                                        : RGB(0x37, 0x8A, 0xDD));
                ::FillRect(dc, &fill, fb);
                ::DeleteObject(fb);
            }
        }
    }

    const int d  = (std::max)(Dp(8), (int)(area.bottom - area.top) / 2);
    const int cy = area.top + (area.bottom - area.top) / 2;
    const int cx = area.left + d / 2 + Dp(2);

    HBRUSH br = ::CreateSolidBrush(dotColor);
    HGDIOBJ ob = ::SelectObject(dc, br);
    HGDIOBJ op = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Ellipse(dc, cx - d / 2, cy - d / 2, cx + d / 2 + 1, cy + d / 2 + 1);
    ::SelectObject(dc, op);
    ::SelectObject(dc, ob);
    ::DeleteObject(br);

    if (text && *text) {
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, ::GetSysColor(COLOR_BTNTEXT));
        RECT tr = area;
        tr.left = cx + d / 2 + Dp(6);
        ::DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// v1.6：带文字的图标按钮
void DrawTextButton(const DRAWITEMSTRUCT& dis, const wchar_t* text,
                    GlyphFn icon, bool danger)
{
    HDC dc = dis.hDC;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis.itemState & ODS_SELECTED) != 0;

    RECT rc = dis.rcItem;
    ::FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
    UINT st = DFCS_BUTTONPUSH;
    if (pressed)  st |= DFCS_PUSHED;
    if (disabled) st |= DFCS_INACTIVE;
    ::DrawFrameControl(dc, &rc, DFC_BUTTON, st);

    const COLORREF col = danger
                             ? RGB(0xA3, 0x2D, 0x2D)
                             : (disabled ? ::GetSysColor(COLOR_GRAYTEXT) : RGB(0x3A, 0x3A, 0x3A));
    if (danger) {
        HBRUSH fb = ::CreateSolidBrush(RGB(0xF0, 0x95, 0x95));
        ::FrameRect(dc, &rc, fb);
        ::DeleteObject(fb);
    }

    RECT gi = rc;
    if (pressed) ::OffsetRect(&gi, Dp(1), Dp(1));

    HFONT f = (HFONT)::SendMessageW(dis.hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldF = f ? ::SelectObject(dc, f) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, col);

    // ★ v1.7：**没有图标**的按钮（文件操作那一排）—— 文字总是要画出来的。
    //   以前的做法是"挤不下就只画图标"，可这一排压根没有图标，
    //   于是窄面板下按钮会变成**一片空白**（看不见自己是什么）。
    //   现在：没有图标 → 文字居中画，挤不下就省略号。
    if (!icon && text && *text) {
        RECT tr = gi;
        ::DrawTextW(dc, text, -1, &tr,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (oldF) ::SelectObject(dc, oldF);
        if (dis.itemState & ODS_FOCUS) {
            RECT fr = rc;
            ::InflateRect(&fr, -Dp(2), -Dp(2));
            ::DrawFocusRect(dc, &fr);
        }
        return;
    }

    SIZE ts{};
    if (text && *text) ::GetTextExtentPoint32W(dc, text, (int)wcslen(text), &ts);
    const int side   = (std::max)(Dp(13), (int)(gi.bottom - gi.top) - Dp(8));
    const int totalW = (icon ? side + Dp(4) : 0) + ts.cx;
    const bool showText = (ts.cx > 0) && (rc.right - rc.left) >= totalW + Dp(12);
    const int drawW = showText ? totalW : (icon ? side : 0);

    int x = gi.left + ((gi.right - gi.left) - drawW) / 2;
    if (icon) {
        const int top = gi.top + ((gi.bottom - gi.top) - side) / 2;
        RECT ir{ x, top, x + side, top + side };
        icon(dc, ir, col);
        x += side + Dp(4);
    }
    if (showText && text) {
        RECT tr{ x, gi.top, gi.right - Dp(2), gi.bottom };
        ::DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (oldF) ::SelectObject(dc, oldF);

    if (dis.itemState & ODS_FOCUS) {
        RECT fr = rc;
        ::InflateRect(&fr, -Dp(2), -Dp(2));
        ::DrawFocusRect(dc, &fr);
    }
}

void DrawDownloadGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int l = rc.left, t = rc.top;
    auto X = [&](int per) { return l + w * per / 100; };
    auto Y = [&](int per) { return t + h * per / 100; };

    HPEN pen = MakeIconPen((std::max)(2, w / 9), col);
    HGDIOBJ op = ::SelectObject(dc, pen);
    HGDIOBJ ob = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::MoveToEx(dc, X(50), Y(12), nullptr); ::LineTo(dc, X(50), Y(56));   // 竖杆
    ::MoveToEx(dc, X(30), Y(38), nullptr); ::LineTo(dc, X(50), Y(58));   // 箭头
    ::MoveToEx(dc, X(70), Y(38), nullptr); ::LineTo(dc, X(50), Y(58));
    ::MoveToEx(dc, X(18), Y(72), nullptr); ::LineTo(dc, X(18), Y(86));   // 托盘
    ::LineTo(dc, X(82), Y(86));
    ::LineTo(dc, X(82), Y(72));

    ::SelectObject(dc, ob);
    ::SelectObject(dc, op);
    ::DeleteObject(pen);
}

void DrawUploadGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int l = rc.left, t = rc.top;
    auto X = [&](int per) { return l + w * per / 100; };
    auto Y = [&](int per) { return t + h * per / 100; };

    HPEN pen = MakeIconPen((std::max)(2, w / 9), col);
    HGDIOBJ op = ::SelectObject(dc, pen);
    HGDIOBJ ob = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::MoveToEx(dc, X(50), Y(58), nullptr); ::LineTo(dc, X(50), Y(14));   // 竖杆（朝上）
    ::MoveToEx(dc, X(30), Y(32), nullptr); ::LineTo(dc, X(50), Y(12));   // 箭头
    ::MoveToEx(dc, X(70), Y(32), nullptr); ::LineTo(dc, X(50), Y(12));
    ::MoveToEx(dc, X(18), Y(72), nullptr); ::LineTo(dc, X(18), Y(86));   // 托盘
    ::LineTo(dc, X(82), Y(86));
    ::LineTo(dc, X(82), Y(72));

    ::SelectObject(dc, ob);
    ::SelectObject(dc, op);
    ::DeleteObject(pen);
}

// v1.7：↺ 撤销（逆时针的圆弧箭头）—— 与刷新图标是"镜像"关系：
//   同样是"整圈留缺口 + 实心三角"，只是把 x 关于圆心翻一下，
//   于是缺口跑到左上、箭头指向逆时针 —— 一眼能和"刷新"区分开。
void DrawUndoGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 6 || h <= 6) return;
    const int cx = rc.left + w / 2, cy = rc.top + h / 2;
    const int side = (std::min)(w, h);
    const int R = side / 2 - (std::max)(2, side / 6);
    if (R < 3) return;
    const int penW = (std::max)(2, R / 3);

    const double kPi   = 3.14159265358979;
    const int    kStep = 48;
    const double a0 = -15.0 * kPi / 180.0;
    const double a1 = 290.0 * kPi / 180.0;

    HPEN pen = MakeIconPen(penW, col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    for (int i = 0; i <= kStep; ++i) {
        const double a = a0 + (a1 - a0) * i / kStep;
        const int x = cx - (int)std::lround(R * std::cos(a));   // ← 唯一与刷新不同处
        const int y = cy + (int)std::lround(R * std::sin(a));
        if (i == 0) ::MoveToEx(dc, x, y, nullptr);
        else        ::LineTo(dc, x, y);
    }
    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    const double pxc = cx - R * std::cos(a1);
    const double pyc = cy + R * std::sin(a1);
    const double dx  =  std::sin(a1);          // 镜像后的切线方向（逆时针前进）
    const double dy  =  std::cos(a1);
    const double nx  = -dy, ny = dx;
    const double L   = (std::max)(4.0, R * 0.72);
    const double Wd  = (std::max)(2.5, R * 0.40);

    POINT tri[3];
    tri[0].x = (LONG)std::lround(pxc + dx * L * 0.62);
    tri[0].y = (LONG)std::lround(pyc + dy * L * 0.62);
    tri[1].x = (LONG)std::lround(pxc - dx * L * 0.38 + nx * Wd);
    tri[1].y = (LONG)std::lround(pyc - dy * L * 0.38 + ny * Wd);
    tri[2].x = (LONG)std::lround(pxc - dx * L * 0.38 - nx * Wd);
    tri[2].y = (LONG)std::lround(pyc - dy * L * 0.38 - ny * Wd);

    HBRUSH  br = ::CreateSolidBrush(col);
    HGDIOBJ ob = ::SelectObject(dc, br);
    HGDIOBJ op = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, tri, 3);
    ::SelectObject(dc, op);
    ::SelectObject(dc, ob);
    ::DeleteObject(br);
}

// v1.8：🖊 重命名 —— 一支斜着的铅笔（笔杆 + 左下角的笔尖）
//
// 为什么不用"一个方框里写 R"那种：这一排是**纯图标按钮**，
// 得让人一眼看出是"改名字"，铅笔是通用符号。
// 复制：两片错开的纸（后面那片只画**露出来的部分**，免得两片糊在一起）
void DrawCopyGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int m    = (std::min)(w, h);
    const int penW = (std::max)(2, m / 8);
    const int off  = (std::max)(2, m / 4);        // 两片纸错开的距离

    HPEN pen = MakeIconPen(penW, col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    const int half = penW / 2;
    const int x0 = rc.left + half, y0 = rc.top + half;
    const int x1 = rc.right - half - off, y1 = rc.bottom - half - off;

    // 后面那片纸：从左下 → 左上 → 右上 → 沿右边缘下来到"被前面挡住"为止
    ::MoveToEx(dc, x1, y0 + off, nullptr);
    ::LineTo(dc, x1, y0);
    ::LineTo(dc, x0, y0);
    ::LineTo(dc, x0, y1);
    ::LineTo(dc, x1 - off, y1);

    // 前面那片纸：完整一个矩形（右下角）
    const int fx = x0 + off, fy = y0 + off;
    const int fx1 = rc.right - half, fy1 = rc.bottom - half;
    ::MoveToEx(dc, fx, fy, nullptr);
    ::LineTo(dc, fx, fy1);
    ::LineTo(dc, fx1, fy1);
    ::LineTo(dc, fx1, fy);
    ::LineTo(dc, fx, fy);

    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawPencilGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 6 || h <= 6) return;
    const int m = (std::min)(w, h);
    const int l = rc.left + (w - m) / 2, t = rc.top + (h - m) / 2;

    auto X = [&](double per) { return l + (int)(m * per); };
    auto Y = [&](double per) { return t + (int)(m * per); };

    const int thick = (std::max)(2, (int)(m * 0.17));

    // ① 笔杆：从左下往右上的一条粗斜线
    HPEN pen = MakeIconPen(thick, col);
    HGDIOBJ op = ::SelectObject(dc, pen);
    HGDIOBJ ob = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::MoveToEx(dc, X(0.31), Y(0.69), nullptr);
    ::LineTo  (dc, X(0.80), Y(0.20));
    ::SelectObject(dc, ob);
    ::SelectObject(dc, op);
    ::DeleteObject(pen);

    // ② 笔尖：左下角一个实心三角（尖点朝左下）
    const double per = 0.3536 * thick;               // thick/2 在 45° 上的分量
    POINT tip[3] = {
        { X(0.14),                      Y(0.86)                       },
        { X(0.31) + (LONG)per,          Y(0.69) + (LONG)per           },
        { X(0.31) - (LONG)per,          Y(0.69) - (LONG)per           },
    };
    HBRUSH br = ::CreateSolidBrush(col);
    HGDIOBJ o2 = ::SelectObject(dc, br);
    HGDIOBJ p2 = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Polygon(dc, tip, 3);
    ::SelectObject(dc, p2);
    ::SelectObject(dc, o2);
    ::DeleteObject(br);
}

// v1.8：✕ 取消（传输中的那个按钮变身成它）
void DrawCancelGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 6 || h <= 6) return;
    const int m = (std::min)(w, h);
    const int pad = (std::max)(3, (int)(m * 0.28));

    HPEN pen = MakeIconPen((std::max)(2, m / 5), col);
    HGDIOBJ op = ::SelectObject(dc, pen);
    HGDIOBJ ob = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::MoveToEx(dc, rc.left + pad,     rc.top + pad, nullptr);
    ::LineTo  (dc, rc.right - pad,    rc.bottom - pad);
    ::MoveToEx(dc, rc.right - pad,    rc.top + pad, nullptr);
    ::LineTo  (dc, rc.left + pad,     rc.bottom - pad);
    ::SelectObject(dc, ob);
    ::SelectObject(dc, op);
    ::DeleteObject(pen);
}

// v1.8：纯图标按钮 —— 就是"原生按钮框 + 你给的图标"，
// 多一个 danger（红框红图标，传输中的「取消」用它）。
// 和 DrawIconButton 的区别只有配色，之所以单独一个：
// DrawIconButton 是给"图标+文字"的老用法用的，别让那两处跟着变色。
void DrawGlyphButton(const DRAWITEMSTRUCT& dis, GlyphFn glyph, bool danger)
{
    HDC dc = dis.hDC;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis.itemState & ODS_SELECTED) != 0;

    RECT rc = dis.rcItem;
    ::FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
    UINT st = DFCS_BUTTONPUSH;
    if (pressed)  st |= DFCS_PUSHED;
    if (disabled) st |= DFCS_INACTIVE;
    ::DrawFrameControl(dc, &rc, DFC_BUTTON, st);

    if (danger) {
        HBRUSH fb = ::CreateSolidBrush(RGB(0xF0, 0x95, 0x95));
        ::FrameRect(dc, &rc, fb);
        ::DeleteObject(fb);
    }

    const COLORREF col = danger ? RGB(0xA3, 0x2D, 0x2D) : IconColor(disabled);

    if (glyph) {
        RECT gi = rc;
        if (pressed) ::OffsetRect(&gi, Dp(1), Dp(1));
        const int inset = (std::max)(Dp(4), (int)(rc.bottom - rc.top) / 5);
        ::InflateRect(&gi, -inset, -inset);
        glyph(dc, gi, col);
    }
    if (dis.itemState & ODS_FOCUS) {
        RECT f = rc;
        ::InflateRect(&f, -Dp(2), -Dp(2));
        ::DrawFocusRect(dc, &f);
    }
}

} // namespace appui
