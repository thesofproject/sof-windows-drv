#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
# Copyright (c) 2026 Intel Corporation. All rights reserved.
#
# test_s0idle_and_d3.py - Automated S0-Idle (D0i3) and D3 Power Management Test

import sys
import os
import time

sys.path.insert(0, r'C:\Users\lgird')
import aphid
import power_control

def main():
    print("=" * 78)
    print(" SOF WINDOWS DRIVER - S0-IDLE (D0i3) & D3 POWER MANAGEMENT VERIFIER")
    print(" Target DUT: Aphid (192.168.129.142)")
    print("=" * 78)

    # 1. Connectivity
    print("\n[Step 1/6] Verifying DUT Connectivity...")
    st, out, err = aphid.run_cmd("hostname", timeout=10)
    if st != 0:
        print("[ERROR] DUT unreachable:", err)
        sys.exit(1)
    print(f"  DUT Hostname: {out.strip()} (Connected)")

    # 2. Check & Configure S0-Idle Settings in Registry
    print("\n[Step 2/6] Verifying S0-Idle Registry Parameters...")
    reg_cmd = """
    $p = 'HKLM:\\SYSTEM\\CurrentControlSet\\Services\\sof_dsp\\Parameters'
    Set-ItemProperty -Path $p -Name 'EnableS0Idle' -Value 1 -Type DWord -Force
    Set-ItemProperty -Path $p -Name 'S0IdleTimeoutMs' -Value 3000 -Type DWord -Force
    $props = Get-ItemProperty -Path $p
    Write-Host "EnableS0Idle:" $props.EnableS0Idle
    Write-Host "S0IdleTimeoutMs:" $props.S0IdleTimeoutMs
    """
    with open('reg_tmp.ps1', 'w', encoding='utf-8') as f:
        f.write(reg_cmd.strip())
    aphid.upload_file('reg_tmp.ps1', r'C:\reg_tmp.ps1')
    st, out, err = aphid.run_cmd(r'powershell -ExecutionPolicy Bypass -File C:\reg_tmp.ps1')
    print(" ", out.strip().replace("\n", "\n  "))

    # 3. Test Phase 1: Wait for S0-Idle (D0 -> D3)
    print("\n[Step 3/6] Testing S0-Idle Inactivity Timeout (D0 -> D3 entry)...")
    print("  Waiting 6 seconds for 3000ms idle timeout to expire...")
    time.sleep(6.0)

    # Check breadcrumbs for D0Exit
    st, out, err = aphid.run_cmd(r'powershell -ExecutionPolicy Bypass -Command "Select-String -Path C:\sof_entry.txt -Pattern \'\[Power\].*TargetState=4|D0I3C.*ENABLED\' | Select-Object -Last 5"')
    print("  Recent D0Exit Breadcrumbs:")
    for line in out.splitlines():
        if line.strip():
            print(f"    {line.strip()}")

    if "TargetState=4" in out or "ENABLED" in out:
        print("  ✓ PASS: Device successfully entered D3 (TargetState=4 / D0i3 ENABLED) on idle!")
    else:
        print("  [INFO] Checking full recent breadcrumbs:")
        st, out_all, _ = aphid.run_cmd(r'powershell -ExecutionPolicy Bypass -Command "Get-Content C:\sof_entry.txt -Tail 20"')
        for l in out_all.splitlines():
            if l.strip():
                print(f"    {l.strip()}")

    # Measure power in D3
    p_d3 = power_control.measure_power()
    if p_d3:
        print(f"  INA228 Idle D3 Power: {p_d3['power_w']} W ({p_d3['power_mw']} mW)")

    # 4. Test Phase 2: Wake on WASAPI Playback Stream Creation (D3 -> D0)
    print("\n[Step 4/6] Testing WASAPI Audio Playback Wake (D3 -> D0)...")
    # Trigger 2-second playback on Speakers
    pb_cmd = r"""
    $ErrorActionPreference = 'Continue'
    Add-Type -Path C:\dut_regression_suite.ps1 -ErrorAction SilentlyContinue
    $engine = [SofRegressionAudioEngine]::new()
    $res = $engine.TestRenderStream('{6CFA13BD-CD13-45B3-9538-1928FB7EC41B}', 48000, 2, 32, 2000)
    Write-Host "Playback Result:" $res
    """
    with open('pb_tmp.ps1', 'w', encoding='utf-8') as f:
        f.write(pb_cmd.strip())
    aphid.upload_file('pb_tmp.ps1', r'C:\pb_tmp.ps1')
    st, out, err = aphid.run_cmd(r'powershell -ExecutionPolicy Bypass -File C:\pb_tmp.ps1', timeout=15)
    print(f"  Playback output: {out.strip()}")

    # Verify D0Entry occurred
    st, out_wake, _ = aphid.run_cmd(r'powershell -ExecutionPolicy Bypass -Command "Select-String -Path C:\sof_entry.txt -Pattern \'\[Power\].*Stream acquired|Entering D0 from state 4|D0I3C.*DISABLED\' | Select-Object -Last 5"')
    print("  Wake / D0Entry Breadcrumbs:")
    for line in out_wake.splitlines():
        if line.strip():
            print(f"    {line.strip()}")

    p_d0 = power_control.measure_power()
    if p_d0:
        print(f"  INA228 Active D0 Power: {p_d0['power_w']} W ({p_d0['power_mw']} mW)")

    # 5. Test Phase 3: Verify Re-Entry to D3 after Stream Close
    print("\n[Step 5/6] Testing S0-Idle Re-Entry (D0 -> D3 after stream release)...")
    print("  Waiting 6 seconds for stream destructor idle countdown...")
    time.sleep(6.0)

    st, out_re, _ = aphid.run_cmd(r'powershell -ExecutionPolicy Bypass -Command "Select-String -Path C:\sof_entry.txt -Pattern \'\[Power\].*released power reference|TargetState=4|D0I3C.*ENABLED\' | Select-Object -Last 5"')
    print("  Re-Entry Breadcrumbs:")
    for line in out_re.splitlines():
        if line.strip():
            print(f"    {line.strip()}")

    # 6. Test Phase 4: Full Regression Suite with S0-Idle
    print("\n[Step 6/6] Running Full 9-Endpoint Regression Suite with S0-Idle Active...")
    reg_cmd = r'powershell -ExecutionPolicy Bypass -File C:\dut_regression_suite.ps1'
    st, stdout_text, stderr_text = aphid.run_cmd(reg_cmd, timeout=90)
    print(f"  Suite exit code: {st}")

    st_json, json_text, _ = aphid.run_cmd(r'powershell -Command "[System.IO.File]::ReadAllText(\'C:\sof_regression_results.json\')"', timeout=10)
    import json
    try:
        results = json.loads(json_text.strip())
        print("\n" + "=" * 78)
        print(f" REGRESSION SUITE UNDER S0-IDLE: {results.get('TotalPassed', 0)} Passed, {results.get('TotalFailed', 0)} Failed")
        print("=" * 78)
        for t in results.get("Tests", []):
            mark = "✓ PASS" if t.get("Status") == "PASS" else "✗ FAIL"
            print(f"  {t.get('Name'):<32} {mark:<10} {t.get('Details', '')}")
    except Exception as e:
        print("  Could not parse JSON results:", e)

    print("\n" + "=" * 78)
    print(" S0-IDLE & D3 POWER MANAGEMENT TEST COMPLETE")
    print("=" * 78)

if __name__ == '__main__':
    main()
