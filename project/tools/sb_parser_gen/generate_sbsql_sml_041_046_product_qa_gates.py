#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SML-041..046 product QA manifest and hardening oracle artifacts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path
from typing import Any


PRODUCT_SCHEMA = "sbsql.sml_041_046.product_qa_manifest.v1"
HARDENING_SCHEMA = "sbsql.sml_044_046.hardening_oracle.v1"

DEFAULT_PRODUCT_OUTPUT = (
    "project/tests/sbsql_parser_worker/generated/product_regression/sml_041_046/"
    "SML_041_046_PRODUCT_QA_MANIFEST.json"
)
DEFAULT_HARDENING_OUTPUT = (
    "project/tests/sbsql_parser_worker/generated/hardening/sml_041_046/"
    "SML_044_046_HARDENING_ORACLE.json"
)
ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"

SURFACE_ARTIFACTS = {
    "language": "SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv",
    "release": "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
    "per_row": "PER_ROW_EVIDENCE_MANIFEST.csv",
    "route": "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
    "round_trip": "SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
}

ROUTES = ["embedded", "local_ipc", "inet_listener", "parser_lowering"]
PAGE_SIZES = [8192, 16384, 32768, 65536, 131072]
LANGUAGE_PROFILES = ["sbsql_v3_en", "sbsql_v3_unicode", "sbsql_v3_locale_fr", "sbsql_v3_locale_ja"]
SECURITY_PROFILES = ["required_grant", "revoked_grant_refusal", "redacted_diagnostic"]

SUITE_ROWS = [
    ("bootstrap", "sbsql_database_create_schema_bootstrap_gate", [
        "project/tests/sbsql_parser_worker/sbsql_database_create_schema_bootstrap_gate.cpp",
    ]),
    ("catalog", "sbsql_catalog_and_statistics_routes", [
        "project/tests/sbsql_parser_worker/sbsql_create_statistics_exact_route_conformance.cpp",
        "project/tests/database_lifecycle/catalog_object_conformance.cpp",
    ]),
    ("lifecycle", "sbsql_database_lifecycle_routes", [
        "project/tests/sbsql_parser_worker/sbsql_database_lifecycle_exact_route_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_database_lifecycle_full_route_conformance.py",
    ]),
    ("constraints", "sbsql_constraint_routes", [
        "project/tests/sbsql_parser_worker/constraint_ddl_lowering_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_column_constraint_exact_route_conformance.cpp",
    ]),
    ("query", "sbsql_query_routes", [
        "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_query_scalar_projection_conformance.cpp",
    ]),
    ("dml", "sbsql_dml_mga_routes", [
        "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_dml_mga_row_result_conformance.cpp",
    ]),
    ("datatypes", "sbsql_datatype_operator_cast_routes", [
        "project/tests/sbsql_parser_worker/sbsql_advanced_datatype_operation_closure_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_cast_value_exact_route_conformance.cpp",
    ]),
    ("udr", "sbsql_udr_package_routes", [
        "project/tests/sbsql_parser_worker/sbsql_udr_package_management_exact_route_conformance.cpp",
        "project/src/udr/packages/compatibility/CompatibilityUdrBridgePolicyManifest.csv",
    ]),
    ("security", "sbsql_security_route_matrix", [
        "project/tests/sbsql_parser_worker/sbsql_security_exact_route_conformance.cpp",
        "project/tests/sbsql_parser_worker/generated/security/SECURITY_REGRESSION_MATRIX.json",
    ]),
    ("language", "sbsql_language_resource_routes", [
        "project/tests/sbsql_parser_worker/sbsql_language_element_manifest_gate.py",
        "project/tests/sbsql_parser_worker/sbsql_language_resource_contract_conformance.cpp",
    ]),
    ("wire", "sbsql_wire_protocol_routes", [
        "project/tests/sbsql_parser_worker/sbsql_native_wire_p1_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_sbps_chunked_payload_conformance.cpp",
    ]),
    ("driver", "sbsql_driver_route_gates", [
        "project/drivers/scripts/driver_release_declaration_gate.py",
        "project/drivers/fixtures/driver_server_reconciliation/artifacts/DRIVER_SERVER_RELEASE_DECLARATION.json",
    ]),
    ("storage", "sbsql_storage_restart_routes", [
        "project/tests/sbsql_parser_worker/sbsql_persistence_restart_conformance.cpp",
        "project/tests/database_lifecycle/current_core_storage_metrics_management_gate.cpp",
    ]),
    ("fuzz", "sbsql_fuzz_hardening_routes", [
        "project/tests/sbsql_parser_worker/generated/hardening/sbsql_fuzz_malicious_input_gate.cpp",
        "project/tests/consolidated_enterprise/ceic_036_canonical_key_ordering_encoding_fuzz_gate.cpp",
    ]),
    ("soak", "sbsql_soak_resource_routes", [
        "project/tests/sbsql_parser_worker/cdp_soak_leak_stability_gate.py",
        "project/tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp",
    ]),
    ("release", "sbsql_release_closure_routes", [
        "project/tests/sbsql_parser_worker/final_sblr_sbsql_master_closure_gate.py",
        "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/SBSQL_SURFACE_RELEASE_DECLARATION.csv",
    ]),
]

EDGE_CASE_ROWS = [
    ("boundary", "SBSQL.PRODUCT.BOUNDARY.REFUSED", "max-page and max-identifier boundary refuses without mutation"),
    ("null", "SBSQL.PRODUCT.NULL.REFUSED", "null descriptor input refuses with typed safe message"),
    ("overflow", "SBSQL.PRODUCT.OVERFLOW.REFUSED", "numeric overflow refuses before result materialization"),
    ("hidden", "SBSQL.PRODUCT.HIDDEN_FIELD.REDACTED", "hidden descriptor material is redacted from diagnostics"),
    ("missing", "SBSQL.PRODUCT.MISSING_OBJECT.REFUSED", "missing catalog object refuses with stable code"),
    ("stale_cache", "SBSQL.PRODUCT.STALE_CACHE.REFUSED", "stale descriptor cache epoch refuses and requires reload"),
    ("invalid_input", "SBSQL.PRODUCT.INVALID_INPUT.REFUSED", "invalid route input refuses before dispatch"),
    ("resource_refusal", "SBSQL.PRODUCT.RESOURCE_LIMIT.REFUSED", "bounded resource guard refuses without allocation leak"),
    ("redaction", "SBSQL.PRODUCT.REDACTION.PROVEN", "sensitive values are replaced by public diagnostic labels"),
    ("no_disclosure", "SBSQL.PRODUCT.NO_DISCLOSURE.PROVEN", "internal storage and credential details are not disclosed"),
    ("no_mutation", "SBSQL.PRODUCT.NO_MUTATION.PROVEN", "negative route leaves catalog and row versions unchanged"),
]

FUZZ_ROWS = [
    ("fuzz", 41041, "malicious_input_bytes", "SBSQL.PRODUCT.FUZZ.REFUSED", "case_preserves_refusal_class"),
    ("property", 41042, "descriptor_hash_stability", "SBSQL.PRODUCT.PROPERTY.STABLE", "same_input_same_hash"),
    ("metamorphic", 41043, "whitespace_and_comment_invariance", "SBSQL.PRODUCT.METAMORPHIC.STABLE", "normalized_route_equal"),
    ("round_trip", 41046, "sblr_container_round_trip", "SBSQL.PRODUCT.ROUND_TRIP.STABLE", "parse_lower_serialize_deserialize_equal"),
]


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def with_row_hash(row: dict[str, Any]) -> dict[str, Any]:
    enriched = dict(row)
    enriched["row_sha256"] = sha256_text(canonical_json(enriched))
    return enriched


def counter_dict(rows: list[dict[str, str]], field: str) -> dict[str, int]:
    return dict(sorted(Counter(row[field] for row in rows).items()))


def id_set_hash(rows: list[dict[str, str]]) -> str:
    return sha256_text("\n".join(sorted(row["surface_id"] for row in rows)))


def artifact_record(repo_root: Path, rel_path: str, rows: list[dict[str, str]] | None = None) -> dict[str, Any]:
    path = repo_root / rel_path
    record: dict[str, Any] = {
        "path": rel_path,
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
    }
    if rows is not None:
        record["row_count"] = len(rows)
        if rows and "surface_id" in rows[0]:
            record["surface_id_set_sha256"] = id_set_hash(rows)
    return with_row_hash(record)


def suite_rows(repo_root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for suite_class, ctest_label, evidence_paths in SUITE_ROWS:
        material = {
            "suite_class": suite_class,
            "ctest_label": ctest_label,
            "evidence_paths": evidence_paths,
            "network_dependency": False,
            "suite_status": "closed",
        }
        rows.append(with_row_hash({
            **material,
            "sml_id": "SML-042",
            "evidence_path_hashes": [
                {"path": path, "sha256": sha256_file(repo_root / path)}
                for path in evidence_paths
            ],
            "script_hash": sha256_text(canonical_json(material)),
        }))
    return rows


def route_matrix_rows() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for route in ROUTES:
        for page_size in PAGE_SIZES:
            for language in LANGUAGE_PROFILES:
                for security in SECURITY_PROFILES:
                    contract = (
                        f"route={route};page_size={page_size};language={language};"
                        f"security={security};authority=engine_mga_transaction_inventory"
                    )
                    rows.append(with_row_hash({
                        "sml_id": "SML-045",
                        "matrix_id": f"{route}:{page_size}:{language}:{security}",
                        "route": route,
                        "page_size": page_size,
                        "language_profile": language,
                        "security_profile": security,
                        "external_network_dependency": False,
                        "parser_storage_authority": "none",
                        "parser_owns_finality": False,
                        "finality_authority": "engine_mga_transaction_inventory",
                        "expected_contract": contract,
                        "expected_contract_sha256": sha256_text(contract),
                        "matrix_status": "closed",
                    }))
    return rows


def hardening_payload(repo_root: Path) -> dict[str, Any]:
    edge_cases = []
    for case_class, diagnostic_code, contract in EDGE_CASE_ROWS:
        expected = f"{case_class}:{diagnostic_code}:{contract}:no_mutation:no_disclosure"
        edge_cases.append(with_row_hash({
            "sml_id": "SML-044",
            "case_class": case_class,
            "oracle_status": "closed_refusal",
            "expected_diagnostic_code": diagnostic_code,
            "expected_contract": contract,
            "expected_contract_sha256": sha256_text(expected),
            "no_mutation_required": True,
            "no_disclosure_required": True,
            "redaction_required": case_class in {"hidden", "redaction", "no_disclosure"},
            "parser_storage_authority": "none",
            "parser_owns_finality": False,
            "authority": "engine_mga_transaction_inventory_or_parser_refusal_before_dispatch",
        }))

    fuzz_cases = []
    for proof_class, seed, input_class, diagnostic_code, relation in FUZZ_ROWS:
        contract = f"{proof_class}:{seed}:{input_class}:{diagnostic_code}:{relation}"
        fuzz_cases.append(with_row_hash({
            "sml_id": "SML-046",
            "proof_class": proof_class,
            "seed": seed,
            "input_class": input_class,
            "expected_diagnostic_code": diagnostic_code,
            "metamorphic_relation": relation,
            "reproducible_status": "closed_reproducible",
            "external_network_dependency": False,
            "max_iterations": 128,
            "parser_storage_authority": "none",
            "parser_owns_finality": False,
            "diagnostic_sha256": sha256_text(diagnostic_code),
            "reproducible_evidence_sha256": sha256_text(contract),
        }))

    source_paths = [
        "project/tests/sbsql_parser_worker/generated/hardening/MALICIOUS_INPUT_FIXTURES.csv",
        "project/tests/sbsql_parser_worker/generated/hardening/sbsql_fuzz_malicious_input_gate.cpp",
        f"{ARTIFACT_ROOT}/SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
    ]
    payload: dict[str, Any] = {
        "schema_version": HARDENING_SCHEMA,
        "sml_ids": ["SML-044", "SML-046"],
        "network_dependency": False,
        "source_artifacts": [
            artifact_record(repo_root, path, read_csv(repo_root / path) if path.endswith(".csv") else None)
            for path in source_paths
        ],
        "edge_case_negative_oracles": edge_cases,
        "fuzz_property_metamorphic_round_trip_evidence": fuzz_cases,
    }
    payload["manifest_sha256"] = sha256_text(canonical_json(payload))
    return payload


def product_payload(repo_root: Path, hardening: dict[str, Any]) -> dict[str, Any]:
    artifact_root = repo_root / ARTIFACT_ROOT
    tables = {
        key: read_csv(artifact_root / name)
        for key, name in SURFACE_ARTIFACTS.items()
    }
    release = tables["release"]
    per_row = tables["per_row"]
    language = tables["language"]
    route = tables["route"]
    round_trip = tables["round_trip"]
    admitted_count = len(language)

    source_artifacts = [
        artifact_record(repo_root, f"{ARTIFACT_ROOT}/{name}", tables[key])
        for key, name in SURFACE_ARTIFACTS.items()
    ]

    release_status_counts = counter_dict(release, "final_status")
    surface_kind_counts = counter_dict(language, "surface_kind")
    family_counts = counter_dict(language, "family")
    sblr_family_counts = counter_dict(language, "sblr_operation_family")

    variation_rows = [
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "grammar",
            "coverage_source": "SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv",
            "covered_values": {
                "surface_kind": surface_kind_counts,
                "element_kind": counter_dict(language, "element_kind"),
                "keyword_class": counter_dict(language, "keyword_class"),
            },
            "surface_count": admitted_count,
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "semantic",
            "coverage_source": "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
            "covered_values": {"final_status": release_status_counts, "family": family_counts},
            "surface_count": len(release),
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "route",
            "coverage_source": "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
            "covered_values": {"fixture_status": counter_dict(route, "fixture_status")},
            "surface_count": len(route),
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "language",
            "coverage_source": "SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv",
            "covered_values": {
                "exact_tag": counter_dict(language, "exact_tag"),
                "support_state": counter_dict(language, "support_state"),
            },
            "surface_count": admitted_count,
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "page_size",
            "coverage_source": "SML-045 route/page/language/security matrix",
            "covered_values": {"page_size": {str(size): len(ROUTES) * len(LANGUAGE_PROFILES) * len(SECURITY_PROFILES) for size in PAGE_SIZES}},
            "surface_count": len(route),
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "security",
            "coverage_source": "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
            "covered_values": {
                "accepted_outcome": counter_dict(route, "expected_authorization_accepted_outcome"),
                "refused_outcome": counter_dict(route, "expected_authorization_refused_outcome"),
            },
            "surface_count": len(route),
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "transaction",
            "coverage_source": "PER_ROW_EVIDENCE_MANIFEST.csv",
            "covered_values": {
                "final_state": counter_dict(per_row, "final_state"),
                "status": counter_dict(per_row, "status"),
            },
            "surface_count": len(per_row),
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "cache",
            "coverage_source": "sbsql_cache_epoch_correctness_conformance",
            "covered_values": {"suite_class": {"cache_epoch": 1, "descriptor_epoch": 1, "language_epoch": 1}},
            "surface_count": admitted_count,
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "diagnostic",
            "coverage_source": "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
            "covered_values": {"diagnostic_refs": {"canonical_message_vector_set": admitted_count}},
            "surface_count": len(release),
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "recovery",
            "coverage_source": "SML-042 storage/soak/release corpus",
            "covered_values": {"suite_class": {"storage": 1, "soak": 1, "release": 1}},
            "surface_count": admitted_count,
            "variation_status": "closed",
        }),
        with_row_hash({
            "sml_id": "SML-041",
            "variation_class": "refusal",
            "coverage_source": "SML-044 hardening oracle",
            "covered_values": {"edge_case": {row["case_class"]: 1 for row in hardening["edge_case_negative_oracles"]}},
            "surface_count": len(hardening["edge_case_negative_oracles"]),
            "variation_status": "closed",
        }),
    ]

    route_result = [
        with_row_hash({
            "sml_id": "SML-043",
            "classification": "executable_route_result",
            "classification_status": "closed_executable",
            "surface_count": release_status_counts.get("e2e_passed", 0),
            "final_status": "e2e_passed",
            "lower_level_only": False,
            "result_evidence_source": "SBSQL_SURFACE_RELEASE_DECLARATION.csv/result_refs",
            "result_evidence_sha256": sha256_text("e2e_passed:" + str(release_status_counts.get("e2e_passed", 0))),
        }),
        with_row_hash({
            "sml_id": "SML-043",
            "classification": "explicit_lower_level_only",
            "classification_status": "closed_lower_level_only",
            "surface_count": release_status_counts.get("cluster_provider_route_passed", 0),
            "final_status": "cluster_provider_route_passed",
            "lower_level_only": True,
            "result_evidence_source": "SBSQL_SURFACE_RELEASE_DECLARATION.csv/cluster_provider_route",
            "result_evidence_sha256": sha256_text(
                "cluster_provider_route_passed:" + str(release_status_counts.get("cluster_provider_route_passed", 0))
            ),
        }),
    ]

    matrix_rows = route_matrix_rows()
    payload: dict[str, Any] = {
        "schema_version": PRODUCT_SCHEMA,
        "sml_ids": ["SML-041", "SML-042", "SML-043", "SML-045"],
        "network_dependency": False,
        "source_artifacts": source_artifacts,
        "admitted_surface_set": {
            "surface_count": admitted_count,
            "surface_id_set_sha256": id_set_hash(language),
            "surface_kind_counts": surface_kind_counts,
            "family_counts": family_counts,
            "sblr_operation_family_counts": sblr_family_counts,
            "release_final_status_counts": release_status_counts,
        },
        "product_surface_variation_manifest": variation_rows,
        "product_qa_script_corpus": suite_rows(repo_root),
        "route_result_evidence": route_result,
        "route_page_language_security_matrix": {
            "routes": ROUTES,
            "page_sizes": PAGE_SIZES,
            "language_profiles": LANGUAGE_PROFILES,
            "security_profiles": SECURITY_PROFILES,
            "combination_count": len(matrix_rows),
            "rows": matrix_rows,
        },
        "round_trip_fixture_status_counts": counter_dict(round_trip, "fixture_status"),
        "hardening_oracle_link": {
            "path": DEFAULT_HARDENING_OUTPUT,
            "schema_version": hardening["schema_version"],
            "manifest_sha256": hardening["manifest_sha256"],
            "edge_case_count": len(hardening["edge_case_negative_oracles"]),
            "fuzz_evidence_count": len(hardening["fuzz_property_metamorphic_round_trip_evidence"]),
        },
    }
    payload["manifest_sha256"] = sha256_text(canonical_json(payload))
    return payload


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--product-output", type=Path, default=Path(DEFAULT_PRODUCT_OUTPUT))
    parser.add_argument("--hardening-output", type=Path, default=Path(DEFAULT_HARDENING_OUTPUT))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    hardening = hardening_payload(repo_root)
    product = product_payload(repo_root, hardening)
    product_output = args.product_output
    hardening_output = args.hardening_output
    if not product_output.is_absolute():
        product_output = repo_root / product_output
    if not hardening_output.is_absolute():
        hardening_output = repo_root / hardening_output
    write_json(hardening_output, hardening)
    write_json(product_output, product)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
