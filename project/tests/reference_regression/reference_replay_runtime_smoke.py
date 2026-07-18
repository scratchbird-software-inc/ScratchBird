#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Start a ScratchBird reference parser replay runtime smoke.

This gate is deliberately small: it proves the canonical replay database,
server endpoint, listener endpoint, and parser worker lifecycle can start and
stop with public runtime binaries. Original upstream regression tools run in
later replay gates against the same endpoint contract.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"


class RuntimeSmokeError(RuntimeError):
    pass


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise RuntimeSmokeError(f"timed out waiting for {path}")


def wait_for_tcp(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeSmokeError(f"timed out waiting for listener port {port}: {last_error}")


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def write_auth_file(database: Path) -> Path:
    path = Path(str(database) + ".sb.local_password_auth")
    path.write_text(f"alice\tlocal_password\t{VERIFIER}\n", encoding="utf-8")
    return path


def read_tail(path: Path, limit: int = 12000) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")[-limit:]


def make_work_dir(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="reference_replay_", dir=root))
    endpoint_probe = work / "server" / "control" / "sbps.sock"
    if len(str(endpoint_probe)) >= 100:
        shutil.rmtree(work, ignore_errors=True)
        raise RuntimeSmokeError(f"work path too long for IPC socket safety: {work}")
    return work


def start_runtime(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    server_root = work / "server"
    listener_root = work / "listener"
    database = work / "database" / "reference_replay.sbdb"
    endpoint = server_root / "control" / "sbps.sock"
    server_stdout = server_root / "server.out"
    server_stderr = server_root / "server.err"
    listener_stdout = listener_root / "listener.out"
    listener_stderr = listener_root / "listener.err"
    database.parent.mkdir(parents=True, exist_ok=True)
    server_root.mkdir(parents=True, exist_ok=True)
    listener_root.mkdir(parents=True, exist_ok=True)
    auth_file = write_auth_file(database)
    port = args.port if args.port > 0 else find_free_port()

    server_cmd = [
        args.server,
        "--foreground",
        "--no-listeners",
        "--create-if-missing",
        "--control-dir",
        str(server_root / "control"),
        "--runtime-dir",
        str(server_root / "runtime"),
        "--database",
        str(database),
        "--sbps-endpoint",
        str(endpoint),
        "--log",
        str(server_root / "server.jsonl"),
        "--log-level",
        "info",
    ]
    server = subprocess.Popen(
        server_cmd,
        stdout=server_stdout.open("wb"),
        stderr=server_stderr.open("wb"),
    )
    try:
        wait_for_path(endpoint)

        listener_cmd = [
            args.listener,
            "--foreground",
            f"--protocol-family={args.family}",
            f"--listener-profile={args.family}_reference_replay",
            "--bundle-contract-id=bundle.default@1",
            f"--database-selector=dev_bootstrap_path:{database}",
            f"--server-endpoint=unix:{endpoint}",
            f"--parser-executable={args.parser_worker}",
            f"--control-dir={listener_root / 'control'}",
            f"--runtime-dir={listener_root / 'runtime'}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
            "--tls-required=false",
        ]
        listener = subprocess.Popen(
            listener_cmd,
            stdout=listener_stdout.open("wb"),
            stderr=listener_stderr.open("wb"),
        )
        try:
            wait_for_tcp(port)
            return {
                "schema_version": "scratchbird_reference_replay_runtime_smoke_v1",
                "gate": "reference_replay_runtime_smoke",
                "timestamp_utc": utc_timestamp(),
                "status": "passed",
                "family": args.family,
                "database_path": str(database),
                "database_created": database.exists(),
                "auth_file_path": str(auth_file),
                "auth_file_created": auth_file.exists(),
                "server": {
                    "pid": server.pid,
                    "endpoint": str(endpoint),
                    "endpoint_exists": endpoint.exists(),
                    "command": server_cmd,
                    "stdout": str(server_stdout),
                    "stderr": str(server_stderr),
                },
                "listener": {
                    "pid": listener.pid,
                    "port": port,
                    "command": listener_cmd,
                    "stdout": str(listener_stdout),
                    "stderr": str(listener_stderr),
                },
                "authority_policy": "parser_listener_feeds_sblr_uuid_route_only_engine_mga_security_storage_authority",
            }
        finally:
            stop_process(listener)
    finally:
        stop_process(server)


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        text = read_tail(path)
        if text:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(text, file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--family", default="firebird")
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args(argv)

    del args.repo_root
    work = make_work_dir(args.work_root)
    try:
        payload = start_runtime(args, work)
        payload["work_dir"] = str(work)
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(
            "reference_replay_runtime_smoke=passed "
            f"family={args.family} work={work} evidence={args.evidence_file}"
        )
        return 0
    except Exception as exc:  # noqa: BLE001 - test output needs the exact failure.
        print(f"reference_replay_runtime_smoke=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
