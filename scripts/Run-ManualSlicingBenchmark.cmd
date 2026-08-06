@echo off
setlocal
title Magpie Manual Slicing Benchmark
echo Close every running Magpie Slicer window before continuing.
echo This launcher will start Magpie with Vulkan diagnostics enabled.
echo.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor_gui_slicing_benchmark.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" echo Monitor stopped with an error. Review the message above.
pause
exit /b %EXIT_CODE%
