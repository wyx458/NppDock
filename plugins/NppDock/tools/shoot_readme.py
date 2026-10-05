#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成 README 里那张图：`docs/images/overview.png`（整个 Notepad++ + 底部面板）。

为什么用一个脚本，而不是人肉截一张丢进 docs/：

1. **不泄露私人内容**：它**自己另起一个 Notepad++ 实例**（`-multiInst`），
   只打开仓库里的源码；绝不碰你正在编辑的窗口 —— 否则桌面上随手开着的
   txt / 便笺就会被一起截进公开仓库。跑完还把 `config.xml`、`panel.ini`
   原样还原，连窗口几何都不动你的。
2. **可复现**：界面改完重跑一次，图就不会过期，不用回忆"当时面板拉多高"。
3. **可校验**：截图最容易出的问题（窗口被别的窗口压住、面板是空的、抓歪了）
   肉眼扫一眼根本看不出来。脚本对成图做像素级体检 —— 标题条 / 编辑区 / 面板条
   各自的墨迹、**活动标签的绿杠**在不在、整图纯黑占比。不达标报 FAIL 并以非 0 退出。

用法（在 `plugins/NppDock/` 下）::

    python tools/shoot_readme.py                  # 直接覆盖 docs/images/overview.png
    python tools/shoot_readme.py --panel-px 620   # 面板再高一点
    python tools/shoot_readme.py --out /tmp/x     # 先写到别处看看，不动仓库里的图

前置：正在用的 Notepad++ 最好先关掉 —— 脚本另起的实例会抢一次前台。
"""

import argparse
import ctypes
import os
import re
import subprocess
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_DIR = os.path.dirname(HERE)                          # plugins/NppDock
REPO_ROOT = os.path.dirname(os.path.dirname(PLUGIN_DIR))    # 仓库根 pluginsWorkspace
NPP_ROOT = os.path.dirname(REPO_ROOT)                       # Notepad++ 安装根
NPP_EXE = os.path.join(NPP_ROOT, "notepad++.exe")
PANEL_INI = os.path.join(NPP_ROOT, "plugins", "Config", "NppDock", "panel.ini")
CFG_XML = os.path.join(NPP_ROOT, "config.xml")

sys.path.insert(0, HERE)
import dock_app_probe as P                                   # noqa: E402

u = P.user32
u.SetProcessDPIAware()

CONTAINER_CLASS = "NppDockContainerWnd"
NPPM_DMMSHOW = 0x0400 + 30
SW_RESTORE = 9

# 截图时面板里停哪三个应用（顺序固定 → 出来的图确定）
PANEL_PAGES = ["NppDockApp_MD5.exe", "NppDockApp_NET.exe", "NppDockApp_PACK.exe"]

DEFAULT_FILE = os.path.join(PLUGIN_DIR, "src", "apps", "backpack", "packcore.cpp")
DEFAULT_OUT = os.path.join(REPO_ROOT, "docs", "images", "overview.png")

fails = []


def note(ok, msg):
    print(("  [OK]   " if ok else "  [FAIL] ") + msg)
    if not ok:
        fails.append(msg)


def warn(msg):
    print("  [warn] " + msg)


# ---------------------------------------------------------------------------
def pid_of(hwnd):
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def launch_instance(src_file):
    if not os.path.isfile(NPP_EXE):
        print("[ERROR] 找不到 Notepad++：%s" % NPP_EXE)
        sys.exit(2)
    print("另起一个实例（只打开仓库里的源码，不碰你正在编辑的窗口）：")
    print("  %s -multiInst %s" % (NPP_EXE, src_file))
    proc = subprocess.Popen([NPP_EXE, "-multiInst", src_file], cwd=NPP_ROOT)
    deadline = time.time() + 25
    while time.time() < deadline:
        for h in P.enum_tops():
            if pid_of(h) == proc.pid and P.class_of(h) == "Notepad++":
                return proc, h
        time.sleep(0.4)
    print("[ERROR] 新实例的窗口没等出来")
    sys.exit(2)


def wait_container(npp, timeout=25.0):
    """等容器面板出现（它是 NPPN_READY 之后才注册的，比主窗口晚）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = P.find_descendant(npp, CONTAINER_CLASS)
        if c:
            return c
        time.sleep(0.3)
    return None


# ---------------------------------------------------------------------------
def patch_config(panel_px):
    """临时改 config.xml：面板拉高 + 标题栏只显示文件名。

    * `bottomHeight` 量的是整个停靠区（含管理器加的约 30px 标题栏），所以要 +32。
      比"拖分隔条"可靠 —— 那条路要正好压在 4px 高的分隔条上，实测会空拖且不报错。
    * 标题栏默认显示**全路径**，会把 `C:\\Users\\<用户名>\\...` 一起截进图里，
      而这张图要进公开仓库 —— 切成 short。
    """
    if not os.path.isfile(CFG_XML):
        warn("没有 config.xml，面板高度/标题栏用默认值")
        return
    with open(CFG_XML, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    want = panel_px + 32
    text, n1 = re.subn(r'(<GUIConfig name="DockingManager"[^>]*?bottomHeight=")\d+(")',
                       lambda m: m.group(1) + str(want) + m.group(2), text)
    text, n2 = re.subn(r'(<GUIConfig name="titleBar" short=")no(")',
                       lambda m: m.group(1) + "yes" + m.group(2), text)
    with open(CFG_XML, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("临时配置：停靠区高度 %dpx（面板目标 %dpx）%s%s"
          % (want, panel_px,
             "" if n1 else "，[warn] 没找到 bottomHeight",
             "，标题栏只显示文件名" if n2 else ""))


def prepare_panel_ini():
    """把 panel.ini 写成"三个不同应用" —— 面板恢复哪些页由它决定，写死才确定。

    格式就是容器自己读的那份（`SavePanelState` 写的）：`visible=` + `pageN=` + `current=`。
    """
    os.makedirs(os.path.dirname(PANEL_INI), exist_ok=True)
    lines = ["visible=1"] + \
            ["page%d=%s" % (i, exe) for i, exe in enumerate(PANEL_PAGES)] + \
            ["current=0"]
    with open(PANEL_INI, "w", encoding="utf-8", newline="") as f:
        f.write("\r\n".join(lines) + "\r\n")
    print("临时写入 panel.ini：%s" % " / ".join(PANEL_PAGES))


def set_window_rect(hwnd, x, y, w, h):
    if u.IsZoomed(hwnd):                      # 最大化了先还原，否则改不了尺寸
        u.ShowWindow(hwnd, SW_RESTORE)
        time.sleep(0.5)
    u.SetWindowPos(hwnd, None, x, y, w, h, 0x0004 | 0x0010)   # NOZORDER|NOACTIVATE
    time.sleep(0.9)


# ---------------------------------------------------------------------------
def shoot(npp, path):
    """抓整窗（标题栏 / 菜单 / 工具栏 / 底部面板）。

    走**抓屏**而不是 PrintWindow：PrintWindow 对没实现 WM_PRINTCLIENT 的自绘控件
    会返回空白（应用界面正是自绘的），而这张图要的就是"用户眼睛看到的样子"。
    代价是要抢前台 —— 所以抓之前先校验归属（防抓到压在旁边的别的窗口）。
    """
    P.bring_to_front(npp)
    r = P.rect_of(npp)
    w, h = r[2] - r[0], r[3] - r[1]
    ok, why = P.screen_region_is_ours(npp, npp)
    if not ok:
        note(False, "抓屏前窗口被别的窗口压住（%s）" % why)
        return None
    px, w, h = P.capture_screen(r[0], r[1], w, h)
    P.save_png(px, w, h, path)
    return path


def band_ink(img, box):
    data = img.crop(box).tobytes()
    n = len(data) // 3
    ink = acc = 0
    for i in range(0, n * 3, 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if r + g + b < 600:
            ink += 1
        if (abs(r - P.ACCENT_RGB[0]) <= P.ACCENT_TOL
                and abs(g - P.ACCENT_RGB[1]) <= P.ACCENT_TOL
                and abs(b - P.ACCENT_RGB[2]) <= P.ACCENT_TOL):
            acc += 1
    return ink / max(n, 1), acc


def verify(path, npp, container):
    """证明这张图里真的有"Notepad++ 的壳 + 打开着的源码 + 我们的面板"。

    分条带看（整图平均会被大面积底色冲淡，看不出问题）：
      · 标题条（最上面 40px）—— 有墨迹 = 抓到的是窗口，不是一块桌面；
      · 编辑区 —— 有墨迹 = 源码文件真的打开了（不是空白新文档）；
      · 面板条（容器的矩形）—— 有墨迹，**且活动标签的绿杠在**
        （那一笔是容器在 WM_PRINTCLIENT 里补画的，它在 = 标签条真画了）；
      · 整图纯黑占比很小 —— 抓歪了最典型的表现就是大片纯黑。
    """
    from PIL import Image
    img = Image.open(path).convert("RGB")
    nr, cr = P.rect_of(npp), P.rect_of(container)
    w_ = nr[2] - nr[0]
    t_ink, _ = band_ink(img, (0, 0, w_, 40))
    e_ink, _ = band_ink(img, (0, 110, w_, max(cr[1] - nr[1] - 12, 111)))
    p_ink, p_acc = band_ink(img, (cr[0] - nr[0], cr[1] - nr[1],
                                  cr[2] - nr[0], cr[3] - nr[1]))
    data = img.tobytes()
    black = sum(1 for i in range(0, len(data), 3 * 29)
                if data[i] < 12 and data[i + 1] < 12 and data[i + 2] < 12)
    black_r = black / max(len(data) // (3 * 29), 1)

    kb = os.path.getsize(path) // 1024
    print("  成图：%dx%d，%dKB" % (img.width, img.height, kb))
    print("  条带：标题 %.1f%%｜编辑区 %.1f%%｜面板 %.1f%%（绿杠 %d px）｜纯黑 %.1f%%"
          % (t_ink * 100, e_ink * 100, p_ink * 100, p_acc, black_r * 100))
    note(t_ink > 0.01 and e_ink > 0.004 and p_ink > 0.02
         and p_acc > 20 and black_r < 0.05, "像素体检通过")


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--panel-px", type=int, default=520, help="面板高度（像素）")
    ap.add_argument("--width", type=int, default=1720)
    ap.add_argument("--height", type=int, default=1240)
    ap.add_argument("--at", default="90,50", help="窗口左上角屏幕坐标")
    ap.add_argument("--file", default=DEFAULT_FILE, help="编辑器里打开的源码")
    ap.add_argument("--keep", action="store_true", help="跑完留着那个 N++ 实例")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    # 备份用户配置 → 跑完原样还原
    backups = {}
    for p in (CFG_XML, PANEL_INI):
        if os.path.isfile(p):
            with open(p, "rb") as f:
                backups[p] = f.read()

    patch_config(args.panel_px)
    prepare_panel_ini()

    proc, npp = launch_instance(args.file)
    try:
        container = wait_container(npp)
        if not container:
            print("[ERROR] 面板（%s）没出现" % CONTAINER_CLASS)
            return 2
        if not u.IsWindowVisible(container):
            print("面板是隐藏的，先显示出来")
            u.SendMessageW(npp, NPPM_DMMSHOW, 0, container)
            time.sleep(0.8)

        ax, ay = (int(v) for v in args.at.split(","))
        set_window_rect(npp, ax, ay, args.width, args.height)
        container = wait_container(npp, 5) or container
        print("面板高度：%dpx" % (P.rect_of(container)[3] - P.rect_of(container)[1]))

        if shoot(npp, args.out):
            verify(args.out, npp, container)
    finally:
        for p, data in backups.items():
            with open(p, "wb") as f:
                f.write(data)
        if not args.keep:
            proc.terminate()
            print("已关掉本次另起的实例（你原来的窗口没动）")

    print("=" * 58)
    if fails:
        print("有 %d 项不达标：" % len(fails))
        for f in fails:
            print("  · " + f)
        return 1
    print("好了：" + args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
