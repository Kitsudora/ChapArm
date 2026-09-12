[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AppExecutable,
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

$appPath = (Resolve-Path -LiteralPath $AppExecutable).ProviderPath
if ([System.IO.Path]::GetExtension($appPath) -ine ".exe") { throw "Select the target application's .exe file." }
$appDirectory = Split-Path -Parent $appPath
$windowsDirectory = [System.IO.Path]::GetFullPath($env:WINDIR).TrimEnd('\')
if ($appDirectory -ieq $windowsDirectory -or $appDirectory.StartsWith("$windowsDirectory\", [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Deploy only to an application directory. Windows/system directories are forbidden."
}
foreach ($process in (Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($appPath)) -ErrorAction SilentlyContinue)) {
    # Be conservative if Windows does not permit inspection of the process path.
    if (-not $process.Path -or $process.Path -ieq $appPath) {
        throw "Close the selected application before changing its Wintab provider."
    }
}

$destination = Join-Path $appDirectory "Wintab32.dll"
$manifestPath = Join-Path $appDirectory "ChapArm.wintab-deployment.json"
if ($Remove) {
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw "No ChapArm deployment record exists; nothing was removed." }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.product -ne "ChapArm" -or $manifest.application -ine $appPath) {
        throw "Deployment record does not match this application; nothing was removed."
    }
    if (Test-Path -LiteralPath $destination) {
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ine $manifest.sha256) {
            throw "Wintab32.dll changed since deployment; refusing to remove it."
        }
        Remove-Item -LiteralPath $destination
    }
    Remove-Item -LiteralPath $manifestPath
    Write-Output "Removed the recorded ChapArm provider from $appDirectory"
    return
}

if (-not $DllPath) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $DllPath = Join-Path $repoRoot "native\build\x64\Release\Wintab32.dll"
}
$source = (Resolve-Path -LiteralPath $DllPath).ProviderPath
if ((Get-PeMachine $appPath) -ne (Get-PeMachine $source)) {
    throw "DLL and application architectures differ. Build x64 for a 64-bit application or Win32 for a 32-bit application."
}
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $manifestPath)) {
    throw "A provider or deployment record already exists. Remove a previous ChapArm deployment with -Remove first; existing tablet providers are never overwritten."
}
$record = [ordered]@{
    product = "ChapArm"
    application = $appPath
    sha256 = $sourceHash
    installed_utc = [DateTime]::UtcNow.ToString("o")
}
Copy-Item -LiteralPath $source -Destination $destination
try {
    $record | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
} catch {
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ieq $sourceHash) {
        Remove-Item -LiteralPath $destination
    }
    throw
}
Write-Output "Installed ChapArm beside $appPath. Verify a consumer context after launch; application loading policy may ignore this DLL."
