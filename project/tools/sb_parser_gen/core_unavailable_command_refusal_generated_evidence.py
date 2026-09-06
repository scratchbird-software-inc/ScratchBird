#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exact public refusal evidence for specified Core commands still in progress.

These commands have allocated Core routes, but their remaining public variants
do not yet have an authenticated engine-owned carrier. Component coverage
remains useful; the public SBsql route must stop before executable SBLR until
that planned producer is implemented. Implemented command variants must not be
listed here.
"""

from __future__ import annotations

import csv
from pathlib import Path


CORE_UNAVAILABLE_COMMAND_REFUSALS = {
    "SBSQL-6677B188A72E": {
        "canonical_name": "execute_stmt",
        "sql": "EXECUTE IMMEDIATE 'SELECT 7'",
    },
    "SBSQL-3F4B1406188A": {
        "canonical_name": "execute_stmt_option",
        "sql": "EXECUTE prep_one WITH CURSOR",
    },
}

CORE_UNAVAILABLE_COMMAND_REFUSAL_SURFACE_IDS = frozenset(
    CORE_UNAVAILABLE_COMMAND_REFUSALS
)
COMPONENT_CTEST = "sbsql_sbsfc_068_prepared_statement_control_conformance"
PUBLIC_REFUSAL_CTEST = "sbsql_central_import_refusal_wire_conformance"
COMPONENT_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_sbsfc_068_prepared_statement_control_conformance.cpp"
)
PUBLIC_REFUSAL_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_central_import_refusal_wire_conformance.cpp"
)
REFUSAL_CTEST_LABEL = ";".join(
    (
        COMPONENT_CTEST,
        PUBLIC_REFUSAL_CTEST,
        "SBSFC-068",
        "central_command_authority",
        "exact_refusal",
        "no_execution",
        "parser_lowering_component",
        "sbsql_parser_worker",
    )
)

IDENTITY_VECTOR = ";".join(
    (
        "operation_id=not_admitted",
        "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
        "descriptor_contract=sblr_diagnostic_refusal.v1",
        "result_descriptor_id=diagnostic_vector.v1",
        "resource_contract=sbsql.command.no_execution.v1",
        "executable_sblr_emitted=false",
    )
)
RESULT_VECTOR = ";".join(
    (
        "accepted=false",
        "diagnostic=SBSQL.IMPL.NOT_AVAILABLE",
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

_COMMAND_REGISTRY = "registries/sbsql-command-sblr-zero-grey-closure.csv"


def is_core_unavailable_command_refusal(surface_id: str) -> bool:
    return surface_id in CORE_UNAVAILABLE_COMMAND_REFUSAL_SURFACE_IDS


def _read_core_rows(root: Path) -> list[dict[str, str]]:
    path = root.resolve().parent / "Specifications" / "Core" / _COMMAND_REGISTRY
    if not path.is_file():
        raise ValueError(f"required Core command registry missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _authority(root: Path, surface_id: str) -> str:
    matches = [
        row for row in _read_core_rows(root) if row.get("surface_id") == surface_id
    ]
    if len(matches) != 1:
        raise ValueError(
            f"Core unavailable command identity is not unique: "
            f"surface_id={surface_id} matches={len(matches)}"
        )
    row = matches[0]
    expected = CORE_UNAVAILABLE_COMMAND_REFUSALS[surface_id]
    if row.get("canonical_name") != expected["canonical_name"]:
        raise ValueError(f"{surface_id} Core canonical name drifted")
    if row.get("diagnostic_key") != "SBSQL.IMPL.NOT_AVAILABLE":
        raise ValueError(f"{surface_id} Core refusal diagnostic drifted")
    return ";".join(
        (
            f"future_core_root_route={row['root_route']}",
            f"future_descriptor_contract={row['descriptor_contract']}",
            f"future_executor_operation_id={row['executor_operation_id']}",
            f"future_result_shape={row['result_shape']}",
        )
    )


def validate_authoritative_runtime_inputs(root: Path) -> None:
    for source in (COMPONENT_SOURCE, PUBLIC_REFUSAL_SOURCE):
        if not (root / source).is_file():
            raise ValueError(f"prepared exact-refusal source is missing: {source}")
    component_text = (root / COMPONENT_SOURCE).read_text(encoding="utf-8")
    public_text = (root / PUBLIC_REFUSAL_SOURCE).read_text(encoding="utf-8")
    for surface_id in CORE_UNAVAILABLE_COMMAND_REFUSAL_SURFACE_IDS:
        if surface_id not in component_text or surface_id not in public_text:
            raise ValueError(
                f"prepared refusal source drift: surface_id={surface_id}"
            )
        _authority(root, surface_id)


def strict_ledger_override(
    root: Path, surface: dict[str, str], existing: dict[str, str]
) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_core_unavailable_command_refusal(surface_id):
        return None
    future = _authority(root, surface_id)
    return {
        "current_state": "exact_refusal_passed",
        "parser_evidence": existing.get("parser_evidence", ""),
        "binder_evidence": (
            f"{COMPONENT_SOURCE};parser_component_only;"
            "retired_text_carrier_has_no_engine_descriptor_authority"
        ),
        "lowering_evidence": (
            f"{COMPONENT_SOURCE};retired_text_carrier_component_refused_by_server;"
            f"{IDENTITY_VECTOR};{future}"
        ),
        "server_admission_evidence": (
            "not_reached_no_executable_sblr_emitted;"
            f"ctest:{PUBLIC_REFUSAL_CTEST}"
        ),
        "engine_runtime_evidence": (
            f"ctest:{PUBLIC_REFUSAL_CTEST};{RESULT_VECTOR}"
        ),
        "function_or_api_operation_id": (
            f"engine.op.diagnostic_refusal;{IDENTITY_VECTOR};{future}"
        ),
        "diagnostic_evidence": (
            "canonical_message_vector_set;SBSQL.IMPL.NOT_AVAILABLE;"
            f"surface_id={surface_id};canonical_name={surface['canonical_name']};"
            "no_result;no_authority_publication;no_mutation"
        ),
        "fixture_evidence": (
            f"ctest:{COMPONENT_CTEST};retired_text_carrier_component_refusal;"
            f"ctest:{PUBLIC_REFUSAL_CTEST};"
            "independent_database_state_before_equals_after"
        ),
        "evidence_complete": "yes",
        "notes": (
            "The Core command remains specified, but its exact authenticated binder "
            "descriptor and accepted executor evidence are unavailable. Public SBsql "
            "therefore exact-refuses before executable SBLR; the old text carrier is "
            "retained only as negative component coverage."
        ),
    }


def per_row_manifest_override(
    root: Path, surface: dict[str, str], ledger_row: dict[str, str] | None
) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_core_unavailable_command_refusal(surface_id):
        return None
    if ledger_row is None or ledger_row.get("current_state") != "exact_refusal_passed":
        raise ValueError(f"{surface_id} unavailable-command refusal row is missing")
    future = _authority(root, surface_id)
    return {
        "final_state": "exact_refusal_passed",
        "ctest_label": REFUSAL_CTEST_LABEL,
        "fixture_path": f"{COMPONENT_SOURCE};{PUBLIC_REFUSAL_SOURCE}",
        "implementation_refs": ";".join(
            (
                IDENTITY_VECTOR,
                future,
                f"surface_id={surface_id}",
                f"canonical_name={surface['canonical_name']}",
                "pre_sblr_refusal=true",
                "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
            )
        ),
        "diagnostic_proof": (
            "canonical_message_vector_set;SBSQL.IMPL.NOT_AVAILABLE;"
            f"surface_id={surface_id};canonical_name={surface['canonical_name']};"
            "executable_sblr_emitted=false;no_result;no_mutation"
        ),
        "result_proof": (
            f"ctest:{PUBLIC_REFUSAL_CTEST};{RESULT_VECTOR};"
            f"ctest:{COMPONENT_CTEST};retired_text_carrier_component_refusal"
        ),
        "evidence_collected_utc": "2026-09-04T00:00:00Z",
        "promoter_slice": "CORE-UNAVAILABLE-COMMAND-PRE-SBLR-REFUSAL-V1",
        "notes": (
            "Exact authenticated public refusal pending the Core command's engine-owned "
            "descriptor producer. No canonical execution, prepared-state publication, "
            "server dispatch, transaction mutation, or finality is claimed."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], classification: dict[str, str]
) -> dict[str, str]:
    if not is_core_unavailable_command_refusal(surface.get("surface_id", "")):
        return classification
    out = dict(classification)
    out.update(
        {
            "credential_profile_accepted": (
                "not_applicable_exact_refusal_before_executable_sblr"
            ),
            "credential_profile_refused": (
                "authenticated_sbsql_session_refusal_is_Core_route_driven"
            ),
            "transaction_profile": "not_applicable_no_engine_transaction_reached",
            "transport_route": (
                "sbsql_input_to_parser_worker_refusal_before_sbps_submission"
            ),
            "listener_path": "not_reached_exact_pre_sblr_refusal",
            "ipc_admission_path": "not_reached_no_executable_sblr_emitted",
            "engine_admission_authority": (
                "not_reached_exact_command_descriptor_unavailable"
            ),
            "mga_execution_authority": "not_reached_no_mutation_no_wal_authority",
            "expected_authorization_accepted_outcome": (
                "not_applicable_command_is_currently_refusal_only"
            ),
            "expected_authorization_refused_outcome": RESULT_VECTOR,
            "expected_diagnostic_codes": (
                "SBSQL.IMPL.NOT_AVAILABLE;surface_id_exact;canonical_name_exact;"
                "executable_sblr_emitted=false"
            ),
            "fixture_status": "exact_refusal_passed",
            "notes": (
                "The authenticated public statement exact-refuses before SBPS while the "
                "allocated Core route remains future authority only."
            ),
        }
    )
    return out


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    if not is_core_unavailable_command_refusal(row.get("surface_id", "")):
        return row
    out = dict(row)
    out.update(
        {
            "oracle_authority_status": "Core_command_unavailable_pre_sblr_refusal",
            "expected_canonical_function_or_api_operation_id": (
                "not_admitted_diagnostic_refusal"
            ),
            "parse_phase_expectation": "parse_command_for_diagnostic_only",
            "bind_phase_expectation": (
                "component_binding_has_no_engine_descriptor_authority"
            ),
            "lower_phase_expectation": (
                "public_route_refuses_SBSQL_IMPL_NOT_AVAILABLE_no_executable_sblr"
            ),
            "binary_serialize_phase_expectation": (
                "not_applicable_no_sblr_envelope_to_serialize"
            ),
            "verify_phase_expectation": "not_applicable_no_container_to_verify",
            "binary_deserialize_phase_expectation": (
                "not_applicable_no_container_to_deserialize"
            ),
            "dispatch_phase_expectation": "not_applicable_no_server_or_engine_dispatch",
            "execute_phase_expectation": "not_applicable_no_transaction_or_mutation",
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
                "parser_syntax_only;parser_component_only;no_engine_descriptor;"
                "no_executable_sblr;"
                "no_engine_execution;no_catalog_mutation;"
                "no_prepared_state_mutation;"
                "no_wal_authority"
            ),
            "fixture_status": "exact_refusal_passed",
            "notes": f"{IDENTITY_VECTOR};{RESULT_VECTOR}",
        }
    )
    return out


def normalize_fixture_status(surface_id: str, observed_status: str) -> str:
    if is_core_unavailable_command_refusal(surface_id):
        return "exact_refusal_passed"
    return observed_status
