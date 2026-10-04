#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""单元测试壳的构建+运行器（v2.0 新增）。

为什么有这东西：
  v2.0 的几处改动（文件名解析、原子写、一次 ssh 的帧分割、输出缓冲）
  **没法从界面驱动** —— 要么需要一台真服务器、要么要造 40 万字符的输出。
  而它们恰恰是"出错就极难反推原因"的那类逻辑。
  做法是让每个壳把被测的 .cpp **整份 include 进来**，于是那些 static
  函数/变量在同一个编译单元里直接可见，可以喂合成样本。
  这条路子实测抓出过三个真 bug（见 docs/铁律与踩坑要点.md「探针与验证手段」），所以留着。

用法：
    python tools/unit/run.py             # 全跑
    python tools/unit/run.py test_session  # 只跑一个

依赖：借用 build.py 的编译环境（它处理了沙箱里 reg.exe 被拦、
vcvars 拿不到 Windows SDK 路径的兜底）。
"""
import os
import subprocess
import sys
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))          # plugins/NppDock

_spec = importlib.util.spec_from_file_location("nppdock_build",
                                               os.path.join(PROJ, "build.py"))
B = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(B)

LIBS = ["user32.lib", "gdi32.lib", "shell32.lib", "advapi32.lib", "comdlg32.lib",
        "iphlpapi.lib", "ws2_32.lib", "dnsapi.lib", "comctl32.lib"]

APP = os.path.join(PROJ, "src", "apps")

# 壳 -> 还需要一起编译的源文件（被测 cpp 由壳自己 include）
TESTS = {
    "test_parsels":  [],
    "test_packcore": [],
    "test_session":  [],
    "test_outbuf":   [os.path.join(APP, "nettest", "netcfg.cpp"),
                      os.path.join(APP, "nettest", "netcore.cpp"),
                      os.path.join(PROJ, "src", "common", "jsonlite.cpp")],
}


def build_one(env, name, extra):
    src = os.path.join(HERE, name + ".cpp")
    obj = os.path.join(HERE, name + ".obj")
    out = os.path.join(HERE, name + ".exe")
    B.compile_one(env, src, obj, include_dirs=[os.path.join(PROJ, "sdk")])
    objs = [obj]
    for i, e in enumerate(extra):
        o = os.path.join(HERE, "%s_%d.obj" % (name, i))
        B.compile_one(env, e, o, include_dirs=[os.path.join(PROJ, "sdk"),
                                              os.path.join(PROJ, "src", "common")])
        objs.append(o)
    link = B.resolve_tool("link.exe", env)
    # ⚠️ 必须 /SUBSYSTEM:CONSOLE：build.py 的 link_exe 是给 GUI 用的（WINDOWS），
    #    用它会让链接器去找 WinMain。
    B.run_tool([link, "/nologo", "/OUT:" + out, "/SUBSYSTEM:CONSOLE",
                "/MACHINE:X64"] + objs + LIBS, env)
    return out


def main():
    names = sys.argv[1:] or list(TESTS.keys())
    _vs, vcvars = B.find_vs()
    env = B.vcvars_env(vcvars)

    failed = []
    for name in names:
        print("=" * 68)
        print("[%s]" % name)
        exe = build_one(env, name, TESTS[name])
        rc = subprocess.call([exe])
        if rc != 0:
            failed.append(name)

    print("=" * 68)
    if failed:
        print("失败：%s" % ", ".join(failed))
        return 1
    print("全部通过（%d 个壳）" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
