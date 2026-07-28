param(
    [Parameter(Mandatory = $true)]
    [string] $Gcode
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$gcodePath = (Resolve-Path $Gcode).Path

$layer = -1
$role = ''
$absoluteXY = $true
$relativeExtrusion = $false
$x = $null
$y = $null
$e = 0.0
$angleLengths = @{}
$supportSegments = 0

foreach ($line in [IO.File]::ReadLines($gcodePath)) {
    if ($line -eq ';LAYER_CHANGE') { $layer++; continue }
    if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
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
    if ($line -notmatch '^G[01](?:\s|$)') { continue }

    $newX = $x
    $newY = $y
    $newE = $e
    $hasX = $false
    $hasY = $false
    $hasE = $false
    foreach ($token in [regex]::Matches($line, '(?:^|\s)([XYE])(-?(?:\d+(?:\.\d*)?|\.\d+))')) {
        $axis = $token.Groups[1].Value
        $value = [double]::Parse($token.Groups[2].Value, $Invariant)
        switch ($axis) {
            'X' { $newX = if ($absoluteXY -or $null -eq $x) { $value } else { $x + $value }; $hasX = $true }
            'Y' { $newY = if ($absoluteXY -or $null -eq $y) { $value } else { $y + $value }; $hasY = $true }
            'E' { $newE = $value; $hasE = $true }
        }
    }

    $extrusion = if (-not $hasE) { 0.0 } elseif ($relativeExtrusion) { $newE } else { $newE - $e }
    if (
        $role -eq 'Support' -and $extrusion -gt 0.0000001 -and
        $null -ne $x -and $null -ne $y -and ($hasX -or $hasY)
    ) {
        $dx = $newX - $x
        $dy = $newY - $y
        $length = [Math]::Sqrt($dx * $dx + $dy * $dy)
        $supportSegments++
        # Ignore short curved boundary fragments. Grid infill is identified by
        # two substantial crossing line directions in the same physical layer.
        if ($length -ge 5.0) {
            $angle = [Math]::Atan2($dy, $dx) * 180.0 / [Math]::PI
            while ($angle -lt 0) { $angle += 180.0 }
            while ($angle -ge 180) { $angle -= 180.0 }
            $bin = [int]([Math]::Round($angle / 5.0) * 5) % 180
            if (-not $angleLengths.ContainsKey($layer)) { $angleLengths[$layer] = @{} }
            $angleLengths[$layer][$bin] = [double] $angleLengths[$layer][$bin] + $length
        }
    }

    if ($hasX) { $x = $newX }
    if ($hasY) { $y = $newY }
    if ($hasE) { $e = $newE }
}

$gridLayers = [Collections.Generic.List[object]]::new()
$dominantDirections = @{}
foreach ($entry in $angleLengths.GetEnumerator()) {
    $directions = @($entry.Value.GetEnumerator() | Sort-Object Value -Descending)
    if ($directions.Count -eq 0) { continue }
    $dominantDirections[[int]$entry.Key] = [int]$directions[0].Key
    $total = ($directions | Measure-Object Value -Sum).Sum
    for ($i = 0; $i -lt $directions.Count; $i++) {
        for ($j = $i + 1; $j -lt $directions.Count; $j++) {
            $left = [int]$directions[$i].Key
            $right = [int]$directions[$j].Key
            $delta = [Math]::Abs($left - $right)
            $delta = [Math]::Min($delta, 180 - $delta)
            $leftRatio = [double]$directions[$i].Value / $total
            $rightRatio = [double]$directions[$j].Value / $total
            if ($delta -ge 75 -and $delta -le 105 -and $leftRatio -ge 0.15 -and $rightRatio -ge 0.15) {
                $gridLayers.Add([pscustomobject]@{
                    Layer = [int]$entry.Key
                    DirectionA = $left
                    DirectionB = $right
                    RatioA = [Math]::Round($leftRatio, 3)
                    RatioB = [Math]::Round($rightRatio, 3)
                })
                $i = $directions.Count
                break
            }
        }
    }
}

$directionGroups = $dominantDirections.Values |
    Group-Object |
    Sort-Object Count -Descending |
    Select-Object @{n='Angle';e={[int]$_.Name}}, Count

Write-Host 'Dominant support directions by layer:'
$directionGroups | Format-Table -AutoSize
Write-Host 'Detected same-layer grid cases:'
$gridLayers | Select-Object -First 20 | Format-Table -AutoSize

[pscustomobject]@{
    Gcode = $gcodePath
    SupportSegments = $supportSegments
    LayersWithLongSupportLines = $angleLengths.Count
    SameLayerGridCount = $gridLayers.Count
    GridDetected = $gridLayers.Count -gt 0
}
