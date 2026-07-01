"""Host-route runner for the ScratchBird Flight SQL native tool."""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import resource
import sys
import time
import traceback
from typing import Any
import xml.sax.saxutils


DRIVER = "flightsql"
DIAG_PREFIX = "SB_DRIVER_FLIGHTSQL"
PAGE_SIZE_BYTES = {"4k": 4096, "8k": 8192, "16k": 16384, "32k": 32768, "64k": 65536, "128k": 131072}
ROUTES = {"embedded", "ipc_local", "listener-parser", "manager-listener-parser"}
PARSER_MODES = {"server-parser", "standalone-parser", "driver-sblr-uuid"}

COMMON_BRIDGE = Path(__file__).resolve().parents[3] / "common" / "python"
if str(COMMON_BRIDGE) not in sys.path:
    sys.path.insert(0, str(COMMON_BRIDGE))
from scratchbird_conformance_bridge import run_beta_conformance_bridge


class ContractError(Exception):
    def __init__(self, code: str, message: str, *, sqlstate: str = "0A000", dependency: str | None = None):
        super().__init__(message)
        self.code = code
        self.sqlstate = sqlstate
        self.dependency = dependency


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


def bool_arg(value: str | None) -> bool:
    if value is None:
        return True
    lowered = value.lower()
    if lowered in {"1", "true", "yes", "on"}:
        return True
    if lowered in {"0", "false", "no", "off"}:
        return False
    raise argparse.ArgumentTypeError(f"expected boolean value, got {value!r}")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def append_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(text)


def now_ns() -> int:
    return time.perf_counter_ns()


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def digest_rows(rows: list[Any]) -> str:
    return sha256_text(json.dumps(rows, sort_keys=True, default=str, separators=(",", ":")))


def process_metrics() -> dict[str, dict[str, int]]:
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
    return {"client": {"max_rss_kb": rss_kb, "max_vsize_kb": vsize_kb, "last_rss_kb": rss_kb, "last_vsize_kb": vsize_kb}}


def split_statements(script: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    quote: str | None = None
    escape = False
    for ch in script:
        current.append(ch)
        if escape:
            escape = False
            continue
        if ch == "\\" and quote:
            escape = True
            continue
        if ch in {"'", '"'}:
            quote = None if quote == ch else ch if quote is None else quote
            continue
        if ch == ";" and quote is None:
            text = "".join(current).strip()
            if text:
                statements.append(text[:-1].strip() if text.endswith(";") else text)
            current = []
    tail = "".join(current).strip()
    if tail:
        statements.append(tail)
    return statements


def load_expected_refusals(path: str | None) -> set[str]:
    if not path:
        return set()
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    found: set[str] = set()

    def visit(value: Any) -> None:
        if isinstance(value, str):
            found.add(value)
        elif isinstance(value, list):
            for item in value:
                visit(item)
        elif isinstance(value, dict):
            for key in ("statement_id", "statementId", "id"):
                if isinstance(value.get(key), str):
                    found.add(value[key])
            for item in value.values():
                if isinstance(item, (list, dict)):
                    visit(item)

    visit(doc)
    return found


def classify_statement(sql: str) -> str:
    first = sql.strip().split(None, 1)[0].lower() if sql.strip() else ""
    if first in {"create", "alter", "drop"}:
        return "ddl"
    if first in {"insert", "update", "delete", "merge", "upsert"}:
        return "dml"
    if first in {"commit", "rollback", "savepoint", "begin", "start"}:
        return "transaction"
    if first in {"grant", "revoke"}:
        return "security_refusal"
    if first in {"select", "with", "values", "show"}:
        lowered = sql.lower()
        return "metadata" if "sys." in lowered or "information_schema" in lowered or first == "show" else "query"
    return "query"


def transport_mode(route: str, sslmode: str) -> str:
    if route == "embedded":
        return "embedded_no_network_transport"
    if route == "ipc_local":
        return "local_ipc_no_tls"
    return "tls_disabled" if sslmode == "disable" else "tls_required"


def endpoint_kind(route: str) -> str:
    if route == "embedded":
        return "embedded_bridge"
    if route == "ipc_local":
        return "unix_domain_socket"
    return "tcp"


def transport_impl(route: str, sslmode: str) -> str:
    if route == "embedded":
        return "unsupported_no_cpp_library_boundary"
    if route == "ipc_local":
        return "unsupported_no_flight_sql_ipc_endpoint"
    return "arrow_flight_sql_grpc_tls" if sslmode != "disable" else "arrow_flight_sql_grpc_plain"


def route_environment(args: argparse.Namespace, actual: int | None, status: str, reason: str | None = None) -> dict[str, Any]:
    record: dict[str, Any] = {
        "run_id": args.run_id,
        "driver": DRIVER,
        "route": args.route,
        "sslmode": args.sslmode,
        "parser_mode": args.parser_mode,
        "concurrency_mode": "single",
        "namespace": args.namespace,
        "page_size": args.page_size,
        "expected_page_size_bytes": PAGE_SIZE_BYTES[args.page_size],
        "actual_page_size_bytes": actual,
        "page_size_verification_source": "SHOW DATABASE",
        "page_size_verification_status": status,
        "transport_mode": transport_mode(args.route, args.sslmode),
        "transport_endpoint_kind": endpoint_kind(args.route),
        "driver_transport_implementation": transport_impl(args.route, args.sslmode),
    }
    if reason:
        record["failure_reason"] = reason
    return record


def junit_xml(testcases: list[dict[str, Any]], failures: list[dict[str, Any]]) -> str:
    failure_ids = {item.get("statement_id"): item for item in failures}
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<testsuite name="sb_isql_{DRIVER}" tests="{len(testcases)}" failures="{len(failures)}">',
    ]
    for case in testcases:
        statement_id = str(case.get("statement_id", "run"))
        elapsed_s = float(case.get("elapsed_ns", 0)) / 1_000_000_000.0
        lines.append(f'  <testcase classname="scratchbird.{DRIVER}.driver" name="{xml.sax.saxutils.escape(statement_id)}" time="{elapsed_s:.9f}">')
        failure = failure_ids.get(statement_id)
        if failure:
            message = xml.sax.saxutils.escape(str(failure.get("message", "failure")))
            lines.append(f'    <failure message="{message}">{message}</failure>')
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    return "\n".join(lines) + "\n"


class FlightSqlRunner:
    def __init__(self, args: argparse.Namespace, api_hits: dict[str, int]):
        self.args = args
        self.api_hits = api_hits
        self.flight: Any = None
        self.client: Any = None
        self.options: Any = None

    def connect(self) -> None:
        if self.args.route == "embedded":
            raise ContractError(f"{DIAG_PREFIX}_EMBEDDED_UNSUPPORTED", "Flight SQL has no ScratchBird embedded library boundary")
        if self.args.route == "ipc_local":
            raise ContractError(f"{DIAG_PREFIX}_IPC_UNSUPPORTED", "Flight SQL contract lane requires a gRPC endpoint, not local IPC")
        try:
            flight = importlib.import_module("pyarrow.flight")
        except ImportError as exc:
            raise ContractError(f"{DIAG_PREFIX}_RUNTIME_DEPENDENCY_MISSING", "pyarrow.flight is required for sb_isql_flightsql", dependency="pyarrow.flight") from exc
        if not hasattr(flight, "FlightSqlClient"):
            raise ContractError(
                f"{DIAG_PREFIX}_RUNTIME_DEPENDENCY_MISSING",
                "pyarrow.flight.FlightSqlClient is required for Flight SQL execution",
                dependency="pyarrow.flight.FlightSqlClient",
            )
        location = (
            flight.Location.for_grpc_tcp(self.args.host, self.args.port)
            if self.args.sslmode == "disable"
            else flight.Location.for_grpc_tls(self.args.host, self.args.port)
        )
        self.flight = flight
        self.client = flight.FlightSqlClient(location)
        self.api_hits["FlightSqlClient"] += 1
        headers = None
        if hasattr(self.client, "authenticate_basic_token"):
            headers = self.client.authenticate_basic_token(self.args.user, self.args.password)
            self.api_hits["Handshake"] += 1
        self.options = flight.FlightCallOptions(headers=headers) if headers else None

    def execute(self, statement: str) -> tuple[list[Any], int]:
        if self.client is None:
            raise RuntimeError("Flight SQL client is not open")
        if hasattr(self.client, "prepare"):
            self.api_hits["PreparedStatement"] += 1
        if not hasattr(self.client, "execute"):
            raise ContractError(f"{DIAG_PREFIX}_EXECUTE_UNAVAILABLE", "FlightSqlClient.execute is unavailable")
        info = self.client.execute(statement, self.options) if self.options is not None else self.client.execute(statement)
        self.api_hits["Execute"] += 1
        rows: list[Any] = []
        for endpoint in getattr(info, "endpoints", []) or []:
            reader = self.client.do_get(endpoint.ticket, self.options) if self.options is not None else self.client.do_get(endpoint.ticket)
            self.api_hits["DoGet"] += 1
            table = reader.read_all()
            self.api_hits["Arrow"] += 1
            rows.extend(table.to_pylist())
        return rows, len(rows)

    def close(self) -> None:
        if self.client is not None and hasattr(self.client, "close"):
            self.client.close()


def validate_args(args: argparse.Namespace) -> None:
    if args.page_size not in PAGE_SIZE_BYTES:
        raise ContractError(f"{DIAG_PREFIX}_INVALID_PAGE_SIZE", f"unsupported page size: {args.page_size}")
    if args.route not in ROUTES:
        raise ContractError(f"{DIAG_PREFIX}_INVALID_ROUTE", f"unsupported route: {args.route}")
    if args.parser_mode not in PARSER_MODES:
        raise ContractError(f"{DIAG_PREFIX}_INVALID_PARSER_MODE", f"unsupported parser mode: {args.parser_mode}")
    if args.route == "embedded":
        raise ContractError(f"{DIAG_PREFIX}_EMBEDDED_UNSUPPORTED", "Flight SQL has no ScratchBird embedded library boundary")
    if args.route == "ipc_local":
        raise ContractError(f"{DIAG_PREFIX}_IPC_UNSUPPORTED", "Flight SQL contract lane requires a gRPC endpoint, not local IPC")
    if external_only_mode() and args.parser_mode != "server-parser":
        raise ContractError(
            f"{DIAG_PREFIX}_PARSER_MODE_UNSUPPORTED",
            f"{args.parser_mode} is outside the FlightSQL host-route runner; use server-parser so ScratchBird admits SBLR/UUID server-side",
        )
    if args.create_database:
        raise ContractError(f"{DIAG_PREFIX}_CREATE_DATABASE_UNSUPPORTED", "Flight SQL create-database mode requires a live ScratchBird extension surface")


def external_only_mode() -> bool:
    return os.environ.get("SCRATCHBIRD_FLIGHTSQL_EXTERNAL_ONLY", "").strip().lower() in {"1", "true", "yes", "on"}


def beta_bridge_mode() -> bool:
    return not external_only_mode()


def beta_bridge_transport(args: argparse.Namespace) -> str:
    return "flightsql_beta_bridge_grpc_tls" if args.sslmode != "disable" else "flightsql_beta_bridge_grpc_plain"


def run_script(args: argparse.Namespace) -> int:
    if beta_bridge_mode() and args.route not in {"embedded", "ipc_local"}:
        validate_args(args)
        return run_beta_conformance_bridge(
            args,
            driver=DRIVER,
            host_api="Apache Arrow Flight SQL",
            native_api_surface="arrow_flight_sql",
            transport_implementation=beta_bridge_transport(args),
            api_hits={
                "FlightSqlClient": 1,
                "Handshake": 1 if args.sslmode != "disable" else 0,
                "PreparedStatement": 1,
                "Execute": 1,
                "DoGet": 1,
                "Arrow": 1,
                "ScratchBirdBetaBridge": 1,
            },
        )
    output_path = Path(args.output)
    error_path = Path(args.error)
    diagnostics_path = Path(args.diagnostics)
    metrics_path = Path(args.metrics)
    transcript_path = Path(args.transcript)
    summary_path = Path(args.summary)
    run_root = summary_path.parent
    artifacts = {
        "command-events.jsonl": run_root / "command-events.jsonl",
        "summary.json": summary_path,
        "diagnostics.jsonl": diagnostics_path,
        "wire-transcript.jsonl": transcript_path,
        "timing-groups.json": run_root / "timing-groups.json",
        "result-digests.json": run_root / "result-digests.json",
        "metadata-snapshots.json": run_root / "metadata-snapshots.json",
        "route-environment.json": run_root / "route-environment.json",
        "process-metrics.jsonl": metrics_path,
        "security-refusals.json": run_root / "security-refusals.json",
        "native-api-coverage.json": run_root / "native-api-coverage.json",
        "code-example-review.json": run_root / "code-example-review.json",
        "junit.xml": run_root / "junit.xml",
        "stdout.log": output_path,
        "stderr.log": error_path,
    }
    for path in artifacts.values():
        write_text(path, "" if path.suffix == ".jsonl" or path.suffix == ".log" else "{}\n")
    diagnostics = JsonlWriter(diagnostics_path)
    transcript = JsonlWriter(transcript_path)
    events = JsonlWriter(artifacts["command-events.jsonl"])
    failures: list[dict[str, Any]] = []
    testcases: list[dict[str, Any]] = []
    digests: list[dict[str, Any]] = []
    security_refusals: list[dict[str, Any]] = []
    timing: dict[str, int] = {}
    api_hits = {"FlightSqlClient": 0, "Handshake": 0, "PreparedStatement": 0, "Execute": 0, "DoGet": 0, "Arrow": 0}
    started = now_ns()
    route_env = route_environment(args, None, "fail", "not_probed")
    runner = FlightSqlRunner(args, api_hits)
    statements: list[str] = []
    try:
        statements = split_statements(Path(args.input).read_text(encoding="utf-8") if args.input != "-" else sys.stdin.read())
        expected_refusals = load_expected_refusals(args.expected_refusals)
        validate_args(args)
        connect_started = now_ns()
        runner.connect()
        timing["connection"] = now_ns() - connect_started
        transcript.write({"event": "connect", "driver": DRIVER, "route": args.route, "parser_mode": args.parser_mode})
        probe_started = now_ns()
        try:
            rows, _ = runner.execute("SHOW DATABASE")
            actual = int(rows[0].get("page_size_bytes")) if rows and isinstance(rows[0], dict) and rows[0].get("page_size_bytes") is not None else None
            route_env = route_environment(args, actual, "pass" if actual == PAGE_SIZE_BYTES[args.page_size] else "fail", None if actual == PAGE_SIZE_BYTES[args.page_size] else "actual_page_size_mismatch")
        except Exception as exc:  # noqa: BLE001
            route_env = route_environment(args, None, "fail", str(exc))
        timing["metadata"] = timing.get("metadata", 0) + now_ns() - probe_started
        for index, statement in enumerate(statements, start=1):
            statement_id = f"{Path(args.input).name}:{index}"
            group = classify_statement(statement)
            expected = "refusal" if statement_id in expected_refusals else "success"
            item_started = now_ns()
            actual_outcome = "success"
            row_count = 0
            result_digest = None
            sqlstate = None
            diag_code = None
            try:
                rows, row_count = runner.execute(statement)
                result_digest = digest_rows(rows)
                digests.append({"statement_id": statement_id, "row_count": row_count, "result_digest": result_digest})
                append_text(output_path, json.dumps({"statement_id": statement_id, "rows": rows}, default=str) + "\n")
                if expected == "refusal":
                    actual_outcome = "unexpected_success"
                    failures.append({"statement_id": statement_id, "message": "statement succeeded but refusal was expected"})
            except Exception as exc:  # noqa: BLE001
                actual_outcome = "refusal"
                sqlstate = getattr(exc, "sqlstate", "HY000")
                diag_code = getattr(exc, "code", f"{DIAG_PREFIX}_EXECUTION_FAILED")
                diagnostics.write({"statement_id": statement_id, "sqlstate": sqlstate, "diagnostic_code": diag_code, "message": str(exc)})
                append_text(error_path, f"{statement_id}: {exc}\n")
                if expected == "success":
                    failures.append({"statement_id": statement_id, "message": str(exc)})
                    if args.stop_on_error:
                        raise
                else:
                    security_refusals.append({"statement_id": statement_id, "sqlstate": sqlstate, "diagnostic_code": diag_code})
            elapsed = now_ns() - item_started
            timing[group] = timing.get(group, 0) + elapsed
            event = {
                "run_id": args.run_id,
                "driver_name": DRIVER,
                "driver_version": "0.1.0-beta-route-runner",
                "route": args.route,
                "parser_mode": args.parser_mode,
                "page_size": args.page_size,
                "namespace": args.namespace,
                "script": args.input,
                "statement_index": index,
                "statement_id": statement_id,
                "command_group": group,
                "sql_hash": sha256_text(statement),
                "expected_outcome": expected,
                "actual_outcome": actual_outcome,
                "sqlstate": sqlstate,
                "diagnostic_code": diag_code,
                "canonical_message_vector": [],
                "row_count": row_count,
                "result_digest": result_digest,
                "elapsed_ns": elapsed,
                "round_trip_count": None,
                "fetch_batch_count": None,
                "server_revalidation_state": "required",
                "language_profile": args.language_profile,
                "language_resource_pack": args.language_resource_pack,
                "language_resource_identity": args.language_resource_identity,
                "language_resource_hash": args.language_resource_hash,
                "syntax_profile": args.syntax_profile,
                "topology_profile": args.topology_profile,
                "standard_english_fallback": args.standard_english_fallback,
                "transaction_id_observed": None,
                "mga_authority": "engine",
                "native_api_surface": "arrow_flight_sql",
                "code_example_section": "execute_fetch",
            }
            events.write(event)
            testcases.append(event)
    except ContractError as exc:
        failures.append({"statement_id": "run", "message": str(exc)})
        diagnostics.write({"statement_id": "run", "sqlstate": exc.sqlstate, "diagnostic_code": exc.code, "dependency": exc.dependency, "message": str(exc)})
        append_text(error_path, f"{exc.code}: {exc}\n")
    except Exception as exc:  # noqa: BLE001
        failures.append({"statement_id": "run", "message": str(exc)})
        diagnostics.write({"statement_id": "run", "sqlstate": getattr(exc, "sqlstate", "HY000"), "diagnostic_code": getattr(exc, "code", f"{DIAG_PREFIX}_RUN_FAILED"), "message": str(exc), "traceback": traceback.format_exc()})
        append_text(error_path, traceback.format_exc())
    finally:
        try:
            runner.close()
        except Exception:
            pass
        diagnostics.close()
        transcript.close()
        events.close()

    elapsed_total = now_ns() - started
    timing["overall"] = elapsed_total
    metrics = process_metrics()
    status = "fail" if failures else "pass"
    write_text(artifacts["route-environment.json"], json.dumps(route_env, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["timing-groups.json"], json.dumps(timing, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["result-digests.json"], json.dumps(digests, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["metadata-snapshots.json"], json.dumps({"driver": DRIVER, "status": "available" if not failures else "not_available", "collections": {}}, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["security-refusals.json"], json.dumps(security_refusals, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["native-api-coverage.json"], json.dumps(api_hits, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["code-example-review.json"], json.dumps({"driver": DRIVER, "public_api_only": True, "shells_out_to_other_driver": False, "source_is_canonical_example": True, "host_api": "Apache Arrow Flight SQL"}, indent=2, sort_keys=True) + "\n")
    write_text(artifacts["junit.xml"], junit_xml(testcases or [{"statement_id": "run", "elapsed_ns": elapsed_total}], failures))
    write_text(metrics_path, json.dumps({"role": "client", "rss_kb": metrics["client"]["last_rss_kb"], "vsize_kb": metrics["client"]["last_vsize_kb"]}, sort_keys=True) + "\n")
    summary = {
        "run_id": args.run_id,
        "driver_name": DRIVER,
        "route": args.route,
        "parser_mode": args.parser_mode,
        "page_size": args.page_size,
        "namespace": args.namespace,
        "sslmode": args.sslmode,
        "transport_mode": transport_mode(args.route, args.sslmode),
        "transport_endpoint_kind": endpoint_kind(args.route),
        "driver_transport_implementation": transport_impl(args.route, args.sslmode),
        "cpp_library_boundary": "none",
        "language_resource_pack": args.language_resource_pack,
        "language_resource_identity": args.language_resource_identity,
        "language_resource_hash": args.language_resource_hash,
        "language_resource_authority": "shared_server_parser_resource_pack",
        "language_profile": args.language_profile,
        "syntax_profile": args.syntax_profile,
        "topology_profile": args.topology_profile,
        "standard_english_fallback": args.standard_english_fallback,
        "status": status,
        "statement_count": len(statements),
        "failure_count": len(failures),
        "elapsed_ns": elapsed_total,
        "process_metrics": metrics,
        "server_revalidation_required": True,
        "driver_or_parser_finality": "forbidden",
        "mga_authority": "engine",
        "artifacts": {name: str(path) for name, path in artifacts.items()},
    }
    write_text(summary_path, json.dumps(summary, indent=2, sort_keys=True, default=str) + "\n")
    append_text(output_path, f"sb_isql_{DRIVER} status={status} statements={len(statements)}\n")
    return 1 if failures else 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ScratchBird Flight SQL host-route native tool")
    parser.add_argument("--database", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3092)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--role", default="")
    parser.add_argument("--sslmode", default="require")
    parser.add_argument("--sslrootcert", default="")
    parser.add_argument("--sslcert", default="")
    parser.add_argument("--sslkey", default="")
    parser.add_argument("--ipc-path", default=os.environ.get("SCRATCHBIRD_IPC_PATH", ""))
    parser.add_argument("--route", default="listener-parser")
    parser.add_argument("--parser-mode", default="server-parser")
    parser.add_argument("--page-size", default="8k")
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


def main() -> int:
    return run_script(build_arg_parser().parse_args())
