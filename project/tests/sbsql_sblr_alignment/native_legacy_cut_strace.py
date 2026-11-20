#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_LEGACY_CUT_TEST (strace half; owner 2026-11-16, plan 38 S3a).

Runs one open step of native_legacy_cut_test per process under strace and
asserts, for a native file and for a legacy file:
  - the step's refusal code (OPEN_NATIVE_RUNTIME_PENDING / OPEN_NATIVE_CRYPTO_OWNER_REQUIRED / OPEN_NOT_A_VALID_DATABASE);
  - 0 syscalls naming a ".sb." path (no lock file, no side file, no probe);
  - the directory afterwards is byte-identical (names, sizes, SHA-256): no
    file created, the presented file untouched;
  - no write-class syscall on any descriptor of the database file.
Also: sb_backup / sb_verify refuse with their codes and create nothing.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strace_attribution import data_descriptors, merge_split_lines  # noqa: E402


def snapshot(directory):
    out = {}
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, "rb") as handle:
                data = handle.read()
            out[name] = (len(data), hashlib.sha256(data).hexdigest())
        else:
            out[name] = "dir"
    return out


def fail(message):
    print("FAIL " + message)
    sys.exit(1)


def traced(binary, mode, path, log):
    command = ["strace", "-f", "-qq", "-s", "512", "-o", log, binary, mode, path]
    run = subprocess.run(command, capture_output=True, text=True, timeout=600)
    if run.returncode != 0:
        fail(f"{mode} {path}: exit {run.returncode}: {run.stderr[-2000:]}")
    with open(log, encoding="utf-8", errors="replace") as handle:
        lines = merge_split_lines(handle.readlines())
    return run.stdout.strip().splitlines()[-1] if run.stdout.strip() else "", lines


def check_step(binary, mode, path, expected_prefix, work):
    directory = os.path.dirname(path)
    before = snapshot(directory)
    log = os.path.join(work, f"{mode}-{os.path.basename(directory)}.strace")
    code, lines = traced(binary, mode, path, log)
    if not code.startswith(expected_prefix):
        fail(f"{mode} {path}: code {code!r}, expected {expected_prefix!r}")
    sb_lines = [line for line in lines if ".sb." in line]
    if sb_lines:
        fail(f"{mode} {path}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
    after = snapshot(directory)
    if after != before:
        fail(f"{mode} {path}: directory changed {before} -> {after}")
    # Pid-aware attribution (strace_attribution.py): a write counts only on a descriptor that the calling pid's own
    # table holds for the database file (opened on it or duplicated from one).
    spans = data_descriptors(lines, path)
    writes = [line for index, line in enumerate(lines)
              if re.search(r'\b(pwrite64|pwrite|write|writev|pwritev2?|ftruncate|fallocate|fsync|fdatasync|sync_file_range)\((\d+)', line)
              and spans.covers(index)]
    if writes:
        fail(f"{mode} {path}: write-class syscall on a database descriptor: {writes[0].strip()}")
    print(f"{mode} {os.path.basename(path)}: {code}; 0 .sb. syscalls; directory unchanged; "
          f"{len(lines)} syscalls traced, {len(spans)} database descriptors")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--sb-backup")
    parser.add_argument("--sb-verify")
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_legacy_cut_")
    try:
        native_dir = os.path.join(work, "native")
        legacy_dir = os.path.join(work, "legacy")
        os.mkdir(native_dir)
        os.mkdir(legacy_dir)
        native = os.path.join(native_dir, "native.sdb")
        legacy = os.path.join(legacy_dir, "legacy.sdb")
        for mode, path in (("create-native", native), ("create-legacy", legacy)):
            run = subprocess.run([args.test_bin, mode, path], capture_output=True, text=True, timeout=600)
            if run.returncode != 0:
                fail(f"{mode}: {run.stderr[-2000:]}")
        if os.listdir(native_dir) != ["native.sdb"]:
            fail(f"native create made more than its file: {os.listdir(native_dir)}")
        logs = os.path.join(work, "logs")
        os.mkdir(logs)
        for mode in ("hosted-open", "embedded-open", "abi-open", "lifecycle-open"):
            # S3b: StartHostedEngine (server and embedded entries, no crypto
            # owner in this harness) has its own code for a valid native file;
            # serving is native_serving_strace.py. The engine-handle open and
            # the lifecycle open stay pending until S3e-S3f.
            # (Was: DATABASE.OPEN_NATIVE_RUNTIME_PENDING for all four modes.)
            native_code = ("DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED" if mode in ("hosted-open", "embedded-open")
                           else "DATABASE.OPEN_NATIVE_RUNTIME_PENDING")
            check_step(args.test_bin, mode, native, native_code, logs)
            check_step(args.test_bin, mode, legacy, "DATABASE.OPEN_NOT_A_VALID_DATABASE", logs)
        tools_dir = os.path.join(work, "tools")
        os.mkdir(tools_dir)
        if args.sb_backup:
            for argv, code in ((["restore", os.path.join(tools_dir, "in.sbbak"), os.path.join(tools_dir, "out.sdb")],
                                "DATABASE.RESTORE_NOT_NATIVE"),
                               (["backup", native, os.path.join(tools_dir, "out.sbbak")],
                                "DATABASE.BACKUP_NATIVE_UNAVAILABLE"),
                               (["verify", os.path.join(tools_dir, "in.sbbak")],
                                "DATABASE.BACKUP_FORMAT_LEGACY_REFUSED")):
                run = subprocess.run([args.sb_backup] + argv, capture_output=True, text=True, timeout=60)
                if run.returncode == 0 or code not in run.stderr:
                    fail(f"sb_backup {argv[0]}: rc={run.returncode} stderr={run.stderr!r}")
            print("sb_backup: restore/backup/verify refused")
        if args.sb_verify:
            run = subprocess.run([args.sb_verify, native, "-o", os.path.join(tools_dir, "report.txt")],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode == 0 or "DATABASE.VERIFY_NATIVE_UNAVAILABLE" not in run.stderr:
                fail(f"sb_verify: rc={run.returncode} stderr={run.stderr!r}")
            print("sb_verify: refused")
        if os.listdir(tools_dir):
            fail(f"refused tools created files: {os.listdir(tools_dir)}")
        print("native_legacy_cut_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
