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

struct StorageTierRouteRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view planner_operation;
  bool mutation;
};

constexpr std::array<StorageTierRouteRow, 6> kRows{{
    {"STORAGE TIER INSPECT", "storage_tier.inspect", "SBLR_STORAGE_TIER_INSPECT", "storage_tier.inspect", false},
    {"STORAGE TIER VALIDATE", "storage_tier.validate", "SBLR_STORAGE_TIER_VALIDATE", "storage_tier.validate", false},
    {"STORAGE TIER PLAN MIGRATION", "storage_tier.plan_migration", "SBLR_STORAGE_TIER_PLAN_MIGRATION", "storage_tier.plan_migration", false},
    {"STORAGE TIER STAGE MIGRATION", "storage_tier.stage_migration", "SBLR_STORAGE_TIER_STAGE_MIGRATION", "storage_tier.stage_migration", true},
    {"STORAGE TIER COMMIT MIGRATION", "storage_tier.commit_migration", "SBLR_STORAGE_TIER_COMMIT_MIGRATION", "storage_tier.commit_migration", true},
    {"STORAGE TIER ROLLBACK MIGRATION", "storage_tier.rollback_migration", "SBLR_STORAGE_TIER_ROLLBACK_MIGRATION", "storage_tier.rollback_migration", true},
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

std::string Message(const StorageTierRouteRow& row,
                    std::string_view phase,
                    std::string_view text) {
  return std::string(row.operation_id) + " " + std::string(phase) + ": " +
         std::string(text);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f2000-0000-7000-8000-000000001001";
  session.connection_uuid = "019f2000-0000-7000-8000-000000001002";
  session.database_uuid = "019f2000-0000-7000-8000-000000001003";
  session.catalog_epoch = 201;
  session.security_policy_epoch = 202;
  session.descriptor_epoch = 203;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f2000-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@storage-tier-exact-route";
  config.build_id = "sbsql-storage-tier-exact-route";
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

void RequireExactLowering(const StorageTierRouteRow& row) {
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
  Require(artifacts.envelope.operation_family == "sblr.storage.management_operation.v3",
          Message(row, "lowering", "operation family mismatch"));
  Require(artifacts.envelope.result_shape_key == "rs.storage_tier.descriptor_plan.v1",
          Message(row, "lowering", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == "EnginePlanStorageTierMigrationOperation",
          Message(row, "lowering", "engine API function mismatch"));
  Require(artifacts.envelope.resource_contract_key ==
              "resource.contract.storage_tier_descriptor",
          Message(row, "lowering", "storage-tier resource contract mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.storage_tier_descriptor_api_required"),
          Message(row, "lowering", "storage-tier descriptor authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          Message(row, "lowering", "parser no-SQL-execution authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          Message(row, "lowering", "parser no-finality authority missing"));
  Require(HasValue(artifacts.envelope.required_rights,
                   row.mutation ? "right.filespace.lifecycle_control" : "right.observe"),
          Message(row, "lowering", "required right mismatch"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          Message(row, "payload", "exact-command evidence missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"engine_api_function\":\"EnginePlanStorageTierMigrationOperation\""),
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
  for (const auto& diagnostic : admission.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.key << '=' << field.value << '\n';
    }
  }
  Require(admission.admitted, Message(row, "server_admission", "admission rejected route"));
  Require(admission.requires_public_abi_dispatch,
          Message(row, "server_admission", "public ABI dispatch not required"));
  Require(admission.operation_id == row.operation_id,
          Message(row, "server_admission", "operation id mismatch"));
  Require(admission.operation_family == "sblr.storage.management_operation.v3",
          Message(row, "server_admission", "public family mismatch"));
}

api::EngineRequestContext EngineContext(const StorageTierRouteRow& row) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-storage-tier-exact-route";
  context.security_context_present = true;
  context.database_path = "/tmp/sbsql_storage_tier_exact_route.sbdb";
  context.database_uuid.canonical = "019f2000-0000-7000-8000-000000002001";
  context.session_uuid.canonical = "019f2000-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f2000-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f2000-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f2000-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f2000-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f2000-0000-7000-8000-000000002007";
  context.local_transaction_id = row.mutation ? 9201 : 0;
  context.catalog_generation_id = 201;
  context.security_epoch = 202;
  context.resource_epoch = 203;
  context.trace_tags.push_back("security.bootstrap");
  if (row.mutation) {
    context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  } else {
    context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  }
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const StorageTierRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.storage_tier.") +
                                             std::string(row.operation_id));
  envelope.result_shape = "rs.storage_tier.descriptor_plan.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", "019f2000-0000-7000-8000-000000002100"});
  envelope.operands.push_back({"text", "target_object_kind", "filespace"});
  return envelope;
}

void RequireRegistryAndDispatch(const StorageTierRouteRow& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, Message(row, "sblr_registry", "operation missing"));
  Require(entry->opcode == row.opcode, Message(row, "sblr_registry", "opcode mismatch"));
  Require(!entry->requires_cluster_authority,
          Message(row, "sblr_registry", "unexpected cluster authority"));

  const auto dispatch = sblr::DispatchSblrOperation(
      {EngineContext(row), EngineEnvelope(row), api::EngineApiRequest{}});
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
  Require(dispatch.api_result.operation_id == row.planner_operation,
          Message(row, "engine_dispatch", "planner operation id mismatch"));
  Require(dispatch.api_result.result_shape.result_kind ==
              "rs.storage_tier.descriptor_plan.v1",
          Message(row, "engine_dispatch", "result shape mismatch"));
  Require(HasEvidence(dispatch.api_result,
                      "storage_tier_descriptor_plan",
                      row.planner_operation),
          Message(row, "engine_dispatch", "planner operation evidence missing"));
  Require(HasEvidence(dispatch.api_result, "parser_storage_authority", "false"),
          Message(row, "engine_dispatch", "parser storage authority was granted"));
  Require(HasEvidence(dispatch.api_result, "durable_state_changed", "false"),
          Message(row, "engine_dispatch", "durable state changed"));
  Require(HasEvidence(dispatch.api_result, "physical_data_movement_dispatched", "false"),
          Message(row, "engine_dispatch", "physical movement dispatched"));
  Require(HasEvidence(dispatch.api_result, "private_provider_dispatch", "false"),
          Message(row, "engine_dispatch", "private provider dispatch was granted"));
  Require(HasEvidence(dispatch.api_result,
                      "mga_visibility_authority",
                      "durable_transaction_inventory"),
          Message(row, "engine_dispatch", "MGA visibility evidence missing"));
}

}  // namespace

int main() {
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireRegistryAndDispatch(row);
  }
  std::cout << "sbsql_storage_tier_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
