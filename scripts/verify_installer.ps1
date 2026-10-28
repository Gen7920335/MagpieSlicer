param(
    [string]$InstallerPath,
    [int]$InstallTimeoutSeconds = 180,
    [int]$SliceTimeoutSeconds = 180,
    [int]$GuiStartupTimeoutSeconds = 45,
    [int]$GuiStabilitySeconds = 15,
    [switch]$AllowElevationPrompt,
    # The installer silently uninstalls any registered Magpie Slicer before installing.
    [switch]$AllowReplacingExistingInstall,
    # Leave the test installation in place instead of uninstalling it afterwards.
    [switch]$KeepInstall
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$verificationRoot = Join-Path $root "build\verification\installer\$stamp"
$testInstallParent = "C:\MagpieInstallTest"
$installRoot = Join-Path $testInstallParent $stamp

if ([string]::IsNullOrWhiteSpace($InstallerPath)) {
    $InstallerPath = @("build\installer") |
        ForEach-Object { Join-Path $root $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
        ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -File -Filter "MagpieSlicer_Windows_Installer_*_x64.exe" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

# CPack enables uninstall-before-install: a silent install removes whatever Magpie Slicer is
# registered, including a user's real installation. Refuse unless that is explicitly allowed.
$registeredInstalls = @()
foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryHive]::CurrentUser)) {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, $view)
        $key = $base.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Uninstall\MagpieSlicer")
        if ($null -ne $key) {
            $uninstaller = [string]$key.GetValue("UninstallString")
            if (-not [string]::IsNullOrWhiteSpace($uninstaller)) {
                $registeredInstalls += Split-Path -Parent $uninstaller.Trim('"')
            }
            $key.Close()
        }
        $base.Close()
    }
}
$foreignInstalls = @($registeredInstalls | Sort-Object -Unique | Where-Object {
    -not $_.StartsWith($testInstallParent + "\", [StringComparison]::OrdinalIgnoreCase)
})
if ($foreignInstalls.Count -gt 0 -and -not $AllowReplacingExistingInstall) {
    throw "Magpie Slicer is installed at $($foreignInstalls -join ', '). The installer would uninstall it first. Pass -AllowReplacingExistingInstall to accept that."
}
if (-not (Test-Path -LiteralPath $InstallerPath -PathType Leaf)) {
    throw "Installer not found: $InstallerPath"
}

New-Item -ItemType Directory -Path $verificationRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $installRoot) -Force | Out-Null

$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
$isAdmin = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)

Write-Output "Installer=$InstallerPath"
Write-Output "InstallRoot=$installRoot"
Write-Output "CurrentShellIsAdministrator=$isAdmin"

if (-not $isAdmin -and -not $AllowElevationPrompt) {
    throw "The NSIS installer requires administrator rights. Run this script from an elevated shell or pass -AllowElevationPrompt while a user is available to approve UAC."
}

$arguments = @(
    "/S",
    "/D=$installRoot"
)
$startInfo = @{
    FilePath = $InstallerPath
    ArgumentList = $arguments
    PassThru = $true
}
if (-not $isAdmin) {
    $startInfo.Verb = "RunAs"
}

$installerProcess = Start-Process @startInfo
if (-not $installerProcess.WaitForExit($InstallTimeoutSeconds * 1000)) {
    Stop-Process -Id $installerProcess.Id -Force -ErrorAction SilentlyContinue
    throw "Installer exceeded the $InstallTimeoutSeconds second timeout."
}
if ($installerProcess.ExitCode -ne 0) {
    throw "Installer exited with code $($installerProcess.ExitCode)."
}

# Every check below runs inside this try so a failed check still removes the test install.
try {
$installedExe = Join-Path $installRoot "magpie-slicer.exe"
$installedDll = Join-Path $installRoot "MagpieSlicer.dll"
$installedResources = Join-Path $installRoot "resources"
$installedGuide = Join-Path $installedResources "web\guide\0\index.html"
foreach ($required in @($installedExe, $installedDll, $installedResources, $installedGuide)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Installed payload is incomplete: $required"
    }
}

# MAGPIE: the official mods are bundled: a package folder each, with its library and, in a signed package, a SIGNATURE.
$officialMods = @("cura_support", "half_layer", "layer_time_blend", "low_temp_calibration", "low_temp_interface",
                  "mixed_support", "multi_nozzle_walls", "resin_support", "snapmaker_u1", "support_extras")
$bundledModsDir = Join-Path $installedResources "mods"
$signedMods = 0
foreach ($id in $officialMods) {
    foreach ($required in @((Join-Path $bundledModsDir "$id\mod.json"), (Join-Path $bundledModsDir "$id\bin\win-x64\$id.dll"))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Bundled official mod is incomplete: $required"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $bundledModsDir "$id\SIGNATURE") -PathType Leaf) {
        $signedMods++
    }
}
Write-Output "BundledMods=$($officialMods.Count) (signed $signedMods)"

$payloadFiles = @(Get-ChildItem -LiteralPath $installRoot -Recurse -File)
$payloadBytes = ($payloadFiles | Measure-Object Length -Sum).Sum
$exeHash = (Get-FileHash -LiteralPath $installedExe -Algorithm SHA256).Hash
$dllHash = (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash

Write-Output "InstalledFiles=$($payloadFiles.Count)"
Write-Output "InstalledBytes=$payloadBytes"
Write-Output "InstalledExeSHA256=$exeHash"
Write-Output "InstalledDllSHA256=$dllHash"

$guiDataDir = Join-Path $verificationRoot "gui-data"
New-Item -ItemType Directory -Path $guiDataDir -Force | Out-Null
$guiProcess = Start-Process `
    -FilePath $installedExe `
    -ArgumentList @("--datadir", $guiDataDir) `
    -WorkingDirectory $installRoot `
    -PassThru
$guiDeadline = (Get-Date).AddSeconds($GuiStartupTimeoutSeconds)
$windowReadyAt = $null
try {
    while ((Get-Date) -lt $guiDeadline) {
        Start-Sleep -Milliseconds 250
        $guiProcess.Refresh()
        if ($guiProcess.HasExited) {
            throw "Installed GUI exited during startup with code $($guiProcess.ExitCode)."
        }
        if ($guiProcess.MainWindowHandle -ne 0 -and $guiProcess.Responding) {
            if ($null -eq $windowReadyAt) {
                $windowReadyAt = Get-Date
            }
            if (((Get-Date) - $windowReadyAt).TotalSeconds -ge $GuiStabilitySeconds) {
                break
            }
        } else {
            $windowReadyAt = $null
        }
    }

    $guiProcess.Refresh()
    if ($null -eq $windowReadyAt -or $guiProcess.HasExited -or -not $guiProcess.Responding) {
        throw "Installed GUI did not remain responsive for $GuiStabilitySeconds seconds within the $GuiStartupTimeoutSeconds second timeout."
    }
    Write-Output "InstalledGuiStartup=PASS"
    # The log of the run says how many mods loaded; every bundled one must have, with its native code.
    $loaded = Get-ChildItem -LiteralPath (Join-Path $guiDataDir "log") -File -ErrorAction SilentlyContinue |
        Select-String -Pattern "Loaded (\d+) mod\(s\), (\d+) with native code" | Select-Object -Last 1
    if ($null -eq $loaded -or [int]$loaded.Matches[0].Groups[2].Value -lt $officialMods.Count) {
        throw "The installed application did not load the $($officialMods.Count) bundled mods: $($loaded.Line)"
    }
    Write-Output "InstalledModsLoaded=$($loaded.Matches[0].Groups[2].Value)"
} finally {
    if (-not $guiProcess.HasExited) {
        $null = $guiProcess.CloseMainWindow()
        if (-not $guiProcess.WaitForExit(10000)) {
            Stop-Process -Id $guiProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

$sliceOutput = Join-Path $verificationRoot "cura-support-geometry"
& (Join-Path $PSScriptRoot "verify_cura_support_geometry.ps1") `
    -SlicerPath $installedExe `
    -OutputRoot $sliceOutput `
    -SliceTimeoutSeconds $SliceTimeoutSeconds
if (-not $?) {
    throw "Installed executable slicing verification failed."
}

$gcodes = @(Get-ChildItem -LiteralPath $sliceOutput -Recurse -File -Filter "*.gcode")
if ($gcodes.Count -lt 3) {
    throw "Expected at least 3 verification G-code files, found $($gcodes.Count)."
}
foreach ($gcode in $gcodes) {
    if ($gcode.Length -le 0) {
        throw "Empty verification G-code: $($gcode.FullName)"
    }
}

$report = [ordered]@{
    passed = $true
    installer = [IO.Path]::GetFullPath($InstallerPath)
    install_root = $installRoot
    installed_file_count = $payloadFiles.Count
    installed_bytes = $payloadBytes
    installed_exe_sha256 = $exeHash
    installed_dll_sha256 = $dllHash
    gui_startup_stability_seconds = $GuiStabilitySeconds
    gcode_count = $gcodes.Count
    gcode_files = @($gcodes | ForEach-Object FullName)
}
$reportPath = Join-Path $verificationRoot "installer-verification.json"
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding UTF8

Write-Output "INSTALLER_VERIFICATION=PASS"
Write-Output "REPORT_PATH=$reportPath"
} finally {
    $uninstaller = Join-Path $installRoot "Uninstall.exe"
    if (-not $KeepInstall -and (Test-Path -LiteralPath $uninstaller -PathType Leaf)) {
        # _?= runs the uninstaller in place so this script can wait for it.
        $uninstallInfo = @{
            FilePath = $uninstaller
            ArgumentList = @("/S", "_?=$installRoot")
            PassThru = $true
        }
        if (-not $isAdmin) {
            $uninstallInfo.Verb = "RunAs"
        }
        $uninstallProcess = Start-Process @uninstallInfo
        if ($uninstallProcess.WaitForExit($InstallTimeoutSeconds * 1000) -and $uninstallProcess.ExitCode -eq 0) {
            Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction SilentlyContinue
            Write-Output "TestInstallRemoved=$installRoot"
        } else {
            Write-Warning "Uninstalling the test install at $installRoot failed; remove it from Apps & features."
        }
    }
}
