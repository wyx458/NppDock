# -*- coding: utf-8 -*-
"""生成 NppDock 的工具栏图标（简约线条风 · 黑白灰）。

产出三个文件（都在 src/main/ 下，随 DLL 一起编进资源）：
    NppDock.ico        浅色工具栏用（深灰线条）
    NppDock_dark.ico   深色工具栏用（浅灰线条）
    NppDock.bmp        老式工具栏图标集用的位图（16x16，与浅色版一致）

为什么要三个：Notepad++ 的 NPPM_ADDTOOLBARICON_FORDARKMODE 要求
hToolbarBmp / hToolbarIcon / hToolbarIconDarkMode **三个都给**
（用户切换图标集或切深色模式时，缺哪个就在哪个场合显示空白）。

图形语义（一眼看懂 + 小尺寸也清楚）：
    一个圆角矩形（= 窗口） + 底部一条实心横杠（= 停靠在底部的那块面板）。
只用线条和两块灰阶，没有渐变、没有彩色 —— 这样在深浅两套工具栏上都不会脏。

用法：
    python tools\\gen_dock_icon.py            # 生成到 src/main/
    python tools\\gen_dock_icon.py --out DIR  # 生成到别处

⚠️ 依赖 PIL。本机用带 PIL 的解释器：
    ~/.workbuddy/binaries/python/envs/default/Scripts/python.exe
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw

# ---- 配色：只有黑白灰 ----
LIGHT_LINE = (0x44, 0x44, 0x44)   # 浅色工具栏上的线条（深灰）
LIGHT_FILL = (0x7A, 0x7A, 0x7A)   # 浅色工具栏上的实心带（中灰）
DARK_LINE  = (0xDC, 0xDC, 0xDC)   # 深色工具栏上的线条（浅灰）
DARK_FILL  = (0xA8, 0xA8, 0xA8)   # 深色工具栏上的实心带

ICO_SIZES = [16, 20, 24, 32, 48, 64]


def draw_glyph(size, line_color, fill_color):
    """画一个"窗口 + 底部面板"的线条图标（透明背景）。

    边距按尺寸缩放（大图多留一点，小图尽量把线画粗一档好认）：
      · 16px：外框 1px、底带 3px 高
      · 大图：外框按 size/16 变粗，底带同步变高
    """
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    scale = max(1.0, size / 16.0)
    stroke = max(1, int(round(scale)))          # 线条粗细
    margin = max(1, int(round(size * 0.09)))    # 外边距
    r = max(2, int(round(size * 0.16)))         # 圆角半径

    x0, y0 = margin, margin
    x1, y1 = size - 1 - margin, size - 1 - margin

    # 外框（圆角矩形，1 档线宽的描边）
    d.rounded_rectangle([x0, y0, x1, y1], radius=r,
                        outline=line_color, width=stroke)

    # 底部实心横杠 = "停靠在底部的面板"
    band_h = max(3, int(round(size * 0.20)))
    by0 = y1 - band_h - margin // 2
    by1 = y1 - margin // 2
    if by1 > by0:
        d.rounded_rectangle([x0 + stroke, by0, x1 - stroke, by1],
                            radius=max(1, r // 2), fill=fill_color)

    return img


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(os.path.join(here, "..", "src", "main"))
    ap.add_argument("--out", default=default_out, help="输出目录（默认 src/main）")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # ---- 浅色 ICO ----
    light = [draw_glyph(s, LIGHT_LINE, LIGHT_FILL) for s in ICO_SIZES]
    p_light = os.path.join(args.out, "NppDock.ico")
    # ⚠️ ICO 必须**最大那张当 base**，其余走 append_images ——
    #    拿 16px 当 base 时 PIL 只会写出那一张（实测 sizes 里就剩 (16,16)），
    #    工具栏在大图标模式下会糊成一片。
    light[-1].save(p_light, format="ICO",
                   sizes=[(s, s) for s in ICO_SIZES],
                   append_images=light[:-1])

    # ---- 深色 ICO ----
    dark = [draw_glyph(s, DARK_LINE, DARK_FILL) for s in ICO_SIZES]
    p_dark = os.path.join(args.out, "NppDock_dark.ico")
    dark[-1].save(p_dark, format="ICO",
                  sizes=[(s, s) for s in ICO_SIZES],
                  append_images=dark[:-1])

    # ---- BMP（16x16，给老式图标集用；BMP 不支持透明，用工具栏底色）----
    bmp = Image.new("RGB", (16, 16), (0xF0, 0xF0, 0xF0))
    glyph16 = draw_glyph(16, LIGHT_LINE, LIGHT_FILL)
    bmp.paste(glyph16, (0, 0), glyph16)
    p_bmp = os.path.join(args.out, "NppDock.bmp")
    bmp.save(p_bmp, format="BMP")

    for p in (p_light, p_dark, p_bmp):
        print(f"[icon] {p}  ({os.path.getsize(p)} 字节)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ImportError:
        print("[ERROR] 缺 PIL，请用带 PIL 的解释器跑这个脚本")
        sys.exit(2)
