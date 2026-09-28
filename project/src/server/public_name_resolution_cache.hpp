// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../core/platform/runtime_platform.hpp"
#include "../core/uuid/uuid.hpp"
#include "../wire/public_result_packet.hpp"
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::server {

// A retained lookup key, not a serialized packet or a source of catalog authority.
// Names and resource labels remain text; system identities stay native binary16.
struct ServerPublicNameResolutionCacheKey {
  core::platform::Uuid database_uuid, effective_user_uuid, dialect_profile_uuid;
  std::string presented_name, object_class, identifier_profile, language, search_path;
  bool quoted = false;
  bool stable = false;
  bool qualified = false;
  std::uint64_t catalog_generation = 0, security_epoch = 0, descriptor_epoch = 0,
      grant_epoch = 0, policy_generation = 0, name_resolution_epoch = 0,
      language_resource_epoch = 0, localized_name_epoch = 0, message_resource_epoch = 0;
  std::string role_set_hash, group_set_hash, search_path_hash, language_profile,
      language_tag, input_syntax_profile, input_language_fallback_tag,
      common_resource_hash, resource_compatibility_identity, resource_version_identity;

  auto operator<=>(const ServerPublicNameResolutionCacheKey&) const = default;

  bool HasValidIdentity() const {
    return core::uuid::IsEngineIdentityUuid(database_uuid) &&
           core::uuid::IsEngineIdentityUuid(effective_user_uuid) &&
           core::uuid::IsEngineIdentityUuid(dialect_profile_uuid) &&
           !presented_name.empty();
  }
};

// Shared by IPC lookups and post-DDL seeding. The caller must already hold the
// admitted server session; this projection does not validate or grant access.
template<class Session>
ServerPublicNameResolutionCacheKey MakeServerPublicNameResolutionCacheKey(
    const Session& session, std::string_view presented_name, bool quoted,
    const core::platform::Uuid& dialect_profile_uuid,
    std::string_view identifier_profile, std::string_view language,
    std::string_view search_path, std::string_view object_class, bool stable) {
  ServerPublicNameResolutionCacheKey key;
  key.database_uuid = session.database_uuid;
  key.effective_user_uuid = core::platform::Uuid{session.effective_user_uuid};
  key.dialect_profile_uuid = dialect_profile_uuid;
  key.presented_name = presented_name;
  key.quoted = quoted;
  key.object_class = object_class;
  key.identifier_profile = identifier_profile;
  key.language = language;
  key.stable = stable;
  key.qualified = presented_name.find('.') != std::string_view::npos;
  // Qualified stable bindings are independent of search-path changes. Absence
  // is explicit, not a magic textual value that can collide with real input.
  if (!(stable && key.qualified)) {
    key.search_path = search_path;
    key.search_path_hash = session.search_path_hash;
  }
  if (!stable) {
    key.catalog_generation = session.catalog_generation;
    key.descriptor_epoch = session.descriptor_epoch;
    key.name_resolution_epoch = session.name_resolution_epoch;
  }
  key.security_epoch = session.security_epoch;
  key.grant_epoch = session.grant_epoch;
  key.policy_generation = session.policy_generation;
  key.role_set_hash = session.role_set_hash;
  key.group_set_hash = session.group_set_hash;
  key.language_profile = session.language_profile;
  key.language_tag = session.language_tag;
  key.input_syntax_profile = session.input_syntax_profile;
  key.input_language_fallback_tag = session.input_language_fallback_tag;
  key.common_resource_hash = session.common_resource_hash;
  key.language_resource_epoch = session.language_resource_epoch;
  key.localized_name_epoch = session.localized_name_epoch;
  key.message_resource_epoch = session.message_resource_epoch;
  key.resource_compatibility_identity = session.resource_compatibility_identity;
  key.resource_version_identity = session.resource_version_identity;
  return key;
}

// Diagnostic serialization only. Never decoded to admit a cache lookup. UUIDs
// are typed 16-byte atoms, generations are u64 LE, and all text is length framed.
inline std::string PublicNameResolutionCacheKeyTrace(
    const ServerPublicNameResolutionCacheKey& key) {
  namespace packet = wire::public_result;
  std::vector<packet::Field> fields;
  auto text = [&](std::string name, const std::string& value) {
    fields.push_back({std::move(name), packet::Kind::text, value});
  };
  auto number = [&](std::string name, std::uint64_t value) {
    fields.push_back({std::move(name), packet::Kind::unsigned_integer, packet::Unsigned(value)});
  };
  auto identity = [&](std::string name, const core::platform::Uuid& value) {
    fields.push_back({std::move(name), packet::Kind::uuid,
        {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()}});
  };
  text("format", "server.public_name_resolution_key.v3");
  identity("database_uuid", key.database_uuid);
  identity("effective_user_uuid", key.effective_user_uuid);
  identity("dialect_profile_uuid", key.dialect_profile_uuid);
  text("presented_name", key.presented_name);
  text("object_class", key.object_class);
  text("identifier_profile", key.identifier_profile);
  text("language", key.language);
  text("search_path", key.search_path);
  number("quoted", key.quoted);
  number("stable", key.stable);
  number("qualified", key.qualified);
  number("catalog_generation", key.catalog_generation);
  number("security_epoch", key.security_epoch);
  number("descriptor_epoch", key.descriptor_epoch);
  number("grant_epoch", key.grant_epoch);
  number("policy_generation", key.policy_generation);
  number("name_resolution_epoch", key.name_resolution_epoch);
  number("language_resource_epoch", key.language_resource_epoch);
  number("localized_name_epoch", key.localized_name_epoch);
  number("message_resource_epoch", key.message_resource_epoch);
  text("role_set_hash", key.role_set_hash);
  text("group_set_hash", key.group_set_hash);
  text("search_path_hash", key.search_path_hash);
  text("language_profile", key.language_profile);
  text("language_tag", key.language_tag);
  text("input_syntax_profile", key.input_syntax_profile);
  text("input_language_fallback_tag", key.input_language_fallback_tag);
  text("common_resource_hash", key.common_resource_hash);
  text("resource_compatibility_identity", key.resource_compatibility_identity);
  text("resource_version_identity", key.resource_version_identity);
  std::string bytes;
  if (!packet::Encode(fields, &bytes)) return {};
  return bytes;
}
} // namespace scratchbird::server
