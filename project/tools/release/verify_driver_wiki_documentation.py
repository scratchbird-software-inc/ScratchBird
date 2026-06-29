#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Verify wiki documentation for complete driver baseline coverage."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_complete_coverage_common import (
    WIKI_DOCUMENTATION_REPORT,
    driver_rows,
    index_rows,
    issue_status_summary,
    load_driver_manifest,
    load_matrix,
    report_status,
    write_report,
)
from driver_release_common import add_common_args, default_report_path, fail, resolve_repo_root


DOC_CANDIDATES = (
    "Driver-Baseline-Coverage.md",
    "Client-Drivers.md",
)

REQUIRED_TEXT_TOKENS = (
    "G1",
    "G14",
    "D1",
    "D27",
    "ODBC",
    "JDBC",
    ".NET",
    "SBsql",
    "SBLR",
    "UUID",
    "server admission",
    "MGA",
    "inet",
    "IPC",
    "embedded",
    "TLS",
    "en-US",
    "en-CA",
    "fr-CA",
    "fr-FR",
    "de-DE",
    "it-IT",
    "es-ES",
    "not_applicable_with_citation",
)


def read_docs(wiki_root: Path) -> tuple[str, list[str]]:
    missing: list[str] = []
    parts: list[str] = []
    for name in DOC_CANDIDATES:
        path = wiki_root / name
        if path.is_file():
            parts.append(path.read_text(encoding="utf-8", errors="replace"))
        else:
            missing.append(name)
    return "\n".join(parts), missing


def build_report(repo_root: Path, wiki_root: Path, matrix_path: Path) -> dict[str, Any]:
    manifest_by_component, manifest_issues = index_rows(
        driver_rows(load_driver_manifest(repo_root)),
        "component_id",
    )
    matrix_rows = load_matrix(matrix_path)
    matrix_by_component, matrix_issues = index_rows(matrix_rows, "component_id")
    text, missing_docs = read_docs(wiki_root)
    lower_text = text.lower()
    issues: list[str] = []
    issues.extend(f"manifest:{issue}" for issue in manifest_issues)
    issues.extend(f"matrix:{issue}" for issue in matrix_issues)
    for name in missing_docs:
        issues.append(f"wiki:missing_document:{name}")
    if not text.strip():
        issues.append("wiki:no_documentation_text_loaded")

    for token in REQUIRED_TEXT_TOKENS:
        if token.lower() not in lower_text:
            issues.append(f"wiki:missing_required_token:{token}")

    expected_components = set(manifest_by_component)
    for component in sorted(expected_components - set(matrix_by_component)):
        issues.append(f"wiki:{component}:missing_matrix_row")
    for component in sorted(expected_components & set(matrix_by_component)):
        driver = manifest_by_component[component].get("name", "").strip()
        for token in (component, driver):
            if token.lower() not in lower_text:
                issues.append(f"wiki:{component}:missing_lane_token:{token}")
        status = matrix_by_component[component].get("wiki_doc_status", "").strip()
        if status not in {"implemented_and_proven", "complete", "completed", "passed", "verified"}:
            issues.append(f"wiki:{component}:wiki_doc_status_non_closing:{status or 'empty'}")

    return {
        "command": "verify_driver_wiki_documentation.py",
        "gate_id": "BETA-DTA-GATE-035",
        "status": report_status(issues),
        "summary": {
            "wiki_root": str(wiki_root),
            "documentation_files_checked": list(DOC_CANDIDATES),
            "manifest_driver_rows": len(manifest_by_component),
            "matrix_rows": len(matrix_rows),
            "status_summary": issue_status_summary(matrix_rows),
        },
        "issues": issues,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    parser.add_argument("--wiki", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    output = args.output or default_report_path(repo_root, WIKI_DOCUMENTATION_REPORT)
    try:
        report = build_report(repo_root, args.wiki.expanduser().resolve(), args.matrix.expanduser().resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_wiki_documentation={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
