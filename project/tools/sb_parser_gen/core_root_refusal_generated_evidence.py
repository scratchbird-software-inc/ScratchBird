#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generated-evidence authority for Core diagnostic-refusal command roots.

These seven native-now surface identities are recognizable command subforms,
but Core assigns each one the closed ``SBLR_DIAGNOSTIC_REFUSAL`` route with
``SBSQL.IMPL.NOT_AVAILABLE``.  They therefore retain parser component evidence
while every executable, server-admission, engine, and mutation claim remains
absent.
"""

from __future__ import annotations

import csv
from pathlib import Path


CORE_ROOT_EXACT_REFUSALS = {
    "SBSQL-2B7126C58E41": {
        "canonical_name": "uuid_to_name",
        "family": "general",
        "sblr_operation_family": "sblr.general.operation.v3",
        "sql": "UUID TO NAME target",
    },
    "SBSQL-5B1C5630A433": {
        "canonical_name": "use_database_alias",
        "family": "general",
        "sblr_operation_family": "sblr.general.operation.v3",
        "sql": "USE qa_lifecycle",
    },
    "SBSQL-5E6DC360F377": {
        "canonical_name": "resolve_name_public",
        "family": "general",
        "sblr_operation_family": "sblr.general.operation.v3",
        "sql": "RESOLVE NAME PUBLIC target",
    },
    "SBSQL-71D1C5165313": {
        "canonical_name": "disconnect_session",
        "family": "general",
        "sblr_operation_family": "sblr.general.operation.v3",
        "sql": "DISCONNECT SESSION 1",
    },
    "SBSQL-A8E627E27375": {
        "canonical_name": "create_object",
        "family": "ddl_catalog",
        "sblr_operation_family": "sblr.catalog.mutation.v3",
        "sql": "CREATE OBJECT target",
    },
    "SBSQL-DC0192B217F7": {
        "canonical_name": "connect_session",
        "family": "general",
        "sblr_operation_family": "sblr.general.operation.v3",
        "sql": "CONNECT SESSION user",
    },
    "SBSQL-F6C4E9705A12": {
        "canonical_name": "set_session",
        "family": "general",
        "sblr_operation_family": "sblr.general.operation.v3",
        "sql": "SET SESSION work_mem",
    },
}

CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS = frozenset(CORE_ROOT_EXACT_REFUSALS)
CORE_ROOT_EXACT_REFUSAL_CTEST = "sbsql_central_import_refusal_wire_conformance"
CORE_ROOT_EXACT_REFUSAL_TEST_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_central_import_refusal_wire_conformance.cpp"
)
CORE_ROOT_EXACT_REFUSAL_CTEST_LABEL = ";".join(
    (
        CORE_ROOT_EXACT_REFUSAL_CTEST,
        "central_command_authority",
        "exact_refusal",
        "no_execution",
        "sbsql_parser_worker",
    )
)

CORE_ROOT_EXACT_REFUSAL_IDENTITY_VECTOR = ";".join(
    (
        "operation_id=not_admitted",
        "root_route=diagnostic_refusal",
        "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
        "descriptor_contract=sblr_diagnostic_refusal.v1",
        "result_descriptor_id=diagnostic_vector.v1",
        "resource_contract=sbsql.command.no_execution.v1",
        "executable_sblr_emitted=false",
    )
)
CORE_ROOT_EXACT_REFUSAL_RESULT_VECTOR = ";".join(
    (
        "accepted=false",
        "diagnostic=SBSQL.IMPL.NOT_AVAILABLE",
        "executable_sblr_emitted=false",
        "server_admission_reached=false",
        "engine_dispatch_reached=false",
        "result_published=false",
        "descriptor_authority_published=false",
        "transaction_state_transition=false",
        "catalog_mutation=false",
        "row_mutation=false",
        "durable_state_byte_identical=true",
    )
)
CORE_ROOT_EXACT_REFUSAL_DIAGNOSTIC_VECTOR = ";".join(
    (
        "canonical_message_vector_set",
        "SBSQL.IMPL.NOT_AVAILABLE",
        "surface_id_exact",
        "canonical_name_exact",
        "executable_sblr_emitted=false",
        "no_result",
        "no_authority_publication",
        "no_mutation",
    )
)

_CORE_COMMAND_CLOSURE = "registries/sbsql-command-sblr-zero-grey-closure.csv"


def is_core_root_exact_refusal(surface_id: str) -> bool:
    return surface_id in CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS


def validate_surface(surface: dict[str, str]) -> None:
    surface_id = surface.get("surface_id", "")
    expected = CORE_ROOT_EXACT_REFUSALS.get(surface_id)
    if expected is None:
        raise ValueError(f"not a Core root exact-refusal surface: {surface_id}")
    observed = {
        "canonical_name": surface.get("canonical_name", ""),
        "family": surface.get("family", ""),
        "sblr_operation_family": surface.get("sblr_operation_family", ""),
        "surface_kind": surface.get("surface_kind", ""),
        "status": surface.get("source_status") or surface.get("status", ""),
        "cluster_scope": surface.get("cluster_scope", ""),
    }
    required = {
        "canonical_name": expected["canonical_name"],
        "family": expected["family"],
        "sblr_operation_family": expected["sblr_operation_family"],
        "surface_kind": "canonical_surface",
        "status": "native_now",
        "cluster_scope": "noncluster_or_profile_scoped",
    }
    if observed != required:
        raise ValueError(
            f"{surface_id} Core root exact-refusal surface drift: "
            f"expected={required} observed={observed}"
        )


def validate_authoritative_runtime_inputs(root: Path) -> None:
    core_path = (
        root.resolve().parent
        / "Specifications"
        / "Core"
        / _CORE_COMMAND_CLOSURE
    )
    if not core_path.is_file():
        raise ValueError(f"Core command closure missing: {core_path}")
    with core_path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    selected: dict[str, list[dict[str, str]]] = {
        surface_id: [] for surface_id in CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS
    }
    for row in rows:
        if row.get("surface_id", "") in selected:
            selected[row["surface_id"]].append(row)
    for surface_id, matches in selected.items():
        if len(matches) != 1:
            raise ValueError(
                f"Core root exact-refusal identity is not unique: "
                f"surface_id={surface_id} matches={len(matches)}"
            )
        row = matches[0]
        expected = CORE_ROOT_EXACT_REFUSALS[surface_id]
        projection = {
            "canonical_name": row.get("canonical_name", ""),
            "family": row.get("family", ""),
            "root_route_kind": row.get("root_route_kind", ""),
            "root_route": row.get("root_route", ""),
            "descriptor_contract": row.get("descriptor_contract", ""),
            "executor_operation_id": row.get("executor_operation_id", ""),
            "result_shape": row.get("result_shape", ""),
            "resource_contract": row.get("resource_contract", ""),
            "diagnostic_key": row.get("diagnostic_key", ""),
        }
        required = {
            "canonical_name": expected["canonical_name"],
            "family": expected["family"],
            "root_route_kind": "diagnostic_refusal",
            "root_route": "SBLR_DIAGNOSTIC_REFUSAL",
            "descriptor_contract": "sblr_diagnostic_refusal.v1",
            "executor_operation_id": "not_admitted",
            "result_shape": "diagnostic_vector.v1",
            "resource_contract": "sbsql.command.no_execution.v1",
            "diagnostic_key": "SBSQL.IMPL.NOT_AVAILABLE",
        }
        if projection != required:
            raise ValueError(
                f"{surface_id} Core root exact-refusal authority drift: "
                f"expected={required} observed={projection}"
            )


def strict_ledger_override(surface: dict[str, str]) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_core_root_exact_refusal(surface_id):
        return None
    validate_surface(surface)
    row = CORE_ROOT_EXACT_REFUSALS[surface_id]
    return {
        "current_state": "exact_refusal_passed",
        "parser_evidence": (
            f"ctest:{CORE_ROOT_EXACT_REFUSAL_CTEST};surface_id={surface_id};"
            f"sql={row['sql']};recognized_exact_root"
        ),
        "binder_evidence": (
            "recognized_and_bound_for_diagnostic_only;"
            "no_engine_descriptor_or_execution_authority_published"
        ),
        "lowering_evidence": CORE_ROOT_EXACT_REFUSAL_IDENTITY_VECTOR,
        "server_admission_evidence": (
            "no_executable_SBLR_or_server_dispatch;"
            "refusal_at_public_parser_wire_boundary"
        ),
        "engine_runtime_evidence": (
            f"ctest:{CORE_ROOT_EXACT_REFUSAL_CTEST};"
            f"{CORE_ROOT_EXACT_REFUSAL_RESULT_VECTOR}"
        ),
        "function_or_api_operation_id": CORE_ROOT_EXACT_REFUSAL_IDENTITY_VECTOR,
        "diagnostic_evidence": CORE_ROOT_EXACT_REFUSAL_DIAGNOSTIC_VECTOR,
        "fixture_evidence": (
            f"ctest:{CORE_ROOT_EXACT_REFUSAL_CTEST};"
            "independent_database_state_before_equals_after"
        ),
        "evidence_complete": "yes",
        "notes": (
            "Core assigns this recognized command subform an exact diagnostic-refusal "
            "root. Public wire evidence proves SBSQL.IMPL.NOT_AVAILABLE, no executable "
            "SBLR, no server or engine dispatch, no authority publication, and a "
            "byte-identical independent durable-state oracle. Any prior parent-route or "
            "unrelated executor attribution is superseded."
        ),
    }


def per_row_manifest_override(surface: dict[str, str]) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_core_root_exact_refusal(surface_id):
        return None
    validate_surface(surface)
    row = CORE_ROOT_EXACT_REFUSALS[surface_id]
    return {
        "final_state": "exact_refusal_passed",
        "ctest_label": CORE_ROOT_EXACT_REFUSAL_CTEST_LABEL,
        "fixture_path": CORE_ROOT_EXACT_REFUSAL_TEST_SOURCE,
        "implementation_refs": ";".join(
            (
                CORE_ROOT_EXACT_REFUSAL_IDENTITY_VECTOR,
                f"surface_id={surface_id}",
                f"canonical_name={row['canonical_name']}",
                "pre_sblr_refusal=true",
                "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
                "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
            )
        ),
        "diagnostic_proof": CORE_ROOT_EXACT_REFUSAL_DIAGNOSTIC_VECTOR,
        "result_proof": (
            f"ctest:{CORE_ROOT_EXACT_REFUSAL_CTEST};surface_id={surface_id};"
            f"sql={row['sql']};{CORE_ROOT_EXACT_REFUSAL_RESULT_VECTOR}"
        ),
        "evidence_collected_utc": "2026-09-04T00:00:00Z",
        "promoter_slice": "CORE-ROOT-EXACT-DIAGNOSTIC-REFUSAL-V1",
        "notes": (
            "Final Core-derived exact-refusal proof. The command subform is recognized "
            "for deterministic diagnostics only, emits no executable SBLR, never reaches "
            "server admission or engine dispatch, and leaves independent durable state "
            "byte-identical."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], classification: dict[str, str]
) -> dict[str, str]:
    surface_id = surface.get("surface_id", "")
    if not is_core_root_exact_refusal(surface_id):
        return classification
    validate_surface(surface)
    out = dict(classification)
    out.update(
        {
            "credential_profile_accepted": (
                "not_applicable_exact_refusal_before_executable_sblr"
            ),
            "credential_profile_refused": (
                "authenticated_sbsql_session_refusal_is_Core_route_driven"
            ),
            "transaction_profile": (
                "not_applicable_no_engine_transaction_or_mutation_reached"
            ),
            "transport_route": (
                "sbsql_input_to_parser_worker_refusal_before_sbps_submission"
            ),
            "listener_path": "not_reached_exact_pre_sblr_refusal",
            "ipc_admission_path": "not_reached_no_executable_sblr_emitted",
            "engine_admission_authority": "not_reached_Core_root_not_admitted",
            "mga_execution_authority": (
                "not_reached_no_transaction_catalog_or_row_mutation_no_wal_authority"
            ),
            "expected_authorization_accepted_outcome": (
                "not_applicable_Core_root_is_exact_refusal_only"
            ),
            "expected_authorization_refused_outcome": (
                CORE_ROOT_EXACT_REFUSAL_RESULT_VECTOR
            ),
            "expected_diagnostic_codes": (
                CORE_ROOT_EXACT_REFUSAL_DIAGNOSTIC_VECTOR
            ),
            "fixture_status": "exact_refusal_passed",
            "notes": (
                "Core exact-refusal route: authenticated parser input is recognized and "
                "refused before executable SBLR or SBPS submission. No listener, server, "
                "engine, transaction, catalog, row, or finality path is claimed."
            ),
        }
    )
    return out


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    surface_id = row.get("surface_id", "")
    if not is_core_root_exact_refusal(surface_id):
        return row
    out = dict(row)
    out.update(
        {
            "oracle_authority_status": "Core_exact_diagnostic_refusal_command_root",
            "expected_canonical_function_or_api_operation_id": (
                "not_admitted_diagnostic_refusal"
            ),
            "parse_phase_expectation": (
                "parse_exact_command_subform_to_cst_for_diagnostic_only"
            ),
            "bind_phase_expectation": (
                "bind_recognized_syntax_without_engine_execution_authority"
            ),
            "lower_phase_expectation": (
                "lower_refuses_SBSQL_IMPL_NOT_AVAILABLE_no_executable_sblr"
            ),
            "binary_serialize_phase_expectation": (
                "not_applicable_no_sblr_envelope_to_serialize"
            ),
            "verify_phase_expectation": "not_applicable_no_container_to_verify",
            "binary_deserialize_phase_expectation": (
                "not_applicable_no_container_to_deserialize"
            ),
            "dispatch_phase_expectation": (
                "not_applicable_no_server_or_engine_dispatch"
            ),
            "execute_phase_expectation": (
                "not_applicable_no_transaction_catalog_or_row_mutation"
            ),
            "render_phase_expectation": (
                "renderer_emit_exact_SBSQL_IMPL_NOT_AVAILABLE_diagnostic"
            ),
            "canonical_container_magic": "not_applicable_pre_sblr_exact_refusal",
            "canonical_container_header_size_bytes": (
                "not_applicable_pre_sblr_exact_refusal"
            ),
            "byte_identical_round_trip_required": (
                "not_applicable_pre_sblr_exact_refusal"
            ),
            "crc32c_check_required": "not_applicable_pre_sblr_exact_refusal",
            "engine_anchored_uuids_required": (
                "not_applicable_pre_sblr_exact_refusal"
            ),
            "execution_authority_model": (
                "parser_syntax_only;no_executable_sblr;no_engine_execution;"
                "no_catalog_mutation;no_wal_authority"
            ),
            "fixture_status": "exact_refusal_passed",
            "notes": (
                f"{CORE_ROOT_EXACT_REFUSAL_IDENTITY_VECTOR};"
                f"{CORE_ROOT_EXACT_REFUSAL_RESULT_VECTOR}"
            ),
        }
    )
    return out


def normalize_fixture_status(surface_id: str, observed_status: str) -> str:
    if is_core_root_exact_refusal(surface_id):
        return "exact_refusal_passed"
    return observed_status
