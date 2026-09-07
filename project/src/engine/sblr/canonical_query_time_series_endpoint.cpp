// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_time_series_endpoint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace scratchbird::engine::sblr::canonical_query_execute_detail {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_TIME_SERIES_ENDPOINT_AUTHORITY
// Owns canonical endpoint syntax validation and nanosecond normalization only.
// Model-family execution, result publication and MGA visibility remain with
// their engine-owned authorities.

namespace {

constexpr std::int64_t TimeSeriesEndpointDaysFromCivil(
    const int year, const unsigned month, const unsigned day) {
  const int adjusted_year = year - (month <= 2 ? 1 : 0);
  const int era =
      (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
  const unsigned year_of_era =
      static_cast<unsigned>(adjusted_year - era * 400);
  const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
  const unsigned day_of_year =
      (153 * adjusted_month + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

bool ParseTimeSeriesEndpointUnsigned(const std::string_view value,
                                     const std::size_t offset,
                                     const std::size_t count,
                                     unsigned* out) {
  if (out == nullptr || offset > value.size() ||
      count > value.size() - offset) {
    return false;
  }
  unsigned parsed = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char ch = value[offset + index];
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10 + static_cast<unsigned>(ch - '0');
  }
  *out = parsed;
  return true;
}

}  // namespace

bool ParseTimeSeriesEndpointNsV1(const std::string_view value,
                                 std::int64_t* timestamp_ns) {
  if (timestamp_ns == nullptr || value.size() < 20 || value[4] != '-' ||
      value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':') {
    return false;
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!ParseTimeSeriesEndpointUnsigned(value, 0, 4, &year) ||
      !ParseTimeSeriesEndpointUnsigned(value, 5, 2, &month) ||
      !ParseTimeSeriesEndpointUnsigned(value, 8, 2, &day) ||
      !ParseTimeSeriesEndpointUnsigned(value, 11, 2, &hour) ||
      !ParseTimeSeriesEndpointUnsigned(value, 14, 2, &minute) ||
      !ParseTimeSeriesEndpointUnsigned(value, 17, 2, &second) || year == 0 ||
      month == 0 || month > 12 || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  constexpr std::array<unsigned, 12> kMonthDays{
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  unsigned maximum_day = kMonthDays[month - 1];
  if (month == 2 &&
      ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    ++maximum_day;
  }
  if (day == 0 || day > maximum_day) return false;

  std::size_t offset = 19;
  std::uint64_t fraction_ns = 0;
  if (offset < value.size() && value[offset] == '.') {
    ++offset;
    const auto fraction_begin = offset;
    while (offset < value.size() && value[offset] >= '0' &&
           value[offset] <= '9') {
      if (offset - fraction_begin >= 9) return false;
      fraction_ns = fraction_ns * 10 +
                    static_cast<std::uint64_t>(value[offset] - '0');
      ++offset;
    }
    if (offset == fraction_begin) return false;
    for (std::size_t index = offset - fraction_begin; index < 9; ++index) {
      fraction_ns *= 10;
    }
  }

  int offset_sign = 0;
  unsigned offset_hour = 0;
  unsigned offset_minute = 0;
  if (offset < value.size() && value[offset] == 'Z') {
    ++offset;
  } else if (offset <= value.size() && value.size() - offset == 6 &&
             (value[offset] == '+' || value[offset] == '-') &&
             value[offset + 3] == ':' &&
             ParseTimeSeriesEndpointUnsigned(value, offset + 1, 2,
                                             &offset_hour) &&
             ParseTimeSeriesEndpointUnsigned(value, offset + 4, 2,
                                             &offset_minute)) {
    offset_sign = value[offset] == '+' ? 1 : -1;
    offset += 6;
    if (offset_hour > 14 || offset_minute > 59 ||
        (offset_hour == 14 && offset_minute != 0)) {
      return false;
    }
  } else {
    return false;
  }
  if (offset != value.size()) return false;

  constexpr __int128 kNsPerSecond = 1'000'000'000;
  const auto days =
      TimeSeriesEndpointDaysFromCivil(static_cast<int>(year), month, day);
  __int128 seconds = static_cast<__int128>(days) * 86'400 +
                     static_cast<__int128>(hour) * 3600 +
                     static_cast<__int128>(minute) * 60 + second;
  seconds -= static_cast<__int128>(offset_sign) *
             (static_cast<__int128>(offset_hour) * 3600 + offset_minute * 60);
  const auto encoded = seconds * kNsPerSecond + fraction_ns;
  if (encoded < std::numeric_limits<std::int64_t>::min() ||
      encoded > std::numeric_limits<std::int64_t>::max()) {
    return false;
  }
  *timestamp_ns = static_cast<std::int64_t>(encoded);
  return true;
}

}  // namespace scratchbird::engine::sblr::canonical_query_execute_detail
