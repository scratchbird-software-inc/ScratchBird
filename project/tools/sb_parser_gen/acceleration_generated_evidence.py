#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reviewed generated evidence for the SHOW ACCELERATION surface cohort.

``acceleration_stmt`` is syntax dispatch, ``show_acceleration`` is the sole
package root, and ``accel_show_target`` is a child spelling selected by that
root.  The three rows share one authenticated, non-mutating public route and
must never be regenerated as three executors or as the retired invented
``ACCELERATION STMT`` command.
"""

from __future__ import annotations

import csv
from pathlib import Path


PARENT_SURFACE_ID = "SBSQL-05DB282498F4"
COMMAND_SURFACE_ID = "SBSQL-DF68DFFA5C1E"
TARGET_SURFACE_ID = "SBSQL-8E570F4EEEF3"
SURFACE_IDS = frozenset(
    (PARENT_SURFACE_ID, COMMAND_SURFACE_ID, TARGET_SURFACE_ID)
)
SURFACE_AUTHORITY = {
    PARENT_SURFACE_ID: (
        "acceleration_stmt",
        "grammar_production",
        "general",
        "sblr.general.operation.v3",
        "parent_syntax_dispatch",
    ),
    COMMAND_SURFACE_ID: (
        "show_acceleration",
        "grammar_production",
        "observability",
        "sblr.observability.inspect.v3",
        "canonical_command_root",
    ),
    TARGET_SURFACE_ID: (
        "accel_show_target",
        "grammar_production",
        "observability",
        "sblr.observability.inspect.v3",
        "child_target_spelling",
    ),
}

FULL_ROUTE_CTEST = "sbsql_acceleration_full_route_gate"
FULL_ROUTE_SOURCE = (
    "project/tests/sbsql_parser_worker/sbsql_acceleration_full_route_gate.py"
)
COMPONENT_CTEST = "sbsql_observability_exact_route_conformance"
COMPONENT_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_observability_exact_route_conformance.cpp"
)
OPERATION_ID = "observability.show_acceleration"
OPCODE = "SBLR_OBSERVABILITY_SHOW_ACCELERATION"
OPCODE_CODE = 3365
OPERAND_DESCRIPTOR = "observability_show_acceleration_descriptor"
RESULT_DESCRIPTOR = "observability_show_acceleration_result"
CSC_IDS = tuple(range(2761, 2765))

_COMMAND_REGISTRY = "registries/sbsql-command-sblr-zero-grey-closure.csv"
_GAP_REGISTRY = "registries/normalized-surface-specification-gap-registry.csv"
_GRAMMAR_REGISTRY = "registries/normalized-grammar-surface-binding-registry.csv"
_OPCODE_CLOSURE = "registries/sblr-opcode-executor-zero-grey-closure.csv"


def is_acceleration_surface(surface_id: str) -> bool:
    return surface_id in SURFACE_IDS


def _read_exact_row(path: Path, key: str, value: str) -> dict[str, str]:
    rows: list[dict[str, str]] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row.get(key) == value:
                rows.append(row)
    if len(rows) != 1:
        raise ValueError(f"expected one {value} authority row in {path}, got {len(rows)}")
    return rows[0]


def _require_tokens(path: Path, tokens: tuple[str, ...]) -> None:
    if not path.is_file():
        raise ValueError(f"acceleration evidence source missing: {path}")
    text = path.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise ValueError(f"acceleration evidence source drift: {path}: {missing}")


def validate_authoritative_runtime_inputs(repo_root: Path) -> None:
    core = repo_root.resolve().parent / "Specifications" / "Core"

    parent = _read_exact_row(
        core / _COMMAND_REGISTRY, "surface_id", PARENT_SURFACE_ID
    )
    if (
        parent.get("canonical_name") != "acceleration_stmt"
        or parent.get("specification_state") != "syntax_only"
        or parent.get("root_route_kind") != "syntax_dispatch"
        or parent.get("root_route") != "child_surface_id_exact_dispatch"
        or parent.get("executor_operation_id") != "not_applicable"
        or parent.get("result_shape") != "none"
    ):
        raise ValueError("acceleration parent syntax-dispatch authority drift")

    command = _read_exact_row(
        core / _COMMAND_REGISTRY, "surface_id", COMMAND_SURFACE_ID
    )
    if (
        command.get("canonical_name") != "show_acceleration"
        or command.get("specification_state") != "specified_admitted"
        or command.get("implementation_state") != "evidence_required"
        or command.get("root_route_kind") != "sblr_opcode"
        or command.get("root_route") != OPCODE
        or command.get("descriptor_contract") != OPERAND_DESCRIPTOR
        or command.get("executor_operation_id") != OPERATION_ID
        or command.get("result_shape") != RESULT_DESCRIPTOR
        or command.get("transaction_effect") != "read"
    ):
        raise ValueError("SHOW ACCELERATION command authority drift")

    for surface_id in sorted(SURFACE_IDS):
        gap = _read_exact_row(core / _GAP_REGISTRY, "surface_id", surface_id)
        if gap.get("status") != "inherited_parent" or "no duplicate executor" not in gap.get(
            "required_action", ""
        ).lower():
            raise ValueError(f"{surface_id} acceleration parent-inheritance drift")
        grammar = _read_exact_row(
            core / _GRAMMAR_REGISTRY, "surface_id", surface_id
        )
        if grammar.get("binding_status") != "exact_ebnf_binding" or (
            "inherit parent" not in grammar.get("action", "").lower()
            or "never create an executor" not in grammar.get("action", "").lower()
        ):
            raise ValueError(f"{surface_id} acceleration grammar-binding drift")

    opcode = _read_exact_row(core / _OPCODE_CLOSURE, "opcode_name", OPCODE)
    if (
        opcode.get("opcode_code") != str(OPCODE_CODE)
        or opcode.get("operand_contract") != OPERAND_DESCRIPTOR
        or opcode.get("result_contract") != RESULT_DESCRIPTOR
        or opcode.get("executor_binding_requirement") != OPERATION_ID
        or opcode.get("specification_state") != "specified_admitted"
        or opcode.get("implementation_state") != "evidence_required"
        or opcode.get("admission_behavior")
        != "require_exact_accepted_executor_evidence_else_reject_before_dispatch"
    ):
        raise ValueError("SHOW ACCELERATION opcode authority drift")

    _require_tokens(
        repo_root.resolve().parent / "Documentation" / "sbsql-full-dialect.ebnf",
        (
            'accel_show_target ::= "ACCELERATION" | "ACCELERATORS" ;',
            "acceleration_stmt       ::= show_acceleration | alter_acceleration ;",
            'show_acceleration       ::= "SHOW"',
        ),
    )
    _require_tokens(
        repo_root / FULL_ROUTE_SOURCE,
        (
            'SQL = "SHOW ACCELERATION"',
            '"execution.interpreter"',
            '"compiler.llvm"',
            '"gpu.cuda"',
            "require_authentication_refusal",
            "require_no_application_journal_mutation",
            "process restart changed the immutable acceleration snapshot",
            "op=dml.",
            "op=index.",
        ),
    )
    _require_tokens(
        repo_root / COMPONENT_SOURCE,
        (
            PARENT_SURFACE_ID,
            COMMAND_SURFACE_ID,
            TARGET_SURFACE_ID,
            "SBLR.OPERATION.NONCANONICAL",
            "SECURITY.ACCESS_DENIED",
            "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
            "PROCESS.CANCELLED",
        ),
    )
    _require_tokens(
        repo_root
        / "project/src/engine/internal_api/observability/acceleration_registry.cpp",
        (
            '"execution.interpreter"',
            'capability == "compiler.llvm"',
            'capability.starts_with("gpu.")',
        ),
    )
    _require_tokens(
        repo_root / "project/src/engine/internal_api/observability/show_api.cpp",
        (
            'constexpr std::string_view kOperation = "observability.show_acceleration"',
            '"observability_show_acceleration_result"',
            '"observability.acceleration.cluster_route_refused"',
            '"observability.acceleration.cancelled"',
        ),
    )
    _require_tokens(
        repo_root / "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
        (OPERATION_ID, OPCODE),
    )
    _require_tokens(
        repo_root / "project/src/engine/sblr/sblr_dispatch.cpp",
        ("exact_show_acceleration", OPERATION_ID, OPCODE),
    )


def _validate_surface(surface: dict[str, str]) -> tuple[str, ...] | None:
    surface_id = surface.get("surface_id", "")
    authority = SURFACE_AUTHORITY.get(surface_id)
    if authority is None:
        return None
    canonical_name, surface_kind, family, sblr_family, _ = authority
    status = surface.get("source_status") or surface.get("status")
    if (
        surface.get("canonical_name") != canonical_name
        or surface.get("surface_kind") != surface_kind
        or surface.get("family") != family
        or surface.get("sblr_operation_family") != sblr_family
        or surface.get("cluster_scope") != "noncluster_or_profile_scoped"
        or status != "native_now"
    ):
        raise ValueError(f"{surface_id} public acceleration surface authority drift")
    return authority


def _surface_role(surface_id: str) -> str:
    return SURFACE_AUTHORITY[surface_id][4]


def strict_ledger_override(
    repo_root: Path, surface: dict[str, str]
) -> dict[str, str] | None:
    del repo_root
    if _validate_surface(surface) is None:
        return None
    surface_id = surface["surface_id"]
    role = _surface_role(surface_id)
    shared = (
        f"parent_surface_id={PARENT_SURFACE_ID};"
        f"command_surface_id={COMMAND_SURFACE_ID};"
        f"target_surface_id={TARGET_SURFACE_ID};surface_role={role};"
        "one_command_root=true;duplicate_executor=false"
    )
    return {
        "current_state": "e2e_passed",
        "parser_evidence": (
            f"{COMPONENT_SOURCE};sql=SHOW ACCELERATION;{shared};"
            "exact_ebnf_parent_child_target_binding=true;parser_executes_sql=false"
        ),
        "binder_evidence": (
            f"{COMPONENT_SOURCE};bound_statement=true;right.observe;"
            "authority.engine.observability_api_required;no_parser_provider_authority"
        ),
        "lowering_evidence": (
            f"{COMPONENT_SOURCE};operation_id={OPERATION_ID};opcode={OPCODE};"
            f"opcode_code={OPCODE_CODE};operand_count=0;result={RESULT_DESCRIPTOR};"
            f"{shared};no_source_sql_text"
        ),
        "server_admission_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};authenticated_TLS_listener=true;"
            "SBWP_1_1=true;SBPS=true;canonical_package=true;"
            f"operation_id={OPERATION_ID};opcode={OPCODE};"
            "requires_public_abi_dispatch=true;exact_executor_identity=true"
        ),
        "engine_runtime_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};api=EngineShowAcceleration;"
            "engine_acceleration_registry_snapshot=true;providers="
            "compiler.llvm_execution.interpreter_gpu.cuda_gpu.hip_gpu.opencl;"
            "shared_generation_and_sha256=true;provider_identity_redacted=true;"
            "independent_sessions_equal=true;process_restart_equal=true;"
            "active_MGA_transaction_preserved=true;no_application_mutation=true"
        ),
        "function_or_api_operation_id": (
            f"{OPERATION_ID};opcode={OPCODE};opcode_code={OPCODE_CODE};"
            f"descriptor={OPERAND_DESCRIPTOR};result={RESULT_DESCRIPTOR};{shared}"
        ),
        "diagnostic_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};SECURITY.AUTHENTICATION_FAILED;"
            f"ctest:{COMPONENT_CTEST};SECURITY.ACCESS_DENIED;"
            "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN;PROCESS.CANCELLED;"
            "SBLR.OPERATION.NONCANONICAL;all_refusals_no_application_mutation"
        ),
        "fixture_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};source={FULL_ROUTE_SOURCE};surface_id={surface_id};"
            "public_TLS_execute_independent_session_restart_no_application_mutation=true;"
            "CSC-TEST-002761..002764"
        ),
        "evidence_complete": "yes",
        "notes": (
            "Reviewed SHOW ACCELERATION V1 evidence. acceleration_stmt is syntax dispatch, "
            "show_acceleration is the single canonical opcode-3365 package root, and "
            "accel_show_target is its inherited target spelling; no duplicate executor is "
            "created. The public TLS route publishes one immutable engine-owned capability "
            "snapshot across independent sessions and restart with exact authentication, "
            "security, cluster-fallthrough, cancellation, retired-text, redaction, and "
            "no-application-mutation checks. It does not promote SHOW ACCELERATION EXTENDED, "
            "ALTER ACCELERATION, DML/index behavior, cluster execution, or WAL authority."
        ),
    }


def per_row_manifest_override(
    repo_root: Path,
    surface: dict[str, str],
    ledger_row: dict[str, str] | None,
) -> dict[str, str] | None:
    del repo_root
    if _validate_surface(surface) is None:
        return None
    if ledger_row is None or ledger_row.get("current_state") != "e2e_passed":
        raise ValueError("acceleration row lacks strict E2E evidence")
    surface_id = surface["surface_id"]
    role = _surface_role(surface_id)
    labels = ";".join(
        (
            FULL_ROUTE_CTEST,
            COMPONENT_CTEST,
            "sbsql_parser_worker",
            "sbsql_sblr_alignment",
            "sbsql_surface_to_sblr_full_implementation_closure",
            "sblr_operation_matrix_gate",
            "sbsql_e2e_passed",
            "authenticated_full_route",
            "independent_post_state",
            "restart_recovery",
            "nonmutating_observability",
            OPCODE,
            surface_id,
            *(f"CSC-TEST-{value:06d}" for value in CSC_IDS),
        )
    )
    return {
        "final_state": "e2e_passed",
        "ctest_label": labels,
        "fixture_path": FULL_ROUTE_SOURCE,
        "implementation_refs": (
            f"surface_id={surface_id};surface_role={role};"
            f"parent_surface_id={PARENT_SURFACE_ID};"
            f"command_surface_id={COMMAND_SURFACE_ID};"
            f"target_surface_id={TARGET_SURFACE_ID};operation_id={OPERATION_ID};"
            f"opcode={OPCODE};opcode_code={OPCODE_CODE};operand_count=0;"
            f"descriptor={OPERAND_DESCRIPTOR};result={RESULT_DESCRIPTOR};"
            "one_command_root=true;duplicate_executor=false;"
            "authenticated_statement_receipt=true;server_public_abi_dispatch=true;"
            "engine_acceleration_registry_snapshot=true;SBWP_1_1_TLS_listener_route=true;"
            "independent_sessions=true;process_restart=true;no_application_mutation=true;"
            "no_source_sql_execution_authority"
        ),
        "diagnostic_proof": (
            f"ctest:{FULL_ROUTE_CTEST};canonical_message_vector_set;"
            "SECURITY.AUTHENTICATION_FAILED;"
            f"ctest:{COMPONENT_CTEST};SECURITY.ACCESS_DENIED;"
            "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN;PROCESS.CANCELLED;"
            "SBLR.OPERATION.NONCANONICAL;preexecution_refusals_publish_no_application_mutation"
        ),
        "result_proof": (
            f"ctest:{FULL_ROUTE_CTEST};surface_id={surface_id};"
            f"operation_id={OPERATION_ID};opcode={OPCODE};"
            "provider_ids=compiler.llvm_execution.interpreter_gpu.cuda_gpu.hip_gpu.opencl;"
            "shared_nonzero_generation=true;shared_snapshot_sha256=true;"
            "provider_identity_redacted=true;independent_session_result_equal=true;"
            "restart_result_equal=true;active_MGA_transaction_preserved=true;"
            "catalog_crud_executable_MGA_relation_state_unchanged=true"
        ),
        "evidence_collected_utc": "authenticated_tls_restart_evidence_2026-09-08",
        "promoter_slice": "IA-10-ACCELERATION-SNAPSHOT-V1",
        "notes": (
            "Exact reviewed evidence for one SHOW ACCELERATION command cohort. The parent "
            "syntax row and target child inherit the command root and never become package "
            "roots themselves. The public route proves exact canonical dispatch, immutable "
            "engine snapshot rows, independent sessions, restart, refusal ordering, and no "
            "application mutation; sibling acceleration functionality remains planned or "
            "in progress."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], current: dict[str, str]
) -> dict[str, str]:
    if _validate_surface(surface) is None:
        return current
    role = _surface_role(surface["surface_id"])
    result = dict(current)
    result.update(
        {
            "credential_profile_accepted": (
                "durable_authenticated_principal_with_CONNECT_and_observability_authority"
            ),
            "credential_profile_refused": "wrong_password_or_missing_security_context",
            "auth_policy": (
                "TLS_authentication_then_engine_security_context_and_local_route_authority"
            ),
            "session_profile": (
                "independent_authenticated_sessions_share_one_immutable_engine_snapshot"
            ),
            "transaction_profile": (
                "active_MGA_transaction_identity_preserved;read_only_inspect_snapshot"
            ),
            "transport_route": (
                "sbwp_1.1_over_tls_1.3;sb_listener;pool_allocated_sbp_sbsql_worker;"
                "SBPS;sb_server;engine_public_abi"
            ),
            "tls_profile_ref": "authenticated_local_tls_listener_profile",
            "listener_path": "public_sb_listener_to_sbp_sbsql_to_sbps_to_sb_server",
            "ipc_admission_path": (
                "SBWP_Query_SHOW_ACCELERATION;canonical_zero_operand_opcode_3365_package;"
                "public_abi_dispatch"
            ),
            "engine_admission_authority": (
                "authenticated_statement_receipt;exact_opcode_and_executor_identity;"
                "engine_owned_acceleration_snapshot_generation_and_sha256;"
                f"surface_role={role};one_command_root;no_duplicate_executor"
            ),
            "mga_execution_authority": (
                "read_only_active_transaction_identity_only;no_catalog_row_index_or_DML_mutation;"
                "no_wal_authority"
            ),
            "expected_authorization_accepted_outcome": (
                "five_sorted_redacted_provider_rows_with_shared_generation_and_sha256_equal_"
                "across_independent_sessions_and_process_restart"
            ),
            "expected_authorization_refused_outcome": (
                "wrong_password_missing_security_cluster_fallthrough_cancelled_and_retired_"
                "text_routes_refuse_before_snapshot_publication_or_application_mutation"
            ),
            "expected_diagnostic_codes": (
                "SECURITY.AUTHENTICATION_FAILED;SECURITY.ACCESS_DENIED;"
                "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN;PROCESS.CANCELLED;"
                "SBLR.OPERATION.NONCANONICAL"
            ),
            "notes": (
                "Authenticated public SHOW ACCELERATION route for the reviewed parent, command, "
                "and target rows. Only show_acceleration is an opcode root. The engine snapshot "
                "is immutable, provider identities are redacted, independent sessions and restart "
                "agree, and operational agent bookkeeping is distinguished from forbidden "
                "application mutation."
            ),
        }
    )
    return result


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    surface_id = row.get("surface_id", "")
    if surface_id not in SURFACE_IDS:
        return row
    role = _surface_role(surface_id)
    result = dict(row)
    result.update(
        {
            "oracle_authority_status": "per_row_acceleration_command_cohort_v1",
            "expected_canonical_function_or_api_operation_id": OPERATION_ID,
            "parse_phase_expectation": (
                f"parse_SHOW_ACCELERATION_and_bind_{role}_surface_identity_pass"
            ),
            "bind_phase_expectation": (
                "bind_authenticated_observability_right_and_parent_child_target_identity_pass"
            ),
            "lower_phase_expectation": (
                "lower_exactly_one_observability.show_acceleration_"
                "SBLR_OBSERVABILITY_SHOW_ACCELERATION_3365_zero_operand_root_pass_no_source_text"
            ),
            "binary_serialize_phase_expectation": (
                "serialize_exact_zero_operand_operation_package_to_canonical_container_crc32c_pass"
            ),
            "verify_phase_expectation": (
                "verify_exact_root_executor_result_receipt_security_local_route_and_evidence_pass"
            ),
            "binary_deserialize_phase_expectation": (
                "deserialize_to_byte_identical_opcode_3365_zero_operand_package_pass"
            ),
            "dispatch_phase_expectation": (
                "dispatch_only_by_observability.show_acceleration_to_EngineShowAcceleration_pass"
            ),
            "execute_phase_expectation": (
                "read_immutable_engine_acceleration_registry_snapshot_and_publish_five_"
                "sorted_redacted_provider_rows_pass_without_application_mutation"
            ),
            "render_phase_expectation": (
                "SBWP_row_description_five_data_rows_SELECT_5_and_ready_with_same_transaction_pass"
            ),
            "canonical_container_magic": "0x53424C52",
            "canonical_container_header_size_bytes": "40",
            "byte_identical_round_trip_required": "yes",
            "crc32c_check_required": "yes",
            "engine_anchored_uuids_required": "yes",
            "forbidden_authority_sources": (
                "sql_text;identifier_names;parser_branch_names;ACCELERATION_STMT_invented_root;"
                "parent_or_target_duplicate_executor;operation_family_only_routing;"
                "DML_or_index_route;WAL_authority"
            ),
            "execution_authority_model": (
                "authenticated_statement_receipt;exact_opcode_and_executor_identity;"
                "sblr_envelope_with_uuid_and_descriptor_authority_only;"
                "engine_owned_immutable_acceleration_snapshot;local_read_only_route;"
                "active_MGA_transaction_identity_preserved;no_application_mutation;"
                "no_wal_authority"
            ),
            "notes": (
                f"SHOW ACCELERATION command-cohort round trip for surface_role={role}. "
                "acceleration_stmt and accel_show_target inherit the sole show_acceleration "
                "opcode root. The canonical package, public TLS result, independent-session "
                "snapshot, restart replay, and refusal paths are executable; no DML/index, "
                "cluster execution, parser-owned provider, or WAL authority is claimed."
            ),
        }
    )
    return result
