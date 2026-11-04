# idf_env.ps1 — Activate the ESP-IDF build environment for this project.
#
# WHY THIS EXISTS:
# The stock `export.ps1` can fail on this machine: it auto-detects the shell's
# Python and/or picks up a Windows Store Python stub, which can make `idf.py`
# unavailable. We pin the exact venv python below to avoid that guessing.
#
# This script sidesteps that by calling `idf_tools.py export` with the KNOWN
# venv Python and importing the emitted environment (PATH, IDF_PYTHON_ENV_PATH,
# toolchain dirs) into the current shell.
#
# USAGE (dot-source so the env sticks in your shell):
#   . .\scripts\idf_env.ps1
#   idf.py build
#
# If the IDF install location or venv name changes, update the three vars below.

# Note: we deliberately do NOT set $ErrorActionPreference = "Stop" here, because
# idf_tools.py writes a harmless cmake-version warning to stderr, which would
# otherwise abort the script. We validate success explicitly at the end instead.

$IDF_PATH       = "C:\esp\v5.4.4\esp-idf"               # ESP-IDF v5.4.4 framework (EIM install)
$IDF_TOOLS_PATH = "C:\Espressif"                        # where tools + venvs live
$VENV_PY        = "C:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe"

if (-not (Test-Path $VENV_PY)) {
    throw "IDF venv python not found: $VENV_PY. Re-run the ESP-IDF installer or update VENV_PY in scripts/idf_env.ps1."
}

$env:IDF_PATH       = $IDF_PATH
$env:IDF_TOOLS_PATH = $IDF_TOOLS_PATH

# Emit the environment using the correct venv, then apply each KEY=VALUE line.
# (Lines that aren't KEY=VALUE — e.g. the cmake-version warning on stderr — are
# ignored.) idf_tools.py writes a harmless warning to stderr; force Continue so
# PowerShell's NativeCommandError on stderr can't abort us, and merge streams.
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$exports = & $VENV_PY "$IDF_PATH\tools\idf_tools.py" export --format key-value 2>&1
$ErrorActionPreference = $prevEAP
foreach ($line in $exports) {
    if ($line -match '^\s*([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        $name  = $Matches[1]
        $value = $Matches[2]
        # The export uses %PATH% as a placeholder for the pre-existing PATH.
        $value = $value -replace '%PATH%', [System.Environment]::GetEnvironmentVariable("PATH")
        Set-Item -Path "Env:$name" -Value $value
    }
}

if (Get-Command idf.py -ErrorAction SilentlyContinue) {
    Write-Host "ESP-IDF $($env:ESP_IDF_VERSION) environment ready. Run: idf.py build" -ForegroundColor Green
} else {
    throw "idf.py still not on PATH after export. Check scripts/idf_env.ps1 paths."
}
