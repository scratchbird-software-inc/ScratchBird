// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-ORDERED-ACCESS-CLOSURE-ANCHOR
#include "index_access_method.hpp"
#include "covering_index_payload.hpp"
#include "index_family_registry.hpp"
#include "index_key_encoding.hpp"
#include "index_maintenance.hpp"
#include "index_metapage.hpp"
#include "index_posting.hpp"
#include "index_quarantine.hpp"
#include "index_recheck.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class OrderedAccessIntent : u32 {
  seek = 1,
  range = 2,
  prefix = 3,
  ordered_scan = 4
};

enum class OrderedScanDirection : u32 {
  forward = 1,
  reverse = 2
};

enum class OrderedBoundKind : u32 {
  unbounded = 1,
  inclusive = 2,
  exclusive = 3,
  prefix = 4
};

enum class OrderedAccessShape : u32 {
  seek = 1,
  range = 2,
  prefix = 3,
  reverse_seek = 4,
  reverse_range = 5,
  reverse_prefix = 6,
  composite_seek = 7,
  composite_range = 8,
  full_ordered_scan = 9
};

enum class OrderedAccessDecision : u32 {
  admitted_exact = 1,
  admitted_requires_recheck = 2,
  admitted_with_fallback_sort = 3,
  refused = 4
};

enum class OrderedUniquenessMode : u32 {
  non_unique = 1,
  unique_immediate = 2,
  unique_deferred = 3
};

enum class OrderedNullUniquenessPolicy : u32 {
  nulls_distinct = 1,
  nulls_not_distinct = 2,
  donor_profile_default = 3
};

enum class OrderedDuplicateLifecycleAction : u32 {
  create_posting_list = 1,
  append_duplicate = 2,
  append_provisional = 3,
  mark_dead = 4,
  purge_dead = 5,
  refuse_duplicate = 6
};

enum class OrderedCompressionDecision : u32 {
  none = 1,
  prefix = 2,
  suffix = 3,
  prefix_suffix = 4,
  abbreviated_key = 5,
  dictionary = 6
};

enum class OrderedBuildMode : u32 {
  incremental = 1,
  bulk_presorted = 2,
  bulk_external_sort = 3,
  rebuild_presorted = 4,
  rebuild_external_sort = 5
};

enum class OrderedOverlayKind : u32 {
  none = 1,
  expression = 2,
  partial = 3,
  covering = 4
};

enum class OrderedOverlayEligibility : u32 {
  eligible_exact = 1,
  eligible_requires_recheck = 2,
  refused = 3
};

enum class OrderedAliasDecisionKind : u32 {
  native_btree = 1,
  alias_to_btree_ordered = 2,
  alias_to_btree_equality_prefix = 3,
  refused = 4
};

enum class OrderedDonorEngine : u32 {
  scratchbird = 1,
  firebird = 2,
  postgresql = 3,
  mysql = 4,
  mariadb = 5,
  sqlite = 6,
  duckdb = 7,
  neo4j = 8,
  unknown = 9
};

struct OrderedKeyBound {
  OrderedBoundKind kind = OrderedBoundKind::unbounded;
  std::vector<IndexKeyEncodingComponent> components;
};

struct OrderedAccessRequest {
  IndexFamily family = IndexFamily::btree;
  OrderedAccessIntent intent = OrderedAccessIntent::seek;
  OrderedScanDirection direction = OrderedScanDirection::forward;
  IndexKeySemanticProfile semantic_profile;
  OrderedKeyBound lower_bound;
  OrderedKeyBound upper_bound;
  u32 equality_prefix_components = 0;
  u32 projected_order_components = 0;
  bool require_total_order_proof = true;
  bool allow_fallback_sort = false;
  bool donor_visible = false;
  bool overlay_predicate_required = false;
  bool require_page_authority = false;
  IndexPageAuthorityInput page_authority;
};

struct OrderedAccessPlan {
  Status status;
  bool admitted = false;
  OrderedAccessDecision decision = OrderedAccessDecision::refused;
  OrderedAccessShape shape = OrderedAccessShape::full_ordered_scan;
  IndexAccessMethodCapabilities capabilities;
  IndexRecheckPolicy recheck_policy;
  IndexQuarantineDecision authority_decision;
  IndexKeyEncodingResult lower_key;
  IndexKeyEncodingResult upper_key;
  bool order_proven = false;
  bool fallback_sort_required = false;
  bool prefix_exact = false;
  bool prefix_lower_bound_generated = false;
  bool prefix_upper_bound_generated = false;
  bool prefix_upper_bound_unbounded = false;
  std::vector<byte> prefix_matcher;
  bool composite_profile = false;
  std::vector<std::string> steps;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedUniquenessRequest {
  IndexFamily family = IndexFamily::unique_btree;
  OrderedUniquenessMode mode = OrderedUniquenessMode::unique_immediate;
  OrderedNullUniquenessPolicy null_policy = OrderedNullUniquenessPolicy::nulls_distinct;
  IndexKeySemanticProfile semantic_profile;
  std::vector<IndexKeyEncodingComponent> key_components;
  bool donor_profile_nulls_distinct = true;
  bool partial_predicate = false;
  bool predicate_proven = true;
};

struct OrderedUniquenessDecision {
  Status status;
  bool admitted = false;
  bool uniqueness_enforced = false;
  bool conflict_probe_required = false;
  bool commit_time_probe_required = false;
  bool null_exempt_from_conflict = false;
  IndexRecheckPolicy recheck_policy;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedDuplicateLifecycleRequest {
  IndexPostingList posting_list;
  IndexPostingEntry incoming;
  IndexFamily family = IndexFamily::unknown;
  IndexKeySemanticProfile semantic_profile;
  OrderedUniquenessMode uniqueness_mode = OrderedUniquenessMode::non_unique;
  OrderedNullUniquenessPolicy null_policy = OrderedNullUniquenessPolicy::nulls_distinct;
  bool incoming_key_has_null = false;
  bool insert = true;
  bool delete_existing = false;
  bool purge_dead = false;
  bool provisional = false;
  bool exact_index = false;
  bool equality_image_proof_present = true;
  bool stable_row_uuid_locators = true;
  bool preserve_mga_visibility_recheck = true;
  bool parser_or_donor_finality_authority = false;
  bool timestamp_or_uuid_order_finality_authority = false;
  u64 oldest_active_transaction_id = 0;
};

struct OrderedDuplicateLifecycleDecision {
  Status status;
  bool admitted = false;
  OrderedDuplicateLifecycleAction action = OrderedDuplicateLifecycleAction::refuse_duplicate;
  IndexPostingListResult posting_result;
  IndexOperationMetricsDelta metrics;
  IndexPostingCompressionCounters compression_counters;
  std::vector<IndexPostingEvidenceField> evidence;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedCompressionRequest {
  u32 page_size = 0;
  u32 key_count = 0;
  u64 uncompressed_key_bytes = 0;
  u64 prefix_savings_bytes = 0;
  u64 suffix_savings_bytes = 0;
  u64 abbreviated_key_savings_bytes = 0;
  u64 dictionary_savings_bytes = 0;
  u64 minimum_savings_bytes = 16;
  bool allow_prefix = true;
  bool allow_suffix = true;
  bool allow_abbreviated_key = true;
  bool allow_dictionary = true;
  bool semantic_bytewise_stable = true;
};

struct OrderedCompressionPlan {
  Status status;
  bool admitted = false;
  OrderedCompressionDecision decision = OrderedCompressionDecision::none;
  u64 estimated_saved_bytes = 0;
  bool order_preserving = true;
  bool requires_key_recheck = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedBuildRequest {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::btree;
  u64 tuple_count_estimate = 0;
  u64 page_budget = 0;
  u64 byte_budget = 0;
  u64 time_budget_microseconds = 0;
  bool rebuild = false;
  bool input_presorted = false;
  bool order_proof_valid = false;
  bool allow_external_sort = true;
  bool online = true;
  bool unique = false;
  bool policy_allows_mutation = false;
  bool read_only_database = false;
};

struct OrderedBuildPlan {
  Status status;
  bool admitted = false;
  OrderedBuildMode mode = OrderedBuildMode::incremental;
  bool requires_external_sort = false;
  bool validates_uniqueness = false;
  bool publishes_new_root = false;
  bool commit_atomic = false;
  IndexMaintenancePlan maintenance_plan;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedOverlayRequest {
  IndexFamily family = IndexFamily::btree;
  OrderedOverlayKind overlay = OrderedOverlayKind::none;
  bool expression_deterministic = false;
  bool expression_resource_epoch_valid = false;
  bool expression_result_lossy = false;
  bool predicate_immutable = false;
  bool predicate_security_safe = false;
  bool predicate_exact = false;
  bool covering_payload_requested = false;
  u32 covering_payload_columns = 0;
  bool payload_freshness_proven = false;
  bool security_projection_safe = false;
  bool can_recheck_base_row = true;
  const CoveringIndexPayloadAdmission* covering_payload_admission = nullptr;
};

struct OrderedOverlayDecision {
  Status status;
  bool admitted = false;
  OrderedOverlayEligibility eligibility = OrderedOverlayEligibility::refused;
  bool use_btree_physical = false;
  bool index_only_allowed = false;
  bool requires_recheck = false;
  IndexRecheckPolicy recheck_policy;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedAliasRequest {
  std::string requested_family;
  bool persistent_required = true;
  bool ordered_iteration_required = true;
  bool prefix_navigation_required = false;
  bool equality_only = false;
  bool donor_requires_native_catalog_identity = false;
  bool donor_requires_native_page_metrics = false;
};

struct OrderedAliasDecision {
  Status status;
  bool admitted = false;
  OrderedAliasDecisionKind decision = OrderedAliasDecisionKind::refused;
  IndexFamily native_family = IndexFamily::btree;
  std::string semantic_profile_id;
  bool independent_catalog_identity = false;
  bool independent_metrics_identity = false;
  bool independent_optimizer_identity = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

struct OrderedDonorSemanticProfile {
  OrderedDonorEngine donor = OrderedDonorEngine::unknown;
  std::string profile_id;
  std::string donor_name;
  IndexFamily native_family = IndexFamily::btree;
  IndexKeyNullPlacement ascending_null_placement = IndexKeyNullPlacement::nulls_last;
  IndexKeyNullPlacement descending_null_placement = IndexKeyNullPlacement::nulls_first;
  OrderedNullUniquenessPolicy unique_null_policy = OrderedNullUniquenessPolicy::nulls_distinct;
  IndexKeySemanticProfile key_profile;
  bool stores_null_keys = true;
  bool supports_descending_keys = true;
  bool supports_prefix_seek = true;
  bool supports_expression_indexes = false;
  bool supports_partial_indexes = false;
  bool supports_covering_indexes = false;
  bool requires_collation_epoch = true;
  bool catalog_projection_allowed = true;
  bool order_proof_requires_recheck = false;
  bool may_require_fallback_sort = false;
  std::vector<std::string> unsupported_modes;
};

struct OrderedDonorProfileRequest {
  OrderedDonorEngine donor = OrderedDonorEngine::scratchbird;
  std::string profile_id;
  OrderedAccessRequest access;
  OrderedUniquenessRequest uniqueness;
  OrderedOverlayRequest overlay;
  bool uniqueness_requested = false;
  bool overlay_requested = false;
  bool descending_key_requested = false;
  bool prefix_seek_requested = false;
  bool donor_catalog_projection_requested = false;
  bool collation_epoch_valid = true;
  bool allow_fallback_sort = false;
};

struct OrderedDonorProfileDecision {
  Status status;
  bool admitted = false;
  OrderedDonorSemanticProfile profile;
  OrderedAccessPlan access_plan;
  OrderedUniquenessDecision uniqueness_decision;
  OrderedOverlayDecision overlay_decision;
  bool fallback_sort_required = false;
  bool catalog_projection_allowed = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> steps;

  bool ok() const { return status.ok() && admitted; }
};

OrderedAccessPlan PlanOrderedBTreeAccess(const OrderedAccessRequest& request);
OrderedUniquenessDecision DecideOrderedUniquenessPolicy(const OrderedUniquenessRequest& request);
OrderedDuplicateLifecycleDecision DecideOrderedDuplicateLifecycle(const OrderedDuplicateLifecycleRequest& request);
OrderedCompressionPlan PlanOrderedCompression(const OrderedCompressionRequest& request);
OrderedBuildPlan PlanOrderedBulkBuild(const OrderedBuildRequest& request);
OrderedOverlayDecision DecideOrderedOverlayEligibility(const OrderedOverlayRequest& request);
OrderedAliasDecision DecideOrderedAlias(const OrderedAliasRequest& request);
const std::vector<OrderedDonorSemanticProfile>& BuiltinOrderedDonorSemanticProfiles();
const OrderedDonorSemanticProfile* FindOrderedDonorSemanticProfile(OrderedDonorEngine donor,
                                                                   std::string_view profile_id = {});
OrderedDonorProfileDecision ApplyOrderedDonorSemanticProfile(const OrderedDonorProfileRequest& request);

const char* OrderedAccessShapeName(OrderedAccessShape shape);
const char* OrderedAccessDecisionName(OrderedAccessDecision decision);
const char* OrderedDuplicateLifecycleActionName(OrderedDuplicateLifecycleAction action);
const char* OrderedDonorEngineName(OrderedDonorEngine donor);
DiagnosticRecord MakeOrderedAccessDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

}  // namespace scratchbird::core::index
