#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate and emit public cluster catalog manifest proof artifacts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any, Iterable


# CLUSTER_CATALOG_MANIFEST_GENERATOR

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)

REQUIRED_POLICY = {
    "engine_owned": True,
    "uuid_only_identity": True,
    "external_provider_required": True,
    "local_runtime_execution_enabled": False,
    "mutable_by_local_core": False,
    "transaction_inventory_remains_finality_authority": True,
    "resolver_rows_own_names": True,
    "comment_rows_own_comments": True,
    "property_bags_allowed": False,
    "untyped_json_allowed": False,
}

CATALOG_COMMON_SOURCE_TOKENS = (
    "BuiltinClusterCatalogTableManifests",
    "BuiltinClusterRoleProfileManifests",
    "ValidateClusterCatalogManifestSet",
    "ValidateClusterCatalogTableManifest",
    "SB-CLUSTER-CATALOG-MANIFEST-NAME-COLUMN-REFUSED",
    "SB-CLUSTER-CATALOG-MANIFEST-PROPERTY-BAG-REFUSED",
)

PROJECTION_SOURCE_TOKENS = (
    "BuiltinClusterCacheProjectionManifests",
    "ProjectionFor",
    "BuiltinClusterCatalogTableManifests",
    "BuiltinClusterRoleProfileManifests",
    "source_authority_epoch",
    "source_generation",
    "source_digest",
    "invalidation_epoch",
    "freshness_epoch_millis",
    "projection_generated_epoch_millis",
    "SB-CLUSTER-CACHE-PROJECTION-AUTHORITY-REFUSED",
)

DESCRIPTOR_SOURCE_TOKENS = (
    "BuiltinClusterDescriptorManifests",
    "ValidateClusterDescriptorManifestSet",
    "transaction_inventory_remains_finality_authority",
    "authority_provenance_uuid",
    "provider_record_digest",
)

CODEC_SOURCE_TOKENS = (
    "kClusterRecordMagic",
    "kClusterCatalogRecordCodecVersionCurrent",
    "kClusterCatalogRecordMaxPayloadBytes",
    "ValidateClusterCatalogRecordSet",
    "ClusterCatalogRecordPrimaryUuidValue",
)

MATRIX_FIELDS = (
    "category",
    "item_id",
    "source_path",
    "stable_id",
    "column_or_label_count",
    "gate",
    "status",
)


def fail(message: str) -> None:
    print(f"cluster_catalog_manifest_generator=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def load_text(path: Path, project_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{path.relative_to(project_root).as_posix()}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def iter_strings(value: Any) -> Iterable[str]:
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for key, nested in value.items():
            yield str(key)
            yield from iter_strings(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from iter_strings(nested)


def table_path(table: dict[str, Any]) -> str:
    return f"{table['schema_path']}.{table['table_name']}"


def role_table_path(role: dict[str, Any]) -> str:
    return f"cluster.sys.catalog.node_role_profile_{role['role_code']}"


def column_names(columns: list[dict[str, Any]]) -> list[str]:
    return [str(column["name"]) for column in columns]


def reject_user_layer_columns(columns: list[dict[str, Any]], context: str) -> None:
    for column in columns:
        name = str(column["name"]).lower()
        type_name = str(column["type"]).lower()
        if "name" in name or "comment" in name or "description" in name:
            fail(f"user_layer_text_column:{context}:{column['name']}")
        if type_name in {"property_bag", "json", "jsonb"}:
            fail(f"untyped_property_column:{context}:{column['name']}")


def validate_column_inventory(columns: list[dict[str, Any]], context: str) -> None:
    names = column_names(columns)
    if len(names) != len(set(names)):
        fail(f"duplicate_column:{context}")
    if "status" not in names:
        fail(f"status_column_missing:{context}")
    reject_user_layer_columns(columns, context)
    if not any(bool(column.get("uuid_identity")) for column in columns):
        fail(f"uuid_identity_column_missing:{context}")


def validate_manifest(manifest: dict[str, Any]) -> None:
    if manifest.get("schema_version") != 1:
        fail("manifest_schema_version_unsupported")
    if manifest.get("manifest_version") != 1:
        fail("cluster_manifest_version_unsupported")
    if manifest.get("manifest_id") != "sb.cluster_catalog.public_source.v1":
        fail("manifest_id_mismatch")

    for text in iter_strings(manifest):
        reject_private_reference(text, "manifest")

    policy = manifest.get("policy", {})
    for key, expected in REQUIRED_POLICY.items():
        if policy.get(key) is not expected:
            fail(f"policy_mismatch:{key}")
    if policy.get("cluster_production_execution") != "external_provider_only":
        fail("cluster_production_execution_policy_mismatch")

    base_tables = manifest.get("base_tables", [])
    if len(base_tables) != 11:
        fail("base_table_count_mismatch")
    table_paths = {table_path(table) for table in base_tables}
    if len(table_paths) != len(base_tables):
        fail("duplicate_base_table_path")
    for table in base_tables:
        validate_column_inventory(table["columns"], table_path(table))
        if len(table.get("primary_key", [])) != 1:
            fail(f"primary_key_count_invalid:{table_path(table)}")
        if table["primary_key"][0] not in column_names(table["columns"]):
            fail(f"primary_key_column_missing:{table_path(table)}")
        if "provider_record_digest" not in column_names(table["columns"]):
            fail(f"provider_digest_missing:{table_path(table)}")

    common_role_columns = manifest.get("role_profile_common_columns", [])
    validate_column_inventory(common_role_columns, "role_profile_common")
    role_profiles = manifest.get("role_profiles", [])
    if len(role_profiles) != 8:
        fail("role_profile_count_mismatch")
    role_codes = [str(role["role_code"]) for role in role_profiles]
    if len(role_codes) != len(set(role_codes)):
        fail("duplicate_role_profile")
    for role in role_profiles:
        reject_user_layer_columns(role["columns"], role_table_path(role))
        if len(role["columns"]) < 3:
            fail(f"role_profile_specific_columns_incomplete:{role['role_code']}")

    projection = manifest.get("projection_policy", {})
    if projection.get("schema_path") != "sys.catalog.cluster_cache":
        fail("projection_schema_path_mismatch")
    if projection.get("source_sets") != ["base_tables", "role_profiles"]:
        fail("projection_source_sets_mismatch")
    if projection.get("projection_only") is not True:
        fail("projection_only_policy_mismatch")
    if projection.get("source_authority_required") is not True:
        fail("projection_source_authority_policy_mismatch")
    if projection.get("cluster_authority") is not False:
        fail("projection_authority_policy_mismatch")
    for required in (
        "projection_uuid",
        "source_record_uuid",
        "source_authority_epoch",
        "source_generation",
        "source_digest",
        "invalidation_epoch",
        "freshness_epoch_millis",
        "status",
    ):
        if required not in projection.get("required_columns", []):
            fail(f"projection_required_column_missing:{required}")

    descriptors = manifest.get("descriptors", [])
    if len(descriptors) != 13:
        fail("descriptor_count_mismatch")
    descriptor_codes = [str(descriptor["code"]) for descriptor in descriptors]
    if len(descriptor_codes) != len(set(descriptor_codes)):
        fail("duplicate_descriptor_code")
    required_categories = {
        "decision",
        "route",
        "fence",
        "topology",
        "cleanup",
        "security",
        "metrics",
        "authority_provenance",
    }
    if {str(row["category"]) for row in descriptors} != required_categories:
        fail("descriptor_category_coverage_mismatch")

    metrics = manifest.get("metrics", [])
    if len(metrics) != 13:
        fail("metric_family_count_mismatch")
    metric_families = [str(metric["family"]) for metric in metrics]
    if len(metric_families) != len(set(metric_families)):
        fail("duplicate_metric_family")
    for metric in metrics:
        if not str(metric["namespace_path"]).startswith("cluster.sys.metrics"):
            fail(f"metric_namespace_mismatch:{metric['family']}")

    codec = manifest.get("codec", {})
    if codec.get("record_magic") != "SBCCAT01":
        fail("codec_magic_mismatch")
    if codec.get("current_version") != 1 or codec.get("min_supported_version") != 1:
        fail("codec_version_mismatch")
    if codec.get("max_supported_version") != 1:
        fail("codec_max_version_mismatch")
    if codec.get("max_payload_bytes") != 65536:
        fail("codec_max_payload_mismatch")
    if len(codec.get("required_diagnostics", [])) < 6:
        fail("codec_required_diagnostics_incomplete")


def check_cmake_tests(project_root: Path,
                      manifest: dict[str, Any]) -> list[dict[str, str]]:
    cmake_path = project_root / "tests" / "release" / "CMakeLists.txt"
    cmake_text = load_text(cmake_path, project_root)
    records: list[dict[str, str]] = []
    for test in manifest["public_tests"]:
        name = str(test["name"])
        gate = str(test["gate"])
        require_contains(cmake_text, f"NAME {name}", f"release_cmake:{name}")
        require_contains(cmake_text, gate, f"release_cmake:{name}")
        require_contains(cmake_text, "public_release_correctness",
                         f"release_cmake:{name}")
        records.append({
            "category": "ctest",
            "item_id": name,
            "source_path": "tests/release/CMakeLists.txt",
            "stable_id": name,
            "column_or_label_count": "0",
            "gate": gate,
            "status": "validated",
        })
    require_contains(cmake_text,
                     "public_cluster_manifest_source_gate",
                     "release_cmake:pcr099_gate")
    return records


def check_catalog_sources(project_root: Path,
                          manifest: dict[str, Any]) -> list[dict[str, str]]:
    catalog_path = project_root / "src" / "core" / "catalog" / "cluster_catalog_manifest.cpp"
    catalog_text = load_text(catalog_path, project_root)
    for token in CATALOG_COMMON_SOURCE_TOKENS:
        require_contains(catalog_text, token, "cluster_catalog_manifest")

    records: list[dict[str, str]] = []
    for table in manifest["base_tables"]:
        source_id = table_path(table)
        for token in (
            table["schema_path"],
            table["table_name"],
            table["stable_table_id"],
            table["record_family"],
            table["primary_key"][0],
        ):
            require_contains(catalog_text, str(token), source_id)
        for column in table["columns"]:
            require_contains(catalog_text, str(column["name"]), source_id)
            require_contains(catalog_text, str(column["type"]), source_id)
        records.append({
            "category": "base_table",
            "item_id": source_id,
            "source_path": "src/core/catalog/cluster_catalog_manifest.cpp",
            "stable_id": str(table["stable_table_id"]),
            "column_or_label_count": str(len(table["columns"])),
            "gate": "PCR-GATE-093",
            "status": "validated",
        })

    for role in manifest["role_profiles"]:
        source_id = role_table_path(role)
        require_contains(catalog_text,
                         f"RoleProfile(\"{role['role_code']}\"",
                         source_id)
        for column in manifest["role_profile_common_columns"] + role["columns"]:
            require_contains(catalog_text, str(column["name"]), source_id)
        records.append({
            "category": "role_profile",
            "item_id": source_id,
            "source_path": "src/core/catalog/cluster_catalog_manifest.cpp",
            "stable_id": f"cluster_catalog.node_role_profile.{role['role_code']}",
            "column_or_label_count": str(
                len(manifest["role_profile_common_columns"]) + len(role["columns"])
            ),
            "gate": "PCR-GATE-093",
            "status": "validated",
        })

    return records


def check_projection_sources(project_root: Path,
                             manifest: dict[str, Any]) -> list[dict[str, str]]:
    schema_path = project_root / "src" / "core" / "catalog" / "cluster_schema_gating.cpp"
    schema_text = load_text(schema_path, project_root)
    for token in PROJECTION_SOURCE_TOKENS:
        require_contains(schema_text, token, "cluster_schema_gating")
    for column in manifest["projection_policy"]["required_columns"]:
        require_contains(schema_text, str(column), "cluster_cache_projection")

    source_count = len(manifest["base_tables"]) + len(manifest["role_profiles"])
    return [{
        "category": "projection_policy",
        "item_id": "sys.catalog.cluster_cache",
        "source_path": "src/core/catalog/cluster_schema_gating.cpp",
        "stable_id": "cluster_cache.projection_policy",
        "column_or_label_count": str(
            len(manifest["projection_policy"]["required_columns"])
        ),
        "gate": "PCR-GATE-094",
        "status": f"validated_sources={source_count}",
    }]


def check_codec_sources(project_root: Path,
                        manifest: dict[str, Any]) -> list[dict[str, str]]:
    codec_path = project_root / "src" / "core" / "catalog" / "cluster_catalog_record_codec.cpp"
    codec_text = load_text(codec_path, project_root)
    codec_header_path = project_root / "src" / "core" / "catalog" / "cluster_catalog_record_codec.hpp"
    codec_header_text = load_text(codec_header_path, project_root)
    codec_combined_text = codec_header_text + "\n" + codec_text
    for token in CODEC_SOURCE_TOKENS:
        require_contains(codec_combined_text, token, "cluster_catalog_record_codec")
    for char in manifest["codec"]["record_magic"]:
        require_contains(codec_text, f"'{char}'", "cluster_record_magic")
    for diagnostic in manifest["codec"]["required_diagnostics"]:
        require_contains(codec_text, str(diagnostic), "cluster_record_codec")
    return [{
        "category": "codec",
        "item_id": "cluster_catalog_record_codec",
        "source_path": "src/core/catalog/cluster_catalog_record_codec.cpp",
        "stable_id": "SBCCAT01:v1",
        "column_or_label_count": str(len(manifest["codec"]["required_diagnostics"])),
        "gate": "PCR-GATE-095",
        "status": "validated",
    }]


def check_descriptor_sources(project_root: Path,
                             manifest: dict[str, Any]) -> list[dict[str, str]]:
    descriptor_path = project_root / "src" / "core" / "catalog" / "cluster_descriptor_manifest.cpp"
    descriptor_text = load_text(descriptor_path, project_root)
    for token in DESCRIPTOR_SOURCE_TOKENS:
        require_contains(descriptor_text, token, "cluster_descriptor_manifest")

    records: list[dict[str, str]] = []
    for descriptor in manifest["descriptors"]:
        source_id = str(descriptor["code"])
        require_contains(descriptor_text,
                         f"Descriptor(\"{source_id}\"",
                         f"descriptor:{source_id}")
        require_contains(descriptor_text,
                         f"ClusterDescriptorCategory::{descriptor['category']}",
                         f"descriptor:{source_id}")
        require_contains(descriptor_text,
                         str(descriptor["table_name"]),
                         f"descriptor:{source_id}")
        for column in descriptor["primary_key"] + descriptor["columns"]:
            require_contains(descriptor_text, str(column), f"descriptor:{source_id}")
        records.append({
            "category": "descriptor",
            "item_id": source_id,
            "source_path": "src/core/catalog/cluster_descriptor_manifest.cpp",
            "stable_id": f"cluster_descriptor.{source_id}",
            "column_or_label_count": str(
                len(descriptor["primary_key"]) + len(descriptor["columns"]) + 8
            ),
            "gate": "PCR-GATE-096",
            "status": "validated",
        })
    return records


def check_metric_sources(project_root: Path,
                         manifest: dict[str, Any]) -> list[dict[str, str]]:
    metric_path = project_root / "src" / "core" / "metrics" / "cluster_metric_descriptors.cpp"
    metric_text = load_text(metric_path, project_root)
    for token in (
        "RequiredClusterMetricFamilies",
        "BuiltinClusterMetricDescriptorManifests",
        "contract_ready_unwired",
        "external_cluster_provider",
    ):
        require_contains(metric_text, token, "cluster_metric_descriptors")

    records: list[dict[str, str]] = []
    for label in manifest["metric_common_labels"]:
        require_contains(metric_text, str(label), "cluster_metric_common_labels")
    for metric in manifest["metrics"]:
        source_id = str(metric["family"])
        require_contains(metric_text, source_id, f"metric:{source_id}")
        require_contains(metric_text,
                         str(metric["namespace_path"]),
                         f"metric:{source_id}")
        for label in metric["labels"]:
            require_contains(metric_text, str(label), f"metric:{source_id}")
        records.append({
            "category": "metric",
            "item_id": source_id,
            "source_path": "src/core/metrics/cluster_metric_descriptors.cpp",
            "stable_id": source_id,
            "column_or_label_count": str(
                len(manifest["metric_common_labels"]) + len(metric["labels"])
            ),
            "gate": "PCR-GATE-096",
            "status": "validated",
        })
    return records


def check_manifest_source_tooling(project_root: Path,
                                  manifest: dict[str, Any]) -> list[dict[str, str]]:
    source_gate = project_root / "tests" / "release" / "public_cluster_manifest_source_gate.py"
    gate_text = load_text(source_gate, project_root)
    require_contains(gate_text,
                     "PUBLIC_CLUSTER_MANIFEST_SOURCE_GATE",
                     "public_cluster_manifest_source_gate")
    require_contains(gate_text,
                     "cluster_catalog_manifest_generator.py",
                     "public_cluster_manifest_source_gate")
    tool_text = load_text(Path(__file__), project_root)
    require_contains(tool_text,
                     "CLUSTER_CATALOG_MANIFEST_GENERATOR",
                     "cluster_catalog_manifest_generator")

    records: list[dict[str, str]] = []
    for output in manifest["generated_outputs"]:
        records.append({
            "category": "generated_output",
            "item_id": str(output),
            "source_path": "tools/release/cluster_catalog_manifest_generator.py",
            "stable_id": str(output),
            "column_or_label_count": "0",
            "gate": "PCR-GATE-099",
            "status": "generated",
        })
    return records


def write_matrix(output_dir: Path, records: list[dict[str, str]]) -> Path:
    path = output_dir / "cluster_catalog_manifest_matrix.csv"
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=MATRIX_FIELDS)
        writer.writeheader()
        for record in records:
            writer.writerow(record)
    return path


def write_readable(output_dir: Path,
                   manifest: dict[str, Any],
                   records: list[dict[str, str]]) -> Path:
    path = output_dir / "cluster_catalog_manifest_readable.md"
    counts: dict[str, int] = {}
    for record in records:
        counts[record["category"]] = counts.get(record["category"], 0) + 1
    lines = [
        "# Cluster Catalog Manifest Source Proof",
        "",
        f"Manifest: `{manifest['manifest_id']}`",
        "",
        "| Category | Count |",
        "| --- | ---: |",
    ]
    for category in sorted(counts):
        lines.append(f"| {category} | {counts[category]} |")
    lines.extend([
        "",
        "Policy:",
        "",
        "- Cluster production execution remains external-provider-only.",
        "- Local catalog shapes are UUID-identity, engine-owned, and non-mutating.",
        "- Resolver and comment rows hold presentation text outside base records.",
        "- MGA transaction inventory remains finality authority.",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def write_evidence(output_dir: Path,
                   manifest: dict[str, Any],
                   records: list[dict[str, str]],
                   manifest_text: str) -> Path:
    path = output_dir / "cluster_catalog_manifest_source_proof.json"
    payload = {
        "schema_version": 1,
        "gate": "PCR-GATE-099",
        "manifest_id": manifest["manifest_id"],
        "manifest_sha256": sha256_text(manifest_text),
        "record_count": len(records),
        "categories": sorted({record["category"] for record in records}),
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "cluster_production_execution": "external_provider_only",
            "mga_transaction_inventory_authority_preserved": True,
        },
        "records": records,
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    payload["evidence_sha256"] = sha256_text(encoded)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    return path


def build_records(project_root: Path,
                  manifest: dict[str, Any]) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    records.extend(check_catalog_sources(project_root, manifest))
    records.extend(check_projection_sources(project_root, manifest))
    records.extend(check_codec_sources(project_root, manifest))
    records.extend(check_descriptor_sources(project_root, manifest))
    records.extend(check_metric_sources(project_root, manifest))
    records.extend(check_cmake_tests(project_root, manifest))
    records.extend(check_manifest_source_tooling(project_root, manifest))
    return records


def validate_roots(project_root: Path, build_root: Path, output_dir: Path) -> None:
    if not project_root.is_dir() or project_root.name != "project":
        fail("project_root_must_be_project_directory")
    if not build_root.is_dir():
        fail("build_root_missing")
    try:
        output_record = output_dir.resolve().relative_to(build_root.resolve()).as_posix()
    except ValueError:
        fail("output_dir_must_be_under_build_root")
    reject_private_reference(output_record, "output_dir")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output_dir = args.output_dir.resolve()
    validate_roots(project_root, build_root, output_dir)
    manifest_path = args.manifest.resolve()
    if not manifest_path.is_file():
        fail("manifest_missing")
    try:
        manifest_relative = manifest_path.relative_to(project_root).as_posix()
    except ValueError:
        fail("manifest_must_be_under_project_root")
    reject_private_reference(manifest_relative, "manifest_path")

    manifest_text = manifest_path.read_text(encoding="utf-8")
    manifest = json.loads(manifest_text)
    validate_manifest(manifest)
    records = build_records(project_root, manifest)
    if not args.check_only:
        output_dir.mkdir(parents=True, exist_ok=True)
        matrix_path = write_matrix(output_dir, records)
        evidence_path = write_evidence(output_dir, manifest, records, manifest_text)
        readable_path = write_readable(output_dir, manifest, records)
        print(f"cluster_catalog_manifest_matrix={matrix_path.relative_to(build_root).as_posix()}")
        print(f"cluster_catalog_manifest_evidence={evidence_path.relative_to(build_root).as_posix()}")
        print(f"cluster_catalog_manifest_readable={readable_path.relative_to(build_root).as_posix()}")
    print(f"cluster_catalog_manifest_generator_records={len(records)}")
    print("cluster_catalog_manifest_generator=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
