#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_RESTART_TEST (strace half; thin-slice S3d, plan 38, owner Q241 = 1).

For each of the five page sizes, runs one serve child (native_serving_test serve <path>) under
strace -f on two files and checks the transaction-store Restart at the syscall level:

  CLEAN file (nothing to mark dead): Restart ran (restart_ran=1, marked_dead=0) and performed 0
  write-class syscalls (write, pwrite64, pwritev, ftruncate, fallocate, fsync, fdatasync,
  sync_file_range) on every descriptor opened on, or duplicated from, the data file: before
  serving, and (Q283) at shutdown. The data file's sha256 is unchanged. 0 ".sb." syscalls.

  DIRTY file (the `many` fixture: 257 transactions left in progress, three 512-byte state units;
  the writer itself made the fixture): Restart marks all 257 dead with exactly three pwrite64 of
  512 bytes each, at 512-byte-aligned offsets inside ONE page, then one fsync (one durability
  barrier after the independent units), all BEFORE the SERVING line, all on a descriptor that is a
  DUPLICATE (F_DUPFD_CLOEXEC) of the custody's read-write open of the data file, made on the
  database loop thread (the only thread that writes). No other write-class syscall touches a data
  descriptor before serving. At shutdown exactly one fsync: the owned writer's dirty flag is set by
  Restart's writes (Q283: an unwritten open syncs nothing; a written one syncs once at close).
  The file differs from its old image in exactly 257 bytes, all inside the transaction-state page
  (one state byte per dead slot); every other byte is identical. 0 ".sb." syscalls.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

SIZES = (8192, 16384, 32768, 65536, 131072)
WRITE_CALLS = r'(pwrite64|write|pwritev2?|ftruncate|fallocate|fsync|fdatasync|sync_file_range)'


def fail(message):
    print("FAIL " + message)
    sys.exit(1)


def sha(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def descriptor_spans(lines, path):
    """(fd, first_line, last_line, origin) for every descriptor opened on the data file or duplicated
    from one. origin is "open" or "dup"."""
    live = {}
    spans = []
    for index, line in enumerate(lines):
        match = re.search(r'open(?:at)?\(.*"' + re.escape(path) + r'".*\)\s*=\s*(\d+)', line)
        if match:
            live[match.group(1)] = (index, "open")
        match = re.search(r'\bfcntl\((\d+),\s*F_DUPFD(?:_CLOEXEC)?,\s*\d+\)\s*=\s*(\d+)', line) or \
            re.search(r'\bdup(?:2|3)?\((\d+)(?:,\s*\d+)*\)\s*=\s*(\d+)', line)
        if match and match.group(1) in live:
            live[match.group(2)] = (index, "dup")
        match = re.search(r'\bclose\((\d+)\)\s*=\s*0', line)
        if match and match.group(1) in live:
            start, origin = live.pop(match.group(1))
            spans.append((match.group(1), start, index, origin))
    for fd, (start, origin) in live.items():
        spans.append((fd, start, len(lines), origin))
    return spans


def span_of(spans, fd, index):
    for f, start, end, origin in spans:
        if f == fd and start < index < end:
            return origin
    return None


def run_serve(test_bin, directory, path, size, label, work):
    log = os.path.join(work, f"{label}-{size}.strace")
    command = ["strace", "-f", "-qq", "-s", "64", "-o", log, test_bin, "serve", path]
    served = subprocess.run(command, capture_output=True, text=True, timeout=900)
    if served.returncode != 0:
        fail(f"{label} {size}: exit {served.returncode}: {served.stderr[-1500:]} {served.stdout[-500:]}")
    first = served.stdout.strip().splitlines()[0] if served.stdout.strip() else ""
    if not first.startswith("SERVING state=serving "):
        fail(f"{label} {size}: {first!r}")
    with open(log, encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()
    sb_lines = [line for line in lines if ".sb." in line]
    if sb_lines:
        fail(f"{label} {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
    spans = descriptor_spans(lines, path)
    if not spans:
        fail(f"{label} {size}: no open of the data file was traced")
    serving_at = next((i for i, line in enumerate(lines) if re.search(r'write\(1, "SERVING', line)), None)
    if serving_at is None:
        fail(f"{label} {size}: the SERVING output line was not traced")
    before, after = [], []
    for index, line in enumerate(lines):
        match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
        if not match:
            continue
        origin = span_of(spans, match.group(2), index)
        if origin is None:
            continue
        (before if index < serving_at else after).append((index, match.group(1), match.group(2), origin, line.strip()))
    return first, lines, before, after


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_restart_")
    try:
        for size in SIZES:
            # ---- clean ----
            directory = os.path.join(work, f"clean{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "restart.sdb")
            if subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, timeout=600).returncode != 0:
                fail(f"create {size}")
            before_sha = sha(path)
            first, lines, before, after = run_serve(args.test_bin, directory, path, size, "clean", work)
            for field in ("restart_ran=1", "marked_dead=0", "restart_writes=0", "restart_syncs=0", "serving_state=serving"):
                if f" {field}" not in first:
                    fail(f"clean {size}: {field} missing: {first[:400]}")
            if before or after:
                fail(f"clean {size}: {len(before)} + {len(after)} write-class syscalls on a data descriptor "
                     f"(a clean Restart performs none), first: {(before + after)[0][4]}")
            if sha(path) != before_sha:
                fail(f"clean {size}: the data file changed")
            print(f"native_restart_strace {size}: clean file: Restart ran, marked 0, 0 write-class syscalls on the data "
                  f"descriptors before or after serving, sha256 unchanged, 0 .sb. syscalls")

            # ---- dirty ----
            directory = os.path.join(work, f"dirty{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "restart.sdb")
            if subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, timeout=600).returncode != 0:
                fail(f"create dirty {size}")
            fixture = subprocess.run([args.test_bin, "fixture", path, "many"], capture_output=True, text=True, timeout=600)
            if fixture.returncode != 0:
                fail(f"fixture {size}: {fixture.stdout} {fixture.stderr}")
            with open(path, "rb") as handle:
                old = handle.read()
            first, lines, before, after = run_serve(args.test_bin, directory, path, size, "dirty", work)
            for field in ("restart_ran=1", "marked_dead=257", "restart_writes=3", "restart_syncs=1", "serving_state=serving"):
                if f" {field}" not in first:
                    fail(f"dirty {size}: {field} missing: {first[:400]}")
            pwrites = [entry for entry in before if entry[1] == "pwrite64"]
            others = [entry for entry in before if entry[1] != "pwrite64"]
            if len(pwrites) != 3:
                fail(f"dirty {size}: expected 3 pwrite64 on the data descriptor before serving, saw {len(pwrites)}: "
                     f"{[e[4][:60] for e in before]}")
            offsets = []
            for _, _, fd, origin, line in pwrites:
                match = re.search(r'pwrite64\(\d+, .*, (\d+), (\d+)\)\s*=\s*(\d+)', line)
                if not match or int(match.group(1)) != 512 or int(match.group(3)) != 512 or int(match.group(2)) % 512:
                    fail(f"dirty {size}: a pwrite64 is not one aligned 512-byte unit: {line[:200]}")
                if origin != "dup":
                    fail(f"dirty {size}: a pwrite64 on a descriptor that is not a duplicate of the custody's open: {line[:200]}")
                offsets.append(int(match.group(2)))
            if len({offset // size for offset in offsets}) != 1:
                fail(f"dirty {size}: the three units are not in one page: {offsets}")
            syncs = [entry for entry in others if entry[1] in ("fsync", "fdatasync")]
            if len(syncs) != 1 or len(others) != 1:
                fail(f"dirty {size}: expected exactly one fsync and nothing else before serving, saw {[e[4][:60] for e in others]}")
            if not (pwrites[-1][0] < syncs[0][0]):
                fail(f"dirty {size}: the fsync is not after the last pwrite64")
            after_syncs = [entry for entry in after if entry[1] in ("fsync", "fdatasync")]
            after_other = [entry for entry in after if entry[1] not in ("fsync", "fdatasync")]
            if len(after_syncs) != 1 or after_other:
                fail(f"dirty {size}: shutdown expected exactly one fsync (the writer was dirtied by Restart) and no write, saw "
                     f"{[e[4][:60] for e in after]}")
            with open(path, "rb") as handle:
                new = handle.read()
            if len(new) != len(old):
                fail(f"dirty {size}: the file size changed")
            diff = [i for i in range(len(old)) if old[i] != new[i]]
            if len(diff) != 257 or len({i // size for i in diff}) != 1:
                fail(f"dirty {size}: expected 257 differing bytes inside one page, saw {len(diff)} in pages {sorted({i // size for i in diff})[:5]}")
            if any(new[i] != 9 for i in diff):
                fail(f"dirty {size}: a changed byte is not the dead state code")
            print(f"native_restart_strace {size}: dirty file (257 in progress): Restart = 3 pwrite64 of 512 bytes on a dup'd "
                  f"data descriptor in one page, then 1 fsync, before SERVING; shutdown 1 fsync (dirty writer), 0 other writes; "
                  f"257 state bytes changed, nothing else; 0 .sb. syscalls; {len(lines)} syscalls traced")
        print("native_restart_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
