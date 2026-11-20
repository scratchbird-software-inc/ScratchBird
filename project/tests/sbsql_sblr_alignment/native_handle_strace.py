#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_HANDLE_STRACE (thin-slice S3e, plan 38; owner Q238 = 1, Q240 = 1, Q243 = 1).

The "handle" child of native_handle_test (the real StartHostedEngine, then an engine handle opened through
OpenServedEngine with the loop's facts, a session, every executor-availability entry point, the legacy registry
refusals) run under strace -f, at each of the five page sizes. Asserts: the child reports HANDLE_OK; 0 syscalls
name a ".sb." path (so no availability, prepared-statement, diagnostic-identity, begin-authority or catalog-event
side file was read or created); the directory holds only the data file, byte-identical; 0 write-class syscalls
(write/pwrite/ftruncate/fallocate/fsync/fdatasync) on any descriptor of the data file for the whole run
(the clean file's Restart writes nothing, and the handle open adds no write); and, after the SERVING-time open
completed, the only opens of the database path are the test's own (none are traced between the first
OpenServedEngine and the end of the cases: the handle reads through duplicates of the lease's descriptor).
A negative control shows the detector sees the create's writes.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, WRITE_CALLS, data_descriptors, fail, snapshot  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_handle_strace_")
    try:
        for size in SIZES:
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "handle.sdb")
            run = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if run.returncode != 0:
                fail(f"create {size}: {run.stderr[-2000:]}")
            before = snapshot(directory)
            log = os.path.join(work, f"handle-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, "handle", path]
            served = subprocess.run(command, capture_output=True, text=True, timeout=900)
            if served.returncode != 0 or not served.stdout.startswith("HANDLE_OK"):
                fail(f"handle {size}: exit {served.returncode}: {served.stdout[-500:]} {served.stderr[-2000:]}")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = handle.readlines()
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"handle {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if snapshot(directory) != before:
                fail(f"handle {size}: the directory changed")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"handle {size}: no open of the data file was traced")
            writes = []
            for index, line in enumerate(lines):
                match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
                if match and any(f == match.group(2) and start < index < end for f, start, end in spans):
                    writes.append(line.strip())
            if writes:
                fail(f"handle {size}: {len(writes)} write-class syscalls on the data file, first: {writes[0]}")
            print(f"native_handle_strace {size}: HANDLE_OK; 0 .sb. syscalls; 0 write-class syscalls on {len(spans)} "
                  f"data descriptor(s); directory unchanged; {len(lines)} syscalls traced")
        control_dir = os.path.join(work, "control")
        os.mkdir(control_dir)
        control = os.path.join(control_dir, "control.sdb")
        control_log = os.path.join(work, "control.strace")
        subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", control_log, args.test_bin, "create", control, "16384"],
                       capture_output=True, text=True, timeout=600)
        with open(control_log, encoding="utf-8", errors="replace") as handle:
            control_lines = handle.readlines()
        control_spans = data_descriptors(control_lines, control)
        control_writes = [line for index, line in enumerate(control_lines)
                          if (m := re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line))
                          and any(f == m.group(2) and start < index < end for f, start, end in control_spans)]
        if not control_writes:
            fail("the detector saw no write-class syscall during a create: it cannot be trusted")
        print(f"negative control: {len(control_writes)} write-class syscalls on the data file during create")
        print("native_handle_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
