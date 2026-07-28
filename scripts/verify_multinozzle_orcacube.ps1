param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [string] $ModelPath,
    [int] $SliceTimeoutSeconds = 180,
    [string] $CaseFilter = '*'
)

$ErrorActionPreference = 'Stop'
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\orca-slicer-trinterface.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\multinozzle-orcacube'
}
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $RepoRoot 'resources\handy_models\OrcaCube_v2.drc'
}

$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$ModelPath = [IO.Path]::GetFullPath($ModelPath)
$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'

foreach ($requiredPath in @($SlicerPath, $ModelPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file not found: $requiredPath"
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

function Format-Number([double] $value) {
    return $value.ToString('0.###', $Invariant)
}

function Set-JsonArrayValue($object, [string] $property, [int] $index, [string] $value) {
    if ($null -eq $object.$property -or $object.$property.Count -le $index) {
        throw "Machine profile property '$property' has no index $index"
    }
    $object.$property[$index] = $value
}

function Set-JsonProperty($object, [string] $property, $value) {
    if ($null -eq $object.PSObject.Properties[$property]) {
        $object | Add-Member -NotePropertyName $property -NotePropertyValue $value
    } else {
        $object.$property = $value
    }
}

function Get-Median([System.Collections.Generic.List[double]] $values) {
    if ($values.Count -eq 0) { return 0.0 }
    $ordered = $values.ToArray() | Sort-Object
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { return [double] $ordered[$middle] }
    return ([double] $ordered[$middle - 1] + [double] $ordered[$middle]) / 2.0
}

function Convert-PrintableArea($printableArea) {
    $points = [Collections.Generic.List[object]]::new()
    foreach ($entry in $printableArea) {
        if ([string] $entry -notmatch '^\s*(-?(?:\d+(?:\.\d*)?|\.\d+))x(-?(?:\d+(?:\.\d*)?|\.\d+))\s*$') {
            throw "Invalid printable_area coordinate: $entry"
        }
        $points.Add([pscustomobject]@{
            X = [double]::Parse($Matches[1], $Invariant)
            Y = [double]::Parse($Matches[2], $Invariant)
        })
    }
    if ($points.Count -lt 3) { throw 'printable_area must contain at least three points' }
    return $points.ToArray()
}

function Test-PointInPrintableArea([double] $x, [double] $y, $points, [double] $tolerance = 0.05) {
    $inside = $false
    for ($i = 0; $i -lt $points.Count; ++$i) {
        $j = if ($i -eq 0) { $points.Count - 1 } else { $i - 1 }
        $a = $points[$j]
        $b = $points[$i]
        $dx = $b.X - $a.X
        $dy = $b.Y - $a.Y
        $lengthSquared = $dx * $dx + $dy * $dy
        if ($lengthSquared -gt 0) {
            $projection = (($x - $a.X) * $dx + ($y - $a.Y) * $dy) / $lengthSquared
            $projection = [Math]::Max(0.0, [Math]::Min(1.0, $projection))
            $distanceX = $x - ($a.X + $projection * $dx)
            $distanceY = $y - ($a.Y + $projection * $dy)
            if ($distanceX * $distanceX + $distanceY * $distanceY -le $tolerance * $tolerance) { return $true }
        }
        if (($a.Y -gt $y) -ne ($b.Y -gt $y)) {
            $intersectionX = ($b.X - $a.X) * ($y - $a.Y) / ($b.Y - $a.Y) + $a.X
            if ($x -lt $intersectionX) { $inside = -not $inside }
        }
    }
    return $inside
}

function Analyze-Gcode(
    [string] $path,
    [double] $layerHeight,
    $printableArea,
    [double] $printableHeight,
    [int] $toolCount
) {
    $tool = -1
    $role = ''
    $layer = -1
    $absoluteXY = $true
    $relativeExtrusion = $false
    $x = $null
    $y = $null
    $z = $null
    $e = 0.0
    $settings = @{}
    $lengthByTool = @{}
    $movesByTool = @{}
    $extrusionMovesByTool = @{}
    $lengthByLayerTool = @{}
    $lengthByRoleTool = @{}
    $widthSamplesByTool = @{}
    $straightWallSamples = @{}
    $toolChangesByLayer = @{}
    $wallTools = [Collections.Generic.HashSet[int]]::new()
    $sparseInfillSeen = $false
    $unsupportedToolCommands = 0
    $extrusionBeforeTool = 0
    $nonFiniteNumbers = 0
    $outOfBoundsExtrusions = 0
    $lastToolChangeLayer = -1
    $filamentArea = [Math]::PI * 1.75 * 1.75 / 4.0

    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { $layer++; continue }
        if ($line -match '^;TYPE:(.+)$') {
            $role = $Matches[1].Trim()
            if ($role -eq 'Sparse infill') { $sparseInfillSeen = $true }
            continue
        }
        if ($line -match '^T(-?\d+)(?:\s|$)') {
            $newTool = [int] $Matches[1]
            if ($newTool -lt 0 -or $newTool -ge $toolCount) { $unsupportedToolCommands++ }
            if ($layer -ge 0 -and $tool -ge 0 -and $newTool -ne $tool) {
                $toolChangesByLayer[$layer] = [int] $toolChangesByLayer[$layer] + 1
                $lastToolChangeLayer = $layer
            }
            $tool = $newTool
            continue
        }
        if ($line -eq 'G90') { $absoluteXY = $true; continue }
        if ($line -eq 'G91') { $absoluteXY = $false; continue }
        if ($line -match '^M82(?:\s|$)') { $relativeExtrusion = $false; continue }
        if ($line -match '^M83(?:\s|$)') { $relativeExtrusion = $true; continue }
        if ($line -match '^G92(?:\s|$)') {
            if ($line -match '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') {
                $e = [double]::Parse($Matches[1], $Invariant)
            }
            continue
        }
        if ($line -match '^; ([a-z][a-z0-9_]*) = (.*)$') {
            $settings[$Matches[1]] = $Matches[2].Trim()
            continue
        }
        if ($line -notmatch '^G[0123](?:\s|$)') { continue }
        if ($line -match '(?i)(?:^|\s)[XYZEIJKRF][+-]?(?:nan|inf(?:inity)?)(?=\s|;|$)') {
            $nonFiniteNumbers++
            continue
        }

        $newX = $x
        $newY = $y
        $newZ = $z
        $newE = $e
        $hasX = $false
        $hasY = $false
        $hasZ = $false
        $hasE = $false
        foreach ($token in [regex]::Matches($line, '(?:^|\s)([XYZE])(-?(?:\d+(?:\.\d*)?|\.\d+))')) {
            $axis = $token.Groups[1].Value
            $value = [double]::Parse($token.Groups[2].Value, $Invariant)
            switch ($axis) {
                'X' { $newX = if ($absoluteXY -or $null -eq $x) { $value } else { $x + $value }; $hasX = $true }
                'Y' { $newY = if ($absoluteXY -or $null -eq $y) { $value } else { $y + $value }; $hasY = $true }
                'Z' { $newZ = if ($absoluteXY -or $null -eq $z) { $value } else { $z + $value }; $hasZ = $true }
                'E' { $newE = $value; $hasE = $true }
            }
        }
        $extrusion = if (-not $hasE) { 0.0 } elseif ($relativeExtrusion) { $newE } else { $newE - $e }
        $isPositiveExtrusion = $extrusion -gt 0.0000001
        if ($isPositiveExtrusion) {
            if ($tool -lt 0 -or $tool -ge $toolCount) {
                $extrusionBeforeTool++
            } else {
                $extrusionMovesByTool[$tool] = [int] $extrusionMovesByTool[$tool] + 1
            }
            if (($null -ne $newX -and $null -ne $newY -and -not (Test-PointInPrintableArea $newX $newY $printableArea)) -or
                ($null -ne $newZ -and ($newZ -lt -0.05 -or $newZ -gt $printableHeight + 0.05))) {
                $outOfBoundsExtrusions++
            }
            if ($tool -ge 0 -and $tool -lt $toolCount -and -not [string]::IsNullOrWhiteSpace($role) -and
                $null -ne $x -and $null -ne $y -and ($hasX -or $hasY)) {
                $roleDx = $newX - $x
                $roleDy = $newY - $y
                $roleLength = [Math]::Sqrt($roleDx * $roleDx + $roleDy * $roleDy)
                if ($roleLength -gt 0.01) {
                    $roleKey = "$role|$tool"
                    $lengthByRoleTool[$roleKey] = [double] $lengthByRoleTool[$roleKey] + $roleLength
                }
            }
        }

        $isWall = $role -in @('Outer wall', 'Inner wall', 'Overhang wall')
        if ($isWall -and $tool -ge 0 -and $layer -ge 0 -and $isPositiveExtrusion -and $null -ne $x -and $null -ne $y -and ($hasX -or $hasY)) {
            $dx = $newX - $x
            $dy = $newY - $y
            $length = [Math]::Sqrt($dx * $dx + $dy * $dy)
            if ($length -gt 0.01) {
                [void] $wallTools.Add($tool)
                $lengthByTool[$tool] = [double] $lengthByTool[$tool] + $length
                $movesByTool[$tool] = [int] $movesByTool[$tool] + 1
                $key = "$layer|$tool"
                $lengthByLayerTool[$key] = [double] $lengthByLayerTool[$key] + $length
                if ($layer -gt 2 -and $length -ge 0.5 -and $length -le 20.0) {
                    if (-not $widthSamplesByTool.ContainsKey($tool)) {
                        $widthSamplesByTool[$tool] = [Collections.Generic.List[double]]::new()
                    }
                    if ($widthSamplesByTool[$tool].Count -lt 20000) {
                        $area = $extrusion * $filamentArea / $length
                        $width = $area / $layerHeight + $layerHeight * (1.0 - [Math]::PI / 4.0)
                        if ($width -gt 0.05 -and $width -lt 2.0) { $widthSamplesByTool[$tool].Add($width) }
                    }
                }
                if ($layer -in @(100, 101) -and [Math]::Abs($dx) -lt 0.002 -and $length -gt 8.0 -and $newX -lt 130.0) {
                    $coordinate = [Math]::Round(($x + $newX) / 2.0, 3)
                    $straightKey = "$layer|$tool|$coordinate"
                    $straightWallSamples[$straightKey] = [double] $straightWallSamples[$straightKey] + $length
                }
            }
        }

        if ($hasX) { $x = $newX }
        if ($hasY) { $y = $newY }
        if ($hasZ) { $z = $newZ }
        if ($hasE) { $e = $newE }
    }

    $medianWidths = @{}
    foreach ($entry in $widthSamplesByTool.GetEnumerator()) {
        $medianWidths[$entry.Key] = Get-Median $entry.Value
    }

    return [pscustomobject]@{
        Settings = $settings
        WallTools = @($wallTools | Sort-Object)
        LengthByTool = $lengthByTool
        MovesByTool = $movesByTool
        ExtrusionMovesByTool = $extrusionMovesByTool
        LengthByLayerTool = $lengthByLayerTool
        LengthByRoleTool = $lengthByRoleTool
        MedianWidthByTool = $medianWidths
        StraightWallSamples = $straightWallSamples
        SparseInfillSeen = $sparseInfillSeen
        LayerCount = $layer + 1
        UnsupportedToolCommands = $unsupportedToolCommands
        ExtrusionBeforeTool = $extrusionBeforeTool
        NonFiniteNumbers = $nonFiniteNumbers
        OutOfBoundsExtrusions = $outOfBoundsExtrusions
        MaxToolChangesPerLayer = if ($toolChangesByLayer.Count -gt 0) { [int] ($toolChangesByLayer.Values | Measure-Object -Maximum).Maximum } else { 0 }
        LastToolChangeLayer = $lastToolChangeLayer
    }
}

function Get-InterlockSpacing($straightWallSamples, [int] $smallTool, [int] $largeTool) {
    $smallSpacings = [Collections.Generic.List[double]]::new()
    $transitions = [Collections.Generic.List[double]]::new()
    $largeSpacings = [Collections.Generic.List[double]]::new()
    foreach ($layer in @(100, 101)) {
        $levels = @($straightWallSamples.GetEnumerator() | ForEach-Object {
            $parts = $_.Key.Split('|')
            if ([int] $parts[0] -eq $layer -and [double] $_.Value -gt 10.0) {
                [pscustomobject]@{ Tool=[int]$parts[1]; Coordinate=[double]::Parse($parts[2], $Invariant) }
            }
        } | Sort-Object Coordinate)
        for ($index = 1; $index -lt $levels.Count; ++$index) {
            $distance = [Math]::Abs($levels[$index].Coordinate - $levels[$index - 1].Coordinate)
            if ($distance -le 0.001 -or $distance -gt 0.8) { continue }
            $previousTool = $levels[$index - 1].Tool
            $currentTool = $levels[$index].Tool
            if ($previousTool -ne $currentTool) {
                $transitions.Add($distance)
            } elseif ($currentTool -eq $smallTool) {
                $smallSpacings.Add($distance)
            } elseif ($currentTool -eq $largeTool) {
                $largeSpacings.Add($distance)
            }
        }
    }
    return [pscustomobject]@{
        Small = Get-Median $smallSpacings
        Transition = Get-Median $transitions
        Large = Get-Median $largeSpacings
        TransitionSamples = $transitions.Count
    }
}

function Get-ParityAverage($lengthByLayerTool, [int] $tool) {
    $sum = @(0.0, 0.0)
    $count = @(0, 0)
    foreach ($entry in $lengthByLayerTool.GetEnumerator()) {
        $parts = $entry.Key.Split('|')
        if ([int] $parts[1] -ne $tool) { continue }
        $layer = [int] $parts[0]
        if ($layer -lt 5) { continue }
        $parity = $layer % 2
        $sum[$parity] += [double] $entry.Value
        $count[$parity]++
    }
    return @(
        $(if ($count[0] -gt 0) { $sum[0] / $count[0] } else { 0.0 }),
        $(if ($count[1] -gt 0) { $sum[1] / $count[1] } else { 0.0 })
    )
}

$cases = @()
foreach ($generator in @('classic', 'arachne')) {
    $cases += [pscustomobject]@{ Name = "${generator}_disabled_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 0; Enabled = $false; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_count1_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 1; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_count3_interlock_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 3; WallLoops = 3; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_count4_interlock_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_count4_nointerlock_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_overlap_min_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; Overlap = 0; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_overlap_max_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; Overlap = 80; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_wallloops_min_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; WallLoops = 1; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_wallloops_max_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; WallLoops = 8; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_count4_020"; Generator = $generator; Nozzle2 = 0.20; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_manual_tool2_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; DetailToolhead = 2; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_reverse_060"; Generator = $generator; Nozzle2 = 0.60; BaseTool = 2; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_manual_tool1_reverse_060"; Generator = $generator; Nozzle2 = 0.60; BaseTool = 2; DetailToolhead = 1; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_reverse_080"; Generator = $generator; Nozzle2 = 0.80; BaseTool = 2; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_fourtool_base3_detail1"; Generator = $generator; Nozzle2 = 0.80; Nozzles = @(0.40, 0.80, 0.60, 0.80); BaseTool = 3; Count = 3; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_fourtool_base4_detail2"; Generator = $generator; Nozzle2 = 0.15; Nozzles = @(0.80, 0.15, 0.80, 0.80); BaseTool = 4; Count = 4; Enabled = $true; Interlock = $false; RequireLarge = $true; OverrideRanges = @(); OverrideChecks = @() }
    $cases += [pscustomobject]@{ Name = "${generator}_max100_015"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 100; Enabled = $true; Interlock = $false; RequireLarge = $false; OverrideRanges = @(); OverrideChecks = @() }

    $cases += [pscustomobject]@{ Name = "${generator}_override_single"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('40:60:1'); OverrideChecks = @(@{ Layer = 39; Tool = 0 }, @{ Layer = 59; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_overlap"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('40:55:1', '50:65:1'); OverrideChecks = @(@{ Layer = 49; Tool = 0 }, @{ Layer = 64; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_last_wins"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('40:65:1', '50:55:2'); OverrideChecks = @(@{ Layer = 49; Tool = 1 }, @{ Layer = 55; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_reversed"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('65:40:1'); OverrideChecks = @(@{ Layer = 39; Tool = 0 }, @{ Layer = 64; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_minimum"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('1:1:1'); OverrideChecks = @(@{ Layer = 0; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_maximum"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('150:1000000:1'); OverrideChecks = @(@{ Layer = 149; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_first20"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $true; Interlock = $true; RequireLarge = $true; OverrideRanges = @('1:20:1'); OverrideChecks = @(@{ Layer = 0; Tool = 0 }, @{ Layer = 19; Tool = 0 }) }
    $cases += [pscustomobject]@{ Name = "${generator}_override_disabled_target2_first20"; Generator = $generator; Nozzle2 = 0.15; BaseTool = 1; Count = 4; Enabled = $false; Interlock = $false; RequireLarge = $true; OverrideRanges = @('1:20:2'); OverrideChecks = @(@{ Layer = 0; Tool = 1 }, @{ Layer = 19; Tool = 1 }, @{ Layer = 20; Tool = 0 }) }
}
$cases = @($cases | Where-Object { $_.Name -like $CaseFilter })
if ($cases.Count -eq 0) { throw "No verification cases matched: $CaseFilter" }

$filamentProfiles = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)
$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()
$layerHeight = 0.10
$widthByNozzle = @{ '0.15' = 0.17; '0.2' = 0.22; '0.4' = 0.45; '0.6' = 0.66; '0.8' = 0.88; '1' = 1.1 }

$caseIndex = 0
foreach ($case in $cases) {
    $caseRoot = Join-Path $runRoot ('case_{0:D2}' -f $caseIndex)
    ++$caseIndex
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'
    $stdoutPath = Join-Path $caseRoot 'cli.out.log'
    $stderrPath = Join-Path $caseRoot 'cli.err.log'

    $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
    $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
    $printableArea = Convert-PrintableArea $machine.printable_area
    $printableHeight = [double]::Parse([string] $machine.printable_height, $Invariant)
    $toolCount = $machine.nozzle_diameter.Count
    $caseNozzles = @($machine.nozzle_diameter | ForEach-Object { [double]::Parse([string] $_, $Invariant) })
    if ($null -ne $case.PSObject.Properties['Nozzles']) {
        $caseNozzles = @($case.Nozzles | ForEach-Object { [double] $_ })
    } else {
        $caseNozzles[1] = [double] $case.Nozzle2
    }
    if ($caseNozzles.Count -ne $toolCount) {
        throw "Case $($case.Name) defines $($caseNozzles.Count) nozzles for a $toolCount-tool machine"
    }
    $machineName = "Codex verify $($case.Name)"
    $machine.name = $machineName
    $machine.setting_id = "verify-$($case.Name)"
    for ($toolIndex = 0; $toolIndex -lt $toolCount; ++$toolIndex) {
        $nozzle = $caseNozzles[$toolIndex]
        $nozzleKey = Format-Number $nozzle
        if (-not $widthByNozzle.ContainsKey($nozzleKey)) {
            throw "No default width is defined for nozzle $nozzleKey"
        }
        $toolWidth = [double] $widthByNozzle[$nozzleKey]
        Set-JsonArrayValue $machine 'nozzle_diameter' $toolIndex $nozzleKey
        Set-JsonArrayValue $machine 'min_layer_height' $toolIndex (Format-Number ([Math]::Min(0.05, $nozzle * 0.25)))
        Set-JsonArrayValue $machine 'max_layer_height' $toolIndex (Format-Number ($nozzle * 0.75))
        foreach ($property in @(
            'toolhead_line_width', 'toolhead_outer_wall_line_width', 'toolhead_inner_wall_line_width',
            'toolhead_top_surface_line_width', 'toolhead_sparse_infill_line_width',
            'toolhead_internal_solid_infill_line_width', 'toolhead_support_line_width'
        )) {
            Set-JsonArrayValue $machine $property $toolIndex (Format-Number $toolWidth)
        }
        Set-JsonArrayValue $machine 'toolhead_initial_layer_line_width' $toolIndex (Format-Number ($nozzle * 1.4))
        Set-JsonArrayValue $machine 'toolhead_bridge_line_width' $toolIndex $nozzleKey
    }

    $process.name = "Codex verify $($case.Name)"
    $process.setting_id = "verify-$($case.Name)"
    $process.compatible_printers = @($machineName)
    $caseOverlap = if ($null -ne $case.PSObject.Properties['Overlap']) { [int] $case.Overlap } else { 15 }
    $caseWallLoops = if ($null -ne $case.PSObject.Properties['WallLoops']) { [int] $case.WallLoops } else { 3 }
    $caseDetailToolhead = if ($null -ne $case.PSObject.Properties['DetailToolhead']) { [int] $case.DetailToolhead } else { 0 }
    Set-JsonProperty $process 'layer_height' (Format-Number $layerHeight)
    Set-JsonProperty $process 'initial_layer_print_height' (Format-Number $layerHeight)
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' $(if ($case.Enabled) { '1' } else { '0' })
    Set-JsonProperty $process 'crisp_corner_detail_toolhead' ([string] $caseDetailToolhead)
    Set-JsonProperty $process 'crisp_corner_small_nozzle_wall_count' ([string] $case.Count)
    Set-JsonProperty $process 'crisp_corner_interlace_small_nozzle_walls' $(if ($case.Interlock) { '1' } else { '0' })
    Set-JsonProperty $process 'crisp_corner_nozzle_wall_overlap' "${caseOverlap}%"
    if ($case.OverrideRanges.Count -gt 0) {
        $process | Add-Member -MemberType NoteProperty -Name 'crisp_corner_large_nozzle_override_regions' -Value @($case.OverrideRanges) -Force
    }
    Set-JsonProperty $process 'wall_generator' $case.Generator
    Set-JsonProperty $process 'wall_loops' ([string] $caseWallLoops)
    Set-JsonProperty $process 'outer_wall_filament_id' ([string] $case.BaseTool)
    Set-JsonProperty $process 'inner_wall_filament_id' ([string] $case.BaseTool)
    Set-JsonProperty $process 'sparse_infill_filament_id' ([string] $case.BaseTool)
    Set-JsonProperty $process 'sparse_infill_density' '15%'
    Set-JsonProperty $process 'sparse_infill_pattern' 'gyroid'
    Set-JsonProperty $process 'bridge_line_width' '0'

    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

    $settingsArg = [char]34 + "$machinePath;$processPath" + [char]34
    $filamentsArg = [char]34 + ($filamentProfiles -join ';') + [char]34
    $outputArg = [char]34 + $caseRoot + [char]34
    $modelArg = [char]34 + $ModelPath + [char]34
    $arguments = @('--slice', '0', '--debug', '1', '--load-settings', $settingsArg, '--load-filaments', $filamentsArg, '--outputdir', $outputArg, $modelArg)
    $processHandle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    if (-not $processHandle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $processHandle.Kill()
        throw "Slice timed out after $SliceTimeoutSeconds seconds: $($case.Name)"
    }
    $processHandle.Refresh()
    $exitCode = $processHandle.ExitCode

    $errors = [Collections.Generic.List[string]]::new()
    $gcodePath = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1 -ExpandProperty FullName
    if ($null -eq $exitCode) { $exitCode = if ($null -ne $gcodePath) { 0 } else { 1 } }
    if ($exitCode -ne 0) { $errors.Add("CLI exit code $exitCode") }
    if ([string]::IsNullOrWhiteSpace($gcodePath)) { $errors.Add('G-code was not generated') }

    $analysis = $null
    $largeTool = [int] $case.BaseTool - 1
    $baseNozzle = $caseNozzles[$largeTool]
    $smallerTools = @(
        for ($toolIndex = 0; $toolIndex -lt $toolCount; ++$toolIndex) {
            if ($toolIndex -ne $largeTool -and $caseNozzles[$toolIndex] -lt $baseNozzle) {
                [pscustomobject]@{ Tool = $toolIndex; Nozzle = $caseNozzles[$toolIndex] }
            }
        }
    )
    $smallTool = if ($smallerTools.Count -gt 0) {
        [int] (($smallerTools | Sort-Object Nozzle,Tool | Select-Object -First 1).Tool)
    } else {
        $largeTool
    }
    if ($null -ne $gcodePath) {
        $analysis = Analyze-Gcode $gcodePath $layerHeight $printableArea $printableHeight $toolCount
        foreach ($nonWallRole in @('Bottom surface', 'Top surface', 'Internal solid infill', 'Sparse infill')) {
            $nonWallKey = "$nonWallRole|$smallTool"
            $baseRoleKey = "$nonWallRole|$([int] $case.BaseTool - 1)"
            $roleLength = 0.0
            foreach ($toolId in 0..($toolCount - 1)) {
                $roleLength += [double] $analysis.LengthByRoleTool["$nonWallRole|$toolId"]
            }
            if ($roleLength -gt 0.001) {
                if ([double] $analysis.LengthByRoleTool[$nonWallKey] -gt 0.001) {
                    $errors.Add("$nonWallRole incorrectly routed to small-nozzle T$smallTool")
                }
                if ([double] $analysis.LengthByRoleTool[$baseRoleKey] -le 0.001) {
                    $errors.Add("$nonWallRole was not emitted on base T$([int] $case.BaseTool - 1)")
                }
            }
        }
        foreach ($check in $case.OverrideChecks) {
            $selectedKey = "$($check.Layer)|$($check.Tool)"
            $otherTool = if ([int] $check.Tool -eq 0) { 1 } else { 0 }
            $otherKey = "$($check.Layer)|$otherTool"
            if ([double] $analysis.LengthByLayerTool[$selectedKey] -le 0.001) {
                $errors.Add("Override layer $($check.Layer + 1) has no wall extrusion on T$($check.Tool)")
            }
            if ([double] $analysis.LengthByLayerTool[$otherKey] -gt 0.001) {
                $errors.Add("Override layer $($check.Layer + 1) still has wall extrusion on T$otherTool")
            }
        }
        if ($analysis.UnsupportedToolCommands -gt 0) { $errors.Add("Unsupported tool commands: $($analysis.UnsupportedToolCommands)") }
        if ($analysis.ExtrusionBeforeTool -gt 0) { $errors.Add("Extrusions without a valid selected tool: $($analysis.ExtrusionBeforeTool)") }
        if ($analysis.NonFiniteNumbers -gt 0) { $errors.Add("Non-finite motion values: $($analysis.NonFiniteNumbers)") }
        if ($analysis.OutOfBoundsExtrusions -gt 0) { $errors.Add("Extrusions outside printable bounds: $($analysis.OutOfBoundsExtrusions)") }
        if ($analysis.MaxToolChangesPerLayer -gt 16) { $errors.Add("Excessive tool changes in one layer: $($analysis.MaxToolChangesPerLayer)") }
        if ($case.Count -lt 100 -and -not $analysis.SparseInfillSeen) { $errors.Add('Sparse infill extrusion was not found') }
        if ($analysis.Settings['sparse_infill_pattern'] -ne 'gyroid') { $errors.Add('Gyroid setting was not embedded') }
        if ($analysis.Settings['sparse_infill_density'] -ne '15%') { $errors.Add('15% infill setting was not embedded') }
        if ($analysis.Settings['wall_generator'] -ne $case.Generator) { $errors.Add('Wall generator setting mismatch') }
        if ($analysis.Settings['crisp_corner_small_nozzle_wall_count'] -ne [string] $case.Count) { $errors.Add('Small wall count setting mismatch') }

        if (-not $case.Enabled) {
            if ($case.OverrideRanges.Count -eq 0 -and $analysis.WallTools -contains $smallTool) { $errors.Add('Disabled mode used the detail tool') }
            if ($analysis.WallTools -notcontains ($case.BaseTool - 1)) { $errors.Add('Disabled mode did not use the base tool') }
        } else {
            if ($analysis.WallTools -notcontains $smallTool) { $errors.Add("Detail tool T$smallTool was not used for walls") }
            if ($case.RequireLarge -and $analysis.WallTools -notcontains $largeTool) { $errors.Add("Large tool T$largeTool was not used for walls") }
            if ($case.RequireLarge -and $analysis.MedianWidthByTool.ContainsKey($smallTool) -and $analysis.MedianWidthByTool.ContainsKey($largeTool)) {
                if ([double] $analysis.MedianWidthByTool[$smallTool] -ge [double] $analysis.MedianWidthByTool[$largeTool]) {
                    $errors.Add('Measured detail-wall width is not smaller than the large-wall width')
                }
                if ($case.Generator -eq 'classic') {
                    $expectedSmallWidth = [double] $machine.toolhead_outer_wall_line_width[$smallTool]
                    $expectedLargeWidth = [double] $machine.toolhead_outer_wall_line_width[$largeTool]
                    if ([Math]::Abs([double] $analysis.MedianWidthByTool[$smallTool] - $expectedSmallWidth) -gt 0.03) {
                        $errors.Add("Small-tool wall width did not follow T$smallTool nozzle settings")
                    }
                    if ([Math]::Abs([double] $analysis.MedianWidthByTool[$largeTool] - $expectedLargeWidth) -gt 0.03) {
                        $errors.Add("Large-tool wall width did not follow T$largeTool nozzle settings")
                    }
                }
            } elseIf ($case.RequireLarge) {
                $errors.Add('Not enough extrusion samples to verify wall widths')
            }
        }
        $expectedExtrusionTools = @($case.BaseTool - 1)
        if ($case.Enabled) { $expectedExtrusionTools += $smallTool }
        foreach ($serializedRange in $case.OverrideRanges) {
            if ($serializedRange -match '^\d+:\d+:(\d+)$') {
                $expectedExtrusionTools += [int] $Matches[1] - 1
            }
        }
        $expectedExtrusionTools = @($expectedExtrusionTools | Sort-Object -Unique)
        $unexpectedExtrusionTools = @($analysis.ExtrusionMovesByTool.Keys | Where-Object { [int] $_ -notin $expectedExtrusionTools })
        if ($unexpectedExtrusionTools.Count -gt 0) {
            $errors.Add("Unexpected extrusion tools: $($unexpectedExtrusionTools -join ',')")
        }

        if ($case.Interlock) {
            $parity = Get-ParityAverage $analysis.LengthByLayerTool $smallTool
            $parityBase = [Math]::Max($parity[0], $parity[1])
            $parityDelta = if ($parityBase -gt 0) { [Math]::Abs($parity[1] - $parity[0]) / $parityBase } else { 0.0 }
            if ($parityDelta -lt 0.03) { $errors.Add('Interlocking did not produce a measurable odd/even layer difference') }
            $spacing = Get-InterlockSpacing $analysis.StraightWallSamples $smallTool $largeTool
            if ($spacing.TransitionSamples -lt 2) {
                $errors.Add('Not enough straight-wall samples to verify the small/large boundary spacing')
            } elseif ($analysis.MedianWidthByTool.ContainsKey($smallTool) -and $analysis.MedianWidthByTool.ContainsKey($largeTool)) {
                $shapeCorrection = $layerHeight * (1.0 - [Math]::PI / 4.0)
                $smallNominalSpacing = [double] $analysis.MedianWidthByTool[$smallTool] - $shapeCorrection
                $largeNominalSpacing = [double] $analysis.MedianWidthByTool[$largeTool] - $shapeCorrection
                $expectedTransition = 0.5 * ($smallNominalSpacing + $largeNominalSpacing) - ($caseOverlap / 100.0) * [Math]::Min($smallNominalSpacing, $largeNominalSpacing)
                if ([Math]::Abs($spacing.Transition - $expectedTransition) -gt 0.006) {
                    $errors.Add("Small/large center spacing $([Math]::Round($spacing.Transition, 3)) mm differs from expected $([Math]::Round($expectedTransition, 3)) mm")
                }
            }
        } else {
            $parity = @(0.0, 0.0)
            $parityDelta = 0.0
            $spacing = [pscustomobject]@{ Small=0.0; Transition=0.0; Large=0.0; TransitionSamples=0 }
        }
    } else {
        $parity = @(0.0, 0.0)
        $parityDelta = 0.0
        $spacing = [pscustomobject]@{ Small=0.0; Transition=0.0; Large=0.0; TransitionSamples=0 }
    }

    $results.Add([pscustomobject]@{
        Case = $case.Name
        Generator = $case.Generator
        Nozzle2 = $case.Nozzle2
        Enabled = $case.Enabled
        Count = $case.Count
        Interlock = $case.Interlock
        SmallTool = $smallTool
        LargeTool = $largeTool
        WallTools = if ($null -ne $analysis) { $analysis.WallTools -join ',' } else { '' }
        SmallWallLength = if ($null -ne $analysis) { [Math]::Round([double] $analysis.LengthByTool[$smallTool], 2) } else { 0 }
        LargeWallLength = if ($null -ne $analysis) { [Math]::Round([double] $analysis.LengthByTool[$largeTool], 2) } else { 0 }
        SmallWallMoves = if ($null -ne $analysis) { [int] $analysis.MovesByTool[$smallTool] } else { 0 }
        LargeWallMoves = if ($null -ne $analysis) { [int] $analysis.MovesByTool[$largeTool] } else { 0 }
        SmallMedianWidth = if ($null -ne $analysis) { [Math]::Round([double] $analysis.MedianWidthByTool[$smallTool], 3) } else { 0 }
        LargeMedianWidth = if ($null -ne $analysis) { [Math]::Round([double] $analysis.MedianWidthByTool[$largeTool], 3) } else { 0 }
        InterlockParityDelta = [Math]::Round($parityDelta, 3)
        SmallCenterSpacing = [Math]::Round([double] $spacing.Small, 3)
        BoundaryCenterSpacing = [Math]::Round([double] $spacing.Transition, 3)
        LargeCenterSpacing = [Math]::Round([double] $spacing.Large, 3)
        UnsupportedToolCommands = if ($null -ne $analysis) { $analysis.UnsupportedToolCommands } else { 0 }
        ExtrusionBeforeTool = if ($null -ne $analysis) { $analysis.ExtrusionBeforeTool } else { 0 }
        NonFiniteNumbers = if ($null -ne $analysis) { $analysis.NonFiniteNumbers } else { 0 }
        OutOfBoundsExtrusions = if ($null -ne $analysis) { $analysis.OutOfBoundsExtrusions } else { 0 }
        MaxToolChangesPerLayer = if ($null -ne $analysis) { $analysis.MaxToolChangesPerLayer } else { 0 }
        Passed = $errors.Count -eq 0
        Errors = $errors -join '; '
        Gcode = $gcodePath
    })
}

foreach ($generator in @('classic', 'arachne')) {
    $minimum = $results | Where-Object { $_.Case -eq "${generator}_count1_015" }
    $interlocked = $results | Where-Object { $_.Case -eq "${generator}_count4_interlock_015" }
    $maximum = $results | Where-Object { $_.Case -eq "${generator}_max100_015" }
    if ($null -ne $minimum -and $null -ne $interlocked -and $minimum.Passed -and $interlocked.Passed -and $interlocked.SmallWallLength -le $minimum.SmallWallLength * 1.25) {
        $interlocked.Passed = $false
        $interlocked.Errors = 'Four-wall case did not substantially increase small-nozzle wall length'
    }
    if ($null -ne $minimum -and $null -ne $maximum -and $minimum.Passed -and $maximum.Passed -and $maximum.SmallWallMoves -le $minimum.SmallWallMoves) {
        $maximum.Passed = $false
        $maximum.Errors = 'Maximum wall count did not increase small-nozzle wall moves'
    }
}

$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Select-Object Case,Passed,WallTools,SmallMedianWidth,LargeMedianWidth,BoundaryCenterSpacing,MaxToolChangesPerLayer,OutOfBoundsExtrusions,Errors |
    Format-Table -AutoSize

$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$resultsPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
