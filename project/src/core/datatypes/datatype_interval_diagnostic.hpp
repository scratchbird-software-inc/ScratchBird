// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../platform/runtime_platform.hpp"
#include <array>
#include <cstdint>
#include <string_view>

namespace scratchbird::core::datatypes {
enum class IntervalDiagnosticParameterKindV3 : platform::u8 {
  none = 0, unsigned_u64 = 1, signed_i64 = 2, uuid = 3, token = 4,
};
struct IntervalDiagnosticParameterV3 {
  IntervalDiagnosticParameterKindV3 kind = IntervalDiagnosticParameterKindV3::none;
  std::string_view name;
  platform::u64 unsigned_value = 0;
  std::int64_t signed_value = 0;
  platform::Uuid uuid_value;
  std::string_view token_value;
};
inline constexpr platform::u8 kIntervalDiagnosticParameterCapacityV3 = 5;
struct IntervalDiagnosticFactV3 {
  platform::Status status;
  std::string_view diagnostic_code;
  std::string_view detail;
  std::array<IntervalDiagnosticParameterV3, kIntervalDiagnosticParameterCapacityV3> parameters{};
  platform::u8 parameter_count = 0;
};
}  // namespace scratchbird::core::datatypes
