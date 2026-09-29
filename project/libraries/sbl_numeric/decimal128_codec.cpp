// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <string>

namespace scratchbird::libraries::sbl_numeric {
namespace {
using boost::multiprecision::cpp_int;
constexpr std::int32_t kBias = 6176;
constexpr std::int32_t kMaximumExponent = 6111;

cpp_int Power10(unsigned power) {
  cpp_int result = 1;
  while (power--) result *= 10;
  return result;
}
cpp_int Read(const std::uint8_t* bytes) {
  cpp_int result = 0;
  for (std::size_t i = 16; i != 0; --i) { result <<= 8; result += bytes[i - 1]; }
  return result;
}
Decimal128Bytes Write(cpp_int value) {
  Decimal128Bytes result{};
  for (auto& byte : result) { byte = static_cast<std::uint8_t>(value & 255); value >>= 8; }
  return result;
}
Decimal128BinaryResult Failure(NumericStatusCode status, const char* detail) {
  Decimal128BinaryResult result;
  result.numeric.status = status;
  result.numeric.value.type = NumericType::decimal_float;
  result.numeric.diagnostic_code = detail;
  result.numeric.invalid = status == NumericStatusCode::invalid_left;
  result.numeric.overflow = status == NumericStatusCode::overflow;
  return result;
}
bool Digit(char ch) { return ch >= '0' && ch <= '9'; }
std::string Lower(std::string_view input) {
  std::string result(input);
  for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return result;
}
}  // namespace

Decimal128BinaryResult DecodeDecimal128LittleEndian(
    const std::uint8_t* bytes, std::size_t size, bool allow_special_values) {
  if (!bytes || size != 16)
    return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.width_invalid");
  const cpp_int bits = Read(bytes);
  const unsigned combination = static_cast<unsigned>((bits >> 122) & 31);
  Decimal128Value value;
  value.negative = (bytes[15] & 128) != 0;
  cpp_int coefficient;
  if (combination >= 30) {
    if (!allow_special_values)
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.special_refused");
    if (combination == 30) {
      if ((bits & ((cpp_int(1) << 122) - 1)) != 0)
        return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.infinity_noncanonical");
      value.classification = Decimal128Class::infinity;
    } else {
      value.classification = ((bits >> 121) & 1) != 0
          ? Decimal128Class::signaling_nan : Decimal128Class::quiet_nan;
      coefficient = bits & ((cpp_int(1) << 110) - 1);
      if (((bits >> 110) & 2047) != 0 || coefficient >= Power10(33))
        return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.nan_noncanonical");
    }
  } else {
    // Steering forms would imply a coefficient outside the decimal128 range.
    // Native canonical storage refuses them; it never turns corrupt bits into 0.
    if ((bytes[15] & 96) == 96)
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.finite_noncanonical");
    value.exponent = static_cast<std::int32_t>((bits >> 113) & 16383) - kBias;
    coefficient = bits & ((cpp_int(1) << 113) - 1);
    if (coefficient >= Power10(34))
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.coefficient_out_of_range");
  }
  value.coefficient = Write(coefficient);
  Decimal128BinaryResult result;
  result.numeric.value.type = NumericType::decimal_float;
  result.bytes = Write(bits);
  result.value = value;
  return result;
}

Decimal128BinaryResult EncodeDecimal128LittleEndian(std::string_view text, bool allow_special_values) {
  if (text.empty()) return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.text_invalid");
  bool negative = false;
  if (text.front() == '+' || text.front() == '-') {
    negative = text.front() == '-'; text.remove_prefix(1);
  }
  if (text.empty()) return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.text_invalid");
  cpp_int bits = negative ? cpp_int(1) << 127 : cpp_int(0);
  const auto special = Lower(text);
  if (special == "inf" || special == "infinity" || special.compare(0, 3, "nan") == 0 || special.compare(0, 4, "snan") == 0) {
    if (!allow_special_values)
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.special_refused");
    if (special == "inf" || special == "infinity") {
      bits |= cpp_int(30) << 122;
    } else {
      const bool signaling = special.front() == 's';
      const auto payload = text.substr(signaling ? 4 : 3);
      if (payload.size() > 33 || !std::all_of(payload.begin(), payload.end(), Digit))
        return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.nan_payload_invalid");
      cpp_int coefficient = 0;
      for (char digit : payload) { coefficient *= 10; coefficient += digit - '0'; }
      bits |= cpp_int(31) << 122;
      if (signaling) bits |= cpp_int(1) << 121;
      bits |= coefficient;
    }
    const auto bytes = Write(bits);
    return DecodeDecimal128LittleEndian(bytes.data(), bytes.size(), allow_special_values);
  }

  // Accumulate only significant decimal digits, not exponent-sized integers.
  // Input may have an arbitrary number of leading/trailing zeroes; overflow
  // and exactness are decided after the complete spelling has been validated.
  std::string digits;
  std::size_t fractional = 0, pos = 0;
  bool point = false, saw_digit = false;
  for (; pos < text.size(); ++pos) {
    const char ch = text[pos];
    if (Digit(ch)) {
      saw_digit = true;
      if (point) ++fractional;
      if (!digits.empty() || ch != '0') digits.push_back(ch);
    } else if (ch == '.' && !point) {
      point = true;
    } else break;
  }
  if (!saw_digit) return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.text_invalid");
  // Saturation here classifies an exponent that cannot possibly be cancelled
  // by this finite input's fractional digits. Never allocate 10^exponent.
  std::int64_t exponent = 0;
  bool exponent_negative = false, exponent_large = false;
  if (pos != text.size()) {
    if (text[pos++] != 'e' && text[pos - 1] != 'E')
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.text_invalid");
    if (pos != text.size() && (text[pos] == '+' || text[pos] == '-')) exponent_negative = text[pos++] == '-';
    if (pos == text.size()) return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.text_invalid");
    for (; pos < text.size(); ++pos) {
      if (!Digit(text[pos])) return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.text_invalid");
      if (exponent > static_cast<std::int64_t>(text.size()) + 10000) exponent_large = true;
      if (!exponent_large) exponent = exponent * 10 + text[pos] - '0';
    }
  }
  if (exponent_large) {
    if (digits.empty()) exponent = exponent_negative ? -kBias : kMaximumExponent;
    else if (exponent_negative)
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.inexact_conversion");
    else return Failure(NumericStatusCode::overflow, "numeric.decimal128.exponent_out_of_range");
  } else {
    if (exponent_negative) exponent = -exponent;
    exponent -= static_cast<std::int64_t>(fractional);
  }
  if (digits.empty()) {
    exponent = std::clamp<std::int64_t>(exponent, -kBias, kMaximumExponent);
    digits = "0";
  } else {
    while ((digits.size() > 34 || exponent < -kBias) && digits.back() == '0') {
      digits.pop_back(); ++exponent;
    }
    if (digits.size() > 34)
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.inexact_conversion");
    if (exponent < -kBias)
      return Failure(NumericStatusCode::invalid_left, "numeric.decimal128.inexact_conversion");
    if (exponent > kMaximumExponent) {
      const auto shift = exponent - kMaximumExponent;
      if (shift > static_cast<std::int64_t>(34 - digits.size()))
        return Failure(NumericStatusCode::overflow, "numeric.decimal128.exponent_out_of_range");
      digits.append(static_cast<std::size_t>(shift), '0');
      exponent = kMaximumExponent;
    }
  }
  cpp_int coefficient = 0;
  for (char digit : digits) { coefficient *= 10; coefficient += digit - '0'; }
  bits |= cpp_int(exponent + kBias) << 113;
  bits |= coefficient;
  const auto bytes = Write(bits);
  return DecodeDecimal128LittleEndian(bytes.data(), bytes.size(), allow_special_values);
}
Decimal128OrderKeyResult MakeDecimal128OrderKey(
    const std::uint8_t* bytes, std::size_t size, Decimal128OrderProfile profile,
    bool allow_special_values) {
  Decimal128OrderKeyResult result;
  if (profile != Decimal128OrderProfile::numeric_total_nan_last &&
      profile != Decimal128OrderProfile::ieee_total_order) {
    result.numeric = Failure(NumericStatusCode::invalid_context,
        "numeric.decimal128.order_profile_invalid").numeric;
    return result;
  }
  const auto decoded = DecodeDecimal128LittleEndian(bytes, size, allow_special_values);
  result.numeric = decoded.numeric;
  if (!decoded.value) return result;
  const auto& value = *decoded.value;
  const bool total = profile == Decimal128OrderProfile::ieee_total_order;
  cpp_int coefficient = Read(value.coefficient.data());
  Decimal128OrderKey key{};
  const auto put_coefficient = [&] {
    const auto magnitude = Write(coefficient);
    for (std::size_t i = 0; i < 16; ++i) key[3 + i] = magnitude[15 - i];
  };
  const auto put_quantum = [&] {
    const auto biased = static_cast<unsigned>(value.exponent + kBias);
    key[19] = static_cast<std::uint8_t>(biased >> 8);
    key[20] = static_cast<std::uint8_t>(biased);
  };
  const auto invert = [&](std::size_t end) {
    for (std::size_t i = 1; i < end; ++i) key[i] ^= 255;
  };
  if (value.classification == Decimal128Class::infinity) {
    key[0] = value.negative ? 2 : 7;
  } else if (value.classification != Decimal128Class::finite) {
    key[0] = 9;
    if (total) {
      const bool signaling = value.classification == Decimal128Class::signaling_nan;
      key[0] = value.negative ? (signaling ? 1 : 0) : (signaling ? 8 : 9);
      put_coefficient();
      if (value.negative) invert(key.size());
    }
  } else if (coefficient == 0) {
    key[0] = total && value.negative ? 4 : 5;
    if (total) {
      put_quantum();
      if (value.negative) invert(key.size());
    }
  } else {
    unsigned digits = 1;
    for (cpp_int remaining = coefficient; remaining >= 10; remaining /= 10) ++digits;
    const unsigned adjusted = static_cast<unsigned>(value.exponent + static_cast<std::int32_t>(digits) - 1 + kBias);
    key[0] = value.negative ? 3 : 6;
    key[1] = static_cast<std::uint8_t>(adjusted >> 8);
    key[2] = static_cast<std::uint8_t>(adjusted);
    coefficient *= Power10(34 - digits);
    put_coefficient();
    if (total) put_quantum();
    if (value.negative) invert(total ? key.size() : 19);
  }
  result.key = key;
  return result;
}
}  // namespace scratchbird::libraries::sbl_numeric
