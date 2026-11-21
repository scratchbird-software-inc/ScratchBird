#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_LOOP_LEDGER_STRACE (plan 36 T4, owner Q165 = 1).

The `ledger_measure` scenario of native_session_ledger_loop_test (a served production-created database; one append, a
page filled by appends, then the append that grows the chain) for one page size under strace -f, with the shared pid-aware
attribution of native_serving_strace. Asserts:
  - the child reports LEDGER_LOOP_OK;
  - 0 syscalls name a ".sb." path, and the directory holds only the data file;
  - before the first mark (open + verification + Restart + ledger Load) and after the last (the close) there is NO
    write-class syscall on the data descriptor: the open and the close write nothing;
  - inside every marked region the data descriptor sees exactly the pwrite64 and fsync/fdatasync calls the loop reported
    (append_plain: 1 write of 512 bytes + 1 sync; fill: one of each per append; append_growth: the growth plan's writes
    and syncs plus the append's), and no other write-class syscall; every pwrite64 is a 512-byte unit or a whole page;
  - comparing the file before and after page by page, only pages of the session ledger chain, the allocation map and the
    TIP chains' pages could change, and the ledger chain pages did.
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

OTHER_WRITES = r'(write|pwritev2?|ftruncate|fallocate|sync_file_range)'


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
    work = tempfile.mkdtemp(prefix="sb_native_ledger_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "ledger.sdb")
            created = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
            with open(path, "rb") as handle:
                image_before = handle.read()
            before = snapshot(directory)
            log = os.path.join(work, f"ledger-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "serve", path, "ledger_measure",
                       field(created.stdout, "database"), field(created.stdout, "bootstrap"), str(size)]
            ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
            if ran.returncode != 0 or "LEDGER_LOOP_OK" not in ran.stdout:
                fail(f"{size}: exit {ran.returncode}: {ran.stdout[-1500:]} {ran.stderr[-2000:]}")
            reported = {}
            for line in ran.stdout.splitlines():
                m = re.match(r"MEASURE op=(\S+) order=(\d+) writes=(\d+) syncs=(\d+) growth_writes=(\d+) growth_syncs=(\d+)", line)
                if m:
                    reported[m.group(1)] = (int(m.group(3)) + int(m.group(5)), int(m.group(4)) + int(m.group(6)))
            if set(reported) != {"append_plain", "fill", "append_growth"}:
                fail(f"{size}: the child reported {sorted(reported)}")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = merge_split_lines(handle.readlines())
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"{size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if os.listdir(directory) != ["ledger.sdb"]:
                fail(f"{size}: the directory holds more than the data file: {os.listdir(directory)}")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"{size}: no open of the data file was traced")
            region = "startup"
            counts = {"startup": [0, 0]}
            sizes_seen = set()
            for index, line in enumerate(lines):
                m = re.search(r'write\(2, "MARK (\S+)\\n"', line)
                if m:
                    region = "after" if m.group(1) == "end" else m.group(1)
                    counts.setdefault(region, [0, 0])
                    continue
                m = re.search(r'\bpwrite64\((\d+),.*,\s*(\d+),\s*\d+\)\s*=\s*(\d+)', line)
                if m and spans.covers(index):
                    if m.group(2) != m.group(3) or int(m.group(2)) not in (512, size):
                        fail(f"{size} {region}: a {m.group(2)}-byte data write (returned {m.group(3)})")
                    sizes_seen.add(int(m.group(2)))
                    counts[region][0] += 1
                    continue
                m = re.search(r'\b(?:fsync|fdatasync)\((\d+)\)\s*=\s*0', line)
                if m and spans.covers(index):
                    counts[region][1] += 1
                    continue
                m = re.search(r'\b' + OTHER_WRITES + r'\((\d+)', line)
                if m and spans.covers(index):
                    fail(f"{size} {region}: an unexpected write-class syscall on the data file: {line.strip()}")
            if counts["startup"] != [0, 0]:
                fail(f"{size}: {counts['startup'][0]} writes / {counts['startup'][1]} syncs on the data file during the open")
            if counts["after"][0] != 0 or counts["after"][1] > 1:
                fail(f"{size}: {counts['after'][0]} writes / {counts['after'][1]} syncs on the data file during the close")
            for name, (w, s) in reported.items():
                got = counts.get(name)
                if got is None or got[0] != w or got[1] != s:
                    fail(f"{size} {name}: strace saw {got} writes/syncs on the data file, the loop reported {(w, s)}")
            if size not in sizes_seen or 512 not in sizes_seen:
                fail(f"{size}: expected 512-byte unit writes and a whole-page growth write, saw {sorted(sizes_seen)}")
            if snapshot(directory).keys() != before.keys():
                fail(f"{size}: the directory's files changed")
            pages = subprocess.run([args.test_bin, "pages", path, str(size)], capture_output=True, text=True, timeout=300)
            match = re.search(r"ledger=([\d,]*) map=([\d,]*)", pages.stdout)
            if not match:
                fail(f"{size}: pages mode: {pages.stdout} {pages.stderr}")
            ledger = {int(x) for x in match.group(1).split(",") if x}
            maps = {int(x) for x in match.group(2).split(",") if x}
            with open(path, "rb") as handle:
                image_after = handle.read()
            if len(image_after) != len(image_before):
                fail(f"{size}: the file length changed")
            changed = {n for n in range(len(image_after) // size)
                       if image_before[n * size:(n + 1) * size] != image_after[n * size:(n + 1) * size]}
            if len(ledger) != 2 or not (ledger & changed) or not changed <= (ledger | maps):
                fail(f"{size}: changed pages {sorted(changed)} are not within the ledger pages {sorted(ledger)} and the allocation maps {sorted(maps)}")
            print(f"native_session_ledger_strace {size}: LEDGER_LOOP_OK; 0 .sb. syscalls; the open and the close write nothing; changed pages {sorted(changed)} "
                  f"(ledger {sorted(ledger)}, maps {sorted(maps)}); {len(lines)} syscalls traced")
            for name in ("append_plain", "fill", "append_growth"):
                print(f"  {name:14s} pwrite64={counts[name][0]} fsync/fdatasync={counts[name][1]}  (the loop reported the same)")
        print("native_session_ledger_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
