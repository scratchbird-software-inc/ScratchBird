// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/udr_api.hpp"
#include "transaction/transaction_api.hpp"
#include "database_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

std::string TempPath() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return "/tmp/sb_udr_lifecycle_probe_" + std::to_string(now) + ".db";
}

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

scratchbird::core::platform::TypedUuid Generate(scratchbird::core::platform::UuidKind kind, std::uint64_t millis) {
  auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

bool CreateDatabase(const std::string& path) {
  std::filesystem::remove(path);
  std::filesystem::remove(path + ".sb.owner.lock");
  scratchbird::storage::database::DatabaseCreateConfig create;
  const auto seed = NowMillis();
  create.path = path;
  create.database_uuid = Generate(scratchbird::core::platform::UuidKind::database, seed);
  create.filespace_uuid = Generate(scratchbird::core::platform::UuidKind::filespace, seed + 1);
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  return create.database_uuid.valid() && create.filespace_uuid.valid() &&
         scratchbird::storage::database::CreateDatabaseFile(create).ok();
}

EngineRequestContext BaseContext(const std::string& path, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "udr-lifecycle-secure" : "udr-lifecycle-open";
  context.database_path = path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001601";
  context.session_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001602" : "00000000-0000-7000-8000-000000001603";
  context.principal_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001604" : "";
  return context;
}

EngineRequestContext Begin(const std::string& path, bool secure) {
  auto context = BaseContext(path, secure);
  EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto tx = EngineBeginTransaction(begin);
  if (tx.ok) {
    context.local_transaction_id = tx.local_transaction_id;
    context.transaction_uuid = tx.transaction_uuid;
  }
  return context;
}

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest commit;
  commit.context = context;
  return EngineCommitTransaction(commit).ok;
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

bool RowFieldEquals(const EngineApiResult& result, const std::string& field_name, const std::string& expected) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name && field.second.encoded_value == expected) { return true; }
    }
  }
  return false;
}

EngineRegisterUdrPackageRequest RegisterRequest(const EngineRequestContext& context,
                                                const std::string& uuid,
                                                const std::string& name) {
  EngineRegisterUdrPackageRequest request;
  request.context = context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "udr_package";
  request.localized_names.push_back({"en", "default", "/extensions/udr", name, true});
  request.option_envelopes.push_back("permission:manage_udr");
  request.option_envelopes.push_back("trust:trusted_cpp");
  request.option_envelopes.push_back("abi:sb_udr_1");
  request.option_envelopes.push_back("binary_digest:sha256:00112233445566778899aabbccddeeff");
  return request;
}

EngineLoadUdrPackageRequest LoadRequest(const EngineRequestContext& context, const std::string& uuid) {
  EngineLoadUdrPackageRequest request;
  request.context = context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "udr_package";
  request.option_envelopes.push_back("permission:manage_udr");
  request.option_envelopes.push_back("abi:sb_udr_1");
  return request;
}

EngineUnloadUdrPackageRequest UnloadRequest(const EngineRequestContext& context, const std::string& uuid) {
  EngineUnloadUdrPackageRequest request;
  request.context = context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "udr_package";
  request.option_envelopes.push_back("permission:manage_udr");
  return request;
}

EngineInvokeUdrPackageRequest InvokeRequest(const EngineRequestContext& context, const std::string& uuid) {
  EngineInvokeUdrPackageRequest request;
  request.context = context;
  request.target_object.uuid.canonical = uuid;
  request.target_object.object_kind = "udr_package";
  request.option_envelopes.push_back("permission:invoke_udr");
  request.option_envelopes.push_back("sblr_opcode:SBLR_UDR_INVOKE");
  request.option_envelopes.push_back("entrypoint:echo");
  request.option_envelopes.push_back("payload:hello");
  request.option_envelopes.push_back("memory_budget_bytes:1024");
  request.option_envelopes.push_back("cpu_budget_microseconds:1000");
  return request;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main() {
  const auto path = TempPath();
  const bool database_created = CreateDatabase(path);

  const std::string dependency_uuid = "00000000-0000-7000-8000-000000001611";
  const std::string main_uuid = "00000000-0000-7000-8000-000000001612";
  const std::string missing_uuid = "00000000-0000-7000-8000-000000001613";

  auto open_context = Begin(path, false);
  auto open_request = RegisterRequest(open_context, "00000000-0000-7000-8000-000000001614", "open_udr");
  const auto security_result = EngineRegisterUdrPackage(open_request);

  auto bad_context = Begin(path, true);
  auto no_permission_request = RegisterRequest(bad_context, "00000000-0000-7000-8000-000000001615", "no_permission_udr");
  no_permission_request.option_envelopes.clear();
  no_permission_request.option_envelopes.push_back("trust:trusted_cpp");
  no_permission_request.option_envelopes.push_back("abi:sb_udr_1");
  const auto no_permission_result = EngineRegisterUdrPackage(no_permission_request);

  auto bad_abi_request = RegisterRequest(bad_context, "00000000-0000-7000-8000-000000001616", "bad_abi_udr");
  bad_abi_request.option_envelopes.clear();
  bad_abi_request.option_envelopes.push_back("permission:manage_udr");
  bad_abi_request.option_envelopes.push_back("trust:trusted_cpp");
  bad_abi_request.option_envelopes.push_back("abi:reference_private_0");
  const auto bad_abi_result = EngineRegisterUdrPackage(bad_abi_request);

  auto bypass_request = RegisterRequest(bad_context, "00000000-0000-7000-8000-000000001617", "bypass_udr");
  bypass_request.option_envelopes.push_back("bypass_mga");
  const auto bypass_result = EngineRegisterUdrPackage(bypass_request);
  const bool bad_commit = Commit(bad_context);

  auto dep_register_context = Begin(path, true);
  const auto dependency_result = EngineRegisterUdrPackage(RegisterRequest(dep_register_context, dependency_uuid, "dependency_udr"));
  const bool dependency_commit = Commit(dep_register_context);

  auto main_register_context = Begin(path, true);
  auto main_register = RegisterRequest(main_register_context, main_uuid, "main_udr");
  main_register.related_objects.push_back({{dependency_uuid}, "udr_package"});
  const auto main_register_result = EngineRegisterUdrPackage(main_register);
  const bool main_register_commit = Commit(main_register_context);

  auto lifecycle_context = Begin(path, true);
  auto missing_load = LoadRequest(lifecycle_context, main_uuid);
  missing_load.related_objects.push_back({{missing_uuid}, "udr_package"});
  const auto missing_dependency_result = EngineLoadUdrPackage(missing_load);

  auto cluster_load = LoadRequest(lifecycle_context, main_uuid);
  cluster_load.option_envelopes.push_back("global_deploy:true");
  const auto cluster_result = EngineLoadUdrPackage(cluster_load);

  auto load = LoadRequest(lifecycle_context, main_uuid);
  load.related_objects.push_back({{dependency_uuid}, "udr_package"});
  const auto load_result = EngineLoadUdrPackage(load);

  auto direct_invoke = InvokeRequest(lifecycle_context, main_uuid);
  direct_invoke.option_envelopes.clear();
  direct_invoke.option_envelopes.push_back("permission:invoke_udr");
  direct_invoke.option_envelopes.push_back("entrypoint:echo");
  const auto direct_invoke_result = EngineInvokeUdrPackage(direct_invoke);

  auto budget_invoke = InvokeRequest(lifecycle_context, main_uuid);
  budget_invoke.option_envelopes.clear();
  budget_invoke.option_envelopes.push_back("permission:invoke_udr");
  budget_invoke.option_envelopes.push_back("sblr_opcode:SBLR_UDR_INVOKE");
  budget_invoke.option_envelopes.push_back("entrypoint:echo");
  budget_invoke.option_envelopes.push_back("payload:hello");
  budget_invoke.option_envelopes.push_back("memory_budget_bytes:1");
  budget_invoke.option_envelopes.push_back("cpu_budget_microseconds:1000");
  const auto budget_invoke_result = EngineInvokeUdrPackage(budget_invoke);

  const auto invoke_result = EngineInvokeUdrPackage(InvokeRequest(lifecycle_context, main_uuid));

  EngineInspectUdrPackageRequest inspect;
  inspect.context = lifecycle_context;
  inspect.option_envelopes.push_back("permission:inspect_udr");
  const auto inspect_result = EngineInspectUdrPackages(inspect);

  const auto unload_result = EngineUnloadUdrPackage(UnloadRequest(lifecycle_context, main_uuid));
  const bool lifecycle_commit = Commit(lifecycle_context);

  auto post_context = Begin(path, true);
  EngineInspectUdrPackageRequest post_inspect;
  post_inspect.context = post_context;
  post_inspect.option_envelopes.push_back("permission:inspect_udr");
  const auto post_inspect_result = EngineInspectUdrPackages(post_inspect);
  const bool post_commit = Commit(post_context);

  const bool security_required = !security_result.ok && HasDiagnosticCode(security_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool permission_required = !no_permission_result.ok && HasDiagnosticCode(no_permission_result, "SB_ENGINE_API_UDR_PERMISSION_REQUIRED");
  const bool abi_rejected = !bad_abi_result.ok && HasDiagnosticCode(bad_abi_result, "SB_ENGINE_API_UDR_ABI_UNSUPPORTED");
  const bool bypass_rejected = !bypass_result.ok && HasDiagnosticCode(bypass_result, "SB_ENGINE_API_UDR_AUTHORITY_BYPASS_REFUSED");
  const bool dependency_registered = dependency_result.ok && HasEvidence(dependency_result, "udr_audit", "registration_evidence_recorded");
  const bool main_registered = main_register_result.ok && HasEvidence(main_register_result, "udr_abi", "supported");
  const bool missing_dependency_rejected = !missing_dependency_result.ok && HasDiagnosticCode(missing_dependency_result, "SB_ENGINE_API_UDR_DEPENDENCY_MISSING");
  const bool cluster_denied = !cluster_result.ok && cluster_result.cluster_authority_required;
  const bool loaded = load_result.ok && HasEvidence(load_result, "extension_behavior", "loaded") && HasEvidence(load_result, "authority_boundary");
  const bool direct_invoke_rejected = !direct_invoke_result.ok && HasDiagnosticCode(direct_invoke_result, "SB_ENGINE_API_UDR_SBLR_INVOCATION_REQUIRED");
  const bool budget_invoke_rejected = !budget_invoke_result.ok && HasDiagnosticCode(budget_invoke_result, "SB_ENGINE_API_UDR_RESOURCE_LIMIT_EXCEEDED");
  const bool invoked = invoke_result.ok && HasEvidence(invoke_result, "sblr_authority", "SBLR_UDR_INVOKE") && RowFieldEquals(invoke_result, "invocation_result", "udr_entrypoint_admitted");
  const bool inspect_loaded = inspect_result.ok && RowFieldEquals(inspect_result, "state", "loaded");
  const bool unloaded = unload_result.ok && HasEvidence(unload_result, "extension_behavior", "unloaded");
  const bool inspect_unloaded = post_inspect_result.ok && RowFieldEquals(post_inspect_result, "state", "unloaded");

  const bool ok = database_created && security_required && permission_required && abi_rejected && bypass_rejected && bad_commit &&
                  dependency_registered && dependency_commit && main_registered && main_register_commit &&
                  missing_dependency_rejected && cluster_denied && loaded && direct_invoke_rejected &&
                  budget_invoke_rejected && invoked && inspect_loaded && unloaded &&
                  lifecycle_commit && inspect_unloaded && post_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("database_created", database_created, true);
  PrintBool("security_required", security_required, true);
  PrintBool("permission_required", permission_required, true);
  PrintBool("abi_rejected", abi_rejected, true);
  PrintBool("bypass_rejected", bypass_rejected, true);
  PrintBool("dependency_registered", dependency_registered, true);
  PrintBool("main_registered", main_registered, true);
  PrintBool("missing_dependency_rejected", missing_dependency_rejected, true);
  PrintBool("cluster_denied", cluster_denied, true);
  PrintBool("loaded", loaded, true);
  PrintBool("direct_invoke_rejected", direct_invoke_rejected, true);
  PrintBool("budget_invoke_rejected", budget_invoke_rejected, true);
  PrintBool("invoked", invoked, true);
  PrintBool("inspect_loaded", inspect_loaded, true);
  PrintBool("unloaded", unloaded, true);
  PrintBool("inspect_unloaded", inspect_unloaded, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
