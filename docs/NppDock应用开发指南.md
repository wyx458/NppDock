# NppDock 应用开发指南

**往 NppDock 面板里塞一个自己写的程序，要遵守哪些规则、违反了会怎样。** 读者：要写/改一个嵌进 dock 的工具的人。

配套阅读：dock 本身怎么工作/装/排查 → `plugins/NppDock/README.md`；某个坑的完整排查 → `docs/铁律与踩坑要点.md`；工作区总览 → `pluginsWorkspace/README.md`；完整参考实现 → `plugins/NppDock/src/apps/md5tool/`。

---

## 1. 一分钟理解整个框架

NppDock 是 Notepad++ 的底部停靠面板，本身**没有功能**，只提供"内容区"。放东西有两条路：

```
                     ┌──────────────────────────── Notepad++ 进程 ───────────────────────────┐
  方式 A             │   NppDock.dll                                                         │
  功能模块           │     ├─ 容器面板 NppDockContainerWnd                                    │
  NppDock_XXX.dll    │     │    ├─ 标签条（常驻）                                             │
  ─ 同进程           │     │    └─ 内容区 NppDockContentPane                                 │
  ─ 直接建子窗口     │     │         ├─ 模块页：NppDock_XXX.dll 在这里建窗口  ◄── 方式 A        │
                     │     │         └─ 应用宿主 NppDockAppHost                            │
                     └─────┼──────────────────┬────────────────────────────────────────────┘
                           │        ┌─────────┴──────────── 应用自己的进程 ────────────────┐
  方式 B                   │        │  NppDockApp_XXX.exe                                  │
  可嵌入应用               │        │    建自己的窗口 -> 改成 WS_CHILD -> SetParent 到宿主  │
  NppDockApp_XXX.exe       │        └──────────────────────────────────────────────────────┘
  ─ 独立进程，跨进程 SetParent
```

|  | 方式 A：功能模块 | 方式 B：可嵌入应用 |
|---|---|---|
| 产物 / 跑在哪 | `NppDock_XXX.dll`，**N++ 进程内** | `NppDockApp_XXX.exe`，**独立进程** |
| 界面怎么进去 / 能拿宿主 API 吗 | 直接 `CreateWindowEx` 建在内容区；能（`NppDockHostApi`） | 自己窗口 `SetParent` 到宿主；**不能**，只能靠字符串契约 |
| 崩了会怎样 / 对接复杂度 | 拖垮 Notepad++；中（守跨 DLL `/MT` 铁律） | 只死自己；低（一个命令行参数） |

**本指南只讲方式 B**：每个工具先做成能单独跑的 exe，再让它顺带能被 dock 嵌 —— 好处是**工具本身不依赖 Notepad++**，可单开、发给别人、命令行调用。

---

## 2. 契约：只有一条字符串

```
NppDockApp_XXX.exe --dock-parent <HWND十进制>
```

dock 把内容区的宿主窗口句柄用命令行参数告诉你，**剩下的全由应用自己搞定**。

> **应用里不许出现任何来自 dock 的 `#include`、常量或函数。** 需要的那几个字符串（前缀、后缀、开关名）**在自己这边抄一遍**。

权威定义在 `plugins/NppDock/src/main/NppDockEmbed.h`：前缀 `NppDockApp_`、后缀 `.exe`、开关 `--dock-parent`。为什么不用共享头文件：`#include NppDockApi.h` 会把两边版本绑死（改 dock 一行常量所有应用都重编）；链接 dock 实现则跨 `/MT` 堆不同、`delete` 必炸。只认一个参数 ⇒ 应用可**单独编译/发布/运行**，不装 dock 时就是普通工具。

---

## 3. 必须遵守的规则（R1–R15）

每条给 **规则 → 症状**；症状是重点，都是"不报错、但表现诡异"的类型。可用模板见 §5。

### R1 · 解析命令行必须用 `GetCommandLineW()`
用 `CommandLineToArgvW(GetCommandLineW(), &argc)`；**不要**用 `wWinMain` 的 `lpCmdLine`（**不含程序名**，而 `CommandLineToArgvW` 按含 `argv[0]` 解析）。
**症状**：整体错位一格，`--dock-parent` 永远匹配不上 → 程序**静默**退化成独立顶层窗口（日志无异常，最难查）。

### R2 · 建任何窗口**之前**先对齐 DPI 感知
`AdoptHostDpiAwareness(hDockParent)`，**必须在 `CreateWindowEx` 之前**。
**症状**：跨进程 `SetParent` 触发系统 **forced reset**，窗口只铺满约 **2/3**、右/下露白（客户区真实 1196×524，量成 797×349）。

### R3 · 子窗口化顺序：改样式 → `SetParent` → `SetWindowPos`
先去 `WS_POPUP`/加 `WS_CHILD`（`SetWindowLongPtrW(GWL_STYLE)`），再 `SetParent(h, host)`，最后按 `GetClientRect(host)` 铺满（`SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOACTIVATE`）。建议加 `WS_EX_TOOLWINDOW`（嵌入态别在任务栏/Alt-Tab 露头）。
**症状**：闪一下、偶发尺寸不生效。

### R4 · 布局必须弹性，能在"很窄很矮"下活
坐标在 `WM_SIZE` 里按 `GetClientRect` 现算、**绝不写死**；尺寸/间距/字号经 DPI 换算；行高/按钮宽/标签列宽**实测**（`GetTextMetrics`/`GetTextExtentPoint32`）。高度不够时按"先压行距 → 再压行高（留下限）→ 最后压状态行"顺序压，**控制行优先**。独立打开 640px 起，嵌 dock 可能只有 **300 多 px 宽、150px 高**。
**症状**：文字被裁、按钮字显示不全。

### R5 · 必须能被干净地结束
`WM_CLOSE → DestroyWindow`；`WM_DESTROY → PostQuitMessage`。dock 关标签页发 `WM_CLOSE` 并等 3 秒（超时才强杀）；Job Object 兜底只是保险。
**症状**：关页后进程残留、任务管理器堆一串 `NppDockApp_*.exe`。

### R6 · 重绘自己负责，且**永不**跨进程同步重画
尺寸变化/被重新显示后作废**整棵子树**：`RedrawWindow(h,nullptr,nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN)`。`InvalidateRect(自己)` 不够（内容全在子控件）；**绝对不要 `RDW_UPDATENOW`** —— 跨进程要求"立刻画完"会与对面互等 → **整个 N++ 死锁**（不是慢，是永远不动）。触发点挂 `WM_SHOWWINDOW`（`wp!=0` 及 `lp!=0`）**和** `WM_WINDOWPOSCHANGED`（不能只挂一处）；要重画用 `PostMessage` **排队列尾**再作废（这两个消息在显示动作中间发出，当场作废会被丢）。
**症状**：隐藏再显示后只剩一块底色。

### R7 · 版本资源里的 `FileDescription` 就是标签页标题
`VALUE "FileDescription", "文件校验"` → dock 读它当 tab 标题；读不到退化为"文件名去前缀"（文件名也别乱起）。改标题只改资源，不动 dock 代码。

### R8 · 产物命名与位置有硬要求
文件名 `NppDockApp_<名字>.exe`、与 `NppDock.dll` **同目录**（dock 按前缀扫目录发现）、子系统 `/SUBSYSTEM:WINDOWS`+`wWinMain`。
> ⚠️ 产物名是持久化状态的键：`panel.ini` 记的是 exe **文件名**（不是下标，下标会随目录增删漂移）。改名会让"重开 N++ 接回上次的页"失效。

### R9 · 长任务要能中止（推荐）
循环里泵消息、读取消标志（零线程零锁）：`for(;;){ ReadChunk(); Update(); PumpMessages(); if (cancel) 收尾; ReportProgress(); }`。只有一个按钮时可**变身**（「计算」↔「终止」），不占横向空间。**收尾必须完整**：恢复被禁用控件、按钮文字变回来、不留上一次结果。

### R10 · 运行期要填的静态表，**不能**声明成 `const`
用 `static ValueList VL = {...}`（非 const）。`const` 会进只读段，写它 `0xC0000005`；`const_cast` 去掉 const **不是**安全逃生门。
**症状**：GUI 窗口根本起不来（进程起 ~200ms 就退），WER 只给 `c000041d`。**找现场**：`wWinMain` 第一行 `SetUnhandledExceptionFilter`，把 code/addr 写进 exe 旁 `.trace.log`；`WM_CREATE` 里分段 `TraceLog`。

### R11 · 界面有"随选择变化的部分"时，重建必须用**当前选择**
① 重建配置区别拿默认参数（例：连接表选 UDP 要少一项"筛选"，写 `Params p{}` 会让这次选择等于没发生）。② 一份数据有"内存/文件/控件"三份呈现就**三份都刷**（例：目标记忆写进内存+盘、却没刷下拉框 → 用户选不到）。自查："**用户能看见的改变，界面上是不是也变了？**"

### R12 · 字号只能"取当前 DPI 的那一份"，**绝不能**再乘一次比例
`SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, ..., g_dpi)` 取一份直接 `CreateFontIndirectW`（老系统才退回补正**一次**）。系统给的字号**已经缩放过了**，再 `lfHeight*dpi/96` 恰好放大 1.5 倍、**不报错**。配套：**等宽字体取正文 `lfHeight`（em），不要 `tmHeight`**（后者含行距，中文字体行距大）；改完**量真实值**（`WM_GETFONT`+`GetObject`）。**重写整个文件时，旧版里有血泪注释的段落要原样搬过来。**

### R13 · 跨进程检查控件：**"它自己可见" ≠ "这一页此刻被显示"**
多页共存时**非当前页被宿主整页藏起来**，`IsWindowVisible(控件)` 返回 False 是正常的；判"控件自己可见"用 `GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE`。同源：**锁屏时鼠标/键盘注入到不了目标窗口**，测试脚本要能识别并 **SKIP（退出码 3）** 而不是 FAIL。

### R14 · 拖入/拖出：UIPI 会**静默**吃掉消息，必须显式放行
除 `DragAcceptFiles`（**每个子控件都要登记**），还要对每个窗口 `ChangeWindowMessageFilterEx(h, WM_DROPFILES, MSGFLT_ALLOW, nullptr)`（另加 `WM_COPYDATA`、`0x0049 WM_COPYGLOBALDATA`）；`appui::AllowDropMessages(h)` 有现成的。**纪律**：这条链路失败什么都没有，应用要留一条轨迹（"已放行 UIPI"）。

### R15 · DPI 感知只有一次机会，`SetProcessDpiAwareness` 在 shcore.dll
建窗口**之前**定下来、一进程只一次。顺序：宿主上下文 → `SetProcessDpiAwarenessContext(-4)`（user32，PMv2）→ **shcore.dll** 的 `SetProcessDpiAwareness(2)` → `SetProcessDPIAware()`。⚠️ 在 user32 里找 `SetProcessDpiAwareness` **永远 NULL、静默失败** → unaware（字发虚、坐标被虚拟化）。直接用 `appui::AdoptHostDpiAwareness(hDockParent)`。

---

## 4. 反例速查

各条"错误做法 → 症状"已逐条写在 **R1–R15** 里，按规则号查。补充一条不在 R 里的：**完整性级别与宿主不同** ⇒ `SetParent` **静默失败**（返回成功但界面毫无变化）。

---

## 5. 最小可编译骨架

放在 `plugins/NppDock/src/apps/<名字>/`，已满足 R1~R8。

```cpp
// NppDockApp_Template.cpp —— 能嵌进 NppDock 的最小应用
#include <windows.h>
static HWND g_hWnd = nullptr;

static HWND ParseDockParent() {                       // R1
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);   // lpCmdLine 不含程序名
    if (!argv) return nullptr;
    HWND h = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"--dock-parent") == 0) {               // 契约字符串，抄一遍
            h = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_wcstoui64(argv[i+1], nullptr, 10)));
            break;
        }
    ::LocalFree(argv);
    return h;
}
typedef HANDLE (WINAPI* PFN_GetWindowDpiAwarenessContext)(HWND);
typedef BOOL   (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
static void AdoptHostDpiAwareness(HWND host) {        // R2：必须在建窗口之前
    HMODULE u = ::GetModuleHandleW(L"user32.dll"); if (!u) return;
    auto setCtx = (PFN_SetProcessDpiAwarenessContext)::GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (!setCtx) return;                              // Win10 1607 以下没有：动态取，同一二进制照样跑
    if (host) {
        auto getCtx = (PFN_GetWindowDpiAwarenessContext)::GetProcAddress(u, "GetWindowDpiAwarenessContext");
        if (getCtx) { HANDLE ctx = getCtx(host); if (ctx && setCtx(ctx)) return; }   // 抄宿主的
    }
    setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));       // 兜底：per-monitor-v2
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:                                   // 必须在 CreateWindowEx 返回前就填 g_hWnd，
        g_hWnd = h; return 0;                         // 否则子控件成"以 NULL 为父的孤儿"
    case WM_SIZE:                                     // R4：这里按客户区重排控件
        ::RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN); return 0;  // R6
    case WM_SHOWWINDOW: case WM_WINDOWPOSCHANGED:
        ::PostMessageW(h, WM_APP + 1, 0, 0); break;   // 排队列尾，别当场重画
    case WM_APP + 1:
        ::RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN); return 0;
    case WM_CLOSE:   ::DestroyWindow(h); return 0;    // R5
    case WM_DESTROY: ::PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: {
        RECT rc{}; ::GetClientRect(h, &rc);
        ::FillRect((HDC)w, &rc, ::GetSysColorBrush(COLOR_BTNFACE)); return 1; }
    }
    return ::DefWindowProcW(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    HWND host = ParseDockParent();
    const bool embedded = (host != nullptr);
    AdoptHostDpiAwareness(host);                      // R2：必须在建窗口之前
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst; wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE); wc.lpszClassName = L"NppDockAppTemplateWnd";
    if (!::RegisterClassExW(&wc)) return 1;
    // 样式一次到位（先建顶层再改子窗口，中间那一瞬会在桌面上闪一下）
    g_hWnd = ::CreateWindowExW(embedded ? WS_EX_TOOLWINDOW : 0, L"NppDockAppTemplateWnd", L"我的应用",
        embedded ? WS_POPUP : WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 220,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) return 2;
    if (embedded) {                                   // R3：顺序不能改
        LONG_PTR st = ::GetWindowLongPtrW(g_hWnd, GWL_STYLE);
        st &= ~(WS_POPUP|WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX);
        st |= WS_CHILD; ::SetWindowLongPtrW(g_hWnd, GWL_STYLE, st);
        ::SetParent(g_hWnd, host);
        RECT rc{}; ::GetClientRect(host, &rc);
        ::SetWindowPos(g_hWnd, nullptr, 0, 0, rc.right > 0 ? rc.right : 400,
                       rc.bottom > 0 ? rc.bottom : 180, SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    ::ShowWindow(g_hWnd, SW_SHOW); ::UpdateWindow(g_hWnd);
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
    return 0;
}
```

配套 `NppDockApp_Template.rc`（R7）：`FILEVERSION 1,0,0,0` / `VOS_NT_WINDOWS32` / `VFT_APP`，`StringFileInfo` 块 `080404b0`（简体中文, Unicode）里 `VALUE "FileDescription", "我的应用"`（dock 读它当 tab 标题）、`FileVersion`、`ProductName`；`VarFileInfo` 里 `Translation 0x804, 1200`。

---

## 6. 怎么验证（别靠肉眼）

跨进程嵌入的 bug 都是"不报错、只是看起来不对"，一律走脚本。

**6.1 给业务逻辑留命令行自检口**：有"算错了也不报错"的逻辑（摘要、编解码、换算）就**必须**留一个不进 UI 的自检开关，让脚本和权威实现对照：`NppDockApp_MD5.exe --selftest foo.bin --algo sha256` ↔ `hashlib.sha256(data).hexdigest() == app_output`。

**6.2 用探针真机跑一遍**：`plugins/NppDock/tools/dock_app_probe.py` 用**真实鼠标事件**驱动（不是 `PostMessage` 假装点击）。必查项：版式不变量（读 `GetWindowRect` 比对）；字号/字体（`WM_GETFONT`+`GetObjectW(LOGFONTW)` 读 `lfHeight`）；控件真的建出来了（枚举子窗口+可见性+尺寸）；关闭后无残留（`tasklist` **差值**）；长任务中止（用**大文件**制造足够长的窗口）。

**6.3 回归护栏**：

```bat
cd build\tests && test_integration.exe          :: 离线集成 32 条断言（不用开 N++）
python tools\dock_app_probe.py toggletest       :: 隐藏 -> 唤出，界面还在吗
python tools\dock_app_probe.py dragsplitter --dy 80 --steps 12   :: 真拖分隔条，全程不白
python tools\dock_app_probe.py multiopen        :: 多页：页数/进程数各 +1，收尾回到起点
python tools\dock_app_probe.py nettest          :: 「网络测试」这一页的总验收（8 小节）
```

> 驱动长任务：**点开始只能异步发（`PostMessage`）**，并**轮询两个状态**（先等真跑起来、再等跑完）；同步 `SendMessage` 会永远卡住，只等"结束"会在"还没开始"时误判通过（见 `docs/铁律与踩坑要点.md` 17.2）。
> ⚠️ `toggletest`/`dragsplitter` 靠**抓屏**判断，需 **PIL**；桌面太乱会报"测量无效"而不是假通过。

---

## 7. 构建与部署

`plugins/NppDock/build.py` 里 `APPS` 表加一行（`name`→产物 `NppDockApp_<name>.exe`、`src`、`rc`、`libs`）完成接入：

```python
APPS = [
  {"name":"MD5", "src":["src/apps/md5tool/md5tool.cpp","src/apps/md5tool/hashcore.cpp"],
   "rc":"src/apps/md5tool/md5tool.rc", "libs":["user32.lib","kernel32.lib","gdi32.lib","comdlg32.lib","shell32.lib"]},
  {"name":"NET", "src":["src/apps/nettest/nettest.cpp","src/apps/nettest/netcore.cpp"],
   "rc":"src/apps/nettest/nettest.rc",
   # 系统 DLL 一律允许：iphlpapi=ICMP/ARP/适配器/连接表，ws2_32=解析+TCP，dnsapi=DNS。
   # --check-deps 只禁 MSVCP*/VCRUNTIME*/MSVCR*/api-ms-win-crt*（VC 运行库）。
   "libs":["user32.lib","kernel32.lib","gdi32.lib","iphlpapi.lib","ws2_32.lib","dnsapi.lib"]},
]
```

```bat
python build.py              :: 编译主 DLL + 所有应用 + 测试
python build.py --check-deps :: 额外 dumpbin /dependents 验证 /MT
python build.py --install    :: 构建并部署到便携版 Notepad++
```

统一 `COMMON_FLAGS`：`/MT` 静态 CRT、`/utf-8`、`/W4`、`/GS /guard:cf`。**不要**给单个应用加 `/MD`（会依赖目标机 VC 运行库、插件静默不加载）。**两条部署纪律**：① 部署的必须是**被验证过的那一份**二进制（构建完先 `md5sum` 比对再拷贝）；② `md5sum` 不一致 ≠ 代码变了（PE 头链接时间戳不同）。`--install` 会先删目标目录旧 `NppDockApp_*.exe`（残留会被扫成打不开的菜单项）。**热替换**：exe 在跑时直接覆盖会失败，但**改名可以**（`mv X X.old` → `cp 新→X` → 删 `.old`）；要让已嵌入的那页换新版，得关掉那页再重新添加。

---

## 8. 更高一层的前提：必须自研

工作区"立规矩"（见 `plugins/NppDock/README.md` §0）：**所有要嵌进 dock 的软件一律自己写，源码与功能目录放工作区内，不接受第三方 exe。** 上面 R2~R6 都要求**改应用自己的代码**（DPI、弹性布局、重绘时机、优雅退出），第三方 exe 一条都做不到。想嵌终端类程序（cmd/powershell）正解是 **ConPTY** + 自绘 UI，不是窗口嵌入。

---

## 9. 常见问题

**菜单里根本不出现我的应用？** 文件名不是 `NppDockApp_*.exe`，或没放 `NppDock.dll` 同目录；用 `dock_app_probe.py log` 看"发现可嵌入应用 N 个"。
**嵌进去右/下露白？** DPI 感知没对齐（R2），十有八九设晚了 —— 必须在建窗口之前。
**界面一个控件都没有、外框却好？** `WM_CREATE` 里没填全局 `g_hWnd`，控件成了"以 NULL 为父的孤儿"。
**嵌进去点不动、敲键盘没反应？** ① 完整性级别不一致（UIPI 静默失败）；② 焦点问题（跨进程 `SetFocus` 有限制，必要时 `AttachThreadInput`，慎用）。
**隐藏面板再唤出，应用区域变一块底色？** 重绘没兜住，看 R6。
**改了 exe 名字，下次打不开上次那页？** `panel.ini` 记文件名，找不到会跳过并自动修正（不崩，那页没了）。
**为什么我的应用要自己处理 `Enter` 键？** 普通窗口（非对话框）的单行 `EDIT` **没人接管回车**（不上报父窗口、不触发默认按钮、还"叮"一声）：子类化编辑框吃掉 `WM_CHAR` 的 `\r`/`\n`，或在 `WM_COMMAND/IDOK` 上兜一道（两道都做防版本差异）。
