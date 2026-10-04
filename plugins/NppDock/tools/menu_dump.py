# 遍历 Notepad++ 主菜单，建立 命令ID -> 菜单文本 映射
# 用法: python menu_dump.py
# 原理: GetMenu/GetMenuItemInfoW/GetMenuStringW 由 win32k 提供跨进程支持，
#       无需远端内存分配。
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32

# ---- 64 位句柄/指针类型声明（关键）----
user32.GetMenu.restype = wintypes.HMENU
user32.GetMenu.argtypes = [wintypes.HWND]
user32.GetMenuItemCount.restype = ctypes.c_int
user32.GetMenuItemCount.argtypes = [wintypes.HMENU]
user32.GetMenuItemInfoW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.BOOL, ctypes.c_void_p]
user32.GetSubMenu.argtypes = [wintypes.HMENU, ctypes.c_int]
user32.GetSubMenu.restype = wintypes.HMENU

MF_BYPOSITION = 0x400
MF_POPUP = 0x10
MFT_SEPARATOR = 0x800

class MENUITEMINFOW(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT),
                ("fMask", wintypes.UINT),
                ("fType", wintypes.UINT),
                ("fState", wintypes.UINT),
                ("wID", wintypes.UINT),
                ("hSubMenu", wintypes.HMENU),
                ("hbmpChecked", wintypes.HBITMAP),
                ("hbmpUnchecked", wintypes.HBITMAP),
                ("dwItemData", ctypes.c_size_t),
                ("dwTypeData", wintypes.LPWSTR),
                ("cch", wintypes.UINT),
                ("hbmpItem", wintypes.HBITMAP)]

MIIM_ID = 0x2
MIIM_SUBMENU = 0x4
MIIM_STRING = 0x40
MIIM_FTYPE = 0x100

def get_text(hmenu, pos):
    mii = MENUITEMINFOW()
    mii.cbSize = ctypes.sizeof(MENUITEMINFOW)
    mii.fMask = MIIM_STRING | MIIM_FTYPE
    buf = ctypes.create_unicode_buffer(256)
    mii.dwTypeData = buf
    mii.cch = 256
    user32.GetMenuItemInfoW(hmenu, pos, True, ctypes.byref(mii))
    return buf.value, mii.fType

def get_id(hmenu, pos):
    mii = MENUITEMINFOW()
    mii.cbSize = ctypes.sizeof(MENUITEMINFOW)
    mii.fMask = MIIM_ID
    user32.GetMenuItemInfoW(hmenu, pos, True, ctypes.byref(mii))
    return mii.wID

def get_submenu(hmenu, pos):
    mii = MENUITEMINFOW()
    mii.cbSize = ctypes.sizeof(MENUITEMINFOW)
    mii.fMask = MIIM_SUBMENU
    user32.GetMenuItemInfoW(hmenu, pos, True, ctypes.byref(mii))
    return mii.hSubMenu

def walk(hmenu, depth=0, prefix=""):
    n = user32.GetMenuItemCount(hmenu)
    for i in range(n):
        text, ftype = get_text(hmenu, i)
        if ftype & MFT_SEPARATOR:
            continue
        sub = get_submenu(hmenu, i)
        label = text.replace("&", "").split("\t")[0]
        if sub:
            print(f"{'  '*depth}[{label}]")
            walk(sub, depth + 1)
        else:
            cid = get_id(hmenu, i)
            print(f"{'  '*depth}{cid:6d}  {label}")

def main():
    holder = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def top_cb(hwnd, lp):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == "Notepad++":
            holder.append(hwnd)
        return True

    user32.EnumWindows(top_cb, 0)
    if not holder:
        print("NPP_NOT_FOUND")
        return
    hwnd = holder[0]
    title = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, title, 256)
    print(f"N++ hwnd={hwnd:#x} title={title.value!r} visible={user32.IsWindowVisible(hwnd)}")
    menu = user32.GetMenu(hwnd)
    if not menu:
        print("NO_MENU (GetMenu returned NULL)")
        return
    print(f"menu={menu:#x}")
    walk(menu)

if __name__ == "__main__":
    main()
