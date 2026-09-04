#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate final generated evidence for the central-import command slice."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
)
HASH_MANIFEST_NAME = "PLAN_IMPORT_ROWS_GENERATED_EVIDENCE_SHA256.csv"
PROVENANCE_ARTIFACT_ID = "IA-GEN-0008"
PROVENANCE_RELATIVE = (
    "Workplans/sbsql-sblr-implementation-alignment/GENERATED_PROVENANCE.csv"
)
SELECTOR_GENERATOR_PATH = (
    "project/tools/sb_parser_gen/refresh_plan_import_rows_generated_evidence.py"
)
SELECTOR_INVOCATION = (
    "cwd=ScratchBird;check=python3 "
    f"{SELECTOR_GENERATOR_PATH} --repo-root .;apply=python3 "
    f"{SELECTOR_GENERATOR_PATH} --repo-root . --apply"
)

SURFACES = {
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
POSITIVE_IDS = {"SBSQL-465931ED7427", "SBSQL-4F912014EA85"}
REFUSAL_IDS = set(SURFACES) - POSITIVE_IDS

POSITIVE_IDENTITY = (
    "operation_id=engine.op.bulk_import_stream",
    "opcode=SBLR_BULK_IMPORT_STREAM",
    "opcode_code=775",
    "operand_descriptor_id=bulk_import_stream_descriptor",
    "result_descriptor_id=bulk_mutation_result",
    "result=BIRS_v1_192_bytes",
)
POSITIVE_RESULT = (
    "BIRS_exact=true",
    "durable_publication=true",
    "affected_rows_from_BIRS=true",
    "command_completion=COPY_N",
    "commit_visibility_proved=true",
    "rollback_invisibility_proved=true",
    "independent_session_post_state=true",
    "restart_post_state=true",
)
REFUSAL_IDENTITY = (
    "operation_id=not_admitted",
    "root_route=diagnostic_refusal",
    "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
    "executable_sblr_emitted=false",
)
REFUSAL_RESULT = (
    "diagnostic=SBSQL.IMPL.NOT_AVAILABLE",
    "executable_sblr_emitted=false",
    "result_published=false",
    "descriptor_authority_published=false",
    "transaction_state_transition=false",
    "catalog_mutation=false",
    "row_mutation=false",
    "durable_state_byte_identical=true",
)
OBSOLETE_TOKENS = (
    "dml.plan_import_rows",
    "SBLR_DML_PLAN_IMPORT_ROWS",
    "opcode_code=793",
    "import_rows_plan_descriptor",
    "import_plan_result",
    "executor_evidence_carrier=IPEV",
    "final_state=pending",
    "fixture_status=fixture_authored",
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index(rows: list[dict[str, str]], label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        surface_id = row.get("surface_id", "")
        if not surface_id or surface_id in out:
            fail(f"{label} missing or duplicate surface id {surface_id}")
        out[surface_id] = row
    return out


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_fixture(path: Path) -> dict[str, str]:
    if not path.is_file():
        fail(f"required central-import fixture missing: {path}")
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith((" ", "-")):
            continue
        if ":" not in line:
            continue
        key, raw = line.split(":", 1)
        value = raw.strip()
        fields[key] = json.loads(value) if value.startswith('"') else value
    return fields


def require_tokens(surface_id: str, label: str, text: str, tokens: tuple[str, ...]) -> None:
    for token in tokens:
        if token not in text:
            fail(f"{surface_id} {label} missing exact token {token}")


def reject_obsolete(surface_id: str, label: str, text: str) -> None:
    for token in OBSOLETE_TOKENS:
        if token in text:
            fail(f"{surface_id} {label} retains obsolete identity/state {token}")


def verify_core_authority(root: Path) -> None:
    core = root.parent / "Specifications/Core"
    command_files = (
        core / "registries/sbsql-command-sblr-zero-grey-closure.csv",
        core / "registries/normalized-semantic-closure-overlay-20260822.csv",
    )
    matches: dict[str, list[dict[str, str]]] = {key: [] for key in SURFACES}
    for path in command_files:
        for row in read_csv(path):
            if row.get("surface_id") in matches:
                matches[row["surface_id"]].append(row)
    for surface_id, rows in matches.items():
        if len(rows) != 1:
            fail(f"{surface_id} Core central-command identity count is {len(rows)}")
        row = rows[0]
        if row.get("canonical_name") != SURFACES[surface_id]:
            fail(f"{surface_id} Core canonical name drift")
        if surface_id in POSITIVE_IDS:
            expected = {
                "specification_state": "specified_admitted",
                "root_route": "SBLR_BULK_IMPORT_STREAM",
                "descriptor_contract": "bulk_import_stream_descriptor.v1",
                "executor_operation_id": "engine.op.bulk_import_stream",
                "result_shape": "bulk_mutation_result",
            }
        else:
            expected = {
                "specification_state": "specified_gated",
                "root_route": "SBLR_DIAGNOSTIC_REFUSAL",
                "executor_operation_id": "not_admitted",
                "result_shape": "diagnostic_vector.v1",
                "diagnostic_key": "SBSQL.IMPL.NOT_AVAILABLE",
            }
        if {key: row.get(key, "") for key in expected} != expected:
            fail(f"{surface_id} Core central-command tuple drift")


def verify_hash_manifest_and_provenance(
    root: Path,
    artifact_root: Path,
    auth: dict[str, dict[str, str]],
    round_trip: dict[str, dict[str, str]],
) -> None:
    manifest_path = artifact_root / HASH_MANIFEST_NAME
    manifest_rows = read_csv(manifest_path)
    indexed: dict[str, dict[str, str]] = {}
    for row in manifest_rows:
        path = row.get("artifact_path", "")
        if not path or path in indexed:
            fail(f"central-import hash manifest has missing or duplicate path {path}")
        indexed[path] = row

    expected: dict[str, tuple[str, str]] = {}
    for name in (
        "STRICT_ROW_COVERAGE_LEDGER.csv",
        "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
        "SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
        "PER_ROW_EVIDENCE_MANIFEST.csv",
        "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
        "SBSQL_SURFACE_RELEASE_DECLARATION.json",
    ):
        expected[f"{DEFAULT_ARTIFACT_ROOT}/{name}"] = ("summary", "")
    for surface_id in sorted(SURFACES):
        expected[
            f"{DEFAULT_ARTIFACT_ROOT}/PER_ELEMENT_CONTRACTS/{surface_id}.md"
        ] = ("per_element_contract", surface_id)
        expected[auth[surface_id]["fixture_path"]] = (
            "authenticated_route_fixture",
            surface_id,
        )
        expected[round_trip[surface_id]["fixture_path"]] = (
            "sblr_binary_round_trip_fixture",
            surface_id,
        )
    if len(expected) != 33 or set(indexed) != set(expected):
        fail(
            "central-import hash manifest artifact set drift: "
            f"expected={len(expected)} observed={len(indexed)}"
        )
    for relative, metadata in sorted(expected.items()):
        row = indexed[relative]
        if (row.get("artifact_kind"), row.get("surface_id")) != metadata:
            fail(f"central-import hash manifest metadata drift: {relative}")
        physical = (root / relative).resolve()
        try:
            physical.relative_to(root)
        except ValueError:
            fail(f"central-import hash manifest path escapes repository: {relative}")
        if not physical.is_file() or row.get("sha256") != sha256_file(physical):
            fail(f"central-import hash manifest digest drift: {relative}")

    provenance_rows = read_csv(root.parent / PROVENANCE_RELATIVE)
    matches = [
        row for row in provenance_rows if row.get("artifact_id") == PROVENANCE_ARTIFACT_ID
    ]
    if len(matches) != 1:
        fail("central-import workplan provenance row is missing or duplicated")
    provenance = matches[0]
    expected_projection = {
        "area_id": "IA-07",
        "artifact_paths": f"{DEFAULT_ARTIFACT_ROOT}/{HASH_MANIFEST_NAME}",
        "generator_path": SELECTOR_GENERATOR_PATH,
        "generator_version": "v2",
        "deterministic_invocation": SELECTOR_INVOCATION,
        "output_sha256_set": sha256_file(manifest_path),
        "generated_at": "2026-09-01T03:21:00Z",
        "validation_result": "PASS",
        "status": "accepted_final_central_import_generated_evidence_two_e2e_seven_exact_refusal",
    }
    if {key: provenance.get(key, "") for key in expected_projection} != expected_projection:
        fail("central-import workplan provenance projection drift")
    input_names = provenance.get("authoritative_inputs", "").split(";")
    input_hashes = provenance.get("input_sha256_set", "").split(";")
    if len(input_names) != len(input_hashes) or not input_names:
        fail("central-import provenance input name/hash cardinality drift")
    required_inputs = (
        "Specifications/Core/registries/sbsql-command-sblr-zero-grey-closure.csv",
        "Specifications/Core/registries/normalized-semantic-closure-overlay-20260822.csv",
        "Specifications/Core/registries/sblr-opcode-executor-zero-grey-closure.csv",
        "Specifications/Core/chapters/data-representation/datatypes/appendix-bulk-import-stream-data-transport-and-recovery.md",
        "ScratchBird/project/tests/sbsql_parser_worker/sbsql_copy_persistence_full_route_gate.py",
        "ScratchBird/project/tests/sbsql_parser_worker/sbsql_central_import_refusal_wire_conformance.cpp",
    )
    if any(value not in input_names for value in required_inputs):
        fail("central-import provenance omitted a required Core or executable input")
    if any(
        len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value)
        for value in input_hashes
    ):
        fail("central-import provenance contains a malformed input SHA-256")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    determinism = subprocess.run(
        [
            sys.executable,
            str(root / SELECTOR_GENERATOR_PATH),
            "--repo-root",
            str(root),
            "--artifact-root",
            str(artifact_root),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if determinism.returncode != 0:
        fail(
            "central-import selector regeneration check failed: "
            f"stdout={determinism.stdout.strip()[:500]} "
            f"stderr={determinism.stderr.strip()[:500]}"
        )

    verify_core_authority(root)
    strict = index(read_csv(artifact_root / "STRICT_ROW_COVERAGE_LEDGER.csv"), "strict ledger")
    manifest = index(read_csv(artifact_root / "PER_ROW_EVIDENCE_MANIFEST.csv"), "per-row manifest")
    auth = index(read_csv(artifact_root / "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"), "authenticated matrix")
    round_trip = index(read_csv(artifact_root / "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"), "round-trip matrix")
    release = index(read_csv(artifact_root / "SBSQL_SURFACE_RELEASE_DECLARATION.csv"), "release declaration")

    for surface_id, canonical_name in sorted(SURFACES.items()):
        rows = (
            strict[surface_id],
            manifest[surface_id],
            auth[surface_id],
            round_trip[surface_id],
            release[surface_id],
        )
        for row in rows:
            if row.get("canonical_name") != canonical_name:
                fail(f"{surface_id} canonical-name drift")
            reject_obsolete(surface_id, "generated row", ";".join(row.values()))

        strict_row, manifest_row, auth_row, round_row, release_row = rows
        expected_state = (
            "e2e_passed" if surface_id in POSITIVE_IDS else "exact_refusal_passed"
        )
        if strict_row.get("current_state") != expected_state:
            fail(f"{surface_id} strict final state drift")
        if strict_row.get("evidence_complete") != "yes":
            fail(f"{surface_id} strict evidence is not complete")
        if manifest_row.get("final_state") != expected_state:
            fail(f"{surface_id} per-row final state drift")
        if not manifest_row.get("evidence_collected_utc"):
            fail(f"{surface_id} final evidence lacks collection time")
        if auth_row.get("fixture_status") != expected_state:
            fail(f"{surface_id} authenticated matrix final state drift")
        if round_row.get("fixture_status") != expected_state:
            fail(f"{surface_id} round-trip matrix final state drift")
        if release_row.get("final_status") != expected_state:
            fail(f"{surface_id} release final state drift")
        expected_claim = (
            "public_sbsql_e2e_implemented"
            if surface_id in POSITIVE_IDS
            else "public_exact_refusal_implemented"
        )
        if (
            release_row.get("release_claim") != expected_claim
            or release_row.get("remaining_risk") != "none"
            or release_row.get("release_status") != "row_evidence_complete"
        ):
            fail(f"{surface_id} release completion projection drift")

        combined = ";".join(
            (
                strict_row.get("function_or_api_operation_id", ""),
                strict_row.get("engine_runtime_evidence", ""),
                manifest_row.get("implementation_refs", ""),
                manifest_row.get("diagnostic_proof", ""),
                manifest_row.get("result_proof", ""),
                auth_row.get("expected_authorization_accepted_outcome", ""),
                auth_row.get("expected_authorization_refused_outcome", ""),
                auth_row.get("expected_diagnostic_codes", ""),
                round_row.get("lower_phase_expectation", ""),
                round_row.get("dispatch_phase_expectation", ""),
                round_row.get("execute_phase_expectation", ""),
            )
        )
        if surface_id in POSITIVE_IDS:
            require_tokens(surface_id, "positive identity", combined, POSITIVE_IDENTITY)
            require_tokens(surface_id, "positive result", combined, POSITIVE_RESULT)
            if round_row.get("expected_canonical_function_or_api_operation_id") != "engine.op.bulk_import_stream":
                fail(f"{surface_id} positive operation identity drift")
            if round_row.get("byte_identical_round_trip_required") != "yes":
                fail(f"{surface_id} positive round-trip requirement drift")
        else:
            require_tokens(surface_id, "refusal identity", combined, REFUSAL_IDENTITY)
            require_tokens(surface_id, "refusal result", combined, REFUSAL_RESULT)
            if round_row.get("expected_canonical_function_or_api_operation_id") != "not_admitted_diagnostic_refusal":
                fail(f"{surface_id} refusal operation identity drift")
            if round_row.get("byte_identical_round_trip_required") != "not_applicable_no_executable_sblr":
                fail(f"{surface_id} refusal round-trip applicability drift")

        for matrix_row, fixture_kind in (
            (auth_row, "authenticated_route"),
            (round_row, "sblr_binary_round_trip"),
        ):
            fixture = parse_fixture(root / matrix_row["fixture_path"])
            if fixture.get("fixture_kind") != fixture_kind:
                fail(f"{surface_id} fixture kind drift")
            if (
                fixture.get("fixture_status") != expected_state
                or fixture.get("per_row_final_state") != expected_state
            ):
                fail(f"{surface_id} fixture final state drift")
            reject_obsolete(surface_id, "fixture", ";".join(fixture.values()))

        per_element = (
            artifact_root / "PER_ELEMENT_CONTRACTS" / f"{surface_id}.md"
        ).read_text(encoding="utf-8")
        if f"| Release final status | {expected_state} |" not in per_element:
            fail(f"{surface_id} per-element final state drift")
        if "| Remaining risk | none |" not in per_element:
            fail(f"{surface_id} per-element retained stale risk")
        reject_obsolete(surface_id, "per-element snapshot", per_element)

    release_summary = json.loads(
        (artifact_root / "SBSQL_SURFACE_RELEASE_DECLARATION.json").read_text(
            encoding="utf-8"
        )
    )
    if (
        release_summary.get("status") != "row_evidence_complete"
        or release_summary.get("blocked_rows") != 0
        or release_summary.get("final_status_counts", {}).get("pending", 0) != 0
        or release_summary.get("remaining_risk_rows") != 0
        or release_summary.get("authenticated_route_pending_rows") != 0
        or release_summary.get("sblr_round_trip_pending_rows") != 0
    ):
        fail("global release declaration retains pending central-import evidence")

    verify_hash_manifest_and_provenance(root, artifact_root, auth, round_trip)
    print(
        "sbsql_plan_import_rows_generated_evidence_gate=passed "
        "surfaces=9 opcode775_e2e=2 exact_refusal=7 pending=0 "
        "hashed_artifacts=33 provenance=IA-GEN-0008 "
        "non_target_rows_byte_identical=true"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
