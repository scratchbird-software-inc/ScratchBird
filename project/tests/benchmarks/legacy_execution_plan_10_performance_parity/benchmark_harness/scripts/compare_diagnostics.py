#!/usr/bin/env python3
"""Compare ScratchBird phase diagnostics against PostgreSQL diagnostics."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple


def load_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def phase_key(phase: Dict[str, Any]) -> Tuple[str, str]:
    return (phase.get("phase_kind", ""), phase.get("phase_name", ""))


def flatten_scratchbird_contributors(phase: Dict[str, Any]) -> List[Dict[str, Any]]:
    summary = phase.get("scratchbird_trace_summary", {})
    combined: Dict[Tuple[str, str], float] = {}
    for trace_name, trace_summary in summary.items():
        for contributor in trace_summary.get("top_time_contributors", []):
            group = contributor.get("group", "")
            field = contributor.get("field", "")
            duration_ms = float(contributor.get("duration_ms", 0.0))
            key = (f"{trace_name}:{group}", field)
            combined[key] = combined.get(key, 0.0) + duration_ms
    ranked = [
        {"component": key[0], "field": key[1], "duration_ms": value}
        for key, value in combined.items()
        if value > 0
    ]
    ranked.sort(key=lambda item: item["duration_ms"], reverse=True)
    return ranked[:15]


def flatten_postgresql_nodes(phase: Dict[str, Any]) -> List[Dict[str, Any]]:
    explain = phase.get("postgresql_explain") or phase.get("postgresql_sample_insert_explain")
    if not isinstance(explain, dict):
        return []
    nodes = explain.get("top_nodes", [])
    return [
        {
            "node_type": node.get("node_type"),
            "relation": node.get("relation"),
            "index_name": node.get("index_name"),
            "actual_total_time_ms": node.get("actual_total_time_ms"),
            "actual_rows": node.get("actual_rows"),
            "actual_loops": node.get("actual_loops"),
        }
        for node in nodes[:15]
    ]


def build_phase_map(payload: Dict[str, Any]) -> Dict[Tuple[str, str], Dict[str, Any]]:
    return {phase_key(phase): phase for phase in payload.get("phases", [])}


def compare_phases(
    scratchbird_summary: Dict[str, Any],
    postgresql_summary: Dict[str, Any],
) -> List[Dict[str, Any]]:
    scratchbird_map = build_phase_map(scratchbird_summary)
    postgresql_map = build_phase_map(postgresql_summary)
    keys = sorted(set(scratchbird_map) | set(postgresql_map))

    comparisons: List[Dict[str, Any]] = []
    for key in keys:
        sb_phase = scratchbird_map.get(key, {})
        pg_phase = postgresql_map.get(key, {})
        sb_duration = sb_phase.get("duration_ms")
        pg_duration = pg_phase.get("duration_ms")
        ratio = None
        if isinstance(sb_duration, (int, float)) and isinstance(pg_duration, (int, float)) and pg_duration > 0:
            ratio = float(sb_duration) / float(pg_duration)
        comparisons.append(
            {
                "phase_kind": key[0],
                "phase_name": key[1],
                "scratchbird_duration_ms": sb_duration,
                "postgresql_duration_ms": pg_duration,
                "duration_ratio_vs_postgresql": ratio,
                "scratchbird_top_time_contributors": flatten_scratchbird_contributors(sb_phase),
                "postgresql_top_plan_nodes": flatten_postgresql_nodes(pg_phase),
                "scratchbird_status": sb_phase.get("status"),
                "postgresql_status": pg_phase.get("status"),
            }
        )
    return comparisons


def write_markdown(comparisons: Iterable[Dict[str, Any]], output_path: Path) -> None:
    lines: List[str] = [
        "# ScratchBird vs PostgreSQL Diagnostics",
        "",
        "| Phase | ScratchBird | PostgreSQL | Ratio |",
        "|---|---:|---:|---:|",
    ]

    for item in comparisons:
        phase = f"{item['phase_kind']}:{item['phase_name']}"
        sb = item.get("scratchbird_duration_ms")
        pg = item.get("postgresql_duration_ms")
        ratio = item.get("duration_ratio_vs_postgresql")
        sb_text = f"{sb:.2f} ms" if isinstance(sb, (int, float)) else "-"
        pg_text = f"{pg:.2f} ms" if isinstance(pg, (int, float)) else "-"
        ratio_text = f"{ratio:.2f}x" if isinstance(ratio, (int, float)) else "-"
        lines.append(f"| {phase} | {sb_text} | {pg_text} | {ratio_text} |")

        sb_contributors = item.get("scratchbird_top_time_contributors", [])
        if sb_contributors:
            lines.append("")
            lines.append(f"ScratchBird top contributors for `{phase}`:")
            for contributor in sb_contributors[:5]:
                lines.append(
                    f"- `{contributor['component']}` `{contributor['field']}`: {contributor['duration_ms']:.2f} ms"
                )

        pg_nodes = item.get("postgresql_top_plan_nodes", [])
        if pg_nodes:
            lines.append("")
            lines.append(f"PostgreSQL top plan nodes for `{phase}`:")
            for node in pg_nodes[:5]:
                node_name = node.get("node_type") or "UNKNOWN"
                relation = node.get("relation")
                index_name = node.get("index_name")
                detail = relation or index_name or "-"
                actual_total_time_ms = node.get("actual_total_time_ms")
                rows = node.get("actual_rows")
                lines.append(
                    f"- `{node_name}` `{detail}`: {actual_total_time_ms} ms, rows={rows}"
                )

        lines.append("")

    output_path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare ScratchBird and PostgreSQL diagnostic summaries")
    parser.add_argument("--scratchbird-diagnostics", type=Path, required=True)
    parser.add_argument("--postgresql-diagnostics", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)

    scratchbird_summary = load_json(args.scratchbird_diagnostics)
    postgresql_summary = load_json(args.postgresql_diagnostics)
    comparisons = compare_phases(scratchbird_summary, postgresql_summary)

    json_path = args.output_dir / "diagnostics-compare.json"
    json_path.write_text(json.dumps({"comparisons": comparisons}, indent=2), encoding="utf-8")

    markdown_path = args.output_dir / "diagnostics-compare.md"
    write_markdown(comparisons, markdown_path)

    print(f"Wrote {json_path}")
    print(f"Wrote {markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
