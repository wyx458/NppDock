// ===========================================================================
// packcore.cpp —— 见 packcore.h
// ===========================================================================
#include "packcore.h"
#include <windows.h>
#include <cstdio>
#include <algorithm>

namespace packcore {

// v1.9：等外部进程时抽的消息泵钩子（见 packcore.h 的说明）。默认不抽。
static void (*g_pump)() = nullptr;
void SetPump(void (*fn)()) { g_pump = fn; }

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// W2U8 定义在下面，但 RJoin/Shq 要先用到 → 前置声明
static std::string W2U8(const std::wstring& w);

// 远端路径一律用 '/' 拼
//
// ⚠️⚠️ 拼之前必须走 W2U8（UTF-8），**不能** `std::string s(w.begin(), w.end())`：
//   那是把每个 wchar_t 硬截成 char（中文只剩低字节），
//   于是"便笺.txt"发到服务器上会变成 0xBF 0x3A ... 这种乱码文件名。
//   （实测踩到：服务器上出现一个名叫 "\xBF:.txt" 的文件。）
static std::string RJoin(const std::wstring& folder, const std::wstring& name)
{
    std::string f = W2U8(folder);
    std::string n = W2U8(name);
    if (f.empty()) return n;
    if (f.back() != '/') f += '/';
    return f + n;
}

// 相对路径 -> 本地原生分隔符（Windows 也能吃 '/'，但 '' 更保险）
static std::wstring ToNativeRel(const std::wstring& rel)
{
    std::wstring o = rel;
    for (wchar_t& ch : o) if (ch == L'/') ch = L'\\';
    return o;
}

// 背包根 + 相对路径（本地）
static std::wstring LocalPathOf(const std::wstring& root, const std::wstring& rel)
{
    // 根目录本身也可能是 "C:/xxx" 这种写法（配置里手写的），统一成正斜杠
    // 混着来在"文件属性"里显示成 "C:/x\y" 很难看。
    std::wstring r = root;
    for (wchar_t& ch : r) if (ch == L'/') ch = L'\\';
    if (rel.empty()) return r;
    return JoinPath(r, ToNativeRel(rel));
}

// 背包根 + 相对路径（远端，POSIX）
static std::wstring RemotePathOf(const std::wstring& folder, const std::wstring& rel)
{
    if (rel.empty()) return folder;
    std::wstring f = folder;
    if (!f.empty() && f.back() == L'/') f.pop_back();
    return f + L"/" + rel;
}

// FILETIME -> Unix 秒
static long long FileTimeToUnix(const FILETIME& ft)
{
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart == 0) return 0;
    // 1601-01-01 → 1970-01-01 之间是 11644473600 秒（单位 100ns）
    const unsigned long long kEpoch = 116444736000000000ULL;
    if (u.QuadPart < kEpoch) return 0;
    return (long long)((u.QuadPart - kEpoch) / 10000000ULL);
}

// POSIX 单引号转义：' -> '\''（远端 shell 里唯一安全的做法）
//
// ⚠️ 同样必须 UTF-8 化（原因见 RJoin）：远端 shell 收的就是字节，
//    非 ASCII 字符按 UTF-8 发出去才对得上服务器上的真实文件名。
static std::string Shq(const std::wstring& w)
{
    std::string s = W2U8(w);
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else           o += c;
    }
    o += "'";
    return o;
}

static std::string W2U8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                  nullptr, nullptr);
    std::string o((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &o[0], n, nullptr, nullptr);
    return o;
}

static std::wstring U82W(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring o((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &o[0], n);
    return o;
}

std::wstring FirstLines(const std::string& utf8, int maxLines)
{
    std::wstring w = U82W(utf8);
    std::wstring out;
    int lines = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\r') continue;
        if (w[i] == L'\n') {
            if (++lines >= maxLines) { out += L"…"; break; }
            out += L" | ";
            continue;
        }
        out += w[i];
    }
    // 去掉两端空白，最多留 300 字，免得把状态栏撑爆
    while (!out.empty() && (out.front() == L' ' || out.front() == L'|')) out.erase(out.begin());
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.size() > 300) out = out.substr(0, 297) + L"…";
    return out;
}

// ---------------------------------------------------------------------------
// 进程执行：把 stdout / stderr 重定向到临时文件，stdin 可选来自文件
//
// 为什么用"重定向到文件"而不是管道：
//   管道要走两个线程边读边等，否则子进程写满 64KB 就会卡住（经典死锁）。
//   我们要的输出量不大（ls 的结果、报错），落文件最简单也最稳。
// ---------------------------------------------------------------------------
struct ProcOut {
    bool        started = false;
    DWORD       exitCode = (DWORD)-1;
    std::string out, err;
    std::wstring why;           // 启动失败的原因
};

static std::wstring TempFile(const wchar_t* tag)
{
    wchar_t dir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, dir);
    static int seq = 0;
    wchar_t name[128];
    swprintf_s(name, L"nppdpack_%u_%d_%s.tmp", ::GetCurrentProcessId(), ++seq, tag);
    return JoinPath(dir, name);
}

static bool ReadWholeFile(const std::wstring& path, std::string& out)
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

static bool WriteWholeFile(const std::wstring& path, const std::string& bytes)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = bytes.empty() ||
              (::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) &&
               wrote == bytes.size());
    ::CloseHandle(h);
    return ok;
}

// 造一份环境块：复制当前环境 + 追加我们需要的（SSH_ASKPASS 那套）
//
// ⚠️ 同名变量必须**先剔旧再追加**：Windows 环境块里同名变量"先出现的赢"，
//    如果我们只是往后面 append，而用户的环境里本来就有 SSH_ASKPASS /
//    NPPDOCK_ASKPASS_PW（以前跑过、或手工设过），那旧值会压过我们的值，
//    结果就是"密码明明填了，却像没填一样一直认证失败"。
static std::vector<wchar_t> BuildEnv(const std::vector<std::pair<std::wstring, std::wstring>>& extra)
{
    std::vector<wchar_t> blk;
    if (LPWCH cur = ::GetEnvironmentStringsW()) {
        for (LPWCH p = cur; *p; p += wcslen(p) + 1) {
            const size_t n = wcslen(p);
            const std::wstring line(p, n);
            const size_t eq = line.find(L'=');
            const std::wstring name = (eq == std::wstring::npos) ? line
                                                                 : line.substr(0, eq);
            bool shadowed = false;
            for (const auto& kv : extra) {
                if (_wcsicmp(kv.first.c_str(), name.c_str()) == 0) {
                    shadowed = true;
                    break;
                }
            }
            if (shadowed) continue;                 // 让位给我们自己的值
            blk.insert(blk.end(), p, p + n);
            blk.push_back(L'\0');
        }
        ::FreeEnvironmentStringsW(cur);
    }
    for (const auto& kv : extra) {
        const std::wstring line = kv.first + L"=" + kv.second;
        blk.insert(blk.end(), line.begin(), line.end());
        blk.push_back(L'\0');
    }
    blk.push_back(L'\0');
    return blk;
}

// argv 风格的命令行拼装（每个参数按需要加引号）；exe 放第一个
static std::wstring BuildCmdLine(const std::wstring& exe, const std::vector<std::wstring>& args)
{
    auto quote = [](const std::wstring& s) {
        if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
        std::wstring o = L"\"";
        for (wchar_t c : s) {
            if (c == L'"') o += L'\\';
            o += c;
        }
        o += L"\"";
        return o;
    };
    std::wstring cl = quote(exe);
    for (const auto& a : args) cl += L" " + quote(a);
    return cl;
}

static ProcOut RunCapture(const std::wstring& exe,
                          const std::vector<std::wstring>& args,
                          const std::wstring& stdinFile,      // 空 = NUL
                          const std::vector<std::pair<std::wstring, std::wstring>>& envExtra,
                          DWORD timeoutMs)
{
    ProcOut r;
    const std::wstring outPath = TempFile(L"out");
    const std::wstring errPath = TempFile(L"err");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = ::CreateFileW(errPath.c_str(), GENERIC_WRITE, 0, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hIn  = ::CreateFileW(stdinFile.empty() ? L"NUL" : stdinFile.c_str(),
                                GENERIC_READ, FILE_SHARE_READ, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE ||
        hIn == INVALID_HANDLE_VALUE) {
        r.why = L"创建临时文件失败";
        if (hOut != INVALID_HANDLE_VALUE) ::CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) ::CloseHandle(hErr);
        if (hIn  != INVALID_HANDLE_VALUE) ::CloseHandle(hIn);
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hIn;
    si.hStdOutput = hOut;
    si.hStdError = hErr;

    std::wstring cl = BuildCmdLine(exe, args);
    std::vector<wchar_t> cmd(cl.begin(), cl.end());
    cmd.push_back(L'\0');
    std::vector<wchar_t> env = BuildEnv(envExtra);

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags,
                          env.data(), nullptr, &si, &pi)) {
        wchar_t b[128];
        swprintf_s(b, L"启动 %s 失败（错误码 %lu）",
                   exe.substr(exe.find_last_of(L'\\') + 1).c_str(), ::GetLastError());
        r.why = b;
    } else {
        r.started = true;
        ::CloseHandle(pi.hThread);
        // v1.9：等待期间**抽消息泵**（每 80ms 一次）。
        //   以前是 `WaitForSingleObject(hProcess, timeoutMs)` 一把等下去 ——
        //   那期间界面完全不响应，连"正在连接…"都画不出来。
        //   超时改用 GetTickCount 自己算（分片等待，累加不可靠）。
        const DWORD t0 = ::GetTickCount();
        for (;;) {
            if (::WaitForSingleObject(pi.hProcess, 80) == WAIT_OBJECT_0) break;
            if (g_pump) g_pump();
            if ((DWORD)(::GetTickCount() - t0) >= timeoutMs) {
                ::TerminateProcess(pi.hProcess, 1);
                ::WaitForSingleObject(pi.hProcess, 3000);
                r.why = L"超时（服务器没响应）";
                break;
            }
        }
        ::GetExitCodeProcess(pi.hProcess, &r.exitCode);
        ::CloseHandle(pi.hProcess);
    }

    ::CloseHandle(hIn);
    ::CloseHandle(hOut);
    ::CloseHandle(hErr);
    ReadWholeFile(outPath, r.out);
    ReadWholeFile(errPath, r.err);
    ::DeleteFileW(outPath.c_str());
    ::DeleteFileW(errPath.c_str());
    return r;
}

// ---------------------------------------------------------------------------
// ssh.exe 定位
// ---------------------------------------------------------------------------
std::wstring SshExe()
{
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t sys[MAX_PATH]{};
    ::GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring cand = JoinPath(sys, L"OpenSSH\\ssh.exe");
    if (::GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
        cached = cand;
        return cached;
    }
    wchar_t found[MAX_PATH * 2]{};
    if (::SearchPathW(nullptr, L"ssh.exe", nullptr, _countof(found), found, nullptr))
        cached = found;
    return cached;
}

// ---------------------------------------------------------------------------
// ssh 调用的公共参数
// ---------------------------------------------------------------------------
struct SshPlan {
    std::vector<std::wstring> args;                              // 给 ssh.exe 的
    std::vector<std::pair<std::wstring, std::wstring>> env;      // 额外环境
};

static SshPlan MakePlan(const Target& t, const std::wstring& remoteCmd, bool needStdin)
{
    SshPlan p;
    auto& a = p.args;

    // accept-new：第一次连的机器自动记住（免掉"yes/no"交互），
    // 但**变了的主机密钥会被拒绝** —— 既不用弹窗，也不放弃中间人检测。
    a.push_back(L"-o"); a.push_back(L"StrictHostKeyChecking=accept-new");
    a.push_back(L"-o"); a.push_back(L"ConnectTimeout=6");
    a.push_back(L"-p"); a.push_back(std::to_wstring(t.port > 0 ? t.port : 22));
    a.push_back(L"-o"); a.push_back(L"LogLevel=ERROR");   // 只留真错误，别把横幅灌进来

    const bool keyAuth = (_wcsicmp(t.authType.c_str(), L"key") == 0);
    if (keyAuth) {
        if (!t.keyPath.empty()) { a.push_back(L"-i"); a.push_back(t.keyPath); }
        // 私钥认证不该出现任何交互：连不上就干净地失败，别卡在那儿等输入
        a.push_back(L"-o"); a.push_back(L"BatchMode=yes");
    } else {
        // 密码认证：**自问自答**（见文件头说明）
        //
        // ⚠️⚠️ 为什么还要设 NPPDOCK_ASKPASS_MODE：
        //   ssh 拉起 askpass 程序时**只把提示串当 argv[1]**，不会加任何开关
        //   （`ssh -vvv` 里那一行就是证据：
        //    `spawning "…\NppDockApp_PACK.exe" "ubuntu@host's password: " as subprocess`）。
        //   所以"我是不是被叫来问密码的"必须靠**环境变量**判断 —— 早先只认 argv 里的
        //   `--askpass`，结果 ssh 拉起来的那个实例跑去建主窗口，
        //   表现就是"点一次刷新弹出一串窗口"，而且**密码永远发不出去**、
        //   服务器只能回一句 Permission denied（会被误判成密码错）。
        wchar_t self[MAX_PATH * 2]{};
        ::GetModuleFileNameW(nullptr, self, _countof(self));
        p.env.push_back({ L"SSH_ASKPASS", self });
        p.env.push_back({ L"SSH_ASKPASS_REQUIRE", L"force" });
        p.env.push_back({ L"NPPDOCK_ASKPASS_MODE", L"1" });     // ← 见上
        p.env.push_back({ L"NPPDOCK_ASKPASS_PW", t.password });
        a.push_back(L"-o"); a.push_back(L"NumberOfPasswordPrompts=1");
        a.push_back(L"-o"); a.push_back(L"PreferredAuthentications=password");
    }
    (void)needStdin;

    a.push_back(t.user + L"@" + t.host);
    a.push_back(remoteCmd);
    return p;
}

// 把 ssh 的失败翻译成"人能懂的一句话"
static Res Classify(const ProcOut& pr, const wchar_t* what)
{
    Res r;
    if (!pr.started) { r.st = St::Error; r.msg = pr.why; return r; }

    const std::string all = pr.out + "\n" + pr.err;
    auto has = [&](const char* s) { return all.find(s) != std::string::npos; };

    // ⚠️ 这里的判定要**精确**：ssh 自己的认证失败长这样
    //      <user>@<host>: Permission denied (publickey,password).
    //   注意那个左括号 —— 远端命令失败时的 "mkdir: ...: Permission denied"
    //   **没有**括号。早先这里只匹配 "Permission denied"，于是"远端目录没权限"
    //   被误报成"用户名或密码不对"，把排查方向带偏（王踩到过）。
    if (has("Permission denied (") ||
        has("Permission denied, please try again") ||
        has("Authentication failed") ||
        has("no supported authentication")) {
        r.st = St::AuthFailed;
        r.msg = L"认证失败：用户名或密码/私钥不对（也可能私钥没在服务器上授权）";
        return r;
    }
    // 到这里还有 "Permission denied" → 是**登录成功之后**远端命令被拒，
    // 最典型的就是背包目录落在了当前用户没权限写的地方。
    if (has("Permission denied")) {
        r.st = St::Error;
        r.msg = L"已登录，但远端目录没有写权限（remote_folder 换成本用户可写的目录，"
                L"例如 /home/<用户名>/npp-backpack）";
        return r;
    }
    if (has("Connection refused")) {
        r.st = St::Unreachable;
        r.msg = L"连接被拒绝：地址/端口不对，或服务器的 SSH 没开";
        return r;
    }
    if (has("Connection timed out") || has("timed out") || has("No route to host") ||
        has("Could not resolve hostname")) {
        r.st = St::Unreachable;
        r.msg = L"连不上服务器（超时 / 路由不通 / 域名解析不了）";
        return r;
    }
    if (has("Host key verification failed")) {
        r.st = St::Unreachable;
        r.msg = L"主机密钥与之前记录的不一致 —— 服务器换过？先确认安全再清掉 known_hosts 里那一条";
        return r;
    }
    if (has("No such file")) { r.st = St::NotFound; r.msg = L"远端没有这个文件"; return r; }

    r.st = St::Error;
    std::wstring tail = FirstLines(pr.err.empty() ? pr.out : pr.err);
    r.msg = std::wstring(what) + L"失败";
    if (!tail.empty()) r.msg += L"：" + tail;
    else {
        wchar_t b[64];
        swprintf_s(b, L"（退出码 %lu）", pr.exitCode);
        r.msg += b;
    }
    return r;
}

// ---------------------------------------------------------------------------
// 本地后端
// ---------------------------------------------------------------------------
static Res LocalEnsureDir(const std::wstring& dir)
{
    const DWORD a = ::GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
        return (a & FILE_ATTRIBUTE_DIRECTORY)
                   ? Res{ St::Ok, L"" }
                   : Res{ St::Error, L"这个路径被一个**文件**占着，不是目录" };
    }
    if (::CreateDirectoryW(dir.c_str(), nullptr)) return Res{ St::Ok, L"" };

    // 父目录可能也不存在 —— 一层层往上建
    const size_t k = dir.find_last_of(L"\\/");
    if (k != std::wstring::npos && k > 0) {
        Res up = LocalEnsureDir(dir.substr(0, k));
        if (!Ok(up)) return up;
        if (::CreateDirectoryW(dir.c_str(), nullptr)) return Res{ St::Ok, L"" };
    }
    wchar_t b[192];
    swprintf_s(b, L"建目录失败（错误码 %lu）", ::GetLastError());
    return Res{ St::Error, b };
}

static Res LocalList(const std::wstring& dir, std::vector<FileItem>& out)
{
    out.clear();
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return { St::Error, L"读不到目录（可能不存在或没权限）" };
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        FileItem it;
        it.name  = fd.cFileName;
        it.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.size  = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        it.mtime = FileTimeToUnix(fd.ftLastWriteTime);
        out.push_back(it);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(out.begin(), out.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDir != b.isDir) return a.isDir;          // 目录在前
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return { St::Ok, L"" };
}

// ---------------------------------------------------------------------------
// 远端 ls -l 的解析
//
// 目标格式（GNU ls，--time-style=+%s）：
//   -rw-r--r-- 1 user group 1234 1696118400 便笺.txt
//   字段：0 权限 1 链接数 2 属主 3 属组 4 大小 5 时间戳 6... 名字（名字里可以有空格）
//
// ⚠️ busybox 之类没有 --time-style，会直接报错 —— 那时回退成 ls -1p（只有名字）。
//    宁可少显示一个大小的列，也不要"文件栏一片空白"。
// ---------------------------------------------------------------------------
static void ParseLs(const std::string& text, std::vector<FileItem>& out)
{
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        // ⚠️ v2.0：只去掉行尾的 \r。以前这里还顺手 trim 掉尾部的空格 ——
        //    那个动作会把"名字以空格结尾"的文件名改掉（去掉一个字符就是另一个文件了）。
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.compare(0, 5, "total") == 0) continue;

        const char kind = line[0];
        const bool isDir   = (kind == 'd');
        const bool isLink  = (kind == 'l');
        // ⚠️ v2.0：p/s/b/c（管道 / 套接字 / 块设备 / 字符设备）以前被整行丢弃，
        //    于是这些条目在列表里**凭空消失**。现在列出来并标成"其它"。
        const bool isOther = (kind == 'p' || kind == 's' || kind == 'b' || kind == 'c');
        if (kind != '-' && !isDir && !isLink && !isOther) continue;

        // 前 6 个字段按**位置**跳过：权限 链接数 属主 属组 大小 时间戳。
        // ⚠️ 块设备/字符设备（b/c）的"大小"那一列是 `主号, 次号` 两个字段，
        //    所以它们比普通文件**多一个** —— 不分开数的话，名字会被读成时间戳
        //    （这条是那个解析测试壳抓出来的，见 _t/test_parsels.cpp）。
        const bool isDev = (kind == 'b' || kind == 'c');
        const int  nFields = isDev ? 7 : 6;

        // 剩下的整段**原样**就是文件名 —— 不再"按空格切开再拼回去"，
        // 那样会把名字里的连续空格塌成一个（0x20 0x20 -> 0x20）。
        std::vector<std::string> f;
        size_t i = 0;
        for (int k = 0; k < nFields; ++k) {
            while (i < line.size() && line[i] == ' ') ++i;
            size_t j = i;
            while (j < line.size() && line[j] != ' ') ++j;
            if (j == i) break;
            f.push_back(line.substr(i, j - i));
            i = j;
        }
        if ((int)f.size() < nFields) continue;
        while (i < line.size() && line[i] == ' ') ++i;   // 时间戳与名字之间那一个空格
        if (i >= line.size()) continue;

        std::string rawName = line.substr(i);

        FileItem it;
        it.isDir   = isDir;
        it.isLink  = isLink;
        it.isOther = isOther;
        it.size    = isDev ? 0 : _strtoui64(f[4].c_str(), nullptr, 10);
        it.mtime   = _strtoi64(f[nFields - 1].c_str(), nullptr, 10);  // +%s 给的就是秒

        if (isLink) {
            // ls -l 对软链的写法是 `名字 -> 目标`。用 **rfind**（最后一次出现）：
            // 名字本身也可能带 " -> "，第一次出现的那个不是分隔符。
            const size_t arrow = rawName.rfind(" -> ");
            if (arrow != std::string::npos) {
                it.linkTo = U82W(rawName.substr(arrow + 4));
                rawName   = rawName.substr(0, arrow);
            }
        }
        it.name = U82W(rawName);
        if (!it.name.empty()) out.push_back(it);
    }
    std::sort(out.begin(), out.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static void ParseLsNames(const std::string& text, std::vector<FileItem>& out)
{
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        FileItem it;
        if (line.back() == '/') { it.isDir = true; line.pop_back(); }   // ls -p 的标记
        it.name = U82W(line);
        if (!it.name.empty()) out.push_back(it);
    }
}

// ---------------------------------------------------------------------------
// 五个动作
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// v2.0：写远端时的**临时文件名**
// ---------------------------------------------------------------------------
// 目的有两个，缺一不可：
//   ① 让"写完再替换"成为可能（先写它，成功了才 mv 覆盖目标）——
//      于是中途取消/断网最多留下一个残骸，目标文件永远是完整的旧内容；
//   ② 残骸要能被认出来，也要能被**一条通配符清掉**。
//
// ⚠️ 名字尽量**不要以点开头**：shell 和 FindFirstFile 的通配符 `*` 都不匹配
//   开头的点（`.foo` 得写 `.?*` 才匹配得上）。这里改成"原名 + 后缀"，
//   于是绝大多数临时文件名都能被 `*.nppbackpack-tmp-*` 一把扫掉；
//   原名本身以点开头的那种（便笺就是）再靠第二条 `.?*nppbackpack-tmp-*` 兜住。
static std::wstring TmpNameFor(const std::wstring& name)
{
    wchar_t b[512];
    swprintf_s(b, L"%s.nppbackpack-tmp-%lu", name.c_str(),
               (unsigned long)::GetCurrentProcessId());
    return b;
}

// 是不是我们自己的临时文件（列表不显示、Probe 时清掉）
bool IsTempName(const std::wstring& name)
{
    return name.find(L".nppbackpack-tmp-") != std::wstring::npos;
}

// 本地模式：把写残的临时文件清掉（远端那边由 Probe 的 shell 命令顺手清）。
// ⚠️ 要扫**两个**通配符 —— 见 TmpNameFor 上面那段关于"开头的点"的说明。
static void SweepLocalTempFiles(const std::wstring& root)
{
    const wchar_t* kPats[] = { L"\\*.nppbackpack-tmp-*", L"\\.?*nppbackpack-tmp-*" };
    for (const wchar_t* pat : kPats) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((root + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                ::DeleteFileW(JoinPath(root, fd.cFileName).c_str());
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
}

Res Probe(const Target& t)
{
    if (t.local) {
        if (t.localRoot.empty()) return { St::Error, L"没配置本地背包目录" };
        Res r0 = LocalEnsureDir(t.localRoot);
        if (!Ok(r0)) return r0;
        SweepLocalTempFiles(t.localRoot);      // v2.0：顺手清掉写残的临时文件
        return { St::Ok, L"" };
    }
    if (t.host.empty())  return { St::Error, L"没填服务器地址" };
    if (t.user.empty())  return { St::Error, L"没填 SSH 用户名" };
    if (SshExe().empty()) {
        return { St::Error,
                 L"系统里找不到 ssh.exe（Windows 设置 → 应用 → 可选功能 → 添加 OpenSSH 客户端）" };
    }

    // mkdir -p 既是"建目录"也是"连通性 + 认证"的一次性探测：
    // 有它就有目录，一步到位（王的要求：有则用、无则建）。
    // v2.0 追加一句清扫：把上一次"写了一半就被取消/断网"留下的临时文件删掉。
    // ⚠️ 两个通配符、而且**都不能加引号**（加了 shell 就不展开了）；
    //    开头的点那件事见 TmpNameFor 的说明。`rm -f` 对"没匹配到"是静默的，
    //    所以这两条不会影响这条命令的退出码。
    const std::string cmd = "mkdir -p " + Shq(t.folder) +
                            " && rm -f " + Shq(t.folder) + "/*.nppbackpack-tmp-*" +
                            " " + Shq(t.folder) + "/.?*nppbackpack-tmp-*" +
                            " && echo NPPDOCK_OK";
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (pr.started && pr.exitCode == 0 && pr.out.find("NPPDOCK_OK") != std::string::npos)
        return { St::Ok, L"" };
    return Classify(pr, L"连接服务器");
}

Res List(const Target& t, const std::wstring& relDir, std::vector<FileItem>& out)
{
    out.clear();
    if (t.local) {
        if (!Ok(Probe(t))) { /* 目录不存在也不影响：下面会报错 */ }
        return LocalList(LocalPathOf(t.localRoot, relDir), out);
    }

    const std::wstring dir = RemotePathOf(t.folder, relDir);

    // 主选：带大小与时间戳的 ls -l。
    // ⚠️ v2.0 加了 --quoting-style=literal：不加的话，名字是否被引号包起来取决于
    //    "标准输出是不是终端"这个 coreutils 的实现细节 —— 我们是管道，按理是
    //    literal，但把话说明白更稳（能跑这条命令的服务器本来就得有 GNU ls，
    //    因为 --time-style 也是 GNU 专有 —— 所以没有引入新的依赖）。
    std::string cmd = "cd " + Shq(dir) +
                      " && LC_ALL=C ls -lA --quoting-style=literal --time-style=+%s";
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (pr.started && pr.exitCode == 0) {
        ParseLs(pr.out, out);
        return { St::Ok, L"" };
    }

    // 回退：只有名字的 ls -1p（busybox 之类）
    cmd = "cd " + Shq(dir) + " && LC_ALL=C ls -1Ap";
    plan = MakePlan(t, U82W(cmd), false);
    pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (pr.started && pr.exitCode == 0) {
        ParseLsNames(pr.out, out);
        return { St::Ok, L"" };
    }
    return Classify(pr, L"列目录");
}

std::wstring FullPathOf(const Target& t, const std::wstring& relName)
{
    if (t.local) return LocalPathOf(t.localRoot, relName);
    return RemotePathOf(t.folder, relName);
}

// 递归删目录（本地）—— v1.8：删除改成"真删、删了就没了"，
// 删目录自然要连里面的东西一起删（远端一直是 `rm -rf`，本地以前却是
// "不是空目录就报错"，两边行为不一致）。
static bool RemoveDirRec(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring p = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirRec(p);
            else {
                ::SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(p.c_str());
            }
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    return ::RemoveDirectoryW(dir.c_str());
}

Res Remove(const Target& t, const std::wstring& relName, bool isDir)
{
    if (relName.empty()) return { St::Error, L"没指定要删的东西" };

    if (t.local) {
        const std::wstring p = LocalPathOf(t.localRoot, relName);
        if (::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
            return { St::NotFound, L"它已经不在了" };
        const BOOL ok = isDir ? (RemoveDirRec(p) ? TRUE : FALSE) : ::DeleteFileW(p.c_str());
        if (ok) return { St::Ok, L"" };
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return { St::NotFound, L"它已经不在了" };
        if (e == ERROR_ACCESS_DENIED)
            return { St::Error, L"没有权限删它（可能被别的程序占着）" };
        return { St::Error, L"删除失败（错误码 " + std::to_wstring(e) + L"）" };
    }

    if (SshExe().empty()) return { St::Error, L"系统里找不到 ssh.exe" };
    // 文件用 rm -f；目录用 rm -rf（调用方已经确认过）
    const std::string cmd = std::string(isDir ? "rm -rf -- " : "rm -f -- ") +
                            Shq(RemotePathOf(t.folder, relName)) + " && echo NPPDOCK_OK";
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (pr.started && pr.exitCode == 0 && pr.out.find("NPPDOCK_OK") != std::string::npos)
        return { St::Ok, L"" };
    return Classify(pr, L"删除");
}

// ---------------------------------------------------------------------------
// v1.7：Move / Exists / MakeDirs（回收站、撤销、重命名用）
// ---------------------------------------------------------------------------
Res Move(const Target& t, const std::wstring& fromRel, const std::wstring& toRel)
{
    if (fromRel.empty() || toRel.empty())
        return { St::Error, L"没指定要移动的东西" };

    if (fromRel == toRel) return { St::Ok, L"" };   // 同名 = 无事可做，别报"已存在"

    if (t.local) {
        const std::wstring a = LocalPathOf(t.localRoot, fromRel);
        const std::wstring b = LocalPathOf(t.localRoot, toRel);
        if (::GetFileAttributesW(a.c_str()) == INVALID_FILE_ATTRIBUTES)
            return { St::NotFound, L"它已经不在了" };
        if (::GetFileAttributesW(b.c_str()) != INVALID_FILE_ATTRIBUTES)
            return { St::Error, L"目标位置已经有同名的东西了" };
        if (::MoveFileExW(a.c_str(), b.c_str(), 0)) return { St::Ok, L"" };
        wchar_t e[160];
        swprintf_s(e, L"移动失败（错误码 %lu）", ::GetLastError());
        return { St::Error, e };
    }

    if (SshExe().empty()) return { St::Error, L"系统里找不到 ssh.exe" };

    // ⚠️ v2.0：和本地的脾气对齐 —— **目标已存在就报错，不覆盖**（契约见 packcore.h）。
    //   以前这里是 `mv -f`，会静默盖掉服务器上的同名文件；而同一个动作在本地
    //   却是报错。两种脾气迟早出人命，所以统一成"先查再移，不覆盖"。
    const std::wstring src = RemotePathOf(t.folder, fromRel);
    const std::wstring dst = RemotePathOf(t.folder, toRel);
    const std::string cmd =
        "if [ -e " + Shq(dst) + " ]; then echo NPPDOCK_EXISTS; exit 7; fi; mv -- " +
        Shq(src) + " " + Shq(dst) + " && echo NPPDOCK_OK";
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (pr.out.find("NPPDOCK_EXISTS") != std::string::npos)
        return { St::Error, L"目标位置已经有同名的东西了" };
    if (pr.started && pr.exitCode == 0 && pr.out.find("NPPDOCK_OK") != std::string::npos)
        return { St::Ok, L"" };
    return Classify(pr, L"移动");
}

Res Exists(const Target& t, const std::wstring& rel, bool& exists)
{
    exists = false;
    if (rel.empty()) return { St::Error, L"没指定路径" };

    if (t.local) {
        exists = ::GetFileAttributesW(LocalPathOf(t.localRoot, rel).c_str())
                 != INVALID_FILE_ATTRIBUTES;
        return { St::Ok, L"" };
    }

    if (SshExe().empty()) return { St::Error, L"系统里找不到 ssh.exe" };
    // test -e：文件或目录都算"在"。用输出判断，别依赖退出码 —— 有些 shell 包装会吃掉它。
    const std::string cmd = "test -e " + Shq(RemotePathOf(t.folder, rel)) +
                            " && echo YES || echo NO";
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (!pr.started) return Classify(pr, L"检查");
    exists = pr.out.find("YES") != std::string::npos;
    return { St::Ok, L"" };
}

Res MakeDirs(const Target& t, const std::wstring& relDir)
{
    if (relDir.empty()) return { St::Ok, L"" };

    if (t.local) return LocalEnsureDir(LocalPathOf(t.localRoot, relDir));

    if (SshExe().empty()) return { St::Error, L"系统里找不到 ssh.exe" };
    const std::string cmd = "mkdir -p " + Shq(RemotePathOf(t.folder, relDir)) +
                            " && echo NPPDOCK_OK";
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (pr.started && pr.exitCode == 0 && pr.out.find("NPPDOCK_OK") != std::string::npos)
        return { St::Ok, L"" };
    return Classify(pr, L"建目录");
}

Res Read(const Target& t, const std::wstring& name, std::string& data)
{
    data.clear();
    if (t.local) {
        const std::wstring p = JoinPath(t.localRoot, name);
        if (!ReadWholeFile(p, data)) {
            return { ::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES
                         ? St::NotFound : St::Error,
                     L"读不到这个文件" };
        }
        return { St::Ok, L"" };
    }

    // ssh 的**标准输出**直接重定向到临时文件（RunCapture 干的就是这件事）
    const std::string cmd = "cat " + Shq(U82W(RJoin(t.folder, name)));
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);
    if (!pr.started) return Classify(pr, L"读文件");
    if (pr.exitCode != 0) {
        // cat 失败：文件不存在时 ExitStatus=1 且 stderr 说 No such file
        Res r = Classify(pr, L"读文件");
        if (r.st == St::Error) r.st = St::NotFound;
        if (r.msg.empty()) r.msg = L"读不到这个文件";
        return r;
    }
    data = pr.out;
    return { St::Ok, L"" };
}

// ---------------------------------------------------------------------------
// v2.0：把"写远端"构造成**要么全成、要么目标文件原样不动**
// ---------------------------------------------------------------------------
// 只写 `cat > tmp && mv tmp dst` 是**不够的**：如果连接是"干净地收到 EOF"而
// 内容只到一半（中间设备正常关闭连接就会这样，实测复现过），`cat` 会成功退出、
// `mv` 照样把残缺文件盖上去。所以要连**字节数**一起校验。
//
// 命令的三段含义：
//   ① cat 写临时文件 → ② 临时文件大小必须正好是 expectBytes → ③ 才 mv 覆盖目标
//   任何一步失败：把临时文件删掉、往 stderr 放一句可识别的记号、以**非 0**退出。
//   ⚠️ 失败分支末尾那个 `false` 不能省 —— 少了它，整条命令的退出码会变成
//      `rm` 的（0），上层就会把"失败"当成"成功"。
static std::string AtomicWriteCmd(const std::string& dstPath,
                                  const std::string& tmpPath,
                                  unsigned long long expectBytes)
{
    char n[32];
    sprintf_s(n, "%llu", expectBytes);
    const std::string d = Shq(U82W(dstPath)), t = Shq(U82W(tmpPath));
    return "{ cat > " + t + " && [ \"$(wc -c < " + t + " | tr -d ' ')\" = \"" + n +
           "\" ] && mv -f -- " + t + " " + d + "; }" +
           " && echo NPPDOCK_OK" +
           " || { echo NPPDOCK_INCOMPLETE >&2; rm -f " + t + "; false; }";
}

// 失败记号 → 人话（Classify / ClassifyStream 共用）
static bool IsIncompleteMarker(const std::string& s)
{
    return s.find("NPPDOCK_INCOMPLETE") != std::string::npos;
}

// ---------------------------------------------------------------------------
// v2.0：一次 ssh 办三件事（见 packcore.h 的说明）
// ---------------------------------------------------------------------------
// 找"独占一整行"的哨兵，返回那一行**行首**的下标。
// 为什么非要"整行"：`<mk> NOTE` 是 `<mk> NOTEEND` 的前缀，只 find 子串会认错行。
static bool FindMarkerLine(const std::string& text, const std::string& mk,
                           const char* tag, size_t& lineStart)
{
    const std::string pat = mk + " " + tag;
    size_t p = 0;
    while ((p = text.find(pat, p)) != std::string::npos) {
        const bool atLineStart = (p == 0) || text[p - 1] == '\n';
        const size_t q = p + pat.size();
        const bool atLineEnd = (q >= text.size()) || text[q] == '\n' || text[q] == '\r';
        if (atLineStart && atLineEnd) { lineStart = p; return true; }
        p = q;
    }
    return false;
}

static size_t AfterLine(const std::string& text, size_t lineStart)
{
    const size_t e = text.find('\n', lineStart);
    return (e == std::string::npos) ? text.size() : e + 1;
}

// 把一段会话输出拆成三段。抽成一个函数是为了能被测试壳直接喂样本
// （见 _t/test_session.cpp）—— 分割逻辑出错的话，"刷不出来"这种症状
// 极难从现象反推原因，所以它必须能被单独验证。
static void ParseSessionFrames(const std::string& text, const std::string& mk,
                               SessionOut& out)
{
    size_t dummy = 0;
    out.probeOk = FindMarkerLine(text, mk, "OK", dummy);

    size_t pNote = 0, pNoteEnd = 0, pList = 0, pListEnd = 0;
    if (!FindMarkerLine(text, mk, "NOTE",    pNote)    ||
        !FindMarkerLine(text, mk, "NOTEEND", pNoteEnd) ||
        !FindMarkerLine(text, mk, "LIST",    pList)    ||
        !FindMarkerLine(text, mk, "LISTEND", pListEnd) ||
        !(pNote < pNoteEnd && pNoteEnd < pList && pList < pListEnd)) {
        return;                       // 帧不全 → 两段都算没拿到
    }

    const size_t noteFrom = AfterLine(text, pNote);
    std::string note = text.substr(noteFrom, pNoteEnd - noteFrom);
    // 去掉我们额外 echo 的那一个换行（只去一个 —— 便笺自己结尾的换行要留着）
    if (!note.empty() && note.back() == '\n') note.pop_back();
    // ⚠️ 只去 \n，**不能**去 \r：便笺很可能是 CRLF 的，去了就把内容改了。
    out.note   = note;
    out.noteOk = true;

    const size_t listFrom = AfterLine(text, pList);
    ParseLs(text.substr(listFrom, pListEnd - listFrom), out.files);
    out.listOk = true;
}

Res Session(const Target& t, const std::wstring& relDir, const std::wstring& noteName,
            SessionOut& out)
{
    out = SessionOut{};

    // 本地模式：本来就是三次内存操作，没有"握手"这回事 —— 直接用老路，
    // 只是把它包成同一个形状（调用方就不用分两套逻辑）。
    if (t.local) {
        out.usable = true;
        Res p = Probe(t);
        out.probeOk = Ok(p);
        if (!out.probeOk) return p;
        out.noteOk = Ok(Read(t, noteName, out.note));
        out.listOk = Ok(List(t, relDir, out.files));
        return { St::Ok, L"" };
    }

    if (t.host.empty())  return { St::Error, L"没填服务器地址" };
    if (t.user.empty())  return { St::Error, L"没填 SSH 用户名" };
    if (SshExe().empty())
        return { St::Error,
                 L"系统里找不到 ssh.exe（Windows 设置 → 应用 → 可选功能 → 添加 OpenSSH 客户端）" };

    // 哨兵：进程号 + 运行毫秒，每次都不一样
    char mkbuf[48];
    sprintf_s(mkbuf, "NPPDOCK%08lX%08lX",
              (unsigned long)::GetCurrentProcessId(),
              (unsigned long)(::GetTickCount64() & 0xFFFFFFFFull));
    const std::string mk = mkbuf;

    const std::string folder   = Shq(t.folder);
    const std::string notePath = Shq(U82W(RJoin(t.folder, noteName)));
    const std::string dirPath  = Shq(U82W(RJoin(t.folder, relDir)));

    // ⚠️ 各段之间用**换行**串起来（不是 &&）：mkdir 失败时后面几段照样跑，
    //    这样"连上了但目录没权限"和"根本没连上"能区分开（看有没有哨兵行）。
    std::string cmd;
    cmd += "mkdir -p " + folder +
           " && rm -f " + folder + "/*.nppbackpack-tmp-* " +
           folder + "/.?*nppbackpack-tmp-* && echo " + mk + " OK\n";
    cmd += "echo " + mk + " NOTE\n";
    cmd += "cat " + notePath + " 2>/dev/null\n";
    cmd += "echo \"\"\n";                 // 收尾换行：便笺末行没换行时也不会把哨兵粘上去
    cmd += "echo " + mk + " NOTEEND\n";
    cmd += "echo " + mk + " LIST\n";
    cmd += "cd " + dirPath + " && LC_ALL=C ls -lA --quoting-style=literal "
                             "--time-style=+%s\n";
    cmd += "echo " + mk + " LISTEND\n";

    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);

    size_t pOk = 0;
    const bool gotOk = FindMarkerLine(pr.out, mk, "OK", pOk);

    if (!gotOk) {
        if (!pr.started || pr.exitCode != 0) {
            // 连接/认证/权限这一层就失败了 —— 老路会给出一模一样的话，
            // 没必要再花三次 ssh 去重复一遍（服务器不通时那是 3×6 秒的干等）。
            out.usable  = true;
            out.probeOk = false;
            out.res     = Classify(pr, L"连接服务器");
            return out.res;
        }
        // 退出码 0 却一个哨兵都没有 = 对面 shell 不认这套写法（老 busybox 之类）
        // → 明确报"不可用"，让调用方走老路。
        out.usable = false;
        return { St::Error, L"会话输出无法解析" };
    }

    out.usable  = true;
    out.probeOk = false;          // 由 ParseSessionFrames 重新判定
    ParseSessionFrames(pr.out, mk, out);
    if (!out.probeOk) {
        // 连不上的话上面那条分支已经拦掉了，走到这里多半是"连上了但 mkdir 没过"
        // （目录没权限之类）—— 交给老路的分类器给个准确的解释。
        // （一次 Classify 不能覆盖所有情况，但这里至少不是"静默"。）
        out.res = Classify(pr, L"连接服务器");
    }
    return { St::Ok, L"" };
}

Res Write(const Target& t, const std::wstring& name, const std::string& data)
{
    // ★ v2.0：**先写临时文件，成功后再整体替换**。
    //   老写法是 `cat > 目标`，那是"先把目标截断、再往里写" —— 中途取消或断网，
    //   服务器上原来的内容就没了（这是这个应用里唯一会真丢东西的地方）。
    const std::wstring tmpName = TmpNameFor(name);

    if (t.local) {
        const std::wstring p  = JoinPath(t.localRoot, name);
        const std::wstring tp = JoinPath(t.localRoot, tmpName);
        if (!WriteWholeFile(tp, data)) {
            ::DeleteFileW(tp.c_str());
            return { St::Error, L"写不进这个文件" };
        }
        if (!::MoveFileExW(tp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ::DeleteFileW(tp.c_str());
            return { St::Error, L"写不进这个文件（替换失败）" };
        }
        return { St::Ok, L"" };
    }

    // 数据从**本地临时文件**喂给 ssh 的 stdin（见 packcore.h 的文件头说明）
    const std::wstring tmp = TempFile(L"in");
    if (!WriteWholeFile(tmp, data)) return { St::Error, L"本地临时文件写不进去" };

    const std::string cmd = AtomicWriteCmd(RJoin(t.folder, name),
                                           RJoin(t.folder, tmpName),
                                           (unsigned long long)data.size());
    SshPlan plan = MakePlan(t, U82W(cmd), true);
    ProcOut pr = RunCapture(SshExe(), plan.args, tmp, plan.env, 30000);
    ::DeleteFileW(tmp.c_str());
    if (IsIncompleteMarker(pr.out) || IsIncompleteMarker(pr.err))
        return { St::Error, L"内容没传完（服务器上的原文件没动）" };
    if (pr.started && pr.exitCode == 0 && pr.out.find("NPPDOCK_OK") != std::string::npos)
        return { St::Ok, L"" };
    return Classify(pr, L"写文件");
}

// ---------------------------------------------------------------------------
// v1.6：流式（带进度 + 可取消）的传输
//
// 进度从哪儿看 —— 两个来源都不用额外线程：
//   · 下载：看**落盘那个文件多大**（GetFileSizeEx）。ssh 的 stdout 就是直接
//     写进它的，所以这个数字就是"已经拿到多少字节"，最准。
//   · 上传：看 **stdin 文件的读指针走到哪儿**。这里有个好用的系统细节：
//     CreateProcess 是**继承句柄**，父子拿到的是**同一个文件对象**，
//     **文件指针是共享的** —— 于是父进程里 SetFilePointerEx(0, FILE_CURRENT)
//     就能读出 ssh 已经吃掉多少字节，不必另开线程去数。
// ---------------------------------------------------------------------------
static const DWORD kTransferTimeoutMs = 10u * 60u * 1000u;   // 单次传输的总上限
static const DWORD kStallMs           = 45u * 1000u;         // 多久没进展就算卡死

enum class ProgSrc { None, OutFile, InFilePos };

struct StreamOut {
    bool         started = false;
    bool         cancelled = false;
    DWORD        exitCode = (DWORD)-1;
    std::string  err;
    std::wstring why;
};

static long long QueryProgress(HANDLE hOut, HANDLE hIn, ProgSrc src)
{
    if (src == ProgSrc::OutFile && hOut && hOut != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (::GetFileSizeEx(hOut, &sz)) return (long long)sz.QuadPart;
    }
    if (src == ProgSrc::InFilePos && hIn && hIn != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER zero{}, cur{};
        if (::SetFilePointerEx(hIn, zero, &cur, FILE_CURRENT))
            return (long long)cur.QuadPart;
    }
    return -1;
}

// 目标文件所在目录可能不存在（用户把 download_dir 指到新地方了）→ 先建出来。
//
// ⚠️⚠️ 必须**递归**建：以前这里只调了一次 CreateDirectoryW，
//   而 `%LOCALAPPDATA%\NppDockBackpack\open` 这种是**两级**新目录 ——
//   父目录还不存在时那一次必然失败，紧接着 CreateFile 也失败，
//   界面报出来的是"创建文件句柄失败"，看着像程序坏了，
//   其实只是"中间那层目录没建出来"（王 2026-10-04 报的就是这个）。
static bool EnsureParentDir(const std::wstring& path)
{
    const size_t k = path.find_last_of(L"\\/");
    if (k == std::wstring::npos || k == 0) return true;      // 没目录部分
    const std::wstring dir = path.substr(0, k);
    if (::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return LocalEnsureDir(dir).st == St::Ok;
}

static StreamOut RunStream(const std::wstring& exe,
                           const std::vector<std::wstring>& args,
                           const std::wstring& stdinFile,
                           const std::wstring& stdoutFile,   // 空 = 用临时文件装走
                           const std::vector<std::pair<std::wstring, std::wstring>>& envExtra,
                           DWORD timeoutMs, ProgSrc src, long long total,
                           ProgressCb cb, void* user)
{
    StreamOut r;
    const bool         ownOut  = stdoutFile.empty();
    const std::wstring outPath = ownOut ? TempFile(L"sout") : stdoutFile;
    const std::wstring errPath = TempFile(L"serr");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = ::CreateFileW(errPath.c_str(), GENERIC_WRITE, 0, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hIn  = ::CreateFileW(stdinFile.empty() ? L"NUL" : stdinFile.c_str(),
                                GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE ||
        hIn == INVALID_HANDLE_VALUE) {
        // 分清楚是哪一个没建成 —— 以前一律报"创建文件句柄失败"，
        // 排障时完全看不出是"目标目录没建出来"还是"临时文件写不了"。
        if (hOut == INVALID_HANDLE_VALUE)
            r.why = L"建不了输出文件（目标目录不存在，或者没有写权限）";
        else if (hErr == INVALID_HANDLE_VALUE)
            r.why = L"建不了临时文件（%TEMP% 不可写？）";
        else
            r.why = L"打不开本地输入文件";
        if (hOut != INVALID_HANDLE_VALUE) ::CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) ::CloseHandle(hErr);
        if (hIn  != INVALID_HANDLE_VALUE) ::CloseHandle(hIn);
        ::DeleteFileW(errPath.c_str());
        if (ownOut) ::DeleteFileW(outPath.c_str());
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hIn;
    si.hStdOutput = hOut;
    si.hStdError = hErr;

    std::wstring cl = BuildCmdLine(exe, args);
    std::vector<wchar_t> cmd(cl.begin(), cl.end());
    cmd.push_back(L'\0');
    std::vector<wchar_t> env = BuildEnv(envExtra);

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags,
                          env.data(), nullptr, &si, &pi)) {
        wchar_t b[128];
        swprintf_s(b, L"启动 %s 失败（错误码 %lu）",
                   exe.substr(exe.find_last_of(L'\\') + 1).c_str(), ::GetLastError());
        r.why = b;
    } else {
        r.started = true;
        ::CloseHandle(pi.hThread);

        const DWORD t0 = ::GetTickCount();
        DWORD lastMove = t0;
        long long lastDone = -1;
        for (;;) {
            if (::WaitForSingleObject(pi.hProcess, 150) == WAIT_OBJECT_0) break;
            // v1.9：抽泵放在回调**之前** —— 回调本身也会抽（界面层那样写），
            //   但上传时 cb 可能为空，那时也得让界面活着。
            if (g_pump) g_pump();

            const long long done = QueryProgress(hOut, hIn, src);
            if (done != lastDone) { lastDone = done; lastMove = ::GetTickCount(); }

            if ((DWORD)(::GetTickCount() - t0) > timeoutMs) {
                ::TerminateProcess(pi.hProcess, 1);
                ::WaitForSingleObject(pi.hProcess, 3000);
                r.why = L"超时";
                break;
            }
            // 45 秒一点没动 → 当成卡死，别再让用户干等
            if (src != ProgSrc::None && (DWORD)(::GetTickCount() - lastMove) > kStallMs) {
                ::TerminateProcess(pi.hProcess, 1);
                ::WaitForSingleObject(pi.hProcess, 3000);
                r.why = L"传输停滞（45 秒没有任何进展）";
                break;
            }
            if (cb && !cb(done < 0 ? 0 : done, total, user)) {
                ::TerminateProcess(pi.hProcess, 1);
                ::WaitForSingleObject(pi.hProcess, 3000);
                r.cancelled = true;
                break;
            }
        }
        ::GetExitCodeProcess(pi.hProcess, &r.exitCode);
        ::CloseHandle(pi.hProcess);
    }

    ::CloseHandle(hIn);
    ::CloseHandle(hOut);
    ::CloseHandle(hErr);
    ReadWholeFile(errPath, r.err);
    ::DeleteFileW(errPath.c_str());
    if (ownOut) ::DeleteFileW(outPath.c_str());
    return r;
}

// 把 StreamOut 翻译成跟同步版本同一套"人话"（复用 Classify 的判据）
static Res ClassifyStream(const StreamOut& so, const wchar_t* what)
{
    ProcOut pr;
    pr.started  = so.started;
    pr.exitCode = so.exitCode;
    pr.err      = so.err;
    pr.why      = so.why;
    return Classify(pr, what);
}

// 本地模式的"下载/上传"就是拷贝 —— 照样分块、照样能给进度、能取消
static Res CopyLocalFile(const std::wstring& src, const std::wstring& dst,
                         long long total, ProgressCb cb, void* user)
{
    HANDLE hi = ::CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hi == INVALID_HANDLE_VALUE) {
        return { ::GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES
                     ? St::NotFound : St::Error, L"读不到这个文件" };
    }
    if (total <= 0) {
        LARGE_INTEGER sz{};
        if (::GetFileSizeEx(hi, &sz)) total = (long long)sz.QuadPart;
    }
    if (!EnsureParentDir(dst)) {
        ::CloseHandle(hi);
        return { St::Error, L"建不了目标目录：" + dst };
    }
    // ★ v2.0：本地这条也走"先写临时文件、成功再替换" —— 否则取消/出错时
    //   目标文件已经被 CREATE_ALWAYS 截断了（同远端那个老毛病）。
    const std::wstring dstTmp = dst + L".nppbackpack-tmp-" +
                               std::to_wstring((unsigned long)::GetCurrentProcessId());
    HANDLE ho = ::CreateFileW(dstTmp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ho == INVALID_HANDLE_VALUE) {
        ::CloseHandle(hi);
        return { St::Error, L"写不进本地文件：" + dst };
    }

    Res out{ St::Ok, L"" };
    std::vector<char> buf(256 * 1024);
    long long done = 0;
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(hi, buf.data(), (DWORD)buf.size(), &got, nullptr) || got == 0)
            break;                                            // 读完 / 出错
        DWORD put = 0;
        if (!::WriteFile(ho, buf.data(), got, &put, nullptr) || put != got) {
            out = { St::Error, L"写本地文件失败" };
            break;
        }
        done += got;
        if (cb && !cb(done, total, user)) { out = { St::Cancelled, L"已取消" }; break; }
    }
    ::CloseHandle(hi);
    ::CloseHandle(ho);

    // 收尾：成功就整体替换过去；取消/失败就把临时文件删掉，目标文件保持原样。
    if (out.st == St::Ok) {
        if (!::MoveFileExW(dstTmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ::DeleteFileW(dstTmp.c_str());
            return { St::Error, L"写不进本地文件（替换失败）：" + dst };
        }
    } else {
        ::DeleteFileW(dstTmp.c_str());
    }
    return out;
}

static Res UploadFile(const Target& t, const std::wstring& name,
                      const std::wstring& localPath, long long total,
                      ProgressCb cb, void* user)
{
    if (t.local)
        return CopyLocalFile(localPath, LocalPathOf(t.localRoot, name), total, cb, user);
    if (SshExe().empty()) return { St::Error, L"系统里找不到 ssh.exe" };

    // ★ v2.0：同 Write —— 上传也走"临时文件 + 原子替换"（上传途中取消/断网，
    //   服务器上原来是老文件、不会变成半截）。残留的临时文件由下一次 Probe
    //   顺手清掉（见 Probe 的命令），列表里也一律过滤掉。
    //   成功判据只用**退出码**：上面那条命令里任何一步失败都会以非 0 退出
    //   （末尾有显式的 `false`），而这条链路的标准输出被重定向掉了。
    const std::wstring tmpName = TmpNameFor(name);
    const std::string cmd = AtomicWriteCmd(RJoin(t.folder, name),
                                           RJoin(t.folder, tmpName),
                                           (unsigned long long)(total > 0 ? total : 0));
    SshPlan plan = MakePlan(t, U82W(cmd), true);
    StreamOut so = RunStream(SshExe(), plan.args, localPath, L"", plan.env,
                             kTransferTimeoutMs, ProgSrc::InFilePos, total, cb, user);
    if (so.cancelled) return { St::Cancelled, L"已取消" };
    if (IsIncompleteMarker(so.err))
        return { St::Error, L"上传不完整（服务器上的原文件没动）" };
    if (so.started && so.exitCode == 0) return { St::Ok, L"" };
    return ClassifyStream(so, L"上传");
}

Res WriteFromFile(const Target& t, const std::wstring& name,
                  const std::wstring& localPath, long long expectSize,
                  ProgressCb cb, void* user)
{
    long long total = expectSize;
    if (total <= 0) {
        HANDLE h = ::CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (::GetFileSizeEx(h, &sz)) total = (long long)sz.QuadPart;
            ::CloseHandle(h);
        }
    }
    return UploadFile(t, name, localPath, total, cb, user);
}

Res ReadToFile(const Target& t, const std::wstring& name,
               const std::wstring& localPath, long long expectSize,
               ProgressCb cb, void* user)
{
    if (t.local)
        return CopyLocalFile(LocalPathOf(t.localRoot, name), localPath,
                             expectSize, cb, user);
    if (SshExe().empty()) return { St::Error, L"系统里找不到 ssh.exe" };

    if (!EnsureParentDir(localPath))
        return { St::Error, L"建不了目标目录：" + localPath };
    const std::string cmd = "cat " + Shq(U82W(RJoin(t.folder, name)));
    SshPlan plan = MakePlan(t, U82W(cmd), false);
    StreamOut so = RunStream(SshExe(), plan.args, L"", localPath, plan.env,
                             kTransferTimeoutMs, ProgSrc::OutFile, expectSize, cb, user);
    if (so.cancelled) { ::DeleteFileW(localPath.c_str()); return { St::Cancelled, L"已取消" }; }
    if (so.started && so.exitCode == 0) return { St::Ok, L"" };
    ::DeleteFileW(localPath.c_str());              // 半截文件不留在用户眼皮底下
    return ClassifyStream(so, L"下载");
}

std::wstring Describe(const Target& t)
{
    if (t.local) return L"本地目录：" + t.localRoot;

    std::wstring s = SshExe();
    if (s.empty()) s = L"(找不到 ssh.exe)";
    s += L"\n  ssh";
    if (_wcsicmp(t.authType.c_str(), L"key") == 0 && !t.keyPath.empty())
        s += L" -i \"" + t.keyPath + L"\"";
    s += L" -p " + std::to_wstring(t.port) + L" " + t.user + L"@" + t.host;
    s += L"\n  背包目录：" + t.folder;
    if (_wcsicmp(t.authType.c_str(), L"key") != 0)
        s += L"\n  认证：密码（走 SSH_ASKPASS 自问自答，密码不会被写进命令行）";
    else
        s += L"\n  认证：私钥";
    return s;
}

// 见 packcore.h：原样跑一遍探针命令，把原始输出交出去（不动界面）
std::wstring Diagnose(const Target& t)
{
    std::wstring s = Describe(t) + L"\n\n";
    s.reserve(4096);

    if (t.local) {
        Res r = LocalEnsureDir(t.localRoot);
        s += L"本地模式，不做 SSH 探测。\n";
        s += L"目录检查：" + std::wstring(Ok(r) ? L"OK" : r.msg) + L"\n";
        return s;
    }
    if (SshExe().empty()) { s += L"系统里找不到 ssh.exe\n"; return s; }

    // 与 Probe 用**同一条**命令：这样"诊断看到的"和"实际连的"必然一致
    const std::string cmd = "mkdir -p " + Shq(t.folder) + " && echo NPPDOCK_OK";
    s += L"实际执行的远端命令：\n  " + U82W(cmd) + L"\n\n";

    SshPlan plan = MakePlan(t, U82W(cmd), false);
    ProcOut pr = RunCapture(SshExe(), plan.args, L"", plan.env, 25000);

    wchar_t b[160];
    swprintf_s(b, L"退出码：%lu%s\n", (unsigned long)pr.exitCode,
               pr.started ? L"" : L"（进程没起来）");
    s += b;
    if (!pr.why.empty()) s += L"启动问题：" + pr.why + L"\n";
    s += L"--- stdout ---\n" + U82W(pr.out) +
         (pr.out.empty() || pr.out.back() == '\n' ? L"" : L"\n") +
         L"--- stderr ---\n" + U82W(pr.err) +
         (pr.err.empty() || pr.err.back() == '\n' ? L"" : L"\n");
    return s;
}

} // namespace packcore
