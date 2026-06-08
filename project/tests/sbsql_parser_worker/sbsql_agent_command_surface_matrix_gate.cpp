// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;

struct CommandRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  bool cluster_scoped = false;
};

struct OperationRow {
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view required_right;
  std::string_view expected_code;
  bool ok_expected = false;
  bool cluster_scoped = false;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasApiDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

std::string ApiDiagnosticCodes(const api::EngineApiResult& result) {
  std::string out;
  for (const auto& diagnostic : result.diagnostics) {
    if (!out.empty()) out += ",";
    out += diagnostic.code;
  }
  if (out.empty()) out = "<none>";
  return out;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f013b-0000-7000-8000-000000000101";
  session.connection_uuid = "019f013b-0000-7000-8000-000000000102";
  session.database_uuid = "019f013b-0000-7000-8000-000000000103";
  session.catalog_epoch = 113;
  session.security_policy_epoch = 127;
  session.descriptor_epoch = 131;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f013b-0000-7000-8000-000000000104";
  config.bundle_contract_id = "sbp_sbsql@pfar-013b-agent-command-surface";
  config.build_id = "pfar-013b-agent-command-surface";
  return config;
}

api::EngineRequestContext EngineContext(std::string_view right,
                                        bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "pfar-013b-agent-command-surface";
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  context.security_context_present = security_context_present;
  context.cluster_authority_available = cluster_provider::ClusterProviderSupportsExecution();
  context.database_uuid.canonical = "019f013b-0000-7000-8000-000000000201";
  context.cluster_uuid.canonical = "019f013b-0000-7000-8000-000000000202";
  context.node_uuid.canonical = "019f013b-0000-7000-8000-000000000203";
  context.principal_uuid.canonical = "019f013b-0000-7000-8000-000000000204";
  context.session_uuid.canonical = "019f013b-0000-7000-8000-000000000205";
  if (!right.empty()) {
    context.trace_tags.push_back("right:" + std::string(right));
    context.trace_tags.push_back("right:OBS_AGENT_CONTROL");
    context.trace_tags.push_back("right:OBS_POLICY_APPLY");
    context.trace_tags.push_back("right:OBS_AGENT_OVERRIDE");
  }
  context.trace_tags.push_back("right:OBS_AGENT_STATE_READ");
  context.trace_tags.push_back("right:OBS_CLUSTER_HEALTH_INSPECT");
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("pfar_013b_agent_command_surface");
  return context;
}

sblr::SblrDispatchRequest DispatchRequest(const OperationRow& row,
                                          std::string_view right,
                                          bool security_context_present = true) {
  sblr::SblrDispatchRequest request;
  request.context = EngineContext(right, security_context_present);
  request.envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                            std::string(row.opcode),
                                            "trace.pfar013b.agent_surface");
  request.envelope.result_shape = row.cluster_scoped ? "cluster.provider.stub.v1"
                                                     : "agent.command_surface.v1";
  request.envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = row.cluster_scoped;
  request.api_request.context = request.context;
  request.api_request.operation_id = std::string(row.operation_id);
  if (row.expected_code == "METRIC.STALE") {
    request.api_request.option_envelopes.push_back("metric_snapshot:stale");
  }
  return request;
}

void RequireCommandLowering(const CommandRow& row) {
  const auto session = ParserSession();
  const auto cst = BuildCst(row.sql);
  const auto ast = BuildAst(cst);
  const auto bound = BindAst(ast, cst, ParserConfigForTest(), session, {});
  const auto envelope = LowerToSblr(bound, cst, session);
  const auto verifier = VerifySblrEnvelope(envelope);
  Require(bound.bound, std::string(row.sql) + " did not bind");
  Require(verifier.admitted,
          std::string(row.sql) + " verifier rejected route: " +
              RenderMessageVectorSet(verifier.messages));
  Require(envelope.operation_id == row.operation_id,
          std::string(row.sql) + " operation id mismatch");
  Require(envelope.sblr_opcode == row.opcode,
          std::string(row.sql) + " opcode mismatch");
  Require(envelope.operation_family ==
              (row.cluster_scoped ? "sblr.cluster.private_operation.v3"
                                  : "sblr.management.runtime_operation.v3"),
          std::string(row.sql) + " operation family mismatch");
  Require(HasValue(envelope.required_authority_steps,
                   "authority.parser.no_agent_execution"),
          std::string(row.sql) + " parser no-agent-execution authority missing");
  Require(HasValue(envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          std::string(row.sql) + " parser storage/finality authority missing");
  Require(!envelope.parser_executes_sql,
          std::string(row.sql) + " allowed parser SQL execution");
  Require(envelope.payload.find(row.sql) == std::string::npos,
          std::string(row.sql) + " embedded source SQL text");
  const auto admission = server::AdmitServerSblrEnvelope(
      server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted,
          std::string(row.sql) + " server admission rejected route");
  Require(admission.operation_id == row.operation_id,
          std::string(row.sql) + " server admission operation drifted");
}

void RequireOperationRoute(const OperationRow& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, std::string(row.operation_id) + " registry row missing");
  Require(entry->opcode == row.opcode,
          std::string(row.operation_id) + " registry opcode mismatch");
  Require(entry->requires_security_context,
          std::string(row.operation_id) + " registry security flag missing");
  if (row.cluster_scoped) {
    Require(entry->requires_cluster_authority,
            std::string(row.operation_id) + " registry cluster flag missing");
  }

  const auto dispatch = sblr::DispatchSblrOperation(
      DispatchRequest(row, row.required_right));
  Require(dispatch.envelope_validated,
          std::string(row.operation_id) + " dispatch envelope rejected");
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          std::string(row.operation_id) + " did not dispatch");
  if (row.cluster_scoped) {
    const auto provider_info = cluster_provider::DescribeClusterProvider();
    const bool compile_link_stub =
        provider_info.compile_link_only &&
        provider_info.provider_type == "compile_link_stub";
    if (compile_link_stub) {
      Require(!dispatch.api_result.ok,
              std::string(row.operation_id) +
                  " compile-link stub unexpectedly executed cluster route");
      Require(HasApiDiagnostic(
                  dispatch.api_result,
                  cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
              std::string(row.operation_id) + " stub diagnostic missing");
      Require(HasDispatchDiagnostic(
                  dispatch,
                  cluster_provider::kClusterHandshakeStubCompileLinkOnlyCode),
              std::string(row.operation_id) + " stub dispatch diagnostic missing");
      Require(HasEvidence(dispatch.api_result, "cluster_provider", "stub"),
              std::string(row.operation_id) + " stub provider evidence missing");
    } else if (cluster_provider::ClusterProviderSupportsExecution()) {
      Require(dispatch.api_result.ok,
              std::string(row.operation_id) + " cluster provider did not accept");
    } else {
      Require(!dispatch.api_result.ok,
              std::string(row.operation_id) + " no-cluster route accepted");
      Require(HasApiDiagnostic(dispatch.api_result,
                               "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
              std::string(row.operation_id) + " no-cluster API diagnostic missing");
      Require(HasDispatchDiagnostic(dispatch,
                                    "SBLR.CLUSTER.SUPPORT_NOT_ENABLED"),
              std::string(row.operation_id) + " no-cluster dispatch diagnostic missing");
    }
    return;
  }

  Require(dispatch.api_result.ok == row.ok_expected,
          std::string(row.operation_id) + " ok state mismatch");
  Require(HasApiDiagnostic(dispatch.api_result, row.expected_code),
          std::string(row.operation_id) + " exact diagnostic missing; saw=" +
              ApiDiagnosticCodes(dispatch.api_result));
  Require(HasEvidence(dispatch.api_result, "agent_command_surface", row.operation_id),
          std::string(row.operation_id) + " command-surface evidence missing");
}

void RequireDeniedActionFixture() {
  OperationRow row{"agents.action.approve",
                   "SBLR_AGENT_ACTION_APPROVE",
                   "",
                   "ACTION.PERMISSION_DENIED",
                   false,
                   false};
  const auto dispatch = sblr::DispatchSblrOperation(
      DispatchRequest(row, "", true));
  Require(dispatch.accepted && dispatch.dispatched_to_api,
          "denied action approval did not dispatch to engine");
  Require(!dispatch.api_result.ok, "denied action approval unexpectedly succeeded");
  Require(HasApiDiagnostic(dispatch.api_result, "ACTION.PERMISSION_DENIED"),
          "denied action approval exact diagnostic missing");
  Require(HasEvidence(dispatch.api_result, "agent_denial_evidence"),
          "denied action approval evidence missing");
  Require(!HasEvidence(dispatch.api_result, "agent_action_approval_evidence"),
          "denied action approval created actuator/evidence success row");
}

}  // namespace

int main() {
  const std::vector<CommandRow> commands = {
      {"SHOW AGENT memory_governor METRICS", "agents.metrics.get", "SBLR_AGENT_METRICS_GET", false},
      {"SHOW AGENT memory_governor POLICY", "agents.policy.get", "SBLR_AGENT_POLICY_GET", false},
      {"SHOW AGENT memory_governor EVIDENCE", "agents.evidence.list", "SBLR_AGENT_EVIDENCE_LIST", false},
      {"SHOW AGENT memory_governor AUDIT", "agents.audit.list", "SBLR_AGENT_AUDIT_LIST", false},
      {"SHOW AGENT ACTIONS", "agents.actions.list", "SBLR_AGENT_ACTION_LIST", false},
      {"SHOW AGENT OVERRIDES", "agents.overrides.list", "SBLR_AGENT_OVERRIDE_LIST", false},
      {"ALTER AGENT memory_governor QUARANTINE REASON checksum_failure", "agents.quarantine", "SBLR_AGENT_QUARANTINE", false},
      {"ALTER AGENT memory_governor ATTACH POLICY baseline_policy", "agents.policy.attach", "SBLR_AGENT_POLICY_ATTACH", false},
      {"ALTER AGENT ACTION action_uuid APPROVE", "agents.action.approve", "SBLR_AGENT_ACTION_APPROVE", false},
      {"CREATE AGENT OVERRIDE FOR memory_governor SUPPRESS page_preallocation UNTIL tomorrow REASON operator_hold", "agents.override.create", "SBLR_AGENT_OVERRIDE_CREATE", false},
      {"SHOW CLUSTER AGENTS", "cluster.agent.list", "SBLR_CLUSTER_AGENT_LIST", true},
      {"SHOW CLUSTER AGENT cluster_autoscale_manager", "cluster.agent.get", "SBLR_CLUSTER_AGENT_GET", true},
      {"ALTER CLUSTER AGENT cluster_autoscale_manager DRAIN", "cluster.agent.control", "SBLR_CLUSTER_AGENT_CONTROL", true},
      {"SHOW FILESPACES", "filespaces.show", "SBLR_SHOW_FILESPACES", false},
      {"SHOW FILESPACE main HEALTH", "filespaces.health.show", "SBLR_SHOW_FILESPACE_HEALTH", false},
      {"SHOW PAGE ALLOCATION BY FAMILY", "pages.allocation.family.show", "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY", false},
  };
  for (const auto& command : commands) {
    RequireCommandLowering(command);
  }

  const std::vector<OperationRow> operations = {
      {"agents.metrics.get", "SBLR_AGENT_METRICS_GET", "OBS_METRICS_READ_FAMILY", "METRIC.STALE", false, false},
      {"agents.policy.get", "SBLR_AGENT_POLICY_GET", "OBS_POLICY_READ", "POLICY.NOT_FOUND", false, false},
      {"agents.evidence.list", "SBLR_AGENT_EVIDENCE_LIST", "OBS_AGENT_EVIDENCE_READ", "AGENT.EVIDENCE_NOT_FOUND", false, false},
      {"agents.audit.list", "SBLR_AGENT_AUDIT_LIST", "OBS_AGENT_EVIDENCE_READ", "AGENT.AUDIT_NOT_FOUND", false, false},
      {"agents.actions.list", "SBLR_AGENT_ACTION_LIST", "OBS_AGENT_RECOMMENDATION_READ", "AGENT.NONE", true, false},
      {"agents.overrides.list", "SBLR_AGENT_OVERRIDE_LIST", "OBS_AGENT_STATE_READ", "AGENT.NONE", true, false},
      {"agents.drain", "SBLR_AGENT_LIFECYCLE_DRAIN", "OBS_AGENT_CONTROL", "AGENT.DRAIN_NOT_SUPPORTED", false, false},
      {"agents.restart", "SBLR_AGENT_LIFECYCLE_RESTART", "OBS_AGENT_CONTROL", "AGENT.RESTART_REFUSED", false, false},
      {"agents.enable", "SBLR_AGENT_LIFECYCLE_ENABLE", "OBS_AGENT_CONTROL", "AGENT.ENABLE_REFUSED", false, false},
      {"agents.disable", "SBLR_AGENT_LIFECYCLE_DISABLE", "OBS_AGENT_CONTROL", "AGENT.DISABLE_REFUSED", false, false},
      {"agents.quarantine", "SBLR_AGENT_QUARANTINE", "OBS_AGENT_CONTROL", "AGENT.ALREADY_QUARANTINED", false, false},
      {"agents.unquarantine", "SBLR_AGENT_UNQUARANTINE", "OBS_AGENT_CONTROL", "AGENT.NOT_QUARANTINED", false, false},
      {"agents.policy.attach", "SBLR_AGENT_POLICY_ATTACH", "OBS_POLICY_APPLY", "POLICY.INVALID", false, false},
      {"agents.policy.detach", "SBLR_AGENT_POLICY_DETACH", "OBS_POLICY_APPLY", "POLICY.REQUIRED", false, false},
      {"agents.policy.validate", "SBLR_AGENT_POLICY_VALIDATE", "OBS_POLICY_VALIDATE", "POLICY.INVALID", false, false},
      {"agents.policy.simulate", "SBLR_AGENT_POLICY_SIMULATE", "OBS_POLICY_SIMULATE", "POLICY.INVALID", false, false},
      {"agents.policy.apply", "SBLR_AGENT_POLICY_APPLY", "OBS_POLICY_APPROVE", "POLICY.NOT_APPROVED", false, false},
      {"agents.policy.rollback", "SBLR_AGENT_POLICY_ROLLBACK", "OBS_POLICY_ROLLBACK", "POLICY.ROLLBACK_REFUSED", false, false},
      {"agents.action.cancel", "SBLR_AGENT_ACTION_CANCEL", "OBS_AGENT_ACTION_CANCEL", "ACTION.NOT_CANCELLABLE", false, false},
      {"agents.action.retry", "SBLR_AGENT_ACTION_RETRY", "OBS_AGENT_ACTION_APPROVE", "ACTION.RETRY_COOLDOWN_ACTIVE", false, false},
      {"agents.action.suppress", "SBLR_AGENT_ACTION_SUPPRESS", "OBS_AGENT_OVERRIDE", "OVERRIDE.EXPIRY_TOO_LONG", false, false},
      {"agents.override.create", "SBLR_AGENT_OVERRIDE_CREATE", "OBS_AGENT_OVERRIDE", "OVERRIDE.EXPIRY_TOO_LONG", false, false},
      {"agents.override.update", "SBLR_AGENT_OVERRIDE_UPDATE", "OBS_AGENT_OVERRIDE", "OVERRIDE.INVALID_PATCH", false, false},
      {"agents.override.drop", "SBLR_AGENT_OVERRIDE_DROP", "OBS_AGENT_OVERRIDE", "OVERRIDE.NOT_FOUND", false, false},
      {"agents.set_mode", "SBLR_AGENT_SET_MODE", "OBS_AGENT_CONTROL", "AGENT.MODE_REFUSED", false, false},
      {"filespaces.show", "SBLR_SHOW_FILESPACES", "OBS_CONFIG_INSPECT", "AGENT.NONE", true, false},
      {"filespaces.health.show", "SBLR_SHOW_FILESPACE_HEALTH", "OBS_METRICS_READ_FAMILY", "FILESPACE.NOT_FOUND", false, false},
      {"filespaces.capacity.show", "SBLR_SHOW_FILESPACE_CAPACITY", "OBS_METRICS_READ_FAMILY", "FILESPACE.NOT_FOUND", false, false},
      {"pages.allocation.show", "SBLR_SHOW_PAGE_ALLOCATION", "OBS_METRICS_READ_FAMILY", "METRIC.STALE", false, false},
      {"pages.allocation.family.show", "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY", "OBS_METRICS_READ_FAMILY", "METRIC.STALE", false, false},
      {"pages.relocation_backlog.show", "SBLR_SHOW_PAGE_RELOCATION_BACKLOG", "OBS_METRICS_READ_FAMILY", "METRIC.STALE", false, false},
      {"filespaces.shrink_readiness.show", "SBLR_SHOW_FILESPACE_SHRINK_READINESS", "OBS_METRICS_READ_FAMILY", "FILESPACE.SHRINK_BLOCKED", false, false},
      {"cluster.agent.list", "SBLR_CLUSTER_AGENT_LIST", "OBS_CLUSTER_HEALTH_INSPECT", "SBLR.CLUSTER.SUPPORT_NOT_ENABLED", false, true},
      {"cluster.agent.get", "SBLR_CLUSTER_AGENT_GET", "OBS_CLUSTER_HEALTH_INSPECT", "SBLR.CLUSTER.SUPPORT_NOT_ENABLED", false, true},
      {"cluster.agent.control", "SBLR_CLUSTER_AGENT_CONTROL", "OBS_CLUSTER_CONTROL", "SBLR.CLUSTER.SUPPORT_NOT_ENABLED", false, true},
  };
  for (const auto& operation : operations) {
    RequireOperationRoute(operation);
  }
  RequireDeniedActionFixture();
  return EXIT_SUCCESS;
}
