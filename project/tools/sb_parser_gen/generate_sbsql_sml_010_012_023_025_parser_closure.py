#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SML-010/011/012/023/025 parser-closure proof fixtures."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any


DEFAULT_OUTPUT_DIR = "project/tests/sbsql_parser_worker/generated/parser_closure"
MANIFEST_NAME = "SML_010_012_023_025_PARSER_CLOSURE_MANIFEST.csv"
ORACLE_NAME = "SML_010_012_023_025_PARSER_CLOSURE_ORACLE.json"
SCHEMA_VERSION = "sbsql.parser_closure.sml_010_012_023_025.v1"

MANIFEST_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "proof_domain",
    "coverage_class",
    "scenario",
    "route_class",
    "evidence_kind",
    "closure_status",
    "parser_role",
    "parser_executes_sql",
    "parser_has_security_authority",
    "parser_has_storage_authority",
    "parser_owns_finality",
    "server_evaluated",
    "resolver_filtering",
    "language_library_authority",
    "security_projection_authority",
    "expected_contract",
    "expected_contract_hash",
    "expected_diagnostic",
    "expected_diagnostic_hash",
    "oracle_id",
    "oracle_hash",
    "evidence_paths",
    "evidence_tokens",
    "artifact_paths",
]

ARTIFACT_PATHS = (
    "project/tools/sb_parser_gen/"
    "generate_sbsql_sml_010_012_023_025_parser_closure.py;"
    "project/tests/sbsql_parser_worker/generated/parser_closure/"
    f"{MANIFEST_NAME};"
    "project/tests/sbsql_parser_worker/generated/parser_closure/"
    f"{ORACLE_NAME};"
    "project/tests/sbsql_parser_worker/generated/parser_closure/"
    "sbsql_sml_010_012_023_025_parser_closure_gate.py"
)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def oracle_payload(row: dict[str, str]) -> dict[str, str]:
    return {
        "sml_id": row["sml_id"],
        "gate_id": row["gate_id"],
        "row_id": row["row_id"],
        "proof_domain": row["proof_domain"],
        "coverage_class": row["coverage_class"],
        "scenario": row["scenario"],
        "route_class": row["route_class"],
        "evidence_kind": row["evidence_kind"],
        "closure_status": row["closure_status"],
        "parser_role": row["parser_role"],
        "parser_executes_sql": row["parser_executes_sql"],
        "parser_has_security_authority": row["parser_has_security_authority"],
        "parser_has_storage_authority": row["parser_has_storage_authority"],
        "parser_owns_finality": row["parser_owns_finality"],
        "server_evaluated": row["server_evaluated"],
        "resolver_filtering": row["resolver_filtering"],
        "language_library_authority": row["language_library_authority"],
        "security_projection_authority": row["security_projection_authority"],
        "expected_contract": row["expected_contract"],
        "expected_diagnostic": row["expected_diagnostic"],
        "evidence_paths": row["evidence_paths"],
        "evidence_tokens": row["evidence_tokens"],
    }


def row(
    *,
    sml_id: str,
    row_id: str,
    proof_domain: str,
    coverage_class: str,
    scenario: str,
    route_class: str,
    evidence_kind: str,
    expected_contract: str,
    expected_diagnostic: str,
    evidence_paths: tuple[str, ...],
    evidence_tokens: tuple[str, ...],
    closure_status: str = "closed_executable",
    server_evaluated: str = "true",
    resolver_filtering: str = "server_revalidated",
    language_library_authority: str = "none",
    security_projection_authority: str = "not_applicable",
) -> dict[str, str]:
    gate_id = sml_id.replace("SML-", "SML-GATE-")
    base = {
        "sml_id": sml_id,
        "gate_id": gate_id,
        "row_id": row_id,
        "proof_domain": proof_domain,
        "coverage_class": coverage_class,
        "scenario": scenario,
        "route_class": route_class,
        "evidence_kind": evidence_kind,
        "closure_status": closure_status,
        "parser_role": "translate_to_sblr_and_diagnostics_only",
        "parser_executes_sql": "false",
        "parser_has_security_authority": "false",
        "parser_has_storage_authority": "false",
        "parser_owns_finality": "false",
        "server_evaluated": server_evaluated,
        "resolver_filtering": resolver_filtering,
        "language_library_authority": language_library_authority,
        "security_projection_authority": security_projection_authority,
        "expected_contract": expected_contract,
        "expected_contract_hash": sha256_text(expected_contract),
        "expected_diagnostic": expected_diagnostic,
        "expected_diagnostic_hash": sha256_text(expected_diagnostic),
        "oracle_id": row_id.replace("SML-", "SML-ORACLE-"),
        "evidence_paths": ";".join(evidence_paths),
        "evidence_tokens": ";".join(evidence_tokens),
        "artifact_paths": ARTIFACT_PATHS,
    }
    base["oracle_hash"] = sha256_text(canonical_json(oracle_payload(base)))
    return base


def manifest_rows() -> list[dict[str, str]]:
    return [
        row(
            sml_id="SML-010",
            row_id="SML-010-RENDERER-REVALIDATION-FILTER",
            proof_domain="localized_rendering_resolver_filtering",
            coverage_class="localized_renderer_server_revalidation",
            scenario="renderer rows must require server revalidation before localized text is rendered",
            route_class="language_element_manifest",
            evidence_kind="ctest_and_manifest_gate",
            expected_contract=(
                "localized renderer rows retain canonical renderer ids and require "
                "server revalidation before release-supported rendering"
            ),
            expected_diagnostic="SBSQL.LANG_ELEMENT_MANIFEST.RENDERER_REVALIDATION_REQUIRED",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_language_resource_contract_conformance.cpp",
                "project/tests/sbsql_parser_worker/sbsql_language_element_manifest_gate.py",
            ),
            evidence_tokens=(
                "SBSQL.LANG_ELEMENT_MANIFEST.RENDERER_REVALIDATION_REQUIRED",
                "server_revalidation_required",
                "SML-010 renderer without server revalidation was accepted",
                "renderer_id",
            ),
        ),
        row(
            sml_id="SML-010",
            row_id="SML-010-PRIVATE-INPUT-FILTER",
            proof_domain="localized_rendering_resolver_filtering",
            coverage_class="sensitive_resolver_filtering",
            scenario="localized diagnostics redact private resolver input and sensitive resource fields",
            route_class="localized_diagnostic_rendering",
            evidence_kind="ctest",
            expected_contract=(
                "localized rendering output is filtered so private resource, profile, "
                "topology, path, and SQL-like diagnostic material is not disclosed"
            ),
            expected_diagnostic="private_input_state=redacted",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_language_resource_contract_conformance.cpp",
            ),
            evidence_tokens=(
                "RequireNoSensitiveMaterial",
                "private_input_state=redacted",
                "sbsql.common_resource_pack.private",
                "SML-091 rendered diagnostic did not mark private input redacted",
            ),
        ),
        row(
            sml_id="SML-010",
            row_id="SML-010-MANIFEST-RELEASE-COMPLETE",
            proof_domain="localized_rendering_resolver_filtering",
            coverage_class="release_manifest_completeness",
            scenario="language element manifest covers keywords, topology, predictive states, renderers, diagnostics, and governance",
            route_class="language_element_manifest",
            evidence_kind="python_gate",
            expected_contract=(
                "release-supported language element rows are unique across surface, "
                "topology, predictive, renderer, compatibility, and message ids"
            ),
            expected_diagnostic="none",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_language_element_manifest_gate.py",
            ),
            evidence_tokens=(
                "REQUIRED_SECTIONS",
                "JSON required_sections does not declare every required SML-010 class",
                "duplicate {field}",
                "release_supported",
            ),
        ),
        row(
            sml_id="SML-011",
            row_id="SML-011-LANGUAGE-CONTROL-NON-AUTHORITY",
            proof_domain="parser_language_library_non_authority",
            coverage_class="language_control_non_authority",
            scenario="SET/RESET/SHOW language routes lower to SBLR and leave session mutation to server authority",
            route_class="language_session_control",
            evidence_kind="ctest",
            expected_contract=(
                "language controls carry parser evidence only; server session "
                "language context remains the authority for mutation and finality"
            ),
            expected_diagnostic="authority.server.session_language_context_required",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_language_control_surface_conformance.cpp",
            ),
            evidence_tokens=(
                "parser_updates_session_language",
                "authority.server.session_language_context_required",
                "prepared_statement_reinterpretation",
                "parser SQL execution",
            ),
            language_library_authority="server_session_language_context",
        ),
        row(
            sml_id="SML-011",
            row_id="SML-011-BUNDLE-MANIFEST-FAIL-CLOSED",
            proof_domain="parser_language_library_non_authority",
            coverage_class="bundle_manifest_fail_closed",
            scenario="language bundle load, unload, and validate routes fail closed without an admitted manifest",
            route_class="language_bundle_control",
            evidence_kind="ctest",
            closure_status="closed_refusal",
            expected_contract=(
                "language package load/unload effects are not executed by the parser "
                "and missing manifests refuse before runtime authority is reached"
            ),
            expected_diagnostic="SBSQL.SBLR.LANGUAGE_BUNDLE_MANIFEST_REQUIRED",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_language_control_surface_conformance.cpp",
            ),
            evidence_tokens=(
                "SBSQL.SBLR.LANGUAGE_BUNDLE_MANIFEST_REQUIRED",
                "load_or_unload_effects_executed_by_parser",
                "admitted_bundle_manifest_attached",
                "did not fail closed on missing manifest",
            ),
            language_library_authority="server_bundle_manifest_admission",
        ),
        row(
            sml_id="SML-011",
            row_id="SML-011-CACHE-AUTHORITY-REFUSAL",
            proof_domain="parser_language_library_non_authority",
            coverage_class="cache_authority_refusal",
            scenario="parser cache rejects authorization, storage, visibility, and finality authority payloads",
            route_class="frontdoor_sblr_template_cache",
            evidence_kind="ctest",
            closure_status="closed_refusal",
            expected_contract=(
                "language library cache entries are mediation-only and invalidate "
                "on resolver, descriptor, security, grant, and language resource epochs"
            ),
            expected_diagnostic="SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.AUTHORIZATION_AUTHORITY_CACHED",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_sml022_shared_parser_runtime_authority_conformance.cpp",
                "project/src/parsers/sbsql_worker/cache/sblr_template_cache.cpp",
            ),
            evidence_tokens=(
                "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.AUTHORIZATION_AUTHORITY_CACHED",
                "parser cache cannot own storage visibility authorization finality rollback commit transaction inventory or recovery authority",
                "parser cache reused stale language-resource entry",
                "resource_version_identity",
            ),
            language_library_authority="server_epoch_and_cache_admission",
        ),
        row(
            sml_id="SML-012",
            row_id="SML-012-EXACT-LOCALE-MATRIX",
            proof_domain="multilingual_edge_cases",
            coverage_class="exact_locale_matrix",
            scenario="exact locale profiles are unique, native-reviewed beta rows and do not collapse to base or wildcard language tags",
            route_class="locale_profile_matrix",
            evidence_kind="ctest",
            expected_contract=(
                "multilingual profiles include exact regional tags and refuse "
                "wildcard or base-language collapse"
            ),
            expected_diagnostic="SML014.WILDCARD_LOCALE_PROFILE",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_language_locale_profile_matrix_conformance.cpp",
            ),
            evidence_tokens=(
                "es-419",
                "SML014.WILDCARD_LOCALE_PROFILE",
                "SML014.BASE_LANGUAGE_PROFILE",
                "SML014.COLLAPSED_RESOURCE_HASH",
            ),
            resolver_filtering="exact_locale_no_collapse",
        ),
        row(
            sml_id="SML-012",
            row_id="SML-012-FALLBACK-CACHE-ISOLATION",
            proof_domain="multilingual_edge_cases",
            coverage_class="fallback_cache_isolation",
            scenario="preferred language fallback cache keys isolate standard English and localized sessions",
            route_class="parser_frontdoor_cache",
            evidence_kind="ctest",
            expected_contract=(
                "standard English fallback parsing preserves language profile, "
                "fallback tag, resource compatibility, and resource version identity"
            ),
            expected_diagnostic="none",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_multilingual_passthrough_runtime_conformance.cpp",
                "project/src/parsers/sbsql_worker/cache/sblr_template_cache.cpp",
            ),
            evidence_tokens=(
                "input_language_fallback_tag",
                "resource_version_identity",
                "preferred-language session did not advertise standard English fallback",
                "canonical English session reused preferred-language fallback cache entry",
            ),
            resolver_filtering="language_resource_epoch_and_fallback_key",
        ),
        row(
            sml_id="SML-012",
            row_id="SML-012-RESOURCE-EDGE-SAFETY",
            proof_domain="multilingual_edge_cases",
            coverage_class="edge_safety_scan",
            scenario="multilingual edge-safety gate scans public source/resource anchors without draft docs or network access",
            route_class="release_resource_gate",
            evidence_kind="python_gate",
            expected_contract=(
                "resource, locale, identifier, malformed-pack, licensing, backup, "
                "and diagnostic multilingual edge cases are checked from public evidence roots"
            ),
            expected_diagnostic="none",
            evidence_paths=(
                "project/tools/release/sbsql_multilingual_resource_edge_safety_gate.py",
                "project/tests/sbsql_parser_worker/sbsql_multilingual_resource_edge_safety_harness.py",
            ),
            evidence_tokens=(
                "never reads draft",
                "never uses the network",
                "SML-097",
                "sbsql_multilingual_resource_edge_safety_harness=passed",
            ),
            resolver_filtering="public_resource_edge_scan",
        ),
        row(
            sml_id="SML-023",
            row_id="SML-023-AUTHENTICATED-ROUTE-FIXTURE-CLOSURE",
            proof_domain="sbsql_full_parser_closure",
            coverage_class="route_fixture_closure",
            scenario="authenticated route fixtures require credential, transport, admission, authorization, diagnostic, and result proof fields",
            route_class="authenticated_full_route_matrix",
            evidence_kind="python_gate",
            expected_contract=(
                "authored authenticated-route fixtures carry per-row CTest labels, "
                "server admission evidence, engine dispatch evidence, and parser boundary proof"
            ),
            expected_diagnostic="none",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/generated/full_surface/sbsql_authenticated_route_fixture_gate.py",
            ),
            evidence_tokens=(
                "REQUIRED_KEYS",
                "per_row_ctest_label",
                "mga_execution_authority",
                "sbsql_authenticated_route_fixture_gate=passed",
            ),
            resolver_filtering="not_applicable",
        ),
        row(
            sml_id="SML-023",
            row_id="SML-023-STRICT-ROW-FINAL-STATE",
            proof_domain="sbsql_full_parser_closure",
            coverage_class="strict_row_final_state",
            scenario="strict row coverage refuses grey states and requires row-specific implementation evidence",
            route_class="strict_row_coverage_ledger",
            evidence_kind="python_gate",
            expected_contract=(
                "native, future, and public cluster-scoped rows must be in final "
                "states with row-specific function/API and fixture evidence"
            ),
            expected_diagnostic="sbsql_no_grey_row_coverage_gate",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/generated/full_surface/sbsql_no_grey_row_coverage_gate.py",
            ),
            evidence_tokens=(
                "final_states",
                "evidence_complete_no",
                "fixture_evidence_grey",
                "sbsql_no_grey_row_coverage_gate=passed",
            ),
            resolver_filtering="not_applicable",
        ),
        row(
            sml_id="SML-023",
            row_id="SML-023-SCALAR-PROJECTION-DISPATCH",
            proof_domain="sbsql_full_parser_closure",
            coverage_class="scalar_projection_dispatch",
            scenario="scalar projection routes lower to SBLR, pass server admission, dispatch through engine APIs, and omit source SQL text",
            route_class="query_evaluate_projection",
            evidence_kind="ctest",
            expected_contract=(
                "full parser closure includes scalar projection admission, engine "
                "dispatch, no source SQL text, and no generic SQL execution"
            ),
            expected_diagnostic="none",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_query_scalar_projection_conformance.cpp",
                "project/tests/sbsql_parser_worker/generated/full_surface/authenticated_route/SBSQL-3BDB82ED4BDB.route.yaml",
            ),
            evidence_tokens=(
                "server admission rejected scalar projection route",
                "no_source_sql_text",
                "no_generic_sql_execution",
                "DispatchSblrOperation/query.evaluate_projection",
            ),
            resolver_filtering="not_applicable",
        ),
        row(
            sml_id="SML-025",
            row_id="SML-025-SECURITY-AUTHORIZATION-SERVER-RECLASSIFY",
            proof_domain="legacy_security_projection_server_evaluated",
            coverage_class="security_authorization_server_reclassify",
            scenario="compatibility security authorization is reclassified by server SBLR admission, not by parser projection",
            route_class="server_sblr_security_admission",
            evidence_kind="ctest",
            expected_contract=(
                "security authorization projections remain server-evaluated and "
                "require server security policy context"
            ),
            expected_diagnostic="PARSER_SERVER_IPC.SBLR_REVALIDATION_FAILED",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_sml022_shared_parser_runtime_authority_conformance.cpp",
                "project/src/server/sblr_admission.cpp",
            ),
            evidence_tokens=(
                "security.authorize",
                "authority.server.security_policy_context_required",
                "authority.parser.no_security_authorization",
                "server did not reclassify security authorization through SBLR authority",
            ),
            resolver_filtering="server_security_context",
            security_projection_authority="server_sblr_admission_and_engine_security_api",
        ),
        row(
            sml_id="SML-025",
            row_id="SML-025-PROTECTED-MATERIAL-REDACTED-PROJECTION",
            proof_domain="legacy_security_projection_server_evaluated",
            coverage_class="protected_material_redacted_projection",
            scenario="protected material catalog and audit projections return redacted server-evaluated results",
            route_class="protected_material_catalog_projection",
            evidence_kind="ctest",
            expected_contract=(
                "protected material projection rows are evaluated through engine "
                "security APIs and redact protected references"
            ),
            expected_diagnostic="protected-material-redacted",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_protected_material_exact_route_conformance.cpp",
            ),
            evidence_tokens=(
                "security.protected_material.catalog.inspect",
                "authority.engine.protected_material_api_required",
                "authority.parser.no_security_authorization",
                "protected-material-redacted",
                "protected reference leaked",
            ),
            resolver_filtering="server_security_projection_redaction",
            security_projection_authority="server_sblr_admission_and_engine_security_api",
        ),
        row(
            sml_id="SML-025",
            row_id="SML-025-PREPARED-SECURITY-EPOCH-REFUSAL",
            proof_domain="legacy_security_projection_server_evaluated",
            coverage_class="security_epoch_prepared_refusal",
            scenario="prepared security projections are refused after server security epoch changes",
            route_class="prepared_sblr_server_registry",
            evidence_kind="ctest",
            closure_status="closed_refusal",
            expected_contract=(
                "server owns prepared statement UUIDs, security epochs, request "
                "lifecycle, and finality tokens for compatibility projection execution"
            ),
            expected_diagnostic="PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
            evidence_paths=(
                "project/tests/sbsql_parser_worker/sbsql_sml022_shared_parser_runtime_authority_conformance.cpp",
            ),
            evidence_tokens=(
                "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
                "server prepared registry did not own prepared UUID",
                "server did not upsert finality record for prepare request",
                "server executed prepared SBLR after security epoch changed",
            ),
            resolver_filtering="server_prepared_epoch_check",
            security_projection_authority="server_sblr_admission_and_engine_security_api",
        ),
    ]


def write_manifest(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=MANIFEST_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def write_oracle(path: Path, rows: list[dict[str, str]]) -> None:
    counts: dict[str, int] = {}
    for item in rows:
        counts[item["sml_id"]] = counts.get(item["sml_id"], 0) + 1
    payload = {
        "schema_version": SCHEMA_VERSION,
        "manifest_name": MANIFEST_NAME,
        "row_count": len(rows),
        "sml_counts": dict(sorted(counts.items())),
        "rows": [
            {
                "sml_id": item["sml_id"],
                "row_id": item["row_id"],
                "proof_domain": item["proof_domain"],
                "coverage_class": item["coverage_class"],
                "oracle_id": item["oracle_id"],
                "oracle_hash": item["oracle_hash"],
            }
            for item in rows
        ],
    }
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path(DEFAULT_OUTPUT_DIR))
    args = parser.parse_args()

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = manifest_rows()
    write_manifest(output_dir / MANIFEST_NAME, rows)
    write_oracle(output_dir / ORACLE_NAME, rows)
    print(
        "generated_sbsql_sml_010_012_023_025_parser_closure "
        f"rows={len(rows)} output_dir={output_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
