// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/llvm_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "extensibility/extensibility_support.hpp"
#include "native_compile.hpp"
#include "runtime_capabilities.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace native = scratchbird::engine::native_compile;
namespace {

constexpr const char* kOperation = "extensibility.compile_llvm_module";
constexpr const char* kArtifactKind = "llvm_compile_artifact";

bool Contains(const std::string& value, const std::string& token) {
  return value.find(token) != std::string::npos;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool HasOptionToken(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (Contains(option, token)) { return true; }
  }
  return false;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

std::string RequestedMode(const EngineApiRequest& request) {
  std::string mode = OptionValue(request, "compile:");
  if (mode.empty()) { mode = OptionValue(request, "mode:"); }
  if (mode.empty()) { mode = "jit"; }
  if (mode == "llvm_jit") { return "jit"; }
  if (mode == "llvm_aot") { return "aot"; }
  return mode;
}

std::string ModulePayload(const EngineApiRequest& request) {
  std::string module = OptionValue(request, "module:");
  if (module.empty()) { module = OptionValue(request, "sblr_fragment:"); }
  return module;
}

std::string Fnv1aHex(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[hash & 0x0fU];
    hash >>= 4U;
  }
  return out;
}

std::string CacheMaterial(const EngineApiRequest& request, const std::string& mode, const std::string& module) {
  std::string material = "mode=" + mode + ";module=" + module;
  material += ";target=" + request.target_object.uuid.canonical;
  material += ";principal=" + request.context.principal_uuid.canonical;
  material += ";policy_epoch=" + std::to_string(request.context.security_epoch);
  for (const auto& descriptor : request.descriptors) {
    material += ";descriptor=" + descriptor.descriptor_uuid.canonical + ":" +
                descriptor.descriptor_kind + ":" + descriptor.canonical_type_name + ":" +
                descriptor.encoded_descriptor;
  }
  for (const auto& profile : request.policy_profile.encoded_profiles) { material += ";policy=" + profile; }
  for (const auto& profile : request.physical_profile.encoded_profiles) { material += ";physical=" + profile; }
  return material;
}

std::string CacheKey(const EngineApiRequest& request, const std::string& mode, const std::string& module) {
  const auto explicit_key = OptionValue(request, "cache_key:");
  if (!explicit_key.empty()) { return explicit_key; }
  return "llvm-" + Fnv1aHex(CacheMaterial(request, mode, module));
}

struct LlvmCapability {
  bool available = false;
  std::string provider = "missing";
};

LlvmCapability DetectLlvmCapability(const EngineApiRequest& request) {
  LlvmCapability capability;
  capability.available = !HasOptionToken(request, "simulate_llvm_unavailable");
  const auto manifest = scratchbird::core::platform::DetectRuntimeCapabilities();
  for (const auto& candidate : manifest.capabilities) {
    if (candidate.key == "compiler.llvm") {
      capability.available = capability.available &&
                             candidate.state == scratchbird::core::platform::CapabilityState::present;
      capability.provider = candidate.provider;
      return capability;
    }
  }
  capability.available = false;
  return capability;
}

EngineApiDiagnostic LlvmDiagnostic(const std::string& code, const std::string& detail) {
  return MakeEngineApiDiagnostic(code, "engine.extensibility.llvm", detail, true);
}

EngineCompileLlvmModuleResult LlvmFailure(const EngineRequestContext& context,
                                          const std::string& code,
                                          const std::string& detail) {
  return MakeApiBehaviorDiagnostic<EngineCompileLlvmModuleResult>(
      context,
      kOperation,
      LlvmDiagnostic(code, detail));
}

std::string LegacyDiagnosticForNative(const std::string& code) {
  if (code == "NATIVE.LLVM_BACKEND_UNAVAILABLE") { return "SB_ENGINE_API_LLVM_REQUIRED_UNAVAILABLE"; }
  if (code == "NATIVE.SQL_COMPILE_FORBIDDEN") { return "SB_ENGINE_API_LLVM_RAW_SQL_REFUSED"; }
  if (code == "NATIVE.JIT_DISABLED_BY_POLICY" || code == "NATIVE.AOT_DISABLED_BY_POLICY" ||
      code == "NATIVE.INVALID_COMPILE_POLICY") {
    return "SB_ENGINE_API_LLVM_MODE_UNSUPPORTED";
  }
  if (code == "NATIVE.CACHE_KEY_INCOMPLETE") { return "SB_ENGINE_API_LLVM_CACHE_KEY_INCOMPLETE"; }
  if (code == "NATIVE.AOT_ARTIFACT_INVALID") { return "SB_ENGINE_API_LLVM_AOT_ARTIFACT_INVALID"; }
  return "SB_ENGINE_API_LLVM_COMPILE_REFUSED";
}

EngineCompileLlvmModuleResult NativeLlvmFailure(const EngineRequestContext& context,
                                                const native::NativeCompileResult& native_result) {
  auto result = LlvmFailure(context,
                            LegacyDiagnosticForNative(native_result.diagnostic_code),
                            native_result.diagnostic_detail.empty() ? native_result.diagnostic_code : native_result.diagnostic_detail);
  result.diagnostics.push_back(LlvmDiagnostic(native_result.diagnostic_code,
                                              native_result.diagnostic_detail.empty() ? "native_compile_refused" : native_result.diagnostic_detail));
  AddApiBehaviorEvidence(&result, "native_compile_diagnostic", native_result.diagnostic_code);
  AddApiBehaviorEvidence(&result, "native_compile_policy", native::NativeCompilePolicyProfileName(native_result.policy_profile));
  AddApiBehaviorEvidence(&result, "llvm_provider", native_result.backend_provider);
  return result;
}

bool AllowsFallback(const EngineApiRequest& request) {
  return HasOptionToken(request, "allow_interpreter_fallback") ||
         HasOptionToken(request, "fallback:interpreter") ||
         HasOptionToken(request, "fallback_allowed:true");
}

bool RequestsRawSql(const EngineApiRequest& request, const std::string& module) {
  return HasOptionToken(request, "raw_sql") ||
         HasOptionToken(request, "sql_text:") ||
         StartsWith(module, "sql:") ||
         Contains(module, "SELECT ") ||
         Contains(module, "INSERT ") ||
         Contains(module, "UPDATE ") ||
         Contains(module, "DELETE ");
}

bool RequestsAuthorityBypass(const EngineApiRequest& request) {
  return HasOptionToken(request, "bypass_mga") ||
         HasOptionToken(request, "bypass_sblr") ||
         HasOptionToken(request, "bypass_catalog") ||
         HasOptionToken(request, "bypass_uuid_catalog") ||
         HasOptionToken(request, "bypass_security") ||
         HasOptionToken(request, "bypass_transaction") ||
         HasOptionToken(request, "direct_storage") ||
         HasOptionToken(request, "direct_catalog_mutation") ||
         HasOptionToken(request, "raw_page") ||
         HasOptionToken(request, "wal_authority");
}

bool ModuleIsSblr(const EngineApiRequest& request, const std::string& module) {
  return HasOptionToken(request, "sblr_fragment") ||
         Contains(module, "sblr") ||
         Contains(module, "SBLR");
}

native::NativeCompileRequest BuildNativeCompileRequest(const EngineCompileLlvmModuleRequest& request,
                                                       const std::string& mode,
                                                       const std::string& module,
                                                       bool previously_cached) {
  native::NativeCompileRequest native_request;
  native_request.requested_mode = mode == "aot" ? native::NativeCompileMode::aot : native::NativeCompileMode::jit;
  native_request.module_payload = module;
  native_request.target_object_uuid = request.target_object.uuid.canonical;
  native_request.principal_uuid = request.context.principal_uuid.canonical;
  native_request.database_path = request.context.database_path;
  native_request.catalog_generation_id = request.context.catalog_generation_id;
  native_request.security_epoch = request.context.security_epoch;
  native_request.policy_epoch = request.context.security_epoch;
  native_request.resource_epoch = request.context.resource_epoch;
  native_request.security_context_present = request.context.security_context_present;
  native_request.allow_interpreter_fallback = AllowsFallback(request);
  native_request.simulate_backend_unavailable = HasOptionToken(request, "simulate_llvm_unavailable");
  native_request.previously_cached = previously_cached;
  native_request.memory_accounting.explicit_test_fixture =
      HasOptionToken(request, "llvm_test_fixture") ||
      HasOptionToken(request, "test_fixture:llvm");
  native_request.memory_accounting.production_like =
      !native_request.memory_accounting.explicit_test_fixture;
  native_request.memory_accounting.operation_id =
      mode == "aot" ? "engine_api.native_compile.aot"
                    : "engine_api.native_compile.jit";
  native_request.memory_accounting.native_callsite =
      "engine.internal_api.extensibility.llvm";
  native_request.memory_accounting.evidence.push_back(
      "engine_api.llvm.compile_memory_accounting=ceic_061");
  native_request.policy_profiles = request.policy_profile.encoded_profiles;
  native_request.physical_profiles = request.physical_profile.encoded_profiles;
  native_request.option_envelopes = request.option_envelopes;
  for (const auto& descriptor : request.descriptors) {
    native_request.descriptors.push_back({descriptor.descriptor_uuid.canonical,
                                          descriptor.descriptor_kind,
                                          descriptor.canonical_type_name,
                                          descriptor.encoded_descriptor});
  }
  return native_request;
}

bool CacheHit(const EngineRequestContext& context, const std::string& key) {
  for (const auto& record : VisibleApiBehaviorRecords(context, kArtifactKind, context.local_transaction_id)) {
    if (record.default_name == key && !record.deleted) { return true; }
  }
  return false;
}

void AddLlvmEvidence(EngineApiResult* result,
                     const std::string& mode,
                     const std::string& cache_state,
                     const std::string& provider) {
  AddApiBehaviorEvidence(result, "llvm_compile_runtime", mode);
  AddApiBehaviorEvidence(result, "llvm_compile_contract", "validated_request_shape");
  AddApiBehaviorEvidence(result, "llvm_cache", cache_state);
  AddApiBehaviorEvidence(result, "llvm_provider", provider.empty() ? "missing" : provider);
  AddApiBehaviorEvidence(result, "llvm_metrics", "compile_event_emitted");
  AddApiBehaviorEvidence(result, "execution_boundary", "sblr_only_engine_authority");
  AddEngineExtensionEvidence(result, "llvm", mode == "aot" ? "aot_compiled" : (mode == "jit" ? "jit_compiled" : "interpreter_fallback"));
}

void AddNativeCompileEvidence(EngineApiResult* result,
                              const native::NativeCompileResult& native_result) {
  // SEARCH_KEY: OEIC_LLVM_AUTHORITY_EVIDENCE
  AddApiBehaviorEvidence(result, "native_compile_mode", native::NativeCompileEffectiveModeName(native_result.effective_mode));
  AddApiBehaviorEvidence(result, "native_compile_policy", native::NativeCompilePolicyProfileName(native_result.policy_profile));
  AddApiBehaviorEvidence(result, "native_compile_lowerability", native_result.lowerability);
  AddApiBehaviorEvidence(result, "native_compile_unit_kind", native_result.unit_kind);
  AddApiBehaviorEvidence(result, "native_compile_llvm_load_mode", native_result.llvm_load_mode);
  AddApiBehaviorEvidence(result, "native_compile_llvm_library_path_digest", native_result.llvm_library_path_digest);
  AddApiBehaviorEvidence(result, "native_compile_llvm_source_root_digest", native_result.llvm_source_root_digest);
  AddApiBehaviorEvidence(result, "native_compile_llvm_tools_root_digest", native_result.llvm_tools_root_digest);
  AddApiBehaviorEvidence(result, "native_compile_llvm_staging_build_dir_digest", native_result.llvm_staging_build_dir_digest);
  AddApiBehaviorEvidence(result, "native_compile_llvm_memory_accounting_required", native_result.llvm_memory_accounting_required ? "true" : "false");
  AddApiBehaviorEvidence(result, "native_compile_llvm_memory_reserved", native_result.llvm_memory_reserved ? "true" : "false");
  AddApiBehaviorEvidence(result, "native_compile_llvm_memory_released", native_result.llvm_memory_released ? "true" : "false");
  AddApiBehaviorEvidence(result, "native_compile_llvm_memory_reserved_bytes", std::to_string(native_result.llvm_memory_reserved_bytes));
  AddApiBehaviorEvidence(result, "native_compile_llvm_memory_reservation_count", std::to_string(native_result.llvm_memory_reservation_count));
  AddApiBehaviorEvidence(result, "native_compile_target_triple", native_result.target_triple);
  AddApiBehaviorEvidence(result, "native_compile_target_feature_set", native_result.target_feature_set);
  AddApiBehaviorEvidence(result, "native_compile_sblr_or_ir_digest", native_result.sblr_or_ir_digest);
  AddApiBehaviorEvidence(result, "native_compile_descriptor_set_digest", native_result.descriptor_set_digest);
  AddApiBehaviorEvidence(result, "native_compile_cache_key_material_digest", native_result.cache_key_material_digest);
  AddApiBehaviorEvidence(result, "native_compile_provenance_hash", native_result.provenance_hash);
  if (!native_result.artifact_digest.empty()) {
    AddApiBehaviorEvidence(result, "native_compile_artifact_digest", native_result.artifact_digest);
  }
  if (!native_result.artifact_path.empty()) {
    AddApiBehaviorEvidence(result, "native_compile_artifact_path", native_result.artifact_path);
  }
  if (!native_result.diagnostic_code.empty()) {
    AddApiBehaviorEvidence(result, "native_compile_diagnostic", native_result.diagnostic_code);
  }
}

std::string OperationIdOr(const EngineApiRequest& request, const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

std::string ResultShapeContract(const EngineApiRequest& request, const std::string& fallback) {
  const auto result_shape = OptionValue(request, "result_shape_contract:");
  return result_shape.empty() ? fallback : result_shape;
}

void AddNativeCompileOperationResult(EngineApiResult* result,
                                     const EngineApiRequest& request,
                                     const std::string& operation_id,
                                     const std::string& api_function,
                                     const std::string& route_kind,
                                     const std::string& result_shape) {
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", api_function);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  AddApiBehaviorRow(result,
                    {{"operation_id", operation_id},
                     {"result_shape", result_shape},
                     {"route_kind", route_kind},
                     {"target_ref_kind", OptionValue(request, "target_ref_kind:")},
                     {"target_ref_visible", OptionValue(request, "target_ref:").empty() ? "false" : "true"},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)}});
  result->result_shape.result_kind = result_shape;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSIBILITY_LLVM_API_BEHAVIOR
EngineCompileLlvmModuleResult EngineCompileLlvmModule(const EngineCompileLlvmModuleRequest& request) {
  if (!request.context.security_context_present) {
    return EngineExtensionSecurityRequired<EngineCompileLlvmModuleResult>(request, kOperation);
  }
  if (!request.context.cluster_authority_available && EngineExtensionRequestsClusterAuthority(request)) {
    return EngineExtensionClusterAuthorityUnavailable<EngineCompileLlvmModuleResult>(request, kOperation);
  }
  if (RequestsAuthorityBypass(request)) {
    return LlvmFailure(
        request.context,
        "SB_ENGINE_API_LLVM_AUTHORITY_BYPASS_REFUSED",
        "llvm_compile_cannot_bypass_sblr_catalog_security_transaction_or_mga_authority");
  }
  const auto mode = RequestedMode(request);
  if (mode != "jit" && mode != "aot") {
    return LlvmFailure(request.context, "SB_ENGINE_API_LLVM_MODE_UNSUPPORTED", "compile_mode_must_be_jit_or_aot");
  }
  const auto module = ModulePayload(request);
  if (module.empty()) {
    return LlvmFailure(request.context, "SB_ENGINE_API_LLVM_MODULE_REQUIRED", "sblr_module_payload_required");
  }
  if (RequestsRawSql(request, module)) {
    return LlvmFailure(request.context, "SB_ENGINE_API_LLVM_RAW_SQL_REFUSED", "llvm_compile_accepts_sblr_not_sql");
  }
  if (!ModuleIsSblr(request, module)) {
    return LlvmFailure(request.context, "SB_ENGINE_API_LLVM_SBLR_REQUIRED", "llvm_compile_accepts_sblr_fragments_only");
  }

  const auto requested_cache_key = CacheKey(request, mode, module);
  const bool prior_hit = CacheHit(request.context, requested_cache_key);
  const auto native_result = native::CompileNativeUnit(BuildNativeCompileRequest(request, mode, module, prior_hit));
  if (!native_result.ok) {
    return NativeLlvmFailure(request.context, native_result);
  }
  const std::string effective_mode = native::NativeCompileEffectiveModeName(native_result.effective_mode);
  const auto cache_key = OptionValue(request, "cache_key:").empty() && !native_result.cache_key.empty()
      ? native_result.cache_key
      : requested_cache_key;

  EngineCompileLlvmModuleRequest persisted_request = request;
  persisted_request.localized_names.clear();
  persisted_request.localized_names.push_back({"en", "default", "/sys/llvm/cache", cache_key, true});
  persisted_request.option_envelopes.push_back("cache_key:" + cache_key);
  persisted_request.option_envelopes.push_back("effective_mode:" + effective_mode);
  persisted_request.option_envelopes.push_back("llvm_provider:" + native_result.backend_provider);
  persisted_request.option_envelopes.push_back("native_provenance:" + native_result.provenance_hash);
  auto result = PersistedRecordResult<EngineCompileLlvmModuleResult>(
      persisted_request,
      kOperation,
      kArtifactKind,
      request.context.local_transaction_id != 0,
      prior_hit ? "cache_hit" : "cache_miss");
  if (!result.ok) { return result; }

  AddApiBehaviorRow(&result, {{"compile_mode", effective_mode},
                              {"requested_mode", mode},
                              {"module_payload", module},
                              {"llvm_library", native_result.backend_provider},
                              {"llvm_verifier_available", native_result.verifier_available ? "true" : "false"},
                              {"native_policy", native::NativeCompilePolicyProfileName(native_result.policy_profile)},
                              {"native_lowerability", native_result.lowerability},
                              {"native_unit_kind", native_result.unit_kind},
                              {"engine_execution_authority", "sblr_only"},
                              {"raw_sql_execution", "false"},
                              {"module_compiled", native_result.compiled ? "true" : "false"},
                              {"interpreter_fallback", effective_mode == "interpreter" ? "true" : "false"},
                              {"cache_key", cache_key},
                              {"cache_state", prior_hit ? "hit" : "miss"},
                              {"native_provenance_hash", native_result.provenance_hash},
                              {"native_artifact_path", native_result.artifact_path},
                              {"compiled_symbol", "sb_native_" + cache_key}});
  AddLlvmEvidence(&result, effective_mode, prior_hit ? "hit" : "miss", native_result.backend_provider);
  AddNativeCompileEvidence(&result, native_result);
  return result;
}

EngineControlNativeCompileResult EngineControlNativeCompile(
    const EngineControlNativeCompileRequest& request) {
  const std::string operation_id = OperationIdOr(request, "native_compile.control");
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineControlNativeCompileResult>(
        request.context,
        operation_id,
        LlvmDiagnostic("SB_ENGINE_API_NATIVE_COMPILE_SECURITY_CONTEXT_REQUIRED",
                       "native_compile_control_requires_security_context"));
  }
  if (!request.context.cluster_authority_available &&
      EngineExtensionRequestsClusterAuthority(request)) {
    return MakeApiBehaviorDiagnostic<EngineControlNativeCompileResult>(
        request.context,
        operation_id,
        LlvmDiagnostic("SB_ENGINE_API_NATIVE_COMPILE_CLUSTER_AUTHORITY_UNAVAILABLE",
                       "native_compile_control_does_not_use_private_cluster_dispatch"));
  }
  auto result =
      MakeApiBehaviorSuccess<EngineControlNativeCompileResult>(request.context, operation_id);
  AddNativeCompileOperationResult(&result,
                                  request,
                                  operation_id,
                                  "EngineControlNativeCompile",
                                  "native_compile_control",
                                  ResultShapeContract(request, "rs.acceleration.control.v1"));
  return result;
}

EngineInspectNativeCompileResult EngineInspectNativeCompile(
    const EngineInspectNativeCompileRequest& request) {
  const std::string operation_id = OperationIdOr(request, "native_compile.inspect");
  auto result =
      MakeApiBehaviorSuccess<EngineInspectNativeCompileResult>(request.context, operation_id);
  const auto fallback_shape =
      operation_id == "op.show.aot_artifacts" ||
              operation_id == "op.show.native_compile" ||
              operation_id == "op.show.native_compile_cache"
          ? "rs.show.native_compile.v1"
          : "rs.show.llvm.v1";
  AddNativeCompileOperationResult(&result,
                                  request,
                                  operation_id,
                                  "EngineInspectNativeCompile",
                                  "native_compile_inspect",
                                  ResultShapeContract(request, fallback_shape));
  return result;
}

}  // namespace scratchbird::engine::internal_api
