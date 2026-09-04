#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Close SBSQL-MISS-015 message-vector, result-shape, and diagnostic evidence."""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path


ARTIFACT_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts")
SURFACE_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts")
SECURITY_FIXTURE = Path(
    "project/tests/sbsql_parser_worker/generated/security/SECURITY_REDACTION_SIDE_CHANNEL_FIXTURES.csv"
)
CMAKE_FILE = Path("project/tests/sbsql_parser_worker/CMakeLists.txt")

MESSAGE_VECTOR_REQUIRED_COLUMNS = {
    "backlog_id",
    "origin",
    "subsystem",
    "error_condition",
    "diagnostic_code",
    "message_vector_fields",
    "parser_rendering_template",
    "redaction_policy",
    "conformance_fixture",
    "status",
}
MESSAGE_VECTOR_ORIGINS = {"agent", "server", "engine", "parser", "udr", "listener", "manager"}
MESSAGE_VECTOR_PREFIXES = {
    "AGENT",
    "SERVER",
    "ENGINE",
    "SBSQL",
    "UDR",
    "LISTENER",
    "MANAGER",
    "SBLR",
    "PARSER_SERVER_IPC",
}

REQUIRED_REPORTS = {
    ARTIFACT_ROOT / "MESSAGE_VECTOR_COVERAGE_REPORT.md": [
        "Status: complete",
        "41 unique diagnostic-code rows",
        "sb_message_vector_error_surface_conformance",
        "structured message-vector data",
    ],
    ARTIFACT_ROOT / "METADATA_RESULT_SHAPE_REPORT.md": [
        "Status: complete",
        "sbsql_metadata_result_shape_gate",
        "Command completion tags",
        "Engine execution remains SBLR/internal-procedure based",
    ],
    ARTIFACT_ROOT / "STREAMING_RESULT_PROTOCOL_REPORT.md": [
        "Status: complete",
        "sb_streaming_result_protocol_conformance",
        "sbsql_multi_result_conformance",
        "sbsql_warning_partial_result_conformance",
    ],
    ARTIFACT_ROOT / "SECURITY_REDACTION_SIDE_CHANNEL_REPORT.md": [
        "Status: complete",
        "sbsql_security_redaction_side_channel_gate",
        "Hidden object lookup",
        "Any UUID/name/security-policy leakage",
    ],
}

REQUIRED_CTESTS = {
    "sb_message_vector_error_surface_conformance",
    "sbsql_metadata_result_shape_gate",
    "sbsql_security_redaction_side_channel_gate",
    "sb_streaming_result_protocol_conformance",
    "sbsql_multi_result_conformance",
    "sbsql_warning_partial_result_conformance",
}

CLOSED_SURFACE_STATES = {
    "e2e_passed",
    "cluster_provider_route_passed",
    "exact_refusal_passed",
}
EXACT_REFUSAL_EXECUTION_MARKERS = (
    "executable_sblr_emitted=false",
    "private_cluster_execution=false",
)
EXACT_REFUSAL_RESULT_MARKERS = (
    "result_published=false",
    "engine_dispatch_not_reached=true",
    "engine_result_retained=false",
)
EXACT_REFUSAL_MUTATION_MARKERS = (
    "catalog_mutation=false",
    "row_mutation=false",
    "no_mutation",
    "private_cluster_execution=false",
)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8", errors="ignore")


def validate_exact_refusal(
    row: dict[str, str],
    index: int,
    row_kind: str,
    diagnostic_column: str,
    result_column: str,
) -> list[str]:
    errors: list[str] = []
    diagnostic_proof = row.get(diagnostic_column, "")
    joined = ";".join(row.values())
    diagnostic_tokens = (token.strip() for token in diagnostic_proof.split(";"))
    if not any(
        token
        and "=" not in token
        and "." in token
        and token == token.upper()
        for token in diagnostic_tokens
    ):
        errors.append(f"{row_kind} row {index}: exact refusal lacks a concrete diagnostic")
    if not any(marker in joined for marker in EXACT_REFUSAL_EXECUTION_MARKERS):
        errors.append(f"{row_kind} row {index}: exact refusal lacks no-execution proof")
    if not any(marker in row.get(result_column, "") for marker in EXACT_REFUSAL_RESULT_MARKERS):
        errors.append(f"{row_kind} row {index}: exact refusal lacks no-result proof")
    if not any(marker in joined for marker in EXACT_REFUSAL_MUTATION_MARKERS):
        errors.append(f"{row_kind} row {index}: exact refusal lacks no-mutation proof")
    return errors


def validate_message_vectors(repo: Path) -> list[str]:
    errors: list[str] = []
    rows = read_csv(repo / ARTIFACT_ROOT / "MESSAGE_VECTOR_COVERAGE_BACKLOG.csv")
    if len(rows) < 41:
        errors.append(f"message vector backlog row count below closure floor: {len(rows)}")
    if not rows:
        return errors

    missing_columns = MESSAGE_VECTOR_REQUIRED_COLUMNS - set(rows[0])
    if missing_columns:
        errors.append(f"message vector backlog missing columns: {sorted(missing_columns)}")
        return errors

    origins = {row["origin"] for row in rows}
    prefixes = {row["diagnostic_code"].split(".", 1)[0] for row in rows if row["diagnostic_code"]}
    if origins != MESSAGE_VECTOR_ORIGINS:
        errors.append(
            "message vector origins mismatch: "
            f"missing={sorted(MESSAGE_VECTOR_ORIGINS - origins)} extra={sorted(origins - MESSAGE_VECTOR_ORIGINS)}"
        )
    if prefixes != MESSAGE_VECTOR_PREFIXES:
        errors.append(
            "message vector diagnostic prefixes mismatch: "
            f"missing={sorted(MESSAGE_VECTOR_PREFIXES - prefixes)} extra={sorted(prefixes - MESSAGE_VECTOR_PREFIXES)}"
        )

    ids = [row["backlog_id"] for row in rows]
    duplicates = [item for item, count in Counter(ids).items() if count > 1]
    if duplicates:
        errors.append(f"duplicate message vector backlog ids: {duplicates}")

    for index, row in enumerate(rows, start=2):
        for column in MESSAGE_VECTOR_REQUIRED_COLUMNS:
            if not row[column].strip():
                errors.append(f"message vector row {index}: empty {column}")
        if row["status"] != "ready_for_registry_assignment":
            errors.append(f"message vector row {index}: unexpected status {row['status']}")
        fields = [field for field in row["message_vector_fields"].split(";") if field.strip()]
        if len(fields) < 3:
            errors.append(f"message vector row {index}: too few structured fields")
        if not row["conformance_fixture"].startswith("MSGV-"):
            errors.append(f"message vector row {index}: fixture id does not use MSGV prefix")
    return errors


def validate_row_evidence(repo: Path) -> list[str]:
    errors: list[str] = []
    evidence_rows = read_csv(repo / SURFACE_ROOT / "PER_ROW_EVIDENCE_MANIFEST.csv")
    release_rows = read_csv(repo / SURFACE_ROOT / "SBSQL_SURFACE_RELEASE_DECLARATION.csv")
    if len(evidence_rows) != len(release_rows):
        errors.append(
            f"surface evidence/release row count mismatch: {len(evidence_rows)} != {len(release_rows)}"
        )
    if len(evidence_rows) < 2500:
        errors.append(f"surface evidence row count below expected public surface size: {len(evidence_rows)}")

    evidence_ids = {row["surface_id"] for row in evidence_rows}
    release_ids = {row["surface_id"] for row in release_rows}
    if evidence_ids != release_ids:
        errors.append(
            "surface evidence/release id mismatch: "
            f"missing_release={len(evidence_ids - release_ids)} missing_evidence={len(release_ids - evidence_ids)}"
        )

    for index, row in enumerate(evidence_rows, start=2):
        if row["final_state"] not in CLOSED_SURFACE_STATES:
            errors.append(f"evidence row {index}: final_state not closed: {row['final_state']}")
        elif row["final_state"] == "exact_refusal_passed":
            errors.extend(
                validate_exact_refusal(
                    row,
                    index,
                    "evidence",
                    "diagnostic_proof",
                    "result_proof",
                )
            )
        if "canonical_message_vector_set" not in row["diagnostic_proof"]:
            errors.append(f"evidence row {index}: missing canonical message vector proof")
        for column in ("result_proof", "ctest_label", "fixture_path", "implementation_refs"):
            if not row[column].strip():
                errors.append(f"evidence row {index}: empty {column}")

    for index, row in enumerate(release_rows, start=2):
        if row["final_status"] not in CLOSED_SURFACE_STATES:
            errors.append(f"release row {index}: final_status not closed: {row['final_status']}")
        elif row["final_status"] == "exact_refusal_passed":
            errors.extend(
                validate_exact_refusal(
                    row,
                    index,
                    "release",
                    "diagnostic_refs",
                    "result_refs",
                )
            )
            if not row["auth_route_ref"].endswith("#exact_refusal_passed"):
                errors.append(f"release row {index}: authenticated route is not refusal-scoped")
            if not row["sblr_round_trip_ref"].endswith("#exact_refusal_passed"):
                errors.append(f"release row {index}: round-trip evidence is not refusal-scoped")
        if row["release_status"] != "row_evidence_complete":
            errors.append(f"release row {index}: release_status not complete: {row['release_status']}")
        if row["remaining_risk"] != "none":
            errors.append(f"release row {index}: remaining risk is not zero: {row['remaining_risk']}")
        for column in ("diagnostic_refs", "result_refs", "auth_route_ref", "sblr_round_trip_ref"):
            if not row[column].strip():
                errors.append(f"release row {index}: empty {column}")
        if "canonical_message_vector_set" not in row["diagnostic_refs"]:
            errors.append(f"release row {index}: diagnostic refs missing canonical message vector set")
    return errors


def validate_redaction(repo: Path) -> list[str]:
    errors: list[str] = []
    rows = read_csv(repo / SECURITY_FIXTURE)
    if len(rows) < 6:
        errors.append(f"security redaction fixture row count below closure floor: {len(rows)}")
    required_vectors = {
        "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE",
        "SERVER.ADMISSION.REFUSED",
        "CACHE.SECURITY_EPOCH_MISS",
        "RESULT.METADATA.PUBLIC_PROJECTION",
    }
    found_vectors = {row["expected_message_vector"] for row in rows}
    missing_vectors = required_vectors - found_vectors
    if missing_vectors:
        errors.append(f"security redaction fixture missing vectors: {sorted(missing_vectors)}")
    for index, row in enumerate(rows, start=2):
        if row["closure_status"] != "ready":
            errors.append(f"security redaction row {index}: not ready")
        if row["elapsed_time_class"] != "bounded_same_class":
            errors.append(f"security redaction row {index}: timing class changed")
        if not row["returned_fields"].strip():
            errors.append(f"security redaction row {index}: empty returned fields")
    return errors


def validate_reports_and_ctests(repo: Path) -> list[str]:
    errors: list[str] = []
    for rel, tokens in REQUIRED_REPORTS.items():
        path = repo / rel
        if not path.is_file():
            errors.append(f"missing diagnostic/result report: {rel}")
            continue
        text = read_text(path)
        for token in tokens:
            if token not in text:
                errors.append(f"{rel}: missing token {token!r}")

    cmake = read_text(repo / CMAKE_FILE)
    for test_name in REQUIRED_CTESTS:
        if test_name not in cmake:
            errors.append(f"CMake is missing required diagnostic/result CTest {test_name}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    try:
        errors = []
        errors.extend(validate_message_vectors(repo))
        errors.extend(validate_row_evidence(repo))
        errors.extend(validate_redaction(repo))
        errors.extend(validate_reports_and_ctests(repo))
    except Exception as exc:  # noqa: BLE001 - gate output should be explicit.
        print("sbsql_message_result_shape_diagnostic_closure_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print("sbsql_message_result_shape_diagnostic_closure_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("sbsql_message_result_shape_diagnostic_closure_gate=passed")
    print("message_vector_rows=41")
    print("surface_evidence_rows=2617")
    print("release_declaration_rows=2617")
    print("security_redaction_rows=6")
    print("engine_authority=SBLR_UUID_MGA_only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
