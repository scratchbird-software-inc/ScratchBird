// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-IAS-006-V1: " << detail << '\n';
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

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = kOwner;
  graph.statement_snapshot_id = 0;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 2;
  graph.result_descriptor_ids = {201};

  plan::CanonicalLogicalRelationalNode values;
  values.logical_node_id = 1;
  values.node_kind = plan::CanonicalLogicalRelationalNodeKind::kValues;
  values.output_descriptor_ids = {201};
  values.bound_expression_ids = {11, 12};
  values.origin_relational_node_ids = {1};
  values.semantic_variant_id = "values.literal-table.v1";

  plan::CanonicalLogicalRelationalNode filter;
  filter.logical_node_id = 2;
  filter.node_kind = plan::CanonicalLogicalRelationalNodeKind::kFilter;
  filter.input_logical_node_ids = {1};
  filter.output_descriptor_ids = {201};
  filter.bound_expression_ids = {11, 12};
  filter.origin_relational_node_ids = {2};
  filter.semantic_variant_id = "filter.where.v1";
  graph.nodes = {values, filter};
  return graph;
}

plan::CanonicalLogicalPropertyCatalog Catalog() {
  plan::CanonicalLogicalPropertyCatalog catalog;
  catalog.bound_sblr_tree_uuid = Uuid(1);
  catalog.catalog_epoch_uuid = Uuid(2);
  catalog.security_context_uuid = Uuid(3);
  catalog.local_transaction_id = kOwner;
  catalog.statement_snapshot_id = 0;
  catalog.mga_statement_context = MgaContext();

  plan::CanonicalLogicalPropertyRecord equivalence;
  equivalence.property_uuid = Uuid(101);
  equivalence.property_kind =
      plan::CanonicalLogicalPropertyKind::kExpressionEquivalence;
  equivalence.origin_logical_node_id = 1;
  equivalence.expression_ids = {12, 11};
  equivalence.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord ordering;
  ordering.property_uuid = Uuid(102);
  ordering.property_kind = plan::CanonicalLogicalPropertyKind::kOrdering;
  ordering.origin_logical_node_id = 1;
  ordering.ordering_terms = {
      {11, plan::CanonicalLogicalPropertySortDirection::kAscending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsLast, Uuid(901)},
      {12, plan::CanonicalLogicalPropertySortDirection::kDescending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsFirst, Uuid(901)},
  };
  ordering.populated_from_bound_sblr = true;
  catalog.properties = {equivalence, ordering};
  return catalog;
}

std::vector<plan::CanonicalLogicalNodePropertyBinding> Bindings() {
  return {
      {1, {}, {Uuid(101), Uuid(102)}},
      {2, {Uuid(102)}, {Uuid(101), Uuid(102)}},
  };
}

plan::CanonicalLogicalPropertyPopulationResult Populated() {
  return plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Catalog(), Bindings());
}

bool ValidateCanonicalSerialization() {
  const auto populated = Populated();
  if (!Require(populated.accepted, "property population failed")) {
    return false;
  }
  const auto baseline = plan::SerializeCanonicalLogicalPropertyCatalog(
      populated.logical_graph, populated.property_catalog);
  bool passed = true;
  passed &= Require(baseline.accepted &&
                        !baseline.canonical_serialization.empty(),
                    "valid property state did not serialize");

  auto reordered_catalog = populated.property_catalog;
  std::ranges::reverse(reordered_catalog.properties);
  auto reordered_graph = populated.logical_graph;
  std::ranges::reverse(reordered_graph.nodes[0].delivered_property_uuids);
  const auto reordered = plan::SerializeCanonicalLogicalPropertyCatalog(
      reordered_graph, reordered_catalog);
  passed &= Require(reordered.accepted &&
                        reordered.canonical_serialization ==
                            baseline.canonical_serialization,
                    "semantic set or record order changed serialization");

  auto changed_catalog = populated.property_catalog;
  std::ranges::reverse(changed_catalog.properties[1].ordering_terms);
  const auto changed = plan::SerializeCanonicalLogicalPropertyCatalog(
      populated.logical_graph, changed_catalog);
  passed &= Require(changed.accepted &&
                        changed.canonical_serialization !=
                            baseline.canonical_serialization,
                    "ordering-term order was not identity-significant");

  auto changed_graph = populated.logical_graph;
  auto changed_scope = populated.property_catalog;
  changed_graph.mga_statement_context.statement_uuid = Uuid(9010);
  changed_scope.mga_statement_context.statement_uuid = Uuid(9010);
  const auto changed_uuid = plan::SerializeCanonicalLogicalPropertyCatalog(
      changed_graph, changed_scope);
  passed &= Require(changed_uuid.accepted &&
                        changed_uuid.canonical_serialization !=
                            baseline.canonical_serialization,
                    "statement UUID mutation did not change serialization");

  changed_graph = populated.logical_graph;
  changed_scope = populated.property_catalog;
  changed_graph.mga_statement_context.active_excluded_local_transaction_ids =
      {kHorizon, kOldestActive, kOwner};
  changed_graph.mga_statement_context.oldest_active_transaction_id = kHorizon;
  changed_scope.mga_statement_context =
      changed_graph.mga_statement_context;
  const auto changed_vector = plan::SerializeCanonicalLogicalPropertyCatalog(
      changed_graph, changed_scope);
  passed &= Require(changed_vector.accepted &&
                        changed_vector.canonical_serialization !=
                            baseline.canonical_serialization,
                    "snapshot exclusion-vector mutation did not change serialization");

  changed_graph = populated.logical_graph;
  changed_scope = populated.property_catalog;
  changed_graph.mga_statement_context.current = false;
  changed_scope.mga_statement_context.current = false;
  const auto changed_current = plan::SerializeCanonicalLogicalPropertyCatalog(
      changed_graph, changed_scope);
  passed &= Require(changed_current.accepted &&
                        changed_current.canonical_serialization !=
                            baseline.canonical_serialization,
                    "snapshot-current mutation did not change serialization");

  changed_graph = populated.logical_graph;
  changed_scope = populated.property_catalog;
  changed_graph.mga_statement_context.oldest_active_transaction_id =
      kInventoryNext;
  changed_scope.mga_statement_context =
      changed_graph.mga_statement_context;
  const auto invalid_horizon =
      plan::SerializeCanonicalLogicalPropertyCatalog(changed_graph,
                                                     changed_scope);
  passed &= Require(!invalid_horizon.accepted,
                    "future MGA horizon mutation was serialized");

  auto missing_owner_graph = populated.logical_graph;
  auto missing_owner_scope = populated.property_catalog;
  missing_owner_graph.mga_statement_context
      .active_excluded_local_transaction_ids.clear();
  missing_owner_scope.mga_statement_context =
      missing_owner_graph.mga_statement_context;
  const auto missing_owner = plan::SerializeCanonicalLogicalPropertyCatalog(
      missing_owner_graph, missing_owner_scope);
  passed &= Require(!missing_owner.accepted,
                    "MGA vector without its active owner was serialized");

  auto nil_graph = populated.logical_graph;
  auto nil_scope = populated.property_catalog;
  nil_graph.mga_statement_context.statement_snapshot_uuid =
      "00000000-0000-0000-0000-000000000000";
  nil_scope.mga_statement_context = nil_graph.mga_statement_context;
  const auto nil_identity = plan::SerializeCanonicalLogicalPropertyCatalog(
      nil_graph, nil_scope);
  passed &= Require(!nil_identity.accepted,
                    "nil MGA snapshot identity was serialized");

  auto nil_catalog_graph = populated.logical_graph;
  auto nil_catalog_scope = populated.property_catalog;
  nil_catalog_graph.catalog_epoch_uuid =
      "00000000-0000-0000-0000-000000000000";
  nil_catalog_scope.catalog_epoch_uuid = nil_catalog_graph.catalog_epoch_uuid;
  const auto nil_catalog = plan::SerializeCanonicalLogicalPropertyCatalog(
      nil_catalog_graph, nil_catalog_scope);
  passed &= Require(!nil_catalog.accepted,
                    "nil catalog epoch identity was serialized");

  auto conflated_graph = populated.logical_graph;
  auto conflated_scope = populated.property_catalog;
  conflated_graph.catalog_epoch_uuid =
      conflated_graph.mga_statement_context.statement_metadata_snapshot_uuid;
  conflated_scope.catalog_epoch_uuid = conflated_graph.catalog_epoch_uuid;
  const auto conflated_identity =
      plan::SerializeCanonicalLogicalPropertyCatalog(conflated_graph,
                                                     conflated_scope);
  passed &= Require(!conflated_identity.accepted,
                    "metadata snapshot was accepted as the catalog epoch");
  return passed;
}

bool ValidateCompleteStatementContextRefusals() {
  using Mutation =
      std::function<void(plan::CanonicalMgaStatementContext&)>;
  const std::vector<std::pair<std::string_view, Mutation>> mutations = {
      {"missing statement UUID", [](auto& context) {
         context.statement_uuid.clear();
       }},
      {"malformed owner UUID", [](auto& context) {
         context.owning_transaction_uuid = "019F0000-0000-7700-8000-000000009002";
       }},
      {"nil snapshot UUID", [](auto& context) {
         context.statement_snapshot_uuid =
             "00000000-0000-0000-0000-000000000000";
       }},
      {"nil metadata UUID", [](auto& context) {
         context.statement_metadata_snapshot_uuid =
             "00000000-0000-0000-0000-000000000000";
       }},
      {"missing local transaction", [](auto& context) {
         context.owning_local_transaction_id = 0;
       }},
      {"missing snapshot kind", [](auto& context) {
         context.snapshot_kind.clear();
       }},
      {"non-authoritative inventory", [](auto& context) {
         context.inventory_authoritative = false;
       }},
      {"incomplete inventory", [](auto& context) {
         context.complete = false;
       }},
      {"missing inventory next", [](auto& context) {
         context.publication_inventory_next_local_transaction_id = 0;
       }},
      {"truncated inventory next", [](auto& context) {
         context.publication_inventory_next_local_transaction_id =
             static_cast<std::uint32_t>(kInventoryNext);
       }},
      {"missing horizon", [](auto& context) {
         context.oldest_snapshot_transaction_id = 0;
       }},
      {"swapped horizon", [](auto& context) {
         std::swap(context.oldest_snapshot_transaction_id,
                   context.oldest_active_transaction_id);
       }},
      {"unsorted active exclusions", [](auto& context) {
         context.active_excluded_local_transaction_ids =
             {kOwner, kOldestActive};
       }},
      {"duplicate active exclusion", [](auto& context) {
         context.active_excluded_local_transaction_ids =
             {kOldestActive, kOwner, kOwner};
       }},
      {"duplicate in-doubt exclusion", [](auto& context) {
         context.in_doubt_excluded_local_transaction_ids =
             {kInDoubt, kInDoubt};
       }},
      {"overlapping exclusions", [](auto& context) {
         context.in_doubt_excluded_local_transaction_ids = {kOwner};
       }},
      {"overflowing exclusion", [](auto& context) {
         context.in_doubt_excluded_local_transaction_ids =
             {kInventoryNext};
       }},
  };

  bool passed = true;
  for (const auto& [detail, mutate] : mutations) {
    auto graph = Graph();
    auto catalog = Catalog();
    mutate(graph.mga_statement_context);
    catalog.mga_statement_context = graph.mga_statement_context;
    const auto result =
        plan::SerializeCanonicalLogicalPropertyCatalog(graph, catalog);
    passed &= Require(!result.accepted &&
                          result.canonical_serialization.empty(),
                      detail);
  }

  auto narrowed_alias = Graph();
  auto narrowed_catalog = Catalog();
  narrowed_alias.local_transaction_id =
      static_cast<std::uint32_t>(kOwner);
  const auto narrowed = plan::SerializeCanonicalLogicalPropertyCatalog(
      narrowed_alias, narrowed_catalog);
  passed &= Require(!narrowed.accepted &&
                        narrowed.canonical_serialization.empty(),
                    "narrowed legacy local-transaction alias was accepted");
  return passed;
}

bool ValidateScopeInvalidationAndPropagation() {
  const auto populated = Populated();
  bool passed = true;
  auto stale_catalog = populated.property_catalog;
  stale_catalog.catalog_epoch_uuid = Uuid(999);
  const auto stale = plan::SerializeCanonicalLogicalPropertyCatalog(
      populated.logical_graph, stale_catalog);
  passed &= Require(!stale.accepted &&
                        stale.canonical_serialization.empty() &&
                        !stale.issues.empty() &&
                        stale.issues.front().diagnostic_id ==
                            "QOW-DIAG-LOGICAL-PROPERTY-SCOPE-V1",
                    "stale catalog scope serialized as reusable state");

  auto bindings = Bindings();
  bindings[0].delivered_property_uuids = {Uuid(102)};
  const auto broken = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Catalog(), bindings);
  passed &= Require(!broken.accepted && !broken.issues.empty() &&
                        broken.issues.front().diagnostic_id ==
                            "QOW-DIAG-LOGICAL-PROPERTY-PROPAGATION-V1",
                    "missing propagation stage was accepted");
  return passed;
}

}  // namespace

// QOW-TEST-IAS-006-V1
int main() {
  if (!ValidateCanonicalSerialization() ||
      !ValidateCompleteStatementContextRefusals() ||
      !ValidateScopeInvalidationAndPropagation()) {
    return 1;
  }
  std::cout << "QOW-TEST-IAS-006-V1: PASS\n";
  return 0;
}
