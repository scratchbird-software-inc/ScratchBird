// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_typed_row_codec.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_TYPED_ROW_CODEC_AUTHORITY
// Typed row encoding and fixed-width validation only; no publication authority.

scratchbird::storage::page::RowDataCell DirectPhysicalCell(
    std::uint16_t ordinal,
    const std::string& encoded_value) {
  scratchbird::storage::page::RowDataCell cell;
  cell.column_ordinal = ordinal;
  cell.value.type_id = scratchbird::core::datatypes::CanonicalTypeId::character;
  cell.value.payload.assign(encoded_value.begin(), encoded_value.end());
  return cell;
}

void DirectAppendLittleUnsigned(std::vector<scratchbird::core::platform::byte>* out,
                                std::uint64_t value,
                                std::size_t bytes) {
  out->reserve(out->size() + bytes);
  for (std::size_t index = 0; index < bytes; ++index) {
    out->push_back(static_cast<scratchbird::core::platform::byte>(
        (value >> (index * 8)) & 0xffu));
  }
}

using DirectU128Bytes = std::array<scratchbird::core::platform::byte, 16>;

void DirectAppendLittleUnsigned128(std::vector<scratchbird::core::platform::byte>* out,
                                   const DirectU128Bytes& value) {
  out->reserve(out->size() + 16);
  out->insert(out->end(), value.begin(), value.end());
}

void DirectAppendLittleSigned(std::vector<scratchbird::core::platform::byte>* out,
                              std::int64_t value,
                              std::size_t bytes) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(value));
  DirectAppendLittleUnsigned(out, bits, bytes);
}

int DirectCompareU128Bytes(const DirectU128Bytes& left,
                           const DirectU128Bytes& right) {
  for (std::size_t reverse = left.size(); reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    if (left[index] < right[index]) { return -1; }
    if (left[index] > right[index]) { return 1; }
  }
  return 0;
}

DirectU128Bytes DirectMaxU128Bytes() {
  DirectU128Bytes value{};
  value.fill(0xff);
  return value;
}

DirectU128Bytes DirectMaxPositiveI128Bytes() {
  DirectU128Bytes value = DirectMaxU128Bytes();
  value.back() = 0x7f;
  return value;
}

DirectU128Bytes DirectMinNegativeI128MagnitudeBytes() {
  DirectU128Bytes value{};
  value.back() = 0x80;
  return value;
}

bool DirectParseUnsigned128Text(std::string_view text,
                                const DirectU128Bytes& max_value,
                                DirectU128Bytes* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  DirectU128Bytes parsed{};
  for (const char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    unsigned carry = static_cast<unsigned>(ch - '0');
    for (std::size_t index = 0; index < parsed.size(); ++index) {
      const unsigned product =
          static_cast<unsigned>(parsed[index]) * 10u + carry;
      parsed[index] = static_cast<scratchbird::core::platform::byte>(product & 0xffu);
      carry = product >> 8;
    }
    if (carry != 0 || DirectCompareU128Bytes(parsed, max_value) > 0) {
      return false;
    }
  }
  *out = parsed;
  return true;
}

DirectU128Bytes DirectTwosComplementNegativeMagnitude(DirectU128Bytes magnitude) {
  for (auto& byte : magnitude) {
    byte = static_cast<scratchbird::core::platform::byte>(~byte);
  }
  unsigned carry = 1;
  for (auto& byte : magnitude) {
    const unsigned sum = static_cast<unsigned>(byte) + carry;
    byte = static_cast<scratchbird::core::platform::byte>(sum & 0xffu);
    carry = sum >> 8;
    if (carry == 0) {
      break;
    }
  }
  return magnitude;
}

bool DirectParseSigned128Payload(std::string_view text,
                                 std::vector<scratchbird::core::platform::byte>* out) {
  if (text.empty()) {
    return false;
  }
  const bool negative = text.front() == '-';
  if (negative) {
    text.remove_prefix(1);
    if (text.empty()) {
      return false;
    }
  } else if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty()) {
      return false;
    }
  }
  const DirectU128Bytes positive_limit = DirectMaxPositiveI128Bytes();
  const DirectU128Bytes negative_limit = DirectMinNegativeI128MagnitudeBytes();
  DirectU128Bytes magnitude{};
  if (!DirectParseUnsigned128Text(text,
                                  negative ? negative_limit : positive_limit,
                                  &magnitude)) {
    return false;
  }
  const bool nonzero =
      std::any_of(magnitude.begin(), magnitude.end(),
                  [](scratchbird::core::platform::byte byte) { return byte != 0; });
  const DirectU128Bytes bits =
      negative && nonzero ? DirectTwosComplementNegativeMagnitude(magnitude)
                          : magnitude;
  DirectAppendLittleUnsigned128(out, bits);
  return true;
}

bool DirectParseUnsigned128Payload(std::string_view text,
                                   std::vector<scratchbird::core::platform::byte>* out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  DirectU128Bytes parsed{};
  if (!DirectParseUnsigned128Text(text, DirectMaxU128Bytes(), &parsed)) {
    return false;
  }
  DirectAppendLittleUnsigned128(out, parsed);
  return true;
}

bool DirectParseFixedDigits(std::string_view text,
                            std::size_t offset,
                            std::size_t count,
                            int* out) {
  if (out == nullptr || offset + count > text.size()) {
    return false;
  }
  int parsed = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char ch = text[offset + index];
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    parsed = parsed * 10 + (ch - '0');
  }
  *out = parsed;
  return true;
}

bool DirectLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DirectDaysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return DirectLeapYear(year) ? 29 : 28;
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

bool DirectParseDateParts(std::string_view text, int* year, int* month, int* day) {
  int parsed_year = 0;
  int parsed_month = 0;
  int parsed_day = 0;
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return false;
  }
  if (!DirectParseFixedDigits(text, 0, 4, &parsed_year) ||
      !DirectParseFixedDigits(text, 5, 2, &parsed_month) ||
      !DirectParseFixedDigits(text, 8, 2, &parsed_day)) {
    return false;
  }
  if (parsed_year < 1 || parsed_month < 1 || parsed_month > 12 ||
      parsed_day < 1 || parsed_day > DirectDaysInMonth(parsed_year, parsed_month)) {
    return false;
  }
  if (year != nullptr) { *year = parsed_year; }
  if (month != nullptr) { *month = parsed_month; }
  if (day != nullptr) { *day = parsed_day; }
  return true;
}

std::int64_t DirectDaysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

bool DirectParseFractionNanos(std::string_view text,
                              std::size_t* offset,
                              std::uint32_t* nanos) {
  if (offset == nullptr || nanos == nullptr) {
    return false;
  }
  *nanos = 0;
  if (*offset >= text.size() || text[*offset] != '.') {
    return true;
  }
  ++(*offset);
  const std::size_t start = *offset;
  std::uint32_t parsed = 0;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset]))) {
    if (*offset - start >= 9) {
      return false;
    }
    parsed = parsed * 10 + static_cast<std::uint32_t>(text[*offset] - '0');
    ++(*offset);
  }
  const std::size_t digits = *offset - start;
  if (digits == 0) {
    return false;
  }
  for (std::size_t index = digits; index < 9; ++index) {
    parsed *= 10;
  }
  *nanos = parsed;
  return true;
}

bool DirectParseTimeParts(std::string_view text,
                          std::uint64_t* nanos_since_midnight,
                          std::uint32_t* nanos_of_second = nullptr) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (text.size() < 8 || text[2] != ':' || text[5] != ':') {
    return false;
  }
  if (!DirectParseFixedDigits(text, 0, 2, &hour) ||
      !DirectParseFixedDigits(text, 3, 2, &minute) ||
      !DirectParseFixedDigits(text, 6, 2, &second)) {
    return false;
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return false;
  }
  std::size_t offset = 8;
  std::uint32_t nanos = 0;
  if (!DirectParseFractionNanos(text, &offset, &nanos) || offset != text.size()) {
    return false;
  }
  if (nanos_since_midnight != nullptr) {
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(hour) * 3600ull +
        static_cast<std::uint64_t>(minute) * 60ull +
        static_cast<std::uint64_t>(second);
    *nanos_since_midnight = seconds * 1000000000ull + nanos;
  }
  if (nanos_of_second != nullptr) {
    *nanos_of_second = nanos;
  }
  return true;
}

bool DirectParseDatePayload(std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (!DirectParseDateParts(text, &year, &month, &day)) {
    return false;
  }
  const std::int64_t days = DirectDaysFromCivil(
      year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  if (days < std::numeric_limits<std::int32_t>::min() ||
      days > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  DirectAppendLittleSigned(out, days, 4);
  return true;
}

bool DirectParseTimePayload(std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  std::uint64_t nanos = 0;
  if (!DirectParseTimeParts(text, &nanos)) {
    return false;
  }
  DirectAppendLittleUnsigned(out, nanos, 8);
  return true;
}

bool DirectParseTimezoneOffsetMinutes(std::string_view text,
                                      std::size_t* end,
                                      int* offset_minutes) {
  if (end == nullptr || offset_minutes == nullptr) {
    return false;
  }
  *offset_minutes = 0;
  if (*end == 0) {
    return true;
  }
  if (text[*end - 1] == 'Z' || text[*end - 1] == 'z') {
    --(*end);
    return true;
  }
  if (*end < 6) {
    return true;
  }
  const std::size_t offset = *end - 6;
  if ((text[offset] != '+' && text[offset] != '-') || text[offset + 3] != ':') {
    return true;
  }
  int hour = 0;
  int minute = 0;
  if (!DirectParseFixedDigits(text, offset + 1, 2, &hour) ||
      !DirectParseFixedDigits(text, offset + 4, 2, &minute) ||
      hour > 23 || minute > 59) {
    return false;
  }
  *offset_minutes = (hour * 60 + minute) * (text[offset] == '-' ? -1 : 1);
  *end = offset;
  return true;
}

bool DirectParseTimestampPayload(std::string_view text,
                                 std::vector<scratchbird::core::platform::byte>* out) {
  std::size_t end = text.size();
  int offset_minutes = 0;
  if (!DirectParseTimezoneOffsetMinutes(text, &end, &offset_minutes)) {
    return false;
  }
  const std::string_view local = text.substr(0, end);
  const std::size_t separator = local.find('T') == std::string_view::npos
                                    ? local.find(' ')
                                    : local.find('T');
  if (separator == std::string_view::npos) {
    return false;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  if (!DirectParseDateParts(local.substr(0, separator), &year, &month, &day)) {
    return false;
  }
  std::uint64_t nanos_since_midnight = 0;
  std::uint32_t nanos_of_second = 0;
  if (!DirectParseTimeParts(local.substr(separator + 1),
                            &nanos_since_midnight,
                            &nanos_of_second)) {
    return false;
  }
  const std::int64_t days = DirectDaysFromCivil(
      year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const std::int64_t seconds_since_midnight =
      static_cast<std::int64_t>(nanos_since_midnight / 1000000000ull);
  const std::int64_t epoch_seconds =
      days * 86400 + seconds_since_midnight -
      static_cast<std::int64_t>(offset_minutes) * 60;
  DirectAppendLittleSigned(out, epoch_seconds, 8);
  DirectAppendLittleUnsigned(out, nanos_of_second, 4);
  DirectAppendLittleSigned(out, 0, 4);
  return true;
}

bool DirectParseI64Text(std::string_view text, std::int64_t* out) {
  if (out == nullptr) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, *out, 10);
  return parse.ec == std::errc{} && parse.ptr == end;
}

bool DirectTargetIsSignedInteger(dt::CanonicalTypeId target_type);
bool DirectTargetIsUnsignedInteger(dt::CanonicalTypeId target_type);

bool DirectReadBinarySignedI64(const EngineTypedValue& typed, std::int64_t* out) {
  if (out == nullptr || typed.binary_value.empty() ||
      typed.binary_value.size() > sizeof(std::int64_t)) {
    return false;
  }
  const dt::CanonicalTypeId source_type =
      dt::CanonicalTypeIdFromStableName(typed.descriptor.canonical_type_name);
  std::uint64_t unsigned_bits = 0;
  for (std::size_t index = 0; index < typed.binary_value.size(); ++index) {
    unsigned_bits |= static_cast<std::uint64_t>(typed.binary_value[index])
                     << (index * 8u);
  }
  if (DirectTargetIsUnsignedInteger(source_type)) {
    if (unsigned_bits >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    *out = static_cast<std::int64_t>(unsigned_bits);
    return true;
  }
  if (source_type != dt::CanonicalTypeId::unknown &&
      !DirectTargetIsSignedInteger(source_type)) {
    return false;
  }
  switch (typed.binary_value.size()) {
    case 1: {
      std::int8_t value = 0;
      std::memcpy(&value, &typed.binary_value[0], sizeof(value));
      *out = static_cast<std::int64_t>(value);
      return true;
    }
    case 2: {
      std::uint16_t bits = static_cast<std::uint16_t>(unsigned_bits);
      std::int16_t value = 0;
      std::memcpy(&value, &bits, sizeof(value));
      *out = static_cast<std::int64_t>(value);
      return true;
    }
    case 4: {
      std::uint32_t bits = static_cast<std::uint32_t>(unsigned_bits);
      std::int32_t value = 0;
      std::memcpy(&value, &bits, sizeof(value));
      *out = static_cast<std::int64_t>(value);
      return true;
    }
    case 8: {
      std::int64_t value = 0;
      std::memcpy(&value, &unsigned_bits, sizeof(value));
      *out = value;
      return true;
    }
    default:
      return false;
  }
}

bool DirectSignedI64FitsBytes(std::int64_t value, std::size_t bytes) {
  switch (bytes) {
    case 1:
      return value >= std::numeric_limits<std::int8_t>::min() &&
             value <= std::numeric_limits<std::int8_t>::max();
    case 2:
      return value >= std::numeric_limits<std::int16_t>::min() &&
             value <= std::numeric_limits<std::int16_t>::max();
    case 4:
      return value >= std::numeric_limits<std::int32_t>::min() &&
             value <= std::numeric_limits<std::int32_t>::max();
    case 8:
      return true;
    default:
      return false;
  }
}

bool DirectUnsignedI64FitsBytes(std::int64_t value, std::size_t bytes) {
  if (value < 0) {
    return false;
  }
  const auto unsigned_value = static_cast<std::uint64_t>(value);
  switch (bytes) {
    case 1:
      return unsigned_value <= std::numeric_limits<std::uint8_t>::max();
    case 2:
      return unsigned_value <= std::numeric_limits<std::uint16_t>::max();
    case 4:
      return unsigned_value <= std::numeric_limits<std::uint32_t>::max();
    case 8:
      return true;
    default:
      return false;
  }
}

bool DirectTargetIsSignedInteger(dt::CanonicalTypeId target_type) {
  switch (target_type) {
    case dt::CanonicalTypeId::int8:
    case dt::CanonicalTypeId::int16:
    case dt::CanonicalTypeId::int32:
    case dt::CanonicalTypeId::int64:
      return true;
    default:
      return false;
  }
}

bool DirectTargetIsUnsignedInteger(dt::CanonicalTypeId target_type) {
  switch (target_type) {
    case dt::CanonicalTypeId::uint8:
    case dt::CanonicalTypeId::uint16:
    case dt::CanonicalTypeId::uint32:
    case dt::CanonicalTypeId::uint64:
      return true;
    default:
      return false;
  }
}

bool DirectBinaryIntegerPayloadCompatible(dt::CanonicalTypeId target_type,
                                          const EngineTypedValue& typed,
                                          std::size_t inline_bytes) {
  if (typed.binary_value.empty()) {
    return false;
  }
  if (inline_bytes != 0 && typed.binary_value.size() == inline_bytes) {
    return true;
  }
  std::int64_t parsed = 0;
  if (!DirectReadBinarySignedI64(typed, &parsed)) {
    return false;
  }
  if (DirectTargetIsSignedInteger(target_type)) {
    return DirectSignedI64FitsBytes(parsed, inline_bytes);
  }
  if (DirectTargetIsUnsignedInteger(target_type)) {
    return DirectUnsignedI64FitsBytes(parsed, inline_bytes);
  }
  return false;
}

bool DirectPackBinaryIntegerPayload(dt::CanonicalTypeId target_type,
                                    const EngineTypedValue& typed,
                                    std::size_t inline_bytes,
                                    std::vector<scratchbird::core::platform::byte>* out) {
  if (out == nullptr || !DirectBinaryIntegerPayloadCompatible(target_type,
                                                              typed,
                                                              inline_bytes)) {
    return false;
  }
  if (inline_bytes != 0 && typed.binary_value.size() == inline_bytes) {
    *out = typed.binary_value;
    return true;
  }
  std::int64_t parsed = 0;
  if (!DirectReadBinarySignedI64(typed, &parsed)) {
    return false;
  }
  if (DirectTargetIsSignedInteger(target_type)) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    DirectAppendLittleUnsigned(out, bits, inline_bytes);
    return true;
  }
  if (DirectTargetIsUnsignedInteger(target_type)) {
    DirectAppendLittleUnsigned(out,
                               static_cast<std::uint64_t>(parsed),
                               inline_bytes);
    return true;
  }
  return false;
}

bool DirectParseIntervalPayload(std::string_view text,
                                std::vector<scratchbird::core::platform::byte>* out) {
  std::int64_t seconds = 0;
  if (DirectParseI64Text(text, &seconds)) {
    if (seconds > std::numeric_limits<std::int64_t>::max() / 1000000000ll ||
        seconds < std::numeric_limits<std::int64_t>::min() / 1000000000ll) {
      return false;
    }
    DirectAppendLittleSigned(out, 0, 4);
    DirectAppendLittleSigned(out, 0, 4);
    DirectAppendLittleSigned(out, seconds * 1000000000ll, 8);
    return true;
  }
  if (text.empty() || text[0] != 'P') {
    return false;
  }
  std::size_t offset = 1;
  bool in_time = false;
  std::int64_t months = 0;
  std::int64_t days = 0;
  std::int64_t nanos = 0;
  bool consumed = false;
  while (offset < text.size()) {
    if (text[offset] == 'T') {
      if (in_time) {
        return false;
      }
      in_time = true;
      ++offset;
      continue;
    }
    const std::size_t number_start = offset;
    while (offset < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[offset]))) {
      ++offset;
    }
    if (number_start == offset || offset >= text.size()) {
      return false;
    }
    std::int64_t value = 0;
    if (!DirectParseI64Text(text.substr(number_start, offset - number_start), &value)) {
      return false;
    }
    const char designator = text[offset++];
    consumed = true;
    switch (designator) {
      case 'Y':
        if (in_time || value > std::numeric_limits<std::int64_t>::max() / 12) {
          return false;
        }
        months += value * 12;
        break;
      case 'M':
        if (in_time) {
          if (value > std::numeric_limits<std::int64_t>::max() / 60 / 1000000000ll) {
            return false;
          }
          nanos += value * 60 * 1000000000ll;
        } else {
          months += value;
        }
        break;
      case 'D':
        if (in_time) {
          return false;
        }
        days += value;
        break;
      case 'H':
        if (!in_time ||
            value > std::numeric_limits<std::int64_t>::max() / 3600 / 1000000000ll) {
          return false;
        }
        nanos += value * 3600 * 1000000000ll;
        break;
      case 'S':
        if (!in_time ||
            value > std::numeric_limits<std::int64_t>::max() / 1000000000ll) {
          return false;
        }
        nanos += value * 1000000000ll;
        break;
      default:
        return false;
    }
  }
  if (!consumed ||
      months < std::numeric_limits<std::int32_t>::min() ||
      months > std::numeric_limits<std::int32_t>::max() ||
      days < std::numeric_limits<std::int32_t>::min() ||
      days > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  DirectAppendLittleSigned(out, months, 4);
  DirectAppendLittleSigned(out, days, 4);
  DirectAppendLittleSigned(out, nanos, 8);
  return true;
}

bool DirectParseSignedIntegerPayload(std::string_view text,
                                     std::size_t bytes,
                                     std::vector<scratchbird::core::platform::byte>* out) {
  if (bytes == 16) {
    return DirectParseSigned128Payload(text, out);
  }
  std::int64_t parsed = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, parsed, 10);
  if (parse.ec != std::errc{} || parse.ptr != end) {
    return false;
  }
  switch (bytes) {
    case 1:
      if (parsed < std::numeric_limits<std::int8_t>::min() ||
          parsed > std::numeric_limits<std::int8_t>::max()) {
        return false;
      }
      break;
    case 2:
      if (parsed < std::numeric_limits<std::int16_t>::min() ||
          parsed > std::numeric_limits<std::int16_t>::max()) {
        return false;
      }
      break;
    case 4:
      if (parsed < std::numeric_limits<std::int32_t>::min() ||
          parsed > std::numeric_limits<std::int32_t>::max()) {
        return false;
      }
      break;
    case 8:
      break;
    default:
      return false;
  }
  std::uint64_t bits = 0;
  std::memcpy(&bits, &parsed, sizeof(parsed));
  DirectAppendLittleUnsigned(out, bits, bytes);
  return true;
}

bool DirectParseUnsignedIntegerPayload(std::string_view text,
                                       std::size_t bytes,
                                       std::vector<scratchbird::core::platform::byte>* out) {
  if (bytes == 16) {
    return DirectParseUnsigned128Payload(text, out);
  }
  std::uint64_t parsed = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, parsed, 10);
  if (parse.ec != std::errc{} || parse.ptr != end) {
    return false;
  }
  switch (bytes) {
    case 1:
      if (parsed > std::numeric_limits<std::uint8_t>::max()) { return false; }
      break;
    case 2:
      if (parsed > std::numeric_limits<std::uint16_t>::max()) { return false; }
      break;
    case 4:
      if (parsed > std::numeric_limits<std::uint32_t>::max()) { return false; }
      break;
    case 8:
      break;
    default:
      return false;
  }
  DirectAppendLittleUnsigned(out, parsed, bytes);
  return true;
}

bool DirectParseBooleanPayload(std::string_view text,
                               std::vector<scratchbird::core::platform::byte>* out) {
  std::string normalized(text);
  for (char& c : normalized) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (normalized == "1" || normalized == "true") {
    out->push_back(1);
    return true;
  }
  if (normalized == "0" || normalized == "false") {
    out->push_back(0);
    return true;
  }
  return false;
}

bool DirectParseFloat32Value(std::string_view text, float* out) {
  if (text.empty() || out == nullptr ||
      std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    return false;
  }
#if defined(__APPLE__)
  std::string value(text);
  char* parsed_end = nullptr;
  errno = 0;
  const float parsed = std::strtof(value.c_str(), &parsed_end);
  if (errno == ERANGE || parsed_end != value.c_str() + value.size()) { return false; }
  *out = parsed;
  return true;
#else
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, *out);
  return parse.ec == std::errc{} && parse.ptr == end;
#endif
}

bool DirectParseFloat64Value(std::string_view text, double* out) {
  if (text.empty() || out == nullptr ||
      std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    return false;
  }
#if defined(__APPLE__)
  std::string value(text);
  char* parsed_end = nullptr;
  errno = 0;
  const double parsed = std::strtod(value.c_str(), &parsed_end);
  if (errno == ERANGE || parsed_end != value.c_str() + value.size()) { return false; }
  *out = parsed;
  return true;
#else
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parse = std::from_chars(begin, end, *out);
  return parse.ec == std::errc{} && parse.ptr == end;
#endif
}

bool DirectParseRealPayload(dt::CanonicalTypeId type_id,
                            std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  if (type_id == dt::CanonicalTypeId::bfloat16 ||
      type_id == dt::CanonicalTypeId::real16) {
    float parsed = 0.0f;
    if (!DirectParseFloat32Value(text, &parsed)) { return false; }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    std::uint16_t packed = 0;
    if (type_id == dt::CanonicalTypeId::bfloat16) {
      const std::uint32_t lsb = (bits >> 16) & 1u;
      packed = static_cast<std::uint16_t>((bits + 0x7fffu + lsb) >> 16);
    } else {
      const std::uint32_t sign = (bits >> 16) & 0x8000u;
      int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
      std::uint32_t mantissa = bits & 0x7fffffu;
      if (exponent <= 0) {
        if (exponent < -10) {
          packed = static_cast<std::uint16_t>(sign);
        } else {
          mantissa = (mantissa | 0x800000u) >> static_cast<unsigned>(1 - exponent);
          packed = static_cast<std::uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
        }
      } else if (exponent >= 31) {
        packed = static_cast<std::uint16_t>(sign | 0x7c00u);
      } else {
        packed = static_cast<std::uint16_t>(
            sign | (static_cast<std::uint32_t>(exponent) << 10) |
            ((mantissa + 0x1000u) >> 13));
      }
    }
    DirectAppendLittleUnsigned(out, packed, sizeof(packed));
    return true;
  }
  if (type_id == dt::CanonicalTypeId::real32) {
    float parsed = 0.0f;
    if (!DirectParseFloat32Value(text, &parsed)) { return false; }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    DirectAppendLittleUnsigned(out, bits, sizeof(bits));
    return true;
  }
  if (type_id == dt::CanonicalTypeId::real64) {
    double parsed = 0.0;
    if (!DirectParseFloat64Value(text, &parsed)) { return false; }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(parsed));
    DirectAppendLittleUnsigned(out, bits, sizeof(bits));
    return true;
  }
  return false;
}

bool DirectParseUuidPayload(std::string_view text,
                            std::vector<scratchbird::core::platform::byte>* out) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok()) { return false; }
  out->assign(parsed.value.bytes.begin(), parsed.value.bytes.end());
  return true;
}

bool DirectPackTypedPayload(dt::CanonicalTypeId target_type,
                            const EngineTypedValue& typed,
                            std::vector<scratchbird::core::platform::byte>* out) {
  const auto layout = dt::LookupDatatypeStorageLayout(target_type);
  const std::size_t inline_bytes =
      layout.ok() ? static_cast<std::size_t>(layout.layout.inline_bytes) : 0;
  if (!typed.binary_value.empty() &&
      (inline_bytes == 0 || typed.binary_value.size() == inline_bytes)) {
    *out = typed.binary_value;
    return true;
  }
  if (DirectPackBinaryIntegerPayload(target_type, typed, inline_bytes, out)) {
    return true;
  }
  switch (target_type) {
    case dt::CanonicalTypeId::character:
      out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
      return true;
    case dt::CanonicalTypeId::boolean:
      return DirectParseBooleanPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::int8:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 1, out);
    case dt::CanonicalTypeId::int16:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 2, out);
    case dt::CanonicalTypeId::int32:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 4, out);
    case dt::CanonicalTypeId::int64:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 8, out);
    case dt::CanonicalTypeId::int128:
      return DirectParseSignedIntegerPayload(typed.encoded_value, 16, out);
    case dt::CanonicalTypeId::uint8:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 1, out);
    case dt::CanonicalTypeId::uint16:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 2, out);
    case dt::CanonicalTypeId::uint32:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 4, out);
    case dt::CanonicalTypeId::uint64:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 8, out);
    case dt::CanonicalTypeId::uint128:
      return DirectParseUnsignedIntegerPayload(typed.encoded_value, 16, out);
    case dt::CanonicalTypeId::bfloat16:
    case dt::CanonicalTypeId::real16:
    case dt::CanonicalTypeId::real32:
    case dt::CanonicalTypeId::real64:
      return DirectParseRealPayload(target_type, typed.encoded_value, out);
    case dt::CanonicalTypeId::uuid:
    case dt::CanonicalTypeId::enum_value:
      return DirectParseUuidPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::date:
      return DirectParseDatePayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::time:
      return DirectParseTimePayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::timestamp:
      return DirectParseTimestampPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::interval:
      return DirectParseIntervalPayload(typed.encoded_value, out);
    case dt::CanonicalTypeId::binary:
      out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
      return true;
    default:
      if (target_type != dt::CanonicalTypeId::unknown &&
          target_type != dt::CanonicalTypeId::character &&
          layout.ok() &&
          layout.layout.storage_class != dt::DatatypeStorageClass::inline_fixed) {
        out->assign(typed.encoded_value.begin(), typed.encoded_value.end());
        return true;
      }
      return false;
  }
}

scratchbird::storage::page::RowDataCell DirectPhysicalCellFromTypedValue(
    std::uint16_t ordinal,
    const EngineTypedValue& typed,
    std::string_view target_canonical_type_name) {
  scratchbird::storage::page::RowDataCell cell;
  cell.column_ordinal = ordinal;
  if (typed.isSqlNull()) {
    cell.value.type_id = dt::CanonicalTypeId::null_type;
    cell.value.is_null = true;
    return cell;
  }
  dt::CanonicalTypeId target_type =
      dt::CanonicalTypeIdFromStableName(std::string(target_canonical_type_name));
  if (target_type == dt::CanonicalTypeId::unknown) {
    target_type = dt::CanonicalTypeIdFromStableName(
        typed.descriptor.canonical_type_name);
  }
  if (target_type != dt::CanonicalTypeId::unknown &&
      target_type != dt::CanonicalTypeId::character) {
    std::vector<scratchbird::core::platform::byte> payload;
    if (DirectPackTypedPayload(target_type, typed, &payload)) {
      cell.value.type_id = target_type;
      cell.value.payload = std::move(payload);
      return cell;
    }
    const auto layout = dt::LookupDatatypeStorageLayout(target_type);
    if (layout.ok() &&
        layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed) {
      cell.value.type_id = target_type;
      return cell;
    }
  }
  cell.value.type_id = dt::CanonicalTypeId::character;
  cell.value.payload.assign(typed.encoded_value.begin(),
                            typed.encoded_value.end());
  return cell;
}

scratchbird::storage::page::RowDataCell DirectPhysicalCellFromTypedValueWithPlan(
    std::uint16_t ordinal,
    const EngineTypedValue& typed,
    const DirectFixedWidthPayloadValidationColumnPlan& column_plan) {
  scratchbird::storage::page::RowDataCell cell;
  cell.column_ordinal = ordinal;
  if (typed.isSqlNull()) {
    cell.value.type_id = dt::CanonicalTypeId::null_type;
    cell.value.is_null = true;
    return cell;
  }
  const dt::CanonicalTypeId target_type = column_plan.target_type;
  if (target_type != dt::CanonicalTypeId::unknown &&
      !column_plan.character_type) {
    std::vector<scratchbird::core::platform::byte> payload;
    if (column_plan.inline_fixed) {
      if (DirectPackBinaryIntegerPayload(target_type,
                                          typed,
                                          column_plan.inline_bytes,
                                          &payload) ||
          DirectPackTypedPayload(target_type, typed, &payload)) {
        cell.value.type_id = target_type;
        cell.value.payload = std::move(payload);
        return cell;
      }
      cell.value.type_id = target_type;
      return cell;
    }
    cell.value.type_id = target_type;
    if (!typed.binary_value.empty()) {
      cell.value.payload = typed.binary_value;
    } else {
      cell.value.payload.assign(typed.encoded_value.begin(),
                                typed.encoded_value.end());
    }
    return cell;
  }
  cell.value.type_id = dt::CanonicalTypeId::character;
  cell.value.payload.assign(typed.encoded_value.begin(),
                            typed.encoded_value.end());
  return cell;
}

std::vector<scratchbird::storage::page::RowDataCell> DirectPhysicalCells(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<scratchbird::storage::page::RowDataCell> cells;
  cells.reserve(values.size());
  std::uint16_t ordinal = 1;
  for (const auto& value : values) {
    cells.push_back(DirectPhysicalCell(ordinal++, value.second));
  }
  return cells;
}

DirectPhysicalTypedCells DirectPhysicalCellsFromTypedInputRow(
    const EngineRowValue& input_row,
    const InsertRowEncoderPlan& row_encoder_plan,
    const std::vector<DirectFixedWidthPayloadValidationColumnPlan>* column_plan) {
  DirectPhysicalTypedCells result;
  result.cells.reserve(input_row.fields.size());
  std::uint16_t ordinal = 1;
  for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
    const auto& field = input_row.fields[field_index];
    const auto& typed = field.second;
    const std::string_view target_type =
        field_index < row_encoder_plan.columns.size()
            ? std::string_view(row_encoder_plan.columns[field_index].canonical_type_name)
            : std::string_view{};
    const dt::CanonicalTypeId target_type_id =
        column_plan != nullptr && field_index < column_plan->size()
            ? (*column_plan)[field_index].target_type
            : dt::CanonicalTypeIdFromStableName(std::string(target_type));
    auto cell =
        column_plan != nullptr && field_index < column_plan->size()
            ? DirectPhysicalCellFromTypedValueWithPlan(
                  ordinal++, typed, (*column_plan)[field_index])
            : DirectPhysicalCellFromTypedValue(ordinal++, typed, target_type);
    if (cell.value.is_null) {
      ++result.null_cells;
    } else if (cell.value.type_id == dt::CanonicalTypeId::int64) {
      ++result.int64_cells;
    } else if (cell.value.type_id == dt::CanonicalTypeId::character &&
               target_type_id == dt::CanonicalTypeId::character) {
      ++result.character_cells;
    }
    if (!cell.value.is_null &&
        cell.value.type_id != dt::CanonicalTypeId::character) {
      ++result.typed_binary_cells;
      ++result.typed_cell_counts[cell.value.type_id];
    }
    result.cells.push_back(std::move(cell));
  }
  return result;
}

std::vector<DirectFixedWidthPayloadValidationColumnPlan>
BuildDirectFixedWidthPayloadValidationPlan(
    const InsertRowEncoderPlan& row_encoder_plan,
    const EngineRowValue* first_row) {
  std::vector<DirectFixedWidthPayloadValidationColumnPlan> plan;
  plan.reserve(row_encoder_plan.columns.size());
  for (std::size_t index = 0; index < row_encoder_plan.columns.size(); ++index) {
    const auto& column = row_encoder_plan.columns[index];
    DirectFixedWidthPayloadValidationColumnPlan column_plan;
    column_plan.column_name = column.column_name;
    column_plan.canonical_type_name = column.canonical_type_name;
    column_plan.target_type =
        dt::CanonicalTypeIdFromStableName(column_plan.canonical_type_name);
    if (column_plan.target_type == dt::CanonicalTypeId::unknown &&
        first_row != nullptr &&
        index < first_row->fields.size()) {
      const std::string& input_type =
          first_row->fields[index].second.descriptor.canonical_type_name;
      const dt::CanonicalTypeId input_type_id =
          dt::CanonicalTypeIdFromStableName(input_type);
      if (input_type_id != dt::CanonicalTypeId::unknown) {
        column_plan.target_type = input_type_id;
        column_plan.canonical_type_name = input_type;
      }
    }
    column_plan.character_type =
        column_plan.target_type == dt::CanonicalTypeId::character;
    if (column_plan.target_type != dt::CanonicalTypeId::unknown &&
        !column_plan.character_type) {
      const auto layout = dt::LookupDatatypeStorageLayout(column_plan.target_type);
      if (layout.ok() &&
          layout.layout.storage_class == dt::DatatypeStorageClass::inline_fixed) {
        column_plan.inline_fixed = true;
        column_plan.inline_bytes =
            static_cast<std::size_t>(layout.layout.inline_bytes);
      }
    }
    plan.push_back(std::move(column_plan));
  }
  return plan;
}

std::string DirectFixedWidthTypedPayloadFailure(
    const EngineRowValue& input_row,
    const std::vector<DirectFixedWidthPayloadValidationColumnPlan>& plan,
    DirectFixedWidthPayloadValidationStats* stats) {
  if (stats != nullptr) {
    ++stats->rows_checked;
  }
  if (input_row.fields.size() != plan.size()) {
    return "typed_row_shape_mismatch";
  }
  for (std::size_t field_index = 0; field_index < input_row.fields.size(); ++field_index) {
    const auto& typed = input_row.fields[field_index].second;
    if (typed.isSqlNull()) {
      if (stats != nullptr) {
        ++stats->null_cells;
      }
      continue;
    }
    if (stats != nullptr) {
      ++stats->cells_seen;
    }
    const auto& column = plan[field_index];
    const dt::CanonicalTypeId target_type = column.target_type;
    if (target_type == dt::CanonicalTypeId::unknown) {
      if (stats != nullptr) {
        ++stats->unknown_type_skips;
      }
      continue;
    }
    if (stats != nullptr) {
      ++stats->cells_by_type[target_type];
    }
    if (column.character_type) {
      if (stats != nullptr) {
        ++stats->character_type_skips;
      }
      continue;
    }
    if (!column.inline_fixed) {
      if (stats != nullptr) {
        ++stats->non_fixed_type_skips;
      }
      continue;
    }
    const std::size_t inline_bytes = column.inline_bytes;
    if (!typed.binary_value.empty() &&
        inline_bytes != 0 &&
        typed.binary_value.size() == inline_bytes) {
      if (stats != nullptr) {
        ++stats->binary_exact_hits;
      }
      continue;
    }
    if (DirectBinaryIntegerPayloadCompatible(target_type, typed, inline_bytes)) {
      if (stats != nullptr) {
        ++stats->binary_integer_downcast_hits;
      }
      continue;
    }
    if (!typed.binary_value.empty() &&
        (inline_bytes == 0 || typed.binary_value.size() == inline_bytes)) {
      if (stats != nullptr) {
        ++stats->binary_shape_hits;
      }
      continue;
    }
    if (stats != nullptr) {
      ++stats->text_pack_attempts;
      ++stats->text_pack_attempts_by_type[target_type];
    }
    std::vector<scratchbird::core::platform::byte> payload;
    if (!DirectPackTypedPayload(target_type, typed, &payload)) {
      if (stats != nullptr) {
        ++stats->text_pack_failures;
      }
      return "typed_fixed_payload_invalid:" + column.column_name + ":" +
             std::string(dt::CanonicalTypeName(target_type));
    }
    if (stats != nullptr) {
      ++stats->text_pack_successes;
    }
  }
  return {};
}

void AddFixedWidthPayloadValidationEvidence(
    const DirectFixedWidthPayloadValidationStats& stats,
    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr || stats.rows_checked == 0) {
    return;
  }
  const auto add = [&](std::string key, std::uint64_t value) {
    result->evidence.push_back(
        {"direct_physical_bulk_trace.native_bulk_payload_validate_" +
             std::move(key),
         std::to_string(value)});
  };
  add("rows_checked", stats.rows_checked);
  add("cells_seen", stats.cells_seen);
  add("null_cells", stats.null_cells);
  add("unknown_type_skips", stats.unknown_type_skips);
  add("character_type_skips", stats.character_type_skips);
  add("non_fixed_type_skips", stats.non_fixed_type_skips);
  add("binary_exact_hits", stats.binary_exact_hits);
  add("binary_integer_downcast_hits", stats.binary_integer_downcast_hits);
  add("binary_shape_hits", stats.binary_shape_hits);
  add("text_pack_attempts", stats.text_pack_attempts);
  add("text_pack_successes", stats.text_pack_successes);
  add("text_pack_failures", stats.text_pack_failures);
  for (const auto& [type_id, count] : stats.cells_by_type) {
    add("type_" + std::string(dt::CanonicalTypeName(type_id)), count);
  }
  for (const auto& [type_id, count] : stats.text_pack_attempts_by_type) {
    add("text_pack_type_" + std::string(dt::CanonicalTypeName(type_id)), count);
  }
}

}  // namespace scratchbird::engine::internal_api::dml::detail
