#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-024 memory readiness manifest generation.

SEARCH_KEY: CEIC_024_MEMORY_READINESS_MANIFEST_GATE_TEST
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if expect_success and result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected success: {' '.join(command)}")
    if not expect_success and result.returncode == 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected failure: {' '.join(command)}")
    return result


def load(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write(path: pathlib.Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def expect_failure_contains(command: list[str], text: str) -> None:
    result = run(command, expect_success=False)
    output = result.stdout + result.stderr
    if text not in output:
        raise AssertionError(f"expected failure output to contain {text!r}, got: {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_memory_readiness_manifest.py"
    committed_manifest = (
        repo_root
        / "docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure/artifacts/"
        / "CEIC-024_MEMORY_READINESS_MANIFEST.yaml"
    )

    committed_result = run(
        [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(committed_manifest)],
        expect_success=True,
    )

    with tempfile.TemporaryDirectory(prefix="ceic_024_memory_manifest_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        generated = temp_dir / "CEIC-024_MEMORY_READINESS_MANIFEST.yaml"
        generated_result = run(
            [
                sys.executable,
                str(tool),
                "--repo-root",
                str(repo_root),
                "--manifest",
                str(generated),
                "--write",
            ],
            expect_success=True,
        )

        stale = load(generated)
        stale["source_evidence_digest"] = "0" * 64
        stale_path = temp_dir / "stale.yaml"
        write(stale_path, stale)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(stale_path)],
            "stale manifest differs",
        )

        static_claim = load(generated)
        static_claim["readiness_state"]["production_claim"] = "production_ready"
        static_claim_path = temp_dir / "static_claim.yaml"
        write(static_claim_path, static_claim)
        expect_failure_contains(
            [
                sys.executable,
                str(tool),
                "--repo-root",
                str(repo_root),
                "--manifest",
                str(static_claim_path),
            ],
            "static claim without generated/integrated proof",
        )

        authority_drift = load(generated)
        authority_drift["authority_boundary"]["transaction_finality_authority"] = True
        authority_path = temp_dir / "authority_drift.yaml"
        write(authority_path, authority_drift)
        expect_failure_contains(
            [
                sys.executable,
                str(tool),
                "--repo-root",
                str(repo_root),
                "--manifest",
                str(authority_path),
            ],
            "transaction_finality_authority must be false",
        )

        integrated_overclaim = load(generated)
        for row in integrated_overclaim["readiness_state"]["pending_integrated_proof"]:
            if row["slice_id"] == "CEIC-093":
                row["status"] = "complete"
        integrated_path = temp_dir / "integrated_overclaim.yaml"
        write(integrated_path, integrated_overclaim)
        expect_failure_contains(
            [
                sys.executable,
                str(tool),
                "--repo-root",
                str(repo_root),
                "--manifest",
                str(integrated_path),
            ],
            "CEIC-093 must remain pending integrated proof",
        )

    print("ceic_024_memory_readiness_manifest_gate_test=pass")
    print(committed_result.stdout.strip())
    print(generated_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
