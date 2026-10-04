// ============================================================================
// NppDock.cpp —— 主插件入口（Notepad++ 唯一加载的那个 DLL）
// ----------------------------------------------------------------------------
// 主 DLL 只做四件事：
//   1. 六个导出函数（Notepad++ 插件 ABI）
//   2. 菜单命令表（FuncItem）
//   3. 面板注册 / 注销（只注册**一个**面板到 CONT_BOTTOM）
//   4. 面板可见性的持久化与恢复
// 具体功能由 NppDock_XXX.dll 提供（容器扫目录自动发现）。
//
// 六个导出函数（顺序与签名必须与官方 PluginInterface.h 完全一致）：
//   setInfo(NppData)                 宿主注入句柄
//   getName()                        插件名（菜单里显示）
//   getFuncsArray(int*)              返回菜单命令表
//   beNotified(SCNotification*)      宿主通知
//   messageProc(UINT,WPARAM,LPARAM)  消息处理（本插件返回 1 = 已处理）
//   isUnicode()                      恒 TRUE
// ============================================================================

#include <windows.h>

#include <cstdlib>   // std::atoi（解析 panel.ini 里的 pageN 下标）
#include <string>
#include <vector>    // panel.ini 里记的"上次开着的应用页"列表

// ⚠️ 包含顺序有讲究：PluginInterface.h 的 beNotified 签名用到 SCNotification，
//    官方靠 Scintilla.h 提供它。本工程用精简版 NppDockScintilla.h，
//    因此**必须先包含它**，再包含 PluginInterface.h。
#include "NppDockScintilla.h"
#include "PluginInterface.h"
#include "Docking.h"
#include "Notepad_plus_msgs.h"
#include "NppDockApi.h"
#include "NppDockUtil.h"
#include "NppDockContainer.h"
#include "NppDockRes.h"

using namespace nppdock;

namespace {

NppData  g_nppData{};
Container g_container;
std::wstring g_pluginDllPath;   // 本插件 DLL 的完整路径
std::wstring g_pluginDir;       // 本插件所在目录（= plugins\NppDock\）
std::wstring g_configRoot;      // 宿主给的插件配置根
std::wstring g_logPath;         // 本插件的日志文件
std::wstring g_statePath;       // 面板状态文件（记住上次显示/隐藏）

bool g_panelRegistered = false;

// 面板可见性 —— **由我们自己维护**，不要用 IsWindowVisible 去问窗口。
// 原因：
//   1) NPPM_DMMSHOW/HIDE 操作的是宿主的"停靠栏"状态，而 hClient 只是被
//      接管的客户窗口，两者的 WS_VISIBLE 并不同步（尤其在 SetParent 之后）。
//   2) 宿主没有任何"查询面板可见性"的消息（SDK 里只有 SHOW/HIDE）。
// 所以：这是唯一可信的状态源，每次 SHOW/HIDE 之后必须同步它。
bool g_panelVisible = false;

// 菜单项 ID：用于在 beNotified / messageProc 里区分
enum {
    CMD_TOGGLE_PANEL = 0,
    CMD_RELOAD_MODULES,
    CMD_OPEN_LOG,
    CMD_COUNT
};

FuncItem g_funcItems[CMD_COUNT];

// 快捷键存储（FuncItem._pShKey 必须指向**持久**内存）
ShortcutKey g_scTogglePanel = { /*Ctrl*/ true, /*Alt*/ true, /*Shift*/ false, 'D' };

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
void LogInit()
{
    // 日志放在插件自己的配置目录下：
    //   <NppConfig>\plugins\config\NppDock\NppDock.log
    g_logPath = PathJoin(g_configRoot.empty()
                             ? PathJoin(g_pluginDir, L"config")
                             : g_configRoot,
                         L"NppDock\\NppDock.log");
    EnsureDirectory(DirNameOf(g_logPath));
    RotateLogIfNeeded(g_logPath);
}

void LogLine(int level, const std::wstring& msg)
{
    AppendLog(g_logPath, level, msg.c_str());
}

// ---------------------------------------------------------------------------
// 面板状态持久化
// ---------------------------------------------------------------------------
// 【为什么需要它】
//   实测结论：Notepad++ 启动时会把已注册的停靠面板**一律显示出来**，
//   即使 config.xml 里写着 <PluginDlg ... isVisible="no" />。
//   也就是说宿主虽然记录了可见性，却不尊重它（至少在便携版 8.x 上如此）。
//   所以"记住上次状态"必须由插件自己完成：
//     ① 每次切换后把状态写进自己的状态文件；
//     ② 启动注册面板后，如果上次是隐藏的，主动发 NPPM_DMMHIDE 收起来。
//
// 状态文件放在 <configRoot>\NppDock\panel.ini，一行一个 key=value（顺序无关）：
//   visible=0|1                  面板上次是显示还是隐藏
//   page0=NppDockApp_MD5.exe     上次开着的应用页，按页序（可有多行）
//   page1=NppDockApp_MD5.exe     ⚠️ v1.2 起**允许重复**：同一个应用可以开多页
//   current=1                    上次选中第几页（**页序号**，v1.2 起）
//
// ⚠️ 页面一律记 **exe 文件名**，不记下标：应用目录一增删，下标就漂了，
//    下次回来会打开**另一个**应用；文件名才是稳定的身份。
// ⚠️ 但 current 必须记**序号**：多开之后"文件名"不再是页的身份
//    （三页文件校验全叫 NppDockApp_MD5.exe），只有序号能定位到具体那一页。
//    老文件里的 current=<文件名> 仍然认（见 LoadPanelState 的双格式解析）。
// ---------------------------------------------------------------------------

// 最多记住多少页（防手改撑爆内存；正常使用远用不到）
constexpr int kMaxRememberedPages = 64;

// 什么时候**不许**写状态（两个都很隐蔽，别删）：
//   · 恢复过程中 —— 否则会把"只恢复了一半"的列表盖上去；
//   · 关闭过程中 —— 容器析构会走 closeAllPages()，那一刻列表已经空了，
//     再写一次就把用户真正想记的东西抹成空表。
bool g_stateFrozen = false;

struct PanelState {
    bool visible    = true;   // visible 的取值
    bool hasVisible = false;  // 文件里到底有没有这一项（用来区分"首次运行"）
    std::vector<std::wstring> pages;   // 上次开着的应用页（exe 文件名，可重复）
    std::wstring currentExe;           // 【老格式】上次选中的页（exe 文件名）
    int          currentIdx = -1;      // 【新格式】上次选中的页序号
};

// ★ 延迟恢复（懒加载）：启动时面板是隐藏的 → **先不恢复应用页**（不起任何应用
//   进程），等面板第一次真被打开再恢复。这样"上次关着 dock 退出"的情况下
//   打开 N++ 不会被几个嵌入应用拖慢 —— 否则懒加载就没意义了。
//   ⚠️ 期间 SavePanelState 必须**保留文件里原来的页列表**：此刻内存里的列表
//      还是空的，照常写盘就把用户上次开着的页抹掉了。
bool        g_pagesRestorePending = false;
PanelState  g_pendingState;        // 延迟恢复用的那份"上次开着哪些页"

// 工具栏图标/位图的句柄（注册时加载，退出时销毁；全程只一份）
HICON   g_toolbarIcon     = nullptr;
HICON   g_toolbarIconDark = nullptr;
HBITMAP g_toolbarBmp      = nullptr;

std::wstring StateFilePath()
{
    return PathJoin(g_configRoot.empty()
                        ? PathJoin(g_pluginDir, L"config")
                        : g_configRoot,                    L"NppDock\\panel.ini");
}

void SavePanelState()
{
    if (g_stateFrozen) return;
    if (g_statePath.empty()) return;
    EnsureDirectory(DirNameOf(g_statePath));

    std::string content = g_panelVisible ? "visible=1\r\n" : "visible=0\r\n";

    if (g_pagesRestorePending) {
        // ★ 延迟恢复期间**绝不能**用"当前页列表"覆盖文件 —— 此刻页还没恢复，
        //   列表是空的，一覆盖就把用户上次开着的页抹掉了。
        //   这段时间只更新 visible=，页列表原样抄回来。
        std::string old;
        if (ReadFileAll(g_statePath, old)) {
            size_t pos = 0;
            while (pos < old.size()) {
                size_t eol = old.find('\n', pos);
                if (eol == std::string::npos) eol = old.size();
                std::string line = old.substr(pos, eol - pos);
                pos = eol + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                if (line.rfind("page", 0) == 0 || line.rfind("current=", 0) == 0) {
                    content += line + "\r\n";
                }
            }
        }
        if (!WriteFileAtomic(g_statePath, content.data(), content.size())) {
            LogLine(NPPDOCK_LOG_WARN, L"写入面板状态失败：" + g_statePath);
        }
        return;
    }

    const std::vector<std::wstring> pages = g_container.openedAppExeNames();
    // ⚠️ v2.0：保存端也要按 kMaxRememberedPages 截断。以前保存不限、读取只认 64，
    //    开了 65 页再重启就会发现"最后那页不见了"，而且没人知道为什么。
    const size_t nPages = pages.size() < (size_t)kMaxRememberedPages
                              ? pages.size() : (size_t)kMaxRememberedPages;
    for (size_t i = 0; i < nPages; ++i) {
        content += "page" + std::to_string(i) + "=" + ToUtf8(pages[i]) + "\r\n";
    }
    // ⚠️ v1.2 起 current 记的是**页序号**，不再是 exe 文件名。
    //    原因：同一个应用允许开多页了，文件名不再是页的唯一身份 ——
    //    三页文件校验在 panel.ini 里全是 NppDockApp_MD5.exe，记了等于没记，
    //    重启后会停在"随便哪一页"上。页序号才是页的身份。
    //    （老文件的 current=<文件名> 仍然认，见 LoadPanelState。）
    const int cur = g_container.currentPage();
    if (cur >= 0 && cur < (int)nPages) {
        content += "current=" + std::to_string(cur) + "\r\n";
    }

    // 原子写：先 .tmp 再整体替换 —— 直接截断写的话，写到一半断电就只剩半截文件。
    if (!WriteFileAtomic(g_statePath, content.data(), content.size())) {
        LogLine(NPPDOCK_LOG_WARN, L"写入面板状态失败：" + g_statePath);
    }
}

// 返回 true 表示读到了明确状态；false 表示没有记录（首次运行）
bool LoadPanelState(PanelState& out)
{
    if (g_statePath.empty() || !FileExists(g_statePath)) return false;

    std::string raw;
    if (!ReadFileAll(g_statePath, raw)) return false;

    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();

        std::string line = raw.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("visible=", 0) == 0) {
            // "visible=" 本身长 8，值从下标 8 开始。
            // ⚠️ 这里曾写成 find('1', 9)，而下标 9 已经越过行尾（"visible=1" 只有 9 字节），
            //    于是永远匹配不到，任何状态都被判成 false —— 表现为"永远恢复成隐藏"。
            out.visible    = (line.find('1', 8) != std::string::npos);
            out.hasVisible = true;
        } else if (line.rfind("current=", 0) == 0) {
            // 两种格式都认（向后兼容老文件）：
            //   current=2                 ← v1.2 起：页序号
            //   current=NppDockApp_MD5.exe ← 老格式：exe 文件名
            // 判据：整串都是数字就当序号，否则当文件名。
            // （"全是数字"这个判据足够安全 —— Windows 文件名不可能是纯数字，
            //   它一定有扩展名 .exe。）
            const std::string v = line.substr(8);
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
                const long idx = std::atol(v.c_str());
                if (idx >= 0 && idx < kMaxRememberedPages) out.currentIdx = (int)idx;
            } else {
                out.currentExe = ToWide(v);
            }
        } else if (line.rfind("page", 0) == 0) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos && eq > 4) {
                const int idx = std::atoi(line.substr(4, eq - 4).c_str());
                // 防呆：手改坏了也不能撑爆内存
                if (idx >= 0 && idx < kMaxRememberedPages) {
                    if ((int)out.pages.size() <= idx) out.pages.resize(idx + 1);
                    out.pages[idx] = ToWide(line.substr(eq + 1));
                }
            }
        }

        if (eol >= raw.size()) break;
        pos = (raw[eol] == '\r') ? eol + 2 : eol + 1;
    }
    return out.hasVisible;
}

// ---------------------------------------------------------------------------
// 显示 / 隐藏面板
// ---------------------------------------------------------------------------
// 【曾经的 bug】旧实现里开头有一句无条件的 NPPM_DMMSHOW，导致
//   "关掉→打开→关掉→打开" 全部塌缩成"永远在显示"，表现为关掉后再也打不开。
// 【正确做法】自己维护 g_panelVisible 作为唯一状态源；动作后同步它。
void TogglePanel()
{
    if (!g_container.hwnd() || !g_panelRegistered) {
        LogLine(NPPDOCK_LOG_WARN, L"面板尚未注册，忽略切换请求");
        return;
    }

    const HWND h = g_container.hwnd();

    // ★ 先与"真实可见性"对账，再决定动作。
    //   用户可能用面板自己的 ✕ 按钮关掉面板，而宿主**不会**为此发任何通知
    //   （SDK 里没有"面板被关闭"事件）。只信缓存会出现"明明关着，却以为它开着"。
    const bool reallyVisible = g_container.isPanelVisible() ? true : false;
    if (reallyVisible != g_panelVisible) {
        LogLine(NPPDOCK_LOG_INFO, L"面板可见性与缓存状态不一致，按实际状态校正");
        g_panelVisible = reallyVisible;
    }

    if (g_panelVisible) {
        ::SendMessageW(g_nppData._nppHandle, NPPM_DMMHIDE, 0, (LPARAM)h);
        g_panelVisible = false;
        g_container.onHide();          // 通知当前页"不可见了"（**不销毁视图**）
        LogLine(NPPDOCK_LOG_INFO, L"面板已隐藏");
    } else {
        ::SendMessageW(g_nppData._nppHandle, NPPM_DMMSHOW, 0, (LPARAM)h);
        g_panelVisible = true;
        g_container.onShow();          // 通知当前页"可见了"（**不销毁视图**）
        g_container.layout();
        LogLine(NPPDOCK_LOG_INFO, L"面板已显示");
    }

    SavePanelState();   // 记住这次的状态，下次启动恢复
}

// 前向声明：这些函数互相调用，而定义顺序是按"讲故事的顺序"排的
// （状态 -> 面板 -> 恢复），所以需要提前打个招呼。
void RestoreAppPages(const PanelState& st);

// ---------------------------------------------------------------------------
// 懒加载的第二步：面板第一次被打开时，把上次开着、但启动时没恢复的应用页补上
// ---------------------------------------------------------------------------
// 为什么要有这一步：启动时面板若是隐藏的，我们**故意不恢复**应用页
// （每恢复一页就得起一个进程，而面板根本没显示 —— 白拖慢 N++ 启动）。
// 但"不恢复"不等于"忘掉"：页列表一直躺在 panel.ini 里（SavePanelState 在
// 这段期间会原样保留它），等面板真被打开时再恢复。
void RestorePendingPages()
{
    if (!g_pagesRestorePending) return;

    // ⚠️ 先清标志**再**恢复：RestoreAppPages 结尾会 SavePanelState，
    //    那时必须已经是"正常模式"，否则又会把页列表按"待恢复"处理。
    g_pagesRestorePending = false;
    PanelState st = g_pendingState;
    g_pendingState = PanelState{};

    LogLine(NPPDOCK_LOG_INFO, L"面板已打开：开始延迟恢复上次的应用页");
    RestoreAppPages(st);
}

// 容器通知"面板被显示出来了"（含外部直接 DMMSHOW 这条路，见 setPanelShownHook）
void OnPanelShown()
{
    RestorePendingPages();
}

// ---------------------------------------------------------------------------
// 工具栏按钮：把「显示/隐藏 NppDock 面板」挂到 Notepad++ 工具栏上
// ---------------------------------------------------------------------------
// 图标三份（浅色 ICO / 深色 ICO / BMP）缺一不可 —— 见 NppDock.rc 的说明。
// 命令 ID 用 FuncItem 里那一项自己的 _cmdID（宿主按它把按钮点击派回 _pFunc）。
void AddToggleButtonToToolbar()
{
    // 本 DLL 的模块句柄：本插件没有 DllMain，用"本函数地址"反查最可靠
    // （不要用 GetModuleHandle(nullptr)：那拿到的是宿主 notepad++.exe）。
    HMODULE hMod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&AddToggleButtonToToolbar),
                              &hMod) || !hMod) {
        LogLine(NPPDOCK_LOG_WARN, L"工具栏：取本模块句柄失败，按钮未注册");
        return;
    }

    if (!g_toolbarIcon)
        g_toolbarIcon = (HICON)::LoadImageW(hMod, MAKEINTRESOURCEW(IDI_NPPDOCK),
                                            IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!g_toolbarIconDark)
        g_toolbarIconDark = (HICON)::LoadImageW(hMod, MAKEINTRESOURCEW(IDI_NPPDOCK_DARK),
                                                IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if (!g_toolbarBmp)
        g_toolbarBmp = (HBITMAP)::LoadImageW(hMod, MAKEINTRESOURCEW(IDB_NPPDOCK),
                                             IMAGE_BITMAP, 0, 0, LR_DEFAULTSIZE);

    if (!g_toolbarIcon || !g_toolbarIconDark || !g_toolbarBmp) {
        LogLine(NPPDOCK_LOG_WARN, L"工具栏：图标资源加载不全，按钮可能显示空白");
    }

    toolbarIconsWithDarkMode icons{};
    icons.hToolbarBmp          = g_toolbarBmp;
    icons.hToolbarIcon         = g_toolbarIcon;
    icons.hToolbarIconDarkMode = g_toolbarIconDark;

    const WPARAM cmd = (WPARAM)g_funcItems[CMD_TOGGLE_PANEL]._cmdID;
    BOOL ok = (BOOL)::SendMessageW(g_nppData._nppHandle,
                                   NPPM_ADDTOOLBARICON_FORDARKMODE,
                                   cmd, (LPARAM)&icons);
    if (!ok) {
        // 宿主对 cmdID 的约定在不同版本上有过"从 0 还是从 1 数"的差异，
        // 兜一次 +1；两次都失败才认输（并且把情况写进日志，别静默）。
        ok = (BOOL)::SendMessageW(g_nppData._nppHandle,
                                  NPPM_ADDTOOLBARICON_FORDARKMODE,
                                  cmd + 1, (LPARAM)&icons);
        if (ok) LogLine(NPPDOCK_LOG_WARN, L"工具栏按钮用 _cmdID+1 才注册成功");
    }

    LogLine(ok ? NPPDOCK_LOG_INFO : NPPDOCK_LOG_WARN,
            ok ? L"工具栏按钮已注册（显示/隐藏 NppDock 面板）"
               : L"工具栏按钮注册失败（宿主两次都返回 FALSE）");
}

void ReloadModules()
{
    LogLine(NPPDOCK_LOG_INFO, L"=== 手动重新扫描功能模块 ===");
    if (!g_container.hwnd()) return;

    g_container.destroy();
    g_container.create();
    g_container.setHostHandles(g_nppData._nppHandle,
                               g_nppData._scintillaMainHandle,
                               g_nppData._scintillaSecondHandle);
    g_container.buildHostApi();
    g_container.setPaths(g_pluginDir, g_configRoot, g_logPath);
    g_container.discoverModules(g_pluginDir);
    g_container.discoverApps(g_pluginDir);
    g_container.rebuildTabs();

    // 重新注册面板（容器窗口是新建的，hClient 变了，必须重新注册一次）
    DockedWidgetData tb{};
    tb.hClient       = g_container.hwnd();
    tb.pszName       = L"NppDock";
    tb.dlgID         = (int)CMD_TOGGLE_PANEL;
    tb.uMask         = DWS_DF_CONT_BOTTOM;
    tb.pszModuleName = L"NppDock.dll";
    ::SendMessageW(g_nppData._nppHandle, NPPM_DMMREGASDCKDLG, 0, (LPARAM)&tb);

    // 重注册后宿主会显示新面板；按上次状态修正
    g_panelVisible = true;
    if (!g_container.isPanelVisible()) g_panelVisible = false;
    LogLine(NPPDOCK_LOG_INFO,
            g_panelVisible ? L"模块重扫描完毕，面板保持显示"
                           : L"模块重扫描完毕，面板保持隐藏");
}

void OpenLog()
{
    if (!FileExists(g_logPath)) {
        LogLine(NPPDOCK_LOG_WARN, L"日志文件还不存在，先写一条占位记录");
    }
    OpenWithDefaultApp(g_logPath);
}

// ---------------------------------------------------------------------------
// 菜单表初始化
// ---------------------------------------------------------------------------
void InitFuncItems()
{
    auto set = [](int i, const wchar_t* name, PFUNCPLUGINCMD fn, ShortcutKey* sk) {
        wcsncpy_s(g_funcItems[i]._itemName, menuItemSize, name, _TRUNCATE);
        g_funcItems[i]._pFunc      = fn;
        g_funcItems[i]._cmdID      = i;
        g_funcItems[i]._init2Check = false;
        g_funcItems[i]._pShKey     = sk;
    };

    set(CMD_TOGGLE_PANEL,   L"显示/隐藏 NppDock 面板", &TogglePanel,   &g_scTogglePanel);
    set(CMD_RELOAD_MODULES, L"重新扫描功能模块",       &ReloadModules, nullptr);
    set(CMD_OPEN_LOG,       L"打开运行日志",           &OpenLog,       nullptr);
}

// ---------------------------------------------------------------------------
// 面板注册
// ---------------------------------------------------------------------------
bool RegisterPanel(const PanelState& st)
{
    if (!g_nppData._nppHandle || !g_container.hwnd()) return false;

    // 向宿主声明"这个窗口是我的无模式对话框"，Notepad++ 会接管它的
    // 消息循环与 Tab 键处理。
    ::SendMessageW(g_nppData._nppHandle, NPPM_MODELESSDIALOG,
                   MODELESSDIALOGADD, (LPARAM)g_container.hwnd());

    DockedWidgetData tb{};
    tb.hClient       = g_container.hwnd();
    tb.pszName       = L"NppDock";     // 面板显示名
    tb.dlgID         = (int)CMD_TOGGLE_PANEL;

    // -----------------------------------------------------------------------
    // ⚠️⚠️ uMask 的高 4 位 = "首次注册时希望停靠在哪个容器"。
    //
    //   曾经踩过的坑：这里写 0，结果面板停到了**左边**而不是下面。
    //   原因：0 的语义是"未指定默认容器"，Notepad++ 的兜底值恰好就是
    //        CONT_LEFT(0)，于是表现为"莫名其妙跑到左边"。
    //   而且 CONT_LEFT 本身就等于 0，所以"没填"和"填左边"完全无法区分。
    //
    //   正确做法：必须显式填 DWS_DF_CONT_BOTTOM（= CONT_BOTTOM << 28
    //   = 0x30000000）。这 4 位只在"该面板尚无持久化记录"时生效；
    //   一旦 config.xml 里已有 <PluginDlg .../> 记录，宿主就优先用记录的
    //   位置（curr 属性）。
    // -----------------------------------------------------------------------
    tb.uMask         = DWS_DF_CONT_BOTTOM;

    tb.hIconTab      = nullptr;
    tb.pszAddInfo    = nullptr;
    // ⚠️ 必须填**本插件 DLL 的文件名**（不是面板标题、不是任意标识符）。
    //    宿主用它在 config.xml 里持久化停靠位置与可见性。
    tb.pszModuleName = L"NppDock.dll";

    BOOL ok = (BOOL)::SendMessageW(g_nppData._nppHandle,
                                   NPPM_DMMREGASDCKDLG, 0, (LPARAM)&tb);
    if (!ok) {
        LogLine(NPPDOCK_LOG_ERROR, L"NPPM_DMMREGASDCKDLG 返回失败");
        return false;
    }

    g_panelRegistered = true;

    // 注册后宿主会把面板显示出来（实测：即使 config.xml 写着 isVisible="no"
    // 也照显不误）。所以这里先按"已显示"对齐缓存，再按上次状态修正。
    g_panelVisible = true;

    // ★ 恢复上次状态：如果上次是隐藏的，就主动收起来。
    if (st.hasVisible) {
        if (!st.visible) {
            ::SendMessageW(g_nppData._nppHandle, NPPM_DMMHIDE, 0,
                           (LPARAM)g_container.hwnd());
            g_panelVisible = false;
            g_container.onHide();
            LogLine(NPPDOCK_LOG_INFO, L"已按上次状态恢复：面板保持隐藏");
        } else {
            LogLine(NPPDOCK_LOG_INFO, L"已按上次状态恢复：面板保持显示");
        }
    } else {
        // 首次运行：默认显示（并立刻记下来，之后就有据可依）
        LogLine(NPPDOCK_LOG_INFO, L"首次运行，无历史状态，面板默认显示");
        SavePanelState();
    }

    // 日志如实打印实际提交的 uMask，别写死字面串（那会掩盖问题）
    {
        wchar_t buf[160];
        swprintf_s(buf, L"面板注册成功（uMask=0x%08X，期望 0x%08X = CONT_BOTTOM）",
                   (unsigned)tb.uMask, (unsigned)DWS_DF_CONT_BOTTOM);
        LogLine(NPPDOCK_LOG_INFO, buf);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 恢复上次开着的应用页
// ---------------------------------------------------------------------------
// ⚠️ 必须在 discoverApps() 之后调用 —— 要先知道"目录里有哪些应用"，
//    才能按 exe 文件名找回它的下标。
//
// 期间每打开一页都会触发 pagesChanged 钩子，所以用 g_stateFrozen 把落盘压住，
// 全部恢复完再统一补存一次（否则中途每个文件都会被写成"只恢复了一半"的列表）。
// ---------------------------------------------------------------------------
void RestoreAppPages(const PanelState& st)
{
    if (st.pages.empty()) {
        LogLine(NPPDOCK_LOG_INFO, L"上次没有开着应用页，无需恢复");
        return;
    }

    g_stateFrozen = true;

    int restored   = 0;
    int missing    = 0;
    int curPageIdx = -1;

    for (const std::wstring& exeName : st.pages) {
        if (exeName.empty()) continue;   // 手改文件留下的空项

        const int appIdx = g_container.findAppIndexByExeName(exeName);
        if (appIdx < 0) {
            // 应用被删了/改名了：跳过就好，不能因为一个缺失让整批恢复都失败
            LogLine(NPPDOCK_LOG_WARN, L"恢复应用页：找不到 " + exeName + L"，已跳过");
            ++missing;
            continue;
        }

        const int pageIdx = g_container.openAppPage(appIdx);
        if (pageIdx < 0) {
            LogLine(NPPDOCK_LOG_WARN, L"恢复应用页失败（进程没起来）：" + exeName);
            ++missing;
            continue;
        }
        ++restored;
        if (!st.currentExe.empty() &&
            _wcsicmp(exeName.c_str(), st.currentExe.c_str()) == 0) {
            curPageIdx = pageIdx;
        }
    }

    // 回到上次选中的那一页。新格式（页序号）优先 —— 只有它能在
    // "同一个应用开多页"时区分到底是哪一页；老格式（文件名）只是兼容。
    int want = -1;
    if (st.currentIdx >= 0 && st.currentIdx < g_container.pageCount()) {
        want = st.currentIdx;
    } else if (curPageIdx >= 0 && curPageIdx < g_container.pageCount()) {
        want = curPageIdx;
    }
    if (want >= 0) {
        g_container.switchToPage(want);
    }

    g_stateFrozen = false;

    wchar_t buf[160];
    swprintf_s(buf, L"应用页恢复完成：成功 %d 个，跳过 %d 个", restored, missing);
    LogLine(restored > 0 ? NPPDOCK_LOG_INFO : NPPDOCK_LOG_WARN, buf);

    // 把"实际恢复出来的"列表固化下来：下次就不用再去尝试那些已经不存在的应用了。
    SavePanelState();
}

void UnregisterPanel()
{
    if (!g_panelRegistered) return;
    if (g_nppData._nppHandle && g_container.hwnd()) {
        ::SendMessageW(g_nppData._nppHandle, NPPM_MODELESSDIALOG,
                       MODELESSDIALOGREMOVE, (LPARAM)g_container.hwnd());
    }
    g_panelRegistered = false;
}

} // namespace

// ===========================================================================
// 六个导出函数
// ===========================================================================
extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    g_nppData = data;

    g_pluginDllPath = GetOwnModulePath();
    g_pluginDir     = StripTrailingSlash(DirNameOf(g_pluginDllPath));

    // 拿宿主给的插件配置目录（对应 Notepad++ 的 plugins\config）
    wchar_t cfg[1024] = {};
    if (::SendMessageW(g_nppData._nppHandle, NPPM_GETPLUGINSCONFIGDIR,
                       (WPARAM)_countof(cfg), (LPARAM)cfg) && cfg[0]) {
        g_configRoot = StripTrailingSlash(cfg);
    } else {
        g_configRoot = PathJoin(g_pluginDir, L"config");
    }

    LogInit();
    g_statePath = StateFilePath();

    LogLine(NPPDOCK_LOG_INFO, L"=============================");
    LogLine(NPPDOCK_LOG_INFO, L"setInfo 被调用");
    LogLine(NPPDOCK_LOG_INFO, L"  插件目录    = " + g_pluginDir);
    LogLine(NPPDOCK_LOG_INFO, L"  配置根      = " + g_configRoot);
    LogLine(NPPDOCK_LOG_INFO, L"  日志        = " + g_logPath);
    LogLine(NPPDOCK_LOG_INFO, L"  状态文件    = " + g_statePath);

    InitFuncItems();
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"NppDock";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* nbF)
{
    *nbF = CMD_COUNT;
    return g_funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* n)
{
    if (!n) return;

    switch (n->nmhdr.code) {
    // ---------------------------------------------------------------
    // 宿主初始化完成 —— 这是**唯一**能安全注册停靠面板的时机。
    // 更早（如 setInfo 里）宿主自己的 Docking Manager 还没建好。
    // ---------------------------------------------------------------
    case NPPN_READY: {
        // 幂等保护：万一 NPPN_READY 重复到达，绝不能再建一套窗口。
        if (g_container.hwnd()) {
            LogLine(NPPDOCK_LOG_WARN, L"NPPN_READY 重复到达，忽略");
            break;
        }

        LogLine(NPPDOCK_LOG_INFO, L"NPPN_READY：开始创建容器与面板");

        // ⚠️ 顺序很关键：**先** setPaths（把 _logPath 设进去），
        //    否则容器内部任何早期日志都写不出去 —— 而插件加载失败是完全静默的，
        //    日志就是唯一排查手段。
        g_container.setPaths(g_pluginDir, g_configRoot, g_logPath);

        // 1) 建容器
        if (!g_container.create()) {
            LogLine(NPPDOCK_LOG_ERROR, L"容器创建失败，插件无法工作");
            return;
        }

        g_container.setHostHandles(g_nppData._nppHandle,
                                   g_nppData._scintillaMainHandle,
                                   g_nppData._scintillaSecondHandle);
        g_container.buildHostApi();

        // 2) 把容器挂到宿主主窗口下（Docking Manager 接管前先有个父窗口，
        //    否则某些 Windows 版本上子窗口会跑到桌面）
        ::SetParent(g_container.hwnd(), g_nppData._nppHandle);

        // 3) 发现功能模块（第一级懒加载：此刻 LoadLibrary 拿标题）
        g_container.discoverModules(g_pluginDir);

        // 4) 发现可嵌入应用（只读文件名 + 版本资源，**不启动任何进程**）
        g_container.discoverApps(g_pluginDir);

        // 5) 建页并默认选中第 0 页（第二级懒加载：此刻才 createView）
        g_container.rebuildTabs();

        // 6) 挂上"页面变了"的通知：之后每次打开/关闭/切换页都会自动落盘。
        //    放在这里（而不是更早）是因为它调的就是 SavePanelState，
        //    而那个函数依赖 g_container 已经能报出页面列表。
        g_container.setPagesChangedHook(&SavePanelState);

        // 7) 读一次状态：显隐交给 RegisterPanel，页面列表留给后面恢复用。
        PanelState st;
        LoadPanelState(st);

        // 8) 注册面板，并按上次状态恢复显隐
        RegisterPanel(st);

        // 9) 恢复上次开着的应用页。
        //    ⚠️ 必须排在 discoverApps() 之后 —— 要先能按 exe 名查到下标。
        //    ★ 懒加载：**只有面板这次是显示的**才恢复。上次是关着的（用户点了
        //      隐藏或面板的 ✕）就不恢复 —— 那几页各自要起一个进程，
        //      而面板根本没显示出来，白拖慢启动。等面板真被打开再补恢复
        //      （见 g_pagesRestorePending / setPanelShownHook）。
        if (g_panelVisible) {
            RestoreAppPages(st);
        } else {
            g_pagesRestorePending = true;
            g_pendingState        = st;
            LogLine(NPPDOCK_LOG_INFO,
                    L"上次面板是隐藏/关闭状态，本次不恢复应用页（懒加载），"
                    L"等面板被打开时再恢复");
        }

        // 10) 挂"面板被显示"的钩子 —— 延迟恢复就靠它。
        //     ⚠️ 必须挂在**恢复决策之后**：注册面板时宿主会显示一次面板，
        //        那一瞬间钩子若已生效，就会在"本次该不该恢复"还没判定时先跑起来。
        g_container.setPanelShownHook(&OnPanelShown);

        LogLine(NPPDOCK_LOG_INFO, L"NPPN_READY 处理完毕");
        break;
    }

    // ---------------------------------------------------------------
    // 宿主允许注册工具栏图标。往工具栏加一个"显示/隐藏 NppDock 面板"按钮。
    // ---------------------------------------------------------------
    case NPPN_TBMODIFICATION: {
        AddToggleButtonToToolbar();
        break;
    }

    // ---------------------------------------------------------------
    // 宿主即将关闭。NPPN_BEFORESHUTDOWN 先来，NPPN_SHUTDOWN 后到。
    // 我们在 SHUTDOWN 里做真正清理 —— 那时窗口树还完整。
    // ---------------------------------------------------------------
    case NPPN_BEFORESHUTDOWN:
        LogLine(NPPDOCK_LOG_INFO, L"NPPN_BEFORESHUTDOWN：准备清理");
        break;

    case NPPN_SHUTDOWN: {
        LogLine(NPPDOCK_LOG_INFO, L"NPPN_SHUTDOWN：开始销毁");

        // ★★ 关闭前**先与真实可见性对账**，再落盘。
        //    这一句就是"点了面板的 ✕ 再退出，下次还弹出来"那个 bug 的解药：
        //    ✕ 是宿主停靠框架的按钮，SDK **不会**为此发任何通知，
        //    我们的 g_panelVisible 缓存还停在"显示"，直接存盘就记错了。
        //    关掉这一刻的窗口树还完整，问得出真实状态（沿父链判可见性）。
        if (g_container.hwnd()) {
            const bool reallyVisible = g_container.isPanelVisible();
            if (reallyVisible != g_panelVisible) {
                LogLine(NPPDOCK_LOG_INFO,
                        L"退出前发现可见性与缓存不一致，按实际状态落盘");
                g_panelVisible = reallyVisible;
            }
        }

        SavePanelState();

        // ★ 之后一律冻结写盘：下面的 destroy() 会走 closeAllPages()，
        //   那一刻页面列表已经清空，再存一次就把上面刚记的抹成空表。
        g_stateFrozen = true;

        UnregisterPanel();

        // 先把面板从宿主布局里摘掉（避免宿主销毁过程中访问已销毁的窗口）
        if (g_nppData._nppHandle && g_container.hwnd()) {
            ::SendMessageW(g_nppData._nppHandle, NPPM_DMMHIDE, 0,
                           (LPARAM)g_container.hwnd());
        }

        // 容器析构会逆序卸载所有模块
        g_container.destroy();

        // 工具栏图标句柄：注册时 LoadImage 换来的，退出时还回去
        // （不还也不会崩，但"每次加载插件漏三份 GDI 对象"会在反复启停后累积）
        if (g_toolbarIcon)     { ::DestroyIcon(g_toolbarIcon);        g_toolbarIcon = nullptr; }
        if (g_toolbarIconDark) { ::DestroyIcon(g_toolbarIconDark);    g_toolbarIconDark = nullptr; }
        if (g_toolbarBmp)      { ::DeleteObject(g_toolbarBmp);        g_toolbarBmp = nullptr; }

        LogLine(NPPDOCK_LOG_INFO, L"NPPN_SHUTDOWN：销毁完毕");
        LogLine(NPPDOCK_LOG_INFO, L"=============================");
        break;
    }

    default:
        break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT /*Message*/,
                                                     WPARAM /*wParam*/,
                                                     LPARAM /*lParam*/)
{
    // 返回值语义：1 = 已处理（宿主不再继续分发），0 = 未处理。
    return 1;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}
