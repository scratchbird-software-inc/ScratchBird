#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_OPEN_BIG_STRACE (2026-11-26).

The served child of native_open_big_test (mode "serve": the real StartHostedEngine, then the offline-equal ListNames, resolver and
login checks) run under strace -f at one page size over a production-created database past the old open tiers: its names
relation grown by the production writers beyond the old 4096-row scan tier ("names"), and its security relation grown beyond the
old 1024-row credential tier by committed successor versions written by the production writer ("principals"). Asserts, with the
shared pid-aware helpers (strace_attribution.py through native_serving_strace.py):
  - the child reports OPENBIG (the open succeeded and every served check passed);
  - 0 syscalls name a ".sb." path (no lock file, no side file);
  - the directory holds only the data file, byte-identical afterwards;
  - 0 write-class syscalls on any descriptor opened on the data file or duplicated from one, for the whole run: the open
    (including its sized scan, capture and credential probe), the served reads and the close write nothing.
A negative control shows the write detector sees the create's writes.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, WRITE_CALLS, data_descriptors, fail, merge_split_lines, snapshot  # noqa: E402

FORGED_NAMES = 1500


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def writes_on_data(lines, path):
    spans = data_descriptors(lines, path)
    out = []
    for index, line in enumerate(lines):
        match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
        if match and spans.covers(index):
            out.append(line.strip())
    return spans, out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--size", type=int, choices=SIZES)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_open_big_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            for state in ("names", "principals"):
                directory = os.path.join(work, f"{state}{size}")
                os.mkdir(directory)
                path = os.path.join(directory, "open.sdb")
                pages = max(512, (128 << 20) // size)
                created = subprocess.run([args.test_bin, "bigcreate", path, str(size), str(pages)], capture_output=True, text=True, timeout=600)
                if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                    fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
                boot, service, database = field(created.stdout, "bootstrap"), field(created.stdout, "service"), field(created.stdout, "database")
                if state == "names":
                    forged = subprocess.run([args.test_bin, "bigforge", path, boot, str(FORGED_NAMES), "0", "250", "0"],
                                            capture_output=True, text=True, timeout=1500)
                    if forged.returncode != 0 or f"FORGED rows={FORGED_NAMES}" not in forged.stdout:
                        fail(f"forge names {size}: {forged.stdout[-500:]} {forged.stderr[-2000:]}")
                else:
                    forged = subprocess.run([args.test_bin, "secforge", path, boot, "400"], capture_output=True, text=True, timeout=900)
                    if forged.returncode != 0 or "SECFORGED versions=400" not in forged.stdout:
                        fail(f"forge principals {size}: {forged.stdout[-500:]} {forged.stderr[-2000:]}")
                before = snapshot(directory)
                log = os.path.join(work, f"open-{state}-{size}.strace")
                command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, "serve", path, "serving", database, boot, service]
                ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
                if ran.returncode != 0 or "OPENBIG " not in ran.stdout:
                    fail(f"{state} {size}: exit {ran.returncode}: {ran.stdout[-800:]} {ran.stderr[-2000:]}")
                with open(log, encoding="utf-8", errors="replace") as handle:
                    lines = merge_split_lines(handle.readlines())
                sb_lines = [line for line in lines if ".sb." in line]
                if sb_lines:
                    fail(f"{state} {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
                if snapshot(directory) != before:
                    fail(f"{state} {size}: the directory changed (a file was created or written)")
                spans, writes = writes_on_data(lines, path)
                if not spans:
                    fail(f"{state} {size}: no open of the data file was traced")
                if writes:
                    fail(f"{state} {size}: {len(writes)} write-class syscalls on the data file, first: {writes[0]}")
                print(f"native_open_big_strace {state} {size}: OPENBIG; 0 .sb. syscalls; 0 write-class syscalls on "
                      f"{len(spans)} data descriptor(s); directory unchanged; {len(lines)} syscalls traced")
                shutil.rmtree(directory, ignore_errors=True)
        control_dir = os.path.join(work, "control")
        os.mkdir(control_dir)
        control = os.path.join(control_dir, "control.sdb")
        control_log = os.path.join(work, "control.strace")
        subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", control_log, args.test_bin, "bigcreate", control, "16384", "512"],
                       capture_output=True, text=True, timeout=600)
        with open(control_log, encoding="utf-8", errors="replace") as handle:
            control_lines = merge_split_lines(handle.readlines())
        _, control_writes = writes_on_data(control_lines, control)
        if not control_writes:
            fail("the detector saw no write-class syscall during a create: it cannot be trusted")
        print(f"negative control: {len(control_writes)} write-class syscalls on the data file during create")
        print("native_open_big_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
