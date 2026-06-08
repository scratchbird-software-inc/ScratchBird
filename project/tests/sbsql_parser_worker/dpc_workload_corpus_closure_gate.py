#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Standalone DPC workload corpus closure gate.

This gate validates deterministic workload-corpus definitions for later live
benchmark gates. It does not execute optimizer features and does not read
execution_plan artifacts at runtime.

Search key: DPC_WORKLOAD_CORPUS_GATE
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


CORPUS_VERSION = "dpc-workload-corpus-v1"
CORPUS_SEED = 2026052207
HASH_ALGORITHM = "sha256:canonical-json:utf-8:sort-keys"
OUTPUT_SCHEMA_VERSION = "dpc.workload_corpus_closure.output.v1"
REQUIRED_LANES = ("WL04", "WL06", "WL08", "WL11", "WL12")
REQUIRED_ROUTE_IDS = ("embedded", "local_ipc", "inet_listener")
REQUIRED_BUILD_FLAGS = {
    "SCRATCHBIRD_ENABLE_DEBUG_LOGS": "OFF",
    "SCRATCHBIRD_ENABLE_HOTPATH_TRACE": "OFF",
    "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE": "OFF",
    "SCRATCHBIRD_ENABLE_PREPARED_TRACE": "OFF",
}
REQUIRED_OUTPUT_FIELDS = (
    "schema_version",
    "corpus_version",
    "corpus_seed",
    "lane_id",
    "route_id",
    "transaction_mode",
    "schema_name",
    "row_count",
    "variant_id",
    "runtime_flags",
    "build_flags",
    "result_hash_algorithm",
    "expected_result_hash",
    "rows_input",
    "rows_affected",
    "rows_returned",
    "metric_requirements",
)


class CorpusGateError(RuntimeError):
    pass


def canonical_hash(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def base_lane(
    lane_id: str,
    name: str,
    schema_name: str,
    row_count: int,
    variants: tuple[dict[str, Any], ...],
    metric_requirements: tuple[str, ...],
) -> dict[str, Any]:
    return {
        "lane_id": lane_id,
        "name": name,
        "schema_name": schema_name,
        "namespace_path": f"/scratchbird/dpc/workload_corpus/{schema_name}",
        "corpus_version": CORPUS_VERSION,
        "corpus_seed": CORPUS_SEED,
        "row_count": row_count,
        "row_width_bytes": 256,
        "skew": {
            "distribution": "zipfian",
            "theta": 0.86,
            "hot_key_percent": 1,
            "stable_seed": CORPUS_SEED,
        },
        "null_distribution": {
            "nullable_columns": ("note", "optional_score", "closed_at"),
            "null_percent": 7,
            "stable_seed": CORPUS_SEED + 17,
        },
        "index_set": (
            "pk_id",
            "idx_tenant_status",
            "idx_hot_key",
            "idx_updated_at",
            "idx_metric_value",
        ),
        "routes": REQUIRED_ROUTE_IDS,
        "transaction_modes": ("autocommit", "explicit_commit", "explicit_rollback"),
        "build_flags": REQUIRED_BUILD_FLAGS,
        "runtime_flags": {
            "SCRATCHBIRD_DPC_CORPUS_VERSION": CORPUS_VERSION,
            "SCRATCHBIRD_DPC_CORPUS_SEED": str(CORPUS_SEED),
            "SCRATCHBIRD_DPC_OPTIMIZATION_FEATURES": "disabled",
            "SCRATCHBIRD_DPC_PAGE_SUMMARY_MODE": "metadata-placeholder-only",
        },
        "result_hash_algorithm": HASH_ALGORITHM,
        "metric_requirements": metric_requirements,
        "variants": variants,
        "output_json_contract": REQUIRED_OUTPUT_FIELDS,
    }


def manifest_without_hashes() -> dict[str, Any]:
    return {
        "schema_version": OUTPUT_SCHEMA_VERSION,
        "corpus_version": CORPUS_VERSION,
        "corpus_seed": CORPUS_SEED,
        "result_hash_policy": {
            "algorithm": HASH_ALGORITHM,
            "scope": "per lane plus route plus transaction mode plus variant",
            "volatile_fields_excluded": ("elapsed_ms", "timestamp_utc", "database_path"),
        },
        "route_ids": REQUIRED_ROUTE_IDS,
        "build_flags": REQUIRED_BUILD_FLAGS,
        "runtime_flags": {
            "SCRATCHBIRD_DPC_CORPUS_VERSION": CORPUS_VERSION,
            "SCRATCHBIRD_DPC_CORPUS_SEED": str(CORPUS_SEED),
            "SCRATCHBIRD_DPC_OPTIMIZATION_FEATURES": "disabled",
            "SCRATCHBIRD_DPC_PAGE_SUMMARY_MODE": "metadata-placeholder-only",
        },
        "standalone_runtime_policy": {
            "reads_execution_plan_files": False,
            "requires_engine_feature_implementation": False,
            "claims_speed_improvement": False,
            "page_summary_claim": "metadata placeholders only; no pruning feature claim",
        },
        "lanes": (
            base_lane(
                "WL04",
                "hot row update",
                "dpc_wl04_hot_row_update",
                100000,
                (
                    {
                        "variant_id": "non_key_hot_update",
                        "hot_key": 42,
                        "session_count": 8,
                        "updates_per_session": 5000,
                        "updated_columns": ("payload", "note", "updated_at"),
                        "indexed_key_changes": False,
                    },
                    {
                        "variant_id": "indexed_key_hot_update",
                        "hot_key": 42,
                        "session_count": 8,
                        "updates_per_session": 5000,
                        "updated_columns": ("hot_key", "tenant_id", "status_code"),
                        "indexed_key_changes": True,
                    },
                ),
                (
                    "rows_affected",
                    "result_hash",
                    "mga_visibility_snapshot",
                    "index_lookup_equality",
                ),
            ),
            base_lane(
                "WL06",
                "delete with secondary index",
                "dpc_wl06_delete_with_index",
                200000,
                (
                    {
                        "variant_id": "delete_1pct_commit",
                        "delete_percent": 1,
                        "transaction_mode": "explicit_commit",
                        "secondary_indexes_required": True,
                    },
                    {
                        "variant_id": "delete_1pct_rollback",
                        "delete_percent": 1,
                        "transaction_mode": "explicit_rollback",
                        "secondary_indexes_required": True,
                    },
                    {
                        "variant_id": "delete_20pct_commit",
                        "delete_percent": 20,
                        "transaction_mode": "explicit_commit",
                        "secondary_indexes_required": True,
                    },
                    {
                        "variant_id": "delete_20pct_rollback",
                        "delete_percent": 20,
                        "transaction_mode": "explicit_rollback",
                        "secondary_indexes_required": True,
                    },
                ),
                (
                    "rows_affected",
                    "post_delete_count",
                    "rollback_visibility_equality",
                    "secondary_index_consistency",
                ),
            ),
            base_lane(
                "WL08",
                "range scan",
                "dpc_wl08_range_scan",
                250000,
                (
                    {
                        "variant_id": "scalar_range_full_scan_compare",
                        "predicate": "metric_value BETWEEN 1000 AND 1999",
                        "comparison_route": "full_scan",
                        "page_summary_metadata": "placeholder_only",
                    },
                    {
                        "variant_id": "date_range_full_scan_compare",
                        "predicate": "event_date >= DATE '2026-01-01' AND event_date < DATE '2026-02-01'",
                        "comparison_route": "full_scan",
                        "page_summary_metadata": "placeholder_only",
                    },
                ),
                (
                    "rows_returned",
                    "full_scan_result_hash",
                    "range_result_hash",
                    "page_summary_metadata_presence",
                ),
            ),
            base_lane(
                "WL11",
                "mixed read write",
                "dpc_wl11_mixed_read_write",
                150000,
                (
                    {
                        "variant_id": "mixed_load_select_update_delete_cleanup_agent",
                        "load_clients": 2,
                        "select_clients": 6,
                        "update_clients": 4,
                        "delete_clients": 2,
                        "cleanup_pressure": "enabled",
                        "agent_activity": ("storage_cleanup", "index_cleanup", "resource_governor_probe"),
                    },
                ),
                (
                    "read_result_hash",
                    "write_result_hash",
                    "cleanup_horizon_respected",
                    "agent_activity_observed",
                ),
            ),
            base_lane(
                "WL12",
                "high concurrency DML",
                "dpc_wl12_high_concurrency_dml",
                300000,
                (
                    {
                        "variant_id": "clients_4_visibility_allocation_wait",
                        "client_count": 4,
                        "session_count": 4,
                    },
                    {
                        "variant_id": "clients_8_visibility_allocation_wait",
                        "client_count": 8,
                        "session_count": 8,
                    },
                    {
                        "variant_id": "clients_16_visibility_allocation_wait",
                        "client_count": 16,
                        "session_count": 16,
                    },
                ),
                (
                    "transaction_visibility",
                    "allocation_wait_ms",
                    "page_allocation_wait_ms",
                    "filespace_growth_wait_ms",
                    "result_hash",
                ),
            ),
        ),
    }


EXPECTED_LANE_HASHES = {
    "WL04": "dce44fae9cb4c3850abb5f92d7f02e0c5d177b127434fe5ffdaf9afbb44e9de5",
    "WL06": "8f9f930dbc43e30e4cac5797ffcfb1c7f727879c4ee9d3084a81c0676ff91261",
    "WL08": "2e9459ce5b1a3f6f19319c83bb5b7d24084e8669a31a01e86ff7d59b48221491",
    "WL11": "61e80393be7e95a382b25895c20b1568956291cf709fe435b041045d1e0d8302",
    "WL12": "b2101e7f657bb1659085ad05078d3a18aa094fad84cb2064bf2c3bca2445081b",
}

EXPECTED_MANIFEST_HASH = "a361778612009988cf8a6bdbfeb1fd59164e5838a755f53620fe8d8d3f44d750"


def with_expected_hashes(manifest: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(manifest)
    for lane in result["lanes"]:
        lane["expected_result_hash"] = EXPECTED_LANE_HASHES[lane["lane_id"]]
    result["manifest_hash"] = EXPECTED_MANIFEST_HASH
    return result


def fail(message: str) -> None:
    raise CorpusGateError(message)


def validate_lane(lane: dict[str, Any]) -> None:
    lane_id = lane.get("lane_id")
    if lane_id not in REQUIRED_LANES:
        fail(f"unexpected lane id {lane_id!r}")
    if lane.get("corpus_version") != CORPUS_VERSION:
        fail(f"{lane_id}: corpus version mismatch")
    if lane.get("corpus_seed") != CORPUS_SEED:
        fail(f"{lane_id}: corpus seed mismatch")
    if not str(lane.get("schema_name", "")).startswith("dpc_wl"):
        fail(f"{lane_id}: schema name must be fixed dpc_wl* name")
    if lane.get("namespace_path") != f"/scratchbird/dpc/workload_corpus/{lane['schema_name']}":
        fail(f"{lane_id}: namespace path mismatch")
    if lane.get("row_count", 0) <= 0 or lane.get("row_width_bytes", 0) <= 0:
        fail(f"{lane_id}: row count and row width must be positive")
    if tuple(lane.get("routes", ())) != REQUIRED_ROUTE_IDS:
        fail(f"{lane_id}: route ids mismatch")
    if "explicit_commit" not in lane.get("transaction_modes", ()):
        fail(f"{lane_id}: explicit commit transaction mode missing")
    if "explicit_rollback" not in lane.get("transaction_modes", ()):
        fail(f"{lane_id}: explicit rollback transaction mode missing")
    if lane.get("build_flags") != REQUIRED_BUILD_FLAGS:
        fail(f"{lane_id}: build flags mismatch")
    if lane.get("runtime_flags", {}).get("SCRATCHBIRD_DPC_OPTIMIZATION_FEATURES") != "disabled":
        fail(f"{lane_id}: runtime disabled-feature flag missing")
    if lane.get("result_hash_algorithm") != HASH_ALGORITHM:
        fail(f"{lane_id}: result hash algorithm mismatch")
    if tuple(lane.get("output_json_contract", ())) != REQUIRED_OUTPUT_FIELDS:
        fail(f"{lane_id}: output JSON contract mismatch")
    if not lane.get("index_set") or "pk_id" not in lane["index_set"]:
        fail(f"{lane_id}: index set missing primary key")
    if not lane.get("variants"):
        fail(f"{lane_id}: variants missing")
    computed = canonical_hash(lane)
    expected = EXPECTED_LANE_HASHES[lane_id]
    if computed != expected:
        fail(f"{lane_id}: deterministic lane hash changed: {computed} != {expected}")


def validate_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    if manifest["corpus_version"] != CORPUS_VERSION:
        fail("manifest corpus version mismatch")
    if manifest["corpus_seed"] != CORPUS_SEED:
        fail("manifest corpus seed mismatch")
    if tuple(manifest["route_ids"]) != REQUIRED_ROUTE_IDS:
        fail("manifest route ids mismatch")
    if manifest["build_flags"] != REQUIRED_BUILD_FLAGS:
        fail("manifest build flags mismatch")
    if manifest["standalone_runtime_policy"]["reads_execution_plan_files"] is not False:
        fail("standalone runtime policy must reject execution_plan runtime reads")
    lane_ids = tuple(lane["lane_id"] for lane in manifest["lanes"])
    if lane_ids != REQUIRED_LANES:
        fail(f"lane order mismatch: {lane_ids!r}")
    for lane in manifest["lanes"]:
        validate_lane(lane)
    computed_manifest_hash = canonical_hash(manifest)
    if computed_manifest_hash != EXPECTED_MANIFEST_HASH:
        fail(f"manifest hash changed: {computed_manifest_hash} != {EXPECTED_MANIFEST_HASH}")
    return with_expected_hashes(manifest)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        manifest = validate_manifest(manifest_without_hashes())
        args.output_dir.mkdir(parents=True, exist_ok=True)
        output_path = args.output_dir / "dpc_workload_corpus_closure_manifest.json"
        output_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"DPC workload corpus closure gate passed: {output_path}")
        return 0
    except CorpusGateError as exc:
        print(f"DPC workload corpus closure gate failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
