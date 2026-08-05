param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [int] $SliceTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\cura-bottom-interface'
}

$auditRoot = Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'build\verification\cura-support-audit') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
if ($null -eq $auditRoot) { throw 'Cura support audit inputs were not found' }
$baseCase = Join-Path $auditRoot.FullName 'auto_angle_70'
$baseMachine = Join-Path $baseCase 'machine.json'
$baseProcess = Join-Path $baseCase 'process.json'
foreach ($path in @($SlicerPath, $baseMachine, $baseProcess)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}

function Set-JsonProperty($object, [string] $name, $value) {
    $object | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force
}

function Analyze-Support([string] $path) {
    $role = ''
    $layer = -1
    $x = $null
    $y = $null
    $support = 0.0
    $interface = 0.0
    $canonical = [Text.StringBuilder]::new()

    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { ++$layer; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -notmatch '^G[01](?:\s|$)' -or
            $line -notmatch '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') { continue }
        $extrusion = [double]::Parse($Matches[1], $Invariant)
        if ($extrusion -le 0) { continue }

        $nextX = $x
        $nextY = $y
        $matchX = [regex]::Match($line, '(?:^|\s)X(-?(?:\d+(?:\.\d*)?|\.\d+))')
        $matchY = [regex]::Match($line, '(?:^|\s)Y(-?(?:\d+(?:\.\d*)?|\.\d+))')
        if ($matchX.Success) { $nextX = [double]::Parse($matchX.Groups[1].Value, $Invariant) }
        if ($matchY.Success) { $nextY = [double]::Parse($matchY.Groups[1].Value, $Invariant) }

        if ($null -ne $x -and $null -ne $y -and $null -ne $nextX -and $null -ne $nextY -and
            $role -in @('Support', 'Support interface', 'Support interface sublayer')) {
            $length = [Math]::Sqrt(($nextX - $x) * ($nextX - $x) + ($nextY - $y) * ($nextY - $y))
            if ($role -eq 'Support') { $support += $length } else { $interface += $length }
            [void] $canonical.AppendLine("$role|$layer|$line")
        }
        if ($null -ne $nextX) { $x = $nextX }
        if ($null -ne $nextY) { $y = $nextY }
    }

    $sha = [Security.Cryptography.SHA256]::Create()
    $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical.ToString()))
    [pscustomobject]@{
        Support = [Math]::Round($support, 2)
        Interface = [Math]::Round($interface, 2)
        Hash = ($bytes | ForEach-Object { $_.ToString('X2') }) -join ''
    }
}

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$modelPath = Join-Path $runRoot 'bottom-contact.obj'
@'
v 0 0 0
v 40 0 0
v 40 40 0
v 0 40 0
v 0 0 10
v 40 0 10
v 40 40 10
v 0 40 10
v 10 10 20
v 30 10 20
v 30 30 20
v 10 30 20
v 10 10 25
v 30 10 25
v 30 30 25
v 10 30 25
f 1 3 2
f 1 4 3
f 5 6 7
f 5 7 8
f 1 2 6
f 1 6 5
f 2 3 7
f 2 7 6
f 3 4 8
f 3 8 7
f 4 1 5
f 4 5 8
f 9 11 10
f 9 12 11
f 13 14 15
f 13 15 16
f 9 10 14
f 9 14 13
f 10 11 15
f 10 15 14
f 11 12 16
f 11 16 15
f 12 9 13
f 12 13 16
'@ | Set-Content -LiteralPath $modelPath -Encoding ascii

$results = @()
foreach ($bottomLayers in @(0, 3)) {
    $caseRoot = Join-Path $runRoot "bottom_$bottomLayers"
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'
    Copy-Item -LiteralPath $baseMachine -Destination $machinePath
    $process = Get-Content -LiteralPath $baseProcess -Raw | ConvertFrom-Json
    Set-JsonProperty $process 'support_type' 'normal_cura(auto)'
    Set-JsonProperty $process 'support_threshold_angle' '90'
    Set-JsonProperty $process 'support_interface_top_layers' '4'
    Set-JsonProperty $process 'support_interface_bottom_layers' ([string] $bottomLayers)
    Set-JsonProperty $process 'support_interface_pattern' 'rectilinear'
    Set-JsonProperty $process 'support_interface_spacing' '0'
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding utf8

    $settings = [char]34 + "$machinePath;$processPath" + [char]34
    $output = [char]34 + $caseRoot + [char]34
    $input = [char]34 + $modelPath + [char]34
    $stdout = Join-Path $caseRoot 'cli.out.log'
    $stderr = Join-Path $caseRoot 'cli.err.log'
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList @(
        '--slice', '0', '--load-settings', $settings, '--outputdir', $output, $input
    ) -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $handle.Kill()
        throw "Slice timed out: bottom interface $bottomLayers"
    }
    $handle.Refresh()
    $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
    if ($null -eq $gcode) { throw "G-code was not generated for bottom interface $bottomLayers" }
    $analysis = Analyze-Support $gcode.FullName
    $results += [pscustomobject]@{
        BottomLayers = $bottomLayers
        Support = $analysis.Support
        Interface = $analysis.Interface
        Hash = $analysis.Hash
        Gcode = $gcode.FullName
    }
}

$baseline = $results | Where-Object BottomLayers -eq 0
$bottom = $results | Where-Object BottomLayers -eq 3
$passed =
    $baseline.Support -gt 0 -and $bottom.Support -gt 0 -and
    $bottom.Interface -gt $baseline.Interface -and
    $bottom.Hash -ne $baseline.Hash

$resultPath = Join-Path $runRoot 'results.json'
[pscustomobject]@{ Passed = $passed; Cases = $results } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding utf8
$results | Select-Object BottomLayers,Support,Interface,Hash,Gcode | Format-Table -AutoSize
"RESULTS=$resultPath"
if (-not $passed) { exit 1 }
