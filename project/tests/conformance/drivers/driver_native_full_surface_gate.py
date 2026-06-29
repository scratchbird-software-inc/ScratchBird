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
LANGUAGE_SURFACE_REL = Path("project/drivers/language/sbsql_language_surface_manifest.json")

RELEASE_BUCKETS_REQUIRING_NATIVE_TOOL = {
    "release_candidate",
    "release_supported",
    "supported",
}

PLANNED_NOT_IMPLEMENTED_STATUSES = {
    "planned_not_implemented",
}

ALLOWED_TRANSPORT_MODES = {
    "embedded_no_network_transport",
    "local_ipc_no_tls",
    "tls_disabled",
    "tls_required",
}

ALLOWED_SSLMODES = {
    "allow",
    "disable",
    "prefer",
    "require",
    "verify-ca",
    "verify-full",
}

NETWORK_ROUTES = {
    "listener-parser",
    "manager-listener-parser",
}

ROUTE_TRANSPORT_MODES = {
    "embedded": {"embedded_no_network_transport"},
    "ipc_local": {"local_ipc_no_tls"},
    "listener-parser": {"tls_disabled", "tls_required"},
    "manager-listener-parser": {"tls_disabled", "tls_required"},
}


def capability_for(tool_matrix: dict[str, Any], driver: str) -> dict[str, Any]:
    caps = tool_matrix.get("transport_capability_by_driver", {})
    if isinstance(caps, dict):
        item = caps.get(driver, {})
        if isinstance(item, dict):
            return item
    return {}


def route_supported_for_driver(route: str, caps: dict[str, Any]) -> bool:
    if route in NETWORK_ROUTES:
        return caps.get("inet_required") is True
    if route == "ipc_local":
        return caps.get("native_ipc_supported") is True
    if route == "embedded":
        return caps.get("embedded_supported") is True
    return False


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
        "--ipc-path",
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
        "--language-resource-pack",
        "--language-resource-identity",
        "--language-resource-hash",
        "--language-profile",
        "--syntax-profile",
        "--topology-profile",
        "--standard-english-fallback",
    }
    required_artifacts = {
        "command-events.jsonl",
        "summary.json",
        "diagnostics.jsonl",
        "wire-transcript.jsonl",
        "timing-groups.json",
        "result-digests.json",
        "metadata-snapshots.json",
        "route-environment.json",
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


def language_contract(repo_root: Path) -> dict[str, Any]:
    manifest = load_json(repo_root / LANGUAGE_SURFACE_REL)
    metadata = manifest.get("common_resource_pack_metadata")
    if not isinstance(metadata, dict):
        raise ValueError("language surface manifest missing common_resource_pack_metadata")
    return {
        "resource_identity": str(metadata.get("resource_identity", "")),
        "resource_hash": str(metadata.get("resource_pack_common_resource_hash", "")),
        "resource_pack_path": str(metadata.get("resource_pack_path", "")),
        "syntax_profile": "sbsql.v3",
        "topology_profile": "topology.sbsql.canonical.v1",
        "supported_exact_profiles": {
            str(profile)
            for profile in as_list(metadata.get("supported_exact_profiles"))
        },
    }


def argument_token_present(text: str, argument: str, tool: dict[str, Any]) -> bool:
    argument_tokens = tool.get("argument_tokens")
    if isinstance(argument_tokens, dict):
        explicit = argument_tokens.get(argument)
        if isinstance(explicit, str):
            return explicit in text
        if isinstance(explicit, list):
            return any(str(token) in text for token in explicit)
    name = argument[2:] if argument.startswith("--") else argument
    return any(
        token in text
        for token in (
            argument,
            f'"{argument}"',
            f"'{argument}'",
            f'"{name}"',
            f"'{name}'",
        )
    )


def validate_tool_matrix(
    doc: dict[str, Any],
    manifest_rows: list[dict[str, str]],
    gate_input: dict[str, Any],
    repo_root: Path,
) -> list[str]:
    errors: list[str] = []
    if doc.get("schema_version") != 1:
        errors.append("tool_matrix:schema_version_must_be_1")
    tools = [item for item in as_list(doc.get("driver_tools")) if isinstance(item, dict)]
    by_driver = {str(item.get("driver", "")): item for item in tools}
    capability_map = doc.get("transport_capability_by_driver", {})
    if not isinstance(capability_map, dict):
        errors.append("tool_matrix:missing_transport_capability_by_driver")
        capability_map = {}
    if len(by_driver) != len(tools):
        errors.append("tool_matrix:duplicate_driver_entries")
    forbidden_tokens = [str(token) for token in as_list(doc.get("forbidden_tokens"))]
    required_tool_arguments = [str(arg) for arg in as_list(gate_input.get("required_tool_arguments"))]
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
        caps = capability_for(doc, name)
        if release_bucket in RELEASE_BUCKETS_REQUIRING_NATIVE_TOOL and driver_status not in PLANNED_NOT_IMPLEMENTED_STATUSES:
            if caps.get("inet_required") is not True:
                errors.append(f"tool_matrix:{name}:inet_support_must_be_required")
            if caps.get("embedded_supported") is True:
                boundary = str(caps.get("embedded_boundary", ""))
                if "cpp" not in boundary.lower() and "c++" not in boundary.lower():
                    errors.append(f"tool_matrix:{name}:embedded_requires_cpp_library_boundary")
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
        missing_arguments = [
            argument for argument in required_tool_arguments
            if not argument_token_present(text, argument, tool)
        ]
        if missing_arguments:
            errors.append(f"tool_matrix:{name}:missing_required_tool_arguments:{','.join(missing_arguments)}")
        banned = [token for token in forbidden_tokens if token in text]
        if banned:
            errors.append(f"tool_matrix:{name}:forbidden_static_or_shellout_tokens:{','.join(banned)}")
    return errors


def validate_artifacts(
    repo_root: Path,
    artifact_root: Path,
    gate_input: dict[str, Any],
    *,
    require_complete_matrix: bool = False,
) -> list[str]:
    errors: list[str] = []
    try:
        language = language_contract(repo_root)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        language = {}
        errors.append(f"artifact_schema:language_contract_load_failed:{exc}")
    try:
        tool_matrix = load_json(repo_root / TOOL_MATRIX_REL)
    except (OSError, json.JSONDecodeError) as exc:
        tool_matrix = {}
        errors.append(f"artifact_schema:tool_matrix_load_failed:{exc}")
    if not artifact_root.exists():
        return [f"artifact_schema:missing_artifact_root:{artifact_root}"]
    required = [str(name) for name in as_list(gate_input.get("required_artifacts"))]
    run_dirs = [path for path in artifact_root.rglob("summary.json") if path.is_file()]
    if not run_dirs:
        return [f"artifact_schema:no_summary_json_under:{artifact_root}"]
    transport_by_driver: dict[str, set[str]] = {}
    network_sslmode_by_driver: dict[str, set[str]] = {}
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
        route_environment_path = run_root / "route-environment.json"
        try:
            route_environment = load_json(route_environment_path)
        except (OSError, json.JSONDecodeError) as exc:
            route_environment = {}
            errors.append(f"artifact_schema:{route_environment_path}:invalid_route_environment:{exc}")
        if route_environment:
            if route_environment.get("page_size") != summary.get("page_size"):
                errors.append(f"artifact_schema:{summary_path}:route_environment_page_size_mismatch")
            expected_page_size_bytes = route_environment.get("expected_page_size_bytes")
            actual_page_size_bytes = route_environment.get("actual_page_size_bytes")
            if not isinstance(expected_page_size_bytes, int) or expected_page_size_bytes <= 0:
                errors.append(f"artifact_schema:{summary_path}:route_environment_expected_page_size_invalid")
            if route_environment.get("route") in NETWORK_ROUTES | {"ipc_local"}:
                if route_environment.get("page_size_verification_status") != "pass":
                    errors.append(f"artifact_schema:{summary_path}:route_environment_page_size_not_verified")
                if actual_page_size_bytes != expected_page_size_bytes:
                    errors.append(f"artifact_schema:{summary_path}:route_environment_page_size_bytes_mismatch")
        if summary.get("language_resource_identity") != language.get("resource_identity"):
            errors.append(f"artifact_schema:{summary_path}:language_resource_identity_mismatch")
        if summary.get("language_resource_hash") != language.get("resource_hash"):
            errors.append(f"artifact_schema:{summary_path}:language_resource_hash_mismatch")
        if summary.get("language_resource_pack") != language.get("resource_pack_path"):
            errors.append(f"artifact_schema:{summary_path}:language_resource_pack_mismatch")
        if summary.get("syntax_profile") != language.get("syntax_profile"):
            errors.append(f"artifact_schema:{summary_path}:syntax_profile_mismatch")
        if summary.get("topology_profile") != language.get("topology_profile"):
            errors.append(f"artifact_schema:{summary_path}:topology_profile_mismatch")
        language_profile = str(summary.get("language_profile") or "")
        if language_profile not in language.get("supported_exact_profiles", set()):
            errors.append(f"artifact_schema:{summary_path}:language_profile_not_supported:{language_profile}")
        if summary.get("standard_english_fallback") is not True:
            errors.append(f"artifact_schema:{summary_path}:standard_english_fallback_not_true")
        if summary.get("language_resource_authority") != "shared_server_parser_resource_pack":
            errors.append(f"artifact_schema:{summary_path}:language_resource_authority_invalid")
        command_events = run_root / "command-events.jsonl"
        if command_events.is_file():
            try:
                first_event_line = next(
                    (line for line in command_events.read_text(encoding="utf-8").splitlines() if line.strip()),
                    "",
                )
                if first_event_line:
                    first_event = json.loads(first_event_line)
                    for key in (
                        "language_resource_identity",
                        "language_resource_hash",
                        "language_profile",
                        "syntax_profile",
                        "topology_profile",
                    ):
                        if first_event.get(key) != summary.get(key):
                            errors.append(f"artifact_schema:{summary_path}:command_event_{key}_mismatch")
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(f"artifact_schema:{command_events}:invalid_command_events:{exc}")
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
        route = str(summary.get("route") or "")
        caps = capability_for(tool_matrix, driver_name)
        if not route_supported_for_driver(route, caps):
            errors.append(
                f"artifact_schema:{summary_path}:route_not_supported_for_driver:"
                f"driver={driver_name}:route={route}"
            )
        endpoint_kind = str(summary.get("transport_endpoint_kind") or "")
        implementation = str(summary.get("driver_transport_implementation") or "")
        cpp_boundary = str(summary.get("cpp_library_boundary") or "")
        if not endpoint_kind:
            errors.append(f"artifact_schema:{summary_path}:missing_transport_endpoint_kind")
        if not implementation:
            errors.append(f"artifact_schema:{summary_path}:missing_driver_transport_implementation")
        if not cpp_boundary:
            errors.append(f"artifact_schema:{summary_path}:missing_cpp_library_boundary")
        unknown_transport_modes = sorted(transport_modes - ALLOWED_TRANSPORT_MODES)
        if unknown_transport_modes:
            errors.append(
                f"artifact_schema:{summary_path}:unknown_transport_modes:{','.join(unknown_transport_modes)}"
            )
        expected_for_route = ROUTE_TRANSPORT_MODES.get(route)
        if expected_for_route is None:
            errors.append(f"artifact_schema:{summary_path}:unknown_route:{route}")
        elif not transport_modes.issubset(expected_for_route):
            errors.append(
                f"artifact_schema:{summary_path}:transport_mode_route_mismatch:"
                f"route={route}:transport={','.join(sorted(transport_modes))}"
            )
        if route == "ipc_local" and endpoint_kind not in {"unix_domain_socket", "windows_named_pipe"}:
            errors.append(
                f"artifact_schema:{summary_path}:ipc_local_requires_native_ipc_endpoint:{endpoint_kind}"
            )
        if route in NETWORK_ROUTES and endpoint_kind != "tcp":
            errors.append(
                f"artifact_schema:{summary_path}:network_route_requires_tcp_endpoint:{endpoint_kind}"
            )
        if route == "embedded" and endpoint_kind != "embedded_bridge":
            errors.append(
                f"artifact_schema:{summary_path}:embedded_route_requires_embedded_bridge:{endpoint_kind}"
            )
        if route == "embedded" and caps.get("embedded_supported") is True:
            boundary = str(caps.get("embedded_boundary", ""))
            if "cpp" not in boundary.lower() and "c++" not in boundary.lower():
                errors.append(
                    f"artifact_schema:{summary_path}:embedded_route_requires_cpp_library_boundary:{boundary}"
                )
        unknown_sslmodes = sorted(sslmodes - ALLOWED_SSLMODES)
        if unknown_sslmodes:
            errors.append(f"artifact_schema:{summary_path}:unknown_sslmodes:{','.join(unknown_sslmodes)}")
        transport_by_driver.setdefault(driver_name, set()).update(transport_modes)
        if route in NETWORK_ROUTES:
            transport_by_driver.setdefault(f"{driver_name}:network", set()).update(transport_modes)
            network_sslmode_by_driver.setdefault(driver_name, set()).update(sslmodes)
    for driver_name, observed in sorted(transport_by_driver.items()):
        if not require_complete_matrix:
            continue
        if driver_name.endswith(":network"):
            if not {"tls_required", "tls_disabled"}.issubset(observed):
                errors.append(
                    f"artifact_schema:{driver_name[:-8]}:missing_tls_and_non_tls_network_transport_modes"
                )
            continue
        if observed.isdisjoint({"tls_required", "tls_disabled"}):
            continue
        if not {"tls_required", "tls_disabled"}.issubset(observed):
            errors.append(
                f"artifact_schema:{driver_name}:missing_tls_and_non_tls_transport_modes"
            )
    for driver_name, observed in sorted(network_sslmode_by_driver.items()):
        if not require_complete_matrix:
            continue
        if not {"require", "disable"}.issubset(observed):
            errors.append(f"artifact_schema:{driver_name}:missing_sslmode_require_and_disable")
    return errors


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
        encoding="utf-8",
    )


def generate_deterministic_artifact_fixture(artifact_root: Path, gate_input: dict[str, Any]) -> Path:
    run_root = artifact_root / "cpp" / "listener-parser" / "8k" / "server-parser" / "fixture-run"
    required = [str(name) for name in as_list(gate_input.get("required_artifacts"))]
    summary = {
        "driver_name": "cpp",
        "driver_or_parser_finality": "forbidden",
        "elapsed_ns": 1000,
        "failure_count": 0,
        "language_profile": "en-US",
        "language_resource_authority": "shared_server_parser_resource_pack",
        "language_resource_hash": "sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc",
        "language_resource_identity": "sbsql.common_resource_pack.v1",
        "language_resource_pack": "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack",
        "mga_authority": "engine",
        "namespace": "users.public.examples.cpp.fixture.listener-parser.8k.w0",
        "page_size": "8k",
        "parser_mode": "server-parser",
        "process_metrics": {
            "client": {
                "last_rss_kb": 1024,
                "last_vsize_kb": 4096,
                "max_rss_kb": 1024,
                "max_vsize_kb": 4096,
            },
            "server": {
                "last_rss_kb": 2048,
                "last_vsize_kb": 8192,
                "max_rss_kb": 2048,
                "max_vsize_kb": 8192,
            },
        },
        "route": "listener-parser",
        "run_id": "fixture-run",
        "server_revalidation_required": True,
        "sslmode": "require",
        "standard_english_fallback": True,
        "status": "pass",
        "syntax_profile": "sbsql.v3",
        "topology_profile": "topology.sbsql.canonical.v1",
        "transport_mode": "tls_required",
        "transport_endpoint_kind": "tcp",
        "driver_transport_implementation": "native_cpp_tcp",
        "cpp_library_boundary": "native_cpp_driver",
    }
    route_environment = {
        "actual_page_size_bytes": 8192,
        "expected_page_size_bytes": 8192,
        "page_size": "8k",
        "page_size_verification_source": "SHOW DATABASE",
        "page_size_verification_status": "pass",
        "route": "listener-parser",
        "run_id": "fixture-run",
    }
    command_event = {
        "actual_outcome": "success",
        "canonical_message_vector": [],
        "code_example_section": "fixture",
        "command_group": "query",
        "diagnostic_code": "",
        "driver_name": "cpp",
        "driver_version": "fixture",
        "elapsed_ns": 1000,
        "expected_outcome": "success",
        "fetch_batch_count": 1,
        "language_profile": summary["language_profile"],
        "language_resource_hash": summary["language_resource_hash"],
        "language_resource_identity": summary["language_resource_identity"],
        "language_resource_pack": summary["language_resource_pack"],
        "mga_authority": "engine",
        "namespace": summary["namespace"],
        "native_api_surface": "cpp",
        "page_size": "8k",
        "parser_mode": "server-parser",
        "result_digest": "sha256:fixture",
        "route": "listener-parser",
        "round_trip_count": 1,
        "row_count": 1,
        "run_id": "fixture-run",
        "script": "fixture.sbsql",
        "server_revalidation_state": "required",
        "sql_hash": "sha256:fixture",
        "sqlstate": "00000",
        "statement_id": "fixture.sbsql:1",
        "statement_index": 1,
        "standard_english_fallback": True,
        "syntax_profile": summary["syntax_profile"],
        "topology_profile": summary["topology_profile"],
        "transaction_id_observed": "fixture",
    }
    for filename in required:
        path = run_root / filename
        if filename.endswith(".jsonl"):
            if filename == "command-events.jsonl":
                write_jsonl(path, [command_event])
            elif filename == "process-metrics.jsonl":
                write_jsonl(path, [{"role": "client", "rss_kb": 1024, "vsize_kb": 4096}])
            else:
                write_jsonl(path, [{"fixture": True}])
        elif filename == "summary.json":
            write_json(path, summary)
        elif filename == "route-environment.json":
            write_json(path, route_environment)
        elif filename == "junit.xml":
            path.write_text(
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<testsuite name="driver-native-artifact-fixture" tests="1" failures="0">\n'
                '  <testcase classname="scratchbird.driver.fixture" name="fixture"/>\n'
                '</testsuite>\n',
                encoding="utf-8",
            )
        elif filename.endswith(".log"):
            path.write_text("fixture\n", encoding="utf-8")
        else:
            write_json(path, {"fixture": True})
    return run_root


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
    parser.add_argument("--require-complete-matrix", action="store_true")
    parser.add_argument("--generate-deterministic-fixture", action="store_true")
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
        errors.extend(validate_tool_matrix(tool_matrix, manifest_rows, gate_input, repo_root))
    if args.mode in ("artifact-schema", "all"):
        artifact_root = (args.artifact_root or repo_root / "build" / "driver-conformance").resolve()
        if args.generate_deterministic_fixture:
            generate_deterministic_artifact_fixture(artifact_root, gate_input)
        errors.extend(
            validate_artifacts(
                repo_root,
                artifact_root,
                gate_input,
                require_complete_matrix=args.require_complete_matrix,
            )
        )

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
