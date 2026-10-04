// ============================================================================
// netcore.h —— 网络测试引擎（与界面**完全解耦**）
// ----------------------------------------------------------------------------
// 这一层只做一件事：**跑一条网络测试，把结果一行一行吐出来**。
// 它不知道有窗口、不知道有下拉框、更不知道 dock 的存在。
//
// 【为什么不调系统的 ping.exe / nslookup.exe / netstat.exe】
//   1) 可控性：ping.exe 根本没有"每次间隔"这个开关（Windows 版只有 -n/-w/-l），
//      而"频率"恰恰是这个工具要能配的东西。要控就得自己发 ICMP。
//   2) 输出形态：调外部程序只能拿到一坨文本，想加统计、想过窄面板自动截断
//      都做不了；自己算的话每个字段都是活的。
//   3) 进程：每次测试起一个控制台子进程，在嵌入面板里会闪黑框、还可能被
//      安全软件拦；直接调 Win32 API 没有这些问题。
//   代价是代码量大一点（这个文件 ~900 行）—— 王说过"不要惧怕 exe 过大"，
//   那就按做对的方式来。
//
// 【依赖说明】
//   用到的三个系统库都是 Windows 自带的，不是第三方运行时：
//     · iphlpapi.dll —— ICMP / ARP 表 / 适配器 / 连接表
//     · ws2_32.dll   —— 域名解析、TCP 连接探测
//     · dnsapi.dll   —— DNS 记录查询（A/AAAA/CNAME/MX/TXT/NS/PTR）
//   构建脚本里的 `--check-deps` 只禁止 MSVCP*/VCRUNTIME*/MSVCR*/api-ms-win-crt*
//   （那是"目标机可能没装 VC 运行库"的问题），系统 DLL 不在禁止之列。
//
// 【自检口】
//   同一个引擎还能被命令行直接调用（`--selftest <命令> [参数]`），
//   见 nettest.cpp。这是"算错了也不报错"的这类逻辑唯一能证明自己对的办法：
//   拿系统自带工具（ping / arp -a / netstat -ano）的输出做对照。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace netcore {

// ---------------------------------------------------------------------------
// 参数：所有命令共用一个大结构，谁用哪几个由 CommandAt() 的表决定。
// 为什么不做成一堆小结构 + 多态：命令是**有限且固定**的一组，
// 共用一个结构可以让"界面改一个下拉框 -> 取一个 int -> 塞进去"这条链路
// 短到看不出错；多态在这里只会让代码更难读。
// ---------------------------------------------------------------------------
struct Params {
    std::wstring target;      // 目标（IP / 域名）。自检类命令不需要

    int count        = 4;     // 次数（0 = 一直跑，直到用户终止）
    int intervalMs   = 1000;  // Ping：两次之间的间隔
    int timeoutMs    = 1000;  // 单次等待回复的超时
    int payload      = 32;    // Ping：负载字节数

    int maxHops      = 30;    // Tracert：最大跳数
    int probesPerHop = 3;     // Tracert：每跳探测几次

    int port         = 80;    // TCP 探测：端口

    // 端口扫描（v1.2 起用"扫一批端口、只报开放的"替代原来的单端口探测）
    // ⚠️ 端口列表由**调用方**解析好（区间/逗号列表的解析属于配置层的事，
    //    引擎不该知道配置文件长什么样）。空 = 引擎自己也不知道扫什么，直接报错。
    std::vector<int> ports;
    int  concurrency = 32;    // 并发数：一轮里同时发多少个连接

    int dnsType      = 1;     // DNS：记录类型（1=A 28=AAAA 5=CNAME 15=MX 16=TXT 2=NS 12=PTR）
    int dnsServer    = 0;     // DNS：0=系统默认，否则是 IPv4 的数字形式

    int view         = 0;     // 本机信息：0=全部 1=仅 IPv4 2=仅 IPv6
    int sortBy       = 0;     // ARP 表：0=按 IP 1=按接口
    int proto        = 0;     // 连接表：0=TCP 1=UDP
    int stateFilter  = 0;     // 连接表：0=全部 1=仅监听 2=已建立
};

// v2.0：把 "114.114.114.114" 这种 IPv4 字面量转成**网络字节序**的 32 位值
//（塞给 Params::dnsServer）。解析不了返回 false。
// 放在引擎层是因为这里已经有 winsock 头 —— 界面层不必为了这么点事再引一遍。
bool ParseIpv4Net(const std::wstring& s, unsigned int& netOrderOut);

enum class Kind {
    Ping,         // ICMP 连通性
    Tracert,      // 路由追踪
    TcpProbe,     // TCP 单端口连通性（保留：自检/对照系统工具时好用）
    PortScan,     // TCP 端口扫描（v1.2 起界面上的「端口扫描」用它）
    Dns,          // DNS 记录查询
    LocalInfo,    // 本机网络信息
    Arp,          // ARP 表
    Connections,  // 连接表（netstat）
};

// ---------------------------------------------------------------------------
// 输出与取消：两个回调，引擎**只**通过它们跟外界打交道
// ---------------------------------------------------------------------------
// emit：吐一行文本（调用方决定是写到的编辑框、还是打印到控制台）
// tick：引擎在每轮循环里调一次，返回 false 表示"用户要求中止"
//       界面侧的实现就是"泵一下消息 + 看取消标志"，见 nettest.cpp。
//       放在引擎里而不是让引擎自己 PeekMessage：引擎不碰 UI 反而更好测
//       （自检模式下 tick 直接返回 true 就行）。
struct Sink {
    void*                  user = nullptr;
    void (*emit)(void* user, const std::wstring& line) = nullptr;
    bool (*tick)(void* user) = nullptr;
};

// 跑一条命令。
//   返回 true  = 正常跑完（哪怕整条命令的结果是"全部超时"）
//   返回 false = 被 tick 要求中止
//   出错（参数不对、打不开 ICMP 句柄等）通过 emit 吐一行"错误：…"，返回 true。
//   —— 为什么不返回错误码：对用户来说"错误"也只是输出里的一行，
//      没必要让调用方再分一种状态出来。
bool Run(Kind kind, const Params& p, Sink& sink);

// 进程级初始化（WSAStartup）。返回 false 表示 Winsock 起不来，
// 此时只有纯 IP Helper 的命令（ARP/连接表/本机信息）还能用。
bool GlobalInit();
void GlobalShutdown();

// ---------------------------------------------------------------------------
// 命令表
// ---------------------------------------------------------------------------
// 界面（下拉框）和自检（--selftest <命令>）**共用这一份表**。
// 为什么要暴露出来而不是让界面自己写死一个列表：
//   以前踩过"界面上有的算法、自检里没有"这种漂移 —— 两边各写一份，
//   加了一条命令就得记得改两个地方，总有一次会漏。
//   现在只加在 CommandAt 的数组里，两边同时生效。
Kind CommandAt(int i);
int  CommandCount();
const wchar_t* CommandName(Kind k);   // 下拉框里显示的完整名（如 "Ping 连通性"）
const wchar_t* CommandKey(Kind k);    // 自检命令行用的短名（如 "ping"）
bool NeedsTarget(Kind k);             // 这条命令要不要填目标（IP / 域名）

// ---------------------------------------------------------------------------
// DNS 记录类型表（同样是共用一份，防止界面和引擎的类型编号对不上）
// ---------------------------------------------------------------------------
int  DnsTypeCount();                    // 条目数
int  DnsTypeValueAt(int i);             // 第 i 项的 Win32 类型值（1/28/5/15/16/2/12）
const wchar_t* DnsTypeLabelAt(int i);   // 第 i 项的下拉框显示名（如 "MX（邮件交换）"）
const wchar_t* DnsTypeName(int t);      // 类型值 -> 输出里那个短名（"A"/"AAAA"/…）

} // namespace netcore
