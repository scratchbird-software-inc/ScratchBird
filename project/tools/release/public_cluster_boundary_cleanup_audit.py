#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Audit public cluster provider boundary cleanup for PCR-097."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any


# CLUSTER_CALL_CLEANUP_AUDIT

SOURCE_SUFFIXES = {
    ".cpp",
    ".hpp",
    ".h",
    ".inc",
    ".def",
    ".yaml",
    ".yml",
    ".cmake",
    ".txt",
}

CLUSTER_NEEDLES = (
    "cluster_provider",
    "cluster_authority",
    "external_cluster_provider",
    "external_provider",
    "no_cluster",
    "cluster_route",
    "cluster_scope",
    "cluster_metric",
)

PUBLIC_CLUSTER_BOUNDARY_FILES = (
    "src/cluster_provider/cluster_provider.hpp",
    "src/cluster_provider/no_cluster_provider.cpp",
    "src/cluster_provider_stub/stub_cluster_provider.cpp",
)

EXPECTED_NORMALIZED_BOUNDARY_TOKENS = (
    "struct ClusterProviderCommandBoundary",
    "std::string_view normalized_command",
    "std::string_view provider_operation_id",
    "bool provider_routed = false",
    "RequiredClusterProviderCommandBoundarySet",
    "RequiredClusterProviderOperationSet",
    "RequiredClusterProviderPreAdmissionRefusalSet",
)

EXPECTED_PRE_ADMISSION_REFUSALS = (
    "cluster.security.refuse_local_cluster_mutation",
    "cluster.admin.refuse_reference_shell_control",
    "cluster.query.refuse_local_query_as_cluster_authority",
)

EXPECTED_COMPATIBILITY_ALIASES = (
    "cluster.inspect_state",
    "cluster.inspect_routing_plan",
    "cluster.place_object",
    "cluster.inspect_replication",
    "cluster.prepare_remote_participant_insert",
    "cluster.validate_insert_route_fence",
    "cluster.inspect_provider",
)

FORBIDDEN_PUBLIC_PRIVATE_BEHAVIOR_PATTERNS = (
    re.compile(r"\bimplements?\s+(?:production\s+)?cluster\s+(?:membership|routing|replication|failover|recovery|distributed\s+transaction|distributed\s+query)", re.I),
    re.compile(r"\bprovides?\s+(?:production\s+)?cluster\s+(?:membership|routing|replication|failover|recovery|distributed\s+transaction|distributed\s+query)", re.I),
    re.compile(r"\bexecutes?\s+(?:production\s+)?cluster\s+(?:membership|routing|replication|failover|recovery|distributed\s+transaction|distributed\s+query)", re.I),
    re.compile(r"\bcluster\s+(?:membership|routing\s+authority|replication|failover|recovery|distributed\s+transaction|distributed\s+query)\s+(?:execution|implementation|engine|runtime)", re.I),
    re.compile(r"\bprivate\s+cluster\s+provider\s+(?:implementation|behavior|runtime|engine)", re.I),
    re.compile(r"\bprivate_provider\s+(?:implementation|behavior|runtime|engine)", re.I),
)

NEGATIVE_OR_BOUNDARY_CONTEXT_MARKERS = (
    "contains no",
    "provides no",
    "no cluster",
    "no public/private",
    "no public or private",
    "not enabled",
    "not supported",
    "unsupported",
    "unlicensed",
    "fail-closed",
    "failed_closed",
    "compile-link",
    "compile_link",
    "external provider only",
    "external provider target",
    "external provider reports",
    "external sb_cluster_provider",
    "external_cluster_provider",
    "private provider required",
    "replaced by the",
)

ALLOWED_EXECUTE_CALLS = {
    "src/core/agents/agent_cluster_boundary.cpp": (
        "requires_cluster_authority = true",
        "contains_sql_text = false",
        "agent_cluster_boundary",
    ),
    "src/engine/internal_api/agents/agent_management_api.cpp": (
        "requires_cluster_authority = true",
        "contains_sql_text = false",
        "agent_cluster_api_route",
    ),
    "src/engine/internal_api/observability/agent_evidence_retention_api.cpp": (
        "requires_cluster_authority = true",
        "contains_sql_text = false",
        "agent_evidence_cluster_route",
    ),
    "src/engine/internal_api/cluster/cluster_provider_boundary.hpp": (
        "MakeInternalClusterProviderRequest",
        "requires_cluster_authority = true",
        "contains_sql_text = false",
        "cluster_provider_boundary",
    ),
    "src/engine/sblr/sblr_dispatch.cpp": (
        "ValidateSblrEnvelope",
        "PropagateClusterApiDiagnostics",
        "SBLR.CLUSTER.",
    ),
}

SURFACE_ROOTS = {
    "api": (
        "src/engine/internal_api/cluster",
        "src/engine/internal_api/transaction",
        "src/engine/internal_api/observability",
    ),
    "agent": (
        "src/core/agents",
        "src/engine/internal_api/agents",
    ),
    "metric": (
        "src/core/metrics",
        "src/engine/internal_api/observability",
    ),
    "optimizer": (
        "src/engine/optimizer",
        "src/core/index",
    ),
    "sblr_route": (
        "src/engine/sblr",
        "src/parsers/sbsql_worker/lowering",
    ),
    "provider": (
        "src/cluster_provider",
        "src/cluster_provider_stub",
    ),
}


def fail(message: str) -> None:
    print(f"public_cluster_boundary_cleanup_audit=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def require_file(path: Path, repo_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, repo_root)}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    private_fragments = (
        "docs" + "/" + "execution-plans",
        "docs" + "/" + "completed-execution-plans",
        "docs" + "/" + "findings",
        "." + "git",
        "/" + "home" + "/" + "dcalford",
        "ScratchBird" + "-Private",
    )
    for fragment in private_fragments:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


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


def boundary_entries(text: str) -> list[dict[str, Any]]:
    body = function_body(text, "RequiredClusterProviderCommandBoundarySet")
    entries = [
        {
            "normalized_command": match.group(1),
            "provider_operation_id": match.group(2),
            "provider_routed": match.group(3) == "true",
        }
        for match in re.finditer(
            r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*\}',
            body,
            re.S,
        )
    ]
    if not entries:
        fail("cluster_provider_command_boundary_entries_missing")
    return entries


def derived_operation_ids(entries: list[dict[str, Any]]) -> list[str]:
    operation_ids: list[str] = []
    for entry in entries:
        if not entry["provider_routed"]:
            continue
        for token in (entry["normalized_command"], entry["provider_operation_id"]):
            if token == "exact_refusal_no_provider_call":
                continue
            if token not in operation_ids:
                operation_ids.append(token)
    return operation_ids


def allowed_negative_or_boundary_context(line: str) -> bool:
    lowered = line.lower()
    return any(marker in lowered for marker in NEGATIVE_OR_BOUNDARY_CONTEXT_MARKERS)


def iter_project_files(project_root: Path):
    skip_dirs = {"__pycache__", ".pytest_cache", "build", "node_modules", "vendor"}
    for dirpath, dirnames, filenames in os.walk(project_root):
        dirnames[:] = [name for name in dirnames if name not in skip_dirs]
        for filename in filenames:
            path = Path(dirpath) / filename
            if path.suffix in SOURCE_SUFFIXES:
                yield path


def check_normalized_cluster_provider_boundary(repo_root: Path,
                                               project_root: Path) -> dict[str, Any]:
    header = require_file(project_root / "src" / "cluster_provider" / "cluster_provider.hpp",
                          repo_root)
    for token in EXPECTED_NORMALIZED_BOUNDARY_TOKENS:
        require_contains(header, token, "normalized_cluster_provider_boundary")

    entries = boundary_entries(header)
    normalized_commands = [entry["normalized_command"] for entry in entries]
    if len(entries) != 59:
        fail(f"normalized_boundary_count_mismatch:{len(entries)}")
    if len(set(normalized_commands)) != len(normalized_commands):
        fail("normalized_boundary_duplicate_normalized_command")

    routed = [entry for entry in entries if entry["provider_routed"]]
    refusals = [entry for entry in entries if not entry["provider_routed"]]
    if len(routed) != 56:
        fail(f"normalized_boundary_routed_count_mismatch:{len(routed)}")
    if len(refusals) != 3:
        fail(f"normalized_boundary_refusal_count_mismatch:{len(refusals)}")

    refusal_set = {entry["normalized_command"] for entry in refusals}
    if refusal_set != set(EXPECTED_PRE_ADMISSION_REFUSALS):
        fail(f"normalized_boundary_refusal_set_mismatch:{','.join(sorted(refusal_set))}")
    for entry in refusals:
        if entry["provider_operation_id"] != "exact_refusal_no_provider_call":
            fail(f"normalized_boundary_refusal_has_provider_call:{entry['normalized_command']}")
    for entry in routed:
        if entry["provider_operation_id"] == "exact_refusal_no_provider_call":
            fail(f"normalized_boundary_routed_exact_refusal:{entry['normalized_command']}")

    provider_aliases = {entry["provider_operation_id"] for entry in routed}
    missing_aliases = sorted(set(EXPECTED_COMPATIBILITY_ALIASES) - provider_aliases)
    if missing_aliases:
        fail(f"normalized_boundary_missing_compatibility_alias:{','.join(missing_aliases)}")

    operation_body = function_body(header, "RequiredClusterProviderOperationSet")
    for token in (
        "RequiredClusterProviderCommandBoundarySet()",
        "if (!command.provider_routed) continue;",
        "add_unique(command.normalized_command)",
        "add_unique(command.provider_operation_id)",
        'operation_id == "exact_refusal_no_provider_call"',
    ):
        require_contains(operation_body, token, "derived_cluster_operation_set")
    if "query.plan_operation" in operation_body:
        fail("derived_cluster_operation_set_contains_local_query_operation")
    for refusal in EXPECTED_PRE_ADMISSION_REFUSALS:
        if refusal in operation_body:
            fail(f"derived_cluster_operation_set_contains_exact_refusal:{refusal}")

    refusal_body = function_body(header, "RequiredClusterProviderPreAdmissionRefusalSet")
    for refusal in EXPECTED_PRE_ADMISSION_REFUSALS:
        require_contains(refusal_body, refusal, "pre_admission_refusal_set")

    operation_ids = derived_operation_ids(entries)
    if any(token in operation_ids for token in EXPECTED_PRE_ADMISSION_REFUSALS):
        fail("derived_operation_inventory_contains_exact_refusal")
    if "query.plan_operation" in operation_ids:
        fail("derived_operation_inventory_contains_local_query_operation")
    return {
        "file": "src/cluster_provider/cluster_provider.hpp",
        "status": "normalized_boundary_public_only_additive_operation_set",
        "boundary_command_count": len(entries),
        "provider_routed_count": len(routed),
        "exact_refusal_count": len(refusals),
        "derived_operation_id_count": len(operation_ids),
        "compatibility_aliases": sorted(set(EXPECTED_COMPATIBILITY_ALIASES)),
    }


def check_public_provider_private_token_leakage(repo_root: Path,
                                                project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for relative in PUBLIC_CLUSTER_BOUNDARY_FILES:
        path = project_root / relative
        text = require_file(path, repo_root)
        for index, line in enumerate(text.splitlines(), start=1):
            for pattern in FORBIDDEN_PUBLIC_PRIVATE_BEHAVIOR_PATTERNS:
                if pattern.search(line) and not allowed_negative_or_boundary_context(line):
                    fail(f"public_cluster_private_behavior_token:{relative}:{index}:{line.strip()}")
        if relative.endswith(("no_cluster_provider.cpp", "stub_cluster_provider.cpp")):
            for token in (
                "info.supports_execution = true;",
                "info.supports_route_admission = true;",
                "info.local_runtime_execution_enabled = true;",
                "info.mutable_by_local_core = true;",
            ):
                if token in text:
                    fail(f"public_provider_live_claim:{relative}:{token}")
            execute = function_body(text, "ExecuteClusterOperation")
            for token in (
                "result.ok = true",
                "result.result_shape.rows.push_back",
                "supports_execution = true",
                "supports_route_admission = true",
            ):
                if token in execute:
                    fail(f"public_provider_execute_live_behavior:{relative}:{token}")
        records.append(
            {
                "file": relative,
                "status": "no_private_cluster_implementation_tokens",
                "sha256": sha256_text(text),
            }
        )
    return records


def check_cmake_controls(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    project_cmake = require_file(project_root / "CMakeLists.txt", repo_root)
    for token in (
        "SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_CLUSTER_PROVIDER_EXTERNAL_INCLUDE_DIR",
        "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB",
    ):
        require_contains(project_cmake, token, "project_cmake_cluster_controls")

    records: list[dict[str, Any]] = []
    agent_gate = require_file(project_root / "cmake" / "AgentProductionBuildGate.cmake",
                              repo_root)
    for token in (
        "SB_CLUSTER_PROVIDER_STUB",
        "SB_AGENT_CLUSTER_STUB_LIVE_CLAIMS",
        "cluster stub live claims are forbidden in production builds",
    ):
        require_contains(token=token,
                         text=agent_gate,
                         context="agent_production_live_stub_claim_rejection")
    agent_matrix = require_file(project_root / "cmake" / "AgentProductionConfigureGateMatrix.cmake",
                                repo_root)
    for token in (
        "-DSB_CLUSTER_PROVIDER_STUB=ON",
        "-DSB_NONCLUSTER_ENGINE_PROFILE=bootstrap",
    ):
        require_contains(token=token,
                         text=agent_matrix,
                         context="agent_configure_stub_matrix")
    records.append(
        {
            "file": "cmake/AgentProductionBuildGate.cmake",
            "status": "release_complete_stub_provider_link_allowed_live_claims_rejected",
            "sha256": sha256_text(agent_gate),
        }
    )
    records.append(
        {
            "file": "cmake/AgentProductionConfigureGateMatrix.cmake",
            "status": "bootstrap_stub_provider_matrix_declared",
            "sha256": sha256_text(agent_matrix),
        }
    )

    for relative in (
        "src/engine/internal_api/CMakeLists.txt",
        "src/engine/sblr/CMakeLists.txt",
    ):
        text = require_file(project_root / relative, repo_root)
        for token in (
            "add_library(sb_cluster_provider UNKNOWN IMPORTED GLOBAL)",
            "IMPORTED_LOCATION",
            "SCRATCHBIRD_CLUSTER_PROVIDER_EXTERNAL=1",
            "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
            "SB_CLUSTER_PROVIDER_STUB",
            "src/cluster_provider_stub",
            "src/cluster_provider",
        ):
            require_contains(text, token, f"{relative}:cluster_provider_selection")
        records.append(
            {
                "file": relative,
                "status": "external_stub_no_cluster_selection_declared",
                "sha256": sha256_text(text),
            }
        )
    return records


def check_provider_sources(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    no_cluster = require_file(
        project_root / "src" / "cluster_provider" / "no_cluster_provider.cpp",
        repo_root,
    )
    no_cluster_execute = function_body(no_cluster, "ExecuteClusterOperation")
    for token in (
        "scratchbird.cluster.no_cluster_provider",
        "\"no_cluster\"",
        "\"not_enabled\"",
        "info.external_provider = false;",
        "info.compile_link_only = false;",
        "info.supports_execution = false;",
        "info.supports_route_admission = false;",
        "info.local_runtime_execution_enabled = false;",
        "info.mutable_by_local_core = false;",
        "ValidateClusterProviderHandshake(info)",
        "cluster_provider_handshake",
        "cluster_provider_route_admission",
        "result.ok = false",
        "result.cluster_authority_required = true",
        "kClusterSupportNotEnabledCode",
        "cluster.provider",
    ):
        require_contains(no_cluster if token in (
                             "scratchbird.cluster.no_cluster_provider",
                             "\"no_cluster\"",
                             "\"not_enabled\"",
                             "info.external_provider = false;",
                             "info.compile_link_only = false;",
                             "info.supports_execution = false;",
                             "info.supports_route_admission = false;",
                             "info.local_runtime_execution_enabled = false;",
                             "info.mutable_by_local_core = false;",
                             "ValidateClusterProviderHandshake(info)",
                             "cluster_provider_handshake",
                             "cluster_provider_route_admission",
                         ) else no_cluster_execute,
                         token,
                         "no_cluster_provider")
    if "result.result_shape.rows.push_back" in no_cluster_execute:
        fail("no_cluster_execute_emits_rows")
    records.append({"provider": "no_cluster", "status": "fail_closed_non_mutating"})

    stub = require_file(
        project_root / "src" / "cluster_provider_stub" / "stub_cluster_provider.cpp",
        repo_root,
    )
    stub_execute = function_body(stub, "ExecuteClusterOperation")
    for token in (
        "scratchbird.cluster.compile_link_stub_provider",
        "\"compile_link_stub\"",
        "\"compile_link_only\"",
        "info.external_provider = false;",
        "info.compile_link_only = true;",
        "info.supports_execution = false;",
        "info.supports_route_admission = false;",
        "info.local_runtime_execution_enabled = false;",
        "info.mutable_by_local_core = false;",
        "ValidateClusterProviderHandshake(info)",
        "cluster_provider_handshake",
        "cluster_provider_route_admission",
        "result.ok = false",
        "result.cluster_authority_required = true",
        "cluster.provider.stub",
        "kClusterHandshakeStubCompileLinkOnlyCode",
    ):
        require_contains(stub if token in (
                             "scratchbird.cluster.compile_link_stub_provider",
                             "\"compile_link_stub\"",
                             "\"compile_link_only\"",
                             "info.external_provider = false;",
                             "info.compile_link_only = true;",
                             "info.supports_execution = false;",
                             "info.supports_route_admission = false;",
                             "info.local_runtime_execution_enabled = false;",
                             "info.mutable_by_local_core = false;",
                             "ValidateClusterProviderHandshake(info)",
                             "cluster_provider_handshake",
                             "cluster_provider_route_admission",
                         ) else stub_execute,
                         token,
                         "compile_link_stub_provider")
    for forbidden in ("result.ok = true", "result.result_shape.rows.push_back", "stub_response"):
        if forbidden in stub_execute:
            fail(f"compile_link_stub_execute_forbidden:{forbidden}")
    if "scratchbird.cluster.dummy_provider" in stub or "\"dummy\"" in stub:
        fail("compile_link_stub_retains_dummy_provider_claim")
    records.append({"provider": "compile_link_stub", "status": "compile_link_only_non_mutating"})
    return records


def check_agent_boundary(repo_root: Path, project_root: Path) -> dict[str, Any]:
    text = require_file(project_root / "src" / "core" / "agents" / "agent_cluster_boundary.cpp",
                        repo_root)
    provider_classifier = function_body(text, "ProviderIsExternalProductionProvider")
    for token in (
        "\"external_cluster_provider\"",
        "\"enabled\"",
    ):
        require_contains(provider_classifier, token, "agent_external_provider_classifier")
    for forbidden in ("\"dummy\"", "\"stub_response\""):
        if forbidden in provider_classifier:
            fail(f"agent_external_provider_classifier_forbidden:{forbidden}")
    provider_request = function_body(text, "ProviderRequest")
    for token in (
        "requires_cluster_authority = true",
        "contains_sql_text = false",
    ):
        require_contains(provider_request, token, "agent_provider_request")
    for token in (
        "production live cluster behavior requires external sb_cluster_provider",
        "agent_cluster_external_provider_required",
    ):
        require_contains(text, token, "agent_external_provider_enforcement")
    return {
        "file": "src/core/agents/agent_cluster_boundary.cpp",
        "status": "compile_link_stub_not_external_provider",
    }


def check_sblr_guards(repo_root: Path, project_root: Path) -> dict[str, Any]:
    envelope = require_file(project_root / "src" / "engine" / "sblr" / "sblr_engine_envelope.cpp",
                            repo_root)
    validation = function_body(envelope, "ValidateSblrEnvelope")
    for token in (
        "SB_SBLR_SQL_TEXT_FORBIDDEN",
        "SB_SBLR_NAMES_NOT_RESOLVED_TO_UUIDS",
    ):
        require_contains(validation, token, "sblr_envelope_validation")
    registry = require_file(project_root / "src" / "engine" / "sblr" / "sblr_opcode_registry.cpp",
                            repo_root)
    registry_validation = function_body(registry, "ValidateSblrOpcodeForEnvelope")
    require_contains(registry_validation,
                     "entry->requires_cluster_authority",
                     "sblr_opcode_cluster_authority_validation")
    return {
        "sblr_envelope": "sql_text_forbidden_and_names_must_resolve_to_uuids",
        "sblr_opcode_registry": "cluster_authority_required_for_cluster_entries",
    }


def check_execute_call_sites(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    seen: dict[str, int] = {}
    for path in iter_project_files(project_root / "src"):
        text = path.read_text(encoding="utf-8")
        if "ExecuteClusterOperation" not in text:
            continue
        relative = rel(path, project_root)
        if relative.startswith("src/cluster_provider/") or relative.startswith("src/cluster_provider_stub/"):
            continue
        count = 0
        for line in text.splitlines():
            if "ExecuteClusterOperation(" in line:
                count += 1
        if count == 0:
            continue
        if relative not in ALLOWED_EXECUTE_CALLS:
            fail(f"unexpected_cluster_execute_call_site:{relative}")
        for token in ALLOWED_EXECUTE_CALLS[relative]:
            require_contains(text, token, f"{relative}:cluster_execute_route")
        seen[relative] = count
        records.append(
            {
                "file": relative,
                "execute_call_count": count,
                "status": "allowed_provider_boundary_route",
            }
        )
    missing = sorted(set(ALLOWED_EXECUTE_CALLS) - set(seen))
    if missing:
        fail(f"expected_cluster_execute_call_missing:{','.join(missing)}")
    return sorted(records, key=lambda record: record["file"])


def inventory_surfaces(project_root: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for surface, roots in SURFACE_ROOTS.items():
        files: set[str] = set()
        hits = 0
        for root in roots:
            root_path = project_root / root
            if not root_path.exists():
                fail(f"surface_root_missing:{root}")
            for path in iter_project_files(root_path):
                text = path.read_text(encoding="utf-8", errors="ignore")
                file_hits = sum(text.count(needle) for needle in CLUSTER_NEEDLES)
                if file_hits:
                    hits += file_hits
                    files.add(rel(path, project_root))
        if hits == 0:
            fail(f"cluster_surface_inventory_empty:{surface}")
        records.append(
            {
                "surface": surface,
                "cluster_reference_count": hits,
                "file_count": len(files),
                "sample_files": sorted(files)[:12],
            }
        )
    return records


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    output = args.output.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    if project_root.name != "project":
        fail("project_root_must_be_project_directory")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-097",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "cluster_production_execution": "external_provider_only",
            "default_provider": "no_cluster_fail_closed",
            "public_stub_scope": "compile_link_only_non_mutating",
            "sblr_sql_text_execution_allowed": False,
        },
        "cmake_controls": check_cmake_controls(repo_root, project_root),
        "normalized_cluster_provider_boundary": check_normalized_cluster_provider_boundary(
            repo_root,
            project_root,
        ),
        "public_provider_private_token_scan": check_public_provider_private_token_leakage(
            repo_root,
            project_root,
        ),
        "provider_modes": check_provider_sources(repo_root, project_root),
        "agent_boundary": check_agent_boundary(repo_root, project_root),
        "sblr_guards": check_sblr_guards(repo_root, project_root),
        "execute_call_sites": check_execute_call_sites(repo_root, project_root),
        "cluster_surface_inventory": inventory_surfaces(project_root),
    }
    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    evidence["evidence_sha256"] = sha256_text(encoded)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"public_cluster_boundary_cleanup_audit_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print(f"public_cluster_boundary_cleanup_audit_sha256={evidence['evidence_sha256']}")
    print("public_cluster_boundary_cleanup_audit=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
