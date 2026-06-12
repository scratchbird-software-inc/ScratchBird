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

#include <algorithm>
#include <array>
#include <chrono>
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
namespace sblr = scratchbird::engine::sblr;

struct PackageRouteRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view required_right;
  std::string_view storage_executor;
  bool mutation;
  bool durable_state_changed;
};

constexpr std::array<PackageRouteRow, 5> kRows{{
    {"EXPORT FILESPACE PACKAGE", "filespace.package.export_manifest", "SBLR_FILESPACE_PACKAGE_EXPORT_MANIFEST", "right.observe", "ExportFilespacePackageManifest", false, false},
    {"INSPECT FILESPACE PACKAGE", "filespace.package.inspect_manifest", "SBLR_FILESPACE_PACKAGE_INSPECT_MANIFEST", "right.observe", "InspectFilespacePackageManifest", false, false},
    {"IMPORT FILESPACE PACKAGE", "filespace.package.import_to_quarantine", "SBLR_FILESPACE_PACKAGE_IMPORT_TO_QUARANTINE", "right.filespace.lifecycle_control", "ImportFilespacePackageToQuarantine", true, true},
    {"ADMIT FILESPACE PACKAGE", "filespace.package.admit", "SBLR_FILESPACE_PACKAGE_ADMIT", "right.filespace.lifecycle_control", "AdmitFilespacePackage", true, true},
    {"REJECT FILESPACE PACKAGE", "filespace.package.reject", "SBLR_FILESPACE_PACKAGE_REJECT", "right.filespace.lifecycle_control", "RejectFilespacePackage", true, true},
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

std::filesystem::path RuntimePackageIoPath() {
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return std::filesystem::temp_directory_path() /
         ("sbsql_filespace_package_runtime_" + std::to_string(millis) + ".sbpkg");
}

std::string Message(const PackageRouteRow& row,
                    std::string_view phase,
                    std::string_view text) {
  return std::string(row.operation_id) + " " + std::string(phase) + ": " +
         std::string(text);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f3100-0000-7000-8000-000000001001";
  session.connection_uuid = "019f3100-0000-7000-8000-000000001002";
  session.database_uuid = "019f3100-0000-7000-8000-000000001003";
  session.catalog_epoch = 311;
  session.security_policy_epoch = 312;
  session.descriptor_epoch = 313;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f3100-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@filespace-package-exact-route";
  config.build_id = "sbsql-filespace-package-exact-route";
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

void RequireExactLowering(const PackageRouteRow& row) {
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
  Require(artifacts.envelope.result_shape_key == "rs.filespace.package_report.v1",
          Message(row, "lowering", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == "EngineFilespacePackageOperation",
          Message(row, "lowering", "engine API function mismatch"));
  Require(artifacts.envelope.resource_contract_key ==
              "resource.contract.filespace_package",
          Message(row, "lowering", "filespace package resource contract mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.filespace_package_api_required"),
          Message(row, "lowering", "filespace package authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          Message(row, "lowering", "parser no-SQL-execution authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          Message(row, "lowering", "parser no-finality authority missing"));
  Require(HasValue(artifacts.envelope.required_rights, row.required_right),
          Message(row, "lowering", "required right missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          Message(row, "payload", "exact-command evidence missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"engine_api_function\":\"EngineFilespacePackageOperation\""),
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

api::EngineRequestContext EngineContext(const PackageRouteRow& row) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-filespace-package-exact-route";
  context.security_context_present = true;
  context.database_path = "/tmp/sbsql_filespace_package_exact_route.sbdb";
  context.database_uuid.canonical = "019f3100-0000-7000-8000-000000002001";
  context.session_uuid.canonical = "019f3100-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f3100-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f3100-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f3100-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f3100-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f3100-0000-7000-8000-000000002007";
  context.catalog_generation_id = 311;
  context.security_epoch = 312;
  context.resource_epoch = 313;
  context.local_transaction_id = row.mutation ? 701 : 0;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:OBS_CONFIG_INSPECT");
  context.trace_tags.push_back("right:FILESPACE_LIFECYCLE_CONTROL");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const PackageRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.filespace.package.") +
                                             std::string(row.operation_id));
  envelope.result_shape = "rs.filespace.package_report.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.mutation;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireRegistryAndDispatch(const PackageRouteRow& row) {
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
              "rs.filespace.package_report.v1",
          Message(row, "engine_dispatch", "result shape mismatch"));
  Require(HasEvidence(dispatch.api_result, "filespace_package_report", row.operation_id),
          Message(row, "engine_dispatch", "package report evidence missing"));
  Require(HasEvidence(dispatch.api_result, "filespace_package_operation", row.operation_id),
          Message(row, "engine_dispatch", "package operation evidence missing"));
  Require(HasEvidence(dispatch.api_result, "filespace_package_member_count", "1"),
          Message(row, "engine_dispatch", "package member count evidence missing"));
  Require(HasRowField(dispatch.api_result, "storage_executor", row.storage_executor),
          Message(row, "engine_dispatch", "storage executor mismatch"));
  Require(HasEvidence(dispatch.api_result,
                      "durable_state_changed",
                      row.durable_state_changed ? "true" : "false"),
          Message(row, "engine_dispatch", "durable-state evidence mismatch"));
  Require(HasEvidence(dispatch.api_result, "runtime_package_file_io_executed", "false"),
          Message(row, "engine_dispatch", "runtime package file IO executed"));
  Require(HasEvidence(dispatch.api_result, "physical_package_transfer_executed", "false"),
          Message(row, "engine_dispatch", "physical package transfer executed"));
  Require(HasEvidence(dispatch.api_result, "physical_package_member_count", "0"),
          Message(row, "engine_dispatch", "physical package member count mismatch"));
  Require(HasEvidence(dispatch.api_result, "physical_package_byte_count", "0"),
          Message(row, "engine_dispatch", "physical package byte count mismatch"));
  Require(HasEvidence(dispatch.api_result, "parser_file_io_authority", "false"),
          Message(row, "engine_dispatch", "parser file IO authority was granted"));
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

void RequireRuntimePackageFileIoDirectEngineApi() {
  const auto path = RuntimePackageIoPath();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{path};

  api::EngineFilespacePackageRequest write_request;
  write_request.context = EngineContext(kRows[0]);
  write_request.package_operation = api::EngineFilespacePackageAction::export_manifest;
  write_request.runtime_package_file_io_requested = true;
  write_request.package_file_write_requested = true;
  write_request.package_file_allow_overwrite = true;
  write_request.package_file_path = path.string();
  const auto written = api::EngineFilespacePackageOperation(write_request);
  for (const auto& diagnostic : written.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(written.ok, "direct package manifest file write failed");
  Require(written.runtime_package_file_io_executed,
          "direct package manifest file write did not execute runtime IO");
  Require(written.member_count == 1, "direct package manifest file write member count mismatch");
  Require(!written.durable_state_changed,
          "direct package manifest file write reported durable state change");
  Require(HasRowField(written, "storage_executor", "WriteFilespacePackageFile"),
          "direct package manifest file write storage executor mismatch");
  Require(HasEvidence(written, "runtime_package_file_io_executed", "true"),
          "direct package manifest file write runtime IO evidence missing");
  Require(HasEvidence(written, "physical_package_transfer_executed", "false"),
          "direct package manifest file write physical transfer evidence missing");
  Require(HasEvidence(written, "encrypted_material_included", "false"),
          "direct package manifest file write encrypted material evidence missing");
  Require(HasEvidence(written, "parser_file_io_authority", "false"),
          "direct package manifest file write parser authority evidence missing");
  Require(HasEvidence(written,
                      "mga_visibility_authority",
                      "durable_transaction_inventory"),
          "direct package manifest file write MGA evidence missing");
  Require(std::filesystem::exists(path),
          "direct package manifest file write did not create file");

  api::EngineFilespacePackageRequest read_request;
  read_request.context = EngineContext(kRows[1]);
  read_request.package_operation = api::EngineFilespacePackageAction::inspect_manifest;
  read_request.runtime_package_file_io_requested = true;
  read_request.package_file_read_requested = true;
  read_request.package_file_path = path.string();
  const auto read = api::EngineFilespacePackageOperation(read_request);
  for (const auto& diagnostic : read.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(read.ok, "direct package manifest file read failed");
  Require(read.runtime_package_file_io_executed,
          "direct package manifest file read did not execute runtime IO");
  Require(read.member_count == 1, "direct package manifest file read member count mismatch");
  Require(!read.durable_state_changed,
          "direct package manifest file read reported durable state change");
  Require(HasRowField(read, "storage_executor", "ReadFilespacePackageFile"),
          "direct package manifest file read storage executor mismatch");
  Require(HasEvidence(read, "runtime_package_file_io_executed", "true"),
          "direct package manifest file read runtime IO evidence missing");
  Require(HasEvidence(read, "physical_package_transfer_executed", "false"),
          "direct package manifest file read physical transfer evidence missing");
  Require(HasEvidence(read, "encrypted_material_included", "false"),
          "direct package manifest file read encrypted material evidence missing");
  Require(HasEvidence(read, "parser_file_io_authority", "false"),
          "direct package manifest file read parser authority evidence missing");

  auto refused = write_request;
  refused.parser_file_io_authority = true;
  const auto refused_result = api::EngineFilespacePackageOperation(refused);
  Require(!refused_result.ok, "direct package file IO accepted parser file authority");
  Require(HasRowField(refused_result, "runtime_package_file_io_executed", "false"),
          "parser-authority refusal executed package file IO");
}

void RequireRuntimePhysicalPackageTransferDirectEngineApi() {
  const auto path = RuntimePackageIoPath();
  const auto source_path = std::filesystem::path("/tmp/scratchbird-package-member.fsp");
  const auto restore_dir = path.parent_path() /
                           (path.filename().string() + ".restore");
  struct Cleanup {
    std::filesystem::path package_path;
    std::filesystem::path source_path;
    std::filesystem::path restore_dir;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(package_path, ignored);
      std::filesystem::remove_all(package_path.parent_path() /
                                      (package_path.filename().string() + ".members"),
                                  ignored);
      std::filesystem::remove_all(restore_dir, ignored);
      std::filesystem::remove(source_path, ignored);
    }
  } cleanup{path, source_path, restore_dir};

  {
    std::ofstream source(source_path, std::ios::binary | std::ios::trunc);
    source << "scratchbird direct runtime physical package member";
  }

  api::EngineFilespacePackageRequest write_request;
  write_request.context = EngineContext(kRows[0]);
  write_request.package_operation = api::EngineFilespacePackageAction::export_manifest;
  write_request.runtime_package_file_io_requested = true;
  write_request.package_file_write_requested = true;
  write_request.package_file_allow_overwrite = true;
  write_request.runtime_physical_package_transfer_requested = true;
  write_request.allow_physical_package_transfer = true;
  write_request.package_file_path = path.string();
  const auto written = api::EngineFilespacePackageOperation(write_request);
  for (const auto& diagnostic : written.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(written.ok, "direct physical package transfer write failed");
  Require(written.runtime_package_file_io_executed,
          "direct physical package transfer write did not execute runtime IO");
  Require(written.physical_package_transfer_executed,
          "direct physical package transfer write did not execute transfer");
  Require(written.physical_package_member_count == 1,
          "direct physical package transfer write member count mismatch");
  Require(written.physical_package_byte_count > 0,
          "direct physical package transfer write byte count missing");
  Require(HasEvidence(written, "physical_package_transfer_executed", "true"),
          "direct physical package transfer write evidence missing");
  Require(HasEvidence(written, "physical_package_member_count", "1"),
          "direct physical package transfer write member count evidence missing");
  Require(std::filesystem::exists(
              path.parent_path() / (path.filename().string() + ".members")),
          "direct physical package transfer write member directory missing");

  api::EngineFilespacePackageRequest read_request;
  read_request.context = EngineContext(kRows[1]);
  read_request.package_operation = api::EngineFilespacePackageAction::inspect_manifest;
  read_request.runtime_package_file_io_requested = true;
  read_request.package_file_read_requested = true;
  read_request.runtime_physical_package_transfer_requested = true;
  read_request.allow_physical_package_transfer = true;
  read_request.package_file_path = path.string();
  read_request.physical_package_transfer_directory = restore_dir.string();
  const auto read = api::EngineFilespacePackageOperation(read_request);
  for (const auto& diagnostic : read.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(read.ok, "direct physical package transfer read failed");
  Require(read.runtime_package_file_io_executed,
          "direct physical package transfer read did not execute runtime IO");
  Require(read.physical_package_transfer_executed,
          "direct physical package transfer read did not execute transfer");
  Require(read.physical_package_member_count == 1,
          "direct physical package transfer read member count mismatch");
  Require(read.physical_package_byte_count == written.physical_package_byte_count,
          "direct physical package transfer read byte count mismatch");
  Require(HasEvidence(read, "physical_package_transfer_executed", "true"),
          "direct physical package transfer read evidence missing");
  Require(HasEvidence(read, "physical_package_member_count", "1"),
          "direct physical package transfer read member count evidence missing");
  bool restored_member_found = false;
  for (const auto& entry : std::filesystem::directory_iterator(restore_dir)) {
    restored_member_found =
        restored_member_found || entry.path().extension() == ".fsp";
  }
  Require(restored_member_found,
          "direct physical package transfer read did not restore member file");
}

}  // namespace

int main() {
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireRegistryAndDispatch(row);
  }
  RequireRuntimePackageFileIoDirectEngineApi();
  RequireRuntimePhysicalPackageTransferDirectEngineApi();
  std::cout << "sbsql_filespace_package_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
