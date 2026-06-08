// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBL_NUMERIC_MANDATORY_BACKEND_IMPLEMENTATION

#include "sbl_numeric.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>

#if defined(SBL_NUMERIC_HAS_QUADMATH) && SBL_NUMERIC_HAS_QUADMATH
#include <quadmath.h>
#endif

namespace scratchbird::libraries::sbl_numeric {
namespace {

using boost::multiprecision::cpp_int;

struct ParsedDecimal {
  bool negative = false;
  bool negative_zero = false;
  cpp_int coefficient = 0;
  std::uint32_t scale = 0;
};

enum class SpecialKind {
  finite,
  quiet_nan,
  signaling_nan,
  positive_infinity,
  negative_infinity
};

struct SpecialValue {
  SpecialKind kind = SpecialKind::finite;
  std::string canonical;
};

cpp_int SignedMin128() {
  return -(cpp_int(1) << 127);
}

cpp_int SignedMax128() {
  return (cpp_int(1) << 127) - 1;
}

cpp_int UnsignedMax128() {
  return (cpp_int(1) << 128) - 1;
}

NumericResult Failure(NumericStatusCode status, std::string code) {
  NumericResult result;
  result.status = status;
  result.diagnostic_code = std::move(code);
  return result;
}

std::string TrimAsciiWhitespace(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

cpp_int Pow10(std::uint32_t exponent) {
  cpp_int value = 1;
  for (std::uint32_t i = 0; i < exponent; ++i) { value *= 10; }
  return value;
}

std::uint32_t DigitCount(cpp_int value) {
  if (value < 0) { value = -value; }
  if (value == 0) { return 1; }
  std::uint32_t digits = 0;
  while (value != 0) {
    value /= 10;
    ++digits;
  }
  return digits;
}

bool ParseSpecial(std::string input, bool allow_special, SpecialValue* out) {
  input = LowerAscii(TrimAsciiWhitespace(std::move(input)));
  if (input == "nan" || input == "+nan" || input == "qnan" || input == "+qnan") {
    if (!allow_special) { return false; }
    *out = {SpecialKind::quiet_nan, "NaN"};
    return true;
  }
  if (input == "snan" || input == "+snan") {
    if (!allow_special) { return false; }
    *out = {SpecialKind::signaling_nan, "sNaN"};
    return true;
  }
  if (input == "inf" || input == "+inf" || input == "infinity" || input == "+infinity") {
    if (!allow_special) { return false; }
    *out = {SpecialKind::positive_infinity, "Infinity"};
    return true;
  }
  if (input == "-inf" || input == "-infinity") {
    if (!allow_special) { return false; }
    *out = {SpecialKind::negative_infinity, "-Infinity"};
    return true;
  }
  *out = {SpecialKind::finite, {}};
  return true;
}

bool ParseDecimal(std::string input, bool allow_exponent, ParsedDecimal* out) {
  input = TrimAsciiWhitespace(std::move(input));
  if (input.empty()) { return false; }

  std::size_t pos = 0;
  bool negative = false;
  if (input[pos] == '+' || input[pos] == '-') {
    negative = input[pos] == '-';
    ++pos;
  }
  if (pos == input.size()) { return false; }

  cpp_int coefficient = 0;
  std::uint32_t scale = 0;
  bool saw_digit = false;
  bool after_decimal = false;
  for (; pos < input.size(); ++pos) {
    const char c = input[pos];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      saw_digit = true;
      coefficient *= 10;
      coefficient += static_cast<int>(c - '0');
      if (after_decimal) { ++scale; }
      continue;
    }
    if (c == '.' && !after_decimal) {
      after_decimal = true;
      continue;
    }
    break;
  }
  if (!saw_digit) { return false; }

  if (pos < input.size()) {
    if (!allow_exponent || (input[pos] != 'e' && input[pos] != 'E')) { return false; }
    ++pos;
    bool exponent_negative = false;
    if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) {
      exponent_negative = input[pos] == '-';
      ++pos;
    }
    if (pos == input.size()) { return false; }
    std::uint32_t exponent = 0;
    for (; pos < input.size(); ++pos) {
      if (!std::isdigit(static_cast<unsigned char>(input[pos]))) { return false; }
      if (exponent > 100000) { return false; }
      exponent = (exponent * 10) + static_cast<std::uint32_t>(input[pos] - '0');
    }
    if (exponent_negative) {
      if (scale > (std::numeric_limits<std::uint32_t>::max() - exponent)) { return false; }
      scale += exponent;
    } else if (exponent >= scale) {
      coefficient *= Pow10(exponent - scale);
      scale = 0;
    } else {
      scale -= exponent;
    }
  }

  ParsedDecimal parsed;
  parsed.negative = negative && coefficient != 0;
  parsed.negative_zero = negative && coefficient == 0;
  parsed.coefficient = coefficient;
  parsed.scale = scale;
  *out = parsed;
  return true;
}

void Normalize(ParsedDecimal* value) {
  while (value->scale > 0 && (value->coefficient % 10) == 0) {
    value->coefficient /= 10;
    --value->scale;
  }
  if (value->coefficient == 0) { value->negative = false; }
}

bool RoundToScale(ParsedDecimal* value, std::uint32_t target_scale, RoundingMode rounding) {
  if (value->scale <= target_scale) {
    value->coefficient *= Pow10(target_scale - value->scale);
    value->scale = target_scale;
    return true;
  }
  const std::uint32_t drop = value->scale - target_scale;
  const cpp_int divisor = Pow10(drop);
  const cpp_int quotient = value->coefficient / divisor;
  const cpp_int remainder = value->coefficient % divisor;
  cpp_int rounded = quotient;
  if (rounding != RoundingMode::truncate && remainder != 0) {
    const cpp_int doubled = remainder * 2;
    bool increment = false;
    if (doubled > divisor) {
      increment = true;
    } else if (doubled == divisor) {
      if (rounding == RoundingMode::half_up) { increment = true; }
      if (rounding == RoundingMode::half_even && (quotient % 2) != 0) { increment = true; }
    }
    if (increment) { ++rounded; }
  }
  value->coefficient = rounded;
  value->scale = target_scale;
  if (value->coefficient == 0) { value->negative = false; }
  return true;
}

std::string CppIntToString(cpp_int value) {
  if (value == 0) { return "0"; }
  if (value < 0) { return "-" + CppIntToString(-value); }
  std::string out;
  while (value != 0) {
    const int digit = static_cast<int>(value % 10);
    out.push_back(static_cast<char>('0' + digit));
    value /= 10;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

bool ParseInteger(std::string input, bool signed_integer, cpp_int* out) {
  input = TrimAsciiWhitespace(std::move(input));
  if (input.empty()) { return false; }
  std::size_t pos = 0;
  bool negative = false;
  if (input[pos] == '+' || input[pos] == '-') {
    negative = input[pos] == '-';
    ++pos;
  }
  if (pos == input.size() || (!signed_integer && negative)) { return false; }

  cpp_int value = 0;
  for (; pos < input.size(); ++pos) {
    if (!std::isdigit(static_cast<unsigned char>(input[pos]))) { return false; }
    value *= 10;
    value += static_cast<int>(input[pos] - '0');
  }
  *out = negative ? -value : value;
  return true;
}

bool IntegerInRange(NumericType type, const cpp_int& value) {
  switch (type) {
    case NumericType::int128:
      return value >= SignedMin128() && value <= SignedMax128();
    case NumericType::uint128:
      return value >= 0 && value <= UnsignedMax128();
    default:
      return false;
  }
}

const char* IntegerRangeDiagnostic(NumericType type) {
  return type == NumericType::uint128 ? "numeric.uint128_out_of_range"
                                      : "numeric.int128_out_of_range";
}

NumericResult IntegerOperation(const NumericRequest& request) {
  const bool signed_integer = request.type == NumericType::int128;
  cpp_int left = 0;
  if (!ParseInteger(request.left.encoded, signed_integer, &left)) {
    return Failure(NumericStatusCode::invalid_left,
                   signed_integer ? "numeric.int128_left_invalid"
                                  : "numeric.uint128_left_invalid");
  }
  if (!IntegerInRange(request.type, left)) {
    return Failure(NumericStatusCode::overflow, IntegerRangeDiagnostic(request.type));
  }
  if (request.operation == NumericOperation::canonicalize) {
    NumericResult result;
    result.value = {request.type, CppIntToString(left), false};
    return result;
  }

  cpp_int right = 0;
  if (!ParseInteger(request.right.encoded, signed_integer, &right)) {
    return Failure(NumericStatusCode::invalid_right,
                   signed_integer ? "numeric.int128_right_invalid"
                                  : "numeric.uint128_right_invalid");
  }
  if (!IntegerInRange(request.type, right)) {
    return Failure(NumericStatusCode::overflow, IntegerRangeDiagnostic(request.type));
  }

  if (request.operation == NumericOperation::compare) {
    NumericResult result;
    result.comparison = left < right ? -1 : (left > right ? 1 : 0);
    result.value = {request.type, result.comparison == 0 ? "true" : "false", false};
    return result;
  }

  cpp_int out = 0;
  switch (request.operation) {
    case NumericOperation::add:
      out = left + right;
      break;
    case NumericOperation::subtract:
      out = left - right;
      break;
    case NumericOperation::multiply:
      out = left * right;
      break;
    case NumericOperation::divide:
      if (right == 0) {
        return Failure(NumericStatusCode::divide_by_zero,
                       signed_integer ? "numeric.int128_divide_by_zero"
                                      : "numeric.uint128_divide_by_zero");
      }
      out = left / right;
      break;
    case NumericOperation::canonicalize:
    case NumericOperation::compare:
      break;
  }
  if (!IntegerInRange(request.type, out)) {
    return Failure(NumericStatusCode::overflow, IntegerRangeDiagnostic(request.type));
  }
  NumericResult result;
  result.value = {request.type, CppIntToString(out), false};
  return result;
}

std::string RenderDecimal(const ParsedDecimal& value) {
  std::string digits = CppIntToString(value.coefficient);
  if (value.scale > 0) {
    if (digits.size() <= value.scale) { digits.insert(digits.begin(), value.scale - digits.size() + 1, '0'); }
    digits.insert(digits.end() - static_cast<std::ptrdiff_t>(value.scale), '.');
  }
  if ((value.negative || value.negative_zero) && digits != "0") { digits.insert(digits.begin(), '-'); }
  if (value.negative_zero && digits == "0") { return "-0"; }
  return digits;
}

cpp_int SignedInteger(const ParsedDecimal& value) {
  cpp_int signed_value = value.coefficient;
  if (value.negative) { signed_value = -signed_value; }
  return signed_value;
}

bool AlignScales(ParsedDecimal* left, ParsedDecimal* right) {
  if (left->scale == right->scale) { return true; }
  if (left->scale < right->scale) {
    left->coefficient *= Pow10(right->scale - left->scale);
    left->scale = right->scale;
  } else {
    right->coefficient *= Pow10(left->scale - right->scale);
    right->scale = left->scale;
  }
  return true;
}

NumericResult DecimalFiniteOperation(const NumericRequest& request, NumericType output_type) {
  NumericContext context = request.context;
  if (context.precision == 0) { context.precision = output_type == NumericType::decimal_float ? 34 : 38; }
  if (context.precision > 38 || context.scale > context.precision) {
    return Failure(NumericStatusCode::invalid_context, "numeric.context_invalid");
  }

  ParsedDecimal left;
  if (!ParseDecimal(request.left.encoded, true, &left)) {
    return Failure(NumericStatusCode::invalid_left, "numeric.left_invalid");
  }
  if (request.operation == NumericOperation::canonicalize) {
    if (context.canonical_preserve_scale) {
      Normalize(&left);
      if (DigitCount(left.coefficient) > context.precision) {
        return Failure(NumericStatusCode::overflow, "numeric.canonicalize_out_of_range");
      }
      NumericResult result;
      result.value = {output_type, RenderDecimal(left), false};
      return result;
    }
    if (!RoundToScale(&left, context.scale, context.rounding) || DigitCount(left.coefficient) > context.precision) {
      return Failure(NumericStatusCode::overflow, "numeric.canonicalize_out_of_range");
    }
    NumericResult result;
    result.value = {output_type, RenderDecimal(left), false};
    return result;
  }

  ParsedDecimal right;
  if (!ParseDecimal(request.right.encoded, true, &right)) {
    return Failure(NumericStatusCode::invalid_right, "numeric.right_invalid");
  }

  ParsedDecimal l = left;
  ParsedDecimal r = right;
  ParsedDecimal out;
  if (request.operation == NumericOperation::compare) {
    AlignScales(&l, &r);
    const cpp_int lv = SignedInteger(l);
    const cpp_int rv = SignedInteger(r);
    NumericResult result;
    result.comparison = lv < rv ? -1 : (lv > rv ? 1 : 0);
    result.value = {output_type, result.comparison == 0 ? "true" : "false", false};
    return result;
  }

  if (request.operation == NumericOperation::add || request.operation == NumericOperation::subtract) {
    AlignScales(&l, &r);
    cpp_int rv = SignedInteger(r);
    if (request.operation == NumericOperation::subtract) { rv = -rv; }
    cpp_int sum = SignedInteger(l) + rv;
    out.negative = sum < 0;
    if (sum < 0) { sum = -sum; }
    out.coefficient = sum;
    out.scale = l.scale;
  } else if (request.operation == NumericOperation::multiply) {
    out.negative = l.negative != r.negative;
    out.coefficient = l.coefficient * r.coefficient;
    out.scale = l.scale + r.scale;
  } else if (request.operation == NumericOperation::divide) {
    if (r.coefficient == 0) { return Failure(NumericStatusCode::divide_by_zero, "numeric.divide_by_zero"); }
    const cpp_int numerator = l.coefficient * Pow10(r.scale + context.scale + 1);
    const cpp_int denominator = r.coefficient * Pow10(l.scale);
    out.negative = l.negative != r.negative;
    out.coefficient = numerator / denominator;
    out.scale = context.scale + 1;
  } else {
    return Failure(NumericStatusCode::invalid_operation, "numeric.operation_invalid");
  }

  if (!RoundToScale(&out, context.scale, context.rounding) || DigitCount(out.coefficient) > context.precision) {
    return Failure(NumericStatusCode::overflow, "numeric.result_out_of_range");
  }
  NumericResult result;
  result.value = {output_type, RenderDecimal(out), false};
  return result;
}

NumericResult CompareSpecial(const SpecialValue& left, const SpecialValue& right, NumericType type) {
  if (left.kind == SpecialKind::quiet_nan || left.kind == SpecialKind::signaling_nan ||
      right.kind == SpecialKind::quiet_nan || right.kind == SpecialKind::signaling_nan) {
    return Failure(NumericStatusCode::unordered, "numeric.nan_unordered");
  }
  NumericResult result;
  if (left.kind == right.kind) {
    result.comparison = 0;
  } else if (left.kind == SpecialKind::positive_infinity || right.kind == SpecialKind::negative_infinity) {
    result.comparison = 1;
  } else if (left.kind == SpecialKind::negative_infinity || right.kind == SpecialKind::positive_infinity) {
    result.comparison = -1;
  } else {
    result.comparison = 0;
  }
  result.value = {type, result.comparison == 0 ? "true" : "false", false};
  return result;
}

NumericResult DecimalFloatOperation(const NumericRequest& request) {
  SpecialValue left_special;
  if (!ParseSpecial(request.left.encoded, true, &left_special)) {
    return Failure(NumericStatusCode::invalid_left, "numeric.decimal_float_left_invalid");
  }
  if (request.operation == NumericOperation::canonicalize && left_special.kind != SpecialKind::finite) {
    NumericResult result;
    result.value = {NumericType::decimal_float, left_special.canonical, false};
    return result;
  }
  if (request.operation != NumericOperation::canonicalize) {
    SpecialValue right_special;
    if (!ParseSpecial(request.right.encoded, true, &right_special)) {
      return Failure(NumericStatusCode::invalid_right, "numeric.decimal_float_right_invalid");
    }
    if (left_special.kind != SpecialKind::finite || right_special.kind != SpecialKind::finite) {
      if (request.operation == NumericOperation::compare) {
        return CompareSpecial(left_special, right_special, NumericType::decimal_float);
      }
      if (left_special.kind == SpecialKind::quiet_nan || left_special.kind == SpecialKind::signaling_nan ||
          right_special.kind == SpecialKind::quiet_nan || right_special.kind == SpecialKind::signaling_nan) {
        NumericResult result;
        result.value = {NumericType::decimal_float, "NaN", false};
        return result;
      }
      return Failure(NumericStatusCode::invalid_operation, "numeric.decimal_float_special_operation_invalid");
    }
  }
  return DecimalFiniteOperation(request, NumericType::decimal_float);
}

#if defined(SBL_NUMERIC_HAS_QUADMATH) && SBL_NUMERIC_HAS_QUADMATH
bool ParseReal128(const std::string& input, __float128* out) {
  char* end = nullptr;
  const std::string trimmed = TrimAsciiWhitespace(input);
  const __float128 value = strtoflt128(trimmed.c_str(), &end);
  if (end == trimmed.c_str() || *end != '\0') { return false; }
  *out = value;
  return true;
}

std::string RenderReal128(__float128 value) {
  char buffer[160];
  quadmath_snprintf(buffer, sizeof(buffer), "%.36Qg", value);
  std::string out(buffer);
  if (out == "nan" || out == "+nan") { return "NaN"; }
  if (out == "inf" || out == "+inf") { return "Infinity"; }
  if (out == "-inf") { return "-Infinity"; }
  return out;
}
#endif

NumericResult Real128Operation(const NumericRequest& request) {
#if defined(SBL_NUMERIC_HAS_QUADMATH) && SBL_NUMERIC_HAS_QUADMATH
  __float128 left = 0;
  if (!ParseReal128(request.left.encoded, &left)) {
    return Failure(NumericStatusCode::invalid_left, "numeric.real128_left_invalid");
  }
  if (request.operation == NumericOperation::canonicalize) {
    NumericResult result;
    result.value = {NumericType::real128, RenderReal128(left), false};
    return result;
  }
  __float128 right = 0;
  if (!ParseReal128(request.right.encoded, &right)) {
    return Failure(NumericStatusCode::invalid_right, "numeric.real128_right_invalid");
  }
  if (isnanq(left) || isnanq(right)) {
    if (request.operation == NumericOperation::compare) {
      return Failure(NumericStatusCode::unordered, "numeric.real128_nan_unordered");
    }
    NumericResult result;
    result.value = {NumericType::real128, "NaN", false};
    return result;
  }
  NumericResult result;
  result.value = {NumericType::real128, {}, false};
  switch (request.operation) {
    case NumericOperation::add: result.value.encoded = RenderReal128(left + right); return result;
    case NumericOperation::subtract: result.value.encoded = RenderReal128(left - right); return result;
    case NumericOperation::multiply: result.value.encoded = RenderReal128(left * right); return result;
    case NumericOperation::divide:
      if (right == 0) { return Failure(NumericStatusCode::divide_by_zero, "numeric.real128_divide_by_zero"); }
      result.value.encoded = RenderReal128(left / right);
      return result;
    case NumericOperation::compare:
      result.comparison = left < right ? -1 : (left > right ? 1 : 0);
      result.value.encoded = result.comparison == 0 ? "true" : "false";
      return result;
    case NumericOperation::canonicalize:
      break;
  }
  return Failure(NumericStatusCode::invalid_operation, "numeric.real128_operation_invalid");
#else
  (void)request;
  return Failure(NumericStatusCode::backend_unavailable, "numeric.real128_backend_unavailable");
#endif
}

}  // namespace

const char* NumericStatusCodeName(NumericStatusCode status) {
  switch (status) {
    case NumericStatusCode::ok: return "ok";
    case NumericStatusCode::null_result: return "null_result";
    case NumericStatusCode::invalid_context: return "invalid_context";
    case NumericStatusCode::invalid_left: return "invalid_left";
    case NumericStatusCode::invalid_right: return "invalid_right";
    case NumericStatusCode::invalid_operation: return "invalid_operation";
    case NumericStatusCode::divide_by_zero: return "divide_by_zero";
    case NumericStatusCode::overflow: return "overflow";
    case NumericStatusCode::unordered: return "unordered";
    case NumericStatusCode::backend_unavailable: return "backend_unavailable";
  }
  return "unknown";
}

const char* NumericTypeName(NumericType type) {
  switch (type) {
    case NumericType::int128: return "int128";
    case NumericType::uint128: return "uint128";
    case NumericType::decimal: return "decimal";
    case NumericType::decimal_float: return "decimal_float";
    case NumericType::real128: return "real128";
  }
  return "unknown";
}

const char* NumericOperationName(NumericOperation operation) {
  switch (operation) {
    case NumericOperation::canonicalize: return "canonicalize";
    case NumericOperation::add: return "add";
    case NumericOperation::subtract: return "subtract";
    case NumericOperation::multiply: return "multiply";
    case NumericOperation::divide: return "divide";
    case NumericOperation::compare: return "compare";
  }
  return "unknown";
}

NumericResult ApplyNumericOperation(const NumericRequest& request) {
  NumericResult result;
  result.value = {request.type, {}, false};
  if (request.left.is_null || (request.operation != NumericOperation::canonicalize && request.right.is_null)) {
    result.status = NumericStatusCode::null_result;
    result.value.is_null = true;
    return result;
  }
  switch (request.type) {
    case NumericType::int128:
    case NumericType::uint128:
      return IntegerOperation(request);
    case NumericType::decimal:
      return DecimalFiniteOperation(request, NumericType::decimal);
    case NumericType::decimal_float:
      return DecimalFloatOperation(request);
    case NumericType::real128:
      return Real128Operation(request);
  }
  return Failure(NumericStatusCode::invalid_operation, "numeric.type_invalid");
}

}  // namespace scratchbird::libraries::sbl_numeric
