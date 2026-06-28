#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the driver-native full-surface execution matrix plan."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any


GATE_INPUT_REL = Path("project/tests/conformance/drivers/native_full_surface_gate_input.json")
TOOL_MATRIX_REL = Path("project/tests/conformance/drivers/native_tool_matrix.json")
MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
REPORT_REL = Path("build/reports/driver_native_matrix_plan_gate.json")

RELEASE_BUCKETS = {"release_candidate", "release_supported", "supported"}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[4]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def release_driver_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [
        row for row in rows
        if row.get("category") == "driver"
        and row.get("release_bucket") in RELEASE_BUCKETS
        and row.get("driver_status") != "planned_not_implemented"
    ]


def namespace_for(driver: str, run_id: str, route: str, page_size: str, worker: str) -> str:
    clean_route = route.replace("-", "_")
    return f"users.public.examples.{driver}.{run_id}.{clean_route}.{page_size}.{worker}"


def route_transport_variants(routes: list[str]) -> list[dict[str, str]]:
    variants: list[dict[str, str]] = []
    for route in routes:
        if route == "embedded":
            variants.append({
                "route": route,
                "sslmode": "disable",
                "transport_mode": "embedded_no_network_transport",
            })
        elif route == "ipc_local":
            variants.append({
                "route": route,
                "sslmode": "disable",
                "transport_mode": "local_ipc_no_tls",
            })
        elif route in {"listener-parser", "manager-listener-parser"}:
            variants.append({
                "route": route,
                "sslmode": "require",
                "transport_mode": "tls_required",
            })
            variants.append({
                "route": route,
                "sslmode": "disable",
                "transport_mode": "tls_disabled",
            })
        else:
            variants.append({
                "route": route,
                "sslmode": "unknown",
                "transport_mode": "unknown",
            })
    return variants


def capability_for(tool_matrix: dict[str, Any], driver: str) -> dict[str, Any]:
    caps = tool_matrix.get("transport_capability_by_driver", {})
    if isinstance(caps, dict):
        item = caps.get(driver, {})
        if isinstance(item, dict):
            return item
    return {}


def route_supported_for_driver(route: str, caps: dict[str, Any]) -> bool:
    if route in {"listener-parser", "manager-listener-parser"}:
        return caps.get("inet_required") is True
    if route == "ipc_local":
        return caps.get("native_ipc_supported") is True
    if route == "embedded":
        return caps.get("embedded_supported") is True
    return False


def validate_and_plan(repo_root: Path) -> tuple[list[str], dict[str, Any]]:
    gate_input = load_json(repo_root / GATE_INPUT_REL)
    tool_matrix = load_json(repo_root / TOOL_MATRIX_REL)
    manifest_rows = read_csv(repo_root / MANIFEST_REL)

    drivers = [row["name"] for row in release_driver_rows(manifest_rows)]
    matrix_drivers = {
        str(item.get("driver")) for item in as_list(tool_matrix.get("driver_tools"))
        if isinstance(item, dict)
    }
    routes = [str(value) for value in as_list(gate_input.get("required_routes"))]
    route_pairs = route_transport_variants(routes)
    page_sizes = [str(value) for value in as_list(gate_input.get("required_page_sizes"))]
    parser_modes = [str(value) for value in as_list(gate_input.get("required_parser_modes"))]
    concurrency_modes = [str(value) for value in as_list(gate_input.get("required_concurrency_modes"))]

    errors: list[str] = []
    for driver in drivers:
        if driver not in matrix_drivers:
            errors.append(f"matrix_plan:{driver}:missing_native_tool_matrix_entry")
        caps = capability_for(tool_matrix, driver)
        if caps.get("inet_required") is not True:
            errors.append(f"matrix_plan:{driver}:inet_support_must_be_required")
        if caps.get("embedded_supported") is True and not str(caps.get("embedded_boundary", "")).endswith("library") and "cpp" not in str(caps.get("embedded_boundary", "")):
            errors.append(f"matrix_plan:{driver}:embedded_requires_cpp_library_boundary")
    for axis, values in (
        ("routes", routes),
        ("page_sizes", page_sizes),
        ("parser_modes", parser_modes),
        ("concurrency_modes", concurrency_modes),
    ):
        if not values:
            errors.append(f"matrix_plan:{axis}:empty")

    examples: list[dict[str, str]] = []
    combination_count = 0
    for driver in drivers:
        caps = capability_for(tool_matrix, driver)
        for route_pair in route_pairs:
            if not route_supported_for_driver(route_pair["route"], caps):
                continue
            for page_size in page_sizes:
                for parser_mode in parser_modes:
                    for concurrency_mode in concurrency_modes:
                        combination_count += 1
                        if len(examples) < 20:
                            examples.append({
                                "driver": driver,
                                "route": route_pair["route"],
                                "sslmode": route_pair["sslmode"],
                                "transport_mode": route_pair["transport_mode"],
                                "page_size": page_size,
                                "parser_mode": parser_mode,
                                "concurrency_mode": concurrency_mode,
                                "namespace": namespace_for(driver, "RUNID", route_pair["route"], page_size, "w0"),
                            })

    required_namespace_parts = ("users", "public", "examples")
    for example in examples:
        namespace = example["namespace"]
        if not namespace.startswith(".".join(required_namespace_parts) + "."):
            errors.append(f"matrix_plan:namespace_prefix_invalid:{namespace}")
        if " " in namespace or ".." in namespace:
            errors.append(f"matrix_plan:namespace_invalid:{namespace}")

    report = {
        "command": "driver_native_matrix_plan_gate.py",
        "status": "pass" if not errors else "fail",
        "driver_count": len(drivers),
        "drivers": drivers,
        "routes": routes,
        "transport_capability_by_driver": {
            driver: capability_for(tool_matrix, driver) for driver in drivers
        },
        "route_transport_variants": route_pairs,
        "page_sizes": page_sizes,
        "parser_modes": parser_modes,
        "concurrency_modes": concurrency_modes,
        "combination_count": combination_count,
        "example_combinations": examples,
        "issues": errors,
    }
    return errors, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    try:
        errors, report = validate_and_plan(repo_root)
    except (OSError, json.JSONDecodeError, KeyError) as exc:
        errors = [f"matrix_plan:load_failed:{exc}"]
        report = {
            "command": "driver_native_matrix_plan_gate.py",
            "status": "fail",
            "issues": errors,
        }

    output = args.output or repo_root / REPORT_REL
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(
        f"driver_native_matrix_plan_gate: OK "
        f"drivers={report['driver_count']} combinations={report['combination_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
