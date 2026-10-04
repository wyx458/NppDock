// ============================================================================
// nettest.cpp —— 网络测试（NppDock 可嵌入应用）· v1.2 界面重做
// ----------------------------------------------------------------------------
// 王的界面要求（原文）：
//   「只保留以下 4 个测试功能，四个按钮等宽：Ping测试 / 路由追踪 / 端口扫描 / DNS查询；
//     host [ ip地址/域名下拉框 ▾ ]   ⚙【清空】；
//     四个功能全部选择简单且合理的参数；某个按钮按下后其他按钮变灰、它自己变成取消，
//     直到测试结束或取消；小齿轮弹出配置文件；最近访问的 host 自动写进配置文件。
//     配置文件里 host 有三处：上次的 / 近期的（下拉框最上面 5 个）/ 常用的（最下面 5 个）」
//
// 与 v1.1 的差别（几乎都在界面层，引擎没怎么动）：
//   · 参数下拉框**全删**：参数都在 JSON 配置文件里（点小齿轮打开），界面上一个不留；
//   · 命令下拉框**换成 4 个等宽按钮**，带"运行中互斥 + 自己变成取消"；
//   · 左下角那行状态信息**去掉**（王说不需要），所有提示一律进输出区；
//   · 输出区改成**等宽（Consolas）小一号字**，并支持 Ctrl+A 全选。
//
// 三层结构（谁都不认识上层）：
//     nettest.cpp（本文件）  UI：窗口、按钮、输出、什么时候读写配置
//     netcfg.h/.cpp          配置：JSON 读写 + host 三处记录 + 端口串解析
//     netcore.h/.cpp         引擎：跑一条测试，一行一行吐结果
//
// 必须守的规矩（违反过的都写在开发指南 R1~R11 / 踩坑记录里，别踩第二遍）：
//   R1  解析命令行用 GetCommandLineW（lpCmdLine 不含程序名）
//   R2  建**任何**窗口之前先按宿主对齐 DPI 感知
//   R3  子窗口化顺序：改样式 -> SetParent -> SetWindowPos
//   R4  布局弹性，很窄很矮也要能用
//   R5  WM_CLOSE -> DestroyWindow -> PostQuitMessage
//   R6  重绘自己负责，**永不**跨进程同步重画（不用 RDW_UPDATENOW）
//   R7  FileDescription 就是 dock 里的标签标题
//   R10 运行期要填的静态表别声明 const（进只读段，一写就崩）
// ============================================================================

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "netcore.h"
#include "netcfg.h"

// ---------------------------------------------------------------------------
// 控件 ID
// ---------------------------------------------------------------------------
// ⚠️ dock_app_probe.py 的 nettest 子命令按这些编号直接读控件（GetDlgItem）。
//    改编号会让探针失效 —— 要加控件请往后排。
enum {
    IDC_HOST_LABEL  = 100,   // "host" 标签
    IDC_HOST_COMBO  = 101,   // 目标（可输入 + 历史下拉）
    IDC_GEAR_BTN    = 102,   // ⚙ 打开配置文件
    IDC_CLEAR_BTN   = 103,   // 清空输出
    IDC_TEST_BASE   = 104,   // 104..107 = 四个测试按钮（顺序见 kTests）
    IDC_OUTPUT_EDIT = 130,   // 输出（只读、多行、等宽小字）
};

// 四个测试按钮（顺序 = 界面从左到右）
struct TestSpec {
    netcore::Kind  kind;
    const wchar_t* label;      // 空闲时的文案
};
static const TestSpec kTests[] = {
    { netcore::Kind::Ping,     L"Ping测试" },
    { netcore::Kind::Tracert,  L"路由追踪" },
    { netcore::Kind::PortScan, L"端口扫描" },
    { netcore::Kind::Dns,      L"DNS查询"  },
};
static const int kTestCount = (int)(sizeof(kTests) / sizeof(kTests[0]));

// 运行中那个按钮的文案（比任何标签都长，算按钮宽度时要算上它）
static const wchar_t* kCancelText  = L"取消";
static const wchar_t* kCancelingText = L"正在取消…";

// ---------------------------------------------------------------------------
// 崩溃定位（发布版保留）：启动轨迹 + 未处理异常都写进 exe 旁边的 .trace.log。
// GUI 模式一旦"窗口没起来 / 起来又没了"，这个文件直接能看出死在哪一步
// （这套东西就是那次"启动即崩 c000041d"逼出来的，别删）。
// ---------------------------------------------------------------------------
// v2.0：轨迹日志也要滚。它以前只追加、永不清理 —— 每次启动都写几行，
// 几年下来就是个大文件。app 是随标签页生灭的短命进程，**启动时查一次**就够。
static void RotateTraceOnce(const std::wstring& f)
{
    static bool done = false;
    if (done) return;
    done = true;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad)) return;
    const unsigned long long sz =
        ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (sz < 1024ull * 1024ull) return;
    const std::wstring old = f + L".old";
    ::DeleteFileW(old.c_str());
    ::MoveFileW(f.c_str(), old.c_str());
}

static void TraceLog(const char* s)
{
    wchar_t buf[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    p = p.substr(0, slash + 1) + L"NppDockApp_NET.trace.log";
    RotateTraceOnce(p);
    HANDLE h = ::CreateFileW(p.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    SYSTEMTIME st{}; ::GetLocalTime(&st);
    char head[48];
    sprintf_s(head, "[%02u:%02u:%02u.%03u] ",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    ::WriteFile(h, head, (DWORD)strlen(head), &wrote, nullptr);
    ::WriteFile(h, s, (DWORD)strlen(s), &wrote, nullptr);
    ::WriteFile(h, "\r\n", 2, &wrote, nullptr);
    ::CloseHandle(h);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord) {
        char buf[160];
        sprintf_s(buf, "CRASH code=0x%08lX addr=%p",
                  (unsigned long)ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress);
        TraceLog(buf);
    } else {
        TraceLog("CRASH code=unknown");
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 让 WER 照常收尾
}

// ===========================================================================
// 全局
// ===========================================================================
static HINSTANCE g_hInst    = nullptr;
static HWND      g_hWnd     = nullptr;
static bool      g_embedded = false;
static UINT      g_dpi      = 96;

static HWND g_hHostLabel = nullptr;
static HWND g_hHostCombo = nullptr;
static HWND g_hGearBtn   = nullptr;
static HWND g_hClearBtn  = nullptr;
static HWND g_hTestBtn[kTestCount] = {};
static HWND g_hOutput    = nullptr;

static HFONT g_hUiFont  = nullptr;   // 按钮/标签：系统 UI 字体
static HFONT g_hOutFont = nullptr;   // 输出区：等宽 + 小一号

static netcfg::Config g_cfg;         // 配置内容（界面唯一的参数来源）
static std::wstring   g_cfgErr;      // 读配置时的问题（要显示出来）
static bool           g_cfgCreated = false;

// 运行状态：同一时刻**只允许一条测试**在跑（王的要求：按下一个，其余变灰）
static bool g_busy            = false;
static int  g_activeTest      = -1;      // 正在跑的是第几个按钮（-1 = 没在跑）
static bool g_cancelRequested = false;

static const wchar_t* kClassName = L"NppDockNetTestWnd";
static const UINT kMsgSelfRepaint = WM_APP + 0x51;
static bool g_selfRepaintPosted = false;

// 【别再把这两行加回来】曾经想用 NPPM_DOOPEN 让宿主打开配置文件，结果
// 把 Notepad++ 写崩了（lParam 是指针，SendMessage 跨进程不封送 —— 踩坑记录 8.5）。
// 现在改用"拿宿主 exe 路径 + CreateProcess"，全程不带指针。见 OpenFileForUser()。

// 前向声明
static void ApplyFont();
static void LayoutChildren(int W, int H);
static void UpdateButtons();
static void LoadConfigIntoUi();

// ===========================================================================
// 小工具
// ===========================================================================
static std::wstring WindowTextOf(HWND h)
{
    if (!h) return L"";
    const int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s((size_t)n + 1, L'\0');
    const int got = ::GetWindowTextW(h, &s[0], n + 1);
    s.resize(got > 0 ? (size_t)got : 0);
    return s;
}

static std::wstring TrimW(std::wstring s)
{
    auto issp = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

static std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                        nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                     &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

static int Dp(int v) { return ::MulDiv(v, (int)g_dpi, 96); }

static UINT DetectDpi(HWND h)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pFn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (HMODULE u = ::GetModuleHandleW(L"user32.dll"))
            pFn = (PFN_GetDpiForWindow)::GetProcAddress(u, "GetDpiForWindow");
    }
    UINT d = 0;
    if (pFn && h) d = pFn(h);
    if (!d) {
        if (HDC dc = ::GetDC(nullptr)) {
            d = (UINT)::GetDeviceCaps(dc, LOGPIXELSX);
            ::ReleaseDC(nullptr, dc);
        }
    }
    return d ? d : 96;
}

// R2：建窗口**之前**把本进程 DPI 感知设成与宿主一致。
//     跨进程 SetParent 会触发 forced reset，不同步的话子窗口只铺满 2/3（踩过）。
static void AdoptHostDpiAwareness(HWND hHost)
{
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) return;
    auto pGet = (HANDLE (WINAPI *)(HWND))
        ::GetProcAddress(u, "GetWindowDpiAwarenessContext");
    auto pSet = (BOOL (WINAPI *)(HANDLE))
        ::GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (pGet && pSet && hHost) {
        HANDLE ctx = pGet(hHost);
        if (ctx && pSet(ctx)) return;
    }
    // ② 独立运行：per-monitor-v2（== -4）。先试 SetProcessDpiAwarenessContext。
    if (pSet && pSet((HANDLE)(INT_PTR)-4)) return;

    // ③ ⚠️ 老系统兜底：SetProcessDpiAwareness 在 **shcore.dll**，
    //    不是 user32.dll —— 之前一直在 user32 里找，永远拿到 NULL，
    //    于是独立运行时进程是 unaware（窗口被系统拉伸、字发虚）。
    if (HMODULE sh = ::LoadLibraryW(L"shcore.dll")) {
        auto p2 = (HRESULT (WINAPI *)(int))::GetProcAddress(sh, "SetProcessDpiAwareness");
        if (p2 && p2(2) == 0) return;
    }
    if (auto p3 = (BOOL (WINAPI *)())::GetProcAddress(u, "SetProcessDPIAware")) p3();
}

// ---------------------------------------------------------------------------
// 消息泵 / 重绘（R6：只作废，永不 RDW_UPDATENOW）
// ---------------------------------------------------------------------------
static void PumpMessages()
{
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { ::PostQuitMessage(0); return; }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

static void PostSelfRepaint(HWND hwnd)
{
    if (g_selfRepaintPosted) return;
    g_selfRepaintPosted = true;
    ::PostMessageW(hwnd, kMsgSelfRepaint, 0, 0);
}

static void RepaintSubtree(HWND hwnd)
{
    ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// 字体：界面一套、输出区一套（等宽，**与界面同号**）
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI* PFN_SpiForDpi)(UINT, UINT, PVOID, UINT, UINT);

// 字体的"字符高度"（像素）。lfHeight 为负 = 按字符高度算，这里统一取正数。
static int FontCellHeight(HFONT f)
{
    if (!f) return 0;
    int h = 0;
    HDC dc = ::GetDC(g_hWnd);
    if (!dc) return 0;
    HGDIOBJ old = ::SelectObject(dc, f);
    TEXTMETRICW tm{};
    if (::GetTextMetricsW(dc, &tm)) h = (int)tm.tmHeight;
    if (old) ::SelectObject(dc, old);
    ::ReleaseDC(g_hWnd, dc);
    return h;
}

// 界面字体 = 当前 DPI 下的系统消息字体。
//
// ⚠️⚠️ 这里踩过一个坑（王一眼看出来的：「四个按钮特别大、很奇怪」）：
//      **不要**把 SPI_GETNONCLIENTMETRICS 给的字号再乘一次 g_dpi/96！
//      系统给的字号本身就已经按 DPI 缩放过了，乘第二遍正好放大 1.5 倍
//      （144 DPI 下 9pt 的字被做成 13.5pt，按钮跟着被撑成 50px 高）。
//      正确做法：用 SystemParametersInfoForDpi(..., g_dpi) 要一份
//      **当前 DPI** 的度量直接用；只有老系统没这个 API 时，才退回
//      「拿到的是系统 DPI 的值 → 按 系统DPI→当前DPI 补正**一次**」。
static HFONT MakeUiFont()
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    bool got = false;
    if (HMODULE u = ::GetModuleHandleW(L"user32.dll")) {
        auto forDpi = (PFN_SpiForDpi)::GetProcAddress(u, "SystemParametersInfoForDpi");
        if (forDpi)
            got = forDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi) != FALSE;
    }
    if (!got) {
        if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return nullptr;
        HDC dc = ::GetDC(nullptr);
        UINT sysDpi = dc ? (UINT)::GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ::ReleaseDC(nullptr, dc);
        if (sysDpi && sysDpi != g_dpi && ncm.lfMessageFont.lfHeight) {
            ncm.lfMessageFont.lfHeight =
                ::MulDiv(ncm.lfMessageFont.lfHeight, (int)g_dpi, (int)sysDpi);
        }
    }
    return ::CreateFontIndirectW(&ncm.lfMessageFont);
}

// 输出区字体：**等宽**（Consolas 优先）、字号与界面**同号**。
// 为什么坚持等宽：arp / netstat / tracert 的输出是靠 %-16s 这类格式对齐的列，
// 换成比例字体就参差不齐，「看表」变成「读散文」。
//
// ⚠️ 字号取界面字体的 **em（lfHeight）**，不是它的 tmHeight。
//    tmHeight 里含着字体自己的行距，而等宽字体的行距比中文字体小得多，
//    照搬 tmHeight 会让输出区的字比界面上的字**大一圈**（实测 18 的界面
//    配出 24 的等宽，一眼就看得出来"不是一个字号"）。
//
// ⚠️ CreateFontIndirect 在字体不存在时**不报错**，会静默换成别的字体，
//    所以必须把真实字体名读回来核对，不能「创建成功就当成了」。
static HFONT MakeOutFont(HFONT uiFont)
{
    if (!uiFont) return nullptr;
    LOGFONTW uiLf{};
    if (::GetObjectW(uiFont, sizeof(uiLf), &uiLf) != sizeof(uiLf)) return nullptr;

    int em = uiLf.lfHeight;
    if (em == 0) em = -FontCellHeight(uiFont);      // 兜底（理论上不会走到）
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

        bool ok = false;
        if (HDC dc = ::GetDC(g_hWnd)) {
            HGDIOBJ old = ::SelectObject(dc, f);
            wchar_t gotFace[LF_FACESIZE]{};
            ::GetTextFaceW(dc, LF_FACESIZE, gotFace);
            if (old) ::SelectObject(dc, old);
            ::ReleaseDC(g_hWnd, dc);
            ok = (_wcsicmp(gotFace, kMono[i]) == 0);
        }
        if (ok) return f;
        ::DeleteObject(f);
    }
    return nullptr;   // 一个等宽字体都拿不到 → 调用方退回界面字体
}

static void ApplyFont()
{
    HFONT newUi = MakeUiFont();
    if (!newUi) return;
    HFONT newOut = MakeOutFont(newUi);      // 可能为 nullptr（没等宽字体）

    HFONT oldUi = g_hUiFont, oldOut = g_hOutFont;
    g_hUiFont  = newUi;
    g_hOutFont = newOut;

    auto set = [](HWND h, HFONT f) {
        if (h && f) ::SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    };
    set(g_hHostLabel, newUi);
    set(g_hHostCombo, newUi);
    set(g_hGearBtn,   newUi);
    set(g_hClearBtn,  newUi);
    for (int i = 0; i < kTestCount; ++i) set(g_hTestBtn[i], newUi);
    set(g_hOutput,    newOut ? newOut : newUi);

    // 顺序：先把新字体装到所有控件上、**再**删旧的 —— 反过来控件会短暂
    // 引用一个已释放的 GDI 对象（「文件校验」那边的注释记过这条）。
    if (oldOut && oldOut != oldUi) ::DeleteObject(oldOut);
    if (oldUi) ::DeleteObject(oldUi);
}


// ---------------------------------------------------------------------------
// 输出区：攒成一个大字符串再整块 SetWindowText（逐行设置是 O(n²)）
// ---------------------------------------------------------------------------
static std::wstring g_outBuf;
static size_t       g_outFlushed = 0;
static DWORD        g_lastFlush  = 0;
static const size_t kMaxOutChars = 400000;   // 防"扫大网段"把内存吃干
// v2.0：从头部被丢掉的整行数。头部那行提示是**渲染时才拼**的，
// 不进 g_outBuf —— 否则每 flush 一次就多写一行。
static int          g_omittedLines = 0;

static void FlushOutput(bool force)
{
    if (!g_hOutput) return;
    if (!force) {
        const DWORD now = ::GetTickCount();
        if (now - g_lastFlush < 80) return;      // 节流
        g_lastFlush = now;
    } else {
        g_lastFlush = ::GetTickCount();
    }

    std::wstring shown;
    if (g_omittedLines > 0) {
        wchar_t head[96];
        swprintf_s(head, L"（前面已省略 %d 行）\r\n", g_omittedLines);
        shown = head;
    }
    shown += g_outBuf;

    ::SetWindowTextW(g_hOutput, shown.c_str());
    ::SendMessageW(g_hOutput, EM_SETSEL, (WPARAM)shown.size(), (LPARAM)shown.size());
    ::SendMessageW(g_hOutput, EM_SCROLLCARET, 0, 0);
    g_outFlushed = shown.size();
}

// v2.0：到上限时**丢最旧的整行、保住尾巴**。
// 老写法是"超了就直接 return"，于是后面的统计行、"（已终止）"全被无声吃掉；
// 而那句一次性提示写在 g_outFlushed == 0 的分支里，可 FlushOutput 每次都会把
// 它置成非零 —— 提示实际上永远不会出现。长扫描/连续 ping 就"跑着跑着不动了"。
static void AppendLine(const std::wstring& s)
{
    const size_t add = s.size() + 2;
    if (add >= kMaxOutChars) {
        // 单行就超上限（极端情况）：只留它的尾巴，注明丢了前面
        g_outBuf.assign(s.substr(0, kMaxOutChars / 2));
        g_outBuf += L"\r\n";
        ++g_omittedLines;
        return;
    }
    while (g_outBuf.size() + add > kMaxOutChars) {
        const size_t eol = g_outBuf.find(L"\r\n");
        if (eol == std::wstring::npos) { g_outBuf.clear(); break; }
        g_outBuf.erase(0, eol + 2);
        ++g_omittedLines;
    }
    g_outBuf += s;
    g_outBuf += L"\r\n";
}

// 没有状态行了：所有"提示"都进输出区（王要求去掉左下角那行）
static void Say(const std::wstring& s)
{
    AppendLine(s);
    FlushOutput(true);
}

// ===========================================================================
// 配置 -> 界面 / 界面 -> 参数
// ===========================================================================
static void RefillHostCombo()
{
    if (!g_hHostCombo) return;
    const std::wstring keep = WindowTextOf(g_hHostCombo);   // 重建不能把用户敲的字弄丢
    ::SendMessageW(g_hHostCombo, CB_RESETCONTENT, 0, 0);
    for (const std::string& h : netcfg::ComboEntries(g_cfg)) {
        const std::wstring w = Utf8ToWide(h);
        ::SendMessageW(g_hHostCombo, CB_ADDSTRING, 0, (LPARAM)w.c_str());
    }
    // ① 上次的 host —— 直接填在输入框里
    if (!keep.empty())            ::SetWindowTextW(g_hHostCombo, keep.c_str());
    else if (!g_cfg.last.empty()) ::SetWindowTextW(g_hHostCombo, Utf8ToWide(g_cfg.last).c_str());
}

static void LoadConfigIntoUi()
{
    bool created = false;
    g_cfgErr.clear();
    netcfg::Load(g_cfg, g_cfgErr, created);
    g_cfgCreated = created;
    RefillHostCombo();
}

// 把界面上的选择变成引擎参数（参数全部来自配置文件）
static void BuildParams(netcore::Kind kind, netcore::Params& p)
{
    p = netcore::Params{};

    std::wstring t = TrimW(WindowTextOf(g_hHostCombo));
    // 手输的一般就是单个地址；两端成对引号去掉（从别处粘过来常见）
    if (t.size() >= 2) {
        const wchar_t a = t.front(), b = t.back();
        if ((a == L'"' && b == L'"') || (a == L'\'' && b == L'\''))
            t = TrimW(t.substr(1, t.size() - 2));
    }
    p.target = t;

    switch (kind) {
    case netcore::Kind::Ping:
        p.count      = g_cfg.pingCount;
        p.intervalMs = g_cfg.pingIntervalMs;
        p.timeoutMs  = g_cfg.pingTimeoutMs;
        p.payload    = g_cfg.pingPayload;
        break;
    case netcore::Kind::Tracert:
        p.maxHops      = g_cfg.traceMaxHops;
        p.probesPerHop = g_cfg.traceProbesPerHop;
        p.timeoutMs    = g_cfg.traceTimeoutMs;
        break;
    case netcore::Kind::PortScan: {
        bool usedDefault = false;
        p.ports       = netcfg::ParsePortSpec(g_cfg.portSpec, usedDefault);
        p.timeoutMs   = g_cfg.scanTimeoutMs;
        p.concurrency = g_cfg.scanConcurrency;
        if (usedDefault) {
            Say(L"提示：配置文件里 portscan.ports 是空的或写错了，"
                L"本次用内置的 100 个常用端口。");
        }
        break;
    }
    case netcore::Kind::Dns: {
        p.dnsType   = g_cfg.dnsType;
        p.timeoutMs = g_cfg.dnsTimeoutMs;      // v2.0：以前这里是硬写的 5000（假参数）
        p.dnsServer = 0;                       // 0 = 跟随系统
        // v2.0：配置文件里的 server 真正落地。只吃 IPv4 数字地址 ——
        // 引擎那边给 DnsQueryEx/DnsQuery_W 传的是 IP4_ARRAY，塞不进 IPv6。
        const std::wstring s = TrimW(Utf8ToWide(g_cfg.dnsServer));
        if (!s.empty()) {
            unsigned int netIp = 0;
            if (netcore::ParseIpv4Net(s, netIp)) {
                p.dnsServer = (int)netIp;      // 引擎按原样塞进 IP4_ARRAY
            } else {
                Say(L"提示：配置里的 dns.server「" + s +
                    L"」不是 IPv4 地址，本次跟随系统解析器。");
            }
        }
        break;
    }
    default:
        break;
    }
}

// ===========================================================================
// 引擎回调：吐一行 / 每轮问一次"要不要停"
// ===========================================================================
static void SinkEmit(void* /*user*/, const std::wstring& line)
{
    AppendLine(line);
    FlushOutput(false);
}

static bool SinkTick(void* /*user*/)
{
    PumpMessages();                       // 用户点「取消」就是在这一步被派发的
    if (g_hWnd && !::IsWindow(g_hWnd)) return false;
    return !g_cancelRequested;
}

// ===========================================================================
// 按钮状态：运行中互斥 + 自己变身「取消」
// ===========================================================================
static void UpdateButtons()
{
    for (int i = 0; i < kTestCount; ++i) {
        HWND h = g_hTestBtn[i];
        if (!h) continue;
        const bool isActive = (g_activeTest == i);
        ::SetWindowTextW(h, isActive ? kCancelText : kTests[i].label);
        // 运行中：只有"正在跑的那个"可点（它就是取消），其余全部变灰
        ::EnableWindow(h, (!g_busy || isActive) ? TRUE : FALSE);
    }
    // 顺便按住 host 行与齿轮/清空：跑一半改 host 没意义，清空会把正在写的输出抹掉
    const BOOL en = g_busy ? FALSE : TRUE;
    if (g_hHostCombo) ::EnableWindow(g_hHostCombo, en);
    if (g_hGearBtn)   ::EnableWindow(g_hGearBtn,   en);
    if (g_hClearBtn)  ::EnableWindow(g_hClearBtn,  en);
}

// ===========================================================================
// 动作
// ===========================================================================
// 拿某个窗口所属进程的 exe 全路径（用于"请 Notepad++ 打开这个文件"）
static std::wstring QueryProcessImagePath(HWND hwnd)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring out;
    if (::QueryFullProcessImageNameW(h, 0, buf, &n)) out = buf;
    ::CloseHandle(h);
    return out;
}

// 打开配置文件：能用 Notepad++ 就用 Notepad++（王选的），否则交给系统默认程序。
//
// ⚠️⚠️ 绝对**不要**发 NPPM_DOOPEN 给宿主 —— 那条消息的 lParam 是 `const wchar_t*`
//      **指针**，而 SendMessage 跨进程**不封送指针**：宿主会去解引用我们地址空间里的
//      那个地址，**直接把 Notepad++ 写崩**。实测后果：点一下齿轮，编辑器当场消失，
//      未保存的内容一起没了（踩坑记录 8.5 记的就是这条，我这次又踩了一遍）。
//      正确做法：用**不带指针**的途径 —— 拿宿主的 exe 路径，以命令行方式再起一个
//      进程。Notepad++ 是单实例，新进程会把文件名转交给已在运行的那个实例，
//      于是文件就在用户眼前打开，而且全程没有跨进程指针。
static void OpenFileForUser(const std::wstring& path)
{
    if (g_embedded) {
        HWND top = ::GetAncestor(g_hWnd, GA_ROOTOWNER);
        if (!top) top = ::GetAncestor(g_hWnd, GA_ROOT);
        wchar_t cls[64]{};
        if (top) ::GetClassNameW(top, cls, 64);
        if (top && _wcsicmp(cls, L"Notepad++") == 0) {
            const std::wstring exe = QueryProcessImagePath(top);
            if (!exe.empty()) {
                std::wstring cmd = L"\"" + exe + L"\" \"" + path + L"\"";
                std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
                mutableCmd.push_back(L'\0');
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                if (::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                     FALSE, 0, nullptr, nullptr, &si, &pi)) {
                    ::CloseHandle(pi.hThread);
                    ::CloseHandle(pi.hProcess);
                    return;
                }
            }
        }
        Say(L"（没能让 Notepad++ 打开配置文件，改用系统默认程序打开）");
    }
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void OnOpenConfig()
{
    const std::wstring path = netcfg::ConfigPath();
    netcfg::Save(g_cfg);          // 先把内存里的配置写回去，保证打开的是最新的
    OpenFileForUser(path);
}

static void OnClear()
{
    if (g_busy) return;                 // 运行中按钮本来就是灰的，这里再兜一层
    g_outBuf.clear();
    g_outFlushed = 0;
    g_omittedLines = 0;
    if (g_hOutput) ::SetWindowTextW(g_hOutput, L"");
}

static void OnTestButton(int idx)
{
    if (idx < 0 || idx >= kTestCount) return;

    if (g_busy) {
        // 运行中点到的只可能是"正在跑的那个"（其余已灰）：这就是取消
        if (g_activeTest == idx) {
            g_cancelRequested = true;
            ::SetWindowTextW(g_hTestBtn[idx], kCancelingText);
        }
        return;
    }

    const netcore::Kind kind = kTests[idx].kind;

    // ★ 每次开跑前**重新读一遍配置文件**：王的要求是"改配置文件就能改这些参数"，
    //   而小齿轮打开的就是那个文件 —— 他改完保存、回来点按钮，就该生效，
    //   不该要求"重启这个页"。文件只有一两 KB，读一次的开销可以忽略。
    //   （下拉框里用户敲的字由 RefillHostCombo 自己保留，不会被刷掉。）
    LoadConfigIntoUi();

    netcore::Params p;
    BuildParams(kind, p);

    if (netcore::NeedsTarget(kind) && p.target.empty()) {
        Say(L"请先填 host：可以手输 ip 或域名，也可以从下拉框里挑一个用过的。");
        ::SetFocus(g_hHostCombo);
        return;
    }

    // 记一次使用（更新配置文件的 hosts 三处记录）并落盘，然后刷新下拉框
    if (!p.target.empty()) {
        netcfg::RememberUse(g_cfg, WideToUtf8(p.target));
        netcfg::Save(g_cfg);
        RefillHostCombo();
        ::SetWindowTextW(g_hHostCombo, p.target.c_str());
    }

    g_busy            = true;
    g_activeTest      = idx;
    g_cancelRequested = false;
    UpdateButtons();

    g_outBuf.clear();
    g_outFlushed = 0;
    g_omittedLines = 0;                 // 新一轮 → 省略计数归零
    if (g_hOutput) ::SetWindowTextW(g_hOutput, L"");

    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        wchar_t head[256];
        swprintf_s(head, L"[%02d:%02d:%02d] %s   目标：%s",
                   st.wHour, st.wMinute, st.wSecond,
                   netcore::CommandName(kind),
                   p.target.empty() ? L"（本机）" : p.target.c_str());
        AppendLine(head);
        AppendLine(L"------------------------------------------------------------");
    }
    FlushOutput(true);

    netcore::Sink sink;
    sink.emit = &SinkEmit;
    sink.tick = &SinkTick;
    const bool finished = netcore::Run(kind, p, sink);

    AppendLine(L"");
    // ⚠️ v2.0：「（已终止）」由**引擎**在取消那一刻就写好了 —— netcore 里每条命令
    //    的取消分支都会 Emit 一行。这里以前又补一行，于是用户看到两条。
    //    `finished` 只用来表示"跑完了没有"，不再用它补文案。
    (void)finished;
    FlushOutput(true);

    g_busy       = false;
    g_activeTest = -1;
    if (::IsWindow(g_hWnd)) {
        UpdateButtons();
        ::SetFocus(g_hTestBtn[idx]);
    }
}

// ===========================================================================
// 输出区子类化：**只**为了 Ctrl+A
// ---------------------------------------------------------------------------
// 只读多行 EDIT 对 Ctrl+A 的支持在不同 Windows 版本上不一致（有的版本会响铃），
// 而王明确要求"输出区要能 Ctrl+A 全选"。自己接一下最省事：
// Ctrl+A -> EM_SETSEL(0, -1)（-1 = 到末尾），其余一律交回原过程。
// ===========================================================================
static WNDPROC g_outOrigProc = nullptr;

static LRESULT CALLBACK OutputEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == 'A' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
        ::SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return g_outOrigProc ? ::CallWindowProcW(g_outOrigProc, hwnd, msg, wp, lp)
                         : ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// 布局
// ===========================================================================
static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text,
                      DWORD style, int id, bool visible = true, DWORD exStyle = 0)
{
    HWND h = ::CreateWindowExW(exStyle, cls, text,
                               WS_CHILD | (visible ? WS_VISIBLE : 0) | style,
                               0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                               g_hInst, nullptr);
    if (h && g_hUiFont) ::SendMessageW(h, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
    return h;
}

// 一行**文字**的高度（不含按钮那种上下留白）。条目紧凑化要用它。
static int RowTextHeight()
{
    int textH = Dp(16);
    if (HDC dc = ::GetDC(g_hWnd)) {
        if (g_hUiFont) {
            HGDIOBJ old = ::SelectObject(dc, g_hUiFont);
            TEXTMETRICW tm{};
            if (::GetTextMetricsW(dc, &tm)) textH = tm.tmHeight;
            ::SelectObject(dc, old);
        }
        ::ReleaseDC(g_hWnd, dc);
    }
    return textH;
}

// 一行控件的高度：文字高度 + 按钮的上下留白
static int RowHeight()
{
    return RowTextHeight() + Dp(10);
}

static int TextWidth(const wchar_t* text)
{
    int w = 0;
    if (HDC dc = ::GetDC(g_hWnd)) {
        if (g_hUiFont) {
            HGDIOBJ old = ::SelectObject(dc, g_hUiFont);
            SIZE sz{};
            if (::GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz)) w = sz.cx;
            ::SelectObject(dc, old);
        }
        ::ReleaseDC(g_hWnd, dc);
    }
    return w;
}

// 四个按钮的等宽值：取"最宽标签"和"正在取消…"里更大的那个 + 内边距。
// ⚠️ 一定要把运行中会出现的最长文案也算进去，否则变身那一刻字被裁
//    （踩坑记录 16.5 就是这么来的）。
static int TestButtonWidth()
{
    int w = 0;
    for (int i = 0; i < kTestCount; ++i)
        w = (std::max)(w, TextWidth(kTests[i].label));
    w = (std::max)(w, TextWidth(kCancelingText));
    return w + Dp(24);
}

// ---------------------------------------------------------------------------
// 两个图标按钮（⚙ 配置 / 🗑 清空）：自绘
//
// 为什么不用「⚙」这类字符当图标：那种符号要靠字体链接去凑，换台机器、
// 换个字号就可能变成方框；而且和旁边的文字按钮风格也不搭。自己用线画，
// 任何机器、任何 DPI 都是同一个样子（线条风、黑白灰，与工具栏图标一致）。
// ---------------------------------------------------------------------------
static HPEN MakeIconPen(int width, COLORREF col)
{
    LOGBRUSH lb{};
    lb.lbStyle = BS_SOLID;
    lb.lbColor = col;
    // 圆头圆角：小尺寸下折角才不会裂开
    HPEN p = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND,
                            (DWORD)(width > 1 ? width : 1), &lb, 0, nullptr);
    return p ? p : ::CreatePen(PS_SOLID, width, col);
}

// 扳手（v1.4 起换掉齿轮）：王说齿轮画出来"像个灯泡"。
// 结构两笔：右上角一个**带缺口的环**（钳口）+ 一根斜手柄（左下）。
static void DrawWrenchGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 6 || h <= 6) return;
    const int m  = (std::min)(w, h);
    const int cx = rc.left + w / 2, cy = rc.top + h / 2;

    HPEN pen = MakeIconPen((std::max)(2, m / 9), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    const int penW = (std::max)(2, m / 9);

    const double kPi = 3.14159265358979;
    // 比例是调出来的：**环要小、手柄要长**。
    // 第一版把钳口半径做成 0.23m、手柄只到 0.34m，整体看着像**放大镜**；
    // 现在环 0.17m（直径只占图标 1/3）、手柄伸到 0.32m，才像一把扳手。
    const int r  = (int)(m * 0.17);
    const int jx = cx + m * 24 / 100, jy = cy - m * 24 / 100;   // 钳口中心（右上）

    // ① 钳口：开口的环（缺口 70°，朝右上 —— 正对着手柄的反方向）
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
    // ② 手柄：从钳口左下方（135°）斜着伸到左下
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

// 垃圾桶（= 清空输出区）：提手 + 盖 + 桶身 + 两条内线
static void DrawTrashGlyph(HDC dc, const RECT& rc, COLORREF col)
{
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 4 || h <= 4) return;
    const int l = rc.left, t = rc.top;
    auto X = [&](int per) { return l + w * per / 100; };
    auto Y = [&](int per) { return t + h * per / 100; };

    HPEN pen = MakeIconPen((std::max)(2, (std::min)(w, h) / 9), col);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBr  = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));

    ::MoveToEx(dc, X(40), Y(12), nullptr);            // 提手
    ::LineTo  (dc, X(60), Y(12));
    ::MoveToEx(dc, X(8),  Y(24), nullptr);            // 盖
    ::LineTo  (dc, X(92), Y(24));
    ::MoveToEx(dc, X(22), Y(24), nullptr);            // 桶身
    ::LineTo  (dc, X(28), Y(90));
    ::LineTo  (dc, X(72), Y(90));
    ::LineTo  (dc, X(78), Y(24));
    ::MoveToEx(dc, X(41), Y(38), nullptr);            // 内线
    ::LineTo  (dc, X(44), Y(78));
    ::MoveToEx(dc, X(59), Y(38), nullptr);
    ::LineTo  (dc, X(56), Y(78));

    ::SelectObject(dc, oldBr);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

// BS_OWNERDRAW 按钮的绘制：先画原生按钮框（含按下/禁用状态），再画线条图标
static void DrawIconButton(const DRAWITEMSTRUCT& dis)
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

    RECT gi = rc;
    if (pressed) ::OffsetRect(&gi, Dp(1), Dp(1));
    // ⚠️ 两边都要是 int：rc 的字段是 LONG，和 int 一起丢进 std::max 会没匹配（编译期报错）
    const int inset = (std::max)(Dp(5), (int)(rc.bottom - rc.top) / 5);
    ::InflateRect(&gi, -inset, -inset);

    const COLORREF col = disabled ? ::GetSysColor(COLOR_GRAYTEXT)
                                  : RGB(0x3A, 0x3A, 0x3A);
    if (dis.CtlID == IDC_GEAR_BTN) DrawWrenchGlyph(dc, gi, col);
    else                           DrawTrashGlyph(dc, gi, col);

    if (dis.itemState & ODS_FOCUS) {
        RECT f = rc;
        ::InflateRect(&f, -Dp(2), -Dp(2));
        ::DrawFocusRect(dc, &f);
    }
}

static void CreateChildren(HWND parent)
{
    // ---- 第一行：四个测试按钮（等宽在 LayoutChildren 里统一给） ----
    for (int i = 0; i < kTestCount; ++i) {
        g_hTestBtn[i] = MakeChild(parent, L"BUTTON", kTests[i].label,
                                  BS_PUSHBUTTON | WS_TABSTOP,
                                  IDC_TEST_BASE + i);
    }

    // ---- 第二行：host 标签 + 可输入下拉框 + ⚙ + 清空 ----
    g_hHostLabel = MakeChild(parent, L"STATIC", L"Host", SS_LEFT | SS_CENTERIMAGE,
                             IDC_HOST_LABEL);
    // CBS_DROPDOWN（不是 DROPDOWNLIST）：历史只是"参考"，手输才是常规操作
    g_hHostCombo = MakeChild(parent, L"COMBOBOX", L"",
                             CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS
                             | WS_VSCROLL | WS_TABSTOP,
                             IDC_HOST_COMBO);
    // BS_OWNERDRAW：文字只作自检/读屏用的标识，真正画的是线条图标（WM_DRAWITEM）
    g_hGearBtn   = MakeChild(parent, L"BUTTON", L"⚙",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_GEAR_BTN);
    g_hClearBtn  = MakeChild(parent, L"BUTTON", L"清空",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_CLEAR_BTN);

    // ---- 输出区 ----
    // 只读 + 多行 + 允许软换行：面板常常只有三四百像素宽，
    // 硬不换行的话输出得横向拖着看，比折行难用得多。
    g_hOutput = MakeChild(parent, L"EDIT", L"",
                          WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY
                          | ES_AUTOVSCROLL | ES_LEFT,
                          IDC_OUTPUT_EDIT, true, WS_EX_CLIENTEDGE);

    ApplyFont();     // 控件都在了，才能挨个套字体（输出区用等宽那套）

    // 输出区子类化：只为 Ctrl+A（见 OutputEditProc 的说明）
    if (g_hOutput) {
        g_outOrigProc = (WNDPROC)(LONG_PTR)::SetWindowLongPtrW(
            g_hOutput, GWLP_WNDPROC, (LONG_PTR)OutputEditProc);
    }

    LoadConfigIntoUi();
    UpdateButtons();
}

// ---------------------------------------------------------------------------
// 单行布局要用的几个固定宽度
// （ClientSizeFor 与 LayoutChildren 共用 —— 两边各算一遍迟早会不一致）
// ---------------------------------------------------------------------------
static int IconButtonWidth() { return RowHeight(); }   // ⚙ / 清空：正方形
static int HostLabelWidth()
{
    return (std::max)(Dp(38), TextWidth(L"Host") + Dp(8));
}

// 右侧那一整块：[四个等宽按钮][⚙][清空]（含它们之间的空隙）
static int RightBlockWidth()
{
    const int gap = Dp(6);
    return 4 * TestButtonWidth() + 3 * gap + gap + IconButtonWidth()
           + gap + IconButtonWidth();
}

// 下拉框宽度下限：比这个还窄就不硬挤单行了（退回两行）
static int ComboMinWidth() { return Dp(110); }

// 设下拉框的条目高度。
// ⚠️ 必须在**改完几何之后**设：任何几何变更都会把它夹回字体默认值。
//    选择框（-1）用 rowH-2*Dp(2) → 闭合时与同行的按钮一样高，行不歪；
//    列表项（0）只按"一行字 + 一点内边距" → 展开时**紧凑**。
static void ApplyComboItemHeight(int rowH)
{
    if (!g_hHostCombo || !::IsWindow(g_hHostCombo)) return;
    const int selH = (std::max)(Dp(12), rowH - 2 * Dp(2));
    int listH = (std::max)(Dp(12), RowTextHeight() + Dp(4));
    if (listH > rowH) listH = rowH;
    ::SendMessageW(g_hHostCombo, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)selH);
    ::SendMessageW(g_hHostCombo, CB_SETITEMHEIGHT, (WPARAM)0,  (LPARAM)listH);
}

static void LayoutChildren(int W, int H)
{
    if (!g_hWnd) return;
    const int pad  = Dp(6);
    const int gap  = Dp(6);
    const int rowH = RowHeight();
    const int cbH  = rowH + Dp(120);   // ComboBox 的高度参数是"展开后"的高度

    const int iconW  = IconButtonWidth();
    const int btnW   = TestButtonWidth();
    const int labelW = HostLabelWidth();
    const int leftX  = pad + labelW + Dp(4);        // 下拉框左边缘
    int comboW = W - leftX - gap - RightBlockWidth() - pad;

    // 单行（王要的布局）：
    //   Host [下拉框 ▾] [Ping测试][路由追踪][端口扫描][DNS查询] [⚙] [🗑]
    // 放不下就退回两行（按钮一行、host 一行）—— 面板被拖得很窄时
    // 绝不能把控件挤出客户区。判据用「下拉框还能剩多少」而不是 W 本身，
    // 因为按钮/图标的宽度是随字号变的。
    const bool oneRow = (comboW >= ComboMinWidth());
    int outTop = 0;

    if (oneRow) {
        const int y = pad;
        int rx = W - pad;
        rx -= iconW;
        ::SetWindowPos(g_hClearBtn, nullptr, rx, y, iconW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        rx -= gap + iconW;
        ::SetWindowPos(g_hGearBtn, nullptr, rx, y, iconW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        rx -= gap;
        int bx = rx - (kTestCount * btnW + (kTestCount - 1) * gap);
        for (int i = 0; i < kTestCount; ++i) {
            if (g_hTestBtn[i] && ::IsWindow(g_hTestBtn[i])) {
                ::SetWindowPos(g_hTestBtn[i], nullptr, bx, y, btnW, rowH,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            bx += btnW + gap;
        }
        ::SetWindowPos(g_hHostLabel, nullptr, pad, y, labelW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        comboW = (std::max)(comboW, Dp(60));
        ::SetWindowPos(g_hHostCombo, nullptr, leftX, y, comboW, cbH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        outTop = pad + rowH + gap;
    } else {
        // ---- 第一行：4 个等宽按钮 ----
        const int avail = W - pad * 2 - gap * (kTestCount - 1);
        int w2 = avail / kTestCount;
        if (w2 > btnW) w2 = btnW;
        if (w2 < Dp(40)) w2 = Dp(40);
        int x = pad;
        for (int i = 0; i < kTestCount; ++i) {
            if (g_hTestBtn[i] && ::IsWindow(g_hTestBtn[i])) {
                ::SetWindowPos(g_hTestBtn[i], nullptr, x, pad, w2, rowH,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            x += w2 + gap;
        }

        // ---- 第二行：Host [下拉框]  ⚙  🗑（右侧两个从右往左排）----
        const int y2 = pad + rowH + gap;
        int r2 = W - pad;
        r2 -= iconW;
        ::SetWindowPos(g_hClearBtn, nullptr, r2, y2, iconW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        r2 -= gap + iconW;
        ::SetWindowPos(g_hGearBtn, nullptr, r2, y2, iconW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        r2 -= gap;
        ::SetWindowPos(g_hHostLabel, nullptr, pad, y2, labelW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        int cw = r2 - leftX;
        if (cw < Dp(60)) cw = Dp(60);
        ::SetWindowPos(g_hHostCombo, nullptr, leftX, y2, cw, cbH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        outTop = y2 + rowH + gap;
    }

    ApplyComboItemHeight(rowH);

    // ---- 输出区：剩下的全给它 ----
    if (g_hOutput) {
        int oh = H - outTop - pad;
        if (oh < Dp(40)) oh = Dp(40);
        ::SetWindowPos(g_hOutput, nullptr, pad, outTop, W - pad * 2, oh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void ClientSizeFor(int& w, int& h)
{
    const int pad = Dp(6), gap = Dp(6), rowH = RowHeight();
    // 最小宽度 = 单行布局所需：Host 标签 + 下拉框下限 + 右边整块
    const int need = pad + HostLabelWidth() + Dp(4) + ComboMinWidth() + gap
                     + RightBlockWidth() + pad;
    w = (std::max)(need, Dp(520));
    h = pad + rowH + gap + Dp(150) + pad;
}


static void AdjustRectForDpi(HWND h, RECT& r)
{
    ::AdjustWindowRectEx(&r, (DWORD)::GetWindowLongW(h, GWL_STYLE), FALSE,
                         (DWORD)::GetWindowLongW(h, GWL_EXSTYLE));
}

// ===========================================================================
// 窗口过程
// ===========================================================================
static void SyncDpi()
{
    const UINT d = DetectDpi(g_hWnd);
    if (d && d != g_dpi) { g_dpi = d; ApplyFont(); }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        // ⚠️ 必须在**这里**先填 g_hWnd 再用它当父窗口建控件：
        //    CreateWindowExW 是"返回之前"就把 WM_CREATE 发出来的，
        //    外面那句赋值此刻还没执行（不填就会把控件建到桌面上）。
        g_hWnd = hwnd;
        CreateChildren(hwnd);
        return 0;

    case WM_SIZE:
        SyncDpi();
        LayoutChildren(LOWORD(lp), HIWORD(lp));
        // 改尺寸不会自动重画（嵌入时更不会），必须自己作废整棵子树。
        // 只作废、**不** RDW_UPDATENOW（跨进程同步重画 = 死锁源，R6）。
        RepaintSubtree(hwnd);
        return 0;

    case WM_SHOWWINDOW:
        if (wp) PostSelfRepaint(hwnd);
        break;

    case WM_WINDOWPOSCHANGED:
        PostSelfRepaint(hwnd);
        break;

    case kMsgSelfRepaint:
        g_selfRepaintPosted = false;
        RepaintSubtree(hwnd);
        return 0;

    case WM_DPICHANGED: {
        g_dpi = LOWORD(wp) ? LOWORD(wp) : g_dpi;
        ApplyFont();
        if (!g_embedded) {
            const RECT* pr = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                           pr->right - pr->left, pr->bottom - pr->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT cr{};
        ::GetClientRect(hwnd, &cr);
        LayoutChildren(cr.right, cr.bottom);
        RepaintSubtree(hwnd);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        int cw = 0, ch = 0;
        ClientSizeFor(cw, ch);
        cw = (std::min)(cw, Dp(320));
        if (!g_embedded) {
            RECT r{ 0, 0, cw, ch };
            AdjustRectForDpi(hwnd, r);
            cw = r.right - r.left;
            ch = r.bottom - r.top;
        }
        mmi->ptMinTrackSize.x = cw;
        mmi->ptMinTrackSize.y = ch;
        return 0;
    }

    case WM_DRAWITEM: {
        // 两个图标按钮是自己画的（见 DrawIconButton）—— 系统不会替我们画，
        // 不处理这条消息按钮就是一片空白。
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (dis && (dis->CtlID == IDC_GEAR_BTN || dis->CtlID == IDC_CLEAR_BTN)) {
            DrawIconButton(*dis);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        const int id   = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id >= IDC_TEST_BASE && id < IDC_TEST_BASE + kTestCount) {
            if (code == BN_CLICKED) OnTestButton(id - IDC_TEST_BASE);
            return 0;
        }
        if (id == IDC_GEAR_BTN  && code == BN_CLICKED) { OnOpenConfig(); return 0; }
        if (id == IDC_CLEAR_BTN && code == BN_CLICKED) { OnClear();      return 0; }
        if (id == IDOK && code == BN_CLICKED) {          // host 框回车 = 跑第一条
            if (!g_busy) OnTestButton(0);
            return 0;
        }
        break;
    }

    case WM_SETFOCUS:
        if (g_hHostCombo) ::SetFocus(g_hHostCombo);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc   = (HDC)wp;
        HWND ctl = (HWND)lp;
        // ⚠️ 只读 EDIT 也走 WM_CTLCOLORSTATIC（不是 WM_CTLCOLOREDIT）。
        //    输出是"要看字"的地方 → 必须白底黑字；其余静态控件用按钮底色。
        if (ctl == g_hOutput) {
            ::SetBkColor(dc, RGB(255, 255, 255));
            ::SetTextColor(dc, RGB(0, 0, 0));
            return (LRESULT)::GetStockObject(WHITE_BRUSH);
        }
        ::SetBkMode(dc, TRANSPARENT);
        return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        netcfg::Save(g_cfg);       // 关掉这一刻的 host 记录也值得记
        if (g_hUiFont)  { ::DeleteObject(g_hUiFont);  g_hUiFont  = nullptr; }
        if (g_hOutFont) { ::DeleteObject(g_hOutFont); g_hOutFont = nullptr; }
        g_hWnd = nullptr;
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// 自检模式：--selftest <命令> [目标] [--参数 …]
// ---------------------------------------------------------------------------
// 引擎与界面解耦的额外好处：同一条链路能在控制台里跑，
// 于是"跟系统工具对账"（ping / arp -a / netstat -ano）不需要人去点界面。
//   NppDockApp_NET.exe --selftest ping 127.0.0.1 --count 4 --interval 200
//   NppDockApp_NET.exe --selftest portscan 127.0.0.1 --ports "22,80,443"
// ===========================================================================
namespace {

class ArgList {
public:
    ArgList() { _argv = ::CommandLineToArgvW(::GetCommandLineW(), &_argc); }
    ~ArgList() { if (_argv) ::LocalFree(_argv); }
    int size() const { return _argc; }
    const wchar_t* at(int i) const { return (i >= 0 && i < _argc) ? _argv[i] : L""; }
    const wchar_t* flag(const wchar_t* name) const
    {
        for (int i = 1; i + 1 < _argc; ++i)
            if (_wcsicmp(_argv[i], name) == 0) return _argv[i + 1];
        return nullptr;
    }
private:
    int     _argc = 0;
    LPWSTR* _argv = nullptr;
};

} // namespace

static bool ParseSelfTest(netcore::Kind& kindOut, netcore::Params& pOut)
{
    ArgList a;
    int selIdx = -1;
    for (int i = 1; i < a.size(); ++i) {
        if (_wcsicmp(a.at(i), L"--selftest") == 0) { selIdx = i + 1; break; }
    }
    if (selIdx < 0 || selIdx >= a.size()) return false;

    const wchar_t* key = a.at(selIdx);
    int found = -1;
    for (int i = 0; i < netcore::CommandCount(); ++i) {
        if (_wcsicmp(netcore::CommandKey(netcore::CommandAt(i)), key) == 0) {
            found = i; break;
        }
    }
    if (found < 0) return false;
    kindOut = netcore::CommandAt(found);

    pOut = netcore::Params{};
    for (int i = selIdx + 1; i < a.size(); ++i) {
        const wchar_t* s = a.at(i);
        if (s[0] == L'-' && s[1] == L'-') { ++i; continue; }   // 跳过开关和它的值
        if (pOut.target.empty()) pOut.target = s;
    }

    auto num = [&a](const wchar_t* f, int def) {
        const wchar_t* v = a.flag(f);
        return v ? (int)_wtoi(v) : def;
    };
    pOut.count        = num(L"--count",       pOut.count);
    pOut.intervalMs   = num(L"--interval",    pOut.intervalMs);
    pOut.timeoutMs    = num(L"--timeout",     pOut.timeoutMs);
    pOut.payload      = num(L"--payload",     pOut.payload);
    pOut.maxHops      = num(L"--hops",        pOut.maxHops);
    pOut.probesPerHop = num(L"--probes",      pOut.probesPerHop);
    pOut.port         = num(L"--port",        pOut.port);
    pOut.concurrency  = num(L"--concurrency", pOut.concurrency);
    pOut.view         = num(L"--view",        pOut.view);
    pOut.sortBy       = num(L"--sort",        pOut.sortBy);
    pOut.stateFilter  = num(L"--state",       pOut.stateFilter);

    if (const wchar_t* ps = a.flag(L"--ports")) {
        bool usedDefault = false;
        pOut.ports = netcfg::ParsePortSpec(WideToUtf8(ps), usedDefault);
    } else if (kindOut == netcore::Kind::PortScan) {
        // 端口扫描没给 --ports 就用内置的那 100 个（和界面里的默认一致）
        bool usedDefault = false;
        pOut.ports = netcfg::ParsePortSpec("", usedDefault);
    }

    if (const wchar_t* t = a.flag(L"--type")) {
        for (int i = 0; i < netcore::DnsTypeCount(); ++i) {
            if (_wcsicmp(netcore::DnsTypeName(netcore::DnsTypeValueAt(i)), t) == 0) {
                pOut.dnsType = netcore::DnsTypeValueAt(i);
                break;
            }
        }
    }
    // v2.0：DNS 的"指定服务器"也要能在自检里给 —— 否则这条链路（服务器生效
    //   与超时）在命令行上根本没法验证，只能靠手工点界面。
    if (const wchar_t* srv = a.flag(L"--server")) {
        unsigned int netIp = 0;
        if (netcore::ParseIpv4Net(srv, netIp)) pOut.dnsServer = (int)netIp;
    }
    if (const wchar_t* pr = a.flag(L"--proto"))
        pOut.proto = (_wcsicmp(pr, L"udp") == 0) ? 1 : 0;
    return true;
}

static HANDLE g_hConsole = nullptr;

static void ConsoleWrite(const std::wstring& w)
{
    const std::string s = WideToUtf8(w);
    if (s.empty() || !g_hConsole) return;
    DWORD wrote = 0;
    ::WriteFile(g_hConsole, s.data(), (DWORD)s.size(), &wrote, nullptr);
}

static void SinkEmitConsole(void* /*user*/, const std::wstring& line)
{
    ConsoleWrite(line + L"\r\n");
}

static int RunSelfTest(netcore::Kind kind, const netcore::Params& p)
{
    // ⚠️ 不要一上来就 AttachConsole：AttachConsole 成功时会把标准句柄**换掉**，
    //    调用方（构建脚本）重定向到管道的那份就丢了，脚本一个字都拿不到。
    //    先看现在的句柄能不能用，不能用再去挂控制台。
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = (hOut && hOut != INVALID_HANDLE_VALUE)
                     ? ::GetFileType(hOut) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_UNKNOWN) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
            type = (hOut && hOut != INVALID_HANDLE_VALUE)
                       ? ::GetFileType(hOut) : FILE_TYPE_UNKNOWN;
            ::SetConsoleOutputCP(CP_UTF8);
        }
    }
    if (type == FILE_TYPE_UNKNOWN) return 3;
    g_hConsole = hOut;

    netcore::GlobalInit();
    netcore::Sink sink;
    sink.emit = &SinkEmitConsole;
    sink.tick = nullptr;          // 自检不中止
    netcore::Run(kind, p, sink);
    netcore::GlobalShutdown();
    return 0;
}

// ===========================================================================
// 入口
// ===========================================================================
static HWND ParseDockParent()
{
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return nullptr;
    HWND h = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dock-parent") == 0) {
            const unsigned long long v = _wcstoui64(argv[i + 1], nullptr, 10);
            h = reinterpret_cast<HWND>(static_cast<UINT_PTR>(v));
            break;
        }
    }
    ::LocalFree(argv);
    return h;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR /*lpCmdLine*/, int)
{
    g_hInst = hInst;
    ::SetUnhandledExceptionFilter(CrashHandler);
    TraceLog("==== wWinMain enter ====");

    // ---- 自检模式：不开窗口 ----
    {
        netcore::Kind k{};
        netcore::Params p{};
        if (ParseSelfTest(k, p)) return RunSelfTest(k, p);
    }

    HWND hDockParent = ParseDockParent();
    g_embedded = (hDockParent != nullptr);
    AdoptHostDpiAwareness(hDockParent);           // R2：必须在建窗口之前
    g_dpi = DetectDpi(hDockParent);
    TraceLog(g_embedded ? "embedded mode" : "standalone mode");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return 1;

    const DWORD style   = g_embedded ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = g_embedded ? WS_EX_TOOLWINDOW : 0;

    g_hWnd = ::CreateWindowExW(exStyle, kClassName, L"网络测试", style,
                               64, 64, 760, 520, nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) { TraceLog("CreateWindow failed"); return 2; }
    TraceLog("window created");

    if (g_embedded) {
        // R3：改样式 -> SetParent -> SetWindowPos，顺序不能动
        LONG_PTR st = ::GetWindowLongPtrW(g_hWnd, GWL_STYLE);
        st &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        st |= WS_CHILD;
        ::SetWindowLongPtrW(g_hWnd, GWL_STYLE, st);
        ::SetParent(g_hWnd, hDockParent);

        RECT rc{};
        ::GetClientRect(hDockParent, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0) w = 400;
        if (h <= 0) h = 260;
        ::SetWindowPos(g_hWnd, nullptr, 0, 0, w, h,
                       SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(g_hWnd, SW_SHOW);
    } else {
        int cw = 0, ch = 0;
        ClientSizeFor(cw, ch);
        RECT r{ 0, 0, cw, ch };
        AdjustRectForDpi(g_hWnd, r);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        if (ww < 380) ww = 380;
        if (wh < 300) wh = 300;

        RECT wa{};
        if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            wa = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                        ::GetSystemMetrics(SM_CYSCREEN) };
        }
        const int x = wa.left + ((wa.right - wa.left) - ww) / 2;
        const int y = wa.top + ((wa.bottom - wa.top) - wh) / 3;
        ::SetWindowPos(g_hWnd, nullptr, x, y, ww, wh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(g_hWnd, SW_SHOW);
    }

    ::UpdateWindow(g_hWnd);
    ::SetWindowTextW(g_hWnd, g_embedded ? L"网络测试（嵌入）" : L"网络测试");

    {
        RECT cr{};
        ::GetClientRect(g_hWnd, &cr);
        LayoutChildren(cr.right, cr.bottom);
    }

    netcore::GlobalInit();     // WSAStartup：ping/tracert/dns/portscan 都依赖它
    TraceLog("enter message loop");

    // 首次运行或读配置出错时，把话说在输出区里（没有状态行了）
    if (g_cfgCreated) {
        Say(L"已生成配置文件（点 ⚙ 打开它改参数）：");
        Say(L"  " + netcfg::ConfigPath());
    } else if (!g_cfgErr.empty()) {
        Say(g_cfgErr);
    }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    netcore::GlobalShutdown();
    return 0;
}
