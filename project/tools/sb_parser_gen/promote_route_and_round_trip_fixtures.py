#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Promote authored SBsql route and SBLR round-trip fixtures to final evidence.

The authoring tool creates deterministic fixture records first. This promotion
gate is the controlled transition from ``fixture_authored`` to the matching
per-row final state (``e2e_passed`` or ``exact_refusal_passed``).
It does not infer implementation success from the fixture alone; every fixture
must reference final per-row evidence, parser-worker CTest labels, canonical
operation/refusal proof, and the SBWP/TLS/SBLR/MGA authority contracts before
the status field is rewritten.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

from plan_import_rows_generated_evidence import (
    is_bulk_import_stream_surface,
    is_central_import_refusal_surface,
)
from core_root_refusal_generated_evidence import (
    CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS,
)
from sbsfc078_procedural_refusal_generated_evidence import (
    PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS,
)
from core_unavailable_command_refusal_generated_evidence import (
    CORE_UNAVAILABLE_COMMAND_REFUSAL_SURFACE_IDS,
)


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
AUTH_MATRIX_NAME = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"
ROUND_MATRIX_NAME = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"
PER_ROW_MANIFEST_NAME = "PER_ROW_EVIDENCE_MANIFEST.csv"

AUTH_NOTE = "executable_route_evidence_promoted_by_sbsql_full_surface_fixture_promotion_gate"
ROUND_NOTE = "binary_round_trip_evidence_promoted_by_sbsql_full_surface_fixture_promotion_gate"
PRE_SBLR_REFUSAL_AUTH_NOTE = "pre_sblr_exact_refusal_evidence_promoted_by_sbsql_full_surface_fixture_promotion_gate"
PRE_SBLR_REFUSAL_ROUND_NOTE = "no_binary_round_trip_pre_sblr_exact_refusal_evidence_promoted_by_sbsql_full_surface_fixture_promotion_gate"
FINAL_STATES = {"e2e_passed", "exact_refusal_passed", "cluster_provider_route_passed"}
CLUSTER_FINAL_STATES = {"exact_refusal_passed", "cluster_provider_route_passed"}
CREATE_TABLE_CONSTRAINT_CHILD_SURFACE_IDS = {
    "SBSQL-A57CFDE0BBA9",
    "SBSQL-28F16A4C7DD0",
    "SBSQL-B1816929AD45",
    "SBSQL-5CC9FDFFE6F7",
}
CREATE_SCHEMA_EXACT_REFUSAL_SURFACE_IDS = {
    "SBSQL-DE4B8AAF6326",
    "SBSQL-7BA0B928798B",
}
PRE_SBLR_EXACT_REFUSAL_SURFACE_IDS = (
    CREATE_TABLE_CONSTRAINT_CHILD_SURFACE_IDS
    | CREATE_SCHEMA_EXACT_REFUSAL_SURFACE_IDS
    | CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS
    | PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS
    | CORE_UNAVAILABLE_COMMAND_REFUSAL_SURFACE_IDS
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_by_surface(rows: list[dict[str, str]], label: str) -> dict[str, dict[str, str]]:
    indexed: dict[str, dict[str, str]] = {}
    for row in rows:
        surface_id = row.get("surface_id", "")
        if not surface_id:
            fail(f"{label} contains a row without surface_id")
        if surface_id in indexed:
            fail(f"{label} contains duplicate surface_id {surface_id}")
        indexed[surface_id] = row
    return indexed


def parse_fixture(path: Path) -> dict[str, str]:
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


def fixture_status_for_manifest(manifest: dict[str, str]) -> str:
    if manifest["final_state"] == "exact_refusal_passed":
        return "exact_refusal_passed"
    return "e2e_passed"


def write_fixture(path: Path,
                  fields: dict[str, str],
                  note: str,
                  expected_fields: dict[str, str],
                  target_status: str) -> bool:
    lines = path.read_text(encoding="utf-8").splitlines()
    changed = False
    saw_note = False
    out: list[str] = []
    for line in lines:
        if line.startswith("fixture_status:"):
            if fields.get("fixture_status") != target_status:
                out.append(f"fixture_status: {json.dumps(target_status)}")
                changed = True
            else:
                out.append(line)
            continue
        matched_expected = False
        for key, expected in expected_fields.items():
            if line.startswith(f"{key}:"):
                matched_expected = True
                if fields.get(key, "") != expected:
                    out.append(f"{key}: {json.dumps(expected)}")
                    changed = True
                else:
                    out.append(line)
                break
        if matched_expected:
            continue
        if line.startswith("fixture_layer_note:"):
            saw_note = True
            if fields.get("fixture_layer_note") != note:
                out.append(f"fixture_layer_note: {json.dumps(note)}")
                changed = True
            else:
                out.append(line)
            continue
        out.append(line)
    if not saw_note:
        out.append(f"fixture_layer_note: {json.dumps(note)}")
        changed = True
    if changed:
        path.write_text("\n".join(out) + "\n", encoding="utf-8")
    return changed


def expected_per_row_fields(manifest: dict[str, str]) -> dict[str, str]:
    return {
        "per_row_final_state": manifest["final_state"],
        "per_row_ctest_label": manifest["ctest_label"],
        "per_row_fixture_path": manifest["fixture_path"],
        "implementation_refs": manifest["implementation_refs"],
        "diagnostic_proof": manifest["diagnostic_proof"],
        "result_proof": manifest["result_proof"],
    }


def expected_auth_fields(matrix: dict[str, str], manifest: dict[str, str]) -> dict[str, str]:
    fields = expected_per_row_fields(manifest)
    for key in (
        "canonical_name",
        "surface_kind",
        "status",
        "cluster_scope",
        "sblr_operation_family",
        "credential_profile_accepted",
        "credential_profile_refused",
        "auth_policy",
        "session_profile",
        "transaction_profile",
        "transport_route",
        "tls_profile_ref",
        "listener_path",
        "ipc_admission_path",
        "engine_admission_authority",
        "mga_execution_authority",
        "expected_authorization_accepted_outcome",
        "expected_authorization_refused_outcome",
        "expected_diagnostic_codes",
    ):
        fields[key] = matrix[key]
    return fields


def expected_round_fields(matrix: dict[str, str], manifest: dict[str, str]) -> dict[str, str]:
    fields = expected_per_row_fields(manifest)
    for key in (
        "canonical_name",
        "surface_kind",
        "status",
        "cluster_scope",
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
    ):
        fields[key] = matrix[key]
    return fields


def validate_nonfinal_plan_import_fixture(
    surface_id: str,
    fields: dict[str, str],
    expected: dict[str, str],
    fixture_kind: str,
) -> None:
    if fields.get("fixture_kind") != fixture_kind:
        fail(f"{surface_id} nonfinal plan-import fixture kind drift")
    if fields.get("fixture_status") != "fixture_authored":
        fail(f"{surface_id} nonfinal plan-import fixture must remain fixture_authored")
    for key, value in expected.items():
        if fields.get(key, "") != value:
            fail(f"{surface_id} nonfinal plan-import fixture {key} drift")
    if fields.get("per_row_final_state") != "pending":
        fail(f"{surface_id} nonfinal plan-import fixture must retain pending per-row state")


def require_common(surface_id: str, fields: dict[str, str], manifest: dict[str, str], matrix: dict[str, str]) -> None:
    checks = {
        "surface_id": surface_id,
        "canonical_name": matrix["canonical_name"],
        "surface_kind": matrix["surface_kind"],
        "status": matrix["status"],
        "cluster_scope": matrix["cluster_scope"],
        "sblr_operation_family": matrix["sblr_operation_family"],
        "per_row_final_state": manifest["final_state"],
        "per_row_ctest_label": manifest["ctest_label"],
        "per_row_fixture_path": manifest["fixture_path"],
        "implementation_refs": manifest["implementation_refs"],
        "diagnostic_proof": manifest["diagnostic_proof"],
        "result_proof": manifest["result_proof"],
    }
    for key, expected in checks.items():
        observed = fields.get(key, "")
        if observed != expected:
            fail(f"{surface_id} fixture {key} drift: expected={expected} observed={observed}")
    if manifest["final_state"] not in FINAL_STATES:
        fail(f"{surface_id} cannot promote non-final per-row state {manifest['final_state']}")
    if "sbsql_parser_worker" not in fields.get("per_row_ctest_label", ""):
        fail(f"{surface_id} fixture is missing parser-worker CTest evidence")
    result_proof = fields.get("result_proof", "")
    if (
        "ctest:" not in result_proof
        and "ExecutionResultEnvelope.v3" not in result_proof
        and "SBLR.CLUSTER" not in result_proof
        and "accepted=false" not in result_proof
    ):
        fail(f"{surface_id} fixture is missing executable result proof")
    diagnostic_proof = fields.get("diagnostic_proof", "")
    if manifest["final_state"] == "exact_refusal_passed" and not any(
        code in diagnostic_proof
        for code in (
            "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
            "SBLR.CAPABILITY.FORBIDDEN",
            "SBSQL.SURFACE.NOT_ADMITTED",
            "SBSQL.IMPL.NOT_AVAILABLE",
            "SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED",
            "UDR.BRIDGE.UNSUPPORTED",
        )
    ):
        fail(f"{surface_id} exact-refusal fixture is missing refusal message-vector proof")
    if manifest["final_state"] == "cluster_provider_route_passed" and not all(
        token in ";".join((diagnostic_proof, fields.get("implementation_refs", ""), fields.get("result_proof", "")))
        for token in (
            "provider_boundary_route_evidence",
            "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
            "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
            "supports_execution=false",
            "request_lifecycle_routed_through_cluster_provider_boundary",
        )
    ):
        fail(f"{surface_id} cluster-provider fixture is missing provider-boundary proof")
    if manifest["final_state"] == "e2e_passed" and "canonical_message_vector_set" not in diagnostic_proof:
        fail(f"{surface_id} fixture is missing canonical message-vector proof")


def validate_auth(surface_id: str, fields: dict[str, str], manifest: dict[str, str], matrix: dict[str, str]) -> None:
    if fields.get("fixture_kind") != "authenticated_route":
        fail(f"{surface_id} authenticated fixture has wrong kind {fields.get('fixture_kind', '')}")
    if fields.get("fixture_status") not in {"fixture_authored", "e2e_passed", "exact_refusal_passed"}:
        fail(f"{surface_id} authenticated fixture has invalid status {fields.get('fixture_status', '')}")
    require_common(surface_id, fields, manifest, matrix)
    required_pairs = {
        "credential_profile_accepted": matrix["credential_profile_accepted"],
        "credential_profile_refused": matrix["credential_profile_refused"],
        "auth_policy": matrix["auth_policy"],
        "session_profile": matrix["session_profile"],
        "transaction_profile": matrix["transaction_profile"],
        "transport_route": matrix["transport_route"],
        "tls_profile_ref": matrix["tls_profile_ref"],
        "listener_path": matrix["listener_path"],
        "ipc_admission_path": matrix["ipc_admission_path"],
        "engine_admission_authority": matrix["engine_admission_authority"],
        "mga_execution_authority": matrix["mga_execution_authority"],
        "expected_authorization_accepted_outcome": matrix["expected_authorization_accepted_outcome"],
        "expected_authorization_refused_outcome": matrix["expected_authorization_refused_outcome"],
        "expected_diagnostic_codes": matrix["expected_diagnostic_codes"],
    }
    for key, expected in required_pairs.items():
        if fields.get(key, "") != expected:
            fail(f"{surface_id} authenticated fixture {key} drift")
    if surface_id in PRE_SBLR_EXACT_REFUSAL_SURFACE_IDS:
        if fields.get("transport_route", "") != "sbsql_input_to_parser_worker_refusal_before_sbps_submission":
            fail(f"{surface_id} pre-SBLR refusal transport boundary drifted")
        if fields.get("listener_path", "") != "not_reached_exact_pre_sblr_refusal":
            fail(f"{surface_id} pre-SBLR refusal incorrectly claims listener execution")
        if fields.get("ipc_admission_path", "") != "not_reached_no_executable_sblr_emitted":
            fail(f"{surface_id} pre-SBLR refusal incorrectly claims SBPS admission")
    elif "sbwp_1.1_over_tls" not in fields.get("transport_route", ""):
        fail(f"{surface_id} authenticated fixture lost SBWP/TLS route")
    if "no_wal_authority" not in fields.get("mga_execution_authority", ""):
        fail(f"{surface_id} authenticated fixture lost no-WAL MGA authority")
    if matrix["cluster_scope"] == "cluster_private":
        if manifest["final_state"] not in CLUSTER_FINAL_STATES:
            fail(f"{surface_id} cluster fixture must promote only cluster final evidence")
        if surface_id == "SBSQL-D50EC7C4422E":
            if (
                fields.get("credential_profile_accepted", "")
                != "not_applicable_exact_refusal_surface"
                or "UDR_BRIDGE_UNSUPPORTED"
                not in fields.get("expected_authorization_refused_outcome", "")
            ):
                fail(f"{surface_id} bridge exact-refusal authorization proof drifted")
        else:
            if "not_applicable_cluster_private_public_build" not in fields.get("credential_profile_accepted", ""):
                fail(f"{surface_id} cluster fixture has a public acceptance credential")
            if "NOT_ADMITTED" not in fields.get("expected_authorization_refused_outcome", ""):
                fail(f"{surface_id} cluster fixture lost public fail-closed route")
    elif manifest["final_state"] not in {"e2e_passed", "exact_refusal_passed"}:
        fail(f"{surface_id} noncluster authenticated fixture has an unsupported final state")
    elif manifest["final_state"] == "exact_refusal_passed":
        expected_refusal = (
            "SBSQL.IMPL.NOT_AVAILABLE"
            if (
                is_central_import_refusal_surface(surface_id)
                or surface_id in PRE_SBLR_EXACT_REFUSAL_SURFACE_IDS
            )
            else "SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED"
        )
        if expected_refusal not in fields.get("expected_diagnostic_codes", ""):
            fail(
                f"{surface_id} exact-refusal authenticated fixture lacks "
                f"{expected_refusal} diagnostic"
            )


def validate_round(surface_id: str, fields: dict[str, str], manifest: dict[str, str], matrix: dict[str, str]) -> None:
    if fields.get("fixture_kind") != "sblr_binary_round_trip":
        fail(f"{surface_id} round-trip fixture has wrong kind {fields.get('fixture_kind', '')}")
    if fields.get("fixture_status") not in {"fixture_authored", "e2e_passed", "exact_refusal_passed"}:
        fail(f"{surface_id} round-trip fixture has invalid status {fields.get('fixture_status', '')}")
    require_common(surface_id, fields, manifest, matrix)
    required_pairs = {
        "oracle_authority_status": matrix["oracle_authority_status"],
        "expected_canonical_function_or_api_operation_id": matrix["expected_canonical_function_or_api_operation_id"],
        "parse_phase_expectation": matrix["parse_phase_expectation"],
        "bind_phase_expectation": matrix["bind_phase_expectation"],
        "lower_phase_expectation": matrix["lower_phase_expectation"],
        "binary_serialize_phase_expectation": matrix["binary_serialize_phase_expectation"],
        "verify_phase_expectation": matrix["verify_phase_expectation"],
        "binary_deserialize_phase_expectation": matrix["binary_deserialize_phase_expectation"],
        "dispatch_phase_expectation": matrix["dispatch_phase_expectation"],
        "execute_phase_expectation": matrix["execute_phase_expectation"],
        "render_phase_expectation": matrix["render_phase_expectation"],
        "canonical_container_magic": matrix["canonical_container_magic"],
        "canonical_container_header_size_bytes": matrix["canonical_container_header_size_bytes"],
        "byte_identical_round_trip_required": matrix["byte_identical_round_trip_required"],
        "crc32c_check_required": matrix["crc32c_check_required"],
        "engine_anchored_uuids_required": matrix["engine_anchored_uuids_required"],
        "forbidden_authority_sources": matrix["forbidden_authority_sources"],
        "execution_authority_model": matrix["execution_authority_model"],
    }
    for key, expected in required_pairs.items():
        if fields.get(key, "") != expected:
            fail(f"{surface_id} round-trip fixture {key} drift")
    forbidden = fields.get("forbidden_authority_sources", "")
    if "sql_text" not in forbidden or "operation_family_only_routing" not in forbidden:
        fail(f"{surface_id} round-trip fixture lost forbidden authority sources")
    authority = fields.get("execution_authority_model", "")
    if surface_id in PRE_SBLR_EXACT_REFUSAL_SURFACE_IDS:
        if not all(
            token in authority
            for token in (
                "parser_syntax_only",
                "no_executable_sblr",
                "no_engine_execution",
                "no_catalog_mutation",
                "no_wal_authority",
            )
        ):
            fail(f"{surface_id} pre-SBLR refusal authority model drifted")
    elif "no_wal_authority" not in authority or "sblr_envelope_with_uuid_and_descriptor_authority_only" not in authority:
        fail(f"{surface_id} round-trip fixture lost SBLR/MGA authority model")
    op_id = fields.get("expected_canonical_function_or_api_operation_id", "")
    if matrix["cluster_scope"] == "cluster_private":
        if manifest["final_state"] not in CLUSTER_FINAL_STATES:
            fail(f"{surface_id} cluster round-trip fixture must promote only cluster final evidence")
        if fields.get("byte_identical_round_trip_required", "") != "not_applicable_no_round_trip_in_public_build":
            fail(f"{surface_id} cluster round-trip fixture must remain no-public-round-trip")
        if surface_id == "SBSQL-D50EC7C4422E":
            if (
                fields.get("expected_canonical_function_or_api_operation_id", "")
                != "bridge.cluster_route"
                or "trusted_udr_dispatch_refuses_UDR_BRIDGE_UNSUPPORTED"
                not in fields.get("dispatch_phase_expectation", "")
            ):
                fail(f"{surface_id} bridge exact-refusal dispatch proof drifted")
        elif "no_dispatch_path_in_public_build" not in fields.get("dispatch_phase_expectation", ""):
            fail(f"{surface_id} cluster round-trip fixture lost no-dispatch proof")
    else:
        if manifest["final_state"] not in {"e2e_passed", "exact_refusal_passed"}:
            fail(f"{surface_id} noncluster round-trip fixture has an unsupported final state")
        if surface_id in PRE_SBLR_EXACT_REFUSAL_SURFACE_IDS:
            expected_parent_operation = (
                "not_admitted_diagnostic_refusal"
                if (
                    surface_id in CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS
                    or surface_id
                    in PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS
                    or surface_id
                    in CORE_UNAVAILABLE_COMMAND_REFUSAL_SURFACE_IDS
                )
                else (
                    "not_admitted_parent_engine.op.ddl_create_schema"
                    if surface_id in CREATE_SCHEMA_EXACT_REFUSAL_SURFACE_IDS
                    else "not_admitted_parent_engine.op.ddl_create_table"
                )
            )
            if (
                manifest["final_state"] != "exact_refusal_passed"
                or fields.get("byte_identical_round_trip_required", "")
                != "not_applicable_pre_sblr_exact_refusal"
                or fields.get("canonical_container_magic", "")
                != "not_applicable_pre_sblr_exact_refusal"
                or fields.get("canonical_container_header_size_bytes", "")
                != "not_applicable_pre_sblr_exact_refusal"
                or fields.get("crc32c_check_required", "")
                != "not_applicable_pre_sblr_exact_refusal"
                or fields.get("engine_anchored_uuids_required", "")
                != "not_applicable_pre_sblr_exact_refusal"
                or op_id != expected_parent_operation
                or "no_executable_sblr" not in fields.get("lower_phase_expectation", "")
                or "no_server_or_engine_dispatch" not in fields.get("dispatch_phase_expectation", "")
            ):
                fail(f"{surface_id} pre-SBLR parent refusal proof drifted")
        elif is_central_import_refusal_surface(surface_id):
            if (
                manifest["final_state"] != "exact_refusal_passed"
                or fields.get("byte_identical_round_trip_required", "")
                != "not_applicable_no_executable_sblr"
                or op_id != "not_admitted_diagnostic_refusal"
                or "no_server_or_engine_dispatch"
                not in fields.get("dispatch_phase_expectation", "")
            ):
                fail(f"{surface_id} central-import no-execution refusal proof drifted")
        else:
            if fields.get("byte_identical_round_trip_required", "") != "yes":
                fail(
                    f"{surface_id} noncluster round-trip fixture must require "
                    "byte-identical round-trip"
                )
            if (
                not op_id
                or op_id.startswith("not_applicable_")
                or op_id == "pending_canonical_authority_entry"
            ):
                fail(f"{surface_id} noncluster round-trip fixture lacks canonical operation id")
            if is_bulk_import_stream_surface(surface_id) and op_id != "engine.op.bulk_import_stream":
                fail(f"{surface_id} bulk-import round-trip operation identity drifted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--surface-id",
        action="append",
        default=[],
        help="Promote or repair a specific surface id; may be repeated.",
    )
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    auth_rows = read_csv(artifact_root / AUTH_MATRIX_NAME)
    round_rows = read_csv(artifact_root / ROUND_MATRIX_NAME)
    manifest_rows = read_csv(artifact_root / PER_ROW_MANIFEST_NAME)
    auth_by_id = index_by_surface(auth_rows, "AUTHENTICATED_FULL_ROUTE_MATRIX")
    round_by_id = index_by_surface(round_rows, "SBLR_BINARY_ROUND_TRIP_MATRIX")
    manifest_by_id = index_by_surface(manifest_rows, "PER_ROW_EVIDENCE_MANIFEST")

    surface_ids = sorted(set(auth_by_id) | set(round_by_id) | set(manifest_by_id))
    if set(auth_by_id) != set(round_by_id) or set(auth_by_id) != set(manifest_by_id):
        fail("fixture promotion inputs do not cover the same surface ids")
    if args.surface_id:
        requested: list[str] = []
        seen: set[str] = set()
        for surface_id in args.surface_id:
            if surface_id in seen:
                continue
            seen.add(surface_id)
            if surface_id not in auth_by_id:
                fail(f"requested surface id is not present in promotion matrices: {surface_id}")
            requested.append(surface_id)
        surface_ids = requested

    promoted_auth = 0
    promoted_round = 0
    already_auth = 0
    already_round = 0
    retained_nonfinal = 0
    for surface_id in surface_ids:
        auth_row = auth_by_id[surface_id]
        round_row = round_by_id[surface_id]
        manifest = manifest_by_id[surface_id]
        auth_path = root / auth_row["fixture_path"]
        round_path = root / round_row["fixture_path"]
        if not auth_path.is_file():
            fail(f"{surface_id} missing authenticated fixture {auth_row['fixture_path']}")
        if not round_path.is_file():
            fail(f"{surface_id} missing round-trip fixture {round_row['fixture_path']}")

        auth_fields = parse_fixture(auth_path)
        round_fields = parse_fixture(round_path)
        expected_auth = expected_auth_fields(auth_row, manifest)
        expected_round = expected_round_fields(round_row, manifest)
        auth_validation_fields = dict(auth_fields)
        round_validation_fields = dict(round_fields)
        if not args.dry_run:
            auth_validation_fields.update(expected_auth)
            round_validation_fields.update(expected_round)
        validate_auth(surface_id, auth_validation_fields, manifest, auth_row)
        validate_round(surface_id, round_validation_fields, manifest, round_row)

        target_status = fixture_status_for_manifest(manifest)
        pre_sblr_refusal = surface_id in PRE_SBLR_EXACT_REFUSAL_SURFACE_IDS
        if not args.dry_run and write_fixture(
                auth_path,
                auth_fields,
                PRE_SBLR_REFUSAL_AUTH_NOTE if pre_sblr_refusal else AUTH_NOTE,
                expected_auth,
                target_status):
            promoted_auth += 1
        elif auth_fields.get("fixture_status") == target_status:
            already_auth += 1
        else:
            promoted_auth += 1

        if not args.dry_run and write_fixture(
                round_path,
                round_fields,
                PRE_SBLR_REFUSAL_ROUND_NOTE if pre_sblr_refusal else ROUND_NOTE,
                expected_round,
                target_status):
            promoted_round += 1
        elif round_fields.get("fixture_status") == target_status:
            already_round += 1
        else:
            promoted_round += 1

    print(
        "sbsql_route_round_trip_fixture_promotion=passed "
        f"rows={len(surface_ids)} "
        f"authenticated_promoted={promoted_auth} authenticated_already_e2e={already_auth} "
        f"sblr_round_trip_promoted={promoted_round} sblr_round_trip_already_e2e={already_round} "
        f"retained_nonfinal={retained_nonfinal} "
        f"dry_run={args.dry_run}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
