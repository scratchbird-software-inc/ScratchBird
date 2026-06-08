#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate AUTHENTICATED_FULL_ROUTE_MATRIX.csv for the SBsql Surface-to-SBLR execution_plan.

Inputs (repo-local, no network):
  public_input_snapshot

Output:
  project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/AUTHENTICATED_FULL_ROUTE_MATRIX.csv

For every SBsql surface row in the canonical registry, this generator
records the authenticated-route requirements that the row's E2E fixture must
satisfy. The matrix declares credential profile, auth policy, session
profile, transaction profile, transport route, listener path, IPC admission
path, engine admission authority, MGA execution authority, and expected
authorization outcomes for both accepted and refused branches.

Per the execution_plan rule, internal parser/server tests are not sufficient final
route evidence. Final-route fixtures must use engine authentication/
authorization authority over SBWP/TLS through the listener, a pool-allocated
SBsql parser worker, SBPS IPC admission, and the engine internal API.

Outcome per status:

- native_now rows: route must reach `accepted_e2e_passes_with_ExecutionResultEnvelope_v3`
  with a credential that holds the required grant; refused branch uses a
  credential without the grant and expects an engine-emitted security
  diagnostic.
- native_future rows: row is not admitted until canonical status changes;
  both branches refuse with `SBSQL.SURFACE.NOT_ADMITTED`.
- cluster_private rows: public build must fail-closed; both branches refuse
  with `SBSQL.SURFACE.NOT_ADMITTED` in the public route; private-profile
  acceptance evidence lives outside the public matrix.

Architecture invariant compliance: every row's `mga_execution_authority`
column declares MGA copy-on-write as the sole transaction recovery model
("no_wal_authority"). No WAL or redo-log surface is referenced anywhere in
the matrix. The generator itself is read-only over a canonical CSV.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path


REGISTRY_CSV = "public_input_snapshot"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
OUTPUT_NAME = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"


COLUMNS = [
    "surface_id",
    "canonical_name",
    "status",
    "cluster_scope",
    "surface_kind",
    "sblr_operation_family",
    "fixture_path",
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
    "fixture_status",
    "notes",
]


# Architecture-uniform constants applied to every fixture row.
AUTH_POLICY = "engine_owned_authority_revoke_all_default_evidence_before_success_with_provider_policy_admission"
SESSION_PROFILE = "session_uuid_v7;engine_language_profile;identifier_profile_sbsql_v3;security_context_required"
TRANSACTION_PROFILE = "mga_authority_required;isolation_read_committed_default;intent_from_sblr_envelope;visibility_through_transaction_inventory_and_version_chains"
TRANSPORT_ROUTE = "sbwp_1.1_over_tls_1.2_or_1.3;cipher_suites_from_scratchbird_tls_profile;mtls_or_password_auth_per_credential_profile;plaintext_refused"
TLS_PROFILE_REF = "public_contract_snapshot"
LISTENER_PATH = "sb_listener_accept;tls_terminated_or_passed_through;scm_rights_handoff_to_pool_allocated_sbp_sbsql_worker;parser_pool_quarantine_on_failure"
IPC_ADMISSION_PATH = "sbps_frame_handshake_kFrameMagic_0x53504253;sblr_admission;sblr_dispatch_server;parser_server_event_ipc_round_trip"
ENGINE_ADMISSION_AUTHORITY = "engine_internal_api_security_authority_api;revoke_all_default;evidence_before_success;catalog_authority_through_sblr_envelope_descriptors_and_uuids"
MGA_EXECUTION_AUTHORITY = "mga_copy_on_write;no_wal_authority;transaction_inventory_and_version_chains;dirty_object_manifest_acceleration_only;page_level_durability_before_visibility"
FIXTURE_KIND = "authenticated_route"
ALLOWED_AUTHORED_FIXTURE_STATUSES = {"fixture_authored", "e2e_passed"}


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


def classify(surface: dict[str, str]) -> dict[str, str]:
    surface_id = surface["surface_id"]
    status = surface["status"]
    cluster_scope = surface["cluster_scope"]

    fixture_path = f"project/tests/sbsql_parser_worker/generated/full_surface/authenticated_route/{surface_id}.route.yaml"

    if cluster_scope == "cluster_private":
        return {
            "fixture_path": fixture_path,
            "credential_profile_accepted": "not_applicable_cluster_private_public_build_fail_closed_only",
            "credential_profile_refused": "sbsql_test_user_any_valid_credential_refusal_is_status_driven_in_public_build",
            "auth_policy": AUTH_POLICY,
            "session_profile": SESSION_PROFILE,
            "transaction_profile": TRANSACTION_PROFILE,
            "transport_route": TRANSPORT_ROUTE,
            "tls_profile_ref": TLS_PROFILE_REF,
            "listener_path": LISTENER_PATH,
            "ipc_admission_path": IPC_ADMISSION_PATH,
            "engine_admission_authority": ENGINE_ADMISSION_AUTHORITY,
            "mga_execution_authority": MGA_EXECUTION_AUTHORITY,
            "expected_authorization_accepted_outcome": "not_applicable_cluster_private_public_build_acceptance_evidence_lives_in_private_profile_only",
            "expected_authorization_refused_outcome": "refused_with_SBSQL_SURFACE_NOT_ADMITTED_at_parser_or_lowering_admission_no_cluster_execution_path_in_public_build",
            "expected_diagnostic_codes": "SBSQL.SURFACE.NOT_ADMITTED;SBSQL.SURFACE.PARSER_PRIVATE_REFUSED;SBSQL.CLUSTER.AUTHORITY_REQUIRED",
            "fixture_status": "pending_authoring",
            "notes": "cluster_scope=cluster_private row: public build must fail-closed regardless of canonical source status; fixture must prove that the authenticated route refuses at parser/lowering admission without entering any cluster execution path; private-profile acceptance evidence, where available, lives outside the public matrix per SBSFC-025.",
        }

    if status == "native_now":
        return {
            "fixture_path": fixture_path,
            "credential_profile_accepted": "sbsql_test_user_with_required_grant_for_surface",
            "credential_profile_refused": "sbsql_test_user_with_revoked_or_missing_required_grant_for_surface",
            "auth_policy": AUTH_POLICY,
            "session_profile": SESSION_PROFILE,
            "transaction_profile": TRANSACTION_PROFILE,
            "transport_route": TRANSPORT_ROUTE,
            "tls_profile_ref": TLS_PROFILE_REF,
            "listener_path": LISTENER_PATH,
            "ipc_admission_path": IPC_ADMISSION_PATH,
            "engine_admission_authority": ENGINE_ADMISSION_AUTHORITY,
            "mga_execution_authority": MGA_EXECUTION_AUTHORITY,
            "expected_authorization_accepted_outcome": "accepted_e2e_passes_with_ExecutionResultEnvelope_v3_and_message_vector_set",
            "expected_authorization_refused_outcome": "refused_with_security_diagnostic_SECURITY_AUTHORIZATION_FORBIDDEN_evidence_before_success",
            "expected_diagnostic_codes": "SECURITY.AUTHORIZATION.FORBIDDEN;SECURITY.AUTHENTICATION.FAILED;SBSQL.BINDING.*;SBLR.ENVELOPE.*;SBLR.OPCODE.*",
            "fixture_status": "pending_authoring",
            "notes": "native_now row: fixture must prove both accepted route (with required grant) and refused route (without grant) end-to-end through sb_isql/driver -> SBWP/TLS -> sb_listener -> SBsql parser pool -> SBPS -> sb_server engine; engine emits ExecutionResultEnvelope.v3 on accept and security diagnostic on refuse; MGA copy-on-write is the only transaction authority; no WAL surface may be introduced.",
        }

    if status == "native_future":
        return {
            "fixture_path": fixture_path,
            "credential_profile_accepted": "not_applicable_row_not_admitted_until_promote_via_sbsfc_009b",
            "credential_profile_refused": "sbsql_test_user_any_valid_credential_refusal_is_status_driven_not_credential_driven",
            "auth_policy": AUTH_POLICY,
            "session_profile": SESSION_PROFILE,
            "transaction_profile": TRANSACTION_PROFILE,
            "transport_route": TRANSPORT_ROUTE,
            "tls_profile_ref": TLS_PROFILE_REF,
            "listener_path": LISTENER_PATH,
            "ipc_admission_path": IPC_ADMISSION_PATH,
            "engine_admission_authority": ENGINE_ADMISSION_AUTHORITY,
            "mga_execution_authority": MGA_EXECUTION_AUTHORITY,
            "expected_authorization_accepted_outcome": "not_applicable_native_future_until_status_promote_to_native_now",
            "expected_authorization_refused_outcome": "refused_with_SBSQL_SURFACE_NOT_ADMITTED_at_parser_or_lowering_admission",
            "expected_diagnostic_codes": "SBSQL.SURFACE.NOT_ADMITTED;SBSQL.SURFACE.NATIVE_FUTURE",
            "fixture_status": "pending_authoring",
            "notes": "native_future row: refusal is status-driven; fixture must prove that any authenticated route attempting to use the surface fails at parser/lowering admission with SBSQL.SURFACE.NOT_ADMITTED before reaching engine execution; refusal must be constant-time per parser security threat model; no implementation work begins until SBSFC-009B promotes status.",
        }

    if status == "cluster_private":
        fail(f"{surface_id} has cluster_private status without cluster_private scope")

    fail(f"{surface_id} has unrecognized status: {status}")
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    surfaces = read_csv(root / REGISTRY_CSV)
    if not surfaces:
        fail("SBSQL_SURFACE_REGISTRY.csv is empty")

    output_rows: list[dict[str, str]] = []
    status_counts: dict[str, int] = {}
    accepted_counts: dict[str, int] = {}

    for surface in sorted(surfaces, key=lambda r: r["surface_id"]):
        classification = classify(surface)
        ledger_row = {
            "surface_id": surface["surface_id"],
            "canonical_name": surface["canonical_name"],
            "status": surface["status"],
            "cluster_scope": surface["cluster_scope"],
            "surface_kind": surface["surface_kind"],
            "sblr_operation_family": surface["sblr_operation_family"],
        }
        ledger_row.update(classification)
        ledger_row["fixture_status"] = fixture_status_for(root, ledger_row["fixture_path"], surface["surface_id"])
        output_rows.append(ledger_row)

        status_counts[surface["status"]] = status_counts.get(surface["status"], 0) + 1
        outcome = (
            "accepted_route_required"
            if surface["status"] == "native_now" and surface["cluster_scope"] != "cluster_private"
            else "fail_closed_only"
        )
        accepted_counts[outcome] = accepted_counts.get(outcome, 0) + 1

    output_path = artifact_root / OUTPUT_NAME
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(output_rows)

    print(
        "authenticated_full_route_matrix=generated "
        f"rows={len(output_rows)} "
        + " ".join(f"{s}={c}" for s, c in sorted(status_counts.items()))
        + " "
        + " ".join(f"{o}={c}" for o, c in sorted(accepted_counts.items()))
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
