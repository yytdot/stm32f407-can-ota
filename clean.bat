@echo off
chcp 65001 >nul
title Keil 工程清理

echo ============================================
echo   清理 Keil 编译中间文件
echo   只删编译产物，源码和工程文件不动
echo ============================================
echo.
echo 将删除：
echo   MDK-ARM\jianlixiangmu\    App 的 .o .crf .axf .hex 等
echo   MDK-ARM\Bootloader\       Bootloader 的中间文件
echo   MDK-ARM\RTE\              自动生成的 RTE 头文件
echo   MDK-ARM\DebugConfig\
echo   MDK-ARM\*.dep *.lnp *.map *.htm *.uvguix.*
echo.
echo 不会删除：.uvprojx 工程文件、.uvoptx 调试设置、所有源码
echo.

set /p ok=确认清理？(Y/N):
if /i not "%ok%"=="Y" goto :cancel

cd /d "%~dp0"

if exist "MDK-ARM\jianlixiangmu"  rd /s /q "MDK-ARM\jianlixiangmu"
if exist "MDK-ARM\Bootloader"     rd /s /q "MDK-ARM\Bootloader"
if exist "MDK-ARM\RTE"            rd /s /q "MDK-ARM\RTE"
if exist "MDK-ARM\DebugConfig"    rd /s /q "MDK-ARM\DebugConfig"
if exist "MDK-ARM\Objects"        rd /s /q "MDK-ARM\Objects"
if exist "MDK-ARM\Listings"       rd /s /q "MDK-ARM\Listings"

del /q "MDK-ARM\*.dep"       2>nul
del /q "MDK-ARM\*.lnp"       2>nul
del /q "MDK-ARM\*.map"       2>nul
del /q "MDK-ARM\*.htm"       2>nul
del /q "MDK-ARM\*.lst"       2>nul
del /q "MDK-ARM\*.uvguix.*"  2>nul
del /q "*.uvguix.*"          2>nul

echo.
echo 清理完成。下次打开 Keil 重新编译即可（Rebuild）。
echo.
pause
exit /b 0

:cancel
echo 已取消，什么都没删。
pause
exit /b 0