// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_verification_policy.hpp"
#include "native_policy_descriptor_creation.hpp"

namespace scratchbird::storage::database {
struct NativeInitialVerificationPolicyInput {
  const core::catalog::CatalogVerificationPolicyBody* body=nullptr;
  core::catalog::CatalogNamedCreationInput named;
};
struct NativeInitialVerificationPolicyRequest {
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  core::platform::Uuid database_default_language;  // a language constant (SB_CATALOG_LANGUAGE_CONSTANTS)
  std::span<const NativeInitialVerificationPolicyInput> policies;
  core::catalog::CatalogNamedCreationLimits limits;
};
// Complete supplied typed bodies and their names, using the common creation
// assembler and budgets. No imported label may stand in for executable rules.
// Owning caller retains actual source/output/scratch grants outside device
// guards and resolves evidence/access-policy references in the complete cohort.
// This produces declarations only; no activation, reference or readiness proof.
NativeInitialPolicyDescriptorResult AssembleNativeInitialVerificationPolicyRecords(
    const NativeInitialVerificationPolicyRequest&) noexcept;
} // namespace scratchbird::storage::database
