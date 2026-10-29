<#
.SYNOPSIS
    Sets up the Python environment for tools/emotibit.py on Windows.
.DESCRIPTION
    Creates .venv-tools in the repository root and installs esptool, pyserial, PlatformIO and bleak into it.
    Then runs `emotibit.py check` to report what is still missing (driver, git, ...).
    Run:  powershell -ExecutionPolicy Bypass -File tools\setup.ps1
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$venv = Join-Path $root ".venv-tools"

$python = $null
foreach ($candidate in @("py -3", "python")) {
    try {
        $parts = $candidate.Split(" ")
        $v = & $parts[0] @($parts[1..($parts.Count - 1)] + @("--version")) 2>&1
        if ($LASTEXITCODE -eq 0 -and "$v" -match "Python 3\.(\d+)" -and [int]$Matches[1] -ge 9) { $python = $parts; break }
    } catch { }
}
if (-not $python) { throw "Python 3.9 or newer was not found. Install it from https://www.python.org/downloads/ (tick 'Add python.exe to PATH')." }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Write-Warning "git was not found. PlatformIO needs it to fetch libraries: https://git-scm.com/download/win" }

if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    Write-Host "Creating $venv ..."
    & $python[0] @($python[1..($python.Count - 1)] + @("-m", "venv", $venv))
    if ($LASTEXITCODE -ne 0) { throw "Could not create the virtual environment." }
}
$vpy = Join-Path $venv "Scripts\python.exe"
& $vpy -m pip install --quiet --upgrade pip
& $vpy -m pip install --quiet -r (Join-Path $PSScriptRoot "requirements.txt")
if ($LASTEXITCODE -ne 0) { throw "pip install failed." }

Write-Host ""
Write-Host "Environment ready. Check the computer:"
& $vpy (Join-Path $PSScriptRoot "emotibit.py") check
Write-Host ""
Write-Host "Use it with:  $vpy $(Join-Path $PSScriptRoot 'emotibit.py') <command>"
