#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NppDock 构建脚本
================================================================================
为什么用 Python 而不是 .bat（任务书第五章要求）：
  工程路径可能含空格（本工程实际就位于 "C:\\Users\\14029\\PortableAPPs\\Notepad++\\..."），
  .bat 里拼接命令行时引号转义极易被破坏（尤其是 set VAR=a b c 与 %VAR% 展开的行为）。
  Python 用**参数列表**调用 cl.exe / link.exe，完全绕开 shell 解析，稳定得多。

产出布局（均相对本目录）：
  build/NppDock/
    ├─ NppDock.dll        ← Notepad++ 唯一加载的插件（容器 + 加载器 + 状态持久化）
    └─ NppDockApp_*.exe   ← 可嵌入应用（与 DLL 同目录，靠前缀被 dock 发现）

用法（在本目录下执行）：
    python build.py                # 构建（主 DLL + 测试）
    python build.py --check-deps   # 额外用 dumpbin 做静态自检（/MT 校验）
    python build.py --clean        # 清理
    python build.py --tests        # 只构建测试可执行文件
================================================================================
"""

import argparse
import os
import shutil
import subprocess
import sys
import glob

# ---------------------------------------------------------------------------
# 路径（全部用绝对路径，避免 cwd 影响）
# ---------------------------------------------------------------------------
# 本文件住在 <工作区>/plugins/NppDock/ 下。工作区根**不写死 ".." 层数** ——
# 以后多套一层（比如 plugins/官方/NppDock/）就会算错，所以改成"向上找 sdk/"。
ROOT = os.path.dirname(os.path.abspath(__file__))          # plugins/NppDock
PLUGIN_DIR  = ROOT
BUILD_DIR   = os.path.join(ROOT, "build")
OUT_DIR     = os.path.join(BUILD_DIR, "NppDock")
OBJ_DIR     = os.path.join(BUILD_DIR, "obj")
TEST_DIR    = os.path.join(BUILD_DIR, "tests")
SRC_DIR     = os.path.join(ROOT, "src")


def _find_workspace(start):
    """工作区根 = 向上第一个含 sdk/PluginInterface.h 的目录。

    为什么要找而不是数 ".."：
      SDK 头是**跨插件共用**的，将来可能有 plugins/别的插件/ 与它平级，
      硬编码层数一旦目录再动一次就静默指向不存在的目录（编译报"找不到
      PluginInterface.h"，很容易误判成 SDK 丢了）。
    """
    d = start
    for _ in range(8):
        if os.path.isfile(os.path.join(d, "sdk", "PluginInterface.h")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return os.path.dirname(os.path.dirname(start))          # 兜底：plugins/NppDock -> 工作区


WORKSPACE   = _find_workspace(ROOT)
SDK_DIR     = os.path.join(WORKSPACE, "sdk")

# Notepad++ 便携版的安装位置（仅用于真机拷贝，可选）

# 注意：一律用**正斜杠**写 Windows 路径。Windows 的 Win32 API 与 cmd.exe 都接受
# 正斜杠；而反斜杠在 Git Bash / MSYS2 环境下会遭遇路径改写（见 README 踩坑记录
# “坑 1：MSYS 路径改写”），把 "C:\\Program Files" 变成 "C://Program Files"。
NPP_PLUGINS = "C:/Users/14029/PortableAPPs/Notepad++/plugins"


def log(msg):
    print(msg, flush=True)


def die(msg, code=1):
    print("\n[ERROR] " + msg, file=sys.stderr, flush=True)
    sys.exit(code)


# ---------------------------------------------------------------------------
# 定位 Visual Studio 工具链
# ---------------------------------------------------------------------------
def norm(p):
    """统一把路径里的反斜杠换成正斜杠，规避 MSYS/Git-Bash 的路径改写。
    这样即使环境变量或 vswhere 输出里带反斜杠，也不会有问题。"""
    return p.replace("\\", "/")


def find_vs():
    """用 vswhere 找 VS 安装路径，再定位 vcvars64.bat"""
    vswhere_candidates = [
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe",
        "C:/Program Files/Microsoft Visual Studio/Installer/vswhere.exe",
    ]
    vswhere = next((p for p in vswhere_candidates if os.path.isfile(p)), None)
    if not vswhere:
        # 退路：PATH 里找
        vswhere = shutil.which("vswhere")
    if not vswhere:
        die("找不到 vswhere.exe，请确认已安装 Visual Studio 2022 或 Build Tools。")

    out = subprocess.run(
        [vswhere, "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True)
    vs_path = out.stdout.strip().splitlines()
    if not vs_path or not vs_path[0]:
        die("vswhere 没找到带 C++ 工具集的 VS 安装。")
    vs_root = norm(vs_path[0].strip())

    vcvars = vs_root.rstrip("/") + "/VC/Auxiliary/Build/vcvars64.bat"
    if not os.path.isfile(vcvars):
        die(f"找不到 vcvars64.bat：{vcvars}")
    return vs_root, vcvars


def find_dumpbin(vs_root):
    pat = os.path.join(norm(vs_root), "VC", "Tools", "MSVC", "*", "bin",
                       "Hostx64", "x64", "dumpbin.exe")
    hits = sorted(glob.glob(pat))
    return norm(hits[-1]) if hits else None


def find_rc():
    """定位资源编译器 rc.exe（属于 Windows SDK，不在 VS 目录下）。

    为什么需要它：
      NppDock 的「应用」以 VERSIONINFO 的 FileDescription 作为 tab 标题，
      编进 exe 的版本资源是唯一规范来源。没有 rc.exe 时降级为"文件名派生标题"，
      功能不受影响，只是标题会变成文件名。
    """
    roots = [
        "C:/Program Files (x86)/Windows Kits/10",
        "C:/Program Files/Windows Kits/10",
    ]
    best = None
    best_ver = ()
    for root in roots:
        for hit in glob.glob(root + "/bin/*/x64/rc.exe"):
            ver = hit.replace("\\", "/").split("/bin/")[1].split("/")[0]
            key = tuple(int(x) if x.isdigit() else 0 for x in ver.split("."))
            if key > best_ver:
                best_ver, best = key, hit
    return norm(best) if best else None


# ---------------------------------------------------------------------------
# 在 vcvars64 环境下执行命令
# ---------------------------------------------------------------------------
# 关键点：cl.exe 需要一大堆环境变量（INCLUDE/LIB/PATH）。
# 我们不手工拼这些变量（极易出错），而是**调用 vcvars64.bat 一次**，
# 让它把环境吐出来（set），解析进 Python 的 env，后续直接用默认环境调用 cl。
_cached_env = None


def _make_env_for_cmd():
    """构造一个干净的、给 cmd.exe 用的环境。

    坑：如果 Python 是从 Git Bash / MSYS2 里启动的，会继承 MSYSTEM / MSYS 等变量，
    以及被改写过的一堆路径变量。这些会让 cmd.exe 里的路径解析出乱子，
    也会让 vcvars64.bat 行为异常。这里把它们剔掉（只在调用 cmd 时用）。
    """
    env = dict(os.environ)
    for k in list(env.keys()):
        if k.upper().startswith("MSYS") or k.upper().startswith("MINGW") \
           or k.upper().startswith("MSYSTEM"):
            env.pop(k, None)
    return env


def short_path(p):
    """取 Windows 8.3 短路径。

    为什么必须这么做（本项目实际踩到的坑，见 README 踩坑记录“坑 2”）：
      `cmd /c "call "C:\\Program Files (x86)\\...\\vcvars64.bat""` 这种
      “路径含空格 + 再套引号”的写法会被 cmd.exe 的引号剥离规则吃掉，
      报 “'C:\\Program' 不是内部或外部命令”。
      /s /c 与加外层引号等多种写法在本机都失败。
      最稳的办法是拿**短路径**（C:/PROGRA~2/...），路径里就没有空格了，
      cmd 怎么解析都不会出错。
    """
    if os.name != "nt":
        return norm(p)
    p = os.path.abspath(p).replace("/", "\\")
    try:
        import ctypes
        from ctypes import wintypes
        GetShortPathNameW = ctypes.windll.kernel32.GetShortPathNameW
        GetShortPathNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR,
                                      wintypes.DWORD]
        GetShortPathNameW.restype = wintypes.DWORD
        buf = ctypes.create_unicode_buffer(1024)
        n = GetShortPathNameW(p, buf, 1024)
        if n and buf.value:
            return buf.value.replace("\\", "/")
    except Exception:
        pass
    return norm(p)


def find_windows_sdk():
    """定位 Windows 10/11 SDK，返回 (include_dirs, lib_dirs)。

    为什么需要自己找（本项目实际踩到的坑，见 README“坑 3”）：
      vcvars64.bat 内部靠 reg.exe 查注册表来定位 Windows SDK。在某些受限环境
      （例如本机的沙箱会拦截 reg.exe）下，注册表查询失败，于是
        WindowsSdkDir / UCRTVersion / WindowsSDKVersion
      都是空的，INCLUDE 里就没有 ucrt（缺 crtdbg.h）、LIB 里也没有
      um/ucrt 目录，编译直接报 “无法打开包括文件: crtdbg.h”。
      这里在“默认位置”兜底 —— 不依赖注册表，纯文件系统探测。
    """
    roots = [
        "C:/Program Files (x86)/Windows Kits/10",
        "C:/Program Files/Windows Kits/10",
    ]
    for root in roots:
        inc_root = root + "/Include"
        lib_root = root + "/Lib"
        if not os.path.isdir(inc_root) or not os.path.isdir(lib_root):
            continue
        # 挑版本号最大的一个
        versions = sorted(os.listdir(inc_root),
                          key=lambda s: [int(x) if x.isdigit() else 0
                                         for x in s.split(".")],
                          reverse=True)
        for ver in versions:
            inc = f"{inc_root}/{ver}"
            lib = f"{lib_root}/{ver}"
            if not os.path.isdir(f"{inc}/ucrt"):
                continue
            includes = [
                f"{inc}/ucrt",
                f"{inc}/shared",
                f"{inc}/um",
                f"{inc}/winrt",
                f"{inc}/cppwinrt",
            ]
            libs = [
                f"{lib}/ucrt/x64",
                f"{lib}/um/x64",
            ]
            includes = [p for p in includes if os.path.isdir(p)]
            libs = [p for p in libs if os.path.isdir(p)]
            if includes and libs:
                return includes, libs
    return [], []


def vcvars_env(vcvars):
    global _cached_env
    if _cached_env is not None:
        return _cached_env

    # 用短路径绕开空格问题，然后 call 它并 set 出完整环境。
    vc = short_path(vcvars)
    cmd = "call " + vc + " 1>nul 2>nul && set"
    out = subprocess.run(["cmd", "/c", cmd], capture_output=True, text=True,
                         errors="replace", env=_make_env_for_cmd())

    if not out.stdout or "INCLUDE" not in out.stdout:
        log("  [debug] vcvars 短路径=%r" % vc)
        log("  [debug] stdout=%r" % (out.stdout or "")[:300])
        log("  [debug] stderr=%r" % (out.stderr or "")[:300])
        die("执行 vcvars64.bat 失败，未能取得编译环境。")

    env = {}
    for line in out.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            k = k.strip()
            # cmd 里有一些以 = 开头的伪变量（如 =C:），跳过
            if not k or k.startswith("="):
                continue
            env[k] = v
    if "INCLUDE" not in env or "LIB" not in env:
        die("vcvars64.bat 没有正确设置 INCLUDE/LIB，环境解析失败。")

    # ---- 兜底：如果 vcvars 没能给出 SDK 路径（reg.exe 被拦时会这样），自己补 ----
    def has_dir(var, needle):
        for p in env.get(var, "").split(";"):
            if needle in p.lower().replace("\\", "/"):
                return True
        return False

    need_sdk_inc = not has_dir("INCLUDE", "/ucrt") or not has_dir("INCLUDE", "/um")
    need_sdk_lib = not has_dir("LIB", "/ucrt") or not has_dir("LIB", "/um")
    if need_sdk_inc or need_sdk_lib:
        sdk_inc, sdk_lib = find_windows_sdk()
        if sdk_inc or sdk_lib:
            log("  [info] vcvars 未提供完整 Windows SDK 路径（可能是 reg.exe 不可用），"
                "已自动补齐：")
            if need_sdk_inc and sdk_inc:
                env["INCLUDE"] = ";".join(sdk_inc) + ";" + env["INCLUDE"]
                log("        INCLUDE += " + ";".join(sdk_inc))
            if need_sdk_lib and sdk_lib:
                env["LIB"] = ";".join(sdk_lib) + ";" + env["LIB"]
                log("        LIB     += " + ";".join(sdk_lib))

    _cached_env = env
    return env


def resolve_tool(name, env):
    """在 env 的 PATH 里找到工具的绝对路径。

    为什么不能只写 "cl.exe"：
      Python 的 subprocess 在 Windows 上不会像 cmd 那样按 PATHEXT 去搜索，
      直接传裸名字会抛 FileNotFoundError。必须自己解析出绝对路径。
    """
    path = env.get("PATH", "")
    for d in path.split(os.pathsep):
        d = d.strip().strip('"')
        if not d:
            continue
        cand = os.path.join(norm(d), name)
        if os.path.isfile(cand):
            return norm(cand)
    # 退路：让 shutil 在当前进程 PATH 里找
    found = shutil.which(name)
    if found:
        return norm(found)
    die(f"在 PATH 里找不到 {name}，请检查 Visual Studio 环境。")


def run_tool(args, env, cwd=None, quiet=False):
    """执行工具，失败即终止并打印完整输出"""
    # 第一个元素若是工具名（不含路径），解析成绝对路径
    args = list(args)
    if not (os.sep in args[0] or "/" in args[0]):
        args[0] = resolve_tool(args[0], env)
    if not quiet:
        log("  $ " + os.path.basename(args[0]) + " " + " ".join(args[1:]))
    p = subprocess.run(args, env=env, cwd=cwd, capture_output=True, text=True,
                       errors="replace")
    combined = (p.stdout or "") + (p.stderr or "")
    if p.returncode != 0:
        log(combined)
        die(f"命令失败（返回码 {p.returncode}）：{args[0]}")
    # cl 的警告也打出来，方便早发现
    if combined.strip() and not quiet:
        for line in combined.splitlines():
            print("    " + line)
    return combined


# ---------------------------------------------------------------------------
# 编译选项
# ---------------------------------------------------------------------------
# /MT  静态链接 CRT —— 任务书第五章要求，产物不依赖目标机 VC 运行库，
#      避免用户机器上"插件静默不加载"。代价：各 DLL 有独立堆，
#      所以绝不能跨 DLL delete（见 NppDockApi.h 的铁律 1）。
COMMON_FLAGS = [
    "/nologo",
    "/c",
    "/MT",
    "/W4", "/WX-",            # 警告级别 4；不把警告当错误（MSVC 头文件自身有告警）
    "/O2", "/Oi", "/GL",      # 优化 + 全局优化
    "/GS", "/guard:cf",       # 安全
    "/utf-8",                 # 源码/执行字符集都用 UTF-8，防止中文注释与字符串乱码
    "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WINDOWS",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/DNOMINMAX",             # 防止 windows.h 的 min/max 宏污染 std::min
]

LINK_FLAGS = [
    "/nologo",
    "/DLL",
    "/LTCG",
    "/INCREMENTAL:NO",
    "/OPT:REF", "/OPT:ICF",
    "/DYNAMICBASE", "/NXCOMPAT",
    "/MACHINE:X64",
]


def compile_one(env, src, obj, extra_defs=None, include_dirs=None, std="/std:c++17"):
    """编译单个 .cpp → .obj"""
    args = ["cl.exe"] + COMMON_FLAGS + [std]
    for d in (include_dirs or []):
        args.append("/I" + d)
    for d in (extra_defs or []):
        args.append("/D" + d)
    args += ["/Fo" + obj, src]
    run_tool(args, env, cwd=ROOT)


def link_dll(env, objs, out_dll, extra_libs=None, def_file=None, implib=None):
    """把 .obj 链接成 DLL"""
    args = ["link.exe"] + LINK_FLAGS + ["/OUT:" + out_dll]
    if implib:
        args.append("/IMPLIB:" + implib)
    if def_file:
        args.append("/DEF:" + def_file)
    args += objs
    args += extra_libs or []
    run_tool(args, env, cwd=ROOT)


def link_exe(env, objs, out_exe, extra_libs=None):
    """把 .obj 链接成独立的 GUI 可执行文件（NppDock 应用）。

    与 link_dll 的区别：/SUBSYSTEM:WINDOWS（GUI 子系统，不弹控制台）+ 不要 /DLL。
    /SUBSYSTEM:WINDOWS 下链接器会去找 wWinMainCRTStartup -> wWinMain，
    正好对应我们 /DUNICODE 的入口函数。
    """
    args = ["link.exe", "/nologo",
            "/OUT:" + out_exe,
            "/SUBSYSTEM:WINDOWS",
            "/LTCG",
            "/INCREMENTAL:NO",
            "/OPT:REF", "/OPT:ICF",
            "/DYNAMICBASE", "/NXCOMPAT",
            "/MACHINE:X64"]
    args += objs
    args += extra_libs or []
    run_tool(args, env, cwd=ROOT)


# ---------------------------------------------------------------------------
# 构建目标
# ---------------------------------------------------------------------------
def build_plugins(env, check_deps, dumpbin, rc):
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(OBJ_DIR, exist_ok=True)

    include_dirs = [
        SDK_DIR,
        SRC_DIR,
        os.path.join(SRC_DIR, "framework"),
        os.path.join(SRC_DIR, "main"),
    ]

    # ---- 主 DLL ----
    log("\n[1/2] 编译主插件 NppDock.dll（容器 + 加载器 + 状态持久化）")
    main_srcs = [
        os.path.join(SRC_DIR, "main", "NppDock.cpp"),
        os.path.join(SRC_DIR, "main", "NppDockContainer.cpp"),
    ]
    main_objs = []
    for s in main_srcs:
        o = os.path.join(OBJ_DIR, os.path.splitext(os.path.basename(s))[0] + ".obj")
        compile_one(env, s, o, include_dirs=include_dirs)
        main_objs.append(o)

    main_dll = os.path.join(OUT_DIR, "NppDock.dll")

    # 主 DLL 的资源：**工具栏图标 + 版本信息**。
    # ⚠️ 图标必须编进 DLL 本身 —— 工具栏按钮是从本模块 LoadImage 出来的，
    #    少了它按钮就是一格空白（而且完全静默，看不出是"没编进去"）。
    main_rc = os.path.join(SRC_DIR, "main", "NppDock.rc")
    if os.path.isfile(main_rc):
        if rc:
            res = os.path.join(OBJ_DIR, "NppDock.res")
            run_tool([rc, "/nologo", "/fo" + res,
                      "/I" + os.path.dirname(main_rc), main_rc], env, cwd=ROOT)
            main_objs.append(res)
            log("    已编译主 DLL 资源 NppDock.res（图标 + 版本信息）")
        else:
            log("    [warn] 未找到 rc.exe：工具栏图标与版本信息都编不进去")

    # 主 DLL 需要导出 6 个 C 函数；用 /EXPORT 显式指定，
    # 这样链接器不会按 C++ 名字修饰导出。
    export_flags = [
        "/EXPORT:setInfo",
        "/EXPORT:getName",
        "/EXPORT:getFuncsArray",
        "/EXPORT:beNotified",
        "/EXPORT:messageProc",
        "/EXPORT:isUnicode",
    ]
    link_dll(env, main_objs, main_dll,
             extra_libs=["user32.lib", "kernel32.lib", "comctl32.lib",
                         "shell32.lib", "shlwapi.lib", "gdi32.lib",
                         "version.lib"] + export_flags,
             implib=os.path.join(OBJ_DIR, "NppDock.lib"))

    # ---- 测试用哑模块（只进 build/tests/，不进正式产物目录）----
    # 它验证"往 NppDock_*.dll 里丢新 DLL 就能提供功能页，主插件零改动"。
    log("\n[extra] 编译测试用哑模块 NppDock_Dummy.dll（验证模块发现与懒加载）")
    dummy_src = os.path.join(SRC_DIR, "tests", "DummyModule.cpp")
    if os.path.isfile(dummy_src):
        os.makedirs(TEST_DIR, exist_ok=True)
        dummy_obj = os.path.join(OBJ_DIR, "DummyModule.obj")
        compile_one(env, dummy_src, dummy_obj, include_dirs=include_dirs)
        dummy_dll = os.path.join(TEST_DIR, "NppDock_Dummy.dll")
        link_dll(env, [dummy_obj], dummy_dll,
                 extra_libs=["user32.lib", "kernel32.lib", "gdi32.lib",
                             "/EXPORT:nppdock_module_entry",
                             "/EXPORT:nppdock_module_destroy"],
                 implib=os.path.join(OBJ_DIR, "NppDock_Dummy.lib"))
        log(f"  {dummy_dll}  ({os.path.getsize(dummy_dll):,} 字节)")

    # ---- NppDock 应用（独立 exe）----
    log("\n[2/2] 编译 NppDock 应用")
    app_exes = build_apps(env, rc)

    log("\n产物：")
    log(f"  {main_dll}  ({os.path.getsize(main_dll):,} 字节)")
    if os.path.isfile(dummy_dll):
        log(f"  {dummy_dll}  ({os.path.getsize(dummy_dll):,} 字节)  [仅测试]")

    if check_deps:
        check_dependencies(env, dumpbin, [main_dll] + app_exes)

    return main_dll


# ---------------------------------------------------------------------------
# NppDock 应用（独立 exe，可单独运行，也可被 dock 嵌入）
# ---------------------------------------------------------------------------
# 约定（见 README「应用契约」）：
#   - 产物命名 NppDockApp_<名字>.exe，与 NppDock.dll 同目录
#   - 容器靠这个前缀识别"要嵌的 app"
#   - exe 的 VERSIONINFO/FileDescription 就是 dock 里那个 tab 的标题
#   - 嵌入方式：exe --dock-parent <HWND>（由 exe 自己完成子窗口化）
APPS = [
    {
        "name": "MD5",
        # 产物名保持 NppDockApp_MD5.exe 不变 —— 它是 panel.ini 里记录应用页的键，
        # 改名会让"重开 N++ 自动接回上次的页"失效（见 README 第 6 节）。
        # 显示名（tab 标题）由 md5tool.rc 的 FileDescription 决定，那个可以随时改。
        "src": ["src/apps/md5tool/md5tool.cpp",
                "src/apps/md5tool/hashcore.cpp"],
        "rc": "src/apps/md5tool/md5tool.rc",
        "libs": ["user32.lib", "kernel32.lib", "gdi32.lib",
                 "comdlg32.lib", "shell32.lib"],
    },
    {
        "name": "NET",
        # 产物名 NppDockApp_NET.exe —— 它同样是 panel.ini 里记录应用页的键，
        # 改名会让"重开 N++ 自动接回上次的页"失效。显示名（tab 标题）由
        # nettest.rc 的 FileDescription 决定。
        # 界面 / 配置 / 引擎三层，外加一个自研的 mini JSON 读写
        # （配置文件是人手改的，但要能读进程序 —— 见 jsonlite.h 的说明）
        "src": ["src/apps/nettest/nettest.cpp",
                "src/apps/nettest/netcfg.cpp",
                "src/common/jsonlite.cpp",
                "src/apps/nettest/netcore.cpp"],
        "rc": "src/apps/nettest/nettest.rc",
        # 三个都是 Windows **系统自带**的库，不是第三方运行时：
        #   iphlpapi —— ICMP / ARP 表 / 适配器 / 连接表
        #   ws2_32   —— 域名解析 + TCP 连接探测
        #   dnsapi   —— DNS 记录查询（A/AAAA/CNAME/MX/TXT/NS/PTR）
        #   shell32  —— CommandLineToArgvW（R1 要求的命令行解析）
        # --check-deps 只禁止 MSVCP*/VCRUNTIME*/MSVCR*/api-ms-win-crt*，
        # 系统 DLL 不在禁止之列（见 README"依赖策略"一节）。
        "libs": ["user32.lib", "kernel32.lib", "gdi32.lib", "shell32.lib",
                 "iphlpapi.lib", "ws2_32.lib", "dnsapi.lib"],
    },
    {
        "name": "PACK",
        # 产物名 NppDockApp_PACK.exe —— 同样是 panel.ini 里记录应用页的键，
        # 改名会让"重开 N++ 自动接回上次的页"失效。显示名由 backpack.rc 决定。
        #
        # 三层：界面 / 配置(config.json) / 传输(本地目录 or ssh+sftp 命令行)。
        # 传输层选"复用 Windows 自带的 OpenSSH 客户端"（王 2026-10-03 拍板）：
        # 它是**系统组件**（System32\OpenSSH\），不是我们打包的第三方 exe，
        # 所以不违反"不许塞第三方程序"的家规；代价是"搬运字节"这段不是自研，
        # 已写成可替换的一小块（packcore 的四个动作），日后要换自研 SSH 只动它。
        # 共用：src/common/jsonlite.cpp（配置读写）、src/common/appui.cpp（字体/图标）
        "src": ["src/apps/backpack/backpack.cpp",
                "src/apps/backpack/packcfg.cpp",
                "src/apps/backpack/packcore.cpp",
                "src/apps/backpack/packprops.cpp",
                "src/common/jsonlite.cpp",
                "src/common/appui.cpp"],
        "rc": "src/apps/backpack/backpack.rc",
        #   comdlg32 —— 「上传 / 导出到本地…」的文件选择与另存为
        #   ole32    —— SHGetKnownFolderPath 拿到的路径要用 CoTaskMemFree 释放
        "libs": ["user32.lib", "kernel32.lib", "gdi32.lib", "shell32.lib",
                 "comdlg32.lib", "ole32.lib", "comctl32.lib"],
    },
]


def build_apps(env, rc):
    """构建所有 NppDock 应用（独立 exe）"""
    app_obj_dir = os.path.join(OBJ_DIR, "apps")
    os.makedirs(app_obj_dir, exist_ok=True)

    exes = []
    log(f"\n[app] 构建 NppDock 应用（{len(APPS)} 个）")
    for app in APPS:
        objs = []
        for rel in app["src"]:
            p = os.path.join(ROOT, rel)
            if not os.path.isfile(p):
                die(f"应用源码缺失：{p}")
            o = os.path.join(app_obj_dir,
                             os.path.splitext(os.path.basename(p))[0] + ".obj")
            compile_one(env, p, o, include_dirs=[SDK_DIR])
            objs.append(o)

        # 版本资源（FileDescription 会被 dock 读去当 tab 标题）
        rc_rel = app.get("rc")
        if rc_rel:
            src_rc = os.path.join(ROOT, rc_rel)
            if os.path.isfile(src_rc):
                if rc:
                    res = os.path.join(app_obj_dir,
                                       os.path.splitext(os.path.basename(src_rc))[0] + ".res")
                    # /I 指向 rc 所在目录：让 .rc 里用**相对路径**引用的图标/位图
                    # （如 "NppDock.ico"）能被找到 —— rc.exe 不会自动去 .rc 的同级目录找。
                    run_tool([rc, "/nologo", "/fo" + res,
                              "/I" + os.path.dirname(src_rc), src_rc], env, cwd=ROOT)
                    objs.append(res)
                    log(f"    已编译版本资源 {os.path.basename(res)}")
                else:
                    log(f"    [warn] 未找到 rc.exe，跳过版本资源；"
                        f"tab 标题将回退为文件名")

        out = os.path.join(OUT_DIR, f"NppDockApp_{app['name']}.exe")
        link_exe(env, objs, out, extra_libs=app["libs"])
        log(f"  {out}  ({os.path.getsize(out):,} 字节)")
        exes.append(out)
    return exes


def check_dependencies(env, dumpbin, dlls):
    """任务书第五章：必须用 dumpbin /dependents 检查产物不含 MSVCP*/VCRUNTIME*/MSVCR*"""
    log("\n=== 静态自检：dumpbin /dependents（验证 /MT 生效）===")
    if not dumpbin:
        log("  [跳过] 没找到 dumpbin.exe")
        return False

    bad_patterns = ["MSVCP", "VCRUNTIME", "MSVCR", "api-ms-win-crt"]
    ok = True
    for dll in dlls:
        out = subprocess.run([dumpbin, "/dependents", dll],
                             env=env, capture_output=True, text=True,
                             errors="replace").stdout
        deps = []
        in_block = False
        for line in out.splitlines():
            if "Image has the following dependencies" in line:
                in_block = True
                continue
            if in_block:
                if not line.strip() or "Summary" in line:
                    in_block = False
                    continue
                name = line.strip().split()[0] if line.strip() else ""
                if name.lower().endswith(".dll"):
                    deps.append(name)

        hits = [d for d in deps
                if any(d.upper().startswith(p.upper()) for p in bad_patterns)]
        status = "OK" if not hits else "失败"
        log(f"\n  {os.path.basename(dll)}  ({status})")
        for d in deps:
            marker = "  <== 不应出现！" if d in hits else ""
            log(f"    - {d}{marker}")
        if hits:
            ok = False
            log(f"    [FAIL] 出现了 CRT 动态库依赖：{hits}")
        else:
            log("    [OK] 不含 MSVCP*/VCRUNTIME*/MSVCR*/api-ms-win-crt*")

    log("\n" + ("静态自检：PASS" if ok else "静态自检：FAIL"))
    return ok


def build_tests(env):
    log("\n[tests] 编译离线测试")
    os.makedirs(TEST_DIR, exist_ok=True)

    # 离线集成测试：假宿主加载真实 DLL
    t2_src = os.path.join(SRC_DIR, "tests", "test_integration.cpp")
    if os.path.isfile(t2_src):
        t2_obj = os.path.join(TEST_DIR, "test_integration.obj")
        compile_one(env, t2_src, t2_obj, include_dirs=[SDK_DIR, SRC_DIR])
        t2_exe = os.path.join(TEST_DIR, "test_integration.exe")
        run_tool(["link.exe", "/nologo", "/OUT:" + t2_exe, "/SUBSYSTEM:CONSOLE",
                  "/MACHINE:X64", t2_obj,
                  "user32.lib", "kernel32.lib", "comctl32.lib", "gdi32.lib",
                  "shell32.lib", "shlwapi.lib"], env, cwd=ROOT)
        log(f"  {t2_exe}")
    else:
        log("  [跳过] 未找到 test_integration.cpp")


def clean():
    for d in (BUILD_DIR,):
        if os.path.isdir(d):
            log(f"清理 {d}")
            shutil.rmtree(d, ignore_errors=True)


def install_to_npp():
    """把产物复制到便携版 Notepad++ 的 plugins\\ 下（真机验证用）"""
    dst = os.path.join(NPP_PLUGINS, "NppDock")
    os.makedirs(dst, exist_ok=True)
    for f in ("NppDock.dll",):
        src = os.path.join(OUT_DIR, f)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(dst, f))
            log(f"  复制 {f} -> {dst}")

    # ★ 先清掉目标目录里旧的 app exe，再复制新的。
    #   容器是**按文件名前缀扫描** NppDockApp_*.exe 的，残留的旧 exe
    #   会在右键菜单里堆出一堆打不开的条目。
    for old in glob.glob(os.path.join(dst, "NppDockApp_*.exe")):
        os.remove(old)
        log(f"  移除旧应用 {os.path.basename(old)}")

    for app in APPS:
        name = f"NppDockApp_{app['name']}.exe"
        src = os.path.join(OUT_DIR, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(dst, name))
            log(f"  复制 {name} -> {dst}")

    # ★ 清理历史遗留的功能模块 DLL。
    #   容器是**按目录扫描** NppDock_*.dll 来发现功能页的，
    #   所以旧版本留下的模块 DLL 即使源码已删，仍会被加载并出现在面板里。
    #   这里主动删除，避免"删了源码但界面还在"的困惑。
    for old in glob.glob(os.path.join(dst, "NppDock_*.dll")):
        if os.path.basename(old).lower() == "nppdock_dummy.dll":
            continue
        os.remove(old)
        log(f"  移除历史模块 {os.path.basename(old)}")

    log("\n安装完成。用便携版启动：notepad++.exe -multiInst")


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="NppDock 构建脚本")
    ap.add_argument("--check-deps", action="store_true",
                    help="构建后用 dumpbin /dependents 做静态自检")
    ap.add_argument("--tests", action="store_true", help="只构建测试")
    ap.add_argument("--clean", action="store_true", help="清理构建目录")
    ap.add_argument("--install", action="store_true",
                    help="构建后复制到便携版 Notepad++ 的 plugins 目录")
    args = ap.parse_args()

    if args.clean:
        clean()
        return 0

    log("=" * 72)
    log("NppDock 构建")
    log("=" * 72)
    log(f"工作区   : {WORKSPACE}")
    log(f"插件目录 : {PLUGIN_DIR}")

    vs_root, vcvars = find_vs()
    log(f"VS       : {vs_root}")
    log(f"vcvars   : {vcvars}")
    env = vcvars_env(vcvars)
    dumpbin = find_dumpbin(vs_root)
    rc = find_rc()
    log(f"dumpbin  : {dumpbin}")
    log(f"rc       : {rc or '(未找到，应用将没有版本资源，tab 标题回退为文件名)'}")

    if args.tests:
        build_tests(env)
    else:
        build_plugins(env, args.check_deps, dumpbin, rc)
        build_tests(env)
        if args.install:
            install_to_npp()

    log("\n构建结束。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
