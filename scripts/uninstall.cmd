@echo off
rem SPDX-License-Identifier: BSD-3-Clause
rem Copyright 2026 The SOF Authors
rem Sound Open Firmware 1-Click Driver Uninstaller
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_driver.ps1" %*
pause
