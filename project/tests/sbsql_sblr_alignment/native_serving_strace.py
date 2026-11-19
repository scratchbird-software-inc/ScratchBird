#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_SERVING_TEST (strace half; thin-slice S3b, plan 38).

Creates a native database at each of the five page sizes, runs one serve child
(native_serving_test serve <path>) under strace -f, and asserts:
  - the child serves (SERVING state=serving) and exits 0;
  - 0 syscalls name a ".sb." path (no lock file, no side file);
  - the directory afterwards is byte-identical (names, sizes, SHA-256);
  - 0 write-class syscalls (write, pwrite64, pwritev, ftruncate, fallocate,
    fsync, fdatasync) on any descriptor opened on the data file, or duplicated
    from one, BEFORE the SERVING line. S3d: the SERVING line now follows the
    transaction-store Restart, so this reads "0 writes and 0 syncs even after
    Restart" for a clean file (the dirty cases: native_restart_strace.py).
    (Was, S3b/S3c: nothing is written or synced before Restart, which did not
    exist yet.) After it (the shutdown) NO write-class syscall on a
    data descriptor either: Q283 = yes (owner 2026-11-19), the writer syncs at
    close only when something was written. (Was: one close-time fsync allowed.)
  - S3c (owner Q239 = 1, 2026-11-19): the serve child runs in its "read" variant,
    which after SERVING obtains read-only served devices on eight threads and
    reads them (native_serving_test.cpp ServedReads). Between the SERVING line
    and the READ line 0 openat/open of the database path appears: the reads use
    duplicates of the lease's retained descriptor, never a path open. (The one
    later open of the path is the test's own re-ownership probe after the close.)
  - the database loop thread existed (a clone and the sb-db-loop name).
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


def snapshot(directory):
    out = {}
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if os.path.isfile(path):
            with open(path, "rb") as handle:
                data = handle.read()
            out[name] = (len(data), hashlib.sha256(data).hexdigest())
        else:
            out[name] = "dir"
    return out


def fail(message):
    print("FAIL " + message)
    sys.exit(1)


def data_descriptors(lines, path):
    """Every descriptor number returned by an open of the data file, with the
    line range during which it is live (numbers are reused after close)."""
    live = {}
    spans = []
    open_re = re.compile(r'\[pid\s+\d+\]\s*|^\d+\s+')
    for index, line in enumerate(lines):
        match = re.search(r'open(?:at)?\(.*"' + re.escape(path) + r'".*\)\s*=\s*(\d+)', line)
        if match:
            live[match.group(1)] = index
        # A duplicate of a live data descriptor (the served-lease dups) is one too.
        match = re.search(r'\bfcntl\((\d+),\s*F_DUPFD(?:_CLOEXEC)?,\s*\d+\)\s*=\s*(\d+)', line) or \
            re.search(r'\bdup(?:2|3)?\((\d+)(?:,\s*\d+)*\)\s*=\s*(\d+)', line)
        if match and match.group(1) in live:
            live[match.group(2)] = index
        match = re.search(r'\bclose\((\d+)\)\s*=\s*0', line)
        if match and match.group(1) in live:
            spans.append((match.group(1), live.pop(match.group(1)), index))
    for fd, start in live.items():
        spans.append((fd, start, len(lines)))
    return spans


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_serving_")
    try:
        for size in SIZES:
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "serving.sdb")
            run = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if run.returncode != 0:
                fail(f"create {size}: {run.stderr[-2000:]}")
            if os.listdir(directory) != ["serving.sdb"]:
                fail(f"native create made more than its file: {os.listdir(directory)}")
            before = snapshot(directory)
            log = os.path.join(work, f"serve-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.test_bin, "serve", path, "read"]
            served = subprocess.run(command, capture_output=True, text=True, timeout=900)
            if served.returncode != 0:
                fail(f"serve {size}: exit {served.returncode}: {served.stderr[-2000:]} {served.stdout[-500:]}")
            first = served.stdout.strip().splitlines()[0] if served.stdout.strip() else ""
            if not first.startswith("SERVING state=serving "):
                fail(f"serve {size}: {first!r}")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = handle.readlines()
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"serve {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if snapshot(directory) != before:
                fail(f"serve {size}: the directory changed")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"serve {size}: no open of the data file was traced")
            # The SERVING line marks the end of the open, which since S3d includes
            # the Restart of the transaction store (a clean file marks nothing
            # dead, so it writes nothing); everything after it is the shutdown.
            # Nothing may write, or sync, a data descriptor before it; after it,
            # the close path may fsync (reported), never write.
            serving_at = next((i for i, line in enumerate(lines) if re.search(r'write\(1, "SERVING', line)), None)
            if serving_at is None:
                fail(f"serve {size}: the SERVING output line was not traced")
            before_writes, closing_syncs, closing_writes = [], [], []
            read_at = next((i for i, line in enumerate(lines) if re.search(r'write\(1, "READ ', line)), None)
            if read_at is None or read_at < serving_at:
                fail(f"serve {size}: the READ line (the served reads after custody) was not traced after SERVING")
            path_opens_after_serving = [
                line.strip() for line in lines[serving_at:read_at]
                if re.search(r'\bopen(?:at)?2?\(.*"' + re.escape(path) + r'"', line)]
            if path_opens_after_serving:
                fail(f"serve {size}: {len(path_opens_after_serving)} opens of the database path after serving, first: {path_opens_after_serving[0]}")
            if not re.search(r'write\(1, "READ pages=\d+ threads=8 .* path_opens=0 file_opens=0 sidecar_opens=0 writable_refused=SB-STORAGE-DISK-SERVED-NATIVE-WRITE-REFUSED',
                             lines[read_at]):
                fail(f"serve {size}: the READ line is not the expected one: {lines[read_at].strip()[:300]}")
            for index, line in enumerate(lines):
                match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
                if not match:
                    continue
                call, fd = match.group(1), match.group(2)
                if not any(f == fd and start < index < end for f, start, end in spans):
                    continue
                if index < serving_at:
                    before_writes.append(line.strip())
                elif call in ("fsync", "fdatasync"):
                    closing_syncs.append(line.strip())
                else:
                    closing_writes.append(line.strip())
            if closing_syncs:
                fail(f"serve {size}: {len(closing_syncs)} fsync/fdatasync on a data descriptor after serving (Q283: an unwritten open syncs nothing), first: {closing_syncs[0]}")
            if before_writes:
                fail(f"serve {size}: {len(before_writes)} write-class syscalls on a data descriptor before serving, first: {before_writes[0]}")
            if closing_writes:
                fail(f"serve {size}: {len(closing_writes)} writes on a data descriptor during shutdown, first: {closing_writes[0]}")
            if not any("sb-db-loop" in line for line in lines) and "sb-db-loop" not in served.stdout:
                fail(f"serve {size}: the sb-db-loop thread was not observed")
            print(f"native_serving_strace {size}: serving; 0 .sb. syscalls; 0 write-class syscalls on {len(spans)} data "
                  f"descriptor(s) before serving; shutdown: {len(closing_syncs)} fsync(s) on the data descriptor, 0 writes; "
                  f"served reads: 0 opens of the database path between SERVING and the reads; directory unchanged; {len(lines)} syscalls traced")
        # Negative control: the same detector must see the create's writes, so a
        # pass above is not a detector that sees nothing.
        control_dir = os.path.join(work, "control")
        os.mkdir(control_dir)
        control = os.path.join(control_dir, "control.sdb")
        control_log = os.path.join(work, "control.strace")
        subprocess.run(["strace", "-f", "-qq", "-s", "256", "-o", control_log, args.test_bin, "create", control, "16384"],
                       capture_output=True, text=True, timeout=600)
        with open(control_log, encoding="utf-8", errors="replace") as handle:
            control_lines = handle.readlines()
        control_spans = data_descriptors(control_lines, control)
        control_writes = [line for index, line in enumerate(control_lines)
                          if (m := re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line))
                          and any(f == m.group(2) and start < index < end for f, start, end in control_spans)]
        if not control_writes:
            fail("the detector saw no write-class syscall during a create: it cannot be trusted")
        print(f"negative control: {len(control_writes)} write-class syscalls on the data file during create")
        print("native_serving_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
