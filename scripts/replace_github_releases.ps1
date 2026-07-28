param(
    [string]$Repository = "Gen7920335/OrcaSlicer",
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath,
    [string]$Tag = "trinterface-cura-support-v2.5.0-dev-20260723",
    [string]$Title = "OrcaSlicer TrInterface Cura Support 2.5.0-dev"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$installer = Get-Item -LiteralPath $InstallerPath -ErrorAction Stop
$installerHash = (Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupDir = Join-Path $root "backups\github-releases-before-replace-$stamp"
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

Write-Output "Repository=$Repository"
Write-Output "Installer=$($installer.FullName)"
Write-Output "InstallerSHA256=$installerHash"

$allReleases = [Collections.Generic.List[object]]::new()
$page = 1
while ($true) {
    $jsonLines = & gh api "repos/$Repository/releases?per_page=100&page=$page"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to list GitHub releases on page $page."
    }
    $parsedItems = ($jsonLines -join "`n") | ConvertFrom-Json
    $pageCount = 0
    foreach ($item in $parsedItems) {
        $allReleases.Add($item)
        $pageCount++
    }
    if ($pageCount -eq 0) {
        break
    }
    if ($pageCount -lt 100) {
        break
    }
    $page++
}

$allReleases |
    ConvertTo-Json -Depth 20 |
    Set-Content -LiteralPath (Join-Path $backupDir "releases.json") -Encoding UTF8
Write-Output "ReleaseMetadataBackup=$backupDir"
Write-Output "ExistingReleaseCount=$($allReleases.Count)"

foreach ($release in $allReleases) {
    $deleted = $false
    for ($attempt = 1; $attempt -le 3 -and -not $deleted; $attempt++) {
        & gh api -X DELETE "repos/$Repository/releases/$($release.id)"
        if ($LASTEXITCODE -eq 0) {
            $deleted = $true
            Write-Output "DeletedRelease=$($release.tag_name)"
        } elseif ($attempt -lt 3) {
            Start-Sleep -Seconds (2 * $attempt)
        }
    }
    if (-not $deleted) {
        throw "Failed to delete release: $($release.tag_name)"
    }
}

$currentBranch = (& git -C $root branch --show-current).Trim()
$targetBranch = $currentBranch
& gh api "repos/$Repository/branches/$targetBranch" *> $null
if ($LASTEXITCODE -ne 0) {
    $repoInfo = & gh api "repos/$Repository" | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to resolve the repository default branch."
    }
    $targetBranch = $repoInfo.default_branch
}

$notes = @"
Windows x64 installer for the integrated TrInterface build.

- Cura-style normal support
- Triangle support interface and sublayer controls
- Multi-nozzle features included
- Installer installation verified
- Installed executable Cura support slicing verification: 3/3 passed
- SHA-256: $installerHash
"@

& gh release create $Tag $installer.FullName `
    --repo $Repository `
    --target $targetBranch `
    --title $Title `
    --notes $notes `
    --latest
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create the replacement GitHub release."
}

$remainingJsonLines = & gh api "repos/$Repository/releases?per_page=100&page=1"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to verify the replacement release."
}
$remaining = [Collections.Generic.List[object]]::new()
$parsedRemaining = ($remainingJsonLines -join "`n") | ConvertFrom-Json
foreach ($release in $parsedRemaining) {
    $remaining.Add($release)
}
if ($remaining.Count -ne 1) {
    throw "Expected exactly one GitHub release, found $($remaining.Count)."
}
$assets = [Collections.Generic.List[object]]::new()
foreach ($asset in $remaining[0].assets) {
    $assets.Add($asset)
}
if ($assets.Count -ne 1 -or $assets[0].name -ne $installer.Name) {
    throw "Replacement release does not contain exactly the expected installer asset."
}

Write-Output "RELEASE_REPLACEMENT=PASS"
Write-Output "ReleaseCount=$($remaining.Count)"
Write-Output "AssetCount=$($assets.Count)"
Write-Output "ReleaseUrl=$($remaining[0].html_url)"
Write-Output "AssetName=$($assets[0].name)"
Write-Output "AssetSize=$($assets[0].size)"
