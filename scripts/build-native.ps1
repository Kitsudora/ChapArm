[CmdletBinding()]
param(
    [ValidateSet("x64", "Win32")][string]$Architecture = "x64",
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $repoRoot "native"
$buildDirectory = Join-Path $sourceDirectory "build\$Architecture"

& cmake -S $sourceDirectory -B $buildDirectory -A $Architecture
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed. Install Visual Studio C++ tools and the Windows SDK." }
& cmake --build $buildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }
if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory -C $Configuration --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { throw "Native probe failed." }
}

Write-Output (Join-Path $buildDirectory "$Configuration\Wintab32.dll")
