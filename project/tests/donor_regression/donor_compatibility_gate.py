#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""P6 donor compatibility profile and regression-import gate.

This gate proves that donor compatibility admission is data-driven, release
packet backed, and constrained to ScratchBird engine authority. Donor tools and
source packets are accepted only as compatibility evidence; they never become
storage, recovery, transaction, or security authority.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import sys
from typing import Iterable

import yaml


TRUE_DONORS = (
    "firebird",
    "postgresql",
    "mysql",
    "mariadb",
    "sqlite",
    "duckdb",
    "clickhouse",
    "cassandra",
    "mongodb",
    "redis",
    "neo4j",
    "opensearch",
    "influxdb",
    "milvus",
    "cockroachdb",
    "tidb",
    "tikv",
    "yugabytedb",
    "vitess",
    "foundationdb",
    "apache_ignite",
    "dolt",
    "immudb",
    "xtdb",
    "opensearch_sql_ppl",
)

RELEASE_PROFILE_VARIANT_OWNERS = {
    "mysql_lts": "mysql",
}

RELEASE_PROFILE_VARIANTS = tuple(RELEASE_PROFILE_VARIANT_OWNERS)

CAPABILITY_REFERENCE_FAMILIES = ("sqlserver", "oracle", "db2")

EXPECTED_COMPATIBILITY_PROFILE_ROWS = (
    "firebird",
    "postgresql",
    "mysql",
    "mariadb",
    "mysql_lts",
    "sqlite",
    "duckdb",
    "clickhouse",
    "cassandra",
    "mongodb",
    "redis",
    "neo4j",
    "opensearch",
    "influxdb",
    "milvus",
    "cockroachdb",
    "tidb",
    "tikv",
    "yugabytedb",
    "vitess",
    "foundationdb",
    "apache_ignite",
    "dolt",
    "immudb",
    "xtdb",
    "sqlserver",
    "oracle",
    "db2",
    "opensearch_sql_ppl",
)

FAMILY_BATCHES = {
    "relational": (
        "firebird",
        "postgresql",
        "mysql",
        "mariadb",
        "sqlite",
        "duckdb",
    ),
    "analytic": ("clickhouse", "opensearch", "influxdb", "milvus", "opensearch_sql_ppl"),
    "nosql": ("cassandra", "mongodb", "redis", "neo4j", "xtdb"),
    "distributed": (
        "cockroachdb",
        "tidb",
        "tikv",
        "yugabytedb",
        "vitess",
        "foundationdb",
        "apache_ignite",
        "dolt",
        "immudb",
    ),
}

P6_GAPS = tuple(f"SB-PUBLIC-GAP-{gap:04d}" for gap in range(89, 129))

CORE_GAPS = tuple(f"SB-PUBLIC-GAP-{gap:04d}" for gap in range(89, 101))

REQUIRED_PROFILE_COLUMNS = (
    "family_id",
    "display_name",
    "gap_id",
    "batch",
    "profile_class",
    "release_profile",
    "release_evidence_manifest",
    "release_regression_root",
    "parser_module",
    "parser_spec_path",
    "capability_map_path",
    "go_readiness_path",
    "conformance_manifest",
    "seed_manifest_family",
    "seed_manifest_path",
    "wire_profile",
    "datatype_profile",
    "index_profile",
    "diagnostic_profile",
    "metadata_overlay_profile",
    "migration_cdc_profile",
    "sandbox_bridge_profile",
    "builtin_surface_profile",
    "udr_bridge_profile",
    "authority_policy",
    "donor_sql_execution",
    "donor_storage_authority",
    "donor_recovery_authority",
    "parser_cross_dialect_dependency",
    "runtime_seed_authority",
    "capability_reference_policy",
    "required_labels",
)

REQUIRED_UDR_COLUMNS = (
    "family_id",
    "profile_class",
    "udr_package",
    "bridge_mode",
    "admission_policy",
    "allowed_runtime_effects",
    "transaction_authority",
    "security_authority",
    "forbidden_authorities",
    "diagnostic_set",
    "conformance_gate",
)

REQUIRED_RELEASE_ARTIFACTS = (
    "source_archive_or_release_tag_clone",
    "release_notes",
    "license_text",
    "version_proof",
    "grammar_sources",
    "upstream_regression_roots",
    "clean_room_notes",
    "redaction_and_visibility_notes",
)

PUBLIC_RELEASE_EVIDENCE_SENTINEL = "private_release_evidence_not_public"
PUBLIC_CONTRACT_SENTINEL = "public_contract_snapshot"

REQUIRED_SEED_SETS = (
    "catalog_object_set",
    "catalog_column_set",
    "default_row_set",
    "default_runtime_generated_value_set",
    "visibility_rule_set",
    "redaction_rule_set",
    "mutation_rule_set",
    "scratchbird_mapping_set",
    "conformance_gate_set",
)

FORBIDDEN_AUTHORITY_TOKENS = (
    "donor_storage_authority",
    "donor_recovery_authority",
    "donor_transaction_authority",
    "parser_transaction_authority",
    "wal_recovery_authority",
)


def relpath(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise AssertionError(f"{path}: missing CSV header")
        return [dict(row) for row in reader]


def load_yaml(path: pathlib.Path) -> object:
    with path.open(encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def require_file(repo_root: pathlib.Path, rel: str, context: str) -> pathlib.Path:
    if rel in {"not_applicable", "forbidden", "none"}:
        raise AssertionError(f"{context}: required file path is {rel!r}")
    path = (repo_root / rel).resolve()
    if not path.exists():
        raise AssertionError(f"{context}: missing required file {rel}")
    if not path.is_file():
        raise AssertionError(f"{context}: expected file path {rel}")
    return path


def require_public_contract_or_file(repo_root: pathlib.Path, rel: str, context: str) -> None:
    if rel == PUBLIC_CONTRACT_SENTINEL:
        return
    require_file(repo_root, rel, context)


def require_dir(repo_root: pathlib.Path, rel: str, context: str) -> pathlib.Path:
    if rel in {"not_applicable", "forbidden", "none"}:
        raise AssertionError(f"{context}: required directory path is {rel!r}")
    path = (repo_root / rel).resolve()
    if not path.exists():
        raise AssertionError(f"{context}: missing required directory {rel}")
    if not path.is_dir():
        raise AssertionError(f"{context}: expected directory path {rel}")
    return path


def assert_columns(rows: list[dict[str, str]], required: Iterable[str], path: pathlib.Path) -> None:
    if not rows:
        raise AssertionError(f"{path}: no rows")
    missing = [column for column in required if column not in rows[0]]
    if missing:
        raise AssertionError(f"{path}: missing columns {', '.join(missing)}")


def load_profiles(repo_root: pathlib.Path) -> list[dict[str, str]]:
    path = repo_root / "project/src/parsers/donor/DonorCompatibilityProfileManifest.csv"
    rows = read_csv(path)
    assert_columns(rows, REQUIRED_PROFILE_COLUMNS, path)
    return rows


def load_udr_policies(repo_root: pathlib.Path) -> dict[str, dict[str, str]]:
    path = repo_root / "project/src/udr/packages/donor/DonorUdrBridgePolicyManifest.csv"
    rows = read_csv(path)
    assert_columns(rows, REQUIRED_UDR_COLUMNS, path)
    return {row["family_id"]: row for row in rows}


def require_bool_false(row: dict[str, str], key: str) -> None:
    if row[key] != "false":
        raise AssertionError(f"{row['family_id']}: {key} must be false")


def require_labels(row: dict[str, str], expected: Iterable[str]) -> None:
    labels = set(filter(None, row["required_labels"].split(";")))
    missing = [label for label in expected if label not in labels]
    if missing:
        raise AssertionError(f"{row['family_id']}: missing labels {', '.join(missing)}")


def validate_release_packet(repo_root: pathlib.Path, row: dict[str, str]) -> None:
    if row["release_evidence_manifest"] == PUBLIC_RELEASE_EVIDENCE_SENTINEL:
        require_dir(repo_root, row["release_regression_root"], f"{row['family_id']} regression root")
        return

    manifest_path = require_file(
        repo_root, row["release_evidence_manifest"], f"{row['family_id']} release evidence"
    )
    packet = load_yaml(manifest_path)
    if not isinstance(packet, dict):
        raise AssertionError(f"{row['family_id']}: malformed release evidence")
    allowed_families = {row["seed_manifest_family"], row["family_id"]}
    if row["family_id"] == "mysql_lts":
        allowed_families.add("mysql")
    if packet.get("family_id") not in allowed_families:
        raise AssertionError(f"{row['family_id']}: release evidence family mismatch")
    if packet.get("structural_audit_status") != "passed":
        raise AssertionError(f"{row['family_id']}: release evidence audit did not pass")
    artifacts = {
        item.get("artifact"): item
        for item in packet.get("required_artifacts", [])
        if isinstance(item, dict)
    }
    for artifact in REQUIRED_RELEASE_ARTIFACTS:
        item = artifacts.get(artifact)
        if not item:
            raise AssertionError(f"{row['family_id']}: missing release artifact {artifact}")
        if not str(item.get("status", "")).startswith("present"):
            raise AssertionError(f"{row['family_id']}: release artifact {artifact} not present")
        local_path = item.get("local_path")
        if not local_path:
            raise AssertionError(f"{row['family_id']}: release artifact {artifact} lacks path")
        artifact_path = manifest_path.parent / str(local_path)
        if not artifact_path.exists():
            raise AssertionError(
                f"{row['family_id']}: release artifact {artifact} path missing: {artifact_path}"
            )
    require_dir(repo_root, row["release_regression_root"], f"{row['family_id']} regression root")


def validate_seed_manifest(repo_root: pathlib.Path, row: dict[str, str]) -> None:
    seed_path = require_file(repo_root, row["seed_manifest_path"], f"{row['family_id']} seed")
    manifest = load_yaml(seed_path)
    if not isinstance(manifest, dict):
        raise AssertionError(f"{row['family_id']}: malformed seed manifest")
    if manifest.get("donor_family") != row["seed_manifest_family"]:
        raise AssertionError(f"{row['family_id']}: seed donor_family mismatch")
    if manifest.get("seed_manifest_status") not in {
        "actual_private_seed_manifest",
        "profile_derived_private_seed_manifest",
    }:
        raise AssertionError(f"{row['family_id']}: seed manifest status is not admissible")
    for key in REQUIRED_SEED_SETS:
        value = manifest.get(key)
        if not value:
            raise AssertionError(f"{row['family_id']}: seed manifest missing {key}")
    row_hashes = manifest.get("rowset_hashes")
    if not isinstance(row_hashes, dict):
        raise AssertionError(f"{row['family_id']}: seed manifest missing rowset hashes")
    for key in REQUIRED_SEED_SETS[:-1]:
        if key not in row_hashes:
            raise AssertionError(f"{row['family_id']}: seed manifest missing hash for {key}")
    for item in manifest.get("default_row_set", []):
        if not isinstance(item, dict) or not item.get("row_hash"):
            raise AssertionError(f"{row['family_id']}: default row missing row_hash")
    for item in manifest.get("scratchbird_mapping_set", []):
        if item.get("uuid_exposure_rule") != "hidden":
            raise AssertionError(f"{row['family_id']}: seed mapping exposes UUIDs")
    if not manifest.get("manifest_hash") or len(str(manifest["manifest_hash"])) != 64:
        raise AssertionError(f"{row['family_id']}: seed manifest hash missing or invalid")


def validate_true_donor(repo_root: pathlib.Path, row: dict[str, str]) -> None:
    family = row["family_id"]
    if row["profile_class"] != "donor_emulation":
        raise AssertionError(f"{family}: true donor must use donor_emulation profile_class")
    if not row["parser_module"].startswith("project/src/parsers/donor/"):
        raise AssertionError(f"{family}: parser module must be under donor parser tree")
    if row["authority_policy"] != "engine_sblr_mga_only":
        raise AssertionError(f"{family}: invalid authority policy")
    for key in (
        "donor_sql_execution",
        "donor_storage_authority",
        "donor_recovery_authority",
        "parser_cross_dialect_dependency",
    ):
        require_bool_false(row, key)
    if row["runtime_seed_authority"] != "donor_catalog_seed_manifest":
        raise AssertionError(f"{family}: runtime seed authority must be seed manifest")
    if row["capability_reference_policy"] != "not_capability_reference":
        raise AssertionError(f"{family}: true donor must not use capability-reference policy")

    for key in (
        "parser_spec_path",
        "capability_map_path",
        "go_readiness_path",
        "conformance_manifest",
    ):
        require_public_contract_or_file(repo_root, row[key], f"{family} {key}")
    validate_release_packet(repo_root, row)
    validate_seed_manifest(repo_root, row)
    require_labels(
        row,
        (
            "donor_core_framework_gate",
            "donor_seed_gate",
            "donor_datatype_gate",
            "donor_index_translation_gate",
            "donor_diagnostic_rendering_gate",
            "donor_metadata_overlay_gate",
            "donor_migration_cdc_gate",
            "donor_sandbox_bridge_gate",
            "donor_release_regression_gate",
            "donor_original_regression_gate",
        ),
    )


def validate_profile_variant(repo_root: pathlib.Path, row: dict[str, str]) -> None:
    family = row["family_id"]
    if family not in RELEASE_PROFILE_VARIANTS:
        raise AssertionError(f"{family}: unknown release/profile variant")
    owner = RELEASE_PROFILE_VARIANT_OWNERS[family]
    if owner not in TRUE_DONORS:
        raise AssertionError(f"{family}: profile variant owner is not a true donor")
    if family in TRUE_DONORS:
        raise AssertionError(f"{family}: profile variant must not be a true donor")
    if row["profile_class"] != "release_profile_variant":
        raise AssertionError(f"{family}: profile variant must use release_profile_variant")
    if row["parser_module"] != f"project/src/parsers/donor/{owner}":
        raise AssertionError(f"{family}: profile variant must map to owner parser module")
    if row["udr_bridge_profile"] != f"{owner}.trusted_parser_support_or_exact_refusal":
        raise AssertionError(f"{family}: profile variant must map to owner parser-support UDR")
    if row["seed_manifest_family"] != owner:
        raise AssertionError(f"{family}: profile variant seed family must be owner donor")
    if row["seed_manifest_path"] != (
        "project/tests/donor_regression/donor_catalog_seeds/mysql_lts/mysql_lts_beta2_full_seed_manifest.yaml"
    ):
        raise AssertionError(f"{family}: profile variant seed evidence path changed unexpectedly")
    if row["authority_policy"] != "engine_sblr_mga_only":
        raise AssertionError(f"{family}: invalid authority policy")
    for key in (
        "donor_sql_execution",
        "donor_storage_authority",
        "donor_recovery_authority",
        "parser_cross_dialect_dependency",
    ):
        require_bool_false(row, key)
    if row["runtime_seed_authority"] != "donor_catalog_seed_manifest":
        raise AssertionError(f"{family}: runtime seed authority must remain seed manifest backed")
    if row["capability_reference_policy"] != "not_capability_reference":
        raise AssertionError(f"{family}: profile variant must not use capability-reference policy")
    for key in (
        "parser_spec_path",
        "capability_map_path",
        "go_readiness_path",
        "conformance_manifest",
    ):
        require_public_contract_or_file(repo_root, row[key], f"{family} {key}")
    validate_release_packet(repo_root, row)
    validate_seed_manifest(repo_root, row)
    require_labels(
        row,
        (
            "donor_core_framework_gate",
            "donor_seed_gate",
            "donor_datatype_gate",
            "donor_index_translation_gate",
            "donor_diagnostic_rendering_gate",
            "donor_metadata_overlay_gate",
            "donor_migration_cdc_gate",
            "donor_sandbox_bridge_gate",
            "donor_release_regression_gate",
            "donor_original_regression_gate",
        ),
    )


def validate_capability_reference(repo_root: pathlib.Path, row: dict[str, str]) -> None:
    family = row["family_id"]
    if row["profile_class"] != "capability_reference":
        raise AssertionError(f"{family}: capref family must use capability_reference profile_class")
    if row["parser_module"] != "forbidden":
        raise AssertionError(f"{family}: capref parser module must be forbidden")
    if row["seed_manifest_path"] != "forbidden":
        raise AssertionError(f"{family}: capref seed manifest must be forbidden")
    if row["wire_profile"] != "inbound_wire_forbidden":
        raise AssertionError(f"{family}: capref inbound wire must be forbidden")
    if row["runtime_seed_authority"] != "forbidden":
        raise AssertionError(f"{family}: capref runtime seed authority must be forbidden")
    if row["capability_reference_policy"] != "commercial_capability_reference_only":
        raise AssertionError(f"{family}: capref policy mismatch")
    for key in ("capability_map_path",):
        require_public_contract_or_file(repo_root, row[key], f"{family} {key}")
    require_labels(row, ("donor_capability_reference_gate",))


def validate_udr_policy(rows: list[dict[str, str]], policies: dict[str, dict[str, str]]) -> None:
    expected = {
        row["family_id"]
        for row in rows
        if row["profile_class"] != "release_profile_variant"
    }
    if set(policies) != expected:
        raise AssertionError("UDR policy families do not match donor profile families")
    variant_families = {
        row["family_id"]
        for row in rows
        if row["profile_class"] == "release_profile_variant"
    }
    unexpected_variants = sorted(variant_families & set(policies))
    if unexpected_variants:
        raise AssertionError(
            "release/profile variants must not have separate UDR policy rows: "
            + ", ".join(unexpected_variants)
        )
    if "mysql_lts" in variant_families and "mysql" not in policies:
        raise AssertionError("mysql_lts release/profile variant requires mysql UDR policy")
    for family, policy in policies.items():
        if policy["transaction_authority"] != "engine_mga":
            raise AssertionError(f"{family}: UDR policy must keep engine_mga transaction authority")
        if policy["security_authority"] != "engine_security":
            raise AssertionError(f"{family}: UDR policy must keep engine security authority")
        forbidden = set(filter(None, policy["forbidden_authorities"].split(";")))
        missing = [token for token in FORBIDDEN_AUTHORITY_TOKENS if token not in forbidden]
        if missing:
            raise AssertionError(f"{family}: UDR policy missing forbidden authorities {missing}")


def validate_seed_index(repo_root: pathlib.Path, rows: list[dict[str, str]]) -> None:
    index_path = (
        repo_root
        / "project/tests/donor_regression/donor_catalog_seeds/actual_per_family_seed_manifest_index.yaml"
    )
    index = load_yaml(index_path)
    if not isinstance(index, dict):
        raise AssertionError("seed manifest index is malformed")
    manifests = {entry["family"]: entry for entry in index.get("manifests", [])}
    expected = {row["seed_manifest_family"] for row in rows if row["profile_class"] == "donor_emulation"}
    missing = sorted(expected - set(manifests))
    if missing:
        raise AssertionError(f"seed manifest index missing families {', '.join(missing)}")
    variant_entries = sorted(set(RELEASE_PROFILE_VARIANTS) & set(manifests))
    if variant_entries:
        raise AssertionError(
            "release/profile variants must not be runtime seed index families: "
            + ", ".join(variant_entries)
        )
    extra = sorted(set(manifests) - expected)
    if extra:
        raise AssertionError(f"seed manifest index contains non-runtime families {', '.join(extra)}")
    forbidden = sorted(set(CAPABILITY_REFERENCE_FAMILIES) & set(manifests))
    if forbidden:
        raise AssertionError(f"capability-reference families in runtime seed index: {forbidden}")
    for family in expected:
        entry = manifests[family]
        manifest_path = require_file(repo_root, entry["manifest_path"], f"{family} indexed seed")
        manifest = load_yaml(manifest_path)
        if not isinstance(manifest, dict):
            raise AssertionError(f"{family}: indexed seed is malformed")
        if entry["manifest_hash"] != manifest.get("manifest_hash"):
            raise AssertionError(f"{family}: index hash does not match manifest")
        if entry["object_count"] != len(manifest.get("catalog_object_set", [])):
            raise AssertionError(f"{family}: index object_count mismatch")
        if entry["default_row_count"] != len(manifest.get("default_row_set", [])):
            raise AssertionError(f"{family}: index default_row_count mismatch")


def validate_target_evidence(repo_root: pathlib.Path) -> None:
    target_manifest = repo_root / (
        "project/tests/donor_regression/fixtures/public_single_node_closure/"
        "artifacts/TARGET_EVIDENCE_MANIFEST.csv"
    )
    rows = read_csv(target_manifest)
    p6 = [row for row in rows if row["gap_id"] in P6_GAPS]
    if {row["gap_id"] for row in p6} != set(P6_GAPS):
        raise AssertionError("target evidence manifest does not contain every P6 gap")
    for row in p6:
        if "artifacts/P6_DONOR_COMPATIBILITY_CLOSURE_EVIDENCE.md" not in row["evidence_artifacts"]:
            raise AssertionError(f"{row['gap_id']}: P6 evidence artifact not wired")


def validate_registry_rollups(repo_root: pathlib.Path) -> None:
    registries = (
        "public_contract_snapshot",
        "public_contract_snapshot",
        "public_contract_snapshot",
        "public_contract_snapshot",
        "public_contract_snapshot",
    )
    for rel in registries:
        if rel == PUBLIC_CONTRACT_SENTINEL:
            continue
        path = require_file(repo_root, rel, "donor registry rollup")
        text = path.read_text(encoding="utf-8")
        for family in TRUE_DONORS:
            if family not in text:
                raise AssertionError(f"{rel}: missing family {family}")


def validate_profiles(repo_root: pathlib.Path) -> list[dict[str, str]]:
    rows = load_profiles(repo_root)
    families = [row["family_id"] for row in rows]
    if set(RELEASE_PROFILE_VARIANTS) & set(TRUE_DONORS):
        raise AssertionError("release/profile variants must not be listed as true donors")
    expected = list(EXPECTED_COMPATIBILITY_PROFILE_ROWS)
    if families != expected:
        raise AssertionError(f"compatibility profile rows are not canonical: {families}")
    gaps = {row["gap_id"] for row in rows}
    expected_family_gaps = {
        *(f"SB-PUBLIC-GAP-{gap:04d}" for gap in range(101, 129)),
        "SB-PUBLIC-GAP-0113-SQL-PPL",
    }
    if gaps != expected_family_gaps:
        raise AssertionError("donor family/capref profile gaps do not match 0101-0128")
    for row in rows:
        if row["profile_class"] == "donor_emulation":
            validate_true_donor(repo_root, row)
        elif row["profile_class"] == "release_profile_variant":
            validate_profile_variant(repo_root, row)
        elif row["profile_class"] == "capability_reference":
            validate_capability_reference(repo_root, row)
        else:
            raise AssertionError(f"{row['family_id']}: unknown profile class")
    validate_udr_policy(rows, load_udr_policies(repo_root))
    return rows


def run_core(repo_root: pathlib.Path) -> None:
    rows = validate_profiles(repo_root)
    validate_seed_index(repo_root, rows)
    validate_registry_rollups(repo_root)
    if len(set(CORE_GAPS)) != 12:
        raise AssertionError("P6 core gap registry must cover SB-PUBLIC-GAP-0089 through 0100")


def run_seed(repo_root: pathlib.Path) -> None:
    rows = validate_profiles(repo_root)
    validate_seed_index(repo_root, rows)


def run_surface(repo_root: pathlib.Path) -> None:
    rows = validate_profiles(repo_root)
    for row in rows:
        if row["profile_class"] != "donor_emulation":
            continue
        for key in (
            "datatype_profile",
            "index_profile",
            "diagnostic_profile",
            "metadata_overlay_profile",
        ):
            if not row[key] or row[key] in {"none", "forbidden"}:
                raise AssertionError(f"{row['family_id']}: missing {key}")


def run_runtime(repo_root: pathlib.Path) -> None:
    rows = validate_profiles(repo_root)
    for row in rows:
        if row["profile_class"] != "donor_emulation":
            continue
        for key in (
            "migration_cdc_profile",
            "sandbox_bridge_profile",
            "wire_profile",
            "builtin_surface_profile",
            "udr_bridge_profile",
        ):
            if not row[key] or row[key] in {"none", "forbidden"}:
                raise AssertionError(f"{row['family_id']}: missing {key}")
        validate_release_packet(repo_root, row)


def run_family(repo_root: pathlib.Path, batch: str) -> None:
    rows = {row["family_id"]: row for row in validate_profiles(repo_root)}
    for family in FAMILY_BATCHES[batch]:
        row = rows[family]
        if row["batch"] != batch:
            raise AssertionError(f"{family}: expected batch {batch}, got {row['batch']}")
        validate_true_donor(repo_root, row)


def run_capref(repo_root: pathlib.Path) -> None:
    rows = {row["family_id"]: row for row in validate_profiles(repo_root)}
    capref_registry_rel = PUBLIC_CONTRACT_SENTINEL
    if capref_registry_rel == PUBLIC_CONTRACT_SENTINEL:
        text = ""
    else:
        capref_registry = require_file(repo_root, capref_registry_rel, "capref registry")
        text = capref_registry.read_text(encoding="utf-8")
    for family in CAPABILITY_REFERENCE_FAMILIES:
        validate_capability_reference(repo_root, rows[family])
        if text and family not in text:
            raise AssertionError(f"capref registry missing {family}")


def run_original_regression(repo_root: pathlib.Path) -> None:
    rows = validate_profiles(repo_root)
    for row in rows:
        if row["profile_class"] == "donor_emulation":
            validate_release_packet(repo_root, row)


def run_targets(repo_root: pathlib.Path) -> None:
    validate_target_evidence(repo_root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument(
        "mode",
        choices=(
            "core",
            "seed",
            "surface",
            "runtime",
            "relational",
            "analytic",
            "nosql",
            "distributed",
            "capref",
            "original-regression",
            "target-evidence",
        ),
    )
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()

    dispatch = {
        "core": run_core,
        "seed": run_seed,
        "surface": run_surface,
        "runtime": run_runtime,
        "relational": lambda root: run_family(root, "relational"),
        "analytic": lambda root: run_family(root, "analytic"),
        "nosql": lambda root: run_family(root, "nosql"),
        "distributed": lambda root: run_family(root, "distributed"),
        "capref": run_capref,
        "original-regression": run_original_regression,
        "target-evidence": run_targets,
    }

    try:
        dispatch[args.mode](repo_root)
    except AssertionError as exc:
        print(f"P6_DONOR_COMPATIBILITY_GATE=failed: {exc}", file=sys.stderr)
        return 1

    digest = hashlib.sha256(args.mode.encode("utf-8")).hexdigest()[:12]
    print(f"P6_DONOR_COMPATIBILITY_GATE=passed mode={args.mode} evidence={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
