#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Compile the shared driver full-surface SBSQL suite into a run-specific chain."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any

from exhaustive_generators import source_summary


SUITE_ROOT = Path(__file__).resolve().parent
MANIFEST_NAME = "manifest.json"
EXPECTED_DIR = "expected"
BUILTIN_FIXTURE_ROOT_REL = Path("project/tests/sbsql_parser_worker/generated/full_surface")


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[5]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def ensure_output_policy(repo_root: Path, output_root: Path) -> None:
    """Forbid generated outputs inside tracked source areas."""
    resolved_repo = repo_root.resolve()
    resolved_output = output_root.resolve()
    try:
        rel = resolved_output.relative_to(resolved_repo)
    except ValueError:
        return
    if not rel.parts or rel.parts[0] != "build":
        raise ValueError(
            f"output root inside the repository must be under build/: {resolved_output}"
        )


def substitution_map(args: argparse.Namespace) -> dict[str, str]:
    artifact_root = args.artifact_root
    if artifact_root is None:
        artifact_root = args.output_root / "artifacts"
    return {
        "__SB_NAMESPACE__": args.namespace,
        "__SB_DRIVER__": args.driver,
        "__SB_RUN_ID__": args.run_id,
        "__SB_ROUTE__": args.route,
        "__SB_PARSER_MODE__": args.parser_mode,
        "__SB_PAGE_SIZE__": args.page_size,
        "__SB_ARTIFACT_ROOT__": str(artifact_root.resolve()),
    }


def apply_substitutions(text: str, values: dict[str, str]) -> str:
    rendered = text
    for placeholder, value in values.items():
        rendered = rendered.replace(placeholder, value)
    return rendered


def copy_expected_files(suite_root: Path, output_root: Path) -> list[str]:
    expected_root = suite_root / EXPECTED_DIR
    output_expected = output_root / EXPECTED_DIR
    output_expected.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    for source in sorted(expected_root.glob("*.json")):
        target = output_expected / source.name
        text = source.read_text(encoding="utf-8")
        target.write_text(text, encoding="utf-8")
        copied.append(str(target))
    return copied


def copy_generated_expected_files(suite_root: Path, output_root: Path) -> list[str]:
    source_root = suite_root / "generated" / EXPECTED_DIR
    if not source_root.is_dir():
        return []
    output_expected = output_root / EXPECTED_DIR
    copied: list[str] = []
    for source in sorted(path for path in source_root.rglob("*") if path.is_file()):
        rel = source.relative_to(source_root)
        target = output_expected / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        copied.append(str(target))
    return copied


def generated_summary_from_manifest(repo_root: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    summary = source_summary(repo_root)
    generated_scripts = [
        item for item in manifest.get("scripts", [])
        if isinstance(item, dict) and str(item.get("script_id", "")).startswith("SBDFS-1")
    ]
    summary["generated_script_count"] = len(generated_scripts)
    summary["generated_case_count"] = sum(
        int(item.get("generated_case_count", 0)) for item in generated_scripts
    )
    minimums = manifest.get("exhaustive_source_minimums", {})
    if isinstance(minimums, dict):
        for key in ("generated_datatype_rows", "generated_index_cases", "generated_cast_cases"):
            summary[key] = int(minimums.get(key, summary.get(key, 0)))
    return summary


def sql_string(value: str | None) -> str:
    if value is None or value == "":
        return "NULL"
    return "'" + value.replace("'", "''") + "'"


def append_batched_values_insert(
    lines: list[str],
    table_name: str,
    rows: list[str],
    *,
    columns: list[str] | None = None,
    batch_size: int = 256,
) -> int:
    if not rows:
        return 0
    column_clause = ""
    if columns:
        column_clause = " (" + ", ".join(columns) + ")"
    statement_count = 0
    for offset in range(0, len(rows), batch_size):
        batch = rows[offset:offset + batch_size]
        lines.append(f"INSERT INTO {table_name}{column_clause} VALUES")
        for index, row in enumerate(batch):
            suffix = ";" if index + 1 == len(batch) else ","
            lines.append(f"    {row}{suffix}")
        statement_count += 1
    return statement_count


def read_builtin_fixture_rows(repo_root: Path) -> tuple[list[dict[str, str]], list[dict[str, Any]]]:
    fixture_root = repo_root / BUILTIN_FIXTURE_ROOT_REL
    rows: list[dict[str, str]] = []
    sources: list[dict[str, Any]] = []
    for path in sorted(fixture_root.glob("*.csv")):
        with path.open(newline="", encoding="utf-8") as handle:
            file_rows = list(csv.DictReader(handle))
        sources.append(
            {
                "path": str(path.relative_to(repo_root)),
                "rows": len(file_rows),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
        for row in file_rows:
            expected_value = row.get("expected_result_value", "")
            if expected_value == "":
                expected_value = row.get("expected_result_json", "")
            rows.append(
                {
                    "fixture_id": row.get("fixture_id", ""),
                    "surface_id": row.get("surface_id", ""),
                    "function_id": row.get("function_id", ""),
                    "canonical_builtin_id": row.get("canonical_builtin_id", ""),
                    "case_kind": row.get("case_kind", ""),
                    "arguments_json": row.get("arguments_json", ""),
                    "expected_result_value": expected_value,
                    "expected_result_descriptor": row.get("expected_result_descriptor", ""),
                    "expected_diagnostic_code": row.get("expected_diagnostic_code", ""),
                    "source_file": path.name,
                }
            )
    return rows, sources


def copy_builtin_fixture_csvs(repo_root: Path, output_root: Path) -> list[str]:
    source_root = repo_root / BUILTIN_FIXTURE_ROOT_REL
    target_root = output_root / EXPECTED_DIR / "builtin_function_fixtures"
    target_root.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    for source in sorted(source_root.glob("*.csv")):
        target = target_root / source.name
        target.write_bytes(source.read_bytes())
        copied.append(str(target))
    return copied


def build_generated_builtin_fixture_script(
    *,
    namespace: str,
    rows: list[dict[str, str]],
    manifest: dict[str, Any],
) -> str:
    expected = manifest.get("builtin_fixture_minimums", {})
    surface_ids = {row["surface_id"] for row in rows if row["surface_id"]}
    diagnostic_rows = [row for row in rows if row["expected_diagnostic_code"]]
    lines = [
        "-- script_id: SBDFS-055",
        "-- Purpose: generated expected-result fixture manifest for every driver-native built-in test lane.",
        "",
        f"CREATE TABLE {namespace}.builtin_fixture_manifest (",
        "    fixture_id VARCHAR(160) PRIMARY KEY,",
        "    surface_id VARCHAR(64) NOT NULL,",
        "    function_id VARCHAR(192),",
        "    canonical_builtin_id VARCHAR(192),",
        "    case_kind VARCHAR(96),",
        "    arguments_json TEXT,",
        "    expected_result_value TEXT,",
        "    expected_result_descriptor VARCHAR(96),",
        "    expected_diagnostic_code VARCHAR(128),",
        "    source_file VARCHAR(160) NOT NULL",
        ");",
        "",
    ]
    manifest_rows: list[str] = []
    for row in rows:
        values = [
            sql_string(row["fixture_id"]),
            sql_string(row["surface_id"]),
            sql_string(row["function_id"]),
            sql_string(row["canonical_builtin_id"]),
            sql_string(row["case_kind"]),
            sql_string(row["arguments_json"]),
            sql_string(row["expected_result_value"]),
            sql_string(row["expected_result_descriptor"]),
            sql_string(row["expected_diagnostic_code"]),
            sql_string(row["source_file"]),
        ]
        manifest_rows.append(f"({', '.join(values)})")
    append_batched_values_insert(
        lines,
        f"{namespace}.builtin_fixture_manifest",
        manifest_rows,
        columns=[
            "fixture_id",
            "surface_id",
            "function_id",
            "canonical_builtin_id",
            "case_kind",
            "arguments_json",
            "expected_result_value",
            "expected_result_descriptor",
            "expected_diagnostic_code",
            "source_file",
        ],
        batch_size=128,
    )
    lines.extend(
        [
            "",
            "SELECT 'SBDFS-055-001' AS assertion_id,",
            "       COUNT(*) AS actual_fixture_rows,",
            f"       {int(expected.get('fixture_rows', len(rows)))} AS expected_fixture_rows",
            f"FROM {namespace}.builtin_fixture_manifest;",
            "",
            "SELECT 'SBDFS-055-002' AS assertion_id,",
            "       COUNT(DISTINCT surface_id) AS actual_fixture_surface_ids,",
            f"       {int(expected.get('fixture_surface_ids', len(surface_ids)))} AS expected_fixture_surface_ids",
            f"FROM {namespace}.builtin_fixture_manifest;",
            "",
            "SELECT 'SBDFS-055-003' AS assertion_id,",
            "       COUNT(*) AS actual_diagnostic_rows,",
            f"       {int(expected.get('diagnostic_rows', len(diagnostic_rows)))} AS expected_diagnostic_rows",
            f"FROM {namespace}.builtin_fixture_manifest",
            "WHERE expected_diagnostic_code IS NOT NULL;",
            "",
            "SELECT 'SBDFS-055-004' AS assertion_id,",
            "       COUNT(DISTINCT l.surface_id) AS actual_released_function_operator_fixture_surface_ids,",
            "       "
            f"{int(expected.get('release_function_operator_fixture_surface_ids', 0))} "
            "AS expected_released_function_operator_fixture_surface_ids",
            "FROM sys.parser.language_elements AS l",
            f"JOIN {namespace}.builtin_fixture_manifest AS f ON f.surface_id = l.surface_id",
            "WHERE l.element_kind IN ('surface_function', 'surface_operator')",
            "  AND l.support_state = 'release_supported';",
            "",
        ]
    )
    return "\n".join(lines)


def compile_suite(
    *,
    repo_root: Path,
    suite_root: Path,
    output_root: Path,
    values: dict[str, str],
) -> dict[str, Any]:
    repo_root = repo_root.resolve()
    suite_root = suite_root.resolve()
    output_root = output_root.resolve()
    ensure_output_policy(repo_root, output_root)

    manifest = load_json(suite_root / MANIFEST_NAME)
    output_scripts = output_root / "scripts"
    output_scripts.mkdir(parents=True, exist_ok=True)
    artifact_root = Path(values.get("__SB_ARTIFACT_ROOT__", str(output_root / "artifacts")))
    artifact_root.mkdir(parents=True, exist_ok=True)

    compiled_scripts: list[dict[str, Any]] = []
    chain_parts: list[str] = [
        "-- ScratchBird driver full-surface SBSQL chain",
        f"-- source_suite_id: {manifest.get('suite_id', '')}",
        f"-- namespace: {values['__SB_NAMESPACE__']}",
        f"-- driver: {values['__SB_DRIVER__']}",
        f"-- route: {values['__SB_ROUTE__']}",
        f"-- parser_mode: {values['__SB_PARSER_MODE__']}",
        f"-- page_size: {values['__SB_PAGE_SIZE__']}",
        "",
    ]

    fixture_rows, fixture_sources = read_builtin_fixture_rows(repo_root)
    copied_fixture_csvs = copy_builtin_fixture_csvs(repo_root, output_root)
    for item in manifest.get("scripts", []):
        if not isinstance(item, dict):
            raise ValueError("manifest scripts must be objects")
        if "generated_from" in item:
            script_id = str(item.get("script_id", ""))
            if script_id != "SBDFS-055":
                raise ValueError(f"unknown generated script id: {script_id}")
            rendered = build_generated_builtin_fixture_script(
                namespace=values["__SB_NAMESPACE__"],
                rows=fixture_rows,
                manifest=manifest,
            )
            target = output_scripts / "055_builtin_fixture_manifest.sbsql"
            target.write_text(rendered, encoding="utf-8")
            digest = sha256_text(rendered)
            compiled_scripts.append(
                {
                    "script_id": item.get("script_id"),
                    "source_path": item.get("generated_from"),
                    "compiled_path": str(target),
                    "sha256": digest,
                    "assertions": item.get("assertions", []),
                    "coverage": item.get("coverage", []),
                    "generated": True,
                }
            )
            chain_parts.extend(
                [
                    f"-- begin_script: {target.name}",
                    rendered.rstrip(),
                    f"-- end_script: {target.name}",
                    "",
                ]
            )
            continue
        rel_path = Path(str(item.get("path", "")))
        if rel_path.is_absolute() or ".." in rel_path.parts:
            raise ValueError(f"invalid script path in manifest: {rel_path}")
        source = suite_root / rel_path
        if not source.is_file():
            raise FileNotFoundError(source)
        source_text = source.read_text(encoding="utf-8")
        rendered = apply_substitutions(source_text, values)
        target = output_scripts / source.name
        target.write_text(rendered, encoding="utf-8")
        digest = sha256_text(rendered)
        compiled_scripts.append(
            {
                "script_id": item.get("script_id"),
                "source_path": str(source.relative_to(suite_root)),
                "compiled_path": str(target),
                "sha256": digest,
                "assertions": item.get("assertions", []),
                "coverage": item.get("coverage", []),
            }
        )
        chain_parts.extend(
            [
                f"-- begin_script: {source.name}",
                rendered.rstrip(),
                f"-- end_script: {source.name}",
                "",
            ]
        )

    chain_text = "\n".join(chain_parts).rstrip() + "\n"
    chain_path = output_root / "full_surface_chain.sbsql"
    chain_path.write_text(chain_text, encoding="utf-8")
    expected_files = copy_expected_files(suite_root, output_root)
    expected_files.extend(copy_generated_expected_files(suite_root, output_root))

    compiled_manifest = {
        "schema_version": 1,
        "suite_id": manifest.get("suite_id"),
        "source_manifest": str((suite_root / MANIFEST_NAME).resolve()),
        "namespace": values["__SB_NAMESPACE__"],
        "driver": values["__SB_DRIVER__"],
        "run_id": values["__SB_RUN_ID__"],
        "route": values["__SB_ROUTE__"],
        "parser_mode": values["__SB_PARSER_MODE__"],
        "page_size": values["__SB_PAGE_SIZE__"],
        "compiled_chain_path": str(chain_path),
        "compiled_chain_sha256": sha256_text(chain_text),
        "compiled_scripts": compiled_scripts,
        "expected_files": expected_files,
        "builtin_fixture_csvs": copied_fixture_csvs,
        "builtin_fixture_sources": fixture_sources,
        "builtin_fixture_rows": len(fixture_rows),
        "builtin_fixture_surface_ids": len(
            {row["surface_id"] for row in fixture_rows if row["surface_id"]}
        ),
        "exhaustive_summary": generated_summary_from_manifest(repo_root, manifest),
    }
    manifest_path = output_root / "compiled_manifest.json"
    manifest_path.write_text(
        json.dumps(compiled_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return compiled_manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--suite-root", type=Path, default=SUITE_ROOT)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--route", required=True)
    parser.add_argument("--parser-mode", required=True)
    parser.add_argument("--page-size", required=True)
    parser.add_argument("--artifact-root", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiled = compile_suite(
        repo_root=args.repo_root,
        suite_root=args.suite_root,
        output_root=args.output_root,
        values=substitution_map(args),
    )
    print(json.dumps(compiled, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
