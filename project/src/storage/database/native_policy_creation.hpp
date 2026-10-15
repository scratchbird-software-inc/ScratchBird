// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_named_creation.hpp"

namespace scratchbird::storage::database {
enum class NativeInitialPolicyError {
  none, invalid_request, source_invalid, invalid_definition, invalid_names,
  duplicate_identity, resource_exhausted
};
struct NativeInitialPolicyRequest {
  std::string_view policy_seed_pack_root;
  core::platform::Uuid database_uuid;
  core::platform::TypedUuid construction_transaction;
  // Actual owner-issued identity of the materialized memory configuration.
  core::platform::Uuid memory_configuration_uuid;
  std::string_view database_default_language;
  // Exact complete metadata/names, with empty definition payloads. The source
  // pack supplies identities/typed values, never owner/audit/name authority.
  std::span<const core::catalog::CatalogNamedCreationInput> definitions;
  core::catalog::CatalogNamedCreationLimits limits;
};
struct NativeInitialPolicyResult {
  NativeInitialPolicyError error=NativeInitialPolicyError::invalid_request;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  std::optional<core::platform::DiagnosticRecord> source_diagnostic;
  std::optional<core::catalog::CatalogNamedCreationSet> records;
  bool ok() const noexcept {return error==NativeInitialPolicyError::none&&records.has_value();}
};
// Owning creation-plan producer for all imported pack/provider/profile/memory
// definitions and their actual supplied aliases. Consumes exact hash-admitted
// source bytes and the same typed translators as production creation, not
// legacy catalog rows or a prototype schema cache. No physical I/O except pack
// intake; no partial package on refusal. Caller retains actual source/input/
// output/scratch memory and admits the referenced namespaces/governance/profiles.
// These limits are not grants. This is not the complete node/security/grant
// cohort, authentication, activation or serving success. Never enter under a
// storage guard: source intake and owning result assembly can allocate.
NativeInitialPolicyResult AssembleNativeInitialPolicyRecords(const NativeInitialPolicyRequest&) noexcept;
} // namespace scratchbird::storage::database
