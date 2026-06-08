// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_aggregate_window_runtime.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
#include "sblr_aggregate_window_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {

std::string Lower(std::string_view input) {
  std::string out(input);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string AggregateOperationId(const SblrAggregateWindowState& state) {
  return state.function_id.empty() ? "sblr.aggregate" : state.function_id;
}

SblrRuntimeDiagnostic AggregateDiagnostic(std::string diagnostic_id,
                                          const SblrExecutionContext& context,
                                          const SblrAggregateWindowState* state,
                                          std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  if (state != nullptr) {
    diagnostic.fields.push_back({"state_uuid", state->state_uuid});
    diagnostic.fields.push_back({"function_uuid", state->function_uuid});
    diagnostic.fields.push_back({"function_id", state->function_id});
  }
  return diagnostic;
}

SblrResult AggregateFailure(SblrStatusCode status,
                            std::string operation_id,
                            SblrRuntimeDiagnostic diagnostic) {
  return MakeSblrFailure(status, std::move(operation_id), std::move(diagnostic));
}

SblrValue NullValue(std::string descriptor_id) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.payload_kind = SblrValuePayloadKind::none;
  out.is_null = true;
  return out;
}

SblrValue Int64Value(std::int64_t value) {
  SblrValue out;
  out.descriptor_id = "int64";
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value;
  out.payload_kind = SblrValuePayloadKind::signed_integer;
  out.encoded_value = std::to_string(value);
  out.text_value = out.encoded_value;
  return out;
}

SblrValue Real64Value(double value) {
  SblrValue out;
  out.descriptor_id = "real64";
  out.is_null = false;
  out.has_real64_value = true;
  out.real64_value = value;
  out.payload_kind = SblrValuePayloadKind::real64;
  std::ostringstream stream;
  stream << std::setprecision(17) << value;
  out.encoded_value = stream.str();
  out.text_value = out.encoded_value;
  return out;
}

SblrValue BoolValue(bool value) {
  SblrValue out;
  out.descriptor_id = "boolean";
  out.payload_kind = SblrValuePayloadKind::boolean;
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value ? 1 : 0;
  out.encoded_value = value ? "TRUE" : "FALSE";
  out.text_value = out.encoded_value;
  return out;
}

std::string ValueText(const SblrValue& value) {
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (value.has_real64_value) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value.real64_value;
    return stream.str();
  }
  if (!value.encoded_value.empty()) return value.encoded_value;
  return value.text_value;
}

bool ParseLongDouble(const SblrValue& value, long double* out) {
  if (value.is_null || out == nullptr) return false;
  if (value.has_int64_value) {
    *out = static_cast<long double>(value.int64_value);
    return true;
  }
  if (value.has_uint64_value) {
    *out = static_cast<long double>(value.uint64_value);
    return true;
  }
  if (value.has_real64_value) {
    *out = static_cast<long double>(value.real64_value);
    return std::isfinite(value.real64_value);
  }
  const std::string text = ValueText(value);
  if (text.empty()) return false;
  char* end = nullptr;
  const long double parsed = std::strtold(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(static_cast<double>(parsed))) return false;
  *out = parsed;
  return true;
}

bool ParseUint64(const SblrValue& value, std::uint64_t* out) {
  if (value.is_null || out == nullptr) return false;
  if (value.has_uint64_value) {
    *out = value.uint64_value;
    return true;
  }
  if (value.has_int64_value) {
    *out = static_cast<std::uint64_t>(value.int64_value);
    return true;
  }
  const std::string text = ValueText(value);
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0') return false;
  *out = static_cast<std::uint64_t>(parsed);
  return true;
}

bool Truthy(const SblrValue& value, bool* out) {
  if (value.is_null || out == nullptr) return false;
  if (value.payload_kind == SblrValuePayloadKind::boolean || value.has_int64_value || value.has_uint64_value) {
    *out = value.has_uint64_value ? value.uint64_value != 0 : value.int64_value != 0;
    return true;
  }
  const std::string text = Lower(ValueText(value));
  if (text == "true" || text == "t" || text == "yes" || text == "y" || text == "1") {
    *out = true;
    return true;
  }
  if (text == "false" || text == "f" || text == "no" || text == "n" || text == "0") {
    *out = false;
    return true;
  }
  return false;
}

int CompareValues(const SblrValue& left, const SblrValue& right) {
  long double left_number = 0.0L;
  long double right_number = 0.0L;
  if (ParseLongDouble(left, &left_number) && ParseLongDouble(right, &right_number)) {
    if (left_number < right_number) return -1;
    if (left_number > right_number) return 1;
    return 0;
  }
  const std::string left_text = ValueText(left);
  const std::string right_text = ValueText(right);
  if (left_text < right_text) return -1;
  if (left_text > right_text) return 1;
  return 0;
}

std::uint64_t EstimateStateBytes(const SblrAggregateWindowState& state) {
  std::uint64_t bytes = static_cast<std::uint64_t>(sizeof(SblrAggregateWindowState) + state.state_uuid.size() +
                                                   state.function_uuid.size() + state.function_id.size() +
                                                   state.result_descriptor_id.size() + state.min_value.text_value.size() +
                                                   state.min_value.encoded_value.size() + state.min_value.binary_value.size() +
                                                   state.max_value.text_value.size() + state.max_value.encoded_value.size() +
                                                   state.max_value.binary_value.size() + state.text_accumulator.size() +
                                                   state.separator.size() + state.binary_accumulator.size());
  for (const auto& value : state.distinct_values) bytes += static_cast<std::uint64_t>(value.size());
  for (const auto& value : state.sketch_values) bytes += static_cast<std::uint64_t>(value.size());
  bytes += static_cast<std::uint64_t>(state.sketch_counts.size() * sizeof(std::uint64_t));
  bytes += static_cast<std::uint64_t>(state.sketch_numeric_values.size() * sizeof(long double));
  return bytes;
}

SblrResult CheckMemoryBudget(SblrAggregateWindowState* state,
                             const SblrAggregateUpdateRequest& request) {
  state->memory_bytes = EstimateStateBytes(*state);
  if (request.options.max_state_bytes != 0 && state->memory_bytes > request.options.max_state_bytes) {
    return AggregateFailure(SblrStatusCode::resource_exhausted,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_STATE_MEMORY_LIMIT",
                                                request.context,
                                                state,
                                                "aggregate state exceeded the configured memory budget"));
  }
  return MakeSblrSuccess(AggregateOperationId(*state));
}

bool CountIncludesValue(const SblrAggregateWindowState& state,
                        const SblrAggregateUpdateRequest& request) {
  if (request.values.empty()) return true;
  if (request.options.count_nulls) return true;
  if (state.function_id.find("count_star") != std::string::npos) return true;
  return !request.values.front().is_null;
}

SblrResult ApplyNumericInput(SblrAggregateWindowState* state,
                             const SblrAggregateUpdateRequest& request,
                             const SblrValue& value) {
  if (value.is_null) return MakeSblrSuccess(AggregateOperationId(*state));
  long double numeric = 0.0L;
  if (!ParseLongDouble(value, &numeric)) {
    return AggregateFailure(SblrStatusCode::execution_failed,
                            AggregateOperationId(*state),
                            AggregateDiagnostic("SB_DIAG_AGGREGATE_NUMERIC_INPUT_REQUIRED",
                                                request.context,
                                                state,
