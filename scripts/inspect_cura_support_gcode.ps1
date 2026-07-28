param(
    [Parameter(Mandatory = $true)]
    [string]$Gcode,
    [int]$TargetLayer = 1
)

$ErrorActionPreference = 'Stop'
$layer = -1
$role = ''
$capturing = $false
$printed = 0
$markers = [System.Collections.Generic.List[string]]::new()

foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $Gcode))) {
    if ($markers.Count -lt 120 -and $line -match '^;(LAYER|TYPE|FEATURE)') {
        $markers.Add("layer=$layer $line")
    }
    if ($line -match '^;LAYER_CHANGE') {
        $layer++
        if ($layer -gt $TargetLayer -and $capturing) { break }
    }
    if ($line -match '^;TYPE:(.+)$') {
        $role = $Matches[1].Trim()
    }
    if ($layer -eq $TargetLayer -and $role -eq 'Support' -and ($line -match '^G[01]\s' -or $line -match '^;TYPE:Support$')) {
        $capturing = $true
    }
    if ($capturing) {
        Write-Host $line
        $printed++
        if ($printed -ge 120 -or ($line -match '^;TYPE:' -and $line -notmatch '^;TYPE:Support$')) {
            break
        }
    }
}

if (-not $capturing) {
    Write-Host "Support block not found on layer $TargetLayer. First markers:"
    $markers | ForEach-Object { Write-Host $_ }
    exit 2
}
