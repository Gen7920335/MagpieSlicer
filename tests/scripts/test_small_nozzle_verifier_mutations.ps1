param([string] $OutputRoot)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $RepoRoot 'scripts\lib\SmallNozzleVerifier.psm1') -Force
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\small-nozzle-verifier-mutations'
}
$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

$invariant = [Globalization.CultureInfo]::InvariantCulture
$layerHeight = 0.1
$smallWidth = 0.17
$largeWidth = 0.45
$shapeCorrection = $layerHeight * (1.0 - [Math]::PI / 4.0)
$smallSpacing = $smallWidth - $shapeCorrection
$largeSpacing = $largeWidth - $shapeCorrection
$overlap = 15.0
$boundarySpacing = 0.5 * ($smallSpacing + $largeSpacing) -
    ($overlap / 100.0) * [Math]::Min($smallSpacing, $largeSpacing)

function Format-Number([double] $Value) {
    return $Value.ToString('0.#####', $invariant)
}

function Add-Loop {
    param(
        [Collections.Generic.List[string]] $Lines,
        [double] $X0,
        [double] $Y0,
        [double] $X1,
        [double] $Y1,
        [ref] $Extrusion,
        [bool] $AbsoluteExtrusion,
        [string] $Target = ''
    )
    $Lines.Add("G1 X$(Format-Number $X0) Y$(Format-Number $Y0) F1800")
    foreach ($point in @(@($X1,$Y0), @($X1,$Y1), @($X0,$Y1), @($X0,$Y0))) {
        if ($AbsoluteExtrusion) {
            $Extrusion.Value += 1.0
            $eValue = $Extrusion.Value
        } else {
            $eValue = 1.0
        }
        if ($Target -and $point[0] -eq $X0 -and $point[1] -eq $Y0) {
            $Lines.Add(";MUTATION_TARGET:$Target")
        }
        $Lines.Add("G1 X$(Format-Number $point[0]) Y$(Format-Number $point[1]) E$(Format-Number $eValue)")
    }
}

function New-SyntheticGcode {
    param(
        [string] $Path,
        [string] $Mutation = 'none',
        [double] $TranslateX = 0.0,
        [double] $TranslateY = 0.0,
        [switch] $AbsoluteExtrusion
    )
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('; wall_generator = classic')
    $lines.Add($(if ($Mutation -eq 'metadata_count') {
        '; crisp_corner_small_nozzle_wall_count = 4'
    } else {
        '; crisp_corner_small_nozzle_wall_count = 3'
    }))
    $lines.Add('; crisp_corner_interlace_small_nozzle_walls = 1')
    $lines.Add('G90')
    $lines.Add($(if ($AbsoluteExtrusion) { 'M82' } else { 'M83' }))
    $e = 0.0
    $layouts = for ($layer = 0; $layer -lt 8; ++$layer) {
        if ($Mutation -eq 'flat_interlock') {
            [pscustomobject]@{ Small=3; Large=3 }
        } elseif (($layer % 2) -eq 0) {
            [pscustomobject]@{ Small=3; Large=3 }
        } else {
            [pscustomobject]@{ Small=2; Large=4 }
        }
    }

    for ($layer = 0; $layer -lt $layouts.Count; ++$layer) {
        $layout = $layouts[$layer]
        $lines.Add(';LAYER_CHANGE')
        $lines.Add("G1 Z$(Format-Number (($layer + 1) * $layerHeight)) F600")
        if ($Mutation -eq 'missing_layer' -and $layer -eq 3) { continue }

        $order = if ($Mutation -eq 'swapped_tool_order' -and $layer -eq 2) {
            @(
                [pscustomobject]@{ Tool=0; Count=$layout.Large; Width=$largeWidth; Spacing=$largeSpacing },
                [pscustomobject]@{ Tool=1; Count=$layout.Small; Width=$smallWidth; Spacing=$smallSpacing }
            )
        } else {
            @(
                [pscustomobject]@{ Tool=1; Count=$layout.Small; Width=$smallWidth; Spacing=$smallSpacing },
                [pscustomobject]@{ Tool=0; Count=$layout.Large; Width=$largeWidth; Spacing=$largeSpacing }
            )
        }
        $inset = 0.1
        for ($groupIndex = 0; $groupIndex -lt $order.Count; ++$groupIndex) {
            $group = $order[$groupIndex]
            $tool = if ($Mutation -eq 'no_detail_tool') { 0 } else { $group.Tool }
            $lines.Add("T$tool")
            $lines.Add(';TYPE:Outer wall')
            $width = if ($Mutation -eq 'width_inversion' -and $group.Tool -eq 1) { 0.55 } else { $group.Width }
            $lines.Add(";WIDTH:$(Format-Number $width)")
            for ($loop = 0; $loop -lt $group.Count; ++$loop) {
                $x0 = 82.0 + $TranslateX + $inset
                $x1 = 119.0 + $TranslateX - $inset
                $y0 = 102.0 + $TranslateY + $inset
                $y1 = 134.0 + $TranslateY - $inset
                $target = if ($layer -eq 0 -and $group.Tool -eq 1 -and $loop -eq 0) { 'FIRST_SMALL_CLOSE' } else { '' }
                Add-Loop -Lines $lines -X0 $x0 -Y0 $y0 -X1 $x1 -Y1 $y1 -Extrusion ([ref] $e) `
                    -AbsoluteExtrusion $AbsoluteExtrusion -Target $target
                if ($Mutation -eq 'duplicate_loop' -and $layer -eq 0 -and $group.Tool -eq 1 -and $loop -eq 0) {
                    Add-Loop -Lines $lines -X0 $x0 -Y0 $y0 -X1 $x1 -Y1 $y1 -Extrusion ([ref] $e) `
                        -AbsoluteExtrusion $AbsoluteExtrusion
                }
                if ($loop -lt $group.Count - 1) {
                    $inset += $group.Spacing
                } elseif ($groupIndex -lt $order.Count - 1) {
                    $inset += $(if ($Mutation -eq 'boundary_gap' -and $layer -eq 4) {
                        $boundarySpacing + 0.2
                    } else {
                        $boundarySpacing
                    })
                }
            }
        }
        if ($Mutation -eq 'role_leak' -and $layer -eq 4) {
            $lines.Add('T1')
            $lines.Add(';TYPE:Sparse infill')
            $lines.Add(';WIDTH:0.17')
            $lines.Add("G1 X$(Format-Number (90+$TranslateX)) Y$(Format-Number (110+$TranslateY)) F1800")
            $eValue = if ($AbsoluteExtrusion) { $e += 1.0; $e } else { 1.0 }
            $lines.Add("G1 X$(Format-Number (100+$TranslateX)) Y$(Format-Number (110+$TranslateY)) E$(Format-Number $eValue)")
        }
        if ($Mutation -eq 'excessive_tool_changes' -and $layer -eq 5) {
            for ($change = 0; $change -lt 18; ++$change) { $lines.Add("T$($change % 2)") }
        }
    }

    if ($Mutation -eq 'unsupported_tool') {
        $lines.Insert(6, 'T9')
    } elseif ($Mutation -eq 'nonfinite') {
        $lines.Insert(6, 'G1 XNaN Y100 E1')
    } elseif ($Mutation -eq 'out_of_bounds') {
        $lines.Insert(6, 'T0')
        $lines.Insert(7, ';TYPE:Outer wall')
        $lines.Insert(8, 'G1 X10 Y10 F1800')
        $lines.Insert(9, 'G1 X-5 Y10 E1')
    } elseif ($Mutation -eq 'open_loop') {
        for ($index = 0; $index -lt $lines.Count - 1; ++$index) {
            if ($lines[$index] -eq ';MUTATION_TARGET:FIRST_SMALL_CLOSE') {
                $lines.RemoveAt($index + 1)
                break
            }
        }
    }
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

$case = [pscustomobject]@{
    Generator='classic'; Nozzle2=0.15; BaseTool=1; Count=3; Enabled=$true
    RequireLarge=$true; OverrideRanges=@(); OverrideChecks=@(); StrictCounts=$true
    ProbeLayerCount=8; WallLoops=3; Interlock=$true; LayerHeight=$layerHeight
    Overlap=$overlap; InfillDensity=0; Speed='0'
}

function Invoke-One {
    param(
        [string] $Name,
        [string] $Mutation,
        [bool] $ExpectedPass,
        [double] $TranslateX = 0.0,
        [double] $TranslateY = 0.0,
        [switch] $AbsoluteExtrusion
    )
    $path = Join-Path $runRoot "$Name.gcode"
    New-SyntheticGcode -Path $path -Mutation $Mutation -TranslateX $TranslateX -TranslateY $TranslateY `
        -AbsoluteExtrusion:$AbsoluteExtrusion
    $printableArea = @(
        "$(Format-Number $TranslateX)x$(Format-Number $TranslateY)",
        "$(Format-Number (270+$TranslateX))x$(Format-Number $TranslateY)",
        "$(Format-Number (270+$TranslateX))x$(Format-Number (270+$TranslateY))",
        "$(Format-Number $TranslateX)x$(Format-Number (270+$TranslateY))"
    )
    $analysis = Get-SmallNozzleGcodeAnalysis -Path $path -PrintableArea $printableArea `
        -PrintableHeight 270 -ToolCount 4 -LayerHeight $layerHeight -ProbeZ $layerHeight -ProbeLayerCount 8
    $verdict = Test-SmallNozzleCase -Case $case -Analysis $analysis -PrintableArea $printableArea
    return [pscustomobject]@{
        Name = $Name
        Mutation = $Mutation
        ExpectedPass = $ExpectedPass
        ActualPass = [bool] $verdict.Passed
        HarnessPassed = [bool] $verdict.Passed -eq $ExpectedPass
        Errors = $verdict.Errors -join '; '
        Gcode = $path
    }
}

$specs = @(
    @{ Name='control_relative'; Mutation='none'; Pass=$true },
    @{ Name='control_absolute_e'; Mutation='none'; Pass=$true; Absolute=$true },
    @{ Name='control_translated'; Mutation='none'; Pass=$true; X=37.0; Y=19.0 },
    @{ Name='mutate_missing_layer'; Mutation='missing_layer'; Pass=$false },
    @{ Name='mutate_flat_interlock'; Mutation='flat_interlock'; Pass=$false },
    @{ Name='mutate_swapped_tool_order'; Mutation='swapped_tool_order'; Pass=$false },
    @{ Name='mutate_no_detail_tool'; Mutation='no_detail_tool'; Pass=$false },
    @{ Name='mutate_open_loop'; Mutation='open_loop'; Pass=$false },
    @{ Name='mutate_duplicate_loop'; Mutation='duplicate_loop'; Pass=$false },
    @{ Name='mutate_boundary_gap'; Mutation='boundary_gap'; Pass=$false },
    @{ Name='mutate_metadata_count'; Mutation='metadata_count'; Pass=$false },
    @{ Name='mutate_role_leak'; Mutation='role_leak'; Pass=$false },
    @{ Name='mutate_excessive_tool_changes'; Mutation='excessive_tool_changes'; Pass=$false },
    @{ Name='mutate_unsupported_tool'; Mutation='unsupported_tool'; Pass=$false },
    @{ Name='mutate_nonfinite'; Mutation='nonfinite'; Pass=$false },
    @{ Name='mutate_out_of_bounds'; Mutation='out_of_bounds'; Pass=$false },
    @{ Name='mutate_width_inversion'; Mutation='width_inversion'; Pass=$false }
)

$results = foreach ($spec in $specs) {
    Invoke-One -Name $spec.Name -Mutation $spec.Mutation -ExpectedPass $spec.Pass `
        -TranslateX $(if ($spec.X) { $spec.X } else { 0.0 }) `
        -TranslateY $(if ($spec.Y) { $spec.Y } else { 0.0 }) `
        -AbsoluteExtrusion:([bool] $spec.Absolute)
}
$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Select-Object Name,ExpectedPass,ActualPass,HarnessPassed,Errors | Format-Table -AutoSize

$survivors = @($results | Where-Object { -not $_.HarnessPassed })
Write-Output "RESULTS=$resultsPath"
Write-Output "CONTROLS_PASSED=$(@($results | Where-Object ExpectedPass | Where-Object HarnessPassed).Count)/$(@($results | Where-Object ExpectedPass).Count)"
Write-Output "MUTATIONS_KILLED=$(@($results | Where-Object { -not $_.ExpectedPass } | Where-Object HarnessPassed).Count)/$(@($results | Where-Object { -not $_.ExpectedPass }).Count)"
if ($survivors.Count -gt 0) {
    throw "Verifier mutation suite failed: $($survivors.Name -join ', ')"
}
