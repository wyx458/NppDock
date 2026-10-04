// ===========================================================================
// appui.h —— 各个可嵌入应用**共用**的一小层界面基础件
//
// 为什么要有这一层（血泪）：
//   字体/DPI 那套代码原本在「文件校验」和「网络测试」里各写了一遍，
//   而 v1.3 重写「网络测试」时把**正确的那份**顺手重写成了错的 ——
//   字号被放大 1.5 倍（错了不报错，只能眼看，见 踩坑记录 20.1）。
//   所以从第三个应用开始，这类"错了不报错"的代码**只留一份**。
//
// 这一层只放两件事：
//   1) 字体：按**当前 DPI** 取系统消息字体（等宽字体取同一个 em）；
//   2) 线条图标：齿轮 / 刷新 / 垃圾桶（自绘按钮用，不依赖字体里的符号）。
//
// 它**不认识** dock（不 include dock 的任何头），只依赖 Win32 ——
// 应用应当能单独编译、单独跑（见《应用开发指南》§2）。
// ===========================================================================
#pragma once

#include <windows.h>

namespace appui {

// DPI **感知**必须在建任何窗口之前定下来（每个进程只能设一次）。

//   host 非空 → 跟宿主用同一套坐标（嵌入 dock 的前提：两边感知不同，
//               同一块屏幕就有两套数值，鼠标点击、窗口矩形全会错位）；
//   host 为空 → 独立窗口要用 per-monitor-v2。
//
//   ⚠️⚠️ 这里有个真踩过的坑：`SetProcessDpiAwareness` 是 **shcore.dll** 导出的，
//   不是 user32.dll！之前两个应用都在 user32 里 GetProcAddress 它，
//   永远拿到 NULL —— 于是"独立运行"时进程是 **unaware**：
//   在 125%/150% 缩放下整个窗口被系统位图拉伸（字发虚），
//   而且跨进程发过来的鼠标坐标会被**虚拟化**（实测 1.5 倍偏差）。
//   所以这里的兜底顺序是：宿主上下文 → SetProcessDpiAwarenessContext(v2)
//   → shcore 的 SetProcessDpiAwareness → SetProcessDPIAware。
void AdoptHostDpiAwareness(HWND host);

// 应用启动时（拿到窗口、测出 DPI 之后）调一次
void SetDpi(int dpi);
int  Dp(int px);

// 字体的"字符高度"（像素）。lfHeight 为负 = 按字符高度算，这里返回正数。
int  FontCellHeight(HFONT f);

// 界面字体 = **当前 DPI 下**的系统消息字体。
// ⚠️ 不要拿 SPI_GETNONCLIENTMETRICS 的字号再乘一次 dpi/96 ——
//    系统给的字号本身已经是缩放过的，乘第二遍正好放大 1.5 倍。
//    这里已经处理好了（SystemParametersInfoForDpi），直接用。
HFONT MakeUiFont();

// 等宽字体（Consolas → Lucida Console → Courier New，都拿不到就返回 nullptr）。
// 字号取 uiFont 的 **em（lfHeight）**，不是 tmHeight ——
// tmHeight 含字体自己的行距，中文字体行距大，照搬会让输出区大一圈。
HFONT MakeMonoFont(HFONT uiFont);

// 当前 DPI（造字体/换算尺寸时要）
int  Dpi();

// 界面字体（当前 DPI 的系统消息字体）换算成"点"—— 想造"比界面小一号"的
// 字体时用它算基准，别写死数字（换个 DPI/主题就偏了）。
int  UiFontPt();

// 通用造字体：face 为空 → 用系统消息字体；
// pt > 0 → 按**点**（72dpi 基准）算；pt <= 0 → 跟 UI 字号（mono 时取 8/9）。
// bold 显式给出（默认不加粗 —— 王明确说过不要加粗）。
// ⚠️ 造完会**回读字型名核对**：CreateFontIndirect 找不到字型时不报错、
//    静默换一个，不核对就会以为设上了（踩坑记录 20.6）。
HFONT MakeFont(const wchar_t* face, int pt, bool mono, bool bold = false);

// ---- 线条图标（简约风、黑白灰，自绘按钮用；不依赖字体里的符号）----
HPEN MakeIconPen(int width, COLORREF col);
void DrawGearGlyph   (HDC dc, const RECT& rc, COLORREF col);   // ⚙ 设置/配置
void DrawWrenchGlyph (HDC dc, const RECT& rc, COLORREF col);   // 🔧 设置/配置（v1.4 起优先用它）
void DrawRefreshGlyph(HDC dc, const RECT& rc, COLORREF col);   // ↻ 同步/刷新
void DrawTrashGlyph  (HDC dc, const RECT& rc, COLORREF col);   // 🗑 删除/清空
void DrawInfoGlyph   (HDC dc, const RECT& rc, COLORREF col);   // ❗ 查看属性
void DrawFolderGlyph (HDC dc, const RECT& rc, COLORREF col);   // 📁 目录（行首）
void DrawFileGlyph   (HDC dc, const RECT& rc, COLORREF col);   // 📄 文件（行首）
void DrawUpGlyph     (HDC dc, const RECT& rc, COLORREF col);   // ↑ 返回上级

// BS_OWNERDRAW 按钮的通用画法：原生按钮框（含按下/禁用） + 你给的图标。
// icon 为 nullptr 时只画框（给"纯图形按钮"以外的场合留口子）。
typedef void (*GlyphFn)(HDC, const RECT&, COLORREF);
void DrawIconButton(const DRAWITEMSTRUCT& dis, GlyphFn icon);

// 画一个"状态圆点 + 文字"（给 SS_OWNERDRAW 的静态控件用）。
// 为什么要自绘：● 这种字符要靠字体链接去凑，换机器可能变方框（同 20.8 的教训）。
void DrawStatusDot(HDC dc, const RECT& rc, COLORREF dotColor, const wchar_t* text);

// v1.6：状态行 = 圆点 + 提示词（+ 可选进度条）。
//
// ⚠️ 圆点和提示词是**两条独立的状态线**：
//    圆点只表示"连接状态"（灰=未知 / 绿=已连接 / 黄=忙 / 红=失败），
//    提示词只表示"当前在说什么事"。两者分别更新，互不覆盖 ——
//    所以这里要两个参数，而不是一个"整行文案"。
// pct < 0 = 不画进度条；0..100 = 画（进度条只出现在下半部，不挤占文字）。
void DrawStatusLine(HDC dc, const RECT& rc, COLORREF dotColor,
                    const wchar_t* text, int pct);

// v1.6：带文字的图标按钮（左上一排：.. 下载 上传 信息 删除）。
// 宽度不够时**自动只画图标**（小面板下不至于把文字挤成一团）。
// danger = true 画红框红字（「取消」用它）。
void DrawTextButton(const DRAWITEMSTRUCT& dis, const wchar_t* text,
                    GlyphFn icon, bool danger);

// v1.6：下载 / 上传图标（箭头 + 托盘）
void DrawDownloadGlyph(HDC dc, const RECT& rc, COLORREF col);
void DrawUploadGlyph  (HDC dc, const RECT& rc, COLORREF col);

// v1.7：撤销图标（逆时针的圆弧箭头）—— 「删除」按钮在"没选中任何东西"时
// 变身成「撤销」，用它表示"把刚删掉的搬回来"。
void DrawUndoGlyph    (HDC dc, const RECT& rc, COLORREF col);

// v1.8：🖊 重命名 / ✕ 取消
void DrawPencilGlyph  (HDC dc, const RECT& rc, COLORREF col);   // 重命名
void DrawCopyGlyph    (HDC dc, const RECT& rc, COLORREF col);   // 复制（两片纸，v1.9.2）
void DrawCancelGlyph  (HDC dc, const RECT& rc, COLORREF col);   // 取消（传输中断）

// v1.8：**纯图标按钮**（不带文字）—— 文件操作那一排改成了图标。
// danger = true 画红框红图标（传输中的「取消」用它）。
void DrawGlyphButton(const DRAWITEMSTRUCT& dis, GlyphFn glyph, bool danger);

} // namespace appui
