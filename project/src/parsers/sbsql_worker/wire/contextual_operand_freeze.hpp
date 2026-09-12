// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "lowering/lowering.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"

#include <limits>
#include <unordered_set>

namespace scratchbird::parser::sbsql {

// Local immutable reservation material, never execution or receipt authority.
// A return value is published only after the entire ordered projection passes.
inline std::optional<std::vector<std::uint8_t>> FreezeContextualOperandsV3(
    const std::vector<SblrOperand>& operands,
    const std::unordered_set<std::uint32_t>& mutable_descriptor_handles) {
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  constexpr auto descriptor_kind = static_cast<std::uint16_t>(
      engine::sblr::SblrValueKind::relational_type_descriptor);
  if (operands.size() > maximum || mutable_descriptor_handles.contains(0))
    return std::nullopt;
  std::vector<std::uint8_t> frozen;
  const auto integer = [&](std::uint64_t value, unsigned width) {
    for (unsigned byte = 0; byte < width; ++byte)
      frozen.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
  };
  const auto text = [&](const std::string& value) {
    integer(value.size(), 4);
    frozen.insert(frozen.end(), value.begin(), value.end());
  };
  integer(3, 2);
  integer(operands.size(), 4);
  std::unordered_set<std::uint32_t> seen_descriptors;
  std::size_t mutable_count = 0;
  for (const auto& operand : operands) {
    if (operand.type.size() > maximum || operand.name.size() > maximum ||
        operand.value.size() > maximum ||
        operand.canonical_value_body.size() > maximum ||
        operand.type == "relational_descriptor_v1" ||
        operand.type == "relational_descriptor_v2" ||
        ((operand.canonical_value_kind != 0 ||
          !operand.canonical_value_body.empty()) && !operand.value.empty()))
      return std::nullopt;
    if (operand.type == "relational_descriptor_v3" ||
        operand.canonical_value_kind == descriptor_kind) {
      engine::internal_api::RelationalTypeDescriptor descriptor;
      if (operand.type != "relational_descriptor_v3" ||
          operand.canonical_value_kind != descriptor_kind ||
          !engine::sblr::DecodeRelationalTypeDescriptorV1(
              operand.canonical_value_body.data(),
              operand.canonical_value_body.size(), &descriptor) ||
          operand.name != "slot_" + std::to_string(descriptor.descriptor_id) ||
          !seen_descriptors.insert(descriptor.descriptor_id).second)
        return std::nullopt;
      if (mutable_descriptor_handles.contains(descriptor.descriptor_id)) {
        integer(1, 1);
        integer(descriptor.descriptor_id, 4);
        ++mutable_count;
        continue;
      }
    }
    integer(0, 1);
    text(operand.type);
    text(operand.name);
    text(operand.value);
    integer(operand.canonical_value_kind, 2);
    integer(operand.canonical_value_body.size(), 4);
    frozen.insert(frozen.end(), operand.canonical_value_body.begin(),
                  operand.canonical_value_body.end());
  }
  if (mutable_count != mutable_descriptor_handles.size()) return std::nullopt;
  return frozen;
}

}  // namespace scratchbird::parser::sbsql
