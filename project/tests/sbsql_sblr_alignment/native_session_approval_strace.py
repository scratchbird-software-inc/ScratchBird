#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_SESSION_APPROVAL_STRACE (S4e; owner 2026-11-21: the user's session and first transaction begin only when
authentication is APPROVED; 2026-11-22: the session ledger is a careful-write chain, durable before cited).

The served child `native_login serve <path> trace` run under strace -f at the given page size. The child marks regions on
stderr (`write(2, "MARK x\\n")`) and prints what the database loop reported per region:
  failed        a wrong-password login: the loop reports 0 writes / 0 syncs
  approve       two approvals (each: connection, session and attachment ledger records, then the first transaction's begin)
  end_sessions  both sessions ended through the loop (each: the transaction's rollback, then the ended ledger record)
  after         the close: nothing is left to write
Asserted, with the pid-aware attribution shared by every strace test (native_serving_strace.data_descriptors over
strace_attribution.py):
  - the child reports LOGIN_OK; 0 syscalls name a ".sb." path; the directory holds only the data file and its length is unchanged;
  - EVERY write-class syscall on a descriptor of the data file is a 512-byte pwrite64 or an fsync / fdatasync (no write, pwritev,
    ftruncate, fallocate, sync_file_range): the only writes are ledger appends and TIP begin / complete units;
  - per region, the data-file pwrite64 and fsync counts equal what the loop reported (so a write the loop did not account for
    cannot hide), the failed login's region has none, the open of a clean file writes nothing, and the close writes nothing
    (it ends with one fsync of the data file, as every served database's close does);
  - the pages that changed are a subset of the transaction-state, transaction-header and session-ledger chains' pages;
  - no test password appears in the trace, the output or the file.
A negative control shows the detector sees a create's writes.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, data_descriptors, fail, merge_split_lines, snapshot  # noqa: E402

PASSWORDS = ("S4a-Bootstrap-Pw-7f3a91c2d8", "S4c-Wrong-Pw-4d9e1a77c0b2")
OTHER_WRITES = r'(write|writev|pwritev2?|pwrite|ftruncate|fallocate|sync_file_range)'


def forms(text):
    raw = text.encode()
    return (text, "".join("\\x%02x" % b for b in raw[:8]), raw.hex(), raw[:12].hex())


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)      # native_login (serve / pages)
    parser.add_argument("--material-bin", required=True)  # native_login_material (create)
    parser.add_argument("--size", type=int, choices=SIZES)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_session_approval_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "login.sdb")
            created = subprocess.run([args.material_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
            with open(path, "rb") as handle:
                image_before = handle.read()
            before = snapshot(directory)
            log = os.path.join(work, f"trace-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "serve", path, "trace",
                       field(created.stdout, "database"), field(created.stdout, "bootstrap"), field(created.stdout, "service")]
            ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
            if ran.returncode != 0 or "LOGIN_OK" not in ran.stdout:
                fail(f"{size}: exit {ran.returncode}: {ran.stdout[-1500:]} {ran.stderr[-2000:]}")
            reported = {}
            for line in ran.stdout.splitlines():
                m = re.match(r"MEASURE op=(\S+) writes=(\d+) syncs=(\d+)", line)
                if m:
                    reported[m.group(1)] = (int(m.group(2)), int(m.group(3)))
            for region in ("failed", "approve", "end_sessions"):
                if region not in reported:
                    fail(f"{size}: the child reported no measurement for {region}")
            if reported["approve"][0] < 8 or reported["end_sessions"][0] < 4:
                fail(f"{size}: implausible reported counts {reported}")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = merge_split_lines(handle.readlines())
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"{size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if os.listdir(directory) != ["login.sdb"]:
                fail(f"{size}: the directory holds more than the data file: {os.listdir(directory)}")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"{size}: no open of the data file was traced")
            region = "startup"
            counts = {"startup": [0, 0]}
            for index, line in enumerate(lines):
                m = re.search(r'write\(2, "MARK (\S+)\\n"', line)
                if m:
                    region = "after" if m.group(1) == "end" else m.group(1)
                    counts.setdefault(region, [0, 0])
                    continue
                m = re.search(r'\bpwrite64\((\d+),.*,\s*(\d+),\s*\d+\)\s*=\s*(\d+)', line)
                if m and spans.covers(index):
                    if m.group(2) != "512" or m.group(3) != "512":
                        fail(f"{size} {region}: a {m.group(2)}-byte data write (returned {m.group(3)})")
                    counts[region][0] += 1
                    continue
                m = re.search(r'\b(?:fsync|fdatasync)\((\d+)\)\s*=\s*0', line)
                if m and spans.covers(index):
                    counts[region][1] += 1
                    continue
                m = re.search(r'\b' + OTHER_WRITES + r'\((\d+)', line)
                if m and spans.covers(index):
                    fail(f"{size} {region}: a write-class syscall that is not a 512-byte pwrite64 or an fsync, on the data file: {line.strip()}")
            if counts["startup"] != [0, 0]:
                fail(f"{size}: {counts['startup'][0]} writes / {counts['startup'][1]} syncs on the data file during the open")
            if counts["after"][0] != 0 or counts["after"][1] > 1:
                fail(f"{size}: {counts['after'][0]} writes / {counts['after'][1]} syncs on the data file during the close")
            for name, (w, s) in reported.items():
                got = counts.get(name)
                if got is None:
                    fail(f"{size}: region {name} was not traced")
                if got[0] != w or got[1] != s:
                    fail(f"{size} {name}: strace saw {got[0]} pwrite64 / {got[1]} fsync on the data file, the loop reported {w} / {s}")
            if counts["failed"] != [0, 0]:
                fail(f"{size}: a failed login wrote to the data file: {counts['failed']}")
            if snapshot(directory).keys() != before.keys():
                fail(f"{size}: the directory's files changed")
            pages = subprocess.run([args.test_bin, "pages", path, str(size)], capture_output=True, text=True, timeout=300)
            match = re.search(r"PAGES state=([\d,]*) header=([\d,]*) ledger=([\d,]*)", pages.stdout)
            if not match:
                fail(f"{size}: pages mode: {pages.stdout} {pages.stderr}")
            allowed = set()
            for group in (1, 2, 3):
                allowed |= {int(x) for x in match.group(group).split(",") if x}
            with open(path, "rb") as handle:
                image_after = handle.read()
            if len(image_after) != len(image_before):
                fail(f"{size}: the file length changed ({len(image_before)} -> {len(image_after)})")
            changed = {n for n in range(len(image_after) // size)
                       if image_before[n * size:(n + 1) * size] != image_after[n * size:(n + 1) * size]}
            if not changed or not changed <= allowed:
                fail(f"{size}: changed pages {sorted(changed)} are not a subset of the TIP / header / ledger pages {sorted(allowed)}")
            text = "".join(lines) + ran.stdout + ran.stderr
            for password in PASSWORDS:
                for form in forms(password):
                    if form in text:
                        fail(f"{size}: a test password appears in the trace or the output as {form!r}")
                if password.encode() in image_after:
                    fail(f"{size}: a test password appears in the database file")
            print(f"native_session_approval_strace {size}: LOGIN_OK; 0 .sb. syscalls; every write-class syscall on the data file is a 512-byte "
                  f"pwrite64 or an fsync; clean open 0 writes, close 0 writes ({counts['after'][1]} fsync); changed pages {sorted(changed)} "
                  f"within the chains {sorted(allowed)}; {len(lines)} syscalls traced")
            for name in ("failed", "approve", "end_sessions"):
                print(f"  {name:13s} pwrite64={counts[name][0]} fsync={counts[name][1]}  (the loop reported {reported[name][0]} / {reported[name][1]})")
        control_dir = os.path.join(work, "control")
        os.mkdir(control_dir)
        control = os.path.join(control_dir, "control.sdb")
        control_log = os.path.join(work, "control.strace")
        subprocess.run(["strace", "-f", "-qq", "-s", "64", "-o", control_log, args.material_bin, "create", control, "16384"],
                       capture_output=True, text=True, timeout=600)
        with open(control_log, encoding="utf-8", errors="replace") as handle:
            control_lines = merge_split_lines(handle.readlines())
        control_spans = data_descriptors(control_lines, control)
        control_writes = [line for index, line in enumerate(control_lines)
                          if re.search(r'\bpwrite64\(', line) and control_spans.covers(index)]
        if not control_writes:
            fail("the detector saw no pwrite64 on the data file during a create: it cannot be trusted")
        print(f"negative control: {len(control_writes)} pwrite64 on the data file during create")
        print("native_session_approval_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
