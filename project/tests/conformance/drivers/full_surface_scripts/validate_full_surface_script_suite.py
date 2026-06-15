#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the shared driver full-surface SBSQL script suite."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any

from compile_full_surface_script_suite import compile_suite
from exhaustive_generators import GENERATED_SCRIPT_SPECS, source_summary


SUITE_ROOT = Path(__file__).resolve().parent
MANIFEST_NAME = "manifest.json"
EXPECTED_ASSERTIONS_REL = Path("expected/expected_assertions.json")
EXPECTED_REFUSALS_REL = Path("expected/expected_refusals.json")
LANGUAGE_MANIFEST_REL = Path(
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/"
    "SBSQL_LANGUAGE_ELEMENT_MANIFEST.json"
)
BUILTIN_FIXTURE_ROOT_REL = Path("project/tests/sbsql_parser_worker/generated/full_surface")
FORBIDDEN_SCRIPT_PATTERNS = (
    re.compile(r"\bTODO\b", re.IGNORECASE),
    re.compile(r"\bTBD\b", re.IGNORECASE),
    re.compile(r"\bFIXME\b", re.IGNORECASE),
    re.compile(r"\bDEFER(?:RED)?\b", re.IGNORECASE),
    re.compile(r"\bstub\b", re.IGNORECASE),
    re.compile(r"not implemented", re.IGNORECASE),
    re.compile(r"\bsqlite\b", re.IGNORECASE),
    re.compile(r"\bPRAGMA\b", re.IGNORECASE),
    re.compile(r"\bjournal_mode\b", re.IGNORECASE),
    re.compile(r"\bWAL\b", re.IGNORECASE),
)


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[5]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def script_key(path_text: str) -> str:
    return Path(path_text).name


def validate_manifest_shape(manifest: dict[str, Any], suite_root: Path) -> list[str]:
    errors: list[str] = []
    if manifest.get("schema_version") != 1:
        errors.append("manifest:schema_version_must_be_1")
    if manifest.get("suite_id") != "scratchbird.driver.full_surface.v1":
        errors.append("manifest:suite_id_drift")

    placeholders = set(as_list(manifest.get("compile_placeholders")))
    required_placeholders = {
        "__SB_NAMESPACE__",
        "__SB_DRIVER__",
        "__SB_RUN_ID__",
        "__SB_ROUTE__",
        "__SB_PARSER_MODE__",
        "__SB_PAGE_SIZE__",
    }
    if placeholders != required_placeholders:
        errors.append("manifest:compile_placeholders_drift")

    required_coverage = {str(item) for item in as_list(manifest.get("required_coverage"))}
    scripts = [item for item in as_list(manifest.get("scripts")) if isinstance(item, dict)]
    if not scripts:
        errors.append("manifest:no_scripts")
        return errors

    script_ids: set[str] = set()
    assertion_ids: set[str] = set()
    covered: set[str] = set()
    if as_list(manifest.get("exhaustive_generated_script_ids")):
        covered.add("generated_full_surface")
    for item in scripts:
        script_id = str(item.get("script_id", ""))
        if not script_id:
            errors.append("manifest:script_missing_id")
        elif script_id in script_ids:
            errors.append(f"manifest:duplicate_script_id:{script_id}")
        script_ids.add(script_id)

        item_coverage = {str(value) for value in as_list(item.get("coverage"))}
        unknown = item_coverage - required_coverage
        if unknown:
            errors.append(f"manifest:{script_id}:unknown_coverage:{','.join(sorted(unknown))}")
        covered.update(item_coverage)

        if "generated_from" in item:
            for assertion_id in [str(value) for value in as_list(item.get("assertions"))]:
                if assertion_id in assertion_ids:
                    errors.append(f"manifest:duplicate_assertion_id:{assertion_id}")
                assertion_ids.add(assertion_id)
            continue

        rel_path = Path(str(item.get("path", "")))
        if rel_path.is_absolute() or ".." in rel_path.parts:
            errors.append(f"manifest:{script_id}:invalid_script_path:{rel_path}")
            continue
        source = suite_root / rel_path
        if not source.is_file():
            errors.append(f"manifest:{script_id}:missing_script:{rel_path}")
            continue

        text = source.read_text(encoding="utf-8")
        if script_key(str(rel_path)) != "050_functions_operators.sbsql":
            if manifest.get("namespace_placeholder") not in text:
                errors.append(f"script:{rel_path}:missing_namespace_placeholder")
        for pattern in FORBIDDEN_SCRIPT_PATTERNS:
            if pattern.search(text):
                errors.append(f"script:{rel_path}:forbidden_token:{pattern.pattern}")
        for assertion_id in [str(value) for value in as_list(item.get("assertions"))]:
            if assertion_id in assertion_ids:
                errors.append(f"manifest:duplicate_assertion_id:{assertion_id}")
            assertion_ids.add(assertion_id)
            if assertion_id not in text:
                errors.append(f"script:{rel_path}:missing_assertion_literal:{assertion_id}")

    missing_coverage = required_coverage - covered
    if missing_coverage:
        errors.append(f"manifest:missing_required_coverage:{','.join(sorted(missing_coverage))}")
    return errors


def read_language_manifest(repo_root: Path) -> tuple[dict[str, Any] | None, list[str]]:
    language_manifest_path = repo_root / LANGUAGE_MANIFEST_REL
    try:
        return load_json(language_manifest_path), []
    except (OSError, json.JSONDecodeError) as exc:
        return None, [f"language_manifest:load_failed:{exc}"]


def validate_expected_files(
    manifest: dict[str, Any],
    expected_assertions: dict[str, Any],
    expected_refusals: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    scripts = [item for item in as_list(manifest.get("scripts")) if isinstance(item, dict)]
    assertion_count = sum(len(as_list(item.get("assertions"))) for item in scripts)
    assertion_count += int(manifest.get("exhaustive_generated_assertion_count", 0))
    if expected_assertions.get("schema_version") != 1:
        errors.append("expected_assertions:schema_version_must_be_1")
    if expected_assertions.get("assertion_count") != assertion_count:
        errors.append(
            "expected_assertions:assertion_count_drift:"
            f"{expected_assertions.get('assertion_count')}!={assertion_count}"
        )

    manifest_refusals = {
        str(value)
        for item in scripts
        for value in as_list(item.get("expected_refusals"))
    }
    expected_statement_ids = {str(value) for value in as_list(expected_refusals.get("statement_ids"))}
    if manifest_refusals != expected_statement_ids:
        errors.append("expected_refusals:statement_id_drift")
    expected_diagnostics = expected_refusals.get("expected_diagnostics", {})
    if not isinstance(expected_diagnostics, dict):
        errors.append("expected_refusals:expected_diagnostics_not_object")
    else:
        for statement_id in expected_statement_ids:
            diagnostics = as_list(expected_diagnostics.get(statement_id))
            if not diagnostics:
                errors.append(f"expected_refusals:{statement_id}:missing_diagnostics")
            if ":" not in statement_id:
                errors.append(f"expected_refusals:{statement_id}:missing_line_suffix")
    return errors


def validate_language_manifest(
    language_manifest: dict[str, Any],
    suite_manifest: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    element_counts = language_manifest.get("element_kind_counts", {})
    if not isinstance(element_counts, dict):
        return ["language_manifest:element_kind_counts_not_object"]
    required_counts = suite_manifest.get("minimum_language_catalog_counts", {})
    if not isinstance(required_counts, dict):
        return ["manifest:minimum_language_catalog_counts_not_object"]
    for kind, minimum in required_counts.items():
        actual = int(element_counts.get(kind, 0))
        if actual < int(minimum):
            errors.append(f"language_manifest:{kind}:below_minimum:{actual}<{minimum}")
    return errors


def validate_builtin_fixture_sources(
    repo_root: Path,
    suite_manifest: dict[str, Any],
    language_manifest: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    fixture_root = repo_root / BUILTIN_FIXTURE_ROOT_REL
    if not fixture_root.is_dir():
        return [f"builtin_fixtures:missing_root:{fixture_root}"]
    language_elements = language_manifest.get("elements", [])
    all_surface_ids = {
        str(item.get("surface_id", ""))
        for item in language_elements
        if isinstance(item, dict)
    }
    release_function_operator_ids = {
        str(item.get("surface_id", ""))
        for item in language_elements
        if isinstance(item, dict)
        and item.get("element_kind") in {"surface_function", "surface_operator"}
        and item.get("support_state") == "release_supported"
    }
    fixture_files = 0
    fixture_rows = 0
    diagnostic_rows = 0
    fixture_ids: set[str] = set()
    fixture_surface_ids: set[str] = set()
    required_columns = {
        "fixture_id",
        "surface_id",
        "function_id",
        "canonical_builtin_id",
        "case_kind",
        "expected_result_descriptor",
        "expected_diagnostic_code",
    }
    argument_columns = {
        "arguments_json",
        "ordered_values_json",
        "expected_result_json",
    }
    for path in sorted(fixture_root.glob("*.csv")):
        fixture_files += 1
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                errors.append(f"builtin_fixtures:{path.name}:missing_header")
                continue
            missing_columns = required_columns - set(reader.fieldnames)
            if missing_columns:
                errors.append(
                    f"builtin_fixtures:{path.name}:missing_columns:"
                    f"{','.join(sorted(missing_columns))}"
                )
                continue
            if not (set(reader.fieldnames) & argument_columns):
                errors.append(f"builtin_fixtures:{path.name}:missing_argument_payload_column")
                continue
            for row in reader:
                fixture_rows += 1
                fixture_id = row.get("fixture_id", "")
                surface_id = row.get("surface_id", "")
                if not fixture_id:
                    errors.append(f"builtin_fixtures:{path.name}:empty_fixture_id")
                elif fixture_id in fixture_ids:
                    errors.append(f"builtin_fixtures:duplicate_fixture_id:{fixture_id}")
                fixture_ids.add(fixture_id)
                if surface_id:
                    fixture_surface_ids.add(surface_id)
                    if surface_id.startswith("SBSQL-") and surface_id not in all_surface_ids:
                        errors.append(f"builtin_fixtures:{path.name}:unknown_surface_id:{surface_id}")
                if row.get("expected_diagnostic_code"):
                    diagnostic_rows += 1
    release_covered = fixture_surface_ids & release_function_operator_ids
    observed = {
        "fixture_files": fixture_files,
        "fixture_rows": fixture_rows,
        "fixture_surface_ids": len(fixture_surface_ids),
        "release_function_operator_fixture_surface_ids": len(release_covered),
        "diagnostic_rows": diagnostic_rows,
    }
    minimums = suite_manifest.get("builtin_fixture_minimums", {})
    if not isinstance(minimums, dict):
        return errors + ["manifest:builtin_fixture_minimums_not_object"]
    for key, minimum in minimums.items():
        actual = int(observed.get(str(key), 0))
        if actual < int(minimum):
            errors.append(f"builtin_fixtures:{key}:below_minimum:{actual}<{minimum}")
    return errors


def validate_exhaustive_sources(repo_root: Path, suite_manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    try:
        observed = source_summary(repo_root)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return [f"exhaustive_sources:load_failed:{exc}"]
    minimums = suite_manifest.get("exhaustive_source_minimums", {})
    if not isinstance(minimums, dict):
        return ["manifest:exhaustive_source_minimums_not_object"]
    for key, minimum in minimums.items():
        if key.startswith("generated_"):
            continue
        actual = int(observed.get(str(key), 0))
        if actual < int(minimum):
            errors.append(f"exhaustive_sources:{key}:below_minimum:{actual}<{minimum}")
    generated_ids = set(str(item) for item in as_list(suite_manifest.get("exhaustive_generated_script_ids")))
    expected_ids = {script_id for script_id, _filename in GENERATED_SCRIPT_SPECS}
    if generated_ids != expected_ids:
        errors.append("manifest:exhaustive_generated_script_ids_drift")
    return errors


def validate_compiled_sample(
    repo_root: Path,
    suite_root: Path,
    output_root: Path,
    manifest: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    try:
        compiled = compile_suite(
            repo_root=repo_root,
            suite_root=suite_root,
            output_root=output_root,
            values={
                "__SB_NAMESPACE__": "users.public.examples.validator.run001.listener_parser.8k",
                "__SB_DRIVER__": "validator",
                "__SB_RUN_ID__": "run001",
                "__SB_ROUTE__": "listener-parser",
                "__SB_PARSER_MODE__": "server-parser",
                "__SB_PAGE_SIZE__": "8k",
            },
        )
    except (OSError, ValueError, FileNotFoundError) as exc:
        return [f"compiler:sample_compile_failed:{exc}"]
    placeholders = [str(value) for value in as_list(manifest.get("compile_placeholders"))]
    for item in as_list(compiled.get("compiled_scripts")):
        if not isinstance(item, dict):
            errors.append("compiler:compiled_script_entry_not_object")
            continue
        path = Path(str(item.get("compiled_path", "")))
        if not path.is_file():
            errors.append(f"compiler:missing_compiled_script:{path}")
            continue
        text = path.read_text(encoding="utf-8")
        leftovers = [placeholder for placeholder in placeholders if placeholder in text]
        if leftovers:
            errors.append(f"compiler:{path.name}:leftover_placeholders:{','.join(leftovers)}")
    chain_path = Path(str(compiled.get("compiled_chain_path", "")))
    if not chain_path.is_file():
        errors.append("compiler:missing_chain")
    elif any(placeholder in chain_path.read_text(encoding="utf-8") for placeholder in placeholders):
        errors.append("compiler:chain_has_leftover_placeholders")
    fixture_csvs = as_list(compiled.get("builtin_fixture_csvs"))
    expected_minimums = manifest.get("builtin_fixture_minimums", {})
    if isinstance(expected_minimums, dict):
        expected_files = int(expected_minimums.get("fixture_files", 0))
        if len(fixture_csvs) < expected_files:
            errors.append(f"compiler:fixture_csv_copy_count:{len(fixture_csvs)}<{expected_files}")
    generated = [
        item for item in as_list(compiled.get("compiled_scripts"))
        if isinstance(item, dict) and item.get("generated") is True
    ]
    if not generated:
        errors.append("compiler:missing_generated_builtin_fixture_script")
    expected_generated_ids = {
        str(item) for item in as_list(manifest.get("exhaustive_generated_script_ids"))
    }
    compiled_generated_ids = {
        str(item.get("script_id"))
        for item in generated
        if str(item.get("script_id", "")).startswith("SBDFS-1")
    }
    if compiled_generated_ids != expected_generated_ids:
        errors.append(
            "compiler:exhaustive_generated_script_id_drift:"
            f"{','.join(sorted(compiled_generated_ids))}"
        )
    summary = compiled.get("exhaustive_summary", {})
    if not isinstance(summary, dict):
        errors.append("compiler:missing_exhaustive_summary")
    else:
        minimums = manifest.get("exhaustive_source_minimums", {})
        if isinstance(minimums, dict):
            for key, minimum in minimums.items():
                actual = int(summary.get(str(key), 0))
                if actual < int(minimum):
                    errors.append(f"compiler:exhaustive_summary:{key}:below_minimum:{actual}<{minimum}")
    return errors


def write_report(path: Path, errors: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "command": "validate_full_surface_script_suite.py",
        "status": "fail" if errors else "pass",
        "issues": errors,
    }
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--suite-root", type=Path, default=SUITE_ROOT)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    suite_root = args.suite_root.resolve()
    output_root = args.output_root.resolve()

    errors: list[str] = []
    language_manifest: dict[str, Any] | None = None
    try:
        manifest = load_json(suite_root / MANIFEST_NAME)
    except (OSError, json.JSONDecodeError) as exc:
        manifest = {}
        errors.append(f"manifest:load_failed:{exc}")
    try:
        expected_assertions = load_json(suite_root / EXPECTED_ASSERTIONS_REL)
    except (OSError, json.JSONDecodeError) as exc:
        expected_assertions = {}
        errors.append(f"expected_assertions:load_failed:{exc}")
    try:
        expected_refusals = load_json(suite_root / EXPECTED_REFUSALS_REL)
    except (OSError, json.JSONDecodeError) as exc:
        expected_refusals = {}
        errors.append(f"expected_refusals:load_failed:{exc}")

    if manifest:
        language_manifest, language_errors = read_language_manifest(repo_root)
        errors.extend(language_errors)
        errors.extend(validate_manifest_shape(manifest, suite_root))
        if language_manifest is not None:
            errors.extend(validate_language_manifest(language_manifest, manifest))
            errors.extend(validate_builtin_fixture_sources(repo_root, manifest, language_manifest))
        errors.extend(validate_exhaustive_sources(repo_root, manifest))
        errors.extend(validate_compiled_sample(repo_root, suite_root, output_root, manifest))
    if manifest and expected_assertions and expected_refusals:
        errors.extend(validate_expected_files(manifest, expected_assertions, expected_refusals))

    if args.report:
        write_report(args.report, errors)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("driver_full_surface_script_suite=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
