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

constexpr std::string_view kFilespaceUuid =
    "019f5000-0000-7000-8000-000000000002";
constexpr std::string_view kProtectedMaterialUuid =
    "019f5000-0000-7000-8000-000000000005";
constexpr std::string_view kProtectedMaterialVersionUuid =
    "019f5000-0000-7000-8000-000000000006";
constexpr std::string_view kDatabasePathPrefix =
    "/tmp/sbsql_protected_material_exact_route_";

struct ProtectedMaterialRouteRow {
  std::string_view sql;
  std::string_view database_uuid;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view engine_api_function;
  std::string_view required_right;
  std::string_view expected_evidence_kind;
  std::string_view expected_action;
  bool mutation;
  bool seed_material;
  bool expect_audit_row;
};

constexpr std::array<ProtectedMaterialRouteRow, 10> kRows{{
    {"CREATE PROTECTED MATERIAL", "019f5200-0000-7000-8000-000000000011", "security.protected_material.create", "SBLR_SECURITY_PROTECTED_MATERIAL_CREATE", "EngineCreateProtectedMaterial", "right.key_release_approve", "protected_material_create", "create", true, false, true},
    {"ADD PROTECTED MATERIAL VERSION", "019f5200-0000-7000-8000-000000000012", "security.protected_material.version.add", "SBLR_SECURITY_PROTECTED_MATERIAL_VERSION_ADD", "EngineAddProtectedMaterialVersion", "right.key_release_approve", "protected_material_version_add", "add_version", true, true, true},
    {"ROTATE PROTECTED MATERIAL", "019f5200-0000-7000-8000-000000000013", "security.protected_material.version.add", "SBLR_SECURITY_PROTECTED_MATERIAL_VERSION_ADD", "EngineAddProtectedMaterialVersion", "right.key_release_approve", "protected_material_version_add", "add_version", true, true, true},
    {"RESOLVE PROTECTED MATERIAL", "019f5200-0000-7000-8000-000000000014", "security.protected_material.resolve", "SBLR_SECURITY_PROTECTED_MATERIAL_RESOLVE", "EngineResolveProtectedMaterial", "right.protected_material_release", "protected_material_resolve", "resolve", false, true, true},
    {"RELEASE PROTECTED MATERIAL", "019f5200-0000-7000-8000-000000000015", "security.protected_material.release", "SBLR_SECURITY_PROTECTED_MATERIAL_RELEASE", "EngineReleaseProtectedMaterial", "right.protected_material_release", "protected_material_release", "release", false, true, true},
    {"PURGE PROTECTED MATERIAL VERSION", "019f5200-0000-7000-8000-000000000016", "security.protected_material.version.purge", "SBLR_SECURITY_PROTECTED_MATERIAL_VERSION_PURGE", "EnginePurgeProtectedMaterialVersion", "right.key_release_approve", "protected_material_purge", "purge", true, true, true},
    {"SHOW PROTECTED MATERIAL CATALOG", "019f5200-0000-7000-8000-000000000017", "security.protected_material.catalog.inspect", "SBLR_SECURITY_PROTECTED_MATERIAL_CATALOG_INSPECT", "EngineInspectProtectedMaterialCatalog", "right.protected_material_release", "protected_material_catalog_inspect", "inspect", false, true, true},
    {"SHOW PROTECTED MATERIAL AUDIT", "019f5200-0000-7000-8000-000000000018", "security.protected_material.catalog.inspect", "SBLR_SECURITY_PROTECTED_MATERIAL_CATALOG_INSPECT", "EngineInspectProtectedMaterialCatalog", "right.protected_material_release", "protected_material_catalog_inspect", "inspect", false, true, true},
    {"EXPORT PROTECTED MATERIAL PACKAGE", "019f5200-0000-7000-8000-000000000019", "security.protected_material.package.export", "SBLR_SECURITY_PROTECTED_MATERIAL_PACKAGE_EXPORT", "EngineExportProtectedMaterialPackage", "right.protected_material_release", "protected_material_package_export", "export", false, true, false},
    {"IMPORT PROTECTED MATERIAL PACKAGE", "019f5200-0000-7000-8000-000000000020", "security.protected_material.package.import", "SBLR_SECURITY_PROTECTED_MATERIAL_PACKAGE_IMPORT", "EngineImportProtectedMaterialPackage", "right.key_release_approve", "protected_material_package_import", "import", true, false, false},
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
    if (Contains(evidence.evidence_kind, text) ||
        Contains(evidence.evidence_id, text)) {
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

std::string Message(const ProtectedMaterialRouteRow& row,
                    std::string_view phase,
                    std::string_view text) {
  return std::string(row.sql) + " " + std::string(phase) + ": " +
         std::string(text);
}

std::string DatabasePath(const ProtectedMaterialRouteRow& row) {
  return std::string(kDatabasePathPrefix) + std::string(row.database_uuid) + ".sbdb";
}

std::string PackageSourcePath(const ProtectedMaterialRouteRow& row) {
  return DatabasePath(row) + ".package_source";
}

SessionContext ParserSession(const ProtectedMaterialRouteRow& row) {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f5200-0000-7000-8000-000000001001";
  session.connection_uuid = "019f5200-0000-7000-8000-000000001002";
  session.database_uuid = std::string(row.database_uuid);
  session.catalog_epoch = 521;
  session.security_policy_epoch = 522;
  session.descriptor_epoch = 523;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f5200-0000-7000-8000-000000001004";
  config.bundle_contract_id = "sbp_sbsql@protected-material-exact-route";
  config.build_id = "sbsql-protected-material-exact-route";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const ProtectedMaterialRouteRow& row) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession(row);
  artifacts.cst = BuildCst(row.sql);
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

void RequireExactLowering(const ProtectedMaterialRouteRow& row) {
  const auto artifacts = RunPipeline(row);
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
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.security.protected_material_catalog") ||
              HasValue(artifacts.envelope.descriptor_refs,
                       "sys.security.protected_material_audit"),
          Message(row, "lowering", "protected material descriptor ref missing"));
  Require(HasValue(artifacts.envelope.policy_refs,
                   row.mutation ? "protected_material_control_policy"
                                : "protected_material_release_policy"),
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

api::EngineRequestContext EngineContext(const ProtectedMaterialRouteRow& row,
                                        bool mutation_context) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-protected-material-exact-route";
  context.security_context_present = true;
  context.database_path = DatabasePath(row);
  context.database_uuid.canonical = std::string(row.database_uuid);
  context.session_uuid.canonical = "019f5200-0000-7000-8000-000000002002";
  context.principal_uuid.canonical = "019f5200-0000-7000-8000-000000002003";
  context.node_uuid.canonical = "019f5200-0000-7000-8000-000000002004";
  context.statement_uuid.canonical = "019f5200-0000-7000-8000-000000002005";
  context.current_diagnostic_uuid.canonical = "019f5200-0000-7000-8000-000000002006";
  context.transaction_uuid.canonical = "019f5200-0000-7000-8000-000000002007";
  context.catalog_generation_id = 521;
  context.security_epoch = 522;
  context.resource_epoch = 523;
  context.local_transaction_id = mutation_context ? 901 : 0;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:KEY_RELEASE_APPROVE");
  context.trace_tags.push_back("right:PROTECTED_MATERIAL_RELEASE");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const ProtectedMaterialRouteRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.protected_material.") +
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
  policy.retention_policy_uuid = "019f5200-0000-7000-8000-000000000101";
  policy.access_policy_uuid = "019f5200-0000-7000-8000-000000000102";
  policy.release_policy_uuid = "019f5200-0000-7000-8000-000000000103";
  policy.purge_policy_uuid = "019f5200-0000-7000-8000-000000000104";
  policy.audit_policy_uuid = "019f5200-0000-7000-8000-000000000105";
  policy.release_purposes = {"filespace.open"};
  return policy;
}

void SeedProtectedMaterialVersion(const ProtectedMaterialRouteRow& row) {
  api::EngineCreateProtectedMaterialRequest request;
  request.context = EngineContext(row, true);
  request.target_database.uuid.canonical = std::string(row.database_uuid);
  request.target_database.object_kind = "database";
  request.protected_material_uuid = std::string(kProtectedMaterialUuid);
  request.object_class = "filespace_encryption_key";
  request.owner_scope_uuid = std::string(kFilespaceUuid);
  request.purpose_class = "encryption_use";
  request.storage_class = "wrapped";
  request.policy = MaterialPolicy();
  request.initial_version_uuid = std::string(kProtectedMaterialVersionUuid);
  request.protected_reference = "kms-ref:v1:protected-material-seed";
  request.envelope_reference = "kms-envelope:v1:protected-material-seed";
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

struct PackageFixture {
  std::string encoded_package;
  std::string package_digest;
};

PackageFixture BuildProtectedMaterialPackageFixture(const ProtectedMaterialRouteRow& row) {
  const std::string source_path = PackageSourcePath(row);
  std::filesystem::remove(std::filesystem::path(source_path));
  std::filesystem::remove(std::filesystem::path(source_path + ".sb.protected_material_catalog"));

  api::EngineRequestContext create_context = EngineContext(row, true);
  create_context.database_path = source_path;
  create_context.local_transaction_id = 903;
  api::EngineCreateProtectedMaterialRequest create;
  create.context = create_context;
  create.target_database.uuid.canonical = std::string(row.database_uuid);
  create.target_database.object_kind = "database";
  create.protected_material_uuid = std::string(kProtectedMaterialUuid);
  create.object_class = "filespace_encryption_key";
  create.owner_scope_uuid = std::string(kFilespaceUuid);
  create.purpose_class = "encryption_use";
  create.storage_class = "wrapped";
  create.policy = MaterialPolicy();
  create.initial_version_uuid = std::string(kProtectedMaterialVersionUuid);
  create.protected_reference = "kms-ref:v1:protected-material-package-seed";
  create.envelope_reference = "kms-envelope:v1:protected-material-package-seed";
  create.payload_hash =
      "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  const auto created = api::EngineCreateProtectedMaterial(create);
  Require(created.ok && HasEvidence(created, "protected_material_create"),
          Message(row, "package_seed", "protected material package seed failed"));

  api::EngineRequestContext export_context = EngineContext(row, false);
  export_context.database_path = source_path;
  api::EngineExportProtectedMaterialPackageRequest export_package;
  export_package.context = export_context;
  export_package.target_database.uuid.canonical = std::string(row.database_uuid);
  export_package.target_database.object_kind = "database";
  export_package.protected_material_uuid = std::string(kProtectedMaterialUuid);
  export_package.include_versions = true;
  export_package.include_audit = true;
  const auto exported = api::EngineExportProtectedMaterialPackage(export_package);
  Require(exported.ok && exported.exported && !exported.encoded_package.empty() &&
              !exported.package_digest.empty(),
          Message(row, "package_seed", "protected material package export failed"));

  std::filesystem::remove(std::filesystem::path(source_path));
  std::filesystem::remove(std::filesystem::path(source_path + ".sb.protected_material_catalog"));
  return PackageFixture{exported.encoded_package, exported.package_digest};
}

void PrepareState(const ProtectedMaterialRouteRow& row) {
  if (row.seed_material) SeedProtectedMaterialVersion(row);
}

void RequireRegistryAndDispatch(const ProtectedMaterialRouteRow& row) {
  PrepareState(row);

  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, Message(row, "sblr_registry", "operation missing"));
  Require(entry->opcode == row.opcode, Message(row, "sblr_registry", "opcode mismatch"));
  Require(!entry->requires_cluster_authority,
          Message(row, "sblr_registry", "unexpected cluster authority"));

  api::EngineApiRequest api_request;
  if (row.operation_id == "security.protected_material.package.import") {
    const auto package = BuildProtectedMaterialPackageFixture(row);
    api_request.option_envelopes.push_back("encoded_package:" + package.encoded_package);
    api_request.option_envelopes.push_back("expected_package_digest:" + package.package_digest);
    api_request.option_envelopes.push_back(
        "protected_material_package_import_authorized:true");
  }
  const auto dispatch = sblr::DispatchSblrOperation(
      {EngineContext(row, row.mutation), EngineEnvelope(row), api_request});
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
  Require(HasRowField(dispatch.api_result, "action", row.expected_action),
          Message(row, "engine_dispatch", "expected result action missing"));
  Require(HasRowField(dispatch.api_result,
                      "protected_material",
                      "<protected-material-redacted>") ||
              row.operation_id == "security.protected_material.catalog.inspect",
          Message(row, "engine_dispatch", "protected material redaction row missing"));
  if (row.expect_audit_row) {
    Require(ResultContains(dispatch.api_result, "protected-material-audit:v1"),
            Message(row, "engine_dispatch", "audit event evidence missing"));
  }
  Require(!ResultContains(dispatch.api_result, "kms-ref"),
          Message(row, "engine_dispatch", "protected reference leaked"));
  Require(!ResultContains(dispatch.api_result, "kms-envelope"),
          Message(row, "engine_dispatch", "envelope reference leaked"));
  Require(!ResultContains(dispatch.api_result, "raw_key"),
          Message(row, "engine_dispatch", "raw key marker leaked"));
  Require(!ResultContains(dispatch.api_result, "plaintext:"),
          Message(row, "engine_dispatch", "plaintext marker leaked"));
}

void CleanTempFiles() {
  for (const auto& row : kRows) {
    const auto path = DatabasePath(row);
    std::filesystem::remove(std::filesystem::path(path));
    std::filesystem::remove(std::filesystem::path(path + ".sb.api_events"));
    std::filesystem::remove(std::filesystem::path(path + ".sb.protected_material_catalog"));
    const auto source_path = PackageSourcePath(row);
    std::filesystem::remove(std::filesystem::path(source_path));
    std::filesystem::remove(std::filesystem::path(source_path + ".sb.protected_material_catalog"));
  }
}

}  // namespace

int main() {
  CleanTempFiles();
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireRegistryAndDispatch(row);
  }
  CleanTempFiles();
  std::cout << "sbsql_protected_material_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
