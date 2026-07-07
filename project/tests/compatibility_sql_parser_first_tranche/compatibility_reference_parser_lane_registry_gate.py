#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reference parser lane registry proof gate.

This gate validates the public implementation tree for every reference parser
lane. It deliberately does not read private tracking packets; it proves only
public source/test assets that must exist before a lane can claim implementation
progress.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


IMPLEMENTATION_LANES = {
    "apache_ignite",
    "cassandra",
    "clickhouse",
    "cockroachdb",
    "dolt",
    "duckdb",
    "firebird",
    "foundationdb",
    "immudb",
    "influxdb",
    "mariadb",
    "milvus",
    "mongodb",
    "mysql",
    "neo4j",
    "opensearch",
    "opensearch_sql_ppl",
    "postgresql",
    "redis",
    "sqlite",
    "tidb",
    "tikv",
    "vitess",
    "xtdb",
    "yugabytedb",
}

CAPABILITY_REFERENCE_ONLY = {"db2", "oracle", "sqlserver"}

SPECIAL_TEST_ROOTS = {
    "opensearch": "project/tests/reference_regression/opensearch/rest_dsl",
    "opensearch_sql_ppl": "project/tests/reference_regression/opensearch/sql_ppl",
}

REQUIRED_REFERENCE_MANIFESTS = (
    "upstream_manifest.csv",
    "fixtures/fixture_manifest.csv",
    "goldens/golden_manifest.csv",
    "management_package_abi/management_package_abi_manifest.csv",
    "wire_transcripts/wire_transcript_manifest.csv",
    "resource_limits/resource_limit_manifest.csv",
    "release_evidence/release_evidence_manifest.csv",
    "enterprise_completion/enterprise_completion_manifest.csv",
)

REQUIRED_PROFILE_COLUMNS = {
    "family_id",
    "display_name",
    "profile_class",
    "release_profile",
    "release_regression_root",
    "parser_module",
    "seed_manifest_path",
    "authority_policy",
    "reference_sql_execution",
    "reference_storage_authority",
    "reference_recovery_authority",
    "parser_cross_dialect_dependency",
    "runtime_seed_authority",
    "capability_reference_policy",
    "required_labels",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read_manifest(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        require(reader.fieldnames is not None, f"{path}: missing CSV header")
        missing = sorted(REQUIRED_PROFILE_COLUMNS - set(reader.fieldnames))
        require(not missing, f"{path}: missing columns {missing}")
        return [dict(row) for row in reader]


def require_file(path: Path) -> None:
    require(path.is_file(), f"missing file: {path}")


def require_dir(path: Path) -> None:
    require(path.is_dir(), f"missing directory: {path}")


def verify_lane(repo_root: Path, row: dict[str, str]) -> dict[str, object]:
    lane = row["family_id"]
    parser_dir = repo_root / row["parser_module"]
    support_udr_dir = repo_root / f"project/src/udr/sbu_{lane}_parser_support"
    test_root_rel = SPECIAL_TEST_ROOTS.get(
        lane, f"project/tests/reference_regression/{lane}"
    )
    test_root = repo_root / test_root_rel

    require(row["profile_class"] == "compatibility_emulation",
            f"{lane}: expected compatibility_emulation profile")
    require(row["authority_policy"] == "engine_sblr_mga_only",
            f"{lane}: authority policy drift")
    for authority_column in (
        "reference_sql_execution",
        "reference_storage_authority",
        "reference_recovery_authority",
        "parser_cross_dialect_dependency",
    ):
        require(row[authority_column] == "false",
                f"{lane}: {authority_column} must be false")
    require(row["runtime_seed_authority"] == "reference_catalog_seed_manifest",
            f"{lane}: runtime seed authority must be catalog seed manifest")
    require(row["capability_reference_policy"] == "not_capability_reference",
            f"{lane}: capability-reference-only policy used on implementation lane")

    require_dir(parser_dir)
    require_file(parser_dir / "CMakeLists.txt")
    require_file(parser_dir / f"{lane}_dialect.cpp")
    require_file(parser_dir / f"{lane}_dialect.hpp")
    require_file(parser_dir / "main.cpp")

    require_dir(support_udr_dir)
    require_file(support_udr_dir / "CMakeLists.txt")
    require_file(support_udr_dir / f"sbu_{lane}_parser_support.cpp")

    require_dir(test_root)
    for manifest in REQUIRED_REFERENCE_MANIFESTS:
        require_file(test_root / manifest)
    require_dir(test_root / "native_tool_harness")
    require_file(test_root / "native_tool_harness" / "native_tool_harness_manifest.csv")

    seed_manifest = repo_root / row["seed_manifest_path"]
    require_file(seed_manifest)

    parser_text = (parser_dir / f"{lane}_dialect.cpp").read_text(encoding="utf-8")
    if lane == "firebird":
        for fragment in (
            "FirebirdLifecycleMappingDescriptor",
            "FirebirdLifecycleMappings",
            "ParseStatement",
            "FirebirdPackageIdentityJson",
        ):
            require(fragment in parser_text,
                    f"{lane}: custom parser missing {fragment}")
    else:
        require("OperationPattern" in parser_text, f"{lane}: no OperationPattern table")
        require("DialectProfile" in parser_text, f"{lane}: no DialectProfile")
        require("engine_sblr_mga_only" not in parser_text,
                f"{lane}: authority policy belongs in package identity/common envelope")

    udr_text = (support_udr_dir / f"sbu_{lane}_parser_support.cpp").read_text(
        encoding="utf-8"
    )
    require("management_abi_version" in udr_text,
            f"{lane}: support UDR missing management ABI")
    require("mga_transaction_authority" in udr_text,
            f"{lane}: support UDR missing MGA authority marker")
    require("scratchbird_engine" in udr_text,
            f"{lane}: support UDR missing engine authority marker")

    labels = set(row["required_labels"].split(";"))
    required_labels = {"reference_original_regression_gate", "reference_release_regression_gate"}
    require(required_labels.issubset(labels),
            f"{lane}: required reference labels missing")

    return {
        "lane": lane,
        "display_name": row["display_name"],
        "parser_dir": row["parser_module"],
        "support_udr_dir": f"project/src/udr/sbu_{lane}_parser_support",
        "test_root": test_root_rel,
        "release_profile": row["release_profile"],
        "required_manifest_count": len(REQUIRED_REFERENCE_MANIFESTS),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    manifest_path = repo_root / "project/src/parsers/compatibility/CompatibilityProfileManifest.csv"
    rows = read_manifest(manifest_path)
    by_family = {row["family_id"]: row for row in rows}

    missing = sorted(IMPLEMENTATION_LANES - set(by_family))
    require(not missing, f"missing implementation lane rows: {missing}")
    unexpected_implementation = sorted(
        family for family, row in by_family.items()
        if row["profile_class"] == "compatibility_emulation"
        and family not in IMPLEMENTATION_LANES
    )
    require(not unexpected_implementation,
            f"unexpected implementation lanes: {unexpected_implementation}")
    for family in CAPABILITY_REFERENCE_ONLY:
        if family in by_family:
            require(by_family[family]["capability_reference_policy"] != "not_capability_reference",
                    f"{family}: capability reference row marked as implementation")

    lane_evidence = [
        verify_lane(repo_root, by_family[lane])
        for lane in sorted(IMPLEMENTATION_LANES)
    ]

    evidence = {
        "gate": "compatibility_reference_parser_lane_registry_gate",
        "status": "passed",
        "lane_count": len(lane_evidence),
        "file_presence_is_completion": False,
        "runtime_behavior_required": True,
        "engine_authority_policy": "engine_sblr_mga_only",
        "reference_execution_authority": False,
        "lanes": lane_evidence,
    }
    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"compatibility_reference_parser_lane_registry_gate=passed lanes={len(lane_evidence)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"compatibility_reference_parser_lane_registry_gate: {exc}")
        raise SystemExit(1)
