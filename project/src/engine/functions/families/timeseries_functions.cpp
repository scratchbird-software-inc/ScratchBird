// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/timeseries_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

std::vector<double> ParseSeries(std::string text) {
  for (char& ch : text) {
    if (ch == ',' || ch == '[' || ch == ']') ch = ' ';
  }
  std::istringstream in(text);
  std::vector<double> out;
  double value = 0.0;
  while (in >> value) out.push_back(value);
  return out;
}

}  // namespace

bool IsTimeseriesFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("timeseries.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.timeseries.", 0) == 0;
}

FunctionCallResult DispatchTimeseriesFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (id == "timeseries.bucket" || id == "timeseries.bucket_epoch_ms" || id == "sb.fn.timeseries.bucket_epoch_ms" ||
      id == "sb.fn.timeseries.sb_time_bucket" || id == "sb.fn.timeseries.time_bucket.bucket_fixed_interval" ||
      id == "sb.fn.timeseries.time_bucket.bucket_calendar") {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bucket_epoch_ms expects bucket milliseconds and epoch milliseconds");
    if (!request.arguments[0].value.has_int64_value || !request.arguments[1].value.has_int64_value ||
        request.arguments[0].value.int64_value <= 0) {
      return RefuseFunctionInvalidInput(request, "bucket_epoch_ms requires positive int64 bucket and int64 epoch milliseconds");
    }
    const auto bucket = request.arguments[0].value.int64_value;
    const auto epoch = request.arguments[1].value.int64_value;
    return MakeFunctionSuccess(request, {MakeInt64Value("timestamp_epoch_ms", (epoch / bucket) * bucket)});
  }

  if (id == "sb.fn.timeseries.time_bucket.truncate_temporal") {
    if (request.arguments.empty() || request.arguments.size() > 2) {
      return RefuseFunctionInvalidInput(request, "truncate_temporal expects epoch milliseconds and optional bucket milliseconds");
    }
    if (!request.arguments[0].value.has_int64_value) {
      return RefuseFunctionInvalidInput(request, "truncate_temporal requires int64 epoch milliseconds");
    }
    if (request.arguments.size() == 1) return MakeFunctionSuccess(request, {MakeInt64Value("timestamp_epoch_ms", request.arguments[0].value.int64_value)});
    if (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value <= 0) {
      return RefuseFunctionInvalidInput(request, "truncate_temporal bucket must be positive int64 milliseconds");
    }
    const auto bucket = request.arguments[1].value.int64_value;
    const auto epoch = request.arguments[0].value.int64_value;
    return MakeFunctionSuccess(request, {MakeInt64Value("timestamp_epoch_ms", (epoch / bucket) * bucket)});
  }

  if (id == "timeseries.gapfill_marker" || id == "sb.fn.timeseries.gapfill_marker") {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "gapfill_marker expects start and end epoch milliseconds");
    if (!request.arguments[0].value.has_int64_value || !request.arguments[1].value.has_int64_value) {
      return RefuseFunctionInvalidInput(request, "gapfill_marker requires int64 start and end");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("time_series_value",
                                                       "GAPFILL(" + std::to_string(request.arguments[0].value.int64_value) +
                                                           "," + std::to_string(request.arguments[1].value.int64_value) + ")")});
  }

  if (id == "timeseries.interpolate_linear" || id == "sb.fn.timeseries.interpolate_linear") {
    if (request.arguments.size() != 5) return RefuseFunctionInvalidInput(request, "interpolate_linear expects x0 y0 x1 y1 x");
    for (const auto& argument : request.arguments) {
      if (!argument.value.has_real64_value && !argument.value.has_int64_value) return RefuseFunctionInvalidInput(request, "interpolate_linear arguments must be numeric");
    }
    const double x0 = request.arguments[0].value.has_real64_value ? request.arguments[0].value.real64_value : request.arguments[0].value.int64_value;
    const double y0 = request.arguments[1].value.has_real64_value ? request.arguments[1].value.real64_value : request.arguments[1].value.int64_value;
    const double x1 = request.arguments[2].value.has_real64_value ? request.arguments[2].value.real64_value : request.arguments[2].value.int64_value;
    const double y1 = request.arguments[3].value.has_real64_value ? request.arguments[3].value.real64_value : request.arguments[3].value.int64_value;
    const double x = request.arguments[4].value.has_real64_value ? request.arguments[4].value.real64_value : request.arguments[4].value.int64_value;
    if (x1 == x0) return RefuseFunctionInvalidInput(request, "interpolate_linear requires x1 != x0");
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", y0 + ((x - x0) * (y1 - y0) / (x1 - x0)))});
  }
  if (id == "timeseries.aggregate" || id == "sb.fn.timeseries.aggregate" ||
      id == "sb.fn.timeseries.time_window_agg.window_aggregate") {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "timeseries.aggregate expects aggregate name and numeric series");
    const auto aggregate = ValueAsText(request.arguments[0].value);
    const auto values = ParseSeries(ValueAsText(request.arguments[1].value));
    if (values.empty()) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    double result = 0.0;
    if (aggregate == "count") {
      result = static_cast<double>(values.size());
    } else if (aggregate == "min") {
      result = *std::min_element(values.begin(), values.end());
    } else if (aggregate == "max") {
      result = *std::max_element(values.begin(), values.end());
    } else {
      for (const auto value : values) result += value;
      if (aggregate == "avg" || aggregate == "average") result /= static_cast<double>(values.size());
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", result)});
  }
  if (id == "timeseries.downsample" || id == "sb.fn.timeseries.downsample" ||
      id == "sb.fn.timeseries.time_window_agg.window_moving") {
    if (request.arguments.size() != 2 || !request.arguments[0].value.has_int64_value) {
      return RefuseFunctionInvalidInput(request, "timeseries.downsample expects stride and numeric series");
    }
    const auto stride = request.arguments[0].value.int64_value;
    if (stride <= 0) return RefuseFunctionInvalidInput(request, "timeseries.downsample stride must be positive");
    const auto values = ParseSeries(ValueAsText(request.arguments[1].value));
    std::string out = "[";
    for (std::size_t index = 0; index < values.size(); index += static_cast<std::size_t>(stride)) {
      if (index != 0) out += ",";
      out += std::to_string(values[index]);
    }
    out += "]";
    return MakeFunctionSuccess(request, {MakeTextValue("time_series_value", out)});
  }
  if (id == "timeseries.descriptor_accepts") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "timeseries descriptor_accepts expects descriptor id");
    const auto descriptor = ValueAsText(request.arguments[0].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", (descriptor == "time_series_value" || descriptor == "timestamp_epoch_ms" ||
                                                                    descriptor == "timestamp" || descriptor == "real64") ? 1 : 0)});
  }

  if (id == "sb.fn.timeseries.time_series_manage.series_create" ||
      id == "sb.fn.timeseries.time_series_manage.series_inspect" ||
      id == "sb.fn.timeseries.time_series_manage.series_manage" ||
      id == "sb.fn.timeseries.time_series_write.series_insert" ||
      id == "sb.fn.timeseries.time_series_write.series_write") {
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::policy_refused,
                                        "SB_DIAG_TIMESERIES_SERIES_CATALOG_POLICY_REQUIRED",
                                        "time-series series lifecycle and writes require catalog DDL/DML authority and cannot execute as scalar helpers");
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_TIMESERIES_FUNCTION_UNHANDLED",
                                      "timeseries helper id is not handled by the activated time-series scalar surface");
}

}  // namespace scratchbird::engine::functions
