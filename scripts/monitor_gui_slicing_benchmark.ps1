param(
    [string] $SlicerPath = "",
    [string] $OutputRoot = "",
    [string] $AppDataRoot = "",
    [ValidateRange(30, 600)]
    [int] $StartupTimeoutSeconds = 120,
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Invariant = [Globalization.CultureInfo]::InvariantCulture

function Get-Sha256Text([string] $Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-LogTimestamp([string] $Line) {
    $match = [regex]::Match($Line, '^\[[^\]]+\]\s+(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6})')
    if (-not $match.Success) { return $null }
    return [DateTime]::ParseExact($match.Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.ffffff', $Invariant,
        [Globalization.DateTimeStyles]::AssumeLocal)
}

function Get-Median([double[]] $Values) {
    if ($Values.Count -eq 0) { return [double]::NaN }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-VulkanMode([string] $ConfigPath) {
    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { return 'auto' }
    try {
        $text = [IO.File]::ReadAllText($ConfigPath)
        $match = [regex]::Match($text, '"vulkan_slicer_mode"\s*:\s*"(auto|on|off)"', 'IgnoreCase')
        if ($match.Success) { return $match.Groups[1].Value.ToLowerInvariant() }
    } catch {}
    return 'auto'
}

function Get-DiagnosticsSnapshot([string] $Path) {
    [int64] $requests = 0
    [int] $dispatches = 0
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in [IO.File]::ReadLines($Path)) {
            $match = [regex]::Match($line, '^([^,]+),(\d+)$')
            if (-not $match.Success) { continue }
            ++$dispatches
            $requests += [int64]$match.Groups[2].Value
        }
    }
    return [pscustomobject]@{ dispatches=$dispatches; requests=$requests }
}

function Read-GcodeConfiguration([string] $Path) {
    $empty = [pscustomobject]@{
        available = $false; sha256 = ''; bytes = 0; values = @{}; error = ''
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $empty.error = 'G-code file not found'
        return $empty
    }
    try {
        $item = Get-Item -LiteralPath $Path
        $tailBytes = [Math]::Min([int64](16MB), $item.Length)
        $buffer = [byte[]]::new([int]$tailBytes)
        $stream = [IO.File]::Open($item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            [void]$stream.Seek(-$tailBytes, [IO.SeekOrigin]::End)
            $read = $stream.Read($buffer, 0, $buffer.Length)
        } finally {
            $stream.Dispose()
        }
        $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
        $start = $text.LastIndexOf('; CONFIG_BLOCK_START', [StringComparison]::Ordinal)
        $end = $text.LastIndexOf('; CONFIG_BLOCK_END', [StringComparison]::Ordinal)
        if ($start -lt 0 -or $end -le $start) {
            $empty.bytes = $item.Length
            $empty.error = 'G-code configuration block not found in the final 16 MiB'
            return $empty
        }
        $block = $text.Substring($start, $end + '; CONFIG_BLOCK_END'.Length - $start)
        $normalized = (($block -split '\r?\n') | ForEach-Object { $_.TrimEnd() }) -join "`n"
        $values = @{}
        foreach ($line in ($block -split '\r?\n')) {
            $match = [regex]::Match($line, '^;\s*([^=]+?)\s*=\s*(.*?)\s*$')
            if ($match.Success) { $values[$match.Groups[1].Value.Trim()] = $match.Groups[2].Value.Trim() }
        }
        return [pscustomobject]@{
            available = $true
            sha256 = Get-Sha256Text $normalized
            bytes = $item.Length
            values = $values
            error = ''
        }
    } catch {
        $empty.error = $_.Exception.Message
        return $empty
    }
}

function Find-SlicerExecutable {
    $candidates = [Collections.Generic.List[string]]::new()
    foreach ($path in @(
        (Join-Path ${env:ProgramFiles} 'Magpie Slicer\magpie-slicer.exe'),
        (Join-Path ${env:ProgramFiles} 'MagpieSlicer\magpie-slicer.exe')
    )) { if ($path) { $candidates.Add($path) } }
    if (Test-Path -LiteralPath 'C:\MagpieInstallTest' -PathType Container) {
        Get-ChildItem -LiteralPath 'C:\MagpieInstallTest' -Directory |
            Sort-Object LastWriteTime -Descending |
            ForEach-Object { $candidates.Add((Join-Path $_.FullName 'magpie-slicer.exe')) }
    }
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $candidates.Add((Join-Path $repoRoot 'build-vulkan\src\Release\magpie-slicer.exe'))
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return [IO.Path]::GetFullPath($candidate) }
    }
    throw 'Magpie Slicer was not found. Pass -SlicerPath explicitly.'
}

function Get-ImportantSetting([hashtable] $Values, [string] $Name) {
    if ($Values.ContainsKey($Name)) { return [string]$Values[$Name] }
    return ''
}

function Write-Results([Collections.Generic.List[object]] $Results, [string] $Root) {
    $jsonPath = Join-Path $Root 'manual-slicing-runs.json'
    $csvPath = Join-Path $Root 'manual-slicing-runs.csv'
    $resultItems = @($Results | ForEach-Object { $_ })
    $resultItems | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
    $resultItems | Select-Object run,selected_mode,actual_backend,core_seconds,total_seconds,
        vulkan_dispatches,vulkan_requests,settings_sha256,printer_profile,process_profile,
        filament_profiles,layer_height,wall_generator,infill_pattern,infill_density,gcode_bytes,gcode_path |
        Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8

    $comparisons = [Collections.Generic.List[object]]::new()
    foreach ($hash in (@($resultItems | Where-Object { $_.settings_sha256 } | ForEach-Object { $_.settings_sha256 } | Sort-Object -Unique))) {
        $same = @($resultItems | Where-Object { $_.settings_sha256 -eq $hash })
        $cpu = @($same | Where-Object { $_.actual_backend -eq 'cpu' })
        $gpu = @($same | Where-Object { $_.actual_backend -eq 'vulkan' })
        if ($cpu.Count -eq 0 -or $gpu.Count -eq 0) { continue }
        $cpuMedian = Get-Median @($cpu.core_seconds)
        $gpuMedian = Get-Median @($gpu.core_seconds)
        $comparisons.Add([pscustomobject][ordered]@{
            settings_sha256 = $hash
            cpu_runs = $cpu.Count
            vulkan_runs = $gpu.Count
            cpu_median_seconds = [Math]::Round($cpuMedian, 4)
            vulkan_median_seconds = [Math]::Round($gpuMedian, 4)
            speedup = [Math]::Round($cpuMedian / $gpuMedian, 4)
            percent_faster = [Math]::Round((($cpuMedian / $gpuMedian) - 1.0) * 100.0, 2)
        })
    }
    $comparisonItems = @($comparisons | ForEach-Object { $_ })
    $comparisonItems | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Root 'manual-slicing-comparisons.json') -Encoding UTF8
    return $comparisonItems
}

if ($SelfTest) {
    $testRoot = Join-Path ([IO.Path]::GetTempPath()) ('magpie-manual-monitor-selftest-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
    $gcode = Join-Path $testRoot 'sample.gcode'
    @"
G1 X1 Y1 E1
; CONFIG_BLOCK_START
; layer_height = 0.20
; printer_settings_id = Test Printer
; print_settings_id = Test Process
; sparse_infill_pattern = grid
; CONFIG_BLOCK_END
"@ | Set-Content -LiteralPath $gcode -Encoding UTF8
    $parsed = Read-GcodeConfiguration $gcode
    $timestamp = Get-LogTimestamp '[info]  2026-08-05 12:34:56.123456[Thread 0x1]:Starting the slicing process.'
    if (-not $parsed.available -or -not $parsed.sha256 -or $parsed.values.layer_height -ne '0.20' -or $null -eq $timestamp) {
        throw 'Manual slicing monitor self-test failed'
    }
    Write-Output 'SELF_TEST=PASS'
    Write-Output "SETTINGS_SHA256=$($parsed.sha256)"
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SlicerPath)) { $SlicerPath = Find-SlicerExecutable }
if (-not (Test-Path -LiteralPath $SlicerPath -PathType Leaf)) { throw "Slicer not found: $SlicerPath" }
$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
if ([string]::IsNullOrWhiteSpace($AppDataRoot)) { $AppDataRoot = Join-Path $env:APPDATA 'MagpieSlicer' }
$AppDataRoot = [IO.Path]::GetFullPath($AppDataRoot)
$logRoot = Join-Path $AppDataRoot 'log'
$configPath = Join-Path $AppDataRoot 'MagpieSlicer.conf'
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $PSScriptRoot ('manual-slicing-results\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$diagnosticsPath = Join-Path $OutputRoot 'vulkan-dispatch.csv'

$running = @(Get-CimInstance Win32_Process -Filter "Name = 'magpie-slicer.exe'" -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $details = ($running | ForEach-Object { "PID $($_.ProcessId): $($_.ExecutablePath)" }) -join "`n"
    throw "Magpie Slicer is already running. Close it, then run this CMD again so diagnostics can be attached.`n$details"
}

$environmentNames = @(
    'MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE', 'MAGPIE_VULKAN_SLICER_DIAGNOSTICS',
    'MAGPIE_VULKAN_SLICER_ENABLE', 'MAGPIE_VULKAN_SLICER_GPU_PRIORITY',
    'MAGPIE_VULKAN_SLICER_FORCE_DISPATCH', 'MAGPIE_VULKAN_SLICER_VALIDATION',
    'MAGPIE_VULKAN_SLICER_POLICY', 'ORCA_VULKAN_SLICER_POLICY'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE = $diagnosticsPath
    $env:MAGPIE_VULKAN_SLICER_DIAGNOSTICS = '1'
    $launchTime = Get-Date
    $slicer = Start-Process -FilePath $SlicerPath -PassThru
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}

Write-Host ''
Write-Host 'Magpie manual slicing benchmark is running.' -ForegroundColor Cyan
Write-Host '1. Slice the same project with Vulkan: Off at least three times.'
Write-Host '2. Slice it with Vulkan: On at least three times.'
Write-Host '3. Do not change model or settings between comparable runs.'
Write-Host '4. Press Q in this window to stop, or close Magpie Slicer.'
Write-Host ''
Write-Host "Slicer: $SlicerPath"
Write-Host "Results: $OutputRoot"

$deadline = $launchTime.AddSeconds($StartupTimeoutSeconds)
$logPath = $null
while ((Get-Date) -lt $deadline -and $null -eq $logPath) {
    if (-not (Test-Path -LiteralPath $logRoot -PathType Container)) { Start-Sleep -Milliseconds 250; continue }
    $candidate = Get-ChildItem -LiteralPath $logRoot -File -Filter 'debug_*.log.0' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $launchTime.AddSeconds(-2) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -ne $candidate) { $logPath = $candidate.FullName; break }
    Start-Sleep -Milliseconds 250
}
if ($null -eq $logPath) { throw "Magpie log was not created within $StartupTimeoutSeconds seconds: $logRoot" }

$stream = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
    [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
$reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
$results = [Collections.Generic.List[object]]::new()
$active = $null
$lastComparisonCount = 0

try {
    while (-not $slicer.HasExited) {
        while (($line = $reader.ReadLine()) -ne $null) {
            $timestamp = Get-LogTimestamp $line
            if ($null -eq $timestamp) { continue }

            if ($line -like '*will start slicing, reset gcode_result*') {
                if ($null -ne $active) {
                    Write-Warning "Run $($active.run) was replaced by a new slice before export completed."
                }
                $diag = Get-DiagnosticsSnapshot $diagnosticsPath
                $active = [pscustomobject][ordered]@{
                    run = $results.Count + 1
                    started = $timestamp
                    core_finished = $null
                    selected_mode_start = Get-VulkanMode $configPath
                    diagnostics_start = $diag
                    gcode_path = ''
                }
                Write-Host ("[{0:HH:mm:ss.fff}] Run {1} started; selected mode={2}" -f $timestamp,$active.run,$active.selected_mode_start) -ForegroundColor Yellow
                continue
            }
            if ($null -eq $active) { continue }
            if ($line -like '*after print::process, send slicing complete event*') {
                $active.core_finished = $timestamp
                continue
            }
            $pathMatch = [regex]::Match($line, 'Will export G-code to (.+?) soon$')
            if ($pathMatch.Success) {
                $active.gcode_path = $pathMatch.Groups[1].Value.Trim()
                continue
            }
            if ($line -like '*export gcode finished*') {
                $diag = Get-DiagnosticsSnapshot $diagnosticsPath
                $dispatches = [Math]::Max(0, $diag.dispatches - $active.diagnostics_start.dispatches)
                $requests = [Math]::Max(0L, $diag.requests - $active.diagnostics_start.requests)
                $backend = if ($dispatches -gt 0) { 'vulkan' } else { 'cpu' }
                $configuration = Read-GcodeConfiguration $active.gcode_path
                $coreEnd = if ($null -ne $active.core_finished) { $active.core_finished } else { $timestamp }
                $values = $configuration.values
                $result = [pscustomobject][ordered]@{
                    run = $active.run
                    started = $active.started.ToString('o')
                    finished = $timestamp.ToString('o')
                    selected_mode = $active.selected_mode_start
                    selected_mode_end = Get-VulkanMode $configPath
                    actual_backend = $backend
                    core_seconds = [Math]::Round(($coreEnd - $active.started).TotalSeconds, 4)
                    total_seconds = [Math]::Round(($timestamp - $active.started).TotalSeconds, 4)
                    vulkan_dispatches = $dispatches
                    vulkan_requests = $requests
                    settings_available = $configuration.available
                    settings_sha256 = $configuration.sha256
                    settings_error = $configuration.error
                    printer_profile = Get-ImportantSetting $values 'printer_settings_id'
                    process_profile = Get-ImportantSetting $values 'print_settings_id'
                    filament_profiles = Get-ImportantSetting $values 'filament_settings_id'
                    layer_height = Get-ImportantSetting $values 'layer_height'
                    wall_generator = Get-ImportantSetting $values 'wall_generator'
                    infill_pattern = Get-ImportantSetting $values 'sparse_infill_pattern'
                    infill_density = Get-ImportantSetting $values 'sparse_infill_density'
                    gcode_bytes = $configuration.bytes
                    gcode_path = $active.gcode_path
                }
                $results.Add($result)
                $comparisons = @(Write-Results $results $OutputRoot)
                $color = if ($backend -eq 'vulkan') { 'Cyan' } else { 'Green' }
                Write-Host ("[{0:HH:mm:ss.fff}] Run {1} finished: backend={2}, core={3:N3}s, total={4:N3}s, dispatches={5}, requests={6}" -f
                    $timestamp,$result.run,$backend,$result.core_seconds,$result.total_seconds,$dispatches,$requests) -ForegroundColor $color
                if (-not $configuration.available) { Write-Warning $configuration.error }
                if ($comparisons.Count -gt $lastComparisonCount) {
                    $latest = $comparisons[-1]
                    Write-Host ("MATCHED SETTINGS: CPU median={0:N3}s, Vulkan median={1:N3}s, speedup={2:N3}x ({3:N2}%)" -f
                        $latest.cpu_median_seconds,$latest.vulkan_median_seconds,$latest.speedup,$latest.percent_faster) -ForegroundColor Magenta
                    $lastComparisonCount = $comparisons.Count
                }
                $active = $null
            }
        }
        try {
            if ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                if ($key.Key -eq [ConsoleKey]::Q) { break }
            }
        } catch {
            # A redirected or detached console cannot report keyboard state.
        }
        Start-Sleep -Milliseconds 100
    }
} finally {
    $reader.Dispose()
    $stream.Dispose()
    if ($results.Count -gt 0) { [void](Write-Results $results $OutputRoot) }
}

Write-Host ''
Write-Host "Monitoring stopped. Runs recorded: $($results.Count)" -ForegroundColor Cyan
Write-Host "CSV: $(Join-Path $OutputRoot 'manual-slicing-runs.csv')"
Write-Host "Comparisons: $(Join-Path $OutputRoot 'manual-slicing-comparisons.json')"
