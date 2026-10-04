#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
文件校验工具正确性回归（MD5 / SHA-1 / SHA-256 / SHA-384 / SHA-512 / CRC32）
================================================================================
为什么需要它：
  摘要算法是"算错也不会报错"的典型 —— 出 bug 时照样吐一串十六进制，
  肉眼完全看不出来（32 位和 128 位看起来都一样"像那么回事"）。
  所以必须拿可信实现做对照。

参考实现用 Python 标准库：
  hashlib（也就是 OpenSSL）提供 md5/sha1/sha256/sha384/sha512
  zlib.crc32 提供 CRC32（IEEE 802.3，与我们的实现同一套多项式）
另有 --certutil 开关再拿 Windows 自带的 certutil 交叉验证一次，
避免"两边都错得一样"这种极端情况（certutil 不支持 CRC32，会跳过）。

用例刻意覆盖各算法的**分块与填充边界**：
  0 / 1 / 55 / 56 / 63 / 64 / 65 / 120 字节 —— 卡 64 字节块的补位门槛（56）
  111 / 112 / 113 字节                     —— 卡 128 字节块的补位门槛（112，SHA-512 系列）
  外加一个 3MB+7 的伪随机大文件（+7 是为了让它不落在 64KB 读块边界上）

  这些边界正是实现最容易写错的地方（补位长度、长度字段的位置/字节序），
  专门测它们比随便测一堆文件有用得多。

用法：
    python tools/check_hash.py                    # 全部算法，对照 hashlib/zlib
    python tools/check_hash.py --algo sha256      # 只测一种
    python tools/check_hash.py --certutil         # 再用 certutil 交叉验证
    python tools/check_hash.py --exe <路径>        # 指定被测 exe
"""

import argparse
import hashlib
import os
import random
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "NppDock", "NppDockApp_MD5.exe")
WORK = os.path.join(ROOT, "_t")


def _crc32_hex(data):
    """zlib.crc32 返回的是无符号 32 位整数，按 8 位小写十六进制输出。"""
    return "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)


# key = --algo 的值（与 hashcore::AlgoFromName 接受的写法一致）
# 每一项：(显示名, 十六进制长度, 计算函数)
ALGOS = [
    ("md5",    "MD5",     32,  hashlib.md5),
    ("sha1",   "SHA-1",   40,  hashlib.sha1),
    ("sha256", "SHA-256", 64,  hashlib.sha256),
    ("sha384", "SHA-384", 96,  hashlib.sha384),
    ("sha512", "SHA-512", 128, hashlib.sha512),
    ("crc32",  "CRC32",   8,   _crc32_hex),
]

# certutil 支持的算法（不支持 CRC32，也就没有"第三方"可对照）
CERTUTIL_NAME = {
    "md5": "MD5", "sha1": "SHA1", "sha256": "SHA256",
    "sha384": "SHA384", "sha512": "SHA512",
}

CASES = [
    ("empty.bin",   b""),
    ("one_a.bin",   b"a"),
    ("abc.bin",     b"abc"),
    ("fox.bin",     b"The quick brown fox jumps over the lazy dog"),
    ("b55.bin",     b"a" * 55),
    ("b56.bin",     b"a" * 56),
    ("b63.bin",     b"a" * 63),
    ("b64.bin",     b"a" * 64),
    ("b65.bin",     b"a" * 65),
    ("b111.bin",    b"a" * 111),
    ("b112.bin",    b"a" * 112),
    ("b113.bin",    b"a" * 113),
    ("b120.bin",    b"a" * 120),
    ("rand3m.bin",  None),          # 由脚本按种子生成
]


def build_cases():
    random.seed(42)
    out = []
    for name, data in CASES:
        if data is None:
            data = bytes(random.getrandbits(8) for _ in range(3 * 1024 * 1024 + 7))
        out.append((name, data))
    return out


def extract_hex(text, want_len):
    """从 selftest 输出里挑出那串十六进制（长度按算法不同而不同）。"""
    for tok in text.split():
        if len(tok) == want_len and all(c in "0123456789abcdef" for c in tok):
            return tok
    return ""


def certutil_hash(path, algo_key):
    """拿 Windows 自带 certutil 算一遍，作为第三方交叉验证。

    ⚠️ certutil 的输出是**系统 ANSI 代码页（本机 GBK）**，不是 UTF-8。
       直接用 text=True 让 Python 按 UTF-8 解码会在中文 Windows 上抛
       UnicodeDecodeError。所以这里收原始字节再宽松解码 ——
       反正我们只要那一行 ASCII 的十六进制，非 ASCII 部分坏掉无所谓。
    """
    name = CERTUTIL_NAME.get(algo_key)
    if not name:
        return ""
    try:
        r = subprocess.run(["certutil", "-hashfile", path, name],
                           capture_output=True, timeout=180)
    except Exception:
        return ""
    if not r.stdout:
        return ""
    text = r.stdout.decode("utf-8", errors="replace")
    want = dict((k, n) for k, _, n, _ in ALGOS)[algo_key]
    for line in text.splitlines():
        s = line.strip().replace(" ", "")
        if len(s) == want and all(c in "0123456789abcdefABCDEF" for c in s):
            return s.lower()
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--algo", default="", help="只测这一种（md5/sha1/sha256/…）")
    ap.add_argument("--certutil", action="store_true",
                    help="再用 Windows certutil 交叉验证一次（不支持 CRC32）")
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print(f"[ERROR] 找不到被测程序：{args.exe}")
        return 1
    print(f"被测程序 : {args.exe}")

    algos = ALGOS
    if args.algo:
        key = args.algo.lower().replace("-", "")
        algos = [a for a in ALGOS if a[0] == key]
        if not algos:
            print(f"[ERROR] 不认识的算法：{args.algo}")
            return 1
    print(f"算法     : {', '.join(a[1] for a in algos)}")
    print()

    os.makedirs(WORK, exist_ok=True)
    cases = build_cases()

    # ---- 表头 ----
    head = f"  {'用例':<13}{'长度':>10}   " + "".join(f"{a[1]:<10}" for a in algos)
    print(head)
    print("  " + "-" * (len(head) - 2))

    failed = 0
    total = 0
    details = []          # (用例, 算法, 得到, 期望)
    big_expected = {}     # rand3m.bin 的期望值，留给 certutil 交叉验证用

    for name, data in cases:
        path = os.path.join(WORK, name)
        with open(path, "wb") as f:
            f.write(data)

        # 期望值先全部算好
        expected = {}
        for key, disp, hexlen, fn in algos:
            v = fn(data)
            expected[key] = v.hexdigest() if hasattr(v, "hexdigest") else v
        if name == "rand3m.bin":
            big_expected = dict(expected)

        cells = []
        for key, disp, hexlen, _ in algos:
            r = subprocess.run([args.exe, "--selftest", path, "--algo", key],
                               capture_output=True, text=True, timeout=600)
            got = extract_hex(r.stdout or "", hexlen)
            total += 1
            if got == expected[key]:
                cells.append("OK")
            else:
                failed += 1
                cells.append("FAIL")
                details.append((name, disp, got, expected[key]))

        print(f"  {name:<13}{len(data):>10}   " + "".join(f"{c:<10}" for c in cells))

    # ---- certutil 交叉验证（大文件，每种算法各一次）----
    cross_notes = []
    if args.certutil:
        big = os.path.join(WORK, "rand3m.bin")
        for key, disp, hexlen, _ in algos:
            if key not in CERTUTIL_NAME:
                cross_notes.append(f"  {disp:<8}: certutil 不支持，跳过")
                continue
            got = certutil_hash(big, key)
            agree = (got != "" and got == big_expected.get(key, ""))
            cross_notes.append(f"  {disp:<8}: {got or '<解析失败>'} -> "
                               f"{'一致' if agree else '不一致'}")
            if not agree:
                failed += 1

    print()
    if cross_notes:
        print(f"  certutil 交叉验证（rand3m.bin）：")
        for line in cross_notes:
            print(line)
        print()

    if details:
        print("  失败明细：")
        for name, disp, got, exp in details:
            print(f"    {name} / {disp}")
            print(f"      得到 {got or '<无输出>'}")
            print(f"      期望 {exp}")
        print()

    print(f"  通过 {total - failed}  失败 {failed}")
    print("  结果: " + ("ALL PASS" if failed == 0 else "FAILED"))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
