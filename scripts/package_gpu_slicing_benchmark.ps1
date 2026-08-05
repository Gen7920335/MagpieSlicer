param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $OutputDirectory = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepoRoot 'build\tools'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stage = Join-Path $OutputDirectory "MagpieGpuBenchmark-$stamp"
$zip = "$stage.zip"

$files = [ordered]@{
    'benchmark_gpu_slicing.ps1' = 'scripts\benchmark_gpu_slicing.ps1'
    'Run-GpuSlicingBenchmark.cmd' = 'scripts\Run-GpuSlicingBenchmark.cmd'
    'README.md' = 'docs\gpu-slicing-benchmark.md'
    'lib\GcodeGeometryAnalyzer.cs' = 'scripts\lib\GcodeGeometryAnalyzer.cs'
    'data\machine.json' = 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
    'data\process.json' = 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
}

New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($entry in $files.GetEnumerator()) {
    $source = Join-Path $RepoRoot $entry.Value
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing package input: $source" }
    $destination = Join-Path $stage $entry.Key
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}

$manifest = [ordered]@{
    schema = 1
    name = 'Magpie GPU Slicing Benchmark'
    created_at = (Get-Date).ToString('o')
    git_head = (& git -C $RepoRoot rev-parse HEAD).Trim()
    files = @($files.Keys | ForEach-Object {
        $path = Join-Path $stage $_
        [ordered]@{
            path = $_ -replace '\\','/'
            bytes = (Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

Write-Output "PACKAGE=$zip"
Write-Output "SHA256=$((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)"
Write-Output "BYTES=$((Get-Item -LiteralPath $zip).Length)"
