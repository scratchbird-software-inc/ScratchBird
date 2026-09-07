#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reviewed evidence for the bounded CREATE/CALL/NULL procedure profile.

The profile is deliberately closed: an empty procedure signature, one typed
``psql_null_stmt`` body node, and zero-argument invocation.  The public proof
crosses SBWP 1.1 over TLS, canonical SBLR admission, the engine public ABI,
MGA-backed catalog/executable-object publication, explicit transaction
finality, an independent session, and restart.  It does not promote procedure
parameters, output values, nested calls, or any other procedural node.
"""

from __future__ import annotations

from dataclasses import dataclass
import csv
from pathlib import Path


FULL_ROUTE_CTEST = "sbsql_procedure_lifecycle_full_route_gate"
FULL_ROUTE_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_procedure_lifecycle_full_route_gate.py"
)
CREATE_PROCESS_CTEST = (
    "sbsql_sblr_alignment_ia08_ddl_create_procedure_process_e2e"
)
INVOKE_PROCESS_CTEST = (
    "sbsql_sblr_alignment_ia06_procedure_invoke_process_e2e"
)
PROCESS_SOURCE = (
    "project/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
)
PROCESS_CLIENT_SOURCE = (
    "project/tests/sbsql_sblr_alignment/ia01_source_map_process_client.cpp"
)
NULL_RUNTIME_CTEST = "sbsql_sblr_alignment_ia06_procedural_null_body_runtime"
NULL_RUNTIME_SOURCE = (
    "project/tests/sbsql_sblr_alignment/ia06_procedural_null_body_runtime_test.cpp"
)


@dataclass(frozen=True)
class ProceduralLifecycleSurface:
    surface_id: str
    canonical_name: str
    surface_kind: str
    canonical_family: str
    role: str
    sql: str
    operation_id: str
    opcode: str
    opcode_code: int
    operand: str
    result: str
    engine_api: str
    csc: int

    @property
    def is_create(self) -> bool:
        return self.operation_id == "engine.op.ddl_create_procedure"

    @property
    def is_null_node(self) -> bool:
        return self.canonical_name == "psql_null_stmt"


PROCEDURAL_LIFECYCLE_SURFACES: tuple[ProceduralLifecycleSurface, ...] = (
    ProceduralLifecycleSurface(
        "SBSQL-13F5A8364A50",
        "create_procedure_stmt",
        "grammar_production",
        "sblr.catalog.mutation.v3",
        "create_root",
        "CREATE PROCEDURE users.public.tls_null_procedure AS BEGIN NULL; END;",
        "engine.op.ddl_create_procedure",
        "SBLR_DDL_CREATE_PROCEDURE",
        1554,
        "create_procedure_descriptor",
        "ddl_result",
        "EngineCreateProcedure",
        5800,
    ),
    ProceduralLifecycleSurface(
        "SBSQL-B5E9C0943E63",
        "procedure_signature",
        "grammar_production",
        "sblr.general.operation.v3",
        "empty_signature_child",
        "CREATE PROCEDURE users.public.tls_null_procedure AS BEGIN NULL; END;",
        "engine.op.ddl_create_procedure",
        "SBLR_DDL_CREATE_PROCEDURE",
        1554,
        "create_procedure_descriptor",
        "ddl_result",
        "EngineCreateProcedure",
        5801,
    ),
    ProceduralLifecycleSurface(
        "SBSQL-F3006C91D952",
        "call",
        "canonical_surface",
        "sblr.general.operation.v3",
        "invoke_root",
        "CALL users.public.tls_null_procedure();",
        "engine.op.procedure_invoke",
        "SBLR_PROCEDURE_INVOKE",
        1030,
        "procedure_invoke_descriptor",
        "procedure_result",
        "EngineInvokeExecutableObject",
        5806,
    ),
    ProceduralLifecycleSurface(
        "SBSQL-FAC34DDEAC9D",
        "call_stmt",
        "grammar_production",
        "sblr.general.operation.v3",
        "invoke_subform",
        "CALL users.public.tls_null_procedure();",
        "engine.op.procedure_invoke",
        "SBLR_PROCEDURE_INVOKE",
        1030,
        "procedure_invoke_descriptor",
        "procedure_result",
        "EngineInvokeExecutableObject",
        5807,
    ),
    ProceduralLifecycleSurface(
        "SBSQL-5AFD1BFCCEC8",
        "psql_null_stmt",
        "grammar_production",
        "sblr.management.control.v3",
        "typed_body_child",
        "CREATE PROCEDURE users.public.tls_null_procedure AS BEGIN NULL; END; CALL users.public.tls_null_procedure();",
        "engine.op.procedure_invoke",
        "SBLR_PROCEDURE_INVOKE",
        1030,
        "procedure_invoke_descriptor",
        "procedure_result",
        "EngineInvokeExecutableObject",
        5796,
    ),
)

PROCEDURAL_LIFECYCLE_BY_SURFACE_ID = {
    row.surface_id: row for row in PROCEDURAL_LIFECYCLE_SURFACES
}

_COMMAND_REGISTRY = "registries/sbsql-command-sblr-zero-grey-closure.csv"
_GRAMMAR_REGISTRY = "registries/normalized-grammar-surface-binding-registry.csv"


def is_procedural_lifecycle_surface(surface_id: str) -> bool:
    return surface_id in PROCEDURAL_LIFECYCLE_BY_SURFACE_ID


def _read_rows(path: Path, selected: set[str]) -> dict[str, dict[str, str]]:
    rows: dict[str, dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            surface_id = row.get("surface_id", "")
            if surface_id not in selected:
                continue
            if surface_id in rows:
                raise ValueError(f"duplicate procedural authority row {surface_id}")
            rows[surface_id] = row
    return rows


def _require_tokens(path: Path, tokens: tuple[str, ...]) -> None:
    if not path.is_file():
        raise ValueError(f"procedural evidence source missing: {path}")
    text = path.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise ValueError(f"procedural evidence source drift: {path}: {missing}")


def validate_authoritative_runtime_inputs(repo_root: Path) -> None:
    core = repo_root.resolve().parent / "Specifications" / "Core"
    command_ids = {
        row.surface_id
        for row in PROCEDURAL_LIFECYCLE_SURFACES
        if row.canonical_name != "procedure_signature"
    }
    command_rows = _read_rows(core / _COMMAND_REGISTRY, command_ids)
    grammar_rows = _read_rows(
        core / _GRAMMAR_REGISTRY,
        {row.surface_id for row in PROCEDURAL_LIFECYCLE_SURFACES},
    )
    for surface in PROCEDURAL_LIFECYCLE_SURFACES:
        grammar = grammar_rows.get(surface.surface_id)
        if grammar is None or grammar.get("canonical_name") != surface.canonical_name:
            raise ValueError(f"procedural grammar authority drift: {surface.surface_id}")
        if surface.canonical_name == "procedure_signature":
            if grammar.get("normalization_decision") != "existing_sbsql_lowering":
                raise ValueError("procedure signature parent-binding authority drift")
            continue
        command = command_rows.get(surface.surface_id)
        if command is None:
            raise ValueError(f"procedural command authority missing: {surface.surface_id}")
        expected_root = (
            "sblr.psql.node.psql_null_stmt.v1"
            if surface.is_null_node
            else surface.opcode
        )
        expected_executor = (
            "engine.psql.ir.psql_null_stmt"
            if surface.is_null_node
            else surface.operation_id
        )
        if (
            command.get("root_route") != expected_root
            or command.get("executor_operation_id") != expected_executor
            or command.get("implementation_state") != "evidence_required"
        ):
            raise ValueError(f"procedural command tuple drift: {surface.surface_id}")

    _require_tokens(
        repo_root / FULL_ROUTE_SOURCE,
        tuple(row.surface_id for row in PROCEDURAL_LIFECYCLE_SURFACES)
        + (
            "CREATE PROCEDURE",
            "CALL {name}()",
            "engine.op.ddl_create_procedure",
            "SBLR_DDL_CREATE_PROCEDURE",
            "engine.op.procedure_invoke",
            "SBLR_PROCEDURE_INVOKE",
            "restart",
            "rollback",
        ),
    )
    _require_tokens(
        repo_root / PROCESS_CLIENT_SOURCE,
        (
            "typed_null_body=true",
            "engine.op.ddl_create_procedure",
            "create_procedure_descriptor",
            "engine.op.procedure_invoke",
            "procedure_invoke_descriptor",
        ),
    )
    _require_tokens(
        repo_root / PROCESS_SOURCE,
        (
            "DDL_CREATE_PROCEDURE",
            "PROCEDURE_INVOKE",
            "restart",
            "rollback",
            "no_state_mutation=true",
        ),
    )
    _require_tokens(
        repo_root / NULL_RUNTIME_SOURCE,
        (
            "MakeSblrPsqlNullProceduralBodyV1",
            "EncodeSblrProceduralBodyV1",
            "DecodeSblrProceduralBodyV1",
        ),
    )


def _validate_surface(surface: dict[str, str]) -> ProceduralLifecycleSurface | None:
    evidence = PROCEDURAL_LIFECYCLE_BY_SURFACE_ID.get(surface["surface_id"])
    if evidence is None:
        return None
    status = surface.get("source_status") or surface.get("status")
    if (
        surface.get("canonical_name") != evidence.canonical_name
        or surface.get("surface_kind") != evidence.surface_kind
        or surface.get("sblr_operation_family") != evidence.canonical_family
        or surface.get("cluster_scope") != "noncluster_or_profile_scoped"
        or status != "native_now"
    ):
        raise ValueError(f"procedural surface authority drift: {evidence.surface_id}")
    return evidence


def strict_ledger_override(
    repo_root: Path, surface: dict[str, str]
) -> dict[str, str] | None:
    evidence = _validate_surface(surface)
    if evidence is None:
        return None
    create_detail = (
        "syntax_only_PCQX_v2=true;receipt_private_PCDX=true;"
        "engine_issued_PCDO_488=true;exact_PCRS_320=true;"
        "empty_signature_v1=true;typed_null_body_288=true"
    )
    invoke_detail = (
        "syntax_only_PIRQ_v2=true;receipt_private_PIDX=true;"
        "engine_issued_PIDO_488=true;exact_PIRS_320=true;"
        "zero_arguments_v1=true;zero_outputs_v1=true;typed_null_body_executed=true"
    )
    if evidence.is_create:
        authority_detail = create_detail
        process_ctest = CREATE_PROCESS_CTEST
    else:
        authority_detail = invoke_detail
        process_ctest = INVOKE_PROCESS_CTEST
    if evidence.is_null_node:
        authority_detail = (
            f"{create_detail};{invoke_detail};"
            "procedural_node=sblr.psql.node.psql_null_stmt.v1;"
            "node_executor=engine.psql.ir.psql_null_stmt;"
            "node_is_not_standalone_package_root=true"
        )

    return {
        "current_state": "e2e_passed",
        "parser_evidence": (
            f"{FULL_ROUTE_SOURCE};sql={evidence.sql};"
            f"registry_surface_id={evidence.surface_id};row_role={evidence.role};"
            "SBWP_1_1_over_TLS=true;parser_executes_sql=false"
        ),
        "binder_evidence": (
            f"{PROCESS_CLIENT_SOURCE};authenticated_statement_receipt=true;"
            f"{authority_detail};catalog_security_resource_transaction_epochs_bound=true;"
            "authority.parser.no_catalog_identity;authority.parser.no_storage_or_finality;"
            "authority.parser.no_sql_text_execution"
        ),
        "lowering_evidence": (
            f"{PROCESS_CLIENT_SOURCE};operation_id={evidence.operation_id};"
            f"sblr_operation={evidence.opcode};opcode_code={evidence.opcode_code};"
            f"operand={evidence.operand};result={evidence.result};"
            f"row_surface_id={evidence.surface_id};row_role={evidence.role};"
            "canonical_three_member_package=true;canonical_container_byte_round_trip=true;"
            "source_sql_not_execution_authority=true"
        ),
        "server_admission_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};authenticated_TLS_listener=true;"
            f"operation_id={evidence.operation_id};opcode={evidence.opcode};"
            "requires_public_abi_dispatch=true;exact_executor_evidence=true"
        ),
        "engine_runtime_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};ctest:{process_ctest};"
            f"api={evidence.engine_api};{authority_detail};explicit_commit=true;"
            "independent_authenticated_session=true;restart_recovery=true;"
            "rollback_nonvisibility=true;publication_barrier=true;"
            "prepublication_refusal_no_mutation=true;no_generic_sql_execution;"
            "no_wal_authority"
        ),
        "function_or_api_operation_id": (
            f"{evidence.operation_id};opcode={evidence.opcode};"
            f"api={evidence.engine_api};row_role={evidence.role};"
            + (
                "procedural_node=sblr.psql.node.psql_null_stmt.v1;"
                "node_executor=engine.psql.ir.psql_null_stmt"
                if evidence.is_null_node
                else "bounded_profile=empty_signature_null_body_zero_arguments_v1"
            )
        ),
        "diagnostic_evidence": (
            f"ctest:{CREATE_PROCESS_CTEST};ctest:{INVOKE_PROCESS_CTEST};"
            "SBLR.OPERAND.INVALID;SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING;"
            "PROCESS.CANCELLED;SECURITY.ACCESS_DENIED;CATALOG.NAME.NOT_FOUND;"
            "all_prepublication_refusals_preserve_catalog_name_and_executable_journals"
        ),
        "fixture_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};source={FULL_ROUTE_SOURCE};"
            f"ctest:{process_ctest};source={PROCESS_SOURCE};fixture={evidence.sql};"
            f"row_surface_id={evidence.surface_id};"
            "commit_independent_session_restart_and_rollback_observed=true"
        ),
        "evidence_complete": "yes",
        "notes": (
            "IA-06 bounded procedure lifecycle V1 is implemented end to end for "
            f"{evidence.surface_id} {evidence.canonical_name}. Evidence proves the "
            "public SBsql/SBWP TLS route, receipt-private engine binding, exact canonical "
            "SBLR carrier, engine public-ABI execution, explicit MGA transaction finality, "
            "independent post-state, restart reconstruction, rollback non-visibility, and "
            "typed NULL-body execution. The admitted profile is limited to an empty "
            "procedure signature, one psql_null_stmt body node, zero invocation arguments, "
            "and zero outputs. Parameters, results, nested calls, other PSQL nodes, ALTER/"
            "DROP PROCEDURE, cluster-positive execution, parser-owned identity/finality, "
            "and WAL authority remain planned or in progress and are not claimed."
        ),
    }


def per_row_manifest_override(
    repo_root: Path,
    surface: dict[str, str],
    ledger_row: dict[str, str] | None,
) -> dict[str, str] | None:
    del repo_root
    evidence = _validate_surface(surface)
    if evidence is None:
        return None
    if ledger_row is None or ledger_row.get("current_state") != "e2e_passed":
        raise ValueError(
            f"procedural lifecycle row lacks strict E2E evidence: {evidence.surface_id}"
        )
    implementation_refs = (
        f"operation_id={evidence.operation_id};opcode={evidence.opcode};"
        f"opcode_code={evidence.opcode_code};operand={evidence.operand};"
        f"result={evidence.result};api={evidence.engine_api};"
        f"registry_surface_id={evidence.surface_id};row_role={evidence.role};"
        "authenticated_statement_receipt=true;receipt_private_engine_authority=true;"
        "canonical_three_member_package=true;server_public_abi_dispatch=true;"
        "SBWP_1_1_TLS_listener_route=true;independent_post_state=true;"
        "explicit_transaction_finality=true;restart_recovery=true;"
        "rollback_nonvisibility=true;no_source_sql_execution_authority;"
        "bounded_profile=empty_signature_null_body_zero_arguments_zero_outputs_v1"
    )
    if evidence.is_null_node:
        implementation_refs += (
            ";procedural_node=sblr.psql.node.psql_null_stmt.v1"
            ";node_executor=engine.psql.ir.psql_null_stmt"
            ";typed_body_bytes=288;node_is_not_standalone_package_root=true"
        )
    labels = ";".join(
        (
            FULL_ROUTE_CTEST,
            CREATE_PROCESS_CTEST,
            INVOKE_PROCESS_CTEST,
            NULL_RUNTIME_CTEST,
            "sbsql_parser_worker",
            "sbsql_sblr_alignment",
            "sbsql_surface_to_sblr_full_implementation_closure",
            "sblr_operation_matrix_gate",
            "sbsql_e2e_passed",
            "authenticated_full_route",
            "independent_post_state",
            "restart_recovery",
            evidence.opcode,
            evidence.surface_id,
            f"CSC-TEST-{evidence.csc:06d}",
        )
    )
    return {
        "final_state": "e2e_passed",
        "ctest_label": labels,
        "fixture_path": ";".join(
            (
                FULL_ROUTE_SOURCE,
                PROCESS_SOURCE,
                PROCESS_CLIENT_SOURCE,
                NULL_RUNTIME_SOURCE,
            )
        ),
        "implementation_refs": implementation_refs,
        "diagnostic_proof": (
            "canonical_message_vector_set;"
            f"ctest:{CREATE_PROCESS_CTEST};ctest:{INVOKE_PROCESS_CTEST};"
            "SBLR.OPERAND.INVALID;SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING;"
            "PROCESS.CANCELLED;SECURITY.ACCESS_DENIED;CATALOG.NAME.NOT_FOUND;"
            "prepublication_refusals_publish_no_catalog_name_or_executable_mutation"
        ),
        "result_proof": (
            f"ctest:{FULL_ROUTE_CTEST};fixture={evidence.sql};"
            f"row_surface_id={evidence.surface_id};operation_id={evidence.operation_id};"
            f"opcode={evidence.opcode};authenticated_full_route=true;"
            "explicit_commit=true;independent_authenticated_session=true;"
            "restart_post_state=true;rollback_nonvisibility=true;"
            "CONNECT_only_SECURITY_ACCESS_DENIED_no_mutation=true;"
            "typed_null_body=true;zero_arguments=true;zero_outputs=true;"
            "publication_barrier=true"
        ),
        "evidence_collected_utc": "authenticated_tls_and_restart_process_evidence_2026-09-07",
        "promoter_slice": "IA-06-PROCEDURE-LIFECYCLE-NULL-V1",
        "notes": (
            "Bounded CREATE PROCEDURE/CALL lifecycle evidence for "
            f"{evidence.surface_id} {evidence.canonical_name}. The public TLS route and "
            "independent direct-SBPS process route prove exact engine-bound descriptors, "
            "canonical opcodes, engine API execution, durable catalog/executable-object "
            "publication, explicit commit, independent and restarted post-state, rollback "
            "non-visibility, and typed NULL-body execution. Only the empty-signature, "
            "zero-argument, zero-output, single-NULL-node V1 profile is promoted; every "
            "broader procedural shape remains planned or in progress."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], current: dict[str, str]
) -> dict[str, str]:
    evidence = _validate_surface(surface)
    if evidence is None:
        return current
    result = dict(current)
    result.update(
        {
            "credential_profile_accepted": (
                "durable_authenticated_principal_with_CONNECT_CATALOG_MUTATE_and_EXECUTE"
            ),
            "credential_profile_refused": (
                "durable_authenticated_CONNECT_only_principal_without_CATALOG_MUTATE_or_EXECUTE"
            ),
            "transaction_profile": (
                "mga_authority_required;explicit_transaction_for_definition_and_invocation;"
                "commit_rollback_and_restart_visibility_proven"
            ),
            "transport_route": (
                "sbwp_1.1_over_tls_1.3;sb_listener;pool_allocated_sbp_sbsql_worker;"
                "SBPS;sb_server;engine_public_abi"
            ),
            "ipc_admission_path": (
                "authenticated_statement_receipt;procedure_bind_request;"
                "receipt_private_engine_descriptor;canonical_three_member_sblr_package;"
                "public_abi_dispatch"
            ),
            "engine_admission_authority": (
                "engine_owned_name_catalog_security_transaction_resource_and_executor_epochs;"
                "PCQX_PCDX_PCDO_or_PIRQ_PIDX_PIDO_exact_authority;"
                "parser_copies_only_engine_bound_descriptor"
            ),
            "mga_execution_authority": (
                "mga_copy_on_write;explicit_commit_or_rollback;"
                "durable_catalog_name_and_executable_object_journals;no_wal_authority"
            ),
            "expected_authorization_accepted_outcome": (
                f"{evidence.operation_id.replace('.', '_')}_executes_through_"
                f"{evidence.engine_api}_with_typed_null_body_then_explicit_commit_"
                "independent_session_and_restart_post_state"
            ),
            "expected_authorization_refused_outcome": (
                "CONNECT_only_CREATE_and_CALL_refuse_SECURITY_ACCESS_DENIED_before_"
                "catalog_name_or_executable_object_mutation"
            ),
            "expected_diagnostic_codes": (
                "SECURITY.ACCESS_DENIED;CATALOG.NAME.NOT_FOUND;SBLR.OPERAND.INVALID;"
                "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING;PROCESS.CANCELLED"
            ),
            "notes": (
                "Bounded procedure lifecycle V1 is proven through SBWP 1.1 over TLS, "
                "the listener and parser pool, exact SBPS receipt-private binding, canonical "
                f"{evidence.operation_id}/{evidence.opcode}, and {evidence.engine_api}. "
                "The accepted branch commits and is observed by an independent authenticated "
                "session and after restart; rollback remains invisible. A CONNECT-only "
                "principal proves SECURITY.ACCESS_DENIED for both CREATE and CALL with "
                "byte-identical durable catalog/name/executable state. The profile is limited "
                "to an empty signature, a single typed NULL body node, zero arguments, and "
                "zero outputs; broader procedural shapes remain planned or in progress."
            ),
        }
    )
    return result


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    evidence = PROCEDURAL_LIFECYCLE_BY_SURFACE_ID.get(row.get("surface_id", ""))
    if evidence is None:
        return row
    result = dict(row)
    result.update(
        {
            "oracle_authority_status": "per_row_manifest_procedural_lifecycle_v1",
            "expected_canonical_function_or_api_operation_id": evidence.operation_id,
            "parse_phase_expectation": (
                "parse_exact_CREATE_PROCEDURE_or_CALL_and_typed_NULL_body_to_CST_pass"
            ),
            "bind_phase_expectation": (
                "bind_with_authenticated_receipt_private_engine_authority_"
                "PCQX_PCDX_or_PIRQ_PIDX_pass"
            ),
            "lower_phase_expectation": (
                f"lower_to_{evidence.operation_id}_{evidence.opcode}_with_exact_"
                f"{evidence.operand}_pass_no_source_text_authority"
            ),
            "binary_serialize_phase_expectation": (
                "serialize_exact_three_member_operation_to_canonical_container_crc32c_pass"
            ),
            "verify_phase_expectation": (
                "verify_exact_descriptor_evidence_epochs_and_executor_availability_pass"
            ),
            "binary_deserialize_phase_expectation": (
                "deserialize_to_byte_identical_canonical_container_and_descriptor_pass"
            ),
            "dispatch_phase_expectation": (
                f"dispatch_only_by_{evidence.operation_id}_and_exact_descriptor_pass"
            ),
            "execute_phase_expectation": (
                f"{evidence.engine_api}_executes_typed_null_body_under_MGA_"
                "with_publication_barrier_pass"
            ),
            "render_phase_expectation": (
                "SBWP_command_completion_without_fabricated_rowset_pass"
            ),
            "canonical_container_magic": "0x53424C52",
            "canonical_container_header_size_bytes": "40",
            "byte_identical_round_trip_required": "yes",
            "crc32c_check_required": "yes",
            "engine_anchored_uuids_required": "yes",
            "execution_authority_model": (
                "authenticated_receipt_private_engine_descriptor;"
                "sblr_envelope_with_uuid_and_descriptor_authority_only;"
                "mga_copy_on_write;explicit_transaction_finality;no_wal_authority"
            ),
            "notes": (
                "Procedural lifecycle V1 exact round trip: public SBsql parses and binds "
                f"{evidence.canonical_name}, serializes the engine-bound descriptor into "
                f"{evidence.operation_id}/{evidence.opcode}, verifies and decodes byte-"
                "identically, dispatches through the engine public ABI, and publishes the "
                "exact result before SBWP command completion. The NULL node is carried only "
                "inside the authenticated procedure body and never as a standalone package "
                "root. Only the empty-signature, zero-argument, zero-output profile is proven."
            ),
        }
    )
    return result
