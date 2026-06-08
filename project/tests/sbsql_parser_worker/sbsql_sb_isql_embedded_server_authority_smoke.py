#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Embedded sb_isql fallback-to-server authority smoke.

When a running server owns the database, sb_isql --mode=embedded must not use
the local sysarch bypass. It must discover the SBPS endpoint and reconnect
through the server-authorized local IPC route.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from live_auth_fixture import local_password_evidence, write_local_password_auth_fixture


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
WRONG_VERIFIER = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"


class SmokeError(RuntimeError):
    pass


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "sb_isql_embedded_authority")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="sbi_", dir=root))
        endpoint_probe = candidate / "sc" / "s.sock"
        if len(str(endpoint_probe)) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise SmokeError("unable to allocate a short-enough embedded authority workspace")


def wait_for_path(path: Path, timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise SmokeError(f"timed out waiting for {path}")


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
    for name in (
        "server.out",
        "server.err",
        "sb_isql_bad.out",
        "sb_isql_bad.err",
        "sb_isql_good.out",
        "sb_isql_good.err",
    ):
        path = work / name
        if path.exists():
            print(f"--- {name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)


def run_embedded_sb_isql(args: argparse.Namespace,
                         work: Path,
                         database: Path,
                         verifier: str,
                         output_prefix: str) -> subprocess.CompletedProcess[bytes]:
    evidence = local_password_evidence("alice", verifier)
    return subprocess.run(
        [
            args.sb_isql,
            str(database),
            "--mode=embedded",
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
        stdout=(work / f"{output_prefix}.out").open("wb"),
        stderr=(work / f"{output_prefix}.err").open("wb"),
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = make_work_dir(Path(args.work_dir))
    server: subprocess.Popen[bytes] | None = None
    try:
        database = work / "server_owned.sbdb"
        server_control = work / "sc"
        server_runtime = work / "sr"
        endpoint = server_control / "s.sock"
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

        bad = run_embedded_sb_isql(args, work, database, WRONG_VERIFIER, "sb_isql_bad")
        if bad.returncode == 0:
            output = (work / "sb_isql_bad.out").read_text(encoding="utf-8", errors="replace").strip()
            raise SmokeError(
                "embedded mode bypassed server authority while the server owned the database; "
                f"unexpected output={output!r}"
            )

        good = run_embedded_sb_isql(args, work, database, VERIFIER, "sb_isql_good")
        if good.returncode != 0:
            raise SmokeError(f"server-owned embedded fallback exited {good.returncode}")
        output = (work / "sb_isql_good.out").read_text(encoding="utf-8", errors="replace").strip()
        if output != "1":
            raise SmokeError(f"server-owned embedded fallback SELECT 1 returned {output!r}")
        print(f"sbsql_sb_isql_embedded_server_authority_smoke=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - ctest should receive the concrete failure.
        print(f"sbsql_sb_isql_embedded_server_authority_smoke=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1
    finally:
        stop_process(server)
        if not any((work / name).exists() and (work / name).stat().st_size
                   for name in ("server.err", "sb_isql_good.err")):
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
