#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import sys
import tempfile
from pathlib import Path

from firebird_reference_native_harness import (
    ALLOWED_CLASSIFICATIONS,
    ALLOWED_LABELS,
    REQUIRED_FAILURE_FIELDS,
    SEED_ONLY_CLASSIFICATIONS,
    validate_failure_inventory_record,
)


POLICY_MUST_CONTAIN = (
    "CTest name and label set",
    "Firebird surface row id",
    "Reference tool name and arguments",
    "ScratchBird endpoint and profile",
    "Raw stdout and stderr paths",
    "Normalized output path",
    "Exit status and signal",
    "Status vector and canonical diagnostic vector",
    "Expected classification",
    "Actual classification",
    "Rerun command",
    "Cleanup status",
    "firebird_operational_failure_inventory_gate",
)


def validate_policy(policy_path: Path) -> list[str]:
    text = policy_path.read_text()
    return [token for token in POLICY_MUST_CONTAIN if token not in text]


def create_valid_record(base_dir: Path) -> dict[str, str]:
    raw_stdout = base_dir / "firebird_isql_original_regression_gate.stdout.raw"
    raw_stderr = base_dir / "firebird_isql_original_regression_gate.stderr.raw"
    normalized = base_dir / "firebird_isql_original_regression_gate.normalized.txt"
    raw_stdout.write_text("Statement failed\n")
    raw_stderr.write_text("SQLCODE=-104\n")
    normalized.write_text("SQLCODE=-104\nSQLSTATE=42000\n")
    return {
        "ctest_name": "firebird_isql_original_regression_gate",
        "label_set": "firebird_isql_original_regression_gate;firebird_reference_native",
        "surface_row_id": "FBCTV-002",
        "reference_tool_name": "isql",
        "reference_tool_args": "-z -i case.sql",
        "scratchbird_endpoint": "firebird://127.0.0.1:3050/scratchbird",
        "scratchbird_profile": "firebird_5_0",
        "raw_stdout_path": str(raw_stdout),
        "raw_stderr_path": str(raw_stderr),
        "normalized_output_path": str(normalized),
        "exit_status": "1",
        "signal": "0",
        "status_vector": "isc_sqlerr;isc_token_err",
        "canonical_diagnostic_vector": "syntax_error;token_error",
        "expected_classification": "pass_normalized",
        "actual_classification": "invalid_input_expected",
        "rerun_command": "ctest --test-dir build/engine_listener_storage_release_gate --output-on-failure -R firebird_isql_original_regression_gate",
        "cleanup_status": "retained_for_evidence",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True)
    args = parser.parse_args()

    policy_path = Path(args.policy).resolve()
    missing = validate_policy(policy_path)
    if missing:
        for token in missing:
            print(f"failure inventory policy missing required token: {token}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="sb_firebird_failures_") as temp:
        base_dir = Path(temp).resolve()
        valid = create_valid_record(base_dir)
        errors = validate_failure_inventory_record(valid, base_dir)
        if errors:
            print("valid failure inventory record rejected:", file=sys.stderr)
            for error in errors:
                print(error, file=sys.stderr)
            return 1
        final_errors = validate_failure_inventory_record(valid, base_dir, final_mode=True)
        if not any("actual_classification must match expected_classification" in error for error in final_errors):
            print("final-mode classification drift was not detected", file=sys.stderr)
            return 1

        seed_blocked = dict(valid)
        seed_blocked["expected_classification"] = "blocked_missing_endpoint"
        seed_blocked["actual_classification"] = "blocked_missing_endpoint"
        errors = validate_failure_inventory_record(seed_blocked, base_dir)
        if errors:
            print("seed-only blocked_missing_endpoint classification was rejected in seed mode", file=sys.stderr)
            for error in errors:
                print(error, file=sys.stderr)
            return 1
        errors = validate_failure_inventory_record(seed_blocked, base_dir, final_mode=True)
        if not any("seed-only value in final mode" in error for error in errors):
            print("seed-only blocked_missing_endpoint classification was accepted in final mode", file=sys.stderr)
            return 1

        incomplete = dict(valid)
        del incomplete["surface_row_id"]
        incomplete["rerun_command"] = ""
        incomplete["raw_stdout_path"] = str(base_dir.parent / "outside.stdout")
        incomplete["actual_classification"] = "blocked_missing_endpoint"
        incomplete["exit_status"] = "not_an_integer"
        incomplete["signal"] = "not_an_integer"
        incomplete["label_set"] = "firebird_isql_original_regression_gate;unknown_label"
        errors = validate_failure_inventory_record(incomplete, base_dir)
        if not errors:
            print("incomplete failure inventory record was accepted", file=sys.stderr)
            return 1
        if not any("rerun_command" in error for error in errors):
            print("missing rerun_command was not detected", file=sys.stderr)
            return 1
        if not any("surface_row_id" in error for error in errors):
            print("missing surface_row_id field was not detected", file=sys.stderr)
            return 1
        if not any("raw_stdout_path escapes" in error for error in errors):
            print("escaping raw_stdout_path was not detected", file=sys.stderr)
            return 1
        if any("actual_classification has invalid value" in error for error in errors):
            print("seed-only actual_classification was treated as an invalid classification", file=sys.stderr)
            return 1
        if not any("exit_status must be an integer" in error for error in errors):
            print("nonnumeric exit_status was not detected", file=sys.stderr)
            return 1
        if not any("signal must be an integer" in error for error in errors):
            print("nonnumeric signal was not detected", file=sys.stderr)
            return 1
        if not any("firebird_reference_native" in error for error in errors):
            print("missing reference-native label was not detected", file=sys.stderr)
            return 1
        if not any("unsupported label" in error for error in errors):
            print("unsupported label was not detected", file=sys.stderr)
            return 1

    print(
        "validated Firebird operational failure inventory schema with "
        f"{len(REQUIRED_FAILURE_FIELDS)} required fields, "
        f"{len(ALLOWED_CLASSIFICATIONS)} final classifications, "
        f"{len(SEED_ONLY_CLASSIFICATIONS)} seed-only classifications, and "
        f"{len(ALLOWED_LABELS)} accepted labels"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
