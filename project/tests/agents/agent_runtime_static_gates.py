#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static PFAR agent-runtime gates over canonical specs and implementation registries."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


AGENT_SPEC_REL = Path("project/tests/agents/fixtures/public_contract_snapshot")
METRIC_SPEC_REL = Path("project/tests/agents/fixtures/public_contract_snapshot")
AGENT_RUNTIME_CPP_REL = Path("project/src/core/agents/agent_runtime.cpp")
AGENT_RUNTIME_HPP_REL = Path("project/src/core/agents/agent_runtime.hpp")
AGENT_RUNTIME_MANIFEST_CPP_REL = Path("project/src/core/agents/agent_runtime_manifest.cpp")
AGENT_RUNTIME_MANIFEST_DEF_REL = Path("project/src/core/agents/agent_runtime_manifest.def")
AGENT_LIFECYCLE_CPP_REL = Path("project/src/core/agents/agent_engine_lifecycle.cpp")
AGENT_IMPL_DIR_REL = Path("project/src/core/agents/agents")
METRIC_REGISTRY_CPP_REL = Path("project/src/core/metrics/metric_registry.cpp")
MANAGEMENT_API_HPP_REL = Path("project/src/engine/internal_api/agents/agent_management_api.hpp")
MANAGEMENT_API_CPP_REL = Path("project/src/engine/internal_api/agents/agent_management_api.cpp")
ACTION_HOOKS_HPP_REL = Path("project/src/engine/internal_api/agents/agent_action_hooks_api.hpp")
ACTION_HOOKS_CPP_REL = Path("project/src/engine/internal_api/agents/agent_action_hooks_api.cpp")
SUPPORT_BUNDLE_API_CPP_REL = Path("project/src/engine/internal_api/management/support_bundle_api.cpp")
AGENT_OBSERVABILITY_API_CPP_REL = Path("project/src/engine/internal_api/observability/agent_observability_api.cpp")
AGENT_EVIDENCE_RETENTION_API_CPP_REL = Path("project/src/engine/internal_api/observability/agent_evidence_retention_api.cpp")
SYS_AGENTS_VIEW_REL = Path("project/src/sys/agents_views.cpp")
PARSER_SRC_REL = Path("project/src/parsers")
DATABASE_LIFECYCLE_CPP_REL = Path("project/src/storage/database/database_lifecycle.cpp")
PAGE_ALLOCATION_LIFECYCLE_CPP_REL = Path("project/src/storage/page/page_allocation_lifecycle.cpp")
PAGE_FILESPACE_HANDOFF_CPP_REL = Path("project/src/storage/page/page_filespace_handoff.cpp")
FILESPACE_GROWTH_CPP_REL = Path("project/src/storage/filespace/filespace_growth.cpp")
AGENT_ACTION_DISPATCH_CPP_REL = Path("project/src/core/agents/agent_action_dispatch.cpp")
AGENT_ACTION_DISPATCH_STORE_CPP_REL = Path("project/src/engine/internal_api/agents/agent_action_dispatch_store_api.cpp")
AGENT_DURABLE_CATALOG_CPP_REL = Path("project/src/core/agents/agent_durable_catalog.cpp")
AGENT_ENTERPRISE_EVIDENCE_CPP_REL = Path("project/src/core/agents/agent_enterprise_evidence.cpp")
AGENT_ENTERPRISE_DECISION_STORE_CPP_REL = Path("project/src/engine/internal_api/agents/agent_enterprise_decision_store_api.cpp")
AGENT_RUNTIME_SERVICE_CPP_REL = Path("project/src/core/agents/agent_runtime_service.cpp")
SERVER_AGENT_RUNTIME_CPP_REL = Path("project/src/server/server_agent_runtime.cpp")
AEIC_SCOPE_MATRIX_REL = Path("project/tests/agents/fixtures/agent_runtime_public_evidence/artifacts/AGENT_RUNTIME_SCOPE_MATRIX.md")
AEIC_STATUS_MATRIX_REL = Path("project/tests/agents/fixtures/agent_runtime_public_evidence/AGENT_RUNTIME_SCOPE_STATUS_MATRIX.csv")
AEIC_TRACEABILITY_MATRIX_REL = Path("project/tests/agents/fixtures/agent_runtime_public_evidence/AGENT_RUNTIME_SCOPE_TRACEABILITY_MATRIX.csv")
AEIC_EXECUTION_PLAN_NAME = "agent-enterprise-runtime-implementation-closure"
AGENTS_CMAKE_REL = Path("project/src/core/agents/CMakeLists.txt")
AGENT_TESTS_CMAKE_REL = Path("project/tests/agents/CMakeLists.txt")


REGISTRY_SPEC = AGENT_SPEC_REL / "appendix-agent-canonical-registry.md"
NO_DEFAULTS_SPEC = AGENT_SPEC_REL / "appendix-agent-cross-reference-and-no-implicit-defaults-validation.md"
FRAMEWORK_SPEC = AGENT_SPEC_REL / "agent-management-and-operational-framework.md"
RESPONSIBILITY_SPEC = AGENT_SPEC_REL / "appendix-agent-responsibility-index.md"
ACTION_CONTRACT_SPEC = AGENT_SPEC_REL / "appendix-agent-action-contract-matrix.md"
SECURITY_SPEC = AGENT_SPEC_REL / "appendix-agent-security-grant-matrix.md"
SHOW_SYS_SPEC = AGENT_SPEC_REL / "appendix-agent-show-and-sys-surfaces.md"
METRIC_DEPENDENCY_SPEC = AGENT_SPEC_REL / "appendix-agent-metric-dependency-contracts.md"
PAGE_AGENT_SPEC = AGENT_SPEC_REL / "appendix-page-allocation-manager.md"
FILESPACE_AGENT_SPEC = AGENT_SPEC_REL / "appendix-filespace-capacity-manager.md"
STORAGE_HEALTH_AGENT_SPEC = AGENT_SPEC_REL / "appendix-storage-health-manager.md"


REQUIRED_REGISTRY_FIELDS = (
    "agent_type_id",
    "deployment",
    "scope",
    "runs_in",
    "default_install_state",
    "default_runtime_state",
    "authority_class",
    "required_policies",
    "required_metrics",
    "emitted_metrics",
    "allowed_actions",
    "forbidden_actions",
    "failure_mode",
)

FRAMEWORK_API_FUNCTIONS = (
    "EngineListAgents",
    "EngineShowAgent",
    "EngineStartAgent",
    "EngineStopAgent",
    "EnginePauseAgent",
    "EngineResumeAgent",
    "EngineConfigureAgent",
    "EngineRunAgent",
    "EngineDryRunAgent",
    "EngineOverrideAgent",
    "EngineSysAgents",
    "EngineClusterSysAgents",
)

ACTION_HOOK_FUNCTIONS = (
    "EngineRequestPagePreallocation",
    "EngineRequestPageRelocation",
    "EngineRequestFilespaceGrowth",
    "EngineNotifyFilespaceShrinkReadiness",
    "EngineRequestIndexDeltaMerge",
    "EngineRequestIndexRebuildOrShadowBuild",
)

REQUIRED_SYS_SURFACES = (
    "sys.agents",
    "sys.agent_metric_dependencies",
    "sys.agent_policies",
    "sys.agent_actions",
    "sys.agent_evidence",
    "cluster.sys.agents",
    "sys.filespace_capacity_agent_state",
    "sys.page_allocation_agent_state",
    "sys.filespace_shrink_readiness",
)

AMBIGUOUS_TOKENS = (
    "tbd",
    "todo",
    "implicit",
    "as needed",
    "best effort",
    "misc",
    "etc.",
)


@dataclass(frozen=True)
class ImplAgent:
    type_id: str
    deployment: str
    scope: str
    authority: str
    activation: str
    metric_dependencies: tuple[tuple[str, str, bool], ...]


class GateFailure(Exception):
    pass


def read_text(repo_root: Path, rel_path: Path) -> str:
    path = repo_root / resolve_repo_rel(repo_root, rel_path)
    return path.read_text(encoding="utf-8", errors="replace")


def resolve_repo_rel(repo_root: Path, rel_path: Path) -> Path:
    path = repo_root / rel_path
    if path.exists():
        return rel_path
    active_prefix = f"docs" "/execution-plans/{AEIC_EXECUTION_PLAN_NAME}/"
    rel_text = rel_path.as_posix()
    if rel_text.startswith(active_prefix):
        suffix = rel_text[len(active_prefix):]
        completed = Path("docs" "/completed-execution-plans") / AEIC_EXECUTION_PLAN_NAME / suffix
        if (repo_root / completed).exists():
            return completed
    return rel_path


def clean_cell(value: str) -> str:
    value = value.strip()
    if value.startswith("`") and value.endswith("`"):
        value = value[1:-1]
    return value.strip()


def clean_identifier(value: str) -> str:
    return clean_cell(value).strip("`").strip()


def split_markdown_row(line: str) -> list[str]:
    return [clean_cell(part) for part in line.strip().strip("|").split("|")]


def markdown_tables(text: str) -> list[list[dict[str, str]]]:
    lines = text.splitlines()
    tables: list[list[dict[str, str]]] = []
    index = 0
    while index < len(lines):
        if not lines[index].lstrip().startswith("|"):
            index += 1
            continue
        block: list[str] = []
        while index < len(lines) and lines[index].lstrip().startswith("|"):
            block.append(lines[index])
            index += 1
        if len(block) < 3:
            continue
        headers = split_markdown_row(block[0])
        separator = split_markdown_row(block[1])
        if not all(set(cell.replace(":", "").replace("-", "").strip()) == set() for cell in separator):
            continue
        rows: list[dict[str, str]] = []
        for line in block[2:]:
            cells = split_markdown_row(line)
            if len(cells) != len(headers):
                continue
            rows.append(dict(zip(headers, cells)))
        tables.append(rows)
    return tables


def rows_with_headers(text: str, required: set[str]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for table in markdown_tables(text):
        if not table:
            continue
        headers = set(table[0].keys())
        if required.issubset(headers):
            rows.extend(table)
    return rows


def load_registry_spec(repo_root: Path) -> dict[str, dict[str, str]]:
    rows = rows_with_headers(read_text(repo_root, REGISTRY_SPEC), set(REQUIRED_REGISTRY_FIELDS))
    registry: dict[str, dict[str, str]] = {}
    for row in rows:
        agent = clean_identifier(row["agent_type_id"])
        if agent:
            registry[agent] = row
    return registry


def load_action_rows(repo_root: Path) -> dict[str, dict[str, str]]:
    action_rows: dict[str, dict[str, str]] = {}
    for rel_path in (ACTION_CONTRACT_SPEC, PAGE_AGENT_SPEC, FILESPACE_AGENT_SPEC, STORAGE_HEALTH_AGENT_SPEC):
        text = read_text(repo_root, rel_path)
        rows = rows_with_headers(text, {"action_id"})
        rows.extend(rows_with_headers(text, {"Action id"}))
        for row in rows:
            action = clean_identifier(row.get("action_id") or row.get("Action id") or "")
            if action:
                action_rows[action] = row | {"_source": rel_path.as_posix()}
        for row in rows_with_headers(text, {"Field", "Requirement"}):
            if clean_identifier(row.get("Field", "")) != "Allowed actions":
                continue
            for action in explicit_action_ids(row.get("Requirement", "")):
                action_rows.setdefault(action, {"_source": rel_path.as_posix(), "authority_field": "Allowed actions"})
    return action_rows


def load_rights(repo_root: Path) -> set[str]:
    text = read_text(repo_root, SECURITY_SPEC)
    rights = {
        clean_identifier(row["right"])
        for row in rows_with_headers(text, {"right", "allows", "does_not_allow"})
    }
    for root in (
        repo_root / "public_contract_snapshot",
        repo_root / "public_contract_snapshot",
        repo_root / "project/src/engine/internal_api/security",
    ):
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix not in {".md", ".yaml", ".yml", ".cpp", ".hpp", ".h"}:
                continue
            rights.update(re.findall(r"\b(?:OBS|SEC)_[A-Z0-9_]+\b", path.read_text(encoding="utf-8", errors="replace")))
    return {right for right in rights if right}


def load_metric_dependency_agents(repo_root: Path) -> set[str]:
    text = read_text(repo_root, METRIC_DEPENDENCY_SPEC)
    agents: set[str] = set()
    for row in rows_with_headers(text, {"agent_name", "metric_family", "namespace"}):
        agent = clean_identifier(row.get("agent_name", ""))
        if agent:
            agents.add(agent)
    return agents


def load_metric_dependency_namespaces(repo_root: Path) -> set[tuple[str, str, str]]:
    text = read_text(repo_root, METRIC_DEPENDENCY_SPEC)
    rows: set[tuple[str, str, str]] = set()
    for row in rows_with_headers(text, {"agent_name", "metric_family", "namespace"}):
        agent = clean_identifier(row.get("agent_name", ""))
        family = clean_identifier(row.get("metric_family", ""))
        namespace = row.get("namespace", "").strip()
        if agent and family and namespace:
            rows.add((agent, family, namespace))
    return rows


def load_surfaces(repo_root: Path) -> dict[str, dict[str, str]]:
    text = read_text(repo_root, SHOW_SYS_SPEC)
    surfaces: dict[str, dict[str, str]] = {}
    for headers in ({"surface", "required_right"}, {"surface", "required_right", "columns"}):
        for row in rows_with_headers(text, headers):
            surface = clean_identifier(row.get("surface", ""))
            if surface:
                surfaces[surface] = row
    return surfaces


def metric_tokens_from_docs(repo_root: Path) -> set[str]:
    roots = [repo_root / METRIC_SPEC_REL, repo_root / AGENT_SPEC_REL]
    tokens: set[str] = set()
    for root in roots:
        for path in root.rglob("*.md"):
            text = path.read_text(encoding="utf-8", errors="replace")
            tokens.update(re.findall(r"\bsb_[A-Za-z0-9_]+\b", text))
    return tokens


def load_metric_descriptors(repo_root: Path) -> dict[str, str]:
    text = read_text(repo_root, METRIC_REGISTRY_CPP_REL)
    descriptors: dict[str, str] = {}
    pattern = re.compile(
        r'Descriptor\(\s*"(?P<family>sb_[A-Za-z0-9_]+)"\s*,\s*'
        r"MetricType::[A-Za-z0-9_]+\s*,\s*MetricUnit::[A-Za-z0-9_]+\s*,\s*"
        r'"(?P<namespace>[^"]+)"',
        re.MULTILINE,
    )
    for match in pattern.finditer(text):
        descriptors[match.group("family")] = match.group("namespace")
    return descriptors


def parse_impl_registry(repo_root: Path) -> dict[str, ImplAgent]:
    text = read_text(repo_root, AGENT_RUNTIME_CPP_REL)
    manifest_text = read_text(repo_root, AGENT_RUNTIME_MANIFEST_CPP_REL)
    manifest_def_text = (
        read_text(repo_root, AGENT_RUNTIME_MANIFEST_DEF_REL)
        if (repo_root / AGENT_RUNTIME_MANIFEST_DEF_REL).exists()
        else ""
    )
    contract_deps: dict[str, tuple[tuple[str, str, bool], ...]] = {}
    contract_pattern = re.compile(
        r'ContractRow\(\s*"(?P<agent>[^"]+)"\s*,\s*'
        r'"(?P<family>sb_[A-Za-z0-9_]+)"\s*,\s*'
        r'"(?P<namespace>[^"]+)"',
        re.MULTILINE,
    )
    for match in contract_pattern.finditer(text):
        namespace = match.group("namespace")
        contract_deps.setdefault(match.group("agent"), tuple())
        contract_deps[match.group("agent")] = contract_deps[match.group("agent")] + (
            (
                match.group("family"),
                namespace,
                namespace.startswith("cluster.sys.metrics."),
            ),
        )

    pattern = re.compile(
        r'Agent\(\s*"(?P<type_id>[^"]+)"\s*,\s*'
        r"AgentDeployment::(?P<deployment>[A-Za-z0-9_]+)\s*,\s*"
        r'"(?P<scope>[^"]+)"\s*,\s*'
        r"AgentAuthorityClass::(?P<authority>[A-Za-z0-9_]+)\s*,\s*"
        r"AgentActivationProfile::(?P<activation>[A-Za-z0-9_]+)\s*,\s*"
        r"(?:\{(?P<deps>.*?)\}|MetricDependenciesForAgent\(\s*\"(?P<deps_agent>[^\"]+)\"\s*\))\s*\)",
        re.DOTALL,
    )
    dep_pattern = re.compile(
        r'Dep\(\s*"(?P<family>[^"]+)"(?:\s*,\s*"(?P<namespace>[^"]*)")?'
        r"(?:\s*,\s*(?P<cluster>true|false))?\s*\)"
    )
    agents: dict[str, ImplAgent] = {}
    manifest_def_pattern = re.compile(
        r"SB_AGENT_MANIFEST_ENTRY\(\s*(?P<type_id>[A-Za-z0-9_]+)\s*,\s*"
        r"(?P<deployment>[A-Za-z0-9_]+)\s*,\s*"
        r'"(?P<scope>[^"]+)"\s*,\s*'
        r"(?P<authority>[A-Za-z0-9_]+)\s*,\s*"
        r"(?P<activation>[A-Za-z0-9_]+)\s*\)"
    )
    for match in manifest_def_pattern.finditer(manifest_def_text):
        agent_type = match.group("type_id")
        agents[agent_type] = ImplAgent(
            type_id=agent_type,
            deployment=match.group("deployment"),
            scope=match.group("scope"),
            authority=match.group("authority"),
            activation=match.group("activation"),
            metric_dependencies=contract_deps.get(agent_type, tuple()),
        )

    entry_pattern = re.compile(
        r'Entry\(\s*"(?P<type_id>[^"]+)"\s*,\s*'
        r"AgentDeployment::(?P<deployment>[A-Za-z0-9_]+)\s*,\s*"
        r'"(?P<scope>[^"]+)"\s*,\s*'
        r"AgentAuthorityClass::(?P<authority>[A-Za-z0-9_]+)\s*,\s*"
        r"AgentActivationProfile::(?P<activation>[A-Za-z0-9_]+)\s*\)",
        re.DOTALL,
    )
    if not agents:
        for match in entry_pattern.finditer(manifest_text):
            agent_type = match.group("type_id")
            agents[agent_type] = ImplAgent(
                type_id=agent_type,
                deployment=match.group("deployment"),
                scope=match.group("scope"),
                authority=match.group("authority"),
                activation=match.group("activation"),
                metric_dependencies=contract_deps.get(agent_type, tuple()),
            )

    for match in pattern.finditer(text):
        if match.group("deps_agent"):
            deps = contract_deps.get(match.group("deps_agent"), tuple())
        else:
            deps = tuple(
                (
                    dep.group("family"),
                    dep.group("namespace") or "",
                    dep.group("cluster") == "true",
                )
                for dep in dep_pattern.finditer(match.group("deps") or "")
            )
        agent_type = match.group("type_id")
        agents[agent_type] = ImplAgent(
            type_id=agent_type,
            deployment=match.group("deployment"),
            scope=match.group("scope"),
            authority=match.group("authority"),
            activation=match.group("activation"),
            metric_dependencies=deps,
        )
    return agents


def parse_manifest_def(repo_root: Path) -> dict[str, ImplAgent]:
    text = read_text(repo_root, AGENT_RUNTIME_MANIFEST_DEF_REL)
    pattern = re.compile(
        r"SB_AGENT_MANIFEST_ENTRY\(\s*(?P<type_id>[A-Za-z0-9_]+)\s*,\s*"
        r"(?P<deployment>[A-Za-z0-9_]+)\s*,\s*"
        r'"(?P<scope>[^"]+)"\s*,\s*'
        r"(?P<authority>[A-Za-z0-9_]+)\s*,\s*"
        r"(?P<activation>[A-Za-z0-9_]+)\s*\)"
    )
    entries: dict[str, ImplAgent] = {}
    for match in pattern.finditer(text):
        agent_type = match.group("type_id")
        if agent_type in entries:
            raise GateFailure(f"duplicate manifest definition entry: {agent_type}")
        entries[agent_type] = ImplAgent(
            type_id=agent_type,
            deployment=match.group("deployment"),
            scope=match.group("scope"),
            authority=match.group("authority"),
            activation=match.group("activation"),
            metric_dependencies=tuple(),
        )
    return entries


def scope_matrix_sets(repo_root: Path) -> tuple[set[str], set[str], set[str]]:
    text = read_text(repo_root, AEIC_SCOPE_MATRIX_REL)
    non_cluster = {
        clean_identifier(row["Agent/module"])
        for row in rows_with_headers(text, {"Agent/module", "Scope", "Required enterprise outcome"})
    }
    noncanonical = {
        clean_identifier(row["Module"])
        for row in rows_with_headers(text, {"Module", "Required disposition"})
    }
    cluster = {
        clean_identifier(row["Agent/module"])
        for row in rows_with_headers(text, {"Agent/module", "Core repository outcome", "Live behavior owner"})
    }
    return non_cluster, noncanonical, cluster


def cmake_agent_sources(repo_root: Path) -> set[str]:
    text = read_text(repo_root, AGENTS_CMAKE_REL)
    return {
        match.group(1)
        for match in re.finditer(r"agents/([A-Za-z0-9_]+)\.cpp", text)
    }


def extract_identifier_list(cell: str) -> set[str]:
    return set(re.findall(r"\b[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)?\b", cell))


def extract_metric_tokens(cell: str) -> set[str]:
    return set(re.findall(r"\bsb_[A-Za-z0-9_]+\b", cell))


def explicit_action_ids(cell: str) -> set[str]:
    backticked = {clean_identifier(match) for match in re.findall(r"`([A-Za-z][A-Za-z0-9_]*)`", cell)}
    if backticked:
        return backticked
    return {
        clean_identifier(part)
        for part in re.split(r";|,", cell)
        if clean_identifier(part) and " " not in clean_identifier(part)
    }


def ensure_no_execution_plan_inputs(rel_paths: tuple[Path, ...], errors: list[str]) -> None:
    for rel_path in rel_paths:
        path_text = rel_path.as_posix()
        if "docs" "/execution-plans" in path_text or "docs" "/completed-execution-plans" in path_text:
            errors.append(f"gate fixture path must not read execution-plans: {path_text}")


def require_files(repo_root: Path, rel_paths: tuple[Path, ...], errors: list[str]) -> None:
    ensure_no_execution_plan_inputs(rel_paths, errors)
    for rel_path in rel_paths:
        if not (repo_root / rel_path).exists():
            errors.append(f"required gate input missing: {rel_path}")


def gate_no_implicit_defaults(repo_root: Path) -> list[str]:
    rel_inputs = (
        REGISTRY_SPEC,
        NO_DEFAULTS_SPEC,
        ACTION_CONTRACT_SPEC,
        SECURITY_SPEC,
        SHOW_SYS_SPEC,
        METRIC_DEPENDENCY_SPEC,
        PAGE_AGENT_SPEC,
        FILESPACE_AGENT_SPEC,
        STORAGE_HEALTH_AGENT_SPEC,
        AGENT_RUNTIME_CPP_REL,
        AGENT_RUNTIME_MANIFEST_CPP_REL,
        AGENT_RUNTIME_HPP_REL,
        METRIC_REGISTRY_CPP_REL,
        MANAGEMENT_API_CPP_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    spec_registry = load_registry_spec(repo_root)
    impl_registry = parse_impl_registry(repo_root)
    action_rows = load_action_rows(repo_root)
    rights = load_rights(repo_root)
    surfaces = load_surfaces(repo_root)
    metric_dependency_agents = load_metric_dependency_agents(repo_root)
    metric_dependency_namespaces = load_metric_dependency_namespaces(repo_root)
    metric_doc_tokens = metric_tokens_from_docs(repo_root)
    metric_descriptors = load_metric_descriptors(repo_root)
    metric_tokens = metric_doc_tokens | set(metric_descriptors)

    if not spec_registry:
        errors.append("canonical agent registry table produced no rows")
    if not impl_registry:
        errors.append("implementation CanonicalAgentRegistry() produced no rows")

    for required_surface in REQUIRED_SYS_SURFACES:
        if required_surface not in surfaces:
            errors.append(f"sys/show surface missing from canonical surface spec: {required_surface}")

    for agent, row in sorted(spec_registry.items()):
        for field in REQUIRED_REGISTRY_FIELDS:
            value = row.get(field, "").strip()
            if not value:
                errors.append(f"{agent}: registry field {field} is empty")
            lowered = value.lower()
            for token in AMBIGUOUS_TOKENS:
                if token in lowered:
                    errors.append(f"{agent}: registry field {field} contains ambiguous token {token!r}: {value}")

        policies = extract_identifier_list(row.get("required_policies", ""))
        policies = {item for item in policies if item.endswith("_policy") or item.endswith("_baseline")}
        if not policies:
            errors.append(f"{agent}: required_policies has no exact policy family")

        if not extract_metric_tokens(row.get("required_metrics", "")) and agent not in metric_dependency_agents:
            errors.append(f"{agent}: required_metrics has no exact metric family")

        for metric in extract_metric_tokens(row.get("required_metrics", "")) | extract_metric_tokens(row.get("emitted_metrics", "")):
            if metric not in metric_tokens:
                errors.append(f"{agent}: metric {metric} is not declared in metrics specs or implementation registry")

        for action in explicit_action_ids(row.get("allowed_actions", "")):
            if action not in action_rows:
                errors.append(f"{agent}: allowed action {action} has no action contract/spec row")

    for action, row in sorted(action_rows.items()):
        evidence_value = (
            row.get("evidence")
            or row.get("Evidence")
            or row.get("Evidence required")
            or row.get("evidence_type")
            or ""
        )
        failure_value = (
            row.get("failure_behavior")
            or row.get("Fail-closed behavior")
            or row.get("fail_behavior")
            or ""
        )
        if "authority_field" not in row and not evidence_value.strip():
            errors.append(f"{action}: action contract does not name evidence")
        if "authority_field" not in row and not failure_value.strip():
            errors.append(f"{action}: action contract does not name fail-closed behavior")
        values = " ".join(row.values())
        for right in re.findall(r"\b(?:OBS|SEC)_[A-Z0-9_]+\b", values):
            if right not in rights:
                errors.append(f"{action}: action contract references unmapped right {right}")

    for surface, row in sorted(surfaces.items()):
        values = " ".join(row.values())
        for right in re.findall(r"\b(?:OBS|SEC)_[A-Z0-9_]+\b", values):
            if right not in rights and right != "OBS_METRICS_READ_FAMILY":
                errors.append(f"{surface}: surface references unmapped right {right}")

    for agent, impl in sorted(impl_registry.items()):
        if agent not in spec_registry:
            errors.append(f"{agent}: implementation registry row missing from canonical registry spec")
            continue
        spec = spec_registry[agent]
        if impl.deployment != spec["deployment"]:
            errors.append(f"{agent}: deployment drift implementation={impl.deployment} spec={spec['deployment']}")
        if impl.scope != spec["scope"]:
            errors.append(f"{agent}: scope drift implementation={impl.scope} spec={spec['scope']}")
        if impl.authority != spec["authority_class"]:
            errors.append(f"{agent}: authority drift implementation={impl.authority} spec={spec['authority_class']}")
        if not impl.metric_dependencies:
            errors.append(f"{agent}: implementation registry has no explicit metric dependencies")
        for family, namespace, _cluster_only in impl.metric_dependencies:
            if family not in metric_tokens:
                errors.append(f"{agent}: implementation metric dependency {family} is not declared")
            expected_namespace = metric_descriptors.get(family)
            if expected_namespace and namespace and namespace != expected_namespace:
                if (agent, family, namespace) in metric_dependency_namespaces:
                    continue
                errors.append(
                    f"{agent}: implementation metric dependency {family} namespace drift "
                    f"implementation={namespace} registry={expected_namespace}"
                )

    runtime_cpp = read_text(repo_root, AGENT_RUNTIME_CPP_REL)
    for diagnostic in (
        "SB_AGENT_REGISTRY.TYPE_ID_REQUIRED",
        "SB_AGENT_REGISTRY.SCOPE_REQUIRED",
        "SB_AGENT_REGISTRY.METRIC_DEPENDENCY_REQUIRED",
        "SB_AGENT_REGISTRY.DUPLICATE_TYPE_ID",
        "ValidateStorageSpaceAgentDefaults",
    ):
        if diagnostic not in runtime_cpp:
            errors.append(f"CanonicalAgentRegistry validation does not enforce {diagnostic}")

    management_cpp = read_text(repo_root, MANAGEMENT_API_CPP_REL)
    for emitted_field in ("agent_type", "deployment", "scope", "authority", "metric_dependencies"):
        if f'"{emitted_field}"' not in management_cpp:
            errors.append(f"agent management API does not emit descriptor field {emitted_field}")

    return errors


def gate_management_framework(repo_root: Path) -> list[str]:
    rel_inputs = (
        FRAMEWORK_SPEC,
        REGISTRY_SPEC,
        ACTION_CONTRACT_SPEC,
        SHOW_SYS_SPEC,
        AGENT_RUNTIME_CPP_REL,
        AGENT_RUNTIME_HPP_REL,
        AGENT_LIFECYCLE_CPP_REL,
        MANAGEMENT_API_HPP_REL,
        MANAGEMENT_API_CPP_REL,
        ACTION_HOOKS_HPP_REL,
        ACTION_HOOKS_CPP_REL,
        SYS_AGENTS_VIEW_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    framework = read_text(repo_root, FRAMEWORK_SPEC)
    for phrase in (
        "Agents are never correctness authority",
        "Transaction finality",
        "parser trust",
        "Every agent decision loop must execute these steps in order",
        "Parsers are clients to these surfaces and cannot enforce agent authority",
    ):
        if phrase not in framework:
            errors.append(f"management framework spec missing required authority phrase: {phrase}")
    for step in range(1, 11):
        if f"{step}." not in framework:
            errors.append(f"management framework control loop missing step {step}")

    runtime_hpp = read_text(repo_root, AGENT_RUNTIME_HPP_REL)
    runtime_cpp = read_text(repo_root, AGENT_RUNTIME_CPP_REL)
    lifecycle_cpp = read_text(repo_root, AGENT_LIFECYCLE_CPP_REL)
    management_hpp = read_text(repo_root, MANAGEMENT_API_HPP_REL)
    management_cpp = read_text(repo_root, MANAGEMENT_API_CPP_REL)
    hooks_hpp = read_text(repo_root, ACTION_HOOKS_HPP_REL)
    hooks_cpp = read_text(repo_root, ACTION_HOOKS_CPP_REL)
    sys_agents = read_text(repo_root, SYS_AGENTS_VIEW_REL)

    for token, source_name, source in (
        ("SB_AGENT_RUNTIME_FRAMEWORK", AGENT_RUNTIME_HPP_REL.as_posix(), runtime_hpp),
        ("SB_ENGINE_INTERNAL_API_AGENT_MANAGEMENT", MANAGEMENT_API_HPP_REL.as_posix(), management_hpp),
        ("SB_PID017_AGENT_ACTION_HOOKS", ACTION_HOOKS_HPP_REL.as_posix(), hooks_hpp),
        ("SB_SYS_AGENTS_SURFACES", SYS_AGENTS_VIEW_REL.as_posix(), sys_agents),
    ):
        if token not in source:
            errors.append(f"{source_name} missing framework search key {token}")

    for function in FRAMEWORK_API_FUNCTIONS:
        if function not in management_hpp or function not in management_cpp:
            errors.append(f"agent management API missing implementation for {function}")
    for function in ACTION_HOOK_FUNCTIONS:
        if function not in hooks_hpp or function not in hooks_cpp:
            errors.append(f"agent action hook API missing implementation for {function}")

    for token in (
        "SecurityContextHasRight",
        "ValidateAgentSecurity",
        "ResolveAgentMetricDependencies",
        "ValidateAgentPolicy",
        "PersistAgentEvidence",
        "AddAgentDescriptorRow",
        "CanonicalAgentRegistry",
        "EvaluateAgentFeatureAvailability",
        "MakeApiBehaviorSuccess",
        "MakeApiBehaviorDiagnostic",
    ):
        if token not in management_cpp:
            errors.append(f"agent management API missing framework call {token}")

    for token in (
        "ValidateCommon",
        "ValidateRuntimeDecision",
        "policy_authorized",
        "evidence_sink_available",
        "metrics_fresh",
        "PersistHookEvidence",
        "EvaluateAgentAction",
        "RecordAgentRuntimeMetric",
    ):
        if token not in hooks_cpp:
            errors.append(f"agent action hooks missing framework call {token}")

    if "AgentPersistenceUsesScratchBirdStorageAuthority() { return true; }" not in runtime_cpp:
        errors.append("agent persistence authority is not explicitly ScratchBird storage backed")
    if "cluster_paths_failed_closed" not in lifecycle_cpp:
        errors.append("database engine lifecycle agent does not publish cluster fail-closed state")

    parser_errors = scan_parser_boundary(repo_root)
    errors.extend(parser_errors)

    return errors


def scan_parser_boundary(repo_root: Path) -> list[str]:
    errors: list[str] = []
    parser_root = repo_root / PARSER_SRC_REL
    if not parser_root.exists():
        errors.append(f"parser source root missing: {PARSER_SRC_REL}")
        return errors
    lowering = parser_root / "sbsql_worker/lowering/lowering.cpp"
    lowering_text = lowering.read_text(encoding="utf-8", errors="replace") if lowering.exists() else ""
    for token in ("authority.engine.agent_management_api_required", "authority.parser.no_agent_execution"):
        if token not in lowering_text:
            errors.append(f"parser lowering missing agent boundary token {token}")
    for path in parser_root.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        rel = path.relative_to(repo_root)
        text = path.read_text(encoding="utf-8", errors="replace")
        if "CanonicalAgentRegistry" in text:
            errors.append(f"parser source must not read canonical agent runtime registry directly: {rel}")
        if re.search(r'#include\s+["<]agent_runtime\.hpp[">]', text):
            errors.append(f"parser source must not include agent_runtime.hpp directly: {rel}")
    return errors


def gate_responsibility_index(repo_root: Path) -> list[str]:
    rel_inputs = (
        REGISTRY_SPEC,
        RESPONSIBILITY_SPEC,
        ACTION_CONTRACT_SPEC,
        PAGE_AGENT_SPEC,
        FILESPACE_AGENT_SPEC,
        STORAGE_HEALTH_AGENT_SPEC,
        AGENT_RUNTIME_CPP_REL,
        AGENT_RUNTIME_MANIFEST_CPP_REL,
        AGENT_LIFECYCLE_CPP_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    spec_registry = load_registry_spec(repo_root)
    impl_registry = parse_impl_registry(repo_root)
    responsibility_rows = rows_with_headers(
        read_text(repo_root, RESPONSIBILITY_SPEC),
        {"Agent", "Responsibility summary", "Owns", "Does not own", "Primary action style", "Detailed authority source"},
    )
    responsibility: dict[str, dict[str, str]] = {}
    for row in responsibility_rows:
        agent = clean_identifier(row["Agent"])
        if agent:
            responsibility[agent] = row

    for agent in sorted(spec_registry):
        if agent not in responsibility:
            errors.append(f"{agent}: canonical registry row missing from responsibility index")
    for agent in sorted(responsibility):
        if agent not in spec_registry:
            errors.append(f"{agent}: responsibility-index row missing from canonical registry")
    for agent in sorted(impl_registry):
        if agent not in responsibility:
            errors.append(f"{agent}: implementation registry row missing from responsibility index")

    for agent, row in sorted(responsibility.items()):
        source = row["Detailed authority source"]
        accepted_source = "appendix-agent-canonical-registry.md" in source
        accepted_source = accepted_source or (
            agent == "filespace_capacity_manager" and "appendix-filespace-capacity-manager.md" in source
        )
        accepted_source = accepted_source or (
            agent == "page_allocation_manager" and "appendix-page-allocation-manager.md" in source
        )
        accepted_source = accepted_source or (
            agent == "storage_health_manager" and "appendix-storage-health-manager.md" in source
        )
        if not accepted_source:
            errors.append(f"{agent}: responsibility row does not cite the canonical registry")
        if not row["Owns"].strip() or not row["Does not own"].strip():
            errors.append(f"{agent}: responsibility ownership or forbidden-action text is empty")

    storage_expected_sources = {
        "filespace_capacity_manager": "appendix-filespace-capacity-manager.md",
        "page_allocation_manager": "appendix-page-allocation-manager.md",
        "storage_health_manager": "appendix-storage-health-manager.md",
    }
    for agent, expected_source in storage_expected_sources.items():
        source = responsibility.get(agent, {}).get("Detailed authority source", "")
        if expected_source not in source:
            errors.append(f"{agent}: responsibility row does not cite {expected_source}")

    action_rows = load_action_rows(repo_root)
    actions_by_owner: dict[str, set[str]] = {}
    for action, row in action_rows.items():
        owner = clean_identifier(row.get("owning_agent") or row.get("Owning agent") or "")
        if owner:
            actions_by_owner.setdefault(owner, set()).add(action)

    forbidden_action_sets = {
        "filespace_capacity_manager": {
            "preallocate_page_family",
            "relocate_pages",
            "defragment_page_family",
            "publish_shrink_ready",
            "recommend_index_rebuild",
            "advance_cleanup_lwm",
        },
        "page_allocation_manager": {
            "request_filespace_expand",
            "request_filespace_move",
            "request_filespace_shrink",
            "request_filespace_truncate",
            "recommend_primary_shadow_promotion",
        },
        "storage_health_manager": {
            "request_filespace_expand",
            "request_filespace_move",
            "request_filespace_shrink",
            "request_filespace_truncate",
            "preallocate_page_family",
            "relocate_pages",
        },
        "transaction_pressure_manager": {
            "rollback_transaction",
            "commit_transaction",
            "advance_cleanup_lwm",
        },
    }
    for agent, forbidden in forbidden_action_sets.items():
        owned_actions = actions_by_owner.get(agent, set())
        drift = sorted(owned_actions & forbidden)
        if drift:
            errors.append(f"{agent}: forbidden actions exposed as owned actions: {', '.join(drift)}")

    runtime_agents_dir = repo_root / AGENT_IMPL_DIR_REL
    for agent in sorted(impl_registry):
        source_path = runtime_agents_dir / f"{agent}.cpp"
        if not source_path.exists():
            errors.append(f"{agent}: implementation anchor missing: {source_path.relative_to(repo_root)}")
            continue
        text = source_path.read_text(encoding="utf-8", errors="replace")
        if "CanonicalAgentRegistry" not in text:
            errors.append(f"{agent}: implementation anchor does not point back to CanonicalAgentRegistry")

    expected_authority = {
        "filespace_capacity_manager": "request_action",
        "page_allocation_manager": "request_action",
        "storage_health_manager": "recommend_only",
    }
    for agent, authority in expected_authority.items():
        if impl_registry.get(agent) is None:
            errors.append(f"{agent}: missing from implementation registry")
        elif impl_registry[agent].authority != authority:
            errors.append(f"{agent}: authority drift implementation={impl_registry[agent].authority} expected={authority}")
        if spec_registry.get(agent, {}).get("authority_class") != authority:
            errors.append(f"{agent}: spec authority drift expected={authority}")

    lifecycle = read_text(repo_root, AGENT_LIFECYCLE_CPP_REL)
    for token in ("cluster_only", "cluster_paths_failed_closed", "selected_agent_type_ids"):
        if token not in lifecycle:
            errors.append(f"engine lifecycle selection missing responsibility boundary token {token}")

    cross_boundary_rows = rows_with_headers(
        read_text(repo_root, RESPONSIBILITY_SPEC),
        {"Boundary", "Owning agent", "Requesting agents", "Rule"},
    )
    for row in cross_boundary_rows:
        owner = clean_identifier(row["Owning agent"])
        if owner in spec_registry:
            pass
        elif "not an operational agent" not in owner:
            errors.append(f"cross-agent boundary has unknown owning agent: {owner}")
        for requester in re.split(r",| and ", row["Requesting agents"]):
            requester = clean_identifier(requester)
            if not requester or requester in {"all storage-related agents"}:
                continue
            if "metrics" in requester or "authority" in requester:
                continue
            if requester not in spec_registry:
                errors.append(f"cross-agent boundary has unknown requesting agent: {requester}")

    return errors


def gate_implementation_anchors(repo_root: Path) -> list[str]:
    rel_inputs = (
        AGENT_RUNTIME_CPP_REL,
        AGENT_RUNTIME_HPP_REL,
        AGENT_LIFECYCLE_CPP_REL,
        DATABASE_LIFECYCLE_CPP_REL,
        PAGE_ALLOCATION_LIFECYCLE_CPP_REL,
        PAGE_FILESPACE_HANDOFF_CPP_REL,
        FILESPACE_GROWTH_CPP_REL,
        ACTION_HOOKS_CPP_REL,
        ACTION_HOOKS_HPP_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    required_search_keys = (
        (AGENT_RUNTIME_CPP_REL, "SB_AGENT_RUNTIME_CANONICAL_REGISTRY_IMPL"),
        (AGENT_RUNTIME_HPP_REL, "SB_AGENT_RUNTIME_CANONICAL_REGISTRY"),
        (AGENT_LIFECYCLE_CPP_REL, "SB_AGENT_RUNTIME_ENGINE_LIFECYCLE_START"),
        (DATABASE_LIFECYCLE_CPP_REL, "SB_DATABASE_RUNTIME_ACTIVATION_EVIDENCE"),
        (PAGE_ALLOCATION_LIFECYCLE_CPP_REL, "SB_PAGE_ALLOCATION_LIFECYCLE_RESERVE"),
        (PAGE_FILESPACE_HANDOFF_CPP_REL, "SB_PAGE_FILESPACE_HANDOFF_QUEUE"),
        (FILESPACE_GROWTH_CPP_REL, "SB_FILESPACE_CAPACITY_GROWTH_LEDGER"),
        (ACTION_HOOKS_CPP_REL, "SB_AGENT_ACTION_HOOK_PAGE_PREALLOCATION_REQUEST"),
        (ACTION_HOOKS_CPP_REL, "SB_AGENT_ACTION_HOOK_FILESPACE_GROWTH_REQUEST"),
        (ACTION_HOOKS_HPP_REL, "SB_PID017_AGENT_ACTION_HOOKS"),
    )
    for rel_path, search_key in required_search_keys:
        if f"SEARCH_KEY: {search_key}" not in read_text(repo_root, rel_path):
            errors.append(f"{rel_path}: missing durable search key {search_key}")

    required_function_anchors = (
        (AGENT_RUNTIME_CPP_REL, "CanonicalAgentRegistry"),
        (AGENT_LIFECYCLE_CPP_REL, "StartDatabaseEngineLifecycleAgent"),
        (DATABASE_LIFECYCLE_CPP_REL, "RecordRuntimeActivationEvidence"),
        (PAGE_ALLOCATION_LIFECYCLE_CPP_REL, "ReservePageAllocation"),
        (PAGE_FILESPACE_HANDOFF_CPP_REL, "EvaluatePageFilespaceAgentRequest"),
        (FILESPACE_GROWTH_CPP_REL, "RequestInsertFilespaceGrowth"),
        (ACTION_HOOKS_CPP_REL, "EngineRequestPagePreallocation"),
        (ACTION_HOOKS_CPP_REL, "EngineRequestFilespaceGrowth"),
    )
    for rel_path, function_anchor in required_function_anchors:
        if function_anchor not in read_text(repo_root, rel_path):
            errors.append(f"{rel_path}: missing function anchor {function_anchor}")

    return errors


def gate_enterprise_production_separation(repo_root: Path) -> list[str]:
    rel_inputs = (
        AGENT_RUNTIME_MANIFEST_CPP_REL,
        AGENT_ACTION_DISPATCH_CPP_REL,
        AGENT_DURABLE_CATALOG_CPP_REL,
        AGENT_ENTERPRISE_EVIDENCE_CPP_REL,
        AGENT_RUNTIME_SERVICE_CPP_REL,
        SERVER_AGENT_RUNTIME_CPP_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    cluster_only_sources = {
        "cluster_autoscale_manager.cpp",
        "cluster_scheduler_manager.cpp",
        "cluster_upgrade_manager.cpp",
        "distributed_query_metrics_agent.cpp",
        "remote_query_routing_agent.cpp",
    }
    impl_dir = repo_root / AGENT_IMPL_DIR_REL
    for path in sorted(impl_dir.glob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = [
            line.strip()
            for line in text.splitlines()
            if line.strip() and not line.strip().startswith("//")
        ]
        if path.name in cluster_only_sources:
            if "external" not in text.lower() or "cluster" not in text.lower():
                errors.append(f"{path.relative_to(repo_root)}: cluster stub lacks external-provider wording")
            continue
        if len(lines) <= 15:
            errors.append(
                f"{path.relative_to(repo_root)}: non-cluster source is too small to be enterprise implementation"
            )
        if "anchor-only" in text.lower() or "anchor only" in text.lower():
            errors.append(f"{path.relative_to(repo_root)}: non-cluster source still uses anchor-only wording")

    core_agent_text = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in (repo_root / "project/src/core/agents").glob("agent_*.cpp")
    )
    for token in ("StableDigest", "fnv1a64", "FNV", "fnv"):
        if token in core_agent_text:
            errors.append(f"core agent enterprise path still contains non-crypto digest token {token}")

    required_tokens = (
        (AGENT_ACTION_DISPATCH_CPP_REL, "LIVE_EXECUTOR_REQUIRED"),
        (AGENT_ACTION_DISPATCH_CPP_REL, "LIVE_EXECUTION_EVIDENCE_REQUIRED"),
        (AGENT_DURABLE_CATALOG_CPP_REL, "ROOT_DIGEST_MISMATCH"),
        (AGENT_ENTERPRISE_EVIDENCE_CPP_REL, "SB_AGENT_ENTERPRISE_EVIDENCE.PERSISTED"),
        (AGENT_RUNTIME_SERVICE_CPP_REL, "AgentRuntimeService::AcquireLease"),
        (SERVER_AGENT_RUNTIME_CPP_REL, "runtime_service_.AcquireLease"),
        (SERVER_AGENT_RUNTIME_CPP_REL, "durable_catalog_root_digest"),
    )
    for rel_path, token in required_tokens:
        if token not in read_text(repo_root, rel_path):
            errors.append(f"{rel_path}: missing enterprise production token {token}")
    return errors


def gate_enterprise_metrics_resource_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        AGENT_ACTION_DISPATCH_CPP_REL,
        AGENT_ACTION_DISPATCH_STORE_CPP_REL,
        AGENT_ENTERPRISE_EVIDENCE_CPP_REL,
        AGENT_ENTERPRISE_DECISION_STORE_CPP_REL,
        ACTION_HOOKS_CPP_REL,
        MANAGEMENT_API_CPP_REL,
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    required_tokens = (
        (AGENT_ACTION_DISPATCH_CPP_REL, "AgentMetricRuntimeMode::production_strict"),
        (AGENT_ACTION_DISPATCH_CPP_REL, "SB_AGENT_COMMERCIAL_EVIDENCE.METRIC_DIGEST_MISMATCH"),
        (AGENT_ACTION_DISPATCH_CPP_REL, "strict_metric_input_digest"),
        (AGENT_ACTION_DISPATCH_STORE_CPP_REL, "ValidateStrictMetricsBeforeActionReservation"),
        (AGENT_ACTION_DISPATCH_STORE_CPP_REL, "SB_AGENT_COMMERCIAL_EVIDENCE.METRIC_DIGEST_MISMATCH"),
        (AGENT_ACTION_DISPATCH_STORE_CPP_REL, "ResourceReservationForAction"),
        (AGENT_ENTERPRISE_EVIDENCE_CPP_REL, "EvaluateStrictMetricEvidence"),
        (AGENT_ENTERPRISE_EVIDENCE_CPP_REL, "SB_AGENT_ENTERPRISE_EVIDENCE.METRIC_DIGEST_MISMATCH"),
        (AGENT_ENTERPRISE_DECISION_STORE_CPP_REL, "ResourceReservationForDecision"),
        (ACTION_HOOKS_CPP_REL, "DurableResourceReservationRequiredForHook"),
        (ACTION_HOOKS_CPP_REL, "SB_AGENT_METRIC_SNAPSHOT.PRODUCTION_OBSERVED_SNAPSHOT_REQUIRED"),
        (ACTION_HOOKS_CPP_REL, "AgentMetricRuntimeMode::production_strict"),
        (MANAGEMENT_API_CPP_REL, "DurableResourceReservationForManagement"),
        (MANAGEMENT_API_CPP_REL, "SB_AGENT_METRIC_SNAPSHOT.PRODUCTION_OBSERVED_SNAPSHOT_REQUIRED"),
        (MANAGEMENT_API_CPP_REL, "AgentMetricRuntimeMode::production_strict"),
    )
    for rel_path, token in required_tokens:
        if token not in read_text(repo_root, rel_path):
            errors.append(f"{rel_path}: missing AEIC-015 token {token}")

    store_text = read_text(repo_root, AGENT_ACTION_DISPATCH_STORE_CPP_REL)
    dispatch_start = store_text.find("AgentActionDispatchStoreResult DispatchAgentActionWithDurableCatalogStore")
    if dispatch_start == -1:
        errors.append("agent_action_dispatch_store_api.cpp: dispatch store function missing")
    else:
        dispatch_body = store_text[dispatch_start:]
        metric_pos = dispatch_body.find("ValidateStrictMetricsBeforeActionReservation")
        load_pos = dispatch_body.find("LoadAgentDurableCatalogImage")
        reservation_pos = dispatch_body.find("AcquireDurableAgentResourceReservation")
        if metric_pos == -1:
            errors.append("agent_action_dispatch_store_api.cpp: missing strict metric prevalidation call")
        if load_pos != -1 and metric_pos != -1 and metric_pos > load_pos:
            errors.append("agent_action_dispatch_store_api.cpp: strict metrics run after durable catalog load")
        if reservation_pos != -1 and metric_pos != -1 and metric_pos > reservation_pos:
            errors.append("agent_action_dispatch_store_api.cpp: strict metrics run after resource reservation acquisition")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_metrics_resource_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_metrics_resource_audit_gate is not registered in agent test CMake")
    if "AEIC-015" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-015 strict metric/resource label")

    action_store_test = read_text(repo_root, Path("project/tests/agents/agent_action_dispatch_store_gate.cpp"))
    if "TestStoreBackedDispatchRequiresStrictMetricsBeforeReservation" not in action_store_test:
        errors.append("agent_action_dispatch_store_gate lacks pre-reservation strict metric refusal test")
    if "forged metric refusal reached durable store/reservation path" not in action_store_test:
        errors.append("agent_action_dispatch_store_gate lacks forged-digest no-reservation assertion")

    return errors


def gate_enterprise_management_surfaces_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        MANAGEMENT_API_CPP_REL,
        SUPPORT_BUNDLE_API_CPP_REL,
        AGENT_OBSERVABILITY_API_CPP_REL,
        AGENT_EVIDENCE_RETENTION_API_CPP_REL,
        AGENT_TESTS_CMAKE_REL,
        Path("project/tests/agents/agent_management_route_evidence_gate.cpp"),
        Path("project/tests/agents/agent_management_durable_mutation_gate.cpp"),
        Path("project/tests/database_lifecycle/agent_metrics_audit_support_bundle_gate.cpp"),
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    required_tokens = (
        (MANAGEMENT_API_CPP_REL, "RequireProductionDurableCatalogStoreContext"),
        (MANAGEMENT_API_CPP_REL, "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_STORE_REQUIRED"),
        (MANAGEMENT_API_CPP_REL, "LoadAgentDurableCatalogImage"),
        (MANAGEMENT_API_CPP_REL, "agent_durable_runtime_catalog"),
        (MANAGEMENT_API_CPP_REL, "agent_command_durable_catalog"),
        (MANAGEMENT_API_CPP_REL, "agent_third_party_durable_catalog"),
        (SUPPORT_BUNDLE_API_CPP_REL, "OPS.SUPPORT_BUNDLE.CALLER_AGENT_EVIDENCE_FORBIDDEN"),
        (SUPPORT_BUNDLE_API_CPP_REL, "OPS.SUPPORT_BUNDLE.DURABLE_AGENT_CATALOG_REQUIRED"),
        (SUPPORT_BUNDLE_API_CPP_REL, "AddDurableCatalogEvidenceRows"),
        (SUPPORT_BUNDLE_API_CPP_REL, "tamper_signature_present"),
        (AGENT_OBSERVABILITY_API_CPP_REL, "agent_observability_durable_catalog"),
        (AGENT_OBSERVABILITY_API_CPP_REL, "agent_observability_caller_records_forbidden"),
        (AGENT_EVIDENCE_RETENTION_API_CPP_REL, "agent_evidence_retention_durable_catalog"),
        (AGENT_EVIDENCE_RETENTION_API_CPP_REL, "agent_evidence_retention_caller_records_forbidden"),
    )
    for rel_path, token in required_tokens:
        if token not in read_text(repo_root, rel_path):
            errors.append(f"{rel_path}: missing AEIC-016 token {token}")

    management_text = read_text(repo_root, MANAGEMENT_API_CPP_REL)
    if "RequireProductionDurableCatalogStoreContext" not in management_text:
        errors.append("agent_management_api.cpp: production store-context guard missing")
    for function_name in ("ResolveDurableCatalogForRead", "MutatingAgentOperation"):
        function_pos = management_text.find(function_name)
        guard_pos = management_text.find("RequireProductionDurableCatalogStoreContext", function_pos)
        if function_pos == -1 or guard_pos == -1:
            errors.append(f"agent_management_api.cpp: {function_name} lacks production store-context guard")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_management_surfaces_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_management_surfaces_audit_gate is not registered in agent test CMake")
    if "AEIC-016" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-016 management/support label")

    route_gate = read_text(repo_root, Path("project/tests/agents/agent_management_route_evidence_gate.cpp"))
    if "PersistAgentDurableCatalogImage" not in route_gate or "StoreContext" not in route_gate:
        errors.append("agent_management_route_evidence_gate does not prove store-backed sys/show surfaces")
    mutation_gate = read_text(repo_root, Path("project/tests/agents/agent_management_durable_mutation_gate.cpp"))
    if "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_STORE_REQUIRED" not in mutation_gate:
        errors.append("agent_management_durable_mutation_gate lacks production store-required refusal")
    support_gate = read_text(repo_root, Path("project/tests/database_lifecycle/agent_metrics_audit_support_bundle_gate.cpp"))
    if "TestProductionSupportBundleReadsDurableAgentCatalog" not in support_gate:
        errors.append("support-bundle gate lacks durable catalog production read test")
    if "production observability accepted caller-supplied records" not in support_gate:
        errors.append("support-bundle gate lacks forged production observability refusal")

    return errors


def gate_enterprise_node_metrics_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/node_resource_agent.cpp"),
        Path("project/src/core/agents/agents/metrics_registry_manager.cpp"),
        Path("project/src/core/agents/agents/metrics_registry_manager.hpp"),
        METRIC_REGISTRY_CPP_REL,
        Path("project/tests/agents/agent_enterprise_local_resource_agents_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    metrics_manager_cpp = read_text(repo_root, Path("project/src/core/agents/agents/metrics_registry_manager.cpp"))
    metrics_manager_hpp = read_text(repo_root, Path("project/src/core/agents/agents/metrics_registry_manager.hpp"))
    metric_registry_cpp = read_text(repo_root, METRIC_REGISTRY_CPP_REL)
    local_resource_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_local_resource_agents_gate.cpp"))

    required_tokens = (
        (metrics_manager_hpp, "MetricsRegistryManagerActionRequest", "metrics manager action request"),
        (metrics_manager_hpp, "ApplyMetricsRegistryManagerAction", "metrics manager action handler"),
        (metrics_manager_cpp, "GenerateMetricRollups", "rollup handler"),
        (metrics_manager_cpp, "AppendMetricRawSample", "raw metric history handler"),
        (metrics_manager_cpp, "sb_export_adapter_queue_depth", "export queue shed gauge mutation"),
        (metrics_manager_cpp, "sb_metric_export_shed_total", "export shed counter mutation"),
        (metrics_manager_cpp, "SB_AGENT_CLUSTER_PROVIDER_REQUIRED", "cluster metric external-provider refusal"),
        (metric_registry_cpp, "sb_metric_export_shed_total", "metric export shed descriptor"),
        (local_resource_gate, "ApplyMetricsRegistryManagerAction", "local resource gate action handler"),
        (local_resource_gate, "rollup_rows_created > 0", "rollup creation assertion"),
        (local_resource_gate, "export_shed_written", "export shed assertion"),
        (local_resource_gate, "cluster metric mutation", "cluster metric refusal assertion"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-020 missing {description}: {token}")

    if "agent_enterprise_node_metrics_audit_gate" not in read_text(repo_root, AGENT_TESTS_CMAKE_REL):
        errors.append("agent_enterprise_node_metrics_audit_gate is not registered in agent test CMake")
    if "AEIC-020" not in read_text(repo_root, AGENT_TESTS_CMAKE_REL):
        errors.append("agent test CMake lacks AEIC-020 node metrics label")

    node_resource_cpp = read_text(repo_root, Path("project/src/core/agents/agents/node_resource_agent.cpp"))
    if "cluster_metric_implementation\", \"external_provider_only" not in node_resource_cpp:
        errors.append("node_resource_agent lacks external-provider-only cluster metric evidence")
    if "SB_AGENT_CLUSTER_PROVIDER_REQUIRED" not in node_resource_cpp:
        errors.append("node_resource_agent lacks cluster metric provider refusal")

    return errors


def gate_enterprise_storage_agents_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/storage_health_manager.cpp"),
        Path("project/src/core/agents/agents/filespace_capacity_manager.cpp"),
        Path("project/src/core/agents/agents/page_allocation_manager.cpp"),
        Path("project/tests/agents/agent_enterprise_storage_agents_gate.cpp"),
        Path("project/tests/agents/page_allocation_manager_direct_preallocation_gate.cpp"),
        Path("project/tests/agents/filespace_capacity_manager_live_gate.cpp"),
        Path("project/tests/agents/storage_health_manager_authority_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    storage_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_storage_agents_gate.cpp"))
    required_storage_gate_tokens = (
        "EvaluatePageAllocationManagerTick",
        "ClassifyPageAllocationLedgerForRecovery",
        "EvaluateFilespaceCapacityManagerTick",
        "SerializePageFilespaceAgentRequestQueue",
        "ClassifyPageFilespaceAgentRequestQueueForRecovery",
        "EvaluateStorageHealthManagerAction",
        "AppendEnterpriseAgentDecisionEvidence",
        "storage agent durable catalog invalid after evidence writes",
    )
    for token in required_storage_gate_tokens:
        if token not in storage_gate:
            errors.append(f"agent_enterprise_storage_agents_gate missing token: {token}")

    page_gate = read_text(repo_root, Path("project/tests/agents/page_allocation_manager_direct_preallocation_gate.cpp"))
    if "ClassifyPageAllocationLedgerForRecovery" not in page_gate:
        errors.append("page allocation direct gate lacks recovery classification proof")
    if "RequireUnchanged" not in page_gate:
        errors.append("page allocation direct gate lacks no-mutation refusal proof")

    filespace_gate = read_text(repo_root, Path("project/tests/agents/filespace_capacity_manager_live_gate.cpp"))
    if "RestorePageFilespaceAgentRequestQueue" not in filespace_gate:
        errors.append("filespace capacity gate lacks queue restore proof")
    if "physical_filespace_mutation_attempted" not in filespace_gate:
        errors.append("filespace capacity gate lacks no direct physical mutation proof")

    storage_health_gate = read_text(repo_root, Path("project/tests/agents/storage_health_manager_authority_gate.cpp"))
    if "ExpectNoMutation" not in storage_health_gate:
        errors.append("storage health gate lacks no direct mutation proof")
    if "filespace_capacity_manager" not in storage_health_gate:
        errors.append("storage health gate lacks quarantine route target proof")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_storage_agents_gate" not in tests_cmake:
        errors.append("agent_enterprise_storage_agents_gate is not registered in agent test CMake")
    if "AEIC-021" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-021 storage agent label")

    return errors


def gate_enterprise_cleanup_archive_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/storage_version_cleanup_agent.cpp"),
        Path("project/src/core/agents/agents/cleanup_archive_manager.cpp"),
        Path("project/tests/agents/agent_enterprise_cleanup_archive_agents_gate.cpp"),
        Path("project/tests/database_lifecycle/dpc_storage_version_cleanup_agent_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    cleanup_cpp = read_text(repo_root, Path("project/src/core/agents/agents/storage_version_cleanup_agent.cpp"))
    archive_cpp = read_text(repo_root, Path("project/src/core/agents/agents/cleanup_archive_manager.cpp"))
    enterprise_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_cleanup_archive_agents_gate.cpp"))
    dpc_gate = read_text(repo_root, Path("project/tests/database_lifecycle/dpc_storage_version_cleanup_agent_gate.cpp"))

    required_tokens = (
        (cleanup_cpp, "ComputeAuthoritativeCleanupHorizon", "authoritative MGA cleanup horizon"),
        (cleanup_cpp, "RunLocalGarbageCollectionSweep", "real local GC sweep"),
        (cleanup_cpp, "parser_finality_authority\", \"false", "parser finality non-authority evidence"),
        (archive_cpp, "legal_hold_active", "legal-hold evidence"),
        (archive_cpp, "recovery_authority\", \"false", "recovery non-authority evidence"),
        (enterprise_gate, "RunStorageVersionCleanupAgentBatch", "enterprise storage cleanup route"),
        (enterprise_gate, "EvaluateCleanupArchiveManager", "enterprise cleanup archive route"),
        (enterprise_gate, "AppendEnterpriseAgentDecisionEvidence", "durable enterprise evidence"),
        (enterprise_gate, "refused_non_authoritative", "non-authoritative cleanup refusal"),
        (enterprise_gate, "legal_hold_active", "legal-hold refusal proof"),
        (dpc_gate, "STORAGE_VERSION_CLEANUP.NON_AUTHORITATIVE_REFUSAL", "DPC non-authoritative refusal coverage"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-022 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_cleanup_archive_agents_gate" not in tests_cmake:
        errors.append("agent_enterprise_cleanup_archive_agents_gate is not registered in agent test CMake")
    if "AEIC-022" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-022 cleanup/archive label")

    return errors


def gate_enterprise_memory_admission_alert_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/memory_governor.cpp"),
        Path("project/src/core/agents/agents/admission_control_manager.cpp"),
        Path("project/src/core/agents/agents/alert_manager.cpp"),
        Path("project/src/core/agents/agent_memory_coupling.cpp"),
        Path("project/tests/agents/agent_enterprise_memory_admission_alert_gate.cpp"),
        Path("project/tests/agents/agent_memory_coupling_hardening_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    memory_cpp = read_text(repo_root, Path("project/src/core/agents/agents/memory_governor.cpp"))
    admission_cpp = read_text(repo_root, Path("project/src/core/agents/agents/admission_control_manager.cpp"))
    alert_cpp = read_text(repo_root, Path("project/src/core/agents/agents/alert_manager.cpp"))
    coupling_cpp = read_text(repo_root, Path("project/src/core/agents/agent_memory_coupling.cpp"))
    enterprise_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_memory_admission_alert_gate.cpp"))
    coupling_gate = read_text(repo_root, Path("project/tests/agents/agent_memory_coupling_hardening_gate.cpp"))

    required_tokens = (
        (memory_cpp, "SB_AGENT_MEMORY_GOVERNOR_AUTHORITY_UNTRUSTED", "memory governor strict authority refusal"),
        (memory_cpp, "MemoryGovernorDecisionKind::force_spill", "memory governor spill decision"),
        (memory_cpp, "MemoryGovernorDecisionKind::shrink_cache", "memory governor cache shrink decision"),
        (memory_cpp, "MemoryGovernorDecisionKind::deny_large_grant", "memory governor hard-limit denial"),
        (admission_cpp, "SB_AGENT_ADMISSION_AUTHORITY_UNTRUSTED", "admission strict authority refusal"),
        (admission_cpp, "AdmissionControlDecisionKind::throttle_admission", "admission throttle decision"),
        (admission_cpp, "AdmissionControlDecisionKind::downgrade_admission", "admission downgrade decision"),
        (admission_cpp, "AdmissionControlDecisionKind::deny_admission", "admission denial decision"),
        (alert_cpp, "SB_AGENT_ALERT_AUTHORITY_UNTRUSTED", "alert strict authority refusal"),
        (alert_cpp, "AlertManagerDecisionKind::fire_alert", "alert fire decision"),
        (alert_cpp, "AlertManagerDecisionKind::silence_alert", "alert silence decision"),
        (alert_cpp, "AlertManagerDecisionKind::clear_alert", "alert clear decision"),
        (coupling_cpp, "AcquireAgentMemoryReservations", "durable memory/resource reservation path"),
        (coupling_cpp, "BuildAgentMemoryEvidenceBundle", "support-bundle redaction path"),
        (coupling_cpp, "EvaluateAgentMemoryPressureActionBoundary", "memory pressure action boundary"),
        (enterprise_gate, "RunConcurrentPressureReservations", "high-concurrency pressure route"),
        (enterprise_gate, "ValidateAgentMemoryMetricSnapshot", "strict memory metric snapshot proof"),
        (enterprise_gate, "AcquireAgentMemoryReservations", "enterprise durable reservation proof"),
        (enterprise_gate, "BuildAgentMemoryEvidenceBundle", "enterprise support-bundle redaction proof"),
        (enterprise_gate, "AppendEnterpriseAgentDecisionEvidence", "durable enterprise decision evidence"),
        (enterprise_gate, "SB_AGENT_MEMORY_GOVERNOR_AUTHORITY_UNTRUSTED", "memory authority negative test"),
        (enterprise_gate, "SB_AGENT_ADMISSION_AUTHORITY_UNTRUSTED", "admission authority negative test"),
        (enterprise_gate, "SB_AGENT_ALERT_AUTHORITY_UNTRUSTED", "alert authority negative test"),
        (coupling_gate, "MMCH_AGENT_MEMORY_EVIDENCE_REDACTION_OVERHEAD", "memory coupling redaction guard"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-023 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_memory_admission_alert_gate" not in tests_cmake:
        errors.append("agent_enterprise_memory_admission_alert_gate is not registered in agent test CMake")
    if "agent_enterprise_memory_admission_alert_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_memory_admission_alert_audit_gate is not registered in agent test CMake")
    if "AEIC-023" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-023 memory/admission/alert label")

    return errors


def gate_enterprise_index_agents_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/index_health_manager.cpp"),
        Path("project/src/core/agents/agents/index_garbage_cleanup_agent.cpp"),
        Path("project/src/core/agents/agents/shadow_index_build_agent.cpp"),
        Path("project/src/core/index/secondary_index_garbage_cleanup.cpp"),
        Path("project/src/core/index/shadow_index_build_lifecycle.cpp"),
        Path("project/tests/agents/agent_enterprise_index_agents_gate.cpp"),
        Path("project/tests/database_lifecycle/dpc_secondary_index_garbage_cleanup_agent_gate.cpp"),
        Path("project/tests/database_lifecycle/dpc_shadow_index_build_lifecycle_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    health_cpp = read_text(repo_root, Path("project/src/core/agents/agents/index_health_manager.cpp"))
    cleanup_cpp = read_text(repo_root, Path("project/src/core/agents/agents/index_garbage_cleanup_agent.cpp"))
    shadow_cpp = read_text(repo_root, Path("project/src/core/agents/agents/shadow_index_build_agent.cpp"))
    cleanup_core = read_text(repo_root, Path("project/src/core/index/secondary_index_garbage_cleanup.cpp"))
    shadow_core = read_text(repo_root, Path("project/src/core/index/shadow_index_build_lifecycle.cpp"))
    enterprise_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_index_agents_gate.cpp"))
    dpc_cleanup_gate = read_text(repo_root, Path("project/tests/database_lifecycle/dpc_secondary_index_garbage_cleanup_agent_gate.cpp"))
    dpc_shadow_gate = read_text(repo_root, Path("project/tests/database_lifecycle/dpc_shadow_index_build_lifecycle_gate.cpp"))

    required_tokens = (
        (health_cpp, "SB_AGENT_INDEX_HEALTH_AUTHORITY_UNTRUSTED", "index health authority refusal"),
        (health_cpp, "recommend_index_rebuild", "index health rebuild recommendation"),
        (health_cpp, "recommend_index_drop", "index health drop recommendation"),
        (health_cpp, "request_fast_filespace_for_index_rebuild", "index health fast filespace request"),
        (cleanup_cpp, "RunSecondaryIndexGarbageCleanupBatch", "index cleanup core route"),
        (cleanup_cpp, "INDEX_GARBAGE_CLEANUP.NON_AUTHORITATIVE_REFUSAL", "index cleanup non-authority refusal"),
        (cleanup_cpp, "parser_finality_authority\", \"false", "index cleanup parser non-authority evidence"),
        (shadow_cpp, "PublishShadowIndexBuild", "shadow index publish route"),
        (shadow_cpp, "shadow_index_agent_non_authoritative_refusal", "shadow index non-authority refusal"),
        (shadow_cpp, "parser_finality_authority\", \"false", "shadow index parser non-authority evidence"),
        (cleanup_core, "validation_before_ok", "index cleanup validation-before proof"),
        (cleanup_core, "validation_after_ok", "index cleanup validation-after proof"),
        (shadow_core, "EvaluateShadowIndexPlannerVisibility", "shadow index visibility gate"),
        (enterprise_gate, "RunIndexGarbageCleanupAgentBatch", "enterprise index cleanup route"),
        (enterprise_gate, "PublishShadowIndexBuildAgentStep", "enterprise shadow publish route"),
        (enterprise_gate, "AppendEnterpriseAgentDecisionEvidence", "durable enterprise decision evidence"),
        (enterprise_gate, "SB_AGENT_INDEX_HEALTH_AUTHORITY_UNTRUSTED", "enterprise index health negative test"),
        (enterprise_gate, "refused_non_authoritative", "enterprise index cleanup non-authority test"),
        (enterprise_gate, "shadow_index_agent_non_authoritative_refusal", "enterprise shadow non-authority test"),
        (dpc_cleanup_gate, "DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT_GATE", "DPC cleanup live gate"),
        (dpc_shadow_gate, "DPC_SHADOW_INDEX_BUILD_LIFECYCLE_GATE", "DPC shadow lifecycle gate"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-024 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_index_agents_gate" not in tests_cmake:
        errors.append("agent_enterprise_index_agents_gate is not registered in agent test CMake")
    if "agent_enterprise_index_agents_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_index_agents_audit_gate is not registered in agent test CMake")
    if "AEIC-024" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-024 index-agent label")

    return errors


def gate_enterprise_transaction_optimizer_policy_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/transaction_pressure_manager.cpp"),
        Path("project/src/core/agents/agents/runtime_learning_agent.cpp"),
        Path("project/src/core/agents/agents/policy_recommendation_manager.cpp"),
        Path("project/src/core/agents/agent_optimizer_recommendation.cpp"),
        Path("project/src/core/agents/agent_policy_recommendation_application.cpp"),
        Path("project/src/engine/optimizer/agent_optimizer_recommendation_bridge.cpp"),
        Path("project/tests/agents/agent_enterprise_transaction_optimizer_policy_gate.cpp"),
        Path("project/tests/agents/agent_optimizer_recommendation_evidence_gate.cpp"),
        Path("project/tests/database_lifecycle/dpc_transaction_pressure_manager_long_idle_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    tx_cpp = read_text(repo_root, Path("project/src/core/agents/agents/transaction_pressure_manager.cpp"))
    learning_cpp = read_text(repo_root, Path("project/src/core/agents/agents/runtime_learning_agent.cpp"))
    policy_cpp = read_text(repo_root, Path("project/src/core/agents/agents/policy_recommendation_manager.cpp"))
    optimizer_cpp = read_text(repo_root, Path("project/src/engine/optimizer/agent_optimizer_recommendation_bridge.cpp"))
    policy_app_cpp = read_text(repo_root, Path("project/src/core/agents/agent_policy_recommendation_application.cpp"))
    enterprise_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_transaction_optimizer_policy_gate.cpp"))
    optimizer_gate = read_text(repo_root, Path("project/tests/agents/agent_optimizer_recommendation_evidence_gate.cpp"))
    tx_gate = read_text(repo_root, Path("project/tests/database_lifecycle/dpc_transaction_pressure_manager_long_idle_gate.cpp"))

    required_tokens = (
        (tx_cpp, "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION", "transaction pressure non-authority refusal"),
        (tx_cpp, "always_active_transaction_replacement", "transaction replacement rule evidence"),
        (tx_cpp, "parser_finality_authority\", \"false", "transaction pressure parser non-authority evidence"),
        (learning_cpp, "SB_AGENT_RUNTIME_LEARNING_AUTHORITY_UNTRUSTED", "runtime learning authority refusal"),
        (learning_cpp, "recommend_planner_correction", "runtime learning planner correction"),
        (policy_cpp, "SB_AGENT_POLICY_RECOMMENDATION_AUTHORITY_UNTRUSTED", "policy recommendation authority refusal"),
        (policy_cpp, "create_policy_recommendation", "policy recommendation creation"),
        (optimizer_cpp, "ValidateAgentOptimizerRecommendationEvidence", "optimizer recommendation consumption"),
        (optimizer_cpp, "SB_OPTIMIZER_AGENT_RECOMMENDATION.OK", "optimizer recommendation accepted diagnostic"),
        (policy_app_cpp, "AEIC_POLICY_RECOMMENDATION_APPLICATION_CONTRACT", "policy recommendation application contract"),
        (policy_app_cpp, "ValidateAgentPolicyConfigAgainstSchema", "policy schema validation"),
        (policy_app_cpp, "AUTO_APPLY_REFUSED", "policy no-auto-apply refusal"),
        (enterprise_gate, "EvaluateTransactionPressureManagerTick", "enterprise transaction pressure route"),
        (enterprise_gate, "EvaluateOptimizerAgentRecommendation", "enterprise optimizer consumption route"),
        (enterprise_gate, "EvaluateAgentPolicyRecommendationApplication", "enterprise policy recommendation application route"),
        (enterprise_gate, "SB_AGENT_OPTIMIZER_RECOMMENDATION.UNSAFE_AUTHORITY", "optimizer unsafe recommendation negative test"),
        (enterprise_gate, "SB_AGENT_POLICY_RECOMMENDATION_APPLICATION.SCHEMA_REFUSED", "policy schema negative test"),
        (enterprise_gate, "AppendEnterpriseAgentDecisionEvidence", "durable enterprise decision evidence"),
        (optimizer_gate, "TestValidDurableRecommendationAccepted", "optimizer recommendation focused gate"),
        (tx_gate, "DPC_TRANSACTION_PRESSURE_MANAGER_LONG_IDLE_GATE", "DPC transaction pressure gate"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-025 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_transaction_optimizer_policy_gate" not in tests_cmake:
        errors.append("agent_enterprise_transaction_optimizer_policy_gate is not registered in agent test CMake")
    if "agent_enterprise_transaction_optimizer_policy_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_transaction_optimizer_policy_audit_gate is not registered in agent test CMake")
    if "AEIC-025" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-025 transaction/optimizer/policy label")

    return errors


def gate_enterprise_parser_support_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/parser_interface_manager.cpp"),
        Path("project/src/core/agents/agents/support_bundle_triage_agent.cpp"),
        Path("project/src/parsers/sbsql_worker/lifecycle/agent_parser_interface_bridge.cpp"),
        Path("project/src/engine/internal_api/agents/agent_support_bundle_triage_route_api.cpp"),
        Path("project/src/engine/internal_api/management/support_bundle_api.cpp"),
        Path("project/tests/agents/agent_enterprise_parser_support_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_advisory_agents_gate.cpp"),
        Path("project/tests/database_lifecycle/supportability_evidence_conformance.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    parser_cpp = read_text(repo_root, Path("project/src/core/agents/agents/parser_interface_manager.cpp"))
    support_cpp = read_text(repo_root, Path("project/src/core/agents/agents/support_bundle_triage_agent.cpp"))
    parser_bridge = read_text(repo_root, Path("project/src/parsers/sbsql_worker/lifecycle/agent_parser_interface_bridge.cpp"))
    support_route = read_text(repo_root, Path("project/src/engine/internal_api/agents/agent_support_bundle_triage_route_api.cpp"))
    support_api = read_text(repo_root, Path("project/src/engine/internal_api/management/support_bundle_api.cpp"))
    enterprise_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_parser_support_gate.cpp"))
    advisory_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_advisory_agents_gate.cpp"))
    supportability_gate = read_text(repo_root, Path("project/tests/database_lifecycle/supportability_evidence_conformance.cpp"))

    required_tokens = (
        (parser_cpp, "SB_AGENT_PARSER_INTERFACE_AUTHORITY_UNTRUSTED", "parser authority refusal"),
        (parser_cpp, "drain_parser_family", "parser drain decision"),
        (parser_cpp, "quarantine_parser_package", "parser package quarantine decision"),
        (support_cpp, "SB_AGENT_SUPPORT_TRIAGE_AUTHORITY_UNTRUSTED", "support triage authority refusal"),
        (support_cpp, "protected_material_suppressed", "support triage protected material suppression"),
        (parser_bridge, "AEIC_PARSER_INTERFACE_LIFECYCLE_BRIDGE", "parser lifecycle bridge search key"),
        (parser_bridge, "RecordRecycleRequested", "parser drain lifecycle mutation"),
        (parser_bridge, "ApplyFailurePolicy", "parser quarantine lifecycle mutation"),
        (parser_bridge, "parser_finality_authority=false", "parser non-authority evidence"),
        (support_route, "AEIC_SUPPORT_BUNDLE_TRIAGE_ROUTE_API", "support route search key"),
        (support_route, "EnginePrepareSupportBundle", "support-bundle API consumption"),
        (support_route, "engine_authorized_support_export:true", "engine support export authorization"),
        (support_route, "sidecar_authority", "sidecar authority refusal/evidence"),
        (support_api, "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN", "support API protected material refusal"),
        (enterprise_gate, "ApplyParserInterfaceAgentLifecycleRoute", "enterprise parser lifecycle route"),
        (enterprise_gate, "ApplySupportBundleTriageAgentRoute", "enterprise support bundle route"),
        (enterprise_gate, "SB_AGENT_PARSER_INTERFACE_ROUTE.UNSAFE_AUTHORITY", "parser unsafe authority negative test"),
        (enterprise_gate, "SB_AGENT_SUPPORT_TRIAGE_ROUTE.UNSAFE_AUTHORITY", "support unsafe authority negative test"),
        (enterprise_gate, "AppendEnterpriseAgentDecisionEvidence", "durable enterprise decision evidence"),
        (advisory_gate, "TestParserAndSupportAgents", "advisory parser/support smoke gate"),
        (supportability_gate, "TestEngineSupportBundleApi", "supportability API route gate"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-026 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_parser_support_gate" not in tests_cmake:
        errors.append("agent_enterprise_parser_support_gate is not registered in agent test CMake")
    if "agent_enterprise_parser_support_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_parser_support_audit_gate is not registered in agent test CMake")
    if "AEIC-026" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-026 parser/support label")

    return errors


def gate_enterprise_nosql_route_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/nosql_family_maintenance_agent.cpp"),
        Path("project/src/core/agents/agents/nosql_backpressure_debt_agent.cpp"),
        Path("project/src/engine/internal_api/nosql/nosql_family_maintenance_api.cpp"),
        Path("project/src/engine/internal_api/nosql/nosql_backpressure_debt_api.cpp"),
        Path("project/tests/agents/agent_enterprise_nosql_agents_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_nosql_route_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    maintenance_cpp = read_text(repo_root, Path("project/src/core/agents/agents/nosql_family_maintenance_agent.cpp"))
    debt_cpp = read_text(repo_root, Path("project/src/core/agents/agents/nosql_backpressure_debt_agent.cpp"))
    maintenance_api = read_text(repo_root, Path("project/src/engine/internal_api/nosql/nosql_family_maintenance_api.cpp"))
    debt_api = read_text(repo_root, Path("project/src/engine/internal_api/nosql/nosql_backpressure_debt_api.cpp"))
    helper_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_nosql_agents_gate.cpp"))
    route_gate = read_text(repo_root, Path("project/tests/agents/agent_enterprise_nosql_route_gate.cpp"))

    required_tokens = (
        (maintenance_cpp, "durable_mga_transaction_inventory", "maintenance MGA authority evidence"),
        (maintenance_cpp, "DynamicCleanupDebtScheduler", "maintenance cleanup scheduler consumption"),
        (maintenance_cpp, "SB_AGENT_IMPLEMENTATION_nosql_family_maintenance_agent", "maintenance implementation anchor"),
        (debt_cpp, "engine_owned_request_context_and_mga_evidence", "backpressure authority evidence"),
        (debt_cpp, "provider_transaction_finality_authority", "backpressure provider non-authority evidence"),
        (debt_cpp, "SB_AGENT_IMPLEMENTATION_nosql_backpressure_debt_agent", "backpressure implementation anchor"),
        (maintenance_api, "SB_ENGINE_INTERNAL_API_NOSQL_FAMILY_MAINTENANCE_API_BEHAVIOR", "maintenance engine API behavior"),
        (maintenance_api, "EngineNoSqlClusterAuthorityUnavailable", "maintenance cluster fail-closed route"),
        (maintenance_api, "behavior_store_scan_selected\", \"false", "maintenance no behavior-store scan evidence"),
        (debt_api, "SB_ENGINE_INTERNAL_API_NOSQL_BACKPRESSURE_DEBT_API_BEHAVIOR", "backpressure engine API behavior"),
        (debt_api, "EngineNoSqlClusterAuthorityUnavailable", "backpressure cluster fail-closed route"),
        (debt_api, "result_returned\", \"false", "backpressure suppression no-result evidence"),
        (helper_gate, "TestNoSqlHelpersAreNotCanonicalProductionAgents", "helper noncanonical exposure gate"),
        (route_gate, "EnginePlanNoSqlFamilyMaintenance", "maintenance engine route gate"),
        (route_gate, "EnginePlanNoSqlBackpressureDebt", "backpressure engine route gate"),
        (route_gate, "cluster_authority_required", "cluster route refusal test"),
        (route_gate, "provider_claims_transaction_finality_authority", "provider finality refusal test"),
    )
    for text, token, description in required_tokens:
        if token not in text:
            errors.append(f"AEIC-027 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_nosql_route_gate" not in tests_cmake:
        errors.append("agent_enterprise_nosql_route_gate is not registered in agent test CMake")
    if "agent_enterprise_nosql_route_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_nosql_route_audit_gate is not registered in agent test CMake")
    if "AEIC-027" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-027 NoSQL label")

    return errors


def gate_enterprise_operational_backup_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/backup_manager.cpp"),
        Path("project/src/core/agents/agents/archive_manager.cpp"),
        Path("project/src/core/agents/agents/restore_drill_manager.cpp"),
        Path("project/src/core/agents/agents/pitr_manager.cpp"),
        Path("project/src/core/agents/agents/export_adapter_manager.cpp"),
        Path("project/src/core/agents/agent_local_workflow.cpp"),
        Path("project/src/engine/internal_api/backup_archive/backup_archive_api.cpp"),
        Path("project/tests/agents/agent_enterprise_operational_agents_gate.cpp"),
        Path("project/tests/agents/agent_local_workflow_store_gate.cpp"),
        Path("project/tests/database_lifecycle/backup_archive_restore_conformance.cpp"),
        Path("project/tests/database_lifecycle/backup_restore_export_admin_gate_conformance.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    files = {
        "backup": read_text(repo_root, Path("project/src/core/agents/agents/backup_manager.cpp")),
        "archive": read_text(repo_root, Path("project/src/core/agents/agents/archive_manager.cpp")),
        "restore": read_text(repo_root, Path("project/src/core/agents/agents/restore_drill_manager.cpp")),
        "pitr": read_text(repo_root, Path("project/src/core/agents/agents/pitr_manager.cpp")),
        "export": read_text(repo_root, Path("project/src/core/agents/agents/export_adapter_manager.cpp")),
        "workflow": read_text(repo_root, Path("project/src/core/agents/agent_local_workflow.cpp")),
        "backup_api": read_text(repo_root, Path("project/src/engine/internal_api/backup_archive/backup_archive_api.cpp")),
        "operational_gate": read_text(repo_root, Path("project/tests/agents/agent_enterprise_operational_agents_gate.cpp")),
        "workflow_gate": read_text(repo_root, Path("project/tests/agents/agent_local_workflow_store_gate.cpp")),
        "backup_gate": read_text(repo_root, Path("project/tests/database_lifecycle/backup_archive_restore_conformance.cpp")),
        "admin_gate": read_text(repo_root, Path("project/tests/database_lifecycle/backup_restore_export_admin_gate_conformance.cpp")),
    }

    required_tokens = (
        ("backup", "AgentLocalWorkflowDomain::backup", "backup local workflow domain"),
        ("backup", "SB_AGENT_BACKUP_AUTHORITY_UNTRUSTED", "backup authority refusal"),
        ("backup", "cluster_route_requested", "backup cluster route refusal"),
        ("archive", "AgentLocalWorkflowDomain::archive", "archive local workflow domain"),
        ("archive", "legal_hold", "archive legal-hold handling"),
        ("restore", "AgentLocalWorkflowDomain::restore_drill", "restore drill local workflow domain"),
        ("restore", "restore_inspection_required", "restore inspection evidence"),
        ("pitr", "AgentLocalWorkflowDomain::pitr", "PITR local workflow domain"),
        ("pitr", "recovery_authority", "PITR recovery non-authority evidence"),
        ("export", "AgentLocalWorkflowDomain::export_adapter", "export adapter workflow domain"),
        ("export", "redaction_policy_valid", "export redaction policy proof"),
        ("workflow", "agent_local_workflow_v1", "durable local workflow store"),
        ("backup_api", "EngineStartLogicalBackup", "backup archive API"),
        ("backup_api", "authoritative_wal\", \"false", "backup anti-WAL authority evidence"),
        ("backup_api", "RESTORE_LIVE_TARGET_FORBIDDEN", "restore live-target refusal"),
        ("operational_gate", "TestBackupArchiveRestorePitrExport", "operational agent workflow gate"),
        ("operational_gate", "ValidateDurableAgentCatalogForProduction", "durable catalog validation"),
        ("workflow_gate", "TestLocalWorkflowPersistsAndReplaysFromStore", "store-backed workflow gate"),
        ("backup_gate", "HasEncodedManifestField", "backup archive restore conformance"),
        ("admin_gate", "management.prepare_support_bundle", "admin support/export route conformance"),
    )
    for key, token, description in required_tokens:
        if token not in files[key]:
            errors.append(f"AEIC-028 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_enterprise_operational_agents_gate" not in tests_cmake:
        errors.append("agent_enterprise_operational_agents_gate is not registered")
    if "agent_enterprise_operational_backup_audit_gate" not in tests_cmake:
        errors.append("agent_enterprise_operational_backup_audit_gate is not registered")
    if "AEIC-028" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-028 operational label")

    return errors


def gate_enterprise_identity_session_job_audit(repo_root: Path) -> list[str]:
    rel_inputs = (
        Path("project/src/core/agents/agents/identity_manager.cpp"),
        Path("project/src/core/agents/agents/session_control_manager.cpp"),
        Path("project/src/core/agents/agents/job_control_manager.cpp"),
        Path("project/src/core/agents/agent_background_jobs.cpp"),
        Path("project/src/server/agent_session_control_route_bridge.cpp"),
        Path("project/src/engine/internal_api/security/identity_api.cpp"),
        Path("project/tests/agents/agent_enterprise_identity_session_job_route_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_operational_agents_gate.cpp"),
        AGENT_TESTS_CMAKE_REL,
    )
    errors: list[str] = []
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    files = {
        "identity": read_text(repo_root, Path("project/src/core/agents/agents/identity_manager.cpp")),
        "session": read_text(repo_root, Path("project/src/core/agents/agents/session_control_manager.cpp")),
        "job": read_text(repo_root, Path("project/src/core/agents/agents/job_control_manager.cpp")),
        "scheduler": read_text(repo_root, Path("project/src/core/agents/agent_background_jobs.cpp")),
        "server_bridge": read_text(repo_root, Path("project/src/server/agent_session_control_route_bridge.cpp")),
        "identity_api": read_text(repo_root, Path("project/src/engine/internal_api/security/identity_api.cpp")),
        "route_gate": read_text(repo_root, Path("project/tests/agents/agent_enterprise_identity_session_job_route_gate.cpp")),
        "operational_gate": read_text(repo_root, Path("project/tests/agents/agent_enterprise_operational_agents_gate.cpp")),
        "tests_cmake": read_text(repo_root, AGENT_TESTS_CMAKE_REL),
    }

    required_tokens = (
        ("identity", "AgentLocalWorkflowDomain::identity", "identity local workflow domain"),
        ("identity", "SB_AGENT_IDENTITY_AUTHORITY_UNTRUSTED", "identity authority refusal"),
        ("session", "AgentLocalWorkflowDomain::session_control", "session local workflow domain"),
        ("session", "SB_AGENT_SESSION_CONTROL_AUTHORITY_UNTRUSTED", "session authority refusal"),
        ("job", "AgentLocalWorkflowDomain::job_control", "job local workflow domain"),
        ("job", "SB_AGENT_JOB_CONTROL_AUTHORITY_UNTRUSTED", "job authority refusal"),
        ("scheduler", "CancelJobControlAction", "job cancel scheduler actuator"),
        ("scheduler", "RetryJobControlAction", "job retry scheduler actuator"),
        ("scheduler", "SuppressJobControlAction", "job suppress scheduler actuator"),
        ("server_bridge", "AEIC_SESSION_CONTROL_SERVER_ROUTE_BRIDGE", "server session-control bridge"),
        ("server_bridge", "server_session_registry_removed=true", "server session registry mutation evidence"),
        ("identity_api", "SB_ENGINE_INTERNAL_API_SECURITY_IDENTITY_API_BEHAVIOR", "engine identity API behavior"),
        ("route_gate", "TestIdentityManagerRoutesThroughEngineSecurityApi", "identity route gate"),
        ("route_gate", "TestSessionControlMutatesServerRegistry", "session route gate"),
        ("route_gate", "TestJobControlMutatesBackgroundScheduler", "job route gate"),
        ("operational_gate", "TestIdentitySessionJob", "operational identity/session/job workflow gate"),
        ("tests_cmake", "agent_enterprise_identity_session_job_route_gate", "identity/session/job route test registration"),
        ("tests_cmake", "agent_enterprise_identity_session_job_audit_gate", "identity/session/job audit test registration"),
    )
    for key, token, description in required_tokens:
        if token not in files[key]:
            errors.append(f"AEIC-029 missing {description}: {token}")
    return errors


def gate_enterprise_per_agent_route_matrix(repo_root: Path) -> list[str]:
    errors: list[str] = []
    rel_inputs = (
        AGENT_RUNTIME_MANIFEST_DEF_REL,
        AGENT_TESTS_CMAKE_REL,
        Path("project/tests/agents/agent_enterprise_local_resource_agents_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_storage_agents_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_cleanup_archive_agents_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_memory_admission_alert_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_index_agents_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_transaction_optimizer_policy_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_parser_support_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_nosql_route_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_operational_agents_gate.cpp"),
        Path("project/tests/agents/agent_enterprise_identity_session_job_route_gate.cpp"),
        Path("project/tests/agents/agent_production_exposure_gate.cpp"),
        Path("project/tests/agents/agent_cluster_provider_boundary_gate.cpp"),
        Path("project/tests/agents/agent_cluster_route_api_provider_gate.cpp"),
        Path("project/tests/agents/agent_management_route_evidence_gate.cpp"),
        Path("project/src/core/agents/agent_local_workflow.cpp"),
        Path("project/src/core/agents/agent_metric_runtime.cpp"),
        Path("project/src/core/agents/agent_cluster_boundary.cpp"),
        Path("project/src/core/agents/agent_enterprise_evidence.cpp"),
        Path("project/src/server/agent_session_control_route_bridge.cpp"),
        Path("project/src/parsers/sbsql_worker/lifecycle/agent_parser_interface_bridge.cpp"),
        Path("project/src/engine/internal_api/agents/agent_support_bundle_triage_route_api.cpp"),
    )
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "AEIC_PER_AGENT_ROUTE_TESTS" not in tests_cmake:
        errors.append("agent tests CMake lacks AEIC_PER_AGENT_ROUTE_TESTS search key")

    combined = tests_cmake
    for path in (repo_root / "project/tests/agents").glob("*.cpp"):
      combined += "\n" + path.read_text(encoding="utf-8", errors="replace")
    for rel_path in rel_inputs:
        if rel_path == AGENT_TESTS_CMAKE_REL:
            continue
        combined += "\n" + read_text(repo_root, rel_path)
    manifest = parse_manifest_def(repo_root)
    for agent_type, entry in sorted(manifest.items()):
        if agent_type not in combined:
            errors.append(f"AEIC-040 missing per-agent test coverage token: {agent_type}")
        if entry.deployment == "cluster":
            if "agent_cluster_provider_boundary_gate" not in combined or \
               "agent_cluster_route_api_provider_gate" not in combined:
                errors.append(f"AEIC-040 cluster route boundary gate missing for: {agent_type}")
        else:
            if "enterprise_no_anchor" not in combined:
                errors.append(f"AEIC-040 enterprise no-anchor coverage missing for: {agent_type}")

    required_test_registrations = (
        "agent_enterprise_local_resource_agents_gate",
        "agent_enterprise_storage_agents_gate",
        "agent_enterprise_cleanup_archive_agents_gate",
        "agent_enterprise_memory_admission_alert_gate",
        "agent_enterprise_index_agents_gate",
        "agent_enterprise_transaction_optimizer_policy_gate",
        "agent_enterprise_parser_support_gate",
        "agent_enterprise_nosql_route_gate",
        "agent_enterprise_operational_agents_gate",
        "agent_enterprise_identity_session_job_route_gate",
        "agent_enterprise_per_agent_route_matrix_gate",
        "agent_management_route_evidence_gate",
        "agent_production_exposure_gate",
        "agent_cluster_provider_boundary_gate",
        "agent_cluster_route_api_provider_gate",
    )
    for test_name in required_test_registrations:
        if test_name not in tests_cmake:
            errors.append(f"AEIC-040 missing route matrix test registration: {test_name}")

    required_behavior_tokens = (
        "ValidateDurableAgentCatalogForProduction",
        "EvaluateStrictMetricEvidence",
        "CLUSTER.EXTERNAL_PROVIDER_REQUIRED",
        "AEIC_SESSION_CONTROL_SERVER_ROUTE_BRIDGE",
        "AEIC_SUPPORT_BUNDLE_TRIAGE_ROUTE_API",
        "AEIC_PARSER_INTERFACE_LIFECYCLE_BRIDGE",
        "SB_AGENT_LOCAL_WORKFLOW.UNTRUSTED_AUTHORITY",
        "SB_AGENT_METRIC_SNAPSHOT.CLUSTER_AUTHORITY_REQUIRED",
    )
    for token in required_behavior_tokens:
        if token not in combined:
            errors.append(f"AEIC-040 missing route/refusal behavior token: {token}")
    return errors


def gate_manifest_drift(repo_root: Path) -> list[str]:
    errors: list[str] = []
    rel_inputs = (
        AGENT_RUNTIME_MANIFEST_DEF_REL,
        AGENT_RUNTIME_MANIFEST_CPP_REL,
        AGENT_RUNTIME_CPP_REL,
        AGENTS_CMAKE_REL,
        AGENT_TESTS_CMAKE_REL,
        AEIC_SCOPE_MATRIX_REL,
        AEIC_STATUS_MATRIX_REL,
        AEIC_TRACEABILITY_MATRIX_REL,
    )
    for rel_path in rel_inputs:
        if not (repo_root / resolve_repo_rel(repo_root, rel_path)).exists():
            errors.append(f"required manifest drift input missing: {rel_path}")
    if errors:
        return errors

    try:
        manifest = parse_manifest_def(repo_root)
    except GateFailure as exc:
        return [str(exc)]
    impl_registry = parse_impl_registry(repo_root)
    non_cluster_scope, noncanonical_scope, cluster_scope = scope_matrix_sets(repo_root)
    cmake_sources = cmake_agent_sources(repo_root)

    if len(manifest) != 29:
        errors.append(f"manifest definition count drifted: {len(manifest)}")
    if set(impl_registry) != set(manifest):
        errors.append(
            "runtime registry source does not match manifest definition: "
            f"manifest_only={sorted(set(manifest) - set(impl_registry))} "
            f"registry_only={sorted(set(impl_registry) - set(manifest))}"
        )

    manifest_cpp = read_text(repo_root, AGENT_RUNTIME_MANIFEST_CPP_REL)
    if '#include "agent_runtime_manifest.def"' not in manifest_cpp:
        errors.append("CanonicalAgentManifest() does not include agent_runtime_manifest.def")
    if 'Entry("' in manifest_cpp:
        errors.append("agent_runtime_manifest.cpp still contains duplicated literal Entry rows")

    for agent, entry in sorted(manifest.items()):
        source_rel = Path("project/src/core/agents/agents") / f"{agent}.cpp"
        if not (repo_root / source_rel).exists():
            errors.append(f"{agent}: manifest implementation source missing: {source_rel}")
        if agent not in cmake_sources:
            errors.append(f"{agent}: manifest source is not compiled by sb_core_agents")
        if entry.deployment == "cluster":
            if agent not in cluster_scope:
                errors.append(f"{agent}: cluster manifest entry missing from AEIC cluster scope matrix")
            if entry.activation != "disabled":
                errors.append(f"{agent}: cluster manifest entry is not disabled by default")
            source_text = (repo_root / source_rel).read_text(encoding="utf-8", errors="replace")
            lowered = source_text.lower()
            if "external" not in lowered or "cluster provider" not in lowered:
                errors.append(f"{agent}: cluster source lacks external provider boundary wording")
        else:
            if agent not in non_cluster_scope:
                errors.append(f"{agent}: non-cluster manifest entry missing from AEIC non-cluster scope matrix")

    for agent in sorted(non_cluster_scope):
        if agent not in manifest:
            errors.append(f"{agent}: AEIC non-cluster scope row missing from manifest definition")
        elif manifest[agent].deployment == "cluster":
            errors.append(f"{agent}: AEIC non-cluster scope row is marked cluster in manifest")
    for agent in sorted(cluster_scope):
        if agent not in manifest:
            errors.append(f"{agent}: AEIC cluster scope row missing from manifest definition")
        elif manifest[agent].deployment != "cluster":
            errors.append(f"{agent}: AEIC cluster scope row is not cluster in manifest")

    for agent in sorted(cmake_sources):
        if agent in manifest:
            continue
        if agent not in noncanonical_scope:
            errors.append(f"{agent}: compiled noncanonical agent lacks AEIC scope disposition")
        source_text = (repo_root / AGENT_IMPL_DIR_REL / f"{agent}.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        if "CanonicalAgentRegistry" in source_text:
            errors.append(f"{agent}: noncanonical helper points itself at canonical registry")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    if "agent_manifest_drift_gate" not in tests_cmake:
        errors.append("agent_manifest_drift_gate is not registered in agent test CMake")
    if "AEIC-051" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-051 drift-gate label")
    if "AEIC-051" not in read_text(repo_root, AEIC_STATUS_MATRIX_REL):
        errors.append("AEIC status matrix lacks AEIC-051 row")
    if "AEIC-AUDIT-051" not in read_text(repo_root, AEIC_TRACEABILITY_MATRIX_REL):
        errors.append("AEIC traceability matrix lacks AEIC-AUDIT-051 row")

    return errors


def gate_enterprise_substrate_refactor(repo_root: Path) -> list[str]:
    # SEARCH_KEY: AEIC_AGENT_RUNTIME_SUBSTRATE_REFACTOR_COMPLETE
    errors: list[str] = []
    rel_inputs = (
        AGENTS_CMAKE_REL,
        Path("project/src/core/agents/agent_runtime_manifest.cpp"),
        Path("project/src/core/agents/agent_runtime_manifest.def"),
        Path("project/src/core/agents/agent_policy_schema.cpp"),
        Path("project/src/core/agents/agent_enterprise_evidence.cpp"),
        Path("project/src/core/agents/agent_commercial_evidence.cpp"),
        Path("project/src/core/agents/agent_action_dispatch.cpp"),
        Path("project/src/core/agents/agent_production_fixture_separation.cpp"),
        Path("project/src/core/agents/agent_runtime_service.cpp"),
        Path("project/src/core/agents/agent_metric_runtime.cpp"),
        Path("project/src/core/agents/resource_governance_admission.cpp"),
        Path("project/src/core/agents/agent_durable_catalog.cpp"),
        Path("project/src/core/agents/agent_cluster_boundary.cpp"),
        Path("project/src/core/agents/agent_feature_gates.cpp"),
        Path("project/src/core/agents/agent_engine_lifecycle.cpp"),
        Path("project/src/core/agents/agent_production_classification.cpp"),
        Path("project/src/core/agents/agent_optimizer_recommendation.cpp"),
    )
    require_files(repo_root, rel_inputs, errors)
    if errors:
        return errors

    agents_cmake = read_text(repo_root, AGENTS_CMAKE_REL)
    required_compiled_sources = (
        "agent_runtime_manifest.cpp",
        "agent_policy_schema.cpp",
        "agent_enterprise_evidence.cpp",
        "agent_commercial_evidence.cpp",
        "agent_action_dispatch.cpp",
        "agent_production_fixture_separation.cpp",
        "agent_runtime_service.cpp",
        "agent_metric_runtime.cpp",
        "agent_durable_catalog.cpp",
        "agent_engine_lifecycle.cpp",
        "agent_feature_gates.cpp",
        "agent_background_jobs.cpp",
        "agent_production_classification.cpp",
        "agent_optimizer_recommendation.cpp",
    )
    for source in required_compiled_sources:
        if source not in agents_cmake:
            errors.append(f"AEIC-050 compiled substrate source missing from sb_core_agents: {source}")
    if "resource_governance_admission.cpp" not in agents_cmake:
        errors.append("AEIC-050 resource governance substrate source missing from sb_core_resource_governance")

    module_expectations = (
        (Path("project/src/core/agents/agent_runtime_manifest.cpp"), '#include "agent_runtime_manifest.def"', "manifest include"),
        (Path("project/src/core/agents/agent_runtime.cpp"), '#include "agent_runtime_manifest.hpp"', "runtime manifest dependency"),
        (Path("project/src/core/agents/agent_runtime.cpp"), '#include "agent_policy_schema.hpp"', "runtime typed policy dependency"),
        (Path("project/src/core/agents/agent_action_dispatch.cpp"), "AgentActuatorProviderRegistry", "actuator registry substrate"),
        (Path("project/src/core/agents/agent_enterprise_evidence.cpp"), "AppendEnterpriseAgentDecisionEvidence", "enterprise evidence substrate"),
        (Path("project/src/core/agents/agent_metric_runtime.cpp"), "EvaluateAgentObservedMetricSnapshots", "strict metric substrate"),
        (Path("project/src/core/agents/agent_runtime_service.cpp"), "AgentRuntimeService", "runtime service substrate"),
        (Path("project/src/core/agents/agent_durable_catalog.cpp"), "DurableAgentCatalogImage", "durable catalog substrate"),
        (Path("project/src/core/agents/agent_cluster_boundary.cpp"), "CLUSTER.EXTERNAL_PROVIDER_REQUIRED", "cluster boundary substrate"),
        (Path("project/src/core/agents/agent_production_classification.cpp"), "ClassifyCanonicalAgentProductionExposure", "production classification substrate"),
    )
    for rel_path, token, description in module_expectations:
        if token not in read_text(repo_root, rel_path):
            errors.append(f"AEIC-050 missing {description}: {token}")

    tests_cmake = read_text(repo_root, AGENT_TESTS_CMAKE_REL)
    required_gates = (
        "agent_policy_schema_gate",
        "agent_runtime_manifest_consistency_gate",
        "agent_durable_runtime_catalog_gate",
        "agent_enterprise_durable_catalog_store_gate",
        "agent_runtime_service_store_gate",
        "agent_action_dispatch_store_gate",
        "agent_metric_resource_runtime_gate",
        "agent_commercial_evidence_gate",
        "agent_management_durable_mutation_gate",
        "agent_enterprise_substrate_refactor_gate",
    )
    for gate in required_gates:
        if gate not in tests_cmake:
            errors.append(f"AEIC-050 missing substrate regression gate registration: {gate}")
    if "AEIC-050" not in tests_cmake:
        errors.append("agent test CMake lacks AEIC-050 substrate-refactor label")
    return errors


def run_gate(repo_root: Path, mode: str) -> list[str]:
    if mode == "no-implicit-defaults":
        return gate_no_implicit_defaults(repo_root)
    if mode == "management-framework":
        return gate_management_framework(repo_root)
    if mode == "responsibility-index":
        return gate_responsibility_index(repo_root)
    if mode == "implementation-anchors":
        return gate_implementation_anchors(repo_root)
    if mode == "enterprise-production-separation":
        return gate_enterprise_production_separation(repo_root)
    if mode == "enterprise-metrics-resource-audit":
        return gate_enterprise_metrics_resource_audit(repo_root)
    if mode == "enterprise-management-surfaces-audit":
        return gate_enterprise_management_surfaces_audit(repo_root)
    if mode == "enterprise-node-metrics-audit":
        return gate_enterprise_node_metrics_audit(repo_root)
    if mode == "enterprise-storage-agents-audit":
        return gate_enterprise_storage_agents_audit(repo_root)
    if mode == "enterprise-cleanup-archive-audit":
        return gate_enterprise_cleanup_archive_audit(repo_root)
    if mode == "enterprise-memory-admission-alert-audit":
        return gate_enterprise_memory_admission_alert_audit(repo_root)
    if mode == "enterprise-index-agents-audit":
        return gate_enterprise_index_agents_audit(repo_root)
    if mode == "enterprise-transaction-optimizer-policy-audit":
        return gate_enterprise_transaction_optimizer_policy_audit(repo_root)
    if mode == "enterprise-parser-support-audit":
        return gate_enterprise_parser_support_audit(repo_root)
    if mode == "enterprise-nosql-route-audit":
        return gate_enterprise_nosql_route_audit(repo_root)
    if mode == "enterprise-operational-backup-audit":
        return gate_enterprise_operational_backup_audit(repo_root)
    if mode == "enterprise-identity-session-job-audit":
        return gate_enterprise_identity_session_job_audit(repo_root)
    if mode == "enterprise-per-agent-route-matrix":
        return gate_enterprise_per_agent_route_matrix(repo_root)
    if mode == "manifest-drift":
        return gate_manifest_drift(repo_root)
    if mode == "enterprise-substrate-refactor":
        return gate_enterprise_substrate_refactor(repo_root)
    raise GateFailure(f"unknown mode: {mode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument(
        "--mode",
        required=True,
        choices=("no-implicit-defaults", "management-framework", "responsibility-index", "implementation-anchors", "enterprise-production-separation", "enterprise-metrics-resource-audit", "enterprise-management-surfaces-audit", "enterprise-node-metrics-audit", "enterprise-storage-agents-audit", "enterprise-cleanup-archive-audit", "enterprise-memory-admission-alert-audit", "enterprise-index-agents-audit", "enterprise-transaction-optimizer-policy-audit", "enterprise-parser-support-audit", "enterprise-nosql-route-audit", "enterprise-operational-backup-audit", "enterprise-identity-session-job-audit", "enterprise-per-agent-route-matrix", "manifest-drift", "enterprise-substrate-refactor"),
    )
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    errors = run_gate(repo_root, args.mode)
    if errors:
        for error in errors:
            print(f"AGENT_RUNTIME_STATIC_GATE[{args.mode}]=failed: {error}", file=sys.stderr)
        return 1
    print(f"AGENT_RUNTIME_STATIC_GATE[{args.mode}]=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
