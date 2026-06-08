#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public guardrail gates for the donor/SBLR surface fixture substrate.

These checks prove fixture promotion, traceability schema, deterministic
resource shape, SBSQL synchronization requirements, and source-level admission
guardrails for the audited SBLR surface closure rows.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path
import re
import sys
from typing import Any


FIXTURE_DIR = Path(
    "project/tests/sblr_surface/fixtures/donor_sblr_interface_gap_2026_06_03"
)

CSV_FILES = {
    "donor_gap_summary": "DONOR_GAP_SUMMARY.csv",
    "donor_private_opcode": "DONOR_INTERNAL_META_OPCODE_CLEANUP_MATRIX.csv",
    "explicit_unsupported": "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv",
    "execution_plan_seed": "IMPLEMENTATION_EXECUTION_PLAN_SEED_MATRIX.csv",
    "non_direct_function": "NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv",
    "stale_deferred_alias": "SBLR_STALE_DEFERRED_ALIAS_CLEANUP_MATRIX.csv",
    "sbsql_family": "SBSQL_SBLR_FAMILY_RECONCILIATION_MATRIX.csv",
    "server_authority_rollup": "SERVER_AUTHORITY_ROLLUP.csv",
    "server_authority": "SERVER_AUTHORITY_SURFACE_MATRIX.csv",
}

TRACEABILITY_MANIFEST = "sblr_surface_traceability_manifest.json"
HASH_MANIFEST = "sblr_surface_fixture_hashes.json"
SBSQL_SYNC_MANIFEST = "sbsql_sync_requirements.json"
PUBLIC_PARSER_HANDOFF_PREFIX = (
    "fixture://execution-plans/driver-parser-sblr-sbsql-readiness-closure/"
)
PUBLIC_SBSQL_HANDOFF_PREFIX = (
    "fixture://execution-plans/sbsql-per-element-contract-completion/"
)

STRICT_ROW_COVERAGE_LEDGER = Path(
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/"
    "STRICT_ROW_COVERAGE_LEDGER.csv"
)
PRIMARY_FAMILY_SNAPSHOT = "sblr_primary_family_snapshot.json"
SERVER_SBLR_ADMISSION = Path("project/src/server/sblr_admission.cpp")
SERVER_SBLR_DISPATCH = Path("project/src/server/sblr_dispatch_server.cpp")
SBLR_OPCODE_REGISTRY = Path("public_contract_snapshot")
ENGINE_OPCODE_REGISTRY = Path("project/src/engine/sblr/sblr_opcode_registry.cpp")
TIKV_DONOR_PROFILE = Path(
    "public_contract_snapshot"
    "donor-tikv-exact-extraction-slice-2-kv-transaction-coprocessor-api-ast-sblr-lowering-profile.yaml"
)
TIKV_DONOR_CHAPTER = Path(
    "public_contract_snapshot"
    "appendix-donor-tikv-exact-extraction-slice-2-kv-transaction-coprocessor-api-ast-sblr-lowering-profile.md"
)
TIKV_CONFORMANCE_MANIFEST = Path(
    "public_contract_snapshot"
    "donor-tikv-exact-extraction-slice-2-kv-transaction-coprocessor-api-ast-sblr-lowering-profile-conformance.yaml"
)
IMMUDB_DONOR_PROFILE = Path(
    "public_contract_snapshot"
    "donor-immudb-exact-extraction-slice-2-sql-kv-document-verifiable-history-api-ast-sblr-lowering-profile.yaml"
)
IMMUDB_DONOR_CHAPTER = Path(
    "public_contract_snapshot"
    "appendix-donor-immudb-exact-extraction-slice-2-sql-kv-document-verifiable-history-api-ast-sblr-lowering-profile.md"
)

RETIRED_NON_OPCODE_TOKENS = {
    "SBLR_CHECK_RETENTION_POLICY": "retention_policy_uuid_on_history_or_dml_descriptor",
    "SBLR_BEGIN_COMPAT_IMMUDB": "SBLR_TXN_BEGIN.versioning_scope=VERIFIABLE_LEDGER",
}

NO_EXTERNAL_REFERENCE_RE = re.compile(
    r"https?://|/home/|/Users/|[A-Za-z]:\\\\", re.IGNORECASE
)
FORBIDDEN_COMPLETION_TOKENS = (
    "skip",
    "skipped",
    "waiver",
    "waived",
    "xfail",
    "disabled",
    "not-run",
    "not_run",
    "fixture-only",
    "fixture_only",
    "platform-excluded",
    "platform_excluded",
)
FORBIDDEN_RECONCILIATION_TOKENS = FORBIDDEN_COMPLETION_TOKENS + (
    "unresolved",
    "deferred",
    "todo",
    "tbd",
    "decide whether",
    "unknown",
    "placeholder",
)
FORBIDDEN_DONOR_DIALECT_TOKENS = (
    "donor_dialect_paste_through",
    "donor dialect paste",
    "paste-through",
    "paste_through",
    "paste through",
)
EXPECTED_RECONCILED_FAMILY_ROWS = 13
EXPECTED_RECONCILED_SBSQL_ROWS = 2328
EXPECTED_PRIMARY_SBLR_FAMILY_ROWS = 53
RECONCILED_FAMILY_STATUS = "resolved_to_declared_primary_sblr_families"
DECLARED_PRIMARY_RECONCILIATION_STATUS = "already_declared_primary"
SBSQL_NATIVE_STYLE = "sbsql_native_normalized"
PARSER_FAMILY_DRIFT_ID = "DPR-DRIFT-009"

REQUIRED_TRACEABILITY_FIELDS = (
    "canonical_spec_authority",
    "implementation_route_placeholder_target",
    "test_id",
    "platform_result_field",
    "proof_class",
    "evidence_artifact_field",
)

REQUIRED_SYNC_FIELDS = (
    "sbsql_cst_reference",
    "ast_sblr_reference",
    "donor_normalization_reference",
    "parser_execution_plan_sync_reference",
)

REFERENCE_FIELD_NAMES = REQUIRED_TRACEABILITY_FIELDS + REQUIRED_SYNC_FIELDS

SYNC_ALLOWED_CHANGE_KINDS = {
    "donor_gap_summary_route",
    "donor_sblr_seed_row",
    "engine_function_surface_route",
    "engine_server_authority_rollup",
    "engine_server_authority_route",
    "engine_sblr_opcode_registry_change",
    "engine_unsupported_refusal_route",
    "donor_sblr_fixture_hash_manifest_change",
    "donor_sblr_traceability_manifest_change",
    "parser_facing_contract_freeze_change",
    "sblr_alias_registry_cleanup",
    "sblr_family_reconciliation",
    "sblr_family_snapshot_change",
    "sblr_opcode_registry_change",
    "sblr_opcode_visibility_cleanup",
    "sbsql_sblr_row_coverage_change",
    "sbsql_sync_manifest_change",
    "server_sblr_admission_change",
    "server_sblr_dispatch_change",
}

ENGINE_SBLR_RESOURCE_SYNC_PATHS = {
    "public_contract_snapshot",
    "project/src/engine/sblr/sblr_opcode_registry.cpp",
    "project/src/server/sblr_admission.cpp",
    "project/src/server/sblr_dispatch_server.cpp",
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/STRICT_ROW_COVERAGE_LEDGER.csv",
    "project/tests/sblr_surface/fixtures/donor_sblr_interface_gap_2026_06_03/sbsql_sync_requirements.json",
    "project/tests/sblr_surface/fixtures/donor_sblr_interface_gap_2026_06_03/sblr_primary_family_snapshot.json",
    "project/tests/sblr_surface/fixtures/donor_sblr_interface_gap_2026_06_03/sblr_surface_traceability_manifest.json",
    "project/tests/sblr_surface/fixtures/donor_sblr_interface_gap_2026_06_03/sblr_surface_fixture_hashes.json",
    "project/tests/engine_listener_enterprise/fixtures/parser_facing_contract_freeze_manifest.json",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--fixture-root")
    parser.add_argument(
        "--mode",
        choices=(
            "all",
            "inventory",
            "traceability",
            "completion-policy",
            "determinism",
            "family-reconciliation",
            "interface-closure",
            "registry-cleanup",
            "sbsql-sync",
        ),
        default="all",
    )
    return parser.parse_args()


def load_csv(fixture_root: Path, filename: str, errors: list[str]) -> list[dict[str, str]]:
    path = fixture_root / filename
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError:
        errors.append(f"missing fixture CSV: {path}")
    except csv.Error as exc:
        errors.append(f"invalid CSV fixture {path}: {exc}")
    except OSError as exc:
        errors.append(f"failed reading fixture CSV {path}: {exc}")
    return []


def load_repo_csv(
    repo_root: Path,
    relative_path: Path,
    errors: list[str],
) -> list[dict[str, str]]:
    path = repo_root / relative_path
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError:
        errors.append(f"missing repo CSV: {relative_path.as_posix()}")
    except csv.Error as exc:
        errors.append(f"invalid repo CSV {relative_path.as_posix()}: {exc}")
    except OSError as exc:
        errors.append(f"failed reading repo CSV {relative_path.as_posix()}: {exc}")
    return []


def load_json(path: Path, errors: list[str]) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as handle:
            loaded = json.load(handle)
    except FileNotFoundError:
        errors.append(f"missing JSON fixture: {path}")
        return {}
    except json.JSONDecodeError as exc:
        errors.append(f"invalid JSON fixture {path}: {exc}")
        return {}
    except OSError as exc:
        errors.append(f"failed reading JSON fixture {path}: {exc}")
        return {}
    if not isinstance(loaded, dict):
        errors.append(f"JSON fixture must be an object: {path}")
        return {}
    return loaded


def split_semicolon_list(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def extract_sblr_family_tokens(text: str) -> set[str]:
    return set(re.findall(r"\bsblr\.[a-z0-9_.]+\.v3\b", text))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def require_count(
    rows: list[dict[str, str]],
    expected: int,
    label: str,
    errors: list[str],
) -> None:
    actual = len(rows)
    if actual != expected:
        errors.append(f"{label} expected {expected} rows, found {actual}")


def rel_to_repo(path: Path, repo_root: Path, errors: list[str]) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        errors.append(f"path is outside repository root: {path}")
    return path.as_posix()


def assert_project_test_fixture_root(
    repo_root: Path,
    fixture_root: Path,
    errors: list[str],
) -> None:
    repo_project_tests = (repo_root / "project/tests").resolve()
    resolved_fixture = fixture_root.resolve()
    try:
        resolved_fixture.relative_to(repo_project_tests)
    except ValueError:
        errors.append(
            "fixture root must be under project/tests, "
            f"got {resolved_fixture}"
        )
    try:
        resolved_fixture.relative_to((repo_root / "public_audit_summary").resolve())
        errors.append("fixture root must not consume public_audit_summary")
    except ValueError:
        pass

    rel = rel_to_repo(fixture_root, repo_root, errors)
    if "public_audit_summary" in rel:
        errors.append(f"fixture root must not reference public_audit_summary: {rel}")


def scan_for_external_paths(fixture_root: Path, errors: list[str]) -> None:
    for path in sorted(fixture_root.iterdir()):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), start=1):
            if NO_EXTERNAL_REFERENCE_RE.search(line):
                errors.append(
                    f"{path}:{lineno} preserves a local absolute path or URL"
                )
            if "public_audit_summary" in line:
                errors.append(f"{path}:{lineno} references public_audit_summary")


def assert_row_selector_exists(
    fixture_root: Path,
    fixture_file: str,
    selector: dict[str, Any],
    errors: list[str],
    context: str,
) -> None:
    if fixture_file not in CSV_FILES.values():
        errors.append(f"{context} references unknown fixture file {fixture_file}")
        return
    if not isinstance(selector, dict):
        errors.append(f"{context} row_selector must be an object")
        return
    column = selector.get("column")
    value = selector.get("value")
    if not column or value is None:
        errors.append(f"{context} row_selector requires column and value")
        return
    rows = load_csv(fixture_root, fixture_file, errors)
    matches = [row for row in rows if row.get(str(column)) == str(value)]
    if len(matches) != 1:
        errors.append(
            f"{context} selector {fixture_file}:{column}={value} "
            f"expected one row, found {len(matches)}"
        )


def assert_repo_reference_exists(
    repo_root: Path,
    reference: str,
    errors: list[str],
    context: str,
    field_name: str,
) -> None:
    if not reference:
        return
    reference_path = reference.split("#", 1)[0]
    if reference_path.startswith(("fixture://", "evidence_artifacts.", "platform_results.")):
        return
    if reference_path.startswith("/") or ".." in Path(reference_path).parts:
        errors.append(f"{context} {field_name} must be a repo-relative path: {reference}")
        return
    if reference_path.startswith("fixture://execution-plans/"):
        return
    if reference_path.startswith(("docs/", "project/")) and not (repo_root / reference_path).exists():
        errors.append(f"{context} {field_name} references missing repo path: {reference}")


def read_repo_text(repo_root: Path, relative_path: Path, errors: list[str]) -> str:
    path = repo_root / relative_path
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"missing repo file: {relative_path.as_posix()}")
    except OSError as exc:
        errors.append(f"failed reading repo file {relative_path.as_posix()}: {exc}")
    return ""


def text_block(text: str, start_marker: str, end_marker: str) -> str:
    start = text.find(start_marker)
    if start == -1:
        return ""
    end = text.find(end_marker, start + len(start_marker))
    if end == -1:
        return text[start:]
    return text[start:end]


def cpp_array_block(text: str, symbol: str) -> str:
    start = text.find(symbol)
    if start == -1:
        return ""
    end = text.find("}};", start)
    if end == -1:
        return ""
    return text[start : end + 3]


def cpp_function_block(text: str, signature_marker: str, next_marker: str) -> str:
    return text_block(text, signature_marker, next_marker)


def cpp_string_literals(text: str) -> set[str]:
    return set(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', text))


def cpp_family_rule_names(text: str) -> set[str]:
    return set(re.findall(r'\{\s*"([^"\\]*(?:\\.[^"\\]*)*)"\s*,', text))


def parse_primary_family_snapshot(
    fixture_root: Path,
    errors: list[str],
) -> tuple[set[str], set[str]]:
    snapshot = load_json(fixture_root / PRIMARY_FAMILY_SNAPSHOT, errors)
    family_values = snapshot.get("declared_envelope_families", [])
    binding_values = snapshot.get("envelope_to_opcode_family_bindings", [])
    if not isinstance(family_values, list):
        errors.append("primary SBLR family snapshot declared_envelope_families must be a list")
        family_values = []
    if not isinstance(binding_values, list):
        errors.append("primary SBLR family snapshot envelope_to_opcode_family_bindings must be a list")
        binding_values = []

    envelope_families = {str(value) for value in family_values}
    envelope_bindings = {str(value) for value in binding_values}
    declared_count = snapshot.get("declared_envelope_family_count")
    binding_count = snapshot.get("envelope_to_opcode_family_binding_count")

    require(
        envelope_families,
        "primary SBLR family snapshot must declare envelope families",
        errors,
    )
    require(
        envelope_bindings,
        "primary SBLR family snapshot must declare envelope-to-opcode bindings",
        errors,
    )
    require(
        envelope_families == envelope_bindings,
        "primary SBLR family snapshot declarations and bindings differ",
        errors,
    )
    require(
        declared_count == len(envelope_families) == EXPECTED_PRIMARY_SBLR_FAMILY_ROWS,
        (
            "primary SBLR family snapshot declared count must be "
            f"{EXPECTED_PRIMARY_SBLR_FAMILY_ROWS} and match "
            f"the family list, found count={declared_count} parsed={len(envelope_families)}"
        ),
        errors,
    )
    require(
        binding_count == len(envelope_bindings) == EXPECTED_PRIMARY_SBLR_FAMILY_ROWS,
        (
            "primary SBLR family snapshot binding count must be "
            f"{EXPECTED_PRIMARY_SBLR_FAMILY_ROWS} and match "
            f"the binding list, found count={binding_count} parsed={len(envelope_bindings)}"
        ),
        errors,
    )
    return envelope_families, envelope_bindings


def assert_manifest_references_exist(
    repo_root: Path,
    row: dict[str, Any],
    errors: list[str],
    context: str,
) -> None:
    for field_name in REFERENCE_FIELD_NAMES:
        value = row.get(field_name)
        if isinstance(value, str):
            assert_repo_reference_exists(repo_root, value, errors, context, field_name)


def validate_inventory(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)
    scan_for_external_paths(fixture_root, errors)

    rows_by_name = {
        key: load_csv(fixture_root, filename, errors)
        for key, filename in CSV_FILES.items()
    }

    require_count(rows_by_name["server_authority"], 318, "server authority", errors)
    server_actions = Counter(
        row.get("server_action", "") for row in rows_by_name["server_authority"]
    )
    require(
        server_actions
        == Counter(
            {
                "security_denial": 181,
                "server_policy_gate": 82,
                "migration_service_route": 55,
            }
        ),
        f"server authority action counts drifted: {dict(server_actions)}",
        errors,
    )
    require(
        Counter(row.get("is_sblr_execution_surface", "") for row in rows_by_name["server_authority"])
        == Counter({"no": 318}),
        "server authority rows must remain non-SBLR execution surfaces",
        errors,
    )

    require_count(rows_by_name["non_direct_function"], 258, "non-direct function/API", errors)
    non_direct_decisions = Counter(
        row.get("implementation_decision", "")
        for row in rows_by_name["non_direct_function"]
    )
    require(
        non_direct_decisions
        == Counter(
            {
                "catalog_projection_only": 112,
                "connector_operation": 80,
                "policy_blocked": 46,
                "trusted_udr_registration": 15,
                "unsupported": 5,
            }
        ),
        f"non-direct function decision counts drifted: {dict(non_direct_decisions)}",
        errors,
    )

    require_count(rows_by_name["explicit_unsupported"], 5, "explicit unsupported", errors)
    require_count(rows_by_name["stale_deferred_alias"], 8, "stale/deferred alias", errors)
    require_count(rows_by_name["donor_private_opcode"], 34, "donor-private opcode", errors)

    sbsql_rows = rows_by_name["sbsql_family"]
    mismatched = [
        row
        for row in sbsql_rows
        if row.get("primary_sblr_envelope_status") != "declared_primary"
    ]
    require_count(mismatched, 13, "mismatched SBSQL family", errors)
    affected_rows = sum(int(row.get("row_count", "0") or "0") for row in mismatched)
    require(
        affected_rows == EXPECTED_RECONCILED_SBSQL_ROWS,
        f"mismatched SBSQL family affected row count expected {EXPECTED_RECONCILED_SBSQL_ROWS}, found {affected_rows}",
        errors,
    )

    summary_rows = rows_by_name["donor_gap_summary"]
    summary_totals = {
        field: sum(int(row.get(field, "0") or "0") for row in summary_rows)
        for field in (
            "catalog_projection_only",
            "connector_operation",
            "policy_blocked",
            "trusted_udr_registration",
            "unsupported",
            "server_authority_rows",
        )
    }
    require(summary_totals["server_authority_rows"] == 318, "donor summary server row total drifted", errors)
    require(
        sum(
            summary_totals[field]
            for field in (
                "catalog_projection_only",
                "connector_operation",
                "policy_blocked",
                "trusted_udr_registration",
                "unsupported",
            )
        )
        == 258,
        f"donor summary non-direct total drifted: {summary_totals}",
        errors,
    )


def validate_parser_family_sync(
    fixture_root: Path,
    reconciliation_rows: list[dict[str, str]],
    errors: list[str],
) -> None:
    sync_manifest = load_json(fixture_root / SBSQL_SYNC_MANIFEST, errors)
    sync_rows = sync_manifest.get("rows", [])
    if not isinstance(sync_rows, list):
        errors.append("SBSQL sync requirements rows must be a list")
        return
    matches = [
        row
        for row in sync_rows
        if isinstance(row, dict)
        and row.get("sync_id") == "SBLR-SURFACE-SYNC-SBSQL-FAMILY"
    ]
    require(
        len(matches) == 1,
        "SBSQL sync manifest must contain one SBLR-SURFACE-SYNC-SBSQL-FAMILY row",
        errors,
    )
    if len(matches) != 1:
        return

    row = matches[0]
    context = "SBLR-SURFACE-SYNC-SBSQL-FAMILY"
    require(
        row.get("change_kind") == "sblr_family_reconciliation",
        f"{context} change_kind must be sblr_family_reconciliation",
        errors,
    )
    require(
        row.get("sbsql_style") == SBSQL_NATIVE_STYLE,
        f"{context} must declare {SBSQL_NATIVE_STYLE}",
        errors,
    )
    joined = " ".join(str(value) for value in row.values())
    require(
        f"DRIFT_REGISTER.csv#{PARSER_FAMILY_DRIFT_ID}" in joined,
        f"{context} must point to {PARSER_FAMILY_DRIFT_ID}",
        errors,
    )
    require(
        all(
            row.get("parser_sync_reference", "").endswith(
                f"DRIFT_REGISTER.csv#{PARSER_FAMILY_DRIFT_ID}"
            )
            for row in reconciliation_rows
            if row.get("primary_sblr_envelope_status") != "declared_primary"
        ),
        f"all reconciled family rows must point to {PARSER_FAMILY_DRIFT_ID}",
        errors,
    )
    lowered = joined.lower()
    for token in FORBIDDEN_DONOR_DIALECT_TOKENS:
        if token in lowered:
            errors.append(f"{context} contains donor dialect paste-through token {token!r}")


def validate_server_admission_family_reconciliation(
    repo_root: Path,
    mismatch_rows: list[dict[str, str]],
    envelope_families: set[str],
    errors: list[str],
) -> None:
    source = read_repo_text(repo_root, SERVER_SBLR_ADMISSION, errors)
    dispatch_source = read_repo_text(repo_root, SERVER_SBLR_DISPATCH, errors)
    if not source:
        return

    stale_families = {row.get("sbsql_family", "") for row in mismatch_rows}
    require(
        len(stale_families) == EXPECTED_RECONCILED_FAMILY_ROWS,
        (
            "server non-primary audit family set expected "
            f"{EXPECTED_RECONCILED_FAMILY_ROWS}, found {len(stale_families)}"
        ),
        errors,
    )

    server_family_block = cpp_array_block(source, "kServerSblrFamilies")
    non_primary_block = cpp_array_block(source, "kNonPrimarySblrAuditFamilies")
    require(server_family_block, "sblr_admission.cpp missing kServerSblrFamilies block", errors)
    require(
        non_primary_block,
        "sblr_admission.cpp missing kNonPrimarySblrAuditFamilies block",
        errors,
    )
    if not server_family_block or not non_primary_block:
        return

    admitted_families = cpp_family_rule_names(server_family_block)
    non_primary_families = cpp_string_literals(non_primary_block)
    require(
        len(admitted_families) == EXPECTED_PRIMARY_SBLR_FAMILY_ROWS,
        (
            "kServerSblrFamilies must contain "
            f"{EXPECTED_PRIMARY_SBLR_FAMILY_ROWS} declared primary families, "
            f"found {len(admitted_families)}"
        ),
        errors,
    )
    require(
        admitted_families <= envelope_families,
        (
            "kServerSblrFamilies admits families outside the primary SBLR snapshot: "
            f"{sorted(admitted_families - envelope_families)}"
        ),
        errors,
    )
    require(
        not (admitted_families & stale_families),
        (
            "kServerSblrFamilies must not admit ELER-071 non-primary audit families: "
            f"{sorted(admitted_families & stale_families)}"
        ),
        errors,
    )
    require(
        non_primary_families == stale_families,
        (
            "kNonPrimarySblrAuditFamilies must exactly match ELER-071 audited families: "
            f"source={sorted(non_primary_families)} fixture={sorted(stale_families)}"
        ),
        errors,
    )
    require(
        "SBLR.FAMILY_RECONCILIATION_REQUIRED" in source,
        "sblr_admission.cpp must expose a specific reconciliation-required diagnostic",
        errors,
    )
    require(
        "RejectFamilyReconciliationRequired" in source,
        "sblr_admission.cpp must route ambiguous audited families through a named fail-closed helper",
        errors,
    )
    require(
        "if (IsNonPrimarySblrAuditFamily(family))" in source,
        "AdmitFamily must reject non-primary audited families before admission lookup",
        errors,
    )
    require(
        "if (IsNonPrimarySblrAuditFamily(encoded))" in source,
        "direct family envelope admission must fail closed for non-primary audited families",
        errors,
    )
    require(
        'RejectFamilyReconciliationRequired("public_envelope_family_unresolved")' in source,
        "binary public-envelope admission must fail closed when no exact primary family exists",
        errors,
    )

    function_checks = {
        "FamilyForOperationId": cpp_function_block(
            source,
            "std::optional<std::string> FamilyForOperationId",
            "\n\nstd::uint64_t RowCountHint",
        ),
        "FamilyForLegacyEnvelope": cpp_function_block(
            source,
            "std::optional<std::string> FamilyForLegacyEnvelope",
            "\n\nstd::optional<std::string> FamilyForOperationId",
        ),
        "FamilyForPublicEnvelope": cpp_function_block(
            source,
            "std::string FamilyForPublicEnvelope",
            "\n\nstd::string OperationForLegacyEnvelope",
        ),
    }
    for name, block in function_checks.items():
        require(block, f"sblr_admission.cpp missing {name} body", errors)
        stale_returns = sorted(cpp_string_literals(block) & stale_families)
        require(
            not stale_returns,
            f"{name} must not return stale ELER-071 aggregate family names: {stale_returns}",
            errors,
        )

    if dispatch_source:
        dispatch_stale_literals = sorted(cpp_string_literals(dispatch_source) & stale_families)
        require(
            not dispatch_stale_literals,
            (
                "sblr_dispatch_server.cpp must not hard-code stale ELER-071 aggregate "
                f"family names: {dispatch_stale_literals}"
            ),
            errors,
        )
        public_abi_bridge = cpp_function_block(
            dispatch_source,
            "scratchbird::engine::SblrOperationFamily PublicAbiFamilyForServerFamily",
            "\n\nbool OperationNeedsTransactionContext",
        )
        require(
            public_abi_bridge,
            "sblr_dispatch_server.cpp missing PublicAbiFamilyForServerFamily body",
            errors,
        )
        if public_abi_bridge:
            stale_bridge_literals = sorted(cpp_string_literals(public_abi_bridge) & stale_families)
            require(
                not stale_bridge_literals,
                (
                    "PublicAbiFamilyForServerFamily must not translate stale ELER-071 "
                    f"aggregate families: {stale_bridge_literals}"
                ),
                errors,
            )

    require(
        "return std::nullopt;" in function_checks["FamilyForOperationId"],
        "FamilyForOperationId must fail closed when exact primary selection is unsafe",
        errors,
    )
    require(
        "return std::nullopt;" in function_checks["FamilyForLegacyEnvelope"],
        "FamilyForLegacyEnvelope must fail closed when exact primary selection is unsafe",
        errors,
    )


def validate_family_reconciliation(
    repo_root: Path,
    fixture_root: Path,
    errors: list[str],
) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)
    reconciliation_rows = load_csv(
        fixture_root,
        CSV_FILES["sbsql_family"],
        errors,
    )
    operation_rows = load_repo_csv(repo_root, STRICT_ROW_COVERAGE_LEDGER, errors)
    envelope_families, envelope_bindings = parse_primary_family_snapshot(
        fixture_root,
        errors,
    )
    validate_parser_family_sync(fixture_root, reconciliation_rows, errors)
    if errors:
        return

    required_columns = {
        "sbsql_family",
        "row_count",
        "primary_sblr_envelope_status",
        "priority_d_envelope_status",
        "reconciliation_status",
        "resolved_target_sblr_families",
        "sbsql_style",
        "parser_sync_reference",
        "source_artifact",
        "proposed_mapping_or_decision",
    }
    for index, row in enumerate(reconciliation_rows, start=1):
        missing = sorted(required_columns - set(row))
        require(
            not missing,
            f"SBSQL family reconciliation row {index} missing columns: {missing}",
            errors,
        )
    if errors:
        return

    family_rows: dict[str, dict[str, str]] = {}
    for row in reconciliation_rows:
        family = row.get("sbsql_family", "")
        if family in family_rows:
            errors.append(f"duplicate SBSQL family reconciliation row: {family}")
        family_rows[family] = row

    matrix_family_counts = Counter(
        row.get("sblr_operation_family", "") for row in operation_rows
    )
    matrix_family_counts.pop("", None)
    require(
        len(operation_rows) == 2617,
        f"SBSQL operation matrix expected 2617 rows, found {len(operation_rows)}",
        errors,
    )
    undeclared_matrix_families = {
        family
        for family in matrix_family_counts
        if family not in envelope_families
    }
    mismatch_rows = [
        row
        for row in reconciliation_rows
        if row.get("primary_sblr_envelope_status") != "declared_primary"
    ]
    mismatched_families = {row.get("sbsql_family", "") for row in mismatch_rows}
    require_count(
        mismatch_rows,
        EXPECTED_RECONCILED_FAMILY_ROWS,
        "SBSQL/SBLR reconciled family",
        errors,
    )
    affected_rows = sum(int(row.get("row_count", "0") or "0") for row in mismatch_rows)
    require(
        affected_rows == EXPECTED_RECONCILED_SBSQL_ROWS,
        (
            "SBSQL/SBLR reconciled family affected row count expected "
            f"{EXPECTED_RECONCILED_SBSQL_ROWS}, found {affected_rows}"
        ),
        errors,
    )
    require(
        undeclared_matrix_families == mismatched_families,
        (
            "undeclared SBSQL operation-matrix families must exactly match "
            f"the reconciled audit set: matrix={sorted(undeclared_matrix_families)} "
            f"fixture={sorted(mismatched_families)}"
        ),
        errors,
    )
    require(
        set(matrix_family_counts) == set(family_rows),
        (
            "SBSQL family reconciliation matrix must declare every family seen "
            f"in the SBSQL operation matrix: matrix={sorted(matrix_family_counts)} "
            f"fixture={sorted(family_rows)}"
        ),
        errors,
    )
    require(
        sum(matrix_family_counts[family] for family in mismatched_families)
        == EXPECTED_RECONCILED_SBSQL_ROWS,
        "SBSQL operation matrix affected-row total for reconciled families drifted",
        errors,
    )
    validate_server_admission_family_reconciliation(
        repo_root,
        mismatch_rows,
        envelope_families,
        errors,
    )

    for row in reconciliation_rows:
        family = row["sbsql_family"]
        context = f"SBSQL family {family}"
        try:
            declared_row_count = int(row.get("row_count", ""))
        except ValueError:
            errors.append(f"{context} row_count must be an integer")
            continue
        require(
            matrix_family_counts.get(family, 0) == declared_row_count,
            (
                f"{context} row_count expected {declared_row_count}, "
                f"SBSQL operation matrix has {matrix_family_counts.get(family, 0)}"
            ),
            errors,
        )
        assert_repo_reference_exists(
            repo_root,
            row.get("source_artifact", ""),
            errors,
            context,
            "source_artifact",
        )
        assert_repo_reference_exists(
            repo_root,
            row.get("parser_sync_reference", ""),
            errors,
            context,
            "parser_sync_reference",
        )
        require(
            row.get("sbsql_style") == SBSQL_NATIVE_STYLE,
            f"{context} must use {SBSQL_NATIVE_STYLE}",
            errors,
        )
        row_text = " ".join(str(value) for value in row.values())
        lowered = row_text.lower()
        for token in FORBIDDEN_RECONCILIATION_TOKENS:
            if token in lowered:
                errors.append(f"{context} contains unresolved completion token {token!r}")
        for token in FORBIDDEN_DONOR_DIALECT_TOKENS:
            if token in lowered:
                errors.append(f"{context} contains donor dialect paste-through token {token!r}")

        target_families = split_semicolon_list(row.get("resolved_target_sblr_families", ""))
        require(target_families, f"{context} must declare resolved target families", errors)
        require(
            len(target_families) == len(set(target_families)),
            f"{context} resolved target families contain duplicates",
            errors,
        )
        for token in extract_sblr_family_tokens(row.get("proposed_mapping_or_decision", "")):
            require(
                token == family or token in envelope_families,
                f"{context} proposed mapping references unknown SBLR family {token}",
                errors,
            )

        if row.get("primary_sblr_envelope_status") == "declared_primary":
            require(
                row.get("reconciliation_status") == DECLARED_PRIMARY_RECONCILIATION_STATUS,
                (
                    f"{context} declared primary row must use "
                    f"{DECLARED_PRIMARY_RECONCILIATION_STATUS}"
                ),
                errors,
            )
            require(
                target_families == [family],
                f"{context} declared primary row must target itself only",
                errors,
            )
            require(
                family in envelope_families and family in envelope_bindings,
                f"{context} declared primary family must be in SBLR operation registry",
                errors,
            )
            continue

        require(
            row.get("reconciliation_status") == RECONCILED_FAMILY_STATUS,
            f"{context} must use {RECONCILED_FAMILY_STATUS}",
            errors,
        )
        require(
            row.get("priority_d_envelope_status") == "absent",
            f"{context} must not depend on a priority-D envelope",
            errors,
        )
        require(
            family not in envelope_families,
            f"{context} original aggregate family must not be a declared primary envelope",
            errors,
        )
        require(
            family not in target_families,
            f"{context} must not target the stale aggregate family",
            errors,
        )
        require(
            row.get("parser_sync_reference", "").endswith(
                f"DRIFT_REGISTER.csv#{PARSER_FAMILY_DRIFT_ID}"
            ),
            f"{context} must synchronize through {PARSER_FAMILY_DRIFT_ID}",
            errors,
        )
        for target in target_families:
            require(
                target in envelope_families,
                f"{context} target family {target} is not declared in SBLR registry",
                errors,
            )
            require(
                target in envelope_bindings,
                f"{context} target family {target} lacks opcode-family binding",
                errors,
            )
            require(
                target not in mismatched_families,
                f"{context} target family {target} is another unreconciled audit family",
                errors,
            )


def validate_registry_cleanup(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)
    stale_rows = load_csv(
        fixture_root / "",
        CSV_FILES["stale_deferred_alias"],
        errors,
    )
    donor_rows = load_csv(
        fixture_root / "",
        CSV_FILES["donor_private_opcode"],
        errors,
    )
    stale_tokens = [row.get("token", "") for row in stale_rows if row.get("token")]
    donor_private_tokens = [row.get("token", "") for row in donor_rows if row.get("token")]
    donor_prefixed_tokens = [
        token.replace("SBLR_TIKV_", "SBLR_DONOR_TIKV_", 1)
        for token in donor_private_tokens
    ]
    authored_stale_tokens = [
        token
        for token in stale_tokens
        if token not in RETIRED_NON_OPCODE_TOKENS
    ]

    registry_text = read_repo_text(repo_root, SBLR_OPCODE_REGISTRY, errors)
    engine_registry_text = read_repo_text(repo_root, ENGINE_OPCODE_REGISTRY, errors)
    tikv_profile_text = read_repo_text(repo_root, TIKV_DONOR_PROFILE, errors)
    tikv_chapter_text = read_repo_text(repo_root, TIKV_DONOR_CHAPTER, errors)
    tikv_conformance_text = read_repo_text(repo_root, TIKV_CONFORMANCE_MANIFEST, errors)
    immudb_profile_text = read_repo_text(repo_root, IMMUDB_DONOR_PROFILE, errors)
    immudb_chapter_text = read_repo_text(repo_root, IMMUDB_DONOR_CHAPTER, errors)
    if errors:
        return

    deferred_block = text_block(
        registry_text,
        "  deferred_canonical_authoring:",
        "  retired_non_opcode_descriptor_mappings:",
    )
    require(deferred_block, "sblr-opcodes.yaml missing deferred_canonical_authoring block", errors)
    require(
        "pending_priority_D: []" in deferred_block,
        "deferred_canonical_authoring must have an empty pending_priority_D list",
        errors,
    )
    for token in stale_tokens:
        require(
            token not in deferred_block,
            f"{token} remains in deferred_canonical_authoring",
            errors,
        )

    for token in authored_stale_tokens:
        require(
            token in registry_text,
            f"{token} canonical opcode disappeared from sblr-opcodes.yaml",
            errors,
        )
        require(
            token in engine_registry_text,
            f"{token} canonical opcode disappeared from engine registry",
            errors,
        )

    for token, target in RETIRED_NON_OPCODE_TOKENS.items():
        require(
            token in registry_text and target in registry_text,
            f"{token} must remain retired with descriptor mapping {target}",
            errors,
        )
        require(
            token not in engine_registry_text,
            f"{token} must not be accepted by the engine opcode registry",
            errors,
        )

    bare_tikv_pattern = re.compile(r"\bSBLR_TIKV_[A-Z0-9_]+\b")
    for relative_path, text in (
        (SBLR_OPCODE_REGISTRY, registry_text),
        (ENGINE_OPCODE_REGISTRY, engine_registry_text),
        (TIKV_DONOR_PROFILE, tikv_profile_text),
        (TIKV_DONOR_CHAPTER, tikv_chapter_text),
        (TIKV_CONFORMANCE_MANIFEST, tikv_conformance_text),
    ):
        matches = sorted(set(bare_tikv_pattern.findall(text)))
        require(
            not matches,
            f"{relative_path.as_posix()} admits unprefixed TiKV SBLR tokens: {matches}",
            errors,
        )

    for token in donor_prefixed_tokens:
        require(
            token in registry_text,
            f"{token} missing from donor_private_meta_opcodes registry block",
            errors,
        )
        require(
            token in tikv_profile_text,
            f"{token} missing from TiKV donor profile after normalization",
            errors,
        )
        require(
            token in tikv_chapter_text,
            f"{token} missing from TiKV donor chapter after normalization",
            errors,
        )
        require(
            token not in engine_registry_text,
            f"{token} must stay donor-private and out of the engine opcode registry",
            errors,
        )

    for token in RETIRED_NON_OPCODE_TOKENS:
        require(
            token not in immudb_profile_text,
            f"{token} remains in immudb donor profile emitted SBLR",
            errors,
        )
        require(
            token not in immudb_chapter_text,
            f"{token} remains in immudb donor chapter emitted SBLR",
            errors,
        )

    require(
        "SBLR_TXN_BEGIN versioning_scope=VERIFIABLE_LEDGER" in immudb_profile_text,
        "immudb donor profile must lower compat begin through SBLR_TXN_BEGIN descriptor extension",
        errors,
    )
    require(
        "retention_policy_uuid descriptor check" in immudb_profile_text,
        "immudb donor profile must model retention check as descriptor policy validation",
        errors,
    )


def validate_completion_policy(manifest: dict[str, Any], errors: list[str]) -> None:
    policy = manifest.get("completion_policy")
    if not isinstance(policy, dict):
        errors.append("traceability manifest missing completion_policy object")
        return

    completed_statuses = policy.get("completed_statuses", [])
    acceptable_fields = policy.get("acceptable_completion_evidence_fields", [])
    if not isinstance(completed_statuses, list) or not isinstance(acceptable_fields, list):
        errors.append("completion_policy completed statuses and evidence fields must be lists")
        return

    completion_values = [str(value).lower() for value in completed_statuses + acceptable_fields]
    for token in FORBIDDEN_COMPLETION_TOKENS:
        if any(token in value for value in completion_values):
            errors.append(
                f"forbidden completion token {token!r} appears in acceptable evidence policy"
            )

    require(
        policy.get("pending_counts_as_complete") is False,
        "pending_counts_as_complete must be false",
        errors,
    )
    rows = manifest.get("representative_rows", [])
    if not isinstance(rows, list):
        errors.append("representative_rows must be a list")
        return
    completed_count = 0
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            errors.append(f"representative row {index} must be an object")
            continue
        status = str(row.get("implementation_status", ""))
        counts_as_complete = row.get("counts_as_complete")
        if status == "pending_implementation":
            require(
                counts_as_complete is False,
                f"{row.get('trace_id', index)} pending row counts as complete",
                errors,
            )
        if counts_as_complete is True:
            completed_count += 1
            values = [
                status,
                str(row.get("proof_class", "")),
                str(row.get("platform_result_field", "")),
                str(row.get("evidence_artifact_field", "")),
            ]
            lowered = [value.lower() for value in values]
            for token in FORBIDDEN_COMPLETION_TOKENS:
                if any(token in value for value in lowered):
                    errors.append(
                        f"{row.get('trace_id', index)} uses forbidden completion token {token!r}"
                    )
    require(
        int(policy.get("completed_proof_row_count", -1)) == completed_count,
        (
            "completion_policy completed_proof_row_count must match completed representative rows: "
            f"policy={policy.get('completed_proof_row_count')} actual={completed_count}"
        ),
        errors,
    )


def validate_traceability(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)
    manifest = load_json(fixture_root / TRACEABILITY_MANIFEST, errors)
    if not manifest:
        return

    require(
        manifest.get("schema_id") == "scratchbird.sblr_surface.public_proof_manifest.v1",
        "traceability manifest schema_id drifted",
        errors,
    )
    require(
        manifest.get("fixture_root") == FIXTURE_DIR.as_posix(),
        "traceability manifest fixture_root must point to project/tests",
        errors,
    )
    proof_scope = manifest.get("proof_scope", {})
    require(
        isinstance(proof_scope, dict)
        and proof_scope.get("gate_kind") == "guardrail_inventory",
        "traceability manifest must identify guardrail_inventory scope",
        errors,
    )
    require(
        isinstance(proof_scope, dict)
        and proof_scope.get("product_completion_claim") is False,
        "traceability manifest must not claim product completion",
        errors,
    )
    rows = manifest.get("representative_rows", [])
    if not isinstance(rows, list):
        errors.append("traceability representative_rows must be a list")
        return
    require(len(rows) >= 6, "traceability manifest must include representative row coverage", errors)
    seen_classes = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            errors.append(f"traceability row {index} must be an object")
            continue
        trace_id = str(row.get("trace_id", f"row-{index}"))
        for field in REQUIRED_TRACEABILITY_FIELDS:
            if not row.get(field):
                errors.append(f"{trace_id} missing required field {field}")
        spec = str(row.get("canonical_spec_authority", ""))
        require(
            spec.startswith("public_release_evidence"),
            f"{trace_id} canonical_spec_authority must reference public_release_evidence",
            errors,
        )
        route = str(row.get("implementation_route_placeholder_target", ""))
        if row.get("counts_as_complete") is True:
            require(
                route.startswith("project/src/") or route.startswith("project/tests/"),
                f"{trace_id} completed implementation route must point at project source or tests",
                errors,
            )
            require(
                "placeholder" not in route.lower(),
                f"{trace_id} completed implementation route must not be a placeholder",
                errors,
            )
        else:
            require(
                "placeholder" in route,
                f"{trace_id} pending implementation route must be an explicit placeholder target",
                errors,
            )
        platform_field = str(row.get("platform_result_field", ""))
        require(
            platform_field.startswith("platform_results."),
            f"{trace_id} platform_result_field must name platform_results.*",
            errors,
        )
        artifact_field = str(row.get("evidence_artifact_field", ""))
        require(
            artifact_field.startswith("evidence_artifacts."),
            f"{trace_id} evidence_artifact_field must name evidence_artifacts.*",
            errors,
        )
        assert_manifest_references_exist(repo_root, row, errors, trace_id)
        fixture_file = str(row.get("fixture_file", ""))
        selector = row.get("row_selector", {})
        assert_row_selector_exists(fixture_root, fixture_file, selector, errors, trace_id)
        seen_classes.add(str(row.get("proof_class", "")))

    expected_classes = {
        "server_authority_exact_route_conformance",
        "migration_route_contract_conformance",
        "non_direct_function_lane_contract",
        "explicit_unsupported_refusal_contract",
        "catalog_projection_seed_rowset_contract",
        "donor_sblr_interface_closure_gate",
        "sbsql_family_reconciliation_guardrail",
        "sblr_alias_cleanup_guardrail",
        "donor_private_opcode_guardrail",
    }
    missing_classes = sorted(expected_classes - seen_classes)
    require(
        not missing_classes,
        f"traceability manifest missing proof classes: {missing_classes}",
        errors,
    )
    validate_completion_policy(manifest, errors)


def validate_determinism(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)
    manifest = load_json(fixture_root / HASH_MANIFEST, errors)
    if not manifest:
        return
    require(
        manifest.get("schema_id") == "scratchbird.sblr_surface.fixture_hashes.v1",
        "hash manifest schema_id drifted",
        errors,
    )
    require(
        manifest.get("fixture_root") == FIXTURE_DIR.as_posix(),
        "hash manifest fixture_root must point to project/tests",
        errors,
    )
    policy = manifest.get("generated_resource_policy", {})
    require(
        isinstance(policy, dict)
        and policy.get("gate_kind") == "determinism_drift_guardrail",
        "hash manifest must declare determinism_drift_guardrail policy",
        errors,
    )
    require(
        isinstance(policy, dict)
        and policy.get("product_completion_claim") is False,
        "hash manifest must not claim product completion",
        errors,
    )
    files = manifest.get("files", {})
    if not isinstance(files, dict):
        errors.append("hash manifest files must be an object")
        return
    expected_csvs = {Path(name).name for name in CSV_FILES.values()}
    actual_csvs = {path.name for path in fixture_root.glob("*.csv")}
    require(
        set(files) == expected_csvs,
        f"hash manifest file set drifted: expected {sorted(expected_csvs)}, got {sorted(files)}",
        errors,
    )
    require(
        actual_csvs == expected_csvs,
        f"fixture CSV file set drifted: expected {sorted(expected_csvs)}, got {sorted(actual_csvs)}",
        errors,
    )
    for filename, expected_hash in sorted(files.items()):
        path = fixture_root / filename
        if not path.exists():
            errors.append(f"hash manifest references missing file {filename}")
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected_hash:
            errors.append(
                f"{filename} sha256 drifted: expected {expected_hash}, found {digest}"
            )

    json_files = manifest.get("json_files", {})
    if not isinstance(json_files, dict):
        errors.append("hash manifest json_files must be an object")
        return
    expected_jsons = {TRACEABILITY_MANIFEST, SBSQL_SYNC_MANIFEST, PRIMARY_FAMILY_SNAPSHOT}
    actual_jsons = {
        path.name
        for path in fixture_root.glob("*.json")
        if path.name != HASH_MANIFEST
    }
    require(
        set(json_files) == expected_jsons,
        (
            "hash manifest json file set drifted: "
            f"expected {sorted(expected_jsons)}, got {sorted(json_files)}"
        ),
        errors,
    )
    require(
        actual_jsons == expected_jsons,
        (
            "fixture JSON file set drifted: "
            f"expected {sorted(expected_jsons)} plus {HASH_MANIFEST}, "
            f"got {sorted(actual_jsons)} plus {HASH_MANIFEST}"
        ),
        errors,
    )
    for filename, expected_hash in sorted(json_files.items()):
        path = fixture_root / filename
        if not path.exists():
            errors.append(f"hash manifest references missing JSON file {filename}")
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected_hash:
            errors.append(
                f"{filename} sha256 drifted: expected {expected_hash}, found {digest}"
            )


def validate_sync_style_and_references(
    repo_root: Path,
    row: dict[str, Any],
    accepted_styles: set[str],
    rejected_styles: set[str],
    errors: list[str],
    context: str,
) -> None:
    style = str(row.get("sbsql_style", ""))
    if style in rejected_styles or style == "donor_dialect_paste_through":
        errors.append(f"{context} uses rejected SBSQL style {style}")
    require(
        style == SBSQL_NATIVE_STYLE and style in accepted_styles,
        f"{context} must use {SBSQL_NATIVE_STYLE}",
        errors,
    )
    change_kind = str(row.get("change_kind", ""))
    require(
        change_kind in SYNC_ALLOWED_CHANGE_KINDS,
        f"{context} has unknown change_kind {change_kind}",
        errors,
    )
    lowered_values = [
        str(value).lower()
        for key, value in row.items()
        if key not in {"fixture_file", "path", "row_selector"} and not isinstance(value, dict)
    ]
    for token in FORBIDDEN_DONOR_DIALECT_TOKENS:
        if any(token in value for value in lowered_values):
            errors.append(f"{context} contains forbidden donor-dialect token {token}")
    for field in REQUIRED_SYNC_FIELDS:
        value = str(row.get(field, ""))
        if not value:
            errors.append(f"{context} missing required sync field {field}")
    require(
        str(row.get("sbsql_cst_reference", "")).startswith(
            "public_contract_snapshot"
        ),
        f"{context} sbsql_cst_reference must point at parser-v3 specs",
        errors,
    )
    require(
        "sblr" in str(row.get("ast_sblr_reference", "")).lower(),
        f"{context} ast_sblr_reference must name AST/SBLR authority",
        errors,
    )
    require(
        str(row.get("donor_normalization_reference", "")).startswith(
            "public_contract_snapshot"
        ),
        f"{context} donor_normalization_reference must point at donor common specs",
        errors,
    )
    parser_ref = str(row.get("parser_execution_plan_sync_reference", ""))
    require(
        parser_ref.startswith(PUBLIC_PARSER_HANDOFF_PREFIX),
        f"{context} parser_execution_plan_sync_reference must point at the active parser/SBLR controller execution_plan",
        errors,
    )
    require(
        PUBLIC_SBSQL_HANDOFF_PREFIX.rstrip("/") not in parser_ref,
        f"{context} must not point at the superseded SBSQL per-element pilot backlog",
        errors,
    )
    for field in ("sbsql_cst_reference", "ast_sblr_reference", "donor_normalization_reference"):
        assert_repo_reference_exists(
            repo_root,
            str(row.get(field, "")),
            errors,
            context,
            field,
        )


def validate_sbsql_full_coverage_requirements(
    repo_root: Path,
    fixture_root: Path,
    manifest: dict[str, Any],
    accepted_styles: set[str],
    rejected_styles: set[str],
    errors: list[str],
) -> None:
    policy = manifest.get("full_coverage_policy", {})
    if not isinstance(policy, dict):
        errors.append("SBSQL sync manifest missing full_coverage_policy object")
        return
    require(
        policy.get("coverage_model") == "row_expanded_by_gate",
        "SBSQL sync full_coverage_policy coverage_model drifted",
        errors,
    )
    require(
        policy.get("required_coverage_scope") == "all_rows",
        "SBSQL sync full_coverage_policy must require all_rows coverage",
        errors,
    )
    requirements = manifest.get("full_coverage_requirements", [])
    if not isinstance(requirements, list):
        errors.append("SBSQL sync full_coverage_requirements must be a list")
        return
    expected_files = set(CSV_FILES.values())
    seen_files: set[str] = set()
    total_rows = 0
    seen_coverage_ids: set[str] = set()
    for index, requirement in enumerate(requirements):
        if not isinstance(requirement, dict):
            errors.append(f"SBSQL full coverage requirement {index} must be an object")
            continue
        coverage_id = str(requirement.get("coverage_id", f"coverage-{index}"))
        if coverage_id in seen_coverage_ids:
            errors.append(f"duplicate SBSQL full coverage id {coverage_id}")
        seen_coverage_ids.add(coverage_id)
        validate_sync_style_and_references(
            repo_root,
            requirement,
            accepted_styles,
            rejected_styles,
            errors,
            coverage_id,
        )
        fixture_file = str(requirement.get("fixture_file", ""))
        if fixture_file not in expected_files:
            errors.append(f"{coverage_id} references unknown fixture file {fixture_file}")
            continue
        if fixture_file in seen_files:
            errors.append(f"duplicate SBSQL full coverage fixture file {fixture_file}")
        seen_files.add(fixture_file)
        require(
            requirement.get("coverage_scope") == "all_rows",
            f"{coverage_id} must require all_rows coverage",
            errors,
        )
        selector = str(requirement.get("row_selector_column", ""))
        rows = load_csv(fixture_root, fixture_file, errors)
        if rows:
            require(
                selector in rows[0],
                f"{coverage_id} selector column {selector} missing from {fixture_file}",
                errors,
            )
            missing_values = sum(1 for row in rows if not row.get(selector))
            require(
                missing_values == 0,
                f"{coverage_id} selector {selector} has {missing_values} empty values",
                errors,
            )
        total_rows += len(rows)
    missing_files = sorted(expected_files - seen_files)
    extra_files = sorted(seen_files - expected_files)
    require(not missing_files, f"SBSQL full coverage missing fixture files: {missing_files}", errors)
    require(not extra_files, f"SBSQL full coverage references extra fixture files: {extra_files}", errors)
    minimum_rows = int(policy.get("minimum_fixture_rows", 0) or 0)
    require(
        total_rows >= minimum_rows >= 700,
        f"SBSQL full coverage row total too small: total={total_rows} minimum={minimum_rows}",
        errors,
    )


def validate_engine_sblr_resource_sync_requirements(
    repo_root: Path,
    manifest: dict[str, Any],
    accepted_styles: set[str],
    rejected_styles: set[str],
    errors: list[str],
) -> None:
    resources = manifest.get("engine_sblr_resource_sync_requirements", [])
    if not isinstance(resources, list):
        errors.append("SBSQL sync engine_sblr_resource_sync_requirements must be a list")
        return
    seen_paths: set[str] = set()
    seen_ids: set[str] = set()
    for index, resource in enumerate(resources):
        if not isinstance(resource, dict):
            errors.append(f"SBSQL resource sync requirement {index} must be an object")
            continue
        sync_id = str(resource.get("resource_sync_id", f"resource-{index}"))
        if sync_id in seen_ids:
            errors.append(f"duplicate SBSQL resource sync id {sync_id}")
        seen_ids.add(sync_id)
        validate_sync_style_and_references(
            repo_root,
            resource,
            accepted_styles,
            rejected_styles,
            errors,
            sync_id,
        )
        path = str(resource.get("path", ""))
        if Path(path).is_absolute() or ".." in Path(path).parts:
            errors.append(f"{sync_id} path must be repo-relative: {path}")
            continue
        if path in seen_paths:
            errors.append(f"duplicate SBSQL resource sync path {path}")
        seen_paths.add(path)
        if not (repo_root / path).is_file():
            errors.append(f"{sync_id} references missing engine/SBLR resource {path}")
    missing_paths = sorted(ENGINE_SBLR_RESOURCE_SYNC_PATHS - seen_paths)
    extra_paths = sorted(seen_paths - ENGINE_SBLR_RESOURCE_SYNC_PATHS)
    require(not missing_paths, f"SBSQL resource sync missing paths: {missing_paths}", errors)
    require(not extra_paths, f"SBSQL resource sync references extra paths: {extra_paths}", errors)


def validate_sbsql_sync(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)
    manifest = load_json(fixture_root / SBSQL_SYNC_MANIFEST, errors)
    if not manifest:
        return
    require(
        manifest.get("schema_id") == "scratchbird.sblr_surface.sbsql_sync_requirements.v1",
        "SBSQL sync manifest schema_id drifted",
        errors,
    )
    require(
        manifest.get("fixture_root") == FIXTURE_DIR.as_posix(),
        "SBSQL sync manifest fixture_root must point to project/tests",
        errors,
    )
    style_policy = manifest.get("style_policy", {})
    if not isinstance(style_policy, dict):
        errors.append("SBSQL sync manifest missing style_policy object")
        return
    accepted_styles = set(style_policy.get("accepted_sbsql_styles", []))
    rejected_styles = set(style_policy.get("rejected_sbsql_styles", []))
    require(
        "sbsql_native_normalized" in accepted_styles,
        "SBSQL sync style policy must accept sbsql_native_normalized",
        errors,
    )
    require(
        "donor_dialect_paste_through" in rejected_styles,
        "SBSQL sync style policy must reject donor_dialect_paste_through",
        errors,
    )
    require(
        style_policy.get("valid_style_field") == "sbsql_style",
        "SBSQL sync style policy valid_style_field must be sbsql_style",
        errors,
    )
    require(
        tuple(manifest.get("required_reference_fields", [])) == REQUIRED_SYNC_FIELDS,
        "SBSQL sync required_reference_fields drifted",
        errors,
    )
    public_policy = manifest.get("public_release_policy", {})
    require(
        isinstance(public_policy, dict)
        and public_policy.get("proof_source") == "project_tests_manifest_and_generated_matrix",
        "SBSQL sync public_release_policy proof_source drifted",
        errors,
    )
    require(
        isinstance(public_policy, dict)
        and public_policy.get("docs_execution-plans_required_at_runtime") is False,
        "SBSQL sync must not require private execution_plan sources at runtime",
        errors,
    )
    require(
        isinstance(public_policy, dict)
        and public_policy.get("product_completion_claim") is False,
        "SBSQL sync manifest must not claim product completion",
        errors,
    )
    rows = manifest.get("rows", [])
    if not isinstance(rows, list):
        errors.append("SBSQL sync rows must be a list")
        return
    require(len(rows) >= 6, "SBSQL sync manifest must cover representative SBLR/engine row categories", errors)
    seen_files = set()
    seen_sync_ids: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            errors.append(f"SBSQL sync row {index} must be an object")
            continue
        sync_id = str(row.get("sync_id", f"row-{index}"))
        if sync_id in seen_sync_ids:
            errors.append(f"duplicate SBSQL sync row id {sync_id}")
        seen_sync_ids.add(sync_id)
        validate_sync_style_and_references(
            repo_root,
            row,
            accepted_styles,
            rejected_styles,
            errors,
            sync_id,
        )
        fixture_file = str(row.get("fixture_file", ""))
        selector = row.get("row_selector", {})
        assert_row_selector_exists(fixture_root, fixture_file, selector, errors, sync_id)
        seen_files.add(fixture_file)

    expected_fixture_files = {
        "SERVER_AUTHORITY_SURFACE_MATRIX.csv",
        "NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv",
        "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv",
        "SBSQL_SBLR_FAMILY_RECONCILIATION_MATRIX.csv",
        "SBLR_STALE_DEFERRED_ALIAS_CLEANUP_MATRIX.csv",
        "DONOR_INTERNAL_META_OPCODE_CLEANUP_MATRIX.csv",
    }
    missing = sorted(expected_fixture_files - seen_files)
    require(not missing, f"SBSQL sync manifest missing fixture categories: {missing}", errors)
    validate_sbsql_full_coverage_requirements(
        repo_root,
        fixture_root,
        manifest,
        accepted_styles,
        rejected_styles,
        errors,
    )
    validate_engine_sblr_resource_sync_requirements(
        repo_root,
        manifest,
        accepted_styles,
        rejected_styles,
        errors,
    )


def validate_interface_closure(repo_root: Path, fixture_root: Path, errors: list[str]) -> None:
    assert_project_test_fixture_root(repo_root, fixture_root, errors)

    rows_by_name = {
        key: load_csv(fixture_root, filename, errors)
        for key, filename in CSV_FILES.items()
    }
    manifest = load_json(fixture_root / TRACEABILITY_MANIFEST, errors)
    if errors:
        return

    require_count(rows_by_name["server_authority"], 318, "server authority", errors)
    require_count(rows_by_name["non_direct_function"], 258, "non-direct function/API", errors)
    require_count(rows_by_name["explicit_unsupported"], 5, "explicit unsupported", errors)
    require_count(rows_by_name["stale_deferred_alias"], 8, "stale/deferred alias", errors)
    require_count(rows_by_name["donor_private_opcode"], 34, "donor-private opcode", errors)

    server_actions = Counter(row.get("server_action", "") for row in rows_by_name["server_authority"])
    require(
        server_actions
        == Counter({
            "security_denial": 181,
            "server_policy_gate": 82,
            "migration_service_route": 55,
        }),
        f"closure server-action counts drifted: {dict(server_actions)}",
        errors,
    )
    require(
        Counter(row.get("is_sblr_execution_surface", "") for row in rows_by_name["server_authority"])
        == Counter({"no": 318}),
        "closure server authority rows must remain non-SBLR execution surfaces",
        errors,
    )
    require(
        Counter(row.get("implementation_decision", "") for row in rows_by_name["non_direct_function"])
        == Counter({
            "catalog_projection_only": 112,
            "connector_operation": 80,
            "policy_blocked": 46,
            "trusted_udr_registration": 15,
            "unsupported": 5,
        }),
        "closure non-direct function decisions drifted",
        errors,
    )

    family_rows = rows_by_name["sbsql_family"]
    unreconciled_families = [
        row
        for row in family_rows
        if row.get("primary_sblr_envelope_status") != "declared_primary"
        and row.get("reconciliation_status") != RECONCILED_FAMILY_STATUS
    ]
    require(not unreconciled_families,
            f"closure has unreconciled SBSQL family rows: {unreconciled_families}",
            errors)
    mismatched = [
        row for row in family_rows
        if row.get("primary_sblr_envelope_status") != "declared_primary"
    ]
    require_count(mismatched, EXPECTED_RECONCILED_FAMILY_ROWS,
                  "closure reconciled SBSQL family", errors)
    require(
        sum(int(row.get("row_count", "0") or "0") for row in mismatched)
        == EXPECTED_RECONCILED_SBSQL_ROWS,
        "closure reconciled SBSQL family affected-row total drifted",
        errors,
    )

    trace_rows = manifest.get("representative_rows", [])
    if not isinstance(trace_rows, list):
        errors.append("closure traceability representative_rows must be a list")
        return
    closure_classes = {
        "sbsql_family_reconciliation_guardrail",
        "server_authority_exact_route_conformance",
        "migration_route_contract_conformance",
        "non_direct_function_lane_contract",
        "explicit_unsupported_refusal_contract",
        "catalog_projection_seed_rowset_contract",
        "sblr_alias_cleanup_guardrail",
        "donor_private_opcode_guardrail",
        "donor_sblr_interface_closure_gate",
    }
    rows_by_class = {
        str(row.get("proof_class", "")): row
        for row in trace_rows
        if isinstance(row, dict)
    }
    missing_classes = sorted(closure_classes - set(rows_by_class))
    require(not missing_classes,
            f"closure traceability missing proof classes: {missing_classes}",
            errors)
    for proof_class in sorted(closure_classes):
        row = rows_by_class.get(proof_class)
        if not row:
            continue
        trace_id = str(row.get("trace_id", proof_class))
        require(
            row.get("counts_as_complete") is True,
            f"{trace_id} must count as complete for interface closure",
            errors,
        )
        require(
            str(row.get("implementation_status", "")).startswith("linux_"),
            f"{trace_id} must have a linux-proven status",
            errors,
        )
        route = str(row.get("implementation_route_placeholder_target", ""))
        require(
            route.startswith("project/src/") or route.startswith("project/tests/"),
            f"{trace_id} closure route must point at project source or tests",
            errors,
        )
        require(
            "placeholder" not in route.lower(),
            f"{trace_id} closure route must not be a placeholder",
            errors,
        )

    ctest_source = read_repo_text(repo_root, Path("project/tests/sblr_surface/CMakeLists.txt"), errors)
    for test_name in (
        "sblr_surface_sbsql_family_reconciliation_guardrail_gate",
        "sblr_surface_server_authority_route_conformance",
        "sblr_surface_migration_route_contract_conformance",
        "sblr_surface_catalog_projection_seed_conformance",
        "sblr_surface_non_direct_function_lane_conformance",
        "sblr_surface_catalog_seed_manifest_gate",
        "sblr_surface_registry_cleanup_guardrail_gate",
        "sblr_surface_donor_interface_closure_gate",
    ):
        require(test_name in ctest_source,
                f"closure CTest wiring missing {test_name}",
                errors)

    scan_for_external_paths(fixture_root, errors)


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    fixture_root = (
        Path(args.fixture_root).resolve()
        if args.fixture_root
        else (repo_root / FIXTURE_DIR).resolve()
    )
    errors: list[str] = []

    if args.mode in ("all", "inventory"):
        validate_inventory(repo_root, fixture_root, errors)
    if args.mode in ("all", "traceability"):
        validate_traceability(repo_root, fixture_root, errors)
    if args.mode == "completion-policy":
        manifest = load_json(fixture_root / TRACEABILITY_MANIFEST, errors)
        if manifest:
            validate_completion_policy(manifest, errors)
    if args.mode in ("all", "determinism"):
        validate_determinism(repo_root, fixture_root, errors)
    if args.mode in ("all", "family-reconciliation"):
        validate_family_reconciliation(repo_root, fixture_root, errors)
    if args.mode in ("all", "interface-closure"):
        validate_interface_closure(repo_root, fixture_root, errors)
    if args.mode in ("all", "registry-cleanup"):
        validate_registry_cleanup(repo_root, fixture_root, errors)
    if args.mode in ("all", "sbsql-sync"):
        validate_sbsql_sync(repo_root, fixture_root, errors)

    if errors:
        for error in errors:
            print(f"SBLR_SURFACE_GUARDRAIL_ERROR: {error}", file=sys.stderr)
        return 1
    print(f"sblr_surface_guardrail_gate: {args.mode} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
