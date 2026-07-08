#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""UDR implementation coverage gate.

This gate keeps the public UDR surface honest. It verifies that the runtime
authority checks, parser-support UDR packages, compatibility bridge policy, and
public naming boundary all remain wired in source. Runtime behavior is tested by
the C++ UDR probes; this gate protects the coverage inventory those probes rely
on.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path


REQUIRED_RUNTIME_TOKENS = {
    "runtime header": "struct UdrPackageDescriptor",
    "runtime state": "struct UdrPackageRuntimeState",
    "runtime register": "UdrStatus RegisterPackage",
    "runtime load": "UdrStatus LoadPackage",
    "runtime unload": "UdrStatus UnloadPackage",
    "runtime unregister": "UdrStatus UnregisterPackage",
    "runtime invoke": "UdrCallResult InvokePackage",
}

REQUIRED_RUNTIME_IMPL_TOKENS = {
    "non-cpp refusal": "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN",
    "provenance required": "UDR.RUNTIME.PROVENANCE_REQUIRED",
    "descriptor conflict": "UDR.RUNTIME.PACKAGE_DESCRIPTOR_CONFLICT",
    "unload active-call block": "UDR.UNLOAD_BLOCKED",
    "unregister missing package": "UDR.RUNTIME.PACKAGE_NOT_REGISTERED",
    "active invocation accounting": "active_invocations",
}

REQUIRED_ENGINE_API_TOKENS = {
    "security context": "EngineExtensionSecurityRequired",
    "alter UDR API": "EngineAlterUdrPackage",
    "drop UDR API": "EngineDropUdrPackage",
    "manage/invoke rights": "SB_ENGINE_API_UDR_PERMISSION_REQUIRED",
    "SBLR invocation required": "SB_ENGINE_API_UDR_SBLR_INVOCATION_REQUIRED",
    "authority bypass refusal": "SB_ENGINE_API_UDR_AUTHORITY_BYPASS_REFUSED",
    "non-cpp engine refusal": "SB_ENGINE_API_UDR_NON_CPP_RUNTIME_FORBIDDEN",
    "runtime descriptor required": "SB_ENGINE_API_UDR_RUNTIME_DESCRIPTOR_REQUIRED",
    "descriptor mismatch": "SB_ENGINE_API_UDR_DESCRIPTOR_MISMATCH",
    "resource limit": "SB_ENGINE_API_UDR_RESOURCE_LIMIT_EXCEEDED",
    "MGA/SBLR/UUID/security boundary": "mga_sblr_uuid_security_transaction_preserved",
    "engine accepts revalidated SBLR only": "engine_accepts_revalidated_sblr_uuid_only",
}

UDR_LIFECYCLE_ROUTE_TOKENS = {
    "parser alter operation": (
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "extensibility.alter_udr_package",
    ),
    "parser drop operation": (
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "extensibility.drop_udr_package",
    ),
    "parser lifecycle payload": (
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "udr_package_lifecycle",
    ),
    "parser create surface": (
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "create_udr_package",
    ),
    "parser drop surface": (
        "project/src/parsers/sbsql_worker/lowering/lowering.cpp",
        "drop_udr_package",
    ),
    "engine alter opcode": (
        "project/src/engine/sblr/sblr_opcode_registry.cpp",
        "SBLR_EXTENSIBILITY_ALTER_UDR_PACKAGE",
    ),
    "engine drop opcode": (
        "project/src/engine/sblr/sblr_opcode_registry.cpp",
        "SBLR_EXTENSIBILITY_DROP_UDR_PACKAGE",
    ),
    "engine alter dispatch": (
        "project/src/engine/sblr/sblr_dispatch.cpp",
        "EngineAlterUdrPackage",
    ),
    "engine drop dispatch": (
        "project/src/engine/sblr/sblr_dispatch.cpp",
        "EngineDropUdrPackage",
    ),
    "server alter admission": (
        "project/src/server/sblr_admission.cpp",
        "extensibility.alter_udr_package",
    ),
    "server drop admission": (
        "project/src/server/sblr_admission.cpp",
        "extensibility.drop_udr_package",
    ),
    "server alter opcode": (
        "project/src/server/sblr_dispatch_server.cpp",
        "SBLR_EXTENSIBILITY_ALTER_UDR_PACKAGE",
    ),
    "server drop opcode": (
        "project/src/server/sblr_dispatch_server.cpp",
        "SBLR_EXTENSIBILITY_DROP_UDR_PACKAGE",
    ),
    "lifecycle conformance create": (
        "project/tests/sbsql_parser_worker/sbsql_udr_package_management_exact_route_conformance.cpp",
        "CREATE UDR PACKAGE",
    ),
    "lifecycle conformance alter": (
        "project/tests/sbsql_parser_worker/sbsql_udr_package_management_exact_route_conformance.cpp",
        "ALTER UDR PACKAGE",
    ),
    "lifecycle conformance drop": (
        "project/tests/sbsql_parser_worker/sbsql_udr_package_management_exact_route_conformance.cpp",
        "DROP UDR PACKAGE",
    ),
}

UDR_FUNCTION_CLASSIFICATION_TOKENS = {
    "standard function registry": (
        "project/src/engine/functions/registry/function_seed_registry.cpp",
        "BuildStandardFunctionSeedPackage",
    ),
    "registry closure validator": (
        "project/src/engine/functions/registry/function_registry.cpp",
        "ValidateFunctionRegistryForClosure",
    ),
    "function dispatcher": (
        "project/src/engine/functions/dispatch/function_dispatch.cpp",
        "DispatchFunctionCall",
    ),
    "missing handler diagnostic": (
        "project/src/engine/functions/dispatch/function_dispatch.cpp",
        "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING",
    ),
    "UDR classification gate source": (
        "project/tests/sbsql_parser_worker/udr_function_classification_dispatch_gate.cpp",
        "udr_function_classification_dispatch_gate=passed",
    ),
    "UDR classification gate closure check": (
        "project/tests/sbsql_parser_worker/udr_function_classification_dispatch_gate.cpp",
        "ValidateFunctionRegistryForClosure",
    ),
    "UDR classification CTest wiring": (
        "project/tests/sbsql_parser_worker/CMakeLists.txt",
        "udr_function_classification_dispatch_gate",
    ),
}

COMMON_PARSER_TOKENS = {
    "descriptor factory": "UdrPackageDescriptor",
    "trusted cpp": "trusted_cpp = true",
    "ABI": 'abi_version = "sb_udr_v1"',
    "binary hash": "binary_hash",
    "signature policy": "signature_policy",
    "capability role": "capability_role",
    "validate syntax entrypoint": "validate_syntax",
    "parse to sblr entrypoint": "parse_to_sblr",
    "normalize entrypoint": "normalize",
    "describe statement entrypoint": "describe_statement",
    "debug capabilities entrypoint": "debug_capabilities",
}

COMPATIBILITY_PARSER_TOKENS = {
    "installer entrypoint": "install_environment",
    "verify entrypoint": "verify_environment",
    "management inventory entrypoint": "management_operation_inventory",
    "management request entrypoint": "management_package_request",
    "trusted context guard": "RequireTrustedContext",
    "trusted engine context": "engine_context=trusted",
    "uuid resolver": "resolver=uuid",
}

BUILTIN_PACKAGE_TOKENS = {
    "catalog search key": "SB_UDR_BUILTIN_PACKAGE_CATALOG",
    "file-provider coverage": "csv_file_provider",
    "parquet coverage": "parquet_file_provider",
    "arrow coverage": "arrow_file_provider",
    "protobuf coverage": "protobuf_file_provider",
    "blob filter": "blob_filter",
    "remote engine": "remote_engine_connector",
    "emulated engine": "emulated_engine_support",
    "cluster fabric": "cluster_fabric",
    "side-effects outbox": "side_effects_outbox",
    "security definer": "security_definer_context",
    "vertical package": "financial_analytics",
    "non-cpp refusal": "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN",
    "external IO refusal": "UDR.FILE_PROVIDER.EXTERNAL_IO_POLICY_REQUIRED",
    "file-provider stream read": "UDR.FILE_PROVIDER.STREAM_READ",
    "file-provider schema mismatch": "UDR.FILE_PROVIDER.SCHEMA_MISMATCH",
    "file-provider budget": "UDR.FILE_PROVIDER.RESOURCE_BUDGET_EXCEEDED",
    "file-provider credential": "credential_ref",
    "file-provider path policy": "path_policy",
    "file-provider path root": "path_root",
    "file-provider source path": "source_path",
    "file-provider pushdown": "pushdown_applied",
    "cluster provider refusal": "UDR.CLUSTER.PROVIDER_REQUIRED",
    "side-effect refusal": "UDR.SIDE_EFFECTS.OUTBOX_REQUIRED",
    "side-effect precommit refusal": "UDR.SIDE_EFFECTS.PRECOMMIT_REFUSED",
    "side-effect outbox admitted": "UDR.SIDE_EFFECTS.OUTBOX_ADMITTED",
    "side-effect idempotent replay": "UDR.SIDE_EFFECTS.IDEMPOTENT_REPLAY",
    "side-effect durable intent": "outbox_durable_intent",
    "side-effect MGA evidence before success": "mga_evidence_before_success",
    "side-effect invisible external effect": "external_effect_visible",
    "side-effect idempotency key": "idempotency_key",
    "security context refusal": "UDR.SECURITY_DEFINER.CONTEXT_REQUIRED",
    "security definer admitted": "UDR.SECURITY_DEFINER.CONTEXT_ADMITTED",
    "security definer spoof refusal": "UDR.SECURITY_DEFINER.SPOOFED_PAYLOAD_CONTEXT",
    "security definer caller principal": "caller_principal_uuid",
    "security definer effective principal": "effective_principal_uuid",
    "security definer role proof": "role_chain_proof",
    "security definer group proof": "group_chain_proof",
    "security definer redaction": "redaction_enforced",
    "canonical function dispatch": "function_dispatch_catalog",
    "deployment manifest": "package_deployment_manifest",
    "deployment manifest API": "BuiltinUdrPackageDeploymentManifest",
    "deployment manifest JSON": "BuiltinUdrPackageDeploymentManifestJson",
    "public ABI freeze": "frozen_builtin_udr_sb_udr_v1",
    "install component": "lib/scratchbird/udr",
    "trusted cpp descriptor": "descriptor.trusted_cpp = true",
    "runtime ABI": 'descriptor.abi_version = "sb_udr_v1"',
    "runtime language": 'descriptor.runtime_language = "cpp"',
}

BUILTIN_PACKAGE_TEST_TOKENS = {
    "runtime registration": "RegisterPackage",
    "runtime load": "LoadPackage",
    "runtime invoke": "InvokePackage",
    "runtime unload": "UnloadPackage",
    "file-provider count": "file_provider_count == 8",
    "vertical family count": "vertical_like_count >= 32",
    "no external effects": "no_external_effects",
    "schema enforcement": "schema_enforced",
    "streaming proof": "streaming",
    "resource budget checked": "resource_budget_checked",
    "admitted source path": "source_path_admitted",
    "side-effect outbox proof": "outbox_durable_intent",
    "side-effect replay proof": "idempotent_replay",
    "security definer spoof proof": "UDR.SECURITY_DEFINER.SPOOFED_PAYLOAD_CONTEXT",
    "security definer context proof": "payload_context_ignored",
    "deployment manifest proof": "VerifyDeploymentManifest",
    "deployment manifest entrypoints": "entrypoints_csv",
    "deployment manifest install component": "install_component",
    "deployment manifest ABI freeze": "frozen_builtin_udr_sb_udr_v1",
    "missing SBLR authority refusal": "missing_sblr_authority",
    "direct SQL entrypoint refusal": "execute_sql_text_directly",
    "path root escape refusal": "source_path_outside_admitted_path_root",
    "cluster provider absence refusal": "cluster_without_provider",
    "MGA authority proof": "engine_mga_authority",
}


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def require_contains(errors: list[str], path: Path, label: str, text: str, token: str) -> None:
    if token not in text:
        fail(errors, f"{path}: missing {label}: {token}")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def parser_family_from_dir(directory: Path) -> str:
    name = directory.name
    return name.removeprefix("sbu_").removesuffix("_parser_support")


def expected_cmake_option(family: str) -> str:
    return "SB_BUILD_SBU_" + family.upper() + "_PARSER_SUPPORT"


def validate_runtime(repo_root: Path, errors: list[str]) -> None:
    header = repo_root / "project/src/udr/runtime/sb_udr_runtime.hpp"
    impl = repo_root / "project/src/udr/runtime/sb_udr_runtime.cpp"
    api = repo_root / "project/src/engine/internal_api/extensibility/udr_api.cpp"
    support = repo_root / "project/src/engine/internal_api/extensibility/extensibility_support.hpp"

    header_text = read(header)
    impl_text = read(impl)
    api_text = read(api)
    support_text = read(support)

    for label, token in REQUIRED_RUNTIME_TOKENS.items():
        require_contains(errors, header, label, header_text, token)
    for label, token in REQUIRED_RUNTIME_IMPL_TOKENS.items():
        require_contains(errors, impl, label, impl_text, token)
    for label, token in REQUIRED_ENGINE_API_TOKENS.items():
        require_contains(errors, api, label, api_text, token)
    for token in ("cluster_deploy", "cluster_provider_deploy", "global_deploy"):
        require_contains(errors, support, "cluster authority trigger", support_text, token)


def validate_parser_packages(repo_root: Path, errors: list[str]) -> set[str]:
    udr_root = repo_root / "project/src/udr"
    cmake = read(repo_root / "project/CMakeLists.txt")
    parser_dirs = sorted(udr_root.glob("sbu_*_parser_support"))
    families: set[str] = set()
    if len(parser_dirs) < 20:
        fail(errors, f"{udr_root}: expected at least 20 parser-support UDR packages, found {len(parser_dirs)}")

    for directory in parser_dirs:
        family = parser_family_from_dir(directory)
        families.add(family)
        source = directory / f"{directory.name}.cpp"
        header = directory / f"{directory.name}.hpp"
        cmake_file = directory / "CMakeLists.txt"
        for required in (source, header, cmake_file):
            if not required.exists():
                fail(errors, f"{directory}: missing {required.name}")
        if not source.exists():
            continue

        source_text = read(source)
        for label, token in COMMON_PARSER_TOKENS.items():
            require_contains(errors, source, label, source_text, token)
        if family != "sbsql":
            for label, token in COMPATIBILITY_PARSER_TOKENS.items():
                require_contains(errors, source, label, source_text, token)

        option = expected_cmake_option(family)
        if option not in cmake:
            fail(errors, f"project/CMakeLists.txt: missing build option {option} for {directory.name}")

    return families


def validate_compatibility_manifest(repo_root: Path, parser_families: set[str], errors: list[str]) -> None:
    manifest = repo_root / "project/src/udr/packages/compatibility/CompatibilityUdrBridgePolicyManifest.csv"
    rows = list(csv.DictReader(manifest.open(newline="", encoding="utf-8")))
    if len(rows) < 28:
        fail(errors, f"{manifest}: expected at least 28 compatibility/capability rows, found {len(rows)}")

    compatibility_rows = [row for row in rows if row["profile_class"] == "compatibility_emulation"]
    capability_reference_rows = [row for row in rows if row["profile_class"] == "capability_reference"]
    if len(compatibility_rows) < 25:
        fail(errors, f"{manifest}: expected at least 25 compatibility emulation rows")
    if len(capability_reference_rows) < 3:
        fail(errors, f"{manifest}: expected sqlserver/oracle/db2 capability reference rows")

    for row in rows:
        family = row["family_id"]
        if row["transaction_authority"] != "engine_mga":
            fail(errors, f"{manifest}: {family} must preserve engine_mga transaction authority")
        if row["security_authority"] != "engine_security":
            fail(errors, f"{manifest}: {family} must preserve engine_security authority")
        forbidden = row["forbidden_authorities"]
        for token in (
            "compatibility_storage_authority",
            "compatibility_recovery_authority",
            "compatibility_transaction_authority",
            "parser_transaction_authority",
            "wal_recovery_authority",
        ):
            if token not in forbidden:
                fail(errors, f"{manifest}: {family} forbidden authority list missing {token}")

    for row in compatibility_rows:
        family = row["family_id"]
        if family not in parser_families:
            fail(errors, f"{manifest}: compatibility family {family} has no parser-support UDR package")
        if "UDR_BRIDGE_UNAVAILABLE" not in row["diagnostic_set"]:
            fail(errors, f"{manifest}: {family} missing bridge-unavailable diagnostic")
        if "SANDBOX_FORBIDDEN_BY_POLICY" not in row["diagnostic_set"]:
            fail(errors, f"{manifest}: {family} missing sandbox-forbidden diagnostic")

    expected_capability_refs = {"sqlserver", "oracle", "db2"}
    actual_capability_refs = {row["family_id"] for row in capability_reference_rows}
    missing_refs = expected_capability_refs - actual_capability_refs
    if missing_refs:
        fail(errors, f"{manifest}: missing capability reference rows {sorted(missing_refs)}")
    for row in capability_reference_rows:
        family = row["family_id"]
        if row["bridge_mode"] != "outbound_cpp_connectivity_udr_only":
            fail(errors, f"{manifest}: {family} capability reference must remain outbound connector only")
        if "CONNECTIVITY_UDR_MISSING" not in row["diagnostic_set"]:
            fail(errors, f"{manifest}: {family} missing connectivity UDR diagnostic")
        if "EMULATION_FORBIDDEN" not in row["diagnostic_set"]:
            fail(errors, f"{manifest}: {family} missing emulation-forbidden diagnostic")


def validate_builtin_package_catalog(repo_root: Path, errors: list[str]) -> None:
    header = repo_root / "project/src/udr/packages/builtin/sb_udr_builtin_packages.hpp"
    impl = repo_root / "project/src/udr/packages/builtin/sb_udr_builtin_packages.cpp"
    cmake = repo_root / "project/src/udr/packages/builtin/CMakeLists.txt"
    test = repo_root / "project/tests/sbsql_parser_worker/udr_builtin_package_catalog_conformance.cpp"
    root_cmake = repo_root / "project/CMakeLists.txt"
    test_cmake = repo_root / "project/tests/sbsql_parser_worker/CMakeLists.txt"

    for path in (header, impl, cmake, test):
        if not path.exists():
            fail(errors, f"{path}: missing builtin UDR package catalog artifact")

    if not impl.exists() or not test.exists():
        return

    impl_text = read(impl)
    test_text = read(test)
    root_cmake_text = read(root_cmake)
    test_cmake_text = read(test_cmake)
    for label, token in BUILTIN_PACKAGE_TOKENS.items():
        require_contains(errors, impl, label, impl_text, token)
    for label, token in BUILTIN_PACKAGE_TEST_TOKENS.items():
        require_contains(errors, test, label, test_text, token)
    if "SB_BUILD_UDR_BUILTIN_PACKAGES" not in root_cmake_text:
        fail(errors, f"{root_cmake}: missing SB_BUILD_UDR_BUILTIN_PACKAGES option")
    if "add_subdirectory(src/udr/packages/builtin)" not in root_cmake_text:
        fail(errors, f"{root_cmake}: missing builtin UDR package catalog build wiring")
    if "udr_builtin_package_catalog_conformance" not in test_cmake_text:
        fail(errors, f"{test_cmake}: missing builtin UDR package catalog CTest wiring")


def validate_udr_lifecycle_routes(repo_root: Path, errors: list[str]) -> None:
    for label, (relative_path, token) in UDR_LIFECYCLE_ROUTE_TOKENS.items():
        path = repo_root / relative_path
        if not path.exists():
            fail(errors, f"{path}: missing UDR lifecycle route artifact")
            continue
        require_contains(errors, path, label, read(path), token)


def validate_udr_function_classification(repo_root: Path, errors: list[str]) -> None:
    for label, (relative_path, token) in UDR_FUNCTION_CLASSIFICATION_TOKENS.items():
        path = repo_root / relative_path
        if not path.exists():
            fail(errors, f"{path}: missing UDR function classification artifact")
            continue
        require_contains(errors, path, label, read(path), token)


def validate_public_naming(repo_root: Path, errors: list[str]) -> None:
    udr_root = repo_root / "project/src/udr"
    for path in udr_root.rglob("*"):
        relative = path.relative_to(repo_root)
        legacy_reference_pattern = r"(^|/)do" r"nor($|[_./-])"
        if re.search(legacy_reference_pattern, str(relative), flags=re.IGNORECASE):
            fail(
                errors,
                f"{relative}: public UDR path must use compatibility/reference naming, "
                "not legacy reference terminology",
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    errors: list[str] = []
    validate_runtime(repo_root, errors)
    parser_families = validate_parser_packages(repo_root, errors)
    validate_compatibility_manifest(repo_root, parser_families, errors)
    validate_builtin_package_catalog(repo_root, errors)
    validate_udr_lifecycle_routes(repo_root, errors)
    validate_udr_function_classification(repo_root, errors)
    validate_public_naming(repo_root, errors)

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    compatibility_count = len(parser_families - {"sbsql"})
    print(
        "UDR implementation coverage gate passed: "
        f"{len(parser_families)} parser-support packages, "
        f"{compatibility_count} compatibility parser packages, "
        "runtime/API authority checks present."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
