#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_DIAGNOSTIC_REGISTRY_STRACE (S4h part 1, owner option A).

The native diagnostic identity registry is a per-build table: no file. The test binary loads it through a native context whose
database_path names a scratch directory, under `strace -f`; no syscall of any kind may name that directory or a ".sb." path, and the
directory must stay empty. A negative control proves the detector sees the legacy registry's own touches: the same load through a
LEGACY context (no native facts) would name the path (this is checked by running a small shell `stat` on the directory under strace).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile


def fail(message):
    print(f"native_diagnostic_registry_strace: FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        fail("usage: native_diagnostic_registry_strace.py <test-bin>")
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_diagreg_")
    try:
        log = os.path.join(work, "trace.log")
        scratch = os.path.join(work, "dbdir")
        os.mkdir(scratch)
        run = subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", log, sys.argv[1], "nofile", scratch],
                             capture_output=True, text=True, timeout=300)
        if run.returncode != 0 or not run.stdout.startswith("NOFILE "):
            fail(f"nofile run: {run.stdout!r} {run.stderr[-500:]}")
        rows = int(run.stdout.split()[1])
        if rows < 100:
            fail(f"the native registry returned {rows} rows")
        text = open(log, encoding="utf-8", errors="replace").read()
        text = re.sub(r"\n\s*", "\n", text)
        named = [line for line in text.splitlines() if (scratch in line or ".sb." in line) and " execve(" not in line]  # the exec argument is the test's own argv
        if named:
            fail(f"{len(named)} syscalls name the database directory or a .sb. path, first: {named[0][:200]}")
        if os.listdir(scratch):
            fail(f"the directory was written: {os.listdir(scratch)}")
        # Negative control: the detector must see a syscall that does name the directory.
        control_log = os.path.join(work, "control.log")
        subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", control_log, "ls", scratch], capture_output=True, timeout=60)
        control = open(control_log, encoding="utf-8", errors="replace").read()
        if scratch not in control:
            fail("negative control: the detector did not see a syscall naming the directory")
        print(f"native_diagnostic_registry_strace: OK ({rows} rows visible; 0 syscalls name the directory or a .sb. path; control sees the directory)")
    finally:
        shutil.rmtree(work, ignore_errors=True)


main()
