$ErrorActionPreference = 'Stop'

$buildRoot = (Resolve-Path (Join-Path (Split-Path -Parent $PSScriptRoot) 'build')).Path
$targets = Get-CimInstance Win32_Process |
    Where-Object {
        $_.ProcessId -in @(9660, 4364, 9840, 17184) -and
        ($_.CommandLine -like "*$buildRoot*" -or $_.Name -in @('link.exe', 'mspdbsrv.exe'))
    } |
    Sort-Object { if ($_.Name -eq 'cmake.exe') { 1 } else { 0 } }

if (-not $targets) {
    Write-Host 'No timed-out build processes remain.'
    exit 0
}

foreach ($process in $targets) {
    Write-Host "Stopping PID $($process.ProcessId) $($process.Name)"
    Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds 1
$remaining = Get-Process -Id $targets.ProcessId -ErrorAction SilentlyContinue
if ($remaining) {
    throw "Build processes still running: $($remaining.Id -join ', ')"
}
Write-Host 'Timed-out build process tree stopped.'
