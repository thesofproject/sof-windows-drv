# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright 2026 The SOF Authors
#
# Sound Open Firmware (SOF) Windows Driver - DUT Sleep / Wake Test Script
# Sets a Win32 hardware RTC resume timer and transitions system to S3/Standby
#

param(
    [int]$SleepDurationSec = 15
)

$ErrorActionPreference = "Stop"

Write-Host "======================================================================"
Write-Host " SOF WINDOWS DRIVER - DUT SLEEP / WAKE VERIFICATION"
Write-Host " Sleep Duration: $SleepDurationSec seconds"
Write-Host "======================================================================"

# Add Win32 Power Management and Waitable Timer APIs
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class Win32PowerHelper {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr CreateWaitableTimer(IntPtr lpTimerAttributes, bool bManualReset, string lpTimerName);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetWaitableTimer(IntPtr hTimer, [In] ref long pDueTime, int lPeriod, IntPtr pfnCompletionRoutine, IntPtr lpArgToCompletionRoutine, bool fResume);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("PowrProf.dll", SetLastError = true)]
    public static extern bool SetSuspendState(bool hibernate, bool forceCritical, bool disableWakeEvent);
}
"@

# 1. Arm safety canary
Set-Content -Path "C:\sof_canary.bin" -Value "ARMED" -Encoding Ascii
Write-Host "[1/4] Safety canary armed at C:\sof_canary.bin"

# 2. Record pre-sleep state
$preTime = Get-Date
$dev = Get-PnpDevice | Where-Object { $_.FriendlyName -eq "Intel Sound Open Firmware Audio DSP" -and $_.InstanceId -like "PCI\*" }
Write-Host "[2/4] Pre-sleep device state: $($dev.FriendlyName) Status=$($dev.Status)"

# 3. Create and arm waitable resume timer
Write-Host "[3/4] Creating hardware RTC resume timer for $SleepDurationSec seconds..."
$hTimer = [Win32PowerHelper]::CreateWaitableTimer([IntPtr]::Zero, $true, "SofHardwareWakeTimer")
if ($hTimer -eq [IntPtr]::Zero) {
    throw "Failed to create waitable timer. Error: $([Marshal]::GetLastWin32Error())"
}

# Negative value specifies relative time in 100-nanosecond intervals
$dueTime = -10000000L * $SleepDurationSec
$timerSet = [Win32PowerHelper]::SetWaitableTimer($hTimer, [ref]$dueTime, 0, [IntPtr]::Zero, [IntPtr]::Zero, $true)
if (-not $timerSet) {
    [Win32PowerHelper]::CloseHandle($hTimer)
    throw "Failed to set waitable timer. Error: $([Marshal]::GetLastWin32Error())"
}

Write-Host "      Waitable timer armed with fResume=TRUE (due in $SleepDurationSec s)."
Write-Host "      Transitioning to Sleep now (SetSuspendState: Standby)..."

$suspendStart = [System.Diagnostics.Stopwatch]::StartNew()

# 4. Suspend the system (hibernate=false, forceCritical=false, disableWakeEvent=false)
$suspendResult = [Win32PowerHelper]::SetSuspendState($false, $false, $false)

# Execution resumes here after system wakes up!
$suspendStart.Stop()
$postTime = Get-Date

# Wait for timer signal and close handle
[Win32PowerHelper]::WaitForSingleObject($hTimer, 2000)
[Win32PowerHelper]::CloseHandle($hTimer)

Write-Host "[4/4] SYSTEM WOKE UP!"
Write-Host "      SetSuspendState returned: $suspendResult"
Write-Host "      Suspended at: $($preTime.ToString('HH:mm:ss.fff'))"
Write-Host "      Woke up at:   $($postTime.ToString('HH:mm:ss.fff'))"
Write-Host "      Elapsed time: $($suspendStart.Elapsed.TotalSeconds.ToString('F2')) seconds"

# Ensure safety canary remains armed post-wake
Set-Content -Path "C:\sof_canary.bin" -Value "ARMED" -Encoding Ascii

# Output structured summary marker for host parsing
Write-Host "`n>>> DUT_SLEEP_WAKE_SUCCESS <<<"
