#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Default policy catalog conformance gates for DBLC-000E/DBLC-000F.

The current public project setup treats the checked-in default policy pack as
the create-time authority. Older public_input_snapshot/public_contract_snapshot
policy packet parsing is intentionally not used here; those snapshots now cover
broader public surfaces and are not machine-readable default-policy registries.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


EXPECTED_POLICY_COUNT = 58
REQUIRED_POLICY_KEYS = {
    "admin.management_command_authorization",
    "backup.archive_restore_snapshot_shadow",
    "cache.checkpoint_preload_flush",
    "capability.feature_gate",
    "cluster.boundary_fail_closed",
    "concurrency.lock_wait_deadlock",
    "configuration.override_reload",
    "configuration.source_precedence",
    "database.bootstrap.tx1",
    "database.create.failure_cleanup",
    "database.first_open.tx2_activation",
    "database.identity",
    "diagnostics.message_vector",
    "event.queue_notification",
    "evidence.retention",
    "executable.side_effect",
    "ipc.frame_auth_backpressure",
    "job.scheduler",
    "lifecycle.maintenance_restricted",
    "lifecycle.ownership_stale_owner",
    "lifecycle.recovery_dirty_open",
    "lifecycle.shutdown_force",
    "lifecycle.shutdown_graceful_drain",
    "listener.bind_tls_pool",
    "observability.metrics_log",
    "parser.package_admission",
    "policy.catalog.bootstrap",
    "reference.emulation_profile",
    "replication.cdc_changefeed_boundary",
    "resource.seed_i18n",
    "resource.signature_provenance",
    "schema.bootstrap.roots",
    "security.audit",
    "security.authentication_provider",
    "security.authorization_default",
    "security.authority_selection",
    "security.bootstrap_password",
    "security.encryption_key_admission",
    "security.principal_role_group_seed",
    "security.protected_material",
    "security.redaction",
    "security.user_home_schema",
    "sequence.generator_cache",
    "server.route_listener_startup",
    "session.disconnect_timeout",
    "storage.allocation_freespace_pagemap",
    "storage.filespace_lifecycle",
    "storage.filespace_profile",
    "support.bundle",
    "temp.spill_workspace",
    "transaction.admission",
    "transaction.commit_durability",
    "transaction.default_isolation_snapshot",
    "transaction.mga_gc_retention",
    "transaction.rollback_savepoint_limbo",
    "udr.extension_trust_resource",
    "upgrade.migration_refusal",
    "workload.resource_quota",
}
REQUIRED_OVERRIDE_CLASSES = {
    "cluster_only",
    "create_database_only",
    "no_override",
    "policy_defined",
    "security_admin",
    "sysarch",
}
ROW_DIAGNOSTICS = {
    "POLICY.FAMILY_MISSING",
    "POLICY.PROFILE_INVALID",
    "POLICY.DEFAULT_PROPERTY_MISSING",
    "POLICY.GENERATION_STALE",
    "POLICY.OVERRIDE_FORBIDDEN",
    "POLICY.CLUSTER_SCOPE_FORBIDDEN",
    "POLICY.FAIL_CLOSED_BOUNDARY",
    "POLICY.AUDIT_REQUIRED",
}
REQUIRED_AUTHORITY_INVARIANTS = {
    "policy_catalog_is_authority",
    "mga_visibility_required",
    "wal_not_authority",
    "parser_not_authority",
    "reference_not_authority",
    "uuid_order_not_finality",
}
FORBIDDEN_AUTHORITY_TEXT = (
    "wal_finality=true",
    "cache_finality=true",
    "checkpoint_finality=true",
    "parser_auth_authority=true",
    "reference_sql_exec_inside_engine=true",
    "uuid_order_is_finality=true",
)


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # pragma: no cover - CTest reports exact exception.
        fail(f"{path} does not parse as JSON: {exc}")


def load_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except Exception as exc:  # pragma: no cover - CTest reports exact exception.
        fail(f"{path} does not parse as CSV: {exc}")


def load_paths(repo_root: Path) -> dict[str, Path]:
    pack_root = repo_root / "project/resources/policy-packs/default-local-password"
    return {
        "pack_root": pack_root,
        "resource_policy_catalog": pack_root / "policies/default_policy_catalog.json",
        "policy_pack_manifest": pack_root / "POLICY_PACK_MANIFEST.json",
        "policy_materialization": pack_root / "catalog_materialization.json",
        "policy_pack_readme": pack_root / "README.md",
        "policy_audit": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_DEFAULT_POLICY_AUDIT.csv",
        "registry_audit": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_DEFAULT_POLICY_REGISTRY_AUDIT.csv",
        "lifecycle_header": repo_root / "project/src/storage/database/database_lifecycle.hpp",
        "lifecycle_source": repo_root / "project/src/storage/database/database_lifecycle.cpp",
        "manifest_gate": repo_root / "project/tools/release/public_policy_pack_manifest_gate.py",
        "migration_gate": repo_root / "project/tools/release/public_config_policy_migration_gate.py",
        "catalog_import_gate": repo_root / "project/tests/release/public_policy_pack_catalog_import_gate.cpp",
        "custom_pack_gate": repo_root / "project/tests/release/public_custom_policy_pack_gate.cpp",
    }


def assert_not_ignored(repo_root: Path, paths: list[Path]) -> None:
    for path in paths:
        rel = path.relative_to(repo_root)
        result = subprocess.run(
            ["git", "check-ignore", "-q", str(rel)],
            cwd=repo_root,
            check=False,
        )
        if result.returncode == 0:
            fail(f"{rel} is ignored by git")
        if result.returncode not in (0, 1):
            fail(f"git check-ignore failed for {rel} with rc={result.returncode}")


def require_text(path: Path, tokens: tuple[str, ...]) -> None:
    text = path.read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            fail(f"{path.relative_to(path.parents[4]) if len(path.parents) > 4 else path} missing token {token}")


def load_resource_policy_catalog(path: Path) -> dict[str, dict[str, Any]]:
    text = path.read_text(encoding="utf-8")
    if any(token in text for token in FORBIDDEN_AUTHORITY_TEXT):
        fail("default policy resource contains forbidden authority text")
    doc = load_json(path)
    expected_header = {
        "schema_version": 1,
        "policy_generation": 1,
        "identity_authority": "uuid",
        "catalog_authority": "durable_catalog_after_create",
        "create_time_only": True,
        "post_create_filesystem_authority": False,
        "default_policy_count": EXPECTED_POLICY_COUNT,
    }
    for key, expected in expected_header.items():
        if doc.get(key) != expected:
            fail(f"default policy resource {key} mismatch")

    rows = doc.get("policies")
    if not isinstance(rows, list) or len(rows) != EXPECTED_POLICY_COUNT:
        fail("default policy resource policy count mismatch")

    by_key: dict[str, dict[str, Any]] = {}
    for expected_ordinal, row in enumerate(rows, start=1):
        if not isinstance(row, dict):
            fail("default policy resource row is not an object")
        key = row.get("policy_key")
        if not isinstance(key, str) or key in by_key:
            fail(f"default policy resource duplicate or invalid key {key}")
        if key not in REQUIRED_POLICY_KEYS:
            fail(f"{key} is not a required default policy key")
        if row.get("ordinal") != expected_ordinal:
            fail(f"{key} default policy resource ordinal mismatch")
        if not isinstance(row.get("default_profile"), str) or not row["default_profile"]:
            fail(f"{key} default_profile missing")
        if row.get("state") not in {"enabled", "fail_closed"}:
            fail(f"{key} state invalid")
        if row.get("override_class") not in REQUIRED_OVERRIDE_CLASSES:
            fail(f"{key} override_class invalid")
        required_properties = row.get("required_properties")
        if (
            not isinstance(required_properties, list)
            or not required_properties
            or not all(isinstance(item, str) and item for item in required_properties)
        ):
            fail(f"{key} default policy resource has invalid required_properties")
        tx1_seed = row.get("tx1_seed")
        if not isinstance(tx1_seed, dict):
            fail(f"{key} default policy resource tx1_seed is missing")
        if tx1_seed.get("required") is not True:
            fail(f"{key} default policy resource tx1_seed.required is not true")
        if tx1_seed.get("policy_generation") != 1:
            fail(f"{key} default policy resource tx1_seed policy_generation is not 1")
        if tx1_seed.get("created_txn") != "tx1":
            fail(f"{key} default policy resource tx1_seed created_txn is not tx1")
        if tx1_seed.get("uuid_source") != "fresh_uuidv7":
            fail(f"{key} default policy resource tx1_seed uuid_source is not fresh_uuidv7")
        invariants = set(row.get("authority_invariants") or [])
        if not REQUIRED_AUTHORITY_INVARIANTS.issubset(invariants):
            fail(f"{key} default policy resource authority invariants incomplete")
        by_key[key] = row

    if set(by_key) != REQUIRED_POLICY_KEYS:
        fail("default policy resource keys do not exactly match required key set")
    return by_key


def validate_policy_pack_files(paths: dict[str, Path]) -> None:
    pack_root = paths["pack_root"]
    manifest = load_json(paths["policy_pack_manifest"])
    if manifest.get("policy_pack_id") != "default-local-password":
        fail("policy pack manifest id mismatch")
    if manifest.get("post_create_filesystem_authority") is not False:
        fail("policy pack manifest post-create filesystem authority must be false")
    materialization = load_json(paths["policy_materialization"])
    if "DefaultPolicyRecord" not in materialization.get("catalog_row_families", []):
        fail("policy pack materialization metadata is missing DefaultPolicyRecord")
    content_manifest = manifest.get("content_manifest")
    if not isinstance(content_manifest, list) or not content_manifest:
        fail("policy pack content_manifest missing")

    aggregate_payload = b""
    seen_paths: set[str] = set()
    for item in content_manifest:
        rel_path = item.get("path") if isinstance(item, dict) else None
        expected_sha = item.get("sha256") if isinstance(item, dict) else None
        if not isinstance(rel_path, str) or not isinstance(expected_sha, str):
            fail("policy pack content_manifest row malformed")
        if rel_path in seen_paths:
            fail(f"duplicate policy pack content path {rel_path}")
        seen_paths.add(rel_path)
        actual_sha = hashlib.sha256((pack_root / rel_path).read_bytes()).hexdigest()
        if actual_sha != expected_sha:
            fail(f"policy pack content hash mismatch for {rel_path}")
        aggregate_payload += rel_path.encode("utf-8")
        aggregate_payload += b"\0"
        aggregate_payload += expected_sha.encode("utf-8")
        aggregate_payload += b"\n"

    if "policies/default_policy_catalog.json" not in seen_paths:
        fail("policy pack manifest is missing policies/default_policy_catalog.json")
    aggregate_sha = hashlib.sha256(aggregate_payload).hexdigest()
    if aggregate_sha != manifest.get("content_sha256"):
        fail("policy pack aggregate content_sha256 mismatch")


def validate_policy_audit(paths: dict[str, Path], policies: dict[str, dict[str, Any]]) -> list[dict[str, str]]:
    rows = load_csv(paths["policy_audit"])
    if len(rows) != EXPECTED_POLICY_COUNT:
        fail("P0E policy audit row count is wrong")
    by_key = {row["policy_key"]: row for row in rows}
    if set(by_key) != set(policies):
        fail("P0E policy audit keys do not match resource catalog")
    for key, policy in policies.items():
        row = by_key[key]
        if row["profile"] != policy["default_profile"]:
            fail(f"{key} P0E profile mismatch")
        if row["state"] != policy["state"]:
            fail(f"{key} P0E state mismatch")
        if row["override_class"] != policy["override_class"]:
            fail(f"{key} P0E override_class mismatch")
        if int(row["required_property_count"]) != len(policy["required_properties"]):
            fail(f"{key} P0E property count mismatch")
        tx1_seed = row["tx1_seed_requirement"]
        if not (tx1_seed == "seed_enabled_generation_1_uuidv7" or
                (tx1_seed.startswith("seed_") and tx1_seed.endswith("_generation_1_uuidv7"))):
            fail(f"{key} P0E tx1 seed requirement mismatch")
        if row["diagnostic_coverage"] != "POLICY_diagnostic_matrix":
            fail(f"{key} P0E diagnostic coverage mismatch")
        if row["ctest_gate"] != "database_lifecycle_default_policy_catalog":
            fail(f"{key} has wrong P0E CTest gate")
        if row["status"] != "ready":
            fail(f"{key} P0E audit status is not ready")
    return rows


def validate_registry_audit(paths: dict[str, Path], policies: dict[str, dict[str, Any]]) -> list[dict[str, str]]:
    rows = load_csv(paths["registry_audit"])
    if len(rows) != EXPECTED_POLICY_COUNT:
        fail("P0F registry audit row count is wrong")
    by_key = {row["policy_key"]: row for row in rows}
    if set(by_key) != set(policies):
        fail("P0F registry audit keys do not match resource catalog")
    for key, policy in policies.items():
        row = by_key[key]
        if row["packet_profile"] != policy["default_profile"]:
            fail(f"{key} P0F packet_profile mismatch")
        if row["registry_profile"] != policy["default_profile"]:
            fail(f"{key} P0F registry_profile mismatch")
        if row["packet_state"] != policy["state"] or row["registry_state"] != policy["state"]:
            fail(f"{key} P0F state mismatch")
        if row["override_class"] != policy["override_class"]:
            fail(f"{key} P0F override_class mismatch")
        if int(row["required_property_count"]) != len(policy["required_properties"]):
            fail(f"{key} P0F property count mismatch")
        if row["tx1_seed_required"] != "True":
            fail(f"{key} P0F tx1 seed flag mismatch")
        diagnostics = set(filter(None, row["diagnostics_mapped"].split(";")))
        if not diagnostics or not diagnostics.issubset(ROW_DIAGNOSTICS):
            fail(f"{key} P0F diagnostics set invalid")
        if row["override_class"] == "no_override" and "POLICY.OVERRIDE_FORBIDDEN" not in diagnostics:
            fail(f"{key} no_override does not map POLICY.OVERRIDE_FORBIDDEN")
        if row["override_class"] == "cluster_only" and not {
            "POLICY.CLUSTER_SCOPE_FORBIDDEN",
            "POLICY.FAIL_CLOSED_BOUNDARY",
        }.issubset(diagnostics):
            fail(f"{key} cluster_only lacks fail-closed diagnostics")
        if not row["metrics_mapped"]:
            fail(f"{key} metrics mapping missing")
        if not row["cache_requirements_mapped"]:
            fail(f"{key} cache mapping missing")
        invariants = set(filter(None, row["authority_invariants_mapped"].split(";")))
        if not REQUIRED_AUTHORITY_INVARIANTS.issubset(invariants):
            fail(f"{key} authority invariants incomplete")
        if row["status"] != "validation_passed":
            fail(f"{key} P0F audit status is not validation_passed")
    return rows


def mode_catalog(repo_root: Path, paths: dict[str, Path]) -> None:
    policies = load_resource_policy_catalog(paths["resource_policy_catalog"])
    validate_policy_pack_files(paths)
    audit_rows = validate_policy_audit(paths, policies)
    assert_not_ignored(
        repo_root,
        [
            paths["resource_policy_catalog"],
            paths["policy_pack_manifest"],
            paths["policy_materialization"],
            paths["policy_audit"],
        ],
    )
    print(f"PASS: default policy resource and P0E audit cover {len(audit_rows)} policy families")


def mode_registry(repo_root: Path, paths: dict[str, Path]) -> None:
    policies = load_resource_policy_catalog(paths["resource_policy_catalog"])
    rows = validate_registry_audit(paths, policies)
    require_text(
        paths["lifecycle_source"],
        (
            "DEFAULT_POLICY_PACK_IMPORT",
            "LoadSelectedPolicySeedPack",
            "PackDefaultRegistryPolicyRecords",
            "default_policy_records",
            "loaded_at_database_create",
            "durable_catalog_after_create",
        ),
    )
    assert_not_ignored(repo_root, [paths["resource_policy_catalog"], paths["registry_audit"]])
    print(f"PASS: default policy registry audit covers {len(rows)} policy families")


def mode_diagnostics(repo_root: Path, paths: dict[str, Path]) -> None:
    policies = load_resource_policy_catalog(paths["resource_policy_catalog"])
    rows = validate_registry_audit(paths, policies)
    referenced = {diag for row in rows for diag in row["diagnostics_mapped"].split(";") if diag}
    if referenced != ROW_DIAGNOSTICS:
        fail(f"diagnostic audit coverage mismatch: {sorted(referenced)}")
    require_text(
        paths["manifest_gate"],
        (
            "validate_default_policy_catalog",
            "missing default policies",
            "unexpected default policies",
        ),
    )
    require_text(
        paths["lifecycle_source"],
        (
            "SB-POLICY-PACK-DEFAULT-POLICIES-INVALID",
            "SB-POLICY-PACK-DEFAULT-POLICIES-UNKNOWN",
        ),
    )
    assert_not_ignored(repo_root, [paths["manifest_gate"], paths["registry_audit"]])
    print(f"PASS: diagnostic audit covers {len(referenced)} row diagnostics and catalog-level fail-closed gates")


def mode_overrides(repo_root: Path, paths: dict[str, Path]) -> None:
    policies = load_resource_policy_catalog(paths["resource_policy_catalog"])
    validate_registry_audit(paths, policies)
    by_class: dict[str, list[str]] = {}
    for key, policy in policies.items():
        by_class.setdefault(policy["override_class"], []).append(key)
    if set(by_class) != REQUIRED_OVERRIDE_CLASSES:
        fail(f"override classes mismatch: {sorted(by_class)}")
    if policies["replication.cdc_changefeed_boundary"]["state"] != "fail_closed":
        fail("replication boundary policy is not fail_closed")
    cluster_properties = set(policies["cluster.boundary_fail_closed"]["required_properties"])
    if "cluster_routes" not in cluster_properties:
        fail("cluster boundary policy does not include cluster_routes")
    if policies["reference.emulation_profile"]["override_class"] != "no_override":
        fail("reference emulation boundary must remain no_override")
    assert_not_ignored(repo_root, [paths["resource_policy_catalog"], paths["registry_audit"]])
    print(f"PASS: override fixtures cover {len(by_class)} override classes")


def mode_specs(repo_root: Path, paths: dict[str, Path]) -> None:
    policies = load_resource_policy_catalog(paths["resource_policy_catalog"])
    validate_policy_pack_files(paths)
    require_text(
        paths["lifecycle_header"],
        (
            "CONFIG_POLICY_MIGRATION",
            "PolicySeedPackCatalogImage",
            "default_policy_records",
            "post_create_filesystem_authority",
        ),
    )
    require_text(
        paths["migration_gate"],
        (
            "PUBLIC_CONFIG_POLICY_MIGRATION_GATE",
            "policy_pack_default_policy_catalog",
            "DefaultPolicyRecord",
            "validate_default_policy_catalog",
        ),
    )
    require_text(
        paths["catalog_import_gate"],
        (
            "default policy rows were not imported from the policy pack",
            "default_policy_records",
            "loaded_at_database_create",
        ),
    )
    require_text(
        paths["custom_pack_gate"],
        (
            "SB-POLICY-PACK-DEFAULT-POLICIES-UNKNOWN",
            "policies/default_policy_catalog.json",
        ),
    )
    require_text(
        paths["policy_pack_readme"],
        (
            "create-time input only",
            "post-create policy mutation must not re-read this filesystem pack",
            "durable",
            "catalog is the authority",
        ),
    )
    assert_not_ignored(
        repo_root,
        [
            paths["resource_policy_catalog"],
            paths["lifecycle_header"],
            paths["lifecycle_source"],
            paths["migration_gate"],
            paths["catalog_import_gate"],
            paths["custom_pack_gate"],
        ],
    )
    print(f"PASS: current default-policy resource closure anchors cover {len(policies)} policy families")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--mode", choices=("catalog", "registry", "diagnostics", "overrides", "specs"), required=True)
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    paths = load_paths(repo_root)
    required = [path for key, path in paths.items() if key != "pack_root"]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        fail(f"required files missing: {missing}")
    if args.mode == "catalog":
        mode_catalog(repo_root, paths)
    elif args.mode == "registry":
        mode_registry(repo_root, paths)
    elif args.mode == "diagnostics":
        mode_diagnostics(repo_root, paths)
    elif args.mode == "overrides":
        mode_overrides(repo_root, paths)
    elif args.mode == "specs":
        mode_specs(repo_root, paths)


if __name__ == "__main__":
    main()
