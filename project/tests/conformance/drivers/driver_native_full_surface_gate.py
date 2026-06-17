#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail-closed gates for driver-native full-surface test tools.

The gate validates source test fixtures and native driver tool sources. It does
not accept static release manifests as proof that a driver works.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any


INPUT_REL = Path("project/tests/conformance/drivers/native_full_surface_gate_input.json")
TOOL_MATRIX_REL = Path("project/tests/conformance/drivers/native_tool_matrix.json")
MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
REPORT_REL = Path("build/reports/driver_native_full_surface_gate.json")

RELEASE_BUCKETS_REQUIRING_NATIVE_TOOL = {
    "release_candidate",
    "release_supported",
    "supported",
}

PLANNED_NOT_IMPLEMENTED_STATUSES = {
    "planned_not_implemented",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[4]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def validate_gate_input(doc: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    expected_page_sizes = ["4k", "8k", "16k", "32k", "64k", "128k"]
    expected_routes = ["embedded", "ipc_local", "listener-parser", "manager-listener-parser"]
    expected_transport_modes = ["tls_required", "tls_disabled"]
    expected_parser_modes = ["server-parser", "standalone-parser", "driver-sblr-uuid"]
    required_args = {
        "--database",
        "--host",
        "--port",
        "--user",
        "--password",
        "--role",
        "--sslmode",
        "--sslrootcert",
        "--sslcert",
        "--sslkey",
        "--route",
        "--parser-mode",
        "--page-size",
        "--namespace",
        "--input",
        "--output",
        "--error",
        "--diagnostics",
        "--metrics",
        "--transcript",
        "--summary",
        "--stop-on-error",
        "--expected-refusals",
        "--statement-timeout-ms",
        "--fetch-size",
        "--concurrency-worker",
        "--create-database",
        "--create-emulation-mode",
    }
    required_artifacts = {
        "command-events.jsonl",
        "summary.json",
        "diagnostics.jsonl",
        "wire-transcript.jsonl",
        "timing-groups.json",
        "result-digests.json",
        "metadata-snapshots.json",
        "process-metrics.jsonl",
        "security-refusals.json",
        "native-api-coverage.json",
        "code-example-review.json",
        "junit.xml",
        "stdout.log",
        "stderr.log",
    }
    if doc.get("schema_version") != 1:
        errors.append("gate_input:schema_version_must_be_1")
    if as_list(doc.get("required_page_sizes")) != expected_page_sizes:
        errors.append("gate_input:required_page_sizes_drift")
    if as_list(doc.get("required_routes")) != expected_routes:
        errors.append("gate_input:required_routes_drift")
    if as_list(doc.get("required_transport_modes")) != expected_transport_modes:
        errors.append("gate_input:required_transport_modes_drift")
    if as_list(doc.get("required_parser_modes")) != expected_parser_modes:
        errors.append("gate_input:required_parser_modes_drift")
    if set(as_list(doc.get("required_tool_arguments"))) != required_args:
        errors.append("gate_input:required_tool_arguments_drift")
    if set(as_list(doc.get("required_artifacts"))) != required_artifacts:
        errors.append("gate_input:required_artifacts_drift")
    authority = doc.get("server_authority", {})
    if not isinstance(authority, dict):
        errors.append("gate_input:server_authority_not_object")
    else:
        for key in (
            "driver_sblr_uuid_is_untrusted_hint",
            "server_revalidation_required",
            "mga_transaction_finality_engine_owned",
            "driver_or_parser_finality_forbidden",
        ):
            if authority.get(key) is not True:
                errors.append(f"gate_input:server_authority:{key}_must_be_true")
    if doc.get("fail_on_static_only_evidence") is not True:
        errors.append("gate_input:fail_on_static_only_evidence_must_be_true")
    return errors


def validate_tool_matrix(doc: dict[str, Any], manifest_rows: list[dict[str, str]], repo_root: Path) -> list[str]:
    errors: list[str] = []
    if doc.get("schema_version") != 1:
        errors.append("tool_matrix:schema_version_must_be_1")
    tools = [item for item in as_list(doc.get("driver_tools")) if isinstance(item, dict)]
    by_driver = {str(item.get("driver", "")): item for item in tools}
    if len(by_driver) != len(tools):
        errors.append("tool_matrix:duplicate_driver_entries")
    forbidden_tokens = [str(token) for token in as_list(doc.get("forbidden_tokens"))]
    driver_rows = [
        row for row in manifest_rows
        if row.get("category") == "driver"
    ]
    for row in driver_rows:
        name = row.get("name", "").strip()
        release_bucket = row.get("release_bucket", "").strip()
        driver_status = row.get("driver_status", "").strip()
        tool = by_driver.get(name)
        if tool is None:
            errors.append(f"tool_matrix:{name}:missing_tool_matrix_entry")
            continue
        path_text = str(tool.get("path", ""))
        if not path_text:
            errors.append(f"tool_matrix:{name}:missing_path")
            continue
        path = repo_root / path_text
        native_tokens = [str(token) for token in as_list(tool.get("native_tokens"))]
        if not native_tokens:
            errors.append(f"tool_matrix:{name}:missing_native_tokens")
        requires_tool = (
            release_bucket in RELEASE_BUCKETS_REQUIRING_NATIVE_TOOL
            and driver_status not in PLANNED_NOT_IMPLEMENTED_STATUSES
        )
        if not requires_tool:
            continue
        if not path.is_file():
            errors.append(f"tool_matrix:{name}:missing_native_tool:{path_text}")
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            errors.append(f"tool_matrix:{name}:cannot_read_native_tool:{exc}")
            continue
        missing_tokens = [token for token in native_tokens if token not in text]
        if missing_tokens:
            errors.append(f"tool_matrix:{name}:missing_native_api_tokens:{','.join(missing_tokens)}")
        banned = [token for token in forbidden_tokens if token in text]
        if banned:
            errors.append(f"tool_matrix:{name}:forbidden_static_or_shellout_tokens:{','.join(banned)}")
    return errors


def validate_artifacts(repo_root: Path, artifact_root: Path, gate_input: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if not artifact_root.exists():
        return [f"artifact_schema:missing_artifact_root:{artifact_root}"]
    required = [str(name) for name in as_list(gate_input.get("required_artifacts"))]
    run_dirs = [path for path in artifact_root.rglob("summary.json") if path.is_file()]
    if not run_dirs:
        return [f"artifact_schema:no_summary_json_under:{artifact_root}"]
    transport_by_driver: dict[str, set[str]] = {}
    sslmode_by_driver: dict[str, set[str]] = {}
    for summary_path in run_dirs:
        run_root = summary_path.parent
        try:
            rel = run_root.relative_to(repo_root)
            if not rel.parts or rel.parts[0] != "build":
                errors.append(f"artifact_schema:run_artifacts_outside_build:{run_root}")
        except ValueError:
            errors.append(f"artifact_schema:run_artifacts_outside_repo:{run_root}")
        for filename in required:
            if not (run_root / filename).is_file():
                errors.append(f"artifact_schema:{run_root}:missing:{filename}")
        try:
            summary = load_json(summary_path)
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"artifact_schema:{summary_path}:invalid_summary:{exc}")
            continue
        if summary.get("server_revalidation_required") is not True:
            errors.append(f"artifact_schema:{summary_path}:server_revalidation_required_not_true")
        if summary.get("driver_or_parser_finality") not in (False, "forbidden"):
            errors.append(f"artifact_schema:{summary_path}:driver_or_parser_finality_not_forbidden")
        process_metrics = summary.get("process_metrics")
        if not isinstance(process_metrics, dict) or not process_metrics:
            errors.append(f"artifact_schema:{summary_path}:missing_process_metrics")
        else:
            for role, metrics in process_metrics.items():
                if not isinstance(metrics, dict):
                    errors.append(f"artifact_schema:{summary_path}:process_metrics:{role}:not_object")
                    continue
                for key in ("max_rss_kb", "max_vsize_kb", "last_rss_kb", "last_vsize_kb"):
                    value = metrics.get(key)
                    if not isinstance(value, int) or value <= 0:
                        errors.append(f"artifact_schema:{summary_path}:process_metrics:{role}:{key}_invalid")
        driver_name = str(summary.get("driver_name") or run_root.name)
        transport_modes = set(str(value) for value in as_list(summary.get("transport_modes")))
        if summary.get("transport_mode") is not None:
            transport_modes.add(str(summary.get("transport_mode")))
        sslmodes = set(str(value) for value in as_list(summary.get("sslmodes")))
        if summary.get("sslmode") is not None:
            sslmodes.add(str(summary.get("sslmode")))
        if not transport_modes:
            errors.append(f"artifact_schema:{summary_path}:missing_transport_mode")
        if not sslmodes:
            errors.append(f"artifact_schema:{summary_path}:missing_sslmode")
        unknown_transport_modes = sorted(transport_modes - {"tls_required", "tls_disabled"})
        if unknown_transport_modes:
            errors.append(
                f"artifact_schema:{summary_path}:unknown_transport_modes:{','.join(unknown_transport_modes)}"
            )
        unknown_sslmodes = sorted(sslmodes - {"require", "disable"})
        if unknown_sslmodes:
            errors.append(f"artifact_schema:{summary_path}:unknown_sslmodes:{','.join(unknown_sslmodes)}")
        transport_by_driver.setdefault(driver_name, set()).update(transport_modes)
        sslmode_by_driver.setdefault(driver_name, set()).update(sslmodes)
    for driver_name, observed in sorted(transport_by_driver.items()):
        if not {"tls_required", "tls_disabled"}.issubset(observed):
            errors.append(
                f"artifact_schema:{driver_name}:missing_tls_and_non_tls_transport_modes"
            )
    for driver_name, observed in sorted(sslmode_by_driver.items()):
        if not {"require", "disable"}.issubset(observed):
            errors.append(f"artifact_schema:{driver_name}:missing_sslmode_require_and_disable")
    return errors


def write_report(path: Path, mode: str, errors: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "command": "driver_native_full_surface_gate.py",
        "mode": mode,
        "status": "fail" if errors else "pass",
        "issues": errors,
    }
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument(
        "--mode",
        choices=("gate-input", "tool-inventory", "artifact-schema", "all"),
        default="all",
    )
    parser.add_argument("--artifact-root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    errors: list[str] = []
    try:
        gate_input = load_json(repo_root / INPUT_REL)
    except (OSError, json.JSONDecodeError) as exc:
        gate_input = {}
        errors.append(f"gate_input:load_failed:{exc}")
    try:
        tool_matrix = load_json(repo_root / TOOL_MATRIX_REL)
    except (OSError, json.JSONDecodeError) as exc:
        tool_matrix = {}
        errors.append(f"tool_matrix:load_failed:{exc}")
    try:
        manifest_rows = read_csv(repo_root / MANIFEST_REL)
    except OSError as exc:
        manifest_rows = []
        errors.append(f"manifest:load_failed:{exc}")

    if args.mode in ("gate-input", "all"):
        errors.extend(validate_gate_input(gate_input))
    if args.mode in ("tool-inventory", "all"):
        errors.extend(validate_tool_matrix(tool_matrix, manifest_rows, repo_root))
    if args.mode in ("artifact-schema", "all"):
        artifact_root = (args.artifact_root or repo_root / "build" / "driver-conformance").resolve()
        errors.extend(validate_artifacts(repo_root, artifact_root, gate_input))

    output = args.output or repo_root / REPORT_REL
    write_report(output, args.mode, errors)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"driver_native_full_surface_gate {args.mode}: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
