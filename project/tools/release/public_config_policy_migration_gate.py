#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public configuration lifecycle and policy migration proof anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_CONFIG_POLICY_MIGRATION_GATE

CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "config_policy_migration_contract",
        "path": "project/src/storage/database/database_lifecycle.hpp",
        "tokens": (
            "CONFIG_POLICY_MIGRATION",
            "DatabaseArtifactVersionCompatibilityRequest",
            "migration_plan_required",
            "migration_plan_id",
            "missing_migration_plan_refused",
            "migration_required_without_plan_refused",
            "PolicySeedPackCatalogImage",
            "post_create_filesystem_authority",
            "policy_generation",
        ),
    },
    {
        "surface": "database_lifecycle_migration_behavior",
        "path": "project/src/storage/database/database_lifecycle.cpp",
        "tokens": (
            "ClassifyDatabaseArtifactVersionCompatibility",
            "ENGINE.DBLC_MIGRATION_PLAN_MISSING",
            "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
            "ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT",
            "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
            "DatabaseOpenCompatibilityClass::supported_migration",
            "ValidateCatalogMigrationEvidence",
            "BuildPolicyImageFromCatalogRows",
            "ValidatePolicySeedCatalogImage",
            "DEFAULT_POLICY_PACK_IMPORT",
            "post_create_filesystem_authority = false",
        ),
    },
    {
        "surface": "server_config_migration_behavior",
        "path": "project/src/server/config.cpp",
        "tokens": (
            "PUBLIC_DEFAULT_CONFIG_CHECK",
            "ClassifyServerConfigFormat",
            "CONFIG.MIGRATION_PLAN_MISSING",
            "CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN",
            "CONFIG.DOWNGRADE_REFUSED",
            "CONFIG.VERSION_NEWER_THAN_SUPPORTED",
            "supported_migration_requires_plan",
            "ValidateServerMemoryPolicy",
            "CONFIG.VALUE_INVALID_ENUM",
        ),
    },
    {
        "surface": "policy_pack_migration_readme",
        "path": "project/resources/policy-packs/default-local-password/README.md",
        "tokens": (
            "POLICY_PACK_MIGRATION",
            "create-time input only",
            "post-create policy mutation must not re-read this filesystem pack",
            "materialize",
            "durable catalog state",
        ),
    },
    {
        "surface": "policy_pack_manifest_versioning",
        "path": "project/resources/policy-packs/default-local-password/POLICY_PACK_MANIFEST.json",
        "tokens": (
            '"schema_version": 1',
            '"min_supported_schema_version": 1',
            '"max_supported_schema_version": 1',
            '"policy_pack_version": "1.0.0"',
            '"signature_status": "signature-ready-unsigned"',
            '"post_create_filesystem_authority": false',
            '"private_inputs_required": false',
            '"policies/default_policy_catalog.json"',
        ),
    },
    {
        "surface": "policy_pack_materialization_metadata",
        "path": "project/resources/policy-packs/default-local-password/catalog_materialization.json",
        "tokens": (
            '"policy_generation": 1',
            '"materialize_inside_create_transaction": true',
            '"requires_mga_catalog_commit": true',
            '"requires_authorization_for_post_create_mutation": true',
            '"catalog_identity_authority": "uuid"',
            '"DefaultPolicyRecord"',
        ),
    },
    {
        "surface": "policy_pack_default_policy_catalog",
        "path": "project/resources/policy-packs/default-local-password/policies/default_policy_catalog.json",
        "tokens": (
            '"default_policy_count": 58',
            '"catalog_authority": "durable_catalog_after_create"',
            '"post_create_filesystem_authority": false',
            '"policy_key": "policy.catalog.bootstrap"',
            '"policy_key": "transaction.admission"',
            '"policy_key": "cluster.boundary_fail_closed"',
        ),
    },
    {
        "surface": "policy_pack_manifest_gate",
        "path": "project/tools/release/public_policy_pack_manifest_gate.py",
        "tokens": (
            "validate_manifest",
            "validate_default_policy_catalog",
            "min_supported_schema_version",
            "max_supported_schema_version",
            "post_create_filesystem_authority",
            "validate_database_lifecycle_descriptor",
            "DEFAULT_POLICY_PACK_IMPORT",
        ),
    },
    {
        "surface": "policy_coverage_diff_gate",
        "path": "project/tools/release/public_policy_coverage_matrix.py",
        "tokens": (
            "POLICY_BLOCKED_AREAS",
            "post_create_filesystem_authority",
            "policy_generation",
            "unsupported_fail_closed",
            "public_policy_coverage_matrix=passed",
        ),
    },
    {
        "surface": "policy_pack_catalog_import_gate",
        "path": "project/tests/release/public_policy_pack_catalog_import_gate.cpp",
        "tokens": (
            "materialized_inside_create_transaction",
            "requires_mga_catalog_commit",
            "post_create_filesystem_authority",
            "default policy rows were not imported from the policy pack",
            "policy profile rows were not imported",
            "SB-POLICY-PACK-CONTENT-HASH-MISMATCH",
            "loaded_at_database_create",
        ),
    },
    {
        "surface": "policy_mutation_boundary_gate",
        "path": "project/tests/release/public_policy_mutation_boundary_gate.cpp",
        "tokens": (
            "policy_pack_root:/tmp/not-authority",
            "filesystem policy pack mutation unexpectedly succeeded",
            "policy mutation without POLICY_ADMIN unexpectedly succeeded",
            "policy mutation did not invalidate policy generation",
            "filesystem_policy_pack_authority",
            "false_after_create",
            "database_command_authority",
        ),
    },
    {
        "surface": "custom_policy_pack_gate",
        "path": "project/tests/release/public_custom_policy_pack_gate.cpp",
        "tokens": (
            "custom policy pack version was not materialized",
            "SB-POLICY-PACK-PROVENANCE-INVALID",
            "SB-POLICY-PACK-CONTENT-MANIFEST-INVALID",
            "SB-POLICY-PACK-DEFAULT-POLICIES-UNKNOWN",
            "SB-POLICY-PACK-PROFILES-UNKNOWN",
            "post-create filesystem policy pack mutation unexpectedly succeeded",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "PUBLIC_CONFIG_POLICY_MIGRATION_GATE",
            "public_config_policy_migration_gate",
            "public_default_config_check",
            "public_policy_pack_manifest_gate",
            "public_policy_coverage_matrix_gate",
            "public_policy_pack_catalog_import_gate",
            "public_policy_mutation_boundary_gate",
            "public_custom_policy_pack_gate",
            "public_upgrade_migration_gate",
            "PCR-GATE-147",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_config_policy_migration_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_text(repo_root: Path, relative_path: str) -> str:
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def load_json(repo_root: Path, relative_path: str) -> Any:
    path = repo_root / relative_path
    require(path.is_file(), f"json_missing:{relative_path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"json_invalid:{relative_path}:{exc}")


def validate_manifest_semantics(repo_root: Path) -> dict[str, Any]:
    manifest = load_json(
        repo_root,
        "project/resources/policy-packs/default-local-password/POLICY_PACK_MANIFEST.json",
    )
    require(isinstance(manifest, dict), "manifest_not_object")
    provenance = manifest.get("provenance")
    require(isinstance(provenance, dict), "manifest_provenance_not_object")
    expected = {
        "schema_version": 1,
        "min_supported_schema_version": 1,
        "max_supported_schema_version": 1,
        "policy_pack_id": "default-local-password",
        "policy_pack_version": "1.0.0",
        "content_hash_algorithm": "sha256",
        "signature_status": "signature-ready-unsigned",
        "create_time_only": True,
        "post_create_filesystem_authority": False,
    }
    for key, value in expected.items():
        require(manifest.get(key) == value, f"manifest_value_mismatch:{key}")
    require(provenance.get("source") == "public-project-tree", "manifest_provenance_source")
    require(provenance.get("private_inputs_required") is False, "manifest_private_inputs")
    require(
        provenance.get("external_provider_runtime_required") is False,
        "manifest_external_provider_runtime",
    )
    return {
        "policy_pack_id": manifest["policy_pack_id"],
        "policy_pack_version": manifest["policy_pack_version"],
        "schema_version": manifest["schema_version"],
        "schema_support": {
            "min": manifest["min_supported_schema_version"],
            "max": manifest["max_supported_schema_version"],
        },
        "signature_status": manifest["signature_status"],
        "private_inputs_required": provenance["private_inputs_required"],
        "post_create_filesystem_authority": manifest["post_create_filesystem_authority"],
    }


def validate_materialization_semantics(repo_root: Path) -> dict[str, Any]:
    metadata = load_json(
        repo_root,
        "project/resources/policy-packs/default-local-password/catalog_materialization.json",
    )
    require(isinstance(metadata, dict), "catalog_materialization_not_object")
    expected = {
        "policy_generation": 1,
        "materialize_inside_create_transaction": True,
        "post_create_filesystem_authority": False,
        "requires_mga_catalog_commit": True,
        "requires_authorization_for_post_create_mutation": True,
        "catalog_identity_authority": "uuid",
    }
    for key, value in expected.items():
        require(metadata.get(key) == value, f"catalog_materialization_value_mismatch:{key}")
    require(
        "DefaultPolicyRecord" in set(metadata.get("catalog_row_families", [])),
        "catalog_materialization_missing_default_policy_record",
    )
    return {
        "policy_generation": metadata["policy_generation"],
        "materialize_inside_create_transaction": metadata["materialize_inside_create_transaction"],
        "post_create_filesystem_authority": metadata["post_create_filesystem_authority"],
        "requires_mga_catalog_commit": metadata["requires_mga_catalog_commit"],
        "requires_authorization_for_post_create_mutation": metadata[
            "requires_authorization_for_post_create_mutation"
        ],
        "catalog_identity_authority": metadata["catalog_identity_authority"],
    }


def validate_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
    surface = check["surface"]
    path_text = check["path"]
    tokens = check["tokens"]
    require(isinstance(surface, str) and surface, "surface_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{surface}")
    require(isinstance(tokens, tuple) and tokens, f"tokens_invalid:{surface}")
    text = read_text(repo_root, path_text)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"token_invalid:{surface}")
        if token not in text:
            fail(f"token_missing:{surface}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "surface": surface,
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "surface",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    records = [validate_check(repo_root, check) for check in CHECKS]
    manifest = validate_manifest_semantics(repo_root)
    materialization = validate_materialization_semantics(repo_root)
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_CONFIG_POLICY_MIGRATION_GATE",
        "marker": "PUBLIC_CONFIG_POLICY_MIGRATION_GATE",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "policy_pack_manifest": manifest,
        "policy_pack_materialization": materialization,
        "authority": {
            "config_migration_authority": "database_lifecycle_and_server_config_classifiers",
            "policy_pack_source_after_create": "durable_catalog_only",
            "filesystem_pack_after_create_authority": False,
            "mga_catalog_commit_required": True,
            "admin_database_command_required": True,
            "release_gate_is_engine_authority": False,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_config_policy_migration_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
