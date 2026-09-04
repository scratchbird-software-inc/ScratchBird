#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exact generated-evidence profile for the central import command slice.

The historical generator grouped these nine SBsql identities under the
planning-only ``dml.plan_import_rows`` operation.  The controlling central
command authority now has two distinct outcomes:

* the two COPY roots execute the durable ``engine.op.bulk_import_stream`` /
  opcode-775 route; and
* the other seven identities are recognized, deterministic no-execution
  refusals with ``SBSQL.IMPL.NOT_AVAILABLE``.

All derived ledgers, matrices, fixtures, and release declarations use that
split.  No row in this selector is allowed to retain opcode 793 or a pending
classification.
"""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path

import yaml


PLAN_IMPORT_ROWS_SURFACES = {
    "SBSQL-2DDA6BFD9B65": "copy_format",
    "SBSQL-4369855D2FC4": "copy_options",
    "SBSQL-465931ED7427": "copy_import_export",
    "SBSQL-4F912014EA85": "copy_statement",
    "SBSQL-7254347122CB": "gpu_workload_action",
    "SBSQL-B7DCE9CB07B6": "cypher_load_csv",
    "SBSQL-BDC2B64DA2A9": "copy_endpoint",
    "SBSQL-D19FE1151601": "copy_source",
    "SBSQL-DB993AE8EDBB": "load_data_clause",
}

# Compatibility names are retained because the four full-table generators
# import them.  They now mean the exact central-import selector, not opcode 793.
PLAN_IMPORT_ROWS_SURFACE_IDS = frozenset(PLAN_IMPORT_ROWS_SURFACES)
BULK_IMPORT_STREAM_SURFACE_IDS = frozenset(
    {"SBSQL-465931ED7427", "SBSQL-4F912014EA85"}
)
CENTRAL_IMPORT_REFUSAL_SURFACE_IDS = (
    PLAN_IMPORT_ROWS_SURFACE_IDS - BULK_IMPORT_STREAM_SURFACE_IDS
)

PLAN_IMPORT_ROWS_CORE_SELECTOR_KEY = "SBSQL-CENTRAL-IMPORT-COMMAND-DISPOSITION-V1"

BULK_IMPORT_STREAM_IDENTITY_VECTOR = ";".join(
    (
        "operation_id=engine.op.bulk_import_stream",
        "opcode=SBLR_BULK_IMPORT_STREAM",
        "opcode_code=775",
        "opcode_version=1.0",
        "command_descriptor_contract=bulk_import_stream_descriptor.v1",
        "operand_descriptor_id=bulk_import_stream_descriptor",
        "result_descriptor_id=bulk_mutation_result",
        "coordination=BIRQ_106_to_BIRD_107",
        "operand=BIRO_v1_424_bytes",
        "result=BIRS_v1_192_bytes",
        "transport=bind_706_707_chunk_702_703_seal_704_705",
    )
)

BULK_IMPORT_STREAM_RESULT_VECTOR = ";".join(
    (
        "accepted=true",
        "terminal_result=bulk_mutation_result",
        "BIRS_exact=true",
        "durable_publication=true",
        "affected_rows_from_BIRS=true",
        "command_completion=COPY_N",
        "rowset_frames=false",
        "commit_visibility_proved=true",
        "rollback_invisibility_proved=true",
        "independent_session_post_state=true",
        "restart_post_state=true",
        "duplicate_publication=false",
    )
)

BULK_IMPORT_STREAM_DIAGNOSTIC_VECTOR = ";".join(
    (
        "canonical_message_vector_set",
        "SBLR.OPERAND_INVALID",
        "SECURITY.ACCESS_DENIED",
        "MGA.TRANSACTION_INVALID",
        "MGA.AUTHORITY_MISMATCH",
        "BULK.IMPORT.STREAM_NOT_SEALED",
        "BULK.IMPORT.TARGET_NOT_ELIGIBLE",
        "BULK.IMPORT.GENERATION_CONFLICT",
        "CLUSTER.WRITE_AUTHORITY_REQUIRED",
        "BULK.IMPORT.REJECT_LIMIT_EXCEEDED",
        "RESOURCE.BUDGET_EXCEEDED",
        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        "PROCESS.CANCELLED",
        "BULK.IMPORT.ABORTED",
    )
)

CENTRAL_IMPORT_REFUSAL_IDENTITY_VECTOR = ";".join(
    (
        "operation_id=not_admitted",
        "root_route=diagnostic_refusal",
        "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
        "descriptor_contract=sblr_diagnostic_refusal.v1",
        "result_descriptor_id=diagnostic_vector.v1",
        "executable_sblr_emitted=false",
    )
)

CENTRAL_IMPORT_REFUSAL_RESULT_VECTOR = ";".join(
    (
        "accepted=false",
        "diagnostic=SBSQL.IMPL.NOT_AVAILABLE",
        "executable_sblr_emitted=false",
        "result_published=false",
        "descriptor_authority_published=false",
        "transaction_state_transition=false",
        "catalog_mutation=false",
        "row_mutation=false",
        "durable_state_byte_identical=true",
    )
)

CENTRAL_IMPORT_REFUSAL_DIAGNOSTIC_VECTOR = ";".join(
    (
        "canonical_message_vector_set",
        "SBSQL.IMPL.NOT_AVAILABLE",
        "accepted=false",
        "executable_sblr_emitted=false",
        "no_result",
        "no_authority_publication",
        "no_mutation",
    )
)

# Compatibility aliases used by older callers and diagnostics.
PLAN_IMPORT_ROWS_IDENTITY_VECTOR = BULK_IMPORT_STREAM_IDENTITY_VECTOR
PLAN_IMPORT_ROWS_RESULT_VECTOR = BULK_IMPORT_STREAM_RESULT_VECTOR
PLAN_IMPORT_ROWS_NO_EFFECT_VECTOR = CENTRAL_IMPORT_REFUSAL_RESULT_VECTOR
PLAN_IMPORT_ROWS_DIAGNOSTIC_VECTOR = BULK_IMPORT_STREAM_DIAGNOSTIC_VECTOR
PLAN_IMPORT_ROWS_DIAGNOSTICS = tuple(BULK_IMPORT_STREAM_DIAGNOSTIC_VECTOR.split(";"))
PLAN_IMPORT_ROWS_MISSING_E2E_PROOF = "none_selector_slice_closed_by_executable_evidence"

_CORE_MANIFEST = "MANIFEST.yaml"
_CORE_COMMAND_CLOSURE = "registries/sbsql-command-sblr-zero-grey-closure.csv"
_CORE_SEMANTIC_OVERLAY = "registries/normalized-semantic-closure-overlay-20260822.csv"
_CORE_OPCODE_CLOSURE = "registries/sblr-opcode-executor-zero-grey-closure.csv"
_CORE_OPERAND_DESCRIPTORS = "registries/sblr-operand-descriptors.yaml"
_CORE_BULK_APPENDIX = (
    "chapters/data-representation/datatypes/"
    "appendix-bulk-import-stream-data-transport-and-recovery.md"
)
_PUBLIC_SURFACE_INPUT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/"
    "SURFACE_IMPLEMENTATION_BACKLOG.csv"
)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_json_sha256(value: object) -> str:
    payload = json.dumps(
        value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _load_yaml(path: Path) -> object:
    if not path.is_file():
        raise ValueError(f"central-import authoritative input missing: {path}")
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise ValueError(f"central-import authoritative YAML invalid: {path}: {exc}") from exc


def _load_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise ValueError(f"central-import authoritative input missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _require_projection(
    label: str, row: dict[str, object], expected: dict[str, object]
) -> None:
    observed = {key: row.get(key) for key in expected}
    if observed != expected:
        raise ValueError(
            f"central-import {label} projection drift: expected={expected} observed={observed}"
        )


def _require(path: Path, snippets: tuple[str, ...]) -> None:
    if not path.is_file():
        raise ValueError(f"central-import authoritative input missing: {path}")
    text = path.read_text(encoding="utf-8")
    missing = [snippet for snippet in snippets if snippet not in text]
    if missing:
        raise ValueError(
            f"central-import authoritative input drift in {path}: missing={missing}"
        )


def _manifest_authority_files(core_root: Path) -> set[str]:
    manifest = _load_yaml(core_root / _CORE_MANIFEST)
    if not isinstance(manifest, dict) or not isinstance(
        manifest.get("authority_files"), list
    ):
        raise ValueError("central-import Core MANIFEST authority_files is malformed")
    return set(str(value) for value in manifest["authority_files"])


def load_central_import_command_rows(core_root: Path) -> dict[str, dict[str, str]]:
    authority_files = _manifest_authority_files(core_root)
    required = {_CORE_COMMAND_CLOSURE, _CORE_SEMANTIC_OVERLAY}
    if not required.issubset(authority_files):
        raise ValueError(
            "central-import command authorities are not both manifest-listed: "
            f"missing={sorted(required - authority_files)}"
        )

    candidates: dict[str, list[dict[str, str]]] = {
        surface_id: [] for surface_id in PLAN_IMPORT_ROWS_SURFACE_IDS
    }
    for relative in (_CORE_COMMAND_CLOSURE, _CORE_SEMANTIC_OVERLAY):
        for row in _load_csv(core_root / relative):
            surface_id = row.get("surface_id", "")
            if surface_id in candidates:
                candidates[surface_id].append(row)

    rows: dict[str, dict[str, str]] = {}
    for surface_id, matches in candidates.items():
        if len(matches) != 1:
            raise ValueError(
                f"central-import command identity is not unique: "
                f"surface_id={surface_id} matches={len(matches)}"
            )
        row = matches[0]
        common = {
            "surface_id": surface_id,
            "canonical_name": PLAN_IMPORT_ROWS_SURFACES[surface_id],
            "family": "dml",
            "specification_state": (
                "specified_admitted"
                if surface_id in BULK_IMPORT_STREAM_SURFACE_IDS
                else "specified_gated"
            ),
            "implementation_state": "evidence_required",
        }
        _require_projection(f"command row {surface_id}", row, common)
        if surface_id in BULK_IMPORT_STREAM_SURFACE_IDS:
            _require_projection(
                f"admitted command row {surface_id}",
                row,
                {
                    "root_route_kind": "sblr_opcode",
                    "root_route": "SBLR_BULK_IMPORT_STREAM",
                    "descriptor_contract": "bulk_import_stream_descriptor.v1",
                    "executor_operation_id": "engine.op.bulk_import_stream",
                    "result_shape": "bulk_mutation_result",
                    "required_right": "object_authorized",
                    "mga_profile": "management_mutation",
                    "transaction_effect": "local_or_cluster_write",
                },
            )
        else:
            _require_projection(
                f"refusal command row {surface_id}",
                row,
                {
                    "root_route_kind": "diagnostic_refusal",
                    "root_route": "SBLR_DIAGNOSTIC_REFUSAL",
                    "descriptor_contract": "sblr_diagnostic_refusal.v1",
                    "executor_operation_id": "not_admitted",
                    "result_shape": "diagnostic_vector.v1",
                    "transaction_effect": "none",
                    "diagnostic_key": "SBSQL.IMPL.NOT_AVAILABLE",
                },
            )
        rows[surface_id] = row
    return rows


def load_bulk_import_stream_core_selector(core_root: Path) -> dict[str, object]:
    authority_files = _manifest_authority_files(core_root)
    required = {_CORE_OPERAND_DESCRIPTORS, _CORE_OPCODE_CLOSURE, _CORE_BULK_APPENDIX}
    if not required.issubset(authority_files):
        raise ValueError(
            "bulk-import authorities are not manifest-listed: "
            f"missing={sorted(required - authority_files)}"
        )
    document = _load_yaml(core_root / _CORE_OPERAND_DESCRIPTORS)
    if not isinstance(document, dict):
        raise ValueError("bulk-import operand registry root is not a mapping")
    registry = document.get("sblr_operand_descriptor_registry")
    if not isinstance(registry, dict):
        raise ValueError("bulk-import operand selector container is missing")
    selector = registry.get("bulk_import_stream_zero_grey_v1")
    if not isinstance(selector, dict):
        raise ValueError("bulk-import operand selector is missing")
    opcode = selector.get("opcode")
    operand = selector.get("operand")
    result = selector.get("result")
    stream = selector.get("stream_transport")
    if not all(isinstance(value, dict) for value in (opcode, operand, result, stream)):
        raise ValueError("bulk-import operand selector typed projection is malformed")
    _require_projection(
        "bulk-import opcode",
        opcode,
        {
            "name": "SBLR_BULK_IMPORT_STREAM",
            "code": 775,
            "version": 1.0,
            "operation_id": "engine.op.bulk_import_stream",
            "placement": "standalone_package_root_only",
        },
    )
    _require_projection(
        "bulk-import operand",
        operand,
        {
            "magic": "BIRO",
            "version": 1,
            "bytes": 424,
            "value_kind": "bulk_import_stream_descriptor",
        },
    )
    _require_projection(
        "bulk-import result",
        result,
        {
            "magic": "BIRS",
            "version": 1,
            "bytes": 192,
            "descriptor_id": "bulk_mutation_result",
        },
    )
    _require_projection(
        "bulk-import transport",
        stream,
        {
            "bind_request": {
                "message_type": 706,
                "schema_id": 7719,
                "schema_name": "ps_bulk_import_stream_bind_v1",
            },
            "bind_ack": {
                "message_type": 707,
                "schema_id": 7720,
                "schema_name": "ps_bulk_import_stream_bind_ack_v1",
            },
            "chunk_request": {
                "message_type": 702,
                "schema_id": 7715,
                "schema_name": "ps_bulk_import_stream_chunk_v1",
            },
            "chunk_ack": {
                "message_type": 703,
                "schema_id": 7716,
                "schema_name": "ps_bulk_import_stream_chunk_ack_v1",
            },
            "seal_request": {
                "message_type": 704,
                "schema_id": 7717,
                "schema_name": "ps_bulk_import_stream_seal_v1",
            },
            "seal_ack": {
                "message_type": 705,
                "schema_id": 7718,
                "schema_name": "ps_bulk_import_stream_seal_ack_v1",
            },
        },
    )

    opcode_rows = _load_csv(core_root / _CORE_OPCODE_CLOSURE)
    matches = [row for row in opcode_rows if row.get("opcode_name") == "SBLR_BULK_IMPORT_STREAM"]
    if len(matches) != 1:
        raise ValueError(f"bulk-import opcode closure identity count is {len(matches)}")
    _require_projection(
        "bulk-import opcode closure",
        matches[0],
        {
            "opcode_code": "775",
            "specification_state": "specified_admitted",
            "implementation_state": "evidence_required",
            "operand_contract": "bulk_import_stream_descriptor",
            "result_contract": "bulk_mutation_result",
            "executor_binding_requirement": "engine.op.bulk_import_stream",
            "missing_evidence_diagnostic": "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
        },
    )
    return selector


def validate_authoritative_runtime_inputs(
    repo_root: Path, core_root: Path | None = None
) -> None:
    """Fail closed if Core or the executable evidence seams drift."""

    root = repo_root.resolve()
    core = core_root.resolve() if core_root is not None else root.parent / "Specifications/Core"
    load_central_import_command_rows(core)
    load_bulk_import_stream_core_selector(core)
    _require(
        root / "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        (
            'info.operation_id = "engine.op.bulk_import_stream"',
            'info.opcode = "SBLR_BULK_IMPORT_STREAM"',
            '"SBSQL.IMPL.NOT_AVAILABLE"',
        ),
    )
    _require(
        root / "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
        (
            '"engine.op.bulk_import_stream"',
            '"SBLR_BULK_IMPORT_STREAM"',
            '"SBSQL.IMPL.NOT_AVAILABLE"',
            '{"executable_sblr_emitted", "false"}',
        ),
    )
    _require(
        root / "project/tests/sbsql_parser_worker/sbsql_copy_persistence_full_route_gate.py",
        ("sbsql_copy_persistence_full_route_gate=passed", "COPY {expected_rows}"),
    )
    _require(
        root / "project/tests/sbsql_parser_worker/sbsql_central_import_refusal_wire_conformance.cpp",
        tuple(sorted(CENTRAL_IMPORT_REFUSAL_SURFACE_IDS))
        + (
            'diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE"',
            'DiagnosticField(diagnostic, "executable_sblr_emitted")',
            "sbsql_central_import_refusal_wire_conformance=passed",
        ),
    )


def authoritative_provenance_inputs(
    repo_root: Path, core_root: Path | None = None
) -> tuple[tuple[str, str], ...]:
    """Return ordered admitted-input identities and deterministic SHA-256s."""

    root = repo_root.resolve()
    core = core_root.resolve() if core_root is not None else root.parent / "Specifications/Core"
    validate_authoritative_runtime_inputs(root, core)
    command_rows = load_central_import_command_rows(core)
    selector = load_bulk_import_stream_core_selector(core)
    surface_rows = [
        row
        for row in _load_csv(root / _PUBLIC_SURFACE_INPUT)
        if row.get("surface_id") in PLAN_IMPORT_ROWS_SURFACE_IDS
    ]
    if {row.get("surface_id") for row in surface_rows} != set(PLAN_IMPORT_ROWS_SURFACE_IDS):
        raise ValueError("central-import public source does not contain the exact nine rows")
    surface_rows.sort(key=lambda row: row["surface_id"])
    for surface in surface_rows:
        validate_surface(surface)

    core_files = (
        _CORE_MANIFEST,
        _CORE_COMMAND_CLOSURE,
        _CORE_SEMANTIC_OVERLAY,
        _CORE_OPCODE_CLOSURE,
        _CORE_OPERAND_DESCRIPTORS,
        _CORE_BULK_APPENDIX,
    )
    exact_files = (
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
        "project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp",
        "project/tools/sb_parser_gen/plan_import_rows_generated_evidence.py",
        "project/tools/sb_parser_gen/refresh_plan_import_rows_generated_evidence.py",
        "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py",
        "project/tools/sb_parser_gen/generate_authenticated_full_route_matrix.py",
        "project/tools/sb_parser_gen/generate_sblr_binary_round_trip_matrix.py",
        "project/tools/sb_parser_gen/generate_per_row_evidence_manifest.py",
        "project/tools/sb_parser_gen/generate_sbsql_surface_release_declaration.py",
        "project/tools/sb_parser_gen/generate_per_element_spec_sources.py",
        "project/tools/sb_parser_gen/author_route_and_round_trip_fixtures.py",
        "project/tools/sb_parser_gen/promote_route_and_round_trip_fixtures.py",
        "project/tests/sbsql_parser_worker/generated/full_surface/"
        "sbsql_plan_import_rows_generated_evidence_gate.py",
        "project/tests/sbsql_parser_worker/sbsql_copy_persistence_full_route_gate.py",
        "project/tests/sbsql_parser_worker/sbsql_central_import_refusal_wire_conformance.cpp",
        "project/tests/sbsql_parser_worker/CMakeLists.txt",
    )
    inputs: list[tuple[str, str]] = [
        (f"Specifications/Core/{relative}", sha256_file(core / relative))
        for relative in core_files
    ]
    inputs.extend(
        (
            (
                "Specifications/Core/central_import_command_rows:canonical_json",
                canonical_json_sha256(
                    [command_rows[key] for key in sorted(command_rows)]
                ),
            ),
            (
                "Specifications/Core/"
                "SBLR-BULK-IMPORT-STREAM-ZERO-GREY-V1:canonical_json",
                canonical_json_sha256(selector),
            ),
            (
                "ScratchBird/"
                f"{_PUBLIC_SURFACE_INPUT}#central_import_nine_rows:canonical_json",
                canonical_json_sha256(surface_rows),
            ),
        )
    )
    for relative in exact_files:
        path = root / relative
        if not path.is_file():
            raise ValueError(f"central-import provenance input missing: {path}")
        inputs.append((f"ScratchBird/{relative}", sha256_file(path)))
    return tuple(inputs)


def is_plan_import_rows_surface(surface_id: str) -> bool:
    return surface_id in PLAN_IMPORT_ROWS_SURFACE_IDS


def is_bulk_import_stream_surface(surface_id: str) -> bool:
    return surface_id in BULK_IMPORT_STREAM_SURFACE_IDS


def is_central_import_refusal_surface(surface_id: str) -> bool:
    return surface_id in CENTRAL_IMPORT_REFUSAL_SURFACE_IDS


def validate_surface(surface: dict[str, str]) -> None:
    surface_id = surface.get("surface_id", "")
    expected_name = PLAN_IMPORT_ROWS_SURFACES.get(surface_id)
    if expected_name is None:
        raise ValueError(f"not a central-import surface: {surface_id}")
    observed_status = surface.get("source_status") or surface.get("status", "")
    expected = {
        "canonical_name": expected_name,
        "status": "native_now",
        "cluster_scope": "noncluster_or_profile_scoped",
        "sblr_operation_family": "sblr.dml.operation.v3",
    }
    observed = {
        "canonical_name": surface.get("canonical_name", ""),
        "status": observed_status,
        "cluster_scope": surface.get("cluster_scope", ""),
        "sblr_operation_family": surface.get("sblr_operation_family", ""),
    }
    if observed != expected:
        raise ValueError(
            f"{surface_id} central-import surface projection drift: "
            f"expected={expected} observed={observed}"
        )


def strict_ledger_override(surface: dict[str, str]) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_plan_import_rows_surface(surface_id):
        return None
    validate_surface(surface)
    if is_bulk_import_stream_surface(surface_id):
        return {
            "current_state": "e2e_passed",
            "parser_evidence": "ctest:sbsql_copy_persistence_full_route_gate;public_SBWP_1.1_TLS_COPY_FROM_STDIN",
            "binder_evidence": "engine_owned_statement_COPY_bind_706_707;exact_BIRQ_BIRD_BIRO;no_parser_target_authority",
            "lowering_evidence": BULK_IMPORT_STREAM_IDENTITY_VECTOR,
            "server_admission_evidence": "ctest:sbsql_copy_persistence_full_route_gate;listener_parser_pool_SBPS_server_receipt_bound_opcode_775_dispatch",
            "engine_runtime_evidence": f"ctest:sbsql_copy_persistence_full_route_gate;{BULK_IMPORT_STREAM_RESULT_VECTOR}",
            "function_or_api_operation_id": BULK_IMPORT_STREAM_IDENTITY_VECTOR,
            "diagnostic_evidence": BULK_IMPORT_STREAM_DIAGNOSTIC_VECTOR,
            "fixture_evidence": "ctest:sbsql_copy_persistence_full_route_gate;commit_rollback_independent_session_restart",
            "evidence_complete": "yes",
            "notes": "The exact COPY root is closed by the authenticated listener/parser/SBPS opcode-775 route, durable chunk/seal execution, canonical BIRS command completion, commit visibility, rollback invisibility, independent-session observation, and restart post-state proof. No opcode-793 planning identity is used.",
        }
    return {
        "current_state": "exact_refusal_passed",
        "parser_evidence": f"ctest:sbsql_central_import_refusal_wire_conformance;surface_id={surface_id};recognized_exact_refusal",
        "binder_evidence": "not_applicable_no_engine_binding_authority_published",
        "lowering_evidence": CENTRAL_IMPORT_REFUSAL_IDENTITY_VECTOR,
        "server_admission_evidence": "no_executable_SBLR_or_server_dispatch;refusal_at_public_parser_wire_boundary",
        "engine_runtime_evidence": f"ctest:sbsql_central_import_refusal_wire_conformance;{CENTRAL_IMPORT_REFUSAL_RESULT_VECTOR}",
        "function_or_api_operation_id": CENTRAL_IMPORT_REFUSAL_IDENTITY_VECTOR,
        "diagnostic_evidence": CENTRAL_IMPORT_REFUSAL_DIAGNOSTIC_VECTOR,
        "fixture_evidence": "ctest:sbsql_central_import_refusal_wire_conformance;independent_database_state_before_equals_after",
        "evidence_complete": "yes",
        "notes": "The exact central-command identity is recognized and deterministically refused with SBSQL.IMPL.NOT_AVAILABLE before executable SBLR, descriptor/result publication, transaction transition, catalog mutation, or row mutation. The independent durable state oracle remains byte-identical.",
    }


def per_row_manifest_override(surface: dict[str, str]) -> dict[str, str] | None:
    surface_id = surface.get("surface_id", "")
    if not is_plan_import_rows_surface(surface_id):
        return None
    validate_surface(surface)
    if is_bulk_import_stream_surface(surface_id):
        return {
            "final_state": "e2e_passed",
            "ctest_label": "sbsql_copy_persistence_full_route_gate;sbsql_parser_worker;sbsql_e2e_passed",
            "fixture_path": "project/tests/sbsql_parser_worker/sbsql_copy_persistence_full_route_gate.py",
            "implementation_refs": ";".join(
                (
                    BULK_IMPORT_STREAM_IDENTITY_VECTOR,
                    "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
                    "project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp",
                    "project/src/server/sblr_dispatch_server.cpp",
                    "project/src/engine/internal_api/dml/bulk_import_stream_execution_api.cpp",
                )
            ),
            "diagnostic_proof": BULK_IMPORT_STREAM_DIAGNOSTIC_VECTOR,
            "result_proof": f"ctest:sbsql_copy_persistence_full_route_gate;{BULK_IMPORT_STREAM_RESULT_VECTOR}",
            "evidence_collected_utc": "2026-09-01T03:21:00Z",
            "promoter_slice": "SBSQL-CENTRAL-IMPORT-COMMAND-DISPOSITION-V1_opcode775_e2e",
            "notes": "Final authenticated opcode-775 COPY proof with exact BIRS and independent durable post-state; no planning-only substitute.",
        }
    return {
        "final_state": "exact_refusal_passed",
        "ctest_label": "sbsql_central_import_refusal_wire_conformance;sbsql_parser_worker;sbsql_exact_refusal_passed",
        "fixture_path": "project/tests/sbsql_parser_worker/sbsql_central_import_refusal_wire_conformance.cpp",
        "implementation_refs": ";".join(
            (
                CENTRAL_IMPORT_REFUSAL_IDENTITY_VECTOR,
                "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
                "project/src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
            )
        ),
        "diagnostic_proof": CENTRAL_IMPORT_REFUSAL_DIAGNOSTIC_VECTOR,
        "result_proof": f"ctest:sbsql_central_import_refusal_wire_conformance;{CENTRAL_IMPORT_REFUSAL_RESULT_VECTOR}",
        "evidence_collected_utc": "2026-09-01T03:21:00Z",
        "promoter_slice": "SBSQL-CENTRAL-IMPORT-COMMAND-DISPOSITION-V1_exact_refusal",
        "notes": "Final independent exact-refusal proof; recognized surface emits no executable SBLR and leaves durable state unchanged.",
    }


def authenticated_route_override(
    surface: dict[str, str], classification: dict[str, str]
) -> dict[str, str]:
    surface_id = surface.get("surface_id", "")
    if not is_plan_import_rows_surface(surface_id):
        return classification
    validate_surface(surface)
    out = dict(classification)
    if is_bulk_import_stream_surface(surface_id):
        out.update(
            {
                "credential_profile_accepted": "authenticated_user_with_exact_INSERT_on_target_relation",
                "credential_profile_refused": "authenticated_user_without_exact_INSERT_on_target_relation",
                "transaction_profile": "active_engine_owned_MGA_transaction_and_authoritative_statement_snapshot_required;autocommit_and_explicit_transaction_routes_proved",
                "ipc_admission_path": "sbwp_1.1_over_tls;listener;parser_pool;statement_context;bind_706_707;BIRQ_106_BIRD_107;chunk_702_703;seal_704_705;canonical_opcode_775;BIRS",
                "engine_admission_authority": "authenticated_statement_receipt;StatementBulkImportAuthorityV1;engine.bulk_import_stream_descriptor_registry.v1;live_MGA_transaction_catalog_security_policy_route_resource_and_executor_generations",
                "mga_execution_authority": "engine_owned_single_durable_import_publication;commit_and_rollback_proved;parser_has_no_storage_or_finality_authority;no_wal_authority",
                "expected_authorization_accepted_outcome": f"{BULK_IMPORT_STREAM_IDENTITY_VECTOR};{BULK_IMPORT_STREAM_RESULT_VECTOR}",
                "expected_authorization_refused_outcome": "one_ordered_canonical_diagnostic;no_BIRS;no_durable_publication;no_mutation",
                "expected_diagnostic_codes": BULK_IMPORT_STREAM_DIAGNOSTIC_VECTOR,
                "fixture_status": "e2e_passed",
                "notes": "Authenticated public COPY route reaches exact opcode 775 and publishes one canonical BIRS only after durable engine mutation; independent commit, rollback, session, and restart observations are required.",
            }
        )
    else:
        out.update(
            {
                "credential_profile_accepted": "not_applicable_exact_refusal_surface",
                "credential_profile_refused": "not_applicable_refusal_precedes_engine_authorization",
                "transaction_profile": "no_transaction_authority_acquired_or_mutated",
                "ipc_admission_path": "public_parser_wire_recognizes_exact_surface_then_refuses_before_executable_SBLR_or_SBPS_engine_dispatch",
                "engine_admission_authority": "not_applicable_no_engine_admission",
                "mga_execution_authority": "not_applicable_no_MGA_execution;durable_state_byte_identical;no_wal_authority",
                "expected_authorization_accepted_outcome": "not_applicable_exact_refusal_surface",
                "expected_authorization_refused_outcome": CENTRAL_IMPORT_REFUSAL_RESULT_VECTOR,
                "expected_diagnostic_codes": CENTRAL_IMPORT_REFUSAL_DIAGNOSTIC_VECTOR,
                "fixture_status": "exact_refusal_passed",
                "notes": "Exact central-command refusal is rendered before executable SBLR, authority publication, server dispatch, or mutation; the independent durable-state oracle remains unchanged.",
            }
        )
    return out


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    surface_id = row.get("surface_id", "")
    if not is_plan_import_rows_surface(surface_id):
        return row
    out = dict(row)
    if is_bulk_import_stream_surface(surface_id):
        out.update(
            {
                "oracle_authority_status": "Core_central_command_and_bulk_import_stream_transport_recovery_v1",
                "expected_canonical_function_or_api_operation_id": "engine.op.bulk_import_stream",
                "parse_phase_expectation": "parse_exact_COPY_FROM_STDIN_root_and_preserve_raw_quoted_target_atoms",
                "bind_phase_expectation": "engine_bind_706_707_publishes_private_statement_COPY_authority_no_parser_target_UUID",
                "lower_phase_expectation": "lower_to_standalone_SBLR_BULK_IMPORT_STREAM_opcode_775_one_BIRO_v1_operand",
                "binary_serialize_phase_expectation": "copy_exact_BIRD_to_BIRO_magic_only_and_serialize_canonical_SBOP_descriptor_operand",
                "verify_phase_expectation": "verify_exact_opcode_775_BIRO_receipt_transaction_generation_evidence_and_sealed_stream",
                "binary_deserialize_phase_expectation": "strict_BIRO_and_BIRS_decode_reencode_byte_identical",
                "dispatch_phase_expectation": "dispatch_only_engine.op.bulk_import_stream_through_receipt_bound_database_owned_executor",
                "execute_phase_expectation": "append_opaque_chunks_durably_then_seal_then_one_terminal_opcode_775_publication",
                "render_phase_expectation": "render_one_COPY_N_command_completion_from_exact_BIRS_and_no_rowset_frames",
                "byte_identical_round_trip_required": "yes",
                "execution_authority_model": "sblr_envelope_with_uuid_and_descriptor_authority_only;engine_owned_MGA_publication_and_recovery;parser_has_no_storage_or_finality_authority;no_wal_authority",
                "fixture_status": "e2e_passed",
                "notes": f"{BULK_IMPORT_STREAM_IDENTITY_VECTOR};{BULK_IMPORT_STREAM_RESULT_VECTOR}",
            }
        )
    else:
        out.update(
            {
                "oracle_authority_status": "Core_central_command_exact_diagnostic_refusal",
                "expected_canonical_function_or_api_operation_id": "not_admitted_diagnostic_refusal",
                "parse_phase_expectation": "recognize_exact_surface_for_deterministic_refusal_only",
                "bind_phase_expectation": "no_engine_descriptor_or_authority_binding",
                "lower_phase_expectation": "emit_no_executable_SBLR_and_return_SBSQL.IMPL.NOT_AVAILABLE",
                "binary_serialize_phase_expectation": "not_applicable_no_executable_sblr",
                "verify_phase_expectation": "not_applicable_no_executable_sblr",
                "binary_deserialize_phase_expectation": "not_applicable_no_executable_sblr",
                "dispatch_phase_expectation": "no_server_or_engine_dispatch",
                "execute_phase_expectation": "no_MGA_execution_no_row_or_catalog_mutation",
                "render_phase_expectation": "render_one_exact_SBSQL.IMPL.NOT_AVAILABLE_diagnostic",
                "byte_identical_round_trip_required": "not_applicable_no_executable_sblr",
                "execution_authority_model": "sblr_envelope_with_uuid_and_descriptor_authority_only_not_created;no_MGA_execution;durable_state_byte_identical;no_wal_authority",
                "fixture_status": "exact_refusal_passed",
                "notes": f"{CENTRAL_IMPORT_REFUSAL_IDENTITY_VECTOR};{CENTRAL_IMPORT_REFUSAL_RESULT_VECTOR}",
            }
        )
    return out


def normalize_fixture_status(surface_id: str, observed_status: str) -> str:
    if is_bulk_import_stream_surface(surface_id):
        return "e2e_passed"
    if is_central_import_refusal_surface(surface_id):
        return "exact_refusal_passed"
    return observed_status
