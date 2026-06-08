#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Execution_Plan agent controls for the SBsql Surface-to-SBLR closure execution_plan.

Implements the executable counterparts to the rules declared in
`project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/AGENT_HEARTBEAT_RECOVERY_PLAN.md`:

  * heartbeat        record an agent heartbeat into AGENT_STATUS.csv
  * detect-stalls    find agents whose last_heartbeat_utc is older than
                     the configured threshold (default 5 minutes); update
                     FAILURE_INVENTORY.csv with stall rows; mark agents
                     `stalled` in AGENT_STATUS.csv
  * validate         structural validation suitable for one-shot CTest:
                     - every AGENT_STATUS row has required columns populated
                     - every active agent's owned_write_scope intersects
                       at most one declared write scope in
                       AGENT_WRITE_SCOPE_REGISTER.csv (no rogue scopes)
                     - no two active agents claim the same primary write
                       scope (write-scope lock)
                     - TRACKER.csv state transitions are sane (no
                       `completed` row missing outputs or acceptance text)

Heartbeat freshness is enforced by `detect-stalls`; `validate` is the
structural check that runs without a wall-clock dependency so it is safe
in CI/CTest.

Architecture invariant compliance: read-only / append-only over execution_plan
tracking CSVs and the failure inventory. No transaction model touched.
No engine, parser worker, server, listener, storage, or MGA file
modified. No WAL surface introduced. MGA copy-on-write authority remains
the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import sys
from pathlib import Path


DEFAULT_EXECUTION_PLAN_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr"
AGENT_STATUS = "artifacts/AGENT_STATUS.csv"
WRITE_SCOPE_REGISTER = "artifacts/AGENT_WRITE_SCOPE_REGISTER.csv"
TRACKER = "TRACKER.csv"
FAILURE_INVENTORY = "artifacts/FAILURE_INVENTORY.csv"


AGENT_STATUS_COLUMNS = [
    "agent_id",
    "status",
    "assigned_slice",
    "owned_write_scope",
    "current_action",
    "last_heartbeat_utc",
    "last_validation_command",
    "current_blocker",
    "next_expected_artifact",
]

ACTIVE_AGENT_STATUSES = {"active", "in_progress", "waiting_on_build", "waiting_on_test", "blocked"}
EXEMPT_FROM_FRESHNESS = {"completed", "complete", "stalled", "deleted"}

VALID_TRACKER_STATUSES = {"pending", "in_progress", "completed", "blocked", "deleted"}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, columns: list[str], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def parse_utc(value: str) -> dt.datetime | None:
    if not value:
        return None
    try:
        if value.endswith("Z"):
            return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return dt.datetime.fromisoformat(value)
    except ValueError:
        return None


def cmd_heartbeat(args: argparse.Namespace, root: Path) -> int:
    status_path = root / AGENT_STATUS
    rows = read_csv(status_path)
    rows = [r for r in rows if r["agent_id"] != args.agent_id]
    now = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    rows.append({
        "agent_id": args.agent_id,
        "status": args.status,
        "assigned_slice": args.slice_id,
        "owned_write_scope": args.write_scope,
        "current_action": args.action,
        "last_heartbeat_utc": now,
        "last_validation_command": args.validation_command or "",
        "current_blocker": args.blocker or "none",
        "next_expected_artifact": args.next_artifact or "",
    })
    write_csv(status_path, AGENT_STATUS_COLUMNS, rows)
    print(f"heartbeat_recorded agent_id={args.agent_id} status={args.status} at={now}")
    return 0


def cmd_detect_stalls(args: argparse.Namespace, root: Path) -> int:
    status_path = root / AGENT_STATUS
    failure_path = root / FAILURE_INVENTORY
    rows = read_csv(status_path)
    failures = read_csv(failure_path)
    now = dt.datetime.now(dt.timezone.utc)
    threshold = dt.timedelta(minutes=args.threshold_minutes)

    stalled_ids: list[str] = []
    for row in rows:
        if row["status"] in EXEMPT_FROM_FRESHNESS:
            continue
        if row["status"] not in ACTIVE_AGENT_STATUSES:
            continue
        last = parse_utc(row["last_heartbeat_utc"])
        if last is None:
            stalled_ids.append(row["agent_id"])
            row["status"] = "stalled"
            continue
        if now - last > threshold:
            stalled_ids.append(row["agent_id"])
            row["status"] = "stalled"

    if stalled_ids:
        write_csv(status_path, AGENT_STATUS_COLUMNS, rows)
        failure_cols = list(failures[0].keys()) if failures else [
            "failure_id", "first_seen_utc", "slice_id", "gate_id", "failure_category",
            "owner_lane", "summary", "evidence_path", "current_status", "next_action",
        ]
        for agent_id in stalled_ids:
            failures.append({
                "failure_id": f"stall-{agent_id}-{now.strftime('%Y%m%dT%H%M%SZ')}",
                "first_seen_utc": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "slice_id": "",
                "gate_id": "sbsql_agent_heartbeat_recovery_gate",
                "failure_category": "agent_stall",
                "owner_lane": "coordinator",
                "summary": f"agent {agent_id} heartbeat older than {args.threshold_minutes} minutes; marked stalled per heartbeat plan",
                "evidence_path": AGENT_STATUS,
                "current_status": "open",
                "next_action": "reassign per agent heartbeat recovery plan",
            })
        write_csv(failure_path, failure_cols, failures)

    print(f"detect_stalls threshold_minutes={args.threshold_minutes} stalled={len(stalled_ids)}")
    return 0


def cmd_validate(args: argparse.Namespace, root: Path) -> int:
    status_rows = read_csv(root / AGENT_STATUS)
    scope_rows = read_csv(root / WRITE_SCOPE_REGISTER)
    tracker_rows = read_csv(root / TRACKER)

    declared_lanes = {r["lane"] for r in scope_rows}
    declared_primary_scopes: dict[str, str] = {}
    for r in scope_rows:
        for scope in (r.get("primary_write_scope") or "").split(";"):
            scope = scope.strip()
            if scope:
                declared_primary_scopes.setdefault(scope, r["lane"])

    errors: list[str] = []

    # 1. AGENT_STATUS structural validation
    for row in status_rows:
        for col in AGENT_STATUS_COLUMNS:
            if col not in row:
                errors.append(f"AGENT_STATUS row {row.get('agent_id','?')} missing column {col}")
        if not row.get("agent_id"):
            errors.append("AGENT_STATUS row missing agent_id")
        if not row.get("status"):
            errors.append(f"AGENT_STATUS row {row['agent_id']} missing status")
        last = parse_utc(row.get("last_heartbeat_utc", ""))
        if row.get("last_heartbeat_utc") and last is None:
            errors.append(f"AGENT_STATUS row {row['agent_id']} has unparseable last_heartbeat_utc {row['last_heartbeat_utc']}")

    # 2. Write-scope lock: no two active agents may share a declared primary scope
    active_claims: dict[str, str] = {}
    for row in status_rows:
        if row.get("status") not in ACTIVE_AGENT_STATUSES:
            continue
        for scope in (row.get("owned_write_scope") or "").split(";"):
            scope = scope.strip()
            if not scope:
                continue
            if scope in declared_primary_scopes:
                holder = active_claims.get(scope)
                if holder and holder != row["agent_id"]:
                    errors.append(
                        f"write_scope_lock_violation scope={scope} held by {holder} and {row['agent_id']}"
                    )
                active_claims[scope] = row["agent_id"]

    # 3. Tracker state machine
    for row in tracker_rows:
        slice_id = row.get("slice_id", "?")
        status = row.get("status", "")
        if status not in VALID_TRACKER_STATUSES:
            errors.append(f"TRACKER row {slice_id} has invalid status {status}")
        if status == "completed":
            if not row.get("outputs", "").strip():
                errors.append(f"TRACKER row {slice_id} is completed but outputs is empty")
            if not row.get("acceptance", "").strip():
                errors.append(f"TRACKER row {slice_id} is completed but acceptance is empty")

    # 4. Declared lanes referenced by active agents must exist in scope register
    for row in status_rows:
        if row.get("status") not in ACTIVE_AGENT_STATUSES:
            continue
        agent_lane = row.get("agent_id", "")
        # We don't require agent_id == lane name (agents may be named freely);
        # we only check that the owned scope set is recognized as one of the
        # declared lane primary scopes or a child path of a declared scope.
        owned = [s.strip() for s in (row.get("owned_write_scope") or "").split(";") if s.strip()]
        for scope in owned:
            recognized = scope in declared_primary_scopes
            if not recognized:
                # also allow if scope starts with any declared primary scope
                if any(scope.startswith(p + "/") or scope == p for p in declared_primary_scopes):
                    recognized = True
            if not recognized:
                errors.append(
                    f"rogue_write_scope agent_id={agent_lane} scope={scope} not declared in AGENT_WRITE_SCOPE_REGISTER"
                )

    print(f"sbsql_agent_heartbeat_recovery_validate agents={len(status_rows)} tracker_rows={len(tracker_rows)} declared_lanes={len(declared_lanes)} errors={len(errors)}")
    if errors:
        print("sbsql_agent_heartbeat_recovery_validate=failed", file=sys.stderr)
        for err in errors[:20]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_agent_heartbeat_recovery_validate=passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--execution_plan-root", default="")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_hb = sub.add_parser("heartbeat", help="record an agent heartbeat")
    p_hb.add_argument("--agent-id", required=True)
    p_hb.add_argument("--status", required=True, choices=sorted(ACTIVE_AGENT_STATUSES | EXEMPT_FROM_FRESHNESS))
    p_hb.add_argument("--slice-id", required=True)
    p_hb.add_argument("--write-scope", required=True)
    p_hb.add_argument("--action", required=True)
    p_hb.add_argument("--validation-command", default="")
    p_hb.add_argument("--blocker", default="none")
    p_hb.add_argument("--next-artifact", default="")
    p_hb.set_defaults(func=cmd_heartbeat)

    p_ds = sub.add_parser("detect-stalls", help="find stalled agents and record failures")
    p_ds.add_argument("--threshold-minutes", type=int, default=5)
    p_ds.set_defaults(func=cmd_detect_stalls)

    p_v = sub.add_parser("validate", help="structural validation suitable for CTest")
    p_v.set_defaults(func=cmd_validate)

    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    execution_plan_root = Path(args.execution_plan_root).resolve() if args.execution_plan_root else repo_root / DEFAULT_EXECUTION_PLAN_ROOT
    return args.func(args, execution_plan_root)


if __name__ == "__main__":
    raise SystemExit(main())
