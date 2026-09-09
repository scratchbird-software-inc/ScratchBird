#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""CSC-TEST-005830: authenticated CREATE transaction visibility across faults.

Oracle: Core's future-object privilege-template CREATE v1 contract says PTRS
proves statement publication, while the owning MGA transaction controls commit
and cross-session visibility. An independent session must be able to create
the same name after rollback or abandoned creation, and must get the exact
name collision after commit, including after a server process is killed.
These probes all enter with SBsql; successful absence probes are rolled back.
This does not assert durable replay of the original PTRS terminal carrier.
"""

from __future__ import annotations

import argparse
import json
import os
import selectors
import subprocess
import sys
from pathlib import Path

from ia01_package_process_e2e import (
    ProofError, allocate_work, seed_database, stop, wait_unix,
)


PREFIX = "security-create-privilege-template"
PENDING = (
    "CSC-TEST-005830 PRIVILEGE_TEMPLATE pending=true "
    "canonical_sblr=true publication_barrier=passed\n"
)
ABSENT = (
    "CSC-TEST-005830 PRIVILEGE_TEMPLATE observer_absent=true "
    "independent_session=true canonical_sblr=true probe_rolled_back=true\n"
)
ROLLBACK = (
    "CSC-TEST-005830 PRIVILEGE_TEMPLATE rollback=true "
    "canonical_sblr=true probe_rolled_back=true\n"
)
COMMITTED = (
    "CSC-TEST-002697 SECURITY_CREATE_PRIVILEGE_TEMPLATE accepted "
    "canonical_sbsql=true canonical_sblr=true durable_catalog=true "
    "commit=true publication_barrier=passed\n"
)
VISIBLE = (
    "CSC-TEST-002697 SECURITY_CREATE_PRIVILEGE_TEMPLATE "
    "observer_visible=true independent_session=true "
    "exact_name_collision=true no_catalog_mutation=true\n"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = allocate_work(Path(args.work_dir))
    database = work / "privilege.sbdb"
    server = None
    held = None
    phases: list[str] = []
    try:
        evidence = seed_database(Path(args.server), database)
        env = os.environ.copy()
        trace = work / "dispatch.jsonl"
        env["SCRATCHBIRD_ENGINE_ABI_PHASE_TRACE_FILE"] = str(trace)
        env["SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"] = str(trace)

        def launch(generation: int) -> tuple[subprocess.Popen, Path]:
            control = work / f"c{generation}"
            endpoint = control / "s.sock"
            with (work / f"server{generation}.out").open("wb") as out, (
                work / f"server{generation}.err"
            ).open("wb") as err:
                process = subprocess.Popen(
                    [args.server, "--foreground", "--no-listeners",
                     "--control-dir", str(control), "--runtime-dir",
                     str(work / f"r{generation}"), "--database", str(database),
                     "--sbps-endpoint", str(endpoint)],
                    stdout=out, stderr=err, env=env,
                )
            try:
                wait_unix(endpoint, timeout=30)
            except BaseException:
                stop(process)
                raise
            return process, endpoint

        server, endpoint = launch(0)

        def command(suffix: str, phase: str) -> list[str]:
            return [args.client, f"unix:{endpoint}", str(database), "alice",
                    evidence, PREFIX + suffix, "privilege-lifecycle-" + phase]

        def run(suffix: str, phase: str, expected: str) -> None:
            result = subprocess.run(
                command(suffix, phase), capture_output=True, text=True,
                timeout=30, env=env,
            )
            (work / f"{phase}.out").write_text(result.stdout)
            (work / f"{phase}.err").write_text(result.stderr)
            if result.returncode != 0 or result.stdout != expected or result.stderr:
                raise ProofError(
                    f"{phase}: exit={result.returncode} "
                    f"stdout={result.stdout!r} stderr={result.stderr!r}"
                )
            phases.append(phase)

        def hold(suffix: str, phase: str) -> subprocess.Popen:
            process = subprocess.Popen(
                command(suffix, phase), stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env=env,
            )
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    if not selector.select(timeout=30):
                        raise ProofError(f"{phase}: CREATE publication timed out")
                    ready = process.stdout.readline()
                if ready != PENDING or process.poll() is not None:
                    raise ProofError(f"{phase}: invalid publication marker {ready!r}")
                (work / f"{phase}.out").write_text(ready)
                phases.append(phase)
                return process
            except BaseException:
                stop(process)
                process.communicate(timeout=5)
                raise

        def kill(process: subprocess.Popen, phase: str) -> None:
            # SIGKILL prevents graceful detach or server shutdown from hiding
            # the abandoned transaction boundary exercised by this fixture.
            if process.poll() is not None:
                raise ProofError(f"{phase}: target exited before the fault")
            process.kill()
            process.wait(timeout=10)
            if process.returncode != -9:
                raise ProofError(f"{phase}: expected SIGKILL, got {process.returncode}")
            phases.append(phase)

        run("-rollback", "explicit-rollback", ROLLBACK)
        run("-observe-absent", "after-rollback", ABSENT)

        held = hold("-hold", "uncommitted-publication")
        kill(held, "client-disconnect")
        remaining_out, remaining_err = held.communicate(timeout=5)
        if remaining_out or remaining_err:
            raise ProofError("held CREATE produced unexpected output after publication")
        held = None
        run("-observe-absent", "after-client-disconnect", ABSENT)

        run("", "explicit-commit", COMMITTED)
        run("-observe", "after-commit", VISIBLE)
        held = hold("-hold-orphan", "orphan-before-server-crash")
        kill(server, "server-crash")
        server = None
        kill(held, "orphan-client-stop")
        remaining_out, remaining_err = held.communicate(timeout=5)
        if remaining_out or remaining_err:
            raise ProofError("orphan CREATE produced unexpected output after publication")
        held = None

        server, endpoint = launch(1)
        run("-observe", "committed-after-crash", VISIBLE)
        run("-observe-orphan-absent", "orphan-after-crash", ABSENT)
        run("-observe", "committed-after-orphan-probe", VISIBLE)

        # A second cold open verifies that the rolled-back absence probe did
        # not publish its own replacement definition.
        stop(server)
        server = None
        server, endpoint = launch(2)
        run("-observe-orphan-absent", "orphan-after-second-restart", ABSENT)
        run("-observe", "committed-after-second-restart", VISIBLE)
        audit = trace.read_text()
        required = (
            "preflight_observe op=engine.op.security_create_privilege_template "
            "opcode=SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE code=1621"
        )
        if required not in audit:
            raise ProofError("canonical CREATE opcode 1621 dispatch evidence is absent")
        (work / "lifecycle-evidence.json").write_text(json.dumps({
            "test_id": "CSC-TEST-005830",
            "authority": "SB-FUTURE-OBJECT-PRIVILEGE-TEMPLATE-CREATE-CARRIER-V1",
            "phases": phases,
            "terminal_replay_claimed": False,
            "result": "PASS",
        }, indent=2) + "\n")
        print(
            "CSC-TEST-005830 PASS rollback disconnect "
            "committed_restart orphan_recovery independent_sessions"
        )
        print(f"evidence={work}")
        return 0
    except (ProofError, OSError, subprocess.TimeoutExpired) as error:
        print(f"CSC-TEST-005830 FAIL: {error}; evidence={work}", file=sys.stderr)
        return 1
    finally:
        stop(held)
        if held is not None:
            held.communicate(timeout=5)
        stop(server)


if __name__ == "__main__":
    raise SystemExit(main())
