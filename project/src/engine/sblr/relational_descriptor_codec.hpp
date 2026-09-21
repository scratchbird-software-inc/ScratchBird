// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "engine/internal_api/query/relational_type_descriptor.hpp"
#include "engine/internal_api/query/relational_expression_record.hpp"
#include "engine/internal_api/query/relational_window_invocation_record.hpp"
#include "engine/internal_api/query/relational_window_definition_record.hpp"
#include "engine/internal_api/query/relational_property_record.hpp"
#include "engine/internal_api/query/relational_row_pattern_record.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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

inline constexpr std::size_t kRelationalExpressionFixedBytesV1 = 60;
inline constexpr std::size_t kRelationalExpressionMaximumBytesV1 = 65536;
bool ValidateRelationalExpressionV1(const internal_api::RelationalExpressionRecord& value) noexcept;
bool EncodeRelationalExpressionV1(const internal_api::RelationalExpressionRecord& value,
                                  std::vector<std::uint8_t>* output);
bool DecodeRelationalExpressionV1(const std::uint8_t* data, std::size_t size,
                                  internal_api::RelationalExpressionRecord* output);

struct RelationalNodeBindingRecord {
  std::uint32_t node_id{0};
  std::string semantic_variant_id;
  std::vector<std::uint32_t> bound_expression_ids;
  std::vector<core::platform::Uuid> required_object_uuids;
  std::vector<core::platform::Uuid> required_property_uuids;
  std::vector<core::platform::Uuid> delivered_property_uuids;
};
inline constexpr std::size_t kRelationalNodeBindingFixedBytesV1 = 32;
inline constexpr std::size_t kRelationalNodeBindingMaximumBytesV1 = 65536;
bool ValidateRelationalNodeBindingV1(const RelationalNodeBindingRecord& value) noexcept;
bool EncodeRelationalNodeBindingV1(const RelationalNodeBindingRecord& value,
                                   std::vector<std::uint8_t>* output);
bool DecodeRelationalNodeBindingV1(const std::uint8_t* data, std::size_t size,
                                   RelationalNodeBindingRecord* output);

inline constexpr std::size_t kRelationalWindowInvocationFixedBytesV1 = 56;
inline constexpr std::size_t kRelationalWindowInvocationMaximumBytesV1 = 65536;
bool ValidateRelationalWindowInvocationV1(const internal_api::RelationalWindowInvocationRecord& value) noexcept;
bool EncodeRelationalWindowInvocationV1(const internal_api::RelationalWindowInvocationRecord& value,
                                        std::vector<std::uint8_t>* output);
bool DecodeRelationalWindowInvocationV1(const std::uint8_t* data, std::size_t size,
                                        internal_api::RelationalWindowInvocationRecord* output);

inline constexpr std::size_t kRelationalWindowDefinitionFixedBytesV1 = 48;
inline constexpr std::size_t kRelationalWindowDefinitionMaximumBytesV1 = 65536;
bool ValidateRelationalWindowDefinitionV1(const internal_api::RelationalWindowDefinitionRecord& value) noexcept;
bool EncodeRelationalWindowDefinitionV1(const internal_api::RelationalWindowDefinitionRecord& value,
                                        std::vector<std::uint8_t>* output);
bool DecodeRelationalWindowDefinitionV1(const std::uint8_t* data, std::size_t size,
                                        internal_api::RelationalWindowDefinitionRecord* output);

inline constexpr std::size_t kRelationalPropertyFixedBytesV1 = 104;
inline constexpr std::size_t kRelationalPropertyMaximumBytesV1 = 65536;
bool ValidateRelationalPropertyV1(const internal_api::RelationalPropertyRecord& value) noexcept;
bool EncodeRelationalPropertyV1(const internal_api::RelationalPropertyRecord& value,
                                std::vector<std::uint8_t>* output);
bool DecodeRelationalPropertyV1(const std::uint8_t* data, std::size_t size,
                                internal_api::RelationalPropertyRecord* output);

inline constexpr std::size_t kRelationalRowPatternFixedBytesV1 = 48;
inline constexpr std::size_t kRelationalRowPatternMaximumBytesV1 = 65536;
bool ValidateRelationalRowPatternV1(const internal_api::RelationalRowPatternRecord& value) noexcept;
bool EncodeRelationalRowPatternV1(const internal_api::RelationalRowPatternRecord& value,
                                  std::vector<std::uint8_t>* output);
bool DecodeRelationalRowPatternV1(const std::uint8_t* data, std::size_t size,
                                  internal_api::RelationalRowPatternRecord* output);

}  // namespace scratchbird::engine::sblr
