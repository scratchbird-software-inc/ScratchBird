#!/usr/bin/env python3

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional


def build_execution_lane_payload(
    *,
    suite: str,
    engine_name: str,
    engine_version: str = "",
    driver_name: str = "",
    driver_version: str = "",
    storage_engine: str = "engine_default",
    lane_class: str = "",
    transaction_mode: str,
    load_mechanism: str,
    ingest_sub_lane: str = "",
    batch_size: Optional[int] = None,
    commit_grouping_policy: str,
    prepared_or_batch_behavior: str,
    statistics_refresh: str = "none",
    plan_capture_mode: str = "none",
    capability_waived: Optional[List[str]] = None,
    waived_claims: Optional[List[str]] = None,
    waiver_rationale: Optional[List[str]] = None,
    degraded_lane_justification: Optional[List[str]] = None,
    notes: Optional[List[str]] = None,
    semantic_contract: Optional[Dict[str, str]] = None,
    schema_artifact: str = "unspecified",
    index_artifact: str = "unspecified",
    encoding_collation_profile: Optional[Dict[str, Any]] = None,
    dataset_profile: Optional[Dict[str, Any]] = None,
    concurrency_profile: Optional[Dict[str, Any]] = None,
    cache_state: Optional[Dict[str, Any]] = None,
    durability_profile: Optional[Dict[str, Any]] = None,
    measurement_policy: Optional[Dict[str, Any]] = None,
    claim_ceiling: str = "",
    evidence_scope: str = "engine_core",
) -> Dict[str, Any]:
    return {
        "schema": "scratchbird_benchmarks.execution_lane.v3",
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "suite": suite,
        "engine": {
            "name": engine_name,
            "version": engine_version,
            "storage_engine": storage_engine,
        },
        "driver": {
            "name": driver_name,
            "version": driver_version,
        },
        "lane_class": lane_class,
        "evidence_scope": evidence_scope,
        "semantic_contract": semantic_contract or {},
        "schema_artifact": schema_artifact,
        "index_artifact": index_artifact,
        "encoding_collation_profile": encoding_collation_profile or {},
        "dataset_profile": dataset_profile or {},
        "concurrency_profile": concurrency_profile or {},
        "cache_state": cache_state or {},
        "durability_profile": durability_profile or {},
        "measurement_policy": measurement_policy or {},
        "execution_lane": {
            "transaction_mode": transaction_mode,
            "load_mechanism": load_mechanism,
            "ingest_sub_lane": ingest_sub_lane,
            "batch_size": batch_size,
            "commit_grouping_policy": commit_grouping_policy,
            "prepared_or_batch_behavior": prepared_or_batch_behavior,
            "statistics_refresh": statistics_refresh,
            "plan_capture_mode": plan_capture_mode,
            "capability_waived": capability_waived or [],
            "waived_claims": waived_claims or [],
            "waiver_rationale": waiver_rationale or [],
            "degraded_lane_justification": degraded_lane_justification or [],
            "notes": notes or [],
        },
        "claim_ceiling": claim_ceiling,
    }


def write_execution_lane_payload(payload: Dict[str, Any], output_path: str) -> None:
    serialized = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    path = Path(output_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(serialized, encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture benchmark execution-lane semantics for firm comparison bundles."
    )
    parser.add_argument("--suite", required=True, help="Benchmark suite name.")
    parser.add_argument("--engine-name", required=True, help="Engine name.")
    parser.add_argument("--engine-version", default="", help="Engine version.")
    parser.add_argument("--driver-name", default="", help="Driver or client library name.")
    parser.add_argument("--driver-version", default="", help="Driver or client library version.")
    parser.add_argument(
        "--lane-class",
        default="",
        help="First-class execution lane classification.",
    )
    parser.add_argument(
        "--transaction-mode",
        required=True,
        help="Effective transaction mode used by the run.",
    )
    parser.add_argument(
        "--load-mechanism",
        required=True,
        help="Effective load path used by the run.",
    )
    parser.add_argument(
        "--ingest-sub-lane",
        default="",
        help="Explicit ingest sub-lane such as prepared_multi_row_batching or server_side_file_ingest.",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=None,
        help="Effective batch size where applicable.",
    )
    parser.add_argument(
        "--commit-grouping-policy",
        required=True,
        help="How commits are grouped for the run.",
    )
    parser.add_argument(
        "--prepared-or-batch-behavior",
        required=True,
        help="Prepared statement, batched execution, or equivalent lane behavior.",
    )
    parser.add_argument(
        "--statistics-refresh",
        default="none",
        help="Statistics refresh command or 'none'.",
    )
    parser.add_argument(
        "--plan-capture-mode",
        default="none",
        help="Plan capture mode or 'none'.",
    )
    parser.add_argument(
        "--capability-waived",
        action="append",
        default=[],
        help="Native engine capability intentionally not used.",
    )
    parser.add_argument(
        "--waived-claim",
        action="append",
        default=[],
        help="Claim intentionally waived for this lane, such as ingest_throughput.",
    )
    parser.add_argument(
        "--waiver-rationale",
        action="append",
        default=[],
        help="Reason the listed claim is explicitly waived.",
    )
    parser.add_argument(
        "--degraded-lane-justification",
        action="append",
        default=[],
        help="Reason the run stayed on a degraded lane.",
    )
    parser.add_argument(
        "--notes",
        action="append",
        default=[],
        help="Additional execution-lane notes.",
    )
    parser.add_argument(
        "--contract-isolation-target",
        default="",
        help="Declared isolation target for the lane.",
    )
    parser.add_argument(
        "--contract-autocommit-behavior",
        default="",
        help="Declared autocommit behavior for the lane.",
    )
    parser.add_argument(
        "--contract-grouped-transaction-behavior",
        default="",
        help="Declared grouped-transaction behavior for the lane.",
    )
    parser.add_argument(
        "--contract-commit-boundary-policy",
        default="",
        help="Declared commit-boundary policy for the lane.",
    )
    parser.add_argument(
        "--contract-allowed-anomaly-envelope",
        default="",
        help="Declared allowed anomaly envelope for the lane.",
    )
    parser.add_argument(
        "--contract-durability-target",
        default="",
        help="Declared durability target for the lane.",
    )
    parser.add_argument(
        "--claim-ceiling",
        default="",
        help="Claim ceiling attached to the lane payload.",
    )
    parser.add_argument(
        "--evidence-scope",
        default="engine_core",
        help="Evidence classification such as engine_core or adapter_ecosystem.",
    )
    parser.add_argument(
        "--output",
        default="",
        help="Optional output file. Defaults to stdout when omitted.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    payload = build_execution_lane_payload(
        suite=args.suite,
        engine_name=args.engine_name,
        engine_version=args.engine_version,
        driver_name=args.driver_name,
        driver_version=args.driver_version,
        lane_class=args.lane_class,
        transaction_mode=args.transaction_mode,
        load_mechanism=args.load_mechanism,
        ingest_sub_lane=args.ingest_sub_lane,
        batch_size=args.batch_size,
        commit_grouping_policy=args.commit_grouping_policy,
        prepared_or_batch_behavior=args.prepared_or_batch_behavior,
        statistics_refresh=args.statistics_refresh,
        plan_capture_mode=args.plan_capture_mode,
        capability_waived=args.capability_waived,
        waived_claims=args.waived_claim,
        waiver_rationale=args.waiver_rationale,
        degraded_lane_justification=args.degraded_lane_justification,
        notes=args.notes,
        semantic_contract={
            "isolation_target": args.contract_isolation_target,
            "autocommit_behavior": args.contract_autocommit_behavior,
            "grouped_transaction_behavior": args.contract_grouped_transaction_behavior,
            "commit_boundary_policy": args.contract_commit_boundary_policy,
            "allowed_anomaly_envelope": args.contract_allowed_anomaly_envelope,
            "durability_target": args.contract_durability_target,
        },
        claim_ceiling=args.claim_ceiling,
        evidence_scope=args.evidence_scope,
    )

    if args.output:
        write_execution_lane_payload(payload, args.output)
    else:
        print(json.dumps(payload, indent=2, sort_keys=True), end="\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
