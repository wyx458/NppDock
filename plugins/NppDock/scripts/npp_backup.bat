@echo off
REM ============================================================================
REM  npp_backup.bat -- 备份当前 Notepad++ 便携版整个目录
REM  (本文件以 GBK/ANSI 编码保存，cmd.exe 才能正确显示中文)
REM ----------------------------------------------------------------------------
REM  放在 Notepad++ 根目录（与 notepad++.exe 同级），双击即可运行。
REM
REM  行为：
REM    1. 把脚本所在目录下的所有内容打进一个 zip
REM    2. 命名：npp_bak_YYYYMMDD.zip（与 npp_bak_20261001.zip 同款风格）
REM       同一天重复运行 -> npp_bak_YYYYMMDD_2.zip、_3.zip …（不覆盖已有备份）
REM
REM  排除（按要求）：
REM    - notepad++.exe        主程序本体
REM    - pluginsWorkspace\    插件开发工作区
REM    - *.zip                所有 zip，避免"备份里套备份"
REM
REM  用法：
REM    npp_backup.bat            正常备份
REM    npp_backup.bat -v         额外打印每个被写入的文件路径
REM ============================================================================

REM 注意：本文件本身以 GBK 保存，控制台保持默认代码页(936)即可正确显示中文。
REM       千万不要 chcp 65001，否则中文会变成乱码。
setlocal EnableExtensions DisableDelayedExpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

REM ---------------------------------------------------------------------------
REM  取日期。%DATE% 随区域设置变化极大，本机实测是 "周四 26.10.01"
REM  （星期还可能在后：26.10.01 周四；也有 2026/10/01、01-10-2026 等等）。
REM
REM  策略（三步，已验证）：
REM    1) 把 / . - 三种分隔符全换成空格；
REM    2) 逐 token 过滤：只保留"纯数字"的 token，周四/Thu/AM 之类一律丢弃，
REM       依次存入 T1/T2/T3；
REM    3) 谁有 4 位谁就是年，剩下两个是月日；若三个都是 1-2 位（本机情形
REM       "26.10.01"），则按"年在前"处理，2 位年补成 20xx。
REM
REM  （不用 wmic：受限环境可能被禁用，且 Win11 已弃用。）
REM ---------------------------------------------------------------------------
set "S=%DATE%"
set "S=%S:/= %"
set "S=%S:.= %"
set "S=%S:-= %"

set "T1=" & set "T2=" & set "T3="
for %%T in (%S%) do call :collect_token "%%T"

set "Y=" & set "M=" & set "D="

REM 4 位 token 视为年：依次检查 T1、T3（DMY 与 YMD 两种主流排列）
if not defined Y if not "%T1:~3,1%"=="" if "%T1:~4,1%"=="" set "Y=%T1%" & set "M=%T2%" & set "D=%T3%"
if not defined Y if not "%T3:~3,1%"=="" if "%T3:~4,1%"=="" set "Y=%T3%" & set "M=%T2%" & set "D=%T1%"

REM 无 4 位 token：按"2 位年在前"处理（本机 26.10.01 -> 2026/10/01）
if not defined Y if not defined M set "Y=20%T1%" & set "M=%T2%" & set "D=%T3%"

REM 兜底
if not defined Y (
    echo [WARN] 无法解析 DATE ^(%DATE%^)，改用固定日期戳。
    set "Y=1970" & set "M=01" & set "D=01"
)

REM 月/日补零
if "%M:~1,1%"=="" set "M=0%M%"
if "%D:~1,1%"=="" set "D=0%D%"

set "STAMP=%Y%%M%%D%"

echo.
REM  注意：这里不能用 "->"，cmd 会把 > 当成重定向，从而生成以日期戳为名的
REM        垃圾文件。用 "==" 或转义成 "^>"。
echo [info] 日期解析: %DATE%  ==  %STAMP%
echo.

REM ===========================================================================
REM  安全护栏：绝不覆盖任何已存在的 npp_bak_*.zip
REM ---------------------------------------------------------------------------
REM  血泪教训：曾经因为“软删除 + 重建”把用户手工留存的 npp_bak_20261001.zip
REM  顶掉过（好在回收站取回）。这里再加一道独立保险：
REM  只要目标名已存在，就一路往后找 _2.._99，仍找不到就报错退出，绝不覆盖。
REM ===========================================================================
set "OUT=%ROOT%\npp_bak_%STAMP%.zip"
if not exist "%OUT%" goto :name_ok
set "OUT="
for /L %%N in (2,1,99) do (
    if not defined OUT (
        if not exist "%ROOT%\npp_bak_%STAMP%_%%N.zip" set "OUT=%ROOT%\npp_bak_%STAMP%_%%N.zip"
    )
)
if not defined OUT (
    echo [错误] npp_bak_%STAMP% 的基础名与 _2.._99 全被占用，已放弃以免覆盖。
    endlocal
    exit /b 1
)
:name_ok
set "TAR=%SystemRoot%\System32\tar.exe"
if not exist "%TAR%" goto :no_tar

echo.
echo ============================================================
echo  Notepad++ 目录备份
echo ------------------------------------------------------------
echo  源目录 : %ROOT%
echo  目标   : %OUT%
echo  排除   : notepad++.exe  pluginsWorkspace  *.zip
echo ============================================================
echo.

pushd "%ROOT%"

if /i "%~1"=="-v" goto :run_verbose

"%TAR%" -a -c -f "%OUT%" --exclude=notepad++.exe --exclude=pluginsWorkspace --exclude=*.zip .
goto :check

:run_verbose
"%TAR%" -a -c -f "%OUT%" --exclude=notepad++.exe --exclude=pluginsWorkspace --exclude=*.zip -v .

:check
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" goto :fail
if not exist "%OUT%" goto :fail

for %%F in ("%OUT%") do set "SZ=%%~zF"
echo.
echo [成功] 备份完成
echo        文件 : %OUT%
echo        大小 : %SZ% 字节
echo.
echo 提示：按要求已排除 notepad++.exe、pluginsWorkspace 与所有 zip。
echo       如需完整备份，请手工把这三类一并复制。
echo.
endlocal
exit /b 0

:no_tar
echo.
echo [错误] 找不到 %TAR%
echo        Windows 10 1803 及以上应自带该程序。
endlocal
exit /b 1

:fail
echo.
echo [失败] 压缩返回码 = %RC%
endlocal
exit /b 1

REM ===========================================================================
REM  子程序：只收集"纯数字"token，依次放进 T1/T2/T3。
REM    - 含非数字字符（周四、Thu、AM…）-> 丢弃
REM    - 用 for /f "delims=0123456789" 探测：若能把字符切出来，说明含非数字
REM  实测 "周四 26.10.01 周四" -> T1=26 T2=10 T3=01
REM ===========================================================================
:collect_token
set "TK=%~1"
if "%TK%"=="" goto :eof
set "CHK=%TK%"
for /f "delims=0123456789" %%X in ("%TK%") do set "CHK=BAD"
if "%CHK%"=="BAD" goto :eof
if not defined T1 set "T1=%CHK%" & goto :eof
if not defined T2 set "T2=%CHK%" & goto :eof
if not defined T3 set "T3=%CHK%"
goto :eof
