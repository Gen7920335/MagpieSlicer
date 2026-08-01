param(
    [ValidateSet('Smoke','Core','Full','Interlock','Bunny','PlanOnly','SelfTest')]
    [string] $Mode = 'Core',
    [string] $SlicerPath,
    [string] $OutputRoot,
    [string] $ModelPath,
    [string] $NativeAnalyzerPath,
    [string] $CaseFilter = '*',
    [string] $CaseNamesPath,
    [int] $SliceTimeoutSeconds = 600,
    [switch] $IncludePairwise,
    [switch] $PlanOnly,
    [int] $ShardIndex = -1,
    [int] $ShardCount = 1
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\SmallNozzleVerifier.psm1') -Force

if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot $(if ($Mode -eq 'Bunny') { 'build\verification\small-nozzle-bunny' } else { 'build\verification\small-nozzle-geometry' })
}
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $RepoRoot $(if ($Mode -eq 'Bunny') { 'resources\handy_models\Stanford_Bunny.drc' } else { 'tests\data\small_nozzle_geometry_probe.stl' })
}
if ([string]::IsNullOrWhiteSpace($NativeAnalyzerPath)) {
    $NativeAnalyzerPath = Join-Path $RepoRoot 'build\src\dev-utils\Release\small-nozzle-gcode-verifier.exe'
}

function Format-Number([double] $Value) {
    return $Value.ToString('0.###', $Invariant)
}

function Set-JsonProperty($Object, [string] $Property, $Value) {
    if ($null -eq $Object.PSObject.Properties[$Property]) {
        $Object | Add-Member -NotePropertyName $Property -NotePropertyValue $Value
    } else {
        $Object.$Property = $Value
    }
}

function Set-JsonArrayValue($Object, [string] $Property, [int] $Index, [string] $Value) {
    if ($null -eq $Object.$Property -or $Object.$Property.Count -le $Index) {
        throw "Machine profile property '$Property' has no index $Index"
    }
    $Object.$Property[$Index] = $Value
}

function Copy-Case($Case) {
    return $Case | ConvertTo-Json -Depth 20 | ConvertFrom-Json
}

function New-BaseCase([string] $Name = 'baseline') {
    return [pscustomobject]@{
        Name = $Name
        Factor = 'baseline'
        Level = 'baseline'
        Generator = 'classic'
        Nozzle2 = 0.15
        BaseTool = 1
        DetailToolhead = 0
        Enabled = $true
        Count = 3
        Interlock = $false
        Overlap = 15
        WallLoops = 3
        LayerHeight = 0.10
        InfillDensity = 15
        InfillPattern = 'gyroid'
        Speed = '0'
        RequireLarge = $true
        StrictCounts = $true
        OverrideRanges = @()
        OverrideChecks = @()
        LineWidthScale = 1.0
        OuterWidthScale = 1.0
        InnerWidthScale = 1.0
        InitialWidthScale = 1.0
        BridgeWidthScale = 1.0
        ProbeLayerCount = 2
        ProcessOverrides = [pscustomobject]@{}
    }
}

function Set-CaseFactor($Case, [string] $Factor, $Value) {
    $Case.Factor = $Factor
    $Case.Level = [string] $Value
    switch ($Factor) {
        'Enabled' { $Case.Enabled = [bool] $Value }
        'Interlock' { $Case.Interlock = [bool] $Value }
        'Generator' { $Case.Generator = [string] $Value }
        'DetailToolhead' { $Case.DetailToolhead = [int] $Value }
        'Count' {
            $Case.Count = [int] $Value
            if ($Case.Count -gt 8) { $Case.StrictCounts = $false; $Case.RequireLarge = $false }
        }
        'Overlap' { $Case.Overlap = [int] $Value }
        'OverrideMode' {
            switch ([string] $Value) {
                'none' { $Case.OverrideRanges = @(); $Case.OverrideChecks = @() }
                'single' {
                    $Case.OverrideRanges = @('1:20:1')
                    $Case.OverrideChecks = @(
                        [pscustomobject]@{ Layer=0; Tool=0 },
                        [pscustomobject]@{ Layer=19; Tool=0 }
                    )
                }
                'overlap' {
                    $Case.OverrideRanges = @('1:15:1','10:20:2')
                    $Case.OverrideChecks = @(
                        [pscustomobject]@{ Layer=0; Tool=0 },
                        [pscustomobject]@{ Layer=9; Tool=1 },
                        [pscustomobject]@{ Layer=19; Tool=1 }
                    )
                }
            }
        }
        'Speed' { $Case.Speed = [string] $Value }
        'Nozzle2' {
            $Case.Nozzle2 = [double] $Value
            $Case.BaseTool = if ($Case.Nozzle2 -gt 0.4) { 2 } else { 1 }
        }
        'WallLoops' { $Case.WallLoops = [int] $Value }
        'LayerHeight' { $Case.LayerHeight = [double] $Value }
        'LineWidthScale' { $Case.LineWidthScale = [double] $Value }
        'OuterWidthScale' { $Case.OuterWidthScale = [double] $Value }
        'InnerWidthScale' { $Case.InnerWidthScale = [double] $Value }
        'InitialWidthScale' { $Case.InitialWidthScale = [double] $Value }
        'BridgeWidthScale' { $Case.BridgeWidthScale = [double] $Value }
        'InfillDensity' { $Case.InfillDensity = [int] $Value }
        'SparsePattern' { $Case.InfillPattern = [string] $Value }
        default {
            $propertyMap = @{
                DetectThinWall='detect_thin_wall'
                PreciseOuterWall='precise_outer_wall'
                OnlyOneWallTop='only_one_wall_top'
                DetectOverhangWall='detect_overhang_wall'
                EnsureVerticalShell='ensure_vertical_shell_thickness'
                ArcFitting='enable_arc_fitting'
                WallSequence='wall_sequence'
                SeamPosition='seam_position'
                InfillOverlap='infill_wall_overlap'
                TopLayers='top_shell_layers'
                BottomLayers='bottom_shell_layers'
                ElephantComp='elefant_foot_compensation'
                Resolution='resolution'
                XYContour='xy_contour_compensation'
                XYHole='xy_hole_compensation'
                SmallPerimeterThreshold='small_perimeter_threshold'
                OuterSpeed='outer_wall_speed'
                InnerSpeed='inner_wall_speed'
                InfillAnchor='infill_anchor'
            }
            $property = $propertyMap[$Factor]
            if ([string]::IsNullOrWhiteSpace($property)) { throw "Unknown factor: $Factor" }
            Set-JsonProperty $Case.ProcessOverrides $property ([string] $Value)
        }
    }
}

function New-OfatCases {
    # Baseline plus one case per non-baseline level: 1 + 9*1 + 28*2 = 66.
    $binary = [ordered]@{
        Enabled = @($true,$false)
        Interlock = @($false,$true)
        Generator = @('classic','arachne')
        DetectThinWall = @('0','1')
        PreciseOuterWall = @('1','0')
        OnlyOneWallTop = @('0','1')
        DetectOverhangWall = @('1','0')
        EnsureVerticalShell = @('1','0')
        ArcFitting = @('0','1')
    }
    $ternary = [ordered]@{
        DetailToolhead = @(0,1,2)
        Count = @(3,1,100)
        Overlap = @(15,0,80)
        OverrideMode = @('none','single','overlap')
        Speed = @('0','30','50%')
        Nozzle2 = @(0.15,0.20,0.80)
        WallLoops = @(3,1,8)
        LayerHeight = @(0.10,0.08,0.20)
        LineWidthScale = @(1.0,0.8,1.4)
        OuterWidthScale = @(1.0,0.8,1.4)
        InnerWidthScale = @(1.0,0.8,1.4)
        InitialWidthScale = @(1.0,0.8,1.4)
        BridgeWidthScale = @(1.0,0.8,1.4)
        WallSequence = @('inner wall/outer wall','outer wall/inner wall','inner-outer-inner wall')
        SeamPosition = @('aligned','nearest','random')
        InfillDensity = @(15,0,40)
        InfillOverlap = @('15%','0%','50%')
        TopLayers = @(5,0,10)
        BottomLayers = @(3,0,10)
        ElephantComp = @(0,0.15,0.4)
        Resolution = @(0.012,0.001,0.1)
        XYContour = @(0,-0.2,0.2)
        XYHole = @(0,-0.2,0.2)
        SmallPerimeterThreshold = @(0,6,20)
        OuterSpeed = @(60,20,120)
        InnerSpeed = @(100,30,180)
        SparsePattern = @('gyroid','rectilinear','honeycomb')
        InfillAnchor = @('400%','0%','1000%')
    }
    if ($binary.Keys.Count -ne 9 -or $ternary.Keys.Count -ne 28) {
        throw "OFAT definition drift: binary=$($binary.Keys.Count), ternary=$($ternary.Keys.Count)"
    }
    $cases = [Collections.Generic.List[object]]::new()
    $cases.Add((New-BaseCase -Name 'ofat_baseline'))
    foreach ($entry in $binary.GetEnumerator()) {
        $case = Copy-Case (New-BaseCase)
        Set-CaseFactor $case $entry.Key $entry.Value[1]
        $case.Name = "ofat_$($entry.Key)_$($entry.Value[1])".ToLowerInvariant().Replace('%','pct').Replace('.','p').Replace('/','-').Replace(' ','-')
        $cases.Add($case)
    }
    foreach ($entry in $ternary.GetEnumerator()) {
        for ($index = 1; $index -lt 3; ++$index) {
            $case = Copy-Case (New-BaseCase)
            Set-CaseFactor $case $entry.Key $entry.Value[$index]
            $case.Name = "ofat_$($entry.Key)_$($entry.Value[$index])".ToLowerInvariant().Replace('%','pct').Replace('.','p').Replace('/','-').Replace(' ','-')
            $cases.Add($case)
        }
    }
    if ($cases.Count -ne 66) { throw "OFAT case count drift: $($cases.Count)" }
    return $cases.ToArray()
}

function New-CoreCases {
    $specs = @(
        @{ Name='classic_disabled'; Generator='classic'; Enabled=$false; Count=3; Interlock=$false },
        @{ Name='classic_count1'; Generator='classic'; Enabled=$true; Count=1; Interlock=$false },
        @{ Name='classic_count3'; Generator='classic'; Enabled=$true; Count=3; Interlock=$false },
        @{ Name='classic_count3_interlock'; Generator='classic'; Enabled=$true; Count=3; Interlock=$true },
        @{ Name='classic_count4_interlock'; Generator='classic'; Enabled=$true; Count=4; Interlock=$true },
        @{ Name='classic_overlap0'; Generator='classic'; Enabled=$true; Count=3; Interlock=$false; Overlap=0 },
        @{ Name='classic_overlap80'; Generator='classic'; Enabled=$true; Count=3; Interlock=$false; Overlap=80 },
        @{ Name='classic_nozzle020'; Generator='classic'; Enabled=$true; Count=3; Interlock=$true; Nozzle2=0.20 },
        @{ Name='classic_reverse060'; Generator='classic'; Enabled=$true; Count=3; Interlock=$true; Nozzle2=0.60; BaseTool=2 },
        @{ Name='classic_reverse080'; Generator='classic'; Enabled=$true; Count=3; Interlock=$true; Nozzle2=0.80; BaseTool=2 },
        @{ Name='classic_override_1_20'; Generator='classic'; Enabled=$true; Count=3; Interlock=$true; OverrideMode='single' },
        @{ Name='classic_override_overlap'; Generator='classic'; Enabled=$true; Count=3; Interlock=$true; OverrideMode='overlap' },
        @{ Name='arachne_disabled'; Generator='arachne'; Enabled=$false; Count=3; Interlock=$false },
        @{ Name='arachne_count1'; Generator='arachne'; Enabled=$true; Count=1; Interlock=$false },
        @{ Name='arachne_count3'; Generator='arachne'; Enabled=$true; Count=3; Interlock=$false },
        @{ Name='arachne_count3_interlock'; Generator='arachne'; Enabled=$true; Count=3; Interlock=$true },
        @{ Name='arachne_count4_interlock'; Generator='arachne'; Enabled=$true; Count=4; Interlock=$true },
        @{ Name='arachne_nozzle020'; Generator='arachne'; Enabled=$true; Count=3; Interlock=$true; Nozzle2=0.20 },
        @{ Name='arachne_reverse060'; Generator='arachne'; Enabled=$true; Count=3; Interlock=$true; Nozzle2=0.60; BaseTool=2 },
        @{ Name='arachne_override_1_20'; Generator='arachne'; Enabled=$true; Count=3; Interlock=$true; OverrideMode='single' }
    )
    $cases = foreach ($spec in $specs) {
        $case = New-BaseCase -Name $spec.Name
        foreach ($property in $spec.GetEnumerator()) {
            if ($property.Key -eq 'Name') { continue }
            if ($property.Key -eq 'OverrideMode') {
                Set-CaseFactor $case 'OverrideMode' $property.Value
            } else {
                $case.($property.Key) = $property.Value
            }
        }
        $case
    }
    return @($cases)
}

function New-InterlockCases {
    $cases = [Collections.Generic.List[object]]::new()
    $wallCountPairs = @(
        @(1,1), @(1,4),
        @(2,1), @(2,4),
        @(3,1), @(3,2), @(3,3), @(3,4), @(3,6), @(3,8),
        @(5,2), @(5,4), @(8,4)
    )
    foreach ($generator in @('classic','arachne')) {
        foreach ($pair in $wallCountPairs) {
            $case = New-BaseCase -Name ("interlock_{0}_large{1}_small{2}" -f $generator,$pair[0],$pair[1])
            $case.Factor = 'interlock-wall-count'
            $case.Level = "large=$($pair[0]);small=$($pair[1])"
            $case.Generator = $generator
            $case.WallLoops = [int] $pair[0]
            $case.Count = [int] $pair[1]
            $case.Interlock = $true
            $case.ProbeLayerCount = 8
            $cases.Add($case)
        }
        foreach ($nozzle in @(0.20,0.60,0.80)) {
            $case = New-BaseCase -Name ("interlock_{0}_nozzle{1}_large3_small4" -f $generator,([string]$nozzle).Replace('.','p'))
            $case.Factor = 'interlock-nozzle-order'
            $case.Level = "nozzle2=$nozzle"
            Set-CaseFactor $case 'Nozzle2' $nozzle
            $case.Generator = $generator
            $case.WallLoops = 3
            $case.Count = 4
            $case.Interlock = $true
            $case.ProbeLayerCount = 8
            $cases.Add($case)
        }
    }
    return $cases.ToArray()
}

function New-BunnyCases {
    return @(
        foreach ($generator in @('classic','arachne')) {
            foreach ($interlock in @($false,$true)) {
                $state = if ($interlock) { 'on' } else { 'off' }
                $case = New-BaseCase -Name ("bunny_{0}_interlock_{1}" -f $generator,$state)
                $case.Factor = 'bunny-interlock-differential'
                $case.Level = $state
                $case.Generator = $generator
                $case.Count = 4
                $case.WallLoops = 3
                $case.Interlock = $interlock
                $case.StrictCounts = $false
                $case.ProbeLayerCount = 2
                $case
            }
        }
    )
}

function New-PairwiseCases {
    # Deterministic mixed-level rows for the high-risk interactions. OFAT remains
    # the source of single-factor boundary coverage.
    $rows = [Collections.Generic.List[object]]::new()
    $generators = @('classic','arachne')
    $nozzles = @(0.15,0.20,0.60,0.80)
    $counts = @(1,3,4,100)
    $overlaps = @(0,15,80)
    $loops = @(1,3,8)
    for ($i = 0; $i -lt 24; ++$i) {
        $case = New-BaseCase -Name ("pairwise_{0:D2}" -f ($i + 1))
        $case.Factor = 'pairwise'
        $case.Level = [string] ($i + 1)
        $case.Generator = $generators[$i % 2]
        $case.Nozzle2 = $nozzles[($i * 3 + [Math]::Floor($i / 4)) % $nozzles.Count]
        $case.BaseTool = if ($case.Nozzle2 -gt 0.4) { 2 } else { 1 }
        $case.Count = $counts[($i * 5 + [Math]::Floor($i / 3)) % $counts.Count]
        $case.Overlap = $overlaps[($i * 2 + [Math]::Floor($i / 2)) % $overlaps.Count]
        $case.WallLoops = $loops[($i + [Math]::Floor($i / 5)) % $loops.Count]
        $case.Interlock = ($i % 3) -ne 0
        $case.DetailToolhead = @(0,1,2)[($i * 2) % 3]
        $case.LayerHeight = @(0.08,0.10,0.20)[($i * 2 + 1) % 3]
        $case.LineWidthScale = @(0.8,1.0,1.4)[($i + 2) % 3]
        $case.StrictCounts = $case.Count -le 8
        $case.RequireLarge = $case.Count -le 8
        $rows.Add($case)
    }
    return $rows.ToArray()
}

function Find-FilamentProfile([string] $Name) {
    foreach ($root in @((Join-Path $RepoRoot 'resources\profiles'), (Join-Path $env:APPDATA 'MagpieSlicer\system'))) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$Name.json" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $Name"
}

function Get-ToolWidth([double] $Nozzle) {
    if ([Math]::Abs($Nozzle - 0.4) -lt 0.001) { return 0.45 }
    return $Nozzle * 1.10
}

function Write-CaseProfiles($Case, [string] $CaseRoot, [string] $BaseMachinePath, [string] $BaseProcessPath) {
    $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
    $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
    $machineName = "Geometry verifier $($Case.Name)"
    $machine.name = $machineName
    $machine.setting_id = "geometry-verifier-$($Case.Name)"

    Set-JsonArrayValue $machine 'nozzle_diameter' 1 (Format-Number $Case.Nozzle2)
    Set-JsonArrayValue $machine 'min_layer_height' 1 (Format-Number ([Math]::Min(0.05, $Case.Nozzle2 * 0.25)))
    Set-JsonArrayValue $machine 'max_layer_height' 1 (Format-Number ($Case.Nozzle2 * 0.75))
    $nozzles = @($machine.nozzle_diameter | ForEach-Object { [double]::Parse([string] $_, $Invariant) })
    $widthProperties = @(
        'toolhead_line_width','toolhead_outer_wall_line_width','toolhead_inner_wall_line_width',
        'toolhead_top_surface_line_width','toolhead_sparse_infill_line_width',
        'toolhead_internal_solid_infill_line_width','toolhead_support_line_width'
    )
    for ($tool = 0; $tool -lt $nozzles.Count; ++$tool) {
        $nominal = Get-ToolWidth $nozzles[$tool]
        foreach ($property in $widthProperties) {
            $scale = $Case.LineWidthScale
            if ($property -eq 'toolhead_outer_wall_line_width') { $scale *= $Case.OuterWidthScale }
            if ($property -eq 'toolhead_inner_wall_line_width') { $scale *= $Case.InnerWidthScale }
            Set-JsonArrayValue $machine $property $tool (Format-Number ($nominal * $scale))
        }
        Set-JsonArrayValue $machine 'toolhead_initial_layer_line_width' $tool `
            (Format-Number ($nozzles[$tool] * 1.4 * $Case.InitialWidthScale))
        Set-JsonArrayValue $machine 'toolhead_bridge_line_width' $tool `
            (Format-Number ($nozzles[$tool] * $Case.BridgeWidthScale))
    }

    $process.name = "Geometry verifier $($Case.Name)"
    $process.setting_id = "geometry-verifier-$($Case.Name)"
    $process.compatible_printers = @($machineName)
    Set-JsonProperty $process 'layer_height' (Format-Number $Case.LayerHeight)
    Set-JsonProperty $process 'initial_layer_print_height' (Format-Number $Case.LayerHeight)
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' $(if ($Case.Enabled) { '1' } else { '0' })
    Set-JsonProperty $process 'crisp_corner_detail_toolhead' ([string] $Case.DetailToolhead)
    Set-JsonProperty $process 'crisp_corner_small_nozzle_wall_count' ([string] $Case.Count)
    Set-JsonProperty $process 'crisp_corner_interlace_small_nozzle_walls' $(if ($Case.Interlock) { '1' } else { '0' })
    Set-JsonProperty $process 'crisp_corner_nozzle_wall_overlap' "$($Case.Overlap)%"
    Set-JsonProperty $process 'crisp_corner_large_nozzle_override_regions' @($Case.OverrideRanges)
    Set-JsonProperty $process 'crisp_corner_small_nozzle_wall_speed' @([string] $Case.Speed)
    Set-JsonProperty $process 'wall_generator' $Case.Generator
    Set-JsonProperty $process 'wall_loops' ([string] $Case.WallLoops)
    Set-JsonProperty $process 'outer_wall_filament_id' ([string] $Case.BaseTool)
    Set-JsonProperty $process 'inner_wall_filament_id' ([string] $Case.BaseTool)
    Set-JsonProperty $process 'sparse_infill_filament_id' ([string] $Case.BaseTool)
    Set-JsonProperty $process 'sparse_infill_density' "$($Case.InfillDensity)%"
    Set-JsonProperty $process 'sparse_infill_pattern' $Case.InfillPattern
    Set-JsonProperty $process 'bridge_line_width' '0'
    foreach ($property in $Case.ProcessOverrides.PSObject.Properties) {
        Set-JsonProperty $process $property.Name $property.Value
    }

    $machinePath = Join-Path $CaseRoot 'machine.json'
    $processPath = Join-Path $CaseRoot 'process.json'
    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8
    return [pscustomobject]@{
        Machine = $machine
        Process = $process
        MachinePath = $machinePath
        ProcessPath = $processPath
    }
}

if ($Mode -eq 'SelfTest') {
    $selfTest = Invoke-SmallNozzleVerifierSelfTest -WorkingDirectory (Join-Path $OutputRoot 'selftest')
    $selfTest | Format-List
    if (-not $selfTest.Passed) { exit 1 }
    exit 0
}

$cases = switch ($Mode) {
    'Smoke' {
        $smokeNames = @(
            'classic_disabled','classic_count3','classic_count3_interlock',
            'arachne_disabled','arachne_count3','arachne_count3_interlock'
        )
        @(New-CoreCases | Where-Object Name -in $smokeNames)
    }
    'Core' { @(New-CoreCases) }
    'Interlock' { @(New-InterlockCases) }
    'Bunny' { @(New-BunnyCases) }
    default { @(New-OfatCases) }
}
if ($IncludePairwise) { $cases += @(New-PairwiseCases) }
$cases = @($cases | Where-Object Name -like $CaseFilter)
if (-not [string]::IsNullOrWhiteSpace($CaseNamesPath)) {
    if (-not (Test-Path -LiteralPath $CaseNamesPath -PathType Leaf)) {
        throw "Case name list not found: $CaseNamesPath"
    }
    $requestedNames = @(Get-Content -LiteralPath $CaseNamesPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $cases = @($cases | Where-Object Name -in $requestedNames)
    $missingNames = @($requestedNames | Where-Object { $_ -notin $cases.Name })
    if ($missingNames.Count -gt 0) { throw "Unknown case names: $($missingNames -join ', ')" }
}
if ($cases.Count -eq 0) { throw "No cases matched '$CaseFilter'" }
$totalCaseCount = $cases.Count
if ($ShardIndex -ge 0) {
    if ($ShardCount -lt 1 -or $ShardIndex -ge $ShardCount) {
        throw "Invalid shard $ShardIndex/$ShardCount"
    }
    $cases = @(
        for ($caseIndex = 0; $caseIndex -lt $totalCaseCount; ++$caseIndex) {
            if (($caseIndex % $ShardCount) -eq $ShardIndex) { $cases[$caseIndex] }
        }
    )
    if ($cases.Count -eq 0) { throw "Shard $ShardIndex/$ShardCount has no cases" }
}

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$plan = [pscustomobject]@{
    SchemaVersion = 1
    Model = [IO.Path]::GetFullPath($ModelPath)
    Mode = $Mode
    OfatDesign = [pscustomobject]@{ BinaryFactors=9; TernaryFactors=28; Cases=66 }
    PairwiseIncluded = [bool] $IncludePairwise
    TotalCaseCount = $totalCaseCount
    ShardIndex = $ShardIndex
    ShardCount = $ShardCount
    SelectedCaseCount = $cases.Count
    Cases = $cases
}
$planPath = Join-Path $runRoot 'case-plan.json'
$plan | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $planPath -Encoding UTF8
if ($Mode -eq 'PlanOnly' -or $PlanOnly) {
    Write-Output "PLAN=$planPath"
    Write-Output "CASES=$($cases.Count)"
    exit 0
}

$required = @(
    $SlicerPath,
    $ModelPath,
    (Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'),
    (Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'),
    $NativeAnalyzerPath
)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}
$baseMachinePath = $required[2]
$baseProcessPath = $required[3]
$filaments = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)

$selfTest = Invoke-SmallNozzleVerifierSelfTest -WorkingDirectory (Join-Path $runRoot 'selftest')
if (-not $selfTest.Passed) { throw "Verifier self-test failed: $($selfTest.Actual -join ',')" }

$results = [Collections.Generic.List[object]]::new()
$index = 0
foreach ($case in $cases) {
    $caseRoot = Join-Path $runRoot ('case_{0:D3}_{1}' -f $index, $case.Name)
    ++$index
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $profiles = Write-CaseProfiles -Case $case -CaseRoot $caseRoot `
        -BaseMachinePath $baseMachinePath -BaseProcessPath $baseProcessPath
    $stdoutPath = Join-Path $caseRoot 'cli.out.log'
    $stderrPath = Join-Path $caseRoot 'cli.err.log'
    $quotedSettings = [char]34 + "$($profiles.MachinePath);$($profiles.ProcessPath)" + [char]34
    $quotedFilaments = [char]34 + ($filaments -join ';') + [char]34
    $quotedOutput = [char]34 + $caseRoot + [char]34
    $quotedModel = [char]34 + ([IO.Path]::GetFullPath($ModelPath)) + [char]34
    $arguments = @('--slice','0','--debug','1','--load-settings',$quotedSettings,
        '--load-filaments',$quotedFilaments,'--outputdir',$quotedOutput,$quotedModel)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $timedOut = -not $handle.WaitForExit($SliceTimeoutSeconds * 1000)
    if ($timedOut) {
        $handle.Kill()
        $handle.WaitForExit()
    }
    $handle.Refresh()
    $timer.Stop()
    $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
    $runtimeErrors = [Collections.Generic.List[string]]::new()
    if ($timedOut) { $runtimeErrors.Add("Timed out after $SliceTimeoutSeconds seconds") }
    $exitCode = $handle.ExitCode
    if ($null -eq $exitCode) { $exitCode = if ($null -ne $gcode) { 0 } else { 1 } }
    if (-not $timedOut -and $exitCode -ne 0) { $runtimeErrors.Add("CLI exit code $exitCode") }
    if ($null -eq $gcode) { $runtimeErrors.Add('G-code was not generated') }

    $analysis = $null
    $verdict = $null
    $analysisTimer = [Diagnostics.Stopwatch]::StartNew()
    if ($null -ne $gcode) {
        $printableArea = Get-PrintableAreaInfo $profiles.Machine.printable_area
        $smallTool = if ([double] $case.Nozzle2 -lt 0.4) { 1 } else { 0 }
        $largeTool = if ([double] $case.Nozzle2 -lt 0.4) { 0 } else { 1 }
        $probeLayers = @(Get-ProbeLayerNumbers -LayerHeight $case.LayerHeight -ProbeZ 8.0 -Count $case.ProbeLayerCount)
        $minX = [double] ($printableArea.Points | Measure-Object X -Minimum).Minimum
        $minY = [double] ($printableArea.Points | Measure-Object Y -Minimum).Minimum
        $maxX = [double] ($printableArea.Points | Measure-Object X -Maximum).Maximum
        $maxY = [double] ($printableArea.Points | Measure-Object Y -Maximum).Maximum
        $nativePath = Join-Path $caseRoot 'native-analysis.json'
        & $NativeAnalyzerPath $gcode.FullName $smallTool $largeTool $profiles.Machine.nozzle_diameter.Count $nativePath `
            $probeLayers[0] $probeLayers.Count (Format-Number $minX) (Format-Number $minY) `
            (Format-Number $maxX) (Format-Number $maxY)
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nativePath -PathType Leaf)) {
            throw "Native G-code analyzer failed for $($case.Name)"
        }
        $native = Get-Content -LiteralPath $nativePath -Raw | ConvertFrom-Json
        $settings = @{}
        foreach ($property in $native.settings.PSObject.Properties) { $settings[$property.Name] = [string] $property.Value }
        $lengthByLayerTool = @{}
        $wallToolsByLayer = @{}
        foreach ($layerMetric in $native.layers) {
            $layerKey = [string] ([int] $layerMetric.layer)
            $lengthByLayerTool["$layerKey|$smallTool"] = [double] $layerMetric.small_length
            $lengthByLayerTool["$layerKey|$largeTool"] = [double] $layerMetric.large_length
            $tools = [Collections.Generic.List[int]]::new()
            if ([double] $layerMetric.small_length -gt 0.01) { $tools.Add($smallTool) }
            if ([double] $layerMetric.large_length -gt 0.01) { $tools.Add($largeTool) }
            $wallToolsByLayer[$layerKey] = $tools.ToArray()
        }
        $lengthByRoleTool = @{}
        foreach ($property in $native.small_nonwall_lengths.PSObject.Properties) {
            $lengthByRoleTool["$($property.Name)|$smallTool"] = [double] $property.Value
        }
        $medianWidths = @{}
        $medianSpeeds = @{}
        $lengthByTool = @{}
        $wallTools = [Collections.Generic.List[int]]::new()
        foreach ($toolMetric in $native.tools) {
            $toolKey = [string] $toolMetric.tool
            $medianWidths[$toolKey] = [double] $toolMetric.median_width
            $medianSpeeds[$toolKey] = [double] $toolMetric.median_speed
            $lengthByTool[$toolKey] = [double] $toolMetric.wall_length
            if ([double] $toolMetric.wall_length -gt 0.01) { $wallTools.Add([int] $toolMetric.tool) }
        }
        $probeSegments = @{}
        foreach ($property in $native.probe_segments.PSObject.Properties) {
            $probeSegments[[int] $property.Name] = @($property.Value)
        }
        $analysis = [pscustomobject]@{
            Settings = $settings
            WallTools = $wallTools.ToArray()
            WallToolsByLayer = $wallToolsByLayer
            LengthByTool = $lengthByTool
            LengthByLayerTool = $lengthByLayerTool
            LengthByRoleTool = $lengthByRoleTool
            MovesByTool = @{}
            MedianWidthByTool = $medianWidths
            MedianSpeedByTool = $medianSpeeds
            ProbeLayers = @($native.probe_layers | ForEach-Object { [int] $_ })
            ProbeSegments = $probeSegments
            SparseInfillSeen = [bool] $native.sparse_infill_seen
            LayerCount = [int] $native.layer_count
            UnsupportedToolCommands = [int] $native.unsupported_tool_commands
            ExtrusionBeforeTool = [int] $native.extrusion_before_tool
            NonFiniteNumbers = [int] $native.non_finite_numbers
            OutOfBoundsExtrusions = [int] $native.out_of_bounds_extrusions
            MaxToolChangesPerLayer = [int] $native.max_tool_changes_per_layer
        }
        if ($Mode -eq 'Bunny') {
            $verdict = Test-SmallNozzleComplexCase -Case $case -Analysis $analysis
        } else {
            $verdict = Test-SmallNozzleCase -Case $case -Analysis $analysis -PrintableArea $printableArea
        }
        foreach ($errorText in $verdict.Errors) { $runtimeErrors.Add($errorText) }
    }
    $analysisTimer.Stop()

    $result = [pscustomobject]@{
        Case = $case.Name
        Factor = $case.Factor
        Level = $case.Level
        Generator = $case.Generator
        Nozzle2 = $case.Nozzle2
        Enabled = $case.Enabled
        Count = $case.Count
        Interlock = $case.Interlock
        WallLoops = $case.WallLoops
        ProbeLayerCount = $case.ProbeLayerCount
        Overlap = $case.Overlap
        SliceSeconds = [Math]::Round($timer.Elapsed.TotalSeconds, 2)
        AnalysisSeconds = [Math]::Round($analysisTimer.Elapsed.TotalSeconds, 2)
        DurationSeconds = [Math]::Round($timer.Elapsed.TotalSeconds + $analysisTimer.Elapsed.TotalSeconds, 2)
        Passed = $runtimeErrors.Count -eq 0
        Errors = $runtimeErrors.ToArray()
        WallTools = if ($analysis) { $analysis.WallTools } else { @() }
        MedianWidths = if ($analysis) { $analysis.MedianWidthByTool } else { @{} }
        MedianSpeeds = if ($analysis) { $analysis.MedianSpeedByTool } else { @{} }
        Section = if ($verdict) { $verdict.Section } else { $null }
        Zones = if ($verdict) { $verdict.Zones } else { $null }
        Complex = if ($verdict) { $verdict.Complex } else { $null }
        Gcode = if ($gcode) { $gcode.FullName } else { $null }
        Directory = $caseRoot
    }
    $result | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $caseRoot 'result.json') -Encoding UTF8
    $results.Add($result)
    Write-Output ("[{0}/{1}] {2}: {3}" -f $index, $cases.Count, $case.Name, $(if ($result.Passed) {'PASS'} else {'FAIL'}))
}

# Cross-case monotonic checks catch regressions that an isolated case cannot.
foreach ($generator in @('classic','arachne')) {
    $count1 = $results | Where-Object Case -eq "${generator}_count1" | Select-Object -First 1
    $count3 = $results | Where-Object Case -eq "${generator}_count3" | Select-Object -First 1
    if ($count1 -and $count3 -and $count1.Section -and $count3.Section) {
        $small1 = [int] ($count1.Section.Layers.SmallCount | Measure-Object -Maximum).Maximum
        $small3 = [int] ($count3.Section.Layers.SmallCount | Measure-Object -Maximum).Maximum
        if ($small3 -le $small1) {
            $count3.Passed = $false
            $count3.Errors += "Small-wall count did not increase relative to count=1 ($small1 -> $small3)"
        }
    }
}

if ($Mode -eq 'Bunny') {
    foreach ($generator in @('classic','arachne')) {
        $off = $results | Where-Object Case -eq "bunny_${generator}_interlock_off" | Select-Object -First 1
        $on = $results | Where-Object Case -eq "bunny_${generator}_interlock_on" | Select-Object -First 1
        if (-not $off -or -not $on -or -not $off.Complex -or -not $on.Complex) { continue }
        $offLayers = @{}
        foreach ($layer in $off.Complex.Layers) { $offLayers[[int]$layer.Layer] = $layer }
        $modified = [Collections.Generic.List[object]]::new()
        $contradictory = [Collections.Generic.List[int]]::new()
        foreach ($layer in $on.Complex.Layers) {
            $baseline = $offLayers[[int]$layer.Layer]
            if ($null -eq $baseline) { continue }
            $smallDecrease = [double]$baseline.SmallLength - [double]$layer.SmallLength
            $largeIncrease = [double]$layer.LargeLength - [double]$baseline.LargeLength
            if ($smallDecrease -gt 0.5 -and $largeIncrease -gt 0.5) {
                $modified.Add([pscustomobject]@{
                    Layer=[int]$layer.Layer
                    SmallDecrease=[Math]::Round($smallDecrease,3)
                    LargeIncrease=[Math]::Round($largeIncrease,3)
                })
            } elseif (($smallDecrease -gt 1.0 -and $largeIncrease -lt -1.0) -or
                     ($smallDecrease -lt -1.0 -and $largeIncrease -gt 1.0)) {
                $contradictory.Add([int]$layer.Layer)
            }
        }
        $parityCounts = @($modified | Group-Object { $_.Layer % 2 } | Sort-Object Count -Descending)
        $dominantParityShare = if ($modified.Count -gt 0) { [double]$parityCounts[0].Count / $modified.Count } else { 0.0 }
        $differential = [pscustomobject]@{
            ModifiedLayers = $modified.ToArray()
            ModifiedLayerCount = $modified.Count
            DominantParityShare = [Math]::Round($dominantParityShare,4)
            ContradictoryLayers = $contradictory.ToArray()
        }
        $on | Add-Member -NotePropertyName InterlockDifferential -NotePropertyValue $differential -Force
        if ($modified.Count -lt 3) {
            $on.Passed = $false
            $on.Errors += "Interlocking changed only $($modified.Count) bunny layers"
        }
        if ($dominantParityShare -lt 0.75) {
            $on.Passed = $false
            $on.Errors += "Interlocking changes are not confined to one layer parity ($([Math]::Round(100*$dominantParityShare,1))%)"
        }
        if ($contradictory.Count -gt 0) {
            $on.Passed = $false
            $on.Errors += "Interlocking changed small/large walls in contradictory directions on layers $($contradictory -join ',')"
        }
    }
}

$resultsPath = Join-Path $runRoot 'results.json'
$csvPath = Join-Path $runRoot 'summary.csv'
$results | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Select-Object Case,Factor,Level,Generator,Nozzle2,Enabled,Count,Interlock,WallLoops,ProbeLayerCount,Overlap,
    DurationSeconds,Passed,@{N='Errors';E={$_.Errors -join '; '}} |
    Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
$failed = @($results | Where-Object { -not $_.Passed })
$results | Select-Object Case,Passed,DurationSeconds,@{N='Errors';E={$_.Errors -join '; '}} | Format-Table -AutoSize
Write-Output "PLAN=$planPath"
Write-Output "RESULTS=$resultsPath"
Write-Output "SUMMARY=$csvPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
