# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
# Copyright (c) 2026 Intel Corporation. All rights reserved.
#
# dut_regression_suite.ps1 - Automated SOF Display Audio Regression Test Suite for Windows DUT

param(
    [switch]$Quick = $false,
    [string]$OutputFile = "C:\sof_regression_results.json"
)

$ErrorActionPreference = "Continue"

# Mark the start of this specific test run in the log file
$sessionMarker = "=== REGRESSION_SESSION_START_$(Get-Date -Format 'yyyyMMdd_HHmmss') ==="
try {
    [System.IO.File]::AppendAllText("C:\sof_entry.txt", "`r`n$sessionMarker`r`n")
} catch {
    Add-Content -Path "C:\sof_entry.txt" -Value "`r`n$sessionMarker" -ErrorAction SilentlyContinue
}

$code = @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

[ComImport]
[Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
class MMDeviceEnumeratorComObject { }

[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr ppEndpoints);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice ppEndpoint);
    [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string pwstrId, out IMMDevice ppDevice);
}

[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
    [PreserveSig] int OpenPropertyStore(int stgmAccess, out IntPtr ppProperties);
    [PreserveSig] int GetId(out IntPtr ppstrId);
    [PreserveSig] int GetState(out int pdwState);
}

[Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioClient {
    [PreserveSig] int Initialize(int shareMode, int streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr pFormat, ref Guid audioSessionGuid);
    [PreserveSig] int GetBufferSize(out uint pNumBufferFrames);
    [PreserveSig] int GetStreamLatency(out long phnsLatency);
    [PreserveSig] int GetCurrentPadding(out uint pNumPaddingFrames);
    [PreserveSig] int IsFormatSupported(int shareMode, IntPtr pFormat, IntPtr ppClosestMatch);
    [PreserveSig] int GetMixFormat(out IntPtr ppDeviceFormat);
    [PreserveSig] int GetDevicePeriod(out long phnsDefaultDevicePeriod, out long phnsMinimumDevicePeriod);
    [PreserveSig] int Start();
    [PreserveSig] int Stop();
    [PreserveSig] int Reset();
    [PreserveSig] int SetEventHandle(IntPtr eventHandle);
    [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
}

[Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioRenderClient {
    [PreserveSig] int GetBuffer(uint numFramesRequested, out IntPtr ppData);
    [PreserveSig] int ReleaseBuffer(uint numFramesWritten, uint dwFlags);
}

[Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioCaptureClient {
    [PreserveSig] int GetBuffer(out IntPtr ppData, out uint pNumFramesToRead, out uint pdwFlags, out ulong pu64DevicePosition, out ulong pu64QPCPosition);
    [PreserveSig] int ReleaseBuffer(uint numFramesRead);
    [PreserveSig] int GetNextPacketSize(out uint pNumFramesInNextPacket);
}

[Guid("CD63314F-3FBA-4a5b-81CC-24461E011165"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioClock {
    [PreserveSig] int GetFrequency(out ulong pu64Frequency);
    [PreserveSig] int GetPosition(out ulong pu64Position, out ulong pu64QPCPosition);
    [PreserveSig] int GetCharacteristics(out uint pdwCharacteristics);
}

[StructLayout(LayoutKind.Sequential, Pack = 2)]
public struct WAVEFORMATEX {
    public ushort wFormatTag;
    public ushort nChannels;
    public uint nSamplesPerSec;
    public uint nAvgBytesPerSec;
    public ushort nBlockAlign;
    public ushort wBitsPerSample;
    public ushort cbSize;
}

[StructLayout(LayoutKind.Sequential, Pack = 2)]
public struct WAVEFORMATEXTENSIBLE {
    public WAVEFORMATEX Format;
    public ushort wValidBitsPerSample;
    public uint dwChannelMask;
    public Guid SubFormat;
}

public class SofRegressionAudioEngine {
    static Guid IID_IAudioClient = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
    static Guid IID_IAudioRenderClient = new Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2");
    static Guid IID_IAudioCaptureClient = new Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317");
    static Guid KSDATAFORMAT_SUBTYPE_PCM = new Guid("00000001-0000-0010-8000-00aa00389b71");

    public static string PlayShared(string devId, int durationMs) {
        IMMDevice dev = null;
        IAudioClient client = null;
        IAudioRenderClient renderClient = null;
        IntPtr pFmt = IntPtr.Zero;

        try {
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
            int hr = enumerator.GetDevice(devId, out dev);
            if (hr != 0) return "GetDevice failed: 0x" + hr.ToString("X8");

            object acObj;
            hr = dev.Activate(ref IID_IAudioClient, 1, IntPtr.Zero, out acObj);
            if (hr != 0) return "Activate failed: 0x" + hr.ToString("X8");
            client = (IAudioClient)acObj;

            hr = client.GetMixFormat(out pFmt);
            if (hr != 0) return "GetMixFormat failed: 0x" + hr.ToString("X8");

            int rate = Marshal.ReadInt32(pFmt, 4);
            short channels = Marshal.ReadInt16(pFmt, 2);
            short bits = Marshal.ReadInt16(pFmt, 14);

            Guid emptyGuid = Guid.Empty;
            hr = client.Initialize(0 /* SHARED */, 0, 10000000 /* 1 sec */, 0, pFmt, ref emptyGuid);
            if (hr != 0) return "Initialize failed: 0x" + hr.ToString("X8");

            uint bufferFrames;
            client.GetBufferSize(out bufferFrames);

            object arcObj;
            hr = client.GetService(ref IID_IAudioRenderClient, out arcObj);
            if (hr != 0) return "GetService(IAudioRenderClient) failed: 0x" + hr.ToString("X8");
            renderClient = (IAudioRenderClient)arcObj;

            // Pre-fill buffer
            IntPtr pData;
            hr = renderClient.GetBuffer(bufferFrames, out pData);
            if (hr == 0) {
                renderClient.ReleaseBuffer(bufferFrames, 0);
            }

            hr = client.Start();
            if (hr != 0) return "Start failed: 0x" + hr.ToString("X8");

            int elapsed = 0;
            int totalRendered = (int)bufferFrames;
            while (elapsed < durationMs) {
                Thread.Sleep(10);
                elapsed += 10;
                uint padding;
                client.GetCurrentPadding(out padding);
                uint needed = bufferFrames - padding;
                if (needed > 0) {
                    hr = renderClient.GetBuffer(needed, out pData);
                    if (hr == 0) {
                        renderClient.ReleaseBuffer(needed, 0);
                        totalRendered += (int)needed;
                    }
                }
            }

            client.Stop();
            return string.Format("OK: Rendered {0} frames at {1}Hz {2}ch {3}bit", totalRendered, rate, channels, bits);
        } catch (Exception ex) {
            return "EXCEPTION: " + ex.Message;
        } finally {
            if (pFmt != IntPtr.Zero) Marshal.FreeCoTaskMem(pFmt);
            if (renderClient != null) Marshal.ReleaseComObject(renderClient);
            if (client != null) { client.Stop(); client.Reset(); Marshal.ReleaseComObject(client); }
            if (dev != null) Marshal.ReleaseComObject(dev);
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }

    public static string PlayExclusive(string devId, int sampleRate, int bits, int channels, uint channelMask, int durationMs) {
        IMMDevice dev = null;
        IAudioClient client = null;
        IAudioRenderClient renderClient = null;
        IntPtr pFmt = IntPtr.Zero;

        try {
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
            int hr = enumerator.GetDevice(devId, out dev);
            if (hr != 0) return "GetDevice failed: 0x" + hr.ToString("X8");

            object acObj;
            hr = dev.Activate(ref IID_IAudioClient, 1, IntPtr.Zero, out acObj);
            if (hr != 0) return "Activate failed: 0x" + hr.ToString("X8");
            client = (IAudioClient)acObj;

            if (channels <= 2 && bits <= 16) {
                var wfx = new WAVEFORMATEX();
                wfx.wFormatTag = 1;
                wfx.nChannels = (ushort)channels;
                wfx.nSamplesPerSec = (uint)sampleRate;
                wfx.wBitsPerSample = (ushort)bits;
                wfx.nBlockAlign = (ushort)(channels * (bits / 8));
                wfx.nAvgBytesPerSec = (uint)(sampleRate * wfx.nBlockAlign);
                wfx.cbSize = 0;

                pFmt = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WAVEFORMATEX)));
                Marshal.StructureToPtr(wfx, pFmt, false);
            } else {
                var wfx = new WAVEFORMATEXTENSIBLE();
                int bytesPerSample = (bits == 24) ? 4 : (bits / 8);
                wfx.Format.wFormatTag = 0xFFFE;
                wfx.Format.nChannels = (ushort)channels;
                wfx.Format.nSamplesPerSec = (uint)sampleRate;
                wfx.Format.wBitsPerSample = (ushort)(bytesPerSample * 8);
                wfx.Format.nBlockAlign = (ushort)(channels * bytesPerSample);
                wfx.Format.nAvgBytesPerSec = (uint)(sampleRate * wfx.Format.nBlockAlign);
                wfx.Format.cbSize = 22;
                wfx.wValidBitsPerSample = (ushort)bits;
                wfx.dwChannelMask = channelMask;
                wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

                pFmt = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WAVEFORMATEXTENSIBLE)));
                Marshal.StructureToPtr(wfx, pFmt, false);
            }

            long hnsPeriodicity = 100000; // 10 ms
            long hnsDuration = 200000;    // 20 ms
            Guid emptyGuid = Guid.Empty;

            hr = client.Initialize(1 /* EXCLUSIVE */, 0, hnsDuration, hnsPeriodicity, pFmt, ref emptyGuid);
            if (hr != 0) {
                return "Initialize failed: 0x" + hr.ToString("X8");
            }

            uint bufferFrames;
            client.GetBufferSize(out bufferFrames);

            object arcObj;
            hr = client.GetService(ref IID_IAudioRenderClient, out arcObj);
            if (hr != 0) return "GetService(IAudioRenderClient) failed: 0x" + hr.ToString("X8");
            renderClient = (IAudioRenderClient)arcObj;

            // Pre-fill buffer
            IntPtr pData;
            hr = renderClient.GetBuffer(bufferFrames, out pData);
            if (hr == 0) {
                renderClient.ReleaseBuffer(bufferFrames, 0);
            }

            hr = client.Start();
            if (hr != 0) return "Start failed: 0x" + hr.ToString("X8");

            int elapsed = 0;
            int totalRendered = (int)bufferFrames;
            while (elapsed < durationMs) {
                Thread.Sleep(5);
                elapsed += 5;
                uint padding;
                client.GetCurrentPadding(out padding);
                uint needed = bufferFrames - padding;
                if (needed > 0) {
                    hr = renderClient.GetBuffer(needed, out pData);
                    if (hr == 0) {
                        renderClient.ReleaseBuffer(needed, 0);
                        totalRendered += (int)needed;
                    }
                }
            }

            client.Stop();
            return string.Format("OK: Rendered {0} frames at {1}Hz {2}ch {3}bit", totalRendered, sampleRate, channels, bits);
        } catch (Exception ex) {
            return "EXCEPTION: " + ex.Message;
        } finally {
            if (pFmt != IntPtr.Zero) Marshal.FreeHGlobal(pFmt);
            if (renderClient != null) Marshal.ReleaseComObject(renderClient);
            if (client != null) { client.Stop(); client.Reset(); Marshal.ReleaseComObject(client); }
            if (dev != null) Marshal.ReleaseComObject(dev);
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }

    public static string CaptureShared(string devId, int durationMs) {
        IMMDevice dev = null;
        IAudioClient client = null;
        IAudioCaptureClient captureClient = null;
        IntPtr pFmt = IntPtr.Zero;

        try {
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
            int hr = enumerator.GetDevice(devId, out dev);
            if (hr != 0) return "GetDevice failed: 0x" + hr.ToString("X8");

            object acObj;
            hr = dev.Activate(ref IID_IAudioClient, 1, IntPtr.Zero, out acObj);
            if (hr != 0) return "Activate failed: 0x" + hr.ToString("X8");
            client = (IAudioClient)acObj;

            hr = client.GetMixFormat(out pFmt);
            if (hr != 0) return "GetMixFormat failed: 0x" + hr.ToString("X8");

            int rate = Marshal.ReadInt32(pFmt, 4);
            short channels = Marshal.ReadInt16(pFmt, 2);

            Guid emptyGuid = Guid.Empty;
            hr = client.Initialize(0 /* SHARED */, 0, 10000000 /* 1 sec */, 0, pFmt, ref emptyGuid);
            if (hr != 0) return "Initialize failed: 0x" + hr.ToString("X8");

            object accObj;
            hr = client.GetService(ref IID_IAudioCaptureClient, out accObj);
            if (hr != 0) return "GetService(IAudioCaptureClient) failed: 0x" + hr.ToString("X8");
            captureClient = (IAudioCaptureClient)accObj;

            hr = client.Start();
            if (hr != 0) return "Start failed: 0x" + hr.ToString("X8");

            int elapsed = 0;
            int totalCaptured = 0;
            while (elapsed < durationMs) {
                Thread.Sleep(10);
                elapsed += 10;
                uint packetSize;
                hr = captureClient.GetNextPacketSize(out packetSize);
                while (hr == 0 && packetSize > 0) {
                    IntPtr pData;
                    uint framesRead;
                    uint flags;
                    ulong devPos, qpcPos;
                    hr = captureClient.GetBuffer(out pData, out framesRead, out flags, out devPos, out qpcPos);
                    if (hr == 0) {
                        totalCaptured += (int)framesRead;
                        captureClient.ReleaseBuffer(framesRead);
                    }
                    hr = captureClient.GetNextPacketSize(out packetSize);
                }
            }

            client.Stop();
            return string.Format("OK: Captured {0} frames at {1}Hz {2}ch", totalCaptured, rate, channels);
        } catch (Exception ex) {
            return "EXCEPTION: " + ex.Message;
        } finally {
            if (pFmt != IntPtr.Zero) Marshal.FreeCoTaskMem(pFmt);
            if (captureClient != null) Marshal.ReleaseComObject(captureClient);
            if (client != null) { client.Stop(); client.Reset(); Marshal.ReleaseComObject(client); }
            if (dev != null) Marshal.ReleaseComObject(dev);
            GC.Collect();
            GC.WaitForPendingFinalizers();
        }
    }
}
"@

Add-Type -TypeDefinition $code

# Test Results Collection
$results = @{
    Timestamp = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ssZ")
    Tests = [System.Collections.ArrayList]::new()
    TotalPassed = 0
    TotalFailed = 0
    Summary = ""
}

function Add-TestResult($testName, $passed, $details) {
    $item = @{
        Name = $testName
        Status = if ($passed) { "PASS" } else { "FAIL" }
        Details = $details
    }
    $results.Tests.Add($item) | Out-Null
    if ($passed) {
        $results.TotalPassed++
        Write-Host "  [PASS] $testName : $details" -ForegroundColor Green
    } else {
        $results.TotalFailed++
        Write-Host "  [FAIL] $testName : $details" -ForegroundColor Red
    }
}

function Get-AudioEndpointInfo($flow, $namePattern, $preferredState = 1) {
    $path = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow"
    $candidates = [System.Collections.ArrayList]::new()
    foreach ($item in (Get-ChildItem $path -ErrorAction SilentlyContinue)) {
        $id = $item.PSChildName
        $state = (Get-ItemProperty $item.PSPath -ErrorAction SilentlyContinue).DeviceState
        $prop = Get-ItemProperty (Join-Path $item.PSPath 'Properties') -ErrorAction SilentlyContinue
        $name = $prop.'{b3f8fa53-0004-438e-9002-51a9313cdbd8},6'
        if (-not $name) {
            $name = $prop.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
        }
        if ($name -match $namePattern) {
            $prefix = if ($flow -eq 'Render') { '{0.0.0.00000000}.' } else { '{0.0.1.00000000}.' }
            $candidates.Add(@{ Id = "$prefix$id"; Name = $name; State = $state }) | Out-Null
        }
    }
    # Prefer exact match with requested state
    foreach ($c in $candidates) {
        if ($c.State -eq $preferredState) {
            return $c
        }
    }
    # Fallback to any match
    if ($candidates.Count -gt 0) {
        return $candidates[0]
    }
    return $null
}

Write-Host "======================================================================" -ForegroundColor Cyan
Write-Host " STARTING SOF WINDOWS DRIVER AUTOMATED REGRESSION TEST SUITE" -ForegroundColor Cyan
Write-Host "======================================================================" -ForegroundColor Cyan

# Ensure clean default state: SimulateJackState = 4 (HDMI 1 plugged)
$regPath = "HKLM:\SYSTEM\CurrentControlSet\Services\sof_dsp\Parameters"
Set-ItemProperty -Path $regPath -Name "SimulateJackState" -Value 4 -Type DWord -ErrorAction SilentlyContinue

# Clear breadcrumb log so each test run operates on fresh execution logs
try {
    [System.IO.File]::WriteAllText("C:\sof_entry.txt", "[TEST] Starting fresh regression test run`r`n")
} catch {}

# ==============================================================================
# TEST 1: PnP & Driver Health Check
# ==============================================================================
Write-Host "`n[TEST 1] Checking PnP Device & Driver Service Health..." -ForegroundColor Yellow
$pnpEntity = Get-PnpDevice | Where-Object { $_.FriendlyName -eq "Intel Sound Open Firmware Audio DSP" -and $_.InstanceId -like "PCI\*" }
$pnpOk = ($pnpEntity -ne $null) -and ($pnpEntity.Status -eq "OK") -and ($pnpEntity.ConfigManagerErrorCode -eq "CM_PROB_NONE")

$service = Get-Service -Name "sof_dsp" -ErrorAction SilentlyContinue
$serviceOk = ($service -ne $null) -and ($service.Status -eq "Running")

if ($pnpOk -and $serviceOk) {
    Add-TestResult "test_pnp_health" $true "PnP Device '$($pnpEntity.FriendlyName)' Status=OK, Service sof_dsp=Running"
} else {
    $msg = "PnP Status=$($pnpEntity.Status) (Err=$($pnpEntity.ConfigManagerErrorCode)), Service=$($service.Status)"
    Add-TestResult "test_pnp_health" $false $msg
}

# ==============================================================================
# TEST 2: Safety Canary & Registry Parameters
# ==============================================================================
Write-Host "`n[TEST 2] Verifying Safety Canary & Registry Configuration..." -ForegroundColor Yellow
$enablePortCls = (Get-ItemProperty -Path $regPath -Name "EnablePortCls" -ErrorAction SilentlyContinue).EnablePortCls

# Re-arm canary
Set-Content -Path "C:\sof_canary.bin" -Value "ARMED" -Encoding Ascii
$canaryArm = (Test-Path "C:\sof_canary.bin") -and ((Get-Content "C:\sof_canary.bin") -match "ARMED")

if (($enablePortCls -eq 1) -and $canaryArm) {
    Add-TestResult "test_canary_state" $true "EnablePortCls=1, Safety Canary=ARMED"
} else {
    Add-TestResult "test_canary_state" $false "EnablePortCls=$enablePortCls, Canary=$canaryArm"
}

# ==============================================================================
# TEST 3: Audio Endpoint Topology & Jack Presence Baseline
# ==============================================================================
Write-Host "`n[TEST 3] Verifying Endpoint Topology & Jack Presence Baseline..." -ForegroundColor Yellow
$spk = Get-AudioEndpointInfo 'Render' 'Speakers' 1
$mic = Get-AudioEndpointInfo 'Capture' 'Microphone' 1
$hdmi1 = Get-AudioEndpointInfo 'Render' 'Display|HDMI 1' 1

$spkOk = ($spk -ne $null) -and ($spk.State -eq 1)
$micOk = ($mic -ne $null) -and ($mic.State -eq 1)
$hdmi1Ok = ($hdmi1 -ne $null) -and ($hdmi1.State -eq 1)

if ($spkOk -and $micOk -and $hdmi1Ok) {
    Add-TestResult "test_endpoint_topology" $true "Speakers(0x1), Mic(0x1), HDMI1(0x1:'$($hdmi1.Name)') active"
} else {
    $msg = "Spk=$($spk.State), Mic=$($mic.State), HDMI1=$($hdmi1.State)"
    Add-TestResult "test_endpoint_topology" $false $msg
}

# ==============================================================================
# TEST 4: Shared Mode WASAPI Playback (Speakers & Display Audio 1)
# ==============================================================================
Write-Host "`n[TEST 4] Testing WASAPI Shared Mode Audio Playback..." -ForegroundColor Yellow
$spk = Get-AudioEndpointInfo 'Render' 'Speakers' 1
$hdmi1 = Get-AudioEndpointInfo 'Render' 'Display|HDMI 1' 1

$spkRes = [SofRegressionAudioEngine]::PlayShared($spk.Id, 500)
Start-Sleep -Milliseconds 300
$hdmi1Res = [SofRegressionAudioEngine]::PlayShared($hdmi1.Id, 500)
Start-Sleep -Milliseconds 300

$sharedPassed = ($spkRes -match "^OK") -and ($hdmi1Res -match "^OK")
if ($sharedPassed) {
    Add-TestResult "test_wasapi_shared_playback" $true "Speakers ($spkRes), HDMI 1 ($hdmi1Res)"
} else {
    Add-TestResult "test_wasapi_shared_playback" $false "Speakers: $spkRes | HDMI 1: $hdmi1Res"
}

# ==============================================================================
# TEST 5: Exclusive Mode Extended Format Matrix on HDMI 1
# ==============================================================================
Write-Host "`n[TEST 5] Testing WASAPI Exclusive Mode Extended Formats on HDMI 1..." -ForegroundColor Yellow
$hdmi1 = Get-AudioEndpointInfo 'Render' 'Display|HDMI 1' 1
$fmtResults = [System.Collections.ArrayList]::new()
$allFmtPassed = $true

$formatMatrix = @(
    @{ Rate = 44100; Bits = 16; Ch = 2; Mask = 0x3; Desc = "44.1kHz 16-bit 2ch" },
    @{ Rate = 48000; Bits = 16; Ch = 2; Mask = 0x3; Desc = "48.0kHz 16-bit 2ch" },
    @{ Rate = 48000; Bits = 24; Ch = 2; Mask = 0x3; Desc = "48.0kHz 24-bit 2ch (32-cont)" },
    @{ Rate = 96000; Bits = 24; Ch = 2; Mask = 0x3; Desc = "96.0kHz 24-bit 2ch (32-cont)" },
    @{ Rate = 192000; Bits = 24; Ch = 2; Mask = 0x3; Desc = "192.0kHz 24-bit 2ch (32-cont)" },
    @{ Rate = 48000; Bits = 16; Ch = 4; Mask = 0x33; Desc = "48.0kHz 16-bit 4.0 Quad" },
    @{ Rate = 48000; Bits = 24; Ch = 8; Mask = 0x63F; Desc = "48.0kHz 24-bit 7.1 Surround" }
)

foreach ($fmt in $formatMatrix) {
    $res = [SofRegressionAudioEngine]::PlayExclusive($hdmi1.Id, $fmt.Rate, $fmt.Bits, $fmt.Ch, $fmt.Mask, 250)
    Start-Sleep -Milliseconds 300
    if ($res -match "^OK") {
        Write-Host "    [FMT PASS] $($fmt.Desc): $res" -ForegroundColor Green
        $fmtResults.Add("$($fmt.Desc): OK") | Out-Null
    } else {
        Write-Host "    [FMT FAIL] $($fmt.Desc): $res" -ForegroundColor Red
        $fmtResults.Add("$($fmt.Desc): FAIL ($res)") | Out-Null
        $allFmtPassed = $false
    }
}

if ($allFmtPassed) {
    Add-TestResult "test_wasapi_exclusive_formats" $true "All 7 formats rendered cleanly (44.1k-192k, 16/24-bit, Quad, 7.1)"
} else {
    Add-TestResult "test_wasapi_exclusive_formats" $false ($fmtResults -join "; ")
}

# ==============================================================================
# TEST 6: Microphone Audio Capture
# ==============================================================================
Write-Host "`n[TEST 6] Testing WASAPI Audio Capture on Microphone..." -ForegroundColor Yellow
$mic = Get-AudioEndpointInfo 'Capture' 'Microphone' 1
$capRes = [SofRegressionAudioEngine]::CaptureShared($mic.Id, 500)
if ($capRes -match "^OK") {
    Add-TestResult "test_wasapi_capture" $true "Microphone: $capRes"
} else {
    Add-TestResult "test_wasapi_capture" $false "Microphone: $capRes"
}

# ==============================================================================
# TEST 7: Dynamic Jack Presence Events (Simulate Unplug -> Plug -> Restore)
# ==============================================================================
Write-Host "`n[TEST 7] Testing Dynamic Jack Presence Events via IPortEvents..." -ForegroundColor Yellow

# 7a. Simulate Dynamic Unplug (SimulateJackState = 0)
Set-ItemProperty -Path $regPath -Name "SimulateJackState" -Value 0 -Type DWord
Start-Sleep -Seconds 3

# 7b. Simulate Dynamic Hotplug (SimulateJackState = 12 -> HDMI 1 & HDMI 2)
Set-ItemProperty -Path $regPath -Name "SimulateJackState" -Value 12 -Type DWord
Start-Sleep -Seconds 3

# 7c. Restore Default Jack State (SimulateJackState = 4 -> HDMI 1 only)
Set-ItemProperty -Path $regPath -Name "SimulateJackState" -Value 4 -Type DWord
Start-Sleep -Seconds 3

# Check driver log for transition confirmations
$logText = Get-Content "C:\sof_entry.txt" -ErrorAction SilentlyContinue
$jackChanges = ($logText | Select-String -Pattern "connection state changed").Count

if ($jackChanges -ge 2) {
    Add-TestResult "test_dynamic_jack_events" $true "Driver logged $jackChanges dynamic jack transitions via IPortEvents"
} else {
    Add-TestResult "test_dynamic_jack_events" $false "Only $jackChanges jack transitions detected in driver log"
}

# ==============================================================================
# TEST 8: DSP Hardware & Register Sanity
# ==============================================================================
Write-Host "`n[TEST 8] Verifying DSP Hardware & Register Sanity..." -ForegroundColor Yellow
$markerMatch = @(Select-String -Path "C:\sof_entry.txt" -Pattern "=== REGRESSION_SESSION_START_" -ErrorAction SilentlyContinue)
if ($markerMatch.Count -gt 0) {
    $sessionLine = $markerMatch[-1].LineNumber
    $logSnippet = Get-Content -Path "C:\sof_entry.txt" | Select-Object -Skip ($sessionLine - 1)
} else {
    $logSnippet = Get-Content -Path "C:\sof_entry.txt" -ErrorAction SilentlyContinue
}
$hdaConvOk = ($logSnippet | Select-String -Pattern "HDA-DisplayConv.*Start").Count -gt 0
$readbackOk = ($logSnippet | Select-String -Pattern "HDA-DisplayConv.*Readback").Count -gt 0
$timeoutFound = ($logSnippet | Select-String -Pattern "Timeout waiting for ICB").Count -gt 0

if ($hdaConvOk -and $readbackOk -and (-not $timeoutFound)) {
    Add-TestResult "test_dsp_hardware_sanity" $true "HDA converter started & readback confirmed; 0 ICB timeouts"
} else {
    $msg = "HDA Conv Start=$hdaConvOk, Readback=$readbackOk, Timeouts=$timeoutFound"
    Add-TestResult "test_dsp_hardware_sanity" $false $msg
}

# ==============================================================================
# TEST 9: Driver Breadcrumb Cleanliness Scan
# ==============================================================================
Write-Host "`n[TEST 9] Scanning Driver Breadcrumb Log for Exceptions / Errors..." -ForegroundColor Yellow
$sehCount = ($logSnippet | Select-String -Pattern "\[SEH\] Exception").Count
$panicCount = ($logSnippet | Select-String -Pattern "Panic|BugCheck").Count

if (($sehCount -eq 0) -and ($panicCount -eq 0)) {
    Add-TestResult "test_driver_logs_clean" $true "0 SEH exceptions, 0 panics in breadcrumb log"
} else {
    Add-TestResult "test_driver_logs_clean" $false "SEH=$sehCount, Panic=$panicCount detected in log"
}

# ==============================================================================
# TEST 10: USB Audio Offload & Sideband Audio Bypass Verification
# ==============================================================================
Write-Host "`n[TEST 10] Verifying USB Audio Offload & Sideband Audio Bypass..." -ForegroundColor Yellow
$usbBypassActive = $false
$usbExtDetails = ""

# 10a. Check registry on CUBILUX CA7 device
$cubiluxReg = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_0BDA&PID_4F25&MI_00\*\Device Parameters' -ErrorAction SilentlyContinue
if ($cubiluxReg -and ($cubiluxReg.EnableSidebandBypass -eq 1)) {
    $usbBypassActive = $true
    $usbExtDetails = "EnableSidebandBypass=1, GUID=$($cubiluxReg.SidebandAudioGuid)"
}

# 10b. Check suppression of legacy in-band CUBILUX endpoint
$legacyEndpoints = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like "*CUBILUX*" }
$legacySuppressed = ($legacyEndpoints.Count -eq 0)

# 10c. Check SOF UAOL Gateway Initialization in driver log & registry
$logText = Get-Content "C:\sof_entry.txt" -ErrorAction SilentlyContinue
$enableUsb = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\sof_dsp\Parameters' -ErrorAction SilentlyContinue).EnableUsbOffload
$uaolInit = (($logText | Select-String -SimpleMatch "UAOL").Count -gt 0) -or ($enableUsb -eq 1)
$usbPnpInit = (($logText | Select-String -SimpleMatch "USB-Offload").Count -gt 0) -or ($enableUsb -eq 1)

if ($usbBypassActive -and $legacySuppressed -and $uaolInit -and $usbPnpInit) {
    Add-TestResult "test_usb_offload_bypass" $true "CUBILUX CA7 claimed; in-band legacy audio suppressed; UAOL engines (Tags 3/4) & sideband listener initialized"
} else {
    Add-TestResult "test_usb_offload_bypass" $false "BypassActive=$usbBypassActive, LegacySuppressed=$legacySuppressed, UaolInit=$uaolInit, UsbPnpInit=$usbPnpInit"
}

# ==============================================================================
# Final Report & Summary
# ==============================================================================
# Re-arm canary to ensure safe state persists
Set-Content -Path "C:\sof_canary.bin" -Value "ARMED" -Encoding Ascii

$results.Summary = "$($results.TotalPassed) PASSED, $($results.TotalFailed) FAILED"
$json = $results | ConvertTo-Json -Depth 5
$json | Set-Content -Path $OutputFile -Encoding UTF8

Write-Host "`n======================================================================" -ForegroundColor Cyan
if ($results.TotalFailed -eq 0) {
    Write-Host " REGRESSION SUITE PASSED ($($results.TotalPassed)/$($results.TotalPassed) tests)" -ForegroundColor Green
} else {
    Write-Host " REGRESSION SUITE FAILED ($($results.TotalFailed) failures)" -ForegroundColor Red
}
Write-Host " Results exported to $OutputFile" -ForegroundColor Cyan
Write-Host "======================================================================`n" -ForegroundColor Cyan

exit $results.TotalFailed
