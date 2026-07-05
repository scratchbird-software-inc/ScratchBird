#!/usr/bin/env python3
"""Write a lane-bounded comparison guard for one benchmark artifact root."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "scratchbird_benchmarks.comparison_guard.v1"

HARNESS_ROOT = Path(__file__).resolve().parents[1]
if str(HARNESS_ROOT) not in sys.path:
    sys.path.insert(0, str(HARNESS_ROOT))

from benchmark_provenance import scan_scratchbird_runtime_storage_policy


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Write benchmark comparison guard")
    parser.add_argument("--run-provenance", type=Path, required=True)
    parser.add_argument("--repeatability-summary", type=Path, default=None)
    parser.add_argument("--output-file", type=Path, required=True)
    return parser.parse_args()


def read_json(path: Path | None) -> dict[str, Any] | None:
    if path is None or not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def as_int(value: Any, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    try:
        return int(str(value))
    except (TypeError, ValueError):
        return default


def extract_runtime_pinning(payload: dict[str, Any]) -> dict[str, Any]:
    engine = payload.get("engine")
    runtime_source = None
    runtime_payload: dict[str, Any] | None = None
    if engine == "scratchbird":
        runtime_source = "scratchbird_runtime"
        runtime_payload = payload.get("scratchbird_runtime")
    elif engine == "matrix":
        runtime_source = "matrix_aggregate"
        runtime_payload = None
    else:
        runtime_source = "reference_engine_runtime"
        runtime_payload = payload.get("reference_engine_runtime")

    pinning = runtime_payload.get("pinning") if isinstance(runtime_payload, dict) else None
    if not isinstance(pinning, dict):
        return {
            "source": runtime_source,
            "present": False,
            "comparison_eligible": False,
            "status": "missing",
            "reason": "runtime pinning metadata is absent",
        }
    return {
        "source": runtime_source,
        "present": True,
        "comparison_eligible": bool(pinning.get("comparison_eligible")),
        "status": pinning.get("status", "unknown"),
        "reason": pinning.get("reason", ""),
    }


def extract_scratchbird_runtime_storage_policy(payload: dict[str, Any]) -> dict[str, Any]:
    if payload.get("engine") != "scratchbird":
        return {
            "present": True,
            "comparison_eligible": True,
            "status": "not_applicable",
            "reason": "runtime storage policy is ScratchBird-only",
            "forbidden_count": 0,
        }

    runtime_payload = payload.get("scratchbird_runtime")
    if not isinstance(runtime_payload, dict):
        return {
            "present": False,
            "comparison_eligible": False,
            "status": "missing",
            "reason": "ScratchBird runtime metadata is absent",
            "forbidden_count": 0,
        }

    policy = runtime_payload.get("runtime_storage_policy")
    if not isinstance(policy, dict):
        observed_runtime_scan = None
        runtime_root = runtime_payload.get("root")
        if runtime_root:
            observed_runtime_scan = scan_scratchbird_runtime_storage_policy(Path(runtime_root))
        reason = "ScratchBird runtime storage policy is absent"
        if observed_runtime_scan and not observed_runtime_scan["comparison_eligible"]:
            reason = f"{reason}; observed runtime scan failed: {observed_runtime_scan['reason']}"
        return {
            "present": False,
            "comparison_eligible": False,
            "status": "missing",
            "reason": reason,
            "forbidden_count": as_int((observed_runtime_scan or {}).get("forbidden_count"), 0),
            "forbidden_artifacts": (observed_runtime_scan or {}).get("forbidden_artifacts", []),
        }

    return {
        "present": True,
        "comparison_eligible": bool(policy.get("comparison_eligible")),
        "status": policy.get("status", "unknown"),
        "reason": policy.get("reason", ""),
        "forbidden_count": as_int(policy.get("forbidden_count"), 0),
        "forbidden_artifacts": policy.get("forbidden_artifacts", []),
    }


def suite_specific_blockers(payload: dict[str, Any], runtime_options: dict[str, Any]) -> list[str]:
    suite = str(payload.get("suite") or "")
    blockers: list[str] = []
    ingest_claim_normalized = str(runtime_options.get("ingest_claim_normalized", "")).lower()
    ingest_claim_waived = str(runtime_options.get("ingest_claim_waived", "")).lower()
    if suite == "stress-tests" and ingest_claim_normalized not in {"true", "1", "yes"}:
        if ingest_claim_waived not in {"true", "1", "yes"}:
            blockers.append(
                "reference-native ingest lane normalization or explicit waiver is not yet recorded for stress-suite ingest claims"
            )
    return blockers


def build_guard(
    payload: dict[str, Any], repeatability_summary: dict[str, Any] | None
) -> dict[str, Any]:
    runtime_options = payload.get("runtime_options") or {}
    engine = str(payload.get("engine") or "")
    suite = str(payload.get("suite") or "")

    minimum_runs = as_int(runtime_options.get("minimum_runs_for_firm_claims"), 5)
    executed_runs = as_int(runtime_options.get("executed_runs"), 0)
    warmup_runs = as_int(runtime_options.get("warmup_runs"), 0)
    measured_runs = as_int(
        (repeatability_summary or {}).get("measured_run_count"),
        max(executed_runs - warmup_runs, 0),
    )
    warm_cold_separation = str(runtime_options.get("warm_cold_separation") or "not_reported")
    summary_statistics = str(runtime_options.get("summary_statistics") or "not_reported")
    tail_latency_statistics = str(runtime_options.get("tail_latency_statistics") or "not_reported")
    variance_policy = str(runtime_options.get("variance_policy") or "not_reported")
    outlier_policy = str(runtime_options.get("outlier_policy") or "not_reported")
    best_run_policy = str(runtime_options.get("best_run_policy") or "not_reported")

    blockers: list[str] = []
    pinning = extract_runtime_pinning(payload)
    scratchbird_storage_policy = extract_scratchbird_runtime_storage_policy(payload)
    if engine == "matrix":
        blockers.append("matrix aggregate bundles are not standalone comparison proofs; evaluate per-engine guards instead")
    if not pinning["comparison_eligible"]:
        blockers.append(pinning["reason"] or "runtime pinning is not comparison-eligible")
    if engine == "scratchbird" and not scratchbird_storage_policy["comparison_eligible"]:
        blockers.append(
            scratchbird_storage_policy["reason"] or "ScratchBird runtime storage policy is not comparison-eligible"
        )
    if measured_runs < minimum_runs:
        blockers.append(
            f"measured run count {measured_runs} is below the minimum firm-claim floor {minimum_runs}"
        )
    if warm_cold_separation in {"", "not_reported", "not_separated"}:
        blockers.append("warm/cold separation is not preserved")
    if summary_statistics in {"", "not_reported"}:
        blockers.append("summary statistics are not declared")
    if tail_latency_statistics in {"", "not_reported"}:
        blockers.append("tail latency statistics are not declared")
    if variance_policy in {"", "not_reported"}:
        blockers.append("variance policy is not declared")
    if outlier_policy in {"", "not_reported"}:
        blockers.append("outlier policy is not declared")
    if best_run_policy in {"best_run_only", "not_reported"}:
        blockers.append("best-run-only reporting is not allowed for firm claims")
    blockers.extend(suite_specific_blockers(payload, runtime_options))

    claim_status = "firm" if not blockers else "provisional"
    return {
        "schema_version": SCHEMA_VERSION,
        "engine": engine,
        "suite": suite,
        "claim_status": claim_status,
        "firm_comparison_eligible": claim_status == "firm",
        "blocking_reasons": blockers,
        "runtime_pinning": pinning,
        "scratchbird_runtime_storage_policy": scratchbird_storage_policy,
        "repeatability": {
            "executed_runs": executed_runs,
            "warmup_runs": warmup_runs,
            "measured_runs": measured_runs,
            "minimum_runs_for_firm_claims": minimum_runs,
            "warm_cold_separation": warm_cold_separation,
            "summary_statistics": summary_statistics,
            "tail_latency_statistics": tail_latency_statistics,
            "variance_policy": variance_policy,
            "outlier_policy": outlier_policy,
            "best_run_policy": best_run_policy,
            "repeatability_summary_present": repeatability_summary is not None,
        },
    }


def main() -> int:
    args = parse_args()
    payload = read_json(args.run_provenance)
    if payload is None:
        raise SystemExit(f"run provenance not found: {args.run_provenance}")
    guard = build_guard(payload, read_json(args.repeatability_summary))
    args.output_file.parent.mkdir(parents=True, exist_ok=True)
    args.output_file.write_text(json.dumps(guard, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(args.output_file)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
