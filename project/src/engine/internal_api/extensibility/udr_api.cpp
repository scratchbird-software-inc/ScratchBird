// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/udr_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "extensibility/extensibility_support.hpp"
#include "metric_registry.hpp"
#include "sb_udr_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace udr_runtime = scratchbird::udr::runtime;

constexpr const char* kUdrKind = "udr_package";
constexpr const char* kRegisterOperation = "extensibility.register_udr_package";
constexpr const char* kLoadOperation = "extensibility.load_udr_package";
constexpr const char* kUnloadOperation = "extensibility.unload_udr_package";
constexpr const char* kInspectOperation = "extensibility.inspect_udr_packages";
constexpr const char* kInvokeOperation = "extensibility.invoke_udr_package";
constexpr const char* kMetricProducer = "udr_runtime";

bool Contains(const std::string& value, const std::string& token) {
  return value.find(token) != std::string::npos;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

void ReplaceAll(std::string* value, const std::string& needle, const std::string& replacement) {
  if (value == nullptr || needle.empty()) return;
  std::size_t pos = 0;
  while ((pos = value->find(needle, pos)) != std::string::npos) {
    value->replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
}

bool HasOptionToken(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (Contains(option, token)) { return true; }
  }
  return false;
}

bool HasOptionPrefix(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return true; }
  }
  return false;
}

std::vector<std::string> OptionValues(const EngineApiRequest& request, const std::string& prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { values.push_back(option.substr(prefix.size())); }
  }
  return values;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

std::uint64_t U64Option(const EngineApiRequest& request, const std::string& prefix, std::uint64_t fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  char* end = nullptr;
  const auto parsed = std::strtoull(value.c_str(), &end, 10);
  return end == value.c_str() ? fallback : static_cast<std::uint64_t>(parsed);
}

bool HasUdrManageRight(const EngineApiRequest& request) {
  return HasOptionToken(request, "permission:manage_udr") ||
         HasOptionToken(request, "permission:udr_manage") ||
         HasOptionToken(request, "right:UDR_MANAGE") ||
         HasOptionToken(request, "right:MANAGE_UDR") ||
         HasOptionToken(request, "grant:MANAGE_UDR") ||
         HasOptionToken(request, "role:DBA") ||
         HasOptionToken(request, "role:SEC") ||
         HasOptionToken(request, "udr_admin");
}

bool HasUdrInspectRight(const EngineApiRequest& request) {
  return HasUdrManageRight(request) ||
         HasOptionToken(request, "permission:inspect_udr") ||
         HasOptionToken(request, "right:UDR_INSPECT") ||
         HasOptionToken(request, "grant:INSPECT_UDR");
}

bool HasUdrInvokeRight(const EngineApiRequest& request) {
  return HasUdrManageRight(request) ||
         HasOptionToken(request, "permission:invoke_udr") ||
         HasOptionToken(request, "permission:udr_invoke") ||
         HasOptionToken(request, "right:UDR_INVOKE") ||
         HasOptionToken(request, "grant:INVOKE_UDR");
}

bool HasTrustedUdrProfile(const EngineApiRequest& request) {
  if (HasOptionToken(request, "untrusted") || HasOptionToken(request, "parser_executable")) { return false; }
  return HasOptionToken(request, "trust:trusted_cpp") ||
         HasOptionToken(request, "trust:engine_trusted") ||
         HasOptionToken(request, "trusted_cpp_udr") ||
         HasOptionToken(request, "register:trusted_cpp_udr");
}

bool IsTrustedCppRuntimeLanguage(const std::string& runtime_language) {
  return runtime_language.empty() ||
         runtime_language == "cpp" ||
         runtime_language == "c++" ||
         runtime_language == "cxx" ||
         runtime_language == "trusted_cpp";
}

std::string RequestedRuntimeLanguage(const EngineApiRequest& request) {
  for (const auto& prefix :
       {"runtime_language:", "udr_runtime:", "runtime:", "language:"}) {
    const auto value = OptionValue(request, prefix);
    if (!value.empty()) { return value; }
  }
  if (HasOptionToken(request, "non_cpp_udr")) { return "non_cpp"; }
  return {};
}

bool RequestsNonCppRuntime(const EngineApiRequest& request,
                           std::string* runtime_language) {
  auto requested = RequestedRuntimeLanguage(request);
  if (runtime_language != nullptr) {
    *runtime_language = requested.empty() ? "unspecified" : requested;
  }
  return !requested.empty() && !IsTrustedCppRuntimeLanguage(requested);
}

bool IsSupportedUdrAbiToken(const std::string& abi) {
  return abi == "1" ||
         abi == "sb_udr_1" ||
         abi == "sb_udr_v1" ||
         abi == "scratchbird_udr_1" ||
         abi == "stable-1";
}

bool HasSupportedUdrAbi(const EngineApiRequest& request, const ApiBehaviorRecord* record = nullptr) {
  for (const auto& abi : OptionValues(request, "abi:")) {
    if (IsSupportedUdrAbiToken(abi)) { return true; }
  }
  if (record != nullptr) {
    return Contains(record->payload, "abi:1") ||
           Contains(record->payload, "abi:sb_udr_1") ||
           Contains(record->payload, "abi:sb_udr_v1") ||
           Contains(record->payload, "abi:scratchbird_udr_1") ||
           Contains(record->payload, "abi:stable-1");
  }
  return false;
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

bool HasSblrInvocationAuthority(const EngineApiRequest& request) {
  return HasOptionToken(request, "sblr_authorized_invocation:true") ||
         HasOptionToken(request, "sblr_opcode:SBLR_UDR_INVOKE") ||
         HasOptionToken(request, "operation_family:sblr.udr.operation.v3");
}

bool ShutdownDrainActive(const EngineApiRequest& request) {
  return HasOptionToken(request, "shutdown_draining:true") ||
         HasOptionToken(request, "database_shutdown_in_progress:true") ||
         HasOptionToken(request, "shutdown_generation:");
}

std::string RequestedEntrypoint(const EngineApiRequest& request) {
  auto entrypoint = OptionValue(request, "entrypoint:");
  if (entrypoint.empty()) { entrypoint = OptionValue(request, "udr_entrypoint:"); }
  return entrypoint.empty() ? "default" : entrypoint;
}

std::string RequestedInvocationPayload(const EngineApiRequest& request) {
  auto payload = OptionValue(request, "payload:");
  if (payload.empty()) { payload = OptionValue(request, "argument_payload:"); }
  return payload;
}

std::string RequestedContextPacket(const EngineApiRequest& request) {
  auto context = OptionValue(request, "context_packet:");
  if (context.empty()) { context = OptionValue(request, "udr_context:"); }
  return context;
}

std::string EngineVisibleUdrPayload(const std::string& runtime_payload,
                                    const std::string& invocation_payload) {
  std::string visible = runtime_payload;
  if (!Contains(visible, "SBLRExecutionEnvelope.v3")) return visible;
  ReplaceAll(&visible, "sql_text", "source_text");
  if (invocation_payload.size() >= 4) {
    ReplaceAll(&visible, invocation_payload, "[redacted_source_text]");
  }
  return visible;
}

bool ResourceBudgetExceeded(const EngineApiRequest& request, std::string* detail) {
  const auto memory_budget = U64Option(request, "memory_budget_bytes:", 1024U * 1024U);
  const auto cpu_budget = U64Option(request, "cpu_budget_microseconds:", 100000U);
  const auto payload_size = static_cast<std::uint64_t>(RequestedInvocationPayload(request).size());
  if (memory_budget == 0 || memory_budget > 64U * 1024U * 1024U) {
    if (detail != nullptr) { *detail = "invalid_memory_budget_bytes"; }
    return true;
  }
  if (cpu_budget == 0 || cpu_budget > 30000000U) {
    if (detail != nullptr) { *detail = "invalid_cpu_budget_microseconds"; }
    return true;
  }
  if (payload_size > memory_budget) {
    if (detail != nullptr) { *detail = "payload_exceeds_memory_budget"; }
    return true;
  }
  return false;
}

std::vector<std::string> DependencyUuids(const EngineApiRequest& request) {
  std::vector<std::string> dependencies = OptionValues(request, "dependency:");
  for (const auto& related : request.related_objects) {
    if (!related.uuid.canonical.empty()) { dependencies.push_back(related.uuid.canonical); }
  }
  return dependencies;
}

EngineApiDiagnostic UdrDiagnostic(const std::string& operation_id,
                                  const std::string& code,
                                  const std::string& detail) {
  return MakeEngineApiDiagnostic(code, "engine.extensibility.udr", detail, true);
}

std::string JoinEntrypoints(const std::vector<std::string>& entrypoints) {
  std::string out;
  for (const auto& entrypoint : entrypoints) {
    if (!out.empty()) out += ",";
    out += entrypoint;
  }
  return out;
}

std::string RuntimeFailureCode(const std::string& runtime_code,
                               const std::string& fallback) {
  if (runtime_code == "UDR.UNLOAD_BLOCKED") return "SB_ENGINE_API_UDR_UNLOAD_BLOCKED";
  if (runtime_code == "UDR.RUNTIME.PACKAGE_NOT_REGISTERED") {
    return "SB_ENGINE_API_UDR_RUNTIME_DESCRIPTOR_REQUIRED";
  }
  if (runtime_code == "UDR.RUNTIME.PACKAGE_NOT_LOADED") {
    return "SB_ENGINE_API_UDR_NOT_LOADED";
  }
  if (runtime_code == "UDR.RUNTIME.ENTRYPOINT_NOT_FOUND") {
    return "SB_ENGINE_API_UDR_ENTRYPOINT_NOT_FOUND";
  }
  if (runtime_code == "UDR.RUNTIME.PACKAGE_DESCRIPTOR_CONFLICT") {
    return "SB_ENGINE_API_UDR_DESCRIPTOR_CONFLICT";
  }
  if (runtime_code == "UDR.RUNTIME.ABI_UNSUPPORTED") {
    return "SB_ENGINE_API_UDR_ABI_UNSUPPORTED";
  }
  if (runtime_code == "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN") {
    return "SB_ENGINE_API_UDR_NON_CPP_RUNTIME_FORBIDDEN";
  }
  if (runtime_code == "UDR.RUNTIME.PROVENANCE_REQUIRED") {
    return "SB_ENGINE_API_UDR_PROVENANCE_REQUIRED";
  }
  return fallback;
}

std::string RuntimeCallFailureCode(const udr_runtime::UdrCallResult& call_result) {
  if (Contains(call_result.message_vector_json, "UDR.RUNTIME.ENTRYPOINT_NOT_FOUND")) {
    return "SB_ENGINE_API_UDR_ENTRYPOINT_NOT_FOUND";
  }
  if (Contains(call_result.message_vector_json, "UDR.RUNTIME.PACKAGE_NOT_LOADED")) {
    return "SB_ENGINE_API_UDR_NOT_LOADED";
  }
  if (Contains(call_result.message_vector_json, "UDR.RUNTIME.PACKAGE_NOT_REGISTERED")) {
    return "SB_ENGINE_API_UDR_RUNTIME_DESCRIPTOR_REQUIRED";
  }
  return "SB_ENGINE_API_UDR_ENTRYPOINT_FAILED";
}

bool DescriptorOptionMatches(const EngineApiRequest& request,
                             const std::string& prefix,
                             const std::string& expected) {
  const auto value = OptionValue(request, prefix);
  return !value.empty() && value == expected;
}

template <typename TResult>
TResult MakeUdrFailure(const EngineRequestContext& context,
                       const std::string& operation_id,
                       const std::string& code,
                       const std::string& detail) {
  return MakeApiBehaviorDiagnostic<TResult>(context, operation_id, UdrDiagnostic(operation_id, code, detail));
}

template <typename TResult>
TResult RuntimeStatusFailure(const EngineRequestContext& context,
                             const std::string& operation_id,
                             const udr_runtime::UdrStatus& status,
                             const std::string& fallback_code) {
  return MakeUdrFailure<TResult>(
      context,
      operation_id,
      RuntimeFailureCode(status.diagnostic_code, fallback_code),
      status.detail.empty() ? status.diagnostic_code : status.detail);
}

template <typename TResult>
TResult ValidateRuntimeDescriptorProvenance(
    const EngineApiRequest& request,
    const std::string& operation_id,
    const udr_runtime::UdrPackageDescriptor& descriptor) {
  if (!HasOptionToken(request, "linked_udr_package:true")) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_RUNTIME_DESCRIPTOR_REQUIRED",
        "linked_udr_package_descriptor_required");
  }
  if (!DescriptorOptionMatches(request, "source_revision:", descriptor.source_revision) ||
      !DescriptorOptionMatches(request, "binary_hash:", descriptor.binary_hash) ||
      !DescriptorOptionMatches(request, "signature_policy:", descriptor.signature_policy) ||
      !DescriptorOptionMatches(request, "capability_role:", descriptor.capability_role)) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_DESCRIPTOR_MISMATCH",
        "udr_descriptor_provenance_or_capability_mismatch");
  }
  return MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
}

template <typename TResult>
TResult ValidateUdrAuthority(const EngineApiRequest& request,
                             const std::string& operation_id,
                             bool require_manage,
                             bool require_inspect,
                             bool require_invoke = false,
                             bool allow_shutdown_cleanup = false) {
  if (!request.context.security_context_present) {
    return EngineExtensionSecurityRequired<TResult>(request, operation_id);
  }
  if (!request.context.cluster_authority_available && EngineExtensionRequestsClusterAuthority(request)) {
    return EngineExtensionClusterAuthorityUnavailable<TResult>(request, operation_id);
  }
  if (ShutdownDrainActive(request) && !allow_shutdown_cleanup) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_SHUTDOWN_DRAIN_ACTIVE",
        "udr_register_load_or_invoke_refused_during_shutdown_drain");
  }
  if (RequestsAuthorityBypass(request)) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_AUTHORITY_BYPASS_REFUSED",
        "udr_package_cannot_bypass_mga_sblr_catalog_security_or_transaction_authority");
  }
  if (require_manage && !HasUdrManageRight(request)) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_PERMISSION_REQUIRED",
        "manage_udr_permission_required");
  }
  if (require_inspect && !HasUdrInspectRight(request)) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_PERMISSION_REQUIRED",
        "inspect_udr_permission_required");
  }
  if (require_invoke && !HasUdrInvokeRight(request)) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_PERMISSION_REQUIRED",
        "invoke_udr_permission_required");
  }
  return MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
}

template <typename TResult>
TResult RequireTargetUuid(const EngineApiRequest& request, const std::string& operation_id) {
  if (request.target_object.uuid.canonical.empty()) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_TARGET_REQUIRED",
        "target_udr_package_uuid_required");
  }
  return MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
}

template <typename TResult>
TResult RequireVisibleUdr(const EngineApiRequest& request,
                          const std::string& operation_id,
                          ApiBehaviorRecord* out_record) {
  const auto target = RequireTargetUuid<TResult>(request, operation_id);
  if (!target.ok) { return target; }
  const auto record = FindVisibleApiBehaviorRecord(
      request.context,
      request.target_object.uuid.canonical,
      request.context.local_transaction_id);
  if (!record || record->object_kind != kUdrKind) {
    return MakeUdrFailure<TResult>(
        request.context,
        operation_id,
        "SB_ENGINE_API_UDR_NOT_FOUND",
        "target_udr_package_not_registered");
  }
  *out_record = *record;
  return MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
}

template <typename TResult>
TResult ValidateDependencies(const EngineApiRequest& request, const std::string& operation_id) {
  for (const auto& dependency_uuid : DependencyUuids(request)) {
    const auto dependency = FindVisibleApiBehaviorRecord(
        request.context,
        dependency_uuid,
        request.context.local_transaction_id);
    if (!dependency || dependency->object_kind != kUdrKind || dependency->state == "failed" || dependency->state == "unloaded") {
      return MakeUdrFailure<TResult>(
          request.context,
          operation_id,
          "SB_ENGINE_API_UDR_DEPENDENCY_MISSING",
          "dependency_not_registered_or_loaded:" + dependency_uuid);
    }
  }
  return MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
}

void AddUdrLifecycleEvidence(EngineApiResult* result,
                             const std::string& lifecycle_state,
                             const std::string& abi_state,
                             const std::string& audit_state) {
  AddEngineExtensionEvidence(result, "udr", lifecycle_state);
  AddApiBehaviorEvidence(result, "security_scope", "permission_checked");
  AddApiBehaviorEvidence(result, "udr_abi", abi_state);
  AddApiBehaviorEvidence(result, "udr_permission", "manage_udr_checked");
  AddApiBehaviorEvidence(result, "udr_audit", audit_state);
  AddApiBehaviorEvidence(result, "udr_metrics", "lifecycle_event_emitted");
  AddApiBehaviorEvidence(result, "execution_boundary", "engine_owned_no_bypass");
  AddApiBehaviorEvidence(result, "authority_boundary", "mga_sblr_uuid_security_transaction_preserved");
}

void EmitUdrMetric(const std::string& family,
                   const EngineApiRequest& request,
                   const std::string& result,
                   const std::string& action,
                   const std::string& reason = {}) {
  auto& registry = scratchbird::core::metrics::DefaultMetricRegistry();
  (void)registry.IncrementCounter(family,
                                  {{"object_uuid", request.target_object.uuid.canonical.empty() ? "none" : request.target_object.uuid.canonical},
                                   {"action", action},
                                   {"result", result},
                                   {"reason", reason.empty() ? "none" : reason}},
                                  1.0,
                                  kMetricProducer);
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSIBILITY_UDR_API_BEHAVIOR
EngineRegisterUdrPackageResult EngineRegisterUdrPackage(const EngineRegisterUdrPackageRequest& request) {
  auto authority = ValidateUdrAuthority<EngineRegisterUdrPackageResult>(
      request,
      kRegisterOperation,
      true,
      false);
  if (!authority.ok) { return authority; }
  std::string requested_runtime_language;
  if (RequestsNonCppRuntime(request, &requested_runtime_language)) {
    EmitUdrMetric("sb_udr_non_cpp_refusal_total", request, "refused", "register", requested_runtime_language);
    return MakeUdrFailure<EngineRegisterUdrPackageResult>(
        request.context,
        kRegisterOperation,
        "SB_ENGINE_API_UDR_NON_CPP_RUNTIME_FORBIDDEN",
        "trusted_cpp_udr_runtime_required:" + requested_runtime_language);
  }
  if (!HasTrustedUdrProfile(request)) {
    EmitUdrMetric("sb_udr_registration_total", request, "refused", "register", "trust_required");
    return MakeUdrFailure<EngineRegisterUdrPackageResult>(
        request.context,
        kRegisterOperation,
        "SB_ENGINE_API_UDR_TRUST_REQUIRED",
        "trusted_cpp_udr_profile_required");
  }
  if (!HasSupportedUdrAbi(request)) {
    EmitUdrMetric("sb_udr_registration_total", request, "refused", "register", "abi_unsupported");
    return MakeUdrFailure<EngineRegisterUdrPackageResult>(
        request.context,
        kRegisterOperation,
        "SB_ENGINE_API_UDR_ABI_UNSUPPORTED",
        "supported_udr_abi_required");
  }
  const auto context_status =
      ValidateApiBehaviorContext(request.context, kRegisterOperation, true, true);
  if (context_status.error) {
    return MakeApiBehaviorDiagnostic<EngineRegisterUdrPackageResult>(
        request.context,
        kRegisterOperation,
        context_status);
  }
  auto target = RequireTargetUuid<EngineRegisterUdrPackageResult>(request, kRegisterOperation);
  if (!target.ok) { return target; }
  if (!request.target_object.uuid.canonical.empty()) {
    const auto existing = FindVisibleApiBehaviorRecord(
        request.context,
        request.target_object.uuid.canonical,
        request.context.local_transaction_id);
    if (existing && existing->object_kind == kUdrKind) {
      return MakeUdrFailure<EngineRegisterUdrPackageResult>(
          request.context,
          kRegisterOperation,
          "SB_ENGINE_API_UDR_ALREADY_REGISTERED",
          "target_udr_package_already_registered");
    }
  }
  const auto descriptor = udr_runtime::FindPackageDescriptor(request.target_object.uuid.canonical);
  if (!descriptor) {
    EmitUdrMetric("sb_udr_registration_total", request, "refused", "register", "runtime_descriptor_required");
    return MakeUdrFailure<EngineRegisterUdrPackageResult>(
        request.context,
        kRegisterOperation,
        "SB_ENGINE_API_UDR_RUNTIME_DESCRIPTOR_REQUIRED",
        "registered_udr_package_must_have_linked_runtime_descriptor");
  }
  const auto provenance =
      ValidateRuntimeDescriptorProvenance<EngineRegisterUdrPackageResult>(
          request, kRegisterOperation, *descriptor);
  if (!provenance.ok) {
    EmitUdrMetric("sb_udr_registration_total", request, "refused", "register", "descriptor_mismatch");
    return provenance;
  }
  const auto runtime_registered = udr_runtime::RegisterPackage(*descriptor);
  if (!runtime_registered.ok) {
    EmitUdrMetric("sb_udr_registration_total", request, "refused", "register", runtime_registered.diagnostic_code);
    return RuntimeStatusFailure<EngineRegisterUdrPackageResult>(
        request.context,
        kRegisterOperation,
        runtime_registered,
        "SB_ENGINE_API_UDR_RUNTIME_DESCRIPTOR_REQUIRED");
  }
  auto dependencies = ValidateDependencies<EngineRegisterUdrPackageResult>(request, kRegisterOperation);
  if (!dependencies.ok) { return dependencies; }
  auto result = PersistedRecordResult<EngineRegisterUdrPackageResult>(
      request,
      kRegisterOperation,
      kUdrKind,
      true,
      "registered");
  if (result.ok) {
    AddUdrLifecycleEvidence(&result, "registered", "supported", "registration_evidence_recorded");
    AddApiBehaviorEvidence(&result, "udr_descriptor", "runtime_descriptor_validated");
    AddApiBehaviorEvidence(&result, "udr_provenance", "source_hash_signature_capability_checked");
    AddApiBehaviorRow(&result, {{"object_uuid", descriptor->package_uuid},
                                {"package_name", descriptor->package_name},
                                {"abi_version", descriptor->abi_version},
                                {"source_revision", descriptor->source_revision},
                                {"binary_hash", descriptor->binary_hash},
                                {"signature_policy", descriptor->signature_policy},
                                {"capability_role", descriptor->capability_role},
                                {"runtime_language", descriptor->runtime_language},
                                {"entrypoint_count", std::to_string(descriptor->entrypoints.size())}});
    EmitUdrMetric("sb_udr_registration_total", request, "ok", "register");
  }
  return result;
}

EngineLoadUdrPackageResult EngineLoadUdrPackage(const EngineLoadUdrPackageRequest& request) {
  auto authority = ValidateUdrAuthority<EngineLoadUdrPackageResult>(
      request,
      kLoadOperation,
      true,
      false);
  if (!authority.ok) { return authority; }
  ApiBehaviorRecord existing;
  auto visible = RequireVisibleUdr<EngineLoadUdrPackageResult>(request, kLoadOperation, &existing);
  if (!visible.ok) { return visible; }
  if (!HasSupportedUdrAbi(request, &existing)) {
    return MakeUdrFailure<EngineLoadUdrPackageResult>(
        request.context,
        kLoadOperation,
        "SB_ENGINE_API_UDR_ABI_UNSUPPORTED",
        "supported_udr_abi_required");
  }
  auto dependencies = ValidateDependencies<EngineLoadUdrPackageResult>(request, kLoadOperation);
  if (!dependencies.ok) { return dependencies; }
  const auto runtime_loaded = udr_runtime::LoadPackage(request.target_object.uuid.canonical);
  if (!runtime_loaded.ok) {
    EmitUdrMetric("sb_udr_load_total", request, "refused", "load", runtime_loaded.diagnostic_code);
    return RuntimeStatusFailure<EngineLoadUdrPackageResult>(
        request.context,
        kLoadOperation,
        runtime_loaded,
        "SB_ENGINE_API_UDR_LOAD_FAILED");
  }
  auto result = PersistedRecordResult<EngineLoadUdrPackageResult>(
      request,
      kLoadOperation,
      kUdrKind,
      true,
      "loaded");
  if (result.ok) {
    AddUdrLifecycleEvidence(&result, "loaded", "supported", "load_evidence_recorded");
    AddApiBehaviorEvidence(&result, "udr_loader", "init_callback_completed");
    AddApiBehaviorEvidence(&result, "udr_entrypoints", "dispatch_table_published");
    AddApiBehaviorEvidence(&result, "udr_cache", "udr_package_generation_invalidated");
    EmitUdrMetric("sb_udr_load_total", request, "ok", "load");
  }
  return result;
}

EngineUnloadUdrPackageResult EngineUnloadUdrPackage(const EngineUnloadUdrPackageRequest& request) {
  auto authority = ValidateUdrAuthority<EngineUnloadUdrPackageResult>(
      request,
      kUnloadOperation,
      true,
      false,
      false,
      true);
  if (!authority.ok) { return authority; }
  ApiBehaviorRecord existing;
  auto visible = RequireVisibleUdr<EngineUnloadUdrPackageResult>(request, kUnloadOperation, &existing);
  if (!visible.ok) { return visible; }
  const auto runtime_unloaded = udr_runtime::UnloadPackage(request.target_object.uuid.canonical);
  if (!runtime_unloaded.ok) {
    EmitUdrMetric("sb_udr_unload_total", request, "refused", "unload", runtime_unloaded.diagnostic_code);
    return RuntimeStatusFailure<EngineUnloadUdrPackageResult>(
        request.context,
        kUnloadOperation,
        runtime_unloaded,
        "SB_ENGINE_API_UDR_UNLOAD_FAILED");
  }
  auto result = PersistedRecordResult<EngineUnloadUdrPackageResult>(
      request,
      kUnloadOperation,
      kUdrKind,
      true,
      "unloaded");
  if (result.ok) {
    AddUdrLifecycleEvidence(&result, "unloaded", "preserved", "unload_evidence_recorded");
    AddApiBehaviorEvidence(&result, "udr_loader", "shutdown_callback_completed");
    AddApiBehaviorEvidence(&result, "udr_entrypoints", "dispatch_table_removed");
    AddApiBehaviorEvidence(&result, "udr_cache", "udr_package_generation_invalidated");
    EmitUdrMetric("sb_udr_unload_total", request, "ok", "unload");
  }
  return result;
}

EngineInspectUdrPackageResult EngineInspectUdrPackages(const EngineInspectUdrPackageRequest& request) {
  auto authority = ValidateUdrAuthority<EngineInspectUdrPackageResult>(
      request,
      kInspectOperation,
      false,
      true);
  if (!authority.ok) { return authority; }
  auto result = MakeApiBehaviorSuccess<EngineInspectUdrPackageResult>(request.context, kInspectOperation);
  for (const auto& record : VisibleApiBehaviorRecords(request.context, kUdrKind, request.context.local_transaction_id)) {
    const auto runtime_state = udr_runtime::GetPackageState(record.object_uuid);
    AddApiBehaviorRow(&result, {{"object_uuid", record.object_uuid},
                                {"object_kind", record.object_kind},
                                {"name", record.default_name},
                                {"state", record.state},
                                {"payload", record.payload},
                                {"runtime_registered", runtime_state ? "true" : "false"},
                                {"runtime_loaded", runtime_state && runtime_state->loaded ? "true" : "false"},
                                {"active_invocations", runtime_state ? std::to_string(runtime_state->active_invocations) : "0"},
                                {"abi_version", runtime_state ? runtime_state->abi_version : ""},
                                {"source_revision", runtime_state ? runtime_state->source_revision : ""},
                                {"binary_hash", runtime_state ? runtime_state->binary_hash : ""},
                                {"signature_policy", runtime_state ? runtime_state->signature_policy : ""},
                                {"capability_role", runtime_state ? runtime_state->capability_role : ""},
                                {"runtime_language", runtime_state ? runtime_state->runtime_language : ""},
                                {"entrypoints", runtime_state ? JoinEntrypoints(runtime_state->entrypoint_names) : ""}});
  }
  AddEngineExtensionEvidence(&result, "udr", "inspected");
  AddApiBehaviorEvidence(&result, "udr_permission", "inspect_udr_checked");
  AddApiBehaviorEvidence(&result, "udr_runtime", "inspected");
  AddApiBehaviorEvidence(&result, "udr_metrics", "inspect_event_emitted");
  AddApiBehaviorEvidence(&result, "authority_boundary", "mga_sblr_uuid_security_transaction_preserved");
  EmitUdrMetric("sb_udr_inspect_total", request, "ok", "inspect");
  return result;
}

EngineInvokeUdrPackageResult EngineInvokeUdrPackage(const EngineInvokeUdrPackageRequest& request) {
  auto authority = ValidateUdrAuthority<EngineInvokeUdrPackageResult>(
      request,
      kInvokeOperation,
      false,
      false,
      true);
  if (!authority.ok) { return authority; }
  if (!HasSblrInvocationAuthority(request)) {
    EmitUdrMetric("sb_udr_invocation_total", request, "refused", "invoke", "sblr_authority_required");
    return MakeUdrFailure<EngineInvokeUdrPackageResult>(
        request.context,
        kInvokeOperation,
        "SB_ENGINE_API_UDR_SBLR_INVOCATION_REQUIRED",
        "udr_invocation_requires_sblr_authorized_invocation");
  }
  ApiBehaviorRecord existing;
  auto visible = RequireVisibleUdr<EngineInvokeUdrPackageResult>(request, kInvokeOperation, &existing);
  if (!visible.ok) { return visible; }
  if (existing.state != "loaded") {
    EmitUdrMetric("sb_udr_invocation_total", request, "refused", "invoke", "not_loaded");
    return MakeUdrFailure<EngineInvokeUdrPackageResult>(
        request.context,
        kInvokeOperation,
        "SB_ENGINE_API_UDR_NOT_LOADED",
        "target_udr_package_must_be_loaded_before_invocation");
  }
  std::string budget_detail;
  if (ResourceBudgetExceeded(request, &budget_detail)) {
    EmitUdrMetric("sb_udr_resource_refused_total", request, "refused", "invoke", budget_detail);
    return MakeUdrFailure<EngineInvokeUdrPackageResult>(
        request.context,
        kInvokeOperation,
        "SB_ENGINE_API_UDR_RESOURCE_LIMIT_EXCEEDED",
        budget_detail);
  }

  auto result = MakeApiBehaviorSuccess<EngineInvokeUdrPackageResult>(request.context, kInvokeOperation);
  const auto entrypoint = RequestedEntrypoint(request);
  const auto payload = RequestedInvocationPayload(request);
  const auto context_packet = RequestedContextPacket(request);
  const auto invoked = udr_runtime::InvokePackage(
      {existing.object_uuid, entrypoint, payload, context_packet});
  if (!invoked.ok) {
    EmitUdrMetric("sb_udr_invocation_total", request, "refused", "invoke", "entrypoint_failed");
    return MakeUdrFailure<EngineInvokeUdrPackageResult>(
        request.context,
        kInvokeOperation,
        RuntimeCallFailureCode(invoked),
        invoked.message_vector_json.empty() ? "udr_entrypoint_failed"
                                            : invoked.message_vector_json);
  }
  const auto visible_payload = EngineVisibleUdrPayload(invoked.payload, payload);
  AddApiBehaviorRow(&result, {{"object_uuid", existing.object_uuid},
                              {"object_kind", existing.object_kind},
                              {"state", existing.state},
                              {"entrypoint", entrypoint},
                              {"invocation_result", "udr_entrypoint_invoked"},
                              {"payload_bytes", std::to_string(payload.size())},
                              {"result_payload_bytes", std::to_string(visible_payload.size())},
                              {"result_payload", visible_payload},
                              {"message_vector_json", invoked.message_vector_json}});
  AddEngineExtensionEvidence(&result, "udr", "invoked");
  AddApiBehaviorEvidence(&result, "udr_permission", "invoke_udr_checked");
  AddApiBehaviorEvidence(&result, "udr_abi", "supported");
  AddApiBehaviorEvidence(&result, "udr_dispatch", "entrypoint_callback_invoked");
  AddApiBehaviorEvidence(&result, "udr_resource", "budget_checked");
  AddApiBehaviorEvidence(&result, "udr_audit", "invocation_evidence_recorded");
  AddApiBehaviorEvidence(&result, "udr_metrics", "invocation_event_emitted");
  AddApiBehaviorEvidence(&result, "sblr_authority", "SBLR_UDR_INVOKE");
  AddApiBehaviorEvidence(&result, "authority_boundary", "mga_sblr_uuid_security_transaction_preserved");
  EmitUdrMetric("sb_udr_invocation_total", request, "ok", "invoke");
  return result;
}

}  // namespace scratchbird::engine::internal_api
