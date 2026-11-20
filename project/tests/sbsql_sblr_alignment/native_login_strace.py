#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_LOGIN_STRACE (S3f0, owner Q301 = yes, 2026-11-19).

The login refusal on a native database (native_login_refusal_test, modes "served" and "path") run under strace -f
at each of the five page sizes. Asserts: the child reports LOGIN_OK (the specific code
SECURITY.AUTHENTICATION.NATIVE_LOGIN_UNAVAILABLE came back through the real login handoff and EngineAuthenticate);
0 syscalls name a ".sb." path (no legacy security / lifecycle side file was read or created); the directory holds
only the data file, byte-identical (nothing created); 0 write-class syscalls on any descriptor of the data file.
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


def writes_on_data(lines, path):
    spans = data_descriptors(lines, path)
    out = []
    for index, line in enumerate(lines):
        match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
        if match and any(f == match.group(2) and start < index < end for f, start, end in spans):
            out.append(line.strip())
    return spans, out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_login_strace_")
    try:
        for size in SIZES:
            for mode in ("served", "path"):
                directory = os.path.join(work, f"{mode}{size}")
                os.mkdir(directory)
                path = os.path.join(directory, "login.sdb")
                run = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
                if run.returncode != 0:
                    fail(f"create {size}: {run.stderr[-2000:]}")
                before = snapshot(directory)
                log = os.path.join(work, f"login-{mode}-{size}.strace")
                command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, mode, path]
                ran = subprocess.run(command, capture_output=True, text=True, timeout=900)
                if ran.returncode != 0 or not ran.stdout.startswith("LOGIN_OK"):
                    fail(f"{mode} {size}: exit {ran.returncode}: {ran.stdout[-500:]} {ran.stderr[-2000:]}")
                with open(log, encoding="utf-8", errors="replace") as handle:
                    lines = handle.readlines()
                sb_lines = [line for line in lines if ".sb." in line]
                if sb_lines:
                    fail(f"{mode} {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
                if snapshot(directory) != before:
                    fail(f"{mode} {size}: the directory changed (a file was created or written)")
                spans, writes = writes_on_data(lines, path)
                if not spans:
                    fail(f"{mode} {size}: no open of the data file was traced")
                if writes:
                    fail(f"{mode} {size}: {len(writes)} write-class syscalls on the data file, first: {writes[0]}")
                print(f"native_login_strace {mode} {size}: LOGIN_OK; 0 .sb. syscalls; 0 write-class syscalls on "
                      f"{len(spans)} data descriptor(s); directory unchanged; {len(lines)} syscalls traced")
        control_dir = os.path.join(work, "control")
        os.mkdir(control_dir)
        control = os.path.join(control_dir, "control.sdb")
        control_log = os.path.join(work, "control.strace")
        subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", control_log, args.test_bin, "create", control, "16384"],
                       capture_output=True, text=True, timeout=600)
        with open(control_log, encoding="utf-8", errors="replace") as handle:
            control_lines = handle.readlines()
        _, control_writes = writes_on_data(control_lines, control)
        if not control_writes:
            fail("the detector saw no write-class syscall during a create: it cannot be trusted")
        print(f"negative control: {len(control_writes)} write-class syscalls on the data file during create")
        print("native_login_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
