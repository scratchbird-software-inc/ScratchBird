#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_TRANSACTION_COMPLETE_AND_BEGIN (strace half; plan 36 T1, owner Q150 = 1).

Runs the complete-and-begin test's `--only measure` section for one page size under strace -f and
checks, at the syscall level on the descriptor opened on the measured data file, that every writer
call performs exactly the pwrite64 and fsync/fdatasync calls its own result reports, and records the
S5 baseline: BEGIN, COMMIT, COMMIT then BEGIN (today's replacement) and CompleteAndBegin (n odd:
state words merge; n even: headers and words merge), under the device's default barrier policy and
under sync_every_dependency. Every data write is one 512-byte unit.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from strace_attribution import data_descriptors, merge_split_lines  # noqa: E402

SIZES = (8192, 16384, 32768, 65536, 131072)


def fail(message):
    print("FAIL " + message)
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--sizes", default=",".join(str(s) for s in SIZES))
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_cab_strace_")
    try:
        for profile, size in enumerate(SIZES):
            if str(size) not in args.sizes.split(","):
                continue
            log = os.path.join(work, f"measure-{size}.strace")
            run = subprocess.run(["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "--profile", str(profile),
                                  "--only", "measure"], capture_output=True, text=True, timeout=900)
            if run.returncode != 0:
                fail(f"{size}: exit {run.returncode}: {run.stderr[-800:]} {run.stdout[-400:]}")
            reported = {}  # (policy, op) -> (writes, syncs)
            for line in run.stdout.splitlines():
                m = re.match(r"measure page_size=\d+ forced_ordered=(\d) policy=(\S+) op=(\S+) writes=(\d+) syncs=(\d+)", line)
                if m:
                    reported[(m.group(2), m.group(3))] = (int(m.group(4)), int(m.group(5)), m.group(1))
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = merge_split_lines(handle.readlines())
            # Pid-aware attribution (strace_attribution.py): every descriptor opened on, or duplicated from, measure.db,
            # in the table of the pid that makes the call.
            spans = data_descriptors(lines, re.compile(r'[^"]*measure\.db'))
            current = None
            counts = {}
            for index, text in enumerate(lines):
                m = re.search(r'write\(2, "MARK (\S+) (\S+)\\n"', text)
                if m:
                    current = None if m.group(1) in ("end", "idle") else (m.group(2), m.group(1))
                    if current:
                        counts[current] = [0, 0]
                    continue
                if current is None:
                    continue
                m = re.search(r'\bpwrite64\((\d+),.*,\s*(\d+),\s*\d+\)\s*=\s*(\d+)', text)
                if m and spans.covers(index):
                    if m.group(2) != "512":
                        fail(f"{size}: a {m.group(2)}-byte data write in {current}")
                    counts[current][0] += 1
                m = re.search(r'\b(?:fsync|fdatasync)\((\d+)\)\s*=\s*0', text)
                if m and spans.covers(index):
                    counts[current][1] += 1
            if not counts or not spans:
                fail(f"{size}: no measured calls were traced")
            # Each MARK names the first op of its region; BEGIN+COMMIT and COMMIT+BEGIN regions are single ops.
            for policy in ("trust_forced_writes", "sync_every_dependency"):
                for mark, ops in (("begin", ("begin",)), ("commit", ("commit",)),
                                  ("complete_and_begin_odd", ("complete_and_begin",)),
                                  ("complete_and_begin_even", ("complete_and_begin",)),
                                  ("commit_then_begin", ("commit+begin",))):
                    got = counts.get((policy, mark))
                    if got is None:
                        fail(f"{size} {policy}: mark {mark} not traced")
                    if mark == "complete_and_begin_odd":
                        want = reported[(policy, "complete_and_begin(n=odd)")]
                    elif mark == "complete_and_begin_even":
                        want = reported[(policy, "complete_and_begin(n=even)")]
                    else:
                        want = reported[(policy, ops[0])]
                    if got[0] != want[0] or got[1] != want[1]:
                        fail(f"{size} {policy} {mark}: strace saw {got[0]} writes / {got[1]} syncs, the writer reported "
                             f"{want[0]} / {want[1]}")
                    print(f"native_complete_and_begin_strace {size} forced_ordered={want[2]} {policy} {mark}: "
                          f"pwrite64={got[0]} (512 bytes each) fsync/fdatasync={got[1]} (writer reported the same)")
        print("PASS native_complete_and_begin_strace")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
