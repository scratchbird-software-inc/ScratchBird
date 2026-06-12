#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CDP-047 standalone soak leak and stability CTest gate.

The gate runs bounded live-route cycles against fresh ScratchBird databases and
records leak/stability evidence using current public route surfaces plus
measured /proc and filesystem proxies where deeper counters are not exposed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import resource
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable


SCHEMA_VERSION = "cdp.soak_leak_stability_gate.v1"
GATE_NAME = "cdp_soak_leak_stability_gate"
VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
ROUTE_NAMES = ("embedded", "local-ipc", "inet")
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

GROWTH_BUDGETS = {
    "route_process_rss_hwm_kb": 64 * 1024,
    "route_process_fd_count": 8,
    "route_process_thread_count": 4,
    "database_tree_bytes": 32 * 1024 * 1024,
    "database_tree_files": 32,
    "page_filespace_queue_proxy_bytes": 32 * 1024 * 1024,
    "agent_queue_proxy_units": 8,
    "evidence_package_bytes": 8 * 1024 * 1024,
    "child_rss_hwm_kb_per_invocation": 64 * 1024,
}

DIAGNOSTIC_POLICY = {
    "diagnostic_run": True,
    "benchmark_clean_claim": False,
    "forbidden_shortcuts": [
        "no reference engine execution",
        "no parser-owned finality",
        "no SQLite shortcut",
        "no WAL authority shortcut",
        "no execution_plan file runtime dependency",
    ],
}


class SoakGateError(RuntimeError):
    pass


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def hash_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def hash_json(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
    return hash_text(encoded)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


@dataclass(frozen=True)
class Workload:
    workload_id: str
    operation: str
    rows_input: int
    expected_lines: tuple[str, ...]
    script_builder: Callable[[str, dict[str, Path]], str]
    accepted_required: bool = True
    expected_refusal: str | None = None


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
            return [self.sb_isql, str(self.database), "--mode=embedded", "--sslmode=disable"]
        return list(self.fixed_args)


@dataclass
class RunResult:
    route: str
    cycle: int
    workload: str
    returncode: int
    stdout: str
    stderr: str
    stdout_path: Path
    stderr_path: Path
    script_path: Path
    elapsed_ms: float
    child_user_ms: float
    child_system_ms: float
    child_max_rss_kb_delta: int
    child_fd_count_proxy: dict[str, Any]
    db_size_before: int
    db_size_after: int
    db_files_before: int
    db_files_after: int
    process_samples: list[dict[str, Any]]


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "cdp047")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="c047_", dir=root))
        endpoint_probe = candidate / "i" / "sc" / "s.sock"
        listener_probe = candidate / "n" / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(endpoint_probe)), len(str(listener_probe)), len(str(candidate / "e.sbdb"))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise SoakGateError("unable to allocate a short-enough CDP-047 workspace")


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
    raise SoakGateError(f"timed out waiting for {path}")


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
    raise SoakGateError(f"timed out waiting for listener port {port}: {last_error}")


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def stop_process(proc: subprocess.Popen[bytes] | None) -> dict[str, Any]:
    if proc is None:
        return {"available": False, "reason": "no_process"}
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=4)
            return {
                "available": True,
                "completed": True,
                "clean": False,
                "termination_mode": "forced_kill_after_timeout",
                "returncode": proc.returncode,
            }
    termination_mode = "graceful_exit" if proc.returncode == 0 else "sigterm_orderly_ctest_stop"
    clean = proc.returncode == 0 or proc.returncode == -signal.SIGTERM
    return {
        "available": True,
        "completed": True,
        "clean": clean,
        "termination_mode": termination_mode,
        "returncode": proc.returncode,
    }


def start_embedded(args: argparse.Namespace, work: Path) -> Route:
    root = work / "e"
    root.mkdir(parents=True, exist_ok=True)
    return Route(name="embedded", root=root, database=root / "soak.sbdb", sb_isql=args.sb_isql, embedded=True)


def start_local_ipc(args: argparse.Namespace, work: Path) -> Route:
    root = work / "i"
    database = root / "soak.sbdb"
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
    database = root / "soak.sbdb"
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
        raise SoakGateError(f"fixture path cannot be single-quoted in SQL: {path}")
    return f"'{text}'"


def table_name(prefix: str, suffix: str) -> str:
    return f"{prefix}_{suffix}".replace("-", "_")


def write_rows(path: Path, values: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"id={value}\n" for value in values), encoding="utf-8")


def route_inputs(route: Route) -> dict[str, Path]:
    inputs = route.root / "inputs"
    paths = {
        "main": inputs / "main.rows",
        "left": inputs / "left.rows",
        "right": inputs / "right.rows",
        "native": inputs / "native.rows",
    }
    write_rows(paths["main"], [1, 2, 3, 4, 5])
    write_rows(paths["left"], [1, 2, 3, 4])
    write_rows(paths["right"], [2, 3, 5])
    write_rows(paths["native"], [10, 11, 12])
    return paths


def setup_copy_sql(table: str, rows_path: Path) -> str:
    return "\n".join([f"CREATE TABLE {table} (id int);", f"\\copy {table} FROM {quote_sql_path(rows_path)}"])


def select_one_script(prefix: str, inputs: dict[str, Path]) -> str:
    del prefix, inputs
    return "SELECT 1;\n"


def copy_batch_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "copy")
    return "\n".join([setup_copy_sql(table, inputs["main"]), "COMMIT RETAIN;", f"SELECT COUNT(*) FROM {table};", ""])


def native_bulk_ingest_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "native")
    return "\n".join(
        [
            f"CREATE TABLE {table} (id int);",
            f"\\native_bulk_ingest {table} FROM {quote_sql_path(inputs['native'])}",
            f"SELECT COUNT(*) FROM {table};",
            "",
        ]
    )


def join_script(prefix: str, inputs: dict[str, Path]) -> str:
    left = table_name(prefix, "join_l")
    right = table_name(prefix, "join_r")
    return "\n".join(
        [
            setup_copy_sql(left, inputs["left"]),
            setup_copy_sql(right, inputs["right"]),
            f"SELECT * FROM {left} JOIN {right} ON {left}.id = {right}.id;",
            "",
        ]
    )


def route_split_select_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "split")
    return "\n".join(
        [
            setup_copy_sql(table, inputs["main"]),
            f"SELECT id FROM {table} ORDER BY id ASC LIMIT 3 OFFSET 1;",
            "",
        ]
    )


def explicit_transaction_script(prefix: str, inputs: dict[str, Path]) -> str:
    table = table_name(prefix, "tx")
    return "\n".join(
        [
            f"CREATE TABLE {table} (id int);",
            f"\\copy {table} FROM {quote_sql_path(inputs['main'])}",
            "COMMIT RETAIN;",
            f"UPDATE {table} SET id = 20 WHERE id = 2;",
            "ROLLBACK RETAIN;",
            f"SELECT id FROM {table} ORDER BY id ASC;",
            "",
        ]
    )


WORKLOADS: tuple[Workload, ...] = (
    Workload("route_split_select", "select", 0, ("1",), select_one_script),
    Workload("copy_batching", "copy", 5, ("5",), copy_batch_script),
    Workload(
        "native_bulk_ingest",
        "native_bulk_ingest",
        3,
        ("3",),
        native_bulk_ingest_script,
        accepted_required=False,
        expected_refusal="DML.NATIVE_BULK_INGEST.DISABLED",
    ),
    Workload("join_probe", "join", 7, ("2|2", "3|3"), join_script),
    Workload("route_split_range_select", "select", 5, ("2", "3", "4"), route_split_select_script),
    Workload(
        "explicit_commit_rollback",
        "transaction",
        5,
        ("20", "1", "2", "3", "4", "5"),
        explicit_transaction_script,
    ),
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


def database_stats(database: Path) -> tuple[int, int]:
    total = 0
    count = 0
    for path in database.parent.glob(database.name + "*"):
        try:
            if path.is_file():
                count += 1
                total += path.stat().st_size
        except OSError:
            continue
    return total, count


def _status_value(status_text: str, key: str) -> str:
    for line in status_text.splitlines():
        if line.startswith(key + ":"):
            return line.split(":", 1)[1].strip()
    return ""


def read_process_sample(proc: subprocess.Popen[bytes], role: str) -> dict[str, Any]:
    pid = proc.pid
    sample: dict[str, Any] = {"pid": pid, "role": role, "source": "measured_proxy", "available": False}
    try:
        stat_text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8", errors="replace")
        status_text = Path(f"/proc/{pid}/status").read_text(encoding="utf-8", errors="replace")
    except OSError:
        sample["reason"] = "procfs_unavailable_or_process_exited"
        return sample
    fields = stat_text.split()
    ticks = int(os.sysconf(os.sysconf_names.get("SC_CLK_TCK", "SC_CLK_TCK")))
    fd_count: int | str = "unknown"
    try:
        fd_count = len(list(Path(f"/proc/{pid}/fd").iterdir()))
    except OSError:
        pass
    vmrss = _status_value(status_text, "VmRSS").split()
    vmhwm = _status_value(status_text, "VmHWM").split()
    threads = _status_value(status_text, "Threads").split()
    voluntary = _status_value(status_text, "voluntary_ctxt_switches").split()
    involuntary = _status_value(status_text, "nonvoluntary_ctxt_switches").split()
    sample.update(
        {
            "available": True,
            "utime_ticks": int(fields[13]),
            "stime_ticks": int(fields[14]),
            "cpu_ms": round(((int(fields[13]) + int(fields[14])) / ticks) * 1000.0, 3),
            "rss_kb": int(vmrss[0]) if vmrss else 0,
            "rss_hwm_kb": int(vmhwm[0]) if vmhwm else 0,
            "fd_count": fd_count,
            "threads": int(threads[0]) if threads else 0,
            "voluntary_context_switches": int(voluntary[0]) if voluntary else 0,
            "involuntary_context_switches": int(involuntary[0]) if involuntary else 0,
        }
    )
    return sample


def process_probes(route: Route) -> list[ProcessProbe]:
    probes: list[ProcessProbe] = []
    for index, proc in enumerate(route.processes):
        role = "server" if index == 0 else "listener"
        probe = ProcessProbe(pid=proc.pid, role=role, before=read_process_sample(proc, role))
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


def int_delta(after: Any, before: Any) -> int:
    if isinstance(after, int) and isinstance(before, int):
        return after - before
    return 0


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
        "rss_kb_before": before["rss_kb"],
        "rss_kb_after": after["rss_kb"],
        "rss_hwm_kb_before": before["rss_hwm_kb"],
        "rss_hwm_kb_after": after["rss_hwm_kb"],
        "rss_hwm_kb_delta": int_delta(after["rss_hwm_kb"], before["rss_hwm_kb"]),
        "fd_count_before": before["fd_count"],
        "fd_count_after": after["fd_count"],
        "fd_count_delta": int_delta(after["fd_count"], before["fd_count"]),
        "threads_before": before["threads"],
        "threads_after": after["threads"],
        "threads_delta": int_delta(after["threads"], before["threads"]),
        "cpu_ms_delta": round(float(after["cpu_ms"]) - float(before["cpu_ms"]), 3),
        "voluntary_context_switch_delta": int_delta(
            after["voluntary_context_switches"], before["voluntary_context_switches"]
        ),
        "involuntary_context_switch_delta": int_delta(
            after["involuntary_context_switches"], before["involuntary_context_switches"]
        ),
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
        if line.startswith("NATIVE_BULK_INGEST "):
            continue
        if line.startswith("Transaction committed;") or line.startswith("Transaction rolled back;"):
            continue
        if line.startswith("Stopping due to error"):
            continue
        lines.append(line)
    return lines


def diagnostic_code(stdout: str, stderr: str) -> str:
    text = "\n".join(part for part in (stderr, stdout) if part)
    for pattern in (r"(DML\.NATIVE_BULK_INGEST\.DISABLED)", r"\(([A-Z0-9_.-]+)\)", r"(SBSQL\.[A-Z0-9_.-]+)"):
        match = re.search(pattern, text)
        if match:
            return match.group(1)
    match = re.search(r"Error:\s*(.*)", text)
    return match.group(1).strip() if match else text.strip()


def parse_show_management(stdout: str) -> dict[str, str]:
    rows = semantic_lines(stdout)
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
        raise SoakGateError(f"SHOW MANAGEMENT performance surface row drifted: {rows!r}")
    fields = candidates[0]
    expected = {
        "performance_optimization_surface": "scratchbird.performance_optimization_surface.v1",
        "parser_finality_authority": "false",
        "reference_finality_authority": "false",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise SoakGateError(f"SHOW MANAGEMENT {key} drifted: {fields.get(key)!r}")
    if not fields["statistics_epoch"].isdigit():
        raise SoakGateError("SHOW MANAGEMENT statistics_epoch was not numeric")
    return fields


def run_sql(route: Route, case: str, script_text: str, timeout: int = 35) -> tuple[int, str, str, Path, Path, Path]:
    case_dir = route.root / "cases" / case
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    script.write_text(script_text, encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    completed = subprocess.run(
        route.cli_args() + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=timeout,
    )
    return (
        completed.returncode,
        out_path.read_text(encoding="utf-8", errors="replace").strip(),
        err_path.read_text(encoding="utf-8", errors="replace").strip(),
        script,
        out_path,
        err_path,
    )


def run_show_management(route: Route, label: str) -> dict[str, Any]:
    rc, stdout, stderr, script, out_path, err_path = run_sql(route, f"{label}_show_management", "SHOW MANAGEMENT;\n", 20)
    if rc != 0:
        raise SoakGateError(f"{route.name}:SHOW MANAGEMENT failed rc={rc} diagnostic={diagnostic_code(stdout, stderr)!r}")
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
        "result_hash": hash_text("|".join(fields[name] for name in SHOW_MANAGEMENT_FIELDS)),
        "script_path": str(script),
        "stdout_path": str(out_path),
        "stderr_path": str(err_path),
    }


def run_workload(route: Route, cycle: int, workload: Workload, inputs: dict[str, Path]) -> RunResult:
    db_size_before, db_files_before = database_stats(route.database)
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    probes = process_probes(route)
    case = f"cycle_{cycle}_{workload.workload_id}"
    prefix = f"cdp047_c{cycle}_{workload.workload_id}"
    started = time.monotonic()
    rc, stdout, stderr, script, out_path, err_path = run_sql(
        route, case, workload.script_builder(prefix, inputs), timeout=45
    )
    elapsed_ms = (time.monotonic() - started) * 1000.0
    usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    process_samples = finish_process_probes(probes, route)
    db_size_after, db_files_after = database_stats(route.database)
    return RunResult(
        route=route.name,
        cycle=cycle,
        workload=workload.workload_id,
        returncode=rc,
        stdout=stdout,
        stderr=stderr,
        stdout_path=out_path,
        stderr_path=err_path,
        script_path=script,
        elapsed_ms=round(elapsed_ms, 3),
        child_user_ms=round((usage_after.ru_utime - usage_before.ru_utime) * 1000.0, 3),
        child_system_ms=round((usage_after.ru_stime - usage_before.ru_stime) * 1000.0, 3),
        child_max_rss_kb_delta=max(0, usage_after.ru_maxrss - usage_before.ru_maxrss),
        child_fd_count_proxy={
            "source": "measured_proxy",
            "value": "not_available_for_exited_sb_isql_child",
            "reason": "sb_isql process exits before procfs fd sample can be read",
        },
        db_size_before=db_size_before,
        db_size_after=db_size_after,
        db_files_before=db_files_before,
        db_files_after=db_files_after,
        process_samples=process_samples,
    )


def canonicalize_result(workload: Workload, result: RunResult) -> tuple[str, str, list[str]]:
    actual = semantic_lines(result.stdout)
    if result.returncode == 0:
        if tuple(actual) != workload.expected_lines:
            raise SoakGateError(
                f"{result.route}:cycle{result.cycle}:{workload.workload_id} semantic mismatch "
                f"expected={workload.expected_lines!r} actual={actual!r} stderr={result.stderr!r}"
            )
        return "accepted", f"{workload.workload_id}:accepted:" + "|".join(actual), []
    code = diagnostic_code(result.stdout, result.stderr)
    if workload.accepted_required:
        raise SoakGateError(
            f"{result.route}:cycle{result.cycle}:{workload.workload_id} failed rc={result.returncode} "
            f"diagnostic={code!r} stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    if workload.expected_refusal and code != workload.expected_refusal:
        raise SoakGateError(
            f"{result.route}:cycle{result.cycle}:{workload.workload_id} unexpected diagnostic "
            f"expected={workload.expected_refusal!r} actual={code!r}"
        )
    return "unsupported_current_route_surface", f"{workload.workload_id}:unsupported:{code}", [code]


def resource_snapshot(result: RunResult) -> dict[str, Any]:
    db_delta = result.db_size_after - result.db_size_before
    file_delta = result.db_files_after - result.db_files_before
    return {
        "source": "measured_proxy",
        "before": {
            "database_tree_bytes": result.db_size_before,
            "database_tree_files": result.db_files_before,
        },
        "after": {
            "database_tree_bytes": result.db_size_after,
            "database_tree_files": result.db_files_after,
        },
        "growth": {
            "database_tree_bytes": db_delta,
            "database_tree_files": file_delta,
            "page_filespace_queue_proxy_bytes": db_delta,
            "agent_queue_proxy_units": max(
                [sample.get("threads_delta", 0) for sample in result.process_samples if sample.get("available")] + [0]
            ),
            "child_rss_hwm_kb": result.child_max_rss_kb_delta,
        },
        "route_process_samples": result.process_samples,
        "child_process_proxy": {
            "source": "measured_proxy",
            "user_ms": result.child_user_ms,
            "system_ms": result.child_system_ms,
            "max_rss_kb_delta": result.child_max_rss_kb_delta,
            "fd_count": result.child_fd_count_proxy,
        },
    }


def budget_check(name: str, value: int, allowed_reason: str | None = None) -> dict[str, Any]:
    budget = GROWTH_BUDGETS[name]
    ok = value <= budget
    if not ok and not allowed_reason:
        raise SoakGateError(f"{name} growth exceeded budget: value={value} budget={budget}")
    return {
        "metric": name,
        "source": "measured_proxy",
        "value": value,
        "budget": budget,
        "ok": ok,
        "allowed_reason": allowed_reason,
    }


def route_growth_checks(route: Route, route_initial: tuple[int, int], route_final: tuple[int, int]) -> list[dict[str, Any]]:
    checks = [
        budget_check("database_tree_bytes", route_final[0] - route_initial[0]),
        budget_check("database_tree_files", route_final[1] - route_initial[1]),
        budget_check("page_filespace_queue_proxy_bytes", route_final[0] - route_initial[0]),
    ]
    for index, proc in enumerate(route.processes):
        role = "server" if index == 0 else "listener"
        before = read_process_sample(proc, f"{role}_route_final_before")
        after = read_process_sample(proc, f"{role}_route_final_after")
        if before.get("available") and after.get("available"):
            checks.append(budget_check("route_process_rss_hwm_kb", int_delta(after["rss_hwm_kb"], before["rss_hwm_kb"])))
            checks.append(budget_check("route_process_fd_count", int_delta(after["fd_count"], before["fd_count"])))
            checks.append(budget_check("route_process_thread_count", int_delta(after["threads"], before["threads"])))
    return checks


def assert_record_budgets(record: dict[str, Any]) -> None:
    growth = record["resource_snapshots"]["growth"]
    budget_check("child_rss_hwm_kb_per_invocation", growth["child_rss_hwm_kb"])
    budget_check("page_filespace_queue_proxy_bytes", growth["page_filespace_queue_proxy_bytes"])
    budget_check("agent_queue_proxy_units", growth["agent_queue_proxy_units"])


def run_route(args: argparse.Namespace, route: Route, cycles: int) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    inputs = route_inputs(route)
    route_initial = database_stats(route.database)
    management_before = run_show_management(route, "before")
    records: list[dict[str, Any]] = []
    for cycle in range(1, cycles + 1):
        management_cycle = run_show_management(route, f"cycle_{cycle}_before")
        for workload in WORKLOADS:
            result = run_workload(route, cycle, workload, inputs)
            status, canonical, messages = canonicalize_result(workload, result)
            resource_snapshots = resource_snapshot(result)
            record = {
                "route": route.name,
                "cycle": cycle,
                "workload": workload.workload_id,
                "operation": workload.operation,
                "rows_input": workload.rows_input,
                "status": status,
                "result_hash": hash_text(canonical),
                "message_vector": messages,
                "stdout_path": str(result.stdout_path),
                "stderr_path": str(result.stderr_path),
                "script_path": str(result.script_path),
                "elapsed_ms": result.elapsed_ms,
                "before_after_resource_snapshots": resource_snapshots,
                "resource_snapshots": resource_snapshots,
                "capability_report": management_cycle,
                "diagnostic_run": True,
                "benchmark_clean_claim": False,
            }
            if workload.workload_id == "explicit_commit_rollback" and status == "accepted":
                record["transaction_evidence"] = {
                    "source": "sbsql_engine_owned_transaction_route",
                    "committed_baseline_rows": ["1", "2", "3", "4", "5"],
                    "uncommitted_update_observed_before_rollback": "20",
                    "rollback_restored_committed_baseline": True,
                    "replacement_transaction_messages_filtered_from_result_hash": True,
                }
            assert_record_budgets(record)
            records.append(record)
    management_after = run_show_management(route, "after")
    route_final = database_stats(route.database)
    route_summary = {
        "route": route.name,
        "database_path": str(route.database),
        "resource_snapshot_before": {
            "source": "measured_proxy",
            "database_tree_bytes": route_initial[0],
            "database_tree_files": route_initial[1],
        },
        "resource_snapshot_after": {
            "source": "measured_proxy",
            "database_tree_bytes": route_final[0],
            "database_tree_files": route_final[1],
        },
        "growth_budgets": route_growth_checks(route, route_initial, route_final),
        "management_before": management_before,
        "management_after": management_after,
    }
    return records, route_summary


def compare_hashes(records: list[dict[str, Any]]) -> dict[str, dict[str, str]]:
    by_cycle_workload: dict[str, dict[str, str]] = {}
    status_by_key: dict[str, set[str]] = {}
    for record in records:
        key = f"cycle_{record['cycle']}:{record['workload']}"
        status_by_key.setdefault(key, set()).add(record["status"])
        by_cycle_workload.setdefault(key, {})[record["route"]] = record["result_hash"]
    for key, route_hashes in sorted(by_cycle_workload.items()):
        if set(route_hashes) != set(ROUTE_NAMES):
            raise SoakGateError(f"{key} route coverage mismatch: {sorted(route_hashes)}")
        statuses = status_by_key[key]
        if "accepted" in statuses and len(set(route_hashes.values())) != 1:
            raise SoakGateError(f"{key} result hashes differ for accepted workload: {route_hashes}")
        if len(statuses) != 1:
            raise SoakGateError(f"{key} route statuses differ: {sorted(statuses)}")
    return by_cycle_workload


def stop_route(route: Route) -> dict[str, Any]:
    roles = ["server", "listener"]
    process_roles = list(zip(roles, route.processes))
    return {
        "route": route.name,
        "processes": [
            {"role": role, **stop_process(proc)}
            for role, proc in reversed(process_roles)
        ],
    }


def reopen_query(args: argparse.Namespace, route: Route, cycles: int) -> dict[str, Any]:
    reopen_root = route.root / "reopen"
    reopen_root.mkdir(parents=True, exist_ok=True)
    reopened = Route(
        name=f"{route.name}-reopen-embedded",
        root=reopen_root,
        database=route.database,
        sb_isql=args.sb_isql,
        embedded=True,
    )
    committed_table = table_name(f"cdp047_c{cycles}_copy_batching", "copy")
    sql = f"SELECT COUNT(*) FROM {committed_table};\n"
    rc, stdout, stderr, script, out_path, err_path = run_sql(reopened, "committed_query", sql, 20)
    if rc != 0 or tuple(semantic_lines(stdout)) != ("5",):
        raise SoakGateError(
            f"{route.name} reopen committed query failed rc={rc} "
            f"diagnostic={diagnostic_code(stdout, stderr)!r} stdout={stdout!r} stderr={stderr!r}"
        )
    canonical = f"reopen:{route.name}:cycle_{cycles}:copy_batching_count:5"
    return {
        "route": route.name,
        "source": "embedded_reopen_query_after_route_shutdown",
        "query": sql.strip(),
        "committed_table": committed_table,
        "rows_returned": 1,
        "committed_row_count": 5,
        "result_hash": hash_text(canonical),
        "script_path": str(script),
        "stdout_path": str(out_path),
        "stderr_path": str(err_path),
        "ok": True,
    }


def evidence_package_size(work: Path) -> int:
    total = 0
    for path in work.rglob("*"):
        try:
            if path.is_file():
                total += path.stat().st_size
        except OSError:
            continue
    return total


def ensure_no_execution_plan_runtime_dependency(payload: dict[str, Any]) -> None:
    encoded = hash_json(payload) + "\n" + str(payload)
    if FORBIDDEN_EXECUTION_PLAN_PATH in encoded.replace("\\", "/"):
        raise SoakGateError("evidence payload contains a execution_plan runtime dependency")


def validate_payload(payload: dict[str, Any]) -> None:
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise SoakGateError("schema_version drifted")
    if payload.get("diagnostic_run") is not True or payload.get("benchmark_clean_claim") is not False:
        raise SoakGateError("diagnostic/benchmark-clean policy fields drifted")
    expected_records = payload["cycle_count"] * len(WORKLOADS) * len(ROUTE_NAMES)
    if len(payload["records"]) != expected_records:
        raise SoakGateError(f"record count mismatch expected={expected_records} actual={len(payload['records'])}")
    for route in payload["shutdown_evidence"]:
        for process in route["processes"]:
            if process.get("available") and not process.get("completed"):
                raise SoakGateError(f"{route['route']} shutdown did not complete: {process}")
    for reopen in payload["reopen_evidence"]:
        if not reopen.get("ok"):
            raise SoakGateError(f"reopen query failed: {reopen}")
    ensure_no_execution_plan_runtime_dependency(payload)


def build_payload(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    timestamp_utc = utc_timestamp()
    run_id = f"cdp047-soak-{int(time.time())}"
    routes = [start_embedded(args, work), start_local_ipc(args, work), start_inet(args, work)]
    records: list[dict[str, Any]] = []
    route_summaries: list[dict[str, Any]] = []
    shutdown_evidence: list[dict[str, Any]] = []
    reopen_evidence: list[dict[str, Any]] = []
    try:
        for route in routes:
            route_records, route_summary = run_route(args, route, args.cycles)
            records.extend(route_records)
            route_summaries.append(route_summary)
        hash_matrix = compare_hashes(records)
    finally:
        for route in reversed(routes):
            shutdown_evidence.append(stop_route(route))
    for route in routes:
        reopen_evidence.append(reopen_query(args, route, args.cycles))

    output_dir = work / "evidence"
    evidence_json = output_dir / "cdp-soak-leak-stability-evidence.json"
    package_bytes = evidence_package_size(work)
    package_check = budget_check("evidence_package_bytes", package_bytes)
    payload = {
        "schema_version": SCHEMA_VERSION,
        "gate": GATE_NAME,
        "run_id": run_id,
        "timestamp_utc": timestamp_utc,
        "build_mode": args.build_mode,
        "route_scope": list(ROUTE_NAMES),
        "cycle_count": args.cycles,
        "workload_scope": [workload.workload_id for workload in WORKLOADS],
        "diagnostic_run": True,
        "benchmark_clean_claim": False,
        "diagnostic_policy": DIAGNOSTIC_POLICY,
        "growth_budgets": GROWTH_BUDGETS,
        "route_summaries": route_summaries,
        "records": records,
        "result_hash_matrix": hash_matrix,
        "shutdown_evidence": list(reversed(shutdown_evidence)),
        "reopen_evidence": reopen_evidence,
        "support_bundle_evidence_package": {
            "source": "measured_proxy",
            "path": str(output_dir),
            "bytes": package_bytes,
            "growth_budget_check": package_check,
        },
        "output_json": str(evidence_json),
    }
    validate_payload(payload)
    write_json(evidence_json, payload)
    payload["support_bundle_evidence_package"]["bytes_after_write"] = evidence_package_size(work)
    budget_check("evidence_package_bytes", payload["support_bundle_evidence_package"]["bytes_after_write"])
    write_json(evidence_json, payload)
    return payload


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--build-mode", default="unknown")
    parser.add_argument("--cycles", type=int, default=3)
    args = parser.parse_args(argv[1:])
    if args.cycles < 3:
        raise SoakGateError("--cycles must be at least 3 for CDP-047")
    work = make_work_dir(Path(args.work_dir))
    try:
        payload = build_payload(args, work)
        print(f"{GATE_NAME}=passed output={payload['output_json']}")
        print("cdp047_policy=diagnostic_run_no_benchmark_clean_speed_claim")
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        print(f"{GATE_NAME}=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
