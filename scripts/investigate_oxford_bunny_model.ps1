$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

Write-Host '=== Bunny model candidates ==='
rg --files resources tests sandboxes |
    Where-Object { $_ -match '(?i)(oxford|bunny|rabbit)' } |
    ForEach-Object {
        $item = Get-Item $_
        [pscustomobject]@{
            Path = $_
            Extension = $item.Extension
            Size = $item.Length
        }
    } |
    Format-Table -AutoSize

Write-Host '=== Built resource candidates ==='
if (Test-Path 'build\src\Release\resources') {
    rg --files 'build\src\Release\resources' |
        Where-Object { $_ -match '(?i)(oxford|bunny|rabbit)' } |
        ForEach-Object {
            $item = Get-Item $_
            [pscustomobject]@{
                Path = $_
                Extension = $item.Extension
                Size = $item.Length
            }
        } |
        Format-Table -AutoSize
}
