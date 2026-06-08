// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace info = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

const std::vector<std::string> kDirectViews = {
    "sys.agents",
    "sys.agent_metric_dependencies",
    "sys.agent_policies",
    "sys.agent_actions",
    "sys.agent_overrides",
    "sys.agent_evidence",
    "sys.agent_audit",
    "sys.filespace_capacity_agent_state",
    "sys.page_allocation_agent_state",
    "sys.filespace_shrink_readiness",
};

info::SysInformationProjectionContext Context() {
  info::SysInformationProjectionContext context;
  context.catalog_display_name = "ManagementDB";
  context.session_language = "en";
  context.default_language = "en";
  context.visible_catalog_generation_id = 4;
  context.cluster_authority_available = false;
  return context;
}

std::string Field(const info::SysInformationProjectionRow& row,
                  std::string_view name) {
  for (const auto& [field_name, value] : row.fields) {
    if (field_name == name) { return value; }
  }
  return {};
}

bool HasColumn(const info::SysInformationProjectionDefinition& definition,
               std::string_view name,
               std::string_view logical_type = {}) {
  for (const auto& column : definition.columns) {
    if (column.column_name == name &&
        (logical_type.empty() || column.logical_type == logical_type)) {
      return true;
    }
  }
  return false;
}

bool HasRowValue(const info::SysInformationProjectionResult& result,
                 std::string_view field_name,
                 std::string_view value) {
  for (const auto& row : result.rows) {
    if (Field(row, field_name) == value) { return true; }
  }
  return false;
}

void RequireOk(const info::SysInformationProjectionResult& result,
               const std::string& message) {
  if (!result.ok) {
    std::cerr << result.diagnostic_code << " " << result.diagnostic_detail << '\n';
  }
  Require(result.ok, message);
}

void RequireNoRawUuidLeak(const info::SysInformationProjectionResult& result) {
  for (const auto& row : result.rows) {
    for (const auto& [field_name, value] : row.fields) {
      if (field_name.size() >= 5 &&
          field_name.substr(field_name.size() - 5) == "_uuid") {
        Require(value.rfind("agent.", 0) != 0,
                "fake agent reference leaked in " + field_name + "=" + value);
        Require(value.rfind("policy.", 0) != 0,
                "fake policy reference leaked in " + field_name + "=" + value);
        Require(value.rfind("scope.", 0) != 0,
                "fake scope reference leaked in " + field_name + "=" + value);
      }
      Require(value.find("raw-principal") == std::string::npos,
              "raw principal token leaked in " + field_name);
      Require(value.find("/dev/scratchbird") == std::string::npos,
              "physical path leaked in " + field_name);
      Require(value.find("blocker-principal") == std::string::npos,
              "blocker detail leaked in " + field_name);
    }
  }
}

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) { return found->second; }

  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1915016000000ull + seed);
  Require(generated.ok(), "fixture UUID generation failed");
  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated.value.value));
  return inserted->second;
}

std::vector<info::SysInformationAgentSource> Agents() {
  return {
      {.agent_uuid = Id(platform::UuidKind::object, 1),
       .agent_ref = "agent.display-only",
       .agent_name = "page_allocation_manager",
       .agent_type_id = "page_allocation_manager",
       .scope_kind = "database",
       .scope_uuid = Id(platform::UuidKind::database, 2),
       .scope_ref = "scope.display-only",
       .component = "storage.pages",
       .state = "running",
       .health_state = "healthy",
       .enabled = "YES",
       .policy_uuid = Id(platform::UuidKind::object, 3),
       .policy_ref = "policy.display-only",
       .policy_name = "page_allocation_default",
       .last_transition_at = "2026-05-21T12:00:00Z",
       .last_diagnostic_code = "AGENT.NONE",
       .catalog_generation_id = 2},
      {.agent_uuid = Id(platform::UuidKind::object, 11),
       .agent_ref = Id(platform::UuidKind::object, 11),
       .agent_name = "hidden_agent",
       .agent_type_id = "hidden_agent",
       .catalog_generation_id = 2,
       .hidden = true},
      {.agent_uuid = Id(platform::UuidKind::object, 12),
       .agent_ref = Id(platform::UuidKind::object, 12),
       .agent_name = "future_agent",
       .agent_type_id = "future_agent",
       .catalog_generation_id = 9},
  };
}

std::vector<info::SysInformationAgentMetricDependencySource> MetricDependencies() {
  return {
      {.agent_uuid = Id(platform::UuidKind::object, 1),
       .agent_ref = Id(platform::UuidKind::object, 1),
       .metric_family = "sys.metrics.storage.pages",
       .metric_namespace = "sys.metrics.storage.pages",
       .required_or_optional = "required",
       .freshness_limit = "15s",
       .current_freshness = "3s",
       .quality_state = "fresh",
       .fail_behavior = "fail_closed",
       .catalog_generation_id = 2,
       .metric_values_visible = false},
  };
}

std::vector<info::SysInformationAgentPolicySource> Policies() {
  return {
      {.agent_uuid = Id(platform::UuidKind::object, 1),
       .agent_ref = Id(platform::UuidKind::object, 1),
       .policy_uuid = Id(platform::UuidKind::object, 3),
       .policy_ref = Id(platform::UuidKind::object, 3),
       .policy_name = "page_allocation_default",
       .policy_family = "page_allocation_policy",
       .version_uuid = Id(platform::UuidKind::object, 4),
       .version_ref = Id(platform::UuidKind::object, 4),
       .active_state = "active",
       .validation_state = "valid",
       .attached_at = "2026-05-21T12:00:01Z",
       .attached_by = "system",
       .catalog_generation_id = 2},
  };
}

std::vector<info::SysInformationAgentActionSource> Actions() {
  return {
      {.action_uuid = Id(platform::UuidKind::object, 5),
       .action_ref = Id(platform::UuidKind::object, 5),
       .agent_uuid = Id(platform::UuidKind::object, 1),
       .agent_ref = Id(platform::UuidKind::object, 1),
       .action_id = "request_page_preallocation",
       .state = "recommended",
       .risk_class = "bounded_storage",
       .created_at = "2026-05-21T12:00:02Z",
       .expires_at = "2026-05-21T12:05:02Z",
       .approval_required = "YES",
       .actor_uuid = "raw-principal-should-not-leak",
       .actor_ref = "principal.operator",
       .diagnostic_code = "AGENT.ACTION_READY",
       .catalog_generation_id = 2,
       .actor_visible = false},
  };
}

std::vector<info::SysInformationAgentOverrideSource> Overrides() {
  return {
      {.override_uuid = Id(platform::UuidKind::object, 6),
       .override_ref = Id(platform::UuidKind::object, 6),
       .target_uuid = Id(platform::UuidKind::object, 5),
       .target_ref = Id(platform::UuidKind::object, 5),
       .scope_uuid = Id(platform::UuidKind::database, 2),
       .scope_ref = Id(platform::UuidKind::database, 2),
       .suppression_class = "maintenance_window",
       .starts_at = "2026-05-21T12:00:00Z",
       .expires_at = "2026-05-21T13:00:00Z",
       .state = "active",
       .reason_code = "operator_suppression",
       .created_by = "raw-principal-should-not-leak",
       .created_by_ref = "principal.operator",
       .catalog_generation_id = 2,
       .actor_visible = false},
  };
}

std::vector<info::SysInformationAgentEvidenceSource> Evidence() {
  return {
      {.evidence_uuid = Id(platform::UuidKind::object, 7),
       .evidence_ref = Id(platform::UuidKind::object, 7),
       .agent_uuid = Id(platform::UuidKind::object, 1),
       .agent_ref = Id(platform::UuidKind::object, 1),
       .evidence_type = "page_preallocation",
       .action_uuid = Id(platform::UuidKind::object, 5),
       .action_ref = Id(platform::UuidKind::object, 5),
       .redaction_class = "summary",
       .created_at = "2026-05-21T12:00:03Z",
       .actor_uuid = "raw-principal-should-not-leak",
       .actor_ref = "principal.operator",
       .payload_digest = "sha256:012345",
       .payload_redacted = "YES",
       .catalog_generation_id = 2,
       .actor_visible = false},
  };
}

std::vector<info::SysInformationAgentAuditSource> Audit() {
  return {
      {.audit_uuid = Id(platform::UuidKind::object, 8),
       .audit_ref = Id(platform::UuidKind::object, 8),
       .evidence_uuid = Id(platform::UuidKind::object, 7),
       .evidence_ref = Id(platform::UuidKind::object, 7),
       .actor_uuid = "raw-principal-should-not-leak",
       .actor_ref = "principal.operator",
       .command_name = "REQUEST PAGE PREALLOCATION",
       .sblr_operation = "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION",
       .api_call = "EngineRequestPagePreallocation",
       .result_state = "completed",
       .diagnostic_code = "ok",
       .created_at = "2026-05-21T12:00:04Z",
       .catalog_generation_id = 2,
       .actor_visible = false},
  };
}

std::vector<info::SysInformationFilespaceCapacityAgentStateSource> FilespaceState() {
  return {
      {.agent_uuid = Id(platform::UuidKind::object, 9),
       .agent_ref = Id(platform::UuidKind::object, 9),
       .filespace_uuid = Id(platform::UuidKind::filespace, 20),
       .filespace_ref = Id(platform::UuidKind::filespace, 20),
       .policy_uuid = Id(platform::UuidKind::object, 21),
       .policy_ref = Id(platform::UuidKind::object, 21),
       .mode = "active",
       .health_state = "healthy",
       .last_capacity_metric_at = "2026-05-21T12:00:05Z",
       .last_health_metric_at = "2026-05-21T12:00:06Z",
       .last_recommendation_code = "FILESPACE.GROWTH_READY",
       .last_refusal_code = "",
       .catalog_generation_id = 2},
  };
}

std::vector<info::SysInformationPageAllocationAgentStateSource> PageState() {
  return {
      {.agent_uuid = Id(platform::UuidKind::object, 1),
       .agent_ref = Id(platform::UuidKind::object, 1),
       .filespace_uuid = Id(platform::UuidKind::filespace, 20),
       .filespace_ref = Id(platform::UuidKind::filespace, 20),
       .page_family = "data",
       .page_type = "relation",
       .policy_uuid = Id(platform::UuidKind::object, 3),
       .policy_ref = Id(platform::UuidKind::object, 3),
       .mode = "active",
       .last_scan_generation = "42",
       .last_shrink_ready_state = "ready",
       .last_refusal_code = "",
       .catalog_generation_id = 2},
  };
}

std::vector<info::SysInformationFilespaceShrinkReadinessSource> ShrinkReadiness() {
  return {
      {.filespace_uuid = Id(platform::UuidKind::filespace, 20),
       .filespace_ref = Id(platform::UuidKind::filespace, 20),
       .safe_start_byte = "0",
       .safe_end_byte = "1048576",
       .truncate_ready_bytes = "65536",
       .blocker_count = "0",
       .readiness_state = "ready",
       .scan_generation = "42",
       .evidence_uuid = Id(platform::UuidKind::object, 22),
       .evidence_ref = Id(platform::UuidKind::object, 22),
       .catalog_generation_id = 2},
  };
}

info::SysInformationProjectionResult Build(std::string_view view_path) {
  return info::BuildSysInformationProjection(
      view_path,
      Context(),
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      Agents(),
      MetricDependencies(),
      Policies(),
      Actions(),
      Overrides(),
      Evidence(),
      Audit(),
      FilespaceState(),
      PageState(),
      ShrinkReadiness());
}

void TestDefinitions() {
  const auto validation = info::ValidateBuiltinSysInformationProjectionDefinitions();
  if (!validation.ok) {
    for (const auto& code : validation.diagnostic_codes) {
      std::cerr << code << '\n';
    }
  }
  Require(validation.ok, "builtin sys projection definitions failed validation");

  for (const auto& view : kDirectViews) {
    const auto* definition = info::FindSysInformationProjectionDefinition(view);
    Require(definition != nullptr, std::string(view) + " definition missing");
    Require(!definition->description.empty(), std::string(view) + " description missing");
    Require(definition->view_scope == "local", std::string(view) + " is not local");
    Require(definition->mga_snapshot_visibility_required,
            std::string(view) + " does not declare MGA snapshot visibility");
    Require(definition->authorization_filter_required,
            std::string(view) + " does not declare authorization filtering");
    Require(definition->redaction_required,
            std::string(view) + " does not declare redaction");
  }

  const auto* agents = info::FindSysInformationProjectionDefinition("sys.agents");
  Require(HasColumn(*agents, "agent_uuid", "uuid"),
          "sys.agents agent_uuid metadata missing");
  Require(HasColumn(*agents, "agent_type_id", "text"),
          "sys.agents agent_type_id metadata missing");
  Require(HasColumn(*agents, "policy_uuid", "uuid"),
          "sys.agents policy_uuid metadata missing");

  const auto* shrink = info::FindSysInformationProjectionDefinition(
      "sys.filespace_shrink_readiness");
  Require(HasColumn(*shrink, "truncate_ready_bytes", "uint64"),
          "sys.filespace_shrink_readiness truncate type missing");
  Require(HasColumn(*shrink, "evidence_uuid", "uuid"),
          "sys.filespace_shrink_readiness evidence metadata missing");
}

void TestDirectRowsAndRedaction() {
  const auto agents = Build("sys.agents");
  RequireOk(agents, "sys.agents projection failed");
  Require(agents.rows.size() == 1, "sys.agents did not filter hidden/future rows");
  Require(HasRowValue(agents, "agent_uuid", Id(platform::UuidKind::object, 1)),
          "sys.agents resolver-sourced agent UUID missing");
  Require(!HasRowValue(agents, "agent_uuid", "agent.display-only"),
          "sys.agents agent_uuid preferred display reference over UUID");
  Require(!HasRowValue(agents, "policy_uuid", "policy.display-only"),
          "sys.agents policy_uuid preferred display reference over UUID");
  Require(!HasRowValue(agents, "scope_uuid", "scope.display-only"),
          "sys.agents scope_uuid preferred display reference over UUID");
  Require(HasRowValue(agents, "policy_uuid", Id(platform::UuidKind::object, 3)),
          "sys.agents resolver-sourced policy UUID missing");
  RequireNoRawUuidLeak(agents);

  const auto metrics = Build("sys.agent_metric_dependencies");
  RequireOk(metrics, "sys.agent_metric_dependencies projection failed");
  Require(HasRowValue(metrics, "current_freshness", "<redacted>"),
          "metric current value was not redacted");
  RequireNoRawUuidLeak(metrics);

  const auto policies = Build("sys.agent_policies");
  RequireOk(policies, "sys.agent_policies projection failed");
  Require(HasRowValue(policies, "version_uuid", Id(platform::UuidKind::object, 4)),
          "policy version resolver-sourced UUID missing");
  RequireNoRawUuidLeak(policies);

  const auto actions = Build("sys.agent_actions");
  RequireOk(actions, "sys.agent_actions projection failed");
  Require(HasRowValue(actions, "actor_uuid", "<redacted:actor_uuid>"),
          "agent action actor was not redacted");
  RequireNoRawUuidLeak(actions);

  const auto overrides = Build("sys.agent_overrides");
  RequireOk(overrides, "sys.agent_overrides projection failed");
  Require(HasRowValue(overrides, "created_by", "<redacted:created_by>"),
          "agent override creator was not redacted");
  RequireNoRawUuidLeak(overrides);

  const auto evidence = Build("sys.agent_evidence");
  RequireOk(evidence, "sys.agent_evidence projection failed");
  Require(HasRowValue(evidence, "payload_redacted", "YES"),
          "agent evidence payload redaction flag missing");
  RequireNoRawUuidLeak(evidence);

  const auto audit = Build("sys.agent_audit");
  RequireOk(audit, "sys.agent_audit projection failed");
  Require(HasRowValue(audit, "actor_uuid", "<redacted:actor_uuid>"),
          "agent audit actor was not redacted");
  RequireNoRawUuidLeak(audit);
}

void TestStorageAgentRows() {
  const auto filespace = Build("sys.filespace_capacity_agent_state");
  RequireOk(filespace, "sys.filespace_capacity_agent_state projection failed");
  Require(HasRowValue(filespace, "filespace_uuid", Id(platform::UuidKind::filespace, 20)),
          "filespace state resolver-sourced filespace UUID missing");
  Require(HasRowValue(filespace, "health_state", "healthy"),
          "filespace state health missing");
  RequireNoRawUuidLeak(filespace);

  const auto pages = Build("sys.page_allocation_agent_state");
  RequireOk(pages, "sys.page_allocation_agent_state projection failed");
  Require(HasRowValue(pages, "page_family", "data"),
          "page allocation state page family missing");
  Require(HasRowValue(pages, "last_scan_generation", "42"),
          "page allocation scan generation missing");
  RequireNoRawUuidLeak(pages);

  const auto shrink = Build("sys.filespace_shrink_readiness");
  RequireOk(shrink, "sys.filespace_shrink_readiness projection failed");
  Require(HasRowValue(shrink, "truncate_ready_bytes", "65536"),
          "shrink readiness truncate bytes missing");
  Require(HasRowValue(shrink, "evidence_uuid", Id(platform::UuidKind::object, 22)),
          "shrink readiness evidence resolver-sourced UUID missing");
  RequireNoRawUuidLeak(shrink);
}

void TestFrontendAliases() {
  const auto agents = Build("sys.frontend.agents");
  RequireOk(agents, "sys.frontend.agents projection failed");
  Require(HasRowValue(agents, "agent_name", "page_allocation_manager"),
          "frontend agents alias did not reuse local source");

  const auto evidence = Build("sys.frontend.agent_evidence");
  RequireOk(evidence, "sys.frontend.agent_evidence projection failed");
  Require(HasRowValue(evidence, "redaction_class", "summary"),
          "frontend evidence alias redaction class missing");
  RequireNoRawUuidLeak(evidence);

  const auto storage = Build("sys.frontend.filespace_shrink_readiness");
  RequireOk(storage, "sys.frontend.filespace_shrink_readiness projection failed");
  Require(HasRowValue(storage, "filespace_name", Id(platform::UuidKind::filespace, 20)),
          "frontend shrink alias filespace missing");
}

void TestClusterAndUnsupportedPaths() {
  const auto cluster = Build("cluster.sys.agents");
  Require(!cluster.ok, "cluster sys view did not fail closed");
  Require(cluster.diagnostic_code == info::kSysInformationDiagnosticClusterScopeForbidden,
          "cluster sys view diagnostic drifted");

  const auto unsupported = Build("sys.unknown_agent_view");
  Require(!unsupported.ok, "unsupported sys agent view succeeded");
  Require(unsupported.diagnostic_code == info::kSysInformationDiagnosticViewUnsupported,
          "unsupported sys view diagnostic drifted");
}

}  // namespace

int main() {
  TestDefinitions();
  TestDirectRowsAndRedaction();
  TestStorageAgentRows();
  TestFrontendAliases();
  TestClusterAndUnsupportedPaths();
  return EXIT_SUCCESS;
}
