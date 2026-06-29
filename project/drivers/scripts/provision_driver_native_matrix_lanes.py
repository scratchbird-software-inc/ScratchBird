#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Provision fresh database/server lanes for the driver-native matrix.

The matrix runner is intentionally read-only with respect to runtime topology:
it consumes a lane manifest keyed by route, sslmode, and page_size. This helper
creates those lanes under build/, starts SBsrv/SBgate where needed, verifies the
route with SBsql, and records enough process state to stop the lanes later.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import signal
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


DEFAULT_REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_RUNTIME_REL = Path("build/driver-conformance/native-matrix-lanes")
DEFAULT_MANIFEST = "lane-manifest.json"
DEFAULT_STATE = "lane-state.json"
DEFAULT_BASE_PORT = 33100
PAGE_SIZE_BYTES = {
    "4k": 4096,
    "8k": 8192,
    "16k": 16384,
    "32k": 32768,
    "64k": 65536,
    "128k": 131072,
}
ROUTE_VARIANTS = [
    ("embedded", "disable"),
    ("ipc_local", "disable"),
    ("listener-parser", "require"),
    ("listener-parser", "disable"),
    ("manager-listener-parser", "require"),
    ("manager-listener-parser", "disable"),
]


class ProvisionError(RuntimeError):
    pass


def utc_stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def repo_root_from_args(value: Path | None) -> Path:
    return (value or DEFAULT_REPO_ROOT).resolve()


def default_runtime_root(repo_root: Path, run_id: str) -> Path:
    return (repo_root / DEFAULT_RUNTIME_REL / run_id).resolve()


def runtime_paths(runtime_root: Path) -> tuple[Path, Path]:
    return runtime_root / DEFAULT_MANIFEST, runtime_root / DEFAULT_STATE


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def selected(values: list[str], requested: list[str] | None) -> list[str]:
    if not requested:
        return values
    requested_set = set(requested)
    missing = sorted(requested_set - set(values))
    if missing:
        raise ProvisionError("unknown requested value(s): " + ", ".join(missing))
    return [value for value in values if value in requested_set]


def selected_route_variants(routes: list[str], sslmodes: list[str]) -> list[tuple[str, str]]:
    return [
        (route, sslmode)
        for route, sslmode in ROUTE_VARIANTS
        if route in routes and sslmode in sslmodes
    ]


def lane_id(route: str, sslmode: str, page_size: str) -> str:
    return f"{route.replace('-', '_')}_{sslmode}_{page_size}"


def short_ipc_endpoint(runtime_root: Path, lane: str) -> Path:
    digest = hashlib.sha256(f"{runtime_root}:{lane}".encode("utf-8")).hexdigest()[:16]
    endpoint_root = Path("/tmp") / f"sbdnml_{digest}"
    endpoint_root.mkdir(parents=True, exist_ok=True)
    return endpoint_root / "sc" / "s.sock"


def short_lane_runtime_root(runtime_root: Path, lane: str) -> Path:
    digest = hashlib.sha256(f"{runtime_root}:{lane}".encode("utf-8")).hexdigest()[:16]
    return Path("/tmp") / f"sbdnml_{digest}"


def validate_binaries(repo_root: Path) -> dict[str, Path]:
    bins = {
        "server": repo_root / "build/output/linux/bin/SBsrv",
        "listener": repo_root / "build/output/linux/bin/SBgate",
        "parser": repo_root / "build/output/linux/bin/SBParser",
        "sbsql": repo_root / "build/output/linux/bin/SBsql",
        "driver_seed": repo_root / "build/output/linux/bin/public_driver_test_database_seed",
    }
    missing = [str(path) for path in bins.values() if not path.exists()]
    if missing:
        raise ProvisionError("missing binaries: " + ", ".join(missing))
    return bins


def run_checked(command: list[str], *, stdout: Path, stderr: Path, cwd: Path) -> None:
    stdout.parent.mkdir(parents=True, exist_ok=True)
    with stdout.open("wb") as out, stderr.open("wb") as err:
        result = subprocess.run(command, cwd=cwd, stdout=out, stderr=err, check=False)
    if result.returncode != 0:
        raise ProvisionError(
            f"command failed rc={result.returncode}: {' '.join(command)} "
            f"stdout={stdout} stderr={stderr}"
        )


def spawn_process(command: list[str], *, stdout: Path, stderr: Path, cwd: Path) -> subprocess.Popen[bytes]:
    stdout.parent.mkdir(parents=True, exist_ok=True)
    out = stdout.open("ab")
    err = stderr.open("ab")
    try:
        return subprocess.Popen(
            command,
            cwd=cwd,
            stdout=out,
            stderr=err,
            start_new_session=True,
        )
    finally:
        out.close()
        err.close()


def wait_for_path(path: Path, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise ProvisionError(f"timed out waiting for {path}")


def wait_for_tcp(host: str, port: int, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise ProvisionError(f"timed out waiting for {host}:{port}: {last_error}")


def ensure_port_available(host: str, port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((host, port))
        except OSError as exc:
            raise ProvisionError(f"{host}:{port} is not available: {exc}") from exc


def ensure_tls_material(runtime_root: Path) -> tuple[Path, Path]:
    tls_dir = runtime_root / "tls"
    tls_dir.mkdir(parents=True, exist_ok=True)
    cert = tls_dir / "server.crt"
    key = tls_dir / "server.key"
    if cert.exists() and key.exists():
        return cert, key
    result = subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-days",
            "30",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:127.0.0.1",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise ProvisionError("failed to generate TLS certificate: " + result.stderr.strip())
    key.chmod(0o600)
    cert.chmod(0o644)
    return cert, key


def seed_database(
    repo_root: Path,
    bins: dict[str, Path],
    lane_root: Path,
    page_size: str,
) -> tuple[Path, Path]:
    database = lane_root / "default.sbdb"
    manifest = lane_root / "driver_test_database.manifest.json"
    seed_pack_root = repo_root / "project/resources/seed-packs/initial-resource-pack"
    run_checked(
        [
            str(bins["driver_seed"]),
            "--output",
            str(database),
            "--manifest",
            str(manifest),
            "--resource-seed-pack-root",
            str(seed_pack_root),
            "--page-size",
            str(PAGE_SIZE_BYTES[page_size]),
            "--overwrite",
        ],
        stdout=lane_root / "logs" / "seed.out",
        stderr=lane_root / "logs" / "seed.err",
        cwd=repo_root,
    )
    return database, manifest


def verify_route(
    bins: dict[str, Path],
    lane_root: Path,
    database: Path,
    route: str,
    sslmode: str,
    *,
    endpoint: Path | None = None,
    port: int | None = None,
    tls_cert: Path | None = None,
) -> None:
    command = [str(bins["sbsql"]), str(database)]
    if route == "embedded":
        command.append("--mode=embedded")
    elif route == "ipc_local":
        if endpoint is None:
            raise ProvisionError("ipc route verification missing endpoint")
        command.extend(["--mode=local-ipc", f"--ipc-path={endpoint}"])
    elif route in {"listener-parser", "manager-listener-parser"}:
        if port is None:
            raise ProvisionError("network route verification missing port")
        command.extend(["--host=127.0.0.1", f"--port={port}", f"--sslmode={sslmode}"])
        if sslmode != "disable" and tls_cert is not None:
            command.extend(["--conn-opt", f"sslrootcert={tls_cert}"])
    else:
        raise ProvisionError(f"unknown route: {route}")
    command.extend(["--conn-opt", "role=sysarch", "-U", "alice", "-P", "scratchbird", "-q", "-A", "-t", "-c", "SELECT 1"])
    run_checked(
        command,
        stdout=lane_root / "logs" / "verify.out",
        stderr=lane_root / "logs" / "verify.err",
        cwd=lane_root,
    )
    output = (lane_root / "logs" / "verify.out").read_text(encoding="utf-8", errors="replace").strip()
    if output != "1":
        raise ProvisionError(f"route verification returned {output!r} for {route}/{sslmode}")


def terminate_pid(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except OSError:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            return


def kill_pid(pid: int) -> None:
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except OSError:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            return


def start(args: argparse.Namespace) -> int:
    repo_root = repo_root_from_args(args.repo_root)
    runtime_root = (args.runtime_root or default_runtime_root(repo_root, args.run_id)).resolve()
    manifest_path, state_path = runtime_paths(runtime_root)
    if state_path.exists() and not args.force:
        existing = read_json(state_path)
        if existing.get("status") == "running":
            raise ProvisionError(f"lane state already running: {state_path}")
    bins = validate_binaries(repo_root)
    host = "127.0.0.1"
    routes = selected([route for route, _ in ROUTE_VARIANTS], args.route)
    page_sizes = selected(list(PAGE_SIZE_BYTES), args.page_size)
    sslmodes = selected(["disable", "require"], args.sslmode)
    route_pairs = selected_route_variants(routes, sslmodes)
    if not route_pairs:
        raise ProvisionError("no route/sslmode lanes selected")

    tls_cert, tls_key = ensure_tls_material(runtime_root)
    lanes: list[dict[str, Any]] = []
    processes: list[dict[str, Any]] = []
    port = int(args.base_port)
    started_at = utc_stamp()
    for page_size in page_sizes:
        for route, sslmode in route_pairs:
            current_lane_id = lane_id(route, sslmode, page_size)
            lane_root = runtime_root / current_lane_id
            short_runtime = short_lane_runtime_root(runtime_root, current_lane_id)
            if args.force and lane_root.exists():
                shutil.rmtree(lane_root)
            if args.force and short_runtime.exists():
                shutil.rmtree(short_runtime)
            lane_root.mkdir(parents=True, exist_ok=True)
            database, seed_manifest = seed_database(repo_root, bins, lane_root, page_size)
            lane: dict[str, Any] = {
                "lane_id": current_lane_id,
                "route": route,
                "sslmode": sslmode,
                "page_size": page_size,
                "page_size_bytes": PAGE_SIZE_BYTES[page_size],
                "database": str(database),
                "host": host,
                "user": "alice",
                "password": "scratchbird",
                "role": "sysarch",
                "sslrootcert": str(tls_cert) if sslmode == "require" else "",
                "sslcert": "",
                "sslkey": "",
                "ipc_path": "",
                "seed_manifest": str(seed_manifest),
            }
            endpoint = short_ipc_endpoint(runtime_root, current_lane_id)
            server_control = short_runtime / "sc"
            server_runtime = short_runtime / "sr"
            listener_control = short_runtime / "lc"
            listener_runtime = short_runtime / "lr"
            if route != "embedded":
                for directory in (server_control, server_runtime):
                    directory.mkdir(parents=True, exist_ok=True)
                server = spawn_process(
                    [
                        str(bins["server"]),
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
                    stdout=lane_root / "logs" / "server.out",
                    stderr=lane_root / "logs" / "server.err",
                    cwd=repo_root,
                )
                processes.append({"role": "server", "lane_id": current_lane_id, "pid": server.pid})
                wait_for_path(endpoint)
                lane["ipc_path"] = str(endpoint)
                lane["runtime_control_root"] = str(short_runtime)
            if route in {"listener-parser", "manager-listener-parser"}:
                ensure_port_available(host, port)
                for directory in (listener_control, listener_runtime):
                    directory.mkdir(parents=True, exist_ok=True)
                listener_command = [
                    str(bins["listener"]),
                    "--foreground",
                    "--protocol-family=sbsql",
                    "--listener-profile=default",
                    "--bundle-contract-id=bundle.default@1",
                    f"--database-selector=dev_bootstrap_path:{database}",
                    f"--server-endpoint=unix:{endpoint}",
                    f"--parser-executable={bins['parser']}",
                    f"--control-dir={listener_control}",
                    f"--runtime-dir={listener_runtime}",
                    "--bind-address=127.0.0.1",
                    f"--port={port}",
                    f"--tls-required={'true' if sslmode == 'require' else 'false'}",
                ]
                if sslmode == "require":
                    listener_command.extend([f"--tls-cert-file={tls_cert}", f"--tls-key-file={tls_key}"])
                listener = spawn_process(
                    listener_command,
                    stdout=lane_root / "logs" / "listener.out",
                    stderr=lane_root / "logs" / "listener.err",
                    cwd=repo_root,
                )
                processes.append({"role": "listener", "lane_id": current_lane_id, "pid": listener.pid})
                wait_for_tcp(host, port)
                lane["port"] = port
                port += 1
            else:
                lane.pop("host", None)
            verify_route(
                bins,
                lane_root,
                database,
                route,
                sslmode,
                endpoint=endpoint,
                port=lane.get("port") if isinstance(lane.get("port"), int) else None,
                tls_cert=tls_cert,
            )
            lane["verification_status"] = "pass"
            lanes.append(lane)

    manifest = {
        "schema_version": "scratchbird_driver_native_matrix_lane_manifest_v1",
        "generated_utc": started_at,
        "runtime_root": str(runtime_root),
        "lane_count": len(lanes),
        "lanes": lanes,
    }
    state = {
        "schema_version": "scratchbird_driver_native_matrix_lane_state_v1",
        "status": "running",
        "generated_utc": started_at,
        "repo_root": str(repo_root),
        "runtime_root": str(runtime_root),
        "lane_manifest": str(manifest_path),
        "lane_count": len(lanes),
        "processes": processes,
    }
    write_json(manifest_path, manifest)
    write_json(state_path, state)
    print(f"driver_native_matrix_lanes=running lanes={len(lanes)} manifest={manifest_path}")
    return 0


def stop(args: argparse.Namespace) -> int:
    repo_root = repo_root_from_args(args.repo_root)
    runtime_root = (args.runtime_root or default_runtime_root(repo_root, args.run_id)).resolve()
    _, state_path = runtime_paths(runtime_root)
    if not state_path.exists():
        print(f"driver_native_matrix_lanes=stopped reason=no_state state={state_path}")
        return 0
    state = read_json(state_path)
    processes = [item for item in state.get("processes", []) if isinstance(item, dict)]
    for process in sorted(processes, key=lambda item: 0 if item.get("role") == "listener" else 1):
        pid = process.get("pid")
        if isinstance(pid, int) and pid > 0:
            terminate_pid(pid)
    time.sleep(1.0)
    for process in processes:
        pid = process.get("pid")
        if isinstance(pid, int) and pid > 0:
            kill_pid(pid)
    state["status"] = "stopped"
    state["stopped_utc"] = utc_stamp()
    write_json(state_path, state)
    print(f"driver_native_matrix_lanes=stopped state={state_path}")
    return 0


def status(args: argparse.Namespace) -> int:
    repo_root = repo_root_from_args(args.repo_root)
    runtime_root = (args.runtime_root or default_runtime_root(repo_root, args.run_id)).resolve()
    manifest_path, state_path = runtime_paths(runtime_root)
    payload = {
        "runtime_root": str(runtime_root),
        "lane_manifest": str(manifest_path),
        "state_path": str(state_path),
        "state": read_json(state_path) if state_path.exists() else {"status": "missing"},
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def plan(args: argparse.Namespace) -> int:
    repo_root = repo_root_from_args(args.repo_root)
    runtime_root = (args.runtime_root or default_runtime_root(repo_root, args.run_id)).resolve()
    routes = selected([route for route, _ in ROUTE_VARIANTS], args.route)
    page_sizes = selected(list(PAGE_SIZE_BYTES), args.page_size)
    sslmodes = selected(["disable", "require"], args.sslmode)
    route_pairs = selected_route_variants(routes, sslmodes)
    lanes = [
        {
            "lane_id": lane_id(route, sslmode, page_size),
            "route": route,
            "sslmode": sslmode,
            "page_size": page_size,
            "page_size_bytes": PAGE_SIZE_BYTES[page_size],
        }
        for page_size in page_sizes
        for route, sslmode in route_pairs
    ]
    report = {
        "command": "provision_driver_native_matrix_lanes.py plan",
        "status": "pass",
        "runtime_root": str(runtime_root),
        "lane_count": len(lanes),
        "lanes": lanes,
    }
    output = args.output or (runtime_root / "lane-plan.json")
    write_json(output, report)
    print(f"driver_native_matrix_lane_plan=pass lanes={len(lanes)} output={output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path)
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--run-id", default="manual")
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_selection_args(target: argparse.ArgumentParser) -> None:
        target.add_argument("--route", action="append")
        target.add_argument("--sslmode", action="append")
        target.add_argument("--page-size", action="append")

    start_parser = subparsers.add_parser("start")
    add_selection_args(start_parser)
    start_parser.add_argument("--base-port", type=int, default=DEFAULT_BASE_PORT)
    start_parser.add_argument("--force", action="store_true")
    start_parser.set_defaults(func=start)

    stop_parser = subparsers.add_parser("stop")
    stop_parser.set_defaults(func=stop)

    status_parser = subparsers.add_parser("status")
    status_parser.set_defaults(func=status)

    plan_parser = subparsers.add_parser("plan")
    add_selection_args(plan_parser)
    plan_parser.add_argument("--output", type=Path)
    plan_parser.set_defaults(func=plan)

    args = parser.parse_args()
    try:
        return int(args.func(args))
    except ProvisionError as exc:
        print(f"driver_native_matrix_lanes=fail: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
