#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 Notepad++ 工具栏精简配置（便携版）。
用法: python gen_toolbar_conf.py
原理: 基于官方 toolbarButtonsConf_example.xml，把指定 id 的按钮设为 hide="yes"，
      并自动隐藏因按钮隐藏而变得多余的分隔符（避免出现孤立/连续竖线）。
输出: <Notepad++ 根目录>/toolbarButtonsConf.xml
"""
import re
import sys
from pathlib import Path

# ---- 要隐藏的按钮（按 command id）----
HIDE_IDS = {
    # --- 第一批（文件/编辑基础操作）---
    41001,  # New（新建）
    41002,  # Open...（打开）
    41006,  # Save（保存）
    41003,  # Close（关闭）
    42001,  # Cut（剪切）
    42002,  # Copy（复制）
    42005,  # Paste（粘贴）
    42003,  # Undo（撤销）
    43001,  # Find...（查找）
    43003,  # Replace...（替换）
    44023,  # Zoom In（放大）
    44024,  # Zoom Out（缩小）
    # --- 第二批（批量操作 / 打印 / 重做 / 同步滚动）---
    41007,  # Save All（全部保存）
    41004,  # Close All（全部关闭）
    41010,  # Print...（打印）
    42004,  # Redo（重做 / 恢复）
    44035,  # Synchronize Vertical Scrolling（垂直同步滚动）
    44036,  # Synchronize Horizontal Scrolling（水平同步滚动）
}

NPP_ROOT = Path(r"C:\Users\14029\PortableAPPs\Notepad++")
EXAMPLE = NPP_ROOT / "toolbarButtonsConf_example.xml"
OUTPUT = NPP_ROOT / "toolbarButtonsConf.xml"

# ⚠️ 不要用「固定空格数」的正则去匹配属性：官方示例里分隔符行的
#    id 与 name 之间有 5 个空格用于对齐，`id="0" name=` 这种写法匹配不到。
#    统一按属性名解析，跟空白无关。
BTN_TAG_RE = re.compile(r"<Button\s+([^>]*?)/>")
ATTR_RE = re.compile(r'(\w+)="([^"]*)"')


def parse_attrs(attr_str):
    return dict(ATTR_RE.findall(attr_str))


def parse_standard(src):
    """解析 <Standard> 段的所有条目（含分隔符）。"""
    m = re.search(r"<Standard[^>]*>(.*?)</Standard>", src, re.S)
    if not m:
        raise SystemExit("找不到 <Standard> 段")
    items = []
    for bm in BTN_TAG_RE.finditer(m.group(1)):
        d = parse_attrs(bm.group(1))
        if "id" not in d or "index" not in d:
            continue
        items.append({
            "id": int(d["id"]),
            "name": d.get("name", ""),
            "index": int(d["index"]),
        })
    return items


def compute_hidden(items, hide_ids):
    """决定哪些条目隐藏：先按 id，再清理多余分隔符。"""
    hidden = {it["index"] for it in items if it["id"] in hide_ids}

    # 分隔符（id=0）清理规则：
    #   1. 前面必须存在可见按钮，否则这个分隔符没有意义；
    #   2. 与**上一个已保留的分隔符**之间必须存在可见按钮，
    #      否则会出现连续竖线（例如 [sep][sep]）。
    seps = [it["index"] for it in items if it["id"] == 0]
    visible_order = [it["index"] for it in items
                     if it["id"] != 0 and it["index"] not in hidden]

    kept_seps = []
    for sep in seps:
        has_visible_before = any(v < sep for v in visible_order)
        if not has_visible_before:
            hidden.add(sep)
            continue
        if kept_seps and not any(kept_seps[-1] < v < sep for v in visible_order):
            hidden.add(sep)          # 与上一个分隔符之间没有可见按钮 -> 多余
            continue
        kept_seps.append(sep)
    return hidden


def main():
    if not EXAMPLE.exists():
        raise SystemExit(f"找不到官方示例文件：{EXAMPLE}")
    src = EXAMPLE.read_text(encoding="utf-8")

    items = parse_standard(src)
    if not any(it["id"] in HIDE_IDS for it in items):
        print("警告：示例文件里没有匹配到任何目标 id，可能版本不匹配")

    hidden = compute_hidden(items, HIDE_IDS)

    def repl(m):
        attrs = m.group(1)
        d = parse_attrs(attrs)
        if "id" not in d or "index" not in d:
            return m.group(0)          # Plugin 段条目、注释里的示例，原样保留
        idx = int(d["index"])
        hide = "yes" if idx in hidden else "no"
        new_attrs = re.sub(r'hide="[^"]*"', f'hide="{hide}"', attrs, count=1)
        return f"<Button {new_attrs}/>"

    out = BTN_TAG_RE.sub(repl, src)
    OUTPUT.write_text(out, encoding="utf-8")

    hidden_btns = [it for it in items if it["index"] in hidden and it["id"] != 0]
    hidden_seps = [it for it in items if it["index"] in hidden and it["id"] == 0]
    kept = [it["name"] for it in items if it["index"] not in hidden and it["id"] != 0]

    print(f"已写入：{OUTPUT}")
    print(f"\n隐藏按钮 {len(hidden_btns)} 个：")
    for it in hidden_btns:
        print(f"  index={it['index']:2d}  id={it['id']}  {it['name']}")
    print(f"\n顺带隐藏多余分隔符 {len(hidden_seps)} 个：{[it['index'] for it in hidden_seps]}")
    print(f"\n保留按钮 {len(kept)} 个：")
    for n in kept:
        print(f"  {n}")


if __name__ == "__main__":
    main()
