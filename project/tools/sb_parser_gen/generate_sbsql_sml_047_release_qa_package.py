#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the SML-047 SBsql release QA evidence package manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.sml_047.release_qa_package.v1"
DEFAULT_OUTPUT = (
    "project/tests/sbsql_parser_worker/generated/release_qa/"
    "SML_047_RELEASE_QA_PACKAGE_MANIFEST.json"
)


EVIDENCE_GROUPS: list[dict[str, Any]] = [
    {
        "group_id": "SML-041-046",
        "claim": "full_product_regression_surface_variation_and_hardening",
        "sml_slices": ["SML-041", "SML-042", "SML-043", "SML-044", "SML-045", "SML-046"],
        "ctest_labels": [
            "sbsql_product_qa_manifest",
            "sbsql_hardening_oracle",
            "sbsql_no_spin_no_wal_no_direct_db_gate",
            "sbsql_fuzz_malicious_input_gate",
        ],
        "paths": [
            "project/tests/sbsql_parser_worker/generated/product_regression/sml_041_046/SML_041_046_PRODUCT_QA_MANIFEST.json",
            "project/tests/sbsql_parser_worker/generated/product_regression/sml_041_046/sbsql_sml_041_046_product_qa_manifest_gate.py",
            "project/tests/sbsql_parser_worker/generated/hardening/sml_041_046/SML_044_046_HARDENING_ORACLE.json",
            "project/tests/sbsql_parser_worker/generated/hardening/sml_041_046/sbsql_sml_044_046_hardening_oracle_gate.py",
            "project/tests/sbsql_parser_worker/generated/hardening/MALICIOUS_INPUT_FIXTURES.csv",
            "project/tests/sbsql_parser_worker/generated/hardening/sbsql_fuzz_malicious_input_gate.cpp",
            "project/tests/sbsql_parser_worker/generated/hardening/sbsql_no_spin_no_wal_no_direct_db_gate.cpp",
        ],
    },
    {
        "group_id": "SML-049-052",
        "claim": "filespace_relocation_primary_migration_and_recovery",
        "sml_slices": ["SML-049", "SML-050", "SML-051", "SML-052"],
        "ctest_labels": ["sbsql_filespace_relocation"],
        "paths": [
            "project/tests/sbsql_parser_worker/generated/filespace_relocation/SML_049_052_FILESPACE_RELOCATION_MANIFEST.csv",
            "project/tests/sbsql_parser_worker/generated/filespace_relocation/SML_049_052_FILESPACE_RELOCATION_ORACLE.json",
            "project/tests/sbsql_parser_worker/generated/filespace_relocation/sbsql_sml_049_052_filespace_relocation_gate.py",
        ],
    },
    {
        "group_id": "SML-054-057-062",
        "claim": "multimodel_capability_and_sql_nosql_join_closure",
        "sml_slices": ["SML-054", "SML-055", "SML-056", "SML-057", "SML-062"],
        "ctest_labels": ["sbsql_multimodel_capability"],
        "paths": [
            "project/tests/sbsql_parser_worker/generated/multimodel_capability/SML_054_057_062_MULTIMODEL_CAPABILITY_MANIFEST.json",
            "project/tests/sbsql_parser_worker/generated/multimodel_capability/SML_054_057_062_MULTIMODEL_CAPABILITY_ORACLE.jsonl",
            "project/tests/sbsql_parser_worker/generated/multimodel_capability/sbsql_sml_054_057_062_multimodel_capability_gate.py",
        ],
    },
    {
        "group_id": "SML-059-061",
        "claim": "encrypted_database_full_route_key_failure_and_recovery",
        "sml_slices": ["SML-059", "SML-060", "SML-061"],
        "ctest_labels": ["sbsql_encrypted_database_matrix"],
        "paths": [
            "project/tests/sbsql_parser_worker/generated/encrypted_database/ENCRYPTED_DATABASE_REGRESSION_MATRIX.json",
            "project/tests/sbsql_parser_worker/generated/encrypted_database/sbsql_sml_059_061_encrypted_database_gate.py",
        ],
    },
    {
        "group_id": "SML-064-066",
        "claim": "native_compile_jit_aot_equivalence_and_release_metrics",
        "sml_slices": ["SML-064", "SML-065", "SML-066"],
        "ctest_labels": ["sbsql_native_compile_jit_aot"],
        "paths": [
            "project/tests/sbsql_parser_worker/generated/native_compile/NATIVE_COMPILE_JIT_AOT_MATRIX.json",
            "project/tests/sbsql_parser_worker/generated/native_compile/sbsql_sml_064_066_native_compile_gate.py",
        ],
    },
]


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def file_record(repo_root: Path, rel_path: str) -> dict[str, Any]:
    path = repo_root / rel_path
    data = path.read_bytes()
    return {
        "path": rel_path,
        "bytes": len(data),
        "sha256": sha256_bytes(data),
    }


def build_manifest(repo_root: Path) -> dict[str, Any]:
    groups: list[dict[str, Any]] = []
    for group in EVIDENCE_GROUPS:
        records = [file_record(repo_root, rel_path) for rel_path in group["paths"]]
        group_payload = {
            "group_id": group["group_id"],
            "claim": group["claim"],
            "sml_slices": group["sml_slices"],
            "ctest_labels": group["ctest_labels"],
            "evidence_files": records,
            "runtime_dependency": "repo_tracked_project_evidence_only",
            "private_workplan_dependency": False,
            "reference_tree_dependency": False,
            "parser_executes_sql": False,
            "parser_owns_finality": False,
            "storage_finality_authority": "scratchbird_engine_mga_transaction_inventory",
            "group_sha256": "",
        }
        group_payload["group_sha256"] = sha256_text(canonical_json({
            key: value for key, value in group_payload.items() if key != "group_sha256"
        }))
        groups.append(group_payload)

    manifest: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "gate_id": "SML-GATE-047",
        "status": "implemented_proven",
        "description": "Self-contained SBsql release QA regression evidence package.",
        "source": "deterministic_generator",
        "network_dependency": False,
        "generated_from_tracked_project_files": True,
        "private_workplan_dependency": False,
        "reference_tree_dependency": False,
        "evidence_groups": groups,
        "manifest_sha256": "",
    }
    manifest["manifest_sha256"] = sha256_text(canonical_json({
        key: value for key, value in manifest.items() if key != "manifest_sha256"
    }))
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path, default=Path(DEFAULT_OUTPUT))
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    output = args.output if args.output.is_absolute() else repo_root / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = build_manifest(repo_root)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
