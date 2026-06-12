#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-029..SML-032 SQL feature closure proof gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_MANIFEST = (
    "project/tests/sbsql_parser_worker/fixtures/sml_029_032_feature_closure/"
    "SML_029_032_SQL_FEATURE_CLOSURE_MANIFEST.csv"
)
GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_sml_029_032_feature_closure_manifest.py"

REQUIRED_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "feature_family",
    "coverage_class",
    "proof_kind",
    "closure_status",
    "route",
    "sblr_operation",
    "expected_result_contract",
    "expected_result_hash",
    "expected_diagnostic_contract",
    "expected_diagnostic_hash",
    "transaction_visibility_class",
    "resource_limits",
    "parser_executes_sql",
    "parser_owns_finality",
    "finality_authority",
    "evidence_paths",
    "evidence_tokens",
]

EXPECTED_SML = {
    "SML-029": "cte_recursive_cte",
    "SML-030": "window_functions_frames",
    "SML-031": "join_forms_depth",
    "SML-032": "datatype_operator_cast_matrix",
}

REQUIRED_COVERAGE = {"positive", "negative", "boundary", "nested_depth", "refusal"}
REQUIRED_PROOF_KINDS = {"executable", "refusal"}
REQUIRED_STATUSES = {"closed_executable", "closed_refusal"}
ALLOWED_VISIBILITY = {
    "engine_owned_mga_snapshot_read",
    "engine_projection_transaction_context",
    "transaction_context_not_required",
    "no_transaction_visibility_parser_refusal",
}

FORBIDDEN_STATUS_TOKENS = (
    "".join(("pend", "ing")),
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "stub",
    "skeleton",
    "generated_only",
    "file_presence",
)

FORBIDDEN_AUTHORITY_TOKENS = (
    "parser_executes_sql=true",
    "parser_owns_finality=true",
    "".join(("parser", "_finality")),
    "sql_text_backend",
)

NETWORK_MODULE_NAMES = (
    "".join(("url", "lib")),
    "http",
    "".join(("so", "cket")),
    "ssl",
    "".join(("ft", "plib")),
    "".join(("telnet", "lib")),
    "".join(("web", "browser")),
    "".join(("re", "quests")),
    "httpx",
    "aiohttp",
)

NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    + "|".join(re.escape(name) for name in NETWORK_MODULE_NAMES)
    + r")(?:[\.\s]|$)",
    re.MULTILINE,
)

HASH_RE = re.compile(r"^[0-9a-f]{64}$")


class GateError(AssertionError):
    pass


def stable_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def split_tokens(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def fail(message: str) -> None:
    raise GateError(message)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def read_manifest(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"manifest missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != REQUIRED_COLUMNS:
            fail(f"manifest columns drifted: {reader.fieldnames}")
        rows = list(reader)
    if not rows:
        fail("manifest has no rows")
    return rows


def scan_no_network(path: Path) -> list[str]:
    text = read_text(path)
    findings: list[str] = []
    for match in NETWORK_MODULE_RE.finditer(text):
        line_num = text[: match.start()].count("\n") + 1
        findings.append(f"{path}:{line_num}: forbidden network import {match.group(1)!r}")
    return findings


def validate_determinism(repo: Path, manifest: Path) -> list[str]:
    errors: list[str] = []
    generator = repo / GENERATOR
    if not generator.is_file():
        return [f"generator missing: {GENERATOR}"]
    errors.extend(scan_no_network(generator))
    with tempfile.TemporaryDirectory(prefix="sml_029_032_manifest_") as tmp:
        generated = Path(tmp) / manifest.name
        result = subprocess.run(
            [sys.executable, str(generator), "--output", str(generated)],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            return [
                "manifest regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:240]}"
            ]
        if generated.read_bytes() != manifest.read_bytes():
            errors.append("tracked SML-029..032 manifest differs from deterministic regeneration")
    return errors


def parse_limits(value: str, context: str, errors: list[str]) -> dict[str, str]:
    limits: dict[str, str] = {}
    for item in split_tokens(value):
        if "=" not in item:
            errors.append(f"{context}: malformed resource limit {item!r}")
            continue
        key, raw_value = item.split("=", 1)
        if key in limits:
            errors.append(f"{context}: duplicate resource limit {key}")
        limits[key] = raw_value
    for required in ("network", "parser_execution"):
        if limits.get(required) != "0":
            errors.append(f"{context}: {required} must be 0")
    for key, raw_value in limits.items():
        if key.startswith("max_") and not raw_value.isdigit():
            errors.append(f"{context}: {key} must be numeric, found {raw_value!r}")
    if not any(key.startswith("max_") for key in limits):
        errors.append(f"{context}: missing max_* resource limit")
    return limits


def validate_row_shape(row: dict[str, str], errors: list[str]) -> None:
    row_id = row.get("row_id", "<missing>")
    for column in REQUIRED_COLUMNS:
        if not row.get(column):
            errors.append(f"{row_id}: missing {column}")
    if row["sml_id"] not in EXPECTED_SML:
        errors.append(f"{row_id}: unexpected sml_id {row['sml_id']}")
    elif row["feature_family"] != EXPECTED_SML[row["sml_id"]]:
        errors.append(f"{row_id}: feature family mismatch for {row['sml_id']}")
    if row["gate_id"] != row["sml_id"].replace("SML-", "SML-GATE-"):
        errors.append(f"{row_id}: gate_id mismatch")
    if row["coverage_class"] not in REQUIRED_COVERAGE:
        errors.append(f"{row_id}: unexpected coverage_class {row['coverage_class']}")
    if row["proof_kind"] not in REQUIRED_PROOF_KINDS:
        errors.append(f"{row_id}: unexpected proof_kind {row['proof_kind']}")
    if row["closure_status"] not in REQUIRED_STATUSES:
        errors.append(f"{row_id}: unexpected closure_status {row['closure_status']}")
    if row["proof_kind"] == "executable" and row["closure_status"] != "closed_executable":
        errors.append(f"{row_id}: executable row must use closed_executable")
    if row["proof_kind"] == "refusal" and row["closure_status"] != "closed_refusal":
        errors.append(f"{row_id}: refusal row must use closed_refusal")
    if row["transaction_visibility_class"] not in ALLOWED_VISIBILITY:
        errors.append(f"{row_id}: unsupported transaction visibility class")
    if row["parser_executes_sql"] != "false":
        errors.append(f"{row_id}: parser_executes_sql must be false")
    if row["parser_owns_finality"] != "false":
        errors.append(f"{row_id}: parser_owns_finality must be false")
    if "parser" in row["finality_authority"] and "refusal" not in row["finality_authority"]:
        errors.append(f"{row_id}: finality authority cannot be held by parser")
    if "SBLR_" not in row["sblr_operation"]:
        errors.append(f"{row_id}: sblr_operation missing opcode")

    for field in ("closure_status", "proof_kind"):
        lowered = row[field].lower()
        for token in FORBIDDEN_STATUS_TOKENS:
            if token in lowered:
                errors.append(f"{row_id}: forbidden status token {token!r}")
    for field in ("expected_result_contract", "expected_diagnostic_contract", "route"):
        lowered = row[field].lower()
        for token in FORBIDDEN_STATUS_TOKENS:
            if token in lowered:
                errors.append(f"{row_id}: forbidden token {token!r} in {field}")
        for token in FORBIDDEN_AUTHORITY_TOKENS:
            if token in lowered:
                errors.append(f"{row_id}: forbidden authority token {token!r} in {field}")

    if not HASH_RE.fullmatch(row["expected_result_hash"]):
        errors.append(f"{row_id}: expected_result_hash missing or invalid")
    elif stable_hash(row["expected_result_contract"]) != row["expected_result_hash"]:
        errors.append(f"{row_id}: expected_result_hash mismatch")
    if not HASH_RE.fullmatch(row["expected_diagnostic_hash"]):
        errors.append(f"{row_id}: expected_diagnostic_hash missing or invalid")
    elif stable_hash(row["expected_diagnostic_contract"]) != row["expected_diagnostic_hash"]:
        errors.append(f"{row_id}: expected_diagnostic_hash mismatch")
    if row["proof_kind"] == "executable" and row["expected_result_contract"] == "not_applicable":
        errors.append(f"{row_id}: executable row must declare a result contract")
    if row["proof_kind"] == "refusal" and row["expected_diagnostic_contract"] == "none":
        errors.append(f"{row_id}: refusal row must declare a diagnostic contract")

    parse_limits(row["resource_limits"], row_id, errors)


def validate_evidence(repo: Path, row: dict[str, str], errors: list[str]) -> None:
    row_id = row["row_id"]
    rel_paths = split_tokens(row["evidence_paths"])
    if not rel_paths:
        errors.append(f"{row_id}: missing evidence paths")
        return
    text_parts: list[str] = []
    for rel in rel_paths:
        if not rel.startswith("project/tests/sbsql_parser_worker/"):
            errors.append(f"{row_id}: evidence path outside allowed test scope: {rel}")
            continue
        path = repo / rel
        if not path.is_file():
            errors.append(f"{row_id}: evidence path missing: {rel}")
            continue
        text_parts.append(read_text(path))
    combined = "\n".join(text_parts).replace('\\"', '"')
    for token in split_tokens(row["evidence_tokens"]):
        if token not in combined:
            errors.append(f"{row_id}: evidence token not found: {token}")


def validate_manifest(repo: Path, rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    row_ids = [row["row_id"] for row in rows]
    if len(row_ids) != len(set(row_ids)):
        errors.append("duplicate row_id in manifest")

    by_sml: dict[str, list[dict[str, str]]] = {sml_id: [] for sml_id in EXPECTED_SML}
    for row in rows:
        validate_row_shape(row, errors)
        validate_evidence(repo, row, errors)
        if row["sml_id"] in by_sml:
            by_sml[row["sml_id"]].append(row)

    for sml_id, feature_family in EXPECTED_SML.items():
        sml_rows = by_sml[sml_id]
        if not sml_rows:
            errors.append(f"{sml_id}: no rows")
            continue
        proof_kinds = {row["proof_kind"] for row in sml_rows}
        coverage = {row["coverage_class"] for row in sml_rows}
        if "executable" not in proof_kinds:
            errors.append(f"{sml_id}: missing executable proof row")
        if "refusal" not in proof_kinds:
            errors.append(f"{sml_id}: missing refusal proof row")
        if "positive" not in coverage:
            errors.append(f"{sml_id}: missing positive coverage")
        if "refusal" not in coverage:
            errors.append(f"{sml_id}: missing refusal coverage")
        if not any(row["feature_family"] == feature_family for row in sml_rows):
            errors.append(f"{sml_id}: feature family {feature_family} missing")

    observed_coverage = {row["coverage_class"] for row in rows}
    missing_coverage = REQUIRED_COVERAGE - observed_coverage
    if missing_coverage:
        errors.append(f"manifest missing coverage classes: {sorted(missing_coverage)}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    manifest = Path(args.manifest)
    if not manifest.is_absolute():
        manifest = repo / manifest

    try:
        rows = read_manifest(manifest)
        errors = []
        errors.extend(scan_no_network(Path(__file__).resolve()))
        errors.extend(validate_determinism(repo, manifest))
        errors.extend(validate_manifest(repo, rows))
    except Exception as exc:  # noqa: BLE001 - gate output should stay explicit.
        print("sbsql_sml_029_032_feature_closure_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print("sbsql_sml_029_032_feature_closure_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("sbsql_sml_029_032_feature_closure_gate=passed")
    print(f"manifest_rows={len(rows)}")
    print("sml_ids=" + ",".join(sorted(EXPECTED_SML)))
    print("execution_plan_runtime_dependency=none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
