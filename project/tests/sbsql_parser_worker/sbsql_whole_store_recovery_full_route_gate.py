#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Kill/restart proof for whole-store recovery through the real SBSql route."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from sbsql_copy_persistence_full_route_gate import (
    CopyPersistenceError,
    StartedRoute,
    isql_args,
    isql_data_lines,
    require_isql_success,
    run_isql,
    start_route,
    stop_process,
    stop_route,
)


TABLE = "sbsfc021_stream_table"
MUTATION_ID = 100
CRASH_ARM = "issue6-real-dml-route"
EARLY_BOUNDARIES = (
    "allocation",
    "partial_page_write",
    "page_sync",
    "directory_mutation",
    "index_write",
    "index_sync",
    "catalog_trigger_effect",
    "mutation_manifest_publication",
)
COMMITTED_BOUNDARIES = (
    "transaction_inventory_publication",
    "final_sync",
)
REGISTERED_BOUNDARIES = EARLY_BOUNDARIES + COMMITTED_BOUNDARIES + (
    "recovery_cleanup",
)


class WholeStoreRecoveryError(RuntimeError):
    pass


def clone_database(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    matched = False
    for artifact in source.parent.glob(source.name + "*"):
        matched = True
        suffix = artifact.name[len(source.name) :]
        destination = target.parent / (target.name + suffix)
        if artifact.is_dir():
            shutil.copytree(artifact, destination)
        else:
            shutil.copy2(artifact, destination)
    if not matched or not target.is_file():
        raise WholeStoreRecoveryError(f"database clone failed: {source} -> {target}")


def setup_template(args: argparse.Namespace, work: Path) -> Path:
    root = work / "template"
    database = root / "whole_store.sbdb"
    route: StartedRoute | None = None
    try:
        route = start_route(args, root / "route", database, tls_required=False)
        setup = run_isql(
            args,
            route,
            "setup",
            "\n".join(
                [
                    f"INSERT INTO {TABLE} (id, payload) VALUES (1, 'seed-1');",
                    f"INSERT INTO {TABLE} (id, payload) VALUES (2, 'seed-2');",
                    "COMMIT;",
                    "",
                ]
            ),
        )
        require_isql_success(setup)
    finally:
        stop_route(route)
    return database


def crash_environment(point: str, marker: Path) -> dict[str, str]:
    return {
        "SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_ARM": CRASH_ARM,
        "SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_POINT": point,
        "SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_MARKER": str(marker),
        "SCRATCHBIRD_TEST_WHOLE_STORE_CRASH_TRIGGER_VALUE": "crash-row",
    }


def wait_for_crash(server: subprocess.Popen[bytes], marker: Path, point: str) -> int:
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        if marker.is_file() and server.poll() is not None:
            break
        time.sleep(0.025)
    if not marker.is_file():
        raise WholeStoreRecoveryError(f"{point}: engine boundary marker was not published")
    marker_text = marker.read_text(encoding="utf-8", errors="replace")
    required = (
        f"boundary={point}",
        "authority=durable_mga_transaction_inventory",
        "parser_finality=false",
        "wal_authority=false",
    )
    missing = [token for token in required if token not in marker_text]
    if missing:
        raise WholeStoreRecoveryError(f"{point}: crash marker missing {missing}")
    transaction_lines = [
        line for line in marker_text.splitlines() if line.startswith("local_transaction_id=")
    ]
    try:
        local_transaction_id = int(transaction_lines[0].split("=", 1)[1])
    except (IndexError, ValueError) as exc:
        raise WholeStoreRecoveryError(
            f"{point}: crash marker has no valid local transaction identity"
        ) from exc
    if point != "recovery_cleanup" and local_transaction_id <= 0:
        raise WholeStoreRecoveryError(
            f"{point}: crash was not bound to the intended DML transaction"
        )
    returncode = server.poll()
    if returncode is None:
        raise WholeStoreRecoveryError(f"{point}: server remained alive after marker")
    if returncode not in (-9, 137):
        raise WholeStoreRecoveryError(
            f"{point}: expected SIGKILL server termination, got rc={returncode}"
        )
    return local_transaction_id


def run_mutation_until_crash(
    args: argparse.Namespace,
    route: StartedRoute,
    point: str,
    marker: Path,
) -> int:
    client_root = route.root / "isql" / "crash_mutation"
    client_root.mkdir(parents=True, exist_ok=True)
    script = client_root / "script.sbsql"
    script.write_text(
        f"INSERT INTO {TABLE} (id, payload) VALUES ({MUTATION_ID}, 'crash-row');\n"
        "COMMIT;\n",
        encoding="utf-8",
    )
    with (client_root / "stdout.log").open("wb") as stdout, (
        client_root / "stderr.log"
    ).open("wb") as stderr:
        client = subprocess.Popen(
            isql_args(args, route) + ["-q", "-A", "-t", "-b", "-f", str(script)],
            stdout=stdout,
            stderr=stderr,
        )
        try:
            local_transaction_id = wait_for_crash(route.server, marker, point)
        finally:
            stop_process(client)
    return local_transaction_id


def require_exact_rows(result_name: str, actual: list[str], expected: list[str]) -> None:
    if actual != expected:
        raise WholeStoreRecoveryError(
            f"{result_name}: expected rows {expected!r}, got {actual!r}"
        )


def select_data_lines(result: subprocess.CompletedProcess[str]) -> list[str]:
    """Normalize the current SBsql rendering for an empty SELECT result."""
    rows = isql_data_lines(result)
    return [] if rows == ["Rows affected: 0"] else rows


def require_last_unique_probe_used_index(route: StartedRoute) -> None:
    trace = route.traces["dispatch"]
    if not trace.is_file():
        raise WholeStoreRecoveryError("unique probe: server runtime trace is absent")
    lines = [
        line
        for line in trace.read_text(encoding="utf-8").splitlines()
        if "layer=statement_context_dispatch_failure" in line
        and "code=CLI.CONSTRAINT_UNIQUE_VIOLATION" in line
    ]
    last = lines[-1] if lines else ""
    required = (
        "bulk_unique_proof_persisted_conflict",
        "key=SBKOHEX:",
    )
    missing = [token for token in required if token not in last]
    if missing:
        raise WholeStoreRecoveryError(
            "unique probe: ordinary SBSql duplicate check did not require the "
            f"persisted index path ({missing}): {last if last else '<empty trace>'}"
        )


def verify_reopened_state(
    args: argparse.Namespace,
    route: StartedRoute,
    *,
    mutation_committed: bool,
    attempted_transaction_id: int | None = None,
) -> None:
    expected_count = "4" if mutation_committed else "3"
    expected_ids = (
        ["1", "2", "6", "100"] if mutation_committed else ["1", "2", "6"]
    )

    count = run_isql(args, route, "verify_scan_count", f"SELECT COUNT(*) FROM {TABLE};\n")
    require_exact_rows("scan count", isql_data_lines(count), [expected_count])

    scan = run_isql(args, route, "verify_scan_rows", f"SELECT id FROM {TABLE} ORDER BY id ASC;\n")
    require_exact_rows("ordered scan", isql_data_lines(scan), expected_ids)

    baseline_point = run_isql(
        args,
        route,
        "verify_index_point_baseline",
        f"SELECT payload FROM {TABLE} WHERE id = 1;\n",
    )
    require_exact_rows(
        "baseline indexed point lookup", isql_data_lines(baseline_point), ["seed-1"]
    )

    crash_point = run_isql(
        args,
        route,
        "verify_index_point_crash_row",
        f"SELECT payload FROM {TABLE} WHERE id = {MUTATION_ID};\n",
    )
    require_exact_rows(
        "crash-row indexed point lookup",
        select_data_lines(crash_point),
        ["crash-row"] if mutation_committed else [],
    )

    duplicate = run_isql(
        args,
        route,
        "verify_unique_constraint",
        f"INSERT INTO {TABLE} (id, payload) VALUES (1, 'duplicate');\nCOMMIT;\n",
    )
    if duplicate.returncode == 0 and not duplicate.stderr:
        raise WholeStoreRecoveryError("unique constraint admitted duplicate key id=1")
    require_last_unique_probe_used_index(route)

    count_after_duplicate = run_isql(
        args,
        route,
        "verify_no_duplicate_ghost",
        f"SELECT COUNT(*) FROM {TABLE};\n",
    )
    require_exact_rows(
        "post-constraint row count",
        isql_data_lines(count_after_duplicate),
        [expected_count],
    )

    transactions = run_isql(args, route, "verify_transactions", "SHOW TRANSACTIONS;\n")
    require_isql_success(transactions)
    transaction_text = transactions.stdout.lower()
    expected_transaction_state = "committed" if mutation_committed else "rolled_back"
    if attempted_transaction_id is not None and (
        f"{attempted_transaction_id}|{expected_transaction_state}"
        not in transaction_text.splitlines()
    ):
        raise WholeStoreRecoveryError(
            "transaction inventory omitted exact crash-transaction classification: "
            f"{attempted_transaction_id}|{expected_transaction_state}"
        )
    if attempted_transaction_id is None and "committed" not in transaction_text:
        raise WholeStoreRecoveryError("transaction inventory omitted committed classification")


def run_no_fault_baseline(args: argparse.Namespace, work: Path, template: Path) -> None:
    root = work / "baseline"
    database = root / "whole_store.sbdb"
    clone_database(template, database)
    route: StartedRoute | None = None
    attempted_transaction_id: int | None = None
    try:
        route = start_route(args, root / "mutate", database, tls_required=False)
        mutation = run_isql(
            args,
            route,
            "no_fault_mutation",
            f"INSERT INTO {TABLE} (id, payload) VALUES ({MUTATION_ID}, 'crash-row');\nCOMMIT;\n",
        )
        require_isql_success(mutation)
        stop_route(route)
        route = start_route(args, root / "restart", database, tls_required=False)
        verify_reopened_state(args, route, mutation_committed=True)
    finally:
        stop_route(route)


def run_boundary_case(
    args: argparse.Namespace,
    work: Path,
    template: Path,
    point: str,
    *,
    mutation_committed: bool,
) -> None:
    root = work / point
    database = root / "whole_store.sbdb"
    clone_database(template, database)
    marker = root / "crash.marker"
    route: StartedRoute | None = None
    try:
        route = start_route(
            args,
            root / "crash",
            database,
            tls_required=False,
            extra_env=crash_environment(point, marker),
        )
        attempted_transaction_id = run_mutation_until_crash(args, route, point, marker)
    finally:
        stop_route(route)

    route = None
    try:
        route = start_route(args, root / "restart", database, tls_required=False)
        verify_reopened_state(
            args,
            route,
            mutation_committed=mutation_committed,
            attempted_transaction_id=attempted_transaction_id,
        )
    finally:
        stop_route(route)


def run_recovery_cleanup_case(
    args: argparse.Namespace,
    work: Path,
    template: Path,
) -> None:
    root = work / "recovery_cleanup"
    database = root / "whole_store.sbdb"
    clone_database(template, database)

    stage_marker = root / "stage_crash.marker"
    route: StartedRoute | None = None
    attempted_transaction_id: int | None = None
    try:
        route = start_route(
            args,
            root / "stage_crash",
            database,
            tls_required=False,
            extra_env=crash_environment("partial_page_write", stage_marker),
        )
        attempted_transaction_id = run_mutation_until_crash(
            args, route, "partial_page_write", stage_marker
        )
    finally:
        stop_route(route)

    recovery_root = root / "recovery_crash"
    server_control = recovery_root / "sc"
    server_runtime = recovery_root / "sr"
    marker = root / "crash.marker"
    recovery_root.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(crash_environment("recovery_cleanup", marker))
    with (recovery_root / "server.out").open("wb") as stdout, (
        recovery_root / "server.err"
    ).open("wb") as stderr:
        server = subprocess.Popen(
            [
                args.server,
                "--foreground",
                "--no-listeners",
                "--control-dir",
                str(server_control),
                "--runtime-dir",
                str(server_runtime),
                "--database",
                str(database),
                "--sbps-endpoint",
                str(server_control / "s.sock"),
            ],
            stdout=stdout,
            stderr=stderr,
            env=env,
        )
        try:
            wait_for_crash(server, marker, "recovery_cleanup")
        finally:
            stop_process(server)

    route = None
    try:
        route = start_route(args, root / "restart", database, tls_required=False)
        verify_reopened_state(
            args,
            route,
            mutation_committed=False,
            attempted_transaction_id=attempted_transaction_id,
        )
    finally:
        stop_route(route)


def dump_logs(work: Path) -> None:
    paths = (
        sorted(work.rglob("*.out"))
        + sorted(work.rglob("*.err"))
        + sorted(work.rglob("*.log"))
        + sorted(work.rglob("*.marker"))
    )
    for path in paths:
        if not path.is_file() or path.stat().st_size == 0:
            continue
        print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
        text = path.read_text(encoding="utf-8", errors="replace")
        print(text[-8000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--example-db-seeder", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    work = Path(tempfile.mkdtemp(prefix="sbwsr_"))
    work_pointer = Path(args.work_dir) / "latest_work_path.txt"
    work_pointer.parent.mkdir(parents=True, exist_ok=True)
    work_pointer.write_text(str(work) + "\n", encoding="utf-8")
    matrix_path = work / "whole_store_recovery_matrix.tsv"
    rows: list[tuple[str, str, str]] = []
    failures: list[str] = []
    try:
        template = setup_template(args, work)
        run_no_fault_baseline(args, work, template)
        rows.append(("no_fault_baseline", "committed", "passed"))

        for point in EARLY_BOUNDARIES:
            try:
                run_boundary_case(
                    args, work, template, point, mutation_committed=False
                )
                rows.append((point, "rolled_back_by_recovery", "passed"))
            except Exception as exc:  # noqa: BLE001 - collect the full matrix.
                rows.append((point, "rolled_back_by_recovery", "failed"))
                failures.append(f"{point}: {exc}")

        for point in COMMITTED_BOUNDARIES:
            try:
                run_boundary_case(
                    args, work, template, point, mutation_committed=True
                )
                rows.append((point, "committed_by_inventory", "passed"))
            except Exception as exc:  # noqa: BLE001 - collect the full matrix.
                rows.append((point, "committed_by_inventory", "failed"))
                failures.append(f"{point}: {exc}")

        try:
            run_recovery_cleanup_case(args, work, template)
            rows.append(("recovery_cleanup", "rolled_back_by_recovery", "passed"))
        except Exception as exc:  # noqa: BLE001 - collect the full matrix.
            rows.append(("recovery_cleanup", "rolled_back_by_recovery", "failed"))
            failures.append(f"recovery_cleanup: {exc}")
    except Exception as exc:  # noqa: BLE001 - retain baseline diagnostics.
        failures.append(f"baseline: {exc}")
    finally:
        matrix_path.parent.mkdir(parents=True, exist_ok=True)
        matrix_path.write_text(
            "boundary\texpected_inventory_classification\tresult\n"
            + "".join("\t".join(row) + "\n" for row in rows),
            encoding="utf-8",
        )

    covered = {row[0] for row in rows if row[2] == "passed"}
    missing = set(REGISTERED_BOUNDARIES) - covered
    if missing:
        failures.append(f"registered boundaries without passing case: {sorted(missing)}")
    if failures:
        print(
            f"sbsql_whole_store_recovery_full_route_gate=failed work={work}",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        dump_logs(work)
        return 1

    print(f"sbsql_whole_store_recovery_full_route_gate=passed work={work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
