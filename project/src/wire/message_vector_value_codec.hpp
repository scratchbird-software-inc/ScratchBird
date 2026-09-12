// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::wire::message_vector {

// Core MESSAGE_VECTOR_REGISTERED_VALUES_V1. These are transport types, not
// datatype descriptors or executable values. Descriptor-owned bytes are
// validated against their actual descriptor by the bridge before crossing IPC.
enum class ValueType : std::uint16_t {
  utf8=1, boolean=2, signed64=3, unsigned64=4, system_uuid=5,
  octets=6, null_value=7, typed_value=8,
  parameter=256, detail=257, cause=258, context=259
};
enum class ValueCodecError { none, malformed, allocation_failure };
struct ValueTlv {
  std::string key;
  ValueType type=ValueType::null_value;
  std::vector<std::uint8_t> value;
  bool operator==(const ValueTlv&) const = default;
};

// Exact one-entry codecs: reject trailing bytes. Never mutate output on failure.
// These validate value structure, not template registration, channel rights,
// section position, active generations, or the surrounding SBMV record/set.
[[nodiscard]] ValueCodecError EncodeValueTlv(
    const ValueTlv& input, std::vector<std::uint8_t>* output) noexcept;
[[nodiscard]] ValueCodecError DecodeValueTlv(
    std::span<const std::uint8_t> input, ValueTlv* output) noexcept;
[[nodiscard]] bool IsRegisteredSourceComponent(std::uint32_t code) noexcept;
[[nodiscard]] bool IsMessageVectorUtf8(std::span<const std::uint8_t> value) noexcept;
[[nodiscard]] bool IsValidValueTlv(const ValueTlv& value) noexcept;

} // namespace scratchbird::wire::message_vector
