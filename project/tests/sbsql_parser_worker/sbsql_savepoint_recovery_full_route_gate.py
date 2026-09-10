#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Actual server SIGKILL/restart at native savepoint and typed DML boundaries."""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    isql_args, isql_data_lines, require_isql_success, run_isql,
    start_route, stop_process, stop_route,
)
from sbsql_whole_store_recovery_full_route_gate import (
    TABLE, clone_database, setup_template, verify_reopened_state,
)
from sbsql_savepoint_rollback_full_route_gate import clean_passed_database

BOUNDARIES = (
    "rollback_before_marker", "rollback_after_marker", "rollback_before_coordinator_evidence",
    "update_after_intent", "update_after_prepared", "update_after_barrier",
    "delete_after_native_marker", "delete_after_intent", "delete_after_rows",
    "delete_after_indexes", "delete_after_prepared", "delete_after_barrier",
)
PROFILES = {
    "integer": "id = 3",
    "text": "payload = 'discard'",
    "overflow": "payload = '" + "x" * 12000 + "'",
    "null": "payload = NULL",
}


def marker_hash(database: Path) -> bytes:
    path = Path(str(database) + ".sb.mga_savepoints")
    return hashlib.sha256(path.read_bytes() if path.exists() else b"").digest()


def run_case(args: argparse.Namespace, root: Path, template: Path, point: str, profile: str) -> None:
    database = root / "sp.sbdb"
    clone_database(template, database)
    marker = root / "crash.marker"
    env = {
        "SCRATCHBIRD_TEST_SAVEPOINT_CRASH_ARM": "issue8-savepoint-route",
        "SCRATCHBIRD_TEST_SAVEPOINT_CRASH_POINT": point,
        "SCRATCHBIRD_TEST_SAVEPOINT_CRASH_MARKER": str(marker),
        "SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_TRIGGER_VALUE": "issue8-arm",
    }
    route = None
    transaction = 0
    try:
        route = start_route(args, root / "crash", database, tls_required=False, extra_env=env)
        script = root / "crash.sbsql"
        script.write_text("\n".join([
            f"INSERT INTO {TABLE} (id, payload) VALUES (4, 'issue8-arm');",
            "SAVEPOINT outer_sp;",
            f"INSERT INTO {TABLE} (id, payload) VALUES (5, 'later');",
            "SAVEPOINT inner_sp;",
            f"UPDATE {TABLE} SET {PROFILES[profile]} WHERE id = 1;",
            *([f"DELETE FROM {TABLE} WHERE id = {3 if profile == 'integer' else 1};"]
              if point.startswith("delete_") else []),
            "ROLLBACK TO SAVEPOINT outer_sp;",
            # No COMMIT: even a published UPDATE statement cannot decide the
            # owning transaction's outcome after the server loses its session.
            "",
        ]), encoding="utf-8")
        with (root / "client.stdout.log").open("wb") as stdout, (root / "client.stderr.log").open("wb") as stderr:
            client = subprocess.Popen(isql_args(args, route) + ["-q", "-A", "-t", "-b", "-f", str(script)],
                                      stdout=stdout, stderr=stderr)
            try:
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline and route.server.poll() is None:
                    if client.poll() is not None and not marker.exists():
                        raise RuntimeError("client ended before the selected engine boundary")
                    time.sleep(0.025)
                if route.server.poll() not in (-9, 137) or not marker.is_file():
                    raise RuntimeError("selected engine boundary did not terminate SBsrv with SIGKILL")
                fields = dict(line.split("=", 1) for line in marker.read_text().splitlines())
                transaction = int(fields.get("local_transaction_id", "0"))
                if (fields.get("boundary") != point or int(fields.get("pid", "0")) != route.server.pid or
                        fields.get("authority") != "durable_mga_transaction_inventory" or transaction <= 0):
                    raise RuntimeError("engine boundary evidence has the wrong owner or point")
            finally:
                stop_process(client)
    finally:
        stop_route(route)

    previous_markers = None
    for repeat in range(2):
        route = None
        try:
            route = start_route(args, root / f"restart_{repeat}", database, tls_required=False)
            verify_reopened_state(args, route, mutation_committed=False,
                                  attempted_transaction_id=transaction)
            current = marker_hash(database)
            if previous_markers is not None and current != previous_markers:
                raise RuntimeError("repeated reopen appended another native rewind/compensation range")
            previous_markers = current
        finally:
            stop_route(route)

    route = None
    try:
        route = start_route(args, root / "subsequent_write", database, tls_required=False)
        result = run_isql(args, route, "reuse_after_recovery", "\n".join([
            f"INSERT INTO {TABLE} (id, payload) VALUES (4, 'after');",
            "SAVEPOINT fresh_sp;",
            f"UPDATE {TABLE} SET id = 3 WHERE id = 1;",
            "ROLLBACK TO SAVEPOINT fresh_sp;", "ROLLBACK TO SAVEPOINT fresh_sp;",
            f"SELECT payload FROM {TABLE} WHERE id = 1;",
            f"SELECT COUNT(*) FROM {TABLE} WHERE id = 3;",
            "RELEASE SAVEPOINT fresh_sp;", "COMMIT;",
            f"SELECT payload FROM {TABLE} WHERE id = 4;", "",
        ]))
        require_isql_success(result)
        probes = [line for line in isql_data_lines(result)
                  if not line.startswith("Rows affected: ")]
        if probes != ["seed-1", "0", "after"]:
            raise RuntimeError(f"subsequent write, key reuse or repeated rewind failed: {probes!r}")
    finally:
        stop_route(route)


def main() -> int:
    parser = argparse.ArgumentParser()
    for flag in ("server", "listener", "parser-worker", "sb-isql", "example-db-seeder", "work-dir"):
        parser.add_argument("--" + flag, required=True)
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="sbsprc_"))
    requested = Path(args.work_dir)
    requested.mkdir(parents=True, exist_ok=True)
    (requested / "artifact_path.txt").write_text(str(work) + "\n")
    cases = {f"{point}_{profile}": (point, profile) for point in BOUNDARIES for profile in PROFILES}
    if args.case:
        unknown = set(args.case) - cases.keys()
        if unknown:
            parser.error(f"unknown cases: {sorted(unknown)}")
        cases = {name: cases[name] for name in args.case}
    template = setup_template(args, work)
    failures = []
    for ordinal, (name, (point, profile)) in enumerate(cases.items()):
        # AF_UNIX limits include the listener's generated management name.
        # Keep descriptive case names in evidence, not in socket directories.
        case_root = work / f"c{ordinal:02d}"
        case_root.mkdir(parents=True, exist_ok=True)
        (case_root / "case.txt").write_text(name + "\n", encoding="utf-8")
        try:
            run_case(args, case_root, template, point, profile)
            clean_passed_database(case_root)
            print(f"{name}=passed", flush=True)
        except Exception as error:
            failures.append(name)
            print(f"{name}=failed {error}", flush=True)
    # Every case owns an independent copy; failed cases retain theirs for
    # debugging. The shared template is now redundant even after a failure.
    for artifact in template.parent.glob(template.name + "*"):
        if artifact.is_dir() and not artifact.is_symlink():
            shutil.rmtree(artifact)
        else:
            artifact.unlink()
    print(f"savepoint_recovery_full_route cases={len(cases)} failures={len(failures)} artifacts={work}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
