// ============================================================================
// netcore.cpp —— 网络测试引擎实现
// ----------------------------------------------------------------------------
// 七条命令，每条一个函数，共用一套"吐行 + 可中止"的骨架：
//
//   Ping          ICMP 回显（自实现，可控"间隔/超时/次数/负载"）
//   Tracert       逐跳递增 TTL 的 ICMP（复用 Ping 的那套句柄）
//   TcpProbe      TCP 三次握手能不能成（等价于 telnet ip port）
//   Dns           查 A / AAAA / CNAME / MX / TXT / NS / PTR
//   LocalInfo     ipconfig 等价（适配器 / 地址 / 网关 / DNS）
//   Arp           arp -a 等价
//   Connections   netstat -ano 等价（TCP/UDP 连接表 + PID）
//
// ⚠️ 头文件顺序不能动：winsock2.h 必须在 windows.h **之前**。
//    反过来的话 windows.h 会先把老的 winsock.h 拉进来，然后 winsock2.h
//    会因为重定义炸一堆错（而且报的是"winsock.h 与 winsock2.h 冲突"，
//    看起来像是 SDK 装坏了，其实是顺序问题）。
// ============================================================================

#include "netcore.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windns.h>

#include <vector>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace netcore {

namespace {

// ---------------------------------------------------------------------------
// 输出小工具
// ---------------------------------------------------------------------------
void Emit(Sink& sink, const std::wstring& s)
{
    if (sink.emit) sink.emit(sink.user, s);
}

// 引擎每轮调一次：false = 用户要求中止。没有 tick 就当成"永不中止"。
bool Ticking(Sink& sink)
{
    return sink.tick ? sink.tick(sink.user) : true;
}

void EmitF(Sink& sink, const wchar_t* fmt, ...)
{
    wchar_t buf[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    Emit(sink, buf);
}

// 可中断的等待：每片最多 20ms，每片之前问一次 tick。
// 为什么不用一次 Sleep(ms)：那样用户在"无限 ping + 5 秒间隔"里点终止，
// 最长要等 5 秒才有反应，感觉像卡住。
//
// ⚠️⚠️ 但**不能**写成"数够 ms/20 片就走" —— 真机实测过，那样"间隔 1 秒"
//      实际会等 1.5 秒左右。原因很反直觉：Windows 默认的定时器精度是
//      **15.6ms**，Sleep(20) 实际睡 31ms（凑到下一个 tick）。按片数累加的话，
//      每一片都多睡 50%，误差会随间隔线性放大（实测 4 次 x 2000ms 的间隔
//      总耗时 9887ms，而应该是 6000ms 左右）—— 而这个工具的核心卖点恰恰是
//      "频率能选"，等出来的时间不对等于这个参数是假的。
//      正解：先算出**绝对截止时刻**，然后一直睡到过线为止。每片还是 20ms
//      （保证终止的响应还是几十毫秒级），但总时长由挂钟决定，不会累积误差。
//      无符号减法（deadline - now）天然处理 GetTickCount 的 49.7 天回绕。
bool SleepSlice(Sink& sink, int ms)
{
    if (ms <= 0) return Ticking(sink);

    const int kSlice = 20;
    const DWORD deadline = ::GetTickCount() + (DWORD)ms;
    for (;;) {
        if (!Ticking(sink)) return false;
        const DWORD now = ::GetTickCount();
        const DWORD left = deadline - now;       // 已过线时是个很大的数，下面判掉
        if ((int)left <= 0) break;               // 有符号解释才能正确判"过线"
        ::Sleep(left > (DWORD)kSlice ? (DWORD)kSlice : left);
    }
    return Ticking(sink);
}

std::wstring FormatIpv4(DWORD netOrder)
{
    IN_ADDR a{};
    a.S_un.S_addr = netOrder;
    wchar_t b[64]{};
    if (!::InetNtopW(AF_INET, &a, b, _countof(b))) return L"?";
    return b;
}

std::wstring FormatMac(const BYTE* p, ULONG len)
{
    if (!p || len == 0) return L"-";
    std::wstring s;
    wchar_t b[8];
    for (ULONG i = 0; i < len; ++i) {
        swprintf_s(b, L"%02X", p[i]);
        if (i) s += L"-";
        s += b;
    }
    return s;
}

std::wstring FormatMs(int ms)
{
    wchar_t b[32];
    if (ms < 1) swprintf_s(b, L"<1 ms");
    else        swprintf_s(b, L"%d ms", ms);
    return b;
}

// ---------------------------------------------------------------------------
// ICMP 状态码 -> 人话
// ---------------------------------------------------------------------------
const wchar_t* IcmpStatusText(DWORD st)
{
    switch (st) {
    case IP_SUCCESS:                 return L"";
    case IP_BUF_TOO_SMALL:           return L"回复缓冲区太小。";
    case IP_DEST_NET_UNREACHABLE:    return L"目标网络不可达。";
    case IP_DEST_HOST_UNREACHABLE:   return L"目标主机不可达。";
    case IP_DEST_PROT_UNREACHABLE:   return L"目标协议不可达。";
    case IP_DEST_PORT_UNREACHABLE:   return L"目标端口不可达。";
    case IP_NO_RESOURCES:            return L"IP 资源不足。";
    case IP_BAD_OPTION:              return L"错误的 IP 选项。";
    case IP_HW_ERROR:                return L"硬件错误。";
    case IP_PACKET_TOO_BIG:          return L"数据包太大。";
    case IP_REQ_TIMED_OUT:           return L"请求超时。";
    case IP_BAD_REQ:                 return L"错误的请求。";
    case IP_BAD_ROUTE:               return L"错误的路由。";
    case IP_TTL_EXPIRED_TRANSIT:     return L"传输中 TTL 过期。";
    case IP_TTL_EXPIRED_REASSEM:     return L"重组时 TTL 过期。";
    case IP_PARAM_PROBLEM:           return L"IP 参数问题。";
    case IP_SOURCE_QUENCH:           return L"源抑制。";
    case IP_OPTION_TOO_BIG:          return L"IP 选项太大。";
    case IP_BAD_DESTINATION:         return L"错误的目标地址。";
    case IP_GENERAL_FAILURE:         return L"一般性故障。";
    default:                         return L"未知的 ICMP 状态。";
    }
}

// ---------------------------------------------------------------------------
// 目标解析
// ---------------------------------------------------------------------------
// 为什么先按"字面量 IP"试一次再走 DNS：
//   InetPtonW 是纯解析、零开销；而 GetAddrInfoW 碰到 192.168.1.1 这种
//   也会先按名字查一遍（部分环境下会去问 DNS，慢且可能被拦）。
//   先试前者，用户敲 IP 时就是瞬时的。
bool ResolveV4(const std::wstring& host, std::wstring& ipOut, std::wstring& errOut)
{
    if (host.empty()) { errOut = L"没有填目标地址"; return false; }

    IN_ADDR a4{};
    if (::InetPtonW(AF_INET, host.c_str(), &a4) == 1) {
        ipOut = FormatIpv4(a4.S_un.S_addr);
        return true;
    }

    ADDRINFOW hints{};
    hints.ai_family   = AF_INET;          // ICMP 的 IcmpSendEcho 只支持 IPv4
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    const int rc = ::GetAddrInfoW(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        errOut = L"解析不了「" + host + L"」（域名或 IP 写错？）";
        return false;
    }
    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    ipOut = FormatIpv4(sin->sin_addr.S_un.S_addr);
    ::FreeAddrInfoW(res);
    return true;
}

// ---------------------------------------------------------------------------
// ICMP 句柄 + 回复缓冲（Ping 与 Tracert 共用）
// ---------------------------------------------------------------------------
class IcmpSession {
public:
    bool open(Sink& sink)
    {
        _h = ::IcmpCreateFile();
        if (_h == INVALID_HANDLE_VALUE) {
            Emit(sink, L"错误：打不开 ICMP 句柄（可能被安全软件或策略拦住了）");
            _h = nullptr;
            return false;
        }
        return true;
    }
    ~IcmpSession() { if (_h) ::IcmpCloseHandle(_h); }

    HANDLE raw() const { return _h; }

private:
    HANDLE _h = nullptr;
};

// 一块回复缓冲 + 一块负载，打包起来省得每处都算大小
struct IcmpBuffers {
    std::vector<unsigned char> reply;
    std::vector<unsigned char> data;

    void prepare(int payload)
    {
        if (payload < 0) payload = 0;
        // 最小要求是 sizeof(ICMP_ECHO_REPLY) + 负载；这里多加 8 字节 ICMP 头
        // 和一点余量：少了它，某些驱动返回"缓冲太小"。
        reply.assign(sizeof(ICMP_ECHO_REPLY) + 8 + (size_t)payload + 64, 0);
        data.assign((size_t)(payload > 0 ? payload : 1), 0);
        for (int i = 0; i < payload; ++i) {
            data[(size_t)i] = (unsigned char)('a' + (i % 23));   // 可辨认的填充
        }
    }

    DWORD send(HANDLE icmp, DWORD addr, int payload, IP_OPTION_INFORMATION* opt,
               int timeoutMs)
    {
        return ::IcmpSendEcho(icmp, addr, data.data(), (WORD)payload, opt,
                              reply.data(), (DWORD)reply.size(), (DWORD)timeoutMs);
    }

    ICMP_ECHO_REPLY* first() { return reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data()); }
};

// ---------------------------------------------------------------------------
// v2.0：**可中断**的一次 ICMP 探测
// ---------------------------------------------------------------------------
// 为什么非做不可：`IcmpSendEcho` 是同步阻塞的 —— 用户点「取消」之后，得等
//   当前这一轮把 timeout 等满才有反应，而配置允许 timeout 到 60 秒。
//   实测感受就是"点了取消，界面还愣着"。
//
// 做法：把这一发探测丢进**一次性工作线程**（自带 ICMP 句柄与收发缓冲），
//   主线程只用 20ms 分片等它的完成事件，顺便查一次"要不要停"。
//   取消时主线程立刻返回，线程自己在后台把那一轮 I/O 走完。
//
// ⚠️ 生命周期是本条唯一的风险点，规矩写死在这儿：
//   · 主线程在"收到完成事件"之前**绝不**碰 job 里的数据；
//   · 取消时主线程把 job 丢进孤儿池，**从此不再引用它**；
//   · delete 只可能发生在**完成事件已触发之后**（正常路径当场删，孤儿池下次回收）。
//   也就是说释放者一定是"确认过 I/O 已结束"的那一方 —— 不存在竞态。
struct EchoJob {
    HANDLE  icmp = nullptr;
    IcmpBuffers buf;
    HANDLE  done = nullptr;
    DWORD   addr = 0;
    int     payload = 0;
    int     timeoutMs = 0;
    IP_OPTION_INFORMATION opt{};
    DWORD   result = 0;      // IcmpSendEcho 的返回值
    DWORD   err = 0;         // 失败时的 GetLastError

    static DWORD WINAPI ThreadEntry(LPVOID p)
    {
        EchoJob* j = static_cast<EchoJob*>(p);
        j->result = j->buf.send(j->icmp, j->addr, j->payload, &j->opt, j->timeoutMs);
        j->err    = j->result ? 0 : ::GetLastError();
        ::SetEvent(j->done);
        return 0;
    }
};

// 被取消掉、但那一轮 I/O 还没走完的 job。它们只占几 KB 和一个 ICMP 句柄，
// 且最多活过一个 timeout —— 下次开跑时顺手回收。
static std::vector<EchoJob*> g_orphanEchoJobs;

static void ReapOrphanEchoJobs()
{
    for (size_t i = 0; i < g_orphanEchoJobs.size();) {
        EchoJob* j = g_orphanEchoJobs[i];
        if (::WaitForSingleObject(j->done, 0) == WAIT_OBJECT_0) {
            delete j;                       // 完成事件已触发 → 线程不再碰它
            g_orphanEchoJobs.erase(g_orphanEchoJobs.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

struct EchoResult {
    DWORD result = 0;
    DWORD err = 0;
    std::vector<unsigned char> reply;       // 把回复**拷出来**（job 可能被孤儿化）
    ICMP_ECHO_REPLY* first() { return reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data()); }
};

enum class EchoOutcome { Got, Canceled, NoHandle };

// addr 用**网络字节序**（就是 IN_ADDR.S_un.S_addr 那个值）。
static EchoOutcome SendEchoInterruptible(Sink& sink, DWORD addr, int payload, int ttl,
                                         int timeoutMs, EchoResult& out)
{
    ReapOrphanEchoJobs();

    EchoJob* j = new EchoJob();
    j->icmp = ::IcmpCreateFile();
    j->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // 手动重置
    if (j->icmp == INVALID_HANDLE_VALUE || !j->icmp || !j->done) {
        if (j->icmp == INVALID_HANDLE_VALUE) j->icmp = nullptr;
        delete j;
        return EchoOutcome::NoHandle;
    }
    j->addr      = addr;
    j->payload   = payload;
    j->timeoutMs = timeoutMs;
    j->opt.Ttl   = (UCHAR)(ttl > 0 ? ttl : 128);
    j->buf.prepare(payload);

    HANDLE th = ::CreateThread(nullptr, 0, &EchoJob::ThreadEntry, j, 0, nullptr);
    if (!th) {
        // 起不了线程（极罕见）：退回同步发送，行为与 v1.9 一样，至少功能不丢。
        out.result = j->buf.send(j->icmp, addr, payload, &j->opt, timeoutMs);
        out.err    = out.result ? 0 : ::GetLastError();
        out.reply  = j->buf.reply;
        delete j;
        return EchoOutcome::Got;
    }

    bool canceled = false;
    for (;;) {
        if (::WaitForSingleObject(j->done, 20) == WAIT_OBJECT_0) break;
        if (!Ticking(sink)) { canceled = true; break; }
    }
    ::CloseHandle(th);            // 线程句柄永远由主线程关（与 job 的生死无关）

    if (canceled) {
        // ⚠️ 从这里开始**不再引用 j**：交给孤儿池，等它的 I/O 自然结束再回收。
        g_orphanEchoJobs.push_back(j);
        return EchoOutcome::Canceled;
    }

    out.result = j->result;
    out.err    = j->err;
    out.reply  = j->buf.reply;    // 拷出来，之后 job 就没了
    delete j;
    return EchoOutcome::Got;
}


// ===========================================================================
// 1) Ping
// ===========================================================================
bool DoPing(const Params& p, Sink& sink)
{
    std::wstring ip, err;
    if (!ResolveV4(p.target, ip, err)) { Emit(sink, L"错误：" + err); return true; }

    IN_ADDR addr{};
    ::InetPtonW(AF_INET, ip.c_str(), &addr);

    // ⚠️ v2.0：这里不再自己开 IcmpSession / IcmpBuffers —— 每一发探测都由
    //    SendEchoInterruptible 用**私有**的句柄与缓冲在工作线程里跑，
    //    这样"取消"才能立刻生效（见那个函数的说明）。

    EmitF(sink, L"正在 Ping %s 具有 %d 字节的数据:", ip.c_str(), p.payload);
    Emit(sink, L"");

    int sent = 0, recv = 0;
    int minRtt = -1, maxRtt = -1;
    long long sumRtt = 0;

    for (int i = 0;; ++i) {
        if (p.count > 0 && i >= p.count) break;
        if (!Ticking(sink)) {
            Emit(sink, L"（已终止）");
            return false;
        }

        EchoResult res;
        const EchoOutcome oc = SendEchoInterruptible(sink, addr.S_un.S_addr, p.payload,
                                                     128, p.timeoutMs, res);
        if (oc == EchoOutcome::Canceled) { Emit(sink, L"（已终止）"); return false; }
        if (oc == EchoOutcome::NoHandle) {
            Emit(sink, L"错误：打不开 ICMP 句柄（可能被安全软件或策略拦住了）");
            return true;
        }
        ++sent;

        if (res.result > 0) {
            ICMP_ECHO_REPLY* r = res.first();
            if (r->Status == IP_SUCCESS) {
                ++recv;
                const int rtt = (int)r->RoundTripTime;
                if (minRtt < 0 || rtt < minRtt) minRtt = rtt;
                if (rtt > maxRtt) maxRtt = rtt;
                sumRtt += rtt;
                EmitF(sink, L"来自 %s 的回复: 字节=%d 时间=%s TTL=%d",
                      FormatIpv4(r->Address).c_str(), (int)r->DataSize,
                      FormatMs(rtt).c_str(), (int)r->Options.Ttl);
            } else {
                Emit(sink, IcmpStatusText(r->Status));
            }
        } else {
            // 超时时 IcmpSendEcho 返回 0，错误码就是 IP_REQ_TIMED_OUT。
            const DWORD e = res.err;
            Emit(sink, (e == IP_REQ_TIMED_OUT || e == 0) ? L"请求超时。"
                                                         : IcmpStatusText(e));
        }

        // 还有下一轮才等间隔（最后一轮不用白等）
        if (p.count == 0 || i + 1 < p.count) {
            if (!SleepSlice(sink, p.intervalMs)) {
                Emit(sink, L"（已终止）");
                return false;
            }
        }
    }

    Emit(sink, L"");
    EmitF(sink, L"%s 的 Ping 统计信息:", ip.c_str());
    const int lost = sent - recv;
    EmitF(sink, L"    数据包: 已发送 = %d，已接收 = %d，丢失 = %d (%.0f%% 丢失)，",
          sent, recv, lost, sent ? (lost * 100.0 / sent) : 0.0);
    if (recv > 0) {
        Emit(sink, L"往返行程的估计时间(以毫秒为单位):");
        EmitF(sink, L"    最短 = %dms，最长 = %dms，平均 = %dms",
              minRtt, maxRtt, (int)(sumRtt / recv));
    }
    return true;
}

// ===========================================================================
// 2) Tracert
// ===========================================================================
// 原理：把 IP 头的 TTL 依次设成 1、2、3…，第一个把 TTL 减到 0 的路由器
// 会回一个"TTL 过期"的 ICMP，它的源地址就是那一跳。
// TTL 递减、超时回包，全部由协议栈完成 —— 我们只管改 TTL 和读回包。
// ===========================================================================
bool DoTracert(const Params& p, Sink& sink)
{
    std::wstring ip, err;
    if (!ResolveV4(p.target, ip, err)) { Emit(sink, L"错误：" + err); return true; }

    IN_ADDR addr{};
    ::InetPtonW(AF_INET, ip.c_str(), &addr);

    // v2.0：同 DoPing —— 每跳每发的探测都走可中断的工作线程版
    EmitF(sink, L"通过最多 %d 个跃点跟踪到 %s 的路由", p.maxHops, ip.c_str());
    Emit(sink, L"");

    for (int ttl = 1; ttl <= p.maxHops; ++ttl) {
        if (!Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }

        std::wstring row;
        {
            wchar_t head[24];
            swprintf_s(head, L"%4d  ", ttl);
            row = head;
        }

        std::wstring hopAddr;
        bool reached = false;

        for (int k = 0; k < p.probesPerHop; ++k) {
            if (!Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }

            EchoResult res;
            const EchoOutcome oc = SendEchoInterruptible(sink, addr.S_un.S_addr, 32,
                                                         ttl, p.timeoutMs, res);
            if (oc == EchoOutcome::Canceled) { Emit(sink, L"（已终止）"); return false; }
            if (oc == EchoOutcome::NoHandle) {
                Emit(sink, L"错误：打不开 ICMP 句柄（可能被安全软件或策略拦住了）");
                return true;
            }

            if (res.result > 0) {
                ICMP_ECHO_REPLY* r = res.first();
                if (r->Status == IP_SUCCESS || r->Status == IP_TTL_EXPIRED_TRANSIT) {
                    const int rtt = (int)r->RoundTripTime;
                    wchar_t cell[32];
                    if (rtt < 1) swprintf_s(cell, L"%9s", L"<1 ms");
                    else         swprintf_s(cell, L"%7d ms", rtt);
                    row += cell;
                    row += L"  ";
                    hopAddr = FormatIpv4(r->Address);
                    if (r->Status == IP_SUCCESS) reached = true;
                } else {
                    row += L"        *  ";
                }
            } else {
                row += L"        *  ";
            }
        }

        row += hopAddr.empty() ? L"请求超时。" : hopAddr;
        Emit(sink, row);

        if (reached) { Emit(sink, L""); Emit(sink, L"跟踪完成。"); break; }
    }
    return true;
}

// ===========================================================================
// 3) TCP 端口探测
// ===========================================================================
// 等价于 `telnet <ip> <port>`：能连上说明"对端在监听、且中间没被挡"。
// 用非阻塞 connect + select 实现超时 —— 阻塞式 connect 的超时是系统默认的
// 20 秒以上，对一个"探测"工具来说不可接受。
// ===========================================================================
struct SockAddrAny {
    sockaddr_storage ss{};
    int              len = 0;
    int              family = AF_UNSPEC;
};

bool ResolveAny(const std::wstring& host, SockAddrAny& out, std::wstring& errOut)
{
    if (host.empty()) { errOut = L"没有填目标地址"; return false; }

    // 先按字面量试（IPv4 / IPv6 都能识）
    {
        IN_ADDR v4{};
        if (::InetPtonW(AF_INET, host.c_str(), &v4) == 1) {
            auto* sin = reinterpret_cast<sockaddr_in*>(&out.ss);
            sin->sin_family = AF_INET;
            sin->sin_addr   = v4;
            out.len = sizeof(sockaddr_in);
            out.family = AF_INET;
            return true;
        }
        IN6_ADDR v6{};
        if (::InetPtonW(AF_INET6, host.c_str(), &v6) == 1) {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(&out.ss);
            sin6->sin6_family = AF_INET6;
            sin6->sin6_addr   = v6;
            out.len = sizeof(sockaddr_in6);
            out.family = AF_INET6;
            return true;
        }
    }

    ADDRINFOW hints{};
    hints.ai_family   = AF_UNSPEC;        // TCP 探测 v4/v6 都行
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    const int rc = ::GetAddrInfoW(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        errOut = L"解析不了「" + host + L"」（域名或 IP 写错？）";
        return false;
    }
    memcpy(&out.ss, res->ai_addr, res->ai_addrlen);
    out.len    = (int)res->ai_addrlen;
    out.family = res->ai_family;
    ::FreeAddrInfoW(res);
    return true;
}

const wchar_t* WsaErrText(int e)
{
    switch (e) {
    case 0:                    return L"成功";
    case WSAECONNREFUSED:      return L"连接被拒绝（对端没有在这个端口上监听）";
    case WSAETIMEDOUT:         return L"连接超时";
    case WSAEHOSTUNREACH:      return L"目标主机不可达";
    case WSAENETUNREACH:       return L"目标网络不可达";
    case WSAECONNRESET:        return L"连接被重置";
    case WSAEADDRNOTAVAIL:     return L"地址不可用";
    case WSAENETDOWN:          return L"网络子系统不可用";
    case WSAEACCES:            return L"被本地策略禁止（防火墙？）";
    default:                   return L"连接失败";
    }
}

// 探测一次：返回 0=成功，其它=耗时毫秒（失败时也用），okOut 表示结果
int ProbeTcpOnce(const SockAddrAny& sa, int port, int timeoutMs, bool& okOut,
                 int& errOut)
{
    okOut = false;
    errOut = 0;

    SOCKET s = ::socket(sa.family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { errOut = ::WSAGetLastError(); return 0; }

    // 把端口写进去（IPv4/IPv6 的端口偏移一样，都在开头 2 字节之后的位置）
    if (sa.family == AF_INET)       reinterpret_cast<sockaddr_in*>(const_cast<sockaddr_storage*>(&sa.ss))->sin_port  = ::htons((u_short)port);
    else if (sa.family == AF_INET6) reinterpret_cast<sockaddr_in6*>(const_cast<sockaddr_storage*>(&sa.ss))->sin6_port = ::htons((u_short)port);

    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);

    const DWORD t0 = ::GetTickCount();
    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&sa.ss), sa.len);
    bool connected = (rc == 0);

    if (!connected) {
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{};
            tv.tv_sec  = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            const int sr = ::select(0, nullptr, &wf, nullptr, &tv);
            if (sr > 0) {
                int soerr = 0;
                int len = (int)sizeof(soerr);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
                connected = (soerr == 0);
                if (!connected) errOut = soerr;
            } else if (sr == 0) {
                errOut = WSAETIMEDOUT;
            } else {
                errOut = ::WSAGetLastError();
            }
        } else {
            errOut = e;
        }
    }

    const int elapsed = (int)(::GetTickCount() - t0);
    ::closesocket(s);
    okOut = connected;
    return elapsed;
}

bool DoTcpProbe(const Params& p, Sink& sink)
{
    SockAddrAny sa;
    std::wstring err;
    if (!ResolveAny(p.target, sa, err)) { Emit(sink, L"错误：" + err); return true; }

    std::wstring shown = p.target;
    if (sa.family == AF_INET) {
        wchar_t b[64]{}; ::InetNtopW(AF_INET, &reinterpret_cast<sockaddr_in*>(&sa.ss)->sin_addr, b, 64);
        shown = b;
    } else if (sa.family == AF_INET6) {
        wchar_t b[80]{}; ::InetNtopW(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&sa.ss)->sin6_addr, b, 80);
        shown = b;
    }

    EmitF(sink, L"正在探测 %s:%d …（超时 %d ms）", shown.c_str(), p.port, p.timeoutMs);
    Emit(sink, L"");

    int okN = 0, failN = 0;
    long long sumOk = 0;

    for (int i = 0; i < p.count; ++i) {
        if (!Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }

        bool ok = false;
        int e = 0;
        const int ms = ProbeTcpOnce(sa, p.port, p.timeoutMs, ok, e);

        if (ok) {
            ++okN;
            sumOk += ms;
            EmitF(sink, L"[%d/%d] 连接成功    耗时 %s", i + 1, p.count, FormatMs(ms).c_str());
        } else {
            ++failN;
            EmitF(sink, L"[%d/%d] %s    耗时 %s", i + 1, p.count,
                  WsaErrText(e), FormatMs(ms).c_str());
        }

        if (i + 1 < p.count) {
            if (!SleepSlice(sink, 300)) { Emit(sink, L"（已终止）"); return false; }
        }
    }

    Emit(sink, L"");
    EmitF(sink, L"统计：成功 %d / 失败 %d / 共 %d", okN, failN, p.count);
    if (okN > 0) {
        EmitF(sink, L"      成功时的平均耗时 %d ms", (int)(sumOk / okN));
    }
    return true;
}

// ===========================================================================
// 3b) TCP 端口扫描（v1.2：界面上「端口扫描」按钮用它）
// ===========================================================================
// 与 DoTcpProbe（单端口、反复探）的区别：
//   · 这里是"一次扫一批端口，只报**开放的**"——扫描结果里满屏"关闭"没有意义；
//   · 用**分批非阻塞**的做法而不是"一个端口一个端口串行等"：
//     100 个端口按 1 秒超时串行要 100 秒，分批并发（默认 32）压到几秒。
//     也没有用线程：单线程 + select 就够，还省掉一堆同步问题。
bool DoPortScan(const Params& p, Sink& sink)
{
    if (p.ports.empty()) {
        Emit(sink, L"错误：没有可扫描的端口（配置文件里的 portscan.ports 是空的？）");
        return true;
    }

    SockAddrAny sa;
    std::wstring err;
    if (!ResolveAny(p.target, sa, err)) { Emit(sink, L"错误：" + err); return true; }

    if (sa.family != AF_INET) {
        // 解析出来的第一个地址是 IPv6 时也照扫（ProbeTcpOnce 支持两种族），
        // 但要在输出里说清楚，免得用户以为扫的是 IPv4。
        Emit(sink, L"提示：该主机解析到 IPv6 地址，按 IPv6 探测");
    }

    std::wstring shown = p.target;
    {
        wchar_t b[80]{};
        if (sa.family == AF_INET)
            ::InetNtopW(AF_INET, &reinterpret_cast<sockaddr_in*>(&sa.ss)->sin_addr, b, 64);
        else
            ::InetNtopW(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&sa.ss)->sin6_addr, b, 80);
        if (b[0]) shown = b;
    }

    const int conc = (p.concurrency > 0 && p.concurrency < 256) ? p.concurrency : 32;
    const int total = (int)p.ports.size();
    EmitF(sink, L"扫描 %s 的 TCP 端口：共 %d 个（超时 %d ms，并发 %d）",
          shown.c_str(), total, p.timeoutMs, conc);
    Emit(sink, L"------------------------------------------------------------");

    std::vector<int> openPorts;
    int done = 0;

    for (size_t base = 0; base < p.ports.size(); base += (size_t)conc) {
        if (!Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }

        const size_t n = (std::min)((size_t)conc, p.ports.size() - base);
        std::vector<SOCKET> socks(n, INVALID_SOCKET);

        // 一轮：全部发起连接，然后一起 select 等结果
        for (size_t k = 0; k < n; ++k) {
            SockAddrAny one = sa;
            const int port = p.ports[base + k];
            if (one.family == AF_INET)
                reinterpret_cast<sockaddr_in*>(&one.ss)->sin_port = ::htons((u_short)port);
            else
                reinterpret_cast<sockaddr_in6*>(&one.ss)->sin6_port = ::htons((u_short)port);

            SOCKET s = ::socket(one.family, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            u_long nb = 1;
            ::ioctlsocket(s, FIONBIO, &nb);
            ::connect(s, reinterpret_cast<const sockaddr*>(&one.ss), one.len);
            socks[k] = s;   // 连上/没连上都先留着，等 select 判
        }

        // 等这一轮里的所有连接给出结果。
        // ⚠️ 等的时候也要能中止：select 的超时切成最多 100ms 一片，
        //    每片之前问一次 tick —— 否则"扫大网段"时点取消要等满一个超时。
        const DWORD t0 = ::GetTickCount();
        std::vector<char> settled(n, 0);
        const int sliceMs = 100;

        for (;;) {
            bool allSettled = true;
            fd_set wf, ef;
            FD_ZERO(&wf); FD_ZERO(&ef);
            for (size_t k = 0; k < n; ++k) {
                if (socks[k] == INVALID_SOCKET || settled[k]) continue;
                allSettled = false;
                FD_SET(socks[k], &wf);
                FD_SET(socks[k], &ef);
            }
            if (allSettled) break;

            timeval tv{};
            tv.tv_sec  = 0;
            tv.tv_usec = sliceMs * 1000;
            const int sr = ::select(0, nullptr, &wf, &ef, &tv);

            if (sr > 0) {
                for (size_t k = 0; k < n; ++k) {
                    if (socks[k] == INVALID_SOCKET || settled[k]) continue;
                    if (!FD_ISSET(socks[k], &wf) && !FD_ISSET(socks[k], &ef)) continue;
                    int soerr = 0;
                    int len = (int)sizeof(soerr);
                    ::getsockopt(socks[k], SOL_SOCKET, SO_ERROR,
                                 reinterpret_cast<char*>(&soerr), &len);
                    if (soerr == 0) {
                        openPorts.push_back(p.ports[base + k]);
                        EmitF(sink, L"[开放] %d", p.ports[base + k]);
                    }
                    settled[k] = 1;
                }
            }

            // 整体超时到了：没 settled 的一律算超时（不报，扫描不报"关闭/超时"）
            if (::GetTickCount() - t0 >= (DWORD)p.timeoutMs) {
                for (size_t k = 0; k < n; ++k) settled[k] = 1;
            }

            if (!Ticking(sink)) {
                for (SOCKET s : socks) if (s != INVALID_SOCKET) ::closesocket(s);
                Emit(sink, L"（已终止）");
                return false;
            }
        }

        for (SOCKET s : socks) if (s != INVALID_SOCKET) ::closesocket(s);
        done += (int)n;
        EmitF(sink, L"… 已扫 %d/%d", done, total);
    }

    std::sort(openPorts.begin(), openPorts.end());
    openPorts.erase(std::unique(openPorts.begin(), openPorts.end()), openPorts.end());

    Emit(sink, L"");
    if (openPorts.empty()) {
        EmitF(sink, L"扫描完成：%d 个端口全部未开放（或被防火墙挡了）", total);
    } else {
        EmitF(sink, L"扫描完成：%d 个端口里开放 %d 个", total, (int)openPorts.size());
        std::wstring list;
        for (size_t i = 0; i < openPorts.size(); ++i) {
            if (i) list += L", ";
            list += std::to_wstring(openPorts[i]);
        }
        Emit(sink, L"开放的端口：" + list);
    }
    return true;
}

// ===========================================================================
// 4) DNS 查询
// ===========================================================================
// ⚠️ DnsTypeName / DnsTypeCount / DnsTypeValueAt / DnsTypeLabelAt 的实现
//    刻意**不放在这个匿名命名空间里**（在文件末尾的"对外接口"段落），
//    因为界面（下拉框）也要用它。放在这里的话，同一个名字会同时存在于
//    netcore::DnsTypeName 与匿名命名空间里，编译期直接报"二义性"。
//
//    而 DoDns 里那句 DnsTypeName(...) 依然能编过 —— 因为头文件里已经声明了
//    netcore::DnsTypeName，匿名命名空间内的查找会向外层找到它。
const wchar_t* DnsErrText(DNS_STATUS st)
{
    switch (st) {
    case DNS_ERROR_RCODE_NO_ERROR:        return L"成功";
    case DNS_ERROR_RCODE_FORMAT_ERROR:    return L"格式错误（域名写法不对？）";
    case DNS_ERROR_RCODE_SERVER_FAILURE:  return L"DNS 服务器故障";
    case DNS_ERROR_RCODE_NAME_ERROR:      return L"域名不存在（NXDOMAIN）";
    case DNS_ERROR_RCODE_NOT_IMPLEMENTED: return L"服务器不支持这类查询";
    case DNS_ERROR_RCODE_REFUSED:         return L"服务器拒绝了这个查询";
    case DNS_INFO_NO_RECORDS:             return L"没有这个类型的记录";
    case DNS_ERROR_RCODE_NXRRSET:         return L"该名字存在，但没有这种类型的记录";
    default: {
        // 静态缓冲区：只在"立刻拼进 wstring"的用法里安全，够用了
        static thread_local wchar_t buf[96];
        swprintf_s(buf, L"查询失败（代码 %ld）", (long)st);
        return buf;
    }
    }
}

// ---------------------------------------------------------------------------
// v2.0：DNS 查询改成**可设超时、可取消**
// ---------------------------------------------------------------------------
// 老写法是裸的同步 DnsQuery_W：没有超时参数（超时由解析器自己定），而且发出去
// 就只能干等 —— 用户点「取消」最长要愣好几秒（那个"5000"甚至是 nettest.cpp
// 里硬写的假参数，配置里根本没有这一项）。
//
// 这里**不用** DnsQueryEx：本机 SDK（10.0.26100）的 DNS_QUERY_REQUEST 里
// **没有 QueryTimeout 这个成员**，靠它设超时是做不到的。
// 改成和 ICMP 那边同一套办法 —— 把同步调用丢进一次性工作线程，主线程用
// 20ms 分片等它的完成事件，同时盯两件事：**用户要不要停**、**是不是超时**。
// 于是"超时"和"取消"都变成了我们自己的判断，跟系统版本无关。
//
// ⚠️ 生命周期规矩同 ICMP：超时/取消之后主线程**不再引用** job，交给孤儿池；
//    delete 只发生在**完成事件已触发**之后（正常路径当场删，孤儿池下次回收）。
struct DnsJob {
    HANDLE       done = nullptr;
    std::wstring name;                    // 工作线程要自己拿一份（主线程可能先走）
    WORD         type = 0;
    DWORD        opts = 0;
    IP4_ARRAY    srv{};
    bool         hasSrv = false;
    DNS_STATUS   status = 0;
    PDNS_RECORDW rec = nullptr;

    static DWORD WINAPI ThreadEntry(LPVOID p)
    {
        DnsJob* j = static_cast<DnsJob*>(p);
        IP4_ARRAY* srvList = j->hasSrv ? &j->srv : nullptr;
        j->status = ::DnsQuery_W(j->name.c_str(), j->type, j->opts, srvList,
                                 &j->rec, nullptr);
        ::SetEvent(j->done);
        return 0;
    }
};

static std::vector<DnsJob*> g_orphanDnsJobs;

static void ReapOrphanDnsJobs()
{
    for (size_t i = 0; i < g_orphanDnsJobs.size();) {
        DnsJob* j = g_orphanDnsJobs[i];
        if (::WaitForSingleObject(j->done, 0) == WAIT_OBJECT_0) {
            if (j->rec) ::DnsRecordListFree(j->rec, DnsFreeRecordList);
            ::CloseHandle(j->done);
            delete j;
            g_orphanDnsJobs.erase(g_orphanDnsJobs.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

bool DoDns(const Params& p, Sink& sink)
{
    if (p.target.empty()) { Emit(sink, L"错误：没有填目标地址"); return true; }

    const WORD type = (WORD)p.dnsType;

    IP4_ARRAY   srv{};
    IP4_ARRAY*  pSrv = nullptr;
    DWORD       opts = 0;                 // 0 = DNS_QUERY_STANDARD
    std::wstring srvName = L"系统默认";
    if (p.dnsServer != 0) {
        srv.AddrCount    = 1;
        srv.AddrArray[0] = (IP4_ADDRESS)p.dnsServer;
        pSrv  = &srv;
        // 只把"额外指定了服务器"这件事交给 DNS_QUERY_BYPASS_CACHE 去表达 ——
        // 这个组合是 MSDN 上 DnsQuery_W 唯一支持"用指定服务器查"的写法。
        opts  = DNS_QUERY_BYPASS_CACHE;
        srvName = FormatIpv4((DWORD)p.dnsServer);
    }

    EmitF(sink, L"查询 %s 的 %s 记录（服务器：%s）",
          p.target.c_str(), DnsTypeName(p.dnsType), srvName.c_str());
    Emit(sink, L"");
    Emit(sink, L"----------------------------------------");

    PDNS_RECORDW rec = nullptr;
    DNS_STATUS   qstatus = 0;
    const DWORD  timeout = (DWORD)(p.timeoutMs > 0 ? p.timeoutMs : 5000);

    ReapOrphanDnsJobs();

    DnsJob* job = new DnsJob();
    job->done   = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    job->name   = p.target;
    job->type   = type;
    job->opts   = opts;
    job->hasSrv = (p.dnsServer != 0);
    if (job->hasSrv) {
        job->srv.AddrCount    = 1;
        job->srv.AddrArray[0] = (IP4_ADDRESS)p.dnsServer;
    }
    if (!job->done) { delete job; Emit(sink, L"错误：建不了事件对象"); return true; }

    HANDLE th = ::CreateThread(nullptr, 0, &DnsJob::ThreadEntry, job, 0, nullptr);
    if (!th) {
        // 起不了线程（极罕见）：退回同步查询，行为与 v1.9 一样，功能不丢。
        qstatus = ::DnsQuery_W(p.target.c_str(), type, opts, pSrv, &rec, nullptr);
        ::CloseHandle(job->done);
        delete job;
    } else {
        bool canceled = false, timedout = false;
        const DWORD t0 = ::GetTickCount();
        for (;;) {
            // ⚠️ 顺序很重要：**先看超时，再决定等多久**。
            //    写成"先等 20ms、再判超时"的话，一个 1ms 的超时也会先白等 20ms，
            //    而且只要查询在那 20ms 里回来了就会被当成"没超时"（实测踩到：
            //    --timeout 1 依然能拿到结果）。切片取"剩余时间"就精确了。
            const DWORD elapsed = (DWORD)(::GetTickCount() - t0);
            if (elapsed >= timeout) { timedout = true; break; }
            DWORD slice = timeout - elapsed;
            if (slice > 20) slice = 20;
            if (::WaitForSingleObject(job->done, slice) == WAIT_OBJECT_0) break;
            if (!Ticking(sink)) { canceled = true; break; }
        }
        ::CloseHandle(th);            // 线程句柄永远由主线程关

        if (canceled || timedout) {
            // ⚠️ 从这里开始**不再引用 job**：交给孤儿池，等查询自然收尾再回收。
            g_orphanDnsJobs.push_back(job);
            if (canceled) { Emit(sink, L"（已终止）"); return false; }
            EmitF(sink, L"查询超时（超过 %u 毫秒没有回应）。", (unsigned)timeout);
            return true;
        }

        rec      = job->rec;
        qstatus  = job->status;
        job->rec = nullptr;           // 所有权已转移，别让回收逻辑再释放一次
        ::CloseHandle(job->done);
        delete job;
    }

    if (qstatus != 0) {
        EmitF(sink, L"查询失败：%s", DnsErrText(qstatus));
        return true;
    }

    int n = 0;
    for (PDNS_RECORDW r = rec; r; r = r->pNext) {
        if (!Ticking(sink)) { ::DnsRecordListFree(rec, DnsFreeRecordList); Emit(sink, L"（已终止）"); return false; }

        // ⚠️ 一个查询的返回链里可能混着不同 wType（CNAME 链就会），
        //    所以要按**每条记录自己的类型**解释它的 Data，不能按请求类型硬套 ——
        //    硬套会读到错的联合体成员，出来一串垃圾地址（而且不会报错）。
        switch (r->wType) {
        case DNS_TYPE_A:
            EmitF(sink, L"A      %s", FormatIpv4(r->Data.A.IpAddress).c_str());
            ++n;
            break;
        case DNS_TYPE_AAAA: {
            wchar_t b[80]{};
            ::InetNtopW(AF_INET6, r->Data.AAAA.Ip6Address.IP6Byte, b, 80);
            EmitF(sink, L"AAAA   %s", b);
            ++n;
            break;
        }
        case DNS_TYPE_CNAME:
        case DNS_TYPE_NS:
        case DNS_TYPE_PTR:
            if (r->Data.PTR.pNameHost) {
                EmitF(sink, L"%-6s %s", DnsTypeName((int)r->wType), r->Data.PTR.pNameHost);
                ++n;
            }
            break;
        case DNS_TYPE_MX:
            if (r->Data.MX.pNameExchange) {
                EmitF(sink, L"MX     优先级 %u  %s",
                      (unsigned)r->Data.MX.wPreference, r->Data.MX.pNameExchange);
                ++n;
            }
            break;
        case DNS_TYPE_TEXT:
            // TXT 记录的字符串是**数组**（一条记录可以由多段拼成），
            // 只读 pStringArray[0] 会漏掉后半截。
            if (r->Data.TXT.dwStringCount > 0) {
                std::wstring txt;
                for (DWORD i = 0; i < r->Data.TXT.dwStringCount; ++i) {
                    if (r->Data.TXT.pStringArray[i]) txt += r->Data.TXT.pStringArray[i];
                }
                EmitF(sink, L"TXT    %s", txt.c_str());
                ++n;
            }
            break;
        default:
            // 字段名是 wDataLength（WORD），不是 dwLength —— 后者是 IP Helper
            // 那几张表（MIB_*ROW）的写法，DNS_RECORDW 上不存在。
            EmitF(sink, L"类型 %u  %u 字节的原始数据", (unsigned)r->wType,
                  (unsigned)r->wDataLength);
            ++n;
            break;
        }
    }

    ::DnsRecordListFree(rec, DnsFreeRecordList);

    Emit(sink, L"----------------------------------------");
    EmitF(sink, L"共 %d 条记录。", n);
    return true;
}

// ===========================================================================
// 5) 本机网络信息（ipconfig 等价）
// ===========================================================================
const wchar_t* OperStatusText(IF_OPER_STATUS st)
{
    switch (st) {
    case IfOperStatusUp:             return L"已连接";
    case IfOperStatusDown:           return L"已断开";
    case IfOperStatusTesting:        return L"测试中";
    case IfOperStatusUnknown:        return L"未知";
    case IfOperStatusDormant:        return L"休眠";
    case IfOperStatusNotPresent:     return L"未插入";
    case IfOperStatusLowerLayerDown: return L"下层断开";
    default:                         return L"?";
    }
}

bool DoLocalInfo(const Params& p, Sink& sink)
{
    const ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS
                      | GAA_FLAG_SKIP_ANYCAST  | GAA_FLAG_SKIP_MULTICAST;

    ULONG size = 16 * 1024;   // 先给一块初始大小；不够会被改大
    std::vector<unsigned char> buf(size);
    ULONG rc = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                      &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                    &size);
    }
    if (rc != NO_ERROR) {
        Emit(sink, L"错误：读不到本机适配器信息");
        return true;
    }

    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    int shown = 0;

    for (auto* a = aa; a; a = a->Next) {
        if (!Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }

        // 环回之类的虚拟网卡默认也列出来 —— 它们对排查"为什么连不上"很有用
        // （比如 VPN 装出来的那块），所以不过滤，只是标注一下。
        EmitF(sink, L"适配器 %s  [%s]  #%lu",
              a->FriendlyName ? a->FriendlyName : L"(无名)",
              OperStatusText(a->OperStatus),
              (unsigned long)a->IfIndex);
        if (a->Description) EmitF(sink, L"  描述    : %s", a->Description);
        EmitF(sink, L"  物理地址: %s", FormatMac(a->PhysicalAddress,
                                                 a->PhysicalAddressLength).c_str());

        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            const int fam = ua->Address.lpSockaddr
                          ? ua->Address.lpSockaddr->sa_family : AF_UNSPEC;
            if (p.view == 1 && fam != AF_INET)  continue;
            if (p.view == 2 && fam != AF_INET6) continue;
            if (p.view == 0 && fam != AF_INET && fam != AF_INET6) continue;

            wchar_t host[128]{};
            if (::GetNameInfoW(ua->Address.lpSockaddr, ua->Address.iSockaddrLength,
                               host, _countof(host), nullptr, 0,
                               NI_NUMERICHOST) != 0) {
                continue;
            }
            const wchar_t* tag = (fam == AF_INET) ? L"IPv4    " : L"IPv6    ";
            EmitF(sink, L"  %s: %s/%u", tag, host,
                  (unsigned)ua->OnLinkPrefixLength);
        }

        // 网关同样按 view 过滤（理由同下方的 DNS）
        for (auto* gw = a->FirstGatewayAddress; gw; gw = gw->Next) {
            const int fam = gw->Address.lpSockaddr
                          ? gw->Address.lpSockaddr->sa_family : AF_UNSPEC;
            if (p.view == 1 && fam != AF_INET)  continue;
            if (p.view == 2 && fam != AF_INET6) continue;
            wchar_t host[128]{};
            if (::GetNameInfoW(gw->Address.lpSockaddr, gw->Address.iSockaddrLength,
                               host, _countof(host), nullptr, 0, NI_NUMERICHOST) == 0) {
                EmitF(sink, L"  网关    : %s", host);
            }
        }

        // DNS 服务器也按 view 过滤：选了「仅 IPv4」却列出一串 IPv6 DNS
        // 是自相矛盾的（真机踩到：`--view 1` 的输出里还挂着 fec0::1%1）。
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            const int fam = d->Address.lpSockaddr
                          ? d->Address.lpSockaddr->sa_family : AF_UNSPEC;
            if (p.view == 1 && fam != AF_INET)  continue;
            if (p.view == 2 && fam != AF_INET6) continue;
            wchar_t host[128]{};
            if (::GetNameInfoW(d->Address.lpSockaddr, d->Address.iSockaddrLength,
                               host, _countof(host), nullptr, 0, NI_NUMERICHOST) == 0) {
                EmitF(sink, L"  DNS     : %s", host);
            }
        }

        Emit(sink, L"");
        ++shown;
    }

    EmitF(sink, L"共 %d 个网络适配器。", shown);
    return true;
}

// ===========================================================================
// 6) ARP 表
// ===========================================================================
const wchar_t* ArpTypeText(DWORD t)
{
    switch (t) {
    case MIB_IPNET_TYPE_OTHER:   return L"其他";
    case MIB_IPNET_TYPE_INVALID: return L"无效";
    case MIB_IPNET_TYPE_DYNAMIC: return L"动态";
    case MIB_IPNET_TYPE_STATIC:  return L"静态";
    default:                     return L"?";
    }
}

struct ArpRow {
    DWORD ifIndex;
    DWORD ip;
    std::wstring mac;
    DWORD type;
};

bool DoArp(const Params& p, Sink& sink)
{
    ULONG size = 0;
    DWORD rc = ::GetIpNetTable(nullptr, &size, TRUE);
    if (rc != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        Emit(sink, L"错误：读不到 ARP 表");
        return true;
    }

    std::vector<unsigned char> buf(size);
    auto* t = reinterpret_cast<PMIB_IPNETTABLE>(buf.data());
    rc = ::GetIpNetTable(t, &size, TRUE);
    if (rc != NO_ERROR) {
        Emit(sink, L"错误：读不到 ARP 表（可能需要管理员权限）");
        return true;
    }

    std::vector<ArpRow> rows;
    rows.reserve(t->dwNumEntries);
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_IPNETROW& r = t->table[i];
        ArpRow row;
        row.ifIndex = r.dwIndex;
        row.ip      = r.dwAddr;
        row.mac     = FormatMac(r.bPhysAddr, r.dwPhysAddrLen);
        row.type    = r.dwType;
        rows.push_back(std::move(row));
    }

    // ⚠️ 排序要按 ntohl() 之后的自然数值，不能直接比 dwAddr。
    //    dwAddr 是**网络序**的 DWORD，而 DWORD 比较是在主机序上做的：
    //    x86 上等于"从最后一个字节开始比"，排出来是
    //    192.168.57.1 → 169.254.x.x → 192.168.57.20 这种颠倒的顺序
    //    （真机踩到，看着就像"排序坏了"）。ntohl 之后第一个八位组才是最高位。
    if (p.sortBy == 0) {
        std::sort(rows.begin(), rows.end(), [](const ArpRow& a, const ArpRow& b) {
            return ::ntohl(a.ip) < ::ntohl(b.ip);
        });
    } else {
        std::sort(rows.begin(), rows.end(), [](const ArpRow& a, const ArpRow& b) {
            if (a.ifIndex != b.ifIndex) return a.ifIndex < b.ifIndex;
            return ::ntohl(a.ip) < ::ntohl(b.ip);
        });
    }

    EmitF(sink, L"ARP 表：共 %u 项", (unsigned)rows.size());
    Emit(sink, L"");
    Emit(sink, L"  接口      IP 地址           物理地址             类型");
    Emit(sink, L"  ------  ----------------  -------------------  --------");
    for (const auto& r : rows) {
        if (!Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }
        EmitF(sink, L"  %6lu  %-16s  %-19s  %s",
              (unsigned long)r.ifIndex,
              FormatIpv4(r.ip).c_str(),
              r.mac.c_str(),
              ArpTypeText(r.type));
    }
    return true;
}

// ===========================================================================
// 7) 连接表（netstat -ano 等价）
// ===========================================================================
const wchar_t* TcpStateText(DWORD st)
{
    switch (st) {
    case MIB_TCP_STATE_CLOSED:      return L"CLOSED";
    case MIB_TCP_STATE_LISTEN:      return L"LISTENING";
    case MIB_TCP_STATE_SYN_SENT:    return L"SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD:    return L"SYN_RCVD";
    case MIB_TCP_STATE_ESTAB:       return L"ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1:   return L"FIN_WAIT_1";
    case MIB_TCP_STATE_FIN_WAIT2:   return L"FIN_WAIT_2";
    case MIB_TCP_STATE_CLOSE_WAIT:  return L"CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING:     return L"CLOSING";
    case MIB_TCP_STATE_LAST_ACK:    return L"LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT:   return L"TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB:  return L"DELETE_TCB";
    default:                        return L"?";
    }
}

std::wstring AddrPort(DWORD netAddr, DWORD netPort)
{
    // 端口在低 16 位、而且仍然是**网络序**，所以要 ntohs ——
    // 直接打印会得到 20480 这种"看起来像端口但完全对不上"的数。
    wchar_t b[80];
    swprintf_s(b, L"%s:%u", FormatIpv4(netAddr).c_str(),
               (unsigned)::ntohs((u_short)(netPort & 0xFFFF)));
    return b;
}

bool DoConnections(const Params& p, Sink& sink)
{
    if (p.proto == 0) {
        ULONG size = 0;
        DWORD rc = ::GetExtendedTcpTable(nullptr, &size, TRUE, AF_INET,
                                         TCP_TABLE_OWNER_PID_ALL, 0);
        if (rc != ERROR_INSUFFICIENT_BUFFER || size == 0) {
            Emit(sink, L"错误：读不到 TCP 连接表");
            return true;
        }
        std::vector<unsigned char> buf(size);
        auto* t = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buf.data());
        rc = ::GetExtendedTcpTable(t, &size, TRUE, AF_INET,
                                   TCP_TABLE_OWNER_PID_ALL, 0);
        if (rc != NO_ERROR) {
            Emit(sink, L"错误：读不到 TCP 连接表（可能需要管理员权限）");
            return true;
        }

        EmitF(sink, L"TCP 连接表（IPv4）：共 %u 项", (unsigned)t->dwNumEntries);
        Emit(sink, L"");
        Emit(sink, L"  本地地址:端口            远端地址:端口            状态          PID");
        Emit(sink, L"  ----------------------  ----------------------  ------------  ------");

        int shown = 0;
        for (DWORD i = 0; i < t->dwNumEntries; ++i) {
            const MIB_TCPROW_OWNER_PID& r = t->table[i];
            if (p.stateFilter == 1 && r.dwState != MIB_TCP_STATE_LISTEN) continue;
            if (p.stateFilter == 2 && r.dwState != MIB_TCP_STATE_ESTAB)  continue;
            EmitF(sink, L"  %-22s  %-22s  %-12s  %6lu",
                  AddrPort(r.dwLocalAddr, r.dwLocalPort).c_str(),
                  AddrPort(r.dwRemoteAddr, r.dwRemotePort).c_str(),
                  TcpStateText(r.dwState),
                  (unsigned long)r.dwOwningPid);
            ++shown;
            if ((shown % 200) == 0 && !Ticking(sink)) {
                Emit(sink, L"（已终止）");
                return false;
            }
        }
        Emit(sink, L"");
        EmitF(sink, L"显示 %d 项。", shown);
        return true;
    }

    // UDP
    ULONG size = 0;
    DWORD rc = ::GetExtendedUdpTable(nullptr, &size, TRUE, AF_INET,
                                     UDP_TABLE_OWNER_PID, 0);
    if (rc != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        Emit(sink, L"错误：读不到 UDP 连接表");
        return true;
    }
    std::vector<unsigned char> buf(size);
    auto* t = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(buf.data());
    rc = ::GetExtendedUdpTable(t, &size, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (rc != NO_ERROR) {
        Emit(sink, L"错误：读不到 UDP 连接表（可能需要管理员权限）");
        return true;
    }

    EmitF(sink, L"UDP 端点表（IPv4）：共 %u 项", (unsigned)t->dwNumEntries);
    Emit(sink, L"");
    Emit(sink, L"  本地地址:端口            远端                 PID");
    Emit(sink, L"  ----------------------  ------------  ------");
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_PID& r = t->table[i];
        EmitF(sink, L"  %-22s  %-12s  %6lu",
              AddrPort(r.dwLocalAddr, r.dwLocalPort).c_str(),
              L"*:*",
              (unsigned long)r.dwOwningPid);
        if ((i % 200) == 0 && !Ticking(sink)) { Emit(sink, L"（已终止）"); return false; }
    }
    Emit(sink, L"");
    EmitF(sink, L"显示 %u 项。", (unsigned)t->dwNumEntries);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
// v2.0：IPv4 字面量 -> 网络字节序的 32 位值（给 Params::dnsServer 用）。
// ⚠️ 必须放在匿名 namespace **外面** —— 里面那个是内部链接，链接器找不到。
bool ParseIpv4Net(const std::wstring& s, unsigned int& netOrderOut)
{
    if (s.empty()) return false;
    IN_ADDR a{};
    if (::InetPtonW(AF_INET, s.c_str(), &a) != 1) return false;
    netOrderOut = (unsigned int)a.S_un.S_addr;
    return true;
}

bool GlobalInit()
{
    WSADATA wsa{};
    return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void GlobalShutdown()
{
    ::WSACleanup();
}

bool Run(Kind kind, const Params& p, Sink& sink)
{
    switch (kind) {
    case Kind::Ping:        return DoPing(p, sink);
    case Kind::Tracert:     return DoTracert(p, sink);
    case Kind::TcpProbe:    return DoTcpProbe(p, sink);
    case Kind::PortScan:    return DoPortScan(p, sink);
    case Kind::Dns:         return DoDns(p, sink);
    case Kind::LocalInfo:   return DoLocalInfo(p, sink);
    case Kind::Arp:         return DoArp(p, sink);
    case Kind::Connections: return DoConnections(p, sink);
    }
    Emit(sink, L"错误：未知的测试命令");
    return true;
}

// ---------------------------------------------------------------------------
// 命令表（界面与自检共用一份，避免"界面上有、自检里没有"这种漂移）
// ---------------------------------------------------------------------------
// ⚠️ 想加一条新命令，只需要动这个数组 + 上面的 Run() 分派 + CommandName/Key/
//    NeedsTarget 三个 switch，界面**一行都不用改**（下拉框是遍历这张表建的）。
const Kind kAllCommands[] = {
    Kind::Ping, Kind::Tracert, Kind::PortScan, Kind::Dns,
    Kind::TcpProbe, Kind::LocalInfo, Kind::Arp, Kind::Connections,
};
const int kCommandCount = (int)(sizeof(kAllCommands) / sizeof(kAllCommands[0]));

Kind CommandAt(int i)
{
    if (i < 0 || i >= kCommandCount) return Kind::Ping;
    return kAllCommands[i];
}

int CommandCount()
{
    return kCommandCount;
}

const wchar_t* CommandName(Kind k)
{
    switch (k) {
    case Kind::Ping:        return L"Ping 连通性";
    case Kind::Tracert:     return L"路由追踪（tracert）";
    case Kind::PortScan:    return L"端口扫描";
    case Kind::TcpProbe:    return L"TCP 端口探测";
    case Kind::Dns:         return L"DNS 查询";
    case Kind::LocalInfo:   return L"本机网络信息";
    case Kind::Arp:         return L"ARP 表";
    case Kind::Connections: return L"连接表（netstat）";
    }
    return L"?";
}

const wchar_t* CommandKey(Kind k)
{
    switch (k) {
    case Kind::Ping:        return L"ping";
    case Kind::Tracert:     return L"tracert";
    case Kind::PortScan:    return L"portscan";
    case Kind::TcpProbe:    return L"tcp";
    case Kind::Dns:         return L"dns";
    case Kind::LocalInfo:   return L"local";
    case Kind::Arp:         return L"arp";
    case Kind::Connections: return L"conn";
    }
    return L"?";
}

bool NeedsTarget(Kind k)
{
    switch (k) {
    case Kind::Ping:
    case Kind::Tracert:
    case Kind::TcpProbe:
    case Kind::PortScan:
    case Kind::Dns:
        return true;
    default:
        return false;   // 本机信息 / ARP 表 / 连接表都是"看自己"，不用目标
    }
}

// ---------------------------------------------------------------------------
// DNS 记录类型表
// ---------------------------------------------------------------------------
// 为什么把"值"和"显示名"分两张表：
//   下拉框要显示 "MX（邮件交换）" 这种带说明的，而引擎输出里只该出现
//   windbg/netstat 那一套短名 "MX"。混用会让输出列对不齐。
struct DnsTypeEntry {
    int            value;
    const wchar_t* label;   // 下拉框
    const wchar_t* shortName; // 输出
};

const DnsTypeEntry kDnsTypes[] = {
    { DNS_TYPE_A,     L"A（IPv4 地址）",  L"A"     },
    { DNS_TYPE_AAAA,  L"AAAA（IPv6 地址）", L"AAAA" },
    { DNS_TYPE_CNAME, L"CNAME（别名）",   L"CNAME" },
    { DNS_TYPE_MX,    L"MX（邮件交换）",  L"MX"    },
    { DNS_TYPE_TEXT,  L"TXT（文本）",     L"TXT"   },
    { DNS_TYPE_NS,    L"NS（域名服务器）", L"NS"    },
    { DNS_TYPE_PTR,   L"PTR（反向解析）", L"PTR"   },
};
const int kDnsTypeCount = (int)(sizeof(kDnsTypes) / sizeof(kDnsTypes[0]));

int DnsTypeCount()
{
    return kDnsTypeCount;
}

int DnsTypeValueAt(int i)
{
    if (i < 0 || i >= kDnsTypeCount) return DNS_TYPE_A;
    return kDnsTypes[i].value;
}

const wchar_t* DnsTypeLabelAt(int i)
{
    if (i < 0 || i >= kDnsTypeCount) return kDnsTypes[0].label;
    return kDnsTypes[i].label;
}

const wchar_t* DnsTypeName(int t)
{
    for (int i = 0; i < kDnsTypeCount; ++i) {
        if (kDnsTypes[i].value == t) return kDnsTypes[i].shortName;
    }
    return L"?";
}

} // namespace netcore
