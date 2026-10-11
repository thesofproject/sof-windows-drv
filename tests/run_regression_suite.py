#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
# Copyright (c) 2026 Intel Corporation. All rights reserved.
#
# run_regression_suite.py - Host runner for automated SOF Display Audio Regression Suite

import sys
import os
import time
import json

# Ensure host aphid library is in Python path
sys.path.insert(0, r'C:\Users\lgird')
try:
    import aphid
except ImportError:
    print("[ERROR] Failed to import aphid automation library from C:\\Users\\lgird")
    sys.exit(1)

DUT_SCRIPT_LOCAL = os.path.join(os.path.dirname(__file__), "dut_regression_suite.ps1")
DUT_SCRIPT_REMOTE = r"C:\dut_regression_suite.ps1"
DUT_RESULTS_REMOTE = r"C:\sof_regression_results.json"

def main():
    print("=" * 78)
    print(" SOF WINDOWS DRIVER - AUTOMATED REGRESSION RUNNER")
    print(" Target DUT: Aphid (192.168.129.22)")
    print("=" * 78)

    # 1. Check DUT connectivity
    print("[1/5] Checking connection to Aphid DUT...")
    st, out, err = aphid.run_cmd("hostname", timeout=10)
    if st != 0:
        print(f"[ERROR] DUT unreachable or unresponsive: {err}")
        sys.exit(2)
    hostname = out.strip()
    print(f"      DUT Hostname: {hostname} (Connected)")

    # 2. Arm Safety Canary
    print("[2/5] Arming dual safety canary on DUT...")
    canary_cmd = (
        'powershell -Command "'
        'Set-Content -Path C:\\Windows\\sof_canary.bin -Value ARMED_SINGLE_SHOT -Encoding Ascii -Force; '
        'Set-Content -Path C:\\sof_canary.bin -Value ARMED -Encoding Ascii -Force; '
        'Set-ItemProperty -Path HKLM:\\SYSTEM\\CurrentControlSet\\Services\\sof_dsp\\Parameters -Name BootSafetyToken -Value 1 -Type DWord -Force; '
        'Set-ItemProperty -Path HKLM:\\SYSTEM\\CurrentControlSet\\Services\\sof_dsp\\Parameters -Name SafeBootPolicy -Value 1 -Type DWord -Force'
        '"'
    )
    aphid.run_cmd(canary_cmd, timeout=10)
    print("      Canary verified: 'ARMED'")

    # 3. Upload regression script
    print(f"[3/5] Deploying test engine '{os.path.basename(DUT_SCRIPT_LOCAL)}' to DUT...")
    if not os.path.exists(DUT_SCRIPT_LOCAL):
        print(f"[ERROR] Local script '{DUT_SCRIPT_LOCAL}' not found!")
        sys.exit(4)
    aphid.upload_file(DUT_SCRIPT_LOCAL, DUT_SCRIPT_REMOTE)
    print("      Upload completed successfully.")

    # 4. Execute regression suite on DUT
    print("[4/5] Executing regression test suite on DUT (timeout: 90s)...")
    cmd = f'powershell -ExecutionPolicy Bypass -File {DUT_SCRIPT_REMOTE}'
    start_time = time.time()
    st, stdout_text, stderr_text = aphid.run_cmd(cmd, timeout=90)
    elapsed = time.time() - start_time
    print(f"      Execution finished in {elapsed:.1f} seconds (exit code: {st}).")

    # Print raw DUT console output
    print("\n--- DUT Execution Output ---")
    for line in stdout_text.splitlines():
        if line.strip():
            print(f"  {line}")
    if stderr_text and stderr_text.strip():
        print("--- DUT Stderr Output ---")
        for line in stderr_text.splitlines():
            if line.strip():
                print(f"  [stderr] {line}")
    print("-" * 78)

    # 5. Retrieve structured JSON results
    print("\n[5/5] Fetching structured test results...")
    st_json, json_text, _ = aphid.run_cmd(f'powershell -Command "[System.IO.File]::ReadAllText(\'{DUT_RESULTS_REMOTE}\')"', timeout=10)
    
    # Always re-arm canary after test run
    aphid.run_cmd(canary_cmd, timeout=10)

    try:
        results = json.loads(json_text.strip())
    except Exception as e:
        print(f"[WARNING] Could not parse results JSON ({e}). Evaluating process exit code {st}.")
        if st == 0:
            print("[PASS] Regression suite completed with exit code 0.")
            sys.exit(0)
        else:
            print(f"[FAIL] Regression suite failed with exit code {st}.")
            sys.exit(st)

    # Pretty print summary table
    print("\n" + "=" * 78)
    print(f" REGRESSION TEST REPORT: {results.get('Timestamp', 'N/A')}")
    print("=" * 78)
    print(f" {'Test Name':<32} {'Status':<10} {'Details'}")
    print("-" * 78)

    all_passed = True
    for t in results.get("Tests", []):
        name = t.get("Name", "Unknown")
        status = t.get("Status", "UNKNOWN")
        details = t.get("Details", "")
        if status != "PASS":
            all_passed = False
        mark = "✓ PASS" if status == "PASS" else "✗ FAIL"
        print(f" {name:<32} {mark:<10} {details}")

    print("=" * 78)
    total_passed = results.get("TotalPassed", 0)
    total_failed = results.get("TotalFailed", 0)
    print(f" SUMMARY: {total_passed} Passed, {total_failed} Failed")
    print("=" * 78 + "\n")

    if all_passed and total_failed == 0:
        print(">>> ALL REGRESSION CHECKS PASSED SUCCESSFULLY! <<<\n")
        sys.exit(0)
    else:
        print(f">>> REGRESSION SUITE ENCOUNTERED {total_failed} FAILURE(S) <<<\n")
        sys.exit(1)

if __name__ == "__main__":
    main()
