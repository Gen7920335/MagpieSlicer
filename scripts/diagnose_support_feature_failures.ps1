$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$root = Get-ChildItem 'build\verification\support-features' -Directory |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $root) {
    throw 'No support-feature verification directory found.'
}

Write-Host "Verification directory: $($root.FullName)"
Write-Host '=== Results ==='
Get-Content (Join-Path $root.FullName 'results.json') -Raw

Write-Host '=== Logs and generated files ==='
Get-ChildItem $root.FullName -Recurse -File |
    Select-Object FullName, Length, LastWriteTime |
    Format-Table -AutoSize

Write-Host '=== Error-like text ==='
Get-ChildItem $root.FullName -Recurse -File |
    Where-Object { $_.Extension -in '.log', '.txt', '.err' } |
    ForEach-Object {
        Write-Host "--- $($_.FullName) ---"
        Get-Content $_.FullName -Tail 80
    }

Write-Host '=== Test invocation code ==='
$script = Get-Content 'scripts\verify_support_features.ps1'
for ($i = 1; $i -le $script.Count; $i++) {
    if ($i -le 215) {
        '{0,5}: {1}' -f $i, $script[$i - 1]
    }
}
