#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_NAME_STREAM_STRACE (streaming reader, owner 2026-11-25).

The streaming reader's child (native_name_stream_test, mode "streamread") run under strace -f over a production-created
database whose names relation the production writers grew past the old 4 MiB / 4096-row scan tier, for the names (role 2),
security (role 4) and configuration (role 5) relations. Asserts, with the shared pid-aware helpers
(strace_attribution.py through native_serving_strace.py):
  - the child reports STREAMREAD for every relation (no error) and the names are more than the old leaf-byte cap;
  - 0 syscalls name a ".sb." path (no lock file, no side file);
  - the directory holds only the data file, byte-identical afterwards;
  - 0 write-class syscalls on any descriptor opened on the data file or duplicated from one, for the whole run.
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

FORGED_ROWS = 2000  # on top of the created names: past the old 4 MiB leaf-image cap at every page size


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
    work = tempfile.mkdtemp(prefix="sb_native_name_stream_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            directory = os.path.join(work, f"stream{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "names.sdb")
            pages = max(512, (64 << 20) // size)
            created = subprocess.run([args.test_bin, "bigcreate", path, str(size), str(pages)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
            forged = subprocess.run([args.test_bin, "bigforge", path, field(created.stdout, "bootstrap"), str(FORGED_ROWS), "0", "250", "0"],
                                    capture_output=True, text=True, timeout=1500)
            if forged.returncode != 0 or f"FORGED rows={FORGED_ROWS}" not in forged.stdout:
                fail(f"forge {size}: {forged.stdout[-500:]} {forged.stderr[-2000:]}")
            before = snapshot(directory)
            for role in (2, 4, 5):
                log = os.path.join(work, f"stream-{role}-{size}.strace")
                command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, "streamread", path, str(role), "4", "1024", "noretain"]
                ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
                if ran.returncode != 0 or f"STREAMREAD role={role} " not in ran.stdout:
                    fail(f"role {role} {size}: exit {ran.returncode}: {ran.stdout[-800:]} {ran.stderr[-2000:]}")
                if role == 2:
                    leaf_bytes = int(re.search(r'\bleaf_reads=(\d+)', ran.stdout).group(1)) * size
                    if leaf_bytes <= 4 * 1024 * 1024:
                        fail(f"{size}: the names relation's base leaves are {leaf_bytes} bytes, not past the old 4 MiB cap")
                with open(log, encoding="utf-8", errors="replace") as handle:
                    lines = merge_split_lines(handle.readlines())
                sb_lines = [line for line in lines if ".sb." in line]
                if sb_lines:
                    fail(f"role {role} {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
                if snapshot(directory) != before:
                    fail(f"role {role} {size}: the directory changed (a file was created or written)")
                spans, writes = writes_on_data(lines, path)
                if not spans:
                    fail(f"role {role} {size}: no open of the data file was traced")
                if writes:
                    fail(f"role {role} {size}: {len(writes)} write-class syscalls on the data file, first: {writes[0]}")
                print(f"native_name_stream_strace role {role} {size}: STREAMREAD; 0 .sb. syscalls; 0 write-class syscalls on "
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
        print("native_name_stream_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
