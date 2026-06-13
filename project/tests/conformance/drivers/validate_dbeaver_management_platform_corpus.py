#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the DBeaver management-platform proof corpus."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


MANIFEST_REL = Path(
    "project/tests/conformance/drivers/fixtures/dbeaver_management_platform/manifest.json"
)
GOLDEN_REL = Path(
    "project/tests/conformance/drivers/goldens/dbeaver_management_platform/expected.json"
)
CORPUS_ID = "dbeaver_management_platform_corpus"
CORPUS_REL = Path(
    "project/tests/conformance/drivers/corpora/dbeaver_management_platform/management_corpus.json"
)

COMPONENT_ID = "adaptor:scratchbird-dbeaver-driver"
DBEAVER_SOURCE_PATH = "project/drivers/adaptor/scratchbird-dbeaver-driver"
LIVE_SERVER_FIXTURE = (
    "project/tests/conformance/drivers/fixtures/live_driver_test_server/manifest.json"
)

REQUIRED_WORKPLAN_IDS = {
    "DBEAVER-MGMT-011",
    "DBEAVER-MGMT-021",
    "DBEAVER-MGMT-036",
    "DBEAVER-MGMT-037",
    "DBEAVER-MGMT-038",
    "DBEAVER-MGMT-039",
    "DBEAVER-MGMT-040",
    "DBEAVER-MGMT-041",
    "DBEAVER-MGMT-042",
    "DBEAVER-MGMT-045",
}

REQUIRED_GATE_IDS = {item.replace("MGMT-", "MGMT-GATE-") for item in REQUIRED_WORKPLAN_IDS}

REQUIRED_LANGUAGE_PROFILES = {
    "en-US",
    "en-CA",
    "fr-CA",
    "fr-FR",
    "de-DE",
    "it-IT",
    "es-ES",
}

REQUIRED_COVERAGE = {
    "dbeaver_data_editor_batch_refresh",
    "dbeaver_data_editor_commit_rollback_autocommit_savepoint",
    "dbeaver_data_editor_generated_sbsql_sblr_refusal",
    "dbeaver_data_editor_insert_update_delete",
    "dbeaver_data_transfer_import_export",
    "dbeaver_data_transfer_transaction_authorization",
    "dbeaver_data_transfer_type_encoding",
    "dbeaver_feature_boundary_refusal",
    "dbeaver_generated_ddl_sbsql_explain",
    "dbeaver_language_fallback_standard_en",
    "dbeaver_language_no_disclosure",
    "dbeaver_live_server_clean_reset",
    "dbeaver_live_server_management_corpus",
    "dbeaver_long_operation_lifecycle",
    "dbeaver_metadata_invalidation_authorized_visibility",
    "dbeaver_multi_session_isolation",
    "dbeaver_object_graph_dependency_search",
    "dbeaver_preview_apply_parity",
    "dbeaver_sblr_uuid_server_revalidation",
    "dbeaver_server_authorization",
    "dbeaver_shared_sbsql_language_resources",
    "dbeaver_stale_object_conflict_refusal",
}

CATEGORY_REQUIREMENTS = {
    "language_and_sblr": {
        "coverage": {
            "dbeaver_shared_sbsql_language_resources",
            "dbeaver_language_fallback_standard_en",
            "dbeaver_language_no_disclosure",
            "dbeaver_sblr_uuid_server_revalidation",
        },
        "list_field": "language_profiles",
        "list_values": REQUIRED_LANGUAGE_PROFILES,
    },
    "live_server_corpus": {
        "coverage": {
            "dbeaver_live_server_management_corpus",
            "dbeaver_live_server_clean_reset",
        },
        "list_field": "live_server_paths",
        "list_values": {
            "shared_sbsql_corpus",
            "dbeaver_management_corpus",
            "clean_server_reset",
            "manager_proxy",
            "embedded_jdbc",
            "server_revalidation",
            "audit_result",
        },
    },
    "multi_session_isolation": {
        "coverage": {"dbeaver_multi_session_isolation"},
        "list_field": "isolation_paths",
        "list_values": {
            "connection_id",
            "principal",
            "role",
            "language_preference",
            "parser_cache_scope",
            "completion_cache_scope",
            "metadata_cache_scope",
            "transaction_scope",
            "background_job_scope",
            "no_cross_connection_leak",
        },
    },
    "management_authorization": {
        "coverage": {"dbeaver_server_authorization", "dbeaver_sblr_uuid_server_revalidation"},
        "list_field": "authorization_vectors",
        "list_values": {
            "principal_mismatch",
            "role_mismatch",
            "group_mismatch",
            "policy_epoch_injection",
            "tampered_sblr_uuid_bundle",
            "server_refusal_vector",
            "audit_event",
        },
    },
    "preview_apply_conflict": {
        "coverage": {
            "dbeaver_preview_apply_parity",
            "dbeaver_stale_object_conflict_refusal",
        },
        "list_field": "preview_apply_paths",
        "list_values": {
            "preview_hash",
            "applied_operation_hash",
            "server_refresh",
            "stale_object_refusal",
            "deleted_object_refusal",
            "concurrent_conflict_refusal",
        },
    },
    "data_editor_crud": {
        "coverage": {
            "dbeaver_data_editor_insert_update_delete",
            "dbeaver_data_editor_batch_refresh",
            "dbeaver_data_editor_commit_rollback_autocommit_savepoint",
            "dbeaver_data_editor_generated_sbsql_sblr_refusal",
        },
        "list_field": "data_editor_paths",
        "list_values": {
            "insert",
            "update",
            "delete",
            "batching",
            "refresh",
            "explicit_commit",
            "explicit_rollback",
            "autocommit_success",
            "autocommit_failure_rollback",
            "savepoint_rollback",
            "permission_refusal",
            "scratchbird_type_handling",
            "generated_sbsql",
            "generated_sblr_refusal",
        },
    },
    "long_operation_lifecycle": {
        "coverage": {"dbeaver_long_operation_lifecycle"},
        "list_field": "long_operation_paths",
        "list_values": {
            "progress_monitor_events",
            "cancellation",
            "timeout",
            "reconnect",
            "safe_retry",
            "partial_failure_state",
            "audit_visibility",
            "ui_thread_nonblocking",
        },
    },
    "data_transfer": {
        "coverage": {
            "dbeaver_data_transfer_import_export",
            "dbeaver_data_transfer_type_encoding",
            "dbeaver_data_transfer_transaction_authorization",
        },
        "list_field": "data_transfer_paths",
        "list_values": {
            "import",
            "export",
            "data_transfer",
            "batching",
            "scratchbird_type_conversion",
            "encoding",
            "explicit_commit",
            "rollback_on_error",
            "authorization_refusal",
            "language_resource_hash",
            "result_parity",
        },
    },
    "object_graph_tooling": {
        "coverage": {
            "dbeaver_object_graph_dependency_search",
            "dbeaver_generated_ddl_sbsql_explain",
            "dbeaver_metadata_invalidation_authorized_visibility",
        },
        "list_field": "object_graph_paths",
        "list_values": {
            "er_dependency_browse",
            "object_search",
            "generated_ddl",
            "generated_sbsql",
            "explain_plan",
            "metadata_invalidation",
            "authorized_visible_set",
            "hidden_object_negative",
        },
    },
    "feature_boundary_refusal": {
        "coverage": {"dbeaver_feature_boundary_refusal"},
        "list_field": "feature_boundary_paths",
        "list_values": {
            "unsupported_surface",
            "unavailable_surface",
            "closed_provider_only_surface",
            "enterprise_only_surface",
            "policy_denied_surface",
            "ui_disabled_or_refusal",
            "audit_result",
            "no_overclaim",
        },
    },
}

ALLOWED_PROOF_STATUSES = {
    "required_not_executed_by_static_gate",
    "contract_static_only",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[4]


def load_json(path: Path, errors: list[str]) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        errors.append(f"missing file: {path}")
    except json.JSONDecodeError as exc:
        errors.append(f"invalid json: {path}: {exc}")
    return {}


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def item_id(item: dict[str, Any]) -> str:
    return str(item.get("case_id") or "")


def collect_cases(doc: dict[str, Any]) -> list[dict[str, Any]]:
    return [case for case in as_list(doc.get("cases")) if isinstance(case, dict)]


def coverage_from(cases: list[dict[str, Any]]) -> set[str]:
    coverage: set[str] = set()
    for case in cases:
        coverage.update(str(value) for value in as_list(case.get("coverage")))
    return coverage


def set_from_list(value: Any) -> set[str]:
    return {str(item) for item in as_list(value)}


def validate_manifest(
    repo_root: Path,
    manifest: dict[str, Any],
    golden: dict[str, Any],
    errors: list[str],
) -> Path:
    if manifest.get("fixture_id") != "DBEAVER-MGMT-FIX-001":
        errors.append("manifest fixture_id must be DBEAVER-MGMT-FIX-001")
    if golden.get("fixture_id") != manifest.get("fixture_id"):
        errors.append("golden fixture_id does not match manifest fixture_id")
    if manifest.get("component_id") != COMPONENT_ID:
        errors.append("manifest component_id must be adaptor:scratchbird-dbeaver-driver")
    if manifest.get("source_path") != DBEAVER_SOURCE_PATH:
        errors.append("manifest source_path does not match DBeaver adapter")
    if manifest.get("requires_live_driver_for_static_gate") is not False:
        errors.append("static corpus gate must not require a live driver")
    if manifest.get("requires_live_server_for_static_gate") is not False:
        errors.append("static corpus gate must not require a live server")
    if manifest.get("requires_live_server_before_closure") is not True:
        errors.append("manifest must require live server proof before closure")
    if manifest.get("transaction_authority") != "server_mga_transaction_inventory":
        errors.append("manifest transaction_authority must be server_mga_transaction_inventory")
    if manifest.get("driver_finality_authority") is not False:
        errors.append("manifest driver_finality_authority must be false")
    if manifest.get("parser_execution_authority") is not False:
        errors.append("manifest parser_execution_authority must be false")
    if set_from_list(manifest.get("workplan_ids")) != REQUIRED_WORKPLAN_IDS:
        errors.append("manifest workplan_ids do not match required DBeaver management set")
    if set_from_list(manifest.get("gate_ids")) != REQUIRED_GATE_IDS:
        errors.append("manifest gate_ids do not match required DBeaver management gate set")
    if set_from_list(golden.get("required_workplan_ids")) != REQUIRED_WORKPLAN_IDS:
        errors.append("golden required_workplan_ids drifted")
    if set_from_list(golden.get("required_gate_ids")) != REQUIRED_GATE_IDS:
        errors.append("golden required_gate_ids drifted")
    if set_from_list(golden.get("required_coverage")) != REQUIRED_COVERAGE:
        errors.append("golden required_coverage drifted")
    if LIVE_SERVER_FIXTURE not in set_from_list(manifest.get("related_static_inputs")):
        errors.append("manifest must reference live driver test server fixture")
    for rel in as_list(manifest.get("related_static_inputs")):
        if not (repo_root / str(rel)).exists():
            errors.append(f"related static input does not exist: {rel}")

    corpus_refs = as_list(manifest.get("corpora"))
    if len(corpus_refs) != 1:
        errors.append("manifest must contain exactly one DBeaver management corpus")
        return repo_root / CORPUS_REL
    corpus_ref = corpus_refs[0]
    if not isinstance(corpus_ref, dict):
        errors.append("manifest corpus entry must be an object")
        return repo_root / CORPUS_REL
    if corpus_ref.get("corpus_id") != CORPUS_ID:
        errors.append("manifest corpus_id drifted")
    if corpus_ref.get("path") != str(CORPUS_REL):
        errors.append("manifest corpus path drifted")
    return repo_root / str(corpus_ref.get("path", CORPUS_REL))


def validate_case_basics(cases: list[dict[str, Any]], errors: list[str]) -> None:
    seen: set[str] = set()
    for case in cases:
        cid = item_id(case)
        if not cid.startswith("DBEAVER-MGMT-"):
            errors.append(f"case id {cid or '<missing>'} must start with DBEAVER-MGMT-")
        if cid in seen:
            errors.append(f"duplicate case id: {cid}")
        seen.add(cid)

        category = str(case.get("category", ""))
        if category not in CATEGORY_REQUIREMENTS:
            errors.append(f"{cid}: unsupported category {category}")
        if case.get("component_id") != COMPONENT_ID:
            errors.append(f"{cid}: component_id must be {COMPONENT_ID}")
        if case.get("server_revalidation") != "required":
            errors.append(f"{cid}: server_revalidation must be required")
        if case.get("server_admission") not in {"required", "refusal_required"}:
            errors.append(f"{cid}: server_admission must be required or refusal_required")
        if case.get("transaction_authority") != "server_mga_transaction_inventory":
            errors.append(f"{cid}: transaction_authority must be server_mga_transaction_inventory")
        if case.get("driver_finality_authority") is not False:
            errors.append(f"{cid}: driver_finality_authority must be false")
        if case.get("parser_execution_authority") is not False:
            errors.append(f"{cid}: parser_execution_authority must be false")
        if case.get("proof_status") not in ALLOWED_PROOF_STATUSES:
            errors.append(f"{cid}: proof_status must not claim completed live proof")
        if case.get("proof_status") == "contract_static_only" and case.get("requires_live_server") is not False:
            errors.append(f"{cid}: contract_static_only cases must not require a live server")
        if case.get("proof_status") == "required_not_executed_by_static_gate" and case.get("requires_live_server") is not True:
            errors.append(f"{cid}: live proof cases must declare requires_live_server=true")
        if case.get("required_before_completion") is not True:
            errors.append(f"{cid}: required_before_completion must be true")
        if case.get("claim_boundary") != "live_evidence_required_before_complete":
            errors.append(f"{cid}: claim_boundary must require live evidence before completion")
        if case.get("proof_status") == "complete" and not as_list(case.get("evidence_refs")):
            errors.append(f"{cid}: completed proof requires evidence_refs")


def validate_required_coverage(cases: list[dict[str, Any]], golden: dict[str, Any], errors: list[str]) -> None:
    coverage = coverage_from(cases)
    missing = REQUIRED_COVERAGE - coverage
    if missing:
        errors.append("missing required coverage: " + ",".join(sorted(missing)))

    workplan_ids: set[str] = set()
    gate_ids: set[str] = set()
    for case in cases:
        workplan_ids.update(set_from_list(case.get("workplan_ids")))
        gate_ids.update(set_from_list(case.get("gate_ids")))
    missing_workplan = REQUIRED_WORKPLAN_IDS - workplan_ids
    missing_gates = REQUIRED_GATE_IDS - gate_ids
    if missing_workplan:
        errors.append("missing workplan ids: " + ",".join(sorted(missing_workplan)))
    if missing_gates:
        errors.append("missing gate ids: " + ",".join(sorted(missing_gates)))

    expected_counts = golden.get("expected_case_counts")
    if not isinstance(expected_counts, dict):
        errors.append("golden expected_case_counts must be an object")
        return
    actual_by_category: dict[str, int] = {}
    for case in cases:
        category = str(case.get("category", ""))
        actual_by_category[category] = actual_by_category.get(category, 0) + 1
    if expected_counts != actual_by_category:
        errors.append(f"category counts drifted: expected {expected_counts}, got {actual_by_category}")


def validate_category_details(cases: list[dict[str, Any]], errors: list[str]) -> None:
    categories_seen = {str(case.get("category", "")) for case in cases}
    missing_categories = set(CATEGORY_REQUIREMENTS) - categories_seen
    if missing_categories:
        errors.append("missing categories: " + ",".join(sorted(missing_categories)))

    for case in cases:
        cid = item_id(case)
        category = str(case.get("category", ""))
        requirement = CATEGORY_REQUIREMENTS.get(category)
        if requirement is None:
            continue
        case_coverage = set_from_list(case.get("coverage"))
        missing_coverage = set(requirement["coverage"]) - case_coverage
        if missing_coverage:
            errors.append(f"{cid}: missing category coverage {sorted(missing_coverage)}")
        list_field = str(requirement["list_field"])
        list_values = set(requirement["list_values"])
        observed = set_from_list(case.get(list_field))
        missing_values = list_values - observed
        if missing_values:
            errors.append(f"{cid}: {list_field} missing {sorted(missing_values)}")
        if category == "language_and_sblr":
            negative_vectors = set_from_list(case.get("negative_vectors"))
            for value in {
                "corrupt_pack",
                "stale_epoch",
                "mismatched_schema",
                "tampered_sblr_uuid_bundle",
                "cross_principal_replay",
            }:
                if value not in negative_vectors:
                    errors.append(f"{cid}: negative_vectors missing {value}")


def validate(repo_root: Path) -> list[str]:
    errors: list[str] = []
    manifest = load_json(repo_root / MANIFEST_REL, errors)
    golden = load_json(repo_root / GOLDEN_REL, errors)
    corpus_path = validate_manifest(repo_root, manifest, golden, errors)
    corpus = load_json(corpus_path, errors)

    if corpus.get("corpus_id") != CORPUS_ID:
        errors.append("corpus_id does not match manifest")
    if corpus.get("component_id") != COMPONENT_ID:
        errors.append("corpus component_id drifted")
    if corpus.get("authority_boundary") != manifest.get("authority_boundary"):
        errors.append("corpus authority_boundary must match manifest")
    if corpus.get("does_not_close_live_server_gate") is not True:
        errors.append("corpus must declare does_not_close_live_server_gate=true")

    cases = collect_cases(corpus)
    validate_case_basics(cases, errors)
    validate_required_coverage(cases, golden, errors)
    validate_category_details(cases, errors)
    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root_from_script(),
        help="ScratchBird repository root. Defaults to this script's repository.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    errors = validate(repo_root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("dbeaver management platform corpus: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
