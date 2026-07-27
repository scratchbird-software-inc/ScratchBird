// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
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
  if (value.descriptor.canonical_type_name == "int64") {
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
      if (IsType(value, "int64")) {
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
                            "SUM and AVG require int64 or real64 input");
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
    if (type == "int64" || type == "real64") return true;
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
    if (type == "int64" || type == "real64") return true;
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

std::string AggregateDistinctKey(const std::vector<EngineTypedValue>& values) {
  std::string key;
  for (const auto& value : values) {
    key += value.descriptor.descriptor_uuid.canonical;
    key.push_back(':');
    key += value.descriptor.canonical_type_name;
    key.push_back(':');
    if (value.state == EngineValueState::sql_null) {
      key += "<NULL>";
    } else {
      key += value.encoded_value;
      key.append(reinterpret_cast<const char*>(value.binary_value.data()),
                 value.binary_value.size());
    }
    key.push_back('\x1f');
  }
  return key;
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
  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
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

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-011-REGISTRY-V1
// Exact ABI-v1 projection of the normative private seed registry.  A registry
// row is not an implementation claim by itself: executable is true only when
// the row is routed through the bounded canonical state below. Descriptor,
// direct-argument, ordering, type, and resource profiles still fail closed.
std::vector<CanonicalAggregateRegistryEntry>
CanonicalAggregateRuntimeRegistryV1() {
  using Function = CanonicalAggregateFunction;
  return {
      {1, Function::count, "sb.aggregate.count", "019de5fc-2400-784a-9aec-371f8b95b7ea", true, false},
      {1, Function::sum, "sb.aggregate.sum", "019de5fc-2400-72e4-8549-82b2eef5a777", true, true},
      {1, Function::avg, "sb.aggregate.avg", "019de5fc-2400-78ac-b50c-45b832831004", true, false},
      {1, Function::min, "sb.aggregate.min", "019de5fc-2400-781c-881b-4af4d55d402b", true, false},
      {1, Function::max, "sb.aggregate.max", "019de5fc-2400-7d1e-8aa4-80bc647fbd9a", true, false},
      {1, Function::bool_and, "sb.aggregate.bool_and", "019de5fc-2400-78b0-ad98-a681e93b4c49", true, false},
      {1, Function::bool_or, "sb.aggregate.bool_or", "019de5fc-2400-7c2a-a3f2-e4b9d36df403", true, false},
      {1, Function::array_agg, "sb.aggregate.array_agg", "019de5fc-2400-7159-9f7b-915513b8c0d4", true, false},
      {1, Function::string_agg, "sb.aggregate.string_agg", "019de5fc-2400-7243-abc6-4f6a777dff00", true, false},
      {1, Function::json_agg, "sb.aggregate.json_agg", "019dffbb-f001-7021-8a00-000000000023", true, false},
      {1, Function::json_object_agg, "sb.aggregate.json_object_agg", "019dffbb-f001-7021-8a00-000000000024", true, false},
      {1, Function::stddev_pop, "sb.aggregate.stddev_pop", "019de5fc-2400-73c9-ba10-4665f741215d", true, false},
      {1, Function::variance_pop, "sb.aggregate.variance_pop", "019de5fc-2400-7fda-b470-e85414dcb314", true, false},
      {1, Function::every, "sb.aggregate.every", "019dffbb-f000-7876-9644-ae83b363d3bc", true, false},
      {1, Function::listagg, "sb.aggregate.listagg", "019dffbb-f000-7e93-8e4d-6063849de049", true, false},
      {1, Function::rank, "sb.aggregate.rank", "019dffbb-f000-7336-ab53-fef5316220d7", true, false},
      {1, Function::dense_rank, "sb.aggregate.dense_rank", "019dffbb-f000-7bd3-a731-1734581eb8ce", true, false},
      {1, Function::percent_rank, "sb.aggregate.percent_rank", "019dffbb-f000-7817-911f-9f8b2e66ebec", true, false},
      {1, Function::cume_dist, "sb.aggregate.cume_dist", "019dffbb-f000-7244-89fd-8fa66ae930d5", true, false},
      {1, Function::mode, "sb.aggregate.mode", "019dffbb-f000-7150-9be6-bcf97f8facf5", true, false},
      {1, Function::percentile_cont, "sb.aggregate.percentile_cont", "019dffbb-f000-7cfd-83dd-15435fe55bf5", true, false},
      {1, Function::percentile_disc, "sb.aggregate.percentile_disc", "019dffbb-f000-7081-b766-7db818a89c04", true, false},
      {1, Function::approx_count_distinct, "sb.aggregate.approx_count_distinct", "019dffbb-f000-7736-96f3-e20cbd532ba5", true, false},
      {1, Function::approx_median, "sb.aggregate.approx_median", "019dffbb-f000-7ce0-85a6-cbcd71f2c86e", true, false},
      {1, Function::approx_percentile_cont, "sb.aggregate.approx_percentile_cont", "019dffbb-f000-76df-98a6-aa77d1a342f8", true, false},
      {1, Function::approx_percentile_disc, "sb.aggregate.approx_percentile_disc", "019dffbb-f000-7578-a88f-8db4bb649755", true, false},
      {1, Function::approx_top_k, "sb.aggregate.approx_top_k", "019dffbb-f000-7f47-8fe1-0c5e0ec87bf0", true, false},
      {1, Function::stddev, "sb.aggregate.stddev", "019dffbb-f000-7475-8516-ff003b2bdad9", true, false},
      {1, Function::variance, "sb.aggregate.variance", "019dffbb-f000-7968-82c5-04cffbeb971b", true, false},
      {1, Function::stddev_samp, "sb.aggregate.stddev_samp", "019dffbb-f000-7d99-a495-70f9c3b1b587", true, false},
      {1, Function::variance_samp, "sb.aggregate.variance_samp", "019dffbb-f000-732b-8a0c-2aa88b04f3c5", true, false},
      {1, Function::corr, "sb.aggregate.corr", "019dffbb-f000-77bb-ba9b-2e78acf84521", true, false},
      {1, Function::covar_pop, "sb.aggregate.covar_pop", "019dffbb-f000-7f09-8ceb-17ad4c70e99f", true, false},
      {1, Function::covar_samp, "sb.aggregate.covar_samp", "019dffbb-f000-747d-bc01-caad9137d070", true, false},
      {1, Function::regr_count, "sb.aggregate.regr_count", "019dffbb-f000-75aa-bbe6-a4a67dacb81f", true, false},
      {1, Function::regr_avgx, "sb.aggregate.regr_avgx", "019dffbb-f000-7662-a816-d1df50e9b664", true, false},
      {1, Function::regr_avgy, "sb.aggregate.regr_avgy", "019dffbb-f000-7d03-ac2d-753cdb7744c0", true, false},
      {1, Function::regr_intercept, "sb.aggregate.regr_intercept", "019dffbb-f000-7c7c-b576-d67ea9d4bcbb", true, false},
      {1, Function::regr_r2, "sb.aggregate.regr_r2", "019dffbb-f000-7a43-9a28-a119b31d9c20", true, false},
      {1, Function::regr_slope, "sb.aggregate.regr_slope", "019dffbb-f000-7f80-b81a-5240a6dbab55", true, false},
      {1, Function::regr_sxx, "sb.aggregate.regr_sxx", "019dffbb-f000-735e-9e55-5f9243786403", true, false},
      {1, Function::regr_sxy, "sb.aggregate.regr_sxy", "019dffbb-f000-788b-a249-866547a43ebe", true, false},
      {1, Function::regr_syy, "sb.aggregate.regr_syy", "019dffbb-f000-74f7-98ba-c24ead6d30df", true, false},
  };
}

CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntime(
    const CanonicalAggregateRuntimeRequest& request) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;

  CanonicalAggregateRuntimeResult result;
  result.descriptor = request.descriptor;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    result.executed_strategy = CanonicalAggregateExecutionStrategy::unknown;
    result.shared_state_authority_used = false;
    return result;
  };

  const auto registry = CanonicalAggregateRuntimeRegistryV1();
  const auto entry = std::find_if(
      registry.begin(), registry.end(), [&](const auto& candidate) {
        return candidate.abi_version == request.descriptor.abi_version &&
               candidate.function == request.descriptor.function &&
               candidate.builtin_id == request.descriptor.builtin_id &&
               candidate.function_uuid == request.descriptor.function_uuid;
      });
  if (request.descriptor.abi_version != 1 || entry == registry.end()) {
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

  const auto dag_validation = ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
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

  struct BoundAggregateTransition {
    std::size_t input_row = 0;
    std::vector<EngineTypedValue> values;
  };
  std::vector<BoundAggregateTransition> transitions;
  transitions.reserve(request.input_batch.rows.size());
  std::set<std::string> distinct_keys;
  result.input_row_count = request.input_batch.rows.size();
  for (std::size_t row = 0; row < request.input_batch.rows.size(); ++row) {
    bool passes = true;
    if (request.filter_truth_values.has_value()) {
      std::string detail;
      if (!QowPredicateConsumerPassesV1(
              (*request.filter_truth_values)[row],
              EnginePredicateConsumer::filter, &passes, &detail)) {
        return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                              std::move(detail)));
      }
    }
    if (!passes) continue;
    ++result.filtered_row_count;
    std::vector<EngineTypedValue> values;
    values.reserve(request.value_columns.size());
    for (const auto column : request.value_columns) {
      values.push_back(request.input_batch.rows[row].values[column]);
    }
    if (request.distinct) {
      if (!distinct_keys.insert(AggregateDistinctKey(values)).second) continue;
      if (distinct_keys.size() > request.maximum_distinct_value_count) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "aggregate DISTINCT bound is exceeded"));
      }
    }
    transitions.push_back({row, std::move(values)});
  }
  result.distinct_tuple_count = request.distinct ? distinct_keys.size() : 0;
  result.filter_applied_before_distinct = true;

  if (!request.aggregate_order_terms.empty()) {
    const auto row_count = transitions.size();
    const auto term_count = request.aggregate_order_terms.size();
    if (request.maximum_order_comparison_count == 0 ||
        (row_count != 0 &&
         row_count > std::numeric_limits<std::size_t>::max() / row_count) ||
        (row_count * row_count != 0 &&
         term_count > std::numeric_limits<std::size_t>::max() /
                          (row_count * row_count)) ||
        row_count * row_count * term_count >
            request.maximum_order_comparison_count) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "aggregate order comparison bound is exceeded"));
    }
    std::vector<std::int8_t> comparisons(row_count * row_count, 0);
    for (std::size_t left = 0; left < row_count; ++left) {
      for (std::size_t right = left + 1; right < row_count; ++right) {
        int comparison = 0;
        for (const auto& term : request.aggregate_order_terms) {
          const auto compared = CompareCanonicalDescriptorOrderValues(
              request.input_batch.rows[transitions[left].input_row]
                  .values[term.column],
              request.input_batch.rows[transitions[right].input_row]
                  .values[term.column],
              term);
          ++result.order_comparison_count;
          if (!compared.diagnostic.ok) {
            return refuse(Refusal("QOW-DIAG-QRY-011-REGISTRY-ORDER-V1",
                                  compared.diagnostic.detail));
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
    std::stable_sort(order.begin(), order.end(),
                     [&](const auto left, const auto right) {
                       return comparisons[left * row_count + right] < 0;
                     });
    std::vector<BoundAggregateTransition> ordered;
    ordered.reserve(row_count);
    for (const auto index : order) {
      ordered.push_back(std::move(transitions[index]));
    }
    transitions = std::move(ordered);
    result.aggregate_order_applied = true;
  }

  DescriptorRuntimeDiagnostic state_diagnostic;
  CanonicalAggregateCoreState state;
  const auto state_within_bound = [&](const CanonicalAggregateCoreState& value) {
    return EstimateCanonicalAggregateStateBytes(value) <=
           request.maximum_state_bytes;
  };
  if (request.forced_strategy == CanonicalAggregateExecutionStrategy::serial) {
    for (const auto& transition : transitions) {
      if (!TransitionCanonicalAggregateCore(request.descriptor,
                                            transition.values, &state,
                                            &state_diagnostic)) {
        return refuse(std::move(state_diagnostic));
      }
      if (!state_within_bound(state)) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "aggregate state byte bound is exceeded"));
      }
    }
  } else {
    CanonicalAggregateCoreState left;
    CanonicalAggregateCoreState right;
    const auto split = transitions.size() / 2;
    for (std::size_t index = 0; index < transitions.size(); ++index) {
      auto* partition = index < split ? &left : &right;
      if (!TransitionCanonicalAggregateCore(request.descriptor,
                                            transitions[index].values,
                                            partition,
                                            &state_diagnostic)) {
        return refuse(std::move(state_diagnostic));
      }
      if (!state_within_bound(*partition)) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "aggregate partial-state byte bound is exceeded"));
      }
    }
    if (!MergeCanonicalAggregateCore(request.descriptor, &state, left,
                                     &state_diagnostic) ||
        !MergeCanonicalAggregateCore(request.descriptor, &state, right,
                                     &state_diagnostic)) {
      return refuse(std::move(state_diagnostic));
    }
    if (!state_within_bound(state)) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "aggregate merged-state byte bound is exceeded"));
    }
  }

  auto value = FinalizeCanonicalAggregateCore(request, state,
                                               &state_diagnostic);
  if (!state_diagnostic.ok) return refuse(std::move(state_diagnostic));
  result.output_batch.columns = {request.result_column};
  result.output_batch.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, aggregate_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));

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
  return result;
}

// QOW-SOURCE-QRY-011-GROUPED-REGISTRY-V1
// Build explicit grouping sets over ordered, descriptor-bound keys and route
// every resulting group through the one canonical aggregate registry/state
// authority above.  Omitted grouping keys are published as typed SQL NULL and
// remain distinguishable from data NULL through GROUPING/GROUPING_ID metadata.
CanonicalGroupedAggregateRuntimeResult ExecuteCanonicalGroupedAggregateRuntime(
    const CanonicalGroupedAggregateRuntimeRequest& request) {
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

  const auto dag_validation = ValidateTypedPhysicalNodeDag(
      aggregate.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
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
      aggregate_node->implementation_id !=
          "aggregate.registry-grouping-sets.v1" ||
      aggregate_node->input_physical_node_ids.size() != 1) {
    return grouped_refusal(
        "selected physical node is not the grouped registry aggregate");
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
    const auto input_nullable = aggregate.input_batch.columns[
        request.group_key_terms[index].column].nullable;
    if ((input_nullable || key_can_be_omitted[index]) &&
        !request.group_result_columns[index].nullable) {
      return grouped_refusal(
          "group result nullability does not cover data or grouping NULL");
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
    std::vector<bool> indicators(included.size(), false);
    std::uint64_t grouping_id = 0;
    bool grand_total = true;
    for (std::size_t index = 0; index < included.size(); ++index) {
      indicators[index] = !included[index];
      if (included[index]) {
        grand_total = false;
      } else {
        grouping_id |= std::uint64_t{1}
                       << (included.size() - 1 - index);
      }
    }
    if (grand_total) {
      WorkingGroup group;
      group.grouping_set_ordinal = static_cast<std::uint32_t>(set_ordinal);
      group.grouping_id = grouping_id;
      group.grouping_indicators = std::move(indicators);
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
      group.grouping_id = grouping_id;
      group.grouping_indicators = indicators;
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
         .source_row_count = group.source_rows.size(),
         .aggregate_transition_count = aggregate_result.transition_count,
         .aggregate_state_bytes = aggregate_result.state_bytes});
  }

  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, aggregate_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));

  result.diagnostic = {};
  result.grouping_set_count = request.grouping_sets.size();
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
  return result;
}

// QOW-SOURCE-QRY-011-GROUPED-SET-V1
// Compose several registry aggregates over one exact grouped physical node.
// Group identity must match across every independently filtered/ordered state;
// aggregate values are then appended in the physical output-descriptor order.
CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntime(
    const CanonicalGroupedAggregateSetRuntimeRequest& request) {
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
  const auto dag_validation = ValidateTypedPhysicalNodeDag(common.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
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
      aggregate_node->implementation_id !=
          "aggregate.registry-grouping-sets.v1" ||
      aggregate_node->output_descriptor_ids.size() !=
          first.group_result_columns.size() + aggregate_count) {
    return set_refusal(
        "selected physical node is not the exact grouped aggregate set");
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
    auto execution =
        ExecuteCanonicalGroupedAggregateRuntime(grouped_request);
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

  result.diagnostic = {};
  result.aggregate_count = aggregate_count;
  result.group_identity_proven = true;
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
  return result;
}

}  // namespace scratchbird::engine::executor
