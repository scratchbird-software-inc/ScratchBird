// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"
#include "catalog/sys_information_projection.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "management/support_bundle_api.hpp"
#include "observability/agent_evidence_retention_api.hpp"
#include "observability/agent_observability_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string Id(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "PFAR-016C UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineRequestContext Context(std::initializer_list<std::string_view> rights) {
  api::EngineRequestContext context;
  context.request_id = "pfar-016c-zero-grey";
  context.database_path = "/tmp/pfar-016c.sbdb";
  context.database_uuid.canonical = Id(platform::UuidKind::database, 1);
  context.node_uuid.canonical = Id(platform::UuidKind::object, 2);
  context.cluster_uuid.canonical = Id(platform::UuidKind::object, 3);
  context.session_uuid.canonical = Id(platform::UuidKind::object, 4);
  context.principal_uuid.canonical = Id(platform::UuidKind::principal, 5);
  context.transaction_uuid.canonical = Id(platform::UuidKind::transaction, 6);
  context.security_context_present = true;
  context.cluster_authority_available = cluster_provider::ClusterProviderSupportsExecution();
  context.local_transaction_id = 16017;
  context.catalog_generation_id = 17;
  for (const auto right : rights) {
    context.trace_tags.push_back("right:" + std::string(right));
  }
  return context;
}

api::EngineAgentCatalogIdentitySource AgentIdentity() {
  api::EngineAgentCatalogIdentitySource source;
  source.agent_type_id = "page_allocation_manager";
  source.agent_uuid = Id(platform::UuidKind::object, 20);
  source.scope_uuid = Id(platform::UuidKind::database, 21);
  source.policy_uuid = Id(platform::UuidKind::object, 22);
  source.policy_name = "page_allocation_baseline";
  source.component = "storage.pages";
  source.scope_kind = "database";
  return source;
}

std::string Field(const api::EngineRowValue& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second.encoded_value; }
  }
  return {};
}

std::string Field(const api::SysInformationProjectionRow& row, std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second; }
  }
  return {};
}

std::string FirstField(const api::EngineApiResult& result, std::string_view name) {
  for (const auto& row : result.result_shape.rows) {
    const auto value = Field(row, name);
    if (!value.empty()) { return value; }
  }
  return {};
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

bool IsUuidField(std::string_view field_name) {
  return field_name.size() >= 5 &&
         field_name.substr(field_name.size() - 5) == "_uuid";
}

platform::UuidKind KindForField(std::string_view field_name) {
  if (field_name == "filespace_uuid") { return platform::UuidKind::filespace; }
  if (field_name == "actor_uuid" ||
      field_name == "actor_principal_uuid" ||
      field_name == "requester_principal_uuid") {
    return platform::UuidKind::principal;
  }
  if (field_name == "transaction_uuid") { return platform::UuidKind::transaction; }
  if (field_name == "database_uuid" || field_name == "scope_uuid") {
    return platform::UuidKind::database;
  }
  return platform::UuidKind::object;
}

void RequireUuidAuthority(std::string_view field_name, std::string_view value) {
  if (!IsUuidField(field_name) || value.empty() || value.rfind("<redacted", 0) == 0) {
    return;
  }
  Require(value.rfind("agent.", 0) != 0 &&
              value.rfind("policy.", 0) != 0 &&
              value.rfind("scope.", 0) != 0,
          std::string(field_name) + " used label-prefixed identity: " + std::string(value));
  Require(uuid::ParseDurableEngineIdentityUuid(KindForField(field_name), std::string(value)).ok(),
          std::string(field_name) + " is not a typed durable engine UUID: " + std::string(value));
}

void RequireZeroGreyRows(const api::EngineApiResult& result,
                         bool rows_require_state,
                         bool rows_require_diagnostic) {
  for (const auto& row : result.result_shape.rows) {
    const auto state = Field(row, "result_state");
    if (rows_require_state) {
      Require(!state.empty(), "zero-grey row omitted result_state");
      Require(api::EngineAgentZeroGreyResultStateAllowed(state),
              "zero-grey row used disallowed result_state: " + state);
      Require(!api::EngineAgentZeroGreyResultStateAmbiguous(state),
              "zero-grey row used ambiguous result_state: " + state);
    }
    const auto diagnostic = Field(row, "diagnostic_code");
    if (rows_require_diagnostic) {
      Require(!diagnostic.empty(), "zero-grey row omitted diagnostic_code");
      Require(diagnostic.find("implementation-defined") == std::string::npos,
              "zero-grey row used vague diagnostic_code");
    }
    for (const auto& field : row.fields) {
      RequireUuidAuthority(field.first, field.second.encoded_value);
      Require(field.second.encoded_value.find("best_effort") == std::string::npos,
              "zero-grey row leaked best_effort");
      Require(field.second.encoded_value.find("implementation-defined") == std::string::npos,
              "zero-grey row leaked implementation-defined");
    }
  }
}

void TestContractAllowlistAndTemplates() {
  const auto contract = api::BuiltinAgentZeroGreyOutputContract();
  const std::set<std::string> states(contract.allowed_result_states.begin(),
                                    contract.allowed_result_states.end());
  for (const auto& state : {"success",
                            "refused",
                            "denied",
                            "empty",
                            "redacted",
                            "hidden",
                            "unsupported",
                            "operator_required",
                            "pending_evidence",
                            "backpressure",
                            "request_accepted",
                            "read_result",
                            "cluster_not_enabled"}) {
    Require(states.count(state) == 1, std::string("missing zero-grey state: ") + state);
    Require(api::EngineAgentZeroGreyResultStateAllowed(state),
            std::string("state helper refused: ") + state);
  }
  for (const auto& vague : {"best_effort", "partial_success", "implementation-defined", "unknown"}) {
    Require(!api::EngineAgentZeroGreyResultStateAllowed(vague),
            std::string("vague state was allowed: ") + vague);
    Require(api::EngineAgentZeroGreyResultStateAmbiguous(vague),
            std::string("vague state was not detected: ") + vague);
  }
  const std::set<std::string> fields(contract.evidence_payload_fields.begin(),
                                    contract.evidence_payload_fields.end());
  for (const auto& required : {"evidence_uuid",
                               "evidence_type",
                               "agent_uuid",
                               "scope_uuid",
                               "action_uuid",
                               "policy_uuid",
                               "actor_principal_uuid",
                               "created_at",
                               "input_metric_snapshot_digest",
                               "decision_payload",
                               "result_state",
                               "diagnostic_code",
                               "redaction_class",
                               "retention_class",
                               "payload_digest"}) {
    Require(fields.count(required) == 1,
            std::string("missing evidence template field: ") + required);
  }
}

api::EngineAgentCommandSurfaceRequest CommandRequest(std::string operation,
                                                     std::initializer_list<std::string_view> rights) {
  api::EngineAgentCommandSurfaceRequest request;
  request.context = Context(rights);
  request.operation_id = std::move(operation);
  request.agent_catalog_identity_sources.push_back(AgentIdentity());
  request.target_object.object_kind = "page_allocation_manager";
  return request;
}

void TestCommandSurfaceStates() {
  auto empty = CommandRequest("agents.actions.list", {"OBS_AGENT_RECOMMENDATION_READ"});
  const auto empty_result = api::EngineAgentCommandSurfaceOperation(empty);
  Require(empty_result.ok, "empty command surface route failed");
  RequireZeroGreyRows(empty_result, true, true);
  Require(FirstField(empty_result, "result_state") == "empty",
          "actions list did not emit empty state");

  auto denied = CommandRequest("agents.restart", {});
  const auto denied_result = api::EngineAgentCommandSurfaceOperation(denied);
  Require(!denied_result.ok, "permission-denied command was accepted");
  RequireZeroGreyRows(denied_result, true, true);
  Require(FirstField(denied_result, "result_state") == "denied",
          "permission denial did not emit denied state");

  auto redacted = CommandRequest("agents.evidence.list", {"OBS_AGENT_EVIDENCE_READ"});
  redacted.option_envelopes.push_back("agent_redaction_policy:summary_only");
  const auto redacted_result = api::EngineAgentCommandSurfaceOperation(redacted);
  Require(redacted_result.ok, "redacted evidence read failed");
  RequireZeroGreyRows(redacted_result, true, true);
  Require(FirstField(redacted_result, "result_state") == "redacted",
          "redacted read did not emit redacted state");

  auto stale = CommandRequest("pages.allocation.show", {"OBS_METRICS_READ_FAMILY"});
  stale.option_envelopes.push_back("metrics_fresh:false");
  const auto refused = api::EngineAgentCommandSurfaceOperation(stale);
  Require(!refused.ok, "stale metric command was accepted");
  RequireZeroGreyRows(refused, true, true);
  Require(FirstField(refused, "result_state") == "refused",
          "stale metric did not emit refused state");
}

api::EngineThirdPartyAgentManagementRequest ThirdPartyRequest(std::string operation,
                                                              std::initializer_list<std::string_view> rights) {
  api::EngineThirdPartyAgentManagementRequest request;
  request.context = Context(rights);
  request.operation_id = "agents.third_party.request";
  request.agent_catalog_identity_sources.push_back(AgentIdentity());
  const auto& source = request.agent_catalog_identity_sources.front();
  request.management_request.request_uuid = Id(platform::UuidKind::object, 40);
  request.management_request.requester_principal_uuid = request.context.principal_uuid.canonical;
  request.management_request.external_system_id = "ticketing";
  request.management_request.agent_ref = source.agent_uuid;
  request.management_request.operation = std::move(operation);
  request.management_request.requested_action = request.management_request.operation;
  request.management_request.policy_ref = source.policy_uuid;
  request.management_request.reason_code = "maintenance_window";
  request.management_request.requested_expiry = "2026-06-01T00:00:00Z";
  request.management_request.redaction_context = "support_safe";
  request.management_request.idempotency_key = "zero-grey-idempotency";
  return request;
}

void TestThirdPartyStates() {
  const auto accepted = api::EngineSubmitThirdPartyAgentManagementRequest(
      ThirdPartyRequest("agents.restart", {"OBS_AGENT_CONTROL"}));
  Require(accepted.ok, "third-party mutating request failed");
  RequireZeroGreyRows(accepted, true, true);
  Require(FirstField(accepted, "result_state") == "request_accepted",
          "third-party mutating request did not emit request_accepted");

  const auto read = api::EngineSubmitThirdPartyAgentManagementRequest(
      ThirdPartyRequest("agents.policy.get", {"OBS_POLICY_READ"}));
  Require(read.ok, "third-party read request failed");
  RequireZeroGreyRows(read, true, true);
  Require(FirstField(read, "result_state") == "read_result",
          "third-party read request did not emit read_result");

  auto pending = ThirdPartyRequest("agents.restart", {"OBS_AGENT_CONTROL"});
  pending.management_request.evidence_store_available = false;
  const auto pending_result = api::EngineSubmitThirdPartyAgentManagementRequest(pending);
  Require(!pending_result.ok, "third-party missing evidence was accepted");
  RequireZeroGreyRows(pending_result, true, true);
  Require(FirstField(pending_result, "result_state") == "pending_evidence",
          "missing evidence did not emit pending_evidence");

  auto backpressure = ThirdPartyRequest("agents.restart", {"OBS_AGENT_CONTROL"});
  backpressure.management_request.backpressure = true;
  backpressure.management_request.retry_after = "PT30S";
  const auto backpressure_result = api::EngineSubmitThirdPartyAgentManagementRequest(backpressure);
  Require(!backpressure_result.ok, "third-party backpressure was accepted");
  RequireZeroGreyRows(backpressure_result, true, true);
  Require(FirstField(backpressure_result, "result_state") == "backpressure",
          "backpressure did not emit backpressure state");

  auto denied = ThirdPartyRequest("agents.restart", {});
  const auto denied_result = api::EngineSubmitThirdPartyAgentManagementRequest(denied);
  Require(!denied_result.ok, "third-party missing right was accepted");
  RequireZeroGreyRows(denied_result, true, true);
  Require(FirstField(denied_result, "result_state") == "denied",
          "third-party missing right did not emit denied state");
}

api::EngineAgentRuntimeEvidenceRecord RuntimeEvidence(std::string result_state) {
  api::EngineAgentRuntimeEvidenceRecord record;
  record.source_surface = "engine_api";
  record.agent_type_id = "page_allocation_manager";
  record.agent_uuid = Id(platform::UuidKind::object, 50);
  record.filespace_uuid = Id(platform::UuidKind::filespace, 51);
  record.policy_uuid = Id(platform::UuidKind::object, 52);
  record.evidence_uuid = Id(platform::UuidKind::object, 53);
  record.action_id = "request_page_preallocation";
  record.evidence_kind = "agent_action_evidence";
  record.result_state = std::move(result_state);
  record.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  record.payload_digest = "sha256:zero-grey";
  record.redaction_class = "summary";
  record.payload_redacted = true;
  return record;
}

void TestObservabilityZeroGreyValidation() {
  api::EngineCollectAgentRuntimeObservabilityRequest request;
  request.context = Context({"OBS_METRICS_READ_FAMILY", "OBS_AGENT_EVIDENCE_READ"});
  request.records.push_back(RuntimeEvidence("success"));
  const auto result = api::EngineCollectAgentRuntimeObservability(request);
  Require(result.ok, "observability collector refused exact state");
  RequireZeroGreyRows(result, true, true);
  Require(FirstField(result, "payload_redacted") == "true",
          "observability payload redaction flag was not exact");

  request.records.front().result_state = "best_effort";
  const auto refused = api::EngineCollectAgentRuntimeObservability(request);
  Require(!refused.ok, "observability accepted vague result state");
  Require(HasDiagnostic(refused, "AGENT.ZERO_GREY.RESULT_STATE_REFUSED"),
          "observability zero-grey diagnostic missing");
  Require(refused.result_shape.rows.empty(),
          "observability rendered rows after zero-grey refusal");
}

api::EngineSupportBundleAgentEvidenceSource SupportBundleEvidence(std::string result_state) {
  api::EngineSupportBundleAgentEvidenceSource source;
  const auto evidence = RuntimeEvidence(std::move(result_state));
  source.agent_type_id = evidence.agent_type_id;
  source.agent_uuid = evidence.agent_uuid;
  source.filespace_uuid = evidence.filespace_uuid;
  source.policy_uuid = evidence.policy_uuid;
  source.evidence_uuid = evidence.evidence_uuid;
  source.evidence_kind = evidence.evidence_kind;
  source.result_state = evidence.result_state;
  source.diagnostic_code = evidence.diagnostic_code;
  source.payload_digest = evidence.payload_digest;
  source.retention_class = "agent_evidence_400_day";
  source.retention_policy_ref = "agent.evidence.default_retention.v1";
  source.retention_deadline = "2027-06-25T00:00:00Z";
  source.payload_redacted = true;
  return source;
}

void TestSupportBundleZeroGreyValidation() {
  api::EnginePrepareSupportBundleRequest request;
  request.context = Context({"OBS_AGENT_EVIDENCE_READ", "OBS_CONFIG_INSPECT"});
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.agent_runtime_evidence.push_back(SupportBundleEvidence("success"));
  const auto result = api::EnginePrepareSupportBundle(request);
  Require(result.ok, "support bundle refused exact zero-grey evidence");
  RequireZeroGreyRows(result, true, true);
  Require(FirstField(result, "result_state") == "success",
          "support bundle did not emit success state");

  request.agent_runtime_evidence.front().result_state = "best_effort";
  const auto vague_state = api::EnginePrepareSupportBundle(request);
  Require(!vague_state.ok, "support bundle accepted vague result state");
  Require(HasDiagnostic(vague_state, "AGENT.ZERO_GREY.RESULT_STATE_REFUSED"),
          "support bundle zero-grey state diagnostic missing");
  Require(vague_state.result_shape.rows.empty(),
          "support bundle rendered rows after vague state refusal");

  request.agent_runtime_evidence.front() = SupportBundleEvidence("denied");
  request.agent_runtime_evidence.front().diagnostic_code.clear();
  const auto missing_diagnostic = api::EnginePrepareSupportBundle(request);
  Require(!missing_diagnostic.ok, "support bundle accepted denied state without diagnostic");
  Require(HasDiagnostic(missing_diagnostic, "AGENT.ZERO_GREY.DIAGNOSTIC_REQUIRED"),
          "support bundle zero-grey diagnostic-required code missing");
}

api::EngineAgentEvidenceAuditRetentionRecord RetentionEvidence(std::string result_state) {
  api::EngineAgentEvidenceAuditRetentionRecord record;
  record.source_surface = "engine_api";
  record.agent_type_id = "page_allocation_manager";
  record.agent_uuid = Id(platform::UuidKind::object, 60);
  record.filespace_uuid = Id(platform::UuidKind::filespace, 61);
  record.policy_uuid = Id(platform::UuidKind::object, 62);
  record.evidence_uuid = Id(platform::UuidKind::object, 63);
  record.action_uuid = Id(platform::UuidKind::object, 64);
  record.actor_uuid = Id(platform::UuidKind::principal, 65);
  record.evidence_kind = "agent_action_evidence";
  record.result_state = std::move(result_state);
  record.diagnostic_code = "AGENT.PAGE_PREALLOCATION.COMPLETED";
  record.retention_class = "agent_evidence_400_day";
  record.retention_policy_ref = "agent.evidence.default_retention.v1";
  record.retention_deadline = "2027-06-25T00:00:00Z";
  record.policy_generation = "42";
  return record;
}

void TestRetentionZeroGreyValidation() {
  api::EngineEvaluateAgentEvidenceRetentionRequest request;
  request.context = Context({"OBS_AGENT_EVIDENCE_READ", "OBS_CONFIG_INSPECT"});
  request.records.push_back(RetentionEvidence("success"));
  request.records.front().evidence_write_available = false;
  request.records.front().evidence_recoverable = true;
  const auto pending = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(pending.ok, "retention collector refused recoverable missing evidence");
  RequireZeroGreyRows(pending, true, true);
  Require(FirstField(pending, "result_state") == "pending_evidence",
          "retention collector did not emit pending_evidence");

  request.records.front().evidence_write_available = true;
  request.records.front().evidence_recoverable = false;
  request.records.front().result_state = "implementation-defined";
  const auto refused = api::EngineEvaluateAgentEvidenceRetention(request);
  Require(!refused.ok, "retention collector accepted vague state");
  Require(HasDiagnostic(refused, "AGENT.ZERO_GREY.RESULT_STATE_REFUSED"),
          "retention zero-grey diagnostic missing");
  Require(refused.result_shape.rows.empty(),
          "retention rendered rows after zero-grey refusal");
}

void TestSysAuditProjection() {
  api::SysInformationProjectionContext context;
  context.catalog_display_name = "ZeroGreyDB";
  context.session_language = "en";
  context.default_language = "en";
  context.visible_catalog_generation_id = 7;
  std::vector<api::SysInformationAgentAuditSource> audit_rows;
  api::SysInformationAgentAuditSource audit;
  audit.audit_uuid = Id(platform::UuidKind::object, 70);
  audit.evidence_uuid = Id(platform::UuidKind::object, 71);
  audit.actor_uuid = Id(platform::UuidKind::principal, 72);
  audit.command_name = "ALTER AGENT RESTART";
  audit.sblr_operation = "SBLR_AGENT_LIFECYCLE_RESTART";
  audit.api_call = "sb_api_agent_restart";
  audit.result_state = "denied";
  audit.diagnostic_code = "AGENT.PERMISSION_DENIED";
  audit.created_at = "2026-05-21T12:00:00Z";
  audit.catalog_generation_id = 3;
  audit.actor_visible = true;
  audit_rows.push_back(audit);
  const auto projection = api::BuildSysInformationProjection("sys.agent_audit",
                                                             context,
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             {},
                                                             audit_rows);
  Require(projection.ok, "sys.agent_audit projection failed");
  Require(projection.rows.size() == 1, "sys.agent_audit did not render fixture row");
  const auto state = Field(projection.rows.front(), "result_state");
  Require(!Field(projection.rows.front(), "diagnostic_code").empty(),
          "sys.agent_audit omitted diagnostic_code");
  Require(api::EngineAgentZeroGreyResultStateAllowed(state),
          "sys.agent_audit rendered disallowed state");
  for (const auto& field : projection.rows.front().fields) {
    RequireUuidAuthority(field.first, field.second);
  }
}

void TestClusterUnsupportedDiagnosticIsExact() {
  api::EngineClusterSysAgentsRequest request;
  request.context = Context({"OBS_AGENT_STATE_READ", "OBS_CLUSTER_HEALTH_INSPECT"});
  request.operation_id = "cluster.sys.agents";
  const auto result = api::EngineClusterSysAgents(request);
  if (!cluster_provider::ClusterProviderSupportsExecution()) {
    Require(!result.ok, "no-cluster provider accepted cluster sys agents");
    Require(HasDiagnostic(result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
            "cluster-not-enabled diagnostic was not exact");
    Require(api::EngineAgentZeroGreyResultStateAllowed("cluster_not_enabled"),
            "cluster_not_enabled was not in zero-grey allowlist");
  }
}

}  // namespace

int main() {
  TestContractAllowlistAndTemplates();
  TestCommandSurfaceStates();
  TestThirdPartyStates();
  TestObservabilityZeroGreyValidation();
  TestSupportBundleZeroGreyValidation();
  TestRetentionZeroGreyValidation();
  TestSysAuditProjection();
  TestClusterUnsupportedDiagnosticIsExact();
  return EXIT_SUCCESS;
}
