#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate deterministic IPAR driver proof artifacts under build/.

The output is a contract fixture for the driver artifact gate. It proves that
the public gate rejects missing fields, enforces SBLR/UUID-only engine
authority, and admits non-TLS only on the explicit test-policy route. It is not
live benchmark evidence.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


SUITE_ROOT_REL = Path("project/tests/conformance/drivers/full_surface_scripts")
SCHEMA_NAME = "ipar_metrics_schema.json"
MANIFEST_NAME = "manifest.json"
ARTIFACT_NAMES = ("ipar-metrics.json", "ipar-metrics.jsonl", "ipar-metrics.csv")

NUMERIC_FIELD_TYPES = {
    "bytes",
    "counter",
    "duration_ms",
    "integer",
    "number",
    "percent",
    "rate",
    "ratio",
}

BASE_FIELD_VALUES: dict[str, Any] = {
    "actual": "42",
    "allocation_ms": 1,
    "allocation_stall_ms": 0,
    "auth_cache_hits": 8,
    "auth_cache_misses": 1,
    "begin_ms": 1,
    "cache_hit_rate": 1.0,
    "cache_invalidation_ms": 0,
    "case_id": "contract_case",
    "chosen_path": "fast_path",
    "command_ms": 1,
    "commit_ms": 1,
    "copy_batch_ms": 1,
    "copy_batches": 1,
    "copy_bytes": 128,
    "copy_rejects": 0,
    "copy_rows": 10,
    "datatype_id": "INTEGER",
    "dirty_page_count": 1,
    "dirty_pages_fenced": 1,
    "disk_extents_preallocated": 1,
    "driver_visible_message": "deterministic contract route proof",
    "encode_ms": 1,
    "execute_ms": 1,
    "expected": "42",
    "fallback_count": 0,
    "filespace_id": "main",
    "fsync_count": 1,
    "full_scan_count": 0,
    "group_resolution_depth": 3,
    "index_family": "btree",
    "index_insert_ms": 1,
    "index_pages_preallocated": 1,
    "index_probe_count": 1,
    "index_split_count": 1,
    "index_variant": "unique",
    "idle_cpu_percent": 0,
    "memory_growth_bytes": 0,
    "memory_peak_bytes": 4096,
    "optimizer_plan_ms": 1,
    "page_allocations": 1,
    "page_size": "8k",
    "p95_ms": 1,
    "p99_ms": 1,
    "plan_ms": 1,
    "prepared_descriptor_hit_rate_percent": 99,
    "prepared_descriptor_hits": 99,
    "prepared_descriptor_misses": 1,
    "reason_code": "none",
    "refusal_count": 0,
    "rejected_rows": 0,
    "relation_state_full_loads": 0,
    "rollback_ms": 1,
    "row_count": 1,
    "row_pages_preallocated": 1,
    "rows_affected": 1,
    "rows_deleted": 1,
    "rows_per_second": 100,
    "rows_updated": 1,
    "rows_written": 1,
    "savepoint_ms": 1,
    "security_epoch": 1,
    "security_invalidation_count": 1,
    "selected_index_path": "primary_key_probe",
    "stale_delta_reads": 0,
    "statement_id": "contract_statement",
    "table_pages_preallocated": 1,
    "transaction_inventory_fences": 1,
    "unique_probe_count": 1,
    "validation_failures": 0,
    "validation_ms": 1,
    "validation_stage": "driver_contract_fixture",
    "visibility_rechecks": 1,
}

SCRIPT_EXTRA_FIELDS: dict[str, dict[str, Any]] = {
    "SBDFS-020": {
        "prepared_insert_route_proofs": 1,
        "prepared_route_proofs": 1,
        "prepared_descriptor_session_handle_proofs": 1,
    },
    "SBDFS-059": {
        "canonical_row_stream_proofs": 1,
        "copy_route_proofs": 1,
    },
    "SBDFS-070": {
        "relation_state_full_loads": 0,
    },
}

IDENTITY_COLUMNS = [
    "script_id",
    "script",
    "driver",
    "route",
    "parser_mode",
    "page_size",
    "sslmode",
    "transport_mode",
    "tls_policy",
]


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def ensure_build_path(repo_root: Path, path: Path) -> Path:
    resolved = path.resolve()
    try:
        rel = resolved.relative_to(repo_root)
    except ValueError as exc:
        raise ValueError(f"IPAR generated artifact root must be inside the repository: {resolved}") from exc
    if not rel.parts or rel.parts[0] != "build":
        raise ValueError(f"IPAR generated artifacts must be under build/: {resolved}")
    return resolved


def schema_targets(schema: dict[str, Any], requested: list[str] | None) -> list[dict[str, Any]]:
    targets = [item for item in as_list(schema.get("target_scripts")) if isinstance(item, dict)]
    if not requested:
        return targets
    requested_set = set(requested)
    return [item for item in targets if str(item.get("script_id", "")) in requested_set]


def manifest_script_paths(manifest: dict[str, Any]) -> dict[str, str]:
    paths: dict[str, str] = {}
    for item in as_list(manifest.get("scripts")):
        if not isinstance(item, dict):
            continue
        script_id = str(item.get("script_id", ""))
        path = str(item.get("path", ""))
        if script_id and path:
            paths[script_id] = path
    return paths


def value_for_field(field: str, field_types: dict[str, Any]) -> Any:
    if field in BASE_FIELD_VALUES:
        return BASE_FIELD_VALUES[field]
    field_type = str(field_types.get(field, "scalar"))
    if field_type in NUMERIC_FIELD_TYPES:
        return 1
    if field_type == "string":
        return f"{field}_contract_value"
    return f"{field}_contract_value"


def threshold_fields(schema: dict[str, Any], script_id: str) -> set[str]:
    targets = as_dict(schema.get("beta_performance_targets"))
    fields = set(as_dict(targets.get("script_thresholds")).get(script_id, {}).keys())
    fields.update(as_list(targets.get("required_target_fields")))
    return {str(field) for field in fields}


def metric_record(
    target: dict[str, Any],
    schema: dict[str, Any],
    script_paths: dict[str, str],
    run_id: str,
) -> dict[str, Any]:
    script_id = str(target.get("script_id", ""))
    field_types = as_dict(schema.get("field_types"))
    fields = set(str(field) for field in as_list(target.get("required_fields")))
    fields.update(threshold_fields(schema, script_id))
    metrics = {field: value_for_field(field, field_types) for field in sorted(fields)}
    metrics.update(SCRIPT_EXTRA_FIELDS.get(script_id, {}))
    return {
        "driver": "cpp",
        "engine_sql_text_execution": False,
        "event": "ipar_metric",
        "parser_mode": "server-parser",
        "route": "listener-parser",
        "run_id": run_id,
        "script": script_paths.get(script_id, ""),
        "script_id": script_id,
        "sslmode": "require",
        "tls_policy": "scratchbird_tls_1_3_floor",
        "transport_mode": "tls_required",
        **metrics,
    }


def route_event(
    script_id: str,
    statement_id: str,
    *,
    copy_stream_used: bool,
    prepared_handle_used: bool,
    driver_payload_kind: str,
    engine_payload_kind: str,
    sslmode: str,
    transport_mode: str,
    tls_policy: str,
) -> dict[str, Any]:
    route = "listener-parser" if transport_mode == "tls_required" else "explicit-test-listener-parser"
    return {
        "copy_stream_used": copy_stream_used,
        "driver": "cpp",
        "driver_payload_kind": driver_payload_kind,
        "engine_payload_kind": engine_payload_kind,
        "engine_sql_text_execution": False,
        "event": "ipar_route_proof",
        "parser_mode": "server-parser",
        "parser_output_to_engine_required": True,
        "prepared_handle_used": prepared_handle_used,
        "route": route,
        "sblr_uuid_or_canonical_rows_required": True,
        "script_id": script_id,
        "sslmode": sslmode,
        "statement_id": statement_id,
        "tls_policy": tls_policy,
        "transport_mode": transport_mode,
    }


def telemetry_overhead() -> dict[str, Any]:
    return {
        "dropped_metric_count": 0,
        "metrics_disabled_ms": 0,
        "metrics_enabled_ms": 1,
        "overhead_percent": 0,
        "sample_rate": 0.007,
        "source_state": "deterministic_contract",
        "summary_source": "deterministic_contract",
        "sys.metrics.ipar.telemetry.audit_persist_skipped": 0,
        "sys.metrics.ipar.telemetry.audit_persist_stride": 128,
        "sys.metrics.ipar.telemetry.dropped_metric_count": 0,
        "sys.metrics.ipar.telemetry.metric_persist_skipped": 0,
        "sys.metrics.ipar.telemetry.metric_persist_stride": 128,
        "sys.metrics.ipar.telemetry.metrics_enabled": 1,
        "sys.metrics.ipar.telemetry.persist_sample_rate_per_mille": 7,
    }


def slow_path_rows() -> list[dict[str, Any]]:
    return [
        {
            "chosen_path": "degraded_path",
            "driver_visible_message": "deterministic contract route recorded degraded path reason",
            "fallback_count": 1,
            "reason_code": "contract_backpressure",
            "script_id": "SBDFS-059",
            "statement_id": "SBDFS-059:001",
            "validation_stage": "driver_route_contract",
        }
    ]


def route_events() -> list[dict[str, Any]]:
    tls = {
        "sslmode": "require",
        "transport_mode": "tls_required",
        "tls_policy": "scratchbird_tls_1_3_floor",
    }
    explicit_non_tls = {
        "sslmode": "disable",
        "transport_mode": "tls_disabled",
        "tls_policy": "explicit_non_tls_test_route",
    }
    return [
        route_event(
            "SBDFS-059",
            "SBDFS-059:001",
            copy_stream_used=True,
            prepared_handle_used=False,
            driver_payload_kind="copy_canonical_rows",
            engine_payload_kind="canonical_rows",
            **tls,
        ),
        route_event(
            "SBDFS-020",
            "SBDFS-020:001",
            copy_stream_used=False,
            prepared_handle_used=True,
            driver_payload_kind="prepared_descriptor_handle",
            engine_payload_kind="server_parser_sblr_uuid_output",
            **tls,
        ),
        route_event(
            "SBDFS-020",
            "SBDFS-020:explicit-non-tls",
            copy_stream_used=False,
            prepared_handle_used=True,
            driver_payload_kind="prepared_descriptor_handle",
            engine_payload_kind="server_parser_sblr_uuid_output",
            **explicit_non_tls,
        ),
    ]


def build_artifact(
    schema: dict[str, Any],
    run_id: str,
    records: list[dict[str, Any]],
    events: list[dict[str, Any]],
    slow_paths: list[dict[str, Any]],
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "schema_id": "scratchbird.ipar.performance_proof.v1",
        "suite_id": schema.get("suite_id", "scratchbird.driver.full_surface.v1"),
        "run_id": run_id,
        "driver": "cpp",
        "route": "listener-parser",
        "parser_mode": "server-parser",
        "page_size": "8k",
        "sslmode": "require",
        "transport_mode": "tls_required",
        "tls_policy": "scratchbird_tls_1_3_floor",
        "artifact_origin": "deterministic_ipar_contract_fixture",
        "generated_fixture": {
            "live_engine_metrics": False,
            "scope": "driver_route_and_metric_contract",
            "source": "public_ipar_schema_and_full_surface_manifest",
        },
        "artifact_contract": {
            "proof_outputs_under_repo_prefix": "build/",
            "engine_execution": "sblr_uuid_only",
            "engine_sql_text_execution": False,
            "parser_output_to_engine_required": True,
            "copy_route_payload": "canonical_rows",
            "transaction_finality_authority": "durable_mga_transaction_inventory",
            "driver_or_parser_finality": "forbidden",
            "missing_required_metrics_fail_gate": True,
        },
        "target_metrics": {record["script_id"]: record for record in records},
        "missing_metric_evidence": [],
        "slow_path_explanations": slow_paths,
        "route_proof_events": events,
        "telemetry_overhead": telemetry_overhead(),
    }


def write_jsonl(path: Path, records: list[dict[str, Any]], events: list[dict[str, Any]], slow_paths: list[dict[str, Any]], run_id: str) -> None:
    telemetry = {
        "event": "ipar_telemetry_summary",
        "metric_id": "IPAR-M031",
        "driver": "cpp",
        "run_id": run_id,
        "route": "listener-parser",
        "parser_mode": "server-parser",
        "page_size": "8k",
        "sslmode": "require",
        "transport_mode": "tls_required",
        "tls_policy": "scratchbird_tls_1_3_floor",
        "telemetry_overhead": telemetry_overhead(),
    }
    with path.open("w", encoding="utf-8") as handle:
        for item in [*records, telemetry, *slow_paths, *events]:
            handle.write(json.dumps(item, sort_keys=True) + "\n")


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    fieldnames = IDENTITY_COLUMNS + sorted(
        key
        for key in {field for record in records for field in record}
        if key not in set(IDENTITY_COLUMNS)
        and key not in {"event", "run_id", "engine_sql_text_execution"}
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for record in records:
            writer.writerow({field: record.get(field, "") for field in fieldnames})


def generate_fixture(
    repo_root: Path,
    artifact_root: Path,
    *,
    suite_root: Path | None = None,
    target_script_ids: list[str] | None = None,
    run_id: str = "IPAR_DETERMINISTIC_CONTRACT_FIXTURE",
) -> list[Path]:
    repo_root = repo_root.resolve()
    artifact_root = ensure_build_path(repo_root, artifact_root)
    suite_root = (suite_root or repo_root / SUITE_ROOT_REL).resolve()
    schema = load_json(suite_root / SCHEMA_NAME)
    manifest = load_json(suite_root / MANIFEST_NAME)
    script_paths = manifest_script_paths(manifest)
    records = [
        metric_record(target, schema, script_paths, run_id)
        for target in schema_targets(schema, target_script_ids)
    ]
    events = route_events()
    slow_paths = slow_path_rows()
    artifact = build_artifact(schema, run_id, records, events, slow_paths)

    artifact_root.mkdir(parents=True, exist_ok=True)
    (artifact_root / "ipar-metrics.json").write_text(
        json.dumps(artifact, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_jsonl(artifact_root / "ipar-metrics.jsonl", records, events, slow_paths, run_id)
    write_csv(artifact_root / "ipar-metrics.csv", records)
    return [artifact_root / name for name in ARTIFACT_NAMES]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--suite-root", type=Path)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--target-script", action="append", default=[])
    parser.add_argument("--run-id", default="IPAR_DETERMINISTIC_CONTRACT_FIXTURE")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    paths = generate_fixture(
        args.repo_root,
        args.artifact_root,
        suite_root=args.suite_root,
        target_script_ids=args.target_script or None,
        run_id=args.run_id,
    )
    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
