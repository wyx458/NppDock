// ============================================================================
// DummyModule.cpp —— 用于验证"模块发现 + 两级懒加载"的哑模块（NppDock_Dummy.dll）
// ----------------------------------------------------------------------------
// 这个模块**纯粹为了测试**，它回答一个关键问题：
//
//   "新增一个功能页，真的不需要改动主插件任何一行代码吗？"
//
// 做法：
//   - 标题是 "Dummy"
//   - createView 不启动任何子进程，只建一个 STATIC 窗口，并把创建时间戳
//     写进一个文件（<ownConfigRoot>\NppDock_Dummy\Dummy_probe.txt）
//   - 集成测试据此断言：
//       ① 模块被扫描到并加载（标题正确）
//       ② 视图创建时机符合"第二级懒加载"
//
// 结论：主插件的 discoverModules 是按文件名通配扫描的，
//       所以"往 plugins\NppDock\ 里丢一个新 DLL = 多一个功能页"。
// ============================================================================

#include <windows.h>

#include "NppDockApi.h"
#include "NppDockUtil.h"
#include "NppDockScintilla.h"
#include "PluginInterface.h"

using namespace nppdock;

namespace {

HWND g_view = nullptr;
bool g_viewCreated = false;
const NppDockHostApi* g_api = nullptr;

// 探针：记录 createView 被调用的次数与时间，写到文件里供测试断言
void WriteProbe(const wchar_t* what)
{
    if (!g_api || !g_api->getOwnConfigRoot) return;

    wchar_t root[MAX_PATH * 2] = {};
    uint32_t need = g_api->getOwnConfigRoot(root, _countof(root));
    if (need == 0 || need >= _countof(root)) return;

    const std::wstring dir = PathJoin(root, L"NppDock_Dummy");
    EnsureDirectory(dir);
    const std::wstring path = PathJoin(dir, L"Dummy_probe.txt");

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t line[256];
    _snwprintf_s(line, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d.%03d createView 被调用: %s\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, what);

    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string u8 = ToUtf8(line);
    DWORD wrote = 0;
    ::WriteFile(h, u8.data(), (DWORD)u8.size(), &wrote, nullptr);
    ::CloseHandle(h);
}

LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return ::DefWindowProcW(h, m, w, l);
}

class DummyModule final : public NppDockModule {
public:
    const wchar_t* getTitle() override { return L"Dummy"; }

    HWND createView(HWND parent) override
    {
        WriteProbe(L"real");

        static bool s_reg = false;
        if (!s_reg) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &DummyProc;
            wc.hInstance = GetOwnModuleHandle();
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = ::GetSysColorBrush(COLOR_WINDOW);
            wc.lpszClassName = L"NppDockDummyViewWnd";
            if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return nullptr;
            s_reg = true;
        }

        g_view = ::CreateWindowExW(0, L"NppDockDummyViewWnd", L"",
                                   WS_CHILD | WS_VISIBLE,
                                   0, 0, 100, 100, parent, nullptr,
                                   GetOwnModuleHandle(), nullptr);
        if (!g_view) return nullptr;

        // 子控件：一个说明文字，证明视图真的建起来了
        HWND st = ::CreateWindowExW(0, L"STATIC",
                                    L"这是 NppDock_Dummy.dll 提供的功能页。\r\n"
                                    L"它的存在证明：新增功能页无需改动主插件。",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    10, 10, 400, 60, g_view, nullptr,
                                    GetOwnModuleHandle(), nullptr);
        ::SendMessageW(st, WM_SETFONT, (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), FALSE);

        g_viewCreated = true;
        SafeLog(g_api, NPPDOCK_LOG_INFO, L"DummyModule：createView 完成（懒加载第二级被触发）");
        return g_view;
    }

    void destroyView() override
    {
        if (g_view && ::IsWindow(g_view)) ::DestroyWindow(g_view);
        g_view = nullptr;
        g_viewCreated = false;
        SafeLog(g_api, NPPDOCK_LOG_INFO, L"DummyModule：destroyView 完成");
    }

    void onShow() override { SafeLog(g_api, NPPDOCK_LOG_INFO, L"DummyModule：onShow"); }
    void onHide() override {}
    int  getPreferredHeight() override { return 200; }
    bool isViewCreated() override { return g_viewCreated; }
    void setViewCreated(bool c) override { g_viewCreated = c; }
    void setHostApi(const NppDockHostApi* api) override { g_api = api; }
};

} // namespace

extern "C" __declspec(dllexport)
NppDockModule* nppdock_module_entry(const NppDockHostApi* api)
{
    wchar_t err[192] = {};
    if (!nppdock::CheckAbi(api, err, _countof(err))) {
        SafeLog(api, NPPDOCK_LOG_ERROR, L"DummyModule：拒绝加载（ABI 不匹配）");
        return nullptr;
    }
    g_api = api;
    SafeLog(api, NPPDOCK_LOG_INFO, L"nppdock_module_entry：创建 DummyModule");
    return new DummyModule();
}

extern "C" __declspec(dllexport)
void nppdock_module_destroy(NppDockModule* self)
{
    if (!self) return;
    if (g_view && ::IsWindow(g_view)) ::DestroyWindow(g_view);
    g_view = nullptr;
    g_viewCreated = false;
    delete self;
}
