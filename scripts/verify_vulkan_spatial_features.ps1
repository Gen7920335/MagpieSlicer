param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $SlicerPath = "",
    [ValidateRange(60, 3600)]
    [int] $SliceTimeoutSeconds = 900,
    [string] $OutputRoot = "",
    [string] $ModelPath = "",
    [ValidateRange(-1, 90)]
    [int] $SupportAngleOverride = -1,
    [string[]] $CaseName = @(),
    [switch] $IncludeAuto,
    [switch] $IncludeAggressive,
    [switch] $IncludeMaximum
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)

if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build-vulkan\src\Release\magpie-slicer.exe'
}
$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
if (-not (Test-Path -LiteralPath $SlicerPath -PathType Leaf)) {
    throw "Magpie slicer not found: $SlicerPath"
}

$machinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$baseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
$analyzerSource = Join-Path $PSScriptRoot 'lib\GcodeGeometryAnalyzer.cs'
foreach ($path in @($machinePath, $baseProcessPath, $analyzerSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required input not found: $path" }
}
Add-Type -Path $analyzerSource

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot "build\verification\vulkan-spatial-$stamp"
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

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

function New-WavyOverhangModel([string] $Path) {
    $segments = 256
    $lowerBottom = [Collections.Generic.List[double[]]]::new()
    $lowerTop = [Collections.Generic.List[double[]]]::new()
    $upperBottom = [Collections.Generic.List[double[]]]::new()
    $upperTop = [Collections.Generic.List[double[]]]::new()
    for ($i = 0; $i -lt $segments; ++$i) {
        $angle = 2.0 * [Math]::PI * $i / $segments
        $lowerRadius = 27.0 + 1.5 * [Math]::Sin(7.0 * $angle)
        $upperRadius = 51.0 + 4.0 * [Math]::Sin(11.0 * $angle) + 1.5 * [Math]::Sin(23.0 * $angle)
        $lowerX = [double]($lowerRadius * [Math]::Cos($angle))
        $lowerY = [double]($lowerRadius * [Math]::Sin($angle))
        $upperX = [double]($upperRadius * [Math]::Cos($angle))
        $upperY = [double]($upperRadius * [Math]::Sin($angle))
        $lowerBottom.Add([double[]]@($lowerX, $lowerY, 0.0))
        $lowerTop.Add([double[]]@($lowerX, $lowerY, 10.0))
        $upperBottom.Add([double[]]@($upperX, $upperY, 10.0))
        $upperTop.Add([double[]]@($upperX, $upperY, 34.0))
    }

    $builder = [Text.StringBuilder]::new()
    [void] $builder.AppendLine('solid magpie_vulkan_spatial_verification')
    $bottomCenter = @(0.0, 0.0, 0.0)
    $topCenter = @(0.0, 0.0, 34.0)
    for ($i = 0; $i -lt $segments; ++$i) {
        $next = ($i + 1) % $segments
        Add-Triangle $builder $bottomCenter $lowerBottom[$next] $lowerBottom[$i]
        Add-Triangle $builder $lowerBottom[$i] $lowerBottom[$next] $lowerTop[$next]
        Add-Triangle $builder $lowerBottom[$i] $lowerTop[$next] $lowerTop[$i]
        Add-Triangle $builder $lowerTop[$i] $upperBottom[$next] $upperBottom[$i]
        Add-Triangle $builder $lowerTop[$i] $lowerTop[$next] $upperBottom[$next]
        Add-Triangle $builder $upperBottom[$i] $upperBottom[$next] $upperTop[$next]
        Add-Triangle $builder $upperBottom[$i] $upperTop[$next] $upperTop[$i]
        Add-Triangle $builder $topCenter $upperTop[$i] $upperTop[$next]
    }
    [void] $builder.AppendLine('endsolid magpie_vulkan_spatial_verification')
    [IO.File]::WriteAllText($Path, $builder.ToString(), [Text.UTF8Encoding]::new($false))
}

function Set-VulkanEnvironment([string] $Mode, [string] $DiagnosticsPath) {
    foreach ($name in @(
        'MAGPIE_VULKAN_SLICER_ENABLE', 'MAGPIE_VULKAN_SLICER_GPU_PRIORITY', 'MAGPIE_VULKAN_SLICER_MAXIMUM',
        'MAGPIE_VULKAN_SLICER_FORCE_DISPATCH', 'MAGPIE_VULKAN_SLICER_VALIDATION',
        'MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE', 'MAGPIE_VULKAN_SLICER_POLICY',
        'ORCA_VULKAN_SLICER_POLICY'
    )) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    if ($Mode -eq 'gpu' -or $Mode -eq 'gpu-auto' -or $Mode -eq 'gpu-aggressive' -or $Mode -eq 'gpu-max') {
        $env:MAGPIE_VULKAN_SLICER_ENABLE = '1'
        $env:MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE = $DiagnosticsPath
        if ($Mode -eq 'gpu') {
            $env:MAGPIE_VULKAN_SLICER_GPU_PRIORITY = '1'
            $env:MAGPIE_VULKAN_SLICER_POLICY = 'gpu'
            $env:MAGPIE_VULKAN_SLICER_FORCE_DISPATCH = '1'
            $env:MAGPIE_VULKAN_SLICER_VALIDATION = 'strict'
        } elseif ($Mode -eq 'gpu-auto') {
            $env:MAGPIE_VULKAN_SLICER_POLICY = 'auto'
        } elseif ($Mode -eq 'gpu-max') {
            $env:MAGPIE_VULKAN_SLICER_MAXIMUM = '1'
            $env:MAGPIE_VULKAN_SLICER_POLICY = 'max'
        } else {
            $env:MAGPIE_VULKAN_SLICER_GPU_PRIORITY = '1'
            $env:MAGPIE_VULKAN_SLICER_POLICY = 'gpu'
            $env:MAGPIE_VULKAN_SLICER_VALIDATION = 'sampled'
        }
    }
}

function Get-CanonicalHash([string] $Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($raw in [IO.File]::ReadLines($Path)) {
            if ($raw -match '^; generated by .* on ') { continue }
            if ($raw -match '^M73(?:\s|$)') { continue }
            $normalized = $raw -replace ' id:\d+', ' id:<normalized>'
            $bytes = [Text.Encoding]::UTF8.GetBytes($normalized.TrimEnd() + "`n")
            [void] $sha.TransformBlock($bytes, 0, $bytes.Length, $null, 0)
        }
        [void] $sha.TransformFinalBlock([byte[]]::new(0), 0, 0)
        return ([BitConverter]::ToString($sha.Hash)).Replace('-', '')
    } finally { $sha.Dispose() }
}

function Invoke-SliceCase($Case, [string] $Mode, [string] $ModelPath) {
    $caseRoot = Join-Path $OutputRoot (Join-Path $Case.name $Mode)
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $diagnostics = Join-Path $caseRoot 'vulkan-dispatch.csv'
    Set-VulkanEnvironment $Mode $diagnostics

    $settings = [char]34 + "$machinePath;$($Case.process_path)" + [char]34
    $arguments = @('--slice','0','--debug','1','--load-settings',$settings,
        '--outputdir',([char]34 + $caseRoot + [char]34),([char]34 + $ModelPath + [char]34))
    $stdout = Join-Path $caseRoot 'stdout.log'
    $stderr = Join-Path $caseRoot 'stderr.log'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
    $reused = $false
    if ($null -ne $gcode -and [IO.File]::ReadAllText($gcode.FullName).Contains('CONFIG_BLOCK_END')) {
        $reused = $true
    } else {
        $process = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $timedOut = -not $process.WaitForExit($SliceTimeoutSeconds * 1000)
        if ($timedOut) {
            & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
            throw "Slice timed out: $($Case.name)/$Mode"
        }
        $process.WaitForExit()
        $process.Refresh()
        $exitCode = $null
        try { $exitCode = $process.ExitCode } catch { $exitCode = $null }
        if ($null -ne $exitCode -and [int]$exitCode -ne 0) {
            throw "Slice failed: $($Case.name)/$Mode exit=$exitCode; stderr=$stderr"
        }
        $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
    }
    $timer.Stop()
    if ($null -eq $gcode) { throw "G-code missing: $($Case.name)/$Mode" }
    $tail = [IO.File]::ReadAllText($gcode.FullName)
    if (-not $tail.Contains('CONFIG_BLOCK_END')) { throw "Incomplete G-code: $($Case.name)/$Mode" }
    if ($tail -match '(?i)(?:^|[\s=,])(?:nan|[+-]?inf(?:inity)?)(?=$|[\s,])') {
        throw "Non-finite value in G-code: $($Case.name)/$Mode"
    }
    $geometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($gcode.FullName, 0.05, $true)
    $coarseGeometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($gcode.FullName, 0.2, $true)
    Start-Sleep -Milliseconds 500
    [pscustomobject]@{
        case = $Case.name
        mode = $Mode
        reused = $reused
        seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
        gcode = $gcode.FullName
        canonical_sha256 = Get-CanonicalHash $gcode.FullName
        segment_sha256 = $geometry.SegmentSha256
        coverage_sha256 = $geometry.CoverageSha256
        metrics_sha256 = $geometry.MetricsSha256
        positive_segments = $geometry.PositiveSegments
        coverage_cells = $geometry.CoverageCells
        coarse_coverage_cells = $coarseGeometry.CoverageCells
        total_length = $geometry.TotalLength
        total_extrusion = $geometry.TotalExtrusion
        layer_count = $geometry.LayerCount
        tool_ids = $geometry.ToolIds
        min_x = $geometry.MinX
        min_y = $geometry.MinY
        max_x = $geometry.MaxX
        max_y = $geometry.MaxY
        geometry = $geometry
        coarse_geometry = $coarseGeometry
        diagnostics = if (Test-Path -LiteralPath $diagnostics) { [IO.File]::ReadAllText($diagnostics) } else { '' }
    }
}

$modelPath = if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $generatedPath = Join-Path $OutputRoot 'wavy-overhang.stl'
    New-WavyOverhangModel $generatedPath
    $generatedPath
} else {
    [IO.Path]::GetFullPath($ModelPath)
}

function Clear-GeometryDetails($Result) {
    # Coverage grids are intentionally very large for the 0.05 mm analyzer.
    # They are needed only while comparing modes for the current case; keeping
    # them in the run-wide results list makes memory grow once per slice.
    $Result.geometry = $null
    $Result.coarse_geometry = $null
}
if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) { throw "Model not found: $modelPath" }
$base = Get-Content -LiteralPath $baseProcessPath -Raw | ConvertFrom-Json
$caseDefinitions = @(
    [pscustomobject]@{ name='gyroid_low_classic'; expected=''; forbidden='gyroid-spatial'; maximum_expected='gyroid-spatial'; comparison='exact'; values=@{
        enable_support='0'; sparse_infill_pattern='gyroid'; sparse_infill_density='12%'; wall_generator='classic'; wall_loops='2' } },
    [pscustomobject]@{ name='gyroid_high_arachne'; expected='arachne-wall-spatial'; forbidden=''; comparison='exact'; values=@{
        enable_support='0'; sparse_infill_pattern='gyroid'; sparse_infill_density='45%'; wall_generator='arachne'; wall_loops='4' } },
    [pscustomobject]@{ name='lightning_overhang'; expected=''; forbidden=''; maximum_expected='distance-spatial'; comparison='tolerant'; values=@{
        enable_support='0'; sparse_infill_pattern='lightning'; sparse_infill_density='18%'; wall_generator='classic'; wall_loops='3' } },
    [pscustomobject]@{ name='tree_90_dense'; expected=''; forbidden='vertical'; comparison='tolerant'; values=@{
        enable_support='1'; support_type='tree(auto)'; support_style='tree_slim'; support_threshold_angle='90';
        support_interface_top_layers='0'; support_interface_bottom_layers='0';
        sparse_infill_pattern='gyroid'; sparse_infill_density='20%'; wall_generator='arachne'; wall_loops='3' } },
    [pscustomobject]@{ name='tree_90_spatial_only'; expected=''; forbidden='vertical'; comparison='tolerant'; values=@{
        enable_support='1'; support_type='tree(auto)'; support_style='tree_slim'; support_threshold_angle='90';
        support_interface_top_layers='0'; support_interface_bottom_layers='0';
        sparse_infill_pattern='rectilinear'; sparse_infill_density='20%'; wall_generator='classic'; wall_loops='3' } },
    [pscustomobject]@{ name='classic_extra_min'; expected='seam-travel-spatial'; forbidden='';
        aggressive_expected=''; aggressive_forbidden='seam-travel-spatial'; comparison='exact'; values=@{
        enable_support='0'; sparse_infill_pattern='rectilinear'; sparse_infill_density='8%'; wall_generator='classic';
        wall_loops='2'; detect_overhang_wall='1'; extra_perimeters_on_overhangs='1' } },
    [pscustomobject]@{ name='classic_extra_max'; expected='classic-wall-spatial'; forbidden=''; comparison='exact'; values=@{
        enable_support='0'; sparse_infill_pattern='rectilinear'; sparse_infill_density='35%'; wall_generator='classic';
        wall_loops='8'; detect_overhang_wall='1'; extra_perimeters_on_overhangs='1' } },
    [pscustomobject]@{ name='cura_70_grid'; expected=''; forbidden='cura-support-spatial'; comparison='exact'; values=@{
        enable_support='1'; support_type='normal_cura(auto)'; support_style='grid'; support_threshold_angle='70';
        support_interface_top_layers='0'; support_interface_bottom_layers='0'; sparse_infill_pattern='rectilinear';
        sparse_infill_density='10%'; wall_generator='classic'; wall_loops='1' } },
    [pscustomobject]@{ name='cura_90_snug'; expected=''; forbidden='cura-support-spatial'; comparison='exact'; values=@{
        enable_support='1'; support_type='normal_cura(auto)'; support_style='snug'; support_threshold_angle='90';
        support_interface_top_layers='4'; support_interface_bottom_layers='2'; sparse_infill_pattern='gyroid';
        sparse_infill_density='30%'; wall_generator='arachne'; wall_loops='6' } }
)
if ($CaseName.Count -gt 0) {
    $caseDefinitions = @($caseDefinitions | Where-Object { $CaseName -contains $_.name })
    if ($caseDefinitions.Count -eq 0) { throw "No Vulkan verification case matched: $($CaseName -join ', ')" }
}

$cases = [Collections.Generic.List[object]]::new()
foreach ($definition in $caseDefinitions) {
    $process = $base | ConvertTo-Json -Depth 50 | ConvertFrom-Json
    foreach ($entry in $definition.values.GetEnumerator()) { Set-JsonProperty $process $entry.Key $entry.Value }
    if ($SupportAngleOverride -ge 0 -and $definition.values.enable_support -eq '1') {
        Set-JsonProperty $process 'support_threshold_angle' ([string] $SupportAngleOverride)
    }
    Set-JsonProperty $process 'name' "Magpie Vulkan verification $($definition.name)"
    $processPath = Join-Path $OutputRoot "$($definition.name)-process.json"
    $process | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath $processPath -Encoding utf8
    $aggressiveExpected = if ($definition.PSObject.Properties['aggressive_expected']) { $definition.aggressive_expected } else { $definition.expected }
    $aggressiveForbidden = if ($definition.PSObject.Properties['aggressive_forbidden']) { $definition.aggressive_forbidden } else { $definition.forbidden }
    $maximumExpected = if ($definition.PSObject.Properties['maximum_expected']) { $definition.maximum_expected } else { $definition.expected }
    $cases.Add([pscustomobject]@{ name=$definition.name; expected=$definition.expected; forbidden=$definition.forbidden;
        aggressive_expected=$aggressiveExpected; aggressive_forbidden=$aggressiveForbidden;
        maximum_expected=$maximumExpected;
        comparison=$definition.comparison; process_path=$processPath })
}

$results = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[string]]::new()
foreach ($case in $cases) {
    $cpu = Invoke-SliceCase $case 'cpu' $modelPath
    $gpu = Invoke-SliceCase $case 'gpu' $modelPath
    $results.Add($cpu)
    $results.Add($gpu)
    if ($case.comparison -eq 'exact') {
        if ($cpu.canonical_sha256 -ne $gpu.canonical_sha256) {
            $failures.Add("$($case.name): canonical G-code hash mismatch")
        }
        foreach ($property in @('segment_sha256','coverage_sha256','metrics_sha256','positive_segments')) {
            if ($cpu.$property -ne $gpu.$property) { $failures.Add("$($case.name): $property mismatch") }
        }
    } else {
        foreach ($property in @('positive_segments','coverage_cells','total_length','total_extrusion')) {
            $cpuValue = [double]$cpu.$property
            $gpuValue = [double]$gpu.$property
            $relative = [Math]::Abs($cpuValue - $gpuValue) / [Math]::Max(1.0, [Math]::Abs($cpuValue))
            if ($relative -gt 0.001) { $failures.Add("$($case.name): $property differs by more than 0.1%") }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($case.expected) -and
        $gpu.diagnostics -notmatch "(?m)^$([regex]::Escape($case.expected)),\d+\r?$") {
        $failures.Add("$($case.name): expected Vulkan dispatch '$($case.expected)' was not recorded")
    }
    if (-not [string]::IsNullOrWhiteSpace($case.forbidden) -and
        $gpu.diagnostics -match "(?m)^$([regex]::Escape($case.forbidden)),\d+\r?$") {
        $failures.Add("$($case.name): small workload unexpectedly dispatched '$($case.forbidden)'")
    }
    if ($IncludeAuto) {
        $auto = Invoke-SliceCase $case 'gpu-auto' $modelPath
        $results.Add($auto)
        if ($case.comparison -eq 'exact') {
            foreach ($property in @('canonical_sha256','segment_sha256','coverage_sha256','metrics_sha256','positive_segments')) {
                if ($cpu.$property -ne $auto.$property) { $failures.Add("$($case.name): Auto $property mismatch") }
            }
        } else {
            foreach ($property in @('positive_segments','coverage_cells','total_length','total_extrusion')) {
                $cpuValue = [double]$cpu.$property
                $autoValue = [double]$auto.$property
                $relative = [Math]::Abs($cpuValue - $autoValue) / [Math]::Max(1.0, [Math]::Abs($cpuValue))
                if ($relative -gt 0.001) { $failures.Add("$($case.name): Auto $property differs by more than 0.1%") }
            }
        }
    }
    if ($IncludeAggressive) {
        $aggressive = Invoke-SliceCase $case 'gpu-aggressive' $modelPath
        $results.Add($aggressive)
        foreach ($property in @('positive_segments','coverage_cells','total_length','total_extrusion')) {
            $cpuValue = [double]$cpu.$property
            $gpuValue = [double]$aggressive.$property
            $relative = [Math]::Abs($cpuValue - $gpuValue) / [Math]::Max(1.0, [Math]::Abs($cpuValue))
            if ($relative -gt 0.001) { $failures.Add("$($case.name): aggressive $property differs by more than 0.1%") }
        }
        $fineCoverageDifference = $cpu.geometry.CoverageDifferenceRatio($aggressive.geometry)
        if ([double]::IsNaN($fineCoverageDifference) -or [double]::IsInfinity($fineCoverageDifference) -or $fineCoverageDifference -gt 0.005) {
            $failures.Add("$($case.name): aggressive 0.05 mm coverage differs by more than 0.5%")
        }
        $coarseCoverageDifference = $cpu.coarse_geometry.CoverageDifferenceRatio($aggressive.coarse_geometry)
        if ([double]::IsNaN($coarseCoverageDifference) -or [double]::IsInfinity($coarseCoverageDifference) -or $coarseCoverageDifference -gt 0.001) {
            $failures.Add("$($case.name): aggressive 0.2 mm coverage differs by more than 0.1%")
        }
        foreach ($property in @('layer_count','tool_ids')) {
            if ($cpu.$property -ne $aggressive.$property) { $failures.Add("$($case.name): aggressive $property mismatch") }
        }
        foreach ($property in @('min_x','min_y','max_x','max_y')) {
            if ([Math]::Abs([double]$cpu.$property - [double]$aggressive.$property) -gt 0.05) {
                $failures.Add("$($case.name): aggressive $property differs by more than 0.05 mm")
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($case.aggressive_expected) -and
            $aggressive.diagnostics -notmatch "(?m)^$([regex]::Escape($case.aggressive_expected)),\d+\r?$") {
            $failures.Add("$($case.name): aggressive Vulkan dispatch '$($case.aggressive_expected)' was not recorded")
        }
        if (-not [string]::IsNullOrWhiteSpace($case.aggressive_forbidden) -and
            $aggressive.diagnostics -match "(?m)^$([regex]::Escape($case.aggressive_forbidden)),\d+\r?$") {
            $failures.Add("$($case.name): tiny aggressive workload unexpectedly dispatched '$($case.aggressive_forbidden)'")
        }
    }
    if ($IncludeMaximum) {
        $maximum = Invoke-SliceCase $case 'gpu-max' $modelPath
        $results.Add($maximum)
        foreach ($property in @('positive_segments','coverage_cells','total_length','total_extrusion')) {
            $cpuValue = [double]$cpu.$property
            $gpuValue = [double]$maximum.$property
            $relative = [Math]::Abs($cpuValue - $gpuValue) / [Math]::Max(1.0, [Math]::Abs($cpuValue))
            if ($relative -gt 0.001) { $failures.Add("$($case.name): maximum GPU $property differs by more than 0.1%") }
        }
        $fineCoverageDifference = $cpu.geometry.CoverageDifferenceRatio($maximum.geometry)
        if ([double]::IsNaN($fineCoverageDifference) -or [double]::IsInfinity($fineCoverageDifference) -or $fineCoverageDifference -gt 0.005) {
            $failures.Add("$($case.name): maximum GPU 0.05 mm coverage differs by more than 0.5%")
        }
        $coarseCoverageDifference = $cpu.coarse_geometry.CoverageDifferenceRatio($maximum.coarse_geometry)
        if ([double]::IsNaN($coarseCoverageDifference) -or [double]::IsInfinity($coarseCoverageDifference) -or $coarseCoverageDifference -gt 0.001) {
            $failures.Add("$($case.name): maximum GPU 0.2 mm coverage differs by more than 0.1%")
        }
        foreach ($property in @('layer_count','tool_ids')) {
            if ($cpu.$property -ne $maximum.$property) { $failures.Add("$($case.name): maximum GPU $property mismatch") }
        }
        foreach ($property in @('min_x','min_y','max_x','max_y')) {
            if ([Math]::Abs([double]$cpu.$property - [double]$maximum.$property) -gt 0.05) {
                $failures.Add("$($case.name): maximum GPU $property differs by more than 0.05 mm")
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($case.maximum_expected) -and
            $maximum.diagnostics -notmatch "(?m)^$([regex]::Escape($case.maximum_expected)),\d+\r?$") {
            $failures.Add("$($case.name): maximum GPU dispatch '$($case.maximum_expected)' was not recorded")
        }
    }

    Clear-GeometryDetails $cpu
    Clear-GeometryDetails $gpu
    if ($IncludeAuto) { Clear-GeometryDetails $auto }
    if ($IncludeAggressive) { Clear-GeometryDetails $aggressive }
    if ($IncludeMaximum) { Clear-GeometryDetails $maximum }
}

$summary = [pscustomobject][ordered]@{
    generated_at = (Get-Date).ToString('o')
    slicer = $SlicerPath
    slicer_sha256 = (Get-FileHash -LiteralPath $SlicerPath -Algorithm SHA256).Hash
    model = $modelPath
    case_count = $cases.Count
    slice_count = $results.Count
    failures = @($failures)
    aggressive = [bool]$IncludeAggressive
    maximum = [bool]$IncludeMaximum
    auto = [bool]$IncludeAuto
    results = @($results | Select-Object -Property * -ExcludeProperty geometry,coarse_geometry)
}
$summaryPath = Join-Path $OutputRoot 'summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Output "VULKAN_SPATIAL_CASES=$($cases.Count)"
Write-Output "VULKAN_SPATIAL_SLICES=$($results.Count)"
Write-Output "VULKAN_SPATIAL_FAILURES=$($failures.Count)"
Write-Output "VULKAN_SPATIAL_SUMMARY=$summaryPath"
$failures | ForEach-Object { Write-Output "ERROR=$_" }
if ($failures.Count -gt 0) { exit 2 }
