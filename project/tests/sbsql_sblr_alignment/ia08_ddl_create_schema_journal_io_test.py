#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exact production I/O helper, real bytes/processes; no database/E2E claim."""
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


def main(executable):
    root = Path(tempfile.mkdtemp(prefix="scratchbird_schema_journal_io_"))
    print("retained_fixture=" + str(root), flush=True)
    children = []

    def run(mode, path, argument, expected=0):
        result = subprocess.run([executable, mode, str(path), str(argument)], timeout=10)
        require(result.returncode == expected, (mode, path, result.returncode, expected))

    def start(mode, path, argument, expected):
        child = subprocess.Popen([executable, mode, str(path), str(argument)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        children.append(child)
        lines = queue.Queue()
        reader = threading.Thread(target=lambda: lines.put(child.stdout.readline()), daemon=True)
        reader.start()
        require(lines.get(timeout=10).strip() == expected, "missing child readiness")
        reader.join(timeout=10)
        return child

    def exact(path, seed):
        require(path.read_bytes() == bytes([seed]) * 1032, "wrong or partial bytes: " + str(path))

    try:
        path = root / "record"
        run("create", path, 17)
        exact(path, 17)
        run("create", path, 23, 2)
        exact(path, 17)
        alias = root / "record-hardlink"
        os.link(path, alias)
        run("create", alias, 99, 2)
        exact(path, 17)
        exact(alias, 17)
        run("create", root / "absent" / "record", 17, 3)

        # Two ready independent creators, released together, must produce one
        # created result and one exists result; final bytes identify the winner.
        for ordinal in range(8):
            raced = root / ("race-" + str(ordinal))
            pair = [start("race", raced, seed, "ready") for seed in (31, 47)]
            for child in pair:
                child.stdin.write("go\n")
                child.stdin.flush()
            outcomes = [child.wait(timeout=10) for child in pair]
            require(sorted(outcomes) == [0, 2], "exclusive creation race failed")
            exact(raced, (31, 47)[outcomes.index(0)])

        prepared = root / "prepared"
        run("create", prepared, 61)
        run("publish", prepared, path)
        require(not prepared.exists(), "replacement did not consume prepared file")
        exact(path, 61)
        exact(alias, 17)  # Replacement, not in-place overwrite of the old inode.
        run("publish", root / "missing", path, 3)
        exact(path, 61)
        run("create", prepared, 71)
        directory = root / "destination-directory"
        directory.mkdir()
        run("publish", prepared, directory, 3)
        exact(prepared, 71)
        require(directory.is_dir(), "failed replacement changed directory target")
        absent_target = root / "first-publication"
        run("publish", prepared, absent_target)
        exact(absent_target, 71)

        if os.name == "nt":
            # A genuine native sharing conflict, not a mocked move result.
            run("create", prepared, 81)
            reader = start("hold-read", path, 0, "held")
            run("publish", prepared, path, 3)
            exact(prepared, 81)
            exact(path, 61)
            reader.communicate("release\n", timeout=10)
            require(reader.returncode == 0, "reader release failed")
            run("publish", prepared, path)
            exact(path, 81)
        else:
            failed = root / "limited-write"
            run("limited", failed, 91, 3)
            require(not failed.exists(), "failed write left a success-looking file")
            run("create", failed, 91)
            exact(failed, 91)
            link = root / "symlink"
            link.symlink_to(path)
            run("create", link, 101, 2)
            exact(path, 61)

        acknowledged = root / "acknowledged-before-kill"
        child = start("create-hold", acknowledged, 111, "created")
        child.kill()
        child.wait(timeout=10)
        exact(acknowledged, 111)
        run("create", acknowledged, 112, 2)
        exact(acknowledged, 111)
        print("PASS exclusive creation, eight races, replacement, refusal and acknowledged bytes")
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()


if __name__ == "__main__":
    require(len(sys.argv) == 2, "expected native I/O probe")
    main(str(Path(sys.argv[1]).resolve()))
