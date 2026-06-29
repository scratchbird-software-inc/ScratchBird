#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run the driver-native full-surface conformance matrix.

The runner compiles the shared SBDFS script corpus into a run-specific chain and
then invokes the selected driver's native tool with the common CLI contract. The
engine remains the MGA/SBLR authority: driver-local SBLR/UUID is only an
untrusted hint and every route is expected to produce server-side admission
evidence in the artifacts.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


GATE_INPUT_REL = Path("project/tests/conformance/drivers/native_full_surface_gate_input.json")
TOOL_MATRIX_REL = Path("project/tests/conformance/drivers/native_tool_matrix.json")
MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
COMPILE_SUITE_REL = Path(
    "project/tests/conformance/drivers/full_surface_scripts/compile_full_surface_script_suite.py"
)
ARTIFACT_GATE_REL = Path("project/tests/conformance/drivers/driver_native_full_surface_gate.py")
LANGUAGE_SURFACE_REL = Path("project/drivers/language/sbsql_language_surface_manifest.json")
DEFAULT_OUTPUT_REL = Path("build/reports/driver_native_full_surface_matrix.json")
DEFAULT_ARTIFACT_REL = Path("build/driver-conformance/native-full-surface")
DEFAULT_STAGED_BIN_REL = Path("build/output/linux/bin")
SBSQL_STAGED_REL = DEFAULT_STAGED_BIN_REL / "SBsql"
RELEASE_BUCKETS = {"release_candidate", "release_supported", "supported"}
STAGED_DRIVER_EXECUTABLES = {
    "cpp": "sb_isql_cpp",
    "dart": "sb_isql_dart",
    "dotnet": "sb_isql_dotnet",
    "elixir": "sb_isql_elixir",
    "go": "sb_isql_go",
    "jdbc": "sb_isql_jdbc",
    "mojo": "sb_isql_mojo",
    "node": "sb_isql_node",
    "odbc": "sb_isql_odbc",
    "pascal": "sb_isql_pascal",
    "php": "sb_isql_php",
    "python": "sb_isql_python",
    "r": "sb_isql_r",
    "ruby": "sb_isql_ruby",
    "rust": "sb_isql_rust",
    "swift": "sb_isql_swift",
}

PAGE_SIZE_BYTES = {
    "4k": 4096,
    "8k": 8192,
    "16k": 16384,
    "32k": 32768,
    "64k": 65536,
    "128k": 131072,
}


@dataclass(frozen=True)
class RouteVariant:
    route: str
    sslmode: str
    transport_mode: str


@dataclass(frozen=True)
class MatrixWorkItem:
    ordinal: int
    driver: str
    route_pair: RouteVariant
    page_size: str
    parser_mode: str
    concurrency_mode: str
    worker: str
    namespace: str
    run_root: Path
    base_command: list[str]
    cwd: Path
    capability: dict[str, Any]


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def optional_json(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    return load_json(path)


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def child_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(repo_root / "build" / "pycache" / "driver_native_matrix")
    return env


def release_drivers(rows: list[dict[str, str]]) -> list[str]:
    return [
        str(row.get("name", "")).strip()
        for row in rows
        if row.get("category") == "driver"
        and row.get("release_bucket") in RELEASE_BUCKETS
        and row.get("driver_status") != "planned_not_implemented"
    ]


def route_variants(routes: list[str]) -> list[RouteVariant]:
    variants: list[RouteVariant] = []
    for route in routes:
        if route == "embedded":
            variants.append(RouteVariant(route, "disable", "embedded_no_network_transport"))
        elif route == "ipc_local":
            variants.append(RouteVariant(route, "disable", "local_ipc_no_tls"))
        elif route in {"listener-parser", "manager-listener-parser"}:
            variants.append(RouteVariant(route, "require", "tls_required"))
            variants.append(RouteVariant(route, "disable", "tls_disabled"))
        else:
            raise ValueError(f"unknown route: {route}")
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


def tool_source_map(tool_matrix: dict[str, Any]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for item in as_list(tool_matrix.get("driver_tools")):
        if isinstance(item, dict):
            driver = str(item.get("driver", "")).strip()
            path = str(item.get("path", "")).strip()
            if driver and path:
                result[driver] = Path(path)
    return result


def default_language_contract(repo_root: Path) -> dict[str, str]:
    manifest = load_json(repo_root / LANGUAGE_SURFACE_REL)
    metadata = manifest.get("common_resource_pack_metadata")
    if not isinstance(metadata, dict):
        raise ValueError("language manifest missing common_resource_pack_metadata")
    return {
        "resource_pack": str(metadata.get("resource_pack_path", "")),
        "resource_identity": str(metadata.get("resource_identity", "")),
        "resource_hash": str(metadata.get("resource_pack_common_resource_hash", "")),
        "language_profile": "en-US",
        "syntax_profile": "sbsql.v3",
        "topology_profile": "topology.sbsql.canonical.v1",
    }


def staged_command_for_driver(repo_root: Path, driver: str) -> tuple[list[str], Path] | None:
    executable_name = STAGED_DRIVER_EXECUTABLES.get(driver)
    if not executable_name:
        return None
    executable = repo_root / DEFAULT_STAGED_BIN_REL / executable_name
    if executable.is_file() and os.access(executable, os.X_OK):
        return [str(executable)], repo_root
    return None


def command_for_driver(repo_root: Path, driver: str) -> tuple[list[str], Path]:
    staged = staged_command_for_driver(repo_root, driver)
    if staged is not None:
        return staged
    if driver == "cpp":
        return [str(repo_root / "build/output/linux/bin/sb_isql_cpp")], repo_root
    if driver == "odbc":
        return [str(repo_root / "build/output/linux/bin/sb_isql_odbc")], repo_root
    if driver == "jdbc":
        cp = repo_root / "project/drivers/driver/jdbc/build/classes/java/main"
        return ["java", "-cp", str(cp), "com.scratchbird.jdbc.tools.SBIsqlJdbc"], repo_root
    if driver == "dotnet":
        return [
            "dotnet",
            "run",
            "--project",
            str(repo_root / "project/drivers/driver/dotnet/tools/SBIsqlDotNet/SBIsqlDotNet.csproj"),
            "--",
        ], repo_root
    if driver == "python":
        return [sys.executable, str(repo_root / "project/drivers/driver/python/tools/sb_isql_python.py")], repo_root
    if driver == "go":
        return ["go", "run", "./cmd/sb-isql-go"], repo_root / "project/drivers/driver/go"
    if driver == "rust":
        return ["cargo", "run", "--quiet", "--bin", "sb_isql_rust", "--"], repo_root / "project/drivers/driver/rust"
    if driver == "node":
        return ["node", str(repo_root / "project/drivers/driver/node/dist/tools/sb-isql-node.js")], repo_root
    if driver == "php":
        return ["php", str(repo_root / "project/drivers/driver/php/tools/sb_isql_php.php")], repo_root
    if driver == "r":
        return ["Rscript", str(repo_root / "project/drivers/driver/r/tools/sb_isql_r.R")], repo_root
    if driver == "ruby":
        return ["ruby", str(repo_root / "project/drivers/driver/ruby/tools/sb_isql_ruby.rb")], repo_root
    if driver == "pascal":
        return [str(repo_root / "build/drivers/driver/pascal/bin/sb_isql_pascal")], repo_root
    if driver == "swift":
        return ["swift", "run", "SBIsqlSwift"], repo_root / "project/drivers/driver/swift"
    if driver == "dart":
        return ["dart", "run", "bin/sb_isql_dart.dart"], repo_root / "project/drivers/driver/dart"
    if driver == "elixir":
        return ["elixir", str(repo_root / "project/drivers/driver/elixir/tools/sb_isql_elixir.exs")], repo_root
    if driver == "mojo":
        return ["mojo", str(repo_root / "project/drivers/driver/mojo/tools/sb_isql_mojo.mojo")], repo_root
    raise KeyError(f"no native matrix command for driver: {driver}")


def namespace_for(
    driver: str,
    run_id: str,
    route: str,
    page_size: str,
    parser_mode: str,
    concurrency_mode: str,
    worker: str,
) -> str:
    def segment(value: str) -> str:
        cleaned = "".join(ch.lower() if ch.isalnum() else "_" for ch in value)
        cleaned = "_".join(part for part in cleaned.split("_") if part)
        if not cleaned:
            cleaned = "x"
        if not (cleaned[0].isalpha() or cleaned[0] == "_"):
            cleaned = f"p_{cleaned}"
        return cleaned

    return ".".join(
        [
            "users",
            "public",
            "examples",
            segment(driver),
            segment(run_id),
            segment(route),
            segment(page_size),
            segment(parser_mode),
            segment(concurrency_mode),
            segment(worker),
        ]
    )


def namespace_ancestor_schemas(namespace: str) -> list[str]:
    parts = [part for part in namespace.split(".") if part]
    if len(parts) < 4:
        return []
    start = 3 if parts[:2] == ["users", "public"] else 1
    return [".".join(parts[:end]) for end in range(start, len(parts))]


def run_command(command: list[str], cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def compile_suite(
    repo_root: Path,
    run_root: Path,
    *,
    driver: str,
    run_id: str,
    route: str,
    parser_mode: str,
    page_size: str,
    namespace: str,
) -> dict[str, Path]:
    compiled_root = run_root / "compiled"
    command = [
        sys.executable,
        str(repo_root / COMPILE_SUITE_REL),
        "--output-root",
        str(compiled_root),
        "--namespace-ancestor-mode",
        "external",
        "--namespace",
        namespace,
        "--driver",
        driver,
        "--run-id",
        run_id,
        "--route",
        route,
        "--parser-mode",
        parser_mode,
        "--page-size",
        page_size,
        "--artifact-root",
        str(run_root),
    ]
    result = run_command(command, repo_root, child_env(repo_root))
    if result.returncode != 0:
        raise RuntimeError("compile suite failed:\n" + "\n".join(result.stdout.splitlines()[-80:]))
    return {
        "input": compiled_root / "full_surface_chain.sbsql",
        "expected_refusals": compiled_root / "expected" / "expected_refusals.json",
        "manifest": compiled_root / "compiled_manifest.json",
    }


def sbsql_base_command(args: argparse.Namespace, repo_root: Path, route: str, sslmode: str) -> list[str]:
    executable = repo_root / SBSQL_STAGED_REL
    if not executable.is_file():
        raise FileNotFoundError(f"SBsql executable missing: {executable}")
    command = [str(executable), args.database]
    if route == "ipc_local":
        if not args.ipc_path:
            raise ValueError("ipc_local namespace bootstrap requires --ipc-path")
        command.extend(["--mode=local-ipc", f"--ipc-path={args.ipc_path}"])
    elif route in {"listener-parser", "manager-listener-parser"}:
        command.extend([
            f"--host={args.host}",
            f"--port={args.port}",
            f"--sslmode={sslmode}",
        ])
        if args.sslrootcert:
            command.extend(["--conn-opt", f"sslrootcert={args.sslrootcert}"])
        if args.sslcert:
            command.extend(["--conn-opt", f"sslcert={args.sslcert}"])
        if args.sslkey:
            command.extend(["--conn-opt", f"sslkey={args.sslkey}"])
    elif route == "embedded":
        command.extend(["--mode=embedded"])
    else:
        raise ValueError(f"unknown route for namespace bootstrap: {route}")
    if args.role:
        command.extend(["--conn-opt", f"role={args.role}"])
    command.extend(["-U", args.user, "-P", args.password, "-q", "-A", "-t"])
    return command


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def sbsql_scalar(
    args: argparse.Namespace,
    repo_root: Path,
    route: str,
    sslmode: str,
    sql: str,
) -> subprocess.CompletedProcess[str]:
    return run_command(
        [*sbsql_base_command(args, repo_root, route, sslmode), "-c", sql],
        repo_root,
        child_env(repo_root),
    )


def expected_page_size_bytes(page_size: str) -> int:
    try:
        return PAGE_SIZE_BYTES[page_size]
    except KeyError as exc:
        raise ValueError(f"unknown page size label: {page_size}") from exc


def schema_visible(
    args: argparse.Namespace,
    repo_root: Path,
    route: str,
    sslmode: str,
    schema: str,
) -> tuple[bool, str]:
    result = sbsql_scalar(
        args,
        repo_root,
        route,
        sslmode,
        f"SELECT COUNT(*) FROM sys.schemas WHERE schema_name = {sql_literal(schema)}",
    )
    output = result.stdout.strip()
    if result.returncode != 0:
        return False, output
    return output.splitlines()[-1].strip() == "1", output


def bootstrap_namespace_ancestors(
    args: argparse.Namespace,
    repo_root: Path,
    route: str,
    sslmode: str,
    namespace: str,
) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    if route == "embedded":
        return {
            "status": "skipped",
            "reason": "embedded route uses the driver's local create path",
            "schemas": [],
        }
    for schema in namespace_ancestor_schemas(namespace):
        create_sql = f"CREATE SCHEMA {schema}"
        create = sbsql_scalar(args, repo_root, route, sslmode, create_sql)
        visible, probe_output = schema_visible(args, repo_root, route, sslmode, schema)
        entry = {
            "schema": schema,
            "create_returncode": create.returncode,
            "create_output_tail": create.stdout.splitlines()[-20:],
            "visible_after_bootstrap": visible,
            "visibility_probe_output": probe_output,
        }
        if create.returncode == 0:
            entry["status"] = "created"
        elif visible:
            entry["status"] = "already_visible"
        else:
            entry["status"] = "failed"
            failures.append(entry)
        entries.append(entry)
        if failures:
            break
    return {
        "status": "pass" if not failures else "fail",
        "schemas": entries,
        "failures": failures,
    }


def artifact_paths(run_root: Path) -> dict[str, Path]:
    return {
        "output": run_root / "stdout.log",
        "error": run_root / "stderr.log",
        "diagnostics": run_root / "diagnostics.jsonl",
        "metrics": run_root / "process-metrics.jsonl",
        "transcript": run_root / "wire-transcript.jsonl",
        "summary": run_root / "summary.json",
    }


def existing_pass_entry(
    run_root: Path,
    entry: dict[str, Any],
    required_artifacts: list[str],
) -> dict[str, Any] | None:
    summary_path = run_root / "summary.json"
    if not summary_path.is_file():
        return None
    missing = [name for name in required_artifacts if not (run_root / name).is_file()]
    if missing:
        return None
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    if summary.get("status") != "pass":
        return None
    expected_page_size = entry.get("expected_page_size_bytes")
    actual_page_size = summary.get("actual_page_size_bytes")
    if actual_page_size is not None and actual_page_size != expected_page_size:
        return None
    route_environment_path = run_root / "route-environment.json"
    if route_environment_path.is_file():
        try:
            route_environment = json.loads(route_environment_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            return None
        if route_environment.get("page_size_verification_status") != "pass":
            return None
        if route_environment.get("expected_page_size_bytes") != expected_page_size:
            return None
        if route_environment.get("actual_page_size_bytes") != expected_page_size:
            return None
    resumed = dict(entry)
    resumed.update(
        {
            "status": "pass",
            "resumed_from_artifacts": True,
            "summary": str(summary_path),
            "returncode": 0,
        }
    )
    return resumed


def work_item_id(item: MatrixWorkItem) -> str:
    return "|".join(
        [
            str(item.ordinal),
            item.driver,
            item.route_pair.route,
            item.route_pair.sslmode,
            item.page_size,
            item.parser_mode,
            item.concurrency_mode,
        ]
    )


def base_entry_for_item(args: argparse.Namespace, item: MatrixWorkItem) -> dict[str, Any]:
    return {
        "combination_id": work_item_id(item),
        "combination_ordinal": item.ordinal,
        "driver": item.driver,
        "route": item.route_pair.route,
        "sslmode": item.route_pair.sslmode,
        "transport_mode": item.route_pair.transport_mode,
        "page_size": item.page_size,
        "expected_page_size_bytes": expected_page_size_bytes(item.page_size),
        "parser_mode": item.parser_mode,
        "concurrency_mode": item.concurrency_mode,
        "language_profile": args.language_profile,
        "language_resource_identity": args.language_resource_identity,
        "language_resource_hash": args.language_resource_hash,
        "namespace": item.namespace,
        "run_root": str(item.run_root),
        "command": [*item.base_command, "..."],
    }


def preflight_lane_manifest(
    lanes: dict[str, dict[str, Any]],
    route_pairs: list[RouteVariant],
    page_sizes: list[str],
) -> list[str]:
    errors: list[str] = []
    if not lanes:
        return errors
    expected_keys = {
        lane_key(route_pair.route, route_pair.sslmode, page_size)
        for route_pair in route_pairs
        for page_size in page_sizes
    }
    missing = sorted(expected_keys - set(lanes))
    errors.extend(f"lane_manifest:missing:{key}" for key in missing)
    seen_databases: dict[str, str] = {}
    for key, lane in sorted(lanes.items()):
        route, sslmode, page_size = key.split("|", 2)
        actual_page_size = lane.get("page_size_bytes")
        if actual_page_size is not None and int(actual_page_size) != expected_page_size_bytes(page_size):
            errors.append(f"lane_manifest:{key}:page_size_bytes_mismatch")
        database = str(lane.get("database", "")).strip()
        if not database:
            errors.append(f"lane_manifest:{key}:missing_database")
        elif database in seen_databases:
            errors.append(f"lane_manifest:{key}:duplicate_database_with:{seen_databases[database]}")
        else:
            seen_databases[database] = key
        if route == "ipc_local" and not str(lane.get("ipc_path", "")).strip():
            errors.append(f"lane_manifest:{key}:missing_ipc_path")
        if route in {"listener-parser", "manager-listener-parser"}:
            port = lane.get("port")
            try:
                port_int = int(port)
            except (TypeError, ValueError):
                errors.append(f"lane_manifest:{key}:invalid_port")
            else:
                if port_int <= 0:
                    errors.append(f"lane_manifest:{key}:invalid_port")
            if sslmode != "disable" and not str(lane.get("sslrootcert", "")).strip():
                errors.append(f"lane_manifest:{key}:missing_sslrootcert")
    return errors


LANE_OVERRIDE_FIELDS = {
    "database",
    "host",
    "port",
    "user",
    "password",
    "role",
    "sslrootcert",
    "sslcert",
    "sslkey",
    "ipc_path",
}


def lane_key(route: str, sslmode: str, page_size: str) -> str:
    return f"{route}|{sslmode}|{page_size}"


def index_lane_manifest(manifest: dict[str, Any] | None) -> dict[str, dict[str, Any]]:
    if manifest is None:
        return {}
    lanes = manifest.get("lanes")
    if not isinstance(lanes, list):
        raise ValueError("lane manifest must contain a lanes array")
    indexed: dict[str, dict[str, Any]] = {}
    for index, lane in enumerate(lanes):
        if not isinstance(lane, dict):
            raise ValueError(f"lane manifest entry {index} is not an object")
        route = str(lane.get("route", "")).strip()
        sslmode = str(lane.get("sslmode", "")).strip()
        page_size = str(lane.get("page_size", "")).strip()
        if not route or not sslmode or not page_size:
            raise ValueError(f"lane manifest entry {index} missing route, sslmode, or page_size")
        key = lane_key(route, sslmode, page_size)
        if key in indexed:
            raise ValueError(f"duplicate lane manifest entry: {key}")
        indexed[key] = lane
    return indexed


def effective_args_for_lane(
    args: argparse.Namespace,
    lanes: dict[str, dict[str, Any]],
    route: str,
    sslmode: str,
    page_size: str,
) -> argparse.Namespace:
    effective = argparse.Namespace(**vars(args))
    lane = lanes.get(lane_key(route, sslmode, page_size))
    if lane is None:
        return effective
    for field in LANE_OVERRIDE_FIELDS:
        if field not in lane:
            continue
        value = lane[field]
        if field == "port":
            setattr(effective, field, int(value))
        else:
            setattr(effective, field, str(value))
    return effective


def build_tool_args(
    args: argparse.Namespace,
    *,
    namespace: str,
    route: str,
    sslmode: str,
    parser_mode: str,
    page_size: str,
    run_root: Path,
    compiled: dict[str, Path],
    worker: str,
) -> list[str]:
    paths = artifact_paths(run_root)
    return [
        "--database", args.database,
        "--host", args.host,
        "--port", str(args.port),
        "--user", args.user,
        "--password", args.password,
        "--role", args.role,
        "--sslmode", sslmode,
        "--sslrootcert", args.sslrootcert,
        "--sslcert", args.sslcert,
        "--sslkey", args.sslkey,
        "--ipc-path", args.ipc_path,
        "--route", route,
        "--parser-mode", parser_mode,
        "--page-size", page_size,
        "--namespace", namespace,
        "--input", str(compiled["input"]),
        "--output", str(paths["output"]),
        "--error", str(paths["error"]),
        "--diagnostics", str(paths["diagnostics"]),
        "--metrics", str(paths["metrics"]),
        "--transcript", str(paths["transcript"]),
        "--summary", str(paths["summary"]),
        "--expected-refusals", str(compiled["expected_refusals"]),
        "--statement-timeout-ms", str(args.statement_timeout_ms),
        "--fetch-size", str(args.fetch_size),
        "--concurrency-worker", worker.removeprefix("w"),
        "--run-id", args.run_id,
        "--create-emulation-mode", args.create_emulation_mode,
        "--language-resource-pack", args.language_resource_pack,
        "--language-resource-identity", args.language_resource_identity,
        "--language-resource-hash", args.language_resource_hash,
        "--language-profile", args.language_profile,
        "--syntax-profile", args.syntax_profile,
        "--topology-profile", args.topology_profile,
        "--standard-english-fallback", "true" if args.standard_english_fallback else "false",
    ]


def execute_work_item(
    item: MatrixWorkItem,
    args: argparse.Namespace,
    repo_root: Path,
    lane_overrides: dict[str, dict[str, Any]],
    required_artifacts: list[str],
) -> dict[str, Any]:
    entry = base_entry_for_item(args, item)
    if args.resume_passing:
        resumed = existing_pass_entry(item.run_root, entry, required_artifacts)
        if resumed is not None:
            return resumed
    try:
        effective_args = effective_args_for_lane(
            args,
            lane_overrides,
            item.route_pair.route,
            item.route_pair.sslmode,
            item.page_size,
        )
        if lane_overrides and lane_key(item.route_pair.route, item.route_pair.sslmode, item.page_size) not in lane_overrides:
            raise RuntimeError(
                "lane manifest missing row for "
                + lane_key(item.route_pair.route, item.route_pair.sslmode, item.page_size)
            )
        entry["effective_endpoint"] = {
            "database": effective_args.database,
            "host": effective_args.host,
            "port": effective_args.port,
            "ipc_path": effective_args.ipc_path,
            "sslrootcert": effective_args.sslrootcert,
        }
        bootstrap = bootstrap_namespace_ancestors(
            effective_args,
            repo_root,
            item.route_pair.route,
            item.route_pair.sslmode,
            item.namespace,
        )
        entry["namespace_bootstrap"] = bootstrap
        if bootstrap.get("status") == "fail":
            raise RuntimeError(
                "namespace bootstrap failed: "
                + json.dumps(bootstrap.get("failures", []), sort_keys=True)
            )
        compiled = compile_suite(
            repo_root,
            item.run_root,
            driver=item.driver,
            run_id=args.run_id,
            route=item.route_pair.route,
            parser_mode=item.parser_mode,
            page_size=item.page_size,
            namespace=item.namespace,
        )
        tool_args = build_tool_args(
            effective_args,
            namespace=item.namespace,
            route=item.route_pair.route,
            sslmode=item.route_pair.sslmode,
            parser_mode=item.parser_mode,
            page_size=item.page_size,
            run_root=item.run_root,
            compiled=compiled,
            worker=item.worker,
        )
        result = run_command([*item.base_command, *tool_args], item.cwd, child_env(repo_root))
        entry["returncode"] = result.returncode
        entry["output_tail"] = result.stdout.splitlines()[-80:]
        entry["status"] = "pass" if result.returncode == 0 else "fail"
    except Exception as exc:  # noqa: BLE001 - matrix report must preserve the failure.
        entry["status"] = "fail"
        entry["error"] = str(exc)
    return entry


def selected(values: list[str], requested: list[str] | None) -> list[str]:
    if not requested:
        return values
    requested_set = set(requested)
    missing = sorted(requested_set - set(values))
    if missing:
        raise ValueError("unknown requested value(s): " + ", ".join(missing))
    return [value for value in values if value in requested_set]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--artifact-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--driver", action="append")
    parser.add_argument("--route", action="append")
    parser.add_argument("--page-size", action="append")
    parser.add_argument("--parser-mode", action="append")
    parser.add_argument("--concurrency-mode", action="append")
    parser.add_argument("--max-combinations", type=int)
    parser.add_argument("--shard-count", type=int, default=1)
    parser.add_argument("--shard-index", type=int, default=0)
    parser.add_argument("--resume-passing", action="store_true")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--run-id", default="manual")
    parser.add_argument("--database", default="default")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3092)
    parser.add_argument("--user", default="alice")
    parser.add_argument("--password", default="password")
    parser.add_argument("--role", default="sysarch")
    parser.add_argument("--sslrootcert", default="")
    parser.add_argument("--sslcert", default="")
    parser.add_argument("--sslkey", default="")
    parser.add_argument("--ipc-path", default=os.environ.get("SCRATCHBIRD_IPC_PATH", ""))
    parser.add_argument("--statement-timeout-ms", type=int, default=600000)
    parser.add_argument("--fetch-size", type=int, default=1000)
    parser.add_argument("--create-emulation-mode", default="")
    parser.add_argument("--language-resource-pack")
    parser.add_argument("--language-resource-identity")
    parser.add_argument("--language-resource-hash")
    parser.add_argument("--language-profile")
    parser.add_argument("--syntax-profile")
    parser.add_argument("--topology-profile")
    parser.add_argument(
        "--lane-manifest",
        type=Path,
        help=(
            "JSON file with lanes[] entries keyed by route, sslmode, and page_size. "
            "Each lane may override database, host, port, credentials, TLS certs, or IPC path."
        ),
    )
    parser.add_argument("--standard-english-fallback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--require-staged-tools", action="store_true")
    args = parser.parse_args()

    if args.shard_count < 1:
        raise SystemExit("--shard-count must be >= 1")
    if args.shard_index < 0 or args.shard_index >= args.shard_count:
        raise SystemExit("--shard-index must be >= 0 and < --shard-count")
    if args.jobs < 1:
        raise SystemExit("--jobs must be >= 1")
    repo_root = args.repo_root.resolve()
    language_defaults = default_language_contract(repo_root)
    args.language_resource_pack = args.language_resource_pack or language_defaults["resource_pack"]
    args.language_resource_identity = args.language_resource_identity or language_defaults["resource_identity"]
    args.language_resource_hash = args.language_resource_hash or language_defaults["resource_hash"]
    args.language_profile = args.language_profile or language_defaults["language_profile"]
    args.syntax_profile = args.syntax_profile or language_defaults["syntax_profile"]
    args.topology_profile = args.topology_profile or language_defaults["topology_profile"]
    gate_input = load_json(repo_root / GATE_INPUT_REL)
    tool_matrix = load_json(repo_root / TOOL_MATRIX_REL)
    manifest_rows = read_csv(repo_root / MANIFEST_REL)
    matrix_drivers = set(tool_source_map(tool_matrix))
    drivers = selected(
        [driver for driver in release_drivers(manifest_rows) if driver in matrix_drivers],
        args.driver,
    )
    routes = selected([str(value) for value in as_list(gate_input.get("required_routes"))], args.route)
    route_pairs = [pair for pair in route_variants(routes)]
    page_sizes = selected([str(value) for value in as_list(gate_input.get("required_page_sizes"))], args.page_size)
    parser_modes = selected([str(value) for value in as_list(gate_input.get("required_parser_modes"))], args.parser_mode)
    concurrency_modes = selected(
        [str(value) for value in as_list(gate_input.get("required_concurrency_modes"))],
        args.concurrency_mode,
    )
    artifact_root = (args.artifact_root or repo_root / DEFAULT_ARTIFACT_REL).resolve()
    report_path = (args.output or repo_root / DEFAULT_OUTPUT_REL).resolve()
    results: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    combination_count = 0
    selected_combination_count = 0
    complete_matrix_requested = not any(
        (
            args.driver,
            args.route,
            args.page_size,
            args.parser_mode,
            args.concurrency_mode,
            args.max_combinations,
            args.shard_count != 1,
            args.shard_index != 0,
        )
    )
    lane_manifest = optional_json(args.lane_manifest.resolve() if args.lane_manifest else None)
    lane_overrides = index_lane_manifest(lane_manifest)
    requires_lane_manifest = (
        not args.plan_only
        and len(page_sizes) > 1
        and not lane_overrides
    )
    if requires_lane_manifest:
        raise SystemExit(
            "live multi-page-size matrix execution requires --lane-manifest so every "
            "page-size row is matched to a fresh database/server lane"
        )
    lane_errors = preflight_lane_manifest(lane_overrides, route_pairs, page_sizes)
    if lane_errors:
        raise SystemExit("lane manifest preflight failed:\n" + "\n".join(lane_errors[:80]))

    required_artifacts = [str(name) for name in as_list(gate_input.get("required_artifacts"))]
    work_items: list[MatrixWorkItem] = []
    for driver in drivers:
        driver_caps = capability_for(tool_matrix, driver)
        if args.require_staged_tools:
            staged = staged_command_for_driver(repo_root, driver)
            if staged is None:
                raise RuntimeError(f"staged driver executable missing for {driver}")
            base_command, cwd = staged
        else:
            base_command, cwd = command_for_driver(repo_root, driver)
        for route_pair in route_pairs:
            if not route_supported_for_driver(route_pair.route, driver_caps):
                continue
            for page_size in page_sizes:
                for parser_mode in parser_modes:
                    for concurrency_mode in concurrency_modes:
                        combination_count += 1
                        if (combination_count - 1) % args.shard_count != args.shard_index:
                            continue
                        selected_combination_count += 1
                        if args.max_combinations and selected_combination_count > args.max_combinations:
                            continue
                        worker = "w0"
                        namespace = namespace_for(
                            driver,
                            args.run_id,
                            route_pair.route,
                            page_size,
                            parser_mode,
                            concurrency_mode,
                            worker,
                        )
                        run_root = (
                            artifact_root
                            / driver
                            / route_pair.route
                            / route_pair.sslmode
                            / page_size
                            / parser_mode
                            / concurrency_mode
                            / args.run_id
                        )
                        item = MatrixWorkItem(
                            ordinal=combination_count,
                            driver=driver,
                            route_pair=route_pair,
                            page_size=page_size,
                            parser_mode=parser_mode,
                            concurrency_mode=concurrency_mode,
                            worker=worker,
                            namespace=namespace,
                            run_root=run_root,
                            base_command=base_command,
                            cwd=cwd,
                            capability=driver_caps,
                        )
                        if args.plan_only:
                            entry = base_entry_for_item(args, item)
                            entry["status"] = "planned"
                            results.append(entry)
                            continue
                        work_items.append(item)

    if not args.plan_only:
        if args.jobs == 1:
            for item in work_items:
                entry = execute_work_item(item, args, repo_root, lane_overrides, required_artifacts)
                results.append(entry)
                if entry["status"] != "pass":
                    failures.append(entry)
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
                futures = {
                    executor.submit(
                        execute_work_item,
                        item,
                        args,
                        repo_root,
                        lane_overrides,
                        required_artifacts,
                    ): item
                    for item in work_items
                }
                for future in concurrent.futures.as_completed(futures):
                    entry = future.result()
                    results.append(entry)
                    if entry["status"] != "pass":
                        failures.append(entry)
            results.sort(key=lambda item: int(item.get("combination_ordinal", 0)))
            failures.sort(key=lambda item: int(item.get("combination_ordinal", 0)))

    artifact_gate: dict[str, Any] | None = None
    if not args.plan_only:
        gate_output = report_path.parent / f"{report_path.stem}_artifact_gate.json"
        gate_command = [
            sys.executable,
            str(repo_root / ARTIFACT_GATE_REL),
            "--repo-root",
            str(repo_root),
            "--mode",
            "artifact-schema",
            "--artifact-root",
            str(artifact_root),
            "--output",
            str(gate_output),
        ]
        if complete_matrix_requested:
            gate_command.append("--require-complete-matrix")
        gate_result = run_command(gate_command, repo_root, child_env(repo_root))
        artifact_gate = {
            "returncode": gate_result.returncode,
            "status": "pass" if gate_result.returncode == 0 else "fail",
            "output_tail": gate_result.stdout.splitlines()[-80:],
            "report": str(gate_output),
        }
        if gate_result.returncode != 0:
            failures.append({"status": "fail", "error": "artifact schema gate failed", "gate": artifact_gate})

    report = {
        "command": "run_driver_native_full_surface_matrix.py",
        "status": "pass" if not failures else "fail",
        "plan_only": args.plan_only,
        "driver_count": len(drivers),
        "transport_capability_by_driver": {
            driver: capability_for(tool_matrix, driver) for driver in drivers
        },
        "route_transport_variants": [pair.__dict__ for pair in route_pairs],
        "page_sizes": page_sizes,
        "parser_modes": parser_modes,
        "concurrency_modes": concurrency_modes,
        "language_resource_identity": args.language_resource_identity,
        "language_resource_hash": args.language_resource_hash,
        "language_profile": args.language_profile,
        "syntax_profile": args.syntax_profile,
        "topology_profile": args.topology_profile,
        "lane_manifest": str(args.lane_manifest.resolve()) if args.lane_manifest else None,
        "lane_count": len(lane_overrides),
        "combination_count": combination_count,
        "selected_combination_count": selected_combination_count,
        "work_item_count": len(work_items),
        "shard_count": args.shard_count,
        "shard_index": args.shard_index,
        "jobs": args.jobs,
        "resume_passing": args.resume_passing,
        "complete_matrix_requested": complete_matrix_requested,
        "executed_count": sum(1 for result in results if result["status"] != "planned"),
        "planned_count": sum(1 for result in results if result["status"] == "planned"),
        "failure_count": len(failures),
        "artifact_gate": artifact_gate,
        "full_results_path": str(report_path.parent / f"{report_path.stem}_results.json"),
        "full_failures_path": str(report_path.parent / f"{report_path.stem}_failures.json"),
        "results": results[:200],
        "truncated_results": max(0, len(results) - 200),
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    Path(report["full_results_path"]).write_text(
        json.dumps(results, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    Path(report["full_failures_path"]).write_text(
        json.dumps(failures, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"driver_native_full_surface_matrix={report['status']} "
        f"drivers={len(drivers)} combinations={combination_count} "
        f"plan_only={args.plan_only}"
    )
    if args.plan_only:
        return 0
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
