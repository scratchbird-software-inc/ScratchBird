#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSQL missing-functionality scope and registry allocation gates.

The execution_plan directories are intentionally not read here. Public release proof
must live under project/tests and verify real source/spec tokens instead of
accepting a planning row as completion evidence.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


EXPECTED_FAMILIES = {
    "migration_management": "SBSQL-MISS-GATE-004",
    "temporal_bitemporal": "SBSQL-MISS-GATE-005",
    "structured_types": "SBSQL-MISS-GATE-006",
    "transaction_lock_compatibility": "SBSQL-MISS-GATE-011",
    "ddl_catalog_gaps": "SBSQL-MISS-GATE-007",
    "dml_upsert_variants": "SBSQL-MISS-GATE-008",
    "bulk_import_export": "SBSQL-MISS-GATE-009",
    "native_system_variables": "SBSQL-MISS-GATE-010",
    "donor_variable_translation": "SBSQL-MISS-GATE-010",
    "last_day_builtin": "SBSQL-MISS-GATE-012",
    "acceleration_management": "SBSQL-MISS-GATE-012",
    "donor_command_function_backfill": "SBSQL-MISS-GATE-014",
}

REQUIRED_COLUMNS = {
    "family",
    "gate_id",
    "accepted_surfaces",
    "sbsql_families",
    "sblr_families",
    "runtime_tokens",
    "spec_tokens",
    "proof_paths",
    "notes",
}

BANNED_COMPLETION_MARKERS = {
    "stub",
    "skeleton",
    "skip",
    "todo",
    "tbd",
    "defer",
}

RUNTIME_SUFFIXES = {".cpp", ".hpp", ".h", ".inc", ".yaml", ".yml", ".csv", ".md"}
SPEC_SUFFIXES = {".yaml", ".yml", ".csv", ".md"}


def split_tokens(value: str) -> list[str]:
    return [token.strip() for token in value.split(";") if token.strip()]


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8", errors="ignore")


def corpus(root: Path, suffixes: set[str]) -> str:
    pieces: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in suffixes:
            continue
        pieces.append(read_text(path))
    return "\n".join(pieces)


def load_manifest(repo: Path) -> list[dict[str, str]]:
    manifest = repo / "project/tests/sbsql_parser_worker/fixtures/missing_functionality_closure/SCOPE_REGISTRY_MANIFEST.csv"
    with manifest.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        fieldnames = set(reader.fieldnames or [])
        missing = REQUIRED_COLUMNS - fieldnames
        if missing:
            raise ValueError(f"manifest missing columns: {sorted(missing)}")
        return list(reader)


def validate_scope(repo: Path, rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    by_family = {row["family"]: row for row in rows}
    if set(by_family) != set(EXPECTED_FAMILIES):
        errors.append(
            "scope family set mismatch: "
            f"missing={sorted(set(EXPECTED_FAMILIES) - set(by_family))} "
            f"extra={sorted(set(by_family) - set(EXPECTED_FAMILIES))}"
        )

    for family, gate_id in EXPECTED_FAMILIES.items():
        row = by_family.get(family)
        if row is None:
            continue
        if row["gate_id"] != gate_id:
            errors.append(f"{family}: expected {gate_id}, found {row['gate_id']}")
        for column in REQUIRED_COLUMNS - {"family", "gate_id", "sbsql_families"}:
            if not row[column].strip():
                errors.append(f"{family}: empty {column}")
        for column, value in row.items():
            lowered = value.lower()
            for marker in BANNED_COMPLETION_MARKERS:
                if marker in lowered:
                    errors.append(f"{family}: banned completion marker {marker!r} in {column}")
        for rel in split_tokens(row["proof_paths"]):
            if not rel.startswith("project/tests/"):
                errors.append(f"{family}: proof path outside project/tests: {rel}")
            if not (repo / rel).is_file():
                errors.append(f"{family}: missing proof path: {rel}")
    return errors


def validate_registry(repo: Path, rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    runtime = corpus(repo / "project/src", RUNTIME_SUFFIXES)
    spec = corpus(repo / "public_contract_snapshot", SPEC_SUFFIXES)
    tests = corpus(repo / "project/tests/sbsql_parser_worker", RUNTIME_SUFFIXES | SPEC_SUFFIXES | {".py", ".txt"})

    for row in rows:
        family = row["family"]
        for token in split_tokens(row["runtime_tokens"]):
            if token not in runtime:
                errors.append(f"{family}: runtime token not found: {token}")
        for token in split_tokens(row["spec_tokens"]):
            if token not in spec:
                errors.append(f"{family}: spec token not found: {token}")
        for token in split_tokens(row["sblr_families"]):
            if token and token not in runtime and token not in spec:
                errors.append(f"{family}: SBLR family not found in runtime/spec: {token}")
        for token in split_tokens(row["sbsql_families"]):
            if token and token not in runtime and token not in tests:
                errors.append(f"{family}: SBSQL family not found in runtime/tests: {token}")

    ast = read_text(repo / "project/src/parsers/sbsql_worker/ast/ast.cpp")
    surface_registry = read_text(repo / "project/src/parsers/sbsql_worker/registry/sbsql_surface_registry.cpp")
    lowering = read_text(repo / "project/src/parsers/sbsql_worker/lowering/lowering.cpp")
    generated_registry = read_text(repo / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp")

    if "sblr.dml.upsert.v3" in ast or "sblr.dml.upsert.v3" in surface_registry:
        errors.append("stale sblr.dml.upsert.v3 declaration remains in hand-written parser registry/AST")
    if '{"sbsql.dml.upsert.v3", "upsert", "sblr.dml.operation.v3", 3}' not in surface_registry:
        errors.append("UPSERT surface registry does not declare canonical sblr.dml.operation.v3 route")
    if 'ast.registry_family = "sbsql.dml.upsert.v3";' not in ast:
        errors.append("UPSERT AST registry family declaration missing")
    if 'ast.operation_family = "sblr.dml.operation.v3";' not in ast:
        errors.append("UPSERT AST operation family does not use sblr.dml.operation.v3")
    if 'info.operation_id = "dml.merge_rows";' not in lowering or 'info.surface_variant = "upsert";' not in lowering:
        errors.append("UPSERT lowering no longer normalizes through dml.merge_rows with surface_variant=upsert")
    if '"upsert", "canonical_surface", "dml"' not in generated_registry:
        errors.append("generated registry missing native UPSERT canonical surface row")
    if '"upsert_statement", "grammar_production", "dml"' not in generated_registry:
        errors.append("generated registry missing UPSERT grammar production row")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--mode", choices=("scope", "registry"), required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    try:
        rows = load_manifest(repo)
        errors = validate_scope(repo, rows)
        if args.mode == "registry":
            errors.extend(validate_registry(repo, rows))
    except Exception as exc:  # noqa: BLE001 - gate output should be explicit.
        print(f"sbsql_missing_functionality_{args.mode}_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print(f"sbsql_missing_functionality_{args.mode}_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    if args.mode == "scope":
        print("sbsql_missing_functionality_scope_freeze_gate=passed")
        print(f"scope_family_count={len(rows)}")
    else:
        print("sbsql_missing_functionality_registry_allocation_gate=passed")
        print("upsert_canonical_route=dml.merge_rows/SBLR_DML_MERGE_ROWS")
    print("execution_plan_runtime_dependency=none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
