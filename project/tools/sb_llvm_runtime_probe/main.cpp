// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/llvm_api.hpp"
#include "transaction/transaction_api.hpp"
#include "database_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string TempPath() {
  return "/tmp/sb_llvm_runtime_probe_" + std::to_string(NowMillis()) + ".db";
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
  context.request_id = secure ? "llvm-runtime-secure" : "llvm-runtime-open";
  context.database_path = path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001701";
  context.session_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001702" : "00000000-0000-7000-8000-000000001703";
  context.principal_uuid.canonical = secure ? "00000000-0000-7000-8000-000000001704" : "";
  context.security_epoch = 42;
  return context;
}

EngineRequestContext Begin(const std::string& path, bool secure) {
  auto context = BaseContext(path, secure);
  EngineBeginTransactionRequest begin;
  begin.context = context;
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

EngineCompileLlvmModuleRequest CompileRequest(const EngineRequestContext& context, const std::string& mode) {
  EngineCompileLlvmModuleRequest request;
  request.context = context;
  request.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001711";
  request.option_envelopes.push_back("compile:" + mode);
  request.option_envelopes.push_back("module:sblr_project_filter_fragment");
  request.descriptors.push_back({{"00000000-0000-7000-8000-000000001712"}, "scalar", "int32", "canonical=int32"});
  request.policy_profile.encoded_profiles.push_back("policy=default");
  request.physical_profile.encoded_profiles.push_back("page_size=16384");
  return request;
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

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main() {
  const auto path = TempPath();
  const bool database_created = CreateDatabase(path);

  auto open_context = Begin(path, false);
  auto open_request = CompileRequest(open_context, "jit");
  const auto security_result = EngineCompileLlvmModule(open_request);

  auto invalid_context = Begin(path, true);
  auto raw_sql = CompileRequest(invalid_context, "jit");
  raw_sql.option_envelopes.clear();
  raw_sql.option_envelopes.push_back("compile:jit");
  raw_sql.option_envelopes.push_back("module:sql:select 1");
  const auto raw_sql_result = EngineCompileLlvmModule(raw_sql);

  auto bad_mode = CompileRequest(invalid_context, "bytecode");
  const auto bad_mode_result = EngineCompileLlvmModule(bad_mode);

  auto unavailable = CompileRequest(invalid_context, "jit");
  unavailable.option_envelopes.push_back("simulate_llvm_unavailable:true");
  unavailable.option_envelopes.push_back("llvm_test_fixture:true");
  const auto unavailable_result = EngineCompileLlvmModule(unavailable);

  auto fallback = CompileRequest(invalid_context, "jit");
  fallback.option_envelopes.push_back("simulate_llvm_unavailable:true");
  fallback.option_envelopes.push_back("llvm_test_fixture:true");
  fallback.option_envelopes.push_back("fallback:interpreter");
  const auto fallback_result = EngineCompileLlvmModule(fallback);

  auto cluster = CompileRequest(invalid_context, "jit");
  cluster.option_envelopes.push_back("global_deploy:true");
  const auto cluster_result = EngineCompileLlvmModule(cluster);
  const bool invalid_commit = Commit(invalid_context);

  auto first_context = Begin(path, true);
  const auto jit_first_result = EngineCompileLlvmModule(CompileRequest(first_context, "jit"));
  const bool first_commit = Commit(first_context);

  auto second_context = Begin(path, true);
  const auto jit_second_result = EngineCompileLlvmModule(CompileRequest(second_context, "jit"));
  auto aot = CompileRequest(second_context, "aot");
  aot.option_envelopes.push_back("cache_key:llvm-runtime-aot-key");
  const auto aot_result = EngineCompileLlvmModule(aot);
  const bool second_commit = Commit(second_context);

  const bool security_required = !security_result.ok && HasDiagnosticCode(security_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool raw_sql_rejected = !raw_sql_result.ok && HasDiagnosticCode(raw_sql_result, "SB_ENGINE_API_LLVM_RAW_SQL_REFUSED");
  const bool bad_mode_rejected = !bad_mode_result.ok && HasDiagnosticCode(bad_mode_result, "SB_ENGINE_API_LLVM_MODE_UNSUPPORTED");
  const bool unavailable_rejected = !unavailable_result.ok && HasDiagnosticCode(unavailable_result, "SB_ENGINE_API_LLVM_REQUIRED_UNAVAILABLE");
  const bool fallback_ok = fallback_result.ok && HasEvidence(fallback_result, "extension_behavior", "interpreter_fallback") && RowFieldEquals(fallback_result, "interpreter_fallback", "true");
  const bool cluster_denied = !cluster_result.ok && cluster_result.cluster_authority_required;
  const bool jit_miss = jit_first_result.ok && HasEvidence(jit_first_result, "extension_behavior", "jit_compiled") &&
                        HasEvidence(jit_first_result, "llvm_cache", "miss") &&
                        HasEvidence(jit_first_result, "native_compile_mode", "jit") &&
                        HasEvidence(jit_first_result, "native_compile_lowerability", "llvm_safe");
  const bool jit_hit = jit_second_result.ok && HasEvidence(jit_second_result, "extension_behavior", "jit_compiled") &&
                       HasEvidence(jit_second_result, "llvm_cache", "hit") &&
                       HasEvidence(jit_second_result, "native_compile_provenance_hash");
  const bool aot_ok = aot_result.ok && HasEvidence(aot_result, "extension_behavior", "aot_compiled") &&
                      HasEvidence(aot_result, "native_compile_mode", "aot") &&
                      HasEvidence(aot_result, "native_compile_artifact_path") &&
                      RowFieldEquals(aot_result, "compile_mode", "aot");

  const bool ok = database_created && security_required && raw_sql_rejected && bad_mode_rejected && unavailable_rejected &&
                  fallback_ok && cluster_denied && invalid_commit && jit_miss && first_commit && jit_hit && aot_ok && second_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("database_created", database_created, true);
  PrintBool("security_required", security_required, true);
  PrintBool("raw_sql_rejected", raw_sql_rejected, true);
  PrintBool("bad_mode_rejected", bad_mode_rejected, true);
  PrintBool("unavailable_rejected", unavailable_rejected, true);
  PrintBool("fallback_ok", fallback_ok, true);
  PrintBool("cluster_denied", cluster_denied, true);
  PrintBool("jit_miss", jit_miss, true);
  PrintBool("jit_hit", jit_hit, true);
  PrintBool("aot_ok", aot_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
