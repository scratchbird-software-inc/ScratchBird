// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "native_compile.hpp"

#include "metric_registry.hpp"
#include "runtime_capabilities.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <openssl/sha.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifndef SCRATCHBIRD_LLVM_LINK_MODE
#define SCRATCHBIRD_LLVM_LINK_MODE "dynamic"
#endif

#ifndef SCRATCHBIRD_LLVM_LIBRARY_PATH
#define SCRATCHBIRD_LLVM_LIBRARY_PATH ""
#endif

#ifndef SCRATCHBIRD_LLVM_SOURCE_ROOT
#define SCRATCHBIRD_LLVM_SOURCE_ROOT ""
#endif

#ifndef SCRATCHBIRD_LLVM_TOOLS_ROOT
#define SCRATCHBIRD_LLVM_TOOLS_ROOT ""
#endif

#ifndef SCRATCHBIRD_LLVM_STAGING_BUILD_DIR
#define SCRATCHBIRD_LLVM_STAGING_BUILD_DIR ""
#endif

namespace scratchbird::engine::native_compile {
namespace memory = scratchbird::core::memory;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;
namespace {

bool Contains(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

bool ProfilePresent(const std::vector<std::string>& profiles, std::string_view token) {
  for (const auto& profile : profiles) {
    if (Contains(profile, token)) {
      return true;
    }
  }
  return false;
}

bool OptionPresent(const std::vector<std::string>& options, std::string_view token) {
  for (const auto& option : options) {
    if (Contains(option, token)) {
      return true;
    }
  }
  return false;
}

std::string HexBytes(const unsigned char* data, std::size_t size) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    const auto value = data[i];
    out.push_back(kHex[(value >> 4U) & 0x0fU]);
    out.push_back(kHex[value & 0x0fU]);
  }
  return out;
}

// SEARCH_KEY: OEIC_LLVM_DYNAMIC_LOADER
// Enterprise LLVM provenance and cache keys use SHA-256 digests. The digest
// material is planning evidence only; SBLR interpreter semantics remain the
// value, transaction, security, visibility, and recovery authority.
std::string HashText(std::string_view value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
  return HexBytes(digest.data(), digest.size());
}

std::string JoinValues(const std::vector<std::string>& values, std::string_view separator) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << separator;
    }
    out << values[i];
  }
  return out.str();
}

std::string RuntimeTargetTriple() {
#if defined(_WIN32)
  constexpr std::string_view os = "windows";
#elif defined(__APPLE__)
  constexpr std::string_view os = "darwin";
#elif defined(__linux__)
  constexpr std::string_view os = "linux";
#elif defined(__FreeBSD__)
  constexpr std::string_view os = "freebsd";
#else
  constexpr std::string_view os = "unknown";
#endif
  return platform::CurrentRuntimeArchitecture() + "-scratchbird-" + std::string(os);
}

std::string DescriptorSetDigest(const NativeCompileRequest& request) {
  std::ostringstream out;
  for (const auto& descriptor : request.descriptors) {
    out << descriptor.descriptor_uuid << ':' << descriptor.descriptor_kind << ':'
        << descriptor.canonical_type_name << ':' << HashText(descriptor.encoded_descriptor) << ';';
  }
  return HashText(out.str());
}

NativeCompilePolicyProfile SelectPolicy(const NativeCompileRequest& request) {
  if (ProfilePresent(request.policy_profiles, "native_compile.dev_debug_ir_export")) {
    return NativeCompilePolicyProfile::dev_debug_ir_export;
  }
  if (ProfilePresent(request.policy_profiles, "native_compile.jit_required_for_declared_units")) {
    return NativeCompilePolicyProfile::jit_required_for_declared_units;
  }
  if (ProfilePresent(request.policy_profiles, "native_compile.aot_package_required")) {
    return NativeCompilePolicyProfile::aot_package_required;
  }
  if (ProfilePresent(request.policy_profiles, "native_compile.aot_optional")) {
    return NativeCompilePolicyProfile::aot_optional;
  }
  if ((request.policy_profiles.empty() || ProfilePresent(request.policy_profiles, "policy=default")) &&
      request.requested_mode == NativeCompileMode::aot) {
    return NativeCompilePolicyProfile::aot_optional;
  }
  if (ProfilePresent(request.policy_profiles, "native_compile.jit_optional") ||
      request.policy_profiles.empty() ||
      ProfilePresent(request.policy_profiles, "policy=default")) {
    return NativeCompilePolicyProfile::jit_optional;
  }
  if (ProfilePresent(request.policy_profiles, "native_compile.disabled")) {
    return NativeCompilePolicyProfile::disabled;
  }
  return NativeCompilePolicyProfile::invalid;
}

bool PolicyAllowsRequestedMode(NativeCompilePolicyProfile profile, NativeCompileMode mode) {
  switch (profile) {
    case NativeCompilePolicyProfile::jit_optional:
    case NativeCompilePolicyProfile::jit_required_for_declared_units:
      return mode == NativeCompileMode::jit;
    case NativeCompilePolicyProfile::aot_optional:
    case NativeCompilePolicyProfile::aot_package_required:
      return mode == NativeCompileMode::aot || mode == NativeCompileMode::jit;
    case NativeCompilePolicyProfile::dev_debug_ir_export:
      return true;
    case NativeCompilePolicyProfile::disabled:
    case NativeCompilePolicyProfile::invalid:
      return false;
  }
  return false;
}

bool PolicyRequiresNative(NativeCompilePolicyProfile profile) {
  return profile == NativeCompilePolicyProfile::jit_required_for_declared_units ||
         profile == NativeCompilePolicyProfile::aot_package_required;
}

struct BackendInfo {
  bool available = false;
  bool verifier_available = false;
  std::string provider = "missing";
  std::string version = "unknown";
  std::string load_mode = SCRATCHBIRD_LLVM_LINK_MODE;
  std::string library_path = SCRATCHBIRD_LLVM_LIBRARY_PATH;
  std::string library_path_digest = HashText(SCRATCHBIRD_LLVM_LIBRARY_PATH);
  std::string source_root = SCRATCHBIRD_LLVM_SOURCE_ROOT;
  std::string source_root_digest = HashText(SCRATCHBIRD_LLVM_SOURCE_ROOT);
  std::string tools_root = SCRATCHBIRD_LLVM_TOOLS_ROOT;
  std::string tools_root_digest = HashText(SCRATCHBIRD_LLVM_TOOLS_ROOT);
  std::string staging_build_dir = SCRATCHBIRD_LLVM_STAGING_BUILD_DIR;
  std::string staging_build_dir_digest = HashText(SCRATCHBIRD_LLVM_STAGING_BUILD_DIR);
  std::string target_triple = RuntimeTargetTriple();
  std::string target_feature_set = JoinValues(platform::CurrentRuntimeCpuFeatures(), ",");
};

struct NativeCompileDefaultMemoryLedgers {
  memory::HierarchicalMemoryBudgetLedger reservation_ledger;
  memory::ForeignMemoryReservationLedger foreign_ledger;
};

NativeCompileDefaultMemoryLedgers& DefaultNativeCompileMemoryLedgers() {
  static NativeCompileDefaultMemoryLedgers ledgers;
  return ledgers;
}

bool DynamicLlvmLibraryLoads(const std::string& path) {
  if (path.empty()) {
    return false;
  }
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(path.c_str());
  if (handle == nullptr) {
    return false;
  }
  FreeLibrary(handle);
  return true;
#else
  void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (handle == nullptr) {
    return false;
  }
  dlclose(handle);
  return true;
#endif
};

memory::ForeignMemoryLinkageMode LinkageModeFromConfig(std::string_view mode) {
  if (mode == "static") {
    return memory::ForeignMemoryLinkageMode::static_library;
  }
  if (mode == "dynamic") {
    return memory::ForeignMemoryLinkageMode::dynamic_library;
  }
  return memory::ForeignMemoryLinkageMode::not_applicable;
}

bool PotentialLiveLlvmConfigured(bool simulate_unavailable) {
  if (simulate_unavailable) {
    return false;
  }
  const std::string_view mode = SCRATCHBIRD_LLVM_LINK_MODE;
  const std::string_view library = SCRATCHBIRD_LLVM_LIBRARY_PATH;
  return mode != "disabled" && !library.empty();
}

BackendInfo DetectBackend(bool simulate_unavailable, bool allow_dynamic_load) {
  BackendInfo backend;
  const auto manifest = platform::DetectRuntimeCapabilities();
  for (const auto& capability : manifest.capabilities) {
    if (capability.key != "compiler.llvm") {
      continue;
    }
    backend.available = !simulate_unavailable && capability.state == platform::CapabilityState::present;
    backend.verifier_available = backend.available;
    backend.provider = capability.provider;
    if (backend.load_mode == "dynamic") {
      const bool loaded = !simulate_unavailable && allow_dynamic_load &&
                          DynamicLlvmLibraryLoads(backend.library_path);
      backend.available = backend.available && loaded;
      backend.verifier_available = backend.available;
      if (!allow_dynamic_load && !simulate_unavailable && !backend.library_path.empty()) {
        backend.provider += "|dynamic_load=not_attempted_memory_accounting_required";
      } else {
        backend.provider += loaded ? "|dynamic_load=ok" : "|dynamic_load=failed";
      }
    } else if (backend.load_mode == "static") {
      backend.provider += "|static_link=ok";
    } else {
      backend.available = false;
      backend.verifier_available = false;
      backend.provider += "|link_mode=disabled";
    }
    const auto marker = std::string("LLVM-");
    const auto pos = capability.provider.find(marker);
    if (pos != std::string::npos) {
      backend.version = capability.provider.substr(pos + marker.size());
    } else {
      backend.version = capability.provider;
    }
    return backend;
  }
  return backend;
}

struct Lowerability {
  bool lowerable = false;
  std::string unit_kind = "unknown";
  std::string reason = "not_classified";
};

Lowerability ClassifyLowerability(const NativeCompileRequest& request) {
  Lowerability lower;
  const auto& module = request.module_payload;
  if (module.empty()) {
    lower.reason = "module_payload_required";
    return lower;
  }
  if (Contains(module, "sql:") || Contains(module, "SELECT ") || Contains(module, "INSERT ") ||
      Contains(module, "UPDATE ") || Contains(module, "DELETE ")) {
    lower.reason = "sql_compile_forbidden";
    return lower;
  }
  if (Contains(module, "parser_ast") || Contains(module, "parse_tree") ||
      OptionPresent(request.option_envelopes, "parser_authority") ||
      OptionPresent(request.option_envelopes, "parser_ast")) {
    lower.reason = "parser_authority_forbidden";
    return lower;
  }
  if (Contains(module, "reference") || OptionPresent(request.option_envelopes, "reference_authority") ||
      OptionPresent(request.option_envelopes, "reference_plan") ||
      OptionPresent(request.option_envelopes, "reference_result")) {
    lower.reason = "reference_authority_forbidden";
    return lower;
  }
  if (Contains(module, "protocol_frame") || Contains(module, "wire_frame") ||
      OptionPresent(request.option_envelopes, "protocol_frame") ||
      OptionPresent(request.option_envelopes, "client_ir")) {
    lower.reason = "protocol_or_client_authority_forbidden";
    return lower;
  }
  const bool sblr_input = Contains(module, "sblr") || Contains(module, "SBLR");
  const bool engine_ir_input = Contains(module, "engine_ir") || Contains(module, "ENGINE_IR");
  if (engine_ir_input &&
      !OptionPresent(request.option_envelopes, "engine_ir_validated") &&
      !OptionPresent(request.option_envelopes, "engine_ir:validated")) {
    lower.reason = "engine_ir_validation_required";
    return lower;
  }
  if (!sblr_input && !engine_ir_input) {
    lower.reason = "sblr_or_engine_ir_required";
    return lower;
  }
  if (Contains(module, "catalog_security") || Contains(module, "grant_check") || Contains(module, "rls_check")) {
    lower.unit_kind = "catalog_or_security_check";
    lower.reason = "authority_check_forbidden";
    return lower;
  }
  if (Contains(module, "mga_visibility")) {
    lower.unit_kind = "mga_visibility_check";
    lower.reason = "mga_visibility_forbidden";
    return lower;
  }
  if (Contains(module, "dml_mutation") || Contains(module, "commit") || Contains(module, "rollback")) {
    lower.unit_kind = "dml_mutation_side_effect";
    lower.reason = "mutation_side_effect_forbidden";
    return lower;
  }
  if (Contains(module, "udr_call")) {
    lower.unit_kind = "udr_call";
    lower.reason = "udr_call_interpreter_only";
    return lower;
  }
  if (Contains(module, "log(") || Contains(module, "logging_function")) {
    lower.unit_kind = "logging_function";
    lower.reason = "logging_interpreter_only";
    return lower;
  }
  if (Contains(module, "cluster")) {
    lower.unit_kind = "cluster_operation";
    lower.reason = "cluster_operation_forbidden_noncluster";
    return lower;
  }
  if (Contains(module, "predicate") || Contains(module, "filter")) {
    lower.unit_kind = "predicate";
  } else if (Contains(module, "projection") || Contains(module, "project")) {
    lower.unit_kind = "projection";
  } else {
    lower.unit_kind = "expression";
  }
  lower.lowerable = true;
  lower.reason = "llvm_safe";
  return lower;
}

std::string CacheKeyMaterial(const NativeCompileRequest& request,
                             const BackendInfo& backend,
                             const Lowerability& lowerability) {
  std::ostringstream out;
  out << "sblr_hash=" << HashText(request.module_payload);
  out << ";sblr_version=" << request.sblr_version;
  out << ";opcode_registry_epoch=" << request.opcode_registry_epoch;
  out << ";target_object_uuid=" << request.target_object_uuid;
  out << ";principal_uuid=" << request.principal_uuid;
  out << ";catalog_generation_id=" << request.catalog_generation_id;
  out << ";security_epoch=" << request.security_epoch;
  out << ";policy_epoch=" << request.policy_epoch;
  out << ";resource_epoch=" << request.resource_epoch;
  out << ";engine_abi_id=" << request.engine_abi_id;
  out << ";numeric_backend_profile=" << request.numeric_backend_profile;
  out << ";backend_provider=" << backend.provider;
  out << ";backend_version=" << backend.version;
  out << ";llvm_load_mode=" << backend.load_mode;
  out << ";llvm_library_path_digest=" << backend.library_path_digest;
  out << ";llvm_source_root_digest=" << backend.source_root_digest;
  out << ";llvm_tools_root_digest=" << backend.tools_root_digest;
  out << ";llvm_staging_build_dir_digest=" << backend.staging_build_dir_digest;
  out << ";target_triple=" << backend.target_triple;
  out << ";target_feature_set=" << backend.target_feature_set;
  out << ";mode=" << (request.requested_mode == NativeCompileMode::aot ? "aot" : "jit");
  out << ";unit_kind=" << lowerability.unit_kind;
  for (const auto& descriptor : request.descriptors) {
    out << ";descriptor=" << descriptor.descriptor_uuid << ':' << descriptor.descriptor_kind << ':'
        << descriptor.canonical_type_name << ':' << HashText(descriptor.encoded_descriptor);
  }
  for (const auto& profile : request.policy_profiles) {
    out << ";policy_profile=" << profile;
  }
  for (const auto& profile : request.physical_profiles) {
    out << ";physical_profile=" << profile;
  }
  for (const auto& option : request.option_envelopes) {
    out << ";option_envelope_hash=" << HashText(option);
  }
  return out.str();
}

bool CacheKeyComplete(const NativeCompileRequest& request, const BackendInfo& backend) {
  return !request.module_payload.empty() &&
         !request.target_object_uuid.empty() &&
         !request.principal_uuid.empty() &&
         !request.engine_abi_id.empty() &&
         !request.sblr_version.empty() &&
         !request.opcode_registry_epoch.empty() &&
         !request.numeric_backend_profile.empty() &&
         request.catalog_generation_id != 0 &&
         request.security_epoch != 0 &&
         request.policy_epoch != 0 &&
         request.resource_epoch != 0 &&
         request.security_context_present &&
         !request.descriptors.empty() &&
         !backend.provider.empty() &&
         !backend.target_triple.empty();
}

std::filesystem::path ArtifactDirectory(const NativeCompileRequest& request) {
  if (request.database_path.empty()) {
    return {};
  }
  return std::filesystem::path(request.database_path + ".sb.native_aot");
}

bool WriteAotArtifact(const NativeCompileRequest& request,
                      const NativeCompileResult& result,
                      const std::string& material,
                      std::string* artifact_path,
                      std::string* artifact_digest) {
  const auto directory = ArtifactDirectory(request);
  if (directory.empty()) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    return false;
  }
  const auto path = directory / (result.cache_key + ".native_aot.meta");
  std::ostringstream payload;
  payload << "artifact_kind=metadata_only_aot_evidence\n";
  payload << "cache_key=" << result.cache_key << "\n";
  payload << "provenance_hash=" << result.provenance_hash << "\n";
  payload << "effective_mode=" << NativeCompileEffectiveModeName(result.effective_mode) << "\n";
  payload << "backend_provider=" << result.backend_provider << "\n";
  payload << "llvm_version=" << result.llvm_version << "\n";
  payload << "llvm_load_mode=" << result.llvm_load_mode << "\n";
  payload << "llvm_library_path_digest=" << result.llvm_library_path_digest << "\n";
  payload << "llvm_source_root_digest=" << result.llvm_source_root_digest << "\n";
  payload << "target_triple=" << result.target_triple << "\n";
  payload << "target_feature_set=" << result.target_feature_set << "\n";
  payload << "unit_kind=" << result.unit_kind << "\n";
  payload << "sblr_or_ir_digest=" << result.sblr_or_ir_digest << "\n";
  payload << "descriptor_set_digest=" << result.descriptor_set_digest << "\n";
  payload << "cache_key_material_hash=" << HashText(material) << "\n";
  const auto encoded = payload.str();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << encoded;
  out.flush();
  if (!out) {
    return false;
  }
  *artifact_path = path.string();
  *artifact_digest = HashText(encoded);
  return true;
}

void AttachBackendEvidence(NativeCompileResult* result,
                           const BackendInfo& backend,
                           const NativeCompileRequest& request) {
  result->backend_available = backend.available;
  result->verifier_available = backend.verifier_available;
  result->backend_provider = backend.provider;
  result->llvm_version = backend.version;
  result->llvm_load_mode = backend.load_mode;
  result->llvm_library_path = backend.library_path;
  result->llvm_library_path_digest = backend.library_path_digest;
  result->llvm_source_root = backend.source_root;
  result->llvm_source_root_digest = backend.source_root_digest;
  result->llvm_tools_root = backend.tools_root;
  result->llvm_tools_root_digest = backend.tools_root_digest;
  result->llvm_staging_build_dir = backend.staging_build_dir;
  result->llvm_staging_build_dir_digest = backend.staging_build_dir_digest;
  result->target_triple = backend.target_triple;
  result->target_feature_set = backend.target_feature_set;
  result->sblr_or_ir_digest = HashText(request.module_payload);
  result->descriptor_set_digest = DescriptorSetDigest(request);
  result->llvm_memory_test_fixture =
      request.memory_accounting.explicit_test_fixture;
}

memory::LlvmMemoryAccountingRequest BuildLlvmMemoryAccountingRequest(
    const NativeCompileRequest& request,
    const BackendInfo& backend,
    bool provider_available) {
  auto accounting = request.memory_accounting;
  auto& default_ledgers = DefaultNativeCompileMemoryLedgers();
  if (accounting.reservation_ledger == nullptr) {
    accounting.reservation_ledger = &default_ledgers.reservation_ledger;
  }
  if (accounting.foreign_ledger == nullptr) {
    accounting.foreign_ledger = &default_ledgers.foreign_ledger;
  }
  if (accounting.scope_chain.empty()) {
    accounting.scope_chain = {
        {memory::HierarchicalMemoryScopeKind::process,
         "native_compile.process"},
        {memory::HierarchicalMemoryScopeKind::database,
         request.database_path.empty() ? "native_compile.database"
                                       : request.database_path},
        {memory::HierarchicalMemoryScopeKind::session,
         request.principal_uuid.empty() ? "native_compile.session"
                                        : "native_compile.session." +
                                              request.principal_uuid},
        {memory::HierarchicalMemoryScopeKind::statement,
         request.target_object_uuid.empty() ? "native_compile.statement"
                                            : "native_compile.statement." +
                                                  request.target_object_uuid}};
  }
  accounting.linkage_mode = LinkageModeFromConfig(backend.load_mode);
  accounting.provider_available = provider_available;
  accounting.aot = request.requested_mode == NativeCompileMode::aot;
  if (accounting.owner_id.empty()) {
    accounting.owner_id = request.principal_uuid.empty()
                              ? "native_compile.unknown_principal"
                              : "native_compile." + request.principal_uuid;
  }
  if (accounting.owning_scope.empty()) {
    accounting.owning_scope = request.target_object_uuid.empty()
                                  ? "native_compile.unknown_target"
                                  : "native_compile." + request.target_object_uuid;
  }
  if (accounting.operation_id.empty()) {
    accounting.operation_id =
        request.requested_mode == NativeCompileMode::aot
            ? "native_compile.aot"
            : "native_compile.jit";
  }
  if (accounting.native_callsite.empty()) {
    accounting.native_callsite = "engine.native_compile.llvm";
  }
  if (accounting.provider_label.empty()) {
    accounting.provider_label = backend.provider;
  }
  if (accounting.provenance.source ==
      memory::HierarchicalMemoryBudgetProvenanceSource::unknown) {
    accounting.provenance.source =
        memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  }
  if (accounting.provenance.source_label.empty()) {
    accounting.provenance.source_label = "native_compile_llvm";
  }
  accounting.provenance.engine_mga_authoritative = true;
  accounting.provenance.memory_evidence_only = true;
  if (accounting.authority.evidence_label.empty()) {
    accounting.authority.evidence_label = "native_compile_llvm";
  }
  if (accounting.authority.authority_generation.empty()) {
    accounting.authority.authority_generation = "runtime";
  }
  accounting.evidence.push_back("native_compile.memory_accounting=ceic_061");
  accounting.evidence.push_back("native_compile.mode=" +
                                std::string(request.requested_mode ==
                                                    NativeCompileMode::aot
                                                ? "aot"
                                                : "jit"));
  accounting.evidence.push_back("native_compile.backend_provider=" +
                                backend.provider);
  accounting.evidence.push_back("native_compile.target_triple=" +
                                backend.target_triple);
  return accounting;
}

void AttachLlvmMemoryEvidence(
    NativeCompileResult* result,
    const memory::LlvmMemoryAccountingAcquireResult& acquired) {
  result->llvm_memory_accounting_required = true;
  if (acquired.ok()) {
    result->llvm_memory_reserved = true;
    const auto snapshot = acquired.reservation->Snapshot();
    result->llvm_memory_reserved_bytes = snapshot.reserved_bytes;
    result->llvm_memory_reservation_count = snapshot.reservation_count;
    result->llvm_memory_test_fixture = snapshot.explicit_test_fixture;
    result->llvm_memory_evidence.insert(result->llvm_memory_evidence.end(),
                                        snapshot.evidence.begin(),
                                        snapshot.evidence.end());
  }
  result->llvm_memory_evidence.insert(result->llvm_memory_evidence.end(),
                                      acquired.evidence.begin(),
                                      acquired.evidence.end());
}

NativeCompileResult FinalizeLlvmMemory(
    NativeCompileResult result,
    memory::LlvmMemoryAccountingReservation* reservation) {
  if (reservation == nullptr) {
    return result;
  }
  const auto release =
      reservation->Release(memory::ForeignMemoryReleaseEvent::adapter_shutdown);
  result.llvm_memory_released = release.ok();
  result.llvm_memory_evidence.insert(result.llvm_memory_evidence.end(),
                                     release.evidence.begin(),
                                     release.evidence.end());
  if (!release.ok()) {
    result.ok = false;
    result.compiled = false;
    result.fallback_used = false;
    result.effective_mode = NativeCompileEffectiveMode::refused;
    result.diagnostic_code = "NATIVE.LLVM_MEMORY_RELEASE_REFUSED";
    result.diagnostic_detail = release.diagnostic.diagnostic_code.empty()
                                   ? "llvm_memory_release_refused"
                                   : release.diagnostic.diagnostic_code;
  }
  return result;
}

void IncrementNativeMetric(const std::string& family,
                           std::vector<metrics::MetricLabel> labels,
                           double value = 1.0) {
  (void)metrics::DefaultMetricRegistry().IncrementCounter(family, std::move(labels), value, "native_compile");
}

void ObserveNativeMetric(const std::string& family,
                         std::vector<metrics::MetricLabel> labels,
                         double value) {
  (void)metrics::DefaultMetricRegistry().ObserveHistogram(family, std::move(labels), value, "native_compile");
}

NativeCompileResult Failure(const NativeCompileRequest& request,
                            NativeCompilePolicyProfile profile,
                            const BackendInfo& backend,
                            std::string code,
                            std::string detail) {
  NativeCompileResult result;
  result.policy_profile = profile;
  AttachBackendEvidence(&result, backend, request);
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  result.effective_mode = NativeCompileEffectiveMode::refused;
  IncrementNativeMetric("sb_native_compile_fallback_total",
                        {{"operation", request.requested_mode == NativeCompileMode::aot ? "aot" : "jit"},
                         {"result", "refused"},
                         {"reason", result.diagnostic_code}});
  return result;
}

}  // namespace

NativeCompileResult CompileNativeUnit(const NativeCompileRequest& request) {
  const auto profile = SelectPolicy(request);
  if (request.simulate_backend_unavailable &&
      !request.memory_accounting.explicit_test_fixture) {
    const auto backend = DetectBackend(true, false);
    return Failure(request,
                   profile,
                   backend,
                   "NATIVE.LLVM_TEST_FIXTURE_REQUIRED",
                   "simulate_backend_unavailable_requires_explicit_test_fixture");
  }
  const bool live_llvm_configured =
      PotentialLiveLlvmConfigured(request.simulate_backend_unavailable);
  BackendInfo preflight_backend;
  preflight_backend.load_mode = SCRATCHBIRD_LLVM_LINK_MODE;
  preflight_backend.library_path = SCRATCHBIRD_LLVM_LIBRARY_PATH;
  preflight_backend.library_path_digest = HashText(SCRATCHBIRD_LLVM_LIBRARY_PATH);
  preflight_backend.source_root = SCRATCHBIRD_LLVM_SOURCE_ROOT;
  preflight_backend.source_root_digest = HashText(SCRATCHBIRD_LLVM_SOURCE_ROOT);
  preflight_backend.tools_root = SCRATCHBIRD_LLVM_TOOLS_ROOT;
  preflight_backend.tools_root_digest = HashText(SCRATCHBIRD_LLVM_TOOLS_ROOT);
  preflight_backend.staging_build_dir = SCRATCHBIRD_LLVM_STAGING_BUILD_DIR;
  preflight_backend.staging_build_dir_digest =
      HashText(SCRATCHBIRD_LLVM_STAGING_BUILD_DIR);
  preflight_backend.target_triple = RuntimeTargetTriple();
  preflight_backend.target_feature_set =
      JoinValues(platform::CurrentRuntimeCpuFeatures(), ",");
  preflight_backend.provider =
      std::string("configured|link-mode=") + SCRATCHBIRD_LLVM_LINK_MODE +
      "|library=" + SCRATCHBIRD_LLVM_LIBRARY_PATH;
  memory::LlvmMemoryAccountingAcquireResult llvm_memory_acquired;
  if (live_llvm_configured) {
    llvm_memory_acquired = memory::AcquireLlvmMemoryAccountingReservation(
        BuildLlvmMemoryAccountingRequest(request, preflight_backend, true));
    if (!llvm_memory_acquired.ok()) {
      auto result = Failure(
          request,
          profile,
          DetectBackend(request.simulate_backend_unavailable, false),
          "NATIVE.LLVM_MEMORY_ACCOUNTING_REFUSED",
          llvm_memory_acquired.diagnostic.diagnostic_code.empty()
              ? "llvm_memory_accounting_refused"
              : llvm_memory_acquired.diagnostic.diagnostic_code);
      AttachLlvmMemoryEvidence(&result, llvm_memory_acquired);
      return result;
    }
  }
  const auto backend =
      DetectBackend(request.simulate_backend_unavailable,
                    !live_llvm_configured || llvm_memory_acquired.ok());
  const auto lowerability = ClassifyLowerability(request);
  const auto start = std::chrono::steady_clock::now();

  auto finalize = [&](NativeCompileResult result) {
    if (llvm_memory_acquired.ok()) {
      AttachLlvmMemoryEvidence(&result, llvm_memory_acquired);
    }
    return FinalizeLlvmMemory(
        std::move(result), llvm_memory_acquired.ok()
                               ? llvm_memory_acquired.reservation.get()
                               : nullptr);
  };

  if (profile == NativeCompilePolicyProfile::invalid) {
    return finalize(Failure(request, profile, backend, "NATIVE.INVALID_COMPILE_POLICY", "invalid_or_unknown_native_compile_policy"));
  }
  if (!PolicyAllowsRequestedMode(profile, request.requested_mode)) {
    if (!PolicyRequiresNative(profile) && request.allow_interpreter_fallback) {
      NativeCompileResult result;
      result.ok = true;
      result.fallback_used = true;
      result.effective_mode = NativeCompileEffectiveMode::interpreter;
      result.policy_profile = profile;
      AttachBackendEvidence(&result, backend, request);
      result.unit_kind = lowerability.unit_kind;
      result.lowerability = "interpreter_by_policy";
      result.diagnostic_code = "NATIVE.COMPILE_FAILED_FALLBACK";
      result.diagnostic_detail = "native_compile_policy_disabled_or_mode_not_enabled";
      IncrementNativeMetric("sb_native_compile_fallback_total", {{"operation", "policy"}, {"result", "fallback"}, {"reason", result.diagnostic_code}});
      return finalize(std::move(result));
    }
    return finalize(Failure(request,
                            profile,
                            backend,
                            request.requested_mode == NativeCompileMode::aot ? "NATIVE.AOT_DISABLED_BY_POLICY" : "NATIVE.JIT_DISABLED_BY_POLICY",
                            "requested_native_mode_disabled_by_policy"));
  }
  if (!lowerability.lowerable) {
    if (request.allow_interpreter_fallback && !PolicyRequiresNative(profile)) {
      NativeCompileResult result;
      result.ok = true;
      result.fallback_used = true;
      result.effective_mode = NativeCompileEffectiveMode::interpreter;
      result.policy_profile = profile;
      AttachBackendEvidence(&result, backend, request);
      result.unit_kind = lowerability.unit_kind;
      result.lowerability = lowerability.reason;
      result.diagnostic_code = "NATIVE.COMPILE_FAILED_FALLBACK";
      result.diagnostic_detail = lowerability.reason;
      IncrementNativeMetric("sb_native_compile_fallback_total", {{"operation", "lowerability"}, {"result", "fallback"}, {"reason", lowerability.reason}});
      return finalize(std::move(result));
    }
    const std::string code = lowerability.reason == "sql_compile_forbidden"
        ? "NATIVE.SQL_COMPILE_FORBIDDEN"
        : "NATIVE.COMPILE_FAILED_REFUSED";
    return finalize(Failure(request, profile, backend, code, lowerability.reason));
  }
  if (!backend.available) {
    if (request.memory_accounting.explicit_test_fixture &&
        request.allow_interpreter_fallback && !PolicyRequiresNative(profile)) {
      NativeCompileResult result;
      result.ok = true;
      result.fallback_used = true;
      result.effective_mode = NativeCompileEffectiveMode::interpreter;
      result.policy_profile = profile;
      AttachBackendEvidence(&result, backend, request);
      result.backend_available = false;
      result.verifier_available = false;
      result.unit_kind = lowerability.unit_kind;
      result.lowerability = lowerability.reason;
      result.diagnostic_code = "NATIVE.COMPILE_FAILED_FALLBACK";
      result.diagnostic_detail = "llvm_backend_unavailable";
      IncrementNativeMetric("sb_native_compile_fallback_total", {{"operation", "backend"}, {"result", "fallback"}, {"reason", "llvm_backend_unavailable"}});
      return finalize(std::move(result));
    }
    return finalize(Failure(request, profile, backend, "NATIVE.LLVM_BACKEND_UNAVAILABLE", "llvm_backend_unavailable"));
  }
  if (!CacheKeyComplete(request, backend)) {
    return finalize(Failure(request, profile, backend, "NATIVE.CACHE_KEY_INCOMPLETE", "native_compile_cache_key_missing_required_dimension"));
  }

  const auto material = CacheKeyMaterial(request, backend, lowerability);
  NativeCompileResult result;
  result.ok = true;
  result.compiled = true;
  result.cache_hit = request.previously_cached;
  result.effective_mode = request.requested_mode == NativeCompileMode::aot ? NativeCompileEffectiveMode::aot : NativeCompileEffectiveMode::jit;
  result.policy_profile = profile;
  AttachBackendEvidence(&result, backend, request);
  result.unit_kind = lowerability.unit_kind;
  result.lowerability = lowerability.reason;
  result.cache_key = "llvm-" + HashText(material);
  result.cache_key_material_digest = HashText(material);
  result.provenance_hash = HashText("provenance:" + material + ";source=" +
                                    backend.source_root_digest + ";library=" +
                                    backend.library_path_digest);
  if (result.effective_mode == NativeCompileEffectiveMode::aot) {
    result.artifact_written =
        WriteAotArtifact(request, result, material, &result.artifact_path, &result.artifact_digest);
    if (!result.artifact_written) {
      if (request.allow_interpreter_fallback && !PolicyRequiresNative(profile)) {
        result.ok = true;
        result.compiled = false;
        result.fallback_used = true;
        result.effective_mode = NativeCompileEffectiveMode::interpreter;
        result.diagnostic_code = "NATIVE.COMPILE_FAILED_FALLBACK";
        result.diagnostic_detail = "aot_artifact_write_failed";
        IncrementNativeMetric("sb_native_compile_fallback_total", {{"operation", "aot"}, {"result", "fallback"}, {"reason", "artifact_write_failed"}});
        return finalize(std::move(result));
      }
      return finalize(Failure(request, profile, backend, "NATIVE.AOT_ARTIFACT_INVALID", "aot_artifact_write_failed"));
    }
    IncrementNativeMetric("sb_native_compile_aot_load_total", {{"operation", "aot"}, {"result", result.cache_hit ? "cache_hit" : "compiled"}, {"reason", "ok"}});
  } else {
    IncrementNativeMetric("sb_native_compile_jit_compile_total", {{"operation", "jit"}, {"result", result.cache_hit ? "cache_hit" : "compiled"}, {"reason", "ok"}});
    if (result.cache_hit) {
      IncrementNativeMetric("sb_native_compile_jit_cache_hit_total", {{"operation", "jit"}, {"result", "hit"}, {"reason", "ok"}});
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  ObserveNativeMetric("sb_native_compile_jit_compile_seconds",
                      {{"operation", result.effective_mode == NativeCompileEffectiveMode::aot ? "aot" : "jit"},
                       {"result", result.cache_hit ? "cache_hit" : "compiled"},
                       {"reason", "ok"}},
                      static_cast<double>(elapsed) / 1000000.0);
  return finalize(std::move(result));
}

bool NativeArtifactInvalidatedByDependency(const std::string& cache_key_material,
                                           const std::string& dependency_family,
                                           const std::string& dependency_value) {
  if (cache_key_material.empty() || dependency_family.empty()) {
    return true;
  }
  if (dependency_value.empty()) {
    return true;
  }
  const std::string token = dependency_family + "=" + dependency_value;
  return cache_key_material.find(token) != std::string::npos ||
         cache_key_material.find(dependency_value) != std::string::npos;
}

std::string NativeCompileEffectiveModeName(NativeCompileEffectiveMode mode) {
  switch (mode) {
    case NativeCompileEffectiveMode::interpreter: return "interpreter";
    case NativeCompileEffectiveMode::jit: return "jit";
    case NativeCompileEffectiveMode::aot: return "aot";
    case NativeCompileEffectiveMode::refused: return "refused";
  }
  return "refused";
}

std::string NativeCompilePolicyProfileName(NativeCompilePolicyProfile profile) {
  switch (profile) {
    case NativeCompilePolicyProfile::disabled: return "native_compile.disabled";
    case NativeCompilePolicyProfile::jit_optional: return "native_compile.jit_optional";
    case NativeCompilePolicyProfile::jit_required_for_declared_units: return "native_compile.jit_required_for_declared_units";
    case NativeCompilePolicyProfile::aot_optional: return "native_compile.aot_optional";
    case NativeCompilePolicyProfile::aot_package_required: return "native_compile.aot_package_required";
    case NativeCompilePolicyProfile::dev_debug_ir_export: return "native_compile.dev_debug_ir_export";
    case NativeCompilePolicyProfile::invalid: return "native_compile.invalid";
  }
  return "native_compile.invalid";
}

}  // namespace scratchbird::engine::native_compile
