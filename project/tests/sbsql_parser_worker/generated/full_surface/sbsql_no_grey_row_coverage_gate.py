#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""No-grey row coverage gate for the SBsql Surface-to-SBLR regression fixtures.

Reads `STRICT_ROW_COVERAGE_LEDGER.csv` (produced by
`project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py`) and the
authoritative `ROW_STATE_CONTRACT.csv`, then refuses to pass if any SBsql
surface row lacks an exact final state or row-level implementation evidence.

The gate fails by design at execution_plan baseline and passes only when every row
has been promoted out of the in-progress states (`inventory_only`,
`lowering_family_mapped`, `parser_bound`, `sblr_verified`, `server_admitted`,
`engine_runtime_implemented`) into a final state with row-specific
function/API operation id and fixture evidence:

- native_now rows MUST reach `e2e_passed`.
- native_future rows MUST reach `e2e_passed` (after SBSFC-003 promotion) or
  must have been removed from the canonical SBsql surface registry by spec
  change.
- cluster_private rows MUST reach `exact_refusal_passed` for the public
  build, and may additionally carry `private_profile_implemented` evidence
  where private cluster authority is available.

For native_now rows, the gate also forbids:

- `function_or_api_operation_id` left at `family_only` or `blocked_by_engine_gap`
- `fixture_evidence` left at `pending_row_level`
- `evidence_complete` left at `no`

For cluster_private rows, the gate forbids `evidence_complete=no`. The
`function_or_api_operation_id` and `fixture_evidence` fields are allowed to
remain `family_only` and `pending_row_level` only when the row is at
`exact_refusal_passed` (refusal route may legitimately not have a row-specific
function id, but it must still have a refusal-fixture path that is not
`pending_row_level`).

Architecture invariant compliance: read-only check; no transaction model
touched; no WAL surface introduced. MGA copy-on-write remains the sole
transaction recovery model.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
LEDGER_NAME = "STRICT_ROW_COVERAGE_LEDGER.csv"
STATE_CONTRACT_NAME = "ROW_STATE_CONTRACT.csv"


REQUIRED_COLUMNS = [
    "surface_id",
    "canonical_name",
    "status",
    "cluster_scope",
    "surface_kind",
    "sblr_operation_family",
    "current_state",
    "parser_evidence",
    "binder_evidence",
    "lowering_evidence",
    "server_admission_evidence",
    "engine_runtime_evidence",
    "function_or_api_operation_id",
    "diagnostic_evidence",
    "fixture_evidence",
    "evidence_complete",
    "notes",
]


FORBIDDEN_FUNCTION_ID_VALUES = {"family_only", "blocked_by_engine_gap", "", "none"}
FORBIDDEN_FIXTURE_VALUES = {"pending_row_level", "", "none"}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    ledger = read_csv(artifact_root / LEDGER_NAME)
    state_contract = read_csv(artifact_root / STATE_CONTRACT_NAME)

    if not ledger:
        fail("STRICT_ROW_COVERAGE_LEDGER.csv is empty")
    missing_cols = [c for c in REQUIRED_COLUMNS if c not in ledger[0]]
    if missing_cols:
        fail(f"ledger missing required columns: {', '.join(missing_cols)}")

    valid_states = {row["state"] for row in state_contract}
    final_states = {
        row["state"] for row in state_contract if row["allowed_final_state"] in {"yes", "conditional"}
    }

    seen_ids: set[str] = set()
    state_violations: list[str] = []
    native_now_state_violations: list[str] = []
    native_future_state_violations: list[str] = []
    cluster_private_state_violations: list[str] = []
    function_id_violations: list[str] = []
    fixture_violations: list[str] = []
    evidence_complete_violations: list[str] = []
    unknown_state_rows: list[str] = []

    for row in ledger:
        surface_id = row["surface_id"]
        if not surface_id:
            fail("ledger row missing surface_id")
        if surface_id in seen_ids:
            fail(f"ledger duplicate surface_id: {surface_id}")
        seen_ids.add(surface_id)

        for column in REQUIRED_COLUMNS:
            if not row.get(column, "").strip():
                fail(f"{surface_id} ledger row missing {column}")

        state = row["current_state"]
        if state not in valid_states:
            unknown_state_rows.append(surface_id)
            continue

        status = row["status"]
        if state not in final_states:
            state_violations.append(surface_id)
            continue

        cluster_private_public_row = (
            status == "cluster_private" or row.get("cluster_scope") == "cluster_private"
        )

        if cluster_private_public_row:
            if state not in {"exact_refusal_passed", "private_profile_implemented"}:
                cluster_private_state_violations.append(surface_id)
        elif status == "native_now" and state != "e2e_passed":
            native_now_state_violations.append(surface_id)
        if status == "native_future" and state != "e2e_passed":
            native_future_state_violations.append(surface_id)

        if status == "native_now" and not cluster_private_public_row:
            if row["function_or_api_operation_id"] in FORBIDDEN_FUNCTION_ID_VALUES:
                function_id_violations.append(surface_id)
            if row["fixture_evidence"] in FORBIDDEN_FIXTURE_VALUES:
                fixture_violations.append(surface_id)

        if cluster_private_public_row:
            if row["fixture_evidence"] in FORBIDDEN_FIXTURE_VALUES:
                fixture_violations.append(surface_id)

        if row["evidence_complete"] != "yes":
            evidence_complete_violations.append(surface_id)

    total = len(ledger)

    print(f"sbsql_no_grey_row_coverage_gate total_rows={total}")
    print(f"  current_state_distribution={dict(Counter(r['current_state'] for r in ledger))}")
    print(f"  unknown_state_rows={len(unknown_state_rows)}")
    print(f"  rows_not_in_final_state={len(state_violations)}")
    print(f"  native_now_not_e2e_passed={len(native_now_state_violations)}")
    print(f"  native_future_not_e2e_passed={len(native_future_state_violations)}")
    print(f"  cluster_private_not_exact_refusal_or_private_profile={len(cluster_private_state_violations)}")
    print(f"  native_now_function_or_api_operation_id_grey={len(function_id_violations)}")
    print(f"  fixture_evidence_grey={len(fixture_violations)}")
    print(f"  evidence_complete_no={len(evidence_complete_violations)}")

    any_violations = (
        unknown_state_rows
        or state_violations
        or native_now_state_violations
        or native_future_state_violations
        or cluster_private_state_violations
        or function_id_violations
        or fixture_violations
        or evidence_complete_violations
    )

    if any_violations:
        sample = sorted(set(
            unknown_state_rows[:3]
            + state_violations[:3]
            + function_id_violations[:3]
            + fixture_violations[:3]
        ))[:6]
        print(f"sbsql_no_grey_row_coverage_gate=failed sample_violation_ids={sample}", file=sys.stderr)
        return 1

    print("sbsql_no_grey_row_coverage_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
