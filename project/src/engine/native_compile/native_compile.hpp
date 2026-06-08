// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "llvm_memory_accounting.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::native_compile {

// SEARCH_KEY: SB_ENGINE_NATIVE_COMPILE_RUNTIME
// Native compilation is acceleration evidence only. SBLR interpreter semantics
// remain authoritative for values, diagnostics, transactions, MGA visibility,
// security, and side effects.

enum class NativeCompileMode {
  jit,
  aot,
};

enum class NativeCompileEffectiveMode {
  interpreter,
  jit,
  aot,
  refused,
};

enum class NativeCompilePolicyProfile {
  disabled,
  jit_optional,
  jit_required_for_declared_units,
  aot_optional,
  aot_package_required,
  dev_debug_ir_export,
  invalid,
};

struct NativeCompileDescriptorDependency {
  std::string descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type_name;
  std::string encoded_descriptor;
};

struct NativeCompileRequest {
  NativeCompileMode requested_mode = NativeCompileMode::jit;
  std::string module_payload;
  std::string target_object_uuid;
  std::string principal_uuid;
  std::string database_path;
  std::string engine_abi_id = "sb_engine_abi_v3";
  std::string sblr_version = "sblr_v3";
  std::string opcode_registry_epoch = "static_v3";
  std::string numeric_backend_profile = "sbl_numeric:int128,uint128,real128";
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  bool security_context_present = false;
  bool allow_interpreter_fallback = false;
  bool simulate_backend_unavailable = false;
  bool previously_cached = false;
  scratchbird::core::memory::LlvmMemoryAccountingRequest memory_accounting;
  std::vector<NativeCompileDescriptorDependency> descriptors;
  std::vector<std::string> policy_profiles;
  std::vector<std::string> physical_profiles;
  std::vector<std::string> option_envelopes;
};

struct NativeCompileResult {
  bool ok = false;
  bool compiled = false;
  bool fallback_used = false;
  bool cache_hit = false;
  bool artifact_written = false;
  bool verifier_available = false;
  bool backend_available = false;
  bool llvm_memory_accounting_required = false;
  bool llvm_memory_reserved = false;
  bool llvm_memory_released = false;
  bool llvm_memory_test_fixture = false;
  NativeCompileEffectiveMode effective_mode = NativeCompileEffectiveMode::refused;
  NativeCompilePolicyProfile policy_profile = NativeCompilePolicyProfile::disabled;
  std::uint64_t llvm_memory_reserved_bytes = 0;
  std::uint64_t llvm_memory_reservation_count = 0;
  std::string cache_key;
  std::string artifact_path;
  std::string unit_kind;
  std::string lowerability;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string backend_provider;
  std::string llvm_version;
  std::string llvm_load_mode;
  std::string llvm_library_path;
  std::string llvm_library_path_digest;
  std::string llvm_source_root;
  std::string llvm_source_root_digest;
  std::string llvm_tools_root;
  std::string llvm_tools_root_digest;
  std::string llvm_staging_build_dir;
  std::string llvm_staging_build_dir_digest;
  std::string target_triple;
  std::string target_feature_set;
  std::string sblr_or_ir_digest;
  std::string descriptor_set_digest;
  std::string cache_key_material_digest;
  std::string provenance_hash;
  std::string artifact_digest;
  std::vector<std::string> llvm_memory_evidence;
};

NativeCompileResult CompileNativeUnit(const NativeCompileRequest& request);
bool NativeArtifactInvalidatedByDependency(const std::string& cache_key_material,
                                           const std::string& dependency_family,
                                           const std::string& dependency_value);
std::string NativeCompileEffectiveModeName(NativeCompileEffectiveMode mode);
std::string NativeCompilePolicyProfileName(NativeCompilePolicyProfile profile);

}  // namespace scratchbird::engine::native_compile
