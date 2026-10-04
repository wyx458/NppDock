// ===========================================================================
// packcfg.h —— 「文件背包」的配置层
//
// 只干三件事：读写 config.json、算"背包目录在哪"、判断走本地还是 SSH。
// 界面不认识 JSON，传输层不认识文件路径规则 —— 归属都在这儿。
// ===========================================================================
#pragma once

#include <string>

namespace packcfg {

// 与 config.json 的字段一一对应（王给的格式，不许擅自改名/加必填项）
struct Config {
    std::wstring host;              // 服务器；也支持 "local:<本地目录>" 或纯本地路径
    int          port          = 22;
    std::wstring username;
    std::wstring authType      = L"password";   // password | key
    std::wstring password;
    std::wstring privateKeyPath;
    std::wstring remoteFolder;                  // 远端背包目录
    std::wstring backpackName  = L"我的背包";

    // ---- 便笺区外观（界面上**没有**调节入口；改这里 → 重开标签页生效）----
    std::wstring noteFont    = L"Consolas";     // 等宽字体名（找不到会自动退回）
    int          noteFontPt  = 8;               // 字号（**点**）；0 = 跟界面字号
    std::wstring noteBg      = L"#FFFFFF";      // 便笺背景（#RRGGBB）
    std::wstring noteFg      = L"#202020";      // 便笺文字色（#RRGGBB）

    // ---- v1.6：本地目录 + "确认连接"的节奏（同样只在配置文件里改）----
    //
    // ★ v1.7：这两项支持 **%环境变量%**（写 `%USERPROFILE%\Downloads` 就不会
    //   把一个具体的用户名钉在配置里了 —— 王 2026-10-04 的要求）。
    //   读配置时会展开；展开失败就原样用。
    std::wstring downloadDir;                   // 「下载」按钮落到哪儿（默认 %USERPROFILE%\Downloads）
    std::wstring openTmpDir;                    // 打开用的临时目录（默认 %LOCALAPPDATA%\NppDockBackpack\open）
    int heartbeatSec    = 300;                  // 每几秒自动确认一次连接（v1.7 起默认 5 分钟）；0 = 不心跳
    int ioMinIntervalMs = 1000;                 // 连接确认的最小间隔（毫秒）：过密的请求合并掉
};

// config.json 的路径 = exe 同目录（和另外两个应用的配置文件同一个位置）
std::wstring ConfigPath();

// 读配置：文件不存在会**生成一份带注释的默认配置**再返回默认值。
// 文件写坏了（手改出语法错误）：原文件改名成 .bad 留证据，用默认值继续跑。
bool Load(Config& out, bool* createdDefault = nullptr);

// 写回配置文件（保持稳定的字段顺序 + 中文注释）
bool Save(const Config& c);

// ★ v1.6：应用**不再回写**配置文件。
//
//   曾经有个 SaveNoteAppearance（Ctrl+滚轮调字号后只覆盖外观四项再写回），
//   现在便笺的字号调节入口已经取消 —— 界面上不再有任何"改配置"的动作，
//   于是这个文件对应用而言变成**只读**：读配置 → 跑；想改就改文件 → 重开标签页。
//   两个写者（人 + 程序）共用一个文本文件的老问题，从根上没有了。
//
// 默认的「下载」目录：优先取系统的「下载」已知文件夹，拿不到就退回 %USERPROFILE%\Downloads。
std::wstring DefaultDownloadsDir();

// 默认的「打开」临时目录：%LOCALAPPDATA%\NppDockBackpack\open
// （拿不到 LOCALAPPDATA 就退到 %TEMP%；都没有就退到 exe 同目录下的 _open）
std::wstring DefaultOpenTmpDir();

// ★ v1.7：展开 %USERPROFILE% / %LOCALAPPDATA% 这类环境变量。
//   为什么值得做：把默认值写死成 `C:\Users\<某个人>\Downloads` 的话，
//   配置一换机器就指到不存在的地方，而报出来的错却是"写不进文件"，
//   很难往"默认值算错了"上想。
std::wstring ExpandEnv(const std::wstring& s);

// ★ v1.7：两个"保留名" —— 它们在文件栏里**一律不显示**。
//   · 便笺：它是这个应用的"正文"，不是背包里的一个普通文件。
//     （之前它在列表里、还能被删；删掉之后应用又会自动重建一份同名空文件，
//       看起来"删了跟没删一样" —— 王的原话。）
//   · 回收站：删除落地的目录（0~9 十个槽 + 一个指针文件）。
//     内部结构，给用户看见只会误操作。
std::wstring NoteFileName();     // 便笺在背包根的文件名（带前导点，像个隐藏文件）
std::wstring TrashDirName();     // 回收站目录名（背包根下）
bool IsReservedName(const std::wstring& name);   // 是不是上面这两个（或它们的旧名字）

// 便笺的旧文件名（v1.6 及以前）。首次连接时如果只有旧文件、没有新文件，
// 就把旧文件改名成新的 —— 用户写下的东西一个字都不能丢。
std::wstring LegacyNoteFileName();

// 补默认值：remoteFolder 为空时按 SSH 用户名算出来（"同一用户名 = 同一个背包"）
void EnsureDefaults(Config& c);

// host 写成 "local:..." 或一个存在的本地目录 → 走本地文件操作。
// 这**不是**测试专用：没网时照样能记东西；顺带让自检能离线跑完整条同步链路。
bool IsLocal(const Config& c);
std::wstring LocalRoot(const Config& c);

// 配置还停在模板上（没填服务器 / 用户名还是占位符）？
// 首次运行用它决定"要不要去连" —— 免得白等一次连接超时。
bool LooksUnconfigured(const Config& c);

// 实际用的远端目录（含默认值推算）
std::wstring EffectiveRemoteFolder(const Config& c);

// 当前用户的 .ssh 目录（拿不到 USERPROFILE 就退回家目录；都没有返回空串）。
// private_key_path 的默认值就是它 —— 王说"默认填当前用户目录下的 .ssh"。
std::wstring DefaultSshDir();

// "#RRGGBB" / "RRGGBB" → COLORREF。解析不了返回 false（调用方用默认色）。
bool ParseColor(const std::wstring& text, unsigned long& outRgb);

// 把 COLORREF 反过来写成 "#RRGGBB"
std::wstring FormatColor(unsigned long rgb);

} // namespace packcfg
