#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_DEFAULT_SBSRV_RUN (S3c, owner Q282 = 1 and Q283 = yes, 2026-11-19).

The REAL SBsrv binary, started with NO --config (the installed default
SBsrv.conf is what it discovers, as a user's first run does), on a natively
created file at each of the five page sizes. The in-process serving tests use
the ServerBootstrapConfig default and cannot see the shipped file's limits, which
is how a 128 MiB per-context limit in the shipped template went unseen until a
real run (the native open needs about 185 MB in one context at 128K pages and
about 320 MB cumulative).

Asserts, per page size:
  - SBsrv reports serving (lifecycle state service_ready=true and the SBPS
    socket exists) with the shipped defaults;
  - a clean SIGTERM shutdown (SBsrv exits 0) and the flock released;
  - the data file's SHA-256 is unchanged and the directory holds only the file;
  - 0 syscalls name a ".sb." path (strace -f);
  - 0 write-class syscalls (write, pwrite, ftruncate, fallocate, fsync,
    fdatasync, sync_file_range) on any descriptor of the data file or dup of
    one, for the WHOLE run including shutdown (Q283: an unwritten open syncs
    nothing).
And once: the three in-code per-context memory defaults, the shipped policy-pack
file and the installed and template SBsrv.conf all name the same value.
"""
import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from native_serving_strace import SIZES, WRITE_CALLS, data_descriptors, fail, merge_split_lines, snapshot  # noqa: E402

PER_CONTEXT = re.compile(r"^\s*per_context_limit_bytes\s*=\s*(\d+)\s*$", re.M)


def conf_value(path):
    with open(path, encoding="utf-8") as handle:
        match = PER_CONTEXT.search(handle.read())
    if not match:
        fail(f"{path}: no per_context_limit_bytes")
    return int(match.group(1))


def check_agreement(args):
    out = subprocess.run([args.test_bin, "memory-defaults"], capture_output=True, text=True, timeout=120)
    if out.returncode != 0 or not out.stdout.startswith("MEMORY_DEFAULTS "):
        fail(f"memory-defaults: {out.stdout!r} {out.stderr[-500:]}")
    code = {k: int(v) for k, v in (f.split("=") for f in out.stdout.split()[1:])}
    with open(os.path.join(args.pack_root, "policies", "server_memory_cache_policy.json"), encoding="utf-8") as handle:
        pack = json.load(handle)["limits"]["per_context_limit_bytes"]
    values = dict(code)
    values["policy_pack"] = pack
    values["template_conf"] = conf_value(args.template_conf)
    values["installed_conf"] = conf_value(args.installed_conf)
    if len(set(values.values())) != 1:
        fail(f"the per-context memory defaults disagree: {values}")
    print(f"native_default_sbsrv_run: one per-context default everywhere: {values}")
    return pack


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sbsrv", required=True)
    parser.add_argument("--test-bin", required=True)
    parser.add_argument("--pack-root", required=True)
    parser.add_argument("--template-conf", required=True)
    parser.add_argument("--installed-conf", required=True)
    args = parser.parse_args()
    if shutil.which("strace") is None:
        fail("strace is required")
    check_agreement(args)
    env = {k: v for k, v in os.environ.items() if k != "SCRATCHBIRD_CONFIG"}
    work = tempfile.mkdtemp(prefix="sb_default_sbsrv_")
    try:
        for size in SIZES:
            directory = os.path.join(work, f"ps{size}")
            control = os.path.join(work, f"ctl{size}")
            runtime = os.path.join(work, f"run{size}")
            for made in (directory, control, runtime):
                os.mkdir(made)
            path = os.path.join(directory, "default.sdb")
            created = subprocess.run([args.test_bin, "create", path, str(size)], capture_output=True, text=True, timeout=600)
            if created.returncode != 0:
                fail(f"create {size}: {created.stderr[-2000:]}")
            before = snapshot(directory)
            log = os.path.join(work, f"sbsrv-{size}.strace")
            out_log = os.path.join(work, f"sbsrv-{size}.out")
            # NO --config. The control and runtime directories and the SBPS endpoint are per-run (the endpoint
            # must lie under the control directory), so parallel tests cannot collide.
            command = ["strace", "-f", "-qq", "-s", "256", "-o", log, args.sbsrv, "--foreground",
                       "--database", path, "--control-dir", control, "--runtime-dir", runtime,
                       "--sbps-endpoint", os.path.join(control, "sb_server.sbps.sock")]
            with open(out_log, "w", encoding="utf-8") as sink:
                server = subprocess.Popen(command, stdout=sink, stderr=subprocess.STDOUT, env=env)
            state_file = os.path.join(control, "sb_server.lifecycle.state")
            pid_file = os.path.join(control, "sb_server.pid")
            serving = False
            deadline = time.time() + 120
            while time.time() < deadline and server.poll() is None:
                try:
                    with open(state_file, encoding="utf-8") as handle:
                        if "service_ready=true" in handle.read():
                            serving = True
                            break
                except OSError:
                    pass
                time.sleep(0.2)
            if not serving:
                server.kill()
                server.wait()
                with open(out_log, encoding="utf-8", errors="replace") as handle:
                    fail(f"SBsrv {size}: not serving with the shipped defaults: {handle.read()[-1500:]}")
            sockets = [name for name in os.listdir(control) if name.endswith(".sock")]
            if not sockets:
                server.kill()
                server.wait()
                fail(f"SBsrv {size}: serving but no SBPS socket in {os.listdir(control)}")
            with open(pid_file, encoding="utf-8") as handle:
                server_pid = int(handle.read().strip())
            os.kill(server_pid, signal.SIGTERM)
            try:
                code = server.wait(timeout=120)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                fail(f"SBsrv {size}: no clean exit within 120 s of SIGTERM")
            if code != 0:
                with open(out_log, encoding="utf-8", errors="replace") as handle:
                    fail(f"SBsrv {size}: exit {code} after SIGTERM: {handle.read()[-1500:]}")
            if snapshot(directory) != before:
                fail(f"SBsrv {size}: the data directory changed (sha256, names or sizes)")
            descriptor = os.open(path, os.O_RDWR)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                fail(f"SBsrv {size}: the data-file lock was not released at shutdown")
            finally:
                os.close(descriptor)
            with open(log, encoding="utf-8", errors="replace") as handle:
                lines = merge_split_lines(handle.readlines())
            sb_lines = [line for line in lines if ".sb." in line]
            if sb_lines:
                fail(f"SBsrv {size}: {len(sb_lines)} syscalls name a .sb. path, first: {sb_lines[0].strip()}")
            spans = data_descriptors(lines, path)
            if not spans:
                fail(f"SBsrv {size}: no open of the data file was traced")
            writes = []
            for index, line in enumerate(lines):
                match = re.search(r'\b' + WRITE_CALLS + r'\((\d+)', line)
                if match and spans.covers(index):
                    writes.append(line.strip())
            if writes:
                fail(f"SBsrv {size}: {len(writes)} write-class syscalls on the data file over the whole run, first: {writes[0]}")
            print(f"native_default_sbsrv_run {size}: no --config; serving (service_ready, SBPS socket); SIGTERM exit 0; "
                  f"sha256 unchanged; flock released; 0 .sb. syscalls; 0 write/fsync syscalls on {len(spans)} data descriptor(s); "
                  f"{len(lines)} syscalls traced")
        print("native_default_sbsrv_run: passed")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
