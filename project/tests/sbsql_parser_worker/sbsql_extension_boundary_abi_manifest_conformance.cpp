// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_provider/cluster_provider.hpp"
#include "extensibility/extension_boundary_manifest.hpp"
#include "extensibility/parser_package_api.hpp"
#include "extensibility/udr_api.hpp"
#include "sblr_dispatch.hpp"
#include "sbu_sbsql_parser_support.hpp"
#include "sb_udr_runtime.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace sblr = scratchbird::engine::sblr;
namespace sbsql_udr = scratchbird::udr::sbsql_parser_support;
namespace udr_runtime = scratchbird::udr::runtime;

constexpr std::string_view kDatabaseUuid = "019f6c00-0000-7000-8000-000000000020";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_cbq020_extension_boundary.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for extension boundary ABI test");
  return std::filesystem::path(made);
}

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  std::uint64_t tx = 77) {
  api::EngineRequestContext context;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.principal_uuid.canonical = "019f6c00-0000-7000-8000-000000000201";
  context.session_uuid.canonical = "019f6c00-0000-7000-8000-000000000202";
  context.transaction_uuid.canonical = "019f6c00-0000-7000-8000-000000000203";
  context.local_transaction_id = tx;
  context.security_context_present = true;
  context.catalog_generation_id = 8;
  context.security_epoch = 8;
  context.resource_epoch = 8;
  return context;
}

api::EngineLocalizedName LocalizedName(std::string name,
                                       std::string path = "sys.extension") {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.path = std::move(path);
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDispatchDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field_name) {
  if (result.result_shape.rows.empty()) return {};
  for (const auto& field : result.result_shape.rows.front().fields) {
    if (field.first == field_name) return field.second.encoded_value;
  }
  return {};
}

void SeedActiveTransaction(const std::filesystem::path& database_path,
                           std::uint64_t tx) {
  std::ofstream out(database_path.string() + ".sb.crud_events",
                    std::ios::binary | std::ios::app);
  out << "SBCRUD1\tTX_BEGIN\t" << tx << "\textension_boundary_abi_manifest\n";
  Require(static_cast<bool>(out),
          "failed to seed MGA transaction evidence for extension boundary test");
}

void AddManageUdrOptions(api::EngineApiRequest* request,
                         const udr_runtime::UdrPackageDescriptor& descriptor) {
  request->option_envelopes.push_back("permission:manage_udr");
  request->option_envelopes.push_back("trusted_cpp_udr");
  request->option_envelopes.push_back("abi:sb_udr_v1");
  request->option_envelopes.push_back("name:" + descriptor.package_name);
  request->option_envelopes.push_back("udr_kind:parser_support");
  request->option_envelopes.push_back("linked_udr_package:true");
  request->option_envelopes.push_back("source_revision:" + descriptor.source_revision);
  request->option_envelopes.push_back("binary_hash:" + descriptor.binary_hash);
  request->option_envelopes.push_back("signature_policy:" + descriptor.signature_policy);
  request->option_envelopes.push_back("capability_role:" + descriptor.capability_role);
}

template <typename TRequest>
TRequest UdrRequest(const std::filesystem::path& database_path,
                    const udr_runtime::UdrPackageDescriptor& descriptor) {
  TRequest request;
  request.context = Context(database_path);
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical = descriptor.package_uuid;
  request.target_object.object_kind = "udr_package";
  request.localized_names.push_back(LocalizedName(descriptor.package_name, "sys.udr"));
  return request;
}

void AddInvokeUdrOptions(api::EngineApiRequest* request) {
  request->option_envelopes.push_back("permission:invoke_udr");
  request->option_envelopes.push_back("sblr_authorized_invocation:true");
  request->option_envelopes.push_back("operation_family:sblr.udr.operation.v3");
  request->option_envelopes.push_back("entrypoint:sbu_sbsql_parse_to_sblr");
  request->option_envelopes.push_back("payload:select 1");
  request->option_envelopes.push_back(
      "context_packet:engine_context=trusted;resolver=public;authenticated=true");
  request->option_envelopes.push_back("memory_budget_bytes:4096");
  request->option_envelopes.push_back("cpu_budget_microseconds:1000");
}

void TestManifestRows() {
  const auto manifest = api::BuiltinExtensionBoundaryManifest();
  Require(manifest.size() == 7, "extension boundary manifest row count drifted");
  Require(api::ExtensionBoundaryManifestHasRequiredCoreRows(),
          "extension boundary manifest is missing required rows");

  for (const auto& entry : manifest) {
    Require(!entry.boundary_id.empty(), "manifest boundary_id is empty");
    Require(!entry.boundary_type.empty(), "manifest boundary_type is empty");
    Require(!entry.contract_version.empty(), "manifest contract_version is empty");
    Require(!entry.abi_surface.empty(), "manifest abi_surface is empty");
    Require(!entry.support_level.empty(), "manifest support_level is empty");
    Require(!entry.core_classification.empty(), "manifest core classification is empty");
    Require(!entry.route_surface.empty(), "manifest route surface is empty");
    Require(!entry.execution_authority.empty(), "manifest execution authority is empty");
    Require(entry.execution_authority == "engine_sblr_internal_api_only",
            "manifest widened extension execution authority");
    Require(!entry.sql_text_authoritative,
            "manifest marked SQL text as extension runtime authority");
    Require(entry.requires_uuid_or_descriptor_authority,
            "manifest omitted UUID/descriptor authority requirement");
    if (entry.cluster_positive_requires_provider) {
      Require(entry.non_cluster_refusal_code != "none",
              "cluster boundary missing non-cluster refusal code");
      Require(entry.positive_cluster_boundary.find("provider_required") !=
                  std::string_view::npos,
              "cluster boundary missing provider-required marker");
    }
  }

  const auto* parser = api::FindExtensionBoundaryManifestEntry(
      "parser_package.sbsql_v3");
  Require(parser != nullptr && parser->parser_translation_boundary,
          "SBsql parser package manifest row is not a parser translation boundary");
  Require(parser->contract_version == "sb_parser_package_v3",
          "SBsql parser package ABI version drifted");

  const auto* udr = api::FindExtensionBoundaryManifestEntry(
      "udr_package.trusted_cpp_v1");
  Require(udr != nullptr && udr->contract_version == "sb_udr_v1",
          "UDR ABI manifest row drifted");

  const auto* provider = api::FindExtensionBoundaryManifestEntry(
      "cluster_provider.v1");
  Require(provider != nullptr &&
              provider->non_cluster_refusal_code ==
                  cluster_provider::kClusterSupportNotEnabledCode,
          "cluster provider manifest refusal code does not match live API");
}

void TestParserPackageBoundary(const std::filesystem::path& database_path) {
  api::EngineRegisterParserPackageRequest request;
  request.context = Context(database_path);
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical =
      "019f6c00-0000-7000-8000-000000000301";
  request.target_object.object_kind = "parser_package";
  request.localized_names.push_back(LocalizedName("sbp_extension_boundary_fixture",
                                                  "sys.parser"));
  request.option_envelopes.push_back("name:sbp_extension_boundary_fixture");
  request.option_envelopes.push_back("contract:sb_parser_package_v3");

  const auto registered = api::EngineRegisterParserPackage(request);
  Require(registered.ok, "parser package boundary registration was refused");
  Require(HasEvidence(registered, "extension_family", "parser_package"),
          "parser package registration omitted extension family evidence");
  Require(HasEvidence(registered, "extension_behavior",
                      "untrusted_translation_package_registration"),
          "parser package registration omitted translation package evidence");
  Require(HasEvidence(registered, "parser_trust_boundary",
                      "untrusted_per_connection"),
          "parser package registration did not publish parser trust boundary");
  Require(HasEvidence(registered, "engine_mutation_authority", "false"),
          "parser package registration claimed engine mutation authority");

  auto cluster_request = request;
  cluster_request.target_object.uuid.canonical =
      "019f6c00-0000-7000-8000-000000000302";
  cluster_request.option_envelopes.push_back("cluster_deploy:true");
  const auto cluster_result = api::EngineRegisterParserPackage(cluster_request);
  Require(!cluster_result.ok && cluster_result.cluster_authority_required,
          "cluster parser package route did not fail closed without authority");
  Require(HasDiagnostic(cluster_result,
                        "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
          "cluster parser package route emitted the wrong diagnostic");
}

void TestTrustedParserSupportUdrBoundary(const std::filesystem::path& database_path) {
  udr_runtime::ResetRuntimeForTest();
  const auto descriptor = sbsql_udr::sbu_sbsql_package_descriptor();
  const auto runtime_registered = udr_runtime::RegisterPackage(descriptor);
  Require(runtime_registered.ok, "failed to seed SBsql parser-support UDR descriptor");

  auto register_request =
      UdrRequest<api::EngineRegisterUdrPackageRequest>(database_path, descriptor);
  AddManageUdrOptions(&register_request, descriptor);
  const auto registered = api::EngineRegisterUdrPackage(register_request);
  Require(registered.ok, "trusted parser-support UDR registration was refused");
  Require(HasEvidence(registered, "udr_descriptor", "runtime_descriptor_validated"),
          "trusted parser-support UDR did not validate runtime descriptor");
  Require(HasEvidence(registered, "udr_provenance",
                      "source_hash_signature_capability_checked"),
          "trusted parser-support UDR did not verify provenance evidence");
  Require(HasEvidence(registered, "execution_boundary", "engine_owned_no_bypass"),
          "trusted parser-support UDR did not publish engine boundary evidence");
  Require(HasEvidence(registered, "authority_boundary",
                      "mga_sblr_uuid_security_transaction_preserved"),
          "trusted parser-support UDR did not preserve authority-boundary evidence");

  auto load_request =
      UdrRequest<api::EngineLoadUdrPackageRequest>(database_path, descriptor);
  AddManageUdrOptions(&load_request, descriptor);
  const auto loaded = api::EngineLoadUdrPackage(load_request);
  Require(loaded.ok, "trusted parser-support UDR load was refused");
  Require(HasEvidence(loaded, "udr_entrypoints", "dispatch_table_published"),
          "trusted parser-support UDR did not publish dispatch table evidence");

  auto invoke_request =
      UdrRequest<api::EngineInvokeUdrPackageRequest>(database_path, descriptor);
  AddInvokeUdrOptions(&invoke_request);
  const auto invoked = api::EngineInvokeUdrPackage(invoke_request);
  Require(invoked.ok, "trusted parser-support UDR SBLR invocation was refused");
  Require(HasEvidence(invoked, "sblr_authority", "SBLR_UDR_INVOKE"),
          "trusted parser-support UDR invocation did not require SBLR authority");
  Require(HasEvidence(invoked, "udr_dispatch", "entrypoint_callback_invoked"),
          "trusted parser-support UDR did not dispatch through the runtime table");
  Require(FieldValue(invoked, "result_payload").find("SBLRExecutionEnvelope.v3") !=
              std::string::npos,
          "trusted parser-support UDR did not return parser-generated SBLR");
  Require(FieldValue(invoked, "result_payload").find("sql_text") ==
              std::string::npos,
          "trusted parser-support UDR exposed SQL text as engine authority");

  auto no_sblr_authority =
      UdrRequest<api::EngineInvokeUdrPackageRequest>(database_path, descriptor);
  no_sblr_authority.option_envelopes.push_back("permission:invoke_udr");
  no_sblr_authority.option_envelopes.push_back("entrypoint:sbu_sbsql_parse_to_sblr");
  const auto no_sblr_result = api::EngineInvokeUdrPackage(no_sblr_authority);
  Require(!no_sblr_result.ok &&
              HasDiagnostic(no_sblr_result,
                            "SB_ENGINE_API_UDR_SBLR_INVOCATION_REQUIRED"),
          "UDR invocation without SBLR authority was admitted");
}

void TestClusterProviderBoundary() {
  const auto* provider = api::FindExtensionBoundaryManifestEntry(
      "cluster_provider.v1");
  Require(provider != nullptr, "cluster provider manifest row is missing");

  api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical = "cluster-provider-boundary-database";
  context.session_uuid.canonical = "cluster-provider-boundary-session";
  context.principal_uuid.canonical = "cluster-provider-boundary-principal";

  auto info_envelope = sblr::MakeSblrEnvelope(
      std::string(cluster_provider::kClusterProviderInfoOperationId),
      std::string(cluster_provider::kClusterProviderInfoOpcode),
      "extension-boundary-cluster-provider-info");
  info_envelope.requires_security_context = true;
  sblr::SblrDispatchRequest info_request;
  info_request.context = context;
  info_request.envelope = info_envelope;
  const auto info_result = sblr::DispatchSblrOperation(info_request);
  Require(info_result.envelope_validated && info_result.accepted &&
              info_result.dispatched_to_api,
          "cluster provider info did not route through SBLR provider boundary");
  Require(info_result.api_result.ok,
          "cluster provider info command failed");
  Require(FieldValue(info_result.api_result, "provider_name") ==
              cluster_provider::DescribeClusterProvider().provider_name,
          "cluster provider info name drifted");
  Require(FieldValue(info_result.api_result, "provider_type") ==
              cluster_provider::DescribeClusterProvider().provider_type,
          "cluster provider info type drifted");
  Require(FieldValue(info_result.api_result, "provider_version") ==
              cluster_provider::DescribeClusterProvider().provider_version,
          "cluster provider info version drifted");

  auto cluster_envelope = sblr::MakeSblrEnvelope(
      "cluster.inspect_state",
      "SBLR_CLUSTER_INSPECT_STATE",
      "extension-boundary-cluster-provider-execute");
  cluster_envelope.requires_security_context = true;
  cluster_envelope.requires_cluster_authority = true;
  sblr::SblrDispatchRequest cluster_request;
  cluster_request.context = context;
  cluster_request.envelope = cluster_envelope;
  const auto cluster_result = sblr::DispatchSblrOperation(cluster_request);
  Require(cluster_result.envelope_validated && cluster_result.accepted &&
              cluster_result.dispatched_to_api,
          "cluster execution operation did not reach provider boundary");
  Require(HasEvidence(cluster_result.api_result, "cluster_provider_name",
                      cluster_provider::DescribeClusterProvider().provider_name),
          "cluster provider execution omitted provider-name evidence");
  Require(HasEvidence(cluster_result.api_result, "cluster_provider_type",
                      cluster_provider::DescribeClusterProvider().provider_type),
          "cluster provider execution omitted provider-type evidence");

  if (cluster_provider::ClusterProviderSupportsExecution()) {
    Require(cluster_result.api_result.ok,
            "cluster-enabled provider/stub did not return success");
    Require(cluster_result.api_result.result_shape.result_kind ==
                "cluster.provider.stub.v1",
            "cluster provider/stub result shape drifted");
  } else {
    Require(!cluster_result.api_result.ok,
            "no-cluster provider executed a cluster operation");
    Require(cluster_result.api_result.cluster_authority_required,
            "no-cluster provider did not mark cluster authority as required");
    Require(HasDiagnostic(cluster_result.api_result,
                          cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster provider diagnostic drifted");
    Require(HasDispatchDiagnostic(cluster_result,
                                  cluster_provider::kClusterSupportNotEnabledCode),
            "no-cluster dispatch diagnostic drifted");
    Require(provider->non_cluster_refusal_code ==
                cluster_provider::kClusterSupportNotEnabledCode,
            "manifest no-cluster refusal code drifted");
  }
}

}  // namespace

int main() {
  TestManifestRows();

  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "extension_boundary_abi.sbdb";
  SeedActiveTransaction(database_path, 77);
  TestParserPackageBoundary(database_path);
  TestTrustedParserSupportUdrBoundary(database_path);
  TestClusterProviderBoundary();
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
