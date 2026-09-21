// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "session_registry.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::server {

inline bool IsServerSessionIdentityShapeValid(const ServerSessionRecord& session) noexcept {
  using scratchbird::core::platform::Uuid;
  using scratchbird::core::uuid::IsEngineIdentityUuid;
  return IsEngineIdentityUuid(Uuid{session.session_uuid}) &&
         IsEngineIdentityUuid(Uuid{session.auth_context_uuid}) &&
         IsEngineIdentityUuid(Uuid{session.principal_uuid}) &&
         IsEngineIdentityUuid(Uuid{session.effective_user_uuid}) &&
         IsEngineIdentityUuid(session.database_uuid);
}

// Derivative lookup material only. No digest or cache hit grants authority.
// No output escapes until every field is encoded; allocation failures propagate.
inline std::optional<std::string> EncodeServerAuthorityCacheScope(
    std::string_view cache_kind, const ServerSessionRecord& session,
    std::string_view operation_id,
    const scratchbird::core::platform::Uuid& target_object_uuid,
    std::string_view statement_shape_hash) {
  using scratchbird::core::platform::Uuid;
  using scratchbird::core::uuid::IsEngineIdentityUuid;
  if (cache_kind.empty() || operation_id.empty() ||
      !IsServerSessionIdentityShapeValid(session) ||
      (!target_object_uuid.is_nil() && !IsEngineIdentityUuid(target_object_uuid)))
    return std::nullopt;
  std::string bytes("SBACKEY2", 8);
  const auto number = [&](std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
      bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  };
  const auto field = [&](std::string_view value) {
    number(static_cast<std::uint64_t>(value.size()));
    bytes.append(value);
  };
  const auto identity = [&](const std::array<std::uint8_t, 16>& value) {
    bytes.append(reinterpret_cast<const char*>(value.data()), value.size());
  };
  field(cache_kind);
  field(operation_id);
  field(statement_shape_hash);
  identity(session.session_uuid);
  identity(session.auth_context_uuid);
  identity(session.principal_uuid);
  identity(session.effective_user_uuid);
  identity(session.database_uuid.bytes);
  identity(target_object_uuid.bytes);
  number(session.catalog_generation);
  number(session.security_epoch);
  number(session.descriptor_epoch);
  number(session.grant_epoch);
  number(session.policy_generation);
  number(session.capability_policy_generation);
  number(session.cache_invalidation_epoch);
  number(session.name_resolution_epoch);
  number(session.resource_epoch);
  field(session.role_set_hash);
  field(session.group_set_hash);
  field(session.search_path_hash);
  return bytes;
}

}  // namespace scratchbird::server
