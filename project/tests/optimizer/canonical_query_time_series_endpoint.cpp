// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_time_series_endpoint.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace endpoint =
    scratchbird::engine::sblr::canonical_query_execute_detail;

namespace {

// SEARCH_KEY: SB_TEST_CANONICAL_QUERY_TIME_SERIES_ENDPOINT_BOUNDARY_MATRIX

struct ValidCase {
  std::string_view text;
  std::int64_t expected_ns;
};

constexpr std::array kValidCases{
    ValidCase{"1970-01-01T00:00:00Z", 0},
    ValidCase{"1970-01-01T00:00:00.1Z", 100'000'000},
    ValidCase{"1970-01-01T00:00:00.123456789Z", 123'456'789},
    ValidCase{"1970-01-01T01:00:00+01:00", 0},
    ValidCase{"1969-12-31T19:00:00-05:00", 0},
    ValidCase{"1970-01-01T14:00:00+14:00", 0},
    ValidCase{"1969-12-31T10:00:00-14:00", 0},
    ValidCase{"2000-02-29T00:00:00Z", 951'782'400'000'000'000LL},
    ValidCase{"2262-04-11T23:47:16.854775807Z",
              std::numeric_limits<std::int64_t>::max()},
    ValidCase{"1677-09-21T00:12:43.145224192Z",
              std::numeric_limits<std::int64_t>::min()},
};

constexpr std::array kInvalidCases{
    std::string_view{},
    std::string_view{"1970-01-01T00:00:00"},
    std::string_view{"1970-01-01t00:00:00Z"},
    std::string_view{"1970-01-01T00:00:00z"},
    std::string_view{"0000-01-01T00:00:00Z"},
    std::string_view{"1970-00-01T00:00:00Z"},
    std::string_view{"1970-13-01T00:00:00Z"},
    std::string_view{"1970-01-00T00:00:00Z"},
    std::string_view{"1970-04-31T00:00:00Z"},
    std::string_view{"1900-02-29T00:00:00Z"},
    std::string_view{"2000-02-30T00:00:00Z"},
    std::string_view{"1970-01-01T24:00:00Z"},
    std::string_view{"1970-01-01T00:60:00Z"},
    std::string_view{"2016-12-31T23:59:60Z"},
    std::string_view{"1970-01-01T00:00:00.Z"},
    std::string_view{"1970-01-01T00:00:00.1234567890Z"},
    std::string_view{"1970-01-01T14:00:00+14:01"},
    std::string_view{"1970-01-01T15:00:00+15:00"},
    std::string_view{"1970-01-01T00:00:00+00:60"},
    std::string_view{"1970-01-01T00:00:00+0000"},
    std::string_view{"1970-01-01T00:00:00Ztrailing"},
    std::string_view{"2262-04-11T23:47:16.854775808Z"},
    std::string_view{"1677-09-21T00:12:43.145224191Z"},
};

}  // namespace

int main() {
  bool passed = true;
  for (const auto& test : kValidCases) {
    std::int64_t actual_ns = 0;
    if (!endpoint::ParseTimeSeriesEndpointNsV1(test.text, &actual_ns) ||
        actual_ns != test.expected_ns) {
      std::cerr << "valid endpoint failed: " << test.text << " expected="
                << test.expected_ns << " actual=" << actual_ns << '\n';
      passed = false;
    }
  }

  for (const auto text : kInvalidCases) {
    std::int64_t actual_ns = 0;
    if (endpoint::ParseTimeSeriesEndpointNsV1(text, &actual_ns)) {
      std::cerr << "invalid endpoint accepted: " << text << " actual="
                << actual_ns << '\n';
      passed = false;
    }
  }

  if (endpoint::ParseTimeSeriesEndpointNsV1(
          "1970-01-01T00:00:00Z", nullptr)) {
    std::cerr << "null output pointer was accepted\n";
    passed = false;
  }

  return passed ? 0 : 1;
}
