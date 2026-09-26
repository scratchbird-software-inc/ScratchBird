// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "wire/parser_server_ipc/parser_server_client.hpp"

#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql::wire_detail {

// Core 06: domain + count:u32le + dense, 48-byte SBPSR2 slot records.
// This encodes negotiated control identities, never user UUID values.
inline std::optional<std::vector<std::uint8_t>> ParameterSlotTableBytesV2(
    const std::vector<ipc::PreparedParameterSlotReference>& slots) {
  if (slots.empty() || slots.size() > 4096) return std::nullopt;
  constexpr std::string_view domain = "ScratchBird.SblrParameterSlots.V2";
  std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
  bytes.reserve(domain.size() + 4 + slots.size() * 48);
  const auto append_le = [&bytes](std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
      bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
  };
  const auto native = [](const std::array<std::uint8_t, 16>& id) {
    return core::uuid::IsEngineIdentityUuid(core::platform::Uuid{id});
  };
  std::set<std::array<std::uint8_t, 16>> identities;
  append_le(slots.size(), 4);
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.slot_ordinal != i || !native(slot.slot_uuid) ||
        !identities.insert(slot.slot_uuid).second ||
        !native(slot.datatype_descriptor_uuid) ||
        !native(slot.datatype_type_uuid) ||
        slot.datatype_descriptor_generation == 0 ||
        slot.direction < 1 || slot.direction > 3 || slot.nullable > 1)
      return std::nullopt;
    append_le(slot.slot_ordinal, 4);
    bytes.insert(bytes.end(), slot.slot_uuid.begin(), slot.slot_uuid.end());
    bytes.insert(bytes.end(), slot.datatype_descriptor_uuid.begin(),
                 slot.datatype_descriptor_uuid.end());
    append_le(slot.datatype_descriptor_generation, 8);
    bytes.push_back(slot.direction);
    bytes.push_back(slot.nullable);
    bytes.push_back(0);  // Unbound slot state.
    bytes.push_back(0);  // Reserved.
  }
  return bytes;
}

}  // namespace scratchbird::parser::sbsql::wire_detail
