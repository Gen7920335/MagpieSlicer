param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $SlicerPath = "",
    [string[]] $ModelPath = @(),
    [string] $MachineSettings = "",
    [string] $ProcessSettings = "",
    [string[]] $FilamentProfiles = @(),
    [string] $OutputRoot = "",
    [ValidateRange(3, 20)]
    [int] $Repeats = 5,
    [ValidateRange(0, 5)]
    [int] $WarmupPairs = 1,
    [ValidateRange(30, 3600)]
    [int] $SliceTimeoutSeconds = 900,
    [ValidateRange(4, 14)]
    [int] $SyntheticGridSize = 10,
    [ValidateRange(20, 120)]
    [double] $SyntheticHeightMm = 80,
    [switch] $OpenReport
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$analyzerSource = Join-Path $PSScriptRoot 'lib\GcodeGeometryAnalyzer.cs'
if (-not (Test-Path -LiteralPath $analyzerSource -PathType Leaf)) {
    throw "G-code geometry analyzer not found: $analyzerSource"
}
Add-Type -Path $analyzerSource

function Resolve-ExistingFile([string] $Path, [string] $Description) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description not found: $Path"
    }
    return [IO.Path]::GetFullPath($Path)
}

function Find-SlicerExecutable {
    $candidates = @(
        (Join-Path $RepoRoot 'build-vulkan\src\Release\magpie-slicer-vulkan-test.exe'),
        (Join-Path $RepoRoot 'build-vulkan\src\Release\magpie-slicer.exe'),
        (Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'),
        (Join-Path ${env:ProgramFiles} 'Magpie Slicer\magpie-slicer.exe'),
        (Join-Path ${env:ProgramFiles} 'MagpieSlicer\magpie-slicer.exe'),
        (Join-Path $PSScriptRoot 'magpie-slicer.exe')
    )
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'Magpie CLI was not found. Pass -SlicerPath explicitly.'
}

function Find-Profile([string] $Name) {
    $roots = @(
        (Join-Path $RepoRoot 'resources\profiles'),
        (Join-Path (Split-Path -Parent $SlicerPath) 'resources\profiles')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter ($Name + '.json') | Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Profile not found: $Name"
}

function Set-JsonProperty($Object, [string] $Name, $Value) {
    if ($null -eq $Object.PSObject.Properties[$Name]) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    } else {
        $Object.$Name = $Value
    }
}

function Add-Triangle([Text.StringBuilder] $Builder, [double[]] $A, [double[]] $B, [double[]] $C) {
    [void] $Builder.AppendLine('  facet normal 0 0 0')
    [void] $Builder.AppendLine('    outer loop')
    foreach ($point in @($A, $B, $C)) {
        [void] $Builder.AppendLine(('      vertex {0} {1} {2}' -f
            $point[0].ToString('0.######', $Invariant),
            $point[1].ToString('0.######', $Invariant),
            $point[2].ToString('0.######', $Invariant)))
    }
    [void] $Builder.AppendLine('    endloop')
    [void] $Builder.AppendLine('  endfacet')
}

function Add-Box([Text.StringBuilder] $Builder, [double] $X0, [double] $Y0, [double] $Z0,
                 [double] $X1, [double] $Y1, [double] $Z1) {
    $p = @(
        @($X0,$Y0,$Z0), @($X1,$Y0,$Z0), @($X1,$Y1,$Z0), @($X0,$Y1,$Z0),
        @($X0,$Y0,$Z1), @($X1,$Y0,$Z1), @($X1,$Y1,$Z1), @($X0,$Y1,$Z1)
    )
    foreach ($face in @(
        @(0,2,1), @(0,3,2), @(4,5,6), @(4,6,7),
        @(0,1,5), @(0,5,4), @(1,2,6), @(1,6,5),
        @(2,3,7), @(2,7,6), @(3,0,4), @(3,4,7)
    )) {
        Add-Triangle $Builder $p[$face[0]] $p[$face[1]] $p[$face[2]]
    }
}

function New-SyntheticBenchmarkModel([string] $Path, [int] $GridSize, [double] $HeightMm) {
    $builder = [Text.StringBuilder]::new()
    [void] $builder.AppendLine('solid magpie_gpu_benchmark')
    $tower = 5.0
    $gap = 2.0
    $pitch = $tower + $gap
    $width = $GridSize * $tower + ($GridSize - 1) * $gap
    Add-Box $builder 0 0 0 $width $width 0.8
    for ($y = 0; $y -lt $GridSize; ++$y) {
        for ($x = 0; $x -lt $GridSize; ++$x) {
            $x0 = $x * $pitch
            $y0 = $y * $pitch
            Add-Box $builder $x0 $y0 0.8 ($x0 + $tower) ($y0 + $tower) $HeightMm
        }
    }
    [void] $builder.AppendLine('endsolid magpie_gpu_benchmark')
    [IO.File]::WriteAllText($Path, $builder.ToString(), [Text.UTF8Encoding]::new($false))
}

function Get-GcodeFingerprint([string] $Path) {
    $lines = 0
    $motion = 0
    $extrusion = 0
    $nonFinite = 0
    foreach ($raw in [IO.File]::ReadLines($Path)) {
        ++$lines
        $command = (($raw.TrimEnd()) -split ';', 2)[0].Trim()
        if ($command -match '^(?:G0|G1|G2|G3|G5|T\d+|M10[4679]|M106|M107)(?:\s|$)') { ++$motion }
        if ($command -match '^G[0123]\s' -and $command -match '(?:^|\s)E-?(?:\d|\.)') { ++$extrusion }
        if ($command -match '(?i)(?:^|[\s=,])(?:nan|[+-]?inf(?:inity)?)(?=$|[\s,])') { ++$nonFinite }
    }
    $geometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($Path, 0.10, $false)
    [pscustomobject]@{
        segment_sha256 = $geometry.SegmentSha256
        metrics_sha256 = $geometry.MetricsSha256
        positive_segments = $geometry.PositiveSegments
        total_length = $geometry.TotalLength
        total_extrusion = $geometry.TotalExtrusion
        bytes = (Get-Item -LiteralPath $Path).Length
        lines = $lines
        motion_commands = $motion
        extrusion_commands = $extrusion
        non_finite = $nonFinite
    }
}

function Get-Median([double[]] $Values) {
    if ($Values.Count -eq 0) { return [double]::NaN }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-Statistics([double[]] $Values) {
    $mean = ($Values | Measure-Object -Average).Average
    $variance = if ($Values.Count -gt 1) {
        (($Values | ForEach-Object { ($_ - $mean) * ($_ - $mean) } | Measure-Object -Sum).Sum) / ($Values.Count - 1)
    } else { 0.0 }
    [pscustomobject]@{
        count = $Values.Count
        median_seconds = [Math]::Round((Get-Median $Values), 4)
        mean_seconds = [Math]::Round([double]$mean, 4)
        minimum_seconds = [Math]::Round([double](($Values | Measure-Object -Minimum).Minimum), 4)
        maximum_seconds = [Math]::Round([double](($Values | Measure-Object -Maximum).Maximum), 4)
        standard_deviation_seconds = [Math]::Round([Math]::Sqrt($variance), 4)
    }
}

function Get-HardwareInventory {
    try { $cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed) }
    catch { $cpu = @([pscustomobject]@{ Name=$env:PROCESSOR_IDENTIFIER; NumberOfCores=$null; NumberOfLogicalProcessors=$env:NUMBER_OF_PROCESSORS; MaxClockSpeed=$null }) }
    try { $gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM,VideoProcessor) }
    catch { $gpu = @() }
    try { $memory = [Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 2) }
    catch { $memory = $null }
    $powerPlan = try { (& powercfg.exe /getactivescheme 2>$null) -join ' ' } catch { '' }
    [pscustomobject][ordered]@{
        computer = $env:COMPUTERNAME
        os = [Environment]::OSVersion.VersionString
        powershell = $PSVersionTable.PSVersion.ToString()
        cpu = $cpu
        gpu = $gpu
        memory_gib = $memory
        active_power_plan = $powerPlan.Trim()
    }
}

function Set-BenchmarkEnvironment([string] $Mode, [string] $DiagnosticsPath) {
    foreach ($name in @(
        'MAGPIE_VULKAN_SLICER_ENABLE', 'MAGPIE_VULKAN_SLICER_GPU_PRIORITY',
        'MAGPIE_VULKAN_SLICER_FORCE_DISPATCH', 'MAGPIE_VULKAN_SLICER_VALIDATION',
        'MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE', 'MAGPIE_VULKAN_SLICER_POLICY',
        'ORCA_VULKAN_SLICER_POLICY'
    )) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    if ($Mode -eq 'gpu') {
        $env:MAGPIE_VULKAN_SLICER_ENABLE = '1'
        $env:MAGPIE_VULKAN_SLICER_GPU_PRIORITY = '1'
        $env:MAGPIE_VULKAN_SLICER_FORCE_DISPATCH = '1'
        $env:MAGPIE_VULKAN_SLICER_POLICY = 'gpu'
        $env:MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE = $DiagnosticsPath
    }
}

function Invoke-Slice([string] $Mode, [string] $Model, [int] $Pair, [bool] $Warmup, [int] $Order) {
    $modelName = [IO.Path]::GetFileNameWithoutExtension($Model)
    $safeModel = $modelName -replace '[^A-Za-z0-9_.-]', '_'
    $id = '{0}-pair{1:D2}-{2}-order{3}' -f $(if ($Warmup) { 'warmup' } else { 'measured' }),$Pair,$Mode,$Order
    $caseRoot = Join-Path (Join-Path $runRoot $safeModel) $id
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $diagnostics = Join-Path $caseRoot 'vulkan-dispatch.csv'
    Set-BenchmarkEnvironment $Mode $diagnostics

    $settings = [char]34 + "$MachineSettings;$benchmarkProcessPath" + [char]34
    $arguments = @('--slice','0','--debug','1','--load-settings',$settings)
    if ($FilamentProfiles.Count -gt 0) {
        $loadedFilaments = [char]34 + ($FilamentProfiles -join ';') + [char]34
        $filamentMap = [char]34 + ((1..$FilamentProfiles.Count) -join ';') + [char]34
        $arguments += @('--load-filaments',$loadedFilaments,'--filament-map',$filamentMap)
    }
    $arguments += @('--outputdir',([char]34 + $caseRoot + [char]34),([char]34 + $Model + [char]34))
    $stdout = Join-Path $caseRoot 'stdout.log'
    $stderr = Join-Path $caseRoot 'stderr.log'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timedOut = -not $process.WaitForExit($SliceTimeoutSeconds * 1000)
    $exitCode = $null
    if ($timedOut) {
        & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
    } else {
        $process.WaitForExit()
        $process.Refresh()
        $exitCode = [int] $process.ExitCode
    }
    $timer.Stop()
    $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
    $fingerprint = if ($null -ne $gcode) { Get-GcodeFingerprint $gcode.FullName } else { $null }
    [array] $dispatchRows = @(if (Test-Path -LiteralPath $diagnostics -PathType Leaf) {
        Get-Content -LiteralPath $diagnostics | Where-Object { $_ -match '^[^,]+,\d+$' }
    })
    $dispatchRequests = 0L
    foreach ($row in $dispatchRows) { $dispatchRequests += [int64](($row -split ',',2)[1]) }
    $errors = [Collections.Generic.List[string]]::new()
    if ($timedOut) { $errors.Add("Timed out after $SliceTimeoutSeconds seconds") }
    if (-not $timedOut -and $exitCode -ne 0) { $errors.Add("Slicer exit code $exitCode") }
    if ($null -eq $gcode) { $errors.Add('G-code was not generated') }
    if ($null -ne $fingerprint -and ($fingerprint.motion_commands -eq 0 -or $fingerprint.extrusion_commands -eq 0)) { $errors.Add('Printable motion is empty') }
    if ($null -ne $fingerprint -and $fingerprint.non_finite -gt 0) { $errors.Add('G-code contains NaN or infinity') }

    [pscustomobject][ordered]@{
        model = $modelName
        model_path = $Model
        mode = $Mode
        pair = $Pair
        warmup = $Warmup
        execution_order = $Order
        seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 4)
        passed = $errors.Count -eq 0
        errors = @($errors)
        exit_code = $exitCode
        vulkan_dispatches = $dispatchRows.Count
        vulkan_requests = $dispatchRequests
        segment_sha256 = if ($fingerprint) { $fingerprint.segment_sha256 } else { '' }
        metrics_sha256 = if ($fingerprint) { $fingerprint.metrics_sha256 } else { '' }
        positive_segments = if ($fingerprint) { $fingerprint.positive_segments } else { 0 }
        gcode_bytes = if ($fingerprint) { $fingerprint.bytes } else { 0 }
        gcode_lines = if ($fingerprint) { $fingerprint.lines } else { 0 }
        gcode = if ($null -ne $gcode) { $gcode.FullName } else { '' }
        stdout = $stdout
        stderr = $stderr
    }
}

if ([string]::IsNullOrWhiteSpace($SlicerPath)) { $SlicerPath = Find-SlicerExecutable }
$SlicerPath = Resolve-ExistingFile $SlicerPath 'Magpie slicer executable'
if ([string]::IsNullOrWhiteSpace($MachineSettings)) {
    $machineCandidates = @(
        (Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'),
        (Join-Path $PSScriptRoot 'data\machine.json')
    )
    $MachineSettings = $machineCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($ProcessSettings)) {
    $processCandidates = @(
        (Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'),
        (Join-Path $PSScriptRoot 'data\process.json')
    )
    $ProcessSettings = $processCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
$MachineSettings = Resolve-ExistingFile $MachineSettings 'Machine settings'
$ProcessSettings = Resolve-ExistingFile $ProcessSettings 'Process settings'
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\gpu-slicing-benchmark'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

if ($FilamentProfiles.Count -eq 0) {
    $FilamentProfiles = @('Snapmaker PLA @U1','Snapmaker ABS @U1','Snapmaker PETG @U1','Snapmaker TPU @U1') |
        ForEach-Object { Find-Profile $_ }
} else {
    $FilamentProfiles = @($FilamentProfiles | ForEach-Object { Resolve-ExistingFile $_ 'Filament profile' })
}

$benchmarkProcess = Get-Content -LiteralPath $ProcessSettings -Raw | ConvertFrom-Json
foreach ($entry in ([ordered]@{
    name = 'Magpie GPU slicing benchmark'
    layer_height = '0.12'
    initial_layer_print_height = '0.20'
    wall_generator = 'classic'
    wall_loops = '3'
    top_shell_layers = '5'
    bottom_shell_layers = '5'
    sparse_infill_pattern = 'grid'
    sparse_infill_density = '55%'
    enable_support = '0'
    use_smaller_nozzles_in_crisp_corners = '0'
    single_nozzle_low_temperature_interface = '0'
}).GetEnumerator()) { Set-JsonProperty $benchmarkProcess $entry.Key $entry.Value }
$benchmarkProcessPath = Join-Path $runRoot 'benchmark-process.json'
$benchmarkProcess | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $benchmarkProcessPath -Encoding UTF8

if ($ModelPath.Count -eq 0) {
    $synthetic = Join-Path $runRoot 'magpie-gpu-synthetic.stl'
    New-SyntheticBenchmarkModel $synthetic $SyntheticGridSize $SyntheticHeightMm
    $ModelPath = @($synthetic)
} else {
    $ModelPath = @($ModelPath | ForEach-Object { Resolve-ExistingFile $_ 'Benchmark model' })
}

$environmentNames = @(
    'MAGPIE_VULKAN_SLICER_ENABLE', 'MAGPIE_VULKAN_SLICER_GPU_PRIORITY',
    'MAGPIE_VULKAN_SLICER_FORCE_DISPATCH', 'MAGPIE_VULKAN_SLICER_VALIDATION',
    'MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE', 'MAGPIE_VULKAN_SLICER_POLICY',
    'ORCA_VULKAN_SLICER_POLICY'
)
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }

$results = [Collections.Generic.List[object]]::new()
try {
    foreach ($model in $ModelPath) {
        $pairCount = $WarmupPairs + $Repeats
        for ($pair = 1; $pair -le $pairCount; ++$pair) {
            $warmup = $pair -le $WarmupPairs
            $modes = if (($pair % 2) -eq 1) { @('cpu','gpu') } else { @('gpu','cpu') }
            for ($order = 0; $order -lt $modes.Count; ++$order) {
                $result = Invoke-Slice $modes[$order] $model $pair $warmup ($order + 1)
                $results.Add($result)
                Write-Output ('PROGRESS model={0} pair={1}/{2} mode={3} seconds={4} dispatches={5} passed={6}' -f
                    $result.model,$pair,$pairCount,$result.mode,$result.seconds,$result.vulkan_dispatches,$result.passed)
            }
        }
    }
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}

$modelSummaries = [Collections.Generic.List[object]]::new()
foreach ($model in ($results.model | Sort-Object -Unique)) {
    $measured = @($results | Where-Object { $_.model -eq $model -and -not $_.warmup })
    $cpu = @($measured | Where-Object mode -eq 'cpu')
    $gpu = @($measured | Where-Object mode -eq 'gpu')
    $cpuStats = Get-Statistics @($cpu.seconds)
    $gpuStats = Get-Statistics @($gpu.seconds)
    $cpuHashes = @($cpu | Where-Object { $_.segment_sha256 } | ForEach-Object { $_.segment_sha256 + ':' + $_.metrics_sha256 } | Sort-Object -Unique)
    $gpuHashes = @($gpu | Where-Object { $_.segment_sha256 } | ForEach-Object { $_.segment_sha256 + ':' + $_.metrics_sha256 } | Sort-Object -Unique)
    $geometryMatch = $cpuHashes.Count -eq 1 -and $gpuHashes.Count -eq 1 -and $cpuHashes[0] -eq $gpuHashes[0]
    $dispatches = ($gpu.vulkan_dispatches | Measure-Object -Sum).Sum
    $requests = ($gpu.vulkan_requests | Measure-Object -Sum).Sum
    $speedup = if ($gpuStats.median_seconds -gt 0) { $cpuStats.median_seconds / $gpuStats.median_seconds } else { [double]::NaN }
    $modelSummaries.Add([pscustomobject][ordered]@{
        model = $model
        valid = @($measured | Where-Object { -not $_.passed }).Count -eq 0 -and $geometryMatch -and $dispatches -gt 0
        geometry_match = $geometryMatch
        vulkan_dispatches = [int64]$dispatches
        vulkan_requests = [int64]$requests
        cpu = $cpuStats
        gpu = $gpuStats
        speedup = [Math]::Round($speedup, 4)
        percent_faster = [Math]::Round(($speedup - 1.0) * 100.0, 2)
    })
}

$measuredResults = @($results | Where-Object { -not $_.warmup })
$overallCpu = [double](($modelSummaries | ForEach-Object { $_.cpu.median_seconds } | Measure-Object -Sum).Sum)
$overallGpu = [double](($modelSummaries | ForEach-Object { $_.gpu.median_seconds } | Measure-Object -Sum).Sum)
$overallSpeedup = if ($overallGpu -gt 0) { $overallCpu / $overallGpu } else { [double]::NaN }
$failedRuns = @($results | Where-Object { -not $_.passed })
$valid = $failedRuns.Count -eq 0 -and @($modelSummaries | Where-Object { -not $_.valid }).Count -eq 0
$summary = [pscustomobject][ordered]@{
    schema = 1
    kind = 'magpie-high-performance-gpu-slicing-benchmark'
    generated_at = (Get-Date).ToString('o')
    valid = $valid
    slicer = [ordered]@{
        path = $SlicerPath
        bytes = (Get-Item -LiteralPath $SlicerPath).Length
        sha256 = (Get-FileHash -LiteralPath $SlicerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    hardware = Get-HardwareInventory
    configuration = [ordered]@{
        repeats = $Repeats
        warmup_pairs = $WarmupPairs
        timeout_seconds = $SliceTimeoutSeconds
        machine_settings = $MachineSettings
        process_settings = $benchmarkProcessPath
        filaments = @($FilamentProfiles)
        models = @($ModelPath)
        gpu_mode = 'forced Vulkan dispatch, GPU-priority, production validation'
        cpu_mode = 'Vulkan disabled'
        execution_order = 'alternating CPU/GPU per pair'
    }
    overall = [ordered]@{
        cpu_total_seconds = [Math]::Round($overallCpu, 4)
        gpu_total_seconds = [Math]::Round($overallGpu, 4)
        speedup = [Math]::Round($overallSpeedup, 4)
        percent_faster = [Math]::Round(($overallSpeedup - 1.0) * 100.0, 2)
    }
    models = @($modelSummaries)
    failed_runs = $failedRuns.Count
    results = @($results)
}

$jsonPath = Join-Path $runRoot 'benchmark-summary.json'
$csvPath = Join-Path $runRoot 'benchmark-runs.csv'
$htmlPath = Join-Path $runRoot 'benchmark-report.html'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$results | Select-Object model,mode,pair,warmup,execution_order,seconds,passed,vulkan_dispatches,vulkan_requests,segment_sha256,metrics_sha256,gcode_bytes,gcode_lines |
    Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8

$encode = { param($value) [Net.WebUtility]::HtmlEncode([string]$value) }
$modelRows = @($modelSummaries | ForEach-Object {
    '<tr><td>{0}</td><td>{1}</td><td>{2:N3}</td><td>{3:N3}</td><td>{4:N2}x</td><td>{5:N1}%</td><td>{6}</td><td>{7}</td></tr>' -f
        (& $encode $_.model),$_.valid,$_.cpu.median_seconds,$_.gpu.median_seconds,$_.speedup,$_.percent_faster,$_.vulkan_dispatches,$_.geometry_match
}) -join "`n"
$runRows = @($results | ForEach-Object {
    '<tr><td>{0}</td><td>{1}</td><td>{2}</td><td>{3}</td><td>{4:N3}</td><td>{5}</td><td>{6}</td><td>{7}</td></tr>' -f
        (& $encode $_.model),$_.mode,$_.pair,$_.warmup,$_.seconds,$_.passed,$_.vulkan_dispatches,$_.vulkan_requests
}) -join "`n"
$cpuName = if ($summary.hardware.cpu.Count -gt 0) { $summary.hardware.cpu[0].Name } else { 'Unknown' }
$gpuNames = @($summary.hardware.gpu | ForEach-Object Name) -join ', '
$statusClass = if ($valid) { 'pass' } else { 'fail' }
$statusText = if ($valid) { 'VALID' } else { 'INVALID' }
$html = @"
<!doctype html><html><head><meta charset="utf-8"><title>Magpie GPU slicing benchmark</title>
<style>body{font-family:Segoe UI,Arial,sans-serif;margin:32px;background:#10151d;color:#e8eef7}h1,h2{color:#3d90e8}table{border-collapse:collapse;width:100%;margin:16px 0}th,td{padding:8px 10px;border:1px solid #344456;text-align:right}th:first-child,td:first-child{text-align:left}.metric{display:inline-block;margin:8px 20px 8px 0;font-size:20px}.pass{color:#70d6a3}.fail{color:#ff7b7b}.muted{color:#9fb0c4}code{color:#9fc8ff}</style></head><body>
<h1>Magpie GPU slicing benchmark</h1><p class="$statusClass"><strong>$statusText</strong></p>
<div class="metric">Overall speedup: <strong>$($summary.overall.speedup)x</strong></div>
<div class="metric">GPU difference: <strong>$($summary.overall.percent_faster)%</strong></div>
<p class="muted">CPU: $(& $encode $cpuName)<br>GPU: $(& $encode $gpuNames)<br>Slicer: <code>$(& $encode $SlicerPath)</code></p>
<h2>Models</h2><table><thead><tr><th>Model</th><th>Valid</th><th>CPU median (s)</th><th>GPU median (s)</th><th>Speedup</th><th>Faster</th><th>Dispatches</th><th>G-code match</th></tr></thead><tbody>$modelRows</tbody></table>
<h2>Runs</h2><table><thead><tr><th>Model</th><th>Mode</th><th>Pair</th><th>Warmup</th><th>Seconds</th><th>Passed</th><th>Dispatches</th><th>Requests</th></tr></thead><tbody>$runRows</tbody></table>
<p class="muted">A valid result requires successful slices, at least one real Vulkan dispatch, and identical CPU/GPU motion G-code hashes.</p>
</body></html>
"@
[IO.File]::WriteAllText($htmlPath, $html, [Text.UTF8Encoding]::new($false))

Write-Output "RESULT_VALID=$valid"
Write-Output "CPU_TOTAL_SECONDS=$($summary.overall.cpu_total_seconds)"
Write-Output "GPU_TOTAL_SECONDS=$($summary.overall.gpu_total_seconds)"
Write-Output "SPEEDUP=$($summary.overall.speedup)"
Write-Output "PERCENT_FASTER=$($summary.overall.percent_faster)"
Write-Output "REPORT=$htmlPath"
Write-Output "JSON=$jsonPath"
if ($OpenReport) { Start-Process $htmlPath }
if (-not $valid) { exit 2 }
