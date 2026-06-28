#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run one staged driver tool against one script and diff the result output."""

from __future__ import annotations

import argparse
import difflib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


DEFAULT_BIN_REL = Path("build/output/linux/bin")
DEFAULT_REPORT_REL = Path("build/reports/driver_script_diff_contract.json")
DRIVER_EXECUTABLES = {
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


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_text_lines(path: Path) -> list[str]:
    if not path.exists():
        return []
    return path.read_text(encoding="utf-8").splitlines(keepends=True)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def artifact_paths(actual: Path) -> dict[str, Path]:
    root = actual.parent
    stem = actual.stem
    return {
        "output": actual,
        "error": root / f"{stem}.stderr.log",
        "diagnostics": root / f"{stem}.diagnostics.jsonl",
        "metrics": root / f"{stem}.metrics.jsonl",
        "transcript": root / f"{stem}.wire-transcript.jsonl",
        "summary": root / f"{stem}.summary.json",
    }


def driver_command(repo_root: Path, bin_root: Path, driver: str) -> list[str]:
    executable_name = DRIVER_EXECUTABLES.get(driver)
    if not executable_name:
        raise ValueError(f"unknown driver: {driver}")
    executable = (bin_root / executable_name).resolve()
    return [str(executable)]


def common_args(args: argparse.Namespace, paths: dict[str, Path]) -> list[str]:
    return [
        "--database", args.database,
        "--host", args.host,
        "--port", str(args.port),
        "--user", args.user,
        "--password", args.password,
        "--role", args.role,
        "--sslmode", args.sslmode,
        "--sslrootcert", args.sslrootcert,
        "--sslcert", args.sslcert,
        "--sslkey", args.sslkey,
        "--ipc-path", args.ipc_path,
        "--route", args.route,
        "--parser-mode", args.parser_mode,
        "--page-size", str(args.page_size),
        "--namespace", args.namespace,
        "--input", str(args.input),
        "--output", str(paths["output"]),
        "--error", str(paths["error"]),
        "--diagnostics", str(paths["diagnostics"]),
        "--metrics", str(paths["metrics"]),
        "--transcript", str(paths["transcript"]),
        "--summary", str(paths["summary"]),
        "--expected-refusals", args.expected_refusals,
        "--statement-timeout-ms", str(args.statement_timeout_ms),
        "--fetch-size", str(args.fetch_size),
        "--concurrency-worker", str(args.concurrency_worker),
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


def run_command(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def contract_check(repo_root: Path, bin_root: Path, report: Path) -> int:
    results = []
    for driver, executable_name in DRIVER_EXECUTABLES.items():
        executable = bin_root / executable_name
        results.append(
            {
                "driver": driver,
                "executable": str(executable),
                "exists": executable.exists(),
                "is_executable": executable.is_file() and os.access(executable, os.X_OK),
            }
        )
    failures = [item for item in results if not item["exists"] or not item["is_executable"]]
    payload = {
        "command": "run_driver_script_diff.py --contract-check",
        "status": "pass" if not failures else "fail",
        "repo_root": str(repo_root),
        "bin_root": str(bin_root),
        "driver_count": len(results),
        "failure_count": len(failures),
        "results": results,
    }
    write_json(report, payload)
    print(f"driver_script_diff_contract={payload['status']} drivers={len(results)}")
    return 0 if not failures else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--bin-root", type=Path)
    parser.add_argument("--driver", choices=sorted(DRIVER_EXECUTABLES))
    parser.add_argument("--input", type=Path)
    parser.add_argument("--expected", type=Path)
    parser.add_argument("--actual", type=Path)
    parser.add_argument("--diff-output", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--contract-check", action="store_true")
    parser.add_argument("--database", default="default")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3092)
    parser.add_argument("--user", default="alice")
    parser.add_argument("--password", default="password")
    parser.add_argument("--role", default="sysarch")
    parser.add_argument("--sslmode", default="require")
    parser.add_argument("--sslrootcert", default="")
    parser.add_argument("--sslcert", default="")
    parser.add_argument("--sslkey", default="")
    parser.add_argument("--ipc-path", default=os.environ.get("SCRATCHBIRD_IPC_PATH", ""))
    parser.add_argument("--route", default="listener-parser")
    parser.add_argument("--parser-mode", default="strict")
    parser.add_argument("--page-size", type=int, default=8192)
    parser.add_argument("--namespace", default="users.public.examples.driver_diff")
    parser.add_argument("--expected-refusals", default="")
    parser.add_argument("--statement-timeout-ms", type=int, default=600000)
    parser.add_argument("--fetch-size", type=int, default=1000)
    parser.add_argument("--concurrency-worker", default="0")
    parser.add_argument("--run-id", default="manual")
    parser.add_argument("--create-emulation-mode", default="")
    parser.add_argument("--language-resource-pack", default="")
    parser.add_argument("--language-resource-identity", default="")
    parser.add_argument("--language-resource-hash", default="")
    parser.add_argument("--language-profile", default="en-US")
    parser.add_argument("--syntax-profile", default="sbsql.v3")
    parser.add_argument("--topology-profile", default="topology.sbsql.canonical.v1")
    parser.add_argument("--standard-english-fallback", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    bin_root = (args.bin_root or repo_root / DEFAULT_BIN_REL).resolve()
    report = (args.report or repo_root / DEFAULT_REPORT_REL).resolve()
    if args.contract_check:
        return contract_check(repo_root, bin_root, report)

    required = {"driver": args.driver, "input": args.input, "expected": args.expected, "actual": args.actual}
    missing = [name for name, value in required.items() if value is None]
    if missing:
        raise SystemExit("missing required argument(s): " + ", ".join(f"--{name}" for name in missing))

    actual = args.actual.resolve()
    expected = args.expected.resolve()
    paths = artifact_paths(actual)
    for path in paths.values():
        path.parent.mkdir(parents=True, exist_ok=True)

    command = [*driver_command(repo_root, bin_root, args.driver), *common_args(args, paths)]
    result = run_command(command, repo_root)
    expected_lines = read_text_lines(expected)
    actual_lines = read_text_lines(actual)
    diff_lines = list(
        difflib.unified_diff(
            expected_lines,
            actual_lines,
            fromfile=str(expected),
            tofile=str(actual),
        )
    )
    diff_output = (args.diff_output or actual.parent / f"{actual.stem}.diff").resolve()
    diff_output.parent.mkdir(parents=True, exist_ok=True)
    diff_output.write_text("".join(diff_lines), encoding="utf-8")

    status = "pass" if result.returncode == 0 and not diff_lines else "fail"
    payload = {
        "command": "run_driver_script_diff.py",
        "status": status,
        "driver": args.driver,
        "returncode": result.returncode,
        "input": str(args.input),
        "expected": str(expected),
        "actual": str(actual),
        "diff_output": str(diff_output),
        "stdout_tail": result.stdout.splitlines()[-80:],
        "artifacts": {name: str(path) for name, path in paths.items()},
    }
    write_json(report, payload)
    print(f"driver_script_diff={status} driver={args.driver} actual={actual}")
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
