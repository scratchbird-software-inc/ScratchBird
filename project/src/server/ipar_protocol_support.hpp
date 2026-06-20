// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "session_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::server {

struct IparSupportEpochVector {
  std::uint64_t catalog_generation = 1;
  std::uint64_t security_epoch = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t grant_epoch = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t cache_invalidation_epoch = 1;
  std::uint64_t name_resolution_epoch = 1;
  std::uint64_t resource_epoch = 1;
  std::string role_set_hash = "roles/default";
  std::string group_set_hash = "groups/default";
  std::string search_path_hash = "search_path/default";
};

struct IparSupportSessionScope {
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> principal_uuid{};
  std::array<std::uint8_t, 16> effective_user_uuid{};
  std::string database_uuid;
  IparSupportEpochVector epoch;
};

struct IparCacheAuthorityFlags {
  bool client_or_parser_authorization_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool grants_authority = false;
};

struct IparCacheStatus {
  bool accepted = false;
  bool cache_hit = false;
  bool selected_specialized_variant = false;
  bool stale = false;
  bool cross_session = false;
  bool cross_authorization = false;
  bool capability_stale = false;
  bool authority_forbidden = false;
  std::string detail;
  std::string cache_key;
  std::string selected_variant_id;
  std::string selected_variant_path;
};

struct IparUuidDependency {
  std::string dependency_kind;
  std::string logical_name;
  std::string object_uuid;
  std::string descriptor_uuid;
  std::string descriptor_hash;
  std::uint64_t catalog_generation = 1;
  std::uint64_t descriptor_epoch = 1;
  std::uint64_t name_resolution_epoch = 1;
};

enum class IparParameterNullabilityClass {
  kUnknown,
  kAllNonNull,
  kNullableSparse,
  kNullableDense,
  kAllNull,
};

enum class IparParameterWidthClass {
  kUnknown,
  kNarrow,
  kMedium,
  kWide,
  kMixed,
};

enum class IparParameterLargeValueClass {
  kUnknown,
  kInlineOnly,
  kLargeValueRefs,
  kMixedInlineAndLarge,
};

enum class IparParameterKeyDistributionClass {
  kUnknown,
  kPointStable,
  kHotKeySkew,
  kRangeClustered,
  kHashDistributed,
};

enum class IparParameterBatchSizeClass {
  kUnknown,
  kSingleRow,
  kSmallBatch,
  kMediumBatch,
  kLargeBatch,
  kBulkBatch,
};

struct IparPreparedParameterSpecializationProfile {
  IparParameterNullabilityClass nullability_class =
      IparParameterNullabilityClass::kUnknown;
  IparParameterWidthClass width_class = IparParameterWidthClass::kUnknown;
  IparParameterLargeValueClass large_value_class =
      IparParameterLargeValueClass::kUnknown;
  IparParameterKeyDistributionClass key_distribution_class =
      IparParameterKeyDistributionClass::kUnknown;
  IparParameterBatchSizeClass batch_size_class =
      IparParameterBatchSizeClass::kUnknown;
  std::uint64_t observed_batch_rows = 0;
};

struct IparPreparedSpecializedVariant {
  std::string variant_id;
  std::string fast_path_label;
  IparPreparedParameterSpecializationProfile profile;
  std::string statement_semantics_hash;
  std::string authorization_context_hash;
  IparCacheAuthorityFlags authority_flags;
  bool changes_statement_semantics = false;
  bool skips_authorization_recheck = false;
  bool parser_or_driver_executes_sql = false;
};

struct IparPreparedTemplatePut {
  IparSupportSessionScope scope;
  std::string operation_id;
  std::string operation_family;
  std::string canonical_sblr_envelope;
  std::string result_descriptor_hash;
  std::string statement_semantics_hash;
  std::string authorization_context_hash;
  std::vector<IparUuidDependency> dependencies;
  std::vector<IparPreparedSpecializedVariant> specialized_variants;
  bool parser_sql_text_present = false;
  bool all_names_resolved_to_uuid = true;
  IparCacheAuthorityFlags authority_flags;
};

struct IparPreparedTemplateRecord {
  std::string cache_key;
  IparSupportSessionScope scope;
  std::string operation_id;
  std::string operation_family;
  std::string canonical_sblr_digest;
  std::string dependency_digest;
  std::string result_descriptor_hash;
  std::string statement_semantics_hash;
  std::string authorization_context_hash;
  std::vector<IparUuidDependency> dependencies;
  std::vector<IparPreparedSpecializedVariant> specialized_variants;
  IparCacheAuthorityFlags authority_flags;
  std::uint64_t generation = 0;
  std::uint64_t hit_count = 0;
  std::uint64_t specialized_variant_hit_count = 0;
};

struct IparPreparedTemplateLookup {
  IparSupportSessionScope scope;
  std::string cache_key;
  std::string operation_id;
  std::string operation_family;
  std::string canonical_sblr_envelope;
  std::string result_descriptor_hash;
  std::string statement_semantics_hash;
  std::string authorization_context_hash;
  std::vector<IparUuidDependency> dependencies;
  IparPreparedParameterSpecializationProfile observed_parameter_profile;
};

struct IparResolvedDescriptorPut {
  IparSupportSessionScope scope;
  std::string cache_key;
  std::string resolved_name;
  std::string object_uuid;
  std::string object_kind;
  std::string descriptor_uuid;
  std::string descriptor_hash;
  std::string operation_id;
  IparCacheAuthorityFlags authority_flags;
};

struct IparDescriptorRecord {
  std::string cache_key;
  IparSupportSessionScope scope;
  std::string resolved_name;
  std::string object_uuid;
  std::string object_kind;
  std::string descriptor_uuid;
  std::string descriptor_hash;
  std::string operation_id;
  IparCacheAuthorityFlags authority_flags;
  std::uint64_t generation = 0;
  std::uint64_t hit_count = 0;
};

enum class IparProtocolOperationClass {
  kDml,
  kDdl,
  kQuery,
  kManagement,
};

struct IparProtocolBatchRequest {
  IparSupportSessionScope scope;
  IparProtocolOperationClass operation_class = IparProtocolOperationClass::kDml;
  std::string operation_id;
  std::uint64_t requested_rows = 0;
  std::uint64_t estimated_payload_bytes = 0;
  std::uint64_t max_batch_rows = 1;
  std::uint64_t max_frame_bytes = 65536;
  std::uint64_t driver_capability_generation = 1;
  std::uint64_t server_capability_generation = 1;
  bool driver_supports_batching = true;
  bool metadata_cache_advisory = true;
  bool contains_dml = false;
  bool contains_ddl = false;
  bool parser_or_driver_finality_authority = false;
};

struct IparBatchWindow {
  std::uint64_t first_row = 0;
  std::uint64_t row_count = 0;
  std::uint64_t estimated_bytes = 0;
};

struct IparProtocolBatchPlan {
  bool accepted = false;
  bool server_revalidation_required = true;
  bool metadata_cache_advisory = true;
  bool driver_batching_enabled = false;
  bool dml_ddl_mixed_refused = false;
  std::string detail;
  std::string capability_cache_key;
  std::vector<IparBatchWindow> windows;
};

struct IparFrameCryptoRequest {
  IparSupportSessionScope scope;
  std::string frame_class = "execute";
  std::string requested_reuse_key;
  std::uint64_t payload_capacity = 0;
  std::uint64_t crypto_epoch = 1;
  std::uint64_t handshake_generation = 1;
  std::string tls_policy_hash = "tls/default";
  std::string key_lineage_id = "key-lineage/default";
  bool contains_plaintext_database_key_material = false;
  bool parser_or_driver_authority = false;
};

struct IparFrameCryptoLease {
  bool accepted = false;
  bool frame_reused = false;
  bool crypto_context_reused = false;
  bool stores_plaintext_key_material = false;
  std::string detail;
  std::string reuse_key;
  std::uint64_t payload_capacity = 0;
};

struct IparResultBufferRequest {
  IparSupportSessionScope scope;
  std::string requested_reuse_key;
  std::string operation_id;
  std::string result_shape_hash;
  std::string target_object_uuid;
  std::uint64_t minimum_capacity = 0;
  bool returning_path = false;
  bool parser_or_driver_finality_authority = false;
};

struct IparResultBufferLease {
  bool accepted = false;
  bool cache_hit = false;
  bool returning_path = false;
  bool finality_authority = false;
  std::string detail;
  std::string reuse_key;
  std::uint64_t capacity = 0;
};

struct IparFailureClassificationRequest {
  IparSupportSessionScope scope;
  std::string requested_cache_key;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string operation_id;
  std::string statement_shape_hash;
  bool engine_finality_known = true;
  bool parser_or_driver_finality_authority = false;
};

struct IparFailureClassificationResult {
  bool accepted = false;
  bool cache_hit = false;
  bool retryable = false;
  bool requires_mga_inventory_check = false;
  bool ends_transaction = false;
  bool finality_authority = false;
  std::string detail;
  std::string cache_key;
  std::string failure_class;
};

struct IparProtocolSupportMetrics {
  std::uint64_t prepared_template_hits = 0;
  std::uint64_t prepared_template_misses = 0;
  std::uint64_t prepared_specialized_variant_hits = 0;
  std::uint64_t prepared_specialized_variant_misses = 0;
  std::uint64_t descriptor_hits = 0;
  std::uint64_t descriptor_misses = 0;
  std::uint64_t stale_rejections = 0;
  std::uint64_t authority_rejections = 0;
  std::uint64_t frame_reuses = 0;
  std::uint64_t crypto_reuses = 0;
  std::uint64_t result_buffer_reuses = 0;
  std::uint64_t failure_cache_hits = 0;
};

IparSupportSessionScope IparSupportScopeForSession(
    const ServerSessionRecord& session);

class IparServerProtocolSupport {
 public:
  IparCacheStatus StorePreparedTemplate(IparPreparedTemplatePut request);
  IparCacheStatus LookupPreparedTemplate(
      const IparPreparedTemplateLookup& request);

  IparCacheStatus StoreResolvedDescriptor(IparResolvedDescriptorPut request);
  IparCacheStatus LookupResolvedDescriptor(
      const IparResolvedDescriptorPut& request);

  IparProtocolBatchPlan PlanProtocolBatch(
      const IparProtocolBatchRequest& request) const;

  IparFrameCryptoLease AcquireFrameCryptoLease(
      const IparFrameCryptoRequest& request);
  IparCacheStatus ReleaseFrameCryptoLease(
      const IparSupportSessionScope& scope,
      const std::string& reuse_key);

  IparResultBufferLease AcquireResultBuffer(
      const IparResultBufferRequest& request);
  IparCacheStatus ReleaseResultBuffer(const IparSupportSessionScope& scope,
                                      const std::string& reuse_key);

  IparFailureClassificationResult ClassifyFailure(
      const IparFailureClassificationRequest& request);
  IparFailureClassificationResult LookupFailureClassification(
      const IparFailureClassificationRequest& request);

  const IparProtocolSupportMetrics& metrics() const { return metrics_; }
  const std::map<std::string, IparPreparedTemplateRecord>&
  prepared_templates() const {
    return prepared_templates_by_key_;
  }
  const std::map<std::string, IparDescriptorRecord>& descriptors() const {
    return descriptors_by_key_;
  }

 private:
  struct PoolEntry {
    IparSupportSessionScope scope;
    std::string reuse_key;
    std::uint64_t capacity = 0;
    std::uint64_t crypto_epoch = 0;
    std::uint64_t handshake_generation = 0;
    std::string tls_policy_hash;
    std::string key_lineage_id;
    std::uint64_t available = 0;
  };

  struct FailureRecord {
    IparSupportSessionScope scope;
    std::string cache_key;
    std::string failure_class;
    std::string diagnostic_code;
    std::string diagnostic_detail;
    std::string operation_id;
    std::string statement_shape_hash;
    bool retryable = false;
    bool requires_mga_inventory_check = false;
    bool ends_transaction = false;
    bool finality_authority = false;
    std::uint64_t hit_count = 0;
  };

  IparCacheStatus ValidateScope(const IparSupportSessionScope& cached,
                                const IparSupportSessionScope& current) const;
  IparCacheStatus ValidateAuthorityFlags(
      const IparCacheAuthorityFlags& flags) const;
  IparFailureClassificationResult MakeFailureResult(
      const FailureRecord& record,
      bool cache_hit) const;

  std::map<std::string, IparPreparedTemplateRecord> prepared_templates_by_key_;
  std::map<std::string, IparDescriptorRecord> descriptors_by_key_;
  std::map<std::string, PoolEntry> frame_pool_by_key_;
  std::map<std::string, PoolEntry> result_buffer_pool_by_key_;
  std::map<std::string, FailureRecord> failures_by_key_;
  std::uint64_t next_generation_ = 1;
  IparProtocolSupportMetrics metrics_;
};

}  // namespace scratchbird::server
