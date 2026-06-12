// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-FILTER-ACCESS-CLOSURE-ANCHOR
#include "index_family_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class HashCollisionDecision {
  exact_match,
  collision_requires_key_recheck,
  reject_mismatched_hash,
  reject_mismatched_key
};

enum class HashRehashDecision {
  keep_directory,
  split_bucket,
  merge_bucket,
  grow_directory,
  shrink_directory,
  rebuild_required
};

enum class HashDuplicateDecision {
  allow_insert,
  reject_visible_duplicate,
  wait_for_mga_resolution,
  allow_same_row_update,
  policy_refusal
};

enum class FilterRecheckDecision {
  exact_accept,
  prune,
  candidate_requires_exact_recheck,
  policy_blocked
};

enum class ApproximationDisclosureLevel {
  exact,
  lossy_with_recheck,
  approximate_candidates,
  approximate_result
};

enum class RedisEqualityAliasKind {
  string_key,
  hash_field,
  set_member,
  zset_member,
  bitmap_bit,
  unsupported
};

enum class BitmapOperationKind {
  equality,
  in_list,
  set_and,
  set_or,
  set_not,
  cleanup_deleted
};

enum class CompressedBitmapEncoding {
  empty,
  literal_bits,
  rle_runs,
  sparse_positions
};

enum class RangePruneDecision {
  prune_all,
  maybe_with_recheck,
  accept_exact,
  stale_resummarize,
  policy_blocked
};

enum class ClickHouseSkipVariant {
  minmax,
  set,
  bloom_filter,
  tokenbf,
  ngrambf,
  sparse_grams,
  hypothesis,
  unsupported
};

enum class DuckDbMappingKind {
  zonemap_summary,
  art_ordered_alias,
  art_equality_alias,
  unsupported
};

enum class AggregateSketchKind {
  exact_aggregate,
  count_min,
  hyperloglog,
  quantile,
  top_k,
  reservoir,
  unsupported
};

struct HashEqualityProbe {
  u64 requested_hash = 0;
  u64 stored_hash = 0;
  std::string encoded_key;
  std::string stored_encoded_key;
  bool stored_key_present = true;
  bool allow_collision_recheck = true;
};

struct HashEqualityResult {
  HashCollisionDecision decision = HashCollisionDecision::reject_mismatched_hash;
  bool hash_matches = false;
  bool key_matches = false;
  bool may_return_candidate = false;
  bool requires_key_recheck = false;
  std::string reason_code;
};

struct HashBucketState {
  u64 directory_depth = 0;
  u64 bucket_depth = 0;
  u64 bucket_count = 1;
  u64 tuple_count = 0;
  u64 overflow_pages = 0;
  u64 tombstones = 0;
  u64 max_bucket_tuples = 256;
  u64 min_bucket_tuples = 32;
  u64 max_overflow_pages = 2;
  bool split_or_merge_in_progress = false;
  bool directory_bucket_mismatch = false;
  bool checksum_valid = true;
};

struct HashRehashPlan {
  HashRehashDecision decision = HashRehashDecision::keep_directory;
  bool requires_exclusive_latch = false;
  bool verify_after_rehash = false;
  std::string reason_code;
};

struct HashUniquePolicy {
  bool unique_requested = false;
  bool family_allows_unique = true;
  bool require_mga_duplicate_check = true;
  bool allow_same_row_version_update = true;
};

struct HashDuplicateObservation {
  bool same_encoded_key = false;
  bool same_logical_row = false;
  bool visibility_checked = true;
  bool visible_committed_duplicate = false;
  bool active_uncommitted_duplicate = false;
  bool deleted_or_garbage_duplicate = false;
};

struct HashDuplicateResult {
  HashDuplicateDecision decision = HashDuplicateDecision::allow_insert;
  bool requires_mga_recheck = false;
  bool conflict = false;
  std::string reason_code;
};

struct BloomFilterProbe {
  bool filter_may_contain = false;
  bool exact_recheck_available = true;
  bool false_positive_disclosed = true;
  bool policy_accepts_lossy_filter = true;
};

struct FilterProbeResult {
  FilterRecheckDecision decision = FilterRecheckDecision::prune;
  bool may_prune = true;
  bool requires_exact_recheck = false;
  ApproximationDisclosureLevel disclosure = ApproximationDisclosureLevel::exact;
  std::string reason_code;
};

struct MinHashLshProbe {
  u32 matching_bands = 0;
  u32 required_bands = 1;
  double estimated_similarity = 0.0;
  double similarity_threshold = 0.0;
  bool approximate_semantics_accepted = true;
  bool exact_rerank_available = true;
  bool result_is_candidate_only = true;
};

struct ApproximateCandidateDecision {
  bool may_emit_candidate = false;
  bool requires_exact_rerank = false;
  bool approximate_result_allowed = false;
  ApproximationDisclosureLevel disclosure = ApproximationDisclosureLevel::approximate_candidates;
  std::string reason_code;
};

struct RedisEqualityAliasRequest {
  RedisEqualityAliasKind kind = RedisEqualityAliasKind::unsupported;
  std::string command_or_type;
  bool native_helper_enabled = true;
  bool reference_emulation_enabled = true;
  bool exact_semantics_required = true;
};

struct RedisEqualityAliasPlan {
  IndexFamily family = IndexFamily::policy_blocked;
  std::string semantic_profile;
  bool accepted = false;
  bool requires_recheck = true;
  bool policy_refusal = true;
  std::string reason_code;
};

struct BitmapSegmentDescriptor {
  CompressedBitmapEncoding encoding = CompressedBitmapEncoding::empty;
  u64 first_row_ordinal = 0;
  u64 row_count = 0;
  u64 set_bits = 0;
  u64 deleted_bits = 0;
  bool sorted = true;
  bool checksum_valid = true;
};

struct BitmapOperationRequest {
  BitmapOperationKind operation = BitmapOperationKind::equality;
  std::vector<BitmapSegmentDescriptor> inputs;
  bool low_cardinality_key = true;
  bool exact_visibility_recheck = true;
};

struct BitmapOperationPlan {
  CompressedBitmapEncoding output_encoding = CompressedBitmapEncoding::empty;
  bool can_execute = false;
  bool requires_visibility_recheck = true;
  bool requires_cleanup = false;
  u64 estimated_set_bits = 0;
  std::string reason_code;
};

struct RangeSummaryBounds {
  std::string encoded_min;
  std::string encoded_max;
  bool has_nulls = false;
  bool all_nulls = false;
  bool stale = false;
  bool min_max_valid = false;
  u64 rows_summarized = 0;
  u64 rows_appended_after_summary = 0;
};

struct RangePredicateProbe {
  std::string encoded_low;
  std::string encoded_high;
  bool low_inclusive = true;
  bool high_inclusive = true;
  bool predicate_on_null = false;
  bool exact_recheck_available = true;
};

struct RangeSummaryDecision {
  RangePruneDecision decision = RangePruneDecision::maybe_with_recheck;
  bool can_prune = false;
  bool requires_lossy_recheck = true;
  bool should_resummarize = false;
  ApproximationDisclosureLevel disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
  std::string reason_code;
};

struct ClickHouseSkipIndexRequest {
  ClickHouseSkipVariant variant = ClickHouseSkipVariant::unsupported;
  bool tokenized_predicate = false;
  bool ngram_predicate = false;
  bool sparse_candidate = false;
  bool hypothesis_accepted = false;
  bool exact_recheck_available = true;
};

struct ClickHouseSkipIndexPlan {
  IndexFamily family = IndexFamily::policy_blocked;
  std::string semantic_profile;
  bool accepted = false;
  bool can_prune = false;
  bool requires_recheck = true;
  ApproximationDisclosureLevel disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
  std::string reason_code;
};

struct DuckDbIndexMappingRequest {
  DuckDbMappingKind kind = DuckDbMappingKind::unsupported;
  bool unique_constraint = false;
  bool ordered_lookup = false;
  bool zonemap_stale = false;
  bool exact_recheck_available = true;
};

struct DuckDbIndexMappingPlan {
  IndexFamily family = IndexFamily::policy_blocked;
  std::string semantic_profile;
  bool accepted = false;
  bool requires_recheck = true;
  bool preserves_art_identity_metrics = false;
  std::string reason_code;
};

struct ColumnarZoneSegmentState {
  RangeSummaryBounds summary;
  u64 segment_id = 0;
  bool segment_metadata_present = true;
  bool segment_deleted = false;
  bool compressed_zone = true;
};

struct ColumnarZonePrunePlan {
  RangeSummaryDecision summary_decision;
  bool segment_pruned = false;
  bool segment_scan_required = true;
  bool metrics_count_pruned_segment = false;
  std::string reason_code;
};

struct AggregateSketchRequest {
  AggregateSketchKind kind = AggregateSketchKind::unsupported;
  bool approximate_allowed = false;
  bool privacy_policy_allows_export = false;
  bool merge_supported = false;
  bool finalize_supported = false;
  bool exact_recheck_available = false;
};

struct AggregateSketchPlan {
  IndexFamily family = IndexFamily::columnar_zone;
  bool accepted = false;
  bool merge_compatible = false;
  bool finalize_compatible = false;
  bool discloses_approximation = false;
  bool privacy_policy_refusal = false;
  ApproximationDisclosureLevel disclosure = ApproximationDisclosureLevel::approximate_result;
  std::string reason_code;
};

HashEqualityResult DecideHashEqualityProbe(const HashEqualityProbe& probe);
HashRehashPlan DecideHashRehash(const HashBucketState& state);
HashDuplicateResult DecideHashUniqueDuplicate(const HashUniquePolicy& policy,
                                              const HashDuplicateObservation& observation);
FilterProbeResult DecideBloomFilterProbe(const BloomFilterProbe& probe);
ApproximateCandidateDecision DecideMinHashLshCandidate(const MinHashLshProbe& probe);
RedisEqualityAliasPlan ResolveRedisEqualityAlias(const RedisEqualityAliasRequest& request);
BitmapOperationPlan PlanBitmapOperation(const BitmapOperationRequest& request);
RangeSummaryDecision DecideRangeSummaryPrune(const RangeSummaryBounds& summary,
                                             const RangePredicateProbe& predicate);
ClickHouseSkipIndexPlan ResolveClickHouseSkipIndex(const ClickHouseSkipIndexRequest& request);
DuckDbIndexMappingPlan ResolveDuckDbIndexMapping(const DuckDbIndexMappingRequest& request);
ColumnarZonePrunePlan PlanColumnarZonePrune(const ColumnarZoneSegmentState& segment,
                                            const RangePredicateProbe& predicate);
AggregateSketchPlan PlanAggregateSketch(const AggregateSketchRequest& request);

const char* HashCollisionDecisionName(HashCollisionDecision decision);
const char* HashRehashDecisionName(HashRehashDecision decision);
const char* HashDuplicateDecisionName(HashDuplicateDecision decision);
const char* FilterRecheckDecisionName(FilterRecheckDecision decision);
const char* ApproximationDisclosureLevelName(ApproximationDisclosureLevel disclosure);
const char* RangePruneDecisionName(RangePruneDecision decision);

}  // namespace scratchbird::core::index
