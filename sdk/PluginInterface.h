// This file is part of Notepad++ project
// Copyright (C)2025 Don HO don.h@free.fr
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

// ============================================================================
// NppDock 工程内备注（非官方原文）：
// 本文件为 Notepad++ 官方插件 SDK 的 PluginInterface.h 原文副本，来源：
//   https://github.com/npp-plugins/plugintemplate/blob/master/src/PluginInterface.h
//   （该仓库是 Notepad++ 官方维护的插件模板，文件本身随 Notepad++ 主仓库同步）
// 为适配本工程“不依赖 notepad++.exe 内注册的 Scintilla 窗口类”这一设计，
// 这里没有 #include "Scintilla.h"（官方原文有），原因是：
//   - Scintilla.h 体积极大，本工程只用其中的消息常量；
//   - SCNotification 在本工程只用到 nmhdr（见 CMake/构建脚本中的 -DSCN_ 精简定义）。
// 如需与官方逐字节一致，把下面这行注释掉的 include 打开即可（并附带官方 Scintilla.h）。
// 除此之外，本文件所有常量、结构体布局、函数签名均与官方原文一致，未做任何数值改动。
// ============================================================================

// For more comprehensive information on plugin communication, please refer to the following resource:
// https://npp-user-manual.org/docs/plugin-communication/
//
#pragma once

#include <windows.h>

// ---- 官方原文为 #include "Scintilla.h"; 本工程用精简版 SCNotification，见 sdk/NppDockScintilla.h ----

typedef const wchar_t * (__cdecl * PFUNCGETNAME)();

struct NppData
{
	HWND _nppHandle = nullptr;
	HWND _scintillaMainHandle = nullptr;
	HWND _scintillaSecondHandle = nullptr;
};

typedef void (__cdecl * PFUNCSETINFO)(NppData);
typedef void (__cdecl * PFUNCPLUGINCMD)();
typedef void (__cdecl * PBENOTIFIED)(SCNotification *);
typedef LRESULT (__cdecl * PMESSAGEPROC)(UINT Message, WPARAM wParam, LPARAM lParam);

struct ShortcutKey
{
	bool _isCtrl = false;
	bool _isAlt = false;
	bool _isShift = false;
	UCHAR _key = 0;
};

// 官方原文常量名。部分较老的副本里叫 CONTEXT_NAME_TCHAR / CONTEXT_ITEMCOUNT（同为 64）。
const int menuItemSize = 64;

struct FuncItem
{
	wchar_t _itemName[menuItemSize] = { '\0' };
	PFUNCPLUGINCMD _pFunc = nullptr;
	int _cmdID = 0;
	bool _init2Check = false;
	ShortcutKey *_pShKey = nullptr;
};

typedef FuncItem * (__cdecl * PFUNCGETFUNCSARRAY)(int *);

// You should implement (or define an empty function body) those functions which are called by Notepad++ plugin manager
extern "C" __declspec(dllexport) void setInfo(NppData);
extern "C" __declspec(dllexport) const wchar_t * getName();
extern "C" __declspec(dllexport) FuncItem * getFuncsArray(int *);
extern "C" __declspec(dllexport) void beNotified(SCNotification *);
extern "C" __declspec(dllexport) LRESULT messageProc(UINT Message, WPARAM wParam, LPARAM lParam);

// This API return always true now, since Notepad++ isn't compiled in ANSI mode anymore
extern "C" __declspec(dllexport) BOOL isUnicode();
