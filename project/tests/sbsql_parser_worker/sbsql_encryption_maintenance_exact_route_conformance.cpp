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
#include "security/protected_material_api.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kDatabaseUuid =
    "019f5000-0000-7000-8000-000000000001";
constexpr std::string_view kFilespaceUuid =
    "019f5000-0000-7000-8000-000000000002";
constexpr std::string_view kKeyUuid =
    "019f5000-0000-7000-8000-000000000003";
constexpr std::string_view kProtectedMaterialUuid =
    "019f5000-0000-7000-8000-000000000005";
constexpr std::string_view kProtectedMaterialVersionUuid =
    "019f5000-0000-7000-8000-000000000006";
constexpr std::string_view kDatabasePath =
    "/tmp/sbsql_encryption_maintenance_exact_route.sbdb";

struct EncryptionRouteRow {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view engine_api_function;
  std::string_view required_right;
  std::string_view expected_evidence_kind;
  bool mutation;
  bool seed_key_cache;
  bool seed_protected_material_version;
};

constexpr std::array<EncryptionRouteRow, 9> kRows{{
    {"ADMIT ENCRYPTION KEY", "security.encryption_key.admit", "SBLR_SECURITY_ENCRYPTION_KEY_ADMIT", "EngineAdmitEncryptionKey", "right.key_release_approve", "protected_material_key_admission", true, false, false},
    {"REKEY FILESPACE", "security.encryption_key.rotate", "SBLR_SECURITY_ENCRYPTION_KEY_ROTATE", "EngineRotateEncryptionKey", "right.key_release_approve", "protected_material_key_rotation", true, true, false},
    {"ALTER FILESPACE ENCRYPTION PROFILE", "security.encryption_key.rotate", "SBLR_SECURITY_ENCRYPTION_KEY_ROTATE", "EngineRotateEncryptionKey", "right.key_release_approve", "protected_material_key_rotation", true, true, false},
    {"SHOW PROTECTED MATERIAL CACHE", "security.protected_material_cache.inspect", "SBLR_SECURITY_PROTECTED_MATERIAL_CACHE_INSPECT", "EngineInspectProtectedMaterialCache", "right.protected_material_release", "protected_material_cache_inspect", false, true, false},
    {"PURGE PROTECTED MATERIAL CACHE", "security.protected_material_cache.purge", "SBLR_SECURITY_PROTECTED_MATERIAL_CACHE_PURGE", "EnginePurgeProtectedMaterial", "right.key_release_approve", "protected_material_cache_purge", true, true, false},
    {"SHUTDOWN PROTECTED MATERIAL CACHE", "security.protected_material_cache.shutdown", "SBLR_SECURITY_PROTECTED_MATERIAL_CACHE_SHUTDOWN", "EngineShutdownProtectedMaterial", "right.key_release_approve", "protected_material_shutdown_purge", true, true, false},
    {"OPEN ENCRYPTED FILESPACE", "security.encrypted_filespace.open", "SBLR_SECURITY_ENCRYPTED_FILESPACE_OPEN", "EngineOpenEncryptedFilespace", "right.protected_material_release", "encrypted_filespace_key_cache_hit", false, true, false},
    {"REQUEST KEY RELEASE", "security.request_protected_material", "SBLR_SECURITY_REQUEST_PROTECTED_MATERIAL", "EngineRequestProtectedMaterial", "right.protected_material_release", "protected_material_release", false, false, false},
    {"CRYPTOGRAPHIC ERASE FILESPACE", "security.protected_material.version.purge", "SBLR_SECURITY_PROTECTED_MATERIAL_VERSION_PURGE", "EnginePurgeProtectedMaterialVersion", "right.key_release_approve", "protected_material_purge", true, false, true},
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
                 std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
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

bool ResultContains(const api::EngineApiResult& result, std::string_view text) {
  for (const auto& evidence : result.evidence) {
    if (Contains(evidence.evidence_kind, text) || Contains(evidence.evidence_id, text)) {
      return true;
    }
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& cell : row.fields) {
      if (Contains(cell.first, text) || Contains(cell.second.encoded_value, text)) {
        return true;
      }
    }
  }
  return false;
}

std::string Message(const EncryptionRouteRow& row,
                    std::string_view phase,
                    std::string_view text) {
  return std::string(row.sql) + " " + std::string(phase) + ": " +
         std::string(text);
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f5100-0000-7000-8000-000000001001";
  session.connection_uuid = "019f5100-0000-7000-8000-000000001002";
  session.database_uuid = std::string(kDatabaseUuid);
  session.catalog_epoch = 511;
  session.security_policy_epoch = 512;
  session.descriptor_epoch = 513;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f5100-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@encryption-maintenance-exact-route";
  config.build_id = "sbsql-encryption-maintenance-exact-route";
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

void RequireExactLowering(const EncryptionRouteRow& row) {
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
  Require(artifacts.ast.family == StatementFamily::kSecurity,
          Message(row, "ast", "security family mismatch"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          Message(row, "lowering", "operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          Message(row, "lowering", "opcode mismatch"));
  Require(artifacts.envelope.operation_family == "sblr.security.mutation.v3",
          Message(row, "lowering", "operation family mismatch"));
  Require(artifacts.envelope.result_shape_key == "rs.security.protected_material.v1",
          Message(row, "lowering", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == row.engine_api_function,
          Message(row, "lowering", "engine API function mismatch"));
  Require(artifacts.envelope.resource_contract_key ==
              "resource.contract.protected_material",
          Message(row, "lowering", "protected material resource contract mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.protected_material_api_required"),
          Message(row, "lowering", "protected material authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_security_authorization"),
          Message(row, "lowering", "parser no-security-authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          Message(row, "lowering", "parser no-storage/finality authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          Message(row, "lowering", "parser no-SQL-execution authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          Message(row, "lowering", "cluster provider exclusion missing"));
  Require(HasValue(artifacts.envelope.required_rights, row.required_right),
          Message(row, "lowering", "required right missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.security.protected_material_cache") ||
              HasValue(artifacts.envelope.descriptor_refs, "sys.security.protected_material_catalog") ||
              HasValue(artifacts.envelope.descriptor_refs, "sys.security.encryption_profile") ||
              HasValue(artifacts.envelope.descriptor_refs, "sys.storage.filespace"),
          Message(row, "lowering", "protected material descriptor ref missing"));
  Require(HasValue(artifacts.envelope.policy_refs, "protected_material_control_policy") ||
              HasValue(artifacts.envelope.policy_refs, "protected_material_release_policy"),
          Message(row, "lowering", "protected material policy missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          Message(row, "payload", "exact-command evidence missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"resource_contract\":\"resource.contract.protected_material\""),
          Message(row, "payload", "resource contract missing"));
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
  Require(admission.operation_family == "sblr.security.mutation.v3",
          Message(row, "server_admission", "public family mismatch"));
}

api::EngineRequestContext EngineContext(const EncryptionRouteRow& row) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-encryption-maintenance-exact-route";
  context.security_context_present = true;
  context.database_path = std::string(kDatabasePath);
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.session_uuid.canonical = "019f5100-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f5100-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f5100-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f5100-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f5100-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f5100-0000-7000-8000-000000002007";
  context.catalog_generation_id = 511;
  context.security_epoch = 512;
  context.resource_epoch = 513;
  context.local_transaction_id = row.mutation ? 901 : 0;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("engine.shutdown");
  context.trace_tags.push_back("right:KEY_RELEASE_APPROVE");
  context.trace_tags.push_back("right:PROTECTED_MATERIAL_RELEASE");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const EncryptionRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.encryption_maintenance.") +
                                             std::string(row.operation_id));
  envelope.result_shape = "rs.security.protected_material.v1";
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.mutation;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineProtectedMaterialPolicySet MaterialPolicy() {
  api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = "019f5000-0000-7000-8000-000000000101";
  policy.access_policy_uuid = "019f5000-0000-7000-8000-000000000102";
  policy.release_policy_uuid = "019f5000-0000-7000-8000-000000000103";
  policy.purge_policy_uuid = "019f5000-0000-7000-8000-000000000104";
  policy.audit_policy_uuid = "019f5000-0000-7000-8000-000000000105";
  policy.release_purposes = {"filespace.open"};
  return policy;
}

void SeedKeyCache(const EncryptionRouteRow& row) {
  api::EngineAdmitEncryptionKeyRequest request;
  request.context = EngineContext(row);
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.target_database.object_kind = "database";
  request.key_uuid = std::string(kKeyUuid);
  request.key_label = "test-key-redacted";
  request.filespace_uuid = std::string(kFilespaceUuid);
  request.secret_evidence = "wrapped-reference:v1:seed";
  const auto result = api::EngineAdmitEncryptionKey(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.ok, Message(row, "seed", "key cache seed failed"));
}

void SeedProtectedMaterialVersion(const EncryptionRouteRow& row) {
  api::EngineCreateProtectedMaterialRequest request;
  request.context = EngineContext(row);
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.target_database.object_kind = "database";
  request.protected_material_uuid = std::string(kProtectedMaterialUuid);
  request.object_class = "filespace_encryption_key";
  request.owner_scope_uuid = std::string(kFilespaceUuid);
  request.purpose_class = "encryption_use";
  request.storage_class = "wrapped";
  request.policy = MaterialPolicy();
  request.initial_version_uuid = std::string(kProtectedMaterialVersionUuid);
  request.protected_reference = "kms-ref:v1:wrapped-material-route";
  request.envelope_reference = "kms-envelope:v1:wrapped-material-route";
  request.payload_hash =
      "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  const auto result = api::EngineCreateProtectedMaterial(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.ok && HasEvidence(result, "protected_material_create"),
          Message(row, "seed", "protected material seed failed"));
}

void PrepareState(const EncryptionRouteRow& row) {
  if (row.seed_key_cache) SeedKeyCache(row);
  if (row.seed_protected_material_version) SeedProtectedMaterialVersion(row);
}

void RequireRegistryAndDispatch(const EncryptionRouteRow& row) {
  PrepareState(row);

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
  Require(HasEvidence(dispatch.api_result, row.expected_evidence_kind),
          Message(row, "engine_dispatch", "expected evidence missing"));
  Require(!ResultContains(dispatch.api_result, "wrapped-reference"),
          Message(row, "engine_dispatch", "wrapped key evidence leaked"));
  Require(!ResultContains(dispatch.api_result, "raw_key"),
          Message(row, "engine_dispatch", "raw key marker leaked"));
  Require(!ResultContains(dispatch.api_result, "plaintext:"),
          Message(row, "engine_dispatch", "plaintext marker leaked"));
  if (row.operation_id == "security.encryption_key.admit" ||
      row.operation_id == "security.encrypted_filespace.open" ||
      row.operation_id == "security.request_protected_material") {
    Require(HasRowField(dispatch.api_result, "plaintext_material_returned", "false"),
            Message(row, "engine_dispatch", "plaintext return flag missing"));
  }
  if (row.operation_id == "security.encryption_key.rotate") {
    Require(HasRowField(dispatch.api_result, "plaintext_material_persisted", "false"),
            Message(row, "engine_dispatch", "plaintext persisted flag missing"));
  }
  if (row.operation_id == "security.protected_material.version.purge") {
    Require(HasEvidence(dispatch.api_result, "protected_material_audit_event"),
            Message(row, "engine_dispatch", "purge audit evidence missing"));
    Require(HasRowField(dispatch.api_result, "action", "purge"),
            Message(row, "engine_dispatch", "purge row action missing"));
  }
}

void CleanTempFiles() {
  std::filesystem::remove(std::filesystem::path(kDatabasePath));
  std::filesystem::remove(std::filesystem::path(std::string(kDatabasePath) +
                                                ".sb.api_events"));
  std::filesystem::remove(std::filesystem::path(std::string(kDatabasePath) +
                                                ".sb.protected_material_catalog"));
}

}  // namespace

int main() {
  CleanTempFiles();
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireRegistryAndDispatch(row);
  }
  CleanTempFiles();
  std::cout << "sbsql_encryption_maintenance_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
