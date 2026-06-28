#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Summarize full-surface driver conformance runtime hotspots.

The full-surface scripts are shared by every driver, adapter, tool, and the
core regression suite. This analyzer consumes a run artifact directory and
produces stable JSON/CSV/Markdown hotspot reports so slow areas are visible
without manually inspecting JSONL ledgers.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class Bucket:
    count: int = 0
    total_ns: int = 0
    failed: int = 0
    max_ns: int = 0
    max_statement_id: str = ""
    values_ns: list[int] = field(default_factory=list)

    def add(self, elapsed_ns: int, statement_id: str, passed: bool) -> None:
        self.count += 1
        self.total_ns += elapsed_ns
        if not passed:
            self.failed += 1
        if elapsed_ns > self.max_ns:
            self.max_ns = elapsed_ns
            self.max_statement_id = statement_id
        self.values_ns.append(elapsed_ns)

    def as_row(self, **extra: Any) -> dict[str, Any]:
        values = sorted(self.values_ns)
        return {
            **extra,
            "statement_count": self.count,
            "failed_count": self.failed,
            "total_ms": round(self.total_ns / 1_000_000.0, 3),
            "avg_ms": round((self.total_ns / max(self.count, 1)) / 1_000_000.0, 3),
            "p50_ms": round(percentile(values, 50) / 1_000_000.0, 3),
            "p95_ms": round(percentile(values, 95) / 1_000_000.0, 3),
            "p99_ms": round(percentile(values, 99) / 1_000_000.0, 3),
            "max_ms": round(self.max_ns / 1_000_000.0, 3),
            "max_statement_id": self.max_statement_id,
        }


@dataclass
class PhaseBucket:
    count: int = 0
    total_us: int = 0
    bytes_total: int = 0
    max_us: int = 0
    max_message_type: str = ""

    def add(self, elapsed_us: int, bytes_count: int, message_type: str) -> None:
        self.count += 1
        self.total_us += elapsed_us
        self.bytes_total += bytes_count
        if elapsed_us > self.max_us:
            self.max_us = elapsed_us
            self.max_message_type = message_type

    def as_row(self, **extra: Any) -> dict[str, Any]:
        return {
            **extra,
            "sample_count": self.count,
            "total_ms": round(self.total_us / 1000.0, 3),
            "avg_ms": round((self.total_us / max(self.count, 1)) / 1000.0, 3),
            "max_ms": round(self.max_us / 1000.0, 3),
            "bytes": self.bytes_total,
            "max_message_type": self.max_message_type,
        }


def percentile(values: list[int], pct: int) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    rank = (len(values) - 1) * (pct / 100.0)
    lower = int(rank)
    upper = min(lower + 1, len(values) - 1)
    fraction = rank - lower
    return values[lower] + (values[upper] - values[lower]) * fraction


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not path.is_file():
        return rows
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                payload = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc}") from exc
            if isinstance(payload, dict):
                rows.append(payload)
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames: list[str] = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def analyze_timing_ledger(
    artifact_root: Path,
    top_n: int,
    slow_statement_ms: float,
    slow_script_ms: float,
) -> dict[str, Any]:
    ledger_path = artifact_root / "timing-ledger.jsonl"
    rows = load_jsonl(ledger_path)
    by_script: dict[str, Bucket] = defaultdict(Bucket)
    by_script_group: dict[tuple[str, str], Bucket] = defaultdict(Bucket)
    by_group: dict[str, Bucket] = defaultdict(Bucket)
    slow_statements: list[dict[str, Any]] = []

    for row in rows:
        elapsed_ns = int(row.get("elapsed_ns") or 0)
        script_id = str(row.get("script_id") or "")
        group = str(row.get("command_group") or "")
        statement_id = str(row.get("statement_id") or "")
        passed = bool(row.get("passed", True))
        by_script[script_id].add(elapsed_ns, statement_id, passed)
        by_script_group[(script_id, group)].add(elapsed_ns, statement_id, passed)
        by_group[group].add(elapsed_ns, statement_id, passed)
        elapsed_ms = elapsed_ns / 1_000_000.0
        if elapsed_ms >= slow_statement_ms:
            slow_statements.append(
                {
                    "script_id": script_id,
                    "command_group": group,
                    "statement_id": statement_id,
                    "elapsed_ms": round(elapsed_ms, 3),
                    "execution_mode": row.get("execution_mode", "statement"),
                    "passed": passed,
                    "sql_hash": row.get("sql_hash", ""),
                    "stage_timing_artifact": row.get("stage_timing_artifact", ""),
                    "server_revalidation_state": row.get("server_revalidation_state", ""),
                }
            )

    script_rows = [
        bucket.as_row(
            script_id=script_id,
            slow_script=round(bucket.total_ns / 1_000_000.0, 3) >= slow_script_ms,
        )
        for script_id, bucket in by_script.items()
    ]
    script_group_rows = [
        bucket.as_row(script_id=script_id, command_group=group)
        for (script_id, group), bucket in by_script_group.items()
    ]
    group_rows = [
        bucket.as_row(command_group=group)
        for group, bucket in by_group.items()
    ]
    script_rows.sort(key=lambda item: item["total_ms"], reverse=True)
    script_group_rows.sort(key=lambda item: item["total_ms"], reverse=True)
    group_rows.sort(key=lambda item: item["total_ms"], reverse=True)
    slow_statements.sort(key=lambda item: item["elapsed_ms"], reverse=True)

    return {
        "ledger_path": str(ledger_path),
        "statement_count": len(rows),
        "by_script": script_rows,
        "by_script_group": script_group_rows,
        "by_group": group_rows,
        "slow_statements": slow_statements[:top_n],
    }


def analyze_phase_traces(artifact_root: Path, top_n: int) -> dict[str, Any]:
    trace_root = artifact_root / "driver-phase-traces"
    by_script_phase: dict[tuple[str, str], PhaseBucket] = defaultdict(PhaseBucket)
    by_statement_phase: dict[tuple[str, str, str, str], PhaseBucket] = defaultdict(PhaseBucket)
    by_phase: dict[str, PhaseBucket] = defaultdict(PhaseBucket)
    slow_phase_samples: list[dict[str, Any]] = []
    if not trace_root.is_dir():
        return {
            "trace_root": str(trace_root),
            "phase_trace_file_count": 0,
            "by_script_phase": [],
            "by_statement_phase": [],
            "by_phase": [],
            "slow_phase_samples": [],
        }

    for trace_path in sorted(trace_root.glob("*.jsonl")):
        script_id = trace_path.stem
        for row in load_jsonl(trace_path):
            phase = str(row.get("phase") or "")
            elapsed_us = int(row.get("elapsed_us") or 0)
            byte_count = int(row.get("bytes") or 0)
            message_type = str(row.get("message_type") or "")
            statement_id = str(row.get("statement_id") or "")
            command_group = str(row.get("command_group") or "")
            by_script_phase[(script_id, phase)].add(elapsed_us, byte_count, message_type)
            if statement_id:
                by_statement_phase[(script_id, statement_id, command_group, phase)].add(
                    elapsed_us,
                    byte_count,
                    message_type,
                )
            by_phase[phase].add(elapsed_us, byte_count, message_type)
            slow_phase_samples.append(
                {
                    "script_id": script_id,
                    "statement_id": statement_id,
                    "command_group": command_group,
                    "execution_mode": row.get("execution_mode", ""),
                    "event": row.get("event", ""),
                    "phase": phase,
                    "elapsed_ms": round(elapsed_us / 1000.0, 3),
                    "bytes": byte_count,
                    "message_type": message_type,
                    "sequence": row.get("sequence", 0),
                    "tls_active": row.get("tls_active", None),
                }
            )

    script_phase_rows = [
        bucket.as_row(script_id=script_id, phase=phase)
        for (script_id, phase), bucket in by_script_phase.items()
    ]
    statement_phase_rows = [
        bucket.as_row(
            script_id=script_id,
            statement_id=statement_id,
            command_group=command_group,
            phase=phase,
        )
        for (script_id, statement_id, command_group, phase), bucket in by_statement_phase.items()
    ]
    phase_rows = [
        bucket.as_row(phase=phase)
        for phase, bucket in by_phase.items()
    ]
    script_phase_rows.sort(key=lambda item: item["total_ms"], reverse=True)
    statement_phase_rows.sort(key=lambda item: item["total_ms"], reverse=True)
    phase_rows.sort(key=lambda item: item["total_ms"], reverse=True)
    slow_phase_samples.sort(key=lambda item: item["elapsed_ms"], reverse=True)
    return {
        "trace_root": str(trace_root),
        "phase_trace_file_count": len(list(trace_root.glob("*.jsonl"))),
        "by_script_phase": script_phase_rows,
        "by_statement_phase": statement_phase_rows,
        "by_phase": phase_rows,
        "slow_phase_samples": slow_phase_samples[:top_n],
    }


def render_markdown(report: dict[str, Any], top_n: int) -> str:
    timing = report["timing"]
    phases = report["phases"]
    lines = [
        "# Full Surface Runtime Hotspots",
        "",
        f"Artifact root: `{report['artifact_root']}`",
        f"Statements analyzed: `{timing['statement_count']}`",
        "",
        "## Slowest Scripts",
        "",
        "| Script | Total ms | Count | Avg ms | P95 ms | Max ms | Max Statement |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for row in timing["by_script"][:top_n]:
        lines.append(
            "| {script_id} | {total_ms} | {statement_count} | {avg_ms} | {p95_ms} | {max_ms} | `{max_statement_id}` |".format(
                **row
            )
        )
    lines.extend(
        [
            "",
            "## Slowest Script/Command Groups",
            "",
            "| Script | Group | Total ms | Count | Avg ms | P95 ms | Max ms | Max Statement |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |",
        ]
    )
    for row in timing["by_script_group"][:top_n]:
        lines.append(
            "| {script_id} | {command_group} | {total_ms} | {statement_count} | {avg_ms} | {p95_ms} | {max_ms} | `{max_statement_id}` |".format(
                **row
            )
        )
    lines.extend(
        [
            "",
            "## Slowest Statements",
            "",
            "| Statement | Script | Group | Mode | Elapsed ms | Passed |",
            "| --- | --- | --- | --- | ---: | --- |",
        ]
    )
    for row in timing["slow_statements"][:top_n]:
        lines.append(
            "| `{statement_id}` | {script_id} | {command_group} | {execution_mode} | {elapsed_ms} | {passed} |".format(
                **row
            )
        )
    lines.extend(
        [
            "",
            "## Driver Phase Totals",
            "",
            "| Phase | Total ms | Count | Avg ms | Max ms | Bytes |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in phases["by_phase"][:top_n]:
        lines.append(
            "| {phase} | {total_ms} | {sample_count} | {avg_ms} | {max_ms} | {bytes} |".format(
                **row
            )
        )
    lines.extend(
        [
            "",
            "## Slowest Statement/Phase Pairs",
            "",
            "| Statement | Script | Group | Phase | Total ms | Count | Avg ms | Max ms | Bytes |",
            "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in phases["by_statement_phase"][:top_n]:
        lines.append(
            "| `{statement_id}` | {script_id} | {command_group} | {phase} | {total_ms} | {sample_count} | {avg_ms} | {max_ms} | {bytes} |".format(
                **row
            )
        )
    lines.extend(
        [
            "",
            "## Slowest Script/Phase Pairs",
            "",
            "| Script | Phase | Total ms | Count | Avg ms | Max ms | Bytes |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in phases["by_script_phase"][:top_n]:
        lines.append(
            "| {script_id} | {phase} | {total_ms} | {sample_count} | {avg_ms} | {max_ms} | {bytes} |".format(
                **row
            )
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", required=True, type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--top-n", type=int, default=30)
    parser.add_argument("--slow-statement-ms", type=float, default=1000.0)
    parser.add_argument("--slow-script-ms", type=float, default=30_000.0)
    args = parser.parse_args()

    artifact_root = args.artifact_root.resolve()
    if not artifact_root.is_dir():
        raise SystemExit(f"artifact root does not exist: {artifact_root}")
    output_root = (args.output_root or (artifact_root / "runtime-analysis")).resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    report = {
        "schema_id": "scratchbird.driver.full_surface_runtime_hotspot_report.v1",
        "artifact_root": str(artifact_root),
        "thresholds": {
            "slow_statement_ms": args.slow_statement_ms,
            "slow_script_ms": args.slow_script_ms,
        },
        "timing": analyze_timing_ledger(
            artifact_root,
            args.top_n,
            args.slow_statement_ms,
            args.slow_script_ms,
        ),
        "phases": analyze_phase_traces(artifact_root, args.top_n),
    }

    (output_root / "runtime-hotspots.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_csv(output_root / "runtime-by-script.csv", report["timing"]["by_script"])
    write_csv(output_root / "runtime-by-script-group.csv", report["timing"]["by_script_group"])
    write_csv(output_root / "runtime-by-command-group.csv", report["timing"]["by_group"])
    write_csv(output_root / "runtime-slowest-statements.csv", report["timing"]["slow_statements"])
    write_csv(output_root / "runtime-by-driver-phase.csv", report["phases"]["by_phase"])
    write_csv(output_root / "runtime-by-script-phase.csv", report["phases"]["by_script_phase"])
    write_csv(output_root / "runtime-by-statement-phase.csv", report["phases"]["by_statement_phase"])
    (output_root / "runtime-hotspots.md").write_text(
        render_markdown(report, args.top_n),
        encoding="utf-8",
    )
    print(f"runtime_hotspot_report={output_root / 'runtime-hotspots.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
