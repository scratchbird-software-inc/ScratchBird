// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "crud_support/retained_row_value.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {
// Pure scalar byte projection for direct row and native packet consumers.
// No catalog resolution, admission, publication, or transaction authority.
std::uint64_t DirectReadLittleEndianU64(std::span<const std::uint8_t> payload);
std::uint32_t DirectReadLittleEndianU32(std::span<const std::uint8_t> payload);
std::string DirectI64ToString(std::int64_t value);
std::string DirectU64ToString(std::uint64_t value);
std::string DirectReal64ToString(double value);
std::string DirectTypedValueTextPayload(const EngineTypedValue& typed);
CrudStoredValue DirectTypedStoredValue(const EngineTypedValue& typed);
EngineApiU64 DirectTypedValuePayloadSize(const EngineTypedValue& typed);
std::string DirectNativePacketTypeName(std::uint8_t tag);
std::vector<core::platform::byte> DirectBigEndianBytes(const std::vector<core::platform::byte>& little_endian);
std::vector<core::platform::byte> DirectLittleEndianBytes32(std::uint32_t value);
std::vector<core::platform::byte> DirectLittleEndianBytes64(std::uint64_t value);
}  // namespace scratchbird::engine::internal_api::dml::detail
