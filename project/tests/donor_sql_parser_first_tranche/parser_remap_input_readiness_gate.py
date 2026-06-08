#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P0 parser-remap input readiness proof.

This gate blocks parser remap work until upstream inventory, cluster
normalization, SBLR expansion, current donor parser packages, parser-support
UDRs, and baseline parser proof CTests are internally consistent.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import sys


EXPECTED_BASELINE_CTESTS = {
    "donor_sql_parser_first_tranche_probe",
    "sbu_donor_sql_parser_support_probe",
    "donor_sql_parser_first_tranche_manifest_gate",
    "donor_sql_parser_standalone_source_guard",
    "non_sql_donor_sblr_surface_gate",
    "donor_sql_parser_first_tranche_cli_evidence_gate",
    "donor_sql_first_tranche_original_tool_replay_gate",
}

EXPECTED_COUNTS = {
    "manifest_donor_emulation_profiles": 25,
    "manifest_capability_reference_profiles": 3,
    "actual_donor_parser_package_dirs": 25,
    "actual_parser_support_udr_package_dirs": 25,
    "inventory_noncluster_handoff_rows": 1438,
    "inventory_external_authority_rows": 380,
    "inventory_architecture_refusal_rows": 393,
    "parser_declared_surface_rows": 2078,
    "cluster_parser_intake_rows": 440,
    "normalized_cluster_provider_commands": 59,
    "sblr_p7_proof_rows": 2760,
    "sblr_p4_noncluster_route_rows": 2701,
    "sblr_p5_cluster_external_policy_rows": 439,
}

UPSTREAM_CSVS = {
    "inventory_noncluster_handoff_rows":
        "public_execution_plan"
        "DONOR_UNSUPPORTED_NONCLUSTER_SURFACE_INVENTORY.csv",
    "inventory_external_authority_rows":
        "public_execution_plan"
        "DONOR_EXTERNAL_AUTHORITY_SURFACE_INVENTORY.csv",
    "inventory_architecture_refusal_rows":
        "public_execution_plan"
        "DONOR_ARCHITECTURE_CONFLICT_REFUSAL_REGISTER.csv",
    "parser_declared_surface_rows":
        "public_execution_plan"
        "PARSER_DECLARED_SURFACE_COVERAGE_MATRIX.csv",
    "cluster_parser_intake_rows":
        "public_execution_plan"
        "CLUSTER_PARSER_REMAP_INTAKE_MATRIX.csv",
    "normalized_cluster_provider_commands":
        "public_execution_plan"
        "NORMALIZED_DONOR_CLUSTER_COMMAND_SET.csv",
    "sblr_p7_proof_rows":
        "public_execution_plan"
        "SBLR_EXECUTION_PROOF_MATRIX.csv",
    "sblr_p4_noncluster_route_rows":
        "public_execution_plan"
        "ENGINE_INTERNAL_API_ROUTE_MATRIX.csv",
    "sblr_p5_cluster_external_policy_rows":
        "public_execution_plan"
        "SBLR_CLUSTER_AND_EXTERNAL_AUTHORITY_POLICY_MATRIX.csv",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(repo_root: pathlib.Path, rel: str) -> list[dict[str, str]]:
    path = repo_root / rel
    try:
        with path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
    except FileNotFoundError:
        fail(f"missing CSV: {rel}")
    if not reader.fieldnames:
        fail(f"CSV has no header: {rel}")
    return rows


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def actual_parser_dirs(repo_root: pathlib.Path) -> set[str]:
    root = repo_root / "project/src/parsers/donor"
    return {
        path.name
        for path in root.iterdir()
        if path.is_dir() and path.name != "common"
    }


def actual_udr_dirs(repo_root: pathlib.Path) -> set[str]:
    root = repo_root / "project/src/udr"
    return {
        path.name
        for path in root.iterdir()
        if path.is_dir()
        and path.name.startswith("sbu_")
        and path.name.endswith("_parser_support")
        and path.name != "sbu_sbsql_parser_support"
    }


def tracker_has_no_pending(repo_root: pathlib.Path, execution_plan: str) -> None:
    rows = read_csv(repo_root, f"{execution_plan}/TRACKER.csv")
    pending = [row["slice_id"] for row in rows if row["status"] == "pending"]
    require(not pending, f"pending upstream tracker rows in {execution_plan}: {pending}")


def require_fpr_tracker_state(repo_root: pathlib.Path) -> None:
    rows = read_csv(
        repo_root,
        "public_execution_plan",
    )
    by_id = {row["slice_id"]: row for row in rows}
    require(
        by_id["FPR-P0"]["status"] == "p0_input_readiness_verified",
        "FPR-P0 tracker status must be readiness verified before this gate passes",
    )
    require(
        by_id["FPR-P1"]["status"] in {"pending", "p1_noncluster_remap_verified"},
        "FPR-P1 must be pending or verified after the non-cluster remap gate closes",
    )
    require(
        by_id["FPR-P2"]["status"] in {"pending", "p2_cluster_route_remap_verified"},
        "FPR-P2 must be pending or verified after the cluster route remap gate closes",
    )
    require(
        by_id["FPR-P3"]["status"] in {"pending", "p3_ast_boundast_envelope_verified"},
        "FPR-P3 must be pending or verified after the AST/BoundAST envelope gate closes",
    )
    require(
        by_id["FPR-P4"]["status"] in {"pending", "p4_refusal_reduction_verified"},
        "FPR-P4 must be pending or verified after the refusal reduction gate closes",
    )
    require(
        by_id["FPR-P5"]["status"] in {"pending", "p5_dialect_isolation_verified"},
        "FPR-P5 must be pending or verified after the dialect isolation gate closes",
    )
    require(
        by_id["FPR-P6"]["status"] in {"pending", "p6_donor_replay_proof_verified"},
        "FPR-P6 must be pending or verified after the donor replay proof gate closes",
    )
    row = by_id["FPR-P7"]
    require(row["status"] in {"pending", "p7_parser_remap_audit_verified"},
            f"{row['slice_id']} must be pending or verified after the independent audit closes")


def registered_ctests(build_root: pathlib.Path) -> set[str]:
    ctest_file = build_root / "tests/donor_sql_parser_first_tranche/CTestTestfile.cmake"
    if not ctest_file.exists():
        fail("donor SQL parser tranche CTest file is not configured")
    text = ctest_file.read_text()
    return set(re.findall(r"add_test\(\[=\[([^\]]+)\]=\]", text))


def validate_scope_matrix(repo_root: pathlib.Path) -> dict[str, int]:
    rows = read_csv(
        repo_root,
        "public_execution_plan"
        "PARSER_REMAP_SCOPE_MATRIX.csv",
    )
    required = {
        "scope_id",
        "scope_category",
        "subject_id",
        "manifest_profile_class",
        "expected_count",
        "observed_count",
        "implementation_package_path",
        "parser_support_udr_path",
        "compatibility_policy",
        "upstream_artifact",
        "readiness_gate",
        "status",
        "evidence",
    }
    require(required.issubset(rows[0]), "scope matrix missing required columns")
    for row in rows:
        for key in required:
            require(row[key], f"scope matrix empty field {key} in {row['scope_id']}")
        require(
            row["status"] == "readiness_verified",
            f"scope row is not readiness verified: {row['scope_id']}",
        )
        require(
            row["readiness_gate"] == "parser_remap_input_readiness_gate",
            f"scope row uses wrong readiness gate: {row['scope_id']}",
        )

    by_category: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_category.setdefault(row["scope_category"], []).append(row)

    require(
        len(by_category.get("manifest_profile", [])) == 29,
        "scope matrix must record 29 manifest profiles",
    )
    require(
        len(by_category.get("parser_package_dir", [])) == 25,
        "scope matrix must record 25 parser package directories",
    )
    require(
        len(by_category.get("parser_support_udr_dir", [])) == 25,
        "scope matrix must record 25 parser-support UDR directories",
    )
    require(
        len(by_category.get("upstream_input_count", [])) == len(EXPECTED_COUNTS),
        "scope matrix missing upstream count rows",
    )
    require(
        len(by_category.get("registered_baseline_ctest", [])) ==
        len(EXPECTED_BASELINE_CTESTS),
        "scope matrix missing baseline CTest rows",
    )

    alias_rows = [
        row for row in rows
        if row["scope_category"] == "profile_alias_policy"
        and row["subject_id"].startswith("mysql_lts_")
    ]
    require(len(alias_rows) == 2, "mysql_lts parser and UDR alias rows are required")
    for row in alias_rows:
        require(
            row["implementation_package_path"] == "project/src/parsers/donor/mysql",
            "mysql_lts parser alias must target mysql parser package",
        )
        require(
            row["parser_support_udr_path"] ==
            "project/src/udr/sbu_mysql_parser_support",
            "mysql_lts UDR alias must target mysql parser-support package",
        )

    opensearch_sql_ppl_rows = [
        row for row in rows
        if row["subject_id"] == "opensearch_sql_ppl"
    ]
    require(
        any(row["scope_category"] == "parser_package_dir" for row in opensearch_sql_ppl_rows),
        "opensearch_sql_ppl parser package row missing",
    )
    require(
        any(row["scope_category"] == "parser_support_udr_dir"
            for row in opensearch_sql_ppl_rows),
        "opensearch_sql_ppl UDR package row missing",
    )
    for row in opensearch_sql_ppl_rows:
        require(
            "no_opensearch_cross_dependency" in row["compatibility_policy"]
            or "standalone" in row["compatibility_policy"],
            "opensearch_sql_ppl must be standalone from opensearch parser",
        )

    apache_rows = [
        row for row in rows
        if row["subject_id"] in {"apache_ignite", "apache_ignite_path_reconciliation"}
    ]
    require(apache_rows, "Apache Ignite compatibility rows missing")
    require(
        any(row["implementation_package_path"] ==
            "project/src/parsers/donor/apache_ignite" for row in apache_rows),
        "Apache Ignite implementation path must be apache_ignite",
    )
    require(
        any("dash_name_manifest_metadata" in row["compatibility_policy"]
            for row in apache_rows),
        "Apache Ignite dash-name metadata policy missing",
    )

    category_counts = {key: len(value) for key, value in by_category.items()}
    category_counts["scope_matrix_rows"] = len(rows)
    return category_counts


def validate_manifest_and_packages(repo_root: pathlib.Path) -> dict[str, int]:
    manifest = read_csv(
        repo_root,
        "project/src/parsers/donor/DonorCompatibilityProfileManifest.csv",
    )
    donor_profiles = [row for row in manifest if row["profile_class"] == "donor_emulation"]
    capability_profiles = [
        row for row in manifest if row["profile_class"] == "capability_reference"
    ]
    require(len(donor_profiles) == 25, "manifest donor_emulation count drift")
    require(len(capability_profiles) == 3, "manifest capability_reference count drift")
    parser_dirs = actual_parser_dirs(repo_root)
    udr_dirs = actual_udr_dirs(repo_root)
    require(len(parser_dirs) == 25, "actual donor parser package count drift")
    require(len(udr_dirs) == 25, "actual parser-support UDR package count drift")
    require("mysql_lts" not in parser_dirs, "mysql_lts must not be a separate parser dir")
    require(
        "sbu_mysql_lts_parser_support" not in udr_dirs,
        "mysql_lts must not be a separate parser-support UDR dir",
    )
    require("mysql" in parser_dirs, "mysql parser package missing")
    require("sbu_mysql_parser_support" in udr_dirs, "mysql parser-support UDR missing")
    require("opensearch_sql_ppl" in parser_dirs, "opensearch_sql_ppl parser dir missing")
    require("opensearch" in parser_dirs, "opensearch parser dir missing")
    require(
        "sbu_opensearch_sql_ppl_parser_support" in udr_dirs,
        "opensearch_sql_ppl parser-support UDR missing",
    )
    require(
        "sbu_opensearch_parser_support" in udr_dirs,
        "opensearch parser-support UDR missing",
    )
    require("apache_ignite" in parser_dirs, "apache_ignite parser dir missing")
    require(
        "apache-ignite" not in parser_dirs,
        "apache-ignite dash-name parser dir must not be used",
    )
    require(
        "sbu_apache_ignite_parser_support" in udr_dirs,
        "apache_ignite parser-support UDR missing",
    )
    for row in manifest:
        fid = row["family_id"]
        if row["profile_class"] == "capability_reference":
            require(row["parser_module"] == "forbidden", f"{fid} parser_module must be forbidden")
            continue
        if fid == "mysql_lts":
            require(
                row["profile_class"] == "release_profile_variant",
                "mysql_lts manifest row must be release/profile metadata, not donor_emulation",
            )
            require(
                row["parser_module"] == "project/src/parsers/donor/mysql",
                "mysql_lts release/profile metadata must map to mysql parser package",
            )
            require(
                row["seed_manifest_family"] == "mysql",
                "mysql_lts release/profile metadata must use mysql seed manifest family",
            )
            require(
                row["seed_manifest_path"] ==
                "project/tests/donor_regression/donor_catalog_seeds/mysql_lts/mysql_lts_beta2_full_seed_manifest.yaml",
                "mysql_lts profile-derived seed evidence path changed unexpectedly",
            )
        elif fid == "apache_ignite":
            require(
                row["parser_module"] == "project/src/parsers/donor/apache-ignite",
                "Apache Ignite manifest dash-name metadata changed unexpectedly",
            )
        else:
            require(fid in parser_dirs, f"manifest donor profile lacks parser dir: {fid}")
    return {
        "manifest_donor_emulation_profiles": len(donor_profiles),
        "manifest_capability_reference_profiles": len(capability_profiles),
        "actual_donor_parser_package_dirs": len(parser_dirs),
        "actual_parser_support_udr_package_dirs": len(udr_dirs),
    }


def validate_upstream_counts(repo_root: pathlib.Path) -> dict[str, int]:
    observed: dict[str, int] = {}
    for key, rel in UPSTREAM_CSVS.items():
        observed[key] = len(read_csv(repo_root, rel))
        require(
            observed[key] == EXPECTED_COUNTS[key],
            f"{key} count drift: expected {EXPECTED_COUNTS[key]} observed {observed[key]}",
        )
    return observed


def validate_scope_count_rows(
    repo_root: pathlib.Path,
    expected_observed: dict[str, int],
) -> None:
    rows = read_csv(
        repo_root,
        "public_execution_plan"
        "PARSER_REMAP_SCOPE_MATRIX.csv",
    )
    count_rows = {
        row["subject_id"]: row
        for row in rows
        if row["scope_category"] == "upstream_input_count"
    }
    for key, value in expected_observed.items():
        require(key in count_rows, f"scope matrix missing count row: {key}")
        row = count_rows[key]
        require(int(row["expected_count"]) == value, f"scope expected count drift: {key}")
        require(int(row["observed_count"]) == value, f"scope observed count drift: {key}")


def validate_registered_tests(repo_root: pathlib.Path, build_root: pathlib.Path) -> None:
    registered = registered_ctests(build_root)
    missing = sorted(EXPECTED_BASELINE_CTESTS - registered)
    require(not missing, f"baseline parser CTests are not registered: {missing}")
    scope_rows = read_csv(
        repo_root,
        "public_execution_plan"
        "PARSER_REMAP_SCOPE_MATRIX.csv",
    )
    matrix_tests = {
        row["subject_id"]
        for row in scope_rows
        if row["scope_category"] == "registered_baseline_ctest"
    }
    require(
        EXPECTED_BASELINE_CTESTS == matrix_tests,
        "scope matrix baseline CTest rows do not match required set",
    )


def write_evidence(
    evidence_file: pathlib.Path,
    category_counts: dict[str, int],
    observed_counts: dict[str, int],
) -> None:
    evidence_file.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "parser_remap_input_readiness_gate",
        "authority_note": "diagnostic_output_only_not_source_authority",
        "category_counts": category_counts,
        "observed_counts": observed_counts,
        "baseline_ctests": sorted(EXPECTED_BASELINE_CTESTS),
    }
    evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    evidence_file = pathlib.Path(args.evidence_file)

    category_counts = validate_scope_matrix(repo_root)
    manifest_counts = validate_manifest_and_packages(repo_root)
    upstream_counts = validate_upstream_counts(repo_root)
    observed_counts = {**manifest_counts, **upstream_counts}
    validate_scope_count_rows(repo_root, observed_counts)
    tracker_has_no_pending(
        repo_root,
        "public_execution_plan",
    )
    tracker_has_no_pending(
        repo_root,
        "public_execution_plan",
    )
    tracker_has_no_pending(
        repo_root,
        "public_execution_plan",
    )
    require_fpr_tracker_state(repo_root)
    validate_registered_tests(repo_root, build_root)
    write_evidence(evidence_file, category_counts, observed_counts)
    print("parser_remap_input_readiness_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
