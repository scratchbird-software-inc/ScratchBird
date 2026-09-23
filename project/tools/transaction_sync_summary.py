# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Strict parser for the runner's Linux strace -f -qqq -ttt -T -yy sync lane.

Counts kernel syscall invocations, not hardware flushes or commit boundaries.
Unexpected syntax fails closed; retain the raw trace for diagnosis.
"""

import math
import re


class SyncTraceError(ValueError):
    pass


PREFIX = re.compile(r"^(\d+)\s+(\d+\.\d+)\s+(.+)$")
CALL = re.compile(r"^(fsync|fdatasync)\((\d+)<(.+)>\)\s+=\s+(0|-1 [A-Z0-9_]+ \([^\n]*\))\s+<([0-9]+\.[0-9]+)>$")
RESUMED = re.compile(r"^<\.\.\. (fsync|fdatasync) resumed>(.*)$")


def summarize_sync(lines):
    pending = {}
    groups = {}
    count = failures = 0
    for ordinal, line in enumerate(lines, 1):
        if not line.endswith("\n"):
            raise SyncTraceError(f"line {ordinal}: incomplete record")
        match = PREFIX.fullmatch(line.rstrip("\n"))
        if not match:
            raise SyncTraceError(f"line {ordinal}: unexpected trace syntax")
        pid, timestamp, body = match.groups()
        if not math.isfinite(float(timestamp)):
            raise SyncTraceError(f"line {ordinal}: invalid timestamp")
        if body.endswith(" <unfinished ...>"):
            if pid in pending or not re.match(r"^(fsync|fdatasync)\(", body):
                raise SyncTraceError(f"line {ordinal}: invalid unfinished call")
            pending[pid] = body.removesuffix(" <unfinished ...>")
            continue
        resumed = RESUMED.fullmatch(body)
        if resumed:
            start = pending.pop(pid, None)
            if start is None or not start.startswith(resumed[1] + "("):
                raise SyncTraceError(f"line {ordinal}: unmatched resumed call")
            body = start + resumed[2]
        elif pid in pending:
            raise SyncTraceError(f"line {ordinal}: pending call not resumed")
        call = CALL.fullmatch(body)
        if not call:
            raise SyncTraceError(f"line {ordinal}: unsupported sync record")
        syscall, fd, path, result, duration = call.groups()
        elapsed = float(duration)
        if not math.isfinite(elapsed):
            raise SyncTraceError(f"line {ordinal}: invalid duration")
        ok = result == "0"
        key = (syscall, path, result)
        group = groups.setdefault(key, {"syscall": syscall, "path": path,
                                       "result": result, "ok": ok, "count": 0,
                                       "duration_seconds_sum": 0., "duration_seconds_max": 0.})
        group["count"] += 1
        group["duration_seconds_sum"] += elapsed
        if not math.isfinite(group["duration_seconds_sum"]):
            raise SyncTraceError("duration sum overflow")
        group["duration_seconds_max"] = max(group["duration_seconds_max"], elapsed)
        count += 1
        failures += not ok
    if pending or not count:
        raise SyncTraceError("unfinished or empty sync trace")
    return {"schema": "scratchbird.transaction.sync-diagnostic.v1", "syscall_count": count,
            "failed_syscall_count": failures, "groups": [groups[key] for key in sorted(groups)],
            "limits": ["fsync/fdatasync only; not all durability-related operations",
                       "syscall success is not independent hardware durability proof",
                       "whole measured process including attach and teardown; no request correlation",
                       "elapsed syscall durations may overlap across threads/processes",
                       "tracer perturbs scheduling and runtime; not a clean throughput baseline"]}
