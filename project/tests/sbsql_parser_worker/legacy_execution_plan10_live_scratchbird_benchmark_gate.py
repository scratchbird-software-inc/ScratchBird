#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run the legacy Execution_Plan 10 ScratchBird benchmark path against a live server."""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
from pathlib import Path


BENCHMARK_VERIFIER = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"


class LiveBenchmarkError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise LiveBenchmarkError(message)


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None, timeout: int = 120) -> str:
    completed = subprocess.run(
        command,
        cwd=str(cwd),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise LiveBenchmarkError(
            f"command failed rc={completed.returncode}: {' '.join(command)}\n{completed.stdout}"
        )
    return completed.stdout


def run_unchecked(
    command: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
    timeout: int = 120,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=str(cwd),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
    )


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


def wait_for_tls(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0) as raw:
                ctx = ssl.create_default_context()
                ctx.minimum_version = ssl.TLSVersion.TLSv1_3
                ctx.maximum_version = ssl.TLSVersion.TLSv1_3
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                with ctx.wrap_socket(raw, server_hostname="localhost"):
                    return
        except Exception as exc:  # noqa: BLE001 - keep the last transport error.
            last_error = exc
            time.sleep(0.05)
    raise LiveBenchmarkError(f"listener TLS endpoint did not become reachable: {last_error}")


def generate_server_cert(openssl: str, work: Path) -> tuple[Path, Path]:
    cert = work / "server.crt"
    key = work / "server.key"
    subprocess.check_call(
        [
            openssl,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-sha256",
            "-days",
            "30",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout",
            str(key),
            "-out",
            str(cert),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    key.chmod(0o600)
    return cert, key


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)


def dump_logs(work: Path) -> None:
    for path in sorted(work.glob("*.out")) + sorted(work.glob("*.err")) + sorted(work.glob("*.log")):
        print(f"\n--- {path} ---", file=sys.stderr)
        print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def benchmark_env(repo_root: Path, port: int) -> dict[str, str]:
    env = os.environ.copy()
    driver_path = repo_root / "project" / "drivers" / "driver" / "python" / "src"
    env["PYTHONPATH"] = f"{driver_path}:{env.get('PYTHONPATH', '')}"
    env["SCRATCHBIRD_DRIVER_PYTHONPATH"] = str(driver_path)
    env["BENCHMARK_SCRATCHBIRD_HOST"] = "127.0.0.1"
    env["BENCHMARK_SCRATCHBIRD_PORT"] = str(port)
    env["BENCHMARK_SCRATCHBIRD_DB"] = "default"
    env["BENCHMARK_SCRATCHBIRD_USER"] = "benchmark_user"
    env["BENCHMARK_SCRATCHBIRD_PASSWORD"] = (
        f"scheme=local_password_v1;principal=benchmark_user;verifier={BENCHMARK_VERIFIER}"
    )
    env["BENCHMARK_SCRATCHBIRD_SSLMODE"] = "prefer"
    env["SCRATCHBIRD_ENABLE_COPY_STREAMING"] = "1"
    env["BENCHMARK_SCRATCHBIRD_ISQL_TIMEOUT"] = "1200"
    return env


def validate_benchmark_result(path: Path) -> None:
    payload = json.loads(path.read_text(encoding="utf-8"))
    require(payload.get("engines") == ["scratchbird"], "benchmark did not run only ScratchBird")
    results = payload.get("results", [])
    require(len(results) == 3, f"expected 3 ScratchBird micro-benchmark results, found {len(results)}")
    expected = {"single_insert", "point_select", "simple_aggregate"}
    found = {entry.get("test_name") for entry in results}
    require(found == expected, f"unexpected benchmark result set: {found}")
    failures = [entry for entry in results if entry.get("error")]
    require(not failures, f"ScratchBird live benchmark failures: {failures}")
    for entry in results:
        require(entry.get("engine") == "scratchbird", f"wrong engine in result: {entry}")
        require(float(entry.get("duration_ms", -1)) >= 0, f"invalid duration in result: {entry}")
        require(int(entry.get("iterations", 0)) > 0, f"invalid iteration count in result: {entry}")


def validate_sb_isql_monitor_artifacts(input_dir: Path, output_dir: Path, monitor_path: Path) -> None:
    require(input_dir.exists(), f"missing sb_isql benchmark input directory: {input_dir}")
    require(output_dir.exists(), f"missing sb_isql benchmark output directory: {output_dir}")
    require(monitor_path.exists(), f"missing sb_isql benchmark monitor stream: {monitor_path}")
    scripts = sorted(input_dir.glob("*.sql"))
    require(scripts, "benchmark did not preserve any generated sb_isql scripts")
    require(any(path.name.endswith(".copy") for path in input_dir.iterdir()),
            "benchmark did not preserve generated COPY input files")
    result_files = sorted(output_dir.glob("*.result.json"))
    require(result_files, "benchmark did not preserve per-script result JSON files")
    events = [
        json.loads(line)
        for line in monitor_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    require(events, "benchmark monitor stream is empty")
    event_names = {event.get("event") for event in events}
    require("script_started" in event_names, "benchmark monitor stream has no script_started event")
    require("script_completed" in event_names, "benchmark monitor stream has no script_completed event")
    completed = [event for event in events if event.get("event") == "script_completed"]
    require(all(event.get("returncode") == 0 for event in completed),
            f"benchmark monitor stream has non-zero completed events: {completed}")


def validate_stress_sb_isql_artifacts(input_dir: Path, output_dir: Path, monitor_path: Path) -> None:
    require(input_dir.exists(), f"missing Execution_Plan 10 sb_isql input directory: {input_dir}")
    require(output_dir.exists(), f"missing Execution_Plan 10 sb_isql output directory: {output_dir}")
    require(monitor_path.exists(), f"missing Execution_Plan 10 sb_isql monitor stream: {monitor_path}")
    require(list(input_dir.glob("*.sql")), "Execution_Plan 10 stress lane did not preserve generated sb_isql scripts")
    require(list(output_dir.glob("*.result.json")),
            "Execution_Plan 10 stress lane did not preserve per-script result JSON files")
    events = [
        json.loads(line)
        for line in monitor_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    require(events, "Execution_Plan 10 stress lane monitor stream is empty")
    event_names = {event.get("event") for event in events}
    require("script_started" in event_names, "Execution_Plan 10 stress monitor has no script_started event")
    require({"script_completed", "script_failed"} & event_names,
            "Execution_Plan 10 stress monitor has no terminal script event")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--example-db-seeder", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    repo_root = Path(args.repo_root).resolve()
    harness_root = repo_root / "docs" / "reference" / "legacy_execution_plan_10_performance_parity" / "benchmark_harness"
    runner = harness_root / "scripts" / "benchmark_runner.py"
    stress_runner = harness_root / "stress-tests" / "runners" / "stress_test_runner.py"
    comparison_script = repo_root / "docs" / "reference" / "legacy_execution_plan_10_performance_parity" / "compare_execution_plan10_baseline.py"
    comparability_gate = repo_root / "project" / "tests" / "sbsql_parser_worker" / "legacy_execution_plan10_comparability_gate.py"
    require(runner.exists(), f"benchmark runner missing: {runner}")
    require(stress_runner.exists(), f"stress benchmark runner missing: {stress_runner}")
    require(comparability_gate.exists(), f"comparability gate missing: {comparability_gate}")
    require(Path(args.sb_isql).exists(), f"sb_isql binary missing: {args.sb_isql}")

    work_root = Path(args.work_dir).resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    work = work_root / f"legacy_execution_plan10_live_{int(time.time())}"
    work.mkdir()
    short_runtime = Path(tempfile.mkdtemp(prefix="sblb_"))
    (work / "short_runtime_root.txt").write_text(str(short_runtime) + "\n", encoding="utf-8")

    database = work / "fresh-benchmark.sbdb"
    server_control = short_runtime / "sc"
    server_runtime = short_runtime / "sr"
    server_endpoint = server_control / "s"
    listener_control = short_runtime / "lc"
    listener_runtime = short_runtime / "lr"
    result_path = work / "scratchbird-live-benchmark.json"
    sb_isql_input_dir = work / "sb_isql-input"
    sb_isql_output_dir = work / "sb_isql-output"
    sb_isql_monitor = work / "sb_isql-monitor.jsonl"
    stress_result_dir = work / "execution_plan10-stress-results"
    stress_input_dir = work / "execution_plan10-sb_isql-input"
    stress_output_dir = work / "execution_plan10-sb_isql-output"
    stress_monitor = work / "execution_plan10-sb_isql-monitor.jsonl"
    comparability_path = work / "execution_plan10-comparability.json"
    comparison_dir = work / "execution_plan10-comparison"
    cert, key = generate_server_cert(args.openssl, work)
    port = find_free_port()

    run([args.example_db_seeder, str(database), "benchmark_user", BENCHMARK_VERIFIER], cwd=repo_root)

    server = None
    listener = None
    try:
        server = subprocess.Popen(
            [
                args.server,
                "--foreground",
                "--no-listeners",
                "--create-if-missing",
                "--control-dir",
                str(server_control),
                "--runtime-dir",
                str(server_runtime),
                "--database",
                str(database),
                "--sbps-endpoint",
                str(server_endpoint),
                "--log",
                str(work / "server.log"),
            ],
            cwd=str(repo_root),
            stdout=(work / "server.out").open("wb"),
            stderr=(work / "server.err").open("wb"),
        )
        if not wait_for_path(server_endpoint):
            raise LiveBenchmarkError("sb_server did not create the SBPS endpoint")

        listener = subprocess.Popen(
            [
                args.listener,
                "--foreground",
                "--protocol-family=sbsql",
                "--listener-profile=default",
                "--bundle-contract-id=bundle.default@1",
                f"--database-selector=dev_bootstrap_path:{database}",
                f"--server-endpoint=unix:{server_endpoint}",
                f"--parser-executable={args.parser_worker}",
                f"--control-dir={listener_control}",
                f"--runtime-dir={listener_runtime}",
                "--bind-address=127.0.0.1",
                f"--port={port}",
                "--tls-required=true",
                f"--tls-cert-file={cert}",
                f"--tls-key-file={key}",
                "--warm-pool-min=1",
                "--warm-pool-max=2",
                "--dbbt-key-source=test_builtin",
                "--allow-test-dbbt-builtin=true",
            ],
            cwd=str(repo_root),
            stdout=(work / "listener.out").open("wb"),
            stderr=(work / "listener.err").open("wb"),
        )
        wait_for_tls(port)

        env = benchmark_env(repo_root, port)
        env["SCRATCHBIRD_SB_ISQL"] = str(Path(args.sb_isql).resolve())
        output = run(
            [
                sys.executable,
                "-B",
                str(runner),
                "--engine",
                "scratchbird",
                "--suite",
                "micro",
                "--iteration-limit",
                "5",
                "--fail-on-error",
                "--output",
                str(result_path),
                "--scratchbird-script-input-dir",
                str(sb_isql_input_dir),
                "--scratchbird-script-output-dir",
                str(sb_isql_output_dir),
                "--scratchbird-monitor-jsonl",
                str(sb_isql_monitor),
            ],
            cwd=harness_root,
            env=env,
            timeout=180,
        )
        (work / "benchmark_runner.out").write_text(output, encoding="utf-8")
        validate_benchmark_result(result_path)
        validate_sb_isql_monitor_artifacts(sb_isql_input_dir, sb_isql_output_dir, sb_isql_monitor)

        stress_completed = run_unchecked(
            [
                sys.executable,
                "-B",
                str(stress_runner),
                "--engine",
                "scratchbird",
                "--host",
                "127.0.0.1",
                "--port",
                str(port),
                "--database",
                "default",
                "--user",
                "benchmark_user",
                "--password",
                env["BENCHMARK_SCRATCHBIRD_PASSWORD"],
                "--scale",
                "small",
                "--test-set",
                "current-native",
                "--transaction-mode",
                "normal_transactional",
                "--fail-on-error",
                "--output-dir",
                str(stress_result_dir),
                "--scratchbird-script-input-dir",
                str(stress_input_dir),
                "--scratchbird-script-output-dir",
                str(stress_output_dir),
                "--scratchbird-monitor-jsonl",
                str(stress_monitor),
            ],
            cwd=harness_root,
            env=env,
            timeout=10800,
        )
        (work / "execution_plan10_stress_runner.out").write_text(stress_completed.stdout, encoding="utf-8")
        validate_stress_sb_isql_artifacts(stress_input_dir, stress_output_dir, stress_monitor)

        comparability_output = run(
            [
                sys.executable,
                "-B",
                str(comparability_gate),
                "--result-root",
                str(stress_result_dir),
                "--output",
                str(comparability_path),
                "--expect-comparable",
            ],
            cwd=repo_root,
            env=env,
            timeout=120,
        )
        (work / "execution_plan10_comparability_gate.out").write_text(comparability_output, encoding="utf-8")
        comparability = json.loads(comparability_path.read_text(encoding="utf-8"))
        require(comparability.get("candidate_count", 0) > 0,
                "Execution_Plan 10 stress runner did not preserve a stress-result JSON artifact")

        if comparability.get("comparable"):
            comparison_output = run(
                [
                    sys.executable,
                    "-B",
                    str(comparison_script),
                    "--current-result-root",
                    str(stress_result_dir),
                    "--current-source-set",
                    "current_scratchbird",
                    "--output-dir",
                    str(comparison_dir),
                ],
                cwd=repo_root,
                env=env,
                timeout=120,
            )
            (work / "execution_plan10_comparison.out").write_text(comparison_output, encoding="utf-8")

        print(
            "legacy_execution_plan10_live_scratchbird_benchmark_gate=passed "
            f"comparable={comparability.get('comparable')} artifacts={work}"
        )
        return 0
    except Exception as exc:  # noqa: BLE001 - emit full live-route evidence.
        print(f"legacy_execution_plan10_live_scratchbird_benchmark_gate=failed: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1
    finally:
        stop_process(listener)
        stop_process(server)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
