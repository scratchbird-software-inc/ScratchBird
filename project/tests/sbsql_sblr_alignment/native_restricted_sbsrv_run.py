#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_RESTRICTED_SBSRV_RUN (thin-slice S3e, owner Q296 = yes, 2026-11-19).

The REAL SBsrv serves a natively created file whose transaction inventory holds a COMMITTING slot (left by the
writer itself, native_serving_test "fixture <path> committing"): Restart refuses before writing and the database
opens RESTRICTED. The lifecycle state file must show it, not a plain service_ready=true:
  restricted_database_count=1 and database_serving_state=restricted
while the daemon-level lines keep their meaning (state=service_ready, service_ready=true: connections are
accepted). A clean file shows restricted_database_count=0 and database_serving_state=serving. The data file is
byte-identical after SIGTERM (a restricted open writes nothing) and the process exits 0. Five page sizes.
"""
import argparse
import os
import signal
import subprocess
import sys
import tempfile
import shutil
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, fail, snapshot  # noqa: E402


def read_state(state_file):
    try:
        with open(state_file, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError:
        return None
    return dict(line.split("=", 1) for line in lines if "=" in line)


def serve(args, work, size, kind):
    directory = os.path.join(work, f"{kind}{size}")
    control = os.path.join(work, f"ctl-{kind}{size}")
    runtime = os.path.join(work, f"run-{kind}{size}")
    for made in (directory, control, runtime):
        os.mkdir(made)
    path = os.path.join(directory, "restricted.sdb")
    created = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
    if created.returncode != 0:
        fail(f"create {size}: {created.stderr[-2000:]}")
    if kind == "restricted":
        fixture = subprocess.run([args.test_bin, "fixture", path, "committing"], capture_output=True, text=True, timeout=600)
        if fixture.returncode != 0:
            fail(f"fixture committing {size}: {fixture.stdout[-500:]} {fixture.stderr[-1000:]}")
    before = snapshot(directory)
    out_log = os.path.join(work, f"sbsrv-{kind}{size}.out")
    command = [args.sbsrv, "--foreground", "--database", path, "--control-dir", control, "--runtime-dir", runtime,
               "--sbps-endpoint", os.path.join(control, "sb_server.sbps.sock")]
    env = {k: v for k, v in os.environ.items() if k != "SCRATCHBIRD_CONFIG"}
    with open(out_log, "w", encoding="utf-8") as sink:
        server = subprocess.Popen(command, stdout=sink, stderr=subprocess.STDOUT, env=env)
    state_file = os.path.join(control, "sb_server.lifecycle.state")
    state = None
    deadline = time.time() + 120
    while time.time() < deadline and server.poll() is None:
        state = read_state(state_file)
        if state and state.get("service_ready") == "true":
            break
        time.sleep(0.2)
    if not state or state.get("service_ready") != "true":
        server.kill()
        server.wait()
        with open(out_log, encoding="utf-8", errors="replace") as handle:
            fail(f"SBsrv {kind} {size}: not serving: {handle.read()[-1500:]} state={state}")
    with open(os.path.join(control, "sb_server.pid"), encoding="utf-8") as handle:
        server_pid = int(handle.read().strip())
    os.kill(server_pid, signal.SIGTERM)
    try:
        code = server.wait(timeout=120)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
        fail(f"SBsrv {kind} {size}: no clean exit within 120 s of SIGTERM")
    if code != 0:
        with open(out_log, encoding="utf-8", errors="replace") as handle:
            fail(f"SBsrv {kind} {size}: exit {code}: {handle.read()[-1500:]}")
    if snapshot(directory) != before:
        fail(f"SBsrv {kind} {size}: the data directory changed")
    return state


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sbsrv", required=True)
    parser.add_argument("--test-bin", required=True)
    args = parser.parse_args()
    work = tempfile.mkdtemp(prefix="sb_restricted_sbsrv_")
    try:
        for size in SIZES:
            restricted = serve(args, work, size, "restricted")
            if restricted.get("restricted_database_count") != "1" or restricted.get("database_serving_state") != "restricted":
                fail(f"SBsrv restricted {size}: the lifecycle file does not show the restriction: {restricted}")
            if restricted.get("service_ready") != "true" or restricted.get("state") != "service_ready":
                fail(f"SBsrv restricted {size}: the daemon-level lines changed meaning: {restricted}")
            clean = serve(args, work, size, "clean")
            if clean.get("restricted_database_count") != "0" or clean.get("database_serving_state") != "serving":
                fail(f"SBsrv clean {size}: the lifecycle file does not show serving: {clean}")
            print(f"native_restricted_sbsrv_run {size}: committing slot -> restricted_database_count=1 "
                  f"database_serving_state=restricted (service_ready=true kept); clean -> 0 / serving; "
                  f"file unchanged; SIGTERM exit 0")
        print("native_restricted_sbsrv_run: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
