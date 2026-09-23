// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_platform.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
namespace scratchbird::storage::database {
// Informational sidecar only. Durable MGA inventory owns drop finality.
struct NativeDropEvidence {
  core::platform::Uuid database_uuid, filespace_uuid, operation_uuid, actor_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint8_t mode = 0; // 1 logical, 2 quarantine, 3 physical delete
  friend bool operator==(const NativeDropEvidence&, const NativeDropEvidence&) = default;
};
using NativeDropEvidenceBytes = std::array<std::uint8_t, 81>;
inline NativeDropEvidenceBytes EncodeNativeDropEvidence(const NativeDropEvidence& value) {
  NativeDropEvidenceBytes bytes{};
  constexpr std::array<std::uint8_t, 8> magic{'S','B','D','R','O','P','0','2'};
  std::copy(magic.begin(), magic.end(), bytes.begin());
  std::size_t offset = 8;
  for (const auto* id : {&value.database_uuid, &value.filespace_uuid,
                          &value.operation_uuid, &value.actor_uuid}) {
    std::copy(id->bytes.begin(), id->bytes.end(), bytes.begin() + offset);
    offset += 16;
  }
  for (unsigned i = 0; i < 8; ++i)
    bytes[72 + i] = static_cast<std::uint8_t>(value.local_transaction_id >> (8 * i));
  bytes[80] = value.mode;
  return bytes;
}
inline std::optional<NativeDropEvidence> DecodeNativeDropEvidence(
    std::span<const std::uint8_t> bytes) {
  constexpr std::array<std::uint8_t, 8> magic{'S','B','D','R','O','P','0','2'};
  if (bytes.size() != 81 || !std::equal(magic.begin(), magic.end(), bytes.begin()) ||
      bytes[80] < 1 || bytes[80] > 3) return std::nullopt;
  NativeDropEvidence value;
  std::size_t offset = 8;
  for (auto* id : {&value.database_uuid, &value.filespace_uuid,
                   &value.operation_uuid, &value.actor_uuid}) {
    std::copy_n(bytes.begin() + offset, 16, id->bytes.begin());
    offset += 16;
  }
  for (unsigned i = 0; i < 8; ++i)
    value.local_transaction_id |= static_cast<std::uint64_t>(bytes[72 + i]) << (8 * i);
  value.mode = bytes[80];
  if (value.database_uuid.is_nil() || value.filespace_uuid.is_nil() ||
      value.local_transaction_id == 0) return std::nullopt;
  return value;
}
} // namespace scratchbird::storage::database
