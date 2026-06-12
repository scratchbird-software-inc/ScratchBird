// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_filter_access.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::core::index {
namespace {

u64 SaturatingAdd(u64 lhs, u64 rhs) {
  const u64 max = std::numeric_limits<u64>::max();
  return lhs > max - rhs ? max : lhs + rhs;
}

u64 SaturatingSub(u64 lhs, u64 rhs) { return lhs > rhs ? lhs - rhs : 0; }

bool NeedsHashGrowth(const HashBucketState& state) {
  return state.tuple_count > state.max_bucket_tuples ||
         state.overflow_pages > state.max_overflow_pages;
}

bool NeedsHashCompaction(const HashBucketState& state) {
  if (state.bucket_count <= 1) return false;
  if (state.tuple_count <= state.min_bucket_tuples) return true;
  return state.tombstones > state.tuple_count && state.tombstones >= state.min_bucket_tuples;
}

u64 MaxRowCount(const std::vector<BitmapSegmentDescriptor>& inputs) {
  u64 row_count = 0;
  for (const auto& input : inputs) {
    row_count = std::max(row_count, input.row_count);
  }
  return row_count;
}

u64 SumSetBits(const std::vector<BitmapSegmentDescriptor>& inputs) {
  u64 set_bits = 0;
  for (const auto& input : inputs) {
    set_bits = SaturatingAdd(set_bits, input.set_bits);
  }
  return set_bits;
}

u64 MinSetBits(const std::vector<BitmapSegmentDescriptor>& inputs) {
  if (inputs.empty()) return 0;
  u64 set_bits = inputs.front().set_bits;
  for (const auto& input : inputs) {
    set_bits = std::min(set_bits, input.set_bits);
  }
  return set_bits;
}

bool HasInvalidBitmapInput(const std::vector<BitmapSegmentDescriptor>& inputs) {
  for (const auto& input : inputs) {
    if (!input.sorted || !input.checksum_valid || input.set_bits > input.row_count) return true;
  }
  return false;
}

bool HasDeletedBitmapInput(const std::vector<BitmapSegmentDescriptor>& inputs) {
  for (const auto& input : inputs) {
    if (input.deleted_bits != 0) return true;
  }
  return false;
}

CompressedBitmapEncoding ChooseBitmapEncoding(u64 set_bits, u64 row_count) {
  if (set_bits == 0 || row_count == 0) return CompressedBitmapEncoding::empty;
  if (set_bits <= std::max<u64>(1, row_count / 16)) return CompressedBitmapEncoding::sparse_positions;
  if (set_bits >= row_count - (row_count / 16)) return CompressedBitmapEncoding::rle_runs;
  if (set_bits >= row_count / 2) return CompressedBitmapEncoding::rle_runs;
  return CompressedBitmapEncoding::literal_bits;
}

bool StrictlyBelow(const std::string& lhs, const std::string& rhs, bool rhs_inclusive) {
  if (rhs.empty()) return false;
  return rhs_inclusive ? lhs < rhs : lhs <= rhs;
}

bool StrictlyAbove(const std::string& lhs, const std::string& rhs, bool rhs_inclusive) {
  if (rhs.empty()) return false;
  return rhs_inclusive ? lhs > rhs : lhs >= rhs;
}

bool RangeCanPrune(const RangeSummaryBounds& summary, const RangePredicateProbe& predicate) {
  if (!summary.min_max_valid) return false;
  const bool summary_below_predicate = StrictlyBelow(summary.encoded_max, predicate.encoded_low, predicate.low_inclusive);
  const bool summary_above_predicate = StrictlyAbove(summary.encoded_min, predicate.encoded_high, predicate.high_inclusive);
  return summary_below_predicate || summary_above_predicate;
}

bool AppendDeltaRequiresResummarize(const RangeSummaryBounds& summary) {
  if (summary.rows_appended_after_summary == 0) return false;
  if (summary.rows_summarized == 0) return true;
  return summary.rows_appended_after_summary * 8 >= summary.rows_summarized;
}

FilterProbeResult PolicyBlockedFilter(std::string reason) {
  FilterProbeResult result;
  result.decision = FilterRecheckDecision::policy_blocked;
  result.may_prune = false;
  result.requires_exact_recheck = true;
  result.disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
  result.reason_code = std::move(reason);
  return result;
}

ClickHouseSkipIndexPlan BlockedClickHousePlan(std::string reason) {
  ClickHouseSkipIndexPlan plan;
  plan.family = IndexFamily::policy_blocked;
  plan.semantic_profile = "policy_blocked";
  plan.accepted = false;
  plan.can_prune = false;
  plan.requires_recheck = true;
  plan.disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
  plan.reason_code = std::move(reason);
  return plan;
}

AggregateSketchPlan BlockedSketchPlan(std::string reason, bool privacy_refusal) {
  AggregateSketchPlan plan;
  plan.family = IndexFamily::columnar_zone;
  plan.accepted = false;
  plan.merge_compatible = false;
  plan.finalize_compatible = false;
  plan.discloses_approximation = true;
  plan.privacy_policy_refusal = privacy_refusal;
  plan.disclosure = ApproximationDisclosureLevel::approximate_result;
  plan.reason_code = std::move(reason);
  return plan;
}

}  // namespace

HashEqualityResult DecideHashEqualityProbe(const HashEqualityProbe& probe) {
  HashEqualityResult result;
  result.hash_matches = probe.requested_hash == probe.stored_hash;
  if (!result.hash_matches) {
    result.decision = HashCollisionDecision::reject_mismatched_hash;
    result.reason_code = "hash_mismatch_pruned";
    return result;
  }

  if (probe.stored_key_present) {
    result.key_matches = probe.encoded_key == probe.stored_encoded_key;
    if (result.key_matches) {
      result.decision = HashCollisionDecision::exact_match;
      result.may_return_candidate = true;
      result.requires_key_recheck = false;
      result.reason_code = "hash_and_key_match";
      return result;
    }
    result.decision = HashCollisionDecision::reject_mismatched_key;
    result.reason_code = "hash_collision_key_mismatch";
    return result;
  }

  if (probe.allow_collision_recheck) {
    result.decision = HashCollisionDecision::collision_requires_key_recheck;
    result.may_return_candidate = true;
    result.requires_key_recheck = true;
    result.reason_code = "hash_match_key_recheck_required";
    return result;
  }

  result.decision = HashCollisionDecision::reject_mismatched_key;
  result.reason_code = "hash_collision_recheck_unavailable";
  return result;
}

HashRehashPlan DecideHashRehash(const HashBucketState& state) {
  HashRehashPlan plan;
  plan.reason_code = "hash_directory_stable";

  if (!state.checksum_valid || state.directory_bucket_mismatch || state.split_or_merge_in_progress) {
    plan.decision = HashRehashDecision::rebuild_required;
    plan.requires_exclusive_latch = true;
    plan.verify_after_rehash = true;
    plan.reason_code = !state.checksum_valid ? "hash_checksum_invalid"
                                             : (state.directory_bucket_mismatch
                                                    ? "hash_directory_bucket_mismatch"
                                                    : "hash_interrupted_split_or_merge");
    return plan;
  }

  if (NeedsHashGrowth(state)) {
    plan.requires_exclusive_latch = true;
    plan.verify_after_rehash = true;
    if (state.bucket_depth >= state.directory_depth) {
      plan.decision = HashRehashDecision::grow_directory;
      plan.reason_code = "hash_bucket_full_grow_directory";
    } else {
      plan.decision = HashRehashDecision::split_bucket;
      plan.reason_code = "hash_bucket_full_split_bucket";
    }
    return plan;
  }

  if (NeedsHashCompaction(state)) {
    plan.requires_exclusive_latch = true;
    plan.verify_after_rehash = true;
    if (state.bucket_depth + 1 < state.directory_depth) {
      plan.decision = HashRehashDecision::shrink_directory;
      plan.reason_code = "hash_directory_sparse_shrink";
    } else {
      plan.decision = HashRehashDecision::merge_bucket;
      plan.reason_code = "hash_bucket_sparse_merge";
    }
    return plan;
  }

  return plan;
}

HashDuplicateResult DecideHashUniqueDuplicate(const HashUniquePolicy& policy,
                                              const HashDuplicateObservation& observation) {
  HashDuplicateResult result;
  result.requires_mga_recheck = policy.unique_requested && policy.require_mga_duplicate_check;

  if (!policy.unique_requested) {
    result.reason_code = "hash_unique_not_requested";
    result.requires_mga_recheck = false;
    return result;
  }

  if (!policy.family_allows_unique) {
    result.decision = HashDuplicateDecision::policy_refusal;
    result.conflict = true;
    result.reason_code = "hash_unique_policy_refusal";
    return result;
  }

  if (!observation.same_encoded_key) {
    result.reason_code = "hash_unique_no_duplicate_key";
    return result;
  }

  if (observation.same_logical_row && policy.allow_same_row_version_update) {
    result.decision = HashDuplicateDecision::allow_same_row_update;
    result.reason_code = "hash_unique_same_row_update";
    return result;
  }

  if (!observation.visibility_checked || observation.active_uncommitted_duplicate) {
    result.decision = HashDuplicateDecision::wait_for_mga_resolution;
    result.requires_mga_recheck = true;
    result.reason_code = !observation.visibility_checked ? "hash_unique_visibility_unknown"
                                                         : "hash_unique_uncommitted_duplicate";
    return result;
  }

  if (observation.visible_committed_duplicate) {
    result.decision = HashDuplicateDecision::reject_visible_duplicate;
    result.conflict = true;
    result.reason_code = "hash_unique_visible_duplicate";
    return result;
  }

  if (observation.deleted_or_garbage_duplicate) {
    result.reason_code = "hash_unique_garbage_duplicate_ignored";
    return result;
  }

  result.reason_code = "hash_unique_duplicate_check_passed";
  return result;
}

FilterProbeResult DecideBloomFilterProbe(const BloomFilterProbe& probe) {
  if (!probe.policy_accepts_lossy_filter) {
    return PolicyBlockedFilter("bloom_lossy_filter_policy_refusal");
  }
  if (!probe.false_positive_disclosed) {
    return PolicyBlockedFilter("bloom_false_positive_disclosure_required");
  }
  if (!probe.filter_may_contain) {
    FilterProbeResult result;
    result.decision = FilterRecheckDecision::prune;
    result.may_prune = true;
    result.requires_exact_recheck = false;
    result.disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
    result.reason_code = "bloom_negative_prune";
    return result;
  }
  if (!probe.exact_recheck_available) {
    return PolicyBlockedFilter("bloom_positive_requires_exact_recheck");
  }

  FilterProbeResult result;
  result.decision = FilterRecheckDecision::candidate_requires_exact_recheck;
  result.may_prune = false;
  result.requires_exact_recheck = true;
  result.disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
  result.reason_code = "bloom_positive_exact_recheck";
  return result;
}

ApproximateCandidateDecision DecideMinHashLshCandidate(const MinHashLshProbe& probe) {
  ApproximateCandidateDecision result;
  if (!probe.approximate_semantics_accepted) {
    result.reason_code = "minhash_lsh_policy_refusal";
    return result;
  }

  const bool band_match = probe.matching_bands >= probe.required_bands;
  const bool similarity_match = probe.estimated_similarity >= probe.similarity_threshold;
  if (!band_match && !similarity_match) {
    result.reason_code = "minhash_lsh_candidate_pruned";
    return result;
  }

  result.may_emit_candidate = true;
  result.requires_exact_rerank = probe.exact_rerank_available;
  result.approximate_result_allowed = !probe.result_is_candidate_only && !probe.exact_rerank_available;
  result.disclosure = result.approximate_result_allowed ? ApproximationDisclosureLevel::approximate_result
                                                        : ApproximationDisclosureLevel::approximate_candidates;
  result.reason_code = result.requires_exact_rerank ? "minhash_lsh_candidate_exact_rerank"
                                                    : (result.approximate_result_allowed
                                                           ? "minhash_lsh_approximate_result_disclosed"
                                                           : "minhash_lsh_candidate_without_exact_rerank");
  if (probe.result_is_candidate_only && !probe.exact_rerank_available) {
    result.may_emit_candidate = false;
    result.reason_code = "minhash_lsh_exact_rerank_required";
  }
  return result;
}

RedisEqualityAliasPlan ResolveRedisEqualityAlias(const RedisEqualityAliasRequest& request) {
  RedisEqualityAliasPlan plan;
  const bool implementation_available = request.native_helper_enabled || request.reference_emulation_enabled;
  if (!implementation_available) {
    plan.reason_code = "redis_equality_alias_no_enabled_implementation";
    return plan;
  }

  plan.accepted = true;
  plan.policy_refusal = false;
  plan.requires_recheck = request.exact_semantics_required;

  switch (request.kind) {
    case RedisEqualityAliasKind::string_key:
      plan.family = IndexFamily::hash;
      plan.semantic_profile = "redis_string_key_equality_profile";
      plan.reason_code = "redis_string_key_hash_alias";
      return plan;
    case RedisEqualityAliasKind::hash_field:
      plan.family = IndexFamily::hash;
      plan.semantic_profile = "redis_hash_field_equality_profile";
      plan.reason_code = "redis_hash_field_hash_alias";
      return plan;
    case RedisEqualityAliasKind::set_member:
      plan.family = IndexFamily::hash;
      plan.semantic_profile = "redis_set_member_equality_profile";
      plan.reason_code = "redis_set_member_hash_alias";
      return plan;
    case RedisEqualityAliasKind::zset_member:
      plan.family = IndexFamily::hash;
      plan.semantic_profile = "redis_zset_member_equality_profile";
      plan.reason_code = "redis_zset_member_hash_alias";
      return plan;
    case RedisEqualityAliasKind::bitmap_bit:
      plan.family = IndexFamily::bitmap;
      plan.semantic_profile = "redis_bitmap_bit_profile";
      plan.reason_code = "redis_bitmap_native_bitmap_alias";
      return plan;
    case RedisEqualityAliasKind::unsupported:
      break;
  }

  plan.family = IndexFamily::policy_blocked;
  plan.semantic_profile = "policy_blocked";
  plan.accepted = false;
  plan.policy_refusal = true;
  plan.reason_code = request.command_or_type.empty() ? "redis_equality_alias_unsupported"
                                                     : "redis_equality_alias_unsupported_type";
  return plan;
}

BitmapOperationPlan PlanBitmapOperation(const BitmapOperationRequest& request) {
  BitmapOperationPlan plan;
  plan.requires_visibility_recheck = request.exact_visibility_recheck;
  plan.requires_cleanup = HasDeletedBitmapInput(request.inputs) ||
                          request.operation == BitmapOperationKind::cleanup_deleted;

  if (request.inputs.empty()) {
    plan.reason_code = "bitmap_operation_requires_input";
    return plan;
  }
  if (HasInvalidBitmapInput(request.inputs)) {
    plan.reason_code = "bitmap_input_invalid";
    return plan;
  }
  if ((request.operation == BitmapOperationKind::equality ||
       request.operation == BitmapOperationKind::in_list) &&
      !request.low_cardinality_key) {
    plan.reason_code = "bitmap_low_cardinality_key_required";
    return plan;
  }

  const u64 max_rows = MaxRowCount(request.inputs);
  switch (request.operation) {
    case BitmapOperationKind::equality:
    case BitmapOperationKind::in_list:
      plan.estimated_set_bits = std::min(SumSetBits(request.inputs), max_rows);
      break;
    case BitmapOperationKind::set_and:
      plan.estimated_set_bits = MinSetBits(request.inputs);
      break;
    case BitmapOperationKind::set_or:
      plan.estimated_set_bits = std::min(SumSetBits(request.inputs), max_rows);
      break;
    case BitmapOperationKind::set_not:
      plan.estimated_set_bits = request.inputs.empty()
                                    ? 0
                                    : SaturatingSub(request.inputs.front().row_count,
                                                    request.inputs.front().set_bits);
      break;
    case BitmapOperationKind::cleanup_deleted:
      plan.estimated_set_bits = request.inputs.empty()
                                    ? 0
                                    : SaturatingSub(request.inputs.front().set_bits,
                                                    request.inputs.front().deleted_bits);
      break;
  }

  plan.output_encoding = ChooseBitmapEncoding(plan.estimated_set_bits, max_rows);
  plan.can_execute = true;
  plan.reason_code = plan.requires_cleanup ? "bitmap_operation_with_cleanup"
                                           : "bitmap_operation_planned";
  return plan;
}

RangeSummaryDecision DecideRangeSummaryPrune(const RangeSummaryBounds& summary,
                                             const RangePredicateProbe& predicate) {
  RangeSummaryDecision result;
  result.should_resummarize = summary.stale || AppendDeltaRequiresResummarize(summary);

  if (summary.all_nulls && !predicate.predicate_on_null) {
    result.decision = RangePruneDecision::prune_all;
    result.can_prune = true;
    result.requires_lossy_recheck = false;
    result.disclosure = ApproximationDisclosureLevel::lossy_with_recheck;
    result.reason_code = "range_summary_all_nulls_pruned";
    return result;
  }

  if (!summary.min_max_valid) {
    result.decision = summary.stale ? RangePruneDecision::stale_resummarize
                                    : RangePruneDecision::maybe_with_recheck;
    result.requires_lossy_recheck = true;
    result.reason_code = summary.stale ? "range_summary_stale_resummarize"
                                       : "range_summary_missing_bounds";
    return result;
  }

  if (RangeCanPrune(summary, predicate) && !summary.stale) {
    result.decision = RangePruneDecision::prune_all;
    result.can_prune = true;
    result.requires_lossy_recheck = false;
    result.reason_code = "range_summary_disjoint_pruned";
    return result;
  }

  if (!predicate.exact_recheck_available) {
    result.decision = RangePruneDecision::policy_blocked;
    result.requires_lossy_recheck = true;
    result.reason_code = "range_summary_requires_exact_recheck";
    return result;
  }

  result.decision = summary.stale ? RangePruneDecision::stale_resummarize
                                  : RangePruneDecision::maybe_with_recheck;
  result.requires_lossy_recheck = true;
  result.reason_code = summary.stale ? "range_summary_stale_scan_recheck"
                                     : "range_summary_overlap_recheck";
  return result;
}

ClickHouseSkipIndexPlan ResolveClickHouseSkipIndex(const ClickHouseSkipIndexRequest& request) {
  if (!request.exact_recheck_available) {
    return BlockedClickHousePlan("clickhouse_skip_requires_exact_recheck");
  }

  ClickHouseSkipIndexPlan plan;
  plan.family = IndexFamily::columnar_zone;
  plan.accepted = true;
  plan.can_prune = true;
  plan.requires_recheck = true;
  plan.disclosure = ApproximationDisclosureLevel::lossy_with_recheck;

  switch (request.variant) {
    case ClickHouseSkipVariant::minmax:
      plan.semantic_profile = "clickhouse_minmax_profile";
      plan.reason_code = "clickhouse_minmax_zone_summary";
      return plan;
    case ClickHouseSkipVariant::set:
      plan.semantic_profile = "clickhouse_set_skip_profile";
      plan.reason_code = "clickhouse_set_skip_summary";
      return plan;
    case ClickHouseSkipVariant::bloom_filter:
      plan.family = IndexFamily::bloom;
      plan.semantic_profile = "clickhouse_bloom_filter_profile";
      plan.reason_code = "clickhouse_bloom_false_positive_recheck";
      return plan;
    case ClickHouseSkipVariant::tokenbf:
      if (!request.tokenized_predicate) return BlockedClickHousePlan("clickhouse_tokenbf_requires_token_predicate");
      plan.family = IndexFamily::bloom;
      plan.semantic_profile = "clickhouse_tokenbf_profile";
      plan.reason_code = "clickhouse_tokenbf_token_summary";
      return plan;
    case ClickHouseSkipVariant::ngrambf:
      if (!request.ngram_predicate) return BlockedClickHousePlan("clickhouse_ngrambf_requires_ngram_predicate");
      plan.family = IndexFamily::ngram;
      plan.semantic_profile = "clickhouse_ngrambf_profile";
      plan.reason_code = "clickhouse_ngrambf_ngram_summary";
      return plan;
    case ClickHouseSkipVariant::sparse_grams:
      if (!request.sparse_candidate) return BlockedClickHousePlan("clickhouse_sparse_grams_requires_sparse_candidate");
      plan.family = IndexFamily::ngram;
      plan.semantic_profile = "clickhouse_sparse_grams_profile";
      plan.reason_code = "clickhouse_sparse_grams_recheck";
      return plan;
    case ClickHouseSkipVariant::hypothesis:
      if (!request.hypothesis_accepted) return BlockedClickHousePlan("clickhouse_hypothesis_policy_refusal");
      plan.semantic_profile = "clickhouse_hypothesis_skip_profile";
      plan.disclosure = ApproximationDisclosureLevel::approximate_candidates;
      plan.reason_code = "clickhouse_hypothesis_candidate_recheck";
      return plan;
    case ClickHouseSkipVariant::unsupported:
      break;
  }

  return BlockedClickHousePlan("clickhouse_skip_variant_unsupported");
}

DuckDbIndexMappingPlan ResolveDuckDbIndexMapping(const DuckDbIndexMappingRequest& request) {
  DuckDbIndexMappingPlan plan;
  if (!request.exact_recheck_available) {
    plan.reason_code = "duckdb_mapping_requires_exact_recheck";
    return plan;
  }

  switch (request.kind) {
    case DuckDbMappingKind::zonemap_summary:
      plan.family = IndexFamily::brin_zone;
      plan.semantic_profile = "duckdb_zonemap_profile";
      plan.accepted = true;
      plan.requires_recheck = true;
      plan.reason_code = request.zonemap_stale ? "duckdb_zonemap_stale_recheck"
                                               : "duckdb_zonemap_summary";
      return plan;
    case DuckDbMappingKind::art_ordered_alias:
    case DuckDbMappingKind::art_equality_alias:
      plan.family = request.unique_constraint ? IndexFamily::unique_btree : IndexFamily::btree;
      plan.semantic_profile = "duckdb_art_profile";
      plan.accepted = true;
      plan.requires_recheck = true;
      plan.preserves_art_identity_metrics = true;
      plan.reason_code = request.ordered_lookup ? "duckdb_art_ordered_btree_alias"
                                                : "duckdb_art_equality_btree_alias";
      return plan;
    case DuckDbMappingKind::unsupported:
      break;
  }

  plan.reason_code = "duckdb_mapping_unsupported";
  return plan;
}

ColumnarZonePrunePlan PlanColumnarZonePrune(const ColumnarZoneSegmentState& segment,
                                            const RangePredicateProbe& predicate) {
  ColumnarZonePrunePlan plan;
  if (segment.segment_deleted) {
    plan.summary_decision.decision = RangePruneDecision::prune_all;
    plan.summary_decision.can_prune = true;
    plan.summary_decision.requires_lossy_recheck = false;
    plan.summary_decision.disclosure = ApproximationDisclosureLevel::exact;
    plan.summary_decision.reason_code = "columnar_zone_segment_deleted";
    plan.segment_pruned = true;
    plan.segment_scan_required = false;
    plan.metrics_count_pruned_segment = true;
    plan.reason_code = "columnar_zone_deleted_segment_pruned";
    return plan;
  }

  if (!segment.segment_metadata_present) {
    plan.summary_decision.decision = RangePruneDecision::maybe_with_recheck;
    plan.summary_decision.can_prune = false;
    plan.summary_decision.requires_lossy_recheck = true;
    plan.summary_decision.reason_code = "columnar_zone_metadata_missing";
    plan.reason_code = "columnar_zone_metadata_missing_scan";
    return plan;
  }

  plan.summary_decision = DecideRangeSummaryPrune(segment.summary, predicate);
  plan.segment_pruned = plan.summary_decision.can_prune;
  plan.segment_scan_required = !plan.segment_pruned;
  plan.metrics_count_pruned_segment = plan.segment_pruned;
  plan.reason_code = plan.segment_pruned ? "columnar_zone_segment_pruned"
                                         : (segment.compressed_zone
                                                ? "columnar_zone_compressed_segment_recheck"
                                                : "columnar_zone_segment_recheck");
  return plan;
}

AggregateSketchPlan PlanAggregateSketch(const AggregateSketchRequest& request) {
  if (request.kind == AggregateSketchKind::unsupported) {
    return BlockedSketchPlan("aggregate_sketch_kind_unsupported", false);
  }

  AggregateSketchPlan plan;
  plan.family = IndexFamily::columnar_zone;
  plan.merge_compatible = request.merge_supported;
  plan.finalize_compatible = request.finalize_supported;

  if (request.kind == AggregateSketchKind::exact_aggregate) {
    plan.accepted = request.finalize_supported;
    plan.discloses_approximation = false;
    plan.privacy_policy_refusal = false;
    plan.disclosure = ApproximationDisclosureLevel::exact;
    plan.reason_code = request.finalize_supported ? "aggregate_exact_summary"
                                                  : "aggregate_exact_finalize_required";
    return plan;
  }

  if (!request.approximate_allowed) {
    return BlockedSketchPlan("aggregate_sketch_approximation_policy_refusal", false);
  }
  if (!request.privacy_policy_allows_export) {
    return BlockedSketchPlan("aggregate_sketch_privacy_policy_refusal", true);
  }
  if (!request.merge_supported || !request.finalize_supported) {
    return BlockedSketchPlan("aggregate_sketch_merge_finalize_required", false);
  }

  plan.accepted = true;
  plan.discloses_approximation = true;
  plan.privacy_policy_refusal = false;
  plan.disclosure = request.exact_recheck_available ? ApproximationDisclosureLevel::approximate_candidates
                                                    : ApproximationDisclosureLevel::approximate_result;
  plan.reason_code = request.exact_recheck_available ? "aggregate_sketch_candidate_recheck"
                                                     : "aggregate_sketch_approximate_result_disclosed";
  return plan;
}

const char* HashCollisionDecisionName(HashCollisionDecision decision) {
  switch (decision) {
    case HashCollisionDecision::exact_match: return "exact_match";
    case HashCollisionDecision::collision_requires_key_recheck: return "collision_requires_key_recheck";
    case HashCollisionDecision::reject_mismatched_hash: return "reject_mismatched_hash";
    case HashCollisionDecision::reject_mismatched_key: return "reject_mismatched_key";
  }
  return "unknown";
}

const char* HashRehashDecisionName(HashRehashDecision decision) {
  switch (decision) {
    case HashRehashDecision::keep_directory: return "keep_directory";
    case HashRehashDecision::split_bucket: return "split_bucket";
    case HashRehashDecision::merge_bucket: return "merge_bucket";
    case HashRehashDecision::grow_directory: return "grow_directory";
    case HashRehashDecision::shrink_directory: return "shrink_directory";
    case HashRehashDecision::rebuild_required: return "rebuild_required";
  }
  return "unknown";
}

const char* HashDuplicateDecisionName(HashDuplicateDecision decision) {
  switch (decision) {
    case HashDuplicateDecision::allow_insert: return "allow_insert";
    case HashDuplicateDecision::reject_visible_duplicate: return "reject_visible_duplicate";
    case HashDuplicateDecision::wait_for_mga_resolution: return "wait_for_mga_resolution";
    case HashDuplicateDecision::allow_same_row_update: return "allow_same_row_update";
    case HashDuplicateDecision::policy_refusal: return "policy_refusal";
  }
  return "unknown";
}

const char* FilterRecheckDecisionName(FilterRecheckDecision decision) {
  switch (decision) {
    case FilterRecheckDecision::exact_accept: return "exact_accept";
    case FilterRecheckDecision::prune: return "prune";
    case FilterRecheckDecision::candidate_requires_exact_recheck: return "candidate_requires_exact_recheck";
    case FilterRecheckDecision::policy_blocked: return "policy_blocked";
  }
  return "unknown";
}

const char* ApproximationDisclosureLevelName(ApproximationDisclosureLevel disclosure) {
  switch (disclosure) {
    case ApproximationDisclosureLevel::exact: return "exact";
    case ApproximationDisclosureLevel::lossy_with_recheck: return "lossy_with_recheck";
    case ApproximationDisclosureLevel::approximate_candidates: return "approximate_candidates";
    case ApproximationDisclosureLevel::approximate_result: return "approximate_result";
  }
  return "unknown";
}

const char* RangePruneDecisionName(RangePruneDecision decision) {
  switch (decision) {
    case RangePruneDecision::prune_all: return "prune_all";
    case RangePruneDecision::maybe_with_recheck: return "maybe_with_recheck";
    case RangePruneDecision::accept_exact: return "accept_exact";
    case RangePruneDecision::stale_resummarize: return "stale_resummarize";
    case RangePruneDecision::policy_blocked: return "policy_blocked";
  }
  return "unknown";
}

}  // namespace scratchbird::core::index
