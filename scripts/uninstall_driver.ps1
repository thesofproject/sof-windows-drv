# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright 2026 The SOF Authors
#
# Sound Open Firmware (SOF) Windows Driver Uninstaller
# Turnkey script to uninstall SOF driver packages and clean up driver store.
#

[CmdletBinding()]
param(
    [switch]$RemoveCert = $false
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) {
    Write-Host "[SOF] Administrator privileges required. Requesting elevation..." -ForegroundColor Yellow
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($RemoveCert) { $arguments += " -RemoveCert" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments
    exit
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Sound Open Firmware (SOF) Driver Uninstaller" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Enumerate and delete published OEM INFs
Write-Host "`n[1/3] Scanning driver store for published SOF packages..." -ForegroundColor Yellow

$rawEnum = & pnputil /enum-drivers | Out-String
$blocks = $rawEnum -split "Published Name:"

$foundDrivers = @()
foreach ($b in $blocks) {
    if ($b -match "(oem\d+\.inf)" -and ($b -match "sof_dsp\.inf" -or $b -match "sof_sdca_ext\.inf" -or $b -match "sof_usb_ext\.inf" -or $b -match "Sound Open Firmware")) {
        $oemName = $matches[1]
        $foundDrivers += $oemName
    }
}

if ($foundDrivers.Count -gt 0) {
    Write-Host "[INFO] Found $($foundDrivers.Count) SOF driver package(s) in driver store:" -ForegroundColor Green
    foreach ($d in $foundDrivers) {
        Write-Host "     Deleting $d (/uninstall /force)..." -NoNewline
        $delRes = & pnputil /delete-driver $d /uninstall /force 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0) {
            Write-Host " [REMOVED]" -ForegroundColor Green
        } else {
            Write-Host " [FAILED (code $LASTEXITCODE)]" -ForegroundColor Red
            Write-Host $delRes -ForegroundColor Red
        }
    }
} else {
    Write-Host "[INFO] No published SOF driver packages found in driver store." -ForegroundColor Gray
}

# 2. Clean up test certificates if requested
if ($RemoveCert) {
    Write-Host "`n[2/3] Removing test certificate from system stores..." -ForegroundColor Yellow
    $certSubject = "CN=SOF Driver Test Certificate"
    
    $stores = @("Cert:\LocalMachine\Root", "Cert:\LocalMachine\TrustedPublisher")
    foreach ($s in $stores) {
        if (Test-Path $s) {
            $certs = Get-ChildItem -Path $s | Where-Object { $_.Subject -eq $certSubject }
            foreach ($c in $certs) {
                Remove-Item -Path "$s\$($c.Thumbprint)" -Force -ErrorAction SilentlyContinue
                Write-Host "     Removed cert from $s (Thumbprint: $($c.Thumbprint))" -ForegroundColor Green
            }
        }
    }
} else {
    Write-Host "`n[2/3] Preserving test certificates (specify -RemoveCert to clean up)." -ForegroundColor Gray
}

# 3. Final Verification
Write-Host "`n[3/3] Checking remaining devices..." -ForegroundColor Yellow
$remDevices = Get-PnpDevice | Where-Object { $_.Service -eq "sof_dsp" }
if ($remDevices) {
    Write-Warning "[WARN] Some devices are still associated with sof_dsp service. A system reboot may be required."
} else {
    Write-Host "[OK] All SOF drivers successfully uninstalled." -ForegroundColor Green
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host " SOF Driver Uninstallation Completed." -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
