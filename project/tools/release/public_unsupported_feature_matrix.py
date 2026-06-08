#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the public unsupported-feature behavior matrix."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_UNSUPPORTED_FEATURE_MATRIX

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

REQUIRED_GROUPS = {
    "agents",
    "cluster_boundary",
    "indexes",
    "security",
}

REFUSAL_CLASSES = {
    "unsupported",
    "external_provider_required",
    "compile_time_disabled",
    "policy_blocked",
}

PUBLIC_VISIBILITY = {
    "public_diagnostic",
    "hidden",
}

CSV_FIELDS = (
    "feature_id",
    "feature_group",
    "surface",
    "public_visibility",
    "refusal_class",
    "diagnostic_code",
    "message_key",
    "runtime_executable",
    "authority_claim",
    "source_path",
    "public_test_path",
)


@dataclass(frozen=True)
class UnsupportedFeatureRow:
    feature_id: str
    feature_group: str
    surface: str
    public_visibility: str
    refusal_class: str
    diagnostic_code: str
    message_key: str
    runtime_executable: bool
    authority_claim: bool
    source_path: str
    source_token: str
    source_no_overclaim_token: str
    public_test_path: str
    public_test_token: str
    public_test_no_overclaim_token: str


MATRIX_ROWS: tuple[UnsupportedFeatureRow, ...] = (
    UnsupportedFeatureRow(
        feature_id="cluster.no_cluster_provider_execution",
        feature_group="cluster_boundary",
        surface="local cluster provider execution when cluster support is off",
        public_visibility="public_diagnostic",
        refusal_class="external_provider_required",
        diagnostic_code="SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        message_key="engine.cluster.support_not_enabled",
        runtime_executable=False,
        authority_claim=False,
        source_path="src/cluster_provider/no_cluster_provider.cpp",
        source_token="kClusterSupportNotEnabledCode",
        source_no_overclaim_token="info.supports_execution = false;",
        public_test_path="tests/release/public_cluster_provider_boundary_cleanup_gate.cpp",
        public_test_token="no_cluster provider did not publish support-not-enabled diagnostic",
        public_test_no_overclaim_token="in-tree cluster provider accepted execution",
    ),
    UnsupportedFeatureRow(
        feature_id="cluster.compile_link_stub_provider_execution",
        feature_group="cluster_boundary",
        surface="compile-link stub cluster provider execution",
        public_visibility="public_diagnostic",
        refusal_class="compile_time_disabled",
        diagnostic_code="SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
        message_key="engine.cluster.stub_compile_link_only",
        runtime_executable=False,
        authority_claim=False,
        source_path="src/cluster_provider_stub/stub_cluster_provider.cpp",
        source_token="kClusterHandshakeStubCompileLinkOnlyCode",
        source_no_overclaim_token="info.compile_link_only = true;",
        public_test_path="tests/release/public_cluster_provider_boundary_cleanup_gate.cpp",
        public_test_token="compile-link stub did not publish compile-link-only diagnostic",
        public_test_no_overclaim_token="in-tree cluster provider accepted execution",
    ),
    UnsupportedFeatureRow(
        feature_id="agents.cluster_live_route",
        feature_group="agents",
        surface="agent production live cluster route or lease without external provider",
        public_visibility="public_diagnostic",
        refusal_class="external_provider_required",
        diagnostic_code="CLUSTER.EXTERNAL_PROVIDER_REQUIRED",
        message_key="agent.cluster.external_provider_required",
        runtime_executable=False,
        authority_claim=False,
        source_path="src/core/agents/agent_cluster_boundary.cpp",
        source_token="CLUSTER.EXTERNAL_PROVIDER_REQUIRED",
        source_no_overclaim_token="result->external_provider_required = true;",
        public_test_path="tests/release/public_agent_operator_explain_cluster_boundary_gate.cpp",
        public_test_token="external-provider-required diagnostic missing",
        public_test_no_overclaim_token="no_cluster provider accepted production live route",
    ),
    UnsupportedFeatureRow(
        feature_id="security.auth_provider_unsupported",
        feature_group="security",
        surface="unknown or non-login authentication provider family",
        public_visibility="public_diagnostic",
        refusal_class="unsupported",
        diagnostic_code="SECURITY.AUTH_PROVIDER_UNSUPPORTED",
        message_key="security.auth_provider.unsupported",
        runtime_executable=False,
        authority_claim=False,
        source_path="src/engine/internal_api/security/auth_provider_model.cpp",
        source_token="SECURITY.AUTH_PROVIDER_UNSUPPORTED",
        source_no_overclaim_token="provider_is_not_login_provider",
        public_test_path="tests/release/public_security_provider_contract_protected_material_gate.cpp",
        public_test_token="SECURITY.AUTH_PROVIDER_UNSUPPORTED",
        public_test_no_overclaim_token="provider without rotation capability should fail closed",
    ),
    UnsupportedFeatureRow(
        feature_id="indexes.temporary_work_runtime_missing",
        feature_group="indexes",
        surface="temporary work index build without a runtime state",
        public_visibility="public_diagnostic",
        refusal_class="unsupported",
        diagnostic_code="INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
        message_key="index.temporary_work.runtime_missing",
        runtime_executable=False,
        authority_claim=False,
        source_path="src/core/index/temporary_work_index_runtime.cpp",
        source_token="INDEX.TEMPORARY_WORK.RUNTIME_MISSING",
        source_no_overclaim_token="fail_closed=true",
        public_test_path="tools/release/public_diagnostic_matrix_generator.py",
        public_test_token='"diagnostic_id": "indexes.temporary_work.runtime_missing"',
        public_test_no_overclaim_token='"compatibility_status": "fail_closed_stable"',
    ),
    UnsupportedFeatureRow(
        feature_id="indexes.advanced_vector_policy_blocked",
        feature_group="indexes",
        surface="advanced vector policy-blocked index family",
        public_visibility="public_diagnostic",
        refusal_class="policy_blocked",
        diagnostic_code="INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
        message_key="index.policy_blocked.policy_not_accepted",
        runtime_executable=False,
        authority_claim=False,
        source_path="src/core/index/policy_blocked_index_admission.cpp",
        source_token="INDEX.POLICY_BLOCKED.POLICY_NOT_ACCEPTED",
        source_no_overclaim_token="policy_blocked.executable=false",
        public_test_path="tests/release/public_index_readiness_matrix_gate.cpp",
        public_test_token="policy_blocked_non_runtime",
        public_test_no_overclaim_token="non_runtime_not_admitted",
    ),
)


def fail(message: str) -> None:
    print(f"public_unsupported_feature_matrix=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def require_file(project_root: Path, relative: str) -> str:
    reject_private_reference(relative, "unsupported_feature_path")
    path = project_root / relative
    if not path.is_file():
        fail(f"missing_file:{relative}")
    return path.read_text(encoding="utf-8")


def require_token(project_root: Path, relative: str, token: str) -> dict[str, str]:
    reject_private_reference(token, f"unsupported_feature_token:{relative}")
    text = require_file(project_root, relative)
    if token not in text:
        fail(f"missing_token:{relative}:{token}")
    return {"path": relative, "sha256": sha256_text(text), "token": token}


def validate_row(project_root: Path, row: UnsupportedFeatureRow) -> dict[str, Any]:
    for field in CSV_FIELDS:
        value = getattr(row, field)
        reject_private_reference(str(value), f"row:{row.feature_id}:{field}")
        require(str(value) != "", f"empty_field:{row.feature_id}:{field}")
    require(row.feature_group in REQUIRED_GROUPS,
            f"unknown_group:{row.feature_id}:{row.feature_group}")
    require(row.public_visibility in PUBLIC_VISIBILITY,
            f"unknown_visibility:{row.feature_id}:{row.public_visibility}")
    require(row.refusal_class in REFUSAL_CLASSES,
            f"unknown_refusal_class:{row.feature_id}:{row.refusal_class}")
    require(not row.runtime_executable,
            f"runtime_execution_overclaim:{row.feature_id}")
    require(not row.authority_claim,
            f"authority_overclaim:{row.feature_id}")
    require("." in row.message_key,
            f"message_key_not_namespaced:{row.feature_id}:{row.message_key}")
    require(row.diagnostic_code != "SB_ENGINE_API_OK",
            f"ok_diagnostic_for_unsupported_feature:{row.feature_id}")
    if row.refusal_class == "external_provider_required":
        require("CLUSTER" in row.diagnostic_code,
                f"external_provider_without_cluster_diagnostic:{row.feature_id}")
    if row.refusal_class == "compile_time_disabled":
        require("STUB" in row.diagnostic_code or "COMPILE" in row.diagnostic_code,
                f"compile_time_disabled_without_compile_diagnostic:{row.feature_id}")
    if row.refusal_class == "policy_blocked":
        require("POLICY_BLOCKED" in row.diagnostic_code,
                f"policy_blocked_without_policy_diagnostic:{row.feature_id}")

    source = require_token(project_root, row.source_path, row.source_token)
    source_no_overclaim = require_token(
        project_root, row.source_path, row.source_no_overclaim_token)
    public_test = require_token(project_root, row.public_test_path,
                                row.public_test_token)
    public_test_no_overclaim = require_token(
        project_root, row.public_test_path, row.public_test_no_overclaim_token)
    return {
        **{field: getattr(row, field) for field in CSV_FIELDS},
        "runtime_executable": "false",
        "authority_claim": "false",
        "source_token": source["token"],
        "source_no_overclaim_token": source_no_overclaim["token"],
        "public_test_token": public_test["token"],
        "public_test_no_overclaim_token": public_test_no_overclaim["token"],
    }


def validate_rows(project_root: Path) -> list[dict[str, Any]]:
    seen_ids: set[str] = set()
    seen_groups: set[str] = set()
    seen_classes: set[str] = set()
    rows: list[dict[str, Any]] = []
    for row in MATRIX_ROWS:
        require(row.feature_id not in seen_ids,
                f"duplicate_feature_id:{row.feature_id}")
        seen_ids.add(row.feature_id)
        seen_groups.add(row.feature_group)
        seen_classes.add(row.refusal_class)
        rows.append(validate_row(project_root, row))

    missing_groups = sorted(REQUIRED_GROUPS - seen_groups)
    missing_classes = sorted(REFUSAL_CLASSES - seen_classes)
    require(not missing_groups, "missing_required_groups:" + ",".join(missing_groups))
    require(not missing_classes,
            "missing_required_refusal_classes:" + ",".join(missing_classes))
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(CSV_FIELDS))
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row[field] for field in CSV_FIELDS})
    return path.read_text(encoding="utf-8")


def build_evidence(project_root: Path, matrix_output: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")
    rows = validate_rows(project_root)
    matrix_text = write_csv(matrix_output, rows)
    group_counts: dict[str, int] = {}
    refusal_counts: dict[str, int] = {}
    for row in rows:
        group_counts[row["feature_group"]] = group_counts.get(row["feature_group"], 0) + 1
        refusal_counts[row["refusal_class"]] = refusal_counts.get(row["refusal_class"], 0) + 1
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-124",
        "marker": "PUBLIC_UNSUPPORTED_FEATURE_MATRIX",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "runtime_execution_overclaim_allowed": False,
            "authority_overclaim_allowed": False,
            "deterministic_refusal_required": True,
            "release_proof_is_evidence_only": True,
        },
        "matrix_path": matrix_output.name,
        "matrix_sha256": sha256_text(matrix_text),
        "row_count": len(rows),
        "group_counts": group_counts,
        "refusal_counts": refusal_counts,
        "rows": rows,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--matrix-output", type=Path, required=True)
    parser.add_argument("--evidence-output", type=Path, required=True)
    args = parser.parse_args(argv)

    project_root = args.project_root.resolve()
    matrix_output = args.matrix_output.resolve()
    evidence_output = args.evidence_output.resolve()
    evidence = build_evidence(project_root, matrix_output)
    evidence_output.parent.mkdir(parents=True, exist_ok=True)
    evidence_output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
    print(f"public_unsupported_feature_matrix_rows={evidence['row_count']}")
    print(f"public_unsupported_feature_matrix_sha256={evidence['matrix_sha256']}")
    print("public_unsupported_feature_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
