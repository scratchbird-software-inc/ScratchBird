#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CDP-041 sb_isql native bulk ingest route gate."""

from __future__ import annotations

import argparse
import os
import hashlib
import re
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "support"))
from owned_test_runtime import CleanupRefused, OwnedTestRuntime

from cdp_database_lifecycle_support import PUBLIC_TEST_PASSWORD, seed_database


TARGET = "cdp_native_bulk_ingest_cli"
EXPECTED_OPERATION = "dml.execute_native_bulk_ingest"
EXPECTED_DISABLED = "DML.NATIVE_BULK_INGEST.DISABLED"
ROUND_TRIP_ROW_COUNT = 1600


class NativeBulkIngestGateError(RuntimeError):
    pass


class RouteShutdownError(NativeBulkIngestGateError):
    pass


@dataclass
class Route:
    name: str
    database: Path
    args: list[str]


@dataclass
class RunResult:
    route: str
    case: str
    returncode: int
    stdout: str
    stderr: str

    @property
    def diagnostic(self) -> str:
        text = "\n".join(part for part in (self.stderr, self.stdout) if part)
        match = re.search(r"(DML\.NATIVE_BULK_INGEST\.DISABLED)", text)
        if match:
            return match.group(1)
        match = re.search(r"Error:\s*(.*)", text)
        return match.group(1).strip() if match else text.strip()


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise NativeBulkIngestGateError(f"timed out waiting for {path}")


def wait_for_tcp(port: int, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise NativeBulkIngestGateError(f"timed out waiting for listener port {port}: {last_error}")


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=4)


def run_sb_isql(route: Route, case: str, script_text: str, work: Path, timeout: int = 25) -> RunResult:
    case_dir = work / route.name / case
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    script.write_text(script_text, encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    with out_path.open("wb") as stdout, err_path.open("wb") as stderr:
        completed = subprocess.run(
            route.args + ["-q", "-A", "-t", "-b", "-f", str(script)],
            stdout=stdout,
            stderr=stderr,
            check=False,
            timeout=timeout,
        )
    return RunResult(
        route=route.name,
        case=case,
        returncode=completed.returncode,
        stdout=out_path.read_text(encoding="utf-8", errors="replace").strip(),
        stderr=err_path.read_text(encoding="utf-8", errors="replace").strip(),
    )


def expected_native_rows() -> list[tuple[int, int, int, int | None]]:
    rows: list[tuple[int, int, int, int | None]] = []
    for row_id in range(1, ROUND_TRIP_ROW_COUNT + 1):
        customer_id = 1000 + (row_id * 973) % 8000
        discount_amount = 1000 + (row_id * 428) % 8000
        if row_id == 2:
            customer_id = 1973
            discount_amount = 2428
        nullable_value = None if row_id % 101 == 0 else 1000 + (row_id * 313) % 8000
        rows.append((row_id, customer_id, discount_amount, nullable_value))
    return rows


def write_native_rows(work: Path) -> Path:
    path = work / "native.rows"
    path.write_text(
        "".join(
            "id={};customer_id={};discount_amount={};nullable_value={}\n".format(
                row_id,
                customer_id,
                discount_amount,
                "NULL" if nullable_value is None else nullable_value,
            )
            for row_id, customer_id, discount_amount, nullable_value in expected_native_rows()
        ),
        encoding="utf-8",
    )
    return path


def create_target_script() -> str:
    return (
        f"CREATE TABLE {TARGET} ("
        "id int, customer_id int, discount_amount int, nullable_value int);\n"
    )


def native_script(rows: Path, disabled: bool = False) -> str:
    suffix = " DISABLED" if disabled else ""
    return f"\\native_bulk_ingest {TARGET} FROM '{rows}'{suffix}\n"


def round_trip_script() -> str:
    return (
        f"SELECT id, customer_id, discount_amount, nullable_value FROM {TARGET} "
        "ORDER BY id ASC;\n"
    )


def verify_destination_carriers(route: Route, work: Path) -> None:
    """Source packet widths must not become retained destination row widths."""
    target = "cdp_native_bulk_destination_carriers"
    rows = work / "destination-carriers.rows"
    rows.write_text(
        "id=1;amount=7\n"
        "id=1;amount=-3\n"
        "id=2;amount=9223372036854775807\n"
        "id=2;amount=-9223372036854775808\n"
        "id=3;amount=NULL\n"
        "id=NULL;amount=19\n",
        encoding="utf-8",
    )
    setup = run_sb_isql(
        route, "destination_carriers_setup",
        f"CREATE TABLE {target} (id BIGINT, amount BIGINT);\n"
        f"\\native_bulk_ingest {target} FROM '{rows}'\n", work, timeout=120,
    )
    verify_accepted([setup])
    if "rows_affected=6" not in setup.stdout:
        raise NativeBulkIngestGateError(f"{route.name} destination row count: {setup.stdout!r}")
    # A separate client reloads committed rows. Embedded mode also reopens the
    # node; IPC/INET exercise parser, canonical transport and live row readers.
    queries = (
        ("top_k", f"SELECT * FROM {target} WHERE id = 2 ORDER BY amount DESC LIMIT 1;\n",
         ["2|9223372036854775807"]),
        ("grouped_sum", f"SELECT id,SUM(amount) FROM {target} GROUP BY id;\n",
         ["(null)|19", "1|4", "2|-1", "3|(null)"]),
        ("join", f"SELECT * FROM {target} AS l INNER JOIN {target} AS r ON l.id = r.id;\n",
         ["1|7|1|7", "1|-3|1|7", "1|7|1|-3", "1|-3|1|-3",
          "2|9223372036854775807|2|9223372036854775807",
          "2|-9223372036854775808|2|9223372036854775807",
          "2|9223372036854775807|2|-9223372036854775808",
          "2|-9223372036854775808|2|-9223372036854775808", "3|(null)|3|(null)"]),
    )
    for name, query, expected in queries:
        result = run_sb_isql(route, "destination_carriers_" + name, query, work, timeout=120)
        # These queries do not prescribe inter-group/join order. Compare the
        # exact multiset (including duplicates and SQL NULL), not just a count.
        if (result.returncode != 0 or result.stderr or
                sorted(result.stdout.splitlines()) != sorted(expected)):
            raise NativeBulkIngestGateError(
                f"{route.name} destination {name} mismatch: rc={result.returncode} "
                f"stdout={result.stdout!r} stderr={result.stderr!r} expected={expected!r}"
            )


def run_embedded(args: argparse.Namespace, work: Path) -> Route:
    database = work / "embedded" / "e.sbdb"
    seed_database(
        database_seed=args.database_seed,
        resource_seed_pack_root=args.resource_seed_pack_root,
        database=database,
        evidence_root=work / "embedded" / "bootstrap",
        fixture_label="embedded",
    )
    return Route(
        name="embedded",
        database=database,
        args=[
            args.sb_isql,
            str(database),
            "--mode=embedded",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            PUBLIC_TEST_PASSWORD,
        ],
    )


def start_local_ipc(args: argparse.Namespace, work: Path,
                    processes: list[subprocess.Popen[bytes]]) -> Route:
    root = work / "ipc"
    database = root / "l.sbdb"
    control = root / "sc"
    runtime = root / "sr"
    endpoint = control / "s.sock"
    root.mkdir(parents=True, exist_ok=True)
    seed_database(
        database_seed=args.database_seed,
        resource_seed_pack_root=args.resource_seed_pack_root,
        database=database,
        evidence_root=root / "bootstrap",
        fixture_label="local-ipc",
    )
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--control-dir",
            str(control),
            "--runtime-dir",
            str(runtime),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    # Retain ownership before readiness can fail, not only after this function
    # returns. The outer finally must join partially started routes as well.
    processes.append(server)
    wait_for_path(endpoint)
    return Route(
            name="local-ipc",
            database=database,
            args=[
                args.sb_isql,
                str(database),
                "--mode=local-ipc",
                "--ipc-method=unix",
                f"--ipc-path={endpoint}",
                "--sslmode=disable",
                "-U",
                "alice",
                "-P",
                PUBLIC_TEST_PASSWORD,
            ],
    )


def start_inet(args: argparse.Namespace, work: Path,
               processes: list[subprocess.Popen[bytes]]) -> Route:
    root = work / "inet"
    database = root / "i.sbdb"
    server_control = root / "sc"
    server_runtime = root / "sr"
    listener_control = root / "lc"
    listener_runtime = root / "lr"
    endpoint = server_control / "s.sock"
    port = find_free_port()
    root.mkdir(parents=True, exist_ok=True)
    seed_database(
        database_seed=args.database_seed,
        resource_seed_pack_root=args.resource_seed_pack_root,
        database=database,
        evidence_root=root / "bootstrap",
        fixture_label="inet",
    )
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
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    processes.append(server)
    wait_for_path(endpoint)
    listener = subprocess.Popen(
        [
            args.listener,
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            f"--database-selector=dev_bootstrap_path:{database}",
            f"--server-endpoint=unix:{endpoint}",
            f"--parser-executable={args.parser_worker}",
            f"--control-dir={listener_control}",
            f"--runtime-dir={listener_runtime}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
        ],
        stdout=(root / "listener.out").open("wb"),
        stderr=(root / "listener.err").open("wb"),
    )
    processes.append(listener)
    wait_for_tcp(port)
    return Route(
            name="inet",
            database=database,
            args=[
                args.sb_isql,
                str(database),
                "--host=127.0.0.1",
                f"--port={port}",
                "--sslmode=disable",
                "-U",
                "alice",
                "-P",
                PUBLIC_TEST_PASSWORD,
            ],
    )


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def verify_accepted(results: list[RunResult]) -> None:
    for result in results:
        if result.returncode != 0 or result.stderr:
            raise NativeBulkIngestGateError(
                f"{result.route} native ingest not accepted: "
                f"rc={result.returncode} stdout={result.stdout!r} stderr={result.stderr!r}"
            )
        if EXPECTED_OPERATION not in result.stdout or "accepted=true" not in result.stdout:
            raise NativeBulkIngestGateError(
                f"{result.route} native ingest envelope missing operation evidence: {result.stdout!r}"
            )
    envelopes = {result.stdout for result in results}
    if len(envelopes) != 1:
        raise NativeBulkIngestGateError(
            "accepted native ingest envelopes differ: "
            + " | ".join(f"{result.route}={result.stdout!r}" for result in results)
        )


def verify_disabled(results: list[RunResult]) -> None:
    for result in results:
        if result.returncode == 0:
            raise NativeBulkIngestGateError(
                f"{result.route} disabled native ingest was accepted: stdout={result.stdout!r}"
            )
    diagnostics = {result.diagnostic for result in results}
    if diagnostics != {EXPECTED_DISABLED}:
        raise NativeBulkIngestGateError(
            "disabled native ingest diagnostics differ: "
            + " | ".join(f"{result.route}={result.diagnostic!r}" for result in results)
        )


def verify_round_trip(results: list[RunResult]) -> None:
    expected_lines = [
        "{}|{}|{}|{}".format(
            row_id,
            customer_id,
            discount_amount,
            # Match the CLI's SQL-NULL rendering, not an empty value. Keep the
            # exact row digest/order check; do not normalize either spelling.
            "(null)" if nullable_value is None else nullable_value,
        )
        for row_id, customer_id, discount_amount, nullable_value in expected_native_rows()
    ]
    expected_text = "\n".join(expected_lines)
    expected_hash = hashlib.sha256(expected_text.encode("utf-8")).hexdigest()
    for result in results:
        if result.returncode != 0 or result.stderr:
            raise NativeBulkIngestGateError(
                f"{result.route} native ingest round-trip query failed: "
                f"rc={result.returncode} stdout={result.stdout!r} stderr={result.stderr!r}"
            )
        actual_hash = hashlib.sha256(result.stdout.encode("utf-8")).hexdigest()
        if actual_hash != expected_hash or result.stdout != expected_text:
            actual_lines = result.stdout.splitlines()
            mismatch = next(
                (
                    index
                    for index, (expected, actual) in enumerate(
                        zip(expected_lines, actual_lines, strict=False), start=1
                    )
                    if expected != actual
                ),
                min(len(expected_lines), len(actual_lines)) + 1,
            )
            raise NativeBulkIngestGateError(
                f"{result.route} native ingest row hash/order mismatch at row {mismatch}: "
                f"expected_sha256={expected_hash} actual_sha256={actual_hash} "
                f"expected_rows={len(expected_lines)} actual_rows={len(actual_lines)}"
            )


def run_gate(args: argparse.Namespace, work: Path) -> None:
    rows = write_native_rows(work)
    routes: list[Route] = []
    processes: list[subprocess.Popen[bytes]] = []
    try:
        routes.append(run_embedded(args, work))
        routes.append(start_local_ipc(args, work, processes))
        routes.append(start_inet(args, work, processes))

        ddl_results = [run_sb_isql(route, "create_target", create_target_script(), work) for route in routes]
        for result in ddl_results:
            if result.returncode != 0 or result.stderr:
                raise NativeBulkIngestGateError(
                    f"{result.route} target DDL failed: rc={result.returncode} "
                    f"stdout={result.stdout!r} stderr={result.stderr!r}"
                )

        accepted = [
            run_sb_isql(route, "native_ingest", native_script(rows), work, timeout=120)
            for route in routes
        ]
        verify_accepted(accepted)

        round_trip = [
            run_sb_isql(route, "native_ingest_round_trip", round_trip_script(), work, timeout=120)
            for route in routes
        ]
        verify_round_trip(round_trip)

        disabled = [
            run_sb_isql(route, "native_ingest_disabled", native_script(rows, disabled=True), work)
            for route in routes
        ]
        verify_disabled(disabled)

        for route in routes:
            verify_destination_carriers(route, work)
    finally:
        errors = []
        for proc in reversed(processes):
            try:
                stop_process(proc)
            except Exception as exc:
                errors.append(str(exc))
        if errors:
            raise RouteShutdownError("route shutdown unresolved: " + "; ".join(errors))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--database-seed", required=True)
    parser.add_argument("--resource-seed-pack-root", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    # A short private root keeps Unix-domain endpoints within their limit.
    # The existing owned-runtime utility retains bounded diagnostics and
    # refuses cleanup if any descendant still references this workspace.
    runtime = OwnedTestRuntime(parent="/tmp" if os.name == "posix" else None)
    work = runtime.path
    status = 0
    shutdown_confirmed = True
    try:
        run_gate(args, work)
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        status = 1
        shutdown_confirmed = not isinstance(exc, RouteShutdownError)
        print(f"cdp_native_bulk_ingest_cli_gate=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
    finally:
        if shutdown_confirmed:
            try:
                runtime.cleanup(Path(args.work_dir) / work.name, controller_finished=True)
            except (CleanupRefused, OSError) as exc:
                status = 1
                print(f"cdp_native_bulk_ingest_cli_gate=cleanup_failed work={work}: {exc}", file=sys.stderr)
    if status == 0:
        print(f"cdp_native_bulk_ingest_cli_gate=passed work={work}")
    return status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
