# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright 2026 The SOF Authors

<#
.SYNOPSIS
    Reads, tails, and decodes Sound Open Firmware (SOF) Zephyr firmware logs on Windows.

.DESCRIPTION
    PowerShell wrapper around the SOF Zephyr Logging Tooling (sof_zephyr_log.py).
    Supports all SOF FW Zephyr logging options:
      - ADSP Mtrace SRAM Debug Window (Text, Dictionary v3, and MIPI Sys-T)
      - SOF Probes DMA Log Streams
      - Post-mortem DSP Crash Coredumps (sof_dsp_coredump.bin)
      - Live log file tailing without POSIX socket issues on Windows

.PARAMETER Path
    Path to the log file to parse (e.g. C:\sof_entry.txt, debug_window.bin, sof_dsp_coredump.bin).

.PARAMETER Elf
    Path to the Zephyr firmware ELF binary (zephyr.elf) for dictionary decoding.

.PARAMETER Database
    Path to a pre-generated log_database.json file.

.PARAMETER Sram
    Indicates the input is a raw ADSP SRAM Debug Window binary dump.

.PARAMETER Coredump
    Indicates the input is a SOF DSP crash coredump binary.

.PARAMETER Probe
    Indicates the input is a SOF Probes DMA stream capture.

.PARAMETER MinLevel
    Minimum log level to display (DBG, INF, WRN, ERR).

.PARAMETER Core
    Filter by DSP core ID (e.g. 0, 1, 2, 3).

.PARAMETER Module
    Filter by log module/source name.

.PARAMETER Follow
    Live tail/stream the log file on Windows.

.EXAMPLE
    .\Read-SofFwLog.ps1 -Path C:\sof_entry.txt -MinLevel WRN

.EXAMPLE
    .\Read-SofFwLog.ps1 -Path debug_window.bin -Sram -Elf \\192.168.122.1\work\sof-ptl\build-ptl\zephyr\zephyr.elf

.EXAMPLE
    .\Read-SofFwLog.ps1 -Path C:\sof_dsp_coredump.bin -Coredump
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $false)]
    [string]$Path = "C:\sof_entry.txt",

    [Parameter(Mandatory = $false)]
    [string]$Elf = "",

    [Parameter(Mandatory = $false)]
    [string]$Database = "",

    [Parameter(Mandatory = $false)]
    [switch]$Sram,

    [Parameter(Mandatory = $false)]
    [switch]$Coredump,

    [Parameter(Mandatory = $false)]
    [switch]$Probe,

    [Parameter(Mandatory = $false)]
    [ValidateSet("DBG", "INF", "WRN", "ERR")]
    [string]$MinLevel = "DBG",

    [Parameter(Mandatory = $false)]
    [int]$Core = -1,

    [Parameter(Mandatory = $false)]
    [string]$Module = "",

    [Parameter(Mandatory = $false)]
    [string]$Search = "",

    [Parameter(Mandatory = $false)]
    [switch]$Follow,

    [Parameter(Mandatory = $false)]
    [switch]$AsObject
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$pythonTool = Join-Path $scriptDir "sof_zephyr_log.py"

if (-not (Test-Path $pythonTool)) {
    Write-Error "Could not locate sof_zephyr_log.py in $scriptDir"
    return
}

$cmdArgs = @($pythonTool, "-i", $Path)

if ($Elf -and (Test-Path $Elf)) {
    $cmdArgs += @("-e", $Elf)
} elseif (-not $Elf -and (Test-Path "\\192.168.122.1\work\sof-ptl\build-ptl\zephyr\zephyr.elf")) {
    $cmdArgs += @("-e", "\\192.168.122.1\work\sof-ptl\build-ptl\zephyr\zephyr.elf")
}

if ($Database -and (Test-Path $Database)) {
    $cmdArgs += @("-d", $Database)
}

if ($Sram) { $cmdArgs += "--sram" }
if ($Coredump) { $cmdArgs += "--coredump" }
if ($Probe) { $cmdArgs += "--probe" }
if ($Follow) { $cmdArgs += "--follow" }
if ($MinLevel) { $cmdArgs += @("--level", $MinLevel) }
if ($Core -ge 0) { $cmdArgs += @("--core", "$Core") }
if ($Module) { $cmdArgs += @("--module", $Module) }
if ($Search) { $cmdArgs += @("--search", $Search) }

if ($AsObject) {
    $cmdArgs += "--json"
    $jsonRaw = & python @cmdArgs
    if ($jsonRaw) {
        return ($jsonRaw | ConvertFrom-Json)
    }
} else {
    & python @cmdArgs
}
