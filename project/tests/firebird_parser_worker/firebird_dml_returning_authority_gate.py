#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Guard Firebird DML RETURNING's exact-once MGA authority boundary."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


def extract_definition(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise ValueError(f"missing definition marker: {marker}")
    opening = source.find("{", start)
    if opening < 0:
        raise ValueError(f"missing definition body: {marker}")

    depth = 0
    index = opening
    state = "code"
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "/" and next_char == "/":
                state = "line_comment"
                index += 1
            elif char == "/" and next_char == "*":
                state = "block_comment"
                index += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        elif state in {"string", "character"}:
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment" and char == "*" and next_char == "/":
            state = "code"
            index += 1
        index += 1
    raise ValueError(f"unterminated definition body: {marker}")


def require_tokens(
    findings: list[str], label: str, text: str, tokens: tuple[str, ...]
) -> None:
    for token in tokens:
        if token not in text:
            findings.append(f"{label}: missing {token}")


def forbid_tokens(
    findings: list[str], label: str, text: str, tokens: tuple[str, ...]
) -> None:
    for token in tokens:
        if token in text:
            findings.append(f"{label}: forbidden {token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker-source", required=True)
    parser.add_argument("--execution-source", required=True)
    args = parser.parse_args()

    worker_path = pathlib.Path(args.worker_source).resolve()
    execution_path = pathlib.Path(args.execution_source).resolve()
    findings: list[str] = []
    for path in (worker_path, execution_path):
        if not path.is_file():
            findings.append(f"missing source file: {path}")
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1

    worker = worker_path.read_text(encoding="utf-8")
    execution = execution_path.read_text(encoding="utf-8")
    try:
        simple_identifier = extract_definition(
            worker, "std::optional<std::string> FirebirdSimpleReturningIdentifier("
        )
        analyzer = extract_definition(
            worker, "FirebirdDmlReturningProjection AnalyzeFirebirdDmlReturningProjection("
        )
        executor = extract_definition(
            worker, "FirebirdDmlReturningOutcome ExecuteFirebirdDmlReturning("
        )
    except ValueError as error:
        findings.append(str(error))
        simple_identifier = ""
        analyzer = ""
        executor = ""

    require_tokens(
        findings,
        "simple returning identifier",
        simple_identifier,
        ("pos != text.size()", "raw_identifier.find('.')"),
    )
    require_tokens(
        findings,
        "returning analyzer",
        analyzer,
        (
            "FindFirebirdView",
            "view_target_not_supported",
            "overlay_only_tables",
            "engine_backed_table_required",
            "base_table_descriptor_required",
            "FirebirdSimpleReturningIdentifier",
            "old_new_or_complex_returning_expression_not_supported",
        ),
    )
    require_tokens(
        findings,
        "returning executor",
        executor,
        (
            "AnalyzeFirebirdDmlReturningProjection",
            "StripTerminatingSemicolon(sql_text)",
            "FirebirdExecutionTransactionSelector",
            "FirebirdTransactionSelectorsEqual",
            "exact_transaction_selector_required",
            "execution_session->RunStatement",
            "server_result_payload",
            "authoritative_result_row_count_mismatch",
            "authoritative_result_row_payload_missing",
            "ParseServerRows",
            "projection.columns",
            "server_affected_rows_present",
            "server_affected_rows",
            "server_row_count",
        ),
    )
    forbid_tokens(
        findings,
        "returning executor",
        executor,
        (
            "ExecuteFirebirdStatement",
            "FirebirdSelectBaseRows",
            "FirebirdDmlWithoutReturning",
            "overlay_rows",
            "PersistFirebirdMetadataOverlay",
            "FirebirdApplyUpdateAssignments",
            "outcome.rows.size()",
        ),
    )
    if executor.count("execution_session->RunStatement") != 1:
        findings.append("returning executor: mutation must use exactly one RunStatement call")
    if not re.search(
        r"RunStatement\s*\(\s*authoritative_sql\s*,\s*\*transaction\s*,\s*"
        r"true\s*,",
        executor,
    ):
        findings.append(
            "returning executor: authoritative SQL and exact transaction selector are not "
            "the direct RunStatement inputs"
        )
    if not re.search(
        r"ParseServerRows\s*\(\s*outcome\.pipeline\.server_result_payload\s*,\s*"
        r"projection\.columns",
        executor,
    ):
        findings.append("returning executor: server payload is not decoded in RETURNING order")
    if not re.search(
        r"outcome\.affected_rows\s*=\s*"
        r"outcome\.pipeline\.server_affected_rows_present\s*\?\s*"
        r"outcome\.pipeline\.server_affected_rows\s*:\s*"
        r"outcome\.pipeline\.server_row_count",
        executor,
    ):
        findings.append("returning executor: affected count is not server-authoritative")

    require_tokens(
        findings,
        "returning prepare",
        worker,
        (
            '\\"runtime_policy\\":\\"canonical_sblr_authoritative_returning_projection\\"',
            "AnalyzeFirebirdDmlReturningProjection(state, sql_text)",
            "statement->translated_sql = StripTerminatingSemicolon(sql_text)",
        ),
    )
    for label, marker in (
        ("UPDATE", "std::string UpdateEnvelope("),
        ("DELETE", "std::string DeleteEnvelope("),
        ("INSERT", "std::string InsertEnvelope("),
    ):
        try:
            envelope = extract_definition(execution, marker)
        except ValueError as error:
            findings.append(str(error))
            continue
        require_tokens(
            findings,
            f"{label} execution lowering",
            envelope,
            (".returning", '\\"result_payload_policy\\":\\"full_payload\\"'),
        )

    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        print(
            f"firebird_dml_returning_authority_gate=failed findings={len(findings)}",
            file=sys.stderr,
        )
        return 1
    print("firebird_dml_returning_authority_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
