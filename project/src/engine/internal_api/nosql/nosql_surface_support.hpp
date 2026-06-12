// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "behavior_support/api_behavior_store.hpp"
#include "dml/write_result_policy.hpp"
#include "heavy_immutable_generation_publication.hpp"
#include "hot_cold_row_split.hpp"
#include "large_payload.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <optional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_SURFACE_SUPPORT
// Shared helpers for native NoSQL/specialized API surfaces. These helpers keep
// cluster-only behavior fail-closed until cluster authority is available.

inline bool EngineNoSqlOptionContains(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (option.find(token) != std::string::npos) { return true; }
  }
  return false;
}

inline bool EngineNoSqlRequiresClusterAuthority(const EngineApiRequest& request) {
  return EngineNoSqlOptionContains(request, "cluster") ||
         EngineNoSqlOptionContains(request, "distributed") ||
         EngineNoSqlOptionContains(request, "shard") ||
         EngineNoSqlOptionContains(request, "replica") ||
         EngineNoSqlOptionContains(request, "route") ||
         EngineNoSqlOptionContains(request, "cross_node") ||
         EngineNoSqlOptionContains(request, "placement");
}

template <typename TResult>
TResult EngineNoSqlClusterAuthorityUnavailable(const EngineApiRequest& request, const std::string& operation_id) {
  auto result = MakeApiBehaviorDiagnostic<TResult>(
      request.context,
      operation_id,
      MakeClusterAuthorityUnavailableDiagnostic(operation_id));
  result.cluster_authority_required = true;
  return result;
}

inline void AddEngineNoSqlSurfaceEvidence(EngineApiResult* result,
                                          const std::string& surface,
                                          const std::string& behavior) {
  AddApiBehaviorEvidence(result, "nosql_surface", surface);
  AddApiBehaviorEvidence(result, "nosql_behavior", behavior);
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

template <typename TResult, typename TRequest>
TResult EngineNoSqlPersistedWriteResult(
    const TRequest& request,
    const std::string& operation_id,
    const std::string& object_kind,
    bool require_transaction = true,
    std::string state = "active",
    bool deleted = false) {
  const auto write_result_policy = ResolveWriteResultPolicy(request, operation_id);
  if (!write_result_policy.ok) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  auto result = PersistedRecordResult<TResult>(
      request,
      operation_id,
      object_kind,
      require_transaction,
      std::move(state),
      deleted);
  if (result.ok) {
    result.dml_summary.rows_changed = 1;
    ApplyWriteResultPolicy(write_result_policy, &result);
  }
  return result;
}

template <typename TResult>
std::optional<TResult> EngineNoSqlWriteResultPolicyFailure(
    const EngineApiRequest& request,
    const std::string& operation_id,
    EngineWriteResultPolicyResolution* resolved_policy) {
  auto policy = ResolveWriteResultPolicy(request, operation_id);
  if (!policy.ok) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(policy, &failure);
    return failure;
  }
  *resolved_policy = std::move(policy);
  return std::nullopt;
}

inline bool EngineNoSqlOptionStartsWith(const std::string& value,
                                        const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

inline std::optional<std::string> EngineNoSqlOptionValue(
    const EngineApiRequest& request,
    const std::string& key) {
  const auto equals_prefix = key + "=";
  const auto colon_prefix = key + ":";
  for (const auto& option : request.option_envelopes) {
    if (EngineNoSqlOptionStartsWith(option, equals_prefix)) {
      return option.substr(equals_prefix.size());
    }
    if (EngineNoSqlOptionStartsWith(option, colon_prefix)) {
      return option.substr(colon_prefix.size());
    }
  }
  return std::nullopt;
}

inline bool EngineNoSqlHasOptionPrefix(const EngineApiRequest& request,
                                       const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (EngineNoSqlOptionStartsWith(option, prefix)) {
      return true;
    }
  }
  return false;
}

inline bool EngineNoSqlRequestsHeavyImmutableGeneration(
    const EngineApiRequest& request) {
  return EngineNoSqlHasOptionPrefix(request, "heavy_generation.");
}

inline bool EngineNoSqlOptionBool(const EngineApiRequest& request,
                                  const std::string& key) {
  const auto value = EngineNoSqlOptionValue(request, key);
  return value.has_value() &&
         (*value == "1" || *value == "true" || *value == "TRUE");
}

inline std::pair<bool, EngineApiU64> EngineNoSqlOptionU64(
    const EngineApiRequest& request,
    const std::string& key) {
  const auto value = EngineNoSqlOptionValue(request, key);
  if (!value.has_value()) {
    return {false, 0};
  }
  try {
    return {true, static_cast<EngineApiU64>(std::stoull(*value))};
  } catch (...) {
    return {true, 0};
  }
}

inline scratchbird::core::platform::TypedUuid EngineNoSqlParseUuid(
    scratchbird::core::platform::UuidKind kind,
    const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const auto parsed =
      scratchbird::core::uuid::ParseDurableEngineIdentityUuid(kind, text);
  return parsed.ok() ? parsed.value : scratchbird::core::platform::TypedUuid{};
}

inline std::string EngineNoSqlDiagnosticDetail(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      return argument.value;
    }
  }
  return diagnostic.diagnostic_code;
}

inline EngineApiDiagnostic EngineNoSqlHeavyGenerationDiagnostic(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  EngineApiDiagnostic api_diagnostic;
  api_diagnostic.code = diagnostic.diagnostic_code;
  api_diagnostic.message_key = diagnostic.message_key;
  api_diagnostic.detail = EngineNoSqlDiagnosticDetail(diagnostic);
  api_diagnostic.error = true;
  return api_diagnostic;
}

struct EngineNoSqlPayloadResolution {
  bool ok = true;
  bool large_descriptor = false;
  bool inline_payload = false;
  std::string payload;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
};

inline scratchbird::storage::page::LargePayloadFamily EngineNoSqlLargePayloadFamily(
    const std::string& object_kind) {
  namespace page = scratchbird::storage::page;
  if (object_kind == "document") { return page::LargePayloadFamily::document; }
  if (object_kind == "key_value") { return page::LargePayloadFamily::key_value; }
  if (object_kind == "vector") { return page::LargePayloadFamily::vector; }
  if (object_kind == "search" || object_kind == "text") { return page::LargePayloadFamily::text; }
  if (object_kind == "graph") { return page::LargePayloadFamily::graph; }
  return page::LargePayloadFamily::blob;
}

inline std::vector<scratchbird::core::platform::byte> EngineNoSqlPayloadBytes(
    const std::string& payload) {
  return std::vector<scratchbird::core::platform::byte>(payload.begin(), payload.end());
}

inline scratchbird::storage::page::LargePayloadStore& EngineNoSqlLargePayloadStoreForContext(
    const EngineRequestContext& context) {
  namespace page = scratchbird::storage::page;
  static std::map<std::string, page::LargePayloadStore> stores;
  std::string key = context.database_path;
  if (key.empty()) { key = context.database_uuid.canonical; }
  if (key.empty()) { key = "embedded_transient_large_payload_store"; }
  return stores[key];
}

inline EngineApiU64 EngineNoSqlDiagnosticPayloadSeed(
    const EngineApiRequest& request,
    const std::string& operation_id,
    const std::string& object_kind,
    const std::string& payload) {
  EngineApiU64 hash = 1469598103934665603ULL;
  const auto mix = [&hash](const std::string& value) {
    for (const unsigned char ch : value) {
      hash ^= static_cast<EngineApiU64>(ch);
      hash *= 1099511628211ULL;
    }
  };
  mix(operation_id);
  mix(object_kind);
  mix(request.target_object.uuid.canonical);
  mix(payload);
  hash ^= request.context.local_transaction_id;
  return hash == 0 ? 1 : hash;
}

inline EngineNoSqlPayloadResolution EngineNoSqlResolvePayloadForStorage(
    const EngineApiRequest& request,
    const std::string& operation_id,
    const std::string& object_kind) {
  namespace page = scratchbird::storage::page;
  namespace platform = scratchbird::core::platform;

  EngineNoSqlPayloadResolution resolution;
  resolution.payload =
      EngineNoSqlOptionValue(request, "large_payload.payload")
          .value_or(ApiBehaviorPayloadFromRequest(request));
  const auto threshold =
      EngineNoSqlOptionU64(request, "large_payload.inline_threshold");
  const auto inline_threshold = threshold.first
                                    ? threshold.second
                                    : page::kLargePayloadDefaultInlineThresholdBytes;
  if (EngineNoSqlOptionBool(request, "hot_cold_split.enabled")) {
    page::LargePayloadStore& store = EngineNoSqlLargePayloadStoreForContext(request.context);
    const auto diagnostic_seed =
        EngineNoSqlDiagnosticPayloadSeed(request, operation_id, object_kind, resolution.payload);
    if (store.next_evidence_sequence == 1 && store.generations.empty()) {
      store.next_evidence_sequence = diagnostic_seed;
      store.overflow_ledger.next_evidence_sequence = diagnostic_seed;
    }

    page::HotColdRowSplitRequest split_request;
    split_request.large_payload_store = &store;
    split_request.database_uuid = EngineNoSqlParseUuid(
        platform::UuidKind::database,
        EngineNoSqlOptionValue(request, "large_payload.database_uuid")
            .value_or(request.context.database_uuid.canonical));
    split_request.filespace_uuid = EngineNoSqlParseUuid(
        platform::UuidKind::filespace,
        EngineNoSqlOptionValue(request, "large_payload.filespace_uuid")
            .value_or(std::string{}));
    split_request.owner_object_uuid = EngineNoSqlParseUuid(
        platform::UuidKind::object,
        EngineNoSqlOptionValue(request, "large_payload.owner_object_uuid")
            .value_or(request.target_object.uuid.canonical));
    split_request.row_uuid = split_request.owner_object_uuid;
    split_request.transaction_uuid = EngineNoSqlParseUuid(
        platform::UuidKind::transaction,
        EngineNoSqlOptionValue(request, "large_payload.transaction_uuid")
            .value_or(request.context.transaction_uuid.canonical));
    split_request.chunk_policy_uuid = EngineNoSqlParseUuid(
        platform::UuidKind::object,
        EngineNoSqlOptionValue(request, "large_payload.chunk_policy_uuid")
            .value_or(EngineNoSqlOptionValue(request, "large_payload.owner_object_uuid")
                          .value_or(request.target_object.uuid.canonical)));
    split_request.local_transaction_id = request.context.local_transaction_id;
    split_request.family = EngineNoSqlLargePayloadFamily(object_kind);
    split_request.cold_threshold_bytes = inline_threshold;
    split_request.engine_storage_admission_authorized = true;
    split_request.mga_write_admitted_by_transaction_inventory =
        request.context.local_transaction_id != 0;
    split_request.reason =
        "nosql_hot_cold_split;diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
    split_request.fields.push_back({"surface", object_kind, true, true, true, false, true, false});
    split_request.fields.push_back({"object_uuid",
                                    request.target_object.uuid.canonical,
                                    true,
                                    true,
                                    true,
                                    false,
                                    true,
                                    false});
    split_request.fields.push_back({"payload_kind", object_kind + "_payload", true, false, true, false, true, false});
    split_request.fields.push_back({"payload", resolution.payload, false, false, false, true, false, true});

    auto split = page::SplitHotColdRow(split_request);
    if (!split.ok()) {
      resolution.ok = false;
      resolution.diagnostic =
          MakeInvalidRequestDiagnostic(operation_id, split.diagnostic.diagnostic_code);
      resolution.evidence.push_back({"hot_cold_split_fail_closed", "true"});
      resolution.evidence.push_back({"hot_cold_split_refused", split.diagnostic.diagnostic_code});
      resolution.evidence.push_back({"hot_cold_split_finality_authority", "false"});
      resolution.evidence.push_back({"hot_cold_split_visibility_authority", "false"});
      return resolution;
    }

    resolution.large_descriptor = !split.hot_head.cold_fields.empty();
    resolution.inline_payload = false;
    resolution.payload = page::SerializeHotColdRowHead(split.hot_head);
    resolution.evidence.push_back({"hot_cold_split_routed", "true"});
    resolution.evidence.push_back({"hot_cold_split_surface", object_kind});
    resolution.evidence.push_back({"hot_cold_split_hot_fields", std::to_string(split.hot_head.hot_fields.size())});
    resolution.evidence.push_back({"hot_cold_split_cold_fields", std::to_string(split.hot_head.cold_fields.size())});
    resolution.evidence.push_back({"hot_cold_split_hot_filespace_class", split.hot_head.hot_filespace_class});
    resolution.evidence.push_back({"hot_cold_split_cold_row_filespace_class", split.hot_head.cold_row_filespace_class});
    if (!split.hot_head.cold_fields.empty()) {
      resolution.evidence.push_back({"hot_cold_split_cold_descriptor_filespace_class",
                                     split.hot_head.cold_fields.front().descriptor.filespace_class});
      resolution.evidence.push_back({"hot_cold_split_cold_generation",
                                     std::to_string(split.hot_head.cold_fields.front().descriptor.generation)});
    }
    resolution.evidence.push_back({"hot_cold_split_finality_authority", "false"});
    resolution.evidence.push_back({"hot_cold_split_visibility_authority", "false"});
    resolution.evidence.push_back({"mga_finality_authority", "engine_transaction_inventory"});
    return resolution;
  }
  if (resolution.payload.size() <= inline_threshold) {
    resolution.inline_payload = true;
    resolution.evidence.push_back({"large_payload_inline", "true"});
    resolution.evidence.push_back({"large_payload_descriptor_only", "false"});
    return resolution;
  }

  page::LargePayloadStore& store = EngineNoSqlLargePayloadStoreForContext(request.context);
  const auto diagnostic_seed =
      EngineNoSqlDiagnosticPayloadSeed(request, operation_id, object_kind, resolution.payload);
  if (store.next_evidence_sequence == 1 && store.generations.empty()) {
    store.next_evidence_sequence = diagnostic_seed;
    store.overflow_ledger.next_evidence_sequence = diagnostic_seed;
  }
  page::LargePayloadStoreRequest storage_request;
  storage_request.database_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::database,
      EngineNoSqlOptionValue(request, "large_payload.database_uuid")
          .value_or(request.context.database_uuid.canonical));
  storage_request.filespace_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::filespace,
      EngineNoSqlOptionValue(request, "large_payload.filespace_uuid")
          .value_or(std::string{}));
  storage_request.owner_object_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::object,
      EngineNoSqlOptionValue(request, "large_payload.owner_object_uuid")
          .value_or(request.target_object.uuid.canonical));
  storage_request.transaction_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::transaction,
      EngineNoSqlOptionValue(request, "large_payload.transaction_uuid")
          .value_or(request.context.transaction_uuid.canonical));
  storage_request.chunk_policy_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::object,
      EngineNoSqlOptionValue(request, "large_payload.chunk_policy_uuid")
          .value_or(EngineNoSqlOptionValue(request, "large_payload.owner_object_uuid")
                        .value_or(request.target_object.uuid.canonical)));
  storage_request.local_transaction_id = request.context.local_transaction_id;
  storage_request.family = EngineNoSqlLargePayloadFamily(object_kind);
  storage_request.payload_bytes = EngineNoSqlPayloadBytes(resolution.payload);
  storage_request.inline_threshold_bytes = inline_threshold;
  storage_request.allow_inline_payload = true;
  storage_request.reason =
      "diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
  storage_request.mga_write_admitted_by_transaction_inventory =
      request.context.local_transaction_id != 0;

  auto stored = page::StoreLargePayloadGeneration(&store, storage_request);
  if (!stored.ok()) {
    resolution.ok = false;
    resolution.diagnostic =
        MakeInvalidRequestDiagnostic(operation_id, stored.diagnostic.diagnostic_code);
    resolution.evidence.push_back({"large_payload_fail_closed", "true"});
    resolution.evidence.push_back({"large_payload_refused", stored.diagnostic.diagnostic_code});
    resolution.evidence.push_back({"large_payload_finality_authority", "false"});
    resolution.evidence.push_back({"large_payload_visibility_authority", "false"});
    return resolution;
  }

  resolution.large_descriptor = stored.descriptor_only;
  resolution.inline_payload = stored.descriptor.inline_payload;
  if (stored.descriptor_only) {
    resolution.payload = page::SerializeLargePayloadDescriptor(stored.descriptor);
  }
  resolution.evidence.push_back({"large_payload_descriptor_only", stored.descriptor_only ? "true" : "false"});
  resolution.evidence.push_back({"large_payload_inline", stored.descriptor.inline_payload ? "true" : "false"});
  resolution.evidence.push_back({"large_payload_filespace_class", stored.descriptor.filespace_class});
  resolution.evidence.push_back({"large_payload_generation", std::to_string(stored.descriptor.generation)});
  resolution.evidence.push_back({"large_payload_cache_token", stored.evidence.evidence_token});
  resolution.evidence.push_back({"large_payload_finality_authority", "false"});
  resolution.evidence.push_back({"large_payload_visibility_authority", "false"});
  resolution.evidence.push_back({"mga_finality_authority", "engine_transaction_inventory"});
  return resolution;
}

template <typename TResult, typename TRequest>
TResult EngineNoSqlPayloadAwarePersistedWriteResult(
    const TRequest& request,
    const std::string& operation_id,
    const std::string& object_kind,
    bool require_transaction = true,
    std::string state = "active",
    bool deleted = false) {
  const auto write_result_policy = ResolveWriteResultPolicy(request, operation_id);
  if (!write_result_policy.ok) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  auto payload = EngineNoSqlResolvePayloadForStorage(request, operation_id, object_kind);
  if (!payload.ok) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        payload.diagnostic);
    for (const auto& evidence : payload.evidence) {
      failure.evidence.push_back(evidence);
    }
    return failure;
  }
  auto result = PersistedRecordResultWithPayload<TResult>(
      request,
      operation_id,
      object_kind,
      require_transaction,
      std::move(state),
      deleted,
      payload.payload);
  if (result.ok) {
    result.dml_summary.rows_changed = 1;
    for (const auto& evidence : payload.evidence) {
      result.evidence.push_back(evidence);
    }
    ApplyWriteResultPolicy(write_result_policy, &result);
  }
  return result;
}

inline void AddEngineNoSqlHeavyGenerationRefusalEvidence(
    EngineApiResult* result,
    const scratchbird::core::index::HeavyImmutableGenerationResult& refused) {
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_fail_closed",
                         "true");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_refused",
                         refused.evidence.diagnostic_code);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_state",
                         scratchbird::core::index::
                             HeavyImmutableGenerationStateName(
                                 refused.evidence.state));
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
  if (!refused.evidence.engine_mga_authority_evidence_ref.empty()) {
    AddApiBehaviorEvidence(result,
                           "heavy_immutable_generation_mga_authority",
                           refused.evidence.engine_mga_authority_evidence_ref);
    AddApiBehaviorEvidence(result,
                           "mga_finality_authority",
                           "engine_transaction_inventory");
  }
}

inline void AddEngineNoSqlHeavyGenerationSuccessEvidence(
    EngineApiResult* result,
    const scratchbird::core::index::HeavyImmutableGenerationResult& published,
    const std::string& operation_evidence_kind) {
  const auto generation_uuid = scratchbird::core::uuid::UuidToString(
      published.generation.identity.generation_uuid.value);
  const auto collection_uuid = scratchbird::core::uuid::UuidToString(
      published.generation.identity.table_or_collection_uuid.value);
  const auto transaction_uuid = scratchbird::core::uuid::UuidToString(
      published.generation.identity.transaction_uuid.value);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_validation_state",
                         "validated");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_validation_proof",
                         published.generation.validation_proof_ref);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_id",
                         generation_uuid);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_family",
                         published.generation.identity.family);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_profile",
                         published.generation.identity.profile);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_mga_authority",
                         published.generation.engine_mga_authority_evidence_ref);
  AddApiBehaviorEvidence(result,
                         "mga_finality_authority",
                         "engine_transaction_inventory");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_publication_fence",
                         published.generation.publication_fence_ref);
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_publication_state",
                         "published");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_descriptor_only",
                         "true");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_visibility_authority",
                         "false");
  AddApiBehaviorEvidence(result,
                         "heavy_immutable_generation_cluster_provider",
                         "false");
  AddApiBehaviorEvidence(result,
                         operation_evidence_kind,
                         "validated_immutable_generation_published");
  AddApiBehaviorRow(result,
                    {{"generation_uuid", generation_uuid},
                     {"table_or_collection_uuid", collection_uuid},
                     {"transaction_uuid", transaction_uuid},
                     {"family", published.generation.identity.family},
                     {"profile", published.generation.identity.profile},
                     {"validation_state", "validated"},
                     {"publication_state", "published"},
                     {"source_row_count",
                      std::to_string(published.generation.source_row_count)},
                     {"source_payload_count",
                      std::to_string(
                          published.generation.source_payload_count)},
                     {"publication_fence",
                      published.generation.publication_fence_ref}});
}

template <typename TResult>
TResult EngineNoSqlPublishHeavyImmutableGeneration(
    const EngineApiRequest& request,
    const std::string& operation_id,
    const std::string& surface,
    const std::string& family,
    const std::string& profile,
    const std::string& operation_evidence_kind) {
  namespace idx = scratchbird::core::index;
  namespace platform = scratchbird::core::platform;

  idx::HeavyImmutableGenerationLedger ledger;
  idx::HeavyImmutableGenerationValidationRequest validation;
  validation.identity.generation_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::object,
      EngineNoSqlOptionValue(request, "heavy_generation.generation_uuid")
          .value_or(std::string{}));
  validation.identity.table_or_collection_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::object,
      EngineNoSqlOptionValue(request,
                             "heavy_generation.table_or_collection_uuid")
          .value_or(request.target_object.uuid.canonical));
  validation.identity.transaction_uuid = EngineNoSqlParseUuid(
      platform::UuidKind::transaction,
      request.context.transaction_uuid.canonical);
  validation.identity.family = family;
  validation.identity.profile = profile;
  const auto source_rows =
      EngineNoSqlOptionU64(request, "heavy_generation.source_row_count");
  validation.source_row_count_present = source_rows.first;
  validation.source_row_count = source_rows.second;
  const auto source_payloads =
      EngineNoSqlOptionU64(request, "heavy_generation.source_payload_count");
  validation.source_payload_count_present = source_payloads.first;
  validation.source_payload_count = source_payloads.second;
  validation.validation_proof_ref =
      EngineNoSqlOptionValue(request, "heavy_generation.validation_proof")
          .value_or(std::string{});
  validation.validation_succeeded =
      EngineNoSqlOptionBool(request, "heavy_generation.validation_succeeded");
  validation.immutable_payload_complete =
      EngineNoSqlOptionBool(request, "heavy_generation.immutable_payload_complete");
  validation.source_counts_verified =
      EngineNoSqlOptionBool(request, "heavy_generation.source_counts_verified");
  validation.checksum_verified =
      EngineNoSqlOptionBool(request, "heavy_generation.checksum_verified");
  validation.engine_mga_authority_evidence_ref =
      EngineNoSqlOptionValue(request, "heavy_generation.mga_authority")
          .value_or(std::string{});
  validation.engine_mga_inventory_evidence_ref =
      EngineNoSqlOptionValue(request, "heavy_generation.mga_inventory")
          .value_or(std::string{});
  validation.operation_id = operation_id;
  validation.exact_diagnostics.push_back("heavy_generation.local_nosql_surface");
  validation.exact_diagnostics.push_back(surface);
  validation.parser_finality_authority =
      EngineNoSqlOptionBool(request, "heavy_generation.parser_finality_authority");
  validation.client_state_authority =
      EngineNoSqlOptionBool(request, "heavy_generation.client_state_authority");
  validation.timestamp_ordering_authority =
      EngineNoSqlOptionBool(request, "heavy_generation.timestamp_ordering_authority");
  validation.uuid_ordering_authority =
      EngineNoSqlOptionBool(request, "heavy_generation.uuid_ordering_authority");
  validation.event_stream_authority =
      EngineNoSqlOptionBool(request, "heavy_generation.event_stream_authority");
  validation.reference_authority =
      EngineNoSqlOptionBool(request, "heavy_generation.reference_authority");
  validation.write_ahead_authority =  // wal-not-authority
      EngineNoSqlOptionBool(request, "heavy_generation.write_ahead_authority");  // wal-not-authority
  validation.cluster_provider_routed = false;

  auto validated = idx::ValidateHeavyImmutableGeneration(&ledger, validation);
  if (!validated.ok()) {
    auto result = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        EngineNoSqlHeavyGenerationDiagnostic(validated.diagnostic));
    AddEngineNoSqlHeavyGenerationRefusalEvidence(&result, validated);
    return result;
  }

  idx::HeavyImmutableGenerationPublicationRequest publication;
  publication.publication_fence_ref =
      EngineNoSqlOptionValue(request, "heavy_generation.publication_fence")
          .value_or(std::string{});
  publication.engine_owned_mga_publication_fence =
      EngineNoSqlOptionBool(request,
                            "heavy_generation.engine_owned_mga_publication_fence");
  publication.authority_source = idx::kHeavyImmutableGenerationAuthoritySource;
  publication.cluster_provider_routed = false;

  auto descriptor = validated.generation;
  auto published =
      idx::PublishHeavyImmutableGeneration(&ledger, &descriptor, publication);
  if (!published.ok()) {
    auto result = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        EngineNoSqlHeavyGenerationDiagnostic(published.diagnostic));
    AddEngineNoSqlHeavyGenerationRefusalEvidence(&result, published);
    return result;
  }

  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  AddEngineNoSqlHeavyGenerationSuccessEvidence(
      &result, published, operation_evidence_kind);
  AddEngineNoSqlSurfaceEvidence(
      &result, surface, "validated_immutable_generation_published");
  return result;
}

}  // namespace scratchbird::engine::internal_api
