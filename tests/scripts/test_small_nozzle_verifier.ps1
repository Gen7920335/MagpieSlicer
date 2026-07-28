param([string] $OutputRoot)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $RepoRoot 'scripts\lib\SmallNozzleVerifier.psm1') -Force
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\small-nozzle-verifier-selftest'
}

$result = Invoke-SmallNozzleVerifierSelfTest -WorkingDirectory $OutputRoot
$result | ConvertTo-Json -Depth 10
if (-not $result.Passed) {
    throw "Expected interlock sections 3:3 then 4:2; got $($result.Actual -join ', ')"
}
