// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-002-V1: " << detail << '\n';
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7700-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

plan::CanonicalMgaStatementContext MgaContext() {
  plan::CanonicalMgaStatementContext context;
  context.statement_uuid = Uuid(9001);
  context.owning_transaction_uuid = Uuid(9002);
  context.statement_snapshot_uuid = Uuid(9003);
  context.statement_metadata_snapshot_uuid = Uuid(9004);
  context.owning_local_transaction_id = kOwner;
  context.visible_committed_high_watermark = 0;
  context.oldest_active_transaction_id = kOldestActive;
  context.oldest_interesting_transaction_id = kHorizon;
  context.oldest_snapshot_transaction_id = kHorizon;
  context.retention_horizon_transaction_id = kHorizon;
  context.active_excluded_local_transaction_ids = {kOldestActive, kOwner};
  context.in_doubt_excluded_local_transaction_ids = {kInDoubt};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = kInventoryNext;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

plan::CanonicalLogicalRelationalNode Node(
    const std::uint32_t id,
    const plan::CanonicalLogicalRelationalNodeKind kind,
    std::vector<std::uint32_t> inputs,
    const std::uint32_t descriptor_id,
    std::string semantic_variant) {
  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = id;
  node.node_kind = kind;
  node.input_logical_node_ids = std::move(inputs);
  node.output_descriptor_ids = {descriptor_id};
  node.bound_expression_ids = {800 + id};
  node.origin_relational_node_ids = {id};
  node.semantic_variant_id = std::move(semantic_variant);
  return node;
}

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = kOwner;
  graph.statement_snapshot_id = 0;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 3;
  graph.result_descriptor_ids = {103};
  graph.nodes = {
      Node(1, plan::CanonicalLogicalRelationalNodeKind::kRelationSource, {},
           101, "source.bound-relation.v1"),
      Node(2, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 102,
           "values.literal-table.v1"),
      Node(3, plan::CanonicalLogicalRelationalNodeKind::kJoin, {1, 2}, 103,
           "join.inner.v1"),
  };
  return graph;
}

plan::CanonicalPhysicalAlternativeRecord Alternative(
    const std::uint64_t ordinal,
    const std::uint32_t node_id,
    std::string implementation_id,
    const std::uint32_t descriptor_id) {
  plan::CanonicalPhysicalAlternativeRecord alternative;
  alternative.alternative_uuid = Uuid(100 + ordinal);
  alternative.logical_node_id = node_id;
  alternative.implementation_id = std::move(implementation_id);
  alternative.capability_uuid = Uuid(200 + ordinal);
  alternative.output_descriptor_ids = {descriptor_id};
  alternative.available = true;
  return alternative;
}

plan::CanonicalPhysicalAlternativeCatalog Catalog() {
  plan::CanonicalPhysicalAlternativeCatalog catalog;
  catalog.bound_sblr_tree_uuid = Uuid(1);
  catalog.catalog_epoch_uuid = Uuid(2);
  catalog.security_context_uuid = Uuid(3);
  catalog.local_transaction_id = kOwner;
  catalog.statement_snapshot_id = 0;
  catalog.mga_statement_context = MgaContext();
  catalog.alternatives = {
      Alternative(1, 1, "scan.heap.v1", 101),
      Alternative(2, 1, "scan.index.v1", 101),
      Alternative(3, 2, "values.literal.v1", 102),
      Alternative(4, 3, "join.hash.v1", 103),
      Alternative(5, 3, "join.nested-loop.v1", 103),
  };
  return catalog;
}

bool HasIssue(
    const plan::CanonicalLogicalPhysicalBoundaryValidationResult& result,
    const std::string_view diagnostic_id,
    const std::uint32_t node_id) {
  for (const auto& issue : result.issues) {
    if (issue.diagnostic_id == diagnostic_id &&
        issue.logical_node_id == node_id) {
      return true;
    }
  }
  return false;
}

// QOW-TEST-OPT-002-V1
bool ValidateSeparatedAlternatives() {
  auto graph = Graph();
  const auto original_source_semantics = graph.nodes[0].semantic_variant_id;
  const auto original_join_semantics = graph.nodes[2].semantic_variant_id;
  const auto result =
      plan::ValidateCanonicalLogicalPhysicalBoundary(graph, Catalog());
  bool passed = true;
  passed &= Require(result.accepted && !result.data_access_allowed &&
                        result.validated_alternative_count == 5 &&
                        result.executable_alternative_count == 5,
                    "valid separated physical alternatives were refused");
  passed &= Require(graph.nodes[0].semantic_variant_id ==
                            original_source_semantics &&
                        graph.nodes[2].semantic_variant_id ==
                            original_join_semantics,
                    "alternative validation mutated logical semantics");
  return passed;
}

bool ValidateUnavailableBeforeAccess() {
  auto catalog = Catalog();
  catalog.alternatives[0].available = false;
  catalog.alternatives[0].refusal_diagnostic_id =
      "QOW-DIAG-HEAP-UNAVAILABLE-V1";
  catalog.alternatives[1].available = false;
  catalog.alternatives[1].refusal_diagnostic_id =
      "QOW-DIAG-INDEX-UNAVAILABLE-V1";
  const auto result =
      plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  return Require(!result.accepted && !result.data_access_allowed &&
                     HasIssue(result,
                              "QOW-DIAG-PHYSICAL-ALTERNATIVE-UNAVAILABLE-V1",
                              1),
                 "node without an executable alternative allowed data access");
}

bool ValidateIdentityAndShapeRefusal() {
  bool passed = true;
  auto catalog = Catalog();
  catalog.alternatives[1].output_descriptor_ids = {999};
  auto result =
      plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-PHYSICAL-ALTERNATIVE-SHAPE-V1",
                                 1),
                    "alternative changed logical result shape");

  catalog = Catalog();
  catalog.alternatives[1].implementation_id =
      catalog.alternatives[0].implementation_id;
  result = plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-PHYSICAL-ALTERNATIVE-IDENTITY-V1",
                                 1),
                    "duplicate node implementation alternative was accepted");

  catalog = Catalog();
  catalog.alternatives[0].logical_node_id = 999;
  result = plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-PHYSICAL-ALTERNATIVE-IDENTITY-V1",
                                 999),
                    "alternative for an unknown logical node was accepted");
  return passed;
}

bool ValidateBoundaryAndAvailabilityEvidence() {
  bool passed = true;
  auto catalog = Catalog();
  catalog.statement_snapshot_id = 999;
  auto result =
      plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-PHYSICAL-ALTERNATIVE-BOUNDARY-V1",
                                 0),
                    "alternative catalog crossed the statement boundary");

  catalog = Catalog();
  catalog.alternatives[0].available = false;
  result = plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  passed &= Require(!result.accepted &&
                        HasIssue(
                            result,
                            "QOW-DIAG-PHYSICAL-ALTERNATIVE-AVAILABILITY-V1",
                            1),
                    "unavailable alternative omitted its refusal diagnostic");

  catalog = Catalog();
  catalog.raw_sql_text_present = true;
  result = plan::ValidateCanonicalLogicalPhysicalBoundary(Graph(), catalog);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-PHYSICAL-ALTERNATIVE-AUTHORITY-V1",
                                 0),
                    "parser SQL text entered physical alternative admission");

  const auto expect_context_refusal = [&](auto mutation,
                                          const std::string_view detail) {
    auto graph = Graph();
    auto changed = Catalog();
    mutation(graph, changed);
    const auto refused =
        plan::ValidateCanonicalLogicalPhysicalBoundary(graph, changed);
    return Require(
        !refused.accepted && !refused.data_access_allowed &&
            refused.validated_alternative_count == 0 &&
            refused.executable_alternative_count == 0 &&
            HasIssue(refused,
                     "QOW-DIAG-PHYSICAL-ALTERNATIVE-BOUNDARY-V1", 0),
        detail);
  };
  passed &= expect_context_refusal(
      [](auto&, auto& changed) {
        changed.mga_statement_context.statement_snapshot_uuid = Uuid(999);
      },
      "swapped alternative-catalog snapshot context was accepted");
  passed &= expect_context_refusal(
      [](auto&, auto& changed) {
        changed.mga_statement_context.current = false;
      },
      "stale alternative-catalog context was accepted");
  passed &= expect_context_refusal(
      [](auto&, auto& changed) {
        changed.mga_statement_context.statement_uuid.clear();
      },
      "missing alternative-catalog statement UUID was accepted");
  passed &= expect_context_refusal(
      [](auto&, auto& changed) {
        changed.mga_statement_context.active_excluded_local_transaction_ids =
            {kOldestActive, kOwner, kOwner};
      },
      "duplicate alternative-catalog exclusion was accepted");
  passed &= expect_context_refusal(
      [](auto&, auto& changed) {
        changed.local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      },
      "narrowed alternative-catalog transaction alias was accepted");
  passed &= expect_context_refusal(
      [](auto& graph, auto& changed) {
        changed.catalog_epoch_uuid =
            graph.mga_statement_context.statement_metadata_snapshot_uuid;
      },
      "metadata snapshot was accepted as an alternative catalog epoch");
  return passed;
}

}  // namespace

int main() {
  if (!ValidateSeparatedAlternatives() || !ValidateUnavailableBeforeAccess() ||
      !ValidateIdentityAndShapeRefusal() ||
      !ValidateBoundaryAndAvailabilityEvidence()) {
    return 1;
  }
  std::cout << "QOW-TEST-OPT-002-V1: PASS\n";
  return 0;
}
