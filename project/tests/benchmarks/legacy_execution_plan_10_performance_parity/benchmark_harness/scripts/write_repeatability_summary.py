#!/usr/bin/env python3
"""Write a repeatability summary JSON from a TSV attempt manifest."""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import statistics
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize repeated benchmark attempts into a repeatability artifact."
    )
    parser.add_argument("--manifest", required=True, help="TSV attempt manifest path")
    parser.add_argument("--output", required=True, help="JSON summary output path")
    return parser.parse_args()


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * weight


def load_attempts(path: pathlib.Path) -> list[dict[str, Any]]:
    attempts: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        for row in reader:
            attempts.append(
                {
                    "attempt_index": int(row["attempt_index"]),
                    "phase": row["phase"],
                    "output_dir": row["output_dir"],
                    "exit_code": int(row["exit_code"]),
                    "elapsed_ms": int(row["elapsed_ms"]),
                }
            )
    return attempts


def build_summary(manifest_path: pathlib.Path, attempts: list[dict[str, Any]]) -> dict[str, Any]:
    measured_attempts = [row for row in attempts if row["phase"] == "measured"]
    measured_successes = [row for row in measured_attempts if row["exit_code"] == 0]
    measured_elapsed_ms = [float(row["elapsed_ms"]) for row in measured_successes]

    mean_ms = statistics.fmean(measured_elapsed_ms) if measured_elapsed_ms else None
    median_ms = statistics.median(measured_elapsed_ms) if measured_elapsed_ms else None
    sample_stdev_ms = (
        statistics.stdev(measured_elapsed_ms) if len(measured_elapsed_ms) >= 2 else 0.0 if measured_elapsed_ms else None
    )
    coefficient_of_variation = (
        sample_stdev_ms / mean_ms if mean_ms not in (None, 0.0) and sample_stdev_ms is not None else None
    )

    return {
        "schema_version": "scratchbird_benchmarks.repeatability_summary.v1",
        "manifest_path": str(manifest_path),
        "summary_statistics": "aggregate_repeatability_summary",
        "tail_latency_statistics": "p95_p99_elapsed_bundle",
        "variance_policy": "sample_stdev_and_cv_over_measured_runs",
        "outlier_policy": "not_applied_report_all_measured_runs",
        "attempt_counts": {
            "total": len(attempts),
            "warmup": sum(1 for row in attempts if row["phase"] == "warmup"),
            "measured": len(measured_attempts),
            "successful": sum(1 for row in attempts if row["exit_code"] == 0),
            "failed": sum(1 for row in attempts if row["exit_code"] != 0),
            "successful_measured": len(measured_successes),
            "failed_measured": sum(1 for row in measured_attempts if row["exit_code"] != 0),
        },
        "measured_elapsed_ms": measured_elapsed_ms,
        "measured_elapsed_summary_ms": {
            "count": len(measured_elapsed_ms),
            "min": min(measured_elapsed_ms) if measured_elapsed_ms else None,
            "max": max(measured_elapsed_ms) if measured_elapsed_ms else None,
            "mean": mean_ms,
            "median": median_ms,
            "p95": percentile(measured_elapsed_ms, 0.95),
            "p99": percentile(measured_elapsed_ms, 0.99),
            "sample_stdev": sample_stdev_ms,
            "coefficient_of_variation": coefficient_of_variation,
        },
        "attempts": attempts,
    }


def main() -> int:
    args = parse_args()
    manifest_path = pathlib.Path(args.manifest).resolve()
    output_path = pathlib.Path(args.output).resolve()
    attempts = load_attempts(manifest_path)
    summary = build_summary(manifest_path, attempts)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
