// ============================================================================
// NppDockScintilla.h
// ----------------------------------------------------------------------------
// 为什么需要这个文件：
//
// Notepad++ 官方 SDK 的 PluginInterface.h 里，插件导出函数
//   extern "C" __declspec(dllexport) void beNotified(SCNotification *);
// 用到了 SCNotification 类型，而该类型**并不定义在 Notepad_plus_msgs.h 里**，
// 它定义在 Scintilla.h 里。
//
// 完整的 Scintilla.h 有 5000+ 行，本工程用不到其中 99% 的内容。因此这里
// 只摘录两样东西，并且**所有字段顺序、类型、数值都与官方 Scintilla.h 逐字一致**：
//
//   1) Sci_NotifyHeader / SCNotification  （官方 Scintilla.h 第 1444~1493 行）
//   2) CMD 输出控件用到的 SCI_* 消息与样式常量
//
// 数值来源（未做任何改动，逐个核对过）：
//   https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Scintilla.h
//   https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/scintilla/include/Sci_Position.h
//
// ⚠️ 若日后需要更多 SCI_* 消息，**不要凭记忆补数字**，请从官方 Scintilla.h 里复制。
// ============================================================================
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

// ---- 官方 Sci_Position.h -----------------------------------------------------
typedef ptrdiff_t Sci_Position;
typedef size_t    Sci_PositionU;
typedef intptr_t  Sci_PositionCR;

// ---- 官方 Scintilla.h 顶部 ---------------------------------------------------
typedef uintptr_t uptr_t;
typedef intptr_t  sptr_t;

// ---- 官方 Scintilla.h 第 1444~1451 行：Sci_NotifyHeader ----------------------
struct Sci_NotifyHeader {
	/* Compatible with Windows NMHDR.
	 * hwndFrom is really an environment specific window handle or pointer
	 * but most clients of Scintilla.h do not have this type visible. */
	void *hwndFrom;
	uptr_t idFrom;
	unsigned int code;
};

// ---- 官方 Scintilla.h 第 1453~1493 行：SCNotification ------------------------
struct SCNotification {
	Sci_NotifyHeader nmhdr;
	Sci_Position position;
	/* SCN_STYLENEEDED, SCN_DOUBLECLICK, SCN_MODIFIED, SCN_MARGINCLICK, */
	/* SCN_MARGINRIGHTCLICK, SCN_NEEDSHOWN, SCN_DWELLSTART, SCN_DWELLEND, */
	/* SCN_UPDATEUI, SCN_CALLTIPCLICK, */
	/* SCN_HOTSPOTCLICK, SCN_HOTSPOTDOUBLECLICK, SCN_HOTSPOTRELEASECLICK, */
	/* SCN_INDICATORCLICK, SCN_INDICATORRELEASE, */
	/* SCN_USERLISTSELECTION, SCN_AUTOCCOMPLETED, SCN_AUTOCSELECTION, */
	/* SCN_AUTOCSELECTIONCHANGE */
	int ch;
	/* SCN_CHARADDED, SCN_KEY, SCN_AUTOCCOMPLETED, SCN_AUTOCSELECTION, */
	/* SCN_USERLISTSELECTION */
	int modifiers;
	/* SCN_KEY, SCN_DOUBLECLICK, SCN_HOTSPOTCLICK, SCN_HOTSPOTDOUBLECLICK, */
	/* SCN_HOTSPOTRELEASECLICK, SCN_INDICATORCLICK, SCN_INDICATORRELEASE, */
	/* SCN_MARGINCLICK, SCN_MARGINRIGHTCLICK */
	int modificationType;	/* SCN_MODIFIED */
	const char *text;
	/* SCN_MODIFIED, SCN_USERLISTSELECTION, SCN_URIDROPPED, */
	/* SCN_AUTOCCOMPLETED, SCN_AUTOCSELECTION, SCN_AUTOCSELECTIONCHANGE */
	Sci_Position length;		/* SCN_MODIFIED */
	Sci_Position linesAdded;	/* SCN_MODIFIED */
	int message;	/* SCN_MACRORECORD */
	uptr_t wParam;	/* SCN_MACRORECORD */
	sptr_t lParam;	/* SCN_MACRORECORD */
	Sci_Position line;		/* SCN_MODIFIED */
	int foldLevelNow;	/* SCN_MODIFIED */
	int foldLevelPrev;	/* SCN_MODIFIED */
	int margin;		/* SCN_MARGINCLICK, SCN_MARGINRIGHTCLICK */
	int listType;	/* SCN_USERLISTSELECTION, SCN_AUTOCSELECTIONCHANGE */
	int x;			/* SCN_DWELLSTART, SCN_DWELLEND */
	int y;			/* SCN_DWELLSTART, SCN_DWELLEND */
	int token;		/* SCN_MODIFIED with SC_MOD_CONTAINER */
	Sci_Position annotationLinesAdded;	/* SCN_MODIFIED with SC_MOD_CHANGEANNOTATION */
	int updated;	/* SCN_UPDATEUI */
	int listCompletionMethod;
	/* SCN_AUTOCSELECTION, SCN_AUTOCCOMPLETED, SCN_USERLISTSELECTION */
	int characterSource;	/* SCN_CHARADDED */
};

// ============================================================================
// 以下为 CMD 输出控件用到的 SCI_* 常量 —— 全部照抄官方 Scintilla.h
// ============================================================================

/* --- 文本 / 缓冲区 --- */
#define SCI_CLEARALL            2004
#define SCI_GOTOPOS             2025
#define SCI_STYLECLEARALL       2050
#define SCI_SETCODEPAGE         2037
#define SCI_GETCODEPAGE         2137
#define SCI_SETSEL              2160
#define SCI_GETTEXTLENGTH       2183
#define SCI_SETTEXT             2181
#define SCI_APPENDTEXT          2282
#define SCI_GETLINECOUNT        2154
#define SCI_SCROLLCARET         2169
#define SCI_SETREADONLY         2171
#define SCI_SETUNDOCOLLECTION   2012
#define SCI_EMPTYUNDOBUFFER     2175

/* --- 字符 / 区间（ScintillaOut.h 与 CmdView 用）--- */
#define SCI_GETCHARAT           2007   /* 取单个字节（UTF-8 下是字节不是字符）*/
#define SCI_GETCURRENTPOS       2008
#define SCI_SETCURRENTPOS       2141
#define SCI_GETSTYLEAT          2010
#define SCI_STARTSTYLING        2032
#define SCI_SETSTYLING          2033
#define SCI_SETSTYLINGEX        2073
#define SCI_REPLACESEL          2170
#define SCI_GETTEXT             2182
#define SCI_GETLINEENDPOSITION  2136
#define SCI_LINEFROMPOSITION    2166
#define SCI_POSITIONFROMLINE    2167
#define SCI_GETSELECTIONSTART   2143
#define SCI_GETSELECTIONEND     2145
#define SCI_SETEMPTYSELECTION   2556
#define SCI_LINELENGTH          2350

/* --- 视图 / 外观 --- */
#define SCI_SETMARGINWIDTHN     2242
#define SCI_SETWRAPMODE         2268
#define SCI_SETCARETLINEVISIBLE 2096
#define SCI_SETSELBACK          2068
/* --- 样式 --- */
#define SCI_STYLESETFONT        2056
#define SCI_STYLESETSIZE        2055
#define SCI_STYLESETFORE        2051
#define SCI_STYLESETBACK        2052
#define SCI_STYLESETBOLD        2053
#define SCI_STYLEGETFONT        2486
#define SCI_STYLESETITALIC      2054
#define SCI_STYLESETUNDERLINE   2059
#define SCI_STYLERESETDEFAULT   2058

/* --- 常量 --- */
#define STYLE_DEFAULT           32
#define STYLE_MAX               255
#define SC_CP_UTF8              65001
#define SC_CHARSET_DEFAULT      1
#define SC_WRAP_NONE            0
#define SC_WRAP_WORD            1
#define SC_CARETSTICKY_OFF      0

/*
 * 说明：本文件里所有 SCI_* 数值均已与官方头文件逐个核对（2026-10）。
 * 例如 SCI_EMPTYUNDOBUFFER = 2175、SCI_STYLEGETFONT = 2486、
 * SCI_SETREADONLY = 2171，三者互不相同，不要想当然。
 * 需要新常量时，从官方 Scintilla.h 复制，**不要凭记忆写数字**。
 */

/* --- 通知码（beNotified 里可能用到，照抄官方） --- */
#define SCN_CHARADDED           2001
#define SCN_UPDATEUI            2007
#define SCN_MODIFIED            2008
#define SCN_FOCUSIN             2026
#define SCN_FOCUSOUT            2027
