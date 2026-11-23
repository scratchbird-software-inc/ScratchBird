#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_RECEIPT_STRACE (thin-slice S4f; owner Q298: the native statement-context receipt is built with S4's session
and transaction).

The served child `native_receipt serve <path> trace` run under strace -f at the given page size. The child marks regions on stderr
(`write(2, "MARK x\\n")`):
  approve     one approval through the real handoff (connection, session and attachment ledger records, then the transaction's begin)
  prepare     the test's own read of the data file's bytes (before the receipt region)
  receipt     TWO receipts for that transaction and one refused acquire (a stale generation): READ ONLY, ONLY the engine
  verify      the test's own read of the data file's bytes again (it asserts the two images are identical)
  disconnect  the session's end through the loop (the transaction's rollback, then the ended ledger record)
  end         the close
Asserted, with the pid-aware attribution shared by every strace test (native_serving_strace.data_descriptors over
strace_attribution.py):
  - the child reports RECEIPT_OK; 0 syscalls name a ".sb." path; the directory holds only the data file;
  - EVERY write-class syscall on a descriptor of the data file is a 512-byte pwrite64 or an fsync / fdatasync;
  - the RECEIPT region contains NO write-class syscall and NO fsync on the data file (acquiring a receipt, and refusing one,
    never writes: the counted writes are the approval's and the disconnect's only); the clean open writes nothing;
  - NO region after the startup opens the data file BY PATH except the test's own prepare and verify reads: the approval, the
    receipts, the disconnect and the close open the path 0 times (served reads go through the owner's descriptor);
  - the approval region does write (a negative control inside the run: the detector sees the loop's own writes);
  - no test password appears in the trace, the output or the file.
A second negative control shows the detector sees a create's writes.
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

PASSWORDS = ("S4a-Bootstrap-Pw-7f3a91c2d8",)
OTHER_WRITES = r'(write|writev|pwritev2?|pwrite|ftruncate|fallocate|sync_file_range)'


def forms(text):
    raw = text.encode()
    return (text, "".join("\\x%02x" % b for b in raw[:8]), raw.hex(), raw[:12].hex())


def field(text, key):
    match = re.search(r'\b' + key + r'=([0-9a-f]+)', text)
    return match.group(1) if match else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test-bin", required=True)      # native_receipt (serve)
    parser.add_argument("--material-bin", required=True)  # native_login_material (create)
    parser.add_argument("--size", type=int, choices=SIZES)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    work = tempfile.mkdtemp(prefix="sb_native_receipt_strace_")
    try:
        for size in ((args.size,) if args.size else SIZES):
            directory = os.path.join(work, f"ps{size}")
            os.mkdir(directory)
            path = os.path.join(directory, "receipt.sdb")
            created = subprocess.run([args.material_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0 or not created.stdout.startswith("CREATED "):
                fail(f"create {size}: {created.stdout[-500:]} {created.stderr[-2000:]}")
            before = snapshot(directory)
            log = os.path.join(work, f"trace-{size}.strace")
            command = ["strace", "-f", "-qq", "-s", "64", "-o", log, args.test_bin, "serve", path, "trace",
                       field(created.stdout, "database"), field(created.stdout, "bootstrap"), field(created.stdout, "service")]
            ran = subprocess.run(command, capture_output=True, text=True, timeout=1500)
            if ran.returncode != 0 or "RECEIPT_OK" not in ran.stdout:
                fail(f"{size}: exit {ran.returncode}: {ran.stdout[-1500:]} {ran.stderr[-2000:]}")
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = merge_split_lines(handle.readlines())
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"{size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            if os.listdir(directory) != ["receipt.sdb"]:
                fail(f"{size}: the directory holds more than the data file: {os.listdir(directory)}")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"{size}: no open of the data file was traced")
            region = "startup"
            counts = {"startup": [0, 0]}
            opens = {}
            for index, line in enumerate(lines):
                m = re.search(r'write\(2, "MARK (\S+)\\n"', line)
                if m:
                    region = m.group(1)
                    counts.setdefault(region, [0, 0])
                    continue
                if re.search(r'\bopen(?:at2?)?\(', line) and "receipt.sdb" in line:
                    opens.setdefault(region, []).append(line.strip())
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
            for quiet in ("approve", "receipt", "disconnect", "end"):
                if opens.get(quiet):
                    fail(f"{size}: the engine opened the data file by path during {quiet}: {opens[quiet][0]}")
            for needed in ("approve", "prepare", "receipt", "verify", "disconnect", "end"):
                if needed not in counts:
                    fail(f"{size}: region {needed} was not traced")
            if counts["startup"] != [0, 0]:
                fail(f"{size}: {counts['startup'][0]} writes / {counts['startup'][1]} syncs on the data file during the open")
            if counts["approve"][0] < 4 or counts["approve"][1] < 4:
                fail(f"{size}: the approval region shows {counts['approve']}: the detector does not see the loop's writes")
            if counts["receipt"] != [0, 0]:
                fail(f"{size}: acquiring receipts wrote to the data file: {counts['receipt'][0]} pwrite64 / {counts['receipt'][1]} fsync")
            if counts["disconnect"][0] < 2:
                fail(f"{size}: the disconnect region shows {counts['disconnect']}: the detector does not see the loop's writes")
            if counts["end"][0] != 0 or counts["end"][1] > 1:
                fail(f"{size}: {counts['end'][0]} writes / {counts['end'][1]} syncs on the data file during the close")
            if snapshot(directory).keys() != before.keys():
                fail(f"{size}: the directory's files changed")
            with open(path, "rb") as handle:
                image_after = handle.read()
            text = "".join(lines) + ran.stdout + ran.stderr
            for password in PASSWORDS:
                for form in forms(password):
                    if form in text:
                        fail(f"{size}: a test password appears in the trace or the output as {form!r}")
                if password.encode() in image_after:
                    fail(f"{size}: a test password appears in the database file")
            print(f"native_receipt_strace {size}: RECEIPT_OK; 0 .sb. syscalls; every write-class syscall on the data file is a 512-byte pwrite64 "
                  f"or an fsync; the receipt region has {counts['receipt'][0]} writes / {counts['receipt'][1]} fsyncs; {len(lines)} syscalls traced")
            for name in ("startup", "approve", "prepare", "receipt", "verify", "disconnect", "end"):
                print(f"  {name:11s} pwrite64={counts[name][0]} fsync={counts[name][1]} path-opens={len(opens.get(name, []))}")
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
        print("native_receipt_strace: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
