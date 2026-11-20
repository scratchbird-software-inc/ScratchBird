#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_LOOP_TRANSACTION_STRACE (thin-slice S4d, plan 40; owner Q305, Q309, Q315).

The `measure` scenario of native_loop_transaction_test (a served production-created database; the loop's begin / complete /
complete-and-begin work and one engine read-transaction scope that also runs the S4a credential read and the names read)
run for one page size under strace -f. The child marks each region on stderr (write(2, "MARK <name>")). Asserts:
  - the child reports LOOP_TXN_OK;
  - 0 syscalls name a ".sb." path (no lock file, no side file);
  - the directory holds only the data file;
  - before the first mark (the open and its Restart) and after the last (the close) there is NO write-class syscall
    on the data descriptor(s): the open of a clean file and the close write nothing;
  - inside every marked region the data descriptor sees exactly the pwrite64 (each one 512-byte write unit) and the
    fsync / fdatasync calls the loop reported for that region (writes and syncs of begin + completion for the scope
    region, so the credential read and the names read inside the scope wrote nothing), and no other write-class syscall;
  - comparing the file before and after page by page, only pages of the TIP state chain and the transaction header chain changed;
  - no test password appears in the trace, the child's output or the file.
Prints the measured table (the S5 baseline: syncs and writes per begin and per completion of a read-only transaction).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, data_descriptors, fail, snapshot  # noqa: E402

PASSWORDS = ("S4d-Bootstrap-Pw-5b19e0c47a", "S4d-Wrong-Pw-91d03c7ee2")
OTHER_WRITES = r'(write|pwritev2?|ftruncate|fallocate|sync_file_range)'


def forms(text):
    raw = text.encode()
    return (text, "".join("\\x%02x" % b for b in raw[:8]), raw.hex(), raw[:12].hex())


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--size", type=int, choices=SIZES)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_loop_txn_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "loop.sdb")
            created = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
            with open(path, "rb") as handle:
                image_before = handle.read()
            before = snapshot(directory)
            log = os.path.join(work, f"measure-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "serve", path, "measure",
                       field(created.stdout, "database"), field(created.stdout, "bootstrap"), str(size)]
            ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
            if ran.returncode != 0 or "LOOP_TXN_OK" not in ran.stdout:
                fail(f"{size}: exit {ran.returncode}: {ran.stdout[-1500:]} {ran.stderr[-2000:]}")
            # What the loop reported per region.
            reported = {}
            order_of = {}
            for line in ran.stdout.splitlines():
                m = re.match(r"MEASURE op=(\S+) order=(\d+) writes=(\d+) syncs=(\d+) growth_writes=(\d+) growth_syncs=(\d+)", line)
                if m:
                    op = m.group(1)
                    w = int(m.group(3)) + int(m.group(5))
                    s = int(m.group(4)) + int(m.group(6))
                    region = "scope_login_names" if op.startswith("scope_login_names") else op
                    old = reported.get(region, (0, 0))
                    reported[region] = (old[0] + w, old[1] + s)
                    order_of[op] = int(m.group(2))
            if not reported:
                fail(f"{size}: the child reported no measurement")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = handle.readlines()
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"{size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if os.listdir(directory) != ["loop.sdb"]:
                fail(f"{size}: the directory holds more than the data file: {os.listdir(directory)}")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"{size}: no open of the data file was traced")

            def on_data(fd, index):
                return any(f == fd and start < index < end for f, start, end in spans)

            region = "startup"
            counts = {"startup": [0, 0]}
            for index, line in enumerate(lines):
                m = re.search(r'write\(2, "MARK (\S+)\\n"', line)
                if m:
                    region = "after" if m.group(1) == "end" else m.group(1)
                    counts.setdefault(region, [0, 0])
                    continue
                m = re.search(r'\bpwrite64\((\d+),.*,\s*(\d+),\s*\d+\)\s*=\s*(\d+)', line)
                if m and on_data(m.group(1), index):
                    if m.group(2) != "512" or m.group(3) != "512":
                        fail(f"{size} {region}: a {m.group(2)}-byte data write (returned {m.group(3)})")
                    counts[region][0] += 1
                    continue
                m = re.search(r'\b(?:fsync|fdatasync)\((\d+)\)\s*=\s*0', line)
                if m and on_data(m.group(1), index):
                    counts[region][1] += 1
                    continue
                m = re.search(r'\b' + OTHER_WRITES + r'\((\d+)', line)
                if m and on_data(m.group(2), index):
                    fail(f"{size} {region}: an unexpected write-class syscall on the data file: {line.strip()}")
            # The open of a clean file writes and syncs nothing; the close writes nothing (it ends with one fsync of the
            # data file, as every served database's close does: native_serving_strace records the same).
            if counts["startup"] != [0, 0]:
                fail(f"{size}: {counts['startup'][0]} writes / {counts['startup'][1]} syncs on the data file during the open")
            if counts["after"][0] != 0 or counts["after"][1] > 1:
                fail(f"{size}: {counts['after'][0]} writes / {counts['after'][1]} syncs on the data file during the close")
            for name, (w, s) in reported.items():
                got = counts.get(name)
                if got is None:
                    fail(f"{size}: region {name} was not traced")
                if got[0] != w or got[1] != s:
                    fail(f"{size} {name}: strace saw {got[0]} writes / {got[1]} syncs on the data file, the loop reported {w} / {s}")
            if snapshot(directory).keys() != before.keys():
                fail(f"{size}: the directory's files changed")
            # Only TIP / header pages changed, page by page.
            pages = subprocess.run([args.test_bin, "pages", path], capture_output=True, text=True, timeout=300)
            match = re.search(r"page_size=(\d+) next=(\d+) state=([\d,]*) header=([\d,]*)", pages.stdout)
            if not match:
                fail(f"{size}: pages mode: {pages.stdout} {pages.stderr}")
            page_size = int(match.group(1))
            allowed = {int(x) for x in match.group(3).split(",") if x} | {int(x) for x in match.group(4).split(",") if x}
            with open(path, "rb") as handle:
                image_after = handle.read()
            if len(image_after) != len(image_before):
                fail(f"{size}: the file length changed ({len(image_before)} -> {len(image_after)})")
            changed = {n for n in range(len(image_after) // page_size)
                       if image_before[n * page_size:(n + 1) * page_size] != image_after[n * page_size:(n + 1) * page_size]}
            if not changed or not changed <= allowed:
                fail(f"{size}: changed pages {sorted(changed)} are not a subset of the TIP/header pages {sorted(allowed)}")
            text = "".join(lines) + ran.stdout + ran.stderr
            for password in PASSWORDS:
                for form in forms(password):
                    if form in text:
                        fail(f"{size}: a test password appears in the trace or the output as {form!r}")
                if password.encode() in image_after:
                    fail(f"{size}: a test password appears in the database file")
            print(f"native_loop_transaction_strace {size}: LOOP_TXN_OK; 0 .sb. syscalls; clean open writes and syncs nothing, the close writes nothing (close fsyncs: {counts['after'][1]}); "
                  f"changed pages {sorted(changed)} (TIP/header pages of {sorted(allowed)}); {len(lines)} syscalls traced")
            for name in ("scope_login_names", "scope_snapshot", "begin_ro", "commit_ro", "begin_rw", "cab_commit", "cab_rollback", "rollback_rw"):
                w, s = counts[name]
                print(f"  {name:18s} pwrite64={w} (512 B each) fsync/fdatasync={s}  (the loop reported the same)"
                      + (f"  order={order_of.get(name, order_of.get('scope_login_names_begin'))}" if name != "scope_login_names" else
                         f"  begin order={order_of['scope_login_names_begin']}"))
        print("native_loop_transaction_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
