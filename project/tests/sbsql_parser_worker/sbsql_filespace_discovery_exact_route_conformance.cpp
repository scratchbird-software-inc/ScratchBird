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
#include "storage/storage_management_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace filespace = scratchbird::storage::filespace;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

struct DiscoveryRouteRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view scope;
  std::string_view required_classification;
};

constexpr std::array<DiscoveryRouteRow, 3> kRows{{
    {"DISCOVER FILESPACE ANOMALIES", "filespace.discovery.scan", "SBLR_FILESPACE_DISCOVERY_SCAN", "all", "foreign_orphan"},
    {"DISCOVER ORPHAN FILESPACES", "filespace.discovery.orphan_scan", "SBLR_FILESPACE_DISCOVERY_ORPHAN_SCAN", "orphan_only", "foreign_orphan"},
    {"DISCOVER STALE FILESPACES", "filespace.discovery.stale_scan", "SBLR_FILESPACE_DISCOVERY_STALE_SCAN", "stale_only", "stale_header"},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  Require(generated.ok(), "uuid generation failed");
  return generated.value;
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

std::string Message(const DiscoveryRouteRow& row,
                    std::string_view phase,
                    std::string_view text) {
  return std::string(row.operation_id) + " " + std::string(phase) + ": " +
         std::string(text);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f3000-0000-7000-8000-000000001001";
  session.connection_uuid = "019f3000-0000-7000-8000-000000001002";
  session.database_uuid = "019f3000-0000-7000-8000-000000001003";
  session.catalog_epoch = 301;
  session.security_policy_epoch = 302;
  session.descriptor_epoch = 303;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f3000-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@filespace-discovery-exact-route";
  config.build_id = "sbsql-filespace-discovery-exact-route";
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

void RequireExactLowering(const DiscoveryRouteRow& row) {
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
  Require(artifacts.envelope.operation_family == "sblr.filespace.management.v3",
          Message(row, "lowering", "operation family mismatch"));
  Require(artifacts.envelope.result_shape_key == "rs.filespace.discovery_report.v1",
          Message(row, "lowering", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == "EngineDiscoverFilespaceAnomalies",
          Message(row, "lowering", "engine API function mismatch"));
  Require(artifacts.envelope.resource_contract_key ==
              "resource.contract.filespace_discovery_report",
          Message(row, "lowering", "filespace discovery resource contract mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.filespace_discovery_api_required"),
          Message(row, "lowering", "filespace discovery authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          Message(row, "lowering", "parser no-SQL-execution authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          Message(row, "lowering", "parser no-finality authority missing"));
  Require(HasValue(artifacts.envelope.required_rights, "right.observe"),
          Message(row, "lowering", "observe right missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          Message(row, "payload", "exact-command evidence missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"engine_api_function\":\"EngineDiscoverFilespaceAnomalies\""),
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
  Require(admission.operation_family == "sblr.filespace.management.v3",
          Message(row, "server_admission", "public family mismatch"));
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-filespace-discovery-exact-route";
  context.security_context_present = true;
  context.database_path = "/tmp/sbsql_filespace_discovery_exact_route.sbdb";
  context.database_uuid.canonical = "019f3000-0000-7000-8000-000000002001";
  context.session_uuid.canonical = "019f3000-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f3000-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f3000-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f3000-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f3000-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f3000-0000-7000-8000-000000002007";
  context.catalog_generation_id = 301;
  context.security_epoch = 302;
  context.resource_epoch = 303;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const DiscoveryRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.filespace.discovery.") +
                                             std::string(row.operation_id));
  envelope.result_shape = "rs.filespace.discovery_report.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireRegistryAndDispatch(const DiscoveryRouteRow& row) {
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
          Message(row, "engine_dispatch", "operation id mismatch"));
  Require(dispatch.api_result.result_shape.result_kind ==
              "rs.filespace.discovery_report.v1",
          Message(row, "engine_dispatch", "result shape mismatch"));
  Require(HasEvidence(dispatch.api_result, "filespace_discovery_report", row.operation_id),
          Message(row, "engine_dispatch", "discovery report evidence missing"));
  Require(HasEvidence(dispatch.api_result, "filespace_discovery_scope", row.scope),
          Message(row, "engine_dispatch", "discovery scope evidence missing"));
  Require(HasRowField(dispatch.api_result,
                      "filespace_discovery_classification",
                      row.required_classification),
          Message(row, "engine_dispatch", "required classification missing"));
  Require(HasEvidence(dispatch.api_result, "durable_state_changed", "false"),
          Message(row, "engine_dispatch", "durable state changed"));
  Require(HasEvidence(dispatch.api_result, "cleanup_or_quarantine_executed", "false"),
          Message(row, "engine_dispatch", "public route executed cleanup or quarantine"));
  Require(HasEvidence(dispatch.api_result, "release_executed", "false"),
          Message(row, "engine_dispatch", "public route executed release"));
  Require(HasEvidence(dispatch.api_result,
                      "filespace_discovery_physical_cleanup_execution_count",
                      "0"),
          Message(row, "engine_dispatch", "public route physical cleanup count mismatch"));
  Require(HasEvidence(dispatch.api_result, "physical_cleanup_executed", "false"),
          Message(row, "engine_dispatch", "public route executed physical cleanup"));
  Require(HasEvidence(dispatch.api_result, "physical_file_removed", "false"),
          Message(row, "engine_dispatch", "public route removed a physical file"));
  Require(HasEvidence(dispatch.api_result, "runtime_filesystem_scan_executed", "false"),
          Message(row, "engine_dispatch", "runtime filesystem scan executed"));
  Require(HasEvidence(dispatch.api_result, "parser_filesystem_authority", "false"),
          Message(row, "engine_dispatch", "parser filesystem authority was granted"));
  Require(HasEvidence(dispatch.api_result, "parser_storage_authority", "false"),
          Message(row, "engine_dispatch", "parser storage authority was granted"));
  Require(HasEvidence(dispatch.api_result, "transaction_finality_authority", "false"),
          Message(row, "engine_dispatch", "transaction finality authority was granted"));
  Require(HasEvidence(dispatch.api_result, "recovery_authority", "false"),
          Message(row, "engine_dispatch", "recovery authority was granted"));
  Require(HasEvidence(dispatch.api_result, "reference_wal_recovery_authority", "false"),
          Message(row, "engine_dispatch", "reference/WAL recovery authority was granted"));
  Require(HasEvidence(dispatch.api_result, "private_provider_dispatch", "false"),
          Message(row, "engine_dispatch", "private provider dispatch was granted"));
  Require(HasEvidence(dispatch.api_result,
                      "mga_visibility_authority",
	                      "durable_transaction_inventory"),
	          Message(row, "engine_dispatch", "MGA visibility evidence missing"));
}

void RequireRuntimeScanDirectEngineApi() {
  api::EngineFilespaceDiscoveryRequest request;
  request.context = EngineContext();
  request.discovery_scope = api::EngineFilespaceDiscoveryScope::all;
  request.runtime_filesystem_scan_requested = true;

  const auto result = api::EngineDiscoverFilespaceAnomalies(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.ok, "runtime_scan engine API returned failure");
  Require(result.runtime_filesystem_scan_executed,
          "runtime_scan engine API did not execute scan branch");
  Require(!result.durable_state_changed,
          "runtime_scan engine API reported durable state mutation");
  Require(!result.parser_filesystem_authority &&
              !result.parser_storage_authority &&
              !result.transaction_finality_authority &&
              !result.recovery_authority &&
              !result.reference_or_wal_recovery_authority &&
              !result.private_provider_dispatch,
          "runtime_scan engine API granted forbidden authority");
  Require(HasEvidence(result, "runtime_filesystem_scan_executed", "true"),
          "runtime_scan engine API evidence missing");
  Require(HasEvidence(result, "parser_filesystem_authority", "false"),
          "runtime_scan parser filesystem evidence mismatch");
  Require(HasEvidence(result, "durable_state_changed", "false"),
          "runtime_scan durable state evidence mismatch");
}

void RequireEngineOwnedQuarantineExecutionDirectApi() {
  api::EngineFilespaceDiscoveryRequest request;
  request.context = EngineContext();
  request.context.local_transaction_id = 701;
  request.context.snapshot_visible_through_local_transaction_id = 701;
  request.context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  request.discovery_scope = api::EngineFilespaceDiscoveryScope::all;
  request.mutation_requested = true;
  request.execute_quarantine_actions = true;
  request.physical_header_required_for_quarantine = false;

  const auto result = api::EngineDiscoverFilespaceAnomalies(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.ok, "engine-owned discovery quarantine execution failed");
  Require(result.durable_state_changed,
          "engine-owned discovery execution did not report durable state change");
  Require(result.cleanup_or_quarantine_executed,
          "engine-owned discovery execution did not report quarantine execution");
  Require(result.quarantine_execution_count == 1,
          "engine-owned discovery execution quarantine count mismatch");
  Require(result.release_execution_count == 0 && !result.release_executed,
          "engine-owned discovery execution unexpectedly released quarantine");
  Require(!result.parser_filesystem_authority &&
              !result.parser_storage_authority &&
              !result.transaction_finality_authority &&
              !result.recovery_authority &&
              !result.reference_or_wal_recovery_authority &&
              !result.private_provider_dispatch,
          "engine-owned discovery execution granted forbidden parser/recovery authority");
  Require(HasEvidence(result, "cleanup_or_quarantine_executed", "true"),
          "engine-owned discovery execution evidence missing");
  Require(HasEvidence(result, "durable_state_changed", "true"),
          "engine-owned discovery execution durable evidence missing");
  Require(HasEvidence(result,
                      "filespace_discovery_quarantine_execution_count",
                      "1"),
          "engine-owned discovery execution count evidence missing");
}

void RequireEngineOwnedPhysicalCleanupDirectApi() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("sbsql_discovery_cleanup_" + std::to_string(CurrentUnixMillis()));
  std::filesystem::create_directories(dir);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{dir};

  const auto now = CurrentUnixMillis();
  const auto database_uuid = MakeUuid(UuidKind::database, now + 10);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, now + 11);
  const auto writer_uuid = MakeUuid(UuidKind::object, now + 12);
  const auto path = dir / "engine-owned-cleanup.fsp";
  {
    std::ofstream file(path);
    file << "scratchbird engine-owned discovery cleanup fixture";
  }

  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.writer_identity_uuid = writer_uuid;
  descriptor.path = path.string();
  descriptor.role = filespace::FilespaceRole::import_candidate;
  descriptor.state = filespace::FilespaceState::quarantine;
  descriptor.page_size = 16384;
  descriptor.physical_filespace_id = 71;
  descriptor.header_generation = 1;
  descriptor.read_only = true;
  descriptor.active = false;

  api::EngineFilespaceDiscoveryRequest request;
  request.context = EngineContext();
  request.context.database_uuid.canonical = uuid::UuidToString(database_uuid.value);
  request.context.local_transaction_id = 702;
  request.context.snapshot_visible_through_local_transaction_id = 702;
  request.context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  request.discovery_scope = api::EngineFilespaceDiscoveryScope::all;
  request.expected_filespaces.push_back(descriptor);
  request.runtime_filesystem_scan_requested = true;
  request.runtime_scan_paths.push_back(path.string());
  request.mutation_requested = true;
  request.execute_physical_cleanup_actions = true;
  request.allow_physical_filespace_delete = true;
  request.physical_delete_legal_hold_clear = true;
  request.physical_delete_retention_satisfied = true;
  request.physical_delete_cleanup_horizon_authoritative = true;

  const auto result = api::EngineDiscoverFilespaceAnomalies(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.ok, "engine-owned discovery physical cleanup failed");
  Require(result.runtime_filesystem_scan_executed,
          "engine-owned discovery cleanup did not execute runtime scan");
  Require(result.durable_state_changed,
          "engine-owned discovery cleanup did not report durable state change");
  Require(result.cleanup_or_quarantine_executed,
          "engine-owned discovery cleanup did not report cleanup execution");
  Require(result.physical_cleanup_executed && result.physical_file_removed,
          "engine-owned discovery cleanup did not report physical file removal");
  Require(result.physical_cleanup_execution_count == 1,
          "engine-owned discovery cleanup count mismatch");
  Require(result.quarantine_execution_count == 0 && result.release_execution_count == 0,
          "engine-owned discovery cleanup executed quarantine or release");
  Require(!std::filesystem::exists(path),
          "engine-owned discovery cleanup left the physical file");
  Require(!result.parser_filesystem_authority &&
              !result.parser_storage_authority &&
              !result.transaction_finality_authority &&
              !result.recovery_authority &&
              !result.reference_or_wal_recovery_authority &&
              !result.private_provider_dispatch,
          "engine-owned discovery cleanup granted forbidden parser/recovery authority");
  Require(HasEvidence(result,
                      "filespace_discovery_physical_cleanup_execution_count",
                      "1"),
          "engine-owned discovery cleanup count evidence missing");
  Require(HasEvidence(result, "physical_cleanup_executed", "true"),
          "engine-owned discovery cleanup execution evidence missing");
  Require(HasEvidence(result, "physical_file_removed", "true"),
          "engine-owned discovery cleanup removal evidence missing");
  Require(HasEvidence(result, "durable_state_changed", "true"),
          "engine-owned discovery cleanup durable evidence missing");
}

}  // namespace

int main() {
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireRegistryAndDispatch(row);
  }
  RequireRuntimeScanDirectEngineApi();
  RequireEngineOwnedQuarantineExecutionDirectApi();
  RequireEngineOwnedPhysicalCleanupDirectApi();
  std::cout << "sbsql_filespace_discovery_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
