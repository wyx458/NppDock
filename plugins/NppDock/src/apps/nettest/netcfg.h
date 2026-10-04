// ============================================================================
// netcfg.h —— 网络测试应用的配置文件（JSON）
// ----------------------------------------------------------------------------
// 文件位置：**与 exe 同目录**的 NppDockApp_NET.json
//   （和以前的 .history.ini 同一个位置；那份旧文件由 Load() 做一次性迁移后不再用）
//
// 为什么配置要放在外面、并且由"小齿轮"按钮打开：
//   王的要求是"四个功能全部选择简单且合理的参数"，同时"改配置文件就能改这些"。
//   于是界面上**一个参数下拉框都没有**，参数全在文件里 —— 界面只留
//   4 个功能按钮 + host 行 + 输出区。想换参数就点小齿轮改文件，改完下次运行生效。
//
// 文件里 host 有三处记录（王明确要求的三个语义）：
//   1. last   —— 上次用的 host，**直接填在输入框里**
//   2. recent —— 最近用过的，**下拉框最上面 5 条**（最新在前）
//   3. common —— 用得最多的，**下拉框最下面 5 条**（按使用次数）
//
// ⚠️ 写文件用"先写临时文件再改名"（见 Save）：配置文件是用户手改过的，
//    绝不能因为写到一半失败（磁盘满、被杀）把它截断成一个坏文件。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace netcfg {

struct HostCount {
    std::string host;
    int         count = 0;
};

struct Config {
    // ---- host 三处记录 ----
    std::string            last;        // 上次的 host（填进输入框）
    std::vector<std::string> recent;    // 最近的（最新在前，最多留 10 条）
    std::vector<HostCount>   common;    // 常用的（按次数排序）

    // ---- 四个功能的参数（全是"简单合理的默认值"）----
    int pingCount      = 4;
    int pingIntervalMs = 1000;
    int pingTimeoutMs  = 1000;
    int pingPayload    = 32;

    int traceMaxHops     = 30;
    int traceProbesPerHop = 3;
    int traceTimeoutMs   = 1000;

    // 端口扫描：王选的"配置文件里写区间，默认 100 个常用端口"
    std::string portSpec     = "";      // 空 = 用内置默认（见 DefaultPortSpec()）
    int         scanTimeoutMs  = 1000;
    int         scanConcurrency = 32;

    int         dnsType   = 1;          // 1 = A
    std::string dnsServer;              // 空 = 跟随系统；否则写 IPv4 数字地址
    int         dnsTimeoutMs = 5000;    // v2.0：查询超时（以前是 nettest 里硬写的假参数）
};

const int kRecentKeep = 10;   // 文件里最多留多少条"最近"（界面只显示前 5）
const int kListShow   = 5;    // 下拉框：近期 5 条在上、常用 5 条在下

// 内置默认端口集合（100 个常用 TCP 端口，逗号分隔），写在配置文件初值里
const char* DefaultPortSpec();

// 配置文件路径（exe 同目录的 NppDockApp_NET.json）
std::wstring ConfigPath();

// 读配置。文件不存在 → 用默认值并**立刻写一份初值**（带说明注释的模板），
// 这样用户第一次就能看到"能改什么"。
// 文件存在但写坏了 → 保留原文件（改名成 .bad 备份）、用默认值继续，
// 并在 outErr 里说明 —— 绝不静默丢用户的文件。
bool Load(Config& out, std::wstring& outErr, bool& outCreated);

// 落盘（临时文件 + 改名）
bool Save(const Config& c);

// 记一次使用：更新 last / recent / common（去重、计数、截断）
void RememberUse(Config& c, const std::string& host);

// 下拉框要显示的顺序：近期前 5（最新在前）+ 常用前 5（次数多的在前，去重）
std::vector<std::string> ComboEntries(const Config& c);

// 把 "22,80,443,8000-8100" 解析成端口列表（去重、升序、过滤非法值）。
// ⚠️ 解析失败**不报错**、返回空 —— 用户的配置文件随时可能写错，
//    上层用 DefaultPortSpec() 兜底即可（并且把"用了默认值"写进输出区）。
std::vector<int> ParsePortSpec(const std::string& spec, bool& outUsedDefault);

} // namespace netcfg
