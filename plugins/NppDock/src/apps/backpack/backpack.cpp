// ===========================================================================
// backpack.cpp —— 「文件背包」的界面层
//
// 版式（王定的；v1.6 起左右固定 38/62、分隔条只当分界线不再能拖；
//       v1.7 起按钮排挪到整行、状态与设置/刷新各占一段）：
//   ┌───────────────────────────────────────────────────────────────────┐
//   │ [返回上级][文件属性][删除][上传][下载][打开文件位置]                │
//   ├──────────────────────────┬────────────────────────────────────────┤
//   │ ● 已连接·就绪              │                      [⚙ 设置][↻ 刷新] │
//   ├──────────────────────────┼────────────────────────────────────────┤
//   │       文件栏 38%          │          便笺区 62%（可编辑）           │
//   └──────────────────────────┴────────────────────────────────────────┘
//
// 三条"必须守"的契约（其余应用也一样，见《NppDock应用开发指南》）：
//   R1 命令行解析 / R2 建窗口前先定 DPI / R3 嵌入时改样式→SetParent→SetWindowPos
//   R5 干净退出 / R6 重绘只作废、永不 RDW_UPDATENOW
//
// 另有三个"模式"：
//   --askpass      密码认证时给 ssh.exe 当"问密码的程序"（见 packcore.h 的说明）
//   --selftest     按配置文件连一次并报告（不开窗口）
//   --sshraw       把探针那条 ssh 命令原样跑一遍、打原始输出（连不上时排障用）
// ===========================================================================
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <commdlg.h>    // GetSaveFileNameW（导出到本地）/ GetOpenFileNameW（上传）
#include <commctrl.h>   // 鼠标悬停提示（tooltip）

#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "packcfg.h"
#include "packcore.h"
#include "packprops.h"
#include "../../common/appui.h"

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
static const wchar_t* kClassName   = L"NppDockBackpackWnd";
static const wchar_t* kSplitterCls = L"NppDockPackSplitter";
static const UINT kMsgSelfRepaint  = WM_APP + 0x61;
static const UINT kMsgSplitterMove = WM_APP + 0x62;
static const UINT kTimerAutoSync  = 1;    // 便笺防抖自动同步
static const UINT kTimerHeartbeat = 2;    // 每 N 秒自动确认一次连接
static const UINT kTimerConfirm   = 3;    // 被限流挡下的那次确认，晚点补跑
static const UINT kTimerBusyTick  = 4;    // 传输中刷新进度文字（兜底）
static const UINT kTimerRefreshShow = 5;  // 刷新前先把"连的是哪台服务器"亮一下（短暂）

enum {
    IDC_GEAR      = 100,   // ⚙ 设置（打开配置文件）
    IDC_STATUS    = 102,   // ● + 提示词（自绘：圆点与文字是**两条独立状态线**）
    IDC_SYNC      = 103,   // ↻ 刷新（统一入口：重读配置 + 确认连接 + 重拉列表）
    IDC_FILELIST  = 110,   // 左侧文件栏（owner-draw 列表框、支持多选）
    IDC_NOTES     = 111,   // 右侧便笺区（可编辑）
    IDC_SPLITTER  = 112,   // 中间分隔条（固定位置，只当分界线）
    // v1.6：文件操作按钮 —— 与"文件"有关的那一排（先选中再点按钮）
    // v1.8：全部改成**纯图标按钮**（王："都用图标显示"），等大、左对齐。
    //       顺序：文件属性 重命名 删除 上传 下载 打开文件位置
    //       （"返回上级"不再占按钮位 —— 它是文件栏里的 `..` 那一行）
    IDC_DOWNLOAD  = 121,   // 下载（落到本地「下载」文件夹）
    IDC_UPLOAD    = 122,   // 上传（选本地文件，传到当前目录）
    IDC_INFO      = 123,   // 文件属性（4 项）
    IDC_DELETE    = 124,   // 删除（**真删**，不进回收站、没有撤销）
    IDC_DLFOLDER  = 125,   // 打开文件位置（本地「下载」文件夹，尽量把文件选出来）
    IDC_RENAME    = 126,   // 重命名（原地改，不弹窗）
};

// 原地重命名用：EDIT 自己发的"收尾"信号。
// 为什么用 Post 而不是直接干：这两个动作要**销毁那个 EDIT**，
// 而在它自己的窗口过程里销毁自己是很不干净的（后面还有代码在跑）。

static const UINT kMsgRenameCommit = WM_APP + 0x63;   // 原地改名：提交
static const UINT kMsgRenameCancel = WM_APP + 0x64;   // 原地改名：放弃
// v1.9：**开始连接** —— 启动时不直接连，而是 Post 这条消息给自己。
//   原因见 wWinMain 末尾：连接要跑 3 次 ssh，在进消息循环之前一口气做完的话，
//   那几秒窗口是"画出来了但冻着"，看起来就是"打开很慢、还没反应"。
static const UINT kMsgStartConnect = WM_APP + 0x65;

// 行内右键菜单（王 v1.7：简洁 —— 不要"复制路径"，不要多余的分割线）
enum {
    kRowMenuOpen          = 1,   // 「打开」—— 只有**目录**才有（进入）
    kRowMenuDownload      = 2,   // 「下载」
    kRowMenuInfo          = 3,   // 「属性」（= 文件栏上方那个「文件属性」按钮，v1.9.2）
    kRowMenuRename        = 4,   // 「重命名」
    kRowMenuDelete        = 5,   // 「删除」
};
enum {
    kEmptyMenuUpload      = 11,  // 「上传」（v1.9.2 加的，跟按钮一个动作）
    kEmptyMenuRefresh     = 12,
    kEmptyMenuOpenDlDir   = 13,
};

// 便笺的保留名（v1.7 起不再叫「便笺.txt」）：
//   · 带前导点 —— 在服务器上天然是隐藏文件；
//   · 文件栏**显式过滤**掉它和回收站，用户看不见、也就删不到。
//   （之前它能在列表里被删，删完应用又自动重建一份同名空文件，
//    王看到的现象是"删了跟没删一样"。）
static const std::wstring kNoteName  = packcfg::NoteFileName();    // 便笺文件（相对背包根）
// v1.8：回收站机制**整个删掉了**（王："删了就删了，啥也没了"）。
// 这个常量只留着"不在文件栏里显示"这一条 —— 万一服务器上还留着
// v1.7 建的那个目录，也不该让它出现在列表里。
static const std::wstring kTrashName = packcfg::TrashDirName();

// 文件栏背景：比 BTNFACE(0xF0) 深 10 个灰度（王的要求，就深一丁点）
static const int kListBgLevel = 0xE6;

// ---------------------------------------------------------------------------
// 全局（单窗口程序）
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst = nullptr;
static HWND      g_hWnd  = nullptr;
static bool      g_embedded = false;
static UINT      g_dpi   = 96;

static HFONT g_hUiFont = nullptr, g_hNoteFont = nullptr, g_hSmallFont = nullptr;
static HFONT g_hListFont = nullptr;      // 文件栏：等宽（Consolas），王 v1.7 要求

static HWND g_hGear = nullptr, g_hStatus = nullptr;
static HWND g_hSync = nullptr, g_hList = nullptr, g_hNotes = nullptr, g_hSplit = nullptr;
// v1.8：文件操作按钮（一排 6 个，**等宽纯图标**）
static HWND g_hInfo = nullptr, g_hRename = nullptr, g_hDelete = nullptr,
            g_hUpload = nullptr, g_hDown = nullptr, g_hDlFolder = nullptr;

static packcfg::Config  g_cfg;
static packcore::Target g_target;

static std::vector<packcore::FileItem> g_files;
static std::wstring g_curDir;           // 文件栏当前浏览的相对目录（"" = 根，用 '/' 分隔）
// v1.8：列表第 0 行是「.. 」伪项（只有**不在根目录**时才出现）。
// 它不是 g_files 里的一项，所以任何"行号 ↔ 文件下标"的换算都要过这两个函数。
static bool   g_showUp    = false;
static int    g_sel       = -1;         // **焦点行**在 g_files 里的下标（-1 = 没选/没焦点）
static std::wstring g_openFile;         // 便笺区当前编辑的是哪个文件（相对背包根）
static bool   g_loading  = false;       // 正在程序性地改便笺内容（别当用户编辑）
static bool   g_dirty    = false;       // 用户改了还没同步
static bool   g_syncing  = false;
static bool   g_connected = false;
static std::wstring g_lastSync;         // "10:32:15"

static bool g_hasSel = false;           // 文件栏里有没有选中（给按钮的可用状态用）

// ---- v1.8：**原地重命名**（王："尽量不要弹窗，原地修改"）----
//   做法跟资源管理器一样：在那一行的名字位置上盖一个 EDIT，
//   回车 = 改，Esc = 放弃，"点到别处"（失去焦点）= 改。
//   为什么不用弹窗：改个名字要弹一个框、还要再点一次"确定"，太重了。
static HWND    g_hRenameEdit     = nullptr;   // 那个 EDIT（文件栏的子窗口）
static int     g_renameIdx   = -1;        // 正在改的是 g_files 里的第几项
static WNDPROC g_renameOrigProc = nullptr;

// ---- v1.6：连接状态（圆点）与提示词（文字）是**两条独立的状态线 ----
//      圆点只表达"链路怎么样"，文字只表达"现在在说什么事"。分开更新，互不覆盖。
enum class Conn { Unknown, Ok, Busy, Fail };
static Conn         g_conn = Conn::Unknown;
static std::wstring g_msg  = L"未连接";
static COLORREF ConnColor(Conn c)
{
    switch (c) {
    case Conn::Ok:   return RGB(0x2E, 0xA0, 0x43);   // 绿：连着
    case Conn::Busy: return RGB(0xC8, 0x8A, 0x00);   // 黄：正在忙
    case Conn::Fail: return RGB(0xC0, 0x30, 0x30);   // 红：连不上
    default:         return RGB(0x9E, 0x9E, 0x9E);   // 灰：还不知道
    }
}

// ---- v1.6：一次文件传输期间的独占状态 ----
static bool         g_busy = false;         // 有传输在跑 → 期间不允许别的文件操作
static int          g_busyBtn = 0;          // 正在跑的那个按钮 id（它会**变身成「取消」**）
// v2.0：批次。多文件传输时，**文件与文件之间的那道缝**也不能松手 ——
// 以前每个文件 BeginBusy/EndBusy 成对，缝里 g_busy 会瞬间变回 false，
// 按钮跟着复活，手快就能点出第二个传输（并套出第二条 ssh）。
static int          g_batchDepth = 0;       // >0 = 一个多文件批次正在进行
static int          g_batchBtn   = 0;       // 这个批次的按钮 id（缝里由它继续扮演「取消」）
static bool         g_cancelReq = false;    // 「取消」被按下了
static int          g_progressPct = -1;     // -1 = 无进度条；0..100 = 有
static long long    g_progressDone = 0, g_progressTotal = 0;
static std::wstring g_busyText;             // 正在干什么（进度回调拿它拼文字）
static bool         g_closePending = false; // 传输中点了关闭：取消完再关

// ---- v1.6：连接确认的限流（"IO 总速率"）----
static DWORD g_lastConfirmTick = 0;
static bool  g_confirming = false;
static bool  g_confirmPending = false;
static bool  g_confirmWantReload = false;   // 待补跑的那次要不要重读配置
static bool  g_confirmWantFull = false;     // 待补跑的那次要不要重拉列表
static bool  g_confirmWantUser = false;     // 待补跑的那次是不是"用户点刷新"触发的
static DWORD g_lastLatencyMs = 0;           // v1.9：上一次连接确认（Probe）花了多少毫秒

// ---- v1.9：传输速率的滑动窗口（"最近一秒内的速率"）----
//   为什么用窗口而不是"上一个回调到这一个回调"：回调是 150ms 一次，
//   单次间隔的抖动会被放大成十倍的速率跳动，看着像乱码。
//   窗口取 ~1 秒：既有"当前速度"的即时感，又不会一跳一跳。
static const int kRateWin = 12;             // 12 × 150ms ≈ 1.8s 的容量，装得下 1 秒窗口
struct RateSample { DWORD tick; long long done; };
static RateSample g_rate[kRateWin];
static int        g_rateN = 0;
static double     g_rateBps = 0.0;          // 字节/秒

static COLORREF g_noteBg = RGB(0xFF, 0xFF, 0xFF);
static COLORREF g_noteFg = RGB(0x20, 0x20, 0x20);
static HBRUSH   g_hNoteBrush = nullptr;
static HBRUSH   g_hListBrush = nullptr;

static int    g_hoverRow = -1;          // 鼠标悬停的行（只用来给行提亮，可选）

static const double kSplitRatio = 0.38; // 左 38% / 右 62%（**固定**，见 LayoutChildren）

static WNDPROC g_listOrigProc   = nullptr;
static std::wstring g_tracePath;

// 前向声明
static void LayoutChildren(int W, int H);
static void ApplyFonts();
static void ReloadConfig();
static void RefreshFileList();
static bool LoadNoteFile(const std::wstring& relName, bool createIfMissing);
static void DoSync(bool manual);
static std::wstring JoinRel(const std::wstring& dir, const std::wstring& name);
static void SetConnState(Conn c);
static void SetMessage(const std::wstring& text);
static void InvalidateStatus();
static void RequestConfirm(bool reloadCfg, bool full = false, bool user = false);
static void RequestRefresh(bool user = false);      // v1.7：统一的"刷新"
static void ApplyFileList(const std::vector<packcore::FileItem>& all);   // v2.0：铺列表
static void ApplyNoteData(const std::string& data);                      // v2.0：铺便笺
static void UpdateButtonsEnabled();
static void RepaintStatusArea();
static void BeginBusy(int btnId, const std::wstring& text);
static void EndBusy();
static void BeginBatch(int btnId);      // v2.0：多文件批次的"整段忙碌"（见定义处）
static void EndBatch();
static bool OnProgress(long long done, long long total, void* user);
static std::wstring JoinPathNative(const std::wstring& dir, const std::wstring& name);
static void OpenFolderInExplorer(const std::wstring& dir, const std::wstring& selectFile);
static void OpenLocalDownloadFolder();
static void StartDownload();
static void StartUpload();
static void DeleteSelected();                       // 删除选中的（**真删**）
static std::vector<int> SelectedIdx();              // 文件栏里选中的**文件**项（按行序升序）
static void StartRename(int fileIdx);               // 原地重命名（盖一个 EDIT 在那一行上）
static void EndRename(bool commit);                 // 收掉那个 EDIT
static std::wstring ServerLabel();                  // 刷新时短暂显示的"服务器域名/IP"
static void SyncBtnCaptions();                      // 把按钮标签同步到窗口标题（探针可读）

static int Dp(int v) { return appui::Dp(v); }

// ---------------------------------------------------------------------------
// 轨迹日志：应用"起不来 / 没反应"的时候，只有它能把话说清楚
// ---------------------------------------------------------------------------
static void TraceLog(const char* line)
{
    if (g_tracePath.empty()) {
        wchar_t p[MAX_PATH * 2]{};
        ::GetModuleFileNameW(nullptr, p, _countof(p));
        std::wstring s(p);
        const size_t k = s.find_last_of(L'.');
        g_tracePath = (k == std::wstring::npos ? s : s.substr(0, k)) + L".trace.log";

        // v2.0：轨迹日志也要滚。放在"算路径"这里就天然只跑一次 ——
        // app 是随标签页生灭的短命进程，启动时查一次足够。
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(g_tracePath.c_str(), GetFileExInfoStandard, &fad)) {
            const unsigned long long sz =
                ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (sz >= 1024ull * 1024ull) {
                const std::wstring old = g_tracePath + L".old";
                ::DeleteFileW(old.c_str());
                ::MoveFileW(g_tracePath.c_str(), old.c_str());
            }
        }
    }
    HANDLE h = ::CreateFileW(g_tracePath.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char buf[512];
    sprintf_s(buf, "[%02d:%02d:%02d.%03d] %s\r\n", st.wHour, st.wMinute,
              st.wSecond, st.wMilliseconds, line);
    DWORD wrote = 0;
    ::WriteFile(h, buf, (DWORD)strlen(buf), &wrote, nullptr);
    ::CloseHandle(h);
}

static void TraceLogW(const char* tag, const std::wstring& s)
{
    char u8[1024]{};
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, u8, sizeof(u8) - 1, nullptr, nullptr);
    char buf[1200];
    sprintf_s(buf, "%s %s", tag, u8);
    TraceLog(buf);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    char buf[256];
    sprintf_s(buf, "!!! 崩了：异常码 0x%08X，地址 %p",
              ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0u,
              ep && ep->ExceptionRecord ? (void*)ep->ExceptionRecord->ExceptionAddress
                                        : nullptr);
    TraceLog(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static std::string U8FromW(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                  nullptr, nullptr);
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring WFromU8(const std::string& s)
{
    if (s.empty()) return std::wstring();
    size_t off = 0;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) off = 3;
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str() + off,
                                  (int)(s.size() - off), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str() + off, (int)(s.size() - off), &w[0], n);
    return w;
}

static std::wstring GetText(HWND h, int maxLen = 4096)
{
    if (!h) return L"";
    const int n = ::GetWindowTextLengthW(h);
    const int cap = (std::min)((std::max)(n + 1, 8), maxLen);
    std::wstring s((size_t)cap, L'\0');
    const int got = ::GetWindowTextW(h, &s[0], cap);
    s.resize((size_t)(got > 0 ? got : 0));
    return s;
}

static void RepaintSubtree(HWND hwnd)
{
    // R6：只作废，**不要** RDW_UPDATENOW（跨进程同步重画 = 死锁源）
    ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
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

// ⚠️ UIPI：Notepad++ 常以管理员身份跑（High），资源管理器是 Medium。
//   低完整性发给高完整性窗口的 WM_DROPFILES 会被**静默丢弃** ——
//   表现就是"能拖、光标也对，但一点反应都没有"（文件校验那边就是这么栽的）。
//   所以文件栏（以及主窗口）都要显式放行这三条消息。
static void AllowDropMessages(HWND h)
{
    typedef BOOL (WINAPI* PFN)(HWND, UINT, DWORD, void*);
    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    if (!u) return;
    auto f = (PFN)::GetProcAddress(u, "ChangeWindowMessageFilterEx");
    if (!f) return;
    const DWORD kAllow = 1;
    f(h, WM_DROPFILES, kAllow, nullptr);
    f(h, WM_COPYDATA,  kAllow, nullptr);
    f(h, 0x0049u /*WM_COPYGLOBALDATA*/, kAllow, nullptr);
    { char b[160];
      sprintf_s(b, "拖放：窗口 %p 已放行 UIPI（ChangeWindowMessageFilterEx 可用）", (void*)h);
      TraceLog(b); }
}

static std::wstring JoinRel(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    return dir + L"/" + name;
}

static std::wstring ParentOf(const std::wstring& dir)
{
    const size_t k = dir.find_last_of(L'/');
    if (k == std::wstring::npos) return std::wstring();
    return dir.substr(0, k);
}

static std::wstring FmtSize(unsigned long long n)
{
    wchar_t b[32];
    if (n < 1024)                 swprintf_s(b, L"%llu B", n);
    else if (n < 1024ull * 1024)   swprintf_s(b, L"%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024) swprintf_s(b, L"%.1f MB", n / 1048576.0);
    else                           swprintf_s(b, L"%.2f GB", n / 1073741824.0);
    return b;
}

// 时间戳 -> "MM-DD HH:MM"（拿不到就给空）
static std::wstring FmtTime(long long unixSec)
{
    if (unixSec <= 0) return std::wstring();
    const time_t t = (time_t)unixSec;
    tm lt{};
    if (localtime_s(&lt, &t) != 0) return std::wstring();
    wchar_t b[32];
    swprintf_s(b, L"%02d-%02d %02d:%02d",
               lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return b;
}

// ---- v1.9：传输速率 与 剩余时间 ----

// 速率：直接复用 FmtSize 的数字格式，尾巴加 "/s"（1.2 MB/s）
static std::wstring FmtRate(double bps)
{
    if (bps < 1.0) return std::wstring();
    return FmtSize((unsigned long long)(bps + 0.5)) + L"/s";
}

// 剩余时间：自动换算到 h / m / s（"1h2m3s" / "2m3s" / "45s"）
static std::wstring FmtEta(long long sec)
{
    if (sec < 0) return std::wstring();
    if (sec < 60) return std::to_wstring(sec) + L"s";
    const long long h = sec / 3600;
    const long long m = (sec % 3600) / 60;
    const long long s = sec % 60;
    std::wstring o;
    if (h > 0) o += std::to_wstring(h) + L"h";
    if (h > 0 || m > 0) o += std::to_wstring(m) + L"m";
    o += std::to_wstring(s) + L"s";
    return o;
}

static void RateReset()
{
    g_rateN = 0;
    g_rateBps = 0.0;
}

// 喂一个进度采样点：只保留"最近约 1 秒"的样本，用窗口两端的差算速率。
static void RatePush(long long done)
{
    const DWORD now = ::GetTickCount();

    if (g_rateN >= kRateWin) {                  // 窗口满了：丢掉最老的
        memmove(g_rate, g_rate + 1, sizeof(g_rate[0]) * (kRateWin - 1));
        g_rateN = kRateWin - 1;
    }
    g_rate[g_rateN].tick = now;
    g_rate[g_rateN].done = done;
    ++g_rateN;

    int first = 0;                              // 掐掉过期样本（> 1 秒）
    while (first < g_rateN - 1 && (now - g_rate[first].tick) > 1000) ++first;
    if (first > 0) {
        memmove(g_rate, g_rate + first, sizeof(g_rate[0]) * (g_rateN - first));
        g_rateN -= first;
    }

    if (g_rateN >= 2) {
        const DWORD dt = g_rate[g_rateN - 1].tick - g_rate[0].tick;
        const long long db = g_rate[g_rateN - 1].done - g_rate[0].done;
        if (dt > 0 && db >= 0) g_rateBps = (double)db * 1000.0 / (double)dt;
    }
}

// ---------------------------------------------------------------------------
// 状态 / 字体
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 状态：圆点与文字**分开**
//
// 为什么要拆开（王 v1.6 明确要求）：圆点回答"链路通不通"，文字回答
// "现在在干什么"。合成一个 SetStatus(connected, text, color) 的后果是
// 每换一句提示词就得顺带猜一次连接状态，写着写着两者必然会互相打架。
// ---------------------------------------------------------------------------
static void InvalidateStatus()
{
    if (g_hStatus && ::IsWindow(g_hStatus)) ::InvalidateRect(g_hStatus, nullptr, TRUE);
}

static void SetConnState(Conn c)
{
    if (g_conn == c) return;
    g_conn = c;
    InvalidateStatus();
}

static void SetMessage(const std::wstring& text)
{
    g_msg = text;
    // 画面上的圆点与文字都是自绘的，但**窗口标题也同步写一份** ——
    // 否则自动化（探针）读不到"现在到底是连上还是没连上"。
    if (g_hStatus && ::IsWindow(g_hStatus)) ::SetWindowTextW(g_hStatus, text.c_str());
    InvalidateStatus();
}

static void RepaintStatusArea()
{
    InvalidateStatus();
    if (g_hWnd) RepaintSubtree(g_hWnd);
}

static void ApplyFonts()
{
    HFONT ui = appui::MakeUiFont();
    if (ui) { HFONT old = g_hUiFont; g_hUiFont = ui; if (old) ::DeleteObject(old); }

    // 顶部状态行：比界面小一号、**不加粗**（王明确提过）
    // ⚠️ 变量千万别叫 small —— Windows 头里有 `#define small char`，
    //    写出来会被静默替换成 char，报的错是"HFONT 后面跟 char 非法"，很费解。
    // v1.8：状态行也换成等宽 —— 王的要求是"总之就是所有字体都改成 console"。
    HFONT smallFont = appui::MakeFont(L"Consolas",
                                      (std::max)(6, appui::UiFontPt() * 8 / 9),
                                      /*mono*/ true, false);
    if (smallFont) { HFONT old = g_hSmallFont; g_hSmallFont = smallFont; if (old) ::DeleteObject(old); }

    // 便笺：等宽（Console 类），字号来自配置文件；0 = 跟界面
    HFONT note = appui::MakeFont(g_cfg.noteFont.c_str(), g_cfg.noteFontPt, true, false);
    if (note) { HFONT old = g_hNoteFont; g_hNoteFont = note; if (old) ::DeleteObject(old); }

    // 文件栏：同样是等宽（王 v1.7 要求"用 console 字体"）——
    // 尺寸跟界面字号走（列表要能一眼扫，不适合跟着便笺的 8 点）。
    HFONT listF = appui::MakeFont(L"Consolas", 0, true, false);
    if (listF) { HFONT old = g_hListFont; g_hListFont = listF; if (old) ::DeleteObject(old); }

    HWND uiKids[] = { g_hGear, g_hSync, g_hDlFolder,
                      g_hInfo, g_hRename, g_hDelete, g_hUpload, g_hDown };
    for (HWND h : uiKids)
        if (h && ::IsWindow(h)) ::SendMessageW(h, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
    if (g_hStatus && ::IsWindow(g_hStatus))
        ::SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hSmallFont, TRUE);
    if (g_hNotes && ::IsWindow(g_hNotes))
        ::SendMessageW(g_hNotes, WM_SETFONT, (WPARAM)g_hNoteFont, TRUE);
    if (g_hList && ::IsWindow(g_hList))
        ::SendMessageW(g_hList, WM_SETFONT, (WPARAM)(g_hListFont ? g_hListFont : g_hUiFont), TRUE);

    // 行高跟着字体走（owner-draw 的行高要自己设）
    if (g_hList && ::IsWindow(g_hList)) {
        int rh = appui::FontCellHeight(g_hListFont ? g_hListFont : g_hUiFont) + Dp(8);
        if (rh < Dp(20)) rh = Dp(20);
        ::SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, (LPARAM)rh);
    }
}

static void RebuildBrushes()
{
    unsigned long v = 0;
    if (packcfg::ParseColor(g_cfg.noteBg, v)) g_noteBg = (COLORREF)v;
    if (packcfg::ParseColor(g_cfg.noteFg, v)) g_noteFg = (COLORREF)v;

    if (g_hNoteBrush) { ::DeleteObject(g_hNoteBrush); g_hNoteBrush = nullptr; }
    g_hNoteBrush = ::CreateSolidBrush(g_noteBg);
    if (!g_hListBrush) g_hListBrush = ::CreateSolidBrush(RGB(kListBgLevel, kListBgLevel, kListBgLevel));
}

// ---------------------------------------------------------------------------
// 配置 / 连接
// ---------------------------------------------------------------------------
static void BuildTarget()
{
    g_target = packcore::Target();
    g_target.local = packcfg::IsLocal(g_cfg);
    if (g_target.local) {
        g_target.localRoot = packcfg::LocalRoot(g_cfg);
    } else {
        g_target.host     = g_cfg.host;
        g_target.port     = g_cfg.port;
        g_target.user     = g_cfg.username;
        g_target.authType = g_cfg.authType;
        g_target.password = g_cfg.password;
        g_target.keyPath  = g_cfg.privateKeyPath;
        g_target.folder   = packcfg::EffectiveRemoteFolder(g_cfg);
    }
}

static void ReloadConfig()
{
    bool created = false;
    packcfg::Load(g_cfg, &created);
    BuildTarget();
    RebuildBrushes();
    ApplyFonts();                       // 便笺字体/配色都来自配置
    if (g_hNotes && ::IsWindow(g_hNotes)) ::InvalidateRect(g_hNotes, nullptr, TRUE);
    if (created) TraceLog("配置文件已生成（默认值）");
}

// ---------------------------------------------------------------------------
// 连接确认 / 刷新
//
// v1.7 时立过一条"刷新只有一套动作"的规矩；v1.9.1 王把它**拆成了两条独立的线**：
//
//   便笺那一路   = 把便笺写下去 + 探一次连接          （不碰文件栏）
//   文件栏那一路 = 重拉文件列表 + 探一次连接          （不碰便笺）
//   用户「刷新」 = 上面两条的**结合体**（先存便笺，再重读配置 + 确认 + 重拉列表）
//   定时心跳     = 文件栏那一路
//
// 为什么改：他观察到"删一个文件，文件栏刷了两次"（删除里先拉一次、末尾 full refresh
// 又拉一次），而且"我只打了个字，文件栏却跟着刷"。两件事本来就独立 —— 便笺是
// "我在写的内容"，文件栏是"服务器那个目录现在长什么样"。
//
// 入口那道**限流闸门**照旧：最快 io_min_interval_ms（默认 1 秒）跑一次；
// 被挡下的请求**不丢**，合并成"待会儿补跑一次" —— 既不会把 IO 打满，
// 又不会因为"刚才那次正好在闸门内"而永远漏掉一次。
// ---------------------------------------------------------------------------
static void DoConfirm(bool reloadCfg, bool full, bool user)
{
    // ⚠️ v1.9：等 ssh 的那几秒里我们会**抽消息泵**（见 packcore::SetPump），
    //   于是消息可以在这段代码还在跑的时候被派发进来 —— 包括"补跑确认"的定时器。
    //   所以入口必须先自己挡住重入，只把请求记成待办
    //   （RequestConfirm 那边已经挂好了补跑定时器，所以不会丢）。
    if (g_confirming) {
        g_confirmPending  = true;
        g_confirmWantReload |= reloadCfg;
        g_confirmWantFull   |= full;
        g_confirmWantUser   |= user;
        return;
    }
    if (g_busy) {                        // 传输中不插队：等它结束再说
        g_confirmPending  = true;
        g_confirmWantReload |= reloadCfg;
        g_confirmWantFull   |= full;
        g_confirmWantUser   |= user;
        return;
    }

    g_confirming = true;
    g_confirmPending = false;
    const bool wantReload = reloadCfg || g_confirmWantReload;
    const bool wantFull   = full      || g_confirmWantFull;
    const bool wantUser   = user      || g_confirmWantUser;
    g_confirmWantReload = g_confirmWantFull = g_confirmWantUser = false;

    if (wantReload) {
        ReloadConfig();                  // 把配置重读一遍（改完配置点刷新即生效）
        // 心跳间隔也可能被改过 → 重新挂一次定时器
        if (g_hWnd && ::IsWindow(g_hWnd)) {
            ::KillTimer(g_hWnd, kTimerHeartbeat);
            if (g_cfg.heartbeatSec > 0)
                ::SetTimer(g_hWnd, kTimerHeartbeat,
                           (UINT)g_cfg.heartbeatSec * 1000u, nullptr);
        }
        TraceLog("确认：重读配置文件");
    }

    // ★ v1.9：把"正在连接"**先写出来**再动手。连接要跑 ssh，
    //   以前这几秒界面是冻着的，看起来就像"打开很慢、还没反应"；
    //   现在等待期间会抽消息泵，所以这句话是真的能被看见的。
    const bool wasConnected = g_connected;
    // ★ v1.9：把"正在连接"写出来 —— 但**只在真的会让人看见的时候**：
    //   · 还没连上（首次连接 / 断线之后重连）：用户必须知道正在跑连接；
    //   · 是他亲手点的刷新：他会看到「正在连接 → 已刷新 · 延迟」这一对。
    //   后台的确认（心跳 / 便笺写完 / 文件操作后）**一个字都不改** ——
    //   否则会把你刚看到的那句「已连接 · 12:00:00 同步」闪掉；而且后台那句
    //   是**不会恢复**的，提示词会永久停在"正在连接…"（探针抓到过）。
    if (!wasConnected || wantUser) {
        const std::wstring srv = ServerLabel();
        SetMessage(srv.empty() ? L"正在连接…" : (L"正在连接 " + srv + L" …"));
        RepaintStatusArea();
    }
    // 按钮禁用跟"消息改不改"是两件事：**任何**确认期间都要真灰掉。
    //（消息泵会把点击派发进来，而"禁用"只在 UpdateButtonsEnabled 里做。）
    UpdateButtonsEnabled();

    const DWORD t0 = ::GetTickCount();
    // ★ v2.0：**一次 ssh 把"探测 + 读便笺 + 列目录"都办完**。
    //   老路子是三个 ssh 进程 —— 每个都要重新握手认证一遍，这是这个应用最大的
    //   延迟来源。Windows 自带的 ssh 不支持 ControlMaster（连接复用），所以只能合并。
    //   会话不可用时（对面 shell 不认这套哨兵写法）**自动退回**原来的三次调用。
    //
    //   会话要列哪个目录：首次连接反正会回根目录，所以那里用根；
    //   其它情况用当前目录（否则带回来的列表是错的）。
    const std::wstring sessDir = wasConnected ? g_curDir : std::wstring();
    packcore::SessionOut so;
    packcore::Res r = packcore::Session(g_target, sessDir, kNoteName, so);
    if (!so.usable) {
        TraceLog("会话不可用（对面 shell 不认哨兵），退回三次独立调用");
        so.noteOk = so.listOk = false;
        r = packcore::Probe(g_target);                // ← 老路
    }
    // ⚠️ 这个数字的含意变了：以前是"跑一次 Probe 的毫秒数"，现在是一次 ssh
    //    把三件事办完的总耗时（合并之后本来也分不开了）。
    g_lastLatencyMs = ::GetTickCount() - t0;
    g_lastConfirmTick = ::GetTickCount();
    g_confirming = false;

    if (packcore::Ok(r)) {
        if (!wasConnected) {
            // 刚连上：便笺和列表**已经在这次会话里拿到了**（v2.0），
            // 只有会话没带到时才各自再补一次 ssh。
            g_curDir.clear();
            g_openFile.clear();
            g_connected = true;

            bool noteDone = false;
            if (so.noteOk && (!g_dirty || GetText(g_hNotes, 1 << 20).empty())) {
                // ⚠️ 有没保存的内容时绝不覆盖（同 LoadNoteFile 的那条铁律）——
                //    这种情况下让 LoadNoteFile 去走它自己那套"保住用户输入"的逻辑。
                ApplyNoteData(so.note);
                g_openFile = kNoteName;
                noteDone = true;
            }
            if (!noteDone) {
                SetMessage(L"已连接，正在读取便笺…");
                RepaintStatusArea();
                if (!LoadNoteFile(kNoteName, /*createIfMissing*/ true)) {
                    SetConnState(Conn::Fail);
                    SetMessage(L"连上了，但便笺文件读写不了");
                    TraceLog("便笺文件读写不了");
                    UpdateButtonsEnabled();
                    return;
                }
            }
            if (so.listOk) {
                ApplyFileList(so.files);
            } else {
                SetMessage(L"正在读取文件列表…");
                RepaintStatusArea();
                RefreshFileList();
            }
            SetConnState(Conn::Ok);
            {
                wchar_t b[96];
                swprintf_s(b, L"已连接 · 就绪 · 延迟 %lu ms",
                           (unsigned long)g_lastLatencyMs);
                SetMessage(b);
            }
        } else {
            SetConnState(Conn::Ok);
            if (wantFull) {
                // v2.0：列表多半就在这次会话里带回来了，省掉一次 ssh
                if (so.listOk) ApplyFileList(so.files);
                else           RefreshFileList();
                // ⚠️ 只有**用户自己点的刷新**才把提示词改掉。
                //   后台的（心跳 / 便笺同步完 / 文件操作后）留着原有提示词 ——
                //   否则"已连接 · 12:00:00 同步"这种刚写上去的话会被一秒后的
                //   后台刷新无声盖成"已刷新"（探针就抓到过这个）。
                //   ★ v1.9：用户点刷新时顺手把**连接延迟**报出来（王的要求）。
                if (wantUser) {
                    wchar_t b[96];
                    swprintf_s(b, L"已刷新 · 延迟 %lu ms",
                               (unsigned long)g_lastLatencyMs);
                    SetMessage(b);
                }
            }
            else if (g_msg.empty() || g_msg.rfind(L"未连接", 0) == 0)
                SetMessage(L"已连接 · 就绪");   // 只有"之前那句是没连上"时才改文案
        }
        TraceLog("确认：连接正常");
    } else {
        g_connected = false;
        SetConnState(Conn::Fail);
        SetMessage(L"未连接：" + r.msg);
        if (g_hList) ::SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);
        g_files.clear();
        g_sel = -1;
        g_openFile.clear();

        // 便笺区也清空（王的要求：连不上就**两侧都空白**）。
        // ⚠️ 但只在**没有未保存改动**时才清 —— 用户敲了一半的字是这里
        //    唯一真正会丢的东西，绝不为了"显示空白"把它抹掉。
        if (g_hNotes && !g_dirty) {
            g_loading = true;
            ::SetWindowTextW(g_hNotes, L"");
            g_loading = false;
            ::InvalidateRect(g_hNotes, nullptr, TRUE);
        }
        TraceLog("确认：连接失败");
    }
    UpdateButtonsEnabled();
}

static void RequestConfirm(bool reloadCfg, bool full, bool user)
{
    if (g_busy) {
        g_confirmPending  = true;
        g_confirmWantReload |= reloadCfg;
        g_confirmWantFull   |= full;
        g_confirmWantUser   |= user;
        return;
    }
    const DWORD now    = ::GetTickCount();
    const DWORD minGap = (DWORD)(g_cfg.ioMinIntervalMs > 0 ? g_cfg.ioMinIntervalMs : 1000);
    const DWORD since  = now - g_lastConfirmTick;

    if (g_confirming || since < minGap) {
        g_confirmPending  = true;                       // 合并，不丢
        g_confirmWantReload |= reloadCfg;
        g_confirmWantFull   |= full;
        g_confirmWantUser   |= user;
        if (!g_confirming) ::SetTimer(g_hWnd, kTimerConfirm, minGap - since, nullptr);
        return;
    }
    DoConfirm(reloadCfg, full, user);
}

// 旧调用点（启动时）用的薄封装
static bool Connect(std::wstring* why = nullptr)
{
    DoConfirm(false, true, false);
    if (why) *why = g_connected ? std::wstring() : g_msg;
    return g_connected;
}

static void SayBusy()
{
    SetMessage(L"还没连上：点右上角 ⚙ 打开配置文件填服务器信息，再点 ↻ 刷新");
}

// ★ v1.7：**唯一**的"刷新"入口。
//   王的要求是"刷新功能要统一"：便笺同步后、文件栏右键刷新、右上角刷新按钮、
//   定时心跳，这四处**必须**是同一套动作 —— 现在都调这里：
//   重读配置文件 → 探一次连接 → 重拉文件列表 + 回收站状态。
static void RequestRefresh(bool user)
{
    RequestConfirm(/*reloadCfg*/ true, /*full*/ true, /*user*/ user);
}

// ---------------------------------------------------------------------------
// ★ v1.9.1：把"便笺"和"文件栏"当成**两条独立的刷新线**（王定的）
//
//   · 便笺写完（自动同步）：只把便笺存下去 + 确认一次连接，**不碰文件栏**；
//   · 文件操作（删除/改名/上传/下载/拖入）：只刷**文件栏** + 确认一次连接；
//   · 「刷新」按钮 / 右键「刷新」：是上面两者的**结合体** —— 先把便笺存下去，
//     再重读配置 + 确认连接 + 重拉文件栏；
//   · 定时心跳：等同"文件栏那一路"（王要的"定时自动刷新"刷的就是列表）。
//
//   为什么要把它们拆开：王观察到"删一个文件，文件栏刷了两次" ——
//   因为删除里先 `RefreshFileList()`，末尾又走了一次 full refresh
//   （而它内部还会再拉一遍）。拆开之后每一边只做自己那一件事，
//   既不会重复、语义也清楚：**"我在写便笺"和"目录变了"本来就是两件事**。
// ---------------------------------------------------------------------------
static void ConfirmOnly()
{
    RequestConfirm(/*reloadCfg*/ false, /*full*/ false, /*user*/ false);
}

// 文件操作之后的收尾：文件栏**只拉一次** + 确认一次连接
static void RefreshListAfterFileOp()
{
    RefreshFileList();
    ConfirmOnly();
}

// 用户主动的「刷新」（按钮 / 右键菜单）= 便笺 + 文件栏的**结合体**
static void UserRefresh()
{
    // 便笺那一路：只在**已经连上**且确实有没存的改动时才写。
    // （没连上时别在这儿先把连接跑一遍 —— 下面那个 RequestRefresh 就是干这个的。）
    if (g_dirty && g_connected) DoSync(/*manual*/ false);
    RequestRefresh(/*user*/ true);      // 文件栏那一路：重读配置 + 确认 + 重拉列表
}

// 刷新时短暂显示"连的是哪台服务器"（王的要求：优先域名）。
// 配置里 host 填的就是域名或 ip —— 直接亮它。不必反查 DNS：
// 显示一个"解析出来的 ip"反而和用户在配置里看到的对不上。
static std::wstring ServerLabel()
{
    if (g_target.local) return g_target.localRoot;
    return g_cfg.host;
}

// 列表行号 ↔ g_files 下标的换算（第 0 行可能是 `..` 伪项）
static int ListRowOf(int fileIdx) { return fileIdx + (g_showUp ? 1 : 0); }

// 把**已经拿到的**目录内容铺到文件栏上。
// v2.0 从 RefreshFileList 里拆出来：一次 ssh 的会话（packcore::Session）
// 也能顺手把列表带回来，没必要为此再跑一次 ls。
static void ApplyFileList(const std::vector<packcore::FileItem>& all)
{
    g_files.clear();
    ::SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);

    for (const auto& f : all) {
        // 便笺（新名/旧名）和旧回收站目录不进列表 —— 它们不是"背包里的东西"，
        // 显示出来只会让人误删、误以为"删了跟没删一样"。
        if (packcfg::IsReservedName(f.name)) continue;
        // v2.0：写一半残留的临时文件也不显示（正常路径几乎不会出现，
        //   但"上传到一半断网"就会留一个 —— 让它出现在列表里只会让人手忙脚乱）。
        if (packcore::IsTempName(f.name)) continue;
        g_files.push_back(f);
    }

    // ★ v1.8：**「..」是文件栏里的一个"文件夹"**（王的要求），只有不在根目录时才有。
    g_showUp = !g_curDir.empty();
    if (g_showUp) ::SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)L"..");

    // 列表项的文字就是文件名 —— 给 LBS_HASSTRINGS 用（owner-draw 不影响这条）。
    for (const auto& f : g_files)
        ::SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)f.name.c_str());

    g_sel = -1;
    ::SendMessageW(g_hList, LB_SETSEL, (WPARAM)FALSE, (LPARAM)-1);  // 清空全部选中
    ::SendMessageW(g_hList, LB_SETCURSEL, (WPARAM)-1, 0);
    ::InvalidateRect(g_hList, nullptr, TRUE);
    // 记一笔：这是**唯一**的"文件栏真的重拉了一次"的痕迹。
    TraceLogW("刷新文件栏：", std::to_wstring(g_files.size()) + L" 项");
    UpdateButtonsEnabled();
}

// 文件栏内容 = 「..」（不在根目录时）+ 当前目录的项
// （便笺与 v1.7 留下的回收站目录**一律不显示**）
static void RefreshFileList()
{
    if (!g_hList) return;

    // 重拉列表时若有没改完的名字，先收掉（不提交）—— 否则那个 EDIT 会
    // 悬在旧行号上，看起来像"改到一半自己跑了"。
    if (g_hRenameEdit) EndRename(false);

    g_files.clear();
    ::SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);

    std::vector<packcore::FileItem> all;
    packcore::Res r = packcore::List(g_target, g_curDir, all);
    if (!packcore::Ok(r)) {
        TraceLogW("列目录失败：", r.msg);
        g_showUp = !g_curDir.empty();
        g_sel = -1;
        ::InvalidateRect(g_hList, nullptr, TRUE);
        UpdateButtonsEnabled();
        return;
    }
    ApplyFileList(all);
}

// 把某个文件读进便笺区。createIfMissing：服务器上没有就建一个空的。
// 把**已经拿到的**便笺内容铺进编辑器。
// v2.0 从 LoadNoteFile 的尾巴拆出来：一次 ssh 的会话也会把便笺带回来。
static void ApplyNoteData(const std::string& data)
{
    g_loading = true;
    if (g_hNotes) ::SetWindowTextW(g_hNotes, WFromU8(data).c_str());
    g_loading = false;
    g_dirty = false;
    if (g_hNotes) ::InvalidateRect(g_hNotes, nullptr, TRUE);
}

static bool LoadNoteFile(const std::wstring& relName, bool createIfMissing)
{
    // ⚠️⚠️ 有**没保存的内容**时，绝不 SetWindowText 盖掉它。
    //
    //   场景：用户点了刷新（连接要几秒），等连接回来时他已经在打字了。
    //   这里如果照常"载入文件内容"，那几秒的字就被无声抹掉 —— 而便笺区
    //   是这个地方**唯一真正会丢**的东西（实测踩到过：探针打完字，
    //   连接一回来文件变成空的）。
    //
    //   处理：把当前编辑的内容当作"要打开的这份文件的内容"，
    //   记下文件名、保持 dirty，让它照常走 1.5 秒后的自动同步写出去。
    //   （上一版只写"有 g_openFile 就先落盘"，但连不上之后 g_openFile 是空的，
    //     照样会被盖 —— 所以判据必须是"编辑器里有没有东西"，不是"有没有文件名"。）
    if (g_dirty) {
        const std::wstring keep = GetText(g_hNotes, 1 << 20);
        if (!keep.empty()) {
            if (!g_openFile.empty()) {
                packcore::Write(g_target, g_openFile, U8FromW(keep));
            }
            g_openFile = relName;     // 就当成是在编辑这份
            return true;              // 内容留在编辑器里，等自动同步
        }
    }

    std::string data;
    packcore::Res r = packcore::Read(g_target, relName, data);
    if (r.st == packcore::St::NotFound) {
        if (!createIfMissing) return false;

        bool migrated = false;
        // ★ v1.7：便笺改了保留名。服务器上如果还留着旧的「便笺.txt」，
        //   把它**改名**过来 —— 王写下的东西一个字都不能丢。
        if (_wcsicmp(relName.c_str(), kNoteName.c_str()) == 0) {
            const std::wstring oldName = packcfg::LegacyNoteFileName();
            std::string old;
            if (packcore::Ok(packcore::Read(g_target, oldName, old))) {
                if (packcore::Ok(packcore::Write(g_target, relName, old))) {
                    packcore::Remove(g_target, oldName, false);
                    data = old;
                    migrated = true;
                    TraceLogW("便笺已从旧名字迁移过来：", oldName);
                }
            }
        }
        if (!migrated) {
            packcore::Res w = packcore::Write(g_target, relName, std::string());
            if (!packcore::Ok(w)) return false;
            data.clear();
        }
    } else if (!packcore::Ok(r)) {
        return false;
    }

    g_openFile = relName;
    ApplyNoteData(data);
    return true;
}

// ---------------------------------------------------------------------------
// 同步
// ---------------------------------------------------------------------------
static std::wstring NowHms()
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t b[24];
    swprintf_s(b, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return b;
}

static void DoSync(bool manual)
{
    if (g_syncing) return;

    if (!g_connected) {
        if (!Connect()) { if (manual) SayBusy(); return; }
    }
    if (g_openFile.empty()) {
        SetMessage(L"还没有打开便笺文件");
        return;
    }

    g_syncing = true;
    SetConnState(Conn::Busy);
    SetMessage(L"正在同步…");
    RepaintSubtree(g_hWnd);      // 先让用户看见"正在同步"，再去做那件会卡一下的事
    PumpMessages();

    const std::wstring text = GetText(g_hNotes, 1 << 20);
    packcore::Res r = packcore::Write(g_target, g_openFile, U8FromW(text));

    g_syncing = false;
    if (packcore::Ok(r)) {
        g_dirty = false;
        g_lastSync = NowHms();
        SetConnState(Conn::Ok);
        // ★ v1.9.1：便笺写完只做"**便笺那一路**" —— 确认一次连接就够。
        //   以前这里走的是 full refresh（连文件栏一起重拉），
        //   结果是"我只是打了个字，文件栏却跟着刷了"（王要求两者独立）。
        ConfirmOnly();
        SetMessage(L"已连接 · " + g_lastSync + L" 同步");
    } else {
        SetConnState(Conn::Fail);
        SetMessage(L"同步失败：" + r.msg);
        TraceLog("同步失败");
    }
}

// ---------------------------------------------------------------------------
// 用 Notepad++ 打开配置文件
//
// ⚠️⚠️ 千万别改成"给宿主发 NPPM_DOOPEN"：那条消息的 lParam 是**指针**，
//   跨进程 SendMessage 不封送指针，会把 Notepad++ 写崩（踩坑记录 19.1）。
//   所以这里走"查出宿主 exe 路径 + CreateProcess 起一个新进程"，全程不带指针。
// ---------------------------------------------------------------------------
static std::wstring ProcessPathOfWindow(HWND hwnd)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE p = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = _countof(buf);
    std::wstring out;
    if (::QueryFullProcessImageNameW(p, 0, buf, &n)) out = buf;
    ::CloseHandle(p);
    return out;
}

static void OpenConfigForUser()
{
    // ⚠️⚠️ 这里**绝不能**再整份 packcfg::Save(g_cfg)！
    //
    //   老代码在这里调 Save，等于"每打开一次配置文件，就拿内存里的旧值
    //   把磁盘上的文件冲一遍"。王的真实操作是：改配置 → 保存 → 重开发现改动没了 ——
    //   改动就是被这一句冲掉的。
    //
    //   ★ v1.6 起更彻底：便笺的字号调节入口已经取消，界面上**再没有任何"改配置"
    //     的动作**，所以应用对配置文件是**纯只读**的 —— 这里什么都不用做。
    const std::wstring path = packcfg::ConfigPath();

    if (g_embedded) {
        HWND top = ::GetAncestor(g_hWnd, GA_ROOTOWNER);
        if (!top) top = ::GetAncestor(g_hWnd, GA_ROOT);
        const std::wstring exe = ProcessPathOfWindow(top);
        if (!exe.empty()) {
            std::wstring cmd = L"\"" + exe + L"\" \"" + path + L"\"";
            std::vector<wchar_t> cl(cmd.begin(), cmd.end());
            cl.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWNORMAL;
            PROCESS_INFORMATION pi{};
            if (::CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, FALSE,
                                 0, nullptr, nullptr, &si, &pi)) {
                ::CloseHandle(pi.hThread);
                ::CloseHandle(pi.hProcess);
                return;               // Notepad++ 是单实例，会把路径转交给已在跑的那个
            }
        }
    }
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// 子控件
// ---------------------------------------------------------------------------
static HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text,
                      DWORD style, int id, bool visible = true, DWORD ex = 0)
{
    HWND h = ::CreateWindowExW(ex, cls, text,
                               WS_CHILD | (visible ? WS_VISIBLE : 0) | style,
                               0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (h && g_hUiFont) ::SendMessageW(h, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
    return h;
}

// ---- 文件栏的行布局：一处算法，画与点都调它 ----
// 一行：[图标] 文件名 ………………  大小  时间戳
//
// ★ v1.6：行内的 ❗/🗑 两个小图标已经**去掉** —— 它们搬到了文件栏上方那一排
//   按钮（先选中、再点按钮）。行内图标既小又难点，而且"画"和"点"两套几何
//   一旦错位就会出"点垃圾桶却打开了文件"这种事；搬走之后这个隐患也一并没了。
struct RowRects {
    RECT icon{}, name{}, size{}, time{};
};
static void RowLayout(const RECT& rc, int cell, RowRects& o)
{
    const int pad = Dp(4);
    RECT r = rc;
    r.top += 2; r.bottom -= 2;

    // 大小 / 时间戳列的宽按字体量（时间戳固定 11 个字符宽）
    HDC dc = ::GetDC(g_hList);
    int tw = Dp(78), sw = Dp(64);
    if (dc) {
        HGDIOBJ old = ::SelectObject(dc, g_hListFont ? g_hListFont : g_hUiFont);
        SIZE s{};
        if (::GetTextExtentPoint32W(dc, L"00-00 00:00", 11, &s)) tw = s.cx + Dp(8);
        if (::GetTextExtentPoint32W(dc, L"9999.9 MB", 9, &s))    sw = s.cx + Dp(8);
        ::SelectObject(dc, old);
        ::ReleaseDC(g_hList, dc);
    }

    o.time = { r.right - pad - tw, r.top, r.right - pad, r.bottom };
    o.size = { o.time.left - sw, r.top, o.time.left, r.bottom };
    o.icon = { r.left + pad, r.top, r.left + pad + cell, r.bottom };
    o.name = { o.icon.right + pad, r.top, o.size.left - pad, r.bottom };
    if (o.name.right < o.name.left) o.name.right = o.name.left;
}

// 光标落在哪一行 / 行内哪个位置。
// ★ v1.7：列表里**没有**「.. 返回上级」那一行了（王说蓝色那行看着多余，
//   返回上级由文件栏上方的按钮负责），所以这里只剩"在某一行上 / 不在"。
enum class HitZone { None, Up, Row };

static HitZone ListHitTest(HWND hList, int cx, int cy, int* outItem, int* outFileIdx)
{
    if (outItem) *outItem = -1;
    if (outFileIdx) *outFileIdx = -1;

    const DWORD r = (DWORD)::SendMessageW(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(cx, cy));
    const int item = LOWORD(r);
    if (HIWORD(r)) return HitZone::None;          // 落在线外
    if (outItem) *outItem = item;

    if (g_showUp && item == 0) return HitZone::Up;
    const int idx = item - (g_showUp ? 1 : 0);
    if (idx < 0 || idx >= (int)g_files.size()) return HitZone::None;
    if (outFileIdx) *outFileIdx = idx;
    return HitZone::Row;
}

// ---- 文件栏行绘制 ----
static void DrawListRow(const DRAWITEMSTRUCT& dis)
{
    const int item = (int)dis.itemID;
    if (item == (UINT)-1) return;

    const bool isUp = (g_showUp && item == 0);
    const int fidx = item - (g_showUp ? 1 : 0);
    if (!isUp && (fidx < 0 || fidx >= (int)g_files.size())) return;

    const bool sel = (dis.itemState & ODS_SELECTED) != 0;
    const COLORREF bg   = sel ? ::GetSysColor(COLOR_HIGHLIGHT)
                              : RGB(kListBgLevel, kListBgLevel, kListBgLevel);
    const COLORREF fg   = sel ? ::GetSysColor(COLOR_HIGHLIGHTTEXT)
                              : RGB(0x20, 0x20, 0x20);
    const COLORREF dim  = sel ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0x70, 0x70, 0x70);

    HBRUSH br = ::CreateSolidBrush(bg);
    ::FillRect(dis.hDC, &dis.rcItem, br);
    ::DeleteObject(br);

    const int cell = (int)dis.rcItem.bottom - (int)dis.rcItem.top;
    RowRects o{};
    RowLayout(dis.rcItem, cell, o);

    ::SetBkMode(dis.hDC, TRANSPARENT);

    // ★ v1.8：「..」就是**一行普通的目录**（蓝色的文件夹图标 + 名字 ".."）。
    //   它只有"不在背包根"时才出现 —— 也就是王说的"一个新的叫 .. 的文件夹类型文件"。
    if (isUp) {
        appui::DrawFolderGlyph(dis.hDC, o.icon, sel ? fg : RGB(0x0B, 0x53, 0x94));
        ::SetTextColor(dis.hDC, sel ? fg : RGB(0x0B, 0x53, 0x94));
        ::DrawTextW(dis.hDC, L"..", -1, &o.name,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (dis.itemState & ODS_FOCUS) ::DrawFocusRect(dis.hDC, &dis.rcItem);
        return;
    }

    const packcore::FileItem& f = g_files[fidx];

    // ---- 文件名的"左半边"（图标 + 名字）与"右半边"（大小 / 时间）之间，
    //      画一条**淡淡的**竖线（王 v1.7 的要求）。
    //      目的很实在：名字长短不一，右边那两列没有依托时看着像"飘着"的。
    //      颜色只比底色深一点点（0xD2 vs 0xE6），不能抢眼。
    {
        const int x = (o.size.left > o.name.right ? o.size.left : o.name.right) + Dp(3);
        RECT v{ x, dis.rcItem.top + Dp(3), x + 1, dis.rcItem.bottom - Dp(3) };
        HBRUSH lb = ::CreateSolidBrush(sel ? ::GetSysColor(COLOR_HIGHLIGHT)
                                           : RGB(0xD2, 0xD2, 0xD2));
        ::FillRect(dis.hDC, &v, lb);
        ::DeleteObject(lb);
    }

    // 行首图标：**目录用蓝色**（和目录名一个色），文件用灰 —— 王 v1.7：
    // "文件夹类型的文件显示为蓝色，图标也请蓝色"。
    if (f.isDir) appui::DrawFolderGlyph(dis.hDC, o.icon, sel ? fg : RGB(0x0B, 0x53, 0x94));
    else         appui::DrawFileGlyph  (dis.hDC, o.icon, sel ? fg : RGB(0x4A, 0x4A, 0x4A));

    // 名字：**目录和文件颜色不同**（王的要求）
    ::SetTextColor(dis.hDC, sel ? fg : (f.isDir ? RGB(0x0B, 0x53, 0x94)
                                                : RGB(0x20, 0x20, 0x20)));
    // ★ v2.0：符号链接显示成 `名字 -> 目标` —— 光看名字的话，软链和普通文件
    //   长得一模一样（这也是"软链目录进不去"让人莫名其妙的原因）。
    std::wstring shown = f.name;
    if (f.isLink && !f.linkTo.empty()) shown += L" -> " + f.linkTo;
    ::DrawTextW(dis.hDC, shown.c_str(), -1, &o.name,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 大小 / 时间戳（目录不显示大小 —— 那个 4096 会误导人；
    //   v2.0 起符号链接也不显示：ls 给的是"目标路径字符串的长度"，更误导）
    if (!f.isDir && !f.isLink) {
        const std::wstring sz = FmtSize(f.size);
        ::SetTextColor(dis.hDC, dim);
        ::DrawTextW(dis.hDC, sz.c_str(), -1, &o.size,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    const std::wstring tm = FmtTime(f.mtime);
    if (!tm.empty()) {
        ::SetTextColor(dis.hDC, dim);
        ::DrawTextW(dis.hDC, tm.c_str(), -1, &o.time,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (dis.itemState & ODS_FOCUS) ::DrawFocusRect(dis.hDC, &dis.rcItem);
}

// ---- 文件栏的鼠标/拖放（子类过程）----
static void DeleteItemAt(int fileIdx);
static void ShowPropsAt(int fileIdx);
static void ListContextMenu(HWND hList, int cx, int cy, int fileIdx, bool onUp);
static void HandleDroppedFiles(HDROP drop);
static void EnterDir(const std::wstring& name);
static void GoUp();
static void ClearListSelection();

static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto callOrig = [&]() {
        return g_listOrigProc ? ::CallWindowProcW(g_listOrigProc, hwnd, msg, wp, lp)
                              : ::DefWindowProcW(hwnd, msg, wp, lp);
    };

    // ★ v1.6：传输期间**不允许任何其它文件操作**（王的要求）。
    //   这里把文件栏的交互整体挡掉 —— 唯一还能按的是那个变身后的「取消」。
    if (g_busy) {
        switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_DROPFILES:
            if (msg == WM_DROPFILES) ::DragFinish((HDROP)wp);
            return 0;
        }
    }

    switch (msg) {
    case WM_DROPFILES: {
        // 从资源管理器拖进来的文件 → 上传到**当前目录**
        HandleDroppedFiles((HDROP)wp);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        int item = -1, idx = -1;
        const HitZone z = ListHitTest(hwnd, cx, cy, &item, &idx);
        const int row = (z == HitZone::None) ? -1 : item;
        if (row != g_hoverRow) {
            g_hoverRow = row;
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            ::TrackMouseEvent(&tme);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_hoverRow = -1;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    // ⚠️ v1.9：**这里原来有一个 WM_LBUTTONDOWN 分支，已经删掉** ——
    //   它做过两件事：① 点"已选中那一行"→ 取消选中；② 点空白 → 取消选中。
    //   王反馈"点击某个文件无法正常选中"，就是这个：
    //     · 第一次点某一行 = 选中；
    //     · 再点同一下（他以为没点上）= **取消选中** —— 看起来就是"选不中"；
    //     · 而且 `LB_ITEMFROMPOINT` 对条目下方的空白返回的是**最近的那一项**，
    //       所以"点空白"实际也会命中第 ① 条。
    //   现在整段删掉，把选中/取消完全交还给原生列表框（Ctrl / Shift 依旧多选，
    //   Ctrl+点已选中项依旧能取消）。王原话："删掉这个功能，保持原先状态"。

    case WM_LBUTTONDBLCLK: {
        // ★ v1.7：**去掉了"打开文件"** —— 王说文件背包的定位就是"小文件的中转"，
        //   不提供打开；真想看内容就手动下载。
        // ★ v1.8：双击「..」= 返回上级（它现在是列表里的一行"目录"）；
        //   双击真目录 = 进去。
        const int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        int item = -1, idx = -1;
        const HitZone z = ListHitTest(hwnd, cx, cy, &item, &idx);
        if (z == HitZone::Up) { GoUp(); return 0; }
        if (z == HitZone::Row && idx >= 0) {
            const packcore::FileItem& f = g_files[idx];
            if (f.isDir) { EnterDir(f.name); return 0; }
            // ★ v2.0：符号链接在 `ls -l` 里**看不出指向的是目录还是文件**，
            //   所以双击时**试着进一下**：能列出内容就当目录进去；列不出来就明说。
            //   （以前软链一律被当成文件，双击毫无反应 —— 指向目录的软链等于进不去。）
            if (f.isLink) {
                const std::wstring sub = JoinRel(g_curDir, f.name);
                std::vector<packcore::FileItem> probe;
                if (packcore::Ok(packcore::List(g_target, sub, probe))) {
                    EnterDir(f.name);
                } else {
                    SetMessage(L"「" + f.name + L"」指向的不是目录，双击进不去");
                }
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN: {
        // F2 重命名（王的要求：右击能改，选中后按 F2 也能改）
        if (wp == VK_F2 && g_sel >= 0 && g_sel < (int)g_files.size()) {
            StartRename(g_sel);
            return 0;
        }
        break;
    }

    case WM_RBUTTONUP: {
        const int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
        int item = -1, idx = -1;
        const HitZone z = ListHitTest(hwnd, cx, cy, &item, &idx);
        const bool onUp = (z == HitZone::Up);
        // 右击一行时先把它选中，免得"右键点 A、按钮却作用在 B 上"
        if (idx >= 0) {
            const int row = ListRowOf(idx);
            if (!(::SendMessageW(hwnd, LB_GETSEL, (WPARAM)row, 0) > 0)) {
                ClearListSelection();
                ::SendMessageW(hwnd, LB_SETSEL, (WPARAM)TRUE, (LPARAM)row);
                ::SendMessageW(hwnd, LB_SETCURSEL, (WPARAM)row, 0);
            }
            g_sel = idx;
            UpdateButtonsEnabled();
        } else {
            ClearListSelection();
            UpdateButtonsEnabled();
        }
        ListContextMenu(hwnd, cx, cy, onUp ? -1 : idx, onUp);
        return 0;
    }

    case WM_ERASEBKGND: {
        // 背景比默认的再深 10 个灰度（王的要求）—— 空行区域也用它，
        // 不能只靠 WM_DRAWITEM（那管不到没有项的空白处）。
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, g_hListBrush ? g_hListBrush
                                             : ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    }
    return callOrig();
}

// 把文件栏的选中全部取消（"点它自己 / 点空白" 都走它）
static void ClearListSelection()
{
    if (g_hList && ::IsWindow(g_hList)) {
        ::SendMessageW(g_hList, LB_SETSEL, (WPARAM)FALSE, (LPARAM)-1);
        ::SendMessageW(g_hList, LB_SETCURSEL, (WPARAM)-1, 0);
    }
    g_sel = -1;
}

// 文件栏里**所有**选中项在 g_files 里的下标（升序）。
// v1.7：文件栏改成多选（LBS_EXTENDEDSEL），所以按钮要作用在"一整批"上。
static std::vector<int> SelectedIdx()
{
    std::vector<int> out;
    if (!g_hList || !::IsWindow(g_hList)) return out;
    const int n = (int)::SendMessageW(g_hList, LB_GETSELCOUNT, 0, 0);
    if (n <= 0) return out;
    std::vector<int> buf((size_t)n, 0);
    const int got = (int)::SendMessageW(g_hList, LB_GETSELITEMS, (WPARAM)n, (LPARAM)buf.data());
    for (int i = 0; i < got; ++i) {
        // ⚠️ 选中项里可能包含第 0 行的「..」（它不是 g_files 里的一项）→ 换算并丢掉越界的。
        const int idx = buf[i] - (g_showUp ? 1 : 0);
        if (idx >= 0 && idx < (int)g_files.size()) out.push_back(idx);
    }
    return out;
}

// 进子目录 / 返回上级（双击目录、以及上方那个「返回上级」按钮共用）
static void EnterDir(const std::wstring& name)
{
    g_curDir = JoinRel(g_curDir, name);
    TraceLogW("进入目录：", g_curDir);
    RefreshFileList();
}

static void GoUp()
{
    if (g_curDir.empty()) return;
    g_curDir = ParentOf(g_curDir);
    RefreshFileList();
}

// ---------------------------------------------------------------------------
// 删除（v1.8）—— **真删，删了就没了**
//
// 王原话："去掉删除撤销的机制，删了就删了，啥也没了，去掉存放删除文件的文件夹"。
// 所以 v1.7 那套"搬进 .nppbackpack-trash + 指针 + 撤销"整个拿掉了。
// 仍然**不弹确认框**（他之前的明确要求）。
//   远端：rm -f / rm -rf；本地：DeleteFile / RemoveDirectory（**目录会连里面一起删**，
//   这是"删了就删了"的应有之义 —— 以前这里是"不是空目录就报错"）。
// ---------------------------------------------------------------------------
static void DeleteItemAt(int fileIdx)
{
    if (fileIdx < 0 || fileIdx >= (int)g_files.size()) return;
    if (!g_connected) { SetMessage(L"还没连上，先点 ↻ 刷新"); return; }

    const packcore::FileItem f = g_files[fileIdx];
    const std::wstring rel = JoinRel(g_curDir, f.name);
    packcore::Res r = packcore::Remove(g_target, rel, f.isDir);
    if (packcore::Ok(r)) {
        SetConnState(Conn::Ok);
        SetMessage(L"已删除 " + f.name);
        TraceLogW("已删除：", rel);
        if (_wcsicmp(rel.c_str(), g_openFile.c_str()) == 0) g_openFile.clear();
    } else {
        SetConnState(Conn::Fail);
        SetMessage(L"删除失败：" + r.msg);
        TraceLogW("删除失败：", r.msg);
    }
    // ⚠️ 这里**只能拉一次**文件栏：以前是"成功分支里 RefreshFileList()，
    //    末尾又 RequestRefresh()（full → 内部再拉一遍）"，王一眼就看出来刷了两次。
    RefreshListAfterFileOp();
}

// 「删除」按钮：把选中的（可多选）一次删掉
static void DeleteSelected()
{
    if (g_busy) return;
    if (!g_connected) { SetMessage(L"还没连上，先点 ↻ 刷新"); return; }

    const std::vector<int> idxs = SelectedIdx();
    if (idxs.empty()) { SetMessage(L"先在文件栏里选中要删的东西，再点删除"); return; }

    // 拷一份名字：DeleteItemAt 里的 RefreshFileList 会把 g_files 换掉
    std::vector<std::wstring> names;
    for (int i : idxs) names.push_back(g_files[i].name);

    int okCount = 0, failCount = 0;
    std::wstring lastErr;
    for (const auto& nm : names) {
        // 每一项都重新在 g_files 里找一次 —— 删掉一个之后下标就变了
        int k = -1;
        for (size_t j = 0; j < g_files.size(); ++j)
            if (g_files[j].name == nm) { k = (int)j; break; }
        if (k < 0) continue;

        const packcore::FileItem f = g_files[k];
        packcore::Res r = packcore::Remove(g_target, JoinRel(g_curDir, f.name), f.isDir);

        // 目录若是"不空就报错"的本地实现，这里补一次递归删（见 packcore::Remove）
        if (!packcore::Ok(r)) { ++failCount; lastErr = r.msg; }
        else                   { ++okCount; }
    }

    if (failCount == 0) {
        wchar_t b[128];
        swprintf_s(b, L"已删除 %d 项", okCount);
        SetConnState(Conn::Ok);
        SetMessage(b);
    } else {
        wchar_t b[160];
        swprintf_s(b, L"删除：%d 项成功，%d 项失败（%s）",
                   okCount, failCount, lastErr.c_str());
        SetConnState(Conn::Fail);
        SetMessage(b);
    }
    RefreshListAfterFileOp();
}

// ---------------------------------------------------------------------------
// 原地重命名（v1.8）—— 王："重命名的时候能不能尽量不要弹窗，原地修改"
//
// 做法跟资源管理器一样：在那一行的**名字位置**上盖一个 EDIT，
//   回车 = 改；Esc = 放弃；点到别处（失去焦点）= 改。
// 为什么用子窗口而不是"直接把列表变成可编辑"：列表框没有这个能力，
//   而一个盖上去的 EDIT 既简单又和系统行为一致（连光标形状都对）。
// ---------------------------------------------------------------------------
static LRESULT CALLBACK RenameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto callOrig = [&]() {
        return g_renameOrigProc ? ::CallWindowProcW(g_renameOrigProc, hwnd, msg, wp, lp)
                                : ::DefWindowProcW(hwnd, msg, wp, lp);
    };

    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            ::PostMessageW(g_hWnd, kMsgRenameCommit, 0, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            ::PostMessageW(g_hWnd, kMsgRenameCancel, 0, 0);
            return 0;
        }
        break;

    case WM_KILLFOCUS:
        // 点到别处 = 提交（资源管理器的习惯）。
        // ⚠️ 用 Post 而不是直接干：收尾要销毁这个 EDIT 自己。
        if (g_hRenameEdit == hwnd) ::PostMessageW(g_hWnd, kMsgRenameCommit, 0, 0);
        break;

    case WM_CHAR:
        // 名字里不能出现路径分隔符：直接吃掉，不给输
        if (wp == L'/' || wp == L'\\') return 0;
        break;

    case WM_NCDESTROY:
        if (g_renameOrigProc) {
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_renameOrigProc);
            g_renameOrigProc = nullptr;
        }
        break;
    }
    return callOrig();
}

static void StartRename(int fileIdx)
{
    if (g_busy) return;
    if (!g_connected) { SetMessage(L"还没连上，先点 ↻ 刷新"); return; }
    if (fileIdx < 0 || fileIdx >= (int)g_files.size()) return;
    if (g_hRenameEdit) EndRename(false);

    const int row = ListRowOf(fileIdx);
    RECT rc{};
    if (!::SendMessageW(g_hList, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&rc)) return;
    const int cell = (int)::SendMessageW(g_hList, LB_GETITEMHEIGHT, 0, 0);
    RowRects o{};
    RowLayout(rc, cell, o);

    g_renameIdx = fileIdx;
    g_hRenameEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                  g_files[fileIdx].name.c_str(),
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                                  o.name.left, rc.top,
                                  (std::max)(Dp(40), (int)(o.name.right - o.name.left)),
                                  (int)(rc.bottom - rc.top) + Dp(2),
                                  g_hList, nullptr, g_hInst, nullptr);
    if (!g_hRenameEdit) { g_renameIdx = -1; return; }

    ::SendMessageW(g_hRenameEdit, WM_SETFONT,
                   (WPARAM)(g_hListFont ? g_hListFont : g_hUiFont), TRUE);

    // 只选中"主文件名"（不含扩展名）—— 跟资源管理器一样，直接打字就能替换掉
    const std::wstring nm = g_files[fileIdx].name;
    const size_t dot = nm.find_last_of(L'.');
    const int selEnd = (dot == std::wstring::npos || dot == 0) ? (int)nm.size() : (int)dot;
    ::SendMessageW(g_hRenameEdit, EM_SETSEL, 0, selEnd);

    g_renameOrigProc = (WNDPROC)(LONG_PTR)::SetWindowLongPtrW(
        g_hRenameEdit, GWLP_WNDPROC, (LONG_PTR)RenameProc);

    ::SetFocus(g_hRenameEdit);
    TraceLogW("开始重命名：", nm);
}

static void EndRename(bool commit)
{
    HWND h = g_hRenameEdit;
    if (!h) return;
    const int idx = g_renameIdx;

    // 先"摘掉"自己的登记，后面谁再调进来都直接返回（避免递归）
    g_hRenameEdit   = nullptr;
    g_renameIdx = -1;

    std::wstring newName;
    if (commit) {
        const int n = ::GetWindowTextLengthW(h);
        std::wstring t((size_t)(std::max)(n + 1, 8), L'\0');
        const int got = ::GetWindowTextW(h, &t[0], (int)t.size());
        t.resize((size_t)(got > 0 ? got : 0));
        while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
        while (!t.empty() && (t.back()  == L' ' || t.back()  == L'\t')) t.pop_back();
        newName = t;
    }

    if (g_renameOrigProc) {
        ::SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)g_renameOrigProc);
        g_renameOrigProc = nullptr;
    }
    ::DestroyWindow(h);
    if (g_hList && ::IsWindow(g_hList)) ::SetFocus(g_hList);

    if (!commit) return;
    if (idx < 0 || idx >= (int)g_files.size()) return;

    const std::wstring oldName = g_files[idx].name;
    if (newName.empty() || newName == oldName) return;      // 没改 / 改空了 → 什么都不做
    if (newName.find(L'/') != std::wstring::npos ||
        newName.find(L'\\') != std::wstring::npos) {
        SetMessage(L"名字里不能带路径分隔符");
        return;
    }
    if (packcfg::IsReservedName(newName)) {
        SetMessage(L"这个名字是应用自己用的，换一个");
        return;
    }

    packcore::Res r = packcore::Move(g_target, JoinRel(g_curDir, oldName),
                                             JoinRel(g_curDir, newName));
    if (packcore::Ok(r)) {
        SetConnState(Conn::Ok);
        SetMessage(L"已重命名为 " + newName);
        TraceLogW("已重命名：", oldName + L" → " + newName);
    } else {
        SetConnState(Conn::Fail);
        SetMessage(L"重命名失败：" + r.msg);
    }
    RefreshListAfterFileOp();
}

// 「文件属性」——王 v1.7 明确只要**四项**：名称 / 类型 / 大小 / 时间。
// （之前塞了十来行"位置 / 当前目录 / 连接方式 / 下载夹"之类，是我想多了。）
static void ShowPropsAt(int fileIdx)
{
    if (fileIdx < 0 || fileIdx >= (int)g_files.size()) return;
    const packcore::FileItem& f = g_files[fileIdx];

    std::vector<packprops::Row> rows;
    rows.push_back({ L"名称", f.name });
    rows.push_back({ L"类型", f.isDir ? L"目录" : L"文件" });
    rows.push_back({ L"大小", f.isDir ? std::wstring(L"—") : FmtSize(f.size) });
    const std::wstring tm = FmtTime(f.mtime);
    rows.push_back({ L"时间", tm.empty() ? L"—" : tm });

    packprops::Show(g_hWnd, L"文件属性", rows);
    ConfirmOnly();
}

static void ListContextMenu(HWND hList, int cx, int cy, int fileIdx, bool onUp)
{
    HMENU m = ::CreatePopupMenu();
    if (!m) return;

    // 菜单是**模态**弹出的 —— 外部（探针/人）看不见它里面有什么项。
    // 所以顺手把这些项记进轨迹日志：既能验"菜单内容对不对"，
    // 也能在用户说"我要的那个菜单项没有"时一眼看清当时到底给了什么。
    std::wstring items;
    auto addItem = [&](UINT id, const wchar_t* text) {
        ::AppendMenuW(m, MF_STRING, id, text);
        if (!items.empty()) items += L" | ";
        items += text;
    };

    // ★ v1.7：简洁（王的要求）—— 名字就用「打开」「下载」；**一条分割线都不留**。
    //   · 「打开」只给**目录**（文件不再提供"打开"这个功能）
    // ★ v1.9.2：去掉「在文件夹中显示」（王："关掉"），换成「属性」（跟按钮同义）；
    //   空白处的菜单加上「上传」。
    if (onUp) {
        // 「..」那一行的菜单：只有"打开"（= 返回上级）
        addItem(kRowMenuOpen, L"打开");
    } else if (fileIdx >= 0 && fileIdx < (int)g_files.size()) {
        const bool isDir = g_files[fileIdx].isDir;
        if (isDir) addItem(kRowMenuOpen,     L"打开");
        else       addItem(kRowMenuDownload, L"下载");
        addItem(kRowMenuInfo,   L"属性");
        addItem(kRowMenuRename, L"重命名");
        addItem(kRowMenuDelete, L"删除");
    } else {
        addItem(kEmptyMenuUpload,    L"上传");
        addItem(kEmptyMenuRefresh,   L"刷新");
        addItem(kEmptyMenuOpenDlDir, L"打开文件位置");
    }
    TraceLogW(onUp ? "右键菜单（..行）："
                   : (fileIdx >= 0 ? "右键菜单（文件行）：" : "右键菜单（空白）："),
              items);

    POINT pt{ cx, cy };
    ::ClientToScreen(hList, &pt);
    // 经典坑：TrackPopupMenu 之前要 SetForegroundWindow，之后要补 WM_NULL
    ::SetForegroundWindow(g_hWnd);
    const int cmd = (int)::TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          pt.x, pt.y, 0, g_hWnd, nullptr);
    ::DestroyMenu(m);
    ::PostMessageW(g_hWnd, WM_NULL, 0, 0);

    if (cmd == kEmptyMenuUpload)     { StartUpload(); return; }
    if (cmd == kEmptyMenuRefresh)    { UserRefresh(); return; }   // 带便笺的那套刷新
    if (cmd == kEmptyMenuOpenDlDir)  { OpenLocalDownloadFolder(); return; }
    if (onUp) {
        if (cmd == kRowMenuOpen) GoUp();
        return;
    }
    if (fileIdx < 0 || fileIdx >= (int)g_files.size()) return;

    const packcore::FileItem f = g_files[fileIdx];

    if (cmd == kRowMenuOpen) {
        if (f.isDir) EnterDir(f.name);
    } else if (cmd == kRowMenuDownload) {
        g_sel = fileIdx;
        StartDownload();
    } else if (cmd == kRowMenuInfo) {
        ShowPropsAt(fileIdx);            // 跟文件栏上方那个「文件属性」按钮**同一个入口**
    } else if (cmd == kRowMenuRename) {
        StartRename(fileIdx);
    } else if (cmd == kRowMenuDelete) {
        DeleteItemAt(fileIdx);
    }
}

// 拖入文件 → 上传到当前目录（跟「上传」按钮走**同一条**流式通道，带进度可取消）
static void HandleDroppedFiles(HDROP drop)
{
    if (!drop) return;
    const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (n == 0) { ::DragFinish(drop); return; }

    if (!g_connected) {
        ::DragFinish(drop);
        SetMessage(L"还没连上，暂时不能上传");
        return;
    }
    g_cancelReq = false;         // 见 StartDownload 的注释

    int okCount = 0, failed = 0;
    BeginBatch(IDC_UPLOAD);      // 拖入多文件也是一批
    for (UINT i = 0; i < n; ++i) {
        if (g_cancelReq) break;
        wchar_t src[MAX_PATH * 2]{};
        if (!::DragQueryFileW(drop, i, src, _countof(src))) continue;

        const DWORD attr = ::GetFileAttributesW(src);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            { ++failed; continue; }          // 拖目录暂不支持（递归上传风险大）

        std::wstring s(src);
        const size_t k = s.find_last_of(L"\\/");
        const std::wstring name = (k == std::wstring::npos) ? s : s.substr(k + 1);
        if (name.empty()) continue;

        wchar_t head[160];
        swprintf_s(head, L"正在上传 %s（%u/%u）", name.c_str(), i + 1, n);
        BeginBusy(IDC_UPLOAD, head);
        packcore::Res r = packcore::WriteFromFile(g_target, JoinRel(g_curDir, name),
                                                 src, 0, OnProgress, nullptr);
        EndBusy();
        if (packcore::Ok(r)) { ++okCount; TraceLogW("已上传：", name); }
        else if (r.st == packcore::St::Cancelled) { SetMessage(L"已取消上传"); break; }
        else { ++failed; TraceLogW("上传失败：", r.msg); }
    }
    EndBatch();
    ::DragFinish(drop);

    if (g_cancelReq) {
        RefreshFileList();
        SetMessage(L"已取消上传");
    } else {
        wchar_t b[160];
        swprintf_s(b, L"上传完成：成功 %d，失败 %d", okCount, failed);
        SetMessage(b);
        RefreshListAfterFileOp();
    }
}

// ---------------------------------------------------------------------------
// v1.6：本地路径 / 文件传输独占（进度 + 「取消」）/ 文件操作
// ---------------------------------------------------------------------------
static std::wstring JoinPathNative(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

// ★ v1.7：「打开」（下载到临时目录再用默认程序打开）这个功能**整个去掉了**。
//   王的定位很清楚 —— 文件背包是"小文件的中转"，不是文件管理器；
//   想看内容就按部就班下载下来自己打开。于是 open_tmp_dir 这个配置项
//   眼下没有使用者了（保留着，免得老配置读进来少一项）。
//   （顺带一提：这里原先的"每次启动清空临时目录"也跟着一起去掉了。）
static void OpenFolderInExplorer(const std::wstring& dir, const std::wstring& selectFile)
{
    if (dir.empty()) return;
    if (!selectFile.empty()) {
        const std::wstring p = JoinPathNative(dir, selectFile);
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring args = L"/select,\"" + p + L"\"";
            ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    if (::GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        ::CreateDirectoryW(dir.c_str(), nullptr);
    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 按"能不能用"刷新按钮的可用状态：没连上/没选中/正在传输 → 该灰就灰
//
// ⚠️⚠️ v1.7 修的一个真 bug：传输期间**被点的那个按钮必须保持可点** ——
//   它此刻的身份是「取消」。以前的写法是"忙碌时把所有按钮一律禁掉"，
//   于是那个变身的「取消」自己也被禁用了，**点不动** ——
//   王报的"上传下载打开的取消按钮并没有生效"就是这个。
// "这个按钮此刻是不是变身为「取消」的那一个"。批次里也要一直认它 ——
// 否则文件与文件之间的缝里，取消按钮会闪回「下载」的脸、而且点了没反应。
static bool IsBusySlot(int id)
{
    if (id == 0) return false;
    if (g_busy) return id == g_busyBtn;
    return g_batchDepth > 0 && id == g_batchBtn;
}

static void UpdateButtonsEnabled()
{
    // v1.9：`g_confirming`（正在探连接）也算"忙" —— 因为等 ssh 的时候会抽消息泵，
    // 这段时间里消息是真会被处理的；不挡住的话用户能在"连接还没回来"时
    // 再点下载/刷新，套出二层 Probe（能跑，但状态会互相盖，没必要）。
    // v2.0：再加 `g_syncing`（便笺同步那一刻同样会抽泵）与批次深度 ——
    // 三处都是"界面看起来闲着、其实 ssh 正在跑"的时刻。
    const bool idle   = !g_busy && !g_confirming && !g_syncing && g_batchDepth == 0;
    const bool conn   = g_connected;
    const bool hasSel = conn && SelectedIdx().size() > 0;

    // 窗口标题也要跟着状态走（探针/读屏靠它认按钮）。
    // ⚠️ 这里**每次都同步**：传输开始/结束也会让"被点那个"换上「取消」的脸，
    //    只在 g_hasSel 变化时同步的话，窗口标题会一直停在「下载」——
    //    画出来是「取消」、读出来是「下载」，两边对不上（探针就栽在这儿）。
    g_hasSel = hasSel;
    SyncBtnCaptions();

    // 忙碌时：只有"正在跑的那个按钮"（它会变身成「取消」）保持可点
    auto set = [&](HWND h, bool on, int id = 0) {
        if (!h || !::IsWindow(h)) return;
        const bool busySlot = IsBusySlot(id);
        ::EnableWindow(h, ((busySlot || (on && idle))) ? TRUE : FALSE);
    };
    set(g_hDown,     hasSel,       IDC_DOWNLOAD);
    set(g_hUpload,   true,         IDC_UPLOAD);
    set(g_hInfo,     hasSel,       IDC_INFO);
    set(g_hRename,   hasSel,       IDC_RENAME);
    set(g_hDelete,   hasSel,       IDC_DELETE);
    if (g_hSync && ::IsWindow(g_hSync)) ::EnableWindow(g_hSync, idle ? TRUE : FALSE);
    if (g_hGear && ::IsWindow(g_hGear)) ::EnableWindow(g_hGear, idle ? TRUE : FALSE);
    // "打开文件位置"跟远端没关系，任何时候都该能按
    if (g_hDlFolder && ::IsWindow(g_hDlFolder)) ::EnableWindow(g_hDlFolder, TRUE);

    if (g_hWnd) RepaintSubtree(g_hWnd);
}

// 批次的两个口子。多文件下载/上传把整个 for 包起来：
//   BeginBatch(IDC_DOWNLOAD) … 循环（里面照旧 BeginBusy/EndBusy）… EndBatch()
// 这样"文件与文件之间"那几毫秒也仍然是禁用状态，取消按钮也不会闪回原脸。
static void BeginBatch(int btnId)
{
    ++g_batchDepth;
    g_batchBtn = btnId;
    UpdateButtonsEnabled();
    SyncBtnCaptions();
}

static void EndBatch()
{
    if (g_batchDepth > 0) --g_batchDepth;
    if (g_batchDepth == 0) g_batchBtn = 0;
    UpdateButtonsEnabled();
    SyncBtnCaptions();
}

static void BeginBusy(int btnId, const std::wstring& text)
{
    g_busy        = true;
    g_busyBtn     = btnId;
    g_cancelReq   = false;
    g_progressPct = -1;
    g_progressDone = g_progressTotal = 0;
    g_busyText    = text;
    RateReset();                        // v1.9：速率窗口每轮重来（否则会带着上一轮的数）
    SetConnState(Conn::Busy);
    SetMessage(text);
    UpdateButtonsEnabled();
    RepaintStatusArea();
}

static void EndBusy()
{
    g_busy        = false;
    g_busyBtn     = 0;
    g_progressPct = -1;
    g_busyText.clear();
    UpdateButtonsEnabled();
    RepaintStatusArea();
    // 传输中用户点了关闭 → 到这里收尾完了，再真的关（异步，避免在按钮处理里销毁窗口）
    if (g_closePending) {
        g_closePending = false;
        ::PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
    }
}

// ⚠️ packcore 在等 ssh 的循环里调这个函数 —— 也就是说它**跑在 UI 线程上**。
//    所以这里必须抽一次消息泵：不然界面是冻着的，「取消」按下去根本没人处理。
static bool OnProgress(long long done, long long total, void*)
{
    PumpMessages();
    if (g_cancelReq) return false;          // → packcore 会 TerminateProcess 并删掉半截文件

    g_progressDone  = done;
    g_progressTotal = total;
    g_progressPct   = (total > 0) ? (int)((done * 100) / total) : -1;

    RatePush(done);                         // v1.9：最近约 1 秒的速率

    wchar_t b[256];
    if (g_progressPct >= 0) {
        swprintf_s(b, L"%s  %d%%（%s / %s）", g_busyText.c_str(), g_progressPct,
                   FmtSize((unsigned long long)done).c_str(),
                   FmtSize((unsigned long long)total).c_str());
    } else {
        swprintf_s(b, L"%s …（%s）", g_busyText.c_str(),
                   FmtSize((unsigned long long)done).c_str());
    }
    std::wstring line = b;

    // ★ v1.9：速率（最近一秒）+ 剩余时间（自动 h/m/s）。
    //   速度还没量出来（刚开头）就什么都不加，免得显示 "0 B/s" 晃眼。
    const std::wstring rate = FmtRate(g_rateBps);
    if (!rate.empty()) {
        line += L"  " + rate;
        if (total > 0 && done < total && g_rateBps > 1.0) {
            const long long left =
                (long long)((double)(total - done) / g_rateBps + 0.5);
            const std::wstring eta = FmtEta(left);
            if (!eta.empty()) line += L"  剩余 " + eta;
        }
    }
    SetMessage(line);
    return true;
}

// 递归下载一个目录：自己走 List，再逐文件 ReadToFile（进度按每个文件算）
static bool DownloadTreeRec(const std::wstring& relDir, const std::wstring& localDir,
                            int& okCount, int& failCount)
{
    std::vector<packcore::FileItem> items;
    packcore::Res r = packcore::List(g_target, relDir, items);
    if (!packcore::Ok(r)) { ++failCount; return false; }

    ::CreateDirectoryW(localDir.c_str(), nullptr);
    for (const auto& it : items) {
        if (g_cancelReq) return false;
        const std::wstring rel = JoinRel(relDir, it.name);
        const std::wstring dst = JoinPathNative(localDir, it.name);
        if (it.isDir) {
            if (!DownloadTreeRec(rel, dst, okCount, failCount)) return false;
        } else {
            packcore::Res d = packcore::ReadToFile(g_target, rel, dst,
                                                   (long long)it.size, OnProgress, nullptr);
            if (packcore::Ok(d)) { ++okCount; }
            else {
                ++failCount;
                if (d.st == packcore::St::Cancelled) return false;
            }
        }
    }
    return true;
}

// 「下载」：选中项（**可多选**）→ 本地「下载」文件夹（目录则递归）
static void StartDownload()
{
    if (g_busy) return;
    if (!g_connected) { SetMessage(L"还没连上，先点 ↻ 刷新"); return; }
    // ⚠️⚠️ 每轮开始必须清掉「取消」标记！它只在 BeginBusy 里清，
    //   而这几个函数在**进循环之前**就会看一眼 g_cancelReq 决定要不要干活 ——
    //   于是"上一次取消过"会连累"这一次刚点下载就自取消"（实测踩到）。
    g_cancelReq = false;

    const std::vector<int> idxs = SelectedIdx();
    if (idxs.empty()) {
        SetMessage(L"先在文件栏里选中一项（可多选），再点「下载」");
        return;
    }
    const std::wstring dir = g_cfg.downloadDir;
    if (dir.empty()) { SetMessage(L"没配置 download_dir（下载目录）"); return; }
    ::CreateDirectoryW(dir.c_str(), nullptr);

    int okCount = 0, failCount = 0;
    std::wstring lastDst;      // 最后落地的那个路径 → 下载完把它在下载夹里选出来
    BeginBatch(IDC_DOWNLOAD);  // v2.0：整批期间按钮不松手（见 BeginBatch 注释）
    for (size_t n = 0; n < idxs.size(); ++n) {
        if (g_cancelReq) break;
        const packcore::FileItem f = g_files[idxs[n]];
        const std::wstring rel = JoinRel(g_curDir, f.name);

        if (f.isDir) {
            const std::wstring dstDir = JoinPathNative(dir, f.name);
            wchar_t head[192];
            swprintf_s(head, L"正在下载目录 %s（%d/%d）", f.name.c_str(),
                       (int)(n + 1), (int)idxs.size());
            BeginBusy(IDC_DOWNLOAD, head);
            DownloadTreeRec(rel, dstDir, okCount, failCount);
            lastDst = dstDir;
            EndBusy();
            if (g_cancelReq) break;
        } else {
            const std::wstring dst = JoinPathNative(dir, f.name);
            wchar_t head[192];
            swprintf_s(head, L"正在下载 %s（%d/%d）", f.name.c_str(),
                       (int)(n + 1), (int)idxs.size());
            BeginBusy(IDC_DOWNLOAD, head);
            packcore::Res r = packcore::ReadToFile(g_target, rel, dst,
                                                   (long long)f.size, OnProgress, nullptr);
            EndBusy();
            if (packcore::Ok(r)) { ++okCount; lastDst = dst; }
            else if (r.st == packcore::St::Cancelled) break;
            else { ++failCount; SetMessage(L"下载失败：" + r.msg); }
        }
    }
    EndBatch();

    if (g_cancelReq) {
        // ⚠️ 用户**主动取消**时不要再走那套统一刷新：刷新会把状态改成"已刷新"，
        //    而"已取消"这句话才是他此刻唯一想看到的（实测就被盖掉过）。
        //    列表单独拉一下就行 —— 那不会动提示词。
        RefreshFileList();
        SetMessage(L"已取消下载");
    } else {
        wchar_t b[176];
        swprintf_s(b, L"下载完成：%d 项成功，%d 项失败", okCount, failCount);
        SetMessage(b);
        SetConnState(failCount ? Conn::Fail : Conn::Ok);
        // 王的要求：下载成功自动打开本地下载文件夹（并尽量把刚下的那个选出来）
        if (okCount > 0) {
            const std::wstring name = lastDst.empty() ? std::wstring()
                                     : lastDst.substr(lastDst.find_last_of(L"\\/") + 1);
            OpenFolderInExplorer(dir, name);
        }
        RefreshListAfterFileOp();
    }
}

// 「上传」：挑本地文件 → 传到当前目录。支持多选。
static void StartUpload()
{
    if (g_busy) return;
    if (!g_connected) { SetMessage(L"还没连上，先点 ↻ 刷新"); return; }
    g_cancelReq = false;         // 见 StartDownload 的注释

    std::vector<wchar_t> buf(64 * 1024, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hWnd;
    ofn.lpstrFilter = L"所有文件\0*.*\0\0";
    ofn.lpstrFile   = buf.data();
    ofn.nMaxFile    = (DWORD)buf.size();
    ofn.lpstrTitle  = L"上传到当前目录";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&ofn)) return;

    // 多选的返回值是"目录 + 一串文件名"（double-null 结尾）；单选时整串就是文件名
    std::vector<std::wstring> files;
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') {
        files.push_back(first);
    } else {
        const std::wstring dir = first;
        while (*p) {
            files.push_back(JoinPathNative(dir, std::wstring(p)));
            p += wcslen(p) + 1;
        }
    }
    if (files.empty()) return;

    int ok = 0, fail = 0;
    BeginBatch(IDC_UPLOAD);
    for (size_t i = 0; i < files.size(); ++i) {
        if (g_cancelReq) break;
        const std::wstring& src = files[i];
        const size_t k = src.find_last_of(L"\\/");
        const std::wstring name = (k == std::wstring::npos) ? src : src.substr(k + 1);

        wchar_t head[176];
        swprintf_s(head, L"正在上传 %s（%d/%d）", name.c_str(),
                   (int)(i + 1), (int)files.size());
        BeginBusy(IDC_UPLOAD, head);
        packcore::Res r = packcore::WriteFromFile(g_target, JoinRel(g_curDir, name),
                                                 src, 0, OnProgress, nullptr);
        EndBusy();
        if (packcore::Ok(r)) { ++ok; SetConnState(Conn::Ok); }
        else if (r.st == packcore::St::Cancelled) { SetMessage(L"已取消上传"); break; }
        else { ++fail; SetConnState(Conn::Fail); SetMessage(L"上传失败：" + r.msg); }
    }
    EndBatch();
    if (g_cancelReq) {
        RefreshFileList();
        SetMessage(L"已取消上传");
    } else {
        wchar_t b[160];
        swprintf_s(b, L"上传完成：成功 %d，失败 %d", ok, fail);
        SetMessage(b);
        RefreshListAfterFileOp();
    }
}

// 「打开文件位置」——把本地「下载」文件夹打开，并把选中项（若下过）选出来。
// 名字按王 v1.7 的叫法统一成"打开文件位置"。
static void OpenLocalDownloadFolder()
{
    const std::wstring dir = g_cfg.downloadDir;
    if (dir.empty()) { SetMessage(L"没配置 download_dir（下载目录）"); return; }

    // 有且只有一项被选中时，尽量把它在文件夹里选出来（"在文件夹中显示"的语义）
    const std::vector<int> idxs = SelectedIdx();
    const std::wstring pick = (idxs.size() == 1 && !g_files[idxs[0]].isDir)
                                  ? g_files[idxs[0]].name : std::wstring();
    OpenFolderInExplorer(dir, pick);
    SetMessage(L"已打开文件位置");
}

// ---- 便笺区 ----
//
// ★ v1.6：**取消了 Ctrl+滚轮调字号**。王给的理由很干脆 ——
//   只要界面上还能改配置，就还会有"程序回写配置文件"这条路，
//   也就还会有"两个写者互相覆盖"的老问题。入口一去，配置文件对应用
//   就是纯只读的：想改？改文件，重开标签页。
// ★ v1.9：连那句空便笺的灰字提示（水印）也去掉了（王："有点多余"）——
//   于是这个控件不再需要子类，纯原生 EDIT。

// 分隔条：**固定位置、不可拖**（王 v1.4 要求左右比不可修改）。
// 名字还叫 splitter，但只负责画那条分界线 + 撑开间距。
static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect(dc, &rc, ::GetSysColorBrush(COLOR_BTNFACE));
        // 一条竖直的灰线，就是"分界"
        RECT line{ (rc.left + rc.right) / 2, rc.top + Dp(2),
                   (rc.left + rc.right) / 2 + 1, rc.bottom - Dp(2) };
        HBRUSH br = ::CreateSolidBrush(RGB(0xC8, 0xC8, 0xC8));
        ::FillRect(dc, &line, br);
        ::DeleteObject(br);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// tooltip：右上一排是纯图标按钮（王要的图标形态），没有提示文字会不好认
static void AddTip(HWND parent, HWND ctl, const wchar_t* text);
static HWND g_hTip = nullptr;

static void CreateTooltip(HWND parent)
{
    g_hTip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               0, 0, 0, 0, parent, nullptr, g_hInst, nullptr);
    if (!g_hTip) return;
    ::SendMessageW(g_hTip, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
}

static void AddTip(HWND parent, HWND ctl, const wchar_t* text)
{
    if (!g_hTip || !ctl) return;
    TOOLINFOW ti{};
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = parent;
    ti.uId      = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    ::SendMessageW(g_hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// ---- v1.7：文件栏上方那一排按钮 ----
//
// 王给的顺序：返回上级 文件属性 删除 上传 下载 打开文件位置。
// 全部**纯文字、等宽、一样大**（他说"这样看起来很爽"）。
//
// 为什么每项有两个名字（full / shortLbl）：
//   这排按钮只占**文件栏那 38%** 的宽度，六个长名字在窄面板里根本排不下。
//   所以布局时量一次文字宽：排得下就用全名，排不下就自动换成短名
//   （提示条里始终写全名，鼠标一停就能看到它到底是什么）。
// ★ v1.8：一排**纯图标按钮**（王："都用图标显示"）。
//   每个按钮 = 图标 + 一句提示（窗口标题里也写一份，探针/读屏能用）。
struct BtnDef { int id; appui::GlyphFn glyph; const wchar_t* label; };
static const BtnDef kFileBtns[] = {
    { IDC_INFO,     appui::DrawInfoGlyph,     L"文件属性"     },
    { IDC_RENAME,   appui::DrawPencilGlyph,   L"重命名"       },
    { IDC_DELETE,   appui::DrawTrashGlyph,    L"删除"         },
    { IDC_UPLOAD,   appui::DrawUploadGlyph,   L"上传"         },
    { IDC_DOWNLOAD, appui::DrawDownloadGlyph, L"下载"         },
    { IDC_DLFOLDER, appui::DrawFolderGlyph,   L"打开文件位置" },
};
static const int kFileBtnCount = (int)(sizeof(kFileBtns) / sizeof(kFileBtns[0]));
// （g_hasSel 定义在文件开头的全局区 —— UpdateButtonsEnabled 也要用它。）

static HWND FileBtnHWnd(int id)
{
    switch (id) {
    case IDC_INFO:     return g_hInfo;
    case IDC_RENAME:   return g_hRename;
    case IDC_DELETE:   return g_hDelete;
    case IDC_UPLOAD:   return g_hUpload;
    case IDC_DOWNLOAD: return g_hDown;
    case IDC_DLFOLDER: return g_hDlFolder;
    }
    return nullptr;
}

// 某个按钮此刻"叫什么"（画出来的是图标，但**窗口标题**里得有个名字）
static std::wstring FileBtnLabel(int id)
{
    for (const auto& b : kFileBtns) if (b.id == id) return b.label;
    return std::wstring();
}

// 把名字同步到窗口标题上 —— owner-draw 按钮本身不存文字，
// 但**窗口标题**存着，探针/读屏就能靠 GetWindowText 读出"这个按钮是什么"。
// 传输中那一个会写成「取消」（跟画出来的红叉保持一致）。
static void SyncBtnCaptions()
{
    for (const auto& b : kFileBtns) {
        HWND h = FileBtnHWnd(b.id);
        if (!h || !::IsWindow(h)) continue;
        const std::wstring cap =
            IsBusySlot(b.id) ? std::wstring(L"取消") : FileBtnLabel(b.id);
        ::SetWindowTextW(h, cap.c_str());
    }
}

static void CreateChildren(HWND parent)
{
    // ---- 文件栏上方那一排（左对齐、等大、**纯图标**）----
    // owner-draw：一是要"取消"时同一个按钮换脸，二是要图标居中。
    g_hInfo      = MakeChild(parent, L"BUTTON", L"文件属性",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_INFO);
    g_hRename    = MakeChild(parent, L"BUTTON", L"重命名",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_RENAME);
    g_hDelete    = MakeChild(parent, L"BUTTON", L"删除",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_DELETE);
    g_hUpload    = MakeChild(parent, L"BUTTON", L"上传",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_UPLOAD);
    g_hDown      = MakeChild(parent, L"BUTTON", L"下载",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_DOWNLOAD);
    g_hDlFolder  = MakeChild(parent, L"BUTTON", L"打开文件位置",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_DLFOLDER);

    // ---- 便笺上方那一行：左边状态（圆点+提示词），右边（设置 ▸ 刷新）----
    g_hStatus    = MakeChild(parent, L"STATIC", L"", SS_OWNERDRAW, IDC_STATUS);

    g_hGear      = MakeChild(parent, L"BUTTON", L"设置",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_GEAR);
    g_hSync      = MakeChild(parent, L"BUTTON", L"刷新",
                             BS_OWNERDRAW | WS_TABSTOP, IDC_SYNC);

    // 文件栏：owner-draw 固定行高（要自己画图标/大小/时间戳）+ **多选**
    // ★ v1.7：加了 LBS_EXTENDEDSEL —— 王要求"文件栏支持多选"
    //   （Ctrl/Shift 点选，跟资源管理器一个习惯）。
    g_hList = MakeChild(parent, L"LISTBOX", L"",
                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED
                        | LBS_HASSTRINGS | LBS_EXTENDEDSEL
                        | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                        IDC_FILELIST);

    g_hSplit = MakeChild(parent, kSplitterCls, L"", 0, IDC_SPLITTER);

    // 便笺：多行、可编辑、带边框（WS_EX_CLIENTEDGE 让人一眼看出是输入框）
    g_hNotes = MakeChild(parent, L"EDIT", L"",
                         WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN
                         | ES_AUTOVSCROLL | ES_LEFT | WS_TABSTOP,
                         IDC_NOTES, true, WS_EX_CLIENTEDGE);

    ApplyFonts();
    SyncBtnCaptions();

    // 文件栏：接受拖入（子类化 + 放开 UIPI）
    if (g_hList) {
        ::DragAcceptFiles(g_hList, TRUE);
        AllowDropMessages(g_hList);
        g_listOrigProc = (WNDPROC)(LONG_PTR)::SetWindowLongPtrW(
            g_hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
    }
    // 便笺是纯原生 EDIT —— v1.9 去掉水印后不再需要子类化。
    UpdateButtonsEnabled();
}

static void LayoutChildren(int W, int H)
{
    if (!g_hWnd) return;
    const int pad = Dp(6), gap = Dp(6);
    const int rowH = (std::max)(Dp(24), appui::FontCellHeight(g_hUiFont) + Dp(10));

    // ---- 先把下半区的 38/62 算出来：顶部那一行要**按同一条分界线分成两段** ----
    const int top    = pad + rowH + gap;
    int bodyH = H - top - pad;
    if (bodyH < Dp(60)) bodyH = Dp(60);

    const int splitW = Dp(10);
    const int usable = W - pad * 2 - splitW;
    int leftW = (int)(usable * kSplitRatio);
    if (leftW < Dp(70))  leftW = Dp(70);
    if (leftW > usable - Dp(70)) leftW = usable - Dp(70);

    const int leftX  = pad;
    const int rightX = pad + leftW + splitW;      // 便笺栏（也是右段）的左边缘

    // ---- 顶部左段（在**文件栏正上方**）：6 个图标按钮左对齐 + 设置/刷新右对齐 ----
    // ★ v1.9：设置/刷新从"便笺上方"搬到这里（王的要求）。
    //   同一排里一共 8 个按钮，所以先按"8 个都排得下"算边长；
    //   排得下就用正方形（= 行高），排不下才一起压窄。
    const int sideCnt = kFileBtnCount + 2;          // 6 个文件操作 + 设置 + 刷新
    int btnW = rowH;
    if (btnW * sideCnt + gap * (sideCnt - 1) > leftW) {
        btnW = (leftW - gap * (sideCnt - 1)) / sideCnt;
        if (btnW < Dp(16)) btnW = Dp(16);           // 极端窄面板的兜底
    }

    int x = leftX;
    auto put = [&](HWND h, int w) {
        if (h && ::IsWindow(h))
            ::SetWindowPos(h, nullptr, x, pad, w, rowH, SWP_NOZORDER | SWP_NOACTIVATE);
        x += w + gap;
    };
    for (int i = 0; i < kFileBtnCount; ++i) put(FileBtnHWnd(kFileBtns[i].id), btnW);

    // 设置/刷新右对齐到**文件栏的右缘**
    int xr = leftX + leftW;
    auto putRightL = [&](HWND h, int w) {
        xr -= w;
        if (h && ::IsWindow(h))
            ::SetWindowPos(h, nullptr, xr, pad, w, rowH, SWP_NOZORDER | SWP_NOACTIVATE);
        xr -= gap;
    };
    putRightL(g_hSync, btnW);
    putRightL(g_hGear, btnW);

    // ---- 顶部右段（在**便笺正上方**）：只剩状态（圆点 + 提示词）----
    // 状态控件从 rightX 起 —— 也就是**跟下面的便笺框同一条左边缘**。
    // 圆点占一点宽度，所以再给便笺正文加一个等宽的左边距，
    // 这样"提示词"那几个字和便笺里打出来的字是**真正对齐**的（王的要求）。
    const int dotSpan = (std::max)(Dp(8), rowH / 2) + Dp(8) - Dp(3);
    int statusW = W - pad - rightX;
    if (statusW < Dp(80)) statusW = Dp(80);
    if (g_hStatus && ::IsWindow(g_hStatus))
        ::SetWindowPos(g_hStatus, nullptr, rightX, pad, statusW, rowH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_hNotes && ::IsWindow(g_hNotes))
        ::SendMessageW(g_hNotes, EM_SETMARGINS, 0x0001 /*EC_LEFTMARGIN*/,
                       MAKELONG(dotSpan, 0));

    // ---- 下半区：文件栏 38% | 分隔线 | 便笺 62%（**固定比例，不可拖**）----
    ::SetWindowPos(g_hList, nullptr, leftX, top, leftW, bodyH,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(g_hSplit, nullptr, leftX + leftW, top, splitW, bodyH,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    int noteW = W - pad - rightX;
    if (noteW < Dp(60)) noteW = Dp(60);
    ::SetWindowPos(g_hNotes, nullptr, rightX, top, noteW, bodyH,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static void SyncDpi()
{
    UINT d = 0;
    typedef UINT (WINAPI* PFN)(HWND);
    if (HMODULE u = ::GetModuleHandleW(L"user32.dll")) {
        if (auto f = (PFN)::GetProcAddress(u, "GetDpiForWindow")) d = f(g_hWnd);
    }
    if (!d) {
        if (HDC dc = ::GetDC(nullptr)) {
            d = (UINT)::GetDeviceCaps(dc, LOGPIXELSY);
            ::ReleaseDC(nullptr, dc);
        }
    }
    if (d && d != g_dpi) { g_dpi = d; appui::SetDpi((int)d); ApplyFonts(); }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        // ⚠️ 必须先在这里把 g_hWnd 填上再建控件：CreateWindowExW 是"返回之前"
        //    就把 WM_CREATE 发出来的，外面那句赋值此刻还没执行。
        g_hWnd = hwnd;
        CreateChildren(hwnd);
        CreateTooltip(hwnd);
        AddTip(hwnd, g_hGear,     L"设置：打开配置文件（改完保存，点刷新生效）");
        AddTip(hwnd, g_hSync,     L"刷新：重读配置 + 确认连接 + 重拉列表");
        AddTip(hwnd, g_hInfo,     L"文件属性：名称 / 类型 / 大小 / 时间");
        AddTip(hwnd, g_hRename,   L"重命名（也可以选中后按 F2、或右击选「重命名」）");
        AddTip(hwnd, g_hDelete,   L"删除（真删，删了就没了；不弹确认框）");
        AddTip(hwnd, g_hUpload,   L"上传本地文件到当前目录（可多选，也可以直接把文件拖进来）");
        AddTip(hwnd, g_hDown,     L"下载选中项到本地「下载」文件夹（目录会递归下载）");
        AddTip(hwnd, g_hDlFolder, L"打开文件位置：打开本地「下载」文件夹并把选中项选出来");
        return 0;

    case WM_SIZE:
        SyncDpi();
        LayoutChildren(LOWORD(lp), HIWORD(lp));
        RepaintSubtree(hwnd);      // 改尺寸不会自动重画（R6：只作废）
        return 0;

    case WM_DPICHANGED: {
        g_dpi = (UINT)LOWORD(wp);
        appui::SetDpi((int)g_dpi);
        ApplyFonts();
        RECT* r = (RECT*)lp;
        if (r) {
            ::SetWindowPos(hwnd, nullptr, r->left, r->top,
                           r->right - r->left, r->bottom - r->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = Dp(360);
        mmi->ptMinTrackSize.y = Dp(200);
        return 0;
    }

    case WM_DROPFILES:
        // 拖到窗口空白处（不在文件栏上）也当作上传 —— 用户不会那么讲究落点
        HandleDroppedFiles((HDROP)wp);
        return 0;

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (!dis) break;

        if (dis->CtlID == IDC_FILELIST) { DrawListRow(*dis); return TRUE; }

        if (dis->CtlID == IDC_STATUS) {
            // 圆点 = 连接状态；文字 = 当前消息；忙的时候再多一条进度条。
            // 两者是**两条独立的状态线**（见 g_conn / g_msg 的注释）。
            ::FillRect(dis->hDC, &dis->rcItem, ::GetSysColorBrush(COLOR_BTNFACE));
            appui::DrawStatusLine(dis->hDC, dis->rcItem, ConnColor(g_conn),
                                  g_msg.c_str(), g_busy ? g_progressPct : -1);
            return TRUE;
        }

        // 文件操作那一排：**纯图标**（王 v1.8："都用图标显示"）。
        // 被点的那一个在传输中变身成**红框红叉**（= 取消）。
        // 图标都从 kFileBtns 来（绘制和布局共用同一份，不会错位）。
        for (const auto& b : kFileBtns) {
            if ((int)dis->CtlID != b.id) continue;   // CtlID 是 UINT，转一下再比（免得 C4389）
            if (IsBusySlot(b.id))
                appui::DrawGlyphButton(*dis, appui::DrawCancelGlyph, true);
            else
                appui::DrawGlyphButton(*dis, b.glyph, false);
            return TRUE;
        }

        // 右侧那两个保留图标（它们不是"文件操作"，一个扳手一个刷新箭头）
        if (dis->CtlID == IDC_GEAR) { appui::DrawTextButton(*dis, L"设置", appui::DrawWrenchGlyph,  false); return TRUE; }
        if (dis->CtlID == IDC_SYNC) { appui::DrawTextButton(*dis, L"刷新", appui::DrawRefreshGlyph, false); return TRUE; }
        break;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);

        // ⚠️ 「取消」要**排在最前面**判断：其余按钮此时都是禁用状态，
        //    唯一可能被按下的就是"变身后的那个"。
        if (code == BN_CLICKED && IsBusySlot(id)) {
            g_cancelReq = true;
            SetMessage(L"正在取消…（已收到的部分会被丢掉）");
            return 0;
        }

        if (code == BN_CLICKED) {
            switch (id) {
            case IDC_GEAR:     OpenConfigForUser();        return 0;
            case IDC_DLFOLDER: OpenLocalDownloadFolder();  return 0;
            case IDC_SYNC: {
                // ★ v1.7：「刷新」= 那套**统一的动作**（重读配置 + 确认 + 重拉列表）。
                //   顺手先把"连的是哪台服务器"亮一下（王的要求：优先显示域名）。
                //
                //   ⚠️ 为什么这里要用定时器兜一下、而不是直接往下走：
                //     若"刚刚才确认过"（限流闸门 1 秒），下面那句会立刻把这句话
                //     改写成"已刷新" —— 那句话存在的时间不到 1 毫秒，**眼睛根本看不见**。
                //     隔 350 毫秒再真的去刷新，用户才真的"看到了一眼服务器是谁"。
                const std::wstring srv = ServerLabel();
                SetMessage(srv.empty() ? L"正在刷新…" : (L"刷新 " + srv + L" …"));
                RepaintStatusArea();
                ::SetTimer(hwnd, kTimerRefreshShow, 350, nullptr);
                return 0;
            }
            case IDC_DOWNLOAD: StartDownload(); return 0;
            case IDC_UPLOAD:   StartUpload();   return 0;
            case IDC_INFO: {
                const std::vector<int> idxs = SelectedIdx();
                if (idxs.empty()) { SetMessage(L"先在文件栏里选中一项，再点「文件属性」"); return 0; }
                // 多选时看"焦点那一项"的属性（跟资源管理器一样：属性只给一个）
                const int idx = (g_sel >= 0 && g_sel < (int)g_files.size()) ? g_sel : idxs[0];
                ShowPropsAt(idx);
                return 0;
            }
            case IDC_RENAME: {
                const std::vector<int> idxs = SelectedIdx();
                if (idxs.empty()) { SetMessage(L"先在文件栏里选中一项，再点重命名"); return 0; }
                // 原地改名一次只能改一个 → 用"焦点那一项"
                const int idx = (g_sel >= 0 && g_sel < (int)g_files.size()) ? g_sel : idxs[0];
                StartRename(idx);
                return 0;
            }
            case IDC_DELETE:
                DeleteSelected();
                return 0;
            }
        }

        if (id == IDC_NOTES && code == EN_CHANGE) {
            ::InvalidateRect(g_hNotes, nullptr, FALSE);   // 空/非空的提示要跟着变
            if (!g_loading) {
                g_dirty = true;
                if (!g_busy) SetMessage(L"编辑中…");
                ::SetTimer(hwnd, kTimerAutoSync, 1500, nullptr);   // 重新计时
                // ★ 便笺发生操作 → 确认一次连接（过限流闸门：打字再快也只 1 秒一次）。
                //   注意这**只是探一次连接**，不碰文件栏（v1.9.1：两件事独立）。
                ConfirmOnly();
            }
            return 0;
        }

        if (id == IDC_FILELIST && (code == LBN_SELCHANGE || code == LBN_SELCANCEL)) {
            // 只记"焦点行"（给右键/属性用）；**真正要用哪些项**是按需读
            // LB_GETSELITEMS（支持多选）。单击不会动用户的文件。
            const int sel = (int)::SendMessageW(g_hList, LB_GETCURSEL, 0, 0) - (g_showUp ? 1 : 0);
            g_sel = (sel >= 0 && sel < (int)g_files.size()) ? sel : -1;
            UpdateButtonsEnabled();
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wp == kTimerAutoSync) {
            ::KillTimer(hwnd, kTimerAutoSync);
            if (g_dirty) DoSync(false);
            return 0;
        }
        if (wp == kTimerHeartbeat) {
            // ★ 每隔 heartbeat_sec 自动刷新一次（默认 300 = 5 分钟；0 = 关掉）
            //   走的是**文件栏那一路**（王说的"定时自动刷新"刷的就是列表）。
            RequestRefresh();
            return 0;
        }
        if (wp == kTimerRefreshShow) {
            // 见 IDC_SYNC 的注释：让"连的是哪台服务器"这句话先亮 350 毫秒
            ::KillTimer(hwnd, kTimerRefreshShow);
            UserRefresh();                   // 「刷新」= 便笺 + 文件栏的结合体（见该函数）
            return 0;
        }
        if (wp == kTimerConfirm) {
            // 被限流挡下的那次，现在补跑
            ::KillTimer(hwnd, kTimerConfirm);
            if (g_confirmPending) DoConfirm(g_confirmWantReload, g_confirmWantFull, g_confirmWantUser);
            return 0;
        }
        if (wp == kTimerBusyTick) {
            InvalidateStatus();      // 兜底：回调万一没来，进度条也不会僵住
            return 0;
        }
        break;

    case kMsgStartConnect:
        // 见 wWinMain 末尾：界面先摆出来，再到这里来连接（不冻界面）。
        Connect();
        return 0;

    case kMsgRenameCommit:
        EndRename(/*commit*/ true);
        return 0;

    case kMsgRenameCancel:
        EndRename(/*commit*/ false);
        return 0;

    case kMsgSplitterMove: {
        RECT cr{};
        ::GetClientRect(hwnd, &cr);
        LayoutChildren(cr.right, cr.bottom);
        RepaintSubtree(hwnd);
        return 0;
    }

    case kMsgSelfRepaint:
        RepaintSubtree(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC:
        ::SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);

    case WM_CTLCOLOREDIT: {
        // 便笺的文字色/背景色来自配置文件
        ::SetTextColor((HDC)wp, g_noteFg);
        ::SetBkColor((HDC)wp, g_noteBg);
        return (LRESULT)(g_hNoteBrush ? g_hNoteBrush
                                      : ::GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CTLCOLORLISTBOX:
        // owner-draw 的行我们自己画，但列表的**空白处**用的是这个画刷
        ::SetBkColor((HDC)wp, RGB(kListBgLevel, kListBgLevel, kListBgLevel));
        return (LRESULT)(g_hListBrush ? g_hListBrush
                                      : ::GetSysColorBrush(COLOR_BTNFACE));

    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    case WM_CLOSE:
        if (g_busy) {
            // 传输中不让直接关：先把传输取消掉，收尾之后再关（见 EndBusy）
            g_cancelReq = true;
            g_closePending = true;
            SetMessage(L"正在取消传输，取消完就关闭…");
            return 0;
        }
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hRenameEdit) EndRename(false);      // 没改完的名字别留着
        ::KillTimer(hwnd, kTimerAutoSync);
        ::KillTimer(hwnd, kTimerHeartbeat);
        ::KillTimer(hwnd, kTimerConfirm);
        ::KillTimer(hwnd, kTimerBusyTick);
        ::KillTimer(hwnd, kTimerRefreshShow);
        if (g_hUiFont)    { ::DeleteObject(g_hUiFont);    g_hUiFont = nullptr; }
        if (g_hNoteFont)  { ::DeleteObject(g_hNoteFont);  g_hNoteFont = nullptr; }
        if (g_hListFont)  { ::DeleteObject(g_hListFont);  g_hListFont = nullptr; }
        if (g_hSmallFont) { ::DeleteObject(g_hSmallFont); g_hSmallFont = nullptr; }
        if (g_hNoteBrush) { ::DeleteObject(g_hNoteBrush); g_hNoteBrush = nullptr; }
        if (g_hListBrush) { ::DeleteObject(g_hListBrush); g_hListBrush = nullptr; }
        TraceLog("窗口销毁，退出");
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// --askpass：给 ssh.exe 用的"问密码的程序"
//
// ⚠️⚠️ 这个判断**不能只看 argv**（v1.5.1 的血泪教训）：
//   ssh 拉 askpass 时只把提示串当 argv[1]（`ssh -vvv` 里看得到），
//   根本没有 `--askpass` 这种东西。所以真身要认"我是被叫来问密码的"，
//   只能靠我们自己设的**环境变量** NPPDOCK_ASKPASS_MODE（见 packcore MakePlan），
//   外加一条双保险：SSH_ASKPASS 指向的就是本 exe。
//   认不出来的后果很具体：那个实例会去建主窗口 → 点一次刷新弹一串窗口，
//   而且密码永远发不出去 → 服务器回 Permission denied（被误判成"密码错"）。
// ---------------------------------------------------------------------------
static bool AmITheAskpassChild()
{
    // ⚠️ 判据只能靠**环境变量**：ssh 拉起 askpass 时不给任何开关
    //    （`ssh -vvv` 里那行 `spawning "<exe>" "<prompt>"` 就是全部，没有 --askpass）。
    //
    //    三条信号**取或**（任一条成立即可）—— 曾经想"必须同时满足"更保险，
    //    结果直接认不出来 → 认证全线失败。宁可宽松：
    //      · NPPDOCK_ASKPASS_MODE：我们自己塞的（见 packcore MakePlan）
    //      · SSH_ASKPASS 指向本 exe：ssh 不一定把这份环境交给子进程，故只当补充
    //    共同前提：**必须拿到了密码**（NPPDOCK_ASKPASS_PW）。这样即便用户环境里
    //    残留了某个变量，只要没给密码也不会误判成 askpass（应用就不会"启动即退出"）。
    wchar_t pw[512]{};
    if (::GetEnvironmentVariableW(L"NPPDOCK_ASKPASS_PW", pw, _countof(pw)) == 0)
        return false;

    wchar_t mode[16]{};
    if (::GetEnvironmentVariableW(L"NPPDOCK_ASKPASS_MODE", mode, _countof(mode)) > 0)
        return true;

    wchar_t self[MAX_PATH * 2]{}, ap[MAX_PATH * 2]{};
    ::GetModuleFileNameW(nullptr, self, _countof(self));
    return ::GetEnvironmentVariableW(L"SSH_ASKPASS", ap, _countof(ap)) > 0 &&
           _wcsicmp(ap, self) == 0;
}

// 排障用：**只在"像是被 ssh 拉起来问密码、但我们没认出来"时**记一笔 ——
// 那一刻正是"弹出一串窗口 + 密码永远发不出去"的病根。
// 正常路径（认出来了）不记，免得每次都往轨迹日志里灌几行。
static void TraceAskpassEnv()
{
    wchar_t pw[512]{};
    if (::GetEnvironmentVariableW(L"NPPDOCK_ASKPASS_PW", pw, _countof(pw)) > 0)
        return;                                   // 正常：拿到了密码，会被认出
    wchar_t ap[MAX_PATH * 2]{};
    if (::GetEnvironmentVariableW(L"SSH_ASKPASS", ap, _countof(ap)) == 0)
        return;                                   // 连标记都没有，谈不上误判
    wchar_t self[MAX_PATH * 2]{};
    ::GetModuleFileNameW(nullptr, self, _countof(self));
    TraceLogW("⚠️ 疑似 askpass 但没有密码, SSH_ASKPASS=", ap);
    TraceLogW("   本 exe=", self);
}

static int RunAskPass()
{
    wchar_t pw[512]{};
    const DWORD n = ::GetEnvironmentVariableW(L"NPPDOCK_ASKPASS_PW", pw, _countof(pw));
    if (n == 0) return 1;
    // 末尾补一个换行：ssh 是按"读一行"取密码的，给个行尾最稳妥
    const std::string utf8 = U8FromW(pw) + "\n";
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD wrote = 0;
    if (out && out != INVALID_HANDLE_VALUE) ::WriteFile(out, utf8.data(),
                                                       (DWORD)utf8.size(), &wrote, nullptr);
    return 0;
}

// ---------------------------------------------------------------------------
// --selftest：按配置文件连一次、列目录，把结果打出来
// ---------------------------------------------------------------------------
static int RunSelfTest()
{
    bool created = false;
    packcfg::Load(g_cfg, &created);
    BuildTarget();
    RebuildBrushes();

    auto out = [](const std::wstring& s) {
        const std::string u = U8FromW(s) + "\n";
        DWORD wrote = 0;
        ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), u.data(), (DWORD)u.size(),
                    &wrote, nullptr);
    };

    out(L"=== 文件背包 自检 ===");
    out(L"配置文件：" + packcfg::ConfigPath() + (created ? L"（已生成默认值）" : L""));
    out(packcore::Describe(g_target));

    packcore::Res r = packcore::Probe(g_target);
    out(L"");
    out(std::wstring(L"[1] 连接/建目录：") + (packcore::Ok(r) ? L"OK" : L"失败"));
    if (!packcore::Ok(r)) { out(L"    " + r.msg); return 1; }

    std::vector<packcore::FileItem> files;
    r = packcore::List(g_target, L"", files);
    out(std::wstring(L"[2] 列目录：") + (packcore::Ok(r) ? L"OK" : L"失败"));
    if (!packcore::Ok(r)) { out(L"    " + r.msg); return 1; }
    out(L"    共 " + std::to_wstring(files.size()) + L" 项");
    for (size_t i = 0; i < files.size() && i < 20; ++i) {
        std::wstring line = L"      ";
        line += files[i].isDir ? L"[目录] " : L"       ";
        line += files[i].name;
        // 自检是**排障**用的，所以"内部文件"也照实列出来，但标一下 ——
        // 免得看到 .nppbackpack-trash 以为那个功能又回来了（v1.8 已经删掉）。
        if (packcfg::IsReservedName(files[i].name)) line += L"   ← 应用内部（界面上不显示）";
        if (!files[i].isDir) line += L"   " + FmtSize(files[i].size);
        line += L"   " + FmtTime(files[i].mtime);
        out(line);
    }

    out(L"");
    out(L"全部通过。回到应用里点右上角的刷新即可。");
    return 0;
}

// ---------------------------------------------------------------------------
// --sshraw：把探针那条 ssh 命令原样跑一遍，把原始输出打出来（排障用）
// ---------------------------------------------------------------------------
static int RunSshRaw()
{
    bool created = false;
    packcfg::Load(g_cfg, &created);
    BuildTarget();

    const std::wstring dump = packcore::Diagnose(g_target);
    const std::string u = U8FromW(dump);
    if (!u.empty()) {
        DWORD wrote = 0;
        ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), u.data(), (DWORD)u.size(),
                    &wrote, nullptr);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInst;

    // ★★ 必须在**建任何窗口之前**认出"我是被 ssh 拉起来问密码的那个实例"。
    //    判据见 AmITheAskpassChild()：ssh 不会给 askpass 传 --askpass，
    //    只看 argv 会漏认 → 弹窗 + 密码发不出去。
    TraceAskpassEnv();                       // 排障：把子进程看到的环境记一笔
    if (AmITheAskpassChild())
        return RunAskPass();

    // --askpass / --selftest / --sshraw 走命令行（手工排障用）
    // --askpass 也留着：手工复现 askpass 链路时用得到。
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (_wcsicmp(argv[i], L"--askpass") == 0) {
                    ::LocalFree(argv);
                    return RunAskPass();
                }
                if (_wcsicmp(argv[i], L"--selftest") == 0) {
                    ::LocalFree(argv);
                    return RunSelfTest();
                }
                if (_wcsicmp(argv[i], L"--sshraw") == 0) {
                    ::LocalFree(argv);
                    return RunSshRaw();
                }
            }
            ::LocalFree(argv);
        }
    }

    ::SetUnhandledExceptionFilter(CrashHandler);
    TraceLog("==== wWinMain enter ====");

    // R1：命令行解析（--dock-parent <hwnd>）
    HWND hDockParent = nullptr;
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i + 1 < argc; ++i) {
                if (_wcsicmp(argv[i], L"--dock-parent") == 0) {
                    hDockParent = reinterpret_cast<HWND>(
                        static_cast<UINT_PTR>(_wcstoui64(argv[i + 1], nullptr, 10)));
                    break;
                }
            }
            ::LocalFree(argv);
        }
    }
    g_embedded = (hDockParent != nullptr);

    // R2：DPI 感知必须在**建任何窗口之前**定下来（见 appui 的实现说明）
    appui::AdoptHostDpiAwareness(hDockParent);

    {
        typedef UINT (WINAPI* PFN)(HWND);
        UINT d = 0;
        if (HMODULE u = ::GetModuleHandleW(L"user32.dll")) {
            if (auto f = (PFN)::GetProcAddress(u, "GetDpiForWindow"))
                d = hDockParent ? f(hDockParent) : 0;
        }
        if (!d) {
            if (HDC dc = ::GetDC(nullptr)) {
                d = (UINT)::GetDeviceCaps(dc, LOGPIXELSY);
                ::ReleaseDC(nullptr, dc);
            }
        }
        g_dpi = d ? d : 96;
        appui::SetDpi((int)g_dpi);
    }
    TraceLog(g_embedded ? "embedded mode" : "standalone mode");

    // 配置要在建控件**之前**读：字体/配色都依赖它
    ReloadConfig();

    // ★ v1.9：告诉传输层"等 ssh.exe 的时候抽一次消息泵"。
    //   不设的话，所有等 ssh 的地方都是死等 —— 界面不重画、按钮点不动，
    //   表现就是"打开慢、像卡死"。（见 packcore::SetPump 的说明。）
    packcore::SetPump(&PumpMessages);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW sc{};
    sc.cbSize        = sizeof(sc);
    sc.lpfnWndProc   = SplitterProc;
    sc.hInstance     = hInst;
    sc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    sc.lpszClassName = kSplitterCls;
    sc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    ::RegisterClassExW(&sc);

    const DWORD style   = g_embedded ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = g_embedded ? WS_EX_TOOLWINDOW : 0;

    g_hWnd = ::CreateWindowExW(exStyle, kClassName, L"文件背包", style,
                               64, 64, 720, 440, nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) { TraceLog("CreateWindow 失败"); return 2; }

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
        if (w <= 0) w = 480;
        if (h <= 0) h = 300;
        ::SetWindowPos(g_hWnd, nullptr, 0, 0, w, h,
                       SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(g_hWnd, SW_SHOW);
    } else {
        RECT wa{};
        if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
            wa = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
        const int ww = Dp(760), wh = Dp(470);
        ::SetWindowPos(g_hWnd, nullptr, wa.left + (wa.right - wa.left - ww) / 2,
                       wa.top + (wa.bottom - wa.top - wh) / 3, ww, wh,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        ::ShowWindow(g_hWnd, SW_SHOW);
    }

    // 主窗口也接受拖入（落在文件栏以外的地方也算）
    ::DragAcceptFiles(g_hWnd, TRUE);
    AllowDropMessages(g_hWnd);

    ::UpdateWindow(g_hWnd);
    ::SetWindowTextW(g_hWnd, g_embedded ? L"文件背包（嵌入）" : L"文件背包");

    {
        RECT cr{};
        ::GetClientRect(g_hWnd, &cr);
        LayoutChildren(cr.right, cr.bottom);
    }

    // 首次运行：配置还是模板（没填服务器）就别去连 ——
    // 连一次要等 ConnectTimeout，白等十几秒会让用户以为这一页卡死了。
    if (packcfg::LooksUnconfigured(g_cfg)) {
        TraceLog("配置还是模板，跳过自动连接");
        SetConnState(Conn::Unknown);
        SetMessage(L"还没配置：点右上角 ⚙ 打开配置文件填服务器信息，再点 ↻ 刷新");
    } else {
        // ★ v1.9：**先把界面摆出来、再连接**（王：加载要快，过程要看得见）。
        //   以前是在进消息循环**之前**一口气连完（Probe + 读便笺 + 列目录 = 3 次 ssh），
        //   那几秒里窗口是"画出来了但冻着"的 —— 看起来就是"打开很慢、还没反应"。
        //   现在只 Post 一条消息，让消息循环先转起来：窗口立刻可交互，
        //   状态行会一步步写「正在连接 → 已连接，正在读取便笺 → 正在读取文件列表」。
        SetConnState(Conn::Unknown);
        SetMessage(L"正在连接…");
        RepaintStatusArea();
        ::PostMessageW(g_hWnd, kMsgStartConnect, 0, 0);
    }

    // ★ v1.6：心跳 —— 每 heartbeat_sec 秒自动确认一次连接状态
    //   （0 = 关掉；确认请求本身还有 1 秒的限流闸门，见 RequestConfirm）
    if (g_cfg.heartbeatSec > 0)
        ::SetTimer(g_hWnd, kTimerHeartbeat, (UINT)g_cfg.heartbeatSec * 1000u, nullptr);

    TraceLog("enter message loop");

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}
