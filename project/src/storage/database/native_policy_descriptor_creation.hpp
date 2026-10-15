// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_policy_descriptor.hpp"
#include "catalog_named_creation.hpp"

namespace scratchbird::storage::database {
struct NativeInitialPolicyDescriptorInput {
  const core::catalog::CatalogPolicyDescriptor* descriptor=nullptr;
  core::catalog::CatalogNamedCreationInput named;
};
struct NativeInitialPolicyDescriptorRequest {
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  std::string_view database_default_language;
  std::span<const NativeInitialPolicyDescriptorInput> policies;
  core::catalog::CatalogNamedCreationLimits limits;
};
enum class NativeInitialPolicyDescriptorError {
  none,invalid_request,invalid_definition,invalid_names,duplicate_identity,resource_exhausted
};
struct NativeInitialPolicyDescriptorResult {
  NativeInitialPolicyDescriptorError error=NativeInitialPolicyDescriptorError::invalid_request;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  std::optional<core::catalog::CatalogNamedCreationSet> records;
  bool ok()const noexcept{return error==NativeInitialPolicyDescriptorError::none&&records.has_value();}
};
// Actual supplied typed source, never an area/mode label or a policy-pack count.
// Complete borrowed validation precedes owning copies. Retain immutable source,
// real input/output/scratch grants and all external admissions throughout entry.
// Limits are not grants. No I/O, reference invention, class-body synthesis,
// authorization, activation or construction-success receipt. Never under guards.
NativeInitialPolicyDescriptorResult AssembleNativeInitialPolicyDescriptorRecords(
    const NativeInitialPolicyDescriptorRequest&) noexcept;
} // namespace scratchbird::storage::database
