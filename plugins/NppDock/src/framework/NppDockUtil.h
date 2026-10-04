// ============================================================================
// NppDockUtil.h —— 纯 Win32 小工具（inline，放头文件里）
// ----------------------------------------------------------------------------
// 为什么放头文件（对应任务书 3.5 铁律 2）：
//   主 DLL 与功能 DLL 各自编译一份，不产生跨模块符号依赖，
//   避免"同一份静态变量在两个 DLL 里各存一份"的静默错乱。
//
// 另外遵循任务书 3.6 的经典坑：
//   - 取"本模块"的 HINSTANCE 必须用 GetModuleHandleExW(FROM_ADDRESS)，
//     绝不能用 GetModuleHandleW(NULL)（那返回 notepad++.exe 的句柄）。
// ============================================================================
#pragma once

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

namespace nppdock {

// ---------------------------------------------------------------------------
// 取本模块（当前 DLL）的 HINSTANCE
// ---------------------------------------------------------------------------
// 任务书 3.6 强调：创建自己注册的窗口类时要传**本模块**的 hInstance。
// 传 GetModuleHandleW(NULL) 会拿到 notepad++.exe 的句柄，导致找不到窗口类。
inline HMODULE GetOwnModuleHandle()
{
    HMODULE h = nullptr;
    // 用一个本模块内的函数地址反查所属模块
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetOwnModuleHandle),
        &h);
    return h;
}

// ---------------------------------------------------------------------------
// 路径操作
// ---------------------------------------------------------------------------
inline std::wstring PathJoin(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    r += b;
    return r;
}

// 确保目录以反斜杠结尾
inline std::wstring EnsureTrailingSlash(std::wstring p)
{
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    return p;
}

// 去掉末尾反斜杠
inline std::wstring StripTrailingSlash(std::wstring p)
{
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

// 取文件所在目录（含末尾反斜杠）
inline std::wstring DirNameOf(const std::wstring& file)
{
    size_t p = file.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    return file.substr(0, p + 1);
}

// 取文件名（不含路径）
inline std::wstring BaseNameOf(const std::wstring& file)
{
    size_t p = file.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? file : file.substr(p + 1);
}

// 取扩展名（含点，小写）
inline std::wstring ExtOf(const std::wstring& file)
{
    size_t p = file.find_last_of(L'.');
    if (p == std::wstring::npos) return L"";
    std::wstring e = file.substr(p);
    ::CharLowerBuffW(&e[0], (DWORD)e.size());
    return e;
}

// 去掉扩展名
inline std::wstring StemOf(const std::wstring& file)
{
    std::wstring b = BaseNameOf(file);
    size_t p = b.find_last_of(L'.');
    return (p == std::wstring::npos) ? b : b.substr(0, p);
}

// 取本 DLL 的完整路径
inline std::wstring GetOwnModulePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(GetOwnModuleHandle(), buf, _countof(buf));
    return buf;
}

// ---------------------------------------------------------------------------
// 文件系统
// ---------------------------------------------------------------------------
inline bool FileExists(const std::wstring& path)
{
    DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool DirExists(const std::wstring& path)
{
    DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 递归建目录（等价 mkdir -p）
inline bool EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty()) return false;
    if (DirExists(dir)) return true;

    // 逐级创建，避免 SHCreateDirectoryEx 在某些情况下失败
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        wchar_t c = dir[i];
        cur += c;
        // 遇到分隔符（且不是盘符后的第一个）就尝试创建
        if (c == L'\\' || c == L'/') {
            if (cur.size() > 1 && cur[cur.size() - 2] != L':') {
                ::CreateDirectoryW(cur.c_str(), nullptr);
            }
        }
    }
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        return ::GetLastError() == ERROR_ALREADY_EXISTS;
    }
    return true;
}

// 读整个文件（字节）
inline bool ReadFileAll(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (1 << 28)) {
        ::CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = TRUE;
    if (!out.empty()) {
        ok = ::ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    }
    ::CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

// 写整个文件（字节）
inline bool WriteFileAll(const std::wstring& path, const void* data, size_t len)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(h, data, (DWORD)len, &wrote, nullptr);
    ::CloseHandle(h);
    return ok && wrote == len;
}

// 原子写：先写 <path>.tmp，成功了再整体替换过去。
// 为什么不直接 CREATE_ALWAYS 截断原文件写 —— 那样"写到一半断电/被杀"
// 会留下半截文件。panel.ini 丢了就等于重置所有状态，代价太大。
inline bool WriteFileAtomic(const std::wstring& path, const void* data, size_t len)
{
    const std::wstring tmp = path + L".tmp";
    if (!WriteFileAll(tmp, data, len)) { ::DeleteFileW(tmp.c_str()); return false; }
    if (::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    ::DeleteFileW(tmp.c_str());
    return false;
}

// 若文件不存在则写入默认内容（任务书 4.5：首次访问写一份带注释的默认配置）
inline bool EnsureFileWithDefault(const std::wstring& path,
                                  const std::string& def)
{
    if (FileExists(path)) return true;
    EnsureDirectory(DirNameOf(path));
    return WriteFileAll(path, def.data(), def.size());
}

// 字符串字面量重载：`EnsureFileWithDefault(path, "...")` 直接可用。
// 没有这个重载的话，编译器会尝试用 initializer_list<char> 构造 std::string
// 而报一串难懂的模板错误（本项目实测踩到，见 README 踩坑记录“坑 8”）。
inline bool EnsureFileWithDefault(const std::wstring& path,
                                  const char* def)
{
    return EnsureFileWithDefault(path, std::string(def ? def : ""));
}

// 用系统默认程序打开（任务书 4.5：配置文件即设置界面）
inline bool OpenWithDefaultApp(const std::wstring& path)
{
    HINSTANCE r = ::ShellExecuteW(nullptr, L"open", path.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

// ---------------------------------------------------------------------------
// 版本资源
// ---------------------------------------------------------------------------
// 读 exe/dll 的 VERSIONINFO 字符串（如 FileDescription）。
// 用途：NppDock 应用的 tab 标题直接取 FileDescription —— 元数据跟着二进制走，
// 复制/改名都不会脱节，比额外放一个清单文件可靠。
//
// 走的是标准两跳：
//   VarFileInfo\Translation 拿到 (语言, 代码页) -> 拼出 StringFileInfo 的子块路径
// 不写死 080404b0，因为应用可以只提供英文资源。
inline std::wstring ReadVersionString(const std::wstring& file, const wchar_t* key)
{
    if (file.empty() || !key) return L"";

    DWORD handle = 0;
    DWORD size = ::GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size == 0) return L"";

    std::vector<BYTE> buf(size);
    if (!::GetFileVersionInfoW(file.c_str(), 0, size, buf.data())) return L"";

    struct LangCp { WORD lang; WORD cp; };
    LangCp* tr = nullptr;
    UINT trLen = 0;
    if (!::VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<LPVOID*>(&tr), &trLen) ||
        trLen < sizeof(LangCp)) {
        return L"";
    }

    wchar_t sub[192];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", tr[0].lang, tr[0].cp, key);

    wchar_t* val = nullptr;
    UINT valLen = 0;
    if (!::VerQueryValueW(buf.data(), sub, reinterpret_cast<LPVOID*>(&val), &valLen) || !val) {
        return L"";
    }
    return std::wstring(val);
}

// ---------------------------------------------------------------------------
// 日志（任务书 2.8：Notepad++ 加载插件失败是完全静默的，日志是唯一排查手段）
// ---------------------------------------------------------------------------
// 滚日志：超过 1MB 就改名成 .old 重新开始，避免无限增长。
// （定义在 AppendLog 之前 —— 它要用到）
inline void RotateLogIfNeeded(const std::wstring& logPath, unsigned long long maxBytes = 1024 * 1024)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) return;
    unsigned long long sz = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (sz < maxBytes) return;
    std::wstring oldPath = logPath + L".old";
    ::DeleteFileW(oldPath.c_str());
    ::MoveFileW(logPath.c_str(), oldPath.c_str());
}

inline void AppendLog(const std::wstring& logPath, int level, const wchar_t* msg)
{
    if (logPath.empty() || !msg) return;
    EnsureDirectory(DirNameOf(logPath));

    // ⚠️ 只在 LogInit 里滚一次是不够的：插件与 Notepad++ 同寿命，长会话
    //    （探针灌日志那种）能一路涨下去。这里每 64 条查一次文件大小。
    static int s_sinceRotateCheck = 0;
    if (++s_sinceRotateCheck >= 64) {
        s_sinceRotateCheck = 0;
        RotateLogIfNeeded(logPath);
    }

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    static const wchar_t* kLevels[] = { L"INFO ", L"WARN ", L"ERROR" };

    wchar_t line[4096];
    _snwprintf_s(line, _TRUNCATE,
                 L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %s\r\n",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 kLevels[(level >= 0 && level <= 2) ? level : 0],
                 msg);

    HANDLE h = ::CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // 转成 UTF-8 写入，避免记事本打开乱码
    int need = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::string u8(need - 1, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, line, -1, &u8[0], need, nullptr, nullptr);
        DWORD wrote = 0;
        ::WriteFile(h, u8.data(), (DWORD)u8.size(), &wrote, nullptr);
    }
    ::CloseHandle(h);
}

// ---------------------------------------------------------------------------
// 字符串
// ---------------------------------------------------------------------------
inline std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return L"";
    std::wstring w((size_t)need, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], need);
    return w;
}

inline std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return "";
    std::string s((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          &s[0], need, nullptr, nullptr);
    return s;
}

inline std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 安全的 wcsncpy 封装（保证以 0 结尾）
inline void CopyToBuffer(const std::wstring& src, wchar_t* buf, uint32_t cap)
{
    if (!buf || cap == 0) return;
    uint32_t n = (uint32_t)src.size();
    if (n >= cap) n = cap - 1;
    if (n) memcpy(buf, src.c_str(), n * sizeof(wchar_t));
    buf[n] = L'\0';
}

// 全量转大写（用于扩展名/开关值比较）
inline std::wstring ToUpper(std::wstring s)
{
    if (!s.empty()) ::CharUpperBuffW(&s[0], (DWORD)s.size());
    return s;
}

} // namespace nppdock
