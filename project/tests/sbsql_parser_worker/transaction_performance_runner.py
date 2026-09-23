#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exploratory real-ScratchBird insert/commit measurements with row verification.

Serial embedded route only in this first executable lane. It exercises the real
ScratchBird engine, never a substitute database. CLI execute timings exclude
result display; process timings include attach/authentication/teardown. Neither
is represented as steady-state network request latency or complete ACID proof.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from cdp_database_lifecycle_support import seed_database, PUBLIC_TEST_PASSWORD

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from transaction_phase_summary import summarize, TraceError
from transaction_sync_summary import summarize_sync, SyncTraceError


class BenchmarkError(RuntimeError):
    pass


TRACE_ENV = {
    "SCRATCHBIRD_TRANSACTION_API_PHASE_TRACE_FILE": "transaction.tsv",
    "SCRATCHBIRD_LOCAL_TXN_STORE_PHASE_TRACE_FILE": "inventory.tsv",
}
TIMING = re.compile(r"^Time: ([0-9]+(?:\.[0-9]+)?) ms$")
BEGIN = "SB_TXP_MEASURE_BEGIN"
END = "SB_TXP_MEASURE_END"
COMMAND = "SB_TXP_COMMAND_"
COMMIT_ACK = "Transaction committed; replacement transaction is active."


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def workload(rows: int, batch: int, mode: str) -> tuple[str, list[str]]:
    if rows < 1 or batch < 1 or mode not in {"explicit", "autocommit"}:
        raise BenchmarkError("positive rows/batch and an explicit completion mode required")
    if mode == "autocommit" and batch != 1:
        raise BenchmarkError("autocommit cannot silently batch multiple statements")
    statements = [f"\\echo {BEGIN}", "\\timing on"]
    operations = []
    for index in range(1, rows + 1):
        # A meta-command flushes the CLI's pending INSERT batch. Each requested
        # statement must remain its own execute call, especially in autocommit.
        statements.append(f"\\echo {COMMAND}{len(operations) + 1}")
        statements.append(f"INSERT INTO txp_events (id, amount) VALUES ({index}, {index * 3});")
        operations.append("insert")
        if mode == "explicit" and (index % batch == 0 or index == rows):
            # Operate on the active default, then consume the server replacement.
            # A hidden BEGIN here would add an independent transaction in SB.
            statements.extend([f"\\echo {COMMAND}{len(operations) + 1}", "COMMIT;"])
            operations.append("commit")
    # The CLI otherwise commits outstanding script work at EOF. Roll back the
    # remaining active transaction explicitly outside the measured command set,
    # so that EOF cannot rescue broken per-statement autocommit or a missing
    # requested commit. Reopen verification must see only already-committed rows.
    statements.extend(["\\timing off", f"\\echo {END}", "ROLLBACK;"])
    return "\n".join(statements) + "\n", operations


def empty_commit_workload(commits: int, batch: int, mode: str) -> tuple[str, list[str]]:
    """Control lane: finalize the live default without INSERT or hidden BEGIN.

    This does not request a READ ONLY policy and does not prove replacement IDs
    or durable inventory transitions individually. Those need separate gates.
    """
    if commits < 1 or batch != 1 or mode != "explicit":
        raise BenchmarkError("empty-commit control requires positive commits, explicit mode and batch 1")
    statements = [f"\\echo {BEGIN}", "\\timing on"]
    for ordinal in range(1, commits + 1):
        statements.extend([f"\\echo {COMMAND}{ordinal}", "COMMIT;"])
    statements.extend(["\\timing off", f"\\echo {END}", "ROLLBACK;"])
    return "\n".join(statements) + "\n", ["commit"] * commits


def verify_empty_table(stdout: str) -> str:
    # An empty stdout is not sufficient: require the real COUNT(*) result so a
    # missing result cannot masquerade as successful zero-row verification.
    if [line.strip() for line in stdout.splitlines() if line.strip()] != ["0"]:
        raise BenchmarkError("reopened empty-table COUNT(*) verification failed")
    return hashlib.sha256(b"0\n").hexdigest()


def prepare_history(args, database: Path, directory: Path) -> dict:
    """Prior real commit requests, outside measurement; not an inventory-row count.

    Same database but a separate live CLI process. Do not inherit measurement
    tracing, insert rows, create hidden BEGINs, or alter cleanup/retention policy.
    """
    if args.history_commits < 0:
        raise BenchmarkError("history commit count cannot be negative")
    if args.history_commits == 0:
        return {"requested_commits": 0, "status": "not_requested",
                "retained_inventory_entries": None}
    sql, operations = empty_commit_workload(args.history_commits, 1, "explicit")
    completed = run_sql(args, database, directory, "history", sql, "explicit")
    samples = execution_samples(completed["stdout"], operations)
    verified = run_sql(args, database, directory, "history_verify",
                       "SELECT COUNT(*) FROM txp_events;\n", "explicit")
    verified_hash = verify_empty_table(verified["stdout"])
    return {"requested_commits": args.history_commits, "status": "acknowledged_empty_table_verified",
            "retained_inventory_entries": None, "result_sha256": verified_hash,
            "command_samples": samples, "script_sha256": completed["script_sha256"],
            "process_wall_seconds": completed["process_wall_seconds"]}


def execution_samples(stdout: str, operations: list[str]) -> list[dict]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if lines.count(BEGIN) != 1 or lines.count(END) != 1:
        raise BenchmarkError("missing/duplicate measurement markers")
    begin, end = lines.index(BEGIN), lines.index(END)
    if end <= begin:
        raise BenchmarkError("measurement markers out of order")
    blocks = []
    for line in lines[begin + 1:end]:
        if line in {"Timing is on", "Timing is off"}:
            continue
        if line == f"{COMMAND}{len(blocks) + 1}":
            blocks.append([])
        elif blocks:
            blocks[-1].append(line)
        else:
            raise BenchmarkError(f"output before first command marker: {line!r}")
    if not operations or len(blocks) != len(operations):
        raise BenchmarkError("missing/duplicate/out-of-order command markers")
    samples = []
    for ordinal, (operation, block) in enumerate(zip(operations, blocks), 1):
        timings = []
        acknowledgements = 0
        affected = []
        for line in block:
            match = TIMING.fullmatch(line)
            if match:
                elapsed = float(match.group(1))
                if not math.isfinite(elapsed):
                    raise BenchmarkError("nonfinite command timing")
                timings.append(elapsed)
            elif operation == "commit" and line == COMMIT_ACK:
                acknowledgements += 1
            elif operation == "insert" and line == "Rows affected: 1":
                affected.append(1)
            else:
                raise BenchmarkError(f"unexpected command {ordinal} output: {line!r}")
        if (operation not in {"insert", "commit"} or len(affected) > 1
                or (operation == "insert" and len(timings) != 1)
                or (operation == "commit" and (acknowledgements != 1 or len(timings) > 1))):
            raise BenchmarkError(f"incomplete/duplicate command {ordinal} evidence")
        samples.append({"command_ordinal": ordinal, "operation": operation,
                        "cli_execute_ms": timings[0] if timings else None,
                        "timing_status": "measured" if timings else "unavailable_embedded_cli"})
    if not samples:
        raise BenchmarkError("no measured commands")
    return samples


def verify_rows(stdout: str, rows: int) -> str:
    actual = [line.strip() for line in stdout.splitlines() if line.strip()]
    expected = [f"{index}|{index * 3}" for index in range(1, rows + 1)]
    if actual != expected:
        raise BenchmarkError(f"reopened row verification failed: expected {rows} exact rows, got {len(actual)} lines")
    return hashlib.sha256(("\n".join(actual) + "\n").encode()).hexdigest()


def statistics(values: list[float]) -> dict:
    if not values or any(not math.isfinite(value) or value < 0 for value in values):
        raise BenchmarkError("invalid or empty timing samples")
    values = sorted(values)
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    return {"count": len(values), "mean": mean, "stddev": math.sqrt(variance),
            "min": values[0], "max": values[-1],
            "p50": values[max(0, math.ceil(len(values) * .50) - 1)],
            "p95": values[max(0, math.ceil(len(values) * .95) - 1)],
            "p99": values[max(0, math.ceil(len(values) * .99) - 1)],
            "percentile_method": "nearest_rank", "samples": values}


def clean_environment() -> dict[str, str]:
    # Do not inherit another agent's trace/fault selectors. Refuse fault selectors
    # rather than hiding them and claiming the requested run was honored.
    faults = [key for key in os.environ if key.startswith(("SCRATCHBIRD_", "SB_"))
              and any(word in key for word in ("FAULT", "CRASH", "INJECT", "_TEST_"))]
    if faults:
        raise BenchmarkError("unset inherited test/fault selectors before benchmarking: " + ", ".join(sorted(faults)))
    return {key: value for key, value in os.environ.items()
            if not (key.startswith(("SCRATCHBIRD_", "SB_")) and "TRACE" in key)}


def collect_traces(directory: Path, minimum_commits: int) -> list[dict]:
    """Require both complete trace families; never combine nested elapsed time."""
    reports = []
    for selector, filename in TRACE_ENV.items():
        path = directory / filename
        try:
            with path.open(encoding="utf-8", newline="") as source:
                report = summarize(source)
        except (OSError, UnicodeError, TraceError) as exc:
            raise BenchmarkError(f"invalid requested trace {filename}: {exc}") from exc
        reports.append({"selector": selector, "source": str(path),
                        "sha256": sha256(path), **report})
    # Persist complete parsed evidence even when it contains failed operations.
    # A trace error is not evidence that an acknowledged commit rolled back.
    with (directory / "phase-summary.json").open("x", encoding="utf-8") as output:
        json.dump({"traces": reports}, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")
    if any(not operation["ok"] for report in reports for operation in report["operations"]):
        raise BenchmarkError("requested traces contain failed operations; inspect phase-summary.json")
    # Necessary evidence only, not request correlation: attach and teardown may
    # add other records. Do not pretend these aggregate counts identify each row.
    required = {"transaction.tsv": {"transaction.commit", "transaction.autocommit_commit_and_begin"},
                "inventory.tsv": {"persist_local_transaction_inventory"}}
    for report in reports:
        count = sum(operation["record_count"] for operation in report["operations"]
                    if operation["operation_id"] in required[Path(report["source"]).name]
                    and operation["ok"])
        if count < minimum_commits:
            raise BenchmarkError("requested trace lacks the minimum commit/publication evidence")
    return reports


def collect_sync_trace(directory: Path) -> dict:
    path = directory / "sync.strace"
    try:
        with path.open(encoding="utf-8", newline="") as source:
            report = {**summarize_sync(source), "source": str(path), "sha256": sha256(path)}
    except (OSError, UnicodeError, SyncTraceError) as exc:
        raise BenchmarkError(f"invalid requested sync trace: {exc}") from exc
    with (directory / "sync-summary.json").open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")
    if report["failed_syscall_count"]:
        raise BenchmarkError("sync syscalls failed; inspect sync-summary.json")
    return report


def sync_command(tracer: Path, directory: Path, command: list[str]) -> list[str]:
    # Trace no SQL, passwords, writes, or buffers. Kill tracees on tracer death
    # so the subprocess timeout cannot leave this measured engine running.
    if (directory / "sync.strace").exists():
        raise BenchmarkError("refusing to overwrite sync trace")
    return [str(tracer), "-f", "-qqq", "-ttt", "-T", "-yy", "--kill-on-exit",
            "-e", "signal=none", "-e", "trace=fsync,fdatasync",
            "-o", str(directory / "sync.strace"), "--", *command]


def run_sql(args, database: Path, directory: Path, label: str, sql: str,
            mode: str, *, traced: bool = False, sync_tracer: Path | None = None) -> dict:
    script = directory / f"{label}.sql"
    script.write_text(sql, encoding="utf-8")
    environment = clean_environment()
    if traced:
        environment.update({name: str(directory / filename)
                            for name, filename in TRACE_ENV.items()})
    command = [str(args.sb_isql), str(database), "--mode=embedded",
               "--sslmode=disable", "-U", "alice", "-P", PUBLIC_TEST_PASSWORD,
               f"--conn-opt=autocommit={'true' if mode == 'autocommit' else 'false'}",
               "-q", "-A", "-t", "-b", "-f", str(script)]
    if sync_tracer is not None:
        command = sync_command(sync_tracer, directory, command)
    out, err = directory / f"{label}.out", directory / f"{label}.err"
    started = time.perf_counter()
    with out.open("wb") as stdout, err.open("wb") as stderr:
        try:
            completed = subprocess.run(command, stdout=stdout, stderr=stderr,
                                       env=environment, timeout=args.timeout, check=False)
        except subprocess.TimeoutExpired as exc:
            raise BenchmarkError(f"{label} timed out; outcome not classified as rollback") from exc
    elapsed = time.perf_counter() - started
    text = out.read_text(encoding="utf-8")
    errors = err.read_text(encoding="utf-8")
    if completed.returncode != 0 or re.search(r"(?im)^\s*(error:|fatal:)", text + "\n" + errors):
        raise BenchmarkError(f"{label} failed rc={completed.returncode}; see {out} and {err}")
    return {"stdout": text, "stderr": errors, "process_wall_seconds": elapsed,
            "script_sha256": sha256(script), "returncode": completed.returncode}


def provenance(args) -> dict:
    def git(*options: str) -> bytes:
        return subprocess.check_output(["git", "-C", str(args.source_root), *options])
    cache = args.build_dir / "CMakeCache.txt"
    content = cache.read_text(encoding="utf-8")
    required = ["CMAKE_BUILD_TYPE:STRING=Release"] + [
        f"{name}:BOOL=OFF" for name in (
            "SCRATCHBIRD_ENABLE_DEBUG_LOGS", "SCRATCHBIRD_ENABLE_HOTPATH_TRACE",
            "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE", "SCRATCHBIRD_ENABLE_PREPARED_TRACE",
            "SB_ENABLE_TEST_CRASH_INJECTION", "SB_ENABLE_TEST_DML_ROUTE_SELECTION")]
    # A crash-gate build is diagnostic even when no kill selector is armed.
    # Require one unambiguous value per key, not merely an OFF substring beside
    # another ON entry. Missing flags cannot establish a benchmark-clean build.
    if any([entry for entry in content.splitlines()
            if entry.startswith(line.split(":", 1)[0] + ":")] != [line]
           for line in required):
        raise BenchmarkError("benchmark-clean Release CMake flags are not proven")
    homes = [line.split("=", 1)[1] for line in content.splitlines()
             if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")]
    if len(homes) != 1 or Path(homes[0]).resolve() != (args.source_root / "project").resolve():
        raise BenchmarkError("CMake source directory does not match the requested checkout")
    # Require an explicit successful build gate supplied by the caller; stale
    # binaries cannot be inferred current from their filesystem timestamps.
    gate = json.loads(args.build_receipt.read_text(encoding="utf-8"))
    if not isinstance(gate, dict):
        raise BenchmarkError("build receipt must be an object")
    revision = git("rev-parse", "HEAD").decode().strip()
    hashes = {"sb_isql": sha256(args.sb_isql), "database_seed": sha256(args.database_seed)}
    if (type(gate.get("build_exit_code")) is not int or gate["build_exit_code"] != 0
            or type(gate.get("correctness_exit_code")) is not int or gate["correctness_exit_code"] != 0
            or gate.get("historical") is not args.historical
            or gate.get("source_commit") != revision
            or gate.get("binary_sha256") != hashes
            or gate.get("cmake_cache_sha256") != sha256(cache)
            or gate.get("tracked_patch_sha256") != hashlib.sha256(git("diff", "--binary", "HEAD")).hexdigest()):
        raise BenchmarkError("receipt does not bind successful build/correctness gates and historical status to these sources/binaries/configuration")
    return {"source_commit": revision,
            "tracked_patch_sha256": hashlib.sha256(git("diff", "--binary", "HEAD")).hexdigest(),
            "git_status": git("status", "--short").decode(),
            "binary_sha256": hashes, "cmake_cache_sha256": sha256(cache),
            "runner_sha256": sha256(Path(__file__)), "platform": platform.platform(),
            "support_tool_sha256": {name: sha256(Path(__file__).resolve().parents[2] / "tools" / name)
                                    for name in ("transaction_phase_summary.py", "transaction_sync_summary.py")},
            "cpu_count": os.cpu_count(), "load_average": list(os.getloadavg()),
            "build_receipt": str(args.build_receipt), "historical": args.historical,
            "shared_host": True, "cold_storage_claim": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sb-isql", "database-seed", "resource-seed-pack-root",
                 "source-root", "build-dir", "build-receipt", "work-root"):
        parser.add_argument("--" + name, type=Path, required=True,
                            help="cdp_database_seed executable (manifest interface), not sbsql_example_database_seed"
                            if name == "database-seed" else None)
    workload_options = parser.add_mutually_exclusive_group()
    workload_options.add_argument("--rows", type=int,
                                  help="insert count (default 100); exclusive with --empty-commits")
    workload_options.add_argument("--empty-commits", type=int,
                                  help="no-write explicit commit control; not a READ ONLY policy request")
    parser.add_argument("--batch", type=int, default=1)
    parser.add_argument("--mode", choices=("explicit", "autocommit"), default="explicit")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--history-commits", type=int, default=0,
                        help="prior no-write commit requests outside measurement; not an exact retained-inventory count")
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--traced", action="store_true")
    parser.add_argument("--sync-tracer", type=Path,
                        help="optional Linux strace executable; diagnostic only, not clean baseline")
    parser.add_argument("--historical", action="store_true")
    args = parser.parse_args()
    try:
        empty_control = args.empty_commits is not None
        if empty_control:
            sql, operations = empty_commit_workload(args.empty_commits, args.batch, args.mode)
            args.rows = 0
        else:
            args.rows = args.rows if args.rows is not None else 100
            sql, operations = workload(args.rows, args.batch, args.mode)
    except BenchmarkError as exc:
        parser.error(str(exc))
    if args.repeat < 1 or args.history_commits < 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("positive repeat count/finite timeout and nonnegative history count required")
    # Fail provenance before creating databases or running any workload.
    try:
        source = provenance(args)
        clean_environment()
        if args.sync_tracer is not None:
            args.sync_tracer = args.sync_tracer.resolve(strict=True)
            version = subprocess.check_output([str(args.sync_tracer), "--version"],
                                              text=True, timeout=10)
            source["sync_tracer"] = {"path": str(args.sync_tracer),
                                     "sha256": sha256(args.sync_tracer), "version": version}
    except (BenchmarkError, OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"transaction benchmark admission refused: {exc}\n")
    args.work_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="txp_", dir=args.work_root)).resolve()
    records = []
    for repeat in range(args.repeat):
        directory = root / str(repeat)
        directory.mkdir()
        database = directory / "db.sbdb"
        record = {"repeat": repeat, "status": "failed", "directory": str(directory)}
        try:
            seed_database(database_seed=str(args.database_seed),
                          resource_seed_pack_root=str(args.resource_seed_pack_root),
                          database=database, evidence_root=directory / "bootstrap",
                          fixture_label="transaction-performance")
            run_sql(args, database, directory, "setup",
                    "CREATE TABLE txp_events (id INTEGER, amount INTEGER);\nCOMMIT;\n",
                    "explicit")
            record["history_preparation"] = prepare_history(args, database, directory)
            measured = run_sql(args, database, directory, "measured", sql, args.mode,
                               traced=args.traced, sync_tracer=args.sync_tracer)
            samples = execution_samples(measured["stdout"], operations)
            verified = run_sql(args, database, directory, "verify",
                               "SELECT COUNT(*) FROM txp_events;\n" if empty_control else
                               "SELECT id, amount FROM txp_events ORDER BY id;\n",
                               "explicit")
            result_hash = (verify_empty_table(verified["stdout"]) if empty_control else
                           verify_rows(verified["stdout"], args.rows))
            # Preserve the independently established row result even if the
            # requested diagnostic evidence subsequently fails validation.
            record.update(result_sha256=result_hash, rows_reopen_verified=True,
                          verification_kind="empty_table_count" if empty_control else "exact_row_values")
            reports = []
            sync_report = collect_sync_trace(directory) if args.sync_tracer else None
            if args.traced:
                expected_commits = args.rows if args.mode == "autocommit" else operations.count("commit")
                reports = collect_traces(directory, expected_commits)
            record.update(status="verified", result_sha256=result_hash, command_samples=samples,
                          phase_traces=reports,
                          sync_trace=sync_report,
                          measured_process_wall_seconds=measured["process_wall_seconds"],
                          process_inclusive_rows_per_second=None if empty_control else
                          args.rows / measured["process_wall_seconds"],
                          workload_sha256=measured["script_sha256"])
        except (BenchmarkError, OSError, ValueError, subprocess.SubprocessError, RuntimeError) as exc:
            record["error"] = str(exc)
        records.append(record)
        # Checkpoint each attempt, including failures, without deleting evidence.
        payload = {"schema": "scratchbird.transaction.exploratory-benchmark.v1",
                   "provenance": source, "mode": args.mode, "rows": args.rows,
                   "workload_kind": "empty_commit_control" if empty_control else "insert",
                   "requested_empty_commits": args.empty_commits,
                   "prior_requested_commits": args.history_commits,
                   "rows_per_explicit_commit": args.batch if args.mode == "explicit" and not empty_control else None,
                   "traced": args.traced,
                   "sync_traced": args.sync_tracer is not None,
                   "diagnostic_run": args.traced or args.sync_tracer is not None,
                   "records": records, "complete": len(records) == args.repeat,
                   "route": "real_scratchbird_embedded_serial",
                   "measurement_limits": ["not Apache or network capacity", "not full ACID qualification",
                                          "CLI times exclude result display", "process times include attach and teardown",
                                          "embedded COMMIT has no CLI timing unless the executable emits one",
                                          "process timing includes post-marker verification rollback and its replacement",
                                          "trace output includes measured-process attach and teardown",
                                          "no-write control does not assert READ ONLY policy or per-transaction finality",
                                          "prior commit requests are not exact retained-inventory or snapshot counts",
                                          "no request-correlated internal spans", "no index-profile coverage in this lane"]}
        if all(row["status"] == "verified" for row in records):
            payload["process_wall_seconds"] = statistics([row["measured_process_wall_seconds"] for row in records])
        (root / "results.json").write_text(json.dumps(payload, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
        print(f"repeat={repeat} status={record['status']} evidence={directory}", flush=True)
    print(f"result={root / 'results.json'}", flush=True)
    return 0 if all(row["status"] == "verified" for row in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
