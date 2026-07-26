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

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-003-V1: " << detail << '\n';
  return condition;
}

std::string Uuid(const std::uint64_t suffix) {
  auto text = std::string("019f0000-0000-7700-8000-000000000000");
  const auto digits = std::to_string(suffix);
  text.replace(text.size() - digits.size(), digits.size(), digits);
  return text;
}

plan::CanonicalLogicalRelationalNode Node(
    const std::uint32_t id,
    const plan::CanonicalLogicalRelationalNodeKind kind,
    std::vector<std::uint32_t> inputs,
    const std::uint32_t descriptor_id,
    std::vector<std::uint32_t> expressions,
    std::string semantic_variant) {
  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = id;
  node.node_kind = kind;
  node.input_logical_node_ids = std::move(inputs);
  node.output_descriptor_ids = {descriptor_id};
  node.bound_expression_ids = std::move(expressions);
  node.origin_relational_node_ids = {id};
  node.semantic_variant_id = std::move(semantic_variant);
  return node;
}

plan::CanonicalLogicalRelationalGraph Graph() {
  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(1);
  graph.catalog_epoch_uuid = Uuid(2);
  graph.security_context_uuid = Uuid(3);
  graph.local_transaction_id = 801;
  graph.statement_snapshot_id = 802;
  graph.root_logical_node_id = 3;
  graph.result_descriptor_ids = {102};
  graph.nodes = {
      Node(1, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 101,
           {11, 12}, "values.literal-table.v1"),
      Node(2, plan::CanonicalLogicalRelationalNodeKind::kSort, {1}, 101,
           {11, 12}, "ordering.logical.v1"),
      Node(3, plan::CanonicalLogicalRelationalNodeKind::kWindow, {2}, 102,
           {11, 12, 13}, "window.over.v1"),
  };
  return graph;
}

plan::CanonicalLogicalPropertyCatalog Catalog() {
  using Kind = plan::CanonicalLogicalPropertyKind;
  plan::CanonicalLogicalPropertyCatalog catalog;
  catalog.bound_sblr_tree_uuid = Uuid(1);
  catalog.catalog_epoch_uuid = Uuid(2);
  catalog.security_context_uuid = Uuid(3);
  catalog.local_transaction_id = 801;
  catalog.statement_snapshot_id = 802;

  plan::CanonicalLogicalPropertyRecord equivalence;
  equivalence.property_uuid = Uuid(101);
  equivalence.property_kind = Kind::kExpressionEquivalence;
  equivalence.origin_logical_node_id = 1;
  equivalence.expression_ids = {12, 11};
  equivalence.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord ordering;
  ordering.property_uuid = Uuid(102);
  ordering.property_kind = Kind::kOrdering;
  ordering.origin_logical_node_id = 2;
  ordering.ordering_terms = {{11,
                              plan::CanonicalLogicalPropertySortDirection::kAscending,
                              plan::CanonicalLogicalPropertyNullPlacement::kNullsLast,
                              Uuid(901)}};
  ordering.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord partitioning;
  partitioning.property_uuid = Uuid(103);
  partitioning.property_kind = Kind::kPartitioning;
  partitioning.origin_logical_node_id = 3;
  partitioning.expression_ids = {12};
  partitioning.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord window;
  window.property_uuid = Uuid(104);
  window.property_kind = Kind::kWindow;
  window.origin_logical_node_id = 3;
  window.dependency_property_uuids = {Uuid(103), Uuid(102)};
  window.window_frame_descriptor_uuid = Uuid(902);
  window.populated_from_bound_sblr = true;

  catalog.properties = {equivalence, ordering, partitioning, window};
  return catalog;
}

std::vector<plan::CanonicalLogicalNodePropertyBinding> Bindings() {
  return {
      {1, {}, {Uuid(101)}},
      {2, {Uuid(102)}, {Uuid(101), Uuid(102)}},
      {3, {Uuid(102), Uuid(103)},
       {Uuid(101), Uuid(102), Uuid(104)}},
  };
}

bool HasIssue(const plan::CanonicalLogicalPropertyPopulationResult& result,
              const std::string_view diagnostic_id) {
  return !result.issues.empty() &&
         result.issues.front().diagnostic_id == diagnostic_id;
}

bool ValidateBoundPopulationAndIdentity() {
  const auto result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Catalog(), Bindings());
  bool passed = true;
  passed &= Require(result.accepted && result.issues.empty(),
                    "bound SBLR properties were not populated");
  passed &= Require(
      result.logical_graph.nodes[2].required_property_uuids.size() == 2 &&
          result.logical_graph.nodes[2].delivered_property_uuids.size() == 3,
      "node required/delivered property identities were not populated");
  auto left = Catalog().properties[0];
  auto right = left;
  right.expression_ids = {11, 12};
  passed &= Require(
      plan::SerializeCanonicalLogicalPropertyIdentity(left) ==
          plan::SerializeCanonicalLogicalPropertyIdentity(right),
      "equivalence identity serialization depends on member order");
  auto empty_catalog = Catalog();
  empty_catalog.properties.clear();
  const auto empty = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), empty_catalog, {});
  passed &= Require(empty.accepted && empty.issues.empty(),
                    "property-free query was refused");
  return passed;
}

bool ValidateSourceScopeAndExpressionRefusal() {
  bool passed = true;
  auto catalog = Catalog();
  catalog.properties[0].populated_from_bound_sblr = false;
  auto result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), catalog, Bindings());
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-LOGICAL-PROPERTY-IDENTITY-V1"),
                    "manual property record was accepted as bound SBLR");

  catalog = Catalog();
  catalog.properties[0].expression_ids = {11, 999};
  result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), catalog, Bindings());
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-LOGICAL-PROPERTY-EXPRESSION-V1"),
                    "unbound expression entered a logical property");

  catalog = Catalog();
  catalog.statement_snapshot_id = 999;
  result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), catalog, Bindings());
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-LOGICAL-PROPERTY-SCOPE-V1"),
                    "property catalog crossed statement scope");
  return passed;
}

bool ValidateBindingAndPropagationRefusal() {
  bool passed = true;
  auto graph = Graph();
  graph.nodes[0].delivered_property_uuids = {Uuid(101)};
  auto result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      graph, Catalog(), Bindings());
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-LOGICAL-PROPERTY-PREBOUND-V1"),
                    "manually prebound node metadata was accepted");

  auto bindings = Bindings();
  bindings[1].delivered_property_uuids = {Uuid(102)};
  result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Catalog(), bindings);
  passed &= Require(!result.accepted &&
                        HasIssue(
                            result,
                            "QOW-DIAG-LOGICAL-PROPERTY-PROPAGATION-V1"),
                    "property skipped its direct input propagation chain");

  bindings = Bindings();
  bindings[2].required_property_uuids.push_back(Uuid(999));
  result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Catalog(), bindings);
  passed &= Require(!result.accepted &&
                        HasIssue(result,
                                 "QOW-DIAG-LOGICAL-PROPERTY-REFERENCE-V1"),
                    "unknown required property was accepted");

  result = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Catalog(), Bindings());
  const auto bounded = plan::ValidateCanonicalLogicalPropertyCatalog(
      result.logical_graph, result.property_catalog, 4, 1);
  passed &= Require(!bounded.accepted && !bounded.issues.empty() &&
                        bounded.issues.front().diagnostic_id ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "property reference budget was not enforced");
  return passed;
}

}  // namespace

// QOW-TEST-OPT-003-V1
int main() {
  if (!ValidateBoundPopulationAndIdentity() ||
      !ValidateSourceScopeAndExpressionRefusal() ||
      !ValidateBindingAndPropagationRefusal()) {
    return 1;
  }
  std::cout << "QOW-TEST-OPT-003-V1: PASS\n";
  return 0;
}
