#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the public cluster provider build-mode matrix for PCR-103."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_CLUSTER_BUILD_MATRIX_GATE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)


@dataclass(frozen=True)
class BuildModeRow:
    mode_id: str
    provider_mode: str
    cmake_options: tuple[str, ...]
    expected_claims: tuple[str, ...]
    required_controls: tuple[str, ...]


REQUIRED_MATRIX = (
    BuildModeRow(
        "PCR103_NO_CLUSTER_DEFAULT",
        "no_cluster",
        (
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_CLUSTER_PROVIDER_STUB=OFF",
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=",
        ),
        (
            "provider_type=no_cluster",
            "supports_execution=false",
            "supports_route_admission=false",
            "route_admission_allowed=false",
            "diagnostic=SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        ),
        (
            "src/cluster_provider/no_cluster_provider.cpp",
            "SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER=1",
        ),
    ),
    BuildModeRow(
        "PCR103_COMPILE_LINK_STUB",
        "compile_link_stub",
        (
            "SB_ENABLE_CLUSTER_PROVIDER=ON",
            "SB_CLUSTER_PROVIDER_STUB=ON",
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=",
        ),
        (
            "provider_type=compile_link_stub",
            "compile_link_only=true",
            "supports_execution=false",
            "supports_route_admission=false",
            "route_admission_allowed=false",
            "diagnostic=SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
        ),
        (
            "src/cluster_provider_stub/stub_cluster_provider.cpp",
            "SCRATCHBIRD_CLUSTER_PROVIDER_STUB=1",
        ),
    ),
    BuildModeRow(
        "PCR103_EXTERNAL_PROVIDER_ONLY_EXECUTABLE_MODE",
        "external_cluster_provider",
        (
            "SB_ENABLE_CLUSTER_PROVIDER=ON",
            "SB_CLUSTER_PROVIDER_STUB=OFF",
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=/path/to/libsb_cluster_provider",
        ),
        (
            "provider_type=external_cluster_provider",
            "external_provider=true",
            "supports_execution=true",
            "supports_route_admission=true",
            "route_admission_allowed=true_after_handshake",
            "diagnostic=SBLR.CLUSTER.HANDSHAKE.ACCEPTED",
        ),
        (
            "add_library(sb_cluster_provider UNKNOWN IMPORTED GLOBAL)",
            "SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL=1",
            "IMPORTED_LOCATION",
        ),
    ),
    BuildModeRow(
        "PCR103_INVALID_STUB_WITHOUT_ENABLE",
        "invalid_configuration",
        (
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_CLUSTER_PROVIDER_STUB=ON",
        ),
        (
            "configure_refuses=SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        ),
        (
            "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        ),
    ),
    BuildModeRow(
        "PCR103_INVALID_EXTERNAL_WITHOUT_ENABLE",
        "invalid_configuration",
        (
            "SB_ENABLE_CLUSTER_PROVIDER=OFF",
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=/path/to/libsb_cluster_provider",
        ),
        (
            "configure_refuses=SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        ),
        (
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        ),
    ),
    BuildModeRow(
        "PCR103_INVALID_STUB_AND_EXTERNAL",
        "invalid_configuration",
        (
            "SB_ENABLE_CLUSTER_PROVIDER=ON",
            "SB_CLUSTER_PROVIDER_STUB=ON",
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY=/path/to/libsb_cluster_provider",
        ),
        (
            "configure_refuses=Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB, not both",
        ),
        (
            "Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB, not both",
        ),
    ),
)


def fail(message: str) -> None:
    print(f"public_cluster_build_matrix_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_file(path: Path, project_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, project_root)}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def reject_contains(text: str, token: str, context: str) -> None:
    if token in text:
        fail(f"{context}_forbidden:{token}")


def function_body(text: str, name: str) -> str:
    start = text.find(name)
    if start < 0:
        fail(f"function_missing:{name}")
    open_brace = text.find("{", start)
    if open_brace < 0:
        fail(f"function_body_missing:{name}")
    depth = 0
    for index in range(open_brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace : index + 1]
    fail(f"function_body_unclosed:{name}")


def check_matrix_rows() -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    seen_modes = {row.provider_mode for row in REQUIRED_MATRIX}
    for required_mode in ("no_cluster", "compile_link_stub",
                          "external_cluster_provider", "invalid_configuration"):
        if required_mode not in seen_modes:
            fail(f"build_matrix_mode_missing:{required_mode}")
    for row in REQUIRED_MATRIX:
        for value in row.cmake_options + row.expected_claims + row.required_controls:
            reject_private_reference(value, row.mode_id)
        records.append(
            {
                "mode_id": row.mode_id,
                "provider_mode": row.provider_mode,
                "cmake_options": row.cmake_options,
                "expected_claims": row.expected_claims,
                "required_controls": row.required_controls,
            }
        )
    return records


def check_project_cmake(project_root: Path) -> list[dict[str, Any]]:
    project_cmake = require_file(project_root / "CMakeLists.txt", project_root)
    for token in (
        "option(SB_ENABLE_CLUSTER_PROVIDER",
        "option(SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR",
        "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB, not both",
    ):
        require_contains(project_cmake, token, "project_cmake_cluster_matrix")

    records: list[dict[str, Any]] = [
        {
            "file": "CMakeLists.txt",
            "status": "top_level_cluster_options_and_invalid_combinations_declared",
            "sha256": sha256_text(project_cmake),
        }
    ]

    for relative in (
        "src/engine/internal_api/CMakeLists.txt",
        "src/engine/sblr/CMakeLists.txt",
    ):
        text = require_file(project_root / relative, project_root)
        for token in (
            "SB_ENABLE_CLUSTER_PROVIDER AND SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
            "add_library(sb_cluster_provider UNKNOWN IMPORTED GLOBAL)",
            "IMPORTED_LOCATION",
            "SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL=1",
            "SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR",
            "SB_CLUSTER_PROVIDER_STUB",
            "src/cluster_provider_stub",
            "src/cluster_provider",
            "requires SB_CLUSTER_PROVIDER_STUB=ON or SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        ):
            require_contains(text, token, f"{relative}:provider_selection")
        records.append(
            {
                "file": relative,
                "status": "no_cluster_stub_external_selection_declared",
                "sha256": sha256_text(text),
            }
        )

    for relative, definition in (
        ("src/cluster_provider/CMakeLists.txt",
         "SCRATCHBIRD_CLUSTER_PROVIDER_NO_CLUSTER=1"),
        ("src/cluster_provider_stub/CMakeLists.txt",
         "SCRATCHBIRD_CLUSTER_PROVIDER_STUB=1"),
    ):
        text = require_file(project_root / relative, project_root)
        for token in ("add_library(sb_cluster_provider", definition):
            require_contains(text, token, f"{relative}:provider_target")
        records.append(
            {
                "file": relative,
                "status": "provider_target_mode_definition_declared",
                "sha256": sha256_text(text),
            }
        )
    return records


def check_handshake_contract(project_root: Path) -> dict[str, Any]:
    header = require_file(
        project_root / "src" / "cluster_provider" / "cluster_provider.hpp",
        project_root,
    )
    for token in (
        "CLUSTER_PROVIDER_ABI_HANDSHAKE",
        "kClusterProviderAbiVersionCurrent",
        "kClusterProviderCatalogManifestVersionCurrent",
        "kClusterProviderCatalogRecordCodecVersionCurrent",
        "kClusterProviderCatalogCompatibilityDigest",
        "external_provider",
        "compile_link_only",
        "supports_route_admission",
        "local_runtime_execution_enabled",
        "mutable_by_local_core",
        "RequiredClusterProviderOperationSet",
        "RequiredClusterProviderFeatureFlags",
        "RequiredClusterProviderAuthorityDomains",
        "ValidateClusterProviderHandshake",
        "EvaluateClusterProviderRouteAdmission",
    ):
        require_contains(header, token, "cluster_provider_handshake_contract")

    handshake = function_body(header, "ValidateClusterProviderHandshake")
    for token in (
        "if (info.compile_link_only)",
        "if (!info.external_provider)",
        "if (!info.supports_execution || !info.supports_route_admission)",
        "if (info.local_runtime_execution_enabled)",
        "if (info.mutable_by_local_core)",
        "kClusterHandshakeExternalProviderRequiredCode",
        "kClusterHandshakeStubCompileLinkOnlyCode",
        "kClusterHandshakeAcceptedCode",
        "result.route_admission_allowed = true",
    ):
        require_contains(token=token, text=handshake,
                         context="cluster_provider_handshake_contract")

    route_admission = function_body(header, "EvaluateClusterProviderRouteAdmission")
    for token in (
        "ValidateClusterProviderHandshake(info)",
        "if (!handshake.ok)",
        "ContainsProviderToken(info.operation_ids, operation_id)",
        "kClusterRouteAdmissionUnsupportedOperationCode",
        "result.route_admitted = true",
    ):
        require_contains(token=token, text=route_admission,
                         context="cluster_provider_route_admission_contract")
    return {
        "file": "src/cluster_provider/cluster_provider.hpp",
        "status": "external_provider_only_route_admission_contract_declared",
        "sha256": sha256_text(header),
    }


def check_provider_sources(project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    providers = (
        {
            "relative": "src/cluster_provider/no_cluster_provider.cpp",
            "provider_name": "scratchbird.cluster.no_cluster_provider",
            "provider_type": "\"no_cluster\"",
            "support_status": "\"not_enabled\"",
            "compile_link": "info.compile_link_only = false;",
            "unsupported_feature": "cluster.provider",
            "diagnostic": "kClusterSupportNotEnabledCode",
            "status": "no_cluster_fail_closed_non_mutating",
        },
        {
            "relative": "src/cluster_provider_stub/stub_cluster_provider.cpp",
            "provider_name": "scratchbird.cluster.compile_link_stub_provider",
            "provider_type": "\"compile_link_stub\"",
            "support_status": "\"compile_link_only\"",
            "compile_link": "info.compile_link_only = true;",
            "unsupported_feature": "cluster.provider.stub",
            "diagnostic": "kClusterHandshakeStubCompileLinkOnlyCode",
            "status": "compile_link_stub_fail_closed_non_mutating",
        },
    )
    for provider in providers:
        text = require_file(project_root / provider["relative"], project_root)
        execute = function_body(text, "ExecuteClusterOperation")
        for token in (
            provider["provider_name"],
            provider["provider_type"],
            provider["support_status"],
            "info.external_provider = false;",
            provider["compile_link"],
            "info.supports_execution = false;",
            "info.supports_route_admission = false;",
            "info.local_runtime_execution_enabled = false;",
            "info.mutable_by_local_core = false;",
            "ValidateClusterProviderHandshake(info)",
            "return DescribeClusterProvider().provider_type;",
            "cluster_provider_handshake",
            "cluster_provider_route_admission",
        ):
            require_contains(text, token, provider["relative"])
        for token in (
            "EvaluateClusterProviderRouteAdmission(info, request.envelope.operation_id)",
            "result.ok = false",
            "result.cluster_authority_required = true",
            provider["unsupported_feature"],
            provider["diagnostic"],
            "cluster_provider_route_admission_diagnostic",
        ):
            require_contains(execute, token, provider["relative"])
        for forbidden in (
            "result.ok = true",
            "result.result_shape.rows.push_back",
            "supports_execution = true",
            "supports_route_admission = true",
            "external_provider = true",
            "mutable_by_local_core = true",
        ):
            reject_contains(execute, forbidden, provider["relative"])
        records.append(
            {
                "file": provider["relative"],
                "status": provider["status"],
                "sha256": sha256_text(text),
            }
        )
    return records


def check_internal_api_cluster_route_boundary(project_root: Path) -> list[dict[str, Any]]:
    helper = require_file(
        project_root / "src" / "engine" / "internal_api" / "cluster" /
        "cluster_provider_boundary.hpp",
        project_root,
    )
    for token in (
        "SB_ENGINE_INTERNAL_API_CLUSTER_PROVIDER_BOUNDARY_HELPER",
        "MakeInternalClusterProviderRequest",
        "ExecuteInternalClusterProviderBoundary",
        "CopyClusterProviderBoundaryProof",
        "RefuseIfClusterProviderBoundaryClosed",
        "cluster_provider_boundary",
        "provider_invoked",
        "cluster_provider_boundary_result",
        "cluster_provider_boundary_api_operation",
    ):
        require_contains(helper, token, "internal_api_cluster_provider_boundary_helper")

    route_sources = (
        ("src/engine/internal_api/cluster/cluster_control_api.cpp",
         ("cluster.control_cluster",)),
        ("src/engine/internal_api/cluster/cluster_inspect_api.cpp",
         ("cluster.inspect_state", "cluster.inspect_routing_plan")),
        ("src/engine/internal_api/cluster/placement_api.cpp",
         ("cluster.place_object",)),
        ("src/engine/internal_api/cluster/cluster_insert_route_api.cpp",
         ("cluster.validate_insert_route_fence",)),
        ("src/engine/internal_api/cluster/remote_participant_insert_api.cpp",
         ("cluster.prepare_remote_participant_insert",)),
        ("src/engine/internal_api/cluster/replication_api.cpp",
         ("cluster.inspect_replication", "replication.evaluate_boundary")),
    )
    records: list[dict[str, Any]] = [
        {
            "file": "src/engine/internal_api/cluster/cluster_provider_boundary.hpp",
            "status": "typed_cluster_api_provider_boundary_helper_declared",
            "sha256": sha256_text(helper),
        }
    ]
    for relative, operations in route_sources:
        text = require_file(project_root / relative, project_root)
        for token in (
            '#include "cluster/cluster_provider_boundary.hpp"',
            "ExecuteInternalClusterProviderBoundary",
            "RefuseIfClusterProviderBoundaryClosed",
        ):
            require_contains(text, token, f"{relative}:provider_boundary_route")
        for operation in operations:
            require_contains(text, operation, f"{relative}:provider_operation")
        records.append(
            {
                "file": relative,
                "status": "typed_cluster_api_routes_through_provider_boundary",
                "operations": operations,
                "sha256": sha256_text(text),
            }
        )
    return records


def check_agent_cluster_boundary_proofs(project_root: Path) -> list[dict[str, Any]]:
    proof_files: tuple[tuple[str, tuple[str, ...]], ...] = (
        (
            "src/core/agents/agent_cluster_boundary.hpp",
            (
                "kAgentClusterSupportNotEnabledCode",
                "kAgentClusterExternalProviderRequiredCode",
            ),
        ),
        (
            "src/core/agents/agent_cluster_boundary.cpp",
            (
                'result.provider_type == "external_cluster_provider"',
                'result.provider_support_status == "enabled"',
                "EnforceExternalProviderForProductionLive",
                "agent_cluster_external_provider_required",
            ),
        ),
        (
            "tests/agents/agent_cluster_provider_build_matrix_gate.cpp",
            (
                "scratchbird.cluster.compile_link_stub_provider",
                "compile_link_stub",
                "compile_link_only",
                "!info.supports_execution",
                "!info.supports_route_admission",
                "kClusterHandshakeStubCompileLinkOnlyCode",
                "!result.ok",
                "compile-link stub provider accepted cluster operation",
            ),
        ),
        (
            "tests/agents/agent_cluster_provider_boundary_gate.cpp",
            (
                "RequireCompileLinkStubVector",
                "scratchbird.cluster.compile_link_stub_provider",
                "compile_link_stub",
                "compile_link_only",
                "kClusterHandshakeStubCompileLinkOnlyCode",
                "cluster.provider.stub",
                "compile-link stub accepted cluster operation",
            ),
        ),
        (
            "tests/agents/agent_cluster_leadership_boundary_gate.cpp",
            (
                "TestCompileLinkStubLeaseSurfacesFailClosed",
                "compile_link_stub",
                "kClusterHandshakeStubCompileLinkOnlyCode",
                "compile-link stub lease surface unexpectedly succeeded",
                "state.state == agents::AgentClusterLeadershipState::follower",
            ),
        ),
        (
            "tests/agents/agent_cluster_route_api_provider_gate.cpp",
            (
                'provider.provider_type == std::string_view("compile_link_stub")',
                "kClusterHandshakeStubCompileLinkOnlyCode",
                "compile_link_only",
                "compile-link stub accepted cluster.sys.agents",
                "cluster_provider_type",
            ),
        ),
        (
            "tests/agents/agent_management_authorization_policy_gate.cpp",
            (
                'provider.provider_type == std::string_view("compile_link_stub")',
                "kClusterHandshakeStubCompileLinkOnlyCode",
                "compile-link stub provider accepted route",
            ),
        ),
        (
            "tests/agents/pfar_standalone_test_coverage_gate.py",
            (
                "kClusterHandshakeStubCompileLinkOnlyCode",
            ),
        ),
    )
    records: list[dict[str, Any]] = []
    stale_tokens = (
        "scratchbird.cluster.dummy_provider",
        '"dummy"',
        "stub_response",
        "SBLR.CLUSTER.STUB_RESPONSE",
        "kAgentClusterStubResponseCode",
    )
    for relative, required_tokens in proof_files:
        text = require_file(project_root / relative, project_root)
        for token in required_tokens:
            require_contains(text, token, f"{relative}:agent_cluster_stub_boundary")
        if relative.endswith((".cpp", ".hpp")):
            for token in stale_tokens:
                reject_contains(text, token,
                                f"{relative}:agent_cluster_stub_boundary")
        records.append(
            {
                "file": relative,
                "status": "agent_cluster_compile_link_stub_fail_closed_proof",
                "sha256": sha256_text(text),
            }
        )
    return records


def check_release_gate_registration(project_root: Path) -> dict[str, Any]:
    text = require_file(project_root / "tests" / "release" / "CMakeLists.txt",
                        project_root)
    for token in (
        "public_cluster_provider_boundary_cleanup_gate",
        "public_cluster_provider_handshake_gate",
        "public_cluster_build_matrix_gate",
        "public_cluster_build_matrix_gate.py",
        "PCR-GATE-103",
        "PCR-103",
        "no_cluster",
        "compile_link_stub",
        "external_provider_mode",
    ):
        require_contains(text, token, "release_ctest_cluster_build_matrix")
    return {
        "file": "tests/release/CMakeLists.txt",
        "status": "public_cluster_build_matrix_gate_registered",
        "sha256": sha256_text(text),
    }


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output = args.output.resolve()
    if not project_root.is_dir() or project_root.name != "project":
        fail("project_root_must_be_project_directory")
    if not build_root.is_dir():
        fail("build_root_missing")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-103",
        "public_tree_inputs_only": True,
        "private_docs_required": False,
        "git_history_required": False,
        "matrix": check_matrix_rows(),
        "cmake_controls": check_project_cmake(project_root),
        "handshake_contract": check_handshake_contract(project_root),
        "provider_sources": check_provider_sources(project_root),
        "typed_internal_api_cluster_routes": check_internal_api_cluster_route_boundary(
            project_root),
        "agent_cluster_boundary_proofs": check_agent_cluster_boundary_proofs(
            project_root),
        "release_gate_registration": check_release_gate_registration(project_root),
    }
    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    evidence["evidence_sha256"] = sha256_text(encoded)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"public_cluster_build_matrix_gate_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print(f"public_cluster_build_matrix_gate_sha256={evidence['evidence_sha256']}")
    print("public_cluster_build_matrix_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
