// ===========================================================================
// packprops.cpp —— 见 packprops.h
// ===========================================================================
#include "packprops.h"
#include "../../common/appui.h"

#include <algorithm>

namespace packprops {

static const wchar_t* kCls = L"NppDockPackPropsWnd";

// v1.8：**弹窗也用等宽字体**（王的要求："弹窗的字体也用 console"）。
// 拿不到 Consolas 就退回界面字体 —— 宁可不那么统一，也不能没字。
static HFONT MakeDlgFont()
{
    HFONT f = appui::MakeFont(L"Consolas", 0, /*mono*/ true, false);
    return f ? f : appui::MakeUiFont();
}

// 控件 id 排布：1000+i 标签 / 2000+i 值 / 3000+i 复制(图标) / 9001 复制全部 / 9003 提示
// ★ v1.9.2：**「关闭」按钮删掉了** —— 现在「复制全部」复制完就关窗（王的要求），
//   另外标题栏的 × 、Esc、以及"点复制全部"都能关。
enum { kIdCopyAll = 9001, kIdHint = 9003 };

struct State {
    std::vector<Row> rows;
    HWND             hHint = nullptr;
    // ⚠️ 字体要**留到窗口销毁**再删：以前是"WM_SETFONT 之后立刻 DeleteObject"，
    //    结果控件手里那个句柄已经失效 —— 画出来的字是 GDI 兜底的，
    //    外部（探针）读 WM_GETFONT 也读不到东西。规矩是"谁用谁负责到用完"。
    HFONT            hFont = nullptr;
};

static State* StateOf(HWND h)
{
    return (State*)::GetWindowLongPtrW(h, GWLP_USERDATA);
}

static void CopyText(HWND owner, const std::wstring& text)
{
    if (!::OpenClipboard(owner)) return;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = ::GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            ::GlobalUnlock(h);
            ::SetClipboardData(CF_UNICODETEXT, h);      // 成功后归属权交给系统
        } else {
            ::GlobalFree(h);
        }
    }
    ::CloseClipboard();
}

std::wstring Format(const Row& r)
{
    return r.label + L"：" + r.value;
}

static void Hint(HWND h, const std::wstring& s)
{
    State* st = StateOf(h);
    if (st && st->hHint) ::SetWindowTextW(st->hHint, s.c_str());
}

static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    State* st = StateOf(h);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        State* s = (State*)cs->lpCreateParams;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        st = s;

        HFONT f = MakeDlgFont();
        s->hFont = f;                       // 见 State::hFont 的注释
        // ★ v1.9.1：整套尺寸收紧一档（王："文件信息的弹窗布局可以更紧凑"）——
        //   外边距 14→10、行高 28→22（+6）、标签列 96→84、复制钮 58→46、
        //   底栏 30→24。字号**没动**（字号是配置里的"界面字号"，动它会牵一发动全身）。
        const int pad   = appui::Dp(10);
        const int gap   = appui::Dp(6);
        const int rowH  = (std::max)(appui::Dp(22), appui::FontCellHeight(f) + appui::Dp(6));
        // ★ v1.9.2：标签列**按实际标签文字量出来**（王："四个标签与后面的内容物靠近一点"）。
        //   以前写死 84，短标签（"名称"两个字）右边会甩出一大片空白，
        //   看着就像标签和值之间隔了老远。
        int labW = 0;
        if (f) {
            if (HDC dc = ::GetDC(h)) {
                HGDIOBJ of = ::SelectObject(dc, f);
                for (const auto& r : st->rows) {
                    SIZE sz{};
                    if (::GetTextExtentPoint32W(dc, r.label.c_str(), (int)r.label.size(), &sz) &&
                        sz.cx > labW)
                        labW = sz.cx;
                }
                ::SelectObject(dc, of);
                ::ReleaseDC(h, dc);
            }
        }
        const int labelW = labW + gap;      // 标签正好占满自己的控件，右边只留 gap
        const int copyW  = rowH;            // 复制按钮 = 正方形（里面只有一个图标）
        RECT rc{};
        ::GetClientRect(h, &rc);
        const int valueW = (std::max)(appui::Dp(80),
                                      (int)rc.right - pad * 2 - labelW - copyW - gap);
        int y = pad;

        for (size_t i = 0; i < st->rows.size(); ++i) {
            // 值框和标签**贴齐同一行的上下沿**（以前值框还往下错 1px，白占地方）
            HWND lab = ::CreateWindowExW(0, L"STATIC", st->rows[i].label.c_str(),
                                         WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                         pad, y, labelW - gap, rowH,
                                         h, (HMENU)(INT_PTR)(1000 + i), cs->hInstance, nullptr);
            // 值用**只读 EDIT**：用户能自己划选、Ctrl+C；旁边再给一个「复制」更省事
            HWND val = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->rows[i].value.c_str(),
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY
                                         | ES_AUTOHSCROLL | ES_LEFT,
                                         pad + labelW, y, valueW, rowH,
                                         h, (HMENU)(INT_PTR)(2000 + i), cs->hInstance, nullptr);
            // ★ v1.9.2：行内的「复制」改成**图标按钮**（王的要求），所以用 owner-draw
            HWND btn = ::CreateWindowExW(0, L"BUTTON", L"复制",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         pad + labelW + valueW + gap, y,
                                         copyW, rowH,
                                         h, (HMENU)(INT_PTR)(3000 + i), cs->hInstance, nullptr);
            if (f) {
                ::SendMessageW(lab, WM_SETFONT, (WPARAM)f, TRUE);
                ::SendMessageW(val, WM_SETFONT, (WPARAM)f, TRUE);
                ::SendMessageW(btn, WM_SETFONT, (WPARAM)f, TRUE);
            }
            y += rowH;
        }

        // 底部：左边一句提示（"已复制"），右边**只剩「复制全部」**
        //（它同时是默认按钮：回车 = 复制全部并关窗）
        const int btnH = appui::Dp(24);
        const int btnW = appui::Dp(78);
        const int fy   = y + appui::Dp(8);
        st->hHint = ::CreateWindowExW(0, L"STATIC", L"",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                      pad, fy, rc.right - pad * 2 - (btnW + gap), btnH,
                                      h, (HMENU)(INT_PTR)kIdHint, cs->hInstance, nullptr);
        HWND all = ::CreateWindowExW(0, L"BUTTON", L"复制全部",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                     rc.right - pad - btnW, fy, btnW, btnH,
                                     h, (HMENU)(INT_PTR)kIdCopyAll, cs->hInstance, nullptr);
        if (f) {
            ::SendMessageW(st->hHint, WM_SETFONT, (WPARAM)f, TRUE);
            ::SendMessageW(all, WM_SETFONT, (WPARAM)f, TRUE);
        }
        return 0;
    }

    case WM_DRAWITEM: {
        // 行内那些「复制」是 owner-draw 的**图标按钮**（v1.9.2）
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (!dis) break;
        if (dis->CtlID >= 3000 && dis->CtlID < 3000 + 500) {
            appui::DrawGlyphButton(*dis, appui::DrawCopyGlyph, false);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == kIdCopyAll && st) {
            std::wstring all;
            for (size_t i = 0; i < st->rows.size(); ++i) {
                if (i) all += L"\r\n";
                all += Format(st->rows[i]);
            }
            CopyText(h, all);
            // ★ v1.9.2：复制完全部就**关窗**（王的要求）。
            //   没有提示语了 —— 窗都关了，谁也看不见那句话。
            ::SendMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (id >= 3000 && id < 3000 + 500 && st) {
            const size_t i = (size_t)(id - 3000);
            if (i < st->rows.size()) {
                CopyText(h, Format(st->rows[i]));
                Hint(h, L"已复制「" + st->rows[i].label + L"」");
            }
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        ::SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { ::SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
        break;

    case WM_CLOSE:
        ::DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        // 注意：**不 PostQuitMessage** —— 这是应用里的一个附属窗口，
        // 关掉它不能让整个应用退出（Show() 的嵌套循环靠 IsWindow 判断结束）。
        if (st && st->hFont) { ::DeleteObject(st->hFont); st->hFont = nullptr; }
        return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

void Show(HWND owner, const std::wstring& title, const std::vector<Row>& rows)
{
    static bool  reg = false;
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = Proc;
        wc.hInstance     = hi;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kCls;
        wc.style         = CS_DBLCLKS;
        if (!::RegisterClassExW(&wc)) return;
        reg = true;
    }

    // 尺寸必须和 WM_CREATE 里那一套**完全一致**（宽/行高/上下留白），
    // 否则底部的按钮会被挤到客户区外面去。改一处就得两处一起改。
    const int pad   = appui::Dp(10);
    HFONT fnt0      = MakeDlgFont();
    const int rowH  = (std::max)(appui::Dp(22), appui::FontCellHeight(fnt0) + appui::Dp(6));
    if (fnt0) ::DeleteObject(fnt0);
    const int w     = appui::Dp(460);
    const int hh    = pad * 2 + (int)rows.size() * rowH + appui::Dp(8) + appui::Dp(24) + appui::Dp(10);

    // 居中到 owner（owner 可能是嵌入态的**子窗口**，所以要用屏幕坐标算）
    int x = 200, y = 200;
    if (owner && ::IsWindow(owner)) {
        RECT o{};
        ::GetWindowRect(owner, &o);
        x = o.left + ((o.right - o.left) - w) / 2;
        y = o.top + ((o.bottom - o.top) - hh) / 3;
    }
    RECT fit{ 0, 0, w, hh };
    ::AdjustWindowRectEx(&fit, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    State st;
    st.rows = rows;
    HWND h = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kCls, title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, fit.right - fit.left, fit.bottom - fit.top,
                               owner, nullptr, hi, &st);
    if (!h) return;

    if (owner) ::EnableWindow(owner, FALSE);
    ::ShowWindow(h, SW_SHOW);
    ::SetFocus(h);

    MSG msg;
    while (::IsWindow(h) && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (!::IsDialogMessageW(h, &msg)) {      // 让 Tab / Enter / Esc 有正常行为
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    if (owner && ::IsWindow(owner)) {
        ::EnableWindow(owner, TRUE);
        ::SetActiveWindow(owner);
    }
}

// ---------------------------------------------------------------------------
// v1.7：单行输入小窗（重命名）
// ---------------------------------------------------------------------------
static const wchar_t* kPromptCls = L"NppDockPackPromptWnd";
enum { kPrEdit = 4001, kPrOk = 4002, kPrCancel = 4003, kPrLabel = 4004 };

struct PromptState {
    std::wstring value;
    bool         accepted = false;
    HWND         hEdit = nullptr;
    HFONT        hFont = nullptr;      // 同 State::hFont：留到窗口销毁再删
};

static LRESULT CALLBACK PromptProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    PromptState* st = (PromptState*)::GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        PromptState* s = (PromptState*)cs->lpCreateParams;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        st = s;

        RECT rc{};
        ::GetClientRect(h, &rc);
        HFONT f = MakeDlgFont();
        s->hFont = f;
        const int pad = appui::Dp(14);
        const int rowH = (std::max)(appui::Dp(26), appui::FontCellHeight(f) + appui::Dp(8));

        HWND lab = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     pad, pad, rc.right - pad * 2, appui::Dp(20),
                                     h, (HMENU)(INT_PTR)kPrLabel, cs->hInstance, nullptr);
        HWND ed = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    pad, pad + appui::Dp(22), rc.right - pad * 2, rowH,
                                    h, (HMENU)(INT_PTR)kPrEdit, cs->hInstance, nullptr);
        const int by = pad + appui::Dp(22) + rowH + appui::Dp(14);
        HWND ok = ::CreateWindowExW(0, L"BUTTON", L"确定",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    rc.right - pad - appui::Dp(96) * 2 - appui::Dp(8), by,
                                    appui::Dp(96), appui::Dp(30),
                                    h, (HMENU)(INT_PTR)kPrOk, cs->hInstance, nullptr);
        HWND ca = ::CreateWindowExW(0, L"BUTTON", L"取消",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                    rc.right - pad - appui::Dp(96), by,
                                    appui::Dp(96), appui::Dp(30),
                                    h, (HMENU)(INT_PTR)kPrCancel, cs->hInstance, nullptr);
        for (HWND c : { lab, ed, ok, ca })
            if (f) ::SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        st->hEdit = ed;
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == kPrCancel) { ::SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (id == kPrOk && st) {
            const int n = ::GetWindowTextLengthW(st->hEdit);
            std::wstring s((size_t)(std::max)(n + 1, 8), L'\0');
            const int got = ::GetWindowTextW(st->hEdit, &s[0], (int)s.size());
            s.resize((size_t)(got > 0 ? got : 0));
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
            while (!s.empty() && (s.back()  == L' ' || s.back()  == L'\t')) s.pop_back();
            if (s.empty()) {                       // 不许留空：直接无视这次「确定」
                ::MessageBeep(MB_ICONWARNING);
                return 0;
            }
            st->value = s;
            st->accepted = true;
            ::SendMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        ::DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        // 同属性窗：**不 PostQuitMessage**，这里只是应用的一个附属窗口
        if (st && st->hFont) { ::DeleteObject(st->hFont); st->hFont = nullptr; }
        return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

bool Prompt(HWND owner, const std::wstring& title, const std::wstring& label,
            const std::wstring& initial, std::wstring& value)
{
    static bool reg = false;
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = PromptProc;
        wc.hInstance     = hi;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = kPromptCls;
        if (!::RegisterClassExW(&wc)) return false;
        reg = true;
    }

    const int pad = appui::Dp(14);
    HFONT f0 = MakeDlgFont();
    const int rowH = (std::max)(appui::Dp(26), appui::FontCellHeight(f0) + appui::Dp(8));
    if (f0) ::DeleteObject(f0);
    const int w  = appui::Dp(460);
    const int hh = pad * 2 + appui::Dp(22) + rowH + appui::Dp(14) + appui::Dp(30) + appui::Dp(10);

    int x = 200, y = 200;
    if (owner && ::IsWindow(owner)) {
        RECT o{};
        ::GetWindowRect(owner, &o);
        x = o.left + ((o.right - o.left) - w) / 2;
        y = o.top + ((o.bottom - o.top) - hh) / 3;
    }
    RECT fit{ 0, 0, w, hh };
    ::AdjustWindowRectEx(&fit, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    PromptState st;
    HWND h = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kPromptCls, title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, fit.right - fit.left, fit.bottom - fit.top,
                               owner, nullptr, hi, &st);
    if (!h) return false;

    ::SetWindowTextW(::GetDlgItem(h, kPrLabel), label.c_str());
    ::SetWindowTextW(::GetDlgItem(h, kPrEdit), initial.c_str());
    ::SendMessageW(::GetDlgItem(h, kPrEdit), EM_SETSEL, 0, -1);   // 全选：直接打字即替换

    if (owner) ::EnableWindow(owner, FALSE);
    ::ShowWindow(h, SW_SHOW);
    ::SetFocus(::GetDlgItem(h, kPrEdit));

    MSG msg;
    while (::IsWindow(h) && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (!::IsDialogMessageW(h, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    if (owner && ::IsWindow(owner)) {
        ::EnableWindow(owner, TRUE);
        ::SetActiveWindow(owner);
    }
    if (st.accepted) value = st.value;
    return st.accepted;
}

} // namespace packprops