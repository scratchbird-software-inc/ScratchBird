#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SBLR_BINARY_ROUND_TRIP_MATRIX.csv for the SBsql Surface-to-SBLR execution_plan.

Inputs (repo-local, no network):
  project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/SURFACE_IMPLEMENTATION_BACKLOG.csv
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/FUNCTION_SEMANTIC_ORACLE_MATRIX.csv
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/PER_ROW_EVIDENCE_MANIFEST.csv

Output:
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/SBLR_BINARY_ROUND_TRIP_MATRIX.csv

For every SBsql surface row in the canonical registry, this generator
records the per-phase round-trip expectation that the SBLR binary container
gate must verify:

  SBsql text -> CST -> AST -> bound-AST -> SBLRExecutionEnvelope.v3
              -> canonical container serialize (magic 0x53424C52, 40-byte header)
              -> verifier admission
              -> binary deserialize (byte-identical envelope)
              -> engine dispatch by canonical function/API operation id
              -> ExecutionResultEnvelope.v3 + message_vector_set
              -> renderer output

Canonical container authority: `public_contract_snapshot
sblr-lowering/appendix-sblr-canonical-container-and-serialization.md`.

Per status:

- native_now rows: all 9 phases expected to pass for accepted credentials;
  serialization must be byte-identical with CRC-32C verified. When the
  oracle authority for the row is `full_oracle`, the canonical function/API
  operation id is the sblr_binding from `builtin-expression-registry.yaml`.
  When the oracle is `binding_only`, the sblr_binding is inherited from
  `builtin-sblr-expression-binding.yaml` but full descriptor oracle is
  pending. For already-final statement, grammar, contextual keyword,
  descriptor-token, and other non-callable rows, canonical operation authority
  is sourced from `PER_ROW_EVIDENCE_MANIFEST.csv`; this avoids reclassifying
  those rows as scalar functions just to author route fixtures. When neither
  oracle nor final per-row evidence provides a canonical operation id, the
  operation id is recorded as `pending_canonical_authority_entry` and
  round-trip oracle is blocked until canonical authority is extended.
- native_future rows: parse and bind may pass for diagnostic purposes,
  but lower MUST refuse with `SBSQL.SURFACE.NOT_ADMITTED`; no binary
  envelope is emitted; subsequent phases are not applicable.
- cluster_private rows: lower MUST refuse in public build with
  `SBSQL.SURFACE.NOT_ADMITTED;SBSQL.CLUSTER.AUTHORITY_REQUIRED`; no public
  binary envelope; no public dispatch or execution path.

Forbidden execution-authority sources are uniform across every row:
SQL text, identifier names, parser branch names, reference command names, and
operation-family-only routing are never execution authority. The round-trip
gate must reject any envelope whose dispatch attempts to use one of these.

Architecture invariant compliance: read-only CSV/YAML consumption; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. The matrix is a
contract-derived expectation record only. MGA copy-on-write remains
the sole transaction execution authority for accepted dispatch.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

from plan_import_rows_generated_evidence import (
    binary_round_trip_override,
    normalize_fixture_status,
    validate_authoritative_runtime_inputs,
)
from core_root_refusal_generated_evidence import (
    binary_round_trip_override as core_root_refusal_binary_round_trip_override,
    normalize_fixture_status as normalize_core_root_refusal_fixture_status,
    validate_authoritative_runtime_inputs as validate_core_root_refusal_inputs,
)
from sbsfc078_procedural_refusal_generated_evidence import (
    binary_round_trip_override as sbsfc078_refusal_binary_round_trip_override,
    normalize_fixture_status as normalize_sbsfc078_refusal_fixture_status,
    validate_authoritative_runtime_inputs as validate_sbsfc078_refusal_inputs,
)


REGISTRY_CSV = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts/"
    "SURFACE_IMPLEMENTATION_BACKLOG.csv"
)
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
ORACLE_MATRIX_NAME = "FUNCTION_SEMANTIC_ORACLE_MATRIX.csv"
PER_ROW_MANIFEST_NAME = "PER_ROW_EVIDENCE_MANIFEST.csv"
OUTPUT_NAME = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"


COLUMNS = [
    "surface_id",
    "canonical_name",
    "status",
    "cluster_scope",
    "surface_kind",
    "sblr_operation_family",
    "oracle_authority_status",
    "expected_canonical_function_or_api_operation_id",
    "parse_phase_expectation",
    "bind_phase_expectation",
    "lower_phase_expectation",
    "binary_serialize_phase_expectation",
    "verify_phase_expectation",
    "binary_deserialize_phase_expectation",
    "dispatch_phase_expectation",
    "execute_phase_expectation",
    "render_phase_expectation",
    "canonical_container_magic",
    "canonical_container_header_size_bytes",
    "byte_identical_round_trip_required",
    "crc32c_check_required",
    "engine_anchored_uuids_required",
    "forbidden_authority_sources",
    "execution_authority_model",
    "fixture_path",
    "fixture_status",
    "notes",
]


CANONICAL_CONTAINER_MAGIC = "0x53424C52"
CANONICAL_CONTAINER_HEADER_SIZE = "40"
FORBIDDEN_AUTHORITY = "sql_text;identifier_names;parser_branch_names;reference_command_names;operation_family_only_routing"
EXECUTION_AUTHORITY = "mga_copy_on_write;no_wal_authority;sblr_envelope_with_uuid_and_descriptor_authority_only"
PENDING = "pending_canonical_authority_entry"
BRIDGE_CLUSTER_ROUTE_SURFACE_ID = "SBSQL-D50EC7C4422E"
FIXTURE_KIND = "sblr_binary_round_trip"
ALLOWED_AUTHORED_FIXTURE_STATUSES = {
    "fixture_authored",
    "e2e_passed",
    "exact_refusal_passed",
}
MANIFEST_OPERATION_PATTERNS = (
    ("sblr_binding", re.compile(r"(?:^|;)sblr_binding=([^;]+)")),
    ("binding", re.compile(r"(?:^|;)binding=([^;]+)")),
    ("operation_id", re.compile(r"(?:^|;)operation_id=([^;]+)")),
    ("operation_or_function", re.compile(r"(?:^|;)operation_or_function=([^;]+)")),
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_fixture_fields(path: Path) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.lstrip().startswith("#") or line.startswith((" ", "-")):
            continue
        if ":" not in line:
            continue
        key, raw_value = line.split(":", 1)
        value = raw_value.strip()
        if value.startswith('"') and value.endswith('"'):
            value = json.loads(value)
        fields[key.strip()] = value
    return fields


def fixture_status_for(root: Path, fixture_path: str, surface_id: str) -> str:
    path = root / fixture_path
    if not path.exists():
        return "pending_authoring"
    fields = parse_fixture_fields(path)
    if fields.get("fixture_kind") != FIXTURE_KIND:
        fail(f"{fixture_path} fixture_kind mismatch: expected={FIXTURE_KIND} observed={fields.get('fixture_kind', '')}")
    if fields.get("surface_id") != surface_id:
        fail(f"{fixture_path} surface_id mismatch: expected={surface_id} observed={fields.get('surface_id', '')}")
    status = fields.get("fixture_status", "")
    if status not in ALLOWED_AUTHORED_FIXTURE_STATUSES:
        fail(f"{fixture_path} fixture_status must be one of {sorted(ALLOWED_AUTHORED_FIXTURE_STATUSES)}")
    return status


def native_now_phases(oracle_status: str, sblr_binding: str) -> dict[str, str]:
    if oracle_status == "full_oracle" and sblr_binding and sblr_binding != "oracle_pending":
        op_id = sblr_binding
        round_trip_required = "yes"
    elif oracle_status == "binding_only" and sblr_binding and sblr_binding != "oracle_pending":
        op_id = sblr_binding
        round_trip_required = "yes_descriptor_oracle_pending"
    else:
        op_id = PENDING
        round_trip_required = "blocked_until_canonical_oracle_entry"

    return {
        "expected_canonical_function_or_api_operation_id": op_id,
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass",
        "bind_phase_expectation": "bind_to_bound_ast_with_uuid_and_descriptor_pass_no_names_as_authority",
        "lower_phase_expectation": "lower_bound_ast_to_sblrexecutionenvelope_v3_with_canonical_operation_id_pass_no_text_branch_or_reference_command_as_authority",
        "binary_serialize_phase_expectation": "serialize_envelope_to_canonical_container_magic_0x53424C52_with_40byte_header_crc32c_deterministic_byte_identical_pass",
        "verify_phase_expectation": "verifier_admit_container_with_magic_version_length_checksum_and_payload_authority_pass",
        "binary_deserialize_phase_expectation": "deserialize_bytes_to_byte_identical_sblrexecutionenvelope_v3_pass",
        "dispatch_phase_expectation": "engine_dispatch_by_canonical_function_or_api_operation_id_pass_reject_family_only_routing",
        "execute_phase_expectation": "engine_execute_under_mga_copy_on_write_authority_emit_ExecutionResultEnvelope_v3_and_message_vector_set_pass",
        "render_phase_expectation": "renderer_emit_native_or_reference_render_pack_per_session_profile_pass",
        "byte_identical_round_trip_required": round_trip_required,
        "notes": "native_now row: all 9 round-trip phases must pass; serialization byte-identical with CRC-32C; engine dispatches by canonical operation id (not family-only); MGA copy-on-write is the only transaction authority; no WAL recovery path may be used at execute or persist."
        + (" Oracle is full; round-trip fixture may be authored immediately." if oracle_status == "full_oracle" else
           " Oracle binding-only; descriptor/cast/null/error oracle pending canonical builtin-expression-registry entry before fixture may close." if oracle_status == "binding_only" else
           " Canonical oracle missing; round-trip oracle blocked until canonical authority is extended or row is routed to remove_by_spec_change via SBSFC-009B."),
    }


def native_now_manifest_phases(operation_id: str, authority_status: str) -> dict[str, str]:
    return {
        "expected_canonical_function_or_api_operation_id": operation_id,
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass",
        "bind_phase_expectation": "bind_to_bound_ast_with_uuid_and_descriptor_pass_no_names_as_authority",
        "lower_phase_expectation": "lower_bound_ast_to_sblrexecutionenvelope_v3_with_canonical_operation_id_pass_no_text_branch_or_reference_command_as_authority",
        "binary_serialize_phase_expectation": "serialize_envelope_to_canonical_container_magic_0x53424C52_with_40byte_header_crc32c_deterministic_byte_identical_pass",
        "verify_phase_expectation": "verifier_admit_container_with_magic_version_length_checksum_and_payload_authority_pass",
        "binary_deserialize_phase_expectation": "deserialize_bytes_to_byte_identical_sblrexecutionenvelope_v3_pass",
        "dispatch_phase_expectation": "engine_dispatch_by_canonical_function_or_api_operation_id_pass_reject_family_only_routing",
        "execute_phase_expectation": "engine_execute_under_mga_copy_on_write_authority_emit_ExecutionResultEnvelope_v3_and_message_vector_set_pass",
        "render_phase_expectation": "renderer_emit_native_or_reference_render_pack_per_session_profile_pass",
        "byte_identical_round_trip_required": "yes",
        "notes": "native_now row: per-row manifest already records final e2e evidence with a canonical function/API operation id; all 9 round-trip phases must pass; serialization byte-identical with CRC-32C; engine dispatches by canonical operation id (not family-only); MGA copy-on-write is the only transaction authority; no WAL recovery path may be used at execute or persist."
        + f" Round-trip authority source={authority_status}.",
    }


def lifecycle_create_manifest_phases(authority_status: str) -> dict[str, str]:
    return {
        "expected_canonical_function_or_api_operation_id": "engine.op.lifecycle_create_database",
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass",
        "bind_phase_expectation": "bind_to_bound_ast_with_uuid_and_descriptor_pass_no_names_as_authority",
        "lower_phase_expectation": "lower_bound_ast_to_sblrexecutionenvelope_v3_with_canonical_operation_id_pass_no_text_branch_or_reference_command_as_authority",
        "binary_serialize_phase_expectation": "serialize_envelope_to_canonical_container_magic_0x53424C52_with_40byte_header_crc32c_deterministic_byte_identical_pass",
        "verify_phase_expectation": "verifier_admit_container_with_magic_version_length_checksum_and_payload_authority_pass",
        "binary_deserialize_phase_expectation": "deserialize_bytes_to_byte_identical_sblrexecutionenvelope_v3_pass",
        "dispatch_phase_expectation": "engine_dispatch_by_canonical_function_or_api_operation_id_pass_reject_family_only_routing",
        "execute_phase_expectation": "engine_dispatch_creates_database_with_engine_assigned_identities_and_durable_tx1_evidence",
        "render_phase_expectation": "renderer_emit_lifecycle_create_result_with_database_and_filespace_uuids",
        "byte_identical_round_trip_required": "yes",
        "notes": "The canonical SBLR container serializes, verifies, deserializes, and admits the exact engine.op.lifecycle_create_database executor identity. Dispatch reaches EngineCreateLifecycle, which creates the database with engine-assigned database/filespace UUIDs and durable tx1 evidence while preserving parser no-storage and no-finality authority. Round-trip authority source=" + authority_status + ".",
    }


def exact_refusal_manifest_phases(operation_id: str, authority_status: str) -> dict[str, str]:
    return {
        "expected_canonical_function_or_api_operation_id": operation_id,
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass",
        "bind_phase_expectation": "bind_to_bound_ast_with_uuid_and_descriptor_pass_no_names_as_authority",
        "lower_phase_expectation": "lower_bound_ast_to_sblrexecutionenvelope_v3_with_canonical_operation_id_pass_no_text_branch_or_reference_command_as_authority",
        "binary_serialize_phase_expectation": "serialize_envelope_to_canonical_container_magic_0x53424C52_with_40byte_header_crc32c_deterministic_byte_identical_pass",
        "verify_phase_expectation": "verifier_admit_container_with_magic_version_length_checksum_and_payload_authority_pass",
        "binary_deserialize_phase_expectation": "deserialize_bytes_to_byte_identical_sblrexecutionenvelope_v3_pass",
        "dispatch_phase_expectation": "engine_dispatch_by_canonical_function_or_api_operation_id_pass_reject_family_only_routing",
        "execute_phase_expectation": "engine_dispatch_returns_SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED_before_storage_mutation",
        "render_phase_expectation": "renderer_emit_refusal_diagnostic_SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED",
        "byte_identical_round_trip_required": "yes",
        "notes": "final per-row exact refusal: the canonical SBLR container still serializes, verifies, deserializes, and dispatches by operation id, but execution returns SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED before any database, security sidecar, parser authority, or MGA finality mutation. Round-trip authority source=" + authority_status + ".",
    }


def pre_sblr_exact_refusal_manifest_phases(
    authority_status: str,
    parent_operation_id: str,
    parent_sblr_operation: str,
    carrier_name: str,
) -> dict[str, str]:
    operation_token = parent_operation_id.replace(".", "_")
    if parent_operation_id == "engine.op.ddl_create_table":
        bind_expectation = "bind_constraint_child_syntax_without_parser_owned_execution_authority"
        render_expectation = "renderer_emit_SBSQL_IMPL_NOT_AVAILABLE_with_canonical_create_table_parent"
        refusal_notes = "Final per-row exact refusal occurs before executable SBLR because the authenticated engine-bound CREATE TABLE CTDX/CTDO constraint vector is unavailable. The constraint child never owns an executor. No canonical container, verifier admission, binary deserialization, server dispatch, EngineCreateConstraint SQL route, catalog mutation, or MGA finality is claimed. Round-trip authority source=" + authority_status + "."
    else:
        bind_expectation = "bind_create_schema_syntax_without_parser_owned_execution_authority"
        render_expectation = "renderer_emit_SBSQL_IMPL_NOT_AVAILABLE_with_canonical_create_schema_parent"
        refusal_notes = "Final per-row exact refusal occurs before executable SBLR because the authenticated engine-bound " + carrier_name + " carrier is unavailable. Canonical parent=" + parent_operation_id + "/" + parent_sblr_operation + ". No canonical container, verifier admission, binary deserialization, server dispatch, SQL-route EngineCreateSchema mutation, catalog mutation, or MGA finality is claimed. Round-trip authority source=" + authority_status + "."
    return {
        "expected_canonical_function_or_api_operation_id": f"not_admitted_parent_{parent_operation_id}",
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass",
        "bind_phase_expectation": bind_expectation,
        "lower_phase_expectation": f"lower_refuses_SBSQL_IMPL_NOT_AVAILABLE_with_parent_{operation_token}_no_executable_sblr",
        "binary_serialize_phase_expectation": "not_applicable_no_sblr_envelope_to_serialize",
        "verify_phase_expectation": "not_applicable_no_container_to_verify",
        "binary_deserialize_phase_expectation": "not_applicable_no_container_to_deserialize",
        "dispatch_phase_expectation": "not_applicable_no_server_or_engine_dispatch",
        "execute_phase_expectation": "not_applicable_no_catalog_mutation",
        "render_phase_expectation": render_expectation,
        "canonical_container_magic": "not_applicable_pre_sblr_exact_refusal",
        "canonical_container_header_size_bytes": "not_applicable_pre_sblr_exact_refusal",
        "byte_identical_round_trip_required": "not_applicable_pre_sblr_exact_refusal",
        "crc32c_check_required": "not_applicable_pre_sblr_exact_refusal",
        "engine_anchored_uuids_required": "not_applicable_pre_sblr_exact_refusal",
        "execution_authority_model": "parser_syntax_only;no_executable_sblr;no_engine_execution;no_catalog_mutation;no_wal_authority",
        "notes": refusal_notes,
    }


def native_future_phases() -> dict[str, str]:
    return {
        "expected_canonical_function_or_api_operation_id": "not_applicable_status_native_future_lower_refuses_before_envelope",
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass_for_diagnostic_purposes",
        "bind_phase_expectation": "bind_to_bound_ast_pass_for_diagnostic_purposes_only",
        "lower_phase_expectation": "lower_refuses_with_SBSQL_SURFACE_NOT_ADMITTED_no_envelope_emitted_constant_time",
        "binary_serialize_phase_expectation": "not_applicable_no_envelope_to_serialize",
        "verify_phase_expectation": "not_applicable_no_container_to_verify",
        "binary_deserialize_phase_expectation": "not_applicable_no_container_to_deserialize",
        "dispatch_phase_expectation": "not_applicable_no_envelope_to_dispatch",
        "execute_phase_expectation": "not_applicable_refused_at_lowering_no_engine_execution",
        "render_phase_expectation": "renderer_emit_refusal_diagnostic_SBSQL_SURFACE_NOT_ADMITTED_constant_time",
        "byte_identical_round_trip_required": "not_applicable_no_round_trip_until_status_promote",
        "notes": "native_future row: parser and binder may run for diagnostic purposes but lower must refuse at admission boundary with SBSQL.SURFACE.NOT_ADMITTED; no binary envelope is emitted; no engine execution path is reached; refusal is constant-time per parser security threat model; no implementation begins until SBSFC-009B promotes the canonical status.",
    }


def cluster_private_phases() -> dict[str, str]:
    return {
        "expected_canonical_function_or_api_operation_id": "not_applicable_status_cluster_private_public_build_refuses_before_envelope",
        "parse_phase_expectation": "parse_sbsql_text_to_cst_pass_for_diagnostic_purposes",
        "bind_phase_expectation": "bind_to_bound_ast_pass_for_diagnostic_purposes_or_refuse_with_SBSQL_SURFACE_PARSER_PRIVATE_REFUSED",
        "lower_phase_expectation": "lower_refuses_with_SBSQL_SURFACE_NOT_ADMITTED_or_SBSQL_CLUSTER_AUTHORITY_REQUIRED_no_envelope_emitted_in_public_build",
        "binary_serialize_phase_expectation": "not_applicable_no_envelope_in_public_build_private_profile_evidence_lives_outside_public_matrix",
        "verify_phase_expectation": "not_applicable_no_container_in_public_build",
        "binary_deserialize_phase_expectation": "not_applicable_no_container_in_public_build",
        "dispatch_phase_expectation": "not_applicable_no_dispatch_path_in_public_build_cluster_execution_paths_are_private_only",
        "execute_phase_expectation": "not_applicable_no_execution_in_public_build",
        "render_phase_expectation": "renderer_emit_refusal_diagnostic_constant_time_no_cluster_details_leaked",
        "byte_identical_round_trip_required": "not_applicable_no_round_trip_in_public_build",
        "notes": "cluster_private row: public build must fail-closed; no binary envelope is serialized; no cluster execution path is reachable from public dispatch; private-profile round-trip evidence lives outside the public matrix per SBSFC-025.",
    }


def bridge_cluster_exact_refusal_phases(authority_status: str) -> dict[str, str]:
    return {
        "expected_canonical_function_or_api_operation_id": "bridge.cluster_route",
        "parse_phase_expectation": "parse_bridge_cluster_route_to_cst_pass",
        "bind_phase_expectation": "bind_bridge_context_and_transaction_descriptor_pass_no_names_as_authority",
        "lower_phase_expectation": "lower_to_sblr_bridge_operation_v3_bridge_cluster_route_SBLR_BRIDGE_VALIDATE_pass",
        "binary_serialize_phase_expectation": "not_applicable_exact_refusal_test_makes_no_canonical_container_round_trip_claim",
        "verify_phase_expectation": "sblr_verifier_and_canonical_server_admission_pass",
        "binary_deserialize_phase_expectation": "not_applicable_no_binary_container_claim_for_trusted_udr_exact_refusal",
        "dispatch_phase_expectation": "trusted_udr_dispatch_refuses_UDR_BRIDGE_UNSUPPORTED_after_canonical_sblr_admission",
        "execute_phase_expectation": "not_applicable_exact_refusal_before_bridge_or_cluster_execution",
        "render_phase_expectation": "renderer_emits_UDR_BRIDGE_UNSUPPORTED_canonical_message_vector",
        "byte_identical_round_trip_required": "not_applicable_no_round_trip_in_public_build",
        "notes": "Bridge cluster route is a Core refusal-only surface. Parse, bind, lowering, verifier, and canonical server admission preserve bridge.cluster_route/SBLR_BRIDGE_VALIDATE identity; trusted parser-support UDR dispatch then returns UDR.BRIDGE.UNSUPPORTED. No provider-route success, binary round-trip claim, private cluster execution, parser finality, or WAL authority is asserted. Round-trip authority source=" + authority_status + ".",
    }


def canonical_operation_from_manifest(manifest_row: dict[str, str] | None) -> tuple[str, str]:
    if not manifest_row:
        return "", ""
    implementation_refs = manifest_row.get("implementation_refs", "")
    for label, pattern in MANIFEST_OPERATION_PATTERNS:
        match = pattern.search(implementation_refs)
        if not match:
            continue
        operation_id = match.group(1).strip()
        if not operation_id:
            continue
        if "operation_family" in label or operation_id.endswith(".operation.v3"):
            continue
        return operation_id, label
    return "", ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    validate_authoritative_runtime_inputs(root)
    validate_core_root_refusal_inputs(root)
    validate_sbsfc078_refusal_inputs(root)

    surfaces = read_csv(root / REGISTRY_CSV)
    oracle = read_csv(artifact_root / ORACLE_MATRIX_NAME)
    manifest = read_csv(artifact_root / PER_ROW_MANIFEST_NAME)
    oracle_by_id = {row["surface_id"]: row for row in oracle}
    manifest_by_id = {row["surface_id"]: row for row in manifest}

    if not surfaces:
        fail("SBSQL_SURFACE_REGISTRY.csv is empty")

    output_rows: list[dict[str, str]] = []
    status_counts: dict[str, int] = {}
    round_trip_required_counts: dict[str, int] = {}

    for surface in sorted(surfaces, key=lambda r: r["surface_id"]):
        surface_id = surface["surface_id"]
        status = surface.get("source_status") or surface["status"]
        oracle_row = oracle_by_id.get(surface_id)
        oracle_status = oracle_row["oracle_authority_status"] if oracle_row else "not_in_oracle_matrix_non_expression_runtime_surface"
        sblr_binding = oracle_row["sblr_binding"] if oracle_row else ""
        manifest_row = manifest_by_id.get(surface_id)
        manifest_operation_id, manifest_authority = canonical_operation_from_manifest(manifest_row)

        if surface_id == BRIDGE_CLUSTER_ROUTE_SURFACE_ID:
            oracle_status = f"per_row_manifest_{manifest_authority}"
            phases = bridge_cluster_exact_refusal_phases(oracle_status)
        elif surface["cluster_scope"] == "cluster_private":
            phases = cluster_private_phases()
        elif status == "native_now":
            if (surface_id == "SBSQL-EB95D772BD63"
                    and manifest_row and manifest_row.get("final_state") == "e2e_passed"):
                oracle_status = f"per_row_manifest_{manifest_authority}"
                phases = lifecycle_create_manifest_phases(oracle_status)
            elif (manifest_row and manifest_row.get("final_state") == "exact_refusal_passed"
                    and "pre_sblr_refusal=true" in manifest_row.get("implementation_refs", "")):
                oracle_status = f"per_row_manifest_{manifest_authority}"
                implementation_refs = manifest_row.get("implementation_refs", "")
                if "parent_operation_id=engine.op.ddl_create_schema" in implementation_refs:
                    phases = pre_sblr_exact_refusal_manifest_phases(
                        oracle_status,
                        "engine.op.ddl_create_schema",
                        "SBLR_DDL_CREATE_SCHEMA",
                        "CREATE SCHEMA CSDX/CSDO",
                    )
                else:
                    phases = pre_sblr_exact_refusal_manifest_phases(
                        oracle_status,
                        "engine.op.ddl_create_table",
                        "SBLR_DDL_CREATE_TABLE",
                        "CREATE TABLE CTDX/CTDO constraint-vector",
                    )
            elif (manifest_row and manifest_row.get("final_state") == "exact_refusal_passed"
                    and manifest_operation_id):
                oracle_status = f"per_row_manifest_{manifest_authority}"
                phases = exact_refusal_manifest_phases(manifest_operation_id, oracle_status)
            elif oracle_status == "full_oracle":
                phases = native_now_phases(oracle_status, sblr_binding)
            elif manifest_row and manifest_row.get("final_state") == "e2e_passed" and manifest_operation_id:
                oracle_status = f"per_row_manifest_{manifest_authority}"
                phases = native_now_manifest_phases(manifest_operation_id, oracle_status)
            else:
                phases = native_now_phases(oracle_status, sblr_binding)
        elif status == "native_future":
            phases = native_future_phases()
        elif status == "cluster_private":
            fail(f"{surface_id} has cluster_private status without cluster_private scope")
        else:
            fail(f"{surface_id} has unrecognized status: {status}")
            raise SystemExit(1)

        ledger_row = {
            "surface_id": surface_id,
            "canonical_name": surface["canonical_name"],
            "status": status,
            "cluster_scope": surface["cluster_scope"],
            "surface_kind": surface["surface_kind"],
            "sblr_operation_family": surface["sblr_operation_family"],
            "oracle_authority_status": oracle_status,
            "canonical_container_magic": CANONICAL_CONTAINER_MAGIC,
            "canonical_container_header_size_bytes": CANONICAL_CONTAINER_HEADER_SIZE,
            "crc32c_check_required": "yes",
            "engine_anchored_uuids_required": "yes",
            "forbidden_authority_sources": FORBIDDEN_AUTHORITY,
            "execution_authority_model": EXECUTION_AUTHORITY,
            "fixture_path": f"project/tests/sbsql_parser_worker/generated/full_surface/sblr_binary_round_trip/{surface_id}.round_trip.yaml",
            "fixture_status": "pending_authoring",
        }
        ledger_row.update(phases)
        ledger_row = binary_round_trip_override(ledger_row)
        ledger_row = core_root_refusal_binary_round_trip_override(ledger_row)
        ledger_row = sbsfc078_refusal_binary_round_trip_override(ledger_row)
        ledger_row["fixture_status"] = normalize_fixture_status(
            surface_id,
            fixture_status_for(root, ledger_row["fixture_path"], surface_id),
        )
        ledger_row["fixture_status"] = normalize_core_root_refusal_fixture_status(
            surface_id, ledger_row["fixture_status"]
        )
        ledger_row["fixture_status"] = normalize_sbsfc078_refusal_fixture_status(
            surface_id, ledger_row["fixture_status"]
        )
        output_rows.append(ledger_row)

        status_counts[status] = status_counts.get(status, 0) + 1
        round_trip_required_counts[phases["byte_identical_round_trip_required"]] = (
            round_trip_required_counts.get(phases["byte_identical_round_trip_required"], 0) + 1
        )

    output_path = artifact_root / OUTPUT_NAME
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(output_rows)

    print(
        "sblr_binary_round_trip_matrix=generated "
        f"rows={len(output_rows)} "
        + " ".join(f"{s}={c}" for s, c in sorted(status_counts.items()))
        + " "
        + " ".join(f"round_trip_{k}={v}" for k, v in sorted(round_trip_required_counts.items()))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
