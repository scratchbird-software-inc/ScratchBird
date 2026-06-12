#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SML-054/055/056/057/062 multimodel capability proof fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


DEFAULT_OUTPUT_DIR = (
    "project/tests/sbsql_parser_worker/generated/multimodel_capability"
)
MANIFEST_NAME = "SML_054_057_062_MULTIMODEL_CAPABILITY_MANIFEST.json"
ORACLE_NAME = "SML_054_057_062_MULTIMODEL_CAPABILITY_ORACLE.jsonl"
SCHEMA_VERSION = "sbsql.multimodel_capability.v1"
GATE_ID = "SML-GATE-054-057-062"
ROUTES = ["embedded", "inet_listener", "local_ipc", "parser_route"]
PAGE_SIZES = [8192, 16384, 32768, 65536, 131072]


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def stable_hash(value: Any) -> str:
    return sha256_text(canonical_json(value))


def script_for(row_id: str, capability_family: str, operation: str) -> str:
    return "\n".join(
        [
            f"-- {row_id}",
            "SET LANGUAGE sbsql_multimodel_v1;",
            f"CAPABILITY {capability_family} {operation};",
            "EXPECT parser_executes_sql = false;",
            "EXPECT storage_authority = scratchbird_engine_mga_pages;",
            "EXPECT finality_authority = scratchbird_mga_transaction_inventory;",
        ]
    )


def base_contract(row_id: str, capability_family: str, coverage_class: str) -> dict[str, Any]:
    return {
        "row_id": row_id,
        "capability_family": capability_family,
        "coverage_class": coverage_class,
        "result_shape": "ExecutionResultEnvelope.v3",
        "uuid_identity_preserved": True,
        "security_context_preserved": True,
        "result_contract_preserved": True,
        "diagnostic_contract_preserved": True,
    }


def diagnostic_contract(row_id: str, expected_result: str) -> dict[str, Any]:
    code = "SBSQL.MULTIMODEL.OK"
    severity = "info"
    if expected_result == "refused":
        code = "SBSQL.MULTIMODEL.FAIL_CLOSED"
        severity = "error"
    return {
        "row_id": row_id,
        "diagnostic_code": code,
        "severity": severity,
        "message_vector": "canonical_message_vector_set",
        "safe_detail": "public_safe",
    }


def make_row(
    *,
    row_id: str,
    sml_ids: list[str],
    capability_family: str,
    feature_area: str,
    coverage_class: str,
    operation: str,
    expected_result: str = "admitted",
    storage_authority: str = "scratchbird_engine_mga_pages",
    finality_authority: str = "scratchbird_mga_transaction_inventory",
    recovery_authority: str = "scratchbird_mga_recovery",
    reference_profile_behavior: str = "not_reference_profile",
    script: str | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    script_text = script or script_for(row_id, capability_family, operation)
    result = base_contract(row_id, capability_family, coverage_class)
    result.update(
        {
            "feature_area": feature_area,
            "operation": operation,
            "expected_result": expected_result,
            "storage_authority": storage_authority,
            "finality_authority": finality_authority,
        }
    )
    diagnostic = diagnostic_contract(row_id, expected_result)
    oracle = {
        "oracle_id": row_id,
        "expected_result_contract": result,
        "expected_diagnostic_contract": diagnostic,
    }
    oracle["oracle_sha256"] = stable_hash(oracle)

    row = {
        "row_id": row_id,
        "sml_ids": sml_ids,
        "capability_family": capability_family,
        "feature_area": feature_area,
        "coverage_class": coverage_class,
        "proof_kind": "refusal_oracle" if expected_result == "refused" else "manifest_oracle",
        "status": "closed_refusal" if expected_result == "refused" else "closed_manifest_oracle",
        "routes": ROUTES,
        "page_sizes": PAGE_SIZES,
        "language_profile": "sbsql_multimodel_v1",
        "transaction_profile": "scratchbird_mga_transaction_inventory",
        "security_profile": "server_security_policy_revalidated",
        "diagnostic_profile": "canonical_message_vector_set",
        "recovery_authority": recovery_authority,
        "storage_authority": storage_authority,
        "finality_authority": finality_authority,
        "parser_executes_sql": False,
        "network_required": False,
        "external_storage_allowed": False,
        "external_finality_allowed": False,
        "reference_profile_behavior": reference_profile_behavior,
        "compatibility_profile": "scratchbird_canonical_or_fail_closed",
        "script_text": script_text,
        "script_sha256": sha256_text(script_text),
        "expected_result_sha256": stable_hash(result),
        "expected_diagnostic_sha256": stable_hash(diagnostic),
        "oracle_id": row_id,
        "oracle_sha256": oracle["oracle_sha256"],
        "uuid_identity_preserved": True,
        "security_context_preserved": True,
        "result_contract_preserved": True,
        "diagnostic_contract_preserved": True,
    }
    row["row_sha256"] = stable_hash(row)
    return row, oracle


ROW_SPECS: list[dict[str, Any]] = [
    {
        "row_id": "SML-054-DOCUMENT-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "document",
        "feature_area": "document_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "document_collection_manifest_and_script",
    },
    {
        "row_id": "SML-054-KEY-VALUE-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "key_value",
        "feature_area": "key_value_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "key_value_manifest_and_script",
    },
    {
        "row_id": "SML-054-WIDE-COLUMN-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "wide_column",
        "feature_area": "wide_column_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "wide_column_manifest_and_script",
    },
    {
        "row_id": "SML-054-GRAPH-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "graph",
        "feature_area": "graph_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "graph_manifest_and_script",
    },
    {
        "row_id": "SML-054-VECTOR-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "vector",
        "feature_area": "vector_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "vector_manifest_and_script",
    },
    {
        "row_id": "SML-054-TEXT-SEARCH-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "text_search",
        "feature_area": "text_search_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "text_search_manifest_and_script",
    },
    {
        "row_id": "SML-054-TIME-SERIES-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "time_series",
        "feature_area": "time_series_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "time_series_manifest_and_script",
    },
    {
        "row_id": "SML-054-HYBRID-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "hybrid",
        "feature_area": "hybrid_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "hybrid_manifest_and_script",
    },
    {
        "row_id": "SML-054-SQL-NOSQL-JOIN-MANIFEST-SCRIPT",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "sql_nosql_join",
        "feature_area": "sql_nosql_join_manifest_script",
        "coverage_class": "manifest_script",
        "operation": "sql_nosql_join_manifest_and_script",
    },
    {
        "row_id": "SML-054-ROUTE-PAGE-LANGUAGE",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "hybrid",
        "feature_area": "route_page_language",
        "coverage_class": "route_page_language",
        "operation": "route_page_size_language_matrix",
    },
    {
        "row_id": "SML-054-TRANSACTION-SECURITY-DIAGNOSTICS-RECOVERY",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "hybrid",
        "feature_area": "transaction_security_diagnostics_recovery",
        "coverage_class": "transaction_security_diagnostics_recovery",
        "operation": "transaction_security_diagnostics_recovery_matrix",
    },
    {
        "row_id": "SML-054-REFUSAL-ROWS",
        "sml_ids": ["SML-054", "SML-062"],
        "capability_family": "hybrid",
        "feature_area": "refusal_rows",
        "coverage_class": "refusal",
        "operation": "unsupported_external_storage_finality_refusal",
        "expected_result": "refused",
        "storage_authority": "not_reached_fail_closed",
        "finality_authority": "not_reached_fail_closed",
        "recovery_authority": "not_reached_fail_closed",
    },
    {
        "row_id": "SML-055-DOCUMENT-LIFECYCLE",
        "sml_ids": ["SML-055"],
        "capability_family": "document",
        "feature_area": "document_lifecycle",
        "coverage_class": "lifecycle",
        "operation": "document_create_read_update_delete",
    },
    {
        "row_id": "SML-055-KEY-VALUE-LIFECYCLE",
        "sml_ids": ["SML-055"],
        "capability_family": "key_value",
        "feature_area": "key_value_lifecycle",
        "coverage_class": "lifecycle",
        "operation": "key_value_put_get_delete",
    },
    {
        "row_id": "SML-055-WIDE-COLUMN-LIFECYCLE",
        "sml_ids": ["SML-055"],
        "capability_family": "wide_column",
        "feature_area": "wide_column_lifecycle",
        "coverage_class": "lifecycle",
        "operation": "wide_column_row_family_lifecycle",
    },
    {
        "row_id": "SML-055-JSON-PATH",
        "sml_ids": ["SML-055"],
        "capability_family": "document",
        "feature_area": "json_path",
        "coverage_class": "json_path",
        "operation": "json_path_lookup",
    },
    {
        "row_id": "SML-055-XML-PATH",
        "sml_ids": ["SML-055"],
        "capability_family": "document",
        "feature_area": "xml_path",
        "coverage_class": "xml_path",
        "operation": "xml_path_lookup",
    },
    {
        "row_id": "SML-055-MUTATION",
        "sml_ids": ["SML-055"],
        "capability_family": "document",
        "feature_area": "mutation",
        "coverage_class": "mutation",
        "operation": "document_patch_mutation",
    },
    {
        "row_id": "SML-055-KEYSPACE",
        "sml_ids": ["SML-055"],
        "capability_family": "key_value",
        "feature_area": "keyspace",
        "coverage_class": "keyspace",
        "operation": "keyspace_descriptor",
    },
    {
        "row_id": "SML-055-CONDITIONAL-MUTATION",
        "sml_ids": ["SML-055"],
        "capability_family": "key_value",
        "feature_area": "conditional_mutation",
        "coverage_class": "conditional_mutation",
        "operation": "compare_and_set_mutation",
    },
    {
        "row_id": "SML-055-RANGE-SCAN",
        "sml_ids": ["SML-055"],
        "capability_family": "wide_column",
        "feature_area": "range_scan",
        "coverage_class": "range_scan",
        "operation": "wide_column_range_scan",
    },
    {
        "row_id": "SML-055-BATCH",
        "sml_ids": ["SML-055"],
        "capability_family": "wide_column",
        "feature_area": "batch",
        "coverage_class": "batch",
        "operation": "wide_column_batch_mutation",
    },
    {
        "row_id": "SML-055-TTL-POLICY",
        "sml_ids": ["SML-055"],
        "capability_family": "key_value",
        "feature_area": "ttl_policy",
        "coverage_class": "ttl_policy",
        "operation": "ttl_policy_admission",
    },
    {
        "row_id": "SML-056-GRAPH-SEARCH",
        "sml_ids": ["SML-056"],
        "capability_family": "graph",
        "feature_area": "graph_search",
        "coverage_class": "graph_search",
        "operation": "graph_pattern_search",
    },
    {
        "row_id": "SML-056-GRAPH-TRAVERSAL",
        "sml_ids": ["SML-056"],
        "capability_family": "graph",
        "feature_area": "graph_traversal",
        "coverage_class": "graph_traversal",
        "operation": "graph_bounded_traversal",
    },
    {
        "row_id": "SML-056-GRAPH-MUTATION",
        "sml_ids": ["SML-056"],
        "capability_family": "graph",
        "feature_area": "graph_mutation",
        "coverage_class": "graph_mutation",
        "operation": "graph_edge_vertex_mutation",
    },
    {
        "row_id": "SML-056-VECTOR-EXACT-SEARCH",
        "sml_ids": ["SML-056"],
        "capability_family": "vector",
        "feature_area": "vector_exact_search",
        "coverage_class": "vector_exact_search",
        "operation": "vector_exact_knn_search",
    },
    {
        "row_id": "SML-056-VECTOR-APPROXIMATE-SEARCH",
        "sml_ids": ["SML-056"],
        "capability_family": "vector",
        "feature_area": "vector_approximate_search",
        "coverage_class": "vector_approximate_search",
        "operation": "vector_approximate_knn_search",
    },
    {
        "row_id": "SML-056-HYBRID-SEARCH",
        "sml_ids": ["SML-056"],
        "capability_family": "hybrid",
        "feature_area": "hybrid_search",
        "coverage_class": "hybrid_search",
        "operation": "hybrid_graph_vector_text_search",
    },
    {
        "row_id": "SML-056-TEXT-SEARCH-WORKLOAD",
        "sml_ids": ["SML-056"],
        "capability_family": "text_search",
        "feature_area": "text_search_workload",
        "coverage_class": "text_search",
        "operation": "text_search_ranked_query",
    },
    {
        "row_id": "SML-056-TIME-SERIES-WORKLOAD",
        "sml_ids": ["SML-056"],
        "capability_family": "time_series",
        "feature_area": "time_series_workload",
        "coverage_class": "time_series",
        "operation": "time_series_window_query",
    },
    {
        "row_id": "SML-056-RELATIONAL-MULTIMODEL-UUID",
        "sml_ids": ["SML-056"],
        "capability_family": "sql_nosql_join",
        "feature_area": "relational_multimodel_uuid_security_results_diagnostics",
        "coverage_class": "relational_multimodel_uuid",
        "operation": "relational_multimodel_join_preserves_identity",
    },
    {
        "row_id": "SML-057-REFERENCE-PROFILE-STORAGE-MAP",
        "sml_ids": ["SML-057"],
        "capability_family": "compatibility_profile",
        "feature_area": "reference_profile_storage_map",
        "coverage_class": "reference_profile_storage_map",
        "operation": "reference_profile_storage_maps_to_scratchbird",
        "reference_profile_behavior": "maps_to_scratchbird_behavior",
    },
    {
        "row_id": "SML-057-REFERENCE-PROFILE-SECURITY-MAP",
        "sml_ids": ["SML-057"],
        "capability_family": "compatibility_profile",
        "feature_area": "reference_profile_security_map",
        "coverage_class": "reference_profile_security_map",
        "operation": "reference_profile_security_maps_to_scratchbird",
        "reference_profile_behavior": "maps_to_scratchbird_behavior",
    },
    {
        "row_id": "SML-057-REFERENCE-PROFILE-RECOVERY-MAP",
        "sml_ids": ["SML-057"],
        "capability_family": "compatibility_profile",
        "feature_area": "reference_profile_recovery_map",
        "coverage_class": "reference_profile_recovery_map",
        "operation": "reference_profile_recovery_maps_to_scratchbird",
        "reference_profile_behavior": "maps_to_scratchbird_behavior",
    },
    {
        "row_id": "SML-057-REFERENCE-PROFILE-STORAGE-FAIL-CLOSED",
        "sml_ids": ["SML-057"],
        "capability_family": "compatibility_profile",
        "feature_area": "reference_profile_storage_fail_closed",
        "coverage_class": "reference_profile_fail_closed_storage",
        "operation": "reference_profile_external_storage_refusal",
        "expected_result": "refused",
        "storage_authority": "not_reached_fail_closed",
        "finality_authority": "not_reached_fail_closed",
        "recovery_authority": "not_reached_fail_closed",
        "reference_profile_behavior": "fail_closed_no_reference_owned_storage",
    },
    {
        "row_id": "SML-057-REFERENCE-PROFILE-FINALITY-FAIL-CLOSED",
        "sml_ids": ["SML-057"],
        "capability_family": "compatibility_profile",
        "feature_area": "reference_profile_finality_fail_closed",
        "coverage_class": "reference_profile_fail_closed_finality",
        "operation": "reference_profile_external_finality_refusal",
        "expected_result": "refused",
        "storage_authority": "not_reached_fail_closed",
        "finality_authority": "not_reached_fail_closed",
        "recovery_authority": "not_reached_fail_closed",
        "reference_profile_behavior": "fail_closed_no_reference_owned_finality",
    },
]


def build_payloads(generator_rel: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    rows: list[dict[str, Any]] = []
    oracle_records: list[dict[str, Any]] = []
    for spec in ROW_SPECS:
        row, oracle = make_row(**spec)
        rows.append(row)
        oracle_records.append(oracle)

    oracle_bytes = "".join(
        canonical_json(record) + "\n" for record in oracle_records
    ).encode("utf-8")

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "gate_id": GATE_ID,
        "status": "complete",
        "generator": {
            "path": generator_rel,
            "kind": "deterministic_generated_fixture",
            "network_required": False,
            "output_files": [MANIFEST_NAME, ORACLE_NAME],
        },
        "required_sml_ids": ["SML-054", "SML-055", "SML-056", "SML-057", "SML-062"],
        "required_routes": ROUTES,
        "required_page_sizes": PAGE_SIZES,
        "authority_contract": {
            "storage_authority": "scratchbird_engine_mga_pages",
            "finality_authority": "scratchbird_mga_transaction_inventory",
            "recovery_authority": "scratchbird_mga_recovery",
            "parser_executes_sql": False,
            "external_storage_allowed": False,
            "external_finality_allowed": False,
        },
        "rows": rows,
        "oracle_file": ORACLE_NAME,
        "oracle_file_sha256": sha256_bytes(oracle_bytes),
    }
    manifest["manifest_sha256"] = stable_hash(manifest)
    return manifest, oracle_records


def write_outputs(output_dir: Path, manifest: dict[str, Any], oracle_records: list[dict[str, Any]]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / MANIFEST_NAME).write_text(
        json.dumps(manifest, ensure_ascii=True, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (output_dir / ORACLE_NAME).write_text(
        "".join(canonical_json(record) + "\n" for record in oracle_records),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output-dir", type=Path, default=Path(DEFAULT_OUTPUT_DIR))
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    output_dir = args.output_dir
    if not output_dir.is_absolute():
        output_dir = repo_root / output_dir

    generator_rel = str(Path(__file__).resolve().relative_to(repo_root))
    manifest, oracle_records = build_payloads(generator_rel)
    write_outputs(output_dir, manifest, oracle_records)
    print(
        "generated SML-054/055/056/057/062 multimodel capability fixtures "
        f"rows={len(manifest['rows'])} manifest_sha256={manifest['manifest_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
