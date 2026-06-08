#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""PFAR standalone CTest coverage gate.

This gate audits durable project test registrations and source anchors only.
It intentionally does not read the page/filespace agent runtime execution_plan
directory, so it remains useful after that planning area is archived or moved.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


FORBIDDEN_TOKENS = (
    "docs/" + "execution-plans",
    "docs/" + "completed" + "-execution-plans",
    "TRACKER" + ".csv",
    "ACCEPTANCE" + "_GATES.csv",
    "SPEC_IMPLEMENTATION" + "_AUDIT_MATRIX.csv",
)


REQUIRED_CTESTS = {
    "agent_runtime_lifecycle_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-002"),
    "noncluster_agent_runtime_matrix_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-002A"),
    "agent_cluster_provider_boundary_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-002B"),
    "agent_policy_config_bootstrap_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-002C"),
    "agent_lifecycle_state_machine_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-002D"),
    "agent_policy_attachment_baseline_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-002E"),
    "agent_instance_identity_persistence_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-003A"),
    "agent_runtime_shutdown_recovery_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-003"),
    "agent_supervision_failure_quarantine_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-003B"),
    "page_filespace_agent_handoff_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-004"),
    "page_allocation_manager_live_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-006"),
    "page_allocation_manager_direct_preallocation_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-007"),
    "filespace_capacity_manager_live_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-008"),
    "filespace_capacity_manager_authority_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-008A"),
    "storage_health_manager_authority_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-008B"),
    "page_allocation_preallocation_preference_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-011"),
    "agent_runtime_uuid_authority_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-016AA"),
    "agent_cluster_route_api_provider_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-013A"),
    "agent_management_authorization_policy_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-014"),
    "agent_third_party_management_request_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-016B"),
    "agent_zero_grey_output_contract_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-016C"),
    "agent_open_state_mode_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-017A"),
    "embedded_no_background_agent_gate": ("project/tests/agents/CMakeLists.txt", "PFAR-017"),
    "filespace_preallocation_runtime_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-009"),
    "filespace_growth_runtime_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-010"),
    "dml_page_allocation_runtime_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-012"),
    "sblr_agent_management_route_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-013"),
    "sys_agent_storage_views_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-015"),
    "sys_agent_view_contract_documentation_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-015A"),
    "agent_catalog_runtime_schema_versioning_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-015B"),
    "agent_show_sys_surface_parity_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-015C"),
    "agent_metrics_audit_support_bundle_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-016"),
    "agent_evidence_audit_redaction_retention_gate": ("project/tests/database_lifecycle/CMakeLists.txt", "PFAR-016A"),
    "sbsql_agent_command_surface_matrix_gate": ("project/tests/sbsql_parser_worker/CMakeLists.txt", "PFAR-013B"),
    "agent_examples_evidence_payload_gate": ("project/tests/sbsql_parser_worker/CMakeLists.txt", "PFAR-020A"),
}


SOURCE_ANCHORS = {
    "project/tests/database_lifecycle/sblr_agent_management_route_gate.cpp": (
        "agents.request_page_preallocation",
        "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION",
        "PreallocatePageFamilyPool",
        "agents.request_filespace_growth",
        "SBLR_AGENT_REQUEST_FILESPACE_GROWTH",
        "ExecuteFilespacePhysicalGrowth",
        "filespace.preallocate",
        "SBLR_FILESPACE_PREALLOCATE",
        "PreallocateFilespace",
    ),
    "project/tests/sbsql_parser_worker/sbsql_agent_command_surface_matrix_gate.cpp": (
        "agents.metrics.get",
        "agents.policy.get",
        "agents.evidence.list",
        "agents.audit.list",
        "agents.policy.apply",
        "cluster.agent.control",
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
    ),
    "project/tests/agents/agent_cluster_route_api_provider_gate.cpp": (
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "kClusterHandshakeStubCompileLinkOnlyCode",
        "cluster.sys.agents",
    ),
    "project/tests/agents/agent_management_authorization_policy_gate.cpp": (
        "AGENT.SECURITY_CONTEXT_REQUIRED",
        "AGENT.MANAGEMENT.READ_ONLY_DENIED",
        "FILESPACE.PERMISSION_DENIED",
    ),
    "project/tests/database_lifecycle/sys_agent_storage_views_gate.cpp": (
        "sys.agents",
        "sys.agent_policies",
        "sys.filespace_capacity_agent_state",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/database_lifecycle/sys_agent_view_contract_documentation_gate.cpp": (
        "SB_AGENT_SYS_VIEW_CONTRACT_DOC",
        "FindSysInformationProjectionDefinition",
    ),
    "project/tests/database_lifecycle/agent_catalog_runtime_schema_versioning_gate.cpp": (
        "ValidateAgentCatalogRuntimeSchema",
        "MakeEngineDatabaseRuntimeState",
    ),
    "project/tests/database_lifecycle/agent_show_sys_surface_parity_gate.cpp": (
        "EngineSysAgents",
        "EngineListAgents",
        "EngineShowAgent",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/database_lifecycle/agent_metrics_audit_support_bundle_gate.cpp": (
        "EngineCollectAgentRuntimeObservability",
        "EnginePrepareSupportBundle",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/database_lifecycle/agent_evidence_audit_redaction_retention_gate.cpp": (
        "EngineEvaluateAgentEvidenceRetention",
        "EngineSupportBundleAgentEvidenceSource",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/agents/agent_runtime_uuid_authority_gate.cpp": (
        "ParseDurableEngineIdentityUuid",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/agents/agent_third_party_management_request_gate.cpp": (
        "EngineSubmitThirdPartyAgentManagementRequest",
        "AGENT.THIRD_PARTY.REQUEST_ACCEPTED",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/agents/agent_zero_grey_output_contract_gate.cpp": (
        "BuiltinAgentZeroGreyOutputContract",
        "EngineAgentZeroGreyResultStateAllowed",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/agents/embedded_no_background_agent_gate.cpp": (
        "embedded_direct",
        "background_agents_suppressed",
        "always_in_transaction_mga_engine_owned",
    ),
    "project/tests/agents/agent_open_state_mode_gate.cpp": (
        "AGENT.MANAGEMENT.REPAIR_DENIED",
        "AGENT.MANAGEMENT.SHUTDOWN_IN_PROGRESS",
        "FILESPACE.SHUTDOWN_IN_PROGRESS",
        "GenerateEngineIdentityV7",
    ),
    "project/tests/sbsql_parser_worker/agent_examples_evidence_payload_gate.cpp": (
        "SHOW AGENT memory_governor METRICS",
        "agents.request_page_preallocation",
        "PreallocatePageFamilyPool",
        "PreallocateFilespace",
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
        "ACTION.PERMISSION_DENIED",
        "support_bundle_agent_runtime_evidence",
        "GenerateEngineIdentityV7",
    ),
}


SCAN_FOR_RUNTIME_EXECUTION_PLAN_DEPENDENCIES = (
    "project/tests/agents/CMakeLists.txt",
    "project/tests/database_lifecycle/CMakeLists.txt",
    "project/tests/sbsql_parser_worker/CMakeLists.txt",
    *SOURCE_ANCHORS.keys(),
)


def read_text(repo: pathlib.Path, relative: str) -> str:
    path = repo / relative
    if not path.exists():
        raise AssertionError(f"missing required file: {relative}")
    return path.read_text(encoding="utf-8")


def require_ctest(repo: pathlib.Path, test_name: str, cmake_rel: str, label: str) -> None:
    text = read_text(repo, cmake_rel)
    if test_name not in text:
        raise AssertionError(f"{test_name} missing self label/name in {cmake_rel}")
    if "add_test" not in text:
        raise AssertionError(f"{cmake_rel} does not register CTest tests")
    if label not in text:
        raise AssertionError(f"{test_name} missing stable label {label}")


def require_source_anchors(repo: pathlib.Path, relative: str, anchors: tuple[str, ...]) -> None:
    text = read_text(repo, relative)
    for anchor in anchors:
        if anchor not in text:
            raise AssertionError(f"{relative} missing coverage anchor {anchor}")


def require_no_runtime_execution_plan_dependency(repo: pathlib.Path, relative: str) -> None:
    text = read_text(repo, relative)
    for token in FORBIDDEN_TOKENS:
        if token in text:
            raise AssertionError(f"{relative} contains runtime execution_plan dependency token {token}")


def require_no_fake_uuid_assignment(repo: pathlib.Path, relative: str) -> None:
    text = read_text(repo, relative)
    pattern = re.compile(
        r"(?:uuid|evidence_uuid|policy_uuid|instance_uuid|action_uuid|scope_uuid)"
        r"[^=\n]*=[^\n]*\"(?:agent\.|policy\.|scope\.|agent-|policy:|agent:|scope:)"
    )
    match = pattern.search(text)
    if match:
        raise AssertionError(f"{relative} assigns a label-prefixed value to a UUID field")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    repo = pathlib.Path(args.repo_root).resolve()

    for test_name, (cmake_rel, label) in REQUIRED_CTESTS.items():
        require_ctest(repo, test_name, cmake_rel, label)

    for relative, anchors in SOURCE_ANCHORS.items():
        require_source_anchors(repo, relative, anchors)

    for relative in SCAN_FOR_RUNTIME_EXECUTION_PLAN_DEPENDENCIES:
        require_no_runtime_execution_plan_dependency(repo, relative)

    for relative in (
        "project/src/core/agents/agent_runtime.cpp",
        "project/src/core/agents/agent_runtime_manager.cpp",
        "project/src/core/agents/agent_engine_lifecycle.cpp",
        "project/src/engine/internal_api/agents/agent_action_hooks_api.cpp",
        "project/src/engine/internal_api/agents/agent_management_api.cpp",
        "project/src/engine/internal_api/storage/storage_management_api.cpp",
        "project/src/engine/internal_api/engine_database_runtime.cpp",
    ):
        require_no_fake_uuid_assignment(repo, relative)

    print("pfar_standalone_test_coverage_gate=passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
