// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "session_registry.hpp"
#include "authority_cache_scope.hpp"
#include "../core/hash/hash_digest.hpp"

namespace scratchbird::server {

std::pair<scratchbird::core::platform::Uuid, std::uint64_t> SessionObjectHandleKey(
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t handle_id) {
  return {scratchbird::core::platform::Uuid{session_uuid}, handle_id};
}

ServerSessionObjectHandleRecord AllocateSessionObjectHandle(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    const scratchbird::core::platform::Uuid& object_uuid,
    std::string object_kind,
    std::string operation_id,
    std::string column_set_hash) {
  ServerSessionObjectHandleRecord handle;
  if (registry == nullptr || !scratchbird::core::uuid::IsEngineIdentityUuid(object_uuid) || !IsServerSessionIdentityShapeValid(session) || operation_id.empty()) {
    return handle;
  }
  for (auto& [_, existing] : registry->object_handles_by_key) {
    if (existing.session_uuid == session.session_uuid &&
        existing.object_uuid == object_uuid &&
        existing.operation_id == operation_id &&
        existing.column_set_hash ==
            (column_set_hash.empty() ? "columns/all" : column_set_hash)) {
      if (!existing.closed &&
          existing.auth_context_uuid == session.auth_context_uuid &&
          existing.principal_uuid == session.principal_uuid &&
          existing.effective_user_uuid == session.effective_user_uuid &&
          existing.database_uuid == session.database_uuid &&
          existing.catalog_generation == session.catalog_generation &&
          existing.security_epoch == session.security_epoch &&
          existing.descriptor_epoch == session.descriptor_epoch &&
          existing.grant_epoch == session.grant_epoch &&
          existing.policy_generation == session.policy_generation &&
          existing.role_set_hash == session.role_set_hash &&
          existing.group_set_hash == session.group_set_hash &&
          existing.search_path_hash == session.search_path_hash) {
        return existing;
      }
      if (existing.generation == UINT64_MAX) return {};
      auto updated = existing;
      updated.closed = false;
      ++updated.generation;
      updated.auth_context_uuid = session.auth_context_uuid;
      updated.principal_uuid = session.principal_uuid;
      updated.effective_user_uuid = session.effective_user_uuid;
      updated.database_uuid = session.database_uuid;
      if (!object_kind.empty()) {
        updated.object_kind = object_kind;
      }
      updated.catalog_generation = session.catalog_generation;
      updated.security_epoch = session.security_epoch;
      updated.descriptor_epoch = session.descriptor_epoch;
      updated.grant_epoch = session.grant_epoch;
      updated.policy_generation = session.policy_generation;
      updated.role_set_hash = session.role_set_hash;
      updated.group_set_hash = session.group_set_hash;
      updated.search_path_hash = session.search_path_hash;
      handle = updated;
      existing = std::move(updated);
      return handle;
    }
  }
  if (registry->next_session_object_handle_id == 0) return handle;
  handle.handle_id = registry->next_session_object_handle_id;
  handle.generation = 1;
  handle.session_uuid = session.session_uuid;
  handle.auth_context_uuid = session.auth_context_uuid;
  handle.principal_uuid = session.principal_uuid;
  handle.effective_user_uuid = session.effective_user_uuid;
  handle.database_uuid = session.database_uuid;
  handle.object_uuid = std::move(object_uuid);
  handle.object_kind = object_kind.empty() ? "object" : std::move(object_kind);
  handle.operation_id = std::move(operation_id);
  handle.column_set_hash = column_set_hash.empty() ? "columns/all" : std::move(column_set_hash);
  handle.catalog_generation = session.catalog_generation;
  handle.security_epoch = session.security_epoch;
  handle.descriptor_epoch = session.descriptor_epoch;
  handle.grant_epoch = session.grant_epoch;
  handle.policy_generation = session.policy_generation;
  handle.role_set_hash = session.role_set_hash;
  handle.group_set_hash = session.group_set_hash;
  handle.search_path_hash = session.search_path_hash;
  const auto inserted = registry->object_handles_by_key.emplace(
      SessionObjectHandleKey(session.session_uuid, handle.handle_id), handle);
  if (!inserted.second) return {};
  registry->next_session_object_handle_id =
      handle.handle_id == UINT64_MAX ? 0 : handle.handle_id + 1;
  return handle;
}

ServerSessionObjectHandleValidation ValidateSessionObjectHandle(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    std::uint64_t handle_id,
    std::uint64_t generation,
    const scratchbird::core::platform::Uuid& object_uuid,
    const std::string& operation_id,
    const std::string& column_set_hash) {
  ServerSessionObjectHandleValidation result;
  if (handle_id == 0 || generation == 0 ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(object_uuid) ||
      !IsServerSessionIdentityShapeValid(session)) {
    result.detail = "session_object_handle_missing";
    return result;
  }
  const auto found =
      registry.object_handles_by_key.find(SessionObjectHandleKey(session.session_uuid,
                                                                 handle_id));
  if (found == registry.object_handles_by_key.end()) {
    result.detail = "session_object_handle_not_found";
    return result;
  }
  const auto& handle = found->second;
  result.handle = &handle;
  if (handle.closed) {
    result.detail = "session_object_handle_closed";
    return result;
  }
  if (handle.generation != generation) {
    result.detail = "session_object_handle_generation_stale";
    return result;
  }
  if (handle.session_uuid != session.session_uuid ||
      handle.auth_context_uuid != session.auth_context_uuid) {
    result.detail = "session_object_handle_authority_stale";
    return result;
  }
  if (handle.principal_uuid != session.principal_uuid ||
      handle.effective_user_uuid != session.effective_user_uuid) {
    result.detail = "session_object_handle_user_stale";
    return result;
  }
  if (handle.database_uuid != session.database_uuid) {
    result.detail = "session_object_handle_database_stale";
    return result;
  }
  if (handle.object_uuid != object_uuid ||
      handle.operation_id != operation_id) {
    result.detail = "session_object_handle_shape_mismatch";
    return result;
  }
  if (handle.column_set_hash != (column_set_hash.empty() ? "columns/all" : column_set_hash)) {
    result.detail = "session_object_handle_column_hash_stale";
    return result;
  }
  if (handle.catalog_generation != session.catalog_generation ||
      handle.security_epoch != session.security_epoch ||
      handle.descriptor_epoch != session.descriptor_epoch ||
      handle.grant_epoch != session.grant_epoch ||
      handle.policy_generation != session.policy_generation) {
    result.detail = "session_object_handle_epoch_stale";
    return result;
  }
  if (handle.role_set_hash != session.role_set_hash ||
      handle.group_set_hash != session.group_set_hash ||
      handle.search_path_hash != session.search_path_hash) {
    result.detail = "session_object_handle_authorization_stale";
    return result;
  }
  result.accepted = true;
  result.detail = "session_object_handle_valid";
  return result;
}

void CloseSessionObjectHandlesForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string) {
  if (registry == nullptr) return;
  for (auto& [_, handle] : registry->object_handles_by_key) {
    if (handle.session_uuid == session_uuid && !handle.closed) {
      handle.closed = true;
      if (handle.generation != UINT64_MAX) ++handle.generation;
    }
  }
}

namespace {

ServerAuthorityCacheEpochVector AuthorityCacheEpochVectorForSession(
    const ServerSessionRecord& session) {
  ServerAuthorityCacheEpochVector vector;
  vector.catalog_generation = session.catalog_generation;
  vector.security_epoch = session.security_epoch;
  vector.descriptor_epoch = session.descriptor_epoch;
  vector.grant_epoch = session.grant_epoch;
  vector.policy_generation = session.policy_generation;
  vector.capability_policy_generation = session.capability_policy_generation;
  vector.cache_invalidation_epoch = session.cache_invalidation_epoch;
  vector.name_resolution_epoch = session.name_resolution_epoch;
  vector.resource_epoch = session.resource_epoch;
  vector.role_set_hash = session.role_set_hash;
  vector.group_set_hash = session.group_set_hash;
  vector.search_path_hash = session.search_path_hash;
  return vector;
}

bool AuthorityCacheEpochNumbersMatch(
    const ServerAuthorityCacheEpochVector& cached,
    const ServerAuthorityCacheEpochVector& current) {
  return cached.catalog_generation == current.catalog_generation &&
         cached.security_epoch == current.security_epoch &&
         cached.descriptor_epoch == current.descriptor_epoch &&
         cached.grant_epoch == current.grant_epoch &&
         cached.policy_generation == current.policy_generation &&
         cached.capability_policy_generation ==
             current.capability_policy_generation &&
         cached.cache_invalidation_epoch == current.cache_invalidation_epoch &&
         cached.name_resolution_epoch == current.name_resolution_epoch &&
         cached.resource_epoch == current.resource_epoch;
}

bool AuthorityCacheAuthorizationHashesMatch(
    const ServerAuthorityCacheEpochVector& cached,
    const ServerAuthorityCacheEpochVector& current) {
  return cached.role_set_hash == current.role_set_hash &&
         cached.group_set_hash == current.group_set_hash &&
         cached.search_path_hash == current.search_path_hash;
}


}  // namespace

std::string ServerAuthorityCacheKey(const std::string& cache_kind,
                                    const ServerSessionRecord& session,
                                    const std::string& operation_id,
                                    const scratchbird::core::platform::Uuid& target_object_uuid,
                                    const std::string& statement_shape_hash) {
  const auto payload = EncodeServerAuthorityCacheScope(
      cache_kind, session, operation_id, target_object_uuid, statement_shape_hash);
  if (!payload) return {};
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(payload->data()),
      payload->size());
  if (!digest.ok()) return {};
  return cache_kind + ":sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

ServerAuthorityCacheRecord StoreServerAuthorityCacheDecision(
    ServerSessionRegistry* registry,
    const ServerSessionRecord& session,
    std::string cache_kind,
    std::string operation_id,
    const scratchbird::core::platform::Uuid& target_object_uuid,
    std::string statement_shape_hash,
    std::string diagnostic_code,
    std::string diagnostic_detail,
    bool refusal) {
  ServerAuthorityCacheRecord record;
  record.cache_key = ServerAuthorityCacheKey(cache_kind,
                                             session,
                                             operation_id,
                                             target_object_uuid,
                                             statement_shape_hash);
  record.cache_kind = std::move(cache_kind);
  record.session_uuid = session.session_uuid;
  record.auth_context_uuid = session.auth_context_uuid;
  record.principal_uuid = session.principal_uuid;
  record.effective_user_uuid = session.effective_user_uuid;
  record.database_uuid = session.database_uuid;
  record.operation_id = std::move(operation_id);
  record.target_object_uuid = std::move(target_object_uuid);
  record.statement_shape_hash = std::move(statement_shape_hash);
  record.epoch_vector = AuthorityCacheEpochVectorForSession(session);
  record.diagnostic_code = std::move(diagnostic_code);
  record.diagnostic_detail = std::move(diagnostic_detail);
  record.refusal = refusal;
  record.grants_authority = false;
  if (registry != nullptr && !record.cache_key.empty()) {
    const auto existing = registry->authority_cache_by_key.find(record.cache_key);
    if (existing != registry->authority_cache_by_key.end()) {
      record.generation = existing->second.generation;
      record.hit_count = existing->second.hit_count;
      auto published = record;
      std::swap(existing->second, published);
    } else {
      if (registry->next_authority_cache_generation == 0) return {};
      record.generation = registry->next_authority_cache_generation;
      registry->authority_cache_by_key.emplace(record.cache_key, record);
      registry->next_authority_cache_generation =
          record.generation == UINT64_MAX ? 0 : record.generation + 1;
    }
  }
  return record;
}

ServerAuthorityCacheValidation ValidateServerAuthorityCacheEntry(
    const ServerSessionRegistry& registry,
    const ServerSessionRecord& session,
    const std::string& cache_key,
    const std::string& cache_kind,
    const std::string& operation_id,
    const scratchbird::core::platform::Uuid& target_object_uuid,
    const std::string& statement_shape_hash) {
  ServerAuthorityCacheValidation validation;
  if (!IsServerSessionIdentityShapeValid(session) || cache_kind.empty() ||
      operation_id.empty() || (!target_object_uuid.is_nil() &&
      !scratchbird::core::uuid::IsEngineIdentityUuid(target_object_uuid))) {
    validation.detail = "authority_cache_scope_invalid";
    return validation;
  }
  const auto found = registry.authority_cache_by_key.find(cache_key);
  if (found == registry.authority_cache_by_key.end()) {
    validation.detail = "authority_cache_not_found";
    return validation;
  }
  const auto& record = found->second;
  validation.record = &record;
  if (record.cache_kind != cache_kind) {
    validation.detail = "authority_cache_kind_mismatch";
    return validation;
  }
  if (record.generation == 0) {
    validation.detail = "authority_cache_generation_invalid";
    return validation;
  }
  if (record.grants_authority) {
    validation.grants_authority = true;
    validation.detail = "authority_cache_grant_forbidden";
    return validation;
  }
  if (cache_kind == "negative_authorization" && !record.refusal) {
    validation.detail = "negative_authorization_cache_must_refuse";
    return validation;
  }
  if (record.session_uuid != session.session_uuid) {
    validation.cross_session = true;
    validation.detail = "authority_cache_cross_session";
    return validation;
  }
  if (record.auth_context_uuid != session.auth_context_uuid) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_auth_context_stale";
    return validation;
  }
  if (record.principal_uuid != session.principal_uuid ||
      record.effective_user_uuid != session.effective_user_uuid) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_cross_user";
    return validation;
  }
  if (record.database_uuid != session.database_uuid) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_cross_database";
    return validation;
  }
  if (record.operation_id != operation_id) {
    validation.detail = "authority_cache_operation_mismatch";
    return validation;
  }
  if (record.target_object_uuid != target_object_uuid) {
    validation.detail = "authority_cache_target_mismatch";
    return validation;
  }
  if (record.statement_shape_hash != statement_shape_hash) {
    validation.detail = "authority_cache_statement_shape_mismatch";
    return validation;
  }
  const auto current = AuthorityCacheEpochVectorForSession(session);
  if (!AuthorityCacheEpochNumbersMatch(record.epoch_vector, current)) {
    validation.stale = true;
    validation.detail = "authority_cache_epoch_stale";
    return validation;
  }
  if (!AuthorityCacheAuthorizationHashesMatch(record.epoch_vector, current)) {
    validation.cross_authorization = true;
    validation.detail = "authority_cache_authorization_hash_stale";
    return validation;
  }
  validation.accepted = true;
  validation.detail = "authority_cache_valid";
  return validation;
}

bool MarkServerAuthorityCacheHit(ServerSessionRegistry* registry,
                                 const std::string& cache_key) {
  if (registry == nullptr) return false;
  auto found = registry->authority_cache_by_key.find(cache_key);
  if (found == registry->authority_cache_by_key.end()) return false;
  if (found->second.hit_count == UINT64_MAX) return false;
  ++found->second.hit_count;
  return true;
}


}  // namespace scratchbird::server
