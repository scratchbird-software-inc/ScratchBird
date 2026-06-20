// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_protocol_support.hpp"

#include "security/security_crypto_policy.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string_view>
#include <tuple>

namespace scratchbird::server {
namespace {

namespace engine_api = scratchbird::engine::internal_api;

std::string Digest(std::string_view payload) {
  const std::string digest = engine_api::SecuritySha256Hex(payload);
  return digest.empty() ? std::string{} : "sha256:" + digest;
}

std::string UuidBytesPayload(const std::array<std::uint8_t, 16>& value) {
  std::ostringstream out;
  out << std::hex;
  for (const std::uint8_t byte : value) {
    if (byte < 16) out << '0';
    out << static_cast<unsigned int>(byte);
  }
  return out.str();
}

void AppendField(std::ostringstream* out, std::string_view name,
                 std::string_view value) {
  (*out) << name << '=' << value << '\n';
}

void AppendField(std::ostringstream* out, std::string_view name,
                 std::uint64_t value) {
  (*out) << name << '=' << value << '\n';
}

void AppendScopePayload(std::ostringstream* out,
                        const IparSupportSessionScope& scope) {
  AppendField(out, "session_uuid", UuidBytesPayload(scope.session_uuid));
  AppendField(out, "auth_context_uuid", UuidBytesPayload(scope.auth_context_uuid));
  AppendField(out, "principal_uuid", UuidBytesPayload(scope.principal_uuid));
  AppendField(out, "effective_user_uuid",
              UuidBytesPayload(scope.effective_user_uuid));
  AppendField(out, "database_uuid", scope.database_uuid);
  AppendField(out, "catalog_generation", scope.epoch.catalog_generation);
  AppendField(out, "security_epoch", scope.epoch.security_epoch);
  AppendField(out, "descriptor_epoch", scope.epoch.descriptor_epoch);
  AppendField(out, "grant_epoch", scope.epoch.grant_epoch);
  AppendField(out, "policy_generation", scope.epoch.policy_generation);
  AppendField(out, "capability_policy_generation",
              scope.epoch.capability_policy_generation);
  AppendField(out, "cache_invalidation_epoch",
              scope.epoch.cache_invalidation_epoch);
  AppendField(out, "name_resolution_epoch",
              scope.epoch.name_resolution_epoch);
  AppendField(out, "resource_epoch", scope.epoch.resource_epoch);
  AppendField(out, "role_set_hash", scope.epoch.role_set_hash);
  AppendField(out, "group_set_hash", scope.epoch.group_set_hash);
  AppendField(out, "search_path_hash", scope.epoch.search_path_hash);
}

std::string ScopeDigestPayload(const IparSupportSessionScope& scope) {
  std::ostringstream out;
  AppendScopePayload(&out, scope);
  return out.str();
}

std::string LowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return lowered;
}

bool IsHex(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

bool LooksLikeCanonicalUuidText(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
      continue;
    }
    if (!IsHex(value[i])) return false;
  }
  return true;
}

bool LooksLikeSqlAuthorityText(std::string_view payload) {
  const std::string lowered = LowerAscii(payload);
  return lowered.find("sql_text=") != std::string::npos ||
         lowered.find("\"sql_text\"") != std::string::npos ||
         lowered.find("raw_sql=") != std::string::npos ||
         lowered.find("select * from") != std::string::npos ||
         lowered.find("parser_executes_sql=true") != std::string::npos;
}

std::string DependencyPayload(std::vector<IparUuidDependency> dependencies) {
  std::sort(dependencies.begin(), dependencies.end(),
            [](const IparUuidDependency& lhs,
               const IparUuidDependency& rhs) {
              return std::tie(lhs.object_uuid, lhs.descriptor_uuid,
                              lhs.dependency_kind, lhs.logical_name) <
                     std::tie(rhs.object_uuid, rhs.descriptor_uuid,
                              rhs.dependency_kind, rhs.logical_name);
            });
  std::ostringstream out;
  for (const IparUuidDependency& dep : dependencies) {
    AppendField(&out, "kind", dep.dependency_kind);
    AppendField(&out, "logical_name", dep.logical_name);
    AppendField(&out, "object_uuid", dep.object_uuid);
    AppendField(&out, "descriptor_uuid", dep.descriptor_uuid);
    AppendField(&out, "descriptor_hash", dep.descriptor_hash);
    AppendField(&out, "catalog_generation", dep.catalog_generation);
    AppendField(&out, "descriptor_epoch", dep.descriptor_epoch);
    AppendField(&out, "name_resolution_epoch", dep.name_resolution_epoch);
  }
  return out.str();
}

bool DependenciesValid(const std::vector<IparUuidDependency>& dependencies,
                       const IparSupportSessionScope& scope,
                       std::string* detail) {
  if (dependencies.empty()) {
    if (detail != nullptr) *detail = "ipar_template_uuid_dependency_required";
    return false;
  }
  for (const IparUuidDependency& dep : dependencies) {
    if (!LooksLikeCanonicalUuidText(dep.object_uuid) ||
        !LooksLikeCanonicalUuidText(dep.descriptor_uuid)) {
      if (detail != nullptr) *detail = "ipar_template_uuid_dependency_invalid";
      return false;
    }
    if (dep.descriptor_hash.empty()) {
      if (detail != nullptr) *detail = "ipar_template_descriptor_hash_required";
      return false;
    }
    if (dep.catalog_generation != scope.epoch.catalog_generation ||
        dep.descriptor_epoch != scope.epoch.descriptor_epoch ||
        dep.name_resolution_epoch != scope.epoch.name_resolution_epoch) {
      if (detail != nullptr) *detail = "ipar_template_dependency_epoch_stale";
      return false;
    }
    const std::string lowered_kind = LowerAscii(dep.dependency_kind);
    if (lowered_kind.find("engine_resolves_from_catalog") != std::string::npos ||
        lowered_kind.find("public_registry") != std::string::npos) {
      if (detail != nullptr) *detail = "ipar_template_dependency_authority_forbidden";
      return false;
    }
  }
  return true;
}

bool ScopeNumbersMatch(const IparSupportSessionScope& lhs,
                       const IparSupportSessionScope& rhs) {
  return lhs.epoch.catalog_generation == rhs.epoch.catalog_generation &&
         lhs.epoch.security_epoch == rhs.epoch.security_epoch &&
         lhs.epoch.descriptor_epoch == rhs.epoch.descriptor_epoch &&
         lhs.epoch.grant_epoch == rhs.epoch.grant_epoch &&
         lhs.epoch.policy_generation == rhs.epoch.policy_generation &&
         lhs.epoch.cache_invalidation_epoch ==
             rhs.epoch.cache_invalidation_epoch &&
         lhs.epoch.name_resolution_epoch == rhs.epoch.name_resolution_epoch &&
         lhs.epoch.resource_epoch == rhs.epoch.resource_epoch;
}

bool ScopeCapabilityMatches(const IparSupportSessionScope& lhs,
                            const IparSupportSessionScope& rhs) {
  return lhs.epoch.capability_policy_generation ==
         rhs.epoch.capability_policy_generation;
}

bool ScopeAuthorizationHashesMatch(const IparSupportSessionScope& lhs,
                                   const IparSupportSessionScope& rhs) {
  return lhs.epoch.role_set_hash == rhs.epoch.role_set_hash &&
         lhs.epoch.group_set_hash == rhs.epoch.group_set_hash &&
         lhs.epoch.search_path_hash == rhs.epoch.search_path_hash;
}

bool ScopeIdentityMatches(const IparSupportSessionScope& lhs,
                          const IparSupportSessionScope& rhs) {
  return lhs.auth_context_uuid == rhs.auth_context_uuid &&
         lhs.principal_uuid == rhs.principal_uuid &&
         lhs.effective_user_uuid == rhs.effective_user_uuid &&
         lhs.database_uuid == rhs.database_uuid;
}

std::string PreparedTemplateKey(const IparPreparedTemplatePut& request,
                                std::string_view envelope_digest,
                                std::string_view dependency_digest) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "operation_id", request.operation_id);
  AppendField(&out, "operation_family", request.operation_family);
  AppendField(&out, "canonical_sblr_digest", envelope_digest);
  AppendField(&out, "dependency_digest", dependency_digest);
  AppendField(&out, "result_descriptor_hash", request.result_descriptor_hash);
  return "ipar.prepared_template:" + Digest(out.str());
}

std::string PreparedTemplateLookupKey(
    const IparPreparedTemplateLookup& request,
    std::string_view envelope_digest,
    std::string_view dependency_digest) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "operation_id", request.operation_id);
  AppendField(&out, "operation_family", request.operation_family);
  AppendField(&out, "canonical_sblr_digest", envelope_digest);
  AppendField(&out, "dependency_digest", dependency_digest);
  AppendField(&out, "result_descriptor_hash", request.result_descriptor_hash);
  return "ipar.prepared_template:" + Digest(out.str());
}

std::string DescriptorKey(const IparResolvedDescriptorPut& request) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "resolved_name", request.resolved_name);
  AppendField(&out, "object_uuid", request.object_uuid);
  AppendField(&out, "object_kind", request.object_kind);
  AppendField(&out, "descriptor_uuid", request.descriptor_uuid);
  AppendField(&out, "descriptor_hash", request.descriptor_hash);
  AppendField(&out, "operation_id", request.operation_id);
  return "ipar.descriptor:" + Digest(out.str());
}

std::string BatchCapabilityKey(const IparProtocolBatchRequest& request) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "operation_id", request.operation_id);
  AppendField(&out, "driver_capability_generation",
              request.driver_capability_generation);
  AppendField(&out, "server_capability_generation",
              request.server_capability_generation);
  return "ipar.batch_capability:" + Digest(out.str());
}

std::string FrameKey(const IparFrameCryptoRequest& request) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "frame_class", request.frame_class);
  AppendField(&out, "crypto_epoch", request.crypto_epoch);
  AppendField(&out, "handshake_generation", request.handshake_generation);
  AppendField(&out, "tls_policy_hash", request.tls_policy_hash);
  AppendField(&out, "key_lineage_id", request.key_lineage_id);
  return "ipar.frame_crypto:" + Digest(out.str());
}

std::string ResultBufferKey(const IparResultBufferRequest& request) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "operation_id", request.operation_id);
  AppendField(&out, "result_shape_hash", request.result_shape_hash);
  AppendField(&out, "target_object_uuid", request.target_object_uuid);
  AppendField(&out, "returning_path", request.returning_path ? "true" : "false");
  return "ipar.result_buffer:" + Digest(out.str());
}

std::string FailureKey(const IparFailureClassificationRequest& request) {
  std::ostringstream out;
  AppendScopePayload(&out, request.scope);
  AppendField(&out, "diagnostic_code", request.diagnostic_code);
  AppendField(&out, "diagnostic_detail", request.diagnostic_detail);
  AppendField(&out, "operation_id", request.operation_id);
  AppendField(&out, "statement_shape_hash", request.statement_shape_hash);
  AppendField(&out, "engine_finality_known",
              request.engine_finality_known ? "true" : "false");
  return "ipar.hot_failure:" + Digest(out.str());
}

}  // namespace

IparSupportSessionScope IparSupportScopeForSession(
    const ServerSessionRecord& session) {
  IparSupportSessionScope scope;
  scope.session_uuid = session.session_uuid;
  scope.auth_context_uuid = session.auth_context_uuid;
  scope.principal_uuid = session.principal_uuid;
  scope.effective_user_uuid = session.effective_user_uuid;
  scope.database_uuid = session.database_uuid;
  scope.epoch.catalog_generation = session.catalog_generation;
  scope.epoch.security_epoch = session.security_epoch;
  scope.epoch.descriptor_epoch = session.descriptor_epoch;
  scope.epoch.grant_epoch = session.grant_epoch;
  scope.epoch.policy_generation = session.policy_generation;
  scope.epoch.capability_policy_generation =
      session.capability_policy_generation;
  scope.epoch.cache_invalidation_epoch = session.cache_invalidation_epoch;
  scope.epoch.name_resolution_epoch = session.name_resolution_epoch;
  scope.epoch.resource_epoch = session.resource_epoch;
  scope.epoch.role_set_hash = session.role_set_hash;
  scope.epoch.group_set_hash = session.group_set_hash;
  scope.epoch.search_path_hash = session.search_path_hash;
  return scope;
}

IparCacheStatus IparServerProtocolSupport::ValidateAuthorityFlags(
    const IparCacheAuthorityFlags& flags) const {
  IparCacheStatus status;
  if (flags.client_or_parser_authorization_authority) {
    status.authority_forbidden = true;
    status.detail = "ipar_cache_authorization_authority_forbidden";
    return status;
  }
  if (flags.transaction_finality_authority) {
    status.authority_forbidden = true;
    status.detail = "ipar_cache_finality_authority_forbidden";
    return status;
  }
  if (flags.visibility_authority) {
    status.authority_forbidden = true;
    status.detail = "ipar_cache_visibility_authority_forbidden";
    return status;
  }
  if (flags.grants_authority) {
    status.authority_forbidden = true;
    status.detail = "ipar_cache_grant_forbidden";
    return status;
  }
  status.accepted = true;
  status.detail = "ipar_cache_authority_advisory";
  return status;
}

IparCacheStatus IparServerProtocolSupport::ValidateScope(
    const IparSupportSessionScope& cached,
    const IparSupportSessionScope& current) const {
  IparCacheStatus status;
  if (cached.session_uuid != current.session_uuid) {
    status.cross_session = true;
    status.detail = "ipar_cache_cross_session";
    return status;
  }
  if (!ScopeIdentityMatches(cached, current)) {
    status.cross_authorization = true;
    status.detail = "ipar_cache_cross_authorization";
    return status;
  }
  if (!ScopeCapabilityMatches(cached, current)) {
    status.capability_stale = true;
    status.stale = true;
    status.detail = "ipar_cache_capability_stale";
    return status;
  }
  if (!ScopeNumbersMatch(cached, current)) {
    status.stale = true;
    status.detail = "ipar_cache_epoch_stale";
    return status;
  }
  if (!ScopeAuthorizationHashesMatch(cached, current)) {
    status.cross_authorization = true;
    status.stale = true;
    status.detail = "ipar_cache_authorization_hash_stale";
    return status;
  }
  status.accepted = true;
  status.detail = "ipar_cache_valid";
  return status;
}

IparFailureClassificationResult IparServerProtocolSupport::MakeFailureResult(
    const FailureRecord& record,
    bool cache_hit) const {
  IparFailureClassificationResult result;
  result.accepted = true;
  result.cache_hit = cache_hit;
  result.retryable = record.retryable;
  result.requires_mga_inventory_check = record.requires_mga_inventory_check;
  result.ends_transaction = record.ends_transaction;
  result.finality_authority = false;
  result.detail = cache_hit ? "ipar_failure_cache_hit"
                            : "ipar_failure_classified";
  result.cache_key = record.cache_key;
  result.failure_class = record.failure_class;
  return result;
}

IparCacheStatus IparServerProtocolSupport::StorePreparedTemplate(
    IparPreparedTemplatePut request) {
  IparCacheStatus status = ValidateAuthorityFlags(request.authority_flags);
  if (!status.accepted) {
    ++metrics_.authority_rejections;
    return status;
  }
  if (request.parser_sql_text_present ||
      LooksLikeSqlAuthorityText(request.canonical_sblr_envelope)) {
    status = {};
    status.authority_forbidden = true;
    status.detail = "ipar_template_sql_text_forbidden";
    ++metrics_.authority_rejections;
    return status;
  }
  if (!request.all_names_resolved_to_uuid) {
    status = {};
    status.stale = true;
    status.detail = "ipar_template_uuid_resolution_required";
    ++metrics_.stale_rejections;
    return status;
  }
  std::string dependency_detail;
  if (!DependenciesValid(request.dependencies, request.scope,
                         &dependency_detail)) {
    status = {};
    status.stale = true;
    status.detail = dependency_detail;
    ++metrics_.stale_rejections;
    return status;
  }
  if (request.operation_id.empty() || request.operation_family.empty() ||
      request.canonical_sblr_envelope.empty() ||
      request.result_descriptor_hash.empty()) {
    status = {};
    status.stale = true;
    status.detail = "ipar_template_shape_required";
    ++metrics_.stale_rejections;
    return status;
  }

  const std::string envelope_digest = Digest(request.canonical_sblr_envelope);
  const std::string dependency_digest = Digest(DependencyPayload(request.dependencies));
  const std::string cache_key =
      PreparedTemplateKey(request, envelope_digest, dependency_digest);

  IparPreparedTemplateRecord& record = prepared_templates_by_key_[cache_key];
  if (record.generation == 0) record.generation = next_generation_++;
  record.cache_key = cache_key;
  record.scope = request.scope;
  record.operation_id = request.operation_id;
  record.operation_family = request.operation_family;
  record.canonical_sblr_digest = envelope_digest;
  record.dependency_digest = dependency_digest;
  record.result_descriptor_hash = request.result_descriptor_hash;
  record.dependencies = std::move(request.dependencies);
  record.authority_flags = {};

  status.accepted = true;
  status.cache_key = cache_key;
  status.detail = "ipar_template_cache_stored";
  return status;
}

IparCacheStatus IparServerProtocolSupport::LookupPreparedTemplate(
    const IparPreparedTemplateLookup& request) {
  const std::string envelope_digest = Digest(request.canonical_sblr_envelope);
  const std::string dependency_digest = Digest(DependencyPayload(request.dependencies));
  const std::string cache_key =
      request.cache_key.empty()
          ? PreparedTemplateLookupKey(request, envelope_digest, dependency_digest)
          : request.cache_key;

  auto iter = prepared_templates_by_key_.find(cache_key);
  if (iter == prepared_templates_by_key_.end()) {
    ++metrics_.prepared_template_misses;
    IparCacheStatus miss;
    miss.detail = "ipar_template_cache_miss";
    miss.cache_key = cache_key;
    return miss;
  }

  IparPreparedTemplateRecord& record = iter->second;
  IparCacheStatus status = ValidateScope(record.scope, request.scope);
  if (!status.accepted) {
    status.cache_key = cache_key;
    ++metrics_.stale_rejections;
    return status;
  }
  status = ValidateAuthorityFlags(record.authority_flags);
  if (!status.accepted) {
    status.cache_key = cache_key;
    ++metrics_.authority_rejections;
    return status;
  }

  std::string dependency_detail;
  if (!DependenciesValid(request.dependencies, request.scope,
                         &dependency_detail) ||
      record.dependency_digest != dependency_digest) {
    status = {};
    status.stale = true;
    status.detail = dependency_detail.empty() ? "ipar_template_dependency_stale"
                                             : dependency_detail;
    status.cache_key = cache_key;
    ++metrics_.stale_rejections;
    return status;
  }
  if (record.operation_id != request.operation_id ||
      record.operation_family != request.operation_family ||
      record.canonical_sblr_digest != envelope_digest ||
      record.result_descriptor_hash != request.result_descriptor_hash) {
    status = {};
    status.stale = true;
    status.detail = "ipar_template_shape_stale";
    status.cache_key = cache_key;
    ++metrics_.stale_rejections;
    return status;
  }

  ++record.hit_count;
  ++metrics_.prepared_template_hits;
  status.accepted = true;
  status.cache_hit = true;
  status.cache_key = cache_key;
  status.detail = "ipar_template_cache_hit";
  return status;
}

IparCacheStatus IparServerProtocolSupport::StoreResolvedDescriptor(
    IparResolvedDescriptorPut request) {
  IparCacheStatus status = ValidateAuthorityFlags(request.authority_flags);
  if (!status.accepted) {
    ++metrics_.authority_rejections;
    return status;
  }
  if (!LooksLikeCanonicalUuidText(request.object_uuid) ||
      !LooksLikeCanonicalUuidText(request.descriptor_uuid) ||
      request.descriptor_hash.empty() || request.operation_id.empty() ||
      request.resolved_name.empty() || request.object_kind.empty()) {
    status = {};
    status.stale = true;
    status.detail = "ipar_descriptor_uuid_shape_required";
    ++metrics_.stale_rejections;
    return status;
  }
  const std::string cache_key = DescriptorKey(request);
  IparDescriptorRecord& record = descriptors_by_key_[cache_key];
  if (record.generation == 0) record.generation = next_generation_++;
  record.cache_key = cache_key;
  record.scope = request.scope;
  record.resolved_name = request.resolved_name;
  record.object_uuid = request.object_uuid;
  record.object_kind = request.object_kind;
  record.descriptor_uuid = request.descriptor_uuid;
  record.descriptor_hash = request.descriptor_hash;
  record.operation_id = request.operation_id;
  record.authority_flags = {};

  status.accepted = true;
  status.cache_key = cache_key;
  status.detail = "ipar_descriptor_cache_stored";
  return status;
}

IparCacheStatus IparServerProtocolSupport::LookupResolvedDescriptor(
    const IparResolvedDescriptorPut& request) {
  const std::string cache_key =
      request.cache_key.empty() ? DescriptorKey(request) : request.cache_key;
  auto iter = descriptors_by_key_.find(cache_key);
  if (iter == descriptors_by_key_.end()) {
    ++metrics_.descriptor_misses;
    IparCacheStatus miss;
    miss.detail = "ipar_descriptor_cache_miss";
    miss.cache_key = cache_key;
    return miss;
  }
  IparDescriptorRecord& record = iter->second;
  IparCacheStatus status = ValidateScope(record.scope, request.scope);
  if (!status.accepted) {
    status.cache_key = cache_key;
    ++metrics_.stale_rejections;
    return status;
  }
  if (record.resolved_name != request.resolved_name ||
      record.object_uuid != request.object_uuid ||
      record.object_kind != request.object_kind ||
      record.descriptor_uuid != request.descriptor_uuid ||
      record.descriptor_hash != request.descriptor_hash ||
      record.operation_id != request.operation_id) {
    status = {};
    status.stale = true;
    status.detail = "ipar_descriptor_shape_stale";
    status.cache_key = cache_key;
    ++metrics_.stale_rejections;
    return status;
  }
  ++record.hit_count;
  ++metrics_.descriptor_hits;
  status.accepted = true;
  status.cache_hit = true;
  status.cache_key = cache_key;
  status.detail = "ipar_descriptor_cache_hit";
  return status;
}

IparProtocolBatchPlan IparServerProtocolSupport::PlanProtocolBatch(
    const IparProtocolBatchRequest& request) const {
  IparProtocolBatchPlan plan;
  plan.server_revalidation_required = true;
  plan.metadata_cache_advisory = request.metadata_cache_advisory;
  if (request.parser_or_driver_finality_authority) {
    plan.detail = "ipar_batch_finality_authority_forbidden";
    return plan;
  }
  if (!request.metadata_cache_advisory) {
    plan.detail = "ipar_batch_metadata_must_be_advisory";
    return plan;
  }
  if (request.driver_capability_generation !=
          request.scope.epoch.capability_policy_generation ||
      request.server_capability_generation !=
          request.scope.epoch.capability_policy_generation) {
    plan.detail = "ipar_batch_capability_epoch_stale";
    return plan;
  }
  if (request.contains_dml && request.contains_ddl) {
    plan.dml_ddl_mixed_refused = true;
    plan.detail = "ipar_batch_dml_ddl_mixed_refused";
    return plan;
  }

  plan.accepted = true;
  plan.capability_cache_key = BatchCapabilityKey(request);
  if (request.requested_rows == 0) {
    plan.detail = "ipar_empty_batch";
    return plan;
  }
  if (request.operation_class == IparProtocolOperationClass::kDdl) {
    plan.driver_batching_enabled = false;
    plan.windows.push_back({0, request.requested_rows,
                            request.estimated_payload_bytes});
    plan.detail = "ipar_ddl_single_operation";
    return plan;
  }

  const std::uint64_t max_rows = std::max<std::uint64_t>(1, request.max_batch_rows);
  const std::uint64_t max_frame =
      std::max<std::uint64_t>(1, request.max_frame_bytes);
  const std::uint64_t avg_bytes =
      std::max<std::uint64_t>(1, (request.estimated_payload_bytes +
                                  request.requested_rows - 1) /
                                     request.requested_rows);
  const std::uint64_t rows_by_frame =
      std::max<std::uint64_t>(1, max_frame / avg_bytes);
  const std::uint64_t rows_per_window =
      std::max<std::uint64_t>(1, std::min(max_rows, rows_by_frame));
  plan.driver_batching_enabled =
      request.driver_supports_batching &&
      request.operation_class == IparProtocolOperationClass::kDml &&
      request.requested_rows > 1;

  for (std::uint64_t first = 0; first < request.requested_rows;) {
    const std::uint64_t remaining = request.requested_rows - first;
    const std::uint64_t count = std::min(rows_per_window, remaining);
    const std::uint64_t bytes =
        std::min<std::uint64_t>(max_frame, count * avg_bytes);
    plan.windows.push_back({first, count, bytes});
    first += count;
  }
  plan.detail = plan.driver_batching_enabled ? "ipar_dml_batch_planned"
                                             : "ipar_operation_window_planned";
  return plan;
}

IparFrameCryptoLease IparServerProtocolSupport::AcquireFrameCryptoLease(
    const IparFrameCryptoRequest& request) {
  IparFrameCryptoLease lease;
  if (request.contains_plaintext_database_key_material) {
    lease.detail = "ipar_crypto_plaintext_key_material_forbidden";
    return lease;
  }
  if (request.parser_or_driver_authority) {
    lease.detail = "ipar_frame_crypto_authority_forbidden";
    return lease;
  }
  const std::string reuse_key = request.requested_reuse_key.empty()
                                    ? FrameKey(request)
                                    : request.requested_reuse_key;
  auto iter = frame_pool_by_key_.find(reuse_key);
  const bool existing = iter != frame_pool_by_key_.end();
  if (existing) {
    IparCacheStatus scope_status = ValidateScope(iter->second.scope,
                                                 request.scope);
    if (!scope_status.accepted) {
      ++metrics_.stale_rejections;
      lease.detail = scope_status.detail;
      lease.reuse_key = reuse_key;
      return lease;
    }
    if (iter->second.crypto_epoch != request.crypto_epoch ||
        iter->second.handshake_generation != request.handshake_generation ||
        iter->second.tls_policy_hash != request.tls_policy_hash ||
        iter->second.key_lineage_id != request.key_lineage_id) {
      ++metrics_.stale_rejections;
      lease.detail = "ipar_frame_crypto_epoch_stale";
      lease.reuse_key = reuse_key;
      return lease;
    }
  }

  PoolEntry& entry = frame_pool_by_key_[reuse_key];
  if (!existing) {
    entry.scope = request.scope;
    entry.reuse_key = reuse_key;
    entry.capacity = request.payload_capacity;
    entry.crypto_epoch = request.crypto_epoch;
    entry.handshake_generation = request.handshake_generation;
    entry.tls_policy_hash = request.tls_policy_hash;
    entry.key_lineage_id = request.key_lineage_id;
  }
  entry.capacity = std::max(entry.capacity, request.payload_capacity);
  lease.frame_reused = entry.available > 0;
  if (lease.frame_reused) {
    --entry.available;
    ++metrics_.frame_reuses;
  }
  lease.crypto_context_reused = existing;
  if (lease.crypto_context_reused) ++metrics_.crypto_reuses;
  lease.accepted = true;
  lease.stores_plaintext_key_material = false;
  lease.detail = "ipar_frame_crypto_lease_acquired";
  lease.reuse_key = reuse_key;
  lease.payload_capacity = entry.capacity;
  return lease;
}

IparCacheStatus IparServerProtocolSupport::ReleaseFrameCryptoLease(
    const IparSupportSessionScope& scope,
    const std::string& reuse_key) {
  IparCacheStatus status;
  auto iter = frame_pool_by_key_.find(reuse_key);
  if (iter == frame_pool_by_key_.end()) {
    status.detail = "ipar_frame_crypto_pool_miss";
    status.cache_key = reuse_key;
    return status;
  }
  status = ValidateScope(iter->second.scope, scope);
  status.cache_key = reuse_key;
  if (!status.accepted) {
    ++metrics_.stale_rejections;
    return status;
  }
  ++iter->second.available;
  status.detail = "ipar_frame_crypto_lease_released";
  return status;
}

IparResultBufferLease IparServerProtocolSupport::AcquireResultBuffer(
    const IparResultBufferRequest& request) {
  IparResultBufferLease lease;
  lease.finality_authority = false;
  if (request.parser_or_driver_finality_authority) {
    lease.detail = "ipar_result_buffer_finality_authority_forbidden";
    return lease;
  }
  const std::string reuse_key = request.requested_reuse_key.empty()
                                    ? ResultBufferKey(request)
                                    : request.requested_reuse_key;
  auto iter = result_buffer_pool_by_key_.find(reuse_key);
  const bool existing = iter != result_buffer_pool_by_key_.end();
  if (existing) {
    IparCacheStatus scope_status = ValidateScope(iter->second.scope,
                                                 request.scope);
    if (!scope_status.accepted) {
      ++metrics_.stale_rejections;
      lease.detail = scope_status.detail;
      lease.reuse_key = reuse_key;
      return lease;
    }
  }

  PoolEntry& entry = result_buffer_pool_by_key_[reuse_key];
  if (!existing) {
    entry.scope = request.scope;
    entry.reuse_key = reuse_key;
    entry.capacity = request.minimum_capacity;
  }
  entry.capacity = std::max(entry.capacity, request.minimum_capacity);
  lease.cache_hit = entry.available > 0;
  if (lease.cache_hit) {
    --entry.available;
    ++metrics_.result_buffer_reuses;
  }
  lease.accepted = true;
  lease.returning_path = request.returning_path;
  lease.detail = lease.cache_hit ? "ipar_result_buffer_cache_hit"
                                 : "ipar_result_buffer_lease_acquired";
  lease.reuse_key = reuse_key;
  lease.capacity = entry.capacity;
  return lease;
}

IparCacheStatus IparServerProtocolSupport::ReleaseResultBuffer(
    const IparSupportSessionScope& scope,
    const std::string& reuse_key) {
  IparCacheStatus status;
  auto iter = result_buffer_pool_by_key_.find(reuse_key);
  if (iter == result_buffer_pool_by_key_.end()) {
    status.detail = "ipar_result_buffer_pool_miss";
    status.cache_key = reuse_key;
    return status;
  }
  status = ValidateScope(iter->second.scope, scope);
  status.cache_key = reuse_key;
  if (!status.accepted) {
    ++metrics_.stale_rejections;
    return status;
  }
  ++iter->second.available;
  status.detail = "ipar_result_buffer_released";
  return status;
}

IparFailureClassificationResult IparServerProtocolSupport::ClassifyFailure(
    const IparFailureClassificationRequest& request) {
  IparFailureClassificationResult result;
  result.finality_authority = false;
  if (request.parser_or_driver_finality_authority) {
    result.detail = "ipar_failure_finality_authority_forbidden";
    return result;
  }
  const std::string cache_key = FailureKey(request);
  auto iter = failures_by_key_.find(cache_key);
  if (iter != failures_by_key_.end()) {
    IparCacheStatus scope_status = ValidateScope(iter->second.scope,
                                                 request.scope);
    if (!scope_status.accepted) {
      ++metrics_.stale_rejections;
      result.detail = scope_status.detail;
      result.cache_key = cache_key;
      return result;
    }
    ++iter->second.hit_count;
    ++metrics_.failure_cache_hits;
    return MakeFailureResult(iter->second, true);
  }

  const std::string lowered_code = LowerAscii(request.diagnostic_code);
  const std::string lowered_detail = LowerAscii(request.diagnostic_detail);
  FailureRecord record;
  record.scope = request.scope;
  record.cache_key = cache_key;
  record.diagnostic_code = request.diagnostic_code;
  record.diagnostic_detail = request.diagnostic_detail;
  record.operation_id = request.operation_id;
  record.statement_shape_hash = request.statement_shape_hash;
  if (lowered_code.find("stale") != std::string::npos ||
      lowered_code.find("cache") != std::string::npos ||
      lowered_detail.find("stale") != std::string::npos ||
      lowered_detail.find("cache") != std::string::npos) {
    record.failure_class = "stale_cache";
    record.retryable = true;
  } else if (lowered_code.find("sql_text_forbidden") != std::string::npos ||
             lowered_code.find("sblr_revalidation_failed") != std::string::npos ||
             lowered_code.find("envelope") != std::string::npos) {
    record.failure_class = "protocol_refusal";
  } else if (!request.engine_finality_known ||
             lowered_code.find("unknown") != std::string::npos ||
             lowered_detail.find("unknown") != std::string::npos ||
             lowered_code.find("engine_dispatch_failed") != std::string::npos) {
    record.failure_class = "engine_outcome_requires_mga_inventory";
    record.retryable = true;
    record.requires_mga_inventory_check = true;
  } else if (lowered_code.find("auth") != std::string::npos) {
    record.failure_class = "authorization_refusal";
  } else {
    record.failure_class = "hot_failure_refusal";
  }
  record.ends_transaction = false;
  record.finality_authority = false;
  failures_by_key_[cache_key] = record;
  return MakeFailureResult(failures_by_key_[cache_key], false);
}

IparFailureClassificationResult
IparServerProtocolSupport::LookupFailureClassification(
    const IparFailureClassificationRequest& request) {
  const std::string cache_key =
      request.requested_cache_key.empty() ? FailureKey(request)
                                          : request.requested_cache_key;
  auto iter = failures_by_key_.find(cache_key);
  IparFailureClassificationResult result;
  result.finality_authority = false;
  result.cache_key = cache_key;
  if (iter == failures_by_key_.end()) {
    result.detail = "ipar_failure_cache_miss";
    return result;
  }
  IparCacheStatus scope_status = ValidateScope(iter->second.scope,
                                               request.scope);
  if (!scope_status.accepted) {
    ++metrics_.stale_rejections;
    result.detail = scope_status.detail;
    return result;
  }
  ++iter->second.hit_count;
  ++metrics_.failure_cache_hits;
  return MakeFailureResult(iter->second, true);
}

}  // namespace scratchbird::server
