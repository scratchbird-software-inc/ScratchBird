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


class UniqueKeyLoader(yaml.SafeLoader):
    """Safe loader that refuses duplicate mapping keys."""


def construct_unique_mapping(
    loader: UniqueKeyLoader, node: yaml.MappingNode, deep: bool = False
) -> dict[Any, Any]:
    loader.flatten_mapping(node)
    mapping: dict[Any, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise yaml.constructor.ConstructorError(
                "while constructing a mapping",
                node.start_mark,
                f"found duplicate key {key!r}",
                key_node.start_mark,
            )
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    construct_unique_mapping,
)


def load_yaml(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return yaml.load(handle, Loader=UniqueKeyLoader)
    except Exception as exc:  # pragma: no cover - CTest reports the message.
        raise SystemExit(f"FAIL: {path} does not parse as YAML: {exc}") from exc


def load_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:  # pragma: no cover - CTest reports the message.
        raise SystemExit(f"FAIL: {path} could not be read: {exc}") from exc


def validate_impl_row_semantics(operation_id: str, impl: dict[str, Any], errors: list[str]) -> None:
    scope = impl.get("scope_status")
    status = str(impl.get("current_implementation_status", ""))
    readiness = impl.get("executor_readiness_status")
    if scope in {"noncluster_required", "local_or_cluster"}:
        if status not in {
            "behavior_implemented",
            "behavior_implemented_policy_gated_no_hardware_release_claim",
            "exact_profile_refusal",
        }:
            errors.append(f"{operation_id} local row has unsupported implementation status {status}")
        expected_readiness = (
            {"mapped_refusal"}
            if status == "exact_profile_refusal"
            else {"mapped_ready", "sblr_callable", "not_sblr_callable"}
        )
        if readiness not in expected_readiness:
            errors.append(f"{operation_id} local row has unsupported readiness {readiness}")
    elif scope in {"cluster_only_fail_closed", "cluster_mapping_unavailable"}:
        if "fail_closed" not in status:
            errors.append(f"{operation_id} cluster row must have fail_closed status, got {status}")
        if readiness not in {
            "cluster_deferred",
            "cluster_fail_closed_mapped",
            "cluster_provider_boundary_mapped",
        }:
            errors.append(f"{operation_id} cluster row has unsupported readiness {readiness}")
    else:
        errors.append(f"{operation_id} has unsupported scope_status {scope}")


def validate_public_snapshot_text(
    spec_text: str,
    opcode_registry_text: str,
    impl_entries: list[dict[str, Any]],
    errors: list[str],
) -> None:
    for token in (
        "SBLR_OPCODE_REGISTRY_PUBLIC_SNAPSHOT",
        "registry_default_validated_by_sblr_operation_matrix",
        "MGA is the controlling transaction",
        "Parser state, reference syntax, CRUD text events",
        "SBLR_BRIDGE_VALIDATE",
    ):
        if token not in spec_text:
            errors.append(f"public contract snapshot missing authority token {token}")
    missing_opcodes = []
    for entry in impl_entries:
        operation = entry.get("sblr_operation")
        if isinstance(operation, str) and operation not in opcode_registry_text:
            missing_opcodes.append(operation)
    if missing_opcodes:
        errors.append(f"engine opcode registry missing SBLR operation matrix rows: {missing_opcodes[:20]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    root = Path(args.repo_root)
    spec_path = root / "public_contract_snapshot"
    impl_path = root / "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
    api_registry_path = root / "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml"
    opcode_registry_path = root / "project/src/engine/sblr/sblr_opcode_registry.cpp"

    spec_text = load_text(spec_path)
    opcode_registry_text = load_text(opcode_registry_path)
    impl = load_yaml(impl_path)
    api_registry = load_yaml(api_registry_path)
    errors: list[str] = []

    entries = impl.get("entries") or []
    if len(entries) < 100:
        errors.append(f"implementation matrix entry count unexpectedly small: {len(entries)}")

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
        validate_impl_row_semantics(operation_id, entry, errors)

    validate_public_snapshot_text(spec_text, opcode_registry_text, entries, errors)

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

    if errors:
        print("sblr_operation_matrix_authority_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"FAIL: {error}", file=sys.stderr)
        if len(errors) > 100:
            print(f"FAIL: ... {len(errors) - 100} additional errors omitted", file=sys.stderr)
        return 1

    print(
        "sblr_operation_matrix_authority_gate=passed "
        f"implementation_entries={len(entries)} api_registry_operations={len(api_ops)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
