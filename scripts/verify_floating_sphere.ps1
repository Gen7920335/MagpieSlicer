param(
    [string]$ProjectPath = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = Join-Path $repoRoot "build\verification\sphere-50mm-z10\Sphere_50mm_Z10.3mf"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot (
        "build\verification\sphere-50mm-z10\cli-" + (Get-Date -Format "yyyyMMdd-HHmmss")
    )
}

$ProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$slicerPath = Join-Path $repoRoot "build\src\Release\magpie-slicer.exe"
$settingsRoot = Join-Path $repoRoot "build\verification\support-post-lowtemp-fix-20260728-213109\20260728-213110\cura_90_triangles"
$machineSettings = Join-Path $settingsRoot "machine.json"
$processSettings = Join-Path $settingsRoot "process.json"

foreach ($requiredPath in @($ProjectPath, $slicerPath, $machineSettings, $processSettings)) {
    if (-not [System.IO.File]::Exists($requiredPath)) {
        throw "Required file is missing: $requiredPath"
    }
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$archive = [System.IO.Compression.ZipFile]::OpenRead($ProjectPath)
try {
    $modelEntry = $archive.GetEntry("3D/3dmodel.model")
    if ($null -eq $modelEntry) {
        throw "3D model entry is missing."
    }

    $reader = [System.IO.StreamReader]::new($modelEntry.Open())
    try {
        [xml]$modelXml = $reader.ReadToEnd()
    }
    finally {
        $reader.Dispose()
    }
}
finally {
    $archive.Dispose()
}

$namespaceManager = [System.Xml.XmlNamespaceManager]::new($modelXml.NameTable)
$namespaceManager.AddNamespace(
    "m",
    "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
)
$buildItem = $modelXml.SelectSingleNode("//m:build/m:item", $namespaceManager)
if ($null -eq $buildItem -or $buildItem.auto_drop -ne "0") {
    throw "The project does not disable automatic bed dropping."
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$arguments = @(
    "--slice", "0",
    "--load-settings", $machineSettings,
    "--load-settings", $processSettings,
    "--outputdir", $OutputDirectory,
    $ProjectPath
)
Start-Process -FilePath $slicerPath -ArgumentList $arguments | Out-Null

$deadline = (Get-Date).AddMinutes(3)
$gcode = $null
$lastLength = -1L
$stableChecks = 0

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 1
    $candidate = Get-ChildItem -LiteralPath $OutputDirectory -File -Filter "*.gcode" |
        Select-Object -First 1

    if ($null -eq $candidate) {
        continue
    }

    if ($candidate.Length -eq $lastLength) {
        $stableChecks++
    }
    else {
        $lastLength = $candidate.Length
        $stableChecks = 0
    }

    if ($stableChecks -ge 2) {
        $tail = Get-Content -LiteralPath $candidate.FullName -Tail 100
        if ($tail -match "CONFIG_BLOCK_END|total_layer_number") {
            $gcode = $candidate
            break
        }
    }
}

if ($null -eq $gcode) {
    throw "CLI slicing did not complete within three minutes."
}

$currentZ = $null
$currentType = ""
$firstSupportZ = $null
$firstModelZ = $null
$invariant = [System.Globalization.CultureInfo]::InvariantCulture

foreach ($line in [System.IO.File]::ReadLines($gcode.FullName)) {
    if ($line -match "^;Z:([0-9.+-]+)") {
        $currentZ = [double]::Parse($Matches[1], $invariant)
    }
    elseif ($line -match "^G[01].*\sZ([0-9.+-]+)") {
        $currentZ = [double]::Parse($Matches[1], $invariant)
    }

    if ($line -match "^;TYPE:(.+)$") {
        $currentType = $Matches[1]
    }
    if ($null -ne $currentZ -and $null -eq $firstSupportZ -and $currentType -match "Support") {
        $firstSupportZ = $currentZ
    }
    if ($null -ne $currentZ -and $null -eq $firstModelZ -and $currentType -match "wall|surface|skin") {
        $firstModelZ = $currentZ
        break
    }
}

if ($null -eq $firstModelZ) {
    throw "No model extrusion was found in the generated G-code."
}
if ($firstModelZ -lt 9.8) {
    throw "The slicer auto-dropped the sphere. First model Z: $firstModelZ"
}

[pscustomobject]@{
    Project = $ProjectPath
    GCode = $gcode.FullName
    AutoDrop = $buildItem.auto_drop
    FirstSupportZ = $firstSupportZ
    FirstModelZ = $firstModelZ
    Result = "Floating placement preserved"
} | Format-List
