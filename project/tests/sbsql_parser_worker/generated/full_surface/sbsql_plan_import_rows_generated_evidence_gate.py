#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the exact nonfinal generated evidence for plan-import rows."""

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
DIAGNOSTICS = ";".join(
    (
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
)
IDENTITY_TOKENS = (
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
RESULT_TOKENS = (
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
NO_EFFECT_TOKENS = (
    "mga_execution_performed=false",
    "row_decode_performed=false",
    "row_mutation=false",
    "catalog_mutation=false",
    "transaction_inventory_mutation=false",
    "transaction_state_transition=false",
    "finality_authority=false",
    "no_wal_authority",
)
FORBIDDEN = (
    "SBLR.ENVELOPE.*",
    "SBLR.OPCODE.*",
    "SECURITY.AUTHORIZATION.FORBIDDEN",
    "engine_execute_under_mga_copy_on_write_authority",
    "accepted_e2e_passes_with_ExecutionResultEnvelope_v3",
    "row_persistence_claimed=true",
    "row_execution_completed=true",
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def index(rows: list[dict[str, str]], label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        surface_id = row.get("surface_id", "")
        if not surface_id or surface_id in out:
            fail(f"{label} missing or duplicate surface id {surface_id}")
        out[surface_id] = row
    return out


def parse_fixture(path: Path) -> dict[str, str]:
    if not path.is_file():
        fail(f"required plan-import fixture missing: {path}")
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


def require_no_forbidden(surface_id: str, label: str, text: str) -> None:
    for token in FORBIDDEN:
        if token in text:
            fail(f"{surface_id} {label} contains forbidden overclaim or alias {token}")


def selector_ids(rows: list[dict[str, str]], field: str, marker: str) -> set[str]:
    return {row["surface_id"] for row in rows if marker in row.get(field, "")}


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
            fail(f"plan-import hash manifest has missing or duplicate path {path}")
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
            "plan-import hash manifest artifact set drift: "
            f"expected={len(expected)} observed={len(indexed)}"
        )
    for relative, metadata in sorted(expected.items()):
        row = indexed[relative]
        if (row.get("artifact_kind"), row.get("surface_id")) != metadata:
            fail(f"plan-import hash manifest metadata drift: {relative}")
        physical = (root / relative).resolve()
        try:
            physical.relative_to(root)
        except ValueError:
            fail(f"plan-import hash manifest path escapes repository: {relative}")
        if not physical.is_file() or row.get("sha256") != sha256_file(physical):
            fail(f"plan-import hash manifest digest drift: {relative}")

    provenance_path = root.parent / PROVENANCE_RELATIVE
    provenance_rows = read_csv(provenance_path)
    matches = [
        row
        for row in provenance_rows
        if row.get("artifact_id") == PROVENANCE_ARTIFACT_ID
    ]
    if len(matches) != 1:
        fail("plan-import workplan provenance row is missing or duplicated")
    provenance = matches[0]
    expected_projection = {
        "area_id": "IA-07",
        "artifact_paths": f"{DEFAULT_ARTIFACT_ROOT}/{HASH_MANIFEST_NAME}",
        "generator_path": SELECTOR_GENERATOR_PATH,
        "generator_version": "v1",
        "deterministic_invocation": SELECTOR_INVOCATION,
        "output_sha256_set": sha256_file(manifest_path),
        "generated_at": "2026-08-29T00:57:02Z",
        "validation_result": "PASS",
        "status": (
            "accepted_nonfinal_plan_import_rows_generated_evidence_"
            "pending_independent_SBWP_TLS_post_state_proof"
        ),
    }
    observed_projection = {
        key: provenance.get(key, "") for key in expected_projection
    }
    if observed_projection != expected_projection:
        fail("plan-import workplan provenance projection drift")
    input_names = provenance.get("authoritative_inputs", "").split(";")
    input_hashes = provenance.get("input_sha256_set", "").split(";")
    if len(input_names) != len(input_hashes) or not input_names:
        fail("plan-import provenance input name/hash cardinality drift")
    required_inputs = (
        "Specifications/Core/MANIFEST.yaml",
        "Specifications/Core/registries/sblr-operand-descriptors.yaml",
        "Specifications/Core/registries/sblr-operand-descriptors.yaml#"
        "SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1:canonical_json",
        "ScratchBird/project/src/engine/internal_api/"
        "SBLR_API_OPERATION_MATRIX.yaml#dml.plan_import_rows:canonical_json",
        "ScratchBird/project/src/engine/internal_api/"
        "ENGINE_API_SURFACE_REGISTRY.yaml#dml.plan_import_rows:canonical_json",
        "ScratchBird/project/src/engine/sblr/sblr_plan_import_rows_codec.hpp",
        "ScratchBird/project/src/engine/internal_api/dml/import_api.hpp",
    )
    if any(value not in input_names for value in required_inputs):
        fail("plan-import provenance omitted a required Core or engine input")
    if any(
        len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value)
        for value in input_hashes
    ):
        fail("plan-import provenance contains a malformed input SHA-256")


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
            str(root / "project/tools/sb_parser_gen/refresh_plan_import_rows_generated_evidence.py"),
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
            "plan-import selector regeneration check failed: "
            f"stdout={determinism.stdout.strip()[:500]} stderr={determinism.stderr.strip()[:500]}"
        )

    strict_rows = read_csv(artifact_root / "STRICT_ROW_COVERAGE_LEDGER.csv")
    manifest_rows = read_csv(artifact_root / "PER_ROW_EVIDENCE_MANIFEST.csv")
    auth_rows = read_csv(artifact_root / "AUTHENTICATED_FULL_ROUTE_MATRIX.csv")
    round_rows = read_csv(artifact_root / "SBLR_BINARY_ROUND_TRIP_MATRIX.csv")
    release_rows = read_csv(artifact_root / "SBSQL_SURFACE_RELEASE_DECLARATION.csv")
    strict = index(strict_rows, "strict ledger")
    manifest = index(manifest_rows, "per-row manifest")
    auth = index(auth_rows, "authenticated route matrix")
    round_trip = index(round_rows, "round-trip matrix")
    release = index(release_rows, "release declaration")
    expected_ids = set(SURFACES)

    derived_sets = (
        selector_ids(strict_rows, "function_or_api_operation_id", "operation_id=dml.plan_import_rows"),
        selector_ids(manifest_rows, "implementation_refs", "operation_id=dml.plan_import_rows"),
        selector_ids(auth_rows, "expected_authorization_accepted_outcome", "operation_id=dml.plan_import_rows"),
        selector_ids(round_rows, "expected_canonical_function_or_api_operation_id", "dml.plan_import_rows"),
        selector_ids(release_rows, "implementation_refs", "operation_id=dml.plan_import_rows"),
    )
    for observed in derived_sets:
        if observed != expected_ids:
            fail(
                "plan-import generated selector set drift: "
                f"expected={sorted(expected_ids)} observed={sorted(observed)}"
            )

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
            require_no_forbidden(surface_id, "generated row", ";".join(row.values()))

        strict_row, manifest_row, auth_row, round_row, release_row = rows
        if strict_row.get("current_state") != "engine_runtime_implemented":
            fail(f"{surface_id} strict row overstates or understates engine-runtime state")
        if strict_row.get("evidence_complete") != "no":
            fail(f"{surface_id} strict row must remain nonfinal")
        if strict_row.get("diagnostic_evidence") != DIAGNOSTICS:
            fail(f"{surface_id} strict diagnostic precedence drift")
        require_tokens(surface_id, "strict identity", strict_row["function_or_api_operation_id"], IDENTITY_TOKENS)
        require_tokens(surface_id, "strict result", strict_row["engine_runtime_evidence"], RESULT_TOKENS)
        require_tokens(surface_id, "strict no-effect", strict_row["engine_runtime_evidence"], NO_EFFECT_TOKENS)

        if manifest_row.get("final_state") != "pending":
            fail(f"{surface_id} per-row final state must remain pending")
        if manifest_row.get("evidence_collected_utc"):
            fail(f"{surface_id} pending per-row evidence may not have a collection timestamp")
        if manifest_row.get("diagnostic_proof") != DIAGNOSTICS:
            fail(f"{surface_id} per-row diagnostic precedence drift")
        if "sbsql_e2e_passed" in manifest_row.get("ctest_label", ""):
            fail(f"{surface_id} pending per-row evidence carries an E2E label")
        if "pending_independent_SBWP_TLS_post_state_proof" not in manifest_row.get("promoter_slice", ""):
            fail(f"{surface_id} pending per-row evidence lost the exact promotion blocker")
        require_tokens(surface_id, "manifest identity", manifest_row["implementation_refs"], IDENTITY_TOKENS)
        require_tokens(surface_id, "manifest result", manifest_row["result_proof"], RESULT_TOKENS)
        require_tokens(surface_id, "manifest no-effect", manifest_row["result_proof"], NO_EFFECT_TOKENS)

        if auth_row.get("fixture_status") != "fixture_authored":
            fail(f"{surface_id} authenticated fixture layer must remain authored/nonfinal")
        if auth_row.get("expected_diagnostic_codes") != DIAGNOSTICS:
            fail(f"{surface_id} authenticated-route diagnostic precedence drift")
        require_tokens(surface_id, "authenticated identity", auth_row["expected_authorization_accepted_outcome"], IDENTITY_TOKENS)
        require_tokens(surface_id, "authenticated result", auth_row["expected_authorization_accepted_outcome"], RESULT_TOKENS)
        require_tokens(surface_id, "authenticated no-effect", auth_row["mga_execution_authority"], NO_EFFECT_TOKENS)

        if round_row.get("fixture_status") != "fixture_authored":
            fail(f"{surface_id} binary fixture layer must remain authored/nonfinal")
        if round_row.get("expected_canonical_function_or_api_operation_id") != "dml.plan_import_rows":
            fail(f"{surface_id} round-trip operation identity drift")
        require_tokens(
            surface_id,
            "round-trip lower phase",
            round_row["lower_phase_expectation"],
            ("SBLR_DML_PLAN_IMPORT_ROWS", "opcode_code_793", "version_1.0", "import_rows_plan_descriptor_v1"),
        )
        require_tokens(
            surface_id,
            "round-trip render phase",
            round_row["render_phase_expectation"],
            ("import_plan_result_v1", "twelve_added_fields", "IPEV_v1_208_bytes"),
        )
        require_tokens(surface_id, "round-trip no-effect", round_row["execution_authority_model"], NO_EFFECT_TOKENS)
        if not round_row.get("execute_phase_expectation", "").startswith("not_applicable_planning_only"):
            fail(f"{surface_id} round-trip execute phase falsely claims execution")

        if (
            release_row.get("final_status") != "pending"
            or release_row.get("release_claim") != "not_releasable"
            or release_row.get("release_status") != "blocked"
        ):
            fail(f"{surface_id} release declaration overstates plan-import completion")
        if release_row.get("diagnostic_refs") != DIAGNOSTICS:
            fail(f"{surface_id} release declaration diagnostic precedence drift")
        if release_row.get("remaining_risk") != (
            "authenticated_route_fixture_status=fixture_authored;"
            "sblr_round_trip_fixture_status=fixture_authored"
        ):
            fail(f"{surface_id} release declaration lost nonfinal fixture risk")

        for matrix_row, fixture_kind in (
            (auth_row, "authenticated_route"),
            (round_row, "sblr_binary_round_trip"),
        ):
            fixture = parse_fixture(root / matrix_row["fixture_path"])
            if fixture.get("fixture_kind") != fixture_kind:
                fail(f"{surface_id} fixture kind drift")
            if fixture.get("fixture_status") != "fixture_authored":
                fail(f"{surface_id} fixture file overstates completion")
            if fixture.get("per_row_final_state") != "pending":
                fail(f"{surface_id} fixture file overstates per-row final state")
            if fixture.get("diagnostic_proof") != DIAGNOSTICS:
                fail(f"{surface_id} fixture diagnostic proof drift")
            require_no_forbidden(surface_id, "fixture", ";".join(fixture.values()))

        per_element = (
            root
            / "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/PER_ELEMENT_CONTRACTS"
            / f"{surface_id}.md"
        ).read_text(encoding="utf-8")
        if f"| Canonical diagnostic precedence | {DIAGNOSTICS} |" not in per_element:
            fail(f"{surface_id} per-element snapshot lost exact diagnostic precedence")
        require_no_forbidden(surface_id, "per-element snapshot", per_element)

    release_summary = json.loads(
        (artifact_root / "SBSQL_SURFACE_RELEASE_DECLARATION.json").read_text(
            encoding="utf-8"
        )
    )
    expected_count = len(SURFACES)
    if (
        release_summary.get("status") != "blocked"
        or release_summary.get("blocked_rows") != expected_count
        or release_summary.get("final_status_counts", {}).get("pending") != expected_count
        or release_summary.get("remaining_risk_rows") != expected_count
        or release_summary.get("authenticated_route_pending_rows") != expected_count
        or release_summary.get("sblr_round_trip_pending_rows") != expected_count
    ):
        fail("global release declaration does not report exactly nine nonfinal plan-import rows")

    verify_hash_manifest_and_provenance(
        root,
        artifact_root,
        auth,
        round_trip,
    )

    print(
        "sbsql_plan_import_rows_generated_evidence_gate=passed "
        "surfaces=9 strict_state=engine_runtime_implemented final_state=pending "
        "route_fixtures=fixture_authored round_trip_fixtures=fixture_authored "
        "diagnostics=9 hashed_artifacts=33 provenance=IA-GEN-0008 "
        "non_target_rows_byte_identical=true"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
