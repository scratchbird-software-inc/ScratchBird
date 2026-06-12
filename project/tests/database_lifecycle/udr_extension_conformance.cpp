// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/udr_api.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "metric_registry.hpp"
#include "parser_package_registry.hpp"
#include "sblr_admission.hpp"
#include "sbu_firebird_parser_support.hpp"
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
namespace metrics = scratchbird::core::metrics;
namespace server = scratchbird::server;
namespace udr_runtime = scratchbird::udr::runtime;
namespace firebird_udr = scratchbird::udr::firebird_parser_support;
namespace sbsql_udr = scratchbird::udr::sbsql_parser_support;

constexpr std::string_view kDatabaseUuid = "019e13b0-0000-7000-8000-000000000001";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013l_udr.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013L UDR test");
  return std::filesystem::path(made);
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
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

bool HasServerDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                         std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
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

bool HasRowField(const api::EngineApiResult& result,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, key) == value) return true;
  }
  return false;
}

bool RowFieldContains(const api::EngineApiResult& result,
                      std::string_view key,
                      std::string_view needle) {
  for (const auto& row : result.result_shape.rows) {
    if (Contains(FieldValue(row, key), needle)) return true;
  }
  return false;
}

bool RuntimeStateHasEntrypoint(const udr_runtime::UdrPackageRuntimeState& state,
                               std::string_view entrypoint_name) {
  for (const auto& entrypoint : state.entrypoint_names) {
    if (entrypoint == entrypoint_name) return true;
  }
  return false;
}

bool MetricHasLabel(const metrics::MetricValue& value,
                    std::string_view key,
                    std::string_view expected) {
  for (const auto& label : value.labels) {
    if (label.key == key && label.value == expected) return true;
  }
  return false;
}

bool HasMetricValue(std::string_view family,
                    std::string_view key,
                    std::string_view expected) {
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent()) {
    if (value.family == family && MetricHasLabel(value, key, expected)) {
      return true;
    }
  }
  return false;
}

void ReplaceOption(std::vector<std::string>* options,
                   std::string_view prefix,
                   std::string replacement) {
  for (auto& option : *options) {
    if (option.rfind(std::string(prefix), 0) == 0) {
      option = std::move(replacement);
      return;
    }
  }
  options->push_back(std::move(replacement));
}

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  std::uint64_t tx = 10) {
  api::EngineRequestContext context;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseUuid);
  context.principal_uuid.canonical = "019e13b0-0000-7000-8000-000000000201";
  context.session_uuid.canonical = "019e13b0-0000-7000-8000-000000000202";
  context.transaction_uuid.canonical = "019e13b0-0000-7000-8000-000000000203";
  context.local_transaction_id = tx;
  context.security_context_present = true;
  context.catalog_generation_id = 3;
  context.security_epoch = 3;
  context.resource_epoch = 3;
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

template <typename TRequest>
TRequest UdrRequest(const std::filesystem::path& database_path,
                    std::string_view uuid = sbsql_udr::kSbuSbsqlPackageUuid,
                    std::uint64_t tx = 10) {
  TRequest request;
  request.context = Context(database_path, tx);
  request.target_database.uuid.canonical = std::string(kDatabaseUuid);
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical = std::string(uuid);
  request.target_object.object_kind = "udr_package";
  request.localized_names.push_back(LocalizedName("sbu_sbsql_parser_support"));
  return request;
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

void AddInvokeUdrOptions(api::EngineApiRequest* request) {
  request->option_envelopes.push_back("permission:invoke_udr");
  request->option_envelopes.push_back("sblr_authorized_invocation:true");
  request->option_envelopes.push_back("operation_family:sblr.udr.operation.v3");
  request->option_envelopes.push_back("entrypoint:sbu_sbsql_parse_to_sblr");
  request->option_envelopes.push_back("payload:select 1");
  request->option_envelopes.push_back("context_packet:engine_context=trusted;resolver=public;authenticated=true");
  request->option_envelopes.push_back("memory_budget_bytes:4096");
  request->option_envelopes.push_back("cpu_budget_microseconds:1000");
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string out;
  std::string line;
  while (std::getline(in, line)) {
    out += line;
    out.push_back('\n');
  }
  return out;
}

void SeedActiveTransaction(const std::filesystem::path& database_path, std::uint64_t tx) {
  std::ofstream out(database_path.string() + ".sb.crud_events", std::ios::binary | std::ios::app);
  out << "SBCRUD1\tTX_BEGIN\t" << tx << "\tudr_lifecycle_test\n";
  Require(static_cast<bool>(out), "failed to seed MGA transaction evidence for UDR test");
}

void TestEngineOwnedUdrLifecycle(const std::filesystem::path& database_path) {
  const auto sbsql_descriptor = sbsql_udr::sbu_sbsql_package_descriptor();
  const auto firebird_descriptor = firebird_udr::sbu_firebird_package_descriptor();
  SeedActiveTransaction(database_path, 10);

  auto register_request = UdrRequest<api::EngineRegisterUdrPackageRequest>(database_path);
  AddManageUdrOptions(&register_request, sbsql_descriptor);
  auto bad_provenance = register_request;
  ReplaceOption(&bad_provenance.option_envelopes,
                "source_revision:",
                "source_revision:tampered-source-revision");
  const auto bad_provenance_result = api::EngineRegisterUdrPackage(bad_provenance);
  Require(!bad_provenance_result.ok &&
              HasDiagnostic(bad_provenance_result, "SB_ENGINE_API_UDR_DESCRIPTOR_MISMATCH"),
          "UDR registration admitted mismatched runtime descriptor provenance");

  const auto registered = api::EngineRegisterUdrPackage(register_request);
  Require(registered.ok, "trusted C++ UDR registration was refused");
  Require(HasEvidence(registered, "extension_behavior", "registered"),
          "UDR registration did not emit lifecycle evidence");
  Require(HasEvidence(registered, "authority_boundary",
                      "mga_sblr_uuid_security_transaction_preserved"),
          "UDR registration did not publish authority-boundary evidence");
  Require(HasEvidence(registered, "udr_descriptor", "runtime_descriptor_validated"),
          "UDR registration did not validate the linked runtime descriptor");
  Require(HasRowField(registered, "entrypoint_count",
                      std::to_string(sbsql_descriptor.entrypoints.size())),
          "UDR registration did not expose the validated dispatch table size");
  Require(HasRowField(registered, "runtime_language", "cpp"),
          "UDR registration did not expose the trusted C++ runtime language");

  const auto duplicate = api::EngineRegisterUdrPackage(register_request);
  Require(!duplicate.ok && HasDiagnostic(duplicate, "SB_ENGINE_API_UDR_ALREADY_REGISTERED"),
          "duplicate UDR registration was not refused");

  auto untrusted = UdrRequest<api::EngineRegisterUdrPackageRequest>(
      database_path, "019e13b0-0000-7000-8000-000000000103");
  untrusted.option_envelopes.push_back("permission:manage_udr");
  untrusted.option_envelopes.push_back("abi:sb_udr_v1");
  const auto untrusted_result = api::EngineRegisterUdrPackage(untrusted);
  Require(!untrusted_result.ok &&
              HasDiagnostic(untrusted_result, "SB_ENGINE_API_UDR_TRUST_REQUIRED"),
          "non-trusted UDR profile was admitted");

  // MDF-017 / DEFER-CXX-ONLY-UDR-ENFORCEMENT: C++ UDRs are the only runtime
  // target admitted by the direct runtime registry or the engine API surface.
  auto non_cpp_descriptor = sbsql_descriptor;
  non_cpp_descriptor.package_uuid = "019e13b0-0000-7000-8000-000000000108";
  non_cpp_descriptor.package_name = "non_cpp_udr_runtime";
  non_cpp_descriptor.runtime_language = "python";
  const auto non_cpp_runtime_result =
      udr_runtime::RegisterPackage(non_cpp_descriptor);
  Require(!non_cpp_runtime_result.ok &&
              non_cpp_runtime_result.diagnostic_code ==
                  "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN",
          "non-C++ UDR runtime descriptor was admitted");

  auto non_cpp_request = UdrRequest<api::EngineRegisterUdrPackageRequest>(
      database_path, "019e13b0-0000-7000-8000-000000000109");
  AddManageUdrOptions(&non_cpp_request, sbsql_descriptor);
  non_cpp_request.option_envelopes.push_back("runtime_language:python");
  const auto non_cpp_request_result =
      api::EngineRegisterUdrPackage(non_cpp_request);
  Require(!non_cpp_request_result.ok &&
              HasDiagnostic(non_cpp_request_result,
                            "SB_ENGINE_API_UDR_NON_CPP_RUNTIME_FORBIDDEN"),
          "engine registration admitted a non-C++ UDR runtime target");
  Require(HasMetricValue("sb_udr_non_cpp_refusal_total", "reason", "python"),
          "non-C++ UDR refusal metric was not emitted");

  auto missing_tx = UdrRequest<api::EngineRegisterUdrPackageRequest>(
      database_path, "019e13b0-0000-7000-8000-000000000104", 0);
  AddManageUdrOptions(&missing_tx, sbsql_descriptor);
  const auto missing_tx_result = api::EngineRegisterUdrPackage(missing_tx);
  Require(!missing_tx_result.ok &&
              HasDiagnostic(missing_tx_result, "SB_ENGINE_API_INVALID_REQUEST"),
          "UDR registration admitted a missing MGA transaction context");

  auto cluster = UdrRequest<api::EngineRegisterUdrPackageRequest>(
      database_path, "019e13b0-0000-7000-8000-000000000105");
  AddManageUdrOptions(&cluster, sbsql_descriptor);
  cluster.option_envelopes.push_back("cluster_deploy:true");
  const auto cluster_result = api::EngineRegisterUdrPackage(cluster);
  Require(!cluster_result.ok &&
              HasDiagnostic(cluster_result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
          "UDR cluster path did not fail closed without cluster authority");

  auto bypass = UdrRequest<api::EngineRegisterUdrPackageRequest>(
      database_path, "019e13b0-0000-7000-8000-000000000106");
  AddManageUdrOptions(&bypass, sbsql_descriptor);
  bypass.option_envelopes.push_back("bypass_mga");
  const auto bypass_result = api::EngineRegisterUdrPackage(bypass);
  Require(!bypass_result.ok &&
              HasDiagnostic(bypass_result, "SB_ENGINE_API_UDR_AUTHORITY_BYPASS_REFUSED"),
          "UDR authority bypass request was admitted");

  auto shutdown_register = UdrRequest<api::EngineRegisterUdrPackageRequest>(
      database_path, "019e13b0-0000-7000-8000-000000000107");
  AddManageUdrOptions(&shutdown_register, sbsql_descriptor);
  shutdown_register.option_envelopes.push_back("shutdown_draining:true");
  const auto shutdown_register_result = api::EngineRegisterUdrPackage(shutdown_register);
  Require(!shutdown_register_result.ok &&
              HasDiagnostic(shutdown_register_result, "SB_ENGINE_API_UDR_SHUTDOWN_DRAIN_ACTIVE"),
          "UDR registration was admitted during shutdown drain");

  auto load_request = UdrRequest<api::EngineLoadUdrPackageRequest>(database_path);
  AddManageUdrOptions(&load_request, sbsql_descriptor);
  const auto loaded = api::EngineLoadUdrPackage(load_request);
  Require(loaded.ok, "registered UDR load was refused");
  Require(HasEvidence(loaded, "extension_behavior", "loaded"),
          "UDR load did not emit lifecycle evidence");
  Require(HasEvidence(loaded, "udr_entrypoints", "dispatch_table_published"),
          "UDR load did not publish runtime dispatch-table evidence");
  const auto runtime_state = udr_runtime::GetPackageState(sbsql_descriptor.package_uuid);
  Require(runtime_state && runtime_state->loaded,
          "UDR runtime state was not loaded after engine load");
  Require(RuntimeStateHasEntrypoint(*runtime_state, "sbu_sbsql_parse_to_sblr"),
          "UDR runtime dispatch table is missing the SBSQL parse-to-SBLR entrypoint");
  Require(runtime_state->runtime_language == "cpp",
          "loaded UDR runtime state did not preserve C++ language enforcement");

  auto shutdown_load = UdrRequest<api::EngineLoadUdrPackageRequest>(database_path);
  AddManageUdrOptions(&shutdown_load, sbsql_descriptor);
  shutdown_load.option_envelopes.push_back("shutdown_generation:22");
  const auto shutdown_load_result = api::EngineLoadUdrPackage(shutdown_load);
  Require(!shutdown_load_result.ok &&
              HasDiagnostic(shutdown_load_result, "SB_ENGINE_API_UDR_SHUTDOWN_DRAIN_ACTIVE"),
          "UDR load was admitted during shutdown drain");

  auto invoke_without_sblr = UdrRequest<api::EngineInvokeUdrPackageRequest>(database_path);
  invoke_without_sblr.option_envelopes.push_back("permission:invoke_udr");
  const auto invoke_without_sblr_result = api::EngineInvokeUdrPackage(invoke_without_sblr);
  Require(!invoke_without_sblr_result.ok &&
              HasDiagnostic(invoke_without_sblr_result, "SB_ENGINE_API_UDR_SBLR_INVOCATION_REQUIRED"),
          "UDR invocation without SBLR authority was admitted");

  auto invoke_request = UdrRequest<api::EngineInvokeUdrPackageRequest>(database_path);
  AddInvokeUdrOptions(&invoke_request);
  const auto invoked = api::EngineInvokeUdrPackage(invoke_request);
  Require(invoked.ok, "loaded UDR invocation through SBLR authority was refused");
  Require(HasEvidence(invoked, "sblr_authority", "SBLR_UDR_INVOKE"),
          "UDR invocation did not record SBLR authority evidence");
  Require(HasEvidence(invoked, "udr_dispatch", "entrypoint_callback_invoked"),
          "UDR invocation did not dispatch through the trusted runtime callback");
  Require(RowFieldContains(invoked, "result_payload", "SBLRExecutionEnvelope.v3"),
          "UDR invocation did not return parser-generated SBLR");
  Require(!RowFieldContains(invoked, "result_payload", "select 1") &&
              !RowFieldContains(invoked, "result_payload", "sql_text"),
          "UDR invocation leaked raw SQL text through the engine result payload");

  auto resource = invoke_request;
  ReplaceOption(&resource.option_envelopes, "memory_budget_bytes:", "memory_budget_bytes:0");
  const auto resource_result = api::EngineInvokeUdrPackage(resource);
  Require(!resource_result.ok &&
              HasDiagnostic(resource_result, "SB_ENGINE_API_UDR_RESOURCE_LIMIT_EXCEEDED"),
          "UDR invocation ignored resource budget refusal");

  auto shutdown_invoke = invoke_request;
  shutdown_invoke.option_envelopes.push_back("database_shutdown_in_progress:true");
  const auto shutdown_invoke_result = api::EngineInvokeUdrPackage(shutdown_invoke);
  Require(!shutdown_invoke_result.ok &&
              HasDiagnostic(shutdown_invoke_result, "SB_ENGINE_API_UDR_SHUTDOWN_DRAIN_ACTIVE"),
          "UDR invocation was admitted during shutdown drain");

  auto second_register =
      UdrRequest<api::EngineRegisterUdrPackageRequest>(database_path,
                                                       firebird_descriptor.package_uuid);
  AddManageUdrOptions(&second_register, firebird_descriptor);
  Require(api::EngineRegisterUdrPackage(second_register).ok,
          "second UDR registration for not-loaded check failed");
  auto not_loaded = UdrRequest<api::EngineInvokeUdrPackageRequest>(
      database_path, firebird_descriptor.package_uuid);
  AddInvokeUdrOptions(&not_loaded);
  const auto not_loaded_result = api::EngineInvokeUdrPackage(not_loaded);
  Require(!not_loaded_result.ok && HasDiagnostic(not_loaded_result, "SB_ENGINE_API_UDR_NOT_LOADED"),
          "unloaded registered UDR invocation was admitted");

  auto loaded_admission_registry = server::ParserPackageRegistry{};
  loaded_admission_registry.entries.push_back(server::ParserPackageRegistryEntry{});
  loaded_admission_registry.entries.front().parser_support_udr_required = true;
  loaded_admission_registry.entries.front().parser_support_udr_available = true;
  loaded_admission_registry.entries.front().parser_support_udr_uuid =
      sbsql_descriptor.package_uuid;
  loaded_admission_registry.entries.front().parser_support_udr_abi =
      sbsql_descriptor.abi_version;
  loaded_admission_registry.entries.front().parser_support_udr_source_revision =
      sbsql_descriptor.source_revision;
  loaded_admission_registry.entries.front().parser_support_udr_binary_hash =
      sbsql_descriptor.binary_hash;
  loaded_admission_registry.entries.front().parser_support_udr_signature_policy =
      sbsql_descriptor.signature_policy;
  loaded_admission_registry.entries.front().parser_support_udr_capability_role =
      sbsql_descriptor.capability_role;
  const auto hello = server::sbps::DecodeHelloRequest(server::sbps::EncodeHelloRequestForTest());
  Require(hello.has_value(), "failed to construct SBPS hello request for parser UDR admission");
  const auto parser_admitted = server::AdmitParserPackage(
      loaded_admission_registry,
      *hello,
      server::sbps::kProtocolMajor,
      server::sbps::kProtocolMinor);
  Require(parser_admitted.admitted,
          "parser package admission did not accept the loaded trusted parser-support UDR");

  auto source_mismatch_registry = loaded_admission_registry;
  source_mismatch_registry.entries.front().parser_support_udr_source_revision =
      "tampered-source-revision";
  const auto parser_rejected = server::AdmitParserPackage(
      source_mismatch_registry,
      *hello,
      server::sbps::kProtocolMajor,
      server::sbps::kProtocolMinor);
  Require(!parser_rejected.admitted &&
              HasServerDiagnostic(parser_rejected.diagnostics,
                                  "SERVER.PARSER.SUPPORT_UDR_REJECTED"),
          "parser package admission ignored parser-support UDR descriptor mismatch");

  udr_runtime::UdrInvocationLease lease;
  const auto lease_status =
      udr_runtime::AcquireInvocationRef(sbsql_descriptor.package_uuid, &lease);
  Require(lease_status.ok && lease.held(), "failed to acquire UDR active invocation lease");
  auto blocked_unload = UdrRequest<api::EngineUnloadUdrPackageRequest>(database_path);
  AddManageUdrOptions(&blocked_unload, sbsql_descriptor);
  const auto blocked_unload_result = api::EngineUnloadUdrPackage(blocked_unload);
  Require(!blocked_unload_result.ok &&
              HasDiagnostic(blocked_unload_result, "SB_ENGINE_API_UDR_UNLOAD_BLOCKED"),
          "UDR unload did not block while active invocations were held");
  lease.Release();

  auto unload_request = UdrRequest<api::EngineUnloadUdrPackageRequest>(database_path);
  AddManageUdrOptions(&unload_request, sbsql_descriptor);
  unload_request.option_envelopes.push_back("shutdown_draining:true");
  const auto unloaded = api::EngineUnloadUdrPackage(unload_request);
  Require(unloaded.ok, "shutdown cleanup UDR unload was refused");
  Require(HasEvidence(unloaded, "extension_behavior", "unloaded"),
          "UDR unload did not emit lifecycle evidence");
  Require(HasEvidence(unloaded, "udr_entrypoints", "dispatch_table_removed"),
          "UDR unload did not remove runtime dispatch table evidence");

  const auto parser_missing = server::AdmitParserPackage(
      loaded_admission_registry,
      *hello,
      server::sbps::kProtocolMajor,
      server::sbps::kProtocolMinor);
  Require(!parser_missing.admitted &&
              HasServerDiagnostic(parser_missing.diagnostics,
                                  "SERVER.PARSER.SUPPORT_UDR_MISSING"),
          "parser package admission did not reject an unloaded parser-support UDR");

  auto inspect = UdrRequest<api::EngineInspectUdrPackageRequest>(database_path);
  inspect.option_envelopes.push_back("permission:inspect_udr");
  const auto inspected = api::EngineInspectUdrPackages(inspect);
  Require(inspected.ok, "UDR inspect was refused");
  Require(inspected.result_shape.rows.size() >= 2, "UDR inspect did not return lifecycle rows");
  Require(RowFieldContains(inspected, "entrypoints", "sbu_sbsql_parse_to_sblr"),
          "UDR inspect did not expose sanitized runtime entrypoint inventory");
  Require(RowFieldContains(inspected, "runtime_language", "cpp"),
          "UDR inspect did not expose the C++ runtime-language inventory");

  const auto restart_catalog = api::VisibleApiBehaviorRecords(
      Context(database_path, 200), "udr_package", 200);
  Require(restart_catalog.size() >= 2,
          "restart catalog reload did not reconstruct UDR package rows");

  const auto event_text = ReadFile(database_path.string() + ".sb.api_events");
  Require(Contains(event_text, "registered") && Contains(event_text, "loaded") &&
              Contains(event_text, "unloaded"),
          "UDR durable lifecycle event evidence was not persisted");
  Require(!Contains(event_text, "PRAGMA") && !Contains(event_text, "journal_mode"),
          "UDR lifecycle evidence introduced non-MGA journal authority text");
}

void TestServerSblrUdrAdmission() {
  server::ServerSblrAdmissionRequest request;
  request.encoded_sblr_envelope =
      "operation_id=extensibility.invoke_udr_package\n"
      "result_shape=engine_result\n"
      "diagnostic_shape=message_vector\n"
      "parser_resolved_names_to_uuids=true\n";
  const auto admitted = server::AdmitServerSblrEnvelope(request);
  Require(admitted.admitted, "server did not admit UDR SBLR operation family");
  Require(admitted.operation_family == "sblr.udr.operation.v3",
          "server did not classify UDR operation as sblr.udr.operation.v3");

  server::ServerSblrAdmissionRequest raw_sql;
  raw_sql.encoded_sblr_envelope = "select extensibility.invoke_udr_package";
  const auto raw_sql_result = server::AdmitServerSblrEnvelope(raw_sql);
  Require(!raw_sql_result.admitted &&
              HasServerDiagnostic(raw_sql_result.diagnostics, "SBLR.SQL_TEXT_FORBIDDEN"),
          "server admitted raw SQL as a UDR route");
}

}  // namespace

int main() {
  udr_runtime::ResetRuntimeForTest();
  const auto sbsql_registered =
      udr_runtime::RegisterPackage(sbsql_udr::sbu_sbsql_package_descriptor());
  Require(sbsql_registered.ok, "failed to register SBSQL parser-support runtime descriptor");
  const auto firebird_registered =
      udr_runtime::RegisterPackage(firebird_udr::sbu_firebird_package_descriptor());
  Require(firebird_registered.ok, "failed to register Firebird parser-support runtime descriptor");

  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "udr_lifecycle.sbdb";
  TestEngineOwnedUdrLifecycle(database_path);
  TestServerSblrUdrAdmission();
  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
