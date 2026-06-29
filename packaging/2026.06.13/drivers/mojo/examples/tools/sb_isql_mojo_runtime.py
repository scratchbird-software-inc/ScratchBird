#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Runtime bookkeeping for the Mojo native conformance shell.

The Mojo binary owns process entry and the ScratchBird C ABI bridge calls. This
module keeps argument parsing, canonical chain chunking, and artifact emission
byte-for-byte close to the other driver runners.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import sys
import time
from typing import Any
import xml.sax.saxutils


PAGE_SIZES = {"4k", "8k", "16k", "32k", "64k", "128k"}
PAGE_SIZE_BYTES = {
    "4k": 4096,
    "8k": 8192,
    "16k": 16384,
    "32k": 32768,
    "64k": 65536,
    "128k": 131072,
}
ROUTES = {"embedded", "ipc_local", "listener-parser", "manager-listener-parser"}
PARSER_MODES = {"server-parser", "standalone-parser", "driver-sblr-uuid"}


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[5]


REPO_ROOT = _repo_root()
PY_DRIVER_SRC = REPO_ROOT / "project" / "drivers" / "driver" / "python" / "src"
if str(PY_DRIVER_SRC) not in sys.path:
    sys.path.insert(0, str(PY_DRIVER_SRC))

from scratchbird.sql import iter_chain_statements, split_top_level_statements  # noqa: E402


def bool_arg(value: str | None) -> bool:
    if value is None:
        return True
    lowered = value.lower()
    if lowered in {"1", "true", "yes", "on"}:
        return True
    if lowered in {"0", "false", "no", "off"}:
        return False
    raise argparse.ArgumentTypeError(f"expected boolean value, got {value!r}")


def now_ns() -> int:
    return time.perf_counter_ns()


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def digest_rows(rows: Any) -> str:
    payload = json.dumps(rows, sort_keys=True, default=str, separators=(",", ":"))
    return sha256_text(payload)


def current_process_metrics() -> dict[str, dict[str, int]]:
    rss_kb = max(1, int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss))
    vsize_kb = rss_kb
    try:
        statm = Path("/proc/self/statm").read_text(encoding="utf-8").split()
        if len(statm) >= 2:
            page_kb = max(1, os.sysconf("SC_PAGE_SIZE") // 1024)
            vsize_kb = max(1, int(statm[0]) * page_kb)
            rss_kb = max(1, int(statm[1]) * page_kb)
    except (OSError, ValueError):
        pass
    return {
        "client": {
            "max_rss_kb": rss_kb,
            "max_vsize_kb": vsize_kb,
            "last_rss_kb": rss_kb,
            "last_vsize_kb": vsize_kb,
        }
    }


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


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def append_log(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(text)


def add_expected_refusal_ids(target: set[str], value: Any) -> None:
    if isinstance(value, str):
        target.add(value)
        return
    if isinstance(value, dict):
        for key in ("statement_id", "statementId", "id"):
            item = value.get(key)
            if isinstance(item, str):
                target.add(item)
        for key in (
            "statement_ids",
            "statementIds",
            "expected_refusals",
            "expectedRefusals",
            "expected_diagnostics",
            "expectedDiagnostics",
        ):
            nested = value.get(key)
            if isinstance(nested, list):
                for item in nested:
                    add_expected_refusal_ids(target, item)


def load_expected_refusals(path: str | None) -> set[str]:
    if not path:
        return set()
    refusal_path = Path(path)
    if not refusal_path.is_file():
        raise FileNotFoundError(f"expected refusal file not found: {refusal_path}")
    doc = json.loads(refusal_path.read_text(encoding="utf-8"))
    expected: set[str] = set()
    if isinstance(doc, dict):
        add_expected_refusal_ids(expected, doc)
        diagnostics = doc.get("expected_diagnostics")
        if isinstance(diagnostics, dict):
            expected.update(str(key) for key in diagnostics)
            aliases = doc.get("compiled_chain_statement_aliases")
            if isinstance(aliases, dict):
                for key in diagnostics:
                    alias = aliases.get(key)
                    if isinstance(alias, str):
                        expected.add(alias)
    elif isinstance(doc, list):
        for item in doc:
            add_expected_refusal_ids(expected, item)
    else:
        raise ValueError("expected refusals must be a JSON object or array")
    return expected


def load_script(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8")


def iter_statement_records(script: str, input_name: str) -> list[tuple[str, int, str]]:
    if "-- begin_script:" in script:
        return list(iter_chain_statements(script))
    return [(Path(input_name).name, index, statement) for index, statement in enumerate(split_top_level_statements(script), start=1)]


def classify_statement(sql: str) -> str:
    first = sql.strip().split(None, 1)[0].lower() if sql.strip() else ""
    if first in {"create", "alter", "drop"}:
        return "ddl"
    if first in {"insert", "update", "delete", "merge", "upsert", "copy"}:
        return "dml"
    if first in {"commit", "rollback", "savepoint", "begin", "start"}:
        return "transaction"
    if first in {"select", "with", "values", "show"}:
        lowered = sql.lower()
        if "sys." in lowered or "information_schema" in lowered or first == "show":
            return "metadata"
        return "query"
    if first in {"grant", "revoke"}:
        return "security_refusal"
    return "query"


def _statement_tokens(sql: str) -> list[str]:
    text = " ".join(line.strip() for line in _non_comment_lines(sql)).strip()
    if not text:
        return []
    return [token.strip().rstrip(";") for token in text.split()]


def transaction_operation(sql: str) -> str:
    tokens = [token.lower() for token in _statement_tokens(sql)]
    if not tokens:
        return ""
    first = tokens[0]
    second = tokens[1] if len(tokens) > 1 else ""
    if first == "begin" or (first == "start" and second == "transaction"):
        return "begin"
    if first == "commit":
        return "commit"
    if first == "rollback" and second != "to":
        return "rollback"
    if first == "savepoint":
        return "savepoint"
    if first == "release" and second == "savepoint":
        return "release_savepoint"
    if first == "rollback" and second == "to":
        return "rollback_to"
    return ""


def statement_transaction_operation(state: RunnerState, index: int) -> str:
    return transaction_operation(state.statements[index][2])


def savepoint_name(sql: str) -> str:
    tokens = _statement_tokens(sql)
    lowered = [token.lower() for token in tokens]
    if not lowered:
        return ""
    if lowered[0] == "savepoint" and len(tokens) >= 2:
        return tokens[1].rstrip(";")
    if lowered[0] == "release" and len(lowered) >= 3 and lowered[1] == "savepoint":
        return tokens[2].rstrip(";")
    if lowered[0] == "rollback" and len(lowered) >= 3 and lowered[1] == "to":
        if lowered[2] == "savepoint" and len(tokens) >= 4:
            return tokens[3].rstrip(";")
        return tokens[2].rstrip(";")
    return ""


def statement_savepoint_name(state: RunnerState, index: int) -> str:
    return savepoint_name(state.statements[index][2])


def write_empty_result(path: str | Path, rows_affected: int = 0) -> None:
    write_text(
        Path(path),
        json.dumps(
            {
                "columns": [],
                "rows": [],
                "row_count": 0,
                "rows_affected": int(rows_affected),
            },
            sort_keys=True,
        )
        + "\n",
    )


def _non_comment_lines(sql: str) -> list[str]:
    lines: list[str] = []
    for raw in sql.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("--"):
            continue
        lines.append(raw)
    return lines


def executable_sql_without_copy_markers(sql: str) -> str:
    lines: list[str] = []
    marker = "-- SB_COPY_INPUT "
    for raw in sql.splitlines():
        stripped = raw.lstrip()
        if stripped.startswith(marker):
            continue
        lines.append(raw)
    return "\n".join(lines).strip()


def copy_payload_for_statement(sql: str) -> str:
    marker = "-- SB_COPY_INPUT "
    rows: list[str] = []
    for raw in sql.splitlines():
        stripped = raw.lstrip()
        if not stripped.startswith(marker):
            continue
        row = stripped[len(marker):]
        if row.endswith("\r"):
            row = row[:-1]
        rows.append(row)
    return "\n".join(rows) + ("\n" if rows else "")


def is_copy_stdin_statement(sql: str) -> bool:
    executable = " ".join(line.strip().lower() for line in _non_comment_lines(sql))
    return executable.startswith("copy ") and " from stdin" in executable


def transport_mode_for_route(route: str, sslmode: str) -> str:
    if route == "embedded":
        return "embedded_no_network_transport"
    if route == "ipc_local":
        return "local_ipc_no_tls"
    return "tls_disabled" if sslmode == "disable" else "tls_required"


def endpoint_kind_for_route(route: str) -> str:
    if route == "ipc_local":
        return "unix_domain_socket"
    if route == "embedded":
        return "embedded_bridge"
    return "tcp"


def transport_implementation_for_route(route: str) -> str:
    if route == "embedded":
        return "unsupported_no_embedded_mojo_boundary"
    if route == "ipc_local":
        return "unsupported_no_native_ipc_boundary"
    return "mojo_external_call_cpp_client_abi_tcp"


def build_dsn(args: argparse.Namespace) -> str:
    query: dict[str, str] = {
        "sslmode": args.sslmode,
        "application_name": "sb_isql_mojo",
        "enable_copy_streaming": "true",
        "binary_transfer": "true",
    }
    if args.role:
        query["role"] = args.role
    if args.sslrootcert:
        query["sslrootcert"] = args.sslrootcert
    if args.sslcert:
        query["sslcert"] = args.sslcert
    if args.sslkey:
        query["sslkey"] = args.sslkey
    if args.route == "manager-listener-parser":
        query["front_door_mode"] = "manager"
    else:
        query["front_door_mode"] = "direct"
    query_text = "&".join(f"{key}={value}" for key, value in query.items())
    return f"scratchbird://{args.user}:{args.password}@{args.host}:{args.port}/{args.database}?{query_text}"


def route_environment(
    args: argparse.Namespace,
    *,
    actual_page_size: int | None,
    status: str,
    reason: str | None = None,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "run_id": args.run_id,
        "driver": "mojo",
        "route": args.route,
        "sslmode": args.sslmode,
        "parser_mode": args.parser_mode,
        "concurrency_mode": "single",
        "namespace": args.namespace,
        "page_size": args.page_size,
        "expected_page_size_bytes": PAGE_SIZE_BYTES[args.page_size],
        "actual_page_size_bytes": actual_page_size,
        "page_size_verification_source": "SHOW DATABASE",
        "page_size_verification_status": status,
        "transport_mode": transport_mode_for_route(args.route, args.sslmode),
        "transport_endpoint_kind": endpoint_kind_for_route(args.route),
        "driver_transport_implementation": transport_implementation_for_route(args.route),
    }
    if reason:
        record["failure_reason"] = reason
    return record


def read_json_file(path: str | Path) -> Any:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def parse_bridge_error(path: str | Path) -> dict[str, Any]:
    error_path = Path(path)
    if not error_path.exists() or not error_path.read_text(encoding="utf-8").strip():
        return {"message": "ScratchBird Mojo bridge returned an error without diagnostic text"}
    try:
        return read_json_file(error_path)
    except json.JSONDecodeError:
        return {"message": error_path.read_text(encoding="utf-8", errors="replace")}


def junit_xml(testcases: list[dict[str, Any]], failures: list[dict[str, Any]]) -> str:
    failure_ids = {item["statement_id"]: item for item in failures if "statement_id" in item}
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<testsuite name="sb_isql_mojo" tests="{len(testcases)}" failures="{len(failures)}">',
    ]
    for case in testcases:
        statement_id = str(case["statement_id"])
        elapsed_s = float(case.get("elapsed_ns", 0)) / 1_000_000_000.0
        lines.append(
            f'  <testcase classname="scratchbird.mojo.driver" '
            f'name="{xml.sax.saxutils.escape(statement_id)}" time="{elapsed_s:.9f}">'
        )
        failure = failure_ids.get(statement_id)
        if failure:
            message = xml.sax.saxutils.escape(str(failure.get("message", "failure")))
            lines.append(f'    <failure message="{message}">{message}</failure>')
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    return "\n".join(lines) + "\n"


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ScratchBird native Mojo driver conformance shell")
    parser.add_argument("--database", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3092)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--role", default="")
    parser.add_argument("--sslmode", default="require")
    parser.add_argument("--sslrootcert")
    parser.add_argument("--sslcert")
    parser.add_argument("--sslkey")
    parser.add_argument("--ipc-path", default=os.environ.get("SCRATCHBIRD_IPC_PATH", ""))
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
    parser.add_argument("--stop-on-error", nargs="?", const="true", default="true", type=bool_arg)
    parser.add_argument("--expected-refusals")
    parser.add_argument("--statement-timeout-ms", type=int, default=30000)
    parser.add_argument("--fetch-size", type=int, default=1000)
    parser.add_argument("--concurrency-worker", type=int, default=0)
    parser.add_argument("--run-id", default=os.environ.get("SB_DRIVER_RUN_ID", "manual"))
    parser.add_argument("--create-database", nargs="?", const="true", default="false", type=bool_arg)
    parser.add_argument("--create-emulation-mode", default="sbsql")
    parser.add_argument("--language-resource-pack", default="project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack")
    parser.add_argument("--language-resource-identity", default="sbsql.common_resource_pack.v1")
    parser.add_argument("--language-resource-hash", default="sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc")
    parser.add_argument("--language-profile", default="en-US")
    parser.add_argument("--syntax-profile", default="sbsql.v3")
    parser.add_argument("--topology-profile", default="topology.sbsql.canonical.v1")
    parser.add_argument("--standard-english-fallback", nargs="?", const="true", default="true", type=bool_arg)
    return parser


def print_help() -> None:
    build_arg_parser().print_help()


def help_text() -> str:
    return build_arg_parser().format_help()


class RunnerState:
    def __init__(self, argv: list[str]):
        normalized = list(argv)
        if "--" in normalized:
            normalized = normalized[normalized.index("--") + 1 :]
        elif normalized:
            normalized = normalized[1:]
        self.args = build_arg_parser().parse_args(normalized)
        if self.args.route in {"embedded", "ipc_local"}:
            raise ValueError(f"Mojo conformance runner does not support route {self.args.route}")
        if self.args.parser_mode != "server-parser":
            raise ValueError(f"Mojo conformance runner supports server-parser mode for this closure lane, got {self.args.parser_mode}")
        self.output_path = Path(self.args.output)
        self.error_path = Path(self.args.error)
        self.diagnostics_path = Path(self.args.diagnostics)
        self.metrics_path = Path(self.args.metrics)
        self.transcript_path = Path(self.args.transcript)
        self.summary_path = Path(self.args.summary)
        self.run_root = self.summary_path.parent
        self.command_events_path = self.run_root / "command-events.jsonl"
        self.timing_groups_path = self.run_root / "timing-groups.json"
        self.result_digests_path = self.run_root / "result-digests.json"
        self.metadata_path = self.run_root / "metadata-snapshots.json"
        self.route_environment_path = self.run_root / "route-environment.json"
        self.process_metrics_path = self.run_root / "process-metrics.jsonl"
        self.security_refusals_path = self.run_root / "security-refusals.json"
        self.native_api_path = self.run_root / "native-api-coverage.json"
        self.code_review_path = self.run_root / "code-example-review.json"
        self.junit_path = self.run_root / "junit.xml"
        self.stdout_log_path = self.run_root / "stdout.log"
        self.stderr_log_path = self.run_root / "stderr.log"
        self.wire_transcript_path = self.run_root / "wire-transcript.jsonl"
        self.bridge_tmp = self.run_root / "bridge-tmp"
        self.bridge_tmp.mkdir(parents=True, exist_ok=True)
        for path in (
            self.output_path,
            self.error_path,
            self.diagnostics_path,
            self.metrics_path,
            self.transcript_path,
            self.stdout_log_path,
            self.stderr_log_path,
        ):
            write_text(path, "")
        self.expected_refusals = load_expected_refusals(self.args.expected_refusals)
        self.stop_requested = False
        self.script = load_script(self.args.input)
        self.statements = iter_statement_records(self.script, self.args.input)
        self.event_writer = JsonlWriter(self.command_events_path)
        self.diagnostic_writer = JsonlWriter(self.diagnostics_path)
        self.transcript_writer = JsonlWriter(self.transcript_path)
        self.wire_writer = JsonlWriter(self.wire_transcript_path)
        self.testcases: list[dict[str, Any]] = []
        self.failures: list[dict[str, Any]] = []
        self.digests: list[dict[str, Any]] = []
        self.security_refusals: list[dict[str, Any]] = []
        self.timing_groups: dict[str, int] = {}
        self.api_hits: dict[str, int] = {
            "ScratchBirdConnection": 0,
            "connect": 0,
            "prepare": 0,
            "execute": 0,
            "query_metadata": 0,
            "commit": 0,
            "rollback": 0,
        }
        self.started_ns = now_ns()
        self.route_env = route_environment(self.args, actual_page_size=None, status="fail", reason="not_probed")
        write_text(self.route_environment_path, json.dumps(self.route_env, indent=2, sort_keys=True) + "\n")

    def close_writers(self) -> None:
        for writer in (self.event_writer, self.diagnostic_writer, self.transcript_writer, self.wire_writer):
            try:
                writer.close()
            except Exception:
                pass


def create_state(py_argv: Any) -> RunnerState:
    return RunnerState([str(item) for item in py_argv])


def connect_dsn(state: RunnerState) -> str:
    return build_dsn(state.args)


def tmp_result_path(state: RunnerState, name: str) -> str:
    return str(state.bridge_tmp / f"{name}.json")


def tmp_error_path(state: RunnerState, name: str) -> str:
    path = state.bridge_tmp / f"{name}.error.json"
    write_text(path, "")
    return str(path)


def statement_count(state: RunnerState) -> int:
    return len(state.statements)


def statement_sql(state: RunnerState, index: int) -> str:
    return state.statements[index][2]


def statement_name(state: RunnerState, index: int) -> str:
    script_name, script_index, _ = state.statements[index]
    return f"{script_name}:{script_index}"


def statement_is_copy(state: RunnerState, index: int) -> bool:
    return is_copy_stdin_statement(state.statements[index][2])


def statement_copy_sql(state: RunnerState, index: int) -> str:
    return executable_sql_without_copy_markers(state.statements[index][2])


def statement_copy_payload(state: RunnerState, index: int) -> str:
    return copy_payload_for_statement(state.statements[index][2])


def should_stop(state: RunnerState) -> bool:
    return state.stop_requested


def record_connect(state: RunnerState, elapsed_ns: int) -> None:
    state.api_hits["ScratchBirdConnection"] += 1
    state.api_hits["connect"] += 1
    state.timing_groups["connection"] = state.timing_groups.get("connection", 0) + int(elapsed_ns)
    state.transcript_writer.write({
        "event": "connect",
        "driver": "mojo",
        "route": state.args.route,
        "parser_mode": state.args.parser_mode,
        "page_size": state.args.page_size,
        "language_profile": state.args.language_profile,
        "language_resource_identity": state.args.language_resource_identity,
        "language_resource_hash": state.args.language_resource_hash,
        "syntax_profile": state.args.syntax_profile,
        "topology_profile": state.args.topology_profile,
        "elapsed_ns": int(elapsed_ns),
    })


def record_connect_failure(state: RunnerState, error_path: str, elapsed_ns: int) -> None:
    error = parse_bridge_error(error_path)
    state.timing_groups["connection"] = state.timing_groups.get("connection", 0) + int(elapsed_ns)
    state.failures.append({"statement_id": "connect", "message": error.get("message", "connection failed")})
    state.diagnostic_writer.write({"statement_id": "connect", **error})
    append_log(state.stderr_log_path, json.dumps(error, sort_keys=True) + "\n")


def record_route_probe(state: RunnerState, rc: int, result_path: str, error_path: str, elapsed_ns: int) -> None:
    state.api_hits["execute"] += 1
    state.timing_groups["metadata"] = state.timing_groups.get("metadata", 0) + int(elapsed_ns)
    if int(rc) != 0:
        error = parse_bridge_error(error_path)
        state.route_env = route_environment(state.args, actual_page_size=None, status="fail", reason=str(error.get("message", "SHOW DATABASE failed")))
        state.failures.append({"statement_id": "route_page_size", "message": "route page-size verification failed"})
    else:
        payload = read_json_file(result_path)
        names = [str(column.get("name", "")).lower() for column in payload.get("columns", [])]
        rows = payload.get("rows") or []
        actual: int | None = None
        if rows and "page_size_bytes" in names:
            raw = rows[0][names.index("page_size_bytes")]
            actual = int(raw) if raw is not None else None
        expected = PAGE_SIZE_BYTES[state.args.page_size]
        status = "pass" if actual == expected else "fail"
        reason = None if status == "pass" else "actual_page_size_mismatch"
        state.route_env = route_environment(state.args, actual_page_size=actual, status=status, reason=reason)
        if status != "pass":
            state.failures.append({
                "statement_id": "route_page_size",
                "message": "route page-size verification failed",
                "expected_page_size_bytes": expected,
                "actual_page_size_bytes": actual,
            })
    write_text(state.route_environment_path, json.dumps(state.route_env, indent=2, sort_keys=True) + "\n")


def record_statement(state: RunnerState, index: int, rc: int, result_path: str, error_path: str, elapsed_ns: int) -> None:
    script_name, script_index, statement = state.statements[index]
    statement_id = f"{script_name}:{script_index}"
    group = classify_statement(statement)
    expected_outcome = "refusal" if statement_id in state.expected_refusals else "success"
    row_count = -1
    result_digest = None
    sqlstate = None
    diagnostic_code = None
    actual_outcome = "success"
    if int(rc) == 0:
        payload = read_json_file(result_path)
        rows = payload.get("rows", [])
        row_count = int(payload.get("row_count", len(rows)))
        result_digest = digest_rows(rows)
        state.digests.append({
            "statement_id": statement_id,
            "row_count": row_count,
            "result_digest": result_digest,
        })
        append_log(state.output_path, json.dumps({"statement_id": statement_id, "rows": rows}, default=str) + "\n")
        if expected_outcome == "refusal":
            actual_outcome = "unexpected_success"
            state.failures.append({"statement_id": statement_id, "message": "statement succeeded but was expected to refuse"})
            if state.args.stop_on_error:
                state.stop_requested = True
    else:
        actual_outcome = "refusal"
        error = parse_bridge_error(error_path)
        diagnostic_code = str(error.get("code", "")) or None
        state.diagnostic_writer.write({
            "statement_id": statement_id,
            "sqlstate": sqlstate,
            "diagnostic_code": diagnostic_code,
            "message": error.get("message", "statement refused"),
        })
        append_log(state.error_path, f"{statement_id}: {error.get('message', 'statement refused')}\n")
        if expected_outcome == "success":
            state.failures.append({"statement_id": statement_id, "message": error.get("message", "statement refused")})
            if state.args.stop_on_error:
                state.stop_requested = True
        else:
            state.security_refusals.append({
                "statement_id": statement_id,
                "sqlstate": sqlstate,
                "diagnostic_code": diagnostic_code,
            })
    state.timing_groups[group] = state.timing_groups.get(group, 0) + int(elapsed_ns)
    state.api_hits["prepare"] += 1
    state.api_hits["execute"] += 1
    event = {
        "run_id": state.args.run_id,
        "driver_name": "mojo",
        "driver_version": "1.0.0b2",
        "route": state.args.route,
        "parser_mode": state.args.parser_mode,
        "page_size": state.args.page_size,
        "namespace": state.args.namespace,
        "script": state.args.input,
        "statement_index": index + 1,
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
        "elapsed_ns": int(elapsed_ns),
        "round_trip_count": None,
        "fetch_batch_count": None,
        "server_revalidation_state": "required",
        "language_profile": state.args.language_profile,
        "language_resource_pack": state.args.language_resource_pack,
        "language_resource_identity": state.args.language_resource_identity,
        "language_resource_hash": state.args.language_resource_hash,
        "syntax_profile": state.args.syntax_profile,
        "topology_profile": state.args.topology_profile,
        "standard_english_fallback": state.args.standard_english_fallback,
        "transaction_id_observed": None,
        "mga_authority": "engine",
        "native_api_surface": "mojo_external_call_cpp_client_abi",
        "code_example_section": "connect_prepare_execute_fetch",
    }
    state.event_writer.write(event)
    state.testcases.append(event)


def record_metadata(state: RunnerState, collection: str, rc: int, result_path: str, error_path: str, elapsed_ns: int) -> dict[str, Any]:
    state.api_hits["query_metadata"] += 1
    if int(rc) == 0:
        payload = read_json_file(result_path)
        rows = payload.get("rows", [])
        return {
            "status": "ok",
            "row_count": int(payload.get("row_count", len(rows))),
            "elapsed_ns": int(elapsed_ns),
            "digest": digest_rows(rows),
        }
    error = parse_bridge_error(error_path)
    return {
        "status": "error",
        "elapsed_ns": int(elapsed_ns),
        "error": error.get("message", f"metadata {collection} failed"),
    }


def empty_metadata_collections() -> dict[str, Any]:
    return {}


def set_metadata_collection(collections: dict[str, Any], name: str, record: dict[str, Any]) -> dict[str, Any]:
    collections[name] = record
    return collections


def write_metadata_snapshot(state: RunnerState, collections: dict[str, Any]) -> None:
    snapshots = {
        "driver": "mojo",
        "route": state.args.route,
        "parser_mode": state.args.parser_mode,
        "page_size": state.args.page_size,
        "namespace": state.args.namespace,
        "collections": collections,
    }
    write_text(state.metadata_path, json.dumps(snapshots, indent=2, sort_keys=True, default=str) + "\n")


def finish(state: RunnerState) -> int:
    state.close_writers()
    total_elapsed = now_ns() - state.started_ns
    state.timing_groups["overall"] = total_elapsed
    process_metrics = current_process_metrics()
    if not state.metadata_path.exists():
        write_metadata_snapshot(state, {})
    summary = {
        "run_id": state.args.run_id,
        "driver_name": "mojo",
        "route": state.args.route,
        "parser_mode": state.args.parser_mode,
        "page_size": state.args.page_size,
        "namespace": state.args.namespace,
        "sslmode": state.args.sslmode,
        "transport_mode": transport_mode_for_route(state.args.route, state.args.sslmode),
        "transport_endpoint_kind": endpoint_kind_for_route(state.args.route),
        "driver_transport_implementation": transport_implementation_for_route(state.args.route),
        "cpp_library_boundary": "cpp_client_c_abi_transport_bridge",
        "language_resource_pack": state.args.language_resource_pack,
        "language_resource_identity": state.args.language_resource_identity,
        "language_resource_hash": state.args.language_resource_hash,
        "language_resource_authority": "shared_server_parser_resource_pack",
        "language_profile": state.args.language_profile,
        "syntax_profile": state.args.syntax_profile,
        "topology_profile": state.args.topology_profile,
        "standard_english_fallback": state.args.standard_english_fallback,
        "status": "fail" if state.failures else "pass",
        "statement_count": len(state.statements),
        "failure_count": len(state.failures),
        "elapsed_ns": total_elapsed,
        "process_metrics": process_metrics,
        "server_revalidation_required": True,
        "driver_or_parser_finality": "forbidden",
        "mga_authority": "engine",
        "artifacts": {
            "command-events.jsonl": str(state.command_events_path),
            "summary.json": str(state.summary_path),
            "diagnostics.jsonl": str(state.diagnostics_path),
            "wire-transcript.jsonl": str(state.wire_transcript_path),
            "timing-groups.json": str(state.timing_groups_path),
            "result-digests.json": str(state.result_digests_path),
            "metadata-snapshots.json": str(state.metadata_path),
            "route-environment.json": str(state.route_environment_path),
            "process-metrics.jsonl": str(state.process_metrics_path),
            "security-refusals.json": str(state.security_refusals_path),
            "native-api-coverage.json": str(state.native_api_path),
            "code-example-review.json": str(state.code_review_path),
            "junit.xml": str(state.junit_path),
            "stdout.log": str(state.stdout_log_path),
            "stderr.log": str(state.stderr_log_path),
        },
    }
    write_text(state.summary_path, json.dumps(summary, indent=2, sort_keys=True, default=str) + "\n")
    process_metrics_record = {
        "role": "client",
        "rss_kb": process_metrics["client"]["last_rss_kb"],
        "vsize_kb": process_metrics["client"]["last_vsize_kb"],
    }
    process_metrics_jsonl = json.dumps(process_metrics_record, sort_keys=True) + "\n"
    write_text(state.metrics_path, process_metrics_jsonl)
    write_text(state.process_metrics_path, process_metrics_jsonl)
    write_text(state.timing_groups_path, json.dumps(state.timing_groups, indent=2, sort_keys=True) + "\n")
    write_text(state.result_digests_path, json.dumps(state.digests, indent=2, sort_keys=True) + "\n")
    write_text(state.security_refusals_path, json.dumps(state.security_refusals, indent=2, sort_keys=True) + "\n")
    write_text(state.native_api_path, json.dumps(state.api_hits, indent=2, sort_keys=True) + "\n")
    write_text(
        state.code_review_path,
        json.dumps(
            {
                "driver": "mojo",
                "public_api_only": True,
                "shells_out_to_other_driver": False,
                "source_is_canonical_example": True,
                "sections": [
                    "connection",
                    "prepared_execution",
                    "fetch",
                    "diagnostics",
                    "metadata",
                    "transaction",
                    "artifact_routing",
                    "language_resources",
                ],
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
    )
    write_text(state.junit_path, junit_xml(state.testcases or [{"statement_id": "run", "elapsed_ns": total_elapsed}], state.failures))
    append_log(state.stdout_log_path, f"sb_isql_mojo status={summary['status']} statements={len(state.statements)}\n")
    return 1 if state.failures else 0
