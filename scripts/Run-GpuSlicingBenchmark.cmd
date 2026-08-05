@echo off
setlocal
title Magpie GPU Slicing Benchmark
echo Magpie CPU vs Vulkan slicing benchmark
echo Results will be written under build\verification\gpu-slicing-benchmark.
echo.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0benchmark_gpu_slicing.ps1" -OpenReport %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" echo Benchmark reported an invalid result. Review the HTML report and logs.
pause
exit /b %EXIT_CODE%
