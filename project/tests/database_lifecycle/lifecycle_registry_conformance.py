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

CREATE_DATABASE_OPERATION = "lifecycle.create_database"
FIREBIRD_CREATE_REFUSAL_DIAGNOSTIC = "SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED"
LOCAL_PROFILE_REFUSAL_OPERATIONS = {"lifecycle.drop_database"}


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
        "lifecycle_header": repo_root / "project/src/engine/internal_api/lifecycle/engine_lifecycle_api.hpp",
        "lifecycle_impl": repo_root / "project/src/engine/internal_api/lifecycle/engine_lifecycle_api.cpp",
        "ddl_create_header": repo_root / "project/src/engine/internal_api/ddl/create_api.hpp",
        "ddl_create_impl": repo_root / "project/src/engine/internal_api/ddl/create_api_01_schema_table_create.inc",
        "sblr_dispatch": repo_root / "project/src/engine/sblr/sblr_dispatch.cpp",
        "sblr_static_registry": repo_root / "project/src/engine/sblr/sblr_opcode_registry.cpp",
        "server_dispatch": repo_root / "project/src/server/sblr_dispatch_server.cpp",
        "manager_control": repo_root / "project/src/server/manager_control.cpp",
        "firebird_worker": repo_root / "project/src/parsers/compatibility/firebird/firebird_worker_session.cpp",
        "public_abi": repo_root / "project/src/engine/public_abi.cpp",
        "public_abi_map": repo_root / "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_PUBLIC_ABI_MAP.csv",
    }


def assert_not_ignored(repo_root: Path, paths: list[Path]) -> None:
    for path in paths:
        rel = path.relative_to(repo_root)
        if rel.parts[:2] == ("docs", "contracts"):
            continue
        result = subprocess.run(
            ["git", "-c", f"safe.directory={repo_root}", "check-ignore", "-q", str(rel)],
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
        expected_status = "behavior_implemented"
        if row.get("implementation_status") != expected_status:
            fail(
                f"engine API registry {operation_id} implementation_status expected "
                f"{expected_status}, got {row.get('implementation_status')}"
            )
        if operation_id == CREATE_DATABASE_OPERATION:
            if row.get("requires_security_context") is not True:
                fail("engine API registry create_database must require security context")
            if row.get("requires_transaction_context") is not True:
                fail("engine API registry create_database must require transaction context")
        if "placeholder" in str(row).lower() or "stub" in str(row).lower() or "deferred" in str(row).lower():
            fail(f"engine API registry {operation_id} contains placeholder/stub/deferred language")
        if opcode not in paths["sblr_static_registry"].read_text(encoding="utf-8"):
            fail(f"static SBLR registry does not mention opcode {opcode}")


def validate_internal_matrix(paths: dict[str, Path]) -> None:
    matrix = load_yaml(paths["internal_matrix"])
    # The shared matrix also contains newer executor-schema rows keyed by
    # operation_id.  This DBLC gate owns only the lifecycle API rows and must
    # not reject unrelated rows for using their registered schema.
    lifecycle_rows = [
        row for row in (matrix.get("entries") or [])
        if str(row.get("api_operation_id", "")).startswith("lifecycle.")
    ]
    entries = by_key(lifecycle_rows, "api_operation_id", "internal SBLR/API matrix")
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
        expected_status = "behavior_implemented"
        if row.get("current_implementation_status") != expected_status:
            fail(
                f"internal SBLR/API matrix {operation_id} current_implementation_status "
                f"expected {expected_status}, got {row.get('current_implementation_status')}"
            )
        if operation_id == CREATE_DATABASE_OPERATION:
            if row.get("executor_readiness_status") != "mapped_ready":
                fail("internal SBLR/API matrix create_database is not mapped_ready")
            if row.get("required_transaction_context") is not True or \
                    row.get("required_security_context") is not True:
                fail("internal SBLR/API matrix create_database authority context is incomplete")
            if row.get("required_descriptor_inputs") != "lifecycle_create_database_descriptor":
                fail("internal SBLR/API matrix create_database descriptor is not registered")


def validate_static_sblr_registry(paths: dict[str, Path]) -> None:
    """Validate the checked-in C++ registry, the actual runtime registry authority.

    The root public-contract snapshot is narrative text rather than an opcode
    registry YAML document.  Parsing it as YAML masked this gate's intended
    checks and made the test fail before it reached any lifecycle assertion.
    """
    static_registry = paths["sblr_static_registry"].read_text(encoding="utf-8")
    for operation_id, opcode, _function in REQUIRED_LIFECYCLE_OPERATIONS:
        expected_support = (
            "local_profile_refusal"
            if operation_id in LOCAL_PROFILE_REFUSAL_OPERATIONS
            else "implemented"
        )
        entry = re.compile(
            rf'Entry\("{re.escape(operation_id)}",\s*"{re.escape(opcode)}",\s*'
            rf'(?:"[^"]+",\s*)?'
            rf'SblrOpcodeCategory::management,\s*'
            rf'SblrOpcodeSupport::{expected_support}',
            re.DOTALL,
        )
        if entry.search(static_registry) is None:
            fail(
                f"static SBLR registry missing {operation_id} -> {opcode} "
                f"with support={expected_support}"
            )


def validate_code_mappings(paths: dict[str, Path]) -> None:
    header = paths["lifecycle_header"].read_text(encoding="utf-8")
    impl = paths["lifecycle_impl"].read_text(encoding="utf-8")
    dispatch = paths["sblr_dispatch"].read_text(encoding="utf-8")
    static_registry = paths["sblr_static_registry"].read_text(encoding="utf-8")
    server_dispatch = paths["server_dispatch"].read_text(encoding="utf-8")
    manager_control = paths["manager_control"].read_text(encoding="utf-8")
    public_abi = paths["public_abi"].read_text(encoding="utf-8")
    ddl_create_header = paths["ddl_create_header"].read_text(encoding="utf-8")
    ddl_create_impl = paths["ddl_create_impl"].read_text(encoding="utf-8")
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
        if re.search(
                rf'op == "{re.escape(operation_id)}".*?{function}',
                dispatch,
                re.DOTALL) is None:
            fail(f"dispatch API map missing {operation_id} -> {function}")
        if f'Entry("{operation_id}", "{opcode}"' not in static_registry:
            fail(f"static SBLR opcode registry missing {operation_id} -> {opcode}")
        if operation_id == CREATE_DATABASE_OPERATION:
            if "CreateDatabaseFile(" not in impl:
                fail("EngineCreateLifecycle does not reach engine-owned database creation")
            if "engine-owned lifecycle mutation" not in header:
                fail("public lifecycle header lacks the engine-owned create authority boundary")
            if 'if (request.operation_key == "create_database")' not in manager_control:
                fail("manager control path does not explicitly handle create_database")
            if "EngineCreateLifecycle(" not in manager_control:
                fail("manager control path does not dispatch create_database to EngineCreateLifecycle")
            if 'if (dispatch_operation_id == "lifecycle.create_database")' in server_dispatch:
                fail("server dispatch still has a create_database seed-forwarding branch")
    if "EngineCreateDatabaseRequest" not in ddl_create_header or \
            "logical database namespace" not in ddl_create_header:
        fail("ddl.create_database is not explicitly classified as catalog namespace DDL")
    if "CreateDatabaseFile(" in ddl_create_impl or \
            "first-principal" not in ddl_create_impl:
        fail("ddl.create_database source is not isolated from physical bootstrap")


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
        expected_status = "mapped"
        if row["status"] != expected_status:
            fail(f"public ABI map status mismatch for {operation_id}")


def validate_firebird_public_create_refusal(paths: dict[str, Path]) -> None:
    """Keep the Firebird wire bootstrap boundary ahead of every stateful step."""
    worker = paths["firebird_worker"].read_text(encoding="utf-8")
    match = re.search(
        r'if \(opcode == 20\) \{  // op_create(?P<body>.*?)\n      \}\n'
        r'      const auto decoded =',
        worker,
        re.DOTALL,
    )
    if match is None:
        fail("Firebird worker lacks the explicit op_create public refusal branch")
    branch = match.group("body")
    for required in (
        FIREBIRD_CREATE_REFUSAL_DIAGNOSTIC,
        r'\"storage_mutation\":false',
        r'\"metadata_overlay_mutation\":false',
        r'\"handle_allocated\":false',
        "local_embedded_isql_startup",
        "continue;",
    ):
        if required not in branch:
            fail(f"Firebird op_create refusal branch lacks {required!r}")
    for forbidden in (
        "DecodeFirebirdParameterBuffer",
        "AuthenticateFirebirdCanonicalSession",
        "RegisterFirebirdServiceAlias",
        "PersistFirebirdMetadataOverlay",
        "state.handles",
    ):
        if forbidden in branch:
            fail(f"Firebird op_create refusal reaches forbidden stateful step {forbidden}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--compatibility-parsers", action="store_true")
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    paths = load_paths(repo_root)
    missing = [str(path) for path in paths.values() if not path.exists()]
    if missing:
        fail(f"required files missing: {missing}")
    assert_not_ignored(repo_root, list(paths.values()))
    validate_engine_registry(paths)
    validate_internal_matrix(paths)
    validate_static_sblr_registry(paths)
    validate_code_mappings(paths)
    validate_public_abi_map(paths)
    if args.compatibility_parsers:
        validate_firebird_public_create_refusal(paths)
    print(f"PASS: DBLC-002 lifecycle registry/API/ABI surface covers {len(REQUIRED_LIFECYCLE_OPERATIONS)} operations")


if __name__ == "__main__":
    main()
