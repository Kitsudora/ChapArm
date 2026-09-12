[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AppExecutable,
    [string]$LauncherPath = "",
    [string]$DllPath = "",
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows PE file: $Path" }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset -gt ($stream.Length - 6)) { throw "Invalid PE header: $Path" }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
    }
}

function Assert-PlainPath([string]$Path) {
    # Reject junctions/symlinks so an application path cannot redirect writes into Windows.
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Reparse points are not supported for deployment: $($item.FullName)"
        }
        if ($item -is [System.IO.DirectoryInfo]) { $item = $item.Parent }
        else { $item = $item.Directory }
    }
}

$appPath = (Resolve-Path -LiteralPath $AppExecutable).ProviderPath
if ([System.IO.Path]::GetFileName($appPath) -ine "krita.exe" -or -not (Test-Path -LiteralPath $appPath -PathType Leaf)) {
    throw "Select the portable application's original krita.exe file."
}
Assert-PlainPath $appPath
$appDirectory = Split-Path -Parent $appPath
$windowsDirectory = [System.IO.Path]::GetFullPath($env:WINDIR).TrimEnd('\')
if ($appDirectory -ieq $windowsDirectory -or $appDirectory.StartsWith("$windowsDirectory\", [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Deploy only to a selected portable application. Windows/system directories are forbidden."
}

$launcherDestination = Join-Path $appDirectory "chaparm-krita.exe"
$localMarker = Join-Path $appDirectory "chaparm-krita.exe.local"
$dllDestination = Join-Path $appDirectory "Wintab32.dll"
$manifestPath = Join-Path $appDirectory "ChapArm.krita-deployment.json"
foreach ($process in (Get-Process -Name "krita", "chaparm-krita" -ErrorAction SilentlyContinue)) {
    # An inaccessible process path cannot establish that changing this deployment is safe.
    if (-not $process.Path -or $process.Path -ieq $appPath -or $process.Path -ieq $launcherDestination) {
        throw "Close the selected Krita application and ChapArm launcher before changing this deployment."
    }
}
if (Test-Path -LiteralPath (Join-Path $appDirectory "ChapArm.wintab-deployment.json")) {
    throw "A generic ChapArm Wintab deployment record exists. Remove that deployment with deploy-wintab.ps1 -AppExecutable '$appPath' -Remove first; the two scripts cannot manage the same provider."
}

if ($Remove) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "No ChapArm Krita deployment record exists; nothing was removed."
    }
    Assert-PlainPath $manifestPath
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.product -ne "ChapArm" -or $manifest.kind -ne "krita-launcher" -or $manifest.application -ine $appPath) {
        throw "Deployment record does not match this application; nothing was removed."
    }
    # Validate every remaining artifact before removing any of them. Missing artifacts
    # are allowed so an interrupted removal can be safely retried.
    foreach ($entry in @(
        @{ Path = $launcherDestination; Hash = $manifest.launcher_sha256 },
        @{ Path = $localMarker; Hash = $manifest.local_sha256 },
        @{ Path = $dllDestination; Hash = $manifest.dll_sha256 }
    )) {
        if ($entry.Hash -notmatch '^[0-9a-fA-F]{64}$') { throw "Invalid deployment hash; nothing was removed." }
        if (Test-Path -LiteralPath $entry.Path) {
            Assert-PlainPath $entry.Path
            if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf) -or
                (Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash -ine $entry.Hash) {
                throw "A deployed file changed: $($entry.Path). Nothing was removed."
            }
        }
    }
    foreach ($path in @($dllDestination, $launcherDestination, $localMarker)) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    Remove-Item -LiteralPath $manifestPath
    Write-Output "Removed the recorded ChapArm launcher and provider from $appDirectory. Original Krita files were not changed."
    return
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $LauncherPath) { $LauncherPath = Join-Path $repoRoot "native\build\x64\Release\chaparm-krita.exe" }
if (-not $DllPath) { $DllPath = Join-Path $repoRoot "native\build\x64\Release\Wintab32.dll" }
$launcherSource = (Resolve-Path -LiteralPath $LauncherPath).ProviderPath
$dllSource = (Resolve-Path -LiteralPath $DllPath).ProviderPath
$kritaLibrary = Join-Path $appDirectory "krita.dll"
if (-not (Test-Path -LiteralPath $kritaLibrary -PathType Leaf)) { throw "The selected portable application is missing krita.dll beside krita.exe." }
$machine = Get-PeMachine $appPath
if ($machine -notin @(0x014c, 0x8664)) { throw "Only Win32 and x64 Krita deployments are supported." }
foreach ($path in @($kritaLibrary, $launcherSource, $dllSource)) {
    if ((Get-PeMachine $path) -ne $machine) { throw "Krita, krita.dll, launcher and Wintab provider architectures must match." }
}
foreach ($path in @($launcherDestination, $localMarker, $dllDestination, $manifestPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "A launcher, .local marker, provider or deployment record already exists. Remove a previous recorded ChapArm deployment with -Remove first; existing files are never overwritten."
    }
}

$record = [ordered]@{
    product = "ChapArm"
    kind = "krita-launcher"
    application = $appPath
    application_sha256 = (Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash
    launcher_sha256 = (Get-FileHash -LiteralPath $launcherSource -Algorithm SHA256).Hash
    local_sha256 = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"
    dll_sha256 = (Get-FileHash -LiteralPath $dllSource -Algorithm SHA256).Hash
    installed_utc = [DateTime]::UtcNow.ToString("o")
}
# CreateNew never overwrites a file even if it appears after the existence checks.
# Track files immediately after creation so a failed write also rolls back its partial file.
$createdFiles = New-Object 'System.Collections.Generic.List[string]'
function Write-NewFile([string]$Path, [byte[]]$Bytes) {
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $createdFiles.Add($Path)
        $stream.Write($Bytes, 0, $Bytes.Length)
    } finally {
        $stream.Dispose()
    }
}
try {
    Write-NewFile $launcherDestination ([System.IO.File]::ReadAllBytes($launcherSource))
    Write-NewFile $localMarker ([byte[]]@())
    Write-NewFile $dllDestination ([System.IO.File]::ReadAllBytes($dllSource))
    if ((Get-FileHash -LiteralPath $launcherDestination -Algorithm SHA256).Hash -ine $record.launcher_sha256 -or
        (Get-FileHash -LiteralPath $dllDestination -Algorithm SHA256).Hash -ine $record.dll_sha256) {
        throw "A source changed during deployment."
    }
    Write-NewFile $manifestPath ([System.Text.Encoding]::UTF8.GetBytes(($record | ConvertTo-Json)))
} catch {
    $deploymentError = $_
    foreach ($path in $createdFiles) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    throw $deploymentError
}
Write-Output "Installed $launcherDestination with its application-local Wintab provider. Launch chaparm-krita.exe, then verify the consumer context before enabling pen output. Original Krita files were not changed."
