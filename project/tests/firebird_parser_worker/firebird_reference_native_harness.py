#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import re
from pathlib import Path


ALLOWED_CLASSIFICATIONS = {
    "pass_exact",
    "pass_normalized",
    "emulated_expected",
    "authority_violation_expected",
    "invalid_input_expected",
}

SEED_ONLY_CLASSIFICATIONS = {
    "blocked_missing_endpoint",
    "blocked_candidate_hash_mismatch",
    "blocked_candidate_source_missing",
    "blocked_parser_rejection",
    "blocked_parser_timeout",
    "blocked_static_replay_extraction",
}

ALLOWED_LABELS = {
    "firebird_reference_native",
    "firebird_parser_worker",
    "firebird_reference_native_result_normalization_gate",
    "firebird_operational_failure_inventory_gate",
    "firebird_isql_original_regression_gate",
    "firebird_original_regression_replay_gate",
    "fbqa_full_original_regression_inventory_gate",
    "fbqa_full_original_regression_final_gate",
}

REQUIRED_FAILURE_FIELDS = (
    "ctest_name",
    "label_set",
    "surface_row_id",
    "reference_tool_name",
    "reference_tool_args",
    "scratchbird_endpoint",
    "scratchbird_profile",
    "raw_stdout_path",
    "raw_stderr_path",
    "normalized_output_path",
    "exit_status",
    "signal",
    "status_vector",
    "canonical_diagnostic_vector",
    "expected_classification",
    "actual_classification",
    "rerun_command",
    "cleanup_status",
)

PROTECTED_PREFIXES = (
    "SQL_RESULT=",
    "COLUMN_LABEL=",
    "TYPE_NAME=",
    "DESCRIPTOR=",
    "SQL_DESCRIPTOR=",
    "PRECISION=",
    "SCALE=",
    "NULLABLE=",
    "CHARSET=",
    "COLLATION=",
    "STATUS_VECTOR=",
    "SQLCODE=",
    "SQLSTATE=",
    "AFFECTED_ROWS=",
    "COMMAND_TAG=",
    "SERVICE_RESULT=",
)


def _replace(pattern: str, value: str, text: str) -> str:
    return re.sub(pattern, value, text, flags=re.IGNORECASE)


def _is_protected_line(line: str) -> bool:
    stripped = line.lstrip()
    return any(stripped.upper().startswith(prefix) for prefix in PROTECTED_PREFIXES)


def _normalize_warning_ordering(text: str) -> str:
    lines = text.splitlines()
    output: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.strip() != "WARNING_CHAIN_EQUIVALENT_BEGIN":
            output.append(line)
            index += 1
            continue

        output.append(line)
        index += 1
        block: list[str] = []
        while index < len(lines) and lines[index].strip() != "WARNING_CHAIN_EQUIVALENT_END":
            block.append(lines[index])
            index += 1
        output.extend(sorted(block, key=str.casefold))
        if index < len(lines):
            output.append(lines[index])
            index += 1
    return "\n".join(output)


def normalize_firebird_reference_output(
    text: str,
    *,
    repo_root: Path,
    temp_root: Path,
    hostname: str = "scratchbird-host",
) -> str:
    protected: dict[str, str] = {}
    lines: list[str] = []
    for index, line in enumerate(text.splitlines()):
        if _is_protected_line(line):
            token = f"__SB_FIREBIRD_PROTECTED_{index}__"
            protected[token] = line
            lines.append(token)
        else:
            lines.append(line)

    normalized = "\n".join(lines)
    normalized = normalized.replace(str(repo_root.resolve()), "<REPO_ROOT>")
    normalized = normalized.replace(str(temp_root.resolve()), "<TEMP_ROOT>")
    normalized = _replace(r"\bRAW_PATH=(?:<REPO_ROOT>|<TEMP_ROOT>)[^\n]*", "RAW_PATH=<PATH>", normalized)
    normalized = _replace(r"\bTEMP_PATH=(?:<REPO_ROOT>|<TEMP_ROOT>)[^\n]*", "TEMP_PATH=<PATH>", normalized)
    normalized = _replace(
        r"(?<![A-Za-z0-9_])(?:<REPO_ROOT>|<TEMP_ROOT>)(?:/[^\s:;,)\]}\"']*)?",
        "<PATH>",
        normalized,
    )
    normalized = _replace(
        r"(?<![A-Za-z0-9_])/(?:tmp|var/tmp|home|build|workspace|mnt|opt|usr/tmp|private/tmp|run/user/\d+)/[^\s:;,)\]}\"']*",
        "<PATH>",
        normalized,
    )
    normalized = _replace(
        r"(?<![A-Za-z0-9_])[A-Z]:\\(?:Users|Temp|Windows\\Temp|workspace|build)\\[^\s:;,)\]}\"']*",
        "<PATH>",
        normalized,
    )
    normalized = _replace(
        r"\b(?:sb_firebird_(?:norm|failures|reference|isql|service)_[A-Za-z0-9_+-]+|tmp[A-Za-z0-9_.-]{6,})\b",
        "<TEMP_DIR>",
        normalized,
    )
    normalized = normalized.replace(hostname, "<HOSTNAME>")
    normalized = _replace(r"\b(?:localhost|127\.0\.0\.1|::1)\b", "<LOOPBACK>", normalized)
    normalized = _replace(r"\bpid\s*=\s*\d+\b", "pid=<PID>", normalized)
    normalized = _replace(r"\bprocess\s+id\s*:\s*\d+\b", "process id:<PID>", normalized)
    normalized = _replace(r"\bconnection\s+id\s*=\s*\d+\b", "connection id=<CONNECTION_ID>", normalized)
    normalized = _replace(r"\btransaction\s+id\s*=\s*\d+\b", "transaction id=<TRANSACTION_ID>", normalized)
    normalized = _replace(r"\bpage\s+id\s*=\s*\d+\b", "page id=<PAGE_ID>", normalized)
    normalized = _replace(r"\battachment\s+id\s*=\s*\d+\b", "attachment id=<ATTACHMENT_ID>", normalized)
    normalized = _replace(
        r"\bobject\s+uuid\s*=\s*[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b",
        "object uuid=<OBJECT_UUID>",
        normalized,
    )
    normalized = _replace(
        r"\b\d{4}-\d{2}-\d{2}[ t]\d{2}:\d{2}:\d{2}(?:\.\d+)?z?\b",
        "<TIMESTAMP>",
        normalized,
    )
    normalized = _replace(r"\bduration\s*=\s*\d+(?:\.\d+)?\s*(?:ms|s)\b", "duration=<DURATION>", normalized)
    normalized = _normalize_warning_ordering(normalized)

    for token, line in protected.items():
        normalized = normalized.replace(token, line)
    return normalized


def changed_protected_lines(before: str, after: str) -> list[str]:
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    changed: list[str] = []
    for before_line, after_line in zip(before_lines, after_lines):
        if _is_protected_line(before_line):
            if before_line != after_line:
                changed.append(before_line)
    return changed


def validate_failure_inventory_record(
    record: dict[str, str],
    base_dir: Path,
    *,
    final_mode: bool = False,
) -> list[str]:
    errors: list[str] = []
    for field in REQUIRED_FAILURE_FIELDS:
        if field not in record:
            errors.append(f"missing required failure field: {field}")
        elif not str(record.get(field, "")).strip():
            errors.append(f"missing required failure field: {field}")

    for path_field in ("raw_stdout_path", "raw_stderr_path", "normalized_output_path"):
        value = record.get(path_field)
        if not value:
            continue
        path = Path(value)
        if not path.is_absolute():
            path = base_dir / path
        try:
            path.resolve().relative_to(base_dir.resolve())
        except ValueError:
            errors.append(f"{path_field} escapes failure evidence root: {path}")
            continue
        if not path.exists():
            errors.append(f"{path_field} does not exist: {path}")

    for classification_field in ("expected_classification", "actual_classification"):
        value = record.get(classification_field)
        if value in SEED_ONLY_CLASSIFICATIONS:
            if final_mode:
                errors.append(f"{classification_field} has seed-only value in final mode: {value}")
        elif value and value not in ALLOWED_CLASSIFICATIONS:
            errors.append(f"{classification_field} has invalid value: {value}")

    expected = record.get("expected_classification")
    actual = record.get("actual_classification")
    if final_mode and expected and actual and expected != actual:
        errors.append("final-mode actual_classification must match expected_classification")

    labels = set(filter(None, record.get("label_set", "").split(";")))
    ctest_name = record.get("ctest_name", "")
    if ctest_name and ctest_name not in labels:
        errors.append("label_set must include the CTest name")
    if labels and "firebird_reference_native" not in labels:
        errors.append("label_set must include firebird_reference_native")
    unknown_labels = labels - ALLOWED_LABELS
    if unknown_labels:
        errors.append("label_set contains unsupported label(s): " + ",".join(sorted(unknown_labels)))

    for numeric_field in ("exit_status", "signal"):
        value = record.get(numeric_field)
        if value and not re.fullmatch(r"-?\d+", value.strip()):
            errors.append(f"{numeric_field} must be an integer")

    if record.get("cleanup_status") not in {"clean", "retained_for_evidence"}:
        errors.append("cleanup_status must be clean or retained_for_evidence")

    if record.get("rerun_command") and "ctest" not in record["rerun_command"]:
        errors.append("rerun_command must include the CTest invocation")

    return errors
