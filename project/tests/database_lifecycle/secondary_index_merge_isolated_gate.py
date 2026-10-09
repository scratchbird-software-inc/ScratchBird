#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Run the complete merge corpus with at most two independent node processes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile
import time

CASES = (
    "authoritative_merge", "horizon_resource", "refusal_diagnostics",
    "empty_synchronous", "nul_synchronous", "empty_deferred", "nul_deferred",
    "malformed_payloads",
)


def run_case(probe, name, deadline):
    # No database, engine session, memory manager or cache is shared between
    # cases. subprocess.run kills and reaps this owned process on timeout before
    # TemporaryDirectory removes any database/sidecar left by failure.
    with tempfile.TemporaryDirectory(prefix="sb_merge_case_") as root:
        env = dict(os.environ, TMPDIR=root, TMP=root, TEMP=root)
        started = time.monotonic()
        remaining = deadline - started
        if remaining <= 0:
            raise RuntimeError(f"{name}: complete-corpus deadline exhausted")
        result = subprocess.run([str(probe), "--case", name], env=env,
                                capture_output=True, text=True, timeout=min(90, remaining))
        expected = f"secondary_merge_case={name} passed"
        if result.returncode or result.stdout.splitlines() != [expected] or result.stderr:
            raise RuntimeError(f"{name}: rc={result.returncode} "
                               f"stdout={result.stdout!r} stderr={result.stderr!r}")
        return name, time.monotonic() - started


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    listing = subprocess.run([str(probe), "--list-cases"], capture_output=True,
                             text=True, timeout=10, check=True)
    if listing.stderr or tuple(listing.stdout.splitlines()) != CASES:
        raise RuntimeError("merge case inventory missing, duplicated or changed")
    # Bounded process parallelism, never threads inside a multi-database engine.
    # Every future is observed; failures cannot become a shortened passing run.
    outcomes, errors = [], []
    deadline = time.monotonic() + 100
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(run_case, probe, name, deadline) for name in CASES]
        for future in futures:
            try:
                outcomes.append(future.result())
            except Exception as error:
                errors.append(str(error))
    if errors:
        raise RuntimeError("\n".join(errors))
    if tuple(name for name, _ in outcomes) != CASES:
        raise RuntimeError("merge execution omitted a registered case")
    for name, elapsed in outcomes:
        print(f"secondary_merge_case={name} passed elapsed_seconds={elapsed:.3f}")
    print(f"secondary_index_merge_isolated=passed cases={len(outcomes)} maximum_processes=2")


if __name__ == "__main__":
    main()
