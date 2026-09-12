#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real physical-header offline ownership regression; not database/SBsql E2E."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from native_file_ownership_test import Child, require


def main(executable):
    root = Path(tempfile.mkdtemp(prefix="scratchbird_offline_filespace_owner_"))
    print("retained_fixture=" + str(root), flush=True)
    children = []
    failures = []

    def probe(mode, path, expected=0, argument="probe"):
        result = subprocess.run([executable, mode, str(path), str(argument)],
                                capture_output=True, text=True, timeout=15)
        if result.returncode != expected:
            failures.append((mode, str(path), "expected", expected, "actual",
                             result.returncode, result.stderr.strip()))

    try:
        path = root / "secondary.sbfs"
        probe("create", path)
        require(not failures, "real physical filespace fixture creation failed")
        original = path.read_bytes()
        require(len(original) == 24576, "incorrect physical capacity")
        alias = root / "secondary.hardlink"
        os.link(path, alias)
        paths = [path, alias]
        if os.name != "nt":
            symlink = root / "secondary.symlink"
            symlink.symlink_to(path)
            paths.append(symlink)

        for owner_kind in ("server", "read", "write"):
            owner = Child(executable, owner_kind, path, "hold")
            children.append(owner)
            owner.expect("held")
            for target in paths:
                probe("offline", target, 2)
                probe("online", target, 2)
                require(path.read_bytes() == original, "ownership refusal changed data")
            owner.release()
            for target in paths:
                probe("offline", target)
                probe("online", target)

        for target in paths:
            probe("internal", path, argument=target)
        owner = Child(executable, "server", path, "hold")
        children.append(owner)
        owner.expect("held")
        probe("offline", alias, 2)
        owner.process.kill()
        owner.process.wait(timeout=15)
        probe("offline", alias)
        require(path.read_bytes() == original, "read/crash handoff changed physical file")

        # Refusals must not become fabricated successful header responses.
        missing = root / "missing.sbfs"
        probe("offline", missing, 3)
        require(not missing.exists(), "missing-file probe created a data file")
        truncated = root / "truncated.sbfs"
        truncated.write_bytes(original[:32])
        probe("offline", truncated, 3)
        corrupt = root / "corrupt.sbfs"
        corrupt.write_bytes(bytes(len(original)))
        probe("offline", corrupt, 3)
        probe("offline", root, 3)
        for failure in failures:
            print("FAIL", failure, flush=True)
        require(not failures, str(len(failures)) + " offline filespace ownership failures")
        print("PASS real header reads, foreign-owner exclusion, aliases, release/crash handoff and invalid input")
    finally:
        for child in children:
            child.close()


if __name__ == "__main__":
    require(len(sys.argv) == 2, "expected filespace ownership probe executable")
    main(str(Path(sys.argv[1]).resolve()))
