param(
    [string]$GuideRoot
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($GuideRoot)) {
    $GuideRoot = Join-Path $root "resources\web\guide"
}
$GuideRoot = [IO.Path]::GetFullPath($GuideRoot)

function Get-Text([string]$relativePath) {
    $path = Join-Path $GuideRoot $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Setup guide file is missing: $path"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Assert-CssRule(
    [string]$css,
    [string]$selector,
    [string[]]$requiredDeclarations
) {
    $selectorPattern = [regex]::Escape($selector)
    $match = [regex]::Match(
        $css,
        "(?ms)$selectorPattern\s*\{(?<body>[^}]*)\}"
    )
    if (-not $match.Success) {
        throw "CSS selector is missing: $selector"
    }

    $body = $match.Groups["body"].Value
    foreach ($declaration in $requiredDeclarations) {
        if ($body -notmatch $declaration) {
            throw "CSS selector '$selector' is missing declaration pattern: $declaration"
        }
    }
}

$printerHtml = Get-Text "21\index.html"
$printerCss = Get-Text "21\common.css"
$filamentHtml = Get-Text "22\index.html"
$filamentCss = Get-Text "22\common.css"

if ($printerHtml -notmatch 'id="Content"') {
    throw "Printer selection scroll container is missing."
}
if ($filamentHtml -notmatch 'class="cbr-content[^"]*"\s+id="MachineList"' -or
    $filamentHtml -notmatch 'class="cbr-content[^"]*"\s+id="FilatypeList"' -or
    $filamentHtml -notmatch 'class="cbr-content[^"]*"\s+id="VendorList"' -or
    $filamentHtml -notmatch 'class="cbr-content[^"]*"\s+id="ItemBlockArea"') {
    throw "One or more filament selection scroll containers are missing."
}

Assert-CssRule $printerCss "#Content" @(
    "height\s*:\s*auto",
    "min-height\s*:\s*0",
    "flex\s*:\s*1\s+1\s+auto",
    "overflow-y\s*:\s*auto"
)
Assert-CssRule $filamentCss "#Content" @(
    "height\s*:\s*auto",
    "min-height\s*:\s*0",
    "flex\s*:\s*1\s+1\s+auto",
    "overflow\s*:\s*hidden"
)
Assert-CssRule $filamentCss ".cbr-browser-container" @(
    "grid-template-rows\s*:\s*minmax\(0,\s*210px\)\s+minmax\(0,\s*1fr\)",
    "min-height\s*:\s*0"
)
Assert-CssRule $filamentCss ".cbr-column" @(
    "min-height\s*:\s*0",
    "overflow\s*:\s*hidden"
)
Assert-CssRule $filamentCss ".cbr-content" @(
    "flex\s*:\s*1\s+1\s+auto",
    "min-height\s*:\s*0",
    "overflow-y\s*:\s*auto"
)

$pageScripts = @(
    Get-Text "21\21.js"
    Get-Text "21\common.js"
    Get-Text "22\22.js"
    Get-Text "22\common.js"
) -join "`n"
if ($pageScripts -match '(?i)(wheel|mousewheel).*(preventDefault)|preventDefault.*(wheel|mousewheel)') {
    throw "Setup guide page scripts intercept mouse-wheel scrolling."
}

Write-Output "SETUP_GUIDE_SCROLL_CONTRACT=PASS"
Write-Output "GUIDE_ROOT=$GuideRoot"
