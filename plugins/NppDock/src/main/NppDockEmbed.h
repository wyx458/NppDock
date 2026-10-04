// ============================================================================
// NppDockEmbed.h —— 「可嵌入应用」的数据模型与契约常量
// ----------------------------------------------------------------------------
// 契约（单向、极简）：
//   · 应用是**独立 exe**，文件名 NppDockApp_<名字>.exe，与 NppDock.dll 同目录
//   · 它的 VERSIONINFO/FileDescription 就是 dock 里那个 tab 的标题
//   · dock 拉起它时传 --dock-parent <HWND>，由**应用自己**完成子窗口化
//
// 为什么"由应用自己子窗口化"而不是 dock 去改它的样式：
//   应用知道自己的 UI 该怎么排（要不要标题栏、边距多少、按钮放哪），
//   dock 一旦去替它做这些决定，就得去猜，而且每换一个应用都要重新猜一遍。
// ============================================================================
#pragma once

#include <windows.h>

#include <string>

namespace nppdock {

// 发现到的可嵌入应用
struct AppEntry {
    std::wstring exeName;   // 文件名，如 NppDockApp_MD5.exe
    std::wstring exePath;   // 全路径
    std::wstring title;     // tab 标题（FileDescription，回退为文件名去前缀）
};

// 一个已经打开的应用页
//
// ⚠️ title 与 exeName 是**两个不同的东西**，别合并：
//   · exeName 是**身份** —— 落盘、查找、恢复全认它（对应 _apps[].exeName）；
//   · title   是**显示名** —— 同一个应用允许开多页，多页时后面几页会带上
//     "（2）""（3）"这样的序号，那时 title ≠ 应用的标题。
//   早期版本只有一个 title 且"一个应用只允许一页"，所以身份靠 appIndex 就够；
//   支持多开之后，用 title 当身份会立刻撞车（两页标题一模一样）。
struct AppPage {
    int          appIndex  = -1;
    std::wstring exeName;               // 身份：来自 _apps[appIndex].exeName
    std::wstring title;                 // 显示标题（同类多开时带序号）

    HWND         hHost     = nullptr;   // 本页宿主窗口，作为嵌入窗口的父
    HWND         hEmbedded = nullptr;   // 被嵌入的窗口（在应用自己的进程里）
    HANDLE       hProcess  = nullptr;   // 应用进程句柄
    DWORD        pid       = 0;
};

// ---- 契约常量 ----
constexpr wchar_t kAppPrefix[]     = L"NppDockApp_";
constexpr wchar_t kAppSuffix[]     = L".exe";
constexpr wchar_t kDockParentArg[] = L"--dock-parent";

} // namespace nppdock
