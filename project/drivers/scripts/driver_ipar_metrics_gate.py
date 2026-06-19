#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run the IPAR proof gate against driver-emitted build artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


IPAR_GATE = Path("project/tests/conformance/drivers/full_surface_scripts/ipar_performance_proof_gate.py")
IPAR_ARTIFACT_NAMES = {
    "json": "ipar-metrics.json",
    "jsonl": "ipar-metrics.jsonl",
    "csv": "ipar-metrics.csv",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def child_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(repo_root / "build" / "pycache" / "driver_scripts")
    return env


def ensure_build_path(repo_root: Path, path: Path) -> Path:
    resolved = path.resolve()
    try:
        rel = resolved.relative_to(repo_root)
    except ValueError as exc:
        raise ValueError(f"IPAR metric artifact must be inside repository build/: {resolved}") from exc
    if not rel.parts or rel.parts[0] != "build":
        raise ValueError(f"IPAR metric artifact must be under build/: {resolved}")
    return resolved


def artifact_inputs(repo_root: Path, artifact_roots: list[Path], shapes: set[str]) -> list[Path]:
    inputs: list[Path] = []
    for root in artifact_roots:
        build_root = ensure_build_path(repo_root, root)
        for shape in ("json", "jsonl", "csv"):
            if shape not in shapes:
                continue
            candidate = build_root / IPAR_ARTIFACT_NAMES[shape]
            if candidate.is_file():
                inputs.append(candidate)
    return inputs


def missing_artifact_shapes(repo_root: Path, artifact_roots: list[Path], shapes: set[str]) -> list[str]:
    missing: list[str] = []
    for root in artifact_roots:
        build_root = ensure_build_path(repo_root, root)
        for shape in ("json", "jsonl", "csv"):
            if shape not in shapes:
                continue
            candidate = build_root / IPAR_ARTIFACT_NAMES[shape]
            if not candidate.is_file() or candidate.stat().st_size == 0:
                missing.append(str(candidate))
    return missing


def read_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path}: expected JSON object")
    return payload


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.strip()
        if not stripped:
            continue
        record = json.loads(stripped)
        if not isinstance(record, dict):
            raise ValueError(f"{path}:{line_number}: expected JSON object")
        records.append(record)
    return records


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return [dict(row) for row in csv.DictReader(handle)]


def validate_driver_route_artifacts(
    inputs: list[Path],
    target_scripts: list[str],
    require_tls: bool,
    require_non_tls: bool,
) -> list[str]:
    errors: list[str] = []
    json_payloads: list[dict[str, Any]] = []
    jsonl_records: list[dict[str, Any]] = []
    csv_rows: list[dict[str, str]] = []
    for path in inputs:
        try:
            if path.name == IPAR_ARTIFACT_NAMES["json"]:
                json_payloads.append(read_json(path))
            elif path.name == IPAR_ARTIFACT_NAMES["jsonl"]:
                jsonl_records.extend(read_jsonl(path))
            elif path.name == IPAR_ARTIFACT_NAMES["csv"]:
                csv_rows.extend(read_csv_rows(path))
        except (OSError, ValueError, json.JSONDecodeError, csv.Error) as exc:
            errors.append(f"driver_artifact:{path}:load_failed:{exc}")

    route_events: list[dict[str, Any]] = [
        record for record in jsonl_records if record.get("event") == "ipar_route_proof"
    ]
    for payload in json_payloads:
        contract = payload.get("artifact_contract", {})
        if not isinstance(contract, dict):
            errors.append("driver_artifact:json:artifact_contract_missing")
            continue
        if contract.get("engine_execution") != "sblr_uuid_only":
            errors.append("driver_artifact:json:engine_execution_not_sblr_uuid_only")
        if contract.get("engine_sql_text_execution") is not False:
            errors.append("driver_artifact:json:engine_sql_text_execution_must_be_false")
        if contract.get("parser_output_to_engine_required") is not True:
            errors.append("driver_artifact:json:parser_output_to_engine_required_missing")
        embedded_events = payload.get("route_proof_events", [])
        if isinstance(embedded_events, list):
            route_events.extend(item for item in embedded_events if isinstance(item, dict))

    present_scripts = {
        str(event.get("script_id"))
        for event in route_events
        if event.get("script_id")
    }
    for payload in json_payloads:
        target_metrics = payload.get("target_metrics", {})
        if isinstance(target_metrics, dict):
            present_scripts.update(str(key) for key in target_metrics if str(key).startswith("SBDFS-"))
    selected = set(target_scripts) if target_scripts else present_scripts

    if "SBDFS-059" in selected and not any(
        event.get("script_id") == "SBDFS-059"
        and event.get("copy_stream_used") is True
        and event.get("driver_payload_kind") == "copy_canonical_rows"
        and event.get("engine_payload_kind") == "canonical_rows"
        for event in route_events
    ):
        errors.append("driver_artifact:route_proof:SBDFS-059_copy_canonical_rows_missing")

    prepared_scripts = {"SBDFS-020", "SBDFS-130"} & selected
    if prepared_scripts and not any(
        event.get("script_id") in prepared_scripts
        and event.get("prepared_handle_used") is True
        and event.get("engine_sql_text_execution") is False
        for event in route_events
    ):
        errors.append("driver_artifact:route_proof:prepared_descriptor_handle_missing")

    if any(event.get("transport_mode") == "tls_disabled" for event in route_events):
        if not all(
            event.get("tls_policy") == "explicit_non_tls_test_route"
            for event in route_events
            if event.get("transport_mode") == "tls_disabled"
        ):
            errors.append("driver_artifact:route_proof:non_tls_without_explicit_policy")
    if require_tls and not any(event.get("transport_mode") == "tls_required" for event in route_events):
        errors.append("driver_artifact:route_proof:tls_required_route_missing")
    if require_non_tls and not any(
        event.get("transport_mode") == "tls_disabled"
        and event.get("tls_policy") == "explicit_non_tls_test_route"
        for event in route_events
    ):
        errors.append("driver_artifact:route_proof:explicit_non_tls_test_route_missing")

    if csv_rows:
        required_columns = {"script_id", "route", "parser_mode", "sslmode", "transport_mode"}
        if not required_columns.issubset(csv_rows[0].keys()):
            errors.append("driver_artifact:csv:required_route_columns_missing")
    elif any(path.name == IPAR_ARTIFACT_NAMES["csv"] for path in inputs):
        errors.append("driver_artifact:csv:no_metric_rows")
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--artifact-root", type=Path, action="append", default=[])
    parser.add_argument("--metrics-input", type=Path, action="append", default=[])
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--target-script", action="append", default=[])
    parser.add_argument("--allow-missing-telemetry", action="store_true")
    parser.add_argument("--require-tls-route-proof", action="store_true")
    parser.add_argument("--require-non-tls-route-proof", action="store_true")
    parser.add_argument(
        "--shape",
        choices=("json", "jsonl", "csv", "all"),
        default="json",
        help="Which sb_regress_cpp IPAR artifact shape to feed from --artifact-root.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    output_root = (
        args.output_root
        or repo_root / "build" / "ipar-performance-proof" / "driver-metrics-gate"
    ).resolve()
    report_path = (
        args.report
        or repo_root / "build" / "reports" / "driver_ipar_metrics_gate.json"
    ).resolve()

    try:
        ensure_build_path(repo_root, output_root)
        ensure_build_path(repo_root, report_path)
        shapes = {"json", "jsonl", "csv"} if args.shape == "all" else {args.shape}
        missing_shapes = missing_artifact_shapes(repo_root, args.artifact_root, shapes)
        if missing_shapes:
            raise ValueError(
                "IPAR artifact root missing required shape(s): " + ", ".join(missing_shapes)
            )
        inputs = artifact_inputs(repo_root, args.artifact_root, shapes)
        inputs.extend(ensure_build_path(repo_root, item) for item in args.metrics_input)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    preflight_errors = validate_driver_route_artifacts(
        inputs,
        args.target_script,
        args.require_tls_route_proof,
        args.require_non_tls_route_proof,
    )
    command = [
        sys.executable,
        str(IPAR_GATE),
        "--repo-root",
        str(repo_root),
        "--mode",
        "validate-artifacts",
        "--output-root",
        str(output_root),
    ]
    for path in inputs:
        command.extend(["--metrics-input", str(path)])
    for script_id in args.target_script:
        command.extend(["--target-script", script_id])
    if args.allow_missing_telemetry:
        command.append("--allow-missing-telemetry")

    if preflight_errors:
        result_returncode = 1
        result_stdout = "\n".join(preflight_errors) + "\n"
    else:
        result = subprocess.run(
            command,
            cwd=repo_root,
            env=child_env(repo_root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        result_returncode = result.returncode
        result_stdout = result.stdout
    report: dict[str, Any] = {
        "command": "driver_ipar_metrics_gate.py",
        "status": "pass" if result_returncode == 0 else "fail",
        "summary": {
            "artifact_roots": [str(path) for path in args.artifact_root],
            "metrics_inputs": [str(path) for path in inputs],
            "target_scripts": args.target_script,
            "allow_missing_telemetry": args.allow_missing_telemetry,
            "require_tls_route_proof": args.require_tls_route_proof,
            "require_non_tls_route_proof": args.require_non_tls_route_proof,
            "preflight_errors": preflight_errors,
            "gate_returncode": result_returncode,
        },
        "gate_command": command,
        "output_tail": result_stdout.splitlines()[-120:],
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_ipar_metrics_gate={report['status']}")
    return 0 if result_returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
