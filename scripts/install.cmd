@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem Copyright 2026 The SOF Authors
rem Sound Open Firmware 1-Click Driver Installer
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_driver.ps1" %*
pause
