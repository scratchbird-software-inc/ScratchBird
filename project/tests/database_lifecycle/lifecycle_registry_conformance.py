#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-002 lifecycle SBLR/API/ABI registry conformance gate."""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml


REQUIRED_LIFECYCLE_OPERATIONS = (
    ("lifecycle.create_database", "SBLR_LIFECYCLE_CREATE_DATABASE", "EngineCreateLifecycle"),
    ("lifecycle.open_database", "SBLR_LIFECYCLE_OPEN_DATABASE", "EngineOpenLifecycle"),
    ("lifecycle.attach_database", "SBLR_LIFECYCLE_ATTACH_DATABASE", "EngineAttachLifecycle"),
    ("lifecycle.detach_database", "SBLR_LIFECYCLE_DETACH_DATABASE", "EngineDetachLifecycle"),
    ("lifecycle.enter_maintenance", "SBLR_LIFECYCLE_ENTER_MAINTENANCE", "EngineEnterMaintenanceLifecycle"),
    ("lifecycle.exit_maintenance", "SBLR_LIFECYCLE_EXIT_MAINTENANCE", "EngineExitMaintenanceLifecycle"),
    ("lifecycle.enter_restricted_open", "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN", "EngineEnterRestrictedOpenLifecycle"),
    ("lifecycle.exit_restricted_open", "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN", "EngineExitRestrictedOpenLifecycle"),
    ("lifecycle.inspect_database", "SBLR_LIFECYCLE_INSPECT_DATABASE", "EngineInspectLifecycle"),
    ("lifecycle.verify_database", "SBLR_LIFECYCLE_VERIFY_DATABASE", "EngineVerifyLifecycle"),
    ("lifecycle.repair_database", "SBLR_LIFECYCLE_REPAIR_DATABASE", "EngineRepairLifecycle"),
    ("lifecycle.shutdown_database", "SBLR_LIFECYCLE_SHUTDOWN_DATABASE", "EngineShutdownLifecycle"),
    ("lifecycle.shutdown_force", "SBLR_LIFECYCLE_SHUTDOWN_FORCE", "EngineForceShutdownLifecycle"),
    ("lifecycle.shutdown_acknowledge", "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE", "EngineAcknowledgeShutdownLifecycle"),
    ("lifecycle.drop_database", "SBLR_LIFECYCLE_DROP_DATABASE", "EngineDropLifecycle"),
)


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_yaml(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return yaml.safe_load(handle)
    except Exception as exc:  # pragma: no cover - CTest reports exact exception.
        fail(f"{path} does not parse as YAML: {exc}")


def load_paths(repo_root: Path) -> dict[str, Path]:
    return {
        "engine_registry": repo_root / "project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml",
        "internal_matrix": repo_root / "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml",
        "sblr_opcodes": repo_root / "public_contract_snapshot",
        "sblr_matrix": repo_root / "public_contract_snapshot",
        "lifecycle_header": repo_root / "project/src/engine/internal_api/lifecycle/engine_lifecycle_api.hpp",
        "lifecycle_impl": repo_root / "project/src/engine/internal_api/lifecycle/engine_lifecycle_api.cpp",
        "sblr_dispatch": repo_root / "project/src/engine/sblr/sblr_dispatch.cpp",
        "sblr_static_registry": repo_root / "project/src/engine/sblr/sblr_opcode_registry.cpp",
        "public_abi": repo_root / "project/src/engine/public_abi.cpp",
        "public_abi_map": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_PUBLIC_ABI_MAP.csv",
    }


def assert_not_ignored(repo_root: Path, paths: list[Path]) -> None:
    for path in paths:
        rel = path.relative_to(repo_root)
        if rel.parts[:2] == ("docs", "contracts"):
            continue
        result = subprocess.run(
            ["git", "check-ignore", "-q", str(rel)],
            cwd=repo_root,
            check=False,
        )
        if result.returncode == 0:
            fail(f"{rel} is ignored by git")
        if result.returncode not in (0, 1):
            fail(f"git check-ignore failed for {rel} with rc={result.returncode}")


def by_key(rows: list[dict[str, Any]], key: str, label: str) -> dict[str, dict[str, Any]]:
    indexed: dict[str, dict[str, Any]] = {}
    for row in rows:
        value = row.get(key)
        if not isinstance(value, str):
            fail(f"{label} row missing {key}")
        if value in indexed:
            fail(f"{label} duplicate {key}: {value}")
        indexed[value] = row
    return indexed


def request_type(function_name: str) -> str:
    return f"{function_name}Request"


def result_type(function_name: str) -> str:
    return f"{function_name}Result"


def validate_engine_registry(paths: dict[str, Path]) -> None:
    registry = load_yaml(paths["engine_registry"])
    operations = by_key(registry.get("operations") or [], "operation_id", "engine API registry")
    for operation_id, opcode, function in REQUIRED_LIFECYCLE_OPERATIONS:
        row = operations.get(operation_id)
        if row is None:
            fail(f"engine API registry missing {operation_id}")
        expected = {
            "function_name": function,
            "request_type": request_type(function),
            "result_type": result_type(function),
            "family": "lifecycle",
            "authority_domain": "engine_lifecycle",
            "header": "lifecycle/engine_lifecycle_api.hpp",
            "implementation": "lifecycle/engine_lifecycle_api.cpp",
            "default_diagnostic": "SB_ENGINE_API_OK",
        }
        for key, value in expected.items():
            if row.get(key) != value:
                fail(f"engine API registry {operation_id} {key} expected {value}, got {row.get(key)}")
        for bool_key, expected_bool in (
            ("cluster_only", False),
            ("requires_cluster_authority", False),
            ("sblr_mapping_required", True),
            ("accepts_names", False),
        ):
            if row.get(bool_key) is not expected_bool:
                fail(f"engine API registry {operation_id} {bool_key} expected {expected_bool}")
        if "placeholder" in str(row).lower() or "stub" in str(row).lower() or "deferred" in str(row).lower():
            fail(f"engine API registry {operation_id} contains placeholder/stub/deferred language")
        if opcode not in paths["sblr_static_registry"].read_text(encoding="utf-8"):
            fail(f"static SBLR registry does not mention opcode {opcode}")


def validate_internal_matrix(paths: dict[str, Path]) -> None:
    matrix = load_yaml(paths["internal_matrix"])
    entries = by_key(matrix.get("entries") or [], "api_operation_id", "internal SBLR/API matrix")
    for operation_id, opcode, function in REQUIRED_LIFECYCLE_OPERATIONS:
        row = entries.get(operation_id)
        if row is None:
            fail(f"internal SBLR/API matrix missing {operation_id}")
        expected = {
            "sblr_operation": opcode,
            "api_function_name": function,
            "request_type": request_type(function),
            "result_type": result_type(function),
            "security_authority_family": "engine_lifecycle",
            "opcode_status": "opcode_registered",
            "scope_status": "noncluster_required",
        }
        for key, value in expected.items():
            if row.get(key) != value:
                fail(f"internal SBLR/API matrix {operation_id} {key} expected {value}, got {row.get(key)}")
        if row.get("current_implementation_status") != "behavior_implemented":
            fail(f"internal SBLR/API matrix {operation_id} is not behavior_implemented")


def validate_sblr_opcode_docs(paths: dict[str, Path]) -> None:
    registry = load_yaml(paths["sblr_opcodes"])
    entries = by_key(registry.get("entries") or [], "name", "SBLR opcode registry")
    seen_codes: set[int] = set()
    for name, row in entries.items():
        code = row.get("code")
        if not isinstance(code, int):
            fail(f"SBLR opcode {name} code is not an integer")
        if code in seen_codes:
            fail(f"SBLR opcode duplicate numeric code {code:#x}")
        seen_codes.add(code)
    for operation_id, opcode, _function in REQUIRED_LIFECYCLE_OPERATIONS:
        row = entries.get(opcode)
        if row is None:
            fail(f"SBLR opcode registry missing {opcode} for {operation_id}")
        if row.get("family") != "database-management":
            fail(f"SBLR opcode {opcode} is not in database-management family")
        if row.get("status") != "required":
            fail(f"SBLR opcode {opcode} status is not required")
        code = row.get("code")
        if not isinstance(code, int) or code < 0x1400 or code > 0x14FF:
            fail(f"SBLR opcode {opcode} code {code!r} is outside database-management range")
        if row.get("security_class") not in {"admin_authorized", "sysarch_authorized"}:
            fail(f"SBLR opcode {opcode} security_class is not lifecycle-admin scoped")
        if row.get("transaction_effect") not in {"read", "management"}:
            fail(f"SBLR opcode {opcode} transaction_effect is not read/management")
        if "DBLC-002" not in str(row.get("search_key", "")) and "SBLR-DATABASE-MANAGEMENT" not in str(row.get("search_key", "")):
            fail(f"SBLR opcode {opcode} lacks DBLC-002/database-management search key")


def validate_spec_operation_matrix(paths: dict[str, Path]) -> None:
    text = paths["sblr_matrix"].read_text(encoding="utf-8")
    for operation_id, opcode, function in REQUIRED_LIFECYCLE_OPERATIONS:
        for token in (operation_id, opcode, function, "engine_lifecycle"):
            if token not in text:
                fail(f"spec SBLR operation matrix missing {token}")
    if "sblr.database.management.v3" not in text:
        fail("spec SBLR operation matrix missing database-management envelope family")


def validate_code_mappings(paths: dict[str, Path]) -> None:
    header = paths["lifecycle_header"].read_text(encoding="utf-8")
    impl = paths["lifecycle_impl"].read_text(encoding="utf-8")
    dispatch = paths["sblr_dispatch"].read_text(encoding="utf-8")
    static_registry = paths["sblr_static_registry"].read_text(encoding="utf-8")
    public_abi = paths["public_abi"].read_text(encoding="utf-8")
    if "DecodeAndDispatchSblrOperation" not in public_abi or "sb_engine_dispatch_sblr" not in public_abi:
        fail("public ABI does not route SBLR envelopes through engine dispatch")
    for operation_id, opcode, function in REQUIRED_LIFECYCLE_OPERATIONS:
        for source_name, source_text in (
            ("header", header),
            ("implementation", impl),
            ("dispatch", dispatch),
        ):
            if function not in source_text:
                fail(f"{source_name} missing {function}")
        if re.search(rf"if \(operation_id == \"{re.escape(operation_id)}\"\) return \"{opcode}\";", dispatch) is None:
            fail(f"dispatch expected-opcode map missing {operation_id} -> {opcode}")
        if re.search(rf'op == "{re.escape(operation_id)}".*{function}', dispatch) is None:
            fail(f"dispatch API map missing {operation_id} -> {function}")
        if f'Entry("{operation_id}", "{opcode}"' not in static_registry:
            fail(f"static SBLR opcode registry missing {operation_id} -> {opcode}")


def validate_public_abi_map(paths: dict[str, Path]) -> None:
    with paths["public_abi_map"].open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    required_headers = {
        "operation_id",
        "sblr_opcode",
        "public_abi_entry",
        "dispatch_route",
        "engine_entrypoint",
        "authority",
        "status",
    }
    if set(rows[0]) != required_headers:
        fail("public ABI map header mismatch")
    indexed = by_key(rows, "operation_id", "public ABI map")
    if set(indexed) != {operation_id for operation_id, _opcode, _function in REQUIRED_LIFECYCLE_OPERATIONS}:
        fail("public ABI map operation set mismatch")
    for operation_id, opcode, function in REQUIRED_LIFECYCLE_OPERATIONS:
        row = indexed[operation_id]
        if row["sblr_opcode"] != opcode:
            fail(f"public ABI map opcode mismatch for {operation_id}")
        if row["public_abi_entry"] != "sb_engine_dispatch_sblr":
            fail(f"public ABI map entry mismatch for {operation_id}")
        if row["dispatch_route"] != "DecodeAndDispatchSblrOperation->DispatchSblrOperation":
            fail(f"public ABI map dispatch route mismatch for {operation_id}")
        if row["engine_entrypoint"] != function:
            fail(f"public ABI map engine entrypoint mismatch for {operation_id}")
        if row["authority"] != "engine_lifecycle":
            fail(f"public ABI map authority mismatch for {operation_id}")
        if row["status"] != "mapped":
            fail(f"public ABI map status mismatch for {operation_id}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    paths = load_paths(repo_root)
    missing = [str(path) for path in paths.values() if not path.exists()]
    if missing:
        fail(f"required files missing: {missing}")
    assert_not_ignored(repo_root, list(paths.values()))
    validate_engine_registry(paths)
    validate_internal_matrix(paths)
    validate_sblr_opcode_docs(paths)
    validate_spec_operation_matrix(paths)
    validate_code_mappings(paths)
    validate_public_abi_map(paths)
    print(f"PASS: DBLC-002 lifecycle registry/API/ABI surface covers {len(REQUIRED_LIFECYCLE_OPERATIONS)} operations")


if __name__ == "__main__":
    main()
