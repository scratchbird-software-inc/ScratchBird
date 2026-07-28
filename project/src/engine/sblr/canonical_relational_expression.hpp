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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scratchbird::engine::sblr {

// QOW-SOURCE-QRY-017-HAVING-ROW-BINDING-V1
// A materialized row slot is an engine-owned execution binding. It names the
// exact materialized function expression whose already-computed value occupies
// one descriptor-exact physical row ordinal. It does not authorize evaluating
// an identifier or a function that was not explicitly prepared by its
// relational operator as an aggregate-result slot.
struct CanonicalRelationalExpressionRowSlotBinding {
  std::uint32_t expression_id{0};
  std::uint32_t descriptor_id{0};
  std::size_t row_ordinal{0};
};

struct CanonicalRelationalExpressionRowBinding {
  std::vector<std::uint32_t> row_descriptor_ids;
  std::vector<CanonicalRelationalExpressionRowSlotBinding> slots;
};

// Object-free first consumer of the canonical relational expression graph.
// Catalog names, parameters, functions, collations, and temporal profiles are
// deliberately refused until their engine-owned bindings are supplied.
class CanonicalRelationalExpressionRuntime {
 public:
  explicit CanonicalRelationalExpressionRuntime(
      const internal_api::TypedRelationalDag& dag);

  bool InferType(std::uint32_t expression_id,
                 std::optional<std::string_view> expected_type,
                 std::string* canonical_type_name,
                 std::string* refusal_detail);

  bool Evaluate(std::uint32_t expression_id,
                std::string_view expected_type,
                internal_api::EngineTypedValue* value,
                std::string* refusal_detail);

  // Shared object-free predicate seam for FILTER/JOIN/HAVING/QUALIFY
  // consumers. SQL NULL is returned as UNKNOWN; callers retain authority for
  // the consumer-specific TRUE/FALSE/UNKNOWN decision.
  bool EvaluatePredicate(
      std::uint32_t expression_id,
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

  std::unordered_map<
      std::uint32_t, const internal_api::RelationalTypeDescriptor*>
      descriptors_;
  std::unordered_map<
      std::uint32_t, const internal_api::RelationalExpressionRecord*>
      expressions_;
  std::unordered_map<std::uint32_t, std::string> descriptor_type_names_;
  std::unordered_set<std::uint32_t> inference_stack_;
  const ActiveRowBinding* active_row_binding_{nullptr};
};

}  // namespace scratchbird::engine::sblr
