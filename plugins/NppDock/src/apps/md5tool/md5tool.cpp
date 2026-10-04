// ============================================================================
// md5tool.cpp —— NppDock 应用示例：文件校验（MD5 / SHA-1 / SHA-256 / SHA-384 /
//                SHA-512 / CRC32）
// ----------------------------------------------------------------------------
// 这是一个**普通的 Win32 程序**，同时支持两种运行形态：
//
//   1) 独立打开（双击 exe）
//        无参启动 -> 建一个标准顶层窗口（有标题栏 / 可最大化 / 出现在任务栏）。
//
//   2) 被 NppDock 嵌入（dock 拉起时传参）
//        md5tool.exe --dock-parent <HWND>
//        -> 把窗口改成 WS_CHILD，SetParent 到宿主给的那个窗口，铺满它的客户区。
//
// 【为什么由 exe 自己完成子窗口化，而不是让 dock 去改它的窗口样式】
//   dock 侧去 SetParent + 改样式当然也能做，但那样 plug-in 就必须去猜
//   "这个 exe 的窗口长什么样、该保留哪些样式"。而 exe 自己最清楚：
//   它知道嵌进去以后不该有标题栏、边距该收多少、按钮该排在哪。
//   所以约定就是 —— **dock 只传一个父窗口句柄，剩下的 exe 自己搞定**。
//
// 【UI 布局必须同时考虑两种形态】
//   实测下来的可用宽度差异很大：独立打开时 640px 起步，嵌进 dock 时可能只有
//   300 多 px 宽、180px 高。所以布局一律**按父窗口客户区动态计算**（见 OnSize），
//   绝不写死坐标，也不假设"窗口一定很宽/一定够高"。
//
// 【本文件只管界面，算法在 hashcore.cpp】
//   六种摘要算法（含常量表）都在 hashcore.h/.cpp 里，这里是纯粹的 UI + 文件读取。
//   这样分层的好处：想再加一种算法（比如 SM3）只需要动 hashcore，界面不用碰。
//
// 【两个小设计】
//   · 状态行字号比正文**小一号**（比例 8/9，见 MakeFontImpl）——
//     它是附加信息，不是操作区，小一档能让三行控件在视觉上更突出。
//   · 「计算」按钮在计算过程中**变成「终止」**：大文件可能算很久，
//     必须留一个能停下来的入口；而面板被拖窄时也腾不出第四个按钮的位置。
//
// 构建：见 plugins/NppDock/build.py（/MT 静态 CRT，产物无第三方依赖）
// ============================================================================

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>   // DragAcceptFiles / DragQueryFile（拖入文件）

#include <algorithm>
#include <string>
#include <vector>
#include <stdio.h>

#include "hashcore.h"

// ---------------------------------------------------------------------------
// 控件 ID
// ---------------------------------------------------------------------------
// ⚠️ 100~107 这几个值与 tools/dock_app_probe.py 里写死的是一致的
//    （探针用 GetDlgItem(h, 101/104/105/107…) 直接读控件）。**别改这些编号**，
//    要加新控件往后排（108 起）。
enum {
    IDC_PATH_LABEL = 100,
    IDC_PATH_EDIT,        // 101  文件路径（可键盘输入）
    IDC_BROWSE_BTN,       // 102  浏览…
    IDC_HASH_LABEL,       // 103  校验值
    IDC_HASH_EDIT,        // 104  结果（只读）
    IDC_CALC_BTN,         // 105  计算
    IDC_COPY_BTN,         // 106  复制
    IDC_STATUS_TEXT,      // 107  状态行
    IDC_ALGO_LABEL,       // 108  校验方式
    IDC_ALGO_COMBO,       // 109  算法下拉框
};

// ---------------------------------------------------------------------------
// 全局（单窗口程序，用不着更复杂的结构）
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst        = nullptr;
static HWND      g_hWnd         = nullptr;
static HWND      g_hSrcEdit     = nullptr;   // 文件路径
static HWND      g_hDstEdit     = nullptr;   // 校验结果
static HWND      g_hAlgoCombo   = nullptr;   // 算法下拉框
static HWND      g_hStatus      = nullptr;
static HFONT     g_hFont        = nullptr;   // 正文（标签/编辑框/按钮）
static HFONT     g_hStatusFont  = nullptr;   // 状态行专用：比正文小一号
static UINT      g_dpi          = 96;        // 当前窗口所在显示器的 DPI

static bool      g_embedded     = false;     // 是否运行在 dock 里
static bool      g_busy         = false;     // 正在计算（同时决定「计算」按钮此刻是「终止」）

// 用户点了「终止」。只在**本线程**读写：
//   计算跑在 UI 线程上（靠 PumpMessages 处理点击，见 ComputeFileHash 的说明），
//   所以这里不需要原子量，普通 bool 就是安全的。
//   —— 注意别把它改成"开线程 + 加锁"再顺手把这里换成普通 bool 的读写，
//      真要开线程时必须换成 std::atomic。
static bool      g_cancelRequested = false;

static std::wstring    g_filePath;           // 当前要校验的文件（规整过的）
static std::string     g_resultHex;          // 当前结果（小写十六进制）
static hashcore::Algo  g_algo = hashcore::Algo::Md5;

// 程序自己在改路径框文本时置起，用来区分"人在敲字"和"我们在回写"。
// 不做这个区分的话，ReadPathFromEdit() 的回写会把刚算好的结果当成"路径被改了"清掉。
static bool      g_syncingPath  = false;

// 路径框原来的窗口过程（子类化用）
static WNDPROC   g_pathEditProc = nullptr;

// 前向声明：CreateChildren 建完控件就要把路径框子类化，
// 而子类过程定义在文件后半部分。
static LRESULT CALLBACK PathEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void OnPathCommitted();
static void AlignComboItems(int rowH, int textH);

// ---------------------------------------------------------------------------
// "把自己整棵子树重画一遍" —— 排到本线程消息队列的末尾再执行
// ---------------------------------------------------------------------------
// 私有消息，只在本进程内自己发给自己，不涉及任何跨进程协议。
const UINT kMsgSelfRepaint = WM_APP + 0x51;

// 队列里是否已经排了一条（拖动时每一帧都会走到，不拦会堆一串做同一件事的）
static bool g_selfRepaintPosted = false;

// ⚠️ 必须是 **PostMessage**，不能直接调 RedrawWindow。
//   因为触发时机往往在"窗口还没真正显示出来"的时候（WM_SHOWWINDOW /
//   WM_WINDOWPOSCHANGED 都是在显示动作**中间**发出来的）：这时候去作废，
//   更新区会被丢掉，等真正显示出来时反而没人重画 —— 实测就是这个结果，
//   症状是"隐藏再显示之后，面板里只剩一块底色"。
//   排到队列末尾，等这一轮显示/移动彻底结束、窗口真的可见了，再作废才有效。
//   （这是 dock 侧同一个坑的另一面，见 docs/铁律与踩坑要点.md「窗口与重绘」。）
static void PostSelfRepaint(HWND hwnd)
{
    if (g_selfRepaintPosted) return;
    g_selfRepaintPosted = true;
    ::PostMessageW(hwnd, kMsgSelfRepaint, 0, 0);
}

// 立即作废整棵子树：只作废自己是不够的，子控件不在更新区里就不会重画。
// 只作废、**不要** RDW_UPDATENOW —— 本窗口跨进程嵌在 dock 里，
// 同步重画会让父进程等我们画完，一旦绘制路径回头找父窗口就是死锁。
static void RepaintSubtree(HWND hwnd)
{
    ::RedrawWindow(hwnd, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// 布局尺寸
// ---------------------------------------------------------------------------
// ⚠️ 这里写的全部是 **96 DPI 下的"设计像素"**，真正用到时一律经 Dp() 换算。
//
// 真机踩到的坑（王的机器正是 150% 缩放）：
//   布局曾经是一组写死的像素值（行高 24、按钮宽 76、标签宽 42）。
//   而本程序是 DPI-aware 的，字体会随缩放变大 —— 150% 下系统消息字体
//   约 19px 高，塞进 24px 的行里上下都露不出来；"浏览…"三个字也宽过 76px。
//   症状就是"排布很紧凑、文字显示不全"。
//   结论：**只要涉及字号，就不能有写死的像素**。行高由实测文字高度推出来，
//   按钮宽度由实测文字宽度 + 内边距推出来，见 ComputeMetrics()。
// ---------------------------------------------------------------------------
static const int kBaseMargin = 12;
static const int kBaseRowH   = 24;    // 仅作下限，实际由字高决定
static const int kBaseRowGap = 9;
static const int kBaseEditW  = 340;   // 独立打开时编辑框的理想宽度
static const int kBasePad    = 10;    // 按钮内边距（左右各 5）
static const int kBaseComboW = 110;   // 下拉框理想宽度（要放得下 "SHA-256" + 箭头）

static const wchar_t* kClassName = L"NppDockMd5ToolWnd";

// ===========================================================================
// 小工具
// ===========================================================================
// 最近一次写进状态行的话。没有窗口时（--selftest）它是唯一的出口 ——
// 以前失败只返回一个退出码 2，排障的人看不到"为什么失败"。
static std::wstring g_lastStatus;
static void SetStatus(const std::wstring& text)
{
    g_lastStatus = text;
    if (g_hStatus) ::SetWindowTextW(g_hStatus, text.c_str());
}

static std::wstring WindowTextOf(HWND h)
{
    if (!h) return L"";
    int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s((size_t)n + 1, L'\0');
    int got = ::GetWindowTextW(h, &s[0], n + 1);
    s.resize(got > 0 ? (size_t)got : 0);
    return s;
}

// 让窗口标题句柄可被外部（含测试）观察：嵌入模式下把标题改成带前缀的形式，
// 便于日志/枚举确认"这一页确实被嵌进来了"。
static void UpdateWindowTitle()
{
    if (!g_hWnd) return;
    std::wstring t = g_embedded ? L"文件校验（嵌入）" : L"文件校验";
    ::SetWindowTextW(g_hWnd, t.c_str());
}

static void PumpMessages()
{
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { ::PostQuitMessage(0); return; }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

static std::wstring FormatBytes(unsigned long long n)
{
    wchar_t b[64];
    if (n >= 1024ull * 1024 * 1024)
        swprintf_s(b, L"%.2f GB", (double)n / (1024.0 * 1024 * 1024));
    else if (n >= 1024ull * 1024)
        swprintf_s(b, L"%.2f MB", (double)n / (1024.0 * 1024));
    else if (n >= 1024ull)
        swprintf_s(b, L"%.1f KB", (double)n / 1024.0);
    else
        swprintf_s(b, L"%llu B", n);
    return b;
}

// ===========================================================================
// 计算
// ===========================================================================
// 同步计算 + 手工泵消息。
//
// 【为什么不开线程】这是个小工具，开线程就要连带处理取消、同步、退出时序、
//   跨线程更新 UI；而"界面不假死"其实只需要在循环里泵一下消息就够。
//
// 【为什么这样就够】在 UI 线程里同步跑，点击事件会在 PumpMessages() 里被派发，
//   「终止」把 g_cancelRequested 置起，下一轮读块时立刻退出 —— 零线程、
//   零锁、零竞态，取消还天然是"立即"的（最坏等一个 64KB 块的读取时间）。
//   代价：算的过程里 UI 只能泵消息、不能"并行做别的事"，对本工具完全够用。
//
// 算法本身在 hashcore 里，这里只负责：开文件 -> 分块读 -> 喂给摘要器 -> 报进度。
enum class ComputeResult { Ok, Failed, Cancelled };

// 目录判断：拖入 / 键入 / 浏览三条入口共用这一份判据，文案才不会各说各话。
static bool IsDirW(const std::wstring& p)
{
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 速率的滑动窗口（最近约 1 秒）。做法与「文件背包」一致：留一串带时间戳的
// 采样点，用**窗口两端**的差来算 —— 只用相邻两点会把误差放大十倍。
struct SpeedWin
{
    struct S { DWORD t; unsigned long long n; };
    S   s[12]{};
    int cnt = 0;

    void add(DWORD t, unsigned long long n)
    {
        if (cnt < 12) { s[cnt++] = S{ t, n }; return; }
        for (int i = 1; i < 12; ++i) s[i - 1] = s[i];
        s[11] = S{ t, n };
    }
    double bps() const
    {
        if (cnt < 2) return 0.0;
        const double dt = (double)(s[cnt - 1].t - s[0].t) / 1000.0;
        if (dt < 0.15) return 0.0;              // 窗口太窄，算出来不可信
        return (double)(s[cnt - 1].n - s[0].n) / dt;
    }
};

static ComputeResult ComputeFileHash(const std::wstring& path, hashcore::Algo algo,
                                     std::string& hexOut)
{
    // 先判"这是不是个文件夹"。不判的话 CreateFileW 打不开目录，会失败在
    // ERROR_ACCESS_DENIED(5)，下面把它译成"没有权限（试试用管理员身份打开）"
    // —— 对一个只是想校验文件的人来说，这句话完全是误导。
    if (IsDirW(path)) {
        SetStatus(L"这是一个文件夹，本工具只校验单个文件");
        return ComputeResult::Failed;
    }

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 手输路径之后，最常见的失败就是"路径打错了"或"粘了个目录"。
        // 光甩一个错误码很难用，这里把几个高频情况翻成人话。
        DWORD e = ::GetLastError();
        std::wstring msg;
        switch (e) {
        case ERROR_FILE_NOT_FOUND:                 // 2
        case ERROR_PATH_NOT_FOUND:                 // 3
            msg = L"找不到这个文件，请检查路径";
            break;
        case ERROR_ACCESS_DENIED:                  // 5
            msg = L"没有权限读取这个文件（试试用管理员身份打开）";
            break;
        case ERROR_SHARING_VIOLATION:              // 32
            msg = L"文件被其他程序独占打开，无法读取";
            break;
        default: {
            wchar_t b[160];
            swprintf_s(b, L"打开文件失败（错误码 %lu）", e);
            msg = b;
            break;
        }
        }
        SetStatus(msg);
        return ComputeResult::Failed;
    }

    LARGE_INTEGER total{};
    // 取不到大小不算致命（管道/特殊文件就是这样）：当成"总量未知"，
    // 界面上就不显示百分比，只报已处理量。
    if (!::GetFileSizeEx(h, &total)) total.QuadPart = 0;

    hashcore::Hasher hasher(algo);
    const wchar_t* algoName = hashcore::AlgoName(algo);

    // 64KB 一块。太小则泵消息过于频繁，太大则状态刷新迟钝、终止响应也变钝
    //（终止的响应延迟上限就是"读完一块"的时间，所以这个值不能调太大）。
    const DWORD kChunk = 64 * 1024;
    std::vector<unsigned char> buf(kChunk);

    unsigned long long done = 0;
    DWORD lastTick = ::GetTickCount();
    SpeedWin speed;                            // 最近约 1 秒的吞吐

    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf.data(), kChunk, &got, nullptr)) {
            ::CloseHandle(h);
            SetStatus(L"读取文件失败");
            return ComputeResult::Failed;
        }
        if (got == 0) break;   // EOF

        hasher.Update(buf.data(), got);
        done += got;

        // 每块都泵一次消息：界面上不会出现"未响应"，
        // 且用户在计算途中点的「终止」就是在这一步被派发处理的。
        PumpMessages();
        // 自检模式没有窗口（g_hWnd 为空），此时跳过窗口存活检查
        if (g_hWnd && !::IsWindow(g_hWnd)) { ::CloseHandle(h); return ComputeResult::Failed; }

        // ⚠️ 必须在 PumpMessages 之后判：顺序反过来的话，用户刚点下的那次点击
        //    要等下一块才生效，大文件上"点了没反应"的感觉很明显。
        if (g_cancelRequested) {
            ::CloseHandle(h);
            SetStatus(L"已终止计算（未得出结果）");
            return ComputeResult::Cancelled;
        }

        DWORD now = ::GetTickCount();
        if (now - lastTick >= 100) {          // 最多每 100ms 刷一次文本
            lastTick = now;
            speed.add(now, done);

            // 速度放前面、总量不放 —— 状态行是 SS_ENDELLIPSIS，
            // 窄面板下从右往左被吃掉，最有用的必须先出现。
            const double bps = speed.bps();
            std::wstring sp;
            if (bps > 1.0) sp = FormatBytes((unsigned long long)bps) + L"/s";

            wchar_t b[256];
            if (total.QuadPart > 0) {
                const int pct = (int)((done * 100) / (unsigned long long)total.QuadPart);
                if (!sp.empty())
                    swprintf_s(b, L"正在计算 %s… %d%% · %s（点「终止」可停下）",
                               algoName, pct, sp.c_str());
                else
                    swprintf_s(b, L"正在计算 %s… %d%%（点「终止」可停下）", algoName, pct);
            } else {
                if (!sp.empty())
                    swprintf_s(b, L"正在计算 %s… %s · %s（点「终止」可停下）",
                               algoName, FormatBytes(done).c_str(), sp.c_str());
                else
                    swprintf_s(b, L"正在计算 %s… %s（点「终止」可停下）",
                               algoName, FormatBytes(done).c_str());
            }
            SetStatus(b);
        }
    }

    ::CloseHandle(h);

    hexOut = hasher.HexDigest();

    wchar_t doneMsg[224];
    swprintf_s(doneMsg, L"%s 校验完成，共处理 %s", algoName, FormatBytes(done).c_str());
    SetStatus(doneMsg);
    return ComputeResult::Ok;
}

// ===========================================================================
// DPI
// ---------------------------------------------------------------------------
// 本程序是 per-monitor-v2 感知的（见 AdoptHostDpiAwareness），
// 所以"1 像素"在不同显示器上代表的物理长度并不一样。
// 凡是尺寸、间距、字号一律经 Dp() 换算 —— 这是"不显得紧凑、文字不被裁"的根。
//
// 为什么动态取函数指针、不直接调用：
//   GetDpiForWindow / SystemParametersInfoForDpi 都是 Win10 1607+ 才有，
//   动态取能让同一个二进制在老系统上照样跑（自动退回老 API），
//   也不需要改 WIN32_WINNT 或加 manifest。
// ===========================================================================
typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
typedef BOOL (WINAPI* PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);
typedef BOOL (WINAPI* PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);

static UINT DetectDpi(HWND h)
{
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (u) {
        auto getDpi = (PFN_GetDpiForWindow)::GetProcAddress(u, "GetDpiForWindow");
        if (getDpi && h) {
            UINT d = getDpi(h);
            if (d) return d;
        }
    }
    HDC dc = ::GetDC(nullptr);
    UINT d = dc ? (UINT)::GetDeviceCaps(dc, LOGPIXELSY) : 0;
    if (dc) ::ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

// 96 DPI 下的"设计像素" -> 当前 DPI 的实际像素
static inline int Dp(int px) { return ::MulDiv(px, (int)g_dpi, 96); }

// 按当前 DPI 取系统消息字体。smaller=true 时"小一号"（状态行用）。
// 不写死 "Microsoft YaHei 9pt"：那样在别人机器上可能没这个字体，也不跟着缩放走。
//
// 【"小一号"怎么算】用**比例**而不是减固定像素：
//   系统消息字体在别人机器上可能是 9pt / 10pt / 11pt（甚至自己改过），
//   减固定 2px 在小字号机器上会小到看不清、在大字号机器上几乎看不出变化。
//   8/9 正好是"9pt -> 8pt"这个档，换算到任何基础字号都成立。
//   lfHeight 是**负数**（表示按字符高度算），所以先取绝对值再缩放、最后取回负号。
static const int kStatusFontNum = 8;      // 分子
static const int kStatusFontDen = 9;      // 分母
static const int kMinFontPx    = 9;       // 下限：再小就糊了

static HFONT MakeMonoFontAt(int lfHeight)
{
    static const wchar_t* kMono[] = { L"Consolas", L"Lucida Console", L"Courier New" };
    for (int i = 0; i < _countof(kMono); ++i) {
        LOGFONTW lf{};
        lf.lfHeight         = lfHeight;
        lf.lfWeight         = FW_NORMAL;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfOutPrecision   = OUT_TT_PRECIS;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcsncpy_s(lf.lfFaceName, kMono[i], _TRUNCATE);

        HFONT f = ::CreateFontIndirectW(&lf);
        if (!f) continue;
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
    return nullptr;         // 三种都拿不到 → 调用方退回系统消息字体
}

static HFONT MakeFontImpl(bool smaller)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    bool got = false;
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (u) {
        auto forDpi = (PFN_SystemParametersInfoForDpi)
            ::GetProcAddress(u, "SystemParametersInfoForDpi");
        if (forDpi) {
            got = forDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi) != FALSE;
        }
    }
    if (!got) {
        if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return nullptr;
        // 老系统：拿到的是系统 DPI 下的字体，按比例补正到当前 DPI
        HDC dc = ::GetDC(nullptr);
        UINT sysDpi = dc ? (UINT)::GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ::ReleaseDC(nullptr, dc);
        if (sysDpi && sysDpi != g_dpi && ncm.lfMessageFont.lfHeight) {
            ncm.lfMessageFont.lfHeight =
                ::MulDiv(ncm.lfMessageFont.lfHeight, (int)g_dpi, (int)sysDpi);
        }
    }

    if (smaller && ncm.lfMessageFont.lfHeight) {
        LONG hgt = ncm.lfMessageFont.lfHeight;
        const LONG sign = (hgt < 0) ? -1 : 1;
        LONG abs_ = (hgt < 0) ? -hgt : hgt;
        LONG shrunk = (abs_ * kStatusFontNum + kStatusFontDen / 2) / kStatusFontDen;
        if (shrunk < kMinFontPx) shrunk = kMinFontPx;
        ncm.lfMessageFont.lfHeight = sign * shrunk;
    }
    // ★ v1.8：拿到"当前 DPI 下的正确字号"之后，把**字型**换成等宽。
    //   字号那套坑照旧走上面的系统消息字体（别自己乘 dpi/96）。
    int lfH = ncm.lfMessageFont.lfHeight;
    if (lfH == 0) lfH = -::MulDiv(12, (int)g_dpi, 96);
    if (HFONT mono = MakeMonoFontAt(lfH)) return mono;
    return ::CreateFontIndirectW(&ncm.lfMessageFont);
}

// v1.8：**所有字体统一成等宽（Consolas）** —— 王的原话是"总之就是所有字体都改成 console"。
// 为什么带"回读核对"：CreateFontIndirect 在字型不存在时**不报错**，
// 会静默换成别的（通常还是比例字体）—— 那样等于"看着设上了、其实没设上"。
static HFONT MakeUiFont()    { return MakeFontImpl(false); }
static HFONT MakeSmallFont() { return MakeFontImpl(true);  }

// 换字体并广播给所有子控件。
// 顺序：先把新字体装到所有子控件上，**再**删旧字体 ——
// 反过来会让子控件短暂引用一个已释放的 GDI 对象。
//
// 状态行（IDC_STATUS_TEXT）单独用**小一号**的字体：
//   它是"附加信息"而不是操作区，字号小一档能让三行控件在视觉上更突出，
//   也让本来就偏长的一行进度文本更容易塞进窄面板。
static void ApplyFont()
{
    HFONT f = MakeUiFont();
    if (!f) return;
    HFONT sf = MakeSmallFont();            // 失败就退回正文字体，不影响可用性
    if (!sf) sf = f;

    HFONT oldMain = g_hFont;
    HFONT oldStat = g_hStatusFont;
    g_hFont       = f;
    g_hStatusFont = sf;

    for (HWND h = ::GetWindow(g_hWnd, GW_CHILD); h; h = ::GetWindow(h, GW_HWNDNEXT)) {
        const bool isStatus = (::GetDlgItem(g_hWnd, IDC_STATUS_TEXT) == h);
        ::SendMessageW(h, WM_SETFONT,
                       (WPARAM)(isStatus ? g_hStatusFont : g_hFont), TRUE);
    }
    if (oldMain) ::DeleteObject(oldMain);
    if (oldStat && oldStat != oldMain) ::DeleteObject(oldStat);
}

static void AdjustRectForDpi(HWND h, RECT& r)
{
    LONG_PTR st = ::GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR ex = ::GetWindowLongPtrW(h, GWL_EXSTYLE);
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (u) {
        auto fn = (PFN_AdjustWindowRectExForDpi)
            ::GetProcAddress(u, "AdjustWindowRectExForDpi");
        if (fn && fn(&r, (DWORD)st, FALSE, (DWORD)ex, g_dpi)) return;
    }
    ::AdjustWindowRectEx(&r, (DWORD)st, FALSE, (DWORD)ex);
}

// ===========================================================================
// 度量
// ---------------------------------------------------------------------------
// 行高、列宽全部**实测**，不猜：
//   行高   >= 字体实际高度 + 上下留白   -> 文字永远不会被裁
//   按钮宽 >= 最宽那个按钮文字 + 内边距 -> "浏览…" 永远不会被裁
//   标签列宽 >= 最宽那个标签文字 + 间距 -> "校验方式" 不会顶到编辑框
// 这样换字体、换语言、换缩放都不会出问题。
// ===========================================================================
struct Metrics {
    int margin;   // 外边距
    int rowH;     // 单行控件高度
    int rowGap;   // 行间距
    int labelW;   // 左侧标签列宽
    int btnW;     // 按钮宽
    int comboW;   // 下拉框宽
    int textH;    // 字体实际高度（行高下限用）
};

static int TextWidth(HDC dc, const wchar_t* s)
{
    if (!g_hFont) return 0;
    HGDIOBJ old = ::SelectObject(dc, g_hFont);
    SIZE sz{};
    ::GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    if (old) ::SelectObject(dc, old);
    return sz.cx;
}

static Metrics ComputeMetrics()
{
    Metrics m;
    m.margin = Dp(kBaseMargin);
    m.rowH   = Dp(kBaseRowH);
    m.rowGap = Dp(kBaseRowGap);
    m.labelW = Dp(40);
    m.btnW   = Dp(80);
    m.comboW = Dp(kBaseComboW);
    m.textH  = Dp(16);

    if (!g_hWnd) return m;
    HDC dc = ::GetDC(g_hWnd);
    if (!dc) return m;

    if (g_hFont) {
        HGDIOBJ old = ::SelectObject(dc, g_hFont);
        TEXTMETRICW tm{};
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, old);

        // 「终止」也列进来：它和「计算」是**同一个按钮**（算的过程中变身），
        // 宽度必须同时满足两个标签，否则变身那一刻按钮里的字会被裁。
        static const wchar_t* kBtnTexts[] = { L"浏览…", L"计算", L"终止", L"复制" };
        int btnTextW = 0;
        for (int i = 0; i < _countof(kBtnTexts); ++i)
            btnTextW = (std::max)(btnTextW, TextWidth(dc, kBtnTexts[i]));

        // 三个标签里"校验方式"最宽 —— 标签列宽必须容得下最宽的那个，
        // 否则它会顶到右边的编辑框上（视觉上就是"文字糊在输入框边上"）。
        static const wchar_t* kLabelTexts[] = { L"文件路径", L"校验结果", L"校验方式" };
        int labelTextW = 0;
        for (int i = 0; i < _countof(kLabelTexts); ++i)
            labelTextW = (std::max)(labelTextW, TextWidth(dc, kLabelTexts[i]));

        // 注意 tm.tmHeight 是 LONG，Dp() 返回 int —— std::max 要求两个实参
        // 类型完全一致，混用会编译不过（C2672），所以这里显式转 int。
        const int textH = (int)tm.tmHeight;
        m.textH  = textH;
        m.rowH   = (std::max)(Dp(kBaseRowH), textH + Dp(10));
        m.btnW   = (std::max)(Dp(72), btnTextW + Dp(kBasePad));
        m.labelW = (std::max)(Dp(32), labelTextW + Dp(8));

        // 下拉框：最宽的条目（SHA-256）+ 下拉箭头和左右内边距。
        // 那个 44 是"箭头 + 两侧留白"的经验值，不是精确算法 ——
        // 少了它 "SHA-256" 会被箭头压住。
        m.comboW = (std::max)(Dp(kBaseComboW), TextWidth(dc, L"SHA-256") + Dp(44));
    }
    ::ReleaseDC(g_hWnd, dc);
    return m;
}

// 给定编辑框理想宽度，算出所需的客户区尺寸（独立打开 / 最小尺寸都用它）
static void ClientSizeFor(int editW, int& w, int& h)
{
    const Metrics m = ComputeMetrics();
    // 上面两行：标签 + 等长的编辑框
    const int rowAB = m.labelW + editW;
    // 底行：标签 + 下拉框 + 右侧三个按钮
    const int rowC  = m.labelW + m.comboW + m.rowGap * 2 + m.btnW * 3;
    w = m.margin * 2 + (std::max)(rowAB, rowC);
    // 三行控件 + 一行状态
    h = m.margin * 2 + m.rowH * 3 + m.rowGap * 3 + Dp(34);
}

// ===========================================================================
// 布局
// ---------------------------------------------------------------------------
// 版式（改动的重点，三条约束是王提的）：
//
//     文件     [______________________________]   ← 与下一行**等长**
//     校验值   [______________________________]   ← 与上一行**等长**
//     校验方式 [SHA-256 ▾]        [浏览…][计算][复制]
//     校验完成，共处理 3.00 MB          ↑ 算的过程中这个按钮变成「终止」
//
//   1) 两个编辑框**同一个起点、同一个宽度** —— 所以左边第一行不能挂"浏览"按钮，
//      第二行也不能挂"计算/复制"：只要某一侧有按钮，那一行的框就短一截。
//      这就是"按钮全部下移"的**根本原因**（不是为了好看，是为了等长能成立）。
//   2) 下拉框与两个编辑框左边缘对齐（同一列起步），宽度另算。
//   3) 按钮横向只有**三个**位置：既然「终止」要用"变身"的方式复用「计算」的坑，
//      按钮数量就不会因为加功能而增长 —— 这正是当初选变身而不是加按钮的原因。
//   4) 高度方向自适应：面板可能被拖得很矮（真机见过 189px 高），
//      而这里现在有 3 行控件 + 1 行状态。顺序是：先压行距 -> 再压行高（有下限）
//      -> 实在不够就压状态行。**控制行优先**，它们才是能操作的东西。
// ===========================================================================
static void LayoutChildren(int W, int H)
{
    if (!g_hWnd) return;
    const Metrics m = ComputeMetrics();

    const int left  = m.margin;
    const int right = W - m.margin;
    const int editX = left + m.labelW;          // 三行控件的公共左边缘

    // 两个编辑框共用这一个宽度 —— 这就是"等长"的实现。
    int editW = right - editX;
    if (editW < Dp(70)) editW = Dp(70);

    // ---- 垂直自适应 ----
    const int minStatus = Dp(14);
    // 行高下限：字高 + 一点余量。低于这个值文字上下就会被裁。
    const int rowFloor  = (std::max)(Dp(18), m.textH + Dp(2));
    int rowH   = m.rowH;
    int rowGap = m.rowGap;
    for (int guard = 0; guard < 256; ++guard) {
        if (m.margin * 2 + rowH * 3 + rowGap * 3 + minStatus <= H) break;
        if (rowGap > 2)      { --rowGap; continue; }
        if (rowH > rowFloor) { --rowH;   continue; }
        break;
    }

    // ---- 第 1 行：文件（标签 + 路径框，可键盘输入）----
    int y = m.margin;
    ::SetWindowPos(::GetDlgItem(g_hWnd, IDC_PATH_LABEL), nullptr,
                   left, y, m.labelW - Dp(6), rowH,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(g_hSrcEdit, nullptr, editX, y, editW, rowH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    // ---- 第 2 行：校验值（标签 + 结果框，只读；宽度与上一行完全相同）----
    y += rowH + rowGap;
    ::SetWindowPos(::GetDlgItem(g_hWnd, IDC_HASH_LABEL), nullptr,
                   left, y, m.labelW - Dp(6), rowH,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(g_hDstEdit, nullptr, editX, y, editW, rowH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    // ---- 第 3 行：校验方式（下拉框）+ 操作按钮（全部靠右下对齐）----
    y += rowH + rowGap;
    ::SetWindowPos(::GetDlgItem(g_hWnd, IDC_ALGO_LABEL), nullptr,
                   left, y, m.labelW - Dp(6), rowH,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    {
        // 按钮从右往左排：复制 | 计算 | 浏览…
        const int copyX = right - m.btnW;
        const int calcX = copyX - m.rowGap - m.btnW;
        const int browX = calcX - m.rowGap - m.btnW;

        int comboW = m.comboW;
        const int comboMax = browX - m.rowGap - editX;   // 别压到按钮上
        if (comboW > comboMax) comboW = comboMax;
        if (comboW < Dp(56))   comboW = Dp(56);          // 再窄也得看得见文字

        ::SetWindowPos(::GetDlgItem(g_hWnd, IDC_BROWSE_BTN), nullptr,
                       browX, y, m.btnW, rowH, SWP_NOZORDER | SWP_NOACTIVATE);
        ::SetWindowPos(::GetDlgItem(g_hWnd, IDC_CALC_BTN), nullptr,
                       calcX, y, m.btnW, rowH, SWP_NOZORDER | SWP_NOACTIVATE);
        ::SetWindowPos(::GetDlgItem(g_hWnd, IDC_COPY_BTN), nullptr,
                       copyX, y, m.btnW, rowH, SWP_NOZORDER | SWP_NOACTIVATE);

        if (g_hAlgoCombo) {
            // ⚠️ 下拉框的"窗口高度"指的是**落下时**的总高度，不是闭合时的高度。
            //    所以这里必须给 rowH + 全部条目的高度，否则下拉列表会被截成一行。
            const int comboH = rowH + Dp(4) + rowH * hashcore::AlgoCount();
            ::SetWindowPos(g_hAlgoCombo, nullptr, editX, y, comboW, comboH,
                           SWP_NOZORDER | SWP_NOACTIVATE);

            // ⚠️⚠️ 顺序很关键：**先定几何，再对齐条目高度**。
            //    反过来的话，改几何会把条目高度夹回字体默认值 ——
            //    真机症状正是"下拉框比旁边按钮矮 7px"（32 vs 39），
            //    而且闭合态看着只是"略小一点"，不量根本发现不了。
            AlignComboItems(rowH, m.textH);
        }
    }

    // ---- 第 4 行：状态（占满剩余宽度，高度自适应）----
    y += rowH + rowGap;
    int statusH = H - y - m.margin;
    if (statusH < minStatus) statusH = minStatus;
    ::SetWindowPos(g_hStatus, nullptr, left, y, right - left, statusH,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// ===========================================================================
// 控件
// ===========================================================================
static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text,
                      DWORD style, int id, DWORD exStyle = 0)
{
    HWND h = ::CreateWindowExW(exStyle, cls, text,
                               WS_CHILD | WS_VISIBLE | style,
                               0, 0, 10, 10,
                               parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (h && g_hFont) ::SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return h;
}

static void CreateChildren(HWND parent)
{
    // SS_CENTERIMAGE：单行静态文本在自身矩形里**垂直居中**。
    // 不加的话文字贴着上边，和旁边的编辑框/按钮在视觉上错开半行 —— 看着就"不齐"。
    MakeChild(parent, L"STATIC", L"文件路径", SS_LEFT | SS_CENTERIMAGE, IDC_PATH_LABEL);

    // 路径框：**去掉 ES_READONLY**，改成可键盘输入（王提的需求）。
    // 只读的那些年，想校验一个"资源管理器里复制来的路径"只能点浏览再一路点进去，
    // 现在可以直接 Ctrl+V 粘进来。配合下面的子类化，回车即开始算。
    g_hSrcEdit = MakeChild(parent, L"EDIT", L"",
                           WS_BORDER | ES_AUTOHSCROLL | ES_LEFT,
                           IDC_PATH_EDIT, WS_EX_CLIENTEDGE);

    MakeChild(parent, L"STATIC", L"校验结果", SS_LEFT | SS_CENTERIMAGE, IDC_HASH_LABEL);

    // 结果框保持只读：它是输出，允许选中/复制，但不该被改。
    g_hDstEdit = MakeChild(parent, L"EDIT", L"",
                           WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                           IDC_HASH_EDIT, WS_EX_CLIENTEDGE);

    MakeChild(parent, L"STATIC", L"校验方式", SS_LEFT | SS_CENTERIMAGE, IDC_ALGO_LABEL);

    // 下拉框：CBS_DROPDOWNLIST = 只能选、不能打字（算法名是固定集合，
    // 允许乱输只会制造"输了个不存在的算法"这种没必要的错误态）。
    g_hAlgoCombo = MakeChild(parent, L"COMBOBOX", L"",
                             CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
                             IDC_ALGO_COMBO);
    if (g_hAlgoCombo) {
        for (int i = 0; i < hashcore::AlgoCount(); ++i) {
            ::SendMessageW(g_hAlgoCombo, CB_ADDSTRING, 0,
                           (LPARAM)hashcore::AlgoName(hashcore::AlgoAt(i)));
        }
        ::SendMessageW(g_hAlgoCombo, CB_SETCURSEL, (WPARAM)0, 0);   // 默认 MD5
        g_algo = hashcore::AlgoAt(0);
    }

    MakeChild(parent, L"BUTTON", L"浏览…", BS_PUSHBUTTON, IDC_BROWSE_BTN);
    MakeChild(parent, L"BUTTON", L"计算",   BS_PUSHBUTTON, IDC_CALC_BTN);
    MakeChild(parent, L"BUTTON", L"复制",   BS_PUSHBUTTON, IDC_COPY_BTN);

    g_hStatus = MakeChild(parent, L"STATIC", L"选择或输入要校验的文件，再点「计算」",
                          SS_LEFT | SS_ENDELLIPSIS, IDC_STATUS_TEXT);

    // 字体统一走 ApplyFont()：它按当前 DPI 取系统消息字体，并广播给所有子控件。
    ApplyFont();

    // 路径框子类化（必须放在建完控件之后、字体设好之后都行，只要句柄有效）
    if (g_hSrcEdit) {
        g_pathEditProc = (WNDPROC)(LONG_PTR)::SetWindowLongPtrW(
            g_hSrcEdit, GWLP_WNDPROC, (LONG_PTR)PathEditProc);
    }
}

// ===========================================================================
// 菜单/按钮动作
// ===========================================================================
// 路径规整：
//   · 剪掉首尾空白（从记事本/聊天窗口复制路径，前后常带空格甚至换行）
//   · 剪掉成对引号 —— 资源管理器「复制为路径」给的就是带引号的，
//     直接拿去 CreateFile 会失败（错误码 2），而用户看着明明是对的。
static std::wstring NormalizePath(std::wstring s)
{
    auto trim = [](std::wstring& x) {
        size_t b = x.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos) { x.clear(); return; }
        size_t e = x.find_last_not_of(L" \t\r\n");
        x = x.substr(b, e - b + 1);
    };
    trim(s);
    if (s.size() >= 2) {
        const wchar_t a = s.front(), b = s.back();
        if ((a == L'"' && b == L'"') || (a == L'\'' && b == L'\''))
            s = s.substr(1, s.size() - 2);
    }
    trim(s);
    return s;
}

// 以**界面上的文字**为准更新 g_filePath（支持手输之后，界面才是唯一事实来源）
static void ReadPathFromEdit()
{
    if (!g_hSrcEdit) return;
    const std::wstring raw = WindowTextOf(g_hSrcEdit);
    const std::wstring norm = NormalizePath(raw);
    g_filePath = norm;

    if (norm != raw) {
        // 回写规整后的文本。要先置 g_syncingPath —— 否则这次 SetWindowText
        // 触发的 EN_CHANGE 会被当成"人在改路径"，把结果清掉。
        g_syncingPath = true;
        ::SetWindowTextW(g_hSrcEdit, norm.c_str());
        g_syncingPath = false;
    }
}

// 路径栏被改动了：旧结果已经跟它对不上了，必须作废。
// 不作废的话会出现"路径框里是 A，结果却是 B 的摘要，点复制还把 B 复制走了"。
static void MarkResultStale()
{
    if (g_resultHex.empty()) return;
    g_resultHex.clear();
    if (g_hDstEdit) ::SetWindowTextW(g_hDstEdit, L"");
    SetStatus(L"路径已修改，点「计算」重新校验");
}

// ---------------------------------------------------------------------------
// 拖入文件（王要的）：从资源管理器把文件拖到窗口里，自动填进路径框
//
// ⚠️ 为什么要给**每个子控件**都登记 DragAcceptFiles：
//    落下时 shell 是把 WM_DROPFILES 投给**光标底下的那个窗口**。
//    路径框 / 结果框 / 状态行几乎铺满了整个客户区，只登记主窗口的话，
//    用户只要不是精确落在控件之间的缝隙上就"拖不进去" ——
//    看起来就像这个功能没做。所以子控件也要登记，再挂一层子类过程
//    把消息转给统一的处理函数。
// ---------------------------------------------------------------------------
static void HandleDroppedFiles(HDROP drop);
static void ApplyChosenFile(const std::wstring& path);   // 定义在后面，拖入那条路要用

struct DropChild { HWND hwnd; WNDPROC orig; };
static DropChild g_dropChildren[16];
static int       g_dropChildCount = 0;

static LRESULT CALLBACK DropForwardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DROPFILES) {          // 子控件收到的：转给统一处理
        HandleDroppedFiles((HDROP)wp);
        return 0;
    }
    for (int i = 0; i < g_dropChildCount; ++i) {
        if (g_dropChildren[i].hwnd == hwnd)
            return ::CallWindowProcW(g_dropChildren[i].orig, hwnd, msg, wp, lp);
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// 拖入的轨迹：只记"收到了哪个文件"。
// 为什么值得单独留一手：这条链路的失败是**完全静默**的（UIPI 直接丢消息），
// 有了它就能一句 `type NppDockApp_MD5.trace.log` 分清是
// "消息根本没到"（没有这一行）还是"到了但没填进去"（有这一行）。
// v2.0：轨迹日志的路径（顺手滚一次）。以前这个路径在 TraceLine / TraceDrop
// 里各算了一遍，而且文件只追加、永不清理 —— 每次启动写十来行，日积月累很大。
static std::wstring TracePathOnce()
{
    wchar_t p[MAX_PATH * 2]{};
    ::GetModuleFileNameW(nullptr, p, _countof(p));
    std::wstring f(p);
    const size_t k = f.find_last_of(L'.');
    f = (k == std::wstring::npos ? f : f.substr(0, k)) + L".trace.log";

    static bool rotated = false;
    if (!rotated) {
        rotated = true;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad)) {
            const unsigned long long sz =
                ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (sz >= 1024ull * 1024ull) {
                const std::wstring old = f + L".old";
                ::DeleteFileW(old.c_str());
                ::MoveFileW(f.c_str(), old.c_str());
            }
        }
    }
    return f;
}

static void TraceLine(const char* msg)
{
    const std::wstring f = TracePathOnce();

    HANDLE h = ::CreateFileW(f.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    sprintf_s(buf, "%s\r\n", msg);
    DWORD wrote = 0;
    ::WriteFile(h, buf, (DWORD)strlen(buf), &wrote, nullptr);
    ::CloseHandle(h);
}

static void TraceDrop(const wchar_t* path)
{
    const std::wstring f = TracePathOnce();

    HANDLE h = ::CreateFileW(f.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char u8[MAX_PATH * 3]{};
    ::WideCharToMultiByte(CP_UTF8, 0, path, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
    char buf[MAX_PATH * 3 + 32];
    sprintf_s(buf, "[拖入] %s\r\n", u8);
    DWORD wrote = 0;
    ::WriteFile(h, buf, (DWORD)strlen(buf), &wrote, nullptr);
    ::CloseHandle(h);
}

// ⚠️⚠️ 光 DragAcceptFiles 是**不够的** —— 这是"拖了但填不进去"的真因：
//
//   UIPI（用户界面特权隔离）会把**低完整性进程发给高完整性窗口**的那些消息
//   **静默丢弃**。而这台机器上：
//       explorer.exe（拖放的发起点）= Medium
//       notepad++.exe 及其子进程（我们）= High（管理员）
//   于是资源管理器发过来的 WM_DROPFILES 在到达我们窗口之前就被拦掉了 ——
//   表现正是"能拖、光标也显示可以放，但路径框毫无反应"，而且**一点错误都不报**。
//
//   解法（微软文档里给的标准做法）：对自己这几个窗口显式放行三条消息。
//     · WM_DROPFILES (0x0233)       —— 拖进来的文件
//     · WM_COPYDATA  (0x004A)       —— 自动化入口那条
//     · WM_COPYGLOBALDATA (0x0049)  —— 拖放时 shell 用它搬运数据
//   只对**我们自己的窗口**放行，不是全局放宽权限。
//
// （同样的逻辑在 appui 里有一份给「文件背包」的文件栏用。）
static void AllowDropMessages(HWND h)
{
    typedef BOOL (WINAPI* PFN)(HWND, UINT, DWORD, void*);
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) return;
    auto f = (PFN)::GetProcAddress(u, "ChangeWindowMessageFilterEx");
    if (!f) return;                       // Win7 之前没有这个 API：那就只能作罢
    const DWORD kAllow = 1;               // MSGFLT_ALLOW
    f(h, WM_DROPFILES, kAllow, nullptr);
    f(h, WM_COPYDATA,  kAllow, nullptr);
    f(h, 0x0049u /*WM_COPYGLOBALDATA*/, kAllow, nullptr);
}

// 放行必须在**真实拖放之前**做过，而且是逐窗口做的。
// 这条链路的失败是静默的（消息被丢掉、不报错），所以留一行痕迹：
// 以后"拖进去没反应"时，先看 trace.log 里有没有这行。
static void LogDropEnabled(HWND h)
{
    typedef BOOL (WINAPI* PFN)(HWND, UINT, DWORD, void*);
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    auto f = u ? (PFN)::GetProcAddress(u, "ChangeWindowMessageFilterEx") : nullptr;
    char b[160];
    sprintf_s(b, "[拖放] 窗口 %p 已放行 UIPI 消息（ChangeWindowMessageFilterEx %s）",
              (void*)h, f ? "可用" : "不存在（XP 时代系统）");
    TraceLine(b);
}

static void EnableFileDrop(HWND parent)
{
    ::DragAcceptFiles(parent, TRUE);
    AllowDropMessages(parent);
    LogDropEnabled(parent);

    g_dropChildCount = 0;
    for (HWND h = ::GetWindow(parent, GW_CHILD);
         h && g_dropChildCount < (int)_countof(g_dropChildren);
         h = ::GetWindow(h, GW_HWNDNEXT)) {
        ::DragAcceptFiles(h, TRUE);
        AllowDropMessages(h);
        WNDPROC orig = (WNDPROC)(LONG_PTR)::SetWindowLongPtrW(
            h, GWLP_WNDPROC, (LONG_PTR)DropForwardProc);
        if (orig) {
            g_dropChildren[g_dropChildCount].hwnd = h;
            g_dropChildren[g_dropChildCount].orig = orig;
            ++g_dropChildCount;
        }
    }
}

static void HandleDroppedFiles(HDROP drop)
{
    if (!drop) return;
    const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (n > 0) {                        // 拿到文件了：留一条痕迹（排查"没反应"时最有用）
        wchar_t b[MAX_PATH * 2]{};
        ::DragQueryFileW(drop, 0, b, _countof(b));
        TraceDrop(b);
    }
    wchar_t path[MAX_PATH * 2] = {};
    if (n > 0) ::DragQueryFileW(drop, 0, path, _countof(path));
    ::DragFinish(drop);                 // 无论用不用都得还回去，否则泄漏
    if (!path[0]) return;

    // 和「键入路径」那条入口用同一句文案 —— 同一个事实不该有两种说法。
    if (IsDirW(path)) {
        SetStatus(L"这是一个文件夹，本工具只校验单个文件");
        return;
    }

    ApplyChosenFile(path);
    if (n > 1) {
        SetStatus(L"拖进来 " + std::to_wstring(n) +
                  L" 个文件，只取了第一个；点「计算」开始校验");
    }
}

// 把"选定了这个文件"落到界面上。
// 浏览… / 拖入 / 自动化入口三条路共用一份逻辑，免得三处各写一遍再各自跑偏。
//
// ⚠️ g_syncingPath 必须包住 SetWindowTextW：那是**程序**在改路径框，
//    不能让 EN_CHANGE 把它当成"人在改"而把刚算好的结果作废。
static void ApplyChosenFile(const std::wstring& path)
{
    if (path.empty() || !g_hSrcEdit) return;

    g_syncingPath = true;
    ::SetWindowTextW(g_hSrcEdit, path.c_str());
    g_syncingPath = false;

    g_filePath = path;
    g_resultHex.clear();
    if (g_hDstEdit) ::SetWindowTextW(g_hDstEdit, L"");
    SetStatus(L"文件已选择，点「计算」开始校验（" +
              std::wstring(hashcore::AlgoName(g_algo)) + L"）");
}

static void OnBrowse()
{
    wchar_t file[MAX_PATH * 2] = {};
    // 用界面上现有的路径当对话框的起始位置（手输过一半再点浏览也不会丢）
    const std::wstring cur = NormalizePath(WindowTextOf(g_hSrcEdit));
    if (!cur.empty()) wcsncpy_s(file, cur.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hWnd;
    ofn.lpstrFilter = L"所有文件\0*.*\0文本文件\0*.txt\0\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = _countof(file);
    ofn.lpstrTitle  = L"选择要校验的文件";
    // OFN_NOCHANGEDIR：不加的话 common dialog 会把当前工作目录改掉，
    // 而我们的工作目录正是 dock 插件目录 —— 被改掉会影响 dock 后续按相对路径找资源。
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!::GetOpenFileNameW(&ofn)) return;
    ApplyChosenFile(file);      // 与"拖入"走同一条落地逻辑
}

// 「计算」按钮的两种身份。
// 王选的是"变身"而不是"再占一个位置"：面板被拖窄时横向空间很紧张，
// 多一个常驻按钮（而且空闲时还是灰的）性价比太低。
// 反过来只要记住一条：**这个按钮的语义由 g_busy 决定**。
static void UpdateCalcButton()
{
    HWND b = ::GetDlgItem(g_hWnd, IDC_CALC_BTN);
    if (b) ::SetWindowTextW(b, g_busy ? L"终止" : L"计算");
}

// 点一下「计算/终止」。
//   · 空闲 -> 开始算（按钮随即变成「终止」）
//   · 正在算 -> 置取消标志（真正的退出发生在读下一块时，见 ComputeFileHash）
static void OnCalcOrAbort()
{
    if (g_busy) {
        g_cancelRequested = true;
        // 这里给个即时反馈：真正退出要等下一块读完，大文件上是"瞬间"的，
        // 但万一是网络盘/慢盘，没有这句会让人以为点了没反应。
        SetStatus(L"正在终止…");
        return;
    }

    // ⚠️ 一律以**界面上的路径**为准，不再依赖 g_filePath 的旧值 ——
    //    支持键盘输入之后，界面才是唯一事实来源。
    ReadPathFromEdit();
    if (g_filePath.empty()) {
        SetStatus(L"请先选择文件，或直接输入文件路径");
        ::SetFocus(g_hSrcEdit);
        return;
    }

    g_busy = true;
    g_cancelRequested = false;
    UpdateCalcButton();                     // 「计算」->「终止」
    // ⚠️「计算」按钮**不能**禁用 —— 它此刻正是「终止」，
    //    禁用它就等于把唯一能停下来的入口关掉了。
    ::EnableWindow(::GetDlgItem(g_hWnd, IDC_BROWSE_BTN), FALSE);
    ::EnableWindow(::GetDlgItem(g_hWnd, IDC_COPY_BTN), FALSE);
    ::EnableWindow(g_hAlgoCombo, FALSE);
    ::EnableWindow(g_hSrcEdit, FALSE);      // 算的过程中别让路径再变
    ::SetWindowTextW(g_hDstEdit, L"");

    wchar_t b[128];
    swprintf_s(b, L"正在计算 %s…（想停下就点「终止」）", hashcore::AlgoName(g_algo));
    SetStatus(b);

    std::string hex;
    const ComputeResult r = ComputeFileHash(g_filePath, g_algo, hex);

    if (r == ComputeResult::Ok) {
        g_resultHex = hex;
        // hex 是窄字符（纯 ASCII 十六进制），转宽显示
        std::wstring w(hex.begin(), hex.end());
        ::SetWindowTextW(g_hDstEdit, w.c_str());
    }
    // Cancelled / Failed 的情况下结果框本来就是空的，这里不再动它 ——
    // 尤其**不能**保留上一次的结果，否则"这个摘要是哪个文件的"就说不清了。

    if (::IsWindow(g_hWnd)) {
        ::EnableWindow(::GetDlgItem(g_hWnd, IDC_BROWSE_BTN), TRUE);
        ::EnableWindow(::GetDlgItem(g_hWnd, IDC_COPY_BTN), TRUE);
        ::EnableWindow(g_hAlgoCombo, TRUE);
        ::EnableWindow(g_hSrcEdit, TRUE);
    }
    g_busy = false;
    UpdateCalcButton();                     // 「终止」->「计算」
    // 终止之后焦点回到路径框，方便立刻换个文件再来一次
    if (r == ComputeResult::Cancelled && ::IsWindow(g_hWnd)) ::SetFocus(g_hSrcEdit);
}

// 路径栏里敲回车 = "就这个路径，开始算"。
// 这是键盘输入路径之后最自然的延续动作，不加的话用户敲完只能去够鼠标。
static void OnPathCommitted()
{
    ReadPathFromEdit();
    OnCalcOrAbort();
}

static void OnCopy()
{
    if (g_resultHex.empty()) { SetStatus(L"还没有结果可复制"); return; }

    const std::wstring w(g_resultHex.begin(), g_resultHex.end());
    if (!::OpenClipboard(g_hWnd)) { SetStatus(L"打开剪贴板失败"); return; }
    ::EmptyClipboard();

    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        if (void* p = ::GlobalLock(h)) {
            memcpy(p, w.c_str(), bytes);
            ::GlobalUnlock(h);
            // ⚠️ SetClipboardData 成功后所有权转移给系统，绝不能再 GlobalFree，
            //    否则剪贴板里会留下已释放的指针（粘贴时崩）。
            if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
        } else {
            ::GlobalFree(h);
        }
    }
    ::CloseClipboard();
    SetStatus(std::wstring(L"已复制 ") + hashcore::AlgoName(g_algo) + L" 结果到剪贴板");
}

// 换算法。
// 语义设计：如果**已经算过一次**（结果框里有东西），就直接按新算法重算 ——
// 换下拉框的人此刻的意图必然是"我要看另一种算法的结果"，让他再点一次「计算」
// 是多余的。如果还没算过，就只更新状态提示，不动任何东西。
static void OnAlgoChanged()
{
    if (!g_hAlgoCombo) return;
    const int sel = (int)::SendMessageW(g_hAlgoCombo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return;

    g_algo = hashcore::AlgoAt(sel);

    if (!g_filePath.empty() && !g_resultHex.empty()) {
        OnCalcOrAbort();
        return;
    }
    std::wstring msg = L"已选择 ";
    msg += hashcore::AlgoName(g_algo);
    msg += g_filePath.empty() ? L"，请选择或输入要校验的文件"
                              : L"，点「计算」开始校验";
    SetStatus(msg);
}

// ===========================================================================
// 路径框子类化
// ---------------------------------------------------------------------------
// 为什么需要：单行 EDIT 在**普通窗口**（不是对话框）里，回车没有任何人接管 ——
//   既不会通知父窗口，也不会触发默认按钮（那是对话框管理器的活）。
//   不接管的结果是"敲完路径按回车毫无反应"，或者更糟：emit 一个响铃。
//   所以这里把窗口过程换掉，自己吃掉回车。
//
// 为什么同时吃 WM_KEYDOWN 和 WM_CHAR：
//   有些情形下 EDIT 在 WM_CHAR 之前就做了默认处理（并且会响铃），
//   两边都拦一遍最省事，代价是基本为零。
// ===========================================================================
static LRESULT CALLBACK PathEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) return 0;              // 吃掉，别让它变成响铃
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == L'\n') {           // 回车 -> 开始算
            OnPathCommitted();
            return 0;
        }
        break;
    default:
        break;
    }
    return ::CallWindowProcW(g_pathEditProc, hwnd, msg, wp, lp);
}

// ===========================================================================
// 下拉框条目高度
// ---------------------------------------------------------------------------
// 下拉框的"闭合高度"不像按钮那样能直接指定：它由**条目高度**推出来。
// 实测量到的关系（tools/dock_app_probe.py combo 会打印这条）：
//
//     闭合高度 = 条目高度 + 2 × Dp(2)
//
// 所以想要闭合高度正好等于 rowH，条目高度就得设成 rowH - 2*Dp(2)。
// 那个 2*Dp(2) 是控件自己画的上下边框，跟系统度量有关，不是我们定的 ——
// 这也是为什么这里宁可"按实测关系反推"，也不去猜边框几像素。
//
// ⚠️ 另外两个反直觉的点：
//   1) SetWindowPos 给下拉框设的高度是**落下时**的总高度，闭合态会被系统夹回去。
//      所以布局里给的是 rowH + 列表高度，别照抄成 rowH。
//   2) 改几何会把条目高度夹回字体默认值，所以**必须在 SetWindowPos 之后**
//      才调这个函数（见 LayoutChildren 里的调用点）。
//      这里刻意不留"上次设过就不重设"的缓存 —— 缓存会和这个重置行为打架。
// ===========================================================================
static void AlignComboItems(int rowH, int textH)
{
    if (!g_hAlgoCombo || rowH <= 0) return;

    const int border = 2 * Dp(2);             // 实测：闭合高度 = 条目高度 + 这个值
    int itemH = rowH - border;
    if (itemH < Dp(12)) itemH = Dp(12);       // 别把条目压得连字都放不下

    // ★ 列表项**紧凑化**（王 2026-10-03 的要求）：条目高度只按"一行字 + 一点内边距"，
    //   不再跟着按钮行高走 —— 行高里含按钮的上下留白，展开后看着很松散。
    //   ⚠️ 选择框（-1）仍然用 rowH - border：**闭合**状态必须和旁边的按钮同高，
    //      否则整行的基线会错位（那个"下拉框比按钮矮 7px"的老坑就是这么来的）。
    int listH = (textH > 0 ? textH : Dp(14)) + Dp(4);
    if (listH < Dp(12)) listH = Dp(12);
    if (listH > rowH)   listH = rowH;         // 别反倒比行还高

    ::SendMessageW(g_hAlgoCombo, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)itemH);
    ::SendMessageW(g_hAlgoCombo, CB_SETITEMHEIGHT, (WPARAM)0,  (LPARAM)listH);
}

// ===========================================================================
// 窗口过程
// ===========================================================================
// 窗口所在显示器的缩放变了就把字体和 DPI 基准换掉。
// 子窗口收不到 WM_DPICHANGED（那个只发给顶层窗口），所以由 WM_SIZE 兜底调用 ——
// 宿主被拖到另一块不同缩放的显示器时，嵌进来这一页也跟着重新缩放。
static void SyncDpi()
{
    UINT d = DetectDpi(g_hWnd);
    if (d && d != g_dpi) {
        g_dpi = d;
        ApplyFont();
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        // ⚠️⚠️ 必须**在这里**先把 g_hWnd 填上，再用它当父窗口建控件。
        //
        // 真机踩到（症状极隐蔽，而且我一开始误判成"布局算错了"）：
        //   CreateWindowExW 是在**返回之前**就把 WM_CREATE 发给窗口过程的，
        //   所以外面那句 g_hWnd = CreateWindowExW(...) 此刻还没执行 ——
        //   g_hWnd 仍是 nullptr。
        //   于是所有控件全被创建成"以 NULL 为父窗口的孤儿"（落到了桌面上），
        //   窗口里当然一个控件都没有，界面一片空白；
        //   更坑的是 GetDlgItem(g_hWnd, id) 也一律返回 NULL，
        //   LayoutChildren 里所有的 SetWindowPos 全部静默失败（没有报错）。
        //   结论：窗口"外框好看、里面全空"时，先查父窗口句柄到底是不是 NULL。
        g_hWnd = hwnd;
        CreateChildren(hwnd);
        EnableFileDrop(hwnd);       // 支持把文件从资源管理器拖进来
        return 0;

    case WM_SIZE:
        SyncDpi();
        LayoutChildren(LOWORD(lp), HIWORD(lp));
        // ⚠️⚠️ 这里有两个坑，都是"空白界面"级别的，别顺手改回去。
        //
        // 【坑一】改尺寸之后**不会自动重画**。
        //   窗口类虽然声明了 CS_HREDRAW | CS_VREDRAW，但嵌进 dock 之后不生效：
        //   窗口的"可见内容"由宿主那边（另一个进程）决定 ——
        //   它调整布局后，我们既收不到重画请求，也没人替我们作废客户区，
        //   屏幕就停在旧内容上。所以必须自己作废。
        //
        // 【坑二】只 InvalidateRect(自己) 是**不够的：子控件不会跟着重画**。
        //   本窗口自己没有 WM_PAINT 处理器（走 DefWindowProc，只 validate 更新区），
        //   真正的内容全是子控件自己画的；InvalidateRect 只作用于本窗口，
        //   子控件不在更新区里，于是它们保持旧像素 ——
        //   表现就是"背景刷出来了，控件却稀稀拉拉只剩几个"。
        //   RDW_ALLCHILDREN 会把整棵子树一起标脏，每个控件各自排一次 WM_PAINT。
        //
        // ⚠️ 只作废、**不要** RDW_UPDATENOW。本窗口是跨进程嵌进 dock 的，
        //   "同步重画"会让父进程（Notepad++）的 UI 线程等我们画完；
        //   一旦绘制路径回头去找父窗口，双方互等 —— 直接死锁，
        //   症状是 Notepad++ 整个"未响应"（dock 侧踩过，见 docs/铁律与踩坑要点.md「窗口与重绘」）。
        RepaintSubtree(hwnd);
        return 0;

    case WM_SHOWWINDOW:
        // 被**显示**时，安排一次整棵子树重画。
        //
        // 为什么必须自己管：本窗口是被 dock 跨进程 SetParent 上去的，
        //   "重新显示"之后父进程那边不会（也没法）替我们把子控件重画 ——
        //   它只能作废我们**这个**窗口，作废不到我们进程里的子控件。
        //   实测症状：隐藏再显示之后，界面只剩一块底色，控件全不见了。
        //   （dock 侧那三层"补重画"都是跨进程的，对子控件无能为力；
        //     跨进程强制同步重画又是死锁源，见 WM_SIZE 里的说明。）
        //
        // wp != 0 是"真的被显示"，lp != 0 是"因为父窗口被显示而跟着显示" ——
        //   两种情况都要管，因为 dock 面板被唤出时收到的是后者。
        // 注意是 PostSelfRepaint（排到队列末尾），不是当场重画，原因见它的注释。
        if (wp) PostSelfRepaint(hwnd);
        break;

    case WM_WINDOWPOSCHANGED:
        // 几何发生变化（含被显示、被移动、被重新挂载）之后也安排一次。
        //   ⚠️ 不能省：真机上"换标签页/重新挂载"并不总会带上 WM_SHOWWINDOW，
        //   而这条消息一定会来 —— 它是 Windows 对"窗口位置尺寸变了"的兜底通知。
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
            // 顶层窗口：按系统建议的新位置/尺寸重设
            const RECT* pr = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                           pr->right - pr->left, pr->bottom - pr->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        {
            RECT cr{};
            ::GetClientRect(hwnd, &cr);
            LayoutChildren(cr.right, cr.bottom);
        }
        // 同上：连子控件一起作废，否则控件保持旧尺寸/旧像素
        RepaintSubtree(hwnd);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        // 最小尺寸由**实测度量**推出来，不是写死的数字。
        // 写死的高/宽在 150% 缩放下会把按钮和文字挤变形（真机踩过）。
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        int cw = 0, ch = 0;
        ClientSizeFor(Dp(80), cw, ch);
        if (!g_embedded) {
            // 顶层窗口的 ptMinTrackSize 是**整窗**尺寸（含标题栏与边框）
            RECT r{ 0, 0, cw, ch };
            AdjustRectForDpi(hwnd, r);
            cw = r.right - r.left;
            ch = r.bottom - r.top;
        }
        mmi->ptMinTrackSize.x = cw;
        mmi->ptMinTrackSize.y = ch;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE_BTN:
            if (HIWORD(wp) == BN_CLICKED) OnBrowse();
            return 0;

        case IDC_CALC_BTN:
            // 同一个按钮：空闲时是「计算」，算的过程中是「终止」。
            if (HIWORD(wp) == BN_CLICKED) OnCalcOrAbort();
            return 0;

        case IDC_COPY_BTN:
            if (HIWORD(wp) == BN_CLICKED) OnCopy();
            return 0;

        case IDC_ALGO_COMBO:
            // CBN_SELCHANGE 是"用户选了另一项"（不是我们 CB_SETCURSEL 设的 ——
            // 那种不会发通知），所以这里可以放心地当成用户意图处理。
            if (HIWORD(wp) == CBN_SELCHANGE) OnAlgoChanged();
            return 0;

        case IDC_PATH_EDIT:
            // EN_CHANGE：人（或我们回写）改了路径框。人在改就把旧结果作废。
            if (HIWORD(wp) == EN_CHANGE && !g_syncingPath) MarkResultStale();
            return 0;

        case IDOK:
            // 兜底：某些情况下单行 EDIT 会把回车转成 WM_COMMAND/IDOK 发给父窗口
            //（子类化里已经拦了一道，这里是第二道，防"某个 Windows 版本行为不同"）。
            if (HIWORD(wp) == BN_CLICKED) OnPathCommitted();
            return 0;

        default:
            break;
        }
        break;

    case WM_DROPFILES:
        // 拖进来的文件（主窗口或任一子控件收到都汇总到这里）
        HandleDroppedFiles((HDROP)wp);
        return 0;

    case WM_COPYDATA: {
        // 自动化/自检入口：别的进程用 WM_COPYDATA 递一个路径过来。
        // ⚠️ 必须用 WM_COPYDATA 而不是自定义消息 + 指针 —— 这条消息系统会
        //    **替你封送**数据，跨进程安全；直接传指针会让对面解引用野地址
        //    （踩坑记录 19.1，写崩过 Notepad++ 一次）。
        // dwData == 1 表示"这是一条文件路径"。
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lp;
        if (cds && cds->dwData == 1 && cds->lpData &&
            cds->cbData >= sizeof(wchar_t)) {
            const wchar_t* p = (const wchar_t*)cds->lpData;
            const size_t maxChars = cds->cbData / sizeof(wchar_t);
            size_t len = 0;
            while (len < maxChars && p[len]) ++len;
            ApplyChosenFile(std::wstring(p, len));
            return TRUE;
        }
        break;
    }

    case WM_SETFOCUS:
        // 焦点给路径框：现在它支持键盘输入了，一进来就能直接粘/敲路径，
        // 不用先去够鼠标。（以前给的是「浏览」按钮。）
        ::SetFocus(g_hSrcEdit);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
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
        if (g_hFont)       { ::DeleteObject(g_hFont);       g_hFont = nullptr; }
        if (g_hStatusFont) { ::DeleteObject(g_hStatusFont); g_hStatusFont = nullptr; }
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// 命令行解析
// ---------------------------------------------------------------------------
// ⚠️⚠️ 这里有个**必须记住**的坑（真机踩到，且症状极隐蔽）：
//   wWinMain 的第 3 个参数 lpCmdLine 是**不含程序名**的命令行；
//   而 CommandLineToArgvW 是按"**含** argv[0]"的约定解析的。
//   把 lpCmdLine 直接喂给它，第一个开关就会被当成 argv[0] 吃掉 ——
//   于是 --selftest / --dock-parent 永远匹配不上，程序静默地退化成一个
//   独立的顶层窗口（看起来"什么都没发生"），排查起来非常费劲。
//   正解：一律用 GetCommandLineW() 拿完整命令行。
//
// 另一个刻意的设计：本文件**不 include dock 那边的头文件**、不引用它的常量。
//   应用只依赖 Win32，两边靠**字符串契约**（--dock-parent）对接，
//   这样应用能单独编译、单独发布，也不会"改 dock 一行、应用全要重编"。
// ===========================================================================
// ===========================================================================
// DPI 感知：必须与宿主一致
// ---------------------------------------------------------------------------
// ⚠️ 真机踩到的坑（现象极具误导性）：
//   普通新建的 Win32 进程默认是 DPI_UNAWARE。当它的窗口被跨进程 SetParent
//   到 dock 里（dock 跑在 DPI-aware 的 Notepad++ 进程内）时，系统会对这个
//   子窗口做一次 **forced reset**（MSDN 的 SetParent 备注里有专门的表格行
//   "SetParent (Cross-Proc) → Forced reset"，所有 Windows 版本都这样）。
//   结果是两边坐标空间不一致：宿主客户区真实是 1196x524，
//   应用量出来是 797x349（×2/3，即按 150% 缩放虚拟化后的值），
//   于是嵌进去的窗口只铺满约 2/3，右边和下边露白。
//
//   正解：在**创建任何窗口之前**，把本进程的 DPI 感知等级设成和宿主一样。
//   这里优先直接读取宿主窗口的感知上下文来对齐，读不到再退回 per-monitor-v2。
//
//   注意：本文件刻意不 include 新版本 SDK 的 DPI 类型，改用 GetProcAddress
//   动态取函数指针 —— 这样在旧 SDK/WIN32_WINNT 下也能编译，不需要额外的
//   manifest 或链接设置。
// ===========================================================================
typedef HANDLE(WINAPI* PFN_GetWindowDpiAwarenessContext)(HWND);
typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);

// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
static const HANDLE kPerMonitorAwareV2 = (HANDLE)(INT_PTR)-4;

static void AdoptHostDpiAwareness(HWND hHost)
{
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) return;

    auto setCtx = (PFN_SetProcessDpiAwarenessContext)
        ::GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (!setCtx) return;   // Win10 1703 以下没有这个 API，保持系统默认

    // 1) 首选：直接抄宿主的感知等级
    if (hHost) {
        auto getCtx = (PFN_GetWindowDpiAwarenessContext)
            ::GetProcAddress(u, "GetWindowDpiAwarenessContext");
        if (getCtx) {
            HANDLE ctx = getCtx(hHost);
            if (ctx && setCtx(ctx)) return;
        }
    }

    // 2) 兜底：per-monitor-v2（绝大多数现代宿主都用这个）
    setCtx(kPerMonitorAwareV2);
}

static HWND ParseDockParent()
{
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return nullptr;

    HWND h = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dock-parent") == 0) {
            unsigned long long v = _wcstoui64(argv[i + 1], nullptr, 10);
            h = reinterpret_cast<HWND>(static_cast<UINT_PTR>(v));
            break;
        }
    }
    ::LocalFree(argv);
    return h;
}

// ===========================================================================
// 自检模式：--selftest <文件> [--algo md5|sha1|sha256|sha384|sha512|crc32]
// ---------------------------------------------------------------------------
// 为什么留这么一个不走界面的口子：
//   摘要算法是"算错也不会报错"的典型 —— 出错时照样吐一串十六进制，
//   肉眼根本分辨不出。必须能和权威实现对照才能证明实现是对的。
//   有了它，构建后跑一行命令就能做回归：
//     NppDockApp_MD5.exe --selftest foo.bin
//     NppDockApp_MD5.exe --selftest foo.bin --algo sha256
//     certutil -hashfile foo.bin SHA256
//
// --algo 缺省是 md5 —— 保证老的调用方式（不传 --algo）行为完全不变。
// ===========================================================================
static bool ParseSelfTest(std::wstring& fileOut, hashcore::Algo& algoOut)
{
    algoOut = hashcore::Algo::Md5;

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return false;

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--selftest") == 0 && i + 1 < argc) {
            fileOut = argv[i + 1];
            found = true;
        } else if (_wcsicmp(argv[i], L"--algo") == 0 && i + 1 < argc) {
            hashcore::Algo a;
            if (hashcore::AlgoFromName(argv[i + 1], &a)) algoOut = a;
        }
    }
    ::LocalFree(argv);
    return found;
}

static std::string NarrowFromWide(const std::wstring& w)
{
    if (w.empty()) return "";
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return "";
    std::string s((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], need,
                          nullptr, nullptr);
    return s;
}

static int RunSelfTest(const std::wstring& file, hashcore::Algo algo)
{
    // 标准输出先原样取回来。
    // ⚠️ 不要一上来就 AttachConsole：AttachConsole 成功时会把本进程的
    //    标准句柄**替换成控制台的句柄**，于是调用方（构建脚本）重定向到
    //    管道的那个句柄就被冲掉了，输出全跑到屏幕上，脚本一个字都拿不到。
    //    正确顺序：先看现在的句柄能不能用，不能用（GUI 子系统双击运行）才去挂控制台。
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = (hOut && hOut != INVALID_HANDLE_VALUE)
                     ? ::GetFileType(hOut) : FILE_TYPE_UNKNOWN;

    if (type == FILE_TYPE_UNKNOWN) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
            type = (hOut && hOut != INVALID_HANDLE_VALUE)
                       ? ::GetFileType(hOut) : FILE_TYPE_UNKNOWN;
        }
    }
    if (type == FILE_TYPE_UNKNOWN) return 3;

    std::string hex;
    if (ComputeFileHash(file, algo, hex) != ComputeResult::Ok) {
        // 失败原因也写出来 —— 否则自检只会"静默返回 2"，用的人不知道发生了什么。
        if (!g_lastStatus.empty()) {
            const std::string m = NarrowFromWide(g_lastStatus) + "\r\n";
            DWORD w = 0;
            ::WriteFile(hOut, m.data(), (DWORD)m.size(), &w, nullptr);
        }
        return 2;
    }

    std::string line = NarrowFromWide(hashcore::AlgoName(algo));
    line += " " + NarrowFromWide(file) + " = " + hex + "\r\n";
    DWORD wrote = 0;
    ::WriteFile(hOut, line.data(), (DWORD)line.size(), &wrote, nullptr);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR /*lpCmdLine*/, int)
{
    g_hInst = hInst;

    // 自检模式：不开窗口，算完直接退出（供构建后与权威实现对照）
    {
        std::wstring selftestFile;
        hashcore::Algo selftestAlgo = hashcore::Algo::Md5;
        if (ParseSelfTest(selftestFile, selftestAlgo)) {
            return RunSelfTest(selftestFile, selftestAlgo);
        }
    }

    // 先解析参数再建窗口：样式必须一次到位。
    // 若先建成顶层再改成子窗口，中间那一瞬窗口会在桌面上闪一下。
    HWND hDockParent = ParseDockParent();
    g_embedded = (hDockParent != nullptr);

    // 必须在建窗口之前定好 DPI 感知，否则跨进程 SetParent 之后
    // 坐标空间会被系统强制重置，嵌进去的窗口尺寸会算错（见上方注释）。
    AdoptHostDpiAwareness(hDockParent);

    // DPI 基准也要在建窗口之前定好 —— 初始尺寸、字体、行高全都要用它。
    //   嵌入模式：直接问宿主窗口（本进程的感知等级刚跟它对齐过）
    //   单开模式：问主显示器
    // 注意这个顺序不能颠倒：先对齐感知等级，再量 DPI，量出来才是同一套坐标。
    g_dpi = DetectDpi(hDockParent);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return 1;

    DWORD style   = g_embedded ? (WS_POPUP) : (WS_OVERLAPPEDWINDOW);
    DWORD exStyle = g_embedded ? WS_EX_TOOLWINDOW : 0;

    g_hWnd = ::CreateWindowExW(exStyle, kClassName, L"文件校验", style,
                               64, 64, 720, 220,
                               nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) return 2;

    if (g_embedded) {
        // -------------------------------------------------------------------
        // 子窗口化。顺序**必须**是这样（MSDN SetParent 备注里写明的）：
        //   改样式（去 WS_POPUP / 加 WS_CHILD）-> SetParent -> SetWindowPos
        //
        // 为什么不能反过来先 SetParent 再改样式：
        //   窗口在 SetParent 那一刻仍是 WS_POPUP，系统会按"桌面的子窗口"接
        //   管它的布局，随后再改 WS_CHILD 需要一次 SWP_FRAMECHANGED 才能让
        //   系统重新算非客户区，多一次闪烁且偶发尺寸不生效。
        // -------------------------------------------------------------------
        LONG_PTR st = ::GetWindowLongPtrW(g_hWnd, GWL_STYLE);
        st &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        st |= WS_CHILD;
        ::SetWindowLongPtrW(g_hWnd, GWL_STYLE, st);

        ::SetParent(g_hWnd, hDockParent);

        RECT rc{};
        ::GetClientRect(hDockParent, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0) w = 400;
        if (h <= 0) h = 180;
        ::SetWindowPos(g_hWnd, nullptr, 0, 0, w, h,
                       SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(g_hWnd, SW_SHOW);
    } else {
        // 独立打开：按**实测度量**算出合适的初始窗口大小，再居中。
        // 不用写死的 720x220 —— 150% 缩放下那个尺寸会把三行控件压得很难看。
        int cw = 0, ch = 0;
        ClientSizeFor(Dp(kBaseEditW), cw, ch);

        RECT r{ 0, 0, cw, ch };
        AdjustRectForDpi(g_hWnd, r);           // 加上标题栏/边框
        int ww = r.right - r.left;
        int wh = r.bottom - r.top;
        if (ww < 360) ww = 360;
        if (wh < 200) wh = 200;

        RECT wa{};
        if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            wa = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                        ::GetSystemMetrics(SM_CYSCREEN) };
        }
        int x = wa.left + ((wa.right - wa.left) - ww) / 2;
        int y = wa.top + ((wa.bottom - wa.top) - wh) / 3;
        ::SetWindowPos(g_hWnd, nullptr, x, y, ww, wh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(g_hWnd, SW_SHOW);
    }

    ::UpdateWindow(g_hWnd);
    UpdateWindowTitle();

    {
        RECT cr{};
        ::GetClientRect(g_hWnd, &cr);
        LayoutChildren(cr.right, cr.bottom);
    }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        // 嵌入模式没有对话框，Tab 键自行处理即可
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}
