#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Summarize existing transaction/store phase traces, not benchmark throughput.

Inputs are SCRATCHBIRD_TRANSACTION_API_PHASE_TRACE_FILE and
SCRATCHBIRD_LOCAL_TXN_STORE_PHASE_TRACE_FILE TSV outputs. Nested layers are
reported separately: their elapsed times must never be added together.
Malformed, truncated and empty input is refused instead of producing a pass.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Iterable


class TraceError(ValueError):
    pass


def statistics(values: list[int]) -> dict:
    ordered = sorted(values)
    if not ordered:
        raise TraceError("cannot summarize empty samples")

    def percentile(fraction: float) -> int:
        return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]

    return {
        "count": len(ordered), "min_us": ordered[0], "max_us": ordered[-1],
        "sum_us": sum(ordered), "mean_us": sum(ordered) / len(ordered),
        "p50_us": percentile(0.50), "p95_us": percentile(0.95),
        "p99_us": percentile(0.99), "percentile_method": "nearest_rank",
    }


def summarize(lines: Iterable[str], *, max_records: int = 1_000_000) -> dict:
    if max_records < 1:
        raise TraceError("max_records must be positive")
    groups: dict = defaultdict(lambda: defaultdict(list))
    count = 0
    for number, line in enumerate(lines, 1):
        if not line.endswith("\n"):
            raise TraceError(f"line {number}: unterminated record")
        if not line.strip():
            continue
        count += 1
        if count > max_records:
            raise TraceError("record limit exceeded; split the trace explicitly")
        fields = {}
        for token in line.rstrip("\r\n").split("\t"):
            key, separator, value = token.partition("=")
            if not separator or not key or not value or key in fields:
                raise TraceError(f"line {number}: malformed or duplicate field")
            fields[key] = value
        operation = fields.get("operation_id")
        outcome = fields.get("ok")
        if not operation or outcome not in {"true", "false"}:
            raise TraceError(f"line {number}: missing operation or boolean outcome")
        if "total_us" not in fields:
            raise TraceError(f"line {number}: missing total_us")
        timings = {}
        for key, value in fields.items():
            if key.endswith("_us"):
                if not value.isascii() or not value.isdigit() or len(value) > 20:
                    raise TraceError(f"line {number}: invalid unsigned timing {key}")
                timing = int(value)
                if timing > (1 << 64) - 1:
                    raise TraceError(f"line {number}: timing overflow {key}")
                timings[key] = timing
        for key, value in timings.items():
            groups[(operation, outcome)][key].append(value)
    if not count:
        raise TraceError("no complete trace records")
    return {
        "schema": "scratchbird.transaction.phase-summary.v1",
        "record_count": count,
        "interpretation": {
            "measured_execution_success": "ok=true only; failures remain separate",
            "nested_layers_additive": False,
            "nested_records_within_one_family_possible": True,
            "record_count_is_sync_or_commit_count": False,
            "transaction_or_request_correlation_available": False,
            "throughput_or_acid_proof": False,
            "missing_phase_policy": "absent, never synthesized as zero",
            "trace_write_overhead_included_in_total": False,
        },
        "operations": [
            {"operation_id": operation, "ok": outcome == "true",
             "record_count": len(phases["total_us"]),
             "phases": {key: statistics(values) for key, values in sorted(phases.items())}}
            for (operation, outcome), phases in sorted(groups.items())
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-records", type=int, default=1_000_000)
    args = parser.parse_args()
    try:
        reports = []
        for path in args.trace:
            with path.open(encoding="utf-8", newline="") as source:
                reports.append({"source": str(path.resolve()),
                                **summarize(source, max_records=args.max_records)})
        # Do not overwrite a baseline or emit an apparently successful prefix.
        payload = json.dumps({"traces": reports}, indent=2, sort_keys=True) + "\n"
        with args.output.open("x", encoding="utf-8") as output:
            output.write(payload)
    except (OSError, UnicodeError, TraceError) as exc:
        parser.exit(1, f"transaction phase summary refused: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
