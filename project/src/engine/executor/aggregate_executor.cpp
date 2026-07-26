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
};

bool IsCanonicalCollectionFunction(
    const CanonicalAggregateFunction function) {
  return function == CanonicalAggregateFunction::array_agg ||
         function == CanonicalAggregateFunction::string_agg ||
         function == CanonicalAggregateFunction::json_agg ||
         function == CanonicalAggregateFunction::json_object_agg ||
         function == CanonicalAggregateFunction::listagg;
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
// row is not an implementation claim: executable marks only functions routed
// through the canonical state below; every other accepted row fails closed.
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
      {1, Function::rank, "sb.aggregate.rank", "019dffbb-f000-7336-ab53-fef5316220d7", false, false},
      {1, Function::dense_rank, "sb.aggregate.dense_rank", "019dffbb-f000-7bd3-a731-1734581eb8ce", false, false},
      {1, Function::percent_rank, "sb.aggregate.percent_rank", "019dffbb-f000-7817-911f-9f8b2e66ebec", false, false},
      {1, Function::cume_dist, "sb.aggregate.cume_dist", "019dffbb-f000-7244-89fd-8fa66ae930d5", false, false},
      {1, Function::mode, "sb.aggregate.mode", "019dffbb-f000-7150-9be6-bcf97f8facf5", false, false},
      {1, Function::percentile_cont, "sb.aggregate.percentile_cont", "019dffbb-f000-7cfd-83dd-15435fe55bf5", false, false},
      {1, Function::percentile_disc, "sb.aggregate.percentile_disc", "019dffbb-f000-7081-b766-7db818a89c04", false, false},
      {1, Function::approx_count_distinct, "sb.aggregate.approx_count_distinct", "019dffbb-f000-7736-96f3-e20cbd532ba5", false, false},
      {1, Function::approx_median, "sb.aggregate.approx_median", "019dffbb-f000-7ce0-85a6-cbcd71f2c86e", false, false},
      {1, Function::approx_percentile_cont, "sb.aggregate.approx_percentile_cont", "019dffbb-f000-76df-98a6-aa77d1a342f8", false, false},
      {1, Function::approx_percentile_disc, "sb.aggregate.approx_percentile_disc", "019dffbb-f000-7578-a88f-8db4bb649755", false, false},
      {1, Function::approx_top_k, "sb.aggregate.approx_top_k", "019dffbb-f000-7f47-8fe1-0c5e0ec87bf0", false, false},
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
      function == CanonicalAggregateFunction::listagg;
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

}  // namespace scratchbird::engine::executor
