// ===========================================================================
// packcfg.cpp —— 见 packcfg.h
// ===========================================================================
#include "packcfg.h"
#include <windows.h>
#include <shlobj.h>          // SHGetKnownFolderPath（拿系统的「下载」夹）
#include "../../common/jsonlite.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace packcfg {

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH * 2]{};
    ::GetModuleFileNameW(nullptr, p, _countof(p));
    std::wstring s(p);
    const size_t k = s.find_last_of(L"\\/");
    return (k == std::wstring::npos) ? std::wstring() : s.substr(0, k);
}

// W()/U8() 定义在下面，但 DefaultJsonText() 要先用到 → 前置声明
static std::wstring W(const std::string& s);
static std::string  U8(const std::wstring& s);

static std::wstring Join(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// ★ v1.4：配置文件改名成 nppbackpack_config.json（王要求的）。
//   旧名字 config.json 太通用了 —— 这个目录下还放着别的应用的文件，
//   一个叫 config.json 的文件谁都不敢动。
std::wstring ConfigPath()
{
    return Join(ExeDir(), L"nppbackpack_config.json");
}

static std::wstring LegacyConfigPath()
{
    return Join(ExeDir(), L"config.json");
}

// 老名字还在、新名字还没有 → 直接改名搬过去（用户填过的东西不能丢）
static void MigrateLegacyConfig()
{
    const std::wstring nw = ConfigPath();
    if (::GetFileAttributesW(nw.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const std::wstring oldp = LegacyConfigPath();
    if (::GetFileAttributesW(oldp.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ::MoveFileExW(oldp.c_str(), nw.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring DefaultSshDir()
{
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", buf, _countof(buf));
    if (n == 0 || n >= _countof(buf)) {
        // 退回"当前用户的 home"：USERPROFILE 拿不到的情况极少，但别崩
        n = ::GetEnvironmentVariableW(L"HOMEDRIVE", buf, _countof(buf));
        if (n == 0) return std::wstring();
        std::wstring drive(buf, n);
        wchar_t rest[MAX_PATH]{};
        DWORD m = ::GetEnvironmentVariableW(L"HOMEPATH", rest, _countof(rest));
        if (m == 0) return std::wstring();
        return Join(drive, rest) + L"\\.ssh";
    }
    return Join(std::wstring(buf, n), L".ssh");
}

// 系统的「下载」已知文件夹。拿不到就退回 %USERPROFILE%\Downloads。
//
// 为什么值得走 KnownFolder 而不是直接拼 %USERPROFILE%\Downloads：
// 用户（或组策略）可以把"下载"重定向到别的盘 —— 那时拼出来的路径是**不存在**的，
// 表现成"下载按钮点了没反应/报写不进去"，很难往"默认值算错了"上想。
std::wstring DefaultDownloadsDir()
{
    // 「下载」这一项比较新（Vista 才加入 KnownFolder），用符号 FOLDERID_Downloads
    // 要依赖 knownfolders.h 的版本宏；直接把 GUID 写在这儿，省掉那层依赖。
    // {374DE290-123F-4565-9164-39C4925E467B}
    static const GUID kDownloads = {
        0x374DE290, 0x123F, 0x4565,
        { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B }
    };
    PWSTR p = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(kDownloads, 0, nullptr, &p)) && p) {
        std::wstring s(p);
        ::CoTaskMemFree(p);
        if (!s.empty()) return s;
    }
    wchar_t up[MAX_PATH * 2]{};
    if (::GetEnvironmentVariableW(L"USERPROFILE", up, _countof(up)) > 0)
        return Join(std::wstring(up), L"Downloads");
    return std::wstring();
}

// 双击/「打开」下载到本地的临时目录（每次启动背包时会被清空重建）
std::wstring DefaultOpenTmpDir()
{
    wchar_t la[MAX_PATH * 2]{};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", la, _countof(la)) > 0)
        return Join(Join(std::wstring(la), L"NppDockBackpack"), L"open");
    wchar_t tmp[MAX_PATH * 2]{};
    if (::GetEnvironmentVariableW(L"TEMP", tmp, _countof(tmp)) > 0)
        return Join(std::wstring(tmp), L"NppDockBackpack_open");
    return Join(ExeDir(), L"_open");
}

// ★ v1.7：展开 %USERPROFILE% 这类环境变量。
//   两次调用：先问长度，再取内容（ExpandEnvironmentStringsW 的常规用法）。
std::wstring ExpandEnv(const std::wstring& s)
{
    if (s.empty() || s.find(L'%') == std::wstring::npos) return s;
    const DWORD n = ::ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (n == 0) return s;
    std::wstring out((size_t)n, L'\0');
    const DWORD got = ::ExpandEnvironmentStringsW(s.c_str(), &out[0], n);
    if (got == 0 || got > n) return s;
    out.resize((size_t)(got > 0 ? got - 1 : 0));     // 返回值含结尾的 '\0'
    return out.empty() ? s : out;
}

// ★ v1.7：两个保留名。都带前导点 —— 在服务器上（Linux）它天然就是"隐藏文件"，
//   本地（Windows）虽然不是隐藏属性，但文件栏会**显式过滤**掉它们。
static const wchar_t* kNoteName  = L".nppbackpack-note.txt";
static const wchar_t* kTrashName = L".nppbackpack-trash";
static const wchar_t* kOldNote   = L"便笺.txt";

std::wstring NoteFileName()       { return kNoteName; }
std::wstring TrashDirName()       { return kTrashName; }
std::wstring LegacyNoteFileName() { return kOldNote; }

bool IsReservedName(const std::wstring& name)
{
    return _wcsicmp(name.c_str(), kNoteName)  == 0 ||
           _wcsicmp(name.c_str(), kTrashName) == 0 ||
           _wcsicmp(name.c_str(), kOldNote)   == 0;   // 旧便笺名也不显示（迁完就没了）
}

// 默认配置的文本（带中文注释；jsonlite 的读取器容忍 // 注释 —— 这是选它的理由之一）
//
// 为什么从"静态字符串"改成"运行时拼"：private_key_path 的默认值要按
// **当前用户**的 .ssh 目录算（王的要求），静态串里写不出来。
static std::string DefaultJsonText()
{
    const std::string keyDir = U8(DefaultSshDir());
    std::string s;
    s += "{\n";
    s += "  // 改完这个文件保存后，回到应用点右上角的刷新（或重开本标签页）即可生效，\n";
    s += "  // 不需要先关标签页；应用也不会反过来覆盖你在这里改的内容。\n";
    s += "  // 服务器地址。留空或写成 local:<本地目录> 就**不连服务器**，\n";
    s += "  // 直接用本地目录当背包（没网也能记东西）。\n";
    s += "  \"host\": \"your-server.com\",\n";
    s += "  \"port\": 22,\n";
    s += "\n";
    s += "  // SSH 用户名。**同一个用户名 = 同一个人 = 同一个背包**：\n";
    s += "  // 从一台机器换到另一台，只要用户名一样，读到的就是同一个背包。\n";
    s += "  \"username\": \"your_ssh_username\",\n";
    s += "\n";
    s += "  // \"key\" = 用私钥（推荐，免密码交互）；\"password\" = 用下面的密码\n";
    s += "  \"auth_type\": \"password\",\n";
    s += "  \"password\": \"\",\n";
    s += "  // 默认给的是当前用户的 .ssh 目录；用私钥时请把它改成**密钥文件**的完整路径\n";
    s += "  \"private_key_path\": " + jsonlite::Quote(keyDir) + ",\n";
    s += "\n";
    s += "  // 服务器上的背包目录。留空会自动算成 /home/<用户名>/npp-backpack，\n";
    s += "  // 不存在时会自动创建（含父目录）。\n";
    s += "  \"remote_folder\": \"/home/your_ssh_username/npp-backpack\",\n";
    s += "\n";
    s += "  // 显示在顶部栏的名字\n";
    s += "  \"backpack_name\": \"我的背包\",\n";
    s += "\n";
    s += "  // ---- 便笺区外观（界面上**没有**调节入口；改完重开标签页生效）----\n";
    s += "  \"note_font\": \"Consolas\",     // 等宽字体名，找不到会自动退回\n";
    s += "  \"note_font_size\": 8,          // 字号（点）；0 = 跟界面字号\n";
    s += "  \"note_background\": \"#FFFFFF\", // 便笺背景色（#RRGGBB）\n";
    s += "  \"note_foreground\": \"#202020\", // 便笺文字色（#RRGGBB）\n";
    s += "\n";
    s += "  // ---- 本地目录与「确认连接」的节奏 ----\n";
    s += "  // ★ 路径里可以写 %USERPROFILE% / %LOCALAPPDATA% 这类环境变量，\n";
    s += "  //   这样配置文件换台机器照样能用（不会钉死在某一个人的用户名上）。\n";
    s += "  // 「下载」按钮存到哪儿\n";
    s += "  \"download_dir\": " + jsonlite::Quote("%USERPROFILE%\\\\Downloads") + ",\n";
    s += "  // 下载下来用默认程序打开时用的临时目录（临时区，每次启动背包会清空）\n";
    s += "  \"open_tmp_dir\": " + jsonlite::Quote("%LOCALAPPDATA%\\\\NppDockBackpack\\\\open") + ",\n";
    s += "  // 每几秒自动确认一次连接状态（默认 300 = 5 分钟）；0 = 不心跳\n";
    s += "  \"heartbeat_sec\": 300,\n";
    s += "  // 确认连接的最小间隔（毫秒）：比这个更密的请求会被合并，避免把 IO 打满\n";
    s += "  \"io_min_interval_ms\": 1000\n";
    s += "}\n";
    return s;
}

static bool WriteTextFileUtf8(const std::wstring& path, const std::string& bytes)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = (bytes.empty() ||
               (::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr)
                && wrote == bytes.size()));
    ::CloseHandle(h);
    return ok;
}

static bool ReadTextFileUtf8(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!out.empty()) ::ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    out.resize(got);
    ::CloseHandle(h);
    return true;
}

static std::wstring W(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

static std::string U8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n,
                          nullptr, nullptr);
    return out;
}

void EnsureDefaults(Config& c)
{
    if (c.authType.empty()) c.authType = L"password";
    if (c.port <= 0 || c.port > 65535) c.port = 22;

    // ★ v1.5：remote_folder 还停在模板占位符上（用户只改了 host/用户名/密码，
    //   没注意到这一行）→ 当成"没填"，下面按用户名重新推算。
    //   不处理的话会去 mkdir "/home/your_ssh_username/npp-backpack"：
    //   普通用户对 /home/your_ssh_username 没有写权限，远端报一句
    //   "mkdir: ... Permission denied"，看起来跟"密码不对"一模一样，
    //   极容易把人带到错误的方向上去排查（王就踩了）。
    if (c.remoteFolder.find(L"your_ssh_username") != std::wstring::npos)
        c.remoteFolder.clear();

    if (c.backpackName.empty()) {
        // "同一用户名 = 同一个人"：名字默认带上用户名，一眼看得出这是谁的背包
        c.backpackName = c.username.empty() ? L"我的背包"
                                            : (c.username + L" 的背包");
    }
    if (c.remoteFolder.empty() && !c.username.empty() && !IsLocal(c))
        c.remoteFolder = L"/home/" + c.username + L"/npp-backpack";

    // 私钥路径默认给当前用户的 .ssh 目录（王的要求）。
    // 只在**空**的时候填 —— 用户填过的绝不覆盖。
    if (c.privateKeyPath.empty()) c.privateKeyPath = DefaultSshDir();

    if (c.noteFont.empty()) c.noteFont = L"Consolas";
    if (c.noteFontPt < 0 || c.noteFontPt > 72) c.noteFontPt = 8;
    if (c.noteBg.empty()) c.noteBg = L"#FFFFFF";
    if (c.noteFg.empty()) c.noteFg = L"#202020";

    // v1.6：本地目录 + 确认节奏
    // ★ v1.7：先展开 %环境变量%（用户可能就写了 %USERPROFILE%\Downloads）
    if (!c.downloadDir.empty()) c.downloadDir = ExpandEnv(c.downloadDir);
    if (!c.openTmpDir.empty())  c.openTmpDir  = ExpandEnv(c.openTmpDir);
    if (c.downloadDir.empty())  c.downloadDir = DefaultDownloadsDir();
    if (c.openTmpDir.empty())   c.openTmpDir  = DefaultOpenTmpDir();
    // 心跳默认 5 分钟（王 v1.7：5 秒太频繁了）
    if (c.heartbeatSec < 0 || c.heartbeatSec > 86400) c.heartbeatSec = 300;
    if (c.ioMinIntervalMs < 0 || c.ioMinIntervalMs > 60000) c.ioMinIntervalMs = 1000;
}

bool Load(Config& out, bool* createdDefault)
{
    out = Config();
    if (createdDefault) *createdDefault = false;

    MigrateLegacyConfig();          // 老名字 config.json 还在就搬过来
    const std::wstring path = ConfigPath();
    std::string text;
    if (!ReadTextFileUtf8(path, text)) {
        WriteTextFileUtf8(path, DefaultJsonText());
        if (createdDefault) *createdDefault = true;
        EnsureDefaults(out);
        return false;                       // 首次运行：用默认值
    }

    jsonlite::Value root;
    std::string err;
    if (!jsonlite::Parse(text, root, err) || !root.IsObj()) {
        // ⚠️ **绝不静默丢用户的文件**：改名成 .bad 留证据，再用默认值继续跑
        const std::wstring bad = path + L".bad";
        ::MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        WriteTextFileUtf8(path, DefaultJsonText());
        if (createdDefault) *createdDefault = true;
        EnsureDefaults(out);
        return false;
    }

    auto str = [&](const char* k) { return W(root.Str(k)); };
    out.host           = str("host");
    out.username       = str("username");
    out.authType       = str("auth_type");
    out.password       = str("password");
    out.privateKeyPath = str("private_key_path");
    out.remoteFolder   = str("remote_folder");
    out.backpackName   = str("backpack_name");
    out.port = root.Int("port", 22);

    // 便笺外观（缺项就用默认值 —— 老配置文件没有这几项，照样能用）
    if (!out.noteFont.empty()) {}                       // 占位，保持结构清楚
    {
        const std::wstring f = str("note_font");
        if (!f.empty()) out.noteFont = f;
        const std::wstring bg = str("note_background");
        if (!bg.empty()) out.noteBg = bg;
        const std::wstring fg = str("note_foreground");
        if (!fg.empty()) out.noteFg = fg;
        // 缺项 → 默认 9；显式写 0 → 0（意思是"跟界面字号"）。
        // jsonlite 没有 Has()，而 Int(key, def) 正好就是这个语义。
        out.noteFontPt = root.Int("note_font_size", 8);
    }

    // v1.6：本地目录 + 确认节奏（缺项 → 默认值，老配置文件照样能用）
    {
        const std::wstring dd = str("download_dir");
        if (!dd.empty()) out.downloadDir = dd;
        const std::wstring od = str("open_tmp_dir");
        if (!od.empty()) out.openTmpDir = od;
        out.heartbeatSec    = root.Int("heartbeat_sec", 300);
        out.ioMinIntervalMs = root.Int("io_min_interval_ms", 1000);
    }

    EnsureDefaults(out);
    return true;
}

bool Save(const Config& c0)
{
    Config c = c0;
    EnsureDefaults(c);

    std::string s;
    s += "{\n";
    s += "  // 见「文件背包」的说明：改完这里保存，回到应用点「刷新」或重开标签页即可生效。\n";
    s += "  // host 写成 local:<本地目录> 就**不连服务器**，用本地目录当背包。\n";
    // jsonlite::Quote() 自己带两端引号 —— 外面不要再加
    s += "  \"host\": "            + jsonlite::Quote(U8(c.host))            + ",\n";
    s += "  \"port\": "              + std::to_string(c.port)             + ",\n";
    s += "  \"username\": "        + jsonlite::Quote(U8(c.username))        + ",\n";
    s += "  \"auth_type\": "       + jsonlite::Quote(U8(c.authType))        + ",\n";
    s += "  \"password\": "        + jsonlite::Quote(U8(c.password))        + ",\n";
    s += "  \"private_key_path\": "+ jsonlite::Quote(U8(c.privateKeyPath))  + ",\n";
    s += "  \"remote_folder\": "   + jsonlite::Quote(U8(c.remoteFolder))    + ",\n";
    s += "  \"backpack_name\": "   + jsonlite::Quote(U8(c.backpackName))    + ",\n";
    s += "\n";
    s += "  // ---- 便笺区外观 ----（界面上**没有**调节入口；改完重开标签页生效）\n";
    s += "  \"note_font\": "        + jsonlite::Quote(U8(c.noteFont))         + ",\n";
    s += "  \"note_font_size\": "   + std::to_string(c.noteFontPt)           + ",\n";
    s += "  \"note_background\": "  + jsonlite::Quote(U8(c.noteBg))           + ",\n";
    s += "  \"note_foreground\": "  + jsonlite::Quote(U8(c.noteFg))           + ",\n";
    s += "\n";
    s += "  // ---- 本地目录与「确认连接」的节奏 ----\n";
    s += "  // 「下载」按钮存到哪儿（默认：系统的「下载」文件夹）\n";
    s += "  \"download_dir\": "      + jsonlite::Quote(U8(c.downloadDir))      + ",\n";
    s += "  // 双击/「打开」下载到哪儿再用默认程序打开（临时目录，每次启动背包会清空）\n";
    s += "  \"open_tmp_dir\": "      + jsonlite::Quote(U8(c.openTmpDir))       + ",\n";
    s += "  // 每几秒自动确认一次连接状态；0 = 不心跳\n";
    s += "  \"heartbeat_sec\": "      + std::to_string(c.heartbeatSec)         + ",\n";
    s += "  // 确认连接的最小间隔（毫秒）：比这个更密的请求会被合并，避免把 IO 打满\n";
    s += "  \"io_min_interval_ms\": " + std::to_string(c.ioMinIntervalMs)      + "\n";
    s += "}\n";
    return WriteTextFileUtf8(ConfigPath(), s);
}

// 见 packcfg.h：应用不再回写配置文件，所以这里没有"回写外观"的入口了。
// Save() 只在两处被用到：① 首次运行生成默认配置；② 文件写坏时留 .bad 再生成一份。

bool IsLocal(const Config& c)
{
    if (c.host.empty()) return true;                     // 没填服务器 = 本地背包
    if (_wcsnicmp(c.host.c_str(), L"local:", 6) == 0) return true;

    // 写成真实路径（C:\... 或 \\server\share）且目录确实存在，也当本地
    const DWORD attr = ::GetFileAttributesW(c.host.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (c.host.find(L'\\') != std::wstring::npos ||
            c.host.find(L':')   != std::wstring::npos) return true;
    }
    return false;
}

std::wstring LocalRoot(const Config& c)
{
    if (_wcsnicmp(c.host.c_str(), L"local:", 6) == 0) return c.host.substr(6);
    if (!c.host.empty()) return c.host;
    // 什么都没填：落在 exe 同目录下的 backpack/（一个能立刻用起来的地方）
    return Join(ExeDir(), L"backpack");
}

bool LooksUnconfigured(const Config& c)
{
    if (c.host.empty()) return true;
    if (c.host.find(L"your-server.com") != std::wstring::npos) return true;      // 模板原样
    if (c.username.empty()) return true;
    if (c.username.find(L"your_ssh_username") != std::wstring::npos) return true;
    if (IsLocal(c)) return false;                       // 本地目录模式：直接算配好了
    if (_wcsicmp(c.authType.c_str(), L"key") == 0) return c.privateKeyPath.empty();
    return false;                                       // 密码认证：允许空密码（有些机器是空口令）
}

static int HexVal(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

bool ParseColor(const std::wstring& text, unsigned long& outRgb)
{
    std::wstring t;
    for (wchar_t ch : text) if (ch != L' ' && ch != L'\t') t += ch;
    if (!t.empty() && t[0] == L'#') t.erase(t.begin());
    if (t.size() != 6) return false;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = HexVal(t[i]);
        if (v[i] < 0) return false;
    }
    const unsigned long r = (unsigned long)(v[0] * 16 + v[1]);
    const unsigned long g = (unsigned long)(v[2] * 16 + v[3]);
    const unsigned long b = (unsigned long)(v[4] * 16 + v[5]);
    outRgb = (r << 16) | (g << 8) | b;      // 与 RGB() 宏的字节序一致
    return true;
}

std::wstring FormatColor(unsigned long rgb)
{
    wchar_t b[16];
    swprintf_s(b, L"#%02X%02X%02X",
               (unsigned)((rgb >> 16) & 0xFF),
               (unsigned)((rgb >> 8) & 0xFF),
               (unsigned)(rgb & 0xFF));
    return b;
}

std::wstring EffectiveRemoteFolder(const Config& c)
{
    if (!c.remoteFolder.empty()) return c.remoteFolder;
    if (!c.username.empty()) return L"/home/" + c.username + L"/npp-backpack";
    return L"~/npp-backpack";
}

} // namespace packcfg
