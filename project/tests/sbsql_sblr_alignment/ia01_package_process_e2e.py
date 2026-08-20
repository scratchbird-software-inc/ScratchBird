#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Process-boundary proof for atomic SBLR package framing.

The proof deliberately enters through the public client boundary.  It does not
construct SBLR or call a server/engine component API.  The live route is:

  sb_isql -> listener -> SBsql parser worker -> SBPS server -> receipt-bound
  SBcore dispatch -> client, followed by verification from a new client process.
"""

# CSC-TEST-002325: SBLR_LITERAL real-process canonical execution proof.

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


PASSWORD = "ScratchBird-E2E-2026!"


class ProofError(RuntimeError):
    pass


def allocate_work(root: Path) -> Path:
    candidates = (root, Path(tempfile.gettempdir()) / "sb_pkg_e2e")
    for candidate in candidates:
        candidate.mkdir(parents=True, exist_ok=True)
        work = Path(tempfile.mkdtemp(prefix="p", dir=candidate))
        server_probe = work / "sc" / "s.sock"
        listener_probe = work / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(server_probe)), len(str(listener_probe))) < 100:
            return work
        shutil.rmtree(work, ignore_errors=True)
    raise ProofError("unable to allocate a short live-route workspace")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_path(path: Path, timeout: float = 60.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise ProofError(f"timed out waiting for {path}")


def wait_tcp(port: int, timeout: float = 60.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.05)
    raise ProofError(f"timed out waiting for listener port {port}")


def stop(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def seed_database(server: Path, database: Path) -> str:
    """Create the database through the admitted embedded-bootstrap fixture."""
    seeder = server.with_name("public_driver_test_database_seed")
    if not seeder.is_file() or not os.access(seeder, os.X_OK):
        raise ProofError(f"approved database seeder is unavailable: {seeder}")
    project_root = Path(__file__).resolve().parents[2]
    manifest = database.with_suffix(".manifest.json")
    seeded = subprocess.run(
        [str(seeder), "--output", str(database), "--manifest", str(manifest),
         "--resource-seed-pack-root",
         str(project_root / "resources" / "seed-packs" / "initial-resource-pack")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=120,
    )
    if seeded.returncode != 0:
        detail = seeded.stderr.decode("utf-8", errors="replace")
        raise ProofError(f"database seeder exited {seeded.returncode}: {detail}")
    for suffix in (".sb.local_password_auth", ".sb.security_principal_events"):
        if Path(str(database) + suffix).exists():
            raise ProofError(f"forbidden security sidecar was created: {suffix}")
    return PASSWORD


def run_client(args: argparse.Namespace, database: Path, port: int,
               evidence: str, work: Path, name: str) -> str:
    out = work / f"{name}.out"
    err = work / f"{name}.err"
    with out.open("wb") as stdout, err.open("wb") as stderr:
        completed = subprocess.run(
            [
                args.sb_isql, str(database), "--host=127.0.0.1",
                f"--port={port}", "--sslmode=disable", "-U", "alice",
                "-P", evidence, "-q", "-A", "-t", "-c",
                "SELECT id FROM app.customers WHERE id = 1",
            ],
            stdout=stdout, stderr=stderr, check=False, timeout=30,
        )
    if completed.returncode != 0:
        detail = err.read_text(encoding="utf-8", errors="replace")
        raise ProofError(f"{name} exited {completed.returncode}: {detail}")
    result = out.read_text(encoding="utf-8", errors="replace").strip()
    if result != "1":
        raise ProofError(
            f"CSC-TEST-002325: {name} returned {result!r}, expected '1'"
        )
    return result


def require_package_evidence(paths: tuple[Path, ...]) -> None:
    for path in paths:
        wait_path(path)
    audit = "\n".join(
        path.read_text(encoding="utf-8", errors="replace") for path in paths
    )
    required = (
        "sblr_opcode_stream_admitted",
        "engine.op.package_begin",
        "engine.op.package_end",
        "PASS_THROUGH",
        "statement_context_receipt",
        "query_execute_result.handle_validated",
        "admitted_query_row_stream_renderer",
        "executor_id=engine.op.literal",
        "opcode_code=3",
        "opcode_version=1.0",
        "operand_descriptor_id=typed_literal",
        "descriptor_uuid=",
        "descriptor_generation=",
        "canonical_value_sha256=",
        "result_descriptor_id=typed_value",
        "result_descriptor_version=1",
        "executor_evidence_sha256=",
        "literal_occurrence_ordinal=",
        "literal_node_id=",
        "literal_parent_expression_id=",
        "parent_consumption=admitted_after_evidence",
        "parent_success_barrier=passed",
    )
    missing = tuple(marker for marker in required if marker not in audit)
    if missing:
        raise ProofError(f"server audit lacks package evidence: {', '.join(missing)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = allocate_work(Path(args.work_dir))
    server: subprocess.Popen[bytes] | None = None
    listener: subprocess.Popen[bytes] | None = None
    try:
        database = work / "package.sbdb"
        server_control, server_runtime = work / "sc", work / "sr"
        listener_control, listener_runtime = work / "lc", work / "lr"
        endpoint = server_control / "s.sock"
        port = free_port()
        evidence = seed_database(Path(args.server), database)
        server_trace = work / "server_phase.jsonl"
        dispatch_trace = work / "dispatch_phase.jsonl"
        worker_trace = work / "worker_phase.jsonl"
        sbps_client_trace = work / "sbps_client_phase.jsonl"
        parser_launcher = work / "parser_worker_launcher.py"
        parser_launcher.write_text(
            "#!/usr/bin/env python3\n"
            "import os, sys\n"
            f"os.environ['SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE'] = {str(worker_trace)!r}\n"
            f"os.environ['SCRATCHBIRD_SBPS_CLIENT_PHASE_TRACE_FILE'] = {str(sbps_client_trace)!r}\n"
            f"os.execv({args.parser_worker!r}, [{args.parser_worker!r}, *sys.argv[1:]])\n",
            encoding="utf-8",
        )
        parser_launcher.chmod(0o700)
        server_env = os.environ.copy()
        server_env["SCRATCHBIRD_SERVER_EXECUTE_PHASE_TRACE_FILE"] = str(server_trace)
        server_env["SCRATCHBIRD_ENGINE_ABI_PHASE_TRACE_FILE"] = str(dispatch_trace)
        server_env["SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"] = str(dispatch_trace)
        server = subprocess.Popen(
            [args.server, "--foreground", "--no-listeners",
             "--control-dir", str(server_control), "--runtime-dir", str(server_runtime),
             "--database", str(database), "--sbps-endpoint", str(endpoint)],
            stdout=(work / "server.out").open("wb"),
            stderr=(work / "server.err").open("wb"),
            env=server_env,
        )
        wait_path(endpoint)
        listener_env = os.environ.copy()
        listener_env["SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE"] = str(worker_trace)
        listener_env["SCRATCHBIRD_SBPS_CLIENT_PHASE_TRACE_FILE"] = str(sbps_client_trace)
        listener = subprocess.Popen(
            [args.listener, "--foreground", "--protocol-family=sbsql",
             "--listener-profile=default", "--bundle-contract-id=bundle.default@1",
             f"--database-selector=dev_bootstrap_path:{database}",
             f"--server-endpoint=unix:{endpoint}",
             f"--parser-executable={parser_launcher}",
             f"--control-dir={listener_control}", f"--runtime-dir={listener_runtime}",
             "--bind-address=127.0.0.1", f"--port={port}",
             "--warm-pool-min=1", "--warm-pool-max=2"],
            stdout=(work / "listener.out").open("wb"),
            stderr=(work / "listener.err").open("wb"),
            env=listener_env,
        )
        wait_tcp(port)

        run_client(args, database, port, evidence, work, "producer_session")
        require_package_evidence((worker_trace, server_trace, dispatch_trace))
        # A distinct OS process and authenticated session verifies the result;
        # no cursor, receipt, parser-worker request, or client state is reused.
        run_client(args, database, port, evidence, work, "verifier_session")
        print(f"sbsql_sblr_alignment_ia01_package_process_e2e=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - preserve concrete test evidence.
        print(f"sbsql_sblr_alignment_ia01_package_process_e2e=failed work={work}: {exc}",
              file=sys.stderr)
        for path in sorted(work.glob("*.err")):
            print(f"--- {path.name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        return 1
    finally:
        stop(listener)
        stop(server)


if __name__ == "__main__":
    raise SystemExit(main())
