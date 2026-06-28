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


@dataclass(frozen=True)
class RouteVariant:
    route: str
    sslmode: str
    transport_mode: str


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


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


def namespace_for(driver: str, run_id: str, route: str, page_size: str, worker: str) -> str:
    clean_route = route.replace("-", "_")
    return f"users.public.examples.{driver}.{run_id}.{clean_route}.{page_size}.{worker}"


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
    parser.add_argument("--standard-english-fallback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--require-staged-tools", action="store_true")
    args = parser.parse_args()

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
                        if args.max_combinations and combination_count > args.max_combinations:
                            continue
                        worker = "w0"
                        namespace = namespace_for(driver, args.run_id, route_pair.route, page_size, worker)
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
                        entry: dict[str, Any] = {
                            "driver": driver,
                            "route": route_pair.route,
                            "sslmode": route_pair.sslmode,
                            "transport_mode": route_pair.transport_mode,
                            "page_size": page_size,
                            "parser_mode": parser_mode,
                            "concurrency_mode": concurrency_mode,
                            "language_profile": args.language_profile,
                            "language_resource_identity": args.language_resource_identity,
                            "language_resource_hash": args.language_resource_hash,
                            "namespace": namespace,
                            "run_root": str(run_root),
                            "command": [*base_command, "..."],
                        }
                        if args.plan_only:
                            entry["status"] = "planned"
                            results.append(entry)
                            continue
                        try:
                            compiled = compile_suite(
                                repo_root,
                                run_root,
                                driver=driver,
                                run_id=args.run_id,
                                route=route_pair.route,
                                parser_mode=parser_mode,
                                page_size=page_size,
                                namespace=namespace,
                            )
                            tool_args = build_tool_args(
                                args,
                                namespace=namespace,
                                route=route_pair.route,
                                sslmode=route_pair.sslmode,
                                parser_mode=parser_mode,
                                page_size=page_size,
                                run_root=run_root,
                                compiled=compiled,
                                worker=worker,
                            )
                            result = run_command([*base_command, *tool_args], cwd, child_env(repo_root))
                            entry["returncode"] = result.returncode
                            entry["output_tail"] = result.stdout.splitlines()[-80:]
                            entry["status"] = "pass" if result.returncode == 0 else "fail"
                        except Exception as exc:  # noqa: BLE001 - matrix report must preserve the failure.
                            entry["status"] = "fail"
                            entry["error"] = str(exc)
                        results.append(entry)
                        if entry["status"] != "pass":
                            failures.append(entry)

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
            "--require-complete-matrix",
            "--output",
            str(gate_output),
        ]
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
        "combination_count": combination_count,
        "executed_count": sum(1 for result in results if result["status"] != "planned"),
        "planned_count": sum(1 for result in results if result["status"] == "planned"),
        "failure_count": len(failures),
        "artifact_gate": artifact_gate,
        "results": results[:200],
        "truncated_results": max(0, len(results) - 200),
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
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
