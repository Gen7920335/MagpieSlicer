param(
    [Parameter(Mandatory = $true)]
    [string]$GcodePath
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $GcodePath)) {
    throw "G-code not found: $GcodePath"
}

$layer = -1
$role = ""
$lastX = $null
$lastY = $null
$lastE = $null
$relativeExtrusion = $false
$stats = @{}

function Get-LayerStats([int]$Layer) {
    if (-not $stats.ContainsKey($Layer)) {
        $stats[$Layer] = [pscustomobject]@{
            Layer = $Layer
            Length = 0.0
            Segments = 0
        }
    }
    return $stats[$Layer]
}

foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $GcodePath))) {
    if ($line -eq ';LAYER_CHANGE') {
        $layer++
        continue
    }
    if ($line -match '^;TYPE:(.+)$') {
        $role = $Matches[1].Trim()
        continue
    }
    if ($line -eq 'M82') {
        $relativeExtrusion = $false
        continue
    }
    if ($line -eq 'M83') {
        $relativeExtrusion = $true
        continue
    }
    if ($line -match '^G92(?:\s|$)') {
        if ($line -match '(?:^|\s)E(-?\d+(?:\.\d+)?)') {
            $lastE = [double]$Matches[1]
        }
        continue
    }
    if ($line -notmatch '^(?:G0|G1)\s') {
        continue
    }

    $x = $lastX
    $y = $lastY
    $e = $lastE
    if ($line -match '(?:^|\s)X(-?\d+(?:\.\d+)?)') { $x = [double]$Matches[1] }
    if ($line -match '(?:^|\s)Y(-?\d+(?:\.\d+)?)') { $y = [double]$Matches[1] }
    if ($line -match '(?:^|\s)E(-?\d+(?:\.\d+)?)') { $e = [double]$Matches[1] }

    $hasExtrusion = $false
    if ($null -ne $e -and $null -ne $lastE) {
        $deltaE = if ($relativeExtrusion) { $e } else { $e - $lastE }
        $hasExtrusion = $deltaE -gt 0.000001
    }

    if (
        $layer -ge 0 -and
        $role -eq 'Support' -and
        $hasExtrusion -and
        $null -ne $lastX -and
        $null -ne $lastY -and
        $null -ne $x -and
        $null -ne $y
    ) {
        $length = [Math]::Sqrt(
            [Math]::Pow($x - $lastX, 2) +
            [Math]::Pow($y - $lastY, 2)
        )
        if ($length -gt 0.001) {
            $entry = Get-LayerStats $layer
            $entry.Length += $length
            $entry.Segments++
        }
    }

    if ($null -ne $x) { $lastX = $x }
    if ($null -ne $y) { $lastY = $y }
    if ($null -ne $e) {
        if ($relativeExtrusion) {
            if ($null -eq $lastE) { $lastE = 0.0 }
            $lastE += $e
        } else {
            $lastE = $e
        }
    }
}

$ranked = @($stats.Values | Sort-Object Length -Descending)
if ($ranked.Count -eq 0) {
    throw "No support-body extrusion was found."
}

($ranked | Select-Object -First 10 |
    Format-Table Layer, Segments, @{Label="LengthMm"; Expression={"{0:F3}" -f $_.Length}} |
    Out-String).TrimEnd() | Write-Output
Write-Output "DENSEST_LAYER=$($ranked[0].Layer)"
