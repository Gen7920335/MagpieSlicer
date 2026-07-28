param(
    [string] $SlicerPath,
    [string] $ImportedSettingsRoot = 'C:\orpkg\imported-orca-settings-20260722-203011\OrcaSlicer',
    [string] $OutputRoot = 'C:\orpkg\imported-multinozzle-wall-count',
    [int] $SliceTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\orca-slicer-trinterface.exe'
}

$modelPath = Join-Path $RepoRoot 'resources\handy_models\OrcaCube_v2.drc'
$baseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$baseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
$importedProcessPath = Join-Path $ImportedSettingsRoot 'user\default\process\U1 0.4nz 0.24lh.json'
foreach ($path in @($SlicerPath, $modelPath, $baseMachinePath, $baseProcessPath, $importedProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

function Find-FilamentProfile([string] $name) {
    $match = Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'resources\profiles') -Recurse -File -Filter "$name.json" |
        Select-Object -First 1
    if ($null -eq $match) { throw "Filament profile not found: $name" }
    return $match.FullName
}

function Set-JsonProperty($object, [string] $name, $value) {
    $object | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force
}

function Measure-WallLoops([string] $gcodePath) {
    $layer = -1
    $tool = 0
    $feature = ''
    $x = 0.0
    $y = 0.0
    $absolute = $true
    $path = $null
    $loops = [Collections.Generic.List[object]]::new()

    $finishPath = {
        if ($null -eq $path) { return }
        if ($path.Points.Count -ge 3) {
            $xs = @($path.Points | ForEach-Object X)
            $ys = @($path.Points | ForEach-Object Y)
            $width = ($xs | Measure-Object -Maximum).Maximum - ($xs | Measure-Object -Minimum).Minimum
            $height = ($ys | Measure-Object -Maximum).Maximum - ($ys | Measure-Object -Minimum).Minimum
            $first = $path.Points[0]
            $last = $path.Points[$path.Points.Count - 1]
            $closure = [Math]::Sqrt([Math]::Pow($last.X - $first.X, 2) + [Math]::Pow($last.Y - $first.Y, 2))
            if ($width -ge 15.0 -and $height -ge 15.0 -and $closure -le 1.5) {
                $loops.Add([pscustomobject]@{
                    Layer = $path.Layer
                    Tool = $path.Tool
                    Feature = $path.Feature
                    Width = [Math]::Round($width, 3)
                    Height = [Math]::Round($height, 3)
                })
            }
        }
        $path = $null
    }

    foreach ($line in [IO.File]::ReadLines($gcodePath)) {
        if ($line -eq ';LAYER_CHANGE') {
            . $finishPath
            ++$layer
            continue
        }
        if ($line -match '^T(\d+)\s*$') {
            . $finishPath
            $tool = [int] $Matches[1]
            continue
        }
        if ($line -match '^;TYPE:(.+)$') {
            . $finishPath
            $feature = $Matches[1].Trim()
            continue
        }
        if ($line -eq 'G90') { $absolute = $true; continue }
        if ($line -eq 'G91') { $absolute = $false; continue }
        if ($line -notmatch '^G[01]\s') { continue }

        $nextX = $x
        $nextY = $y
        if ($line -match '(?:^|\s)X(-?(?:\d+(?:\.\d*)?|\.\d+))') {
            $value = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
            $nextX = if ($absolute) { $value } else { $x + $value }
        }
        if ($line -match '(?:^|\s)Y(-?(?:\d+(?:\.\d*)?|\.\d+))') {
            $value = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
            $nextY = if ($absolute) { $value } else { $y + $value }
        }
        $hasXY = $nextX -ne $x -or $nextY -ne $y
        $extruding = $false
        if ($line -match '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') {
            $extruding = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) -gt 0
        }

        $isWall = $feature -in @('Outer wall', 'Inner wall')
        if ($layer -ge 0 -and $isWall -and $hasXY -and $extruding) {
            if ($null -eq $path) {
                $path = [pscustomobject]@{
                    Layer = $layer
                    Tool = $tool
                    Feature = $feature
                    Points = [Collections.Generic.List[object]]::new()
                }
                $path.Points.Add([pscustomobject]@{ X = $x; Y = $y })
            }
            $path.Points.Add([pscustomobject]@{ X = $nextX; Y = $nextY })
        } else {
            . $finishPath
        }
        $x = $nextX
        $y = $nextY
    }
    . $finishPath
    return $loops
}

function Split-BoundaryCounts($loops) {
    $ordered = @($loops | Sort-Object Width)
    if ($ordered.Count -lt 2) {
        return [pscustomobject]@{ Inner = $ordered.Count; Outer = 0; Gap = 0.0; Midpoint = 0.0 }
    }

    $split = 1
    $largestGap = -1.0
    for ($i = 1; $i -lt $ordered.Count; ++$i) {
        $gap = [double] $ordered[$i].Width - [double] $ordered[$i - 1].Width
        if ($gap -gt $largestGap) {
            $largestGap = $gap
            $split = $i
        }
    }
    return [pscustomobject]@{
        Inner = $split
        Outer = $ordered.Count - $split
        Gap = [Math]::Round($largestGap, 3)
        Midpoint = ([double] $ordered[$split - 1].Width + [double] $ordered[$split].Width) / 2.0
    }
}

$filamentProfiles = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$results = [Collections.Generic.List[object]]::new()

foreach ($generator in @('classic', 'arachne')) {
    $caseRoot = Join-Path $runRoot $generator
    New-Item -ItemType Directory -Path $caseRoot -Force | Out-Null
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'

    $machine = Get-Content -LiteralPath $baseMachinePath -Raw | ConvertFrom-Json
    $machine.nozzle_diameter = @('0.4', '0.15', '0.4', '0.4')
    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8

    $process = Get-Content -LiteralPath $baseProcessPath -Raw | ConvertFrom-Json
    $importedProcess = Get-Content -LiteralPath $importedProcessPath -Raw | ConvertFrom-Json
    foreach ($property in $importedProcess.PSObject.Properties) {
        if ($property.Name -notin @('name', 'inherits', 'from', 'version', 'print_settings_id')) {
            Set-JsonProperty $process $property.Name $property.Value
        }
    }
    Set-JsonProperty $process 'name' "Imported U1 wall count $generator"
    Set-JsonProperty $process 'setting_id' "imported-u1-wall-count-$generator"
    Set-JsonProperty $process 'wall_generator' $generator
    Set-JsonProperty $process 'wall_loops' '6'
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '1'
    Set-JsonProperty $process 'crisp_corner_detail_toolhead' '0'
    Set-JsonProperty $process 'crisp_corner_small_nozzle_wall_count' '4'
    Set-JsonProperty $process 'crisp_corner_interlace_small_nozzle_walls' '1'
    Set-JsonProperty $process 'crisp_corner_nozzle_wall_overlap' '15%'
    Set-JsonProperty $process 'enable_support' '0'
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

    $settingsArg = [char]34 + "$machinePath;$processPath" + [char]34
    $filamentsArg = [char]34 + ($filamentProfiles -join ';') + [char]34
    $outputArg = [char]34 + $caseRoot + [char]34
    $modelArg = [char]34 + $modelPath + [char]34
    $stdoutPath = Join-Path $caseRoot 'cli.out.log'
    $stderrPath = Join-Path $caseRoot 'cli.err.log'
    $arguments = @('--slice', '0', '--debug', '1', '--load-settings', $settingsArg, '--load-filaments', $filamentsArg, '--outputdir', $outputArg, $modelArg)
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $handle.Kill()
        throw "Slice timed out: $generator"
    }
    $handle.Refresh()
    $gcodePath = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1 -ExpandProperty FullName
    if ($null -eq $gcodePath) {
        throw "No G-code produced for $generator`n$(Get-Content -LiteralPath $stderrPath -Raw)"
    }

    $loops = Measure-WallLoops $gcodePath
    $layers = foreach ($group in ($loops | Group-Object Layer)) {
        $layerNumber = [int] $group.Name
        # OrcaCube lettering starts changing the contour topology above this
        # range. Keep the assertion on the stable hollow-wall section.
        if ($layerNumber -lt 5 -or $layerNumber -gt 14) { continue }
        $smallLoops = @($group.Group | Where-Object Tool -eq 1)
        $largeLoops = @($group.Group | Where-Object Tool -eq 0)
        $smallSides = Split-BoundaryCounts $smallLoops
        $largeInner = @($largeLoops | Where-Object { [double] $_.Width -lt $smallSides.Midpoint }).Count
        $largeOuter = $largeLoops.Count - $largeInner
        $expectedSmall = if (($layerNumber % 2) -eq 0) { 4 } else { 3 }
        [pscustomobject]@{
            Layer = $layerNumber + 1
            SmallInner = $smallSides.Inner
            SmallOuter = $smallSides.Outer
            LargeInner = $largeInner
            LargeOuter = $largeOuter
            ExpectedSmall = $expectedSmall
            SpaceLimited = $largeInner -lt 6 -or $largeOuter -lt 6
        }
    }
    $checked = @($layers | Where-Object { $_.SmallInner -gt 0 -and $_.SmallOuter -gt 0 -and $_.LargeInner -gt 0 -and $_.LargeOuter -gt 0 })
    $failures = @($checked | Where-Object {
        $_.SmallInner -ne $_.ExpectedSmall -or
        $_.SmallOuter -ne $_.ExpectedSmall -or
        $_.LargeInner -lt 2 -or
        $_.LargeOuter -lt 2 -or
        [Math]::Abs($_.LargeInner - $_.LargeOuter) -gt 1
    })
    $results.Add([pscustomobject]@{
        Generator = $generator
        CheckedLayers = $checked.Count
        FailedLayers = $failures.Count
        Passed = $checked.Count -ge 4 -and $failures.Count -eq 0
        Layers = $checked
        Gcode = $gcodePath
    })
}

$resultPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$results | Select-Object Generator, CheckedLayers, FailedLayers, Passed | Format-Table -AutoSize
foreach ($result in $results) {
    "--- $($result.Generator) ---"
    $result.Layers | Select-Object -First 12 | Format-Table -AutoSize
}
"RESULTS=$resultPath"
if (@($results | Where-Object { -not $_.Passed }).Count -gt 0) { exit 1 }
