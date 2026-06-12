// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

struct MemoryRouteRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view planner_operation;
  std::string_view planner_family;
  bool mutation;
};

constexpr std::array<MemoryRouteRow, 29> kRows{{
    {"MEMORY PROFILE LIST", "memory.profile.list", "SBLR_MEMORY_PROFILE_LIST", "memory.governance.inspect", "governance", false},
    {"MEMORY PROFILE SHOW mixed_default", "memory.profile.show", "SBLR_MEMORY_PROFILE_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY PROFILE SET high_security", "memory.profile.set", "SBLR_MEMORY_PROFILE_SET", "memory.policy_migration.plan", "policy_upgrade", true},
    {"MEMORY POLICY VALIDATE", "memory.policy.validate", "SBLR_MEMORY_POLICY_VALIDATE", "memory.governance.validate", "governance", false},
    {"MEMORY TREE SHOW", "memory.tree.show", "SBLR_MEMORY_TREE_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY PRESSURE SHOW", "memory.pressure.show", "SBLR_MEMORY_PRESSURE_SHOW", "memory.governance.plan_pressure_response", "governance", false},
    {"MEMORY CACHE SHOW", "memory.cache.show", "SBLR_MEMORY_CACHE_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY CACHE FLUSH", "memory.cache.flush", "SBLR_MEMORY_CACHE_FLUSH", "memory.governance.plan_cache_control", "governance", true},
    {"MEMORY CACHE INVALIDATE", "memory.cache.invalidate", "SBLR_MEMORY_CACHE_INVALIDATE", "memory.governance.plan_cache_control", "governance", true},
    {"MEMORY SCAVENGE", "memory.scavenge", "SBLR_MEMORY_SCAVENGE", "memory.governance.plan_cache_control", "governance", true},
    {"MEMORY GRANTS SHOW", "memory.grants.show", "SBLR_MEMORY_GRANTS_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY GRANT FEEDBACK RESET", "memory.grant_feedback.reset", "SBLR_MEMORY_GRANT_FEEDBACK_RESET", "memory.governance.plan_cache_control", "governance", true},
    {"MEMORY STREAMS SHOW", "memory.streams.show", "SBLR_MEMORY_STREAMS_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY STREAM POLICY SET", "memory.stream_policy.set", "SBLR_MEMORY_STREAM_POLICY_SET", "memory.policy_migration.plan", "policy_upgrade", true},
    {"MEMORY UDR SHOW", "memory.udr.show", "SBLR_MEMORY_UDR_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY UDR LIMIT SET", "memory.udr_limit.set", "SBLR_MEMORY_UDR_LIMIT_SET", "memory.policy_migration.plan", "policy_upgrade", true},
    {"MEMORY DUMP POLICY SET", "memory.dump_policy.set", "SBLR_MEMORY_DUMP_POLICY_SET", "memory.policy_migration.plan", "policy_upgrade", true},
    {"MEMORY PLATFORM SHOW", "memory.platform.show", "SBLR_MEMORY_PLATFORM_SHOW", "memory.governance.inspect", "governance", false},
    {"MEMORY INCIDENT BUNDLE", "memory.incident.bundle", "SBLR_MEMORY_INCIDENT_BUNDLE", "memory.automation.create_report", "automation", false},
    {"MEMORY REPORT CREATE", "memory.report.create", "SBLR_MEMORY_REPORT_CREATE", "memory.automation.create_report", "automation", false},
    {"MEMORY OPTIMIZER SHOW", "memory.optimizer.show", "SBLR_MEMORY_OPTIMIZER_SHOW", "memory.automation.review_recommendation", "automation", false},
    {"MEMORY OPTIMIZER SET", "memory.optimizer.set", "SBLR_MEMORY_OPTIMIZER_SET", "memory.automation.apply_safe_recommendation", "automation", true},
    {"MEMORY OPTIMIZER RUN", "memory.optimizer.run", "SBLR_MEMORY_OPTIMIZER_RUN", "memory.automation.apply_safe_recommendation", "automation", true},
    {"MEMORY OBJECT RESIDENCY SHOW customer_table", "memory.object_residency.show", "SBLR_MEMORY_OBJECT_RESIDENCY_SHOW", "memory.object_residency.inspect", "object_residency", false},
    {"MEMORY OBJECT RESIDENCY SET customer_table", "memory.object_residency.set", "SBLR_MEMORY_OBJECT_RESIDENCY_SET", "memory.object_residency.set", "object_residency", true},
    {"MEMORY RATE LIMIT SHOW", "memory.rate_limit.show", "SBLR_MEMORY_RATE_LIMIT_SHOW", "memory.rate_limit.inspect", "rate_limit", false},
    {"MEMORY RATE LIMIT SET", "memory.rate_limit.set", "SBLR_MEMORY_RATE_LIMIT_SET", "memory.rate_limit.set", "rate_limit", true},
    {"MEMORY POLICY UPGRADE PLAN", "memory.policy_upgrade.plan", "SBLR_MEMORY_POLICY_UPGRADE_PLAN", "memory.policy_upgrade.plan", "policy_upgrade", false},
    {"MEMORY POLICY MIGRATE PLAN", "memory.policy_migration.plan", "SBLR_MEMORY_POLICY_MIGRATION_PLAN", "memory.policy_migration.plan", "policy_upgrade", true},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string Message(const MemoryRouteRow& row, std::string_view phase, std::string_view text) {
  return std::string(row.operation_id) + " " + std::string(phase) + ": " +
         std::string(text);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f1000-0000-7000-8000-000000001001";
  session.connection_uuid = "019f1000-0000-7000-8000-000000001002";
  session.database_uuid = "019f1000-0000-7000-8000-000000001003";
  session.catalog_epoch = 101;
  session.security_policy_epoch = 102;
  session.descriptor_epoch = 103;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f1000-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@memory-management-exact-route";
  config.build_id = "sbsql-memory-management-exact-route";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireExactLowering(const MemoryRouteRow& row) {
  const auto artifacts = RunPipeline(row.sql);
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (artifacts.envelope.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.envelope.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), Message(row, "parser", "CST failed"));
  Require(!artifacts.ast.messages.has_errors(), Message(row, "parser", "AST failed"));
  Require(artifacts.bound.bound, Message(row, "binder", "bind failed"));
  Require(!artifacts.envelope.messages.has_errors(), Message(row, "lowering", "lowering emitted errors"));
  Require(artifacts.verifier.admitted, Message(row, "verifier", "SBLR verifier rejected route"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          Message(row, "lowering", "operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          Message(row, "lowering", "opcode mismatch"));
  Require(artifacts.envelope.operation_family == "sblr.management.runtime_operation.v3",
          Message(row, "lowering", "operation family mismatch"));
  Require(artifacts.envelope.result_shape_key == "rs.memory.management.descriptor_plan.v1",
          Message(row, "lowering", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == "EnginePlanMemoryManagementOperation",
          Message(row, "lowering", "engine API function mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.memory_management_descriptor_api_required"),
          Message(row, "lowering", "memory descriptor authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          Message(row, "lowering", "parser no-SQL-execution authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          Message(row, "lowering", "parser no-finality authority missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          Message(row, "payload", "exact-command evidence missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"engine_api_function\":\"EnginePlanMemoryManagementOperation\""),
          Message(row, "payload", "engine API function missing"));
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          Message(row, "payload", "parser execution flag missing"));
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          Message(row, "payload", "cluster/private dispatch exclusion missing"));
  Require(Contains(artifacts.envelope.payload, "\"wal_recovery_authority\":false") &&
              Contains(artifacts.envelope.payload, "\"recovery_authority\":false"),
          Message(row, "payload", "WAL/recovery exclusion missing"));
  Require(!Contains(artifacts.envelope.payload, row.sql),
          Message(row, "payload", "source SQL text leaked into payload"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, Message(row, "server_admission", "admission rejected route"));
  Require(admission.requires_public_abi_dispatch,
          Message(row, "server_admission", "public ABI dispatch not required"));
  Require(admission.operation_id == row.operation_id,
          Message(row, "server_admission", "operation id mismatch"));
  Require(admission.operation_family == (row.mutation ? "sblr.management.control.v3"
                                                      : "sblr.management.report.v3"),
          Message(row, "server_admission", "public family mismatch"));
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-memory-management-exact-route";
  context.security_context_present = true;
  context.database_path = "/tmp/sbsql_memory_management_exact_route.sbdb";
  context.database_uuid.canonical = "019f1000-0000-7000-8000-000000002001";
  context.session_uuid.canonical = "019f1000-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f1000-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f1000-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f1000-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f1000-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f1000-0000-7000-8000-000000002007";
  context.local_transaction_id = 9101;
  context.catalog_generation_id = 101;
  context.security_epoch = 102;
  context.resource_epoch = 103;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  context.trace_tags.push_back("right:OBS_CONFIG_CONTROL");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const MemoryRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.memory.") +
                                             std::string(row.operation_id));
  envelope.result_shape = "rs.memory.management.descriptor_plan.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", "019f1000-0000-7000-8000-000000002100"});
  envelope.operands.push_back({"text", "target_object_kind", "table"});
  return envelope;
}

void RequireRegistryAndDispatch(const MemoryRouteRow& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, Message(row, "sblr_registry", "operation missing"));
  Require(entry->opcode == row.opcode, Message(row, "sblr_registry", "opcode mismatch"));
  Require(!entry->requires_cluster_authority,
          Message(row, "sblr_registry", "unexpected cluster authority"));

  const auto dispatch = sblr::DispatchSblrOperation(
      {EngineContext(), EngineEnvelope(row), api::EngineApiRequest{}});
  for (const auto& diagnostic : dispatch.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : dispatch.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(dispatch.envelope_validated, Message(row, "engine_dispatch", "envelope rejected"));
  Require(dispatch.accepted, Message(row, "engine_dispatch", "dispatch not accepted"));
  Require(dispatch.dispatched_to_api, Message(row, "engine_dispatch", "not dispatched to API"));
  Require(dispatch.api_result.ok, Message(row, "engine_dispatch", "API returned failure"));
  Require(dispatch.api_result.operation_id == row.operation_id,
          Message(row, "engine_dispatch", "public operation id not preserved"));
  Require(dispatch.api_result.result_shape.result_kind ==
              "rs.memory.management.descriptor_plan.v1",
          Message(row, "engine_dispatch", "result shape mismatch"));
  Require(HasEvidence(dispatch.api_result,
                      "memory_management_descriptor_plan",
                      row.planner_operation),
          Message(row, "engine_dispatch", "planner operation evidence missing"));
  Require(HasEvidence(dispatch.api_result, "memory_management_family", row.planner_family),
          Message(row, "engine_dispatch", "planner family evidence missing"));
  Require(HasEvidence(dispatch.api_result, "parser_memory_authority", "false"),
          Message(row, "engine_dispatch", "parser memory authority was granted"));
  Require(HasEvidence(dispatch.api_result, "transaction_finality_authority", "false"),
          Message(row, "engine_dispatch", "transaction finality authority was granted"));
  Require(HasEvidence(dispatch.api_result, "recovery_authority", "false"),
          Message(row, "engine_dispatch", "recovery authority was granted"));
  Require(HasEvidence(dispatch.api_result, "donor_wal_recovery_authority", "false"),
          Message(row, "engine_dispatch", "donor/WAL recovery authority was granted"));
  Require(HasEvidence(dispatch.api_result, "private_provider_dispatch", "false"),
          Message(row, "engine_dispatch", "private provider dispatch was granted"));
  Require(HasEvidence(dispatch.api_result, "physical_action_dispatched", "false"),
          Message(row, "engine_dispatch", "physical action was dispatched"));
}

}  // namespace

int main() {
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireRegistryAndDispatch(row);
  }
  std::cout << "sbsql_memory_management_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
