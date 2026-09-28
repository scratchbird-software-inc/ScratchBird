// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_engine_envelope.hpp"
#include "../../core/uuid/uuid.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <string_view>

namespace scratchbird::engine::sblr {
struct NativeRowFieldView {
  core::platform::Uuid row_uuid;
  std::string_view payload;
  bool is_null = false;
  bool allocate_row_identity = false;
};
inline bool IsNewNativeRowFieldType(std::string_view type) noexcept {
  return type.starts_with("row_new_field_binary16.") ||
         type.starts_with("row_new_null_field_binary16.");
}
inline bool IsNativeRowFieldType(std::string_view type) noexcept {
  return IsNewNativeRowFieldType(type) || type.starts_with("row_field_binary16.") ||
         type.starts_with("row_null_field_binary16.");
}
inline core::platform::Uuid RequestedNativeRowIdentity(const NativeRowFieldView& field) noexcept {
  return field.allocate_row_identity ? core::platform::Uuid{} : field.row_uuid;
}
inline std::optional<NativeRowFieldView> DecodeNativeRowField(const SblrOperand& operand) {
  const bool is_null = operand.type.starts_with("row_null_field_binary16.") ||
                       operand.type.starts_with("row_new_null_field_binary16.");
  if (!IsNativeRowFieldType(operand.type) ||
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
  result.allocate_row_identity = IsNewNativeRowFieldType(operand.type);
  if ((is_null && !result.payload.empty()) ||
      (!is_null && operand.type.ends_with(".uuid") && result.payload.size() != 16))
    return std::nullopt;
  return result;
}
// Validate the entire grouping before any API request is assembled. A UUID
// cannot mean both a requested durable identity and a fresh-row grouping key.
inline bool ValidateNativeRowFieldGroups(const SblrOperationEnvelope& envelope) {
  std::map<core::platform::Uuid, bool> modes;
  for (const auto& operand : envelope.operands) {
    if (!IsNativeRowFieldType(operand.type)) continue;
    const auto field = DecodeNativeRowField(operand);
    if (!field) return false;
    if (field->allocate_row_identity && envelope.operation_id != "dml.insert_rows" &&
        envelope.operation_id != "dml.execute_native_bulk_ingest" &&
        envelope.operation_id != "dml.execute_import_rows") return false;
    const auto [entry, inserted] = modes.emplace(field->row_uuid, field->allocate_row_identity);
    if (!inserted && entry->second != field->allocate_row_identity) return false;
  }
  return true;
}
} // namespace scratchbird::engine::sblr
