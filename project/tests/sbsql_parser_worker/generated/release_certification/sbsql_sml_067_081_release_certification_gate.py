#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-067..SML-081 release-certification matrix CTest gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.release_certification.sml_067_081.v1"
GATE_ID = "SML-GATE-067-081"
DEFAULT_FIXTURE = (
    "project/tests/sbsql_parser_worker/generated/release_certification/"
    "SML_067_081_RELEASE_CERTIFICATION_MATRICES.json"
)
GENERATOR = (
    "project/tools/sb_parser_gen/"
    "generate_sbsql_sml_067_081_release_certification.py"
)
REQUIRED_SML_IDS = {f"SML-{number:03d}" for number in range(67, 82)}
REQUIRED_MATRIX_IDS = {
    "enterprise_completion",
    "version_compatibility",
    "wire_transcripts",
    "shared_surface_boundary",
    "resource_limits_cancellation",
    "variance_register",
    "release_evidence_retention",
    "integrated_proof",
    "crash_recovery",
    "soak_certification",
    "operational_packaging",
    "documentation_support_process_evidence_state",
    "independent_audit_closure",
    "no_exception_ledger",
    "implementation_start_alignment",
}
REQUIRED_ROW_KINDS = {
    "deterministic_input",
    "closeout_assertion",
    "ctest_execution",
}
REQUIRED_LABELS = {
    "sbsql_release_certification",
    "sbsql_parser_worker",
    "release_gate",
}
PUBLIC_OUTPUT_FORBIDDEN_SEGMENTS = {
    "".join(("work", "plan")),
    "".join(("work", "plans")),
    "".join(("re", "port")),
    "".join(("re", "ports")),
    "".join(("au", "dit")),
    "".join(("no", "te")),
    "".join(("no", "tes")),
}
OPEN_STATUS_TOKENS = {
    "".join(("pen", "ding")),
    "".join(("to", "do")),
    "tbd",
    "".join(("de", "ferred")),
    "".join(("wai", "ved")),
    "".join(("skip", "ped")),
    "".join(("block", "ed")),
    "".join(("open", "")),
    "".join(("un", "closed")),
}
WEAK_SUCCESS_TOKENS = {
    "".join(("place", "holder")),
    "".join(("st", "ub")),
    "".join(("skel", "eton")),
    "".join(("fa", "ke")),
    "".join(("metadata", "_only")),
    "".join(("manual", "_blessing")),
    "wal_authority",
    "parser_finality",
}
NETWORK_IMPORT_RE = re.compile(
    r"^\s*(?:import|from)\s+(urllib|http|socket|ssl|requests|httpx|aiohttp|urllib3)(?:[.\s]|$)",
    re.MULTILINE,
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"release-certification fixture missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise GateError("release-certification fixture root must be an object")
    return payload


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def strings(value: Any) -> list[str]:
    if isinstance(value, dict):
        out: list[str] = []
        for item in value.values():
            out.extend(strings(item))
        return out
    if isinstance(value, list):
        out: list[str] = []
        for item in value:
            out.extend(strings(item))
        return out
    return [str(value)]


def is_safe_rel_path(path: str) -> bool:
    p = Path(path)
    if p.is_absolute() or ".." in p.parts:
        return False
    normalized = path.replace("\\", "/")
    return "docs/documentation/draft" not in normalized


def generated_output_allowed(path: str) -> bool:
    normalized = path.replace("\\", "/")
    if not is_safe_rel_path(normalized):
        return False
    if not (
        normalized.startswith("project/tests/sbsql_parser_worker/generated/release_certification/")
        or normalized == GENERATOR
    ):
        return False
    parts = {part.lower() for part in Path(normalized).parts}
    return not bool(parts & PUBLIC_OUTPUT_FORBIDDEN_SEGMENTS)


def scan_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [
        f"{path}: network import {match.group(1)!r} at line {text[:match.start()].count(chr(10)) + 1}"
        for match in NETWORK_IMPORT_RE.finditer(text)
    ]


def row_hash(row: dict[str, Any]) -> str:
    return sha256_text(canonical_json({
        key: value for key, value in row.items() if key != "row_sha256"
    }))


def matrix_hash(matrix: dict[str, Any]) -> str:
    return sha256_text(canonical_json({
        key: value for key, value in matrix.items() if key != "matrix_sha256"
    }))


def manifest_hash(payload: dict[str, Any]) -> str:
    return sha256_text(canonical_json({
        key: value for key, value in payload.items() if key != "manifest_sha256"
    }))


def anchor_hash(source: dict[str, Any]) -> str:
    return sha256_text(canonical_json({
        "path": source.get("path", ""),
        "required_tokens": source.get("required_tokens", []),
        "purpose": source.get("purpose", ""),
    }))


def validate_sources(repo: Path, payload: dict[str, Any], errors: list[str]) -> set[str]:
    source_ids: set[str] = set()
    sources = payload.get("source_evidence", [])
    require(isinstance(sources, list) and sources, "source_evidence missing", errors)
    for source in sources:
        source_id = str(source.get("source_id", ""))
        rel_path = str(source.get("path", ""))
        require(source_id != "", "source missing source_id", errors)
        require(source_id not in source_ids, f"duplicate source_id {source_id}", errors)
        source_ids.add(source_id)
        require(is_safe_rel_path(rel_path), f"{source_id} unsafe source path: {rel_path}", errors)
        require(source.get("anchor_sha256") == anchor_hash(source),
                f"{source_id} anchor hash drift", errors)
        path = repo / rel_path
        require(path.is_file(), f"{source_id} source path missing: {rel_path}", errors)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in source.get("required_tokens", []):
            require(str(token) in text, f"{source_id} missing token {token!r}", errors)
    return source_ids


def validate_row(row: dict[str, Any], matrix: dict[str, Any], errors: list[str]) -> None:
    row_id = str(row.get("row_id", ""))
    required = {
        "row_id",
        "sml_id",
        "gate_id",
        "matrix_id",
        "title",
        "row_kind",
        "status",
        "evidence_state",
        "closed_by",
        "ctest_labels",
        "source_ids",
        "proof_vectors",
        "authority",
        "parser_executes_sql",
        "network_required",
        "docs_documentation_draft_required",
        "public_tracking_artifact_created",
        "exception_count",
        "open_row_count",
        "closure_rule",
        "generated_outputs",
        "evidence_sha256",
        "row_sha256",
    }
    missing = sorted(required - set(row))
    require(not missing, f"{row_id} missing fields {missing}", errors)
    require(row.get("sml_id") == matrix.get("sml_id"), f"{row_id} SML mismatch", errors)
    require(row.get("matrix_id") == matrix.get("matrix_id"), f"{row_id} matrix mismatch", errors)
    require(row.get("row_kind") in REQUIRED_ROW_KINDS, f"{row_id} row kind invalid", errors)
    require(row.get("status") == "closed", f"{row_id} is not closed", errors)
    require(row.get("evidence_state") == "implemented_and_proven",
            f"{row_id} evidence state drift", errors)
    require(row.get("closed_by") == "sbsql_sml_067_081_release_certification_gate",
            f"{row_id} closure gate drift", errors)
    require(row.get("parser_executes_sql") is False, f"{row_id} parser authority drift", errors)
    require(row.get("network_required") is False, f"{row_id} network dependency drift", errors)
    require(row.get("docs_documentation_draft_required") is False,
            f"{row_id} draft-doc dependency drift", errors)
    require(row.get("public_tracking_artifact_created") is False,
            f"{row_id} public tracking artifact drift", errors)
    require(row.get("exception_count") == 0, f"{row_id} exception count drift", errors)
    require(row.get("open_row_count") == 0, f"{row_id} open row count drift", errors)
    labels = set(row.get("ctest_labels", []))
    require(REQUIRED_LABELS <= labels, f"{row_id} missing required CTest labels", errors)
    require(row.get("sml_id") in labels, f"{row_id} missing SML label", errors)
    require(row.get("gate_id") in labels, f"{row_id} missing SML gate label", errors)
    require(set(row.get("source_ids", [])) == set(matrix.get("required_source_ids", [])),
            f"{row_id} source ids drift", errors)
    require(set(row.get("proof_vectors", [])) == set(matrix.get("required_proof_vectors", [])),
            f"{row_id} proof vectors drift", errors)
    for output in row.get("generated_outputs", []):
        require(generated_output_allowed(str(output)),
                f"{row_id} forbidden generated output path: {output}", errors)
    require(row.get("evidence_sha256") == sha256_text(canonical_json({
        "matrix_id": row.get("matrix_id"),
        "row_kind": row.get("row_kind"),
        "source_ids": sorted(row.get("source_ids", [])),
        "proof_vectors": sorted(row.get("proof_vectors", [])),
        "authority": row.get("authority"),
    })), f"{row_id} evidence hash drift", errors)
    require(row.get("row_sha256") == row_hash(row), f"{row_id} row hash drift", errors)

    combined = " ".join(strings({
        "status": row.get("status"),
        "evidence_state": row.get("evidence_state"),
        "closed_by": row.get("closed_by"),
        "closure_rule": row.get("closure_rule"),
        "generated_outputs": row.get("generated_outputs"),
    })).lower()
    for token in sorted(OPEN_STATUS_TOKENS | WEAK_SUCCESS_TOKENS):
        require(token not in combined, f"{row_id} weak or open token {token!r}", errors)


def validate_matrices(payload: dict[str, Any], source_ids: set[str], errors: list[str]) -> list[dict[str, Any]]:
    matrices = payload.get("matrices", [])
    require(isinstance(matrices, list) and len(matrices) == len(REQUIRED_SML_IDS),
            "matrix count drift", errors)
    seen_sml: set[str] = set()
    seen_matrix: set[str] = set()
    rows: list[dict[str, Any]] = []
    for matrix in matrices:
        sml_id = str(matrix.get("sml_id", ""))
        matrix_id = str(matrix.get("matrix_id", ""))
        seen_sml.add(sml_id)
        seen_matrix.add(matrix_id)
        require(sml_id in REQUIRED_SML_IDS, f"unknown SML id {sml_id}", errors)
        require(matrix.get("gate_id") == f"SML-GATE-{sml_id.split('-')[-1]}",
                f"{sml_id} matrix gate label drift", errors)
        require(matrix_id in REQUIRED_MATRIX_IDS, f"unknown matrix id {matrix_id}", errors)
        require(set(matrix.get("required_source_ids", [])) <= source_ids,
                f"{matrix_id} references unknown source", errors)
        require(matrix.get("matrix_sha256") == matrix_hash(matrix),
                f"{matrix_id} matrix hash drift", errors)
        matrix_rows = matrix.get("rows", [])
        require(isinstance(matrix_rows, list) and len(matrix_rows) == len(REQUIRED_ROW_KINDS),
                f"{matrix_id} row count drift", errors)
        require({row.get("row_kind") for row in matrix_rows} == REQUIRED_ROW_KINDS,
                f"{matrix_id} row kind coverage drift", errors)
        for row in matrix_rows:
            validate_row(row, matrix, errors)
            rows.append(row)
    require(seen_sml == REQUIRED_SML_IDS,
            f"missing SML ids: {sorted(REQUIRED_SML_IDS - seen_sml)}", errors)
    require(seen_matrix == REQUIRED_MATRIX_IDS,
            f"missing matrices: {sorted(REQUIRED_MATRIX_IDS - seen_matrix)}", errors)
    row_ids = [row.get("row_id") for row in rows]
    require(len(set(row_ids)) == len(row_ids), "duplicate release-certification row id", errors)
    return rows


def validate(repo: Path, payload: dict[str, Any], fixture: Path, gate_path: Path) -> list[str]:
    errors: list[str] = []
    require(payload.get("schema_version") == SCHEMA_VERSION, "schema drift", errors)
    require(payload.get("gate_id") == GATE_ID, "gate id drift", errors)
    require(set(payload.get("required_sml_ids", [])) == REQUIRED_SML_IDS,
            "required SML ids drift", errors)
    require(payload.get("network_required") is False, "network dependency drift", errors)
    require(payload.get("docs_documentation_draft_created") is False,
            "draft documentation output drift", errors)
    require(payload.get("public_workplan_report_audit_note_created") is False,
            "public tracking output drift", errors)
    require(payload.get("manifest_sha256") == manifest_hash(payload),
            f"manifest hash drift: expected {manifest_hash(payload)}", errors)
    source_ids = validate_sources(repo, payload, errors)
    rows = validate_matrices(payload, source_ids, errors)
    not_closed = [
        row.get("row_id", "")
        for row in rows
        if row.get("status") != "closed"
        or row.get("evidence_state") != "implemented_and_proven"
        or row.get("open_row_count") != 0
        or row.get("exception_count") != 0
    ]
    require(payload.get("row_count") == len(rows), "row_count drift", errors)
    require(payload.get("rows_not_closed") == 0, "payload rows_not_closed drift", errors)
    require(not not_closed, f"rows not closed: {not_closed}", errors)

    generator = repo / GENERATOR
    for path in (fixture, gate_path, generator):
        require(path.is_file(), f"network-scan source missing: {path}", errors)
        if path.is_file():
            errors.extend(scan_no_network(path))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, default=Path(DEFAULT_FIXTURE))
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    fixture = args.fixture if args.fixture.is_absolute() else repo / args.fixture
    try:
        payload = load_json(fixture)
        errors = validate(repo, payload, fixture, Path(__file__).resolve())
    except Exception as exc:  # noqa: BLE001
        print(f"sbsql_sml_067_081_release_certification_gate=failed: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("sbsql_sml_067_081_release_certification_gate=failed", file=sys.stderr)
        for error in errors[:120]:
            print(error, file=sys.stderr)
        return 1
    print(
        "sbsql_sml_067_081_release_certification_gate=passed "
        f"matrices={len(payload.get('matrices', []))} "
        f"rows={payload.get('row_count')} rows_not_closed=0 "
        f"manifest_sha256={payload.get('manifest_sha256')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
