// ============================================================================
// test_integration.cpp —— 离线集成测试（不需要真实的 Notepad++）
// ----------------------------------------------------------------------------
// 为什么要做这个：Notepad++ 插件加载失败是**完全静默**的（没有对话框、
// 没有事件日志），只在真机上反复重启试错，效率极低。所以我们自己造一个"假宿主"：
//
//   1. 注册一个窗口类冒充 Notepad++ 主窗口；它实现 NPPM_DMMREGASDCKDLG /
//      NPPM_DMMSHOW / NPPM_DMMHIDE / NPPM_MODELESSDIALOG /
//      NPPM_GETPLUGINSCONFIGDIR 等本插件用到的消息。
//   2. LoadLibrary 真实产出的 NppDock.dll，按宿主流程调用
//      setInfo → getFuncsArray → beNotified(NPPN_READY)。
//   3. 断言（对应当前架构：无标签空白面板 + 模块发现 + 状态持久化）：
//      - 主 DLL 导出了全部 6 个 C 函数
//      - NPPN_READY 后容器窗口存在，且容器内**没有** SysTabControl32
//      - uMask 必须是 DWS_DF_CONT_BOTTOM（防"停到左边"复发）
//      - 模块发现：目录里的 NppDock_*.dll 被加载并可取标题
//      - 容器内没有模块 DLL 时，面板保持空白外壳（不崩）
//      - 状态持久化：面板状态文件被写出，且内容与当前显隐一致
//      - 显示/隐藏切换反复有效（状态机回归护栏）
//      - beNotified(NPPN_SHUTDOWN) 后窗口与模块干净回收，DLL 可卸载
//      - 幂等：连续两次 NPPN_READY 不产生两套窗口
//
// 这个测试**不依赖**任何外部 test framework，输出用纯 printf。
// ============================================================================

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "NppDockScintilla.h"
#include "PluginInterface.h"
#include "Docking.h"
#include "Notepad_plus_msgs.h"
#include "NppDockApi.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ---------------------------------------------------------------------------
// 轻量断言
// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
static const char* g_section = "";

#define SECTION(name) do { g_section = name; std::printf("\n[%s]\n", name); } while (0)

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) { ++g_pass; std::printf("  OK   %s\n", #cond); }         \
        else { ++g_fail; std::printf("  FAIL %s   (%s:%d)\n", #cond,       \
                                     __FILE__, __LINE__); }                \
    } while (0)

#define CHECK_MSG(cond, msg)                                              \
    do {                                                                  \
        if (cond) { ++g_pass; std::printf("  OK   %s\n", msg); }           \
        else { ++g_fail; std::printf("  FAIL %s   (%s:%d)\n", msg,         \
                                     __FILE__, __LINE__); }                \
    } while (0)

static HWND FindFirstClass(HWND root, const wchar_t* cls);
static bool HasClass(HWND root, const wchar_t* cls);

// 宽字符串转 UTF-8（避免 %ls 在 C locale 下输出乱码）
static std::string W2U8(const wchar_t* w)
{
    if (!w || !*w) return "(空)";
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return "?";
    std::string s((size_t)need - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], need, nullptr, nullptr);
    return s;
}
static std::string W2U8(const std::wstring& w) { return W2U8(w.c_str()); }

// ---------------------------------------------------------------------------
// 假宿主窗口
// ---------------------------------------------------------------------------
static HWND  g_fakeNpp   = nullptr;
static HWND  g_registeredPanel = nullptr;   // NPPM_DMMREGASDCKDLG 收到的 hClient
static UINT  g_registeredUMask = 0;         // NPPM_DMMREGASDCKDLG 收到的 uMask
static int   g_registerCount   = 0;         // 注册次数（验证幂等/重注册）
static std::wstring g_fakeConfigDir;
static int   g_dmmShowCount = 0, g_dmmHideCount = 0;
static int   g_modelessAddCount = 0, g_modelessRemoveCount = 0;

LRESULT CALLBACK FakeNppProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == NPPM_DMMREGASDCKDLG) {
        auto* tb = reinterpret_cast<DockedWidgetData*>(l);
        if (tb) {
            g_registeredPanel = tb->hClient;
            g_registeredUMask = tb->uMask;
            ++g_registerCount;
            std::printf("      [fakeNpp] 收到 NPPM_DMMREGASDCKDLG: hClient=%p "
                        "uMask=0x%08X name=%s module=%s\n",
                        (void*)tb->hClient, (unsigned)tb->uMask,
                        W2U8(tb->pszName).c_str(), W2U8(tb->pszModuleName).c_str());

            // ⚠️ 真实 Notepad++ 在注册后会**立即把面板显示出来**
            //   （实测：即使 config.xml 写着 isVisible="no" 也照显不误）。
            //   假宿主必须模拟这一点，否则插件的"可见性对账"逻辑
            //   会因为面板实际不可见而永远走 SHOW 分支，测不出真实行为。
            ::ShowWindow(tb->hClient, SW_SHOW);
        }
        return TRUE;
    }
    // 假宿主用 ShowWindow 真正模拟"停靠栏显示/隐藏"。
    // ⚠️ 必须真做，不能只计数：插件的 TogglePanel 会调 isPanelVisible()
    //    与缓存状态对账，若假宿主不真的改变可见性，插件会永远认为面板是隐藏的。
    if (m == NPPM_DMMSHOW) {
        ++g_dmmShowCount;
        if (l) ::ShowWindow((HWND)l, SW_SHOW);
        return TRUE;
    }
    if (m == NPPM_DMMHIDE) {
        ++g_dmmHideCount;
        if (l) ::ShowWindow((HWND)l, SW_HIDE);
        return TRUE;
    }

    if (m == NPPM_MODELESSDIALOG) {
        if (w == MODELESSDIALOGADD)    ++g_modelessAddCount;
        if (w == MODELESSDIALOGREMOVE) ++g_modelessRemoveCount;
        return TRUE;
    }
    if (m == NPPM_GETPLUGINSCONFIGDIR) {
        // wParam = 缓冲字符数，lParam = 缓冲
        auto* buf = reinterpret_cast<wchar_t*>(l);
        if (buf && w >= g_fakeConfigDir.size() + 1) {
            wcsncpy_s(buf, (size_t)w, g_fakeConfigDir.c_str(), _TRUNCATE);
        }
        return (LRESULT)(g_fakeConfigDir.size() + 1);
    }
    return ::DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// 窗口树工具
// ---------------------------------------------------------------------------
struct FindCtx { const wchar_t* cls; HWND found; };

// 诊断：打印整棵子窗口树的类名
static void DumpTree(HWND root, int depth = 0)
{
    if (!root) return;
    for (HWND c = ::GetWindow(root, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
        wchar_t name[256] = {};
        ::GetClassNameW(c, name, _countof(name));
        std::printf("%*s- %s (%p)\n", depth * 2 + 6, "", W2U8(name).c_str(), (void*)c);
        DumpTree(c, depth + 1);
    }
}

static BOOL CALLBACK EnumChildProc(HWND child, LPARAM lp)
{
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    if (ctx->found) return FALSE;

    wchar_t name[256] = {};
    ::GetClassNameW(child, name, _countof(name));
    if (::_wcsicmp(name, ctx->cls) == 0) {
        ctx->found = child;
        return FALSE;
    }
    ::EnumChildWindows(child, EnumChildProc, lp);
    return ctx->found ? FALSE : TRUE;
}

static HWND FindFirstClass(HWND root, const wchar_t* cls)
{
    if (!root) return nullptr;
    FindCtx ctx{ cls, nullptr };
    ::EnumChildWindows(root, EnumChildProc, (LPARAM)&ctx);
    return ctx.found;
}

static bool HasClass(HWND root, const wchar_t* cls)
{
    return FindFirstClass(root, cls) != nullptr;
}

// ---------------------------------------------------------------------------
// 插件 ABI 函数指针
// ---------------------------------------------------------------------------
typedef void        (*PFN_setInfo)(NppData);
typedef const wchar_t* (*PFN_getName)(void);
typedef FuncItem*   (*PFN_getFuncsArray)(int*);
typedef void        (*PFN_beNotified)(SCNotification*);
typedef LRESULT     (*PFN_messageProc)(UINT, WPARAM, LPARAM);
typedef BOOL        (*PFN_isUnicode)(void);

// ---------------------------------------------------------------------------
// 读取面板状态文件
// ---------------------------------------------------------------------------
static bool ReadStateFile(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256] = {};
    DWORD rd = 0;
    BOOL ok = ::ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    out.assign(buf, rd);
    return true;
}

// 读**整个**文件。
// ⚠️ 不能拿上面那个 256 字节的 ReadStateFile 去读日志：它是给小配置文件的，
//    而日志开头一大段是 setInfo 的路径横幅，256 字节还没读到正题就截断了 ——
//    表现就是"日志里明明有，测试却断言找不到"。这个坑真实踩过。
static bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char buf[4096];
    DWORD rd = 0;
    while (::ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) {
        out.append(buf, rd);
        if (rd < sizeof(buf)) break;
    }
    ::CloseHandle(h);
    return !out.empty();
}

// ---------------------------------------------------------------------------
// 覆盖写入面板状态文件（用于往返测试）
// ---------------------------------------------------------------------------
static bool WriteStateFile(const std::wstring& path, const std::string& data)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = ::WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    ::CloseHandle(h);
    return ok && wr == (DWORD)data.size();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    std::printf("================================================================\n");
    std::printf("  NppDock 离线集成测试（假宿主 + 真实 DLL）\n");
    std::printf("================================================================\n");

    // 主 DLL 路径：默认 build/NppDock/NppDock.dll，可用 argv[1] 覆盖
    std::wstring dllPath;
    if (argc > 1) {
        int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        dllPath.resize((size_t)n - 1);
        ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &dllPath[0], n);
    } else {
        dllPath = L"..\\NppDock\\NppDock.dll";
    }

    std::printf("[0. 准备环境]\n");
    std::printf("    主 DLL = %s\n", W2U8(dllPath).c_str());

    // 假配置目录
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    g_fakeConfigDir = std::wstring(tmp) + L"NppDockTest_config";
    ::CreateDirectoryW(g_fakeConfigDir.c_str(), nullptr);
    std::printf("    配置目录 = %s\n", W2U8(g_fakeConfigDir).c_str());

    // 清掉上一次运行留下的日志。
    // 日志是**追加**的，而下面第 3b 节要靠日志内容判定"应用发现成功" ——
    // 不清掉的话，上一次的记录会让断言无条件通过，等于测了个寂寞。
    ::DeleteFileW((g_fakeConfigDir + L"\\NppDock\\NppDock.log").c_str());

    // 假宿主窗口类
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &FakeNppProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FakeNppMainWnd";
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::printf("  FAIL 注册假宿主窗口类失败\n");
        return 1;
    }
    g_fakeNpp = ::CreateWindowExW(0, L"FakeNppMainWnd", L"FakeNpp",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  0, 0, 800, 600, nullptr, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    CHECK_MSG(g_fakeNpp != nullptr, "假宿主主窗口创建成功");

    // ⚠️ 主窗口必须真的可见：插件的 isPanelVisible() 会沿父链向上检查，
    //    父窗口不可见时子窗口一律报"不可见"。真实 Notepad++ 主窗口当然可见。
    CHECK_MSG(g_fakeNpp && ::IsWindowVisible(g_fakeNpp),
              "假宿主主窗口可见（保证父链可见性判定正确）");

    // -----------------------------------------------------------------------
    SECTION("1. 加载 NppDock.dll 并检查 6 个导出函数");
    HMODULE hDll = ::LoadLibraryExW(dllPath.c_str(), nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hDll) {
        std::printf("  FAIL 加载 %s 失败，错误码 %lu\n",
                    W2U8(dllPath).c_str(), ::GetLastError());
        return 1;
    }
    std::printf("      DLL 句柄 = %p\n", (void*)hDll);

    auto pSetInfo     = (PFN_setInfo)::GetProcAddress(hDll, "setInfo");
    auto pGetName     = (PFN_getName)::GetProcAddress(hDll, "getName");
    auto pGetFuncs    = (PFN_getFuncsArray)::GetProcAddress(hDll, "getFuncsArray");
    auto pBeNotified  = (PFN_beNotified)::GetProcAddress(hDll, "beNotified");
    auto pMessageProc = (PFN_messageProc)::GetProcAddress(hDll, "messageProc");
    auto pIsUnicode   = (PFN_isUnicode)::GetProcAddress(hDll, "isUnicode");

    CHECK(pSetInfo != nullptr);
    CHECK(pGetName != nullptr);
    CHECK(pGetFuncs != nullptr);
    CHECK(pBeNotified != nullptr);
    CHECK(pMessageProc != nullptr);
    CHECK(pIsUnicode != nullptr);

    if (!pSetInfo || !pGetFuncs || !pBeNotified) {
        std::printf("  FAIL 关键导出缺失，中止\n");
        ::FreeLibrary(hDll);
        return 1;
    }

    // -----------------------------------------------------------------------
    SECTION("2. setInfo + 菜单表");
    NppData nd{};
    nd._nppHandle = g_fakeNpp;
    nd._scintillaMainHandle = nullptr;
    nd._scintillaSecondHandle = nullptr;
    pSetInfo(nd);

    std::printf("      getName() = \"%s\"\n", W2U8(pGetName()).c_str());
    CHECK_MSG(pGetName() && pGetName()[0] != 0, "getName() 返回非空");

    int nbF = 0;
    FuncItem* items = pGetFuncs(&nbF);
    std::printf("      菜单项数 = %d\n", nbF);
    CHECK_MSG(items != nullptr && nbF >= 1, "菜单表非空");
    for (int i = 0; i < nbF; ++i) {
        std::printf("        [%d] %s\n", i, W2U8(items[i]._itemName).c_str());
    }

    // -----------------------------------------------------------------------
    SECTION("3. NPPN_READY：注册面板（无标签空白容器）");
    SCNotification sc{};
    sc.nmhdr.code = NPPN_READY;
    pBeNotified(&sc);

    CHECK_MSG(g_registeredPanel != nullptr, "宿主收到了面板注册请求");
    CHECK_MSG(g_modelessAddCount == 1, "容器已声明为无模式对话框");

    // uMask 必须是底部槽位（防"停到左边"复发）
    CHECK_MSG(g_registeredUMask == (UINT)DWS_DF_CONT_BOTTOM,
              "uMask == DWS_DF_CONT_BOTTOM（底部槽位）");
    std::printf("      uMask = 0x%08X（期望 0x%08X）\n",
                g_registeredUMask, (unsigned)DWS_DF_CONT_BOTTOM);

    // 容器是自研窗口类
    CHECK_MSG(HasClass(g_registeredPanel, L"NppDockContentPane") ||
              HasClass(g_registeredPanel, L"NppDockContainerWnd"),
              "容器内存在自研内容区窗口类");

    // ★ 核心断言：标签条**存在且可见**（哪怕一个应用页都没有）。
    //
    //   设计变更说明（第三版）：
    //     v1 要求"面板里完全没有标签条"；v2 改成"标签条存在但没页时隐藏"。
    //     v3（现在）：**常驻可见**。原因是"添加新应用"的唯一入口变成了
    //     "右击标签条的空白处" —— 藏起来用户就没地方加东西了。
    {
        std::printf("      [诊断] 容器窗口树（root=%p）:\n", (void*)g_registeredPanel);
        DumpTree(g_registeredPanel);
        HWND tab = FindFirstClass(g_registeredPanel, L"SysTabControl32");
        CHECK_MSG(tab != nullptr, "容器内已建标签条（应用页与添加入口都靠它）");
        if (tab) {
            const bool vis = (::IsWindowVisible(tab) != FALSE);
            std::printf("      [诊断] SysTabControl32 hwnd=%p visible=%d\n",
                        (void*)tab, (int)vis);
            CHECK_MSG(vis, "无应用页时标签条也**可见**（常驻，右击它才能加应用）");
            // 常驻标签条必须有真实高度，否则字会被裁掉
            RECT tr{};
            ::GetWindowRect(tab, &tr);
            std::printf("      [诊断] 标签条尺寸 = %ldx%ld\n",
                        (long)(tr.right - tr.left), (long)(tr.bottom - tr.top));
        }
    }

    // -----------------------------------------------------------------------
    SECTION("3b. 应用发现：同目录下的 NppDockApp_*.exe");
    {
        // 本测试加载的 DLL 就在 build/NppDock/ 下，而那里同时放着真实的
        // NppDockApp_MD5.exe —— 所以这一段跑的是**完整真实链路**：
        //   扫文件名 -> 读 VERSIONINFO 的 FileDescription -> 得到 tab 标题
        // 用日志作为观察窗（容器不导出内部状态，日志是唯一稳定的可观测面）。
        const std::wstring logPath = g_fakeConfigDir + L"\\NppDock\\NppDock.log";
        std::string logText;
        if (ReadWholeFile(logPath, logText)) {
            const bool discovered = logText.find("发现可嵌入应用") != std::string::npos;
            const bool foundExe   = logText.find("NppDockApp_MD5.exe") != std::string::npos;
            const bool foundTitle = logText.find("文件校验") != std::string::npos;

            std::printf("      [诊断] 发现记录=%d  文件名=%d  版本资源标题=%d\n",
                        (int)discovered, (int)foundExe, (int)foundTitle);

            CHECK_MSG(discovered, "日志记录了应用发现动作");
            CHECK_MSG(foundExe,   "发现同目录下的 NppDockApp_MD5.exe");
            CHECK_MSG(foundTitle, "从 exe 版本资源读到了中文标题「文件校验」");

            // ★ v2.0：内容区中央那句话也是走日志看的（它画在窗口上、不是控件）。
            //   一句话都没有的空白面板，新用户是猜不到"入口在标签条上"的。
            const bool hint = logText.find("内容区提示：") != std::string::npos;
            const bool hintText = logText.find("添加应用") != std::string::npos;
            std::printf("      [诊断] 内容区提示=%d  文案含「添加应用」=%d\n",
                        (int)hint, (int)hintText);
            CHECK_MSG(hint,     "日志记录了内容区提示（空面板的引导）");
            CHECK_MSG(hintText, "提示文案里写明了入口在标签条上");
        } else {
            CHECK_MSG(false, "能读到插件日志（应用发现无从验证）");
        }
    }

    // -----------------------------------------------------------------------
    SECTION("4. 面板状态持久化");
    std::wstring statePath = g_fakeConfigDir + L"\\NppDock\\panel.ini";
    std::string stateContent;
    bool hasState = ReadStateFile(statePath, stateContent);
    std::printf("      状态文件 = %s\n", W2U8(statePath).c_str());
    if (hasState) {
        std::printf("      内容 = %s\n", stateContent.c_str());
    }
    CHECK_MSG(hasState, "首次运行写出了面板状态文件");
    CHECK_MSG(stateContent.find("visible=") != std::string::npos,
              "状态文件包含 visible= 字段");

    // -----------------------------------------------------------------------
    // 4b. 状态**往返**校验：状态文件写什么，重启后就该恢复成什么。
    //
    // 旧 bug（真实踩过）：解析写成 line.find('1', 9)，而 "visible=1" 只有
    // 9 字节，下标 9 已越界 -> 永远返回 false -> 不管存的是什么都被判成
    // "隐藏"。表现就是"每次开机面板都是隐藏的，怎么切都记不住"。
    // 这里用**往返**（写文件 -> 重启注册）而不是只看文件内容，才能钉死这类 bug。
    //
    // 时序说明（两个坑都踩过）：
    //   1) 状态文件是"上一次退出时"落盘的，本次启动只读不写。所以必须在
    //      SHUTDOWN **之后**手工写状态文件，否则会被插件的退出落盘覆盖掉。
    //   2) NPPN_SHUTDOWN 处理器自身会发一次 NPPM_DMMHIDE（把面板从宿主
    //      布局摘下来，防止宿主访问已销毁窗口）。统计"状态恢复引发的
    //      HIDE"时必须以 SHUTDOWN 之后的计数为基准，否则会把这个
    //      摘除用的 HIDE 误算进来。
    {
        // 场景 A：上次关闭时面板是隐藏的 -> 本次启动应保持隐藏
        sc.nmhdr.code = NPPN_SHUTDOWN; pBeNotified(&sc);   // 上一进程退出（含 1 次摘除 HIDE，不计）
        WriteStateFile(statePath, "visible=0\r\n");        // 模拟"上次退出时已落盘为隐藏"
        std::string probe;
        ReadStateFile(statePath, probe);
        std::printf("      [4b-A] 状态文件 = %s\n", probe.c_str());

        const int hideBeforeA = g_dmmHideCount;
        sc.nmhdr.code = NPPN_READY; pBeNotified(&sc);      // 新进程启动，只读取

        const int hideDeltaA = g_dmmHideCount - hideBeforeA;
        std::printf("      [4b-A] 重启后 HIDE +%d\n", hideDeltaA);
        CHECK_MSG(hideDeltaA >= 1, "visible=0 时重启后面板保持隐藏");

        // 场景 B：上次关闭时面板是显示的 -> 本次启动不应隐藏
        sc.nmhdr.code = NPPN_SHUTDOWN; pBeNotified(&sc);   // 退出（摘除 HIDE 不计）
        WriteStateFile(statePath, "visible=1\r\n");        // 模拟"上次退出时已落盘为显示"
        std::printf("      [4b-B] 状态文件 = visible=1\n");

        const int hideBeforeB = g_dmmHideCount;
        sc.nmhdr.code = NPPN_READY; pBeNotified(&sc);

        const int hideDeltaB = g_dmmHideCount - hideBeforeB;
        std::printf("      [4b-B] 重启后 HIDE +%d\n", hideDeltaB);
        CHECK_MSG(hideDeltaB == 0,
                  "visible=1 时重启后面板保持显示（不误判为隐藏）");
    }

    // -----------------------------------------------------------------------
    SECTION("5. 显示/隐藏切换反复有效（回归护栏）");
    // 旧 bug：TogglePanel 开头无条件先 SHOW，导致第二次点击又走"隐藏"分支，
    //         表现为"关掉后打不开"。这里用 4 连切钉死状态机。
    if (nbF >= 1 && items) {
        const int showBefore = g_dmmShowCount;
        const int hideBefore = g_dmmHideCount;

        items[0]._pFunc();   // 切换 1：显示 -> 隐藏
        items[0]._pFunc();   // 切换 2：隐藏 -> 显示
        items[0]._pFunc();   // 切换 3：显示 -> 隐藏
        items[0]._pFunc();   // 切换 4：隐藏 -> 显示

        const int showDelta = g_dmmShowCount - showBefore;
        const int hideDelta = g_dmmHideCount - hideBefore;
        std::printf("      4 次切换：SHOW +%d, HIDE +%d\n", showDelta, hideDelta);

        CHECK_MSG(showDelta >= 2, "4 次切换中至少有 2 次 SHOW（可再次打开）");
        CHECK_MSG(hideDelta >= 2, "4 次切换中至少有 2 次 HIDE（可再次关闭）");
        CHECK_MSG(::IsWindow(g_registeredPanel), "4 次切换后面板窗口仍然有效");
    }

    // -----------------------------------------------------------------------
    SECTION("6. 幂等：重复 NPPN_READY 不产生第二套窗口");
    HWND panelBefore = g_registeredPanel;
    int  regBefore = g_registerCount;
    pBeNotified(&sc);
    CHECK_MSG(g_registeredPanel == panelBefore, "重复 NPPN_READY 后面板句柄未变");
    CHECK_MSG(g_registerCount == regBefore, "重复 NPPN_READY 未重复注册");
    CHECK_MSG(g_registeredUMask == (UINT)DWS_DF_CONT_BOTTOM,
              "重复 NPPN_READY 后 uMask 仍为底部槽位");

    // -----------------------------------------------------------------------
    SECTION("7. NPPN_SHUTDOWN：干净回收");
    SCNotification sd{};
    sd.nmhdr.code = NPPN_SHUTDOWN;
    pBeNotified(&sd);

    CHECK_MSG(!::IsWindow(panelBefore), "容器窗口已被销毁");
    CHECK_MSG(g_modelessRemoveCount >= 1, "退出时向宿主注销了无模式对话框");

    // -----------------------------------------------------------------------
    SECTION("8. 卸载 DLL（验证无残留引用）");
    if (::FreeLibrary(hDll)) {
        CHECK_MSG(true, "FreeLibrary(NppDock.dll) 成功");
    } else {
        CHECK_MSG(false, "FreeLibrary(NppDock.dll) 成功");
    }

    ::DestroyWindow(g_fakeNpp);

    std::printf("\n================================================================\n");
    std::printf("  通过: %d   失败: %d\n", g_pass, g_fail);
    std::printf("  结果: %s\n", g_fail == 0 ? "ALL PASS" : "FAILED");
    std::printf("================================================================\n");
    return g_fail == 0 ? 0 : 1;
}
