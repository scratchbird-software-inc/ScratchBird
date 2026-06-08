#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Standalone CDP route split benchmark fixture gate.

This CTest gate is benchmark evidence plumbing, not a speed claim. It runs the
same deterministic DML workload through embedded, local IPC, and INET/listener
sb_isql routes, compares per-lane semantic hashes, and writes the baseline,
corpus, environment, and evidence-retention JSON package required by the first
CDP fixture closure.
"""

from __future__ import annotations

import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

import cdp_benchmark_fixture_support as support


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
class RouteBenchmarkError(RuntimeError):
    pass


@dataclass(frozen=True)
class LaneSpec:
    lane_id: str
    fixture_name: str
    statement_id: str
    operation: str
    rows_input: int
    rows_affected: int
    rows_returned: int
    semantic_lines: tuple[str, ...]
    script_builder: Callable[[str, dict[str, Path]], str]
    expected_refusal: str | None = None


@dataclass
class RunResult:
    route: str
    lane_id: str
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float
    stdout_path: Path
    stderr_path: Path
    database: Path


@dataclass
class RouteContext:
    name: str
    root: Path
    database: Path
    sb_isql: str
    fixed_args: list[str] = field(default_factory=list)
    embedded: bool = False
    processes: list[subprocess.Popen[bytes]] = field(default_factory=list)

    def database_for_lane(self, lane_id: str) -> Path:
        if self.embedded:
            return self.root / f"{lane_id}.sbdb"
        return self.database

    def cli_args(self, lane_id: str) -> list[str]:
        if not self.embedded:
            return list(self.fixed_args)
        database = self.database_for_lane(lane_id)
        database.parent.mkdir(parents=True, exist_ok=True)
        return [
            self.sb_isql,
            str(database),
            "--mode=embedded",
            "--sslmode=disable",
        ]


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "cdp_route_split")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="cdp_", dir=root))
        endpoint_probe = candidate / "i" / "sc" / "s.sock"
        listener_probe = candidate / "n" / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(endpoint_probe)), len(str(listener_probe)), len(str(candidate / "e.sbdb"))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise RouteBenchmarkError("unable to allocate a short-enough route split workspace")


def wait_for_path(path: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise RouteBenchmarkError(f"timed out waiting for {path}")


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
    raise RouteBenchmarkError(f"timed out waiting for listener port {port}: {last_error}")


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=4)


def stop_route(route: RouteContext) -> None:
    for proc in reversed(route.processes):
        stop_process(proc)


def auth_file(database: Path) -> None:
    Path(str(database) + ".sb.local_password_auth").write_text(
        f"alice\tlocal_password\t{VERIFIER}\n", encoding="utf-8")


def quote_sql_path(path: Path) -> str:
    text = str(path)
    if "'" in text:
        raise RouteBenchmarkError(f"fixture path cannot be single-quoted in SQL: {path}")
    return f"'{text}'"


def table_name(prefix: str, suffix: str) -> str:
    return f"{prefix}_{suffix}".replace("-", "_")


def write_rows(path: Path, values: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"id={value}\n" for value in values), encoding="utf-8")


def route_inputs(route: RouteContext) -> dict[str, Path]:
    inputs = route.root / "inputs"
    paths = {
        "main": inputs / "main.rows",
        "left": inputs / "left.rows",
        "right": inputs / "right.rows",
    }
    write_rows(paths["main"], [1, 2, 3, 4, 5])
    write_rows(paths["left"], [1, 2, 3, 4])
    write_rows(paths["right"], [2, 3, 5])
    return paths


def setup_copy_sql(table: str, rows_path: Path) -> str:
    return "\n".join([
        f"CREATE TABLE {table} (id int);",
        f"\\copy {table} FROM {quote_sql_path(rows_path)}",
    ])


def route_split_script(prefix: str, inputs: dict[str, Path]) -> str:
    del prefix, inputs
    return "SELECT 1;\n"


def copy_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "copy")
    return "\n".join([
        setup_copy_sql(table, inputs["main"]),
        f"SELECT COUNT(*) FROM {table};",
        "",
    ])


def point_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "point")
    return "\n".join([
        setup_copy_sql(table, inputs["main"]),
        f"SELECT id FROM {table} WHERE id = 3;",
        "",
    ])


def range_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "range")
    return "\n".join([
        setup_copy_sql(table, inputs["main"]),
        f"SELECT id FROM {table} ORDER BY id ASC LIMIT 3 OFFSET 1;",
        "",
    ])


def aggregate_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "agg")
    return "\n".join([
        setup_copy_sql(table, inputs["main"]),
        f"SELECT COUNT(*) FROM {table};",
        "",
    ])


def update_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "upd")
    return "\n".join([
        setup_copy_sql(table, inputs["main"]),
        f"UPDATE {table} SET id = 20 WHERE id = 2;",
        f"SELECT id FROM {table} ORDER BY id ASC;",
        "",
    ])


def join_script(prefix: str, inputs: dict[str, Path]) -> str:
    left = table_name(prefix, "join_l")
    right = table_name(prefix, "join_r")
    return "\n".join([
        setup_copy_sql(left, inputs["left"]),
        setup_copy_sql(right, inputs["right"]),
        f"SELECT * FROM {left} JOIN {right} ON {left}.id = {right}.id;",
        "",
    ])


LANES: tuple[LaneSpec, ...] = (
    LaneSpec(
        lane_id="route_split",
        fixture_name="cdp_route_select_one",
        statement_id="cdp.route.select_one.v1",
        operation="select",
        rows_input=0,
        rows_affected=0,
        rows_returned=1,
        semantic_lines=("1",),
        script_builder=route_split_script,
    ),
    LaneSpec(
        lane_id="copy_10k",
        fixture_name="cdp_copy_narrow_rows",
        statement_id="cdp.copy.narrow.ctest.v1",
        operation="copy",
        rows_input=5,
        rows_affected=5,
        rows_returned=1,
        semantic_lines=("5",),
        script_builder=copy_script,
    ),
    LaneSpec(
        lane_id="point_select",
        fixture_name="cdp_select_point_and_range",
        statement_id="cdp.select.point.ctest.v1",
        operation="select",
        rows_input=5,
        rows_affected=0,
        rows_returned=1,
        semantic_lines=("3",),
        script_builder=point_script,
    ),
    LaneSpec(
        lane_id="range_select",
        fixture_name="cdp_select_point_and_range",
        statement_id="cdp.select.range.ctest.v1",
        operation="select",
        rows_input=5,
        rows_affected=0,
        rows_returned=3,
        semantic_lines=("2", "3", "4"),
        script_builder=range_script,
    ),
    LaneSpec(
        lane_id="aggregate_count",
        fixture_name="cdp_aggregate_count_rows",
        statement_id="cdp.aggregate.count.ctest.v1",
        operation="aggregate",
        rows_input=5,
        rows_affected=0,
        rows_returned=1,
        semantic_lines=("5",),
        script_builder=aggregate_script,
    ),
    LaneSpec(
        lane_id="single_row_dml",
        fixture_name="cdp_update_hot_rows",
        statement_id="cdp.update.single_row.ctest.v1",
        operation="update",
        rows_input=5,
        rows_affected=1,
        rows_returned=6,
        semantic_lines=("20", "1", "3", "4", "5", "20"),
        script_builder=update_script,
    ),
    LaneSpec(
        lane_id="execution_plan10_joins",
        fixture_name="cdp_join_customer_order_items",
        statement_id="cdp.join.execution_plan10_shape.ctest.v1",
        operation="join",
        rows_input=7,
        rows_affected=0,
        rows_returned=2,
        semantic_lines=("2|2", "3|3"),
        script_builder=join_script,
    ),
)


def start_embedded(args: argparse.Namespace, work: Path) -> RouteContext:
    root = work / "e"
    root.mkdir(parents=True, exist_ok=True)
    return RouteContext(
        name="embedded",
        root=root,
        database=root / "route_split.sbdb",
        sb_isql=args.sb_isql,
        embedded=True,
    )


def start_local_ipc(args: argparse.Namespace, work: Path) -> RouteContext:
    root = work / "i"
    database = root / "ipc.sbdb"
    control = root / "sc"
    runtime = root / "sr"
    endpoint = control / "s.sock"
    root.mkdir(parents=True, exist_ok=True)
    auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
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
    wait_for_path(endpoint)
    evidence = f"scheme=local_password_v1;principal=alice;verifier={VERIFIER}"
    return RouteContext(
        name="ipc",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        fixed_args=[
            args.sb_isql,
            str(database),
            "--mode=local-ipc",
            "--ipc-method=unix",
            f"--ipc-path={endpoint}",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            evidence,
        ],
        processes=[server],
    )


def start_inet(args: argparse.Namespace, work: Path) -> RouteContext:
    root = work / "n"
    database = root / "inet.sbdb"
    server_control = root / "sc"
    server_runtime = root / "sr"
    listener_control = root / "lc"
    listener_runtime = root / "lr"
    endpoint = server_control / "s.sock"
    port = find_free_port()
    root.mkdir(parents=True, exist_ok=True)
    auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
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
    wait_for_tcp(port)
    evidence = f"scheme=local_password_v1;principal=alice;verifier={VERIFIER}"
    return RouteContext(
        name="inet",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        fixed_args=[
            args.sb_isql,
            str(database),
            "--host=127.0.0.1",
            f"--port={port}",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            evidence,
        ],
        processes=[server, listener],
    )


def run_sb_isql(route: RouteContext, lane: LaneSpec, script_text: str) -> RunResult:
    case_dir = route.root / "cases" / lane.lane_id
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    script.write_text(script_text, encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    started = time.monotonic()
    completed = subprocess.run(
        route.cli_args(lane.lane_id) + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=35,
    )
    elapsed_ms = (time.monotonic() - started) * 1000.0
    return RunResult(
        route=route.name,
        lane_id=lane.lane_id,
        returncode=completed.returncode,
        stdout=out_path.read_text(encoding="utf-8", errors="replace").strip(),
        stderr=err_path.read_text(encoding="utf-8", errors="replace").strip(),
        elapsed_ms=elapsed_ms,
        stdout_path=out_path,
        stderr_path=err_path,
        database=route.database_for_lane(lane.lane_id),
    )


def semantic_lines(stdout: str) -> list[str]:
    lines: list[str] = []
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("Rows affected:"):
            continue
        if line.startswith("COPY ") and " rows from " in line:
            continue
        if line.startswith("Stopping due to error"):
            continue
        lines.append(line)
    return lines


def diagnostic_code(result: RunResult) -> str:
    text = "\n".join(part for part in (result.stderr, result.stdout) if part)
    match = re.search(r"\(([A-Z0-9_.-]+)\)", text)
    if match:
        return match.group(1)
    match = re.search(r"(SBSQL\.[A-Z0-9_.-]+)", text)
    if match:
        return match.group(1)
    return text.strip()


def canonicalize_lane(lane: LaneSpec, result: RunResult) -> tuple[str, int, str, list[str]]:
    actual_lines = semantic_lines(result.stdout)
    if result.returncode == 0:
        if tuple(actual_lines) != lane.semantic_lines:
            raise RouteBenchmarkError(
                f"{result.route}:{lane.lane_id} semantic output mismatch: "
                f"expected={lane.semantic_lines!r} actual={actual_lines!r} stderr={result.stderr!r}"
            )
        return (
            f"{lane.lane_id}:accepted:" + "|".join(actual_lines),
            len(actual_lines),
            "accepted",
            [],
        )

    code = diagnostic_code(result)
    if lane.expected_refusal and code == lane.expected_refusal:
        return (
            f"{lane.lane_id}:refused_expected:{code}",
            0,
            "refused_expected",
            [code],
        )
    raise RouteBenchmarkError(
        f"{result.route}:{lane.lane_id} failed rc={result.returncode} "
        f"diagnostic={code!r} stdout={result.stdout!r} stderr={result.stderr!r}"
    )


def route_counters(result: RunResult, semantic_count: int) -> dict[str, Any]:
    return {
        "returncode": result.returncode,
        "stdout_line_count": len(result.stdout.splitlines()) if result.stdout else 0,
        "stderr_line_count": len(result.stderr.splitlines()) if result.stderr else 0,
        "semantic_line_count": semantic_count,
    }


def run_route_lanes(args: argparse.Namespace,
                    route: RouteContext,
                    run_id: str,
                    timestamp_utc: str) -> list[dict[str, Any]]:
    inputs = route_inputs(route)
    records: list[dict[str, Any]] = []
    for lane in LANES:
        prefix = f"cdp_{lane.lane_id}".replace("-", "_")
        result = run_sb_isql(route, lane, lane.script_builder(prefix, inputs))
        canonical, rows_returned, operation_status, messages = canonicalize_lane(lane, result)
        record = support.benchmark_record(
            run_id=run_id,
            timestamp_utc=timestamp_utc,
            build_mode=args.build_mode,
            route=route.name,
            database_path=result.database,
            fixture_name=lane.fixture_name,
            statement_id=lane.statement_id,
            operation=lane.operation,
            lane_id=lane.lane_id,
            rows_input=lane.rows_input,
            rows_affected=lane.rows_affected if operation_status == "accepted" else 0,
            elapsed_ms=round(result.elapsed_ms, 3),
            rows_returned=rows_returned,
            result_text=canonical,
            status="passed",
            message_vector=messages,
            operation_status=operation_status,
            route_counters=route_counters(result, rows_returned),
        )
        record["ctest_smoke_rows"] = lane.rows_input
        record["semantic_result"] = canonical
        record["stdout_path"] = str(result.stdout_path)
        record["stderr_path"] = str(result.stderr_path)
        if lane.lane_id == "copy_10k":
            record["preserved_large_lane_rows"] = [10_000, 100_000, 1_000_000]
        elif lane.lane_id == "execution_plan10_joins":
            record["current_route_contract"] = "accepted_uuid_resolved_public_join_route"
        records.append(record)
    return records


def compare_lane_hashes(records: list[dict[str, Any]]) -> None:
    lanes = sorted({record["lane_id"] for record in records})
    for lane in lanes:
        lane_records = [record for record in records if record["lane_id"] == lane]
        hashes = {record["result_hash"] for record in lane_records}
        if len(hashes) != 1:
            raise RouteBenchmarkError(f"{lane} result hashes differ: {sorted(hashes)}")
        statuses = {record["operation_status"] for record in lane_records}
        if len(statuses) != 1:
            raise RouteBenchmarkError(f"{lane} operation statuses differ: {sorted(statuses)}")


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def build_payload(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    run_id = f"cdp-route-split-{int(time.time())}"
    timestamp_utc = support.utc_timestamp()
    routes = [
        start_embedded(args, work),
        start_local_ipc(args, work),
        start_inet(args, work),
    ]
    records: list[dict[str, Any]] = []
    try:
        for route in routes:
            records.extend(run_route_lanes(args, route, run_id, timestamp_utc))
    finally:
        for route in reversed(routes):
            stop_route(route)

    compare_lane_hashes(records)
    route_names = [route.name for route in routes]
    environments = [
        support.capture_environment(
            route=route.name,
            storage_path=route.database,
            build_mode=args.build_mode,
            tracing_mode=None)
        for route in routes
    ]
    fingerprint = support.environment_fingerprint(environments)
    output_dir = work / "evidence"
    benchmark_json = output_dir / "cdp-route-split-benchmark.json"
    baseline_json = output_dir / "cdp-route-split-baseline.json"
    corpus_json = output_dir / "cdp-route-split-corpus.json"
    environment_json = output_dir / "cdp-route-split-environment.json"
    manifest_json = output_dir / "cdp-route-split-evidence-manifest.json"

    corpus = support.build_corpus_metadata()
    targets = support.performance_targets()
    baseline = {
        "schema_version": support.BASELINE_SCHEMA_VERSION,
        "run_id": run_id,
        "timestamp_utc": timestamp_utc,
        "scope": "first_batch_route_split_copy_select_join_aggregate_update",
        "records": records,
        "route_names": route_names,
        "lane_ids": [lane.lane_id for lane in LANES],
        "performance_targets": targets,
        "baseline_status": "captured_for_ctest_fixture_and_future_large_runs",
    }
    manifest = support.build_evidence_manifest(
        run_id=run_id,
        repo_root=Path(__file__).resolve().parents[3],
        build_mode=args.build_mode,
        benchmark_json=benchmark_json,
        baseline_json=baseline_json,
        corpus_json=corpus_json,
        environment_json=environment_json,
        route_names=route_names,
        records=records,
        logs=[str(path) for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err"))],
        environment_fingerprint=fingerprint)
    payload = {
        "schema_version": support.BENCHMARK_SCHEMA_VERSION,
        "run_id": run_id,
        "timestamp_utc": timestamp_utc,
        "gate": "cdp_route_split_benchmark_gate",
        "supported_slices": list(support.SUPPORTED_SLICES),
        "route_scope": "embedded ipc inet",
        "acceptance_status": "route_fixture_evidence_closed_for_first_batch",
        "lane_ids": [lane.lane_id for lane in LANES],
        "benchmark_records": records,
        "baseline": baseline,
        "corpus": corpus,
        "environments": environments,
        "performance_targets": targets,
        "evidence_manifest": manifest,
        "output_artifacts": {
            "benchmark_json": str(benchmark_json),
            "baseline_json": str(baseline_json),
            "corpus_json": str(corpus_json),
            "environment_json": str(environment_json),
            "evidence_manifest_json": str(manifest_json),
        },
    }

    errors = support.validate_payload(payload)
    if errors:
        raise RouteBenchmarkError("; ".join(errors))

    support.write_json(corpus_json, corpus)
    support.write_json(baseline_json, baseline)
    support.write_json(environment_json, {"environments": environments, "environment_fingerprint": fingerprint})
    support.write_json(manifest_json, manifest)
    support.write_json(benchmark_json, payload)
    artifact_errors = support.validate_written_artifacts(
        benchmark_json=benchmark_json,
        baseline_json=baseline_json,
        corpus_json=corpus_json,
        environment_json=environment_json,
        manifest_json=manifest_json,
    )
    if artifact_errors:
        raise RouteBenchmarkError("; ".join(artifact_errors))
    payload["benchmark_json_path"] = str(benchmark_json)
    return payload


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--build-mode", default="unknown")
    args = parser.parse_args(argv[1:])

    work = make_work_dir(Path(args.work_dir))
    try:
        payload = build_payload(args, work)
        print(f"cdp_route_split_benchmark_gate=passed output={payload['benchmark_json_path']}")
        print("cdp_route_split_lanes=" + ",".join(payload["lane_ids"]))
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        print(f"cdp_route_split_benchmark_gate=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
