#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public cluster catalog readiness proof wiring for PCR-098."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


# PUBLIC_CLUSTER_CATALOG_MANIFEST_CHECK

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)

REQUIRED_TESTS = {
    "public_cluster_catalog_manifest_gate": "PCR-GATE-093",
    "public_cluster_schema_projection_gate": "PCR-GATE-094",
    "public_cluster_catalog_codec_resolver_gate": "PCR-GATE-095",
    "public_cluster_descriptor_manifest_gate": "PCR-GATE-096",
    "public_cluster_provider_boundary_cleanup_gate": "PCR-GATE-097",
    "public_cluster_boundary_cleanup_audit_gate": "PCR-GATE-097",
    "public_cluster_catalog_readiness_suite": "PCR-GATE-098",
    "public_cluster_catalog_manifest_check_gate": "PCR-GATE-098",
}

REQUIRED_SOURCE_ANCHORS = {
    "tests/release/public_cluster_catalog_readiness_suite.cpp": (
        "PUBLIC_CLUSTER_CATALOG_READINESS_SUITE",
        "ExecuteClusterOperation",
        "ValidateClusterCatalogManifestSet",
        "ValidateClusterCacheProjectionManifestSet",
        "ValidateClusterCatalogRecordSet",
        "ValidateClusterDescriptorManifestSet",
        "ValidateClusterMetricDescriptorManifestSet",
    ),
    "src/core/catalog/cluster_catalog_manifest.hpp": (
        "CLUSTER_CATALOG_MANIFESTS",
        "ClusterCatalogManifestSet",
    ),
    "src/core/catalog/cluster_schema_gating.hpp": (
        "CLUSTER_SCHEMA_ROOT_GATING",
        "CLUSTER_CACHE_PROJECTIONS",
    ),
    "src/core/catalog/cluster_catalog_record_codec.hpp": (
        "CLUSTER_RECORD_CODEC",
        "ClusterCatalogRecordSet",
    ),
    "src/core/catalog/cluster_descriptor_manifest.hpp": (
        "CLUSTER_DECISION_ROUTE_TOPOLOGY_DESCRIPTORS",
        "transaction_inventory_remains_finality_authority",
    ),
    "src/core/metrics/cluster_metric_descriptors.hpp": (
        "CLUSTER_METRIC_DESCRIPTORS",
        "contract_ready_unwired",
    ),
    "src/cluster_provider/cluster_provider.hpp": (
        "CLUSTER_PROVIDER_BOUNDARY",
        "supports_execution",
    ),
    "tools/release/public_cluster_boundary_cleanup_audit.py": (
        "CLUSTER_CALL_CLEANUP_AUDIT",
        "compile_link_only_non_mutating",
    ),
}


def fail(message: str) -> None:
    print(f"public_cluster_catalog_manifest_check=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_file(path: Path, project_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{path.relative_to(project_root).as_posix()}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def test_block(cmake_text: str, test_name: str) -> str:
    add_test = re.search(
        r"add_test\s*\(\s*NAME\s+" + re.escape(test_name) + r"\b",
        cmake_text,
        re.MULTILINE,
    )
    if not add_test:
        fail(f"release_cmake_test_missing:{test_name}")
    label_start = cmake_text.find(
        f"set_tests_properties({test_name} PROPERTIES", add_test.end()
    )
    if label_start < 0:
        fail(f"release_cmake_labels_missing:{test_name}")
    next_add_test = cmake_text.find("add_test(", label_start + 1)
    next_add_executable = cmake_text.find("add_executable(", label_start + 1)
    end_candidates = [
        candidate for candidate in (next_add_test, next_add_executable)
        if candidate >= 0
    ]
    end = min(end_candidates) if end_candidates else len(cmake_text)
    return cmake_text[label_start:end]


def check_release_cmake(project_root: Path) -> list[dict[str, Any]]:
    cmake_path = project_root / "tests" / "release" / "CMakeLists.txt"
    cmake_text = require_file(cmake_path, project_root)
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in cmake_text:
            fail(f"private_reference_in_release_cmake:{fragment}")

    records: list[dict[str, Any]] = []
    for test_name, gate in REQUIRED_TESTS.items():
        block = test_block(cmake_text, test_name)
        require_contains(block, "public_release_correctness",
                         f"{test_name}:labels")
        require_contains(block, gate, f"{test_name}:gate_label")
        records.append(
            {
                "test": test_name,
                "gate": gate,
                "labels": "public_release_correctness",
            }
        )

    prereq_match = re.search(
        r"set\s*\(\s*PUBLIC_RELEASE_CORRECTNESS_BUILD_TARGETS(?P<body>.*?)\n\)",
        cmake_text,
        re.DOTALL,
    )
    if not prereq_match:
        fail("public_release_build_target_set_missing")
    prereq_body = prereq_match.group("body")
    for target in (
        "public_cluster_catalog_manifest_gate",
        "public_cluster_schema_projection_gate",
        "public_cluster_catalog_codec_resolver_gate",
        "public_cluster_descriptor_manifest_gate",
        "public_cluster_provider_boundary_cleanup_gate",
        "public_cluster_catalog_readiness_suite",
    ):
        require_contains(prereq_body, target, "public_release_build_targets")

    return records


def check_source_anchors(project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for relative, anchors in REQUIRED_SOURCE_ANCHORS.items():
        reject_private_reference(relative, "source_anchor_path")
        text = require_file(project_root / relative, project_root)
        for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
            if fragment in text:
                fail(f"private_reference_in_source:{relative}:{fragment}")
        for anchor in anchors:
            require_contains(text, anchor, f"{relative}:anchor")
        records.append(
            {
                "file": relative,
                "anchor_count": len(anchors),
                "sha256": sha256_text(text),
            }
        )
    return records


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output = args.output.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    if project_root.name != "project":
        fail("project_root_must_be_project_directory")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-098",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "cluster_production_execution": "external_provider_only",
            "local_cluster_provider_modes": [
                "no_cluster_fail_closed",
                "compile_link_stub_non_mutating",
            ],
            "catalog_authority": "uuid_identity_and_external_provider_bound",
            "mga_transaction_inventory_authority_preserved": True,
        },
        "release_cmake": check_release_cmake(project_root),
        "source_anchors": check_source_anchors(project_root),
    }
    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    evidence["evidence_sha256"] = sha256_text(encoded)
    return evidence


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
    print(
        "public_cluster_catalog_manifest_check_output="
        f"{output.relative_to(args.build_root.resolve()).as_posix()}"
    )
    print(
        "public_cluster_catalog_manifest_check_sha256="
        f"{evidence['evidence_sha256']}"
    )
    print("public_cluster_catalog_manifest_check=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
