#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real Linux IPC owner exit codes; no public query/E2E completion claim."""
import argparse
import selectors
import subprocess
import tempfile
import time
from pathlib import Path

from ia01_package_process_e2e import ProofError, seed_database, stop, wait_unix


def run_case(server_binary, client_binary, root, database, password, mode, loss):
    work = root / f"{mode}-{loss}"
    work.mkdir()
    endpoint = work / "sc" / "s.sock"
    server = client = None
    with (work / "server.out").open("wb") as out, (work / "server.err").open("wb") as err:
        try:
            server = subprocess.Popen(
                [str(server_binary), "--foreground", "--no-listeners", "--control-dir",
                 str(work / "sc"), "--runtime-dir", str(work / "sr"),
                 "--database", str(database), "--sbps-endpoint", str(endpoint)],
                stdout=out, stderr=err,
            )
            wait_unix(endpoint)
            client = subprocess.Popen(
                [str(client_binary), f"unix:{endpoint}", str(database), password,
                 mode, "loss" if loss else "live"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True,
            )
            with selectors.DefaultSelector() as selector:
                selector.register(client.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=30):
                    raise ProofError("authenticated transaction handshake timed out")
            ready = client.stdout.readline()
            if ready != "AUTHENTICATED_TRANSACTION_READY\n":
                stdout, stderr = client.communicate(timeout=10)
                raise ProofError(f"transaction handshake failed: {ready}{stdout}{stderr}")
            if loss:
                stop(server)
                if server.poll() is None:
                    raise ProofError("server-loss injection did not stop the server")
            stdout, stderr = client.communicate(input="go\n", timeout=30)
            expected = f"OWNER_TERMINAL_VERIFIED mode={mode} loss={int(loss)} rc={int(loss)}\n"
            if client.returncode != 0 or stderr or stdout != expected:
                raise ProofError(f"{mode} loss={loss}: rc={client.returncode} {stdout}{stderr}")
        finally:
            stop(client)
            stop(server)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    failures = []
    # Exact mkdtemp-owned fixtures are removed only after both processes stop.
    # Logs needed to explain failures are copied into the failing test output.
    with tempfile.TemporaryDirectory(prefix="sb-do-") as name:
        root = Path(name)
        database = root / "d.sbdb"
        # These cases perform BEGIN followed by disconnect, with no data or
        # schema mutation. Retain one credentialed database and reopen it in
        # six separate server processes; each previous owner is joined before
        # the next starts. This also exercises recovery across server loss,
        # without six costly resource-catalog bootstraps or database copies.
        password = seed_database(args.server, database)
        for mode in ("text", "native", "eof"):
            for loss in (False, True):
                started = time.monotonic()
                try:
                    run_case(args.server, args.client, root, database, password, mode, loss)
                    print(f"disconnect_owner_case mode={mode} loss={int(loss)} "
                          f"elapsed_seconds={time.monotonic() - started:.3f} PASS", flush=True)
                except (ProofError, subprocess.TimeoutExpired) as exc:
                    logs = []
                    for filename in ("server.out", "server.err"):
                        path = root / f"{mode}-{loss}" / filename
                        if path.exists():
                            logs.append(f"--- {mode}-{loss}/{filename} ---\n" +
                                        path.read_text(encoding="utf-8", errors="replace"))
                    failures.append(f"{mode} loss={int(loss)}: {exc}\n" + "\n".join(logs))
    if failures:
        raise ProofError("\n".join(failures))
    print("disconnect_owner_process cases=6 PASS actual_server=true query_e2e=false artifacts_cleaned=true")


if __name__ == "__main__":
    main()
