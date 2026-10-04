// ===========================================================================
// packcore.h —— 「文件背包」的传输层
//
// 整个应用只认这**五个动作**：Probe（连通+建目录）/ List / Read / Write。
// 为什么要把传输隔离得这么干：
//   王 2026-10-03 拍板走"复用 Windows 自带的 OpenSSH 客户端"（系统组件，
//   不是我们打包的第三方 exe）。代价是**搬运字节这一段不是自研**，
//   所以它被单独关在这一个文件里 —— 日后要换自研 SSH/SFTP，
//   只改这里，界面与配置一行不用动。
//
// 两种后端：
//   · 本地目录（host 写成 local:<目录>）：直接文件操作。
//     **不是测试专用**：没网时照样能记东西；也正因为它，
//     那条"编辑→自动同步→重开→内容回来"的全链路才能离线自检。
//   · SSH（ssh.exe / sftp.exe）：
//       探测/建目录/列目录 → ssh.exe "mkdir -p / ls"
//       读 → ssh.exe "cat '<远端文件>'"（标准输出重定向到本地临时文件）
//       写 → ssh.exe "cat > '<远端文件>'"（标准输入来自本地临时文件）
//     密码认证靠**自问自答**：把 SSH_ASKPASS 指向我们自己的 exe
//     （backpack.exe --askpass 从环境变量取密码打出来），
//     不用 ConPTY、不用 sshpass。
// ===========================================================================
#pragma once

#include <string>
#include <vector>

namespace packcore {

enum class St {
    Ok,
    NotFound,      // 远端没有这个文件
    Unreachable,   // 连不上（网络/端口/主机）
    AuthFailed,    // 连上了但认证不过
    Cancelled,     // 用户中途点了「取消」
    Error,         // 其它（含本地 IO 失败）
};

// 一次操作的成败 + 给人看的原因
struct Res { St st = St::Error; std::wstring msg; };
inline bool Ok(const Res& r) { return r.st == St::Ok; }

struct FileItem {
    std::wstring name;
    unsigned long long size = 0;
    long long          mtime = 0;      // Unix 秒（0 = 拿不到）
    bool               isDir = false;
    // ---- v2.0 ----
    bool               isLink = false; // 符号链接（ls -l 的 `l`）。指向目录还是文件，
                                       //   ls -l 看不出来 —— 界面上"双击时试着进一下"。
    std::wstring       linkTo;         // 指向哪儿（只用于显示 `名字 -> 目标`）
    bool               isOther = false;// fifo / socket / 块设备 / 字符设备。
                                       //   以前这些行被整条丢掉（列表里直接消失），
                                       //   现在列出来、标成"其它"、双击无动作。
};

// 目标：本地 or 远端，界面把 config 拆成这个再交给传输层
struct Target {
    bool         local = false;
    std::wstring localRoot;                 // 本地模式：直接操作这个目录

    std::wstring host;                      // SSH 模式
    int          port = 22;
    std::wstring user;
    std::wstring authType;                  // "password" | "key"
    std::wstring password;
    std::wstring keyPath;
    std::wstring folder;                    // 远端背包目录（POSIX 路径）
};

Res Probe(const Target& t);                                             // 连通 + 建目录

// 列目录。relDir 是**相对背包根**的子目录（空 = 根），分隔符一律用 '/'。
// 为什么要有它：文件栏要能点进子文件夹、还能返回上级（王 v1.4 的要求）。
Res List (const Target& t, const std::wstring& relDir, std::vector<FileItem>& out);
inline Res List(const Target& t, std::vector<FileItem>& out) { return List(t, L"", out); }

// v2.0：是不是"写了一半临时文件"的名字（列表里一律不显示；Probe 会清掉它们）
bool IsTempName(const std::wstring& name);

// ---------------------------------------------------------------------------
// v2.0：**一次 ssh 办三件事**（探测 + 读便笺 + 列目录）
// ---------------------------------------------------------------------------
// 为什么：刷新的老路子是 Probe + Read + List **三个 ssh 进程**，每个都要重新
// 握手认证一遍 —— 这是这个应用最大的延迟来源。Windows 自带的 ssh 不支持
// ControlMaster（连接复用），所以只能把三条命令合到一次会话里。
//
// 帧格式：每条命令前打一行随机哨兵，客户端按哨兵切段。哨兵每次运行都不一样，
// 撞车概率可以忽略；**万一没解析出来就报"不可用"，调用方退回三次独立调用** ——
// 绝不猜、绝不半信半疑地用。
struct SessionOut {
    bool         usable  = false;    // 这次会话能不能用（false = 调用方走老路）
    bool         probeOk = false;    // mkdir 那一段过了吗（连接 + 权限）
    bool         noteOk  = false;    // 便笺段拿到了吗（文件可能还不存在）
    bool         listOk  = false;    // 目录段拿到了吗
    std::string  note;               // 便笺内容（**原样**，不做任何 trim）
    std::vector<FileItem> files;     // 目录内容
    Res          res{ St::Error, L"" };   // usable 但 probeOk 为假时的具体错误
};

// 成功（Ok）= 会话可用，看 out 里哪几段拿到了；
// 失败且 out.usable==false = 不可用（对面 shell 不认这套写法），调用方走老路；
// 失败且 out.usable==true  = 是真失败（连接/认证/权限），消息在返回值里。
Res Session(const Target& t, const std::wstring& relDir, const std::wstring& noteName,
            SessionOut& out);

Res Read (const Target& t, const std::wstring& name, std::string& data);
Res Write(const Target& t, const std::wstring& name, const std::string& data);

// ---------------------------------------------------------------------------
// v1.6：带进度、可取消的**文件级**传输（大文件专用）
//
// 为什么不复用上面的 Read/Write：那两个是把整份内容塞进 std::string ——
// 一个 2GB 的文件会直接把内存吃光，而且中途没法给进度、没法中断。
// 下面这两个是"流式"的：下载把 ssh 的 stdout 直接重定向到本地文件，
// 上传把本地文件接到 ssh 的 stdin，全程只在磁盘之间搬。
//
// cb 的返回值 = **是否继续**：返回 false 表示调用方要求取消，
// packcore 会立刻 TerminateProcess 并删掉没下完的半截文件。
// user 由调用方自己用（界面层拿它更新进度条、抽消息泵，保证「取消」点得到）。
// ---------------------------------------------------------------------------
typedef bool (*ProgressCb)(long long done, long long total, void* user);

// name → localPath。expectSize 传已知大小（列表里那个），用来算百分比；0 = 不知道。
Res ReadToFile (const Target& t, const std::wstring& name,
                const std::wstring& localPath, long long expectSize,
                ProgressCb cb, void* user);

// localPath → name。
Res WriteFromFile(const Target& t, const std::wstring& name,
                  const std::wstring& localPath, long long expectSize,
                  ProgressCb cb, void* user);

// 删除（文件直接删；目录要调用方先确认 —— 远端用的是 rm -rf）。
Res Remove(const Target& t, const std::wstring& relName, bool isDir);

// ---------------------------------------------------------------------------
// v1.7：给「回收站 / 撤销 / 重命名」用的三个原语
//
// 为什么把回收站放在**界面层**实现、只在这里加三个通用动作：
//   回收站的规则（10 个槽怎么绕圈、指针文件放哪儿）是这个应用的**策略**，
//   而"移动/存在/建目录"是任何文件系统都有的**能力**。策略放界面层，
//   日后改规则（比如槽位数）不用碰传输层；能力放这里，本地/SSH 各实现一次。
// ---------------------------------------------------------------------------
// 移动 / 改名（fromRel → toRel，都是相对背包根的路径）。
// 目标已存在时**不覆盖**，直接报错 —— 静默覆盖别人的文件是最不该发生的事。
Res Move(const Target& t, const std::wstring& fromRel, const std::wstring& toRel);

// 某个相对路径在不在。
Res Exists(const Target& t, const std::wstring& rel, bool& exists);

// 递归建目录（相对背包根）。
Res MakeDirs(const Target& t, const std::wstring& relDir);

// 拼出"给人看的完整位置"（本地是盘符路径，远端是 POSIX 路径）。
// 用途：右击的「复制路径」和"文件属性"里那一行。
std::wstring FullPathOf(const Target& t, const std::wstring& relName);

// 诊断用：把"会执行什么命令"拼出来（不真的执行）。
// 出问题（或要人工核对）时，一眼看清到底跑的是什么 —— 命令行工具最怕这个不透明。
std::wstring Describe(const Target& t);

// 诊断用：把探针那条 ssh 命令**原样跑一遍**，返回"命令 + 退出码 + stdout + stderr"。
// 界面上的错误是"翻译过"的一句话，往往不够；排障时（或需要人工核对时）要看原始输出。
std::wstring Diagnose(const Target& t);

// ssh.exe 的位置（找不到返回空串）。先用 System32\OpenSSH，再退到 PATH。
std::wstring SshExe();

// ---------------------------------------------------------------------------
// v1.9：设置"抽消息泵"的回调（界面层传自己的 PumpMessages 进来）。
//
// 为什么需要它：等 ssh.exe 的那几秒里，本线程是**完全阻塞**的 ——
// 界面既不重画、也不响应。结果就是"打开文件背包要等好几秒，而且这几秒
// 窗口看起来是死的"，连不上时更糟：一个卡住的窗口 + 最后一句"未连接"。
//
// 传输层自己不认识 UI，所以只留一个钩子：等进程的循环里隔一会儿调一次，
// 交给上层的消息泵去把重绘/输入处理掉。传 nullptr = 不抽（默认）。
//
// ⚠️ 抽泵意味着"这段代码还没跑完，别的消息就可能被派发进来"——
//    调用方必须自己挡住重入（文件背包是用 g_confirming / g_busy）。
// ---------------------------------------------------------------------------
void SetPump(void (*fn)());

// 把一段文本切成"看起来像行"的样子（给状态/错误提示用，最多 3 行）
std::wstring FirstLines(const std::string& utf8, int maxLines = 3);

} // namespace packcore
