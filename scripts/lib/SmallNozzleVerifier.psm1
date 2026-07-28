$ErrorActionPreference = 'Stop'
$script:Invariant = [Globalization.CultureInfo]::InvariantCulture

function Get-Median {
    param([double[]] $Values)
    if ($null -eq $Values -or $Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $middle = [int] [Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { return [double] $ordered[$middle] }
    return ([double] $ordered[$middle - 1] + [double] $ordered[$middle]) / 2.0
}

function Convert-SettingNumber {
    param($Value)
    return [double]::Parse(([string] $Value).TrimEnd('%'), $script:Invariant)
}

function Get-PrintableAreaInfo {
    param($PrintableArea)
    $points = foreach ($entry in $PrintableArea) {
        if ([string] $entry -notmatch '^\s*(-?(?:\d+(?:\.\d*)?|\.\d+))x(-?(?:\d+(?:\.\d*)?|\.\d+))\s*$') {
            throw "Invalid printable_area coordinate: $entry"
        }
        [pscustomobject]@{
            X = [double]::Parse($Matches[1], $script:Invariant)
            Y = [double]::Parse($Matches[2], $script:Invariant)
        }
    }
    if (@($points).Count -lt 3) { throw 'printable_area must contain at least three points' }
    $minX = [double] ($points.X | Measure-Object -Minimum).Minimum
    $maxX = [double] ($points.X | Measure-Object -Maximum).Maximum
    $minY = [double] ($points.Y | Measure-Object -Minimum).Minimum
    $maxY = [double] ($points.Y | Measure-Object -Maximum).Maximum
    return [pscustomobject]@{
        Points = @($points)
        MinX = $minX
        MaxX = $maxX
        MinY = $minY
        MaxY = $maxY
        CenterX = ($minX + $maxX) / 2.0
        CenterY = ($minY + $maxY) / 2.0
    }
}

function Test-PointInPolygon {
    param(
        [double] $X,
        [double] $Y,
        $Points,
        [double] $Tolerance = 0.05
    )
    $inside = $false
    for ($i = 0; $i -lt $Points.Count; ++$i) {
        $j = if ($i -eq 0) { $Points.Count - 1 } else { $i - 1 }
        $a = $Points[$j]
        $b = $Points[$i]
        $dx = $b.X - $a.X
        $dy = $b.Y - $a.Y
        $lengthSquared = $dx * $dx + $dy * $dy
        if ($lengthSquared -gt 0) {
            $projection = (($X - $a.X) * $dx + ($Y - $a.Y) * $dy) / $lengthSquared
            $projection = [Math]::Max(0.0, [Math]::Min(1.0, $projection))
            $distanceX = $X - ($a.X + $projection * $dx)
            $distanceY = $Y - ($a.Y + $projection * $dy)
            if ($distanceX * $distanceX + $distanceY * $distanceY -le $Tolerance * $Tolerance) { return $true }
        }
        if (($a.Y -gt $Y) -ne ($b.Y -gt $Y)) {
            $intersectionX = ($b.X - $a.X) * ($Y - $a.Y) / ($b.Y - $a.Y) + $a.X
            if ($X -lt $intersectionX) { $inside = -not $inside }
        }
    }
    return $inside
}

function Get-ProbeLayerNumbers {
    param(
        [double] $LayerHeight,
        [double] $ProbeZ = 8.0,
        [ValidateRange(2, 100)]
        [int] $Count = 2
    )
    $first = [Math]::Max(0, [int] [Math]::Round($ProbeZ / $LayerHeight) - 1)
    return @(0..($Count - 1) | ForEach-Object { $first + $_ })
}

function Add-MapNumber {
    param(
        [hashtable] $Map,
        [string] $Key,
        [double] $Value
    )
    $Map[$Key] = [double] $Map[$Key] + $Value
}

function Get-SmallNozzleGcodeAnalysis {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] $PrintableArea,
        [Parameter(Mandatory)] [double] $PrintableHeight,
        [Parameter(Mandatory)] [int] $ToolCount,
        [Parameter(Mandatory)] [double] $LayerHeight,
        [double] $ProbeZ = 8.0,
        [ValidateRange(2, 100)]
        [int] $ProbeLayerCount = 2,
        [switch] $CaptureAllWallLayers
    )

    $area = if ($PrintableArea.PSObject.Properties['Points']) {
        $PrintableArea
    } else {
        Get-PrintableAreaInfo $PrintableArea
    }
    $probeLayers = Get-ProbeLayerNumbers -LayerHeight $LayerHeight -ProbeZ $ProbeZ -Count $ProbeLayerCount
    $probeLayerSet = @{}
    foreach ($probeLayer in $probeLayers) { $probeLayerSet[$probeLayer] = $true }

    $tool = -1
    $role = ''
    $width = 0.0
    $layer = -1
    $absoluteXY = $true
    $relativeExtrusion = $false
    $x = $null
    $y = $null
    $z = $null
    $e = 0.0
    $settings = @{}
    $lengthByLayerTool = @{}
    $lengthByRoleTool = @{}
    $lengthByTool = @{}
    $movesByTool = @{}
    $widthSamplesByTool = @{}
    $speedSamplesByTool = @{}
    $wallToolsByLayer = @{}
    $toolChangesByLayer = @{}
    $probeSegments = @{}
    $wallTools = [Collections.Generic.HashSet[int]]::new()
    $unsupportedToolCommands = 0
    $extrusionBeforeTool = 0
    $nonFiniteNumbers = 0
    $outOfBoundsExtrusions = 0
    $sparseInfillSeen = $false
    $currentFeed = 0.0
    $filamentArea = [Math]::PI * 1.75 * 1.75 / 4.0

    foreach ($line in [IO.File]::ReadLines([IO.Path]::GetFullPath($Path))) {
        if ($line -eq ';LAYER_CHANGE') {
            ++$layer
            continue
        }
        if ($line -match '^;TYPE:(.+)$') {
            $role = $Matches[1].Trim()
            if ($role -eq 'Sparse infill') { $sparseInfillSeen = $true }
            continue
        }
        if ($line -match '^;WIDTH:([0-9.]+)') {
            $width = [double]::Parse($Matches[1], $script:Invariant)
            continue
        }
        if ($line -match '^; ([a-z][a-z0-9_]*) = (.*)$') {
            $settings[$Matches[1]] = $Matches[2].Trim()
            continue
        }
        if ($line -match '^T(-?\d+)(?:\s|$)') {
            $newTool = [int] $Matches[1]
            if ($newTool -lt 0 -or $newTool -ge $ToolCount) { ++$unsupportedToolCommands }
            if ($layer -ge 0 -and $tool -ge 0 -and $newTool -ne $tool) {
                $toolChangesByLayer[$layer] = [int] $toolChangesByLayer[$layer] + 1
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
                $e = [double]::Parse($Matches[1], $script:Invariant)
            }
            continue
        }
        if ($line -notmatch '^G[0123](?:\s|$)') { continue }
        if ($line -match '(?i)(?:^|\s)[XYZEIJKRF][+-]?(?:nan|inf(?:inity)?)(?=\s|;|$)') {
            ++$nonFiniteNumbers
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
        foreach ($token in [regex]::Matches($line, '(?:^|\s)([XYZEF])(-?(?:\d+(?:\.\d*)?|\.\d+))')) {
            $axis = $token.Groups[1].Value
            $value = [double]::Parse($token.Groups[2].Value, $script:Invariant)
            switch ($axis) {
                'X' { $newX = if ($absoluteXY -or $null -eq $x) { $value } else { $x + $value }; $hasX = $true }
                'Y' { $newY = if ($absoluteXY -or $null -eq $y) { $value } else { $y + $value }; $hasY = $true }
                'Z' { $newZ = if ($absoluteXY -or $null -eq $z) { $value } else { $z + $value }; $hasZ = $true }
                'E' { $newE = $value; $hasE = $true }
                'F' { $currentFeed = $value }
            }
        }
        $extrusion = if (-not $hasE) { 0.0 } elseif ($relativeExtrusion) { $newE } else { $newE - $e }
        $positiveExtrusion = $extrusion -gt 0.0000001
        $isWall = $role -in @('Outer wall', 'Inner wall', 'Overhang wall')

        if ($positiveExtrusion) {
            if ($tool -lt 0 -or $tool -ge $ToolCount) {
                ++$extrusionBeforeTool
            }
            if (($null -ne $newX -and $null -ne $newY -and
                 -not (Test-PointInPolygon -X $newX -Y $newY -Points $area.Points)) -or
                ($null -ne $newZ -and ($newZ -lt -0.05 -or $newZ -gt $PrintableHeight + 0.05))) {
                ++$outOfBoundsExtrusions
            }
        }

        if ($positiveExtrusion -and $tool -ge 0 -and $tool -lt $ToolCount -and
            $null -ne $x -and $null -ne $y -and ($hasX -or $hasY)) {
            $dx = $newX - $x
            $dy = $newY - $y
            $length = [Math]::Sqrt($dx * $dx + $dy * $dy)
            if ($length -gt 0.005) {
                Add-MapNumber -Map $lengthByRoleTool -Key "$role|$tool" -Value $length
                if ($isWall -and $layer -ge 0) {
                    [void] $wallTools.Add($tool)
                    Add-MapNumber -Map $lengthByTool -Key ([string] $tool) -Value $length
                    Add-MapNumber -Map $lengthByLayerTool -Key "$layer|$tool" -Value $length
                    $movesByTool[[string] $tool] = [int] $movesByTool[[string] $tool] + 1
                    if (-not $wallToolsByLayer.ContainsKey($layer)) {
                        $wallToolsByLayer[$layer] = [Collections.Generic.HashSet[int]]::new()
                    }
                    [void] $wallToolsByLayer[$layer].Add($tool)
                    if (-not $widthSamplesByTool.ContainsKey($tool)) {
                        $widthSamplesByTool[$tool] = [Collections.Generic.List[double]]::new()
                        $speedSamplesByTool[$tool] = [Collections.Generic.List[double]]::new()
                    }
                    $sampleWidth = $width
                    if ($sampleWidth -le 0.0 -and $LayerHeight -gt 0.0) {
                        $areaFromExtrusion = $extrusion * $filamentArea / $length
                        $sampleWidth = $areaFromExtrusion / $LayerHeight + $LayerHeight * (1.0 - [Math]::PI / 4.0)
                    }
                    if ($sampleWidth -gt 0.05 -and $sampleWidth -lt 2.0 -and $widthSamplesByTool[$tool].Count -lt 50000) {
                        $widthSamplesByTool[$tool].Add($sampleWidth)
                    }
                    if ($currentFeed -gt 0 -and $speedSamplesByTool[$tool].Count -lt 50000) {
                        $speedSamplesByTool[$tool].Add($currentFeed / 60.0)
                    }
                    if ($CaptureAllWallLayers -or $probeLayerSet.ContainsKey($layer)) {
                        if (-not $probeSegments.ContainsKey($layer)) {
                            $probeSegments[$layer] = [Collections.Generic.List[object]]::new()
                        }
                        $probeSegments[$layer].Add([pscustomobject]@{
                            X1 = [double] $x
                            Y1 = [double] $y
                            X2 = [double] $newX
                            Y2 = [double] $newY
                            Tool = $tool
                            Width = $sampleWidth
                            Length = $length
                            Role = $role
                        })
                    }
                }
            }
        }

        if ($hasX) { $x = $newX }
        if ($hasY) { $y = $newY }
        if ($hasZ) { $z = $newZ }
        if ($hasE) { $e = $newE }
    }

    $medianWidths = @{}
    $medianSpeeds = @{}
    foreach ($entry in $widthSamplesByTool.GetEnumerator()) {
        $medianWidths[[string] $entry.Key] = Get-Median -Values $entry.Value.ToArray()
    }
    foreach ($entry in $speedSamplesByTool.GetEnumerator()) {
        $medianSpeeds[[string] $entry.Key] = Get-Median -Values $entry.Value.ToArray()
    }
    $normalizedWallToolsByLayer = @{}
    foreach ($entry in $wallToolsByLayer.GetEnumerator()) {
        $normalizedWallToolsByLayer[[string] $entry.Key] = @($entry.Value | Sort-Object)
    }

    $capturedProbeLayers = if ($CaptureAllWallLayers) {
        @($probeSegments.Keys | Sort-Object)
    } else {
        $probeLayers
    }

    return [pscustomobject]@{
        Settings = $settings
        WallTools = @($wallTools | Sort-Object)
        WallToolsByLayer = $normalizedWallToolsByLayer
        LengthByTool = $lengthByTool
        LengthByLayerTool = $lengthByLayerTool
        LengthByRoleTool = $lengthByRoleTool
        MovesByTool = $movesByTool
        MedianWidthByTool = $medianWidths
        MedianSpeedByTool = $medianSpeeds
        ProbeLayers = $capturedProbeLayers
        ProbeSegments = $probeSegments
        SparseInfillSeen = $sparseInfillSeen
        LayerCount = $layer + 1
        UnsupportedToolCommands = $unsupportedToolCommands
        ExtrusionBeforeTool = $extrusionBeforeTool
        NonFiniteNumbers = $nonFiniteNumbers
        OutOfBoundsExtrusions = $outOfBoundsExtrusions
        MaxToolChangesPerLayer = if ($toolChangesByLayer.Count -gt 0) {
            [int] ($toolChangesByLayer.Values | Measure-Object -Maximum).Maximum
        } else { 0 }
    }
}

function Get-ProbeCrossings {
    param(
        [Parameter(Mandatory)] $Segments,
        [Parameter(Mandatory)] [double] $BedCenterX,
        [Parameter(Mandatory)] [double] $BedCenterY,
        [double] $MergeTolerance = 0.035
    )
    # The STL is 120 x 80 mm and centered by Orca. The ray crosses the isolated
    # straight-wall body at model Y=23 and keeps only its left-side walls.
    $offsetX = $BedCenterX - 60.0
    $offsetY = $BedCenterY - 40.0
    $rayY = $offsetY + 23.0
    $minimumX = $offsetX + 5.0
    $maximumX = $offsetX + 25.0
    $raw = [Collections.Generic.List[object]]::new()
    foreach ($segment in $Segments) {
        $crosses = (($segment.Y1 -le $rayY -and $segment.Y2 -gt $rayY) -or
                    ($segment.Y2 -le $rayY -and $segment.Y1 -gt $rayY))
        if (-not $crosses) { continue }
        $ratio = ($rayY - $segment.Y1) / ($segment.Y2 - $segment.Y1)
        $crossX = $segment.X1 + $ratio * ($segment.X2 - $segment.X1)
        if ($crossX -lt $minimumX -or $crossX -gt $maximumX) { continue }
        $raw.Add([pscustomobject]@{
            X = $crossX
            Tool = [int] $segment.Tool
            Width = [double] $segment.Width
        })
    }
    $merged = [Collections.Generic.List[object]]::new()
    foreach ($crossing in @($raw | Sort-Object X)) {
        if ($merged.Count -eq 0 -or
            [Math]::Abs($crossing.X - $merged[$merged.Count - 1].X) -gt $MergeTolerance) {
            $merged.Add($crossing)
            continue
        }
        $previous = $merged[$merged.Count - 1]
        if ($previous.Tool -ne $crossing.Tool) {
            $merged.Add($crossing)
        }
    }
    return $merged.ToArray()
}

function Get-ProbeLoopTopology {
    param(
        [Parameter(Mandatory)] $Segments,
        [Parameter(Mandatory)] [double] $BedCenterX,
        [Parameter(Mandatory)] [double] $BedCenterY,
        [double] $Quantization = 0.025,
        [switch] $AllSegments
    )
    $offsetX = $BedCenterX - 60.0
    $offsetY = $BedCenterY - 40.0
    $x0 = $offsetX + 6.5
    $x1 = $offsetX + 44.5
    $y0 = $offsetY + 6.5
    $y1 = $offsetY + 39.5
    $byTool = @{}
    foreach ($segment in $Segments) {
        if (-not $AllSegments -and
            ($segment.X1 -lt $x0 -or $segment.X1 -gt $x1 -or
             $segment.X2 -lt $x0 -or $segment.X2 -gt $x1 -or
             $segment.Y1 -lt $y0 -or $segment.Y1 -gt $y1 -or
             $segment.Y2 -lt $y0 -or $segment.Y2 -gt $y1)) { continue }
        $toolKey = [string] $segment.Tool
        if (-not $byTool.ContainsKey($toolKey)) {
            $byTool[$toolKey] = [Collections.Generic.List[object]]::new()
        }
        $byTool[$toolKey].Add($segment)
    }

    $topology = @{}
    foreach ($entry in $byTool.GetEnumerator()) {
        $adjacency = @{}
        $toolWidths = [Collections.Generic.List[double]]::new()
        foreach ($segment in $entry.Value) {
            if ($segment.Width -gt 0) { $toolWidths.Add([double] $segment.Width) }
            $a = '{0},{1}' -f [Math]::Round($segment.X1 / $Quantization), [Math]::Round($segment.Y1 / $Quantization)
            $b = '{0},{1}' -f [Math]::Round($segment.X2 / $Quantization), [Math]::Round($segment.Y2 / $Quantization)
            if ($a -eq $b) { continue }
            if (-not $adjacency.ContainsKey($a)) { $adjacency[$a] = [Collections.Generic.List[string]]::new() }
            if (-not $adjacency.ContainsKey($b)) { $adjacency[$b] = [Collections.Generic.List[string]]::new() }
            $adjacency[$a].Add($b)
            $adjacency[$b].Add($a)
        }
        $visited = [Collections.Generic.HashSet[string]]::new()
        $components = 0
        $closedComponents = 0
        $openEndpoints = 0
        $endpointKeys = [Collections.Generic.List[string]]::new()
        foreach ($start in $adjacency.Keys) {
            if ($visited.Contains($start)) { continue }
            ++$components
            $queue = [Collections.Generic.Queue[string]]::new()
            $queue.Enqueue($start)
            [void] $visited.Add($start)
            $componentOpen = 0
            while ($queue.Count -gt 0) {
                $node = $queue.Dequeue()
                $degree = $adjacency[$node].Count
                if ($degree -eq 1) {
                    ++$componentOpen
                    $endpointKeys.Add($node)
                }
                foreach ($next in $adjacency[$node]) {
                    if ($visited.Add($next)) { $queue.Enqueue($next) }
                }
            }
            $openEndpoints += $componentOpen
            if ($componentOpen -eq 0) { ++$closedComponents }
        }
        $seamTolerance = [Math]::Max(0.08, (Get-Median -Values $toolWidths.ToArray()) * 1.5)
        $remaining = [Collections.Generic.List[string]]::new()
        foreach ($key in $endpointKeys) { $remaining.Add($key) }
        $maximumPairedGap = 0.0
        $unpairedOpenEndpoints = 0
        while ($remaining.Count -gt 1) {
            $a = $remaining[0]
            $aParts = $a.Split(',')
            $nearestIndex = -1
            $nearestDistance = [double]::PositiveInfinity
            for ($i = 1; $i -lt $remaining.Count; ++$i) {
                $bParts = $remaining[$i].Split(',')
                $dx = ([double] $aParts[0] - [double] $bParts[0]) * $Quantization
                $dy = ([double] $aParts[1] - [double] $bParts[1]) * $Quantization
                $distance = [Math]::Sqrt($dx * $dx + $dy * $dy)
                if ($distance -lt $nearestDistance) {
                    $nearestDistance = $distance
                    $nearestIndex = $i
                }
            }
            if ($nearestIndex -lt 0 -or $nearestDistance -gt $seamTolerance) {
                ++$unpairedOpenEndpoints
                $remaining.RemoveAt(0)
                continue
            }
            $maximumPairedGap = [Math]::Max($maximumPairedGap, $nearestDistance)
            $remaining.RemoveAt($nearestIndex)
            $remaining.RemoveAt(0)
        }
        $unpairedOpenEndpoints += $remaining.Count
        $topology[$entry.Key] = [pscustomobject]@{
            Components = $components
            ClosedComponents = $closedComponents
            RawOpenEndpoints = $openEndpoints
            UnpairedOpenEndpoints = $unpairedOpenEndpoints
            MaximumPairedSeamGap = [Math]::Round($maximumPairedGap, 4)
            SeamTolerance = [Math]::Round($seamTolerance, 4)
        }
    }
    return $topology
}

function Get-ProbeSectionMetrics {
    param(
        [Parameter(Mandatory)] $Analysis,
        [Parameter(Mandatory)] $PrintableArea,
        [Parameter(Mandatory)] [int] $SmallTool,
        [Parameter(Mandatory)] [int] $LargeTool
    )
    $area = if ($PrintableArea.PSObject.Properties['Points']) {
        $PrintableArea
    } else {
        Get-PrintableAreaInfo $PrintableArea
    }
    $layers = [Collections.Generic.List[object]]::new()
    foreach ($layer in $Analysis.ProbeLayers) {
        $segments = $Analysis.ProbeSegments[$layer]
        $crossings = if ($null -eq $segments) {
            @()
        } else {
            @(Get-ProbeCrossings -Segments $segments -BedCenterX $area.CenterX -BedCenterY $area.CenterY)
        }
        $small = @($crossings | Where-Object Tool -eq $SmallTool)
        $large = @($crossings | Where-Object Tool -eq $LargeTool)
        $topology = if ($null -eq $segments) {
            @{}
        } else {
            Get-ProbeLoopTopology -Segments $segments -BedCenterX $area.CenterX -BedCenterY $area.CenterY
        }
        $smallSpacings = [Collections.Generic.List[double]]::new()
        $largeSpacings = [Collections.Generic.List[double]]::new()
        $boundarySpacings = [Collections.Generic.List[double]]::new()
        for ($i = 1; $i -lt $crossings.Count; ++$i) {
            $spacing = [Math]::Abs($crossings[$i].X - $crossings[$i - 1].X)
            if ($spacing -le 0.05 -or $spacing -gt 1.5) { continue }
            if ($crossings[$i].Tool -ne $crossings[$i - 1].Tool) {
                $boundarySpacings.Add($spacing)
            } elseif ($crossings[$i].Tool -eq $SmallTool) {
                $smallSpacings.Add($spacing)
            } elseif ($crossings[$i].Tool -eq $LargeTool) {
                $largeSpacings.Add($spacing)
            }
        }
        $layers.Add([pscustomobject]@{
            Layer = $layer
            SmallCount = $small.Count
            LargeCount = $large.Count
            TotalCount = $crossings.Count
            ToolSequence = @($crossings | ForEach-Object { [int] $_.Tool })
            ToolTransitions = @(
                for ($index = 1; $index -lt $crossings.Count; ++$index) {
                    if ($crossings[$index].Tool -ne $crossings[$index - 1].Tool) { 1 }
                }
            ).Count
            SmallSpacing = Get-Median -Values $smallSpacings.ToArray()
            BoundarySpacing = Get-Median -Values $boundarySpacings.ToArray()
            LargeSpacing = Get-Median -Values $largeSpacings.ToArray()
            Topology = $topology
            Crossings = @($crossings)
        })
    }
    return [pscustomobject]@{ Layers = $layers.ToArray() }
}

function Get-ProbeZoneMetrics {
    param(
        [Parameter(Mandatory)] $Analysis,
        [Parameter(Mandatory)] $PrintableArea,
        [Parameter(Mandatory)] [int] $SmallTool,
        [Parameter(Mandatory)] [int] $LargeTool
    )
    $area = if ($PrintableArea.PSObject.Properties['Points']) {
        $PrintableArea
    } else {
        Get-PrintableAreaInfo $PrintableArea
    }
    $offsetX = $area.CenterX - 60.0
    $offsetY = $area.CenterY - 40.0
    $zones = @(
        @{ Name='straight'; X0=7; Y0=7; X1=44; Y1=39 },
        @{ Name='angles'; X0=50; Y0=7; X1=114; Y1=24 },
        @{ Name='concave'; X0=50; Y0=28; X1=83; Y1=58 },
        @{ Name='curves'; X0=84; Y0=26; X1=115; Y1=58 },
        @{ Name='letter'; X0=49; Y0=62; X1=65; Y1=77 },
        @{ Name='thin'; X0=69; Y0=62; X1=101; Y1=77 },
        @{ Name='steps'; X0=6; Y0=44; X1=43; Y1=74 }
    )
    $result = @{}
    foreach ($zone in $zones) {
        $smallLength = 0.0
        $largeLength = 0.0
        foreach ($layer in $Analysis.ProbeLayers) {
            foreach ($segment in @($Analysis.ProbeSegments[$layer])) {
                $midX = (($segment.X1 + $segment.X2) / 2.0) - $offsetX
                $midY = (($segment.Y1 + $segment.Y2) / 2.0) - $offsetY
                if ($midX -lt $zone.X0 -or $midX -gt $zone.X1 -or
                    $midY -lt $zone.Y0 -or $midY -gt $zone.Y1) { continue }
                if ($segment.Tool -eq $SmallTool) { $smallLength += $segment.Length }
                if ($segment.Tool -eq $LargeTool) { $largeLength += $segment.Length }
            }
        }
        $result[$zone.Name] = [pscustomobject]@{
            SmallLength = [Math]::Round($smallLength, 3)
            LargeLength = [Math]::Round($largeLength, 3)
        }
    }
    return $result
}

function Get-ExpectedBoundarySpacing {
    param(
        [double] $SmallWidth,
        [double] $LargeWidth,
        [double] $LayerHeight,
        [double] $OverlapPercent
    )
    $shapeCorrection = $LayerHeight * (1.0 - [Math]::PI / 4.0)
    $smallSpacing = $SmallWidth - $shapeCorrection
    $largeSpacing = $LargeWidth - $shapeCorrection
    return 0.5 * ($smallSpacing + $largeSpacing) -
        ($OverlapPercent / 100.0) * [Math]::Min($smallSpacing, $largeSpacing)
}

function Test-SmallNozzleCase {
    param(
        [Parameter(Mandatory)] $Case,
        [Parameter(Mandatory)] $Analysis,
        [Parameter(Mandatory)] $PrintableArea
    )
    $errors = [Collections.Generic.List[string]]::new()
    $warnings = [Collections.Generic.List[string]]::new()
    $smallTool = if ([double] $Case.Nozzle2 -lt 0.4) { 1 } else { 0 }
    $largeTool = if ([double] $Case.Nozzle2 -lt 0.4) { 0 } else { 1 }
    $baseTool = [int] $Case.BaseTool - 1

    if ($Analysis.UnsupportedToolCommands -gt 0) { $errors.Add("Unsupported tool commands: $($Analysis.UnsupportedToolCommands)") }
    if ($Analysis.ExtrusionBeforeTool -gt 0) { $errors.Add("Extrusions without a selected tool: $($Analysis.ExtrusionBeforeTool)") }
    if ($Analysis.NonFiniteNumbers -gt 0) { $errors.Add("Non-finite motion values: $($Analysis.NonFiniteNumbers)") }
    if ($Analysis.OutOfBoundsExtrusions -gt 0) { $errors.Add("Out-of-bounds extrusions: $($Analysis.OutOfBoundsExtrusions)") }
    if ($Analysis.MaxToolChangesPerLayer -gt 16) { $errors.Add("Excessive tool changes per layer: $($Analysis.MaxToolChangesPerLayer)") }
    if ($Analysis.Settings['wall_generator'] -ne $Case.Generator) { $errors.Add('Embedded wall_generator mismatch') }
    if ($Analysis.Settings['crisp_corner_small_nozzle_wall_count'] -ne [string] $Case.Count) {
        $errors.Add('Embedded small-nozzle wall count mismatch')
    }

    $section = Get-ProbeSectionMetrics -Analysis $Analysis -PrintableArea $PrintableArea -SmallTool $smallTool -LargeTool $largeTool
    $zones = Get-ProbeZoneMetrics -Analysis $Analysis -PrintableArea $PrintableArea -SmallTool $smallTool -LargeTool $largeTool
    $expectedProbeLayers = if ($null -ne $Case.PSObject.Properties['ProbeLayerCount']) {
        [int] $Case.ProbeLayerCount
    } else {
        2
    }
    if ($section.Layers.Count -ne $expectedProbeLayers -or
        @($section.Layers | Where-Object TotalCount -gt 0).Count -ne $expectedProbeLayers) {
        $errors.Add("Expected $expectedProbeLayers probe layers with straight-wall intersections, got $($section.Layers.Count)")
    }

    if (-not $Case.Enabled) {
        if ($Case.OverrideRanges.Count -eq 0 -and $Analysis.WallTools -contains $smallTool) {
            $errors.Add("Disabled mode emitted walls on detail T$smallTool")
        }
        if ($Analysis.WallTools -notcontains $baseTool) { $errors.Add("Disabled mode omitted base T$baseTool") }
    } else {
        if ($Analysis.WallTools -notcontains $smallTool) { $errors.Add("Detail T$smallTool was not used for walls") }
        if ($Case.RequireLarge -and $Analysis.WallTools -notcontains $largeTool) {
            $errors.Add("Large-nozzle T$largeTool was not used for walls")
        }
        $smallWidth = [double] $Analysis.MedianWidthByTool[[string] $smallTool]
        $largeWidth = [double] $Analysis.MedianWidthByTool[[string] $largeTool]
        if ($smallWidth -le 0 -or ($Case.RequireLarge -and $largeWidth -le 0)) {
            $errors.Add('Insufficient wall-width samples')
        } elseif ($Case.RequireLarge -and $smallWidth -ge $largeWidth) {
            $errors.Add("Detail width $smallWidth is not smaller than large width $largeWidth")
        }
    }

    foreach ($check in $Case.OverrideChecks) {
        $layerKey = [string] ([int] $check.Layer)
        $tools = @($Analysis.WallToolsByLayer[$layerKey])
        if ($tools -notcontains [int] $check.Tool) {
            $errors.Add("Override layer $([int]$check.Layer + 1) omitted T$($check.Tool)")
        }
        $unexpected = @($tools | Where-Object { $_ -ne [int] $check.Tool })
        if ($unexpected.Count -gt 0) {
            $errors.Add("Override layer $([int]$check.Layer + 1) also used T$($unexpected -join ',T')")
        }
    }

    if ($Case.StrictCounts -and $section.Layers.Count -eq $expectedProbeLayers) {
        $expectedSmall = [Math]::Max(1, [int] $Case.Count)
        $expectedLarge = if ($Case.Enabled -and $Case.RequireLarge) {
            [Math]::Max(2, [int] $Case.WallLoops)
        } else {
            [int] $Case.WallLoops
        }
        if (-not $Case.Enabled) {
            foreach ($layerMetric in $section.Layers) {
                if ($layerMetric.SmallCount -ne 0 -or $layerMetric.LargeCount -ne $expectedLarge) {
                    $errors.Add("Layer $($layerMetric.Layer): expected large=$expectedLarge small=0, got large=$($layerMetric.LargeCount) small=$($layerMetric.SmallCount)")
                }
            }
        } elseif ($Case.Interlock) {
            # The required detail loop must never disappear. With one small
            # wall there is no safe boundary wall to exchange.
            $alternateLarge = if ($expectedSmall -gt 1) { $expectedLarge + 1 } else { $expectedLarge }
            $alternateSmall = if ($expectedSmall -gt 1) { $expectedSmall - 1 } else { $expectedSmall }
            $basePair = "$expectedLarge`:$expectedSmall"
            $alternatePair = "$alternateLarge`:$alternateSmall"
            $actualPairs = @($section.Layers | ForEach-Object { "$($_.LargeCount):$($_.SmallCount)" })
            $firstIsBase = $actualPairs.Count -gt 0 -and $actualPairs[0] -eq $basePair
            $firstIsAlternate = $actualPairs.Count -gt 0 -and $actualPairs[0] -eq $alternatePair
            if (-not $firstIsBase -and -not $firstIsAlternate) {
                $errors.Add("Interlock first layer expected $basePair or $alternatePair, got $($actualPairs[0])")
            } else {
                for ($index = 0; $index -lt $actualPairs.Count; ++$index) {
                    $basePhase = if ($firstIsBase) { ($index % 2) -eq 0 } else { ($index % 2) -eq 1 }
                    $expectedPair = if ($basePhase) { $basePair } else { $alternatePair }
                    if ($actualPairs[$index] -ne $expectedPair) {
                        $errors.Add("Interlock layer $($section.Layers[$index].Layer): expected $expectedPair, got $($actualPairs[$index])")
                    }
                    if ($section.Layers[$index].TotalCount -ne ($expectedLarge + $expectedSmall)) {
                        $errors.Add("Interlock layer $($section.Layers[$index].Layer): total wall count changed to $($section.Layers[$index].TotalCount)")
                    }
                }
            }
        } elseif ($Case.Enabled) {
            foreach ($layerMetric in $section.Layers) {
                if ($layerMetric.SmallCount -ne $expectedSmall -or $layerMetric.LargeCount -ne $expectedLarge) {
                    $errors.Add("Layer $($layerMetric.Layer): expected large=$expectedLarge small=$expectedSmall, got large=$($layerMetric.LargeCount) small=$($layerMetric.SmallCount)")
                }
            }
        }
    }

    foreach ($layerMetric in $section.Layers) {
        if ($Case.Enabled -and $Case.RequireLarge -and
            $layerMetric.SmallCount -gt 0 -and $layerMetric.LargeCount -gt 0) {
            $expectedToolSequence = @(
                @(1..$layerMetric.SmallCount | ForEach-Object { $smallTool })
                @(1..$layerMetric.LargeCount | ForEach-Object { $largeTool })
            )
            if (($layerMetric.ToolSequence -join ',') -ne ($expectedToolSequence -join ',')) {
                $errors.Add("Layer $($layerMetric.Layer): wall tool order is $($layerMetric.ToolSequence -join ','), expected $($expectedToolSequence -join ',')")
            }
            if ($layerMetric.ToolTransitions -ne 1) {
                $errors.Add("Layer $($layerMetric.Layer): expected one small/large wall boundary, got $($layerMetric.ToolTransitions)")
            }
        }
        foreach ($toolKey in @([string] $smallTool, [string] $largeTool)) {
            $toolTopology = $layerMetric.Topology[$toolKey]
            $toolCount = if ([int] $toolKey -eq $smallTool) { $layerMetric.SmallCount } else { $layerMetric.LargeCount }
            if ($toolCount -gt 0 -and $null -ne $toolTopology -and $toolTopology.UnpairedOpenEndpoints -gt 0) {
                $errors.Add("Layer $($layerMetric.Layer) T$toolKey has $($toolTopology.UnpairedOpenEndpoints) unpaired open endpoints in the straight loop body")
            }
        }
    }

    if ($Case.Enabled -and $Case.RequireLarge -and $section.Layers.Count -gt 0) {
        $boundaryValues = @($section.Layers.BoundarySpacing | Where-Object { $_ -gt 0 })
        if ($boundaryValues.Count -eq 0) {
            $errors.Add('No small/large boundary spacing sample was found')
        } else {
            $actualBoundary = Get-Median -Values $boundaryValues
            $smallWidth = [double] $Analysis.MedianWidthByTool[[string] $smallTool]
            $largeWidth = [double] $Analysis.MedianWidthByTool[[string] $largeTool]
            $expectedBoundary = Get-ExpectedBoundarySpacing -SmallWidth $smallWidth -LargeWidth $largeWidth `
                -LayerHeight $Case.LayerHeight -OverlapPercent $Case.Overlap
            $tolerance = [Math]::Max(0.02, [Math]::Min($smallWidth, $largeWidth) * 0.15)
            if ([Math]::Abs($actualBoundary - $expectedBoundary) -gt $tolerance) {
                $errors.Add("Boundary spacing $([Math]::Round($actualBoundary,3)) mm differs from expected $([Math]::Round($expectedBoundary,3)) mm")
            }
        }
    }

    foreach ($role in @('Bottom surface', 'Top surface', 'Internal solid infill', 'Sparse infill')) {
        $smallRoleLength = [double] $Analysis.LengthByRoleTool["$role|$smallTool"]
        if ($Case.Enabled -and $smallRoleLength -gt 0.01 -and $baseTool -ne $smallTool) {
            $errors.Add("$role incorrectly used detail T$smallTool ($([Math]::Round($smallRoleLength,2)) mm)")
        }
    }
    if ($Case.InfillDensity -gt 0 -and -not $Analysis.SparseInfillSeen) {
        $errors.Add('Sparse infill was expected but not emitted')
    }

    $speedSetting = [string] $Case.Speed
    if ($Case.Enabled -and $speedSetting -ne '0') {
        $smallSpeed = [double] $Analysis.MedianSpeedByTool[[string] $smallTool]
        $largeSpeed = [double] $Analysis.MedianSpeedByTool[[string] $largeTool]
        if ($smallSpeed -le 0) {
            $errors.Add('Small-nozzle wall speed could not be measured')
        } elseif ($speedSetting.EndsWith('%') -and $largeSpeed -gt 0) {
            $ratio = [double]::Parse($speedSetting.TrimEnd('%'), $script:Invariant) / 100.0
            if ($smallSpeed -gt $largeSpeed * ($ratio + 0.15)) {
                $errors.Add("Small-wall median speed $([Math]::Round($smallSpeed,1)) exceeds the $speedSetting override envelope")
            }
        } elseif (-not $speedSetting.EndsWith('%')) {
            $limit = [double]::Parse($speedSetting, $script:Invariant)
            if ($smallSpeed -gt $limit + 1.0) {
                $errors.Add("Small-wall median speed $([Math]::Round($smallSpeed,1)) exceeds $limit mm/s")
            }
        }
    }

    if ($Case.Enabled -and $zones.Count -gt 0) {
        $smallZoneCount = @($zones.Values | Where-Object SmallLength -gt 0.01).Count
        if ($smallZoneCount -eq 0) { $errors.Add('No probe feature zone used the detail tool') }
        if ($Case.RequireLarge -and @($zones.Values | Where-Object LargeLength -gt 0.01).Count -eq 0) {
            $errors.Add('No probe feature zone retained large-nozzle walls')
        }
    }

    return [pscustomobject]@{
        Passed = $errors.Count -eq 0
        Errors = $errors.ToArray()
        Warnings = $warnings.ToArray()
        SmallTool = $smallTool
        LargeTool = $largeTool
        Section = $section
        Zones = $zones
    }
}

function Test-SmallNozzleComplexCase {
    param(
        [Parameter(Mandatory)] $Case,
        [Parameter(Mandatory)] $Analysis
    )
    $errors = [Collections.Generic.List[string]]::new()
    $warnings = [Collections.Generic.List[string]]::new()
    $smallTool = if ([double] $Case.Nozzle2 -lt 0.4) { 1 } else { 0 }
    $largeTool = if ([double] $Case.Nozzle2 -lt 0.4) { 0 } else { 1 }
    $baseTool = [int] $Case.BaseTool - 1

    if ($Analysis.UnsupportedToolCommands -gt 0) { $errors.Add("Unsupported tool commands: $($Analysis.UnsupportedToolCommands)") }
    if ($Analysis.ExtrusionBeforeTool -gt 0) { $errors.Add("Extrusions without a selected tool: $($Analysis.ExtrusionBeforeTool)") }
    if ($Analysis.NonFiniteNumbers -gt 0) { $errors.Add("Non-finite motion values: $($Analysis.NonFiniteNumbers)") }
    if ($Analysis.OutOfBoundsExtrusions -gt 0) { $errors.Add("Out-of-bounds extrusions: $($Analysis.OutOfBoundsExtrusions)") }
    if ($Analysis.MaxToolChangesPerLayer -gt 16) { $errors.Add("Excessive tool changes per layer: $($Analysis.MaxToolChangesPerLayer)") }
    if ($Analysis.Settings['wall_generator'] -ne $Case.Generator) { $errors.Add('Embedded wall_generator mismatch') }
    if ($Analysis.Settings['crisp_corner_small_nozzle_wall_count'] -ne [string] $Case.Count) {
        $errors.Add('Embedded small-nozzle wall count mismatch')
    }
    $embeddedInterlock = $Analysis.Settings['crisp_corner_interlace_small_nozzle_walls']
    $expectedInterlock = if ($Case.Interlock) { '1' } else { '0' }
    if ($embeddedInterlock -ne $expectedInterlock) { $errors.Add('Embedded interlocking setting mismatch') }

    if ($Analysis.WallTools -notcontains $smallTool) { $errors.Add("Detail T$smallTool was not used for bunny walls") }
    if ($Analysis.WallTools -notcontains $largeTool) { $errors.Add("Large-nozzle T$largeTool was not used for bunny walls") }
    $smallWidth = [double] $Analysis.MedianWidthByTool[[string] $smallTool]
    $largeWidth = [double] $Analysis.MedianWidthByTool[[string] $largeTool]
    if ($smallWidth -le 0 -or $largeWidth -le 0) {
        $errors.Add('Insufficient bunny wall-width samples')
    } elseif ($smallWidth -ge $largeWidth) {
        $errors.Add("Detail width $smallWidth is not smaller than large width $largeWidth")
    }

    foreach ($role in @('Bottom surface', 'Top surface', 'Internal solid infill', 'Sparse infill')) {
        $smallRoleLength = [double] $Analysis.LengthByRoleTool["$role|$smallTool"]
        if ($smallRoleLength -gt 0.01 -and $baseTool -ne $smallTool) {
            $errors.Add("$role incorrectly used detail T$smallTool ($([Math]::Round($smallRoleLength,2)) mm)")
        }
    }

    $layers = [Collections.Generic.List[object]]::new()
    $totalSmallLength = 0.0
    $totalLargeLength = 0.0
    $mixedLayers = 0
    $topologyWarnings = 0
    $wallLayers = @($Analysis.WallToolsByLayer.Keys | ForEach-Object { [int] $_ } | Sort-Object)
    foreach ($layer in $wallLayers) {
        $smallLength = [double] $Analysis.LengthByLayerTool["$layer|$smallTool"]
        $largeLength = [double] $Analysis.LengthByLayerTool["$layer|$largeTool"]
        if ($smallLength -gt 0.01 -and $largeLength -gt 0.01) { ++$mixedLayers }
        $totalSmallLength += $smallLength
        $totalLargeLength += $largeLength
        $layers.Add([pscustomobject]@{
            Layer = [int] $layer
            SmallLength = [Math]::Round($smallLength, 4)
            LargeLength = [Math]::Round($largeLength, 4)
        })
    }

    # Full-model topology is quadratic on complex meshes. Check only the
    # deterministic probe layers while all-layer wall ownership stays streamed.
    foreach ($layer in $Analysis.ProbeLayers) {
        $segments = @($Analysis.ProbeSegments[$layer])
        if ($segments.Count -eq 0) { continue }
        $topology = Get-ProbeLoopTopology -Segments $segments -BedCenterX 0 -BedCenterY 0 -AllSegments
        $unpaired = 0
        foreach ($toolKey in @([string] $smallTool, [string] $largeTool)) {
            if ($null -ne $topology[$toolKey]) { $unpaired += [int] $topology[$toolKey].UnpairedOpenEndpoints }
        }
        if ($unpaired -gt 0) { ++$topologyWarnings }
    }

    $wallLength = $totalSmallLength + $totalLargeLength
    if ($layers.Count -lt 10) { $errors.Add("Too few bunny wall layers were captured: $($layers.Count)") }
    if ($mixedLayers -lt 3) { $errors.Add("Too few mixed-tool bunny wall layers: $mixedLayers") }
    if ($wallLength -le 0) {
        $errors.Add('No bunny wall length was measured')
    } else {
        $smallShare = $totalSmallLength / $wallLength
        if ($smallShare -le 0.01) { $errors.Add("Detail-wall share is too small: $([Math]::Round(100*$smallShare,2))%") }
        if ($smallShare -ge 0.95) { $errors.Add("Detail tool captured nearly all bunny walls: $([Math]::Round(100*$smallShare,2))%") }
    }
    if ($topologyWarnings -gt 0) {
        $warnings.Add("$topologyWarnings sampled layers contain open wall endpoints; retained for Classic/Arachne comparison")
    }

    return [pscustomobject]@{
        Passed = $errors.Count -eq 0
        Errors = $errors.ToArray()
        Warnings = $warnings.ToArray()
        SmallTool = $smallTool
        LargeTool = $largeTool
        Complex = [pscustomobject]@{
            Layers = $layers.ToArray()
            WallLayers = $layers.Count
            MixedLayers = $mixedLayers
            TotalSmallLength = [Math]::Round($totalSmallLength, 3)
            TotalLargeLength = [Math]::Round($totalLargeLength, 3)
            SmallLengthShare = if ($wallLength -gt 0) { [Math]::Round($totalSmallLength / $wallLength, 5) } else { 0 }
            SampledLayersWithOpenEndpoints = $topologyWarnings
        }
    }
}

function Invoke-SmallNozzleVerifierSelfTest {
    param([string] $WorkingDirectory)
    if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $WorkingDirectory = Join-Path ([IO.Path]::GetTempPath()) 'small-nozzle-verifier-selftest'
    }
    New-Item -ItemType Directory -Force -Path $WorkingDirectory | Out-Null
    $path = Join-Path $WorkingDirectory 'synthetic_interlock.gcode'
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('; wall_generator = classic')
    $lines.Add('; crisp_corner_small_nozzle_wall_count = 3')
    $lines.Add('G90')
    $lines.Add('M83')
    $offsetX = 75.0
    $offsetY = 95.0
    $layouts = @(
        @{ Small=3; Large=3 },
        @{ Small=2; Large=4 },
        @{ Small=3; Large=3 },
        @{ Small=2; Large=4 },
        @{ Small=3; Large=3 },
        @{ Small=2; Large=4 },
        @{ Small=3; Large=3 },
        @{ Small=2; Large=4 }
    )
    for ($layoutIndex = 0; $layoutIndex -lt $layouts.Count; ++$layoutIndex) {
        $layout = $layouts[$layoutIndex]
        $lines.Add(';LAYER_CHANGE')
        $lines.Add('G1 Z0.1 F600')
        $inset = 0.10
        foreach ($toolSpec in @(
            @{ Tool=1; Count=$layout.Small; Width=0.17 },
            @{ Tool=0; Count=$layout.Large; Width=0.45 }
        )) {
            $lines.Add("T$($toolSpec.Tool)")
            # Orca may omit TYPE at the start of a layer when the role did not
            # change. The parser must preserve the previous role in that case.
            if ($layoutIndex -eq 0 -or $toolSpec.Tool -eq 0) {
                $lines.Add(';TYPE:Outer wall')
            }
            $lines.Add(";WIDTH:$($toolSpec.Width)")
            for ($i = 0; $i -lt $toolSpec.Count; ++$i) {
                $x0 = $offsetX + 7 + $inset
                $x1 = $offsetX + 44 - $inset
                $y0 = $offsetY + 7 + $inset
                $y1 = $offsetY + 39 - $inset
                $lines.Add("G1 X$x0 Y$y0 F1800")
                $lines.Add("G1 X$x1 Y$y0 E1")
                $lines.Add("G1 X$x1 Y$y1 E1")
                $lines.Add("G1 X$x0 Y$y1 E1")
                $lines.Add("G1 X$x0 Y$y0 E1")
                $inset += if ($toolSpec.Tool -eq 1) { 0.17 } else { 0.45 }
            }
        }
    }
    [IO.File]::WriteAllLines($path, $lines, [Text.UTF8Encoding]::new($false))
    $printableArea = @('0x0','270x0','270x270','0x270')
    $analysis = Get-SmallNozzleGcodeAnalysis -Path $path -PrintableArea $printableArea `
        -PrintableHeight 270 -ToolCount 4 -LayerHeight 0.1 -ProbeZ 0.1 -ProbeLayerCount 8
    $section = Get-ProbeSectionMetrics -Analysis $analysis -PrintableArea $printableArea -SmallTool 1 -LargeTool 0
    $pairs = @($section.Layers | ForEach-Object { "$($_.LargeCount):$($_.SmallCount)" })
    $expected = @('3:3','4:2','3:3','4:2','3:3','4:2','3:3','4:2')
    $sequences = @($section.Layers | ForEach-Object { $_.ToolSequence -join ',' })
    $expectedSequences = @('1,1,1,0,0,0','1,1,0,0,0,0','1,1,1,0,0,0','1,1,0,0,0,0',
                           '1,1,1,0,0,0','1,1,0,0,0,0','1,1,1,0,0,0','1,1,0,0,0,0')
    $passed = ($pairs -join '|') -eq ($expected -join '|') -and
              ($sequences -join '|') -eq ($expectedSequences -join '|')
    return [pscustomobject]@{
        Passed = $passed
        Expected = $expected
        Actual = $pairs
        ExpectedSequences = $expectedSequences
        ActualSequences = $sequences
        Gcode = $path
    }
}

Export-ModuleMember -Function @(
    'Get-PrintableAreaInfo',
    'Get-ProbeLayerNumbers',
    'Get-SmallNozzleGcodeAnalysis',
    'Get-ProbeSectionMetrics',
    'Get-ProbeZoneMetrics',
    'Test-SmallNozzleCase',
    'Test-SmallNozzleComplexCase',
    'Invoke-SmallNozzleVerifierSelfTest'
)
