#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Authenticated parser-process -> binary SBPS -> server -> engine regression."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sbsql_sblr_alignment"))
from ia01_package_process_e2e import seed_database, stop, wait_unix


def main():
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--server", required=True, type=Path)
    arguments.add_argument("--client", required=True, type=Path)
    args = arguments.parse_args()
    work = Path(tempfile.mkdtemp(prefix="sbfn"))
    server = None
    passed = False
    try:
        database = work / "node.sbdb"
        password = seed_database(args.server, database)
        for phase in ("initial", "restart"):
            control = work / (phase + "-control")
            endpoint = control / "s.sock"
            with (work / (phase + ".server.out")).open("wb") as out, (work / (phase + ".server.err")).open("wb") as err:
                server = subprocess.Popen(
                    [str(args.server), "--foreground", "--no-listeners", "--database", str(database),
                     "--control-dir", str(control), "--runtime-dir", str(work / (phase + "-runtime")),
                     "--sbps-endpoint", str(endpoint)], stdout=out, stderr=err)
                wait_unix(endpoint)
                for ordinal in range(2):
                    process = subprocess.run([str(args.client), "unix:" + str(endpoint), str(database), password,
                                              phase + str(ordinal)], capture_output=True, text=True, timeout=120)
                    (work / f"{phase}.{ordinal}.client.log").write_text(process.stdout + process.stderr)
                    if process.returncode or process.stderr or process.stdout != f"builtin_function_binary_process=passed phase={phase}{ordinal}\n":
                        raise RuntimeError(f"client exited {process.returncode}: {process.stdout} {process.stderr}")
                stop(server)
                server = None
        passed = True
        print("builtin_function_binary_process_e2e=passed")
        return 0
    except Exception as error:
        print(f"builtin_function_binary_process_e2e=failed work={work}: {error}", file=sys.stderr)
        return 1
    finally:
        stop(server)
        if passed:
            shutil.rmtree(work)


if __name__ == "__main__":
    raise SystemExit(main())
