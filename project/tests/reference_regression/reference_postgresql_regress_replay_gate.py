#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run PostgreSQL pg_regress against a ScratchBird PostgreSQL listener."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import reference_original_tool_smoke as smoke


SKIP_RETURN_CODE = 77
DEFAULT_MODE = "public-no-payload"
VALID_RUN_MODES = {"local-optional", "release-mandatory", "single-family"}


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", errors="replace")).hexdigest()


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def normalized(status: str,
               classification: str,
               command_id: str,
               output: str,
               diagnostic: str = "") -> dict[str, Any]:
    return {
        "schema_version": "scratchbird_reference_replay_normalized_result_v1",
        "status": status,
        "classification": classification,
        "command_id": command_id,
        "output_sha256": sha256_text(output),
        "diagnostic": diagnostic,
        "canonicalization": {
            "whitespace": "pg_regress_results_and_diff_hash",
            "ordering": "pg_regress_schedule_order",
            "nondeterministic_values": "timestamps_and_temp_paths_in_artifact_only",
        },
    }


def skip(args: argparse.Namespace, reason: str) -> int:
    payload = {
        "schema_version": "scratchbird_postgresql_regress_replay_gate_v1",
        "gate": "reference_postgresql_regress_replay_gate",
        "status": "skipped",
        "timestamp_utc": utc_timestamp(),
        "family": "postgresql",
        "run_mode": args.mode,
        "reason": reason,
        "normalized_result": normalized("skipped", reason, "postgresql.pg_regress", reason, reason),
        "authority_policy": "pg_regress_drives_parser_listener_only_engine_mga_security_storage_authority",
    }
    write_json(args.evidence_file, payload)
    write_failure_ledger(args.failure_ledger_file, [], payload)
    print(f"reference_postgresql_regress_replay_gate=skipped reason={reason}")
    return SKIP_RETURN_CODE


def candidate_pg_regress_tools() -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_POSTGRESQL_PG_REGRESS")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            Path("/usr/lib/postgresql/18/lib/pgxs/src/test/regress/pg_regress"),
            Path("/usr/lib/postgresql/17/lib/pgxs/src/test/regress/pg_regress"),
            Path("/usr/lib/postgresql/16/lib/pgxs/src/test/regress/pg_regress"),
            Path("/usr/lib/postgresql/13/lib/pgxs/src/test/regress/pg_regress"),
            Path("/usr/lib/postgresql/11/lib/pgxs/src/test/regress/pg_regress"),
        ]
    )
    return candidates


def resolve_pg_regress_tool() -> Path | None:
    for candidate in candidate_pg_regress_tools():
        candidate = candidate.expanduser()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def default_regress_root(repo_root: Path) -> Path:
    return (
        repo_root
        / "project/tests/reference_regression/reference_release_acquisition/"
        "postgresql/18.3/regression/acquired/postgresql_18_3/src/test/regress"
    )


def read_tail(path: Path, limit: int = 12000) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")[-limit:]


def failure_rows(payload: dict[str, Any]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for case in payload.get("case_results", []):
        if case.get("status") == "passed":
            continue
        digest = case.get("output_sha256", "")
        rows.append(
            {
                "case_id": case.get("case_id", "postgresql.pg_regress.unknown_case"),
                "family_id": "postgresql",
                "release_profile": "postgresql_18_beta2_full",
                "tool_name": "pg_regress",
                "tool_version": payload.get("tool_version", "unknown"),
                "suite_path": payload.get("regress_root", ""),
                "input_digest": case.get("input_sha256", digest),
                "input_kind": "sql",
                "expected_digest": case.get("expected_sha256", "not_applicable"),
                "actual_digest": digest,
                "expected_summary": "PostgreSQL regression case executes through ScratchBird listener",
                "actual_summary": case.get("summary", ""),
                "scratchbird_diagnostic": case.get("diagnostic", "not_applicable"),
                "classification": case.get("classification", "mapped_failure"),
                "owner": "parser",
                "fix_target": "project/src/parsers/compatibility/postgresql",
                "fix_commit": "",
                "rerun_status": "failed",
                "evidence_path": str(payload.get("evidence_file", "")),
                "public_status_required": "true",
                "notes": "Generated by reference_postgresql_regress_replay_gate.",
            }
        )
    return rows


def write_failure_ledger(path: Path, rows: list[dict[str, str]], payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "case_id",
        "family_id",
        "release_profile",
        "tool_name",
        "tool_version",
        "suite_path",
        "input_digest",
        "input_kind",
        "expected_digest",
        "actual_digest",
        "expected_summary",
        "actual_summary",
        "scratchbird_diagnostic",
        "classification",
        "owner",
        "fix_target",
        "fix_commit",
        "rerun_status",
        "evidence_path",
        "public_status_required",
        "notes",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def parse_case_results(regress_root: Path, output_dir: Path, stdout_text: str) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    seen: set[str] = set()
    patterns = [
        (re.compile(r"test\s+([A-Za-z0-9_.-]+)\s+\.\.\.\s+ok"), "passed", "semantic_case_passed"),
        (re.compile(r"test\s+([A-Za-z0-9_.-]+)\s+\.\.\.\s+FAILED"), "failed", "mapped_failure"),
    ]
    for line in stdout_text.splitlines():
        for pattern, status, classification in patterns:
            match = pattern.search(line)
            if not match:
                continue
            name = match.group(1)
            seen.add(name)
            out_path = output_dir / "results" / f"{name}.out"
            diff_path = output_dir / "results" / f"{name}.diff"
            sql_path = regress_root / "sql" / f"{name}.sql"
            expected_path = regress_root / "expected" / f"{name}.out"
            diagnostic = read_tail(diff_path) or read_tail(out_path) or line
            output_payload = diagnostic + "\n" + read_tail(out_path)
            results.append(
                {
                    "case_id": f"postgresql.pg_regress.{name}",
                    "name": name,
                    "status": status,
                    "classification": classification,
                    "sql": str(sql_path),
                    "expected": str(expected_path),
                    "actual": str(out_path),
                    "diff": str(diff_path) if diff_path.exists() else "",
                    "input_sha256": sha256_text(sql_path.read_text(encoding="utf-8", errors="replace"))
                    if sql_path.exists()
                    else sha256_text(name),
                    "expected_sha256": sha256_text(expected_path.read_text(encoding="utf-8", errors="replace"))
                    if expected_path.exists()
                    else "not_applicable",
                    "output_sha256": sha256_text(output_payload),
                    "diagnostic": diagnostic[-2000:],
                    "summary": line,
                }
            )
    if results:
        return results
    for diff_path in sorted((output_dir / "results").glob("*.diff")):
        name = diff_path.stem
        if name in seen:
            continue
        sql_path = regress_root / "sql" / f"{name}.sql"
        expected_path = regress_root / "expected" / f"{name}.out"
        diagnostic = read_tail(diff_path)
        results.append(
            {
                "case_id": f"postgresql.pg_regress.{name}",
                "name": name,
                "status": "failed",
                "classification": "mapped_failure",
                "sql": str(sql_path),
                "expected": str(expected_path),
                "actual": str(output_dir / "results" / f"{name}.out"),
                "diff": str(diff_path),
                "input_sha256": sha256_text(sql_path.read_text(encoding="utf-8", errors="replace"))
                if sql_path.exists()
                else sha256_text(name),
                "expected_sha256": sha256_text(expected_path.read_text(encoding="utf-8", errors="replace"))
                if expected_path.exists()
                else "not_applicable",
                "output_sha256": sha256_text(diagnostic),
                "diagnostic": diagnostic[-2000:],
                "summary": f"pg_regress diff produced for {name}",
            }
        )
    return results


def run_pg_regress(args: argparse.Namespace, regress_root: Path, tool: Path, work: Path) -> dict[str, Any]:
    server = None
    listener = None
    output_dir = work / "pg_regress_output"
    output_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = output_dir / "pg_regress.out"
    stderr_path = output_dir / "pg_regress.err"
    try:
        server, server_info = smoke.start_server(args, work)
        listener, listener_info = smoke.start_listener(args, work, server_info)
        schedule = Path(os.environ.get("SB_REFERENCE_POSTGRESQL_SCHEDULE", regress_root / "parallel_schedule"))
        if not schedule.is_file():
            raise RuntimeError(f"PostgreSQL regression schedule not found: {schedule}")
        cmd = [
            str(tool),
            "--use-existing",
            "--host=127.0.0.1",
            f"--port={listener_info['port']}",
            "--user=alice",
            "--dbname=default",
            f"--inputdir={regress_root}",
            f"--expecteddir={regress_root / 'expected'}",
            f"--outputdir={output_dir}",
            f"--schedule={schedule}",
            "--max-concurrent-tests=1",
        ]
        env = dict(os.environ)
        env["PGPASSWORD"] = "local_password"
        env["PGSSLMODE"] = "disable"
        proc = subprocess.run(
            cmd,
            cwd=regress_root,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.tool_timeout,
            check=False,
        )
        stdout_path.write_text(proc.stdout, encoding="utf-8", errors="replace")
        stderr_path.write_text(proc.stderr, encoding="utf-8", errors="replace")
        combined = proc.stdout + "\n" + proc.stderr + "\n" + read_tail(output_dir / "regression.diffs")
        case_results = parse_case_results(regress_root, output_dir, proc.stdout)
        failed_cases = [case for case in case_results if case["status"] != "passed"]
        status = "passed" if proc.returncode == 0 and not failed_cases else "failed"
        classification = "semantic_suite_passed" if status == "passed" else "mapped_failures"
        return {
            "schema_version": "scratchbird_postgresql_regress_replay_gate_v1",
            "gate": "reference_postgresql_regress_replay_gate",
            "status": status,
            "timestamp_utc": utc_timestamp(),
            "family": "postgresql",
            "run_mode": args.mode,
            "regress_root": str(regress_root),
            "work_dir": str(work),
            "server": server_info,
            "listener": listener_info,
            "tool": str(tool),
            "tool_version": subprocess.run(
                [str(tool), "--version"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            ).stdout.strip(),
            "command": cmd,
            "returncode": proc.returncode,
            "stdout": str(stdout_path),
            "stderr": str(stderr_path),
            "output_dir": str(output_dir),
            "case_results": case_results,
            "discovered_case_count": len(case_results),
            "passed_case_count": len(case_results) - len(failed_cases),
            "failed_case_count": len(failed_cases),
            "summary": combined[-4000:],
            "normalized_result": normalized(
                status,
                classification,
                "postgresql.pg_regress.parallel_schedule",
                combined,
                combined[-2000:],
            ),
            "authority_policy": "pg_regress_drives_parser_listener_only_engine_mga_security_storage_authority",
        }
    finally:
        smoke.stop_process(listener)
        smoke.stop_process(server)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--family", default="postgresql")
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--failure-ledger-file", required=True, type=Path)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--tool-timeout", type=int, default=1800)
    args = parser.parse_args(argv)

    args.mode = os.environ.get("SB_REFERENCE_REPLAY_MODE", DEFAULT_MODE)
    if args.mode == DEFAULT_MODE:
        return skip(args, "public_no_payload_mode")
    if args.mode not in VALID_RUN_MODES:
        print(f"unsupported SB_REFERENCE_REPLAY_MODE={args.mode}", file=sys.stderr)
        return 1
    if args.family != "postgresql":
        return skip(args, "native_original_tool_replay_not_implemented_for_family")

    repo_root = args.repo_root.resolve()
    regress_root = Path(os.environ.get("SB_REFERENCE_POSTGRESQL_REGRESS_ROOT", default_regress_root(repo_root))).resolve()
    if not regress_root.is_dir():
        if args.mode == "release-mandatory":
            print(f"PostgreSQL regression root is missing: {regress_root}", file=sys.stderr)
            return 1
        return skip(args, "postgresql_original_suite_not_present")
    tool = resolve_pg_regress_tool()
    if tool is None:
        if args.mode == "release-mandatory":
            print("PostgreSQL pg_regress tool is missing", file=sys.stderr)
            return 1
        return skip(args, "postgresql_original_tool_not_present")

    work = smoke.make_work_dir(args.work_root)
    try:
        payload = run_pg_regress(args, regress_root, tool, work)
    except Exception as exc:  # noqa: BLE001 - preserve replay failure context.
        payload = {
            "schema_version": "scratchbird_postgresql_regress_replay_gate_v1",
            "gate": "reference_postgresql_regress_replay_gate",
            "status": "failed",
            "timestamp_utc": utc_timestamp(),
            "family": "postgresql",
            "run_mode": args.mode,
            "regress_root": str(regress_root),
            "work_dir": str(work),
            "summary": str(exc),
            "normalized_result": normalized(
                "failed",
                "environment_or_protocol_error",
                "postgresql.pg_regress.parallel_schedule",
                str(exc),
                str(exc),
            ),
            "authority_policy": "pg_regress_drives_parser_listener_only_engine_mga_security_storage_authority",
        }
    payload["evidence_file"] = str(args.evidence_file)
    write_json(args.evidence_file, payload)
    write_failure_ledger(args.failure_ledger_file, failure_rows(payload), payload)
    print(
        "reference_postgresql_regress_replay_gate="
        f"{payload['status']} evidence={args.evidence_file} ledger={args.failure_ledger_file}"
    )
    return 0 if payload["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
