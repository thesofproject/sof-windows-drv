#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
# Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
"""
verify_upstream_integrity.py - Pre-Commit / Build Guard for Linux Shared Files

Verifies that no files designated as shared with the upstream Linux SOF driver
have been inadvertently edited or modified without going through the upstream sync tool.

Returns 0 on success, 1 on hash mismatch or missing file.
"""

import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from sync_upstream_linux import cmd_verify

class Args:
    pass

if __name__ == "__main__":
    sys.exit(cmd_verify(Args()))
