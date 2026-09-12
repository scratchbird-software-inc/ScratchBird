// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "parser_client_types.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::parser::ipc::public_resolution_cache {

inline void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}
inline void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out->push_back(static_cast<std::uint8_t>(value >> shift));
}
inline void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    out->push_back(static_cast<std::uint8_t>(value >> shift));
}
inline void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU64(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}
inline void PutUuid(std::vector<std::uint8_t>* out,
                    const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

inline std::vector<std::uint8_t> SbpsClientPublicResolutionScopeKey(
    std::string_view endpoint, const ParserSessionContext& session) {
  // Fields are framed independently. UUIDs are the engine-issued 16 bytes;
  // textual separators and UUID spelling are not cache identity.
  std::vector<std::uint8_t> key;
  PutString(&key, endpoint);
  PutUuid(&key, session.session_uuid.bytes);
  PutUuid(&key, session.connection_uuid.bytes);
  PutUuid(&key, session.database_uuid.bytes);
  PutUuid(&key, session.authenticated_user_uuid.bytes);
  PutString(&key, session.principal_claim);
  PutString(&key, session.auth_provider_family);
  PutU64(&key, session.catalog_epoch);
  PutU64(&key, session.security_policy_epoch);
  PutU64(&key, session.grant_epoch);
  PutU64(&key, session.descriptor_epoch);
  PutU64(&key, session.localized_name_epoch);
  PutU64(&key, session.language_resource_epoch);
  PutU64(&key, session.message_resource_epoch);
  const auto append_identity_set = [&key](auto identities) {
    std::sort(identities.begin(), identities.end());
    PutU64(&key, identities.size());
    for (const auto& identity : identities) PutUuid(&key, identity.bytes);
  };
  append_identity_set(session.effective_role_uuids);
  append_identity_set(session.effective_group_uuids);
  // Search-path order changes name resolution and must never be sorted.
  PutU64(&key, session.search_path.size());
  for (const auto& schema : session.search_path) PutString(&key, schema);
  PutString(&key, session.default_language);
  PutString(&key, session.language_profile);
  PutString(&key, session.language_tag);
  PutString(&key, session.input_syntax_profile);
  PutString(&key, session.input_language_fallback_tag);
  PutString(&key, session.common_resource_hash);
  PutUuid(&key, session.dialect_profile_uuid.bytes);
  PutUuid(&key, session.policy_profile_uuid.bytes);
  PutString(&key, session.resource_compatibility_identity);
  PutString(&key, session.resource_version_identity);
  return key;
}

inline std::vector<std::uint8_t> SbpsClientResolveNameCacheKey(
    std::string_view endpoint, const ParserSessionContext& session,
    std::string_view presented_name, bool quoted, std::string_view object_class,
    const ParserClientConfig& config) {
  auto key = SbpsClientPublicResolutionScopeKey(endpoint, session);
  PutU8(&key, 1);
  PutString(&key, presented_name);
  PutU8(&key, quoted ? 1 : 0);
  PutString(&key, object_class);
  PutString(&key, config.profile_id);
  PutString(&key, config.dialect);
  PutU32(&key, config.registry_version);
  return key;
}

inline std::vector<std::uint8_t> SbpsClientRenderUuidCacheKey(
    std::string_view endpoint, const ParserSessionContext& session,
    const scratchbird::core::platform::Uuid& object_uuid) {
  auto key = SbpsClientPublicResolutionScopeKey(endpoint, session);
  PutU8(&key, 2);
  PutUuid(&key, object_uuid.bytes);
  return key;
}

} // namespace scratchbird::parser::ipc::public_resolution_cache
