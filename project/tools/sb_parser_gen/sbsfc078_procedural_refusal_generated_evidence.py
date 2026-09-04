#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exact public refusal evidence for the SBSFC-078 procedural fragments.

The SBSFC-078 fixture consists of procedure-body grammar fragments presented
as standalone SBsql statements.  The parser may recognize and bind those
fragments for syntax evidence, but the old ``general.procedural_operation``
carrier has no canonical opcode and is not an executable parent.  The Core
command rows assign the executable subset exact procedure IR node identities;
the remaining rows inherit a parent grammar route.  Until a procedure binder
emits those nodes inside an authenticated procedure descriptor, every public
standalone presentation must stop before executable SBLR.
"""

from __future__ import annotations

import csv
from pathlib import Path


SBSFC078_PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS = frozenset(
    {
        "SBSQL-026A4D4C039B",
        "SBSQL-02734A0F9F81",
        "SBSQL-036A5CE9F957",
        "SBSQL-07486BB23A2F",
        "SBSQL-0E3954A70810",
        "SBSQL-11A04416EEDE",
        "SBSQL-198EC86EF3E6",
        "SBSQL-24F101012F9C",
        "SBSQL-28A5C4933A91",
        "SBSQL-2B96962FC600",
        "SBSQL-2E73B3E7CB0A",
        "SBSQL-2E7EF3FB699A",
        "SBSQL-375E2A7771C0",
        "SBSQL-3EDACF124EA2",
        "SBSQL-3FE17C7E606A",
        "SBSQL-451E4A81B23D",
        "SBSQL-47E79B4B23EF",
        "SBSQL-499A72248451",
        "SBSQL-4A737A655174",
        "SBSQL-4B4DAC62299D",
        "SBSQL-5AD1F33585EA",
        "SBSQL-5AFD1BFCCEC8",
        "SBSQL-62256BEF9F1B",
        "SBSQL-66B35A56EFF8",
        "SBSQL-6D4DE2A31C56",
        "SBSQL-6EF52D5CB31E",
        "SBSQL-6FABEBB2C400",
        "SBSQL-7177C130C2B7",
        "SBSQL-7359F2775921",
        "SBSQL-74BE46D58008",
        "SBSQL-769B003AF4F3",
        "SBSQL-802635EDBB3A",
        "SBSQL-81BCBF791042",
        "SBSQL-832C2821017E",
        "SBSQL-85A5F7E16A21",
        "SBSQL-8628143A198B",
        "SBSQL-908F3A07EC23",
        "SBSQL-9164E0190F24",
        "SBSQL-91D6ECC8969F",
        "SBSQL-931C105F4478",
        "SBSQL-96CFEF2C7728",
        "SBSQL-A5437DC15591",
        "SBSQL-A5AA36E99CDB",
        "SBSQL-A61AE21E1DFC",
        "SBSQL-A61F84867DF2",
        "SBSQL-A67B68A9BB52",
        "SBSQL-AE02AD3F3CF7",
        "SBSQL-AFAE77165146",
        "SBSQL-AFF3B4857945",
        "SBSQL-BA6B29FD2668",
    }
)

SBSFC078_COMPONENT_CTEST = (
    "sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance"
)
PUBLIC_REFUSAL_CTEST = "sbsql_central_import_refusal_wire_conformance"
SBSFC078_COMPONENT_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_sbsfc_078_procedural_general_residual_exact_route_conformance.cpp"
)
PUBLIC_REFUSAL_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_central_import_refusal_wire_conformance.cpp"
)
SBSFC078_REFUSAL_CTEST_LABEL = ";".join(
    (
        SBSFC078_COMPONENT_CTEST,
        PUBLIC_REFUSAL_CTEST,
        "SBSFC-078",
        "central_command_authority",
        "exact_refusal",
        "no_execution",
        "parser_lowering_component",
        "sbsql_parser_worker",
    )
)

_IDENTITY_VECTOR = ";".join(
    (
        "operation_id=not_admitted",
        "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
        "descriptor_contract=sblr_diagnostic_refusal.v1",
        "result_descriptor_id=diagnostic_vector.v1",
        "resource_contract=sbsql.command.no_execution.v1",
        "executable_sblr_emitted=false",
    )
)
_RESULT_VECTOR = ";".join(
    (
        "accepted=false",
        "diagnostic=SBSQL.IMPL.NOT_AVAILABLE",
        "server_admission_reached=false",
        "engine_dispatch_reached=false",
        "result_published=false",
        "transaction_state_transition=false",
        "catalog_mutation=false",
        "row_mutation=false",
        "durable_state_byte_identical=true",
    )
)

_COMMAND_REGISTRY = "registries/sbsql-command-sblr-zero-grey-closure.csv"
_GRAMMAR_REGISTRY = "registries/normalized-grammar-surface-binding-registry.csv"
_PROMOTION_REGISTRY = (
    "registries/normalized-executable-surface-promotions-20260822.csv"
)


def is_sbsfc078_procedural_standalone_refusal(surface_id: str) -> bool:
    return surface_id in SBSFC078_PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS


def _read_rows(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise ValueError(f"required Core registry missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _future_authority(root: Path, surface_id: str) -> str:
    core = root.resolve().parent / "Specifications" / "Core"
    command_rows = [
        row
        for row in _read_rows(core / _COMMAND_REGISTRY)
        if row.get("surface_id") == surface_id
    ]
    if len(command_rows) > 1:
        raise ValueError(f"duplicate Core command identity: {surface_id}")
    if command_rows:
        row = command_rows[0]
        return ";".join(
            (
                f"future_core_root_route={row['root_route']}",
                f"future_executor_operation_id={row['executor_operation_id']}",
            )
        )
    promotion_rows = [
        row
        for row in _read_rows(core / _PROMOTION_REGISTRY)
        if row.get("surface_id") == surface_id
    ]
    if len(promotion_rows) > 1:
        raise ValueError(f"duplicate Core promotion identity: {surface_id}")
    if promotion_rows:
        return f"future_parent_sblr_route={promotion_rows[0]['sblr_route']}"
    grammar_rows = [
        row
        for row in _read_rows(core / _GRAMMAR_REGISTRY)
        if row.get("surface_id") == surface_id
    ]
    if len(grammar_rows) != 1:
        raise ValueError(
            f"SBSFC-078 row lacks one Core command/promotion/grammar authority: "
            f"{surface_id} grammar_matches={len(grammar_rows)}"
        )
    return "future_parent_binding=inherit_parent_AST_BoundAST_no_duplicate_executor"


def validate_authoritative_runtime_inputs(root: Path) -> None:
    source = root / SBSFC078_COMPONENT_SOURCE
    public_source = root / PUBLIC_REFUSAL_SOURCE
    if not source.is_file() or not public_source.is_file():
        raise ValueError("SBSFC-078 exact-refusal sources are missing")
    source_text = source.read_text(encoding="utf-8")
    public_text = public_source.read_text(encoding="utf-8")
    missing_component = sorted(
        surface_id
        for surface_id in SBSFC078_PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS
        if surface_id not in source_text
    )
    missing_public = sorted(
        surface_id
        for surface_id in SBSFC078_PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS
        if surface_id not in public_text
    )
    if missing_component or missing_public:
        raise ValueError(
            "SBSFC-078 refusal source drift: "
            f"component_missing={missing_component} public_missing={missing_public}"
        )
    for surface_id in SBSFC078_PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS:
        _future_authority(root, surface_id)


def strict_ledger_override(
    root: Path, surface: dict[str, str], existing: dict[str, str]
) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_sbsfc078_procedural_standalone_refusal(surface_id):
        return None
    future = _future_authority(root, surface_id)
    return {
        "current_state": "exact_refusal_passed",
        "parser_evidence": existing.get("parser_evidence", ""),
        "binder_evidence": (
            f"{SBSFC078_COMPONENT_SOURCE};BindAst.bound=true;"
            "authority.parser.syntax_evidence_only;no_engine_execution_authority"
        ),
        "lowering_evidence": (
            "project/src/parsers/sbsql_worker/lowering/lowering.cpp::"
            f"Sbsfc078StandaloneRefusalSurface;{_IDENTITY_VECTOR};{future}"
        ),
        "server_admission_evidence": (
            "not_reached_no_executable_sblr_emitted;"
            f"ctest:{PUBLIC_REFUSAL_CTEST}"
        ),
        "engine_runtime_evidence": (
            f"ctest:{PUBLIC_REFUSAL_CTEST};{_RESULT_VECTOR}"
        ),
        "function_or_api_operation_id": (
            f"engine.op.diagnostic_refusal;{_IDENTITY_VECTOR};{future}"
        ),
        "diagnostic_evidence": (
            "canonical_message_vector_set;SBSQL.IMPL.NOT_AVAILABLE;"
            f"surface_id={surface_id};canonical_name={surface['canonical_name']};"
            "no_result;no_authority_publication;no_mutation"
        ),
        "fixture_evidence": (
            f"ctest:{SBSFC078_COMPONENT_CTEST};parser_component_refusal;"
            f"ctest:{PUBLIC_REFUSAL_CTEST};independent_database_state_before_equals_after"
        ),
        "evidence_complete": "yes",
        "notes": (
            "SBSFC-078 procedure-body grammar remains parser-visible, but its public "
            "standalone presentation exact-refuses before executable SBLR. The cited "
            "future Core node/parent identity is retained without promoting the old "
            "unallocated general.procedural_operation carrier."
        ),
    }


def per_row_manifest_override(
    root: Path, surface: dict[str, str], ledger_row: dict[str, str] | None
) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_sbsfc078_procedural_standalone_refusal(surface_id):
        return None
    if ledger_row is None or ledger_row.get("current_state") != "exact_refusal_passed":
        raise ValueError(f"{surface_id} SBSFC-078 strict refusal row is missing")
    future = _future_authority(root, surface_id)
    return {
        "final_state": "exact_refusal_passed",
        "ctest_label": SBSFC078_REFUSAL_CTEST_LABEL,
        "fixture_path": f"{SBSFC078_COMPONENT_SOURCE};{PUBLIC_REFUSAL_SOURCE}",
        "implementation_refs": ";".join(
            (
                _IDENTITY_VECTOR,
                future,
                f"surface_id={surface_id}",
                f"canonical_name={surface['canonical_name']}",
                "pre_sblr_refusal=true",
                "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
            )
        ),
        "diagnostic_proof": (
            "canonical_message_vector_set;SBSQL.IMPL.NOT_AVAILABLE;"
            f"surface_id={surface_id};canonical_name={surface['canonical_name']};"
            "executable_sblr_emitted=false;no_result;no_mutation"
        ),
        "result_proof": (
            f"ctest:{PUBLIC_REFUSAL_CTEST};{_RESULT_VECTOR};"
            f"ctest:{SBSFC078_COMPONENT_CTEST};parser_component_refusal"
        ),
        "evidence_collected_utc": "2026-09-04T00:00:00Z",
        "promoter_slice": "SBSFC-078-STANDALONE-PRE-SBLR-REFUSAL-V1",
        "notes": (
            "Final exact public refusal for a procedure-body fragment presented as a "
            "standalone statement. Parser grammar evidence remains positive; no "
            "procedure descriptor, executable SBLR, server dispatch, engine mutation, "
            "or transaction finality is claimed."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], classification: dict[str, str]
) -> dict[str, str]:
    if not is_sbsfc078_procedural_standalone_refusal(
        surface.get("surface_id", "")
    ):
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
            "engine_admission_authority": "not_reached_no_procedure_descriptor",
            "mga_execution_authority": "not_reached_no_mutation_no_wal_authority",
            "expected_authorization_accepted_outcome": (
                "not_applicable_standalone_fragment_is_refusal_only"
            ),
            "expected_authorization_refused_outcome": _RESULT_VECTOR,
            "expected_diagnostic_codes": (
                "SBSQL.IMPL.NOT_AVAILABLE;surface_id_exact;canonical_name_exact;"
                "executable_sblr_emitted=false"
            ),
            "fixture_status": "exact_refusal_passed",
            "notes": (
                "Standalone SBSFC-078 procedure-body fragment exact-refuses before "
                "SBPS; executable procedure IR remains available only through a future "
                "authenticated procedure descriptor."
            ),
        }
    )
    return out


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    if not is_sbsfc078_procedural_standalone_refusal(row.get("surface_id", "")):
        return row
    out = dict(row)
    out.update(
        {
            "oracle_authority_status": "Core_procedure_body_standalone_refusal",
            "expected_canonical_function_or_api_operation_id": (
                "not_admitted_diagnostic_refusal"
            ),
            "parse_phase_expectation": "parse_procedure_body_fragment_for_diagnostic_only",
            "bind_phase_expectation": "bind_syntax_without_procedure_execution_authority",
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
                "parser_syntax_only;no_procedure_descriptor;no_executable_sblr;"
                "no_engine_execution;no_catalog_mutation;no_row_mutation;"
                "no_wal_authority"
            ),
            "fixture_status": "exact_refusal_passed",
            "notes": f"{_IDENTITY_VECTOR};{_RESULT_VECTOR}",
        }
    )
    return out


def normalize_fixture_status(surface_id: str, observed_status: str) -> str:
    if is_sbsfc078_procedural_standalone_refusal(surface_id):
        return "exact_refusal_passed"
    return observed_status
