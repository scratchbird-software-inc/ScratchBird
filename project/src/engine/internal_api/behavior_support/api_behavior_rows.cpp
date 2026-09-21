// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "behavior_support/api_behavior_store.hpp"

#include <type_traits>
#include <iterator>
#include <stdexcept>

namespace scratchbird::engine::internal_api {
void AddApiBehaviorEvidence(EngineApiResult* result, std::string kind, EngineEvidenceValue id) {
  if (!result) return;
  static_assert(std::is_nothrow_move_constructible_v<EngineEvidenceReference>);
  result->evidence.push_back({std::move(kind), std::move(id)});
}

void AppendApiEvidenceGroup(std::vector<EngineEvidenceReference>& destination,
                            std::span<const EngineEvidenceReference> source,
                            std::string_view evidence_namespace) {
  if (source.empty()) return;
  if (source.size() > destination.max_size() - destination.size())
    throw std::length_error("evidence group exceeds vector capacity");
  std::vector<EngineEvidenceReference> staged;
  staged.reserve(source.size());
  for (const auto& item : source) {
    std::string kind;
    if (!evidence_namespace.empty()) {
      kind.assign(evidence_namespace);
      kind.push_back('.');
    }
    kind.append(item.evidence_kind);
    // Perform fallible TEXT copying before constructing its variant. Moving
    // the completed value cannot leave a partially constructed alternative
    // during allocation-failure unwinding.
    auto value = std::visit([](const auto& source_value) -> EngineEvidenceValue {
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(source_value)>, std::string>) {
        std::string text = source_value;
        return EngineEvidenceValue{std::move(text)};
      } else {
        return EngineEvidenceValue{source_value};
      }
    }, item.evidence_id);
    staged.push_back({std::move(kind), std::move(value)});
  }
  // Stage before reserving destination: source and namespace can borrow its
  // storage. Publication only moves fully constructed nonthrowing values.
  static_assert(std::is_nothrow_move_constructible_v<EngineEvidenceReference>);
  static_assert(std::is_nothrow_move_assignable_v<EngineEvidenceReference>);
  destination.reserve(destination.size() + staged.size());
  destination.insert(destination.end(), std::make_move_iterator(staged.begin()),
                     std::make_move_iterator(staged.end()));
}

EngineTypedValue ApiBehaviorValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

EngineTypedValue ApiBehaviorValue(const EngineUuid& value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "uuid";
  // Value transport is not system-identity admission. User UUID data may have
  // any version; owners must validate system identities before calling us.
  typed.binary_value.assign(value.bytes.begin(), value.bytes.end());
  return typed;
}

EngineRowValue ApiBehaviorRow(ApiBehaviorFields fields) {
  EngineRowValue row;
  row.requested_row_uuid = GenerateCrudEngineUuid("row");
  row.fields.reserve(fields.size());
  for (auto& [name, value] : fields) {
    auto typed = std::visit([](auto&& source) -> EngineTypedValue {
      if constexpr (std::is_same_v<std::remove_cvref_t<decltype(source)>, EngineTypedValue>)
        return std::move(source);
      else
        return ApiBehaviorValue(std::move(source));
    }, std::move(value));
    row.fields.emplace_back(std::move(name), std::move(typed));
  }
  return row;
}

void AddApiBehaviorRow(EngineApiResult* result, ApiBehaviorFields fields) {
  if (!result) return;
  // Neither a partial row nor a new result kind survives an allocation fault.
  auto row = ApiBehaviorRow(std::move(fields));
  std::string kind = "api_behavior_rows";
  static_assert(std::is_nothrow_move_constructible_v<EngineRowValue>);
  result->result_shape.rows.push_back(std::move(row));
  result->result_shape.result_kind.swap(kind);
}
} // namespace scratchbird::engine::internal_api
