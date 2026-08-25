param(
    [Parameter(Mandatory = $true)]
    [string] $SlicerPath,
    [string] $SourceDataRoot = (Join-Path $env:APPDATA 'MagpieSlicer'),
    [string] $WorkingRoot = (Join-Path $env:TEMP ("magpie-resin-legacy-ui-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [int] $StartupTimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'

foreach ($path in @($SlicerPath, $SourceDataRoot)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required path not found: $path"
    }
}

$legacyProcessPresets = @(
    Get-ChildItem -LiteralPath $SourceDataRoot -Recurse -File -Filter '*.json' -ErrorAction SilentlyContinue |
        Where-Object {
            $_.FullName -match '[\\/]process[\\/]' -and
            -not (Select-String -LiteralPath $_.FullName -SimpleMatch 'resin_support_tree_type' -Quiet)
        }
)
if ($legacyProcessPresets.Count -eq 0) {
    throw "No legacy process preset without resin_support_tree_type was found in $SourceDataRoot."
}

New-Item -ItemType Directory -Path $WorkingRoot -Force | Out-Null
& robocopy $SourceDataRoot $WorkingRoot /E /XD log /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -gt 7) {
    throw "Failed to clone legacy user data. Robocopy exit code: $LASTEXITCODE"
}

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class MagpieUiNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Point { public int X; public int Y; }

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr WindowFromPoint(Point point);

    [DllImport("user32.dll")]
    private static extern bool ScreenToClient(IntPtr hWnd, ref Point point);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

    public static void ClickAt(int screenX, int screenY)
    {
        Point point = new Point { X = screenX, Y = screenY };
        IntPtr target = WindowFromPoint(point);
        if (target == IntPtr.Zero) throw new InvalidOperationException("No window found at the requested point.");
        if (!ScreenToClient(target, ref point)) throw new InvalidOperationException("ScreenToClient failed.");
        IntPtr lParam = new IntPtr((point.Y << 16) | (point.X & 0xffff));
        PostMessage(target, 0x0201, new IntPtr(1), lParam);
        PostMessage(target, 0x0202, IntPtr.Zero, lParam);
    }

    public static void SendKey(IntPtr target, int virtualKey)
    {
        PostMessage(target, 0x0100, new IntPtr(virtualKey), IntPtr.Zero);
        PostMessage(target, 0x0101, new IntPtr(virtualKey), IntPtr.Zero);
    }
}
'@

function Find-ElementByAutomationId(
    [Windows.Automation.AutomationElement] $Root,
    [string] $AutomationId,
    [switch] $RequireNativeHandle
) {
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::AutomationIdProperty,
        $AutomationId
    )
    $matches = $Root.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)
    foreach ($match in $matches) {
        if (-not $RequireNativeHandle -or $match.Current.NativeWindowHandle -ne 0) {
            return $match
        }
    }
    throw "UI element not found: AutomationId=$AutomationId"
}

function Click-AutomationElement([Windows.Automation.AutomationElement] $Element) {
    $rect = $Element.Current.BoundingRectangle
    if ($rect.IsEmpty) { throw "UI element has an empty bounding rectangle." }
    [MagpieUiNative]::ClickAt(
        [int]($rect.Left + ($rect.Width / 2)),
        [int]($rect.Top + ($rect.Height / 2))
    )
}

$process = $null
try {
    $process = Start-Process -FilePath $SlicerPath -ArgumentList @('--datadir', "`"$WorkingRoot`"") -PassThru
    $deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 500
        $process.Refresh()
        if ($process.HasExited) { throw "Magpie Slicer exited during startup with code $($process.ExitCode)." }
    } while (
        ($process.MainWindowHandle -eq 0 -or $process.MainWindowTitle -notmatch 'Magpie Slicer') -and
        (Get-Date) -lt $deadline
    )
    if ($process.MainWindowHandle -eq 0 -or $process.MainWindowTitle -notmatch 'Magpie Slicer') {
        throw "Magpie Slicer did not create its stable main window in time."
    }

    [MagpieUiNative]::SetForegroundWindow([IntPtr]$process.MainWindowHandle) | Out-Null
    $mainRoot = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$process.MainWindowHandle)
    $prepareTab = Find-ElementByAutomationId $mainRoot '-30173'
    Click-AutomationElement $prepareTab
    Start-Sleep -Seconds 3

    $mainRoot = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$process.MainWindowHandle)
    $supportTab = Find-ElementByAutomationId $mainRoot '-31704'
    Click-AutomationElement $supportTab
    Start-Sleep -Seconds 2

    $mainRoot = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$process.MainWindowHandle)
    $supportType = Find-ElementByAutomationId $mainRoot '-30160' -RequireNativeHandle
    $supportTypeHandle = [IntPtr]$supportType.Current.NativeWindowHandle
    for ($index = 0; $index -lt 7; ++$index) {
        [MagpieUiNative]::SendKey($supportTypeHandle, 0x28)
        Start-Sleep -Milliseconds 700
        $process.Refresh()
        if ($process.HasExited) { throw "Magpie Slicer crashed while changing support type." }
    }

    $mainRoot = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$process.MainWindowHandle)
    $supportType = Find-ElementByAutomationId $mainRoot '-30160' -RequireNativeHandle
    $selectedType = $supportType.Current.Name
    if ($selectedType -ne 'Resin style (auto)') {
        throw "Expected Resin style (auto), but selected support type is: $selectedType"
    }

    $resinTreeType = Find-ElementByAutomationId $mainRoot '-30072' -RequireNativeHandle
    $resinTreeHandle = [IntPtr]$resinTreeType.Current.NativeWindowHandle
    [MagpieUiNative]::SendKey($resinTreeHandle, 0x28)
    Start-Sleep -Seconds 1
    $process.Refresh()
    if ($process.HasExited -or -not $process.Responding) {
        throw "Magpie Slicer stopped responding while selecting the Branching resin strategy."
    }

    $mainRoot = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$process.MainWindowHandle)
    $resinTreeType = Find-ElementByAutomationId $mainRoot '-30072' -RequireNativeHandle
    $selectedTreeType = $resinTreeType.Current.Name
    if ($selectedTreeType -ne 'Branching (experimental)') {
        throw "Expected Branching (experimental), but selected resin tree type is: $selectedTreeType"
    }

    Write-Output "LEGACY_PRESET_COUNT=$($legacyProcessPresets.Count)"
    Write-Output "SUPPORT_TYPE=$selectedType"
    Write-Output "RESIN_TREE_TYPE=$selectedTreeType"
    Write-Output "PROCESS_RESPONDING=$($process.Responding)"
    Write-Output "PASSED=1"
}
finally {
    if ($null -ne $process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
        }
    }
}
