<#
.SYNOPSIS
  Build (and optionally flash) the firmware with Arduino CLI.

.DESCRIPTION
  Uses the "cores3" profile in src/sketch.yaml, which pins the Espressif Arduino core
  and the libraries. Each variant builds into its own folder under build/ (git-ignored),
  so normal and test firmware never share object files.

.EXAMPLE
  .\scripts\build.ps1                    # build
  .\scripts\build.ps1 -Upload            # build and flash to COM6
  .\scripts\build.ps1 -Upload -Port COM7
  .\scripts\build.ps1 -Fault -Upload     # fault-injection test firmware (serial commands)
  .\scripts\build.ps1 -Placeholder       # CI-style check without src/secrets.h (never flash it)
#>
param(
    [switch]$Upload,
    [string]$Port = "COM6",
    [switch]$Fault,
    [switch]$Placeholder
)
$ErrorActionPreference = "Stop"

if ($Placeholder -and $Upload) { throw "-Placeholder firmware has published credentials: it must not be flashed." }

$root   = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $root "src"
$defines = @()
if ($Fault)       { $defines += "-DPPS_FAULT_INJECTION" }
if ($Placeholder) { $defines += "-DALLOW_PLACEHOLDER_SECRETS" }
$variant   = if ($Fault) { "fault" } elseif ($Placeholder) { "placeholder" } else { "release" }
$buildPath = Join-Path $root "build\$variant"

# The toolchain archives are ~400 MB; the default network timeout aborts the first download.
if (-not $env:ARDUINO_NETWORK_CONNECTION_TIMEOUT) { $env:ARDUINO_NETWORK_CONNECTION_TIMEOUT = "3600s" }

$compileArgs = @("compile", "--profile", "cores3", "--warnings", "default", "--build-path", $buildPath)
# Not build.defines: the board menu uses it for -DBOARD_HAS_PSRAM, and overriding it drops that.
# compiler.{c,cpp}.extra_flags are empty in the ESP32 platform and not set by any board option.
if ($defines) {
    $d = $defines -join ' '
    $compileArgs += @("--build-property", "compiler.c.extra_flags=$d", "--build-property", "compiler.cpp.extra_flags=$d")
}
$compileArgs += $sketch

Write-Host "arduino-cli $($compileArgs -join ' ')"
& arduino-cli @compileArgs
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

if ($Upload) {
    & arduino-cli upload --profile cores3 -p $Port --input-dir $buildPath $sketch
    if ($LASTEXITCODE -ne 0) { throw "upload failed ($LASTEXITCODE)" }
}
