#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Synchronize dedicated ALTER/DROP TRIGGER rows into public artifacts."""

from __future__ import annotations

import argparse
import csv
import os
import tempfile
from pathlib import Path

from sbsql_trigger_command_surface import (
    CANONICAL_SPEC,
    ENGINE_PACKET,
    NEW_TRIGGER_COMMAND_SURFACES,
    PARSER_PACKET,
    SBLR_OPERATION_FAMILY,
    TRIGGER_COMMAND_BY_SURFACE_ID,
)


ARTIFACT_ROOT = Path(
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
PUBLIC_REGISTRY = Path("public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv")
BACKLOG = ARTIFACT_ROOT / "SURFACE_IMPLEMENTATION_BACKLOG.csv"
BATCH = ARTIFACT_ROOT / "BATCH_ROW_MEMBERSHIP.csv"
ORACLE = ARTIFACT_ROOT / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
BATCH_PLAN = ARTIFACT_ROOT / "REGISTRY_FAMILY_BATCHING_PLAN.csv"
CORE_SURFACE_REGISTRY = Path("registries/sbsql-consolidated-surface-registry.csv")
CORE_SURFACE_TO_SBLR = Path("registries/sbsql-consolidated-surface-to-sblr.csv")
CORE_COMMAND_CLOSURE = Path("registries/sbsql-command-sblr-zero-grey-closure.csv")
CORE_GRAMMAR_BINDING = Path("registries/normalized-grammar-surface-binding-registry.csv")
CORE_SPECIFICATION_GAP = Path("registries/normalized-surface-specification-gap-registry.csv")
CORE_COVERAGE = Path("registries/normalized-surface-coverage-registry.csv")
CORE_IMPLEMENTATION_CONTRACTS = Path(
    "registries/normalized-surface-implementation-contracts-20260822.yaml"
)
TRIGGER_BATCH_ID = "BATCH-0010"
TRIGGER_BATCH_CTEST = "sbsql_ddl_catalog_grammar_production_native_now_02"


def fail(message: str) -> None:
    raise SystemExit(message)


def load(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            fail(f"missing CSV header: {path}")
        rows = [dict(row) for row in reader]
    return list(reader.fieldnames), rows


def index(rows: list[dict[str, str]], key: str, label: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for position, row in enumerate(rows):
        value = row.get(key, "")
        if not value or value in result:
            fail(f"{label}: missing or duplicate {key}={value!r}")
        result[value] = position
    return result


def insert_or_compare(
    rows: list[dict[str, str]],
    key: str,
    expected: dict[str, str],
    anchor_key: str,
    anchor_value: str,
    label: str,
) -> bool:
    lookup = index(rows, key, label)
    identity = expected[key]
    if identity in lookup:
        observed = rows[lookup[identity]]
        drift = [name for name, value in expected.items() if observed.get(name) != value]
        if drift:
            fail(f"{label}: {identity} field drift: {','.join(drift)}")
        return False
    anchors = [
        position
        for position, row in enumerate(rows)
        if row.get(anchor_key) == anchor_value
    ]
    if len(anchors) != 1:
        fail(
            f"{label}: insertion anchor must be unique: "
            f"{anchor_key}={anchor_value} matches={len(anchors)}"
        )
    anchor = anchors[0]
    rows.insert(anchor + 1, expected)
    return True


def insert_sorted_or_compare(
    rows: list[dict[str, str]],
    key: str,
    expected: dict[str, str],
    label: str,
) -> bool:
    lookup = index(rows, key, label)
    identity = expected[key]
    if identity in lookup:
        observed = rows[lookup[identity]]
        drift = [name for name, value in expected.items() if observed.get(name) != value]
        if drift:
            fail(f"{label}: {identity} field drift: {','.join(drift)}")
        return False
    insertion = next(
        (position for position, row in enumerate(rows) if row[key] > identity),
        len(rows),
    )
    rows.insert(insertion, expected)
    return True


def insert_or_compare_relaxed(
    rows: list[dict[str, str]],
    key: str,
    expected: dict[str, str],
    anchor_key: str,
    anchor_value: str,
    label: str,
) -> bool:
    identity = expected[key]
    matches = [row for row in rows if row.get(key) == identity]
    if len(matches) > 1:
        fail(f"{label}: duplicate target {key}={identity}")
    if matches:
        drift = [name for name, value in expected.items() if matches[0].get(name) != value]
        if drift:
            fail(f"{label}: {identity} field drift: {','.join(drift)}")
        return False
    anchors = [
        position for position, row in enumerate(rows)
        if row.get(anchor_key) == anchor_value
    ]
    if len(anchors) != 1:
        fail(
            f"{label}: insertion anchor must be unique: "
            f"{anchor_key}={anchor_value} matches={len(anchors)}"
        )
    rows.insert(anchors[0] + 1, expected)
    return True


def public_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "surface_kind": "grammar_production",
        "family": "ddl_catalog",
        "source_status": "native_now",
        "cluster_scope": "noncluster_or_profile_scoped",
        "canonical_spec": CANONICAL_SPEC,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "parser_packet": PARSER_PACKET,
        "engine_packet": ENGINE_PACKET,
        "owner_lane": "statement parser worker",
        "batch_id": TRIGGER_BATCH_ID,
        "ctest_label": TRIGGER_BATCH_CTEST,
        "parser_handler_key": "parser.statement_family.ddl_catalog",
        "udr_handler_key": "udr.sbsql_parser_support.parse_describe_normalize",
        "lowering_handler_key": "lowering.sblr_family.sblr_catalog_mutation_v3",
        "server_admission_key": "server.admission.sblr_catalog_mutation_v3",
        "engine_rule_key": "engine.rule.sblr_catalog_mutation_v3",
        "diagnostic_key": "diagnostic.canonical_message_vector",
        "oracle_key": "canonical_spec_plus_sblr_matrix",
        "validation_fixture_id": surface.validation_fixture_id,
        "final_acceptance_rule": "parse_bind_lower_server_engine_diagnostic_and_regression_evidence",
        "closure_action": "implement_full_route_or_exact_canonical_refusal",
        "status": "native_now",
    }


def backlog_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "surface_kind": "grammar_production",
        "family": "ddl_catalog",
        "source_status": "native_now",
        "cluster_scope": "noncluster_or_profile_scoped",
        "source_search_key": surface.surface_id,
        "canonical_spec": CANONICAL_SPEC,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "parser_packet": PARSER_PACKET,
        "engine_packet": ENGINE_PACKET,
        "owner_lane": "statement parser worker",
        "target_file_group": "project/src/parsers/sbsql_worker/statements;project/src/server;project/src/engine/internal_api",
        "parser_target_behavior": f"parse_bind_lower_{surface.canonical_name}_through_engine_bound_{surface.bound_magic}_{surface.operand_magic}",
        "udr_target_behavior": "validate_parse_describe_normalize_decompile_under_engine_context_when_applicable",
        "server_target_behavior": "admit_exact_trigger_lifecycle_package_revalidate_receipt_and_dispatch_public_abi",
        "engine_target_behavior": f"execute_{surface.engine_api}_catalog_lifecycle_mutation_under_MGA_transaction_authority",
        "diagnostic_target": "canonical_message_vector_and_parser_rendering",
        "validation_fixture_id": surface.validation_fixture_id,
        "final_acceptance_rule": "parse_bind_lower_server_engine_diagnostic_and_regression_evidence",
        "closure_action": "implement_full_route_or_exact_canonical_refusal",
        "status": "e2e_passed",
    }


def batch_row(surface: object) -> dict[str, str]:
    return {
        "batch_id": TRIGGER_BATCH_ID,
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "family": "ddl_catalog",
        "surface_kind": "grammar_production",
        "source_status": "native_now",
        "cluster_scope": "noncluster_or_profile_scoped",
        "owner_lane": "statement parser worker",
        "validation_fixture_id": surface.validation_fixture_id,
        "ctest_label": TRIGGER_BATCH_CTEST,
        "source_search_key": surface.surface_id,
        "status": "ready_for_fixture_generation",
    }


def oracle_row(surface: object) -> dict[str, str]:
    return {
        "fixture_id": surface.validation_fixture_id,
        "surface_id": surface.surface_id,
        "oracle_type": "canonical_spec_plus_sblr_matrix",
        "oracle_source": CANONICAL_SPEC,
        "source_search_key": surface.surface_id,
        "expected_result_summary": (
            f"expected {surface.verb} TRIGGER parse, authenticated {surface.request_magic}/"
            f"{surface.bound_magic}/{surface.operand_magic} binding, canonical "
            f"{surface.operation_id}/{surface.opcode} admission, {surface.engine_api} "
            f"catalog mutation, exact {surface.result_magic}, refusal, rollback, replay, "
            "and independent-session post-state behavior"
        ),
        "status": "closed_by_semantic_oracle_authority_gate",
    }


def core_surface_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "surface_kind": "grammar_production",
        "family": "ddl_catalog",
        "status": "native_now",
        "cluster_scope": "noncluster_or_profile_scoped",
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "documentation_family": "language_reference_ddl",
        "notes": "Dedicated trigger lifecycle command root with exact engine-bound authority.",
    }


def core_surface_to_sblr_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "ingress_envelope": "SBLRExecutionEnvelope.v3",
        "required_context": (
            "session_uuid; database_uuid; transaction_context; security_context; "
            "language_profile; result_contract"
        ),
        "binding_steps": (
            "parse_to_ast; resolve_names_public_for_parser; bind_descriptors; "
            "assign_catalog_uuids; validate_security; build_sblr_envelope; "
            "verify_sblr; dispatch_engine_api"
        ),
        "result_shape": "rs.sbsql.command_completion.v1",
        "diagnostics": (
            "diag.parser.syntax.v1; diag.binding.failure.v1; "
            "diag.sbsql.catalog_resolution.v1; diag.rights.failure.v1; "
            "diag.sbsql.sblr_envelope.v1; diag.sbsql.opcode_admission.v1"
        ),
        "notes": "Dedicated trigger lifecycle command root; exact tuple is fixed by the command closure registry.",
    }


def core_command_row(surface: object) -> dict[str, str]:
    title = surface.verb.title()
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "surface_kind": "command_subform",
        "family": "ddl_catalog",
        "language_intent_status": "native_now",
        "specification_state": "specified_admitted",
        "implementation_state": "evidence_required",
        "grammar_key": surface.canonical_name,
        "ast_node_kind": f"{title}TriggerStatementAstV1",
        "bound_ast_node_kind": f"Bound{title}TriggerStatementV1",
        "root_route_kind": "sblr_opcode",
        "root_route": surface.opcode,
        "descriptor_contract": f"{surface.operand}.v1",
        "executor_operation_id": surface.operation_id,
        "result_shape": "ddl_result",
        "required_right": "admin_authorized",
        "mga_profile": "catalog_mutation",
        "transaction_effect": "local_or_cluster_write",
        "resource_contract": "sbsql.command.mutation.v1",
        "cluster_contract": "local_or_bound_cluster_route_epoch_and_fence",
        "diagnostic_key": "SBSQL.IMPL.NOT_AVAILABLE",
        "conformance_fixture": f"SBSQL-ZG-{surface.surface_id}",
        "canonical_contract": (
            "chapters/parser-v3/native-sbsql/"
            "appendix-sbsql-and-sblr-command-zero-grey-closure-contract.md"
        ),
        "semantic_dependency": (
            "chapters/parser-v3/native-sbsql/"
            "appendix-sbsql-and-sblr-command-zero-grey-closure-contract.md"
        ),
        "closure_notes": (
            "Target semantics and exact route are specified; runtime admission still "
            "requires accepted implementation evidence."
        ),
    }


def core_grammar_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "family": "ddl_catalog",
        "ebnf_production": surface.canonical_name,
        "binding_status": "exact_ebnf_binding",
        "normalization_decision": "existing_sbsql_lowering",
        "parameter_extension_required": "no",
        "action": (
            "Use the exact EBNF production as a dedicated command root with its own "
            "authenticated AST, BoundAST, descriptor, executor, and result lifecycle."
        ),
    }


def core_gap_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "missing_sections": "none",
        "status": "closure_complete",
        "required_action": (
            "Closure fields, exact EBNF, diagnostic, transport, and registered fixture "
            "are complete; runtime evidence remains tracked separately."
        ),
    }


def core_coverage_row(surface: object) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "surface_kind": "grammar_production",
        "family": "ddl_catalog",
        "source_status": "native_now",
        "canonical_spec": CANONICAL_SPEC,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "parser_packet": PARSER_PACKET,
        "owner_lane": "statement parser worker",
        "normalized_statement_family": "review_required",
        "donor_option_profile": "review_required",
        "sblr_coverage_status": "review_pending",
        "required_action": (
            "Review this dedicated trigger command root against its exact AST, BoundAST, "
            "descriptor, SBLR opcode, public ABI, MGA effect, diagnostics, and E2E evidence."
        ),
    }


def core_implementation_contract(surface: object) -> str:
    title = surface.verb.title()
    if surface.verb == "ALTER":
        ebnf = (
            '  - alter_trigger_statement ::= ALTER TRIGGER qualified_name '
            'alter_trigger_action { alter_trigger_action } ;\n'
            '  - alter_trigger_action ::= ACTIVE | INACTIVE | ENABLE | DISABLE | COMPILE '
            '| VALIDATE | SET ORDER uint_literal | SET SECURITY ( INVOKER | DEFINER ) '
            '| SET FAILURE POLICY MANDATORY\n'
        )
    else:
        ebnf = (
            '  - drop_trigger_statement ::= DROP TRIGGER qualified_name '
            '[ RESTRICT | CASCADE ] ;\n'
        )
    return (
        f"- surface_id: {surface.surface_id}\n"
        f"  canonical_name: {surface.canonical_name}\n"
        "  family: ddl_catalog\n"
        "  implementation_mode: explicit_core_closure\n"
        "  implementation_ready_for_low_capability_agent: true\n"
        "  cluster_scope: noncluster_or_profile_scoped\n"
        "  exact_ebnf:\n"
        f"{ebnf}"
        f"  parent_identity: {surface.canonical_name}\n"
        "  ast_bound_ast:\n"
        f"    ast_type: {title}TriggerStatementAstV1\n"
        f"    bound_ast_type: Bound{title}TriggerStatementV1\n"
        "    fields: all fields enumerated by the closure contract\n"
        "    span: source span retained\n"
        "  options_defaults_conflicts_validation:\n"
        "    required_right: admin_authorized\n"
        "    resource_contract: sbsql.command.mutation.v1\n"
        "    cluster_contract: local_or_bound_cluster_route_epoch_and_fence\n"
        "    unknown_option: refuse\n"
        "  datatype_result_descriptors:\n"
        f"    descriptor: {surface.operand}.v1\n"
        f"    executor: {surface.operation_id}\n"
        "    result_shape: ddl_result\n"
        "    mga: catalog_mutation\n"
        "    transaction: local_or_cluster_write\n"
        "  sblr_opcode_byte_layout:\n"
        f"    operation_family: {SBLR_OPERATION_FAMILY}\n"
        "    ingress: SBLRExecutionEnvelope.v3\n"
        "    result: rs.sbsql.command_completion.v1\n"
        f"    descriptor: {surface.operand}.v1\n"
        "  authority_security_mga_transaction_recovery:\n"
        "    required_context:\n"
        "    - session_uuid\n"
        "    - database_uuid\n"
        "    - transaction_context\n"
        "    - security_context\n"
        "    - language_profile\n"
        "    - result_contract\n"
        "    right: admin_authorized\n"
        "    cluster: local_or_bound_cluster_route_epoch_and_fence\n"
        "    closure: Target semantics and exact route are specified; runtime admission still\n"
        "      requires accepted implementation evidence.\n"
        "  diagnostics_refusals:\n"
        "  - SBSQL.IMPL.NOT_AVAILABLE\n"
        "  - diag.binding.failure.v1\n"
        "  - diag.parser.syntax.v1\n"
        "  - diag.rights.failure.v1\n"
        "  - diag.sbsql.catalog_resolution.v1\n"
        "  - diag.sbsql.opcode_admission.v1\n"
        "  - diag.sbsql.sblr_envelope.v1\n"
        "  parser_registration_lowering:\n"
        "    parser_packet: public_input_snapshot\n"
        "    lowering_handler: lowering.sblr_family.sblr_catalog_mutation_v3\n"
        "    binding_steps:\n"
        "    - parse_to_ast\n"
        "    - resolve_names_public_for_parser\n"
        "    - bind_descriptors\n"
        "    - assign_catalog_uuids\n"
        "    - validate_security\n"
        "    - build_sblr_envelope\n"
        "    - verify_sblr\n"
        "    - dispatch_engine_api\n"
        "    refusal: false\n"
        "  transport_public_abi:\n"
        "    ingress: SBLRExecutionEnvelope.v3\n"
        "    result: rs.sbsql.command_completion.v1\n"
        "    required_context:\n"
        "    - session_uuid\n"
        "    - database_uuid\n"
        "    - transaction_context\n"
        "    - security_context\n"
        "    - language_profile\n"
        "    - result_contract\n"
        "  tests:\n"
        f"  - {surface.validation_fixture_id}\n"
        f"  - SBSQL-ZG-{surface.surface_id}\n"
        "  refusal_only: false\n"
    )


def insert_or_compare_yaml_contract(
    text: str, surface: object, anchor_surface_id: str, label: str
) -> tuple[str, bool]:
    expected = core_implementation_contract(surface)
    marker = f"- surface_id: {surface.surface_id}\n"
    if marker in text:
        start = text.index(marker)
        end = text.find("- surface_id: ", start + len(marker))
        if end < 0:
            end = len(text)
        if text[start:end] != expected:
            fail(f"{label}: {surface.surface_id} implementation contract drift")
        return text, False
    anchor = f"- surface_id: {anchor_surface_id}\n"
    anchor_start = text.find(anchor)
    if anchor_start < 0:
        fail(f"{label}: missing implementation contract anchor {anchor_surface_id}")
    insertion = text.find("- surface_id: ", anchor_start + len(anchor))
    if insertion < 0:
        insertion = len(text)
    return text[:insertion] + expected + text[insertion:], True


def write(path: Path, header: list[str], rows: list[dict[str, str]], quote_all: bool) -> None:
    mode = path.stat().st_mode & 0o777
    with tempfile.NamedTemporaryFile(
        mode="w", newline="", encoding="utf-8", dir=path.parent,
        prefix=f".{path.name}.", suffix=".tmp", delete=False,
    ) as handle:
        temporary = Path(handle.name)
        writer = csv.DictWriter(
            handle,
            fieldnames=header,
            lineterminator="\n",
            quoting=csv.QUOTE_ALL if quote_all else csv.QUOTE_MINIMAL,
        )
        if quote_all:
            handle.write(",".join(header) + "\n")
        else:
            writer.writeheader()
        writer.writerows({field: row.get(field, "") for field in header} for row in rows)
    os.chmod(temporary, mode)
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--core-root")
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    paths = [root / item for item in (PUBLIC_REGISTRY, BACKLOG, BATCH, ORACLE, BATCH_PLAN)]
    if any(not path.is_file() or path.is_symlink() for path in paths):
        fail("trigger sync requires regular, non-symlink public artifacts")

    public_header, public_rows = load(paths[0])
    backlog_header, backlog_rows = load(paths[1])
    batch_header, batch_rows = load(paths[2])
    oracle_header, oracle_rows = load(paths[3])
    plan_header, plan_rows = load(paths[4])
    changed = [False, False, False, False, False]

    for surface in NEW_TRIGGER_COMMAND_SURFACES:
        changed[0] |= insert_or_compare(
            public_rows, "surface_id", public_row(surface), "canonical_name",
            surface.insertion_anchor, "public registry",
        )
        changed[1] |= insert_or_compare(
            backlog_rows, "surface_id", backlog_row(surface), "canonical_name",
            surface.insertion_anchor, "implementation backlog",
        )
        changed[2] |= insert_or_compare(
            batch_rows, "surface_id", batch_row(surface), "surface_id",
            surface.batch_insertion_anchor, "batch membership",
        )
        public_anchor_id = next(
            row["surface_id"]
            for row in public_rows
            if row.get("canonical_name") == surface.insertion_anchor
        )
        changed[3] |= insert_or_compare(
            oracle_rows, "surface_id", oracle_row(surface), "surface_id",
            public_anchor_id, "semantic oracle",
        )

    plan_lookup = index(plan_rows, "batch_id", "batch plan")
    plan_row = plan_rows[plan_lookup[TRIGGER_BATCH_ID]]
    expected_count = sum(row.get("batch_id") == TRIGGER_BATCH_ID for row in batch_rows)
    if plan_row.get("row_count") != str(expected_count):
        plan_row["row_count"] = str(expected_count)
        changed[4] = True

    if any(changed) and not args.update:
        print(
            "sbsql_trigger_command_surface_rows=stale "
            f"changed_files={sum(changed)} rows={len(NEW_TRIGGER_COMMAND_SURFACES)}"
        )
        return 1
    if args.update:
        outputs = (
            (paths[0], public_header, public_rows, True),
            (paths[1], backlog_header, backlog_rows, False),
            (paths[2], batch_header, batch_rows, False),
            (paths[3], oracle_header, oracle_rows, False),
            (paths[4], plan_header, plan_rows, False),
        )
        for was_changed, output in zip(changed, outputs):
            if was_changed:
                write(*output)

    core_changed: list[bool] = []
    if args.core_root:
        core_root = Path(args.core_root).resolve()
        core_paths = [
            core_root / item
            for item in (
                CORE_SURFACE_REGISTRY,
                CORE_SURFACE_TO_SBLR,
                CORE_COMMAND_CLOSURE,
                CORE_GRAMMAR_BINDING,
                CORE_SPECIFICATION_GAP,
                CORE_COVERAGE,
            )
        ]
        implementation_path = core_root / CORE_IMPLEMENTATION_CONTRACTS
        if any(not path.is_file() or path.is_symlink() for path in (*core_paths, implementation_path)):
            fail("trigger sync requires regular, non-symlink Core artifacts")
        core_tables = [load(path) for path in core_paths]
        core_changed = [False] * (len(core_tables) + 1)
        for surface in NEW_TRIGGER_COMMAND_SURFACES:
            core_changed[0] |= insert_sorted_or_compare(
                core_tables[0][1], "surface_id", core_surface_row(surface),
                "Core consolidated surface registry",
            )
            core_changed[1] |= insert_sorted_or_compare(
                core_tables[1][1], "surface_id", core_surface_to_sblr_row(surface),
                "Core surface-to-SBLR registry",
            )
            for table_index, expected, label in (
                (2, core_command_row(surface), "Core command closure"),
                (3, core_grammar_row(surface), "Core grammar binding"),
                (4, core_gap_row(surface), "Core specification gap"),
                (5, core_coverage_row(surface), "Core coverage"),
            ):
                core_changed[table_index] |= insert_or_compare_relaxed(
                    core_tables[table_index][1], "surface_id", expected,
                    "canonical_name", surface.insertion_anchor, label,
                )

        create_id = "SBSQL-5127560F8031"
        create_grammar_rows = [
            row for row in core_tables[3][1] if row.get("surface_id") == create_id
        ]
        if len(create_grammar_rows) != 1:
            fail("Core grammar binding: CREATE TRIGGER row is missing or duplicated")
        create_grammar = create_grammar_rows[0]
        create_grammar_action = (
            "Use the exact EBNF production as a dedicated command root with its own "
            "authenticated AST, BoundAST, descriptor, executor, and result lifecycle."
        )
        if create_grammar.get("action") != create_grammar_action:
            create_grammar["action"] = create_grammar_action
            core_changed[3] = True
        create_gap_rows = [
            row for row in core_tables[4][1] if row.get("surface_id") == create_id
        ]
        if len(create_gap_rows) != 1:
            fail("Core specification gap: CREATE TRIGGER row is missing or duplicated")
        create_gap = create_gap_rows[0]
        create_gap_expected = core_gap_row(TRIGGER_COMMAND_BY_SURFACE_ID[create_id])
        for name, value in create_gap_expected.items():
            if create_gap.get(name) != value:
                create_gap[name] = value
                core_changed[4] = True

        implementation_text = implementation_path.read_text(encoding="utf-8")
        for surface in NEW_TRIGGER_COMMAND_SURFACES:
            anchor_id = next(
                row["surface_id"]
                for row in core_tables[3][1]
                if row.get("canonical_name") == surface.insertion_anchor
            )
            implementation_text, inserted = insert_or_compare_yaml_contract(
                implementation_text, surface, anchor_id,
                "Core implementation contracts",
            )
            core_changed[6] |= inserted
        expected_surface_count = len(core_tables[4][1])
        old_count = "surface_count: 2617\n"
        new_count = f"surface_count: {expected_surface_count}\n"
        if old_count in implementation_text:
            implementation_text = implementation_text.replace(old_count, new_count, 1)
            core_changed[6] = True
        elif new_count not in implementation_text:
            fail("Core implementation contracts surface_count drift")

        if any(core_changed) and not args.update:
            print(
                "sbsql_trigger_core_rows=stale "
                f"changed_files={sum(core_changed)} rows={len(NEW_TRIGGER_COMMAND_SURFACES)}"
            )
            return 1
        if args.update:
            for was_changed, path, table in zip(core_changed[:6], core_paths, core_tables):
                if was_changed:
                    write(path, table[0], table[1], False)
            if core_changed[6]:
                implementation_path.write_text(implementation_text, encoding="utf-8")
        print(
            "sbsql_trigger_core_rows="
            f"{'synchronized' if args.update else 'verified'} "
            f"changed_files={sum(core_changed)} rows={len(NEW_TRIGGER_COMMAND_SURFACES)}"
        )
    print(
        "sbsql_trigger_command_surface_rows="
        f"{'synchronized' if args.update else 'verified'} "
        f"changed_files={sum(changed)} rows={len(NEW_TRIGGER_COMMAND_SURFACES)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
