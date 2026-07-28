param(
    [Parameter(Mandatory = $true)]
    [string]$Gcode
)

$ErrorActionPreference = 'Stop'
$gcodePath = (Resolve-Path $Gcode).Path

$layer = -1
$role = ''
$x = 0.0
$y = 0.0
$lastWasSupportExtrusion = $false
$layers = @{}

function Get-LayerRecord([int]$Layer) {
    if (-not $layers.ContainsKey($Layer)) {
        $layers[$Layer] = [ordered]@{
            Layer = $Layer
            Paths = 0
            Segments = 0
            Length = 0.0
            MinX = [double]::PositiveInfinity
            MaxX = [double]::NegativeInfinity
            MinY = [double]::PositiveInfinity
            MaxY = [double]::NegativeInfinity
        }
    }
    return $layers[$Layer]
}

foreach ($line in [System.IO.File]::ReadLines($gcodePath)) {
    if ($line -match '^;LAYER_CHANGE') {
        $layer++
        $lastWasSupportExtrusion = $false
        continue
    }
    if ($line -match '^;TYPE:(.+)$') {
        $role = $Matches[1].Trim()
        if ($role -notmatch '^Support$') {
            $lastWasSupportExtrusion = $false
        }
        continue
    }
    if ($line -notmatch '^G[01]\s') {
        continue
    }

    $newX = $x
    $newY = $y
    $number = '-?(?:\d+(?:\.\d*)?|\.\d+)'
    if ($line -match "(?:^|\s)X($number)") { $newX = [double]$Matches[1] }
    if ($line -match "(?:^|\s)Y($number)") { $newY = [double]$Matches[1] }
    $hasX = $line -match "(?:^|\s)X($number)"
    $hasY = $line -match "(?:^|\s)Y($number)"
    $eValue = 0.0
    $hasE = $line -match "(?:^|\s)E($number)"
    if ($hasE) { $eValue = [double]$Matches[1] }
    $isExtrusion = $hasE -and $eValue -gt 0
    $isSupport = $role -eq 'Support'

    if ($isSupport -and $isExtrusion -and $layer -ge 0) {
        $record = Get-LayerRecord $layer
        if (-not $lastWasSupportExtrusion) {
            $record.Paths++
        }
        $dx = $newX - $x
        $dy = $newY - $y
        $record.Segments++
        $record.Length += [Math]::Sqrt($dx * $dx + $dy * $dy)
        $record.MinX = [Math]::Min($record.MinX, [Math]::Min($x, $newX))
        $record.MaxX = [Math]::Max($record.MaxX, [Math]::Max($x, $newX))
        $record.MinY = [Math]::Min($record.MinY, [Math]::Min($y, $newY))
        $record.MaxY = [Math]::Max($record.MaxY, [Math]::Max($y, $newY))
        $lastWasSupportExtrusion = $true
    } elseif (($hasX -or $hasY) -and -not $isExtrusion) {
        $lastWasSupportExtrusion = $false
    }

    $x = $newX
    $y = $newY
}

$records = $layers.Values | ForEach-Object { [pscustomobject]$_ } | Sort-Object Layer
$body = $records | Where-Object {
    $_.MinX -gt 80 -and $_.MaxX -lt 180 -and $_.MinY -gt 80 -and $_.MaxY -lt 180
}

Write-Host "G-code: $gcodePath"
Write-Host "Support layers parsed: $($records.Count)"
Write-Host "Torus-only layers: $($body.Count)"
Write-Host 'Path-count distribution:'
$body | Group-Object Paths | Sort-Object { [int]$_.Name } |
    Select-Object @{n='Paths';e={[int]$_.Name}}, Count |
    Format-Table -AutoSize

Write-Host 'First 12 torus support layers:'
$body | Select-Object -First 12 Layer, Paths, Segments, @{n='Length';e={[Math]::Round($_.Length, 2)}}, MinX, MaxX, MinY, MaxY |
    Format-Table -AutoSize

Write-Host 'Last 12 torus support layers:'
$body | Select-Object -Last 12 Layer, Paths, Segments, @{n='Length';e={[Math]::Round($_.Length, 2)}}, MinX, MaxX, MinY, MaxY |
    Format-Table -AutoSize

$singlePathCount = @($body | Where-Object Paths -eq 1).Count
[pscustomobject]@{
    Passed = $body.Count -gt 0 -and $singlePathCount -eq $body.Count
    TorusSupportLayers = $body.Count
    SinglePathLayers = $singlePathCount
    MultiPathLayers = $body.Count - $singlePathCount
}
