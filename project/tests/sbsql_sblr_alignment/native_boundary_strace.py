#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_BOUNDARY_STRACE (thin-slice S5, 2026-12-09). The measured writes and syncs of ONE COMMIT / ROLLBACK boundary
(the real server step, in process) under strace -f with the pid-aware attribution shared by every strace test.

The child `native_boundary serve <path> trace` marks a region `b<n>` before each of 8 boundaries (n = the completing transaction's order).
Asserted: 0 syscalls name a `.sb.` path; every write-class syscall on the data file is a 512-byte pwrite64 or an fsync; each boundary region
holds EXACTLY the T1 numbers (plan 36, owner Q314): 4 sector writes for an odd n and 3 for an even n, each followed by its sync (the loop's
sync_every_dependency policy: 4 or 3 syncs; T1's plan-level "1 sync per call" is the writer's default policy, not the loop's);
the boundary opens the data file by path 0 times. The measured numbers are printed.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, data_descriptors, fail, merge_split_lines  # noqa: E402

OTHER_WRITES = r'(write|writev|pwritev2?|pwrite|ftruncate|fallocate|sync_file_range)'


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--material-bin", required=True)
    parser.add_argument("--size", type=int, choices=SIZES, required=True)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_boundary_strace_")
    try:
        directory = os.path.join(work, "d")
        os.mkdir(directory)
        path = os.path.join(directory, "boundary.sdb")
        created = subprocess.run([args.material_bin, "create", path, str(args.size)], capture_output=True, text=True, timeout=600)
        if created.returncode != 0 or not created.stdout.startswith("CREATED "):
            fail(f"create: {created.stdout[-500:]} {created.stderr[-1500:]}")
        log = os.path.join(work, "trace.strace")
        command = ["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "serve", path, "trace",
                   field(created.stdout, "database"), field(created.stdout, "bootstrap"), field(created.stdout, "service")]
        ran = subprocess.run(command, capture_output=True, text=True, timeout=900)
        if ran.returncode != 0 or "RECEIPT_OK" not in ran.stdout:
            fail(f"exit {ran.returncode}: {ran.stdout[-1500:]} {ran.stderr[-2000:]}")
        with open(log, encoding="utf-8", errors="replace") as handle:
            lines = merge_split_lines(handle.readlines())
        sb_lines = [line for line in lines if ".sb." in line]
        if sb_lines:
            fail(f"{len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
        spans = data_descriptors(lines, path)
        if not spans:
            fail("no open of the data file was traced")
        region = "startup"
        counts = {}
        opens = {}
        for index, line in enumerate(lines):
            m = re.search(r'write\(2, "MARK (\S+)\\n"', line)
            if m:
                region = m.group(1)
                counts.setdefault(region, [0, 0])
                continue
            counts.setdefault(region, [0, 0])
            if re.search(r'\bopen(?:at2?)?\(', line) and "boundary.sdb" in line:
                opens.setdefault(region, []).append(line.strip())
            m = re.search(r'\bpwrite64\((\d+),.*,\s*(\d+),\s*\d+\)\s*=\s*(\d+)', line)
            if m and spans.covers(index):
                if m.group(2) != "512" or m.group(3) != "512":
                    fail(f"{region}: a {m.group(2)}-byte data write (returned {m.group(3)})")
                counts[region][0] += 1
                continue
            m = re.search(r'\b(?:fsync|fdatasync)\((\d+)\)\s*=\s*0', line)
            if m and spans.covers(index):
                counts[region][1] += 1
                continue
            m = re.search(r'\b' + OTHER_WRITES + r'\((\d+)', line)
            if m and spans.covers(index):
                fail(f"{region}: a write-class syscall that is not a 512-byte pwrite64 or an fsync on the data file: {line.strip()}")
        boundaries = sorted((int(name[1:]), value) for name, value in counts.items() if re.fullmatch(r"b\d+", name))
        if len(boundaries) != 8:
            fail(f"expected 8 traced boundaries, found {len(boundaries)}")
        report = []
        for order, (writes, syncs) in boundaries:
            expected = 4 if order % 2 == 1 else 3
            if (writes, syncs) != (expected, expected):
                fail(f"boundary completing slot {order}: {writes} writes / {syncs} syncs, T1 says {expected} writes / {expected} syncs under sync_every_dependency")
            if opens.get(f"b{order}"):
                fail(f"boundary {order} opened the data file by path: {opens[f'b{order}'][0]}")
            report.append(f"n={order}:{writes}w/{syncs}s")
        if counts.get("approve", [0, 0])[0] < 4:
            fail(f"the approval region shows {counts.get('approve')}: the detector does not see the loop's writes")
        print("native_boundary_strace " + str(args.size) + ": RECEIPT_OK; 0 .sb. syscalls; per boundary (completing order: writes/syncs) " + " ".join(report))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
