#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate explicit, conservative public capability maturity claims."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys


MATURITY_LEVELS = (
    "codec_contract",
    "routing",
    "logical_implementation",
    "physical_implementation",
    "durability_proven",
    "production_qualified",
)
MATURITY_RANK = {level: rank for rank, level in enumerate(MATURITY_LEVELS)}

MATRIX_PATH = Path("project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml")
REGISTRY_PATH = Path("project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml")
MATURITY_DOC_PATH = Path("project/docs/public_api/IMPLEMENTATION_MATURITY.md")
SBLR_NAMING_DOC_PATH = Path("project/src/engine/sblr/README.md")


def parse_rows(path: Path, item_field: str) -> tuple[list[dict[str, str]], list[str]]:
    """Parse the flat operation rows without adding a CI YAML dependency."""

    rows: list[dict[str, str]] = []
    errors: list[str] = []
    current: dict[str, str] | None = None
    item = re.compile(rf"^  - {re.escape(item_field)}:\s*(.+?)\s*$")
    field = re.compile(r"^    ([A-Za-z0-9_]+):\s*(.*?)\s*$")
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        start = item.match(line)
        if start:
            if current is not None:
                rows.append(current)
            current = {item_field: start.group(1).strip('"\'')}
            continue
        match = field.match(line)
        if match and current is not None:
            key, value = match.groups()
            if key in current:
                errors.append(f"{path}:{line_number}: duplicate row field {key}")
            current[key] = value.strip('"\'')
    if current is not None:
        rows.append(current)
    return rows, errors


def existing_evidence_paths(repo_root: Path, evidence: str) -> bool:
    paths = [part.strip() for part in evidence.split(";") if part.strip()]
    return bool(paths) and all((repo_root / path).is_file() for path in paths)


def validate(repo_root: Path) -> tuple[list[str], Counter[str]]:
    errors: list[str] = []
    matrix_path = repo_root / MATRIX_PATH
    registry_path = repo_root / REGISTRY_PATH
    for path in (matrix_path, registry_path):
        if not path.is_file():
            errors.append(f"missing maturity authority: {path.relative_to(repo_root)}")
    if errors:
        return errors, Counter()

    matrix_text = matrix_path.read_text(encoding="utf-8")
    for token in (
        "maturity_authority: implementation_maturity",
        "maturity_order_low_to_high:",
        "runtime_suffix_is_execution_evidence: false",
    ):
        if token not in matrix_text:
            errors.append(f"matrix maturity policy missing {token}")
    expected_order = "\n".join(
        ["  maturity_order_low_to_high:"]
        + [f"    - {level}" for level in MATURITY_LEVELS]
    )
    if expected_order not in matrix_text:
        errors.append("matrix maturity order differs from the public six-level taxonomy")

    matrix_rows, parse_errors = parse_rows(matrix_path, "sblr_operation")
    registry_rows, registry_parse_errors = parse_rows(registry_path, "operation_id")
    errors.extend(parse_errors)
    errors.extend(registry_parse_errors)
    if len(matrix_rows) < 100 or len(registry_rows) < 100:
        errors.append(
            f"operation inventory unexpectedly small: matrix={len(matrix_rows)} "
            f"registry={len(registry_rows)}"
        )

    matrix_by_id: dict[str, dict[str, str]] = {}
    for row in matrix_rows:
        operation_id = row.get("api_operation_id", "")
        if not operation_id:
            errors.append("matrix row missing api_operation_id")
            continue
        if operation_id in matrix_by_id:
            errors.append(f"duplicate matrix operation {operation_id}")
        matrix_by_id[operation_id] = row

    registry_by_id: dict[str, dict[str, str]] = {}
    for row in registry_rows:
        operation_id = row.get("operation_id", "")
        if not operation_id:
            errors.append("registry row missing operation_id")
            continue
        if operation_id in registry_by_id:
            errors.append(f"duplicate registry operation {operation_id}")
        registry_by_id[operation_id] = row

    if set(matrix_by_id) != set(registry_by_id):
        errors.append(
            "matrix/registry operation inventory differs: "
            f"matrix_only={sorted(set(matrix_by_id) - set(registry_by_id))[:10]} "
            f"registry_only={sorted(set(registry_by_id) - set(matrix_by_id))[:10]}"
        )

    counts: Counter[str] = Counter()
    for operation_id, matrix_row in matrix_by_id.items():
        maturity = matrix_row.get("implementation_maturity", "")
        if maturity not in MATURITY_RANK:
            errors.append(f"{operation_id}: invalid or missing implementation_maturity {maturity!r}")
            continue
        counts[maturity] += 1
        registry_maturity = registry_by_id.get(operation_id, {}).get(
            "implementation_maturity", ""
        )
        if registry_maturity != maturity:
            errors.append(
                f"{operation_id}: maturity differs between matrix={maturity!r} "
                f"and registry={registry_maturity!r}"
            )

        if maturity == "codec_contract":
            codec_component = matrix_row.get("codec_component", "")
            if matrix_row.get("codec_component_role") != "request_descriptor_result_codec":
                errors.append(f"{operation_id}: codec contract missing explicit component role")
            if not codec_component or not (
                repo_root / "project/src" / codec_component
            ).is_file():
                errors.append(
                    f"{operation_id}: codec contract must identify an existing codec component"
                )

        if MATURITY_RANK[maturity] >= MATURITY_RANK["physical_implementation"]:
            evidence = matrix_row.get("maturity_evidence", "")
            if not existing_evidence_paths(repo_root, evidence):
                errors.append(
                    f"{operation_id}: physical claim maturity_evidence must be existing test paths"
                )
            else:
                function_name = registry_by_id.get(operation_id, {}).get("function_name", "")
                evidence_text = "\n".join(
                    (repo_root / path.strip()).read_text(encoding="utf-8", errors="replace")
                    for path in evidence.split(";")
                    if path.strip()
                )
                if not function_name or function_name not in evidence_text:
                    errors.append(
                        f"{operation_id}: physical maturity evidence does not exercise {function_name!r}"
                    )

        # Refusals and provider boundaries are routing claims, not completed
        # implementations. This prevents legacy readiness labels from being
        # translated into a higher maturity.
        legacy = matrix_row.get("current_implementation_status", "")
        if ("fail_closed" in legacy or legacy == "exact_profile_refusal") and maturity != "routing":
            errors.append(f"{operation_id}: fail-closed/refusal row must remain routing")

        if MATURITY_RANK[maturity] >= MATURITY_RANK["durability_proven"]:
            boundary = matrix_row.get("restart_boundary", "")
            if not boundary:
                errors.append(f"{operation_id}: durable claim missing restart_boundary")
        if maturity == "production_qualified":
            evidence = matrix_row.get("qualification_evidence", "")
            decision = matrix_row.get("release_qualification_decision", "")
            if not existing_evidence_paths(repo_root, evidence):
                errors.append(
                    f"{operation_id}: production claim qualification_evidence must be existing file paths"
                )
            if decision != "approved":
                errors.append(f"{operation_id}: production claim requires approved release decision")

    create_table = matrix_by_id.get("ddl.create_table", {})
    expected_create_table = {
        "codec_component": "engine/sblr/sblr_ddl_create_table_runtime.cpp",
        "codec_component_role": "request_descriptor_result_codec",
        "execution_path": "engine/sblr/sblr_dispatch.cpp -> engine/internal_api/ddl/EngineCreateTable",
    }
    for key, expected in expected_create_table.items():
        if create_table.get(key) != expected:
            errors.append(f"ddl.create_table: {key} must be {expected!r}")
    if create_table.get("implementation_maturity") != "physical_implementation":
        errors.append("ddl.create_table: codec carrier must be distinguished from physical capability")

    create_fdw = matrix_by_id.get("engine.op.ddl_create_fdw", {})
    expected_create_fdw = {
        "implementation_maturity": "codec_contract",
        "codec_component": "engine/sblr/sblr_ddl_create_fdw_runtime.cpp",
        "codec_component_role": "request_descriptor_result_codec",
    }
    for key, expected in expected_create_fdw.items():
        if create_fdw.get(key) != expected:
            errors.append(f"engine.op.ddl_create_fdw: {key} must be {expected!r}")

    required_docs = {
        MATURITY_DOC_PATH: (
            *MATURITY_LEVELS,
            "The `runtime` suffix",
        ),
        SBLR_NAMING_DOC_PATH: (
            "does not assert",
            "request/descriptor/result codec",
            "internal_api::EngineCreateTable",
        ),
        Path("KNOWN_LIMITATIONS.md"): (
            "explicit implementation maturity",
            "filename is not evidence",
        ),
    }
    for relative_path, snippets in required_docs.items():
        path = repo_root / relative_path
        if not path.is_file():
            errors.append(f"missing maturity documentation: {relative_path}")
            continue
        text = path.read_text(encoding="utf-8")
        for snippet in snippets:
            if snippet not in text:
                errors.append(f"{relative_path}: missing maturity boundary text {snippet!r}")

    return errors, counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    errors, counts = validate(repo_root)
    if errors:
        print("implementation_maturity_gate=failed", file=sys.stderr)
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    summary = " ".join(f"{level}={counts[level]}" for level in MATURITY_LEVELS)
    print(f"implementation_maturity_gate=passed {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
