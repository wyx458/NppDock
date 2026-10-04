// ============================================================================
// NppDockApi.h —— 主 DLL 与功能模块 DLL 之间的跨模块接口
// ----------------------------------------------------------------------------
// 设计约束：
//   1. 只用 C 链接 + POD 类型 + 纯虚接口，**不跨 DLL 传 C++ 标准库对象**。
//   2. 功能 DLL **不得链接主 DLL 的任何实现符号**：
//        - 主 DLL 的能力通过**函数指针表** NppDockHostApi 下发；
//        - 工具函数一律写成 inline 放在本头文件里。
//   3. 禁止跨 DLL delete（各 DLL 用 /MT，各有独立 CRT 堆）。
//      → 功能模块必须导出 nppdock_module_destroy 让主 DLL 调用。
//
// ABI 版本策略：接口一旦发布，**虚函数顺序不得更改**；新增能力只能
//   **追加到末尾**，并递增 NPPDOCK_ABI_VERSION。
// ============================================================================
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

#define NPPDOCK_ABI_VERSION 1

// 功能模块 DLL 必须导出的两个 C 符号名
#define NPPDOCK_ENTRY_SYMBOL   "nppdock_module_entry"
#define NPPDOCK_DESTROY_SYMBOL "nppdock_module_destroy"

// 功能模块 DLL 的命名前缀：主 DLL 只扫描本目录下 NppDock_*.dll
#define NPPDOCK_MODULE_PREFIX  L"NppDock_"
#define NPPDOCK_MODULE_SUFFIX  L".dll"

// ---------------------------------------------------------------------------
// 日志级别（给 hostApi->log 用）
// ---------------------------------------------------------------------------
enum NppDockLogLevel {
    NPPDOCK_LOG_INFO  = 0,
    NPPDOCK_LOG_WARN  = 1,
    NPPDOCK_LOG_ERROR = 2,
};

// ---------------------------------------------------------------------------
// 主 DLL 提供给功能模块的能力表
// ---------------------------------------------------------------------------
// 命名约定：
//   - 返回 uint32_t 的"取字符串"函数：成功返回**不含结尾 0 的字符数**，
//     缓冲区不足时返回所需字符数（不含结尾 0）且不写入内容；返回 0 表示失败。
//   - 返回 int 的：0 表示成功，非 0 表示失败（可当错误码）。
//   - 返回指针/HWND 的：失败返回 nullptr。
//
// 结构体字段只能**往末尾追加**，不得插入或重排。
struct NppDockHostApi {
    uint32_t abiVersion;                 // 必须校验：功能模块进来第一件事就是查它

    // ---- 宿主句柄类 ----
    HWND (*getNppHandle)(void);          // Notepad++ 主窗口
    HWND (*getContainerHandle)(void);    // 本插件的容器窗口

    // ---- 路径类 ----
    // 插件所在目录，含结尾反斜杠，例如 C:\...\plugins\NppDock\
    // ⚠️ 注意上面这行注释末尾的反斜杠！C/C++ 里行尾反斜杠是"续行符"，
    //    会把**下一行**也吞进注释 —— 于是 getPluginDir 的声明整行消失，
    //    报错却是"不是 NppDockHostApi 的成员"。所以注释绝不能以反斜杠结尾。
    uint32_t (*getPluginDir)(wchar_t* buf, uint32_t cap);
    // 插件配置根目录（宿主 NPPM_GETPLUGINSCONFIGDIR 得到），末位无反斜杠
    uint32_t (*getConfigRoot)(wchar_t* buf, uint32_t cap);
    // 主 DLL 自己管理的配置根：<configRoot>\NppDock，保证已存在
    uint32_t (*getOwnConfigRoot)(wchar_t* buf, uint32_t cap);
    // 日志文件完整路径
    uint32_t (*getLogFilePath)(wchar_t* buf, uint32_t cap);

    // ---- 文件/目录小工具 ----
    int (*ensureDirectory)(const wchar_t* dir);                       // 递归建目录
    int (*ensureFileWithDefault)(const wchar_t* path, const char* def, uint32_t len);
    int (*openWithDefaultApp)(const wchar_t* path);
    int (*readFileAll)(const wchar_t* path, char* buf, uint32_t cap, uint32_t* outLen);

    // ---- 服务 ----
    void (*logEx)(int level, const wchar_t* msg);      // 写日志（带级别）
    void (*log)(const wchar_t* msg);                   // 等价 logEx(INFO, msg)
    // 请求容器把自己那一页切到前台。self 是 NppDockModule*。
    void (*requestShow)(void* self);
    // 动态改标题。title 会被容器立即复制一份，调用后可释放。
    void (*setDirtyTitle)(void* self, const wchar_t* title);
    // 请求容器重新计算布局
    void (*requestRelayout)(void* self);

    // ---- 跨 DLL 析构（禁止跨 DLL delete）----
    // 主 DLL 卸载模块前必须调用它，而不是 delete。
    void (*destroyModule)(class NppDockModule* self);

    // ---- 面板显示/隐藏 ----
    BOOL (*showPanel)(void);
    BOOL (*hidePanel)(void);
    BOOL (*isPanelVisible)(void);
};

// ---------------------------------------------------------------------------
// 功能模块必须实现的接口
// ---------------------------------------------------------------------------
// 所有方法都是纯虚：功能模块**不链接**主 DLL 的实现；容器通过虚表调用它。
class NppDockModule {
public:
    virtual ~NppDockModule() {}

    // 标题，如 L"CMD"。返回值必须持久可读（字面量或模块静态缓冲）。
    virtual const wchar_t* getTitle() = 0;

    // 创建视图。parent = 容器给的内容区 HWND。
    // 约定：实现方应在 parent 之下再建一个**自己的子窗口**并铺满 parent 客户区，
    //       所有控件挂在那个子窗口上。
    // 返回自己那个子窗口的 HWND；失败返回 nullptr。
    virtual HWND createView(HWND parent) = 0;

    // 销毁视图并释放模块内部状态。必须保证可被反复 create/destroy。
    virtual void destroyView() = 0;

    // 页被切到前台 / 切到后台。容器保证在 createView 成功之后才会调用。
    virtual void onShow() = 0;
    virtual void onHide() = 0;

    // 建议高度（像素），返回 0 表示"随容器"。
    virtual int getPreferredHeight() = 0;

    // 视图是否已创建（两级懒加载的第二级状态位，由模块自己维护）
    virtual bool isViewCreated() = 0;
    virtual void setViewCreated(bool created) = 0;

    // 容器注入宿主能力表。**必须在 createView 之前调用**。
    virtual void setHostApi(const NppDockHostApi* api) = 0;
};

// ---------------------------------------------------------------------------
// 功能模块导出（在模块源码里实现，主 DLL 用 GetProcAddress 取）
// ---------------------------------------------------------------------------
typedef NppDockModule* (*PFN_nppdock_module_entry)(const NppDockHostApi* api);
typedef void           (*PFN_nppdock_module_destroy)(NppDockModule* self);

// ---------------------------------------------------------------------------
// inline 工具：ABI 校验 & 安全取字符串
// ---------------------------------------------------------------------------
namespace nppdock {

// 统一封装取字符串函数。语义：f(buf, cap) 成功返回"不含结尾 0 的字符数"，
// 缓冲区不足返回所需字符数。
inline bool GetString(uint32_t (*f)(wchar_t*, uint32_t), wchar_t* out, uint32_t cap)
{
    if (!f || !out || cap == 0) return false;
    out[0] = 0;
    uint32_t need = f(out, cap);
    if (need == 0 || need >= cap) { out[0] = 0; return false; }
    return true;
}

inline bool CheckAbi(const NppDockHostApi* api, wchar_t* errBuf, uint32_t errCap)
{
    if (!api) {
        if (errBuf && errCap) wcsncpy_s(errBuf, errCap, L"hostApi == nullptr", _TRUNCATE);
        return false;
    }
    if (api->abiVersion != NPPDOCK_ABI_VERSION) {
        if (errBuf && errCap) {
            wchar_t tmp[160];
            swprintf_s(tmp, L"ABI 不匹配：模块=%u 宿主=%u", api->abiVersion, NPPDOCK_ABI_VERSION);
            wcsncpy_s(errBuf, errCap, tmp, _TRUNCATE);
        }
        return false;
    }
    return true;
}

// 模块侧的安全日志：api 可能还没注入，所以先判空
inline void SafeLog(const NppDockHostApi* api, int level, const wchar_t* msg)
{
    if (api && api->logEx) api->logEx(level, msg);
}

} // namespace nppdock
