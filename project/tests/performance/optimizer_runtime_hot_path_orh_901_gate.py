#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ORH-901 final audit and movement-readiness validator."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path


EXECUTION_PLAN = "docs" "/completed-execution-plans/optimizer-runtime-hot-path-operationalization-closure"
TRACKER_DEPRECATED = {"ORH-212", "ORH-270", "ORH-288", "ORH-900"}
GATE_DEPRECATED = {
    "ORH-GATE-212",
    "ORH-GATE-270",
    "ORH-GATE-288",
    "ORH-GATE-900",
}
AUDIT_DEPRECATED = {
    "ORH-AUDIT-212",
    "ORH-AUDIT-270",
    "ORH-AUDIT-288",
    "ORH-AUDIT-900",
}
REQUIRED_FINAL_AUDIT_TOKENS = {
    "Status: completed",
    "Movement readiness: `movement_ready`",
    "This package is eligible to move to `docs" "/completed-execution-plans/`",
    "ORH_SUCCESSOR_AUDIT_SCOPE_DEPRECATED",
    "ORH_DONOR_DOMINANCE_NOT_ACHIEVED",
    "ORH_LARGE_SCALE_BENCHMARK_TIER_UNAVAILABLE",
    "ORH_FOCUSED_LIVE_ROUTE_BLOCKED",
    "ORH_FINAL_AUDIT_MOVEMENT_READY",
    "all 24 donor",
    "Donor dominance is unclaimed",
    "Live route benchmark-clean closure is",
    "Parser, client, driver, and donor evidence remains advisory",
}
FORBIDDEN_FINAL_AUDIT_TOKENS = {
    "movement_not_ready",
    "Package moved to `docs" "/completed-execution-plans/`",
    "donor dominance is claimed",
    "live route closure is complete",
    "benchmark-clean live-route closure is complete",
}


def fail(message: str) -> None:
    print(f"ORH-901 gate failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def load_rows(path: Path) -> list[list[str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.reader(handle))
    except Exception as exc:  # pragma: no cover - diagnostic path
        fail(f"unable to read {path}: {exc}")


def status_map(path: Path, status_index: int) -> dict[str, str]:
    rows = load_rows(path)
    require(len(rows) > 1, f"{path} has no data rows")
    return {row[0]: row[status_index] for row in rows[1:]}


def require_statuses(
    statuses: dict[str, str],
    deprecated: set[str],
    row_name: str,
) -> None:
    pending = {key for key, status in statuses.items() if status == "pending"}
    require(not pending, f"{row_name} still has pending rows: {sorted(pending)}")
    actual_blocked = {
        key for key, status in statuses.items() if status == "completed-blocked"
    }
    require(
        not actual_blocked,
        f"{row_name} still has completed-blocked rows: {sorted(actual_blocked)}",
    )
    actual_deprecated = {
        key for key, status in statuses.items() if status == "deprecated"
    }
    require(
        actual_deprecated == deprecated,
        f"{row_name} deprecated rows mismatch: {sorted(actual_deprecated)}",
    )


def validate_final_audit(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    for token in REQUIRED_FINAL_AUDIT_TOKENS:
        require(token in text, f"FINAL_AUDIT missing token: {token}")
    for token in FORBIDDEN_FINAL_AUDIT_TOKENS:
        require(token not in text, f"FINAL_AUDIT contains forbidden overclaim: {token}")


def validate(repo_root: Path) -> None:
    root = repo_root / EXECUTION_PLAN
    require_statuses(
        status_map(root / "TRACKER.csv", 3),
        TRACKER_DEPRECATED,
        "TRACKER.csv",
    )
    require_statuses(
        status_map(root / "ACCEPTANCE_GATES.csv", 2),
        GATE_DEPRECATED,
        "ACCEPTANCE_GATES.csv",
    )
    require_statuses(
        status_map(root / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv", 5),
        AUDIT_DEPRECATED,
        "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
    )
    validate_final_audit(root / "artifacts" / "FINAL_AUDIT.md")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    args = parser.parse_args()
    validate(args.repo_root)
    print("ORH-901 final audit movement gate passed: completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
