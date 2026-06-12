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

struct ShardPlacementRouteRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view placement_operation;
  std::string_view required_right;
  std::string_view expected_state;
  bool mutation;
  bool physical_movement_required;
};

constexpr std::array<ShardPlacementRouteRow, 12> kRows{{
    {"SHARD PLACEMENT CREATE", "shard_placement.create", "SBLR_SHARD_PLACEMENT_CREATE", "create", "right.filespace.lifecycle_control", "created_descriptor", true, false},
    {"SHARD PLACEMENT VERIFY", "shard_placement.verify", "SBLR_SHARD_PLACEMENT_VERIFY", "verify", "right.observe", "verified_descriptor", false, false},
    {"SHARD PLACEMENT MOVE", "shard_placement.move", "SBLR_SHARD_PLACEMENT_MOVE", "move", "right.filespace.lifecycle_control", "move_planned", true, true},
    {"SHARD PLACEMENT SPLIT", "shard_placement.split", "SBLR_SHARD_PLACEMENT_SPLIT", "split", "right.filespace.lifecycle_control", "split_planned", true, true},
    {"SHARD PLACEMENT MERGE", "shard_placement.merge", "SBLR_SHARD_PLACEMENT_MERGE", "merge", "right.filespace.lifecycle_control", "merge_planned", true, true},
    {"SHARD PLACEMENT REBALANCE", "shard_placement.rebalance", "SBLR_SHARD_PLACEMENT_REBALANCE", "rebalance", "right.filespace.lifecycle_control", "rebalance_planned", true, true},
    {"SHARD PLACEMENT FREEZE", "shard_placement.freeze", "SBLR_SHARD_PLACEMENT_FREEZE", "freeze", "right.filespace.lifecycle_control", "frozen", true, false},
    {"SHARD PLACEMENT ARCHIVE", "shard_placement.archive", "SBLR_SHARD_PLACEMENT_ARCHIVE", "archive", "right.filespace.lifecycle_control", "archived", true, false},
    {"SHARD PLACEMENT REATTACH", "shard_placement.reattach", "SBLR_SHARD_PLACEMENT_REATTACH", "reattach", "right.filespace.lifecycle_control", "reattach_planned", true, true},
    {"SHARD PLACEMENT QUARANTINE", "shard_placement.quarantine", "SBLR_SHARD_PLACEMENT_QUARANTINE", "quarantine", "right.filespace.lifecycle_control", "quarantined", true, false},
    {"SHARD PLACEMENT RECONCILE", "shard_placement.reconcile", "SBLR_SHARD_PLACEMENT_RECONCILE", "reconcile", "right.filespace.lifecycle_control", "reconcile_planned", true, false},
    {"SHARD PLACEMENT DROP", "shard_placement.drop", "SBLR_SHARD_PLACEMENT_DROP", "drop", "right.filespace.lifecycle_control", "drop_planned", true, false},
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

bool HasRowField(const api::EngineApiResult& result,
                 std::string_view field,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& cell : row.fields) {
      if (cell.first == field && cell.second.encoded_value == value) return true;
    }
  }
  return false;
}

std::string Message(const ShardPlacementRouteRow& row,
                    std::string_view phase,
                    std::string_view text) {
  return std::string(row.operation_id) + " " + std::string(phase) + ": " +
         std::string(text);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f4100-0000-7000-8000-000000001001";
  session.connection_uuid = "019f4100-0000-7000-8000-000000001002";
  session.database_uuid = "019f4100-0000-7000-8000-000000001003";
  session.catalog_epoch = 411;
  session.security_policy_epoch = 412;
  session.descriptor_epoch = 413;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f4100-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@shard-placement-exact-route";
  config.build_id = "sbsql-shard-placement-exact-route";
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

void RequireExactLowering(const ShardPlacementRouteRow& row) {
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
  Require(artifacts.envelope.result_shape_key == "rs.shard_placement.descriptor_plan.v1",
          Message(row, "lowering", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == "EnginePlanShardPlacementOperation",
          Message(row, "lowering", "engine API function mismatch"));
  Require(artifacts.envelope.resource_contract_key ==
              "resource.contract.shard_placement_descriptor",
          Message(row, "lowering", "shard placement resource contract mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.shard_placement_descriptor_api_required"),
          Message(row, "lowering", "shard placement authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          Message(row, "lowering", "parser no-SQL-execution authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          Message(row, "lowering", "parser no-finality authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          Message(row, "lowering", "cluster provider exclusion missing"));
  Require(HasValue(artifacts.envelope.required_rights, row.required_right),
          Message(row, "lowering", "required right missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.storage.shard_placement"),
          Message(row, "lowering", "shard placement descriptor ref missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          Message(row, "payload", "exact-command evidence missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"engine_api_function\":\"EnginePlanShardPlacementOperation\""),
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
  }
  Require(admission.admitted, Message(row, "server_admission", "admission rejected route"));
  Require(admission.requires_public_abi_dispatch,
          Message(row, "server_admission", "public ABI dispatch not required"));
  Require(admission.operation_id == row.operation_id,
          Message(row, "server_admission", "operation id mismatch"));
  Require(admission.operation_family == "sblr.storage.management_operation.v3",
          Message(row, "server_admission", "public family mismatch"));
}

api::EngineRequestContext EngineContext(const ShardPlacementRouteRow& row) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-shard-placement-exact-route";
  context.security_context_present = true;
  context.database_path = "/tmp/sbsql_shard_placement_exact_route.sbdb";
  context.database_uuid.canonical = "019f4100-0000-7000-8000-000000002001";
  context.session_uuid.canonical = "019f4100-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f4100-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f4100-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f4100-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f4100-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f4100-0000-7000-8000-000000002007";
  context.catalog_generation_id = 411;
  context.security_epoch = 412;
  context.resource_epoch = 413;
  context.local_transaction_id = row.mutation ? 801 : 0;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const ShardPlacementRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.shard_placement.") +
                                             std::string(row.operation_id));
  envelope.result_shape = "rs.shard_placement.descriptor_plan.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.mutation;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireRegistryAndDispatch(const ShardPlacementRouteRow& row) {
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
  Require(dispatch.api_result.operation_id == row.operation_id,
          Message(row, "engine_dispatch", "operation id mismatch"));
  Require(dispatch.api_result.result_shape.result_kind ==
              "rs.shard_placement.descriptor_plan.v1",
          Message(row, "engine_dispatch", "result shape mismatch"));
  Require(HasEvidence(dispatch.api_result, "shard_placement_operation",
                      row.placement_operation),
          Message(row, "engine_dispatch", "operation evidence missing"));
  Require(HasEvidence(dispatch.api_result, "shard_placement_state",
                      row.expected_state),
          Message(row, "engine_dispatch", "placement state evidence missing"));
  Require(HasRowField(dispatch.api_result, "route_operation_id", row.operation_id),
          Message(row, "engine_dispatch", "route operation id row missing"));
  Require(HasRowField(dispatch.api_result, "placement_operation",
                      row.placement_operation),
          Message(row, "engine_dispatch", "placement operation row missing"));
  Require(HasRowField(dispatch.api_result, "placement_state", row.expected_state),
          Message(row, "engine_dispatch", "placement state row missing"));
  Require(HasRowField(dispatch.api_result, "physical_data_movement_required",
                      row.physical_movement_required ? "true" : "false"),
          Message(row, "engine_dispatch", "physical movement requirement mismatch"));
  Require(HasEvidence(dispatch.api_result, "durable_state_changed", "false"),
          Message(row, "engine_dispatch", "durable state changed"));
  Require(HasEvidence(dispatch.api_result, "physical_data_movement_dispatched", "false"),
          Message(row, "engine_dispatch", "physical data movement dispatched"));
  Require(HasEvidence(dispatch.api_result, "parser_storage_authority", "false"),
          Message(row, "engine_dispatch", "parser storage authority was granted"));
  Require(HasEvidence(dispatch.api_result, "transaction_finality_authority", "false"),
          Message(row, "engine_dispatch", "transaction finality authority was granted"));
  Require(HasEvidence(dispatch.api_result, "recovery_authority", "false"),
          Message(row, "engine_dispatch", "recovery authority was granted"));
  Require(HasEvidence(dispatch.api_result, "reference_wal_recovery_authority", "false"),
          Message(row, "engine_dispatch", "reference/WAL recovery authority was granted"));
  Require(HasEvidence(dispatch.api_result, "private_cluster_execution", "false"),
          Message(row, "engine_dispatch", "private cluster execution was granted"));
  Require(HasEvidence(dispatch.api_result, "cluster_provider_dispatch", "false"),
          Message(row, "engine_dispatch", "cluster provider dispatch was granted"));
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
  std::cout << "sbsql_shard_placement_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
