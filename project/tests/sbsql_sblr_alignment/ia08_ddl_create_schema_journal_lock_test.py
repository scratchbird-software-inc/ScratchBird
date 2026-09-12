#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real-process native journal-lock checks; not a database or SBsql E2E test."""
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


class Owner:
    def __init__(self, executable, path):
        self.process = subprocess.Popen(
            [executable, "hold", str(path)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()

    def read_output(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put("<eof>")

    def expect(self, line):
        require(self.lines.get(timeout=10) == line, "expected " + line)

    def blocked(self):
        try:
            line = self.lines.get(timeout=0.25)
        except queue.Empty:
            require(self.process.poll() is None, "waiting contender exited")
            return
        raise RuntimeError("contender advanced while owner held lock: " + line)

    def release(self):
        self.process.stdin.write("release\n")
        self.process.stdin.flush()
        require(self.process.wait(timeout=10) == 0, "owner release failed")

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=10)

    def close(self):
        if self.process.poll() is None:
            self.kill()
        self.reader.join(timeout=10)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


def run(executable):
    root = Path(tempfile.mkdtemp(prefix="scratchbird_schema_journal_lock_"))
    print("retained_fixture=" + str(root), flush=True)
    payload = b"stable native journal lock sidecar\n"
    path = root / "record.lock"
    path.write_bytes(payload)
    alias = root / "record-hardlink.lock"
    os.link(path, alias)
    owners = []

    def start(target):
        owner = Owner(executable, target)
        owners.append(owner)
        owner.expect("ready")
        return owner

    def probe(target, expected):
        result = subprocess.run([executable, "probe", str(target)], timeout=10)
        require(result.returncode == expected, "native exclusion oracle failed: " + str(target))

    try:
        for target in (root, root / "absent-parent" / "record.lock"):
            result = subprocess.run([executable, "refuse", str(target)], timeout=10)
            require(result.returncode == 0, "invalid native lock path reported success")
        if os.name != "nt":
            symlink = root / "record-symlink.lock"
            symlink.symlink_to(path)
            result = subprocess.run([executable, "refuse", str(symlink)], timeout=10)
            require(result.returncode == 0, "symlink lock path accepted")

        # Independent kernel probes provide deterministic exclusion evidence;
        # ready/blocked/acquired checks also exercise the blocking waiter path.
        for terminate, target in ((False, path), (True, alias)):
            owner = start(path)
            owner.expect("acquired")
            probe(path, 2)
            probe(alias, 2)
            waiter = start(target)
            waiter.blocked()
            if terminate:
                owner.kill()
            else:
                owner.release()
            waiter.expect("acquired")
            probe(path, 2)
            waiter.release()
            probe(path, 0)
            probe(alias, 0)
            require(path.read_bytes() == payload and alias.read_bytes() == payload,
                    "locking changed sidecar bytes")
            require(os.path.samefile(path, alias), "sidecar inode replaced")

        empty = root / "new-empty.lock"
        owner = start(empty)
        owner.expect("acquired")
        probe(empty, 2)
        owner.release()
        probe(empty, 0)
        require(empty.read_bytes() == b"", "lock acquisition wrote dummy data")
        require(not list(root.glob("*.second")), "repeat acquisition changed lock target")
        print("PASS native exclusion, refusal, wait, release, crash handoff and stable bytes")
    finally:
        for owner in owners:
            owner.close()


if __name__ == "__main__":
    require(len(sys.argv) == 2, "expected native probe executable")
    run(str(Path(sys.argv[1]).resolve()))
