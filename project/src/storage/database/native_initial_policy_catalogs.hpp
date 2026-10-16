// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_policy_creation.hpp"
#include "native_creation_workspace.hpp"

namespace scratchbird::storage::database {

enum class NativeInitialPolicyCatalogError {
  none, source_failure, unsupported_definition, invalid_name, resource_exhausted
};
struct NativeInitialPolicyCatalogPlan {
  NativeCreationCatalogSeed catalogs;
  core::platform::u64 encoded_bytes = 0;
};
struct NativeInitialPolicyCatalogResult {
  NativeInitialPolicyCatalogError error = NativeInitialPolicyCatalogError::source_failure;
  NativeInitialPolicyError source_error = NativeInitialPolicyError::invalid_request;
  core::catalog::CatalogNamedCreationError name_error =
      core::catalog::CatalogNamedCreationError::none;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::optional<NativeInitialPolicyCatalogPlan> plan;
  bool ok() const noexcept {
    return error == NativeInitialPolicyCatalogError::none && plan.has_value();
  }
};

// Initial selected-pack declaration placement, not publication, activation,
// authentication, allocation or reference-closure authority. Invoke outside
// source/device guards with admitted input/output/scratch backing retained.
// These use the same complete admitted-source producers as ordinary assembly;
// no public mutable record collection can bypass their validation here.
// Names go to native role2, security definitions to role4, bootstrap provider/
// policy/memory/runtime configuration to role5. Roles1/3/6 remain untouched.
// Unknown future families refuse atomically, never default to the object head.
// Owning payloads transfer without copies; all source/placement errors retain
// their typed causes and return no partial plan or credential diagnostics.
NativeInitialPolicyCatalogResult AssembleNativeInitialPolicyCatalogs(
    const NativeInitialPolicyRequest&) noexcept;
NativeInitialPolicyCatalogResult AssembleNativeInitialPolicySecurityCatalogs(
    const NativeInitialPolicySecurityRequest&) noexcept;

} // namespace scratchbird::storage::database
