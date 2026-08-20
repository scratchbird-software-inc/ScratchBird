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
  // One descriptor-exact input column consumed by a row-dependent canonical
  // expression.  This is deliberately separate from a grouping key: JOIN,
  // FILTER, and PROJECT may read it, but it never gains aggregate authority.
  input_identifier = 2,
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

// Upper envelope for one descriptor-exact materialized row slot.  The
// expression-memory preflight consumes maxima rather than a particular row so
// planning and execution can bind the same finite predicate program.
struct CanonicalPredicateRowValueEnvelope {
  internal_api::EngineDescriptor descriptor;
  std::uint64_t maximum_encoded_value_bytes{0};
  std::uint64_t maximum_binary_value_bytes{0};
  bool any_non_null{false};
};

struct CanonicalPredicateCallbackRequirements {
  bool descriptor_type_resolution_may_execute{false};
  bool collation_comparison_may_execute{false};
  bool timezone_normalization_may_execute{false};
  bool function_evaluator_may_execute{false};
};

// Ordinary typed-operator logical-memory contract for one prepared row
// predicate.  Logical carrier entries and their dynamic strings are counted;
// allocator buckets, node links, reserved capacity, process RSS, and the
// engine-owned erased-callable service targets are not.  Any service that may
// execute is surfaced through callbacks and keeps callback_memory_complete
// false until a separately bounded callback ABI is present.
// Stage-A callers may inspect this result, but live admission must not consume
// it until callback_memory_complete is also true.
// `ok` means this finite memory analysis succeeded; it does not replace the
// descriptor-batch or scalar semantic validation at the execution seam.
struct CanonicalPredicateLogicalMemoryBound {
  bool ok{false};
  bool core_bound_complete{false};
  bool core_bound_exact{false};
  bool callback_memory_complete{false};

  std::size_t dag_expression_count{0};
  std::size_t dag_descriptor_count{0};
  std::size_t reachable_expression_count{0};
  std::size_t reachable_descriptor_count{0};
  std::size_t reachable_edge_count{0};
  std::size_t maximum_expression_depth{0};
  std::size_t row_slot_count{0};
  std::size_t unique_row_ordinal_count{0};

  std::uint64_t runtime_resident_structural_bytes{0};
  std::uint64_t prepare_row_binding_peak_structural_bytes{0};
  std::uint64_t active_row_binding_resident_bytes{0};
  std::uint64_t evaluation_peak_value_bytes{0};
  std::uint64_t evaluation_peak_transient_structural_bytes{0};
  std::uint64_t callback_handoff_peak_bytes{0};
  std::uint64_t core_expression_peak_bytes{0};

  CanonicalPredicateCallbackRequirements callbacks;
  std::string detail;
};

CanonicalPredicateLogicalMemoryBound
BoundCanonicalRowPredicateLogicalMemoryV1(
    const internal_api::TypedRelationalDag& dag,
    std::uint32_t root_expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<CanonicalPredicateRowValueEnvelope>& row_values,
    internal_api::EngineCanonicalExpressionConsumer consumer,
    const std::function<bool()>& abort_requested = {});

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

// Binds the engine-owned comparison result required by character and
// timezone-profile operands.  Core non-collated comparisons and SQL NULL do
// not require a precomputed result.
bool BindCanonicalRelationalComparisonAuthorityV1(
    const internal_api::EngineTypedValue& left,
    const internal_api::EngineTypedValue& right,
    const CanonicalRelationalExpressionRuntimeServices& services,
    std::optional<int>* precomputed_comparison,
    std::string* refusal_detail);

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

  // Infers or evaluates one canonical scalar expression against a completely
  // prepared materialized input row. Only the explicit row-slot binding may
  // supply identifier leaves; the ordinary typed runtime retains scalar,
  // NULL, comparison, and consumer semantics.
  bool InferTypeForConsumer(
      std::uint32_t expression_id,
      const CanonicalRelationalExpressionRowBinding& row_binding,
      const std::vector<internal_api::EngineTypedValue>& row_values,
      internal_api::EngineCanonicalExpressionConsumer consumer,
      std::string* canonical_type_name,
      std::string* refusal_detail);

  bool EvaluateForConsumer(
      std::uint32_t expression_id,
      std::string_view expected_type,
      const CanonicalRelationalExpressionRowBinding& row_binding,
      const std::vector<internal_api::EngineTypedValue>& row_values,
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
  // Only explicitly bound row-slot leaves may consume row values; the
  // surrounding literal/operator graph still uses the ordinary canonical
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
