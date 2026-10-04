"""运行编译产物并回显输出（绕开混合 shell 的 stdout 捕获问题）。

用法: python run.py <exe路径> [参数...]
返回：被测程序退出码（原样传递，便于 CI 判断）
"""
import subprocess
import sys
import os

if len(sys.argv) < 2:
    print("用法: python run.py <exe> [args...]")
    sys.exit(2)

exe = sys.argv[1]
if not os.path.isfile(exe):
    print(f"找不到可执行文件：{exe}")
    sys.exit(2)

# 直接继承父进程的 stdout/stderr，避免管道捕获在混合 shell 下丢输出
p = subprocess.run([exe] + sys.argv[2:])
sys.exit(p.returncode)
