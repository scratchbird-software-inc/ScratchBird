// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/canonical_int64_literal_fixture.hpp"
#include "engine/sblr/canonical_query_aggregate_registration.hpp"
#include "engine/sblr/canonical_query_node_composition.hpp"
#include "engine/sblr/canonical_query_object_free_composition_support.hpp"
#include "engine/sblr/canonical_query_set_composition.hpp"
#include "engine/sblr/canonical_query_scalar_support.hpp"

#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace sblr = scratchbird::engine::sblr;

namespace {

struct Results {
  unsigned cases = 0;
  unsigned failures = 0;
  void Check(bool passed, std::string_view name) {
    ++cases;
    if (!passed) {
      ++failures;
      std::cerr << "relational preparation failed: " << name << '\n';
    }
  }
};

void SetProfilesAndBounds(Results& results) {
  using Operation = exec::CanonicalSetOperationKind;
  using Quantifier = exec::CanonicalSetOperationQuantifier;
  struct Base { std::string_view name; Operation operation; Quantifier quantifier; };
  const Base bases[] = {
      {"union-all", Operation::kUnion, Quantifier::kAll},
      {"union-distinct", Operation::kUnion, Quantifier::kDistinct},
      {"intersect-all", Operation::kIntersect, Quantifier::kAll},
      {"intersect-distinct", Operation::kIntersect, Quantifier::kDistinct},
      {"except-all", Operation::kExcept, Quantifier::kAll},
      {"except-distinct", Operation::kExcept, Quantifier::kDistinct}};
  for (const auto& base : bases) {
    for (unsigned mask = 0; mask < 8; ++mask) {
      const auto name = "set-operation." + std::string(base.name) +
          (mask & 1 ? ".by-name" : "") +
          (mask & 2 ? ".type-reconciled" : "") +
          (mask & 4 ? ".null-collation" : "") + ".v1";
      const auto profile = sblr::ResolveLiveSetOperationProfileForComposition(name);
      results.Check(profile.matched && profile.operation == base.operation &&
          profile.quantifier == base.quantifier &&
          profile.alignment == (mask & 1
              ? exec::CanonicalSetOperationAlignment::kByName
              : exec::CanonicalSetOperationAlignment::kOrdinal) &&
          profile.type_profile == (mask & 2
              ? exec::CanonicalSetOperationTypeProfile::kLosslessImplicit
              : exec::CanonicalSetOperationTypeProfile::kExact) &&
          profile.equality_profile == (mask & 4
              ? exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation
              : exec::CanonicalSetOperationEqualityProfile::kExactTyped), name);
      // The bound helper consumes only the already-prepared column count.
      sblr::PreparedSetOperationRoot prepared;
      prepared.ok = true;
      prepared.result_columns.resize(2);
      if (mask & 4) prepared.collation_bindings.resize(2);
      std::uint64_t comparisons = 0, collation = 0;
      results.Check(sblr::BoundSetOperationEqualityComparisonsForComposition(
          prepared, profile, 10, &comparisons, &collation) &&
          comparisons == 100 && collation == (mask & 4 ? 90 : 0),
          "bounded comparisons: " + name);
      results.Check(!sblr::BoundSetOperationEqualityComparisonsForComposition(
          prepared, profile, std::numeric_limits<std::uint64_t>::max(),
          &comparisons, &collation), "overflow refused: " + name);
    }
  }
  for (const auto name : {"", "set-operation.union-all.v2",
      "set-operation.unknown.v1", "set-operation.union-all.by-name.by-name.v1",
      "set-operation.union-all.type-reconciled.by-name.v1",
      "set-operation.union-all.null-collation.type-reconciled.v1",
      "set-operation.union-all.unknown.v1"}) {
    results.Check(!sblr::ResolveLiveSetOperationProfileForComposition(name).matched,
                  "invalid set profile: " + std::string(name));
  }
}

void SubqueryProfilesAndCardinality(Results& results) {
  for (const auto name : {"subquery.exists.v1", "subquery.in-int64.v1",
                          "subquery.in-typed.v1"}) {
    results.Check(sblr::MatchLivePredicateSubqueryProfileForComposition(name).matched, name);
  }
  for (const auto quantifier : {"any", "all"}) {
    for (const auto comparison : {"eq", "ne", "lt", "le", "gt", "ge"}) {
      for (const auto type : {"int64", "typed"}) {
        const auto name = "subquery.quantified-" + std::string(quantifier) +
            "-" + comparison + "-" + type + ".v1";
        const auto profile = sblr::MatchLivePredicateSubqueryProfileForComposition(name);
        results.Check(profile.matched && profile.quantifier ==
            (std::string_view(quantifier) == "any"
                ? exec::CanonicalQuantifiedSubqueryQuantifier::kAny
                : exec::CanonicalQuantifiedSubqueryQuantifier::kAll) &&
            profile.required_operand_type ==
                (std::string_view(type) == "int64" ? "int64" : ""), name);
      }
    }
  }
  for (const auto form : {"lateral-inner", "lateral-left", "cross-apply", "outer-apply"}) {
    for (const auto type : {"int64", "typed"}) {
      const auto name = "join." + std::string(form) + "-" + type + "-equality.v1";
      results.Check(sblr::MatchLiveLateralSubqueryProfileForComposition(name).matched, name);
    }
  }
  for (const auto name : {"cte.recursive-union-all-int64-increment.v1",
      "cte.recursive-union-distinct-int64-increment.v1",
      "cte.recursive-search-breadth-cycle-int64-increment.v1"}) {
    results.Check(sblr::MatchLiveRecursiveCteProfileForComposition(name).matched, name);
  }
  for (const auto name : {"", "unknown.v1", "subquery.exists.v2"}) {
    results.Check(!sblr::MatchLivePredicateSubqueryProfileForComposition(name).matched &&
        !sblr::MatchLiveLateralSubqueryProfileForComposition(name).matched &&
        !sblr::MatchLiveRecursiveCteProfileForComposition(name).matched,
        "unknown subquery profile");
  }
  sblr::PreparedRecursiveCteRoot prepared;
  prepared.profile = sblr::MatchLiveRecursiveCteProfileForComposition(
      "cte.recursive-union-all-int64-increment.v1");
  sblr::LivePhysicalNodeProfile anchor;
  anchor.logical_node_id = 7;
  anchor.physical_node_kind = exec::PhysicalNodeKind::kAggregate;
  anchor.implementation_id = "aggregate.count-star.v1";
  anchor.estimated_rows = 999999;  // Estimate is not the hard cardinality bound.
  results.Check(sblr::BindPreparedRecursiveCteCardinality(&prepared, {anchor}, 7, 9, 10) &&
      prepared.maximum_anchor_row_count == 1 &&
      prepared.maximum_result_row_count == 10 && prepared.rows_examined == 20,
      "recursive hard bound ignores estimated rows");
  results.Check(!sblr::BindPreparedRecursiveCteCardinality(&prepared, {anchor}, 7, 10, 10),
                "recursive ceiling refusal");
  results.Check(!sblr::BindPreparedRecursiveCteCardinality(&prepared, {anchor, anchor}, 7, 9, 10),
                "duplicate recursive anchor refusal");
  results.Check(!sblr::BindPreparedRecursiveCteCardinality(&prepared, {}, 7, 9, 10),
                "missing recursive anchor refusal");
  anchor.implementation_id = "aggregate.sum.v1";
  results.Check(!sblr::BindPreparedRecursiveCteCardinality(&prepared, {anchor}, 7, 9, 10),
                "non-COUNT recursive anchor refusal");
}

void RowBindingAndBounds(Results& results) {
  api::TypedRelationalDag dag;
  for (unsigned id = 1; id <= 2; ++id) {
    api::RelationalExpressionRecord expression;
    expression.expression_id = id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = 100 + id;
    dag.expressions.push_back(expression);
  }
  api::RelationalExpressionRecord expression;
  expression.expression_id = 3;
  expression.expression_kind = api::RelationalExpressionKind::kBinary;
  expression.child_expression_ids = {1, 2};
  expression.result_descriptor_id = 103;
  expression.operator_name = "=";
  dag.expressions.push_back(expression);
  sblr::CanonicalRelationalExpressionRowBinding binding;
  std::string detail;
  std::uint64_t memory = 0;
  results.Check(sblr::PrepareInputRowBindingForComposition(
      dag, 3, {101, 102}, &binding, &detail, &memory) &&
      binding.slots.size() == 2 && memory >= sizeof(binding), "row binding and memory receipt");
  results.Check(!sblr::PrepareInputRowBindingForComposition(
      dag, 3, {101, 101}, &binding, &detail) && binding.slots.empty(),
      "ambiguous row descriptors");
  results.Check(!sblr::PrepareInputRowBindingForComposition(
      dag, 3, {101}, &binding, &detail) && binding.slots.empty(), "missing row identifier");
  dag.expressions[2].child_expression_ids.push_back(99);
  results.Check(!sblr::PrepareInputRowBindingForComposition(
      dag, 3, {101, 102}, &binding, &detail) && binding.slots.empty(), "dangling row child");

  const auto descriptor = scratchbird::tests::Int64LiteralDescriptor(1);
  dag = {};
  dag.descriptors = {descriptor};
  expression = {};
  expression.expression_id = 1;
  expression.expression_kind = api::RelationalExpressionKind::kLiteral;
  expression.result_descriptor_id = 1;
  expression.literal_kind = api::RelationalLiteralKind::kNumeric;
  for (const auto payload : {std::int64_t{0}, std::int64_t{17},
       std::numeric_limits<std::int64_t>::max(), std::int64_t{-1},
       std::numeric_limits<std::int64_t>::min()}) {
    scratchbird::tests::SetInt64Literal(expression, descriptor, payload);
    dag.expressions = {expression};
    sblr::CanonicalRelationalExpressionRuntime runtime(dag);
    std::uint64_t bound = 99;
    const bool ok = sblr::EvaluateNonNegativeRowBoundForComposition(&runtime, 1, &bound, &detail);
    const bool admitted = payload >= 0;
    results.Check(ok == admitted && (admitted ? bound == static_cast<std::uint64_t>(payload) : bound == 99),
                  "row bound: " + std::to_string(payload));
  }
  // Decimal spelling belongs to parser admission, not the engine value carrier.
  for (const auto text : {"0", "17", "9223372036854775808"}) {
    auto invalid = expression;
    invalid.literal_typed_value_v1.reset();
    invalid.literal_or_parameter_ref = text;
    dag.expressions = {invalid};
    sblr::CanonicalRelationalExpressionRuntime runtime(dag);
    std::uint64_t bound = 99;
    results.Check(!sblr::EvaluateNonNegativeRowBoundForComposition(&runtime, 1, &bound, &detail) && bound == 99,
                  "text/overflow row bound refused");
  }
  scratchbird::tests::SetInt64Literal(expression, descriptor, 17);
  for (unsigned mutation = 0; mutation < std::size(scratchbird::tests::kInvalidInt64LiteralCases); ++mutation) {
    dag.descriptors = {descriptor};
    dag.expressions = {expression};
    scratchbird::tests::InvalidateInt64Literal(mutation, dag.descriptors[0], dag.expressions[0]);
    sblr::CanonicalRelationalExpressionRuntime runtime(dag);
    std::uint64_t bound = 99;
    results.Check(!sblr::EvaluateNonNegativeRowBoundForComposition(&runtime, 1, &bound, &detail) &&
                  bound == 99 && !detail.empty(), scratchbird::tests::kInvalidInt64LiteralCases[mutation]);
  }
  dag.descriptors = {descriptor};
  dag.expressions = {expression};
  sblr::CanonicalRelationalExpressionRuntime runtime(dag);
  api::EngineTypedValue native;
  results.Check(runtime.EvaluateForConsumer(1, "int64", api::EngineCanonicalExpressionConsumer::projection,
      &native, &detail) && native.encoded_value.empty() && native.binary_value.size() == 8 &&
      native.descriptor.datatype_descriptor_uuid == descriptor.descriptor_uuid &&
      native.descriptor.datatype_descriptor_generation == descriptor.descriptor_generation,
      "runtime preserves exact binary INT64 binding");
  for (unsigned mutation = 0; mutation < 12; ++mutation) {
    auto invalid = native;
    switch (mutation) {
      case 0: invalid.encoded_value = "17"; break;
      case 1: invalid.binary_value.clear(); invalid.encoded_value = "17"; break;
      case 2: invalid.binary_value.resize(7); break;
      case 3: invalid.descriptor.datatype_descriptor_uuid = {}; break;
      case 4: ++invalid.descriptor.datatype_descriptor_generation; break;
      case 5: invalid.descriptor.encoded_descriptor += ";width=64"; break;
      case 6: invalid.descriptor.collation_uuid = scratchbird::tests::FixtureUuid(1401, 506); break;
      case 7: invalid.descriptor.type_uuid = {}; break;
      case 8: invalid.descriptor.descriptor_uuid = {}; break;
      case 9: invalid.descriptor.descriptor_kind = "domain"; break;
      case 10: invalid.is_null = true; break;
      case 11: invalid.state = api::EngineValueState::sql_null; break;
    }
    std::int64_t decoded = 99;
    results.Check(!sblr::DecodeCanonicalInt64Scalar(invalid, &decoded, &detail) && decoded == 99 && !detail.empty(),
                  "bound scalar decoder refuses malformed identity/carrier atomically");
    exec::DescriptorBatch batch;
    batch.columns.push_back({"int64_value", invalid.descriptor, false, 1});
    batch.rows.push_back({{invalid}});
    results.Check(!exec::ValidateDescriptorBatch(batch).ok,
                  "batch validator refuses the same malformed bound INT64");
  }
  std::uint64_t bound = 0;
  results.Check(!sblr::EvaluateNonNegativeRowBoundForComposition(nullptr, 1, &bound, &detail),
                "missing row-bound runtime");
}

void NativeInt64Values(Results& results) {
  api::TypedRelationalDag dag;
  dag.descriptors.push_back(scratchbird::tests::Int64LiteralDescriptor(1));
  api::RelationalDagNode node;
  node.node_id = 1;
  node.node_kind = api::RelationalDagNodeKind::kValues;
  node.output_descriptor_ids = {1};
  node.semantic_variant_id = "values.literal-table.v1";
  dag.outputs.push_back({1, 1, 1, "int64_value", 1, true, 0});
  std::vector<std::int64_t> expected{0, -1};
  for (unsigned bit = 0; bit < 64; ++bit) {
    expected.push_back(std::bit_cast<std::int64_t>(std::uint64_t{1} << bit));
    expected.push_back(std::bit_cast<std::int64_t>(~(std::uint64_t{1} << bit)));
  }
  for (unsigned i = 0; i < expected.size(); ++i) {
    api::RelationalExpressionRecord expression;
    expression.expression_id = i + 1;
    expression.expression_kind = api::RelationalExpressionKind::kLiteral;
    expression.result_descriptor_id = 1;
    expression.literal_kind = api::RelationalLiteralKind::kNumeric;
    scratchbird::tests::SetInt64Literal(expression, dag.descriptors[0], expected[i]);
    dag.expressions.push_back(expression);
    dag.values_rows.push_back({i + 1, {i + 1}});
    node.values_row_ids.push_back(i + 1);
  }
  dag.nodes.push_back(node);
  scratchbird::engine::planner::CanonicalLogicalRelationalNode logical;
  logical.logical_node_id = 1;
  const auto materialized = sblr::MaterializeValues(dag, logical, {});
  results.Check(materialized.ok && materialized.batch.rows.size() == expected.size(),
                "INT64 VALUES materialization preserves all bit patterns");
  if (!materialized.ok) std::cerr << materialized.detail << '\n';
  if (materialized.ok) {
    const auto& column = materialized.batch.columns[0].descriptor;
    results.Check(column.datatype_descriptor_uuid == dag.descriptors[0].descriptor_uuid &&
                  column.datatype_descriptor_generation == dag.descriptors[0].descriptor_generation,
                  "INT64 VALUES column retains exact datatype authority");
    for (unsigned i = 0; i < expected.size(); ++i) {
      const auto& value = materialized.batch.rows[i].values[0];
      std::int64_t decoded = 0;
      std::string detail;
      results.Check(value.descriptor == column && value.encoded_value.empty() &&
          value.binary_value == dag.expressions[i].literal_typed_value_v1->canonical_value_bytes &&
          sblr::DecodeCanonicalInt64Scalar(value, &decoded, &detail) && decoded == expected[i],
          "INT64 VALUES has no text conversion or lost binding");
    }
  }
  for (unsigned mutation = 0; mutation < std::size(scratchbird::tests::kInvalidInt64LiteralCases); ++mutation) {
    auto invalid = dag;
    scratchbird::tests::InvalidateInt64Literal(mutation, invalid.descriptors[0], invalid.expressions.back());
    const auto refused = sblr::MaterializeValues(invalid, logical, {});
    results.Check(!refused.ok && refused.batch.columns.empty() && refused.batch.rows.empty() &&
                  refused.result_bindings.empty(), "bad INT64 VALUES binding or final row publishes no prefix");
  }
}

}  // namespace

int main() {
  Results results;
  SetProfilesAndBounds(results);
  SubqueryProfilesAndCardinality(results);
  RowBindingAndBounds(results);
  NativeInt64Values(results);
  std::cout << "relational_preparation_cases=" << results.cases
            << " failures=" << results.failures << '\n';
  return results.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
