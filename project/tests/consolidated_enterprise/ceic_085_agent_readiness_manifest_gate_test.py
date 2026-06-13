#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-085 agent readiness manifest generation.

SEARCH_KEY: CEIC_085_AGENT_READINESS_MANIFEST_GATE_TEST
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


REQUIRED_COMPONENTS = {
    "agent_profiles",
    "signed_typed_policies",
    "policy_override_resolution",
    "lane_slo_cost_governance",
    "metric_quorum_source_attestation",
    "action_safety_rollout",
    "approval_break_glass",
    "evidence_key_privacy_tamper_chain",
    "dependency_lifecycle",
    "tenant_coordination",
    "package_plugin_actuator_provenance",
    "replay_compensation_quarantine",
    "noncluster_surface_closure",
    "memory_pressure_metric_integration",
    "index_optimizer_boundary",
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
    if data.get("search_key") != "CEIC_085_AGENT_READINESS_MANIFEST":
        raise AssertionError("CEIC-085 search key mismatch")
    if data.get("manifest_kind") != "agent_enterprise_readiness_manifest":
        raise AssertionError("manifest kind mismatch")
    if data.get("slice_ids") != [f"CEIC-{value:03d}" for value in range(70, 86)]:
        raise AssertionError("slice_ids must be CEIC-070..CEIC-085")
    components = {row.get("component_id") for row in data.get("components", [])}
    if components != REQUIRED_COMPONENTS:
        raise AssertionError(f"component set mismatch: {components}")

    required_proofs = {
        "agent_profiles",
        "policies",
        "lane_slo_proof",
        "metrics_proof",
        "action_approval_proof",
        "evidence_proof",
        "plugin_provenance_proof",
        "memory_coupling_proof",
        "index_optimizer_coupling_proof",
        "crash_security_proof",
        "support_bundle_readiness",
    }
    proofs = set(data.get("readiness_proofs", {}))
    if not required_proofs.issubset(proofs):
        raise AssertionError(f"missing readiness proof sections: {required_proofs - proofs}")

    readiness = data.get("readiness_state", {})
    for slice_id in [f"CEIC-{value:03d}" for value in range(90, 96)]:
        if not any(
            row.get("slice_id") == slice_id
            and row.get("status") in {"complete", "completed", "done", "closed", "complete_move_ready"}
            for row in readiness.get("integrated_release_proof", [])
        ):
            raise AssertionError(f"{slice_id} integrated proof must be complete")

    cluster = data.get("cluster_boundary", {})
    if cluster.get("cluster_agent_code_state") != "compile_time_stubbed_external_provider_only":
        raise AssertionError("cluster agents/code must remain compile-time stubbed")
    if cluster.get("local_cluster_readiness") != "fail_closed":
        raise AssertionError("local cluster readiness must fail closed")
    if cluster.get("external_cluster_provider_proof_present") is not False:
        raise AssertionError("current manifest must not claim external cluster provider proof")

    if data.get("readiness_proofs", {}).get("index_optimizer_coupling_proof", {}).get("recommendation_only") is not True:
        raise AssertionError("index/optimizer coupling must remain recommendation-only")
    if data.get("readiness_proofs", {}).get("support_bundle_readiness", {}).get("integrated_support_bundle_ready") is not True:
        raise AssertionError("support-bundle readiness must close CEIC-091 integrated proof")

    for component in data.get("components", []):
        if component.get("static_only_readiness") is not False:
            raise AssertionError(f"{component.get('component_id')} static-only readiness was admitted")
        if component.get("descriptor_only_readiness") is not False:
            raise AssertionError(f"{component.get('component_id')} descriptor-only readiness was admitted")
        if component.get("anchor_only_live_exposure") is not False:
            raise AssertionError(f"{component.get('component_id')} anchor-only live exposure was admitted")
        if component.get("sidecar_only_evidence") is not False:
            raise AssertionError(f"{component.get('component_id')} sidecar-only evidence was admitted")
        if component.get("cluster_claim_blocked") is not True:
            raise AssertionError(f"{component.get('component_id')} cluster claim was not blocked")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_agent_readiness_manifest.py"
    committed_manifest = (
        repo_root
        / "project/tests/release_evidence/consolidated_enterprise_public_evidence/artifacts/"
        / "CEIC-085_AGENT_READINESS_MANIFEST.yaml"
    )

    committed_result = run(
        [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(committed_manifest)],
        expect_success=True,
    )
    assert_shape(load(committed_manifest))

    with tempfile.TemporaryDirectory(prefix="ceic_085_agent_manifest_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        generated = temp_dir / "CEIC-085_AGENT_READINESS_MANIFEST.yaml"
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
        static_only["components"][0]["static_only_readiness"] = True
        static_path = temp_dir / "static_only.yaml"
        write(static_path, static_only)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(static_path)],
            "static-only readiness",
        )

        descriptor_only = load(generated)
        descriptor_only["components"][1]["descriptor_only_readiness"] = True
        descriptor_path = temp_dir / "descriptor_only.yaml"
        write(descriptor_path, descriptor_only)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(descriptor_path)],
            "descriptor-only readiness",
        )

        anchor_only = load(generated)
        anchor_only["components"][12]["anchor_only_live_exposure"] = True
        anchor_path = temp_dir / "anchor_only.yaml"
        write(anchor_path, anchor_only)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(anchor_path)],
            "anchor-only live exposure",
        )

        authority_drift = load(generated)
        authority_drift["authority_boundary"]["agent_action_authority"] = True
        authority_path = temp_dir / "authority_drift.yaml"
        write(authority_path, authority_drift)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(authority_path)],
            "agent_action_authority must be false",
        )

        cluster_production = load(generated)
        cluster_production["cluster_boundary"]["cluster_production_claim"] = "production_ready"
        cluster_path = temp_dir / "cluster_production.yaml"
        write(cluster_path, cluster_production)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(cluster_path)],
            "cluster production claim missing external-cluster-provider proof",
        )

        cluster_stub = load(generated)
        cluster_stub["cluster_boundary"]["cluster_stub_claimed_as_production"] = True
        cluster_stub_path = temp_dir / "cluster_stub.yaml"
        write(cluster_stub_path, cluster_stub)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(cluster_stub_path)],
            "cluster stubs must not be claimed as production",
        )

        fixture_path_claim = load(generated)
        fixture_path_claim["production_test_separation"]["fixture_test_only_production_paths"] = True
        fixture_path = temp_dir / "fixture_path.yaml"
        write(fixture_path, fixture_path_claim)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(fixture_path)],
            "fixture/test-only production path",
        )

        sidecar = load(generated)
        sidecar["components"][7]["sidecar_only_evidence"] = True
        sidecar_path = temp_dir / "sidecar.yaml"
        write(sidecar_path, sidecar)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(sidecar_path)],
            "sidecar-only evidence",
        )

        runtime_authority = load(generated)
        runtime_authority["readiness_state"]["generated_readiness_runtime_authority"] = True
        runtime_path = temp_dir / "runtime_authority.yaml"
        write(runtime_path, runtime_authority)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(runtime_path)],
            "generated readiness cannot be runtime authority",
        )

        integrated_overclaim = load(generated)
        for row in integrated_overclaim["readiness_state"]["integrated_release_proof"]:
            if row["slice_id"] == "CEIC-090":
                row["status"] = "pending"
        integrated_path = temp_dir / "integrated_overclaim.yaml"
        write(integrated_path, integrated_overclaim)
        expect_failure_contains(
            [sys.executable, str(tool), "--repo-root", str(repo_root), "--manifest", str(integrated_path)],
            "CEIC-090 integrated proof must be complete",
        )

    print("ceic_085_agent_readiness_manifest_gate_test=pass")
    print(committed_result.stdout.strip())
    print(generated_result.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
