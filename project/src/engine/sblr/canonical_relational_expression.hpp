// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query/plan_api.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scratchbird::engine::sblr {

// QOW-SOURCE-QRY-017-HAVING-ROW-BINDING-V1
// A materialized row slot is an engine-owned execution binding. It names the
// exact grouping-key identifier or materialized function expression whose
// already-computed value occupies one descriptor-exact physical row ordinal.
// The explicit kind prevents an identifier from being admitted as an
// aggregate-result slot (or vice versa); no unprepared expression gains row
// authority merely because its descriptor happens to match.
enum class CanonicalRelationalExpressionRowSlotKind : std::uint8_t {
  materialized_function = 0,
  grouping_key = 1,
};

struct CanonicalRelationalExpressionRowSlotBinding {
  std::uint32_t expression_id{0};
  std::uint32_t descriptor_id{0};
  std::size_t row_ordinal{0};
  CanonicalRelationalExpressionRowSlotKind slot_kind{
      CanonicalRelationalExpressionRowSlotKind::materialized_function};
};

struct CanonicalRelationalExpressionRowBinding {
  std::vector<std::uint32_t> row_descriptor_ids;
  std::vector<CanonicalRelationalExpressionRowSlotBinding> slots;
};

// RCP-024 catalog/runtime handoff. The expression evaluator retains ownership
// of descriptor, NULL, cast, comparison, and consumer semantics; these
// callbacks provide only engine-owned identities or calculated scalar values
// that cannot be derived from the object-free relational graph.
struct CanonicalRelationalExpressionRuntimeServices {
  std::function<bool(std::string_view type_uuid,
                     std::string* canonical_type_name,
                     std::string* diagnostic_id,
                     std::string* refusal_detail)>
      descriptor_type_resolver;
  std::function<bool(
      std::string_view function_uuid,
      const std::vector<internal_api::EngineTypedValue>& arguments,
      internal_api::EngineTypedValue* value,
      std::string* diagnostic_id,
      std::string* refusal_detail)>
      function_evaluator;
  std::function<bool(const internal_api::EngineTypedValue& left,
                     const internal_api::EngineTypedValue& right,
                     int* comparison,
                     std::string* diagnostic_id,
                     std::string* refusal_detail)>
      comparison_evaluator;
};

// Object-free first consumer of the canonical relational expression graph.
// Parameters remain refused until an engine-owned value binding exists;
// functions, collations, temporal profiles, and non-core descriptor types are
// accepted only through the engine-owned services above.
class CanonicalRelationalExpressionRuntime {
 public:
  explicit CanonicalRelationalExpressionRuntime(
      const internal_api::TypedRelationalDag& dag,
      CanonicalRelationalExpressionRuntimeServices services = {});

  bool InferType(std::uint32_t expression_id,
                 std::optional<std::string_view> expected_type,
                 std::string* canonical_type_name,
                 std::string* refusal_detail);

  bool Evaluate(std::uint32_t expression_id,
                std::string_view expected_type,
                internal_api::EngineTypedValue* value,
                std::string* refusal_detail);

  bool EvaluateForConsumer(
      std::uint32_t expression_id,
      std::string_view expected_type,
      internal_api::EngineCanonicalExpressionConsumer consumer,
      internal_api::EngineTypedValue* value,
      std::string* refusal_detail);

  // Shared object-free predicate seam for FILTER/JOIN/HAVING/QUALIFY
  // consumers. SQL NULL is returned as UNKNOWN; callers retain authority for
  // the consumer-specific TRUE/FALSE/UNKNOWN decision.
  bool EvaluatePredicate(
      std::uint32_t expression_id,
      internal_api::EngineSqlTruthValue* truth,
      std::string* refusal_detail);

  bool EvaluatePredicateForConsumer(
      std::uint32_t expression_id,
      internal_api::EngineCanonicalExpressionConsumer consumer,
      internal_api::EngineSqlTruthValue* truth,
      std::string* refusal_detail);

  // Evaluates one canonical predicate against a materialized engine row.
  // Only explicitly bound materialized function leaves may consume row values;
  // the surrounding literal/operator graph still uses the ordinary canonical
  // inference, typed comparison, and SQL three-valued runtime.
  bool EvaluatePredicate(
      std::uint32_t expression_id,
      const CanonicalRelationalExpressionRowBinding& row_binding,
      const std::vector<internal_api::EngineTypedValue>& row_values,
      internal_api::EngineSqlTruthValue* truth,
      std::string* refusal_detail);

  bool EvaluatePredicateForConsumer(
      std::uint32_t expression_id,
      const CanonicalRelationalExpressionRowBinding& row_binding,
      const std::vector<internal_api::EngineTypedValue>& row_values,
      internal_api::EngineCanonicalExpressionConsumer consumer,
      internal_api::EngineSqlTruthValue* truth,
      std::string* refusal_detail);

 private:
  struct ActiveRowBinding {
    std::unordered_map<std::uint32_t,
                       const internal_api::EngineTypedValue*>
        values_by_expression;
  };

  bool PrepareRowBinding(
      std::uint32_t root_expression_id,
      const CanonicalRelationalExpressionRowBinding& row_binding,
      const std::vector<internal_api::EngineTypedValue>& row_values,
      internal_api::EngineCanonicalExpressionConsumer consumer,
      ActiveRowBinding* prepared,
      std::string* refusal_detail) const;
  bool InferTypeInternal(std::uint32_t expression_id,
                         std::optional<std::string_view> expected_type,
                         std::string* canonical_type_name,
                         std::string* refusal_detail);
  bool EvaluateInternal(std::uint32_t expression_id,
                        std::string_view expected_type,
                        internal_api::EngineTypedValue* value,
                        std::string* refusal_detail);
  bool BindDescriptorType(std::uint32_t descriptor_id,
                          std::string_view type_name,
                          std::string* refusal_detail);
  bool BuildDescriptor(std::uint32_t descriptor_id,
                       std::string_view type_name,
                       internal_api::EngineDescriptor* descriptor,
                       std::string* refusal_detail) const;
  bool FinishValue(std::uint32_t descriptor_id,
                   internal_api::EngineTypedValue value,
                   internal_api::EngineTypedValue* output,
                   std::string* refusal_detail) const;
  bool IsNullPredicateRight(std::uint32_t expression_id, bool* negate) const;
  bool ResolveDescriptorType(
      const internal_api::RelationalTypeDescriptor& descriptor,
      std::string* canonical_type_name,
      std::string* refusal_detail) const;

  std::unordered_map<
      std::uint32_t, const internal_api::RelationalTypeDescriptor*>
      descriptors_;
  std::unordered_map<
      std::uint32_t, const internal_api::RelationalExpressionRecord*>
      expressions_;
  std::unordered_map<std::uint32_t, std::string> descriptor_type_names_;
  std::unordered_set<std::uint32_t> inference_stack_;
  CanonicalRelationalExpressionRuntimeServices services_;
  const ActiveRowBinding* active_row_binding_{nullptr};
  internal_api::EngineCanonicalExpressionConsumer active_consumer_{
      internal_api::EngineCanonicalExpressionConsumer::projection};
};

}  // namespace scratchbird::engine::sblr
