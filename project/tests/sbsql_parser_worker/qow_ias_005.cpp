// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(bool condition, std::string_view detail) {
  if (!condition) std::cerr << detail << '\n';
  return condition;
}

bool LegacyRouteRefused(std::string_view operation,
                        std::string_view projection = {}) {
  const auto disposition =
      api::RefuseNoncanonicalLegacyWindowRoute(operation, projection);
  return disposition.applies && !disposition.accepted &&
         disposition.diagnostic_code ==
             "QOW-DIAG-IAS-005-NONCANONICAL-WINDOW-ROUTE-V1" &&
         !disposition.detail.empty();
}

bool ValidateLegacyRouteRemoval() {
  const std::vector<std::string_view> operations = {
      "window",          "row_number_window", "partition_count_window",
      "lag_window",      "lead_window",       "first_value_window",
      "last_value_window", "ntile_window",    "percent_rank_window",
      "cume_dist_window", "nth_value_window",
  };
  bool passed = true;
  for (const auto operation : operations) {
    passed &= Require(LegacyRouteRefused(operation),
                      "route-specific window helper remained authoritative");
  }
  passed &= Require(
      LegacyRouteRefused("materialized_cte", "window_assertion"),
      "materialized-window assertion helper remained authoritative");

  const auto ordinary =
      api::RefuseNoncanonicalLegacyWindowRoute("filter", {});
  const auto aggregate_projection =
      api::RefuseNoncanonicalLegacyWindowRoute("materialized_cte",
                                               "aggregate_assertion");
  passed &= Require(!ordinary.applies && !ordinary.accepted &&
                        ordinary.diagnostic_code.empty() &&
                        !aggregate_projection.applies,
                    "non-window route was captured by IAS-005 refusal");
  return passed;
}

}  // namespace

// QOW-TEST-IAS-005-V1
int main() {
  return ValidateLegacyRouteRemoval() ? EXIT_SUCCESS : EXIT_FAILURE;
}
