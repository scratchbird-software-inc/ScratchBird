// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/canonical_query_aggregate_registration.hpp"
#include "engine/sblr/canonical_query_node_composition.hpp"
#include "engine/sblr/canonical_query_object_free_composition_support.hpp"
#include "engine/sblr/canonical_query_set_composition.hpp"

#include <array>
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

  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = 1;
  descriptor.descriptor_uuid = "019f0000-0000-7200-8000-000000009c01";
  descriptor.type_uuid = sblr::ExactCanonicalCoreDatatypeTypeUuidV1("int64");
  descriptor.nullability = api::RelationalNullability::kNonNull;
  dag = {};
  dag.descriptors = {descriptor};
  expression = {};
  expression.expression_id = 1;
  expression.expression_kind = api::RelationalExpressionKind::kLiteral;
  expression.result_descriptor_id = 1;
  expression.literal_kind = api::RelationalLiteralKind::kNumeric;
  for (const auto payload : {"0", "17", "9223372036854775807", "-1", "9223372036854775808"}) {
    expression.literal_or_parameter_ref = payload;
    dag.expressions = {expression};
    sblr::CanonicalRelationalExpressionRuntime runtime(dag);
    std::uint64_t bound = 99;
    const bool ok = sblr::EvaluateNonNegativeRowBoundForComposition(&runtime, 1, &bound, &detail);
    const bool admitted = std::string_view(payload) != "-1" &&
                          std::string_view(payload) != "9223372036854775808";
    results.Check(ok == admitted && (!admitted || bound == std::stoull(payload)),
                  "row bound: " + std::string(payload));
  }
  std::uint64_t bound = 0;
  results.Check(!sblr::EvaluateNonNegativeRowBoundForComposition(nullptr, 1, &bound, &detail),
                "missing row-bound runtime");
}

}  // namespace

int main() {
  Results results;
  SetProfilesAndBounds(results);
  SubqueryProfilesAndCardinality(results);
  RowBindingAndBounds(results);
  std::cout << "relational_preparation_cases=" << results.cases
            << " failures=" << results.failures << '\n';
  return results.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
