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
#include "extensibility/udr_api.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sb_udr_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace udr_runtime = scratchbird::udr::runtime;

constexpr std::string_view kPackageUuid = "019f0000-0000-7000-8000-000000003901";
constexpr std::string_view kPackageName = "sbup_demo";
constexpr std::string_view kDatabasePath =
    "/tmp/sbsql_udr_package_management_exact_route_conformance.sbdb";

struct UdrRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view expected_field;
  std::string_view expected_value;
  bool contains{false};
};

constexpr std::array<UdrRowEvidence, 6> kUdrRows{{
    {"SBSQL-F0CF86A4B3AF",
     "udr_package_stmt",
     "SBSQL-SURFACE-9084358EE3A1",
     "object_kind",
     "udr_package",
     false},
    {"SBSQL-A3D801F6079D",
     "udr_package_name",
     "SBSQL-SURFACE-09C11F5E5BD4",
     "name",
     "sbup_demo",
     false},
    {"SBSQL-C3138AC0D3EA",
     "udr_name",
     "SBSQL-SURFACE-6D67C6B15DBF",
     "name",
     "sbup_demo",
     false},
    {"SBSQL-202B0DD6C682",
     "udr_binary_ref",
     "SBSQL-SURFACE-365AB3C05772",
     "binary_hash",
     "sha256:sbup-demo",
     false},
    {"SBSQL-2FA96214E399",
     "udr_capability",
     "SBSQL-SURFACE-5FC597942BFA",
     "capability_role",
     "parser_support",
     false},
    {"SBSQL-7152B9A9B751",
     "udr_entry_point",
     "SBSQL-SURFACE-AA7130CCC0F5",
     "entrypoints",
     "sb_udr_demo_entry",
     true},
}};

std::string EvidenceMessage(const UdrRowEvidence& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view key) {
  for (const auto& [field_name, field_value] : row.fields) {
    if (field_name == key) return field_value.encoded_value;
  }
  return {};
}

bool ApiResultHasField(const api::EngineApiResult& result,
                       std::string_view name,
                       std::string_view value,
                       bool contains) {
  for (const auto& row : result.result_shape.rows) {
    const std::string field = FieldValue(row, name);
    if (contains ? Contains(field, value) : field == value) return true;
  }
  return false;
}

bool ApiResultHasEvidence(const api::EngineApiResult& result,
                          std::string_view kind,
                          std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000003701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000003702";
  session.database_uuid = "019f0000-0000-7000-8000-000000003703";
  session.catalog_epoch = 43;
  session.security_policy_epoch = 47;
  session.descriptor_epoch = 53;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000003704";
  config.bundle_contract_id = "sbp_sbsql@udr-package-management-route-test";
  config.build_id = "sbsql-udr-package-management-route-test";
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

void RequireRegistryEvidence(const UdrRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "runtime_management",
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "registry", "canonical SBLR operation family mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireExactLowering(const UdrRowEvidence& row) {
  const auto artifacts = RunPipeline("SHOW UDR PACKAGES");
  Require(artifacts.bound.bound,
          EvidenceMessage(row, "parser_bind_lower", "UDR package statement did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "parser_bind_lower", "UDR inspect route required name registry resolution"));
  Require(!artifacts.bound.requires_transaction_authority,
          EvidenceMessage(row, "parser_bind_lower", "parser owned UDR transaction authority"));
  Require(artifacts.bound.statement_parser_category == "runtime_management",
          EvidenceMessage(row, "parser_bind_lower", "UDR route did not bind as runtime management"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "UDR SBLR verifier rejected exact route"));
  Require(artifacts.envelope.operation_family == "sblr.udr.operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.udr.operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "parser_bind_lower", "engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES",
          EvidenceMessage(row, "parser_bind_lower", "opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.udr_inspect_api_required"),
          EvidenceMessage(row, "parser_bind_lower", "UDR inspect authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          EvidenceMessage(row, "parser_bind_lower", "UDR transaction context handoff missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_udr_execution"),
          EvidenceMessage(row, "parser_bind_lower", "parser no-UDR-execution step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "parser no-SQL-execution authority step missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.udr_package_registry"),
          EvidenceMessage(row, "parser_bind_lower", "UDR package registry descriptor missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.udr_runtime_descriptor"),
          EvidenceMessage(row, "parser_bind_lower", "UDR runtime descriptor missing"));
  Require(HasValue(artifacts.envelope.required_rights, "right.udr_inspect"),
          EvidenceMessage(row, "parser_bind_lower", "required right mismatch"));
  Require(!artifacts.envelope.parser_executes_sql,
          EvidenceMessage(row, "no_sql_engine_execution",
                          "UDR lowering allowed parser SQL execution"));
  Require(!artifacts.envelope.real_file_effects,
          EvidenceMessage(row, "no_donor_execution",
                          "UDR lowering allowed parser file effects"));
  Require(!Contains(artifacts.envelope.payload, "SHOW UDR PACKAGES"),
          EvidenceMessage(row, "no_sql_text_authority", "UDR envelope embedded source SQL"));
  Require(Contains(artifacts.envelope.payload,
                   "\"udr_envelope_kind\":\"udr_package_inspect\""),
          EvidenceMessage(row, "parser_bind_lower", "UDR envelope kind missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"runtime_component\":\"udr_packages\""),
          EvidenceMessage(row, "parser_bind_lower", "UDR runtime component missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"parser_executes_udr\":false"),
          EvidenceMessage(row, "parser_bind_lower", "parser UDR execution denial missing"));
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          EvidenceMessage(row, "parser_bind_lower", "row surface id missing from UDR payload"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission", "server admission rejected UDR route"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission",
                          "server admission did not require engine public ABI dispatch"));
  Require(admission.operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "server_admission", "server admission operation id mismatch"));
  Require(admission.operation_family == "sblr.udr.operation.v3",
          EvidenceMessage(row, "server_admission", "server admission family mismatch"));
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-udr-package-management-exact-route";
  context.security_context_present = true;
  context.trace_tags.push_back("right:UDR_MANAGE");
  context.trace_tags.push_back("right:UDR_INSPECT");
  context.database_path = std::string(kDatabasePath);
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000003801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000003802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000003803";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000003804";
  context.local_transaction_id = 77;
  context.catalog_generation_id = 43;
  context.security_epoch = 47;
  context.resource_epoch = 53;
  return context;
}

api::EngineLocalizedName LocalizedName(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.path = "sys.udr";
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

udr_runtime::UdrCallResult DemoEntrypoint(const udr_runtime::UdrCallInput&) {
  udr_runtime::UdrCallResult result;
  result.ok = true;
  result.payload = "{\"ok\":true}";
  result.message_vector_json = "{\"diagnostics\":[]}";
  return result;
}

udr_runtime::UdrPackageDescriptor DemoDescriptor() {
  udr_runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = std::string(kPackageUuid);
  descriptor.package_name = std::string(kPackageName);
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "src-rev-sbup-demo";
  descriptor.binary_hash = "sha256:sbup-demo";
  descriptor.signature_policy = "trusted-test-signature";
  descriptor.capability_role = "parser_support";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints.push_back({"sb_udr_demo_entry", "parser_support", DemoEntrypoint});
  return descriptor;
}

void AddManageUdrOptions(api::EngineApiRequest* request,
                         const udr_runtime::UdrPackageDescriptor& descriptor) {
  request->option_envelopes.push_back("permission:manage_udr");
  request->option_envelopes.push_back("trusted_cpp_udr");
  request->option_envelopes.push_back("abi:sb_udr_v1");
  request->option_envelopes.push_back("name:" + descriptor.package_name);
  request->option_envelopes.push_back("linked_udr_package:true");
  request->option_envelopes.push_back("source_revision:" + descriptor.source_revision);
  request->option_envelopes.push_back("binary_hash:" + descriptor.binary_hash);
  request->option_envelopes.push_back("signature_policy:" + descriptor.signature_policy);
  request->option_envelopes.push_back("capability_role:" + descriptor.capability_role);
}

void SeedActiveTransaction() {
  std::ofstream out(std::string(kDatabasePath) + ".sb.crud_events",
                    std::ios::binary | std::ios::app);
  out << "SBCRUD1\tTX_BEGIN\t77\tsbsql_udr_package_management_exact_route\n";
  Require(static_cast<bool>(out), "failed to seed MGA transaction evidence for UDR route test");
}

void RegisterDemoPackage() {
  const auto descriptor = DemoDescriptor();
  const auto runtime_registered = udr_runtime::RegisterPackage(descriptor);
  Require(runtime_registered.ok, "failed to seed UDR runtime package descriptor");
  SeedActiveTransaction();

  api::EngineRegisterUdrPackageRequest request;
  request.context = EngineContext();
  request.target_database.uuid.canonical = request.context.database_uuid.canonical;
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical = descriptor.package_uuid;
  request.target_object.object_kind = "udr_package";
  request.localized_names.push_back(LocalizedName(descriptor.package_name));
  AddManageUdrOptions(&request, descriptor);
  const auto registered = api::EngineRegisterUdrPackage(request);
  for (const auto& diagnostic : registered.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(registered.ok, "engine refused trusted C++ UDR package registration");
  Require(ApiResultHasEvidence(registered, "udr_descriptor", "runtime_descriptor_validated"),
          "UDR registration did not validate the runtime descriptor");
  Require(ApiResultHasEvidence(registered, "authority_boundary",
                               "mga_sblr_uuid_security_transaction_preserved"),
          "UDR registration did not preserve MGA/SBLR/UUID/security authority evidence");
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "extensibility.inspect_udr_packages",
      "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES",
      "trace.udr_package_management.exact_route");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch(const UdrRowEvidence& row) {
  api::EngineApiRequest api_request;
  api_request.option_envelopes.push_back("permission:inspect_udr");
  const sblr::SblrDispatchRequest request{EngineContext(), EngineEnvelope(), api_request};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR envelope did not validate"));
  Require(result.accepted,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR dispatch did not accept operation"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "engine_dispatch", "engine SBLR dispatch did not route to API"));
  Require(result.api_result.ok,
          EvidenceMessage(row, "engine_dispatch", "engine API returned failure"));
  Require(result.api_result.operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "engine_dispatch", "engine API operation id mismatch"));
  Require(ApiResultHasEvidence(result.api_result, "extension_behavior", "inspected"),
          EvidenceMessage(row, "engine_dispatch", "UDR inspect evidence missing"));
  Require(ApiResultHasEvidence(result.api_result, "authority_boundary",
                               "mga_sblr_uuid_security_transaction_preserved"),
          EvidenceMessage(row, "engine_dispatch", "authority-boundary evidence missing"));
  Require(ApiResultHasField(result.api_result,
                            row.expected_field,
                            row.expected_value,
                            row.contains),
          EvidenceMessage(row, "engine_dispatch", "expected UDR result field missing"));
}

}  // namespace

int main() {
  std::remove(std::string(kDatabasePath).c_str());
  std::remove((std::string(kDatabasePath) + ".sb.api_events").c_str());
  std::remove((std::string(kDatabasePath) + ".sb.crud_events").c_str());
  udr_runtime::ResetRuntimeForTest();
  RegisterDemoPackage();
  for (const auto& row : kUdrRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
    RequireEngineDispatch(row);
  }
  std::cout << "sbsql_udr_package_management_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
