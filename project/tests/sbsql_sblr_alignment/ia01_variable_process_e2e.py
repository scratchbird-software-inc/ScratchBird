#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""CSC-TEST-002333: real process SBLR_VARIABLE exact normative refusal."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from ia01_package_process_e2e import (
    PASSWORD, ProofError, allocate_work, free_port, seed_database, stop,
    wait_path, wait_tcp,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = allocate_work(Path(args.work_dir))
    server = listener = None
    try:
        database = work / "variable.sbdb"
        endpoint = work / "sc" / "s.sock"
        port = free_port()
        evidence = seed_database(Path(args.server), database)
        server_trace = work / "server_phase.jsonl"
        dispatch_trace = work / "dispatch_phase.jsonl"
        worker_trace = work / "worker_phase.jsonl"
        launcher = work / "parser_worker_launcher.py"
        launcher.write_text(
            "#!/usr/bin/env python3\nimport os,sys\n"
            f"os.environ['SCRATCHBIRD_SBSQL_PIPELINE_PHASE_TRACE_FILE']={str(worker_trace)!r}\n"
            f"os.execv({args.parser_worker!r},[{args.parser_worker!r},*sys.argv[1:]])\n",
            encoding="utf-8",
        )
        launcher.chmod(0o700)
        env = os.environ.copy()
        env["SCRATCHBIRD_SERVER_EXECUTE_PHASE_TRACE_FILE"] = str(server_trace)
        env["SCRATCHBIRD_ENGINE_ABI_PHASE_TRACE_FILE"] = str(dispatch_trace)
        env["SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"] = str(dispatch_trace)
        server = subprocess.Popen(
            [args.server, "--foreground", "--no-listeners", "--control-dir",
             str(work / "sc"), "--runtime-dir", str(work / "sr"),
             "--database", str(database), "--sbps-endpoint", str(endpoint)],
            stdout=(work / "server.out").open("wb"),
            stderr=(work / "server.err").open("wb"), env=env,
        )
        wait_path(endpoint)
        listener = subprocess.Popen(
            [args.listener, "--foreground", "--protocol-family=sbsql",
             "--listener-profile=default", "--bundle-contract-id=bundle.default@1",
             f"--database-selector=dev_bootstrap_path:{database}",
             f"--server-endpoint=unix:{endpoint}",
             f"--parser-executable={launcher}",
             f"--control-dir={work / 'lc'}", f"--runtime-dir={work / 'lr'}",
             "--bind-address=127.0.0.1", f"--port={port}",
             "--warm-pool-min=1", "--warm-pool-max=2"],
            stdout=(work / "listener.out").open("wb"),
            stderr=(work / "listener.err").open("wb"),
        )
        wait_tcp(port)
        refused = subprocess.run(
            [args.sb_isql, str(database), "--host=127.0.0.1", f"--port={port}",
             "--sslmode=disable", "-U", "alice", "-P", evidence, "-q", "-A",
             "-t", "-c", "SELECT id FROM app.customers WHERE id = @v"],
            capture_output=True, text=True, timeout=30,
        )
        rendered = refused.stdout + refused.stderr
        if refused.returncode == 0 or "SBLR.VARIABLE.UNINITIALIZED" not in rendered:
            raise ProofError(f"variable refusal was not exact: {rendered}")
        audit_paths = (worker_trace, server_trace, dispatch_trace,
                       work / "sc" / "sb_server.audit.jsonl")
        audit = "\n".join(
            p.read_text(encoding="utf-8", errors="replace")
            for p in audit_paths if p.exists()
        )
        for marker in ("SBLR.VARIABLE.UNINITIALIZED",
                       "sblr.variable.uninitialized"):
            if marker not in audit:
                raise ProofError(f"variable refusal evidence lacks {marker}")
        for forbidden in ("executor_id=engine.op.variable",
                          "result_descriptor_id=typed_value"):
            if forbidden in audit:
                raise ProofError(
                    f"variable refusal falsely published success evidence: {forbidden}"
                )
        verifier = subprocess.run(
            [args.sb_isql, str(database), "--host=127.0.0.1", f"--port={port}",
             "--sslmode=disable", "-U", "alice", "-P", PASSWORD, "-q", "-A",
             "-t", "-c", "SELECT id FROM app.customers WHERE id = @v"],
            capture_output=True, text=True, timeout=30,
        )
        verifier_rendered = verifier.stdout + verifier.stderr
        if (verifier.returncode == 0 or
                "SBLR.VARIABLE.UNINITIALIZED" not in verifier_rendered):
            raise ProofError(
                "independent process post-state verification failed: "
                f"exit={verifier.returncode} stdout={verifier.stdout!r} "
                f"stderr={verifier.stderr!r}"
            )
        print(f"sbsql_sblr_alignment_ia01_variable_process_e2e=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"sbsql_sblr_alignment_ia01_variable_process_e2e=failed work={work}: {exc}",
              file=sys.stderr)
        for path in sorted(work.glob("*.err")):
            print(f"--- {path.name} ---\n{path.read_text(errors='replace')}", file=sys.stderr)
        return 1
    finally:
        stop(listener)
        stop(server)


if __name__ == "__main__":
    raise SystemExit(main())
