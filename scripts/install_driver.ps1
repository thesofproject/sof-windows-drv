# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright 2026 The SOF Authors
#
# Sound Open Firmware (SOF) Windows Driver Installer
# Turnkey script to install test certificates, firmware, and driver packages.
#

[CmdletBinding()]
param(
    [string]$PackagePath = "",
    [switch]$SkipCert = $false,
    [switch]$SkipFw = $false,
    [switch]$Force = $false
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Auto-elevate to Administrator if running interactively without admin rights
if (-not (Test-IsAdmin)) {
    Write-Host "[SOF] Administrator privileges required. Requesting elevation..." -ForegroundColor Yellow
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($PackagePath) { $arguments += " -PackagePath `"$PackagePath`"" }
    if ($SkipCert) { $arguments += " -SkipCert" }
    if ($SkipFw) { $arguments += " -SkipFw" }
    if ($Force) { $arguments += " -Force" }

    Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments
    exit
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Sound Open Firmware (SOF) Driver Installer" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Locate package directory
$scriptDir = Split-Path -Parent $PSCommandPath
$candidates = @(
    $PackagePath,
    $scriptDir,
    (Join-Path $scriptDir "..\bin\x64\Release\package"),
    (Join-Path $scriptDir "package")
)

$targetDir = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path (Join-Path $c "sof_dsp.inf"))) {
        $targetDir = (Resolve-Path $c).Path
        break
    }
}

if (-not $targetDir) {
    Write-Error "[ERR] Could not locate driver package containing sof_dsp.inf. Specify -PackagePath."
    exit 1
}

Write-Host "[INFO] Package directory: $targetDir" -ForegroundColor Green

# 2. Check Testsigning Status
Write-Host "`n[1/5] Checking Windows Test-Signing configuration..." -ForegroundColor Yellow
$bcdOutput = & bcdedit /enum "{current}" 2>&1 | Out-String
if ($bcdOutput -notmatch "testsigning\s+(Yes|on|true)") {
    Write-Warning "[WARN] Windows Test-Signing is currently DISABLED on this machine."
    Write-Warning "       Self-signed / test-signed kernel drivers require testsigning to be enabled."
    Write-Host "       To enable, run: 'bcdedit /set testsigning on' in Admin cmd and reboot." -ForegroundColor Yellow
    if (-not $Force) {
        $response = Read-Host "Would you like to enable testsigning now? (A reboot will be required) [Y/N]"
        if ($response -match "^[Yy]") {
            & bcdedit /set testsigning on
            Write-Host "[OK] Testsigning enabled in BCD. Please reboot the machine after installation completes." -ForegroundColor Green
        }
    }
} else {
    Write-Host "[OK] Testsigning is enabled." -ForegroundColor Green
}

# 3. Install Test Certificate
if (-not $SkipCert) {
    Write-Host "`n[2/5] Installing test certificate..." -ForegroundColor Yellow
    $cerPath = Join-Path $targetDir "sof_test.cer"
    if (Test-Path $cerPath) {
        try {
            # Add to LocalMachine Root and TrustedPublisher stores
            & certutil -addstore -f "Root" $cerPath | Out-Null
            & certutil -addstore -f "TrustedPublisher" $cerPath | Out-Null
            Write-Host "[OK] Certificate installed into Root and TrustedPublisher stores." -ForegroundColor Green
        } catch {
            Write-Warning "[WARN] Failed to install certificate via certutil: $_"
        }
    } else {
        Write-Warning "[WARN] sof_test.cer not found in package directory. Skipping certificate install."
    }
} else {
    Write-Host "`n[2/5] Skipping certificate installation (-SkipCert specified)." -ForegroundColor Gray
}

# 4. Stage Firmware Binaries
if (-not $SkipFw) {
    Write-Host "`n[3/5] Setting up DSP firmware directory..." -ForegroundColor Yellow
    $systemFwDir = "C:\Windows\System32\drivers\intel\sof"
    if (-not (Test-Path $systemFwDir)) {
        New-Item -ItemType Directory -Path $systemFwDir -Force | Out-Null
        Write-Host "[OK] Created directory: $systemFwDir" -ForegroundColor Green
    }

    # Search for any firmware binaries in package or adjacent directories
    $fwSources = @(
        $targetDir,
        (Join-Path $targetDir "firmware"),
        (Join-Path $scriptDir "..\firmware")
    )
    $fwFiles = @()
    foreach ($fs in $fwSources) {
        if (Test-Path $fs) {
            $fwFiles += Get-ChildItem -Path $fs -Filter "*.ri" -File -ErrorAction SilentlyContinue
            $fwFiles += Get-ChildItem -Path $fs -Filter "*.tplg" -File -ErrorAction SilentlyContinue
        }
    }

    if ($fwFiles.Count -gt 0) {
        foreach ($f in $fwFiles) {
            Copy-Item -Path $f.FullName -Destination $systemFwDir -Force
            Write-Host "     Staged firmware/topology: $($f.Name)" -ForegroundColor Green
        }
        Write-Host "[OK] Staged $($fwFiles.Count) firmware/topology file(s) to $systemFwDir." -ForegroundColor Green
    } else {
        Write-Host "[INFO] No local *.ri or *.tplg files found in package. Ensure firmware is placed in $systemFwDir." -ForegroundColor Gray
    }
} else {
    Write-Host "`n[3/5] Skipping firmware staging (-SkipFw specified)." -ForegroundColor Gray
}

# 5. Install Driver Packages via PnPUtil
Write-Host "`n[4/5] Installing driver packages via PnPUtil..." -ForegroundColor Yellow

$infFiles = @(
    "sof_dsp.inf",
    "sof_sdca_ext.inf",
    "sof_usb_ext.inf"
)

foreach ($infName in $infFiles) {
    $infFull = Join-Path $targetDir $infName
    if (Test-Path $infFull) {
        Write-Host "     Installing $infName..." -NoNewline
        $pnpRes = & pnputil /add-driver $infFull /install 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 259 -or $LASTEXITCODE -eq 3010) {
            Write-Host " [SUCCESS]" -ForegroundColor Green
            if ($LASTEXITCODE -eq 3010) {
                Write-Host "     (Reboot required by system)" -ForegroundColor Yellow
            }
        } else {
            Write-Host " [FAILED (code $LASTEXITCODE)]" -ForegroundColor Red
            Write-Host $pnpRes -ForegroundColor Red
        }
    }
}

# 6. Verify Device Status
Write-Host "`n[5/5] Verifying PnP device status..." -ForegroundColor Yellow
Start-Sleep -Seconds 2

$devices = Get-PnpDevice | Where-Object {
    $_.FriendlyName -like "*Sound Open Firmware*" -or
    $_.Service -eq "sof_dsp" -or
    $_.InstanceId -like "*VEN_8086&DEV_*"
}

if ($devices) {
    Write-Host "`nDetected SOF / Audio DSP Hardware Devices:" -ForegroundColor Cyan
    foreach ($d in $devices) {
        $color = if ($d.Status -eq "OK") { [ConsoleColor]::Green } else { [ConsoleColor]::Yellow }
        Write-Host ("  [{0,-4}] {1,-35} (ID: {2})" -f $d.Status, $d.FriendlyName, $d.InstanceId) -ForegroundColor $color
    }
} else {
    Write-Host "[INFO] No active SOF audio DSP devices currently bound." -ForegroundColor Gray
    Write-Host "       The driver package is successfully staged in the driver store." -ForegroundColor Gray
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host " SOF Driver Installation Completed." -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
