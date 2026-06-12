#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-042 index readiness manifest drift closure.

SEARCH_KEY: CEIC_042_INDEX_READINESS_MANIFEST_DRIFT_GATE_TEST
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
INTEGRATED_PENDING = tuple(f"CEIC-{value:03d}" for value in range(90, 96))


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


def assert_ceic_042_shape(data: dict) -> None:
    successor = {
        row.get("slice_id"): row.get("status")
        for row in data.get("index_successor_slice_statuses", [])
        if isinstance(row, dict)
    }
    if successor.get("CEIC-042") not in COMPLETE_STATUSES:
        raise AssertionError("CEIC-042 must be complete in the drift gate manifest")
    if "CEIC-042" not in data.get("slice_ids", []):
        raise AssertionError("manifest slice_ids must include CEIC-042")
    if "CEIC-042" in data.get("future_index_slices_pending", []):
        raise AssertionError("CEIC-042 must not remain in future_index_slices_pending")

    integrated = {
        row.get("slice_id"): row.get("status")
        for row in data.get("integrated_boundary_slice_statuses", [])
        if isinstance(row, dict)
    }
    for slice_id in INTEGRATED_PENDING:
        if integrated.get(slice_id) != "pending":
            raise AssertionError(f"{slice_id} must remain pending integrated proof")

    drift = data.get("readiness_drift_gate_evidence", {})
    if drift.get("state") != "complete":
        raise AssertionError("CEIC-042 drift gate evidence state must be complete")
    if drift.get("static_or_smoke_only") is not False:
        raise AssertionError("CEIC-042 drift gate must reject static/smoke-only proof")
    if drift.get("missing_family_manifest_count") != 0:
        raise AssertionError("CEIC-042 drift gate must reject missing family manifests")
    if drift.get("stale_manifest_allowed") is not False:
        raise AssertionError("CEIC-042 drift gate must reject stale manifests")
    for key in ("reference_dominance_claimed", "all_index_readiness_claimed", "enterprise_readiness_claimed"):
        if drift.get(key) is not False:
            raise AssertionError(f"CEIC-042 must not claim {key}")

    families = data.get("families", [])
    if not families:
        raise AssertionError("families are required")
    for row in families:
        if row.get("enum_name") in {"reference_emulated", "policy_blocked"}:
            if row.get("runtime_availability", {}).get("runtime_available_static_input") is not False:
                raise AssertionError(f"{row.get('family_id')} must remain non-runtime")
            if row.get("readiness_drift_gate_status", {}).get("status") != "blocked":
                raise AssertionError(f"{row.get('family_id')} drift status must remain blocked")
            continue
        if row.get("benchmark_evidence_status", {}).get("status") != "complete":
            raise AssertionError(f"{row.get('family_id')} CEIC-042 benchmark evidence is incomplete")
        if row.get("readiness_drift_gate_status", {}).get("status") != "complete":
            raise AssertionError(f"{row.get('family_id')} CEIC-042 drift status is incomplete")
        if row.get("enterprise_ready") is not False:
            raise AssertionError(f"{row.get('family_id')} must not claim enterprise readiness")
        classification = row.get("family_classification_summary", {})
        for key in ("all_index_readiness_claimed", "reference_dominance_claimed", "enterprise_readiness_claimed"):
            if classification.get(key) is not False:
                raise AssertionError(f"{row.get('family_id')} must not claim {key}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_index_readiness_manifest.py"
    committed_manifest = (
        repo_root
        / "docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure/artifacts/"
        / "CEIC-030_INDEX_READINESS_MANIFEST.yaml"
    )

    committed_result = run(
        [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(committed_manifest)],
        expect_success=True,
    )
    assert_ceic_042_shape(load(committed_manifest))

    with tempfile.TemporaryDirectory(prefix="ceic_042_index_manifest_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        generated = temp_dir / "CEIC-030_INDEX_READINESS_MANIFEST.yaml"
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
        assert_ceic_042_shape(load(generated))

        stale = load(generated)
        stale["source_evidence_digest"] = "0" * 64
        stale_path = temp_dir / "stale.yaml"
        write(stale_path, stale)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(stale_path)],
            "stale manifest differs",
        )

        missing_family = load(generated)
        missing_family["families"] = missing_family["families"][:-1]
        missing_path = temp_dir / "missing_family.yaml"
        write(missing_path, missing_family)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(missing_path)],
            "family_count must match families length",
        )

        static_smoke = load(generated)
        static_smoke["readiness_drift_gate_evidence"]["static_or_smoke_only"] = True
        static_path = temp_dir / "static_smoke.yaml"
        write(static_path, static_smoke)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(static_path)],
            "static/smoke-only",
        )

        route_drift = load(generated)
        for row in route_drift["families"]:
            if row["enum_name"] == "hash":
                row["route_capability_summary"]["supports_ordered_range"] = True
                break
        route_path = temp_dir / "route_drift.yaml"
        write(route_path, route_drift)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(route_path)],
            "hash must not support ordered ranges",
        )

        metric_drift = load(generated)
        for row in metric_drift["families"]:
            if row.get("persistent") is True:
                row["metric_producer_status"]["status"] = "pending"
                break
        metric_path = temp_dir / "metric_drift.yaml"
        write(metric_path, metric_drift)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(metric_path)],
            "CEIC-042 requires metric_producer_status complete",
        )

        crash_drift = load(generated)
        for row in crash_drift["families"]:
            if row.get("persistent") is True:
                row["crash_evidence_status"]["status"] = "pending"
                break
        crash_path = temp_dir / "crash_drift.yaml"
        write(crash_path, crash_drift)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(crash_path)],
            "CEIC-042 requires crash_evidence_status complete",
        )

        integrated_overclaim = load(generated)
        for row in integrated_overclaim["integrated_boundary_slice_statuses"]:
            if row["slice_id"] == "CEIC-093":
                row["status"] = "complete"
                break
        integrated_path = temp_dir / "integrated_overclaim.yaml"
        write(integrated_path, integrated_overclaim)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(integrated_path)],
            "CEIC-093 must remain pending integrated proof",
        )

    print("ceic_042_index_readiness_manifest_drift_gate_test=pass")
    print(committed_result.stdout.strip())
    print(generated_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
