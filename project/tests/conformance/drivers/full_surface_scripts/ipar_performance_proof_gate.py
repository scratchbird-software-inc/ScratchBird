#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate IPAR insert/COPY performance proof artifacts.

The gate is intentionally proof-infrastructure only. It validates that targeted
full-surface runs emitted the required metric fields and writes normalized proof
summaries under build/. It does not manufacture benchmark success.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any


SUITE_ROOT = Path(__file__).resolve().parent
SCHEMA_NAME = "ipar_metrics_schema.json"
MANIFEST_NAME = "manifest.json"
REPORT_JSON_NAME = "ipar_metric_validation.json"
REPORT_CSV_NAME = "ipar_metric_validation.csv"
TARGET_PLAN_JSON_NAME = "ipar_target_scripts.json"
TARGET_PLAN_CSV_NAME = "ipar_target_scripts.csv"

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

METADATA_KEYS = {
    "acceptance",
    "driver",
    "labels",
    "metric_id",
    "metrics",
    "path",
    "route",
    "run_id",
    "script",
    "script_id",
    "source",
    "type",
    "unit",
    "value",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[5]


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def ensure_output_policy(repo_root: Path, output_root: Path) -> None:
    resolved_repo = repo_root.resolve()
    resolved_output = output_root.resolve()
    try:
        rel = resolved_output.relative_to(resolved_repo)
    except ValueError as exc:
        raise ValueError(f"output root must stay under the repository build tree: {resolved_output}") from exc
    if not rel.parts or rel.parts[0] != "build":
        raise ValueError(f"IPAR proof output inside the repository must be under build/: {resolved_output}")


def target_scripts(schema: dict[str, Any]) -> list[dict[str, Any]]:
    return [item for item in as_list(schema.get("target_scripts")) if isinstance(item, dict)]


def schema_script_ids(schema: dict[str, Any]) -> list[str]:
    return [str(item.get("script_id", "")) for item in target_scripts(schema)]


def all_required_fields(schema: dict[str, Any]) -> set[str]:
    fields: set[str] = set()
    for item in target_scripts(schema):
        fields.update(str(field) for field in as_list(item.get("required_fields")))
    fields.update(str(field) for field in as_list(as_dict(schema.get("slow_path_explanation")).get("required_fields")))
    fields.update(str(field) for field in as_list(as_dict(schema.get("telemetry_overhead_controls")).get("required_summary_fields")))
    return fields


def validate_ipar_schema(schema: dict[str, Any], suite_manifest: dict[str, Any] | None = None) -> list[str]:
    errors: list[str] = []
    if schema.get("schema_version") != 1:
        errors.append("ipar_schema:schema_version_must_be_1")
    if schema.get("schema_id") != "scratchbird.ipar.performance_proof.v1":
        errors.append("ipar_schema:schema_id_drift")
    if schema.get("suite_id") != "scratchbird.driver.full_surface.v1":
        errors.append("ipar_schema:suite_id_drift")

    authority = as_dict(schema.get("authority"))
    if authority.get("engine_execution") != "sblr_uuid_only":
        errors.append("ipar_schema:authority:engine_execution_not_sblr_uuid_only")
    if authority.get("transaction_finality_authority") != "durable_mga_transaction_inventory":
        errors.append("ipar_schema:authority:transaction_finality_not_mga_inventory")
    for key in (
        "driver_or_parser_finality_forbidden",
        "reference_or_text_event_storage_authority_forbidden",
    ):
        if authority.get(key) is not True:
            errors.append(f"ipar_schema:authority:{key}_must_be_true")

    policy = as_dict(schema.get("artifact_policy"))
    if policy.get("proof_outputs_under_repo_prefix") != "build/":
        errors.append("ipar_schema:artifact_policy:proof_outputs_must_be_under_build")
    if policy.get("public_workplans_or_proof_ledgers_forbidden") is not True:
        errors.append("ipar_schema:artifact_policy:public_ledgers_must_be_forbidden")

    record_contract = as_dict(schema.get("metric_record_contract"))
    if record_contract.get("script_metric_path_template") != "sys.metrics.ipar.script.{field}":
        errors.append("ipar_schema:metric_record_contract:path_template_drift")
    if set(as_list(record_contract.get("script_metric_labels_required"))) != {"script_id"}:
        errors.append("ipar_schema:metric_record_contract:required_labels_drift")
    if record_contract.get("field_label") != "field":
        errors.append("ipar_schema:metric_record_contract:field_label_drift")
    required_shapes = {
        "json_object_with_records_array",
        "json_object_with_script_metrics_map",
        "jsonl_record_per_metric_or_script",
        "csv_row_per_script_or_case",
        "server_metric_sample_with_path_value_labels",
    }
    if set(as_list(record_contract.get("accepted_artifact_shapes"))) != required_shapes:
        errors.append("ipar_schema:metric_record_contract:accepted_shapes_drift")

    targets = target_scripts(schema)
    target_ids = schema_script_ids(schema)
    if len(target_ids) != len(set(target_ids)):
        errors.append("ipar_schema:duplicate_target_script_ids")
    expected_ids = {
        "SBDFS-020",
        "SBDFS-059",
        "SBDFS-060",
        "SBDFS-070",
        "SBDFS-085",
        "SBDFS-090",
        "SBDFS-120",
        "SBDFS-130",
        "SBDFS-140",
        "SBDFS-150",
        "SBDFS-170",
        "SBDFS-180",
    }
    if set(target_ids) != expected_ids:
        errors.append(f"ipar_schema:target_script_id_drift:{','.join(sorted(target_ids))}")

    field_types = as_dict(schema.get("field_types"))
    missing_field_types = sorted(all_required_fields(schema) - set(field_types))
    if missing_field_types:
        errors.append(f"ipar_schema:field_types_missing:{','.join(missing_field_types)}")
    unknown_field_types = sorted(
        field for field, field_type in field_types.items()
        if field_type not in NUMERIC_FIELD_TYPES | {"scalar", "string"}
    )
    if unknown_field_types:
        errors.append(f"ipar_schema:unknown_field_types:{','.join(unknown_field_types)}")

    metric_ids: set[str] = set()
    for item in targets:
        script_id = str(item.get("script_id", ""))
        metric_id = str(item.get("metric_id", ""))
        required_fields = [str(field) for field in as_list(item.get("required_fields"))]
        if not metric_id.startswith("IPAR-M"):
            errors.append(f"ipar_schema:{script_id}:metric_id_invalid:{metric_id}")
        if metric_id in metric_ids:
            errors.append(f"ipar_schema:duplicate_metric_id:{metric_id}")
        metric_ids.add(metric_id)
        if not required_fields:
            errors.append(f"ipar_schema:{script_id}:missing_required_fields")
        if len(required_fields) != len(set(required_fields)):
            errors.append(f"ipar_schema:{script_id}:duplicate_required_fields")
        if not str(item.get("acceptance", "")).strip():
            errors.append(f"ipar_schema:{script_id}:missing_acceptance")

    telemetry = as_dict(schema.get("telemetry_overhead_controls"))
    if telemetry.get("budget_id") != "IPAR-M031":
        errors.append("ipar_schema:telemetry_budget_id_drift")
    if not as_list(telemetry.get("required_summary_fields")):
        errors.append("ipar_schema:telemetry_missing_required_summary_fields")
    if not as_list(telemetry.get("required_metric_paths")):
        errors.append("ipar_schema:telemetry_missing_required_metric_paths")
    if not isinstance(telemetry.get("default_overhead_budget_percent"), (int, float)):
        errors.append("ipar_schema:telemetry_missing_default_overhead_budget_percent")

    system_views = as_dict(schema.get("system_view_observability"))
    if system_views.get("metric_id") != "IPAR-M024":
        errors.append("ipar_schema:system_view_metric_id_drift")
    expected_views = {
        "sys.ipar.agent_lifecycle",
        "sys.ipar.metric_counters",
        "sys.ipar.telemetry_controls",
        "sys.ipar.slow_path_reasons",
    }
    observed_views = {str(view) for view in as_list(system_views.get("required_views"))}
    if observed_views != expected_views:
        errors.append(
            "ipar_schema:system_view_required_views_drift:"
            f"{','.join(sorted(observed_views))}"
        )
    required_fields_by_view = as_dict(system_views.get("required_fields_by_view"))
    expected_fields_by_view = {
        "sys.ipar.agent_lifecycle": {
            "runtime_id",
            "lifecycle_state",
            "idle_state",
            "worker_thread_count",
            "scheduler_ticks",
            "total_worker_ticks",
            "source_state",
        },
        "sys.ipar.metric_counters": {
            "metric_id",
            "metric_path",
            "metric_type",
            "metric_unit",
            "value",
            "sample_count",
            "source_state",
        },
        "sys.ipar.telemetry_controls": {
            "budget_id",
            "control_name",
            "metric_path",
            "sample_rate_per_mille",
            "persist_stride",
            "skipped_count",
            "dropped_metric_count",
            "source_state",
        },
        "sys.ipar.slow_path_reasons": {
            "metric_id",
            "statement_id",
            "chosen_path",
            "reason_code",
            "fallback_count",
            "validation_stage",
            "driver_visible_message",
            "source_state",
        },
    }
    for view, expected_fields in expected_fields_by_view.items():
        observed_fields = {
            str(field)
            for field in as_list(required_fields_by_view.get(view))
        }
        if observed_fields != expected_fields:
            errors.append(
                "ipar_schema:system_view_required_fields_drift:"
                f"{view}:{','.join(sorted(observed_fields))}"
            )

    slow_path = as_dict(schema.get("slow_path_explanation"))
    allowed_paths = {str(path) for path in as_list(slow_path.get("allowed_chosen_paths"))}
    if "fast_path" not in allowed_paths or "refused" not in allowed_paths:
        errors.append("ipar_schema:slow_path_allowed_paths_incomplete")
    required_slow_fields = {str(field) for field in as_list(slow_path.get("required_fields"))}
    if required_slow_fields != {
        "statement_id",
        "chosen_path",
        "reason_code",
        "fallback_count",
        "validation_stage",
        "driver_visible_message",
    }:
        errors.append("ipar_schema:slow_path_required_fields_drift")

    if suite_manifest is not None:
        manifest_scripts = {
            str(item.get("script_id", ""))
            for item in as_list(suite_manifest.get("scripts"))
            if isinstance(item, dict)
        }
        missing_from_manifest = sorted(expected_ids - manifest_scripts)
        if missing_from_manifest:
            errors.append(f"ipar_schema:target_scripts_missing_from_manifest:{','.join(missing_from_manifest)}")
    errors.extend(validate_target_contract(schema))
    return errors


def read_csv_records(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return [dict(row) for row in csv.DictReader(handle)]


def read_jsonl_records(path: Path) -> list[Any]:
    records: list[Any] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = line.strip()
        if not stripped:
            continue
        try:
            records.append(json.loads(stripped))
        except json.JSONDecodeError as exc:
            raise ValueError(f"{path}:{line_number}: invalid JSONL: {exc}") from exc
    return records


def load_artifact_payloads(paths: list[Path]) -> list[tuple[Path, Any]]:
    payloads: list[tuple[Path, Any]] = []
    for path in paths:
        if not path.is_file():
            raise FileNotFoundError(path)
        suffix = path.suffix.lower()
        if suffix == ".csv":
            payloads.append((path, read_csv_records(path)))
        elif suffix == ".jsonl":
            payloads.append((path, read_jsonl_records(path)))
        else:
            payloads.append((path, load_json(path)))
    return payloads


def script_id_from_record(record: dict[str, Any]) -> str:
    labels = as_dict(record.get("labels"))
    for key in ("script_id", "script", "sbdfs_script_id"):
        value = record.get(key)
        if value:
            return str(value)
        value = labels.get(key)
        if value:
            return str(value)
    return ""


def field_from_metric_path(path: str, labels: dict[str, Any], schema: dict[str, Any]) -> str:
    for key in ("field", "metric_field", "counter", "counter_name"):
        if labels.get(key):
            return str(labels[key])
    for field in sorted(as_dict(schema.get("field_types")), key=len, reverse=True):
        if path.endswith("." + field) or path.endswith("/" + field):
            return field
    return ""


def non_empty(value: Any) -> bool:
    return value is not None and str(value).strip() != ""


def is_number(value: Any) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, (int, float)):
        return True
    if isinstance(value, str):
        try:
            float(value)
        except ValueError:
            return False
        return True
    return False


def to_float(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value)
        except ValueError:
            return None
    return None


def coerce_fields(record: dict[str, Any], schema: dict[str, Any]) -> dict[str, Any]:
    fields: dict[str, Any] = {}
    nested = record.get("metrics")
    if isinstance(nested, dict):
        fields.update(nested)
    nested = record.get("fields")
    if isinstance(nested, dict):
        fields.update(nested)
    path = str(record.get("path", ""))
    if path:
        labels = as_dict(record.get("labels"))
        field = field_from_metric_path(path, labels, schema)
        if field:
            fields[field] = record.get("value")
    for key, value in record.items():
        if key not in METADATA_KEYS and key not in fields:
            fields[key] = value
    return fields


def collect_records_from_payload(
    payload: Any,
    source: Path,
    schema: dict[str, Any],
    records: list[dict[str, Any]],
    telemetry: dict[str, Any],
    slow_paths: list[dict[str, Any]],
) -> None:
    if isinstance(payload, list):
        for item in payload:
            collect_records_from_payload(item, source, schema, records, telemetry, slow_paths)
        return
    if not isinstance(payload, dict):
        return

    if isinstance(payload.get("telemetry_overhead"), dict):
        telemetry.update(payload["telemetry_overhead"])
    if isinstance(payload.get("telemetry"), dict):
        telemetry.update(payload["telemetry"])
    if isinstance(payload.get("slow_path_explanations"), list):
        for item in payload["slow_path_explanations"]:
            if isinstance(item, dict):
                item = dict(item)
                item.setdefault("_source_path", str(source))
                slow_paths.append(item)

    for container_key in ("records", "target_metrics", "script_metrics", "samples"):
        container = payload.get(container_key)
        if isinstance(container, list):
            for item in container:
                collect_records_from_payload(item, source, schema, records, telemetry, slow_paths)
        elif isinstance(container, dict):
            for script_id, value in container.items():
                if str(script_id).startswith("SBDFS-") and isinstance(value, dict):
                    record = dict(value)
                    record.setdefault("script_id", str(script_id))
                    record.setdefault("_source_path", str(source))
                    records.append(record)

    if any(str(key).startswith("SBDFS-") for key in payload.keys()):
        for script_id, value in payload.items():
            if str(script_id).startswith("SBDFS-") and isinstance(value, dict):
                record = dict(value)
                record.setdefault("script_id", str(script_id))
                record.setdefault("_source_path", str(source))
                records.append(record)

    script_id = script_id_from_record(payload)
    if script_id:
        record = dict(payload)
        record.setdefault("_source_path", str(source))
        records.append(record)
    elif str(payload.get("path", "")).startswith("sys.metrics.ipar.telemetry."):
        telemetry[str(payload.get("path"))] = payload.get("value")


def collect_artifact_records(
    payloads: list[tuple[Path, Any]],
    schema: dict[str, Any],
) -> tuple[list[dict[str, Any]], dict[str, Any], list[dict[str, Any]]]:
    records: list[dict[str, Any]] = []
    telemetry: dict[str, Any] = {}
    slow_paths: list[dict[str, Any]] = []
    for source, payload in payloads:
        collect_records_from_payload(payload, source, schema, records, telemetry, slow_paths)
    for record in records:
        fields = coerce_fields(record, schema)
        if "chosen_path" in fields or "fallback_count" in fields:
            slow_record = {
                "script_id": script_id_from_record(record),
                "_source_path": record.get("_source_path", ""),
                **fields,
            }
            slow_paths.append(slow_record)
    return records, telemetry, slow_paths


def collect_observed_fields(
    records: list[dict[str, Any]],
    schema: dict[str, Any],
    selected_script_ids: set[str],
) -> tuple[dict[str, dict[str, list[Any]]], dict[tuple[str, str], set[str]]]:
    by_script: dict[str, dict[str, list[Any]]] = {script_id: {} for script_id in selected_script_ids}
    sources_by_script_field: dict[tuple[str, str], set[str]] = {}
    for record in records:
        script_id = script_id_from_record(record)
        if script_id not in selected_script_ids:
            continue
        fields = coerce_fields(record, schema)
        for field, value in fields.items():
            if not non_empty(value):
                continue
            by_script.setdefault(script_id, {}).setdefault(field, []).append(value)
            sources_by_script_field.setdefault((script_id, field), set()).add(str(record.get("_source_path", "")))
    for script_id, fields in by_script.items():
        hits = to_float((fields.get("prepared_descriptor_hits") or [None])[-1])
        misses = to_float((fields.get("prepared_descriptor_misses") or [None])[-1])
        if hits is not None and misses is not None:
            denominator = hits + misses
            hit_rate = (hits / denominator * 100.0) if denominator > 0 else 0.0
            fields.setdefault("prepared_descriptor_hit_rate_percent", []).append(hit_rate)
            sources_by_script_field.setdefault((script_id, "prepared_descriptor_hit_rate_percent"), set()).add("derived")
    return by_script, sources_by_script_field


def validate_value_type(field: str, value: Any, schema: dict[str, Any]) -> str | None:
    field_type = str(as_dict(schema.get("field_types")).get(field, "scalar"))
    if field_type in NUMERIC_FIELD_TYPES and not is_number(value):
        return f"{field}_not_numeric"
    if field_type == "string" and not non_empty(value):
        return f"{field}_empty"
    if field_type == "scalar" and not non_empty(value):
        return f"{field}_empty"
    return None


def validate_metrics_records(
    records: list[dict[str, Any]],
    schema: dict[str, Any],
    selected_script_ids: set[str],
) -> tuple[list[str], list[dict[str, str]]]:
    errors: list[str] = []
    proof_rows: list[dict[str, str]] = []
    by_script, sources_by_script_field = collect_observed_fields(records, schema, selected_script_ids)

    targets_by_id = {str(item.get("script_id")): item for item in target_scripts(schema)}
    for script_id in sorted(selected_script_ids):
        target = targets_by_id.get(script_id)
        if target is None:
            errors.append(f"metrics:{script_id}:not_in_schema")
            continue
        observed = by_script.get(script_id, {})
        if not observed:
            errors.append(f"metrics:{script_id}:missing_script_metrics")
        for field in [str(value) for value in as_list(target.get("required_fields"))]:
            values = observed.get(field, [])
            status = "present" if values else "missing"
            if not values:
                errors.append(f"metrics:{script_id}:missing_required_field:{field}")
            else:
                type_error = validate_value_type(field, values[-1], schema)
                if type_error is not None:
                    status = "invalid"
                    errors.append(f"metrics:{script_id}:{type_error}")
            proof_rows.append(
                {
                    "script_id": script_id,
                    "metric_id": str(target.get("metric_id", "")),
                    "field": field,
                    "status": status,
                    "source": ";".join(sorted(sources_by_script_field.get((script_id, field), set()))),
                }
            )
    return errors, proof_rows


def validate_target_contract(schema: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    targets = as_dict(schema.get("beta_performance_targets"))
    if not targets:
        return ["ipar_schema:beta_performance_targets_missing"]
    if str(targets.get("target_version", "")).strip() != "2026.06.ipar.beta1":
        errors.append("ipar_schema:beta_performance_targets:target_version_drift")
    if str(targets.get("profile", "")).strip() != "single_node_local_beta":
        errors.append("ipar_schema:beta_performance_targets:profile_drift")
    required_target_fields = {str(field) for field in as_list(targets.get("required_target_fields"))}
    expected_target_fields = {
        "rows_per_second",
        "p95_ms",
        "p99_ms",
        "memory_growth_bytes",
        "allocation_stall_ms",
        "idle_cpu_percent",
        "cache_invalidation_ms",
    }
    if required_target_fields != expected_target_fields:
        errors.append(
            "ipar_schema:beta_performance_targets:required_target_fields_drift:"
            f"{','.join(sorted(required_target_fields))}"
        )
    field_types = as_dict(schema.get("field_types"))
    for field in sorted(required_target_fields):
        if field not in field_types:
            errors.append(f"ipar_schema:beta_performance_targets:field_type_missing:{field}")
    for scope_name, thresholds in (
        ("global_thresholds", as_dict(targets.get("global_thresholds"))),
        ("script_thresholds", as_dict(targets.get("script_thresholds"))),
    ):
        if not thresholds:
            errors.append(f"ipar_schema:beta_performance_targets:{scope_name}_missing")
            continue
        if scope_name == "global_thresholds":
            threshold_items = [("global", thresholds)]
        else:
            threshold_items = [(str(script_id), as_dict(value)) for script_id, value in thresholds.items()]
        for owner, owner_thresholds in threshold_items:
            for field, contract in owner_thresholds.items():
                contract_dict = as_dict(contract)
                if field not in field_types:
                    errors.append(f"ipar_schema:beta_performance_targets:{owner}:{field}:field_type_missing")
                if not non_empty(contract_dict.get("unit")):
                    errors.append(f"ipar_schema:beta_performance_targets:{owner}:{field}:unit_missing")
                has_comparator = False
                for comparator in ("min", "max"):
                    if comparator in contract_dict:
                        has_comparator = True
                        if not is_number(contract_dict.get(comparator)):
                            errors.append(
                                f"ipar_schema:beta_performance_targets:{owner}:{field}:{comparator}_not_numeric"
                            )
                if not has_comparator:
                    errors.append(f"ipar_schema:beta_performance_targets:{owner}:{field}:comparator_missing")
    return errors


def validate_performance_targets(
    records: list[dict[str, Any]],
    schema: dict[str, Any],
    selected_script_ids: set[str],
) -> tuple[list[str], list[dict[str, str]]]:
    errors: list[str] = []
    proof_rows: list[dict[str, str]] = []
    target_contract = as_dict(schema.get("beta_performance_targets"))
    global_thresholds = as_dict(target_contract.get("global_thresholds"))
    script_thresholds = as_dict(target_contract.get("script_thresholds"))
    by_script, sources_by_script_field = collect_observed_fields(records, schema, selected_script_ids)

    def evaluate(script_id: str, field: str, threshold: dict[str, Any], require_observed: bool) -> None:
        values = by_script.get(script_id, {}).get(field, [])
        if not values:
            if require_observed:
                errors.append(f"targets:{script_id}:missing_target_field:{field}")
                proof_rows.append(
                    {
                        "script_id": script_id,
                        "metric_id": "IPAR-M016",
                        "field": field,
                        "status": "target_missing",
                        "source": "",
                    }
                )
            return
        numeric = to_float(values[-1])
        if numeric is None:
            errors.append(f"targets:{script_id}:{field}_not_numeric")
            status = "target_invalid"
        else:
            status = "target_pass"
            minimum = to_float(threshold.get("min"))
            maximum = to_float(threshold.get("max"))
            if minimum is not None and numeric < minimum:
                errors.append(f"targets:{script_id}:{field}_below_min:{numeric}<{minimum}")
                status = "target_fail"
            if maximum is not None and numeric > maximum:
                errors.append(f"targets:{script_id}:{field}_above_max:{numeric}>{maximum}")
                status = "target_fail"
        proof_rows.append(
            {
                "script_id": script_id,
                "metric_id": "IPAR-M016",
                "field": field,
                "status": status,
                "source": ";".join(sorted(sources_by_script_field.get((script_id, field), set()))),
            }
        )

    for script_id in sorted(selected_script_ids):
        for field, threshold in global_thresholds.items():
            evaluate(script_id, str(field), as_dict(threshold), require_observed=False)
        for field, threshold in as_dict(script_thresholds.get(script_id)).items():
            evaluate(script_id, str(field), as_dict(threshold), require_observed=True)
    return errors, proof_rows


def validate_telemetry(telemetry: dict[str, Any], schema: dict[str, Any], allow_missing: bool) -> list[str]:
    errors: list[str] = []
    controls = as_dict(schema.get("telemetry_overhead_controls"))
    required_fields = [str(field) for field in as_list(controls.get("required_summary_fields"))]
    required_paths = [str(path) for path in as_list(controls.get("required_metric_paths"))]
    for field in required_fields:
        value = telemetry.get(field)
        if value is None:
            if not allow_missing:
                errors.append(f"telemetry:missing_required_summary_field:{field}")
            continue
        if not is_number(value):
            errors.append(f"telemetry:{field}_not_numeric")
    if telemetry.get("overhead_percent") is not None and is_number(telemetry["overhead_percent"]):
        budget = float(controls.get("default_overhead_budget_percent", 0))
        if budget > 0 and float(telemetry["overhead_percent"]) > budget:
            errors.append(
                "telemetry:overhead_percent_above_budget:"
                f"{telemetry['overhead_percent']}>{budget}"
            )
    missing_paths = [path for path in required_paths if path not in telemetry]
    if missing_paths and not allow_missing:
        errors.append(f"telemetry:missing_ipar_telemetry_metric_paths:{','.join(missing_paths)}")
    return errors


def validate_slow_paths(
    slow_paths: list[dict[str, Any]],
    schema: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    contract = as_dict(schema.get("slow_path_explanation"))
    required_fields = [str(field) for field in as_list(contract.get("required_fields"))]
    allowed_paths = {str(path) for path in as_list(contract.get("allowed_chosen_paths"))}
    for index, row in enumerate(slow_paths, start=1):
        chosen_path = str(row.get("chosen_path", "")).strip()
        if chosen_path and chosen_path not in allowed_paths:
            errors.append(f"slow_path:{index}:unknown_chosen_path:{chosen_path}")
        fallback_count = row.get("fallback_count", 0)
        try:
            fallback_number = float(fallback_count)
        except (TypeError, ValueError):
            fallback_number = 0.0
        explanation_required = chosen_path not in ("", "fast_path") or fallback_number > 0
        if not explanation_required:
            continue
        for field in required_fields:
            if not non_empty(row.get(field)):
                errors.append(f"slow_path:{index}:missing_required_field:{field}")
    return errors


def write_target_plan(
    output_root: Path,
    schema: dict[str, Any],
    suite_manifest: dict[str, Any],
    selected_script_ids: set[str],
) -> dict[str, Any]:
    scripts_by_id = {
        str(item.get("script_id")): item
        for item in as_list(suite_manifest.get("scripts"))
        if isinstance(item, dict)
    }
    targets = []
    for item in target_scripts(schema):
        script_id = str(item.get("script_id"))
        if script_id not in selected_script_ids:
            continue
        manifest_item = scripts_by_id.get(script_id, {})
        target_contract = as_dict(schema.get("beta_performance_targets"))
        targets.append(
            {
                "metric_id": item.get("metric_id"),
                "script_id": script_id,
                "script_path": manifest_item.get("path"),
                "route": item.get("route"),
                "required_fields": item.get("required_fields", []),
                "acceptance": item.get("acceptance", ""),
                "global_thresholds": as_dict(target_contract.get("global_thresholds")),
                "script_thresholds": as_dict(as_dict(target_contract.get("script_thresholds")).get(script_id)),
            }
        )
    output_root.mkdir(parents=True, exist_ok=True)
    plan = {
        "schema_version": 1,
        "schema_id": schema.get("schema_id"),
        "target_count": len(targets),
        "targets": targets,
    }
    (output_root / TARGET_PLAN_JSON_NAME).write_text(
        json.dumps(plan, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (output_root / TARGET_PLAN_CSV_NAME).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["metric_id", "script_id", "script_path", "route", "required_fields"],
        )
        writer.writeheader()
        for target in targets:
            writer.writerow(
                {
                    "metric_id": target["metric_id"],
                    "script_id": target["script_id"],
                    "script_path": target["script_path"],
                    "route": target["route"],
                    "required_fields": ";".join(str(field) for field in target["required_fields"]),
                }
            )
    return plan


def write_validation_report(
    output_root: Path,
    mode: str,
    schema: dict[str, Any],
    issues: list[str],
    proof_rows: list[dict[str, str]],
    telemetry: dict[str, Any],
    slow_path_count: int,
) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    report = {
        "command": "ipar_performance_proof_gate.py",
        "mode": mode,
        "schema_id": schema.get("schema_id"),
        "status": "fail" if issues else "pass",
        "issues": issues,
        "proof_rows": len(proof_rows),
        "telemetry_observed_keys": sorted(str(key) for key in telemetry),
        "slow_path_records": slow_path_count,
        "csv_report": str((output_root / REPORT_CSV_NAME).resolve()),
    }
    (output_root / REPORT_JSON_NAME).write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (output_root / REPORT_CSV_NAME).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["script_id", "metric_id", "field", "status", "source"],
        )
        writer.writeheader()
        for row in proof_rows:
            writer.writerow(row)


def selected_scripts(schema: dict[str, Any], requested: list[str] | None) -> set[str]:
    available = set(schema_script_ids(schema))
    if not requested:
        return available
    return {script_id for script_id in requested if script_id in available}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--suite-root", type=Path, default=SUITE_ROOT)
    parser.add_argument("--schema", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument(
        "--mode",
        choices=("schema", "plan", "validate-artifacts", "all"),
        default="schema",
    )
    parser.add_argument("--metrics-input", type=Path, action="append", default=[])
    parser.add_argument("--target-script", action="append")
    parser.add_argument("--allow-missing-telemetry", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    suite_root = args.suite_root.resolve()
    schema_path = (args.schema or suite_root / SCHEMA_NAME).resolve()
    output_root = (args.output_root or repo_root / "build" / "ipar-performance-proof").resolve()
    try:
        ensure_output_policy(repo_root, output_root)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    issues: list[str] = []
    proof_rows: list[dict[str, str]] = []
    telemetry: dict[str, Any] = {}
    slow_path_count = 0

    try:
        schema = load_json(schema_path)
    except (OSError, json.JSONDecodeError) as exc:
        schema = {}
        issues.append(f"ipar_schema:load_failed:{exc}")
    try:
        manifest = load_json(suite_root / MANIFEST_NAME)
    except (OSError, json.JSONDecodeError) as exc:
        manifest = {}
        issues.append(f"manifest:load_failed:{exc}")

    if schema:
        issues.extend(validate_ipar_schema(schema, manifest if isinstance(manifest, dict) else None))
    selected = selected_scripts(schema, args.target_script) if schema else set()

    if args.mode in ("plan", "all") and schema and isinstance(manifest, dict):
        write_target_plan(output_root, schema, manifest, selected)

    if args.mode in ("validate-artifacts", "all") and schema:
        if not args.metrics_input:
            issues.append("metrics:metrics_input_required")
        else:
            try:
                payloads = load_artifact_payloads([path.resolve() for path in args.metrics_input])
                records, telemetry, slow_paths = collect_artifact_records(payloads, schema)
                metric_errors, proof_rows = validate_metrics_records(records, schema, selected)
                issues.extend(metric_errors)
                target_errors, target_rows = validate_performance_targets(records, schema, selected)
                issues.extend(target_errors)
                proof_rows.extend(target_rows)
                issues.extend(validate_telemetry(telemetry, schema, args.allow_missing_telemetry))
                issues.extend(validate_slow_paths(slow_paths, schema))
                slow_path_count = len(slow_paths)
            except (OSError, ValueError, json.JSONDecodeError) as exc:
                issues.append(f"metrics:load_failed:{exc}")

    write_validation_report(output_root, args.mode, schema, issues, proof_rows, telemetry, slow_path_count)
    if issues:
        for issue in issues:
            print(issue, file=sys.stderr)
        return 1
    print(f"ipar_performance_proof_gate {args.mode}: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
