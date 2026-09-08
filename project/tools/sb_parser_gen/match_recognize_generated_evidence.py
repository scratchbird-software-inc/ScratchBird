#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reviewed evidence for the bounded MATCH_RECOGNIZE query-child profile.

The public surface is not a scalar function and is never a package root.  The
only admitted V1 shape is a row-pattern node beneath ``query.execute`` over a
source-free ``generate_series`` input.  This module keeps generated evidence
from falling back to the historical ``sb.scalar.match_recognize`` marker.
"""

from __future__ import annotations

import csv
from pathlib import Path


SURFACE_ID = "SBSQL-14EDC2636B45"
CANONICAL_NAME = "MATCH_RECOGNIZE"
FULL_ROUTE_CTEST = "sbsql_match_recognize_full_route_gate"
FULL_ROUTE_SOURCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_match_recognize_full_route_gate.py"
)
ROOT_OPERATION_ID = "query.execute"
ROOT_OPCODE = "SBLR_QUERY_EXECUTE"
ROOT_OPCODE_CODE = 4615
PLAN_NODE_OPERATION_ID = "engine.op.match_recognize"
PLAN_NODE_OPCODE = "SBLR_MATCH_RECOGNIZE"
PLAN_NODE_OPCODE_CODE = 1294
PLAN_NODE_DESCRIPTOR = "row_pattern_descriptor"

CSC_IDS = tuple(range(5814, 5821))

_COMMAND_REGISTRY = "registries/sbsql-command-sblr-zero-grey-closure.csv"
_GAP_REGISTRY = "registries/normalized-surface-specification-gap-registry.csv"
_OPCODE_CLOSURE = "registries/sblr-opcode-executor-zero-grey-closure.csv"
_PLAN_TREE_CONTRACT = (
    "chapters/parser-v3/sblr-lowering/"
    "appendix-sblr-relational-plan-tree-descriptors.md"
)
_STRICT_PROFILE_CONTRACT = (
    "chapters/parser-v3/native-sbsql/"
    "appendix-sql2023-strict-profile-and-language-gap-closure.md"
)


def is_match_recognize_surface(surface_id: str) -> bool:
    return surface_id == SURFACE_ID


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
        raise ValueError(f"MATCH_RECOGNIZE evidence source missing: {path}")
    text = path.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise ValueError(f"MATCH_RECOGNIZE evidence source drift: {path}: {missing}")


def validate_authoritative_runtime_inputs(repo_root: Path) -> None:
    core = repo_root.resolve().parent / "Specifications" / "Core"
    command = _read_exact_row(core / _COMMAND_REGISTRY, "surface_id", SURFACE_ID)
    if (
        command.get("canonical_name") != CANONICAL_NAME
        or command.get("root_route_kind") != "diagnostic_refusal"
        or command.get("root_route") != "SBLR_DIAGNOSTIC_REFUSAL"
        or command.get("diagnostic_key") != "SBSQL.IMPL.NOT_AVAILABLE"
    ):
        raise ValueError("MATCH_RECOGNIZE standalone command disposition drift")

    gap = _read_exact_row(core / _GAP_REGISTRY, "surface_id", SURFACE_ID)
    if gap.get("status") != "inherited_parent" or "no duplicate executor" not in gap.get(
        "required_action", ""
    ).lower():
        raise ValueError("MATCH_RECOGNIZE parent-inheritance authority drift")

    opcode = _read_exact_row(core / _OPCODE_CLOSURE, "opcode_name", PLAN_NODE_OPCODE)
    if (
        opcode.get("opcode_code") != str(PLAN_NODE_OPCODE_CODE)
        or opcode.get("operand_contract") != PLAN_NODE_DESCRIPTOR
        or opcode.get("result_contract") != "plan_node_descriptor"
        or opcode.get("executor_binding_requirement") != PLAN_NODE_OPERATION_ID
        or opcode.get("specification_state") != "specified_admitted"
        or opcode.get("implementation_state") != "evidence_required"
    ):
        raise ValueError("MATCH_RECOGNIZE SBLR plan-node authority drift")

    _require_tokens(
        core / _PLAN_TREE_CONTRACT,
        (
            "`SBLR_QUERY_EXECUTE` is the only engine-admissible relational read root",
            "`SBLR_MATCH_RECOGNIZE`",
            "`row_pattern_descriptor`",
        ),
    )
    _require_tokens(
        core / _STRICT_PROFILE_CONTRACT,
        (
            "`MATCH_RECOGNIZE` lowers to `sblr.query.row_pattern.v1`",
            "`ALL ROWS PER MATCH`",
            "`AFTER MATCH SKIP`",
        ),
    )
    _require_tokens(
        repo_root / FULL_ROUTE_SOURCE,
        (
            "SELECT * FROM generate_series",
            "PATTERN (A+)",
            "DEFINE A AS TRUE",
            "invalid BIND payload",
            "QOW-DIAG-QRY-001-AST-MALFORMED",
            "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN",
            "restart",
            "wrong-password",
        ),
    )
    _require_tokens(
        repo_root / "project/src/engine/public_abi.cpp",
        (
            "statement_management_source_free_match_recognize_parameterized_query_template",
            "source_free_parameterized_query_template",
        ),
    )
    _require_tokens(
        repo_root / "project/src/server/engine_host.cpp",
        ("RecoverSblrPreparedCoordinationRegistry",),
    )
    _require_tokens(
        repo_root / "project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp",
        ("invalid BIND payload",),
    )


def _validate_surface(surface: dict[str, str]) -> bool:
    if surface.get("surface_id") != SURFACE_ID:
        return False
    status = surface.get("source_status") or surface.get("status")
    if (
        surface.get("canonical_name") != CANONICAL_NAME
        or surface.get("surface_kind") != "function"
        or surface.get("sblr_operation_family") != "sblr.expression.runtime.v3"
        or surface.get("cluster_scope") != "noncluster_or_profile_scoped"
        or status != "native_now"
    ):
        raise ValueError("MATCH_RECOGNIZE public surface authority drift")
    return True


def oracle_record() -> dict[str, object]:
    return {
        "builtin_id": "sblr.query.row_pattern.v1",
        "canonical_name": CANONICAL_NAME,
        "overloads": [
            {
                "signature": "query MATCH_RECOGNIZE(row_pattern_descriptor)",
                "argument_rule": (
                    "query child only; bounded V1 accepts a source-free generate_series "
                    "input with two or three bound BIGINT parameters, PARTITION/ORDER BY "
                    "the generated value, ALL ROWS PER MATCH, SKIP PAST LAST ROW, "
                    "PATTERN(A+), and DEFINE A AS TRUE"
                ),
            }
        ],
        "return_type_rule": (
            "plan_node_descriptor beneath query.execute; the parent query returns one "
            "BIGINT column for every accepted row in deterministic ascending order"
        ),
        "coercion_rule": (
            "SBWP parameter OIDs and engine-negotiated descriptors must be exact BIGINT; "
            "no parser-authored type or scalar descriptor is authority"
        ),
        "null_behavior": "NULL start, stop, or step is outside the bounded admitted profile",
        "collation_charset_rule": "not applicable",
        "timezone_rule": "not applicable",
        "volatility": "stable_statement",
        "determinism": "deterministic for the exact bound series and row-pattern descriptor",
        "side_effects": "none; read-only query plan node",
        "sblr_binding": "sblr.query.row_pattern.v1",
        "ast_binding": "ast.query.match_recognize",
        "engine_entrypoint": PLAN_NODE_OPERATION_ID,
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "bounded_relational_row_pattern",
        },
        "security_policy": (
            "authenticated query.execute statement receipt, exact parameter negotiation, "
            "resource epochs, and engine-owned statement-management authority are required; "
            "the plan node is never a standalone package root"
        ),
        "reference_rendering": (
            "parser renders MATCH_RECOGNIZE only as a query clause; no scalar function or "
            "standalone command spelling is execution authority"
        ),
        "error_semantics": (
            "malformed Bind arity refuses 08P01; unsupported row modes refuse with "
            "QOW-DIAG-QRY-001-AST-MALFORMED; standalone use refuses before SBLR dispatch; "
            "authentication failure refuses before statement admission"
        ),
        "syntax_forms": ["query_match_recognize_clause"],
        "conformance_cases": [f"CSC-TEST-{value:06d}" for value in CSC_IDS],
    }


def strict_ledger_override(
    repo_root: Path, surface: dict[str, str]
) -> dict[str, str] | None:
    del repo_root
    if not _validate_surface(surface):
        return None
    return {
        "current_state": "e2e_passed",
        "parser_evidence": (
            f"{FULL_ROUTE_SOURCE};surface_id={SURFACE_ID};SBWP_1_1_over_TLS=true;"
            "accepted_sql=SELECT_generate_series_MATCH_RECOGNIZE;"
            "standalone_MATCH_RECOGNIZE_child_refused=true;parser_executes_sql=false"
        ),
        "binder_evidence": (
            f"{FULL_ROUTE_SOURCE};authenticated_statement_receipt=true;"
            "parameter_count=2_or_3;parameter_oid=BIGINT;"
            "engine_parameter_prebind_and_finalization=true;"
            "catalog_security_resource_transaction_epochs_bound=true"
        ),
        "lowering_evidence": (
            f"{FULL_ROUTE_SOURCE};operation_id={ROOT_OPERATION_ID};"
            f"sblr_operation={ROOT_OPCODE};opcode_code={ROOT_OPCODE_CODE};"
            f"nested_plan_node={PLAN_NODE_OPCODE};nested_opcode_code={PLAN_NODE_OPCODE_CODE};"
            f"descriptor={PLAN_NODE_DESCRIPTOR};no_source_sql_text;"
            "no_scalar_match_recognize_route"
        ),
        "server_admission_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};authenticated_TLS_listener=true;"
            f"operation_id={ROOT_OPERATION_ID};opcode={ROOT_OPCODE};"
            "requires_public_abi_dispatch=true;exact_executor_evidence=true"
        ),
        "engine_runtime_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};engine_plan_node={PLAN_NODE_OPERATION_ID};"
            "descending_step_rows=1_3_5;default_step_rows=1_2_3;"
            "restart_rows=2_3_4;active_MGA_transaction_preserved=true;"
            "no_mutation_route=true;no_wal_authority"
        ),
        "function_or_api_operation_id": (
            f"{ROOT_OPERATION_ID};opcode={ROOT_OPCODE};"
            f"nested_plan_node={PLAN_NODE_OPERATION_ID};"
            f"nested_opcode={PLAN_NODE_OPCODE};descriptor={PLAN_NODE_DESCRIPTOR}"
        ),
        "diagnostic_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};08P01_invalid_BIND_payload;"
            "QOW-DIAG-QRY-001-AST-MALFORMED;"
            "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN;"
            "SECURITY.AUTHENTICATION_FAILED;all_refusals_no_mutation"
        ),
        "fixture_evidence": (
            f"ctest:{FULL_ROUTE_CTEST};source={FULL_ROUTE_SOURCE};"
            f"surface_id={SURFACE_ID};CSC-TEST-005814..005820;"
            "public_TLS_parse_bind_execute_restart=true"
        ),
        "evidence_complete": "yes",
        "notes": (
            "Bounded MATCH_RECOGNIZE query-child V1 is implemented end to end. The exact "
            "public route is SELECT over generate_series with two or three negotiated "
            "BIGINT parameters, ALL ROWS PER MATCH, SKIP PAST LAST ROW, PATTERN(A+), and "
            "DEFINE A AS TRUE. It lowers only as SBLR_MATCH_RECOGNIZE beneath the sole "
            "query.execute root, survives process restart, and has authenticated, malformed-"
            "Bind, unsupported-profile, standalone-child, and no-mutation proof. It does not "
            "promote ONE ROW, MEASURES, other patterns/DEFINE predicates, relation-backed "
            "inputs, the separate expr.match_recognize.v1 row, a scalar function route, a "
            "standalone package root, DML/index behavior, cluster execution, or WAL authority."
        ),
    }


def per_row_manifest_override(
    repo_root: Path,
    surface: dict[str, str],
    ledger_row: dict[str, str] | None,
) -> dict[str, str] | None:
    del repo_root
    if not _validate_surface(surface):
        return None
    if ledger_row is None or ledger_row.get("current_state") != "e2e_passed":
        raise ValueError("MATCH_RECOGNIZE row lacks strict E2E evidence")
    labels = ";".join(
        (
            FULL_ROUTE_CTEST,
            "sbsql_parser_worker",
            "sbsql_sblr_alignment",
            "sbsql_surface_to_sblr_full_implementation_closure",
            "sblr_operation_matrix_gate",
            "sbsql_e2e_passed",
            "authenticated_full_route",
            "independent_post_state",
            "restart_recovery",
            ROOT_OPCODE,
            PLAN_NODE_OPCODE,
            SURFACE_ID,
            *(f"CSC-TEST-{value:06d}" for value in CSC_IDS),
        )
    )
    return {
        "final_state": "e2e_passed",
        "ctest_label": labels,
        "fixture_path": FULL_ROUTE_SOURCE,
        "implementation_refs": (
            f"surface_id={SURFACE_ID};operation_id={ROOT_OPERATION_ID};"
            f"opcode={ROOT_OPCODE};opcode_code={ROOT_OPCODE_CODE};"
            f"nested_plan_node={PLAN_NODE_OPERATION_ID};"
            f"nested_opcode={PLAN_NODE_OPCODE};nested_opcode_code={PLAN_NODE_OPCODE_CODE};"
            f"descriptor={PLAN_NODE_DESCRIPTOR};authenticated_statement_receipt=true;"
            "engine_parameter_negotiation=true;canonical_source_free_query_template=true;"
            "server_public_abi_dispatch=true;SBWP_1_1_TLS_listener_route=true;"
            "process_restart_recovery=true;standalone_child_refusal=true;"
            "no_scalar_match_recognize_route=true;no_source_sql_execution_authority"
        ),
        "diagnostic_proof": (
            f"ctest:{FULL_ROUTE_CTEST};canonical_message_vector_set;"
            "08P01_invalid_BIND_payload;"
            "QOW-DIAG-QRY-001-AST-MALFORMED;"
            "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN;"
            "SECURITY.AUTHENTICATION_FAILED;preexecution_refusals_publish_no_mutation"
        ),
        "result_proof": (
            f"ctest:{FULL_ROUTE_CTEST};surface_id={SURFACE_ID};"
            f"operation_id={ROOT_OPERATION_ID};opcode={ROOT_OPCODE};"
            f"nested_plan_node={PLAN_NODE_OPCODE};authenticated_full_route=true;"
            "descending_step_values=5_1_minus2;rows=1_3_5;"
            "default_step_values=1_3;rows=1_2_3;"
            "restart_values=2_4;rows=2_3_4;"
            "active_MGA_transaction_preserved=true;no_mutation_route=true"
        ),
        "evidence_collected_utc": "authenticated_tls_and_restart_process_evidence_2026-09-07",
        "promoter_slice": "IA-EXT-OPT-ABSORBED-MATCH-RECOGNIZE-BOUNDED-V1",
        "notes": (
            "Exact reviewed evidence for the bounded MATCH_RECOGNIZE query child. The "
            "public parent SELECT route, nested row-pattern opcode, negotiated parameters, "
            "result rows, process restart, standalone-child refusal, malformed Bind, "
            "unsupported profile, and authentication refusal are all executable. Broader "
            "MATCH_RECOGNIZE semantics and the separate expr.match_recognize.v1 surface "
            "remain planned or in progress."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], current: dict[str, str]
) -> dict[str, str]:
    if not _validate_surface(surface):
        return current
    result = dict(current)
    result.update(
        {
            "credential_profile_accepted": (
                "durable_authenticated_principal_with_CONNECT_and_query_read_authority"
            ),
            "credential_profile_refused": (
                "wrong_password_or_unbound_malformed_or_unsupported_query_profile"
            ),
            "transaction_profile": (
                "mga_read_authority_required;active_transaction_identity_preserved;"
                "restart_reconstructs_prepared_coordination_registry"
            ),
            "transport_route": (
                "sbwp_1.1_over_tls_1.3;sb_listener;pool_allocated_sbp_sbsql_worker;"
                "SBPS;sb_server;engine_public_abi"
            ),
            "ipc_admission_path": (
                "SBWP_Parse_parameter_OIDs;engine_parameter_prebind;SBWP_Bind_exact_arity;"
                "canonical_query_execute_package;public_abi_dispatch"
            ),
            "engine_admission_authority": (
                "engine_owned_statement_receipt_transaction_security_resource_and_executor_epochs;"
                "exact_source_free_parameterized_template;parser_copies_only_bound_descriptors"
            ),
            "mga_execution_authority": (
                "mga_read_only_active_transaction;no_catalog_or_row_mutation;no_wal_authority"
            ),
            "expected_authorization_accepted_outcome": (
                "query_execute_with_nested_SBLR_MATCH_RECOGNIZE_returns_exact_ordered_BIGINT_rows_"
                "before_and_after_process_restart"
            ),
            "expected_authorization_refused_outcome": (
                "wrong_password_malformed_Bind_unsupported_row_mode_and_standalone_child_all_"
                "refuse_before_mutation"
            ),
            "expected_diagnostic_codes": (
                "SECURITY.AUTHENTICATION_FAILED;08P01_invalid_BIND_payload;"
                "QOW-DIAG-QRY-001-AST-MALFORMED;"
                "SBSQL.PARSER.STATEMENT_FAMILY_UNKNOWN"
            ),
            "notes": (
                "Public authenticated route for the bounded MATCH_RECOGNIZE query child. "
                "Only query.execute is a package root; SBLR_MATCH_RECOGNIZE is nested, "
                "parameter authority is engine-negotiated, exact rows are observed over TLS, "
                "and restart recovers orphaned prepared coordination without minting parser "
                "authority. No scalar function or standalone executor is admitted."
            ),
        }
    )
    return result


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    if row.get("surface_id") != SURFACE_ID:
        return row
    result = dict(row)
    result.update(
        {
            "oracle_authority_status": "per_row_match_recognize_query_child_v1",
            "expected_canonical_function_or_api_operation_id": ROOT_OPERATION_ID,
            "parse_phase_expectation": (
                "parse_SELECT_generate_series_MATCH_RECOGNIZE_with_2_or_3_parameters_pass"
            ),
            "bind_phase_expectation": (
                "bind_engine_negotiated_BIGINT_parameters_and_statement_epochs_pass"
            ),
            "lower_phase_expectation": (
                "lower_to_query.execute_SBLR_QUERY_EXECUTE_with_nested_"
                "SBLR_MATCH_RECOGNIZE_row_pattern_descriptor_pass_no_source_text_authority"
            ),
            "binary_serialize_phase_expectation": (
                "serialize_exact_query_package_and_nested_plan_node_to_canonical_container_crc32c_pass"
            ),
            "verify_phase_expectation": (
                "verify_exact_parameter_bijection_plan_shape_epochs_and_executor_evidence_pass"
            ),
            "binary_deserialize_phase_expectation": (
                "deserialize_to_byte_identical_query_execute_and_row_pattern_descriptors_pass"
            ),
            "dispatch_phase_expectation": (
                "dispatch_root_only_by_query.execute_and_nested_node_only_by_"
                "engine.op.match_recognize_pass"
            ),
            "execute_phase_expectation": (
                "execute_bounded_A_plus_true_ALL_ROWS_pattern_over_generate_series_under_"
                "active_MGA_read_authority_pass"
            ),
            "render_phase_expectation": (
                "SBWP_row_description_data_rows_command_complete_and_ready_pass"
            ),
            "canonical_container_magic": "0x53424C52",
            "canonical_container_header_size_bytes": "40",
            "byte_identical_round_trip_required": "yes",
            "crc32c_check_required": "yes",
            "engine_anchored_uuids_required": "yes",
            "forbidden_authority_sources": (
                "sql_text;identifier_names;parser_branch_names;scalar_match_recognize_marker;"
                "standalone_SBLR_MATCH_RECOGNIZE_root;operation_family_only_routing"
            ),
            "execution_authority_model": (
                "authenticated_statement_receipt;engine_parameter_descriptors;"
                "sblr_envelope_with_uuid_and_descriptor_authority_only;"
                "mga_read_only_active_transaction;restart_recovery;no_wal_authority"
            ),
            "notes": (
                "MATCH_RECOGNIZE bounded query-child V1 round trip. The canonical container "
                "has query.execute as its only root and carries SBLR_MATCH_RECOGNIZE solely "
                "inside the exact row-pattern plan. The public TLS gate proves exact rows, "
                "restart, malformed Bind, unsupported-profile, standalone-child, and "
                "authentication refusals without DML/index mutation."
            ),
        }
    )
    return result
