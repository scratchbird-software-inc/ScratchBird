#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import csv
import re
import sys
from pathlib import Path


FORBIDDEN = re.compile(
    r"out[ -]?of[ -]?scope|unsupported_by_profile|unsupported|degraded|"
    r"defer|deferred|unavailable|not parser surface|future|later|"
    r"sbsql-gated|refus|accepted/refused|not .*scope|not .*surface|"
    r"excluded|exclude",
    re.IGNORECASE,
)


def read_csv(path: Path):
    with path.open(newline="") as handle:
        return list(csv.reader(handle))


def validate_csv_widths(root: Path) -> list[str]:
    errors: list[str] = []
    for path in sorted(root.rglob("*.csv")):
        rows = read_csv(path)
        if not rows:
            errors.append(f"{path}: empty CSV")
            continue
        width = len(rows[0])
        for index, row in enumerate(rows, start=1):
            if len(row) != width:
                errors.append(
                    f"{path}:{index}: expected {width} columns, found {len(row)}"
                )
    return errors


def validate_required_artifacts(artifact_root: Path) -> list[str]:
    errors: list[str] = []
    required = artifact_root / "FIREBIRD_REQUIRED_P0_ARTIFACTS.csv"
    if not required.exists():
        return [f"missing required artifact manifest: {required}"]
    with required.open(newline="") as handle:
        for row in csv.DictReader(handle):
            artifact = artifact_root / row["artifact"]
            if not artifact.exists():
                errors.append(f"required P0 artifact missing: {artifact}")
    return errors


def validate_forbidden_words(root: Path) -> list[str]:
    errors: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in {".csv", ".md"}:
            continue
        for index, line in enumerate(path.read_text().splitlines(), start=1):
            if FORBIDDEN.search(line):
                errors.append(f"{path}:{index}: forbidden closure wording: {line}")
    return errors


def validate_agent_status(artifact_root: Path) -> list[str]:
    status_path = artifact_root / "AGENT_EXECUTION_STATUS.csv"
    if not status_path.exists():
        return [f"missing agent status file: {status_path}"]
    rows = list(csv.DictReader(status_path.open(newline="")))
    errors: list[str] = []
    if not rows:
        errors.append("agent status has no rows")
    for row in rows:
        for column in (
            "slice_id",
            "agent_role",
            "assignment_status",
            "owned_scope",
            "last_refresh_utc",
            "last_test_gate",
            "last_test_result",
            "next_action",
            "evidence_path",
        ):
            if not row.get(column):
                errors.append(f"agent status row {row.get('slice_id', '<missing>')} missing {column}")
    return errors


def validate_seed_only_status_claims(artifact_root: Path) -> list[str]:
    # Final-vs-seed closure claims are validated by
    # firebird_final_surface_audit_gate.py. This generic artifact gate stays
    # runnable while the execution_plan is active so status/schema drift is caught
    # without requiring final closure evidence on every incremental patch.
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", required=True)
    args = parser.parse_args()

    execution_plan_root = Path(args.execution_plan_root)
    artifact_root = execution_plan_root / "artifacts"
    errors: list[str] = []
    errors.extend(validate_csv_widths(execution_plan_root))
    errors.extend(validate_required_artifacts(artifact_root))
    errors.extend(validate_forbidden_words(execution_plan_root))
    errors.extend(validate_agent_status(artifact_root))
    errors.extend(validate_seed_only_status_claims(artifact_root))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("firebird execution_plan artifacts validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
