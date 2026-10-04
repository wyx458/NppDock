# 检查 NppDockContentPane 面板的实际屏幕可见性
# 用法: python check_pane.py
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32
found = []

@ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def child_cb(hwnd, lparam):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    if buf.value == "NppDockContentPane":
        found.append((hwnd, user32.IsWindowVisible(hwnd)))
    return True

@ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def top_cb(hwnd, lparam):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    if buf.value == "Notepad++":
        user32.EnumChildWindows(hwnd, child_cb, 0)
    return True

user32.EnumWindows(top_cb, 0)

if not found:
    print("PANE_NOT_FOUND")
else:
    for hwnd, vis in found:
        print(f"PANE hwnd={hwnd:#x} visible={vis}")
