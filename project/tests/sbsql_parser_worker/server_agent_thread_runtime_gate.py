#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""PFAR-019B live proof that server agents run in independent OS threads."""

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
                    timeout: float = 8.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if path.exists():
            last = read_status(path)
            if predicate(last):
                return last
        time.sleep(0.05)
    raise GateError(f"timed_out_waiting_for_agent_runtime:{reason}:{last}")


def linux_thread_names(pid: int) -> list[str]:
    task_root = Path("/proc") / str(pid) / "task"
    if not task_root.exists():
        return []
    names: list[str] = []
    for task in sorted(task_root.iterdir()):
        try:
            names.append((task / "comm").read_text(encoding="utf-8").strip())
        except OSError:
            continue
    return names


def has_staggered_worker_progress(status: dict[str, Any]) -> bool:
    worker_count = int(status.get("worker_thread_count", 0))
    scheduler_ticks = int(status.get("scheduler_ticks", 0))
    total_worker_ticks = int(status.get("total_worker_ticks", 0))
    return (
        worker_count >= 2
        and scheduler_ticks >= worker_count
        and total_worker_ticks >= worker_count
    )


def assert_process_threads(pid: int, status: dict[str, Any]) -> None:
    names = linux_thread_names(pid)
    require(names, "linux_thread_names_unavailable")
    worker_count = int(status.get("worker_thread_count", 0))
    require(worker_count >= 2, f"agent_worker_thread_count_too_low:{worker_count}")
    require(len(names) >= worker_count + 2,
            f"process_thread_count_too_low:names={names}:worker_count={worker_count}")
    require("sb-agent-sch" in names, f"agent_scheduler_thread_not_named:{names}")
    workers = [name for name in names if name.startswith("sb-agent-w")]
    require(len(workers) >= worker_count,
            f"agent_worker_threads_not_named:names={names}:worker_count={worker_count}")


def assert_agent_action_threads(status: dict[str, Any]) -> None:
    require(status.get("started") is True, "agent_runtime_not_started")
    worker_count = int(status.get("worker_thread_count", 0))
    require(worker_count >= 2, f"agent_runtime_worker_count_too_low:{worker_count}")
    require(int(status.get("foreground_reserved_capacity", 0)) >= 1,
            f"foreground_capacity_not_reserved:{status}")
    require(int(status.get("background_worker_slots", 0)) == worker_count,
            f"background_worker_slot_mismatch:{status}")
    require(status.get("worker_wake_policy") == "staggered_worker_per_scheduler_tick",
            f"agent_worker_wake_policy_mismatch:{status.get('worker_wake_policy')}")
    require(int(status.get("scheduler_ticks", 0)) >= worker_count,
            "scheduler_ticks_not_advancing")
    require(int(status.get("total_worker_ticks", 0)) >= worker_count,
            "worker_ticks_not_advancing")
    total_action_decisions = (
        int(status.get("total_actions_accepted", 0))
        + int(status.get("total_actions_refused", 0))
    )
    require(total_action_decisions >= 2,
            f"agent_action_decisions_not_recorded:{status}")
    threads = status.get("threads", [])
    require(isinstance(threads, list) and len(threads) == worker_count,
            "agent_runtime_thread_records_mismatch")
    by_agent = {thread.get("agent_type_id"): thread for thread in threads}
    require("page_allocation_manager" in by_agent,
            "page_allocation_manager_worker_missing")
    require("filespace_capacity_manager" in by_agent,
            "filespace_capacity_manager_worker_missing")
    page_decisions = (
        int(by_agent["page_allocation_manager"].get("actions_accepted", 0))
        + int(by_agent["page_allocation_manager"].get("actions_refused", 0))
    )
    filespace_decisions = (
        int(by_agent["filespace_capacity_manager"].get("actions_accepted", 0))
        + int(by_agent["filespace_capacity_manager"].get("actions_refused", 0))
    )
    require(page_decisions >= 1,
            f"page_allocator_action_decision_missing:{by_agent['page_allocation_manager']}")
    require(filespace_decisions >= 1,
            f"filespace_capacity_action_decision_missing:{by_agent['filespace_capacity_manager']}")


def run_client_pressure(args: argparse.Namespace, endpoint: Path, seconds: float = 2.0) -> None:
    deadline = time.monotonic() + seconds
    iteration = 0
    while time.monotonic() < deadline:
        live.run_ipc(args, endpoint, "database_status", f"pressure_{iteration}")
        iteration += 1
    require(iteration > 0, "client_pressure_did_not_run")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--ipc-tester", required=True)
    parser.add_argument("--work-dir", required=True)
    parsed = parser.parse_args()
    parsed.work = live.make_work_dir(Path(parsed.work_dir))

    server: subprocess.Popen[bytes] | None = None
    try:
        database = parsed.work / "agent-thread-runtime.sbdb"
        live.write_live_auth_fixture(database)
        control_dir = parsed.work / "sc"
        endpoint = control_dir / "s.sock"
        server = live.start_server(
            parsed, database, control_dir, parsed.work / "sr", endpoint, "agent_threads", True
        )
        status_path = control_dir / "sb_server.agent_runtime.json"
        pre_client = wait_for_status(
            status_path,
            lambda status: (
                int(status.get("total_actions_accepted", 0))
                + int(status.get("total_actions_refused", 0))
            ) >= 2
            and has_staggered_worker_progress(status),
            "pre_client_agent_actions",
            timeout=20.0,
        )
        assert_agent_action_threads(pre_client)
        assert_process_threads(server.pid, pre_client)

        before_ticks = int(pre_client.get("total_worker_ticks", 0))
        run_client_pressure(parsed, endpoint)
        during_client = wait_for_status(
            status_path,
            lambda status: int(status.get("total_worker_ticks", 0)) > before_ticks
            and has_staggered_worker_progress(status),
            "agent_ticks_during_client_pressure",
            timeout=10.0,
        )
        assert_agent_action_threads(during_client)
        assert_process_threads(server.pid, during_client)

        live.stop_server_via_ipc(parsed, endpoint, server, "agent_threads")
        server = None
        print(f"server_agent_thread_runtime_gate=passed work={parsed.work}")
        shutil.rmtree(parsed.work, ignore_errors=True)
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest needs concrete failure plus logs.
        print(f"server_agent_thread_runtime_gate=failed work={parsed.work}: {exc}", file=sys.stderr)
        status_files = sorted(parsed.work.glob("**/sb_server.agent_runtime.json"))
        for path in status_files:
            print(f"--- {path} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        live.dump_logs(parsed.work)
        return 1
    finally:
        live.stop_process(server)


if __name__ == "__main__":
    raise SystemExit(main())
