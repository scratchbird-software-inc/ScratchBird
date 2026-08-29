#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""True-process E2E oracle for the exact narrow catalog query profiles.

This gate deliberately uses only public SBsql process routes.  It does not
construct SBQNPB01 itself and therefore cannot claim malformed/cross-authority
carrier coverage.  The positive process route must expose the exact selected
Core profile ID after live SBQNPB01 validation and before source access; a
generic, legacy, or compatibility execution path cannot satisfy that proof.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from cdp_database_lifecycle_support import PUBLIC_TEST_PASSWORD, seed_database


ORDERED_PROFILE = "query.execute.catalog_scan.ordered_projection.v1"
PROJECTION_OCCURRENCE_PROFILE = (
    "query.execute.catalog_scan.projection_occurrence.v1"
)
SELF_JOIN_PROFILE = "query.execute.catalog_scan.alias_distinct_self_join.v1"

PROFILE_EVIDENCE_KEY = "narrow_query_profile_id"
PROFILE_EVIDENCE_RE = re.compile(
    rf"{PROFILE_EVIDENCE_KEY}=([A-Za-z0-9._-]+)"
)
FORBIDDEN_FALLBACK_TOKENS = (
    "PROFILE.BUILTIN_PROFILE_UNAVAILABLE",
    "SBLR.OPERATION.NONCANONICAL",
    "SBLR.PLAN_TREE.INVALID_HANDLE",
    "generic_query_fallback",
    "legacy_query_fallback",
    "compatibility_query_fallback",
    "legacy_query_root",
)
ROUTE_NAMES = ("embedded", "local-ipc", "inet")


class NarrowQueryE2EError(RuntimeError):
    pass


@dataclass(frozen=True)
class QueryCase:
    case_id: str
    profile_id: str
    sql: str
    expected_rows: tuple[str, ...]
    expected_row_count: int = 0
    expected_column_count: int = 0
    expected_member_values: tuple[str, ...] = ()
    require_cross_route_row_hash: bool = True


@dataclass
class ProcessResult:
    route: str
    case_id: str
    command: list[str]
    returncode: int
    stdout: str
    stderr: str
    trace_delta: str
    stdout_path: Path
    stderr_path: Path
    script_path: Path
    elapsed_ms: float


@dataclass
class Route:
    name: str
    root: Path
    database: Path
    sb_isql: str
    trace_path: Path
    fixed_args: list[str] = field(default_factory=list)
    embedded: bool = False
    processes: list[subprocess.Popen[bytes]] = field(default_factory=list)

    def cli_args(self) -> list[str]:
        if self.embedded:
            return [
                self.sb_isql,
                str(self.database),
                "--mode=embedded",
                "--sslmode=disable",
                "-U",
                "alice",
                "-P",
                PUBLIC_TEST_PASSWORD,
            ]
        return list(self.fixed_args)


QUERY_CASES: tuple[QueryCase, ...] = (
    QueryCase(
        case_id="ordered_first_key_ties_and_nulls",
        profile_id=ORDERED_PROFILE,
        sql=(
            "SELECT payload FROM qnp_ordered "
            "ORDER BY k1 ASC NULLS LAST, k2 DESC NULLS FIRST;"
        ),
        expected_rows=(
            "k1-one-null-second",
            "tie-first",
            "tie-second",
            "k1-two",
            "null-both",
            "null-first-key-value",
        ),
    ),
    QueryCase(
        case_id="projection_occurrence_semicolon_equals",
        profile_id=PROJECTION_OCCURRENCE_PROFILE,
        sql=(
            "SELECT payload, payload "
            "FROM qnp_projection_semicolon_equals;"
        ),
        expected_rows=("semi;equals=payload|semi;equals=payload",),
    ),
    QueryCase(
        case_id="projection_occurrence_empty",
        profile_id=PROJECTION_OCCURRENCE_PROFILE,
        sql="SELECT payload, payload FROM qnp_projection_empty;",
        expected_rows=("|",),
    ),
    QueryCase(
        case_id="projection_occurrence_null",
        profile_id=PROJECTION_OCCURRENCE_PROFILE,
        sql="SELECT payload, payload FROM qnp_projection_null;",
        expected_rows=("(null)|(null)",),
    ),
    QueryCase(
        case_id="projection_occurrence_equals",
        profile_id=PROJECTION_OCCURRENCE_PROFILE,
        sql="SELECT payload, payload FROM qnp_projection_equals;",
        expected_rows=("=left=right=|=left=right=",),
    ),
    QueryCase(
        case_id="alias_distinct_three_way_self_join",
        profile_id=SELF_JOIN_PROFILE,
        sql=(
            "SELECT a.id, b.id, c.id FROM qnp_self AS a "
            "CROSS JOIN qnp_self AS b CROSS JOIN qnp_self AS c;"
        ),
        expected_rows=(
            "1|1|1",
            "1|1|2",
            "1|2|1",
            "1|2|2",
            "2|1|1",
            "2|1|2",
            "2|2|1",
            "2|2|2",
        ),
    ),
    QueryCase(
        case_id="alias_distinct_nine_way_limit_one_boundary",
        profile_id=SELF_JOIN_PROFILE,
        sql=(
            "SELECT a.id, b.id, c.id, d.id, e.id, f.id, g.id, h.id, i.id "
            "FROM qnp_self AS a CROSS JOIN qnp_self AS b "
            "CROSS JOIN qnp_self AS c CROSS JOIN qnp_self AS d "
            "CROSS JOIN qnp_self AS e CROSS JOIN qnp_self AS f "
            "CROSS JOIN qnp_self AS g CROSS JOIN qnp_self AS h "
            "CROSS JOIN qnp_self AS i LIMIT 1;"
        ),
        expected_rows=(),
        expected_row_count=1,
        expected_column_count=9,
        expected_member_values=("1", "2"),
        require_cross_route_row_hash=False,
    ),
)


SETUP_SQL = """
CREATE TABLE qnp_ordered (
    row_id int,
    k1 int,
    k2 int,
    payload text
);
INSERT INTO qnp_ordered (row_id, k1, k2, payload) VALUES
    (1, 1, 10, 'tie-first'),
    (2, 1, 9, 'tie-second'),
    (3, 1, NULL, 'k1-one-null-second'),
    (4, 2, 20, 'k1-two'),
    (5, NULL, 30, 'null-first-key-value'),
    (6, NULL, NULL, 'null-both');

CREATE TABLE qnp_projection_semicolon_equals (row_id int, payload text);
INSERT INTO qnp_projection_semicolon_equals (row_id, payload) VALUES
    (1, 'semi;equals=payload');

CREATE TABLE qnp_projection_empty (row_id int, payload text);
INSERT INTO qnp_projection_empty (row_id, payload) VALUES (1, '');

CREATE TABLE qnp_projection_null (row_id int, payload text);
INSERT INTO qnp_projection_null (row_id, payload) VALUES (1, NULL);

CREATE TABLE qnp_projection_equals (row_id int, payload text);
INSERT INTO qnp_projection_equals (row_id, payload) VALUES
    (1, '=left=right=');

CREATE TABLE qnp_self (id int);
INSERT INTO qnp_self (id) VALUES (1), (2);
COMMIT;
""".strip()


POST_STATE_SQL = """
SELECT COUNT(*) FROM qnp_ordered;
SELECT COUNT(*) FROM qnp_projection_semicolon_equals;
SELECT COUNT(*) FROM qnp_projection_empty;
SELECT COUNT(*) FROM qnp_projection_null;
SELECT COUNT(*) FROM qnp_projection_equals;
SELECT COUNT(*) FROM qnp_self;
""".strip()

POST_STATE_ROWS = ("6", "1", "1", "1", "1", "2")


def make_work_dir(preferred_root: Path) -> Path:
    roots = (
        preferred_root,
        Path(tempfile.gettempdir()) / "nqp",
    )
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="nqp_", dir=root))
        probes = (
            candidate / "i" / "sc" / "s.sock",
            candidate
            / "n"
            / "lc"
            / ("sbsql_" + ("0" * 32) + ".management.sock"),
            candidate / "e" / "nqp.sbdb",
        )
        if max(len(str(path)) for path in probes) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise NarrowQueryE2EError(
        "unable to allocate a short-enough narrow-query process workspace"
    )


def wait_for_path(path: Path, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise NarrowQueryE2EError(f"timed out waiting for {path}")


def wait_for_tcp(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise NarrowQueryE2EError(
        f"timed out waiting for listener port {port}: {last_error}"
    )


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def process_environment(trace_path: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE"] = str(trace_path)
    env["SCRATCHBIRD_SBSQL_WORKER_PHASE_TRACE_FILE"] = str(trace_path)
    return env


def start_process(
    command: list[str], root: Path, label: str, trace_path: Path
) -> subprocess.Popen[bytes]:
    root.mkdir(parents=True, exist_ok=True)
    return subprocess.Popen(
        command,
        stdout=(root / f"{label}.out").open("wb"),
        stderr=(root / f"{label}.err").open("wb"),
        env=process_environment(trace_path),
    )


def start_embedded(args: argparse.Namespace, work: Path) -> Route:
    root = work / "e"
    database = root / "nqp.sbdb"
    trace_path = root / "route.phase.trace"
    seed_database(
        database_seed=args.database_seed,
        resource_seed_pack_root=args.resource_seed_pack_root,
        database=database,
        evidence_root=root / "bootstrap",
        fixture_label="narrow-query-embedded",
    )
    return Route(
        name="embedded",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        trace_path=trace_path,
        embedded=True,
    )


def start_local_ipc(args: argparse.Namespace, work: Path) -> Route:
    require_route_tools(args, "local-ipc")
    root = work / "i"
    database = root / "nqp.sbdb"
    control = root / "sc"
    runtime = root / "sr"
    endpoint = control / "s.sock"
    trace_path = root / "route.phase.trace"
    seed_database(
        database_seed=args.database_seed,
        resource_seed_pack_root=args.resource_seed_pack_root,
        database=database,
        evidence_root=root / "bootstrap",
        fixture_label="narrow-query-local-ipc",
    )
    server = start_process(
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
        root,
        "server",
        trace_path,
    )
    wait_for_path(endpoint)
    return Route(
        name="local-ipc",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        trace_path=trace_path,
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
            PUBLIC_TEST_PASSWORD,
        ],
        processes=[server],
    )


def start_inet(args: argparse.Namespace, work: Path) -> Route:
    require_route_tools(args, "inet")
    root = work / "n"
    database = root / "nqp.sbdb"
    server_control = root / "sc"
    server_runtime = root / "sr"
    listener_control = root / "lc"
    listener_runtime = root / "lr"
    endpoint = server_control / "s.sock"
    trace_path = root / "route.phase.trace"
    port = find_free_port()
    seed_database(
        database_seed=args.database_seed,
        resource_seed_pack_root=args.resource_seed_pack_root,
        database=database,
        evidence_root=root / "bootstrap",
        fixture_label="narrow-query-inet",
    )
    server = start_process(
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
        root,
        "server",
        trace_path,
    )
    wait_for_path(endpoint)
    listener = start_process(
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
        root,
        "listener",
        trace_path,
    )
    wait_for_tcp(port)
    return Route(
        name="inet",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        trace_path=trace_path,
        fixed_args=[
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
        processes=[server, listener],
    )


def require_route_tools(args: argparse.Namespace, route: str) -> None:
    missing = [
        name
        for name in ("server", "listener", "parser_worker")
        if not getattr(args, name)
    ]
    if route == "local-ipc":
        missing = [name for name in missing if name == "server"]
    if missing:
        raise NarrowQueryE2EError(
            f"{route} selected but required process tools are absent: "
            + ",".join(missing)
        )


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def stop_route(route: Route) -> None:
    for process in reversed(route.processes):
        stop_process(process)


def read_trace_delta(path: Path, offset: int) -> str:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size > offset:
            break
        time.sleep(0.02)
    if not path.exists():
        return ""
    with path.open("rb") as source:
        source.seek(offset)
        return source.read().decode("utf-8", errors="replace")


def run_process_sql(
    route: Route,
    case_id: str,
    sql: str,
    *,
    capture_profile_trace: bool,
) -> ProcessResult:
    case_dir = route.root / "cases" / case_id
    case_dir.mkdir(parents=True, exist_ok=True)
    script_path = case_dir / "script.sql"
    stdout_path = case_dir / "sb_isql.out"
    stderr_path = case_dir / "sb_isql.err"
    script_path.write_text(sql.rstrip() + "\n", encoding="utf-8")
    trace_offset = (
        route.trace_path.stat().st_size if route.trace_path.exists() else 0
    )
    command = route.cli_args() + [
        "-q",
        "-A",
        "-t",
        "-b",
        "-F",
        "|",
        "-f",
        str(script_path),
    ]
    started = time.monotonic()
    with stdout_path.open("wb") as stdout_file, stderr_path.open(
        "wb"
    ) as stderr_file:
        completed = subprocess.run(
            command,
            stdout=stdout_file,
            stderr=stderr_file,
            check=False,
            timeout=90,
            env=process_environment(route.trace_path),
        )
    elapsed_ms = (time.monotonic() - started) * 1000.0
    trace_delta = (
        read_trace_delta(route.trace_path, trace_offset)
        if capture_profile_trace
        else ""
    )
    return ProcessResult(
        route=route.name,
        case_id=case_id,
        command=command,
        returncode=completed.returncode,
        stdout=stdout_path.read_text(encoding="utf-8", errors="replace"),
        stderr=stderr_path.read_text(encoding="utf-8", errors="replace"),
        trace_delta=trace_delta,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        script_path=script_path,
        elapsed_ms=elapsed_ms,
    )


def semantic_rows(stdout: str) -> tuple[str, ...]:
    rows: list[str] = []
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("Rows affected:"):
            continue
        if line.startswith("Transaction committed"):
            continue
        if line.startswith("Replacement transaction"):
            continue
        rows.append(line)
    return tuple(rows)


def require_process_success(result: ProcessResult) -> None:
    if result.returncode != 0:
        raise NarrowQueryE2EError(
            f"{result.route}:{result.case_id} failed rc={result.returncode} "
            f"stdout={result.stdout!r} stderr={result.stderr!r} "
            f"trace={result.trace_delta!r}"
        )
    combined = "\n".join(
        (result.stdout, result.stderr, result.trace_delta)
    )
    forbidden = [
        token for token in FORBIDDEN_FALLBACK_TOKENS if token in combined
    ]
    if forbidden:
        raise NarrowQueryE2EError(
            f"{result.route}:{result.case_id} used/refused through a "
            f"forbidden path: {forbidden}"
        )


def require_profile_evidence(result: ProcessResult, profile_id: str) -> None:
    observed = PROFILE_EVIDENCE_RE.findall(result.trace_delta)
    if not observed:
        raise NarrowQueryE2EError(
            f"{result.route}:{result.case_id} returned rows without exact "
            f"process-visible {PROFILE_EVIDENCE_KEY} evidence"
        )
    if set(observed) != {profile_id}:
        raise NarrowQueryE2EError(
            f"{result.route}:{result.case_id} profile evidence mismatch: "
            f"expected={profile_id!r} observed={observed!r}"
        )


def sha256_rows(rows: tuple[str, ...]) -> str:
    material = b"".join(
        len(row.encode("utf-8")).to_bytes(8, "little")
        + row.encode("utf-8")
        for row in rows
    )
    return hashlib.sha256(material).hexdigest()


def setup_route(route: Route) -> None:
    result = run_process_sql(
        route,
        "setup",
        SETUP_SQL,
        capture_profile_trace=False,
    )
    require_process_success(result)


def run_query_case(route: Route, case: QueryCase) -> dict[str, Any]:
    result = run_process_sql(
        route,
        case.case_id,
        case.sql,
        capture_profile_trace=True,
    )
    require_process_success(result)
    actual_rows = semantic_rows(result.stdout)
    membership_oracle = bool(case.expected_member_values)
    if membership_oracle:
        member_values = set(case.expected_member_values)
        columns = actual_rows[0].split("|") if len(actual_rows) == 1 else []
        rows_valid = (
            len(actual_rows) == case.expected_row_count
            and len(columns) == case.expected_column_count
            and all(value in member_values for value in columns)
        )
    else:
        rows_valid = actual_rows == case.expected_rows
    if not rows_valid:
        expected = (
            {
                "row_count": case.expected_row_count,
                "column_count": case.expected_column_count,
                "member_values": list(case.expected_member_values),
            }
            if membership_oracle
            else case.expected_rows
        )
        raise NarrowQueryE2EError(
            f"{route.name}:{case.case_id} exact row/order mismatch: "
            f"expected={expected!r} actual={actual_rows!r}"
        )
    require_profile_evidence(result, case.profile_id)
    return {
        "route": route.name,
        "case_id": case.case_id,
        "profile_id": case.profile_id,
        "expected_rows": list(case.expected_rows),
        "expected_membership_oracle": (
            {
                "row_count": case.expected_row_count,
                "column_count": case.expected_column_count,
                "member_values": list(case.expected_member_values),
            }
            if membership_oracle
            else None
        ),
        "actual_rows": list(actual_rows),
        "row_sequence_sha256": sha256_rows(actual_rows),
        "profile_evidence": f"{PROFILE_EVIDENCE_KEY}={case.profile_id}",
        "elapsed_ms": round(result.elapsed_ms, 3),
        "stdout_path": str(result.stdout_path),
        "stderr_path": str(result.stderr_path),
        "script_path": str(result.script_path),
    }


def verify_independent_post_state(route: Route) -> dict[str, Any]:
    result = run_process_sql(
        route,
        "independent_post_state",
        POST_STATE_SQL,
        capture_profile_trace=False,
    )
    require_process_success(result)
    actual_rows = semantic_rows(result.stdout)
    if actual_rows != POST_STATE_ROWS:
        raise NarrowQueryE2EError(
            f"{route.name}:independent post-state mismatch: "
            f"expected={POST_STATE_ROWS!r} actual={actual_rows!r}"
        )
    return {
        "route": route.name,
        "independent_process": True,
        "expected_relation_counts": list(POST_STATE_ROWS),
        "actual_relation_counts": list(actual_rows),
        "row_sequence_sha256": sha256_rows(actual_rows),
        "stdout_path": str(result.stdout_path),
        "stderr_path": str(result.stderr_path),
        "script_path": str(result.script_path),
    }


def compare_route_oracles(records: list[dict[str, Any]]) -> None:
    for case in QUERY_CASES:
        matching = [row for row in records if row["case_id"] == case.case_id]
        expected_routes = {row["route"] for row in records}
        actual_routes = {row["route"] for row in matching}
        if actual_routes != expected_routes:
            raise NarrowQueryE2EError(
                f"{case.case_id} route coverage mismatch: "
                f"expected={sorted(expected_routes)} actual={sorted(actual_routes)}"
            )
        if not case.require_cross_route_row_hash:
            continue
        hashes = {row["row_sequence_sha256"] for row in matching}
        if len(hashes) != 1:
            raise NarrowQueryE2EError(
                f"{case.case_id} exact row/order hash differs by route: "
                f"{sorted(hashes)}"
            )


def parse_routes(value: str) -> tuple[str, ...]:
    routes = tuple(item.strip() for item in value.split(",") if item.strip())
    if not routes:
        raise NarrowQueryE2EError("at least one process route is required")
    unknown = sorted(set(routes) - set(ROUTE_NAMES))
    if unknown:
        raise NarrowQueryE2EError(f"unknown routes: {unknown}")
    if len(set(routes)) != len(routes):
        raise NarrowQueryE2EError("duplicate route selection is not allowed")
    return routes


def start_route(args: argparse.Namespace, work: Path, name: str) -> Route:
    if name == "embedded":
        return start_embedded(args, work)
    if name == "local-ipc":
        return start_local_ipc(args, work)
    if name == "inet":
        return start_inet(args, work)
    raise NarrowQueryE2EError(f"unknown route {name}")


def dump_logs(work: Path) -> None:
    paths = sorted(work.rglob("*.out")) + sorted(work.rglob("*.err"))
    paths += sorted(work.rglob("*.trace"))
    for path in paths:
        if not path.exists() or path.stat().st_size == 0:
            continue
        print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
        print(
            path.read_text(encoding="utf-8", errors="replace")[-16000:],
            file=sys.stderr,
        )


def run_gate(args: argparse.Namespace, work: Path) -> Path:
    selected_routes = parse_routes(args.routes)
    route_records: list[dict[str, Any]] = []
    post_state_records: list[dict[str, Any]] = []
    completed_routes: list[str] = []
    for route_name in selected_routes:
        route = start_route(args, work, route_name)
        try:
            setup_route(route)
            for case in QUERY_CASES:
                route_records.append(run_query_case(route, case))
            post_state_records.append(verify_independent_post_state(route))
            completed_routes.append(route.name)
        finally:
            stop_route(route)
    compare_route_oracles(route_records)
    evidence = {
        "schema_version": "sbsql.narrow_query_profiles.process_e2e.v1",
        "gate": "sbsql_narrow_query_profiles_process_e2e_gate",
        "status": "passed",
        "routes": completed_routes,
        "profile_contracts": [
            {
                "profile_id": ORDERED_PROFILE,
                "sblr_profile_id": "SBLR-QUERY-ORDERED-PROJECTION-V1",
                "profile_code": 1,
            },
            {
                "profile_id": PROJECTION_OCCURRENCE_PROFILE,
                "sblr_profile_id": "SBLR-QUERY-PROJECTION-OCCURRENCE-V1",
                "profile_code": 2,
            },
            {
                "profile_id": SELF_JOIN_PROFILE,
                "sblr_profile_id": "SBLR-QUERY-ALIAS-DISTINCT-SELF-JOIN-V1",
                "profile_code": 3,
            },
        ],
        "query_records": route_records,
        "independent_post_state": post_state_records,
        "fallback_policy": "forbidden_and_exact_profile_evidence_required",
        "carrier_negative_scope": (
            "not_exercised_process_harness_exposes_only_sbsql_text_and_cannot_"
            "submit_exact_SBQNPB01"
        ),
    }
    evidence_path = work / "sbsql_narrow_query_profiles_process_e2e.json"
    evidence_path.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return evidence_path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--database-seed", required=True)
    parser.add_argument("--resource-seed-pack-root", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--server", default="")
    parser.add_argument("--listener", default="")
    parser.add_argument("--parser-worker", default="")
    parser.add_argument(
        "--routes",
        default=",".join(ROUTE_NAMES),
        help="comma-separated subset of embedded,local-ipc,inet",
    )
    args = parser.parse_args(argv[1:])
    work = make_work_dir(Path(args.work_dir))
    try:
        evidence = run_gate(args, work)
        print(
            "sbsql_narrow_query_profiles_process_e2e_gate=passed "
            f"evidence={evidence}"
        )
        return 0
    except Exception as exc:
        print(
            "sbsql_narrow_query_profiles_process_e2e_gate=failed "
            f"work={work} error={exc}",
            file=sys.stderr,
        )
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
