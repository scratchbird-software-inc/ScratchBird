// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_engine_envelope.hpp"
#include "../../core/uuid/uuid.hpp"
#include <algorithm>
#include <optional>
#include <string_view>

namespace scratchbird::engine::sblr {
struct NativeRowFieldView {
  core::platform::Uuid row_uuid;
  std::string_view payload;
  bool is_null = false;
};
inline std::optional<NativeRowFieldView> DecodeNativeRowField(const SblrOperand& operand) {
  const bool is_null = operand.type.starts_with("row_null_field_binary16.");
  if ((!is_null && !operand.type.starts_with("row_field_binary16.")) ||
      operand.name.empty() || !operand.value.empty() ||
      operand.value_kind != SblrValueKind::literal_typed || operand.value_body.size() < 40)
    return std::nullopt;
  std::uint64_t length = 0;
  for (unsigned byte = 0; byte < 8; ++byte)
    length |= static_cast<std::uint64_t>(operand.value_body[16 + byte]) << (8 * byte);
  if (length != operand.value_body.size() - 24) return std::nullopt;
  NativeRowFieldView result;
  std::copy_n(operand.value_body.begin() + 24, 16, result.row_uuid.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(result.row_uuid)) return std::nullopt;
  result.payload = {reinterpret_cast<const char*>(operand.value_body.data() + 40),
                    operand.value_body.size() - 40};
  result.is_null = is_null;
  if ((is_null && !result.payload.empty()) ||
      (!is_null && operand.type == "row_field_binary16.uuid" && result.payload.size() != 16))
    return std::nullopt;
  return result;
}
} // namespace scratchbird::engine::sblr
