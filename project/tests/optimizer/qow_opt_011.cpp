// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_candidate_legality.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-011-V1: " << detail << '\n';
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
    const std::uint32_t descriptor,
    std::vector<std::uint32_t> expressions,
    std::string semantic_variant) {
  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = id;
  node.node_kind = kind;
  node.input_logical_node_ids = std::move(inputs);
  node.output_descriptor_ids = {descriptor};
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
  graph.local_transaction_id = 501;
  graph.statement_snapshot_id = 502;
  graph.root_logical_node_id = 4;
  graph.result_descriptor_ids = {103};
  graph.nodes = {
      Node(1, plan::CanonicalLogicalRelationalNodeKind::kValues, {}, 101,
           {11, 12}, "values.literal-table.v1"),
      Node(2, plan::CanonicalLogicalRelationalNodeKind::kAggregate, {1}, 102,
           {11, 12, 13}, "aggregate.group.v1"),
      Node(3, plan::CanonicalLogicalRelationalNodeKind::kSort, {2}, 102,
           {11, 13}, "ordering.logical.v1"),
      Node(4, plan::CanonicalLogicalRelationalNodeKind::kWindow, {3}, 103,
           {11, 13, 14}, "window.over.v1"),
  };
  return graph;
}

plan::CanonicalLogicalPropertyCatalog Properties() {
  using Kind = plan::CanonicalLogicalPropertyKind;
  plan::CanonicalLogicalPropertyCatalog catalog;
  catalog.bound_sblr_tree_uuid = Uuid(1);
  catalog.catalog_epoch_uuid = Uuid(2);
  catalog.security_context_uuid = Uuid(3);
  catalog.local_transaction_id = 501;
  catalog.statement_snapshot_id = 502;

  plan::CanonicalLogicalPropertyRecord source_order;
  source_order.property_uuid = Uuid(101);
  source_order.property_kind = Kind::kOrdering;
  source_order.origin_logical_node_id = 1;
  source_order.ordering_terms = {
      {11, plan::CanonicalLogicalPropertySortDirection::kAscending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsLast, Uuid(901)}};
  source_order.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord grouping;
  grouping.property_uuid = Uuid(102);
  grouping.property_kind = Kind::kGrouping;
  grouping.origin_logical_node_id = 2;
  grouping.expression_ids = {11};
  grouping.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord result_order;
  result_order.property_uuid = Uuid(103);
  result_order.property_kind = Kind::kOrdering;
  result_order.origin_logical_node_id = 3;
  result_order.ordering_terms = {
      {13, plan::CanonicalLogicalPropertySortDirection::kDescending,
       plan::CanonicalLogicalPropertyNullPlacement::kNullsFirst, Uuid(901)}};
  result_order.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord partitioning;
  partitioning.property_uuid = Uuid(104);
  partitioning.property_kind = Kind::kPartitioning;
  partitioning.origin_logical_node_id = 4;
  partitioning.expression_ids = {11};
  partitioning.populated_from_bound_sblr = true;

  plan::CanonicalLogicalPropertyRecord window;
  window.property_uuid = Uuid(105);
  window.property_kind = Kind::kWindow;
  window.origin_logical_node_id = 4;
  window.dependency_property_uuids = {Uuid(103), Uuid(104)};
  window.window_frame_descriptor_uuid = Uuid(902);
  window.populated_from_bound_sblr = true;
  catalog.properties =
      {source_order, grouping, result_order, partitioning, window};
  return catalog;
}

std::vector<plan::CanonicalLogicalNodePropertyBinding> Bindings() {
  return {
      {1, {}, {Uuid(101)}},
      {2, {}, {Uuid(102)}},
      {3, {Uuid(103)}, {Uuid(103)}},
      {4, {Uuid(103), Uuid(104)}, {Uuid(103), Uuid(104), Uuid(105)}},
  };
}

plan::CanonicalPhysicalAlternativeRecord Alternative(
    const std::uint64_t ordinal,
    const std::uint32_t node_id,
    std::string implementation_id,
    const std::uint32_t descriptor,
    std::vector<std::string> required,
    std::vector<std::string> delivered) {
  plan::CanonicalPhysicalAlternativeRecord alternative;
  alternative.alternative_uuid = Uuid(200 + ordinal);
  alternative.logical_node_id = node_id;
  alternative.implementation_id = std::move(implementation_id);
  alternative.capability_uuid = Uuid(300 + ordinal);
  alternative.output_descriptor_ids = {descriptor};
  alternative.available = true;
  alternative.required_property_uuids = std::move(required);
  alternative.delivered_property_uuids = std::move(delivered);
  return alternative;
}

plan::CanonicalPhysicalAlternativeCatalog Alternatives() {
  plan::CanonicalPhysicalAlternativeCatalog catalog;
  catalog.bound_sblr_tree_uuid = Uuid(1);
  catalog.catalog_epoch_uuid = Uuid(2);
  catalog.security_context_uuid = Uuid(3);
  catalog.local_transaction_id = 501;
  catalog.statement_snapshot_id = 502;
  catalog.alternatives = {
      Alternative(1, 1, "values.materialize.v1", 101, {}, {Uuid(101)}),
      Alternative(2, 2, "aggregate.streaming.v1", 102, {Uuid(101)},
                  {Uuid(102)}),
      Alternative(3, 2, "aggregate.hash.v1", 102, {}, {Uuid(102)}),
      Alternative(4, 3, "sort.full.v1", 102, {}, {Uuid(103)}),
      Alternative(5, 3, "sort.noop.v1", 102, {Uuid(103)}, {Uuid(103)}),
      Alternative(6, 4, "window.streaming.v1", 103,
                  {Uuid(103), Uuid(104)},
                  {Uuid(103), Uuid(104), Uuid(105)}),
      Alternative(7, 4, "window.materialize.v1", 103, {Uuid(103)},
                  {Uuid(103), Uuid(104), Uuid(105)}),
  };
  return catalog;
}

struct Inputs {
  plan::CanonicalLogicalRelationalGraph graph;
  plan::CanonicalLogicalPropertyCatalog properties;
  plan::CanonicalPhysicalAlternativeCatalog alternatives;
};

Inputs ValidInputs() {
  const auto populated = plan::PopulateCanonicalLogicalPropertiesFromBoundSblr(
      Graph(), Properties(), Bindings());
  if (!populated.accepted) return {};
  return {populated.logical_graph, populated.property_catalog, Alternatives()};
}

const opt::CanonicalRelationalCandidateLegalityRecord* Candidate(
    const opt::CanonicalRelationalCandidateLegalityResult& result,
    const std::uint64_t ordinal) {
  const auto uuid = Uuid(200 + ordinal);
  for (const auto& candidate : result.candidates) {
    if (candidate.alternative_uuid == uuid) return &candidate;
  }
  return nullptr;
}

bool ValidateCanonicalLegalityAndEnforcement() {
  const auto inputs = ValidInputs();
  const auto result = opt::EvaluateCanonicalRelationalCandidateLegality(
      inputs.graph, inputs.properties, inputs.alternatives);
  bool passed = true;
  passed &= Require(result.accepted && result.complete_legal_coverage &&
                        !result.data_access_allowed &&
                        result.selectable_candidate_count == 5 &&
                        result.candidates.size() == 7,
                    "canonical candidate legality result is incomplete");
  const auto* sort_full = Candidate(result, 4);
  const auto* sort_noop = Candidate(result, 5);
  const auto* window_stream = Candidate(result, 6);
  const auto* window_materialize = Candidate(result, 7);
  passed &= Require(sort_full && sort_full->legal &&
                        sort_full->property_enforcement_required &&
                        sort_full->enforced_property_uuids ==
                            std::vector<std::string>{Uuid(103)},
                    "full sort did not explicitly enforce ordering");
  passed &= Require(sort_noop && !sort_noop->legal &&
                        sort_noop->missing_property_uuids ==
                            std::vector<std::string>{Uuid(103)},
                    "no-op sort ignored missing input order");
  passed &= Require(window_stream && !window_stream->legal &&
                        window_stream->missing_property_uuids ==
                            std::vector<std::string>{Uuid(104)},
                    "streaming window ignored missing partition property");
  passed &= Require(window_materialize && window_materialize->legal &&
                        window_materialize->property_enforcement_required &&
                        window_materialize->enforced_property_uuids ==
                            std::vector<std::string>{Uuid(104)},
                    "materialized window did not enforce partition property");
  return passed;
}

bool ValidateNoMarkersOrDefaults() {
  auto graph = Graph();
  auto properties = Properties();
  properties.properties.clear();
  auto alternatives = Alternatives();
  for (auto& alternative : alternatives.alternatives) {
    alternative.required_property_uuids.clear();
    alternative.delivered_property_uuids.clear();
  }
  const auto result = opt::EvaluateCanonicalRelationalCandidateLegality(
      graph, properties, alternatives);
  bool passed = true;
  passed &= Require(!result.accepted && !result.issues.empty() &&
                        result.issues.front().diagnostic_id ==
                            "QOW-DIAG-CANDIDATE-LOGICAL-PROPERTY-UNAVAILABLE-V1",
                    "sort/window candidates used a default property");

  auto inputs = ValidInputs();
  inputs.alternatives.alternatives[0].required_property_uuids = {Uuid(999)};
  const auto malformed = opt::EvaluateCanonicalRelationalCandidateLegality(
      inputs.graph, inputs.properties, inputs.alternatives);
  passed &= Require(!malformed.accepted && !malformed.issues.empty() &&
                        malformed.issues.front().diagnostic_id ==
                            "QOW-DIAG-CANDIDATE-PROPERTY-REFERENCE-V1",
                    "unknown alternative property was accepted");
  return passed;
}

bool ValidatePerNodeCoverageRefusal() {
  auto inputs = ValidInputs();
  inputs.alternatives.alternatives[3].delivered_property_uuids.clear();
  const auto result = opt::EvaluateCanonicalRelationalCandidateLegality(
      inputs.graph, inputs.properties, inputs.alternatives);
  return Require(result.accepted && !result.complete_legal_coverage &&
                     !result.data_access_allowed && !result.issues.empty() &&
                     result.issues.front().diagnostic_id ==
                         "QOW-DIAG-NO-LEGAL-PROPERTY-CANDIDATE-V1",
                 "node without a property-complete candidate was executable");
}

}  // namespace

// QOW-TEST-OPT-011-V1
int main() {
  if (!ValidateCanonicalLegalityAndEnforcement() ||
      !ValidateNoMarkersOrDefaults() ||
      !ValidatePerNodeCoverageRefusal()) {
    return 1;
  }
  std::cout << "QOW-TEST-OPT-011-V1: PASS\n";
  return 0;
}
