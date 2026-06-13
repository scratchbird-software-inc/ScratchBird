#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""AEIC live proof that killed server agent state replays from durable storage."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

import live_server_agent_storage_benchmark_gate as live


class GateError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def read_status(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise GateError(f"agent_runtime_status_missing:{path}") from exc
    except json.JSONDecodeError as exc:
        raise GateError(f"agent_runtime_status_not_json:{path}") from exc
    status = payload.get("server_agent_runtime", {})
    require(isinstance(status, dict), "server_agent_runtime_object_missing")
    return status


def wait_for_status(path: Path,
                    predicate: Callable[[dict[str, Any]], bool],
                    reason: str,
                    timeout: float = 20.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if path.exists():
            last = read_status(path)
            if predicate(last):
                return last
        time.sleep(0.05)
    raise GateError(f"timed_out_waiting_for_agent_runtime:{reason}:{last}")


def worker_leases_ready(status: dict[str, Any]) -> bool:
    worker_count = int(status.get("worker_thread_count", 0))
    return (
        status.get("started") is True
        and worker_count >= 2
        and worker_count <= 5
        and int(status.get("durable_catalog_generation", 0)) > 0
        and int(status.get("durable_lease_count", 0)) >= worker_count
        and int(status.get("total_worker_ticks", 0)) >= worker_count
    )


def kill_server(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.kill()
    live.wait_for_exit(proc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--ipc-tester", required=True)
    parser.add_argument("--work-dir", required=True)
    parsed = parser.parse_args()
    parsed.work = live.make_work_dir(Path(parsed.work_dir))

    server1: subprocess.Popen[bytes] | None = None
    server2: subprocess.Popen[bytes] | None = None
    try:
        database = parsed.work / "agent-runtime-kill-replay.sbdb"
        live.write_live_auth_fixture(database)

        endpoint1 = parsed.work / "sc1" / "s.sock"
        server1 = live.start_server(
            parsed, database, parsed.work / "sc1", parsed.work / "sr1",
            endpoint1, "first", True
        )
        status1_path = parsed.work / "sc1" / "sb_server.agent_runtime.json"
        first = wait_for_status(status1_path, worker_leases_ready,
                                "first_runtime_leases")
        require(int(first.get("last_recovery_replayed_count", 0)) == 0,
                f"first_start_reported_replay:{first}")
        first_generation = int(first.get("durable_catalog_generation", 0))
        require("durable_action_backlog_count" in first,
                "durable_action_backlog_count_missing_before_kill")
        require("durable_replay_pending_lease_count" in first,
                "durable_replay_pending_lease_count_missing_before_kill")

        kill_server(server1)
        server1 = None

        endpoint2 = parsed.work / "sc2" / "s.sock"
        server2 = live.start_server(
            parsed, database, parsed.work / "sc2", parsed.work / "sr2",
            endpoint2, "second", False
        )
        status2_path = parsed.work / "sc2" / "sb_server.agent_runtime.json"
        recovered = wait_for_status(
            status2_path,
            lambda status: worker_leases_ready(status)
            and int(status.get("last_recovery_replayed_count", 0)) >= 1
            and int(status.get("durable_catalog_generation", 0)) > first_generation,
            "killed_runtime_recovered_replay",
        )
        require("durable_action_backlog_count" in recovered,
                "durable_action_backlog_count_missing_after_recovery")
        require("durable_replay_pending_action_count" in recovered,
                "durable_replay_pending_action_count_missing_after_recovery")
        require(int(recovered.get("last_recovery_replayed_count", 0)) >=
                int(first.get("durable_lease_count", 0)),
                f"replayed_count_too_low:{recovered}")

        live.stop_server_via_ipc(parsed, endpoint2, server2, "second")
        server2 = None
        print(f"server_agent_runtime_process_kill_replay_gate=passed work={parsed.work}")
        shutil.rmtree(parsed.work, ignore_errors=True)
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest needs concrete failure plus logs.
        print(
            f"server_agent_runtime_process_kill_replay_gate=failed work={parsed.work}: {exc}",
            file=sys.stderr,
        )
        for path in sorted(parsed.work.glob("**/sb_server.agent_runtime.json")):
            print(f"--- {path} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        live.dump_logs(parsed.work)
        return 1
    finally:
        live.stop_process(server2)
        live.stop_process(server1)


if __name__ == "__main__":
    raise SystemExit(main())
