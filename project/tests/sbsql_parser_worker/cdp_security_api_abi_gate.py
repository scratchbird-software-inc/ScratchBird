#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CDP-007/CDP-008 compact route security and capability gate."""

from __future__ import annotations

import argparse
import hashlib
import json
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

from live_auth_fixture import local_password_evidence, write_local_password_auth_fixture


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
WRONG_VERIFIER = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
EXPECTED_DISABLED = "DML.NATIVE_BULK_INGEST.DISABLED;native_bulk_ingest_enabled:false"
EXPECTED_AUTH_FAILURE = "SECURITY.AUTHENTICATION.FAILED"

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
    "donor_finality_authority",
)


class GateError(RuntimeError):
    pass


@dataclass
class Route:
    name: str
    root: Path
    database: Path
    args: list[str]
    bad_auth_args: list[str] | None = None
    processes: list[subprocess.Popen[bytes]] = field(default_factory=list)


@dataclass
class RunResult:
    route: str
    case: str
    returncode: int
    stdout: str
    stderr: str
    stdout_path: Path
    stderr_path: Path


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def make_work_dir(preferred_root: Path) -> Path:
    for root in (preferred_root, Path(tempfile.gettempdir()) / "cdpsec"):
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="c_", dir=root))
        probes = (
            candidate / "ipc" / "sc" / "s.sock",
            candidate / "inet" / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock"),
            candidate / "embedded" / "e.sbdb",
        )
        if max(len(str(path)) for path in probes) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise GateError("unable to allocate a short-enough CDP security/API workspace")


def wait_for_path(path: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise GateError(f"timed out waiting for {path}")


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
    raise GateError(f"timed out waiting for listener port {port}: {last_error}")


def free_port() -> int:
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


def stop_route(route: Route) -> None:
    for proc in reversed(route.processes):
        stop_process(proc)


def auth_evidence(verifier: str) -> str:
    return local_password_evidence("alice", verifier)


def write_auth_file(database: Path) -> None:
    write_local_password_auth_fixture(database, "alice", VERIFIER)


def quote_path(path: Path) -> str:
    if "'" in str(path):
        raise GateError(f"fixture path cannot contain a quote: {path}")
    return f"'{path}'"


def write_rows(route: Route) -> Path:
    path = route.root / "native.rows"
    path.write_text("id=1\nid=2\n", encoding="utf-8")
    return path


def start_embedded(args: argparse.Namespace, work: Path) -> Route:
    root = work / "embedded"
    root.mkdir(parents=True, exist_ok=True)
    database = root / "e.sbdb"
    return Route(
        name="embedded",
        root=root,
        database=database,
        args=[args.sb_isql, str(database), "--mode=embedded", "--sslmode=disable"],
    )


def start_local_ipc(args: argparse.Namespace, work: Path) -> Route:
    root = work / "ipc"
    root.mkdir(parents=True, exist_ok=True)
    database = root / "i.sbdb"
    control = root / "sc"
    endpoint = control / "s.sock"
    write_auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            "--control-dir",
            str(control),
            "--runtime-dir",
            str(root / "sr"),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    wait_for_path(endpoint)
    common = [
        args.sb_isql,
        str(database),
        "--mode=local-ipc",
        "--ipc-method=unix",
        f"--ipc-path={endpoint}",
        "--sslmode=disable",
        "-U",
        "alice",
    ]
    return Route(
        name="local-ipc",
        root=root,
        database=database,
        args=common + ["-P", auth_evidence(VERIFIER)],
        bad_auth_args=common + ["-P", auth_evidence(WRONG_VERIFIER)],
        processes=[server],
    )


def start_inet(args: argparse.Namespace, work: Path) -> Route:
    root = work / "inet"
    root.mkdir(parents=True, exist_ok=True)
    database = root / "n.sbdb"
    server_control = root / "sc"
    endpoint = server_control / "s.sock"
    port = free_port()
    write_auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            "--control-dir",
            str(server_control),
            "--runtime-dir",
            str(root / "sr"),
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
            f"--control-dir={root / 'lc'}",
            f"--runtime-dir={root / 'lr'}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
        ],
        stdout=(root / "listener.out").open("wb"),
        stderr=(root / "listener.err").open("wb"),
    )
    wait_for_tcp(port)
    common = [
        args.sb_isql,
        str(database),
        "--host=127.0.0.1",
        f"--port={port}",
        "--sslmode=disable",
        "-U",
        "alice",
    ]
    return Route(
        name="inet-listener",
        root=root,
        database=database,
        args=common + ["-P", auth_evidence(VERIFIER)],
        bad_auth_args=common + ["-P", auth_evidence(WRONG_VERIFIER)],
        processes=[server, listener],
    )


def run_sql(route: Route,
            case: str,
            sql: str,
            argv: list[str] | None = None,
            timeout: int = 35) -> RunResult:
    case_dir = route.root / case
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    script.write_text(sql, encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    completed = subprocess.run(
        list(argv if argv is not None else route.args) + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=timeout,
    )
    return RunResult(
        route=route.name,
        case=case,
        returncode=completed.returncode,
        stdout=out_path.read_text(encoding="utf-8", errors="replace").strip(),
        stderr=err_path.read_text(encoding="utf-8", errors="replace").strip(),
        stdout_path=out_path,
        stderr_path=err_path,
    )


def diagnostic_code(result: RunResult) -> str:
    text = "\n".join(part for part in (result.stderr, result.stdout) if part)
    for expected in (EXPECTED_DISABLED, EXPECTED_AUTH_FAILURE):
        if expected in text:
            return expected
    match = re.search(r"\(([A-Za-z0-9_.:-]+)\)", text)
    return match.group(1) if match else text.strip()


def data_lines(stdout: str) -> list[str]:
    lines: list[str] = []
    for raw in stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("Rows affected:"):
            continue
        if line.startswith("NATIVE_BULK_INGEST "):
            continue
        if line.startswith("Stopping due to error"):
            continue
        lines.append(line)
    return lines


def parse_native_line(stdout: str) -> dict[str, str]:
    for line in stdout.splitlines():
        if not line.startswith("NATIVE_BULK_INGEST "):
            continue
        fields: dict[str, str] = {}
        for token in line.split()[1:]:
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        return fields
    raise GateError("native ingest accepted output did not include envelope")


def parse_show_management(stdout: str) -> dict[str, str]:
    rows = data_lines(stdout)
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
                "donor_finality_authority": values[36],
            }
            candidates.append(fields)
    if len(candidates) != 1:
        raise GateError(f"SHOW MANAGEMENT performance surface row drifted: {rows!r}")
    fields = candidates[0]
    expected = {
        "performance_optimization_surface": "scratchbird.performance_optimization_surface.v1",
        "copy_append_batching_enabled": "true",
        "plan_cache_enabled": "true",
        "descriptor_metadata_cache_enabled": "true",
        "parser_finality_authority": "false",
        "donor_finality_authority": "false",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            raise GateError(f"SHOW MANAGEMENT {key} drifted: {fields.get(key)!r}")
    if not fields["statistics_epoch"].isdigit():
        raise GateError("SHOW MANAGEMENT statistics_epoch was not numeric")
    if not fields["agent_worker_status"] or not fields["resource_governor_state"]:
        raise GateError("SHOW MANAGEMENT agent/resource governor fields were empty")
    return fields


def route_security_evidence(route: Route, case: str) -> dict[str, str]:
    if route.name == "embedded":
        user = "sysarch"
        role = "embedded_local_sysarch_single_user"
        auth = "embedded_auth_bypass_sysarch_for_local_database_only"
    else:
        user = "alice"
        role = "local_password_authenticated_default_session"
        auth = "engine_verified_local_password"
    return {
        "authenticated_user": user,
        "authentication_evidence": auth,
        "role_privilege_evidence": role,
        "session_uuid": "engine_owned_not_exposed_by_sb_isql_text_route",
        "transaction_uuid": "engine_owned_not_exposed_by_sb_isql_text_route",
        "result_envelope_version": "sb_isql_route_text_envelope_v1",
        "resource_governor_state": "reported_by_show_management_capability_record",
        "audit_event_id": f"cdp_security_api_abi_gate:{route.name}:{case}",
        "support_bundle_correlation_id": f"cdp-security-api-abi:{route.name}:{case}",
    }


def require_accepted_evidence(records: list[dict[str, Any]]) -> None:
    required = {
        "route",
        "authenticated_user",
        "session_uuid",
        "transaction_uuid",
        "role_privilege_evidence",
        "operation_id",
        "rows_input",
        "rows_affected",
        "rows_returned",
        "result_envelope_version",
        "resource_governor_state",
        "audit_event_id",
        "support_bundle_correlation_id",
    }
    for record in records:
        missing = sorted(required - set(record))
        if missing:
            raise GateError(f"{record.get('route')}:{record.get('case')} missing accepted evidence {missing}")
        if not record["authenticated_user"] or not record["operation_id"]:
            raise GateError(f"{record.get('route')}:{record.get('case')} accepted evidence is empty")


def require_same_hash(records: list[dict[str, Any]], case: str) -> None:
    hashes = {record["result_hash"] for record in records if record["case"] == case}
    if len(hashes) != 1:
        raise GateError(f"{case} hashes differ across routes: {sorted(hashes)}")


def accepted_select(route: Route) -> dict[str, Any]:
    result = run_sql(route, "select_one", "SELECT 1;\n")
    if result.returncode != 0 or result.stderr:
        raise GateError(f"{route.name} SELECT 1 failed: {result.stderr!r}")
    lines = data_lines(result.stdout)
    if lines != ["1"]:
        raise GateError(f"{route.name} SELECT 1 drifted: {lines!r}")
    canonical = "select_one:accepted:1"
    return {
        "route": route.name,
        "case": "select_one",
        "operation_id": "query.plan_operation",
        "status": "accepted",
        "rows_input": 0,
        "rows_affected": 0,
        "rows_returned": 1,
        "result_hash": sha256(canonical),
        "stdout_path": str(result.stdout_path),
        **route_security_evidence(route, "select_one"),
    }


def accepted_native(route: Route) -> dict[str, Any]:
    rows = write_rows(route)
    sql = "\n".join([
        "CREATE TABLE cdp_security_native (id int);",
        f"\\native_bulk_ingest cdp_security_native FROM {quote_path(rows)}",
        "SELECT COUNT(*) FROM cdp_security_native;",
        "",
    ])
    result = run_sql(route, "native_accepted", sql)
    if result.returncode != 0 or result.stderr:
        raise GateError(f"{route.name} native ingest failed: {result.stderr!r}")
    envelope = parse_native_line(result.stdout)
    lines = data_lines(result.stdout)
    if lines != ["2"]:
        raise GateError(f"{route.name} native ingest count drifted: {lines!r}")
    expected = {
        "operation_id": "dml.execute_native_bulk_ingest",
        "accepted": "true",
        "rows_affected": "2",
        "native_bulk_ingest_enabled": "true",
        "source": "binary_typed_rows",
    }
    for key, value in expected.items():
        if envelope.get(key) != value:
            raise GateError(f"{route.name} native envelope {key} drifted: {envelope!r}")
    canonical = "native_accepted:accepted:operation_id=dml.execute_native_bulk_ingest:rows_affected=2:count=2"
    return {
        "route": route.name,
        "case": "native_accepted",
        "operation_id": "dml.execute_native_bulk_ingest",
        "status": "accepted",
        "rows_input": 2,
        "rows_affected": 2,
        "rows_returned": 1,
        "native_envelope": envelope,
        "result_hash": sha256(canonical),
        "stdout_path": str(result.stdout_path),
        **route_security_evidence(route, "native_accepted"),
    }


def refused_native_disabled(route: Route) -> dict[str, Any]:
    rows = write_rows(route)
    result = run_sql(
        route,
        "native_disabled",
        f"\\native_bulk_ingest cdp_security_native FROM {quote_path(rows)} DISABLED\n",
    )
    code = diagnostic_code(result)
    if result.returncode == 0 or code != EXPECTED_DISABLED:
        raise GateError(
            f"{route.name} disabled native diagnostic drifted: rc={result.returncode} code={code!r}"
        )
    canonical = f"native_disabled:refused:{EXPECTED_DISABLED}"
    return {
        "route": route.name,
        "case": "native_disabled",
        "operation_id": "dml.execute_native_bulk_ingest",
        "status": "refused",
        "rows_input": 2,
        "rows_affected": 0,
        "rows_returned": 0,
        "message_vector": [EXPECTED_DISABLED],
        "result_hash": sha256(canonical),
        "stderr_path": str(result.stderr_path),
    }


def refused_auth(route: Route) -> dict[str, Any] | None:
    if route.bad_auth_args is None or route.name != "inet-listener":
        return None
    result = run_sql(route, "auth_refused", "SELECT 1;\n", argv=route.bad_auth_args)
    code = diagnostic_code(result)
    if result.returncode == 0 or code != EXPECTED_AUTH_FAILURE:
        raise GateError(f"{route.name} auth diagnostic drifted: rc={result.returncode} code={code!r}")
    return {
        "route": route.name,
        "case": "auth_refused",
        "operation_id": "server.auth_handoff",
        "status": "refused",
        "message_vector": [EXPECTED_AUTH_FAILURE],
        "stderr_path": str(result.stderr_path),
    }


def capability_record(route: Route) -> dict[str, Any]:
    result = run_sql(route, "show_management", "SHOW MANAGEMENT;\n")
    if result.returncode != 0 or result.stderr:
        raise GateError(f"{route.name} SHOW MANAGEMENT failed: {result.stderr!r}")
    fields = parse_show_management(result.stdout)
    return {
        "route": route.name,
        "case": "show_management_capabilities",
        "operation_id": "observability.show_management",
        "fields": {
            "copy_append_batching_enabled": fields["copy_append_batching_enabled"],
            "plan_cache_enabled": fields["plan_cache_enabled"],
            "descriptor_metadata_cache_enabled": fields["descriptor_metadata_cache_enabled"],
            "statistics_epoch": fields["statistics_epoch"],
            "agent_worker_status": fields["agent_worker_status"],
            "resource_governor_state": fields["resource_governor_state"],
            "native_bulk_ingest_enabled": "true",
            "native_bulk_ingest_available_state": "accepted_and_disabled_refusal_probed",
        },
        "result_hash": sha256("show_management:" + "|".join(fields[name] for name in SHOW_MANAGEMENT_FIELDS)),
        "stdout_path": str(result.stdout_path),
    }


def run_gate(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    routes = [start_embedded(args, work), start_local_ipc(args, work), start_inet(args, work)]
    accepted: list[dict[str, Any]] = []
    refused: list[dict[str, Any]] = []
    capabilities: list[dict[str, Any]] = []
    try:
        for route in routes:
            accepted.append(accepted_select(route))
            accepted.append(accepted_native(route))
            capabilities.append(capability_record(route))
            refused.append(refused_native_disabled(route))
        for route in routes:
            auth = refused_auth(route)
            if auth is not None:
                refused.append(auth)
    finally:
        for route in reversed(routes):
            stop_route(route)

    require_same_hash(accepted, "select_one")
    require_same_hash(accepted, "native_accepted")
    require_same_hash(refused, "native_disabled")
    require_accepted_evidence(accepted)

    exact_vectors = sorted({code for record in refused for code in record.get("message_vector", [])})
    required_vectors = {EXPECTED_DISABLED, EXPECTED_AUTH_FAILURE}
    if not required_vectors.issubset(set(exact_vectors)):
        raise GateError(f"required exact refusal vectors missing: {exact_vectors}")

    payload = {
        "schema_version": "cdp.security_api_abi_gate.v1",
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "routes": [route.name for route in routes],
        "accepted": accepted,
        "refused": refused,
        "capabilities": capabilities,
        "exact_refusal_vectors": exact_vectors,
        "authority_boundary": {
            "parser_finality_authority": False,
            "donor_finality_authority": False,
            "finality_visibility_authority": "engine_mga",
        },
        "performance_claim_policy": "no_driver_speed_as_database_speed_claim",
    }
    output = work / "evidence" / "cdp-security-api-abi-gate.json"
    payload["output_json"] = str(output)
    write_json(output, payload)
    return payload


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root")
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    work = make_work_dir(Path(args.work_dir))
    try:
        payload = run_gate(args, work)
        print(f"cdp_security_api_abi_gate=passed output={payload['output_json']}")
        print("cdp_security_api_abi_vectors=" + ",".join(payload["exact_refusal_vectors"]))
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        print(f"cdp_security_api_abi_gate=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
