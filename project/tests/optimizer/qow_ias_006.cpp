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
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace plan = scratchbird::engine::planner;

namespace {

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

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = 71;
  graph.statement_snapshot_id = 72;
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
  catalog.local_transaction_id = 71;
  catalog.statement_snapshot_id = 72;

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
      !ValidateScopeInvalidationAndPropagation()) {
    return 1;
  }
  std::cout << "QOW-TEST-IAS-006-V1: PASS\n";
  return 0;
}
