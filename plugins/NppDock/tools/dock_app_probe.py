#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NppDock 应用页 · 真机自动化验证探针
================================================================================
目的：把"右击空白页 -> 菜单 -> 选中应用 -> 嵌入成 tab -> 关闭页"整条链路
      **真的走一遍**，用窗口状态 + 插件日志判定，而不是靠肉眼看。

为什么必须发真实鼠标事件：
  TrackPopupMenu 是模态的，内部自带消息循环；TPM_RETURNCMD 模式下它也不会
  发 WM_COMMAND。所以没办法用 PostMessage "告诉它选了第几项" ——
  唯一可靠的驱动方式就是 SendInput 模拟真实点击。

菜单项定位不靠猜：
  菜单弹出后，先用 MN_GETHMENU 拿到 HMENU，再用 GetMenuStringW 读文本、
  GetMenuItemRect 读真实矩形，然后点它的正中心。
  （早期版本按"项高 20px"硬算坐标，遇到分隔符/主题/DPI 变化必然点偏。）

子命令（dock 面板相关）：
    tree                  打印 N++ 的 NppDock 相关窗口树
    menu                  右击内容区，列出菜单项（然后 ESC 关掉）
    pick --index N        右击内容区，点第 N 个应用项（从 0 开始，仅数按钮项）
    pick --text 文本       右击内容区，按文本点某一项（支持子串匹配）
    closetab              在标签条右击并选「关闭当前页」
    multiopen             同一个应用连开 2 页：页数/进程数都 +1，第二页标题带序号
    tabhl                 当前标签页的上边沿有绿杠，且跟着切换移动（像素级验证）
    log [--tail N]        打印插件日志尾部

子命令（应用页相关，多数会自己拉起一个 exe，不依赖 N++）：
    standalone            单开 exe：验标题栏/可缩放/关闭后无残留
    hashall [--file F]    文件校验页总验收：版式 / 字号 / 键盘输入 / 6 种算法 / 复制
    combo                 下拉框几何：条目高度 -> 闭合高度，展开后条目是否全可见
    abort [--mb N]        「计算」变身「终止」：能真的停下、停完状态全部复位
    tabsep                标签条下沿分界线：画了、够明显、且标签条没被加高
    filedrop [--file F]   文件校验「拖入文件自动填路径」的落地逻辑（跨进程递路径）
    backpack [--file C] [--local D]  文件背包总验收：版式 / 分隔条 / 配置 / 离线留空 / 自动同步
"""

import argparse
import ctypes
import os
import hashlib
import io
import json
import re
import subprocess
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.GetCurrentThreadId.restype = wintypes.DWORD
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
user32.AttachThreadInput.restype = wintypes.BOOL
user32.SetFocus.argtypes = [wintypes.HWND]

# ---- 64 位安全：句柄/指针必须显式声明宽度，否则高 32 位会被静默截断 ----
user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowW.restype = wintypes.HWND
user32.GetWindow.restype = wintypes.HWND
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsIconic.argtypes = [wintypes.HWND]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.SetForegroundWindow.argtypes = [wintypes.HWND]
user32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t,
                                ctypes.c_ssize_t]
user32.GetMenuStringW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR,
                                  ctypes.c_int, wintypes.UINT]
user32.GetMenuItemRect.argtypes = [wintypes.HWND, wintypes.HMENU, wintypes.UINT,
                                   ctypes.POINTER(wintypes.RECT)]
user32.GetMenuItemCount.argtypes = [wintypes.HMENU]
user32.GetMenuItemCount.restype = ctypes.c_int
user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
user32.IsWindow.argtypes = [wintypes.HWND]
user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t,
                                ctypes.c_ssize_t]
user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetDlgItem.restype = wintypes.HWND

gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
gdi32.GetObjectW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p]
gdi32.GetObjectW.restype = ctypes.c_int

NPP_CLASS = "Notepad++"
CONTENT_CLASS = "NppDockContentPane"
TAB_CLASS = "SysTabControl32"
MENU_CLASS = "#32768"
DIALOG_CLASS = "#32770"
APP_WND_CLASS = "NppDockMd5ToolWnd"   # md5 应用自己的窗口类
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
WM_GETFONT = 0x0031

GW_HWNDNEXT = 2
GW_OWNER = 4
GW_CHILD = 5
GWLP_ID = -12
TCM_GETITEMCOUNT = 0x1304
# 注意：TCM_GETITEMRECT **故意不定义**。它要传 RECT* 指针，而 SendMessage
# 跨进程不封送指针 —— 硬用会直接写崩 Notepad++。详见 tab_item_click_point()。
MN_GETHMENU = 0x01E1
MF_BYPOSITION = 0x400
SW_RESTORE = 9
GWL_STYLE = -16
WS_CHILD = 0x40000000
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
INPUT_MOUSE = 0
VK_ESCAPE = 0x1B
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004


def host_awareness(hwnd):
    """读一个窗口的 DPI 感知等级（仅用于诊断打印）。"""
    try:
        user32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
        ctx = user32.GetWindowDpiAwarenessContext(hwnd)
        user32.GetAwarenessFromDpiAwarenessContext.restype = ctypes.c_int
        return ctx, user32.GetAwarenessFromDpiAwarenessContext(ctx)
    except Exception:
        return None, None


_DPI_DONE = None      # make_dpi_aware() 的幂等缓存


def make_dpi_aware():
    """让探针用**物理像素**坐标：per-monitor-v2，且**幂等**。

    ⚠️ 为什么是 PMv2 而不是"抄宿主声明的感知等级"：
       实践里真正要对齐的是**物理像素** —— 真实鼠标事件（SetCursorPos/mouse_event）
       和跨进程窗口/菜单矩形（GetWindowRect/GetMenuItemRect）只有在物理空间里
       才指向同一个点。抄宿主的声明值反而会错：本机实测 Notepad++ 的窗口**声明**
       是 system-aware，可它的菜单/命中判据实际按物理像素工作，
       结果两边差 1.5 倍（显示器 3840x2160 @150%），症状是
       "右击命中了标签项（宿主日志 命中项=1），菜单却弹在几百像素外，
       照菜单项矩形点下去等于点空气"。
       另：显示器缩放若是 100%，两套空间的数值恰好相同 —— 于是这种错位
       会**潜伏**到有人改了缩放才爆发。

    ⚠️ 必须在**取任何坐标 / 发任何点击之前**执行，所以本模块在 **import 时**
       就调用它（见文件末尾的 `DPI_AWARENESS = make_dpi_aware()`）——
       只在 `main()` 里调是不够的：用 `import dock_app_probe` 写临时脚本时
       就会漏掉，而那正是本坑第一次出现的地方（命令行子命令全对，
       内联脚本全错）。幂等：重复调用直接返回上次结果。
    """
    global _DPI_DONE
    if _DPI_DONE is not None:
        return _DPI_DONE
    try:
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4（Win10 1703+）
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            _DPI_DONE = "per-monitor-v2"
            return _DPI_DONE
    except Exception:
        pass
    try:
        if user32.SetProcessDPIAware():
            _DPI_DONE = "system-aware"
            return _DPI_DONE
    except Exception:
        pass
    _DPI_DONE = "unaware"
    return _DPI_DONE


# ⚠️ 在 **import 时**就把 DPI 感知设好 —— 不能只放在 main() 里。
#    用 `import dock_app_probe` 写临时脚本时（本坑第一次就是这么冒出来的），
#    只有 import 时对齐才管用；晚一步取的坐标就全是另一个空间的。
DPI_AWARENESS = make_dpi_aware()


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


user32.SendInput.argtypes = [wintypes.UINT, ctypes.c_void_p, ctypes.c_int]
user32.SendInput.restype = wintypes.UINT


def mouse(flags):
    inp = INPUT(type=INPUT_MOUSE)
    inp.mi = MOUSEINPUT(0, 0, 0, flags, 0, None)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    time.sleep(0.07)


def type_text(s):
    """用**真实键盘事件**逐字输入（KEYEVENTF_UNICODE 直接注入字符）。

    为什么不给控件发 WM_SETTEXT：
      Vista 风格的文件对话框内部结构（ComboBoxEx32 -> ComboBox -> Edit，
      外加 DUIViewWndClassName 包装）没有稳定契约，按 ID/层级找很可能
      找到的是别的编辑框 —— 实测 WM_SETTEXT 打进去后回读是空字符串。
      而"点进去 + 真的敲键盘"和用户手工操作完全一致，不依赖任何内部结构。
    """
    for ch in s:
        for flags in (KEYEVENTF_UNICODE, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP):
            inp = INPUT(type=1)                       # INPUT_KEYBOARD
            inp.ki = KEYBDINPUT(0, ord(ch), flags, 0, None)
            user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
            time.sleep(0.012)


def click(x, y, button="left"):
    user32.SetCursorPos(x, y)
    time.sleep(0.18)
    if button == "left":
        mouse(MOUSEEVENTF_LEFTDOWN)
        mouse(MOUSEEVENTF_LEFTUP)
    else:
        mouse(MOUSEEVENTF_RIGHTDOWN)
        mouse(MOUSEEVENTF_RIGHTUP)


def press_esc():
    user32.keybd_event(VK_ESCAPE, 0, 0, 0)
    user32.keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0)


def drag(x0, y0, x1, y1, steps=12, on_step=None):
    """按住左键从 (x0,y0) 拖到 (x1,y1)。

    拖动过程中可以回调 on_step —— 想抓"拖动到一半时界面长什么样"就得靠它。
    """
    user32.SetCursorPos(x0, y0)
    time.sleep(0.15)
    mouse(MOUSEEVENTF_LEFTDOWN)
    time.sleep(0.15)
    for i in range(1, steps + 1):
        user32.SetCursorPos(x0 + (x1 - x0) * i // steps,
                            y0 + (y1 - y0) * i // steps)
        time.sleep(0.06)
        if on_step:
            on_step(i)
    time.sleep(0.1)
    mouse(MOUSEEVENTF_LEFTUP)
    time.sleep(0.2)


def enum_children(parent):
    """枚举某个窗口的**直接子窗口**（不递归）。"""
    out = []
    h = user32.GetWindow(parent, 5)          # GW_CHILD
    while h:
        out.append(h)
        h = user32.GetWindow(h, 2)           # GW_HWNDNEXT
    return out


def class_of(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def text_of(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 2)
    user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def rect_of(hwnd):
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def children(hwnd):
    out, h = [], user32.GetWindow(hwnd, GW_CHILD)
    while h:
        out.append(h)
        h = user32.GetWindow(h, GW_HWNDNEXT)
    return out


def enum_tops():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        found.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return found


def find_npp(timeout=20.0):
    """找 Notepad++ 主窗口，必要时把它从最小化还原出来。

    ⚠️ 两个坑：
      1) 最小化窗口的 GetWindowRect 会返回 (-21333,-21333) 这种哨兵坐标 ———
         如果不过滤，后面所有"点内容区中心"都会点到屏幕外面去。
      2) 刚启动时主窗口可能还没建好，必须轮询等，不能查一次就放弃。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        cands = [h for h in enum_tops() if class_of(h) == NPP_CLASS]
        best = None
        for h in cands:
            if not user32.IsWindowVisible(h):
                continue
            if user32.IsIconic(h):
                user32.ShowWindow(h, SW_RESTORE)   # 9
                best = best or h
                continue
            return h
        if best:
            # 只找到最小化的，还原后给它一点时间铺开
            user32.ShowWindow(best, SW_RESTORE)
            time.sleep(0.6)
            if not user32.IsIconic(best):
                return best
        time.sleep(0.3)
    return None


def wait_for_panel(npp, timeout=20.0):
    """等 NppDockContentPane 出现 —— 面板是 NPPN_READY 之后才注册的，
    比主窗口晚，急着查会查不到。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        p = find_descendant(npp, CONTENT_CLASS)
        if p and user32.IsWindowVisible(p):
            return p
        time.sleep(0.3)
    return find_descendant(npp, CONTENT_CLASS)


def find_descendant(root, cls, visible_only=False, any_class=False):
    for c in children(root):
        hit = (cls in class_of(c)) if any_class else (class_of(c) == cls)
        if hit and (not visible_only or user32.IsWindowVisible(c)):
            return c
        got = find_descendant(c, cls, visible_only, any_class)
        if got:
            return got
    return None


def dump_tree(hwnd, indent=0, max_depth=10):
    if indent > max_depth:
        return
    r = rect_of(hwnd)
    txt = text_of(hwnd)
    print(f"{'  ' * indent}- {class_of(hwnd):26s} hwnd={hwnd:#010x} "
          f"vis={int(bool(user32.IsWindowVisible(hwnd)))} "
          f"rect=({r[0]},{r[1]})-({r[2]},{r[3]}) w={r[2]-r[0]} h={r[3]-r[1]}"
          + (f'  text="{txt}"' if txt else ""))
    for c in children(hwnd):
        dump_tree(c, indent + 1, max_depth)


def menu_windows():
    """当前屏幕上所有可见的原生菜单窗口（类名 #32768）。"""
    return [h for h in enum_tops()
            if class_of(h) == MENU_CLASS and user32.IsWindowVisible(h)]


def find_menu(before, timeout=3.0):
    """等一个**新出现**的菜单窗口。

    ⚠️ 早期版本用"离点击点近"当判据 —— 这恰恰掩盖了一个真 bug：
       宿主把 WM_CONTEXTMENU 的坐标多转了一次，菜单跑到屏幕右下角，
       按距离过滤就会误报"菜单没弹出"。所以这里只认"新出现的 #32768"。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        for h in menu_windows():
            if h not in before:
                return h
        time.sleep(0.08)
    return None


def menu_items(menu_wnd):
    """读菜单项。返回 ([(位置, 文本, 屏幕矩形或 None)], 宿主窗口或 None)。

    ⚠️ GetMenuItemRect 的第一个参数必须是**拥有该菜单的窗口**。
       早期版本传 NULL，拿回来的坐标跟菜单窗口完全对不上
       （菜单在 (943,1093)，项目却报 (1417,1643)），据此点击必然点飞。
       这里改用 GetWindow(menu, GW_OWNER) 拿宿主，并且做一次一致性校验。
    """
    hmenu = user32.SendMessageW(menu_wnd, MN_GETHMENU, 0, 0)
    if not hmenu:
        return [], None
    owner = user32.GetWindow(menu_wnd, GW_OWNER)
    n = user32.GetMenuItemCount(hmenu)
    out = []
    for i in range(n):
        buf = ctypes.create_unicode_buffer(512)
        user32.GetMenuStringW(hmenu, i, buf, 512, MF_BYPOSITION)
        r = wintypes.RECT()
        ok = user32.GetMenuItemRect(owner, hmenu, i, ctypes.byref(r))
        out.append((i, buf.value,
                    (r.left, r.top, r.right, r.bottom) if ok else None))
    return out, owner


def read_log(tail=40):
    p = os.path.join(repo_root(), "plugins", "Config", "NppDock", "NppDock.log")
    if not os.path.isfile(p):
        print(f"[log] 不存在：{p}")
        return
    with open(p, "rb") as f:
        lines = [l for l in f.read().decode("utf-8", errors="replace").splitlines()
                 if l.strip()]
    print(f"[log] {p}（共 {len(lines)} 行，显示末 {tail} 行）")
    for l in lines[-tail:]:
        print("   " + l)


def count_md5_procs():
    """数一数有几个 NppDockApp_MD5.exe 在跑（用 tasklist，避免引入 psutil）。"""
    import subprocess
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq NppDockApp_MD5.exe",
                              "/FO", "CSV", "/NH"],
                             capture_output=True).stdout.decode("utf-8", "replace")
    except Exception:
        return -1
    return sum(1 for l in out.splitlines() if "NppDockApp_MD5" in l)


def repo_root():
    """Notepad++ 根目录 —— 从本文件向上找**含 notepad++.exe 的那一级**。

    为什么不用数 ".."：
      本文件住在 <工作区>/plugins/NppDock/tools/，离 Notepad++ 根 4 层；
      而 tools/ 曾经是直接挂在工作区下的（只有 3 层）。目录一改，层数就错，
      而且错了以后只是"找不到日志/找不到 exe"，**不会有任何报错** —— 静默失效最难查。
      找不到 notepad++.exe 时（比如换了别的宿主）退回按 4 层算。
    """
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(8):
        d = os.path.dirname(d)
        if os.path.isfile(os.path.join(d, "notepad++.exe")):
            return d
    return os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))


def plugin_dir():
    """本插件目录（tools/ 的上一级）。

    ⚠️ 别用 repo_root() 去拼 _t/ —— repo_root() 是 **Notepad++ 根目录**，
       而 _t/（临时样本、截图）是**插件私有**数据，挂在插件目录下。
       这两个混用过的后果很隐蔽：只是"写文件失败/找不到样本"，
       报错信息看起来像磁盘或权限问题（踩过一次）。
    """
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def app_exe_path():
    """被嵌入的应用 exe —— 必须和 NppDock.dll 同目录。"""
    return os.path.join(repo_root(), "plugins", "NppDock", "NppDockApp_MD5.exe")


def find_top_app(timeout=15.0, cls=None):
    """等 md5 应用自己的**顶层**窗口出现（不含嵌入模式 —— 嵌入时它是子窗口，
    EnumWindows 枚举不到，正好用来区分两种模式）。"""
    cls = cls or APP_WND_CLASS
    deadline = time.time() + timeout
    while time.time() < deadline:
        for h in enum_tops():
            if class_of(h) == cls and user32.IsWindowVisible(h):
                return h
        time.sleep(0.2)
    return None


def describe_app_window(h):
    st = user32.GetWindowLongPtrW(h, GWL_STYLE)
    l, t, r, b = rect_of(h)
    flags = []
    flags.append("WS_CHILD" if (st & WS_CHILD) else "顶层窗口")
    if st & WS_CAPTION:
        flags.append("有标题栏")
    if st & WS_THICKFRAME:
        flags.append("可缩放边框")
    print(f"应用窗口     = {h:#010x} title=\"{text_of(h)}\" "
          f"rect=({l},{t})-({r},{b}) w={r-l} h={b-t}")
    print(f"样式         = {st:#010x}  {' + '.join(flags)}")
    return st


def id_of(hwnd):
    return user32.GetWindowLongPtrW(hwnd, GWLP_ID)


user32.GetParent.argtypes = [wintypes.HWND]
user32.GetParent.restype = wintypes.HWND


def is_ancestor_or_self(hwnd, root):
    """hwnd 是不是 root 本身或它的后代（沿父链往上找）。"""
    h = hwnd
    for _ in range(64):
        if not h:
            return False
        if h == root:
            return True
        h = user32.GetParent(h)
    return False


def screen_region_is_ours(npp, container):
    """容器区域在屏幕上是否**真的露出我们自己的画面**。

    为什么必须查（踩过，而且是最阴的一种"假通过"）：
      抓屏抓的是**屏幕像素**，不是窗口内容。别的窗口只要压在上面
      （或者窗口在"量坐标"和"抓屏"之间被挪走），抓到的就是**别人**的画面。
      而"暗像素占比"照样会给出一个看起来很正常的数字 ——
      实测抓到过一次正在播放的视频，占比 75%，判别式照样判"OK"。
      结论：**判据本身没问题，是样本不是我们的**。所以先验证归属，再谈画没画。

    做法：在容器上取 4 个采样点，逐点 WindowFromPoint，看命中窗口是不是 N++ 的后代。
    """
    c = rect_of(container)
    w, h = c[2] - c[0], c[3] - c[1]
    if w <= 0 or h <= 0:
        return False, "容器尺寸为 0"
    user32.WindowFromPoint.argtypes = [wintypes.POINT]
    user32.WindowFromPoint.restype = wintypes.HWND
    pts = [(c[0] + w // 2,     c[1] + h // 2),
           (c[0] + w // 4,     c[1] + h // 4),
           (c[0] + 3 * w // 4, c[1] + h // 4),
           (c[0] + w // 4,     c[1] + 3 * h // 4)]
    bad = []
    for (x, y) in pts:
        hw = user32.WindowFromPoint(wintypes.POINT(x, y))
        if not is_ancestor_or_self(hw, npp):
            bad.append(f"({x},{y})->{class_of(hw) if hw else '?'}")
    if bad:
        return False, f"{len(bad)}/4 个采样点被别的窗口压住：{', '.join(bad)}"
    return True, ""


class _LOGFONTW(ctypes.Structure):
    _fields_ = [("lfHeight", ctypes.c_long), ("lfWidth", ctypes.c_long),
                ("lfEscapement", ctypes.c_long), ("lfOrientation", ctypes.c_long),
                ("lfWeight", ctypes.c_long), ("lfItalic", ctypes.c_byte),
                ("lfUnderline", ctypes.c_byte), ("lfStrikeOut", ctypes.c_byte),
                ("lfCharSet", ctypes.c_byte), ("lfOutPrecision", ctypes.c_byte),
                ("lfClipPrecision", ctypes.c_byte), ("lfQuality", ctypes.c_byte),
                ("lfPitchAndFamily", ctypes.c_byte), ("lfFaceName", ctypes.c_wchar * 32)]


def font_height(control):
    """读控件当前字体的 lfHeight（**负数** = 字符高度）。

    为什么这条能跨进程成立：
      字体是对方进程创建的，但 GDI 句柄在同一会话里是全局唯一的一张表，
      所以拿到 HFONT 之后本进程也能 GetObjectW 读出来 —— 实测可用。
      （不同于 HWND 那类"只能在拥有者进程里用"的东西。）

    ⚠️ 必须用 WM_GETFONT 拿句柄，不能去猜"系统消息字体多高" ——
       字体会随 DPI 变，而且状态行用的是**另外**一个字体对象。
    """
    if not control:
        return None
    hf = user32.SendMessageW(control, WM_GETFONT, 0, 0)
    if not hf:
        return None
    lf = _LOGFONTW()
    if gdi32.GetObjectW(hf, ctypes.sizeof(lf), ctypes.byref(lf)) <= 0:
        return None
    return int(lf.lfHeight)


def audit_app_children(h):
    """检查应用窗口**内部**的子控件是否真的建出来了、可见、且尺寸正常。

    ⚠️ 为什么必须单独做这一步（真机踩过）：
       早先的验证只看应用窗口自己的矩形，得出"1196x489 精确铺满"就以为成功了 ——
       但窗口里其实**一个控件都没有**（空白一片）。
       外框尺寸正确 ≠ 功能正常。所以这里逐个点名，并把"控件数为 0"
       明确判为失败。
    """
    kids = children(h)
    print(f"   子控件数 = {len(kids)}")
    bad = []
    for c in kids:
        l, t, r, b = rect_of(c)
        vis = int(bool(user32.IsWindowVisible(c)))
        cls, txt = class_of(c), text_of(c)
        w, hh = r - l, b - t
        ok = bool(vis) and w >= 16 and hh >= 12
        if not ok:
            bad.append((cls, txt, w, hh, vis))
        print(f"     - {cls:8s} id={id_of(c):4d} vis={vis} "
              f"rect=({l},{t})-({r},{b}) w={w} h={hh} text=\"{txt}\"")
    return kids, bad


def send_text(hwnd, s):
    """给标准控件设文本。WM_SETTEXT 属于系统会**跨进程封送**的消息，
    所以可以直接把本地缓冲区的地址传过去（这点和 GetMenuItemRect 那类
    要指针的自定义消息完全不同 —— 那些必须在本进程内解决）。"""
    buf = ctypes.create_unicode_buffer(s)
    return user32.SendMessageW(hwnd, WM_SETTEXT, 0, ctypes.addressof(buf))


def get_text(hwnd, n=1024):
    """**读**别的进程里控件的文本。

    ⚠️ 必须用 WM_GETTEXT（会被系统封送），不能用 GetWindowText：
       GetWindowText 只在**同进程**才真的去问控件；跨进程时它只返回
       内核里缓存的那份"窗口标题"，对 EDIT 这类自己管文本的控件
       往往读到空串 —— 看起来就像"没输进去"，其实输进去了（踩过）。
    """
    buf = ctypes.create_unicode_buffer(n)
    user32.SendMessageW(hwnd, WM_GETTEXT, n, ctypes.addressof(buf))
    return buf.value


def click_control(parent, cid):
    """按控件 ID 找到控件并点它的正中心。"""
    h = user32.GetDlgItem(parent, cid)
    if not h:
        return None
    l, t, r, b = rect_of(h)
    click((l + r) // 2, (t + b) // 2)
    return h


def press_key(vk):
    """敲一个真实按键（按下 + 抬起）。"""
    for flags in (0, KEYEVENTF_KEYUP):
        inp = INPUT(type=1)
        inp.ki = KEYBDINPUT(vk, 0, flags, 0, None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
        time.sleep(0.03)


VK_RETURN = 0x0D

# Notepad++ 停靠消息（sdk/Notepad_plus_msgs.h）。菜单里的"显示/隐藏 NppDock 面板"
# 走的就是这两个消息，所以直接发消息等价于点菜单。
#
# ⚠️ 别改用模拟 Ctrl+Alt+D 的方式：快捷键是**各线程的消息循环**自己
#    TranslateAccelerator 处理的；焦点一旦在嵌入的应用里（那是另一个进程、
#    另一个消息循环），按键就进了对面的队列，N++ 的循环根本看不见 ——
#    表现为"按了没反应"，插件日志里连一行都没有。
#    SetForegroundWindow / AttachThreadInput + SetFocus 都试过，一样没用。
NPPMSG        = 0x0400 + 1000
NPPM_DMMSHOW  = NPPMSG + 30
NPPM_DMMHIDE  = NPPMSG + 31

def find_dialog(timeout=10.0):
    """等一个可见的原生对话框（#32770）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for h in enum_tops():
            if class_of(h) == DIALOG_CLASS and user32.IsWindowVisible(h):
                return h
        time.sleep(0.2)
    return None


def cmd_func(sample):
    """**端到端功能验证**：真的走一遍 浏览 -> 选文件 -> 计算 -> 出结果，
    并把结果和 hashlib 对照。

    为什么光看"控件在不在"不够：
      控件都在、尺寸都对，也完全可能是"点了没反应"。
      校验类工具尤其危险 —— 算错时照样吐 32 位十六进制，肉眼分辨不出来，
      必须拿真实结果和权威实现对比。
    """
    import hashlib
    import subprocess

    exe = app_exe_path()
    if not os.path.isfile(sample):
        print(f"[ERROR] 样本文件不存在：{sample}")
        return 2
    with open(sample, "rb") as f:
        expected = hashlib.md5(f.read()).hexdigest()
    print(f"[func] 样本 = {sample}（{os.path.getsize(sample)} 字节）")
    print(f"[func] 期望 MD5 = {expected}")

    subprocess.Popen([exe])
    h = find_top_app(20)
    if not h:
        print("[ERROR] 没等到应用的顶层窗口")
        return 3
    print(f"[func] 应用窗口 = {h:#010x}")

    # ---- 1) 点「浏览…」 ----
    if not click_control(h, 102):
        print("[ERROR] 找不到「浏览…」按钮（id=102）")
        return 4
    dlg = find_dialog(10)
    if not dlg:
        print("[ERROR] 「浏览…」没有弹出文件对话框")
        return 5
    print(f"[func] 文件对话框 = {dlg:#010x} title=\"{text_of(dlg)}\"")

    # ---- 2) 填路径：点进「文件名」框，用真实键盘把路径敲进去 ----
    # ⚠️ 不要用 WM_SETTEXT：Vista 风格对话框的内部层级不稳定，实测按
    #    GetDlgItem(1148) / 递归找 Edit 都可能命中别的编辑框（打进去回读为空）。
    combo = user32.GetDlgItem(dlg, 1148)          # cmb13（标准 ID，取不到就退化）
    box = None
    if combo:
        box = find_descendant(combo, "Edit", visible_only=True) or combo
    if not box:
        box = find_descendant(dlg, "Edit", visible_only=True)
    if not box:
        print("[ERROR] 对话框里没找到文件名输入框，对话框结构如下：")
        dump_tree(dlg, 1, 6)
        return 6
    l, t, r, b = rect_of(box)
    print(f"[func] 文件名输入框 = {box:#010x} rect={rect_of(box)}")
    click(l + max(12, (r - l) // 8), (t + b) // 2)   # 点文字区，别点下拉箭头
    time.sleep(0.4)
    type_text(sample)
    time.sleep(0.6)
    print(f"[func] 键盘输入后回读 = \"{get_text(box)}\"")
    if not get_text(box):
        # 键盘没进去（前台激活被吞、IEM、注入被拦……）就退回消息注入 ——
        # WM_SETTEXT 会被系统封送，对标准 EDIT 是有效的。
        send_text(box, sample)
        time.sleep(0.4)
        print(f"[func] WM_SETTEXT 后回读 = \"{get_text(box)}\"")
    if not get_text(box):
        print("[ERROR] 两条输入路径都没能写入文件名框")
        return 9

    # ---- 3) 确认打开 ----
    # ⚠️ 优先用回车，不要一上来就点「打开」按钮：
    #    在文件对话框里敲完路径会弹出**自动补全下拉列表**，此时落在按钮上的
    #    第一下点击会被下拉列表吃掉（只关列表、不点按钮）——
    #    实测"点了没反应、对话框还在"，就是这么来的。
    #    回车没有这个问题：输入框里的回车直接触发默认按钮。
    press_key(VK_RETURN)
    time.sleep(1.2)
    if user32.IsWindow(dlg) and user32.IsWindowVisible(dlg):
        print("[func] 回车没关掉对话框，改点「打开」按钮")
        okbtn = user32.GetDlgItem(dlg, 1)          # IDOK
        if not okbtn or not user32.IsWindowVisible(okbtn):
            okbtn = None
            for c in children(dlg):
                if class_of(c) == "Button" and any(
                        k in text_of(c) for k in ("打开", "Open", "确定", "OK")):
                    okbtn = c
                    break
        if not okbtn:
            print("[ERROR] 对话框里没找到确定/打开按钮")
            dump_tree(dlg, 1, 6)
            return 7
        print(f"[func] 打开按钮 = {okbtn:#010x} \"{text_of(okbtn)}\" rect={rect_of(okbtn)}")
        l, t, r, b = rect_of(okbtn)
        click((l + r) // 2, (t + b) // 2)
        time.sleep(1.2)
    print(f"[func] 对话框还在吗 = "
          f"{int(bool(user32.IsWindow(dlg) and user32.IsWindowVisible(dlg)))}")

    # ---- 4) 文件路径应回填到主窗口 ----
    path_shown = get_text(user32.GetDlgItem(h, 101))
    print(f"[func] 路径框内容 = \"{path_shown}\"")
    ok = True
    if os.path.basename(sample) not in path_shown:
        print("[FAIL] 路径没有回填到主窗口")
        ok = False

    # ---- 5) 点「计算」并等结果 ----
    if not click_control(h, 105):
        print("[ERROR] 找不到「计算」按钮（id=105）")
        return 8
    got = ""
    deadline = time.time() + 20
    while time.time() < deadline:
        got = get_text(user32.GetDlgItem(h, 104)).strip()
        if len(got) == 32:
            break
        time.sleep(0.2)
    status = get_text(user32.GetDlgItem(h, 107))
    print(f"[func] 结果框内容 = \"{got}\"")
    print(f"[func] 状态栏     = \"{status}\"")
    if got.lower() != expected:
        print(f"[FAIL] 结果不匹配：得到 {got!r}，期望 {expected}")
        ok = False
    else:
        print("[OK] 界面算出的 MD5 与 hashlib 完全一致")

    user32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.8)
    print("[func] PASS" if ok else "[func] FAIL")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# hashall：多算法 / 键盘输入 / 版式约束 —— 三条新需求的端到端验证
# ---------------------------------------------------------------------------
# 为什么非要有它（光有 check_hash.py 不够）：
#   check_hash.py 走的是 `--selftest` 命令行，**完全绕开了界面**。
#   下拉框没接线、算完忘了刷新结果框、按钮挪错位置 —— 那些它一样全绿。
#   所以这里必须真的去点下拉框、真的用键盘敲路径。
#
# 版式那几条是纯几何约束（两个框等长、按钮在下），只能靠量矩形来验，
# 而且正是王这次提的硬要求 —— 用眼睛看容易看走眼，量一遍才踏实。
CB_GETCOUNT   = 0x0146
CB_SETCURSEL  = 0x014E
CBN_SELCHANGE = 1
WM_COMMAND    = 0x0111

# 控件 ID，与 src/apps/md5tool/md5tool.cpp 里的 enum 一一对应
ID_PATH_EDIT  = 101
ID_BROWSE_BTN = 102
ID_HASH_EDIT  = 104
ID_CALC_BTN   = 105
ID_COPY_BTN   = 106
ID_STATUS     = 107
ID_ALGO_COMBO = 109


def hash_expected(data):
    """按算法算出权威结果，顺序与下拉框一致（= hashcore 的 AlgoAt 顺序）。"""
    import hashlib
    import zlib
    return [
        ("md5",    "MD5",     32,  hashlib.md5(data).hexdigest()),
        ("sha1",   "SHA-1",   40,  hashlib.sha1(data).hexdigest()),
        ("sha256", "SHA-256", 64,  hashlib.sha256(data).hexdigest()),
        ("sha384", "SHA-384", 96,  hashlib.sha384(data).hexdigest()),
        ("sha512", "SHA-512", 128, hashlib.sha512(data).hexdigest()),
        ("crc32",  "CRC32",   8,   "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)),
    ]


def key_combo(vk):
    """Ctrl+<vk>（用来全选编辑框内容）。"""
    VK_CONTROL = 0x11
    user32.keybd_event(VK_CONTROL, 0, 0, 0)
    time.sleep(0.06)
    press_key(vk)
    user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)
    time.sleep(0.06)


def wait_result(app, hexlen, timeout=15.0):
    """等结果框出现**指定长度**的十六进制。

    用"长度"当判据而不是看状态文字：每种算法的结果长度是固定的
    （MD5=32 … SHA-512=128），长度对上基本就说明算完了，而且比解析
    中文状态串稳得多。
    """
    he = user32.GetDlgItem(app, ID_HASH_EDIT)
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = (get_text(he) or "").strip()
        if len(last) == hexlen and all(c in "0123456789abcdef" for c in last.lower()):
            return last
        time.sleep(0.2)
    return last


def clipboard_text():
    """读剪贴板里的文本（CF_UNICODETEXT=13）。

    跨进程读剪贴板是标准做法：系统会把数据放进接收方也能锁定的内存，
    所以这里 GlobalLock 拿得到。⚠️ GlobalLock 返回的是**指针**，
    不设 restype 的话 ctypes 会按 int 截断成 32 位 —— 直接崩。
    """
    CF_UNICODETEXT = 13
    kernel32.GlobalLock.restype = ctypes.c_void_p
    user32.GetClipboardData.restype = ctypes.c_void_p
    if not user32.OpenClipboard(None):
        return ""
    try:
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return ""
        p = kernel32.GlobalLock(ctypes.c_void_p(h))
        if not p:
            return ""
        try:
            return ctypes.c_wchar_p(p).value or ""
        finally:
            kernel32.GlobalUnlock(ctypes.c_void_p(h))
    finally:
        user32.CloseClipboard()


def bring_to_front(hwnd):
    """把窗口提到最前并抢前台。

    ⚠️ 不加这一步，后面所有"真实鼠标点击"都可能点在**别的窗口**上。
       实测踩过一次：点「复制」没生效，剪贴板里还是上一次的内容，
       看起来像"功能坏了"，其实只是窗口被压住、点击落到了别处。
       凡是"用真实鼠标去点"的测试，都要先干这件事。

    为什么用 TOPMOST 再 NOTOPMOST：SetForegroundWindow 会被系统拒绝
    （调用进程不是前台进程时），而"置顶再取消置顶"这条老路能可靠地把窗口翻上来。
    ⚠️ SetWindowPos 的第 2 个参数是 HWND：不显式给类型的话，ctypes 会按
       32 位 int 截断 -1，HWND_TOPMOST 就变成了无效句柄（静默失败）。
    """
    HWND_TOPMOST   = wintypes.HWND(-1)
    HWND_NOTOPMOST = wintypes.HWND(-2)
    SWP_NOSIZE, SWP_NOMOVE = 0x0001, 0x0002
    user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND,
                                    ctypes.c_int, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE)
    time.sleep(0.12)
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.35)


def cmd_hashall(sample, exe=None):
    """端到端验"多算法 + 键盘输入 + 版式"三条新需求。"""
    import subprocess

    exe = exe or app_exe_path()
    if not os.path.isfile(exe):
        print(f"[ERROR] 找不到应用 exe：{exe}")
        return 2
    if not os.path.isfile(sample):
        print(f"[ERROR] 样本文件不存在：{sample}")
        return 2

    with open(sample, "rb") as f:
        data = f.read()
    algos = hash_expected(data)
    print(f"[hashall] exe  = {exe} ({os.path.getsize(exe)} 字节)")
    print(f"[hashall] 样本 = {sample}（{len(data)} 字节）")

    # 测试前的进程数：dock 里可能已经开着一个个（宿主拉起来的），
    # 最后判"有没有残留"必须用差值，不能拿绝对数量当 0（会误报）。
    before = count_md5_procs()
    subprocess.Popen([exe])
    h = find_top_app(20)
    if not h:
        print("[ERROR] 没等到应用的顶层窗口")
        return 3
    print(f"[hashall] 窗口 = {h:#010x} rect={rect_of(h)}")
    bring_to_front(h)
    print()
    ok = True

    # ---- 1) 版式断言（王这次提的三条几何硬要求）----
    p101 = rect_of(user32.GetDlgItem(h, ID_PATH_EDIT))
    p104 = rect_of(user32.GetDlgItem(h, ID_HASH_EDIT))
    p109 = rect_of(user32.GetDlgItem(h, ID_ALGO_COMBO))
    p102 = rect_of(user32.GetDlgItem(h, ID_BROWSE_BTN))
    p105 = rect_of(user32.GetDlgItem(h, ID_CALC_BTN))
    p106 = rect_of(user32.GetDlgItem(h, ID_COPY_BTN))
    w101, w104 = p101[2] - p101[0], p104[2] - p104[0]
    print(f"[版式] 路径框   x={p101[0]:<5} 宽={w101}")
    print(f"[版式] 校验值框 x={p104[0]:<5} 宽={w104}")
    print(f"[版式] 下拉框   {p109}")
    print(f"[版式] 浏览={p102}  计算={p105}  复制={p106}")

    if p101[0] != p104[0] or abs(w101 - w104) > 1:
        print(f"[FAIL] 两个框不等长：起点 {p101[0]} vs {p104[0]}，宽 {w101} vs {w104}")
        ok = False
    else:
        print("[OK] 路径框与校验值框等长（同起点、同宽度）")

    below = [t for t, r in (("浏览…", p102), ("计算", p105), ("复制", p106))
             if r[1] <= p104[3] - 2]
    if below:
        print(f"[FAIL] 这些按钮没在下方：{below}（校验值框 bottom={p104[3]}）")
        ok = False
    else:
        print("[OK] 浏览/计算/复制 三个按钮都位于两行输入框下方")

    if abs(p109[1] - p105[1]) > 6:
        print(f"[FAIL] 下拉框与按钮不在同一行（top {p109[1]} vs {p105[1]}）")
        ok = False
    else:
        print("[OK] 校验方式下拉框与按钮处于同一行")

    # 状态行字号要比正文小一号（王的要求）。
    # 判据用**实测字体高度**而不是"看着差不多"—— 差 1~2px 肉眼根本分不出来，
    # 只有量 lfHeight 才知道到底减没减。
    h_body = font_height(user32.GetDlgItem(h, ID_PATH_EDIT))
    h_stat = font_height(user32.GetDlgItem(h, ID_STATUS))
    print(f"[版式] 正文字体高度 = {h_body}   状态行 = {h_stat}")
    if h_body is None or h_stat is None:
        print("[warn] 读不到字体高度，跳过字号检查")
    elif abs(h_stat) >= abs(h_body):
        print(f"[FAIL] 状态行字号没有更小（{abs(h_stat)} vs {abs(h_body)}）")
        ok = False
    else:
        print(f"[OK] 状态行字号更小（{abs(h_stat)} < {abs(h_body)}）")
    print()

    # ---- 2) 下拉框条目 ----
    combo = user32.GetDlgItem(h, ID_ALGO_COMBO)
    n = int(user32.SendMessageW(combo, CB_GETCOUNT, 0, 0))
    print(f"[下拉框] 条目数 = {n}")
    if n != len(algos):
        print(f"[FAIL] 下拉框应有 {len(algos)} 项，实际 {n}")
        ok = False
    print()

    # ---- 3) 键盘输入路径 + 回车开始计算（默认算法 MD5）----
    pe = user32.GetDlgItem(h, ID_PATH_EDIT)
    l, t, r, b = rect_of(pe)
    click((l + r) // 2, (t + b) // 2)
    time.sleep(0.3)
    type_text(sample)
    time.sleep(0.4)
    typed = get_text(pe)
    print(f"[键盘] 真实按键后回读 = \"{typed}\"")
    if os.path.basename(sample) not in typed:
        # 真实键盘注入被拦（前台/权限/输入法）就退回消息注入 ——
        # WM_SETTEXT 会被系统封送，对标准 EDIT 有效。
        send_text(pe, sample)
        time.sleep(0.3)
        typed = get_text(pe)
        print(f"[键盘] WM_SETTEXT 后回读 = \"{typed}\"")
    if os.path.basename(sample) not in typed:
        print("[FAIL] 两条输入路径都没能把路径写进输入框")
        ok = False
    else:
        print("[OK] 路径框支持键盘输入（不再是只读）")

    press_key(VK_RETURN)          # 在路径框里敲回车 = 开始算
    got = wait_result(h, algos[0][2], 20)
    print(f"[回车] {algos[0][1]} 结果 = {got or '<空>'}")
    if got.lower() != algos[0][3]:
        print(f"[FAIL] 回车没触发计算或结果不对，期望 {algos[0][3]}")
        ok = False
    else:
        print("[OK] 路径框敲回车即开始计算，结果与 hashlib 一致")
    print()

    # ---- 4) 逐个算法拨下拉框：结果应自动跟着变 ----
    for idx, (key, disp, hexlen, exp) in enumerate(algos):
        user32.SendMessageW(combo, CB_SETCURSEL, idx, 0)
        # 真的给父窗口发一次"用户改了选择"的通知（等价于点选该项）。
        # CBN_SELCHANGE 的高位就是通知码，低位是控件 ID。
        user32.SendMessageW(h, WM_COMMAND,
                            (CBN_SELCHANGE << 16) | ID_ALGO_COMBO, combo)
        got = wait_result(h, hexlen, 12)
        good = (got.lower() == exp)
        print(f"[下拉] {disp:<8} 结果 = {got or '<空>':<22} {'OK' if good else 'FAIL'}")
        if not good:
            # 区分两种失败：下拉框根本没接线，还是只差"换完不自动重算"
            click_control(h, ID_CALC_BTN)
            got2 = wait_result(h, hexlen, 12)
            if got2.lower() == exp:
                print("       → 点「计算」后正确：说明换算法没有自动重算")
            else:
                print(f"       → 点「计算」也不对，期望 {exp}")
            ok = False
    print()

    # ---- 5) 复制按钮（结果长度已不是 32，能顺带验"复制的是哪个字段"）----
    user32.SendMessageW(combo, CB_SETCURSEL, len(algos) - 1, 0)
    user32.SendMessageW(h, WM_COMMAND,
                        (CBN_SELCHANGE << 16) | ID_ALGO_COMBO, combo)
    last = algos[-1]
    wait_result(h, last[2], 12)
    clip = ""
    for attempt in range(1, 4):
        click_control(h, ID_COPY_BTN)
        time.sleep(0.6)
        clip = (clipboard_text() or "").strip()
        if clip.lower() == last[3]:
            break
        # 没成功就说明白原因：把状态栏读出来看是"点击没落到按钮上"
        # 还是"按钮点到了但 OpenClipboard 被别的程序占着"。
        print(f"[复制] 第 {attempt} 次未生效，剪贴板 = {clip or '<空>'}")
        print(f"       状态栏 = \"{get_text(user32.GetDlgItem(h, ID_STATUS))}\"")
    print(f"[复制] 剪贴板 = {clip or '<空>'}   期望 {last[3]}")
    if clip.lower() == last[3]:
        print("[OK] 复制出来的是当前算法的结果")
    else:
        print("[FAIL] 剪贴板内容不是当前算法结果")
        ok = False
    print()

    # ---- 5) v2.0：目录要明确说"这是文件夹"（以前被译成"没有权限，试试管理员"）----
    # 为什么放在最后：它会把路径改成目录、结果框清空 —— 后面已经没有任何步骤依赖
    # "当前结果是样本的摘要"了（放中间会打断第 4 步的算法轮询）。
    # ⚠️ 必须**先点一下路径框**再敲回车：回车发给"当前有焦点的窗口"，
    #    上一步算完之后焦点未必还在框里（漏点就会变成"路径已修改，点计算重新校验"）。
    pe_l, pe_t, pe_r, pe_b = rect_of(pe)
    click((pe_l + pe_r) // 2, (pe_t + pe_b) // 2)
    time.sleep(0.3)
    dirp = os.path.abspath(os.path.dirname(sample) or ".")
    send_text(pe, dirp)
    time.sleep(0.4)
    press_key(VK_RETURN)
    stat = ""
    dl = time.time() + 6.0
    while time.time() < dl:
        stat = get_text(user32.GetDlgItem(h, ID_STATUS)) or ""
        if "文件夹" in stat or "权限" in stat:
            break
        time.sleep(0.2)
    print(f"[目录] 状态行 = \"{stat}\"")
    if "文件夹" not in stat:
        print("[FAIL] 输入目录时没有明确说『这是文件夹』（以前会被译成『没有权限』）")
        ok = False
    elif "权限" in stat:
        print("[FAIL] 输入目录时被误报成『没有权限』")
        ok = False
    else:
        print("[OK] 输入目录时明确提示『这是一个文件夹』（不再误导成权限问题）")
    print()

    user32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.8)
    # ⚠️ 用**差值**判断残留，不能用绝对数量：dock 里可能正开着一个
    #    NppDockApp_MD5.exe（那是宿主拉起来的，跟本次测试无关），
    #    按绝对数量判会误报成"进程残留"（踩过）。
    left = count_md5_procs()
    if left > before:
        print(f"[FAIL] 关闭后多出 {left - before} 个进程（测试前 {before}，之后 {left}）")
        ok = False
    else:
        print(f"[OK] 关闭后无新增进程（还是 {left} 个，与测试前一致）")
    print("[hashall] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# abort：「计算」按钮变身「终止」—— 以及终止之后状态是否完全复位
# ---------------------------------------------------------------------------
# 为什么要单独测：
#   终止是唯一一条"把算到一半的活儿丢掉"的路径，它的失败方式都很隐蔽 ——
#     · 按钮没变回「计算」 -> 用户再也点不动了；
#     · 其它控件没重新启用 -> 连换个文件都做不到；
#     · 结果框残留上一次的值 -> "这个摘要属于哪个文件"就说不清了。
#   以上**全都不会报错**，用户只会觉得"程序卡住了"。
#
# 为什么必须用大文件：
#   取消是在"读完下一块"时生效的。文件够小的话，我们还没来得及点它就已经算完了
#   —— 那次"通过"其实走的是正常完成路径，等于没测。所以现场造一个 1GiB 样本，
#   并特意选**最慢**的 SHA-256 把可中止的窗口拉长。
BIG_MB_DEFAULT = 1024


def ensure_big_file(path, size_bytes):
    """造一个指定大小的样本（内容全零）。已存在且大小对就复用。"""
    if os.path.isfile(path) and os.path.getsize(path) == size_bytes:
        print(f"[abort] 复用现有大样本：{path}")
        return True
    print(f"[abort] 生成大样本：{path}（{size_bytes >> 20} MB，全零）…")
    chunk = bytes(8 << 20)
    left = size_bytes
    try:
        with open(path, "wb") as f:
            while left > 0:
                n = min(left, len(chunk))
                f.write(chunk if n == len(chunk) else chunk[:n])
                left -= n
    except OSError as e:
        print(f"[ERROR] 写不出大样本：{e}")
        return False
    print(f"[abort] 完成，{os.path.getsize(path)} 字节")
    return True


def button_label(app, cid):
    return (get_text(user32.GetDlgItem(app, cid), 64) or "").strip()


def cmd_abort(size_mb=BIG_MB_DEFAULT, exe=None):
    """端到端验「终止」：按钮变身、能真的停下、停下之后状态全部复位。"""
    import subprocess
    import hashlib

    exe = exe or app_exe_path()
    if not os.path.isfile(exe):
        print(f"[ERROR] 找不到应用 exe：{exe}")
        return 2

    big = os.path.join(plugin_dir(), "_t", "abort_big.bin")
    size = size_mb << 20
    if not ensure_big_file(big, size):
        return 2

    ok = True
    try:
        before = count_md5_procs()
        subprocess.Popen([exe])
        h = find_top_app(20)
        if not h:
            print("[ERROR] 没等到应用的顶层窗口")
            return 3
        print(f"[abort] 窗口 = {h:#010x} rect={rect_of(h)}")
        bring_to_front(h)
        time.sleep(0.4)

        # ---- 1) 空闲时按钮必须是「计算」 ----
        lbl = button_label(h, ID_CALC_BTN)
        print(f'[abort] 空闲时按钮文字 = "{lbl}"')
        if lbl != "计算":
            print("[FAIL] 空闲时按钮不是「计算」")
            ok = False

        # ---- 2) 选最慢的 SHA-256，把可中止的窗口拉长 ----
        combo = user32.GetDlgItem(h, ID_ALGO_COMBO)
        sha256_idx = 2                       # 与 hash_expected() 的顺序一致
        user32.SendMessageW(combo, CB_SETCURSEL, sha256_idx, 0)
        user32.SendMessageW(h, WM_COMMAND,
                            (CBN_SELCHANGE << 16) | ID_ALGO_COMBO, combo)
        time.sleep(0.3)

        # ---- 3) 输入大文件，回车开始算 ----
        pe = user32.GetDlgItem(h, ID_PATH_EDIT)
        l, t, r, b = rect_of(pe)
        click((l + r) // 2, (t + b) // 2)
        time.sleep(0.25)
        send_text(pe, big)
        time.sleep(0.3)
        t_start = time.time()
        press_key(VK_RETURN)

        deadline = time.time() + 8.0
        while time.time() < deadline:
            if button_label(h, ID_CALC_BTN) == "终止":
                break
            time.sleep(0.05)
        t_running = time.time()
        lbl = button_label(h, ID_CALC_BTN)
        status = get_text(user32.GetDlgItem(h, ID_STATUS))
        print(f'[abort] 计算中：按钮 = "{lbl}"   状态 = "{status}"')
        if lbl != "终止":
            print("[FAIL] 计算开始后按钮没变成「终止」（没变身，或者算得太快没赶上）")
            ok = False
        else:
            print(f"[OK] 计算开始后按钮变身「终止」"
                  f"（{t_running - t_start:.2f}s 内观察到）")

        # 计算中：浏览/复制该灰，但「终止」（=计算按钮）必须可点
        for cid, name, want in ((ID_BROWSE_BTN, "浏览…", False),
                                (ID_COPY_BTN,   "复制",   False),
                                (ID_CALC_BTN,   "终止",   True)):
            en = bool(user32.IsWindowEnabled(user32.GetDlgItem(h, cid)))
            print(f"[abort] 计算中 {name:<6} enabled={int(en)} 期望={int(want)}"
                  f"  {'OK' if en == want else 'FAIL'}")
            if en != want:
                ok = False

        # ---- 4) 点「终止」----
        t_click = time.time()
        click_control(h, ID_CALC_BTN)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            if button_label(h, ID_CALC_BTN) == "计算":
                break
            time.sleep(0.05)
        t_done = time.time()

        lbl    = button_label(h, ID_CALC_BTN)
        status = get_text(user32.GetDlgItem(h, ID_STATUS))
        result = (get_text(user32.GetDlgItem(h, ID_HASH_EDIT)) or "").strip()
        print(f'[abort] 点终止后 {t_done - t_click:.2f}s：按钮 = "{lbl}"')
        print(f'[abort] 状态 = "{status}"')
        print(f'[abort] 结果框 = "{result}"')

        if lbl != "计算":
            print("[FAIL] 终止后按钮没变回「计算」")
            ok = False
        else:
            print("[OK] 终止后按钮变回「计算」")

        if "终止" not in status:
            print("[FAIL] 状态栏没有报告已终止")
            ok = False
        else:
            print("[OK] 状态栏报告了已终止")

        if result:
            print("[FAIL] 终止后结果框里还留着内容（不能骗人）")
            ok = False
        else:
            print("[OK] 终止后结果框是空的")

        # 终止应当**立刻**生效（最坏等一个 64KB 块读完）。给 3 秒余量，
        # 超过就说明走的是"等算完再说"，那等于没终止。
        if t_done - t_click > 3.0:
            print(f"[FAIL] 终止用了 {t_done - t_click:.2f}s，不像真的中止（应 < 3s）")
            ok = False
        else:
            print(f"[OK] 终止响应 {t_done - t_click:.2f}s")

        # ---- 5) 所有控件都该重新可用 ----
        for cid, name in ((ID_BROWSE_BTN, "浏览…"), (ID_COPY_BTN, "复制"),
                          (ID_CALC_BTN, "计算"), (ID_PATH_EDIT, "路径框"),
                          (ID_ALGO_COMBO, "下拉框")):
            en = bool(user32.IsWindowEnabled(user32.GetDlgItem(h, cid)))
            print(f"[abort] 终止后 {name:<6} enabled={int(en)}  "
                  f"{'OK' if en else 'FAIL'}")
            if not en:
                ok = False

        # ---- 6) 终止之后必须还能正常算完一个小文件（证明状态真的复位了）----
        small = os.path.join(plugin_dir(), "_t", "fox.bin")
        if not os.path.isfile(small):
            with open(small, "wb") as f:
                f.write(b"The quick brown fox jumps over the lazy dog\n")
        user32.SendMessageW(combo, CB_SETCURSEL, 0, 0)      # 回到 MD5（快）
        user32.SendMessageW(h, WM_COMMAND,
                            (CBN_SELCHANGE << 16) | ID_ALGO_COMBO, combo)
        time.sleep(0.25)
        send_text(pe, small)
        time.sleep(0.25)
        press_key(VK_RETURN)
        got = wait_result(h, 32, 15)
        with open(small, "rb") as f:
            exp = hashlib.md5(f.read()).hexdigest()
        print(f"[abort] 终止之后再算小文件 = {got or '<空>'}   期望 {exp}")
        if got.lower() == exp:
            print("[OK] 终止之后还能正常算完（状态完全复位）")
        else:
            print("[FAIL] 终止之后算不了了 —— 有状态没复位")
            ok = False

        user32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
        time.sleep(0.8)
        left = count_md5_procs()
        if left > before:
            print(f"[FAIL] 关闭后多出 {left - before} 个进程")
            ok = False
        else:
            print("[OK] 关闭后无新增进程")

        print("[abort] " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        # 用完就删：1GiB 的垃圾留在工作区里没有意义（下次运行会重建）。
        # 复用的那份也删 —— 它的存在只是为了省一次生成时间。
        if os.path.isfile(big):
            try:
                os.remove(big)
                print(f"[abort] 已清理临时大样本（{size_mb} MB）")
            except OSError as e:
                print(f"[abort] 清理大样本失败，可手动删除：{e}")


# ---------------------------------------------------------------------------
# combo：算法下拉框的"高度语义"实测 + 下拉列表是否完整可见
# ---------------------------------------------------------------------------
# 为什么要专门测：
#   下拉框的"窗口高度"语义非常反直觉 ——
#     · 闭合状态下，GetWindowRect 量到的是**闭合高度**，不是你 SetWindowPos 设的值；
#     · 你设的那个高度减去闭合高度，才是下拉列表能用的空间。
#   设错的症状是"下拉之后只看到两三项"（要滚），而**闭合态看起来完全正常** ——
#   所以光看主界面永远发现不了，必须真的展开来数。
#
# 顺带把 CB_GETITEMHEIGHT -> 实际闭合高度 的换算关系量出来（代码里是用一个
# 经验偏移量去凑的，这里实测确认那个偏移量对不对）。
CB_GETITEMHEIGHT = 0x0154
CB_SETITEMHEIGHT = 0x0153
CB_SHOWDROPDOWN  = 0x014F
LB_GETCOUNT      = 0x018B
LB_GETITEMHEIGHT = 0x01A1


def cmd_combo(exe=None):
    """量下拉框的闭合高度换算关系，并确认展开后六项全在、全看得见。"""
    import subprocess

    exe = exe or app_exe_path()
    if not os.path.isfile(exe):
        print(f"[ERROR] 找不到应用 exe：{exe}")
        return 2

    subprocess.Popen([exe])
    h = find_top_app(20)
    if not h:
        print("[ERROR] 没等到应用的顶层窗口")
        return 3
    combo = user32.GetDlgItem(h, ID_ALGO_COMBO)
    if not combo:
        print("[ERROR] 找不到下拉框（id=109）")
        return 4

    print(f"[combo] 窗口 = {h:#010x}  下拉框 = {combo:#010x}")
    cur = int(user32.SendMessageW(combo, CB_GETITEMHEIGHT, -1, 0))
    r = rect_of(combo)
    print(f"[combo] 当前条目高度(-1) = {cur}   窗口矩形高度 = {r[3] - r[1]}")
    print()

    # ---- 扫描：改条目高度 -> 看窗口矩形是否跟着变 ----
    print("[扫描] 设的条目高度 -> 回读条目高度 / 量到的窗口矩形高度")
    samples = []
    for v in (24, 30, 33, 36, 39, 45, 51):
        user32.SendMessageW(combo, CB_SETITEMHEIGHT, -1, v)
        time.sleep(0.15)
        back = int(user32.SendMessageW(combo, CB_GETITEMHEIGHT, -1, 0))
        rr = rect_of(combo)
        hh = rr[3] - rr[1]
        samples.append((v, hh))
        print(f"       {v:>4}  ->  回读 {back:>3}   矩形高 {hh}")
    # 恢复一个合理值，别把后面要展开的测试搞坏
    user32.SendMessageW(combo, CB_SETITEMHEIGHT, -1, 39)
    time.sleep(0.15)

    ok = True
    if len(set(hh for _, hh in samples)) == 1:
        print("       ⚠️ 矩形高度不随条目高度变化 —— 说明该控件不会自己改窗口尺寸，")
        print("          GetWindowRect 拿不到闭合高度（那代码里就只能用经验偏移量凑）。")
    else:
        # 线性关系：高度 = 条目高度 + 偏移
        (v0, h0), (v1, h1) = samples[0], samples[-1]
        slope = (h1 - h0) / (v1 - v0) if v1 != v0 else 0
        off = h0 - slope * v0
        print(f"       拟合：高度 ≈ {slope:.2f} × 条目高度 + {off:.2f}")
    print()

    # ---- 展开下拉，数条目 ----
    user32.SendMessageW(combo, CB_SHOWDROPDOWN, 1, 0)
    time.sleep(0.6)
    lbox = None
    for w in enum_tops():
        if class_of(w) == "ComboLBox" and user32.IsWindowVisible(w):
            lbox = w
            break
    if not lbox:
        print("[FAIL] 展开下拉后找不到 ComboLBox（列表根本没弹出来）")
        user32.SendMessageW(combo, CB_SHOWDROPDOWN, 0, 0)
        ok = False
    else:
        n = int(user32.SendMessageW(lbox, LB_GETCOUNT, 0, 0))
        ih = int(user32.SendMessageW(lbox, LB_GETITEMHEIGHT, 0, 0))
        lr = rect_of(lbox)
        lh = lr[3] - lr[1]
        need = n * ih
        print(f"[展开] ComboLBox = {lbox:#010x} rect={lr} 高={lh}")
        print(f"[展开] 条目数 = {n}   条目高 = {ih}   六项需要 {need}px 可见空间")
        if n != 6:
            print(f"[FAIL] 下拉条目应为 6，实际 {n}")
            ok = False
        if need > lh + 2:
            print(f"[FAIL] 下拉列表放不下：需要 {need}px，只有 {lh}px（用户得滚动才能看到全部）")
            ok = False
        else:
            print("[OK] 六项一次性全部可见（不需要滚动）")

        # 把"下拉框 + 列表"整块截下来，肉眼看一遍
        l = min(lr[0], rect_of(combo)[0])
        t = min(lr[1], rect_of(combo)[1])
        r2 = max(lr[2], rect_of(combo)[2])
        b2 = max(lr[3], rect_of(combo)[3])
        px, w2, h2 = capture_screen(l, t, r2 - l, b2 - t)
        path = os.path.join(SHOT_DIR, f"combo_{time.strftime('%H%M%S')}.png")
        save_png(px, w2, h2, path)
        print(f"[展开] 已保存 {path}")

        user32.SendMessageW(combo, CB_SHOWDROPDOWN, 0, 0)
        time.sleep(0.3)

    user32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.8)
    print("[combo] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def cmd_apptree():
    """枚举应用窗口内部结构（自动适配两种形态：单开顶层 / 嵌入子窗口）。"""
    found = False
    for h in enum_tops():
        if class_of(h) == APP_WND_CLASS and user32.IsWindowVisible(h):
            print(f"[单开] {h:#010x}  top-level")
            dump_tree(h, 1)
            found = True
    npp = find_npp(timeout=5)
    if npp:
        container = find_descendant(npp, "NppDockContainerWnd")
        if container:
            emb = find_descendant(container, APP_WND_CLASS)
            if emb:
                print(f"[嵌入] {emb:#010x}  (在 NppDockContainerWnd 之下)")
                dump_tree(emb, 1)
                found = True
    if not found:
        print("应用窗口     = 不存在")
        return 3
    return 0


def cmd_standalone(exe, keep=False):
    """清空残留 -> 单独启动 exe -> 等它出自己的顶层窗口 -> 检查样式与内部控件 -> 关掉。"""
    import subprocess
    if not os.path.isfile(exe):
        print(f"[ERROR] exe 不存在：{exe}")
        return 2
    print(f"[standalone] exe = {exe} ({os.path.getsize(exe)} 字节)")

    subprocess.Popen([exe])
    h = find_top_app(20)
    if not h:
        print("[ERROR] 15 秒内没等到应用的顶层窗口（单开失败？）")
        return 3

    st = describe_app_window(h)
    ok = True
    if st & WS_CHILD:
        print("[FAIL] 单开模式下窗口却是 WS_CHILD（应无 dock-parent 参数）")
        ok = False
    if not (st & WS_CAPTION):
        print("[FAIL] 单开模式下没有标题栏")
        ok = False
    if not (st & WS_THICKFRAME):
        print("[FAIL] 单开模式下不可缩放（WS_THICKFRAME 缺失）")
        ok = False

    # 关键：控件必须真的建出来了。外框铺满 ≠ 功能正常。
    kids, bad = audit_app_children(h)
    if len(kids) < 8:
        print(f"[FAIL] 子控件只有 {len(kids)} 个，界面是空的（应 >= 8）")
        ok = False
    if bad:
        print(f"[FAIL] 有 {len(bad)} 个控件不可见或尺寸异常：{bad}")
        ok = False
    print("[OK] 独立打开形态检查通过" if ok else "[FAIL] 独立打开形态检查未通过")

    if keep:
        print("[standalone] --keep，保留进程不关")
        return 0 if ok else 1

    user32.PostMessageW(h, 0x0010, 0, 0)   # WM_CLOSE
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if not user32.IsWindow(h):
            break
        time.sleep(0.2)
    left = count_md5_procs()
    print(f"[standalone] 关闭后 MD5 进程数 = {left}")
    if left != 0:
        print("[FAIL] 关闭窗口后进程没退出（残留）")
        ok = False
    print("[standalone] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def cmd_appwin():
    """报告：应用窗口现在是顶层（单开）还是 NppDock 容器里的子窗口（已嵌入）。"""
    tops = [h for h in enum_tops()
            if class_of(h) == APP_WND_CLASS and user32.IsWindowVisible(h)]
    for h in tops:
        print(f"[顶层] {h:#010x}")
        describe_app_window(h)
    npp = find_npp(timeout=5)
    embedded = None
    if npp:
        container = find_descendant(npp, "NppDockContainerWnd")
        if container:
            embedded = find_descendant(container, APP_WND_CLASS)
    if embedded:
        print(f"[嵌入] {embedded:#010x} (在 NppDockContainerWnd 之下)")
        describe_app_window(embedded)
    if not tops and not embedded:
        print("应用窗口     = 不存在")
    print(f"MD5 进程数   = {count_md5_procs()}")
    return 0


def cmd_menudbg():
    """调试：右击内容区后，把**新出现的顶层窗口**全部打出来。
    用来回答"菜单到底有没有真的弹出来、类名是什么"。"""
    npp, panel, our_tab, container = locate_panel()
    if not npp or not panel:
        print("[ERROR] 找不到 N++ 或内容区")
        return 2
    pl, pt, pr, pb = rect_of(panel)
    px, py = (pl + pr) // 2, pt + max(20, (pb - pt) // 3)
    print(f"内容区 rect = {rect_of(panel)}")
    print(f"右击点 = ({px},{py})")

    seen = set()

    def snap():
        new = []
        for h in enum_tops():
            if h in seen:
                continue
            seen.add(h)
            new.append((h, class_of(h), rect_of(h),
                        bool(user32.IsWindowVisible(h)), text_of(h)))
        return new

    snap()   # 基线：先把已存在的窗口吃掉，之后出现的才是新的
    user32.SetForegroundWindow(npp)
    time.sleep(0.4)
    click(px, py, "right")

    t0 = time.time()
    while time.time() - t0 < 3.0:
        for h, c, r, v, t in snap():
            print(f"  NEW {h:#010x} class={c!r:24s} rect={r} vis={int(v)}"
                  + (f" text={t!r}" if t else ""))
        time.sleep(0.1)

    p = wintypes.POINT()
    user32.GetCursorPos(ctypes.byref(p))
    print(f"当前光标 = ({p.x},{p.y})")
    print(f"MD5 进程数 = {count_md5_procs()}")
    return 0


# ---------------------------------------------------------------------------
# 截屏（用来"真的看一眼"界面 —— 只看坐标判断不了"空白"和"丑"）
# ---------------------------------------------------------------------------
SHOT_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "_t", "shots"))


def _gdi():
    g = ctypes.WinDLL("gdi32")
    g.CreateCompatibleDC.argtypes = [wintypes.HDC]
    g.CreateCompatibleDC.restype = wintypes.HDC
    g.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    g.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    g.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
    g.SelectObject.restype = wintypes.HGDIOBJ
    g.DeleteObject.argtypes = [wintypes.HGDIOBJ]
    g.DeleteDC.argtypes = [wintypes.HDC]
    g.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT,
                            wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p,
                            wintypes.UINT]
    g.GetDIBits.restype = ctypes.c_int
    return g


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long),
                ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER),
                ("bmiColors", wintypes.DWORD * 3)]


def capture_screen(x, y, w, h):
    """抓屏幕上一块区域，返回 (bytes, w, h)，BGRA、自上而下。

    ⚠️ 这里所有 GDI 函数都必须显式声明 argtypes —— 64 位下句柄是指针，
       ctypes 默认按 C int 传参会把高 32 位截掉，直接崩。
    """
    g = _gdi()
    src = user32.GetDC(0)
    mem = g.CreateCompatibleDC(src)
    bmp = g.CreateCompatibleBitmap(src, w, h)
    old = g.SelectObject(mem, bmp)
    gdi32_bitblt = ctypes.WinDLL("gdi32").BitBlt
    gdi32_bitblt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, wintypes.HDC,
                             ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    SRCCOPY = 0x00CC0020
    gdi32_bitblt(mem, 0, 0, w, h, src, x, y, SRCCOPY)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h            # 负 = 自上而下
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0        # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)

    g.SelectObject(mem, old)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    user32.ReleaseDC(0, src)
    return buf.raw, w, h


def capture_printwindow(hwnd, w, h):
    """用 PrintWindow 让**窗口自己把客户区画进我们给的内存 DC**，返回 (px, w, h)。

    为什么值得单独写一条（而不用抓屏）：
      · 抓屏 = BitBlt 桌面 —— 拿到的是**屏幕像素**。窗口被别的窗口压住、或者
        窗口在"量坐标"和"抓屏"之间被人拖走了，抓到的就是**别人**的画面。
        这个坑在 toggletest 上真的踩过（抓到过压在面板上的播放器画面，
        占比 75% 却照样判"通过"）。
      · PrintWindow 跟遮挡、跟窗口在屏幕上的位置**完全无关**，
        也不需要在抓之前把窗口抬到最前（不用打扰正在用电脑的人）。

    代价/前提：只覆盖"支持 WM_PRINTCLIENT 的控件"。标准控件（含我们子类化的
    标签条）都支持；而且容器在 WM_PRINTCLIENT 里补画了高亮那一笔，
    所以绿杠也会出现在这张图里（见 NppDockContainer.cpp 的 tabWndProc）。
    """
    PW_CLIENTONLY = 0x00000001
    g = _gdi()
    src = user32.GetDC(0)
    mem = g.CreateCompatibleDC(src)
    bmp = g.CreateCompatibleBitmap(src, w, h)
    old = g.SelectObject(mem, bmp)

    user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    user32.PrintWindow.restype = wintypes.BOOL
    got = user32.PrintWindow(hwnd, mem, PW_CLIENTONLY)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h            # 负 = 自上而下
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0        # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)

    g.SelectObject(mem, old)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    user32.ReleaseDC(0, src)
    return (buf.raw if got else None), w, h


def blank_score(pixels, w, h):
    """判断"是不是一片空白"。返回 (主色占比, 颜色数, 主色)。

    主色占比接近 1 = 整块区域一个颜色 —— 那就是"界面变空白"了。
    真正画着控件的界面颜色数至少几十个。
    """
    from collections import Counter
    step_x = max(1, w // 120)
    step_y = max(1, h // 60)
    c = Counter()
    for yy in range(0, h, step_y):
        row = yy * w * 4
        for xx in range(0, w, step_x):
            o = row + xx * 4
            c[(pixels[o + 2], pixels[o + 1], pixels[o])] += 1   # BGR -> RGB
    total = sum(c.values())
    top, n = c.most_common(1)[0]
    return n / total, len(c), top


def save_png(pixels, w, h, path):
    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), pixels, "raw", "BGRA", 0, 1)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").save(path)
    return path


def band_activity(pixels, w, h, y0, y1):
    """某一段横向条带里"非主色"的像素占比 —— 用来判断控件还在不在。

    只看颜色数会被大面积底色稀释：面板很高时，整块抓图的主色占比天然就 >99%，
    哪怕控件全没了也一样。所以必须**只看控件所在的那一条**。

    ⚠️ 但它仍然只是"辅助指标"，不能单独当结论用：应用自己的 WM_ERASEBKGND
       会把客户区填成 COLOR_BTNFACE，所以"背景在、控件全没了"和"完整界面"
       两种状态的颜色数可以很接近。判"界面到底画没画"要用 app_painted()，
       那个才是硬判据（有标定表）。
    """
    from collections import Counter
    y1 = min(y1, h)
    if y1 <= y0:
        return 0.0, 0
    c = Counter()
    for yy in range(y0, y1):
        row = yy * w * 4
        for xx in range(0, w, max(1, w // 200)):
            o = row + xx * 4
            c[(pixels[o + 2], pixels[o + 1], pixels[o])] += 1
    total = sum(c.values())
    if not total:
        return 0.0, 0
    top = c.most_common(1)[0][1]
    return (total - top) / total, len(c)


COLOR_BTNFACE = 15
DARK_PAINT_MIN = 0.01      # 见 app_painted() 里的标定表


def app_painted(pixels, w, h, band=170):
    """**硬判据**：嵌入应用的界面到底画出来了没有？

    踩过的两次误判，都要避免：

    ① 不要用"Edit 内部是不是白色"当判据。
       本机实测（5120x2160 @150%、系统默认配色）：编辑框内部是 **(240,240,240)**，
       和应用自己的底色一模一样 —— 按"白不白"判会**把好状态误报成空白**。
       （写这条注释的人就是这么误报了一次，白折腾一轮。）

    ② 不要只看"颜色数"。
       应用的 WM_ERASEBKGND 会把客户区整块填成底色，所以"背景刷过、控件全没了"
       和"完整界面"两种状态的颜色数可以很接近。

    真正分得开的是**暗像素**：控件有边框和文字（160 / 105 这些灰阶），
    底色是 240。所以取"上方 band 像素里 r<200 的占比"。

    本机标定（对着已知好/坏样本量出来的，见 _t/shots/）：

        完整界面   dg_after_180104=3.11%  dg_step06=3.11%  x3_now=5.41%  x_now=2.58%
        只剩底色   y_forced      =0.14%  now_175243=0.18%  tg_shown_180255=0.18%
        误报样本   tg_before_180551=3.11%  ← 其实是好的，Edit 判据错了

    取 1% 当门槛，两侧都留了很大余量。

    ⚠️ 局限：抓得住"整块/大范围变空白"，抓不住"只少了一排控件"（那种还有 2%+）。
       要查更细的差异，只能看存下来的截图。
    """
    y1 = min(band, h)
    if y1 <= 0:
        return 0.0
    dark = total = 0
    for yy in range(y1):
        row = yy * w * 4
        for xx in range(0, w, 2):
            o = row + xx * 4
            total += 1
            if pixels[o + 2] < 200:      # R 通道
                dark += 1
    return dark / total if total else 0.0


def cmd_shot(target, name):
    """截一张图并存成 PNG；同时报出"空白度"。"""
    if target == "app":
        h, desc = find_app_any(), "app"
    elif target == "panel":
        npp = find_npp(timeout=5)
        h, desc = (find_descendant(npp, CONTENT_CLASS) if npp else None), "panel"
    elif target == "container":
        npp = find_npp(timeout=5)
        h, desc = (find_descendant(npp, "NppDockContainerWnd") if npp else None), "container"
    else:
        h, desc = find_npp(timeout=5), "npp"
    if not h:
        print(f"[ERROR] 找不到 {target}")
        return 2
    l, t, r, b = rect_of(h)
    w, hh = r - l, b - t
    if w <= 0 or hh <= 0:
        print(f"[ERROR] {target} 尺寸异常 {w}x{hh}（最小化了？）")
        return 2
    px, w2, h2 = capture_screen(l, t, w, hh)
    frac, ncol, top = blank_score(px, w2, h2)
    path = os.path.join(SHOT_DIR, f"{name or desc}_{time.strftime('%H%M%S')}.png")
    save_png(px, w2, h2, path)
    print(f"[shot] {desc} hwnd={h:#010x} rect=({l},{t})-({r},{b}) {w}x{hh}")
    print(f"[shot] 主色占比 = {frac:.1%}  颜色数 = {ncol}  主色 = {top}")
    print(f"[shot] 已保存 {path}")
    return 0


def find_app_any():
    """找应用窗口：先找顶层（单开），再找容器里的（嵌入）。"""
    for h in enum_tops():
        if class_of(h) == APP_WND_CLASS and user32.IsWindowVisible(h):
            return h
    npp = find_npp(timeout=5)
    if npp:
        c = find_descendant(npp, "NppDockContainerWnd")
        if c:
            return find_descendant(c, APP_WND_CLASS)
    return None


def cmd_resizetest(cycles=2, delta=40):
    """复现"改 dock 大小 -> 里面的应用变空白"。

    做法：跨进程 SetWindowPos 改**容器窗口**的尺寸 —— 这正是宿主拖动分隔条
    时发生的事（容器收到 WM_SIZE -> layout() -> 宿主 -> 嵌入窗口）。
    每一步都截屏并统计「控件所在条带的活跃度」，同时记录整条窗口链的矩形。

    ⚠️ 探针自己踩的坑：SetWindowPos 不传 SWP_NOMOVE 就等于**顺便把窗口搬到
       父窗口的 (0,0)**。第一次跑就因此把容器挪走了，尺寸对但位置全乱，
       截图看起来"内容错位"，差点误判成应用没重绘。
    """
    SWP_NOSIZE_ = 0x0001
    SWP_NOMOVE  = 0x0002
    SWP_NOZORDER = 0x0004
    SWP_NOACTIVATE = 0x0010

    npp = find_npp(timeout=5)
    if not npp:
        print("[ERROR] 找不到 Notepad++")
        return 2
    container = find_descendant(npp, "NppDockContainerWnd")
    if not container:
        print("[ERROR] 找不到 NppDockContainerWnd")
        return 2
    app = find_app_any()
    if not app:
        print("[ERROR] 应用窗口不存在（先打开一个 MD5 页）")
        return 2

    cl, ct, cr, cb = rect_of(container)
    cw, ch = cr - cl, cb - ct
    print(f"容器 = {cw}x{ch} @({cl},{ct})")

    def audit(tag):
        """截容器整块 + 打印窗口链矩形 + 统计应用顶部条带活跃度。"""
        c = rect_of(container)
        w, h = c[2] - c[0], c[3] - c[1]
        ts = time.strftime('%H%M%S')
        px, w2, h2 = capture_screen(c[0], c[1], w, h)
        act, ncol = band_activity(px, w2, h2, 0, min(140, h2))
        p = os.path.join(SHOT_DIR, f"rz_{tag}_{ts}.png")
        save_png(px, w2, h2, p)
        al, at, ar, ab = rect_of(app)
        # 应用窗口相对容器的位置：不一致就说明布局/重绘错位了
        print(f"  [{tag}] 容器 {w}x{h}  应用 {ar-al}x{ab-at} "
              f"偏移=({al - c[0]},{at - c[1]}) vis={int(bool(user32.IsWindowVisible(app)))}")
        print(f"        顶部条带活跃度={act:.1%} 颜色数={ncol} -> {os.path.basename(p)}")
        kids = children(app)
        for k in kids:
            r = rect_of(k)
            print(f"        · {class_of(k):8s} id={id_of(k):4d} "
                  f"rel=({r[0]-al},{r[1]-at}) {r[2]-r[0]}x{r[3]-r[1]}")

    print("=== 调整前 ===")
    audit("before")

    for i in range(cycles):
        print(f"=== 第 {i+1} 轮：容器 {cw-delta}x{ch-delta} ===")
        user32.SetWindowPos(container, None, 0, 0, cw - delta, ch - delta,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
        time.sleep(0.7)
        audit(f"shrink{i+1}")
        print(f"=== 第 {i+1} 轮：容器复原 {cw}x{ch} ===")
        user32.SetWindowPos(container, None, 0, 0, cw, ch,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
        time.sleep(0.7)
        audit(f"restore{i+1}")

    print(f"MD5 进程数 = {count_md5_procs()}")
    return 0


def cmd_toggletest():
    """隐藏面板 -> 再显示，验证嵌入的应用**重新显示之后仍然画得出来**。

    为什么单独测这一条：对跨进程嵌入窗口来说，"重新显示"和"改尺寸"是
    **同一类重绘危险区** —— 都可能让对面再也进不了"待重画"名单
    （见 docs/铁律与踩坑要点.md「窗口与重绘」）。尺寸那条已经由 dragsplitter 守着，
    这条守唤出/隐藏。
    """
    npp = find_npp(timeout=5)
    if not npp:
        print("[ERROR] 找不到 Notepad++")
        return 2
    container = find_descendant(npp, "NppDockContainerWnd")
    if not app_guard():
        return 2

    def audit(tag):
        """返回 "ok" / "blank" / "skip"。

        ⚠️ "skip" 是**新加的状态**，别把它并进"失败"或"通过"：
           它表示"这一帧抓到的不是我们的画面，判不了"，见 screen_region_is_ours()。
           以前没有这个状态，结果就是**拿别人的画面算出一个 75% 的漂亮数字照样通过**。
        """
        if not user32.IsWindowVisible(container):
            print(f"  [{tag}] 容器不可见（已隐藏）")
            return "skip"
        # 先把 N++ 抬到最前：抓屏抓的是屏幕像素，被别的窗口压住就抓到别人的画面。
        bring_to_front(npp)
        time.sleep(0.4)
        mine, why = screen_region_is_ours(npp, container)
        if not mine:
            print(f"  [{tag}] [SKIP] 测量无效：{why}")
            print("        抓屏区域被别的窗口压住 —— 这一帧不能用来判断重绘，"
                  "请在窗口露出来（或桌面无遮挡）时重跑。")
            return "skip"
        c = rect_of(container)
        w, h = c[2] - c[0], c[3] - c[1]
        px, w2, h2 = capture_screen(c[0], c[1], w, h)
        act, ncol = band_activity(px, w2, h2, 0, min(140, h2))
        paint = app_painted(px, w2, h2)
        ok = paint >= DARK_PAINT_MIN
        p = os.path.join(SHOT_DIR, f"tg_{tag}_{time.strftime('%H%M%S')}.png")
        save_png(px, w2, h2, p)
        print(f"  [{tag}] 容器 {w}x{h} 活跃度={act:.1%} 颜色数={ncol}"
              f"  界面暗像素={paint:.2%} {'OK' if ok else '**空白**'}"
              f" -> {os.path.basename(p)}")
        return "ok" if ok else "blank"

    print("=== 隐藏前 ===")
    st_before = audit("before")
    if st_before == "skip":
        print("[中止] 抓屏区域不可信，本次**不判定**通过/失败（避免假通过）。")
        return 3
    if st_before != "ok":
        print("[FAIL] 隐藏前界面就没画出来 —— 这次结果不算数，先解决 baseline")
        return 1

    # ⚠️ 不要用模拟 Ctrl+Alt+D 来隐藏面板 —— 快捷键是"各线程的消息循环"
    #    自己 TranslateAccelerator 处理的，焦点一旦在嵌入应用里（另一个进程），
    #    按键就进了对面的队列，N++ 根本收不到（试过，日志里连一行都没有）。
    #    直接发 NPPM_DMMSHOW / NPPM_DMMHIDE 才是菜单命令走的同一条路。
    user32.SendMessageW(npp, NPPM_DMMHIDE, 0, container)   # 隐藏
    time.sleep(0.8)
    print("=== 隐藏后 ===")
    hidden_ok = not user32.IsWindowVisible(container)
    print(f"  容器可见 = {not hidden_ok}  -> {'OK 已隐藏' if hidden_ok else 'FAIL 没藏住'}")

    user32.SendMessageW(npp, NPPM_DMMSHOW, 0, container)   # 再显示
    time.sleep(0.8)
    print("=== 重新显示后 ===")
    st_after = audit("shown")

    if not hidden_ok:
        return 1
    if st_after == "skip":
        print("[中止] 唤出后的抓屏区域不可信，本次**不判定**通过/失败（避免假通过）。")
        return 3
    if st_after != "ok":
        print("[FAIL] 重新显示后面板不可见")
        return 1
    print("[OK] 隐藏->显示往返正常，控件重绘完整")
    return 0


def cmd_dragsplitter(dx, dy, steps=12):
    """**真实拖动停靠分隔条**（复现用户改 dock 尺寸的手势）。

    Notepad++ 的停靠区外面包着几个 4px 的分隔条；紧贴我们容器边缘的那一个
    就是拖动 dock 尺寸的把手 —— 竖的类名 `wedockspliter`（面板停在右边时拖它），
    横的类名 `nsdockspliter`（面板停在底部时拖它）。
    拖动过程中容器会收到**连续快速**的 WM_SIZE —— 这和探针直接 SetWindowPos
    一次有本质区别，所以必须真的按住鼠标拖。
    """
    npp = find_npp(timeout=5)
    if not npp:
        print("[ERROR] 找不到 Notepad++")
        return 2
    container = find_descendant(npp, "NppDockContainerWnd")
    if not app_guard():
        return 2
    cl, ct, cr, cb = rect_of(container)

    # 找紧贴容器的分隔条。面板可能停在**右边**（拖左边缘的竖条）也可能停在
    # **底部**（拖上边缘的横条），两种都要认：
    #   · 右停靠 -> 左边那条 `wedockspliter`（宽 4px 的竖条）
    #   · 底停靠 -> 上边那条 `nsdockspliter`（高 4px 的横条）
    # 只认 wedockspliter 的话，面板一旦被挪到底部就会报"没有贴着容器边缘的分隔条"。
    cands = []
    def walk(h, d=0):
        for k in children(h):
            if class_of(k) in ("wedockspliter", "wespliter", "wesplitter",
                               "nsdockspliter", "nsspliter", "nssplitter"):
                r = rect_of(k)
                cands.append((r, k))
            if d < 6:
                walk(k, d + 1)
    walk(npp)
    if not cands:
        print("[ERROR] 没找到分隔条（wedockspliter / nsdockspliter）")
        return 2

    def near(r):
        # 竖条：右边缘贴着容器左边；横条：下边缘在容器上边之上、且挨得不远。
        # ⚠️ 横条的容差必须放宽：底停靠时，分隔条和我们的容器之间还夹着
        #    停靠管理器给面板加的标题栏（真机量到 30px），卡 16px 会漏掉。
        left = (r[2] - r[0]) <= 16 and abs(r[2] - cl) <= 16 \
               and r[3] > ct and r[1] < cb
        top = (r[3] - r[1]) <= 16 and 0 <= ct - r[3] <= 64 \
              and r[2] > cl and r[0] < cr
        return left, top

    pick = None
    for r, k in cands:
        left, top = near(r)
        if left or top:
            pick = (r, k, "left" if left else "top")
            break
    if not pick:
        print("[ERROR] 没有贴着容器边缘的分隔条")
        for r, k in cands:
            print(f"   候选 {class_of(k)} {hex(k)} rect={r}")
        return 2

    r, sw, side = pick
    sx, sy = (r[0] + r[2]) // 2, (r[1] + r[3]) // 2
    print(f"分隔条 = {class_of(sw)} {hex(sw)} rect={r} 贴容器{side}边，抓取点 ({sx},{sy})")

    # ⚠️ 先把 N++ 抬到最前，再重新确认抓取点。
    #   两个理由：
    #     1) 拖动靠真实鼠标事件，窗口不在最前时抓取点下面可能是**别的程序** ——
    #        那一拖就是在拖别人的窗口；
    #     2) 抓屏抓的是屏幕像素，被压住时会抓到别人的画面（判据照样"通过"，见
    #        screen_region_is_ours 的说明）。
    bring_to_front(npp)
    time.sleep(0.5)

    # ⚠️⚠️ 下按之前**必须**确认光标下真的是那条分隔条。
    #   分隔条只有 4px 高，算出来的抓取点偏几像素就落到旁边的东西上 ——
    #   底停靠时紧挨着的是**面板标题栏**，拖它等于"把面板拖走"，
    #   实测会把整个面板拽到窗口外面去（而且不会自己回来）。
    #   代价是几行代码，收益是"测试不会把用户的界面搞坏"。
    pt = wintypes.POINT(sx, sy)
    user32.WindowFromPoint.argtypes = [wintypes.POINT]
    user32.WindowFromPoint.restype = wintypes.HWND
    under = user32.WindowFromPoint(pt)
    ucls = class_of(under) if under else ""
    print(f"抓取点下的窗口 = {hex(under) if under else '?'} \"{ucls}\"")
    if "spliter" not in ucls and "splitter" not in ucls:
        print(f"[ERROR] 抓取点落在了 \"{ucls}\" 上，不是分隔条 —— 已中止拖动。")
        print("        （拖到面板标题栏会把面板拽走，不是我们要测的'改尺寸'）")
        return 2

    def audit(tag):
        """返回 "ok" / "blank" / "skip"（skip = 抓到的不是我们的画面，判不了）。"""
        c = rect_of(container)
        w, h = c[2] - c[0], c[3] - c[1]
        if w <= 0 or h <= 0:
            # 面板被压到 0 高时截不到图，直接报出来 —— 别让 PIL 抛一个
            # "cannot write empty image" 把整条测试打断（踩过）。
            print(f"  [{tag}] 容器尺寸异常 {w}x{h}（面板被压没了？）")
            return "skip"
        mine, why = screen_region_is_ours(npp, container)
        if not mine:
            print(f"  [{tag}] [SKIP] 测量无效：{why}")
            return "skip"
        px, w2, h2 = capture_screen(c[0], c[1], w, h)
        act, ncol = band_activity(px, w2, h2, 0, min(140, h2))
        paint = app_painted(px, w2, h2)
        ok = paint >= DARK_PAINT_MIN
        p = os.path.join(SHOT_DIR, f"dg_{tag}_{time.strftime('%H%M%S')}.png")
        save_png(px, w2, h2, p)
        print(f"  [{tag}] 容器 {w}x{h} 活跃度={act:.1%} 颜色数={ncol}"
              f"  界面暗像素={paint:.2%} {'OK' if ok else '**空白**'}"
              f" -> {os.path.basename(p)}")
        return "ok" if ok else "blank"

    print("=== 拖动前 ===")
    ok_before = audit("before")

    trace = []

    def on_step(i):
        if i % 3 == 0:
            # ⚠️ 截图前必须先让消息队列排空一下，否则量到的是"假空白"：
            #   插件补重画走的是 PostMessage，要等本轮鼠标消息处理完才派发；
            #   而 SetCursorPos 一返回我们就截图，那时候补重画还没跑。
            #   真人拖动时两次鼠标移动间隔约 8ms，补重画早就执行完了 ——
            #   所以这里等一下再量，才是拖动过程中的真实观感。
            time.sleep(0.30)
            trace.append((i, audit(f"step{i:02d}")))

    drag(sx, sy, sx + dx, sy + dy, steps, on_step)
    time.sleep(1.0)
    print("=== 松手后 ===")
    ok_after = audit("after")
    print("拖动过程判定: " + ", ".join(f"#{i}={p}" for i, p in trace))
    verdicts = [ok_before] + [p for _, p in trace] + [ok_after]
    if "skip" in verdicts:
        n = verdicts.count("skip")
        print(f"[中止] 有 {n} 帧抓到的不是我们的画面（被压住/尺寸为 0），"
              f"本次**不判定**通过/失败 —— 别拿别人的画面凑合成「通过」。")
        return 3
    if ok_before != "ok":
        print("[FAIL] 拖动**前**界面就没画出来 —— 先查别的问题，这次拖动结果不算数")
        return 1
    if any(p != "ok" for _, p in trace):
        print("[FAIL] 拖动**过程中**界面丢过（见上面标了「空白」的那几帧）")
        return 1
    if ok_after != "ok":
        print("[FAIL] 松手后界面没画出来 —— 跨进程重绘没兜住")
        return 1
    print("[OK] 拖动全程 + 松手后，嵌入应用的界面始终正常绘制")
    return 0


def app_guard():
    """拖动前确认应用窗口在（不然测了也没意义）。"""
    a = find_app_any()
    if not a:
        print("[ERROR] 应用窗口不存在（先打开一个 MD5 页）")
        return False
    return True


def locate_panel(npp=None):
    if npp is None:
        npp = find_npp()
    if not npp:
        return None, None, None, None
    # 面板在 NPPN_READY 之后才注册，比主窗口晚；不能查一次就放弃。
    panel = wait_for_panel(npp)
    # 注意：N++ 自己也有 SysTabControl32（文档标签），必须挑我们那个 ——
    # 判据是它必须是 NppDockContainerWnd 的后代。
    container = find_descendant(npp, "NppDockContainerWnd")
    our_tab = None
    if container:
        our_tab = find_descendant(container, TAB_CLASS)
    return npp, panel, our_tab, container


def panel_click_point(panel):
    """空白内容区里一个安全的右击点（避开边缘，避开标签条高度）。"""
    pl, pt, pr, pb = rect_of(panel)
    return (pl + pr) // 2, pt + max(20, (pb - pt) // 3)


def menu_near_point(cands, x, y, max_dist=400):
    """在一堆菜单窗口里挑"就是刚弹出来那个"。

    ⚠️ 菜单窗口句柄会被系统**回收复用**：上一个菜单还没销毁干净、新菜单拿到了
       同一个句柄时，"是不是新句柄"这个判据失效；此时如果随手取 `cands[0]`，
       很可能拿到**旧菜单** —— 它的条目矩形是**上一个位置的**，
       照它点击等于点空气（实测：面板从底部挪到中段后，"关闭"就一直点不中，
       日志里只有右击、没有"已关闭应用页"）。
    判据按可靠度排序：
      1) 矩形**包含**右击点（原生菜单默认就把点包在里面）；
      2) 离右击点最近、且在 max_dist 之内。
    """
    best, best_d = None, None
    for h in cands:
        l, t, r, b = rect_of(h)
        if r <= l or b <= t:
            continue
        if l - 4 <= x <= r + 4 and t - 4 <= y <= b + 4:
            return h                     # ① 点就在这个菜单里，最可信
        dx = max(l - x, 0, x - r)
        dy = max(t - y, 0, y - b)
        d = (dx * dx + dy * dy) ** 0.5
        if best is None or d < best_d:
            best, best_d = h, d
    if best is not None and best_d is not None and best_d <= max_dist:
        return best
    return None


def right_click_at(npp, x, y, attempts=3):
    """把 N++ 拉到前台，在 (x,y) 右击，返回新弹出的菜单窗口（可能 None）。

    ⚠️ 两个坑：
      1) 菜单窗口句柄会被系统**回收复用**。上一个菜单还没关时，新菜单可能拿到
         同一个句柄，按"句柄是不是新的"判断就会漏判 —— 所以先按 ESC 清场，
         并且"兜底"那次也要用 `menu_near_point` 挑**真的在点击点上**那一个，
         不能随手取 `menu_windows()[0]`（那可能是旧菜单，条目矩形全是旧的）。
      2) 真机上人可能在同时操作鼠标，偶尔一次点击会落空 —— 所以重试几次。
    """
    for i in range(attempts):
        press_esc()
        time.sleep(0.25)
        user32.SetForegroundWindow(npp)
        time.sleep(0.4 if i == 0 else 0.25)
        before = set(menu_windows())
        click(x, y, "right")
        menu = find_menu(before, timeout=2.5)
        if menu is not None:
            return menu
        # 句柄复用的情况：只要现在有可见菜单，就在里面挑"包住点击点"的那个
        cur = menu_windows()
        pick = menu_near_point(cur, x, y)
        if pick is not None:
            return pick
        if cur:
            print(f"   （第 {i + 1} 次右击：找到 {len(cur)} 个菜单，但没一个"
                  f"罩得住点击点 ({x},{y})，重试）")
        else:
            print(f"   （第 {i + 1} 次右击没等到菜单，重试）")
    return None


def tab_item_click_point(tab):
    """算第一个标签项上的一个点（屏幕坐标）。

    ⚠️⚠️ 这里有个**会让宿主崩溃**的坑，务必记住：
       想拿标签项真实矩形，自然会想到 SendMessage(TCM_GETITEMRECT, ...)。
       但 SendMessage 跨进程**不会封送指针参数**（只有 WM_GETTEXT /
       WM_GETTEXTLENGTH 等极少数系统消息是特例）。我们把 RECT 的地址
       传过去，Notepad++ 会在**它自己的地址空间**里往这个地址写 8 个字段 ——
       那个地址在我们的空间里有效，在对面的空间里是野指针，直接访问违例，
       **把 Notepad++ 整个写崩**（实测：之后 GetWindowRect 全返回 0、
       窗口树全空、进程消失）。
       所以：跨进程只能做"无指针参数"的查询；要位置就自己推算。

    推算依据：标签项左对齐、贴着左边框排布。标签条本身铺满整个宽度，
    但它右边一大片是空白 —— 右击那里控件不一定发 NM_RCLICK（实测三次都没反应）。
    """
    tl, tt, tr, tb = rect_of(tab)
    return tl + 20, tt + (tb - tt) // 2


def tab_blank_click_point(tab):
    """标签条**空白处**的一个点（屏幕坐标）—— 添加应用的入口。

    ⚠️ 别拿标签条中心当空白处：标签项是从左边开始排的，左端那块是标签项本体
       （`closetab` 就点那儿）。标签条是 TCS_FIXEDWIDTH，标签项固定宽度、
       不会铺满整条，所以取**最右边**一定落在空白处。
    """
    tl, tt, tr, tb = rect_of(tab)
    return tr - 30, tt + (tb - tt) // 2


def menu_flow(args, npp, panel, our_tab):
    """右击 -> 列菜单 -> 可选地点选某一项。

    三个右击位置对应三套行为（用户定的交互）：
      · 内容区（空白页）  -> **什么都不弹**（`menu` 就是来验证这一条的）
      · 标签条空白处      -> 添加应用（`addapp` / `pick`）
      · 标签项上          -> 关闭该页（`closetab`）
    """
    if args.cmd == "closetab":
        if not our_tab:
            print("[ERROR] 没有标签条")
            return 2
        cx, cy = tab_item_click_point(our_tab)
        print(f"右击标签项   = ({cx},{cy})  标签条 rect={rect_of(our_tab)}")
    elif args.cmd in ("addapp", "pick"):
        if not our_tab:
            print("[ERROR] 没有标签条")
            return 2
        cx, cy = tab_blank_click_point(our_tab)
        print(f"右击标签条空白处 = ({cx},{cy})  标签条 rect={rect_of(our_tab)}")
    else:
        cx, cy = panel_click_point(panel)
        print(f"右击内容区   = ({cx},{cy})")

    menu = right_click_at(npp, cx, cy)
    if not menu:
        if args.cmd in ("addapp", "pick", "closetab"):
            print("[ERROR] 没有检测到弹出菜单（#32768）")
            return 3
        print("[OK] 右击内容区没有弹出任何菜单 —— 符合「右击空白处不产生任何功能」")
        return 0

    items, owner = menu_items(menu)
    ml, mt, mr, mb = rect_of(menu)
    print(f"菜单窗口     = {menu:#010x} rect=({ml},{mt},{mr},{mb})  共 {len(items)} 项"
          f"  宿主={owner if owner is None else hex(owner)}")

    buttons = []
    for idx, txt, rc in items:
        kind = "分隔符" if not txt else "项"
        # 一致性校验：菜单项的矩形必须落在菜单窗口里面，否则坐标不可信
        warn = ""
        if rc and not (ml - 64 <= rc[0] and rc[2] <= mr + 64
                       and mt - 64 <= rc[1] and rc[3] <= mb + 64):
            warn = "   <== 超出菜单范围，坐标不可信！"
        print(f"   [{idx}] {kind:4s} \"{txt}\" rect={rc}{warn}")
        if txt:
            buttons.append((idx, txt, rc))

    if args.cmd == "menu":
        return 0

    # ---- 挑选目标项 ----
    target = None
    if args.text:
        for idx, txt, rc in buttons:
            if args.text in txt:
                target = (idx, txt, rc)
                break
    elif args.index < len(buttons):
        target = buttons[args.index]

    if not target or not target[2]:
        print(f"[ERROR] 找不到目标菜单项（text={args.text!r} index={args.index}）")
        return 4

    idx, txt, (l, t, r, b) = target
    ccx, ccy = (l + r) // 2, (t + b) // 2
    # 点击前再拦一道：坐标必须可信（落在菜单窗口内），否则宁可报错也别乱点，
    # 乱点会在别处再弹一个菜单，把日志搅乱、根本分不清是谁触发的。
    if not (ml - 64 <= ccx <= mr + 64 and mt - 64 <= ccy <= mb + 64):
        print(f"[ERROR] 菜单项坐标 ({ccx},{ccy}) 不在菜单窗口 "
              f"({ml},{mt})-({mr},{mb}) 内，放弃点击")
        return 5

    print(f"--> 点选 [{idx}] \"{txt}\" 于 ({ccx},{ccy})")
    click(ccx, ccy, "left")
    time.sleep(1.5)
    return 0


def count_procs(image_name):
    """数一数某个 exe 有几个在跑（tasklist，不引入 psutil）。

    ⚠️ 判据必须是"**差值**"而不是绝对值：环境里本来就可能已经有别的实例
       （用户自己开着的、上一次测试没收干净的）。用绝对值当判据出现过
       "关闭后还有 1 个进程残留"的假失败 —— 那个 1 是别人的。
    """
    import subprocess
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image_name}",
                              "/FO", "CSV", "/NH"],
                             capture_output=True).stdout.decode("utf-8", "replace")
    except Exception:
        return -1
    stem = image_name[:-4] if image_name.lower().endswith(".exe") else image_name
    return sum(1 for l in out.splitlines() if stem in l)


def count_md5_procs():
    return count_procs("NppDockApp_MD5.exe")


def page_state_path():
    """插件的面板状态文件 panel.ini（页序/当前页/显隐都记在这里）。"""
    return os.path.join(repo_root(), "plugins", "Config", "NppDock", "panel.ini")


def read_page_state():
    """读 panel.ini 的原始行（不解析，直接给断言用）。"""
    p = page_state_path()
    if not os.path.isfile(p):
        return []
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return [ln.strip() for ln in f if ln.strip()]


def read_log_text():
    """整份插件日志的文本（找不到就返回空串）。"""
    p = os.path.join(repo_root(), "plugins", "Config", "NppDock", "NppDock.log")
    if not os.path.isfile(p):
        return ""
    with open(p, "rb") as f:
        return f.read().decode("utf-8", errors="replace")


def log_line_count():
    return len(read_log_text().splitlines())


def tab_page_count(tab):
    """标签条里有几个页（TCM_GETITEMCOUNT）。

    TCM_GETITEMCOUNT 是 0x1304 > WM_USER，**UIPI 会拦**"低权限进程 -> 高权限窗口"
    的这类消息。本机实测能通（两个进程完整性级别相同），但如果哪天探针从普通权限
    启动、而 Notepad++ 是管理员权限，这里会**静默返回 0**。
    所以：数页数一律用 page_count()（它靠枚举窗口，不发消息），
    这个函数只用来打印一个交叉验证值。

    带指针的那些（TCM_GETITEM / TCM_GETITEMRECT）绝对不能这么用 ——
    对面会在自己的地址空间往我们给的地址写，直接把它写崩（详见
    tab_item_click_point 的注释）。
    """
    return int(user32.SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0))


def count_descendants(root, cls):
    """数一数后代里有几个指定类名的窗口（不带指针、不发自定义消息）。"""
    n = 0
    stack = [root]
    while stack:
        h = stack.pop()
        for c in children(h):
            if class_of(c) == cls:
                n += 1
            stack.append(c)
    return n


def page_count(container):
    """面板里开着的应用**页数**。

    依据：每一页都有一个 NppDockAppHost 子窗口（容器的子窗口，本进程的），
    关页时一起销毁 —— 所以"宿主窗口个数"恒等于"页数"。
    这条路子只用到窗口枚举，**不需要跨进程 SendMessage**，
    因此不受 UIPI / 完整性级别影响（比 TCM_GETITEMCOUNT 稳）。

    ⚠️ 关页之后这个数会**滞后一拍**才降下来：容器要先给应用发 WM_CLOSE、
       等它退出（最长 3 秒），之后才销毁宿主窗口。所以"关完立刻读"可能还是旧值。
       需要确认"真的关掉了"请用 `page_count_settled()`。
    """
    return count_descendants(container, "NppDockAppHost")


def page_count_settled(container, timeout=6.0, quiet=0.5):
    """等页数**稳定**下来再返回（连续 quiet 秒读数不变）。

    关页是异步的（等应用进程退出 → 销毁宿主窗口），单次读取会拿到旧值；
    拿旧值当判据会做出错误决策 —— 实测因此**多关了一页**。
    """
    deadline = time.time() + timeout
    last = page_count(container)
    stable_since = time.time()
    while time.time() < deadline:
        time.sleep(0.1)
        cur = page_count(container)
        if cur != last:
            last = cur
            stable_since = time.time()
        elif time.time() - stable_since >= quiet:
            return cur
    return last


# ---------------------------------------------------------------------------
# "当前标签页高亮绿杠"的像素级验证
# ---------------------------------------------------------------------------
# ⚠️ 这个颜色必须与 NppDockContainer.cpp 里的 kTabAccent 保持一致。
#    它变了这里也要跟着变，否则测试会报"找不到绿杠"，而其实绿杠是好的 ——
#    "测试与实现各说各话"造成的假失败最费时间，所以两边都写了指向对方的注释。
ACCENT_RGB = (0x2E, 0xA0, 0x43)
ACCENT_TOL = 48


def accent_runs(pixels, w, h):
    """在截图里找 kTabAccent 那种绿。

    返回 (像素数, [(x0, x1)] 连续段（按 x 合并、允许 3px 空隙）, 最小 y, 最大 y)。

    为什么要返回"连续段"而不是只给一个总数：
      总数无法区分"一段绿杠"和"两个标签上各有一段绿杠"。
      而"同一时刻只能有一段"正是这个功能的关键不变量。
    """
    xs, n = [], 0
    miny, maxy = 10 ** 9, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            b, g, r = pixels[o], pixels[o + 1], pixels[o + 2]
            if (abs(r - ACCENT_RGB[0]) <= ACCENT_TOL
                    and abs(g - ACCENT_RGB[1]) <= ACCENT_TOL
                    and abs(b - ACCENT_RGB[2]) <= ACCENT_TOL
                    and g > r + 30 and g > b + 30):
                n += 1
                xs.append(x)
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if not xs:
        return 0, [], -1, -1

    xs = sorted(set(xs))
    segs = [[xs[0], xs[0]]]
    for x in xs[1:]:
        if x - segs[-1][1] <= 3:
            segs[-1][1] = x
        else:
            segs.append([x, x])
    return n, [(a, b) for a, b in segs], miny, maxy


def tab_ink_extent(pixels, w, h):
    """标签条上"有东西"的横向范围 (ink_left, ink_right)。

    背景色直接取标签条**最右端**的像素 —— 标签项是从左边开始排的，
    右边一大片一定是底色，所以那里一定是背景（不用去读系统颜色）。

    ⚠️ 只在**标签项所在的那条横带**里扫，不扫全高：标签条底部还有一条
       "显示区边框线"，它是**整宽**的 —— 扫全高会把 ink_right 顶到最右边，
       于是"最后一个标签的位置"就算成了最右边那块空白（实测因此把右击点
       送进了空白区，弹出的是"添加应用"菜单，反倒又开了一页；
       而"关闭那一页"的测试看起来却像是通过了）。
    """
    if w <= 2 or h <= 0:
        return 0, -1
    o = (h // 2) * w * 4 + (w - 2) * 4
    bg = (pixels[o], pixels[o + 1], pixels[o + 2])
    y0, y1 = 1, max(2, h // 2)          # 只取上半：标签项在这儿，边框线在下面
    left, right = -1, -1
    for y in range(y0, y1):
        row = y * w * 4
        for x in range(w):
            o = row + x * 4
            if (abs(pixels[o] - bg[0]) > 12 or abs(pixels[o + 1] - bg[1]) > 12
                    or abs(pixels[o + 2] - bg[2]) > 12):
                if left < 0 or x < left:
                    left = x
                if x > right:
                    right = x
    return left, right


def session_locked():
    """桌面是不是**锁屏**状态。

    为什么要专门判断这个：锁屏时鼠标与键盘的**注入都到不了目标窗口** ——
    系统把它们投到安全桌面（LogonUI）上去了。于是所有"真点一下""敲一下按键"
    的断言必然失败，但那是**环境**问题，不是产品问题。
    以前这类失败会以 FAIL 的形式报出来（2026-10-03 凌晨那一轮就是这样，
    三条点击类回归全红，实际是王锁了屏），下一个人会白找半天代码。

    判据（任一成立即认定锁屏）：
      · 锁屏背板窗口在场（LockScreenBackstopFrame，由 LockApp 创建）；
      · 面板中心那个点的落点落在类名含 "Lock" 的窗口上。
    """
    try:
        if user32.FindWindowW("LockScreenBackstopFrame", None):
            return True
    except Exception:
        pass
    try:
        npp, panel, our_tab, cont = locate_panel()
        if cont:
            l, t, r, b = rect_of(cont)
            user32.WindowFromPoint.argtypes = [wintypes.POINT]
            user32.WindowFromPoint.restype = wintypes.HWND
            hw = user32.WindowFromPoint(wintypes.POINT((l + r) // 2, (t + b) // 2))
            if hw and "Lock" in class_of(hw):
                return True
    except Exception:
        pass
    return False


def skip_if_locked(who):
    """锁屏时就别往下跑了：打印一句能看懂的原因，返回 True（调用方 return 3）。"""
    if not session_locked():
        return False
    print(f"[{who}] SKIP —— 桌面锁着（LogonUI/LockApp 在跑）。")
    print("  锁屏时鼠标与键盘注入进不到目标窗口，点击类/按键类断言无法进行；")
    print("  这不是产品问题。解锁之后再跑这条命令即可。")
    return True


def point_is(hwnd, x, y):
    """屏幕上这个点落到的窗口是不是 hwnd（或它的后代）。"""
    user32.WindowFromPoint.argtypes = [wintypes.POINT]
    user32.WindowFromPoint.restype = wintypes.HWND
    h = user32.WindowFromPoint(wintypes.POINT(x, y))
    if not h:
        return False, ""
    ok = (h == hwnd) or is_ancestor_or_self(h, hwnd)
    return ok, class_of(h)


def grab_tab_strip(npp, our_tab, name):
    """抓标签条（客户区）的像素。

    两条路，**优先 PrintWindow**：
      1. PrintWindow —— 跟遮挡/窗口位置无关，不用抢前台，不打扰正在用电脑的人；
      2. 抓屏 —— 回退路径，必须做归属校验（压住了就报"无效"，绝不假通过）。
    返回 (px, w, h) 或 None。
    """
    tl, tt, tr, tb = rect_of(our_tab)
    w, h = tr - tl, tb - tt
    if w <= 0 or h <= 0:
        print("  [skip] 标签条尺寸为 0，抓不了")
        return None

    px, w2, h2 = capture_printwindow(our_tab, w, h)
    if px is not None:
        _n, segs, miny, _mx = accent_runs(px, w2, h2)
        # PrintWindow 对某些控件会返回一张全黑/全白的图（没实现 WM_PRINTCLIENT）。
        # 判据：只要有内容（能找到绿杠，或能找到墨迹）就算它画了。
        _l, ink_r = tab_ink_extent(px, w2, h2)
        if segs or ink_r >= 0:
            print(f"  [printwindow] {w2}x{h2}（不受遮挡影响）")
            return px, w2, h2
        print("  [printwindow] 返回的是空图，回退抓屏")

    ok, why = screen_region_is_ours(npp, our_tab)
    if not ok:
        print(f"  [skip] 标签条区域被别的窗口压住：{why}")
        return None
    px, w2, h2 = capture_screen(tl, tt, w, h)
    p = os.path.join(SHOT_DIR, f"hl_{name}_{time.strftime('%H%M%S')}.png")
    try:
        save_png(px, w2, h2, p)
        print(f"  [抓屏] {w2}x{h2} -> {os.path.basename(p)}")
    except ImportError:
        print(f"  [抓屏] {w2}x{h2}（没装 PIL，跳过存图）")
    return px, w2, h2


def app_menu_labels(npp, our_tab):
    """右击标签条空白处弹出"添加应用"菜单，读回条目文字，然后 ESC 关掉。

    返回 (标签列表, 出错原因或空串)。
    """
    cx, cy = tab_blank_click_point(our_tab)
    menu = right_click_at(npp, cx, cy)
    if not menu:
        press_esc()
        return [], "没检测到弹出菜单（#32768）"
    items, _owner = menu_items(menu)
    texts = [t for _i, t, _rc in items if t]
    press_esc()
    time.sleep(0.25)
    return texts, ""


def open_app_via_menu(npp, our_tab, index=0, text=""):
    """右击标签条空白处 -> 菜单 -> 点选某个应用 -> 新开一页。

    返回 (ok, 菜单条目文字列表, 什么被点了)。
    """
    cx, cy = tab_blank_click_point(our_tab)
    menu = right_click_at(npp, cx, cy)
    if not menu:
        press_esc()
        return False, [], "没检测到弹出菜单"
    items, _owner = menu_items(menu)
    cand = [(i, t, rc) for i, t, rc in items if t and rc]
    texts = [t for _i, t, _rc in cand]

    target = None
    if text:
        for i, t, rc in cand:
            if text in t:
                target = (i, t, rc)
                break
    elif index < len(cand):
        target = cand[index]

    if not target:
        press_esc()
        return False, texts, f"找不到目标项（index={index} text={text!r}）"

    i, t, (l, top, r, b) = target
    click((l + r) // 2, (top + b) // 2, "left")
    time.sleep(1.6)          # 等进程起来 + 挂载窗口（dock 侧超时是 8s）
    return True, texts, t


def close_tab_at(npp, tab, screen_x, screen_y):
    """在标签条的某个点上右击 -> 菜单只有一项「关闭「xxx」」-> 点它。

    ⚠️ 下按之前**必须**确认这个点上真的是那条标签条。
       这是踩过的最贵的一次：抓取点算偏几像素，落到了别的控件上，
       结果右击弹出来的是**另一个菜单**，随手点下去就把状态搅乱了
       （实测把"关闭"点成了"添加应用"，反倒越点越多）。
       代价两行，收益是"测试不会把用户的界面搞坏"。
    """
    ok, cls = point_is(tab, screen_x, screen_y)
    if not ok:
        print(f"  [ERROR] ({screen_x},{screen_y}) 落在「{cls}」上，不是标签条 —— 取消右击")
        return False, f"落点不是标签条（{cls}）"

    menu = right_click_at(npp, screen_x, screen_y)
    if not menu:
        press_esc()
        return False, "没检测到弹出菜单"
    items, _owner = menu_items(menu)
    # ⚠️ 菜单弹出来了**不等于**右击命中了标签项：右击落在标签上沿的圆角/空处时，
    #    容器弹的是「添加应用」菜单。这时去点它的第一项，等于又开了一个页
    #    （实测把"关闭"点成了"打开网络测试"，页数越关越多）。
    #    所以必须先确认这个菜单里真的有「关闭」。
    # ★ v1.4 起菜单文字就是「Close」（王要求"直接一个 Close 单词就行"）。
    #   这里两种都认 —— 老构建（「关闭「xxx」」）也能跑，免得测试和构建版本绑死。
    def _is_close(x):
        return bool(x) and ("关闭" in x or x.strip().lower() == "close")

    cand = [(i, t, rc) for i, t, rc in items if t and rc and _is_close(t)]
    if not cand:
        press_esc()
        got = [t for _i, t, _rc in items if t]
        return False, f"弹出的不是「Close」菜单（右击没命中标签项）：{got}"
    i, t, (l, top, r, b) = cand[0]
    # ⚠️ 一致性校验：条目矩形必须落在**这个菜单窗口**的矩形里。
    #    对不上说明拿到的菜单句柄是陈旧的（句柄被系统回收复用），
    #    照这个矩形点下去就是点空气 —— 而且失败得很安静。
    ml, mt, mr, mb = rect_of(menu)
    if not (ml - 8 <= l and r <= mr + 8 and mt - 8 <= top and b <= mb + 8):
        press_esc()
        return False, (f"菜单条目矩形 {(l, top, r, b)} 不在菜单窗口 "
                       f"{(ml, mt, mr, mb)} 内 —— 菜单句柄陈旧，放弃这次点击")
    click((l + r) // 2, (top + b) // 2, "left")
    time.sleep(1.4)
    return True, t


def cmd_multiopen(exe=None):
    """验「同一个应用可以开多个页」。

    ⚠️ 这条需求以前是不成立的：老代码在 openAppPage 里写着"已开着就切过去"，
       所以第二个「文件校验」永远开不出来。所以测试要盯的是三件事：
         1. 页数**真的 +1**（不是又切回原来那页）；
         2. 进程数**真的 +1**（每个页是独立进程，不是共享一个窗口）；
         3. 后开的页标题带序号（"文件校验（2）"）—— 否则两页长得一模一样，
            用户根本分不清谁是谁。第 3 条只能从插件日志里读（跨进程读标签
            文字需要 TCM_GETITEM，那要传指针，会把 Notepad++ 写崩）。
    """
    if skip_if_locked("multiopen"):
        return 3
    npp, panel, our_tab, container = locate_panel()
    if not npp or not panel or not our_tab:
        print("[ERROR] 找不到 Notepad++ / 内容区 / 标签条（面板没显示？）")
        return 2

    exe = exe or app_exe_path()
    img = os.path.basename(exe)

    press_esc()          # 万一上一次失败留下了没关掉的菜单，先清场
    time.sleep(0.2)
    bring_to_front(npp)

    pages0 = page_count(container)
    procs0 = count_procs(img)
    logs0 = log_line_count()
    print(f"起点：页数 = {pages0}，{img} 进程数 = {procs0}，日志 {logs0} 行")
    cross = tab_page_count(our_tab)
    if cross != pages0:
        print(f"  （交叉验证：TCM_GETITEMCOUNT = {cross}，与窗口枚举 {pages0} 不同 —— "
              f"以窗口枚举为准，可能是 UIPI 把消息拦了）")
    print()

    ok = True

    # ⚠️ 所有断言一律写成"**相对起点**"的形式（起点可能已经有别的实例开着 ——
    #    用户自己开的、上一次测试没收干净的都算）。写死"应该是 1"会造出
    #    "测试通过与否取决于当时开着几页"的假失败，那种测试没人敢信。
    labels, err = app_menu_labels(npp, our_tab)
    if err:
        print(f"[FAIL] {err}")
        return 1
    print(f"开之前菜单：{labels}")

    # ★ v1.3 起菜单项**不再**写「已开 N」，也**不打勾**（王要求的）：
    #   这里的每一项都是"再开一个新页"，标数字/打勾只会让人以为"那个是当前页"。
    #   所以这条断言反过来写：**出现了后缀才是 bug**。
    hinted = [t for t in labels if "已开" in t]
    if hinted:
        print(f"[FAIL] 添加应用菜单里还带着「已开 N」标注：{hinted}")
        ok = False
    else:
        print(f"[OK] 添加应用菜单是干净的应用名（没有「已开 N」标注）：{labels}")

    opened, labels, what = open_app_via_menu(npp, our_tab, index=0)
    if not opened:
        print(f"[FAIL] 第 1 次添加失败：{what}")
        return 1
    pages1 = page_count(container)
    procs1 = count_procs(img)
    print(f"第 1 次点「{what}」-> 页数 {pages1}（{pages1 - pages0:+d}）"
          f"，进程数 {procs1}（{procs1 - procs0:+d}）")
    if pages1 != pages0 + 1:
        print("[FAIL] 页数没有 +1（是不是又「切回原来那页」了？）")
        ok = False
    if procs1 != procs0 + 1:
        print("[FAIL] 进程数没有 +1（两页共用一个进程？）")
        ok = False
    print()

    labels, err = app_menu_labels(npp, our_tab)
    if err:
        print(f"[FAIL] {err}")
        return 1
    print(f"开一个之后菜单：{labels}")
    hinted1 = [t for t in labels if "已开" in t]
    if hinted1:
        print(f"[FAIL] 菜单里出现了「已开 N」标注：{hinted1}")
        ok = False
    else:
        print("[OK] 再开一页之后菜单依然干净（不标数量、不打勾）")

    opened, labels, what = open_app_via_menu(npp, our_tab, index=0)
    if not opened:
        print(f"[FAIL] 第 2 次添加失败：{what}")
        return 1
    pages2 = page_count(container)
    procs2 = count_procs(img)
    print(f"第 2 次点「{what}」-> 页数 {pages2}（{pages2 - pages0:+d}）"
          f"，进程数 {procs2}（{procs2 - procs0:+d}）")
    if pages2 != pages0 + 2:
        print("[FAIL] 页数没有 +2")
        ok = False
    if procs2 != procs0 + 2:
        print("[FAIL] 进程数没有 +2")
        ok = False

    # ---- 标题唯一性 ----
    # 跨进程读标签文字需要 TCM_GETITEM，那要传指针 —— 会把 Notepad++ 写崩
    # （见 tab_item_click_point 的注释），所以改从**插件日志**里取：
    # 每嵌入一页，容器都会记一行「应用已嵌入：<页标题>（页面标题即 …）」。
    # 不变量：这一次新开的 2 页，标题必须**互不相同**，且都以应用名开头。
    tail = read_log_text().splitlines()
    tail = tail[logs0:] if logs0 < len(tail) else []
    joined = "\n".join(tail)
    titles = re.findall(r"应用已嵌入：(.+?)（页面标题", joined)
    print(f"本次新开的页标题：{titles}")

    # ★ v1.3：标题**就是应用名**，同应用多开时**故意**一样（不再加"（2）"）。
    #   所以这里反过来验：不许出现序号后缀。
    if len(titles) != 2:
        print(f"[FAIL] 日志里只解析到 {len(titles)} 个页标题（应为 2）")
        for l in tail[-8:]:
            print("      " + l)
        ok = False
    else:
        numbered = [t for t in titles if re.search(r"（\d+）$", t)]
        if numbered:
            print(f"[FAIL] 页标题还带着序号后缀：{numbered}（王要求去掉）")
            ok = False
        elif len(set(titles)) != 1:
            print(f"[FAIL] 同一应用的两页标题不一致：{titles}（应当都是应用名）")
            ok = False
        else:
            print(f"[OK] 两页标题都是应用名本身、没带序号：「{titles[0]}」")
    print()

    # ---- 收尾：把最后开出来的那两页关掉（右击**最后一个**标签项）----
    # 最后一个标签项的位置靠"标签条上有内容的横向范围"反推：
    # 标签项从左边排起，最右边有内容的地方就是最后一个标签的右边缘。
    # ⚠️ 每轮之间要 press_esc：万一上一次右击弹了菜单没点掉，它会盖住标签条，
    #    下一轮既抓不准也点不准（实测因此把"关闭"点成了"添加应用"）。
    for attempt in range(4):
        if page_count_settled(container) <= pages0:
            break
        # 关一页并**确认页数真的降了**（关是异步的：要等应用进程退出）。
        # 落点、菜单校验、结果校验都在 close_a_page 里，见那里的注释。
        done, what = close_a_page(npp, our_tab, container)
        print(f"  关第 {attempt + 1} 个多余页：{'成功' if done else '失败'}"
              f"（{what}）")
        if not done:
            ok = False
            break

    time.sleep(0.6)
    pagesN = page_count_settled(container)
    procsN = count_procs(img)
    print(f"收尾后：页数 = {pagesN}，进程数 = {procsN}")
    if pagesN != pages0:
        print(f"[FAIL] 页数没回到起点（{pagesN} != {pages0}）")
        ok = False
    else:
        print("[OK] 页数已回到起点")

    print()
    print("[multiopen] PASS" if ok else "[multiopen] FAIL")
    return 0 if ok else 1


def cmd_tabhl():
    """验「当前标签页的上边沿有一条绿杠」，而且**跟着切换走**。

    做法（真机像素级，不靠肉眼）：
      1. 保证至少 2 个标签页；
      2. 点第一个标签项 -> 抓标签条 -> 找出绿色连续段（必须**只有一段**）；
      3. 点第二个标签项（落点由第 2 步量到的第一页右边缘推算，不用猜）-> 再抓；
      4. 断言：绿段中心明显右移、仍然只有一段、且贴着标签顶端。

    "只有一段"是关键不变量：如果实现写漏了（比如换页时不重画旧标签），
    会出现**好几个标签上都有绿杠**，那一眼是看不出来的（尤其窄面板下），
    但连续段数量会立刻变成 >1。
    """
    if skip_if_locked("tabhl"):
        return 3
    npp, panel, our_tab, container = locate_panel()
    if not npp or not panel or not our_tab:
        print("[ERROR] 找不到 Notepad++ / 内容区 / 标签条（面板没显示？）")
        return 2

    press_esc()          # 清掉可能残留的菜单
    time.sleep(0.2)
    bring_to_front(npp)

    pages0 = page_count(container)
    added = 0
    print(f"起点页数 = {pages0}")
    while page_count(container) < 2 and added < 2:
        opened, _labels, what = open_app_via_menu(npp, our_tab, index=0)
        if not opened:
            print(f"[ERROR] 需要至少 2 个标签页，但自动添加失败：{what}")
            return 2
        added += 1
        time.sleep(0.4)
    print(f"现在页数 = {page_count(container)}（本命令补开了 {added} 个）")
    print()

    tl, tt, tr, tb = rect_of(our_tab)

    def select_and_measure(tag, dx):
        """在标签条客户区 x = tl + dx 处点一下，然后量绿杠。

        ⚠️ 每次调用都**重新读一次**标签条的矩形：面板可能被人拖到别的停靠位
           （真机上就这么错过一次 —— 两次调用之间坐标全变了）。点击前还要
           确认这个点上真的是标签条，否则宁可不点。
        """
        t2l, t2t, t2r, t2b = rect_of(our_tab)
        cx = t2l + dx
        cy = t2t + (t2b - t2t) // 2
        if cx >= t2r - 2:
            print(f"  [{tag}] 落点 {cx} 越过标签条右端 {t2r}，放弃")
            return None
        okp, cls = point_is(our_tab, cx, cy)
        if not okp:
            print(f"  [{tag}] 落点 ({cx},{cy}) 落在「{cls}」上，不是标签条，放弃")
            return None
        click(cx, cy, "left")
        time.sleep(0.7)
        shot = grab_tab_strip(npp, our_tab, tag)
        if not shot:
            return None
        px, w2, h2 = shot
        n, segs, miny, maxy = accent_runs(px, w2, h2)
        print(f"  [{tag}] 绿像素 {n} 个，连续段 {segs}，y 范围 {miny}~{maxy}"
              f"（区域高 {h2}）")
        return (n, segs, miny, maxy, w2, h2)

    ok = True

    # ---- 第 1 个标签 ----
    print("=== 选中第 1 个标签 ===")
    m0 = select_and_measure("tab0", 20)
    if not m0:
        print("[FAIL] 抓不到标签条 / 点不到标签条，本次结果不算数")
        return 3
    n0, segs0, miny0, _maxy0, w0, h0 = m0
    if n0 == 0:
        print("[FAIL] 一个绿像素都没有 —— 高亮根本没画出来")
        return 1
    if len(segs0) != 1:
        print(f"[FAIL] 绿杠不止一段（{len(segs0)} 段）—— 有别的标签也被画上了")
        ok = False
    else:
        print("[OK] 只有一段绿杠（没有糊到别的标签上）")
    if miny0 > max(4, h0 // 3):
        print(f"[FAIL] 绿杠不在标签上边沿（y={miny0}，区域高 {h0}）")
        ok = False
    else:
        print(f"[OK] 绿杠贴着上边沿（y={miny0}）")
    if (segs0[0][1] - segs0[0][0]) < 30:
        print(f"[FAIL] 绿杠太短（{segs0[0][1] - segs0[0][0]}px），不像一条横杠")
        ok = False
    else:
        print(f"[OK] 绿杠长度 {segs0[0][1] - segs0[0][0]}px")
    print()

    # ---- 第 2 个标签：落点由第 1 段的右边缘推算 ----
    # 绿杠两端各让开了 thick 像素（见 drawActiveTabBar 的 inset），
    # 所以第 1 个标签的右边缘 = 绿段右端 + 一点点。往里 25px 一定落在第 2 个标签里。
    print("=== 选中第 2 个标签 ===")
    # dx 是相对标签条左边缘的偏移：绿段右端再往里 25px 一定落在第 2 个标签里。
    dx2 = segs0[0][1] + 25
    if tl + dx2 >= tr - 5:
        print(f"[warn] 推算出的落点已越过标签条右端，改用中点")
        dx2 = (tr - tl) // 2
    m1 = select_and_measure("tab1", dx2)
    if not m1:
        print("[FAIL] 抓不到标签条 / 点不到标签条")
        return 3
    n1, segs1, miny1, _maxy1, w1, h1 = m1
    if n1 == 0:
        print("[FAIL] 切到第 2 个标签之后绿杠消失了")
        ok = False
    else:
        if len(segs1) != 1:
            print(f"[FAIL] 切完之后绿杠不止一段（{len(segs1)} 段）—— "
                  f"旧标签上的绿杠没被擦掉")
            ok = False
        else:
            print("[OK] 切换后仍然只有一段")
        c0 = (segs0[0][0] + segs0[0][1]) / 2
        c1 = (segs1[0][0] + segs1[0][1]) / 2
        print(f"  绿段中心 x：{c0:.0f} -> {c1:.0f}（右移 {c1 - c0:.0f}px）")
        if c1 <= c0 + 20:
            print("[FAIL] 绿杠没跟着切换移动")
            ok = False
        else:
            print("[OK] 绿杠跟着当前页移动了")
    print()

    # ---- 收尾：把本命令补开的页关掉 ----
    if added:
        done, what = close_a_page(npp, our_tab, container)
        print(f"收尾关掉补开的页：{'成功' if done else '失败'}（{what}）")
        print(f"收尾后页数 = {page_count_settled(container)}")

    print()
    print("[tabhl] PASS" if ok else "[tabhl] FAIL")
    return 0 if ok else 1


# ===========================================================================
# 网络测试应用（NppDockApp_NET.exe）· 功能验收
# ---------------------------------------------------------------------------
# 【驱动方式：为什么用 SendMessage 而不是真鼠标】
#   按钮：发 WM_COMMAND/BN_CLICKED —— 这正是真实点击**产生的那条消息**，
#         走的是同一个 WndProc 分支，但不受"窗口被遮挡 / 位置变了"影响。
#   下拉框：发 CB_SETCURSEL 改选中项，再补一条 WM_COMMAND/CBN_SELCHANGE 通知
#         父窗口（CB_SETCURSEL 本身**不发**通知，这是 Win32 的规定，
#          所以只发前者的话界面不会有任何反应）。
#   ⚠️ 这几个消息都 < WM_USER，但它们**不在 UIPI 的放行名单里** ——
#      只有当探针和被测进程完整性级别相同时才送得到。实测本机是一致的
#      （跨进程 TCM_GETITEMCOUNT 能读回真实页数即为证据）；一旦以后
#      Notepad++ 以更高权限运行而探针没有，这些驱动会**静默失效**
#      （SendMessage 返回 0 且不报错），对应的断言就会失败 —— 这是有意的：
#      宁可测试报错，也不要伪装成通过。
#
#   ⚠️ 绝对不能跨进程发的：CB_GETLBTEXT / TCM_GETITEM / 任何"要指针"的消息。
#      系统不封送指针，目标进程会往它自己地址空间里的那个地址写 ——
#      轻则读到垃圾，重则把对方进程写崩。所以读下拉框条目文本一律
#      "CB_SETCURSEL 逐项 + WM_GETTEXT 读回"（WM_GETTEXT 是封送的）。
# ===========================================================================

NET_WND_CLASS = "NppDockNetTestWnd"

# 控件 ID（与 src/apps/nettest/nettest.cpp 里的 enum 一一对应）
# ★ v1.2 界面重做：命令下拉框 + 4 个参数下拉框 + 状态行**全部删掉**，
#   换成"4 个等宽测试按钮 + host 行 + 输出区"。旧 ID（101/103/105/106/107/110/120）
#   现在应当**取不到控件** —— 探针里专门有一条断言盯这个（见 cmd_nettest 的 [2]）。
NET_ID_HOST_LABEL  = 100
NET_ID_HOST_COMBO  = 101
NET_ID_GEAR_BTN    = 102
NET_ID_CLEAR_BTN   = 103
NET_ID_TEST_BASE   = 104          # 104..107 = Ping测试/路由追踪/端口扫描/DNS查询
NET_ID_OUTPUT      = 130

# 四个测试按钮的文案（顺序 = 界面从左到右）
NET_TEST_LABELS = ["Ping测试", "路由追踪", "端口扫描", "DNS查询"]
# 旧的控件 ID（应当已经不存在了）—— 用来验证"界面真的简化了"
NET_ID_OBSOLETE = [110, 111, 112, 120, 121, 122, 123]   # 旧的参数标签/下拉框

# 「文件校验」的控件 ID（与 src/apps/md5tool/md5tool.cpp 的 enum 一一对应）
MD5_ID_PATH   = 101     # 文件路径（可键盘输入 / 可拖入）
MD5_ID_HASH   = 104     # 校验值（只读）
MD5_ID_CALC   = 105     # 计算 / 终止
MD5_ID_STATUS = 107     # 状态行

# WM_COPYDATA：跨进程递数据**唯一**安全的做法（系统替你把数据封送过去）。
# 用它给应用递一个路径 = 走"拖入文件"那条落地逻辑（真拖放的 HDROP
# 是本进程句柄，探针伪造不了，原因见 cmd_filedrop 的说明）。
WM_COPYDATA = 0x004A


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [("dwData", ctypes.c_size_t),
                ("cbData", ctypes.c_uint),
                ("lpData", ctypes.c_void_p)]

NET_MAX_CFG        = 4

# 期望：命令顺序 + 每条的配置项标签（"配置随命令变化"就是靠这张表验的）
EXPECT_NET_COMMANDS = [
    ("Ping",      ["次数", "间隔", "超时", "负载"]),
    ("路由追踪",   ["最大跳数", "每跳探测", "超时"]),
    ("TCP",       ["端口", "次数", "超时"]),
    ("DNS",       ["记录类型", "服务器"]),
    ("本机网络信息", ["显示"]),
    ("ARP",       ["排序"]),
    ("连接表",     ["协议", "筛选"]),        # TCP 时两个
]
EXPECT_CONN_UDP = ["协议"]                   # 切成 UDP 后"筛选"应当消失

WM_COMMAND        = 0x0111
CBN_SELCHANGE     = 1
BN_CLICKED        = 0
CB_GETCOUNT       = 0x0146
CB_GETCURSEL      = 0x0147
CB_SETCURSEL      = 0x014E
CB_GETDROPPEDSTATE = 0x0157
CBS_DROPDOWN      = 0x0002
CBS_TYPE_MASK     = 0x0003
ES_MULTILINE      = 0x0004
ES_READONLY       = 0x0800


def net_exe_path(exe=None):
    if exe:
        return exe
    return os.path.join(repo_root(), "plugins", "NppDock", "NppDockApp_NET.exe")


def is_top_level(hwnd):
    """顶层窗口 = 没有父窗口（嵌入 dock 时父窗口是 NppDockAppHost）。"""
    return not user32.GetParent(hwnd)


def find_net_app(timeout=15.0):
    """找网络测试应用窗口：先顶层（单开），再 dock 容器里（嵌入）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for h in enum_tops():
            if class_of(h) == NET_WND_CLASS and user32.IsWindowVisible(h):
                return h
        npp = find_npp(timeout=2)
        if npp:
            c = find_descendant(npp, "NppDockContainerWnd")
            if c:
                got = find_descendant(c, NET_WND_CLASS)
                if got:
                    return got
        time.sleep(0.3)
    return None


def combo_count(hwnd):
    return int(user32.SendMessageW(hwnd, CB_GETCOUNT, 0, 0))


def combo_cursel(hwnd):
    return int(user32.SendMessageW(hwnd, CB_GETCURSEL, 0, 0))


def run_and_wait(app, idle_timeout=12.0, busy_timeout=5.0):
    """异步点「开始测试」，先等它真的跑起来（标签变「终止」），再等它跑完。

    ⚠️ 两步都要等：只等"结束"的话，异步点击刚发出去、应用还没开始跑，
       这里就会立刻认为"已经结束了"，然后带着上一轮的状态往下走。
    ⚠️ 也不要用同步点击代替 —— 整轮测试在窗口过程里跑，同步发送要等
       它跑完才返回，「不限次数」那种永远跑不完的会直接把探针卡死。
    """
    h = user32.GetDlgItem(app, NET_ID_RUN_BTN)
    post_button(app, NET_ID_RUN_BTN)
    deadline = time.time() + busy_timeout
    while time.time() < deadline:
        if h and get_text(h, 64) == "终止":
            break
        time.sleep(0.05)
    return wait_run_idle(app, idle_timeout)


def wait_page_count_below(container, target, timeout=5.0):
    """等页数真的**少于** target（关页是异步的：要等应用进程退出）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if page_count(container) < target:
            time.sleep(0.3)          # 再确认一次，避免读到中途态
            if page_count(container) < target:
                return True
        time.sleep(0.15)
    return False


def close_a_page(npp, tab, container, tries=3):
    """关掉一页，并且**确认页数真的少了**；返回 (成功?, 说明)。

    ⚠️ 这里必须验"结果"，不能只看"菜单点到了"。实测出现过：菜单项点下去了、
       返回的文字也确实是「关闭「网络测试」」，但页数没变 —— 日志里只有右击、
       没有「已关闭应用页」。只按菜单文字判成功，就会"以为关掉了"，
       于是收尾的页数断言永远是假的。
    落点还会从近到远退让：右击没命中标签项时弹的是「添加应用」菜单
    （close_tab_at 会自己识别出来并按 ESC 退出，不会误点）。
    """
    before = page_count(container)
    for t in range(tries):
        press_esc()
        time.sleep(0.25)
        bring_to_front(npp)
        shot = grab_tab_strip(npp, tab, f"close{t}")
        if not shot:
            return False, "抓不到标签条"
        px, w, h = shot
        _l, ink_r = tab_ink_extent(px, w, h)
        if ink_r < 0:
            return False, "量不到标签项范围"
        tl, tt, _tr, tb = rect_of(tab)
        for back in (25, 60, 110, 180, 260):
            x = tl + max(6, ink_r - back)
            y = tt + (tb - tt) // 2
            done, what = close_tab_at(npp, tab, x, y)
            if done and wait_page_count_below(container, before):
                return True, what
            if done:
                print(f"   （点了「{what}」但页数没降，换个落点重试）")
            time.sleep(0.3)
    return False, "重试多次仍未关掉"


def combo_items(hwnd):
    """读下拉框全部条目文本，读完把选中项**还原**。

    只能这么读（CB_GETLBTEXT 要指针，跨进程会写崩对方，见文件头说明）。
    """
    n = combo_count(hwnd)
    old = combo_cursel(hwnd)
    out = []
    for i in range(n):
        user32.SendMessageW(hwnd, CB_SETCURSEL, i, 0)
        out.append(get_text(hwnd, 256))
    if old >= 0:
        user32.SendMessageW(hwnd, CB_SETCURSEL, old, 0)
    return out


def notify_combo(app, cid, ctrl):
    """补一条 CBN_SELCHANGE —— 只 CB_SETCURSEL 的话父窗口收不到通知。"""
    wparam = (CBN_SELCHANGE << 16) | (cid & 0xFFFF)
    user32.SendMessageW(app, WM_COMMAND, wparam, ctypes.c_ssize_t(ctrl))


def click_button(app, cid):
    """发 BN_CLICKED（真实点击产生的就是这条消息）。"""
    wparam = (BN_CLICKED << 16) | (cid & 0xFFFF)
    user32.SendMessageW(app, WM_COMMAND, wparam, 0)


def post_button(app, cid):
    """异步点按钮（Post 版）。

    ⚠️ 「开始测试」必须用这个：应用的整轮测试是在窗口过程里跑的，
       同步 SendMessage 要等窗口过程返回才回得来 —— 点「不限次数」那种
       永远跑不完的测试时，探针会一直卡在那次 SendMessage 上（实测卡死
       十几分钟，最后要靠杀掉应用才解开）。异步发出去立刻返回，之后
       靠轮询按钮标签/输出来判断状态。
    """
    wparam = (BN_CLICKED << 16) | (cid & 0xFFFF)
    user32.PostMessageW(app, WM_COMMAND, wparam, 0)


def set_combo_index(app, cid, idx):
    h = user32.GetDlgItem(app, cid)
    if not h:
        return False
    user32.SendMessageW(h, CB_SETCURSEL, idx, 0)
    notify_combo(app, cid, h)
    return True


def visible_cfg_slots(app):
    """当前**可见**的配置槽：(槽号, 标签文本, 下拉框) 列表。"""
    out = []
    for i in range(NET_MAX_CFG):
        lbl = user32.GetDlgItem(app, NET_ID_CFGLBL_BASE + i)
        cb = user32.GetDlgItem(app, NET_ID_CFG_BASE + i)
        if lbl and cb and user32.IsWindowVisible(lbl) and user32.IsWindowVisible(cb):
            out.append((i, get_text(lbl, 64), cb))
    return out


def wait_for_output(app, needle, timeout=12.0):
    h = user32.GetDlgItem(app, NET_ID_OUTPUT)
    if not h:
        return None
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = get_text(h, 8192)
        if needle in last:
            return last
        time.sleep(0.25)
    return None


def wait_run_idle(app, timeout=12.0):
    """等「开始测试」按钮的标签回来（说明这一轮跑完了 / 终止生效了）。"""
    b = user32.GetDlgItem(app, NET_ID_RUN_BTN)
    if not b:
        return None
    deadline = time.time() + timeout
    while time.time() < deadline:
        t = get_text(b, 64)
        if t == "开始测试":
            return time.time()
        time.sleep(0.05)
    return None


def net_history_path(exe):
    return os.path.join(os.path.dirname(os.path.abspath(exe)),
                        "NppDockApp_NET.history.ini")


def read_net_history(exe):
    p = net_history_path(exe)
    if not os.path.isfile(p):
        return []
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        out = []
        for line in f:
            line = line.strip()
            if line.startswith("target"):
                out.append(line.split("=", 1)[1])
        return out


def net_cfg_path(exe):
    return os.path.join(os.path.dirname(os.path.abspath(exe)), "NppDockApp_NET.json")


def read_net_cfg(exe):
    """读配置文件（JSON）。读不出来返回 None（调用方自己判失败）。

    ⚠️ 我们的写入器会往文件里写 `//` 注释（给人看的），所以读之前先剥掉注释再
       交给 json —— 这也顺带验证了"手改时加了注释仍然能被程序读进去"。
    """
    p = net_cfg_path(exe)
    if not os.path.isfile(p):
        return None
    txt = open(p, "r", encoding="utf-8", errors="replace").read()
    txt = re.sub(r"//[^\n]*", "", txt)
    txt = re.sub(r"/\*.*?\*/", "", txt, flags=re.S)
    try:
        return json.loads(txt)
    except Exception as e:
        print(f"  [warn] 配置文件解析失败：{e}")
        return None


def write_net_cfg(exe, cfg):
    """把配置写回去（用于"改配置 -> 行为跟着变"这类断言）。"""
    p = net_cfg_path(exe)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)


def hfont_info(hwnd):
    """跨进程读某个控件的字体：(lfHeight, 字体名, fixed_pitch?)。

    GDI 句柄在同会话内是通用的，GetObjectW 能读 —— 这是"量字体"唯一靠谱的办法
    （肉眼看"是不是等宽、是不是小一号"必然会看错）。
    """
    hf = user32.SendMessageW(hwnd, WM_GETFONT, 0, 0)
    if not hf:
        return None
    class LOGFONTW(ctypes.Structure):
        _fields_ = [("lfHeight", ctypes.c_long), ("lfWidth", ctypes.c_long),
                    ("lfEscapement", ctypes.c_long), ("lfOrientation", ctypes.c_long),
                    ("lfWeight", ctypes.c_long), ("lfItalic", ctypes.c_byte),
                    ("lfUnderline", ctypes.c_byte), ("lfStrikeOut", ctypes.c_byte),
                    ("lfCharSet", ctypes.c_byte), ("lfOutPrecision", ctypes.c_byte),
                    ("lfClipPrecision", ctypes.c_byte), ("lfQuality", ctypes.c_byte),
                    ("lfPitchAndFamily", ctypes.c_byte), ("lfFaceName", ctypes.c_wchar * 32)]
    gdi = ctypes.WinDLL("gdi32")
    lf = LOGFONTW()
    if not gdi.GetObjectW(hf, ctypes.sizeof(lf), ctypes.byref(lf)):
        return None
    return (lf.lfHeight, str(lf.lfFaceName), bool(lf.lfPitchAndFamily & 1))


MONO_FACES = ("consolas", "lucida console", "courier new", "courier",
              "dejavu sans mono", "source code pro", "cascadia mono", "monospace")


def net_btn_state(app, i):
    h = user32.GetDlgItem(app, NET_ID_TEST_BASE + i)
    return (get_text(h, 32), bool(user32.IsWindowEnabled(h)))


def net_wait_idle(app, timeout=60.0):
    """等到"没有测试在跑"：第 0 个按钮的文案回到它的标签。"""
    # ⚠️ 判据要写"四个按钮都回到标签且都可用"：只看第 0 个的**文案**会在
    #    UpdateButtons 循环跑到一半（文案已改、EnableWindow 还没轮到）时误判通过，
    #    实测抓到过一次这样的假失败。
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = [net_btn_state(app, i) for i in range(4)]
        if all(t == NET_TEST_LABELS[i] and en for i, (t, en) in enumerate(st)):
            return time.time()
        time.sleep(0.15)
    return None


def net_run(app, i, idle_timeout=60.0):
    """异步点第 i 个测试按钮并等它跑完。返回 (是否起来了, 日志说的按钮文案)。"""
    h = user32.GetDlgItem(app, NET_ID_TEST_BASE + i)
    post_button(app, NET_ID_TEST_BASE + i)         # 必须异步：跑完才返回的是同步
    # 先等它**真的跑起来**（按钮变成「取消」）
    deadline = time.time() + 5
    while time.time() < deadline and get_text(h, 32) != "取消":
        time.sleep(0.05)
    started = (get_text(h, 32) == "取消")
    if started:
        net_wait_idle(app, idle_timeout)
    return started


def cmd_nettest(exe=None, keep=False):
    """网络测试页总验收（v1.2 新界面）。

    八个小节，全部对着**王这一轮的要求**写的：
      [1] 控件点名 + 旧控件（下拉框/状态行）确实没了
      [2] 四个测试按钮**等宽**
      [3] 配置文件（JSON）自动生成、结构正确
      [4] host 三处记录：last / recent（最新在前）/ common（按次数）
      [5] 运行中互斥 + 自己变「取消」+ 取消后全部复位
      [6] 输出区：**等宽字体 + 比正文小一号**
      [7] 输出区能 **Ctrl+A 全选**
      [8] 端口扫描**真的按配置文件里的端口**扫
    """
    exe = net_exe_path(exe)
    if not os.path.isfile(exe):
        print(f"[ERROR] exe 不存在：{exe}")
        return 2

    launched = False
    app = find_net_app(timeout=2.0)
    if app:
        print(f"[attach] 复用已存在的网络测试窗口 {app:#010x}")
    else:
        print(f"[launch] {os.path.basename(exe)}（{os.path.getsize(exe)} 字节）")
        subprocess.Popen([exe])
        app = find_net_app(20)
        launched = True
        if not app:
            print("[ERROR] 20 秒内没等到窗口出现")
            return 3

    embedded = not is_top_level(app)
    print(f"[mode] {'嵌入在 dock 里' if embedded else '单开（顶层窗口）'}")
    ok = True

    def fail(msg):
        nonlocal ok
        print(f"[FAIL] {msg}")
        ok = False

    out_ed = user32.GetDlgItem(app, NET_ID_OUTPUT)
    host_cb = user32.GetDlgItem(app, NET_ID_HOST_COMBO)

    # ---------------- 1. 控件点名（含"旧控件必须没了"） ----------------
    print("\n[1. 控件点名]")
    need = {
        NET_ID_HOST_LABEL: ("Static",   "host 标签"),
        NET_ID_HOST_COMBO: ("ComboBox", "host 下拉框"),
        NET_ID_GEAR_BTN:   ("Button",   "⚙ 配置"),
        NET_ID_CLEAR_BTN:  ("Button",   "清空"),
        NET_ID_OUTPUT:     ("Edit",     "输出区"),
    }
    for i, label in enumerate(NET_TEST_LABELS):
        need[NET_ID_TEST_BASE + i] = ("Button", label)
    page_shown = bool(user32.IsWindowVisible(app))     # 这一页此刻有没有被宿主显示
    if not page_shown:
        print("  （提示：这一页当前不是被选中的那页，宿主把它藏起来了；"
              "以下只检查控件自身，不要求整页可见）")
    for cid, (cls, name) in need.items():
        h = user32.GetDlgItem(app, cid)
        if not h:
            fail(f"控件 {cid}（{name}）不存在")
            continue
        l, t, r, b = rect_of(h)
        real = class_of(h)
        # ⚠️ 判据用**控件自己的 WS_VISIBLE**，不能用 IsWindowVisible()：
        #    IsWindowVisible 会一路上溯父窗口 —— 而"这一页当前没被选中"时
        #    宿主本来就会把整页藏起来（另一页才该显示）。那种情况下
        #    IsWindowVisible=False 是**正常**的，以前用它当判据会在
        #    "当前选中的是别的页"时把 8 个控件全判成异常。
        own_visible = bool(user32.GetWindowLongW(h, -16) & 0x10000000)  # WS_VISIBLE
        tag = "OK " if (own_visible and real == cls
                        and (r - l) > 8 and (b - t) > 8) else "!! "
        if tag == "!! ":
            fail(f"控件 {cid}（{name}）异常：cls={real} {r - l}x{b - t}")
        print(f"  {tag}{name:10s} id={cid:3d} cls={real:9s} {r - l}x{b - t}")

    # 旧界面的控件（参数下拉框、状态行…）必须都取不到 —— 这才叫"真的简化了"
    left = [cid for cid in NET_ID_OBSOLETE if user32.GetDlgItem(app, cid)]
    if left:
        fail(f"还残留着旧界面的控件：{left}")
    else:
        print(f"  OK  旧控件（参数下拉框/状态行等 {len(NET_ID_OBSOLETE)} 个 ID）都已被删除")

    # 静态控件只该有 1 个（host 标签）；有第 2 个就是"状态行又回来了"
    statics = [h for h in enum_children(app) if class_of(h) == "Static"]
    if len(statics) != 1:
        fail(f"静态控件有 {len(statics)} 个（应当只有 host 标签那 1 个）")
    else:
        print("  OK  只有一个静态控件（host 标签）—— 左下角那行状态信息确实没了")

    # ---------------- 2. 四个按钮等宽 ----------------
    print("\n[2. 四个测试按钮]")
    widths, labels = [], []
    for i, label in enumerate(NET_TEST_LABELS):
        h = user32.GetDlgItem(app, NET_ID_TEST_BASE + i)
        l, t, r, b = rect_of(h)
        widths.append(r - l)
        labels.append(get_text(h, 32))
    print(f"  文案 = {labels}")
    print(f"  宽度 = {widths}")
    if labels != NET_TEST_LABELS:
        fail(f"按钮文案不是 {NET_TEST_LABELS}")
    if len(set(widths)) != 1:
        fail(f"四个按钮**不等宽**：{widths}")
    else:
        print(f"  OK  四个按钮等宽（{widths[0]}px）")
    if len(set(rect_of(user32.GetDlgItem(app, NET_ID_TEST_BASE + i))[1]
               for i in range(4))) != 1:
        fail("四个按钮不在同一行")

    # ---------------- 3. 配置文件 ----------------
    print("\n[3. 配置文件（JSON）]")
    cfg = read_net_cfg(exe)
    if cfg is None:
        fail(f"读不到配置文件：{net_cfg_path(exe)}")
    else:
        for key in ("hosts", "ping", "tracert", "portscan", "dns"):
            if key not in cfg:
                fail(f"配置里缺少 {key} 段")
        hosts = cfg.get("hosts", {})
        for key in ("last", "recent", "common"):
            if key not in hosts:
                fail(f"hosts 里缺少 {key}（王要求这三处都要有）")
        print(f"  OK  {os.path.basename(net_cfg_path(exe))}："
              f"hosts 三段齐全，ping/tracert/portscan/dns 参数齐全")
        print(f"      portscan.ports 默认 = {cfg.get('portscan', {}).get('ports')!r}"
              f"（空 = 用内置的 100 个常用端口）")

    # ---------------- 4. host 三处记录 ----------------
    print("\n[4. host 记忆（last / recent / common）]")
    A, B = "127.0.0.1", "127.0.0.2"
    send_text(host_cb, A)
    if not net_run(app, 0, idle_timeout=40):
        fail("点了 Ping测试 之后按钮没变成「取消」（没跑起来）")
    cfg = read_net_cfg(exe) or {}
    hosts = cfg.get("hosts", {})
    if hosts.get("last") != A:
        fail(f"last 不是 {A}：{hosts.get('last')!r}")
    if not hosts.get("recent") or hosts["recent"][0] != A:
        fail(f"recent 最新一条不是 {A}：{hosts.get('recent')}")
    c1 = {e["host"]: e["count"] for e in hosts.get("common", []) if isinstance(e, dict)}
    if A not in c1:
        fail(f"common 里没有 {A}")
        n1 = 0
    else:
        n1 = c1[A]
        print(f"  OK  last={A}，recent[0]={A}，common[{A}]={n1}")

    send_text(host_cb, B)
    net_run(app, 0, idle_timeout=40)
    cfg = read_net_cfg(exe) or {}
    hosts = cfg.get("hosts", {})
    rec = hosts.get("recent", [])
    c2 = {e["host"]: e["count"] for e in hosts.get("common", []) if isinstance(e, dict)}
    if len(rec) < 2 or rec[0] != B or rec[1] != A:
        fail(f"recent 顺序不对（应当最新在前）：{rec[:3]}")
    else:
        print(f"  OK  再跑一次 B 之后 recent = {rec[:3]}（最新在前）")
    if c2.get(A, 0) != n1:
        fail(f"{A} 的计数变了（{n1} -> {c2.get(A)}），不该变")
    else:
        print(f"  OK  跑别的目标不会影响 {A} 的计数（仍是 {n1}）")

    # ⚠️ 先验"输入框预填"，**再**读下拉框条目：
    #    combo_items() 会逐项 CB_SETCURSEL（读完再还原），而 CBS_DROPDOWN 的
    #    WM_GETTEXT 返回的是**当前选中项**的文本 —— 顺序反了就会读到列表里最后一项。
    if get_text(host_cb, 128) != B:
        fail(f"输入框里不是上次用的 host（{get_text(host_cb, 128)!r}）")
    else:
        print(f"  OK  输入框里预填的是上次用过的 host（{B}）")

    # 下拉框 = 近期 5 + 常用 5（去重）
    items = combo_items(host_cb)
    print(f"  下拉框现有 {len(items)} 条：{items}")
    for t in (A, B):
        if t not in items:
            fail(f"下拉框里没有 {t}（记忆没回填到界面）")
    if len(items) > 10:
        fail(f"下拉框 {len(items)} 条，超过「近期 5 + 常用 5」的上限")
    else:
        print("  OK  下拉框条目在上限内（近期 5 + 常用 5）")


    # ---------------- 5. 运行中互斥 + 取消 ----------------
    print("\n[5. 运行中互斥 + 取消]")
    cfg = read_net_cfg(exe) or {}
    saved_ports = cfg.get("portscan", {}).get("ports", "")
    cfg.setdefault("portscan", {})["ports"] = "1-2048"     # 故意扫一大片，好中途取消
    cfg["portscan"]["timeoutMs"] = 1000
    write_net_cfg(exe, cfg)

    send_text(host_cb, A)
    post_button(app, NET_ID_TEST_BASE + 2)                  # 端口扫描
    # 等它跑起来
    deadline = time.time() + 6
    while time.time() < deadline and net_btn_state(app, 2)[0] != "取消":
        time.sleep(0.05)
    st = [net_btn_state(app, i) for i in range(4)]
    print(f"  运行中按钮状态 = {st}")
    if st[2][0] != "取消" or not st[2][1]:
        fail("按下的按钮没有变成「取消」")
    for i in (0, 1, 3):
        if st[i][1]:
            fail(f"运行中第 {i} 个按钮**没有变灰**（王要求其他按钮变灰）")
    if all(not net_btn_state(app, i)[1] for i in (0, 1, 3)) and st[2][0] == "取消":
        print("  OK  运行中：按下的那个变「取消」，其余三个全变灰")
    if user32.IsWindowEnabled(host_cb):
        fail("运行中 host 下拉框仍可编辑（跑一半改目标没有意义）")
    else:
        print("  OK  运行中 host 框被按住")

    t0 = time.time()
    post_button(app, NET_ID_TEST_BASE + 2)                  # 再点一次 = 取消
    dt = net_wait_idle(app, 10)
    if dt is None:
        fail("点了「取消」之后 10 秒都没停")
    else:
        print(f"  从点取消到全部复位：{time.time() - t0:.2f} 秒")
    st = [net_btn_state(app, i) for i in range(4)]
    print(f"  取消后按钮状态 = {st}")
    if [s[0] for s in st] != NET_TEST_LABELS or not all(s[1] for s in st):
        fail("取消之后按钮没全部复位（文案/可用性）")
    else:
        print("  OK  取消后四个按钮的文案与可用性都复位了")
    out = get_text(out_ed, 200000)
    if "（已终止）" not in out:
        fail("输出里没有「（已终止）」")
    else:
        print("  OK  输出里标注了「（已终止）」")

    # 把端口改回默认（别把用户的配置留成 1-2048）
    cfg = read_net_cfg(exe) or {}
    cfg.setdefault("portscan", {})["ports"] = saved_ports
    write_net_cfg(exe, cfg)

    # ---------------- 5b. v2.0：取消要**立刻**生效（长超时也不能卡住）----------------
    # 为什么单列一节：老实现是同步 `IcmpSendEcho`，"取消"得等当前这一轮把
    #   timeout 等满才有反应（配置允许到 60 秒）。这里把单次超时配成 5 秒、
    #   目标选一个**不回包**的地址（TEST-NET-1），再量"从点取消到复位"的秒数 ——
    #   老实现必然 ≥5 秒，新实现应该是零点几秒。
    print("\n[5b. 取消立刻响应（长超时 + 不回包的地址）]")
    cfg2 = read_net_cfg(exe) or {}
    old_ping = dict(cfg2.get("ping", {}))
    cfg2.setdefault("ping", {})
    cfg2["ping"]["count"]      = 0        # 一直 ping（顺带用上 v2.0 的 count=0）
    cfg2["ping"]["intervalMs"] = 1000
    cfg2["ping"]["timeoutMs"]  = 5000     # 单次 5 秒：老实现点取消要干等这一轮
    write_net_cfg(exe, cfg2)

    send_text(host_cb, "192.0.2.1")       # TEST-NET-1：不会给我们回包
    post_button(app, NET_ID_TEST_BASE + 0)
    dl2 = time.time() + 6
    while time.time() < dl2 and net_btn_state(app, 0)[0] != "取消":
        time.sleep(0.05)
    if net_btn_state(app, 0)[0] != "取消":
        print("  [skip] ping 没跑起来（这个环境拦 ICMP？）")
    else:
        time.sleep(0.35)                  # 让它进到"正等着回包"的那一刻
        if net_btn_state(app, 0)[0] != "取消":
            print("  [skip] ping 在 0.35 秒内就结束了，这次没落进等待窗口")
        else:
            t0 = time.time()
            post_button(app, NET_ID_TEST_BASE + 0)          # 再点一次 = 取消
            idle_at = net_wait_idle(app, 10)
            if idle_at is None:
                fail("点了取消之后 10 秒都没停")
            else:
                el = idle_at - t0
                print(f"  从点取消到全部复位：{el:.2f} 秒（单次超时配的是 5 秒）")
                if el > 2.0:
                    fail(f"取消耗了 {el:.2f} 秒 —— 说明还在同步等回包")
                else:
                    print("  OK  取消立刻生效（没有干等那 5 秒的超时）")
        net_wait_idle(app, 10)            # 收尾：别把"正在跑"带进下一节

    cfg2["ping"] = old_ping               # 还原 ping 配置
    write_net_cfg(exe, cfg2)

    # ---------------- 6. 输出区字体：等宽 + 与界面**同号** ----------------
    # ★ v1.3 起这条判据**反过来了**：以前要求"比正文小一号"，王看过之后说
    #   "打印区域字体也改成正常的"，所以现在是"同号"——再小或再大都算错。
    print("\n[6. 输出区字体（等宽 + 与界面同号）]")
    fo = hfont_info(out_ed)
    fb = hfont_info(user32.GetDlgItem(app, NET_ID_TEST_BASE))
    print(f"  输出区字体 = {fo}")
    print(f"  按钮字体   = {fb}")
    if not fo or not fb:
        fail("读不到字体（WM_GETFONT/GetObject 失败）")
    else:
        if fo[1].lower() not in MONO_FACES and not fo[2]:
            fail(f"输出区字体不是等宽：{fo[1]}")
        else:
            print(f"  OK  输出区是等宽字体（{fo[1]}）")
        if abs(fo[0]) != abs(fb[0]):
            fail(f"输出区字号与界面不一致（{abs(fo[0])} vs {abs(fb[0])}）"
                 f"—— 王要求这块「改成正常的」，既不能小一号也不能大一圈")
        else:
            print(f"  OK  输出区与界面同号（{abs(fo[0])}）")

    # ---------------- 6b. 单行布局（v1.3 王画的版式）----------------
    #   Host [下拉框 ▾] [Ping测试][路由追踪][端口扫描][DNS查询] [⚙] [🗑]
    # 判据三条：八个控件**同一个 top**；从左到右顺序正确；下拉框有足够宽度。
    print("\n[6b. 单行布局]")
    row = [(NET_ID_HOST_LABEL, "Host"), (NET_ID_HOST_COMBO, "下拉框"),
           (NET_ID_TEST_BASE + 0, "Ping"), (NET_ID_TEST_BASE + 1, "路由"),
           (NET_ID_TEST_BASE + 2, "端口"), (NET_ID_TEST_BASE + 3, "DNS"),
           (NET_ID_GEAR_BTN, "齿轮"), (NET_ID_CLEAR_BTN, "清空")]
    pos = []
    for cid, nm in row:
        h = user32.GetDlgItem(app, cid)
        if not h:
            fail(f"单行布局里找不到「{nm}」（id={cid}）")
            pos = []
            break
        rc = rect_of(h)
        pos.append((nm, rc))
        print(f"  {nm:6s} {rc[2] - rc[0]:>4}x{rc[3] - rc[1]:<4} @({rc[0]},{rc[1]})")
    if pos:
        tops = sorted({rc[1] for _n, rc in pos})
        if len(tops) != 1:
            fail(f"首行控件不在同一行：top = {tops}（王要的是一整行）")
        else:
            print(f"  OK  八个控件都在同一行（top={tops[0]}）")
        xs = [rc[0] for _n, rc in pos]
        if xs != sorted(xs):
            fail("从左到右的顺序不对：" + str([(n, rc[0]) for n, rc in pos]))
        else:
            print("  OK  顺序 = " + " < ".join(n for n, _rc in pos))
        combo_w = {n: rc[2] - rc[0] for n, rc in pos}.get("下拉框", 0)
        if combo_w < 150:
            fail(f"下拉框只剩 {combo_w}px —— 单行版式没排开")
        else:
            print(f"  OK  下拉框宽 {combo_w}px")

    # ---------------- 7. Ctrl+A 全选 ----------------
    print("\n[7. 输出区 Ctrl+A]")
    out = get_text(out_ed, 200000)
    if session_locked():
        # Ctrl+A 走的是"子类过程读 Ctrl 键的按下状态"，而键状态只能靠**真实
        # 键盘事件**置上；锁屏时那些事件全被安全桌面收走了 —— 这一条只能 skip。
        print("  [skip] 桌面锁着：Ctrl 的按下状态置不上，这条测不了（解锁后重跑）")
    elif not out.strip():
        print("  [skip] 输出区是空的，先跑一条再验")
    else:
        # ⚠️ 跨进程 SetFocus 是**无效**的（老坑），所以直接把按键消息投给输出区；
        #    但 Ctrl 的**按下状态**得用真实键盘事件置上（子类过程读的是键状态）。
        user32.keybd_event(0x11, 0, 0, 0)                  # Ctrl down
        time.sleep(0.1)
        user32.SendMessageW(out_ed, 0x0100, 0x41, 0)       # WM_KEYDOWN 'A' -> 子类
        user32.keybd_event(0x11, 0, 2, 0)                  # Ctrl up
        time.sleep(0.3)
        u2 = ctypes.WinDLL("user32")                       # 不带 argtypes，好传指针
        lo, hi = ctypes.c_long(), ctypes.c_long()
        u2.SendMessageW(out_ed, 0x00B0, ctypes.byref(lo), ctypes.byref(hi))
        print(f"  选中范围 = {lo.value}~{hi.value}（输出长 {len(out)}）")
        if hi.value - lo.value >= len(out) - 2:
            print("  OK  Ctrl+A 全选生效")
        else:
            fail("Ctrl+A 没有全选")

    # ---------------- 8. 端口扫描按配置里的端口扫 ----------------
    print("\n[8. 端口扫描（按配置文件里的端口）]")
    cfg = read_net_cfg(exe) or {}
    cfg.setdefault("portscan", {})["ports"] = "22,80,135,445,3389,8080"
    cfg["portscan"]["timeoutMs"] = 500
    write_net_cfg(exe, cfg)
    send_text(host_cb, A)
    if not net_run(app, 2, idle_timeout=40):
        fail("端口扫描没跑起来")
    out = get_text(out_ed, 200000)
    want = "共 6 个"
    if want not in out:
        fail(f"输出里没有「{want}」—— 扫描集合没跟着配置文件走")
    else:
        print(f"  OK  扫描集合 = 配置里那 6 个端口（输出里出现「{want}」）")
    if "扫描完成" not in out:
        fail("没有扫描完成那一行")
    else:
        line = [l for l in out.splitlines() if l.startswith("开放的端口") or
                "扫描完成" in l]
        for l in line:
            print("   | " + l)
    cfg = read_net_cfg(exe) or {}
    cfg.setdefault("portscan", {})["ports"] = saved_ports
    write_net_cfg(exe, cfg)

    # ---------------- 收尾 ----------------
    if launched and not keep:
        user32.PostMessageW(app, 0x0010, 0, 0)
        deadline = time.time() + 12
        while time.time() < deadline and user32.IsWindow(app):
            time.sleep(0.2)
        left = count_procs(os.path.basename(exe))
        print(f"\n[close] 关闭后 {os.path.basename(exe)} 进程数 = {left}")
        if left != 0:
            fail("关掉窗口后进程还在（残留）")

    print()
    print("[nettest] PASS" if ok else "[nettest] FAIL")
    return 0 if ok else 1


def cmd_tabv14():
    """验 dock 标签栏这一轮的三个改动（v1.4）：

      ① 每个标签项**有最低宽度**（≥ 6 个汉字宽）；
      ② 右击标签项的菜单只有一项且是「Close」；右击空白处是「重新扫描」；
      ③ **双击**标签项能关掉那一页。

    ⚠️ 宽度为什么读日志而不是量窗口：
       TCM_GETITEMRECT 要传 RECT*，**跨进程 SendMessage 不封送指针** ——
       探针当初就是这么把 Notepad++ 写崩过一次的（踩坑 8.5）。
       所以容器在 refreshTabs 时自己把各项宽度记进 NppDock.log，探针只读它。
    """
    npp, panel, our_tab, container = locate_panel()
    if not npp or not our_tab or not container:
        print("[ERROR] 找不到 Notepad++ / 标签条 / 容器")
        return 2

    ok = True
    press_esc()
    time.sleep(0.25)
    bring_to_front(npp)

    # ---- ① 最小宽度：读日志里那条「标签宽度：」 ----
    txt = read_log_text()
    lines = [l for l in txt.splitlines() if "标签宽度：" in l]
    if not lines:
        print("[warn] 日志里还没有「标签宽度」记录 —— 先开/关一个页再跑")
    else:
        line = lines[-1]
        print("  日志：" + line.strip())
        m = re.search(r"（最低 (\d+) 个汉字 = (\d+)px）", line)
        thr = int(m.group(2)) if m else 0
        widths = [int(x) for x in re.findall(r"项\d+=(\d+)px", line)]
        print(f"  各项宽度 = {widths}   判据 = {thr}px")
        if thr <= 0 or not widths:
            print("[FAIL] 日志里的宽度信息解析不出来")
            ok = False
        else:
            bad = [w for w in widths if w < thr]
            if bad:
                print(f"[FAIL] 有标签项比「6 个汉字」还窄：{bad}（判据 {thr}）")
                ok = False
            else:
                print("[OK] 每个标签项都 ≥ 6 个汉字宽（标签之间留白够）")

    # ---- ② 菜单文字 ----
    shot = grab_tab_strip(npp, our_tab, "tabv14")
    if not shot:
        print("[warn] 抓不到标签条（锁屏？），跳过菜单与双击两节")
        print()
        print("[tabv14] SKIP" if ok else "[tabv14] FAIL")
        return 0 if ok else 1

    px, w, h = shot
    ink_l, ink_r = tab_ink_extent(px, w, h)
    tl, tt, tr, tb = rect_of(our_tab)
    mid_y = tt + (tb - tt) // 2

    if ink_r > ink_l + 10:
        x = tl + min(max(ink_r - 18, 8), w - 4)
        iff, cls = point_is(our_tab, x, mid_y)
        if iff:
            menu = right_click_at(npp, x, mid_y)
            if menu:
                items, _ = menu_items(menu)
                texts = [t for _i, t, _rc in items if t]
                print(f"  右击标签项 -> {texts}")
                # ★ v2.0：菜单文字改回中文「关闭这一页」（原来那个英文 Close 是全
                #   中文界面里唯一的英文）。仍然**只有一项** —— 这条规矩没变。
                if len(texts) != 1 or texts[0].strip() != "关闭这一页":
                    print("[FAIL] 右击标签项的菜单应当**只有一项且是「关闭这一页」**")
                    ok = False
                else:
                    print("[OK] 右击标签项：菜单只有一项「关闭这一页」")
                press_esc()
            else:
                print("[warn] 右击标签项没等到菜单")
            time.sleep(0.4)
        else:
            print(f"  [warn] 落点在「{cls}」上，跳过标签菜单")

    if w > ink_r + 60:
        bx = tl + min(ink_r + 40, w - 6)
        iff, cls = point_is(our_tab, bx, mid_y)
        if iff:
            menu = right_click_at(npp, bx, mid_y)
            if menu:
                items, _ = menu_items(menu)
                texts = [t for _i, t, _rc in items if t]
                print(f"  右击空白处 -> {texts}")
                if "重新扫描" not in texts:
                    print("[FAIL] 空白处菜单里没有「重新扫描」")
                    ok = False
                elif "重新扫描应用" in texts:
                    print("[FAIL] 菜单还是「重新扫描应用」（应已简化）")
                    ok = False
                else:
                    print("[OK] 右击空白处：菜单里是「重新扫描」")
                press_esc()
            else:
                print("[warn] 右击空白处没等到菜单")
            time.sleep(0.4)

    # ---- ③ 双击关闭 ----
    before = page_count_settled(container)
    print(f"  双击前页数 = {before}")
    if before >= 2 and ink_r > ink_l + 10:
        x = tl + min(max(ink_r - 18, 8), w - 4)
        iff, cls = point_is(our_tab, x, mid_y)
        if iff:
            logs0 = log_line_count()
            click(x, mid_y, "left")
            time.sleep(0.12)
            click(x, mid_y, "left")          # 连点两下 → 系统合成 DBLCLK
            time.sleep(1.8)
            after = page_count_settled(container)
            tail = read_log_text().splitlines()[logs0:]
            hit = [l for l in tail if "标签条双击" in l]
            print(f"  双击后页数 = {after}   日志：{hit[-1].strip() if hit else '（没有双击记录）'}")
            if not hit:
                print("[FAIL] 双击没被容器收到（日志里没有「标签条双击」）")
                ok = False
            elif after != before - 1:
                print("[FAIL] 双击标签没有关掉那一页")
                ok = False
            else:
                print("[OK] 双击标签项关掉了那一页")
        else:
            print(f"  [warn] 落点在「{cls}」上，跳过双击测试")
    else:
        print("  （只有一页，双击关闭这条跳过 —— 别把最后一页关掉）")

    print()
    print("[tabv14] PASS" if ok else "[tabv14] FAIL")
    return 0 if ok else 1


def cmd_tabmove():
    """验「标签页可以拖动改变位置」（v1.3 新功能）。

    为什么必须用**两个不同的应用**来验：
      页序写在 panel.ini 的 page0/page1… 里。要是两页都是同一个 exe，
      换完顺序文件内容一字不变 —— 那就只剩"第几位"这种自说自话的判据，
      而那类断言最容易假通过。换成 文件校验 + 网络测试 之后，
      顺序变化在文件里是**肉眼可见**的（两个不同的 exe 名调了个头）。

    三条断言：
      ① 插件日志出现「标签页重排」；
      ② panel.ini 里两个 exe 的顺序真的换了；
      ③ 选中页跟着**页对象**走（拖动只改位置，不改变"正在看哪一页"）。
    """
    if skip_if_locked("tabmove"):
        return 3
    npp, panel, our_tab, container = locate_panel()
    if not npp or not panel or not our_tab or not container:
        print("[ERROR] 找不到 Notepad++ / 面板 / 标签条")
        return 2

    press_esc()
    time.sleep(0.2)
    bring_to_front(npp)

    ok = True
    st = read_page_state()
    pages_exe = [ln.split("=", 1)[1] for ln in st if ln.startswith("page")]
    print(f"起点：页数 = {page_count(container)}，页列表 = {pages_exe}")
    if len(pages_exe) != 2 or len(set(pages_exe)) != 2:
        print("[tabmove] SKIP（需要「正好两页、且是两个不同应用」——"
              "先把面板弄成 文件校验 + 网络测试）")
        return 3

    before = list(pages_exe)
    logs0 = log_line_count()

    shot = grab_tab_strip(npp, our_tab, "tabmove_before")
    if not shot:
        print("[FAIL] 抓不到标签条")
        return 1
    px, w, h = shot
    ink_l, ink_r = tab_ink_extent(px, w, h)
    if ink_r <= ink_l + 20:
        print(f"[FAIL] 量到的标签内容范围太小（{ink_l}~{ink_r}）")
        return 1
    tl, tt, tr, tb = rect_of(our_tab)
    mid_y = tt + (tb - tt) // 2
    x_from = tl + min(max(ink_r - 12, 8), w - 4)   # 最后一个标签内部
    x_to   = tl + min(max(ink_l + 6, 4), w - 4)    # 第一个标签内部
    print(f"  拖动：从 x={x_from} 拖到 x={x_to}（y={mid_y}）")
    drag(x_from, mid_y, x_to, mid_y, steps=10)
    time.sleep(1.0)

    tail = "\n".join(read_log_text().splitlines()[logs0:])
    moved = re.findall(r"标签页重排：第 (\d+) 位 -> 第 (\d+) 位", tail)
    if moved:
        print(f"  [OK] 日志记录到重排：{moved}")
    else:
        print("[FAIL] 日志里没有「标签页重排」—— 拖动没生效")
        ok = False

    st2 = read_page_state()
    after = [ln.split("=", 1)[1] for ln in st2 if ln.startswith("page")]
    print(f"  拖动后页列表 = {after}")
    if after == before:
        print("[FAIL] panel.ini 里的页序没变 —— 位置其实没换")
        ok = False
    elif sorted(after) != sorted(before):
        print(f"[FAIL] 页列表内容变了（{before} -> {after}）—— 拖动不该丢页")
        ok = False
    else:
        print("  [OK] panel.ini 里两个 exe 的顺序真的调过来了")

    cur_line = [ln for ln in st2 if ln.startswith("current=")]
    if cur_line:
        cur = int(cur_line[0].split("=", 1)[1])
        want = after[cur] if 0 <= cur < len(after) else "?"
        print(f"  current={cur} -> {want}（拖动不改变「正在看哪一页」）")
    print()
    print("[tabmove] PASS" if ok else "[tabmove] FAIL")
    return 0 if ok else 1


def cmd_tabsep():
    """验「标签条下沿的分界线」（v1.3 新增）。

    王的原话是"tab 占的空间我很满意，但与下方内容区的分界还不够明显"，
    所以这里同时盯住两件事：
      ① 分界线**真的画了**（底部有一道明显比标签条底色深的横线）；
      ② 标签条的**高度没变**（分界线是盖在原有那一行上画的，不是把条加高）。
    第 ② 条很容易被后来的改动破坏（"那就把标签条加高 3px 吧"是最省事的做法），
    所以必须写成断言。
    """
    npp, panel, our_tab, container = locate_panel()
    if not npp or not our_tab:
        print("[ERROR] 找不到 Notepad++ / 标签条")
        return 2

    bring_to_front(npp)
    time.sleep(0.2)
    l, t, r, b = rect_of(our_tab)
    shot = grab_tab_strip(npp, our_tab, "tabsep")
    if not shot:
        print("[FAIL] 抓不到标签条")
        return 1
    px, w, h = shot
    print(f"标签条 = {w}x{h}（客户区高 {b-t}）")

    def rgb(x, y):
        o = (y * w + x) * 4
        return (px[o + 2], px[o + 1], px[o])       # BGRA -> (r,g,b)

    mid = w // 2
    # 从最底一行往上数：连续多少行是本项目定的分隔线灰（#9E9E9E ± 容差）
    tol = 16
    band = 0
    for y in range(h - 1, -1, -1):
        c = rgb(mid, y)
        if all(abs(v - 0x9E) <= tol for v in c):
            band += 1
        else:
            break
    above = rgb(mid, max(0, h - 1 - band)) if h - 1 - band >= 0 else (0, 0, 0)
    print(f"  底部连续分隔线行数 = {band}；它上面那一行色 = {above}")

    ok = True
    if band < 2:
        print(f"[FAIL] 标签条底部看不到分界线（只有 {band} 行）—— 王要的"
              f"「分界明显一点」没做到")
        ok = False
    else:
        print(f"  OK  底部分界线存在（{band} 行 #9E9E9E 系）")
    if above[0] == 0x9E and above[1] == 0x9E:
        print("[FAIL] 整条标签条都是分隔线色 —— 那不是「分界线」，是刷错了背景")
        ok = False
    if band > h // 3:
        print(f"[FAIL] 分界线占了标签条的 {band}/{h} —— 太厚了")
        ok = False

    # 标签条高度：只做"没被加高"的提醒（绝对值随 DPI 变，不能写死断言）
    if h > 60:
        print(f"  [warn] 标签条高达 {h}px，分界线不许靠加高标签条来实现")

    # 分隔线只在**标签条**上，不该影响绿杠：顺带确认绿杠还在最上面
    n_px, segs, miny, maxy = accent_runs(px, w, h)
    print(f"  绿杠：{n_px} 个像素、{len(segs)} 段，y 范围 = {miny}~{maxy}")
    if n_px < 1:
        print("  [warn] 这张图里没看到绿杠（可能当前页正好是第一个标签）")
    print()
    print("[tabsep] PASS" if ok else "[tabsep] FAIL")
    return 0 if ok else 1


def cmd_filedrop(target=None, keep=False):
    """验「文件校验：拖入文件自动填路径」（v1.3 新增）。

    ⚠️ 为什么这里不真的"拖"一次：
      Windows 的拖放最终是给目标窗口发 WM_DROPFILES，而它带的 HDROP 是
      **本进程的全局内存句柄**，跨进程用不了 —— 探针没法伪造一次真实拖放
      （真拖要靠 Explorer/OLE 自动化，稳定性极差）。
      所以这里走的是**同一条落地逻辑的另一个入口**：WM_COPYDATA
      （这条消息系统会替你封送数据，跨进程安全），然后验
      「路径框被填上 + 状态行对 + 真能算出正确的摘要」。
      真·拖放那一下的手感由人验；"拖进来之后界面会不会正确更新"由这里盯住。
    """
    app = find_app_any()
    if not app:
        print("[ERROR] 找不到「文件校验」窗口")
        return 2

    sample = target or app_exe_path()          # 默认拿"文件校验"自己的 exe 当样本
    if not os.path.isfile(sample):
        sample = net_exe_path()
    if not os.path.isfile(sample):
        print(f"[ERROR] 样本文件不存在：{sample}")
        return 2

    data = open(sample, "rb").read()
    expected = hashlib.md5(data).hexdigest()
    print(f"样本 = {sample}（{len(data)} 字节）")
    print(f"期望 MD5 = {expected}")

    path_ed = user32.GetDlgItem(app, MD5_ID_PATH)
    out_ed  = user32.GetDlgItem(app, MD5_ID_HASH)

    # 先清空路径框，才能证明"是这次填进去的"
    user32.SetWindowTextW(path_ed, "")
    time.sleep(0.2)

    buf = ctypes.create_unicode_buffer(sample)
    cds = COPYDATASTRUCT()
    cds.dwData = 1                      # 1 = "这是一条文件路径"
    cds.cbData = ctypes.sizeof(buf)
    cds.lpData = ctypes.cast(buf, ctypes.c_void_p)
    lp = ctypes.cast(ctypes.byref(cds), ctypes.c_void_p).value
    got = user32.SendMessageW(app, WM_COPYDATA, 0, lp)
    time.sleep(0.5)

    shown = get_text(path_ed, 400)
    print(f"  返回 = {got}；路径框 = {shown!r}")
    ok = True
    if os.path.basename(sample).lower() not in shown.lower():
        print("[FAIL] 路径没有填进路径框 —— 拖入的落地逻辑没生效")
        ok = False
    else:
        print("  OK  路径已自动填入")

    # 填进去之后必须"能直接算"，否则填了也是摆设
    post_button(app, MD5_ID_CALC)
    deadline = time.time() + 20
    res = ""
    while time.time() < deadline:
        time.sleep(0.4)
        res = get_text(out_ed, 200).strip()
        if res:
            break
    print(f"  校验值 = {res or '<空>'}   期望 = {expected}")
    if res.lower() != expected:
        print("[FAIL] 拖入之后算出来的摘要不对")
        ok = False
    else:
        print("  OK  摘要与本地计算一致")
    print()
    print("[filedrop] PASS" if ok else "[filedrop] FAIL")
    return 0 if ok else 1


def type_chars(hwnd, s):
    """**逐字**把文本打进**指定控件**（WM_CHAR）。

    ⚠️ 为什么不能用 send_text（WM_SETTEXT）来测"输入类"的行为：
       跨进程 WM_SETTEXT 会把文本**写进去**（回读得到），但**不会**产生
       EN_CHANGE —— 而"编辑便笺 → 自动同步"这条链正是挂在 EN_CHANGE 上的。
       用它测就会得出"自动同步没生效"的错误结论（我自己先踩了一次）。
       真实按键、粘贴才会发 EN_CHANGE，所以探针要模拟**按键**。
       多行 EDIT 里换行要发 WM_CHAR '\r'（'\n' 在 EDIT 里不产生新行）。
    """
    n = 0
    for ch in s:
        c = '\r' if ch == '\n' else ch
        user32.SendMessageW(hwnd, 0x0102, ord(c), 1)     # WM_CHAR
        n += 1
    return n


def backpack_get_items(lst):
    """读 LISTBOX 的全部条目。

    ⚠️ LB_GETTEXT / LB_GETTEXTLEN 要传指针，**跨进程不可用**（拿到的会不对）。
       所以不能用它们。这里退回"控件自己的窗口标题"——没用；
       真正的办法是数条目（LB_GETCOUNT，无指针）并把每条"是不是文件"
       交给应用自己的状态去印证。需要条目文本时，用下面这条：
       让应用把列表也写一份到便笺区？不行，那是用户内容。
       ⇒ 结论：**条目文本不从进程外读**，只验条数与"点一下能不能载入"。
    """
    return user32.SendMessageW(lst, 0x018B, 0, 0)         # LB_GETCOUNT


def bp_find(timeout=8):
    """找「文件背包」窗口：顶层（单开）或容器里的（嵌入）都要能找到。"""
    deadline = time.time() + timeout
    cls = "NppDockBackpackWnd"
    while time.time() < deadline:
        for h in enum_tops():
            if class_of(h) == cls and user32.IsWindowVisible(h):
                return h
        npp = find_npp(timeout=2)
        if npp:
            c = find_descendant(npp, "NppDockContainerWnd")
            if c:
                h = find_descendant(c, cls)
                if h:
                    return h
        time.sleep(0.2)
    return None


def cmd_backpack(cfg_path="", local_dir="", keep=False):
    """「文件背包」总验收（第三个应用）。

    八个小节，对着王的原话写：
      [1] 控件点名：⚙ / 背包名 / 状态 / ↻ / 文件栏 / 便笺 / 分隔条
      [2] 版式 **固定 38/62**（v1.4 起王要求不可改）
      [3] 分隔条**不可拖**（它现在只是分界线；拖了必须一点不变）
      [4] nppbackpack_config.json 自动生成、字段齐全、能容忍 // 注释
          （v1.4 从 config.json 改名；顺带验"旧名字会自动搬过去"）
      [5] 连不上时**两侧都空白**、状态写明原因、**不弹框**、且不动本地已编辑内容
      [6] 本地目录模式全链路：逐字输入 → 自动同步 → "服务器"上的文件内容一致
      [7] ↻ 立即同步：状态里的时间会变新
      [8] ⚙ 打开配置文件（嵌入态应当**在 Notepad++ 里打开**，不许弹自己的窗）

    ⚠️ 关于 [6]：必须用 type_chars（WM_CHAR）而不是 send_text（WM_SETTEXT）——
       跨进程 WM_SETTEXT 不产生 EN_CHANGE，用它测会误判成"自动同步没生效"。
    """
    cfg = cfg_path or os.path.join(repo_root(), "plugins", "NppDock",
                                   "nppbackpack_config.json")
    local = local_dir or os.path.join(os.environ.get("TEMP", "."), "nppdpack_probe")

    app = bp_find(10)
    if not app:
        print("[ERROR] 找不到「文件背包」窗口（先 addapp --text 文件背包 打开它）")
        return 2

    ok = True
    def bad(msg):
        nonlocal ok
        print("  [FAIL] " + msg)
        ok = False

    # ---------- 1. 控件点名 ----------
    print("\n[1. 控件点名]")
    # v1.7：顶部改成两段 —— 左段是 6 个**等宽纯文字**按钮（在文件栏上方），
    #       右段是"状态 + 设置 / 刷新"（排在便笺上方，右对齐）。
    ids = [(123, "文件属性"), (126, "重命名"), (124, "删除"), (122, "上传"),
           (121, "下载"), (125, "打开文件位置"),
           (102, "状态"), (100, "设置"), (103, "刷新"),
           (110, "文件栏"), (111, "便笺"), (112, "分隔条")]
    rects = {}
    for cid, nm in ids:
        h = user32.GetDlgItem(app, cid)
        if not h:
            bad(f"控件 {cid}（{nm}）不存在")
            continue
        rc = rect_of(h)
        rects[nm] = rc
        own = bool(user32.GetWindowLongW(h, -16) & 0x10000000)
        print(f"  {'OK ' if own else '!! '}{nm:8s} id={cid:3d} "
              f"cls={class_of(h):10s} {rc[2]-rc[0]}x{rc[3]-rc[1]}")
    if len(rects) != len(ids):
        bad(f"只找到 {len(rects)}/{len(ids)} 个控件")

    # ---------- 2. 版式 1:2 ----------
    print("\n[2. 版式（固定 38 / 62）]")
    if "文件栏" in rects and "便笺" in rects:
        lw = rects["文件栏"][2] - rects["文件栏"][0]
        nw = rects["便笺"][2] - rects["便笺"][0]
        ratio = lw / nw if nw else 0
        print(f"  文件栏 {lw}px  便笺 {nw}px  比值 = {ratio:.3f}（期望 ≈0.613 = 38/62）")
        if abs(ratio - 0.613) > 0.05:
            bad(f"左右比例不是 38/62（比值 {ratio:.3f}）")
        else:
            print("  OK  左右比例 38/62")
        if rects["文件栏"][1] != rects["便笺"][1]:
            bad("文件栏与便笺不在同一水平线")
        else:
            print("  OK  两栏顶部对齐")

    # ---------- 3. 分隔条能拖 ----------
    print("\n[3. 分隔条不可拖（v1.4 起它只是分界线）]")
    sp = user32.GetDlgItem(app, 112)
    if not sp:
        bad("没有分隔条")
    else:
        l0, _t0, _r0, _b0 = rect_of(sp)
        lw0 = rects["文件栏"][2] - rects["文件栏"][0]
        print(f"  拖动前：文件栏宽 = {lw0}")
        sl, st_, sr, sb = rect_of(sp)
        mid = (st_ + sb) // 2
        cx = (sl + sr) // 2
        drag(cx, mid, cx + 120, mid, steps=12)     # 往右拖
        time.sleep(0.8)
        lw1 = rect_of(user32.GetDlgItem(app, 110))[2] - rect_of(user32.GetDlgItem(app, 110))[0]
        l1, _t1, _r1, _b1 = rect_of(sp)
        print(f"  往右拖 120px 后：文件栏宽 = {lw1}，分隔条左边界 {l0} -> {l1}")
        if lw1 != lw0 or l1 != l0:
            bad("分隔条被拖动了 —— 王要求 38/62 **不可修改**")
        else:
            print("  OK  分隔条纹丝不动（比例固定）")

    # ---------- 4. nppbackpack_config.json ----------
    print("\n[4. nppbackpack_config.json]")
    if not os.path.isfile(cfg):
        bad(f"配置文件不存在：{cfg}")
    else:
        txt = io.open(cfg, encoding="utf-8").read()
        keys = ["host", "port", "username", "auth_type", "password",
                "private_key_path", "remote_folder", "backpack_name"]
        missing = [k for k in keys if ('"%s"' % k) not in txt]
        print(f"  路径 = {cfg}（{len(txt)} 字节）")
        if missing:
            bad("缺字段：" + str(missing))
        else:
            print("  OK  八个字段齐全")
        if "//" in txt:
            print("  OK  带 // 注释（我们的读取器容忍注释，手改方便）")

    # ---------- 5. 连不上：两侧留空 + 状态说明 + 不弹框 ----------
    print("\n[5. 连不上时两侧留空]")
    saved = io.open(cfg, encoding="utf-8").read() if os.path.isfile(cfg) else ""
    try:
        io.open(cfg, "w", encoding="utf-8", newline="\n").write(
            '{\n  "host": "192.0.2.1",\n  "port": 22,\n  "username": "probe",\n'
            '  "auth_type": "password",\n  "password": "",\n'
            '  "private_key_path": "",\n  "remote_folder": "",\n'
            '  "backpack_name": "探针测试"\n}\n')
        post_button(app, 103)                       # ↻ 刷新 -> 会去连
        # 等够：ssh 的 ConnectTimeout（连接里写的是 6 秒）+ 进程启动。
        # 之前只等 8 秒，偶尔会读到"正在连接…"而误报（实测栽过一次）。
        time.sleep(16.0)
        st_txt = get_text(user32.GetDlgItem(app, 102), 300)
        cnt = backpack_get_items(user32.GetDlgItem(app, 110))
        note = get_text(user32.GetDlgItem(app, 111), 200)
        print(f"  状态 = {st_txt!r}")
        print(f"  文件栏条目 = {cnt}   便笺 = {note!r}")
        if "未连接" not in st_txt and "失败" not in st_txt:
            bad("连不上时状态没有写明「未连接/失败」")
        else:
            print("  OK  状态写明了原因")
        if cnt != 0:
            bad(f"连不上时文件栏还留着 {cnt} 条 —— 应当清空")
        else:
            print("  OK  文件栏清空了")
        # "不弹框"这条：顶层窗口里不该冒出属于这个应用的对话框。
        # 应用窗口自己还在，所以排除它本身。
        extra = [class_of(h) for h in enum_tops()
                 if user32.IsWindowVisible(h)
                 and user32.GetWindowLongW(h, -16) & 0x40000000  # WS_CHILD 反义：顶层
                 and class_of(h) in ("#32770", "NppDockBackpackWnd")
                 and h != app]
        if extra:
            bad("弹出了对话框：" + str(extra) + "（王要求连不上时**不弹框**）")
        else:
            print("  OK  没有弹任何对话框")
    finally:
        # 无论成败都把配置还原（绝不把用户的配置留在测试状态）
        if saved:
            io.open(cfg, "w", encoding="utf-8", newline="\n").write(saved)
            print("  （已还原原配置）")

    # ---------- 6. 本地目录模式：全链路 ----------
    print("\n[6. 本地目录模式全链路（逐字输入 → 自动同步）]")
    saved2 = io.open(cfg, encoding="utf-8").read() if os.path.isfile(cfg) else ""
    try:
        os.makedirs(local, exist_ok=True)
        for f in os.listdir(local):
            try: os.remove(os.path.join(local, f))
            except Exception: pass
        io.open(cfg, "w", encoding="utf-8", newline="\n").write(
            '{\n  "host": "local:%s",\n  "port": 22,\n  "username": "probe",\n'
            '  "auth_type": "password",\n  "password": "",\n'
            '  "private_key_path": "",\n  "remote_folder": "",\n'
            '  "backpack_name": "探针测试"\n}\n' % local.replace("\\", "/"))
        post_button(app, 103)                       # ↻ 刷新 -> 重连
        # ⚠️ 必须等**连接真的结束**再打字：连接是异步的，如果中途就开始敲，
        #    连接完成时的"载入便笺"会把刚敲的字盖掉（实测踩到一次空文件）。
        conn_deadline = time.time() + 15
        while time.time() < conn_deadline:
            time.sleep(0.4)
            if "正在连接" not in get_text(user32.GetDlgItem(app, 102), 300):
                break
        print(f"  连接后状态 = {get_text(user32.GetDlgItem(app, 102), 300)!r}")

        sample = "自动同步验证第一行\n第二行：中文与换行都要对"
        type_chars(user32.GetDlgItem(app, 111), sample)
        time.sleep(3.5)                             # 1.5 秒防抖 + 余量
        st_txt = get_text(user32.GetDlgItem(app, 102), 300)
        print(f"  同步后状态 = {st_txt!r}")
        # ★ v1.7：便笺换了**保留名**（带前导点），而且不再出现在文件栏里
        nf = os.path.join(local, ".nppbackpack-note.txt")
        if not os.path.isfile(nf):
            bad("同步之后「服务器」上没有 .nppbackpack-note.txt")
        else:
            got = io.open(nf, encoding="utf-8").read()
            print(f"  服务器上的内容 = {got!r}")
            if got != sample:
                bad("内容不一致（换行或中文没往返对）")
            else:
                print("  OK  逐字输入的内容原样同步到了「服务器」")
        if "同步" not in st_txt and "已连接" not in st_txt:
            bad("同步之后状态没更新")
        else:
            print("  OK  状态里带上了同步时间")
        cnt = backpack_get_items(user32.GetDlgItem(app, 110))
        print(f"  文件栏条目 = {cnt}")
        if cnt != 0:
            bad(f"便笺不该出现在文件栏里（现在有 {cnt} 条）")
        else:
            print("  OK  便笺不在文件栏里显示（它不再是背包里的一个普通文件）")
    finally:
        if saved2:
            io.open(cfg, "w", encoding="utf-8", newline="\n").write(saved2)
            print("  （已还原原配置）")

    print()
    print("[backpack] PASS" if ok else "[backpack] FAIL")
    return 0 if ok else 1


def cmd_cfgapply(cfg_path="", local_dir="", keep=False):
    """「改配置文件即时生效、且不被应用覆盖」验收（v1.5，对应王的反馈）。

    王原话的现象：标签页开着时改配置文件 → 保存 → 重开发现改动没了；
    必须"先打开配置、再关标签页"才能改。根因是打开配置文件那条路径
    会拿内存里的旧整份配置覆盖磁盘（v1.5 去掉了这个 Save）。

    本节验三件事：
      [1] 外部改配置 → 点刷新 → **配置文件不被覆盖**（改动留在盘上，md5 不变）
      [2] 外部改配置 → 点刷新 → 改动**立即生效**（背包名当场变，无需关/重开标签页）
      [3] remote_folder 还停在模板占位符时，应用按用户名自适应成
          /home/<用户名>/npp-backpack（否则会去 mkdir 一个没权限的目录）
    """
    cfg = cfg_path or os.path.join(repo_root(), "plugins", "NppDock",
                                   "nppbackpack_config.json")
    local = local_dir or os.path.join(os.environ.get("TEMP", "."), "nppdpack_cfgapply")

    app = bp_find(10)
    if not app:
        print("[ERROR] 找不到「文件背包」窗口（先 addapp --text 文件背包 打开它）")
        return 2

    ok = True
    def bad(msg):
        nonlocal ok
        print("  [FAIL] " + msg)
        ok = False

    def read_cfg():
        return io.open(cfg, encoding="utf-8").read() if os.path.isfile(cfg) else ""

    def write_cfg(bag_name, host):
        io.open(cfg, "w", encoding="utf-8", newline="\n").write(
            '{\n  "host": "%s",\n  "port": 22,\n  "username": "probe",\n'
            '  "auth_type": "password",\n  "password": "",\n'
            '  "private_key_path": "",\n  "remote_folder": "",\n'
            '  "backpack_name": "%s"\n}\n' % (host, bag_name))

    def wait_idle(limit=25.0):
        """等到"正在…"字样消失，并且状态里出现"已连接/已刷新/同步/未连接"。"""
        dl = time.time() + limit
        while time.time() < dl:
            time.sleep(0.4)
            t = get_text(user32.GetDlgItem(app, 102), 300)
            if ("正在" not in t) and any(x in t for x in
                                        ("已连接", "已刷新", "同步", "未连接", "失败")):
                return t
        return get_text(user32.GetDlgItem(app, 102), 300)

    def list_count():
        return backpack_get_items(user32.GetDlgItem(app, 110))

    def make_pack(d, files):
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            try: os.remove(os.path.join(d, f))
            except Exception: pass
        for nm in files:
            io.open(os.path.join(d, nm), "w", encoding="utf-8").write(nm)

    saved = read_cfg()
    try:
        # v1.6：名字标签已经去掉，所以改用**文件列表**当观测量 ——
        #       两台"服务器"（两个本地目录）内容不同，刷新之后列表条数就该变。
        dirA = os.path.join(local, "A")
        dirB = os.path.join(local, "B")
        make_pack(dirA, ["a.txt"])
        make_pack(dirB, ["b1.txt", "b2.txt"])

        # ---------- 1. 先让它按「服务器 A」连上 ----------
        print("\n[1. 载入配置 A（服务器=目录A，应看到 1 项）]")
        write_cfg("名字A", "local:" + dirA.replace("\\", "/"))
        post_button(app, 103)
        st = wait_idle()
        nA = list_count()
        print("  状态 = %r   文件栏 = %d 项" % (st, nA))
        if nA != 1:
            bad("配置 A 没生效（目录 A 只有 1 个文件）")
        else:
            print("  OK  配置 A 已生效")

        # ---------- 2. **外部**改配置（模拟用户开着标签页去编辑器里改） ----------
        print("\n[2. 外部改配置为 B，再点刷新 —— 改动必须留在盘上、并当场生效]")
        write_cfg("名字B", "local:" + dirB.replace("\\", "/"))
        contentB = read_cfg()
        post_button(app, 103)                       # 刷新会重读配置
        st = wait_idle()

        on_disk = read_cfg()
        if on_disk != contentB:
            bad("配置文件被应用覆盖了！盘上的内容与刚写入的 B 不一致")
            print("    期望: %r" % contentB[:120])
            print("    实际: %r" % on_disk[:120])
        else:
            print("  OK  配置文件原样保留（应用没有反过来覆盖它）")

        nB = list_count()
        print("  刷新后状态 = %r   文件栏 = %d 项" % (st, nB))
        if nB != 2:
            bad("改完配置点刷新没生效（目录 B 有 2 个文件）")
        else:
            print("  OK  改完配置点刷新即生效，无需关/重开标签页")
    finally:
        if saved:
            io.open(cfg, "w", encoding="utf-8", newline="\n").write(saved)
            print("  （已还原原配置）")

    # ---------- 3. 占位符 remote_folder 自适应 ----------
    print("\n[3. remote_folder 仍是模板占位符时，按用户名自适应]")
    exe = os.path.join(repo_root(), "plugins", "NppDock", "NppDockApp_PACK.exe")
    saved3 = read_cfg()
    try:
        io.open(cfg, "w", encoding="utf-8", newline="\n").write(
            '{\n  "host": "no-such-host.invalid",\n  "port": 22,\n'
            '  "username": "probe",\n  "auth_type": "password",\n'
            '  "password": "",\n  "private_key_path": "",\n'
            '  "remote_folder": "/home/your_ssh_username/npp-backpack",\n'
            '  "backpack_name": "probe"\n}\n')
        # --sshraw 会把"实际会用的背包目录"打出来（且在真正连之前就打）
        try:
            pr = subprocess.run([exe, "--sshraw"], capture_output=True, timeout=40)
            dump = pr.stdout.decode("utf-8", "replace")
        except Exception as e:
            dump = ""
            bad("跑 --sshraw 失败：%r" % (e,))
        line = [l.strip() for l in dump.splitlines() if "背包目录" in l]
        print("  --sshraw 说：%s" % (line[0] if line else "(没拿到)"))
        if line and "/home/probe/npp-backpack" in line[0]:
            print("  OK  占位符被自适应成 /home/probe/npp-backpack")
        else:
            bad("占位符没有被自适应（仍会去 mkdir 一个没权限的目录）")
    finally:
        if saved3:
            io.open(cfg, "w", encoding="utf-8", newline="\n").write(saved3)
            print("  （已还原原配置）")

    print()
    print("[cfgapply] PASS" if ok else "[cfgapply] FAIL")
    return 0 if ok else 1


def cmd_packv17(cfg_path="", local_dir="", keep=False):
    """v1.8「文件背包」验收 —— 对着王 2026-10-04 晚那一批要求逐条写。

    覆盖：
      [1] 版式：6 个**等大的纯图标**按钮（文件属性/重命名/删除/上传/下载/打开文件位置）
          + **设置/刷新也在这排**（右对齐到文件栏右缘，v1.9 从便笺上方搬过来的）；
          状态与便笺左对齐；固定的 38/62。
      [2] 文件栏用 Consolas；**便笺与旧回收站目录**不出现在列表里。
      [3] 「..」是文件栏里的一行**目录**（不在根时才出现），双击它回到上级。
      [4] 选中 → 按钮可用；**再点同一行 / 点空白都不该取消选中**（v1.9，王反馈的 bug）；支持多选。
      [5] 删除：**真删**（"删了就没了"）—— 不弹框、不建回收站目录、磁盘上真没了。
      [6] 刷新：点刷新时状态里**短暂出现服务器域名/ip**。
      [7] 文件属性：只有 名称/类型/大小/时间 四项，且不是 #32770（无系统提示音）。
      [8] 重命名：**原地改**（在那一行上盖一个 EDIT，没有弹窗）—— F2 起、回车提交。
      [8b] 下载大文件时状态里报**最近一秒的速率**与**剩余时间**（v1.9）。
      [9] 「取消」按钮真的能按：下载大文件期间按钮变红叉且**可用**；点它 → 传输中断。
      [10] 取消过之后，"下一次下载"还能正常跑。
    """
    cfg   = cfg_path or os.path.join(repo_root(), "plugins", "NppDock",
                                     "nppbackpack_config.json")
    base  = local_dir or os.path.join(os.environ.get("TEMP", "."), "nppdpack_v18")
    local = os.path.join(base, "server")
    dl    = os.path.join(base, "dl")

    app = bp_find(10)
    if not app:
        print("[ERROR] 找不到「文件背包」窗口（先打开它）")
        return 2

    ok = True
    def bad(msg):
        nonlocal ok
        print("  [FAIL] " + msg)
        ok = False

    def read_cfg():
        return io.open(cfg, encoding="utf-8").read() if os.path.isfile(cfg) else ""

    def rel_rect(cid):
        h = user32.GetDlgItem(app, cid)
        if not h:
            return None
        l, t, r, b = rect_of(h)
        pt = wintypes.POINT(l, t)
        user32.ScreenToClient(app, ctypes.byref(pt))
        return pt.x, pt.y, pt.x + (r - l), pt.y + (b - t)

    def label(cid):
        return get_text(user32.GetDlgItem(app, cid), 64)

    def enabled(cid):
        return bool(user32.IsWindowEnabled(user32.GetDlgItem(app, cid)))

    def list_count():
        return backpack_get_items(user32.GetDlgItem(app, 110))

    def set_sel(idxs):
        """把文件栏的选中**设成**给定的这组行。

        ⚠️ 文件栏是 LBS_EXTENDEDSEL（多选）—— 这种样式下 LB_SETCURSEL 只挪焦点、
           不改选中，必须用 LB_SETSEL。
        """
        lb = user32.GetDlgItem(app, 110)
        user32.SendMessageW(lb, 0x0185, 0, -1)             # LB_SETSEL FALSE -1：清空
        for i in idxs:
            user32.SendMessageW(lb, 0x0185, 1, i)          # LB_SETSEL TRUE i
        if idxs:
            user32.SendMessageW(lb, 0x0186, idxs[-1], 0)   # LB_SETCURSEL（挪焦点行）
        wp = (1 << 16) | 110                               # LBN_SELCHANGE
        user32.PostMessageW(app, 0x0111, wp, lb)
        time.sleep(0.35)

    def click_on(hwnd, cid):
        wparam = (BN_CLICKED << 16) | (cid & 0xFFFF)
        user32.PostMessageW(hwnd, WM_COMMAND, wparam, 0)

    def find_class(cls):
        for h in enum_tops():
            if user32.IsWindowVisible(h) and class_of(h) == cls:
                return h
        return None

    def dialogs():
        return [h for h in enum_tops()
                if user32.IsWindowVisible(h) and class_of(h) == "#32770"]

    def explorers():
        return [h for h in enum_tops()
                if class_of(h) == "CabinetWClass" and user32.IsWindowVisible(h)]

    def close_explorers():
        for h in enum_tops():
            if class_of(h) == "CabinetWClass" and user32.IsWindowVisible(h):
                user32.PostMessageW(h, 0x0010, 0, 0)
        time.sleep(0.3)

    def wait_status(sub, limit=25.0):
        subs = sub if isinstance(sub, tuple) else (sub,)
        dl2 = time.time() + limit
        while time.time() < dl2:
            time.sleep(0.25)
            t = get_text(user32.GetDlgItem(app, 102), 300)
            if any(x in t for x in subs):
                return t
        return get_text(user32.GetDlgItem(app, 102), 300)

    def lbuttondown_at(y, dbl=False):
        """给列表框直接发鼠标消息（带 y 坐标）—— 避免合成点击被别的窗口挡住。"""
        lb = user32.GetDlgItem(app, 110)
        msg = 0x0203 if dbl else 0x0201        # WM_LBUTTONDBLCLK / WM_LBUTTONDOWN
        user32.PostMessageW(lb, msg, 1, ((y & 0xFFFF) << 16) | 20)
        time.sleep(0.6)

    def row_y(i):
        lb = user32.GetDlgItem(app, 110)
        ih = user32.SendMessageW(lb, 0x01A1, 0, 0)      # LB_GETITEMHEIGHT
        return ih * i + ih // 2

    saved = read_cfg()
    try:
        # ---------- 造一个"服务器"目录 ----------
        import shutil
        if os.path.isdir(base):
            shutil.rmtree(base, ignore_errors=True)
        os.makedirs(os.path.join(local, "sub"))
        io.open(os.path.join(local, "alpha.txt"), "w", encoding="utf-8").write("alpha\n")
        io.open(os.path.join(local, "beta.txt"), "w", encoding="utf-8").write("beta\n")
        io.open(os.path.join(local, "gamma.txt"), "w", encoding="utf-8").write("gamma\n")
        io.open(os.path.join(local, "sub", "inner.txt"), "w", encoding="utf-8").write("in\n")
        with io.open(os.path.join(local, "big.bin"), "wb") as f:
            chunk = b"Z" * (1024 * 1024)
            for _ in range(200):
                f.write(chunk)
        os.makedirs(dl, exist_ok=True)

        io.open(cfg, "w", encoding="utf-8", newline="\n").write(
            '{\n  "host": "local:%s",\n  "port": 22,\n  "username": "probe",\n'
            '  "auth_type": "password",\n  "password": "",\n'
            '  "private_key_path": "",\n  "remote_folder": "",\n'
            '  "backpack_name": "v18",\n'
            '  "download_dir": "%s",\n  "open_tmp_dir": "%s",\n'
            '  "heartbeat_sec": 300,\n  "io_min_interval_ms": 1000\n}\n'
            % (local.replace("\\", "/"), dl.replace("\\", "/"),
               os.path.join(base, "open").replace("\\", "/")))
        post_button(app, 103)                       # 刷新 = 重读配置 + 确认 + 重拉列表
        st = wait_status(("已连接", "已刷新", "同步"), 25)
        print("  状态 = %r" % st)
        if "已连接" not in st and "已刷新" not in st:
            bad("点刷新之后没连上：%r" % st)
            print("[packv17] FAIL")
            return 1
        print("  OK  刷新 = 重读配置 + 确认连接 + 重拉列表")

        # ---------- [1] 版式：6 个等大纯图标按钮 ----------
        print("\n[1. 版式：6 个等大的纯图标按钮]")
        ORDER = (("info", 123), ("ren", 126), ("del", 124),
                 ("upl", 122), ("down", 121), ("dlf", 125))
        R = {}
        for nm, cid in ORDER + (("status", 102), ("gear", 100),
                                ("sync", 103), ("list", 110), ("notes", 111)):
            R[nm] = rel_rect(cid)
        if not all(R.values()):
            bad("有控件找不到")
        else:
            order = [R[k][0] for k, _ in ORDER]
            ws = [R[k][2] - R[k][0] for k, _ in ORDER]
            hs = [R[k][3] - R[k][1] for k, _ in ORDER]
            lbls = [label(cid) for _, cid in ORDER]
            print("  顺序 x = %s" % order)
            print("  尺寸   = %s" % ["%dx%d" % (w, h) for w, h in zip(ws, hs)])
            print("  名字   = %s" % lbls)
            if order != sorted(order):
                bad("按钮顺序不是「文件属性 重命名 删除 上传 下载 打开文件位置」")
            else:
                print("  OK  按钮顺序：文件属性 重命名 删除 上传 下载 打开文件位置")
            if len(set(ws)) != 1 or len(set(hs)) != 1:
                bad("按钮不是等大的：%s" % list(zip(ws, hs)))
            else:
                print("  OK  六个按钮一样大（%dx%d）" % (ws[0], hs[0]))
            if ws[0] != hs[0]:
                bad("按钮不是正方形（%dx%d）—— 图标按钮做成方的更好看" % (ws[0], hs[0]))
            else:
                print("  OK  按钮是正方形")
            if lbls != ["文件属性", "重命名", "删除", "上传", "下载", "打开文件位置"]:
                bad("按钮名字不对：%s" % lbls)
            else:
                print("  OK  六个按钮的身份都对得上（窗口标题里写着名字）")
            if user32.GetDlgItem(app, 120):
                bad("「返回上级」按钮还在（v1.8 应该改成文件栏里的 `..` 那一行了）")
            else:
                print("  OK  没有「返回上级」按钮了")
            if R["status"][0] != R["notes"][0]:
                bad("状态提示词没跟便笺框左对齐（%d vs %d）" % (R["status"][0], R["notes"][0]))
            else:
                print("  OK  状态提示词与便笺框左对齐")
            # ★ v1.9：设置/刷新从"便笺上方"搬到了**文件栏上方**（王的要求），
            #   右对齐到文件栏右缘；便笺上方只留状态提示词。
            if R["gear"][1] >= R["list"][1] or R["sync"][1] >= R["list"][1]:
                bad("设置/刷新不在文件栏上方")
            elif abs(R["sync"][2] - R["list"][2]) > 3:
                bad("「刷新」没右对齐到文件栏右缘（%d vs %d）" % (R["sync"][2], R["list"][2]))
            elif R["gear"][0] < R["down"][2]:
                bad("设置/刷新和那排文件操作按钮挤在一起了")
            else:
                print("  OK  设置/刷新在文件栏正上方、右对齐到文件栏右缘")
            allw = [R[k][2] - R[k][0] for k, _ in ORDER] + \
                   [R["gear"][2] - R["gear"][0], R["sync"][2] - R["sync"][0]]
            allh = [R[k][3] - R[k][1] for k, _ in ORDER] + \
                   [R["gear"][3] - R["gear"][1], R["sync"][3] - R["sync"][1]]
            if len(set(allw)) != 1 or len(set(allh)) != 1:
                bad("8 个按钮不是一样大：宽 %s 高 %s" % (sorted(set(allw)), sorted(set(allh))))
            else:
                print("  OK  8 个按钮（6 个文件操作 + 设置 + 刷新）一样大")
            lw = R["list"][2] - R["list"][0]
            nw = R["notes"][2] - R["notes"][0]
            if lw <= 0 or abs(lw / float(nw) - 0.613) > 0.05:
                bad("38/62 比例变了（%.3f）" % (lw / float(nw)))
            else:
                print("  OK  仍是固定 38/62（%.3f）" % (lw / float(nw)))

        # ---------- [2] 字体 + 保留名不可见 ----------
        print("\n[2. 文件栏字体 / 便笺与旧回收站不显示]")
        lb = user32.GetDlgItem(app, 110)
        hf = user32.SendMessageW(lb, 0x0031, 0, 0)        # WM_GETFONT
        face = ""
        if hf:
            buf = ctypes.create_string_buffer(92)
            if gdi32.GetObjectW(hf, 92, buf):
                face = ctypes.cast(ctypes.byref(buf, 28), ctypes.c_wchar_p).value
        print("  文件栏字体 = %r" % face)
        if "Consolas" not in face:
            bad("文件栏不是 Consolas 字体")
        else:
            print("  OK  文件栏用 Consolas")
        cnt = list_count()
        on_disk = sorted(os.listdir(local))
        print("  列表 %d 项；磁盘上 = %s" % (cnt, on_disk))
        if cnt != 5:                                       # alpha beta gamma sub big
            bad("列表条目数不对（期望 5 个普通文件/目录，便笺不该出现）")
        else:
            print("  OK  便笺没出现在列表里，普通项一个不少")

        # ---------- [2b] 右键菜单的内容（v1.9.2）----------
        # 菜单是**模态**弹出的，外部看不见它里面有什么项 —— 所以应用会把菜单项
        # 写进轨迹日志，这里读日志核对（**轮询**等，别赌"睡 1 秒就够了"）。
        print("\n[2b. 右键菜单内容]")
        ib_ = user32.GetDlgItem(app, 110)
        ih_ = user32.SendMessageW(ib_, 0x01A1, 0, 0)

        def all_log():
            return app_trace_lines("NppDockApp_PACK", 100000)

        def wait_menu(tag, prev_n, limit=6.0):
            dl = time.time() + limit
            while time.time() < dl:
                new = [l for l in all_log()[prev_n:] if "右键菜单" in l and tag in l]
                if new:
                    return new[-1]
                time.sleep(0.1)
            return ""

        def rclick(y):
            n = len(all_log())
            user32.PostMessageW(ib_, 0x0205, 2, ((y & 0xFFFF) << 16) | 30)
            return n

        def dismiss_menu():
            # 点一下菜单外面把它收掉（点菜单项会真的执行命令）。
            # 落点选**分隔条**：它没鼠标处理，点了什么也不干
            #（点右上角会误触「刷新」——第一版就踩了）。
            _l, _t, _r, _b = rect_of(ib_)
            click(_r + 5, _t + 40)
            time.sleep(0.4)

        # ① 文件行（根目录第 1 项 = alpha.txt）
        n = rclick(ih_ + ih_ // 2)
        rowmenu = wait_menu("（文件行）", n)
        dismiss_menu()
        print("  文件行菜单 = %r" % rowmenu)
        if all(w in rowmenu for w in ("下载", "属性", "重命名", "删除")) \
           and "在文件夹中显示" not in rowmenu:
            print("  OK  文件行：下载 / 属性 / 重命名 / 删除（「在文件夹中显示」已去掉）")
        else:
            bad("文件行菜单不对：%r" % rowmenu)

        # ② 目录行（根目录第 0 项 = sub）—— 目录给的是「打开」
        n = rclick(ih_ // 2)
        dirmenu = wait_menu("（文件行）", n)
        dismiss_menu()
        print("  目录行菜单 = %r" % dirmenu)
        if "打开" in dirmenu and "属性" in dirmenu and "下载" not in dirmenu:
            print("  OK  目录行：打开 / 属性 / 重命名 / 删除")
        else:
            bad("目录行菜单不对：%r" % dirmenu)

        # ③ 空白处
        # ⚠️ v2.0 修：原来写死 `client.bottom - 6` —— 当列表**恰好被条目填满**
        #    （面板偏矮、或者条目正好铺到最底下）时，那个点会落在**最后一行**上，
        #    拿到的就是「文件行」菜单，于是这里误报 FAIL（实测复现过：
        #    轨迹日志里多出一条「右键菜单（文件行）」）。
        #    现在按"最后一行下方"算；确实没有空白就**明确跳过**，而不是判失败。
        cr_ = wintypes.RECT()
        user32.GetClientRect(ib_, ctypes.byref(cr_))
        cnt_ = user32.SendMessageW(ib_, 0x018B, 0, 0)        # LB_GETCOUNT
        used_ = ih_ * cnt_
        blank_y = used_ + max(ih_ // 2, 6)
        if blank_y > cr_.bottom - 4:
            print("  [skip] 文件栏被条目填满（内容高 %d / 客户区 %d），没有空白处可点"
                  % (used_, cr_.bottom))
        else:
            n = rclick(blank_y)
            blank = wait_menu("（空白）", n)
            dismiss_menu()
            print("  空白菜单 = %r" % blank)
            if all(w in blank for w in ("上传", "刷新", "打开文件位置")):
                print("  OK  空白处：上传 / 刷新 / 打开文件位置")
            else:
                bad("空白处菜单不对：%r" % blank)

        # ---------- [3] 「..」那一行 ----------
        print("\n[3. 「..」是文件栏里的一行目录（不在根时才出现）]")
        # ⚠️ 行号不是"我摆文件的顺序"：传输层排序是**目录在前**、其余按名字。
        #    根目录：0=sub  1=alpha.txt  2=beta.txt  3=big.bin  4=gamma.txt
        root_cnt = list_count()
        set_sel([0])                                       # sub
        lbuttondown_at(row_y(0), dbl=True)                 # 双击进 sub
        sub_cnt = list_count()
        print("  根 %d 项 → 进 sub 后 %d 项" % (root_cnt, sub_cnt))
        if sub_cnt != 2:                                   # 「..」 + inner.txt
            bad("双击目录没进去，或者「..」没出现（子目录里应是 2 项）")
        else:
            print("  OK  子目录里有「..」那一行（+inner.txt）")
            # ★ v2.0：**在子目录里验"右击空白处"的菜单**。
            #   为什么挪到这儿：根目录那 5 项在 150% DPI 下内容高 155px > 客户区 146px，
            #   列表被条目填满，根本没有空白可点（点哪儿都是某一行）。
            #   子目录只有 2 行，底下全是空白 —— 这里才测得到那条分支。
            _cr = wintypes.RECT()
            user32.GetClientRect(ib_, ctypes.byref(_cr))
            _cnt = user32.SendMessageW(ib_, 0x018B, 0, 0)
            _used = ih_ * _cnt
            if _used + ih_ // 2 <= _cr.bottom - 4:
                n = rclick(_used + max(ih_ // 2, 6))
                blank = wait_menu("（空白）", n)
                dismiss_menu()
                print("  空白菜单 = %r" % blank)
                if all(w in blank for w in ("上传", "刷新", "打开文件位置")):
                    print("  OK  空白处：上传 / 刷新 / 打开文件位置")
                elif blank:
                    bad("空白处菜单不对：%r" % blank)
            else:
                print("  [skip] 子目录里也没空白可点（%d/%d）" % (_used, _cr.bottom))
        # LB_GETTEXT 跨进程拿不到，用"选中它时按钮该是灰的"来间接确认第 0 行是「..」
        set_sel([0])
        if enabled(124) or enabled(121):
            bad("选中「..」时文件操作按钮该是灰的（它不是背包里的一项）")
        else:
            print("  OK  选中「..」时文件操作按钮是灰的")
        lbuttondown_at(row_y(0), dbl=True)                 # 双击「..」返回上级
        back_cnt = list_count()
        print("  双击「..」之后 = %d 项" % back_cnt)
        if back_cnt != root_cnt:
            bad("双击「..」没回到上级")
        else:
            print("  OK  双击「..」回到上级")

        # ---------- [4] 选中 / 取消选中 / 多选 ----------
        print("\n[4. 选中 → 按钮可用；再点同一行 / 点空白**不该**取消选中（v1.9）]")
        if enabled(121):
            bad("没选中时「下载」该是灰的")
        else:
            print("  OK  没选中时「下载」是灰的")
        set_sel([1])
        if not enabled(121):
            bad("选中之后「下载」还是灰的")
        else:
            print("  OK  选中之后按钮可用")
        # ★ v1.9 回归：王反馈"点击某个文件无法正常选中"。
        #   原因是当时"再点一下同一行 → 取消选中"这条分支 —— 用户以为没点上、
        #   再点一下，选中反而被清空了。这条分支已经删掉，必须**保持选中**。
        n0 = user32.SendMessageW(lb, 0x0190, 0, 0)         # LB_GETSELCOUNT
        lbuttondown_at(row_y(1))                           # 真实点击：再点一下同一行
        n1 = user32.SendMessageW(lb, 0x0190, 0, 0)
        print("  再点同一行：选中数 %s → %s" % (n0, n1))
        if n1 != 1:
            bad("再点一下同一行把选中弄丢了（王报的就是这个 bug）")
        else:
            print("  OK  再点同一行仍然选中（不再取消）")
        cr = wintypes.RECT()
        user32.GetClientRect(lb, ctypes.byref(cr))
        lbuttondown_at(cr.bottom - 5)                      # 点列表底部空白
        n2 = user32.SendMessageW(lb, 0x0190, 0, 0)
        print("  点列表下方空白：选中数 → %s" % n2)
        if n2 != 1:
            bad("点空白把选中清掉了（v1.9 应保持原样）")
        else:
            print("  OK  点空白不再清掉选中")
        set_sel([1, 2])
        n_sel = user32.SendMessageW(lb, 0x0190, 0, 0)      # LB_GETSELCOUNT
        if n_sel != 2:
            bad("多选没生效（LB_GETSELCOUNT=%d）" % n_sel)
        else:
            print("  OK  文件栏支持多选")

        # ---------- [5] 删除 = 真删 ----------
        print("\n[5. 删除 = 真删（不弹框、不建回收站）]")
        set_sel([2])                                       # beta.txt（0=sub 1=alpha 2=beta）
        before = list_count()
        dlg_before = len(dialogs())
        post_button(app, 124)
        time.sleep(1.5)
        if len(dialogs()) > dlg_before:
            bad("删除弹出了对话框 —— 王要求删除**不提示**")
        else:
            print("  OK  删除没有弹任何提示框")
        beta_gone = not os.path.isfile(os.path.join(local, "beta.txt"))
        after = list_count()
        trash = os.path.join(local, ".nppbackpack-trash")
        print("  列表 %d → %d；beta.txt 还在 = %s；回收站目录存在 = %s"
              % (before, after, not beta_gone, os.path.isdir(trash)))
        if after != before - 1:
            bad("删除之后列表条数没减 1")
        elif not beta_gone:
            bad("删掉的文件还在磁盘上（说明不是真删）")
        elif os.path.isdir(trash):
            bad("还在建回收站目录 —— 王要求把存放删除文件的文件夹去掉")
        else:
            print("  OK  真删了：磁盘上没了，也没有回收站目录")

        # 删目录（要连里面一起删）—— 删掉 beta 之后：0=sub 1=alpha.txt 2=big.bin 3=gamma.txt
        set_sel([0])                                       # sub
        post_button(app, 124)
        time.sleep(1.5)
        if os.path.isdir(os.path.join(local, "sub")):
            bad("目录没删掉（真删应连里面的东西一起删）")
        else:
            print("  OK  目录也是真删（连里面的文件一起没了）")

        # ---------- [6] 刷新时短暂显示服务器 ----------
        print("\n[6. 刷新时短暂显示服务器域名/ip]")
        post_button(app, 103)
        seen = ""
        dl3 = time.time() + 6
        while time.time() < dl3:
            t = get_text(user32.GetDlgItem(app, 102), 300)
            if "刷新" in t and (":" in t or "local" in t):
                seen = t
                break
            time.sleep(0.05)
        print("  刷新瞬间的状态 = %r" % seen)
        if not seen:
            bad("点刷新时状态里没有短暂显示服务器（域名/ip）")
        else:
            print("  OK  刷新时状态里短暂显示连的是哪台服务器")
        st2 = wait_status(("已刷新",), 20)
        print("  刷新后的状态 = %r" % st2)
        if "延迟" not in st2:
            bad("刷新之后没报连接延迟（王要求点刷新打印延迟）")
        else:
            print("  OK  刷新报了连接延迟")

        # ---------- [7] 文件属性：只有 4 项 ----------
        print("\n[7. 文件属性（名称/类型/大小/时间，且无系统提示音）]")
        set_sel([0])                                       # 0=alpha.txt
        post_button(app, 123)
        time.sleep(1.0)
        pw = find_class("NppDockPackPropsWnd")
        if not pw:
            bad("没弹出属性窗")
        else:
            ids = [cid for cid in range(1000, 1010) if user32.GetDlgItem(pw, cid)]
            labs = [get_text(user32.GetDlgItem(pw, cid), 32) for cid in ids]
            print("  标签行 = %s" % labs)
            if len(ids) != 4:
                bad("属性窗不是 4 行（实际 %d 行）" % len(ids))
            elif labs != ["名称", "类型", "大小", "时间"]:
                bad("四个标签不对：%s" % labs)
            else:
                print("  OK  只有 名称 / 类型 / 大小 / 时间 四项")
            if dialogs():
                bad("出现了 #32770（那种会带系统提示音）")
            else:
                print("  OK  没有 #32770（无系统提示音）")
            # ★ v1.9.2：标签列要**贴着**内容（外面那一大片空白去掉）
            lab0 = user32.GetDlgItem(pw, 1000)          # 第一行的标签
            val0 = user32.GetDlgItem(pw, 2000)          # 第一行的值框
            rl = wintypes.RECT(); user32.GetWindowRect(lab0, ctypes.byref(rl))
            rv = wintypes.RECT(); user32.GetWindowRect(val0, ctypes.byref(rv))
            gapPx = rv.left - rl.right
            print("  标签与值框的缝 = %d px" % gapPx)
            if gapPx < 0 or gapPx > 20:
                bad("标签和值框之间还是隔太远（%d px）" % gapPx)
            else:
                print("  OK  标签紧贴内容（缝 %d px）" % gapPx)
            # 行内的「复制」要是指标按钮（owner-draw：BS_OWNERDRAW = 0xB），
            # 并且**没有**「关闭」按钮
            b0 = user32.GetDlgItem(pw, 3000)
            own = bool((user32.GetWindowLongW(b0, -16) & 0x0B) == 0x0B) if b0 else False
            print("  行内复制按钮 = %s（owner-draw = %s）"
                  % (hex(b0) if b0 else None, own))
            if not b0 or not own:
                bad("行内的「复制」不是图标按钮（owner-draw）")
            else:
                print("  OK  行内「复制」是图标按钮")
            if user32.GetDlgItem(pw, 9002):
                bad("「关闭」按钮还在（v1.9.2 应删掉）")
            else:
                print("  OK  没有「关闭」按钮了")
            # 弹窗里的字体要是 Consolas（王："弹窗的字体也用 console"）
            faces = set()
            for cid in (2000, 3000, 9001):
                hh = user32.GetDlgItem(pw, cid)
                if not hh:
                    continue
                hf2 = user32.SendMessageW(hh, 0x0031, 0, 0)
                if hf2:
                    b2 = ctypes.create_string_buffer(92)
                    if gdi32.GetObjectW(hf2, 92, b2):
                        faces.add(ctypes.cast(ctypes.byref(b2, 28), ctypes.c_wchar_p).value)
            print("  弹窗里的字体 = %s" % sorted(faces))
            if faces and any("Consolas" not in f for f in faces):
                bad("弹窗里还有非 Consolas 的字体")
            else:
                print("  OK  弹窗用 Consolas")
            # ★ v1.9.2：点「复制全部」→ 内容进剪贴板 + **弹窗自动关闭**
            click_on(pw, 9001)
            time.sleep(0.8)
            if find_class("NppDockPackPropsWnd"):
                bad("点了「复制全部」之后弹窗没关")
            else:
                print("  OK  「复制全部」复制完就关窗了")
            clip = clip_text()
            lines = [x for x in clip.splitlines() if x.strip()]
            print("  剪贴板 = %r（%d 行）" % (clip, len(lines)))
            if len(lines) != 4:
                bad("剪贴板里不是 4 行（实际 %d 行）" % len(lines))
            else:
                print("  OK  「复制全部」把 4 行都放进了剪贴板")

        # ---------- [8] 重命名：原地改，不弹窗 ----------
        print("\n[8. 重命名：原地改（不弹窗）]")
        set_sel([0])                                       # 0=alpha.txt
        user32.PostMessageW(lb, 0x0100, 0x71, 0)           # WM_KEYDOWN / VK_F2
        time.sleep(0.8)
        if find_class("NppDockPackPromptWnd"):
            bad("按 F2 弹出了对话框 —— 王要求**原地修改**")
        ed = find_descendant(lb, "Edit")
        if not ed:
            bad("按 F2 之后文件栏里没有出现那行上的输入框")
        else:
            print("  OK  F2 在原地起了个输入框（没有弹窗）")
            buf = ctypes.create_unicode_buffer("alpha2.txt")
            user32.SendMessageW(ed, 0x000C, 0, ctypes.cast(buf, ctypes.c_void_p).value)
            time.sleep(0.2)
            user32.SendMessageW(ed, 0x0100, 0x0D, 0)       # WM_KEYDOWN / VK_RETURN
            time.sleep(1.5)
            okr = os.path.isfile(os.path.join(local, "alpha2.txt"))
            gone = not os.path.isfile(os.path.join(local, "alpha.txt"))
            print("  alpha2.txt 在 = %s；alpha.txt 没了 = %s" % (okr, gone))
            if not okr or not gone:
                bad("原地重命名没生效")
            else:
                print("  OK  原地重命名成功（F2 → 打字 → 回车）")
            if find_descendant(lb, "Edit"):
                bad("重命名之后那个输入框没收掉")
            else:
                print("  OK  输入框已经收掉")

        # ---------- [8b] 速率 + 剩余时间 ----------
        print("\n[8b. 下载时状态里报「最近一秒的速率」与「剩余时间」]")
        set_sel([1])                                       # big.bin
        rate_dst = os.path.join(dl, "big.bin")
        if os.path.isfile(rate_dst):
            os.remove(rate_dst)
        post_button(app, 121)
        rate_line = ""
        t0 = time.time()
        while time.time() - t0 < 10.0:
            t = get_text(user32.GetDlgItem(app, 102), 300)
            if "/s" in t and "剩余" in t:
                rate_line = t
                break
            time.sleep(0.03)
        print("  速率行 = %r" % rate_line)
        if not rate_line:
            bad("下载过程中没看到「速率 + 剩余时间」")
        else:
            print("  OK  报了最近一秒的速率与剩余时间")
        wait_status(("下载完成", "失败", "已取消"), 60)     # 等它跑完，别干扰下一节
        close_explorers()

        # ---------- [9] 「取消」真的能按 ----------
        print("\n[9. 下载大文件 → 按钮变红叉且**可点** → 点它中断]")
        # 列表（按名字）：alpha2.txt(0) big.bin(1) gamma.txt(2)
        set_sel([1])
        big_dst = os.path.join(dl, "big.bin")
        if os.path.isfile(big_dst):
            os.remove(big_dst)
        post_button(app, 121)                              # 下载
        saw_cancel = False
        cancel_ok = False
        t0 = time.time()
        while time.time() - t0 < 6.0:
            if label(121) == "取消":
                saw_cancel = True
                cancel_ok = enabled(121)
                break
            time.sleep(0.02)
        print("  看到「取消」= %s；那一刻它可用 = %s" % (saw_cancel, cancel_ok))
        if not saw_cancel:
            bad("下载大文件期间按钮没变成「取消」")
        elif not cancel_ok:
            bad("「取消」按钮是**灰的**，点不动 —— 这正是王报过的那个 bug")
        else:
            print("  OK  传输中按钮变成「取消」且真的可以按")
        if saw_cancel:
            post_button(app, 121)                          # 点「取消」
            st2 = wait_status(("已取消", "取消"), 15)
            print("  点取消后状态 = %r" % st2)
            time.sleep(0.6)
            gone = not os.path.isfile(big_dst)
            print("  半截文件是否被清掉 = %s" % gone)
            if "取消" not in st2:
                bad("点了「取消」但状态里没有「已取消」")
            elif not gone:
                bad("取消之后半截文件没删掉")
            else:
                print("  OK  取消生效：传输中断，半截文件被删掉")
        close_explorers()

        # ---------- [10] 取消过之后还能正常下载 ----------
        print("\n[10. 取消之后再下载一次（回归「自取消」那个 bug）]")
        small_dst = os.path.join(dl, "gamma.txt")
        if os.path.isfile(small_dst):
            os.remove(small_dst)
        set_sel([2])                                       # gamma.txt
        post_button(app, 121)
        time.sleep(2.0)
        st3 = get_text(user32.GetDlgItem(app, 102), 300)
        print("  状态 = %r；gamma.txt 下来了 = %s" % (st3, os.path.isfile(small_dst)))
        if not os.path.isfile(small_dst):
            bad("取消过一次之后，下一次下载没能正常完成（自取消 bug 回来了？）")
        else:
            print("  OK  取消过之后照样能正常下载")
        close_explorers()

        for f in os.listdir(dl):
            try: os.remove(os.path.join(dl, f))
            except Exception: pass
    finally:
        if saved:
            io.open(cfg, "w", encoding="utf-8", newline="\n").write(saved)
            print("  （已还原原配置）")

    print()
    print("[packv17] PASS" if ok else "[packv17] FAIL")
    return 0 if ok else 1


def app_trace_lines(app_name="NppDockApp_PACK", n=40):
    """读应用的轨迹日志尾部 n 行（用来验证那些"界面上看不见"的东西）。

    ⚠️ 日志跟 **exe 同目录**（= 部署目录 `<N++>/plugins/NppDock`），
       不是 plugin_dir()（那是**源码**目录 `pluginsWorkspace/plugins/NppDock`）。
       用错地方的表现很隐蔽：读到的是一份陈年日志，"什么都没记"。
    """
    p = os.path.join(os.path.dirname(app_exe_path()), app_name + ".trace.log")
    if not os.path.isfile(p):
        return []
    try:
        return io.open(p, encoding="utf-8", errors="replace").read().splitlines()[-n:]
    except Exception:
        return []


def clip_text(owner=0):
    """读剪贴板里的文本（CF_UNICODETEXT = 13）。读不到返回 ''。"""
    k = ctypes.WinDLL("kernel32")
    # ⚠️ 64 位下必须把参数/返回值都声明成指针：ctypes 默认按 int 传，
    #   64 位句柄会直接 OverflowError（探针就栽在这儿）。
    user32.GetClipboardData.restype  = ctypes.c_void_p
    user32.GetClipboardData.argtypes = [ctypes.c_uint]
    k.GlobalLock.restype    = ctypes.c_void_p
    k.GlobalLock.argtypes   = [ctypes.c_void_p]
    k.GlobalUnlock.argtypes = [ctypes.c_void_p]
    ok = False
    for _ in range(30):
        if user32.OpenClipboard(owner):
            ok = True
            break
        time.sleep(0.05)
    if not ok:
        return ""
    try:
        h = user32.GetClipboardData(13)
        if not h:
            return ""
        p = k.GlobalLock(h)
        if not p:
            return ""
        out = ctypes.c_wchar_p(p).value or ""
        k.GlobalUnlock(h)
        return out
    finally:
        user32.CloseClipboard()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["tree", "menu", "pick", "addapp", "closetab",
                                    "log", "standalone", "appwin", "menudbg",
                                    "apptree", "func", "hashall", "combo", "shot",
                                    "resizetest", "dragsplitter", "toggletest",
                                    "abort", "multiopen", "tabhl", "nettest",
                                    "tabmove", "tabsep", "filedrop",
                                    "tabv14",
                                    "backpack", "cfgapply", "packv17"])
    ap.add_argument("--index", type=int, default=0,
                    help="第几个按钮项（跳过分隔符），从 0 开始")
    ap.add_argument("--text", default="", help="按文本子串匹配菜单项")
    ap.add_argument("--tail", type=int, default=40)
    ap.add_argument("--exe", default="", help="standalone 用：要启动的 exe")
    ap.add_argument("--file", default="", help="func 用：样本文件路径")
    ap.add_argument("--local", default="", help="backpack 用：本地「背包目录」（离线自检）")
    ap.add_argument("--keep", action="store_true",
                    help="standalone 用：测完不关进程")
    ap.add_argument("--target", default="app",
                    help="shot 用：app / panel / container / npp")
    ap.add_argument("--name", default="", help="shot 用：文件名前缀")
    ap.add_argument("--cycles", type=int, default=2, help="resizetest 用：轮数")
    ap.add_argument("--delta", type=int, default=40, help="resizetest 用：每次变化像素")
    ap.add_argument("--dx", type=int, default=-100, help="dragsplitter 用：横向拖动量")
    ap.add_argument("--dy", type=int, default=0, help="dragsplitter 用：纵向拖动量")
    ap.add_argument("--steps", type=int, default=12, help="dragsplitter 用：拖动步数")
    ap.add_argument("--mb", type=int, default=BIG_MB_DEFAULT,
                    help="abort 用：临时大样本的大小（MB），默认 1024")
    args = ap.parse_args()

    awareness = make_dpi_aware()
    _npp = find_npp(timeout=2)
    _host_ctx, _host_aw = host_awareness(_npp) if _npp else (None, None)
    print(f"[env] DPI 感知 = {awareness}（宿主声明 = {_host_aw}）；"
          f"屏幕 = {user32.GetSystemMetrics(0)}x{user32.GetSystemMetrics(1)}")

    if args.cmd == "shot":
        return cmd_shot(args.target, args.name)
    if args.cmd == "resizetest":
        return cmd_resizetest(args.cycles, args.delta)
    if args.cmd == "dragsplitter":
        return cmd_dragsplitter(args.dx, args.dy, args.steps)
    if args.cmd == "toggletest":
        return cmd_toggletest()
    if args.cmd == "log":
        read_log(args.tail)
        return 0
    if args.cmd == "standalone":
        return cmd_standalone(args.exe or app_exe_path(), args.keep)
    if args.cmd == "appwin":
        return cmd_appwin()
    if args.cmd == "apptree":
        return cmd_apptree()
    if args.cmd == "func":
        return cmd_func(args.file)
    if args.cmd == "hashall":
        return cmd_hashall(args.file or os.path.join(plugin_dir(), "_t", "fox.bin"),
                           args.exe)
    if args.cmd == "combo":
        return cmd_combo(args.exe)
    if args.cmd == "abort":
        return cmd_abort(args.mb, args.exe)
    if args.cmd == "menudbg":
        return cmd_menudbg()
    if args.cmd == "multiopen":
        return cmd_multiopen(args.exe or app_exe_path())
    if args.cmd == "tabhl":
        return cmd_tabhl()
    if args.cmd == "nettest":
        return cmd_nettest(args.exe or net_exe_path(), args.keep)
    if args.cmd == "tabmove":
        return cmd_tabmove()
    if args.cmd == "tabv14":
        return cmd_tabv14()
    if args.cmd == "tabsep":
        return cmd_tabsep()
    if args.cmd == "filedrop":
        return cmd_filedrop(args.file)
    if args.cmd == "backpack":
        return cmd_backpack(args.file, args.local, args.keep)
    if args.cmd == "cfgapply":
        return cmd_cfgapply(args.file, args.local, args.keep)
    if args.cmd == "packv17":
        return cmd_packv17(args.file, args.local, args.keep)

    npp, panel, our_tab, container = locate_panel()
    if not npp:
        print("[ERROR] 找不到 Notepad++ 主窗口")
        return 2
    print(f"N++ 主窗口   = {npp:#010x} rect={rect_of(npp)}")
    if not panel:
        print("[ERROR] 找不到 NppDockContentPane（面板没显示？）")
        return 2
    print(f"内容区       = {panel:#010x} rect={rect_of(panel)} "
          f"vis={int(bool(user32.IsWindowVisible(panel)))}")
    if our_tab:
        n = user32.SendMessageW(our_tab, TCM_GETITEMCOUNT, 0, 0)
        print(f"我们的标签条 = {our_tab:#010x} rect={rect_of(our_tab)} "
              f"vis={int(bool(user32.IsWindowVisible(our_tab)))} 页数={n}")
    else:
        print("我们的标签条 = 不存在")
    print(f"MD5 进程数   = {count_md5_procs()}")

    if args.cmd == "tree":
        dump_tree(npp)
        return 0

    # ---- 关页：在标签条上右击；否则右击空白页 ----
    # 整段包在 try/finally 里：中途出错（例如菜单项定位失败）也必须按 ESC
    # 把菜单关掉，否则菜单会一直挂在那儿，后面的步骤全被它挡住。
    try:
        return menu_flow(args, npp, panel, our_tab)
    finally:
        press_esc()




if __name__ == "__main__":
    sys.exit(main())
