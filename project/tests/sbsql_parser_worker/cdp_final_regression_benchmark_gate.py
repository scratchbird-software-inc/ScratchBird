#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CDP-050 final standalone regression and benchmark package gate."""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "cdp.final_regression_benchmark_gate.v1"
GATE_NAME = "cdp_final_regression_benchmark_gate"
ROUTE_NAMES = {"embedded", "ipc", "inet", "local-ipc", "inet-listener"}
REQUIRED_BINARIES = ("sb_isql", "sb_server", "sb_listener", "sbp_sbsql")
FORBIDDEN_PATH_TOKENS = (
    "/".join(("docs", "execution-plans")),
    "/".join(("docs", "completed-execution-plans")),
)


class FinalGateError(RuntimeError):
    pass


@dataclass(frozen=True)
class Component:
    key: str
    script: str
    extra_args: tuple[str, ...] = ()


COMPONENTS = (
    Component(
        key="no_execution_plan_dependency",
        script="ctest_no_execution_plan_runtime_dependency_gate.py",
    ),
    Component(
        key="route_split_benchmark",
        script="cdp_route_split_benchmark_gate.py",
        extra_args=("--build-mode", "configured"),
    ),
    Component(
        key="security_api_abi",
        script="cdp_security_api_abi_gate.py",
    ),
    Component(
        key="config_defaults_rollback",
        script="cdp_config_defaults_rollback_gate.py",
    ),
    Component(
        key="profiler_evidence",
        script="cdp_profiler_evidence_gate.py",
        extra_args=("--build-mode", "configured"),
    ),
    Component(
        key="soak_leak_stability",
        script="cdp_soak_leak_stability_gate.py",
        extra_args=("--build-mode", "configured"),
    ),
)


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - convert to fail-closed gate detail.
        raise FinalGateError(f"malformed JSON artifact {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise FinalGateError(f"JSON artifact is not an object: {path}")
    return payload


def walk_strings(value: Any):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from walk_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk_strings(item)


def forbid_execution_plan_paths(value: Any, context: str) -> None:
    for text in walk_strings(value):
        normalized = text.replace("\\", "/")
        for token in FORBIDDEN_PATH_TOKENS:
            if token in normalized:
                raise FinalGateError(f"{context} contains runtime execution_plan path token {token}")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise FinalGateError(message)


def require_file(path: Path, label: str) -> None:
    require(path.exists(), f"missing {label}: {path}")
    require(path.is_file(), f"{label} is not a file: {path}")
    require(os.access(path, os.X_OK), f"{label} is not executable: {path}")


def make_work_root() -> Path:
    base = Path(tempfile.gettempdir()) / "cdp050"
    base.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="final_", dir=base))


def extract_output_json(stdout: str) -> Path | None:
    match = re.search(r"\boutput=([^\s]+\.json)\b", stdout)
    if not match:
        return None
    return Path(match.group(1)).resolve()


def cmake_cache_summary(build_root: Path) -> dict[str, Any]:
    cache = build_root / "CMakeCache.txt"
    summary: dict[str, Any] = {
        "cache_path": str(cache),
        "cache_present": cache.exists(),
        "build_type": None,
        "instrumentation_flags": {},
        "instrumentation_flag_types": {},
        "benchmark_clean_proven": False,
        "benchmark_clean_unproven_reasons": [],
    }
    if not cache.exists():
        return summary
    text = cache.read_text(encoding="utf-8", errors="replace")
    required_flags = (
        "SCRATCHBIRD_ENABLE_DEBUG_LOGS",
        "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
        "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE",
        "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
    )
    for line in text.splitlines():
        if line.startswith("CMAKE_BUILD_TYPE:"):
            summary["build_type"] = line.split("=", 1)[-1]
        for name in required_flags:
            prefix = f"{name}:"
            if line.startswith(prefix):
                key, value = line.split("=", 1)
                _, cache_type = key.split(":", 1)
                summary["instrumentation_flags"][name] = value
                summary["instrumentation_flag_types"][name] = cache_type
    flags = summary["instrumentation_flags"]
    flag_types = summary["instrumentation_flag_types"]
    reasons: list[str] = []
    if summary.get("build_type") != "Release":
        reasons.append("CMAKE_BUILD_TYPE is not Release")
    for name in required_flags:
        value = str(flags.get(name, "")).upper()
        cache_type = str(flag_types.get(name, ""))
        if name not in flags:
            reasons.append(f"{name} missing")
        elif cache_type == "UNINITIALIZED":
            reasons.append(f"{name} was not consumed by CMake")
        elif value not in {"OFF", "FALSE", "0"}:
            reasons.append(f"{name} is {flags[name]}")
    summary["benchmark_clean_unproven_reasons"] = reasons
    summary["benchmark_clean_proven"] = not reasons
    return summary


def diagnostic_policy(cache: dict[str, Any]) -> dict[str, Any]:
    return {
        "benchmark_clean_claim": bool(cache.get("benchmark_clean_proven")),
        "diagnostic_claim": not bool(cache.get("benchmark_clean_proven")),
        "policy": (
            "benchmark_clean_build_flags_proven"
            if cache.get("benchmark_clean_proven")
            else "diagnostic_or_configured_build_no_benchmark_clean_speed_claim"
        ),
        "component_gates_record_equivalence_and_diagnostic_evidence": True,
    }


def component_command(args: argparse.Namespace, component: Component, component_work: Path) -> list[str]:
    script = Path(args.repo_root) / "project" / "tests" / "sbsql_parser_worker" / component.script
    command = [sys.executable, str(script)]
    if component.key == "no_execution_plan_dependency":
        command.extend(["--repo-root", args.repo_root, "--build-root", args.build_root])
    else:
        if component.key in {"security_api_abi", "config_defaults_rollback"}:
            command.extend(["--repo-root", args.repo_root])
        command.extend(
            [
                "--server",
                args.server,
                "--listener",
                args.listener,
                "--parser-worker",
                args.parser_worker,
                "--sb-isql",
                args.sb_isql,
                "--work-dir",
                str(component_work),
            ]
        )
        command.extend(component.extra_args)
    return command


def run_component(args: argparse.Namespace, work_root: Path, component: Component) -> dict[str, Any]:
    component_work = work_root / "components" / component.key
    component_work.mkdir(parents=True, exist_ok=True)
    stdout_path = component_work / "component.stdout"
    stderr_path = component_work / "component.stderr"
    command = component_command(args, component, component_work)
    start = time.monotonic()
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    elapsed = time.monotonic() - start
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    artifact = extract_output_json(completed.stdout)
    record: dict[str, Any] = {
        "component": component.key,
        "script": component.script,
        "returncode": completed.returncode,
        "status": "passed" if completed.returncode == 0 else "failed",
        "elapsed_seconds": round(elapsed, 3),
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "json_artifact_path": str(artifact) if artifact else None,
    }
    if completed.returncode != 0:
        record["stderr_tail"] = completed.stderr[-4000:]
    if component.key != "no_execution_plan_dependency":
        require(artifact is not None, f"{component.key} did not report a JSON artifact path")
        require(artifact.exists(), f"{component.key} JSON artifact is missing: {artifact}")
        payload = read_json(artifact)
        validate_component_payload(component.key, payload)
        forbid_execution_plan_paths(payload, component.key)
        record["artifact_schema_version"] = payload.get("schema_version")
    return record


def collect_routes(payload: dict[str, Any]) -> set[str]:
    routes: set[str] = set()
    for key in ("routes", "route_scope"):
        value = payload.get(key)
        if isinstance(value, str):
            routes.update(value.split())
        elif isinstance(value, list):
            routes.update(str(item) for item in value)
    for key in ("benchmark_records", "records", "accepted", "refused", "live_route_evidence", "capabilities"):
        value = payload.get(key)
        if isinstance(value, list):
            for record in value:
                if isinstance(record, dict) and isinstance(record.get("route"), str):
                    routes.add(record["route"])
    return routes


def validate_authority_boundary(payload: dict[str, Any], component: str) -> None:
    boundary = payload.get("authority_boundary")
    if isinstance(boundary, dict):
        require(boundary.get("parser_finality_authority") is False, f"{component} parser finality authority drifted")
        require(boundary.get("reference_finality_authority") is False, f"{component} reference finality authority drifted")
        require(boundary.get("finality_visibility_authority") == "engine_mga", f"{component} engine MGA authority missing")
        if "reference_engine_execution" in boundary:
            require(boundary.get("reference_engine_execution") is False, f"{component} reference execution drifted")
        return
    found_parser_false = False
    found_reference_false = False
    for record in walk_dicts(payload):
        if record.get("parser_finality_authority") in (False, "false"):
            found_parser_false = True
        elif "parser_finality_authority" in record:
            raise FinalGateError(f"{component} parser finality authority drifted")
        if record.get("reference_finality_authority") in (False, "false"):
            found_reference_false = True
        elif "reference_finality_authority" in record:
            raise FinalGateError(f"{component} reference finality authority drifted")
    if found_parser_false != found_reference_false:
        raise FinalGateError(f"{component} exposed incomplete parser/reference finality evidence")


def walk_dicts(value: Any):
    if isinstance(value, dict):
        yield value
        for item in value.values():
            yield from walk_dicts(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk_dicts(item)


def validate_hash_or_live_evidence(component: str, payload: dict[str, Any]) -> None:
    has_hash = False
    for record in walk_dicts(payload):
        if any(key in record for key in ("result_hash", "result_hashes", "result_hash_matrix", "default_show_management_hash")):
            has_hash = True
            break
    has_live = any(key in payload for key in ("live_route_evidence", "accepted", "records", "benchmark_records"))
    require(has_hash or has_live, f"{component} missing lane/result hashes or live route evidence")


def validate_component_payload(component: str, payload: dict[str, Any]) -> None:
    require(isinstance(payload.get("schema_version"), str) and payload["schema_version"], f"{component} missing schema_version")
    routes = collect_routes(payload)
    require(len(routes & ROUTE_NAMES) >= 3, f"{component} route coverage mismatch: {sorted(routes)}")
    validate_hash_or_live_evidence(component, payload)
    validate_authority_boundary(payload, component)
    if component == "route_split_benchmark":
        lane_ids = payload.get("lane_ids")
        records = payload.get("benchmark_records")
        require(isinstance(lane_ids, list) and lane_ids, "route split lane_ids missing")
        require(isinstance(records, list) and records, "route split benchmark_records missing")
        for lane_id in lane_ids:
            hashes = {record.get("result_hash") for record in records if isinstance(record, dict) and record.get("lane_id") == lane_id}
            require(len(hashes) == 1 and None not in hashes, f"route split lane hash mismatch for {lane_id}: {sorted(str(h) for h in hashes)}")
    if component in {"profiler_evidence", "soak_leak_stability"}:
        require(payload.get("benchmark_clean_claim") is False, f"{component} incorrectly claims benchmark-clean speed evidence")


def build_summary(args: argparse.Namespace) -> dict[str, Any]:
    cache = cmake_cache_summary(Path(args.build_root))
    return {
        "repo_root": args.repo_root,
        "build_root": args.build_root,
        "binary_paths": {
            "sb_isql": args.sb_isql,
            "sb_server": args.server,
            "sb_listener": args.listener,
            "sbp_sbsql": args.parser_worker,
        },
        "python_version": sys.version,
        "python_executable": sys.executable,
        "platform": platform.platform(),
        "timestamp_utc": utc_timestamp(),
        "cmake_cache": cache,
        "diagnostic_vs_benchmark_clean_policy": diagnostic_policy(cache),
    }


def build_payload(args: argparse.Namespace, work_root: Path, records: list[dict[str, Any]], status: str) -> dict[str, Any]:
    payload = {
        "schema_version": SCHEMA_VERSION,
        "gate": GATE_NAME,
        "run_id": work_root.name,
        "timestamp_utc": utc_timestamp(),
        "status": status,
        "build_config_runtime_summary": build_summary(args),
        "component_results": records,
        "component_scope": [component.key for component in COMPONENTS],
        "runtime_dependency_policy": {
            "standalone_runtime": True,
            "execution_plan_artifacts_required": False,
            "docs_execution-plans_runtime_paths_allowed": False,
        },
        "authority_boundary": {
            "finality_visibility_authority": "engine_mga",
            "parser_finality_authority": False,
            "reference_finality_authority": False,
            "reference_engine_execution": False,
        },
        "evidence_package_json": str(work_root / "evidence" / "cdp-final-regression-benchmark-gate.json"),
    }
    forbid_execution_plan_paths(payload, GATE_NAME)
    return payload


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir")
    args = parser.parse_args(argv[1:])
    args.repo_root = str(Path(args.repo_root).resolve())
    args.build_root = str(Path(args.build_root).resolve())
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    work_root = make_work_root()
    evidence_path = work_root / "evidence" / "cdp-final-regression-benchmark-gate.json"
    records: list[dict[str, Any]] = []
    status = "failed"
    try:
        require_file(Path(args.sb_isql), "sb_isql")
        require_file(Path(args.server), "sb_server")
        require_file(Path(args.listener), "sb_listener")
        require_file(Path(args.parser_worker), "sbp_sbsql")
        for component in COMPONENTS:
            record = run_component(args, work_root, component)
            records.append(record)
            if record["status"] != "passed":
                raise FinalGateError(f"{component.key} failed")
        status = "passed"
        payload = build_payload(args, work_root, records, status)
        write_json(evidence_path, payload)
        print(f"{GATE_NAME}=passed output={evidence_path}")
        print("cdp050_components=" + ",".join(record["component"] for record in records))
        print("cdp050_policy=" + payload["build_config_runtime_summary"]["diagnostic_vs_benchmark_clean_policy"]["policy"])
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        payload = build_payload(args, work_root, records, status)
        payload["failure"] = str(exc)
        write_json(evidence_path, payload)
        print(f"{GATE_NAME}=failed output={evidence_path}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
