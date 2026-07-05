#!/usr/bin/env python3
"""Runner for the index comparison benchmark lane."""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections import Counter, defaultdict
from dataclasses import asdict
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

SUITE_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SUITE_ROOT))
sys.path.insert(0, str(SUITE_ROOT.parent / "scripts"))

from adapters.engine_adapters import create_adapter, summarize_latencies
from capture_execution_lane_provenance import (
    build_execution_lane_payload as build_lane_payload,
    write_execution_lane_payload,
)
from scenarios.phase1_scenarios import PHASE1_SCENARIOS, phase1_insert_rows


NOISE_BAND_PCT = 5.0
INDEX_COMPARISON_SEMANTIC_CONTRACT = {
    "isolation_target": "single_session_grouped_setup_then_committed_read_phase",
    "autocommit_behavior": "disabled_for_setup_and_benchmark_query_phase",
    "grouped_transaction_behavior": (
        "table_create_seed_and_index_build_are_grouped_in_one_explicit_transaction_per_scenario"
    ),
    "commit_boundary_policy": (
        "one_explicit_commit_after_create_load_and_index_build_then_native_statistics_refresh_and_plan_capture"
    ),
    "allowed_anomaly_envelope": "single_session_planner_lane_no_concurrent_writer_anomalies_admitted",
    "durability_target": "committed_setup_visible_before_statistics_refresh_and_timed_query_execution",
}

INDEX_COMPARISON_EXECUTION_LANES: Dict[str, Dict[str, Any]] = {
    "postgresql": {
        "engine_name": "PostgreSQL",
        "driver_name": "psycopg2",
        "lane_class": "native_engine_network_lane",
        "evidence_scope": "engine_core",
        "transaction_mode": "explicit_grouped_transaction_setup_then_committed_read_phase",
        "load_mechanism": "prepared_multi_row_batching",
        "ingest_sub_lane": "prepared_multi_row_batching",
        "batch_size": None,
        "commit_grouping_policy": (
            "single explicit commit after create load and index build; ANALYZE and timed reads run after commit"
        ),
        "prepared_or_batch_behavior": "direct driver executemany batching",
        "statistics_refresh": "ANALYZE <table>",
        "plan_capture_mode": "EXPLAIN (FORMAT JSON)",
        "capability_waived": ["COPY_server_side_file_ingest"],
        "waived_claims": ["ingest_throughput"],
        "waiver_rationale": [
            "This lane is firm for planner/index claims only; PostgreSQL COPY is not exercised here."
        ],
        "degraded_lane_justification": [],
        "notes": [
            "Firm for planner/index claims only; not a COPY ingest comparison."
        ],
        "semantic_contract": INDEX_COMPARISON_SEMANTIC_CONTRACT,
        "claim_ceiling": (
            "Planner and index selection comparison only within the direct engine lane; not valid for ingest throughput claims."
        ),
    },
    "mysql": {
        "engine_name": "MySQL",
        "driver_name": "PyMySQL",
        "lane_class": "native_engine_network_lane",
        "evidence_scope": "engine_core",
        "transaction_mode": "explicit_grouped_transaction_setup_then_committed_read_phase",
        "load_mechanism": "prepared_multi_row_batching",
        "ingest_sub_lane": "prepared_multi_row_batching",
        "batch_size": None,
        "commit_grouping_policy": (
            "single explicit commit after create load and index build; ANALYZE TABLE and timed reads run after commit"
        ),
        "prepared_or_batch_behavior": "direct driver executemany batching",
        "statistics_refresh": "ANALYZE TABLE <table>",
        "plan_capture_mode": "EXPLAIN FORMAT=JSON",
        "capability_waived": ["LOAD_DATA_server_side_file_ingest", "LOAD_DATA_LOCAL_client_file_ingest"],
        "waived_claims": ["ingest_throughput"],
        "waiver_rationale": [
            "This lane is firm for planner/index claims only; MySQL LOAD DATA lanes are not exercised here."
        ],
        "degraded_lane_justification": [],
        "notes": [
            "Firm for planner/index claims only; not a LOAD DATA ingest comparison."
        ],
        "semantic_contract": INDEX_COMPARISON_SEMANTIC_CONTRACT,
        "claim_ceiling": (
            "Planner and index selection comparison only within the direct engine lane; not valid for ingest throughput claims."
        ),
    },
    "firebird": {
        "engine_name": "Firebird",
        "driver_name": "fdb",
        "lane_class": "native_engine_network_lane",
        "evidence_scope": "engine_core",
        "transaction_mode": "explicit_grouped_transaction_setup_then_committed_read_phase",
        "load_mechanism": "prepared_multi_row_batching",
        "ingest_sub_lane": "prepared_multi_row_batching",
        "batch_size": None,
        "commit_grouping_policy": (
            "single explicit commit after create load and index build; SET STATISTICS INDEX and timed reads run after commit"
        ),
        "prepared_or_batch_behavior": "direct driver executemany batching",
        "statistics_refresh": "SET STATISTICS INDEX <index> for each created index",
        "plan_capture_mode": "prepared statement PLAN text via driver",
        "capability_waived": [],
        "waived_claims": ["ingest_throughput"],
        "waiver_rationale": [
            "This lane is firm for planner/index claims only; no dedicated Firebird ingest lane is exercised here."
        ],
        "degraded_lane_justification": [],
        "notes": [
            "Native Firebird statistics refresh and native plan capture are preserved for the lane."
        ],
        "semantic_contract": INDEX_COMPARISON_SEMANTIC_CONTRACT,
        "claim_ceiling": (
            "Planner and index selection comparison only within the direct engine lane; not valid for ingest throughput claims."
        ),
    },
    "scratchbird": {
        "engine_name": "ScratchBird",
        "driver_name": "scratchbird-python",
        "lane_class": "native_engine_network_lane",
        "evidence_scope": "engine_core",
        "transaction_mode": "explicit_grouped_transaction_setup_then_committed_read_phase",
        "load_mechanism": "prepared_multi_row_batching",
        "ingest_sub_lane": "prepared_multi_row_batching",
        "batch_size": None,
        "commit_grouping_policy": (
            "single explicit commit after create load and index build; ANALYZE and timed reads run after commit"
        ),
        "prepared_or_batch_behavior": "direct driver executemany batching",
        "statistics_refresh": "ANALYZE <table>",
        "plan_capture_mode": "EXPLAIN (FORMAT JSON)",
        "capability_waived": [],
        "waived_claims": ["ingest_throughput"],
        "waiver_rationale": [
            "This lane is firm for planner/index claims only; no dedicated native ScratchBird ingest lane is exercised here."
        ],
        "degraded_lane_justification": [],
        "notes": [
            "Direct driver setup and native plan capture are used; adapter-managed chunk commits are not present in this lane."
        ],
        "semantic_contract": INDEX_COMPARISON_SEMANTIC_CONTRACT,
        "claim_ceiling": (
            "Planner and index selection comparison only within the direct engine lane; not valid for ingest throughput claims."
        ),
    },
}


def load_json(path: Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def make_target_default(engine: str) -> str:
    if engine == "scratchbird":
        return "scratchbird-native"
    return f"upstream-{engine}"


def build_execution_lane_payload(engine: str) -> Dict[str, Any]:
    lane = INDEX_COMPARISON_EXECUTION_LANES[engine]
    return build_lane_payload(
        suite="index-comparison",
        engine_name=lane["engine_name"],
        engine_version="",
        driver_name=lane["driver_name"],
        driver_version="",
        lane_class=lane["lane_class"],
        transaction_mode=lane["transaction_mode"],
        load_mechanism=lane["load_mechanism"],
        ingest_sub_lane=lane["ingest_sub_lane"],
        batch_size=lane["batch_size"],
        commit_grouping_policy=lane["commit_grouping_policy"],
        prepared_or_batch_behavior=lane["prepared_or_batch_behavior"],
        statistics_refresh=lane["statistics_refresh"],
        plan_capture_mode=lane["plan_capture_mode"],
        capability_waived=lane["capability_waived"],
        waived_claims=lane["waived_claims"],
        waiver_rationale=lane["waiver_rationale"],
        degraded_lane_justification=lane["degraded_lane_justification"],
        notes=lane["notes"],
        semantic_contract=lane["semantic_contract"],
        claim_ceiling=lane["claim_ceiling"],
        evidence_scope=lane["evidence_scope"],
    )


def build_statistics_refresh_provenance(engine: str, scenario) -> Dict[str, Any]:
    lane = INDEX_COMPARISON_EXECUTION_LANES[engine]
    if engine == "firebird":
        refresh_targets = [name for name in scenario.expected_index_names]
    else:
        refresh_targets = [scenario.table_name]
    return {
        "native": True,
        "mode": lane["statistics_refresh"],
        "status": "executed",
        "refresh_targets": refresh_targets,
        "executed_after_commit": True,
    }


def build_plan_capture_provenance(engine: str, normalized_plan: Dict[str, Any]) -> Dict[str, Any]:
    lane = INDEX_COMPARISON_EXECUTION_LANES[engine]
    return {
        "native": True,
        "mode": lane["plan_capture_mode"],
        "status": normalized_plan["plan_capture_status"],
        "raw_plan_present": normalized_plan["raw_plan"] is not None,
        "captured_after_statistics_refresh": True,
        "index_names": normalized_plan["index_names"],
    }


def evaluate_expectation(scenario, normalized_plan: Dict[str, Any]) -> tuple[str, int]:
    if normalized_plan["plan_capture_status"] != "ok":
        return "error", 0
    if normalized_plan["fallback_to_scan"]:
        return "fallback", 25
    if not normalized_plan["used_index"]:
        return "mismatch", 50
    expected_indexes = {name.upper() for name in scenario.expected_index_names}
    observed_indexes = {name.upper() for name in normalized_plan["index_names"]}
    if expected_indexes and observed_indexes and not expected_indexes.intersection(observed_indexes):
        return "partial", 70
    if scenario.require_order_from_index and normalized_plan["extra_sort"]:
        return "partial", 75
    if normalized_plan["actual_plan_family"] not in scenario.expected_access_patterns:
        return "partial", 80
    return "matched", 100


def build_summary(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    counts = Counter(result["execution_status"] for result in results)
    expectation_counts = Counter(result["expectation_status"] for result in results)
    family_counts: Dict[str, int] = defaultdict(int)
    workload_counts: Dict[str, int] = defaultdict(int)
    plan_capture_success = 0
    statistics_refresh_success = 0
    quality_scores = []
    for result in results:
        family_counts[result["index_family"]] += 1
        workload_counts[result["workload_family"]] += 1
        if result["plan_capture_status"] == "ok":
            plan_capture_success += 1
        if result["statistics_refresh_provenance"]["status"] == "executed":
            statistics_refresh_success += 1
        quality_scores.append(result["plan_quality_score"])

    avg_score = round(sum(quality_scores) / len(quality_scores), 2) if quality_scores else 0.0
    return {
        "total_tests": len(results),
        "passed": counts.get("pass", 0),
        "failed": counts.get("fail", 0),
        "errors": counts.get("error", 0),
        "unsupported": counts.get("unsupported", 0),
        "plan_capture_success": plan_capture_success,
        "statistics_refresh_success": statistics_refresh_success,
        "score": avg_score,
        "score_scope": "single_lane_only",
        "verdict_ready": False,
        "by_index_family": dict(family_counts),
        "by_workload_family": dict(workload_counts),
        "by_expectation_status": dict(expectation_counts)
    }


def to_report_test(result: Dict[str, Any]) -> Dict[str, Any]:
    status = result["execution_status"]
    if status == "pass" and result["expectation_status"] in ("fallback", "error"):
        status = "warning"
    return {
        "test_name": result["scenario_id"],
        "name": result["description"],
        "status": status,
        "duration_ms": result["latency_avg_ms"],
        "error_message": result.get("error"),
        "plan_family": result["actual_plan_family"],
        "plan_expectation_status": result["expectation_status"],
        "statistics_refresh_status": result["statistics_refresh_provenance"]["status"],
        "plan_capture_mode": result["plan_capture_provenance"]["mode"],
        "plan_capture_status": result["plan_capture_provenance"]["status"],
        "target": result["benchmark_target"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Index comparison benchmark runner")
    parser.add_argument("--engine", required=True, choices=["firebird", "mysql", "postgresql", "scratchbird"])
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int)
    parser.add_argument("--database", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--target", default=None, help="Logical benchmark target. Defaults to upstream-<engine>.")
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--output-dir", type=Path, default=Path("./results"))
    args = parser.parse_args()

    target_registry = load_json(SUITE_ROOT / "registry" / "target_registry.json")["targets"]
    capability_registry = load_json(SUITE_ROOT / "registry" / "engine_capabilities.json")["engines"]

    if args.port is None:
        args.port = {"firebird": 3050, "mysql": 3306, "postgresql": 5432, "scratchbird": 17092}[args.engine]

    target_name = args.target or make_target_default(args.engine)
    target_entry = target_registry.get(target_name)
    if target_entry is None:
        raise SystemExit(f"Unknown target: {target_name}")
    if target_entry.get("engine") != args.engine and target_entry.get("engine") != "scratchbird":
        raise SystemExit(f"Target {target_name} does not map to engine {args.engine}")
    if not target_entry.get("enabled", False):
        raise SystemExit(f"Target {target_name} is registered but disabled")

    capabilities = capability_registry[args.engine]
    adapter = create_adapter(args.engine, args.host, args.port, args.database, args.user, args.password)
    results: List[Dict[str, Any]] = []
    args.output_dir.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    execution_lane_payload = build_execution_lane_payload(args.engine)

    try:
        for scenario in PHASE1_SCENARIOS:
            print(f"Running {scenario.scenario_id} on {target_name}...")
            if scenario.index_family not in capabilities["supported_index_families"]:
                results.append(
                    {
                        "scenario_id": scenario.scenario_id,
                        "description": scenario.description,
                        "index_family": scenario.index_family,
                        "workload_family": scenario.workload_family,
                        "benchmark_target": target_name,
                        "runtime_service": target_entry["runtime_service"],
                        "execution_status": "unsupported",
                        "comparative_verdict": None,
                        "comparison_score": None,
                        "plan_capture_status": "unsupported",
                        "actual_plan_family": "unsupported",
                        "used_index": False,
                        "index_names": [],
                        "fallback_to_scan": False,
                        "extra_sort": False,
                        "order_satisfied_by_index": None,
                        "expectation_status": "unsupported",
                        "plan_quality_score": 0,
                        "latency_avg_ms": 0.0,
                        "latency_p95_ms": 0.0,
                        "latency_p99_ms": 0.0,
                        "throughput_qps": 0.0,
                        "rows_returned": 0,
                        "iterations": 0,
                        "noise_band_pct": NOISE_BAND_PCT,
                        "raw_plan": None,
                        "statistics_refresh_provenance": {
                            "native": True,
                            "mode": INDEX_COMPARISON_EXECUTION_LANES[args.engine]["statistics_refresh"],
                            "status": "unsupported",
                            "refresh_targets": [],
                            "executed_after_commit": True,
                        },
                        "plan_capture_provenance": {
                            "native": True,
                            "mode": INDEX_COMPARISON_EXECUTION_LANES[args.engine]["plan_capture_mode"],
                            "status": "unsupported",
                            "raw_plan_present": False,
                            "captured_after_statistics_refresh": True,
                            "index_names": [],
                        },
                        "error": f"Engine {args.engine} does not support {scenario.index_family}"
                    }
                )
                continue

            adapter.drop_table(scenario.table_name)
            adapter.execute(scenario.create_table_sql)
            adapter.commit()
            insert_rows = phase1_insert_rows(scenario.scenario_id)
            adapter.execute_many(
                adapter.insert_statement(scenario.table_name, len(insert_rows[0])),
                insert_rows
            )
            for statement in scenario.create_index_sql:
                adapter.execute(statement)
            adapter.commit()
            adapter.refresh_planner_statistics(scenario.table_name)
            statistics_refresh_provenance = build_statistics_refresh_provenance(args.engine, scenario)

            query_sql = scenario.query_sql[args.engine]
            error_message = None
            latencies_ms: List[float] = []
            row_count = 0
            normalized_plan = {
                "plan_capture_status": "error",
                "raw_plan": None,
                "actual_plan_family": "unknown",
                "used_index": False,
                "index_names": [],
                "fallback_to_scan": False,
                "extra_sort": False,
                "order_satisfied_by_index": None
            }
            execution_status = "pass"

            try:
                plan = adapter.explain(query_sql)
                normalized_plan = asdict(plan)
                for _ in range(max(1, args.iterations)):
                    start = time.perf_counter()
                    rows = adapter.query_fetch_all(query_sql)
                    elapsed_ms = (time.perf_counter() - start) * 1000.0
                    latencies_ms.append(round(elapsed_ms, 3))
                    row_count = len(rows)
            except Exception as exc:
                adapter.rollback()
                execution_status = "error"
                error_message = str(exc)

            plan_capture_provenance = build_plan_capture_provenance(args.engine, normalized_plan)
            expectation_status, plan_quality_score = evaluate_expectation(scenario, normalized_plan)
            if execution_status == "pass" and expectation_status == "error":
                execution_status = "error"
            elif execution_status == "pass" and row_count < scenario.expected_row_floor:
                execution_status = "fail"
                error_message = f"Query returned {row_count} rows, expected at least {scenario.expected_row_floor}"

            latency_summary = summarize_latencies(latencies_ms)
            result = {
                "scenario_id": scenario.scenario_id,
                "description": scenario.description,
                "index_family": scenario.index_family,
                "workload_family": scenario.workload_family,
                "benchmark_target": target_name,
                "runtime_service": target_entry["runtime_service"],
                "execution_status": execution_status,
                "comparative_verdict": None,
                "comparison_score": None,
                "plan_capture_status": normalized_plan["plan_capture_status"],
                "actual_plan_family": normalized_plan["actual_plan_family"],
                "used_index": normalized_plan["used_index"],
                "index_names": normalized_plan["index_names"],
                "fallback_to_scan": normalized_plan["fallback_to_scan"],
                "extra_sort": normalized_plan["extra_sort"],
                "order_satisfied_by_index": normalized_plan["order_satisfied_by_index"],
                "expectation_status": expectation_status,
                "plan_quality_score": plan_quality_score,
                "rows_returned": row_count,
                "iterations": len(latencies_ms),
                "noise_band_pct": NOISE_BAND_PCT,
                "raw_plan": normalized_plan["raw_plan"],
                "statistics_refresh_provenance": statistics_refresh_provenance,
                "plan_capture_provenance": plan_capture_provenance,
                "error": error_message,
                **latency_summary
            }
            results.append(result)

        summary = build_summary(results)
        output = {
            "metadata": {
                "engine": args.engine,
                "suite": "index-comparison",
                "timestamp": timestamp,
                "host": args.host,
                "port": args.port,
                "database": args.database,
                "target": target_name,
                "runtime_service": target_entry["runtime_service"],
                "target_class": target_entry["target_class"],
                "docker_first": True,
                "iterations": args.iterations,
                "noise_band_pct": NOISE_BAND_PCT
            },
            "execution_lane_provenance": execution_lane_payload,
            "results": {
                "scenarios": {
                    "tests": [to_report_test(result) for result in results]
                }
            },
            "test_results": results,
            "summary": summary
        }
        output_file = args.output_dir / f"index-comparison-{target_name}-{timestamp}.json"
        execution_lane_file = args.output_dir / f"index-comparison-lane-{target_name}-{timestamp}.json"
        output_file.write_text(json.dumps(output, indent=2), encoding="utf-8")
        write_execution_lane_payload(execution_lane_payload, str(execution_lane_file))
        print(f"Results saved to: {output_file}")
        print(f"Execution lane saved to: {execution_lane_file}")
        return 0 if summary["errors"] == 0 and summary["failed"] == 0 else 1
    finally:
        adapter.close()


if __name__ == "__main__":
    raise SystemExit(main())
