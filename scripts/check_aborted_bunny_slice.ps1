$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $repo 'build\verification\cura-bunny-support-top'

Write-Host '=== Active Orca CLI processes ==='
$active = Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -match '^orca-slicer.*\.exe$' -and
        $_.CommandLine -like '*cura-bunny-support-top*'
    }
$active | Select-Object ProcessId, Name, CreationDate, CommandLine | Format-List

Write-Host '=== Latest bunny output ==='
if (Test-Path $outputRoot) {
    $latest = Get-ChildItem $outputRoot -Directory |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($latest) {
        Write-Host $latest.FullName
        Get-ChildItem $latest.FullName -File |
            Select-Object Name, Length, LastWriteTime |
            Format-Table -AutoSize
    }
}
