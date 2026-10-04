// ============================================================================
// NppDockContainer.h —— 容器窗口（我们唯一注册给 Notepad++ 的面板）
// ----------------------------------------------------------------------------
// 只向 Notepad++ 注册**一个**面板（CONT_BOTTOM），它的客户窗口就是本容器。
//
// 容器有线两种"页"：
//   1) 模块页 —— NppDock_*.dll 提供的进程内视图（早期机制，保留）
//   2) 应用页 —— NppDockApp_*.exe，由本容器拉起进程、把它的窗口嵌进来
//
// 应用页是本文件新增的重点。它的存在感完全由用户驱动：
//   · tab 条**常驻**（哪怕一个应用页都没有）—— 它是"添加应用"的唯一入口；
//   · 在 **tab 条的空白处**右击 -> 弹出应用菜单 -> 选中某个应用 -> 多出一个 tab；
//   · 在 tab 项上右击 -> 关闭该页；
//   · 内容区（空白页）右击**什么都不做**（用户明确要求）。
//
// 为什么"默认必须空白"：
//   用户明确要求面板不要自己弹东西出来。所以这里**绝不自动打开任何应用**，
//   一切都要用户右击点选。
//
// 两级懒加载：
//   第一级 —— 只扫描文件名，不加载/不启动任何东西；
//   第二级 —— 某页第一次被选中时才 LoadLibrary + createView（模块）
//             或 CreateProcess + SetParent（应用）。
// ============================================================================
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

#include "NppDockApi.h"
#include "NppDockUtil.h"
#include "NppDockScintilla.h"      // SCNotification
#include "PluginInterface.h"       // FuncItem / ShortcutKey / NppData
#include "Notepad_plus_msgs.h"
#include "NppDockEmbed.h"

namespace nppdock {

// 一个"模块页"的描述（NppDock_*.dll）
struct ModuleSlot {
    std::wstring dllName;      // 例如 NppDock_Cmd.dll
    std::wstring dllPath;      // 全路径
    std::wstring title;        // 标签标题（第一级就要拿到）

    HMODULE        hModule  = nullptr;  // 第二级加载后才非空
    NppDockModule* instance = nullptr;
    HWND           hView    = nullptr;  // 视图 HWND（createView 的返回值）
    bool           viewCreated = false;

    bool isLoaded() const { return hModule != nullptr; }
};

class Container {
public:
    Container();
    ~Container();

    bool create();
    void destroy();

    HWND hwnd() const { return _hwnd; }

    // 扫描插件目录下的 NppDock_*.dll，并全部加载以取得标题
    void discoverModules(const std::wstring& pluginDir);

    // 扫描同目录下的 NppDockApp_*.exe（只读文件名与版本资源，不启动进程）
    void discoverApps(const std::wstring& pluginDir);

    // 建页并默认选中第 0 页（无标签条，纯代码驱动）
    void rebuildTabs();

    // 切到第 idx 页。首次切过去会触发 createView（第二级懒加载）。
    void selectIndex(int idx);

    int currentIndex() const { return _current; }

    // ---- 应用页 ----
    const std::vector<AppEntry>& apps() const { return _apps; }
    int  pageCount() const { return (int)_pages.size(); }
    int  currentPage() const { return _curPage; }

    // 打开某应用为新的 tab 页。返回页索引，失败 -1。
    //
    // ⚠️ 语义（v1.2 起改了）：**每次调用都新开一页**，同一个应用允许开多个实例。
    //    老版本是"已打开就切过去"，那会让"同一个工具想同时跑两份"没法做
    //    （比如两个文件校验页、一个 ping 着另一个在测别的）。想切回已开的那页
    //    有更直接的入口 —— **标签项就在上面**，点一下即可，不必绕菜单。
    //    菜单项文字会带上"（已开 N）"，提示"再点一次就是再开一个"。
    int  openAppPage(int appIndex);
    bool closePage(int pageIdx);
    void closeAllPages();
    void switchToPage(int pageIdx);

    // 已打开页对应的 exe 文件名（按页序）。供上层落盘"上次开着哪些页"。
    // 只在容器内部做 appIndex -> exeName 的翻译，上层不必知道 _apps 的结构。
    std::vector<std::wstring> openedAppExeNames() const;

    // 按 exe 文件名反查应用下标（恢复页面用）。找不到返回 -1。
    int  findAppIndexByExeName(const std::wstring& exeName) const;

    // 页面增删/切换后的通知钩子。上层用它把"当前开着哪些页"落盘。
    // 为什么用函数指针而不是让容器自己去写配置文件：
    //   容器只管窗口与进程，不该知道配置文件的格式、路径和落盘时机；
    //   而且落盘还牵扯"恢复期间不要写"这类全局状态，那是上层的知识。
    void setPagesChangedHook(void (*fn)()) { _pagesChangedHook = fn; }

    // 请求把某模块的页切到前台（hostApi->requestShow 用）
    void requestShow(NppDockModule* m);

    // 动态改标题（本版本无标签条，仅更新内部记录）
    void setDirtyTitle(NppDockModule* m, const std::wstring& title);

    const NppDockHostApi* hostApi() const { return &_api; }

    void layout();
    void onShow();
    void onHide();

    // 面板当前是否真的可见（沿父链检查）
    bool isPanelVisible() const;

    static const wchar_t* PanelName() { return L"NppDock"; }

    // 真机自检钩子：容器收到 WM_TIMER 时回调（返回 true 表示已消费）
    void setTimerHook(bool (*fn)(WPARAM)) { _timerHook = fn; }

    // 面板被显示出来时回调（含"外部直接 DMMSHOW"这条路）。
    // 上层用它做**延迟恢复**：启动时面板是隐藏的就不恢复应用页，
    // 等它真被打开再恢复 —— 上次关着的情况下启动 N++ 就不必起任何应用进程。
    void setPanelShownHook(void (*fn)()) { _panelShownHook = fn; }

    // ★ v2.0：内容区中央的那句话（没页时显示）。
    //   两个用途：① 页全关掉时告诉人"入口在哪"；② 应用起不来时给出原因 ——
    //   以前这两件事都是**静默**的（只写日志），界面上看起来就是"点了没反应"。
    void setHint(const std::wstring& s);

private:
    HFONT ensureHintFont();      // 提示文字用的字体（懒建，destroy 时删）

    std::wstring _hint;          // 见 setHint
    HFONT        _hHintFont = nullptr;

    bool (*_timerHook)(WPARAM) = nullptr;
    void (*_pagesChangedHook)() = nullptr;   // 见 setPagesChangedHook
    void (*_panelShownHook)() = nullptr;     // 见 setPanelShownHook

    // ---- 标签拖动排序的状态（在 tabWndProc 里维护）----
    // 为什么要"阈值"：标签项的普通点击也是 LBUTTONDOWN/MOVE/UP，
    // 不设阈值的话"点一下换个页"会被当成拖动，把页序搅乱。
    int   _dragSrcPage = -1;    // 正在拖的页号（-1 = 没在拖）
    POINT _dragStart{ 0, 0 };   // 按下时的客户区坐标（判"是点还是拖"）
    bool  _dragging    = false; // 位移超过阈值才算拖动

    // ⚠️ 拖动开始时**冻结**的各标签槽位边界（客户区 x 范围）。
    //    为什么不实时用 TCM_HITTEST 判目标：拖动中标签内容一直在变
    //    （页序换了、标题长度不同 → 每项宽度不同），槽位边界跟着动，于是
    //    "命中 → 搬过去 → 边界又变 → 又命中回原来那个槽 → 搬回来"，
    //    实测一个拖动里来回抖了 6 次、最后回到原位（等于没拖成功）。
    //    边界冻结之后：指针停在哪个槽就是哪个槽，搬过去后指针仍在该槽
    //    ⇒ 目标 == 当前位置 ⇒ 不再动，天然稳定。
    std::vector<RECT> _dragSlotRects;

    // ★ v1.4：标签项实际使用的文字（可能被补过全角空格凑最小宽度）。
    //   必须**留一份活着的副本**给 comctl32：TCM_INSERTITEM 虽然会拷贝，
    //   但把局部临时量的 c_str() 递进去始终是"看实现脸色"，留一份最省心。
    std::vector<std::wstring> _tabTexts;

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    static LRESULT CALLBACK ContentThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT contentWndProc(HWND, UINT, WPARAM, LPARAM);

    static LRESULT CALLBACK HostThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT hostWndProc(HWND, UINT, WPARAM, LPARAM);

    // 标签条子类化：为了分清"右击在标签项上"和"右击在标签条空白处"。
    static LRESULT CALLBACK TabThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT tabWndProc(HWND, UINT, WPARAM, LPARAM);

    bool registerWindowClass();
    void createControls();
    void createTabControl();
    void destroyModules();
    HWND ensureViewFor(int idx);
    bool loadModule(ModuleSlot& slot);
    void unloadModule(ModuleSlot& slot);
    void fillHostApi();

    // ---- 应用页内部实现 ----
    void refreshTabs();
    int  measureTabHeight();
    void layoutPages(int contentTop, int contentW, int contentH);
    bool startApp(AppPage& page);
    void stopApp(AppPage& page);
    void resizeEmbedded(AppPage& page);
    void repaintEmbeddedPages();   // 把所有页的嵌入窗口整块作废（异步，绝不阻塞）
    void scheduleRepaint();        // 排入"补重画"（队列尾一次 + 防抖一次），见实现
    void sizeHostToContent(HWND hHost);
    bool waitForEmbedded(AppPage& page, DWORD timeoutMs);
    void showAddAppMenu(POINT screenPt);          // tab 空白处右击：添加应用
    void showTabItemMenu(POINT screenPt, int pageIdx);  // tab 项右击：关闭该页
    int  currentAppIndex() const;
    AppPage* findPageByHost(HWND hHost);

    // 同类多开时的标题去重：base 已被占用就依次试 "base（2）""base（3）"…
    // 为什么不用"当前有几个同类页 + 1"：关掉中间一页后那个算法会算出**已存在**的
    //   序号（3 个页关掉第 1 个 → 剩 2 个 → 下一个算成"（3）"，可"（3）"还在），
    // 当前有几个页来自同一个应用（日志/自检用；菜单里**不再显示**"已开 N"）
    int  countPagesOfApp(int appIndex) const;

    // 算出标签项真正显示的文字：不足最小宽度（6 个汉字）就用全角空格补齐。
    // 见实现里的说明 —— comctl32 的非等宽标签宽度 = 文字宽 + 内边距，
    // 想在"不改样式"的前提下给标签一个最低宽度，只能补文字本身。
    std::wstring tabTitleFor(const std::wstring& title) const;

    // 页序重排（标签拖动）：把 from 号页挪到 to 号位置。返回是否真的动了。
    // 选中页跟着**页对象**走（不是跟着序号），所以拖动本身不改变"当前在看哪一页"。
    bool movePage(int from, int to);

    // 给"当前选中的那个标签项"的上边沿画一条高亮横杠（绿杠）。
    //
    // 标签条是 comctl32 的标准控件，没有"自定义高亮"这种能力，
    // 所以做法是：**先让它按原样画完，再在最上层补一笔**（见 tabWndProc）。
    //
    // hdc 传 nullptr = 画到窗口上（WM_PAINT）；传具体 DC = 画到那个 DC（WM_PRINTCLIENT）。
    // 为什么两条路都要走：
    //   · WM_PAINT  —— 正常显示，这是"用户看得见"的那条；
    //   · WM_PRINTCLIENT —— 让高亮也出现在"把窗口渲染到 DC"的场合：
    //     窗口缩略图、PrintWindow（截图工具/自动化测试）等。
    //     少了它，那些场合抓到的是**没有绿杠**的标签条 ——
    //     测试会误判成"功能坏了"，而屏幕上看是好端端的（吃过这个亏）。
    void drawActiveTabBar(HWND hTab, HDC hdc);
    // 标签条下沿的分界线（不改变标签条高度）。hdc=nullptr 表示 WM_PAINT 那条路
    void drawTabStripSeparator(HWND hTab, HDC hdc);

    HWND _hwnd = nullptr;
    HWND _hContent = nullptr;     // 内容区（各页视图/应用宿主的父窗口）
    HWND _hTab = nullptr;         // 标签条（常驻，见文件头说明）
    WNDPROC _tabOrigProc = nullptr;   // 标签条原始窗口过程（子类化前）

    // 队列里是否已经排了一条"补重画"消息（见 resizeEmbedded）。
    // 拖动时每一帧都会走到 resizeEmbedded，不拦一下就会堆出一串重复的补重画；
    // 它们做的都是同一件事（按最终尺寸重画一遍），纯属白干。
    bool _repaintPosted = false;

    // 作业对象：把所有被拉起的应用进程塞进来，并设成"作业句柄一关就全杀"。
    // 这样即使 Notepad++ 被强杀（任务管理器 / taskkill /F），内核也会替我们
    // 回收所有应用进程，不留孤儿进程。
    HANDLE _hJob = nullptr;
    HANDLE ensureJob();

    HWND _nppHandle = nullptr;
    HWND _scintillaMain = nullptr;
    HWND _scintillaSecond = nullptr;

    std::vector<ModuleSlot> _slots;    // 模块页（NppDock_*.dll）
    std::vector<AppEntry>   _apps;     // 发现到的应用（NppDockApp_*.exe）
    std::vector<AppPage>    _pages;    // 已打开的应用页

    int _current = -1;                 // 当前模块页
    int _curPage = -1;                 // 当前应用页

    NppDockHostApi _api{};

    std::wstring _pluginDir;
    std::wstring _configRoot;
    std::wstring _ownConfigRoot;
    std::wstring _logPath;

    static Container* s_instance;

public:
    static Container* instance() { return s_instance; }

    void setHostHandles(HWND npp, HWND sciMain, HWND sciSecond)
    {
        _nppHandle       = npp;
        _scintillaMain   = sciMain;
        _scintillaSecond = sciSecond;
    }

    void buildHostApi() { fillHostApi(); }

    const std::wstring& pluginDir()     const { return _pluginDir; }
    const std::wstring& configRoot()    const { return _configRoot; }
    const std::wstring& ownConfigRoot() const { return _ownConfigRoot; }
    const std::wstring& logPath()       const { return _logPath; }

    void setPaths(const std::wstring& pluginDir,
                  const std::wstring& configRoot,
                  const std::wstring& logPath)
    {
        _pluginDir     = StripTrailingSlash(pluginDir);
        _configRoot    = StripTrailingSlash(configRoot);
        _ownConfigRoot = PathJoin(_configRoot, L"NppDock");
        _logPath       = logPath;
        EnsureDirectory(_ownConfigRoot);
    }

    void log(int level, const std::wstring& msg) const;
};

} // namespace nppdock
