#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate P4 server-route benchmark and performance-budget evidence."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SERVER_PACKET_ARTIFACT = "SERVER_VERIFICATION_PACKETS.json"
BENCHMARK_ARTIFACT = "FULL_ROUTE_BENCHMARK_EVIDENCE.json"
BUDGET_ARTIFACT = "PERFORMANCE_BUDGETS.json"

REQUIRED_ROUTE_NODES = {
    "listener",
    "parser_pool",
    "parser_worker",
    "parser_server_ipc",
    "sb_server",
    "engine_auth_policy",
    "engine_mga_transaction_inventory",
}
START_NODES = {"client_tool_driver", "client_tool_driver_or_adaptor", "client", "tool", "driver"}
TRANSPORT_NODES = {"sbwp_tls_transport", "admitted_ipc_transport"}
ALLOWED_TRANSPORTS = {"sbwp_tls", "admitted_ipc"}
DIRECT_ENGINE_MARKERS = {
    "direct_engine",
    "direct_engine_api",
    "embedded_engine",
    "engine_api_direct",
    "parser_only",
}
REQUIRED_THRESHOLD_KEYS = {
    "latency_p95_ms": "max",
    "throughput_rows_per_second": "min",
    "tls_handshake_p95_ms": "max",
    "prepared_statement_p95_ms": "max",
    "bulk_insert_rows_per_second": "min",
    "memory_rss_mb": "max",
}
REQUIRED_REGRESSION_KEYS = {
    "latency_regression_percent",
    "throughput_regression_percent",
    "tls_regression_percent",
    "prepared_regression_percent",
    "bulk_regression_percent",
    "memory_regression_percent",
}


@dataclass(frozen=True)
class Context:
    repo_root: Path
    project_root: Path
    execution_plan_root: Path

    @property
    def drivers_root(self) -> Path:
        return self.project_root / "drivers"

    @property
    def artifact_root(self) -> Path:
        return self.execution_plan_root / "artifacts"


@dataclass(frozen=True)
class Component:
    component_id: str
    category: str
    name: str
    conformance_ctest_label: str


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def render_path(path: Path, ctx: Context) -> str:
    try:
        return str(path.relative_to(ctx.repo_root))
    except ValueError:
        return str(path)


def is_empty(value: Any) -> bool:
    if value is None:
        return True
    if isinstance(value, str):
        return value.strip() == ""
    if isinstance(value, (list, tuple, set, dict)):
        return len(value) == 0
    return False


def is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def load_components(ctx: Context) -> dict[str, Component]:
    manifest = ctx.drivers_root / "DriverPackageManifest.csv"
    rows = read_csv(manifest)
    components: dict[str, Component] = {}
    for row in rows:
        component_id = row.get("component_id", "").strip()
        if not component_id:
            raise ValueError("DriverPackageManifest row missing component_id")
        if component_id in components:
            raise ValueError(f"DriverPackageManifest duplicate component_id: {component_id}")
        components[component_id] = Component(
            component_id=component_id,
            category=row.get("category", "").strip(),
            name=row.get("name", "").strip(),
            conformance_ctest_label=row.get("conformance_profile_ref", "").strip(),
        )
    return components


def indexed_records(records: Any, key: str, artifact_name: str) -> tuple[dict[str, dict[str, Any]], list[str]]:
    errors: list[str] = []
    if not isinstance(records, list):
        return {}, [f"{artifact_name} {key} must be a list"]
    indexed: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            errors.append(f"{artifact_name} {key}[{index}] must be an object")
            continue
        component_id = str(record.get("component_id", "")).strip()
        if not component_id:
            errors.append(f"{artifact_name} {key}[{index}] missing component_id")
            continue
        if component_id in indexed:
            errors.append(f"{artifact_name} duplicate component_id: {component_id}")
        indexed[component_id] = record
    return indexed, errors


def component_coverage_errors(
    records: dict[str, dict[str, Any]], components: dict[str, Component], artifact_name: str
) -> list[str]:
    missing = sorted(set(components) - set(records))
    unknown = sorted(set(records) - set(components))
    errors: list[str] = []
    if missing:
        errors.append(f"{artifact_name} missing components: {', '.join(missing)}")
    if unknown:
        errors.append(f"{artifact_name} unknown components: {', '.join(unknown)}")
    return errors


def ctest_label_errors(
    record: dict[str, Any],
    component: Component,
    dsr_label: str,
    artifact_name: str,
) -> list[str]:
    errors: list[str] = []
    if record.get("conformance_ctest_label") != component.conformance_ctest_label:
        errors.append(
            f"{artifact_name} {component.component_id} conformance_ctest_label must be "
            f"{component.conformance_ctest_label}"
        )
    labels = record.get("ctest_labels")
    if not isinstance(labels, list):
        return errors + [f"{artifact_name} {component.component_id} ctest_labels must be a list"]
    label_set = {str(label) for label in labels}
    for required in ("driver", "driver_lane_ctest_gate", "driver_server_reconciliation", dsr_label):
        if required not in label_set:
            errors.append(f"{artifact_name} {component.component_id} missing CTest label {required}")
    if component.conformance_ctest_label not in label_set:
        errors.append(
            f"{artifact_name} {component.component_id} missing conformance label "
            f"{component.conformance_ctest_label}"
        )
    return errors


def route_profile_errors(data: dict[str, Any], record: dict[str, Any], artifact_name: str) -> list[str]:
    route_profiles = data.get("route_profiles")
    if not isinstance(route_profiles, dict):
        return [f"{artifact_name} route_profiles must be an object"]
    route_profile_name = str(record.get("route_profile", "")).strip()
    if not route_profile_name:
        return [f"{artifact_name} {record.get('component_id', '<unknown>')} missing route_profile"]
    route = route_profiles.get(route_profile_name)
    if not isinstance(route, dict):
        return [f"{artifact_name} route_profile not found: {route_profile_name}"]

    errors: list[str] = []
    if route.get("direct_engine_path") is not False:
        errors.append(f"{artifact_name} route {route_profile_name} must set direct_engine_path=false")
    if route.get("transport") not in ALLOWED_TRANSPORTS:
        errors.append(
            f"{artifact_name} route {route_profile_name} transport must be sbwp_tls or admitted_ipc"
        )
    nodes = route.get("nodes")
    if not isinstance(nodes, list):
        return errors + [f"{artifact_name} route {route_profile_name} nodes must be a list"]
    normalized_nodes = {str(node).strip().lower() for node in nodes}
    if not normalized_nodes.intersection(START_NODES):
        errors.append(
            f"{artifact_name} route {route_profile_name} must start at a client/tool/driver surface"
        )
    missing = sorted(REQUIRED_ROUTE_NODES - normalized_nodes)
    if missing:
        errors.append(f"{artifact_name} route {route_profile_name} missing route nodes: {', '.join(missing)}")
    if not normalized_nodes.intersection(TRANSPORT_NODES):
        errors.append(
            f"{artifact_name} route {route_profile_name} must use SBWP/TLS or admitted IPC transport"
        )
    marker_hits = sorted(normalized_nodes.intersection(DIRECT_ENGINE_MARKERS))
    route_kind = str(route.get("route_kind", "")).lower()
    marker_hits.extend(marker for marker in DIRECT_ENGINE_MARKERS if marker in route_kind)
    if marker_hits:
        errors.append(
            f"{artifact_name} route {route_profile_name} contains direct-engine marker(s): "
            + ", ".join(sorted(set(marker_hits)))
        )
    return errors


def direct_engine_rejection_errors(data: dict[str, Any], record: dict[str, Any], artifact_name: str) -> list[str]:
    errors: list[str] = []
    policy = data.get("invalid_evidence_policy")
    if not isinstance(policy, dict):
        errors.append(f"{artifact_name} invalid_evidence_policy must be an object")
    elif policy.get("direct_engine_benchmark_evidence") != "rejected":
        errors.append(f"{artifact_name} must reject direct_engine_benchmark_evidence")
    if record.get("direct_engine_benchmark_evidence_accepted") is not False:
        errors.append(
            f"{artifact_name} {record.get('component_id', '<unknown>')} must set "
            "direct_engine_benchmark_evidence_accepted=false"
        )
    if record.get("direct_engine_benchmark_evidence") != "rejected":
        errors.append(
            f"{artifact_name} {record.get('component_id', '<unknown>')} must explicitly reject "
            "direct_engine_benchmark_evidence"
        )
    return errors


def validate_server_packets(ctx: Context, components: dict[str, Component]) -> int:
    artifact = ctx.artifact_root / SERVER_PACKET_ARTIFACT
    try:
        data = read_json(artifact)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return fail(str(exc))
    packets, errors = indexed_records(
        data.get("server_verification_packets"), "server_verification_packets", SERVER_PACKET_ARTIFACT
    )
    errors.extend(component_coverage_errors(packets, components, SERVER_PACKET_ARTIFACT))
    for component_id, packet in sorted(packets.items()):
        component = components.get(component_id)
        if component is None:
            continue
        if is_empty(packet.get("server_verification_packet_id")):
            errors.append(f"{SERVER_PACKET_ARTIFACT} {component_id} missing server_verification_packet_id")
        errors.extend(ctest_label_errors(packet, component, "DSR-040", SERVER_PACKET_ARTIFACT))
        errors.extend(route_profile_errors(data, packet, SERVER_PACKET_ARTIFACT))
    return print_result(errors, f"DSR-040 server verification packets ok: {len(packets)} packets")


def validate_benchmark_evidence(ctx: Context, components: dict[str, Component]) -> int:
    artifact = ctx.artifact_root / BENCHMARK_ARTIFACT
    try:
        data = read_json(artifact)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return fail(str(exc))
    evidence, errors = indexed_records(
        data.get("benchmark_evidence"), "benchmark_evidence", BENCHMARK_ARTIFACT
    )
    errors.extend(component_coverage_errors(evidence, components, BENCHMARK_ARTIFACT))
    for component_id, record in sorted(evidence.items()):
        component = components.get(component_id)
        if component is None:
            continue
        if is_empty(record.get("benchmark_evidence_id")):
            errors.append(f"{BENCHMARK_ARTIFACT} {component_id} missing benchmark_evidence_id")
        errors.extend(ctest_label_errors(record, component, "DSR-041", BENCHMARK_ARTIFACT))
        errors.extend(route_profile_errors(data, record, BENCHMARK_ARTIFACT))
        errors.extend(direct_engine_rejection_errors(data, record, BENCHMARK_ARTIFACT))
    return print_result(errors, f"DSR-041 full-route benchmark evidence ok: {len(evidence)} records")


def threshold_errors(profile_name: str, profile: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    thresholds = profile.get("thresholds")
    if not isinstance(thresholds, dict):
        return [f"{BUDGET_ARTIFACT} budget profile {profile_name} thresholds must be an object"]
    for threshold_name, comparator in REQUIRED_THRESHOLD_KEYS.items():
        threshold = thresholds.get(threshold_name)
        if not isinstance(threshold, dict):
            errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} missing {threshold_name}")
            continue
        if not is_number(threshold.get(comparator)):
            errors.append(
                f"{BUDGET_ARTIFACT} budget profile {profile_name} {threshold_name} "
                f"missing numeric {comparator}"
            )
        if is_empty(threshold.get("unit")):
            errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} {threshold_name} missing unit")
    return errors


def regression_policy_errors(profile_name: str, profile: dict[str, Any]) -> list[str]:
    policy = profile.get("regression_policy")
    if not isinstance(policy, dict):
        return [f"{BUDGET_ARTIFACT} budget profile {profile_name} regression_policy must be an object"]
    errors: list[str] = []
    if policy.get("fail_ctest_on_regression") is not True:
        errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} must fail CTest on regression")
    if policy.get("direct_engine_baseline_policy") != "reject":
        errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} must reject direct-engine baselines")
    if is_empty(policy.get("compare_against")):
        errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} missing compare_against")
    for key in sorted(REQUIRED_REGRESSION_KEYS):
        if not is_number(policy.get(key)):
            errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} missing numeric {key}")
    return errors


def validate_budgets(ctx: Context, components: dict[str, Component]) -> int:
    artifact = ctx.artifact_root / BUDGET_ARTIFACT
    try:
        data = read_json(artifact)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return fail(str(exc))
    budget_profiles = data.get("budget_profiles")
    errors: list[str] = []
    if not isinstance(budget_profiles, dict) or not budget_profiles:
        errors.append(f"{BUDGET_ARTIFACT} budget_profiles must be a non-empty object")
        budget_profiles = {}
    for profile_name, profile in budget_profiles.items():
        if not isinstance(profile, dict):
            errors.append(f"{BUDGET_ARTIFACT} budget profile {profile_name} must be an object")
            continue
        errors.extend(threshold_errors(str(profile_name), profile))
        errors.extend(regression_policy_errors(str(profile_name), profile))

    budgets, indexed_errors = indexed_records(data.get("component_budgets"), "component_budgets", BUDGET_ARTIFACT)
    errors.extend(indexed_errors)
    errors.extend(component_coverage_errors(budgets, components, BUDGET_ARTIFACT))
    for component_id, record in sorted(budgets.items()):
        component = components.get(component_id)
        if component is None:
            continue
        profile_name = str(record.get("budget_profile", "")).strip()
        if profile_name not in budget_profiles:
            errors.append(f"{BUDGET_ARTIFACT} {component_id} references unknown budget_profile {profile_name}")
        errors.extend(ctest_label_errors(record, component, "DSR-043", BUDGET_ARTIFACT))
        errors.extend(route_profile_errors(data, record, BUDGET_ARTIFACT))
        errors.extend(direct_engine_rejection_errors(data, record, BUDGET_ARTIFACT))
    return print_result(errors, f"DSR-043 performance budgets ok: {len(budgets)} component budgets")


def print_result(errors: list[str], success: str) -> int:
    if errors:
        print("failed: P4 route/performance gate errors:", file=sys.stderr)
        for error in errors[:200]:
            print(f"- {error}", file=sys.stderr)
        if len(errors) > 200:
            print(f"- ... {len(errors) - 200} additional errors", file=sys.stderr)
        return 1
    print(success)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", type=Path, required=True)
    parser.add_argument(
        "mode",
        choices=[
            "server-verification-packets",
            "full-route-benchmark-evidence",
            "performance-budgets",
            "all",
        ],
    )
    args = parser.parse_args()
    ctx = Context(
        repo_root=args.repo_root.resolve(),
        project_root=args.project_root.resolve(),
        execution_plan_root=args.execution_plan_root.resolve(),
    )
    try:
        components = load_components(ctx)
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    if args.mode == "server-verification-packets":
        return validate_server_packets(ctx, components)
    if args.mode == "full-route-benchmark-evidence":
        return validate_benchmark_evidence(ctx, components)
    if args.mode == "performance-budgets":
        return validate_budgets(ctx, components)

    results = [
        validate_server_packets(ctx, components),
        validate_benchmark_evidence(ctx, components),
        validate_budgets(ctx, components),
    ]
    return 1 if any(result != 0 for result in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
