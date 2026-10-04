# 枚举 Notepad++ 工具栏按钮（idCommand + tooltip 文本）
# 用法: python check_toolbar.py
# 原理: N++ 主窗口 -> ToolbarWindow32；Python 与 N++ 不同进程，
#       TBBUTTON/字符串需 VirtualAllocEx + 跨进程读写。
# ⚠️ 必须 set argtypes/restype，否则 64 位地址被截断成垃圾指针会让 N++ 崩溃。
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# ---- 修正 64 位指针/句柄类型（关键！）----
LRESULT = ctypes.c_ssize_t
user32.SendMessageW.restype = LRESULT
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.VirtualAllocEx.restype = wintypes.LPVOID
kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.WriteProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.ReadProcessMemory.restype = wintypes.BOOL
kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

TB_BUTTONCOUNT    = 0x0418
TB_GETBUTTON      = 0x0417
TB_GETBUTTONTEXTW = 0x044B
TBSTYLE_SEP       = 0x04

class TBBUTTON(ctypes.Structure):
    _fields_ = [("iBitmap", ctypes.c_int),
                ("idCommand", ctypes.c_int),
                ("fsState", ctypes.c_ubyte),
                ("fsStyle", ctypes.c_ubyte),
                ("bReserved", ctypes.c_ubyte * 6),
                ("dwData", ctypes.c_size_t),
                ("iString", ctypes.c_ssize_t)]  # 32 bytes on x64

def read_remote(hproc, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    ok = kernel32.ReadProcessMemory(hproc, wintypes.LPCVOID(addr), buf, size, ctypes.byref(got))
    return buf if ok else None

def write_remote(hproc, addr, data):
    got = ctypes.c_size_t(0)
    return kernel32.WriteProcessMemory(hproc, wintypes.LPVOID(addr), data, len(data), ctypes.byref(got))

def get_buttons(hwnd, hproc):
    count = user32.SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0)
    if count <= 0 or count > 200:
        return None
    remote = kernel32.VirtualAllocEx(hproc, None, 1024, 0x3000, 0x04)
    if not remote:
        return None
    try:
        result = []
        for i in range(count):
            write_remote(hproc, remote, bytes(32))
            if not user32.SendMessageW(hwnd, TB_GETBUTTON, i, remote):
                result.append((i, -1, "<TB_GETBUTTON failed>"))
                continue
            buf = read_remote(hproc, remote, 32)
            if not buf:
                result.append((i, -1, "<read failed>"))
                continue
            tb = TBBUTTON.from_buffer_copy(buf.raw[:32])
            cmd = tb.idCommand
            if tb.fsStyle & TBSTYLE_SEP:
                hidden = bool(tb.fsState & 0x08)
                result.append((i, 0, ("[隐藏] " if hidden else "") + "---SEP---"))
                continue
            hidden = bool(tb.fsState & 0x08)   # TBSTATE_HIDDEN
            write_remote(hproc, remote, bytes(1024))
            n = user32.SendMessageW(hwnd, TB_GETBUTTONTEXTW, cmd, remote)
            text = ""
            if n > 0:
                sbuf = read_remote(hproc, remote, min((n + 1) * 2, 1024))
                if sbuf:
                    text = sbuf.raw[:n * 2].decode("utf-16-le", errors="replace")
            result.append((i, cmd, ("[隐藏] " if hidden else "") + text))
        return result
    finally:
        kernel32.VirtualFreeEx(hproc, remote, 0, 0x8000)

def parent_class(hwnd):
    p = user32.GetParent(hwnd)
    if not p:
        return "(no parent)"
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(p, buf, 256)
    return buf.value

def main():
    toolbars = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def tb_cb(hwnd, lp):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == "ToolbarWindow32":
            toolbars.append(hwnd)
        return True

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def top_cb(hwnd, lp):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == "Notepad++":
            user32.EnumChildWindows(hwnd, tb_cb, 0)
        return True

    user32.EnumWindows(top_cb, 0)
    if not toolbars:
        print("TOOLBAR_NOT_FOUND")
        return

    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(toolbars[0], ctypes.byref(pid))
    hproc = kernel32.OpenProcess(0x0008 | 0x0010 | 0x0020, False, pid)
    if not hproc:
        print("OPEN_PROCESS_FAILED pid=%d" % pid.value)
        return
    try:
        print(f"pid={pid.value}, found {len(toolbars)} toolbar(s)")
        for hwnd in toolbars:
            count = user32.SendMessageW(hwnd, TB_BUTTONCOUNT, 0, 0)
            print(f"\nTOOLBAR hwnd={hwnd:#x} parent={parent_class(hwnd)} count={count}")
            buttons = get_buttons(hwnd, hproc)
            if buttons is None:
                print("  <enumeration failed>")
                continue
            for i, cmd, text in buttons:
                print(f"  [{i:2d}] cmd={cmd:6d}  {text if text else '(无文本)'}")
    finally:
        kernel32.CloseHandle(hproc)

if __name__ == "__main__":
    main()
