// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_ordered_access.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

constexpr u32 Flag(IndexPostingFlag flag) { return static_cast<u32>(flag); }

bool HasFlag(u32 flags, IndexPostingFlag flag) {
  return (flags & Flag(flag)) != 0;
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool SameLocator(const IndexRowLocator& left, const IndexRowLocator& right) {
  if (!SameTypedUuid(left.table_uuid, right.table_uuid) || !SameTypedUuid(left.row_uuid, right.row_uuid)) {
    return false;
  }
  if (left.version_uuid.valid() && right.version_uuid.valid()) {
    return SameTypedUuid(left.version_uuid, right.version_uuid);
  }
  return true;
}

bool IsOrderedBTreeFamily(IndexFamily family) {
  switch (family) {
    case IndexFamily::btree:
    case IndexFamily::unique_btree:
    case IndexFamily::expression:
    case IndexFamily::partial:
    case IndexFamily::covering:
      return true;
    default:
      return false;
  }
}

bool IsAcceptedOrderedFamily(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  return IsOrderedBTreeFamily(family) && descriptor != nullptr &&
         descriptor->completion == IndexCompletionStatus::accepted_requires_full_implementation;
}

bool HasBound(const OrderedKeyBound& bound) {
  return bound.kind != OrderedBoundKind::unbounded;
}

bool HasNullComponent(const std::vector<IndexKeyEncodingComponent>& components) {
  for (const auto& component : components) {
    if (component.is_null) {
      return true;
    }
  }
  return false;
}

bool BoundComponentsValid(const OrderedKeyBound& bound) {
  return bound.kind == OrderedBoundKind::unbounded || !bound.components.empty();
}

OrderedAccessShape DetermineAccessShape(const OrderedAccessRequest& request,
                                        bool lower_present,
                                        bool upper_present,
                                        bool equal_bounds) {
  const bool reverse = request.direction == OrderedScanDirection::reverse;
  const bool prefix = request.intent == OrderedAccessIntent::prefix ||
                      request.lower_bound.kind == OrderedBoundKind::prefix ||
                      request.upper_bound.kind == OrderedBoundKind::prefix;
  const bool composite = request.equality_prefix_components > 0 &&
                         request.projected_order_components > request.equality_prefix_components;
  if (prefix) {
    return reverse ? OrderedAccessShape::reverse_prefix : OrderedAccessShape::prefix;
  }
  if (request.intent == OrderedAccessIntent::ordered_scan && !lower_present && !upper_present) {
    return OrderedAccessShape::full_ordered_scan;
  }
  if (composite) {
    return equal_bounds ? OrderedAccessShape::composite_seek : OrderedAccessShape::composite_range;
  }
  if (request.intent == OrderedAccessIntent::seek || (lower_present && upper_present && equal_bounds)) {
    return reverse ? OrderedAccessShape::reverse_seek : OrderedAccessShape::seek;
  }
  return reverse ? OrderedAccessShape::reverse_range : OrderedAccessShape::range;
}

bool BoundRequiresRecheck(const IndexKeyEncodingResult& result) {
  return result.status.ok() && (result.lossy || result.requires_recheck);
}

OrderedNullUniquenessPolicy EffectiveNullPolicy(OrderedNullUniquenessPolicy requested,
                                                bool donor_nulls_distinct) {
  if (requested != OrderedNullUniquenessPolicy::donor_profile_default) {
    return requested;
  }
  return donor_nulls_distinct ? OrderedNullUniquenessPolicy::nulls_distinct
                              : OrderedNullUniquenessPolicy::nulls_not_distinct;
}

bool DuplicateConflictPossible(OrderedUniquenessMode mode,
                               OrderedNullUniquenessPolicy null_policy,
                               bool incoming_key_has_null) {
  if (mode == OrderedUniquenessMode::non_unique) {
    return false;
  }
  return !(incoming_key_has_null && null_policy == OrderedNullUniquenessPolicy::nulls_distinct);
}

bool PostingIsLive(const IndexPostingEntry& entry, u64 oldest_active_transaction_id) {
  if (HasFlag(entry.flags, IndexPostingFlag::deleted)) {
    return false;
  }
  return entry.visible_until_transaction_id == 0 ||
         oldest_active_transaction_id == 0 ||
         entry.visible_until_transaction_id >= oldest_active_transaction_id;
}

bool ExactNonUniquePostingCompressionRequested(const OrderedDuplicateLifecycleRequest& request) {
  return request.exact_index &&
         request.uniqueness_mode == OrderedUniquenessMode::non_unique &&
         request.family == IndexFamily::btree;
}

IndexPostingEqualityProof EqualityProofFor(const OrderedDuplicateLifecycleRequest& request) {
  IndexPostingEqualityProof proof;
  proof.proof_present = request.equality_image_proof_present;
  proof.non_unique_exact = ExactNonUniquePostingCompressionRequested(request);
  proof.encoded_key_bytewise_stable = request.semantic_profile.bytewise_stable;
  proof.stable_row_uuid_locators = request.stable_row_uuid_locators;
  proof.preserves_mga_visibility_recheck =
      request.preserve_mga_visibility_recheck;
  proof.parser_or_donor_finality_authority =
      request.parser_or_donor_finality_authority;
  proof.timestamp_or_uuid_order_finality_authority =
      request.timestamp_or_uuid_order_finality_authority;
  return proof;
}

void AddDuplicateEvidence(OrderedDuplicateLifecycleDecision* decision,
                          std::string name,
                          std::string value) {
  decision->evidence.push_back({std::move(name), std::move(value)});
}

void CopyPostingEvidence(OrderedDuplicateLifecycleDecision* decision) {
  decision->compression_counters = decision->posting_result.counters;
  decision->evidence.insert(decision->evidence.end(),
                            decision->posting_result.evidence.begin(),
                            decision->posting_result.evidence.end());
  AddDuplicateEvidence(decision,
                       "selected_lifecycle_action",
                       OrderedDuplicateLifecycleActionName(decision->action));
}

void PreparePostingListForLifecycle(IndexPostingList* posting_list,
                                    const OrderedDuplicateLifecycleRequest& request) {
  posting_list->compressed_duplicates =
      ExactNonUniquePostingCompressionRequested(request);
  posting_list->recheck_required = true;
  posting_list->equality_proof = EqualityProofFor(request);
  if (posting_list->compressed_duplicates) {
    for (auto& entry : posting_list->entries) {
      entry.flags |= Flag(IndexPostingFlag::requires_recheck);
    }
  }
}

std::string Normalize(std::string value) {
  for (char& ch : value) {
    if (ch == '-' || ch == ' ') {
      ch = '_';
    } else {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }
  return value;
}

IndexKeySemanticProfile KeyProfile(std::string profile_id,
                                   bool donor_visible_tiebreak,
                                   bool bytewise_stable,
                                   bool requires_recheck) {
  IndexKeySemanticProfile profile;
  profile.profile_id = std::move(profile_id);
  profile.donor_visible_tiebreak = donor_visible_tiebreak;
  profile.bytewise_stable = bytewise_stable;
  profile.requires_recheck = requires_recheck;
  return profile;
}

OrderedDonorSemanticProfile DonorProfile(OrderedDonorEngine donor,
                                         const char* profile_id,
                                         const char* donor_name,
                                         IndexKeyNullPlacement asc_nulls,
                                         IndexKeyNullPlacement desc_nulls,
                                         OrderedNullUniquenessPolicy unique_null_policy,
                                         bool stores_nulls,
                                         bool expression,
                                         bool partial,
                                         bool covering,
                                         bool requires_recheck,
                                         bool fallback_sort,
                                         std::vector<std::string> unsupported) {
  OrderedDonorSemanticProfile profile;
  profile.donor = donor;
  profile.profile_id = profile_id;
  profile.donor_name = donor_name;
  profile.native_family = IndexFamily::btree;
  profile.ascending_null_placement = asc_nulls;
  profile.descending_null_placement = desc_nulls;
  profile.unique_null_policy = unique_null_policy;
  profile.key_profile = KeyProfile(profile_id, donor != OrderedDonorEngine::scratchbird, !fallback_sort, requires_recheck);
  profile.stores_null_keys = stores_nulls;
  profile.supports_descending_keys = true;
  profile.supports_prefix_seek = true;
  profile.supports_expression_indexes = expression;
  profile.supports_partial_indexes = partial;
  profile.supports_covering_indexes = covering;
  profile.requires_collation_epoch = true;
  profile.catalog_projection_allowed = true;
  profile.order_proof_requires_recheck = requires_recheck;
  profile.may_require_fallback_sort = fallback_sort;
  profile.unsupported_modes = std::move(unsupported);
  return profile;
}

OrderedAccessPlan RefuseAccess(std::string code, std::string key, std::string detail) {
  OrderedAccessPlan plan;
  plan.status = ErrorStatus();
  plan.admitted = false;
  plan.decision = OrderedAccessDecision::refused;
  plan.diagnostic = MakeOrderedAccessDiagnostic(plan.status, std::move(code), std::move(key), std::move(detail));
  return plan;
}

OrderedUniquenessDecision RefuseUniqueness(std::string code, std::string key, std::string detail) {
  OrderedUniquenessDecision decision;
  decision.status = ErrorStatus();
  decision.admitted = false;
  decision.diagnostic = MakeOrderedAccessDiagnostic(decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}

OrderedDuplicateLifecycleDecision RefuseDuplicate(std::string code, std::string key, std::string detail) {
  OrderedDuplicateLifecycleDecision decision;
  decision.status = ErrorStatus();
  decision.admitted = false;
  decision.action = OrderedDuplicateLifecycleAction::refuse_duplicate;
  decision.diagnostic = MakeOrderedAccessDiagnostic(decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}

OrderedOverlayDecision RefuseOverlay(std::string code, std::string key, std::string detail) {
  OrderedOverlayDecision decision;
  decision.status = ErrorStatus();
  decision.admitted = false;
  decision.eligibility = OrderedOverlayEligibility::refused;
  decision.diagnostic = MakeOrderedAccessDiagnostic(decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}

OrderedAliasDecision RefuseAlias(std::string code, std::string key, std::string detail) {
  OrderedAliasDecision decision;
  decision.status = ErrorStatus();
  decision.admitted = false;
  decision.decision = OrderedAliasDecisionKind::refused;
  decision.diagnostic = MakeOrderedAccessDiagnostic(decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}

OrderedDonorProfileDecision RefuseDonor(std::string code, std::string key, std::string detail) {
  OrderedDonorProfileDecision decision;
  decision.status = ErrorStatus();
  decision.admitted = false;
  decision.diagnostic = MakeOrderedAccessDiagnostic(decision.status, std::move(code), std::move(key), std::move(detail));
  return decision;
}
}  // namespace

OrderedAccessPlan PlanOrderedBTreeAccess(const OrderedAccessRequest& request) {
  if (!IsAcceptedOrderedFamily(request.family)) {
    return RefuseAccess("SB-INDEX-ORDERED-FAMILY-NOT-ADMITTED",
                        "index.ordered.family_not_admitted",
                        IndexFamilyName(request.family));
  }
  if (!BoundComponentsValid(request.lower_bound) || !BoundComponentsValid(request.upper_bound)) {
    return RefuseAccess("SB-INDEX-ORDERED-BOUND-EMPTY",
                        "index.ordered.bound_empty",
                        IndexFamilyName(request.family));
  }

  OrderedAccessPlan plan;
  plan.status = OkStatus();
  plan.admitted = true;
  plan.capabilities = CapabilitiesForFamily(request.family);
  plan.recheck_policy.require_mga_visibility = true;
  plan.recheck_policy.require_predicate_match = request.overlay_predicate_required;
  plan.recheck_policy.require_security_visibility = true;
  plan.recheck_policy.accept_lossy_without_exact_predicate = false;
  plan.steps.push_back("validate_ordered_family_descriptor");

  if (request.require_page_authority) {
    plan.authority_decision = ClassifyIndexPageAuthority(request.page_authority);
    plan.steps.push_back("classify_page_authority");
    if (!plan.authority_decision.ok()) {
      plan.status = plan.authority_decision.status;
      plan.admitted = false;
      plan.decision = OrderedAccessDecision::refused;
      plan.diagnostic = plan.authority_decision.diagnostic;
      return plan;
    }
  }

  const bool lower_present = HasBound(request.lower_bound);
  const bool upper_present = HasBound(request.upper_bound);
  if (lower_present) {
    if (request.lower_bound.kind == OrderedBoundKind::prefix) {
      const auto prefix =
          BuildEncodedPrefixMatcher(request.lower_bound.components,
                                    request.semantic_profile);
      plan.lower_key.status = prefix.status;
      plan.lower_key.encoded = prefix.matcher_prefix;
      plan.lower_key.lossy = prefix.lossy;
      plan.lower_key.requires_recheck = prefix.requires_recheck;
      plan.lower_key.evidence = prefix.evidence;
      plan.lower_key.diagnostic = prefix.diagnostic;
      plan.prefix_lower_bound_generated = true;
      plan.prefix_matcher = prefix.matcher_prefix;
      plan.steps.push_back("encode_prefix_matcher_bound");
    } else {
      plan.lower_key = EncodeIndexKey(request.lower_bound.components, request.semantic_profile);
      plan.steps.push_back("encode_lower_bound");
    }
    if (!plan.lower_key.ok()) {
      plan.status = plan.lower_key.status;
      plan.admitted = false;
      plan.decision = OrderedAccessDecision::refused;
      plan.diagnostic = plan.lower_key.diagnostic;
      return plan;
    }
  }
  if (upper_present) {
    if (request.upper_bound.kind == OrderedBoundKind::prefix) {
      const auto prefix =
          BuildEncodedPrefixUpperBound(request.upper_bound.components,
                                       request.semantic_profile);
      plan.upper_key.status = prefix.status;
      plan.upper_key.encoded = prefix.upper_bound;
      plan.upper_key.lossy = prefix.lossy;
      plan.upper_key.requires_recheck = prefix.requires_recheck;
      plan.upper_key.evidence = prefix.evidence;
      plan.upper_key.diagnostic = prefix.diagnostic;
      plan.prefix_upper_bound_generated = !prefix.upper_bound_unbounded;
      plan.prefix_upper_bound_unbounded = prefix.upper_bound_unbounded;
      if (plan.prefix_matcher.empty() && !prefix.matcher_prefix.empty()) {
        plan.prefix_matcher = prefix.matcher_prefix;
      }
      plan.steps.push_back(prefix.upper_bound_unbounded
                               ? "encode_prefix_upper_bound_unbounded"
                               : "encode_prefix_upper_bound");
    } else {
      plan.upper_key = EncodeIndexKey(request.upper_bound.components, request.semantic_profile);
      plan.steps.push_back("encode_upper_bound");
    }
    if (!plan.upper_key.ok()) {
      plan.status = plan.upper_key.status;
      plan.admitted = false;
      plan.decision = OrderedAccessDecision::refused;
      plan.diagnostic = plan.upper_key.diagnostic;
      return plan;
    }
  }

  bool equal_bounds = false;
  if (lower_present && upper_present &&
      request.lower_bound.kind != OrderedBoundKind::prefix &&
      request.upper_bound.kind != OrderedBoundKind::prefix) {
    const auto compare = CompareEncodedIndexKeys(plan.lower_key.encoded, plan.upper_key.encoded);
    plan.steps.push_back("compare_encoded_bounds");
    if (!compare.ok()) {
      plan.status = compare.status;
      plan.admitted = false;
      plan.decision = OrderedAccessDecision::refused;
      plan.diagnostic = compare.diagnostic;
      return plan;
    }
    if (compare.comparison > 0) {
      return RefuseAccess("SB-INDEX-ORDERED-BOUND-INVERSION",
                          "index.ordered.bound_inversion",
                          IndexFamilyName(request.family));
    }
    equal_bounds = compare.comparison == 0 &&
                   request.lower_bound.kind == OrderedBoundKind::inclusive &&
                   request.upper_bound.kind == OrderedBoundKind::inclusive;
  }

  plan.shape = DetermineAccessShape(request, lower_present, upper_present, equal_bounds);
  plan.prefix_exact = plan.shape == OrderedAccessShape::prefix || plan.shape == OrderedAccessShape::reverse_prefix;
  plan.composite_profile = plan.shape == OrderedAccessShape::composite_seek ||
                           plan.shape == OrderedAccessShape::composite_range;

  const bool order_stable = request.semantic_profile.bytewise_stable && plan.capabilities.can_satisfy_order;
  if (request.require_total_order_proof && !order_stable) {
    if (!request.allow_fallback_sort) {
      return RefuseAccess("SB-INDEX-ORDERED-ORDER-PROOF-REFUSED",
                          "index.ordered.order_proof_refused",
                          request.semantic_profile.profile_id);
    }
    plan.fallback_sort_required = true;
    plan.order_proven = false;
    plan.decision = OrderedAccessDecision::admitted_with_fallback_sort;
    plan.steps.push_back("attach_fallback_sort");
  } else {
    plan.order_proven = order_stable;
  }

  const bool requires_recheck = request.semantic_profile.requires_recheck ||
                                BoundRequiresRecheck(plan.lower_key) ||
                                BoundRequiresRecheck(plan.upper_key) ||
                                request.overlay_predicate_required;
  if (requires_recheck && plan.decision != OrderedAccessDecision::admitted_with_fallback_sort) {
    plan.decision = OrderedAccessDecision::admitted_requires_recheck;
  } else if (plan.decision != OrderedAccessDecision::admitted_with_fallback_sort) {
    plan.decision = OrderedAccessDecision::admitted_exact;
  }
  plan.recheck_policy.require_predicate_match = plan.recheck_policy.require_predicate_match || requires_recheck;
  plan.steps.push_back("publish_ordered_access_plan");
  return plan;
}

OrderedUniquenessDecision DecideOrderedUniquenessPolicy(const OrderedUniquenessRequest& request) {
  if (!IsAcceptedOrderedFamily(request.family)) {
    return RefuseUniqueness("SB-INDEX-ORDERED-UNIQUE-FAMILY-NOT-ADMITTED",
                            "index.ordered.unique_family_not_admitted",
                            IndexFamilyName(request.family));
  }
  const auto caps = CapabilitiesForFamily(request.family);
  if (request.mode != OrderedUniquenessMode::non_unique && !caps.can_be_unique) {
    return RefuseUniqueness("SB-INDEX-ORDERED-UNIQUE-NOT-SUPPORTED",
                            "index.ordered.unique_not_supported",
                            IndexFamilyName(request.family));
  }

  OrderedUniquenessDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.recheck_policy.require_mga_visibility = true;
  decision.recheck_policy.require_security_visibility = true;
  decision.recheck_policy.require_predicate_match = request.partial_predicate;
  decision.recheck_policy.accept_lossy_without_exact_predicate = false;
  decision.steps.push_back("classify_unique_profile");

  if (request.mode == OrderedUniquenessMode::non_unique) {
    decision.uniqueness_enforced = false;
    decision.steps.push_back("admit_non_unique_ordered_key");
    return decision;
  }

  const auto effective_null_policy = EffectiveNullPolicy(request.null_policy, request.donor_profile_nulls_distinct);
  decision.null_exempt_from_conflict = HasNullComponent(request.key_components) &&
                                       effective_null_policy == OrderedNullUniquenessPolicy::nulls_distinct;
  decision.uniqueness_enforced = !decision.null_exempt_from_conflict;
  decision.conflict_probe_required = decision.uniqueness_enforced;
  decision.commit_time_probe_required = request.mode == OrderedUniquenessMode::unique_deferred;
  decision.recheck_policy.require_predicate_match = decision.recheck_policy.require_predicate_match ||
                                                   request.semantic_profile.requires_recheck ||
                                                   !request.predicate_proven;
  decision.steps.push_back(decision.null_exempt_from_conflict ? "null_key_exempt_from_conflict"
                                                              : "probe_conflicting_visible_versions");
  if (decision.commit_time_probe_required) {
    decision.steps.push_back("defer_unique_probe_until_commit");
  }
  return decision;
}

OrderedDuplicateLifecycleDecision DecideOrderedDuplicateLifecycle(const OrderedDuplicateLifecycleRequest& request) {
  if (!request.posting_list.index_uuid.valid() || request.posting_list.encoded_key.empty()) {
    return RefuseDuplicate("SB-INDEX-ORDERED-DUPLICATE-POSTING-INVALID",
                           "index.ordered.duplicate_posting_invalid",
                           {});
  }
  if (request.exact_index && request.family != IndexFamily::btree &&
      request.family != IndexFamily::unique_btree) {
    return RefuseDuplicate("SB-INDEX-ORDERED-DUPLICATE-NON-EXACT-FAMILY",
                           "index.ordered.duplicate_non_exact_family",
                           IndexFamilyName(request.family));
  }
  if ((request.insert || request.delete_existing) &&
      (!request.incoming.locator.table_uuid.valid() || !request.incoming.locator.row_uuid.valid())) {
    return RefuseDuplicate("SB-INDEX-ORDERED-DUPLICATE-LOCATOR-INVALID",
                           "index.ordered.duplicate_locator_invalid",
                           {});
  }

  OrderedDuplicateLifecycleDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.posting_result.posting_list = request.posting_list;
  PreparePostingListForLifecycle(&decision.posting_result.posting_list, request);
  decision.metrics.candidates = static_cast<u64>(request.posting_list.entries.size());
  decision.steps.push_back("load_posting_list");

  if (request.purge_dead) {
    auto& entries = decision.posting_result.posting_list.entries;
    const auto before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const IndexPostingEntry& entry) {
                    return HasFlag(entry.flags, IndexPostingFlag::deleted) &&
                           (request.oldest_active_transaction_id == 0 ||
                            (entry.visible_until_transaction_id != 0 &&
                             entry.visible_until_transaction_id < request.oldest_active_transaction_id));
                  }),
                  entries.end());
    decision.action = OrderedDuplicateLifecycleAction::purge_dead;
    decision.metrics.rechecks = static_cast<u64>(before - entries.size());
    decision.steps.push_back("purge_dead_postings_below_horizon");
    decision.posting_result = BuildIndexPostingList(decision.posting_result.posting_list);
    decision.status = decision.posting_result.status;
    decision.admitted = decision.posting_result.ok();
    decision.diagnostic = decision.posting_result.diagnostic;
    CopyPostingEvidence(&decision);
    return decision;
  }

  if (request.delete_existing) {
    bool found = false;
    for (auto& entry : decision.posting_result.posting_list.entries) {
      if (SameLocator(entry.locator, request.incoming.locator)) {
        entry.flags |= Flag(IndexPostingFlag::deleted);
        entry.visible_until_transaction_id = request.incoming.visible_until_transaction_id;
        found = true;
        break;
      }
    }
    if (!found) {
      return RefuseDuplicate("SB-INDEX-ORDERED-DUPLICATE-DELETE-MISSING",
                             "index.ordered.duplicate_delete_missing",
                             {});
    }
    decision.action = OrderedDuplicateLifecycleAction::mark_dead;
    decision.metrics.pages_written = 1;
    decision.steps.push_back("mark_posting_dead");
    decision.posting_result = BuildIndexPostingList(decision.posting_result.posting_list);
    decision.status = decision.posting_result.status;
    decision.admitted = decision.posting_result.ok();
    decision.diagnostic = decision.posting_result.diagnostic;
    CopyPostingEvidence(&decision);
    return decision;
  }

  const auto effective_null_policy = EffectiveNullPolicy(request.null_policy, true);
  if (DuplicateConflictPossible(request.uniqueness_mode, effective_null_policy, request.incoming_key_has_null)) {
    for (const auto& entry : request.posting_list.entries) {
      if (PostingIsLive(entry, request.oldest_active_transaction_id) &&
          !SameLocator(entry.locator, request.incoming.locator)) {
        return RefuseDuplicate("SB-INDEX-ORDERED-DUPLICATE-UNIQUE-CONFLICT",
                               "index.ordered.duplicate_unique_conflict",
                               {});
      }
    }
  }

  auto incoming = request.incoming;
  const bool compression_requested =
      ExactNonUniquePostingCompressionRequested(request);
  if (compression_requested) {
    const auto proof = EqualityProofFor(request);
    if (!IndexPostingEqualityProofAccepted(proof)) {
      decision.status = ErrorStatus();
      decision.admitted = false;
      decision.action = OrderedDuplicateLifecycleAction::refuse_duplicate;
      decision.diagnostic = MakeOrderedAccessDiagnostic(
          decision.status,
          "SB-INDEX-ORDERED-DUPLICATE-EQUALITY-PROOF-REFUSED",
          "index.ordered.duplicate_equality_proof_refused");
      decision.posting_result = BuildIndexPostingList(decision.posting_result.posting_list);
      decision.compression_counters = decision.posting_result.counters;
      decision.evidence = decision.posting_result.evidence;
      AddDuplicateEvidence(&decision,
                           "selected_lifecycle_action",
                           OrderedDuplicateLifecycleActionName(decision.action));
      decision.steps.push_back("refuse_posting_compression_without_equality_proof");
      return decision;
    }
    incoming.flags |= Flag(IndexPostingFlag::requires_recheck);
    decision.steps.push_back("accept_non_unique_exact_equality_image_proof");
  }
  if (request.provisional || request.uniqueness_mode == OrderedUniquenessMode::unique_deferred) {
    incoming.flags |= Flag(IndexPostingFlag::provisional);
    decision.action = OrderedDuplicateLifecycleAction::append_provisional;
  } else if (decision.posting_result.posting_list.entries.empty()) {
    decision.action = OrderedDuplicateLifecycleAction::create_posting_list;
  } else {
    decision.action = OrderedDuplicateLifecycleAction::append_duplicate;
  }
  decision.posting_result.posting_list.entries.push_back(std::move(incoming));
  decision.metrics.pages_written = 1;
  decision.steps.push_back("append_ordered_posting");
  decision.posting_result = BuildIndexPostingList(decision.posting_result.posting_list);
  decision.status = decision.posting_result.status;
  decision.admitted = decision.posting_result.ok();
  decision.diagnostic = decision.posting_result.diagnostic;
  CopyPostingEvidence(&decision);
  return decision;
}

OrderedCompressionPlan PlanOrderedCompression(const OrderedCompressionRequest& request) {
  OrderedCompressionPlan plan;
  if (request.page_size == 0 || request.key_count == 0) {
    plan.status = ErrorStatus();
    plan.diagnostic = MakeOrderedAccessDiagnostic(plan.status,
                                                  "SB-INDEX-ORDERED-COMPRESSION-INVALID-INPUT",
                                                  "index.ordered.compression_invalid_input");
    return plan;
  }
  plan.status = OkStatus();
  plan.admitted = true;
  plan.steps.push_back("classify_key_compression_candidates");

  struct Candidate {
    OrderedCompressionDecision decision;
    u64 savings;
    bool requires_recheck;
    bool order_preserving;
    u32 priority;
  };
  std::vector<Candidate> candidates;
  candidates.push_back({OrderedCompressionDecision::none, 0, false, true, 100});
  if (request.allow_prefix) {
    candidates.push_back({OrderedCompressionDecision::prefix, request.prefix_savings_bytes, false, true, 40});
  }
  if (request.allow_suffix) {
    candidates.push_back({OrderedCompressionDecision::suffix, request.suffix_savings_bytes, false, true, 50});
  }
  if (request.allow_prefix && request.allow_suffix) {
    const u64 combined = std::min(request.uncompressed_key_bytes,
                                  request.prefix_savings_bytes + request.suffix_savings_bytes);
    candidates.push_back({OrderedCompressionDecision::prefix_suffix, combined, false, true, 30});
  }
  if (request.allow_abbreviated_key) {
    candidates.push_back({OrderedCompressionDecision::abbreviated_key,
                          request.abbreviated_key_savings_bytes,
                          true,
                          request.semantic_bytewise_stable,
                          70});
  }
  if (request.allow_dictionary) {
    candidates.push_back({OrderedCompressionDecision::dictionary,
                          request.dictionary_savings_bytes,
                          false,
                          true,
                          60});
  }

  Candidate best = candidates.front();
  for (const auto& candidate : candidates) {
    const bool saves_enough = candidate.savings >= request.minimum_savings_bytes;
    const bool best_saves_enough = best.savings >= request.minimum_savings_bytes;
    if ((saves_enough && (!best_saves_enough || candidate.savings > best.savings)) ||
        (saves_enough == best_saves_enough && candidate.savings == best.savings && candidate.priority < best.priority)) {
      best = candidate;
    }
  }
  if (best.savings < request.minimum_savings_bytes) {
    best = candidates.front();
  }

  plan.decision = best.decision;
  plan.estimated_saved_bytes = best.savings;
  plan.requires_key_recheck = best.requires_recheck;
  plan.order_preserving = best.order_preserving;
  plan.steps.push_back(plan.decision == OrderedCompressionDecision::none ? "store_uncompressed_keys"
                                                                         : "apply_order_preserving_key_compression");
  return plan;
}

OrderedBuildPlan PlanOrderedBulkBuild(const OrderedBuildRequest& request) {
  if (!request.index_uuid.valid() || !IsAcceptedOrderedFamily(request.family)) {
    OrderedBuildPlan plan;
    plan.status = ErrorStatus();
    plan.diagnostic = MakeOrderedAccessDiagnostic(plan.status,
                                                  "SB-INDEX-ORDERED-BUILD-INVALID-REQUEST",
                                                  "index.ordered.build_invalid_request",
                                                  IndexFamilyName(request.family));
    return plan;
  }

  OrderedBuildPlan plan;
  plan.status = OkStatus();
  plan.admitted = true;
  plan.validates_uniqueness = request.unique || request.family == IndexFamily::unique_btree;
  plan.publishes_new_root = request.rebuild || request.tuple_count_estimate > 1;
  plan.commit_atomic = true;
  plan.steps.push_back("capture_build_snapshot");

  const bool sorted_input = request.input_presorted && request.order_proof_valid;
  if (request.tuple_count_estimate <= 1 && !request.rebuild) {
    plan.mode = OrderedBuildMode::incremental;
    plan.steps.push_back("route_singleton_build_to_incremental_insert");
  } else if (sorted_input) {
    plan.mode = request.rebuild ? OrderedBuildMode::rebuild_presorted : OrderedBuildMode::bulk_presorted;
    plan.steps.push_back("consume_sorted_key_stream");
  } else if (request.allow_external_sort) {
    plan.mode = request.rebuild ? OrderedBuildMode::rebuild_external_sort : OrderedBuildMode::bulk_external_sort;
    plan.requires_external_sort = true;
    plan.steps.push_back("sort_key_stream_before_leaf_pack");
  } else {
    plan.status = ErrorStatus();
    plan.admitted = false;
    plan.diagnostic = MakeOrderedAccessDiagnostic(plan.status,
                                                  "SB-INDEX-ORDERED-BUILD-SORT-REFUSED",
                                                  "index.ordered.build_sort_refused",
                                                  IndexFamilyName(request.family));
    return plan;
  }

  if (plan.validates_uniqueness) {
    plan.steps.push_back("validate_unique_key_runs");
  }
  plan.steps.push_back("pack_leaf_pages_with_fence_keys");
  plan.steps.push_back("build_branch_levels_and_publish_root");

  IndexMaintenanceRequest maintenance;
  maintenance.index_uuid = request.index_uuid;
  maintenance.family = request.family;
  maintenance.operation = request.rebuild ? IndexMaintenanceOperation::rebuild : IndexMaintenanceOperation::verify;
  maintenance.page_budget = request.page_budget;
  maintenance.byte_budget = request.byte_budget;
  maintenance.time_budget_microseconds = request.time_budget_microseconds;
  maintenance.online = request.online;
  maintenance.read_only_database = request.read_only_database;
  maintenance.policy_allows_mutation = request.policy_allows_mutation;
  plan.maintenance_plan = PlanIndexMaintenance(maintenance);
  if (!plan.maintenance_plan.ok()) {
    plan.status = plan.maintenance_plan.status;
    plan.admitted = false;
    plan.diagnostic = plan.maintenance_plan.diagnostic;
    return plan;
  }
  plan.steps.push_back("attach_maintenance_budget_and_publication_guard");
  return plan;
}

OrderedOverlayDecision DecideOrderedOverlayEligibility(const OrderedOverlayRequest& request) {
  if (!IsAcceptedOrderedFamily(request.family)) {
    return RefuseOverlay("SB-INDEX-ORDERED-OVERLAY-FAMILY-NOT-ADMITTED",
                         "index.ordered.overlay_family_not_admitted",
                         IndexFamilyName(request.family));
  }

  OrderedOverlayDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.eligibility = OrderedOverlayEligibility::eligible_exact;
  decision.use_btree_physical = true;
  decision.recheck_policy.require_mga_visibility = true;
  decision.recheck_policy.require_security_visibility = true;
  decision.recheck_policy.require_predicate_match = false;
  decision.recheck_policy.accept_lossy_without_exact_predicate = false;
  decision.steps.push_back("classify_ordered_overlay");

  switch (request.overlay) {
    case OrderedOverlayKind::none:
      decision.steps.push_back("use_plain_btree_profile");
      return decision;
    case OrderedOverlayKind::expression:
      if (!request.expression_deterministic || !request.expression_resource_epoch_valid) {
        return RefuseOverlay("SB-INDEX-ORDERED-EXPRESSION-REFUSED",
                             "index.ordered.expression_refused",
                             IndexFamilyName(request.family));
      }
      decision.requires_recheck = request.expression_result_lossy;
      decision.recheck_policy.require_predicate_match = request.expression_result_lossy;
      decision.eligibility = request.expression_result_lossy ? OrderedOverlayEligibility::eligible_requires_recheck
                                                             : OrderedOverlayEligibility::eligible_exact;
      decision.steps.push_back("bind_expression_key_resource_epoch");
      return decision;
    case OrderedOverlayKind::partial:
      if (!request.predicate_immutable || !request.predicate_security_safe) {
        return RefuseOverlay("SB-INDEX-ORDERED-PARTIAL-REFUSED",
                             "index.ordered.partial_refused",
                             IndexFamilyName(request.family));
      }
      if (!request.predicate_exact && !request.can_recheck_base_row) {
        return RefuseOverlay("SB-INDEX-ORDERED-PARTIAL-RECHECK-REFUSED",
                             "index.ordered.partial_recheck_refused",
                             IndexFamilyName(request.family));
      }
      decision.requires_recheck = !request.predicate_exact;
      decision.recheck_policy.require_predicate_match = true;
      decision.eligibility = decision.requires_recheck ? OrderedOverlayEligibility::eligible_requires_recheck
                                                       : OrderedOverlayEligibility::eligible_exact;
      decision.steps.push_back("attach_partial_predicate_recheck");
      return decision;
    case OrderedOverlayKind::covering:
      if (!request.covering_payload_requested || request.covering_payload_columns == 0) {
        return RefuseOverlay("SB-INDEX-ORDERED-COVERING-EMPTY",
                             "index.ordered.covering_empty",
                             IndexFamilyName(request.family));
      }
      if (request.covering_payload_admission != nullptr) {
        const auto& admission = *request.covering_payload_admission;
        if (!admission.admitted || admission.fail_closed) {
          return RefuseOverlay("SB-INDEX-ORDERED-COVERING-PAYLOAD-REFUSED",
                               "index.ordered.covering_payload_refused",
                               admission.diagnostic.diagnostic_code);
        }
        decision.index_only_allowed = admission.index_only_admitted;
        if (!decision.index_only_allowed &&
            (!request.can_recheck_base_row ||
             !admission.base_row_recheck_handoff_proven)) {
          return RefuseOverlay("SB-INDEX-ORDERED-COVERING-RECHECK-REFUSED",
                               "index.ordered.covering_recheck_refused",
                               IndexFamilyName(request.family));
        }
        decision.requires_recheck = !decision.index_only_allowed;
        decision.recheck_policy.require_predicate_match =
            decision.requires_recheck || admission.base_row_recheck_required;
        decision.eligibility = decision.requires_recheck
                                   ? OrderedOverlayEligibility::eligible_requires_recheck
                                   : OrderedOverlayEligibility::eligible_exact;
        decision.steps.push_back("consume_covering_payload_admission");
        decision.steps.push_back(decision.index_only_allowed
                                     ? "admit_covering_index_only_projection"
                                     : "admit_covering_with_base_row_recheck");
        return decision;
      }
      decision.index_only_allowed = request.payload_freshness_proven && request.security_projection_safe;
      if (!decision.index_only_allowed && !request.can_recheck_base_row) {
        return RefuseOverlay("SB-INDEX-ORDERED-COVERING-RECHECK-REFUSED",
                             "index.ordered.covering_recheck_refused",
                             IndexFamilyName(request.family));
      }
      decision.requires_recheck = !decision.index_only_allowed;
      decision.recheck_policy.require_predicate_match = decision.requires_recheck;
      decision.eligibility = decision.requires_recheck ? OrderedOverlayEligibility::eligible_requires_recheck
                                                       : OrderedOverlayEligibility::eligible_exact;
      decision.steps.push_back(decision.index_only_allowed ? "admit_covering_index_only_projection"
                                                           : "admit_covering_with_base_row_recheck");
      return decision;
  }
  return RefuseOverlay("SB-INDEX-ORDERED-OVERLAY-UNKNOWN",
                       "index.ordered.overlay_unknown",
                       IndexFamilyName(request.family));
}

OrderedAliasDecision DecideOrderedAlias(const OrderedAliasRequest& request) {
  const std::string family = Normalize(request.requested_family);
  const bool known = family == "btree" ||
                     family == "unique_btree" ||
                     family == "art" ||
                     family == "adaptive_radix_tree" ||
                     family == "trie" ||
                     family == "prefix_trie" ||
                     family == "stl_sort" ||
                     family == "neo4j_range";
  if (!known) {
    return RefuseAlias("SB-INDEX-ORDERED-ALIAS-UNKNOWN",
                       "index.ordered.alias_unknown",
                       request.requested_family);
  }
  if (request.donor_requires_native_catalog_identity || request.donor_requires_native_page_metrics) {
    return RefuseAlias("SB-INDEX-ORDERED-ALIAS-NATIVE-REQUIRED",
                       "index.ordered.alias_native_required",
                       request.requested_family);
  }

  OrderedAliasDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.native_family = family == "unique_btree" ? IndexFamily::unique_btree : IndexFamily::btree;
  decision.independent_catalog_identity = true;
  decision.independent_metrics_identity = true;
  decision.independent_optimizer_identity = true;
  decision.semantic_profile_id = family == "btree" || family == "unique_btree" ? "native_btree"
                                                                                : family + "_btree_alias";
  decision.steps.push_back("preserve_named_family_identity");
  if (family == "btree" || family == "unique_btree") {
    decision.decision = OrderedAliasDecisionKind::native_btree;
    decision.steps.push_back("use_native_btree_physical_family");
  } else if (request.equality_only && !request.ordered_iteration_required) {
    decision.decision = OrderedAliasDecisionKind::alias_to_btree_equality_prefix;
    decision.steps.push_back("map_equality_to_btree_prefix_probe");
  } else {
    decision.decision = OrderedAliasDecisionKind::alias_to_btree_ordered;
    decision.steps.push_back(request.prefix_navigation_required ? "map_prefix_navigation_to_btree_range"
                                                                : "map_ordered_iteration_to_btree_scan");
  }
  if (!request.persistent_required) {
    decision.steps.push_back("mark_alias_rebuildable_for_temporary_scope");
  }
  return decision;
}

const std::vector<OrderedDonorSemanticProfile>& BuiltinOrderedDonorSemanticProfiles() {
  static const std::vector<OrderedDonorSemanticProfile> profiles = {
      DonorProfile(OrderedDonorEngine::scratchbird,
                   "sb_native_btree",
                   "scratchbird",
                   IndexKeyNullPlacement::nulls_last,
                   IndexKeyNullPlacement::nulls_first,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   true,
                   true,
                   false,
                   false,
                   {}),
      DonorProfile(OrderedDonorEngine::firebird,
                   "firebird_btree_ordered",
                   "firebird",
                   IndexKeyNullPlacement::nulls_first,
                   IndexKeyNullPlacement::nulls_last,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   false,
                   false,
                   true,
                   false,
                   {"partial_indexes", "include_covering_columns"}),
      DonorProfile(OrderedDonorEngine::postgresql,
                   "postgresql_btree_ordered",
                   "postgresql",
                   IndexKeyNullPlacement::nulls_last,
                   IndexKeyNullPlacement::nulls_first,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   true,
                   true,
                   false,
                   false,
                   {}),
      DonorProfile(OrderedDonorEngine::mysql,
                   "mysql_innodb_btree_ordered",
                   "mysql",
                   IndexKeyNullPlacement::nulls_first,
                   IndexKeyNullPlacement::nulls_last,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   false,
                   false,
                   true,
                   false,
                   {"partial_indexes", "include_covering_columns"}),
      DonorProfile(OrderedDonorEngine::mariadb,
                   "mariadb_innodb_btree_ordered",
                   "mariadb",
                   IndexKeyNullPlacement::nulls_first,
                   IndexKeyNullPlacement::nulls_last,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   false,
                   false,
                   true,
                   false,
                   {"partial_indexes", "include_covering_columns"}),
      DonorProfile(OrderedDonorEngine::sqlite,
                   "sqlite_btree_ordered",
                   "sqlite",
                   IndexKeyNullPlacement::nulls_first,
                   IndexKeyNullPlacement::nulls_last,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   true,
                   false,
                   true,
                   false,
                   {"include_covering_columns"}),
      DonorProfile(OrderedDonorEngine::duckdb,
                   "duckdb_ordered_art_alias",
                   "duckdb",
                   IndexKeyNullPlacement::nulls_last,
                   IndexKeyNullPlacement::nulls_first,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   true,
                   true,
                   false,
                   false,
                   true,
                   true,
                   {"partial_indexes", "include_covering_columns"}),
      DonorProfile(OrderedDonorEngine::neo4j,
                   "neo4j_range_ordered_alias",
                   "neo4j",
                   IndexKeyNullPlacement::nulls_last,
                   IndexKeyNullPlacement::nulls_last,
                   OrderedNullUniquenessPolicy::nulls_distinct,
                   false,
                   false,
                   false,
                   false,
                   true,
                   true,
                   {"sql_expression_indexes", "partial_indexes", "include_covering_columns", "stored_null_keys"})};
  return profiles;
}

const OrderedDonorSemanticProfile* FindOrderedDonorSemanticProfile(OrderedDonorEngine donor,
                                                                   std::string_view profile_id) {
  for (const auto& profile : BuiltinOrderedDonorSemanticProfiles()) {
    if (profile.donor == donor && (profile_id.empty() || profile.profile_id == profile_id)) {
      return &profile;
    }
  }
  return nullptr;
}

OrderedDonorProfileDecision ApplyOrderedDonorSemanticProfile(const OrderedDonorProfileRequest& request) {
  const auto* profile = FindOrderedDonorSemanticProfile(request.donor, request.profile_id);
  if (!profile) {
    return RefuseDonor("SB-INDEX-ORDERED-DONOR-PROFILE-UNKNOWN",
                       "index.ordered.donor_profile_unknown",
                       request.profile_id.empty() ? OrderedDonorEngineName(request.donor) : request.profile_id);
  }
  if (request.descending_key_requested && !profile->supports_descending_keys) {
    return RefuseDonor("SB-INDEX-ORDERED-DONOR-DESCENDING-REFUSED",
                       "index.ordered.donor_descending_refused",
                       profile->profile_id);
  }
  if (request.prefix_seek_requested && !profile->supports_prefix_seek) {
    return RefuseDonor("SB-INDEX-ORDERED-DONOR-PREFIX-REFUSED",
                       "index.ordered.donor_prefix_refused",
                       profile->profile_id);
  }
  if (profile->requires_collation_epoch && !request.collation_epoch_valid) {
    return RefuseDonor("SB-INDEX-ORDERED-DONOR-COLLATION-EPOCH-STALE",
                       "index.ordered.donor_collation_epoch_stale",
                       profile->profile_id);
  }
  if (request.donor_catalog_projection_requested && !profile->catalog_projection_allowed) {
    return RefuseDonor("SB-INDEX-ORDERED-DONOR-CATALOG-REFUSED",
                       "index.ordered.donor_catalog_refused",
                       profile->profile_id);
  }

  OrderedDonorProfileDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.profile = *profile;
  decision.catalog_projection_allowed = profile->catalog_projection_allowed && request.donor_catalog_projection_requested;
  decision.steps.push_back("load_donor_ordered_semantic_profile");

  OrderedAccessRequest access = request.access;
  access.family = access.family == IndexFamily::unknown || access.family == IndexFamily::donor_emulated
                      ? profile->native_family
                      : access.family;
  access.semantic_profile = profile->key_profile;
  access.allow_fallback_sort = request.allow_fallback_sort || profile->may_require_fallback_sort;
  access.require_total_order_proof = access.require_total_order_proof || request.descending_key_requested;
  decision.access_plan = PlanOrderedBTreeAccess(access);
  if (!decision.access_plan.ok()) {
    decision.status = decision.access_plan.status;
    decision.admitted = false;
    decision.diagnostic = decision.access_plan.diagnostic;
    return decision;
  }
  decision.fallback_sort_required = decision.access_plan.fallback_sort_required || profile->may_require_fallback_sort;
  decision.steps.push_back("plan_native_btree_access_for_donor_profile");

  if (request.uniqueness_requested) {
    OrderedUniquenessRequest uniqueness = request.uniqueness;
    uniqueness.family = uniqueness.family == IndexFamily::unknown || uniqueness.family == IndexFamily::donor_emulated
                            ? IndexFamily::unique_btree
                            : uniqueness.family;
    uniqueness.semantic_profile = profile->key_profile;
    uniqueness.null_policy = profile->unique_null_policy;
    uniqueness.donor_profile_nulls_distinct = profile->unique_null_policy != OrderedNullUniquenessPolicy::nulls_not_distinct;
    decision.uniqueness_decision = DecideOrderedUniquenessPolicy(uniqueness);
    if (!decision.uniqueness_decision.ok()) {
      decision.status = decision.uniqueness_decision.status;
      decision.admitted = false;
      decision.diagnostic = decision.uniqueness_decision.diagnostic;
      return decision;
    }
    decision.steps.push_back("apply_donor_unique_null_policy");
  } else {
    decision.uniqueness_decision.status = OkStatus();
    decision.uniqueness_decision.admitted = true;
  }

  if (request.overlay_requested) {
    if ((request.overlay.overlay == OrderedOverlayKind::expression && !profile->supports_expression_indexes) ||
        (request.overlay.overlay == OrderedOverlayKind::partial && !profile->supports_partial_indexes) ||
        (request.overlay.overlay == OrderedOverlayKind::covering && !profile->supports_covering_indexes)) {
      return RefuseDonor("SB-INDEX-ORDERED-DONOR-OVERLAY-REFUSED",
                         "index.ordered.donor_overlay_refused",
                         profile->profile_id);
    }
    OrderedOverlayRequest overlay = request.overlay;
    overlay.family = overlay.family == IndexFamily::unknown || overlay.family == IndexFamily::donor_emulated
                         ? profile->native_family
                         : overlay.family;
    decision.overlay_decision = DecideOrderedOverlayEligibility(overlay);
    if (!decision.overlay_decision.ok()) {
      decision.status = decision.overlay_decision.status;
      decision.admitted = false;
      decision.diagnostic = decision.overlay_decision.diagnostic;
      return decision;
    }
    decision.steps.push_back("apply_donor_overlay_policy");
  } else {
    decision.overlay_decision.status = OkStatus();
    decision.overlay_decision.admitted = true;
    decision.overlay_decision.eligibility = OrderedOverlayEligibility::eligible_exact;
  }

  decision.steps.push_back("publish_donor_ordered_profile_decision");
  return decision;
}

const char* OrderedAccessShapeName(OrderedAccessShape shape) {
  switch (shape) {
    case OrderedAccessShape::seek: return "seek";
    case OrderedAccessShape::range: return "range";
    case OrderedAccessShape::prefix: return "prefix";
    case OrderedAccessShape::reverse_seek: return "reverse_seek";
    case OrderedAccessShape::reverse_range: return "reverse_range";
    case OrderedAccessShape::reverse_prefix: return "reverse_prefix";
    case OrderedAccessShape::composite_seek: return "composite_seek";
    case OrderedAccessShape::composite_range: return "composite_range";
    case OrderedAccessShape::full_ordered_scan: return "full_ordered_scan";
  }
  return "unknown";
}

const char* OrderedAccessDecisionName(OrderedAccessDecision decision) {
  switch (decision) {
    case OrderedAccessDecision::admitted_exact: return "admitted_exact";
    case OrderedAccessDecision::admitted_requires_recheck: return "admitted_requires_recheck";
    case OrderedAccessDecision::admitted_with_fallback_sort: return "admitted_with_fallback_sort";
    case OrderedAccessDecision::refused: return "refused";
  }
  return "unknown";
}

const char* OrderedDuplicateLifecycleActionName(OrderedDuplicateLifecycleAction action) {
  switch (action) {
    case OrderedDuplicateLifecycleAction::create_posting_list:
      return "create_posting_list";
    case OrderedDuplicateLifecycleAction::append_duplicate:
      return "append_duplicate";
    case OrderedDuplicateLifecycleAction::append_provisional:
      return "append_provisional";
    case OrderedDuplicateLifecycleAction::mark_dead:
      return "mark_dead";
    case OrderedDuplicateLifecycleAction::purge_dead:
      return "purge_dead";
    case OrderedDuplicateLifecycleAction::refuse_duplicate:
      return "refuse_duplicate";
  }
  return "refuse_duplicate";
}

const char* OrderedDonorEngineName(OrderedDonorEngine donor) {
  switch (donor) {
    case OrderedDonorEngine::scratchbird: return "scratchbird";
    case OrderedDonorEngine::firebird: return "firebird";
    case OrderedDonorEngine::postgresql: return "postgresql";
    case OrderedDonorEngine::mysql: return "mysql";
    case OrderedDonorEngine::mariadb: return "mariadb";
    case OrderedDonorEngine::sqlite: return "sqlite";
    case OrderedDonorEngine::duckdb: return "duckdb";
    case OrderedDonorEngine::neo4j: return "neo4j";
    case OrderedDonorEngine::unknown: return "unknown";
  }
  return "unknown";
}

DiagnosticRecord MakeOrderedAccessDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.ordered_access");
}

}  // namespace scratchbird::core::index
