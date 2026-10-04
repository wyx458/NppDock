# NppDock — Notepad++ 底部停靠容器面板

在**底部停靠区**提供容器面板，本身不带功能、只提供"内容区"。加东西有**两条路**：

| 方式 | 加什么 | 界面在哪 | 适合 |
|---|---|---|---|
| **功能模块** | `NppDock_XXX.dll` | 本进程内直接建窗口 | 与编辑器深度交互、需要宿主 API |
| **可嵌入应用** | `NppDockApp_XXX.exe` | **独立进程**，自己 `SetParent` 上来 | 已写好的小工具，复用其 UI |

两条路都做到"**新增功能不改动任何既有代码**"。

```
┌─ Notepad++ 窗口 ──────────────────────────────┐
│  … 编辑区 …                                    │
├───────────────────────────────────────────────┤
│  文件校验 │  （标签条：常驻，没有应用页时也在）      │
│  ┌───────────────────────────────────────┐    │
│  │  NppDockApp_MD5.exe 的窗口（跨进程）    │    │
│  └───────────────────────────────────────┘    │
└───────────────────────────────────────────────┘
          ↑ 整个这一块 = 向 N++ 注册的唯一一个面板（CONT_BOTTOM）
```

---

## 0. 立规矩：可嵌入应用必须自研

**所有要嵌进 dock 的软件一律自己写，源码与功能目录都放工作区（`pluginsWorkspace/`）内，不接受第三方 exe。** 理由（工程约束）：UI 必须同时为"独立打开/嵌入式"两形态设计；DPI 感知必须与宿主一致，否则跨进程 `SetParent` 后坐标被 forced reset、尺寸算错；收到 `WM_CLOSE` 必须干净退出、不留孤儿进程；代码必须可控。对接只靠一个**字符串契约**（`--dock-parent <HWND>`）：应用不 include dock 头文件、不引用其常量，因此可**单独编译/发布/运行**。

---

## 1. 关键设计

**只向 N++ 注册 1 个面板**（固定 `CONT_BOTTOM`）：宿主多个面板会排成一行各自占位，NppDock 只占一个，对宿主永远是普通面板、不干扰其布局。

**懒加载**：上次退出时面板关着/隐藏 → 下次启动**不显示面板、也不恢复里面的应用页**（恢复一页要起一个进程），等面板被打开再补恢复；上次是开着的则照旧恢复。

> 实测 N++ 启动时会把已注册停靠面板一律显示，即使 `config.xml` 写着 `isVisible="no"`；所以插件自己维护状态文件并在注册后主动修正。

---

## 2. 目录结构

本插件在 `plugins/NppDock/`；**以下相对路径都相对本目录**（`cd pluginsWorkspace/plugins/NppDock` 后执行）；工作区总览见 `pluginsWorkspace/README.md`。

```
pluginsWorkspace/
├─ docs/NppDock应用开发指南.md（★ 写应用前先读） · docs/铁律与踩坑要点.md（建议先读）
├─ sdk/          N++ 官方 SDK 头（原样拷贝）：PluginInterface.h / Docking.h / Notepad_plus_msgs.h /
│                 NppDockScintilla.h（精简版）
└─ plugins/NppDock/
   ├─ build.py（编译+测试+静态自检） · scripts/run.py（拉起隔离实例手测）
   ├─ src/       NppDockApi.h（纯 C ABI 契约） · framework/NppDockUtil.h（无状态工具） ·
   │              main/（NppDock.cpp 入口·NppDockContainer 容器·NppDockEmbed 契约常量） ·
   │              apps/md5tool · apps/nettest（两个自研应用） · tests/（哑模块·离线集成）
   ├─ tools/     check_hash.py · check_toolbar.py · gen_toolbar_conf.py ·
   │              dock_app_probe.py（真机探针） · unit/（4 个单元壳 + run.py）
   ├─ _t/（测试数据/截图，可随时删） · build/NppDock/（产物 dll + NppDockApp_*.exe）
```

> ⚠️ `build.py` 定位 `sdk/` 用**向上找**（找含 `sdk/PluginInterface.h` 的那一级），不是数 `..`。见 `docs/铁律与踩坑要点.md` 第十四节。

---

## 3. 安装

把 **两个**文件复制到 `Notepad++/plugins/NppDock/`：

```bat
mkdir "C:\...\Notepad++\plugins\NppDock"
copy build\NppDock\NppDock.dll        "C:\...\Notepad++\plugins\NppDock\"
copy build\NppDock\NppDockApp_MD5.exe "C:\...\Notepad++\plugins\NppDock\"
```

`NppDockApp_*.exe` **必须与 `NppDock.dll` 同目录**（插件靠扫描自己所在目录发现应用）。也可 `python build.py --install`。重启 N++ 生效。

---

## 4. 快速验证

1. 菜单 **插件 → NppDock** 三项：`显示/隐藏 NppDock 面板`（`Ctrl+Alt+D`）、`重新扫描功能模块`、`打开运行日志`。
2. 面板顶部是**常驻标签条**（无应用页时也在）—— "添加应用"的**唯一入口**。
3. 在**标签条空白处**（标签项右侧）**右击** → 出应用列表 → 选中即多一页。**内容区右击不弹任何东西** —— 刻意设计。在**标签项上**右击 → `关闭这一页`。日志：`plugins\config\NppDock\NppDock.log`

> 三条右击规则：**内容区什么都不做；标签条空白处 = 加应用；标签项 = 关那一页。**

### 标签页行为（改动时别破坏）

* **同一应用可开多页**：右击空白 →「添加应用」→ 再点同一应用即**再开一页**（每页独立进程）。菜单是**干净应用名**：不标"已开 N"、不打勾。页标题=**应用名本身**，多页时**故意相同**（位置即区分）。
* **每个标签项最低宽度 ≥ 6 个汉字**（标签条自动宽度，短标题会挤成一块）：做法是给**文字**补全角空格（comctl32 无"最低宽度"设置，见 21.2）。**双击标签项 = 关闭那一页**；右击标签项菜单只有 **「关闭这一页」**；右击空白处叫 **「重新扫描」**。
* **标签可拖动改位置**：选中页**跟着页对象走**（只改位置、不改"在看哪页"）；位置落盘 `panel.ini` 的 `page0/page1…` 顺序，下次照此恢复。
* **当前标签上边沿有一条绿杠** `RGB(0x2E,0xA0,0x43)`，切页跟着走：做法"原生控件先画、之后在上面盖一条"，`WM_PAINT` 与 `WM_PRINTCLIENT` **两条都要挂**（见 15.1/17.6）。
* **内容区中央一句话**：无页时"右击上方标签条空白处添加应用（已发现 N 个）"；无应用时"`NppDockApp_*.exe` 必须和 `NppDock.dll` 放在同一目录"；应用起不来时写失败原因（进程没起来 / 8 秒内没挂上窗口）。⚠️ 它挂在 `contentWndProc` 的 `WM_PAINT`，该函数**故意不处理 `WM_CONTEXTMENU`**（内容区右击什么都不做）—— 别顺手把菜单加回来。

## 4.1 「文件校验」`NppDockApp_MD5.exe`

自带功能目录 + 自研应用样板。界面固定**三行 + 一条状态**，窄宽度下自动压缩行高、绝不横向滚动：

```
┌ 文件校验（嵌入） ──────────────────────────────┐
│ 文件路径   [ D:\path\to\file.bin               ]│
│ 校验结果   [ 8f14e45fceea167a5a36dedd4bea2543 ] │
│ 校验方式 [ SHA-256 ▾ ]     [浏览…][计算][复制] │
│ 就绪：支持 MD5 / SHA-1 / SHA-256 / … / CRC32   │  ← 这一行字号比上面小一号
└────────────────────────────────────────────────┘
                    ↑ 正在计算时这个按钮变成「终止」
```

* 三行布局（行1 路径 / 行2 结果 / 行3 方式下拉 + 三按钮 / 末行状态），**全界面 Consolas**（含编辑框），「计算」计算中**变身「终止」**。硬规定：① **两编辑框必须等长**（共用 `editX/editW`）⇒ **按钮不准放这两行里**；② **按钮与下拉框统一放第 3 行**；③ **下拉框高度按条目数算**、**条目高度必须在 `SetWindowPos` 之后**设置（几何变更会重置回字体默认值），否则展开只看半行；闭合高度仍与按钮同高；④ **按钮数量不准再涨**。
* 进度 `正在计算 SHA-256… 42% · 128 MB/s（点「终止」可停下）`（速度=最近 1 秒窗口），**百分比/速度放前面**（状态行 `SS_ENDELLIPSIS` 从右往左吃）。路径可键盘敲（带引号 `"复制为路径"` 自动去引号）/`浏览…`/回车；换算法**自动重算**，改路径把旧结果标"已过期"。算法 **MD5/SHA-1/SHA-256/SHA-384/SHA-512/CRC32**（全自实现、`dumpbin /dependents` 干净）。
* **拖入**拖文件自动填路径（文件夹明确拒绝，别译成"没有权限读取"）。⚠️ 光 `DragAcceptFiles` 不够：宿主常以**管理员**运行（High）+ 资源管理器 Medium，UIPI **静默丢掉** `WM_DROPFILES` ⇒ 对每个窗口 `ChangeWindowMessageFilterEx` 放行 `WM_DROPFILES`/`WM_COPYDATA`/`WM_COPYGLOBALDATA`，且 `DragAcceptFiles` 要**给每个子控件登记**（落下时 shell 投给光标底下的窗口）。

> 要写嵌进 dock 的应用？先读 [`../docs/NppDock应用开发指南.md`](../docs/NppDock应用开发指南.md)。

---

## 4.2 应用嵌入（把自研 exe 塞进 dock）

```
dock 侧（N++ 进程）                            应用侧（独立进程）
──────────────────────                        ─────────────────────
1. 建空宿主子窗口 NppDockAppHost
2. CreateProcess("app.exe" --dock-parent <HWND>) ──► 拿到宿主句柄
3. 泵消息等它挂上来                              3'. 建自己的窗口
                                                 4'. 去 WS_POPUP/标题栏、加 WS_CHILD
                                                 5'. SetParent(自己窗口, 宿主)
                                                 6'. SetWindowPos 铺满宿主客户区
4. FindWindowEx 找到它 = 挂载成功
5. 之后宿主 WM_SIZE -> 显式 SetWindowPos      ◄──  收到 WM_SIZE 自己重排控件
```

**关键约定：dock 只传一个父窗口句柄，剩下的应用自己搞定。** 应用侧必须做到 5 件事（解析参数用 `GetCommandLineW()`+`CommandLineToArgvW`、先改 `GWL_STYLE`→`SetParent`→`SetWindowPos`、**建窗口前**对齐 DPI 感知、处理 `WM_CLOSE`→`DestroyWindow`→`PostQuitMessage`、UI 弹性布局）—— 逐条的规则、症状与代码见 `docs/NppDock应用开发指南.md` 的 R1–R8。**清理**：关标签页 `WM_CLOSE` → 等 3 秒（**期间继续泵消息**）→ 超时才强杀；退出 N++ 用 `closeAllPages()`；N++ 被强杀时插件没机会跑，靠 **Job Object** 兜底（应用进程都放进设了 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的作业对象，宿主销毁时内核一并杀，**实测残留为 0**）。

---

## 4.3 「网络测试」`NppDockApp_NET.exe`

界面压成**一整行**，参数全部搬进 JSON 配置：

```
┌ 网络测试 ──────────────────────────────────────────────────────────┐
│ Host [ 192.168.1.1 ▾ ] [Ping测试][路由追踪][端口扫描][DNS查询] [⚙][🗑] │
│ ┌────────────────────────────────────────────────────────────────┐ │
│ │ [01:35:03] Ping 连通性   目标：192.168.1.1                      │ │
│ │ 来自 192.168.1.1 的回复: 字节=32 时间=2ms TTL=64                │ │
│ │ 数据包: 已发送 = 4，已接收 = 4，丢失 = 0 (0% 丢失)               │ │
│ └────────────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────┘
```

> **面板窄到一行放不下时自动退回两行**（判据是"下拉框还能剩多少"）；绝不能出现控件被挤出客户区。**两个图标按钮自绘**（扳手=设置、垃圾桶=清空输出区）；不用「⚙」（靠字体链接凑，换机器可能变方框）。

* **硬要求**：① **四按钮等宽**（按「最宽标签」与「运行时最长文案」较大者算）；② **运行中互斥**（按下一个 → 其余三个**变灰**、它变「取消」，结束/取消全复位；host 与扳手/清空也按住）；③ **无参数下拉框、无左下角状态行**，提示全进输出区；④ **输出区** Consolas、与界面**同号**、**Ctrl+A 全选**（挂只做这件事的子类过程）。
* **行为**：① 输出上限 **40 万字符**，到顶**丢最旧整行、保住尾巴**、顶部加"（前面已省略 N 行）"；② **取消立刻响应**（ICMP 在**一次性工作线程**、主线程 20ms 分片轮询；取消后**不再引用**该 job，见 29.7）；③ `dns.server` 真生效（留空=系统）+ `dns.timeoutMs`（默认 5000）；④ `ping.count = 0` = 一直 ping 直到「取消」。⚠️ **字体别"顺手重写"**：字号必须用 `SystemParametersInfoForDpi(..., dpi)` 取**当前 DPI** 一份直接用（再乘 `dpi/96` 会放大 1.5 倍且**不报错**）；等宽字体取正文 **`lfHeight`（em）**不是 `tmHeight`。功能：Ping测试 / 路由追踪 / 端口扫描（只报开放的）/ DNS查询（A/AAAA/CNAME/MX/TXT/NS/PTR）。
* **配置 `NppDockApp_NET.json`**（点扳手打开）：⚠️ 打开方式=拿宿主 exe 路径再 `CreateProcess`（不给宿主发消息 —— 跨进程 `SendMessage` 不封送指针，`NPPM_DOOPEN` 那种会把 N++ 写崩）；改完保存点按钮**即生效**；`//` 与 `/* */` 注释可写；写坏（语法错误）原文件**改名 `.bad`**、用默认值继续并写在输出区；**host 三处** `last`/`recent`（下拉上前 5，最多 10 条）/`common`（下拉下 5）；**`ports` 三种写法混用**（`"22,80,443"` / `"8000-8100"` / 混用），留空=内置 **100 个常用端口**。自检模式**不读配置文件**；依赖只用系统 DLL（`iphlpapi`/`ws2_32`/`dnsapi`）。

```
NppDockApp_NET.exe --selftest ping 127.0.0.1 --count 4 --interval 200
NppDockApp_NET.exe --selftest portscan 127.0.0.1 --ports "22,80,443,3389"
NppDockApp_NET.exe --selftest dns www.qq.com --type MX
NppDockApp_NET.exe --selftest dns example.com --server 192.0.2.1 --timeout 800  # 验超时
NppDockApp_NET.exe --selftest arp --sort 1
```

---

## 4.4 「文件背包」`NppDockApp_PACK.exe`

把服务器上一个 txt 当**便笺**（本地改完自动同步），同时是**小文件中转站**（下载/上传/删除/重命名）。

```
┌ 文件背包 ──────────────────────────────────────────────────────────────┐
│ [❗][🖊][🗑][↑][↓][📁]        [⚙][↻] │ ● 已连接 · 就绪 · 延迟 87 ms  │   ← 左：8 个等大图标按钮
├───────────────────────────┬────────────────────────────────────────────┤
│ 📁 ..                     │                                            │
│ 📁 子目录      12-03 13:20│  （便笺：纯输入框，不写字就没有任何提示）   │
│ 📄 readme.txt   1.2 KB …  │                                            │
└───────────────────────────┴────────────────────────────────────────────┘
   文件栏 38%（8 个图标按钮在它上方）      便笺 62%（状态提示词在它上方）
```

* ① 文件栏上方 **8 个等大正方形图标按钮**，左 6 固定顺序 `文件属性(❗) 重命名(🖊) 删除(🗑) 上传(↑) 下载(↓) 打开文件位置(📁)`、右 2 `设置(⚙)` `刷新(↻)`；便笺上方只**状态**（圆点 + 提示词）。② `..` 行 = 回上级（仅非根、双击返回）；单击选中、**多选** Ctrl/Shift 作用整批；**选中/取消交给原生列表框**（不加"点空白=取消"，`LB_ITEMFROMPOINT` 会误伤）。③ **删除 = 真删**（不弹确认、不进回收站；远端 `rm -f`/`rm -rf`）；**双击目录 = 进去**；**不提供"打开文件"**。
* ④ **传输可中断**：被点按钮**原地变红框红叉**（=取消，**且必须能按**）；期间**其它按钮全灰**；进度 `正在下载 xxx  43%（4.0/9.3 MB）  1.2 MB/s  剩余 4s`。⑤ **重命名原地改**（右击/🖊/**F2**；**回车**提交、**Esc** 放弃、点别处提交）。⑥ **状态线两条**（圆点=链路、提示词=当前事）。⑦ **刷新线两条**：便笺停 1.5 秒只存便笺+探连接、**不碰文件栏**；文件操作只**重拉一次**文件栏+探连接；「刷新」=存便笺+重读配置+确认连接+重拉；心跳 `heartbeat_sec`（默认 300 秒）同文件栏那路。**真**重拉写日志 `刷新文件栏：N 项`；**限流** `io_min_interval_ms`（默认 1 秒）被挡请求**合并补跑**。⑧ 便笺文件名保留名 `.nppbackpack-note.txt`，外观全来自配置（**纯只读**）；先摆界面再连接，**等 ssh 时抽消息泵**（`packcore::SetPump`）才不冻。
* ⑨ **数据安全**：① 写远端先写 `.nppbackpack-tmp-<pid>`、**校验字节数**后才 `mv`（只 `cat && mv` 会在连接"正常关闭"时盖半截，见 29.4；失败/取消删临时文件）；② 远端重命名**先查再移**；③ **"同步中"与"整个批次"都算忙碌**；④ **文件名按位置解析**（不"按空格切开再拼"）：特殊类型标"其它"、软链双击**试着进**；⑤ **刷新只起 1 个 ssh**（Win ssh **不支持 ControlMaster**）：三命令合到**一次会话**、哨兵分帧，解析不出**自动退回**三次。配置 `nppbackpack_config.json`：`host=local:<目录>` 不连服务器 / `private_key_path` 默认 `.ssh` / 改完保存即生效 / **从不回写** / `remote_folder` 占位符算成 `/home/<用户名>/npp-backpack` / `download_dir`·`open_tmp_dir` 支持 `%USERPROFILE%` / `heartbeat_sec`(300,0关) / `io_min_interval_ms`。CLI：`--selftest` / `--sshraw` / `--askpass`（密码取 `NPPDOCK_ASKPASS_PW`；别把 `NPPDOCK_ASKPASS_MODE` 设到系统环境）。

---

## 5. 面板停错位置？

停靠位置两部分决定，**持久化记录优先**：`config.xml` 的 `<PluginDlg>` 有记录时以 `curr` 为准；`uMask` 高 4 位**仅在该面板没有持久化记录时**生效。`curr`：`0`=左 `1`=右 `2`=上 `3`=下 `4`=浮动。**排查**：关闭 N++ → 删 `config.xml` 里 `<PluginDlg pluginName="NppDock.dll" .../>` 那行 → 重启；插件用 `uMask = DWS_DF_CONT_BOTTOM` 重新落位底部。

---

## 6. 面板状态没记住？

状态在 `plugins\config\NppDock\panel.ini`：

```ini
visible=1                     ; 面板上次显示(1)/隐藏(0)
page0=NppDockApp_MD5.exe      ; 上次开着的应用页，按页序（可多行）
page1=NppDockApp_Foo.exe
current=NppDockApp_MD5.exe    ; 上次选中的页
```

**重开 N++ 会一并恢复：面板显隐 + 上次开着的应用页（含选中页）。** 内容不对 → 删掉该文件，下次恢复默认（显示、无页面）。应用被删/改名 → 恢复时**跳过并记 WARN**，其余页照常，同时把状态文件**自动修正**为实际恢复结果。日志行：`已按上次状态恢复：…`、`应用页恢复完成：成功 N 个，跳过 M 个`、`恢复应用页：找不到 xxx.exe，已跳过`。⚠️ 页面记的是 **exe 文件名**不是下标（目录增删使下标漂移、下次会开**另一个**应用）。**显示/隐藏 + 页面**由插件管（panel.ini）；**高度/宽度**由 N++ 管（`config.xml` 的 `DockingManager`）。该文件**先写 `.tmp` 再整体替换**（`WriteFileAtomic`）；保存端也按 `kMaxRememberedPages`（64）截断，与读取端一致。

---

## 7. 架构要点

**7.1 纯 C ABI + POD + 纯虚接口**：模块与主 DLL 间**不传 C++ 标准库对象**；主 DLL 能力经函数指针表 `NppDockHostApi` 下发（结构体只能往末尾追加字段）；模块实现纯虚接口 `NppDockModule`，只导出 `nppdock_module_entry` / `nppdock_module_destroy`。**7.2 三条跨 DLL `/MT` 铁律**：① **禁止跨 DLL `delete`**（模块必须导出 destroy 函数由主 DLL 调用）；② **功能 DLL 不得链接主 DLL 的实现符号**（工具函数写成 `inline` 放头文件）；③ **静态状态在卸载后失效**。**7.3 两级懒加载**：第一级（启动扫描目录）只 `LoadLibrary` 取标题、**不创建视图**；第二级（第一次切到前台）才调 `createView`。

---

## 8. 新增功能页（DLL 方式）

1. 复制 `src/tests/DummyModule.cpp` 作模板。2. 实现 `NppDockModule` 全部纯虚函数（标题、`createView`、`destroyView` 等）。3. 编译成 DLL，文件名必须 **`NppDock_XXX.dll`**。4. 丢进 `Notepad++/plugins/NppDock/` 重启（或点"重新扫描功能模块"）。主 DLL 按文件名排序扫描加载。**不需改动主插件任何一行代码。**

---

## 8.1 新增可嵌入应用（exe 方式）

1. 在 `src/apps/<名字>/` 写普通 Win32 程序（参考 `md5tool/`）。2. 按 §4.2 那 5 条实现"双形态"：无参=标准顶层窗口；带 `--dock-parent <HWND>`=把自己 `SetParent` 进去。3. 加 `.rc` 版本资源，**`FileDescription`** 写成想要的中文标题 —— dock 读它**直接当标签页标题**（读不到退化为文件名去前缀）。4. 在 `build.py` 的 `APPS` 列表登记一行。5. 产物名必须 **`NppDockApp_XXX.exe`** 且与 `NppDock.dll` 同目录。`python build.py --install` 自动编译部署。**不需改动主插件任何一行代码。**

---

## 9. 构建

```bat
python build.py                # 主 DLL + 应用 + 测试
python build.py --check-deps   # 额外 dumpbin 静态自检（验证 /MT）
python build.py --install      # 构建并部署到便携版 Notepad++
python build.py --tests        # 只构建测试
python build.py --clean        # 清理
```

产物：`build/NppDock/` 下的 `NppDock.dll` + `NppDockApp_*.exe`

---

## 10. 测试

### 10.1 离线集成测试（无需 N++）

```bat
cd build\tests
test_integration.exe
```

"假宿主" + 真实 `NppDock.dll`，共 **32 条断言 / 9 组**：① 6 导出函数齐全 ② `setInfo`+菜单表 ③ 注册面板（`uMask` 必须底部槽位）③b 应用发现 ④ 状态持久化（写出/读回/隐藏与显示两种）⑤ 显隐反复切换有效 ⑥ 重复 `NPPN_READY` 幂等 ⑦ `NPPN_SHUTDOWN` 干净回收 ⑧ DLL 可卸载。

### 10.2 工具自检

```bat
python tools\check_hash.py       # 6 种摘要正确性：每种 14 组长度边界 + 3MB 随机，对照 hashlib/zlib
                                 #   默认全跑；--algo sha256 只跑一种、--certutil 再对系统 certutil
python tools\check_toolbar.py    # 读工具栏每按钮真实隐藏状态（TBSTATE_HIDDEN）
python tools\unit\run.py         # 单元测试壳（4 个）
```

**`tools/unit/`** 把被测 `.cpp` **整份 include 进来**，使 `static` 函数在编译单元内可见。四壳：`test_parsels`（`ls -l` 解析：连续/结尾空格·软链·fifo/socket/设备·`total` 行）、`test_packcore`（原子写·失败不留垃圾·`Move` 不覆盖·Probe 清扫）、`test_session`（一次 ssh 帧分割：空便笺/无尾换行/便笺含哨兵/缺段/无哨兵）、`test_outbuf`（丢最旧整行·保尾巴·上限后统计行仍在）。⚠️ 同一 `.cpp` 编译第二次、`build.py` 产物不受影响；**只证明纯逻辑**。`check_hash.py` 含**各算法补齐临界点**（MD5/SHA-1/SHA-256 的 `55/56/63/64/65`、SHA-384/512 的 `111/112/113`）。`NppDockApp_MD5.exe --selftest <文件> [--algo md5|sha1|sha256|sha384|sha512|crc32]`。

### 10.3 真机链路驱动

`tools/dock_app_probe.py` 用 `SendInput` 发**真实鼠标事件**跑完整链路（`TrackPopupMenu` 模态，不能 `PostMessage` 假装选项）：

```bat
python tools\dock_app_probe.py standalone    # 单开：标题栏/可缩放/无残留
python tools\dock_app_probe.py addapp        # 右击标签条空白 -> 选应用 -> 出现标签页
python tools\dock_app_probe.py menu          # 右击内容区 -> 应什么都不弹
python tools\dock_app_probe.py closetab      # 右击标签项 -> 关闭当前页
python tools\dock_app_probe.py dragsplitter --dx 140 --steps 12   # 真拖分隔条，验重绘
python tools\dock_app_probe.py toggletest                         # 隐藏 -> 唤出，验重绘
python tools\dock_app_probe.py resizetest --cycles 2 --delta 40   # 程序化改尺寸（对照）
python tools\dock_app_probe.py hashall <样本文件>  # 文件校验页版式/字号/键盘/6 算法/复制/无残留
python tools\dock_app_probe.py combo               # 下拉框几何：条目高度/展开条目数/抓图
python tools\dock_app_probe.py abort               # 「计算」变身「终止」：能停 + 状态全复位
python tools\dock_app_probe.py multiopen           # 多页：页数/进程数各 +1，标题不带序号
python tools\dock_app_probe.py tabmove             # 标签拖动排序
python tools\dock_app_probe.py tabv14              # 标签最低宽度 / 菜单文字 / 双击关闭
python tools\dock_app_probe.py tabsep              # 标签条下沿分界线
python tools\dock_app_probe.py filedrop            # 文件校验「拖入自动填路径」（含真算摘要）
python tools\dock_app_probe.py tabhl               # 当前标签绿杠：一段、贴上沿、随切换移动
python tools\dock_app_probe.py nettest             # 网络测试页总验收
python tools\dock_app_probe.py tree / appwin / log
```

`nettest` 总验收：控件点名 / 命令下拉 **7 条**与 `netcore` 表一致且目标框能手输 / 配置随命令变（切 UDP 后「筛选」消失）/ 真跑 ping / 目标记忆（文件与下拉框都更新、上限 10 条）/ 中止**复位必须 < 2.5 秒** / 复制清空 / 无残留。`hashall` 验版式（两编辑框 x、宽相同、按钮与下拉在第 3 行）+ 算法正确 + 剪贴板=结果 + 进程增量 0；`combo` 验下拉几何（6 条目全装下）；`dragsplitter`/`toggletest` 验重绘（门槛 1%，`--dy` 正负各一遍，**必须有 PIL**）。

**探针两条纪律（改探针时别破坏）**：① **DPI 对齐在 `import` 时完成**（幂等；只在 `main()` 里调会漏，坐标差 1.5 倍、点击全飞），目标**物理像素（per-monitor-v2）**；② **"点了" ≠ "生效"**：菜单项点完要校验，长任务按钮只能**异步发**（`PostMessage`）并轮询"起来了/结束了"（见 17.2/17.3/17.7）。三个细节：启动先调 `SetProcessDpiAwarenessContext`（否则看到**虚拟化**桌面，坐标永远差一个缩放比）；菜单项位置用 `MN_GETHMENU`+`GetMenuItemRect`（**必须传宿主窗口**，NULL 拿野坐标）；**绝不**跨进程 `SendMessage` 传指针（会把 N++ 写崩）。

---

## 11. 已知限制

* **面板尺寸不由插件控制**：停靠区大小由宿主 Docking Manager 保存恢复，插件无读写 API。
* **改尺寸/唤出后的重绘必须自己兜**：宿主改我们祖先尺寸或重新显示面板时**不会**替我们重画跨进程嵌进来的窗口；插件做三层补偿、**两条路径各走一遍**（改尺寸 `resizeEmbedded`；唤出走容器 `WM_SHOWWINDOW`，见 9.5/9.7）。**补偿不是根治** —— 探针 `dragsplitter`/`toggletest` 分守两路。
* **⚠️ 绝不用同步跨进程重画**：`RDW_UPDATENOW` 之类会让**整个 Notepad++ 死锁**；补重画一律**异步 + 时机**（`PostMessage` 排队列尾/防抖定时器，见 9.6）。判据：`IsHungAppWindow(宿主)==1` 而应用自身 `WM_NULL` 秒回 ⇒ 卡住的是**我们**。
* **应用必须自研**（§0）；第三方 exe 强嵌会遇到 DPI 不匹配、UI 不适应窄宽度、无法优雅退出。**完整性级别必须一致**（外部不同权限的程序无法互相 `SetParent`，UIPI 拦且**常静默失败**）。
* **快捷键冲突**：默认 `Ctrl+Alt+D`，可在 设置 → 快捷键 改。**深色模式未适配**（宿主提供 `DWS_USEOWNDARKMODE`）。
* **文件名里含换行**：文件背包目录解析**按行**（`ls -lA`），名字带 `\n` 会被拆两行；连续/结尾空格、软链、fifo/设备文件都已处理（§4.4），换行只影响**列表显示**、不影响**按名字操作**。根治需 `find -printf '%f\0'`（依赖 GNU findutils）。
* **Windows 的 ssh 不支持连接复用**：`ControlMaster`/`ControlPath` 在 Win32-OpenSSH 上是空操作；文件背包只能靠"把一次刷新的三条命令合成一条"降延迟（§4.4）。
