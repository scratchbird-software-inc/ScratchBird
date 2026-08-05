// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include "temp_spill_executor.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

using scratchbird::engine::internal_api::EngineTypedValue;
using scratchbird::engine::internal_api::EngineValueState;

struct CanonicalAggregateCoreState {
  std::size_t transition_count = 0;
  std::size_t non_null_count = 0;
  __int128 int64_sum = 0;
  long double real_sum = 0.0L;
  long double numeric_mean = 0.0L;
  long double numeric_m2 = 0.0L;
  long double pair_mean_x = 0.0L;
  long double pair_mean_y = 0.0L;
  long double pair_m2_x = 0.0L;
  long double pair_m2_y = 0.0L;
  long double pair_comoment = 0.0L;
  bool saw_true = false;
  bool saw_false = false;
  std::optional<EngineTypedValue> extremum;
  std::vector<EngineTypedValue> collection_values;
  std::vector<std::string> text_values;
  std::vector<std::pair<std::string, std::string>> json_object_values;
  std::vector<long double> ordered_numeric_values;
  std::vector<std::string> approximate_distinct_values;
  std::vector<std::pair<EngineTypedValue, std::size_t>> frequency_values;
};

bool IsCanonicalHypotheticalSetFunction(
    const CanonicalAggregateFunction function) {
  return function == CanonicalAggregateFunction::rank ||
         function == CanonicalAggregateFunction::dense_rank ||
         function == CanonicalAggregateFunction::percent_rank ||
         function == CanonicalAggregateFunction::cume_dist;
}

bool IsCanonicalQuantileFunction(
    const CanonicalAggregateFunction function) {
  return function == CanonicalAggregateFunction::percentile_cont ||
         function == CanonicalAggregateFunction::percentile_disc ||
         function == CanonicalAggregateFunction::approx_median ||
         function == CanonicalAggregateFunction::approx_percentile_cont ||
         function == CanonicalAggregateFunction::approx_percentile_disc;
}

bool IsCanonicalOrderedSetFunction(
    const CanonicalAggregateFunction function) {
  return IsCanonicalHypotheticalSetFunction(function) ||
         function == CanonicalAggregateFunction::mode ||
         function == CanonicalAggregateFunction::percentile_cont ||
         function == CanonicalAggregateFunction::percentile_disc ||
         function == CanonicalAggregateFunction::approx_percentile_cont ||
         function == CanonicalAggregateFunction::approx_percentile_disc;
}

std::size_t CanonicalAggregateDirectArgumentCount(
    const CanonicalAggregateFunction function) {
  if (IsCanonicalHypotheticalSetFunction(function) ||
      function == CanonicalAggregateFunction::percentile_cont ||
      function == CanonicalAggregateFunction::percentile_disc ||
      function == CanonicalAggregateFunction::approx_percentile_cont ||
      function == CanonicalAggregateFunction::approx_percentile_disc ||
      function == CanonicalAggregateFunction::approx_top_k) {
    return 1;
  }
  return 0;
}

bool IsCanonicalUnivariateStatisticalFunction(
    const CanonicalAggregateFunction function) {
  return function == CanonicalAggregateFunction::stddev_pop ||
         function == CanonicalAggregateFunction::variance_pop ||
         function == CanonicalAggregateFunction::stddev ||
         function == CanonicalAggregateFunction::variance ||
         function == CanonicalAggregateFunction::stddev_samp ||
         function == CanonicalAggregateFunction::variance_samp;
}

bool IsCanonicalPairStatisticalFunction(
    const CanonicalAggregateFunction function) {
  return function == CanonicalAggregateFunction::corr ||
         function == CanonicalAggregateFunction::covar_pop ||
         function == CanonicalAggregateFunction::covar_samp ||
         function == CanonicalAggregateFunction::regr_count ||
         function == CanonicalAggregateFunction::regr_avgx ||
         function == CanonicalAggregateFunction::regr_avgy ||
         function == CanonicalAggregateFunction::regr_intercept ||
         function == CanonicalAggregateFunction::regr_r2 ||
         function == CanonicalAggregateFunction::regr_slope ||
         function == CanonicalAggregateFunction::regr_sxx ||
         function == CanonicalAggregateFunction::regr_sxy ||
         function == CanonicalAggregateFunction::regr_syy;
}

bool DecodeAggregateNumeric(const EngineTypedValue& value,
                            long double* decoded,
                            DescriptorRuntimeDiagnostic* diagnostic) {
  if (decoded == nullptr || diagnostic == nullptr) return false;
  if (IsCanonicalBoundedSignedIntegerDescriptor(value.descriptor)) {
    const auto result = DecodeInt64Value(value);
    if (!result.ok()) {
      *diagnostic = result.diagnostic;
      return false;
    }
    *decoded = static_cast<long double>(result.value);
    return true;
  }
  if (value.descriptor.canonical_type_name == "real64") {
    const auto result = DecodeReal64Value(value);
    if (!result.ok()) {
      *diagnostic = result.diagnostic;
      return false;
    }
    *decoded = static_cast<long double>(result.value);
    return true;
  }
  *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
                        "statistical aggregate requires numeric input");
  return false;
}

bool IsType(const EngineTypedValue& value, const std::string_view type_name) {
  return value.descriptor.canonical_type_name == type_name;
}

bool IsType(const ExecutorColumnDescriptor& column,
            const std::string_view type_name) {
  return column.descriptor.canonical_type_name == type_name;
}

std::size_t CanonicalAggregateExpectedArity(
    const CanonicalAggregateFunction function,
    const bool count_star) {
  if (count_star) return 0;
  if (IsCanonicalPairStatisticalFunction(function) ||
      function == CanonicalAggregateFunction::json_object_agg) {
    return 2;
  }
  return 1;
}

std::string FormatAggregateReal(long double value);

std::string EscapeAggregateJson(const std::string_view input) {
  std::ostringstream stream;
  stream << '"';
  for (const unsigned char byte : input) {
    switch (byte) {
      case '\\': stream << "\\\\"; break;
      case '"': stream << "\\\""; break;
      case '\b': stream << "\\b"; break;
      case '\f': stream << "\\f"; break;
      case '\n': stream << "\\n"; break;
      case '\r': stream << "\\r"; break;
      case '\t': stream << "\\t"; break;
      default:
        if (byte < 0x20) {
          stream << "\\u" << std::hex << std::setw(4)
                 << std::setfill('0') << static_cast<unsigned int>(byte)
                 << std::dec << std::setfill(' ');
        } else {
          stream << static_cast<char>(byte);
        }
        break;
    }
  }
  stream << '"';
  return stream.str();
}

bool RenderAggregateJsonValue(const EngineTypedValue& value,
                              std::string* rendered,
                              DescriptorRuntimeDiagnostic* diagnostic) {
  if (rendered == nullptr || diagnostic == nullptr) return false;
  if (value.state == EngineValueState::sql_null) {
    *rendered = "null";
    return true;
  }
  const auto& type = value.descriptor.canonical_type_name;
  if (type == "int64") {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      *diagnostic = decoded.diagnostic;
      return false;
    }
    *rendered = std::to_string(decoded.value);
    return true;
  }
  if (type == "real64") {
    const auto decoded = DecodeReal64Value(value);
    if (!decoded.ok()) {
      *diagnostic = decoded.diagnostic;
      return false;
    }
    *rendered = FormatAggregateReal(decoded.value);
    return true;
  }
  if (type == "boolean") {
    const auto decoded = DecodeBoolValue(value);
    if (!decoded.ok()) {
      *diagnostic = decoded.diagnostic;
      return false;
    }
    *rendered = decoded.value ? "true" : "false";
    return true;
  }
  if (type == "json") {
    if (value.encoded_value.empty()) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-JSON-V1",
                            "JSON aggregate input has no canonical payload");
      return false;
    }
    *rendered = value.encoded_value;
    return true;
  }
  *rendered = EscapeAggregateJson(value.encoded_value);
  return true;
}

std::size_t EstimateCanonicalAggregateStateBytes(
    const CanonicalAggregateCoreState& state) {
  std::size_t bytes = sizeof(state);
  for (const auto& value : state.collection_values) {
    bytes += sizeof(value) + value.encoded_value.size() +
             value.binary_value.size() +
             value.descriptor.canonical_type_name.size() +
             value.descriptor.encoded_descriptor.size();
  }
  for (const auto& value : state.text_values) bytes += value.size();
  for (const auto& [key, value] : state.json_object_values) {
    bytes += key.size() + value.size();
  }
  bytes += state.ordered_numeric_values.size() * sizeof(long double);
  for (const auto& value : state.approximate_distinct_values) {
    bytes += value.size();
  }
  for (const auto& [value, count] : state.frequency_values) {
    (void)count;
    bytes += sizeof(value) + value.encoded_value.size() +
             value.binary_value.size() +
             value.descriptor.canonical_type_name.size() +
             value.descriptor.encoded_descriptor.size();
  }
  return bytes;
}

std::string JoinAggregateTextValues(const std::vector<std::string>& values,
                                    const std::size_t count,
                                    const std::string_view separator) {
  std::string joined;
  const auto limit = std::min(count, values.size());
  for (std::size_t index = 0; index < limit; ++index) {
    if (index != 0) joined += separator;
    joined += values[index];
  }
  return joined;
}

bool SameAggregateValueIdentity(const EngineTypedValue& left,
                                const EngineTypedValue& right) {
  return left.descriptor.descriptor_uuid.canonical ==
             right.descriptor.descriptor_uuid.canonical &&
         left.descriptor.canonical_type_name ==
             right.descriptor.canonical_type_name &&
         left.state == right.state && left.is_null == right.is_null &&
         left.encoded_value == right.encoded_value &&
         left.binary_value == right.binary_value;
}

void AddCanonicalFrequencyValue(CanonicalAggregateCoreState* state,
                                const EngineTypedValue& value,
                                const std::size_t count = 1) {
  const auto existing = std::find_if(
      state->frequency_values.begin(), state->frequency_values.end(),
      [&](const auto& candidate) {
        return SameAggregateValueIdentity(candidate.first, value);
      });
  if (existing == state->frequency_values.end()) {
    state->frequency_values.emplace_back(value, count);
  } else {
    existing->second += count;
  }
}

EngineTypedValue AggregateValue(const ExecutorColumnDescriptor& column,
                                std::string encoded_value) {
  EngineTypedValue value;
  value.descriptor = column.descriptor;
  value.encoded_value = std::move(encoded_value);
  value.state = EngineValueState::value;
  return value;
}

EngineTypedValue AggregateNull(const ExecutorColumnDescriptor& column) {
  EngineTypedValue value;
  value.descriptor = column.descriptor;
  value.is_null = true;
  value.state = EngineValueState::sql_null;
  return value;
}

std::string FormatAggregateReal(const long double value) {
  std::ostringstream stream;
  stream << std::setprecision(17) << static_cast<double>(value);
  return stream.str();
}

bool CompareAggregateValues(const EngineTypedValue& left,
                            const EngineTypedValue& right,
                            int* comparison,
                            std::string* detail) {
  if (comparison == nullptr || detail == nullptr) return false;
  CanonicalDescriptorOrderTerm term;
  const auto compared = CompareCanonicalDescriptorOrderValues(left, right,
                                                               term);
  if (!compared.diagnostic.ok) {
    *detail = compared.diagnostic.detail;
    return false;
  }
  *comparison = compared.comparison;
  detail->clear();
  return true;
}

bool TransitionCanonicalAggregateCore(
    const CanonicalAggregateDescriptor& descriptor,
    const std::vector<EngineTypedValue>& values,
    CanonicalAggregateCoreState* state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (state == nullptr || diagnostic == nullptr) return false;
  ++state->transition_count;
  if (descriptor.function == CanonicalAggregateFunction::count &&
      descriptor.count_star) {
    ++state->non_null_count;
    return true;
  }
  const std::size_t expected_arity = CanonicalAggregateExpectedArity(
      descriptor.function, descriptor.count_star);
  if (values.size() != expected_arity) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-ARITY-V1",
                          "aggregate transition arity is not bound");
    return false;
  }
  if (expected_arity == 2) {
    if (descriptor.function == CanonicalAggregateFunction::json_object_agg) {
      const auto& key = values[0];
      const auto& value = values[1];
      if (key.state == EngineValueState::sql_null) {
        *diagnostic = Refusal(
            "QOW-DIAG-QRY-011-REGISTRY-JSON-KEY-V1",
            "JSON object aggregate key cannot be SQL NULL");
        return false;
      }
      std::string rendered;
      if (!RenderAggregateJsonValue(value, &rendered, diagnostic)) {
        return false;
      }
      const auto key_text = key.encoded_value;
      const auto existing = std::find_if(
          state->json_object_values.begin(), state->json_object_values.end(),
          [&](const auto& member) { return member.first == key_text; });
      if (existing != state->json_object_values.end()) {
        state->json_object_values.erase(existing);
      }
      state->json_object_values.emplace_back(key_text, std::move(rendered));
      ++state->non_null_count;
      return true;
    }
    const auto& y_value = values[0];
    const auto& x_value = values[1];
    if (y_value.state == EngineValueState::sql_null ||
        x_value.state == EngineValueState::sql_null) {
      return true;
    }
    long double y = 0.0L;
    long double x = 0.0L;
    if (!DecodeAggregateNumeric(y_value, &y, diagnostic) ||
        !DecodeAggregateNumeric(x_value, &x, diagnostic)) {
      return false;
    }
    ++state->non_null_count;
    const auto count = static_cast<long double>(state->non_null_count);
    const auto delta_x = x - state->pair_mean_x;
    state->pair_mean_x += delta_x / count;
    const auto delta_y = y - state->pair_mean_y;
    state->pair_mean_y += delta_y / count;
    state->pair_m2_x += delta_x * (x - state->pair_mean_x);
    state->pair_m2_y += delta_y * (y - state->pair_mean_y);
    state->pair_comoment += delta_x * (y - state->pair_mean_y);
    return true;
  }
  const auto& value = values.front();
  if (descriptor.function == CanonicalAggregateFunction::array_agg) {
    state->collection_values.push_back(value);
    if (value.state != EngineValueState::sql_null) ++state->non_null_count;
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::json_agg) {
    std::string rendered;
    if (!RenderAggregateJsonValue(value, &rendered, diagnostic)) return false;
    state->text_values.push_back(std::move(rendered));
    if (value.state != EngineValueState::sql_null) ++state->non_null_count;
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::string_agg ||
      descriptor.function == CanonicalAggregateFunction::listagg) {
    if (value.state == EngineValueState::sql_null) return true;
    state->text_values.push_back(value.encoded_value);
    ++state->non_null_count;
    return true;
  }
  if (IsCanonicalHypotheticalSetFunction(descriptor.function) ||
      IsCanonicalQuantileFunction(descriptor.function)) {
    if (value.state == EngineValueState::sql_null) return true;
    long double numeric = 0.0L;
    if (!DecodeAggregateNumeric(value, &numeric, diagnostic)) return false;
    state->ordered_numeric_values.push_back(numeric);
    ++state->non_null_count;
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::mode ||
      descriptor.function == CanonicalAggregateFunction::approx_top_k) {
    if (value.state == EngineValueState::sql_null) return true;
    AddCanonicalFrequencyValue(state, value);
    ++state->non_null_count;
    return true;
  }
  if (descriptor.function ==
      CanonicalAggregateFunction::approx_count_distinct) {
    if (value.state == EngineValueState::sql_null) return true;
    std::string identity = value.descriptor.descriptor_uuid.canonical + ":" +
                           value.descriptor.canonical_type_name + ":" +
                           value.encoded_value;
    identity.append(reinterpret_cast<const char*>(value.binary_value.data()),
                    value.binary_value.size());
    if (std::find(state->approximate_distinct_values.begin(),
                  state->approximate_distinct_values.end(), identity) ==
        state->approximate_distinct_values.end()) {
      state->approximate_distinct_values.push_back(std::move(identity));
    }
    ++state->non_null_count;
    return true;
  }
  if (value.state == EngineValueState::sql_null) return true;
  ++state->non_null_count;

  switch (descriptor.function) {
    case CanonicalAggregateFunction::count:
      return true;
    case CanonicalAggregateFunction::sum:
    case CanonicalAggregateFunction::avg:
      if (IsCanonicalBoundedSignedIntegerDescriptor(value.descriptor)) {
        const auto decoded = DecodeInt64Value(value);
        if (!decoded.ok()) {
          *diagnostic = decoded.diagnostic;
          return false;
        }
        state->int64_sum += static_cast<__int128>(decoded.value);
        state->real_sum += static_cast<long double>(decoded.value);
        return true;
      }
      if (IsType(value, "real64")) {
        const auto decoded = DecodeReal64Value(value);
        if (!decoded.ok()) {
          *diagnostic = decoded.diagnostic;
          return false;
        }
        state->real_sum += static_cast<long double>(decoded.value);
        if (!std::isfinite(state->real_sum)) {
          *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                                "floating aggregate state is not finite");
          return false;
        }
        return true;
      }
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
                            "SUM and AVG require bounded signed integer or "
                            "real64 input");
      return false;
    case CanonicalAggregateFunction::stddev_pop:
    case CanonicalAggregateFunction::variance_pop:
    case CanonicalAggregateFunction::stddev:
    case CanonicalAggregateFunction::variance:
    case CanonicalAggregateFunction::stddev_samp:
    case CanonicalAggregateFunction::variance_samp: {
      long double numeric = 0.0L;
      if (!DecodeAggregateNumeric(value, &numeric, diagnostic)) return false;
      const auto count = static_cast<long double>(state->non_null_count);
      const auto delta = numeric - state->numeric_mean;
      state->numeric_mean += delta / count;
      const auto delta2 = numeric - state->numeric_mean;
      state->numeric_m2 += delta * delta2;
      return true;
    }
    case CanonicalAggregateFunction::min:
    case CanonicalAggregateFunction::max: {
      if (!state->extremum.has_value()) {
        state->extremum = value;
        return true;
      }
      int comparison = 0;
      std::string detail;
      if (!CompareAggregateValues(value, *state->extremum, &comparison,
                                  &detail)) {
        *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-COMPARE-V1",
                              std::move(detail));
        return false;
      }
      if ((descriptor.function == CanonicalAggregateFunction::min &&
           comparison < 0) ||
          (descriptor.function == CanonicalAggregateFunction::max &&
           comparison > 0)) {
        state->extremum = value;
      }
      return true;
    }
    case CanonicalAggregateFunction::bool_and:
    case CanonicalAggregateFunction::bool_or:
    case CanonicalAggregateFunction::every: {
      const auto decoded = DecodeBoolValue(value);
      if (!decoded.ok()) {
        *diagnostic = decoded.diagnostic;
        return false;
      }
      state->saw_true = state->saw_true || decoded.value;
      state->saw_false = state->saw_false || !decoded.value;
      return true;
    }
    default:
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-UNIMPLEMENTED-V1",
                            "aggregate has no canonical core state");
      return false;
  }
}

bool InverseCanonicalAggregateCore(
    const CanonicalAggregateDescriptor& descriptor,
    const std::vector<EngineTypedValue>& values,
    CanonicalAggregateCoreState* state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (state == nullptr || diagnostic == nullptr) return false;
  if (descriptor.function != CanonicalAggregateFunction::count &&
      descriptor.function != CanonicalAggregateFunction::sum &&
      descriptor.function != CanonicalAggregateFunction::avg) {
    *diagnostic = Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-UNAVAILABLE-V1",
        "aggregate has no admitted inverse transition state");
    return false;
  }
  if (state->transition_count == 0) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-INVERSE-STATE-V1",
                          "inverse transition underflowed aggregate state");
    return false;
  }
  if (descriptor.function == CanonicalAggregateFunction::count &&
      descriptor.count_star) {
    if (!values.empty() || state->non_null_count == 0) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-INVERSE-STATE-V1",
                            "COUNT(*) inverse state is inconsistent");
      return false;
    }
    --state->transition_count;
    --state->non_null_count;
    return true;
  }
  if (values.size() != 1) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-ARITY-V1",
                          "inverse aggregate transition arity is not bound");
    return false;
  }
  const auto& value = values.front();
  if (value.state == EngineValueState::sql_null) {
    --state->transition_count;
    return true;
  }
  if (state->non_null_count == 0) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-INVERSE-STATE-V1",
                          "inverse non-NULL transition underflowed state");
    return false;
  }
  std::int64_t decoded_value = 0;
  if (descriptor.function == CanonicalAggregateFunction::sum ||
      descriptor.function == CanonicalAggregateFunction::avg) {
    if (!IsCanonicalBoundedSignedIntegerDescriptor(value.descriptor)) {
      *diagnostic = Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-INVERSE-TYPE-V1",
          "moving SUM and AVG inverse state currently requires bounded "
          "signed integer input");
      return false;
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      *diagnostic = decoded.diagnostic;
      return false;
    }
    decoded_value = decoded.value;
  }
  --state->transition_count;
  --state->non_null_count;
  if (descriptor.function == CanonicalAggregateFunction::sum ||
      descriptor.function == CanonicalAggregateFunction::avg) {
    state->int64_sum -= static_cast<__int128>(decoded_value);
    state->real_sum -= static_cast<long double>(decoded_value);
    if (state->non_null_count == 0) {
      state->int64_sum = 0;
      state->real_sum = 0.0L;
    }
  }
  return true;
}

bool MergeCanonicalAggregateCore(
    const CanonicalAggregateDescriptor& descriptor,
    CanonicalAggregateCoreState* target,
    const CanonicalAggregateCoreState& source,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (target == nullptr || diagnostic == nullptr) return false;
  target->transition_count += source.transition_count;
  if (IsCanonicalUnivariateStatisticalFunction(descriptor.function)) {
    const auto left_count = target->non_null_count;
    const auto right_count = source.non_null_count;
    if (right_count == 0) return true;
    if (left_count == 0) {
      target->non_null_count = source.non_null_count;
      target->numeric_mean = source.numeric_mean;
      target->numeric_m2 = source.numeric_m2;
      return true;
    }
    const auto left = static_cast<long double>(left_count);
    const auto right = static_cast<long double>(right_count);
    const auto total = left + right;
    const auto delta = source.numeric_mean - target->numeric_mean;
    target->numeric_m2 +=
        source.numeric_m2 + delta * delta * left * right / total;
    target->numeric_mean += delta * right / total;
    target->non_null_count += source.non_null_count;
    return true;
  }
  if (IsCanonicalPairStatisticalFunction(descriptor.function)) {
    const auto left_count = target->non_null_count;
    const auto right_count = source.non_null_count;
    if (right_count == 0) return true;
    if (left_count == 0) {
      target->non_null_count = source.non_null_count;
      target->pair_mean_x = source.pair_mean_x;
      target->pair_mean_y = source.pair_mean_y;
      target->pair_m2_x = source.pair_m2_x;
      target->pair_m2_y = source.pair_m2_y;
      target->pair_comoment = source.pair_comoment;
      return true;
    }
    const auto left = static_cast<long double>(left_count);
    const auto right = static_cast<long double>(right_count);
    const auto total = left + right;
    const auto delta_x = source.pair_mean_x - target->pair_mean_x;
    const auto delta_y = source.pair_mean_y - target->pair_mean_y;
    target->pair_comoment +=
        source.pair_comoment + delta_x * delta_y * left * right / total;
    target->pair_m2_x +=
        source.pair_m2_x + delta_x * delta_x * left * right / total;
    target->pair_m2_y +=
        source.pair_m2_y + delta_y * delta_y * left * right / total;
    target->pair_mean_x += delta_x * right / total;
    target->pair_mean_y += delta_y * right / total;
    target->non_null_count += source.non_null_count;
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::array_agg) {
    target->non_null_count += source.non_null_count;
    target->collection_values.insert(target->collection_values.end(),
                                     source.collection_values.begin(),
                                     source.collection_values.end());
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::string_agg ||
      descriptor.function == CanonicalAggregateFunction::listagg ||
      descriptor.function == CanonicalAggregateFunction::json_agg) {
    target->non_null_count += source.non_null_count;
    target->text_values.insert(target->text_values.end(),
                               source.text_values.begin(),
                               source.text_values.end());
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::json_object_agg) {
    target->non_null_count += source.non_null_count;
    for (const auto& member : source.json_object_values) {
      const auto existing = std::find_if(
          target->json_object_values.begin(), target->json_object_values.end(),
          [&](const auto& candidate) {
            return candidate.first == member.first;
          });
      if (existing != target->json_object_values.end()) {
        target->json_object_values.erase(existing);
      }
      target->json_object_values.push_back(member);
    }
    return true;
  }
  if (IsCanonicalHypotheticalSetFunction(descriptor.function) ||
      IsCanonicalQuantileFunction(descriptor.function)) {
    target->non_null_count += source.non_null_count;
    target->ordered_numeric_values.insert(
        target->ordered_numeric_values.end(),
        source.ordered_numeric_values.begin(),
        source.ordered_numeric_values.end());
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::mode ||
      descriptor.function == CanonicalAggregateFunction::approx_top_k) {
    target->non_null_count += source.non_null_count;
    for (const auto& [value, count] : source.frequency_values) {
      AddCanonicalFrequencyValue(target, value, count);
    }
    return true;
  }
  if (descriptor.function ==
      CanonicalAggregateFunction::approx_count_distinct) {
    target->non_null_count += source.non_null_count;
    for (const auto& identity : source.approximate_distinct_values) {
      if (std::find(target->approximate_distinct_values.begin(),
                    target->approximate_distinct_values.end(), identity) ==
          target->approximate_distinct_values.end()) {
        target->approximate_distinct_values.push_back(identity);
      }
    }
    return true;
  }
  target->non_null_count += source.non_null_count;
  target->int64_sum += source.int64_sum;
  target->real_sum += source.real_sum;
  target->saw_true = target->saw_true || source.saw_true;
  target->saw_false = target->saw_false || source.saw_false;
  if (!source.extremum.has_value()) return true;
  if (!target->extremum.has_value()) {
    target->extremum = source.extremum;
    return true;
  }
  int comparison = 0;
  std::string detail;
  if (!CompareAggregateValues(*source.extremum, *target->extremum,
                              &comparison, &detail)) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-COMPARE-V1",
                          std::move(detail));
    return false;
  }
  if ((descriptor.function == CanonicalAggregateFunction::min &&
       comparison < 0) ||
      (descriptor.function == CanonicalAggregateFunction::max &&
       comparison > 0)) {
    target->extremum = source.extremum;
  }
  return true;
}

bool ValidateCanonicalAggregateResultType(
    const CanonicalAggregateRuntimeRequest& request,
    DescriptorRuntimeDiagnostic* diagnostic) {
  const auto function = request.descriptor.function;
  if (function == CanonicalAggregateFunction::count) {
    if (IsType(request.result_column, "int64") &&
        !request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::regr_count) {
    if (IsType(request.result_column, "int64") &&
        !request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::rank ||
             function == CanonicalAggregateFunction::dense_rank ||
             function ==
                 CanonicalAggregateFunction::approx_count_distinct) {
    if (IsType(request.result_column, "int64") &&
        !request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::percent_rank ||
             function == CanonicalAggregateFunction::cume_dist) {
    if (IsType(request.result_column, "real64") &&
        !request.result_column.nullable) {
      return true;
    }
  } else if (IsCanonicalQuantileFunction(function)) {
    if (IsType(request.result_column, "real64") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::approx_top_k) {
    if (IsType(request.result_column, "json") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (IsCanonicalUnivariateStatisticalFunction(function) ||
             IsCanonicalPairStatisticalFunction(function)) {
    if (IsType(request.result_column, "real64") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::sum &&
             !request.value_columns.empty()) {
    const auto& input =
        request.input_batch.columns[request.value_columns.front()];
    if (request.result_column.nullable &&
        ((IsCanonicalBoundedSignedIntegerDescriptor(input.descriptor) &&
          IsType(request.result_column, "int64")) ||
         (input.descriptor.canonical_type_name == "real64" &&
          IsType(request.result_column, "real64")))) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::avg) {
    if (IsType(request.result_column, "real64") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::bool_and ||
             function == CanonicalAggregateFunction::bool_or ||
             function == CanonicalAggregateFunction::every) {
    if (IsType(request.result_column, "boolean") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::array_agg) {
    const auto& type = request.result_column.descriptor.canonical_type_name;
    if (request.result_column.nullable &&
        (type.starts_with("list<") || type.starts_with("array<"))) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::string_agg ||
             function == CanonicalAggregateFunction::listagg) {
    if (IsType(request.result_column, "text") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (function == CanonicalAggregateFunction::json_agg ||
             function == CanonicalAggregateFunction::json_object_agg) {
    if (IsType(request.result_column, "json") &&
        request.result_column.nullable) {
      return true;
    }
  } else if (!request.value_columns.empty()) {
    const auto& input = request.input_batch.columns[request.value_columns[0]];
    if (request.result_column.descriptor.canonical_type_name ==
            input.descriptor.canonical_type_name &&
        request.result_column.nullable) {
      return true;
    }
  }
  *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-RESULT-TYPE-V1",
                        "aggregate result descriptor is not canonical");
  return false;
}

bool ValidateCanonicalAggregateInputType(
    const CanonicalAggregateRuntimeRequest& request,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (request.descriptor.count_star ||
      request.descriptor.function == CanonicalAggregateFunction::count) {
    return true;
  }
  if (IsCanonicalPairStatisticalFunction(request.descriptor.function)) {
    for (const auto column : request.value_columns) {
      const auto& type = request.input_batch.columns[column]
                             .descriptor.canonical_type_name;
      if (type != "int64" && type != "real64") {
        *diagnostic = Refusal(
            "QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
            "pair statistical aggregate requires two numeric descriptors");
        return false;
      }
    }
    return true;
  }
  if (request.descriptor.function ==
      CanonicalAggregateFunction::json_object_agg) {
    const auto& key_type = request.input_batch
                               .columns[request.value_columns.front()]
                               .descriptor.canonical_type_name;
    if (key_type == "text" || key_type == "varchar" ||
        key_type == "char") {
      return true;
    }
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
                          "JSON object aggregate key must be text");
    return false;
  }
  if (request.descriptor.function == CanonicalAggregateFunction::string_agg ||
      request.descriptor.function == CanonicalAggregateFunction::listagg) {
    const auto& type = request.input_batch
                           .columns[request.value_columns.front()]
                           .descriptor.canonical_type_name;
    if (type == "text" || type == "varchar" || type == "char") return true;
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
                          "string aggregate input must be text");
    return false;
  }
  if (request.descriptor.function == CanonicalAggregateFunction::array_agg ||
      request.descriptor.function == CanonicalAggregateFunction::json_agg) {
    return true;
  }
  if (IsCanonicalHypotheticalSetFunction(request.descriptor.function) ||
      IsCanonicalQuantileFunction(request.descriptor.function)) {
    const auto& type = request.input_batch
                           .columns[request.value_columns.front()]
                           .descriptor.canonical_type_name;
    if (IsCanonicalBoundedSignedIntegerDescriptor(
            request.input_batch
                .columns[request.value_columns.front()]
                .descriptor) ||
        type == "real64") {
      return true;
    }
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
                          "ordered numeric aggregate requires numeric input");
    return false;
  }
  if (request.descriptor.function == CanonicalAggregateFunction::mode ||
      request.descriptor.function ==
          CanonicalAggregateFunction::approx_count_distinct ||
      request.descriptor.function ==
          CanonicalAggregateFunction::approx_top_k) {
    return true;
  }
  const auto& type = request.input_batch
                         .columns[request.value_columns.front()]
                         .descriptor.canonical_type_name;
  if (request.descriptor.function == CanonicalAggregateFunction::sum ||
      request.descriptor.function == CanonicalAggregateFunction::avg ||
      IsCanonicalUnivariateStatisticalFunction(
          request.descriptor.function)) {
    if (IsCanonicalBoundedSignedIntegerDescriptor(
            request.input_batch
                .columns[request.value_columns.front()]
                .descriptor) ||
        type == "real64") {
      return true;
    }
  } else if (request.descriptor.function ==
                 CanonicalAggregateFunction::bool_and ||
             request.descriptor.function ==
                 CanonicalAggregateFunction::bool_or ||
             request.descriptor.function ==
                 CanonicalAggregateFunction::every) {
    if (type == "boolean") return true;
  } else {
    return true;
  }
  *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
                        "aggregate input descriptor is not admitted");
  return false;
}

EngineTypedValue FinalizeCanonicalAggregateCore(
    const CanonicalAggregateRuntimeRequest& request,
    const CanonicalAggregateCoreState& state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  const auto function = request.descriptor.function;
  if (function == CanonicalAggregateFunction::count) {
    if (state.non_null_count >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "COUNT exceeds int64 result profile");
      return {};
    }
    return AggregateValue(request.result_column,
                          std::to_string(state.non_null_count));
  }
  if (function == CanonicalAggregateFunction::array_agg) {
    if (state.transition_count == 0) return AggregateNull(request.result_column);
    std::string encoded = "list[";
    for (std::size_t index = 0; index < state.collection_values.size();
         ++index) {
      if (index != 0) encoded.push_back(';');
      const auto& value = state.collection_values[index];
      if (value.state == EngineValueState::sql_null) {
        encoded += "NULL";
      } else {
        encoded += value.descriptor.canonical_type_name;
        encoded.push_back(':');
        encoded += value.encoded_value;
      }
    }
    encoded.push_back(']');
    return AggregateValue(request.result_column, std::move(encoded));
  }
  if (function == CanonicalAggregateFunction::json_agg) {
    if (state.transition_count == 0) return AggregateNull(request.result_column);
    return AggregateValue(
        request.result_column,
        "[" + JoinAggregateTextValues(state.text_values,
                                       state.text_values.size(), ",") +
            "]");
  }
  if (function == CanonicalAggregateFunction::json_object_agg) {
    if (state.transition_count == 0) return AggregateNull(request.result_column);
    std::string encoded = "{";
    for (std::size_t index = 0; index < state.json_object_values.size();
         ++index) {
      if (index != 0) encoded.push_back(',');
      encoded += EscapeAggregateJson(state.json_object_values[index].first);
      encoded.push_back(':');
      encoded += state.json_object_values[index].second;
    }
    encoded.push_back('}');
    return AggregateValue(request.result_column, std::move(encoded));
  }
  if (function == CanonicalAggregateFunction::string_agg ||
      function == CanonicalAggregateFunction::listagg) {
    if (state.text_values.empty()) return AggregateNull(request.result_column);
    const auto full_text = JoinAggregateTextValues(
        state.text_values, state.text_values.size(),
        request.aggregate_separator);
    if (function == CanonicalAggregateFunction::string_agg ||
        request.listagg_overflow_mode == CanonicalListaggOverflowMode::none ||
        full_text.size() <= request.listagg_max_output_bytes) {
      return AggregateValue(request.result_column, full_text);
    }
    if (request.listagg_overflow_mode ==
        CanonicalListaggOverflowMode::error) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
                            "LISTAGG result exceeds its bound output bytes");
      return {};
    }
    const auto suffix = [&](const std::size_t truncated_count) {
      auto value = request.listagg_truncation_indicator.empty()
                       ? std::string("...")
                       : request.listagg_truncation_indicator;
      if (request.listagg_with_count) {
        value += "(" + std::to_string(truncated_count) + ")";
      }
      return value;
    };
    for (std::size_t retained = state.text_values.size(); retained > 0;
         --retained) {
      const auto truncated = state.text_values.size() - retained;
      if (truncated == 0) continue;
      auto candidate = JoinAggregateTextValues(
          state.text_values, retained, request.aggregate_separator);
      candidate += request.aggregate_separator;
      candidate += suffix(truncated);
      if (candidate.size() <= request.listagg_max_output_bytes) {
        return AggregateValue(request.result_column, std::move(candidate));
      }
    }
    auto suffix_only = suffix(state.text_values.size());
    if (suffix_only.size() <= request.listagg_max_output_bytes) {
      return AggregateValue(request.result_column, std::move(suffix_only));
    }
    *diagnostic = Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-INDICATOR-V1",
        "LISTAGG truncation indicator cannot fit its output bound");
    return {};
  }
  if (IsCanonicalHypotheticalSetFunction(function)) {
    long double hypothetical = 0.0L;
    if (!DecodeAggregateNumeric(request.direct_arguments.front(),
                                &hypothetical, diagnostic)) {
      return {};
    }
    std::vector<long double> values = state.ordered_numeric_values;
    std::sort(values.begin(), values.end());
    const auto less = static_cast<std::size_t>(std::count_if(
        values.begin(), values.end(),
        [&](const auto value) { return value < hypothetical; }));
    const auto less_equal = static_cast<std::size_t>(std::count_if(
        values.begin(), values.end(),
        [&](const auto value) { return value <= hypothetical; }));
    if (function == CanonicalAggregateFunction::rank) {
      if (less >= static_cast<std::size_t>(
                      std::numeric_limits<std::int64_t>::max())) {
        *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                              "hypothetical RANK exceeds int64");
        return {};
      }
      return AggregateValue(request.result_column, std::to_string(less + 1));
    }
    if (function == CanonicalAggregateFunction::dense_rank) {
      values.erase(std::unique(values.begin(), values.end()), values.end());
      const auto distinct_less = static_cast<std::size_t>(std::count_if(
          values.begin(), values.end(),
          [&](const auto value) { return value < hypothetical; }));
      if (distinct_less >= static_cast<std::size_t>(
                               std::numeric_limits<std::int64_t>::max())) {
        *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                              "hypothetical DENSE_RANK exceeds int64");
        return {};
      }
      return AggregateValue(request.result_column,
                            std::to_string(distinct_less + 1));
    }
    long double result_value = 0.0L;
    if (function == CanonicalAggregateFunction::percent_rank) {
      const auto denominator = values.empty()
                                   ? 1.0L
                                   : static_cast<long double>(values.size());
      result_value = static_cast<long double>(less) / denominator;
    } else {
      result_value = static_cast<long double>(less_equal + 1) /
                     static_cast<long double>(values.size() + 1);
    }
    return AggregateValue(request.result_column,
                          FormatAggregateReal(result_value));
  }
  if (IsCanonicalQuantileFunction(function)) {
    if (state.ordered_numeric_values.empty()) {
      return AggregateNull(request.result_column);
    }
    long double fraction = 0.5L;
    if (function != CanonicalAggregateFunction::approx_median &&
        !DecodeAggregateNumeric(request.direct_arguments.front(), &fraction,
                                diagnostic)) {
      return {};
    }
    auto values = state.ordered_numeric_values;
    std::sort(values.begin(), values.end());
    long double quantile = 0.0L;
    if (function == CanonicalAggregateFunction::percentile_disc ||
        function == CanonicalAggregateFunction::approx_percentile_disc) {
      const auto scaled = fraction * static_cast<long double>(values.size());
      const auto index = fraction <= 0.0L
                             ? 0U
                             : static_cast<std::size_t>(std::ceil(scaled) -
                                                        1.0L);
      quantile = values[std::min(index, values.size() - 1)];
    } else {
      const auto scaled =
          fraction * static_cast<long double>(values.size() - 1);
      const auto lower = static_cast<std::size_t>(std::floor(scaled));
      const auto upper = static_cast<std::size_t>(std::ceil(scaled));
      const auto weight = scaled - static_cast<long double>(lower);
      quantile = values[lower] + (values[upper] - values[lower]) * weight;
    }
    if (!std::isfinite(static_cast<double>(quantile))) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "quantile result exceeds real64");
      return {};
    }
    return AggregateValue(request.result_column,
                          FormatAggregateReal(quantile));
  }
  if (function == CanonicalAggregateFunction::approx_count_distinct) {
    if (state.approximate_distinct_values.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "approximate distinct count exceeds int64");
      return {};
    }
    return AggregateValue(
        request.result_column,
        std::to_string(state.approximate_distinct_values.size()));
  }
  if (function == CanonicalAggregateFunction::mode) {
    if (state.frequency_values.empty()) return AggregateNull(request.result_column);
    std::size_t selected = 0;
    for (std::size_t index = 1; index < state.frequency_values.size(); ++index) {
      bool replace = state.frequency_values[index].second >
                     state.frequency_values[selected].second;
      if (!replace && state.frequency_values[index].second ==
                          state.frequency_values[selected].second) {
        int comparison = 0;
        std::string detail;
        if (!CompareAggregateValues(state.frequency_values[index].first,
                                    state.frequency_values[selected].first,
                                    &comparison, &detail)) {
          *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-COMPARE-V1",
                                std::move(detail));
          return {};
        }
        replace = comparison < 0;
      }
      if (replace) selected = index;
    }
    auto value = state.frequency_values[selected].first;
    value.descriptor = request.result_column.descriptor;
    return value;
  }
  if (function == CanonicalAggregateFunction::approx_top_k) {
    if (state.frequency_values.empty()) return AggregateNull(request.result_column);
    const auto decoded = DecodeInt64Value(request.direct_arguments.front());
    if (!decoded.ok()) {
      *diagnostic = decoded.diagnostic;
      return {};
    }
    std::vector<std::size_t> order(state.frequency_values.size());
    std::iota(order.begin(), order.end(), 0);
    bool comparison_failed = false;
    std::string comparison_detail;
    std::stable_sort(order.begin(), order.end(), [&](const auto left,
                                                     const auto right) {
      if (state.frequency_values[left].second !=
          state.frequency_values[right].second) {
        return state.frequency_values[left].second >
               state.frequency_values[right].second;
      }
      int comparison = 0;
      if (!CompareAggregateValues(state.frequency_values[left].first,
                                  state.frequency_values[right].first,
                                  &comparison, &comparison_detail)) {
        comparison_failed = true;
        return left < right;
      }
      return comparison < 0;
    });
    if (comparison_failed) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-COMPARE-V1",
                            std::move(comparison_detail));
      return {};
    }
    const auto limit = std::min(
        static_cast<std::size_t>(decoded.value), order.size());
    std::string encoded = "[";
    for (std::size_t rank = 0; rank < limit; ++rank) {
      if (rank != 0) encoded.push_back(',');
      const auto index = order[rank];
      encoded += "{\"value\":" + EscapeAggregateJson(
          state.frequency_values[index].first.encoded_value);
      encoded += ",\"count\":" +
                 std::to_string(state.frequency_values[index].second) + "}";
    }
    encoded.push_back(']');
    return AggregateValue(request.result_column, std::move(encoded));
  }
  if (function == CanonicalAggregateFunction::regr_count) {
    if (state.non_null_count > static_cast<std::size_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "REGR_COUNT exceeds int64 result profile");
      return {};
    }
    return AggregateValue(request.result_column,
                          std::to_string(state.non_null_count));
  }
  if (state.non_null_count == 0) return AggregateNull(request.result_column);
  if (function == CanonicalAggregateFunction::sum) {
    if (IsType(request.result_column, "int64")) {
      if (state.int64_sum <
              static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
          state.int64_sum >
              static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
        *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                              "SUM exceeds int64 result width");
        return {};
      }
      return AggregateValue(
          request.result_column,
          std::to_string(static_cast<std::int64_t>(state.int64_sum)));
    }
    if (!std::isfinite(static_cast<double>(state.real_sum))) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "SUM exceeds real64 result width");
      return {};
    }
    return AggregateValue(request.result_column,
                          FormatAggregateReal(state.real_sum));
  }
  if (function == CanonicalAggregateFunction::avg) {
    const auto average = state.real_sum /
                         static_cast<long double>(state.non_null_count);
    if (!std::isfinite(static_cast<double>(average))) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "AVG exceeds real64 result width");
      return {};
    }
    return AggregateValue(request.result_column,
                          FormatAggregateReal(average));
  }
  if (IsCanonicalUnivariateStatisticalFunction(function)) {
    const bool population =
        function == CanonicalAggregateFunction::variance_pop ||
        function == CanonicalAggregateFunction::stddev_pop;
    if (state.non_null_count < (population ? 1U : 2U)) {
      return AggregateNull(request.result_column);
    }
    const auto denominator = static_cast<long double>(
        population ? state.non_null_count : state.non_null_count - 1);
    auto statistic = state.numeric_m2 / denominator;
    if (statistic < 0.0L && statistic > -1e-18L) statistic = 0.0L;
    if (function == CanonicalAggregateFunction::stddev_pop ||
        function == CanonicalAggregateFunction::stddev ||
        function == CanonicalAggregateFunction::stddev_samp) {
      statistic = std::sqrt(statistic);
    }
    if (!std::isfinite(static_cast<double>(statistic))) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "statistical result exceeds real64 width");
      return {};
    }
    return AggregateValue(request.result_column,
                          FormatAggregateReal(statistic));
  }
  if (IsCanonicalPairStatisticalFunction(function)) {
    if (state.non_null_count == 0) return AggregateNull(request.result_column);
    long double statistic = 0.0L;
    switch (function) {
      case CanonicalAggregateFunction::corr:
        if (state.non_null_count < 2 || state.pair_m2_x <= 0.0L ||
            state.pair_m2_y <= 0.0L) {
          return AggregateNull(request.result_column);
        }
        statistic = state.pair_comoment /
                    std::sqrt(state.pair_m2_x * state.pair_m2_y);
        break;
      case CanonicalAggregateFunction::covar_pop:
        statistic = state.pair_comoment /
                    static_cast<long double>(state.non_null_count);
        break;
      case CanonicalAggregateFunction::covar_samp:
        if (state.non_null_count < 2) {
          return AggregateNull(request.result_column);
        }
        statistic = state.pair_comoment /
                    static_cast<long double>(state.non_null_count - 1);
        break;
      case CanonicalAggregateFunction::regr_avgx:
        statistic = state.pair_mean_x;
        break;
      case CanonicalAggregateFunction::regr_avgy:
        statistic = state.pair_mean_y;
        break;
      case CanonicalAggregateFunction::regr_intercept:
        if (state.pair_m2_x == 0.0L) {
          return AggregateNull(request.result_column);
        }
        statistic = state.pair_mean_y -
                    state.pair_mean_x * state.pair_comoment /
                        state.pair_m2_x;
        break;
      case CanonicalAggregateFunction::regr_r2:
        if (state.pair_m2_x == 0.0L) {
          return AggregateNull(request.result_column);
        }
        statistic = state.pair_m2_y == 0.0L
                        ? 1.0L
                        : state.pair_comoment * state.pair_comoment /
                              (state.pair_m2_x * state.pair_m2_y);
        break;
      case CanonicalAggregateFunction::regr_slope:
        if (state.pair_m2_x == 0.0L) {
          return AggregateNull(request.result_column);
        }
        statistic = state.pair_comoment / state.pair_m2_x;
        break;
      case CanonicalAggregateFunction::regr_sxx:
        statistic = state.pair_m2_x;
        break;
      case CanonicalAggregateFunction::regr_sxy:
        statistic = state.pair_comoment;
        break;
      case CanonicalAggregateFunction::regr_syy:
        statistic = state.pair_m2_y;
        break;
      default:
        *diagnostic = Refusal(
            "QOW-DIAG-QRY-011-REGISTRY-UNIMPLEMENTED-V1",
            "pair statistical final state is unavailable");
        return {};
    }
    if (!std::isfinite(static_cast<double>(statistic))) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "pair statistical result exceeds real64 width");
      return {};
    }
    return AggregateValue(request.result_column,
                          FormatAggregateReal(statistic));
  }
  if (function == CanonicalAggregateFunction::min ||
      function == CanonicalAggregateFunction::max) {
    auto value = *state.extremum;
    value.descriptor = request.result_column.descriptor;
    return value;
  }
  if (function == CanonicalAggregateFunction::bool_and ||
      function == CanonicalAggregateFunction::every) {
    return AggregateValue(request.result_column,
                          state.saw_false ? "false" : "true");
  }
  if (function == CanonicalAggregateFunction::bool_or) {
    return AggregateValue(request.result_column,
                          state.saw_true ? "true" : "false");
  }
  *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-UNIMPLEMENTED-V1",
                        "aggregate final state is unavailable");
  return {};
}

void AppendAggregateDistinctKeyField(std::string* key,
                                     const std::string_view field) {
  if (key == nullptr) return;
  const auto size = static_cast<std::uint64_t>(field.size());
  for (unsigned shift = 0; shift < 64; shift += 8) {
    key->push_back(static_cast<char>(size >> shift));
  }
  key->append(field);
}

std::string AggregateDistinctKey(const std::vector<EngineTypedValue>& values) {
  std::string key;
  key.reserve(values.size() * 64);
  for (const auto& value : values) {
    AppendAggregateDistinctKeyField(
        &key, value.descriptor.descriptor_uuid.canonical);
    AppendAggregateDistinctKeyField(&key,
                                    value.descriptor.canonical_type_name);
    key.push_back(static_cast<char>(value.state));
    key.push_back(value.is_null ? 1 : 0);
    AppendAggregateDistinctKeyField(&key, value.encoded_value);
    const auto binary = value.binary_value.empty()
                            ? std::string_view{}
                            : std::string_view(
                                  reinterpret_cast<const char*>(
                                      value.binary_value.data()),
                                  value.binary_value.size());
    AppendAggregateDistinctKeyField(&key, binary);
  }
  return key;
}

struct BoundAggregateTransition {
  std::size_t input_row = 0;
  std::vector<EngineTypedValue> values;
};

struct PreparedAggregateTransitions {
  std::vector<BoundAggregateTransition> transitions;
  std::size_t input_row_count = 0;
  std::size_t filtered_row_count = 0;
  std::size_t distinct_tuple_count = 0;
  std::size_t order_comparison_count = 0;
  bool aggregate_order_applied = false;
};

bool PrepareCanonicalAggregateTransitions(
    const CanonicalAggregateRuntimeRequest& request,
    PreparedAggregateTransitions* prepared,
    DescriptorRuntimeDiagnostic* diagnostic) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;
  if (prepared == nullptr || diagnostic == nullptr) return false;
  *prepared = {};
  prepared->input_row_count = request.input_batch.rows.size();
  prepared->transitions.reserve(request.input_batch.rows.size());
  std::set<std::string> distinct_keys;
  for (std::size_t row = 0; row < request.input_batch.rows.size(); ++row) {
    bool passes = true;
    if (request.filter_truth_values.has_value()) {
      std::string detail;
      if (!QowPredicateConsumerPassesV1(
              (*request.filter_truth_values)[row],
              EnginePredicateConsumer::filter, &passes, &detail)) {
        *diagnostic = Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                              std::move(detail));
        return false;
      }
    }
    if (!passes) continue;
    ++prepared->filtered_row_count;
    std::vector<EngineTypedValue> values;
    values.reserve(request.value_columns.size());
    for (const auto column : request.value_columns) {
      values.push_back(request.input_batch.rows[row].values[column]);
    }
    if (request.distinct) {
      if (!distinct_keys.insert(AggregateDistinctKey(values)).second) continue;
      if (distinct_keys.size() > request.maximum_distinct_value_count) {
        *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "aggregate DISTINCT bound is exceeded");
        return false;
      }
    }
    prepared->transitions.push_back({row, std::move(values)});
  }
  prepared->distinct_tuple_count = request.distinct ? distinct_keys.size() : 0;

  if (request.aggregate_order_terms.empty()) return true;
  const auto row_count = prepared->transitions.size();
  const auto term_count = request.aggregate_order_terms.size();
  if (request.maximum_order_comparison_count == 0 ||
      (row_count != 0 &&
       row_count > std::numeric_limits<std::size_t>::max() / row_count) ||
      (row_count * row_count != 0 &&
       term_count > std::numeric_limits<std::size_t>::max() /
                        (row_count * row_count)) ||
      row_count * row_count * term_count >
          request.maximum_order_comparison_count) {
    *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate order comparison bound is exceeded");
    return false;
  }
  std::vector<std::int8_t> comparisons(row_count * row_count, 0);
  for (std::size_t left = 0; left < row_count; ++left) {
    for (std::size_t right = left + 1; right < row_count; ++right) {
      int comparison = 0;
      for (const auto& term : request.aggregate_order_terms) {
        const auto compared = CompareCanonicalDescriptorOrderValues(
            request.input_batch.rows[prepared->transitions[left].input_row]
                .values[term.column],
            request.input_batch.rows[prepared->transitions[right].input_row]
                .values[term.column],
            term);
        ++prepared->order_comparison_count;
        if (!compared.diagnostic.ok) {
          *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-ORDER-V1",
                                compared.diagnostic.detail);
          return false;
        }
        comparison = compared.comparison;
        if (comparison != 0) break;
      }
      comparisons[left * row_count + right] =
          static_cast<std::int8_t>(comparison);
      comparisons[right * row_count + left] =
          static_cast<std::int8_t>(-comparison);
    }
  }
  std::vector<std::size_t> order(row_count);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](const auto left,
                                                    const auto right) {
    return comparisons[left * row_count + right] < 0;
  });
  std::vector<BoundAggregateTransition> ordered;
  ordered.reserve(row_count);
  for (const auto index : order) {
    ordered.push_back(std::move(prepared->transitions[index]));
  }
  prepared->transitions = std::move(ordered);
  prepared->aggregate_order_applied = true;
  return true;
}

bool BuildCanonicalAggregateCoreState(
    const CanonicalAggregateRuntimeRequest& request,
    const std::vector<BoundAggregateTransition>& transitions,
    CanonicalAggregateCoreState* state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (state == nullptr || diagnostic == nullptr) return false;
  *state = {};
  const auto state_within_bound = [&](const CanonicalAggregateCoreState& value) {
    return EstimateCanonicalAggregateStateBytes(value) <=
           request.maximum_state_bytes;
  };
  if (request.forced_strategy == CanonicalAggregateExecutionStrategy::serial) {
    for (const auto& transition : transitions) {
      if (!TransitionCanonicalAggregateCore(request.descriptor,
                                            transition.values, state,
                                            diagnostic)) {
        return false;
      }
      if (!state_within_bound(*state)) {
        *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "aggregate state byte bound is exceeded");
        return false;
      }
    }
    return true;
  }

  CanonicalAggregateCoreState left;
  CanonicalAggregateCoreState right;
  const auto split = transitions.size() / 2;
  for (std::size_t index = 0; index < transitions.size(); ++index) {
    auto* partition = index < split ? &left : &right;
    if (!TransitionCanonicalAggregateCore(request.descriptor,
                                          transitions[index].values,
                                          partition, diagnostic)) {
      return false;
    }
    if (!state_within_bound(*partition)) {
      *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "aggregate partial-state byte bound is exceeded");
      return false;
    }
  }
  if (!MergeCanonicalAggregateCore(request.descriptor, state, left,
                                   diagnostic) ||
      !MergeCanonicalAggregateCore(request.descriptor, state, right,
                                   diagnostic)) {
    return false;
  }
  if (!state_within_bound(*state)) {
    *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate merged-state byte bound is exceeded");
    return false;
  }
  return true;
}

using AggregateStateBytes = std::vector<std::uint8_t>;

void AppendStateU64(AggregateStateBytes* bytes, const std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendStateString(AggregateStateBytes* bytes,
                       const std::string_view value) {
  AppendStateU64(bytes, value.size());
  bytes->insert(bytes->end(), value.begin(), value.end());
}

std::string Int128StateText(const __int128 value) {
  if (value == 0) return "0";
  const bool negative = value < 0;
  unsigned __int128 magnitude = negative
                                    ? static_cast<unsigned __int128>(
                                          -(value + 1)) + 1
                                    : static_cast<unsigned __int128>(value);
  std::string text;
  while (magnitude != 0) {
    text.push_back(static_cast<char>('0' + magnitude % 10));
    magnitude /= 10;
  }
  if (negative) text.push_back('-');
  std::reverse(text.begin(), text.end());
  return text;
}

std::string LongDoubleStateText(const long double value) {
  std::ostringstream stream;
  stream << std::scientific
         << std::setprecision(std::numeric_limits<long double>::max_digits10)
         << value;
  return stream.str();
}

void AppendStateValue(AggregateStateBytes* bytes,
                      const EngineTypedValue& value) {
  AppendStateString(bytes, value.descriptor.descriptor_uuid.canonical);
  AppendStateString(bytes, value.descriptor.descriptor_kind);
  AppendStateString(bytes, value.descriptor.canonical_type_name);
  AppendStateString(bytes, value.descriptor.encoded_descriptor);
  AppendStateString(bytes, value.encoded_value);
  AppendStateU64(bytes, value.binary_value.size());
  bytes->insert(bytes->end(), value.binary_value.begin(),
                value.binary_value.end());
  bytes->push_back(value.is_null ? 1 : 0);
  bytes->push_back(static_cast<std::uint8_t>(value.state));
}

bool SerializeCanonicalAggregateCoreState(
    const CanonicalAggregateRuntimeRequest& request,
    const CanonicalAggregateCoreState& state,
    const std::size_t maximum_bytes,
    AggregateStateBytes* bytes) {
  if (bytes == nullptr || maximum_bytes == 0) return false;
  bytes->clear();
  AppendStateString(bytes, "scratchbird.aggregate-state.v1");
  AppendStateU64(bytes, request.descriptor.abi_version);
  AppendStateU64(bytes,
                 static_cast<std::uint8_t>(request.descriptor.function));
  AppendStateString(bytes, request.descriptor.builtin_id);
  AppendStateString(bytes, request.descriptor.function_uuid);
  bytes->push_back(request.descriptor.count_star ? 1 : 0);
  AppendStateU64(bytes,
                 static_cast<std::uint8_t>(request.forced_strategy));
  AppendStateU64(bytes, state.transition_count);
  AppendStateU64(bytes, state.non_null_count);
  AppendStateString(bytes, Int128StateText(state.int64_sum));
  for (const auto value :
       {state.real_sum, state.numeric_mean, state.numeric_m2,
        state.pair_mean_x, state.pair_mean_y, state.pair_m2_x,
        state.pair_m2_y, state.pair_comoment}) {
    if (!std::isfinite(value)) return false;
    AppendStateString(bytes, LongDoubleStateText(value));
  }
  bytes->push_back(state.saw_true ? 1 : 0);
  bytes->push_back(state.saw_false ? 1 : 0);
  bytes->push_back(state.extremum.has_value() ? 1 : 0);
  if (state.extremum.has_value()) AppendStateValue(bytes, *state.extremum);
  AppendStateU64(bytes, state.collection_values.size());
  for (const auto& value : state.collection_values) {
    AppendStateValue(bytes, value);
  }
  AppendStateU64(bytes, state.text_values.size());
  for (const auto& value : state.text_values) AppendStateString(bytes, value);
  AppendStateU64(bytes, state.json_object_values.size());
  for (const auto& [key, value] : state.json_object_values) {
    AppendStateString(bytes, key);
    AppendStateString(bytes, value);
  }
  AppendStateU64(bytes, state.ordered_numeric_values.size());
  for (const auto value : state.ordered_numeric_values) {
    if (!std::isfinite(value)) return false;
    AppendStateString(bytes, LongDoubleStateText(value));
  }
  AppendStateU64(bytes, state.approximate_distinct_values.size());
  for (const auto& value : state.approximate_distinct_values) {
    AppendStateString(bytes, value);
  }
  AppendStateU64(bytes, state.frequency_values.size());
  for (const auto& [value, count] : state.frequency_values) {
    AppendStateValue(bytes, value);
    AppendStateU64(bytes, count);
  }
  return bytes->size() <= maximum_bytes;
}

class AggregateStateReader {
 public:
  AggregateStateReader(const AggregateStateBytes& bytes,
                       const std::size_t maximum_bytes)
      : bytes_(bytes), maximum_bytes_(maximum_bytes) {}

  bool ReadU64(std::uint64_t* value) {
    if (value == nullptr || Remaining() < 8) return false;
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      *value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
    }
    return true;
  }

  bool ReadSize(std::size_t* value) {
    std::uint64_t decoded = 0;
    if (!ReadU64(&decoded) ||
        decoded > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    *value = static_cast<std::size_t>(decoded);
    return true;
  }

  bool ReadString(std::string* value) {
    std::size_t size = 0;
    if (value == nullptr || !ReadSize(&size) || size > Remaining() ||
        size > maximum_bytes_) {
      return false;
    }
    value->assign(reinterpret_cast<const char*>(bytes_.data() + offset_),
                  size);
    offset_ += size;
    return true;
  }

  bool ReadByte(std::uint8_t* value) {
    if (value == nullptr || Remaining() == 0) return false;
    *value = bytes_[offset_++];
    return true;
  }

  bool ReadValue(EngineTypedValue* value) {
    if (value == nullptr ||
        !ReadString(&value->descriptor.descriptor_uuid.canonical) ||
        !ReadString(&value->descriptor.descriptor_kind) ||
        !ReadString(&value->descriptor.canonical_type_name) ||
        !ReadString(&value->descriptor.encoded_descriptor) ||
        !ReadString(&value->encoded_value)) {
      return false;
    }
    std::size_t binary_size = 0;
    if (!ReadSize(&binary_size) || binary_size > Remaining() ||
        binary_size > maximum_bytes_) {
      return false;
    }
    value->binary_value.assign(bytes_.begin() + offset_,
                               bytes_.begin() + offset_ + binary_size);
    offset_ += binary_size;
    std::uint8_t is_null = 0;
    std::uint8_t state = 0;
    if (!ReadByte(&is_null) || is_null > 1 || !ReadByte(&state) ||
        state > static_cast<std::uint8_t>(EngineValueState::protected_value)) {
      return false;
    }
    value->is_null = is_null != 0;
    value->state = static_cast<EngineValueState>(state);
    return true;
  }

  std::size_t Remaining() const { return bytes_.size() - offset_; }

 private:
  const AggregateStateBytes& bytes_;
  std::size_t maximum_bytes_ = 0;
  std::size_t offset_ = 0;
};

bool ParseInt128StateText(const std::string_view text, __int128* value) {
  if (value == nullptr || text.empty()) return false;
  const bool negative = text.front() == '-';
  std::size_t index = negative ? 1 : 0;
  if (index == text.size()) return false;
  const unsigned __int128 negative_limit =
      static_cast<unsigned __int128>(1) << 127;
  const unsigned __int128 limit =
      negative ? negative_limit : negative_limit - 1;
  unsigned __int128 magnitude = 0;
  for (; index < text.size(); ++index) {
    if (text[index] < '0' || text[index] > '9') return false;
    const auto digit =
        static_cast<unsigned __int128>(text[index] - '0');
    if (magnitude > (limit - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }
  if (!negative) {
    *value = static_cast<__int128>(magnitude);
  } else if (magnitude == negative_limit) {
    *value = -static_cast<__int128>(magnitude - 1) - 1;
  } else {
    *value = -static_cast<__int128>(magnitude);
  }
  return true;
}

bool ParseLongDoubleStateText(const std::string& text, long double* value) {
  if (value == nullptr || text.empty()) return false;
  std::istringstream stream(text);
  stream >> *value;
  return stream && stream.peek() == std::char_traits<char>::eof() &&
         std::isfinite(*value);
}

bool DeserializeCanonicalAggregateCoreState(
    const CanonicalAggregateRuntimeRequest& request,
    const AggregateStateBytes& bytes,
    const std::size_t maximum_bytes,
    CanonicalAggregateCoreState* state) {
  if (state == nullptr || bytes.empty() || bytes.size() > maximum_bytes) {
    return false;
  }
  *state = {};
  AggregateStateReader reader(bytes, maximum_bytes);
  std::string text;
  std::uint64_t abi_version = 0;
  std::uint64_t function = 0;
  std::string builtin_id;
  std::string function_uuid;
  std::uint8_t count_star = 0;
  std::uint64_t strategy = 0;
  if (!reader.ReadString(&text) || text != "scratchbird.aggregate-state.v1" ||
      !reader.ReadU64(&abi_version) ||
      abi_version != request.descriptor.abi_version ||
      !reader.ReadU64(&function) ||
      function != static_cast<std::uint8_t>(request.descriptor.function) ||
      !reader.ReadString(&builtin_id) ||
      builtin_id != request.descriptor.builtin_id ||
      !reader.ReadString(&function_uuid) ||
      function_uuid != request.descriptor.function_uuid ||
      !reader.ReadByte(&count_star) || count_star > 1 ||
      (count_star != 0) != request.descriptor.count_star ||
      !reader.ReadU64(&strategy) ||
      strategy != static_cast<std::uint8_t>(request.forced_strategy) ||
      !reader.ReadSize(&state->transition_count) ||
      !reader.ReadSize(&state->non_null_count) ||
      !reader.ReadString(&text) ||
      !ParseInt128StateText(text, &state->int64_sum)) {
    return false;
  }
  for (auto* value :
       {&state->real_sum, &state->numeric_mean, &state->numeric_m2,
        &state->pair_mean_x, &state->pair_mean_y, &state->pair_m2_x,
        &state->pair_m2_y, &state->pair_comoment}) {
    if (!reader.ReadString(&text) ||
        !ParseLongDoubleStateText(text, value)) {
      return false;
    }
  }
  std::uint8_t saw_true = 0;
  std::uint8_t saw_false = 0;
  std::uint8_t has_extremum = 0;
  if (!reader.ReadByte(&saw_true) || saw_true > 1 ||
      !reader.ReadByte(&saw_false) || saw_false > 1 ||
      !reader.ReadByte(&has_extremum) || has_extremum > 1) {
    return false;
  }
  state->saw_true = saw_true != 0;
  state->saw_false = saw_false != 0;
  if (has_extremum != 0) {
    EngineTypedValue extremum;
    if (!reader.ReadValue(&extremum)) return false;
    state->extremum = std::move(extremum);
  }
  const auto read_values = [&](std::vector<EngineTypedValue>* values) {
    std::size_t count = 0;
    if (!reader.ReadSize(&count) || count > maximum_bytes) return false;
    values->reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      EngineTypedValue value;
      if (!reader.ReadValue(&value)) return false;
      values->push_back(std::move(value));
    }
    return true;
  };
  if (!read_values(&state->collection_values)) return false;
  const auto read_strings = [&](std::vector<std::string>* values) {
    std::size_t count = 0;
    if (!reader.ReadSize(&count) || count > maximum_bytes) return false;
    values->reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      std::string value;
      if (!reader.ReadString(&value)) return false;
      values->push_back(std::move(value));
    }
    return true;
  };
  if (!read_strings(&state->text_values)) return false;
  std::size_t json_count = 0;
  if (!reader.ReadSize(&json_count) || json_count > maximum_bytes) return false;
  state->json_object_values.reserve(json_count);
  for (std::size_t index = 0; index < json_count; ++index) {
    std::string key;
    std::string value;
    if (!reader.ReadString(&key) || !reader.ReadString(&value)) return false;
    state->json_object_values.emplace_back(std::move(key), std::move(value));
  }
  std::size_t numeric_count = 0;
  if (!reader.ReadSize(&numeric_count) || numeric_count > maximum_bytes) {
    return false;
  }
  state->ordered_numeric_values.reserve(numeric_count);
  for (std::size_t index = 0; index < numeric_count; ++index) {
    long double value = 0.0L;
    if (!reader.ReadString(&text) ||
        !ParseLongDoubleStateText(text, &value)) {
      return false;
    }
    state->ordered_numeric_values.push_back(value);
  }
  if (!read_strings(&state->approximate_distinct_values)) return false;
  std::size_t frequency_count = 0;
  if (!reader.ReadSize(&frequency_count) || frequency_count > maximum_bytes) {
    return false;
  }
  state->frequency_values.reserve(frequency_count);
  for (std::size_t index = 0; index < frequency_count; ++index) {
    EngineTypedValue value;
    std::size_t count = 0;
    if (!reader.ReadValue(&value) || !reader.ReadSize(&count)) return false;
    state->frequency_values.emplace_back(std::move(value), count);
  }
  return reader.Remaining() == 0 &&
         state->non_null_count <= state->transition_count;
}

bool SameCanonicalAggregateRuntimeScalar(
    const CanonicalAggregateRuntimeResult& left,
    const CanonicalAggregateRuntimeResult& right) {
  if (!left.diagnostic.ok || !right.diagnostic.ok ||
      left.output_batch.columns.size() != 1 ||
      right.output_batch.columns.size() != 1 ||
      left.output_batch.rows.size() != 1 || right.output_batch.rows.size() != 1 ||
      left.output_batch.rows.front().values.size() != 1 ||
      right.output_batch.rows.front().values.size() != 1) {
    return false;
  }
  return SameAggregateValueIdentity(
             left.output_batch.rows.front().values.front(),
             right.output_batch.rows.front().values.front()) &&
         left.output_batch.columns.front().descriptor_id ==
             right.output_batch.columns.front().descriptor_id &&
         left.transition_count == right.transition_count &&
         left.non_null_transition_count == right.non_null_transition_count &&
         left.distinct_tuple_count == right.distinct_tuple_count &&
         left.modifier_count == right.modifier_count &&
         left.aggregate_order_term_count == right.aggregate_order_term_count &&
         left.order_comparison_count == right.order_comparison_count &&
         left.modifier_pipeline_validated ==
             right.modifier_pipeline_validated &&
         left.filter_modifier_applied == right.filter_modifier_applied &&
         left.distinct_modifier_applied == right.distinct_modifier_applied &&
         left.filter_applied_before_distinct ==
             right.filter_applied_before_distinct &&
         left.distinct_applied_before_order ==
             right.distinct_applied_before_order &&
         left.aggregate_order_applied == right.aggregate_order_applied &&
         left.selected_plan_uuid == right.selected_plan_uuid &&
         left.executed_physical_node_id == right.executed_physical_node_id &&
         left.causal_counter_id == right.causal_counter_id;
}

bool IsCanonicalAggregateStateSpillUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(byte) || std::isupper(byte)) return false;
  }
  return true;
}

}  // namespace

// QOW-SOURCE-QRY-007-AGGREGATE-V1
// First canonical implementation in this module: global COUNT(*).  It counts
// physical input rows (including rows containing SQL NULL), returns one row on
// empty input, and uses the bound non-null int64 descriptor for its result.
CanonicalDescriptorCountResult ExecuteCanonicalDescriptorCountStar(
    const CanonicalDescriptorCountRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalDescriptorCountResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected aggregate node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-AGGREGATE-PHYSICAL-ROUTE-V1",
        "COUNT(*) requires one selected aggregate node"));
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids.size() != 1 ||
      request.count_column.descriptor_id !=
          selected_node->output_descriptor_ids.front() ||
      request.count_column.nullable ||
      request.count_column.descriptor.canonical_type_name != "int64") {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "COUNT(*) output descriptor is not bound int64"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (request.input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal("QOW-DIAG-QRY-007-AGGREGATE-OVERFLOW-V1",
                          "COUNT(*) exceeds int64 result width"));
  }

  EngineTypedValue count_value;
  count_value.descriptor = request.count_column.descriptor;
  count_value.encoded_value =
      std::to_string(request.input_batch.rows.size());
  count_value.state = EngineValueState::value;
  result.output_batch.columns = {request.count_column};
  result.output_batch.rows = {{{std::move(count_value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-REGISTRY-V1
// Exact ABI-v1 projection of the normative private seed registry.  A registry
// row is not an implementation claim by itself: executable is true only when
// the row is routed through the bounded canonical state below.  Every accepted
// aggregate is eligible for the canonical OVER bridge; descriptor,
// direct-argument, ordering, type, and resource profiles still fail closed.
const std::vector<CanonicalAggregateRegistryEntry>&
CanonicalAggregateRuntimeRegistryV1() {
  using Function = CanonicalAggregateFunction;
  static const std::vector<CanonicalAggregateRegistryEntry> registry = {
      {1, Function::count, "sb.aggregate.count", "019de5fc-2400-784a-9aec-371f8b95b7ea", true, true, true},
      {1, Function::sum, "sb.aggregate.sum", "019de5fc-2400-72e4-8549-82b2eef5a777", true, true, true},
      {1, Function::avg, "sb.aggregate.avg", "019de5fc-2400-78ac-b50c-45b832831004", true, true, true},
      {1, Function::min, "sb.aggregate.min", "019de5fc-2400-781c-881b-4af4d55d402b", true, true},
      {1, Function::max, "sb.aggregate.max", "019de5fc-2400-7d1e-8aa4-80bc647fbd9a", true, true},
      {1, Function::bool_and, "sb.aggregate.bool_and", "019de5fc-2400-78b0-ad98-a681e93b4c49", true, true},
      {1, Function::bool_or, "sb.aggregate.bool_or", "019de5fc-2400-7c2a-a3f2-e4b9d36df403", true, true},
      {1, Function::array_agg, "sb.aggregate.array_agg", "019de5fc-2400-7159-9f7b-915513b8c0d4", true, true},
      {1, Function::string_agg, "sb.aggregate.string_agg", "019de5fc-2400-7243-abc6-4f6a777dff00", true, true},
      {1, Function::json_agg, "sb.aggregate.json_agg", "019dffbb-f001-7021-8a00-000000000023", true, true},
      {1, Function::json_object_agg, "sb.aggregate.json_object_agg", "019dffbb-f001-7021-8a00-000000000024", true, true},
      {1, Function::stddev_pop, "sb.aggregate.stddev_pop", "019de5fc-2400-73c9-ba10-4665f741215d", true, true},
      {1, Function::variance_pop, "sb.aggregate.variance_pop", "019de5fc-2400-7fda-b470-e85414dcb314", true, true},
      {1, Function::every, "sb.aggregate.every", "019dffbb-f000-7876-9644-ae83b363d3bc", true, true},
      {1, Function::listagg, "sb.aggregate.listagg", "019dffbb-f000-7e93-8e4d-6063849de049", true, true},
      {1, Function::rank, "sb.aggregate.rank", "019dffbb-f000-7336-ab53-fef5316220d7", true, true},
      {1, Function::dense_rank, "sb.aggregate.dense_rank", "019dffbb-f000-7bd3-a731-1734581eb8ce", true, true},
      {1, Function::percent_rank, "sb.aggregate.percent_rank", "019dffbb-f000-7817-911f-9f8b2e66ebec", true, true},
      {1, Function::cume_dist, "sb.aggregate.cume_dist", "019dffbb-f000-7244-89fd-8fa66ae930d5", true, true},
      {1, Function::mode, "sb.aggregate.mode", "019dffbb-f000-7150-9be6-bcf97f8facf5", true, true},
      {1, Function::percentile_cont, "sb.aggregate.percentile_cont", "019dffbb-f000-7cfd-83dd-15435fe55bf5", true, true},
      {1, Function::percentile_disc, "sb.aggregate.percentile_disc", "019dffbb-f000-7081-b766-7db818a89c04", true, true},
      {1, Function::approx_count_distinct, "sb.aggregate.approx_count_distinct", "019dffbb-f000-7736-96f3-e20cbd532ba5", true, true},
      {1, Function::approx_median, "sb.aggregate.approx_median", "019dffbb-f000-7ce0-85a6-cbcd71f2c86e", true, true},
      {1, Function::approx_percentile_cont, "sb.aggregate.approx_percentile_cont", "019dffbb-f000-76df-98a6-aa77d1a342f8", true, true},
      {1, Function::approx_percentile_disc, "sb.aggregate.approx_percentile_disc", "019dffbb-f000-7578-a88f-8db4bb649755", true, true},
      {1, Function::approx_top_k, "sb.aggregate.approx_top_k", "019dffbb-f000-7f47-8fe1-0c5e0ec87bf0", true, true},
      {1, Function::stddev, "sb.aggregate.stddev", "019dffbb-f000-7475-8516-ff003b2bdad9", true, true},
      {1, Function::variance, "sb.aggregate.variance", "019dffbb-f000-7968-82c5-04cffbeb971b", true, true},
      {1, Function::stddev_samp, "sb.aggregate.stddev_samp", "019dffbb-f000-7d99-a495-70f9c3b1b587", true, true},
      {1, Function::variance_samp, "sb.aggregate.variance_samp", "019dffbb-f000-732b-8a0c-2aa88b04f3c5", true, true},
      {1, Function::corr, "sb.aggregate.corr", "019dffbb-f000-77bb-ba9b-2e78acf84521", true, true},
      {1, Function::covar_pop, "sb.aggregate.covar_pop", "019dffbb-f000-7f09-8ceb-17ad4c70e99f", true, true},
      {1, Function::covar_samp, "sb.aggregate.covar_samp", "019dffbb-f000-747d-bc01-caad9137d070", true, true},
      {1, Function::regr_count, "sb.aggregate.regr_count", "019dffbb-f000-75aa-bbe6-a4a67dacb81f", true, true},
      {1, Function::regr_avgx, "sb.aggregate.regr_avgx", "019dffbb-f000-7662-a816-d1df50e9b664", true, true},
      {1, Function::regr_avgy, "sb.aggregate.regr_avgy", "019dffbb-f000-7d03-ac2d-753cdb7744c0", true, true},
      {1, Function::regr_intercept, "sb.aggregate.regr_intercept", "019dffbb-f000-7c7c-b576-d67ea9d4bcbb", true, true},
      {1, Function::regr_r2, "sb.aggregate.regr_r2", "019dffbb-f000-7a43-9a28-a119b31d9c20", true, true},
      {1, Function::regr_slope, "sb.aggregate.regr_slope", "019dffbb-f000-7f80-b81a-5240a6dbab55", true, true},
      {1, Function::regr_sxx, "sb.aggregate.regr_sxx", "019dffbb-f000-735e-9e55-5f9243786403", true, true},
      {1, Function::regr_sxy, "sb.aggregate.regr_sxy", "019dffbb-f000-788b-a249-866547a43ebe", true, true},
      {1, Function::regr_syy, "sb.aggregate.regr_syy", "019dffbb-f000-74f7-98ba-c24ead6d30df", true, true},
  };
  return registry;
}

const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateByFunctionV1(
    const CanonicalAggregateFunction function) {
  const auto& registry = CanonicalAggregateRuntimeRegistryV1();
  const auto found = std::ranges::find_if(
      registry, [&](const auto& entry) { return entry.function == function; });
  return found == registry.end() ? nullptr : &*found;
}

const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateByBuiltinIdV1(
    const std::string_view builtin_id) {
  const auto& registry = CanonicalAggregateRuntimeRegistryV1();
  const auto found = std::ranges::find_if(registry, [&](const auto& entry) {
    return entry.builtin_id == builtin_id;
  });
  return found == registry.end() ? nullptr : &*found;
}

const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateByUuidV1(
    const std::string_view function_uuid) {
  const auto& registry = CanonicalAggregateRuntimeRegistryV1();
  const auto found = std::ranges::find_if(registry, [&](const auto& entry) {
    return entry.function_uuid == function_uuid;
  });
  return found == registry.end() ? nullptr : &*found;
}

const CanonicalAggregateRegistryEntry* LookupCanonicalAggregateExactV1(
    const std::uint16_t abi_version,
    const CanonicalAggregateFunction function,
    const std::string_view builtin_id,
    const std::string_view function_uuid) {
  const auto* entry = LookupCanonicalAggregateByFunctionV1(function);
  return entry != nullptr && entry->abi_version == abi_version &&
                 entry->builtin_id == builtin_id &&
                 entry->function_uuid == function_uuid
             ? entry
             : nullptr;
}

std::vector<std::string> ValidateCanonicalAggregateRuntimeRegistryV1() {
  std::vector<std::string> errors;
  std::set<CanonicalAggregateFunction> functions;
  std::set<std::string> builtin_ids;
  std::set<std::string> function_uuids;
  for (const auto& entry : CanonicalAggregateRuntimeRegistryV1()) {
    if (entry.abi_version != 1 ||
        entry.function == CanonicalAggregateFunction::unknown ||
        entry.builtin_id.empty() || entry.function_uuid.empty()) {
      errors.push_back("incomplete aggregate registry row");
      continue;
    }
    if (!functions.insert(entry.function).second) {
      errors.push_back("duplicate aggregate function enum");
    }
    if (!builtin_ids.insert(entry.builtin_id).second) {
      errors.push_back("duplicate aggregate builtin id: " + entry.builtin_id);
    }
    if (!function_uuids.insert(entry.function_uuid).second) {
      errors.push_back("duplicate aggregate function UUID: " +
                       entry.function_uuid);
    }
    if (LookupCanonicalAggregateByFunctionV1(entry.function) != &entry ||
        LookupCanonicalAggregateByBuiltinIdV1(entry.builtin_id) != &entry ||
        LookupCanonicalAggregateByUuidV1(entry.function_uuid) != &entry ||
        LookupCanonicalAggregateExactV1(entry.abi_version, entry.function,
                                        entry.builtin_id,
                                        entry.function_uuid) != &entry) {
      errors.push_back("aggregate registry lookup drift: " +
                       entry.builtin_id);
    }
  }
  return errors;
}

static CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntimeSelected(
    const CanonicalAggregateRuntimeRequest& request,
    const bool state_exchange_execution_context) {
  CanonicalAggregateRuntimeResult result;
  result.descriptor = request.descriptor;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    const auto descriptor = result.descriptor;
    result = {};
    result.descriptor = descriptor;
    result.diagnostic = std::move(diagnostic);
    return result;
  };

  const auto* entry = LookupCanonicalAggregateExactV1(
      request.descriptor.abi_version, request.descriptor.function,
      request.descriptor.builtin_id, request.descriptor.function_uuid);
  if (request.descriptor.abi_version != 1 || entry == nullptr) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-DESCRIPTOR-V1",
                          "aggregate ABI, enum, id, and UUID must match one exact registry row"));
  }
  if (!entry->executable) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-UNIMPLEMENTED-V1",
                          request.descriptor.builtin_id));
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-AUTHORITY-V1",
                          "aggregate runtime cannot own parser execution, transaction finality, or recovery"));
  }
  if (request.forced_strategy != CanonicalAggregateExecutionStrategy::serial &&
      request.forced_strategy !=
          CanonicalAggregateExecutionStrategy::partitioned_combine) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-STRATEGY-V1",
                          "aggregate physical strategy is not bound"));
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected aggregate node is not the root"));
  }
  const PhysicalNodeRecord* aggregate_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      aggregate_node = &node;
    }
  }
  if (aggregate_node == nullptr ||
      aggregate_node->node_kind != PhysicalNodeKind::kAggregate ||
      aggregate_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-PHYSICAL-V1",
                          "canonical aggregate requires one physical input"));
  }
  const bool state_exchange_selected =
      aggregate_node->implementation_id ==
      "aggregate.registry-state-exchange.v1";
  if (state_exchange_selected != state_exchange_execution_context) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-STRATEGY-V1",
        state_exchange_selected
            ? "selected aggregate state exchange requires the exchange runtime"
            : "aggregate state exchange payload does not match the selected implementation"));
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        aggregate_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr || aggregate_node->output_descriptor_ids.size() != 1 ||
      request.result_column.descriptor_id == 0 ||
      aggregate_node->output_descriptor_ids.front() !=
          request.result_column.descriptor_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "aggregate input or result handle is unresolved"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  const bool count_star =
      request.descriptor.function == CanonicalAggregateFunction::count &&
      request.descriptor.count_star;
  if (request.descriptor.count_star && !count_star) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-ARITY-V1",
                          "count-star is valid only for COUNT"));
  }
  const std::size_t expected_arity = CanonicalAggregateExpectedArity(
      request.descriptor.function, count_star);
  if (request.value_columns.size() != expected_arity ||
      request.value_expression_descriptor_ids.size() != expected_arity) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-ARITY-V1",
                          "aggregate value arity does not match its descriptor"));
  }
  const auto expected_direct_argument_count =
      CanonicalAggregateDirectArgumentCount(request.descriptor.function);
  if (request.direct_arguments.size() != expected_direct_argument_count) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-DIRECT-ARITY-V1",
                          "aggregate direct-argument arity is not bound"));
  }
  if (expected_direct_argument_count != 0) {
    const auto& direct = request.direct_arguments.front();
    if (direct.state != EngineValueState::value || direct.is_null) {
      return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-DIRECT-V1",
                            "aggregate direct argument must be non-NULL"));
    }
    if (request.descriptor.function ==
        CanonicalAggregateFunction::approx_top_k) {
      const auto decoded = DecodeInt64Value(direct);
      if (!decoded.ok() || decoded.value <= 0) {
        return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-DIRECT-V1",
                              "top-k direct argument must be positive int64"));
      }
    } else {
      long double decoded = 0.0L;
      DescriptorRuntimeDiagnostic direct_diagnostic;
      if (!DecodeAggregateNumeric(direct, &decoded, &direct_diagnostic)) {
        return refuse(std::move(direct_diagnostic));
      }
      if (IsCanonicalQuantileFunction(request.descriptor.function) &&
          (decoded < 0.0L || decoded > 1.0L)) {
        return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-DIRECT-V1",
                              "percentile fraction must be between zero and one"));
      }
    }
  }
  for (std::size_t index = 0; index < request.value_columns.size(); ++index) {
    const auto column = request.value_columns[index];
    if (column >= request.input_batch.columns.size() ||
        request.input_batch.columns[column].descriptor_id !=
            request.value_expression_descriptor_ids[index]) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "aggregate value expression handle is unresolved"));
    }
  }
  if (request.distinct && count_star) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-DISTINCT-V1",
                          "COUNT(DISTINCT *) is not admitted"));
  }
  if (request.distinct && request.maximum_distinct_value_count == 0) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate DISTINCT bound is zero"));
  }
  if (request.maximum_transition_count == 0 ||
      request.maximum_state_bytes == 0 ||
      request.input_batch.rows.size() > request.maximum_transition_count) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate transition or state bound is exceeded"));
  }
  if (request.filter_truth_values.has_value() &&
      request.filter_truth_values->size() != request.input_batch.rows.size()) {
    return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                          "aggregate FILTER cardinality is not bound"));
  }
  if (request.filter_truth_values.has_value()) {
    using scratchbird::engine::internal_api::EnginePredicateConsumer;
    using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;
    for (const auto truth : *request.filter_truth_values) {
      bool passes = false;
      std::string detail;
      if (!QowPredicateConsumerPassesV1(
              truth, EnginePredicateConsumer::filter, &passes, &detail)) {
        return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                              std::move(detail)));
      }
    }
  }
  if (!request.aggregate_order_terms.empty() &&
      (request.maximum_aggregate_order_term_count == 0 ||
       request.aggregate_order_terms.size() >
           request.maximum_aggregate_order_term_count)) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate order-term bound is exceeded"));
  }
  for (const auto& term : request.aggregate_order_terms) {
    if (term.column >= request.input_batch.columns.size()) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "aggregate order term column is unresolved"));
    }
    auto validation = ValidateCanonicalDescriptorOrderTerm(
        term, request.input_batch.columns[term.column]);
    if (!validation.ok) return refuse(std::move(validation));
  }
  const auto function = request.descriptor.function;
  const bool ordered_collection_required =
      function == CanonicalAggregateFunction::array_agg ||
      function == CanonicalAggregateFunction::json_agg ||
      function == CanonicalAggregateFunction::json_object_agg ||
      function == CanonicalAggregateFunction::listagg ||
      IsCanonicalOrderedSetFunction(function);
  if (ordered_collection_required && request.aggregate_order_terms.empty()) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-ORDER-V1",
                          "ordered collection aggregate has no bound order"));
  }
  if (request.listagg_overflow_mode != CanonicalListaggOverflowMode::none &&
      request.listagg_overflow_mode != CanonicalListaggOverflowMode::error &&
      request.listagg_overflow_mode !=
          CanonicalListaggOverflowMode::truncate) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-OPTIONS-V1",
                          "LISTAGG overflow mode is not canonical"));
  }
  if (function == CanonicalAggregateFunction::listagg) {
    if (request.listagg_overflow_mode != CanonicalListaggOverflowMode::none &&
        request.listagg_max_output_bytes == 0) {
      return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-OPTIONS-V1",
                            "LISTAGG overflow mode requires a byte bound"));
    }
  } else if (request.listagg_overflow_mode !=
                 CanonicalListaggOverflowMode::none ||
             request.listagg_max_output_bytes != 0 ||
             request.listagg_truncation_indicator != "..." ||
             !request.listagg_with_count) {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-OPTIONS-V1",
                          "LISTAGG options reached another aggregate"));
  }
  if (function != CanonicalAggregateFunction::string_agg &&
      function != CanonicalAggregateFunction::listagg &&
      request.aggregate_separator != ",") {
    return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-OPTIONS-V1",
                          "string separator reached another aggregate"));
  }

  DescriptorRuntimeDiagnostic type_diagnostic;
  if (!ValidateCanonicalAggregateInputType(request, &type_diagnostic)) {
    return refuse(std::move(type_diagnostic));
  }
  if (!ValidateCanonicalAggregateResultType(request, &type_diagnostic)) {
    return refuse(std::move(type_diagnostic));
  }

  PreparedAggregateTransitions prepared;
  DescriptorRuntimeDiagnostic state_diagnostic;
  if (!PrepareCanonicalAggregateTransitions(request, &prepared,
                                            &state_diagnostic)) {
    return refuse(std::move(state_diagnostic));
  }
  result.input_row_count = prepared.input_row_count;
  result.filtered_row_count = prepared.filtered_row_count;
  result.distinct_tuple_count = prepared.distinct_tuple_count;
  result.order_comparison_count = prepared.order_comparison_count;
  result.aggregate_order_applied = prepared.aggregate_order_applied;
  result.modifier_count =
      (request.filter_truth_values.has_value() ? 1U : 0U) +
      (request.distinct ? 1U : 0U) +
      (!request.aggregate_order_terms.empty() ? 1U : 0U);
  result.aggregate_order_term_count = request.aggregate_order_terms.size();
  result.modifier_pipeline_validated = true;
  result.filter_modifier_applied = request.filter_truth_values.has_value();
  result.distinct_modifier_applied = request.distinct;
  result.filter_applied_before_distinct = true;
  result.distinct_applied_before_order = true;

  CanonicalAggregateCoreState state;
  if (!BuildCanonicalAggregateCoreState(request, prepared.transitions, &state,
                                        &state_diagnostic)) {
    return refuse(std::move(state_diagnostic));
  }

  auto value = FinalizeCanonicalAggregateCore(request, state,
                                               &state_diagnostic);
  if (!state_diagnostic.ok) return refuse(std::move(state_diagnostic));
  result.output_batch.columns = {request.result_column};
  result.output_batch.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, aggregate_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.executed_strategy = request.forced_strategy;
  result.transition_count = state.transition_count;
  result.non_null_transition_count = state.non_null_count;
  result.state_bytes = EstimateCanonicalAggregateStateBytes(state);
  result.direct_argument_count = request.direct_arguments.size();
  result.every_descriptor_field_consumed = true;
  result.shared_state_authority_used = true;
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.owns_transaction_finality = false;
  result.authority.owns_recovery = false;
  result.authority.owns_parser_execution = false;
  result.authority.owns_visibility_outside_engine_mga = false;
  result.authority.wal_is_transaction_or_recovery_authority = false;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = aggregate_node->physical_node_id;
  result.causal_counter_id = aggregate_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntime(
    const CanonicalAggregateRuntimeRequest& request) {
  return ExecuteCanonicalAggregateRuntimeSelected(request, false);
}

// QOW-SOURCE-QRY-011-REGISTRY-STATE-SPILL-V1
// Serialize the complete versioned aggregate transition state, spill and
// reopen it through engine-owned temporary work, restore the state, and run
// the ordinary canonical finalizer. The optimizer-selected physical node and
// exact MGA/security/recheck contract govern the route; temporary metadata
// never owns row visibility, transaction finality, or recovery.
CanonicalAggregateStateSpillResult ExecuteCanonicalAggregateStateSpill(
    const CanonicalAggregateStateSpillRequest& request) {
  CanonicalAggregateStateSpillResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.aggregate_result = {};
    result.state_restored = false;
    result.restored_result_equivalent = false;
    return result;
  };

  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority,
      request.aggregate_request.physical_dag);
  if (!entry_authority.ok) {
    return refuse(entry_authority.diagnostic_code, entry_authority.detail);
  }

  const auto baseline =
      ExecuteCanonicalAggregateRuntime(request.aggregate_request);
  if (!baseline.diagnostic.ok) {
    return refuse(baseline.diagnostic.diagnostic_code,
                  baseline.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          baseline.mga_statement_context,
          request.aggregate_request.mga_authority.statement_context)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-MGA-V1",
                  "aggregate spill baseline returned a different MGA statement context");
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.aggregate_request.physical_dag.nodes) {
    if (node.physical_node_id ==
        request.aggregate_request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      (selected_node->implementation_id !=
           "aggregate.registry-state-spill.v1" &&
       selected_node->implementation_id !=
           "window.aggregate-registry-state-spill.v1" &&
       selected_node->implementation_id !=
           "aggregate.registry-grouping-sets-state-spill.v1") ||
      !request.aggregate_request.physical_dag.spill_allowed) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-STRATEGY-V1",
                  "aggregate state spill was not selected and permitted by the physical plan");
  }
  if (request.spill_root.empty() || !request.spill_root.is_absolute() ||
      !IsCanonicalAggregateStateSpillUuid(request.spill_owner_uuid) ||
      request.runtime_generation == 0 || request.memory_quota_bytes == 0 ||
      request.maximum_serialized_state_bytes == 0 ||
      request.maximum_spill_record_count == 0) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-OWNERSHIP-V1",
                  "aggregate state spill ownership or resource context is invalid");
  }
  const auto owner_directory =
      (request.spill_root / request.spill_owner_uuid).lexically_normal();
  if (owner_directory.filename() != request.spill_owner_uuid) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-OWNERSHIP-V1",
                  "aggregate state spill owner directory is not exact");
  }
  const auto has_owned_artifact = [&]() {
    std::error_code error;
    if (!std::filesystem::exists(owner_directory, error)) {
      return static_cast<bool>(error);
    }
    if (!std::filesystem::is_directory(owner_directory, error) || error) {
      return true;
    }
    for (std::filesystem::directory_iterator iterator(owner_directory, error),
         end;
         !error && iterator != end; iterator.increment(error)) {
      const auto filename = iterator->path().filename().string();
      if (filename.rfind("orh283_temp_spill-", 0) == 0 &&
          iterator->path().extension() == ".sbtmpidx") {
        return true;
      }
    }
    return static_cast<bool>(error);
  };
  const auto remove_owned_artifacts = [&]() {
    std::error_code error;
    if (!std::filesystem::exists(owner_directory, error)) return !error;
    for (std::filesystem::directory_iterator iterator(owner_directory, error),
         end;
         !error && iterator != end; iterator.increment(error)) {
      const auto filename = iterator->path().filename().string();
      if (filename.rfind("orh283_temp_spill-", 0) == 0 &&
          iterator->path().extension() == ".sbtmpidx") {
        std::filesystem::remove(iterator->path(), error);
      }
    }
    return !error && !has_owned_artifact();
  };
  std::error_code filesystem_error;
  if (std::filesystem::is_symlink(owner_directory, filesystem_error) ||
      filesystem_error || has_owned_artifact()) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-OWNERSHIP-V1",
                  "aggregate state spill owner is unsafe or already occupied");
  }

  PreparedAggregateTransitions prepared;
  DescriptorRuntimeDiagnostic state_diagnostic;
  if (!PrepareCanonicalAggregateTransitions(request.aggregate_request,
                                            &prepared,
                                            &state_diagnostic)) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  CanonicalAggregateCoreState state;
  if (!BuildCanonicalAggregateCoreState(request.aggregate_request,
                                        prepared.transitions, &state,
                                        &state_diagnostic)) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  AggregateStateBytes serialized;
  if (!SerializeCanonicalAggregateCoreState(
          request.aggregate_request, state,
          request.maximum_serialized_state_bytes, &serialized)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SERIALIZE-V1",
                  "aggregate state cannot be canonically serialized within its bound");
  }
  result.state_serialized = true;
  result.serialized_state_bytes = serialized.size();
  constexpr std::size_t kBytesPerSpillRecord = 7;
  const auto record_count =
      (serialized.size() + kBytesPerSpillRecord - 1) /
      kBytesPerSpillRecord;
  if (record_count == 0 ||
      record_count > request.maximum_spill_record_count) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-RESOURCE-V1",
                  "serialized aggregate state exceeds its spill record bound");
  }

  TempSpillRequest spill;
  spill.route_kind = TempSpillRouteKind::kSort;
  spill.route_label =
      "qow205.aggregate-registry-state." + request.spill_owner_uuid;
  spill.spill_directory = owner_directory;
  spill.runtime_generation = request.runtime_generation;
  spill.reopen_runtime_generation = request.reopen_runtime_generation;
  spill.memory_quota_bytes = request.memory_quota_bytes;
  spill.cancellation_requested = request.cancellation_requested;
  spill.cleanup_after_cancellation = request.cleanup_after_cancellation;
  spill.restart_recovery_proof_available =
      request.restart_recovery_proof_available;
  spill.authority.engine_mga_snapshot_bound = true;
  spill.authority.transaction_inventory_authoritative = true;
  spill.authority.security_recheck_required = true;
  spill.authority.security_context_bound = true;
  spill.authority.exact_recheck_required = true;
  spill.rows.reserve(record_count);
  const auto record_key = [](const std::size_t index) {
    std::ostringstream stream;
    stream << "qow205.state." << std::setw(20) << std::setfill('0') << index;
    return stream.str();
  };
  for (std::size_t record = 0; record < record_count; ++record) {
    std::int64_t payload = 0;
    for (std::size_t byte = 0; byte < kBytesPerSpillRecord; ++byte) {
      const auto offset = record * kBytesPerSpillRecord + byte;
      if (offset == serialized.size()) break;
      payload |= static_cast<std::int64_t>(serialized[offset]) << (byte * 8);
    }
    spill.rows.push_back(
        {record_key(record), payload, static_cast<std::uint64_t>(record + 1)});
  }

  const auto spilled = ExecuteBoundedTempSpillRoute(spill);
  result.spilled = spilled.spilled;
  result.spill_reopened = spilled.reopen_recovery_proven;
  result.cleanup_proven = spilled.cleanup_proven;
  result.cancellation_observed = request.cancellation_requested;
  result.spill_evidence = spilled.evidence;
  result.spilled_state_record_count = spill.rows.size();
  if (has_owned_artifact()) {
    result.cleanup_proven =
        remove_owned_artifacts() && result.cleanup_proven;
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-CLEANUP-V1",
                  "aggregate state spill artifact survived cleanup");
  }
  if (!spilled.ok || !spilled.spilled || !spilled.cleanup_proven ||
      !spilled.reopen_recovery_proven ||
      spilled.output_rows.size() != record_count) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-V1",
                  spilled.diagnostic_code + ":" + spilled.fallback_reason);
  }

  AggregateStateBytes reopened_bytes;
  reopened_bytes.reserve(serialized.size());
  for (std::size_t record = 0; record < spilled.output_rows.size(); ++record) {
    const auto& output = spilled.output_rows[record];
    const auto first_separator = output.find(':');
    const auto last_separator = output.rfind(':');
    if (first_separator == std::string::npos ||
        last_separator == std::string::npos ||
        first_separator == last_separator ||
        output.substr(0, first_separator) != record_key(record)) {
      return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-RESTORE-V1",
                    "reopened aggregate state record is malformed");
    }
    std::int64_t payload = 0;
    std::uint64_t ordinal = 0;
    const auto payload_text = std::string_view(output).substr(
        first_separator + 1, last_separator - first_separator - 1);
    const auto ordinal_text =
        std::string_view(output).substr(last_separator + 1);
    const auto payload_parse = std::from_chars(
        payload_text.data(), payload_text.data() + payload_text.size(),
        payload);
    const auto ordinal_parse = std::from_chars(
        ordinal_text.data(), ordinal_text.data() + ordinal_text.size(),
        ordinal);
    if (payload_parse.ec != std::errc{} ||
        payload_parse.ptr != payload_text.data() + payload_text.size() ||
        ordinal_parse.ec != std::errc{} ||
        ordinal_parse.ptr != ordinal_text.data() + ordinal_text.size() ||
        ordinal != record + 1 || payload < 0) {
      return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-RESTORE-V1",
                    "reopened aggregate state record fields are invalid");
    }
    for (std::size_t byte = 0;
         byte < kBytesPerSpillRecord &&
         reopened_bytes.size() < serialized.size();
         ++byte) {
      reopened_bytes.push_back(
          static_cast<std::uint8_t>(payload >> (byte * 8)));
    }
  }
  if (reopened_bytes != serialized) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-RESTORE-V1",
                  "reopened aggregate state bytes differ from the serialized state");
  }

  CanonicalAggregateCoreState restored_state;
  if (!DeserializeCanonicalAggregateCoreState(
          request.aggregate_request, reopened_bytes,
          request.maximum_serialized_state_bytes, &restored_state)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-RESTORE-V1",
                  "reopened aggregate state cannot be decoded");
  }
  result.state_restored = true;
  state_diagnostic = {};
  auto restored_value = FinalizeCanonicalAggregateCore(
      request.aggregate_request, restored_state, &state_diagnostic);
  if (!state_diagnostic.ok) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  auto restored = baseline;
  restored.output_batch.rows = {{{std::move(restored_value)}}};
  restored.transition_count = restored_state.transition_count;
  restored.non_null_transition_count = restored_state.non_null_count;
  restored.state_bytes = EstimateCanonicalAggregateStateBytes(restored_state);
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      restored.output_batch,
      {request.aggregate_request.result_column.descriptor_id});
  if (!output_validation.ok ||
      !SameCanonicalAggregateRuntimeScalar(baseline, restored)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-EQUIVALENCE-V1",
                  "restored aggregate state finalization differs from the in-memory route");
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority,
      request.aggregate_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code, result_authority.detail);
  }

  result.diagnostic = {};
  result.aggregate_result = std::move(restored);
  result.restored_result_equivalent = true;
  result.mga_statement_context =
      request.aggregate_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-REGISTRY-STATE-EXCHANGE-V1
// Materialize one complete transition state per optimizer-bound worker,
// serialize each state across an explicit exchange boundary, restore it at
// the coordinator, and merge in worker-ordinal order through the same state
// authority used by serial, local-combine, spill, grouped, and window routes.
// The exchange carries no transaction-finality, recovery, visibility, or
// parser-execution authority of its own.
CanonicalAggregateStateExchangeResult ExecuteCanonicalAggregateStateExchange(
    const CanonicalAggregateStateExchangeRequest& request) {
  CanonicalAggregateStateExchangeResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.aggregate_result = {};
    result.exchange_identity_proven = false;
    result.all_states_restored = false;
    result.deterministic_merge_order_proven = false;
    result.merged_result_equivalent = false;
    return result;
  };

  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority,
      request.aggregate_request.physical_dag);
  if (!entry_authority.ok) {
    return refuse(entry_authority.diagnostic_code, entry_authority.detail);
  }

  const auto worker_count = request.worker_ordinals.size();
  if (worker_count < 2 || worker_count > 1024 ||
      request.maximum_partial_state_count == 0 ||
      worker_count > request.maximum_partial_state_count ||
      request.maximum_serialized_state_bytes_per_worker == 0 ||
      request.maximum_combined_serialized_state_bytes == 0 ||
      request.exchange_generation == 0 ||
      request.coordinator_exchange_generation == 0) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-SHAPE-V1",
        "aggregate state exchange worker, generation, or resource shape is invalid");
  }
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    if (request.worker_ordinals[worker] != worker) {
      return refuse(
          "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-IDENTITY-V1",
          "aggregate state exchange worker ordinals are not exact and increasing");
    }
  }
  if (request.exchange_generation !=
      request.coordinator_exchange_generation) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-GENERATION-V1",
        "aggregate state exchange reached a stale coordinator generation");
  }
  if (request.aggregate_request.forced_strategy !=
      CanonicalAggregateExecutionStrategy::partitioned_combine) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-STRATEGY-V1",
        "aggregate state exchange requires the partitioned-combine strategy");
  }
  if (request.cancellation_requested) {
    result.cancellation_observed = true;
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-CANCELLED-V1",
                  "aggregate state exchange was cancelled before publication");
  }

  const auto baseline = ExecuteCanonicalAggregateRuntimeSelected(
      request.aggregate_request, true);
  if (!baseline.diagnostic.ok) {
    return refuse(baseline.diagnostic.diagnostic_code,
                  baseline.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          baseline.mga_statement_context,
          request.aggregate_request.mga_authority.statement_context)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-MGA-V1",
                  "aggregate exchange baseline returned a different MGA statement context");
  }

  PreparedAggregateTransitions prepared;
  DescriptorRuntimeDiagnostic state_diagnostic;
  if (!PrepareCanonicalAggregateTransitions(request.aggregate_request,
                                            &prepared,
                                            &state_diagnostic)) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }

  struct ExchangedPartialState {
    std::uint64_t generation = 0;
    std::uint32_t worker_ordinal = 0;
    CanonicalAggregateFunction function = CanonicalAggregateFunction::unknown;
    AggregateStateBytes bytes;
  };
  std::vector<ExchangedPartialState> exchanged;
  exchanged.reserve(worker_count);
  result.worker_transition_counts.reserve(worker_count);
  result.worker_state_bytes.reserve(worker_count);
  result.worker_serialized_state_bytes.reserve(worker_count);

  const auto base_partition_size = prepared.transitions.size() / worker_count;
  const auto extra_partition_count = prepared.transitions.size() % worker_count;
  std::size_t partition_begin = 0;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    const auto partition_size =
        base_partition_size + (worker < extra_partition_count ? 1 : 0);
    const auto partition_end = partition_begin + partition_size;
    CanonicalAggregateCoreState partial_state;
    for (std::size_t transition = partition_begin;
         transition < partition_end; ++transition) {
      if (!TransitionCanonicalAggregateCore(
              request.aggregate_request.descriptor,
              prepared.transitions[transition].values, &partial_state,
              &state_diagnostic)) {
        return refuse(state_diagnostic.diagnostic_code,
                      state_diagnostic.detail);
      }
      if (EstimateCanonicalAggregateStateBytes(partial_state) >
          request.aggregate_request.maximum_state_bytes) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "aggregate exchange partial-state byte bound is exceeded");
      }
    }
    partition_begin = partition_end;

    AggregateStateBytes serialized;
    if (!SerializeCanonicalAggregateCoreState(
            request.aggregate_request, partial_state,
            request.maximum_serialized_state_bytes_per_worker,
            &serialized)) {
      return refuse(
          "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-SERIALIZE-V1",
          "aggregate worker state cannot be serialized within its bound");
    }
    if (result.serialized_state_bytes >
            request.maximum_combined_serialized_state_bytes ||
        serialized.size() >
            request.maximum_combined_serialized_state_bytes -
                result.serialized_state_bytes) {
      return refuse(
          "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-RESOURCE-V1",
          "combined aggregate exchange state byte bound is exceeded");
    }
    result.serialized_state_bytes += serialized.size();
    result.worker_transition_counts.push_back(partial_state.transition_count);
    result.worker_state_bytes.push_back(
        EstimateCanonicalAggregateStateBytes(partial_state));
    result.worker_serialized_state_bytes.push_back(serialized.size());
    exchanged.push_back({request.exchange_generation,
                         request.worker_ordinals[worker],
                         request.aggregate_request.descriptor.function,
                         std::move(serialized)});
  }
  if (partition_begin != prepared.transitions.size()) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-PARTITION-V1",
        "aggregate exchange partitions do not cover the prepared transitions");
  }
  result.partial_state_count = exchanged.size();
  result.states_serialized = true;

  CanonicalAggregateCoreState merged_state;
  for (std::size_t worker = 0; worker < exchanged.size(); ++worker) {
    const auto& envelope = exchanged[worker];
    if (envelope.generation != request.coordinator_exchange_generation ||
        envelope.worker_ordinal != worker ||
        envelope.function != request.aggregate_request.descriptor.function) {
      return refuse(
          "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-IDENTITY-V1",
          "aggregate partial-state envelope identity is not coordinator-bound");
    }
    CanonicalAggregateCoreState restored_state;
    if (!DeserializeCanonicalAggregateCoreState(
            request.aggregate_request, envelope.bytes,
            request.maximum_serialized_state_bytes_per_worker,
            &restored_state) ||
        restored_state.transition_count !=
            result.worker_transition_counts[worker] ||
        EstimateCanonicalAggregateStateBytes(restored_state) !=
            result.worker_state_bytes[worker]) {
      return refuse(
          "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-RESTORE-V1",
          "aggregate partial state cannot be restored exactly");
    }
    ++result.restored_partial_state_count;
    if (!MergeCanonicalAggregateCore(request.aggregate_request.descriptor,
                                     &merged_state, restored_state,
                                     &state_diagnostic)) {
      return refuse(state_diagnostic.diagnostic_code,
                    state_diagnostic.detail);
    }
    if (EstimateCanonicalAggregateStateBytes(merged_state) >
        request.aggregate_request.maximum_state_bytes) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "aggregate exchange merged-state byte bound is exceeded");
    }
    ++result.merged_partial_state_count;
  }
  result.exchange_identity_proven = true;
  result.all_states_restored =
      result.restored_partial_state_count == exchanged.size();
  result.deterministic_merge_order_proven =
      result.merged_partial_state_count == exchanged.size();

  if (merged_state.transition_count != baseline.transition_count ||
      merged_state.non_null_count != baseline.non_null_transition_count ||
      EstimateCanonicalAggregateStateBytes(merged_state) !=
          baseline.state_bytes) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-EQUIVALENCE-V1",
        "merged aggregate exchange state differs from local combine evidence");
  }
  auto merged_value = FinalizeCanonicalAggregateCore(
      request.aggregate_request, merged_state, &state_diagnostic);
  if (!state_diagnostic.ok) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  auto merged = baseline;
  merged.output_batch.rows = {{{std::move(merged_value)}}};
  merged.transition_count = merged_state.transition_count;
  merged.non_null_transition_count = merged_state.non_null_count;
  merged.state_bytes = EstimateCanonicalAggregateStateBytes(merged_state);
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      merged.output_batch,
      {request.aggregate_request.result_column.descriptor_id});
  if (!output_validation.ok ||
      !SameCanonicalAggregateRuntimeScalar(baseline, merged)) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-EQUIVALENCE-V1",
        "merged aggregate exchange finalization differs from local combine");
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority,
      request.aggregate_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code, result_authority.detail);
  }

  result.diagnostic = {};
  result.aggregate_result = std::move(merged);
  result.merged_result_equivalent = true;
  result.mga_statement_context =
      request.aggregate_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-REGISTRY-MOVING-INVERSE-V1
// Maintain one exact COUNT/SUM/AVG state across an ordered sequence of window
// frames.  Additions use the canonical transition function, removals use an
// explicitly admitted inverse transition, and every output uses the same
// canonical finalizer.  Unsupported functions and modifiers fail closed
// rather than relabelling frame recomputation as inverse execution.
CanonicalAggregateMovingRuntimeResult ExecuteCanonicalAggregateMovingRuntime(
    const CanonicalAggregateMovingRuntimeRequest& request) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;

  CanonicalAggregateMovingRuntimeResult result;
  result.descriptor = request.aggregate_request.descriptor;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    const auto descriptor = result.descriptor;
    result = {};
    result.descriptor = descriptor;
    result.diagnostic = std::move(diagnostic);
    return result;
  };
  const auto& aggregate = request.aggregate_request;
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!entry_authority.ok) return refuse(entry_authority);
  const auto* entry = LookupCanonicalAggregateExactV1(
      aggregate.descriptor.abi_version, aggregate.descriptor.function,
      aggregate.descriptor.builtin_id,
      aggregate.descriptor.function_uuid);
  if (entry == nullptr || !entry->executable ||
      !entry->aggregate_as_window || !entry->moving_window_inverse) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-UNAVAILABLE-V1",
        "aggregate registry row is not admitted for moving inverse state"));
  }
  if (aggregate.forced_strategy !=
          CanonicalAggregateExecutionStrategy::serial ||
      aggregate.distinct || !aggregate.aggregate_order_terms.empty()) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-MODIFIER-V1",
        "moving inverse state does not admit combine, DISTINCT, or aggregate ordering"));
  }
  if (request.maximum_output_rows == 0 ||
      request.effective_frame_row_indices.size() >
          request.maximum_output_rows ||
      request.maximum_addition_transition_count == 0 ||
      request.maximum_inverse_transition_count == 0 ||
      request.maximum_cumulative_state_bytes == 0) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "moving aggregate resource contract is invalid"));
  }
  if ((aggregate.descriptor.function == CanonicalAggregateFunction::sum ||
       aggregate.descriptor.function == CanonicalAggregateFunction::avg) &&
      (aggregate.value_columns.size() != 1 ||
       aggregate.value_columns.front() >= aggregate.input_batch.columns.size() ||
       aggregate.input_batch.columns[aggregate.value_columns.front()]
               .descriptor.canonical_type_name != "int64")) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-TYPE-V1",
        "moving SUM and AVG inverse state currently requires one int64 input"));
  }
  if (aggregate.filter_truth_values.has_value() &&
      aggregate.filter_truth_values->size() != aggregate.input_batch.rows.size()) {
    return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                          "moving aggregate FILTER cardinality is not bound"));
  }
  std::vector<std::uint32_t> input_descriptor_ids;
  input_descriptor_ids.reserve(aggregate.input_batch.columns.size());
  for (const auto& column : aggregate.input_batch.columns) {
    input_descriptor_ids.push_back(column.descriptor_id);
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      aggregate.input_batch, input_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  for (const auto& frame : request.effective_frame_row_indices) {
    if (!std::is_sorted(frame.begin(), frame.end()) ||
        std::adjacent_find(frame.begin(), frame.end()) != frame.end() ||
        (!frame.empty() && frame.back() >= aggregate.input_batch.rows.size())) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-INVERSE-FRAME-V1",
          "moving aggregate frame rows are not sorted unique input handles"));
    }
  }

  auto preflight_request = aggregate;
  preflight_request.input_batch.rows.clear();
  if (preflight_request.filter_truth_values.has_value()) {
    preflight_request.filter_truth_values =
        std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>{};
  }
  const auto preflight = ExecuteCanonicalAggregateRuntime(preflight_request);
  if (!preflight.diagnostic.ok) {
    return refuse(preflight.diagnostic);
  }
  if (!PhysicalMgaStatementContextEqual(
          preflight.mga_statement_context,
          aggregate.mga_authority.statement_context)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-MGA-V1",
        "moving aggregate preflight returned a different MGA statement context"));
  }

  DescriptorRuntimeDiagnostic state_diagnostic;
  CanonicalAggregateCoreState state;
  std::vector<std::size_t> active_frame;
  result.values.reserve(request.effective_frame_row_indices.size());
  const auto row_passes_filter = [&](const std::size_t row,
                                     bool* passes) {
    if (passes == nullptr) return false;
    *passes = true;
    if (!aggregate.filter_truth_values.has_value()) return true;
    std::string detail;
    if (!QowPredicateConsumerPassesV1(
            (*aggregate.filter_truth_values)[row],
            EnginePredicateConsumer::filter, passes, &detail)) {
      state_diagnostic = Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                                 std::move(detail));
      return false;
    }
    return true;
  };
  const auto transition_values = [&](const std::size_t row) {
    std::vector<EngineTypedValue> values;
    values.reserve(aggregate.value_columns.size());
    for (const auto column : aggregate.value_columns) {
      values.push_back(aggregate.input_batch.rows[row].values[column]);
    }
    return values;
  };
  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  for (const auto& frame : request.effective_frame_row_indices) {
    std::vector<std::size_t> removals;
    std::vector<std::size_t> additions;
    std::set_difference(active_frame.begin(), active_frame.end(),
                        frame.begin(), frame.end(),
                        std::back_inserter(removals));
    std::set_difference(frame.begin(), frame.end(), active_frame.begin(),
                        active_frame.end(), std::back_inserter(additions));
    for (const auto row : removals) {
      bool passes = true;
      if (!row_passes_filter(row, &passes)) {
        return refuse(std::move(state_diagnostic));
      }
      if (!passes) continue;
      if (result.inverse_transition_count >=
          request.maximum_inverse_transition_count) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "moving aggregate inverse bound is exceeded"));
      }
      if (!InverseCanonicalAggregateCore(
              aggregate.descriptor, transition_values(row), &state,
              &state_diagnostic)) {
        return refuse(std::move(state_diagnostic));
      }
      ++result.inverse_transition_count;
    }
    for (const auto row : additions) {
      bool passes = true;
      if (!row_passes_filter(row, &passes)) {
        return refuse(std::move(state_diagnostic));
      }
      if (!passes) continue;
      if (result.addition_transition_count >=
          request.maximum_addition_transition_count) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "moving aggregate addition bound is exceeded"));
      }
      if (!TransitionCanonicalAggregateCore(
              aggregate.descriptor, transition_values(row), &state,
              &state_diagnostic)) {
        return refuse(std::move(state_diagnostic));
      }
      ++result.addition_transition_count;
    }
    const auto state_bytes = EstimateCanonicalAggregateStateBytes(state);
    if (state_bytes > aggregate.maximum_state_bytes ||
        !within_total(state_bytes, result.cumulative_state_bytes,
                      request.maximum_cumulative_state_bytes)) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "moving aggregate state byte bound is exceeded"));
    }
    result.cumulative_state_bytes += state_bytes;
    result.maximum_retained_state_bytes =
        std::max(result.maximum_retained_state_bytes, state_bytes);
    auto value = FinalizeCanonicalAggregateCore(aggregate, state,
                                                &state_diagnostic);
    if (!state_diagnostic.ok) return refuse(std::move(state_diagnostic));
    result.values.push_back(std::move(value));
    active_frame = frame;
  }

  DescriptorBatch output;
  output.columns = {aggregate.result_column};
  output.rows.reserve(result.values.size());
  for (const auto& value : result.values) output.rows.push_back({{value}});
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {aggregate.result_column.descriptor_id});
  if (!output_validation.ok) return refuse(output_validation);
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.moving_inverse_state_used = true;
  result.frame_recomputation_used = false;
  result.authority = preflight.authority;
  result.selected_plan_uuid = preflight.selected_plan_uuid;
  result.executed_physical_node_id = preflight.executed_physical_node_id;
  result.causal_counter_id = preflight.causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-GROUPING-EXPANSION-V1
// Expand every accepted grouping form once, in the engine, before grouping
// state is allocated.  Explicit GROUPING SETS preserve source order and
// duplicates.  ROLLUP emits longest prefix to empty; CUBE emits descending
// membership masks (full set to empty), which also preserves the established
// two-key order: (a,b), (a), (b), ().
CanonicalAggregateGroupingExpansionResult
ExpandCanonicalAggregateGroupingSets(
    const CanonicalAggregateGroupingExpansionRequest& request) {
  CanonicalAggregateGroupingExpansionResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result.diagnostic = Refusal(std::move(code), std::move(detail));
    result.grouping_sets.clear();
    result.grouping_set_member_count = 0;
    result.repeated_explicit_sets_preserved = false;
    return result;
  };
  const auto add_member_count = [&](const std::size_t count) {
    if (result.grouping_set_member_count >
            request.maximum_grouping_set_member_count ||
        count > request.maximum_grouping_set_member_count -
                    result.grouping_set_member_count) {
      return false;
    }
    result.grouping_set_member_count += count;
    return true;
  };

  if (request.group_key_count > 64) {
    return refuse("QOW-DIAG-QRY-011-GROUPING-EXPANSION-V1",
                  "GROUPING_ID admits at most 64 ordered grouping keys");
  }
  if (request.maximum_grouping_set_count == 0 ||
      request.maximum_grouping_set_member_count == 0) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "grouping-set expansion limits must be nonzero");
  }

  switch (request.kind) {
    case CanonicalAggregateGroupingExpansionKind::explicit_sets: {
      if (request.explicit_grouping_sets.empty() ||
          request.explicit_grouping_sets.size() >
              request.maximum_grouping_set_count) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "explicit grouping-set count is empty or exceeds its bound");
      }
      std::set<std::vector<std::size_t>> observed;
      result.grouping_sets.reserve(request.explicit_grouping_sets.size());
      for (const auto& grouping_set : request.explicit_grouping_sets) {
        std::optional<std::size_t> previous;
        for (const auto ordinal : grouping_set.key_term_ordinals) {
          if (ordinal >= request.group_key_count ||
              (previous.has_value() && ordinal <= *previous)) {
            return refuse(
                "QOW-DIAG-QRY-011-GROUPING-EXPANSION-V1",
                "explicit grouping-set members must be unique, increasing, and bound");
          }
          previous = ordinal;
        }
        if (!add_member_count(grouping_set.key_term_ordinals.size())) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "explicit grouping-set member bound is exceeded");
        }
        if (!observed.insert(grouping_set.key_term_ordinals).second) {
          result.repeated_explicit_sets_preserved = true;
        }
        result.grouping_sets.push_back(grouping_set);
      }
      break;
    }
    case CanonicalAggregateGroupingExpansionKind::rollup: {
      if (request.group_key_count ==
              std::numeric_limits<std::size_t>::max() ||
          request.group_key_count + 1 > request.maximum_grouping_set_count) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "ROLLUP grouping-set count exceeds its bound");
      }
      result.grouping_sets.reserve(request.group_key_count + 1);
      for (std::size_t prefix_size = request.group_key_count;;
           --prefix_size) {
        if (!add_member_count(prefix_size)) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "ROLLUP grouping-set member bound is exceeded");
        }
        CanonicalAggregateGroupingSet grouping_set;
        grouping_set.key_term_ordinals.resize(prefix_size);
        std::iota(grouping_set.key_term_ordinals.begin(),
                  grouping_set.key_term_ordinals.end(), 0);
        result.grouping_sets.push_back(std::move(grouping_set));
        if (prefix_size == 0) break;
      }
      break;
    }
    case CanonicalAggregateGroupingExpansionKind::cube: {
      std::size_t grouping_set_count = 1;
      for (std::size_t key = 0; key < request.group_key_count; ++key) {
        if (grouping_set_count >
            request.maximum_grouping_set_count / 2) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "CUBE grouping-set count exceeds its bound");
        }
        grouping_set_count *= 2;
      }
      result.grouping_sets.reserve(grouping_set_count);
      for (std::size_t descending = grouping_set_count; descending > 0;
           --descending) {
        const auto membership_mask = descending - 1;
        CanonicalAggregateGroupingSet grouping_set;
        for (std::size_t key = 0; key < request.group_key_count; ++key) {
          const auto bit = request.group_key_count - 1 - key;
          if ((membership_mask & (std::size_t{1} << bit)) != 0) {
            grouping_set.key_term_ordinals.push_back(key);
          }
        }
        if (!add_member_count(grouping_set.key_term_ordinals.size())) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "CUBE grouping-set member bound is exceeded");
        }
        result.grouping_sets.push_back(std::move(grouping_set));
      }
      break;
    }
    default:
      return refuse("QOW-DIAG-QRY-011-GROUPING-EXPANSION-V1",
                    "grouping-set expansion kind is unknown");
  }

  result.diagnostic = {};
  return result;
}

CanonicalAggregateGroupingMetadataResult
ComputeCanonicalAggregateGroupingMetadata(
    const std::size_t group_key_count,
    const CanonicalAggregateGroupingSet& grouping_set) {
  CanonicalAggregateGroupingMetadataResult result;
  CanonicalAggregateGroupingExpansionRequest validation;
  validation.kind = CanonicalAggregateGroupingExpansionKind::explicit_sets;
  validation.group_key_count = group_key_count;
  validation.explicit_grouping_sets = {grouping_set};
  validation.maximum_grouping_set_count = 1;
  validation.maximum_grouping_set_member_count = 64;
  const auto expanded = ExpandCanonicalAggregateGroupingSets(validation);
  if (!expanded.diagnostic.ok) {
    result.diagnostic = expanded.diagnostic;
    return result;
  }

  result.grouping_indicators.assign(group_key_count, true);
  for (const auto ordinal : grouping_set.key_term_ordinals) {
    result.grouping_indicators[ordinal] = false;
  }
  for (std::size_t key = 0; key < group_key_count; ++key) {
    if (result.grouping_indicators[key]) {
      result.grouping_id |=
          std::uint64_t{1} << (group_key_count - 1 - key);
    }
  }
  result.diagnostic = {};
  return result;
}

// QOW-SOURCE-QRY-011-GROUPED-REGISTRY-V1
// Build explicit grouping sets over ordered, descriptor-bound keys and route
// every resulting group through the one canonical aggregate registry/state
// authority above.  Omitted grouping keys are published as typed SQL NULL and
// remain distinguishable from data NULL through GROUPING/GROUPING_ID metadata.
static CanonicalGroupedAggregateRuntimeResult
ExecuteCanonicalGroupedAggregateRuntimeSelected(
    const CanonicalGroupedAggregateRuntimeRequest& request,
    const bool spill_execution_context) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalGroupedAggregateRuntimeResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    result.groups.clear();
    result.grouping_set_count = 0;
    result.grouping_key_comparison_count = 0;
    result.grouping_set_transition_count = 0;
    result.aggregate_transition_count = 0;
    result.aggregate_distinct_tuple_count = 0;
    result.aggregate_order_comparison_count = 0;
    result.combined_state_bytes = 0;
    result.aggregate_state_spill_required = false;
    result.shared_state_authority_used = false;
    result.authority = {};
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  const auto grouped_refusal = [&](std::string detail) {
    return refuse(Refusal("QOW-DIAG-QRY-011-GROUPED-REFUSAL-V1",
                          std::move(detail)));
  };

  const auto& aggregate = request.aggregate_request;
  if (request.grouping_sets.empty() ||
      request.group_key_terms.size() > 64 ||
      request.group_result_columns.size() !=
          request.group_key_terms.size() ||
      request.maximum_grouping_set_count == 0 ||
      request.maximum_grouping_set_member_count == 0 ||
      request.maximum_group_count == 0 ||
      request.maximum_grouping_key_comparison_count == 0 ||
      request.maximum_grouping_set_transition_count == 0 ||
      request.maximum_combined_distinct_tuple_count == 0 ||
      request.maximum_combined_order_comparison_count == 0 ||
      request.maximum_combined_state_bytes == 0 ||
      request.maximum_output_rows == 0) {
    return grouped_refusal(
        "grouped aggregate shape or resource contract is invalid");
  }

  CanonicalAggregateGroupingExpansionRequest expansion_request;
  expansion_request.kind =
      CanonicalAggregateGroupingExpansionKind::explicit_sets;
  expansion_request.group_key_count = request.group_key_terms.size();
  expansion_request.explicit_grouping_sets = request.grouping_sets;
  expansion_request.maximum_grouping_set_count =
      request.maximum_grouping_set_count;
  expansion_request.maximum_grouping_set_member_count =
      request.maximum_grouping_set_member_count;
  const auto expansion =
      ExpandCanonicalAggregateGroupingSets(expansion_request);
  if (!expansion.diagnostic.ok) {
    return refuse(expansion.diagnostic);
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (aggregate.selected_physical_node_id == 0 ||
      aggregate.selected_physical_node_id !=
          aggregate.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected grouped aggregate node is not the root"));
  }
  const PhysicalNodeRecord* aggregate_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id == aggregate.selected_physical_node_id) {
      aggregate_node = &node;
    }
  }
  if (aggregate_node == nullptr ||
      aggregate_node->node_kind != PhysicalNodeKind::kAggregate ||
      aggregate_node->input_physical_node_ids.size() != 1) {
    return grouped_refusal(
        "selected physical node is not the grouped registry aggregate");
  }
  bool aggregate_state_spill_required = false;
  if (aggregate_node->implementation_id ==
      "aggregate.registry-grouping-sets.v1") {
    aggregate_state_spill_required = false;
  } else if (aggregate_node->implementation_id ==
             "aggregate.registry-grouping-sets-state-spill.v1") {
    aggregate_state_spill_required = true;
  } else {
    return grouped_refusal(
        "selected physical implementation is not a grouped registry strategy");
  }
  if (aggregate_state_spill_required != spill_execution_context) {
    return grouped_refusal(
        aggregate_state_spill_required
            ? "selected grouped state spill requires the spill runtime"
            : "grouped spill payload does not match the selected implementation");
  }
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id ==
        aggregate_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      aggregate_node->output_descriptor_ids.size() !=
          request.group_result_columns.size() + 1 ||
      aggregate.result_column.descriptor_id == 0 ||
      aggregate_node->output_descriptor_ids.back() !=
          aggregate.result_column.descriptor_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "grouped aggregate input or result shape is unresolved"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      aggregate.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  std::set<std::uint32_t> group_expression_ids;
  std::vector<bool> key_can_be_omitted(request.group_key_terms.size(), false);
  std::vector<CanonicalDescriptorOrderTerm> group_result_terms;
  group_result_terms.reserve(request.group_key_terms.size());
  for (std::size_t index = 0; index < request.group_key_terms.size(); ++index) {
    const auto& term = request.group_key_terms[index];
    if (term.column >= aggregate.input_batch.columns.size()) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "group key column is unresolved"));
    }
    auto term_validation = ValidateCanonicalDescriptorOrderTerm(
        term, aggregate.input_batch.columns[term.column]);
    if (!term_validation.ok) return refuse(std::move(term_validation));
    if (!group_expression_ids.insert(term.expression_descriptor_id).second) {
      return grouped_refusal("group key expression handles are not unique");
    }
    const auto& output = request.group_result_columns[index];
    if (output.descriptor_id == 0 ||
        aggregate_node->output_descriptor_ids[index] != output.descriptor_id ||
        output.descriptor.descriptor_kind !=
            aggregate.input_batch.columns[term.column]
                .descriptor.descriptor_kind ||
        output.descriptor.canonical_type_name !=
            aggregate.input_batch.columns[term.column]
                .descriptor.canonical_type_name) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "group result descriptor is not bound to its key"));
    }
    auto output_term = term;
    output_term.column = index;
    output_term.expression_descriptor_id = output.descriptor_id;
    auto output_term_validation =
        ValidateCanonicalDescriptorOrderTerm(output_term, output);
    if (!output_term_validation.ok) {
      return refuse(std::move(output_term_validation));
    }
    group_result_terms.push_back(std::move(output_term));
  }

  std::vector<std::vector<bool>> grouping_membership;
  grouping_membership.reserve(request.grouping_sets.size());
  for (const auto& grouping_set : request.grouping_sets) {
    std::vector<bool> included(request.group_key_terms.size(), false);
    std::optional<std::size_t> previous;
    for (const auto ordinal : grouping_set.key_term_ordinals) {
      if (ordinal >= request.group_key_terms.size() ||
          (previous.has_value() && ordinal <= *previous)) {
        return grouped_refusal(
            "grouping-set key ordinals must be unique, increasing, and bound");
      }
      included[ordinal] = true;
      previous = ordinal;
    }
    for (std::size_t index = 0; index < included.size(); ++index) {
      if (!included[index]) key_can_be_omitted[index] = true;
    }
    grouping_membership.push_back(std::move(included));
  }
  for (std::size_t index = 0; index < request.group_result_columns.size();
       ++index) {
    const auto& input_column = aggregate.input_batch.columns[
        request.group_key_terms[index].column];
    const bool expected_output_nullable =
        input_column.nullable || key_can_be_omitted[index];
    const auto& output_column = request.group_result_columns[index];
    if (output_column.nullable != expected_output_nullable ||
        !CanonicalDerivedDescriptorTypeMatches(
            input_column.descriptor, input_column.nullable,
            output_column.descriptor, expected_output_nullable)) {
      return grouped_refusal(
          "group result descriptor does not exactly preserve key type and required nullability");
    }
  }

  const auto row_count = aggregate.input_batch.rows.size();
  if (aggregate.filter_truth_values.has_value() &&
      aggregate.filter_truth_values->size() != row_count) {
    return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                          "grouped aggregate FILTER cardinality is not bound"));
  }
  if (row_count != 0 &&
      request.grouping_sets.size() >
          request.maximum_grouping_set_transition_count / row_count) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "grouping-set transition bound is exceeded"));
  }
  result.grouping_set_transition_count =
      row_count * request.grouping_sets.size();

  const auto make_kernel_request = [&]() {
    auto kernel_request = aggregate;
    // The grouped node's complete output was validated above.  Its internal
    // registry-state kernel publishes only the aggregate field, so project
    // that already-bound field while retaining the exact plan/node/MGA IDs.
    for (auto& node : kernel_request.physical_dag.nodes) {
      if (node.physical_node_id == kernel_request.selected_physical_node_id) {
        node.output_descriptor_ids = {
            kernel_request.result_column.descriptor_id};
        break;
      }
    }
    return kernel_request;
  };
  auto preflight_request = make_kernel_request();
  preflight_request.input_batch.rows.clear();
  if (preflight_request.filter_truth_values.has_value()) {
    preflight_request.filter_truth_values =
        std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>{};
  }
  const auto preflight = ExecuteCanonicalAggregateRuntime(preflight_request);
  if (!preflight.diagnostic.ok) {
    return refuse(preflight.diagnostic);
  }

  struct WorkingGroup {
    std::uint32_t grouping_set_ordinal = 0;
    std::uint64_t grouping_id = 0;
    std::vector<bool> grouping_indicators;
    std::size_t representative_row = 0;
    std::vector<std::size_t> source_rows;
  };
  std::vector<WorkingGroup> working_groups;
  const auto compare_key = [&](const EngineTypedValue& left,
                               const EngineTypedValue& right,
                               const CanonicalDescriptorOrderTerm& term,
                               bool* equal) {
    if (result.grouping_key_comparison_count ==
        request.maximum_grouping_key_comparison_count) {
      return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                     "grouping-key comparison bound is exceeded");
    }
    ++result.grouping_key_comparison_count;
    const auto compared = CompareCanonicalDescriptorOrderValues(
        left, right, term);
    if (!compared.diagnostic.ok) return compared.diagnostic;
    *equal = compared.comparison == 0;
    return DescriptorRuntimeDiagnostic{};
  };
  const auto add_group = [&](WorkingGroup group) {
    if (working_groups.size() == request.maximum_group_count ||
        working_groups.size() == request.maximum_output_rows) {
      return false;
    }
    working_groups.push_back(std::move(group));
    return true;
  };

  for (std::size_t set_ordinal = 0;
       set_ordinal < grouping_membership.size(); ++set_ordinal) {
    const auto& included = grouping_membership[set_ordinal];
    const auto metadata = ComputeCanonicalAggregateGroupingMetadata(
        request.group_key_terms.size(), request.grouping_sets[set_ordinal]);
    if (!metadata.diagnostic.ok) {
      return refuse(metadata.diagnostic);
    }
    bool grand_total = true;
    for (std::size_t index = 0; index < included.size(); ++index) {
      if (included[index]) {
        grand_total = false;
      }
    }
    if (grand_total) {
      WorkingGroup group;
      group.grouping_set_ordinal = static_cast<std::uint32_t>(set_ordinal);
      group.grouping_id = metadata.grouping_id;
      group.grouping_indicators = metadata.grouping_indicators;
      group.source_rows.resize(row_count);
      std::iota(group.source_rows.begin(), group.source_rows.end(), 0);
      if (!add_group(std::move(group))) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "group or output row bound is exceeded"));
      }
      continue;
    }

    const auto set_group_begin = working_groups.size();
    for (std::size_t row = 0; row < row_count; ++row) {
      std::optional<std::size_t> matching_group;
      for (std::size_t group_index = set_group_begin;
           group_index < working_groups.size(); ++group_index) {
        bool matches = true;
        for (std::size_t term_index = 0; term_index < included.size();
             ++term_index) {
          if (!included[term_index]) continue;
          const auto& term = request.group_key_terms[term_index];
          bool equal = false;
          auto comparison = compare_key(
              aggregate.input_batch.rows[row].values[term.column],
              aggregate.input_batch
                  .rows[working_groups[group_index].representative_row]
                  .values[term.column],
              term, &equal);
          if (!comparison.ok) return refuse(std::move(comparison));
          if (!equal) {
            matches = false;
            break;
          }
        }
        if (matches) {
          matching_group = group_index;
          break;
        }
      }
      if (matching_group.has_value()) {
        working_groups[*matching_group].source_rows.push_back(row);
        continue;
      }

      // A first representative has no peer comparison, so self-compare each
      // included key to validate its typed encoding and collation authority.
      for (std::size_t term_index = 0; term_index < included.size();
           ++term_index) {
        if (!included[term_index]) continue;
        const auto& term = request.group_key_terms[term_index];
        bool equal = false;
        auto comparison = compare_key(
            aggregate.input_batch.rows[row].values[term.column],
            aggregate.input_batch.rows[row].values[term.column], term,
            &equal);
        if (!comparison.ok) return refuse(std::move(comparison));
        if (!equal) {
          return grouped_refusal("group key is not equal to itself");
        }
      }
      WorkingGroup group;
      group.grouping_set_ordinal = static_cast<std::uint32_t>(set_ordinal);
      group.grouping_id = metadata.grouping_id;
      group.grouping_indicators = metadata.grouping_indicators;
      group.representative_row = row;
      group.source_rows = {row};
      if (!add_group(std::move(group))) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "group or output row bound is exceeded"));
      }
    }
  }

  result.output_batch.columns = request.group_result_columns;
  result.output_batch.columns.push_back(aggregate.result_column);
  result.output_batch.rows.reserve(working_groups.size());
  result.groups.reserve(working_groups.size());
  for (const auto& group : working_groups) {
    auto group_request = make_kernel_request();
    group_request.input_batch.rows.clear();
    group_request.input_batch.rows.reserve(group.source_rows.size());
    for (const auto row : group.source_rows) {
      group_request.input_batch.rows.push_back(aggregate.input_batch.rows[row]);
    }
    if (aggregate.filter_truth_values.has_value()) {
      std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
          group_filter;
      group_filter.reserve(group.source_rows.size());
      for (const auto row : group.source_rows) {
        group_filter.push_back((*aggregate.filter_truth_values)[row]);
      }
      group_request.filter_truth_values = std::move(group_filter);
    }
    auto aggregate_result = ExecuteCanonicalAggregateRuntime(group_request);
    if (!aggregate_result.diagnostic.ok) {
      return refuse(std::move(aggregate_result.diagnostic));
    }
    if (aggregate_result.output_batch.rows.size() != 1 ||
        aggregate_result.output_batch.rows.front().values.size() != 1 ||
        !aggregate_result.shared_state_authority_used) {
      return grouped_refusal(
          "shared aggregate state returned an invalid group result");
    }
    if (aggregate_result.state_bytes >
        request.maximum_combined_state_bytes - result.combined_state_bytes) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "combined grouped state byte bound is exceeded"));
    }
    result.combined_state_bytes += aggregate_result.state_bytes;
    result.aggregate_transition_count += aggregate_result.transition_count;
    if (aggregate_result.distinct_tuple_count >
        request.maximum_combined_distinct_tuple_count -
            result.aggregate_distinct_tuple_count) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "combined grouped DISTINCT bound is exceeded"));
    }
    result.aggregate_distinct_tuple_count +=
        aggregate_result.distinct_tuple_count;
    if (aggregate_result.order_comparison_count >
        request.maximum_combined_order_comparison_count -
            result.aggregate_order_comparison_count) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "combined grouped aggregate-order bound is exceeded"));
    }
    result.aggregate_order_comparison_count +=
        aggregate_result.order_comparison_count;

    DescriptorTuple output_row;
    output_row.values.reserve(request.group_result_columns.size() + 1);
    for (std::size_t key_index = 0;
         key_index < request.group_result_columns.size(); ++key_index) {
      EngineTypedValue value;
      if (group.grouping_indicators[key_index]) {
        value.descriptor = request.group_result_columns[key_index].descriptor;
        value.is_null = true;
        value.state = EngineValueState::sql_null;
      } else {
        const auto source_column = request.group_key_terms[key_index].column;
        value = aggregate.input_batch.rows[group.representative_row]
                    .values[source_column];
        value.descriptor = request.group_result_columns[key_index].descriptor;
        bool self_equal = false;
        auto comparison = compare_key(value, value,
                                      group_result_terms[key_index],
                                      &self_equal);
        if (!comparison.ok) return refuse(std::move(comparison));
        if (!self_equal) {
          return grouped_refusal(
              "published group key is not equal to itself");
        }
      }
      output_row.values.push_back(std::move(value));
    }
    output_row.values.push_back(
        std::move(aggregate_result.output_batch.rows.front().values.front()));
    result.output_batch.rows.push_back(std::move(output_row));
    result.groups.push_back(
        {.grouping_set_ordinal = group.grouping_set_ordinal,
         .grouping_id = group.grouping_id,
         .grouping_indicators = group.grouping_indicators,
         .source_row_indices = group.source_rows,
         .source_row_count = group.source_rows.size(),
         .aggregate_transition_count = aggregate_result.transition_count,
         .aggregate_state_bytes = aggregate_result.state_bytes});
  }

  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, aggregate_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.grouping_set_count = request.grouping_sets.size();
  result.aggregate_state_spill_required = aggregate_state_spill_required;
  result.shared_state_authority_used = true;
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.owns_transaction_finality = false;
  result.authority.owns_recovery = false;
  result.authority.owns_parser_execution = false;
  result.authority.owns_visibility_outside_engine_mga = false;
  result.authority.wal_is_transaction_or_recovery_authority = false;
  result.selected_plan_uuid = aggregate.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = aggregate_node->physical_node_id;
  result.causal_counter_id = aggregate_node->causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

CanonicalGroupedAggregateRuntimeResult ExecuteCanonicalGroupedAggregateRuntime(
    const CanonicalGroupedAggregateRuntimeRequest& request) {
  return ExecuteCanonicalGroupedAggregateRuntimeSelected(request, false);
}

// QOW-SOURCE-QRY-011-GROUPED-SET-V1
// Compose several registry aggregates over one exact grouped physical node.
// Group identity must match across every independently filtered/ordered state;
// aggregate values are then appended in the physical output-descriptor order.
static CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntimeSelected(
    const CanonicalGroupedAggregateSetRuntimeRequest& request,
    const bool spill_execution_context) {
  CanonicalGroupedAggregateSetRuntimeResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    result.groups.clear();
    result.aggregate_count = 0;
    result.grouping_set_transition_count = 0;
    result.grouping_key_comparison_count = 0;
    result.aggregate_transition_count = 0;
    result.aggregate_distinct_tuple_count = 0;
    result.aggregate_order_comparison_count = 0;
    result.combined_state_bytes = 0;
    result.group_identity_proven = false;
    result.aggregate_state_spill_required = false;
    result.shared_state_authority_used = false;
    result.authority = {};
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  const auto set_refusal = [&](std::string detail) {
    return refuse(Refusal("QOW-DIAG-QRY-011-GROUPED-SET-REFUSAL-V1",
                          std::move(detail)));
  };

  const auto aggregate_count = request.additional_aggregates.size() + 1;
  if (aggregate_count == 0 ||
      aggregate_count > request.maximum_aggregate_count ||
      request.maximum_combined_grouping_set_transition_count == 0 ||
      request.maximum_combined_grouping_key_comparison_count == 0 ||
      request.maximum_combined_aggregate_transition_count == 0 ||
      request.maximum_combined_distinct_tuple_count == 0 ||
      request.maximum_combined_order_comparison_count == 0 ||
      request.maximum_combined_state_bytes == 0) {
    return set_refusal("aggregate-set shape or resource contract is invalid");
  }

  const auto& first = request.first_aggregate;
  const auto& common = first.aggregate_request;
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      common.mga_authority, common.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const PhysicalNodeRecord* aggregate_node = nullptr;
  for (const auto& node : common.physical_dag.nodes) {
    if (node.physical_node_id == common.selected_physical_node_id) {
      aggregate_node = &node;
      break;
    }
  }
  if (common.selected_physical_node_id == 0 ||
      common.selected_physical_node_id !=
          common.physical_dag.root_physical_node_id ||
      aggregate_node == nullptr ||
      aggregate_node->node_kind != PhysicalNodeKind::kAggregate ||
      aggregate_node->output_descriptor_ids.size() !=
          first.group_result_columns.size() + aggregate_count) {
    return set_refusal(
        "selected physical node is not the exact grouped aggregate set");
  }
  bool aggregate_state_spill_required = false;
  if (aggregate_node->implementation_id ==
      "aggregate.registry-grouping-sets.v1") {
    aggregate_state_spill_required = false;
  } else if (aggregate_node->implementation_id ==
             "aggregate.registry-grouping-sets-state-spill.v1") {
    aggregate_state_spill_required = true;
  } else {
    return set_refusal(
        "selected physical implementation is not a grouped aggregate-set strategy");
  }
  if (aggregate_state_spill_required != spill_execution_context) {
    return set_refusal(
        aggregate_state_spill_required
            ? "selected grouped aggregate-set spill requires the spill runtime"
            : "grouped aggregate-set spill payload does not match the selected implementation");
  }

  std::vector<const CanonicalAggregateRuntimeRequest*> aggregate_specs = {
      &common};
  aggregate_specs.reserve(aggregate_count);
  const auto has_shadow_authority = [](const auto& specification) {
    const auto& dag = specification.physical_dag;
    return specification.selected_physical_node_id != 0 ||
           !specification.input_batch.columns.empty() ||
           !specification.input_batch.rows.empty() || dag.abi_version != 1 ||
           !dag.selected_plan_uuid.empty() || dag.root_physical_node_id != 0 ||
           dag.local_transaction_id != 0 || dag.statement_snapshot_id != 0 ||
           !dag.admission_evidence.empty() || !dag.nodes.empty() ||
           !dag.bound_sblr_tree_uuid.empty() ||
           !dag.catalog_epoch_uuid.empty() ||
           !dag.security_context_uuid.empty() ||
           !dag.capability_snapshot_uuid.empty() ||
           !dag.resource_snapshot_uuid.empty() ||
           !dag.statistics_snapshot_uuid.empty() ||
           !dag.route_snapshot_uuid.empty() || dag.catalog_generation != 0 ||
           dag.security_epoch != 0 || dag.policy_epoch != 0 ||
           dag.resource_epoch != 0 || dag.statistics_generation != 0 ||
           dag.route_epoch != 0 || dag.route_generation != 0 ||
           dag.memory_budget_bytes != 0 || dag.spill_allowed ||
           dag.optimizer_published || dag.immutable_node_identity_validated ||
           dag.capability_validated_before_access || dag.data_access_observed ||
           dag.parser_execution_authority_claimed ||
           dag.transaction_finality_authority_claimed;
  };
  for (const auto& specification : request.additional_aggregates) {
    if (has_shadow_authority(specification)) {
      return set_refusal(
          "additional aggregate carries shadow physical or input authority");
    }
    const auto additional_authority = RevalidateCanonicalExecutionMgaAuthority(
        specification.mga_authority, common.physical_dag);
    if (!additional_authority.ok ||
        !PhysicalMgaStatementContextEqual(
            specification.mga_authority.statement_context,
            common.mga_authority.statement_context)) {
      return set_refusal(
          "additional aggregate carries a missing, stale, or different MGA statement context");
    }
    aggregate_specs.push_back(&specification);
  }

  std::vector<std::uint32_t> expected_output_ids;
  expected_output_ids.reserve(first.group_result_columns.size() +
                              aggregate_count);
  for (const auto& column : first.group_result_columns) {
    expected_output_ids.push_back(column.descriptor_id);
  }
  std::set<std::uint32_t> aggregate_result_ids;
  for (const auto* specification : aggregate_specs) {
    if (specification->result_column.descriptor_id == 0 ||
        !aggregate_result_ids
             .insert(specification->result_column.descriptor_id)
             .second) {
      return set_refusal(
          "aggregate result descriptor handles are zero or repeated");
    }
    expected_output_ids.push_back(specification->result_column.descriptor_id);
  }
  if (aggregate_node->output_descriptor_ids != expected_output_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "aggregate-set result order is not physically bound"));
  }

  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  std::vector<CanonicalGroupedAggregateRuntimeResult> executions;
  executions.reserve(aggregate_count);
  for (std::size_t index = 0; index < aggregate_specs.size(); ++index) {
    auto grouped_request = first;
    grouped_request.aggregate_request = *aggregate_specs[index];
    grouped_request.aggregate_request.physical_dag = common.physical_dag;
    grouped_request.aggregate_request.selected_physical_node_id =
        common.selected_physical_node_id;
    grouped_request.aggregate_request.input_batch = common.input_batch;
    grouped_request.aggregate_request.mga_authority = common.mga_authority;
    for (auto& node : grouped_request.aggregate_request.physical_dag.nodes) {
      if (node.physical_node_id == common.selected_physical_node_id) {
        node.output_descriptor_ids.clear();
        for (const auto& column : first.group_result_columns) {
          node.output_descriptor_ids.push_back(column.descriptor_id);
        }
        node.output_descriptor_ids.push_back(
            grouped_request.aggregate_request.result_column.descriptor_id);
        break;
      }
    }
    auto execution = ExecuteCanonicalGroupedAggregateRuntimeSelected(
        grouped_request, spill_execution_context);
    if (!execution.diagnostic.ok) {
      return refuse(std::move(execution.diagnostic));
    }
    if (!within_total(execution.grouping_set_transition_count,
                      result.grouping_set_transition_count,
                      request.maximum_combined_grouping_set_transition_count) ||
        !within_total(execution.grouping_key_comparison_count,
                      result.grouping_key_comparison_count,
                      request.maximum_combined_grouping_key_comparison_count) ||
        !within_total(execution.aggregate_transition_count,
                      result.aggregate_transition_count,
                      request.maximum_combined_aggregate_transition_count) ||
        !within_total(execution.aggregate_distinct_tuple_count,
                      result.aggregate_distinct_tuple_count,
                      request.maximum_combined_distinct_tuple_count) ||
        !within_total(execution.aggregate_order_comparison_count,
                      result.aggregate_order_comparison_count,
                      request.maximum_combined_order_comparison_count) ||
        !within_total(execution.combined_state_bytes,
                      result.combined_state_bytes,
                      request.maximum_combined_state_bytes)) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "combined aggregate-set resource bound is exceeded"));
    }
    result.grouping_set_transition_count +=
        execution.grouping_set_transition_count;
    result.grouping_key_comparison_count +=
        execution.grouping_key_comparison_count;
    result.aggregate_transition_count += execution.aggregate_transition_count;
    result.aggregate_distinct_tuple_count +=
        execution.aggregate_distinct_tuple_count;
    result.aggregate_order_comparison_count +=
        execution.aggregate_order_comparison_count;
    result.combined_state_bytes += execution.combined_state_bytes;
    executions.push_back(std::move(execution));
  }

  const auto& identity = executions.front();
  const auto key_count = first.group_result_columns.size();
  for (std::size_t execution_index = 1;
       execution_index < executions.size(); ++execution_index) {
    const auto& candidate = executions[execution_index];
    if (candidate.groups.size() != identity.groups.size() ||
        candidate.output_batch.rows.size() !=
            identity.output_batch.rows.size()) {
      return set_refusal("aggregate group cardinalities diverged");
    }
    for (std::size_t group = 0; group < identity.groups.size(); ++group) {
      const auto& expected = identity.groups[group];
      const auto& actual = candidate.groups[group];
      if (actual.grouping_set_ordinal != expected.grouping_set_ordinal ||
          actual.grouping_id != expected.grouping_id ||
          actual.grouping_indicators != expected.grouping_indicators ||
          actual.source_row_indices != expected.source_row_indices ||
          actual.source_row_count != expected.source_row_count) {
        return set_refusal("aggregate grouping metadata diverged");
      }
      for (std::size_t key = 0; key < key_count; ++key) {
        const auto& expected_value =
            identity.output_batch.rows[group].values[key];
        const auto& actual_value =
            candidate.output_batch.rows[group].values[key];
        if (actual_value.state != expected_value.state ||
            actual_value.is_null != expected_value.is_null ||
            actual_value.encoded_value != expected_value.encoded_value ||
            actual_value.binary_value != expected_value.binary_value ||
            actual_value.descriptor.descriptor_uuid.canonical !=
                expected_value.descriptor.descriptor_uuid.canonical ||
            actual_value.descriptor.canonical_type_name !=
                expected_value.descriptor.canonical_type_name ||
            actual_value.descriptor.encoded_descriptor !=
                expected_value.descriptor.encoded_descriptor) {
          return set_refusal("aggregate group key values diverged");
        }
      }
    }
  }

  result.output_batch.columns = first.group_result_columns;
  for (const auto* specification : aggregate_specs) {
    result.output_batch.columns.push_back(specification->result_column);
  }
  result.output_batch.rows.reserve(identity.output_batch.rows.size());
  result.groups.reserve(identity.groups.size());
  for (std::size_t group = 0; group < identity.groups.size(); ++group) {
    DescriptorTuple row;
    row.values.insert(row.values.end(),
                      identity.output_batch.rows[group].values.begin(),
                      identity.output_batch.rows[group].values.begin() +
                          static_cast<std::ptrdiff_t>(key_count));
    CanonicalGroupedAggregateSetMetadata metadata;
    metadata.grouping_set_ordinal =
        identity.groups[group].grouping_set_ordinal;
    metadata.grouping_id = identity.groups[group].grouping_id;
    metadata.grouping_indicators =
        identity.groups[group].grouping_indicators;
    metadata.source_row_indices =
        identity.groups[group].source_row_indices;
    metadata.source_row_count = identity.groups[group].source_row_count;
    metadata.aggregate_transition_counts.reserve(aggregate_count);
    metadata.aggregate_state_bytes.reserve(aggregate_count);
    for (const auto& execution : executions) {
      row.values.push_back(execution.output_batch.rows[group].values[key_count]);
      metadata.aggregate_transition_counts.push_back(
          execution.groups[group].aggregate_transition_count);
      metadata.aggregate_state_bytes.push_back(
          execution.groups[group].aggregate_state_bytes);
    }
    result.output_batch.rows.push_back(std::move(row));
    result.groups.push_back(std::move(metadata));
  }

  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, aggregate_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      common.mga_authority, common.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.aggregate_count = aggregate_count;
  result.group_identity_proven = true;
  result.aggregate_state_spill_required = aggregate_state_spill_required;
  result.shared_state_authority_used = true;
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.owns_transaction_finality = false;
  result.authority.owns_recovery = false;
  result.authority.owns_parser_execution = false;
  result.authority.owns_visibility_outside_engine_mga = false;
  result.authority.wal_is_transaction_or_recovery_authority = false;
  result.selected_plan_uuid = common.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = aggregate_node->physical_node_id;
  result.causal_counter_id = aggregate_node->causal_counter_id;
  result.mga_statement_context = common.mga_authority.statement_context;
  return result;
}

CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntime(
    const CanonicalGroupedAggregateSetRuntimeRequest& request) {
  return ExecuteCanonicalGroupedAggregateSetRuntimeSelected(request, false);
}

// QOW-SOURCE-QRY-011-GROUPED-SET-STATE-SPILL-V1
// Preserve canonical grouping identity in memory, then serialize, spill,
// restore, and ordinarily finalize each aggregate state for each physical
// group. The optimizer-selected grouped implementation owns the route, while
// temporary work remains subordinate to engine MGA/security/recheck authority.
CanonicalGroupedAggregateSetStateSpillResult
ExecuteCanonicalGroupedAggregateSetStateSpill(
    const CanonicalGroupedAggregateSetStateSpillRequest& request) {
  using scratchbird::engine::internal_api::EngineSqlTruthValue;

  CanonicalGroupedAggregateSetStateSpillResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.grouped_result = {};
    return result;
  };

  const auto& entry_common =
      request.grouped_request.first_aggregate.aggregate_request;
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      entry_common.mga_authority, entry_common.physical_dag);
  if (!entry_authority.ok) {
    return refuse(entry_authority.diagnostic_code, entry_authority.detail);
  }
  for (const auto& additional : request.grouped_request.additional_aggregates) {
    const auto additional_authority = RevalidateCanonicalExecutionMgaAuthority(
        additional.mga_authority, entry_common.physical_dag);
    if (!additional_authority.ok ||
        !PhysicalMgaStatementContextEqual(
            additional.mga_authority.statement_context,
            entry_common.mga_authority.statement_context)) {
      return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-MGA-V1",
                    "grouped spill aggregate carriers do not share one current MGA statement context");
    }
  }

  const auto baseline = ExecuteCanonicalGroupedAggregateSetRuntimeSelected(
      request.grouped_request, true);
  if (!baseline.diagnostic.ok) {
    return refuse(baseline.diagnostic.diagnostic_code,
                  baseline.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          baseline.mga_statement_context,
          entry_common.mga_authority.statement_context)) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-MGA-V1",
                  "grouped spill baseline returned a different MGA statement context");
  }
  const auto& first = request.grouped_request.first_aggregate;
  const auto& common = first.aggregate_request;
  if (!baseline.aggregate_state_spill_required ||
      baseline.aggregate_count == 0 || baseline.groups.empty() ||
      request.spill_root.empty() || !request.spill_root.is_absolute() ||
      !IsCanonicalAggregateStateSpillUuid(request.spill_owner_uuid) ||
      request.runtime_generation == 0 || request.memory_quota_bytes == 0 ||
      request.maximum_serialized_state_bytes == 0 ||
      request.maximum_spill_record_count == 0 ||
      !common.physical_dag.spill_allowed) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-V1",
                  "grouped aggregate state spill ownership, shape, or resource context is invalid");
  }

  std::vector<CanonicalAggregateRuntimeRequest> aggregate_specs;
  aggregate_specs.reserve(baseline.aggregate_count);
  aggregate_specs.push_back(common);
  for (const auto& additional : request.grouped_request.additional_aggregates) {
    auto specification = additional;
    specification.physical_dag = common.physical_dag;
    specification.selected_physical_node_id =
        common.selected_physical_node_id;
    specification.input_batch = common.input_batch;
    aggregate_specs.push_back(std::move(specification));
  }
  if (aggregate_specs.size() != baseline.aggregate_count) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EVIDENCE-V1",
                  "grouped aggregate state inventory diverges from the canonical result");
  }

  auto restored = baseline;
  std::size_t aggregate_transition_count = 0;
  std::size_t distinct_tuple_count = 0;
  std::size_t order_comparison_count = 0;
  std::size_t combined_state_bytes = 0;
  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  const auto key_count = first.group_result_columns.size();
  for (std::size_t group_index = 0;
       group_index < baseline.groups.size(); ++group_index) {
    const auto& group = baseline.groups[group_index];
    if (group.source_row_count != group.source_row_indices.size() ||
        restored.output_batch.rows[group_index].values.size() !=
            key_count + baseline.aggregate_count ||
        group.aggregate_transition_counts.size() != baseline.aggregate_count ||
        group.aggregate_state_bytes.size() != baseline.aggregate_count) {
      return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EVIDENCE-V1",
                    "grouped aggregate source or state evidence is inconsistent");
    }
    for (std::size_t aggregate_index = 0;
         aggregate_index < aggregate_specs.size(); ++aggregate_index) {
      if (result.serialized_aggregate_state_bytes >=
              request.maximum_serialized_state_bytes ||
          result.spilled_aggregate_state_record_count >=
              request.maximum_spill_record_count) {
        return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                      "grouped aggregate state spill bound is exhausted");
      }
      auto aggregate = aggregate_specs[aggregate_index];
      const auto full_filter = aggregate.filter_truth_values;
      aggregate.input_batch.rows.clear();
      aggregate.input_batch.rows.reserve(group.source_row_indices.size());
      for (const auto row : group.source_row_indices) {
        if (row >= common.input_batch.rows.size()) {
          return refuse(
              "QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EVIDENCE-V1",
              "grouped aggregate source-row identity is out of range");
        }
        aggregate.input_batch.rows.push_back(common.input_batch.rows[row]);
      }
      if (full_filter.has_value()) {
        if (full_filter->size() != common.input_batch.rows.size()) {
          return refuse("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                        "grouped aggregate spill FILTER cardinality is not bound");
        }
        std::vector<EngineSqlTruthValue> group_filter;
        group_filter.reserve(group.source_row_indices.size());
        for (const auto row : group.source_row_indices) {
          group_filter.push_back((*full_filter)[row]);
        }
        aggregate.filter_truth_values = std::move(group_filter);
      }
      for (auto& node : aggregate.physical_dag.nodes) {
        if (node.physical_node_id == aggregate.selected_physical_node_id) {
          node.output_descriptor_ids = {
              aggregate.result_column.descriptor_id};
          break;
        }
      }

      CanonicalAggregateStateSpillRequest state_request;
      state_request.aggregate_request = std::move(aggregate);
      state_request.spill_root = request.spill_root;
      state_request.spill_owner_uuid = request.spill_owner_uuid;
      state_request.runtime_generation = request.runtime_generation;
      state_request.reopen_runtime_generation =
          request.reopen_runtime_generation;
      state_request.memory_quota_bytes = request.memory_quota_bytes;
      state_request.maximum_serialized_state_bytes =
          request.maximum_serialized_state_bytes -
          result.serialized_aggregate_state_bytes;
      state_request.maximum_spill_record_count =
          request.maximum_spill_record_count -
          result.spilled_aggregate_state_record_count;
      state_request.cancellation_requested = request.cancellation_requested;
      state_request.cleanup_after_cancellation =
          request.cleanup_after_cancellation;
      state_request.restart_recovery_proof_available =
          request.restart_recovery_proof_available;
      auto state = ExecuteCanonicalAggregateStateSpill(state_request);

      result.spilled = result.spilled || state.spilled;
      result.spill_reopened = result.spill_reopened || state.spill_reopened;
      result.cleanup_proven =
          result.spilled_aggregate_state_count == 0
              ? state.cleanup_proven
              : result.cleanup_proven && state.cleanup_proven;
      result.cancellation_observed =
          result.cancellation_observed || state.cancellation_observed;
      result.spill_evidence.insert(result.spill_evidence.end(),
                                   state.spill_evidence.begin(),
                                   state.spill_evidence.end());
      if (!state.diagnostic.ok) {
        return refuse(state.diagnostic.diagnostic_code,
                      state.diagnostic.detail);
      }
      if (!state.state_serialized || !state.spilled ||
          !state.spill_reopened || !state.state_restored ||
          !state.restored_result_equivalent || !state.cleanup_proven ||
          state.aggregate_result.output_batch.rows.size() != 1 ||
          state.aggregate_result.output_batch.rows.front().values.size() != 1 ||
          state.aggregate_result.selected_plan_uuid !=
              baseline.selected_plan_uuid ||
          state.aggregate_result.executed_physical_node_id !=
              baseline.executed_physical_node_id ||
          state.aggregate_result.causal_counter_id !=
              baseline.causal_counter_id ||
          state.aggregate_result.transition_count !=
              group.aggregate_transition_counts[aggregate_index] ||
          state.aggregate_result.state_bytes !=
              group.aggregate_state_bytes[aggregate_index] ||
          !PhysicalMgaStatementContextEqual(
              state.mga_statement_context,
              common.mga_authority.statement_context) ||
          !PhysicalMgaStatementContextEqual(
              state.aggregate_result.mga_statement_context,
              common.mga_authority.statement_context)) {
        return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EVIDENCE-V1",
                      "restored grouped aggregate state evidence is inconsistent");
      }
      const auto& expected =
          baseline.output_batch.rows[group_index]
              .values[key_count + aggregate_index];
      const auto& actual =
          state.aggregate_result.output_batch.rows.front().values.front();
      if (!SameAggregateValueIdentity(expected, actual) ||
          !within_total(state.serialized_state_bytes,
                        result.serialized_aggregate_state_bytes,
                        request.maximum_serialized_state_bytes) ||
          !within_total(state.spilled_state_record_count,
                        result.spilled_aggregate_state_record_count,
                        request.maximum_spill_record_count) ||
          !within_total(state.aggregate_result.transition_count,
                        aggregate_transition_count,
                        baseline.aggregate_transition_count) ||
          !within_total(state.aggregate_result.distinct_tuple_count,
                        distinct_tuple_count,
                        baseline.aggregate_distinct_tuple_count) ||
          !within_total(state.aggregate_result.order_comparison_count,
                        order_comparison_count,
                        baseline.aggregate_order_comparison_count) ||
          !within_total(state.aggregate_result.state_bytes,
                        combined_state_bytes,
                        baseline.combined_state_bytes)) {
        return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EQUIVALENCE-V1",
                      "restored grouped aggregate value or resource evidence diverges");
      }

      ++result.spilled_aggregate_state_count;
      result.serialized_aggregate_state_bytes += state.serialized_state_bytes;
      result.spilled_aggregate_state_record_count +=
          state.spilled_state_record_count;
      aggregate_transition_count += state.aggregate_result.transition_count;
      distinct_tuple_count += state.aggregate_result.distinct_tuple_count;
      order_comparison_count +=
          state.aggregate_result.order_comparison_count;
      combined_state_bytes += state.aggregate_result.state_bytes;
      restored.output_batch.rows[group_index]
          .values[key_count + aggregate_index] = std::move(
              state.aggregate_result.output_batch.rows.front().values.front());
    }
  }

  if (aggregate_transition_count != baseline.aggregate_transition_count ||
      distinct_tuple_count != baseline.aggregate_distinct_tuple_count ||
      order_comparison_count != baseline.aggregate_order_comparison_count ||
      combined_state_bytes != baseline.combined_state_bytes) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EQUIVALENCE-V1",
                  "restored grouped aggregate totals diverge from the in-memory result");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      common.mga_authority, common.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code, result_authority.detail);
  }
  result.diagnostic = {};
  result.grouped_result = std::move(restored);
  result.mga_statement_context = common.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
