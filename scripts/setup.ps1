[CmdletBinding()]
param([string]$Python = "")

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$venvPython = Join-Path $repoRoot ".venv\Scripts\python.exe"

if (-not (Test-Path -LiteralPath $venvPython)) {
    if ($Python) {
        & $Python -m venv (Join-Path $repoRoot ".venv")
    } elseif (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3 -m venv (Join-Path $repoRoot ".venv")
    } else {
        & python -m venv (Join-Path $repoRoot ".venv")
    }
    if ($LASTEXITCODE -ne 0) { throw "Creating the virtual environment failed." }
}

& $venvPython -c "import sys; assert sys.version_info >= (3, 11), 'Python 3.11 or newer is required'"
if ($LASTEXITCODE -ne 0) { throw "Use Python 3.11+; remove the incompatible .venv and rerun setup." }
& $venvPython -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) { throw "Updating pip failed." }
& $venvPython -m pip install -e "${repoRoot}[dev]"
if ($LASTEXITCODE -ne 0) { throw "Installing ChapArm failed." }

Write-Output "Installed ChapArm. Run: .\.venv\Scripts\chaparm.exe serve"
