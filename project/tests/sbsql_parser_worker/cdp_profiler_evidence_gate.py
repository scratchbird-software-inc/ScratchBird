#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CDP-046 standalone profiler evidence CTest gate.

This gate collects diagnostic attribution evidence from live ScratchBird routes.
It intentionally does not claim benchmark-clean speed evidence: subprocess and
route process counters are measured proxies unless a ScratchBird capability
surface reports a bucket directly.
"""

from __future__ import annotations

import argparse
import os
import re
import resource
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


SCHEMA_VERSION = "cdp.profiler_evidence_gate.v1"
GATE_NAME = "cdp_profiler_evidence_gate"
VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
FORBIDDEN_EXECUTION_PLAN_PATH = "docs" + "/execution-plans/"
SHOW_MANAGEMENT_FIELDS = (
    "read_only_mode",
    "catalog_generation_id",
    "security_epoch",
    "resource_epoch",
    "performance_optimization_surface",
    "optimization_profile",
    "copy_append_batching_enabled",
    "plan_cache_enabled",
    "descriptor_metadata_cache_enabled",
    "statistics_epoch",
    "agent_worker_status",
    "resource_governor_state",
    "parser_finality_authority",
    "reference_finality_authority",
)
DIAGNOSTIC_POLICY = {
    "diagnostic_run": True,
    "benchmark_clean_claim": False,
    "policy": (
        "CDP-046 profiler evidence is diagnostic attribution evidence only. "
        "It uses live ScratchBird routes, fresh databases, route equivalence "
        "hashes, capability reports where exposed, and measured subprocess or "
        "route-process proxies where deeper instrumentation is unavailable. "
        "It is not benchmark-clean speed evidence and must not be used as an "
        "optimized-over-baseline performance win claim."
    ),
    "forbidden_shortcuts": [
        "no reference engine execution",
        "no parser-owned finality",
        "no WAL authority shortcut",
        "no SQLite shortcut",
        "no execution_plan file runtime dependency",
    ],
}


class ProfilerGateError(RuntimeError):
    pass


@dataclass(frozen=True)
class Workload:
    workload_id: str
    operation: str
    rows_input: int
    expected_lines: tuple[str, ...]
    script_builder: Callable[[str, dict[str, Path]], str]


@dataclass
class ProcessProbe:
    pid: int
    role: str
    before: dict[str, Any] = field(default_factory=dict)
    after: dict[str, Any] = field(default_factory=dict)


@dataclass
class Route:
    name: str
    root: Path
    database: Path
    sb_isql: str
    fixed_args: list[str] = field(default_factory=list)
    embedded: bool = False
    processes: list[subprocess.Popen[bytes]] = field(default_factory=list)

    def cli_args(self) -> list[str]:
        if self.embedded:
            self.database.parent.mkdir(parents=True, exist_ok=True)
            return [
                self.sb_isql,
                str(self.database),
                "--mode=embedded",
                "--sslmode=disable",
            ]
        return list(self.fixed_args)


@dataclass
class RunResult:
    route: str
    workload: str
    returncode: int
    stdout: str
    stderr: str
    stdout_path: Path
    stderr_path: Path
    script_path: Path
    elapsed_ms: float
    user_ms: float
    system_ms: float
    involuntary_context_switches: int
    voluntary_context_switches: int
    input_blocks: int
    output_blocks: int
    max_rss_kb_delta: int
    db_size_before: int
    db_size_after: int
    file_count_before: int
    file_count_after: int
    process_samples: list[dict[str, Any]]


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "cdp_profiler")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="cdp046_", dir=root))
        endpoint_probe = candidate / "i" / "sc" / "s.sock"
        listener_probe = candidate / "n" / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(endpoint_probe)), len(str(listener_probe)), len(str(candidate / "e.sbdb"))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise ProfilerGateError("unable to allocate a short-enough profiler evidence workspace")


def auth_file(database: Path) -> None:
    Path(str(database) + ".sb.local_password_auth").write_text(
        f"alice\tlocal_password\t{VERIFIER}\n", encoding="utf-8"
    )


def wait_for_path(path: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise ProfilerGateError(f"timed out waiting for {path}")


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
    raise ProfilerGateError(f"timed out waiting for listener port {port}: {last_error}")


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


def start_embedded(args: argparse.Namespace, work: Path) -> Route:
    root = work / "e"
    root.mkdir(parents=True, exist_ok=True)
    return Route(name="embedded", root=root, database=root / "profiler.sbdb", sb_isql=args.sb_isql, embedded=True)


def start_local_ipc(args: argparse.Namespace, work: Path) -> Route:
    root = work / "i"
    database = root / "profiler.sbdb"
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
    return Route(
        name="local-ipc",
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


def start_inet(args: argparse.Namespace, work: Path) -> Route:
    root = work / "n"
    database = root / "profiler.sbdb"
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
    return Route(
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


def quote_sql_path(path: Path) -> str:
    text = str(path)
    if "'" in text:
        raise ProfilerGateError(f"fixture path cannot be single-quoted in SQL: {path}")
    return f"'{text}'"


def write_rows(path: Path, values: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"id={value}\n" for value in values), encoding="utf-8")


def route_inputs(route: Route) -> dict[str, Path]:
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


def table_name(prefix: str, suffix: str) -> str:
    return f"{prefix}_{suffix}".replace("-", "_")


def setup_copy_sql(table: str, rows_path: Path) -> str:
    return "\n".join([
        f"CREATE TABLE {table} (id int);",
        f"\\copy {table} FROM {quote_sql_path(rows_path)}",
    ])


def select_one_script(prefix: str, inputs: dict[str, Path]) -> str:
    del prefix, inputs
    return "SELECT 1;\n"


def copy_count_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "copy")
    return "\n".join([setup_copy_sql(table, inputs["main"]), f"SELECT COUNT(*) FROM {table};", ""])


def range_select_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "range")
    return "\n".join([
        setup_copy_sql(table, inputs["main"]),
        f"SELECT id FROM {table} ORDER BY id ASC LIMIT 3 OFFSET 1;",
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


WORKLOADS: tuple[Workload, ...] = (
    Workload("select_one", "select", 0, ("1",), select_one_script),
    Workload("copy_count", "copy", 5, ("5",), copy_count_script),
    Workload("range_select", "select", 5, ("2", "3", "4"), range_select_script),
    Workload("single_row_update", "update", 5, ("20", "1", "3", "4", "5", "20"), update_script),
    Workload("join_probe", "join", 7, ("2|2", "3|3"), join_script),
)


def dir_stats(root: Path) -> tuple[int, int]:
    total = 0
    count = 0
    if not root.exists():
        return 0, 0
    for path in root.rglob("*"):
        try:
            if path.is_file():
                count += 1
                total += path.stat().st_size
        except OSError:
            continue
    return total, count


def read_process_sample(proc: subprocess.Popen[bytes], role: str) -> dict[str, Any]:
    pid = proc.pid
    sample: dict[str, Any] = {"pid": pid, "role": role, "available": False}
    try:
        stat_text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8", errors="replace")
        status_text = Path(f"/proc/{pid}/status").read_text(encoding="utf-8", errors="replace")
    except OSError:
        sample["reason"] = "procfs_unavailable_or_process_exited"
        return sample
    fields = stat_text.split()
    ticks = int(os.sysconf(os.sysconf_names.get("SC_CLK_TCK", "SC_CLK_TCK")))
    threads = 0
    voluntary = 0
    involuntary = 0
    for line in status_text.splitlines():
        if line.startswith("Threads:"):
            threads = int(line.split()[1])
        elif line.startswith("voluntary_ctxt_switches:"):
            voluntary = int(line.split()[1])
        elif line.startswith("nonvoluntary_ctxt_switches:"):
            involuntary = int(line.split()[1])
    sample.update({
        "available": True,
        "utime_ticks": int(fields[13]),
        "stime_ticks": int(fields[14]),
        "cpu_ms": round(((int(fields[13]) + int(fields[14])) / ticks) * 1000.0, 3),
        "threads": threads,
        "voluntary_context_switches": voluntary,
        "involuntary_context_switches": involuntary,
    })
    return sample


def process_probes(route: Route) -> list[ProcessProbe]:
    probes: list[ProcessProbe] = []
    for index, proc in enumerate(route.processes):
        role = "server" if index == 0 else "listener"
        probe = ProcessProbe(pid=proc.pid, role=role)
        probe.before = read_process_sample(proc, role)
        probes.append(probe)
    return probes


def finish_process_probes(probes: list[ProcessProbe], route: Route) -> list[dict[str, Any]]:
    by_pid = {probe.pid: probe for probe in probes}
    for index, proc in enumerate(route.processes):
        role = "server" if index == 0 else "listener"
        probe = by_pid.get(proc.pid)
        if probe is not None:
            probe.after = read_process_sample(proc, role)
    return [process_delta(probe) for probe in probes]


def process_delta(probe: ProcessProbe) -> dict[str, Any]:
    before = probe.before
    after = probe.after
    if not before.get("available") or not after.get("available"):
        return {"pid": probe.pid, "role": probe.role, "source": "measured_proxy", "available": False}
    return {
        "pid": probe.pid,
        "role": probe.role,
        "source": "measured_proxy",
        "available": True,
        "cpu_ms_delta": round(float(after["cpu_ms"]) - float(before["cpu_ms"]), 3),
        "threads_before": before["threads"],
        "threads_after": after["threads"],
        "voluntary_context_switch_delta": after["voluntary_context_switches"] - before["voluntary_context_switches"],
        "involuntary_context_switch_delta": after["involuntary_context_switches"] - before["involuntary_context_switches"],
    }


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
        lines.append(line)
    return lines


def show_management_lines(stdout: str) -> list[str]:
    lines = semantic_lines(stdout)
    return [line for line in lines if not line.startswith("Stopping due to error")]


def parse_show_management(stdout: str) -> dict[str, str]:
    rows = show_management_lines(stdout)
    candidates: list[dict[str, str]] = []
    for row in rows:
        values = row.split("|")
        if len(values) >= 38 and values[4] == "scratchbird.performance_optimization_surface.v1":
            fields = {
                "read_only_mode": values[0],
                "catalog_generation_id": values[1],
                "security_epoch": values[2],
                "resource_epoch": values[3],
                "performance_optimization_surface": values[4],
                "optimization_profile": values[5],
                "copy_append_batching_enabled": values[6],
                "plan_cache_enabled": values[7],
                "descriptor_metadata_cache_enabled": values[8],
                "statistics_epoch": values[9],
                "agent_worker_status": values[17],
                "resource_governor_state": values[26],
                "parser_finality_authority": values[35],
                "reference_finality_authority": values[36],
            }
            candidates.append(fields)
    if len(candidates) != 1:
        raise ProfilerGateError(f"SHOW MANAGEMENT performance surface row drifted: {rows!r}")
    fields = candidates[0]
    expected = {
        "performance_optimization_surface": "scratchbird.performance_optimization_surface.v1",
        "copy_append_batching_enabled": "true",
        "plan_cache_enabled": "true",
        "descriptor_metadata_cache_enabled": "true",
        "parser_finality_authority": "false",
        "reference_finality_authority": "false",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise ProfilerGateError(f"SHOW MANAGEMENT {key} drifted: {fields.get(key)!r}")
    if not fields["statistics_epoch"].isdigit():
        raise ProfilerGateError("SHOW MANAGEMENT statistics_epoch was not numeric")
    return fields


def run_show_management(route: Route) -> dict[str, Any]:
    case_dir = route.root / "cases" / "show_management"
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    script.write_text("SHOW MANAGEMENT;\n", encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    completed = subprocess.run(
        route.cli_args() + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=20,
    )
    stdout = out_path.read_text(encoding="utf-8", errors="replace").strip()
    stderr = err_path.read_text(encoding="utf-8", errors="replace").strip()
    if completed.returncode != 0:
        raise ProfilerGateError(
            f"{route.name}:show_management failed rc={completed.returncode} "
            f"diagnostic={diagnostic_code(stdout, stderr)!r}"
        )
    fields = parse_show_management(stdout)
    return {
        "route": route.name,
        "source": "show_management_capability_report",
        "fields": {
            "performance_optimization_surface": fields["performance_optimization_surface"],
            "optimization_profile": fields["optimization_profile"],
            "copy_append_batching_enabled": fields["copy_append_batching_enabled"],
            "plan_cache_enabled": fields["plan_cache_enabled"],
            "descriptor_metadata_cache_enabled": fields["descriptor_metadata_cache_enabled"],
            "statistics_epoch": fields["statistics_epoch"],
            "agent_worker_status": fields["agent_worker_status"],
            "resource_governor_state": fields["resource_governor_state"],
            "parser_finality_authority": fields["parser_finality_authority"],
            "reference_finality_authority": fields["reference_finality_authority"],
        },
        "result_hash": support.hash_text("|".join(fields[name] for name in SHOW_MANAGEMENT_FIELDS)),
        "stdout_path": str(out_path),
        "stderr_path": str(err_path),
    }


def diagnostic_code(stdout: str, stderr: str) -> str:
    text = "\n".join(part for part in (stderr, stdout) if part)
    match = re.search(r"\(([A-Z0-9_.-]+)\)", text)
    if match:
        return match.group(1)
    match = re.search(r"(SBSQL\.[A-Z0-9_.-]+)", text)
    if match:
        return match.group(1)
    return text.strip()


def run_workload(route: Route, workload: Workload, inputs: dict[str, Path]) -> RunResult:
    case_dir = route.root / "cases" / workload.workload_id
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    prefix = f"cdp046_{workload.workload_id}"
    script.write_text(workload.script_builder(prefix, inputs), encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    db_size_before, file_count_before = dir_stats(route.database.parent)
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    probes = process_probes(route)
    started = time.monotonic()
    completed = subprocess.run(
        route.cli_args() + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=40,
    )
    elapsed_ms = (time.monotonic() - started) * 1000.0
    usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    process_samples = finish_process_probes(probes, route)
    db_size_after, file_count_after = dir_stats(route.database.parent)
    return RunResult(
        route=route.name,
        workload=workload.workload_id,
        returncode=completed.returncode,
        stdout=out_path.read_text(encoding="utf-8", errors="replace").strip(),
        stderr=err_path.read_text(encoding="utf-8", errors="replace").strip(),
        stdout_path=out_path,
        stderr_path=err_path,
        script_path=script,
        elapsed_ms=round(elapsed_ms, 3),
        user_ms=round((usage_after.ru_utime - usage_before.ru_utime) * 1000.0, 3),
        system_ms=round((usage_after.ru_stime - usage_before.ru_stime) * 1000.0, 3),
        involuntary_context_switches=usage_after.ru_nivcsw - usage_before.ru_nivcsw,
        voluntary_context_switches=usage_after.ru_nvcsw - usage_before.ru_nvcsw,
        input_blocks=usage_after.ru_inblock - usage_before.ru_inblock,
        output_blocks=usage_after.ru_oublock - usage_before.ru_oublock,
        max_rss_kb_delta=max(0, usage_after.ru_maxrss - usage_before.ru_maxrss),
        db_size_before=db_size_before,
        db_size_after=db_size_after,
        file_count_before=file_count_before,
        file_count_after=file_count_after,
        process_samples=process_samples,
    )


def require_accepted(workload: Workload, result: RunResult) -> list[str]:
    if result.returncode != 0:
        raise ProfilerGateError(
            f"{result.route}:{workload.workload_id} failed rc={result.returncode} "
            f"diagnostic={diagnostic_code(result.stdout, result.stderr)!r} "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    actual = semantic_lines(result.stdout)
    if tuple(actual) != workload.expected_lines:
        raise ProfilerGateError(
            f"{result.route}:{workload.workload_id} semantic mismatch "
            f"expected={workload.expected_lines!r} actual={actual!r}"
        )
    return actual


def bucket(name: str, source: str, value: Any, evidence: str) -> dict[str, Any]:
    return {"bucket": name, "source": source, "value": value, "evidence": evidence}


def attribution_buckets(result: RunResult, capability: dict[str, Any]) -> dict[str, dict[str, Any]]:
    server_cpu_ms = sum(
        sample.get("cpu_ms_delta", 0.0)
        for sample in result.process_samples
        if sample.get("available")
    )
    capability_fields = capability["fields"]
    thread_activity = [
        {
            "role": sample.get("role"),
            "threads_before": sample.get("threads_before"),
            "threads_after": sample.get("threads_after"),
            "cpu_ms_delta": sample.get("cpu_ms_delta"),
        }
        for sample in result.process_samples
    ]
    return {
        "cpu": bucket(
            "cpu",
            "measured_proxy",
            {
                "wall_ms": result.elapsed_ms,
                "sb_isql_user_ms": result.user_ms,
                "sb_isql_system_ms": result.system_ms,
                "route_process_cpu_ms_delta": round(server_cpu_ms, 3),
            },
            "resource.getrusage(RUSAGE_CHILDREN), monotonic wall clock, and /proc route-process samples",
        ),
        "lock_waits": bucket(
            "lock_waits",
            "capability_report",
            {
                "lock_wait_ms": None,
                "mutex_hold_ms": None,
                "reason": "SHOW MANAGEMENT reports optimization capability but not per-workload lock or mutex timing",
            },
            "live SHOW MANAGEMENT capability report plus explicit no-speed-claim null timing",
        ),
        "syscalls_file_io": bucket(
            "syscalls_file_io",
            "measured_proxy",
            {
                "input_blocks": result.input_blocks,
                "output_blocks": result.output_blocks,
                "voluntary_context_switches": result.voluntary_context_switches,
                "involuntary_context_switches": result.involuntary_context_switches,
            },
            "resource.getrusage child block and context-switch counters",
        ),
        "allocator_pressure": bucket(
            "allocator_pressure",
            "measured_proxy",
            {"max_rss_kb_delta": result.max_rss_kb_delta},
            "child ru_maxrss delta; allocator-specific counters not exposed by this route",
        ),
        "page_filespace_growth": bucket(
            "page_filespace_growth",
            "measured_proxy",
            {
                "database_tree_bytes_before": result.db_size_before,
                "database_tree_bytes_after": result.db_size_after,
                "database_tree_bytes_delta": result.db_size_after - result.db_size_before,
                "file_count_before": result.file_count_before,
                "file_count_after": result.file_count_after,
            },
            "fresh database directory file-size scan around workload",
        ),
        "index_maintenance": bucket(
            "index_maintenance",
            "capability_report",
            "not_exposed_by_current_sb_isql_live_route_surface",
            "no index-maintenance timing counter was emitted; no speed claim is made",
        ),
        "parser_lowering_plan_timing": bucket(
            "parser_lowering_plan_timing",
            "capability_report",
            {
                "parse_ms": None,
                "bind_ms": None,
                "lower_ms": None,
                "plan_ms": None,
                "plan_cache_enabled": capability_fields["plan_cache_enabled"],
                "descriptor_metadata_cache_enabled": capability_fields["descriptor_metadata_cache_enabled"],
                "statistics_epoch": capability_fields["statistics_epoch"],
                "reason": "phase timing not exposed by current sb_isql route smoke",
            },
            "live SHOW MANAGEMENT capability report; diagnostic build trace hooks may fill phase timing",
        ),
        "agent_cpu_thread_activity": bucket(
            "agent_cpu_thread_activity",
            "measured_proxy",
            {
                "show_management_agent_worker_status": capability_fields["agent_worker_status"],
                "route_process_threads": thread_activity,
            },
            "/proc samples for sb_server/sb_listener plus SHOW MANAGEMENT agent status",
        ),
        "resource_governor_throttling": bucket(
            "resource_governor_throttling",
            "capability_report",
            {
                "resource_governor_state": capability_fields["resource_governor_state"],
                "throttle_event": None,
                "reason": "SHOW MANAGEMENT reports governor state; no per-workload throttle event surfaced",
            },
            "live SHOW MANAGEMENT capability report",
        ),
    }


def run_route(route: Route, timestamp_utc: str) -> tuple[list[dict[str, Any]], list[str]]:
    inputs = route_inputs(route)
    capability = run_show_management(route)
    records: list[dict[str, Any]] = []
    hashes: list[str] = []
    for workload in WORKLOADS:
        result = run_workload(route, workload, inputs)
        accepted_lines = require_accepted(workload, result)
        result_text = f"{workload.workload_id}:accepted:" + "|".join(accepted_lines)
        result_hash = support.hash_text(result_text)
        hashes.append(result_hash)
        records.append({
            "timestamp_utc": timestamp_utc,
            "route": route.name,
            "workload": {
                "workload_id": workload.workload_id,
                "operation": workload.operation,
                "rows_input": workload.rows_input,
                "accepted": True,
                "semantic_lines": accepted_lines,
                "result_hash": result_hash,
            },
            "database_path": str(route.database),
            "stdout_path": str(result.stdout_path),
            "stderr_path": str(result.stderr_path),
            "script_path": str(result.script_path),
            "capability_report": capability,
            "baseline": {
                "diagnostic_run": True,
                "benchmark_clean_claim": False,
                "elapsed_ms": result.elapsed_ms,
                "user_ms": result.user_ms,
                "system_ms": result.system_ms,
                "source": "measured_proxy",
            },
            "optimized": {
                "diagnostic_run": True,
                "benchmark_clean_claim": False,
                "status": "not_measured_by_ctest_profiler_evidence_gate",
                "reason": "gate records attribution readiness only and does not compare optimized speed",
            },
            "attribution_buckets": attribution_buckets(result, capability),
        })
    return records, hashes


def compare_hashes(records: list[dict[str, Any]]) -> dict[str, dict[str, str]]:
    by_workload: dict[str, dict[str, str]] = {}
    for record in records:
        workload_id = record["workload"]["workload_id"]
        by_workload.setdefault(workload_id, {})[record["route"]] = record["workload"]["result_hash"]
    for workload_id, route_hashes in sorted(by_workload.items()):
        if set(route_hashes) != {"embedded", "local-ipc", "inet"}:
            raise ProfilerGateError(f"{workload_id} route coverage mismatch: {sorted(route_hashes)}")
        if len(set(route_hashes.values())) != 1:
            raise ProfilerGateError(f"{workload_id} result hashes differ: {route_hashes}")
    return by_workload


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def ensure_no_execution_plan_runtime_dependency(payload: dict[str, Any]) -> None:
    encoded = support.hash_json(payload) + "\n" + str(payload)
    if FORBIDDEN_EXECUTION_PLAN_PATH in encoded.replace("\\", "/"):
        raise ProfilerGateError("evidence payload contains a execution_plan runtime dependency")


def validate_payload(payload: dict[str, Any]) -> None:
    required_buckets = {
        "cpu",
        "lock_waits",
        "syscalls_file_io",
        "allocator_pressure",
        "page_filespace_growth",
        "index_maintenance",
        "parser_lowering_plan_timing",
        "agent_cpu_thread_activity",
        "resource_governor_throttling",
    }
    records = payload.get("records", [])
    if len(records) != len(WORKLOADS) * 3:
        raise ProfilerGateError(f"unexpected profiler record count: {len(records)}")
    for record in records:
        missing = required_buckets - set(record.get("attribution_buckets", {}))
        if missing:
            raise ProfilerGateError(f"{record.get('route')}:{record.get('workload')} missing buckets {sorted(missing)}")
        capability = record.get("capability_report", {})
        fields = capability.get("fields", {})
        for key in ("agent_worker_status", "resource_governor_state", "plan_cache_enabled"):
            if not fields.get(key):
                raise ProfilerGateError(f"{record.get('route')} missing capability field {key}")


def build_payload(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    run_id = f"cdp046-profiler-evidence-{int(time.time())}"
    timestamp_utc = support.utc_timestamp()
    routes = [
        start_embedded(args, work),
        start_local_ipc(args, work),
        start_inet(args, work),
    ]
    records: list[dict[str, Any]] = []
    try:
        for route in routes:
            route_records, _ = run_route(route, timestamp_utc)
            records.extend(route_records)
    finally:
        for route in reversed(routes):
            for proc in reversed(route.processes):
                stop_process(proc)

    route_hashes = compare_hashes(records)
    environments = [
        support.capture_environment(
            route=route.name,
            storage_path=route.database,
            build_mode=args.build_mode,
            tracing_mode=None,
        )
        for route in routes
    ]
    output_dir = work / "evidence"
    evidence_json = output_dir / "cdp-profiler-evidence.json"
    corpus = support.build_corpus_metadata()
    payload = {
        "schema_version": SCHEMA_VERSION,
        "run_id": run_id,
        "timestamp_utc": timestamp_utc,
        "gate": GATE_NAME,
        "slice": "CDP-046",
        "readiness_label": "CDP_P5_PROFILER_EVIDENCE_READY",
        "corpus_version": support.CORPUS_VERSION,
        "corpus": corpus,
        "diagnostic_run": True,
        "benchmark_clean_claim": False,
        "diagnostic_policy": DIAGNOSTIC_POLICY,
        "route_scope": ["embedded", "local-ipc", "inet"],
        "workload_scope": [workload.workload_id for workload in WORKLOADS],
        "route_equivalence": {
            "accepted_workloads_have_equivalent_result_hashes": True,
            "result_hashes_by_workload": route_hashes,
        },
        "records": records,
        "environments": environments,
        "source_commit": support.read_git_commit(Path(__file__).resolve().parents[3]),
        "output_artifacts": {"profiler_evidence_json": str(evidence_json)},
    }
    ensure_no_execution_plan_runtime_dependency(payload)
    validate_payload(payload)
    support.write_json(evidence_json, payload)
    payload["profiler_evidence_json_path"] = str(evidence_json)
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
        print(f"{GATE_NAME}=passed output={payload['profiler_evidence_json_path']}")
        print("cdp046_routes=embedded,local-ipc,inet")
        print("cdp046_policy=diagnostic_run_no_benchmark_clean_speed_claim")
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        print(f"{GATE_NAME}=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
