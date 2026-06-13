#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-062 optimizer readiness manifest generation.

SEARCH_KEY: CEIC_062_OPTIMIZER_READINESS_MANIFEST_GATE_TEST
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


REQUIRED_COMPONENTS = {
    "live_routes",
    "persisted_benchmark_evidence",
    "correctness_oracles",
    "crash_reopen_persistence",
    "metrics_feedback",
    "transformation_memo_coverage",
    "workload_regression_budgets",
    "driver_visible_explain",
    "reference_comparison_artifacts",
    "memory_feedback",
    "index_readiness_coupling",
    "llvm_memory_accounting",
}


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


def assert_shape(data: dict) -> None:
    if data.get("search_key") != "CEIC_062_OPTIMIZER_READINESS_MANIFEST":
        raise AssertionError("CEIC-062 search key mismatch")
    if data.get("manifest_kind") != "optimizer_readiness_manifest":
        raise AssertionError("manifest kind mismatch")
    if data.get("slice_ids") != [f"CEIC-{value:03d}" for value in range(50, 63)]:
        raise AssertionError("slice_ids must be CEIC-050..CEIC-062")
    components = {row.get("component_id") for row in data.get("components", [])}
    if components != REQUIRED_COMPONENTS:
        raise AssertionError(f"component set mismatch: {components}")
    readiness = data.get("readiness_state", {})
    for slice_id in ("CEIC-059", "CEIC-060", "CEIC-061"):
        if not any(
            row.get("slice_id") == slice_id and row.get("status") == "complete"
            for row in readiness.get("required_completed_coupling", [])
        ):
            raise AssertionError(f"{slice_id} coupling is missing")
    cluster = data.get("cluster_boundary", {})
    if cluster.get("local_cluster_optimizer_readiness") != "fail_closed":
        raise AssertionError("local cluster optimizer readiness must fail closed")
    if cluster.get("external_cluster_provider_only") is not True:
        raise AssertionError("cluster optimization must remain external-provider-only")
    for row in data.get("components", []):
        if row.get("static_only_proof") is not False:
            raise AssertionError(f"{row.get('component_id')} static-only proof was admitted")
        if row.get("placeholder_runtime_evidence") is not False:
            raise AssertionError(f"{row.get('component_id')} placeholder evidence was admitted")
        if row.get("local_cluster_evidence_present") is not False:
            raise AssertionError(f"{row.get('component_id')} local cluster evidence was admitted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_optimizer_readiness_manifest.py"
    committed_manifest = (
        repo_root
        / "project/tests/release_evidence/consolidated_enterprise_public_evidence/artifacts/"
        / "CEIC-062_OPTIMIZER_READINESS_MANIFEST.yaml"
    )

    committed_result = run(
        [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(committed_manifest)],
        expect_success=True,
    )
    assert_shape(load(committed_manifest))

    with tempfile.TemporaryDirectory(prefix="ceic_062_optimizer_manifest_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        generated = temp_dir / "CEIC-062_OPTIMIZER_READINESS_MANIFEST.yaml"
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
        assert_shape(load(generated))

        stale = load(generated)
        stale["source_evidence_digest"] = "0" * 64
        stale_path = temp_dir / "stale.yaml"
        write(stale_path, stale)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(stale_path)],
            "stale manifest differs",
        )

        missing_component = load(generated)
        missing_component["components"] = missing_component["components"][:-1]
        missing_path = temp_dir / "missing_component.yaml"
        write(missing_path, missing_component)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(missing_path)],
            "required component missing",
        )

        static_only = load(generated)
        static_only["components"][0]["static_only_proof"] = True
        static_path = temp_dir / "static_only.yaml"
        write(static_path, static_only)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(static_path)],
            "static-only proof",
        )

        placeholder_contract = load(generated)
        placeholder_contract["components"][0]["result_contract_hash"] = "result-contract-v1"
        placeholder_path = temp_dir / "placeholder_contract.yaml"
        write(placeholder_path, placeholder_contract)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(placeholder_path)],
            "placeholder result contract",
        )

        placeholder_epoch = load(generated)
        placeholder_epoch["components"][0]["catalog_epoch"] = 1
        placeholder_epoch_path = temp_dir / "placeholder_epoch.yaml"
        write(placeholder_epoch_path, placeholder_epoch)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(placeholder_epoch_path)],
            "placeholder catalog_epoch",
        )

        default_stats = load(generated)
        default_stats["components"][0]["synthetic_statistics"] = True
        default_stats["components"][1]["local_default_statistics"] = True
        default_stats["components"][2]["policy_default_statistics"] = True
        default_stats_path = temp_dir / "default_stats.yaml"
        write(default_stats_path, default_stats)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(default_stats_path)],
            "synthetic_statistics must be false",
        )

        missing_coupling = load(generated)
        for row in missing_coupling["readiness_state"]["required_completed_coupling"]:
            if row["slice_id"] == "CEIC-059":
                row["status"] = "pending"
        missing_coupling_path = temp_dir / "missing_coupling.yaml"
        write(missing_coupling_path, missing_coupling)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(missing_coupling_path)],
            "missing required CEIC-059 coupling",
        )

        authority_drift = load(generated)
        authority_drift["components"][8]["reference_authority_claimed"] = True
        authority_drift["components"][8]["benchmark_dominance_claimed"] = True
        authority_path = temp_dir / "authority_drift.yaml"
        write(authority_path, authority_drift)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(authority_path)],
            "authority drift",
        )

        local_cluster = load(generated)
        local_cluster["cluster_boundary"]["local_cluster_evidence_present"] = True
        local_cluster_path = temp_dir / "local_cluster.yaml"
        write(local_cluster_path, local_cluster)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(local_cluster_path)],
            "local cluster evidence is forbidden",
        )

        external_overclaim = load(generated)
        external_overclaim["components"][0]["external_cluster_overclaim"] = True
        external_path = temp_dir / "external_overclaim.yaml"
        write(external_path, external_overclaim)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(external_path)],
            "external-cluster overclaim",
        )

    print("ceic_062_optimizer_readiness_manifest_gate_test=pass")
    print(committed_result.stdout.strip())
    print(generated_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
