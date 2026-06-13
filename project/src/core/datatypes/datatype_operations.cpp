// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"

#include "sbl_numeric.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::datatypes};
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  if ((value.size() % 2) != 0) { return {}; }
  std::string out;
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

bool IsSignedInteger(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::int8 || type_id == CanonicalTypeId::int16 ||
         type_id == CanonicalTypeId::int32 || type_id == CanonicalTypeId::int64 ||
         type_id == CanonicalTypeId::int128;
}

bool IsUnsignedInteger(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::uint8 || type_id == CanonicalTypeId::uint16 ||
         type_id == CanonicalTypeId::uint32 || type_id == CanonicalTypeId::uint64 ||
         type_id == CanonicalTypeId::uint128;
}

bool IsInteger(CanonicalTypeId type_id) {
  return IsSignedInteger(type_id) || IsUnsignedInteger(type_id);
}

bool IsReal(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::real16 || type_id == CanonicalTypeId::bfloat16 ||
         type_id == CanonicalTypeId::real32 || type_id == CanonicalTypeId::real64 ||
         type_id == CanonicalTypeId::real128;
}

bool IsNumeric(CanonicalTypeId type_id) {
  return IsInteger(type_id) || IsReal(type_id) || type_id == CanonicalTypeId::decimal ||
         type_id == CanonicalTypeId::decimal_float;
}

bool IsCharacter(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::character;
}

bool IsTemporal(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::date || type_id == CanonicalTypeId::time ||
         type_id == CanonicalTypeId::timestamp;
}

bool IsBinaryLike(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::binary || type_id == CanonicalTypeId::blob;
}

bool IsDocument(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::document || type_id == CanonicalTypeId::json_document ||
         type_id == CanonicalTypeId::binary_json_document || type_id == CanonicalTypeId::bson_document ||
         type_id == CanonicalTypeId::xml_document || type_id == CanonicalTypeId::hstore_document ||
         type_id == CanonicalTypeId::object_document || type_id == CanonicalTypeId::flattened_object_document;
}

bool IsOpaqueRenderOnly(CanonicalTypeId type_id) {
  return type_id == CanonicalTypeId::opaque_extension;
}

std::uint32_t IntegerBits(CanonicalTypeId type_id) {
  switch (type_id) {
    case CanonicalTypeId::int8:
    case CanonicalTypeId::uint8: return 8;
    case CanonicalTypeId::int16:
    case CanonicalTypeId::uint16: return 16;
    case CanonicalTypeId::int32:
    case CanonicalTypeId::uint32: return 32;
    case CanonicalTypeId::int64:
    case CanonicalTypeId::uint64: return 64;
    case CanonicalTypeId::int128:
    case CanonicalTypeId::uint128: return 128;
    default: return 0;
  }
}

std::string TrimLeadingZeros(std::string digits) {
  const bool negative = !digits.empty() && digits.front() == '-';
  const bool signed_prefix =
      !digits.empty() && (digits.front() == '-' || digits.front() == '+');
  std::size_t first = signed_prefix ? 1 : 0;
  while (first + 1 < digits.size() && digits[first] == '0') { ++first; }
  std::string normalized = digits.substr(first);
  if (normalized.empty()) { normalized = "0"; }
  if (negative && normalized != "0") { normalized.insert(normalized.begin(), '-'); }
  return normalized;
}

bool DecimalIntegerText(std::string_view value, bool allow_negative) {
  if (value.empty()) { return false; }
  std::size_t start = 0;
  if (value.front() == '-') {
    if (!allow_negative) { return false; }
    start = 1;
  } else if (value.front() == '+') {
    start = 1;
  }
  if (start == value.size()) { return false; }
  for (std::size_t i = start; i < value.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(value[i]))) { return false; }
  }
  return true;
}

int CompareUnsignedDecimal(std::string left, std::string right) {
  left = TrimLeadingZeros(std::move(left));
  right = TrimLeadingZeros(std::move(right));
  if (left.size() < right.size()) { return -1; }
  if (left.size() > right.size()) { return 1; }
  if (left < right) { return -1; }
  if (left > right) { return 1; }
  return 0;
}

std::string AbsoluteDecimal(std::string value) {
  if (!value.empty() && (value.front() == '-' || value.front() == '+')) { value.erase(value.begin()); }
  return TrimLeadingZeros(std::move(value));
}

bool IntegerFits(CanonicalTypeId target_type_id, const std::string& value) {
  if (!DecimalIntegerText(value, IsSignedInteger(target_type_id))) { return false; }
  const bool negative = !value.empty() && value.front() == '-';
  if (IsUnsignedInteger(target_type_id) && negative) { return false; }
  switch (target_type_id) {
    case CanonicalTypeId::int8:
    case CanonicalTypeId::int16:
    case CanonicalTypeId::int32:
    case CanonicalTypeId::int64: {
      try {
        std::size_t pos = 0;
        const long long parsed = std::stoll(value, &pos);
        if (pos != value.size()) { return false; }
        if (target_type_id == CanonicalTypeId::int8) {
          return parsed >= std::numeric_limits<std::int8_t>::min() && parsed <= std::numeric_limits<std::int8_t>::max();
        }
        if (target_type_id == CanonicalTypeId::int16) {
          return parsed >= std::numeric_limits<std::int16_t>::min() && parsed <= std::numeric_limits<std::int16_t>::max();
        }
        if (target_type_id == CanonicalTypeId::int32) {
          return parsed >= std::numeric_limits<std::int32_t>::min() && parsed <= std::numeric_limits<std::int32_t>::max();
        }
        return true;
      } catch (...) {
        return false;
      }
    }
    case CanonicalTypeId::uint8:
    case CanonicalTypeId::uint16:
    case CanonicalTypeId::uint32:
    case CanonicalTypeId::uint64: {
      try {
        std::size_t pos = 0;
        const unsigned long long parsed = std::stoull(value, &pos);
        if (pos != value.size()) { return false; }
        if (target_type_id == CanonicalTypeId::uint8) { return parsed <= std::numeric_limits<std::uint8_t>::max(); }
        if (target_type_id == CanonicalTypeId::uint16) { return parsed <= std::numeric_limits<std::uint16_t>::max(); }
        if (target_type_id == CanonicalTypeId::uint32) { return parsed <= std::numeric_limits<std::uint32_t>::max(); }
        return true;
      } catch (...) {
        return false;
      }
    }
    case CanonicalTypeId::int128: {
      static const std::string kMax = "170141183460469231731687303715884105727";
      static const std::string kMinAbs = "170141183460469231731687303715884105728";
      return CompareUnsignedDecimal(AbsoluteDecimal(value), negative ? kMinAbs : kMax) <= 0;
    }
    case CanonicalTypeId::uint128: {
      static const std::string kMax = "340282366920938463463374607431768211455";
      return !negative && CompareUnsignedDecimal(AbsoluteDecimal(value), kMax) <= 0;
    }
    default:
      return false;
  }
}

bool FloatingText(std::string_view value) {
  if (value.empty()) { return false; }
  try {
    std::size_t pos = 0;
    (void)std::stold(std::string(value), &pos);
    return pos == value.size();
  } catch (...) {
    return false;
  }
}

std::string TrimAsciiWhitespace(std::string value);

using U128 = unsigned __int128;
using I128 = __int128_t;

struct ParsedDecimal {
  bool negative = false;
  bool negative_zero = false;
  U128 coefficient = 0;
  std::uint32_t scale = 0;
};

enum class NumericSpecialKind {
  finite,
  quiet_nan,
  signaling_nan,
  positive_infinity,
  negative_infinity
};

struct ParsedSpecialNumeric {
  NumericSpecialKind kind = NumericSpecialKind::finite;
  bool negative_zero = false;
  std::string canonical;
};

U128 Pow10U128(std::uint32_t exponent) {
  U128 value = 1;
  for (std::uint32_t i = 0; i < exponent; ++i) { value *= 10; }
  return value;
}

std::string U128ToString(U128 value) {
  if (value == 0) { return "0"; }
  std::string out;
  while (value != 0) {
    const int digit = static_cast<int>(value % 10);
    out.push_back(static_cast<char>('0' + digit));
    value /= 10;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::uint32_t U128DigitCount(U128 value) {
  return static_cast<std::uint32_t>(U128ToString(value).size());
}

bool ParseU128DecimalDigits(const std::string& digits, U128* out) {
  U128 value = 0;
  for (char c : digits) {
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    value = (value * 10) + static_cast<U128>(c - '0');
  }
  *out = value;
  return true;
}

bool ParseSignedDecimalExponent(const std::string& value, long long* out) {
  if (value.empty()) { return false; }
  std::size_t pos = 0;
  bool negative = false;
  if (value[pos] == '+' || value[pos] == '-') {
    negative = value[pos] == '-';
    ++pos;
  }
  if (pos == value.size()) { return false; }
  long long parsed = 0;
  for (; pos < value.size(); ++pos) {
    if (!std::isdigit(static_cast<unsigned char>(value[pos]))) { return false; }
    parsed = (parsed * 10) + (value[pos] - '0');
    if (parsed > 10000) { return false; }
  }
  *out = negative ? -parsed : parsed;
  return true;
}

bool ParseDecimalFiniteText(const std::string& input, ParsedDecimal* out) {
  std::string value = TrimAsciiWhitespace(input);
  if (value.empty()) { return false; }
  ParsedDecimal parsed;
  if (value.front() == '+' || value.front() == '-') {
    parsed.negative = value.front() == '-';
    value.erase(value.begin());
  }
  if (value.empty()) { return false; }
  const std::size_t exponent_pos = value.find_first_of("eE");
  std::string significand = exponent_pos == std::string::npos ? value : value.substr(0, exponent_pos);
  long long exponent = 0;
  if (exponent_pos != std::string::npos &&
      !ParseSignedDecimalExponent(value.substr(exponent_pos + 1), &exponent)) {
    return false;
  }
  const std::size_t decimal_pos = significand.find('.');
  if (decimal_pos != std::string::npos && significand.find('.', decimal_pos + 1) != std::string::npos) {
    return false;
  }
  std::string digits;
  std::uint32_t fractional_digits = 0;
  bool saw_digit = false;
  for (std::size_t i = 0; i < significand.size(); ++i) {
    const char c = significand[i];
    if (c == '.') { continue; }
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    saw_digit = true;
    digits.push_back(c);
    if (decimal_pos != std::string::npos && i > decimal_pos) { ++fractional_digits; }
  }
  if (!saw_digit) { return false; }
  while (!digits.empty() && digits.front() == '0') { digits.erase(digits.begin()); }
  long long final_scale = static_cast<long long>(fractional_digits) - exponent;
  if (digits.empty()) {
    parsed.coefficient = 0;
    parsed.scale = 0;
    parsed.negative_zero = parsed.negative;
    *out = parsed;
    return true;
  }
  if (final_scale < 0) {
    const long long zeros = -final_scale;
    if (zeros > 38 || digits.size() + static_cast<std::size_t>(zeros) > 38) { return false; }
    digits.append(static_cast<std::size_t>(zeros), '0');
    final_scale = 0;
  }
  if (digits.size() > 38 || final_scale > 10000) { return false; }
  if (!ParseU128DecimalDigits(digits, &parsed.coefficient)) { return false; }
  parsed.scale = static_cast<std::uint32_t>(final_scale);
  parsed.negative_zero = parsed.negative && parsed.coefficient == 0;
  *out = parsed;
  return true;
}

bool RoundDecimalToScale(ParsedDecimal* value, std::uint32_t target_scale, DatatypeRoundingMode rounding) {
  if (value->coefficient == 0) {
    value->scale = target_scale;
    return true;
  }
  if (value->scale == target_scale) { return true; }
  if (value->scale < target_scale) {
    const std::uint32_t diff = target_scale - value->scale;
    if (diff > 38) { return false; }
    value->coefficient *= Pow10U128(diff);
    value->scale = target_scale;
    return true;
  }
  const std::uint32_t diff = value->scale - target_scale;
  if (diff > 38) { return false; }
  const U128 divisor = Pow10U128(diff);
  U128 quotient = value->coefficient / divisor;
  const U128 remainder = value->coefficient % divisor;
  bool increment = false;
  if (rounding != DatatypeRoundingMode::truncate && remainder != 0) {
    const U128 half = divisor / 2;
    if (remainder > half) {
      increment = true;
    } else if (remainder == half) {
      increment = rounding == DatatypeRoundingMode::half_up ||
                  (rounding == DatatypeRoundingMode::half_even && (quotient % 2) != 0);
    }
  }
  if (increment) { ++quotient; }
  value->coefficient = quotient;
  value->scale = target_scale;
  return true;
}

std::string RenderFixedDecimal(const ParsedDecimal& value) {
  std::string digits = U128ToString(value.coefficient);
  if (value.scale > 0) {
    if (digits.size() <= value.scale) {
      digits.insert(digits.begin(), static_cast<std::size_t>(value.scale - digits.size() + 1), '0');
    }
    digits.insert(digits.end() - static_cast<std::ptrdiff_t>(value.scale), '.');
  }
  if (value.negative && value.coefficient != 0) { digits.insert(digits.begin(), '-'); }
  return digits;
}

bool CanonicalizeExactDecimalText(const std::string& input,
                                  const DatatypeNumericContext& context,
                                  std::string* out) {
  if (context.precision == 0 || context.precision > 38 || context.scale > context.precision) { return false; }
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(input, &parsed)) { return false; }
  if (!RoundDecimalToScale(&parsed, context.scale, context.rounding)) { return false; }
  if (U128DigitCount(parsed.coefficient) > context.precision) { return false; }
  *out = RenderFixedDecimal(parsed);
  return true;
}

ParsedSpecialNumeric ParseSpecialNumericText(const std::string& input) {
  const std::string value = LowerAscii(TrimAsciiWhitespace(input));
  ParsedSpecialNumeric parsed;
  if (value == "nan" || value == "+nan" || value == "-nan") {
    parsed.kind = NumericSpecialKind::quiet_nan;
    parsed.canonical = "NaN";
    return parsed;
  }
  if (value == "snan" || value == "+snan" || value == "-snan") {
    parsed.kind = NumericSpecialKind::signaling_nan;
    parsed.canonical = "sNaN";
    return parsed;
  }
  if (value == "inf" || value == "+inf" || value == "infinity" || value == "+infinity") {
    parsed.kind = NumericSpecialKind::positive_infinity;
    parsed.canonical = "Infinity";
    return parsed;
  }
  if (value == "-inf" || value == "-infinity") {
    parsed.kind = NumericSpecialKind::negative_infinity;
    parsed.canonical = "-Infinity";
    return parsed;
  }
  return parsed;
}

std::string RenderDecimalFloat(const ParsedDecimal& value) {
  if (value.coefficient == 0) { return value.negative_zero ? "-0E+0" : "0E+0"; }
  ParsedDecimal normalized = value;
  while (normalized.scale > 0 && (normalized.coefficient % 10) == 0) {
    normalized.coefficient /= 10;
    --normalized.scale;
  }
  std::string digits = U128ToString(normalized.coefficient);
  const long long exponent = static_cast<long long>(digits.size()) - 1 - static_cast<long long>(normalized.scale);
  std::string mantissa;
  if (normalized.negative) { mantissa.push_back('-'); }
  mantissa.push_back(digits.front());
  if (digits.size() > 1) {
    mantissa.push_back('.');
    mantissa.append(digits.substr(1));
  }
  mantissa.push_back('E');
  mantissa.push_back(exponent < 0 ? '-' : '+');
  mantissa.append(std::to_string(exponent < 0 ? -exponent : exponent));
  return mantissa;
}

bool CanonicalizeDecimalFloatText(const std::string& input,
                                  const DatatypeNumericContext& context,
                                  std::string* out) {
  const ParsedSpecialNumeric special = ParseSpecialNumericText(input);
  if (special.kind != NumericSpecialKind::finite) {
    if (!context.allow_special_values) { return false; }
    *out = special.canonical;
    return true;
  }
  DatatypeNumericContext finite_context = context;
  if (finite_context.precision == 0) { finite_context.precision = 34; }
  if (finite_context.precision > 38) { return false; }
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(input, &parsed)) { return false; }
  while (parsed.scale > 0 && (parsed.coefficient % 10) == 0) {
    parsed.coefficient /= 10;
    --parsed.scale;
  }
  if (U128DigitCount(parsed.coefficient) > finite_context.precision) { return false; }
  *out = RenderDecimalFloat(parsed);
  return true;
}

bool CanonicalizeReal128Text(const std::string& input, std::string* out) {
  const ParsedSpecialNumeric special = ParseSpecialNumericText(input);
  if (special.kind != NumericSpecialKind::finite) {
    *out = special.canonical;
    return true;
  }
  std::string value = TrimAsciiWhitespace(input);
  if (value.empty()) { return false; }
  if (value.front() == '+') { value.erase(value.begin()); }
  errno = 0;
  char* end = nullptr;
  const long double parsed = std::strtold(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0' || errno == ERANGE) { return false; }
  if (std::isnan(parsed)) {
    *out = "NaN";
    return true;
  }
  if (std::isinf(parsed)) {
    *out = std::signbit(parsed) ? "-Infinity" : "Infinity";
    return true;
  }
  if (parsed == 0.0L) {
    *out = std::signbit(parsed) || (!value.empty() && value.front() == '-') ? "-0" : "0";
    return true;
  }
  *out = value;
  return true;
}

DatatypeNumericOperationResult NumericFailure(std::string detail) {
  DatatypeNumericOperationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      "SB_DATATYPE_NUMERIC_OPERATION_REJECTED",
                                                      "datatype.numeric_operation.rejected",
                                                      std::move(detail));
  return result;
}

I128 DecimalToSignedInteger(const ParsedDecimal& value) {
  const I128 magnitude = static_cast<I128>(value.coefficient);
  return value.negative && magnitude != 0 ? -magnitude : magnitude;
}

bool SignedIntegerToDecimal(I128 input, std::uint32_t scale, ParsedDecimal* out) {
  ParsedDecimal value;
  value.negative = input < 0;
  value.coefficient = input < 0 ? static_cast<U128>(-input) : static_cast<U128>(input);
  value.scale = scale;
  value.negative_zero = false;
  *out = value;
  return true;
}

bool AlignDecimalScales(ParsedDecimal* left, ParsedDecimal* right, std::uint32_t target_scale) {
  if (left->scale > target_scale || right->scale > target_scale) { return false; }
  if (left->scale < target_scale) {
    const std::uint32_t diff = target_scale - left->scale;
    if (diff > 38) { return false; }
    left->coefficient *= Pow10U128(diff);
    left->scale = target_scale;
  }
  if (right->scale < target_scale) {
    const std::uint32_t diff = target_scale - right->scale;
    if (diff > 38) { return false; }
    right->coefficient *= Pow10U128(diff);
    right->scale = target_scale;
  }
  return true;
}

struct ExactDecimalTextParts {
  bool negative = false;
  std::string integer_part = "0";
  std::string fractional_part;
};

bool ParseExactDecimalTextParts(const std::string& input,
                                ExactDecimalTextParts* out) {
  std::string value = TrimAsciiWhitespace(input);
  if (value.empty()) { return false; }
  ExactDecimalTextParts parts;
  if (value.front() == '+' || value.front() == '-') {
    parts.negative = value.front() == '-';
    value.erase(value.begin());
  }
  if (value.empty()) { return false; }
  const std::size_t exponent_pos = value.find_first_of("eE");
  std::string significand =
      exponent_pos == std::string::npos ? value : value.substr(0, exponent_pos);
  long long exponent = 0;
  if (exponent_pos != std::string::npos &&
      !ParseSignedDecimalExponent(value.substr(exponent_pos + 1), &exponent)) {
    return false;
  }
  const std::size_t decimal_pos = significand.find('.');
  if (decimal_pos != std::string::npos &&
      significand.find('.', decimal_pos + 1) != std::string::npos) {
    return false;
  }
  std::string digits;
  long long fractional_digits = 0;
  bool saw_digit = false;
  for (std::size_t i = 0; i < significand.size(); ++i) {
    const char c = significand[i];
    if (c == '.') { continue; }
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    saw_digit = true;
    digits.push_back(c);
    if (decimal_pos != std::string::npos && i > decimal_pos) {
      ++fractional_digits;
    }
  }
  if (!saw_digit) { return false; }
  while (!digits.empty() && digits.front() == '0') {
    digits.erase(digits.begin());
  }
  if (digits.empty()) {
    parts.negative = false;
    *out = std::move(parts);
    return true;
  }
  long long final_scale = fractional_digits - exponent;
  if (final_scale < 0) {
    digits.append(static_cast<std::size_t>(-final_scale), '0');
    final_scale = 0;
  }
  const auto scale = static_cast<std::size_t>(final_scale);
  if (scale == 0) {
    parts.integer_part = std::move(digits);
  } else if (digits.size() <= scale) {
    parts.integer_part = "0";
    parts.fractional_part.assign(scale - digits.size(), '0');
    parts.fractional_part += digits;
  } else {
    const std::size_t integer_digits = digits.size() - scale;
    parts.integer_part = digits.substr(0, integer_digits);
    parts.fractional_part = digits.substr(integer_digits);
  }
  while (parts.integer_part.size() > 1 && parts.integer_part.front() == '0') {
    parts.integer_part.erase(parts.integer_part.begin());
  }
  while (!parts.fractional_part.empty() &&
         parts.fractional_part.back() == '0') {
    parts.fractional_part.pop_back();
  }
  *out = std::move(parts);
  return true;
}

bool CompareFiniteDecimalText(const std::string& left_text,
                              const std::string& right_text,
                              int* comparison) {
  ExactDecimalTextParts left;
  ExactDecimalTextParts right;
  if (!ParseExactDecimalTextParts(left_text, &left) ||
      !ParseExactDecimalTextParts(right_text, &right)) {
    return false;
  }
  if (left.negative != right.negative) {
    *comparison = left.negative ? -1 : 1;
    return true;
  }
  if (left.integer_part.size() < right.integer_part.size()) {
    *comparison = left.negative ? 1 : -1;
    return true;
  }
  if (left.integer_part.size() > right.integer_part.size()) {
    *comparison = left.negative ? -1 : 1;
    return true;
  }
  if (left.integer_part < right.integer_part) {
    *comparison = left.negative ? 1 : -1;
    return true;
  }
  if (left.integer_part > right.integer_part) {
    *comparison = left.negative ? -1 : 1;
    return true;
  }
  const std::size_t max_fraction =
      std::max(left.fractional_part.size(), right.fractional_part.size());
  for (std::size_t i = 0; i < max_fraction; ++i) {
    const char l = i < left.fractional_part.size() ? left.fractional_part[i] : '0';
    const char r = i < right.fractional_part.size() ? right.fractional_part[i] : '0';
    if (l < r) {
      *comparison = left.negative ? 1 : -1;
      return true;
    }
    if (l > r) {
      *comparison = left.negative ? -1 : 1;
      return true;
    }
  }
  *comparison = 0;
  return true;
}

bool ParseFixedDigits(const std::string& value, std::size_t pos, std::size_t count, int* out) {
  if (pos + count > value.size()) { return false; }
  int parsed = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = value[pos + i];
    if (!std::isdigit(static_cast<unsigned char>(c))) { return false; }
    parsed = parsed * 10 + (c - '0');
  }
  *out = parsed;
  return true;
}

bool LeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return LeapYear(year) ? 29 : 28;
    case 3: return 31;
    case 4: return 30;
    case 5: return 31;
    case 6: return 30;
    case 7: return 31;
    case 8: return 31;
    case 9: return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 0;
  }
}

bool TimezoneSuffixText(std::string_view suffix) {
  if (suffix.empty()) { return true; }
  if (suffix == "Z" || suffix == "z") { return true; }
  if (suffix.size() != 6 || (suffix[0] != '+' && suffix[0] != '-') || suffix[3] != ':') { return false; }
  int hour = 0;
  int minute = 0;
  const std::string text(suffix);
  return ParseFixedDigits(text, 1, 2, &hour) && ParseFixedDigits(text, 4, 2, &minute) &&
         hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

std::size_t TimezoneSuffixStart(const std::string& value, std::size_t start) {
  for (std::size_t i = start; i < value.size(); ++i) {
    if (value[i] == 'Z' || value[i] == 'z' || value[i] == '+' || value[i] == '-') { return i; }
  }
  return value.size();
}

bool DateText(const std::string& value) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') { return false; }
  if (!ParseFixedDigits(value, 0, 4, &year) || !ParseFixedDigits(value, 5, 2, &month) ||
      !ParseFixedDigits(value, 8, 2, &day)) {
    return false;
  }
  return year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month);
}

bool TimeText(const std::string& value) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (value.size() < 8 || value[2] != ':' || value[5] != ':') { return false; }
  if (!ParseFixedDigits(value, 0, 2, &hour) || !ParseFixedDigits(value, 3, 2, &minute) ||
      !ParseFixedDigits(value, 6, 2, &second)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) { return false; }
  std::size_t pos = 8;
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    const std::size_t fraction_start = pos;
    while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) { ++pos; }
    const std::size_t fraction_digits = pos - fraction_start;
    if (fraction_digits == 0 || fraction_digits > 12) { return false; }
  }
  const std::size_t tz_start = TimezoneSuffixStart(value, pos);
  if (tz_start != pos && tz_start != value.size()) { return false; }
  return TimezoneSuffixText(std::string_view(value).substr(tz_start));
}

bool TimestampText(const std::string& value) {
  const auto t_pos = value.find('T');
  const auto space_pos = value.find(' ');
  const auto pos = t_pos == std::string::npos ? space_pos : t_pos;
  return pos != std::string::npos && DateText(value.substr(0, pos)) && TimeText(value.substr(pos + 1));
}

bool HexText(std::string_view value, bool allow_prefix, bool require_even_digits) {
  if (allow_prefix && value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    value.remove_prefix(2);
  }
  if (value.empty()) { return false; }
  if (require_even_digits && (value.size() % 2) != 0) { return false; }
  for (char c : value) {
    if (HexValue(c) < 0) { return false; }
  }
  return true;
}

std::string HexDecodeStrict(std::string_view value, bool allow_prefix, bool* ok) {
  if (allow_prefix && value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    value.remove_prefix(2);
  }
  if (!HexText(value, false, true)) {
    *ok = false;
    return {};
  }
  std::string out;
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    out.push_back(static_cast<char>((HexValue(value[i]) << 4) | HexValue(value[i + 1])));
  }
  *ok = true;
  return out;
}

std::string HexEncodeLower(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

std::string UuidCompactHex(const std::string& value) {
  std::string compact;
  compact.reserve(32);
  for (char c : value) {
    if (c != '-') { compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); }
  }
  return compact;
}

std::string UuidTextToBytes(const std::string& value, bool* ok) {
  if (!IsUuidText(value)) {
    *ok = false;
    return {};
  }
  return HexDecodeStrict(UuidCompactHex(value), false, ok);
}

std::string BytesToUuidText(std::string_view value) {
  if (value.size() != 16) { return {}; }
  const std::string hex = HexEncodeLower(value);
  return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
         hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

class JsonTextValidator {
 public:
  explicit JsonTextValidator(std::string_view text) : text_(text) {}

  bool Valid() {
    SkipWhitespace();
    if (!ParseValue()) { return false; }
    SkipWhitespace();
    return pos_ == text_.size();
  }

 private:
  void SkipWhitespace() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
  }

  bool Consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool ConsumeLiteral(std::string_view expected) {
    if (text_.substr(pos_, expected.size()) != expected) { return false; }
    pos_ += expected.size();
    return true;
  }

  bool ParseValue() {
    SkipWhitespace();
    if (pos_ >= text_.size()) { return false; }
    const char c = text_[pos_];
    if (c == '{') { return ParseObject(); }
    if (c == '[') { return ParseArray(); }
    if (c == '"') { return ParseString(); }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) { return ParseNumber(); }
    if (c == 't') { return ConsumeLiteral("true"); }
    if (c == 'f') { return ConsumeLiteral("false"); }
    if (c == 'n') { return ConsumeLiteral("null"); }
    return false;
  }

  bool ParseObject() {
    if (!Consume('{')) { return false; }
    SkipWhitespace();
    if (Consume('}')) { return true; }
    while (true) {
      SkipWhitespace();
      if (!ParseString()) { return false; }
      SkipWhitespace();
      if (!Consume(':')) { return false; }
      if (!ParseValue()) { return false; }
      SkipWhitespace();
      if (Consume('}')) { return true; }
      if (!Consume(',')) { return false; }
    }
  }

  bool ParseArray() {
    if (!Consume('[')) { return false; }
    SkipWhitespace();
    if (Consume(']')) { return true; }
    while (true) {
      if (!ParseValue()) { return false; }
      SkipWhitespace();
      if (Consume(']')) { return true; }
      if (!Consume(',')) { return false; }
    }
  }

  bool ParseString() {
    if (!Consume('"')) { return false; }
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') { return true; }
      if (c < 0x20) { return false; }
      if (c == '\\') {
        if (pos_ >= text_.size()) { return false; }
        const char escaped = text_[pos_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' || escaped == 'f' ||
            escaped == 'n' || escaped == 'r' || escaped == 't') {
          continue;
        }
        if (escaped == 'u') {
          for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size() || HexValue(text_[pos_++]) < 0) { return false; }
          }
          continue;
        }
        return false;
      }
    }
    return false;
  }

  bool ParseNumber() {
    if (Consume('-') && pos_ >= text_.size()) { return false; }
    if (Consume('0')) {
      if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
    } else {
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    if (Consume('.')) {
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) { ++pos_; }
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) { return false; }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) { ++pos_; }
    }
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

bool JsonText(const std::string& value) {
  return JsonTextValidator(value).Valid();
}

std::string TrimAsciiWhitespace(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) { value.erase(value.begin()); }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) { value.pop_back(); }
  return value;
}

bool XmlNameText(std::string_view value) {
  if (value.empty()) { return false; }
  const unsigned char first = static_cast<unsigned char>(value.front());
  if (!std::isalpha(first) && first != '_' && first != ':') { return false; }
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '_' && c != '-' && c != ':' && c != '.') { return false; }
  }
  return true;
}

bool XmlText(const std::string& input) {
  const std::string value = TrimAsciiWhitespace(input);
  if (value.size() < 7 || value.front() != '<' || value.back() != '>') { return false; }
  if (StartsWith(value, "<?xml")) {
    const auto declaration_end = value.find("?>");
    if (declaration_end == std::string::npos || declaration_end + 2 >= value.size()) { return false; }
    return XmlText(value.substr(declaration_end + 2));
  }
  if (value[1] == '/' || value[1] == '!' || value[1] == '?') { return false; }
  const auto open_end = value.find('>');
  if (open_end == std::string::npos || open_end < 2) { return false; }
  std::string open_name = value.substr(1, open_end - 1);
  const auto attr_pos = open_name.find_first_of(" \t\r\n/");
  const bool self_closing = !open_name.empty() && open_name.back() == '/';
  if (attr_pos != std::string::npos) { open_name = open_name.substr(0, attr_pos); }
  if (!XmlNameText(open_name)) { return false; }
  if (self_closing || (open_end > 0 && value[open_end - 1] == '/')) { return true; }
  const std::string close = "</" + open_name + ">";
  return value.size() >= close.size() && value.substr(value.size() - close.size()) == close;
}

DatatypeOperationValue BoolValue(bool value) {
  return {CanonicalTypeId::boolean, value ? "true" : "false", false};
}

DatatypeOperationValue UInt64Value(std::uint64_t value) {
  return {CanonicalTypeId::uint64, std::to_string(value), false};
}

DatatypeCastResult CastFailure(std::string detail, DatatypeCastCategory category = DatatypeCastCategory::forbidden) {
  DatatypeCastResult result;
  result.status = ErrorStatus();
  result.category = category;
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      "SB_DATATYPE_CAST_REJECTED",
                                                      "datatype.cast.rejected",
                                                      std::move(detail));
  return result;
}

DatatypeExtractResult ExtractFailure(std::string detail) {
  DatatypeExtractResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      "SB_DATATYPE_EXTRACT_REJECTED",
                                                      "datatype.extract.rejected",
                                                      std::move(detail));
  return result;
}

DatatypeSetOperationResult SetFailure(std::string detail) {
  DatatypeSetOperationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                      "SB_DATATYPE_SET_OPERATION_REJECTED",
                                                      "datatype.set_operation.rejected",
                                                      std::move(detail));
  return result;
}

bool ParseEncodedSet(const std::string& encoded, std::vector<std::string>* values) {
  if (!StartsWith(encoded, "SBSET1;")) { return false; }
  const auto marker = encoded.find(";items=");
  if (marker == std::string::npos) { return false; }
  const std::string item_list = encoded.substr(marker + 7);
  if (item_list.empty()) { return true; }
  for (const auto& hex : Split(item_list, ',')) {
    const std::string decoded = HexDecode(hex);
    if (decoded.empty() && !hex.empty()) { return false; }
    values->push_back(decoded);
  }
  return true;
}

std::string ExtractTemporalComponent(const std::string& value, const std::string& field) {
  const std::string date = value.size() >= 10 ? value.substr(0, 10) : value;
  if ((field == "year" || field == "yyyy") && DateText(date)) { return date.substr(0, 4); }
  if ((field == "month" || field == "mm") && DateText(date)) { return date.substr(5, 2); }
  if ((field == "day" || field == "dd") && DateText(date)) { return date.substr(8, 2); }
  const auto time_start = value.find('T') != std::string::npos ? value.find('T') + 1 :
                          (value.find(' ') != std::string::npos ? value.find(' ') + 1 : 0);
  const std::string time = time_start == 0 ? value : value.substr(time_start);
  if (field == "hour" && TimeText(time)) { return time.substr(0, 2); }
  if (field == "minute" && TimeText(time)) { return time.substr(3, 2); }
  if (field == "second" && TimeText(time)) { return time.substr(6, 2); }
  return {};
}

std::uint64_t HexToU64(std::string_view hex) {
  std::uint64_t value = 0;
  for (char c : hex) {
    const int digit = HexValue(c);
    if (digit < 0) { return 0; }
    value = (value << 4) | static_cast<std::uint64_t>(digit);
  }
  return value;
}

}  // namespace

const char* DatatypeCastCategoryName(DatatypeCastCategory category) {
  switch (category) {
    case DatatypeCastCategory::identity: return "identity";
    case DatatypeCastCategory::lossless_implicit: return "lossless_implicit";
    case DatatypeCastCategory::lossless_explicit: return "lossless_explicit";
    case DatatypeCastCategory::lossy_explicit: return "lossy_explicit";
    case DatatypeCastCategory::reference_compatibility_explicit: return "reference_compatibility_explicit";
    case DatatypeCastCategory::domain_to_base: return "domain_to_base";
    case DatatypeCastCategory::base_to_domain: return "base_to_domain";
    case DatatypeCastCategory::forbidden: return "forbidden";
  }
  return "forbidden";
}

const char* DatatypeSetOperationKindName(DatatypeSetOperationKind operation) {
  switch (operation) {
    case DatatypeSetOperationKind::membership: return "membership";
    case DatatypeSetOperationKind::equals: return "equals";
    case DatatypeSetOperationKind::subset: return "subset";
    case DatatypeSetOperationKind::superset: return "superset";
    case DatatypeSetOperationKind::cardinality: return "cardinality";
  }
  return "membership";
}

const char* DatatypeNumericOperationKindName(DatatypeNumericOperationKind operation) {
  switch (operation) {
    case DatatypeNumericOperationKind::canonicalize: return "canonicalize";
    case DatatypeNumericOperationKind::add: return "add";
    case DatatypeNumericOperationKind::subtract: return "subtract";
    case DatatypeNumericOperationKind::multiply: return "multiply";
    case DatatypeNumericOperationKind::divide: return "divide";
    case DatatypeNumericOperationKind::compare: return "compare";
  }
  return "canonicalize";
}

const char* DatatypeRoundingModeName(DatatypeRoundingMode rounding) {
  switch (rounding) {
    case DatatypeRoundingMode::half_even: return "half_even";
    case DatatypeRoundingMode::half_up: return "half_up";
    case DatatypeRoundingMode::truncate: return "truncate";
  }
  return "half_even";
}

DatatypeCastCategory ClassifyDatatypeCast(CanonicalTypeId source_type_id,
                                          CanonicalTypeId target_type_id,
                                          bool reference_compatibility_profile) {
  if (source_type_id == target_type_id) { return DatatypeCastCategory::identity; }
  if (source_type_id == CanonicalTypeId::null_type) { return DatatypeCastCategory::lossless_implicit; }
  if (IsOpaqueRenderOnly(source_type_id) && IsCharacter(target_type_id)) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if (IsOpaqueRenderOnly(source_type_id) || IsOpaqueRenderOnly(target_type_id)) {
    return DatatypeCastCategory::forbidden;
  }
  if (IsInteger(source_type_id) && IsInteger(target_type_id)) {
    if (IsSignedInteger(source_type_id) == IsSignedInteger(target_type_id) &&
        IntegerBits(target_type_id) >= IntegerBits(source_type_id)) {
      return DatatypeCastCategory::lossless_implicit;
    }
    if (IntegerBits(target_type_id) >= IntegerBits(source_type_id)) { return DatatypeCastCategory::lossless_explicit; }
    return DatatypeCastCategory::lossy_explicit;
  }
  if (IsNumeric(source_type_id) && IsNumeric(target_type_id)) { return DatatypeCastCategory::lossy_explicit; }
  if (IsInteger(source_type_id) && target_type_id == CanonicalTypeId::boolean) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if ((IsNumeric(source_type_id) || source_type_id == CanonicalTypeId::boolean || source_type_id == CanonicalTypeId::uuid ||
       IsTemporal(source_type_id) || source_type_id == CanonicalTypeId::interval || IsDocument(source_type_id)) &&
      IsCharacter(target_type_id)) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if (IsCharacter(source_type_id) &&
      (IsNumeric(target_type_id) || target_type_id == CanonicalTypeId::boolean || target_type_id == CanonicalTypeId::uuid ||
       IsTemporal(target_type_id) || target_type_id == CanonicalTypeId::interval || IsDocument(target_type_id))) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if ((IsBinaryLike(source_type_id) && IsCharacter(target_type_id)) ||
      (IsCharacter(source_type_id) && IsBinaryLike(target_type_id)) ||
      (IsBinaryLike(source_type_id) && IsBinaryLike(target_type_id)) ||
      (source_type_id == CanonicalTypeId::uuid && target_type_id == CanonicalTypeId::binary) ||
      (source_type_id == CanonicalTypeId::binary && target_type_id == CanonicalTypeId::uuid)) {
    return DatatypeCastCategory::lossless_explicit;
  }
  if (source_type_id == CanonicalTypeId::set_value || target_type_id == CanonicalTypeId::set_value) {
    return source_type_id == target_type_id ? DatatypeCastCategory::identity : DatatypeCastCategory::lossless_explicit;
  }
  if (reference_compatibility_profile) { return DatatypeCastCategory::reference_compatibility_explicit; }
  return DatatypeCastCategory::forbidden;
}

DatatypeCastResult CastDatatypeValue(const DatatypeCastRequest& request) {
  DatatypeCastResult result;
  result.category = ClassifyDatatypeCast(request.value.type_id, request.target_type_id, request.reference_compatibility_profile);
  if (result.category == DatatypeCastCategory::forbidden) {
    return CastFailure(std::string(CanonicalTypeName(request.value.type_id)) + "->" + CanonicalTypeName(request.target_type_id));
  }
  if (!request.explicit_cast && result.category != DatatypeCastCategory::identity &&
      result.category != DatatypeCastCategory::lossless_implicit) {
    return CastFailure("explicit_cast_required", result.category);
  }
  result.status = OkStatus();
  result.value.type_id = request.target_type_id;
  result.value.is_null = request.value.is_null;
  result.value.encoded_value = request.value.encoded_value;
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.is_null) { return result; }

  const std::string value = request.value.encoded_value;
  if (request.value.type_id == CanonicalTypeId::uuid && request.target_type_id == CanonicalTypeId::binary) {
    bool ok = false;
    result.value.encoded_value = UuidTextToBytes(value, &ok);
    if (!ok) { return CastFailure("uuid_invalid", result.category); }
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::binary && request.target_type_id == CanonicalTypeId::uuid) {
    result.value.encoded_value = BytesToUuidText(value);
    if (result.value.encoded_value.empty()) { return CastFailure("uuid_binary_length_invalid", result.category); }
    return result;
  }
  if (IsBinaryLike(request.value.type_id) && request.target_type_id == CanonicalTypeId::character) {
    result.value.encoded_value = HexEncodeLower(value);
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::character && IsBinaryLike(request.target_type_id)) {
    bool ok = false;
    result.value.encoded_value = HexDecodeStrict(value, true, &ok);
    if (!ok) { return CastFailure("binary_hex_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::character) { return result; }
  if (request.target_type_id == CanonicalTypeId::decimal) {
    DatatypeNumericContext context;
    context.precision = 38;
    context.scale = 0;
    if (!CanonicalizeExactDecimalText(value, context, &result.value.encoded_value)) {
      return CastFailure("decimal_invalid_or_out_of_range", result.category);
    }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::decimal_float) {
    DatatypeNumericContext context;
    context.precision = 34;
    context.allow_special_values = true;
    if (!CanonicalizeDecimalFloatText(value, context, &result.value.encoded_value)) {
      return CastFailure("decimal_float_invalid_or_out_of_range", result.category);
    }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::real128) {
    namespace numeric = scratchbird::libraries::sbl_numeric;
    numeric::NumericRequest backend_request;
    backend_request.type = numeric::NumericType::real128;
    backend_request.operation = numeric::NumericOperation::canonicalize;
    backend_request.left = {numeric::NumericType::real128, value, false};
    const auto backend_result = numeric::ApplyNumericOperation(backend_request);
    if (backend_result.status != numeric::NumericStatusCode::ok) {
      return CastFailure(backend_result.diagnostic_code.empty()
                             ? "real128_invalid_or_out_of_range"
                             : backend_result.diagnostic_code,
                         result.category);
    }
    result.value.encoded_value = backend_result.value.encoded;
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::int128 ||
      request.target_type_id == CanonicalTypeId::uint128) {
    if (!IsInteger(request.value.type_id) && !IsCharacter(request.value.type_id)) {
      return CastFailure("numeric_source_required", result.category);
    }
    namespace numeric = scratchbird::libraries::sbl_numeric;
    const auto backend_type = request.target_type_id == CanonicalTypeId::int128
                                  ? numeric::NumericType::int128
                                  : numeric::NumericType::uint128;
    numeric::NumericRequest backend_request;
    backend_request.type = backend_type;
    backend_request.operation = numeric::NumericOperation::canonicalize;
    backend_request.left = {backend_type, value, false};
    const auto backend_result = numeric::ApplyNumericOperation(backend_request);
    if (backend_result.status != numeric::NumericStatusCode::ok) {
      return CastFailure(backend_result.diagnostic_code.empty()
                             ? "integer_out_of_range_or_invalid"
                             : backend_result.diagnostic_code,
                         result.category);
    }
    result.value.encoded_value = backend_result.value.encoded;
    return result;
  }
  if (IsInteger(request.target_type_id)) {
    if (IsInteger(request.value.type_id) || IsCharacter(request.value.type_id)) {
      if (!IntegerFits(request.target_type_id, value)) { return CastFailure("integer_out_of_range_or_invalid", result.category); }
      result.value.encoded_value = TrimLeadingZeros(value);
      return result;
    }
    return CastFailure("numeric_source_required", result.category);
  }
  if (IsReal(request.target_type_id)) {
    if (!FloatingText(value)) { return CastFailure("real_or_decimal_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::boolean) {
    const std::string lower = LowerAscii(value);
    if (lower == "true" || lower == "1") {
      result.value.encoded_value = "true";
      return result;
    }
    if (lower == "false" || lower == "0") {
      result.value.encoded_value = "false";
      return result;
    }
    return CastFailure("boolean_invalid", result.category);
  }
  if (request.target_type_id == CanonicalTypeId::uuid) {
    if (!IsUuidText(value)) { return CastFailure("uuid_invalid", result.category); }
    result.value.encoded_value = LowerAscii(value);
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::date) {
    if (!DateText(value)) { return CastFailure("date_invalid", result.category); }
    result.value.encoded_value = value.substr(0, 10);
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::time) {
    if (!TimeText(value)) { return CastFailure("time_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::timestamp) {
    if (!TimestampText(value)) { return CastFailure("timestamp_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::interval) {
    if (!StartsWith(value, "P") && !DecimalIntegerText(value, true)) { return CastFailure("interval_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::set_value && !StartsWith(value, "SBSET1;")) {
    return CastFailure("set_encoding_invalid", result.category);
  }
  if (request.target_type_id == CanonicalTypeId::json_document ||
      request.target_type_id == CanonicalTypeId::binary_json_document ||
      request.target_type_id == CanonicalTypeId::bson_document ||
      request.target_type_id == CanonicalTypeId::object_document ||
      request.target_type_id == CanonicalTypeId::flattened_object_document ||
      request.target_type_id == CanonicalTypeId::document) {
    if (!JsonText(value)) { return CastFailure("json_document_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::xml_document) {
    if (!XmlText(value)) { return CastFailure("xml_document_invalid", result.category); }
    return result;
  }
  if (request.target_type_id == CanonicalTypeId::hstore_document) {
    return CastFailure("hstore_document_requires_domain_or_reference_wire_profile", result.category);
  }
  return result;
}

DatatypeExtractResult ExtractDatatypeField(const DatatypeExtractRequest& request) {
  DatatypeExtractResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.is_null) {
    result.value = {CanonicalTypeId::null_type, {}, true};
    return result;
  }
  if (IsOpaqueRenderOnly(request.value.type_id)) {
    return ExtractFailure("opaque_extract_rejected");
  }
  const std::string field = LowerAscii(request.field);
  const std::string value = request.value.encoded_value;
  if (IsTemporal(request.value.type_id)) {
    const std::string component = ExtractTemporalComponent(value, field);
    if (component.empty()) { return ExtractFailure("unsupported_temporal_field:" + field); }
    result.value = {CanonicalTypeId::int32, TrimLeadingZeros(component), false};
    return result;
  }
  if (request.value.type_id == CanonicalTypeId::interval) {
    if (field == "seconds" || field == "total_seconds") {
      result.value = {CanonicalTypeId::int64, value, false};
      return result;
    }
    return ExtractFailure("unsupported_interval_field:" + field);
  }
  if (request.value.type_id == CanonicalTypeId::uuid) {
    if (!IsUuidText(value)) { return ExtractFailure("uuid_invalid"); }
    if (field == "version") {
      result.value = {CanonicalTypeId::uint8, std::string(1, value[14]), false};
      return result;
    }
    if (field == "uuidv7_unix_millis") {
      if (value[14] != '7') { return ExtractFailure("uuid_not_v7"); }
      std::string compact;
      for (char c : value) {
        if (c != '-') { compact.push_back(c); }
      }
      result.value = {CanonicalTypeId::uint64, std::to_string(HexToU64(std::string_view(compact).substr(0, 12))), false};
      return result;
    }
    return ExtractFailure("unsupported_uuid_field:" + field);
  }
  if (request.value.type_id == CanonicalTypeId::character) {
    if (field == "character_length" || field == "length") {
      result.value = UInt64Value(value.size());
      return result;
    }
    if (field == "octet_length") {
      result.value = UInt64Value(value.size());
      return result;
    }
    return ExtractFailure("unsupported_text_field:" + field);
  }
  if (request.value.type_id == CanonicalTypeId::binary || request.value.type_id == CanonicalTypeId::blob) {
    if (field == "octet_length" || field == "length") {
      result.value = UInt64Value(value.size());
      return result;
    }
    return ExtractFailure("unsupported_binary_field:" + field);
  }
  if (request.value.type_id == CanonicalTypeId::set_value && field == "cardinality") {
    std::vector<std::string> values;
    if (!ParseEncodedSet(value, &values)) { return ExtractFailure("set_encoding_invalid"); }
    result.value = UInt64Value(values.size());
    return result;
  }
  return ExtractFailure("unsupported_extract_type:" + std::string(CanonicalTypeName(request.value.type_id)));
}

DatatypeSetOperationResult EncodeSetValue(const DatatypeSetDescriptor& descriptor,
                                          const std::vector<DatatypeOperationValue>& values) {
  if (descriptor.element_type_id == CanonicalTypeId::unknown) { return SetFailure("set_element_descriptor_required"); }
  std::vector<std::string> encoded_items;
  std::set<std::string> unique_items;
  for (const auto& value : values) {
    if (value.is_null && !descriptor.allow_null_elements) { return SetFailure("set_null_element_forbidden"); }
    if (!value.is_null && value.type_id != descriptor.element_type_id) { return SetFailure("set_element_type_mismatch"); }
    const std::string encoded = value.is_null ? "<NULL>" : value.encoded_value;
    if (!descriptor.allow_duplicates) {
      if (!unique_items.insert(encoded).second) { continue; }
    }
    encoded_items.push_back(encoded);
  }
  if (!descriptor.ordered) { std::sort(encoded_items.begin(), encoded_items.end()); }
  std::ostringstream out;
  out << "SBSET1;element=" << CanonicalTypeName(descriptor.element_type_id)
      << ";ordered=" << (descriptor.ordered ? "1" : "0")
      << ";nulls=" << (descriptor.allow_null_elements ? "1" : "0")
      << ";duplicates=" << (descriptor.allow_duplicates ? "1" : "0")
      << ";items=";
  for (std::size_t i = 0; i < encoded_items.size(); ++i) {
    if (i != 0) { out << ","; }
    out << HexEncode(encoded_items[i]);
  }
  DatatypeSetOperationResult result;
  result.status = OkStatus();
  result.encoded_set = out.str();
  result.value = {CanonicalTypeId::set_value, result.encoded_set, false};
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  return result;
}

DatatypeSetOperationResult ApplySetOperation(const DatatypeSetOperationRequest& request) {
  if (IsOpaqueRenderOnly(request.descriptor.element_type_id)) {
    return SetFailure("opaque_set_operation_rejected");
  }
  std::vector<std::string> left;
  if (!ParseEncodedSet(request.left_encoded_set, &left)) { return SetFailure("left_set_encoding_invalid"); }
  DatatypeSetOperationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.operation == DatatypeSetOperationKind::cardinality) {
    result.value = UInt64Value(left.size());
    return result;
  }
  std::vector<std::string> right;
  if (request.operation == DatatypeSetOperationKind::membership) {
    result.value = BoolValue(std::find(left.begin(), left.end(), request.right_encoded_set_or_value) != left.end());
    return result;
  }
  if (!ParseEncodedSet(request.right_encoded_set_or_value, &right)) { return SetFailure("right_set_encoding_invalid"); }
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  if (request.operation == DatatypeSetOperationKind::equals) {
    result.value = BoolValue(left == right);
    return result;
  }
  if (request.operation == DatatypeSetOperationKind::subset) {
    result.value = BoolValue(std::includes(right.begin(), right.end(), left.begin(), left.end()));
    return result;
  }
  if (request.operation == DatatypeSetOperationKind::superset) {
    result.value = BoolValue(std::includes(left.begin(), left.end(), right.begin(), right.end()));
    return result;
  }
  return SetFailure("unknown_set_operation");
}

DatatypeNumericOperationResult ApplyNumericOperation(const DatatypeNumericOperationRequest& request) {
  namespace numeric = scratchbird::libraries::sbl_numeric;

  DatatypeNumericOperationResult result;
  result.status = OkStatus();
  result.value = {request.type_id, {}, false};
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");

  numeric::NumericRequest backend_request;
  backend_request.left.is_null = request.left.is_null;
  backend_request.left.encoded = request.left.encoded_value;
  backend_request.right.is_null = request.right.is_null;
  backend_request.right.encoded = request.right.encoded_value;
  backend_request.context.precision = request.context.precision;
  backend_request.context.scale = request.context.scale;
  backend_request.context.allow_special_values = request.context.allow_special_values;
  switch (request.context.rounding) {
    case DatatypeRoundingMode::half_even: backend_request.context.rounding = numeric::RoundingMode::half_even; break;
    case DatatypeRoundingMode::half_up: backend_request.context.rounding = numeric::RoundingMode::half_up; break;
    case DatatypeRoundingMode::truncate: backend_request.context.rounding = numeric::RoundingMode::truncate; break;
  }
  switch (request.operation) {
    case DatatypeNumericOperationKind::canonicalize: backend_request.operation = numeric::NumericOperation::canonicalize; break;
    case DatatypeNumericOperationKind::add: backend_request.operation = numeric::NumericOperation::add; break;
    case DatatypeNumericOperationKind::subtract: backend_request.operation = numeric::NumericOperation::subtract; break;
    case DatatypeNumericOperationKind::multiply: backend_request.operation = numeric::NumericOperation::multiply; break;
    case DatatypeNumericOperationKind::divide: backend_request.operation = numeric::NumericOperation::divide; break;
    case DatatypeNumericOperationKind::compare: backend_request.operation = numeric::NumericOperation::compare; break;
  }
  switch (request.type_id) {
    case CanonicalTypeId::int128:
      backend_request.type = numeric::NumericType::int128;
      break;
    case CanonicalTypeId::uint128:
      backend_request.type = numeric::NumericType::uint128;
      break;
    case CanonicalTypeId::decimal:
      backend_request.type = numeric::NumericType::decimal;
      break;
    case CanonicalTypeId::decimal_float:
      backend_request.type = numeric::NumericType::decimal_float;
      break;
    case CanonicalTypeId::real128:
      backend_request.type = numeric::NumericType::real128;
      break;
    default:
      return NumericFailure("unsupported_numeric_type");
  }
  backend_request.left.type = backend_request.type;
  backend_request.right.type = backend_request.type;

  const numeric::NumericResult backend_result = numeric::ApplyNumericOperation(backend_request);
  if (backend_result.status == numeric::NumericStatusCode::null_result) {
    result.value = {CanonicalTypeId::null_type, {}, true};
    return result;
  }
  if (backend_result.status != numeric::NumericStatusCode::ok) {
    return NumericFailure(backend_result.diagnostic_code.empty() ? "numeric_backend_failed" : backend_result.diagnostic_code);
  }
  result.comparison = backend_result.comparison;
  if (request.operation == DatatypeNumericOperationKind::compare) {
    result.value = BoolValue(result.comparison == 0);
    return result;
  }
  result.value.encoded_value = backend_result.value.encoded;
  return result;
}

DatatypeComparisonResult CompareDatatypeValues(const DatatypeComparisonRequest& request) {
  DatatypeComparisonResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");

  if (request.left.is_null || request.right.is_null) {
    if (request.left.is_null && request.right.is_null) {
      result.comparison = 0;
    } else {
      const int null_compare = request.null_ordering == DatatypeNullOrdering::nulls_first ? -1 : 1;
      result.comparison = request.left.is_null ? null_compare : -null_compare;
    }
    return result;
  }
  if (request.left.type_id != request.right.type_id) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_COMPARISON_REJECTED",
                                                        "datatype.comparison.rejected",
                                                        "type_mismatch");
    return result;
  }
  if (IsOpaqueRenderOnly(request.left.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_COMPARISON_REJECTED",
                                                        "datatype.comparison.rejected",
                                                        "opaque_comparison_rejected");
    return result;
  }
  std::string left = request.left.encoded_value;
  std::string right = request.right.encoded_value;
  if (request.left.type_id == CanonicalTypeId::character &&
      request.case_insensitive_character_compare) {
    if (!request.text_seed.active || request.text_seed.collation_name.empty()) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          "collation_resource_missing");
      return result;
    }
    left = LowerAscii(std::move(left));
    right = LowerAscii(std::move(right));
  }
  if (IsInteger(request.left.type_id)) {
    const bool left_negative = !left.empty() && left.front() == '-';
    const bool right_negative = !right.empty() && right.front() == '-';
    if (left_negative != right_negative) {
      result.comparison = left_negative ? -1 : 1;
      return result;
    }
    result.comparison = CompareUnsignedDecimal(AbsoluteDecimal(left), AbsoluteDecimal(right));
    if (left_negative) {
      result.comparison = -result.comparison;
    }
    return result;
  }
  if (request.left.type_id == CanonicalTypeId::decimal ||
      request.left.type_id == CanonicalTypeId::decimal_float) {
    if (!CompareFiniteDecimalText(left, right, &result.comparison)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_COMPARISON_REJECTED",
          "datatype.comparison.rejected",
          "numeric_comparison_invalid");
    }
    return result;
  }
  if (IsReal(request.left.type_id)) {
    const long double left_value = std::strtold(left.c_str(), nullptr);
    const long double right_value = std::strtold(right.c_str(), nullptr);
    result.comparison = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    return result;
  }
  result.comparison = left < right ? -1 : (left > right ? 1 : 0);
  return result;
}

std::string OrderedIntegerKey(const std::string& value) {
  const bool negative = !value.empty() && value.front() == '-';
  std::string magnitude = AbsoluteDecimal(value);
  if (magnitude.size() > 40) {
    magnitude = magnitude.substr(magnitude.size() - 40);
  }
  magnitude.insert(magnitude.begin(), 40 - magnitude.size(), '0');
  if (!negative) {
    return "1" + magnitude;
  }
  for (char& digit : magnitude) {
    digit = static_cast<char>('9' - (digit - '0'));
  }
  return "0" + magnitude;
}

std::string FixedWidthUnsigned(std::size_t value, std::size_t width) {
  std::string out = std::to_string(value);
  if (out.size() < width) {
    out.insert(out.begin(), width - out.size(), '0');
  }
  return out;
}

void InvertDecimalDigits(std::string* value) {
  for (char& digit : *value) {
    digit = static_cast<char>('9' - (digit - '0'));
  }
}

struct DecimalSortParts {
  bool negative = false;
  std::string integer_part;
  std::string fractional_part;
};

bool DecimalSortPartsFromText(const std::string& value,
                              DecimalSortParts* parts) {
  ParsedDecimal parsed;
  if (!ParseDecimalFiniteText(value, &parsed)) {
    return false;
  }
  while (parsed.scale > 0 && (parsed.coefficient % 10) == 0) {
    parsed.coefficient /= 10;
    --parsed.scale;
  }

  parts->negative = parsed.negative && parsed.coefficient != 0;
  parts->integer_part = "0";
  parts->fractional_part.clear();
  if (parsed.coefficient == 0) {
    return true;
  }

  std::string digits = U128ToString(parsed.coefficient);
  if (parsed.scale == 0) {
    parts->integer_part = std::move(digits);
    return true;
  }
  const auto scale = static_cast<std::size_t>(parsed.scale);
  if (digits.size() <= scale) {
    parts->integer_part = "0";
    parts->fractional_part.assign(scale - digits.size(), '0');
    parts->fractional_part += digits;
  } else {
    const std::size_t integer_digits = digits.size() - scale;
    parts->integer_part = digits.substr(0, integer_digits);
    parts->fractional_part = digits.substr(integer_digits);
  }
  while (parts->integer_part.size() > 1 && parts->integer_part.front() == '0') {
    parts->integer_part.erase(parts->integer_part.begin());
  }
  while (!parts->fractional_part.empty() &&
         parts->fractional_part.back() == '0') {
    parts->fractional_part.pop_back();
  }
  return true;
}

std::string OrderedFiniteDecimalKey(const std::string& value) {
  DecimalSortParts parts;
  if (!DecimalSortPartsFromText(value, &parts)) {
    return {};
  }
  if (parts.integer_part == "0" && parts.fractional_part.empty()) {
    return "1:zero";
  }

  static constexpr std::size_t kIntegerLengthWidth = 6;
  static constexpr std::size_t kMaxEncodedIntegerLength = 999999;
  static constexpr std::size_t kNegativeFractionWidth = 10000;
  const std::size_t integer_length =
      std::min(parts.integer_part.size(), kMaxEncodedIntegerLength);
  if (!parts.negative) {
    return "2:" + FixedWidthUnsigned(integer_length, kIntegerLengthWidth) +
           ":" + parts.integer_part + ":" + parts.fractional_part;
  }

  std::string inverted_integer = parts.integer_part;
  InvertDecimalDigits(&inverted_integer);
  std::string inverted_fraction = parts.fractional_part;
  if (inverted_fraction.size() < kNegativeFractionWidth) {
    inverted_fraction.append(kNegativeFractionWidth - inverted_fraction.size(),
                             '0');
  }
  if (inverted_fraction.size() > kNegativeFractionWidth) {
    inverted_fraction.resize(kNegativeFractionWidth);
  }
  InvertDecimalDigits(&inverted_fraction);
  return "0:" + FixedWidthUnsigned(kMaxEncodedIntegerLength - integer_length,
                                   kIntegerLengthWidth) +
         ":" + inverted_integer + ":" + inverted_fraction;
}

bool CanonicalHashPayload(const DatatypeOperationValue& value,
                          std::string* payload,
                          std::string* failure_detail) {
  if (value.is_null) {
    payload->clear();
    return true;
  }
  if (IsInteger(value.type_id)) {
    if (!IntegerFits(value.type_id, value.encoded_value)) {
      *failure_detail = "integer_hash_value_invalid";
      return false;
    }
    *payload = TrimLeadingZeros(value.encoded_value);
    return true;
  }
  if (value.type_id == CanonicalTypeId::decimal) {
    ParsedDecimal parsed;
    if (!ParseDecimalFiniteText(value.encoded_value, &parsed)) {
      *failure_detail = "decimal_hash_value_invalid";
      return false;
    }
    while (parsed.scale > 0 && (parsed.coefficient % 10) == 0) {
      parsed.coefficient /= 10;
      --parsed.scale;
    }
    *payload = RenderFixedDecimal(parsed);
    return true;
  }
  if (value.type_id == CanonicalTypeId::decimal_float) {
    DatatypeNumericContext context;
    context.precision = 38;
    context.allow_special_values = true;
    if (!CanonicalizeDecimalFloatText(value.encoded_value, context, payload)) {
      *failure_detail = "decimal_float_hash_value_invalid";
      return false;
    }
    return true;
  }
  if (IsReal(value.type_id)) {
    if (!CanonicalizeReal128Text(value.encoded_value, payload)) {
      *failure_detail = "real_hash_value_invalid";
      return false;
    }
    return true;
  }
  *payload = value.encoded_value;
  return true;
}

DatatypeSortKeyResult MakeDatatypeSortKey(const DatatypeSortKeyRequest& request) {
  DatatypeSortKeyResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.is_null) {
    result.sort_key =
        request.null_ordering == DatatypeNullOrdering::nulls_first ? "00:null" : "ff:null";
    return result;
  }
  if (IsOpaqueRenderOnly(request.value.type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_SORT_KEY_REJECTED",
                                                        "datatype.sort_key.rejected",
                                                        "opaque_sort_key_rejected");
    return result;
  }
  std::string value = request.value.encoded_value;
  if (request.value.type_id == CanonicalTypeId::character) {
    if (request.case_insensitive_character_compare) {
      if (!request.text_seed.active || request.text_seed.collation_name.empty()) {
        result.status = ErrorStatus();
        result.diagnostic = MakeDatatypeOperationDiagnostic(
            result.status,
            "SB_DATATYPE_SORT_KEY_REJECTED",
            "datatype.sort_key.rejected",
            "collation_resource_missing");
        return result;
      }
      value = LowerAscii(std::move(value));
    }
    result.sort_key = "20:" + request.text_seed.seed_pack_name + ":" +
                      request.text_seed.seed_pack_version + ":" +
                      request.text_seed.collation_name + ":" + HexEncodeLower(value);
    return result;
  }
  if (IsInteger(request.value.type_id)) {
    result.sort_key = "10:" + OrderedIntegerKey(value);
    return result;
  }
  if (IsNumeric(request.value.type_id)) {
    const ParsedSpecialNumeric special = ParseSpecialNumericText(value);
    if (special.kind == NumericSpecialKind::negative_infinity) {
      result.sort_key = "11:00:-inf";
      return result;
    }
    if (special.kind == NumericSpecialKind::positive_infinity) {
      result.sort_key = "11:fe:+inf";
      return result;
    }
    if (special.kind == NumericSpecialKind::quiet_nan ||
        special.kind == NumericSpecialKind::signaling_nan) {
      result.sort_key = "11:ff:nan";
      return result;
    }
    const std::string ordered = OrderedFiniteDecimalKey(value);
    if (ordered.empty()) {
      result.status = ErrorStatus();
      result.diagnostic = MakeDatatypeOperationDiagnostic(
          result.status,
          "SB_DATATYPE_SORT_KEY_REJECTED",
          "datatype.sort_key.rejected",
          "numeric_sort_key_invalid");
      return result;
    }
    result.sort_key = "11:" + ordered;
    return result;
  }
  if (IsTemporal(request.value.type_id) || request.value.type_id == CanonicalTypeId::interval) {
    result.sort_key = "30:" + value;
    return result;
  }
  result.sort_key = "40:" + HexEncodeLower(value);
  return result;
}

DatatypeHashResult HashDatatypeValue(const DatatypeHashRequest& request) {
  DatatypeHashResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  std::uint64_t hash = 1469598103934665603ull;
  auto mix = [&hash](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(static_cast<std::uint64_t>(request.value.type_id));
  mix(request.value.is_null ? 1u : 0u);
  std::string payload;
  std::string failure_detail;
  if (!CanonicalHashPayload(request.value, &payload, &failure_detail)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(
        result.status,
        "SB_DATATYPE_HASH_REJECTED",
        "datatype.hash.rejected",
        failure_detail);
    return result;
  }
  for (unsigned char c : payload) {
    mix(c);
  }
  static constexpr char kHex[] = "0123456789abcdef";
  result.stable_hash_hex.resize(16);
  for (int index = 15; index >= 0; --index) {
    result.stable_hash_hex[index] = kHex[hash & 0x0fu];
    hash >>= 4;
  }
  return result;
}

DatatypeSerializationResult SerializeDatatypeValue(
    const DatatypeSerializationRequest& request) {
  DatatypeSerializationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (request.value.type_id == CanonicalTypeId::unknown) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_SERIALIZATION_REJECTED",
                                                        "datatype.serialization.rejected",
                                                        "unknown_type");
    return result;
  }
  result.serialized_value = std::string("SBDV1;type=") +
                            CanonicalTypeName(request.value.type_id) +
                            ";state=" + (request.value.is_null ? "null" : "value") +
                            ";payload=" + HexEncodeLower(request.value.encoded_value);
  return result;
}

DatatypeDeserializationResult DeserializeDatatypeValue(
    const DatatypeDeserializationRequest& request) {
  DatatypeDeserializationResult result;
  result.status = OkStatus();
  result.diagnostic = MakeDatatypeOperationDiagnostic(result.status, "SB_DATATYPE_OK", "datatype.ok");
  if (!StartsWith(request.serialized_value, "SBDV1;")) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_DESERIALIZATION_REJECTED",
                                                        "datatype.deserialization.rejected",
                                                        "bad_magic");
    return result;
  }
  std::map<std::string, std::string> fields;
  for (const auto& part : Split(request.serialized_value.substr(6), ';')) {
    const auto equals = part.find('=');
    if (equals == std::string::npos) { continue; }
    fields[part.substr(0, equals)] = part.substr(equals + 1);
  }
  const CanonicalTypeId type_id = CanonicalTypeIdFromStableName(fields["type"]);
  if (type_id == CanonicalTypeId::unknown ||
      (request.expected_type_id != CanonicalTypeId::unknown &&
       request.expected_type_id != type_id)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_DESERIALIZATION_REJECTED",
                                                        "datatype.deserialization.rejected",
                                                        "type_mismatch");
    return result;
  }
  bool ok = false;
  result.value.type_id = type_id;
  result.value.is_null = fields["state"] == "null";
  result.value.encoded_value = result.value.is_null ? std::string() :
                               HexDecodeStrict(fields["payload"], false, &ok);
  if (!result.value.is_null && !ok) {
    result.status = ErrorStatus();
    result.diagnostic = MakeDatatypeOperationDiagnostic(result.status,
                                                        "SB_DATATYPE_DESERIALIZATION_REJECTED",
                                                        "datatype.deserialization.rejected",
                                                        "payload_hex_invalid");
  }
  return result;
}

CanonicalTypeId CanonicalTypeIdFromStableName(const std::string& stable_name) {
  const std::string lower = LowerAscii(stable_name);
  if (lower == "string" || lower == "varchar" || lower == "char" || lower == "text") { return CanonicalTypeId::character; }
  if (lower == "integer") { return CanonicalTypeId::int32; }
  if (lower == "bigint") { return CanonicalTypeId::int64; }
  if (lower == "smallint") { return CanonicalTypeId::int16; }
  if (lower == "double" || lower == "double_precision") { return CanonicalTypeId::real64; }
  if (lower == "float") { return CanonicalTypeId::real32; }
  if (lower == "set") { return CanonicalTypeId::set_value; }
  for (const auto& descriptor : BuiltinDatatypeDescriptors()) {
    if (LowerAscii(descriptor.stable_name) == lower || LowerAscii(CanonicalTypeName(descriptor.type_id)) == lower) {
      return descriptor.type_id;
    }
  }
  return CanonicalTypeId::unknown;
}

bool IsUuidText(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
    } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

DiagnosticRecord MakeDatatypeOperationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) { arguments.push_back({"detail", std::move(detail)}); }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.operations");
}

}  // namespace scratchbird::core::datatypes
