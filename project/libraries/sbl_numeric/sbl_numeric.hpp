// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SBL_NUMERIC_MANDATORY_BACKEND_PUBLIC_API

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::libraries::sbl_numeric {

enum class NumericType : std::uint16_t {
  decimal = 0,
  decimal_float = 1,
  real128 = 2,
  int128 = 3,
  uint128 = 4
};

enum class NumericOperation : std::uint16_t {
  canonicalize,
  add,
  subtract,
  multiply,
  divide,
  compare
};

enum class RoundingMode : std::uint16_t {
  half_even,
  half_up,
  truncate
};

enum class NumericStatusCode : std::uint16_t {
  ok,
  null_result,
  invalid_context,
  invalid_left,
  invalid_right,
  invalid_operation,
  divide_by_zero,
  overflow,
  unordered,
  backend_unavailable
};

struct NumericContext {
  std::uint32_t precision = 38;
  std::uint32_t scale = 0;
  RoundingMode rounding = RoundingMode::half_even;
  bool allow_special_values = false;
  bool canonical_preserve_scale = false;
};

struct NumericValue {
  NumericType type = NumericType::decimal;
  std::string encoded;
  bool is_null = false;
};

struct NumericRequest {
  NumericOperation operation = NumericOperation::canonicalize;
  NumericType type = NumericType::decimal;
  NumericValue left;
  NumericValue right;
  NumericContext context;
};

struct NumericResult {
  NumericStatusCode status = NumericStatusCode::ok;
  NumericValue value;
  int comparison = 0;
  std::string diagnostic_code;
};

const char* NumericStatusCodeName(NumericStatusCode status);
const char* NumericTypeName(NumericType type);
const char* NumericOperationName(NumericOperation operation);
// Returns the exact real128 implementation compiled into this library.  The
// runtime capability manifest must report the same value.
const char* Real128BackendName();
NumericResult ApplyNumericOperation(const NumericRequest& request);
// Canonical signed two's-complement storage payload; no host encoding accepted.
NumericResult DecodeInt128LittleEndian(const std::vector<std::uint8_t>& payload);
inline constexpr std::size_t kExactDecimalBinaryBytes = 24;
struct ExactDecimalBinaryResult {
  bool ok = false;
  bool overflow = false;
  std::string detail;
  std::uint8_t precision = 0;
  std::uint8_t scale = 0;
  std::string canonical_lexical;
  std::array<std::uint8_t, kExactDecimalBinaryBytes> canonical_bytes{};
};
// Shared exact decimal VALUE codec. Numeric values only: no SQL suffix,
// delimiter, descriptor inference, floating-point conversion or host layout.
ExactDecimalBinaryResult EncodeExactDecimalLittleEndian(std::string_view value);
ExactDecimalBinaryResult DecodeExactDecimalLittleEndian(
    const std::uint8_t* bytes, std::size_t size);

struct NumericBinaryResult {
  NumericStatusCode status = NumericStatusCode::invalid_left;
  std::vector<std::uint8_t> payload;
  std::string diagnostic_code;
};
// Exact canonical integer spelling to signed two's-complement little endian.
// Rejects whitespace, plus, leading zeroes, negative zero and out-of-range
// values. Uses the same portable arithmetic and range authority as decoding.
NumericBinaryResult EncodeInt128LittleEndian(std::string_view canonical);

}  // namespace scratchbird::libraries::sbl_numeric
