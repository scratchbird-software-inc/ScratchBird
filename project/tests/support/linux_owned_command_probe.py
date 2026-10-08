#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Profile one owned command and reject surviving descendants on Linux.

Run this as a separate process, never inside a multi-command worker. A kernel
subreaper adopts only this command's orphaned descendants; no global process
search or name-based termination is used. This does NOT provide filesystem or
PID-namespace isolation and does not replace the full regression namespace.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time


def children(pid):
    result = set()
    try:
        tasks = list(Path(f"/proc/{pid}/task").iterdir())
    except FileNotFoundError:
        return result
    for task in tasks:
        try:
            result.update(map(int, (task / "children").read_text().split()))
        except (FileNotFoundError, ProcessLookupError):
            pass
    return result


def snapshot(pid):
    try:
        root = Path(f"/proc/{pid}")
        raw = (root / "stat").read_text()
        fields = raw[raw.rfind(") ") + 2:].split()
        result = {"pid": pid, "start_ticks": int(fields[19]), "state": fields[0],
                  "user_ticks": int(fields[11]), "system_ticks": int(fields[12]),
                  "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGE_SIZE")}
        # Procfs may revoke ptrace-gated fields during exec/exit. Sampling is
        # observational, not process-ownership authority: record unavailable
        # fields explicitly, never abort a test or pretend they were zero.
        for source, key in (("cmdline", "command"), ("wchan", "wait_channel"), ("io", "io")):
            try:
                value = (root / source).read_bytes().decode(errors="replace")
                result[key] = ({k: int(v) for k, v in
                    (line.split(":", 1) for line in value.splitlines())}
                    if source == "io" else value.replace("\0", " ").strip())
            except (PermissionError, FileNotFoundError, ProcessLookupError) as error:
                result[key] = None
                result.setdefault("unavailable", {})[key] = type(error).__name__
        return result
    except (FileNotFoundError, ProcessLookupError):
        return None


def reap():
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def drain():
    # The command controller has already been waited. All remaining direct
    # children are kernel-adopted descendants, not other users' daemons.
    reap()
    leaked = [value for pid in sorted(children(os.getpid()))
              if (value := snapshot(pid)) is not None]
    for sig in (signal.SIGTERM, signal.SIGKILL):
        deadline = time.monotonic() + 5
        while True:
            reap()
            owned = children(os.getpid())
            if not owned:
                return leaked
            for pid in owned:
                try:
                    fd = os.pidfd_open(pid)
                except ProcessLookupError:
                    continue
                try:
                    signal.pidfd_send_signal(fd, sig)
                except ProcessLookupError:
                    pass
                finally:
                    os.close(fd)
            if time.monotonic() >= deadline:
                break
            time.sleep(0.02)
    raise RuntimeError("Owned descendants did not terminate")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not sys.platform.startswith("linux") or not command or args.timeout <= 0:
        parser.error("Requires Linux, a command, and a positive timeout")
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        parser.error("Requires a Python interpreter with Linux pidfd support")
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER")
    def interrupted(number, _frame):
        raise InterruptedError(f"Probe interrupted by signal {number}")
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, interrupted)
    args.evidence.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    receipt = {"command": command, "status": "RUNNING", "sample_interval_seconds": 1,
               "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
               "sampling_limits": "Short-lived children may fall between samples; no stack or syscall trace."}
    path = args.evidence / "receipt.json"
    path.write_text(json.dumps(receipt, indent=2) + "\n")
    with (args.evidence / "command.log").open("wb") as output, \
            (args.evidence / "process-samples.jsonl").open("w") as samples:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            while process.poll() is None:
                pending = list(children(os.getpid()))
                seen = set()
                observations = []
                while pending:
                    pid = pending.pop()
                    if pid in seen:
                        continue
                    seen.add(pid)
                    pending.extend(children(pid))
                    row = snapshot(pid)
                    if row is not None:
                        observations.append(row)
                samples.write(json.dumps({"elapsed_seconds": time.monotonic() - started,
                                          "processes": observations}) + "\n")
                samples.flush()
                if time.monotonic() - started >= args.timeout:
                    receipt["timed_out"] = True
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                    break
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
            receipt["exit_code"] = process.wait()
        finally:
            error = sys.exc_info()[1]
            # Finish cleanup even if the user repeats an interrupt.
            for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                signal.signal(number, signal.SIG_IGN)
            if process.poll() is None:
                process.kill()
                process.wait()
            try:
                receipt["leaked_descendants"] = drain()
            except BaseException as cleanup_error:
                receipt["cleanup_error"] = repr(cleanup_error)
                error = cleanup_error
                raise
            finally:
                if error is not None:
                    receipt.update(status="FAILED", probe_error=repr(error),
                                   elapsed_seconds=time.monotonic() - started)
                    path.write_text(json.dumps(receipt, indent=2) + "\n")
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    receipt.update(elapsed_seconds=time.monotonic() - started,
                   user_cpu_seconds=usage.ru_utime, system_cpu_seconds=usage.ru_stime,
                   peak_child_rss_kib=usage.ru_maxrss, input_blocks=usage.ru_inblock,
                   output_blocks=usage.ru_oublock,
                   voluntary_context_switches=usage.ru_nvcsw,
                   involuntary_context_switches=usage.ru_nivcsw)
    passed = receipt["exit_code"] == 0 and not receipt.get("timed_out") and not receipt["leaked_descendants"]
    receipt["status"] = "PASS" if passed else "FAILED"
    path.write_text(json.dumps(receipt, indent=2) + "\n")
    print(path, receipt["status"], flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
