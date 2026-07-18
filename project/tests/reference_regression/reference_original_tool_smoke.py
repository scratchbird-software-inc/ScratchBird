#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run a small original-tool smoke against a ScratchBird reference listener."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
PRINCIPAL_UUID = "019f0a11-ce00-7000-8000-000000000001"
MYSQL_WIRE_FAMILIES = {"dolt", "mariadb", "mysql", "mysql_lts", "tidb", "vitess"}
POSTGRESQL_WIRE_FAMILIES = {"cockroachdb", "postgresql", "xtdb", "yugabytedb"}
HTTP_REST_FAMILIES = {"opensearch", "opensearch_sql_ppl"}
APACHE_IGNITE_JDBC_FAMILIES = {"apache_ignite"}
INFLUXDB_HTTP_FAMILIES = {"influxdb"}
MONGODB_WIRE_FAMILIES = {"mongodb"}
REDIS_WIRE_FAMILIES = {"redis"}
CASSANDRA_WIRE_FAMILIES = {"cassandra"}
CLICKHOUSE_WIRE_FAMILIES = {"clickhouse"}
NEO4J_WIRE_FAMILIES = {"neo4j"}
MILVUS_GRPC_FAMILIES = {"milvus"}
IMMUDB_GRPC_FAMILIES = {"immudb"}
TIKV_GRPC_FAMILIES = {"tikv"}
FOUNDATIONDB_FLOW_FAMILIES = {"foundationdb"}
EMBEDDED_SHELL_FAMILIES = {"duckdb", "sqlite"}


class OriginalToolSmokeError(RuntimeError):
    pass


class OriginalToolStartupError(OriginalToolSmokeError):
    def __init__(
        self,
        component: str,
        reason: str,
        *,
        returncode: int | None,
        stdout_tail: str,
        stderr_tail: str,
    ) -> None:
        self.component = component
        self.reason = reason
        self.returncode = returncode
        self.stdout_tail = stdout_tail
        self.stderr_tail = stderr_tail
        super().__init__(
            f"{component} startup failed: {reason}; rc={returncode}; "
            f"stdout_tail={stdout_tail[-2000:]!r}; stderr_tail={stderr_tail[-2000:]!r}"
        )

    def evidence(self) -> dict[str, Any]:
        return {
            "component": self.component,
            "reason": self.reason,
            "returncode": self.returncode,
            "stdout_tail": self.stdout_tail,
            "stderr_tail": self.stderr_tail,
        }


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(
    path: Path,
    timeout: float = 10.0,
    process: subprocess.Popen[bytes] | None = None,
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process is not None and process.poll() is not None:
            raise OriginalToolSmokeError(
                f"process exited rc={process.returncode} while waiting for {path}"
            )
        time.sleep(0.05)
    raise OriginalToolSmokeError(f"timed out waiting for {path}")


def wait_for_tcp(
    port: int,
    timeout: float = 10.0,
    process: subprocess.Popen[bytes] | None = None,
) -> None:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise OriginalToolSmokeError(
                f"process exited rc={process.returncode} while waiting for listener port {port}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise OriginalToolSmokeError(f"timed out waiting for listener port {port}: {last_error}")


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def startup_timeout(args: argparse.Namespace) -> float:
    configured = getattr(args, "startup_timeout", None)
    return float(configured) if configured is not None else 10.0


def startup_error(
    component: str,
    reason: str,
    proc: subprocess.Popen[bytes] | None,
    stdout: Path,
    stderr: Path,
) -> OriginalToolStartupError:
    return OriginalToolStartupError(
        component,
        reason,
        returncode=proc.poll() if proc is not None else None,
        stdout_tail=read_tail(stdout),
        stderr_tail=read_tail(stderr),
    )


def read_tail(path: Path, limit: int = 12000) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")[-limit:]


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", errors="replace")).hexdigest()


def hex_text(value: str) -> str:
    return value.encode("utf-8").hex()


def local_password_fingerprint(verifier: str) -> str:
    digest = hashlib.sha256(verifier.encode("utf-8")).hexdigest()
    return f"local-password-verifier:v1:sha256:{digest}"


def grant_uuid(principal_uuid: str, right: str) -> str:
    digest = hashlib.sha256(f"{principal_uuid}:{right}".encode("utf-8")).hexdigest()
    return f"019f0a11-ce00-7000-8000-{digest[:12]}"


def normalized_result(status: str,
                      classification: str,
                      command_id: str,
                      output: str,
                      diagnostic: str = "") -> dict[str, Any]:
    return {
        "schema_version": "scratchbird_reference_replay_normalized_result_v1",
        "status": status,
        "classification": classification,
        "command_id": command_id,
        "output_sha256": sha256_text(output),
        "diagnostic": diagnostic,
        "canonicalization": {
            "whitespace": "preserve_for_smoke_tail_hash",
            "ordering": "single_probe_row",
            "nondeterministic_values": "none",
        },
    }


def write_auth_file(database: Path) -> Path:
    path = Path(str(database) + ".sb.local_password_auth")
    path.write_text(f"alice\tlocal_password\t{VERIFIER}\n", encoding="utf-8")
    event = "\t".join(
        [
            "SBSECPL1",
            "PRINCIPAL",
            "0",
            PRINCIPAL_UUID,
            hex_text("alice"),
            "user",
            "active",
            hex_text(local_password_fingerprint(VERIFIER)),
            "1",
            "0",
        ]
    )
    grant_lines = []
    for generation, right in enumerate(("CONNECT", "SEC_IDENTITY_ADMIN"), start=2):
        grant_lines.append(
            "\t".join(
                [
                    "SBSECPL1",
                    "GRANT",
                    "0",
                    grant_uuid(PRINCIPAL_UUID, right),
                    PRINCIPAL_UUID,
                    "principal",
                    "",
                    "",
                    right,
                    PRINCIPAL_UUID,
                    "allow",
                    str(generation),
                    "0",
                ]
            )
        )
    Path(str(database) + ".sb.security_principal_events").write_text(
        event
        + "\n"
        + "\n".join(grant_lines)
        + "\n",
        encoding="utf-8",
    )
    return path


def make_work_dir(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="rt_", dir=root))
    endpoint_probe = work / "s" / "c" / "sbps.sock"
    listener_probe = work / "l" / "c" / "l00000.management.sock"
    if len(str(endpoint_probe)) >= 100 or len(str(listener_probe)) >= 100:
        raise OriginalToolSmokeError(f"work path too long for IPC socket safety: {work}")
    return work


def start_server(args: argparse.Namespace, work: Path) -> tuple[subprocess.Popen[bytes], dict[str, Any]]:
    server_root = work / "s"
    database = work / "d" / "r.sbdb"
    endpoint = server_root / "c" / "sbps.sock"
    database.parent.mkdir(parents=True, exist_ok=True)
    server_root.mkdir(parents=True, exist_ok=True)
    auth_file = write_auth_file(database)
    stdout = server_root / "server.out"
    stderr = server_root / "server.err"
    cmd = [
        args.server,
        "--foreground",
        "--no-listeners",
        "--create-if-missing",
        "--control-dir",
        str(server_root / "c"),
        "--runtime-dir",
        str(server_root / "r"),
        "--database",
        str(database),
        "--sbps-endpoint",
        str(endpoint),
        "--log",
        str(server_root / "server.jsonl"),
        "--log-level",
        "info",
    ]
    env = dict(os.environ)
    if args.family == "firebird":
        env.setdefault("SB_COMPATIBILITY_FIREBIRD_PASSWORD", "local_password")
        env.setdefault("SB_REFERENCE_FIREBIRD_PASSWORD", "local_password")
        env.setdefault("SB_COMPATIBILITY_FIREBIRD_VERIFIER", VERIFIER)
        env.setdefault("SB_REFERENCE_FIREBIRD_VERIFIER", VERIFIER)
        env.setdefault("SB_COMPATIBILITY_FIREBIRD_PRINCIPAL_UUID", PRINCIPAL_UUID)
        env.setdefault("SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID", PRINCIPAL_UUID)
    proc: subprocess.Popen[bytes] | None = None
    try:
        with stdout.open("wb") as stdout_handle, stderr.open("wb") as stderr_handle:
            proc = subprocess.Popen(cmd, stdout=stdout_handle, stderr=stderr_handle, env=env)
        wait_for_path(endpoint, startup_timeout(args), proc)
    except Exception as exc:
        stop_process(proc)
        if isinstance(exc, OriginalToolStartupError):
            raise
        raise startup_error("scratchbird_server", str(exc), proc, stdout, stderr) from exc
    assert proc is not None
    return proc, {
        "command": cmd,
        "database": str(database),
        "auth_file": str(auth_file),
        "endpoint": str(endpoint),
        "stdout": str(stdout),
        "stderr": str(stderr),
    }


def start_listener(args: argparse.Namespace,
                   work: Path,
                   server_info: dict[str, Any]) -> tuple[subprocess.Popen[bytes], dict[str, Any]]:
    listener_root = work / "l"
    listener_root.mkdir(parents=True, exist_ok=True)
    port = args.port if args.port > 0 else find_free_port()
    stdout = listener_root / "listener.out"
    stderr = listener_root / "listener.err"
    cmd = [
        args.listener,
        "--foreground",
        f"--protocol-family={args.family}",
        f"--listener-profile={args.family}_reference_replay",
        "--bundle-contract-id=bundle.default@1",
        f"--database-selector=dev_bootstrap_path:{server_info['database']}",
        f"--server-endpoint=unix:{server_info['endpoint']}",
        f"--parser-executable={args.parser_worker}",
        f"--control-dir={listener_root / 'c'}",
        f"--runtime-dir={listener_root / 'r'}",
        "--bind-address=127.0.0.1",
        f"--port={port}",
        "--warm-pool-min=1",
        "--warm-pool-max=2",
        "--tls-required=false",
    ]
    env = dict(os.environ)
    if args.family == "firebird":
        # SBgate is one generic listener executable.  Its configured child
        # envelope accepts only family-neutral credentials; family aliases are
        # intentionally not part of the listener/controller boundary.
        env.setdefault("SB_COMPATIBILITY_AUTH_PASSWORD", "local_password")
        env.setdefault("SB_COMPATIBILITY_AUTH_VERIFIER", VERIFIER)
        env.setdefault("SB_COMPATIBILITY_AUTH_PRINCIPAL_UUID", PRINCIPAL_UUID)
        for family_alias in (
            "SB_COMPATIBILITY_FIREBIRD_PASSWORD",
            "SB_REFERENCE_FIREBIRD_PASSWORD",
            "SB_COMPATIBILITY_FIREBIRD_VERIFIER",
            "SB_REFERENCE_FIREBIRD_VERIFIER",
            "SB_COMPATIBILITY_FIREBIRD_PRINCIPAL_UUID",
            "SB_REFERENCE_FIREBIRD_PRINCIPAL_UUID",
        ):
            env.pop(family_alias, None)
    proc: subprocess.Popen[bytes] | None = None
    try:
        with stdout.open("wb") as stdout_handle, stderr.open("wb") as stderr_handle:
            proc = subprocess.Popen(cmd, stdout=stdout_handle, stderr=stderr_handle, env=env)
        wait_for_tcp(port, startup_timeout(args), proc)
    except Exception as exc:
        stop_process(proc)
        if isinstance(exc, OriginalToolStartupError):
            raise
        raise startup_error("scratchbird_listener", str(exc), proc, stdout, stderr) from exc
    assert proc is not None
    return proc, {
        "command": cmd,
        "port": port,
        "stdout": str(stdout),
        "stderr": str(stderr),
    }


def firebird_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    script = work / "firebird_probe.sql"
    output = work / "firebird_probe.out"
    script.write_text(
        "set bail on;\n"
        "select 1 as sb_reference_probe from rdb$database;\n"
        "quit;\n",
        encoding="utf-8",
    )
    database = f"127.0.0.1/{port}:default"
    cmd = [
        args.tool,
        "-q",
        "-user",
        "alice",
        "-password",
        "local_password",
        "-i",
        str(script),
        "-o",
        str(output),
        database,
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    out = read_tail(output)
    combined = (proc.stdout or "") + "\n" + out
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"Firebird isql smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if "SB_REFERENCE_PROBE" not in combined.upper() or "1" not in combined:
        raise OriginalToolSmokeError(f"Firebird isql smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "firebird.isql.select_constant_from_rdb_database",
        combined,
    )
    return {
        "command": cmd,
        "database": database,
        "script": str(script),
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": (proc.stdout or "")[-4000:],
        "output_tail": out[-4000:],
        "normalized_result": norm,
    }


def postgresql_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_postgresql_probe.out"
    cmd = [
        args.tool,
        "-h",
        "127.0.0.1",
        "-p",
        str(port),
        "-U",
        "alice",
        "-d",
        "default",
        "-v",
        "ON_ERROR_STOP=1",
        "-At",
        "-c",
        "select 1 as sb_reference_probe",
    ]
    env = dict(os.environ)
    env["PGPASSWORD"] = "local_password"
    env["PGSSLMODE"] = "disable"
    proc = subprocess.run(
        cmd,
        cwd=work,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"PostgreSQL psql smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    lines = [line.strip() for line in combined.splitlines() if line.strip()]
    if "1" not in lines:
        raise OriginalToolSmokeError(f"PostgreSQL psql smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        f"{args.family}.psql.select_constant",
        combined,
    )
    return {
        "command": cmd,
        "database": f"{args.family}+postgresql://alice@127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def mysql_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_mysql_probe.out"
    cmd = [
        args.tool,
        "--protocol=TCP",
        "--host=127.0.0.1",
        f"--port={port}",
        "--user=alice",
        "--password=local_password",
        "--database=default",
        "--ssl-mode=DISABLED",
        "--batch",
        "--skip-column-names",
        "--execute=select 1 as sb_reference_probe",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"MySQL client smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    lines = [line.strip() for line in combined.splitlines() if line.strip()]
    if "1" not in lines:
        raise OriginalToolSmokeError(f"MySQL client smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        f"{args.family}.mysql_client.select_constant",
        combined,
    )
    return {
        "command": cmd,
        "database": f"{args.family}+mysql://alice@127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def http_rest_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_curl_probe.out"
    if args.family == "opensearch_sql_ppl":
        url = f"http://127.0.0.1:{port}/_plugins/_sql"
        data = '{"query":"select 1"}'
        command_id = "opensearch_sql_ppl.curl.rest_probe"
    else:
        url = f"http://127.0.0.1:{port}/scratchbird_probe/_search"
        data = '{"query":{"match_all":{}}}'
        command_id = "opensearch.curl.rest_probe"
    cmd = [
        args.tool,
        "--silent",
        "--show-error",
        "--fail-with-body",
        "--request",
        "POST",
        "--header",
        "Content-Type: application/json",
        "--data",
        data,
        url,
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"curl REST smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if '"scratchbird_reference_probe":1' not in combined.replace(" ", ""):
        raise OriginalToolSmokeError(f"curl REST smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        command_id,
        combined,
    )
    return {
        "command": cmd,
        "database": f"{args.family}+http://127.0.0.1:{port}",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def apache_ignite_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_sqlline_probe.out"
    jdbc_url = f"jdbc:ignite:thin://127.0.0.1:{port}/"
    cmd = [
        args.tool,
        "--silent=true",
        "--showHeader=false",
        "--outputformat=csv",
        "-u",
        jdbc_url,
        "-n",
        "alice",
        "-p",
        "local_password",
        "-e",
        "SELECT 1;",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"Apache Ignite SQLLine smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    normalized_lines = [
        line.strip().strip("'\"")
        for line in combined.replace("\r", "").splitlines()
        if line.strip()
    ]
    if "1" not in normalized_lines:
        raise OriginalToolSmokeError(
            f"Apache Ignite SQLLine smoke returned unexpected output:\n{combined[-12000:]}"
        )
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "apache_ignite.sqlline.select_constant",
        combined,
    )
    return {
        "command": cmd,
        "database": jdbc_url,
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def influxdb_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_influxdb3_probe.out"
    cmd = [
        args.tool,
        "query",
        "--host",
        f"http://127.0.0.1:{port}",
        "--database",
        "default",
        "--format",
        "json",
        "select 1 as sb_reference_probe",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"influxdb3 query smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    compact = combined.replace(" ", "")
    if '"scratchbird_reference_probe":1' not in compact:
        raise OriginalToolSmokeError(f"influxdb3 query smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "influxdb.influxdb3.query_sql_select_constant",
        combined,
    )
    return {
        "command": cmd,
        "database": f"influxdb3://127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def clickhouse_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_clickhouse_probe.out"
    tool_name = Path(args.tool).name
    cmd = [args.tool]
    if tool_name == "clickhouse":
        cmd.append("client")
    cmd.extend(
        [
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--user",
            "alice",
            "--password",
            "local_password",
            "--database",
            "default",
            "--no-secure",
            "--compression=0",
            "--format",
            "TabSeparated",
            "--no-warnings",
            "--no-server-client-version-message",
            "--query",
            "select 1 as sb_reference_probe",
        ]
    )
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"clickhouse client smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    lines = [line.strip() for line in combined.splitlines() if line.strip()]
    if "1" not in lines:
        raise OriginalToolSmokeError(
            f"clickhouse client smoke returned unexpected output:\n{combined[-12000:]}"
        )
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "clickhouse.clickhouse_client.select_constant",
        combined,
    )
    return {
        "command": cmd,
        "database": f"clickhouse://127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def neo4j_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_cypher_shell_probe.out"
    cmd = [
        args.tool,
        "-a",
        f"bolt://127.0.0.1:{port}",
        "-u",
        "neo4j",
        "-p",
        "local_password",
        "--format",
        "plain",
        "RETURN 1 AS sb_reference_probe",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"cypher-shell smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if "sb_reference_probe" not in combined or "1" not in combined.split():
        raise OriginalToolSmokeError(f"cypher-shell smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "neo4j.cypher_shell.return_constant",
        combined,
    )
    return {
        "command": cmd,
        "database": f"bolt://127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def milvus_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_pymilvus_probe.out"
    code = (
        "import grpc\n"
        "from pymilvus.grpc_gen import milvus_pb2, milvus_pb2_grpc\n"
        f"channel = grpc.insecure_channel('127.0.0.1:{port}')\n"
        "stub = milvus_pb2_grpc.MilvusServiceStub(channel)\n"
        "response = stub.ShowCollections(milvus_pb2.ShowCollectionsRequest(), timeout=5)\n"
        "print('status=' + str(response.status.error_code))\n"
        "print('collections=' + ','.join(response.collection_names))\n"
    )
    cmd = [args.tool, "-c", code]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"PyMilvus generated-stub smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if "status=0" not in combined or "scratchbird_reference_probe" not in combined:
        raise OriginalToolSmokeError(
            f"PyMilvus generated-stub smoke returned unexpected output:\n{combined[-12000:]}"
        )
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "milvus.pymilvus_stub.show_collections",
        combined,
    )
    return {
        "command": cmd,
        "database": f"milvus+grpc://127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def immudb_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / "immudb_immuclient_probe.out"
    commands = [
        [
            args.tool,
            "health",
            "--immudb-address",
            "127.0.0.1",
            "--immudb-port",
            str(port),
        ]
    ]
    env = dict(os.environ)
    env["HOME"] = str(work)
    combined_parts: list[str] = []
    returncodes: list[int] = []
    for cmd in commands:
        proc = subprocess.run(
            cmd,
            cwd=work,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.tool_timeout,
            check=False,
        )
        returncodes.append(proc.returncode)
        combined_parts.append("$ " + " ".join(cmd))
        combined_parts.append(proc.stdout or "")
        if proc.returncode != 0:
            combined = "\n".join(combined_parts)
            output.write_text(combined, encoding="utf-8", errors="replace")
            raise OriginalToolSmokeError(
                f"immuclient smoke failed rc={proc.returncode}\n{combined[-12000:]}"
            )
    combined = "\n".join(combined_parts)
    output.write_text(combined, encoding="utf-8", errors="replace")
    lower = combined.lower()
    if "error" in lower or ("pending" not in lower and "request" not in lower):
        raise OriginalToolSmokeError(
            f"immuclient smoke returned unexpected output:\n{combined[-12000:]}"
        )
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "immudb.immuclient.health",
        combined,
    )
    return {
        "command": commands,
        "database": f"immudb+grpc://127.0.0.1:{port}/defaultdb",
        "output": str(output),
        "returncode": max(returncodes),
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def tikv_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / "tikv_ctl_probe.out"
    cmd = [
        args.tool,
        "--host",
        f"127.0.0.1:{port}",
        "store",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"tikv-ctl smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if "store id" not in combined.lower() or "1" not in combined:
        raise OriginalToolSmokeError(f"tikv-ctl smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "tikv.tikv_ctl.store_info",
        combined,
    )
    return {
        "command": cmd,
        "database": f"tikv+grpc://127.0.0.1:{port}/default",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def foundationdb_probe(args: argparse.Namespace,
                       work: Path,
                       port: int,
                       listener_info: dict[str, Any]) -> dict[str, Any]:
    cluster_file = work / "scratchbird.cluster"
    output = work / "foundationdb_fdbcli_probe.out"
    cluster_file.write_text(f"scratchbird:73@127.0.0.1:{port}\n", encoding="utf-8")
    cmd = [
        args.tool,
        "--cluster-file",
        str(cluster_file),
        "--timeout",
        "1",
        "--exec",
        "status json",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=max(args.tool_timeout, 5),
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    listener_tail = read_tail(Path(listener_info["stderr"]))
    worker_marker = "foundationdb_flow_open_database_cluster_status_refusal"
    if worker_marker not in listener_tail:
        raise OriginalToolSmokeError(
            "fdbcli did not reach the FoundationDB Flow OpenDatabase boundary\n"
            f"tool output:\n{combined[-4000:]}\nlistener stderr:\n{listener_tail[-8000:]}"
        )
    if "specified timeout reached" not in combined.lower():
        raise OriginalToolSmokeError(
            f"fdbcli status-json probe did not return the expected bounded timeout:\n{combined[-12000:]}"
        )
    norm = normalized_result(
        "unsupported",
        "mapped_failure",
        "foundationdb.fdbcli.status_json.cluster_boundary_refusal",
        combined + "\n" + listener_tail,
        "FOUNDATIONDB.CLUSTER_STATUS.UNSUPPORTED_PUBLIC_CLUSTER_PROVIDER",
    )
    return {
        "command": cmd,
        "database": f"foundationdb+flow://127.0.0.1:{port}/default",
        "cluster_file": str(cluster_file),
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "listener_stderr_tail": listener_tail[-4000:],
        "normalized_result": norm,
    }


def mongodb_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_mongosh_probe.out"
    database = f"mongodb://127.0.0.1:{port}/default?directConnection=true"
    eval_text = (
        "const r = db.runCommand({ping: 1}); "
        "if (r.ok !== 1 || r.scratchbird_reference_probe !== 1) { "
        "throw new Error('ScratchBird MongoDB probe marker missing: ' + JSON.stringify(r)); "
        "} "
        "print(JSON.stringify(r));"
    )
    cmd = [
        args.tool,
        database,
        "--quiet",
        "--eval",
        eval_text,
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"mongosh smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    compact = combined.replace(" ", "")
    if '"scratchbird_reference_probe":1' not in compact or '"ok":1' not in compact:
        raise OriginalToolSmokeError(f"mongosh smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "mongodb.mongosh.run_command_ping",
        combined,
    )
    return {
        "command": cmd,
        "database": database,
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def redis_probe(args: argparse.Namespace, work: Path, port: int) -> dict[str, Any]:
    output = work / f"{args.family}_redis_cli_probe.out"
    cmd = [
        args.tool,
        "-h",
        "127.0.0.1",
        "-p",
        str(port),
        "PING",
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"redis-cli smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if "PONG" not in combined.upper():
        raise OriginalToolSmokeError(f"redis-cli smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "redis.redis_cli.ping",
        combined,
    )
    return {
        "command": cmd,
        "database": f"redis://127.0.0.1:{port}/0",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def cassandra_python(repo_root: Path) -> str:
    env_python = os.environ.get("SB_REFERENCE_CASSANDRA_PYTHON")
    if env_python:
        return env_python
    candidate = (
        repo_root
        / "project/tests/reference_regression/reference_release_acquisition/cassandra/5.0.8/regression/tools/cqlsh-venv/bin/python"
    )
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    return sys.executable


def cassandra_probe(args: argparse.Namespace, work: Path, port: int, repo_root: Path) -> dict[str, Any]:
    output = work / f"{args.family}_cqlsh_probe.out"
    query = "SELECT * FROM system.local WHERE key='local';"
    cmd = [
        cassandra_python(repo_root),
        args.tool,
        "127.0.0.1",
        str(port),
        "-e",
        query,
    ]
    proc = subprocess.run(
        cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"cqlsh smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    if "ScratchBirdReference" not in combined:
        raise OriginalToolSmokeError(f"cqlsh smoke returned unexpected output:\n{combined[-12000:]}")
    norm = normalized_result(
        "passed",
        "semantic_probe_passed",
        "cassandra.cqlsh.select_system_local",
        combined,
    )
    return {
        "command": cmd,
        "database": f"cql://127.0.0.1:{port}/system",
        "output": str(output),
        "returncode": proc.returncode,
        "stdout_tail": combined[-4000:],
        "normalized_result": norm,
    }


def embedded_shell_probe(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    output = work / f"{args.family}_embedded_shell_probe.out"
    parser_output = work / f"{args.family}_parser_probe.out"
    sql = "select 1 as sb_reference_probe;"
    if args.family == "sqlite":
        cmd = [args.tool, "-batch", "-noheader", "-list", ":memory:"]
    elif args.family == "duckdb":
        cmd = [args.tool, "-batch", "-noheader", "-list", ":memory:"]
    else:
        raise OriginalToolSmokeError(f"embedded shell probe is not implemented for {args.family}")
    proc = subprocess.run(
        cmd,
        cwd=work,
        input=sql + "\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    output.write_text(proc.stdout or "", encoding="utf-8", errors="replace")
    combined = proc.stdout or ""
    if proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"{args.family} original shell smoke failed rc={proc.returncode}\n{combined[-12000:]}"
        )
    lines = [line.strip() for line in combined.splitlines() if line.strip()]
    if "1" not in lines:
        raise OriginalToolSmokeError(
            f"{args.family} original shell smoke returned unexpected output:\n{combined[-12000:]}"
        )

    parser_cmd = [args.parser_worker, sql]
    parser_proc = subprocess.run(
        parser_cmd,
        cwd=work,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.tool_timeout,
        check=False,
    )
    parser_combined = parser_proc.stdout or ""
    parser_output.write_text(parser_combined, encoding="utf-8", errors="replace")
    if parser_proc.returncode != 0:
        raise OriginalToolSmokeError(
            f"{args.family} ScratchBird parser probe failed rc={parser_proc.returncode}\n"
            f"{parser_combined[-12000:]}"
        )
    if f'"dialect":"{args.family}"' not in parser_combined.replace(" ", ""):
        raise OriginalToolSmokeError(
            f"{args.family} parser output did not identify the expected dialect:\n"
            f"{parser_combined[-12000:]}"
        )
    if '"mapping_disposition":"admitted_sblr"' not in parser_combined.replace(" ", ""):
        raise OriginalToolSmokeError(
            f"{args.family} parser output did not admit the shell probe as SBLR:\n"
            f"{parser_combined[-12000:]}"
        )

    combined_for_hash = combined + "\n--- parser ---\n" + parser_combined
    norm = normalized_result(
        "passed",
        "embedded_shell_and_parser_probe_passed",
        f"{args.family}.embedded_shell.select_constant",
        combined_for_hash,
    )
    return {
        "command": cmd,
        "parser_command": parser_cmd,
        "database": f"{args.family}+embedded-shell://:memory:",
        "output": str(output),
        "parser_output": str(parser_output),
        "returncode": proc.returncode,
        "parser_returncode": parser_proc.returncode,
        "stdout_tail": combined[-4000:],
        "parser_stdout_tail": parser_combined[-4000:],
        "normalized_result": norm,
    }


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        text = read_tail(path)
        if text:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(text, file=sys.stderr)


def run(args: argparse.Namespace, work: Path, repo_root: Path) -> dict[str, Any]:
    server: subprocess.Popen[bytes] | None = None
    listener: subprocess.Popen[bytes] | None = None
    try:
        if args.family in EMBEDDED_SHELL_FAMILIES:
            tool_info = embedded_shell_probe(args, work)
            return {
                "schema_version": "scratchbird_reference_original_tool_smoke_v1",
                "gate": "reference_original_tool_smoke",
                "status": "passed",
                "timestamp_utc": utc_timestamp(),
                "family": args.family,
                "work_dir": str(work),
                "tool": tool_info,
                "authority_policy": "original_embedded_shell_validates_reference_syntax_scratchbird_parser_lowers_to_sblr_engine_not_bypassed",
            }
        server, server_info = start_server(args, work)
        listener, listener_info = start_listener(args, work, server_info)
        if args.family == "firebird":
            tool_info = firebird_probe(args, work, int(listener_info["port"]))
        elif args.family in MYSQL_WIRE_FAMILIES:
            tool_info = mysql_probe(args, work, int(listener_info["port"]))
        elif args.family in POSTGRESQL_WIRE_FAMILIES:
            tool_info = postgresql_probe(args, work, int(listener_info["port"]))
        elif args.family in HTTP_REST_FAMILIES:
            tool_info = http_rest_probe(args, work, int(listener_info["port"]))
        elif args.family in APACHE_IGNITE_JDBC_FAMILIES:
            tool_info = apache_ignite_probe(args, work, int(listener_info["port"]))
        elif args.family in INFLUXDB_HTTP_FAMILIES:
            tool_info = influxdb_probe(args, work, int(listener_info["port"]))
        elif args.family in CLICKHOUSE_WIRE_FAMILIES:
            tool_info = clickhouse_probe(args, work, int(listener_info["port"]))
        elif args.family in NEO4J_WIRE_FAMILIES:
            tool_info = neo4j_probe(args, work, int(listener_info["port"]))
        elif args.family in MILVUS_GRPC_FAMILIES:
            tool_info = milvus_probe(args, work, int(listener_info["port"]))
        elif args.family in IMMUDB_GRPC_FAMILIES:
            tool_info = immudb_probe(args, work, int(listener_info["port"]))
        elif args.family in TIKV_GRPC_FAMILIES:
            tool_info = tikv_probe(args, work, int(listener_info["port"]))
        elif args.family in FOUNDATIONDB_FLOW_FAMILIES:
            tool_info = foundationdb_probe(args, work, int(listener_info["port"]), listener_info)
        elif args.family in MONGODB_WIRE_FAMILIES:
            tool_info = mongodb_probe(args, work, int(listener_info["port"]))
        elif args.family in REDIS_WIRE_FAMILIES:
            tool_info = redis_probe(args, work, int(listener_info["port"]))
        elif args.family in CASSANDRA_WIRE_FAMILIES:
            tool_info = cassandra_probe(args, work, int(listener_info["port"]), repo_root)
        else:
            raise OriginalToolSmokeError(
                f"original-tool smoke is not implemented for family={args.family}"
            )
        return {
            "schema_version": "scratchbird_reference_original_tool_smoke_v1",
            "gate": "reference_original_tool_smoke",
            "status": "passed",
            "timestamp_utc": utc_timestamp(),
            "family": args.family,
            "work_dir": str(work),
            "server": server_info,
            "listener": listener_info,
            "tool": tool_info,
            "authority_policy": "original_tool_feeds_reference_parser_listener_only_engine_mga_security_storage_authority",
        }
    finally:
        stop_process(listener)
        stop_process(server)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--family", default="firebird")
    parser.add_argument("--tool", required=True)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--tool-timeout", type=int, default=20)
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    tool_path = Path(args.tool)
    if not tool_path.is_absolute():
        tool_path = repo_root / tool_path
    args.tool = str(tool_path)
    del args.repo_root
    work = make_work_dir(args.work_root)
    try:
        payload = run(args, work, repo_root)
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                                      encoding="utf-8")
        print(
            "reference_original_tool_smoke=passed "
            f"family={args.family} work={work} evidence={args.evidence_file}"
        )
        return 0
    except Exception as exc:  # noqa: BLE001 - preserve test failure context.
        print(f"reference_original_tool_smoke=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        failure_payload = {
            "schema_version": "scratchbird_reference_original_tool_smoke_v1",
            "gate": "reference_original_tool_smoke",
            "status": "failed",
            "timestamp_utc": utc_timestamp(),
            "family": args.family,
            "work_dir": str(work),
            "normalized_result": normalized_result(
                "failed",
                "environment_or_protocol_error",
                f"{args.family}.original_tool_smoke",
                str(exc),
                str(exc),
            ),
            "authority_policy": "original_tool_feeds_reference_parser_listener_only_engine_mga_security_storage_authority",
        }
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(
            json.dumps(failure_payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
