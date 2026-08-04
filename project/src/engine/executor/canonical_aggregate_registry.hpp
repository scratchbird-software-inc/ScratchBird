// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::executor {

// SEARCH_KEY: QOW_RCP025_CANONICAL_GLOBAL_AGGREGATE_REGISTRY_V1
//
// This is the sole engine runtime identity registry for canonical aggregates.
// Parser and donor aliases may translate to one builtin_id, but no executor,
// window bridge, scalar-function bridge, or compatibility route may maintain
// a second executable aggregate identity table.
enum class CanonicalAggregateFunction : std::uint8_t {
  unknown = 0,
  count,
  sum,
  avg,
  min,
  max,
  bool_and,
  bool_or,
  array_agg,
  string_agg,
  json_agg,
  json_object_agg,
  stddev_pop,
  variance_pop,
  every,
  listagg,
  rank,
  dense_rank,
  percent_rank,
  cume_dist,
  mode,
  percentile_cont,
  percentile_disc,
  approx_count_distinct,
  approx_median,
  approx_percentile_cont,
  approx_percentile_disc,
  approx_top_k,
  stddev,
  variance,
  stddev_samp,
  variance_samp,
  corr,
  covar_pop,
  covar_samp,
  regr_count,
  regr_avgx,
  regr_avgy,
  regr_intercept,
  regr_r2,
  regr_slope,
  regr_sxx,
  regr_sxy,
  regr_syy,
};

struct CanonicalAggregateRegistryEntry {
  std::uint16_t abi_version = 0;
  CanonicalAggregateFunction function = CanonicalAggregateFunction::unknown;
  std::string builtin_id;
  std::string function_uuid;
  bool executable = false;
  bool aggregate_as_window = false;
  bool moving_window_inverse = false;
};

// The returned container and every entry address are stable for the process
// lifetime. Callers must use the lookup functions below for identity
// resolution; the container view exists for finite inventory/conformance.
const std::vector<CanonicalAggregateRegistryEntry>&
CanonicalAggregateRuntimeRegistryV1();

const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateByFunctionV1(
    CanonicalAggregateFunction function);
const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateByBuiltinIdV1(
    std::string_view builtin_id);
const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateByUuidV1(
    std::string_view function_uuid);
const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateExactV1(
    std::uint16_t abi_version,
    CanonicalAggregateFunction function,
    std::string_view builtin_id,
    std::string_view function_uuid);

std::vector<std::string> ValidateCanonicalAggregateRuntimeRegistryV1();

}  // namespace scratchbird::engine::executor
