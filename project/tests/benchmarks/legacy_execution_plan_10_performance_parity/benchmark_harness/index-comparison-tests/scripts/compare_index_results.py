#!/usr/bin/env python3
"""Compare normalized index-comparison results across targets."""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from benchmark_provenance import validate_scratchbird_result_provenance


NOISE_BAND_PCT = 5.0


def load_json(path: Path) -> Dict[str, Any]:
    validate_scratchbird_result_provenance(path)
    return json.loads(path.read_text(encoding="utf-8"))


def safe_pct_delta(candidate: float, baseline: float) -> float:
    if baseline == 0:
        return 0.0
    return ((candidate - baseline) / baseline) * 100.0


def directional_verdict(candidate: Dict[str, Any], baseline: Dict[str, Any]) -> str:
    if candidate["execution_status"] == "unsupported":
        return "unsupported"
    if baseline["execution_status"] == "unsupported":
        return "better" if candidate["execution_status"] == "pass" else "invalid"
    if candidate["execution_status"] != "pass" or baseline["execution_status"] != "pass":
        return "invalid"
    if candidate["expectation_status"] == "fallback" and baseline["expectation_status"] != "fallback":
        return "fallback"
    if candidate["plan_quality_score"] < baseline["plan_quality_score"]:
        return "worse"

    latency_delta = safe_pct_delta(candidate["latency_avg_ms"], baseline["latency_avg_ms"])
    throughput_delta = safe_pct_delta(candidate["throughput_qps"], baseline["throughput_qps"])

    if candidate["plan_quality_score"] > baseline["plan_quality_score"]:
        if latency_delta <= -NOISE_BAND_PCT or throughput_delta >= NOISE_BAND_PCT:
            return "better"
        return "equivalent"

    if abs(latency_delta) <= NOISE_BAND_PCT and abs(throughput_delta) <= NOISE_BAND_PCT:
        return "equivalent"
    if latency_delta <= -NOISE_BAND_PCT or throughput_delta >= NOISE_BAND_PCT:
        return "better"
    return "worse"


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare index-comparison result JSON files")
    parser.add_argument("--results", nargs="+", type=Path, required=True, help="Per-target index comparison JSON files")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    loaded = [load_json(path) for path in args.results]
    scenario_map: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for payload in loaded:
        target = payload.get("metadata", {}).get("target", payload.get("metadata", {}).get("engine", "unknown"))
        for result in payload.get("test_results", []):
            scenario_map[result["scenario_id"]].append({**result, "target": target})

    pairwise_results: List[Dict[str, Any]] = []
    for scenario_id, entries in sorted(scenario_map.items()):
        for candidate in entries:
            for baseline in entries:
                if candidate["target"] == baseline["target"]:
                    continue
                pairwise_results.append(
                    {
                        "scenario_id": scenario_id,
                        "candidate_target": candidate["target"],
                        "baseline_target": baseline["target"],
                        "index_family": candidate["index_family"],
                        "workload_family": candidate["workload_family"],
                        "candidate_execution_status": candidate["execution_status"],
                        "baseline_execution_status": baseline["execution_status"],
                        "candidate_plan_quality_score": candidate["plan_quality_score"],
                        "baseline_plan_quality_score": baseline["plan_quality_score"],
                        "candidate_latency_avg_ms": candidate["latency_avg_ms"],
                        "baseline_latency_avg_ms": baseline["latency_avg_ms"],
                        "candidate_throughput_qps": candidate["throughput_qps"],
                        "baseline_throughput_qps": baseline["throughput_qps"],
                        "comparative_verdict": directional_verdict(candidate, baseline)
                    }
                )

    verdict_counts = Counter(item["comparative_verdict"] for item in pairwise_results)
    by_target = defaultdict(Counter)
    for item in pairwise_results:
        by_target[item["candidate_target"]][item["comparative_verdict"]] += 1

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.output_dir / f"index-comparison-pairwise-{timestamp}.json"
    text_path = args.output_dir / f"index-comparison-pairwise-{timestamp}.txt"

    payload = {
        "metadata": {
            "engine": "comparison",
            "suite": "index-comparison-pairwise",
            "timestamp": timestamp,
            "targets": sorted({item["candidate_target"] for item in pairwise_results}),
            "noise_band_pct": NOISE_BAND_PCT
        },
        "results": {
            "pairwise": {
                "tests": pairwise_results
            }
        },
        "test_results": pairwise_results,
        "summary": {
            "total_tests": len(pairwise_results),
            "passed": sum(1 for item in pairwise_results if item["comparative_verdict"] in ("better", "equivalent")),
            "failed": sum(1 for item in pairwise_results if item["comparative_verdict"] in ("worse", "fallback", "invalid")),
            "errors": 0,
            "score": round(
                (
                    sum(100 for item in pairwise_results if item["comparative_verdict"] == "better") +
                    sum(85 for item in pairwise_results if item["comparative_verdict"] == "equivalent")
                ) / len(pairwise_results),
                2
            ) if pairwise_results else 0.0,
            "by_verdict": dict(verdict_counts),
            "by_target": {target: dict(counts) for target, counts in by_target.items()}
        }
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    lines = []
    lines.append("=" * 70)
    lines.append("INDEX COMPARISON PAIRWISE SUMMARY")
    lines.append("=" * 70)
    lines.append("")
    lines.append("VERDICT COUNTS")
    lines.append("-" * 70)
    for verdict in ("better", "equivalent", "worse", "fallback", "unsupported", "invalid"):
        lines.append(f"{verdict:<15} {verdict_counts.get(verdict, 0)}")
    lines.append("")
    lines.append("BY TARGET")
    lines.append("-" * 70)
    for target in sorted(by_target):
        lines.append(target)
        for verdict in ("better", "equivalent", "worse", "fallback", "unsupported", "invalid"):
            lines.append(f"  {verdict:<13} {by_target[target].get(verdict, 0)}")
        lines.append("")
    text_path.write_text("\n".join(lines), encoding="utf-8")

    print(json_path)
    print(text_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
