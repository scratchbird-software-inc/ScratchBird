#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Actual fresh-process replay of empty/NUL hash/B-tree index mutations."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    # Separate execs, not fork-only workers: no engine cache or session survives
    # a phase. Each process closes its node before the next process opens it.
    with tempfile.TemporaryDirectory(prefix="sbkeyrestart_") as root:
        for phase in ("prepare", "update", "delete", "absent"):
            result = subprocess.run(
                [str(args.probe), "--restart-phase", phase, root],
                capture_output=True, text=True, timeout=300, check=False,
            )
            print(result.stdout, end="")
            if result.returncode != 0:
                raise RuntimeError(f"{phase}: rc={result.returncode}: {result.stderr}")
            expected = f"restart_phase={phase} profiles=8"
            if expected not in result.stdout.splitlines():
                raise RuntimeError(f"{phase}: incomplete profile execution: {result.stdout!r}")
    print("secondary_index_empty_binary_restart=passed phases=4 profiles_per_phase=8")


if __name__ == "__main__":
    main()
