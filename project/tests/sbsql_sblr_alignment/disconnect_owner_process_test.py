#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real Linux IPC owner exit codes; no public query/E2E completion claim."""
import argparse
import selectors
import subprocess
import tempfile
from pathlib import Path

from ia01_package_process_e2e import ProofError, seed_database, stop, wait_unix


def run_case(server_binary, client_binary, root, mode, loss):
    work = root / f"{mode}-{loss}"
    work.mkdir()
    database = work / "d.sbdb"
    endpoint = work / "sc" / "s.sock"
    password = seed_database(server_binary, database)
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
    with tempfile.TemporaryDirectory(prefix="sb-do-", dir="/tmp") as name:
        for mode in ("text", "native", "eof"):
            for loss in (False, True):
                try:
                    run_case(args.server, args.client, Path(name), mode, loss)
                except (ProofError, subprocess.TimeoutExpired) as exc:
                    failures.append(str(exc))
    if failures:
        raise ProofError("\n".join(failures))
    print("disconnect_owner_process cases=6 PASS actual_server=true query_e2e=false artifacts_cleaned=true")


if __name__ == "__main__":
    main()
