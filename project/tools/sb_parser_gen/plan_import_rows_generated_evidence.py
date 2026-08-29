#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exact generated-evidence profile for ``dml.plan_import_rows``.

The nine SBsql surface rows below share one canonical engine operation.  This
module keeps their derived evidence consistent without promoting in-process
parser/SBPS/public-ABI coverage to authenticated SBWP/TLS end-to-end proof.
The fixed values are checked against the engine operation registries and pure
codec header before a generator may emit them.
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
PLAN_IMPORT_ROWS_SURFACE_IDS = frozenset(PLAN_IMPORT_ROWS_SURFACES)

PLAN_IMPORT_ROWS_CORE_SELECTOR_KEY = "SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1"
PLAN_IMPORT_ROWS_CORE_SELECTOR_SHA256 = (
    "ab12515c583c6cbb39c31929e9579dd6a53540f63e314030525ff5cc5caf48c1"
)

PLAN_IMPORT_ROWS_DIAGNOSTICS = (
    "SBLR.OPCODE_INVALID",
    "SBLR.OPERAND_INVALID",
    "SECURITY.ACCESS_DENIED",
    "MGA.TRANSACTION_INVALID",
    "MGA.AUTHORITY_MISMATCH",
    "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
    "PROCESS.CANCELLED",
    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
    "SBLR.OPERATION_UNSUPPORTED",
)
PLAN_IMPORT_ROWS_DIAGNOSTIC_VECTOR = ";".join(PLAN_IMPORT_ROWS_DIAGNOSTICS)

PLAN_IMPORT_ROWS_IDENTITY_VECTOR = ";".join(
    (
        "operation_id=dml.plan_import_rows",
        "opcode=SBLR_DML_PLAN_IMPORT_ROWS",
        "opcode_code=793",
        "opcode_version=1.0",
        "request_descriptor_id=import_rows_plan_descriptor",
        "request_descriptor_version=1",
        "result_descriptor_id=import_plan_result",
        "result_descriptor_version=1",
        "executor_evidence_carrier=IPEV",
        "executor_evidence_version=1",
        "executor_evidence_bytes=208",
    )
)

PLAN_IMPORT_ROWS_RESULT_VECTOR = ";".join(
    (
        "surface_accepted=true",
        "planning_only=true",
        "execution_requires_execute_import_rows=true",
        "row_execution_completed=false",
        "row_persistence_claimed=false",
        "normalized_insert_mode=exact_closed_numeric_code",
        "normalized_source_kind=exact_closed_numeric_code",
        "normalized_format_family=exact_closed_numeric_code",
        "mapped_column_count=exact_IMAP_mapping_count",
        "validated_request_descriptor_uuid=exact_IPLP_descriptor_uuid",
        "validated_request_descriptor_generation=exact_IPLP_descriptor_generation",
        "validated_request_projection_sha256=exact_IPLP_projection_sha256",
    )
)

PLAN_IMPORT_ROWS_NO_EFFECT_VECTOR = ";".join(
    (
        "transaction_effect=read",
        "mga_execution_performed=false",
        "row_decode_performed=false",
        "row_mutation=false",
        "catalog_mutation=false",
        "transaction_inventory_mutation=false",
        "transaction_state_transition=false",
        "commit_rollback_savepoint=false",
        "durable_plan_record=false",
        "finality_authority=false",
        "no_wal_authority",
    )
)

PLAN_IMPORT_ROWS_MISSING_E2E_PROOF = (
    "pending_independent_authenticated_SBWP_1.1_over_TLS_separate_session_route_"
    "through_listener_parser_pool_SBPS_public_ABI_with_post_state_success_refusal_"
    "no_mutation_proof"
)

_SBLR_API_MATRIX = "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
_ENGINE_API_REGISTRY = "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml"
_CODEC_HEADER = "project/src/engine/sblr/sblr_plan_import_rows_codec.hpp"
_IMPORT_API_HEADER = "project/src/engine/internal_api/dml/import_api.hpp"
_PUBLIC_SURFACE_INPUT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/"
    "SURFACE_IMPLEMENTATION_BACKLOG.csv"
)
_CORE_MANIFEST = "MANIFEST.yaml"
_CORE_OPERAND_DESCRIPTORS = "registries/sblr-operand-descriptors.yaml"
_CORE_SELECTOR_CONTAINER = "sblr_operand_descriptor_registry"
_CORE_SELECTOR_ENTRY = "dml_plan_import_rows_zero_grey_v1"


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_json_sha256(value: object) -> str:
    payload = json.dumps(
        value,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _load_yaml(path: Path) -> object:
    if not path.is_file():
        raise ValueError(f"plan-import authoritative input missing: {path}")
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise ValueError(f"plan-import authoritative YAML invalid: {path}: {exc}") from exc


def _single_mapping(
    rows: object,
    field: str,
    value: str,
    label: str,
) -> dict[str, object]:
    if not isinstance(rows, list):
        raise ValueError(f"plan-import {label} row set is not a list")
    matches = [
        row
        for row in rows
        if isinstance(row, dict) and row.get(field) == value
    ]
    if len(matches) != 1:
        raise ValueError(
            f"plan-import {label} identity is not unique: field={field} "
            f"value={value} matches={len(matches)}"
        )
    return matches[0]


def load_plan_import_rows_core_selector(core_root: Path) -> dict[str, object]:
    """Load the one manifest-admitted Core selector and require exact equality."""

    root = core_root.resolve()
    manifest = _load_yaml(root / _CORE_MANIFEST)
    if not isinstance(manifest, dict):
        raise ValueError("plan-import Core MANIFEST root is not a mapping")
    authority_files = manifest.get("authority_files")
    if not isinstance(authority_files, list) or authority_files.count(
        _CORE_OPERAND_DESCRIPTORS
    ) != 1:
        raise ValueError(
            "plan-import Core selector registry is not admitted exactly once by "
            "MANIFEST.yaml authority_files"
        )

    document = _load_yaml(root / _CORE_OPERAND_DESCRIPTORS)
    if not isinstance(document, dict):
        raise ValueError("plan-import Core operand registry root is not a mapping")
    registry = document.get(_CORE_SELECTOR_CONTAINER)
    if not isinstance(registry, dict):
        raise ValueError("plan-import Core operand selector container is missing")
    selector = registry.get(_CORE_SELECTOR_ENTRY)
    if not isinstance(selector, dict):
        raise ValueError("plan-import Core selector entry is missing")
    matches = [
        key
        for key, candidate in registry.items()
        if isinstance(candidate, dict)
        and candidate.get("search_key") == PLAN_IMPORT_ROWS_CORE_SELECTOR_KEY
    ]
    if matches != [_CORE_SELECTOR_ENTRY]:
        raise ValueError(
            "plan-import Core search key is not uniquely bound to the canonical entry: "
            f"matches={matches}"
        )
    observed_sha256 = canonical_json_sha256(selector)
    if observed_sha256 != PLAN_IMPORT_ROWS_CORE_SELECTOR_SHA256:
        raise ValueError(
            "plan-import Core selector exact projection drift: "
            f"expected={PLAN_IMPORT_ROWS_CORE_SELECTOR_SHA256} "
            f"observed={observed_sha256}"
        )
    return selector


def _load_runtime_rows(
    repo_root: Path,
) -> tuple[dict[str, object], dict[str, object]]:
    sblr_document = _load_yaml(repo_root / _SBLR_API_MATRIX)
    engine_document = _load_yaml(repo_root / _ENGINE_API_REGISTRY)
    if not isinstance(sblr_document, dict) or not isinstance(engine_document, dict):
        raise ValueError("plan-import engine registry document root is not a mapping")
    sblr_row = _single_mapping(
        sblr_document.get("entries"),
        "api_operation_id",
        "dml.plan_import_rows",
        "SBLR API matrix",
    )
    engine_row = _single_mapping(
        engine_document.get("operations"),
        "operation_id",
        "dml.plan_import_rows",
        "engine API registry",
    )
    return sblr_row, engine_row


def _require_projection(
    label: str,
    row: dict[str, object],
    expected: dict[str, object],
) -> None:
    observed = {key: row.get(key) for key in expected}
    if observed != expected:
        raise ValueError(
            f"plan-import {label} exact projection drift: "
            f"expected={expected} observed={observed}"
        )


def _require(path: Path, snippets: tuple[str, ...]) -> None:
    if not path.is_file():
        raise ValueError(f"plan-import authoritative input missing: {path}")
    text = path.read_text(encoding="utf-8")
    missing = [snippet for snippet in snippets if snippet not in text]
    if missing:
        raise ValueError(
            f"plan-import authoritative input drift in {path}: missing={missing}"
        )


def _require_block(
    path: Path,
    anchor: str,
    next_anchor: str,
    snippets: tuple[str, ...],
) -> None:
    if not path.is_file():
        raise ValueError(f"plan-import authoritative input missing: {path}")
    text = path.read_text(encoding="utf-8")
    start = text.find(anchor)
    if start < 0:
        raise ValueError(f"plan-import authoritative block missing in {path}: {anchor.strip()}")
    end = text.find(next_anchor, start + len(anchor))
    block = text[start : len(text) if end < 0 else end]
    missing = [snippet for snippet in snippets if snippet not in block]
    if missing:
        raise ValueError(
            f"plan-import authoritative block drift in {path}: missing={missing}"
        )


def validate_authoritative_runtime_inputs(
    repo_root: Path,
    core_root: Path | None = None,
) -> None:
    """Fail closed if Core or the live engine mirrors drift from the tuple."""

    root = repo_root.resolve()
    core = (
        core_root.resolve()
        if core_root is not None
        else root.parent / "Specifications/Core"
    )
    selector = load_plan_import_rows_core_selector(core)
    sblr_row, engine_row = _load_runtime_rows(root)
    opcode = selector["opcode"]
    request_descriptor = selector["request_descriptor"]
    descriptor_registry = selector["descriptor_registry"]
    result = selector["result"]
    executor = selector["executor"]
    diagnostic_reconciliation = selector["diagnostic_reconciliation"]
    if not all(
        isinstance(value, dict)
        for value in (
            opcode,
            request_descriptor,
            descriptor_registry,
            result,
            executor,
            diagnostic_reconciliation,
        )
    ):
        raise ValueError("plan-import Core selector typed projection is malformed")

    executor_row = {
        "executor_id": executor["executor_id"],
        "opcode_code": executor["opcode_code"],
        "opcode_version": executor["opcode_version"],
        "operand_descriptor_id": executor["operand_descriptor_id"],
        "result_descriptor_id": executor["result_descriptor_id"],
        "result_descriptor_version": executor["result_descriptor_version"],
    }
    common_projection = {
        "semantic_contract_key": selector["search_key"],
        "operand_contract": executor["operand_descriptor_id"],
        "operand_descriptor_class": request_descriptor["descriptor_class"],
        "result_contract": result["descriptor_id"],
        "executor_id": executor["executor_id"],
        "executor_availability_registry": executor["availability_registry"],
        "executor_availability_row": executor_row,
        "transaction_effect": opcode["transaction_effect"],
        "security_class": opcode["security_class"],
        "required_rights": selector["authority"]["required_rights"],
        "accepts_names": False,
        "requires_cluster_authority": "conditional",
        "cluster_activation_predicate": (
            "cluster_context_or_cluster_transaction_or_route_fence_present"
        ),
        "cluster_provider_capability": "cluster.context_execution.v1",
        "cluster_local_fallback": "forbidden",
        "cluster_refusal_diagnostic": (
            diagnostic_reconciliation["cluster_gateway_refusal"]
        ),
        "parser_text_authority": "forbidden",
        "invalid_transaction_diagnostic": (
            diagnostic_reconciliation[
                "absent_ended_or_nonactive_transaction_identity"
            ]
        ),
        "stale_or_incompatible_live_binding_diagnostic": (
            diagnostic_reconciliation[
                "live_transaction_authority_snapshot_or_profile_mismatch"
            ]
        ),
        "diagnostic_precedence": selector["refusal_precedence"],
    }
    _require_projection(
        "SBLR API matrix",
        sblr_row,
        {
            **common_projection,
            "sblr_operation": opcode["name"],
            "opcode_code": opcode["code"],
            "opcode_version": opcode["version"],
            "opcode_family": opcode["family"],
            "api_operation_id": opcode["operation_id"],
            "operand_reference_contract": "SBOP_v1_descriptor_ref_exact_24_bytes",
            "result_shape": result["descriptor_id"],
            "executor_evidence_required": True,
            "executor_evidence_accepted": True,
            "missing_executor_evidence_diagnostic": (
                "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
            ),
            "unsupported_diagnostic": (
                diagnostic_reconciliation[
                    "recognized_but_unadmitted_source_format_pair_or_policy_profile"
                ]
            ),
            "api_function_name": "EnginePlanImportRows",
            "request_type": "EnginePlanImportRowsRequest",
            "result_type": "EnginePlanImportRowsResult",
            "required_transaction_context": True,
            "required_security_context": True,
            "descriptor_authority": descriptor_registry["relation"],
            "descriptor_binding": descriptor_registry["current_resolution"],
            "current_implementation_status": "behavior_implemented",
        },
    )
    _require_projection(
        "engine API registry",
        engine_row,
        {
            **common_projection,
            "operation_id": opcode["operation_id"],
            "sblr_opcode": opcode["name"],
            "sblr_opcode_code": opcode["code"],
            "sblr_opcode_version": opcode["version"],
            "authority_domain": descriptor_registry["relation"],
            "descriptor_binding": descriptor_registry["current_resolution"],
            "function_name": "EnginePlanImportRows",
            "request_type": "EnginePlanImportRowsRequest",
            "result_type": "EnginePlanImportRowsResult",
            "requires_transaction_context": True,
            "requires_security_context": True,
            "implementation_status": "behavior_implemented",
        },
    )

    common = (
        "import_rows_plan_descriptor",
        "import_plan_result",
        '"1.0"',
        "opcode_code: 793",
        "result_descriptor_version: 1",
        "transaction_effect: read",
    )
    _require_block(
        root / _SBLR_API_MATRIX,
        "  - sblr_operation: SBLR_DML_PLAN_IMPORT_ROWS\n",
        "\n  - sblr_operation:",
        ("api_operation_id: dml.plan_import_rows",) + common,
    )
    _require_block(
        root / _ENGINE_API_REGISTRY,
        "  - operation_id: dml.plan_import_rows\n",
        "\n  - operation_id:",
        tuple(value.replace("opcode_code:", "sblr_opcode_code:") for value in common)
        + ("sblr_opcode: SBLR_DML_PLAN_IMPORT_ROWS",),
    )
    _require(
        root / _CODEC_HEADER,
        (
            "kPlanImportRowsOpcodeCodeV1 = 793",
            "kPlanImportRowsExecutorEvidenceBytesV1 = 208",
            "kPlanImportRowsAcceptedValidationBitsV1",
            "0x00000000000003ffULL",
            "SB_ENGINE_SBLR_PLAN_IMPORT_ROWS_CODEC_V1",
        ),
    )
    _require(
        root / _IMPORT_API_HEADER,
        (
            "struct EnginePlanImportRowsResult",
            "bool surface_accepted = false",
            "bool planning_only = false",
            "bool execution_requires_execute_import_rows = false",
            "bool row_execution_completed = false",
            "bool row_persistence_claimed = false",
            "normalized_insert_mode_code",
            "normalized_source_kind_code",
            "normalized_format_family_code",
            "mapped_column_count",
            "validated_request_descriptor_uuid",
            "validated_request_descriptor_generation",
            "validated_request_projection_sha256",
            "accepted_executor_evidence",
        ),
    )


def authoritative_provenance_inputs(
    repo_root: Path,
    core_root: Path | None = None,
) -> tuple[tuple[str, str], ...]:
    """Return ordered admitted-input identities and deterministic SHA-256s."""

    root = repo_root.resolve()
    core = (
        core_root.resolve()
        if core_root is not None
        else root.parent / "Specifications/Core"
    )
    validate_authoritative_runtime_inputs(root, core)
    selector = load_plan_import_rows_core_selector(core)
    sblr_row, engine_row = _load_runtime_rows(root)

    with (root / _PUBLIC_SURFACE_INPUT).open(newline="", encoding="utf-8") as handle:
        surface_rows = [
            row
            for row in csv.DictReader(handle)
            if row.get("surface_id") in PLAN_IMPORT_ROWS_SURFACE_IDS
        ]
    if {row.get("surface_id") for row in surface_rows} != set(
        PLAN_IMPORT_ROWS_SURFACE_IDS
    ):
        raise ValueError("plan-import public source does not contain the exact nine rows")
    surface_rows.sort(key=lambda row: row["surface_id"])
    for surface in surface_rows:
        validate_surface(surface)

    exact_files = (
        _CODEC_HEADER,
        _IMPORT_API_HEADER,
        "project/tools/sb_parser_gen/plan_import_rows_generated_evidence.py",
        "project/tools/sb_parser_gen/refresh_plan_import_rows_generated_evidence.py",
        "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py",
        "project/tools/sb_parser_gen/generate_authenticated_full_route_matrix.py",
        "project/tools/sb_parser_gen/generate_sblr_binary_round_trip_matrix.py",
        "project/tools/sb_parser_gen/generate_per_row_evidence_manifest.py",
        "project/tools/sb_parser_gen/generate_sbsql_surface_release_declaration.py",
        "project/tools/sb_parser_gen/generate_per_element_spec_sources.py",
        "project/tools/sb_parser_gen/author_route_and_round_trip_fixtures.py",
        "project/tests/sbsql_parser_worker/generated/full_surface/"
        "sbsql_plan_import_rows_generated_evidence_gate.py",
        "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
        "project/tests/sbsql_parser_worker/sbsql_sbsfc_076_dml_residual_exact_route_conformance.cpp",
        "project/tests/database_lifecycle/sblr_plan_import_rows_conformance.cpp",
        "project/tests/database_lifecycle/cdp_copy_append_batching_gate.cpp",
        "project/tests/sbsql_sblr_alignment/plan_import_rows_sbps_coordination_test.cpp",
    )
    inputs: list[tuple[str, str]] = [
        ("Specifications/Core/MANIFEST.yaml", sha256_file(core / _CORE_MANIFEST)),
        (
            "Specifications/Core/registries/sblr-operand-descriptors.yaml",
            sha256_file(core / _CORE_OPERAND_DESCRIPTORS),
        ),
        (
            "Specifications/Core/registries/sblr-operand-descriptors.yaml#"
            f"{PLAN_IMPORT_ROWS_CORE_SELECTOR_KEY}:canonical_json",
            canonical_json_sha256(selector),
        ),
        (
            "ScratchBird/"
            f"{_PUBLIC_SURFACE_INPUT}#plan_import_rows_nine_rows:canonical_json",
            canonical_json_sha256(surface_rows),
        ),
        (
            "ScratchBird/"
            f"{_SBLR_API_MATRIX}#dml.plan_import_rows:canonical_json",
            canonical_json_sha256(sblr_row),
        ),
        (
            "ScratchBird/"
            f"{_ENGINE_API_REGISTRY}#dml.plan_import_rows:canonical_json",
            canonical_json_sha256(engine_row),
        ),
    ]
    for relative in exact_files:
        path = root / relative
        if not path.is_file():
            raise ValueError(f"plan-import provenance input missing: {path}")
        inputs.append((f"ScratchBird/{relative}", sha256_file(path)))
    return tuple(inputs)


def is_plan_import_rows_surface(surface_id: str) -> bool:
    return surface_id in PLAN_IMPORT_ROWS_SURFACE_IDS


def validate_surface(surface: dict[str, str]) -> None:
    surface_id = surface.get("surface_id", "")
    expected_name = PLAN_IMPORT_ROWS_SURFACES.get(surface_id)
    if expected_name is None:
        raise ValueError(f"not a plan-import surface: {surface_id}")
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
            f"{surface_id} plan-import surface projection drift: "
            f"expected={expected} observed={observed}"
        )


def strict_ledger_override(surface: dict[str, str]) -> dict[str, str] | None:
    if not is_plan_import_rows_surface(surface.get("surface_id", "")):
        return None
    validate_surface(surface)
    in_process_sources = ";".join(
        (
            "project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp",
            "project/tests/sbsql_parser_worker/sbsql_sbsfc_076_dml_residual_exact_route_conformance.cpp",
        )
    )
    engine_sources = ";".join(
        (
            "project/tests/database_lifecycle/sblr_plan_import_rows_conformance.cpp",
            "project/tests/database_lifecycle/cdp_copy_append_batching_gate.cpp",
        )
    )
    return {
        "current_state": "engine_runtime_implemented",
        "parser_evidence": f"{in_process_sources};surface_id={surface['surface_id']};in_process_only",
        "binder_evidence": "authenticated_server_binder_factory_issues_and_publishes_exact_engine_bound_import_descriptor;no_parser_identity_authority",
        "lowering_evidence": f"{in_process_sources};{PLAN_IMPORT_ROWS_IDENTITY_VECTOR};standalone_package_root_only",
        "server_admission_evidence": "project/src/server/sblr_admission.cpp;project/src/server/sblr_dispatch_server.cpp;authenticated_statement_receipt_required;in_process_SBPS_public_ABI_only",
        "engine_runtime_evidence": f"{engine_sources};{PLAN_IMPORT_ROWS_RESULT_VECTOR};{PLAN_IMPORT_ROWS_NO_EFFECT_VECTOR}",
        "function_or_api_operation_id": PLAN_IMPORT_ROWS_IDENTITY_VECTOR,
        "diagnostic_evidence": PLAN_IMPORT_ROWS_DIAGNOSTIC_VECTOR,
        "fixture_evidence": PLAN_IMPORT_ROWS_MISSING_E2E_PROOF,
        "evidence_complete": "no",
        "notes": "Exact plan-import parser, binder, standalone SBOP, public-ABI, immutable descriptor-registry, live-authority revalidation, result, IPEV, refusal-precedence, replay, and no-effect evidence exists in process. The row remains nonfinal until an independent authenticated SBWP/TLS separate-session route proves both success and refusal plus post-state no mutation; fixture authoring alone is not E2E.",
    }


def per_row_manifest_override(surface: dict[str, str]) -> dict[str, str] | None:
    if not is_plan_import_rows_surface(surface.get("surface_id", "")):
        return None
    validate_surface(surface)
    return {
        "final_state": "pending",
        "ctest_label": "sbsql_parser_worker;sblr_plan_import_rows_conformance;cdp_copy_append_batching_gate",
        "fixture_path": "project/tests/database_lifecycle/sblr_plan_import_rows_conformance.cpp;project/tests/database_lifecycle/cdp_copy_append_batching_gate.cpp",
        "implementation_refs": ";".join(
            (
                PLAN_IMPORT_ROWS_IDENTITY_VECTOR,
                "project/src/engine/internal_api/dml/import_api.cpp",
                "project/src/engine/sblr/sblr_dispatch.cpp",
                "project/src/engine/public_abi.cpp",
                "authority_domain=engine.bound_import_descriptor_registry.v1",
            )
        ),
        "diagnostic_proof": PLAN_IMPORT_ROWS_DIAGNOSTIC_VECTOR,
        "result_proof": ";".join(
            (
                PLAN_IMPORT_ROWS_RESULT_VECTOR,
                PLAN_IMPORT_ROWS_NO_EFFECT_VECTOR,
                "IPEV_accepted_validation_bits=0x00000000000003FF",
                "in_process_engine_public_ABI_proof_only",
            )
        ),
        "evidence_collected_utc": "",
        "promoter_slice": "SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1_pending_independent_SBWP_TLS_post_state_proof",
        "notes": f"Nonfinal plan-import evidence row. {PLAN_IMPORT_ROWS_MISSING_E2E_PROOF}. Existing parser, SBPS, public-ABI, engine-registry, live-MGA, cancellation, result, IPEV, rollback, commit-without-execute, and zero-mutation tests justify engine_runtime_implemented only; they do not justify e2e_passed.",
    }


def authenticated_route_override(
    surface: dict[str, str], classification: dict[str, str]
) -> dict[str, str]:
    if not is_plan_import_rows_surface(surface.get("surface_id", "")):
        return classification
    validate_surface(surface)
    out = dict(classification)
    out.update(
        {
            "credential_profile_accepted": "authenticated_user_with_exact_INSERT_on_target_relation",
            "credential_profile_refused": "authenticated_user_without_exact_INSERT_on_target_relation",
            "transaction_profile": "active_engine_owned_MGA_transaction_and_authoritative_statement_snapshot_required;validation_only_read;no_transaction_state_transition",
            "ipc_admission_path": "sbps_frame_handshake_kFrameMagic_0x53504253;canonical_sblr_container;exact_SBOS_three_child_standalone_package;SBOP_descriptor_ref_exact_24_bytes;sblr_admission;sblr_dispatch_server;authenticated_public_ABI_projection",
            "engine_admission_authority": "authenticated_statement_receipt;engine.bound_import_descriptor_registry.v1;live_engine_MGA_transaction_and_snapshot;current_target_relation_descriptor;authenticated_security_snapshot;current_policy_snapshot;current_resource_admission;current_executor_availability_generation;INSERT",
            "mga_execution_authority": "live_engine_MGA_snapshot_validation_authority_only;mga_execution_performed=false;row_decode_performed=false;row_mutation=false;catalog_mutation=false;transaction_inventory_mutation=false;transaction_state_transition=false;commit_rollback_savepoint=false;durable_plan_record=false;finality_authority=false;no_wal_authority",
            "expected_authorization_accepted_outcome": f"{PLAN_IMPORT_ROWS_IDENTITY_VECTOR};{PLAN_IMPORT_ROWS_RESULT_VECTOR};IPEV_accepted_validation_bits=0x00000000000003FF;one_terminal_import_plan_result;no_execution_or_finality",
            "expected_authorization_refused_outcome": "ordered_exact_diagnostic;no_import_plan_result_success_extensions;no_IPEV_publication;no_mutation_or_finality",
            "expected_diagnostic_codes": PLAN_IMPORT_ROWS_DIAGNOSTIC_VECTOR,
            "fixture_status": "fixture_authored",
            "notes": f"Planning-only authenticated-route contract for the exact opcode 793/version 1.0 descriptor-ref path. The tracked fixture is authored but nonfinal. {PLAN_IMPORT_ROWS_MISSING_E2E_PROOF}; no execution, mutation, transaction transition, or finality is claimed.",
        }
    )
    return out


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    if not is_plan_import_rows_surface(row.get("surface_id", "")):
        return row
    out = dict(row)
    out.update(
        {
            "oracle_authority_status": "engine_API_operation_registry_and_plan_import_rows_codec_v1",
            "expected_canonical_function_or_api_operation_id": "dml.plan_import_rows",
            "parse_phase_expectation": "parse_public_import_syntax_to_CST_pass_no_source_bytes_decoded",
            "bind_phase_expectation": "authenticated_engine_binder_resolves_target_UUID_and_publishes_immutable_engine_bound_import_descriptor_v1_no_names_as_authority",
            "lower_phase_expectation": "lower_to_standalone_SBLR_DML_PLAN_IMPORT_ROWS_opcode_code_793_version_1.0_one_import_rows_plan_descriptor_v1_request_operand",
            "binary_serialize_phase_expectation": "serialize_exact_SBOP_descriptor_ref_24_bytes_in_canonical_SBLR_container_crc32c_deterministic_byte_identical_pass",
            "verify_phase_expectation": "verify_exact_opcode_793_version_1.0_operand_descriptor_import_rows_plan_descriptor_v1_and_standalone_package_root_pass",
            "binary_deserialize_phase_expectation": "deserialize_and_reencode_exact_24_byte_descriptor_ref_byte_identical_pass_no_defaults",
            "dispatch_phase_expectation": "dispatch_only_dml.plan_import_rows_to_EnginePlanImportRows_resolve_exact_immutable_descriptor_generation_and_revalidate_live_authority",
            "execute_phase_expectation": "not_applicable_planning_only_no_MGA_execution_no_row_decode_no_mutation_no_finality_follow_on_execution_requires_dml.execute_import_rows",
            "render_phase_expectation": "render_exact_import_plan_result_v1_twelve_added_fields_and_IPEV_v1_208_bytes_or_one_ordered_diagnostic",
            "byte_identical_round_trip_required": "yes",
            "execution_authority_model": "live_engine_MGA_snapshot_validation_authority_only;sblr_envelope_with_uuid_and_descriptor_authority_only;mga_execution_performed=false;row_decode_performed=false;row_mutation=false;catalog_mutation=false;transaction_inventory_mutation=false;transaction_state_transition=false;finality_authority=false;no_wal_authority",
            "fixture_status": "fixture_authored",
            "notes": f"{PLAN_IMPORT_ROWS_IDENTITY_VECTOR};{PLAN_IMPORT_ROWS_RESULT_VECTOR};{PLAN_IMPORT_ROWS_NO_EFFECT_VECTOR}. Binary and in-process dispatch evidence is nonfinal; {PLAN_IMPORT_ROWS_MISSING_E2E_PROOF}.",
        }
    )
    return out


def normalize_fixture_status(surface_id: str, observed_status: str) -> str:
    if is_plan_import_rows_surface(surface_id) and observed_status != "pending_authoring":
        return "fixture_authored"
    return observed_status
