#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Live sb_isql route smoke.

Drives the public CLI through:
  sb_isql -> sb_listener -> sbp_sbsql -> SBPS -> sb_server -> engine auth/query.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from live_auth_fixture import local_password_evidence, write_local_password_auth_fixture


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"


class SmokeError(RuntimeError):
    pass


def make_work_dir(preferred_root: Path) -> Path:
    roots: tuple[Path, ...]
    if os.name == "nt":
        configured_short_root = os.environ.get("SBP_SHORT_TMP")
        roots = (
            ((Path(configured_short_root),) if configured_short_root else ())
            + (preferred_root, Path(tempfile.gettempdir()) / "sblive")
        )
    else:
        roots = (preferred_root, Path(tempfile.gettempdir()) / "sb_isql_live")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="l", dir=root))
        endpoint_probe = candidate / "sc" / "s.sock"
        listener_probe = candidate / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(endpoint_probe)), len(str(listener_probe))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise SmokeError("unable to allocate a short-enough live route workspace")


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise SmokeError(f"timed out waiting for {path}")


def wait_for_tcp(port: int, timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise SmokeError(f"timed out waiting for listener port {port}: {last_error}")


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def dump_logs(work: Path) -> None:
    for name in ("server.out", "server.err", "listener.out", "listener.err", "sb_isql.out", "sb_isql.err"):
        path = work / name
        if path.exists():
            print(f"--- {name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = make_work_dir(Path(args.work_dir))
    server: subprocess.Popen[bytes] | None = None
    listener: subprocess.Popen[bytes] | None = None
    try:
        database = work / "live.sbdb"
        server_control = work / "sc"
        server_runtime = work / "sr"
        listener_control = work / "lc"
        listener_runtime = work / "lr"
        endpoint = server_control / "s.sock"
        port = find_free_port()
        write_local_password_auth_fixture(database, "alice", VERIFIER)

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
            stdout=(work / "server.out").open("wb"),
            stderr=(work / "server.err").open("wb"),
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
            stdout=(work / "listener.out").open("wb"),
            stderr=(work / "listener.err").open("wb"),
        )
        wait_for_tcp(port)

        evidence = local_password_evidence("alice", VERIFIER)
        completed = subprocess.run(
            [
                args.sb_isql,
                str(database),
                "--host=127.0.0.1",
                f"--port={port}",
                "--sslmode=disable",
                "-U",
                "alice",
                "-P",
                evidence,
                "-q",
                "-A",
                "-t",
                "-c",
                "SELECT 1",
            ],
            stdout=(work / "sb_isql.out").open("wb"),
            stderr=(work / "sb_isql.err").open("wb"),
            check=False,
        )
        if completed.returncode != 0:
            raise SmokeError(f"sb_isql exited {completed.returncode}")
        output = (work / "sb_isql.out").read_text(encoding="utf-8", errors="replace").strip()
        if output != "1":
            raise SmokeError(f"sb_isql SELECT 1 returned {output!r}")
        print(f"sbsql_sb_isql_live_route_smoke=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - ctest should receive the concrete failure.
        print(f"sbsql_sb_isql_live_route_smoke=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1
    finally:
        stop_process(listener)
        stop_process(server)
        if not any((work / name).exists() and (work / name).stat().st_size for name in ("sb_isql.err", "server.err", "listener.err")):
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
