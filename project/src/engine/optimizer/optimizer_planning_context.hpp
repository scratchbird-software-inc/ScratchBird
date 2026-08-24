// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../executor/physical_node_abi.hpp"
#include "../planner/logical_plan.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

enum class CanonicalPlannerCursorMode : std::uint8_t {
  kForwardOnly = 1,
  kScrollable,
};

enum class CanonicalPlannerCursorHoldability : std::uint8_t {
  kNotHoldable = 1,
  kHoldable,
};

enum class CanonicalPlannerWhatIfHypothesisKind : std::uint8_t {
  kIndex = 1,
  kStatistics,
  kPolicy,
};

// Common authority envelope required by every optimizer planning context.
// It is planning metadata only and deliberately carries no MGA statement
// context, transaction handle, visibility decision, or execution authority.
struct CanonicalPlannerContextAuthority {
  std::string context_uuid;
  std::uint64_t generation{0};
  std::string authority_uuid;
  std::uint64_t authority_generation{0};
  std::uint16_t confidence_basis_points{0};
  std::string dependency_signature;
  std::string invalid_state_behavior_id;
  bool engine_owned{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};

  bool operator==(const CanonicalPlannerContextAuthority&) const = default;
};

struct CanonicalPlannerContinuationContext {
  std::uint16_t abi_version{1};
  CanonicalPlannerContextAuthority authority;
  std::string prepared_statement_uuid;
  std::uint64_t prepared_statement_generation{0};
  std::string cursor_uuid;
  std::uint64_t cursor_generation{0};
  std::string continuation_token_uuid;
  std::uint64_t continuation_token_generation{0};
  std::string resume_boundary_uuid;
  std::uint64_t resume_boundary_generation{0};
  std::string result_schema_uuid;
  std::string required_ordering_property_uuid;
  std::string required_materialization_property_uuid;
  std::string required_rewindability_property_uuid;
  CanonicalPlannerCursorMode cursor_mode{
      CanonicalPlannerCursorMode::kForwardOnly};
  CanonicalPlannerCursorHoldability holdability{
      CanonicalPlannerCursorHoldability::kNotHoldable};
  bool continuation_requested{false};
  bool single_use_replay_required{true};

  bool operator==(const CanonicalPlannerContinuationContext&) const = default;
};

struct CanonicalPlannerWhatIfHypothesis {
  CanonicalPlannerWhatIfHypothesisKind hypothesis_kind{
      CanonicalPlannerWhatIfHypothesisKind::kIndex};
  std::string hypothesis_uuid;
  std::uint64_t generation{0};
  std::string definition_digest;

  bool operator==(const CanonicalPlannerWhatIfHypothesis&) const = default;
};

struct CanonicalPlannerWhatIfContext {
  std::uint16_t abi_version{1};
  CanonicalPlannerContextAuthority authority;
  std::string policy_uuid;
  std::uint64_t policy_generation{0};
  std::vector<CanonicalPlannerWhatIfHypothesis> hypotheses;
  bool enabled{false};
  bool advisory_only{true};
  bool normal_plan_influence_permitted{false};

  bool operator==(const CanonicalPlannerWhatIfContext&) const = default;
};

struct CanonicalPlannerContinuationReceipt {
  std::uint16_t abi_version{1};
  CanonicalPlannerContinuationContext context;
  std::string bound_sblr_tree_uuid;
  std::uint32_t root_logical_node_id{0};
  bool ordering_requirement_validated{false};
  bool materialization_requirement_validated{false};
  bool rewindability_requirement_validated{false};
  bool physical_root_delivery_validated{false};
  bool resumable_plan{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};

  bool operator==(const CanonicalPlannerContinuationReceipt&) const = default;
};

struct CanonicalPlannerWhatIfReceipt {
  std::uint16_t abi_version{1};
  CanonicalPlannerWhatIfContext context;
  std::string bound_sblr_tree_uuid;
  std::uint32_t root_logical_node_id{0};
  std::string hypothesis_set_digest;
  bool advisory_plan_only{false};
  bool physical_publication_forbidden{true};
  bool cache_admission_forbidden{true};
  bool execution_forbidden{true};
  bool normal_plan_unchanged{true};

  bool operator==(const CanonicalPlannerWhatIfReceipt&) const = default;
};

struct CanonicalPlannerContextIssue {
  std::string diagnostic_id;
  std::string field_id;
};

struct CanonicalPlannerContextValidationResult {
  bool accepted{false};
  bool normal_planning{false};
  bool continuation_planning{false};
  bool what_if_planning{false};
  bool physical_publication_allowed{false};
  bool cache_admission_allowed{false};
  bool execution_allowed{false};
  std::optional<CanonicalPlannerContinuationReceipt> continuation_receipt;
  std::optional<CanonicalPlannerWhatIfReceipt> what_if_receipt;
  std::vector<CanonicalPlannerContextIssue> issues;
};

struct CanonicalPlannerContinuationReplayRequest {
  std::uint16_t abi_version{1};
  CanonicalPlannerContinuationContext context;
  bool engine_replay_authorized{false};
  bool cursor_state_revalidated{false};
  bool continuation_state_revalidated{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPlannerContinuationReplayReceipt {
  std::string replay_identity;
  std::string continuation_context_uuid;
  std::uint64_t continuation_generation{0};
  std::string prepared_statement_uuid;
  std::uint64_t prepared_statement_generation{0};
  std::string cursor_uuid;
  std::uint64_t cursor_generation{0};
  std::string continuation_token_uuid;
  std::uint64_t continuation_token_generation{0};
  std::string resume_boundary_uuid;
  std::uint64_t resume_boundary_generation{0};
  bool identity_revalidated{false};
  bool dependency_revalidated{false};
  bool consumed_once{false};
  bool optimizer_reinvoked{false};

  bool operator==(const CanonicalPlannerContinuationReplayReceipt&) const =
      default;
};

CanonicalPlannerContextValidationResult ValidateCanonicalPlannerContexts(
    const planner::CanonicalLogicalRelationalGraph& graph,
    const planner::CanonicalLogicalPropertyCatalog& properties,
    const std::optional<CanonicalPlannerContinuationContext>& continuation,
    const std::optional<CanonicalPlannerWhatIfContext>& what_if);

bool ValidateCanonicalContinuationPhysicalRoot(
    CanonicalPlannerContinuationReceipt* receipt,
    const executor::TypedPhysicalNodeDag& dag);

bool CanonicalPlannerContinuationReceiptValid(
    const CanonicalPlannerContinuationReceipt& receipt);

bool CanonicalPlannerContinuationReplayMatches(
    const CanonicalPlannerContinuationReceipt& expected,
    const CanonicalPlannerContinuationReplayRequest& replay,
    CanonicalPlannerContinuationReplayReceipt* receipt);

}  // namespace scratchbird::engine::optimizer
