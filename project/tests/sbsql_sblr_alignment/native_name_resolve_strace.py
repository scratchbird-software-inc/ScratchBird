#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_NAME_RESOLVE_STRACE (thin-slice S4b, plan 40; owner Q306 = 1, Q309).

The served name-resolve child (native_name_resolve_test, mode "serve") run under strace -f at one page size, for the
`serving` and `restricted` databases (a committing slot left by the native_serving binary's own writer) or for the
`names` and `dead` databases (names written by the production storage writers BEFORE the trace, by a separate child).
Every resolve case of the child runs inside the trace. Asserts:
  - the child reports NAME_RESOLVE_OK;
  - 0 syscalls name a ".sb." path (no lock file, no side file);
  - the directory holds only the data file, byte-identical afterwards;
  - 0 write-class syscalls (write, pwrite64, pwritev, ftruncate, fallocate, fsync, fdatasync, sync_file_range) on any
    descriptor opened on the data file or duplicated from one, for the whole run: a names read writes nothing, and
    neither the open of these states nor the close does;
  - the test password (used only for the cross-check with the S4a login-material read) appears nowhere.
A negative control shows the write detector sees the create's writes. The `failed` database is not traced (its Restart
writes and fails by construction) and neither is `uncommitted` (the open's Restart kills the dead transaction: a write).
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

# The same constant as native_name_resolve_test.cpp (kBootstrapPassword).
PASSWORDS = ("S4b-Bootstrap-Pw-5e1d77a0c3",)


def forms(text):
    raw = text.encode()
    return (text, "".join("\\x%02x" % b for b in raw[:8]), raw.hex(), raw[:12].hex())


def writes_on_data(lines, path):
    spans = data_descriptors(lines, path)
    out = []
    for index, line in enumerate(lines):
        match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
        if match and any(f == match.group(2) and start < index < end for f, start, end in spans):
            out.append(line.strip())
    return spans, out


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--serving-bin", required=True)
    parser.add_argument("--size", type=int, choices=SIZES)
    parser.add_argument("--states", nargs="+", default=["serving", "restricted", "names", "dead"],
                        choices=["serving", "restricted", "names", "dead"])
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_name_resolve_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            for state in args.states:
                directory = os.path.join(work, f"{state}{size}")
                os.mkdir(directory)
                path = os.path.join(directory, "names.sdb")
                created = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
                if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                    fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
                if state == "restricted":
                    fixture = subprocess.run([args.serving_bin, "fixture", path, "committing"], capture_output=True, text=True, timeout=600)
                    if fixture.returncode != 0 or "FIXTURE ok" not in fixture.stdout:
                        fail(f"fixture {size}: {fixture.stdout[-500:]} {fixture.stderr[-1000:]}")
                elif state in ("names", "dead"):
                    forged = subprocess.run([args.test_bin, "forge", path, state, field(created.stdout, "bootstrap"),
                                             field(created.stdout, "service")], capture_output=True, text=True, timeout=900)
                    if forged.returncode != 0 or f"FORGED kind={state}" not in forged.stdout:
                        fail(f"forge {state} {size}: {forged.stdout[-500:]} {forged.stderr[-2000:]}")
                before = snapshot(directory)
                log = os.path.join(work, f"resolve-{state}-{size}.strace")
                command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, "serve", path, state,
                           field(created.stdout, "database"), field(created.stdout, "bootstrap"), field(created.stdout, "service")]
                ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
                if ran.returncode != 0 or "NAME_RESOLVE_OK" not in ran.stdout:
                    fail(f"{state} {size}: exit {ran.returncode}: {ran.stdout[-800:]} {ran.stderr[-2000:]}")
                with open(log, encoding="utf-8", errors="replace") as handle:
                    lines = handle.readlines()
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
                text = "".join(lines) + ran.stdout + ran.stderr
                with open(path, "rb") as data:
                    image = data.read()
                for password in PASSWORDS:
                    for form in forms(password):
                        if form in text:
                            fail(f"{state} {size}: the test password appears in the trace or the output as {form!r}")
                    if password.encode() in image:
                        fail(f"{state} {size}: the test password appears in the database file")
                print(f"native_name_resolve_strace {state} {size}: NAME_RESOLVE_OK; 0 .sb. syscalls; 0 write-class syscalls on "
                      f"{len(spans)} data descriptor(s); directory unchanged; no password in the trace; {len(lines)} syscalls traced")
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
        print("native_name_resolve_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
