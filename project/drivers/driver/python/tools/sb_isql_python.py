#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Native Python driver conformance shell and example.

This tool intentionally uses the public ScratchBird Python DB-API surface. It is
both a conformance runner and a copyable example for connection setup, script
execution, diagnostics, metadata, transaction handling, and artifact routing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import traceback
from typing import Any
import xml.sax.saxutils

import scratchbird
from scratchbird.sql import split_top_level_statements


PAGE_SIZES = {"4k", "8k", "16k", "32k", "64k", "128k"}
ROUTES = {"embedded", "ipc_local", "listener-parser", "manager-listener-parser"}
PARSER_MODES = {"server-parser", "standalone-parser", "driver-sblr-uuid"}


class JsonlWriter:
    def __init__(self, path: Path):
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = self.path.open("w", encoding="utf-8")

    def write(self, record: dict[str, Any]) -> None:
        self.handle.write(json.dumps(record, sort_keys=True, default=str) + "\n")
        self.handle.flush()

    def close(self) -> None:
        self.handle.close()


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def digest_rows(rows: list[Any]) -> str:
    payload = json.dumps(rows, sort_keys=True, default=str, separators=(",", ":"))
    return sha256_text(payload)


def now_ns() -> int:
    return time.perf_counter_ns()


def load_expected_refusals(path: str | None) -> set[str]:
    if not path:
        return set()
    refusal_path = Path(path)
    if not refusal_path.is_file():
        raise FileNotFoundError(f"expected refusal file not found: {refusal_path}")
    doc = json.loads(refusal_path.read_text(encoding="utf-8"))
    if isinstance(doc, dict):
        return {str(value) for value in doc.get("statement_ids", [])}
    if isinstance(doc, list):
        return {str(value) for value in doc}
    raise ValueError("expected refusals must be a JSON object or array")


def load_script(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8")


def classify_statement(sql: str) -> str:
    first = sql.strip().split(None, 1)[0].lower() if sql.strip() else ""
    if first in {"create", "alter", "drop"}:
        return "ddl"
    if first in {"insert", "update", "delete", "merge", "upsert"}:
        return "dml"
    if first in {"commit", "rollback", "savepoint", "begin", "start"}:
        return "transaction"
    if first in {"select", "with", "values"}:
        lowered = sql.lower()
        if "sys." in lowered or "information_schema" in lowered:
            return "metadata"
        return "query"
    if first in {"grant", "revoke"}:
        return "security_refusal"
    return "query"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def append_log(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(text)


def junit_xml(testcases: list[dict[str, Any]], failures: list[dict[str, Any]]) -> str:
    failure_ids = {item["statement_id"]: item for item in failures if "statement_id" in item}
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<testsuite name="sb_isql_python" tests="{len(testcases)}" failures="{len(failures)}">',
    ]
    for case in testcases:
        statement_id = str(case["statement_id"])
        elapsed_s = float(case.get("elapsed_ns", 0)) / 1_000_000_000.0
        lines.append(
            f'  <testcase classname="scratchbird.python.driver" '
            f'name="{xml.sax.saxutils.escape(statement_id)}" time="{elapsed_s:.9f}">'
        )
        failure = failure_ids.get(statement_id)
        if failure:
            message = xml.sax.saxutils.escape(str(failure.get("message", "failure")))
            lines.append(f'    <failure message="{message}">{message}</failure>')
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    return "\n".join(lines) + "\n"


def connect_with_public_api(args: argparse.Namespace):
    kwargs = {
        "host": args.host,
        "port": args.port,
        "database": args.database,
        "user": args.user,
        "password": args.password,
        "role": args.role,
        "sslmode": args.sslmode,
        "application_name": "sb_isql_python",
        "connect_timeout": max(1, int(args.statement_timeout_ms / 1000))
        if args.statement_timeout_ms
        else 30,
    }
    if args.route == "manager-listener-parser":
        kwargs["front_door_mode"] = "manager"
    elif args.route == "ipc_local":
        kwargs["front_door_mode"] = "local_ipc"
    elif args.route == "embedded":
        kwargs["front_door_mode"] = "embedded"
    else:
        kwargs["front_door_mode"] = "direct"
    return scratchbird.connect(**kwargs)


def emit_metadata_snapshot(conn, path: Path, args: argparse.Namespace) -> None:
    snapshots: dict[str, Any] = {
        "driver": "python",
        "route": args.route,
        "parser_mode": args.parser_mode,
        "page_size": args.page_size,
        "namespace": args.namespace,
        "collections": {},
    }
    # Public metadata example: conn.query_metadata uses the driver metadata API.
    for collection in ("schemas", "tables", "columns", "indexes", "procedures", "functions"):
        started = now_ns()
        try:
            cur = conn.query_metadata(collection)
            rows = cur.fetchall()
            snapshots["collections"][collection] = {
                "status": "ok",
                "row_count": len(rows),
                "elapsed_ns": now_ns() - started,
                "digest": digest_rows(rows),
            }
        except Exception as exc:  # noqa: BLE001
            snapshots["collections"][collection] = {
                "status": "error",
                "elapsed_ns": now_ns() - started,
                "error": str(exc),
            }
    write_text(path, json.dumps(snapshots, indent=2, sort_keys=True, default=str) + "\n")


def run_script(args: argparse.Namespace) -> int:
    if args.page_size not in PAGE_SIZES:
        raise ValueError(f"unsupported page size: {args.page_size}")
    if args.route not in ROUTES:
        raise ValueError(f"unsupported route: {args.route}")
    if args.parser_mode not in PARSER_MODES:
        raise ValueError(f"unsupported parser mode: {args.parser_mode}")

    output_path = Path(args.output)
    error_path = Path(args.error)
    diagnostics_path = Path(args.diagnostics)
    metrics_path = Path(args.metrics)
    transcript_path = Path(args.transcript)
    summary_path = Path(args.summary)
    run_root = summary_path.parent
    command_events_path = run_root / "command-events.jsonl"
    timing_groups_path = run_root / "timing-groups.json"
    result_digests_path = run_root / "result-digests.json"
    metadata_path = run_root / "metadata-snapshots.json"
    security_refusals_path = run_root / "security-refusals.json"
    native_api_path = run_root / "native-api-coverage.json"
    code_review_path = run_root / "code-example-review.json"
    junit_path = run_root / "junit.xml"
    stdout_log_path = run_root / "stdout.log"
    stderr_log_path = run_root / "stderr.log"
    wire_transcript_path = run_root / "wire-transcript.jsonl"

    for path in (
        output_path,
        error_path,
        diagnostics_path,
        metrics_path,
        transcript_path,
        stdout_log_path,
        stderr_log_path,
    ):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("", encoding="utf-8")

    expected_refusals = load_expected_refusals(args.expected_refusals)
    script = load_script(args.input)
    statements = split_top_level_statements(script)
    event_writer = JsonlWriter(command_events_path)
    diagnostic_writer = JsonlWriter(diagnostics_path)
    transcript_writer = JsonlWriter(transcript_path)
    wire_writer = JsonlWriter(wire_transcript_path)
    testcases: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    digests: list[dict[str, Any]] = []
    security_refusals: list[dict[str, Any]] = []
    timing_groups: dict[str, int] = {}
    api_hits: dict[str, int] = {
        "scratchbird.connect": 0,
        "cursor.execute": 0,
        "cursor.fetchall": 0,
        "conn.commit": 0,
        "conn.rollback": 0,
        "conn.query_metadata": 0,
        "conn.attach_create": 0,
        "conn.execute_sblr": 0,
    }

    run_started = now_ns()
    conn = None
    try:
        connect_started = now_ns()
        conn = connect_with_public_api(args)
        api_hits["scratchbird.connect"] += 1
        connect_elapsed = now_ns() - connect_started
        timing_groups["connection"] = timing_groups.get("connection", 0) + connect_elapsed
        transcript_writer.write({
            "event": "connect",
            "driver": "python",
            "route": args.route,
            "parser_mode": args.parser_mode,
            "page_size": args.page_size,
            "elapsed_ns": connect_elapsed,
        })

        if args.create_database:
            create_started = now_ns()
            conn.attach_create(args.create_emulation_mode, args.database)
            api_hits["conn.attach_create"] += 1
            timing_groups["database_create"] = timing_groups.get("database_create", 0) + (now_ns() - create_started)

        if args.parser_mode != "server-parser":
            message = (
                f"{args.parser_mode} is not yet implemented by the Python native tool; "
                "the tool fails closed instead of silently using server-parser mode"
            )
            raise NotImplementedError(message)

        for index, statement in enumerate(statements, start=1):
            group = classify_statement(statement)
            statement_id = f"{Path(args.input).name}:{index}"
            expected_outcome = "refusal" if statement_id in expected_refusals else "success"
            started = now_ns()
            rows: list[Any] = []
            actual_outcome = "success"
            sqlstate = None
            diagnostic_code = None
            row_count = -1
            result_digest = None
            try:
                # Public DB-API example: prepare/execute/fetch through cursor.
                cursor = conn.cursor()
                cursor.execute(statement)
                api_hits["cursor.execute"] += 1
                rows = cursor.fetchall()
                api_hits["cursor.fetchall"] += 1
                row_count = len(rows)
                result_digest = digest_rows(rows)
                digests.append({
                    "statement_id": statement_id,
                    "row_count": row_count,
                    "result_digest": result_digest,
                })
                append_log(output_path, json.dumps({
                    "statement_id": statement_id,
                    "rows": rows,
                }, default=str) + "\n")
                if expected_outcome == "refusal":
                    failures.append({
                        "statement_id": statement_id,
                        "message": "statement succeeded but was expected to refuse",
                    })
                    actual_outcome = "unexpected_success"
            except Exception as exc:  # noqa: BLE001
                actual_outcome = "refusal"
                sqlstate = getattr(exc, "sqlstate", None)
                diagnostic_code = getattr(exc, "diagnostic_code", None)
                diagnostic_writer.write({
                    "statement_id": statement_id,
                    "sqlstate": sqlstate,
                    "diagnostic_code": diagnostic_code,
                    "message": str(exc),
                })
                append_log(error_path, f"{statement_id}: {exc}\n")
                if expected_outcome == "success":
                    failures.append({
                        "statement_id": statement_id,
                        "message": str(exc),
                    })
                    if args.stop_on_error:
                        raise
                else:
                    security_refusals.append({
                        "statement_id": statement_id,
                        "sqlstate": sqlstate,
                        "diagnostic_code": diagnostic_code,
                    })
            elapsed = now_ns() - started
            timing_groups[group] = timing_groups.get(group, 0) + elapsed
            event = {
                "run_id": args.run_id,
                "driver_name": "python",
                "driver_version": getattr(scratchbird, "__version__", "unknown"),
                "route": args.route,
                "parser_mode": args.parser_mode,
                "page_size": args.page_size,
                "namespace": args.namespace,
                "script": args.input,
                "statement_index": index,
                "statement_id": statement_id,
                "command_group": group,
                "sql_hash": sha256_text(statement),
                "expected_outcome": expected_outcome,
                "actual_outcome": actual_outcome,
                "sqlstate": sqlstate,
                "diagnostic_code": diagnostic_code,
                "canonical_message_vector": [],
                "row_count": row_count,
                "result_digest": result_digest,
                "elapsed_ns": elapsed,
                "round_trip_count": None,
                "fetch_batch_count": None,
                "server_revalidation_state": "required",
                "transaction_id_observed": None,
                "mga_authority": "engine",
                "native_api_surface": "python_dbapi_2",
                "code_example_section": "execute_fetch",
            }
            event_writer.write(event)
            testcases.append(event)

        metadata_started = now_ns()
        emit_metadata_snapshot(conn, metadata_path, args)
        api_hits["conn.query_metadata"] += 1
        timing_groups["metadata"] = timing_groups.get("metadata", 0) + (now_ns() - metadata_started)
        if not conn.autocommit:
            conn.commit()
            api_hits["conn.commit"] += 1
    except Exception as exc:  # noqa: BLE001
        if conn is not None:
            try:
                conn.rollback()
                api_hits["conn.rollback"] += 1
            except Exception:
                pass
        failures.append({"statement_id": "run", "message": str(exc)})
        diagnostic_writer.write({
            "statement_id": "run",
            "message": str(exc),
            "traceback": traceback.format_exc(),
        })
        append_log(stderr_log_path, traceback.format_exc())
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass
        event_writer.close()
        diagnostic_writer.close()
        transcript_writer.close()
        wire_writer.close()

    total_elapsed = now_ns() - run_started
    timing_groups["overall"] = total_elapsed
    summary = {
        "run_id": args.run_id,
        "driver_name": "python",
        "route": args.route,
        "parser_mode": args.parser_mode,
        "page_size": args.page_size,
        "namespace": args.namespace,
        "status": "fail" if failures else "pass",
        "statement_count": len(statements),
        "failure_count": len(failures),
        "elapsed_ns": total_elapsed,
        "server_revalidation_required": True,
        "driver_or_parser_finality": "forbidden",
        "mga_authority": "engine",
        "artifacts": {
            "command-events.jsonl": str(command_events_path),
            "summary.json": str(summary_path),
            "diagnostics.jsonl": str(diagnostics_path),
            "wire-transcript.jsonl": str(wire_transcript_path),
            "timing-groups.json": str(timing_groups_path),
            "result-digests.json": str(result_digests_path),
            "metadata-snapshots.json": str(metadata_path),
            "security-refusals.json": str(security_refusals_path),
            "native-api-coverage.json": str(native_api_path),
            "code-example-review.json": str(code_review_path),
            "junit.xml": str(junit_path),
            "stdout.log": str(stdout_log_path),
            "stderr.log": str(stderr_log_path),
        },
    }
    write_text(summary_path, json.dumps(summary, indent=2, sort_keys=True, default=str) + "\n")
    write_text(metrics_path, json.dumps(timing_groups, indent=2, sort_keys=True) + "\n")
    write_text(timing_groups_path, json.dumps(timing_groups, indent=2, sort_keys=True) + "\n")
    write_text(result_digests_path, json.dumps(digests, indent=2, sort_keys=True) + "\n")
    write_text(security_refusals_path, json.dumps(security_refusals, indent=2, sort_keys=True) + "\n")
    write_text(native_api_path, json.dumps(api_hits, indent=2, sort_keys=True) + "\n")
    write_text(code_review_path, json.dumps({
        "driver": "python",
        "public_api_only": True,
        "shells_out_to_other_driver": False,
        "source_is_canonical_example": True,
        "sections": [
            "connection",
            "database_create",
            "prepared_execution",
            "fetch",
            "diagnostics",
            "metadata",
            "transaction",
            "artifact_routing",
        ],
    }, indent=2, sort_keys=True) + "\n")
    write_text(junit_path, junit_xml(testcases or [{"statement_id": "run", "elapsed_ns": total_elapsed}], failures))
    append_log(stdout_log_path, f"sb_isql_python status={summary['status']} statements={len(statements)}\n")
    return 1 if failures else 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ScratchBird native Python driver conformance shell")
    parser.add_argument("--database", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3092)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--role", default="")
    parser.add_argument("--sslmode", default="require")
    parser.add_argument("--route", choices=sorted(ROUTES), default="listener-parser")
    parser.add_argument("--parser-mode", choices=sorted(PARSER_MODES), default="server-parser")
    parser.add_argument("--page-size", choices=sorted(PAGE_SIZES), default="8k")
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--error", required=True)
    parser.add_argument("--diagnostics", required=True)
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--transcript", required=True)
    parser.add_argument("--summary", required=True)
    parser.add_argument("--stop-on-error", choices=("true", "false"), default="true")
    parser.add_argument("--expected-refusals")
    parser.add_argument("--statement-timeout-ms", type=int, default=30000)
    parser.add_argument("--fetch-size", type=int, default=1000)
    parser.add_argument("--concurrency-worker", type=int, default=0)
    parser.add_argument("--run-id", default=os.environ.get("SB_DRIVER_RUN_ID", "manual"))
    parser.add_argument("--create-database", action="store_true")
    parser.add_argument("--create-emulation-mode", default="sbsql")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    args.stop_on_error = args.stop_on_error.lower() == "true"
    return run_script(args)


if __name__ == "__main__":
    raise SystemExit(main())
