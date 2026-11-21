#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_SNAPSHOT_POINT_VIEW_STRACE. The serving child of native_snapshot_view_test under strace -f. The child
marks regions on stderr ("MARK <name>"): change_* regions make real catalog changes, read_* regions are the scope's and
the live reads. Asserts: the child reports SNAPVIEW_OK; 0 syscalls name a ".sb." path; the directory holds only the data
file; in every read_* region 0 write-class syscalls on the data file (pid-aware attribution, shared helper); the change
regions DO write (negative control: the detector sees the writes); no test password in the trace."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, WRITE_CALLS, data_descriptors, fail, merge_split_lines, snapshot  # noqa: E402

PASSWORDS = ("S4f-Bootstrap-Pw-3c88d10e6b", "S4f-Wrong-Pw-b0a4e9d217")


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
    work = tempfile.mkdtemp(prefix="sb_native_snapview_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "snap.sdb")
            created = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
            log = os.path.join(work, f"snap-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "serve", path,
                       field(created.stdout, "database"), field(created.stdout, "bootstrap"), field(created.stdout, "service")]
            ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
            if ran.returncode != 0 or "SNAPVIEW_OK" not in ran.stdout:
                fail(f"{size}: exit {ran.returncode}: {ran.stdout[-1500:]} {ran.stderr[-2000:]}")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = merge_split_lines(handle.readlines())
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"{size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if os.listdir(directory) != ["snap.sdb"]:
                fail(f"{size}: the directory holds more than the data file: {os.listdir(directory)}")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"{size}: no open of the data file was traced")
            region = "startup"
            seen_regions = {}
            for index, line in enumerate(lines):
                m = re.search(r'write\(2, "MARK (\S+)\\n"', line)
                if m:
                    region = m.group(1)
                    seen_regions.setdefault(region, 0)
                    continue
                m = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
                if m and spans.covers(index):
                    seen_regions[region] = seen_regions.get(region, 0) + 1
                    if region.startswith("read"):
                        fail(f"{size}: a write-class syscall on the data file in region {region}: {line.strip()}")
            reads = [r for r in seen_regions if r.startswith("read")]
            changes = [r for r in seen_regions if r.startswith("change")]
            if len(reads) < 8 or len(changes) < 4:
                fail(f"{size}: the child did not mark the expected regions: {sorted(seen_regions)}")
            if not any(seen_regions[r] for r in changes):
                fail(f"{size}: the detector saw no write in any change region: it cannot be trusted")
            text = "".join(lines) + ran.stdout + ran.stderr
            for password in PASSWORDS:
                if password in text:
                    fail(f"{size}: a test password appears in the trace or the output")
            print(f"native_snapshot_view_strace {size}: SNAPVIEW_OK; 0 .sb. syscalls; 0 write-class syscalls on the data file in "
                  f"{len(reads)} read regions; {sum(seen_regions[r] for r in changes)} writes seen in {len(changes)} change regions; "
                  f"{len(lines)} syscalls traced")
        print("native_snapshot_view_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
