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
#include <optional>
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
  // Numeric facts, not success/finality authority. Underflow denotes tiny AND
  // inexact after rounding; exact subnormals have only the subnormal flag.
  bool inexact = false;
  bool underflow = false;
  bool overflow = false;
  bool invalid = false;
  bool divide_by_zero = false;
  bool subnormal = false;
};

const char* NumericStatusCodeName(NumericStatusCode status);
const char* NumericTypeName(NumericType type);
const char* NumericOperationName(NumericOperation operation);
// Returns the exact real128 implementation compiled into this library.  The
// runtime capability manifest must report the same value.
const char* Real128BackendName();
bool Real128BackendAvailable() noexcept;
// Runtime thread owner calls this only after its MPFR work has quiesced.
void ReleaseReal128ThreadCache() noexcept;
NumericResult ApplyNumericOperation(const NumericRequest& request);
using Real128Bytes = std::array<std::uint8_t, 16>;
struct Real128BinaryResult {
  NumericResult numeric;
  // Present only for a successful numeric value; compare returns only the
  // comparison outcome. An error is never represented by zero-filled bytes.
  std::optional<Real128Bytes> bytes;
};
struct Real128BinaryRequest {
  NumericOperation operation = NumericOperation::canonicalize;
  std::optional<Real128Bytes> left;
  std::optional<Real128Bytes> right;
  NumericContext context;
};
// Value codecs only. SQL NULL and descriptor/resource authority are carried by
// the caller's typed envelope, not by an IEEE bit pattern or this context.
Real128BinaryResult EncodeReal128LittleEndian(
    std::string_view text, const NumericContext& context = {});
Real128BinaryResult DecodeReal128LittleEndian(
    const std::uint8_t* bytes, std::size_t size, const NumericContext& context = {},
    bool render_canonical_text = false);
Real128BinaryResult ApplyReal128BinaryOperation(const Real128BinaryRequest& request);
struct Real128TotalOrderKeyResult {
  NumericResult numeric;
  // Big-endian IEEE total-order key, not a storage value. Distinguishes signed
  // zeros and NaN signs/payloads; special admission follows the given context.
  std::optional<Real128Bytes> key;
};
Real128TotalOrderKeyResult MakeReal128TotalOrderKey(
    const std::uint8_t* bytes, std::size_t size, const NumericContext& context = {});
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

using Decimal128Bytes = std::array<std::uint8_t, 16>;
enum class Decimal128Class : std::uint8_t { finite, infinity, quiet_nan, signaling_nan };
struct Decimal128Value {
  Decimal128Class classification = Decimal128Class::finite;
  bool negative = false;
  // Finite quantum exponent, not the adjusted scientific exponent.
  std::int32_t exponent = 0;
  // Little-endian unsigned coefficient, or NaN payload. No host ABI encoding.
  Decimal128Bytes coefficient{};
};
struct Decimal128BinaryResult {
  NumericResult numeric;
  std::optional<Decimal128Bytes> bytes;
  std::optional<Decimal128Value> value;
};
// Exact canonical BID decimal128 value codec. Preserves finite cohorts, signed
// zeros and NaN payloads. Cannot silently round, saturate, or reinterpret DPD.
// Codec selection/version and SQL NULL belong to the caller's bound descriptor.
Decimal128BinaryResult EncodeDecimal128LittleEndian(
    std::string_view text, bool allow_special_values = false);
Decimal128BinaryResult DecodeDecimal128LittleEndian(
    const std::uint8_t* bytes, std::size_t size, bool allow_special_values = false);
enum class Decimal128OrderProfile : std::uint8_t {
  numeric_total_nan_last,
  ieee_total_order
};
using Decimal128OrderKey = std::array<std::uint8_t, 21>;
struct Decimal128OrderKeyResult {
  NumericResult numeric;
  std::optional<Decimal128OrderKey> key;
};
// Explicit descriptor-selected comparison profile; this API does not bind a
// catalog policy. Numeric order coalesces finite cohorts, signed zeros and NaNs.
// IEEE total order preserves their quantum, sign, class and payload ordering.
Decimal128OrderKeyResult MakeDecimal128OrderKey(
    const std::uint8_t* bytes, std::size_t size, Decimal128OrderProfile profile,
    bool allow_special_values = false);

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
