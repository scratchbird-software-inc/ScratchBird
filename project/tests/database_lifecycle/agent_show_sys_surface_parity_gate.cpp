// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"
#include "catalog/sys_information_projection.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace info = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineRequestContext AgentContext() {
  api::EngineRequestContext context;
  context.request_id = "pfar-015c-agent-show-sys-surface-parity";
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.database_uuid.canonical = "019f015c-0000-7000-8000-000000000001";
  context.principal_uuid.canonical = "principal.management-ui.local";
  context.session_uuid.canonical = "session.management-ui.local";
  context.catalog_generation_id = 15;
  context.trace_tags = {
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_AGENT_RECOMMENDATION_READ",
      "right:OBS_AGENT_EVIDENCE_READ",
      "right:OBS_METRICS_READ_FAMILY",
      "right:OBS_POLICY_READ",
      "agent_show_sys_surface_parity_gate",
  };
  return context;
}

std::string MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1915015000000ull + seed);
  Require(generated.ok(), "PFAR-015C fixture UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

const std::vector<api::EngineAgentCatalogIdentitySource>& FixtureCatalogIdentities() {
  static const std::vector<api::EngineAgentCatalogIdentitySource> identities = {
      {.agent_type_id = "memory_governor",
       .agent_uuid = MakeUuid(platform::UuidKind::object, 1),
       .scope_uuid = MakeUuid(platform::UuidKind::database, 2),
       .policy_uuid = MakeUuid(platform::UuidKind::object, 3),
       .policy_name = "memory_governor_policy",
       .component = "engine.runtime",
       .scope_kind = "database"},
      {.agent_type_id = "page_allocation_manager",
       .agent_uuid = MakeUuid(platform::UuidKind::object, 4),
       .scope_uuid = MakeUuid(platform::UuidKind::database, 5),
       .policy_uuid = MakeUuid(platform::UuidKind::object, 6),
       .policy_name = "page_preallocation_policy",
       .component = "storage.pages",
       .scope_kind = "database"},
  };
  return identities;
}

const api::EngineAgentCatalogIdentitySource& MemoryIdentity() {
  return FixtureCatalogIdentities().front();
}

api::EngineAgentCommandSurfaceRequest CommandRequest(std::string operation_id) {
  api::EngineAgentCommandSurfaceRequest request;
  request.context = AgentContext();
  request.operation_id = std::move(operation_id);
  request.target_object.object_kind = "memory_governor";
  request.agent_catalog_identity_sources = FixtureCatalogIdentities();
  return request;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) { return field.second.encoded_value; }
  }
  return {};
}

bool RowHasField(const api::EngineRowValue& row, std::string_view field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) { return true; }
  }
  return false;
}

const api::EngineRowValue* FindRow(const api::EngineApiResult& result,
                                   std::string_view field_name,
                                   std::string_view field_value) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, field_name) == field_value) { return &row; }
  }
  return nullptr;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool IsUuidField(std::string_view field_name) {
  return field_name.size() >= 5 &&
         field_name.substr(field_name.size() - 5) == "_uuid";
}

void RequireNoFakeCatalogIdentity(const api::EngineApiResult& result) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      const auto& value = field.second.encoded_value;
      Require(value.find("/dev/") == std::string::npos,
              "PFAR-015C physical path leaked in row field");
      if (IsUuidField(field.first)) {
        Require(value.rfind("agent.", 0) != 0,
                "PFAR-015C fake agent stable ref leaked in UUID field");
        Require(value.rfind("policy.", 0) != 0,
                "PFAR-015C fake policy stable ref leaked in UUID field");
        Require(value.rfind("scope.", 0) != 0,
                "PFAR-015C fake scope stable ref leaked in UUID field");
      }
    }
  }
}

void RequireSysAgentsColumns(const api::EngineRowValue& row) {
  const auto* definition = info::FindSysInformationProjectionDefinition("sys.agents");
  Require(definition != nullptr, "sys.agents definition missing");
  for (const auto& column : definition->columns) {
    Require(RowHasField(row, column.column_name),
            "Engine agent row missing sys.agents column " + column.column_name);
    if (column.exposes_internal_uuid) {
      Require(column.logical_type == "uuid",
              "sys.agents UUID column lost resolver-sourced uuid metadata");
      const auto value = FieldValue(row, column.column_name);
      Require(value.empty() || value.rfind("<redacted:", 0) == 0 ||
                  value.find('-') != std::string::npos,
              "Engine agent row UUID column is not resolver sourced");
    }
  }
}

void RequireAgentRowParity(const api::EngineRowValue& left,
                           const api::EngineRowValue& right) {
  const auto* definition = info::FindSysInformationProjectionDefinition("sys.agents");
  Require(definition != nullptr, "sys.agents definition missing for parity check");
  for (const auto& column : definition->columns) {
    Require(FieldValue(left, column.column_name) == FieldValue(right, column.column_name),
            "SHOW/API sys.agents field mismatch for " + column.column_name);
  }
}

void TestSysListShowAgentsParity() {
  api::EngineSysAgentsRequest sys_request;
  sys_request.context = AgentContext();
  sys_request.agent_catalog_identity_sources = FixtureCatalogIdentities();
  const auto sys = api::EngineSysAgents(sys_request);
  Require(sys.ok, "EngineSysAgents failed");
  Require(HasEvidence(sys, "sys_surface", "sys.agents"),
          "EngineSysAgents sys surface evidence missing");
  RequireNoFakeCatalogIdentity(sys);

  const auto* sys_memory = FindRow(sys, "agent_type_id", "memory_governor");
  Require(sys_memory != nullptr, "EngineSysAgents memory_governor row missing");
  RequireSysAgentsColumns(*sys_memory);
  Require(FieldValue(*sys_memory, "agent_uuid") == MemoryIdentity().agent_uuid,
          "EngineSysAgents did not preserve fixture-generated agent UUID");
  Require(FieldValue(*sys_memory, "scope_uuid") == MemoryIdentity().scope_uuid,
          "EngineSysAgents did not preserve fixture-generated scope UUID");
  Require(FieldValue(*sys_memory, "policy_uuid") == MemoryIdentity().policy_uuid,
          "EngineSysAgents did not preserve fixture-generated policy UUID");
  Require(RowHasField(*sys_memory, "agent_type"),
          "EngineSysAgents legacy agent_type alias missing");
  Require(FieldValue(*sys_memory, "agent_type") == "memory_governor",
          "EngineSysAgents legacy agent_type alias changed");
  Require(FindRow(sys, "agent_type_id", "cluster_autoscale_manager") == nullptr,
          "EngineSysAgents leaked cluster-only agent in no-cluster build");

  api::EngineListAgentsRequest list_request;
  list_request.context = AgentContext();
  list_request.target_object.object_kind = "memory_governor";
  list_request.agent_catalog_identity_sources = FixtureCatalogIdentities();
  const auto list = api::EngineListAgents(list_request);
  Require(list.ok, "EngineListAgents failed");
  Require(HasEvidence(list, "sys_surface", "sys.agents"),
          "EngineListAgents sys surface evidence missing");
  RequireNoFakeCatalogIdentity(list);
  const auto* list_memory = FindRow(list, "agent_type_id", "memory_governor");
  Require(list_memory != nullptr, "EngineListAgents memory_governor row missing");
  RequireAgentRowParity(*sys_memory, *list_memory);
  Require(FieldValue(*list_memory, "agent_type") == "memory_governor",
          "EngineListAgents legacy agent_type alias changed");

  api::EngineShowAgentRequest show_request;
  show_request.context = AgentContext();
  show_request.target_object.object_kind = "memory_governor";
  show_request.agent_catalog_identity_sources = FixtureCatalogIdentities();
  const auto show = api::EngineShowAgent(show_request);
  Require(show.ok, "EngineShowAgent failed");
  RequireNoFakeCatalogIdentity(show);
  const auto* show_memory = FindRow(show, "agent_type_id", "memory_governor");
  Require(show_memory != nullptr, "EngineShowAgent memory_governor row missing");
  RequireAgentRowParity(*sys_memory, *show_memory);
  Require(RowHasField(*show_memory, "feature_availability"),
          "EngineShowAgent compatibility field missing");
}

void RequireExactCommandSurfaceRow(const api::EngineApiResult& result,
                                   std::string_view operation_id,
                                   std::string_view opcode,
                                   std::string_view api_call,
                                   std::string_view sys_surface,
                                   std::string_view state,
                                   std::string_view diagnostic,
                                   std::string_view evidence_kind) {
  Require(!result.result_shape.rows.empty(), "command surface row missing");
  const auto& row = result.result_shape.rows.front();
  Require(FieldValue(row, "operation_id") == operation_id,
          "command surface operation_id mismatch");
  Require(FieldValue(row, "sblr_operation") == opcode,
          "command surface SBLR operation mismatch");
  Require(FieldValue(row, "api_call") == api_call,
          "command surface API call mismatch");
  Require(FieldValue(row, "sys_surface") == sys_surface,
          "command surface sys surface mismatch");
  Require(FieldValue(row, "agent_type_id") == "memory_governor",
          "command surface canonical agent_type_id missing");
  Require(FieldValue(row, "agent_uuid") == MemoryIdentity().agent_uuid,
          "command surface did not preserve fixture-generated agent UUID");
  Require(FieldValue(row, "result_state") == state,
          "command surface result_state mismatch");
  Require(FieldValue(row, "diagnostic_code") == diagnostic,
          "command surface diagnostic_code mismatch");
  Require(FieldValue(row, "evidence_kind") == evidence_kind,
          "command surface evidence kind mismatch");
  Require(RowHasField(row, "redaction_state"),
          "command surface redaction_state missing");
  Require(RowHasField(row, "payload_redacted"),
          "command surface payload_redacted missing");
}

void TestCommandSurfaceParityRows() {
  struct Case {
    const char* operation_id;
    const char* opcode;
    const char* api_call;
    const char* sys_surface;
    const char* result_state;
    const char* diagnostic_code;
    const char* evidence_kind;
  };
  const std::vector<Case> cases = {
      {"agents.metrics.get", "SBLR_AGENT_METRICS_GET", "sb_api_agent_metrics_get",
       "sys.agent_metric_dependencies", "refused", "METRIC.STALE", ""},
      {"agents.policy.get", "SBLR_AGENT_POLICY_GET", "sb_api_agent_policy_get",
       "sys.agent_policies", "refused", "POLICY.NOT_FOUND", ""},
      {"agents.evidence.list", "SBLR_AGENT_EVIDENCE_LIST", "sb_api_agent_evidence_list",
       "sys.agent_evidence", "refused", "AGENT.EVIDENCE_NOT_FOUND",
       "agent_read_evidence"},
      {"agents.audit.list", "SBLR_AGENT_AUDIT_LIST", "sb_api_agent_audit_list",
       "sys.agent_audit", "refused", "AGENT.AUDIT_NOT_FOUND", "agent_read_evidence"},
      {"agents.actions.list", "SBLR_AGENT_ACTION_LIST", "sb_api_agent_action_list",
       "sys.agent_actions", "empty", "AGENT.NONE", ""},
      {"agents.overrides.list", "SBLR_AGENT_OVERRIDE_LIST", "sb_api_agent_override_list",
       "sys.agent_overrides", "empty", "AGENT.NONE", ""},
  };

  const std::set<std::string_view> exact_states = {
      "success", "refused", "denied", "empty", "redacted", "unsupported",
      "operator_required"};
  for (const auto& c : cases) {
    auto request = CommandRequest(c.operation_id);
    const auto result = api::EngineAgentCommandSurfaceOperation(request);
    RequireNoFakeCatalogIdentity(result);
    Require(exact_states.count(c.result_state) == 1,
            "test case uses a non-exact command surface result state");
    RequireExactCommandSurfaceRow(result,
                                  c.operation_id,
                                  c.opcode,
                                  c.api_call,
                                  c.sys_surface,
                                  c.result_state,
                                  c.diagnostic_code,
                                  c.evidence_kind);
    Require(HasDiagnostic(result, c.diagnostic_code),
            "command surface diagnostic vector missing expected code");
    Require(HasEvidence(result, "agent_command_surface", c.operation_id),
            "command surface evidence missing operation id");
    Require(HasEvidence(result, "agent_command_api", c.api_call),
            "command surface API evidence missing");
  }

  auto redacted_request = CommandRequest("agents.evidence.list");
  redacted_request.option_envelopes.push_back("agent_redaction_policy:summary_only");
  const auto redacted = api::EngineAgentCommandSurfaceOperation(redacted_request);
  Require(redacted.ok, "redacted command surface read was refused");
  RequireExactCommandSurfaceRow(redacted,
                                "agents.evidence.list",
                                "SBLR_AGENT_EVIDENCE_LIST",
                                "sb_api_agent_evidence_list",
                                "sys.agent_evidence",
                                "redacted",
                                "AGENT.REDACTED",
                                "agent_read_evidence");
  Require(HasEvidence(redacted, "agent_redaction_evidence", "agents.evidence.list"),
          "redacted command surface evidence missing");
  RequireNoFakeCatalogIdentity(redacted);
}

}  // namespace

int main() {
  TestSysListShowAgentsParity();
  TestCommandSurfaceParityRows();
  return EXIT_SUCCESS;
}
