#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBLR operation matrix spec/implementation authority gate."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import yaml


BACKPORT_KEY = "engine_sblr_api_operation_matrix_backport_v1"
LIFECYCLE_OVERLAY_KEY = "DBLC_002_lifecycle_operation_rows"

REQUIRED_IMPL_FIELDS = (
    "sblr_operation",
    "opcode_status",
    "api_operation_id",
    "scope_status",
    "closure_slice",
    "api_function_name",
    "request_type",
    "result_type",
    "required_transaction_context",
    "result_shape",
    "diagnostic_mapping",
    "evidence_mapping",
    "current_implementation_status",
    "executor_readiness_status",
)


def load_yaml(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return yaml.safe_load(handle)
    except Exception as exc:  # pragma: no cover - CTest reports the message.
        raise SystemExit(f"FAIL: {path} does not parse as YAML: {exc}") from exc


def path_label(path: tuple[Any, ...]) -> str:
    label = ""
    for part in path:
        if isinstance(part, int):
            label += f"[{part}]"
        else:
            label += f"/{part}"
    return label or "/"


def walk_operation_records(node: Any, path: tuple[Any, ...] = ()) -> list[tuple[tuple[Any, ...], dict[str, Any]]]:
    records: list[tuple[tuple[Any, ...], dict[str, Any]]] = []
    if isinstance(node, dict):
        if "operation_id" in node and "sblr_operation" in node:
            records.append((path, node))
        for key, value in node.items():
            records.extend(walk_operation_records(value, path + (key,)))
    elif isinstance(node, list):
        for index, value in enumerate(node):
            records.extend(walk_operation_records(value, path + (index,)))
    return records


def collect_backport_operations(backport: dict[str, Any], errors: list[str]) -> dict[str, dict[str, Any]]:
    by_operation: dict[str, dict[str, Any]] = {}
    seen_sblr: dict[str, str] = {}
    total = 0
    groups = backport.get("groups")
    if not isinstance(groups, list):
        errors.append(f"{BACKPORT_KEY}.groups must be a list")
        return by_operation

    for group in groups:
        group_name = group.get("group")
        operations = group.get("operations")
        if not isinstance(group_name, str):
            errors.append(f"{BACKPORT_KEY} group missing group name")
            continue
        if not isinstance(operations, list):
            errors.append(f"{BACKPORT_KEY}.{group_name}.operations must be a list")
            continue
        declared_count = group.get("operation_count")
        if declared_count != len(operations):
            errors.append(
                f"{BACKPORT_KEY}.{group_name} operation_count={declared_count} actual={len(operations)}"
            )
        total += len(operations)
        for row in operations:
            operation_id = row.get("operation_id") if isinstance(row, dict) else None
            sblr_operation = row.get("sblr_operation") if isinstance(row, dict) else None
            if not isinstance(operation_id, str) or not isinstance(sblr_operation, str):
                errors.append(f"{BACKPORT_KEY}.{group_name} contains an operation without operation_id/sblr_operation")
                continue
            if operation_id in by_operation:
                errors.append(f"duplicate canonical operation_id in {BACKPORT_KEY}: {operation_id}")
            if sblr_operation in seen_sblr:
                errors.append(
                    f"duplicate canonical sblr_operation in {BACKPORT_KEY}: {sblr_operation} "
                    f"for {seen_sblr[sblr_operation]} and {operation_id}"
                )
            by_operation[operation_id] = {
                "operation_id": operation_id,
                "sblr_operation": sblr_operation,
                "group": group_name,
            }
            seen_sblr[sblr_operation] = operation_id

    declared_total = backport.get("total_operation_count")
    if declared_total != total:
        errors.append(f"{BACKPORT_KEY}.total_operation_count={declared_total} actual={total}")
    return by_operation


def validate_lifecycle_overlay(spec: dict[str, Any], canonical_ops: dict[str, dict[str, Any]], errors: list[str]) -> None:
    overlay = spec.get(LIFECYCLE_OVERLAY_KEY)
    if not isinstance(overlay, dict):
        errors.append(f"{LIFECYCLE_OVERLAY_KEY} missing")
        return
    row_contract = overlay.get("row_contract") or {}
    if row_contract.get("canonical_operation_authority") != BACKPORT_KEY:
        errors.append(f"{LIFECYCLE_OVERLAY_KEY}.row_contract must point canonical_operation_authority at {BACKPORT_KEY}")
    if row_contract.get("operation_row_key_mode") != "detail_overlay_extends_canonical_backport":
        errors.append(f"{LIFECYCLE_OVERLAY_KEY}.row_contract must declare detail overlay key mode")
    for row in overlay.get("operations") or []:
        operation_id = row.get("extends_operation_id")
        sblr_operation = row.get("extends_sblr_operation")
        canonical = canonical_ops.get(operation_id)
        if canonical is None:
            errors.append(f"{LIFECYCLE_OVERLAY_KEY} overlay references unknown operation {operation_id}")
            continue
        if canonical.get("sblr_operation") != sblr_operation:
            errors.append(
                f"{LIFECYCLE_OVERLAY_KEY} overlay {operation_id} sblr mismatch: "
                f"{sblr_operation} != {canonical.get('sblr_operation')}"
            )


def validate_impl_groups(
    canonical_ops: dict[str, dict[str, Any]], impl_by_operation: dict[str, dict[str, Any]], errors: list[str]
) -> None:
    for operation_id, canonical in canonical_ops.items():
        impl = impl_by_operation[operation_id]
        group = canonical["group"]
        scope = impl.get("scope_status")
        status = impl.get("current_implementation_status")
        readiness = impl.get("executor_readiness_status")
        if group in {"noncluster_supported", "source_scope_gap_normalized_noncluster_required"}:
            if scope != "noncluster_required":
                errors.append(f"{operation_id} must be noncluster_required for {group}, got {scope}")
            if status != "behavior_implemented":
                errors.append(f"{operation_id} must be behavior_implemented for {group}, got {status}")
            if readiness not in {"mapped_ready", "sblr_callable"}:
                errors.append(f"{operation_id} must be mapped/sblr callable for {group}, got {readiness}")
        elif group == "engine_api_internal_not_external_sblr_callable":
            if scope != "noncluster_required":
                errors.append(f"{operation_id} internal API row must remain noncluster_required, got {scope}")
            if status != "behavior_implemented":
                errors.append(f"{operation_id} internal API row must be behavior_implemented, got {status}")
            if readiness != "not_sblr_callable":
                errors.append(f"{operation_id} must not be externally SBLR callable, got {readiness}")
        elif group == "noncluster_supported_policy_gated_no_hardware_claim":
            if scope != "noncluster_required":
                errors.append(f"{operation_id} policy-gated row must be noncluster_required, got {scope}")
            if status != "behavior_implemented_policy_gated_no_hardware_release_claim":
                errors.append(f"{operation_id} policy-gated row has wrong status {status}")
            if readiness != "mapped_ready":
                errors.append(f"{operation_id} policy-gated row must be mapped_ready, got {readiness}")
        elif group == "cluster_fail_closed":
            if scope not in {"cluster_only_fail_closed", "cluster_mapping_unavailable"}:
                errors.append(f"{operation_id} cluster row must be fail-closed scoped, got {scope}")
            if "fail_closed" not in str(status):
                errors.append(f"{operation_id} cluster row must have fail_closed status, got {status}")
            if readiness not in {
                "cluster_deferred",
                "cluster_fail_closed_mapped",
                "cluster_provider_boundary_mapped",
            }:
                errors.append(f"{operation_id} cluster row must be cluster deferred/fail-closed mapped, got {readiness}")
        else:
            errors.append(f"{operation_id} appears in unknown SBLR matrix group {group}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    root = Path(args.repo_root)
    spec_path = root / "public_contract_snapshot"
    impl_path = root / "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
    api_registry_path = root / "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml"

    spec = load_yaml(spec_path)
    impl = load_yaml(impl_path)
    api_registry = load_yaml(api_registry_path)
    errors: list[str] = []

    backport = spec.get(BACKPORT_KEY)
    if not isinstance(backport, dict):
        errors.append(f"{BACKPORT_KEY} missing from spec registry")
        backport = {}
    elif backport.get("normative") is not True:
        errors.append(f"{BACKPORT_KEY}.normative must be true")

    canonical_ops = collect_backport_operations(backport, errors)
    canonical_pairs = {(row["operation_id"], row["sblr_operation"]) for row in canonical_ops.values()}

    for record_path, row in walk_operation_records(spec):
        if not record_path or record_path[0] != BACKPORT_KEY:
            errors.append(
                "non-canonical operation authority row outside "
                f"{BACKPORT_KEY}: {path_label(record_path)} "
                f"{row.get('operation_id')} {row.get('sblr_operation')}"
            )

    validate_lifecycle_overlay(spec, canonical_ops, errors)

    entries = impl.get("entries") or []
    if len(entries) != len(canonical_ops):
        errors.append(f"implementation matrix entry count {len(entries)} != canonical operation count {len(canonical_ops)}")

    impl_by_operation: dict[str, dict[str, Any]] = {}
    impl_sblr_by_name: dict[str, str] = {}
    for entry in entries:
        operation_id = entry.get("api_operation_id")
        sblr_operation = entry.get("sblr_operation")
        if not isinstance(operation_id, str) or not isinstance(sblr_operation, str):
            errors.append("implementation matrix row missing api_operation_id/sblr_operation")
            continue
        if operation_id in impl_by_operation:
            errors.append(f"duplicate implementation api_operation_id: {operation_id}")
        if sblr_operation in impl_sblr_by_name:
            errors.append(f"duplicate implementation sblr_operation: {sblr_operation}")
        impl_by_operation[operation_id] = entry
        impl_sblr_by_name[sblr_operation] = operation_id
        for field in REQUIRED_IMPL_FIELDS:
            if entry.get(field) in (None, ""):
                errors.append(f"{operation_id} missing required implementation metadata field {field}")

    impl_pairs = {(row.get("api_operation_id"), row.get("sblr_operation")) for row in entries}
    missing_in_spec = sorted(impl_pairs - canonical_pairs)
    missing_in_impl = sorted(canonical_pairs - impl_pairs)
    if missing_in_spec:
        errors.append(f"implementation pairs missing canonical spec rows: {missing_in_spec[:20]}")
    if missing_in_impl:
        errors.append(f"canonical spec pairs missing implementation rows: {missing_in_impl[:20]}")

    api_ops = {
        row.get("operation_id")
        for row in (api_registry.get("operations") or [])
        if isinstance(row, dict) and isinstance(row.get("operation_id"), str)
    }
    if api_ops != set(impl_by_operation):
        errors.append(
            "engine API surface registry and SBLR implementation matrix operation sets differ: "
            f"api_only={sorted(api_ops - set(impl_by_operation))[:20]} "
            f"matrix_only={sorted(set(impl_by_operation) - api_ops)[:20]}"
        )

    if not errors:
        validate_impl_groups(canonical_ops, impl_by_operation, errors)

    if errors:
        print("sblr_operation_matrix_authority_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"FAIL: {error}", file=sys.stderr)
        if len(errors) > 100:
            print(f"FAIL: ... {len(errors) - 100} additional errors omitted", file=sys.stderr)
        return 1

    print(
        "sblr_operation_matrix_authority_gate=passed "
        f"canonical_operations={len(canonical_ops)} implementation_entries={len(entries)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
