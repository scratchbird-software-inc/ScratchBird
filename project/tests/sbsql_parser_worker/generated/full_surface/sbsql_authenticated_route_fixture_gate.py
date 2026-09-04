#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate authored SBsql authenticated-route fixtures against the matrix."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
MATRIX_NAME = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"
FIXTURE_DIR = "project/tests/sbsql_parser_worker/generated/full_surface/authenticated_route"
FIXTURE_KIND = "authenticated_route"
ALLOWED_STATUSES = {
    "pending_authoring",
    "fixture_authored",
    "e2e_passed",
    "exact_refusal_passed",
}
BRIDGE_EXACT_REFUSAL_SURFACE_ID = "SBSQL-D50EC7C4422E"
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
REQUIRED_KEYS = [
    "fixture_kind",
    "fixture_status",
    "surface_id",
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
    "per_row_final_state",
    "per_row_ctest_label",
    "per_row_fixture_path",
    "implementation_refs",
    "diagnostic_proof",
    "result_proof",
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    generator_root = root / "project/tools/sb_parser_gen"
    sys.path.insert(0, str(generator_root))
    from plan_import_rows_generated_evidence import (  # pylint: disable=import-outside-toplevel
        is_central_import_refusal_surface,
    )
    from core_root_refusal_generated_evidence import (  # pylint: disable=import-outside-toplevel
        CORE_ROOT_EXACT_REFUSAL_SURFACE_IDS,
        is_core_root_exact_refusal,
        validate_authoritative_runtime_inputs as validate_core_root_refusals,
    )
    from sbsfc078_procedural_refusal_generated_evidence import (  # pylint: disable=import-outside-toplevel
        SBSFC078_PROCEDURAL_STANDALONE_REFUSAL_SURFACE_IDS,
        is_sbsfc078_procedural_standalone_refusal,
        validate_authoritative_runtime_inputs as validate_sbsfc078_refusals,
    )

    try:
        validate_core_root_refusals(root)
        validate_sbsfc078_refusals(root)
    except ValueError as exc:
        fail(f"Core root exact-refusal authority validation failed: {exc}")

    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    rows = read_csv(artifact_root / MATRIX_NAME)
    matrix_by_path = {row["fixture_path"]: row for row in rows}
    errors: list[str] = []
    counts: Counter[str] = Counter()

    for row in rows:
        status = row.get("fixture_status", "")
        surface_id = row.get("surface_id", "")
        fixture_path = root / row["fixture_path"]
        counts[status] += 1
        if status not in ALLOWED_STATUSES:
            errors.append(f"{surface_id} invalid fixture_status={status}")
            continue
        if status == "pending_authoring":
            if fixture_path.exists():
                errors.append(f"{surface_id} fixture file exists but matrix still says pending_authoring: {row['fixture_path']}")
            continue
        if not fixture_path.is_file():
            errors.append(f"{surface_id} matrix says {status} but fixture is missing: {row['fixture_path']}")
            continue
        fields = parse_fixture(fixture_path)
        for key in REQUIRED_KEYS:
            if not fields.get(key, ""):
                errors.append(f"{surface_id} fixture missing {key}")
        expected_pairs = {
            "fixture_kind": FIXTURE_KIND,
            "fixture_status": status,
            "surface_id": surface_id,
            "canonical_name": row["canonical_name"],
            "surface_kind": row["surface_kind"],
            "status": row["status"],
            "cluster_scope": row["cluster_scope"],
            "sblr_operation_family": row["sblr_operation_family"],
            "transport_route": row["transport_route"],
            "listener_path": row["listener_path"],
            "ipc_admission_path": row["ipc_admission_path"],
            "expected_authorization_accepted_outcome": row["expected_authorization_accepted_outcome"],
            "expected_authorization_refused_outcome": row["expected_authorization_refused_outcome"],
        }
        for key, expected in expected_pairs.items():
            if fields.get(key, "") != expected:
                errors.append(f"{surface_id} fixture {key} drift: expected={expected} observed={fields.get(key, '')}")
        if "no_wal_authority" not in fields.get("mga_execution_authority", ""):
            errors.append(f"{surface_id} fixture lost no_wal_authority")
        if "sbsql_parser_worker" not in fields.get("per_row_ctest_label", ""):
            errors.append(f"{surface_id} fixture missing parser-worker CTest evidence")
        if status == "exact_refusal_passed":
            evidence = "\n".join(
                [f"{key}={value}" for key, value in row.items()]
                + [f"{key}={value}" for key, value in fields.items()]
            )
            if fields.get("per_row_final_state") != "exact_refusal_passed":
                errors.append(f"{surface_id} exact-refusal fixture lost per-row final state")
            if surface_id == BRIDGE_EXACT_REFUSAL_SURFACE_ID:
                required = (
                    "canonical_name=bridge_cluster_route_stub",
                    "operation_id=bridge.cluster_route",
                    "opcode=SBLR_BRIDGE_VALIDATE",
                    "canonical_sblr_admission_before_trusted_udr_dispatch",
                    "UDR.BRIDGE.UNSUPPORTED",
                    "accepted=false",
                    "private_cluster_execution=false",
                    "sbsql_exact_refusal_passed",
                )
                forbidden = (
                    "cluster_provider_route_passed",
                    "SBLR.CLUSTER.STUB_RESPONSE",
                    "UDR.BRIDGE.UNLICENSED",
                )
            elif is_central_import_refusal_surface(surface_id):
                required = (
                    "cluster_scope=noncluster_or_profile_scoped",
                    "SBSQL.IMPL.NOT_AVAILABLE",
                    "accepted=false",
                    "executable_sblr_emitted=false",
                    "no_result",
                    "no_authority_publication",
                    "result_published=false",
                    "descriptor_authority_published=false",
                    "transaction_state_transition=false",
                    "catalog_mutation=false",
                    "row_mutation=false",
                    "durable_state_byte_identical=true",
                    "sbsql_exact_refusal_passed",
                )
                forbidden = (
                    "dml.plan_import_rows",
                    "SBLR_DML_PLAN_IMPORT_ROWS",
                    "engine.op.bulk_import_stream",
                    "SBLR_BULK_IMPORT_STREAM",
                    "result_published=true",
                    "catalog_mutation=true",
                    "row_mutation=true",
                )
            elif is_sbsfc078_procedural_standalone_refusal(surface_id):
                required = (
                    "cluster_scope=noncluster_or_profile_scoped",
                    "not_applicable_exact_refusal_before_executable_sblr",
                    "sbsql_input_to_parser_worker_refusal_before_sbps_submission",
                    "operation_id=not_admitted",
                    "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
                    "SBSQL.IMPL.NOT_AVAILABLE",
                    "pre_sblr_refusal=true",
                    "accepted=false",
                    "executable_sblr_emitted=false",
                    "server_admission_reached=false",
                    "engine_dispatch_reached=false",
                    "result_published=false",
                    "transaction_state_transition=false",
                    "catalog_mutation=false",
                    "row_mutation=false",
                    "durable_state_byte_identical=true",
                    "fixture_status=exact_refusal_passed",
                )
                forbidden = (
                    "general.procedural_operation",
                    "SBLR_GENERAL_PROCEDURAL_OPERATION",
                    "server_admission_reached=true",
                    "engine_dispatch_reached=true",
                    "result_published=true",
                    "catalog_mutation=true",
                    "row_mutation=true",
                )
            elif is_core_root_exact_refusal(surface_id):
                required = (
                    "cluster_scope=noncluster_or_profile_scoped",
                    "operation_id=not_admitted",
                    "root_route=diagnostic_refusal",
                    "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
                    "SBSQL.IMPL.NOT_AVAILABLE",
                    "accepted=false",
                    "executable_sblr_emitted=false",
                    "server_admission_reached=false",
                    "engine_dispatch_reached=false",
                    "result_published=false",
                    "descriptor_authority_published=false",
                    "transaction_state_transition=false",
                    "catalog_mutation=false",
                    "row_mutation=false",
                    "durable_state_byte_identical=true",
                    "fixture_status=exact_refusal_passed",
                )
                forbidden = (
                    "result_published=true",
                    "descriptor_authority_published=true",
                    "server_admission_reached=true",
                    "engine_dispatch_reached=true",
                    "catalog_mutation=true",
                    "row_mutation=true",
                )
            elif surface_id in CREATE_TABLE_CONSTRAINT_CHILD_SURFACE_IDS:
                required = (
                    "not_applicable_exact_refusal_before_executable_sblr",
                    "sbsql_input_to_parser_worker_refusal_before_sbps_submission",
                    "SBSQL.IMPL.NOT_AVAILABLE",
                    "parent_operation_id=engine.op.ddl_create_table",
                    "parent_opcode=SBLR_DDL_CREATE_TABLE",
                    "pre_sblr_refusal=true",
                    "executable_sblr_emitted=false",
                    "server_admission_not_reached=true",
                    "engine_dispatch_not_reached=true",
                    "catalog_mutation=false",
                )
                forbidden = (
                    "ddl.constraint.create",
                    "SBLR_DDL_CONSTRAINT_CREATE",
                    "sbwp_1.1_over_tls",
                    "server_admission_not_reached=false",
                    "engine_dispatch_not_reached=false",
                    "catalog_mutation=true",
                )
            elif surface_id in CREATE_SCHEMA_EXACT_REFUSAL_SURFACE_IDS:
                required = (
                    "not_applicable_exact_refusal_before_executable_sblr",
                    "sbsql_input_to_parser_worker_refusal_before_sbps_submission",
                    "SBSQL.IMPL.NOT_AVAILABLE",
                    "parent_operation_id=engine.op.ddl_create_schema",
                    "parent_opcode=SBLR_DDL_CREATE_SCHEMA",
                    "pre_sblr_refusal=true",
                    "executable_sblr_emitted=false",
                    "server_admission_not_reached=true",
                    "engine_dispatch_not_reached=true",
                    "catalog_mutation=false",
                )
                forbidden = (
                    "sbwp_1.1_over_tls",
                    "server_admission_not_reached=false",
                    "engine_dispatch_not_reached=false",
                    "catalog_mutation=true",
                )
            else:
                errors.append(f"{surface_id} is not an admitted exact-refusal route fixture")
                required = ()
                forbidden = ()
            for token in required:
                if token not in evidence:
                    errors.append(f"{surface_id} exact-refusal fixture missing {token}")
            for token in forbidden:
                if token in evidence:
                    errors.append(f"{surface_id} exact-refusal fixture contains forbidden {token}")

    fixture_root = root / FIXTURE_DIR
    if fixture_root.exists():
        for path in fixture_root.glob("*.route.yaml"):
            rel = path.relative_to(root).as_posix()
            if rel not in matrix_by_path:
                errors.append(f"orphan authenticated-route fixture: {rel}")

    authored = (
        counts["fixture_authored"]
        + counts["e2e_passed"]
        + counts["exact_refusal_passed"]
    )
    if authored == 0:
        errors.append("no authenticated-route fixtures have been authored")

    if errors:
        for error in errors[:50]:
            print(error, file=sys.stderr)
        if len(errors) > 50:
            print(f"... {len(errors) - 50} additional errors", file=sys.stderr)
        print("sbsql_authenticated_route_fixture_gate=failed", file=sys.stderr)
        return 1

    print(
        "sbsql_authenticated_route_fixture_gate=passed "
        f"rows={len(rows)} pending_authoring={counts['pending_authoring']} "
        f"fixture_authored={counts['fixture_authored']} e2e_passed={counts['e2e_passed']} "
        f"exact_refusal_passed={counts['exact_refusal_passed']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
