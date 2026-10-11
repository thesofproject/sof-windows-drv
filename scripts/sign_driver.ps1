# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright 2026 The SOF Authors
#
# Sound Open Firmware (SOF) Windows Driver Test Signing & Packaging Script
#

param(
    [string]$Config = "Release",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
$BinDir = Join-Path $RootDir "bin\$Platform\$Config"
$PackageDir = Join-Path $BinDir "package"
$InxPath = Join-Path $RootDir "src\driver\sof_dsp.inx"
$ExtInxPath = Join-Path $RootDir "src\driver\sof_sdca_ext.inx"
$UsbExtInxPath = Join-Path $RootDir "src\driver\sof_usb_ext.inx"
$SysPath = Join-Path $BinDir "sof_dsp.sys"

$Inf2CatPath = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe"
$SignToolPath = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Packaging & Signing SOF Windows Audio Driver ($Platform|$Config)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Ensure staging directory exists
if (Test-Path $PackageDir) {
    Remove-Item -Path $PackageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null

# 2. Copy driver binary
if (-not (Test-Path $SysPath)) {
    throw "Driver binary not found: $SysPath. Please build the solution first."
}
Copy-Item -Path $SysPath -Destination $PackageDir -Force
Write-Host "[1/5] Copied sof_dsp.sys to staging directory." -ForegroundColor Green

# 3. Generate INFs from INX templates
$InfPath = Join-Path $PackageDir "sof_dsp.inf"
$inxContent = Get-Content $InxPath -Raw
$infContent = $inxContent -replace '\$ARCH\$', 'amd64'
Set-Content -Path $InfPath -Value $infContent -Encoding Ascii
Write-Host "[2/5] Generated sof_dsp.inf from sof_dsp.inx." -ForegroundColor Green

if (Test-Path $ExtInxPath) {
    $ExtInfPath = Join-Path $PackageDir "sof_sdca_ext.inf"
    $extInxContent = Get-Content $ExtInxPath -Raw
    $extInfContent = $extInxContent -replace '\$ARCH\$', 'amd64'
    Set-Content -Path $ExtInfPath -Value $extInfContent -Encoding Ascii
    Write-Host "      Generated sof_sdca_ext.inf from sof_sdca_ext.inx." -ForegroundColor Green
}

if (Test-Path $UsbExtInxPath) {
    $UsbExtInfPath = Join-Path $PackageDir "sof_usb_ext.inf"
    $usbExtInxContent = Get-Content $UsbExtInxPath -Raw
    $usbExtInfContent = $usbExtInxContent -replace '\$ARCH\$', 'amd64'
    Set-Content -Path $UsbExtInfPath -Value $usbExtInfContent -Encoding Ascii
    Write-Host "      Generated sof_usb_ext.inf from sof_usb_ext.inx." -ForegroundColor Green
}

# 4. Create or obtain test certificate
$CertSubject = "CN=SOF Driver Test Certificate"
$cert = Get-ChildItem -Path Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1

if (-not $cert) {
    Write-Host "Creating self-signed test code signing certificate..." -ForegroundColor Yellow
    $cert = New-SelfSignedCertificate -Type CodeSigningCert `
                                      -Subject $CertSubject `
                                      -CertStoreLocation "Cert:\CurrentUser\My" `
                                      -NotAfter (Get-Date).AddYears(5)
}

# Export public certificate to package folder
$certBytes = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
$cerPath = Join-Path $PackageDir "sof_test.cer"
[System.IO.File]::WriteAllBytes($cerPath, $certBytes)
Write-Host "[3/5] Exported test certificate to $cerPath" -ForegroundColor Green

# 5. Generate catalog files via Inf2Cat
Write-Host "Generating catalog files via Inf2Cat..." -ForegroundColor Yellow
& $Inf2CatPath /driver:"$PackageDir" /os:10_X64,Server10_X64
if ($LASTEXITCODE -ne 0) {
    Write-Warning "Inf2Cat exited with code $LASTEXITCODE."
}
Write-Host "[4/5] Generated catalog files." -ForegroundColor Green

# 6. Sign driver binary and all catalog files
Write-Host "Signing driver package files..." -ForegroundColor Yellow
& $SignToolPath sign /v /s My /n "SOF Driver Test Certificate" /fd SHA256 "$PackageDir\sof_dsp.sys"
Get-ChildItem -Path $PackageDir -Filter "*.cat" | ForEach-Object {
    & $SignToolPath sign /v /s My /n "SOF Driver Test Certificate" /fd SHA256 $_.FullName
}
Write-Host "[5/5] Signed sof_dsp.sys and catalog files." -ForegroundColor Green

# 7. Copy installer tools to package directory
Copy-Item -Path (Join-Path $PSScriptRoot "install_driver.ps1") -Destination $PackageDir -Force
Copy-Item -Path (Join-Path $PSScriptRoot "uninstall_driver.ps1") -Destination $PackageDir -Force
Copy-Item -Path (Join-Path $PSScriptRoot "install.cmd") -Destination $PackageDir -Force
Copy-Item -Path (Join-Path $PSScriptRoot "uninstall.cmd") -Destination $PackageDir -Force
Write-Host "      Copied turnkey installer and uninstaller scripts to package directory." -ForegroundColor Green

# 8. Verify Signature
Write-Host "`nVerifying digital signatures:" -ForegroundColor Yellow
$filesToVerify = @("$PackageDir\sof_dsp.sys") + (Get-ChildItem -Path $PackageDir -Filter "*.cat").FullName
Get-AuthenticodeSignature $filesToVerify | Format-Table -AutoSize

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Driver package ready in: $PackageDir" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
