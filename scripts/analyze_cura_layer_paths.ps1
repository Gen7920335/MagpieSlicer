param(
    [Parameter(Mandatory = $true)]
    [string]$Gcode,
    [int]$TargetLayer = 1
)

$ErrorActionPreference = 'Stop'
$number = '-?(?:\d+(?:\.\d*)?|\.\d+)'
$layer = -1
$role = ''
$x = 0.0
$y = 0.0
$path = $null
$paths = [System.Collections.Generic.List[object]]::new()

function Finish-Path {
    if ($null -ne $script:path) {
        $script:paths.Add([pscustomobject]$script:path)
        $script:path = $null
    }
}

foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $Gcode))) {
    if ($line -match '^;LAYER_CHANGE') {
        if ($layer -eq $TargetLayer) { Finish-Path; break }
        $layer++
        continue
    }
    if ($line -match '^;TYPE:(.+)$') {
        $role = $Matches[1].Trim()
        if ($layer -eq $TargetLayer -and $role -ne 'Support') { Finish-Path }
        continue
    }
    if ($line -notmatch '^G[01]\s') { continue }

    $newX = $x
    $newY = $y
    if ($line -match "(?:^|\s)X($number)") { $newX = [double]$Matches[1] }
    if ($line -match "(?:^|\s)Y($number)") { $newY = [double]$Matches[1] }
    $hasX = $line -match "(?:^|\s)X($number)"
    $hasY = $line -match "(?:^|\s)Y($number)"
    $hasE = $line -match "(?:^|\s)E($number)"
    $e = if ($hasE) { [double]$Matches[1] } else { 0.0 }

    if ($layer -eq $TargetLayer -and $role -eq 'Support' -and $hasE -and $e -gt 0) {
        if ($null -eq $path) {
            $path = [ordered]@{
                Index = $paths.Count + 1
                StartX = $x
                StartY = $y
                EndX = $newX
                EndY = $newY
                Segments = 0
                Length = 0.0
                MinX = [Math]::Min($x, $newX)
                MaxX = [Math]::Max($x, $newX)
                MinY = [Math]::Min($y, $newY)
                MaxY = [Math]::Max($y, $newY)
            }
        }
        $dx = $newX - $x
        $dy = $newY - $y
        $path.Segments++
        $path.Length += [Math]::Sqrt($dx * $dx + $dy * $dy)
        $path.EndX = $newX
        $path.EndY = $newY
        $path.MinX = [Math]::Min($path.MinX, [Math]::Min($x, $newX))
        $path.MaxX = [Math]::Max($path.MaxX, [Math]::Max($x, $newX))
        $path.MinY = [Math]::Min($path.MinY, [Math]::Min($y, $newY))
        $path.MaxY = [Math]::Max($path.MaxY, [Math]::Max($y, $newY))
    } elseif ($layer -eq $TargetLayer -and ($hasX -or $hasY)) {
        Finish-Path
    }
    $x = $newX
    $y = $newY
}

Finish-Path
$paths | ForEach-Object {
    $_.Length = [Math]::Round($_.Length, 3)
    $_
} | Format-Table -AutoSize

[pscustomobject]@{
    Layer = $TargetLayer
    PathCount = $paths.Count
    TotalSegments = ($paths | Measure-Object Segments -Sum).Sum
    TotalLength = [Math]::Round(($paths | Measure-Object Length -Sum).Sum, 3)
}
