// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_predicate_support.hpp"

#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_PREDICATE_SUPPORT_AUTHORITY
CanonicalPredicateScratchBound BoundCanonicalPredicateScratchBytes(
    const api::TypedRelationalDag& dag, const std::uint32_t root_expression_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::uint64_t pair_payload_bytes,
    const std::function<bool()>& abort_requested) {
  CanonicalPredicateScratchBound result;
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& candidate) {
      return candidate.expression_id == expression_id;
    });
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& candidate) {
      return candidate.descriptor_id == descriptor_id;
    });
  };
  const auto ascii_equal = [](const std::string_view value,
                              const std::string_view uppercase) {
    if (value.size() != uppercase.size()) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (static_cast<char>(std::toupper(
              static_cast<unsigned char>(value[index]))) != uppercase[index]) {
        return false;
      }
    }
    return true;
  };
  struct EvaluationBound {
    std::uint64_t output_payload_bytes = 0;
    std::uint64_t peak_scratch_bytes = 0;
  };
  std::size_t visited_expression_count = 0;
  std::size_t maximum_expression_visits = 0;
  if (dag.expressions.empty() ||
      dag.expressions.size() >
          std::numeric_limits<std::size_t>::max() / dag.expressions.size()) {
    result.detail = "join predicate expression visit bound overflowed";
    return result;
  }
  maximum_expression_visits = dag.expressions.size() * dag.expressions.size();
  const auto bound_expression = [&](auto&& self,
                                    const std::uint32_t expression_id,
                                    EvaluationBound* bound,
                                    const std::size_t depth) -> bool {
    if (abort_requested && abort_requested()) {
      result.cancelled = true;
      return false;
    }
    if (depth > dag.expressions.size() ||
        ++visited_expression_count > maximum_expression_visits) {
      result.detail =
          "join predicate expression graph is cyclic or exceeds its finite "
          "visit ceiling";
      return false;
    }
    const auto expression = expression_for(expression_id);
    if (expression == dag.expressions.end()) {
      result.detail = "join predicate references an absent expression";
      return false;
    }
    const auto& record = *expression;
    const auto descriptor = descriptor_for(record.result_descriptor_id);
    if (descriptor == dag.descriptors.end()) {
      result.detail = "join predicate result descriptor is absent";
      return false;
    }
    const auto& type = *descriptor;
    const std::uint64_t declared_extent =
        static_cast<std::uint64_t>(type.width.value_or(0)) +
        static_cast<std::uint64_t>(type.precision.value_or(0)) +
        static_cast<std::uint64_t>(type.scale.value_or(0)) + 64;
    EvaluationBound computed;
    const auto finish_leaf = [&](const std::uint64_t payload) {
      computed.output_payload_bytes = payload;
      computed.peak_scratch_bytes = payload;
      return true;
    };
    const auto finish_unary = [&](const EvaluationBound& child,
                                  const std::uint64_t output,
                                  const std::uint64_t copies) {
      std::uint64_t frame_payload = 0;
      if (!CheckedAdd(child.output_payload_bytes, output, &frame_payload) ||
          !CheckedMultiply(frame_payload, copies, &frame_payload)) {
        return false;
      }
      computed.output_payload_bytes = output;
      computed.peak_scratch_bytes =
          std::max(child.peak_scratch_bytes, frame_payload);
      return true;
    };
    const auto finish_binary = [&](const EvaluationBound& left,
                                   const EvaluationBound& right,
                                   const std::uint64_t output,
                                   const std::uint64_t copies) {
      std::uint64_t right_with_left = 0;
      std::uint64_t frame_payload = 0;
      if (!CheckedAdd(left.output_payload_bytes, right.peak_scratch_bytes,
                      &right_with_left) ||
          !CheckedAdd(left.output_payload_bytes, right.output_payload_bytes,
                      &frame_payload) ||
          !CheckedAdd(frame_payload, output, &frame_payload) ||
          !CheckedMultiply(frame_payload, copies, &frame_payload)) {
        return false;
      }
      computed.output_payload_bytes = output;
      computed.peak_scratch_bytes =
          std::max({left.peak_scratch_bytes, right_with_left, frame_payload});
      return true;
    };

    const auto materialized_slot = std::ranges::find_if(
        row_binding.slots, [&](const auto& slot) {
          return slot.expression_id == record.expression_id;
        });
    if (materialized_slot != row_binding.slots.end()) {
      const bool duplicate_slot =
          std::ranges::find_if(
              std::next(materialized_slot), row_binding.slots.end(),
              [&](const auto& slot) {
                return slot.expression_id == record.expression_id;
              }) != row_binding.slots.end();
      const bool exact_materialized_function =
          materialized_slot->slot_kind ==
              CanonicalRelationalExpressionRowSlotKind::materialized_function &&
          record.expression_kind ==
              api::RelationalExpressionKind::kFunctionCall &&
          record.function_uuid.has_value() &&
          CanonicalUuidText(*record.function_uuid) &&
          !record.bound_name_uuid.has_value() &&
          !record.literal_kind.has_value() &&
          !record.operator_name.has_value() &&
          !record.literal_or_parameter_ref.has_value();
      const bool exact_input_identifier =
          materialized_slot->slot_kind ==
              CanonicalRelationalExpressionRowSlotKind::input_identifier &&
          record.expression_kind ==
              api::RelationalExpressionKind::kIdentifier &&
          record.child_expression_ids.empty() &&
          record.bound_name_uuid.has_value() &&
          CanonicalUuidText(*record.bound_name_uuid) &&
          !record.function_uuid.has_value() &&
          !record.literal_kind.has_value() &&
          !record.operator_name.has_value() &&
          !record.literal_or_parameter_ref.has_value();
      std::uint64_t materialized_payload = 0;
      if (duplicate_slot || materialized_slot->descriptor_id !=
                                record.result_descriptor_id ||
          materialized_slot->row_ordinal >=
              row_binding.row_descriptor_ids.size() ||
          row_binding.row_descriptor_ids[materialized_slot->row_ordinal] !=
              materialized_slot->descriptor_id ||
          (!exact_materialized_function && !exact_input_identifier) ||
          !CheckedAdd(pair_payload_bytes, declared_extent,
                      &materialized_payload) ||
          !finish_leaf(materialized_payload)) {
        result.detail =
            "join predicate materialized row slot is not exactly bounded";
        return false;
      }
      *bound = computed;
      return true;
    }

    bool bounded = false;
    switch (record.expression_kind) {
      case api::RelationalExpressionKind::kLiteral: {
        if (!record.child_expression_ids.empty()) break;
        std::uint64_t literal_payload = declared_extent;
        if (record.literal_or_parameter_ref.has_value() &&
            !CheckedAdd(literal_payload,
                        record.literal_or_parameter_ref->size(),
                        &literal_payload)) {
          break;
        }
        bounded = finish_leaf(literal_payload);
        break;
      }
      case api::RelationalExpressionKind::kIdentifier: {
        if (!record.child_expression_ids.empty()) break;
        std::uint64_t identifier_payload = 0;
        bounded = CheckedAdd(pair_payload_bytes, declared_extent,
                             &identifier_payload) &&
                  finish_leaf(identifier_payload);
        break;
      }
      case api::RelationalExpressionKind::kParenthesized: {
        if (record.child_expression_ids.size() != 1) break;
        EvaluationBound child;
        std::uint64_t output = 0;
        bounded = self(self, record.child_expression_ids.front(), &child,
                       depth + 1) &&
                  CheckedAdd(child.output_payload_bytes, declared_extent,
                             &output) &&
                  finish_unary(child, output, 3);
        break;
      }
      case api::RelationalExpressionKind::kUnary: {
        const std::string_view operation =
            record.operator_name.has_value()
                ? std::string_view(*record.operator_name)
                : std::string_view{};
        if (record.child_expression_ids.size() != 1 ||
            (!ascii_equal(operation, "+") &&
             !ascii_equal(operation, "-") &&
             !ascii_equal(operation, "NOT"))) {
          break;
        }
        EvaluationBound child;
        std::uint64_t output = 0;
        bounded = self(self, record.child_expression_ids.front(), &child,
                       depth + 1) &&
                  CheckedAdd(child.output_payload_bytes, declared_extent,
                             &output) &&
                  finish_unary(child, output, 4);
        break;
      }
      case api::RelationalExpressionKind::kBinary: {
        const std::string_view operation =
            record.operator_name.has_value()
                ? std::string_view(*record.operator_name)
                : std::string_view{};
        const bool arithmetic =
            ascii_equal(operation, "+") || ascii_equal(operation, "-") ||
            ascii_equal(operation, "*") || ascii_equal(operation, "/") ||
            ascii_equal(operation, "%");
        const bool logical = ascii_equal(operation, "AND") ||
                             ascii_equal(operation, "OR") ||
                             ascii_equal(operation, "XOR");
        const bool text_concat = ascii_equal(operation, "||");
        const bool text_match = ascii_equal(operation, "LIKE") ||
                                ascii_equal(operation, "ILIKE");
        const bool comparison =
            ascii_equal(operation, "IS") || ascii_equal(operation, "=") ||
            ascii_equal(operation, "<>") || ascii_equal(operation, "!=") ||
            ascii_equal(operation, "<") || ascii_equal(operation, "<=") ||
            ascii_equal(operation, ">") || ascii_equal(operation, ">=") ||
            ascii_equal(operation, "IS DISTINCT FROM") ||
            ascii_equal(operation, "IS NOT DISTINCT FROM");
        if (record.child_expression_ids.size() != 2 ||
            (!arithmetic && !logical && !text_concat && !text_match &&
             !comparison)) {
          break;
        }
        EvaluationBound left;
        EvaluationBound right;
        std::uint64_t child_payload = 0;
        std::uint64_t output = 0;
        if (!self(self, record.child_expression_ids[0], &left, depth + 1) ||
            !self(self, record.child_expression_ids[1], &right, depth + 1) ||
            !CheckedAdd(left.output_payload_bytes,
                        right.output_payload_bytes, &child_payload)) {
          break;
        }
        if (logical || text_match || comparison) {
          bounded = CheckedAdd(8, declared_extent, &output) &&
                    finish_binary(left, right, output,
                                  text_match ? 6 : 4);
        } else {
          bounded = CheckedAdd(child_payload, declared_extent, &output) &&
                    finish_binary(left, right, output, 4);
        }
        break;
      }
      case api::RelationalExpressionKind::kFunctionCall: {
        // POINT is the sole functionless built-in in this runtime and has a
        // fixed 24-byte SBP1 result. External function callbacks have no payload
        // ceiling in this ABI and therefore remain fail-closed here.
        if (record.function_uuid.has_value() ||
            record.operator_name != "POINT" ||
            record.child_expression_ids.size() != 2) {
          break;
        }
        EvaluationBound left;
        EvaluationBound right;
        std::uint64_t output = 0;
        bounded = self(self, record.child_expression_ids[0], &left,
                       depth + 1) &&
                  self(self, record.child_expression_ids[1], &right,
                       depth + 1) &&
                  CheckedAdd(24, declared_extent, &output) &&
                  finish_binary(left, right, output, 4);
        break;
      }
      case api::RelationalExpressionKind::kParameter: {
        if (!record.child_expression_ids.empty() ||
            !record.parameter_typed_value_v1.has_value() ||
            record.literal_or_parameter_ref.has_value() ||
            record.literal_typed_value_v1.has_value() ||
            record.function_uuid.has_value() ||
            record.bound_name_uuid.has_value() ||
            record.literal_kind.has_value() ||
            record.operator_name.has_value()) {
          break;
        }
        const auto& typed = *record.parameter_typed_value_v1;
        const bool null_value = typed.value_state == "null";
        const bool materialized_value = typed.value_state == "value";
        if ((!null_value && !materialized_value) ||
            typed.descriptor_generation == 0 ||
            typed.descriptor_uuid != type.descriptor_uuid ||
            (null_value && !typed.canonical_value_bytes.empty()) ||
            (materialized_value &&
             typed.canonical_value_bytes.size() != 8)) {
          break;
        }
        std::uint64_t parameter_payload = declared_extent;
        std::uint64_t typed_metadata_bytes = 0;
        bounded = CheckedAdd(
                      parameter_payload,
                      static_cast<std::uint64_t>(
                          typed.canonical_value_bytes.size()),
                      &parameter_payload) &&
                  CheckedAdd(
                      static_cast<std::uint64_t>(typed.descriptor_uuid.size()),
                      static_cast<std::uint64_t>(typed.value_state.size()),
                      &typed_metadata_bytes) &&
                  CheckedAdd(typed_metadata_bytes, 2,
                             &typed_metadata_bytes) &&
                  CheckedAdd(parameter_payload, typed_metadata_bytes,
                             &parameter_payload) &&
                  finish_leaf(parameter_payload);
        break;
      }
    }
    if (!bounded) {
      if (result.detail.empty()) {
        result.detail =
            "join predicate is outside the finite payload-bounded profile "
            "or its multiplicity-aware bound overflowed";
      }
      return false;
    }
    *bound = computed;
    return true;
  };

  EvaluationBound root_bound;
  if (!bound_expression(bound_expression, root_expression_id, &root_bound,
                        1)) {
    return result;
  }
  result.maximum_payload_bytes = root_bound.peak_scratch_bytes;
  std::uint64_t structural_bytes =
      sizeof(CanonicalRelationalExpressionRuntime) +
      8 * sizeof(void*);
  std::uint64_t entry_bytes = 0;
  std::uint64_t reachable_edges = 1;
  for (const auto& expression : dag.expressions) {
    if (!CheckedAdd(reachable_edges, expression.child_expression_ids.size(),
                    &reachable_edges)) {
      result.detail = "join predicate structural edge bound overflowed";
      return result;
    }
  }
  const auto add_entries = [&](const std::uint64_t count,
                               const std::uint64_t bytes_per_entry) {
    return CheckedMultiply(count, bytes_per_entry, &entry_bytes) &&
           CheckedAdd(structural_bytes, entry_bytes, &structural_bytes);
  };
  // Conservative node-plus-bucket envelopes for the runtime descriptor,
  // expression, inferred-type, inference-stack, row-binding, reachability,
  // and pending-work containers. The row values themselves are carried by
  // pair_payload_bytes above.
  if (!add_entries(dag.expressions.size(),
                   sizeof(std::pair<const std::uint32_t,
                                    const api::RelationalExpressionRecord*>) +
                       5 * sizeof(void*)) ||
      !add_entries(dag.descriptors.size(),
                   sizeof(std::pair<const std::uint32_t,
                                    const api::RelationalTypeDescriptor*>) +
                       sizeof(std::pair<const std::uint32_t, std::string>) +
                       8 * sizeof(void*) + 129) ||
      !add_entries(dag.expressions.size(),
                   2 * sizeof(std::uint32_t) + 6 * sizeof(void*)) ||
      !add_entries(row_binding.row_descriptor_ids.size(),
                   sizeof(std::uint32_t) + 3 * sizeof(void*)) ||
      !add_entries((row_binding.row_nullable.size() + 63) / 64,
                   sizeof(std::uint64_t)) ||
      !add_entries(row_binding.slots.size(),
                   sizeof(CanonicalRelationalExpressionRowSlotBinding) +
                       sizeof(std::pair<const std::uint32_t,
                                        const api::EngineTypedValue*>) +
                       sizeof(std::pair<
                           const std::size_t,
                           CanonicalRelationalExpressionRowSlotKind>) +
                       12 * sizeof(void*)) ||
      !add_entries(reachable_edges,
                   sizeof(std::uint32_t) + sizeof(void*))) {
    result.detail = "join predicate structural memory bound overflowed";
    return result;
  }
  result.maximum_structural_bytes = structural_bytes;
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::sblr

