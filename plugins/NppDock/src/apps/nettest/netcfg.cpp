// ============================================================================
// netcfg.cpp —— 见 netcfg.h
// ============================================================================
#include "netcfg.h"
#include "../../common/jsonlite.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace netcfg {

// DNS 记录类型的"数字 -> 名字"：写配置文件时用的是**名字**（人改起来直观）。
// 这里刻意跟 netcore 的表解耦 —— 配置层不该依赖引擎层（依赖方向：UI -> 两者）。
static const char* DnsTypeNameForCfg(int t)
{
    switch (t) {
    case 28: return "AAAA";
    case 5:  return "CNAME";
    case 15: return "MX";
    case 16: return "TXT";
    case 2:  return "NS";
    case 12: return "PTR";
    case 1:  return "A";
    default: return "A";
    }
}

// ---------------------------------------------------------------------------
// 内置默认端口集合：100 个常用 TCP 端口
// ---------------------------------------------------------------------------
// 挑选原则：① 排在前面的按"日常最可能遇到的"排（写进文件里一眼能看懂）；
//           ② 覆盖常见服务（Web/邮件/数据库/远程/中间件/消息队列/容器）；
//           ③ 不多塞冷门端口 —— 100 个是王定的量级，扫一轮几秒钟。
const char* DefaultPortSpec()
{
    static const char* kSpec =
        "21,22,23,25,53,67,68,69,80,81,88,110,111,123,135,137,138,139,143,161,"
        "162,179,389,443,445,465,500,514,515,520,548,554,587,623,631,636,873,902,"
        "989,990,993,995,1025,1080,1099,1194,1433,1434,1521,1701,1723,1883,1900,"
        "2049,2082,2083,2181,2375,2376,3000,3128,3260,3306,3389,4443,4505,4506,"
        "5000,5060,5222,5357,5432,5555,5601,5672,5900,5984,6000,6379,6443,7001,"
        "7002,7077,8000,8008,8009,8080,8081,8088,8090,8161,8443,8500,8888,9000,"
        "9001,9042,9092,9200,9300";
    return kSpec;
}

std::wstring ConfigPath()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (n == 0 || n >= _countof(buf)) return L"NppDockApp_NET.json";
    std::wstring p = buf;
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"NppDockApp_NET.json";
    return p.substr(0, slash + 1) + L"NppDockApp_NET.json";
}

// ---------------------------------------------------------------------------
// 小工具：读写文件 / 宽窄串转换
// ---------------------------------------------------------------------------
namespace {

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                        nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                     &out[0], n, nullptr, nullptr);
    return out;
}

bool ReadAll(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz)) { ::CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    const bool ok = out.empty() ||
                    (::ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    ::CloseHandle(h);
    return ok;
}

bool WriteAll(const std::wstring& path, const std::string& data)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = data.empty() ||
                    (::WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) &&
                     wrote == data.size());
    ::CloseHandle(h);
    return ok;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && sp(s[a])) ++a;
    while (b > a && sp(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string Lower(const std::string& s)
{
    std::string o = s;
    for (char& c : o) c = (char)::tolower((unsigned char)c);
    return o;
}

} // namespace

// ---------------------------------------------------------------------------
// 端口串解析
// ---------------------------------------------------------------------------
std::vector<int> ParsePortSpec(const std::string& spec, bool& outUsedDefault)
{
    outUsedDefault = false;
    const std::string src = Trim(spec).empty() ? std::string(DefaultPortSpec()) : spec;
    outUsedDefault = Trim(spec).empty();

    std::set<int> ports;
    size_t i = 0;
    while (i < src.size()) {
        // 跳到下一段
        while (i < src.size() && (src[i] == ',' || src[i] == ' ' || src[i] == ';'))
            ++i;
        if (i >= src.size()) break;
        const size_t start = i;
        while (i < src.size() && src[i] != ',' && src[i] != ';') ++i;
        const std::string tok = Trim(src.substr(start, i - start));
        if (tok.empty()) continue;

        const size_t dash = tok.find('-');
        if (dash == std::string::npos) {
            const int p = std::atoi(tok.c_str());
            if (p >= 1 && p <= 65535) ports.insert(p);
            continue;
        }
        const int lo = std::atoi(tok.substr(0, dash).c_str());
        const int hi = std::atoi(tok.substr(dash + 1).c_str());
        if (lo < 1 || hi > 65535 || hi < lo) continue;
        // 区间上限做个防呆：一口气扫 6 万个端口要按分钟计，容易让人以为卡死
        const int cap = (hi - lo > 4096) ? lo + 4096 : hi;
        for (int p = lo; p <= cap; ++p) ports.insert(p);
    }

    std::vector<int> out(ports.begin(), ports.end());
    if (out.empty()) {
        // 写错了就退回默认（上层会在输出区里说明）
        outUsedDefault = true;
        const std::string def = DefaultPortSpec();
        bool dummy = false;
        return ParsePortSpec(def, dummy);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------
bool Load(Config& out, std::wstring& outErr, bool& outCreated)
{
    outCreated = false;
    outErr.clear();
    out = Config{};

    const std::wstring path = ConfigPath();
    std::string raw;
    if (!ReadAll(path, raw)) {
        // 首次运行：写一份带说明的初值，用户一看就知道能改什么
        Save(out);
        outCreated = true;
        return true;
    }

    jsonlite::Value root;
    std::string err;
    if (!jsonlite::Parse(raw, root, err) || !root.IsObj()) {
        // ⚠️ 不覆盖用户的原文件：改名留证据，然后用默认值继续跑。
        //    （配置文件是用户手改过的，直接覆盖等于把他的修改弄丢。）
        const std::wstring bad = path + L".bad";
        ::MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        outErr = L"配置文件解析失败（已备份为 .bad，本次用默认值）：" +
                 Utf8ToWide(err);
        Save(out);
        return false;
    }

    if (const jsonlite::Value* h = root.Find("hosts")) {
        out.last = h->Str("last");
        if (const jsonlite::Value* r = h->Find("recent"); r && r->IsArr()) {
            for (const auto& v : r->arr)
                if (v.IsStr() && !v.str.empty()) out.recent.push_back(v.str);
        }
        if (const jsonlite::Value* cm = h->Find("common"); cm && cm->IsArr()) {
            for (const auto& v : cm->arr) {
                if (!v.IsObj()) continue;
                HostCount hc;
                hc.host  = v.Str("host");
                hc.count = v.Int("count", 0);
                if (!hc.host.empty()) out.common.push_back(hc);
            }
        }
    }
    if (const jsonlite::Value* p = root.Find("ping")) {
        out.pingCount      = p->Int("count", out.pingCount);
        out.pingIntervalMs = p->Int("intervalMs", out.pingIntervalMs);
        out.pingTimeoutMs  = p->Int("timeoutMs", out.pingTimeoutMs);
        out.pingPayload    = p->Int("payload", out.pingPayload);
    }
    if (const jsonlite::Value* t = root.Find("tracert")) {
        out.traceMaxHops      = t->Int("maxHops", out.traceMaxHops);
        out.traceProbesPerHop = t->Int("probesPerHop", out.traceProbesPerHop);
        out.traceTimeoutMs    = t->Int("timeoutMs", out.traceTimeoutMs);
    }
    if (const jsonlite::Value* s = root.Find("portscan")) {
        out.portSpec        = s->Str("ports", out.portSpec);
        out.scanTimeoutMs   = s->Int("timeoutMs", out.scanTimeoutMs);
        out.scanConcurrency = s->Int("concurrency", out.scanConcurrency);
    }
    if (const jsonlite::Value* d = root.Find("dns")) {
        // 记录类型允许写成 "A"/"aaaa" 这类名字，也允许直接写数字
        const jsonlite::Value* ty = d->Find("type");
        if (ty && ty->IsNum()) {
            out.dnsType = (int)ty->num;
        } else if (ty && ty->IsStr()) {
            const std::string n = Lower(ty->str);
            if      (n == "a")     out.dnsType = 1;
            else if (n == "aaaa")  out.dnsType = 28;
            else if (n == "cname") out.dnsType = 5;
            else if (n == "mx")    out.dnsType = 15;
            else if (n == "txt")   out.dnsType = 16;
            else if (n == "ns")    out.dnsType = 2;
            else if (n == "ptr")   out.dnsType = 12;
        }
        out.dnsServer = d->Str("server", out.dnsServer);
        out.dnsTimeoutMs = d->Int("timeoutMs", out.dnsTimeoutMs);
    }

    // 参数防呆：手改容易写出 0 / 负数（会变成"0 次""0 毫秒"这种诡异行为）
    auto clamp = [](int& v, int lo, int hi) { if (v < lo) v = lo; if (v > hi) v = hi; };
    // ⚠️ ping.count 的下限是 **0**，不是 1：0 = 一直 ping，直到点「取消」为止
    //    （v2.0 王要的连续 ping）。引擎本来就支持（见 netcore 的 DoPing）。
    clamp(out.pingCount, 0, 10000);
    clamp(out.pingIntervalMs, 100, 60000);
    clamp(out.pingTimeoutMs, 100, 60000);
    clamp(out.pingPayload, 8, 1400);
    clamp(out.traceMaxHops, 1, 64);
    clamp(out.traceProbesPerHop, 1, 10);
    clamp(out.traceTimeoutMs, 100, 30000);
    clamp(out.scanTimeoutMs, 100, 30000);
    clamp(out.scanConcurrency, 1, 256);
    clamp(out.dnsTimeoutMs, 200, 60000);      // v2.0：DNS 查询超时
    if (out.recent.size() > (size_t)kRecentKeep) out.recent.resize(kRecentKeep);
    return true;
}

// ---------------------------------------------------------------------------
// 写（临时文件 + 改名，绝不把用户文件写成半截）
// ---------------------------------------------------------------------------
bool Save(const Config& c)
{
    std::string out;
    out += "{\n";
    out += "  // ─────────────────────────────────────────────────────────────\n";
    out += "  // NppDock 网络测试 · 配置文件（改完下一次点测试按钮就生效）\n";
    out += "  //   这个文件由程序维护：每跑一次测试，hosts 里的三处记录会自动更新。\n";
    out += "  //   想改参数直接改这里的数字，保存即可（支持 // 和 /* */ 注释）。\n";
    out += "  // ─────────────────────────────────────────────────────────────\n";
    out += "\n";
    out += "  \"hosts\": {\n";
    out += "    // ① 上次用的 host：下次打开直接填在输入框里\n";
    out += "    \"last\": " + jsonlite::Quote(c.last) + ",\n";
    out += "    // ② 最近用过的：显示在下拉框最上面 5 条（最新在前）\n";
    out += "    \"recent\": [";
    for (size_t i = 0; i < c.recent.size(); ++i) {
        if (i) out += ", ";
        out += jsonlite::Quote(c.recent[i]);
    }
    out += "],\n";
    out += "    // ③ 用得最多的：显示在下拉框最下面 5 条（按使用次数）\n";
    out += "    \"common\": [";
    for (size_t i = 0; i < c.common.size(); ++i) {
        if (i) out += ", ";
        out += "{\"host\": " + jsonlite::Quote(c.common[i].host) +
               ", \"count\": " + std::to_string(c.common[i].count) + "}";
    }
    out += "]\n";
    out += "  },\n";
    out += "\n";
    out += "  // Ping测试：次数 / 间隔(毫秒) / 超时(毫秒) / 负载字节\n";
    out += "  //          count 写 0 = 一直 ping，直到点「取消」为止\n";
    {
        char buf[256];
        sprintf_s(buf, "  \"ping\": { \"count\": %d, \"intervalMs\": %d, \"timeoutMs\": %d, \"payload\": %d },\n",
                  c.pingCount, c.pingIntervalMs, c.pingTimeoutMs, c.pingPayload);
        out += buf;
    }
    out += "  // 路由追踪：最大跳数 / 每跳探测次数 / 超时(毫秒)\n";
    {
        char buf[256];
        sprintf_s(buf, "  \"tracert\": { \"maxHops\": %d, \"probesPerHop\": %d, \"timeoutMs\": %d },\n",
                  c.traceMaxHops, c.traceProbesPerHop, c.traceTimeoutMs);
        out += buf;
    }
    out += "  // 端口扫描：ports 支持 \"22,80,443\" 与 \"8000-8100\" 混写；\n";
    out += "  //          留空 = 用内置的 100 个常用端口\n";
    out += "  \"portscan\": {\n";
    out += "    \"ports\": " + jsonlite::Quote(c.portSpec) + ",\n";
    {
        char buf[256];
        sprintf_s(buf, "    \"timeoutMs\": %d, \"concurrency\": %d\n",
                  c.scanTimeoutMs, c.scanConcurrency);
        out += buf;
    }
    out += "  },\n";
    out += "\n";
    out += "  // DNS查询：type 可写 A / AAAA / CNAME / MX / TXT / NS / PTR；\n";
    out += "  //          server 写 IPv4 地址（如 \"114.114.114.114\"）；留空 = 跟随系统\n";
    out += "  //          timeoutMs = 查询超时（Win8+ 才真正生效；老系统跟随解析器）\n";
    {
        out += "  \"dns\": { \"type\": ";
        out += jsonlite::Quote(std::string(DnsTypeNameForCfg(c.dnsType)));
        out += ", \"server\": " + jsonlite::Quote(c.dnsServer);
        char tb[64];
        sprintf_s(tb, ", \"timeoutMs\": %d }\n", c.dnsTimeoutMs);
        out += tb;
    }
    out += "}\n";

    const std::wstring path = ConfigPath();
    const std::wstring tmp  = path + L".tmp";
    if (!WriteAll(tmp, out)) return false;
    // 先删目标再改名（MoveFileEx 的 REPLACE 在个别杀软在场时会失败）
    ::DeleteFileW(path.c_str());
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 记一次使用
// ---------------------------------------------------------------------------
void RememberUse(Config& c, const std::string& host)
{
    const std::string h = Trim(host);
    if (h.empty()) return;

    c.last = h;

    // recent：去重 + 最新在前 + 截断
    for (size_t i = 0; i < c.recent.size(); ++i) {
        if (_stricmp(c.recent[i].c_str(), h.c_str()) == 0) {
            c.recent.erase(c.recent.begin() + (long)i);
            break;
        }
    }
    c.recent.insert(c.recent.begin(), h);
    if (c.recent.size() > (size_t)kRecentKeep) c.recent.resize(kRecentKeep);

    // common：计数 + 按次数排序
    bool hit = false;
    for (auto& hc : c.common) {
        if (_stricmp(hc.host.c_str(), h.c_str()) == 0) { ++hc.count; hit = true; break; }
    }
    if (!hit) c.common.push_back(HostCount{ h, 1 });
    std::stable_sort(c.common.begin(), c.common.end(),
                     [](const HostCount& a, const HostCount& b) {
                         return a.count > b.count;
                     });
    if (c.common.size() > 40) c.common.resize(40);   // 文件别无限长
}

std::vector<std::string> ComboEntries(const Config& c)
{
    std::vector<std::string> out;
    auto add = [&out](const std::string& h) {
        if (h.empty()) return;
        for (const auto& e : out)
            if (_stricmp(e.c_str(), h.c_str()) == 0) return;   // 两处重复只显示一次
        out.push_back(h);
    };
    // 近期在上（最多 5 条）
    for (size_t i = 0; i < c.recent.size() && i < (size_t)kListShow; ++i)
        add(c.recent[i]);
    // 常用在下（最多 5 条）
    for (size_t i = 0; i < c.common.size() && i < (size_t)kListShow; ++i)
        add(c.common[i].host);
    return out;
}

} // namespace netcfg
