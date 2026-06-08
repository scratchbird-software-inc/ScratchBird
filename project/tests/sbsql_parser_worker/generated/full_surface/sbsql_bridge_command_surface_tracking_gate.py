#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate bridge command integration across SBsql/SBLR proof artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
AUTH_MATRIX = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"
ROUND_MATRIX = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"
LEDGER = "STRICT_ROW_COVERAGE_LEDGER.csv"
MANIFEST = "PER_ROW_EVIDENCE_MANIFEST.csv"
REGISTRY = "public_input_snapshot"
STATUS = "public_input_snapshot"
OP_MATRIX = "public_input_snapshot"
AUTH_DIR = "project/tests/sbsql_parser_worker/generated/full_surface/authenticated_route"
ROUND_DIR = "project/tests/sbsql_parser_worker/generated/full_surface/sblr_binary_round_trip"
BRIDGE_SPEC = "public_contract_snapshot"
AST_SPEC = "public_contract_snapshot"
GRAMMAR_SPEC = "public_contract_snapshot"
CMAKE = "project/tests/sbsql_parser_worker/CMakeLists.txt"


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def load_bridge_module(repo_root: Path):
    tool_dir = repo_root / "project/tools/sb_parser_gen"
    sys.path.insert(0, str(tool_dir))
    from sbsql_bridge_command_surface import BRIDGE_COMMAND_SURFACES  # type: ignore

    return BRIDGE_COMMAND_SURFACES


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_by_surface(label: str, rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        surface_id = row.get("surface_id", "")
        if not surface_id:
            fail(f"{label} row missing surface_id")
        if surface_id in out:
            fail(f"{label} duplicate surface_id {surface_id}")
        out[surface_id] = row
    return out


def parse_fixture(path: Path) -> dict[str, str]:
    if not path.is_file():
        fail(f"required fixture missing: {path}")
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith((" ", "-")):
            continue
        if ":" not in line:
            continue
        key, raw_value = line.split(":", 1)
        value = raw_value.strip()
        if value.startswith('"') and value.endswith('"'):
            value = json.loads(value)
        fields[key.strip()] = value
    return fields


def require_contains(errors: list[str], label: str, text: str, token: str) -> None:
    if token not in text:
        errors.append(f"{label} missing {token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    bridges = load_bridge_module(root)
    expected_ids = {row.surface_id for row in bridges}
    if len(expected_ids) != 34:
        fail(f"bridge command surface definition count drift: {len(expected_ids)}")

    registry = index_by_surface("SBSQL_SURFACE_REGISTRY", read_csv(root / REGISTRY))
    status = index_by_surface("SBSQL_SURFACE_STATUS_MATRIX", read_csv(root / STATUS))
    op_matrix = index_by_surface("SBSQL_TO_SBLR_OPERATION_MATRIX", read_csv(root / OP_MATRIX))
    ledger = index_by_surface("STRICT_ROW_COVERAGE_LEDGER", read_csv(artifact_root / LEDGER))
    manifest = index_by_surface("PER_ROW_EVIDENCE_MANIFEST", read_csv(artifact_root / MANIFEST))
    auth = index_by_surface("AUTHENTICATED_FULL_ROUTE_MATRIX", read_csv(artifact_root / AUTH_MATRIX))
    round_trip = index_by_surface("SBLR_BINARY_ROUND_TRIP_MATRIX", read_csv(artifact_root / ROUND_MATRIX))

    errors: list[str] = []
    for bridge in bridges:
        surface_id = bridge.surface_id
        for label, rows in (
            ("registry", registry),
            ("status", status),
            ("operation_matrix", op_matrix),
            ("strict_ledger", ledger),
            ("manifest", manifest),
            ("authenticated_route_matrix", auth),
            ("round_trip_matrix", round_trip),
        ):
            if surface_id not in rows:
                errors.append(f"{label} missing bridge surface_id {surface_id} {bridge.canonical_name}")
        if surface_id not in registry:
            continue

        reg = registry[surface_id]
        if reg.get("canonical_name") != bridge.canonical_name:
            errors.append(f"{surface_id} registry canonical_name drift")
        if reg.get("family") != "bridge":
            errors.append(f"{surface_id} registry family drift")
        if reg.get("cluster_scope") != bridge.cluster_scope:
            errors.append(f"{surface_id} registry cluster_scope drift")
        if reg.get("sblr_operation_family") != "sblr.bridge.operation.v3":
            errors.append(f"{surface_id} registry SBLR family drift")

        op = op_matrix.get(surface_id, {})
        op_text = ";".join(op.values())
        for token in (
            f"operation_id={bridge.operation_id}",
            f"opcode={bridge.opcode}",
            f"requires_transaction_context={str(bridge.requires_transaction_context).lower()}",
            f"cluster_route={str(bridge.cluster_route).lower()}",
        ):
            require_contains(errors, f"{surface_id} operation matrix", op_text, token)

        led = ledger.get(surface_id, {})
        man = manifest.get(surface_id, {})
        final_text = ";".join(man.values()) + ";" + ";".join(led.values())
        require_contains(errors, surface_id, final_text, f"operation_id={bridge.operation_id}")
        require_contains(errors, surface_id, final_text, f"opcode={bridge.opcode}")
        require_contains(errors, surface_id, final_text, "sbsql_bridge_command_route_conformance")
        require_contains(errors, surface_id, final_text, "sbsql_bridge_command_route_conformance.cpp")
        if bridge.cluster_route:
            if led.get("current_state") != "exact_refusal_passed":
                errors.append(f"{surface_id} cluster strict ledger state drift")
            if man.get("final_state") != "cluster_provider_route_passed":
                errors.append(f"{surface_id} cluster manifest final_state drift")
            for token in (
                "provider_boundary_route_evidence",
                "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
                "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
                "request_lifecycle_routed_through_cluster_provider_boundary",
                "UDR.BRIDGE.UNSUPPORTED",
                "UDR.BRIDGE.UNLICENSED",
                "private_cluster_execution=false",
            ):
                require_contains(errors, surface_id, final_text, token)
        else:
            if led.get("current_state") != "e2e_passed":
                errors.append(f"{surface_id} strict ledger state drift")
            if man.get("final_state") != "e2e_passed":
                errors.append(f"{surface_id} manifest final_state drift")
            require_contains(errors, surface_id, final_text, "canonical_message_vector_set")
            if bridge.expected_refusal_code:
                require_contains(errors, surface_id, final_text, bridge.expected_refusal_code)

        auth_fields = parse_fixture(root / AUTH_DIR / f"{surface_id}.route.yaml")
        round_fields = parse_fixture(root / ROUND_DIR / f"{surface_id}.round_trip.yaml")
        for fields, kind in ((auth_fields, "authenticated_route"), (round_fields, "sblr_binary_round_trip")):
            if fields.get("fixture_status") != "e2e_passed":
                errors.append(f"{surface_id} {kind} fixture not promoted")
            if fields.get("canonical_name") != bridge.canonical_name:
                errors.append(f"{surface_id} {kind} canonical_name drift")
            if fields.get("per_row_final_state") != man.get("final_state"):
                errors.append(f"{surface_id} {kind} per_row_final_state drift")
        if not bridge.cluster_route:
            if round_fields.get("expected_canonical_function_or_api_operation_id") != bridge.operation_id:
                errors.append(f"{surface_id} round-trip canonical operation id drift")
            if round_fields.get("byte_identical_round_trip_required") != "yes":
                errors.append(f"{surface_id} noncluster round-trip requirement drift")
        else:
            if round_fields.get("byte_identical_round_trip_required") != "not_applicable_no_round_trip_in_public_build":
                errors.append(f"{surface_id} cluster round-trip refusal requirement drift")

    ctest_source = (root / "project/tests/sbsql_parser_worker/sbsql_bridge_command_route_conformance.cpp").read_text(encoding="utf-8")
    for bridge in bridges:
        require_contains(errors, "route conformance source", ctest_source, bridge.sql)
        require_contains(errors, "route conformance source", ctest_source, bridge.operation_id)
        require_contains(errors, "route conformance source", ctest_source, bridge.opcode)

    specs = {
        BRIDGE_SPEC: (root / BRIDGE_SPEC).read_text(encoding="utf-8"),
        AST_SPEC: (root / AST_SPEC).read_text(encoding="utf-8"),
        GRAMMAR_SPEC: (root / GRAMMAR_SPEC).read_text(encoding="utf-8"),
        CMAKE: (root / CMAKE).read_text(encoding="utf-8"),
    }
    for path, text in specs.items():
        require_contains(errors, path, text, "SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING")
    for token in (
        "SBLR_BRIDGE_OPEN_CHANNEL",
        "SBLR_BRIDGE_TX_BEGIN",
        "SBLR_BRIDGE_CDC_APPLY",
        "SBLR_BRIDGE_CUTOVER",
        "BRIDGE STREAM OPEN PHYSICAL PAGE COPY",
        "BRIDGE CLUSTER ROUTE",
    ):
        require_contains(errors, BRIDGE_SPEC, specs[BRIDGE_SPEC], token)
        require_contains(errors, AST_SPEC, specs[AST_SPEC], token)
    require_contains(errors, CMAKE, specs[CMAKE], "sbsql_bridge_command_surface_tracking_gate")

    if errors:
        for error in errors[:80]:
            print(error, file=sys.stderr)
        if len(errors) > 80:
            print(f"... {len(errors) - 80} additional errors", file=sys.stderr)
        print("sbsql_bridge_command_surface_tracking_gate=failed", file=sys.stderr)
        return 1

    print(f"sbsql_bridge_command_surface_tracking_gate=passed rows={len(expected_ids)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
