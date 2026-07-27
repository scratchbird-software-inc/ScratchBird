// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query/plan_api.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace scratchbird::engine::sblr {

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

 private:
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
};

}  // namespace scratchbird::engine::sblr
