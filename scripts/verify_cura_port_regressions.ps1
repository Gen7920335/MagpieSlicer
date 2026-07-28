$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$exe = Join-Path $repo 'build\src\Release\orca-slicer.exe'
if (-not (Test-Path $exe)) {
    throw "Executable not found: $exe"
}

$tests = @(
    'scripts\verify_cura_support_geometry.ps1',
    'scripts\verify_cura_bottom_interface.ps1',
    'scripts\verify_support_features.ps1'
)

$summary = [System.Collections.Generic.List[object]]::new()
foreach ($test in $tests) {
    $started = Get-Date
    Write-Host "=== Running $test ==="
    if ($test -like '*verify_support_features.ps1') {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $test -SlicerPath $exe
    } else {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $test -OrcaExe $exe
    }
    $exitCode = $LASTEXITCODE
    $summary.Add([pscustomobject]@{
        Test = $test
        ExitCode = $exitCode
        Seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    })
    if ($exitCode -ne 0) {
        break
    }
}

Write-Host '=== Regression summary ==='
$summary | Format-Table -AutoSize
if ($summary.Count -ne $tests.Count -or @($summary | Where-Object ExitCode -ne 0).Count -gt 0) {
    throw 'Cura port regression suite failed.'
}
