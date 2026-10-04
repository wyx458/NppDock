// ============================================================================
// NppDockContainer.cpp —— 容器窗口实现
// ----------------------------------------------------------------------------
// 窗口树：
//   Container（我们自己注册的顶层窗口，被宿主停靠管理器接管）
//     ├─ _hTab            标签条（SysTabControl32，**常驻**）
//     └─ _hContent        内容区
//          ├─ hView        模块页视图（NppDock_*.dll -> createView 返回）
//          └─ hHost        应用页宿主（NppDockApp_*.exe 的窗口就嵌在它下面）
//                            └─ hEmbedded   ← 跨进程的子窗口
//
// 交互约定（用户定的）：
//   · 右击**标签条空白处** -> 菜单：添加应用
//   · 右击**标签项**       -> 菜单：关闭该页
//   · 右击内容区（空白页） -> 什么都不做
// ============================================================================

#include "NppDockContainer.h"

#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>

namespace nppdock {

Container* Container::s_instance = nullptr;

namespace {

const wchar_t* kContainerClass = L"NppDockContainerWnd";
const wchar_t* kContentClass   = L"NppDockContentPane";
const wchar_t* kAppHostClass   = L"NppDockAppHost";

// 右键菜单命令 ID 分配
constexpr UINT kMenuAppBase   = 1000;   // 1000 + 应用下标
constexpr UINT kMenuRescan    = 900;
constexpr UINT kMenuClosePage = 901;

// 应用宿主子窗口的控件 ID 基址
constexpr int kHostIdBase = 0x4000;

// 私有消息：把"作废跨进程窗口"这件事**排到本轮 resize 之后**再做。
// 为什么要绕这一下，见 resizeEmbedded 里的说明（立刻作废会被宿主后续
// 对父窗口的尺寸调整冲掉，拖动过程中依然空白）。
constexpr UINT kMsgRepaintEmbedded = WM_APP + 0x5D;

// 定时器：拖动**停下来之后**再补一次重画。
// 为什么非要有它，见 resizeEmbedded 第 3 层的说明。
constexpr UINT_PTR kTimerRepaintSettle = 0x5D01;
constexpr UINT     kRepaintSettleMs    = 120;

// ---------------------------------------------------------------------------
// 当前标签页的高亮色 —— "标签项上边沿的一条绿杠"
// ---------------------------------------------------------------------------
// 为什么用这种绿：标签底色是系统灰（COLOR_BTNFACE）或主题近白，
// 中等饱和度的绿在这两种底色上都够清楚；纯 #00FF00 太刺眼，
// 深绿又会和标题文字糊在一起。
// ⚠️ RGB 宏是 (红, 绿, 蓝)，不是十六进制书写顺序 —— 写成 RGB(0x2E,0xA0,0x43)
//    得到的才是"绿"；手滑写成 (0x43,0xA0,0x2E) 会得到一块土黄。
constexpr COLORREF kTabAccent = RGB(0x2E, 0xA0, 0x43);

// 标签项最小宽度（按「几个汉字」算）。王要的是 6 个。
constexpr int kTabMinChars = 6;

// 标签条下沿那条分界线（v1.3 起由我们自己画，见 drawTabStripSeparator）。
// 选这个灰是因为它同时"压得住"标签条的浅灰底和内容区（白/浅色），
// 换成 #E0E0E0 那档跟原来 comctl32 自带的边线一样，等于没画。
constexpr COLORREF kTabSepLine = RGB(0x9E, 0x9E, 0x9E);

// ---------------------------------------------------------------------------
// 让"活在别的进程里"的窗口整块重画
// ---------------------------------------------------------------------------
// ⚠️⚠️ 真机踩到，症状就是用户的原话："调整 dock 大小，里面的程序立马变成空白"。
//
//   先把现象记清楚（这样下次见到能秒认）：
//     · 控件一个没少 —— 窗口层级、可见性、每个子控件的矩形**全对**，进程也活着；
//     · 就是屏幕上什么都没有：面板只剩底色，应用整块消失；
//     · 松手也不会自己恢复，一直空白；
//     · 对它做 SetWindowPos（哪怕尺寸真的变了）、空 SetWindowPos、
//       SW_HIDE/SW_SHOW —— **统统没用**；只有显式 RedrawWindow 才回来。
//   → 结论：不是布局问题，是**这个窗口再也没被请求重绘**。
//
//   触发条件很具体：**祖先窗口被宿主改尺寸**（拖停靠分隔条）时才会发生；
//   我们自己 SetWindowPos 改容器尺寸（不碰祖先）反而不会。
//   跨进程子窗口的可见区域/重定向表面是跟着"父窗口尺寸变更"重算的，
//   这一趟重算之后它就不再出现在"待重画"名单里 —— 于是永远停在旧内容。
//
//   解法分三层，缺一层都能看到残留（都是实测出来的，不是猜的）：
//
//   第 1 层 · 改完尺寸立刻作废
//     「作废」= 把这块区域标成脏，让系统稍后给它排一次 WM_PAINT。
//     · RDW_INVALIDATE 就够；
//     · RDW_ALLCHILDREN 不能少：少了它，应用自己的 8 个控件仍然不画；
//     · 顺带一提：这个窗口是"另一个进程的"，所以没有任何"谁帮我自动重画"的兜底。
//
//   第 2 层 · 把同一次作废**排到自己消息队列的尾部再做**（PostMessage 见
//     resizeEmbedded）。拖动过程中宿主每一帧都会先改父窗口尺寸、再改我们的，
//     我们当场那次作废会被"重算可见区域"冲掉；排到本轮消息末尾才落得住。
//
//   第 3 层 · 等尺寸停住（防抖定时器）再补一次。
//     实测：第 1、2 层日志证明每帧都执行了，可松手后画面仍是空的 ——
//     拖动期间宿主的停靠管理器压着整棵子树的重绘，这期间作废都是白做。
//     详见 resizeEmbedded 第 3 层。
//
//   ⚠️⚠️⚠️ 绝对不要在这里加 RDW_UPDATENOW（"同步重画"）—— 会把 Notepad++ 整个卡死！
//     这是一次真机死锁换来的教训，别再犯：
//       · 本函数跑在 Notepad++ 的 **UI 线程**上（我们是它的插件）；
//       · RDW_UPDATENOW 要求"立刻画完"，而目标窗口**在另一个进程**，
//         于是窗口管理器要等对面那次 WM_PAINT 结束才让我们的线程继续；
//       · 对面的绘制路径（子控件、主题、窗口管理器同步）只要有任何一步
//         回头找它的父窗口 —— 而它的父窗口正是我们，我们的线程正卡在等它 ——
//         双方互等，**永久死锁**。
//     症状（务必记住，因为看上去完全不像我们的问题）：
//       · 任务栏 / 标题栏显示 "Notepad++ 未响应"，Windows 弹出"等待程序响应"对话框；
//       · 面板里只剩下底色，连标签条都是残留；
//       · `IsHungAppWindow(主窗口) == 1`，但**应用进程自己是活的**
//         （`SendMessageTimeout(应用, WM_NULL)` 秒回 1）—— 因为卡住的是我们；
//       · 这时候从外面再怎么 `RedrawWindow` 都没用：我们的线程根本没在跑。
//     排错要点：一看"是不是只有宿主无响应、应用正常"就几乎可以定案。
//
//   那"同步"这一步本来要解决的问题（第一排控件留空洞）怎么办？
//   → 交给**第 3 层的防抖定时器**：等宿主放开重绘压制之后再作废。
//     那次作废是**异步**的（只往对方队列里投一个 WM_PAINT），
//     不阻塞任何线程，效果却和同步一样 —— 时机对了就不需要"强制"。
// ---------------------------------------------------------------------------
void InvalidateCrossProcWindow(HWND h)
{
    if (!h || !::IsWindow(h)) return;
    // ★ 这里永远、永远不加 RDW_UPDATENOW，原因见上。
    ::RedrawWindow(h, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
Container::Container() = default;

Container::~Container()
{
    destroy();
}

// ---------------------------------------------------------------------------
// 创建
// ---------------------------------------------------------------------------
bool Container::registerWindowClass()
{
    static bool s_registered = false;
    if (s_registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Container::WndProcThunk;
    // ⚠️ 必须用**本 DLL** 的 HINSTANCE。写 GetModuleHandleW(NULL) 会拿到
    //    notepad++.exe 的句柄，窗口类注册到 exe 名下，CreateWindowEx 找不到类。
    wc.hInstance     = GetOwnModuleHandle();
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kContainerClass;

    if (!::RegisterClassExW(&wc)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            wchar_t b[192];
            _snwprintf_s(b, _TRUNCATE,
                         L"RegisterClassExW(%s) 失败，错误码 %lu", kContainerClass, e);
            log(NPPDOCK_LOG_ERROR, b);
            return false;
        }
    }

    // 内容区自己的窗口类。
    // 用 ContentThunk 而不是 DefWindowProc：内容区要自己擦底色
    // （"右击空白处不产生任何功能"是它现在的行为，见 contentWndProc）。
    WNDCLASSEXW wc2{};
    wc2.cbSize        = sizeof(wc2);
    wc2.style         = CS_HREDRAW | CS_VREDRAW;
    wc2.lpfnWndProc   = &Container::ContentThunk;
    wc2.hInstance     = GetOwnModuleHandle();
    wc2.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc2.hbrBackground = ::GetSysColorBrush(COLOR_WINDOW);
    wc2.lpszClassName = kContentClass;
    if (!::RegisterClassExW(&wc2)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            wchar_t b[192];
            _snwprintf_s(b, _TRUNCATE,
                         L"RegisterClassExW(%s) 失败，错误码 %lu", kContentClass, e);
            log(NPPDOCK_LOG_ERROR, b);
            return false;
        }
    }

    // 应用宿主窗口类。它的 WM_SIZE 负责把跨进程的嵌入窗口跟着拉伸 ——
    // 这是"嵌入窗口不会自动跟随父窗口大小"那个坑的正解。
    WNDCLASSEXW wc3{};
    wc3.cbSize        = sizeof(wc3);
    wc3.style         = CS_HREDRAW | CS_VREDRAW;
    wc3.lpfnWndProc   = &Container::HostThunk;
    wc3.hInstance     = GetOwnModuleHandle();
    wc3.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc3.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc3.lpszClassName = kAppHostClass;
    if (!::RegisterClassExW(&wc3)) {
        DWORD e = ::GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            wchar_t b[192];
            _snwprintf_s(b, _TRUNCATE,
                         L"RegisterClassExW(%s) 失败，错误码 %lu", kAppHostClass, e);
            log(NPPDOCK_LOG_ERROR, b);
            return false;
        }
    }

    s_registered = true;
    return true;
}

bool Container::create()
{
    if (_hwnd) return true;

    // 标签条属于 Common Controls。Notepad++ 自己肯定初始化过，但我们的 DLL
    // 不能假设宿主的初始化覆盖了所有类（且多调一次是幂等无害的）。
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&icc);

    if (!registerWindowClass()) {
        log(NPPDOCK_LOG_ERROR, L"注册容器窗口类失败");
        return false;
    }

    s_instance = this;

    // ⚠️ 不指定 WS_VISIBLE：由宿主决定何时显示（并恢复上次状态）。
    // ⚠️ 顶层容器不能是 WS_CHILD + NULL parent：该组合在部分 Windows 版本上
    //    CreateWindowEx 直接失败（错误码 1406，误导为"找不到窗口类"）。
    //    正确做法：先 WS_POPUP 建出来，注册到 Docking Manager 后由宿主接管。
    _hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kContainerClass, L"NppDockContainer",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 400, 200,
        /*parent*/ nullptr,
        nullptr, GetOwnModuleHandle(), this);

    if (!_hwnd) {
        wchar_t b[192];
        _snwprintf_s(b, _TRUNCATE, L"创建容器窗口失败，错误码 %lu", ::GetLastError());
        log(NPPDOCK_LOG_ERROR, b);
        s_instance = nullptr;
        return false;
    }

    log(NPPDOCK_LOG_INFO, L"容器窗口已创建（WS_POPUP，待宿主接管）");
    createControls();
    return true;
}

void Container::createControls()
{
    // 内容区：面板的全部可见区域
    _hContent = ::CreateWindowExW(
        0, kContentClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 10, 10,
        _hwnd, /*id*/ (HMENU)(INT_PTR)1, GetOwnModuleHandle(), this);

    if (!_hContent) {
        log(NPPDOCK_LOG_ERROR, L"创建内容区失败");
        return;
    }

    createTabControl();
}

void Container::createTabControl()
{
    // 标签条**常驻**（哪怕一页都没有）。
    // 它是"添加新应用"的唯一入口：右击标签条的空白处才会弹添加菜单，
    // 藏起来用户就没地方加东西了。
    //
    // ⚠️ v1.2 起**去掉了 TCS_FIXEDWIDTH**（原来是固定宽度）。
    //   原因：支持同类多开之后会同时出现"文件校验"和"文件校验（2）"，
    //   固定宽度是按默认尺寸来的，长标题会被截掉尾巴 —— 而尾巴恰恰是
    //   "(2)" 这个唯一能区分两页的信息。改成自动宽度：每个标签恰好够放下
    //   自己的文字，永不截断。（标签条宽度不够时 comctl32 会自己出滚动按钮。）
    _hTab = ::CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | TCS_SINGLELINE | TCS_TABS,
        0, 0, 10, 10,
        _hwnd, /*id*/ (HMENU)(INT_PTR)2, GetOwnModuleHandle(), nullptr);

    if (!_hTab) {
        log(NPPDOCK_LOG_WARN, L"创建标签条失败（将无法添加应用）");
        return;
    }

    // 标签字体跟随系统消息字体
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        HFONT hf = ::CreateFontIndirectW(&ncm.lfMessageFont);
        if (hf) ::SendMessageW(_hTab, WM_SETFONT, (WPARAM)hf, FALSE);
    }

    // 子类化标签条 —— 必须自己接 WM_RBUTTONUP，不能等 NM_RCLICK：
    //   NM_RCLICK 只说"右击了"，**不告诉你点在标签项上还是空白处**；
    //   而这两个位置的行为完全不同（一个是关页，一个是加应用）。
    ::SetWindowLongPtrW(_hTab, GWLP_USERDATA, (LONG_PTR)this);
    _tabOrigProc = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(_hTab, GWLP_WNDPROC,
                            (LONG_PTR)&Container::TabThunk));
    if (!_tabOrigProc) {
        log(NPPDOCK_LOG_WARN, L"标签条子类化失败（右击添加应用将不可用）");
    } else {
        // 如实记下原过程指针：排查"右击标签条没反应"时，第一件事就是
        // 确认子类化到底成没成。
        wchar_t b[160];
        swprintf_s(b, L"标签条已子类化（原窗口过程 = %p）", (void*)_tabOrigProc);
        log(NPPDOCK_LOG_INFO, b);
    }
}

// ---------------------------------------------------------------------------
// 销毁
// ---------------------------------------------------------------------------
void Container::destroy()
{
    if (!_hwnd) {
        // 窗口没建成功也要清干净（discoverModules 可能已加载过模块）
        closeAllPages();
        destroyModules();
        return;
    }

    // ⚠️ 顺序：先收应用页（会请对方进程退出并等它走），
    //    再析构模块、最后销毁容器窗口。
    //    反过来的话宿主窗口先没了，嵌入窗口的父指针悬空，
    //    对面进程可能留下一个画不出来的窗口，只能靠强杀收场。
    closeAllPages();

    destroyModules();
    ::DestroyWindow(_hwnd);
    _hwnd = nullptr;
    _hContent = nullptr;
    _hTab = nullptr;

    if (_hHintFont) { ::DeleteObject(_hHintFont); _hHintFont = nullptr; }
    _hint.clear();

    if (s_instance == this) s_instance = nullptr;
}

void Container::destroyModules()
{
    // 逆序卸载：先销毁视图，再走模块导出的 destroy，最后 FreeLibrary。
    for (auto it = _slots.rbegin(); it != _slots.rend(); ++it) {
        unloadModule(*it);
    }
    _slots.clear();
    _current = -1;
}

void Container::unloadModule(ModuleSlot& slot)
{
    if (!slot.instance) {
        if (slot.hModule) { ::FreeLibrary(slot.hModule); slot.hModule = nullptr; }
        return;
    }

    // 1) 先销毁视图
    if (slot.viewCreated) {
        __try {
            slot.instance->destroyView();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            log(NPPDOCK_LOG_ERROR,
                L"模块 destroyView 抛异常，已忽略（" + slot.dllName + L"）");
        }
        slot.viewCreated = false;
        slot.hView = nullptr;
    }

    // 2) 走模块自己的析构 —— **绝不能 delete**（/MT 下跨 DLL delete 会崩）。
    //    必须由模块 DLL 导出的 nppdock_module_destroy 来释放。
    slot.instance->setHostApi(nullptr);

    if (slot.hModule) {
        auto destroyFn = reinterpret_cast<PFN_nppdock_module_destroy>(
            ::GetProcAddress(slot.hModule, NPPDOCK_DESTROY_SYMBOL));
        if (destroyFn) {
            __try {
                destroyFn(slot.instance);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                log(NPPDOCK_LOG_ERROR,
                    L"模块 destroy 抛异常，已忽略（" + slot.dllName + L"）");
            }
        } else {
            log(NPPDOCK_LOG_ERROR,
                L"模块未导出 " NPPDOCK_DESTROY_SYMBOL L"，为避免跨 DLL delete，"
                L"该对象被放弃（内存泄漏，但不是崩溃）");
        }

        ::FreeLibrary(slot.hModule);
        slot.hModule = nullptr;
    }
    slot.instance = nullptr;
}

void Container::onHide()
{
    // 通知当前页"不可见了"。**不销毁视图/不结束应用进程**。
    if (_current >= 0 && _current < (int)_slots.size()) {
        ModuleSlot& s = _slots[_current];
        if (s.viewCreated && s.instance) {
            __try { s.instance->onHide(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

bool Container::isPanelVisible() const
{
    if (!_hwnd) return false;

    // ⚠️ 不要只看 IsWindowVisible(_hwnd)：宿主隐藏停靠栏时常常只隐藏
    //    **上层容器窗口**，我们这层客户窗口的 WS_VISIBLE 可能仍是 1，会误判。
    //    正确做法：沿父链一路向上，只要有一层不可见就认为面板不可见。
    for (HWND h = _hwnd; h; ) {
        if (!::IsWindowVisible(h)) return false;
        HWND parent = ::GetParent(h);
        if (!parent) break;
        if (::GetWindowLongW(h, GWL_STYLE) & WS_CHILD) { h = parent; continue; }
        break;
    }
    return true;
}

void Container::onShow()
{
    if (_curPage >= 0 && _curPage < (int)_pages.size()) {
        // 有应用页：让它自己重绘（跨进程窗口被祖先隐藏再显示时不会自动刷新）。
        // 只作废、不强制同步 —— 之前这里用 immediate 是**潜在死锁点**，见
        // InvalidateCrossProcWindow 上面那段警告。
        AppPage& p = _pages[_curPage];
        InvalidateCrossProcWindow(p.hEmbedded);

        // ★ 但只这一下是**不够**的：实测"隐藏 -> 唤出"之后应用整块是白的
        //   （界面暗像素 3.11% -> 0.18%）。原因有两条，缺一不可：
        //   ① 唤出时**尺寸根本没变**，所以 resizeEmbedded 那套补重画压根不跑，
        //      我们只有上面这一发作废；
        //   ② 而这一发是同步做的，正好赶在宿主 ShowWindow 之后、
        //      窗口管理器重算可见区域之前 —— 紧接着就被冲掉了。
        //   所以这里必须和改尺寸走同一条路：排到队列尾 + 等停住再补一次。
        //   （调用方 NppDock.cpp 里，onShow 是紧跟在 NPPM_DMMSHOW 返回之后调的。）
        scheduleRepaint();
    } else {
        if (_current < 0 && !_slots.empty()) {
            selectIndex(0);
        }
        if (_current >= 0 && _current < (int)_slots.size()) {
            ModuleSlot& s = _slots[_current];
            if (s.viewCreated && s.instance) {
                __try { s.instance->onShow(); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }
    layout();
}

// ---------------------------------------------------------------------------
// 模块发现（NppDock_*.dll）
// ---------------------------------------------------------------------------
void Container::discoverModules(const std::wstring& pluginDir)
{
    _pluginDir = pluginDir;
    _slots.clear();
    _current = -1;

    if (pluginDir.empty() || !DirExists(pluginDir)) {
        log(NPPDOCK_LOG_ERROR, L"插件目录不存在：" + pluginDir);
        return;
    }

    // 扫描 NppDock_*.dll —— 只看文件名（主 DLL NppDock.dll 不以 "NppDock_" 开头，天然排除）
    const std::wstring pattern = PathJoin(pluginDir, L"NppDock_*.dll");

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        log(NPPDOCK_LOG_INFO,
            L"未发现功能模块（" + pattern + L"），面板保持空白外壳");
        return;
    }

    std::vector<std::wstring> names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.push_back(fd.cFileName);
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

    std::sort(names.begin(), names.end());   // 固定顺序，保证可预测

    for (const auto& n : names) {
        ModuleSlot slot;
        slot.dllName = n;
        slot.dllPath = PathJoin(pluginDir, n);
        _slots.push_back(std::move(slot));
    }

    size_t okCount = 0;
    for (auto& s : _slots) {
        if (loadModule(s)) ++okCount;
    }

    // 加载失败的模块直接剔除
    _slots.erase(std::remove_if(_slots.begin(), _slots.end(),
                                [](const ModuleSlot& s) { return s.instance == nullptr; }),
                 _slots.end());

    log(NPPDOCK_LOG_INFO,
        L"发现模块 " + std::to_wstring(_slots.size()) + L" 个"
        L"（成功加载 " + std::to_wstring(okCount) + L" 个）");
}

bool Container::loadModule(ModuleSlot& slot)
{
    // 动态加载：隐式链接会导致模块缺失时主 DLL 加载失败，而 Notepad++ 对此完全静默。
    HMODULE h = ::LoadLibraryExW(slot.dllPath.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        log(NPPDOCK_LOG_ERROR,
            L"加载模块失败（错误码 " + std::to_wstring(::GetLastError()) + L"）："
            + slot.dllName);
        return false;
    }

    auto entryFn = reinterpret_cast<PFN_nppdock_module_entry>(
        ::GetProcAddress(h, NPPDOCK_ENTRY_SYMBOL));
    auto destroyFn = reinterpret_cast<PFN_nppdock_module_destroy>(
        ::GetProcAddress(h, NPPDOCK_DESTROY_SYMBOL));

    if (!entryFn || !destroyFn) {
        log(NPPDOCK_LOG_ERROR,
            slot.dllName + L" 缺少导出符号（需要 " NPPDOCK_ENTRY_SYMBOL
            L" 与 " NPPDOCK_DESTROY_SYMBOL L"），已跳过");
        ::FreeLibrary(h);
        return false;
    }

    NppDockModule* inst = nullptr;
    __try {
        inst = entryFn(&_api);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log(NPPDOCK_LOG_ERROR, slot.dllName + L" 的 entry 抛异常，已跳过");
        ::FreeLibrary(h);
        return false;
    }

    if (!inst) {
        log(NPPDOCK_LOG_ERROR, slot.dllName + L" 的 entry 返回 nullptr，已跳过");
        ::FreeLibrary(h);
        return false;
    }

    const wchar_t* t = nullptr;
    __try { t = inst->getTitle(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { t = nullptr; }

    slot.hModule  = h;
    slot.instance = inst;
    slot.title    = (t && *t) ? t : StemOf(slot.dllName);
    slot.viewCreated = false;
    slot.hView = nullptr;

    log(NPPDOCK_LOG_INFO, L"已加载模块 " + slot.dllName + L"（标题：" + slot.title + L"）");
    return true;
}

// ---------------------------------------------------------------------------
// 应用发现（NppDockApp_*.exe）
// ---------------------------------------------------------------------------
void Container::discoverApps(const std::wstring& pluginDir)
{
    _apps.clear();

    if (pluginDir.empty() || !DirExists(pluginDir)) return;

    const std::wstring pattern = PathJoin(pluginDir, std::wstring(kAppPrefix) + L"*" + kAppSuffix);

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        log(NPPDOCK_LOG_INFO, L"未发现可嵌入应用（" + pattern + L"）");
        return;
    }

    std::vector<std::wstring> names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        names.push_back(fd.cFileName);
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

    std::sort(names.begin(), names.end());

    for (const auto& n : names) {
        AppEntry e;
        e.exeName = n;
        e.exePath = PathJoin(pluginDir, n);

        // 标题优先取 exe 的版本资源；读不到就回退到文件名去前缀。
        // 回退路径不是可选项 —— rc.exe 缺失或资源没编进去时全靠它。
        e.title = ReadVersionString(e.exePath, L"FileDescription");
        if (e.title.empty()) {
            std::wstring stem = StemOf(n);
            const std::wstring prefix = kAppPrefix;
            if (stem.rfind(prefix, 0) == 0) stem = stem.substr(prefix.size());
            e.title = stem;
        }
        _apps.push_back(std::move(e));
    }

    log(NPPDOCK_LOG_INFO, L"发现可嵌入应用 " + std::to_wstring(_apps.size()) + L" 个");
    for (const auto& a : _apps) {
        log(NPPDOCK_LOG_INFO, L"    · " + a.title + L"（" + a.exeName + L"）");
    }
}

// ---------------------------------------------------------------------------
// 模块页管理
// ---------------------------------------------------------------------------
void Container::rebuildTabs()
{
    if (!_slots.empty()) {
        selectIndex(0);       // 会触发第二级懒加载
    } else {
        _current = -1;
        log(NPPDOCK_LOG_INFO, L"没有功能模块，内容区留空（可右击标签条空白处添加应用）");
    }
    refreshTabs();
    layout();
}

void Container::selectIndex(int idx)
{
    if (idx < 0 || idx >= (int)_slots.size()) return;
    if (idx == _current) return;

    // 老页 onHide（不销毁视图）
    if (_current >= 0 && _current < (int)_slots.size()) {
        ModuleSlot& old = _slots[_current];
        if (old.viewCreated && old.instance) {
            __try { old.instance->onHide(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (old.hView) ::ShowWindow(old.hView, SW_HIDE);
    }

    _current = idx;

    // 新页：第二级懒加载 —— 第一次切过来才 createView
    HWND hv = ensureViewFor(idx);
    if (hv) {
        ::ShowWindow(hv, SW_SHOW);
        ::SetWindowPos(hv, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ::InvalidateRect(hv, nullptr, TRUE);
    }

    ModuleSlot& cur = _slots[idx];
    if (cur.viewCreated && cur.instance) {
        __try { cur.instance->onShow(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    layout();
}

HWND Container::ensureViewFor(int idx)
{
    if (idx < 0 || idx >= (int)_slots.size()) return nullptr;
    ModuleSlot& s = _slots[idx];

    if (s.viewCreated && s.hView && ::IsWindow(s.hView)) {
        return s.hView;   // 已建过，复用（**绝不重复 createView**）
    }
    if (!s.instance) return nullptr;

    // ⚠️ 第二级懒加载的真身：直到这一刻才调用 createView。
    log(NPPDOCK_LOG_INFO, L"首次创建视图：" + s.title + L"（" + s.dllName + L"）");

    HWND hv = nullptr;
    __try {
        hv = s.instance->createView(_hContent);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        hv = nullptr;
    }

    if (!hv) {
        log(NPPDOCK_LOG_ERROR, s.dllName + L" 的 createView 失败");
        s.viewCreated = false;
        s.hView = nullptr;
        return nullptr;
    }

    s.hView = hv;
    s.viewCreated = true;
    __try { s.instance->setViewCreated(true); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (::GetParent(hv) != _hContent) {
        ::SetParent(hv, _hContent);
    }
    ::ShowWindow(hv, SW_HIDE);   // 先隐藏，由 selectIndex 决定显隐

    return hv;
}

void Container::requestShow(NppDockModule* m)
{
    int idx = -1;
    for (size_t i = 0; i < _slots.size(); ++i) {
        if (_slots[i].instance == m) { idx = (int)i; break; }
    }
    if (idx < 0) {
        log(NPPDOCK_LOG_WARN, L"requestShow：找不到对应模块");
        return;
    }

    // 真正的"把面板从隐藏变可见"必须由宿主执行（发 NPPM_DMMSHOW）
    if (_api.showPanel) _api.showPanel();

    if (idx != _current) {
        selectIndex(idx);
    } else if (_slots[idx].viewCreated && _slots[idx].instance) {
        __try { _slots[idx].instance->onShow(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void Container::setDirtyTitle(NppDockModule* m, const std::wstring& title)
{
    if (title.empty()) return;
    for (auto& s : _slots) {
        if (s.instance == m) { s.title = title; break; }
    }
}

// ===========================================================================
// 应用页
// ===========================================================================
int Container::currentAppIndex() const
{
    if (_curPage < 0 || _curPage >= (int)_pages.size()) return -1;
    return _pages[_curPage].appIndex;
}

AppPage* Container::findPageByHost(HWND hHost)
{
    for (auto& p : _pages) {
        if (p.hHost == hHost) return &p;
    }
    return nullptr;
}

int Container::openAppPage(int appIndex)
{
    if (appIndex < 0 || appIndex >= (int)_apps.size()) return -1;

    // ★ 每次调用都是**新开一页**（v1.2 起支持同一个应用多开）。
    //   老版本这里有一段"已开着就 switchToPage 再 return"，删掉它是有意的：
    //   那样"同时跑两份同一个工具"就永远做不到。想切回已开的那页点标签项即可。
    // ★ v1.3：标题就是应用名，**不再加"（2）"这类序号**（王: 很难看）。
    //   同一个应用开多页时标签文字会一样 —— 这是接受的：
    //   用户靠"位置"和"我点了几次"分辨，点开内容里的标题条也能看出是哪个实例。
    AppPage page;
    page.appIndex = appIndex;
    page.exeName  = _apps[appIndex].exeName;
    page.title    = _apps[appIndex].title;

    if (!startApp(page)) {
        stopApp(page);
        return -1;
    }

    _pages.push_back(std::move(page));
    _curPage = (int)_pages.size() - 1;

    refreshTabs();
    layout();
    if (_pagesChangedHook) _pagesChangedHook();   // 落盘"当前开着哪些页"
    return _curPage;
}

// ---------------------------------------------------------------------------
// 页序重排（标签拖动）
// ---------------------------------------------------------------------------
bool Container::movePage(int from, int to)
{
    if (from < 0 || from >= (int)_pages.size()) return false;
    if (to   < 0 || to   >= (int)_pages.size()) return false;
    if (from == to) return false;

    // 先记下"当前选中的是哪一个页对象"，挪完再按它把 _curPage 找回来 ——
    // 直接按序号平移很容易在"跨过当前位置"时算错（拖着自己往前/往后各是一种情况）。
    const bool curIsMoved = (_curPage == from);
    AppPage moved = std::move(_pages[from]);
    _pages.erase(_pages.begin() + from);
    _pages.insert(_pages.begin() + to, std::move(moved));

    if (curIsMoved) {
        _curPage = to;
    } else {
        // 被移动的页从 from 挪到了 to：
        //   跨过我 -> 我的下标反方向挪一位；没跨过我 -> 我不动。
        if (from < _curPage && to >= _curPage)      --_curPage;
        else if (to <= _curPage && from > _curPage) ++_curPage;
    }

    refreshTabs();
    if (_pagesChangedHook) _pagesChangedHook();   // 顺序也要落盘
    return true;
}

int Container::countPagesOfApp(int appIndex) const
{
    int n = 0;
    for (const auto& p : _pages) {
        if (p.appIndex == appIndex) ++n;
    }
    return n;
}

void Container::switchToPage(int pageIdx)
{
    if (pageIdx < 0 || pageIdx >= (int)_pages.size()) return;
    _curPage = pageIdx;
    if (_hTab && ::IsWindow(_hTab)) {
        ::SendMessageW(_hTab, TCM_SETCURSEL, (WPARAM)pageIdx, 0);

        // ⚠️ 必须自己再作废一次标签条，不能只靠 TCM_SETCURSEL 的自动重画：
        //   我们在 WM_PAINT 里给"当前页"补一条绿色高亮横杠（drawActiveTabBar），
        //   换页时**旧的那条要消失、新的那条要出现** —— 只有整条标签条重画一遍
        //   才能保证旧位置被原生画法覆盖掉。少了这一句的症状是
        //   "切几页之后，好几条标签上都有绿杠"。
        //   bErase 传 FALSE：标签条自己会把整个客户区画满，不需要先擦一遍背景
        //   （传 TRUE 会先刷一遍父窗口底色，换页时看得见闪）。
        ::InvalidateRect(_hTab, nullptr, FALSE);
    }
    layout();

    // 跨进程窗口被隐藏/尺寸没变时不会重绘，主动催一下。
    // 只作废、不强制同步（曾经用过 immediate，那是个潜在死锁点，
    // 见 InvalidateCrossProcWindow 上面的警告）。
    // 刚 ShowWindow 过的窗口，作废之后系统本来就会给它排一次 WM_PAINT，
    // 所以这里不必"立刻"。
    AppPage& p = _pages[pageIdx];
    if (p.hEmbedded && ::IsWindow(p.hEmbedded)) {
        ::SetWindowPos(p.hEmbedded, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                       | SWP_SHOWWINDOW);
        InvalidateCrossProcWindow(p.hEmbedded);
    }

    // 选中页也要落盘，重开时才能停在同一页上。
    if (_pagesChangedHook) _pagesChangedHook();
}

bool Container::closePage(int pageIdx)
{
    if (pageIdx < 0 || pageIdx >= (int)_pages.size()) return false;

    const std::wstring title = _pages[pageIdx].title;
    stopApp(_pages[pageIdx]);
    _pages.erase(_pages.begin() + pageIdx);

    if (_pages.empty()) {
        _curPage = -1;
    } else if (pageIdx < _curPage) {
        _curPage--;
    } else if (pageIdx == _curPage && _curPage >= (int)_pages.size()) {
        _curPage = (int)_pages.size() - 1;
    }

    log(NPPDOCK_LOG_INFO, L"已关闭应用页：" + title);
    refreshTabs();
    layout();
    if (_pagesChangedHook) _pagesChangedHook();   // 关掉的页不该在下次重开时回来
    return true;
}

void Container::closeAllPages()
{
    for (auto& p : _pages) {
        stopApp(p);
    }
    _pages.clear();
    _curPage = -1;
    // 也通知一次：调用方（NppDock 的关闭流程）会自行判断此刻该不该落盘 ——
    // 正常退出时它已经先把状态存好了，这时候再写会把列表清空。
    if (_pagesChangedHook) _pagesChangedHook();
}

// ---------------------------------------------------------------------------
// 页面 -> exe 文件名（供上层落盘）
// ---------------------------------------------------------------------------
// 为什么不在上层直接读 _apps：_pages 里的 appIndex 是**下标**，
// 应用目录一变（增删 exe）下标就漂了；落盘只认 exe 文件名才稳。
//
// ⚠️ 用 page.exeName（建页时抄下来的），不要用 _apps[p.appIndex].exeName：
//    同类多开之后有两页指向同一个 appIndex，走 _apps 也能对，但多绕一层；
//    而且 exeName 在**建页那一刻**就固定了，语义更直白。
//    （另外这样也允许将来"两页来自同一个 exe 但参数不同"。）
// ---------------------------------------------------------------------------
std::vector<std::wstring> Container::openedAppExeNames() const
{
    std::vector<std::wstring> out;
    out.reserve(_pages.size());
    for (const auto& p : _pages) {
        out.push_back(p.exeName);
    }
    return out;
}

int Container::findAppIndexByExeName(const std::wstring& exeName) const
{
    if (exeName.empty()) return -1;
    for (size_t i = 0; i < _apps.size(); ++i) {
        // 不区分大小写：Windows 文件名本来就不区分
        if (_wcsicmp(_apps[i].exeName.c_str(), exeName.c_str()) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 作业对象：拿内核兜底"父死子清"
// ---------------------------------------------------------------------------
// 为什么必须有它：
//   正常退出时我们能走 closeAllPages() 一个个结束应用；但 Notepad++ 被强杀
//   （任务管理器结束任务 / taskkill /F / 崩溃）时，插件根本没机会执行任何代码，
//   被拉起的应用就成了没人管的孤儿进程，一直挂在桌面上。
//   把应用进程放进一个设了 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 的作业对象后，
//   作业句柄随本进程一起销毁，内核会把作业内所有进程全部杀掉 ——
//   这是唯一不依赖"插件代码被执行"的回收方式。
// ---------------------------------------------------------------------------
HANDLE Container::ensureJob()
{
    if (_hJob) return _hJob;

    HANDLE j = ::CreateJobObjectW(nullptr, nullptr);
    if (!j) {
        wchar_t b[192];
        swprintf_s(b, L"创建作业对象失败（错误码 %lu），强杀时将依赖逐个结束应用",
                   ::GetLastError());
        log(NPPDOCK_LOG_WARN, b);
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(j, JobObjectExtendedLimitInformation,
                                   &li, sizeof(li))) {
        wchar_t b[192];
        swprintf_s(b, L"设置 KILL_ON_JOB_CLOSE 失败（错误码 %lu），"
                      L"强杀时可能残留应用进程", ::GetLastError());
        log(NPPDOCK_LOG_WARN, b);
    } else {
        log(NPPDOCK_LOG_INFO, L"作业对象已就绪（父进程退出时内核自动回收应用进程）");
    }

    _hJob = j;
    return _hJob;
}

bool Container::startApp(AppPage& page)
{
    if (!_hContent) return false;
    const AppEntry& app = _apps[page.appIndex];

    // 1) 建宿主窗口（本进程的普通子窗口）
    page.hHost = ::CreateWindowExW(
        0, kAppHostClass, L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 400, 120,
        _hContent,
        (HMENU)(INT_PTR)(kHostIdBase + page.appIndex),
        GetOwnModuleHandle(), this);

    if (!page.hHost) {
        log(NPPDOCK_LOG_ERROR, L"创建应用宿主窗口失败：" + app.exeName);
        return false;
    }

    // 先把宿主铺到内容区大小。
    // 不做这一步的话应用启动时拿到的是 10x10，随后被拉伸，视觉上会闪一下。
    sizeHostToContent(page.hHost);

    // ⚠️ 必须显式显示宿主（真机踩过）：宿主创建时没带 WS_VISIBLE，
    //    而 IsWindowVisible 对"父窗口不可见的子窗口"一律返回 FALSE ——
    //    于是嵌入进来的应用窗口永远"不可见"，
    //    waitForEmbedded 会一直等不到、8 秒后误报"应用未挂载"，
    //    而实际上应用已经妥妥地 SetParent 上来了（只是没被画出来）。
    ::ShowWindow(page.hHost, SW_SHOW);

    // 2) 拉起应用进程，把宿主句柄告诉它
    wchar_t hwndText[32];
    swprintf_s(hwndText, L"%llu", (unsigned long long)(UINT_PTR)page.hHost);

    std::wstring cmd = L"\"" + app.exePath + L"\" " + kDockParentArg + L" " + hwndText;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    std::wstring workDir = StripTrailingSlash(DirNameOf(app.exePath));

    if (!::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                          0, nullptr, workDir.c_str(), &si, &pi)) {
        DWORD e = ::GetLastError();
        wchar_t b[256];
        swprintf_s(b, L"拉起应用失败（错误码 %lu）：%s", e, app.exeName.c_str());
        log(NPPDOCK_LOG_ERROR, b);
        // v2.0：以前这里只写日志 —— 用户右击选了应用、界面毫无反应，
        //   完全不知道发生了什么。现在把原因摆到内容区中间。
        setHint(std::wstring(L"「") + (page.title.empty() ? page.exeName : page.title) +
                L"」启动失败（进程没起来）—— 细节见 NppDock.log");
        ::DestroyWindow(page.hHost);
        page.hHost = nullptr;
        return false;
    }

    ::CloseHandle(pi.hThread);

    // 立刻把子进程收进作业对象：越早越好，晚了万一它已经自己退出，
    // 我们手上就只剩一个僵尸句柄，AssignProcessToJobObject 会报错。
    if (HANDLE job = ensureJob()) {
        if (!::AssignProcessToJobObject(job, pi.hProcess)) {
            wchar_t b[192];
            swprintf_s(b, L"把 %s 加入作业对象失败（错误码 %lu）",
                       app.exeName.c_str(), ::GetLastError());
            log(NPPDOCK_LOG_WARN, b);
        }
    }

    page.hProcess = pi.hProcess;
    page.pid      = pi.dwProcessId;

    {
        wchar_t b[256];
        swprintf_s(b, L"已拉起应用 %s（PID %lu）", app.exeName.c_str(), page.pid);
        log(NPPDOCK_LOG_INFO, b);
    }

    // 3) 等它把窗口挂上来
    if (!waitForEmbedded(page, 8000)) {
        log(NPPDOCK_LOG_ERROR, L"应用未在超时内挂载窗口：" + app.exeName);
        // 两种可能：进程提前退了、或者它压根没 SetParent 上来。
        // 加一句可见提示（原因只能给到这一层，细节在日志里）。
        setHint(std::wstring(L"「") + (page.title.empty() ? page.exeName : page.title) +
                L"」没能嵌进来（8 秒内没挂上窗口）—— 它可能自己退出了，细节见 NppDock.log");
        return false;
    }

    log(NPPDOCK_LOG_INFO, L"应用已嵌入：" + page.title
                          + L"（页面标题即 exe 的 FileDescription）");
    return true;
}

bool Container::waitForEmbedded(AppPage& page, DWORD timeoutMs)
{
    const DWORD start = ::GetTickCount();

    for (;;) {
        // 进程已经死了就别等了
        if (page.hProcess &&
            ::WaitForSingleObject(page.hProcess, 0) == WAIT_OBJECT_0) {
            log(NPPDOCK_LOG_ERROR, L"应用进程提前退出，无法嵌入");
            return false;
        }

        // 宿主下出现子窗口 = 应用已经 SetParent 上来了，就算挂载成功。
        //
        // 为什么不要求 IsWindowVisible：可见性依赖整条父链都可见，
        // 任何一环没显示都会让 IsWindowVisible 返回 FALSE（踩过：
        // 宿主自己漏了 ShowWindow，结果应用明明嵌好了却被判超时）。
        // 尺寸不对由外面的 resizeEmbedded 收拾，不该卡在"等可见"上。
        for (HWND c = ::FindWindowExW(page.hHost, nullptr, nullptr, nullptr);
             c; c = ::FindWindowExW(page.hHost, c, nullptr, nullptr)) {
            page.hEmbedded = c;
            break;
        }

        if (page.hEmbedded && ::IsWindow(page.hEmbedded)) {
            resizeEmbedded(page);
            {
                wchar_t b[192];
                swprintf_s(b, L"检测到应用窗口已挂载（hwnd=%p，可见=%d）",
                           (void*)page.hEmbedded,
                           (int)::IsWindowVisible(page.hEmbedded));
                log(NPPDOCK_LOG_INFO, b);
            }
            return true;
        }

        // 等待期间要泵消息，否则这最多 8 秒的等待会把整个界面卡住
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (::GetTickCount() - start > timeoutMs) return false;
        ::Sleep(25);
    }
}

void Container::sizeHostToContent(HWND hHost)
{
    if (!hHost || !_hContent || !::IsWindow(hHost)) return;
    RECT cr{};
    ::GetClientRect(_hContent, &cr);
    int w = cr.right - cr.left;
    int h = cr.bottom - cr.top;
    if (w <= 0) w = 400;
    if (h <= 0) h = 120;
    ::SetWindowPos(hHost, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// 调整嵌入窗口的尺寸
// ---------------------------------------------------------------------------
// 后半段的"作废重画"是必需的，原因见文件头部 InvalidateCrossProcWindow 的注释
// （症状：拖动 dock 分隔条后，应用整块变空白且不恢复）。
// ---------------------------------------------------------------------------
void Container::resizeEmbedded(AppPage& page)
{
    if (!page.hHost || !page.hEmbedded) return;
    if (!::IsWindow(page.hHost) || !::IsWindow(page.hEmbedded)) return;

    // ⚠️ 子窗口从不会自动填满父窗口，跨进程的更不会。
    //    宿主变了尺寸必须由我们显式把嵌入窗口也 SetWindowPos 一次，
    //    对方收到 WM_SIZE 后才会重排它自己的控件。
    RECT cr{};
    ::GetClientRect(page.hHost, &cr);
    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return;

    // SWP_NOCOPYBITS：不要把"上一次的客户区内容"搬过来。
    // 跨进程窗口搬过去的内容常常是错位的残留，作废掉让它整块重画更干净。
    ::SetWindowPos(page.hEmbedded, nullptr, 0, 0, w, h,
                   SWP_NOCOPYBITS | SWP_NOZORDER | SWP_NOACTIVATE);

    // 第 1 层：立刻作废一次。尺寸之后没再变的情况下，这一步就够了。
    InvalidateCrossProcWindow(page.hEmbedded);

    // 第 2、3 层：真正把画面刷干净的两步，见 scheduleRepaint()。
    scheduleRepaint();
}

// ---------------------------------------------------------------------------
// 排入"补重画"：队列尾一次 + 防抖一次
// ---------------------------------------------------------------------------
// 什么时候需要它：只要是"窗口刚被宿主显示出来 / 刚改完尺寸"这一类时刻，
// 一次同步作废都会落空 —— 因为窗口管理器紧接着会重算可见区域，把它冲掉。
// 目前已知需要它的两个场景：
//   · resizeEmbedded（拖 dock 分隔条改尺寸，见那里的注释）；
//   · onShow（隐藏后再唤出，尺寸根本不变，所以走不到 resizeEmbedded，
//     必须在这里单独排一次，否则唤出后应用整块是白的）。
//
// 第 2 层 · 把作废**排到自己消息队列的尾部**再做。
//   宿主一次鼠标移动的处理顺序是：
//     先改**我们父窗口**的尺寸 -> 再改我们的尺寸 -> 我们布局 -> 作废
//   而"改父窗口尺寸"会让窗口管理器重算子窗口可见区域，把当场那次作废冲掉。
//   排到队列尾就落得住了。_repaintPosted 拦着，队列里最多一条，不堆积。
//
// 第 3 层 · 等动作停住（防抖定时器）再补一次。这一层才是真正治好拖动的那步。
//   实测：第 1、2 层都执行了，松手后画面仍是空的 —— 拖动期间宿主的停靠管理器
//   压着整棵子树的重绘（它压在上层窗口上，WM_SETREDRAW 那条路我们收不到），
//   这期间任何 RedrawWindow 都是白做。所以只要还在连续动作，就一直把补重画
//   往后推；停住 120ms 之后才真正补一次，那时候宿主早放开了。
//   —— 用定时器是**对的**：要判断的正是"还有没有在动"，本来就是时间上的事，
//      不是消息先后的事（第 2 层才是消息先后）。
//   ⚠️ 定时器里那次作废**必须是异步的**（只投 WM_PAINT、不等待）。
//      曾经为了补"第一排空洞"把它写成 RDW_UPDATENOW 同步重画，
//      结果整个 Notepad++ 死锁 —— 详见 InvalidateCrossProcWindow 的警告。
// ---------------------------------------------------------------------------
void Container::scheduleRepaint()
{
    if (!_hwnd) return;
    if (!_repaintPosted) {
        _repaintPosted = true;
        ::PostMessageW(_hwnd, kMsgRepaintEmbedded, 0, 0);
    }
    ::SetTimer(_hwnd, kTimerRepaintSettle, kRepaintSettleMs, nullptr);
}

// ---------------------------------------------------------------------------
// 把所有已打开页的嵌入窗口整块重刷一遍
// ---------------------------------------------------------------------------
// 只作废（异步），绝不强制同步 —— 见 InvalidateCrossProcWindow 的死锁警告。
// ---------------------------------------------------------------------------
void Container::repaintEmbeddedPages()
{
    for (auto& p : _pages) {
        if (p.hEmbedded && ::IsWindow(p.hEmbedded)) {
            InvalidateCrossProcWindow(p.hEmbedded);
        }
    }
}

// ---------------------------------------------------------------------------
// 一边等进程退出，一边泵消息
// ---------------------------------------------------------------------------
// ⚠️ 为什么不能直接 WaitForSingleObject（真机踩到，症状是"关标签页僵 3 秒"）：
//   应用收到 WM_CLOSE 后会 DestroyWindow；而销毁一个**父窗口位于别的进程**的
//   子窗口时，系统要向它的父窗口发 WM_PARENTNOTIFY 并**同步等待**处理完毕。
//   那个父窗口就是我们的宿主窗口，归 Notepad++ 的 UI 线程 —— 而此刻 N++ 的
//   UI 线程正卡在 WaitForSingleObject 上，消息循环停摆。双方互等，死锁，
//   对面永远走不完 DestroyWindow，只能等满超时才由 TerminateProcess 强杀。
//   → 正解：等待期间照常派发消息（MsgWaitForMultipleObjects + PeekMessage）。
//   注意本函数跑在 N++ 的 UI 线程上，所以每次只泵一轮就回去继续等，
//   既解死锁又能在超时后干净退出。
// ---------------------------------------------------------------------------
static bool WaitForProcessExitPumping(HANDLE hProcess, DWORD timeoutMs)
{
    const DWORD start = ::GetTickCount();
    for (;;) {
        const DWORD spent = ::GetTickCount() - start;
        if (spent >= timeoutMs) return false;

        const DWORD w = ::MsgWaitForMultipleObjects(
            1, &hProcess, FALSE, timeoutMs - spent, QS_ALLINPUT);

        if (w == WAIT_OBJECT_0) return true;        // 进程退出了
        if (w == WAIT_TIMEOUT)  return false;

        if (w == WAIT_OBJECT_0 + 1) {
            // 有消息：派发掉。这是解开死锁的关键一步。
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) continue;  // 别把退出消息吃掉
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            continue;
        }
        return false;                                // WAIT_FAILED 之类
    }
}

void Container::stopApp(AppPage& page)
{
    // 1) 先请对方自己退出。
    //    用 WM_CLOSE 而不是直接 TerminateProcess：对方能正常跑完 WM_DESTROY、
    //    把临时文件之类收拾干净。
    if (page.hEmbedded && ::IsWindow(page.hEmbedded)) {
        ::PostMessageW(page.hEmbedded, WM_CLOSE, 0, 0);
    }

    // 2) 等它退出（边等边泵消息，否则会死锁 3 秒，见函数上方注释），超时强杀兜底。
    if (page.hProcess) {
        if (!WaitForProcessExitPumping(page.hProcess, 3000)) {
            wchar_t b[192];
            swprintf_s(b, L"应用未在 3 秒内退出，强制结束（PID %lu）", page.pid);
            log(NPPDOCK_LOG_WARN, b);
            ::TerminateProcess(page.hProcess, 0);
            ::WaitForSingleObject(page.hProcess, 1000);
        }
        ::CloseHandle(page.hProcess);
        page.hProcess = nullptr;
    }

    // 3) 拆宿主窗口。**必须在对方进程退出之后**，否则父指针悬空。
    if (page.hHost && ::IsWindow(page.hHost)) {
        ::DestroyWindow(page.hHost);
    }
    page.hHost = nullptr;
    page.hEmbedded = nullptr;
    page.pid = 0;
}

// ---------------------------------------------------------------------------
// 标签条
// ---------------------------------------------------------------------------
// 给"当前选中的标签项"的上边沿补一条绿色横杠。
//
// 【为什么不能用控件自己的机制】
//   SysTabControl32 没有任何"自定义高亮"能力：它只有"选中态"那套原生画法
//   （选中项比别的略亮/略高一点），而且随系统主题变。想要一条确定的绿色横杠，
//   只有两条路：① TCS_OWNERDRAWFIXED 自己画整个标签（要连圆角、主题、
//   图标位、按下态全部复刻一遍，很容易画得比原生难看）；
//   ② **让原生先画完，再在最上层补一笔**（我们选这条）。
//   ②的代价只有一笔 FillRect，换来的是"外观永远跟系统一致、只有那一笔是我们加的"。
//
// 【为什么画在这里是安全的】
//   本函数在标签条自己的 WM_PAINT 里被调用（见 tabWndProc），
//   和控件同线程、同进程，GetDC(hwnd) 拿到的是同线程 DC —— 不像跨进程重画
//   那样会牵扯到"等对面画完"（那种同步是死锁源，见 InvalidateCrossProcWindow）。
// ---------------------------------------------------------------------------
void Container::drawActiveTabBar(HWND hTab, HDC hdc)
{
    if (!hTab || !::IsWindow(hTab)) return;
    if (_curPage < 0 || _curPage >= (int)_pages.size()) return;
    if (TabCtrl_GetItemCount(hTab) <= _curPage) return;

    // ⚠️ TCM_GETITEMRECT 要传 RECT*。这里**同进程**（标签条是我们自己创建的），
    //    所以传指针是安全的；跨进程那样做会把对面写崩（探针注释里有血泪记录，
    //    见 tools/dock_app_probe.py 的 tab_item_click_point）。
    RECT it{};
    if (!TabCtrl_GetItemRect(hTab, _curPage, &it)) return;
    if (it.right <= it.left || it.bottom <= it.top) return;

    RECT cr{};
    ::GetClientRect(hTab, &cr);
    // 标签多了以后 comctl32 会加滚动按钮，被滚出去的那一项拿到的矩形落在客户区外，
    // 这时候不要画（画了就是"绿杠飘在滚动按钮上"）。
    if (it.bottom <= cr.top || it.top >= cr.bottom) return;
    if (it.right <= cr.left || it.left >= cr.right) return;

    // 横杠厚度按**标签项实测高度**的 1/8 取，至少 2px。
    // 不写死 3px：标签高度是跟着 DPI 与字体走的（96dpi 下约 26px，
    // 150% 缩放下约 39px），按比例算才能"看着一样粗"。
    const int itemH = it.bottom - it.top;
    int thick = itemH / 8;
    if (thick < 2) thick = 2;

    RECT bar = it;
    bar.bottom = bar.top + thick;
    // 顶端两个角是圆的，整宽矩形会在圆角外糊出两个尖角 —— 左右各让开一点。
    const int inset = thick;
    bar.left  += inset;
    bar.right -= inset;
    if (bar.right <= bar.left) return;

    // hdc 为 nullptr 时自己取窗口 DC（WM_PAINT 那条路）；
    // 否则用调用方给的（WM_PRINTCLIENT）—— 那条路上窗口可能压根没显示在屏幕上。
    HDC own = nullptr;
    if (!hdc) {
        own = ::GetDC(hTab);
        hdc = own;
    }
    if (hdc) {
        HBRUSH br = ::CreateSolidBrush(kTabAccent);
        if (br) {
            ::FillRect(hdc, &bar, br);
            ::DeleteObject(br);
        }
    }
    if (own) ::ReleaseDC(hTab, own);
}

// ---------------------------------------------------------------------------
// 标签条下沿的分界线（王：标签条与内容区"分界还不够明显"）
//
// 做法是**在原控件画完之后，把最下面那几像素重新刷一遍**：
//   · 不动标签条的高度 —— 王明确要求"不增加 tab 占用空间"，
//     所以不能靠加高标签条/嵌一条真的分隔控件来做；
//   · comctl32 原本在这一行也有一条边线（很浅，是它自己的边框），
//     我们只是用更深的灰把它盖掉。
//
// 厚度取标签条高度的 1/10（最少 2px）：标签高度是跟着 DPI 和字体走的
// （96dpi 下约 23px、150% 缩放下约 35px），按比例算才"看着一样粗"。
// ---------------------------------------------------------------------------
void Container::drawTabStripSeparator(HWND hTab, HDC hdc)
{
    if (!hTab || !::IsWindow(hTab)) return;

    RECT cr{};
    ::GetClientRect(hTab, &cr);
    const int hh = cr.bottom - cr.top;
    if (hh <= 4) return;

    int thick = hh / 10;
    if (thick < 2) thick = 2;
    if (thick > hh / 3) thick = hh / 3;

    RECT bar{ cr.left, cr.bottom - thick, cr.right, cr.bottom };

    // hdc 为 nullptr 时自己取窗口 DC（WM_PAINT 那条路）；否则用调用方给的
    // （WM_PRINTCLIENT）—— 那条路上窗口可能压根没显示在屏幕上。
    HDC own = nullptr;
    if (!hdc) {
        own = ::GetDC(hTab);
        hdc = own;
    }
    if (hdc) {
        HBRUSH br = ::CreateSolidBrush(kTabSepLine);
        if (br) {
            ::FillRect(hdc, &bar, br);
            ::DeleteObject(br);
        }
    }
    if (own) ::ReleaseDC(hTab, own);
}

// ---------------------------------------------------------------------------
// 标签项显示的文字：不足「6 个汉字」宽就用全角空格补齐
//
// 为什么这么做（王的要求是「每个 tab 有个最低宽度，例如 6 个汉字」）：
//   comctl32 的标签条在**非等宽**模式下的宽度 = 该项文字宽 + 一点内边距。
//   于是「文件校验」和「网络测试」两个标签紧挨着，中间几乎没有缝 ——
//   用户看着就是「标签之间分隔不明显」。
//   想直接设最低宽度是**没有 API 的**：
//     · TCM_SETITEMSIZE 只对 TCS_FIXEDWIDTH / TCS_OWNERDRAWFIXED 生效；
//     · 而 TCS_FIXEDWIDTH 会让所有标签等宽，长标题被截断（v1.2 就因为它
//       把「（2）」截掉过，才专门去掉的）。
//   所以补的是**文字本身的宽度**：用全角空格 U+3000（正好一个字宽），
//   左右各补一半 —— 标签文字本来就是居中的，所以视觉上只多出留白、不偏移。
// ---------------------------------------------------------------------------
std::wstring Container::tabTitleFor(const std::wstring& title) const
{
    if (!_hTab || !::IsWindow(_hTab)) return title;

    HFONT f = (HFONT)::SendMessageW(_hTab, WM_GETFONT, 0, 0);
    if (!f) f = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);

    HDC dc = ::GetDC(_hTab);
    if (!dc) return title;
    HGDIOBJ old = ::SelectObject(dc, f);

    SIZE one{};
    ::GetTextExtentPoint32W(dc, L"\u6587", 1, &one);          // 一个汉字宽
    SIZE cur{};
    ::GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &cur);

    ::SelectObject(dc, old);
    ::ReleaseDC(_hTab, dc);

    if (one.cx <= 0) return title;
    const int minW = one.cx * kTabMinChars;
    if (cur.cx >= minW) return title;

    int need = (minW - cur.cx + one.cx - 1) / one.cx;     // 还差几个全角宽
    if (need % 2) ++need;                                 // 取偶，保证左右对称
    const int left = need / 2;
    std::wstring out;
    out.append((size_t)left, L'\u3000');
    out += title;
    out.append((size_t)left, L'\u3000');
    return out;
}

void Container::refreshTabs()
{
    if (!_hTab || !::IsWindow(_hTab)) return;

    ::SendMessageW(_hTab, WM_SETREDRAW, FALSE, 0);
    TabCtrl_DeleteAllItems(_hTab);

    // 补齐后的文字留一份在 _tabTexts 里活着（comctl32 会拷贝，但别赌实现）
    _tabTexts.clear();
    _tabTexts.reserve(_pages.size());
    for (size_t i = 0; i < _pages.size(); ++i)
        _tabTexts.push_back(tabTitleFor(_pages[i].title));

    for (size_t i = 0; i < _pages.size(); ++i) {
        TCITEMW it{};
        it.mask    = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(_tabTexts[i].c_str());
        TabCtrl_InsertItem(_hTab, (int)i, &it);
    }

    if (_curPage >= 0 && _curPage < (int)_pages.size()) {
        TabCtrl_SetCurSel(_hTab, _curPage);
    }

    // 每个标签项的实际宽度 + "最低宽度"的判据，记一条日志。
    // 为什么要落盘：标签项矩形只能**进程内**用 TCM_GETITEMRECT 拿
    // （跨进程传指针会把宿主写崩，踩坑 8.5 / 19.1），所以自动化想验
    // "每个 tab 至少有 6 个汉字宽"就只能读这条日志。
    {
        HFONT f  = (HFONT)::SendMessageW(_hTab, WM_GETFONT, 0, 0);
        HDC   dc = ::GetDC(_hTab);
        int unit = 0;
        if (dc) {
            HGDIOBJ old = f ? ::SelectObject(dc, f) : nullptr;
            SIZE s{};
            if (::GetTextExtentPoint32W(dc, L"\u6587", 1, &s)) unit = s.cx;
            if (old) ::SelectObject(dc, old);
            ::ReleaseDC(_hTab, dc);
        }
        std::wstring line = L"标签宽度：";
        for (int i = 0; i < TabCtrl_GetItemCount(_hTab); ++i) {
            RECT r{};
            wchar_t b[48];
            if (TabCtrl_GetItemRect(_hTab, i, &r))
                swprintf_s(b, L"项%d=%ldpx ", i, (long)(r.right - r.left));
            else
                swprintf_s(b, L"项%d=? ", i);
            line += b;
        }
        wchar_t b2[96];
        swprintf_s(b2, L"（最低 %d 个汉字 = %dpx）", kTabMinChars, unit * kTabMinChars);
        line += b2;
        log(NPPDOCK_LOG_INFO, line.c_str());
    }

    ::SendMessageW(_hTab, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(_hTab, nullptr, TRUE);

    // ★ v2.0：页全关掉之后，内容区中央给一句"入口在哪"。
    //   这句话也是"应用起不来"的出口（那种情况下 openAppPage 会把它改成原因）。
    if (_pages.empty()) {
        if (_apps.empty()) {
            setHint(L"没有发现可嵌入的应用 —— NppDockApp_*.exe 必须和 NppDock.dll 放在同一目录");
        } else {
            wchar_t b[160];
            swprintf_s(b, L"右击上方标签条空白处添加应用（已发现 %d 个）", (int)_apps.size());
            setHint(b);
        }
    } else {
        setHint(L"");
    }
}

int Container::measureTabHeight()
{
    if (!_hTab || !::IsWindow(_hTab)) return 24;

    // 有页：直接量第一个标签项的真实高度（最准）
    if (TabCtrl_GetItemCount(_hTab) > 0) {
        RECT r{};
        if (TabCtrl_GetItemRect(_hTab, 0, &r) && r.bottom > r.top) {
            return (r.bottom - r.top) + 6;
        }
    }

    // 没页：用 TCM_ADJUSTRECT（传入窗口矩形，返回显示区矩形）反推标签条高度。
    // 0 个标签项时它也有效 —— 因为标签行高是由字体算的，跟有没有项无关。
    // ⚠️ 别图省事拍一个常数：字体随系统 DPI 与用户设置变，拍小了字就被裁掉
    //    （之前"字体显示不全"就是这么来的）。
    RECT rc{ 0, 0, 200, 200 };
    RECT disp = rc;
    if (TabCtrl_AdjustRect(_hTab, FALSE, &disp) && disp.top > rc.top) {
        return (disp.top - rc.top) + 6;
    }

    // 最后兜底：按字体度量算一行高度 + 上下内边距
    HFONT hf = reinterpret_cast<HFONT>(::SendMessageW(_hTab, WM_GETFONT, 0, 0));
    HDC dc = ::GetDC(_hTab);
    if (dc) {
        HGDIOBJ old = hf ? ::SelectObject(dc, hf) : nullptr;
        TEXTMETRICW tm{};
        int h = 0;
        if (::GetTextMetricsW(dc, &tm)) {
            h = tm.tmHeight + (int)tm.tmExternalLeading + 12;
        }
        if (old) ::SelectObject(dc, old);
        ::ReleaseDC(_hTab, dc);
        if (h > 0) return h;
    }
    return 28;
}

// ---------------------------------------------------------------------------
// 右键菜单
// ---------------------------------------------------------------------------
// 两个菜单，入口分得很清楚（这是用户明确要求的交互）：
//   · showAddAppMenu  —— 只有"右击**标签条空白处**"才弹，用来添加新应用；
//   · showTabItemMenu —— "右击**某个标签项**"，只用来关掉那一页。
// 内容区（空白页）右击什么都不弹。
// ---------------------------------------------------------------------------
void Container::showAddAppMenu(POINT screenPt)
{
    // 记一条日志：这是"用户能不能自己加应用"的唯一入口，
    // 出问题时最先要确认的就是"菜单到底有没有弹出来"。
    {
        wchar_t b[192];
        swprintf_s(b, L"弹出「添加应用」菜单（可用应用 %u 个，已开页 %u 个）at (%ld,%ld)",
                   (unsigned)_apps.size(), (unsigned)_pages.size(),
                   (long)screenPt.x, (long)screenPt.y);
        log(NPPDOCK_LOG_INFO, b);
    }

    HMENU m = ::CreatePopupMenu();
    if (!m) return;

    if (_apps.empty()) {
        ::AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
                      L"（同目录下未发现 NppDockApp_*.exe）");
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    } else {
        // ★ v1.3：菜单项**不加勾、不写"已开 N"**（王要求的）。
        //   理由：这里的每一项都是"再开一个新页"，勾选会误导成"这个是当前页"；
        //   而"已开几个"对用户没有决策价值 —— 想开几个就点几下。
        for (size_t i = 0; i < _apps.size(); ++i) {
            ::AppendMenuW(m, MF_STRING, kMenuAppBase + (UINT)i,
                          _apps[i].title.c_str());
        }
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }

    ::AppendMenuW(m, MF_STRING, kMenuRescan, L"重新扫描");

    // ⚠️ 老牌经典坑：TrackPopupMenu 之前必须先 SetForegroundWindow，
    //    之后必须 PostMessage(WM_NULL)。少任何一步，菜单在别的窗口被点击时
    //    不会自动消失，会"粘"在屏幕上。
    ::SetForegroundWindow(_hwnd);
    const int cmd = (int)::TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          screenPt.x, screenPt.y, 0, _hwnd, nullptr);
    ::DestroyMenu(m);
    ::PostMessageW(_hwnd, WM_NULL, 0, 0);

    if (cmd >= (int)kMenuAppBase) {
        openAppPage(cmd - (int)kMenuAppBase);
    } else if (cmd == (int)kMenuRescan) {
        // 已打开的应用页保持不动（进程还在跑），只刷新菜单数据
        discoverApps(_pluginDir);
    }
}

void Container::showTabItemMenu(POINT screenPt, int pageIdx)
{
    if (pageIdx < 0 || pageIdx >= (int)_pages.size()) return;

    // ★ v1.4：王要求「直接一个 Close 单词就行」—— 菜单只有一个条目，
    //   而且标签项就在鼠标底下，不需要再用标题去区分「关的是哪一个」。
    //   ★ v2.0：字换回中文。它原来是全中文界面里唯一的英文单词，
    //   看着像没做完的本地化。仍然**只有一个条目**（这条规矩没变）。
    const std::wstring label = L"关闭这一页";

    HMENU m = ::CreatePopupMenu();
    if (!m) return;
    ::AppendMenuW(m, MF_STRING, kMenuClosePage, label.c_str());

    ::SetForegroundWindow(_hwnd);
    const int cmd = (int)::TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                          screenPt.x, screenPt.y, 0, _hwnd, nullptr);
    ::DestroyMenu(m);
    ::PostMessageW(_hwnd, WM_NULL, 0, 0);

    if (cmd == (int)kMenuClosePage) {
        closePage(pageIdx);
    }
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
void Container::layout()
{
    if (!_hwnd) return;

    RECT rc{};
    ::GetClientRect(_hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const bool hasPages = !_pages.empty();

    // 标签条**常驻**：没有应用页时也显示。
    // 它是"添加新应用"的唯一入口（右击标签条空白处），所以不能藏。
    int contentTop = 0;
    if (_hTab && ::IsWindow(_hTab)) {
        const int tabH = measureTabHeight();
        ::ShowWindow(_hTab, SW_SHOW);
        ::SetWindowPos(_hTab, nullptr, 0, 0, w, tabH,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        contentTop = tabH;
    }

    if (_hContent && ::IsWindow(_hContent)) {
        ::SetWindowPos(_hContent, nullptr, 0, contentTop, w, h - contentTop,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    layoutPages(contentTop, w, h - contentTop);

    // 模块页：一旦有应用页就让位（两套页不叠加显示）
    for (size_t i = 0; i < _slots.size(); ++i) {
        HWND hv = _slots[i].hView;
        if (!hv || !::IsWindow(hv)) continue;
        if (hasPages || (int)i != _current) { ::ShowWindow(hv, SW_HIDE); continue; }

        RECT cr{};
        if (_hContent) ::GetClientRect(_hContent, &cr);
        ::SetWindowPos(hv, nullptr, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Container::layoutPages(int contentTop, int contentW, int contentH)
{
    // contentTop/W/H 目前不需要参与计算（页宿主直接铺满 _hContent 客户区），
    // 保留参数是为了日后要加边距/分隔条时不用改签名。
    (void)contentTop; (void)contentW; (void)contentH;

    if (_pages.empty() || !_hContent || !::IsWindow(_hContent)) return;

    RECT cr{};
    ::GetClientRect(_hContent, &cr);
    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;

    for (int i = 0; i < (int)_pages.size(); ++i) {
        AppPage& p = _pages[i];
        if (!p.hHost || !::IsWindow(p.hHost)) continue;

        if (i != _curPage) {
            ::ShowWindow(p.hHost, SW_HIDE);
            continue;
        }

        ::SetWindowPos(p.hHost, nullptr, 0, 0, w, h,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

        // 宿主尺寸变了，嵌入窗口要跟着走
        // （hostWndProc 的 WM_SIZE 也会做一次，这里兜住"尺寸没变但需要重新对齐"的情况）
        resizeEmbedded(p);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK Container::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // WM_NCCREATE 时（且只有那时）才能从 CREATESTRUCT 拿到 this
    Container* self = reinterpret_cast<Container*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<Container*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->_hwnd = hwnd;
    }

    if (self) {
        return self->WndProc(hwnd, msg, wp, lp);
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Container::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm && _hTab && nm->hwndFrom == _hTab) {
            if (nm->code == TCN_SELCHANGE) {
                switchToPage(TabCtrl_GetCurSel(_hTab));
                return 0;
            }
            // 注意：这里**没有** NM_RCLICK 分支。
            // 右击改由标签条的子类过程（tabWndProc）直接处理 —— 因为
            // NM_RCLICK 不区分"点在标签项上"还是"点在空白处"，
            // 而这两种位置的行为完全不同（关页 / 加应用）。
        }
        break;
    }

    case WM_SETFOCUS:
        // 焦点转给当前页视图。有应用页时不抢 —— 应用在自己的进程里管焦点，
        // 我们跨进程 SetFocus 本来就无效，抢一下只会让对面闪烁。
        if (_curPage < 0) {
            if (_current >= 0 && _current < (int)_slots.size()) {
                HWND hv = _slots[_current].hView;
                if (hv && ::IsWindow(hv)) { ::SetFocus(hv); return 0; }
            }
        }
        break;

    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    case WM_SHOWWINDOW:
        // ★ 容器自己被显示出来时（不管是谁触发的），嵌入的应用窗口不会自动重绘：
        //   · 它自己的 WS_VISIBLE 位没变（变的是**祖先**），所以它收不到
        //     WM_SHOWWINDOW —— 在应用那一侧挂这个钩子是没用的；
        //   · 尺寸也没变，所以走不到 resizeEmbedded，那三层补偿全不参与；
        //   · 跨进程子窗口也没有任何"谁帮我自动重画"的兜底。
        //   实测症状：隐藏后再唤出，应用整块是白的（暗像素 3.11% -> 0.18%）。
        //   ⚠️ 钩子必须挂在**容器**这里：宿主/外部可以绕过 NppDock 的 TogglePanel
        //      直接发 NPPM_DMMSHOW（探针、菜单命令、框架自己恢复面板都会），
        //      那样 Container::onShow 根本不会被调用。
        if (wp) {
            scheduleRepaint();
            // 面板"第一次被显示出来"时通知上层：上层用它做**延迟恢复**
            // （启动时面板是隐藏的就不恢复应用页，等它真被打开再恢复 ——
            //   这样"上次关着的情况下启动 N++"不用起任何应用进程）。
            if (_panelShownHook) _panelShownHook();
        }
        break;

    case WM_DESTROY:
        _hwnd = nullptr;
        _hContent = nullptr;
        _hTab = nullptr;
        // 容器都销毁了，应用进程没有理由继续活着。
        // 关掉作业句柄 = 内核立刻回收作业内所有进程（比逐个 TerminateProcess 更干脆）。
        if (_hJob) {
            ::CloseHandle(_hJob);
            _hJob = nullptr;
        }
        return 0;

    case WM_TIMER:
        if (_timerHook && _timerHook(wp)) return 0;
        if (wp == kTimerRepaintSettle) {
            // 第 3 层：尺寸已经停住，宿主那边的重绘压制也放开了 —— 现在补画一次。
            // 只作废（异步），不要 RDW_UPDATENOW，见 InvalidateCrossProcWindow 的警告。
            ::KillTimer(hwnd, kTimerRepaintSettle);
            repaintEmbeddedPages();
            return 0;
        }
        break;

    case kMsgRepaintEmbedded:
        // 第 2 层"补重画"：排在 resize 之后执行。
        // 只作废（异步）—— 曾经用 immediate=true 补第一排空洞，会死锁，
        // 见 InvalidateCrossProcWindow 的警告；第一排的问题现在由第 3 层解决。
        // 所有已打开的页都催一遍 —— 拖动时被隐藏的那些页切换回来时也要是好的。
        _repaintPosted = false;
        repaintEmbeddedPages();
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 内容区 ----
LRESULT CALLBACK Container::ContentThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Container* self = reinterpret_cast<Container*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<Container*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->contentWndProc(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Container::contentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    // ★ 这里**故意不处理 WM_CONTEXTMENU**：
    //   用户明确要求"右击空白处不产生任何功能"。添加应用的入口搬到了
    //   标签条的空白处（见 tabWndProc），所以内容区右击就什么都不做，
    //   交给 DefWindowProc（它也不弹任何菜单）。
    //
    //   【历史】早期这里是弹菜单的入口，当时还踩过一个坑：
    //   WM_CONTEXTMENU 的 lParam **本来就是屏幕坐标**（不是客户区坐标），
    //   却又调了一次 ClientToScreen，等于把内容区原点加了两遍，
    //   菜单被推到屏幕右下角 —— 现在这条路径已经不存在了，但坑值得记着。

    // 内容区是"空白页"：给它填上窗口底色，别露出宿主的默认灰
    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, ::GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }

    // ★ v2.0：内容区中央那一句话（见 setHint）。
    //   为什么非要自己画：内容区别的路径是"什么都不做"，所以"没页"和
    //   "应用起不来"这两件事在界面上原本**一点痕迹都没有**。
    //   画的时候顺带把底也铺了 —— 调用方可能是"只作废不擦"地重画的。
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect(dc, &rc, ::GetSysColorBrush(COLOR_WINDOW));

        if (!_hint.empty() && _pages.empty()) {
            HFONT f = ensureHintFont();
            HGDIOBJ old = nullptr;
            if (f) old = ::SelectObject(dc, f);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, ::GetSysColor(COLOR_GRAYTEXT));
            RECT t = rc;
            t.left  += 12;
            t.right -= 12;
            ::DrawTextW(dc, _hint.c_str(), -1, &t,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                        DT_NOPREFIX | DT_END_ELLIPSIS);
            if (old) ::SelectObject(dc, old);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        // 双击空白处分不出太多语义，保持沉默（避免误触）
        return 0;

    default:
        break;
    }
    (void)wp;
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// v2.0：内容区中央那句话
// ---------------------------------------------------------------------------
void Container::setHint(const std::wstring& s)
{
    if (_hint == s) return;
    _hint = s;
    // v2.0：记一行 —— 这句话画在内容区中央（不是控件），外部看不见；
    //   而且"应用起不来"这类事只有日志+这句话两边能对上。
    if (!s.empty()) log(NPPDOCK_LOG_INFO, L"内容区提示：" + s);
    if (_hContent && ::IsWindow(_hContent))
        ::InvalidateRect(_hContent, nullptr, FALSE);
}

HFONT Container::ensureHintFont()
{
    if (_hHintFont) return _hHintFont;
    // 跟标签条一样跟随系统消息字体（不自己定字号 —— 那是 150% 缩放下最容易出错的地方）
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        _hHintFont = ::CreateFontIndirectW(&ncm.lfMessageFont);
    return _hHintFont;
}

// ---- 应用宿主 ----
LRESULT CALLBACK Container::HostThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Container* self = reinterpret_cast<Container*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<Container*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->hostWndProc(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 标签条（子类化：全靠它分辨"标签项"与"空白处"）----
LRESULT CALLBACK Container::TabThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Container* self = reinterpret_cast<Container*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        // 理论上不可达（子类化时就把 this 写进去了）。真到了这儿，
        // 只能走默认处理 —— 千万别拿空指针去 CallWindowProc。
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->tabWndProc(hwnd, msg, wp, lp);
}

LRESULT Container::tabWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    // ⚠️ 高亮横杠就靠这一条：**先让控件按原样画完，再补一笔**。
    //
    //   顺序不能反。反过来（先画后让原生画）会被原生画法整块盖掉，什么都看不见。
    //   也不能改成在别处"定时补画"：那样每次原生重画（换页、改尺寸、被遮挡后
    //   暴露）之后绿杠就消失，得等下一次定时器 —— 表现为"绿杠一闪一闪"。
    //   挂在 WM_PAINT 里最省心：**凡是这个控件重画，绿杠必然跟着重画**。
    case WM_PAINT: {
        const LRESULT r = _tabOrigProc
            ? ::CallWindowProcW(_tabOrigProc, hwnd, msg, wp, lp)
            : ::DefWindowProcW(hwnd, msg, wp, lp);
        drawTabStripSeparator(hwnd, nullptr);   // 先画分界线，绿杠再压上去
        drawActiveTabBar(hwnd, nullptr);
        return r;
    }

    // "把窗口渲染到 DC"（窗口缩略图、PrintWindow、自动化测试）——
    // 同样先让原生画完，再补绿杠。见 drawActiveTabBar 的说明：
    // 少了这条，那些场合抓到的是"没有绿杠"的标签条。
    case WM_PRINTCLIENT: {
        const LRESULT r = _tabOrigProc
            ? ::CallWindowProcW(_tabOrigProc, hwnd, msg, wp, lp)
            : ::DefWindowProcW(hwnd, msg, wp, lp);
        if (wp && (lp & PRF_CLIENT)) {
            drawTabStripSeparator(hwnd, (HDC)wp);
            drawActiveTabBar(hwnd, (HDC)wp);
        }
        return r;
    }

    // ⚠️⚠️ 真机踩到，而且非常费劲才定位 —— 记牢：
    //
    //   comctl32 的标签条在**空白区**（没有标签项的地方）对 WM_NCHITTEST
    //   返回 HTTRANSPARENT(-1)，意思是"这一点不算我的"。
    //   于是 Windows 把鼠标消息**直接交给父窗口**，标签条根本收不到
    //   "右击空白处"这个动作 —— 表现就是"右击标签条空白处毫无反应"。
    //
    //   排查经过（别的坑也可能长这样，值得记住这个套路）：
    //     · 日志显示右击处理函数压根没被调用；
    //     · WindowFromPoint / ChildWindowFromPointEx 都说是标签条 —— 骗人；
    //     · 直接 SendMessage(WM_RBUTTONDOWN) 给标签条 —— **正常触发**，
    //       说明子类化没装错、判断逻辑也没错；
    //     · 那就只剩"真实鼠标输入与 SendMessage 的差别"这一条：
    //       真实输入要先过**命中测试**，SendMessage 不用。
    //       → 查 WM_NCHITTEST，果然是 -1（HTTRANSPARENT）。
    //
    //   解法：只要点落在标签条的窗口矩形里，一律回答 HTCLIENT，把输入留下。
    //   （标签项自己的选中逻辑走的是 WM_LBUTTONDOWN + 内部命中测试，
    //     不依赖 WM_NCHITTEST，所以这么改不会影响切标签。）
    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // 这里 lParam 是屏幕坐标
        RECT rc{};
        ::GetWindowRect(hwnd, &rc);
        if (pt.x >= rc.left && pt.x < rc.right &&
            pt.y >= rc.top  && pt.y < rc.bottom) {
            return HTCLIENT;
        }
        break;
    }

    // ---- 标签拖动排序 ----
    // 做在标签条自己的子类过程里（comctl32 没有"移动标签项"的 API，
    // 页序是我们自己的 _pages 数组，所以只能自己接鼠标自己排）。
    //
    // 三条纪律：
    //   ① 只有"按在**应用页**标签项上"才可能是拖动（功能模块页的位置不动）；
    //   ② 位移超阈值才算拖动 —— 否则"点一下切页"会被当成拖动把顺序搅乱；
    //   ③ 拖动**不改变选中页**：选中跟着页对象走（见 Container::movePage）。
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        TCHITTESTINFO hti{};
        hti.pt = pt;
        const int item = TabCtrl_HitTest(hwnd, &hti);
        const int moduleCount = (int)_slots.size();
        if (item >= moduleCount && item < moduleCount + (int)_pages.size()) {
            _dragSrcPage = item - moduleCount;
            _dragStart   = pt;
            _dragging    = false;
            // 冻结所有标签项的槽位边界（同进程调用，TCM_GETITEMRECT 安全）
            _dragSlotRects.clear();
            const int total = TabCtrl_GetItemCount(hwnd);
            for (int i = 0; i < total; ++i) {
                RECT r{};
                if (TabCtrl_GetItemRect(hwnd, i, &r)) _dragSlotRects.push_back(r);
            }
        } else {
            _dragSrcPage = -1;
            _dragSlotRects.clear();
        }
        break;   // 不 return：原过程还要用这条消息做"选中该标签项"
    }

    case WM_MOUSEMOVE: {
        if (_dragSrcPage < 0) break;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!_dragging) {
            const int dx = pt.x - _dragStart.x, dy = pt.y - _dragStart.y;
            if (dx * dx + dy * dy < 16) break;      // < 4px 视作点击
            _dragging = true;
            log(NPPDOCK_LOG_INFO, L"开始拖动标签页");
        }
        // 用**冻结的槽位**判目标（见头文件里的说明：实时命中会来回抖）
        const int moduleCount = (int)_slots.size();
        int slot = -1;
        for (size_t i = 0; i < _dragSlotRects.size(); ++i) {
            const RECT& r = _dragSlotRects[i];
            if (pt.x >= r.left && pt.x < r.right) { slot = (int)i; break; }
        }
        if (slot < 0 && !_dragSlotRects.empty()) {
            // 落在标签项之外：按最近的一侧算（往左拖出界 = 放到最前）
            slot = (pt.x < _dragSlotRects.front().left) ? 0
                                                       : (int)_dragSlotRects.size() - 1;
        }
        const int target = slot - moduleCount;
        if (target >= 0 && target < (int)_pages.size() &&
            target != _dragSrcPage) {
            const int from = _dragSrcPage;
            if (movePage(from, target)) {
                _dragSrcPage = target;              // 拖到哪就从哪接着拖
                layout();
                wchar_t b[128];
                swprintf_s(b, L"标签页重排：第 %d 位 -> 第 %d 位",
                           from + 1, target + 1);
                log(NPPDOCK_LOG_INFO, b);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        _dragSrcPage = -1;
        _dragging    = false;
        _dragSlotRects.clear();
        break;

    // ---- 双击标签项 = 关闭那一页（王 v1.4 要的）----
    //
    // 为什么能收到这条消息：SysTabControl32 的窗口类带 CS_DBLCLKS
    //（它能发 NM_DBLCLK 就是证据），所以双击会走 WM_LBUTTONDBLCLK。
    // 与拖动的次序是"DOWN / UP / DBLCLK / UP" —— 中间没有位移，
    // 不会误触发拖动排序。
    //
    // ⚠️ 第一次点击已经让这一页变成当前页了（comctl32 自己做的），
    //    所以"双击关掉一个后台页"实际上会先切过去再关掉；这是可接受的
    //    （用户点的就是它），而且和右击菜单「Close」的结果一致。
    case WM_LBUTTONDBLCLK: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        TCHITTESTINFO hti{};
        hti.pt = pt;
        const int item = TabCtrl_HitTest(hwnd, &hti);
        const int moduleCount = (int)_slots.size();
        const int page = item - moduleCount;
        if (page >= 0 && page < (int)_pages.size()) {
            wchar_t b[192];
            swprintf_s(b, L"标签条双击：关闭第 %d 页（%s）",
                       page + 1, _pages[page].title.c_str());
            log(NPPDOCK_LOG_INFO, b);
            closePage(page);
        }
        return 0;   // 双击不再往下传（原生会发 NM_DBLCLK，我们不需要）
    }

    case WM_RBUTTONUP: {
        // 为什么要自己接 WM_RBUTTONUP，而不用控件发的 NM_RCLICK：
        //   NM_RCLICK 只说"右击了控件"，**不告诉你点在标签项上还是空白处**。
        //   而这里必须分清楚：
        //     · 标签项上 -> 关闭那一页
        //     · 空白处   -> 添加新应用（用户要求的唯一入口）
        //   所以自己拿客户区坐标做一次命中测试（-1 表示不在任何标签项上）。
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT screen = pt;
        ::ClientToScreen(hwnd, &screen);

        TCHITTESTINFO hti{};
        hti.pt = pt;
        const int item = TabCtrl_HitTest(hwnd, &hti);

        {
            wchar_t b[192];
            swprintf_s(b, L"标签条右击：客户区(%ld,%ld) 命中项=%d 页数=%d",
                       (long)pt.x, (long)pt.y, item, (int)_pages.size());
            log(NPPDOCK_LOG_INFO, b);
        }

        // 注意判据写的是"命中了**已存在**的页"而不是"item >= 0"：
        // 没有页时命中值没有意义，不能凭它去关一个不存在的页。
        if (item >= 0 && item < (int)_pages.size()) {
            showTabItemMenu(screen, item);
        } else {
            showAddAppMenu(screen);
        }

        // 吃掉这条消息：不往下传，comctl32 就不会再补发一个 NM_RCLICK，
        // 否则同一份菜单会弹两遍。
        return 0;
    }

    case WM_NCDESTROY:
        // 标签条要没了：把窗口过程还回去再让它继续销毁。
        // （不还也能跑，但保留原过程的习惯能避免"类被卸载后回调悬空"，代价为零）
        if (_tabOrigProc) {
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)_tabOrigProc);
            WNDPROC orig = _tabOrigProc;
            _tabOrigProc = nullptr;
            return ::CallWindowProcW(orig, hwnd, msg, wp, lp);
        }
        break;

    default:
        break;
    }
    if (_tabOrigProc) {
        return ::CallWindowProcW(_tabOrigProc, hwnd, msg, wp, lp);
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Container::hostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (AppPage* p = findPageByHost(hwnd)) {
            resizeEmbedded(*p);
        }
        return 0;

    // 同 ContentPane：宿主右击也**不弹菜单**（"右击空白处不产生任何功能"）。
    // 宿主要么被应用窗口完全盖住，要么是应用还没嵌上来的短暂空窗期。

    case WM_ERASEBKGND: {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        ::FillRect((HDC)wp, &rc, ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    case WM_SETFOCUS:
        // 跨进程 SetFocus 无效，交给用户点击时由系统处理，这里不折腾
        return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// hostApi 实现
// ---------------------------------------------------------------------------
void Container::log(int level, const std::wstring& msg) const
{
    if (_logPath.empty()) return;
    AppendLog(_logPath, level, msg.c_str());
}

void Container::fillHostApi()
{
    _api = NppDockHostApi{};
    _api.abiVersion = NPPDOCK_ABI_VERSION;

    _api.getNppHandle = []() -> HWND {
        Container* c = s_instance;
        return c ? c->_nppHandle : nullptr;
    };
    _api.getContainerHandle = []() -> HWND {
        Container* c = s_instance;
        return c ? c->_hwnd : nullptr;
    };

    _api.getPluginDir = [](wchar_t* buf, uint32_t cap) -> uint32_t {
        Container* c = s_instance;
        if (!c) return 0;
        if (!buf || cap == 0) return (uint32_t)c->_pluginDir.size();
        CopyToBuffer(EnsureTrailingSlash(c->_pluginDir), buf, cap);
        return (uint32_t)EnsureTrailingSlash(c->_pluginDir).size();
    };
    _api.getConfigRoot = [](wchar_t* buf, uint32_t cap) -> uint32_t {
        Container* c = s_instance;
        if (!c) return 0;
        if (!buf || cap == 0) return (uint32_t)c->_configRoot.size();
        CopyToBuffer(c->_configRoot, buf, cap);
        return (uint32_t)c->_configRoot.size();
    };
    _api.getOwnConfigRoot = [](wchar_t* buf, uint32_t cap) -> uint32_t {
        Container* c = s_instance;
        if (!c) return 0;
        if (!buf || cap == 0) return (uint32_t)c->_ownConfigRoot.size();
        CopyToBuffer(c->_ownConfigRoot, buf, cap);
        return (uint32_t)c->_ownConfigRoot.size();
    };
    _api.getLogFilePath = [](wchar_t* buf, uint32_t cap) -> uint32_t {
        Container* c = s_instance;
        if (!c) return 0;
        if (!buf || cap == 0) return (uint32_t)c->_logPath.size();
        CopyToBuffer(c->_logPath, buf, cap);
        return (uint32_t)c->_logPath.size();
    };

    _api.ensureDirectory = [](const wchar_t* dir) -> int {
        return (dir && EnsureDirectory(dir)) ? 0 : 1;
    };
    _api.ensureFileWithDefault = [](const wchar_t* path, const char* def, uint32_t len) -> int {
        if (!path || !def) return 1;
        std::string d(def, def + len);
        return EnsureFileWithDefault(path, d) ? 0 : 1;
    };
    _api.openWithDefaultApp = [](const wchar_t* path) -> int {
        return (path && OpenWithDefaultApp(path)) ? 0 : 1;
    };
    _api.readFileAll = [](const wchar_t* path, char* buf, uint32_t cap, uint32_t* outLen) -> int {
        if (!path) return 1;
        std::string data;
        if (!ReadFileAll(path, data)) return 2;
        if (outLen) *outLen = (uint32_t)data.size();
        if (!buf || cap == 0) return 0;             // 只查询长度
        if (data.size() > cap) return 3;            // 缓冲不够
        memcpy(buf, data.data(), data.size());
        return 0;
    };

    _api.logEx = [](int level, const wchar_t* msg) {
        Container* c = s_instance;
        if (c && msg) c->log(level, msg);
    };
    _api.log = [](const wchar_t* msg) {
        Container* c = s_instance;
        if (c && msg) c->log(NPPDOCK_LOG_INFO, msg);
    };

    _api.requestShow = [](void* self) {
        Container* c = s_instance;
        if (c) c->requestShow(reinterpret_cast<NppDockModule*>(self));
    };
    _api.setDirtyTitle = [](void* self, const wchar_t* title) {
        Container* c = s_instance;
        if (c && title) c->setDirtyTitle(reinterpret_cast<NppDockModule*>(self), title);
    };
    _api.requestRelayout = [](void*) {
        Container* c = s_instance;
        if (c) c->layout();
    };

    _api.destroyModule = [](NppDockModule* self) {
        // 给模块自己在内部想销毁时用。走和 unloadModule 同一条路（模块导出函数）。
        if (!self) return;
        Container* c = s_instance;
        if (c) {
            for (auto& s : c->_slots) {
                if (s.instance == self && s.hModule) {
                    auto fn = reinterpret_cast<PFN_nppdock_module_destroy>(
                        ::GetProcAddress(s.hModule, NPPDOCK_DESTROY_SYMBOL));
                    if (fn) fn(self);
                    return;
                }
            }
        }
    };

    _api.showPanel = [](void) -> BOOL {
        Container* c = s_instance;
        if (!c || !c->_nppHandle || !c->_hwnd) return FALSE;
        return (BOOL)::SendMessageW(c->_nppHandle, NPPM_DMMSHOW, 0, (LPARAM)c->_hwnd);
    };
    _api.hidePanel = [](void) -> BOOL {
        Container* c = s_instance;
        if (!c || !c->_nppHandle || !c->_hwnd) return FALSE;
        return (BOOL)::SendMessageW(c->_nppHandle, NPPM_DMMHIDE, 0, (LPARAM)c->_hwnd);
    };
    _api.isPanelVisible = [](void) -> BOOL {
        Container* c = s_instance;
        if (!c) return FALSE;
        return c->isPanelVisible() ? TRUE : FALSE;
    };
}

} // namespace nppdock
