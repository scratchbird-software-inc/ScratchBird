#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate AI conformance evidence artifacts against spec gate contracts.

This validator enforces the public artifact rules defined in:
docs/releases/EARLY_BETA_CONFORMANCE_GATES.md
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

EVIDENCE_ROW_PREFIX = "| `EVID-"
REQUIRED_JSON_FIELDS = {
    "generated_at_utc",
    "git_commit",
    "status",
    "check_count",
    "passed_checks",
    "failed_checks",
}
ALLOWED_STATUS = {"PASS", "FAIL"}


@dataclass(frozen=True)
class EvidenceRow:
    evidence_id: str
    artifact_paths: list[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate ScratchBird AI evidence gates")
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root containing docs/ and artifacts/ (default: current directory)",
    )
    parser.add_argument(
        "--artifact-root",
        default=None,
        help=(
            "Directory containing generated artifact families. When supplied, "
            "spec paths beginning with artifacts/ are resolved below this root."
        ),
    )
    parser.add_argument(
        "--spec",
        default="docs/releases/EARLY_BETA_CONFORMANCE_GATES.md",
        help="Path to evidence traceability spec file relative to repo root",
    )
    parser.add_argument(
        "--max-age-days",
        type=int,
        default=14,
        help="Maximum artifact age in days before stale failure (default: 14)",
    )
    parser.add_argument(
        "--release-time-utc",
        default=None,
        help="Override release evaluation time in UTC ISO-8601 (e.g., 2026-02-24T18:00:00Z)",
    )
    return parser.parse_args()


def resolve_artifact_path(repo_root: Path, artifact_root: Path | None, rel_path: str) -> Path:
    candidate = Path(rel_path)
    if candidate.is_absolute():
        return candidate
    if artifact_root is not None:
        parts = candidate.parts
        if parts and parts[0] == "artifacts":
            return artifact_root.joinpath(*parts[1:])
    return repo_root / candidate


def parse_utc(ts: str) -> datetime:
    value = ts.strip()
    if value.endswith("Z"):
        value = value[:-1] + "+00:00"
    dt = datetime.fromisoformat(value)
    if dt.tzinfo is None:
        raise ValueError("timestamp must include timezone")
    return dt.astimezone(timezone.utc)


def parse_evidence_rows(spec_text: str) -> list[EvidenceRow]:
    rows: list[EvidenceRow] = []
    in_section = False

    for line in spec_text.splitlines():
        if line.startswith("## 3. Evidence IDs and Gates"):
            in_section = True
            continue
        if in_section and line.startswith("## "):
            break
        if not in_section:
            continue
        if not line.startswith(EVIDENCE_ROW_PREFIX):
            continue

        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) < 6:
            raise ValueError(f"invalid evidence table row: {line}")

        evidence_id = cells[0].strip("`")
        artifact_cell = cells[5]
        artifact_paths = re.findall(r"`([^`]+)`", artifact_cell)
        if not artifact_paths:
            raise ValueError(f"evidence row has no artifact paths: {line}")

        rows.append(EvidenceRow(evidence_id=evidence_id, artifact_paths=artifact_paths))

    if not rows:
        raise ValueError("no evidence rows parsed from spec section 3")
    return rows


def validate_json_artifact(
    *,
    rel_path: str,
    full_path: Path,
    release_time: datetime,
    max_age_days: int,
    errors: list[str],
    commits: dict[str, list[str]],
) -> None:
    try:
        payload = json.loads(full_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        errors.append(f"{rel_path}: invalid JSON ({exc})")
        return

    if not isinstance(payload, dict):
        errors.append(f"{rel_path}: root must be JSON object")
        return

    missing = REQUIRED_JSON_FIELDS - set(payload.keys())
    if missing:
        errors.append(f"{rel_path}: missing required fields {sorted(missing)}")
        return

    generated = payload["generated_at_utc"]
    git_commit = payload["git_commit"]
    status = payload["status"]
    check_count = payload["check_count"]
    passed_checks = payload["passed_checks"]
    failed_checks = payload["failed_checks"]

    if not isinstance(generated, str):
        errors.append(f"{rel_path}: generated_at_utc must be string")
        return
    if not isinstance(git_commit, str) or not git_commit.strip():
        errors.append(f"{rel_path}: git_commit must be non-empty string")
        return
    if status not in ALLOWED_STATUS:
        errors.append(f"{rel_path}: status must be one of {sorted(ALLOWED_STATUS)}")
        return
    if not isinstance(check_count, int) or check_count < 0:
        errors.append(f"{rel_path}: check_count must be integer >= 0")
        return
    if not isinstance(passed_checks, int) or passed_checks < 0:
        errors.append(f"{rel_path}: passed_checks must be integer >= 0")
        return
    if not isinstance(failed_checks, list) or not all(
        isinstance(item, str) for item in failed_checks
    ):
        errors.append(f"{rel_path}: failed_checks must be array of strings")
        return

    try:
        generated_dt = parse_utc(generated)
    except ValueError as exc:
        errors.append(f"{rel_path}: generated_at_utc invalid ({exc})")
        return

    if generated_dt < release_time - timedelta(days=max_age_days):
        errors.append(
            f"{rel_path}: stale artifact (generated_at_utc={generated_dt.isoformat()}, "
            f"max_age_days={max_age_days})"
        )

    if status == "PASS":
        if len(failed_checks) != 0:
            errors.append(
                f"{rel_path}: PASS artifacts must have empty failed_checks"
            )
        if passed_checks != check_count:
            errors.append(
                f"{rel_path}: PASS artifacts must satisfy passed_checks == check_count"
            )
    else:
        if len(failed_checks) == 0:
            errors.append(
                f"{rel_path}: FAIL artifacts must include at least one failed check"
            )
        errors.append(f"{rel_path}: status=FAIL blocks release")

    commits.setdefault(git_commit, []).append(rel_path)


def validate_csv_artifact(*, rel_path: str, full_path: Path, errors: list[str]) -> None:
    try:
        with full_path.open("r", encoding="utf-8", newline="") as handle:
            rows = list(csv.reader(handle))
    except OSError as exc:
        errors.append(f"{rel_path}: unable to read CSV ({exc})")
        return

    if len(rows) < 2:
        errors.append(f"{rel_path}: CSV must contain header row and at least one data row")
        return

    header = rows[0]
    if not header or all(not cell.strip() for cell in header):
        errors.append(f"{rel_path}: CSV header row must contain at least one non-empty column")


def validate_junit_artifact(*, rel_path: str, full_path: Path, errors: list[str]) -> None:
    try:
        tree = ET.parse(full_path)
    except ET.ParseError as exc:
        errors.append(f"{rel_path}: invalid XML ({exc})")
        return
    except OSError as exc:
        errors.append(f"{rel_path}: unable to read XML ({exc})")
        return

    testcases = tree.findall(".//testcase")
    if len(testcases) < 1:
        errors.append(f"{rel_path}: JUnit XML must contain at least one <testcase>")


def main() -> None:
    args = parse_args()

    repo_root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root).resolve() if args.artifact_root else None
    spec_path = repo_root / args.spec
    if not spec_path.exists():
        print(f"ERROR: spec file not found: {spec_path}", file=sys.stderr)
        raise SystemExit(1)

    if args.release_time_utc:
        try:
            release_time = parse_utc(args.release_time_utc)
        except ValueError as exc:
            print(f"ERROR: invalid --release-time-utc ({exc})", file=sys.stderr)
            raise SystemExit(1)
    else:
        release_time = datetime.now(timezone.utc)

    try:
        rows = parse_evidence_rows(spec_path.read_text(encoding="utf-8"))
    except ValueError as exc:
        print(f"ERROR: unable to parse evidence spec ({exc})", file=sys.stderr)
        raise SystemExit(1)

    errors: list[str] = []
    commits: dict[str, list[str]] = {}

    for row in rows:
        if not row.artifact_paths:
            errors.append(f"{row.evidence_id}: no artifact paths declared")
            continue

        for rel_path in row.artifact_paths:
            full_path = resolve_artifact_path(repo_root, artifact_root, rel_path)
            if not full_path.exists():
                errors.append(f"{row.evidence_id}: missing artifact {rel_path}")
                continue

            if full_path.suffix == ".json":
                validate_json_artifact(
                    rel_path=rel_path,
                    full_path=full_path,
                    release_time=release_time,
                    max_age_days=args.max_age_days,
                    errors=errors,
                    commits=commits,
                )
                continue

            if full_path.suffix == ".csv":
                validate_csv_artifact(rel_path=rel_path, full_path=full_path, errors=errors)
                continue

            if full_path.name.endswith(".junit.xml"):
                validate_junit_artifact(
                    rel_path=rel_path,
                    full_path=full_path,
                    errors=errors,
                )
                continue

            errors.append(
                f"{row.evidence_id}: unsupported artifact type for {rel_path} "
                f"(expected .json, .csv, or .junit.xml)"
            )

    if len(commits) > 1:
        details = ", ".join(
            f"{commit} ({len(paths)} artifacts)" for commit, paths in sorted(commits.items())
        )
        errors.append(
            "all proof artifacts for a release candidate must reference the same git_commit; "
            f"found multiple: {details}"
        )

    if errors:
        print("ERROR: evidence gate validation failed", file=sys.stderr)
        for err in errors:
            print(f" - {err}", file=sys.stderr)
        raise SystemExit(1)

    total_paths = sum(len(row.artifact_paths) for row in rows)
    print(
        "OK: evidence gates valid "
        f"(rows={len(rows)}, artifacts={total_paths}, git_commit={next(iter(commits), 'n/a')})"
    )


if __name__ == "__main__":
    main()
