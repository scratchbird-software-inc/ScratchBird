#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_LOGIN_STRACE_S4C (thin-slice S4c, plan 40; owner Q306-Q312, the 2026-11-21 snapshot-point ruling).

The served NATIVE LOGIN child (native_login_test, mode "serve") run under strace -f at the given page size, for the
`serving` and the `restricted` database (a committing slot left by the native_serving binary's own writer). Every login
case of the child runs inside the trace: accepted, every credential cause, the injection cases, the attach and the
embedded attach refusals, the close. (The create, the login-name forge and the offline derivation are separate processes
run before the trace.) Asserts:
  - the child reports LOGIN_OK;
  - 0 syscalls name a ".sb." path (no lock file, no side file, no legacy security file);
  - the directory holds only the data file, byte-identical afterwards [S4e: for `restricted` only; `serving` approves logins, which
    write: see the S4e note in the check];
  - 0 write-class syscalls (write, pwrite64, pwritev, ftruncate, fallocate, fsync, fdatasync, sync_file_range) on any
    descriptor opened on the data file or duplicated from one, for the whole run (a credential read writes nothing,
    and neither the open of these two states nor the close does) [S4e: for `restricted` only; for `serving` every such syscall must
    be a 512-byte pwrite64 or an fsync];
  - no test password appears in the trace (plain text or as the escaped bytes strace prints), in the child's output
    or in the file.
A negative control shows the write detector sees the create's writes. The `failed` database is not traced here: its
Restart writes (and fails) by construction.
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

# The same constants as native_login_test.cpp (kBootstrapPassword / kWrongPassword).
PASSWORDS = ("S4a-Bootstrap-Pw-7f3a91c2d8", "S4c-Wrong-Pw-4d9e1a77c0b2")


def forms(text):
    raw = text.encode()
    return (text, "".join("\\x%02x" % b for b in raw[:8]), raw.hex(), raw[:12].hex())


def writes_on_data(lines, path):
    spans = data_descriptors(lines, path)
    out = []
    for index, line in enumerate(lines):
        match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
        if match and spans.covers(index):
            out.append(line.strip())
    return spans, out


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--material-bin", required=True)
    parser.add_argument("--names-bin", required=True)
    parser.add_argument("--serving-bin", required=True)
    # One page size per ctest entry (each takes about 20 s untraced, 25 s traced); none: all five.
    parser.add_argument("--size", type=int, choices=SIZES)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_login_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            for state in ("serving", "restricted"):
                directory = os.path.join(work, f"{state}{size}")
                os.mkdir(directory)
                path = os.path.join(directory, "login.sdb")
                created = subprocess.run([args.material_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
                if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                    fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
                if state == "serving":   # (a restricted file carries a committing slot: no names forge, as in native_login_test)
                    forged = subprocess.run([args.names_bin, "forge", path, "login", field(created.stdout, "bootstrap"), field(created.stdout, "service")],
                                            capture_output=True, text=True, timeout=600)
                    if forged.returncode != 0 or "FORGED kind=login" not in forged.stdout:
                        fail(f"forge login {size}: {forged.stdout[-500:]} {forged.stderr[-1000:]}")
                if state == "restricted":
                    fixture = subprocess.run([args.serving_bin, "fixture", path, "committing"], capture_output=True, text=True, timeout=600)
                    if fixture.returncode != 0 or "FIXTURE ok" not in fixture.stdout:
                        fail(f"fixture {size}: {fixture.stdout[-500:]} {fixture.stderr[-1000:]}")
                derived = ""
                if state in ("serving", "restricted"):
                    d = subprocess.run([args.material_bin, "derive", path, field(created.stdout, "binding")], capture_output=True, text=True, timeout=600)
                    if d.returncode != 0 or not d.stdout.startswith("DERIVED"):
                        fail(f"derive {size}: {d.stdout[-300:]} {d.stderr[-500:]}")
                    derived = d.stdout[len("DERIVED"):].strip()
                before = snapshot(directory)
                log = os.path.join(work, f"login-{state}-{size}.strace")
                command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, "serve", path, state,
                           field(created.stdout, "database"), field(created.stdout, "bootstrap"),
                           field(created.stdout, "service")]
                env = dict(os.environ)
                if derived:
                    env["SB_LOGIN_TEST_DERIVED"] = derived   # the derived agent passwords: in the environment, never argv
                ran = subprocess.run(command, capture_output=True, text=True, timeout=1500, env=env)
                if ran.returncode != 0 or "LOGIN_OK" not in ran.stdout:
                    fail(f"{state} {size}: exit {ran.returncode}: {ran.stdout[-800:]} {ran.stderr[-2000:]}")
                with open(log, encoding="utf-8", errors="replace") as handle:
                    lines = merge_split_lines(handle.readlines())
                sb_lines = [line for line in lines if ".sb." in line]
                if sb_lines:
                    fail(f"{state} {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
                spans, writes = writes_on_data(lines, path)
                if not spans:
                    fail(f"{state} {size}: no open of the data file was traced")
                if state == "restricted":
                    # A restricted database logs in but its session is refused (Q299) before anything is written.
                    if snapshot(directory) != before:
                        fail(f"{state} {size}: the directory changed (a file was created or written)")
                    if writes:
                        fail(f"{state} {size}: {len(writes)} write-class syscalls on the data file, first: {writes[0]}")
                else:
                    # S4e (owner 2026-11-21: the user's session and first transaction begin only when authentication is
                    # APPROVED). OLD (S4c): 0 write-class syscalls on the data file and a byte-identical directory in BOTH
                    # states. NOW the `serving` child's approved logins (and its graceful close, which rolls the sessions'
                    # transactions back and ends the sessions) write: the ledger appends and the TIP units, nothing else. The
                    # exact counts are asserted by native_session_approval_strace.py; here every write-class syscall must be a
                    # 512-byte pwrite64 or an fsync on the data file, the directory must hold the same file, and its length is unchanged.
                    after = snapshot(directory)
                    if after.keys() != before.keys() or any(after[name][0] != before[name][0] for name in before):
                        fail(f"{state} {size}: the directory's files or their lengths changed")
                    others = [w for w in writes if not re.search(r'\b(pwrite64\(\d+,.*,\s*512,\s*\d+\)\s*=\s*512|fsync\(\d+\)\s*=\s*0|fdatasync\(\d+\)\s*=\s*0)', w)]
                    if others:
                        fail(f"{state} {size}: a write-class syscall on the data file that is not a 512-byte pwrite64 or an fsync: {others[0]}")
                    if not writes:
                        fail(f"{state} {size}: no write on the data file although logins were approved")
                text = "".join(lines) + ran.stdout + ran.stderr
                with open(path, "rb") as data:
                    image = data.read()
                for password in PASSWORDS:
                    for form in forms(password):
                        if form in text:
                            fail(f"{state} {size}: a test password appears in the trace or the output as {form!r}")
                    if password.encode() in image:
                        fail(f"{state} {size}: a test password appears in the database file")
                what = ("0 write-class syscalls on " + f"{len(spans)} data descriptor(s); directory unchanged" if state == "restricted"
                        else f"{len(writes)} write-class syscalls on {len(spans)} data descriptor(s), all 512-byte pwrite64 / fsync (approved logins)")
                print(f"native_login_strace {state} {size}: LOGIN_OK; 0 .sb. syscalls; {what}; no password in the trace; {len(lines)} syscalls traced")
        control_dir = os.path.join(work, "control")
        os.mkdir(control_dir)
        control = os.path.join(control_dir, "control.sdb")
        control_log = os.path.join(work, "control.strace")
        subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", control_log, args.material_bin, "create", control, "16384"],
                       capture_output=True, text=True, timeout=600)
        with open(control_log, encoding="utf-8", errors="replace") as handle:
            control_lines = merge_split_lines(handle.readlines())
        _, control_writes = writes_on_data(control_lines, control)
        if not control_writes:
            fail("the detector saw no write-class syscall during a create: it cannot be trusted")
        print(f"negative control: {len(control_writes)} write-class syscalls on the data file during create")
        print("native_login_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
