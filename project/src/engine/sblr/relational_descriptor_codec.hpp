// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/internal_api/query/relational_type_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace scratchbird::engine::sblr {

inline constexpr std::size_t kRelationalDescriptorFixedBytesV1 = 152;
inline constexpr std::size_t kRelationalDescriptorMaximumBytesV1 = 1432;

// Pure structural transport. Authority still comes from the owning live
// receipt and datatype registry. Failure leaves the caller's output unchanged;
// allocation failure propagates, never becoming successful partial output.
bool ValidateRelationalTypeDescriptorV1(
    const internal_api::RelationalTypeDescriptor& value) noexcept;
bool EncodeRelationalTypeDescriptorV1(
    const internal_api::RelationalTypeDescriptor& value,
    std::vector<std::uint8_t>* output);
bool DecodeRelationalTypeDescriptorV1(
    const std::uint8_t* data, std::size_t size,
    internal_api::RelationalTypeDescriptor* output);

}  // namespace scratchbird::engine::sblr
