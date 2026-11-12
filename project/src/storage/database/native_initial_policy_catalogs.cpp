// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_initial_policy_catalogs.hpp"
#include "catalog_security_record_codec.hpp"
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using E = NativeInitialPolicyCatalogError;
namespace c = core::catalog;

// Zero-based slots, not caller-selectable placement or authorization.
std::optional<std::size_t> DefinitionSlot(const c::CatalogCreationRecord& row) noexcept {
  const auto kind = row.metadata.record.header.kind;
  const auto& subtype = row.metadata.object_subtype;
  if (c::IsCatalogSecurityRecordKind(kind) && subtype == c::CatalogSecuritySubtype(kind))
    return 3;
  if (kind == c::CatalogRecordKind::policy &&
      (subtype == c::CatalogObjectSubtype::bootstrap_policy_pack || subtype == c::CatalogObjectSubtype::bootstrap_security_provider ||
       subtype == c::CatalogObjectSubtype::bootstrap_policy_profile)) return 4;
  if (kind == c::CatalogRecordKind::config_profile &&
      (subtype == c::CatalogObjectSubtype::memory_configuration || subtype == c::CatalogObjectSubtype::agent_runtime_authority)) return 4;
  // SB_NATIVE_POLICY_FAMILY_SEEDS: the native policy families. Policies and
  // naming profiles share role5 with the bootstrap profiles; management
  // definitions take role6.
  if (kind == c::CatalogRecordKind::policy &&
      (subtype == c::CatalogObjectSubtype::policy_descriptor || subtype == c::CatalogObjectSubtype::verification_policy_body ||
       subtype == c::CatalogObjectSubtype::evidence_policy_body || subtype == c::CatalogObjectSubtype::authorization_policy_body ||
       subtype == c::CatalogObjectSubtype::transaction_policy_body || subtype == c::CatalogObjectSubtype::lock_policy_body ||
       subtype == c::CatalogObjectSubtype::isolation_profile || subtype == c::CatalogObjectSubtype::naming_collision_policy_body)) return 4;
  if (kind == c::CatalogRecordKind::config_profile &&
      (subtype == c::CatalogObjectSubtype::identifier_resolution_profile || subtype == c::CatalogObjectSubtype::naming_dialect_profile)) return 4;
  if (kind == c::CatalogRecordKind::management_operation &&
      (subtype == c::CatalogObjectSubtype::management_profile || subtype == c::CatalogObjectSubtype::management_descriptor)) return 5;
  return std::nullopt;
}
NativeInitialPolicyCatalogResult Route(NativeInitialPolicyResult source) noexcept {
  NativeInitialPolicyCatalogResult result;
  result.source_error = source.error;
  result.name_error = source.name_error;
  result.source_diagnostic = std::move(source.source_diagnostic);
  if (!source.ok()) return result;
  try {
    std::array<std::size_t,6> counts{};
    for (const auto& row : source.records->definitions) {
      const auto slot = DefinitionSlot(row);
      if (!slot || row.name) { result.error = E::unsupported_definition; return result; }
      ++counts[*slot];
    }
    for (const auto& row : source.records->names) {
      if (row.metadata.record.header.kind != c::CatalogRecordKind::localized_name || !row.name) {
        result.error = E::invalid_name;
        return result;
      }
    }
    NativeInitialPolicyCatalogPlan plan;
    plan.encoded_bytes = source.records->encoded_bytes;
    // Reserve all fallible destination backing before transferring payloads.
    // The input source owns its complete records until these reserves succeed.
    for (std::size_t slot = 0; slot != counts.size(); ++slot)
      if (counts[slot]) plan.catalogs[slot].reserve(counts[slot]);
    for (auto& row : source.records->definitions)
      plan.catalogs[*DefinitionSlot(row)].push_back(std::move(row));
    plan.catalogs[1] = std::move(source.records->names);
    result.plan = std::move(plan);
    result.error = E::none;
    return result;
  } catch (const std::bad_alloc&) { result.error = E::resource_exhausted; }
    catch (const std::length_error&) { result.error = E::resource_exhausted; }
  return result;
}
} // namespace

NativeInitialPolicyCatalogResult AssembleNativeInitialPolicyCatalogs(
    const NativeInitialPolicyRequest& request) noexcept {
  return Route(AssembleNativeInitialPolicyRecords(request));
}
NativeInitialPolicyCatalogResult AssembleNativeInitialPolicySecurityCatalogs(
    const NativeInitialPolicySecurityRequest& request) noexcept {
  return Route(AssembleNativeInitialPolicySecurityRecords(request));
}
} // namespace scratchbird::storage::database
