#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 The SOF Authors
# Copyright (c) 2026 Intel Corporation / Sound Open Firmware Project.
"""
sync_upstream_linux.py - Upstream Linux SOF Tracking, Diffing, and Update Tool

Manages files shared between the Sound Open Firmware (SOF) Linux driver
(sound/soc/sof/ and include/sound/sof/) and the Windows driver codebase.

Default Upstream: https://github.com/thesofproject/linux (branch: topic/sof-dev)
Local Fallback:   //192.168.122.1/work/linux (moth mirror)

Usage:
    python scripts/sync_upstream_linux.py status
    python scripts/sync_upstream_linux.py check-updates [--limit N]
    python scripts/sync_upstream_linux.py diff [--file <path>] [--ref <git-ref>]
    python scripts/sync_upstream_linux.py update [--ref <git-ref>] [--dry-run] [--skip-build]
    python scripts/sync_upstream_linux.py verify
"""

import argparse
import difflib
import hashlib
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime

# Script location and repository root
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
MANIFEST_PATH = os.path.join(REPO_ROOT, "UPSTREAM_LINUX.json")
MSBUILD_PATH = r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

USER_AGENT = "SOF-Windows-Driver-Sync/1.0"


def load_manifest():
    """Load and return the upstream tracking manifest."""
    if not os.path.exists(MANIFEST_PATH):
        print(f"Error: Manifest not found at {MANIFEST_PATH}", file=sys.stderr)
        sys.exit(1)
    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


def save_manifest(manifest):
    """Save updated manifest to disk."""
    with open(MANIFEST_PATH, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


def compute_sha256(filepath, normalize_crlf=True):
    """Compute SHA-256 hash of a file's content in binary mode.
    
    If normalize_crlf is True, line endings are normalized to CRLF (the Windows manifest standard)
    so verification remains invariant across Linux and Windows checkouts.
    """
    if not os.path.exists(filepath):
        return None
    with open(filepath, "rb") as f:
        data = f.read()
    if normalize_crlf:
        data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    return hashlib.sha256(data).hexdigest()



def normalize_newlines_to_crlf(data: bytes) -> bytes:
    """Normalize line endings to Windows CRLF."""
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def normalize_newlines_to_lf(data: bytes) -> bytes:
    """Normalize line endings to UNIX LF for content comparison."""
    return data.replace(b"\r\n", b"\n")


def get_auth_headers():
    """Get HTTP headers including optional GitHub auth token."""
    headers = {"User-Agent": USER_AGENT}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = f"token {token}"
    return headers


def fetch_url(url, as_json=False):
    """Fetch content from a URL with error handling."""
    req = urllib.request.Request(url, headers=get_auth_headers())
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = resp.read()
            if as_json:
                return json.loads(data.decode("utf-8"))
            return data
    except urllib.error.HTTPError as e:
        if e.code == 429:
            print(f"Warning: GitHub rate limit reached (HTTP 429).", file=sys.stderr)
        elif e.code == 403 and "rate limit" in str(e.reason).lower():
            print(f"Warning: GitHub API rate limit exceeded (HTTP 403). Set GITHUB_TOKEN to increase limits.", file=sys.stderr)
        else:
            print(f"HTTP Error {e.code} fetching {url}: {e.reason}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"Error fetching {url}: {e}", file=sys.stderr)
        return None


def fetch_file_content(linux_path, ref="topic/sof-dev", source_path=None):
    """
    Fetch file content either from local git mirror or GitHub raw endpoint.
    Returns bytes or None.
    """
    # 1. Try local mirror if provided or reachable
    if source_path and os.path.exists(source_path):
        # Use git show if it's a git repo
        git_dir = os.path.join(source_path, ".git")
        if os.path.exists(git_dir) or os.path.exists(source_path + "/.git"):
            try:
                cmd = ["git", f"--git-dir={git_dir}", f"--work-tree={source_path}", "show", f"{ref}:{linux_path}"]
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                return res.stdout
            except Exception:
                pass
        # Fallback to direct file read if worktree
        direct_path = os.path.join(source_path, linux_path.replace("/", os.sep))
        if os.path.exists(direct_path):
            with open(direct_path, "rb") as f:
                return f.read()

    # 2. Fetch from GitHub raw content
    raw_url = f"https://raw.githubusercontent.com/thesofproject/linux/{ref}/{linux_path}"
    data = fetch_url(raw_url, as_json=False)
    return data


def query_github_latest_commit(repo="thesofproject/linux", branch="topic/sof-dev"):
    """Query GitHub API for latest commit on a branch."""
    api_url = f"https://api.github.com/repos/{repo}/commits/{branch}"
    data = fetch_url(api_url, as_json=True)
    if data and "sha" in data:
        return {
            "sha": data["sha"],
            "date": data["commit"]["author"]["date"],
            "author": f"{data['commit']['author']['name']} <{data['commit']['author']['email']}>",
            "subject": data["commit"]["message"].splitlines()[0]
        }
    return None


def query_local_latest_commit(mirror_path, ref="sof/topic/sof-dev"):
    """Query local git repository for latest commit."""
    git_dir = os.path.join(mirror_path, ".git")
    if not os.path.exists(git_dir):
        return None
    try:
        cmd = ["git", f"--git-dir={git_dir}", "log", "-n", "1", "--format=%H%x00%aI%x00%an <%ae>%x00%s", ref]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
        parts = res.stdout.strip().split("\x00")
        if len(parts) >= 4:
            return {
                "sha": parts[0],
                "date": parts[1],
                "author": parts[2],
                "subject": parts[3]
            }
    except Exception:
        pass
    return None


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_verify(args):
    """Verify integrity of all local tracked Linux files against manifest."""
    manifest = load_manifest()
    mismatches = []
    missing = []
    total = len(manifest["mappings"])

    print(f"Verifying {total} tracked Linux files against UPSTREAM_LINUX.json...")

    for m in manifest["mappings"]:
        win_path = m["windows"]
        full_path = os.path.join(REPO_ROOT, win_path.replace("/", os.sep))
        expected_hash = manifest["file_hashes"].get(win_path)

        if not os.path.exists(full_path):
            missing.append(win_path)
            continue

        actual_hash = compute_sha256(full_path)
        if actual_hash != expected_hash:
            mismatches.append((win_path, expected_hash, actual_hash))

    if missing:
        print(f"\n[FAIL] {len(missing)} files missing:", file=sys.stderr)
        for f in missing:
            print(f"  - {f}", file=sys.stderr)

    if mismatches:
        print(f"\n[FAIL] {len(mismatches)} files have been modified locally without manifest update:", file=sys.stderr)
        for f, exp, act in mismatches:
            print(f"  - {f}\n      Expected: {exp}\n      Actual:   {act}", file=sys.stderr)
        print("\nNote: Files shared with Linux must not be modified directly. Use include/compat/ or include/sof/sof_osal.h.", file=sys.stderr)
        return 1

    if not missing and not mismatches:
        print(f"[PASS] All {total} tracked Linux files match their expected SHA-256 hashes.")
        return 0
    return 1


def cmd_status(args):
    """Show detailed synchronization status against manifest and upstream."""
    manifest = load_manifest()
    upstream = manifest["upstream"]

    print("=" * 80)
    print("UPSTREAM LINUX SOF SYNCHRONIZATION STATUS")
    print("=" * 80)
    print(f"Tracked Repository: {upstream['repository_url']}")
    print(f"Default Branch:     {upstream['default_branch']}")
    print(f"Tracked Commit:     {upstream['tracked_commit'][:12]} ({upstream.get('tracked_commit_date', 'N/A')})")
    print(f"Commit Subject:     {upstream.get('tracked_commit_subject', 'N/A')}")
    print(f"Last Sync Time:     {upstream.get('last_sync', 'N/A')}")
    print(f"Local Mirror:       {upstream.get('local_mirror_path', 'N/A')}")
    print("-" * 80)

    # 1. Local file integrity check
    local_ok = 0
    local_modified = 0
    local_missing = 0
    for m in manifest["mappings"]:
        win_path = m["windows"]
        full_path = os.path.join(REPO_ROOT, win_path.replace("/", os.sep))
        if not os.path.exists(full_path):
            local_missing += 1
        elif compute_sha256(full_path) != manifest["file_hashes"].get(win_path):
            local_modified += 1
        else:
            local_ok += 1

    print(f"Local File State ({len(manifest['mappings'])} files):")
    print(f"  Synced / Untouched: {local_ok}")
    if local_modified > 0:
        print(f"  Locally Modified:   {local_modified} (WARNING: Shared files modified)")
    if local_missing > 0:
        print(f"  Missing:            {local_missing}")

    # 2. Check upstream HEAD
    print("-" * 80)
    print("Querying upstream HEAD...")
    latest = None
    # Try local mirror first for instant response
    local_mirror = upstream.get("local_mirror_path")
    if local_mirror and os.path.exists(local_mirror):
        latest = query_local_latest_commit(local_mirror, f"sof/{upstream['default_branch']}")
        if latest:
            print(f"  [via local mirror: {local_mirror}]")

    # If no local mirror or failed, query GitHub API
    if not latest:
        latest = query_github_latest_commit("thesofproject/linux", upstream["default_branch"])
        if latest:
            print("  [via GitHub API]")

    if latest:
        print(f"Upstream Latest Commit: {latest['sha'][:12]} ({latest['date']})")
        print(f"Author:                 {latest['author']}")
        print(f"Subject:                {latest['subject']}")
        if latest["sha"] == upstream["tracked_commit"]:
            print("\n>> Windows driver is UP TO DATE with upstream HEAD.")
        else:
            print(f"\n>> Updates available! Tracked commit is {upstream['tracked_commit'][:12]}, upstream HEAD is {latest['sha'][:12]}.")
            print("   Run 'python scripts/sync_upstream_linux.py check-updates' to view commits.")
            print("   Run 'python scripts/sync_upstream_linux.py update' to apply updates.")
    else:
        print("Could not retrieve upstream HEAD commit (offline or rate-limited).")
    print("=" * 80)
    return 0


def cmd_check_updates(args):
    """Query and display upstream commits affecting tracked files."""
    manifest = load_manifest()
    upstream = manifest["upstream"]
    limit = args.limit or 10

    print(f"Checking incoming commits on {upstream['default_branch']} since {upstream['tracked_commit'][:12]}...")

    # Try local mirror first
    local_mirror = upstream.get("local_mirror_path")
    if local_mirror and os.path.exists(local_mirror):
        git_dir = os.path.join(local_mirror, ".git")
        try:
            cmd = [
                "git", f"--git-dir={git_dir}", "log",
                f"{upstream['tracked_commit']}..sof/{upstream['default_branch']}",
                f"-n{limit}", "--oneline",
                "--", "sound/soc/sof", "include/sound/sof"
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
            lines = res.stdout.strip().splitlines()
            if lines:
                print(f"\nFound {len(lines)} incoming commit(s) touching SOF files (via local mirror):")
                for line in lines:
                    print(f"  {line}")
                return 0
            else:
                print("\nNo new commits affecting tracked SOF directories.")
                return 0
        except Exception:
            pass

    # GitHub API fallback
    api_url = f"https://api.github.com/repos/thesofproject/linux/commits?sha={upstream['default_branch']}&path=sound/soc/sof&per_page={limit}"
    commits = fetch_url(api_url, as_json=True)
    if commits:
        print(f"\nRecent upstream commits touching sound/soc/sof on GitHub:")
        for c in commits:
            sha = c["sha"][:12]
            date = c["commit"]["author"]["date"][:10]
            subj = c["commit"]["message"].splitlines()[0]
            marker = " [TRACKED]" if c["sha"] == upstream["tracked_commit"] else ""
            print(f"  {sha} ({date}): {subj}{marker}")
    else:
        print("Unable to fetch commit history from GitHub (rate limit or network issue).")
    return 0


def cmd_diff(args):
    """Compute unified diff between local files and upstream ref."""
    manifest = load_manifest()
    upstream = manifest["upstream"]
    ref = args.ref or upstream["default_branch"]
    source = args.source or upstream.get("local_mirror_path")

    target_files = manifest["mappings"]
    if args.file:
        target_files = [m for m in manifest["mappings"] if m["windows"] == args.file or m["linux"] == args.file]
        if not target_files:
            print(f"Error: Specified file '{args.file}' is not in tracked mappings.", file=sys.stderr)
            return 1

    print(f"Comparing local files against upstream '{ref}' (normalizing newlines)...")
    total_diffs = 0

    for m in target_files:
        win_path = m["windows"]
        linux_path = m["linux"]
        full_win_path = os.path.join(REPO_ROOT, win_path.replace("/", os.sep))

        if not os.path.exists(full_win_path):
            print(f"\nLocal file missing: {win_path}")
            continue

        with open(full_win_path, "rb") as f:
            local_bytes = f.read()

        upstream_bytes = fetch_file_content(linux_path, ref=ref, source_path=source)
        if upstream_bytes is None:
            print(f"Warning: Could not fetch upstream version of {linux_path}", file=sys.stderr)
            continue

        # Normalize line endings to LF for unified diff comparison
        local_lines = normalize_newlines_to_lf(local_bytes).decode("utf-8", errors="replace").splitlines(keepends=True)
        upstream_lines = normalize_newlines_to_lf(upstream_bytes).decode("utf-8", errors="replace").splitlines(keepends=True)

        diff = list(difflib.unified_diff(
            local_lines, upstream_lines,
            fromfile=f"local/{win_path}",
            tofile=f"upstream/{ref}/{linux_path}"
        ))

        if diff:
            total_diffs += 1
            print("".join(diff))

    if total_diffs == 0:
        print(f"No code differences found across {len(target_files)} tracked file(s) against upstream '{ref}'.")
    else:
        print(f"\nFound code differences in {total_diffs} file(s).")
    return 0


def cmd_update(args):
    """Fetch updated files, update manifest, and test MSBuild."""
    manifest = load_manifest()
    upstream = manifest["upstream"]
    ref = args.ref or upstream["default_branch"]
    source = args.source or upstream.get("local_mirror_path")
    dry_run = args.dry_run
    skip_build = args.skip_build

    print("=" * 80)
    print(f"UPDATING LINUX DRIVER FILES FROM UPSTREAM: {ref}")
    if dry_run:
        print("MODE: DRY RUN (No changes will be written)")
    print("=" * 80)

    # 1. Identify upstream commit metadata
    commit_info = None
    if source and os.path.exists(source):
        commit_info = query_local_latest_commit(source, f"sof/{ref}" if not ref.startswith("sof/") else ref)
    if not commit_info:
        commit_info = query_github_latest_commit("thesofproject/linux", ref)

    if commit_info:
        print(f"Target Commit:  {commit_info['sha']}")
        print(f"Author:         {commit_info['author']}")
        print(f"Date:           {commit_info['date']}")
        print(f"Subject:        {commit_info['subject']}")
    else:
        print(f"Target Ref:     {ref} (commit details could not be retrieved)")
    print("-" * 80)

    updated_count = 0
    unchanged_count = 0
    failed_count = 0
    new_hashes = {}

    for m in manifest["mappings"]:
        win_path = m["windows"]
        linux_path = m["linux"]
        full_win_path = os.path.join(REPO_ROOT, win_path.replace("/", os.sep))

        upstream_bytes = fetch_file_content(linux_path, ref=ref, source_path=source)
        if upstream_bytes is None:
            print(f"  [ERROR] Failed to fetch: {linux_path}")
            failed_count += 1
            continue

        # Normalize line endings to Windows CRLF for local consistency
        crlf_bytes = normalize_newlines_to_crlf(upstream_bytes)

        if os.path.exists(full_win_path):
            with open(full_win_path, "rb") as f:
                local_bytes = f.read()
            is_same = (normalize_newlines_to_lf(local_bytes) == normalize_newlines_to_lf(upstream_bytes))
        else:
            is_same = False

        if is_same:
            unchanged_count += 1
            new_hashes[win_path] = compute_sha256(full_win_path)
        else:
            updated_count += 1
            print(f"  [UPDATE] {win_path} <- {linux_path}")
            if not dry_run:
                os.makedirs(os.path.dirname(full_win_path), exist_ok=True)
                with open(full_win_path, "wb") as f:
                    f.write(crlf_bytes)
                new_hashes[win_path] = hashlib.sha256(crlf_bytes).hexdigest()
            else:
                new_hashes[win_path] = hashlib.sha256(crlf_bytes).hexdigest()

    print("-" * 80)
    print(f"Results: {updated_count} file(s) updated, {unchanged_count} unchanged, {failed_count} failed.")

    if failed_count > 0:
        print("[FAIL] Some files could not be fetched. Aborting manifest update.", file=sys.stderr)
        return 1

    if dry_run:
        print("\nDry run complete. No files or manifest were altered.")
        return 0

    if updated_count == 0:
        print("\nAll files are already identical to target ref. Updating sync timestamp.")
        manifest["upstream"]["last_sync"] = datetime.utcnow().isoformat() + "Z"
        save_manifest(manifest)
        return 0

    # Update manifest
    manifest["upstream"]["tracked_commit"] = commit_info["sha"] if commit_info else ref
    if commit_info:
        manifest["upstream"]["tracked_commit_date"] = commit_info["date"]
        manifest["upstream"]["tracked_commit_author"] = commit_info["author"]
        manifest["upstream"]["tracked_commit_subject"] = commit_info["subject"]
    manifest["upstream"]["last_sync"] = datetime.utcnow().isoformat() + "Z"
    manifest["file_hashes"] = new_hashes
    save_manifest(manifest)
    print(f"Updated UPSTREAM_LINUX.json manifest.")

    # 2. Automated MSBuild verification
    if not skip_build:
        print("\nRunning MSBuild verification to test OSAL compatibility...")
        if not os.path.exists(MSBUILD_PATH):
            print(f"Warning: MSBuild not found at {MSBUILD_PATH}. Skipping build test.", file=sys.stderr)
            return 0

        cmd = [
            MSBUILD_PATH,
            os.path.join(REPO_ROOT, "sof_windows_driver.sln"),
            "/p:Configuration=Release",
            "/p:Platform=x64",
            "/t:Build"
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode == 0:
            print("[PASS] Solution compiled cleanly (Release|x64) with 0 errors.")
        else:
            print("[WARNING] Build failed after sync! Upstream may have added new kernel APIs not yet supported in OSAL.", file=sys.stderr)
            print("Compiler Errors:", file=sys.stderr)
            for line in res.stdout.splitlines():
                if "error C" in line or "error MSB" in line:
                    print(f"  {line}", file=sys.stderr)
            print("\nPlease update include/sof/sof_osal.h or include/compat/ to bridge any missing functions.", file=sys.stderr)
            return 2

    print("\nSynchronization completed successfully!")
    return 0


# ---------------------------------------------------------------------------
# Main Entry Point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Upstream Linux SOF Tracking, Diffing, and Update Tool"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # status
    p_status = subparsers.add_parser("status", help="Show current sync status and upstream HEAD")

    # check-updates
    p_check = subparsers.add_parser("check-updates", help="List incoming upstream commits touching tracked files")
    p_check.add_argument("--limit", type=int, default=10, help="Number of commits to display (default: 10)")

    # diff
    p_diff = subparsers.add_parser("diff", help="Compute diff against upstream ref")
    p_diff.add_argument("--file", help="Specific file to diff (Windows or Linux path)")
    p_diff.add_argument("--ref", help="Upstream git ref (default: topic/sof-dev)")
    p_diff.add_argument("--source", help="Path to local mirror worktree")

    # update
    p_update = subparsers.add_parser("update", help="Update files from upstream, update manifest, and test build")
    p_update.add_argument("--ref", help="Upstream git ref or commit SHA (default: topic/sof-dev)")
    p_update.add_argument("--source", help="Path to local mirror worktree")
    p_update.add_argument("--dry-run", action="store_true", help="Preview changes without modifying files")
    p_update.add_argument("--skip-build", action="store_true", help="Skip MSBuild compilation check")

    # verify
    p_verify = subparsers.add_parser("verify", help="Verify that all local tracked files match manifest hashes")

    args = parser.parse_args()

    dispatch = {
        "status": cmd_status,
        "check-updates": cmd_check_updates,
        "diff": cmd_diff,
        "update": cmd_update,
        "verify": cmd_verify,
    }

    ret = dispatch[args.command](args)
    sys.exit(ret or 0)


if __name__ == "__main__":
    main()
