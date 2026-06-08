#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public-tree migration coverage for private proof dependencies."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any


FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)

PROOF_MIGRATION_ROWS: tuple[dict[str, Any], ...] = (
    {
        "proof_id": "PPM-001",
        "source_class": "consolidated_enterprise_release_export",
        "public_target": "project/tests/release#public_project_export_gate",
        "required_gates": ["PCR-GATE-001", "PCR-GATE-005"],
    },
    {
        "proof_id": "PPM-002",
        "source_class": "consolidated_enterprise_source_hygiene",
        "public_target": "project/tests/release#public_private_reference_scan_gate",
        "required_gates": ["PCR-GATE-002"],
    },
    {
        "proof_id": "PPM-003",
        "source_class": "memory_readiness",
        "public_target": "project/tests/release#public_memory_regression_suite",
        "required_gates": ["PCR-GATE-010", "PCR-GATE-011", "PCR-GATE-012"],
    },
    {
        "proof_id": "PPM-004",
        "source_class": "index_readiness",
        "public_target": "project/tools/release#public_index_readiness_matrix",
        "required_gates": ["PCR-GATE-050"],
    },
    {
        "proof_id": "PPM-005",
        "source_class": "optimizer_readiness",
        "public_target": "project/tools/release#public_optimizer_implementation_matrix",
        "required_gates": ["PCR-GATE-060"],
    },
    {
        "proof_id": "PPM-006",
        "source_class": "agent_readiness",
        "public_target": "project/tools/release#public_agent_readiness_matrix",
        "required_gates": ["PCR-GATE-080"],
    },
    {
        "proof_id": "PPM-007",
        "source_class": "memory_commercial_hardening",
        "public_target": "project/tests/release#public_memory_regression_suite",
        "required_gates": ["PCR-GATE-010", "PCR-GATE-011", "PCR-GATE-012"],
    },
    {
        "proof_id": "PPM-008",
        "source_class": "optimizer_enterprise_runtime",
        "public_target": "project/tests/release#public_optimizer_regression_suite",
        "required_gates": ["PCR-GATE-060", "PCR-GATE-061", "PCR-GATE-062"],
    },
    {
        "proof_id": "PPM-009",
        "source_class": "agent_enterprise_runtime",
        "public_target": "project/tests/release#public_agent_regression_suite",
        "required_gates": ["PCR-GATE-080", "PCR-GATE-081", "PCR-GATE-082"],
    },
    {
        "proof_id": "PPM-010",
        "source_class": "index_runtime_correctness",
        "public_target": "project/tests/release#public_index_regression_suite",
        "required_gates": ["PCR-GATE-050", "PCR-GATE-052", "PCR-GATE-053"],
    },
    {
        "proof_id": "PPM-011",
        "source_class": "repair_history_audit",
        "public_target": "project/tests/release#public_repair_history_regression_suite",
        "required_gates": ["PCR-GATE-007"],
    },
    {
        "proof_id": "PPM-012",
        "source_class": "code_review_closeout",
        "public_target": "project/tests/release#public_code_review_closeout_gate",
        "required_gates": ["PCR-GATE-900"],
    },
    {
        "proof_id": "PPM-013",
        "source_class": "mga_audit_transaction_location",
        "public_target": "project/tests/release#public_mga_audit_transaction_location_suite",
        "required_gates": ["PCR-GATE-008"],
    },
    {
        "proof_id": "PPM-014",
        "source_class": "uuid_identity_resolution",
        "public_target": "project/tests/release#public_uuid_identity_resolution_suite",
        "required_gates": ["PCR-GATE-006"],
    },
    {
        "proof_id": "PPM-015",
        "source_class": "mga_sweep_archive_backup_forward",
        "public_target": "project/tests/release#public_mga_sweep_archive_backup_forward_suite",
        "required_gates": ["PCR-GATE-071", "PCR-GATE-072", "PCR-GATE-073", "PCR-GATE-074", "PCR-GATE-075"],
    },
    {
        "proof_id": "PPM-016",
        "source_class": "cluster_catalog_readiness",
        "public_target": "project/tests/release#public_cluster_catalog_readiness_suite",
        "required_gates": ["PCR-GATE-093", "PCR-GATE-094", "PCR-GATE-095", "PCR-GATE-096", "PCR-GATE-097", "PCR-GATE-098"],
    },
    {
        "proof_id": "PPM-017",
        "source_class": "cluster_catalog_enterprise_readiness",
        "public_target": "project/tests/release#public_cluster_catalog_enterprise_readiness_suite",
        "required_gates": ["PCR-GATE-099", "PCR-GATE-100", "PCR-GATE-101", "PCR-GATE-102", "PCR-GATE-103", "PCR-GATE-104", "PCR-GATE-105", "PCR-GATE-106", "PCR-GATE-107", "PCR-GATE-108"],
    },
    {
        "proof_id": "PPM-018",
        "source_class": "public_release_hardening",
        "public_target": "project/tests/release#public_release_hardening_suite",
        "required_gates": ["PCR-GATE-109", "PCR-GATE-110", "PCR-GATE-111", "PCR-GATE-112", "PCR-GATE-113", "PCR-GATE-114", "PCR-GATE-115", "PCR-GATE-116", "PCR-GATE-117", "PCR-GATE-118"],
    },
    {
        "proof_id": "PPM-019",
        "source_class": "public_consumption_and_artifact_trust",
        "public_target": "project/tests/release#public_consumption_trust_suite",
        "required_gates": ["PCR-GATE-119", "PCR-GATE-120", "PCR-GATE-121", "PCR-GATE-122", "PCR-GATE-123", "PCR-GATE-124", "PCR-GATE-125", "PCR-GATE-126", "PCR-GATE-127"],
    },
    {
        "proof_id": "PPM-020",
        "source_class": "policy_bootstrap_seed_pack",
        "public_target": "project/tests/release#public_policy_bootstrap_seed_pack_suite",
        "required_gates": ["PCR-GATE-128", "PCR-GATE-129", "PCR-GATE-130", "PCR-GATE-131", "PCR-GATE-132"],
    },
    {
        "proof_id": "PPM-021",
        "source_class": "enterprise_release_readiness",
        "public_target": "project/tests/release#public_enterprise_readiness_suite",
        "required_gates": ["PCR-GATE-133", "PCR-GATE-134", "PCR-GATE-135", "PCR-GATE-136", "PCR-GATE-137", "PCR-GATE-138", "PCR-GATE-139", "PCR-GATE-140", "PCR-GATE-141", "PCR-GATE-142", "PCR-GATE-143", "PCR-GATE-144", "PCR-GATE-145", "PCR-GATE-146", "PCR-GATE-147", "PCR-GATE-148"],
    },
    {
        "proof_id": "PPM-022",
        "source_class": "public_governance_legal_ip",
        "public_target": "project/tests/release#public_governance_legal_ip_suite",
        "required_gates": ["PCR-GATE-149", "PCR-GATE-150", "PCR-GATE-151", "PCR-GATE-152", "PCR-GATE-153", "PCR-GATE-154", "PCR-GATE-155", "PCR-GATE-156", "PCR-GATE-157", "PCR-GATE-158", "PCR-GATE-159"],
    },
)


def fail(message: str) -> None:
    print(f"public_proof_migration_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_project_target(target: str, proof_id: str) -> None:
    reject_private_reference(target, f"public_target:{proof_id}")
    if "#" not in target:
        fail(f"public_target_missing_anchor:{proof_id}:{target}")
    path, _anchor = target.split("#", 1)
    if not (path.startswith("project/tests/") or path.startswith("project/tools/")):
        fail(f"public_target_not_project_local:{proof_id}:{target}")


def validate_manifest() -> dict[str, Any]:
    proof_ids: set[str] = set()
    gate_ids: set[str] = set()
    target_paths: set[str] = set()
    gate_re = re.compile(r"^PCR-GATE-(?:[0-9]{3}|[0-9]{3}[A-Z])$")
    for row in PROOF_MIGRATION_ROWS:
        proof_id = row["proof_id"]
        if proof_id in proof_ids:
            fail(f"duplicate_proof_id:{proof_id}")
        proof_ids.add(proof_id)
        reject_private_reference(proof_id, "proof_id")
        reject_private_reference(row["source_class"], f"source_class:{proof_id}")
        require_project_target(row["public_target"], proof_id)
        target_paths.add(row["public_target"].split("#", 1)[0])
        required_gates = row["required_gates"]
        if not required_gates:
            fail(f"required_gate_missing:{proof_id}")
        for gate in required_gates:
            reject_private_reference(gate, f"required_gate:{proof_id}")
            if not gate_re.match(gate):
                fail(f"required_gate_malformed:{proof_id}:{gate}")
            gate_ids.add(gate)
    return {
        "proof_count": len(PROOF_MIGRATION_ROWS),
        "required_gate_count": len(gate_ids),
        "public_target_path_count": len(target_paths),
        "status": "project_local_public_targets_mapped",
    }


def scan_public_release_cmake(project_root: Path) -> dict[str, Any]:
    cmake_path = project_root / "tests" / "release" / "CMakeLists.txt"
    if not cmake_path.exists():
        fail("release_cmake_missing")
    text = cmake_path.read_text(encoding="utf-8")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in text:
            fail(f"private_reference_in_release_cmake:{fragment}")
    for required in (
        "public_project_export_gate",
        "public_private_reference_scan_gate",
        "public_proof_migration_gate",
    ):
        if required not in text:
            fail(f"release_cmake_required_gate_missing:{required}")
    labels = sorted(set(re.findall(r"PCR-GATE-[0-9A-Z]+", text)))
    return {
        "release_cmake": "project/tests/release/CMakeLists.txt",
        "registered_gate_label_count": len(labels),
        "status": "private_inputs_absent",
    }


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output = args.output.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")
    return {
        "schema_version": 1,
        "gate": "PCR-GATE-003",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "manifest_status_is_mapping_not_behavioral_pass": True,
        },
        "migration_manifest": validate_manifest(),
        "release_cmake": scan_public_release_cmake(project_root),
        "rows": PROOF_MIGRATION_ROWS,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"public_proof_migration_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print("public_proof_migration_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
