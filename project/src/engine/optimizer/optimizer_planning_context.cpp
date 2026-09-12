// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_planning_context.hpp"

#include "../../core/hash/hash_digest.hpp"

#include <algorithm>
#include <ranges>
#include <new>
#include <stdexcept>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace {

bool CanonicalUuid(const planner::CanonicalPlannerUuid& value) {
  return scratchbird::core::uuid::IsEngineIdentityUuid(value);
}

bool CanonicalDigest(const std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](const unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool CommonAuthorityValid(const CanonicalPlannerContextAuthority& authority,
                          const std::string_view invalid_state_behavior) {
  return CanonicalUuid(authority.context_uuid) && authority.generation != 0 &&
         CanonicalUuid(authority.authority_uuid) &&
         authority.authority_generation != 0 &&
         authority.confidence_basis_points != 0 &&
         authority.confidence_basis_points <= 10'000 &&
         CanonicalDigest(authority.dependency_signature) &&
         authority.invalid_state_behavior_id == invalid_state_behavior &&
         authority.engine_owned &&
         !authority.parser_execution_authority_claimed &&
         !authority.transaction_visibility_authority_claimed &&
         !authority.transaction_finality_authority_claimed &&
         !authority.recovery_authority_claimed;
}

const planner::CanonicalLogicalPropertyRecord* FindProperty(
    const planner::CanonicalLogicalPropertyCatalog& properties,
    const planner::CanonicalPlannerUuid& property_uuid) {
  const auto found = std::ranges::find_if(
      properties.properties, [&](const auto& property) {
        return property.property_uuid == property_uuid;
      });
  return found == properties.properties.end() ? nullptr : &*found;
}

bool RootRequires(const planner::CanonicalLogicalRelationalNode& root,
                  const planner::CanonicalPlannerUuid& property_uuid) {
  return std::ranges::find(root.required_property_uuids, property_uuid) !=
         root.required_property_uuids.end();
}

void EncodeAuthority(planner::CanonicalPlannerBindingBytes& out,
                     const CanonicalPlannerContextAuthority& authority) {
  out.Identity(authority.context_uuid);
  out.Number(authority.generation);
  out.Identity(authority.authority_uuid);
  out.Number(authority.authority_generation);
  out.Number(authority.confidence_basis_points);
  out.Text(authority.dependency_signature);
  out.Text(authority.invalid_state_behavior_id);
  out.Flag(authority.engine_owned);
  out.Flag(authority.parser_execution_authority_claimed);
  out.Flag(authority.transaction_visibility_authority_claimed);
  out.Flag(authority.transaction_finality_authority_claimed);
  out.Flag(authority.recovery_authority_claimed);
}

std::string HypothesisSetDigest(const CanonicalPlannerWhatIfContext& context) {
  planner::CanonicalPlannerBindingBytes out("planner-what-if-v2");
  out.Number(context.abi_version);
  EncodeAuthority(out, context.authority);
  out.Identity(context.policy_uuid);
  out.Number(context.policy_generation);
  out.Flag(context.enabled); out.Flag(context.advisory_only);
  out.Flag(context.normal_plan_influence_permitted);
  out.Number(context.hypotheses.size());
  for (const auto& hypothesis : context.hypotheses) {
    out.Number(static_cast<std::uint8_t>(hypothesis.hypothesis_kind));
    out.Identity(hypothesis.hypothesis_uuid);
    out.Number(hypothesis.generation);
    out.Text(hypothesis.definition_digest);
  }
  const auto bytes = std::move(out).Take();
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data()), bytes.size());
  return digest.ok() ? scratchbird::core::hash::HexLower(digest.digest) : std::string{};
}

std::string ReplayIdentity(const CanonicalPlannerContinuationContext& context) {
  planner::CanonicalPlannerBindingBytes out("planner-continuation-replay-v2");
  out.Number(context.abi_version);
  EncodeAuthority(out, context.authority);
  out.Identity(context.prepared_statement_uuid);
  out.Number(context.prepared_statement_generation);
  out.Identity(context.cursor_uuid); out.Number(context.cursor_generation);
  out.Identity(context.continuation_token_uuid);
  out.Number(context.continuation_token_generation);
  out.Identity(context.resume_boundary_uuid); out.Number(context.resume_boundary_generation);
  out.Identity(context.result_schema_uuid);
  out.Identity(context.required_ordering_property_uuid);
  out.Identity(context.required_materialization_property_uuid);
  out.Identity(context.required_rewindability_property_uuid);
  out.Number(static_cast<std::uint8_t>(context.cursor_mode));
  out.Number(static_cast<std::uint8_t>(context.holdability));
  out.Flag(context.continuation_requested);
  out.Flag(context.single_use_replay_required);
  return std::move(out).Take();
}

}  // namespace

CanonicalPlannerContextValidationResult ValidateCanonicalPlannerContexts(
    const planner::CanonicalLogicalRelationalGraph& graph,
    const planner::CanonicalLogicalPropertyCatalog& properties,
    const std::optional<CanonicalPlannerContinuationContext>& continuation,
    const std::optional<CanonicalPlannerWhatIfContext>& what_if) try {
  CanonicalPlannerContextValidationResult result;
  const auto refuse = [&](std::string field_id) {
    result = {};
    result.issues.push_back(
        {"QOW-DIAG-OPT-PLANNING-CONTEXT-REFUSAL-V1", std::move(field_id)});
    return result;
  };

  if (continuation.has_value() && what_if.has_value()) {
    return refuse("continuation_what_if_isolation");
  }
  if (!continuation.has_value() && !what_if.has_value()) {
    result.accepted = true;
    result.normal_planning = true;
    result.physical_publication_allowed = true;
    result.cache_admission_allowed = true;
    result.execution_allowed = true;
    return result;
  }
  const auto graph_validation =
      planner::ValidateCanonicalLogicalRelationalGraph(graph);
  const auto property_validation =
      planner::ValidateCanonicalLogicalPropertyCatalog(graph, properties);
  if (!graph_validation.accepted || !property_validation.accepted ||
      graph.bound_sblr_tree_uuid != properties.bound_sblr_tree_uuid) {
    return refuse("validated_logical_information_graph");
  }
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (root == graph.nodes.end()) return refuse("logical_root");

  if (continuation.has_value()) {
    const auto& context = *continuation;
    if (context.abi_version != 1 ||
        !CommonAuthorityValid(context.authority,
                              "reject_continuation_plan") ||
        !CanonicalUuid(context.prepared_statement_uuid) ||
        context.prepared_statement_generation == 0 ||
        !CanonicalUuid(context.cursor_uuid) || context.cursor_generation == 0 ||
        !CanonicalUuid(context.continuation_token_uuid) ||
        context.continuation_token_generation == 0 ||
        !CanonicalUuid(context.resume_boundary_uuid) ||
        context.resume_boundary_generation == 0 ||
        !CanonicalUuid(context.result_schema_uuid) ||
        !CanonicalUuid(context.required_ordering_property_uuid) ||
        !CanonicalUuid(context.required_materialization_property_uuid) ||
        !CanonicalUuid(context.required_rewindability_property_uuid) ||
        !context.continuation_requested ||
        !context.single_use_replay_required ||
        context.cursor_mode < CanonicalPlannerCursorMode::kForwardOnly ||
        context.cursor_mode > CanonicalPlannerCursorMode::kScrollable ||
        context.holdability <
            CanonicalPlannerCursorHoldability::kNotHoldable ||
        context.holdability > CanonicalPlannerCursorHoldability::kHoldable) {
      return refuse("continuation_identity");
    }
    const auto* ordering =
        FindProperty(properties, context.required_ordering_property_uuid);
    const auto* materialization = FindProperty(
        properties, context.required_materialization_property_uuid);
    const auto* rewindability = FindProperty(
        properties, context.required_rewindability_property_uuid);
    const bool ordering_valid =
        ordering != nullptr &&
        ordering->property_kind ==
            planner::CanonicalLogicalPropertyKind::kOrdering &&
        !ordering->ordering_terms.empty() &&
        RootRequires(*root, ordering->property_uuid);
    const bool materialization_valid =
        materialization != nullptr &&
        materialization->property_kind ==
            planner::CanonicalLogicalPropertyKind::kMaterialization &&
        (materialization->materialization_kind ==
             planner::CanonicalLogicalMaterializationKind::kMaterialized ||
         materialization->materialization_kind ==
             planner::CanonicalLogicalMaterializationKind::kSpillBacked) &&
        RootRequires(*root, materialization->property_uuid);
    const bool rewindability_valid =
        rewindability != nullptr &&
        rewindability->property_kind ==
            planner::CanonicalLogicalPropertyKind::kRewindability &&
        (rewindability->rewindability_kind ==
             planner::CanonicalLogicalRewindabilityKind::kRewindable ||
         rewindability->rewindability_kind ==
             planner::CanonicalLogicalRewindabilityKind::kMarkRestore) &&
        RootRequires(*root, rewindability->property_uuid);
    if (!ordering_valid || !materialization_valid || !rewindability_valid) {
      return refuse("continuation_resumable_properties");
    }
    CanonicalPlannerContinuationReceipt receipt;
    receipt.context = context;
    receipt.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    receipt.root_logical_node_id = graph.root_logical_node_id;
    receipt.ordering_requirement_validated = true;
    receipt.materialization_requirement_validated = true;
    receipt.rewindability_requirement_validated = true;
    receipt.resumable_plan = true;
    result.accepted = true;
    result.continuation_planning = true;
    result.physical_publication_allowed = true;
    result.cache_admission_allowed = true;
    result.execution_allowed = true;
    result.continuation_receipt = std::move(receipt);
    return result;
  }

  const auto& context = *what_if;
  if (context.abi_version != 1 ||
      !CommonAuthorityValid(context.authority,
                            "advisory_only_no_normal_plan_influence") ||
      !CanonicalUuid(context.policy_uuid) || context.policy_generation == 0 ||
      !context.enabled || !context.advisory_only ||
      context.normal_plan_influence_permitted || context.hypotheses.empty() ||
      context.hypotheses.size() > 64) {
    return refuse("what_if_isolation");
  }
  using HypothesisKey = std::pair<std::uint8_t, planner::CanonicalPlannerUuid>;
  std::optional<HypothesisKey> previous_key;
  for (const auto& hypothesis : context.hypotheses) {
    const auto known_kind =
        hypothesis.hypothesis_kind >=
            CanonicalPlannerWhatIfHypothesisKind::kIndex &&
        hypothesis.hypothesis_kind <=
            CanonicalPlannerWhatIfHypothesisKind::kPolicy;
    const HypothesisKey key{static_cast<std::uint8_t>(hypothesis.hypothesis_kind),
                            hypothesis.hypothesis_uuid};
    if (!known_kind || !CanonicalUuid(hypothesis.hypothesis_uuid) ||
        hypothesis.generation == 0 ||
        !CanonicalDigest(hypothesis.definition_digest) ||
        (previous_key && key <= *previous_key)) {
      return refuse("what_if_hypothesis_inventory");
    }
    previous_key = key;
  }
  const auto digest = HypothesisSetDigest(context);
  if (!CanonicalDigest(digest)) return refuse("what_if_hypothesis_digest");
  CanonicalPlannerWhatIfReceipt receipt;
  receipt.context = context;
  receipt.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  receipt.root_logical_node_id = graph.root_logical_node_id;
  receipt.hypothesis_set_digest = digest;
  receipt.advisory_plan_only = true;
  result.accepted = true;
  result.what_if_planning = true;
  result.physical_publication_allowed = false;
  result.cache_admission_allowed = false;
  result.execution_allowed = false;
  result.what_if_receipt = std::move(receipt);
  return result;
} catch (const std::bad_alloc&) {
  return {};
} catch (const std::length_error&) {
  return {};
}

bool ValidateCanonicalContinuationPhysicalRoot(
    CanonicalPlannerContinuationReceipt* receipt,
    const executor::TypedPhysicalNodeDag& dag) {
  if (receipt == nullptr) return false;
  receipt->physical_root_delivery_validated = false;
  if (!CanonicalPlannerContinuationReceiptValid(*receipt) ||
      !dag.optimizer_published || !dag.immutable_node_identity_validated ||
      !dag.property_contract_validated ||
      dag.bound_sblr_tree_uuid != receipt->bound_sblr_tree_uuid) {
    return false;
  }
  const auto root = std::ranges::find_if(dag.nodes, [&](const auto& node) {
    return node.physical_node_id == dag.root_physical_node_id;
  });
  if (root == dag.nodes.end() ||
      root->relational_node_id != receipt->root_logical_node_id) {
    return false;
  }
  const auto delivered = [&](const planner::CanonicalPlannerUuid& property_uuid) {
    return std::ranges::find(root->delivered_property_uuids, property_uuid) !=
           root->delivered_property_uuids.end();
  };
  if (!delivered(receipt->context.required_ordering_property_uuid) ||
      !delivered(receipt->context.required_materialization_property_uuid) ||
      !delivered(receipt->context.required_rewindability_property_uuid)) {
    return false;
  }
  receipt->physical_root_delivery_validated = true;
  return true;
}

bool CanonicalPlannerContinuationReceiptValid(
    const CanonicalPlannerContinuationReceipt& receipt) {
  const auto& context = receipt.context;
  return receipt.abi_version == 1 && context.abi_version == 1 &&
         CommonAuthorityValid(context.authority,
                              "reject_continuation_plan") &&
         CanonicalUuid(context.prepared_statement_uuid) &&
         context.prepared_statement_generation != 0 &&
         CanonicalUuid(context.cursor_uuid) && context.cursor_generation != 0 &&
         CanonicalUuid(context.continuation_token_uuid) &&
         context.continuation_token_generation != 0 &&
         CanonicalUuid(context.resume_boundary_uuid) &&
         context.resume_boundary_generation != 0 &&
         CanonicalUuid(context.result_schema_uuid) &&
         CanonicalUuid(context.required_ordering_property_uuid) &&
         CanonicalUuid(context.required_materialization_property_uuid) &&
         CanonicalUuid(context.required_rewindability_property_uuid) &&
         context.continuation_requested && context.single_use_replay_required &&
         context.cursor_mode >= CanonicalPlannerCursorMode::kForwardOnly &&
         context.cursor_mode <= CanonicalPlannerCursorMode::kScrollable &&
         context.holdability >=
             CanonicalPlannerCursorHoldability::kNotHoldable &&
         context.holdability <= CanonicalPlannerCursorHoldability::kHoldable &&
         CanonicalUuid(receipt.bound_sblr_tree_uuid) &&
         receipt.root_logical_node_id != 0 &&
         receipt.ordering_requirement_validated &&
         receipt.materialization_requirement_validated &&
         receipt.rewindability_requirement_validated && receipt.resumable_plan &&
         !receipt.parser_execution_authority_claimed &&
         !receipt.transaction_visibility_authority_claimed &&
         !receipt.transaction_finality_authority_claimed &&
         !receipt.recovery_authority_claimed;
}

bool CanonicalPlannerContinuationReplayMatches(
    const CanonicalPlannerContinuationReceipt& expected,
    const CanonicalPlannerContinuationReplayRequest& replay,
    CanonicalPlannerContinuationReplayReceipt* receipt) {
  if (receipt == nullptr) return false;
  *receipt = {};
  if (!CanonicalPlannerContinuationReceiptValid(expected) ||
      replay.abi_version != 1 || !replay.engine_replay_authorized ||
      !replay.cursor_state_revalidated ||
      !replay.continuation_state_revalidated ||
      replay.parser_execution_authority_claimed ||
      replay.transaction_finality_authority_claimed ||
      replay.recovery_authority_claimed || replay.context != expected.context) {
    return false;
  }
  try {
    CanonicalPlannerContinuationReplayReceipt staged;
    staged.replay_identity = ReplayIdentity(replay.context);
    staged.continuation_context_uuid = replay.context.authority.context_uuid;
    staged.continuation_generation = replay.context.authority.generation;
    staged.prepared_statement_uuid = replay.context.prepared_statement_uuid;
    staged.prepared_statement_generation =
        replay.context.prepared_statement_generation;
    staged.cursor_uuid = replay.context.cursor_uuid;
    staged.cursor_generation = replay.context.cursor_generation;
    staged.continuation_token_uuid = replay.context.continuation_token_uuid;
    staged.continuation_token_generation =
        replay.context.continuation_token_generation;
    staged.resume_boundary_uuid = replay.context.resume_boundary_uuid;
    staged.resume_boundary_generation =
        replay.context.resume_boundary_generation;
    staged.identity_revalidated = true;
    staged.dependency_revalidated = true;
    staged.optimizer_reinvoked = false;
    *receipt = std::move(staged);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

}  // namespace scratchbird::engine::optimizer
