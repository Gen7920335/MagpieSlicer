param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [string] $ModelPath,
    [ValidateSet('classic', 'arachne')]
    [string] $WallGenerator = 'classic',
    [int] $SliceTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\orca-slicer-trinterface.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\small-nozzle-wall-speed'
}
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $RepoRoot 'resources\handy_models\OrcaCube_v2.drc'
}

$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$ModelPath = [IO.Path]::GetFullPath($ModelPath)
$MachineTemplate = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$ProcessTemplate = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'

foreach ($path in @($SlicerPath, $ModelPath, $MachineTemplate, $ProcessTemplate)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

function Find-FilamentProfile([string] $name) {
    $roots = @(
        (Join-Path $RepoRoot 'resources\profiles'),
        (Join-Path $env:APPDATA 'OrcaSlicerTrInterface\system')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$name.json" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $name"
}

function Set-JsonProperty($object, [string] $property, $value) {
    if ($null -eq $object.PSObject.Properties[$property]) {
        $object | Add-Member -NotePropertyName $property -NotePropertyValue $value
    } else {
        $object.$property = $value
    }
}

function Get-Median([Collections.Generic.List[double]] $values) {
    if ($values.Count -eq 0) { return 0.0 }
    $ordered = @($values | Sort-Object)
    $middle = [int] [Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { return [double] $ordered[$middle] }
    return ([double] $ordered[$middle - 1] + [double] $ordered[$middle]) / 2.0
}

function Measure-DetailWallSpeeds([string] $GcodePath, [int] $DetailTool) {
    $layer = -1
    $tool = -1
    $role = ''
    $feedrate = 0.0
    $relativeExtrusion = $false
    $extrusionPosition = 0.0
    $outer = [Collections.Generic.List[double]]::new()
    $inner = [Collections.Generic.List[double]]::new()
    $embeddedSetting = $null

    foreach ($line in [IO.File]::ReadLines($GcodePath)) {
        if ($line -eq ';LAYER_CHANGE') { ++$layer; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -match '^T(\d+)(?:\s|$)') { $tool = [int] $Matches[1]; continue }
        if ($line -match '^M82(?:\s|$)') { $relativeExtrusion = $false; continue }
        if ($line -match '^M83(?:\s|$)') { $relativeExtrusion = $true; continue }
        if ($line -match '^; crisp_corner_small_nozzle_wall_speed = (.+)$') {
            $embeddedSetting = $Matches[1].Trim()
            continue
        }
        if ($line -match '^G92(?:\s|$)' -and $line -match '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') {
            $extrusionPosition = [double]::Parse($Matches[1], $Invariant)
            continue
        }
        if ($line -notmatch '^G[123](?:\s|$)') { continue }
        if ($line -match '(?:^|\s)F(-?(?:\d+(?:\.\d*)?|\.\d+))') {
            $feedrate = [double]::Parse($Matches[1], $Invariant)
        }
        if ($line -notmatch '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') { continue }
        $nextExtrusion = [double]::Parse($Matches[1], $Invariant)
        $extruded = if ($relativeExtrusion) { $nextExtrusion } else { $nextExtrusion - $extrusionPosition }
        if (-not $relativeExtrusion) { $extrusionPosition = $nextExtrusion }
        if ($extruded -le 0.0000001 -or $layer -le 2 -or $tool -ne $DetailTool) { continue }

        $speed = $feedrate / 60.0
        if ($role -eq 'Outer wall') {
            $outer.Add($speed)
        } elseif ($role -eq 'Inner wall') {
            $inner.Add($speed)
        }
    }

    return [pscustomobject]@{
        OuterMedian = Get-Median $outer
        InnerMedian = Get-Median $inner
        OuterSamples = $outer.Count
        InnerSamples = $inner.Count
        EmbeddedSetting = $embeddedSetting
    }
}

$filamentProfileSources = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)
$cases = @(
    [pscustomobject]@{ Name='inherit'; Value='0'; ExpectedOuter=80.0; ExpectedInner=120.0 },
    [pscustomobject]@{ Name='absolute_20'; Value='20'; ExpectedOuter=20.0; ExpectedInner=20.0 },
    [pscustomobject]@{ Name='percent_50'; Value='50%'; ExpectedOuter=40.0; ExpectedInner=60.0 },
    [pscustomobject]@{ Name='absolute_300'; Value='300'; ExpectedOuter=300.0; ExpectedInner=300.0 }
)

$runRoot = Join-Path $OutputRoot "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$WallGenerator"
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$filamentProfiles = @(
    for ($index = 0; $index -lt $filamentProfileSources.Count; ++$index) {
        $profile = Get-Content -Raw -LiteralPath $filamentProfileSources[$index] | ConvertFrom-Json
        $profile.name = "Codex speed verification filament $index"
        Set-JsonProperty $profile 'setting_id' "speed-verification-filament-$index"
        Set-JsonProperty $profile 'filament_max_volumetric_speed' @('100')
        Set-JsonProperty $profile 'slow_down_for_layer_cooling' @('0')
        Set-JsonProperty $profile 'fan_cooling_layer_time' @('0')
        Set-JsonProperty $profile 'slow_down_layer_time' @('0')
        $profilePath = Join-Path $runRoot "filament_$index.json"
        $profile | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $profilePath -Encoding UTF8
        $profilePath
    }
)
$results = [Collections.Generic.List[object]]::new()

foreach ($case in $cases) {
    $caseRoot = Join-Path $runRoot $case.Name
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'
    $stdoutPath = Join-Path $caseRoot 'cli.out.log'
    $stderrPath = Join-Path $caseRoot 'cli.err.log'

    $machine = Get-Content -Raw -LiteralPath $MachineTemplate | ConvertFrom-Json
    $process = Get-Content -Raw -LiteralPath $ProcessTemplate | ConvertFrom-Json
    $machineName = "Codex speed verify $WallGenerator $($case.Name)"
    $machine.name = $machineName
    $machine.setting_id = "speed-$WallGenerator-$($case.Name)"
    $process.name = $machineName
    $process.setting_id = "speed-$WallGenerator-$($case.Name)"
    $process.compatible_printers = @($machineName)

    Set-JsonProperty $process 'wall_generator' $WallGenerator
    Set-JsonProperty $process 'wall_loops' '3'
    Set-JsonProperty $process 'only_one_wall_top' '0'
    Set-JsonProperty $process 'only_one_wall_first_layer' '0'
    Set-JsonProperty $process 'slow_down_layers' '1'
    Set-JsonProperty $process 'resonance_avoidance' '0'
    Set-JsonProperty $process 'outer_wall_speed' '80'
    Set-JsonProperty $process 'inner_wall_speed' '120'
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '1'
    Set-JsonProperty $process 'crisp_corner_small_nozzle_wall_count' '4'
    Set-JsonProperty $process 'crisp_corner_interlace_small_nozzle_walls' '0'
    Set-JsonProperty $process 'crisp_corner_small_nozzle_wall_speed' $case.Value
    Set-JsonProperty $process 'sparse_infill_density' '15%'
    Set-JsonProperty $process 'sparse_infill_pattern' 'gyroid'

    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

    $settingsArg = [char]34 + "$machinePath;$processPath" + [char]34
    $filamentsArg = [char]34 + ($filamentProfiles -join ';') + [char]34
    $outputArg = [char]34 + $caseRoot + [char]34
    $modelArg = [char]34 + $ModelPath + [char]34
    $arguments = @('--slice', '0', '--debug', '1', '--load-settings', $settingsArg,
        '--load-filaments', $filamentsArg, '--outputdir', $outputArg, $modelArg)
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $handle.Kill()
        throw "Slice timed out after $SliceTimeoutSeconds seconds: $($case.Name)"
    }
    $handle.Refresh()
    $gcodePath = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1 -ExpandProperty FullName
    $exitCode = $handle.ExitCode
    if ($null -eq $exitCode) { $exitCode = if ([string]::IsNullOrWhiteSpace($gcodePath)) { 1 } else { 0 } }
    if ($exitCode -ne 0 -or [string]::IsNullOrWhiteSpace($gcodePath)) {
        throw "CLI slice failed for $($case.Name), exit code $exitCode"
    }

    $measurement = Measure-DetailWallSpeeds $gcodePath 1
    $errors = [Collections.Generic.List[string]]::new()
    if ($measurement.OuterSamples -eq 0) { $errors.Add('No detail-tool outer-wall samples') }
    if ($measurement.InnerSamples -eq 0) { $errors.Add('No detail-tool inner-wall samples') }
    if ([Math]::Abs($measurement.OuterMedian - $case.ExpectedOuter) -gt 0.2) {
        $errors.Add("Outer speed $($measurement.OuterMedian) != $($case.ExpectedOuter)")
    }
    if ([Math]::Abs($measurement.InnerMedian - $case.ExpectedInner) -gt 0.2) {
        $errors.Add("Inner speed $($measurement.InnerMedian) != $($case.ExpectedInner)")
    }
    if ($measurement.EmbeddedSetting -ne $case.Value) {
        $errors.Add("Embedded setting '$($measurement.EmbeddedSetting)' != '$($case.Value)'")
    }

    $results.Add([pscustomobject]@{
        Case = $case.Name
        Setting = $case.Value
        OuterSpeed = [Math]::Round($measurement.OuterMedian, 3)
        InnerSpeed = [Math]::Round($measurement.InnerMedian, 3)
        OuterSamples = $measurement.OuterSamples
        InnerSamples = $measurement.InnerSamples
        Passed = $errors.Count -eq 0
        Errors = $errors -join '; '
        Gcode = $gcodePath
    })
}

$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Format-Table Case,Setting,OuterSpeed,InnerSpeed,OuterSamples,InnerSamples,Passed,Errors -AutoSize
$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$resultsPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
