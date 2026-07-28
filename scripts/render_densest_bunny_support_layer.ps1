param(
    [Parameter(Mandatory = $true)]
    [string]$GcodePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

$finderOutput = & (Join-Path $scriptRoot "find_densest_support_layer.ps1") -GcodePath $GcodePath |
    ForEach-Object { "$_" }
$finderOutput | Write-Output

$marker = $finderOutput | Where-Object { $_ -match '^DENSEST_LAYER=(\d+)$' } | Select-Object -Last 1
if (-not $marker -or $marker -notmatch '^DENSEST_LAYER=(\d+)$') {
    throw "The densest support layer could not be determined."
}

$layer = [int]$Matches[1]
& (Join-Path $scriptRoot "render_cura_support_section.ps1") `
    -Gcode $GcodePath `
    -Layer $layer `
    -OutputPath $OutputPath `
    -MinCoordinate -1000

Write-Output "OUTPUT_PATH=$OutputPath"
