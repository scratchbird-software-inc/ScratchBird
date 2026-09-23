// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sblr_engine_envelope.hpp"
#include "core/uuid/uuid.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <limits>
#include <new>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::array<std::string_view, 16> kBoundObjectIdentityRoles{
    "target_object_uuid", "source_uuid", "routine_object_uuid",
    "insert_select_source_uuid_0", "insert_select_source_uuid_1", "target_schema_uuid",
    "grantee_uuid", "member_principal_uuid", "container_uuid",
    "principal_uuid", "policy_uuid", "role_uuid", "group_uuid",
    "mask_uuid", "rls_uuid", "definer_principal_uuid"};

inline constexpr std::array<std::string_view, 14> kLegacyCatalogIdentityAliases{
    "index_target_uuid", "statistics_target_uuid", "target_table_uuid",
    "index_object_uuid", "index_template_object_uuid", "object_uuid", "schema_uuid",
    "function_object_uuid", "procedure_object_uuid", "trigger_object_uuid", "target_filespace_uuid",
    "owner_object_uuid", "grant_uuid", "membership_uuid"};

inline constexpr bool IsLegacyCatalogIdentityAlias(std::string_view role) noexcept {
  return std::find(kLegacyCatalogIdentityAliases.begin(), kLegacyCatalogIdentityAliases.end(), role) !=
      kLegacyCatalogIdentityAliases.end();
}

inline constexpr bool IsBoundObjectIdentityRole(std::string_view role) noexcept {
  return std::find(kBoundObjectIdentityRoles.begin(), kBoundObjectIdentityRoles.end(), role) !=
      kBoundObjectIdentityRoles.end();
}

inline constexpr bool IsRelatedObjectIdentityRole(std::string_view role) noexcept {
  return role.starts_with("related_object_") && role.ends_with("_uuid");
}

inline bool DecodeRelatedObjectIdentityIndex(std::string_view role, std::size_t* index) noexcept {
  if (!index || !IsRelatedObjectIdentityRole(role) || role.size() < 21) return false;
  const auto digits = role.substr(15, role.size() - 20);
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
  std::size_t decoded = 0;
  for (const char ch : digits) {
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::size_t>(ch - '0');
    if (decoded > (std::numeric_limits<std::size_t>::max() - digit) / 10) return false;
    decoded = decoded * 10 + digit;
  }
  *index = decoded;
  return true;
}

struct SblrBoundObjectIdentities {
  std::array<std::optional<core::platform::Uuid>, kBoundObjectIdentityRoles.size()> values;

  std::optional<core::platform::Uuid> Find(std::string_view role) const noexcept {
    for (std::size_t i = 0; i < kBoundObjectIdentityRoles.size(); ++i) {
      if (kBoundObjectIdentityRoles[i] == role) return values[i];
    }
    return std::nullopt;
  }
};

inline constexpr bool IsProjectionFunctionIdentityRole(std::string_view role) noexcept {
  return role.starts_with("projection_") && role.ends_with("function_uuid");
}

inline bool IsProjectionFunctionIdentityPath(std::string_view prefix) noexcept {
  if (!prefix.starts_with("projection_")) return false;
  auto remaining = prefix.substr(11);
  while (true) {
    const auto delimiter = remaining.find('_');
    if (delimiter == std::string_view::npos || delimiter == 0) return false;
    const auto digits = remaining.substr(0, delimiter);
    if (digits.size() > 1 && digits.front() == '0') return false;
    std::size_t index = 0;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9' ||
          index > (std::numeric_limits<std::size_t>::max() - (digit - '0')) / 10)
        return false;
      index = index * 10 + static_cast<std::size_t>(digit - '0');
    }
    remaining.remove_prefix(delimiter + 1);
    if (remaining.empty()) return true;
    if (!remaining.starts_with("arg_")) return false;
    remaining.remove_prefix(4);
  }
}

inline bool DecodeProjectionFunctionIdentityPath(
    std::string_view role, std::string_view* path) noexcept {
  if (!path || !IsProjectionFunctionIdentityRole(role)) return false;
  const auto prefix = role.substr(0, role.size() - 13);
  if (!IsProjectionFunctionIdentityPath(prefix)) return false;
  *path = prefix;
  return true;
}

// Structural projection only: catalog visibility, receipt ownership, access
// rights and transaction validity still require their actual engine owners.
inline bool DecodeSblrBoundObjectIdentities(
    const SblrOperationEnvelope& envelope, SblrBoundObjectIdentities* output) noexcept {
  if (!output) return false;
  SblrBoundObjectIdentities decoded;
  for (std::size_t ordinal = 0; ordinal < envelope.operands.size(); ++ordinal) {
    const auto& operand = envelope.operands[ordinal];
    if (IsLegacyCatalogIdentityAlias(operand.name)) return false;
    const auto role = std::find(kBoundObjectIdentityRoles.begin(),
                                kBoundObjectIdentityRoles.end(), operand.name);
    if (role == kBoundObjectIdentityRoles.end()) continue;
    auto& identity = decoded.values[static_cast<std::size_t>(role - kBoundObjectIdentityRoles.begin())];
    if (identity || operand.ordinal != ordinal + 1 || operand.type != "uuid" ||
        !operand.value.empty() || operand.value_kind != SblrValueKind::uuid_ref ||
        operand.value_flags != 0 || operand.value_body.size() != 16) return false;
    core::platform::Uuid value;
    std::copy(operand.value_body.begin(), operand.value_body.end(), value.bytes.begin());
    if (!core::uuid::IsEngineIdentityUuid(value)) return false;
    identity = value;
  }
  *output = decoded;
  return true;
}

enum class SblrIdentityProjectionFailure { none, invalid_operand, allocation_failed };

enum class SblrSecurityDclKind { privilege, membership };

struct SblrSecurityDclIdentities {
  core::platform::Uuid target_object_uuid;
  core::platform::Uuid grantee_uuid;
  core::platform::Uuid member_principal_uuid;
  core::platform::Uuid container_uuid;
  bool operator==(const SblrSecurityDclIdentities&) const = default;
};

// This is structural admission, not privilege authorization. Grant/membership
// record identities are allocated by the catalog owner, never copied from a
// principal/container or accepted as untyped options.
inline bool DecodeSblrSecurityDclIdentities(
    const SblrOperationEnvelope& envelope, SblrSecurityDclKind kind,
    SblrSecurityDclIdentities* output) noexcept {
  if (!output || (kind != SblrSecurityDclKind::privilege &&
                  kind != SblrSecurityDclKind::membership)) return false;
  SblrBoundObjectIdentities identities;
  if (!DecodeSblrBoundObjectIdentities(envelope, &identities)) return false;
  if (std::count_if(identities.values.begin(), identities.values.end(),
                   [](const auto& value) { return value.has_value(); }) != 2)
    return false;
  for (const auto& operand : envelope.operands)
    if (IsRelatedObjectIdentityRole(operand.name) ||
        IsProjectionFunctionIdentityRole(operand.name)) return false;
  SblrSecurityDclIdentities decoded;
  const auto first = identities.Find(kind == SblrSecurityDclKind::privilege
      ? "target_object_uuid" : "member_principal_uuid");
  const auto second = identities.Find(kind == SblrSecurityDclKind::privilege
      ? "grantee_uuid" : "container_uuid");
  if (!first || !second) return false;
  if (kind == SblrSecurityDclKind::privilege) {
    decoded.target_object_uuid = *first;
    decoded.grantee_uuid = *second;
  } else {
    decoded.member_principal_uuid = *first;
    decoded.container_uuid = *second;
  }
  *output = decoded;
  return true;
}

inline bool ProjectSblrBoundTargetIdentity(
    const SblrOperationEnvelope& envelope, internal_api::EngineApiRequest* request,
    SblrIdentityProjectionFailure* failure = nullptr) noexcept try {
  if (failure) *failure = SblrIdentityProjectionFailure::invalid_operand;
  if (!request) return false;
  SblrBoundObjectIdentities identities;
  if (!DecodeSblrBoundObjectIdentities(envelope, &identities)) return false;
  // A legacy option string is never a fallback or a second identity authority,
  // even when a valid binary operand is present alongside it.
  for (const auto& option : request->option_envelopes) {
    const auto name = std::string_view(option).substr(0, option.find(':'));
    if (IsLegacyCatalogIdentityAlias(name) || IsRelatedObjectIdentityRole(name) ||
        IsBoundObjectIdentityRole(name) || IsProjectionFunctionIdentityRole(name))
      return false;
  }
  const auto identity = identities.Find("target_object_uuid");
  const auto schema = identities.Find("target_schema_uuid");
  const auto& bound = request->target_object.uuid;
  if (!bound.is_nil() && (!core::uuid::IsEngineIdentityUuid(bound) ||
                         (identity && *identity != bound))) return false;
  const auto& bound_schema = request->target_schema.uuid;
  if (!bound_schema.is_nil() && (!core::uuid::IsEngineIdentityUuid(bound_schema) ||
                               (schema && *schema != bound_schema))) return false;
  for (const auto& related : request->related_objects)
    if (!core::uuid::IsEngineIdentityUuid(related.uuid)) return false;
  const auto related_count = static_cast<std::size_t>(std::count_if(
      envelope.operands.begin(), envelope.operands.end(), [](const auto& operand) {
        return IsRelatedObjectIdentityRole(operand.name);
      }));
  std::vector<internal_api::EngineObjectReference> related;
  if (related_count != 0) {
    if (related_count > related.max_size()) return false;
    if (!request->related_objects.empty() && request->related_objects.size() != related_count)
      return false;
    related = request->related_objects;
    related.resize(related_count);
    std::vector<bool> seen(related_count);
    for (std::size_t ordinal = 0; ordinal < envelope.operands.size(); ++ordinal) {
      const auto& operand = envelope.operands[ordinal];
      if (!IsRelatedObjectIdentityRole(operand.name)) continue;
      std::size_t index = 0;
      if (!DecodeRelatedObjectIdentityIndex(operand.name, &index) || index >= related_count ||
          seen[index] || operand.ordinal != ordinal + 1 || operand.type != "uuid" ||
          !operand.value.empty() || operand.value_kind != SblrValueKind::uuid_ref ||
          operand.value_flags != 0 || operand.value_body.size() != 16) return false;
      core::platform::Uuid value;
      std::copy(operand.value_body.begin(), operand.value_body.end(), value.bytes.begin());
      if (!core::uuid::IsEngineIdentityUuid(value) ||
          (!related[index].uuid.is_nil() && related[index].uuid != value)) return false;
      related[index].uuid = value;
      seen[index] = true;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end()) return false;
  }
  const auto function_count = static_cast<std::size_t>(std::count_if(
      envelope.operands.begin(), envelope.operands.end(), [](const auto& operand) {
        return IsProjectionFunctionIdentityRole(operand.name);
      }));
  std::vector<std::pair<std::string, core::platform::Uuid>> functions;
  const auto& bound_functions = request->projection.function_identities;
  for (std::size_t i = 0; i < bound_functions.size(); ++i) {
    const auto& binding = bound_functions[i];
    if (!IsProjectionFunctionIdentityPath(binding.first) ||
        !core::uuid::IsEngineIdentityUuid(binding.second)) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (bound_functions[j].first == binding.first) return false;
  }
  if (function_count != 0) {
    if (!bound_functions.empty() && bound_functions.size() != function_count) return false;
    functions.reserve(function_count);
    for (std::size_t ordinal = 0; ordinal < envelope.operands.size(); ++ordinal) {
      const auto& operand = envelope.operands[ordinal];
      if (!IsProjectionFunctionIdentityRole(operand.name)) continue;
      std::string_view path;
      if (!DecodeProjectionFunctionIdentityPath(operand.name, &path) ||
          operand.ordinal != ordinal + 1 || operand.type != "uuid" ||
          !operand.value.empty() || operand.value_kind != SblrValueKind::uuid_ref ||
          operand.value_flags != 0 || operand.value_body.size() != 16) return false;
      core::platform::Uuid value;
      std::copy(operand.value_body.begin(), operand.value_body.end(), value.bytes.begin());
      if (!core::uuid::IsEngineIdentityUuid(value) ||
          std::any_of(functions.begin(), functions.end(), [&](const auto& binding) {
            return binding.first == path;
          })) return false;
      if (!bound_functions.empty() && std::none_of(bound_functions.begin(), bound_functions.end(),
          [&](const auto& binding) { return binding.first == path && binding.second == value; }))
        return false;
      functions.emplace_back(path, value);
    }
  }
  // Publish the complete object/schema/related/function cohort only after validation
  // and allocation. No conflicting suffix may leave a newly assigned target.
  if (identity) request->target_object.uuid = *identity;
  if (schema) request->target_schema.uuid = *schema;
  if (related_count != 0) request->related_objects.swap(related);
  if (function_count != 0) request->projection.function_identities.swap(functions);
  if (failure) *failure = SblrIdentityProjectionFailure::none;
  return true;
} catch (const std::bad_alloc&) {
  if (failure) *failure = SblrIdentityProjectionFailure::allocation_failed;
  return false;
}

} // namespace scratchbird::engine::sblr
