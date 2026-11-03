#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Register C9 live instrument: database-file and sidecar opens made by sbsrv.

Runs the real server with an LD_PRELOAD open counter, once with no client
statements and once per statement count, and reports the opens each run made.
Under the single-owner model only sbsrv touches the database's files and its
own threads read through the open device, so serving-phase opens of the
database file should fall to zero as C9 converts the read sites.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

import live_server_agent_storage_benchmark_gate as live

STATEMENT_COUNTS = (1, 4)


def read_counts(path: Path) -> tuple[tuple[int, int], tuple[int, int]]:
    """(serving file, serving sidecar), (agent-thread file, agent-thread sidecar)."""
    live.require(path.is_file(), f"count_file_missing:{path}")
    fields = dict(item.split("=", 1) for item in path.read_text().split())
    return ((int(fields["file_opens"]), int(fields["sidecar_opens"])),
            (int(fields["agent_file_opens"]), int(fields["agent_sidecar_opens"])))


def run_statements(args: argparse.Namespace, database: Path, endpoint: Path, count: int, tag: str) -> None:
    port = live.find_free_port()
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
            f"--control-dir={args.work / ('l' + str(count))}",
            f"--runtime-dir={args.work / ('m' + str(count))}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
        ],
        stdout=(args.work / f"{tag}_listener.out").open("wb"),
        stderr=(args.work / f"{tag}_listener.err").open("wb"),
    )
    try:
        live.wait_for_tcp(port)
        script = args.work / f"{tag}.sql"
        script.write_text("SELECT 1;\n" * count)
        completed = subprocess.run(
            [args.sb_isql, str(database), "--host=127.0.0.1", f"--port={port}", "--sslmode=disable",
             "-U", "alice", "-P", live.PASSWORD, "-q", "-A", "-t", "-f", str(script)],
            stdout=(args.work / f"{tag}_isql.out").open("wb"),
            stderr=(args.work / f"{tag}_isql.err").open("wb"),
            check=False,
        )
        live.require(completed.returncode == 0, f"{tag}_sb_isql_failed:{completed.returncode}")
        rows = (args.work / f"{tag}_isql.out").read_text(errors="replace").split()
        live.require(rows == ["1"] * count, f"{tag}_sb_isql_unexpected:{rows!r}")
    finally:
        live.stop_process(listener)


def measure(args: argparse.Namespace, database: Path, statements: int):
    tag = f"run{statements}"
    counts = args.work / f"{tag}.counts"
    endpoint = args.work / f"c{statements}" / "s.sock"
    env = dict(os.environ)
    env["LD_PRELOAD"] = args.preload
    env["SB_C9_WATCH_PATH"] = str(database)
    env["SB_C9_COUNT_FILE"] = str(counts)
    command = [args.server, "--foreground", "--no-listeners",
               "--control-dir", str(endpoint.parent),
               "--runtime-dir", str(args.work / f"r{statements}"),
               "--database", str(database), "--sbps-endpoint", str(endpoint)]
    proc = subprocess.Popen(command, env=env,
                            stdout=(args.work / f"{tag}_server.out").open("wb"),
                            stderr=(args.work / f"{tag}_server.err").open("wb"))
    try:
        live.wait_for_path(endpoint)
        if statements:
            run_statements(args, database, endpoint, statements, tag)
        live.stop_server_via_ipc(args, endpoint, proc, tag)
        proc = None
    finally:
        live.stop_process(proc)
    return read_counts(counts)


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("--server", "--listener", "--parser-worker", "--sb-isql", "--ipc-tester",
                 "--database-seeder", "--preload", "--work-dir"):
        parser.add_argument(name, required=True)
    parser.add_argument("--expect", default="", help="exact counts, e.g. 0:F,S;1:F,S;4:F,S")
    parser.add_argument("--keep-work", action="store_true", help="keep logs (with SB_C9_TRACE_OPENS)")
    args = parser.parse_args()
    args.work = live.make_work_dir(Path(args.work_dir))
    try:
        database = args.work / "live.sbdb"
        seeded = subprocess.run([args.database_seeder, str(database), "alice", live.PASSWORD],
                                stdout=(args.work / "seed.out").open("wb"),
                                stderr=(args.work / "seed.err").open("wb"), check=False)
        live.require(seeded.returncode == 0 and database.is_file(), f"seed_failed:{seeded.returncode}")
        observed = {n: measure(args, database, n) for n in (0, *STATEMENT_COUNTS)}
        # Exact: opens made on non-agent server threads. Agent threads
        # ("sb-agent-*") run timed cycles whose opens vary with how long a run
        # takes (one landing inside a run adds about 8 database-file and 170
        # sidecar opens), so they are reported and not compared until the
        # agent contexts are converted.
        report = ";".join(f"{n}:{s[0]},{s[1]}" for n, (s, _a) in observed.items())
        agents = ";".join(f"{n}:{a[0]},{a[1]}" for n, (_s, a) in observed.items())
        print(f"c9_live_server_path_opens {report} agent_threads {agents}")
        if args.expect:
            live.require(report == args.expect,
                         f"live_open_counts_changed observed={report} expected={args.expect}; "
                         "if a C9 conversion removed opens, lower the expectation with it")
        if args.keep_work:
            print(f"work={args.work}")
        else:
            shutil.rmtree(args.work, ignore_errors=True)
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest needs the concrete failure and logs.
        print(f"c9_live_server_path_open_instrument=failed work={args.work}: {exc}", file=sys.stderr)
        live.dump_logs(args.work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
