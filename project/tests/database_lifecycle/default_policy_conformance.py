#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Default policy catalog conformance gates for DBLC-000E/DBLC-000F."""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml


EXPECTED_POLICY_COUNT = 58
REQUIRED_DIAGNOSTICS = {
    "POLICY.CATALOG_MISSING",
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
    "donor_not_authority",
    "uuid_order_not_finality",
}
FORBIDDEN_AUTHORITY_TEXT = (
    "wal_finality=true",
    "cache_finality=true",
    "checkpoint_finality=true",
    "parser_auth_authority=true",
    "donor_sql_exec_inside_engine=true",
    "uuid_order_is_finality=true",
)


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_yaml(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return yaml.safe_load(handle)
    except Exception as exc:  # pragma: no cover - CTest reports exact exception.
        fail(f"{path} does not parse as YAML: {exc}")


def parse_packet(packet_path: Path) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    text = packet_path.read_text(encoding="utf-8")
    if "DEFAULT-POLICY-CATALOG-CREATE-DATABASE" not in text:
        fail("default policy packet is missing authority search key")
    if any(token in text for token in FORBIDDEN_AUTHORITY_TEXT):
        fail("default policy packet contains forbidden authority text")

    policies: dict[str, dict[str, Any]] = {}
    in_table = False
    for line in text.splitlines():
        if line.strip() == "## Default policy family table":
            in_table = True
            continue
        if in_table and line.startswith("## "):
            break
        if not in_table or not line.startswith("| `"):
            continue
        cols = [col.strip() for col in line.strip().strip("|").split("|")]
        if len(cols) != 3:
            continue
        policy_key = cols[0].strip("`")
        profile_state = re.findall(r"`([^`]+)`", cols[1])
        if len(profile_state) != 2:
            fail(f"policy row {policy_key} does not expose profile and state")
        property_names = re.findall(r"([A-Za-z0-9_]+)=", cols[2])
        if not property_names:
            fail(f"policy row {policy_key} has no required properties")
        policies[policy_key] = {
            "default_profile": profile_state[0],
            "state": profile_state[1],
            "required_property_count": len(property_names),
            "required_property_names": set(property_names),
        }

    override_map: dict[str, str] = {}
    in_override_map = False
    for line in text.splitlines():
        if line.strip() == "## Default policy override-class map":
            in_override_map = True
            continue
        if in_override_map and line.startswith("## "):
            break
        if not in_override_map or not line.startswith("| `"):
            continue
        cols = [col.strip() for col in line.strip().strip("|").split("|")]
        if len(cols) >= 2:
            override_map[cols[0].strip("`")] = cols[1].strip("`")

    if len(policies) != EXPECTED_POLICY_COUNT:
        fail(f"packet policy count {len(policies)} != {EXPECTED_POLICY_COUNT}")
    if set(override_map) != set(policies):
        fail("override-class map does not exactly match packet policy keys")
    return policies, override_map


def load_paths(repo_root: Path) -> dict[str, Path]:
    return {
        "packet": repo_root / "public_input_snapshot",
        "engine_lifecycle": repo_root / "public_input_snapshot",
        "registry": repo_root / "public_contract_snapshot",
        "conformance": repo_root / "public_contract_snapshot",
        "manifest": repo_root / "public_contract_snapshot",
        "diag_codes": repo_root / "public_contract_snapshot",
        "diag_shapes": repo_root / "public_contract_snapshot",
        "policy_audit": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_DEFAULT_POLICY_AUDIT.csv",
        "registry_audit": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_DEFAULT_POLICY_REGISTRY_AUDIT.csv",
        "hygiene_audit": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_REPO_HYGIENE_AUDIT.csv",
        "storage_chapter": repo_root / "public_contract_snapshot",
        "security_chapter": repo_root / "public_contract_snapshot",
        "server_chapter": repo_root / "public_contract_snapshot",
        "resource_chapter": repo_root / "public_contract_snapshot",
        "ops_chapter": repo_root / "public_contract_snapshot",
    }


def assert_not_ignored(repo_root: Path, paths: list[Path]) -> None:
    for path in paths:
        rel = path.relative_to(repo_root)
        if rel.parts[:2] == ("docs", "contracts"):
            continue
        result = subprocess.run(
            ["git", "check-ignore", "-q", str(rel)],
            cwd=repo_root,
            check=False,
        )
        if result.returncode == 0:
            fail(f"{rel} is ignored by git")
        if result.returncode not in (0, 1):
            fail(f"git check-ignore failed for {rel} with rc={result.returncode}")


def manifest_entries(manifest: dict[str, Any]) -> set[str]:
    entries: set[str] = set()
    for key in ("authority_files", "registry_files"):
        for item in manifest.get(key, []) or []:
            if isinstance(item, str):
                entries.add(item.removeprefix("public_release_evidence"))
    for item in manifest.get("appendix_authority_files", []) or []:
        if isinstance(item, dict) and isinstance(item.get("path"), str):
            entries.add(item["path"].removeprefix("public_release_evidence"))
    return entries


def registry_by_key(registry: dict[str, Any]) -> dict[str, dict[str, Any]]:
    policies = registry.get("policies")
    if not isinstance(policies, list):
        fail("registry.policies is not a list")
    by_key: dict[str, dict[str, Any]] = {}
    for policy in policies:
        key = policy.get("policy_key")
        if not isinstance(key, str):
            fail("registry policy without policy_key")
        if key in by_key:
            fail(f"duplicate registry policy key {key}")
        by_key[key] = policy
    return by_key


def validate_registry_core(
    repo_root: Path,
    paths: dict[str, Path],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]], dict[str, str]]:
    packet_policies, override_map = parse_packet(paths["packet"])
    registry = load_yaml(paths["registry"])
    if registry.get("policy_family_count") != EXPECTED_POLICY_COUNT:
        fail("registry policy_family_count is wrong")
    policies = registry_by_key(registry)
    if set(policies) != set(packet_policies):
        fail("registry policy keys do not exactly match packet")

    required_top = {
        "schema_version",
        "registry_id",
        "search_key",
        "source_of_truth",
        "universal_seed_requirements",
        "authority_invariants",
        "diagnostic_codes_required",
        "policies",
    }
    missing_top = sorted(required_top - set(registry))
    if missing_top:
        fail(f"registry missing top-level fields: {missing_top}")
    if set(registry["diagnostic_codes_required"]) != REQUIRED_DIAGNOSTICS:
        fail("registry diagnostic_codes_required does not match packet diagnostics")

    for key, packet_row in packet_policies.items():
        policy = policies[key]
        if policy.get("default_profile") != packet_row["default_profile"]:
            fail(f"{key} default_profile mismatch")
        if policy.get("state") != packet_row["state"]:
            fail(f"{key} state mismatch")
        if policy.get("override_class") != override_map[key]:
            fail(f"{key} override_class mismatch")
        required_properties = policy.get("required_properties")
        if not isinstance(required_properties, dict) or not required_properties:
            fail(f"{key} has no registry required_properties")
        if set(required_properties) != packet_row["required_property_names"]:
            fail(f"{key} required property names mismatch")
        tx1_seed = policy.get("tx1_seed")
        if not isinstance(tx1_seed, dict):
            fail(f"{key} tx1_seed is missing")
        if tx1_seed.get("required") is not True:
            fail(f"{key} tx1_seed.required is not true")
        if tx1_seed.get("policy_generation") != 1:
            fail(f"{key} tx1_seed policy_generation is not 1")
        if tx1_seed.get("uuid_source") != "fresh_uuidv7":
            fail(f"{key} tx1_seed uuid_source is not fresh_uuidv7")
        if tx1_seed.get("created_txn") != "tx1":
            fail(f"{key} tx1_seed created_txn is not tx1")
        diagnostics = policy.get("diagnostics")
        if not isinstance(diagnostics, list) or not diagnostics:
            fail(f"{key} diagnostics missing")
        if len(diagnostics) != len(set(diagnostics)):
            fail(f"{key} has duplicate diagnostics")
        if not set(diagnostics).issubset(REQUIRED_DIAGNOSTICS):
            fail(f"{key} references unknown policy diagnostics")
        if policy.get("override_class") == "no_override" and "POLICY.OVERRIDE_FORBIDDEN" not in diagnostics:
            fail(f"{key} no_override does not map POLICY.OVERRIDE_FORBIDDEN")
        if not policy.get("metrics"):
            fail(f"{key} metrics missing")
        if not policy.get("cache_requirements"):
            fail(f"{key} cache requirements missing")
        invariants = set(policy.get("authority_invariants") or [])
        if not REQUIRED_AUTHORITY_INVARIANTS.issubset(invariants):
            fail(f"{key} authority invariants incomplete")

    assert_not_ignored(
        repo_root,
        [paths["packet"], paths["registry"], paths["conformance"], paths["diag_codes"], paths["diag_shapes"]],
    )
    manifest = load_yaml(paths["manifest"])
    entries = manifest_entries(manifest)
    for required in (
        "implementation_inputs/default_policy_catalog.md",
        "registries/default-policy-catalog.yaml",
        "conformance_manifests/default-policy-catalog-conformance.yaml",
    ):
        if required not in entries:
            fail(f"manifest missing {required}")
    return packet_policies, policies, override_map


def collect_code_rows(node: Any) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if isinstance(node, dict):
        if isinstance(node.get("code"), str):
            rows.append(node)
        for value in node.values():
            rows.extend(collect_code_rows(value))
    elif isinstance(node, list):
        for value in node:
            rows.extend(collect_code_rows(value))
    return rows


def collect_shape_rows(node: Any) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if isinstance(node, dict):
        if isinstance(node.get("diagnostic_shape_id"), str):
            rows.append(node)
        for value in node.values():
            rows.extend(collect_shape_rows(value))
    elif isinstance(node, list):
        for value in node:
            rows.extend(collect_shape_rows(value))
    return rows


def mode_catalog(repo_root: Path, paths: dict[str, Path]) -> None:
    packet_policies, _ = parse_packet(paths["packet"])
    with paths["policy_audit"].open(newline="", encoding="utf-8") as handle:
        audit_rows = list(csv.DictReader(handle))
    if len(audit_rows) != EXPECTED_POLICY_COUNT:
        fail("P0E policy audit row count is wrong")
    if {row["policy_key"] for row in audit_rows} != set(packet_policies):
        fail("P0E policy audit keys do not match packet")
    for row in audit_rows:
        if row["ctest_gate"] != "database_lifecycle_default_policy_catalog":
            fail(f"{row['policy_key']} has wrong P0E CTest gate")
        if row["status"] != "ready":
            fail(f"{row['policy_key']} P0E audit status is not ready")
        expected_count = packet_policies[row["policy_key"]]["required_property_count"]
        if int(row["required_property_count"]) != expected_count:
            fail(f"{row['policy_key']} P0E property count mismatch")
    assert_not_ignored(repo_root, [paths["packet"]])
    print(f"PASS: default policy packet and P0E audit cover {len(audit_rows)} policy families")


def mode_registry(repo_root: Path, paths: dict[str, Path]) -> None:
    _, policies, _ = validate_registry_core(repo_root, paths)
    with paths["registry_audit"].open(newline="", encoding="utf-8") as handle:
        audit_rows = list(csv.DictReader(handle))
    if len(audit_rows) != EXPECTED_POLICY_COUNT:
        fail("P0F registry audit row count is wrong")
    if {row["policy_key"] for row in audit_rows} != set(policies):
        fail("P0F registry audit keys do not match registry")
    conformance = load_yaml(paths["conformance"])
    gate_ids = {gate.get("id") for gate in conformance.get("gates", [])}
    for gate in (
        "database_lifecycle_default_policy_registry",
        "database_lifecycle_policy_diagnostic_registry",
        "database_lifecycle_policy_override_fixtures",
    ):
        if gate not in gate_ids:
            fail(f"conformance manifest missing gate {gate}")
    print(f"PASS: default policy registry covers {len(policies)} policy families")


def mode_diagnostics(repo_root: Path, paths: dict[str, Path]) -> None:
    _, policies, _ = validate_registry_core(repo_root, paths)
    code_rows = collect_code_rows(load_yaml(paths["diag_codes"]))
    policy_codes = {row["code"]: row for row in code_rows if row.get("class") == "POLICY"}
    if not REQUIRED_DIAGNOSTICS.issubset(policy_codes):
        fail("diagnostic-code registry does not cover all POLICY.* codes")
    shape_rows = collect_shape_rows(load_yaml(paths["diag_shapes"]))
    shapes = {row["diagnostic_shape_id"]: row for row in shape_rows}
    for code in REQUIRED_DIAGNOSTICS:
        row = policy_codes[code]
        for field in ("severity", "retryable", "owner", "audit_policy", "shape"):
            if field not in row:
                fail(f"{code} missing {field}")
        shape = shapes.get(row["shape"])
        if not shape:
            fail(f"{code} references missing shape {row['shape']}")
        if not shape.get("message_vector_mapping"):
            fail(f"{code} shape has no message_vector_mapping")
        if shape.get("canonical_message_key", "").startswith("raw"):
            fail(f"{code} shape uses raw string diagnostic")
    referenced = {diag for policy in policies.values() for diag in policy.get("diagnostics", [])}
    if not referenced.issubset(policy_codes):
        fail("registry references POLICY diagnostics absent from diagnostic-code registry")
    print(f"PASS: diagnostic registries cover {len(referenced)} referenced policy diagnostics")


def mode_overrides(repo_root: Path, paths: dict[str, Path]) -> None:
    _, policies, _ = validate_registry_core(repo_root, paths)
    by_class: dict[str, list[str]] = {}
    for key, policy in policies.items():
        by_class.setdefault(policy["override_class"], []).append(key)
    required_classes = {"no_override", "create_database_only", "security_admin", "sysarch", "policy_defined", "cluster_only"}
    if set(by_class) != required_classes:
        fail(f"override classes mismatch: {sorted(by_class)}")
    for key in by_class["no_override"]:
        if "POLICY.OVERRIDE_FORBIDDEN" not in policies[key]["diagnostics"]:
            fail(f"{key} no_override lacks override-forbidden diagnostic")
    for key in by_class["cluster_only"]:
        diagnostics = set(policies[key]["diagnostics"])
        if not {"POLICY.CLUSTER_SCOPE_FORBIDDEN", "POLICY.FAIL_CLOSED_BOUNDARY"}.intersection(diagnostics):
            fail(f"{key} cluster_only lacks fail-closed diagnostics")
    if policies["replication.cdc_changefeed_boundary"]["state"] != "fail_closed":
        fail("replication boundary policy is not fail_closed")
    if policies["cluster.boundary_fail_closed"]["required_properties"].get("cluster_routes") != "deny":
        fail("cluster boundary policy does not deny standalone cluster routes")
    print(f"PASS: override fixtures cover {len(by_class)} override classes")


def mode_specs(repo_root: Path, paths: dict[str, Path]) -> None:
    spec_paths = [
        paths["engine_lifecycle"],
        paths["storage_chapter"],
        paths["security_chapter"],
        paths["server_chapter"],
        paths["resource_chapter"],
        paths["ops_chapter"],
    ]
    assert_not_ignored(repo_root, spec_paths)
    search_key = "DBLC-001-CANONICAL-LIFECYCLE-SPEC-CLOSURE"
    for path in spec_paths:
        text = path.read_text(encoding="utf-8")
        if search_key not in text:
            fail(f"{path.relative_to(repo_root)} is missing {search_key}")
    engine_text = paths["engine_lifecycle"].read_text(encoding="utf-8")
    required_engine_terms = (
        "create tx1",
        "first-open tx2",
        "open/reopen",
        "attach",
        "transaction admission",
        "detach",
        "maintenance",
        "restricted-open",
        "diagnostic",
        "verify",
        "repair",
        "graceful shutdown",
        "force shutdown",
        "final clean shutdown transaction",
        "drop",
        "message vector",
        "cache invalidation",
        "MGA",
        "WAL",
        "cluster fail-closed",
        "manager",
        "listener",
        "parser",
        "process association",
    )
    lowered = engine_text.lower()
    for term in required_engine_terms:
        if term.lower() not in lowered:
            fail(f"engine lifecycle packet missing canonical term: {term}")
    cross_file_terms = {
        "storage_chapter": ("tx1", "tx2", "final shutdown", "MGA", "WAL", "filespace"),
        "security_chapter": ("authentication", "authorization", "policy generation", "engine-owned"),
        "server_chapter": ("manager", "listener", "parser", "shutdown", "process association"),
        "resource_chapter": ("timezone", "charset", "collation", "tx1", "first-open"),
        "ops_chapter": ("maintenance", "restricted", "verify", "repair", "metrics", "diagnostic"),
    }
    for key, terms in cross_file_terms.items():
        text = paths[key].read_text(encoding="utf-8").lower()
        for term in terms:
            if term.lower() not in text:
                fail(f"{paths[key].relative_to(repo_root)} missing term {term}")
    print("PASS: canonical lifecycle contract closure search key and required terms are present")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--mode", choices=("catalog", "registry", "diagnostics", "overrides", "specs"), required=True)
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    paths = load_paths(repo_root)
    missing = [str(path) for path in paths.values() if not path.exists()]
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
