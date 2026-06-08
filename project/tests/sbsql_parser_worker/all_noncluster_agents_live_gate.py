#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""PFAR-019A live coverage for all non-cluster runtime agents."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import live_server_agent_storage_benchmark_gate as live


@dataclass(frozen=True)
class AgentDescriptor:
    agent_type_id: str
    deployment: str
    scope: str
    authority: str
    default_activation: str


EXPECTED_SELECTED_DATABASE_AGENTS = {
    "metrics_registry_manager",
    "storage_health_manager",
    "filespace_capacity_manager",
    "page_allocation_manager",
    "memory_governor",
    "index_health_manager",
    "admission_control_manager",
    "transaction_pressure_manager",
    "cleanup_archive_manager",
    "policy_recommendation_manager",
    "runtime_learning_agent",
    "support_bundle_triage_agent",
    "backup_manager",
    "archive_manager",
    "pitr_manager",
    "alert_manager",
}

EXPECTED_VISIBLE_NOT_SELECTED = {
    "node_resource_agent": "node_scope_not_database_applicable",
    "parser_interface_manager": "parser_interface_scope_not_database_applicable",
    "job_control_manager": "baseline_operator_only",
    "restore_drill_manager": "baseline_operator_only",
    "identity_manager": "baseline_operator_only",
    "session_control_manager": "baseline_operator_only",
    "export_adapter_manager": "baseline_disabled",
}

EXPECTED_NOT_DATABASE_APPLICABLE = {
    "node_resource_agent",
    "parser_interface_manager",
}

EXPECTED_OPERATOR_ONLY = {
    "job_control_manager",
    "restore_drill_manager",
    "identity_manager",
    "session_control_manager",
}

EXPECTED_POLICY_DISABLED = {
    "export_adapter_manager",
}

STORAGE_AGENT_TYPES = {
    "storage_health_manager",
    "filespace_capacity_manager",
    "page_allocation_manager",
}


class GateError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def parse_registry(source: Path) -> dict[str, AgentDescriptor]:
    text = source.read_text(encoding="utf-8")
    pattern = re.compile(
        r'Agent\("([^"]+)",\s*AgentDeployment::(local|both|cluster),\s*"([^"]+)",\s*'
        r'AgentAuthorityClass::([a-z_]+),\s*AgentActivationProfile::([a-z_]+),'
    )
    descriptors: dict[str, AgentDescriptor] = {}
    for match in pattern.finditer(text):
        descriptor = AgentDescriptor(
            agent_type_id=match.group(1),
            deployment=match.group(2),
            scope=match.group(3),
            authority=match.group(4),
            default_activation=match.group(5),
        )
        descriptors[descriptor.agent_type_id] = descriptor
    require(descriptors, "canonical_agent_registry_not_parsed")
    return descriptors


def parse_show_agents_extended(output: str) -> dict[str, dict[str, str]]:
    rows: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        if not line.strip():
            continue
        fields = line.strip().split("|")
        require(len(fields) == 6, f"show_agents_extended_bad_row:{line}")
        rows[fields[0]] = {
            "deployment": fields[1],
            "scope": fields[2],
            "authority": fields[3],
            "cluster_only": fields[4],
            "state": fields[5],
        }
    require(rows, "show_agents_extended_empty")
    return rows


def run_sb_isql(args: argparse.Namespace,
                database: Path,
                port: int,
                sql: str,
                principal: str = "alice") -> str:
    evidence = f"scheme=local_password_v1;principal={principal};verifier={live.VERIFIER}"
    completed = subprocess.run(
        [
            args.sb_isql,
            str(database),
            "--host=127.0.0.1",
            f"--port={port}",
            "--sslmode=disable",
            "-U",
            principal,
            "-P",
            evidence,
            "-q",
            "-A",
            "-t",
            "-c",
            sql,
        ],
        stdout=(args.work / f"{sql.lower().replace(' ', '_')}.out").open("wb"),
        stderr=(args.work / f"{sql.lower().replace(' ', '_')}.err").open("wb"),
        check=False,
    )
    require(completed.returncode == 0, f"sb_isql_failed:{sql}:{completed.returncode}")
    return (args.work / f"{sql.lower().replace(' ', '_')}.out").read_text(
        encoding="utf-8", errors="replace")


def start_listener(args: argparse.Namespace, database: Path, endpoint: Path) -> tuple[subprocess.Popen[bytes], int]:
    listener_control = args.work / "lc"
    listener_runtime = args.work / "lr"
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
            f"--control-dir={listener_control}",
            f"--runtime-dir={listener_runtime}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
        ],
        stdout=(args.work / "listener.out").open("wb"),
        stderr=(args.work / "listener.err").open("wb"),
    )
    live.wait_for_tcp(port)
    return listener, port


def assert_live_agent_coverage(registry: dict[str, AgentDescriptor],
                               show_rows: dict[str, dict[str, str]],
                               database: dict[str, Any]) -> None:
    noncluster = {
        agent_type_id: descriptor
        for agent_type_id, descriptor in registry.items()
        if descriptor.deployment in {"local", "both"}
    }
    require(noncluster, "no_noncluster_agents_in_registry")
    visible_noncluster = {
        agent_type_id
        for agent_type_id, row in show_rows.items()
        if row["deployment"] in {"local", "both"} and row["cluster_only"] == "false"
    }
    require(set(noncluster) == visible_noncluster,
            "live_show_agents_extended_noncluster_visibility_mismatch:"
            f"missing={sorted(set(noncluster) - visible_noncluster)} "
            f"extra={sorted(visible_noncluster - set(noncluster))}")

    for agent_type_id, descriptor in noncluster.items():
        row = show_rows[agent_type_id]
        require(row["deployment"] == descriptor.deployment,
                f"live_deployment_mismatch:{agent_type_id}")
        require(row["scope"] == descriptor.scope, f"live_scope_mismatch:{agent_type_id}")
        require(row["authority"] == descriptor.authority,
                f"live_authority_mismatch:{agent_type_id}")
        require(row["state"] == "inactive", f"unexpected_show_agent_state:{agent_type_id}:{row['state']}")

    selected = set(
        database.get("database_engine_agent_health", {})
        .get("database_engine_agent", {})
        .get("selected_agents", [])
    )
    require(EXPECTED_SELECTED_DATABASE_AGENTS <= selected,
            "live_selected_agents_missing:"
            f"{sorted(EXPECTED_SELECTED_DATABASE_AGENTS - selected)}")
    require(selected <= set(noncluster),
            f"live_selected_agents_include_nonlocal_or_cluster:{sorted(selected - set(noncluster))}")
    require(STORAGE_AGENT_TYPES <= selected, "live_storage_agents_not_all_selected")

    unselected = set(noncluster) - selected
    require(unselected == set(EXPECTED_VISIBLE_NOT_SELECTED),
            "unexpected_visible_noncluster_not_selected:"
            f"actual={sorted(unselected)} expected={sorted(EXPECTED_VISIBLE_NOT_SELECTED)}")

    health = database.get("database_engine_agent_health", {}).get("database_engine_agent", {})
    require(health, "live_agent_health_payload_missing")
    require(health.get("agent_state") == "active", "live_agent_health_not_active")
    require(int(health.get("health_generation", 0)) >= 1, "live_health_generation_missing")
    require(health.get("authority_boundary_valid") is True, "live_authority_boundary_invalid")
    require(health.get("cluster_paths_failed_closed") is True,
            "live_noncluster_cluster_paths_not_failed_closed")
    diagnostics = set(health.get("diagnostics", []))
    require("ENGINE.AGENT_LIFECYCLE_HEALTHY" in diagnostics,
            "live_agent_lifecycle_healthy_evidence_missing")

    tick_records = health.get("noncluster_tick_health_records", [])
    require(isinstance(tick_records, list), "live_tick_health_records_not_list")
    tick_by_agent = {record.get("agent_type_id"): record for record in tick_records}
    require(set(tick_by_agent) == set(noncluster),
            "live_tick_health_noncluster_set_mismatch:"
            f"missing={sorted(set(noncluster) - set(tick_by_agent))} "
            f"extra={sorted(set(tick_by_agent) - set(noncluster))}")

    selection_records = health.get("selection_evidence", [])
    require(isinstance(selection_records, list), "live_selection_evidence_not_list")
    selection_by_agent = {record.get("agent_type_id"): record for record in selection_records}
    require(set(noncluster) <= set(selection_by_agent),
            "live_selection_evidence_missing_noncluster:"
            f"{sorted(set(noncluster) - set(selection_by_agent))}")

    for agent_type_id, record in tick_by_agent.items():
        require(record.get("tick_produced") is True,
                f"live_tick_not_produced:{agent_type_id}")
        require(record.get("health_published") is True,
                f"live_health_not_published:{agent_type_id}")
        require(record.get("diagnostic_code"), f"live_tick_diagnostic_missing:{agent_type_id}")
        live.require_uuid(str(record.get("policy_uuid", "")),
                          f"live_policy_uuid_invalid:{agent_type_id}")
        live.require_uuid(str(record.get("health_evidence_uuid", "")),
                          f"live_health_evidence_uuid_invalid:{agent_type_id}")
        if record.get("action_evidence_required") is True:
            require(record.get("action_evidence_published") is True,
                    f"live_action_evidence_not_published:{agent_type_id}")
            live.require_uuid(str(record.get("action_evidence_uuid", "")),
                              f"live_action_evidence_uuid_invalid:{agent_type_id}")
        for field in ("policy_uuid", "health_evidence_uuid", "action_evidence_uuid"):
            value = str(record.get(field, ""))
            require(not value.startswith(("agent.", "policy.", "scope.", "database:", "engine-instance:")),
                    f"live_label_prefixed_uuid:{agent_type_id}:{field}:{value}")

    for agent_type_id in EXPECTED_SELECTED_DATABASE_AGENTS:
        decision = selection_by_agent[agent_type_id]
        require(decision.get("database_applicable") is True,
                f"live_selected_not_database_applicable:{agent_type_id}")
        require(decision.get("selected") is True,
                f"live_selected_agent_missing_selection_evidence:{agent_type_id}")
        require(decision.get("diagnostic_code") in {
            "ENGINE.AGENT_RUNTIME_MANAGER.SELECTED",
            "ENGINE.AGENT_RUNTIME_MANAGER.LOCAL_PROJECTION_SELECTED_CLUSTER_PATH_FAIL_CLOSED",
        }, f"live_selected_agent_bad_diagnostic:{agent_type_id}:{decision.get('diagnostic_code')}")

    for agent_type_id in EXPECTED_NOT_DATABASE_APPLICABLE:
        decision = selection_by_agent[agent_type_id]
        require(decision.get("database_applicable") is False,
                f"live_not_database_applicable_flag_missing:{agent_type_id}")
        require(decision.get("selected") is False,
                f"live_not_database_applicable_selected:{agent_type_id}")
        require(decision.get("diagnostic_code") ==
                "ENGINE.AGENT_RUNTIME_MANAGER.NOT_DATABASE_APPLICABLE",
                f"live_not_database_applicable_diagnostic_missing:{agent_type_id}")

    for agent_type_id in EXPECTED_OPERATOR_ONLY:
        decision = selection_by_agent[agent_type_id]
        record = tick_by_agent[agent_type_id]
        require(decision.get("policy_disabled") is True,
                f"live_operator_only_policy_disabled_selection_missing:{agent_type_id}")
        require(decision.get("operator_only") is True,
                f"live_operator_only_selection_flag_missing:{agent_type_id}")
        require(decision.get("diagnostic_code") ==
                "ENGINE.AGENT_RUNTIME_MANAGER.POLICY_DISABLED",
                f"live_operator_only_selection_diagnostic_missing:{agent_type_id}")
        require(record.get("tick_class") in {"manual_approval_operator_only", "failed_closed"},
                f"live_operator_only_tick_or_refusal_missing:{agent_type_id}:{record.get('tick_class')}")
        if record.get("tick_class") == "manual_approval_operator_only":
            require(record.get("manual_approval_required") is True,
                    f"live_operator_only_manual_approval_missing:{agent_type_id}")
            require(record.get("operator_only") is True,
                    f"live_operator_only_tick_flag_missing:{agent_type_id}")
        else:
            require(record.get("failed_closed") is True,
                    f"live_operator_only_refusal_flag_missing:{agent_type_id}")
        require(record.get("action_evidence_required") is True,
                f"live_operator_only_action_evidence_requirement_missing:{agent_type_id}")

    for agent_type_id in EXPECTED_POLICY_DISABLED:
        decision = selection_by_agent[agent_type_id]
        record = tick_by_agent[agent_type_id]
        require(decision.get("policy_disabled") is True,
                f"live_policy_disabled_selection_missing:{agent_type_id}")
        require(decision.get("diagnostic_code") ==
                "ENGINE.AGENT_RUNTIME_MANAGER.POLICY_DISABLED",
                f"live_policy_disabled_selection_diagnostic_missing:{agent_type_id}")
        require(record.get("tick_class") == "policy_disabled",
                f"live_policy_disabled_tick_class_missing:{agent_type_id}:{record.get('tick_class')}")
        require(record.get("policy_disabled") is True,
                f"live_policy_disabled_tick_flag_missing:{agent_type_id}")
        require(record.get("action_evidence_required") is True,
                f"live_policy_disabled_action_evidence_requirement_missing:{agent_type_id}")


def assert_metrics_and_support_evidence(args: argparse.Namespace, endpoint: Path) -> None:
    metrics = live.run_ipc(
        args,
        endpoint,
        "management_show_metrics",
        "all_noncluster",
        "--expect-payload-contains",
        "sys.metrics.server.management.request_total",
    )
    payload = str(metrics.get("payload", ""))
    require("sys.metrics.server.management.request_total" in payload,
            "live_metrics_management_counter_missing")
    require("sys.metrics.server.support_bundle.export_total" in payload,
            "live_support_bundle_metric_missing")

    support = live.run_ipc(
        args,
        endpoint,
        "management_export_support_bundle",
        "all_noncluster",
        "--principal",
        "sysdba",
        "--expect-payload-contains",
        "support_bundle",
    )
    support_payload = str(support.get("payload", ""))
    require("support_bundle_uuid" in support_payload, "live_support_bundle_uuid_missing")
    require("[path-redacted]" in support_payload, "live_support_bundle_redaction_missing")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--ipc-tester", required=True)
    parser.add_argument("--agent-runtime-source", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    args.work = live.make_work_dir(Path(args.work_dir))

    server: subprocess.Popen[bytes] | None = None
    listener: subprocess.Popen[bytes] | None = None
    try:
        registry = parse_registry(Path(args.agent_runtime_source))
        database_path = args.work / "all_noncluster_live.sbdb"
        Path(str(database_path) + ".sb.local_password_auth").write_text(
            f"alice\tlocal_password\t{live.VERIFIER}\n"
            f"sysdba\tlocal_password\t{live.VERIFIER}\n",
            encoding="utf-8",
        )
        endpoint = args.work / "sc" / "s.sock"
        server = live.start_server(
            args, database_path, args.work / "sc", args.work / "sr", endpoint, "all_noncluster", True
        )
        status = live.decode_payload_json(
            live.run_ipc(args, endpoint, "database_status", "all_noncluster"), "database_status")
        database = live.first_database(status)
        live.assert_agent_runtime(database, expect_created=True, expected_uuid=None)

        listener, port = start_listener(args, database_path, endpoint)
        show_output = run_sb_isql(args, database_path, port, "SHOW AGENTS EXTENDED")
        assert_live_agent_coverage(registry, parse_show_agents_extended(show_output), database)
        assert_metrics_and_support_evidence(args, endpoint)

        live.stop_server_via_ipc(args, endpoint, server, "all_noncluster")
        server = None
        print(f"all_noncluster_agents_live_gate=passed work={args.work}")
        shutil.rmtree(args.work, ignore_errors=True)
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive exact failure context.
        print(f"all_noncluster_agents_live_gate=failed work={args.work}: {exc}", file=sys.stderr)
        live.dump_logs(args.work)
        return 1
    finally:
        live.stop_process(listener)
        live.stop_process(server)


if __name__ == "__main__":
    raise SystemExit(main())
