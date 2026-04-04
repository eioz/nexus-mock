<#
.SYNOPSIS
    Build script for nexus-mock.
.EXAMPLE
    .\build.ps1                     # Debug x64 (default)
    .\build.ps1 -Config Release     # Release x64
    .\build.ps1 -Clean              # Clean build artifacts
    .\build.ps1 -Rebuild            # Full rebuild
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",

    [switch]$Clean,
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

$VsWherePath = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWherePath)) {
    Write-Error "vswhere not found. Install Visual Studio or Build Tools."
    exit 1
}

$MSBuildPath = & $VsWherePath -latest -products * -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $MSBuildPath) {
    Write-Error "MSBuild not found. Install Visual Studio or Build Tools."
    exit 1
}

$SolutionPath = Join-Path $PSScriptRoot "nexus_mock.sln"
$BuildArgs = @($SolutionPath, "/p:Configuration=$Config", "/p:Platform=x64", "/m", "/v:minimal")

if ($Clean) {
    $BuildArgs += "/t:Clean"
}
elseif ($Rebuild) {
    $BuildArgs += "/t:Rebuild"
}

Write-Host "[$Config] " -NoNewline -ForegroundColor Cyan
if ($Clean) { Write-Host "Cleaning..." }
elseif ($Rebuild) { Write-Host "Rebuilding..." }
else { Write-Host "Building..." }

& $MSBuildPath @BuildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
