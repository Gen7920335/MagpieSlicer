param(
    [string] $SlicerPath,
    [string] $ProbeGeneratorPath,
    [string] $OutputRoot,
    [int] $SliceTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build-vulkan\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build-vulkan\verification\tsunami-actual-slice'
}
if ([string]::IsNullOrWhiteSpace($ProbeGeneratorPath)) {
    $ProbeGeneratorPath = Join-Path $RepoRoot 'build-vulkan\src\dev-utils\Release\tsunami-probe-generator.exe'
}
$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$ProbeGeneratorPath = [IO.Path]::GetFullPath($ProbeGeneratorPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
$TemplatePath = Join-Path $PSScriptRoot 'resources\tsunami-actual-slice.fragment.html'
foreach ($path in @($SlicerPath, $ProbeGeneratorPath, $BaseMachinePath, $BaseProcessPath, $TemplatePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}

function Format-Number([double] $value) { $value.ToString('0.######', $Invariant) }

function Set-JsonProperty($object, [string] $name, $value) {
    $object | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force
}

function Find-FilamentProfile([string] $name) {
    foreach ($root in @((Join-Path $RepoRoot 'resources\profiles'), (Join-Path $env:APPDATA 'MagpieSlicer\system'))) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$name.json" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $name"
}

function Token-Value([string] $line, [char] $token) {
    $match = [regex]::Match($line, "(?:^|\s)$token(-?(?:\d+(?:\.\d*)?|\.\d+))")
    if ($match.Success) { return [double]::Parse($match.Groups[1].Value, $Invariant) }
    return $null
}

function Read-Toolpaths([string] $path) {
    $layers = @{}
    $settings = @{}
    $layer = -1; $z = 0.0; $role = ''
    $absoluteXY = $true; $relativeExtrusion = $false
    $x = $null; $y = $null; $e = 0.0
    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { ++$layer; continue }
        if ($line -match '^;Z:(-?(?:\d+(?:\.\d*)?|\.\d+))') { $z = [double]::Parse($Matches[1], $Invariant); continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -match '^; ([a-z][a-z0-9_]*) = (.*)$') { $settings[$Matches[1]] = $Matches[2].Trim(); continue }
        if ($line -eq 'G90') { $absoluteXY = $true; continue }
        if ($line -eq 'G91') { $absoluteXY = $false; continue }
        if ($line -match '^M82(?:\s|$)') { $relativeExtrusion = $false; continue }
        if ($line -match '^M83(?:\s|$)') { $relativeExtrusion = $true; continue }
        if ($line -match '^G92(?:\s|$)') { $newE = Token-Value $line 'E'; if ($null -ne $newE) { $e = $newE }; continue }
        if ($line -notmatch '^G[0123](?:\s|$)') { continue }
        $tx = Token-Value $line 'X'; $ty = Token-Value $line 'Y'; $te = Token-Value $line 'E'
        $nextX = if ($null -eq $tx) { $x } elseif ($absoluteXY -or $null -eq $x) { $tx } else { $x + $tx }
        $nextY = if ($null -eq $ty) { $y } elseif ($absoluteXY -or $null -eq $y) { $ty } else { $y + $ty }
        $deltaE = if ($null -eq $te) { 0.0 } elseif ($relativeExtrusion) { $te } else { $te - $e }
        if ($deltaE -gt 0.0000001 -and $null -ne $x -and $null -ne $y -and $null -ne $nextX -and $null -ne $nextY) {
            if (-not $layers.ContainsKey($layer)) {
                $layers[$layer] = [pscustomobject]@{
                    Layer=$layer; Z=$z
                    Model=[Collections.Generic.List[object]]::new()
                    Support=[Collections.Generic.List[object]]::new()
                }
            }
            $segment = @(
                [Math]::Round([double]$x,3), [Math]::Round([double]$y,3),
                [Math]::Round([double]$nextX,3), [Math]::Round([double]$nextY,3))
            if ($role -match '(?i)support') { $layers[$layer].Support.Add($segment) }
            elseif ($role -notmatch '(?i)skirt|brim|prime tower') { $layers[$layer].Model.Add($segment) }
            $layers[$layer].Z = $z
        }
        if ($null -ne $tx) { $x = $nextX }
        if ($null -ne $ty) { $y = $nextY }
        if ($null -ne $te) { $e = if ($relativeExtrusion) { $e + $te } else { $te } }
    }
    return [pscustomobject]@{ Layers=$layers; Settings=$settings }
}

function Render-ToolpathPreview([object[]] $layers, [string] $path) {
    Add-Type -AssemblyName System.Drawing
    $width = 1800; $height = 1120; $columns = 3; $rows = 2
    $margin = 42; $header = 88; $gap = 24
    $panelWidth = ($width - 2*$margin - ($columns-1)*$gap) / $columns
    $panelHeight = ($height - $header - $margin - ($rows-1)*$gap) / $rows
    $bitmap = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.Clear([Drawing.Color]::FromArgb(14,18,23))
    $titleFont = [Drawing.Font]::new('Segoe UI', 30, [Drawing.FontStyle]::Bold)
    $labelFont = [Drawing.Font]::new('Segoe UI', 18, [Drawing.FontStyle]::Bold)
    $smallFont = [Drawing.Font]::new('Segoe UI', 14)
    $white = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(239,244,250))
    $muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(165,176,190))
    $panelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(22,28,35))
    $panelPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(60,72,86), 2)
    $modelPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(145,156,170), 1.2)
    $supportPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(30,210,220), 2.6)
    $modelPen.StartCap = $modelPen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $supportPen.StartCap = $supportPen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $graphics.DrawString('Actual Tsunami G-code toolpath', $titleFont, $white, $margin, 24)
    $graphics.DrawString('Gray: model extrusion   Cyan: Tsunami support extrusion', $smallFont, $muted, $margin+2, 62)
    for ($index = 0; $index -lt $layers.Count; ++$index) {
        $layer = $layers[$index]
        $column = $index % $columns; $row = [Math]::Floor($index / $columns)
        $left = $margin + $column*($panelWidth+$gap); $top = $header + $row*($panelHeight+$gap)
        $graphics.FillRectangle($panelBrush, [float]$left, [float]$top, [float]$panelWidth, [float]$panelHeight)
        $graphics.DrawRectangle($panelPen, [float]$left, [float]$top, [float]$panelWidth, [float]$panelHeight)
        $graphics.DrawString("Layer $($layer.layer)  Z $([Math]::Round($layer.z,2)) mm", $labelFont, $white, $left+18, $top+14)
        $all = @($layer.model) + @($layer.support)
        $xs = @($all | ForEach-Object { [double]$_[0]; [double]$_[2] })
        $ys = @($all | ForEach-Object { [double]$_[1]; [double]$_[3] })
        $minX = ($xs | Measure-Object -Minimum).Minimum; $maxX = ($xs | Measure-Object -Maximum).Maximum
        $minY = ($ys | Measure-Object -Minimum).Minimum; $maxY = ($ys | Measure-Object -Maximum).Maximum
        $rangeX = [Math]::Max(0.001, $maxX-$minX); $rangeY = [Math]::Max(0.001, $maxY-$minY)
        $plotLeft=$left+18; $plotTop=$top+58; $plotWidth=$panelWidth-36; $plotHeight=$panelHeight-78
        $scale=[Math]::Min($plotWidth/$rangeX,$plotHeight/$rangeY)
        $originX=$plotLeft+($plotWidth-$rangeX*$scale)/2; $originY=$plotTop+($plotHeight-$rangeY*$scale)/2
        foreach ($series in @(@($layer.model,$modelPen),@($layer.support,$supportPen))) {
            foreach ($segment in @($series[0])) {
                $x1=$originX+([double]$segment[0]-$minX)*$scale
                $y1=$originY+($maxY-[double]$segment[1])*$scale
                $x2=$originX+([double]$segment[2]-$minX)*$scale
                $y2=$originY+($maxY-[double]$segment[3])*$scale
                $graphics.DrawLine($series[1],[float]$x1,[float]$y1,[float]$x2,[float]$y2)
            }
        }
    }
    $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    foreach ($item in @($supportPen,$modelPen,$panelPen,$panelBrush,$muted,$white,$smallFont,$labelFont,$titleFont,$graphics,$bitmap)) {
        $item.Dispose()
    }
}

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$modelPath = Join-Path $runRoot 'tsunami-circular-overhang.obj'
$machinePath = Join-Path $runRoot 'machine.json'
$processPath = Join-Path $runRoot 'process.json'
$savedPath = $env:PATH
try {
    $env:PATH = "$(Split-Path -Parent $SlicerPath);$env:PATH"
    & $ProbeGeneratorPath $modelPath
} finally {
    $env:PATH = $savedPath
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
    throw "Tsunami probe generation failed with exit code $LASTEXITCODE"
}

$machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
$process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
$profileName = 'Tsunami actual slice verification'
$machine.name = $profileName; $machine.setting_id = 'tsunami-actual-slice-machine'
$machine.printable_area = @('-60x-60','60x-60','60x60','-60x60')
Set-JsonProperty $machine 'bed_exclude_area' @()
$process.name = $profileName; $process.setting_id = 'tsunami-actual-slice-process'
$process.compatible_printers = @($profileName)
foreach ($setting in @{
    enable_support='1'; support_type='tsunami(auto)'; support_on_build_plate_only='1';
    independent_support_layer_height='0';
    support_threshold_angle='30'; support_object_xy_distance='0'; bridge_no_support='0';
    support_interface_top_layers='0'; support_interface_bottom_layers='0';
    layer_height='0.2'; initial_layer_print_height='0.2';
    tsunami_branch_angle='45'; tsunami_micro_branch_enabled='0';
    tsunami_trunk_height='4'; tsunami_rib_spacing='1.5';
    tsunami_min_bed_contact_area='1'; tsunami_max_bed_contact_area='100';
    use_smaller_nozzles_in_crisp_corners='0'
}.GetEnumerator()) { Set-JsonProperty $process $setting.Key $setting.Value }
$machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
$process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

$filaments = @('Snapmaker PLA @U1','Snapmaker ABS @U1','Snapmaker PETG @U1','Snapmaker TPU @U1') |
    ForEach-Object { Find-FilamentProfile $_ }
$settingsArg = [char]34 + "$machinePath;$processPath" + [char]34
$filamentsArg = [char]34 + ($filaments -join ';') + [char]34
$stdout = Join-Path $runRoot 'cli.out.log'; $stderr = Join-Path $runRoot 'cli.err.log'
$arguments = @('--slice','0','--debug','1','--load-settings',$settingsArg,'--load-filaments',$filamentsArg,
    '--outputdir',([char]34 + $runRoot + [char]34),([char]34 + $modelPath + [char]34))
$handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) { $handle.Kill(); throw 'Tsunami slice timed out' }
$handle.Refresh()
$gcode = Get-ChildItem -LiteralPath $runRoot -Filter '*.gcode' -File | Select-Object -First 1
if ($null -eq $gcode -or ($null -ne $handle.ExitCode -and $handle.ExitCode -ne 0)) {
    throw "Slicing failed. $(Get-Content $stderr -Raw)"
}

$parsed = Read-Toolpaths $gcode.FullName
if ($parsed.Settings['support_type'] -ne 'tsunami(auto)') { throw 'Generated G-code did not use Tsunami support' }
$supportLayers = [object[]]@($parsed.Layers.Values | Where-Object { $_.Support.Count -gt 0 } | Sort-Object Layer)
$supportLayerCount = [int]$supportLayers.Length
if ($supportLayerCount -lt 5) { throw "Only $supportLayerCount support layers were generated" }
$lastSupportLayerIndex = $supportLayerCount - 1
$indexes = @(0, [Math]::Floor($lastSupportLayerIndex*0.25), [Math]::Floor($lastSupportLayerIndex*0.5),
    [Math]::Floor($lastSupportLayerIndex*0.75), $lastSupportLayerIndex) | Sort-Object -Unique
$previewLayers = @($indexes | ForEach-Object {
    $item = $supportLayers[$_]
    [pscustomobject]@{ layer=$item.Layer; z=$item.Z; model=@($item.Model); support=@($item.Support) }
})
$data = [pscustomobject]@{ gcode=$gcode.Name; layers=$previewLayers }
$json = $data | ConvertTo-Json -Depth 8 -Compress
$fragmentPath = Join-Path $runRoot 'tsunami-actual-slice.html'
$template = Get-Content -LiteralPath $TemplatePath -Raw
[IO.File]::WriteAllText($fragmentPath, $template.Replace('__TSUNAMI_SLICE_DATA__', $json), [Text.UTF8Encoding]::new($false))
$imagePath = Join-Path $runRoot 'tsunami-actual-slice.png'
Render-ToolpathPreview $previewLayers $imagePath
$result = [pscustomobject]@{
    Model=$modelPath; Gcode=$gcode.FullName; Fragment=$fragmentPath; Image=$imagePath
    SupportLayers=$supportLayerCount; PreviewLayers=@($previewLayers | ForEach-Object { $_.layer })
}
$resultPath = Join-Path $runRoot 'results.json'
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8
$result | Format-List
Write-Output "RUN_ROOT=$runRoot"
Write-Output "MODEL=$modelPath"
Write-Output "GCODE=$($gcode.FullName)"
Write-Output "FRAGMENT=$fragmentPath"
Write-Output "IMAGE=$imagePath"
Write-Output "RESULTS=$resultPath"
